/*
  HU-058D Custom Firmware v0.05-dev
  ===============================

  Target:
    ESP-01S / ESP8266EX
    1 MiB flash
    HU-058D clock PCB

  v0.05-dev:
    - everything proven in v0.04
    - 48-sample in-RAM NTP quality history ring buffer
    - lightweight browser-rendered drift and correction graphs
    - recent-sample table and history summary statistics
    - /api/ntp-history JSON endpoint
    - manual in-RAM history clear action

  Configuration format remains compatible with v0.02/v0.03/v0.04.

  No external Arduino libraries are required beyond the ESP8266 Arduino core.
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266mDNS.h>
#include <Updater.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <coredecls.h>
#include <lwip/apps/sntp.h>
#include <sys/time.h>
#include <time.h>

// -----------------------------------------------------------------------------
// Firmware identity
// -----------------------------------------------------------------------------

static const char *FW_NAME    = "HU-058D Custom Firmware";
static const char *FW_VERSION = "v0.05-dev-ringtest";
static const char *FW_BUILD   = __DATE__ " " __TIME__;
static const char *MDNS_HOST  = "hu058d-clock";

// -----------------------------------------------------------------------------
// Setup AP
// -----------------------------------------------------------------------------

static const char *SETUP_AP_SSID     = "HU058D-Setup";
static const char *SETUP_AP_PASSWORD = "hu058dclock";

static const IPAddress SETUP_AP_IP(192, 168, 4, 1);
static const IPAddress SETUP_AP_GW(192, 168, 4, 1);
static const IPAddress SETUP_AP_MASK(255, 255, 255, 0);

// -----------------------------------------------------------------------------
// Defaults
// -----------------------------------------------------------------------------

// POSIX timezone rule for New Zealand:
// NZST UTC+12, NZDT UTC+13
// DST begins last Sunday in September at 02:00 standard time
// DST ends first Sunday in April at 03:00 daylight time
static const char *DEFAULT_TZ =
    "NZST-12NZDT,M9.5.0/2,M4.1.0/3";

static const char *DEFAULT_NTP1 = "nz.pool.ntp.org";
static const char *DEFAULT_NTP2 = "pool.ntp.org";
static const char *DEFAULT_NTP3 = "time.cloudflare.com";

// Anything older than 2024-01-01 is treated as unsynchronised.
static const time_t MIN_VALID_EPOCH = 1704067200;

// Diagnostics / logging cadence.
static const uint32_t SERIAL_HEARTBEAT_INTERVAL_MS = 60000UL;
static const uint32_t NTP_REACH_SAMPLE_INTERVAL_MS = 5000UL;

// ESP8266 lwIP is configured for three SNTP servers.
static const uint8_t NTP_SERVER_COUNT = 3;

// Keep two days of hourly time-quality history in RAM. The buffer is deliberately
// not persisted in v0.05 so we can study the data structure and memory behaviour
// before introducing flash wear and persistent-history migration problems.
static const uint8_t NTP_HISTORY_CAPACITY = 48;

// TEMPORARY v0.05 ring-buffer rollover test.
// Synthetic samples are injected locally every 5 seconds. No extra NTP traffic
// is generated. This entire test block is intended to be reverted after use.
static const bool NTP_HISTORY_TEST_MODE = true;
static const uint32_t NTP_HISTORY_TEST_INTERVAL_MS = 5000UL;

// The default SNTP refresh interval is one hour. After 75 minutes without a
// successful update we call the clock "HOLDOVER" rather than pretending the
// network time source is still current.
static const uint32_t NTP_HOLDOVER_AFTER_SECONDS = 4500UL;

// -----------------------------------------------------------------------------
// Persistent configuration
// -----------------------------------------------------------------------------

static const uint32_t CONFIG_MAGIC   = 0x48553032UL; // "HU02"
static const uint16_t CONFIG_VERSION = 2;
static const size_t EEPROM_SIZE      = 512;

struct DeviceConfig {
  uint32_t magic;
  uint16_t version;

  char wifiSsid[33];
  char wifiPassword[65];

  char timezone[80];

  char ntp1[64];
  char ntp2[64];
  char ntp3[64];

  uint32_t checksum;
};

static DeviceConfig config;

// -----------------------------------------------------------------------------
// Runtime state
// -----------------------------------------------------------------------------

static ESP8266WebServer server(80);
static ESP8266HTTPUpdateServer httpUpdater(true);
static DNSServer dnsServer;

static bool setupApActive = false;
static bool mdnsActive = false;

static time_t lastNtpSyncEpoch = 0;
static time_t lastPacketEpoch = 0;

// NTP quality statistics are intentionally runtime-only. They describe the
// current boot and do not needlessly wear flash by being persisted.
static uint32_t ntpSyncCount = 0;
static uint32_t ntpObservedFailureCount = 0;
static uint8_t ntpReachability[NTP_SERVER_COUNT] = {0, 0, 0};
static bool ntpReachKnown[NTP_SERVER_COUNT] = {false, false, false};
static bool ntpPollSeen[NTP_SERVER_COUNT] = {false, false, false};
static int8_t lastNtpServerIndex = -1;

static uint64_t lastNtpSyncMonoUs = 0;
static int64_t lastNtpSyncWallUs = 0;
static uint64_t lastNtpIntervalUs = 0;
static int64_t lastNtpCorrectionUs = 0;
static double lastNtpDriftPpm = 0.0;
static bool ntpQualityValid = false;

struct NtpHistorySample {
  uint32_t epoch;
  float correctionMs;
  float driftPpm;
  uint32_t intervalSeconds;
  int8_t serverIndex;
};

static NtpHistorySample ntpHistory[NTP_HISTORY_CAPACITY];
static uint8_t ntpHistoryHead = 0;
static uint8_t ntpHistoryCount = 0;

static uint32_t restartAtMs = 0;
static uint32_t wifiReconnectAtMs = 0;
static uint32_t lastReconnectAttemptMs = 0;
static uint32_t lastWaitingPacketMs = 0;
static uint32_t lastHeartbeatMs = 0;
static uint32_t lastNtpReachSampleMs = 0;

static uint32_t lastNtpHistoryTestMs = 0;
static uint32_t ntpHistoryTestInjected = 0;

static uint8_t lastOtaLoggedPercent = 0;

// -----------------------------------------------------------------------------
// Small helpers
// -----------------------------------------------------------------------------

static void copyText(char *dst, size_t dstSize, const String &src)
{
  if (dstSize == 0) {
    return;
  }

  size_t n = src.length();
  if (n >= dstSize) {
    n = dstSize - 1;
  }

  memcpy(dst, src.c_str(), n);
  dst[n] = '\0';
}

static void copyText(char *dst, size_t dstSize, const char *src)
{
  copyText(dst, dstSize, String(src ? src : ""));
}

static uint32_t configCrc(const DeviceConfig &cfg)
{
  // Lightweight FNV-1a checksum over the structure, excluding checksum itself.
  const uint8_t *p = reinterpret_cast<const uint8_t *>(&cfg);
  const size_t length = sizeof(DeviceConfig) - sizeof(cfg.checksum);

  uint32_t hash = 2166136261UL;

  for (size_t i = 0; i < length; ++i) {
    hash ^= p[i];
    hash *= 16777619UL;
  }

  return hash;
}

static void setConfigDefaults()
{
  memset(&config, 0, sizeof(config));

  config.magic = CONFIG_MAGIC;
  config.version = CONFIG_VERSION;

  copyText(config.timezone, sizeof(config.timezone), DEFAULT_TZ);
  copyText(config.ntp1, sizeof(config.ntp1), DEFAULT_NTP1);
  copyText(config.ntp2, sizeof(config.ntp2), DEFAULT_NTP2);
  copyText(config.ntp3, sizeof(config.ntp3), DEFAULT_NTP3);

  config.checksum = configCrc(config);
}

static bool loadConfig()
{
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, config);

  if (config.magic != CONFIG_MAGIC ||
      config.version != CONFIG_VERSION ||
      config.checksum != configCrc(config)) {
    setConfigDefaults();
    return false;
  }

  // Defensive terminators in case EEPROM contains malformed data.
  config.wifiSsid[sizeof(config.wifiSsid) - 1] = '\0';
  config.wifiPassword[sizeof(config.wifiPassword) - 1] = '\0';
  config.timezone[sizeof(config.timezone) - 1] = '\0';
  config.ntp1[sizeof(config.ntp1) - 1] = '\0';
  config.ntp2[sizeof(config.ntp2) - 1] = '\0';
  config.ntp3[sizeof(config.ntp3) - 1] = '\0';

  return true;
}

static bool saveConfig()
{
  config.magic = CONFIG_MAGIC;
  config.version = CONFIG_VERSION;
  config.checksum = configCrc(config);

  EEPROM.put(0, config);
  return EEPROM.commit();
}

static String htmlEscape(const String &input)
{
  String out;
  out.reserve(input.length() + 16);

  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];

    switch (c) {
      case '&':  out += F("&amp;");  break;
      case '<':  out += F("&lt;");   break;
      case '>':  out += F("&gt;");   break;
      case '"':  out += F("&quot;"); break;
      case '\'': out += F("&#39;");  break;
      default:   out += c;           break;
    }
  }

  return out;
}

static String jsonEscape(const String &input)
{
  String out;
  out.reserve(input.length() + 16);

  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];

    switch (c) {
      case '"':  out += F("\\\""); break;
      case '\\': out += F("\\\\"); break;
      case '\b': out += F("\\b");  break;
      case '\f': out += F("\\f");  break;
      case '\n': out += F("\\n");  break;
      case '\r': out += F("\\r");  break;
      case '\t': out += F("\\t");  break;
      default:
        if (static_cast<uint8_t>(c) < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(static_cast<uint8_t>(c)));
          out += buf;
        } else {
          out += c;
        }
        break;
    }
  }

  return out;
}

static String formatUptime()
{
  uint32_t seconds = millis() / 1000UL;

  const uint32_t days = seconds / 86400UL;
  seconds %= 86400UL;

  const uint32_t hours = seconds / 3600UL;
  seconds %= 3600UL;

  const uint32_t minutes = seconds / 60UL;
  seconds %= 60UL;

  char buf[64];

  if (days > 0) {
    snprintf(buf, sizeof(buf), "%lu d %02lu:%02lu:%02lu",
             static_cast<unsigned long>(days),
             static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes),
             static_cast<unsigned long>(seconds));
  } else {
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
             static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes),
             static_cast<unsigned long>(seconds));
  }

  return String(buf);
}

static String formatEpochLocal(time_t epoch)
{
  if (epoch < MIN_VALID_EPOCH) {
    return F("Never");
  }

  struct tm localTime;
  localtime_r(&epoch, &localTime);

  char buf[48];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &localTime);

  return String(buf);
}

static String currentLocalTime()
{
  const time_t now = time(nullptr);

  if (now < MIN_VALID_EPOCH) {
    return F("Waiting for NTP");
  }

  return formatEpochLocal(now);
}

static String formatEpochUtc(time_t epoch)
{
  if (epoch < MIN_VALID_EPOCH) {
    return F("Waiting for NTP");
  }

  struct tm utcTime;
  gmtime_r(&epoch, &utcTime);

  char buf[40];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &utcTime);

  return String(buf);
}

static String wifiStatusText(wl_status_t status)
{
  switch (status) {
    case WL_IDLE_STATUS:     return F("Idle");
    case WL_NO_SSID_AVAIL:   return F("SSID not available");
    case WL_SCAN_COMPLETED:  return F("Scan completed");
    case WL_CONNECTED:       return F("Connected");
    case WL_CONNECT_FAILED:  return F("Connection failed");
    case WL_CONNECTION_LOST: return F("Connection lost");
    case WL_DISCONNECTED:    return F("Disconnected");
    default:                 return String(F("Unknown (")) + String(static_cast<int>(status)) + ')';
  }
}

static void logPrefix(const char *tag)
{
  const time_t now = time(nullptr);

  if (now >= MIN_VALID_EPOCH) {
    struct tm localTime;
    localtime_r(&now, &localTime);

    char stamp[12];
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &localTime);

    Serial.printf("[%s] %-5s ", stamp, tag);
  } else {
    Serial.printf("[+%06lus] %-5s ",
                  static_cast<unsigned long>(millis() / 1000UL),
                  tag);
  }
}

static String formatDurationSeconds(uint32_t seconds)
{
  const uint32_t days = seconds / 86400UL;
  seconds %= 86400UL;

  const uint32_t hours = seconds / 3600UL;
  seconds %= 3600UL;

  const uint32_t minutes = seconds / 60UL;
  seconds %= 60UL;

  char buf[64];

  if (days > 0) {
    snprintf(buf, sizeof(buf), "%lu d %02lu:%02lu:%02lu",
             static_cast<unsigned long>(days),
             static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes),
             static_cast<unsigned long>(seconds));
  } else {
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
             static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes),
             static_cast<unsigned long>(seconds));
  }

  return String(buf);
}

static uint32_t ntpSyncAgeSeconds()
{
  if (lastNtpSyncEpoch < MIN_VALID_EPOCH) {
    return UINT32_MAX;
  }

  const time_t now = time(nullptr);

  if (now < lastNtpSyncEpoch) {
    return 0;
  }

  return static_cast<uint32_t>(now - lastNtpSyncEpoch);
}

static String ntpStateText()
{
  if (lastNtpSyncEpoch < MIN_VALID_EPOCH) {
    return F("UNSYNCED");
  }

  const uint32_t age = ntpSyncAgeSeconds();

  if (age == UINT32_MAX) {
    return F("UNSYNCED");
  }

  if (age > NTP_HOLDOVER_AFTER_SECONDS) {
    return F("HOLDOVER");
  }

  return F("SYNCED");
}

static String formatNtpCorrection()
{
  if (!ntpQualityValid) {
    return F("Waiting for second sync");
  }

  const double milliseconds =
      static_cast<double>(lastNtpCorrectionUs) / 1000.0;

  String out;

  if (milliseconds >= 0.0) {
    out += '+';
  }

  out += String(milliseconds, 3);
  out += F(" ms");

  return out;
}

static String formatNtpDrift()
{
  if (!ntpQualityValid) {
    return F("Waiting for second sync");
  }

  String out;

  if (lastNtpDriftPpm >= 0.0) {
    out += '+';
  }

  out += String(lastNtpDriftPpm, 3);
  out += F(" ppm ");
  out += (lastNtpDriftPpm >= 0.0) ? F("fast") : F("slow");

  return out;
}

static String formatNtpInterval()
{
  if (!ntpQualityValid || lastNtpIntervalUs == 0) {
    return F("-");
  }

  return formatDurationSeconds(
      static_cast<uint32_t>(lastNtpIntervalUs / 1000000ULL));
}

static uint8_t countBits8(uint8_t value)
{
  uint8_t count = 0;

  while (value != 0) {
    count += value & 1U;
    value >>= 1;
  }

  return count;
}

static String ntpServerName(uint8_t index)
{
  if (index >= NTP_SERVER_COUNT) {
    return F("-");
  }

  const char *name = sntp_getservername(index);

  if (name != nullptr && name[0] != '\0') {
    return String(name);
  }

  return F("-");
}

static String ntpServerIp(uint8_t index)
{
  if (index >= NTP_SERVER_COUNT) {
    return F("-");
  }

  const ip_addr_t *address = sntp_getserver(index);

  if (address == nullptr) {
    return F("-");
  }

  IPAddress ip = *address;

  if (!ip.isSet()) {
    return F("-");
  }

  return ip.toString();
}

static String ntpReachabilityText(uint8_t index)
{
  if (index >= NTP_SERVER_COUNT || !ntpPollSeen[index]) {
    return F("No observed polls yet");
  }

  char buf[40];

  snprintf(buf, sizeof(buf), "%u/8 recent successes (0%03o)",
           static_cast<unsigned int>(countBits8(ntpReachability[index])),
           static_cast<unsigned int>(ntpReachability[index]));

  return String(buf);
}

static String ntpServerDisplay(uint8_t index)
{
  String out = ntpServerName(index);
  const String ip = ntpServerIp(index);

  if (ip != F("-")) {
    out += F(" (");
    out += ip;
    out += ')';
  }

  out += F(" - ");
  out += ntpReachabilityText(index);

  return out;
}

static String lastNtpServerDisplay()
{
  if (lastNtpServerIndex < 0 ||
      lastNtpServerIndex >= static_cast<int8_t>(NTP_SERVER_COUNT)) {
    return F("Unknown");
  }

  String out = ntpServerName(static_cast<uint8_t>(lastNtpServerIndex));
  const String ip = ntpServerIp(static_cast<uint8_t>(lastNtpServerIndex));

  if (ip != F("-")) {
    out += F(" (");
    out += ip;
    out += ')';
  }

  return out;
}

static uint8_t ntpHistoryPhysicalIndex(uint8_t chronologicalIndex)
{
  if (chronologicalIndex >= ntpHistoryCount) {
    return 0;
  }

  const uint8_t oldest =
      (ntpHistoryCount < NTP_HISTORY_CAPACITY)
          ? 0
          : ntpHistoryHead;

  return static_cast<uint8_t>(
      (oldest + chronologicalIndex) % NTP_HISTORY_CAPACITY);
}

static const NtpHistorySample &ntpHistoryAt(uint8_t chronologicalIndex)
{
  return ntpHistory[ntpHistoryPhysicalIndex(chronologicalIndex)];
}

static void storeNtpHistorySample(
    uint32_t epoch,
    float correctionMs,
    float driftPpm,
    uint32_t intervalSeconds,
    int8_t serverIndex)
{
  NtpHistorySample &sample = ntpHistory[ntpHistoryHead];

  sample.epoch = epoch;
  sample.correctionMs = correctionMs;
  sample.driftPpm = driftPpm;
  sample.intervalSeconds = intervalSeconds;
  sample.serverIndex = serverIndex;

  ntpHistoryHead =
      static_cast<uint8_t>((ntpHistoryHead + 1U) % NTP_HISTORY_CAPACITY);

  if (ntpHistoryCount < NTP_HISTORY_CAPACITY) {
    ++ntpHistoryCount;
  }
}

static void addNtpHistorySample()
{
  if (!ntpQualityValid || lastNtpSyncEpoch < MIN_VALID_EPOCH) {
    return;
  }

  storeNtpHistorySample(
      static_cast<uint32_t>(lastNtpSyncEpoch),
      static_cast<float>(
          static_cast<double>(lastNtpCorrectionUs) / 1000.0),
      static_cast<float>(lastNtpDriftPpm),
      static_cast<uint32_t>(lastNtpIntervalUs / 1000000ULL),
      lastNtpServerIndex);
}

static void injectNtpHistoryTestSample()
{
  if (!NTP_HISTORY_TEST_MODE) {
    return;
  }

  const time_t now = time(nullptr);
  if (now < MIN_VALID_EPOCH) {
    return;
  }

  // A 25-point sawtooth gives two obvious wraps in the 48-sample window.
  const uint8_t phase =
      static_cast<uint8_t>(ntpHistoryTestInjected % 25UL);

  const float driftPpm = -12.0f + static_cast<float>(phase);
  const float correctionMs =
      30.0f - (static_cast<float>(phase) * 2.5f);

  storeNtpHistorySample(
      static_cast<uint32_t>(now),
      correctionMs,
      driftPpm,
      NTP_HISTORY_TEST_INTERVAL_MS / 1000UL,
      -2);

  ++ntpHistoryTestInjected;

  logPrefix("HISTTEST");
  Serial.print(F("injected="));
  Serial.print(ntpHistoryTestInjected);
  Serial.print(F(" stored="));
  Serial.print(ntpHistoryCount);
  Serial.print('/');
  Serial.print(NTP_HISTORY_CAPACITY);
  Serial.print(F(" next_head="));
  Serial.print(ntpHistoryHead);
  Serial.print(F(" correction="));
  if (correctionMs >= 0.0f) {
    Serial.print('+');
  }
  Serial.print(correctionMs, 1);
  Serial.print(F(" ms drift="));
  if (driftPpm >= 0.0f) {
    Serial.print('+');
  }
  Serial.print(driftPpm, 1);
  Serial.println(F(" ppm"));
}

static void clearNtpHistory()
{
  memset(ntpHistory, 0, sizeof(ntpHistory));
  ntpHistoryHead = 0;
  ntpHistoryCount = 0;

  if (NTP_HISTORY_TEST_MODE) {
    ntpHistoryTestInjected = 0;
    lastNtpHistoryTestMs = millis();
  }

  logPrefix("HIST");
  Serial.println(F("In-RAM NTP history cleared."));
}

static void scheduleRestart(uint32_t delayMs = 1500)
{
  restartAtMs = millis() + delayMs;
}

// -----------------------------------------------------------------------------
// HU-058D ESP -> STC protocol
// -----------------------------------------------------------------------------

static uint8_t packetChecksum(const uint8_t *data, size_t length)
{
  uint8_t sum = 0;

  for (size_t i = 0; i < length; ++i) {
    sum = static_cast<uint8_t>(sum + data[i]);
  }

  return sum;
}

static void sendRawPacket(uint8_t packet[9])
{
  packet[8] = packetChecksum(packet, 8);
  Serial1.write(packet, 9);
  Serial1.flush();
}

static void sendControlPacket(uint8_t command)
{
  uint8_t packet[9] = {
    0, 0, 0, 0, 0, 0, 0,
    command,
    0
  };

  sendRawPacket(packet);
}

static void sendTimePacket(const struct tm &localTime)
{
  uint8_t packet[9] = {
    static_cast<uint8_t>(localTime.tm_year),
    static_cast<uint8_t>(localTime.tm_mon),
    static_cast<uint8_t>(localTime.tm_mday),
    static_cast<uint8_t>(localTime.tm_hour),
    static_cast<uint8_t>(localTime.tm_min),
    static_cast<uint8_t>(localTime.tm_sec),
    static_cast<uint8_t>(localTime.tm_wday),
    0x00,
    0x00
  };

  sendRawPacket(packet);
}

// -----------------------------------------------------------------------------
// Time / SNTP
// -----------------------------------------------------------------------------

static int8_t sampleNtpReachability(bool successfulSyncContext)
{
  int8_t successfulServer = -1;

  for (uint8_t i = 0; i < NTP_SERVER_COUNT; ++i) {
    const uint8_t current = sntp_getreachability(i);

    if (ntpReachKnown[i]) {
      const uint8_t previous = ntpReachability[i];

      if (current != previous) {
        // RFC 5905 reachability is an 8-bit shift register. A normal poll
        // update is previous<<1 with the newest result in bit 0.
        const uint8_t shiftedPrevious = static_cast<uint8_t>(previous << 1);
        const bool looksLikePollUpdate =
            (current & 0xFEU) == shiftedPrevious;

        if (looksLikePollUpdate) {
          if ((current & 0x01U) != 0) {
            successfulServer = static_cast<int8_t>(i);
          } else {
            ++ntpObservedFailureCount;

            // Update the cached register before formatting it for the log.
            ntpReachability[i] = current;

            logPrefix("NTP");
            Serial.print(F("Observed failed poll: server="));
            Serial.print(i + 1);
            Serial.print(F(" name="));
            Serial.print(ntpServerName(i));
            Serial.print(F(" reach="));
            Serial.println(ntpReachabilityText(i));
          }
        } else if (successfulSyncContext && (current & 0x01U) != 0) {
          // A configuration restart can reset the register, so don't call
          // that a failure. During a known successful callback, bit 0 still
          // tells us which server most recently answered.
          successfulServer = static_cast<int8_t>(i);
        }
      }
    } else {
      ntpReachKnown[i] = true;

      if (successfulSyncContext && (current & 0x01U) != 0) {
        successfulServer = static_cast<int8_t>(i);
      }
    }

    if (current != 0 || (ntpReachKnown[i] && current != ntpReachability[i])) {
      ntpPollSeen[i] = true;
    }

    ntpReachability[i] = current;
  }

  if (successfulSyncContext && successfulServer < 0) {
    // Fallback for the first callback or any case where the register did not
    // visibly change between our samples.
    for (uint8_t i = 0; i < NTP_SERVER_COUNT; ++i) {
      if ((ntpReachability[i] & 0x01U) != 0) {
        successfulServer = static_cast<int8_t>(i);
        break;
      }
    }
  }

  if (successfulServer >= 0) {
    lastNtpServerIndex = successfulServer;
  }

  return successfulServer;
}

static void resetNtpReachabilityTracking()
{
  for (uint8_t i = 0; i < NTP_SERVER_COUNT; ++i) {
    ntpReachability[i] = 0;
    ntpReachKnown[i] = false;
    ntpPollSeen[i] = false;
  }

  lastNtpServerIndex = -1;
}

static void onTimeSet(bool fromSntp)
{
  if (!fromSntp) {
    return;
  }

  struct timeval tv;
  gettimeofday(&tv, nullptr);

  const uint64_t monoNowUs = micros64();
  const int64_t wallNowUs =
      static_cast<int64_t>(tv.tv_sec) * 1000000LL +
      static_cast<int64_t>(tv.tv_usec);

  bool qualityUpdatedThisSync = false;

  if (lastNtpSyncMonoUs != 0 && lastNtpSyncWallUs != 0) {
    const uint64_t elapsedMonoUs = monoNowUs - lastNtpSyncMonoUs;

    // A tiny interval is not useful for oscillator estimation and can happen
    // after a manual reconfiguration. Wait at least 60 seconds.
    if (elapsedMonoUs >= 60000000ULL) {
      const int64_t predictedWallUs =
          lastNtpSyncWallUs + static_cast<int64_t>(elapsedMonoUs);

      lastNtpCorrectionUs = wallNowUs - predictedWallUs;
      lastNtpIntervalUs = elapsedMonoUs;

      // Positive correction means the free-running clock had fallen behind.
      // Report oscillator drift with the intuitive sign:
      //   positive ppm = oscillator running fast
      //   negative ppm = oscillator running slow
      lastNtpDriftPpm =
          -static_cast<double>(lastNtpCorrectionUs) * 1000000.0 /
          static_cast<double>(elapsedMonoUs);

      ntpQualityValid = true;
      qualityUpdatedThisSync = true;
    }
  }

  lastNtpSyncMonoUs = monoNowUs;
  lastNtpSyncWallUs = wallNowUs;
  lastNtpSyncEpoch = static_cast<time_t>(tv.tv_sec);
  ++ntpSyncCount;

  sampleNtpReachability(true);

  if (qualityUpdatedThisSync) {
    addNtpHistorySample();
  }

  logPrefix("NTP");
  Serial.print(F("Sync #"));
  Serial.print(ntpSyncCount);
  Serial.print(F(" time="));
  Serial.print(formatEpochLocal(lastNtpSyncEpoch));

  if (ntpQualityValid) {
    Serial.print(F(" correction="));
    Serial.print(formatNtpCorrection());
    Serial.print(F(" interval="));
    Serial.print(formatNtpInterval());
    Serial.print(F(" drift="));
    Serial.print(formatNtpDrift());
  } else {
    Serial.print(F(" quality=waiting-for-second-sync"));
  }

  Serial.print(F(" server="));
  Serial.println(lastNtpServerDisplay());
}

static void applyTimeConfiguration()
{
  resetNtpReachabilityTracking();

  logPrefix("NTP");
  Serial.print(F("Timezone rule: "));
  Serial.println(config.timezone);

  logPrefix("NTP");
  Serial.print(F("Servers: "));
  Serial.print(config.ntp1);
  Serial.print(F(", "));
  Serial.print(config.ntp2);
  Serial.print(F(", "));
  Serial.println(config.ntp3);

  configTime(config.timezone, config.ntp1, config.ntp2, config.ntp3);
}

static void requestNtpResync()
{
  logPrefix("NTP");
  Serial.println(F("Manual resynchronisation requested."));

  applyTimeConfiguration();
}

static void logHeartbeat()
{
  const time_t now = time(nullptr);
  const uint32_t age = ntpSyncAgeSeconds();

  logPrefix("STAT");

  Serial.print(F("time="));
  if (now >= MIN_VALID_EPOCH) {
    struct tm localTime;
    localtime_r(&now, &localTime);

    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S%Z", &localTime);
    Serial.print(timeBuf);
  } else {
    Serial.print(F("unsynchronised"));
  }

  Serial.print(F(" wifi="));
  Serial.print(wifiStatusText(WiFi.status()));

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F(" rssi="));
    Serial.print(WiFi.RSSI());
    Serial.print(F("dBm"));
  }

  Serial.print(F(" ntp="));
  Serial.print(ntpStateText());

  Serial.print(F(" ntp_age="));
  if (age == UINT32_MAX) {
    Serial.print('-');
  } else {
    Serial.print(age);
    Serial.print('s');
  }

  Serial.print(F(" syncs="));
  Serial.print(ntpSyncCount);

  Serial.print(F(" failed_polls="));
  Serial.print(ntpObservedFailureCount);

  Serial.print(F(" history="));
  Serial.print(ntpHistoryCount);
  Serial.print('/');
  Serial.print(NTP_HISTORY_CAPACITY);

  if (ntpQualityValid) {
    Serial.print(F(" correction="));
    Serial.print(formatNtpCorrection());

    Serial.print(F(" drift="));
    Serial.print(formatNtpDrift());
  }

  Serial.print(F(" heap="));
  Serial.print(ESP.getFreeHeap());
  Serial.println(F("B"));
}

// -----------------------------------------------------------------------------
// Wi-Fi
// -----------------------------------------------------------------------------

static bool hasSavedWifi()
{
  return config.wifiSsid[0] != '\0';
}

static void startStation()
{
  if (!hasSavedWifi()) {
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  logPrefix("WIFI");
  Serial.print(F("Connecting to SSID="));
  Serial.println(config.wifiSsid);

  WiFi.begin(config.wifiSsid, config.wifiPassword);
}

static void startSetupAp()
{
  if (setupApActive) {
    return;
  }

  logPrefix("AP");
  Serial.println(F("Starting setup access point."));

  if (WiFi.getMode() == WIFI_STA) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_AP);
  }

  WiFi.softAPConfig(SETUP_AP_IP, SETUP_AP_GW, SETUP_AP_MASK);

  if (!WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD)) {
    logPrefix("AP");
    Serial.println(F("ERROR: failed to start setup AP."));
    return;
  }

  dnsServer.start(53, "*", SETUP_AP_IP);
  setupApActive = true;

  logPrefix("AP");
  Serial.print(F("Ready SSID="));
  Serial.print(SETUP_AP_SSID);
  Serial.print(F(" IP="));
  Serial.println(WiFi.softAPIP());
}

static void stopSetupAp()
{
  if (!setupApActive) {
    return;
  }

  dnsServer.stop();
  WiFi.softAPdisconnect(true);

  if (hasSavedWifi()) {
    WiFi.mode(WIFI_STA);
  }

  setupApActive = false;

  logPrefix("AP");
  Serial.println(F("Setup access point stopped."));
}

static bool connectStationWithTimeout(uint32_t timeoutMs)
{
  if (!hasSavedWifi()) {
    return false;
  }

  startStation();

  const uint32_t start = millis();
  uint32_t lastWaitPacket = 0;

  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < timeoutMs) {
    if (millis() - lastWaitPacket >= 1000UL) {
      lastWaitPacket = millis();
      sendControlPacket(0x02);
      Serial.print('.');
    }

    delay(20);
    yield();
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    logPrefix("WIFI");
    Serial.print(F("Connected IP="));
    Serial.print(WiFi.localIP());
    Serial.print(F(" RSSI="));
    Serial.print(WiFi.RSSI());
    Serial.println(F(" dBm"));
    return true;
  }

  logPrefix("WIFI");
  Serial.println(F("Connection attempt timed out."));
  return false;
}

// -----------------------------------------------------------------------------
// mDNS
// -----------------------------------------------------------------------------

static void stopMdns()
{
  if (!mdnsActive) {
    return;
  }

  MDNS.close();
  mdnsActive = false;

  logPrefix("MDNS");
  Serial.println(F("Responder stopped."));
}

static void startMdns()
{
  if (WiFi.status() != WL_CONNECTED || mdnsActive) {
    return;
  }

  if (!MDNS.begin(MDNS_HOST)) {
    logPrefix("MDNS");
    Serial.println(F("WARNING: responder failed to start."));
    return;
  }

  MDNS.addService("http", "tcp", 80);
  mdnsActive = true;

  logPrefix("MDNS");
  Serial.print(F("Ready at http://"));
  Serial.print(MDNS_HOST);
  Serial.println(F(".local/"));
}

// -----------------------------------------------------------------------------
// HTML
// -----------------------------------------------------------------------------

static String pageStart(const String &title)
{
  String html;
  html.reserve(3000);

  html += F(
    "<!doctype html><html lang='en'><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>"
  );

  html += htmlEscape(title);

  html += F(
    "</title>"
    "<style>"
    ":root{color-scheme:light dark;--bg:#101418;--card:#1a2026;--text:#edf2f7;"
    "--muted:#9aa7b3;--accent:#42a5f5;--danger:#ef5350;--ok:#66bb6a;--border:#303943}"
    "@media(prefers-color-scheme:light){:root{--bg:#f3f5f7;--card:#fff;--text:#1e252b;"
    "--muted:#66727d;--border:#d8dee4}}"
    "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);"
    "font:16px system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}"
    ".wrap{max-width:760px;margin:0 auto;padding:18px}"
    "h1{font-size:1.55rem;margin:.2rem 0 1rem}h2{font-size:1.15rem;margin:0 0 .8rem}"
    ".card{background:var(--card);border:1px solid var(--border);border-radius:14px;"
    "padding:16px;margin:0 0 14px;box-shadow:0 4px 16px #0002}"
    ".grid{display:grid;grid-template-columns:minmax(130px,1fr) minmax(160px,2fr);gap:8px 14px}"
    ".k{color:var(--muted)}.v{overflow-wrap:anywhere}"
    "nav{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:14px}"
    "a.btn,button{display:inline-block;border:0;border-radius:10px;padding:10px 14px;"
    "background:var(--accent);color:white;text-decoration:none;font-weight:600;cursor:pointer}"
    "button.secondary,a.secondary{background:#5f6b76}"
    "button.danger{background:var(--danger)}"
    "input,select{width:100%;padding:10px;border-radius:8px;border:1px solid var(--border);"
    "background:var(--bg);color:var(--text);font:inherit}"
    "label{display:block;margin:12px 0 5px;font-weight:600}"
    ".hint{color:var(--muted);font-size:.9rem;margin:.35rem 0}"
    ".ok{color:var(--ok);font-weight:700}.bad{color:var(--danger);font-weight:700}"
    ".actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:14px}"
    "form.inline{display:inline}"
    "code{overflow-wrap:anywhere}"
    ".chart-wrap{position:relative;width:100%;height:240px;margin-top:10px}"
    "canvas.chart{width:100%;height:240px;display:block}"
    ".summary{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:8px;margin:10px 0}"
    ".metric{border:1px solid var(--border);border-radius:10px;padding:10px}"
    ".metric .mv{font-size:1.1rem;font-weight:700;margin-top:3px}"
    "table{width:100%;border-collapse:collapse;font-size:.9rem}"
    "th,td{text-align:left;padding:7px 6px;border-bottom:1px solid var(--border);white-space:nowrap}"
    ".table-scroll{overflow-x:auto}"
    "</style></head><body><div class='wrap'>"
  );

  html += F("<h1>HU-058D Clock <small style='color:var(--muted);font-size:.65em'>");
  html += FW_VERSION;
  html += F("</small></h1><nav>"
            "<a class='btn' href='/'>Status</a>"
            "<a class='btn' href='/wifi'>Wi-Fi</a>"
            "<a class='btn' href='/time'>Time</a>"
            "<a class='btn' href='/history'>History</a>"
            "<a class='btn' href='/system'>System</a>"
            "</nav>");

  return html;
}

static String pageEnd()
{
  return F("</div></body></html>");
}

static void sendHtml(const String &html, int code = 200)
{
  server.sendHeader(F("Cache-Control"), F("no-store, no-cache, must-revalidate"));
  server.send(code, F("text/html; charset=utf-8"), html);
}

static String selectedAttr(bool selected)
{
  return selected ? F(" selected") : String();
}

// -----------------------------------------------------------------------------
// Web handlers
// -----------------------------------------------------------------------------

static void handleRoot()
{
  String html = pageStart(F("HU-058D Status"));

  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const time_t now = time(nullptr);
  const bool timeValid = now >= MIN_VALID_EPOCH;
  const uint32_t syncAge = ntpSyncAgeSeconds();

  html += F("<div class='card'><h2>Clock</h2><div class='grid'>");

  html += F("<div class='k'>Local time</div><div class='v' id='local-time'>");
  html += htmlEscape(currentLocalTime());
  html += F("</div>");

  html += F("<div class='k'>UTC time</div><div class='v' id='utc-time'>");
  html += htmlEscape(formatEpochUtc(now));
  html += F("</div>");

  html += F("<div class='k'>Unix epoch</div><div class='v' id='unix-time'>");
  if (timeValid) {
    html += String(static_cast<uint32_t>(now));
  } else {
    html += F("-");
  }
  html += F("</div>");

  html += F("<div class='k'>Timezone rule</div><div class='v'><code>");
  html += htmlEscape(config.timezone);
  html += F("</code></div>");

  html += F("</div></div>");

  html += F("<div class='card'><h2>NTP quality</h2><div class='grid'>");

  html += F("<div class='k'>State</div><div class='v' id='ntp-state'>");
  html += htmlEscape(ntpStateText());
  html += F("</div>");

  html += F("<div class='k'>Last sync</div><div class='v' id='last-ntp-sync'>");
  html += htmlEscape(formatEpochLocal(lastNtpSyncEpoch));
  html += F("</div>");

  html += F("<div class='k'>Sync age</div><div class='v' id='ntp-age'>");
  if (syncAge == UINT32_MAX) {
    html += F("-");
  } else {
    html += htmlEscape(formatDurationSeconds(syncAge));
  }
  html += F("</div>");

  html += F("<div class='k'>Successful syncs</div><div class='v' id='ntp-sync-count'>");
  html += String(ntpSyncCount);
  html += F("</div>");

  html += F("<div class='k'>Observed failed polls</div><div class='v' id='ntp-failure-count'>");
  html += String(ntpObservedFailureCount);
  html += F("</div>");

  html += F("<div class='k'>Last correction</div><div class='v' id='ntp-correction'>");
  html += htmlEscape(formatNtpCorrection());
  html += F("</div>");

  html += F("<div class='k'>Estimated drift</div><div class='v' id='ntp-drift'>");
  html += htmlEscape(formatNtpDrift());
  html += F("</div>");

  html += F("<div class='k'>Sync interval</div><div class='v' id='ntp-interval'>");
  html += htmlEscape(formatNtpInterval());
  html += F("</div>");

  html += F("<div class='k'>History samples</div><div class='v'><span id='ntp-history-count'>");
  html += String(ntpHistoryCount);
  html += F("</span> / ");
  html += String(NTP_HISTORY_CAPACITY);
  html += F(" <a href='/history' style='color:var(--accent)'>(view graphs)</a></div>");

  html += F("<div class='k'>Last server</div><div class='v' id='ntp-last-server'>");
  html += htmlEscape(lastNtpServerDisplay());
  html += F("</div>");

  for (uint8_t i = 0; i < NTP_SERVER_COUNT; ++i) {
    html += F("<div class='k'>Server ");
    html += String(i + 1);
    html += F("</div><div class='v' id='ntp-server-");
    html += String(i);
    html += F("'>");
    html += htmlEscape(ntpServerDisplay(i));
    html += F("</div>");
  }

  html += F("</div>"
            "<p class='hint'>Correction and drift are estimated from the ESP8266's "
            "monotonic clock between successful SNTP updates. They are useful clock-quality "
            "diagnostics, but they are not raw four-timestamp NTP offset or network-delay measurements. "
            "The failed-poll counter is based on observable changes in lwIP's reachability registers.</p>"
            "</div>");

  html += F("<div class='card'><h2>Network</h2><div class='grid'>");

  html += F("<div class='k'>Wi-Fi</div><div class='v' id='wifi-status'>");
  html += htmlEscape(wifiStatusText(WiFi.status()));
  html += F("</div>");

  html += F("<div class='k'>SSID</div><div class='v'>");
  if (wifiConnected) {
    html += htmlEscape(WiFi.SSID());
  } else {
    html += F("-");
  }
  html += F("</div>");

  html += F("<div class='k'>IP address</div><div class='v'>");
  if (wifiConnected) {
    html += WiFi.localIP().toString();
  } else {
    html += F("-");
  }
  html += F("</div>");

  html += F("<div class='k'>Hostname</div><div class='v'>");
  if (wifiConnected && mdnsActive) {
    html += F("<a href='http://");
    html += MDNS_HOST;
    html += F(".local/' style='color:var(--accent)'>");
    html += MDNS_HOST;
    html += F(".local</a>");
  } else {
    html += F("-");
  }
  html += F("</div>");

  html += F("<div class='k'>Signal</div><div class='v' id='wifi-rssi'>");
  if (wifiConnected) {
    html += String(WiFi.RSSI());
    html += F(" dBm");
  } else {
    html += '-';
  }
  html += F("</div>");

  html += F("<div class='k'>Setup AP</div><div class='v'>");
  if (setupApActive) {
    html += htmlEscape(String(SETUP_AP_SSID));
    html += F(" @ ");
    html += WiFi.softAPIP().toString();
  } else {
    html += F("Off");
  }
  html += F("</div>");

  html += F("</div></div>");

  html += F("<div class='card'><h2>System</h2><div class='grid'>");

  html += F("<div class='k'>Firmware</div><div class='v'>");
  html += FW_VERSION;
  html += F("</div>");

  html += F("<div class='k'>Build</div><div class='v'>");
  html += FW_BUILD;
  html += F("</div>");

  html += F("<div class='k'>Uptime</div><div class='v' id='uptime'>");
  html += htmlEscape(formatUptime());
  html += F("</div>");

  html += F("<div class='k'>Free heap</div><div class='v' id='free-heap'>");
  html += String(ESP.getFreeHeap());
  html += F(" bytes</div>");

  html += F("</div></div>");

  // Keep the status page useful on a desk without requiring manual refreshes.
  // A five-second poll is deliberately modest for this tiny ESP8266.
  html += F(
    "<script>"
    "function huFmtDuration(s){"
      "if(s===null||s===undefined)return '-';"
      "s=Math.max(0,Math.floor(s));"
      "const d=Math.floor(s/86400);s%=86400;"
      "const h=Math.floor(s/3600);s%=3600;"
      "const m=Math.floor(s/60);const x=s%60;"
      "const p=n=>String(n).padStart(2,'0');"
      "return d?d+' d '+p(h)+':'+p(m)+':'+p(x):p(h)+':'+p(m)+':'+p(x);"
    "}"
    "function huSet(id,v){const e=document.getElementById(id);if(e)e.textContent=v;}"
    "async function huRefresh(){"
      "try{"
        "const r=await fetch('/api/status',{cache:'no-store'});"
        "if(!r.ok)return;"
        "const s=await r.json();"
        "huSet('local-time',s.local_time);"
        "huSet('utc-time',s.utc_time);"
        "huSet('unix-time',s.unix_time||'-');"
        "huSet('ntp-state',s.ntp_state);"
        "huSet('last-ntp-sync',s.last_ntp_sync);"
        "huSet('ntp-age',huFmtDuration(s.ntp_age_s));"
        "huSet('ntp-sync-count',s.ntp_sync_count);"
        "huSet('ntp-failure-count',s.ntp_failure_count);"
        "huSet('ntp-correction',s.ntp_correction);"
        "huSet('ntp-drift',s.ntp_drift);"
        "huSet('ntp-interval',s.ntp_interval);"
        "huSet('ntp-last-server',s.ntp_last_server);"
        "huSet('ntp-history-count',s.ntp_history_count);"
        "if(s.ntp_servers){"
          "s.ntp_servers.forEach((v,i)=>huSet('ntp-server-'+i,v.display));"
        "}"
        "huSet('wifi-status',s.wifi_status);"
        "huSet('wifi-rssi',s.rssi?s.rssi+' dBm':'-');"
        "huSet('free-heap',s.free_heap+' bytes');"
        "huSet('uptime',huFmtDuration(Math.floor(s.uptime_ms/1000)));"
      "}catch(e){}"
    "}"
    "setTimeout(huRefresh,1000);"
    "setInterval(huRefresh,5000);"
    "</script>"
  );

  sendHtml(html + pageEnd());
}

static void handleHistoryPage()
{
  String html = pageStart(F("HU-058D Time Quality History"));

  html += F(
    "<div class='card'><h2>Time-quality history</h2>"
    "<p>Each point is one successful SNTP correction separated from the previous "
    "sample by at least 60 seconds. History is stored only in RAM and starts fresh after reboot.</p>"
    "<div class='summary'>"
      "<div class='metric'><div class='k'>Samples</div><div class='mv' id='hist-count'>-</div></div>"
      "<div class='metric'><div class='k'>Mean drift</div><div class='mv' id='hist-mean'>-</div></div>"
      "<div class='metric'><div class='k'>Drift range</div><div class='mv' id='hist-range'>-</div></div>"
      "<div class='metric'><div class='k'>Latest correction</div><div class='mv' id='hist-latest'>-</div></div>"
    "</div>"
    "</div>"

    "<div class='card'><h2>Estimated oscillator drift</h2>"
    "<div class='chart-wrap'><canvas class='chart' id='drift-chart'></canvas></div>"
    "<p class='hint'>Positive values mean the ESP8266 free-running clock was fast; negative values mean slow.</p>"
    "</div>"

    "<div class='card'><h2>NTP clock correction</h2>"
    "<div class='chart-wrap'><canvas class='chart' id='correction-chart'></canvas></div>"
    "<p class='hint'>Positive values mean SNTP moved the system clock forward; negative values moved it backward.</p>"
    "</div>"

    "<div class='card'><h2>Recent samples</h2>"
    "<div class='table-scroll'><table>"
    "<thead><tr><th>Time</th><th>Correction</th><th>Drift</th><th>Interval</th><th>Server</th></tr></thead>"
    "<tbody id='history-rows'><tr><td colspan='5'>Loading...</td></tr></tbody>"
    "</table></div>"
    "<form method='post' action='/history/clear' "
    "onsubmit=\"return confirm('Clear the in-RAM NTP history?')\">"
    "<div class='actions'><button class='secondary' type='submit'>Clear RAM history</button></div>"
    "</form></div>"

    "<script>"
    "function fmtSigned(v,n,u){if(v===null||v===undefined)return '-';return (v>=0?'+':'')+Number(v).toFixed(n)+' '+u;}"
    "function fmtDur(s){s=Math.max(0,Math.round(s||0));const h=Math.floor(s/3600);s%=3600;const m=Math.floor(s/60);const x=s%60;const p=n=>String(n).padStart(2,'0');return p(h)+':'+p(m)+':'+p(x);}"
    "function drawChart(id,data,key,unit){"
      "const c=document.getElementById(id),box=c.parentElement;"
      "const dpr=window.devicePixelRatio||1,w=Math.max(280,box.clientWidth),h=240;"
      "c.width=Math.floor(w*dpr);c.height=Math.floor(h*dpr);"
      "const x=c.getContext('2d');x.setTransform(dpr,0,0,dpr,0,0);"
      "const css=getComputedStyle(document.documentElement);"
      "const text=css.getPropertyValue('--muted').trim()||'#999';"
      "const border=css.getPropertyValue('--border').trim()||'#555';"
      "const accent=css.getPropertyValue('--accent').trim()||'#42a5f5';"
      "x.clearRect(0,0,w,h);x.font='12px system-ui';x.fillStyle=text;x.strokeStyle=border;x.lineWidth=1;"
      "if(!data.length){x.fillText('Waiting for NTP history samples...',12,24);return;}"
      "let vals=data.map(s=>Number(s[key])).filter(Number.isFinite);"
      "if(!vals.length){x.fillText('No valid samples yet',12,24);return;}"
      "let lo=Math.min(...vals,0),hi=Math.max(...vals,0);"
      "if(lo===hi){lo-=1;hi+=1;}const pad=(hi-lo)*0.12||1;lo-=pad;hi+=pad;"
      "const L=58,R=12,T=14,B=34,pw=w-L-R,ph=h-T-B;"
      "x.textAlign='right';x.textBaseline='middle';"
      "for(let i=0;i<=4;i++){const y=T+ph*i/4,val=hi-(hi-lo)*i/4;"
        "x.strokeStyle=border;x.beginPath();x.moveTo(L,y);x.lineTo(w-R,y);x.stroke();"
        "x.fillStyle=text;x.fillText(val.toFixed(Math.abs(val)<10?2:1),L-7,y);"
      "}"
      "const zeroY=T+(hi/(hi-lo))*ph;if(zeroY>=T&&zeroY<=T+ph){x.strokeStyle=text;x.beginPath();x.moveTo(L,zeroY);x.lineTo(w-R,zeroY);x.stroke();}"
      "x.strokeStyle=accent;x.lineWidth=2;x.beginPath();"
      "data.forEach((s,i)=>{const px=L+(data.length===1?pw/2:pw*i/(data.length-1));const val=Number(s[key]);const py=T+(hi-val)/(hi-lo)*ph;if(i===0)x.moveTo(px,py);else x.lineTo(px,py);});x.stroke();"
      "x.fillStyle=accent;data.forEach((s,i)=>{const px=L+(data.length===1?pw/2:pw*i/(data.length-1));const val=Number(s[key]);const py=T+(hi-val)/(hi-lo)*ph;x.beginPath();x.arc(px,py,2.5,0,Math.PI*2);x.fill();});"
      "x.fillStyle=text;x.textAlign='left';x.textBaseline='top';"
      "const first=new Date(data[0].epoch*1000),last=new Date(data[data.length-1].epoch*1000);"
      "x.fillText(first.toLocaleString(),L,h-B+9);"
      "x.textAlign='right';x.fillText(last.toLocaleString(),w-R,h-B+9);"
      "x.save();x.translate(14,T+ph/2);x.rotate(-Math.PI/2);x.textAlign='center';x.fillText(unit,0,0);x.restore();"
    "}"
    "async function loadHistory(){"
      "try{const r=await fetch('/api/ntp-history',{cache:'no-store'});const j=await r.json();const d=j.samples||[];"
        "document.getElementById('hist-count').textContent=d.length+' / '+j.capacity;"
        "if(d.length){"
          "const dr=d.map(s=>Number(s.drift_ppm));const mean=dr.reduce((a,b)=>a+b,0)/dr.length;"
          "document.getElementById('hist-mean').textContent=fmtSigned(mean,3,'ppm');"
          "document.getElementById('hist-range').textContent=Math.min(...dr).toFixed(3)+' to '+Math.max(...dr).toFixed(3)+' ppm';"
          "document.getElementById('hist-latest').textContent=fmtSigned(d[d.length-1].correction_ms,3,'ms');"
        "}else{document.getElementById('hist-mean').textContent='-';document.getElementById('hist-range').textContent='-';document.getElementById('hist-latest').textContent='-';}"
        "drawChart('drift-chart',d,'drift_ppm','ppm');drawChart('correction-chart',d,'correction_ms','ms');"
        "const rows=document.getElementById('history-rows');rows.innerHTML='';"
        "if(!d.length){rows.innerHTML=\"<tr><td colspan='5'>Waiting for the second suitable NTP sync.</td></tr>\";return;}"
        "d.slice().reverse().slice(0,12).forEach(s=>{const tr=document.createElement('tr');"
          "const vals=[new Date(s.epoch*1000).toLocaleString(),fmtSigned(s.correction_ms,3,'ms'),fmtSigned(s.drift_ppm,3,'ppm'),fmtDur(s.interval_s),s.server||'Unknown'];"
          "vals.forEach(v=>{const td=document.createElement('td');td.textContent=v;tr.appendChild(td);});rows.appendChild(tr);"
        "});"
      "}catch(e){console.log('history refresh failed',e);}"
    "}"
    "window.addEventListener('resize',()=>loadHistory());loadHistory();setInterval(loadHistory,5000);"
    "</script>"
  );

  sendHtml(html + pageEnd());
}

static void handleHistoryClear()
{
  clearNtpHistory();

  server.sendHeader(F("Location"), F("/history"), true);
  server.send(303, F("text/plain"), F("History cleared"));
}

static void handleWifiPage()
{
  String html = pageStart(F("HU-058D Wi-Fi"));

  html += F("<div class='card'><h2>Wi-Fi configuration</h2>");

  if (WiFi.status() == WL_CONNECTED) {
    html += F("<p>Currently connected to <strong>");
    html += htmlEscape(WiFi.SSID());
    html += F("</strong> at <strong>");
    html += WiFi.localIP().toString();
    html += F("</strong>.</p>");
  }

  html += F(
    "<form method='post' action='/wifi/save'>"
    "<label for='ssid'>SSID</label>"
    "<input id='ssid' name='ssid' list='networks' maxlength='32' required value='"
  );

  html += htmlEscape(String(config.wifiSsid));
  html += F("'><datalist id='networks'>");

  int count = WiFi.scanNetworks(false, true);

  if (count > 0) {
    for (int i = 0; i < count; ++i) {
      html += F("<option value='");
      html += htmlEscape(WiFi.SSID(i));
      html += F("'>");
    }
  }

  html += F(
    "</datalist>"
    "<p class='hint'>The list above comes from a fresh Wi-Fi scan. You can also type an SSID manually.</p>"
    "<label for='password'>Password</label>"
    "<input id='password' name='password' maxlength='64' type='password' autocomplete='new-password'>"
    "<p class='hint'>Leave blank to keep the existing saved password when the SSID has not changed."
    " For a new SSID, enter its password.</p>"
    "<div class='actions'><button type='submit'>Save Wi-Fi &amp; reboot</button></div>"
    "</form></div>"
  );

  WiFi.scanDelete();

  sendHtml(html + pageEnd());
}

static void handleWifiSave()
{
  String ssid = server.arg(F("ssid"));
  String password = server.arg(F("password"));

  ssid.trim();

  if (ssid.length() == 0 || ssid.length() > 32 || password.length() > 64) {
    sendHtml(pageStart(F("Wi-Fi Error")) +
             F("<div class='card'><h2>Invalid Wi-Fi settings</h2>"
               "<p>SSID must be 1-32 characters and password no more than 64 characters.</p>"
               "<a class='btn' href='/wifi'>Back</a></div>") +
             pageEnd(), 400);
    return;
  }

  const bool sameSsid = ssid == String(config.wifiSsid);

  copyText(config.wifiSsid, sizeof(config.wifiSsid), ssid);

  if (!sameSsid || password.length() > 0) {
    copyText(config.wifiPassword, sizeof(config.wifiPassword), password);
  }

  if (!saveConfig()) {
    sendHtml(pageStart(F("Save Error")) +
             F("<div class='card'><h2>Save failed</h2>"
               "<p>Could not commit the configuration to flash.</p></div>") +
             pageEnd(), 500);
    return;
  }

  String html = pageStart(F("Wi-Fi Saved"));
  html += F("<div class='card'><h2>Wi-Fi saved</h2>"
            "<p>The clock will reboot and attempt to join <strong>");
  html += htmlEscape(ssid);
  html += F("</strong>.</p>"
            "<p>If it cannot connect, the <strong>HU058D-Setup</strong> access point will return.</p>"
            "</div>");

  sendHtml(html + pageEnd());
  scheduleRestart();
}

static void handleTimePage()
{
  String html = pageStart(F("HU-058D Time"));

  const String tz = String(config.timezone);
  const bool isNz = tz == DEFAULT_TZ;
  const bool isUtc = tz == "UTC0";
  const bool isCustom = !isNz && !isUtc;

  html += F("<div class='card'><h2>Time configuration</h2>"
            "<form method='post' action='/time/save'>"
            "<label for='tzpreset'>Timezone</label>"
            "<select id='tzpreset' name='tzpreset'>"
            "<option value='NZ'");

  html += selectedAttr(isNz);

  html += F(">New Zealand - automatic NZST/NZDT</option>"
            "<option value='UTC'");

  html += selectedAttr(isUtc);

  html += F(">UTC</option>"
            "<option value='CUSTOM'");

  html += selectedAttr(isCustom);

  html += F(">Custom POSIX TZ rule</option>"
            "</select>"
            "<label for='timezone'>Custom / current POSIX TZ rule</label>"
            "<input id='timezone' name='timezone' maxlength='79' value='");

  html += htmlEscape(tz);

  html += F("'><p class='hint'>For New Zealand the default rule is "
            "<code>NZST-12NZDT,M9.5.0/2,M4.1.0/3</code>.</p>"
            "<label for='ntp1'>NTP server 1</label>"
            "<input id='ntp1' name='ntp1' maxlength='63' value='");

  html += htmlEscape(String(config.ntp1));

  html += F("'><label for='ntp2'>NTP server 2</label>"
            "<input id='ntp2' name='ntp2' maxlength='63' value='");

  html += htmlEscape(String(config.ntp2));

  html += F("'><label for='ntp3'>NTP server 3</label>"
            "<input id='ntp3' name='ntp3' maxlength='63' value='");

  html += htmlEscape(String(config.ntp3));

  html += F("'><div class='actions'><button type='submit'>Save time settings</button></div>"
            "</form>"
            "<form method='post' action='/time/sync' style='margin-top:12px'>"
            "<button class='secondary' type='submit'>Sync now</button>"
            "</form></div>");

  sendHtml(html + pageEnd());
}

static void handleTimeSave()
{
  String preset = server.arg(F("tzpreset"));
  String timezone = server.arg(F("timezone"));
  String ntp1 = server.arg(F("ntp1"));
  String ntp2 = server.arg(F("ntp2"));
  String ntp3 = server.arg(F("ntp3"));

  timezone.trim();
  ntp1.trim();
  ntp2.trim();
  ntp3.trim();

  if (preset == F("NZ")) {
    timezone = DEFAULT_TZ;
  } else if (preset == F("UTC")) {
    timezone = F("UTC0");
  } else if (preset == F("CUSTOM")) {
    if (timezone.length() == 0) {
      sendHtml(pageStart(F("Time Error")) +
               F("<div class='card'><h2>Timezone required</h2>"
                 "<p>A custom timezone rule cannot be empty.</p>"
                 "<a class='btn' href='/time'>Back</a></div>") +
               pageEnd(), 400);
      return;
    }
  } else {
    sendHtml(pageStart(F("Time Error")) +
             F("<div class='card'><h2>Invalid timezone selection</h2></div>") +
             pageEnd(), 400);
    return;
  }

  if (timezone.length() > 79 ||
      ntp1.length() == 0 || ntp1.length() > 63 ||
      ntp2.length() > 63 ||
      ntp3.length() > 63) {
    sendHtml(pageStart(F("Time Error")) +
             F("<div class='card'><h2>Invalid time settings</h2>"
               "<p>Check timezone and NTP server lengths. NTP server 1 is required.</p>"
               "<a class='btn' href='/time'>Back</a></div>") +
             pageEnd(), 400);
    return;
  }

  copyText(config.timezone, sizeof(config.timezone), timezone);
  copyText(config.ntp1, sizeof(config.ntp1), ntp1);
  copyText(config.ntp2, sizeof(config.ntp2), ntp2);
  copyText(config.ntp3, sizeof(config.ntp3), ntp3);

  if (!saveConfig()) {
    sendHtml(pageStart(F("Save Error")) +
             F("<div class='card'><h2>Save failed</h2>"
               "<p>Could not commit the configuration to flash.</p></div>") +
             pageEnd(), 500);
    return;
  }

  applyTimeConfiguration();

  String html = pageStart(F("Time Saved"));
  html += F("<div class='card'><h2>Time settings saved</h2>"
            "<p>The SNTP client has been restarted with the new settings.</p>"
            "<a class='btn' href='/'>Status</a></div>");

  sendHtml(html + pageEnd());
}

static void handleSyncNow()
{
  requestNtpResync();

  String html = pageStart(F("NTP Sync"));
  html += F("<div class='card'><h2>NTP sync requested</h2>"
            "<p>The clock has restarted its SNTP client. The status page will show the next successful sync.</p>"
            "<a class='btn' href='/'>Status</a></div>");

  sendHtml(html + pageEnd());
}

static void handleSystemPage()
{
  String html = pageStart(F("HU-058D System"));

  html += F("<div class='card'><h2>Device information</h2><div class='grid'>");

  html += F("<div class='k'>Firmware</div><div class='v'>");
  html += FW_VERSION;
  html += F("</div>");

  html += F("<div class='k'>Build</div><div class='v'>");
  html += FW_BUILD;
  html += F("</div>");

  html += F("<div class='k'>ESP8266 core</div><div class='v'>");
  html += htmlEscape(ESP.getCoreVersion());
  html += F("</div>");

  html += F("<div class='k'>SDK</div><div class='v'>");
  html += htmlEscape(String(ESP.getSdkVersion()));
  html += F("</div>");

  html += F("<div class='k'>Chip ID</div><div class='v'>0x");
  html += String(ESP.getChipId(), HEX);
  html += F("</div>");

  html += F("<div class='k'>Real flash size</div><div class='v'>");
  html += String(ESP.getFlashChipRealSize());
  html += F(" bytes</div>");

  html += F("<div class='k'>Configured flash size</div><div class='v'>");
  html += String(ESP.getFlashChipSize());
  html += F(" bytes</div>");

  html += F("<div class='k'>Sketch size</div><div class='v'>");
  html += String(ESP.getSketchSize());
  html += F(" bytes</div>");

  html += F("<div class='k'>OTA free sketch space</div><div class='v'>");
  html += String(ESP.getFreeSketchSpace());
  html += F(" bytes</div>");

  html += F("<div class='k'>Free heap</div><div class='v'>");
  html += String(ESP.getFreeHeap());
  html += F(" bytes</div>");

  html += F("<div class='k'>Reset reason</div><div class='v'>");
  html += htmlEscape(ESP.getResetReason());
  html += F("</div>");

  html += F("<div class='k'>Uptime</div><div class='v'>");
  html += htmlEscape(formatUptime());
  html += F("</div>");

  html += F("<div class='k'>Serial logging</div><div class='v'>Event log + ");
  html += String(SERIAL_HEARTBEAT_INTERVAL_MS / 1000UL);
  html += F(" s heartbeat @ 115200 baud</div>");

  html += F("</div></div>");

  html += F(
    "<div class='card'><h2>Firmware update</h2>"
    "<p>Upload a compiled ESP8266 <code>.bin</code> file over Wi-Fi. "
    "The clock will reboot automatically after a successful update.</p>"
    "<p class='hint'>Use the 1MB OTA-capable flash layout when compiling. "
    "Do not remove power while the upload is in progress.</p>"
    "<a class='btn' href='/firmware'>Open firmware updater</a>"
    "</div>"
  );

  html += F(
    "<div class='card'><h2>Actions</h2>"
    "<div class='actions'>"
    "<form class='inline' method='post' action='/system/reboot'>"
    "<button type='submit'>Reboot</button></form>"
    "<form class='inline' method='post' action='/system/reconnect-wifi'>"
    "<button class='secondary' type='submit'>Reconnect Wi-Fi</button></form>"
    "<form class='inline' method='post' action='/system/reset-wifi' "
    "onsubmit=\"return confirm('Clear saved Wi-Fi and reboot into setup mode?')\">"
    "<button class='secondary' type='submit'>Reset Wi-Fi</button></form>"
    "<form class='inline' method='post' action='/system/factory-reset' "
    "onsubmit=\"return confirm('Erase all HU-058D settings and reboot?')\">"
    "<button class='danger' type='submit'>Factory reset</button></form>"
    "</div></div>"
  );

  sendHtml(html + pageEnd());
}

static void handleFirmwarePage()
{
  String html = pageStart(F("HU-058D Firmware Update"));

  html += F(
    "<div class='card'><h2>Firmware update</h2>"
    "<p>Choose a compiled <code>.bin</code> for this HU-058D firmware and upload it.</p>"
    "<form method='post' action='/update' enctype='multipart/form-data'>"
    "<label for='update'>Firmware binary</label>"
    "<input id='update' name='update' type='file' accept='.bin,application/octet-stream' required>"
    "<div class='actions'>"
    "<button type='submit' onclick=\"return confirm('Install this firmware and reboot the clock?')\">"
    "Upload &amp; install</button>"
    "</div></form>"
    "<p class='hint'>Current version: "
  );

  html += FW_VERSION;

  html += F("<br>Current sketch size: ");
  html += String(ESP.getSketchSize());
  html += F(" bytes<br>Available OTA sketch space: ");
  html += String(ESP.getFreeSketchSpace());
  html += F(" bytes</p></div>");

  html += F(
    "<div class='card'><h2>Building an OTA image</h2>"
    "<p>In Arduino IDE use <strong>Sketch &rarr; Export Compiled Binary</strong>, "
    "then upload the generated <code>.ino.bin</code> file here.</p>"
    "<p class='hint'>Recommended Flash Size: "
    "<code>1MB (FS:none OTA:~502KB)</code>. This firmware does not use a filesystem.</p>"
    "</div>"
  );

  sendHtml(html + pageEnd());
}

static void handleReboot()
{
  sendHtml(pageStart(F("Reboot")) +
           F("<div class='card'><h2>Rebooting</h2><p>The clock will restart in a moment.</p></div>") +
           pageEnd());

  scheduleRestart();
}

static void handleReconnectWifi()
{
  if (!hasSavedWifi()) {
    sendHtml(pageStart(F("Reconnect Wi-Fi")) +
             F("<div class='card'><h2>No saved Wi-Fi</h2>"
               "<p>There is no saved network to reconnect to.</p>"
               "<a class='btn' href='/wifi'>Configure Wi-Fi</a></div>") +
             pageEnd(), 400);
    return;
  }

  sendHtml(pageStart(F("Reconnect Wi-Fi")) +
           F("<div class='card'><h2>Reconnecting Wi-Fi</h2>"
             "<p>The network connection will briefly disappear while the clock reconnects.</p></div>") +
           pageEnd());

  wifiReconnectAtMs = millis() + 1200UL;
}

static void handleResetWifi()
{
  config.wifiSsid[0] = '\0';
  config.wifiPassword[0] = '\0';
  saveConfig();

  sendHtml(pageStart(F("Reset Wi-Fi")) +
           F("<div class='card'><h2>Wi-Fi cleared</h2>"
             "<p>The clock will reboot into the HU058D-Setup access point.</p></div>") +
           pageEnd());

  scheduleRestart();
}

static void handleFactoryReset()
{
  setConfigDefaults();
  saveConfig();

  sendHtml(pageStart(F("Factory Reset")) +
           F("<div class='card'><h2>Settings cleared</h2>"
             "<p>The clock will reboot with HU-058D firmware defaults.</p></div>") +
           pageEnd());

  scheduleRestart();
}

static void handleApiStatus()
{
  const time_t now = time(nullptr);
  const uint32_t syncAge = ntpSyncAgeSeconds();

  String json;
  json.reserve(2200);

  json += F("{\"firmware\":\"");
  json += FW_VERSION;
  json += F("\",\"build\":\"");
  json += jsonEscape(FW_BUILD);
  json += F("\",\"hostname\":\"");
  json += MDNS_HOST;
  json += F(".local\",\"wifi_status\":\"");
  json += jsonEscape(wifiStatusText(WiFi.status()));
  json += F("\",\"ssid\":\"");
  json += jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String());
  json += F("\",\"ip\":\"");
  json += WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String();
  json += F("\",\"rssi\":");
  json += WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : String(0);

  json += F(",\"local_time\":\"");
  json += jsonEscape(currentLocalTime());
  json += F("\",\"utc_time\":\"");
  json += jsonEscape(formatEpochUtc(now));
  json += F("\",\"unix_time\":");
  json += now >= MIN_VALID_EPOCH ? String(static_cast<uint32_t>(now)) : String(0);

  json += F(",\"ntp_state\":\"");
  json += jsonEscape(ntpStateText());
  json += F("\",\"last_ntp_sync\":\"");
  json += jsonEscape(formatEpochLocal(lastNtpSyncEpoch));
  json += F("\",\"ntp_age_s\":");
  if (syncAge == UINT32_MAX) {
    json += F("null");
  } else {
    json += String(syncAge);
  }

  json += F(",\"ntp_sync_count\":");
  json += String(ntpSyncCount);
  json += F(",\"ntp_failure_count\":");
  json += String(ntpObservedFailureCount);

  json += F(",\"ntp_correction\":\"");
  json += jsonEscape(formatNtpCorrection());
  json += F("\",\"ntp_drift\":\"");
  json += jsonEscape(formatNtpDrift());
  json += F("\",\"ntp_interval\":\"");
  json += jsonEscape(formatNtpInterval());
  json += F("\",\"ntp_last_server\":\"");
  json += jsonEscape(lastNtpServerDisplay());
  json += F("\"");

  json += F(",\"ntp_correction_ms\":");
  if (ntpQualityValid) {
    json += String(static_cast<double>(lastNtpCorrectionUs) / 1000.0, 3);
  } else {
    json += F("null");
  }

  json += F(",\"ntp_drift_ppm\":");
  if (ntpQualityValid) {
    json += String(lastNtpDriftPpm, 3);
  } else {
    json += F("null");
  }

  json += F(",\"ntp_interval_s\":");
  if (ntpQualityValid) {
    json += String(static_cast<double>(lastNtpIntervalUs) / 1000000.0, 3);
  } else {
    json += F("null");
  }

  json += F(",\"ntp_servers\":[");

  for (uint8_t i = 0; i < NTP_SERVER_COUNT; ++i) {
    if (i != 0) {
      json += ',';
    }

    json += F("{\"index\":");
    json += String(i);
    json += F(",\"name\":\"");
    json += jsonEscape(ntpServerName(i));
    json += F("\",\"ip\":\"");
    json += jsonEscape(ntpServerIp(i));
    json += F("\",\"reach\":");
    json += String(ntpReachability[i]);
    json += F(",\"reach_successes\":");
    json += String(countBits8(ntpReachability[i]));
    json += F(",\"display\":\"");
    json += jsonEscape(ntpServerDisplay(i));
    json += F("\"}");
  }

  json += F("]");

  json += F(",\"ntp_history_count\":");
  json += String(ntpHistoryCount);
  json += F(",\"ntp_history_capacity\":");
  json += String(NTP_HISTORY_CAPACITY);

  json += F(",\"setup_ap\":");
  json += setupApActive ? F("true") : F("false");
  json += F(",\"mdns\":");
  json += mdnsActive ? F("true") : F("false");
  json += F(",\"free_heap\":");
  json += String(ESP.getFreeHeap());
  json += F(",\"sketch_size\":");
  json += String(ESP.getSketchSize());
  json += F(",\"free_sketch_space\":");
  json += String(ESP.getFreeSketchSpace());
  json += F(",\"uptime_ms\":");
  json += String(millis());
  json += '}';

  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), json);
}

static void handleApiNtpHistory()
{
  String json;
  json.reserve(7600);

  json += F("{\"capacity\":");
  json += String(NTP_HISTORY_CAPACITY);
  json += F(",\"count\":");
  json += String(ntpHistoryCount);
  json += F(",\"samples\":[");

  for (uint8_t i = 0; i < ntpHistoryCount; ++i) {
    const NtpHistorySample &sample = ntpHistoryAt(i);

    if (i != 0) {
      json += ',';
    }

    json += F("{\"epoch\":");
    json += String(sample.epoch);
    json += F(",\"correction_ms\":");
    json += String(sample.correctionMs, 3);
    json += F(",\"drift_ppm\":");
    json += String(sample.driftPpm, 3);
    json += F(",\"interval_s\":");
    json += String(sample.intervalSeconds);
    json += F(",\"server_index\":");
    json += String(static_cast<int>(sample.serverIndex));
    json += F(",\"server\":\"");

    if (sample.serverIndex >= 0 &&
        sample.serverIndex < static_cast<int8_t>(NTP_SERVER_COUNT)) {
      json += jsonEscape(ntpServerName(static_cast<uint8_t>(sample.serverIndex)));
    } else if (sample.serverIndex == -2) {
      json += F("Synthetic test");
    } else {
      json += F("Unknown");
    }

    json += F("\"}");
  }

  json += F("]}");

  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), json);
}

static void handleNotFound()
{
  // When connected to the setup AP, make random captive-portal URLs land on
  // the clock's UI instead of a dead page.
  if (setupApActive) {
    server.sendHeader(F("Location"), F("http://192.168.4.1/"), true);
    server.send(302, F("text/plain"), "");
    return;
  }

  server.send(404, F("text/plain"), F("Not found"));
}

static void configureOtaLogging()
{
  Update.onStart([]() {
    lastOtaLoggedPercent = 0;

    logPrefix("OTA");
    Serial.println(F("Firmware upload started."));
  });

  Update.onProgress([](size_t current, size_t total) {
    if (total == 0) {
      return;
    }

    const uint8_t percent =
        static_cast<uint8_t>((current * 100ULL) / total);
    const uint8_t bucket =
        static_cast<uint8_t>((percent / 25U) * 25U);

    if (bucket >= 25U && bucket > lastOtaLoggedPercent) {
      lastOtaLoggedPercent = bucket;

      logPrefix("OTA");
      Serial.print(F("Progress "));
      Serial.print(bucket);
      Serial.print(F("% ("));
      Serial.print(current);
      Serial.print('/');
      Serial.print(total);
      Serial.println(F(" bytes)"));
    }
  });

  Update.onEnd([]() {
    logPrefix("OTA");
    Serial.print(F("Firmware write complete: "));
    Serial.print(Update.progress());
    Serial.println(F(" bytes; reboot pending."));
  });

  Update.onError([](uint8_t error) {
    logPrefix("OTA");
    Serial.print(F("Update failed: code="));
    Serial.print(error);
    Serial.print(F(" reason="));
    Serial.println(Update.getErrorString());
  });
}

static void configureWebServer()
{
  server.on("/", HTTP_GET, handleRoot);

  server.on("/wifi", HTTP_GET, handleWifiPage);
  server.on("/wifi/save", HTTP_POST, handleWifiSave);

  server.on("/time", HTTP_GET, handleTimePage);
  server.on("/time/save", HTTP_POST, handleTimeSave);
  server.on("/time/sync", HTTP_POST, handleSyncNow);

  server.on("/history", HTTP_GET, handleHistoryPage);
  server.on("/history/clear", HTTP_POST, handleHistoryClear);

  server.on("/system", HTTP_GET, handleSystemPage);
  server.on("/firmware", HTTP_GET, handleFirmwarePage);
  server.on("/system/reboot", HTTP_POST, handleReboot);
  server.on("/system/reconnect-wifi", HTTP_POST, handleReconnectWifi);
  server.on("/system/reset-wifi", HTTP_POST, handleResetWifi);
  server.on("/system/factory-reset", HTTP_POST, handleFactoryReset);

  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/ntp-history", HTTP_GET, handleApiNtpHistory);

  // ESP8266 core-provided browser OTA endpoint.
  // Our styled /firmware page posts the selected binary to /update.
  configureOtaLogging();
  httpUpdater.setup(&server, "/update");

  server.on("/favicon.ico", HTTP_GET, []() {
    server.send(204);
  });

  server.onNotFound(handleNotFound);

  server.begin();

  logPrefix("HTTP");
  Serial.println(F("Web server started on port 80."));
}

// -----------------------------------------------------------------------------
// Arduino setup / loop
// -----------------------------------------------------------------------------

void setup()
{
  // UART0 / GPIO1: diagnostics only.
  Serial.begin(115200);

  // UART1 / GPIO2: proven HU-058D ESP -> STC link.
  Serial1.begin(9600);

  Serial.println();
  Serial.println();

  logPrefix("BOOT");
  Serial.print(FW_NAME);
  Serial.print(' ');
  Serial.print(FW_VERSION);
  Serial.print(F(" build="));
  Serial.println(FW_BUILD);

  logPrefix("SYS");
  Serial.print(F("Reset reason: "));
  Serial.println(ESP.getResetReason());

  const bool configWasValid = loadConfig();

  logPrefix("CFG");
  if (!configWasValid) {
    Serial.println(F("No valid configuration found; defaults loaded."));
    saveConfig();
  } else {
    Serial.println(F("Configuration loaded."));
  }

  settimeofday_cb(onTimeSet);
  applyTimeConfiguration();

  bool connected = false;

  if (hasSavedWifi()) {
    connected = connectStationWithTimeout(15000UL);
  }

  if (!connected) {
    startSetupAp();
  }

  configureWebServer();

  if (WiFi.status() == WL_CONNECTED) {
    startMdns();

    logPrefix("HTTP");
    Serial.print(F("Web UI: http://"));
    Serial.print(WiFi.localIP());
    Serial.print(F("/  mDNS=http://"));
    Serial.print(MDNS_HOST);
    Serial.println(F(".local/"));
  } else {
    logPrefix("HTTP");
    Serial.println(F("Web UI: http://192.168.4.1"));

    logPrefix("AP");
    Serial.print(F("Setup password: "));
    Serial.println(SETUP_AP_PASSWORD);
  }

  // Start periodic jobs from a clean baseline rather than immediately dumping
  // a heartbeat as setup() exits.
  lastHeartbeatMs = millis();
  lastNtpReachSampleMs = millis();
  lastNtpHistoryTestMs = millis();

  if (NTP_HISTORY_TEST_MODE) {
    logPrefix("HISTTEST");
    Serial.println(
        F("TEMPORARY synthetic ring-buffer test enabled: one sample every 5s; no extra NTP traffic."));
  }
}

void loop()
{
  server.handleClient();

  if (setupApActive) {
    dnsServer.processNextRequest();
  }

  if (mdnsActive && WiFi.status() == WL_CONNECTED) {
    MDNS.update();
  }

  // Handle a delayed restart so the HTTP response has time to reach the browser.
  if (restartAtMs != 0 &&
      static_cast<int32_t>(millis() - restartAtMs) >= 0) {
    logPrefix("SYS");
    Serial.println(F("Restarting."));
    delay(50);
    ESP.restart();
  }

  // Reconnect Wi-Fi after the HTTP response has had time to leave the device.
  if (wifiReconnectAtMs != 0 &&
      static_cast<int32_t>(millis() - wifiReconnectAtMs) >= 0) {
    wifiReconnectAtMs = 0;

    logPrefix("WIFI");
    Serial.println(F("Manual reconnect starting."));

    stopMdns();

    WiFi.disconnect(false);
    delay(150);

    if (setupApActive) {
      WiFi.mode(WIFI_AP_STA);
    } else {
      WiFi.mode(WIFI_STA);
    }

    WiFi.begin(config.wifiSsid, config.wifiPassword);
  }

  const wl_status_t wifiStatus = WiFi.status();

  // Track connection transitions. Initial state is captured without repeating
  // the successful connection already logged during setup().
  static bool wifiStateInitialised = false;
  static bool wasConnected = false;

  const bool connectedNow = wifiStatus == WL_CONNECTED;

  if (!wifiStateInitialised) {
    wasConnected = connectedNow;
    wifiStateInitialised = true;
  } else if (connectedNow && !wasConnected) {
    logPrefix("WIFI");
    Serial.print(F("Connected IP="));
    Serial.print(WiFi.localIP());
    Serial.print(F(" RSSI="));
    Serial.print(WiFi.RSSI());
    Serial.println(F(" dBm"));

    if (setupApActive) {
      // Give any just-completed association a moment to settle.
      delay(100);
      stopSetupAp();
    }

    startMdns();
  } else if (!connectedNow && wasConnected) {
    logPrefix("WIFI");
    Serial.print(F("Disconnected status="));
    Serial.println(wifiStatusText(wifiStatus));

    stopMdns();
  }

  wasConnected = connectedNow;

  // Periodically retry a saved network when disconnected.
  if (hasSavedWifi() &&
      wifiStatus != WL_CONNECTED &&
      millis() - lastReconnectAttemptMs >= 30000UL) {
    lastReconnectAttemptMs = millis();

    logPrefix("WIFI");
    Serial.println(F("Retrying saved network."));

    if (WiFi.getMode() == WIFI_AP) {
      WiFi.mode(WIFI_AP_STA);
    }

    WiFi.begin(config.wifiSsid, config.wifiPassword);
  }

  // Reachability can change on failed SNTP polls without a time-set callback,
  // so sample the RFC 5905 registers separately.
  if (millis() - lastNtpReachSampleMs >= NTP_REACH_SAMPLE_INTERVAL_MS) {
    lastNtpReachSampleMs = millis();
    sampleNtpReachability(false);
  }

  // TEMPORARY local-only ring-buffer rollover test. This does not contact any
  // NTP server; it only inserts synthetic samples into the existing history
  // storage path.
  if (NTP_HISTORY_TEST_MODE &&
      millis() - lastNtpHistoryTestMs >= NTP_HISTORY_TEST_INTERVAL_MS) {
    lastNtpHistoryTestMs = millis();
    injectNtpHistoryTestSample();
  }

  // Human-friendly proof of life for a laptop attached to the service UART.
  if (millis() - lastHeartbeatMs >= SERIAL_HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = millis();
    logHeartbeat();
  }

  const time_t now = time(nullptr);

  if (now >= MIN_VALID_EPOCH) {
    if (now != lastPacketEpoch) {
      lastPacketEpoch = now;

      struct tm localTime;
      localtime_r(&now, &localTime);

      sendTimePacket(localTime);
    }
  } else {
    // Match the factory behaviour while waiting for usable time.
    if (millis() - lastWaitingPacketMs >= 1000UL) {
      lastWaitingPacketMs = millis();
      sendControlPacket(0x02);
    }
  }

  delay(5);
}
