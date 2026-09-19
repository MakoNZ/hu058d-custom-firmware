/*
  HU-058D Custom Firmware v0.07-dev
  ===============================

  Target:
    ESP-01S / ESP8266EX
    1 MiB flash
    HU-058D clock PCB

  v0.07-dev:
    - everything proven in v0.05
    - diagnostic-only raw four-timestamp NTP probe
    - NTP offset, network RTT, server processing time, stratum and packet metadata
    - manual probe page/API; probes never set or adjust the system clock
    - record the resolved SNTP peer IP with each history sample
    - keep ESP8266 station Wi-Fi in no-sleep mode for lower and more stable NTP latency
    - MQTT telemetry with configurable broker, credentials, topic and publish interval

  Configuration v2 is migrated automatically to v3 when MQTT settings are saved.

  No external Arduino libraries are required beyond the ESP8266 Arduino core.
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
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
static const char *FW_VERSION = "v0.07-dev";
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

// The raw NTP probe is manual and diagnostic-only. It sends one UDP request
// per button press, never adjusts the clock, and has a small local cooldown so
// an enthusiastic browser finger cannot accidentally become a packet cannon.
static const uint16_t RAW_NTP_LOCAL_PORT = 2390;
static const uint32_t RAW_NTP_TIMEOUT_MS = 2500UL;
static const uint32_t RAW_NTP_MIN_GAP_MS = 5000UL;
static const uint32_t NTP_UNIX_EPOCH_OFFSET = 2208988800UL;

// ESP8266 lwIP is configured for three SNTP servers.
static const uint8_t NTP_SERVER_COUNT = 3;

// Keep two days of hourly time-quality history in RAM. The buffer is deliberately
// not persisted in v0.05 so we can study the data structure and memory behaviour
// before introducing flash wear and persistent-history migration problems.
static const uint8_t NTP_HISTORY_CAPACITY = 48;

// The default SNTP refresh interval is one hour. After 75 minutes without a
// successful update we call the clock "HOLDOVER" rather than pretending the
// network time source is still current.
static const uint32_t NTP_HOLDOVER_AFTER_SECONDS = 4500UL;

// -----------------------------------------------------------------------------
// Persistent configuration
// -----------------------------------------------------------------------------

static const uint32_t CONFIG_MAGIC   = 0x48553032UL; // "HU02"
static const uint16_t CONFIG_VERSION = 3;
static const size_t EEPROM_SIZE      = 1024;

struct DeviceConfig {
  uint32_t magic;
  uint16_t version;

  char wifiSsid[33];
  char wifiPassword[65];

  char timezone[80];

  char ntp1[64];
  char ntp2[64];
  char ntp3[64];

  uint8_t mqttEnabled;
  char mqttHost[64];
  uint16_t mqttPort;
  char mqttUsername[33];
  char mqttPassword[65];
  char mqttTopic[65];
  uint32_t mqttIntervalSeconds;

  uint32_t checksum;
};

// Exact v2 layout, retained only so existing clocks can migrate without losing
// Wi-Fi/time settings when the larger v3 MQTT configuration is introduced.
struct LegacyDeviceConfigV2 {
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
static WiFiClient mqttClient;

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
  uint8_t serverIp[4];
};

static NtpHistorySample ntpHistory[NTP_HISTORY_CAPACITY];
static uint8_t ntpHistoryHead = 0;
static uint8_t ntpHistoryCount = 0;

enum RawNtpProbeState : uint8_t {
  RAW_NTP_IDLE = 0,
  RAW_NTP_WAITING,
  RAW_NTP_DONE,
  RAW_NTP_ERROR
};

enum RawNtpProbeError : uint8_t {
  RAW_NTP_ERR_NONE = 0,
  RAW_NTP_ERR_WIFI,
  RAW_NTP_ERR_TIME,
  RAW_NTP_ERR_BUSY,
  RAW_NTP_ERR_COOLDOWN,
  RAW_NTP_ERR_DNS,
  RAW_NTP_ERR_UDP,
  RAW_NTP_ERR_SEND,
  RAW_NTP_ERR_TIMEOUT
};

struct RawNtpProbeResult {
  RawNtpProbeState state;
  RawNtpProbeError error;
  uint8_t serverIndex;
  char host[64];
  uint8_t ip[4];
  uint64_t t1Raw;
  int64_t t1NtpUs;
  uint64_t t1MonoUs;
  uint32_t startedMs;
  uint32_t completedEpoch;
  bool timingValid;
  bool originateMatched;
  uint8_t leap;
  uint8_t version;
  uint8_t mode;
  uint8_t stratum;
  int8_t pollExponent;
  int8_t precisionExponent;
  int64_t offsetUs;
  int64_t rttUs;
  int64_t serverProcessingUs;
  int32_t rootDelayRaw;
  uint32_t rootDispersionRaw;
  uint32_t referenceId;
  uint64_t referenceTimestamp;
  uint64_t receiveTimestamp;
  uint64_t transmitTimestamp;
};

static WiFiUDP rawNtpUdp;
static RawNtpProbeResult rawNtpProbe = {};
static uint32_t lastRawNtpProbeStartMs = 0;

static uint32_t restartAtMs = 0;
static uint32_t wifiReconnectAtMs = 0;
static uint32_t lastReconnectAttemptMs = 0;
static uint32_t lastWaitingPacketMs = 0;
static uint32_t lastHeartbeatMs = 0;
static uint32_t lastNtpReachSampleMs = 0;

static uint8_t lastOtaLoggedPercent = 0;

static bool mqttConnected = false;
static uint32_t mqttConnectCount = 0;
static uint32_t mqttPublishCount = 0;
static uint32_t mqttLastConnectAttemptMs = 0;
static uint32_t mqttLastPublishMs = 0;
static uint32_t mqttLastIoMs = 0;
static String mqttLastError;

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

static uint32_t fnv1a(const uint8_t *p, size_t length)
{
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < length; ++i) { hash ^= p[i]; hash *= 16777619UL; }
  return hash;
}

static uint32_t configCrc(const DeviceConfig &cfg)
{
  return fnv1a(reinterpret_cast<const uint8_t *>(&cfg),
               sizeof(DeviceConfig) - sizeof(cfg.checksum));
}

static uint32_t legacyConfigCrc(const LegacyDeviceConfigV2 &cfg)
{
  return fnv1a(reinterpret_cast<const uint8_t *>(&cfg),
               sizeof(LegacyDeviceConfigV2) - sizeof(cfg.checksum));
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

  config.mqttEnabled = 0;
  config.mqttPort = 1883;
  copyText(config.mqttTopic, sizeof(config.mqttTopic), "hu058d/clock");
  config.mqttIntervalSeconds = 60;

  config.checksum = configCrc(config);
}

static bool loadConfig()
{
  EEPROM.begin(EEPROM_SIZE);

  uint32_t storedMagic = 0;
  uint16_t storedVersion = 0;
  EEPROM.get(0, storedMagic);
  EEPROM.get(sizeof(storedMagic), storedVersion);

  if (storedMagic == CONFIG_MAGIC && storedVersion == CONFIG_VERSION) {
    EEPROM.get(0, config);
    if (config.checksum != configCrc(config)) { setConfigDefaults(); return false; }
  } else if (storedMagic == CONFIG_MAGIC && storedVersion == 2) {
    LegacyDeviceConfigV2 oldConfig = {};
    EEPROM.get(0, oldConfig);
    if (oldConfig.checksum != legacyConfigCrc(oldConfig)) { setConfigDefaults(); return false; }

    setConfigDefaults();
    copyText(config.wifiSsid, sizeof(config.wifiSsid), oldConfig.wifiSsid);
    copyText(config.wifiPassword, sizeof(config.wifiPassword), oldConfig.wifiPassword);
    copyText(config.timezone, sizeof(config.timezone), oldConfig.timezone);
    copyText(config.ntp1, sizeof(config.ntp1), oldConfig.ntp1);
    copyText(config.ntp2, sizeof(config.ntp2), oldConfig.ntp2);
    copyText(config.ntp3, sizeof(config.ntp3), oldConfig.ntp3);
    config.checksum = configCrc(config);
    // Migration remains in RAM until the next explicit settings save.
  } else {
    setConfigDefaults();
    return false;
  }

  config.wifiSsid[sizeof(config.wifiSsid)-1] = '\0';
  config.wifiPassword[sizeof(config.wifiPassword)-1] = '\0';
  config.timezone[sizeof(config.timezone)-1] = '\0';
  config.ntp1[sizeof(config.ntp1)-1] = '\0';
  config.ntp2[sizeof(config.ntp2)-1] = '\0';
  config.ntp3[sizeof(config.ntp3)-1] = '\0';
  config.mqttHost[sizeof(config.mqttHost)-1] = '\0';
  config.mqttUsername[sizeof(config.mqttUsername)-1] = '\0';
  config.mqttPassword[sizeof(config.mqttPassword)-1] = '\0';
  config.mqttTopic[sizeof(config.mqttTopic)-1] = '\0';
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

static const char *configuredNtpServerName(uint8_t index)
{
  switch (index) {
    case 0: return config.ntp1;
    case 1: return config.ntp2;
    case 2: return config.ntp3;
    default: return "";
  }
}

static uint32_t readBe32(const uint8_t *p)
{
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}

static uint64_t readBe64(const uint8_t *p)
{
  return (static_cast<uint64_t>(readBe32(p)) << 32) |
         static_cast<uint64_t>(readBe32(p + 4));
}

static void writeBe32(uint8_t *p, uint32_t value)
{
  p[0] = static_cast<uint8_t>(value >> 24);
  p[1] = static_cast<uint8_t>(value >> 16);
  p[2] = static_cast<uint8_t>(value >> 8);
  p[3] = static_cast<uint8_t>(value);
}

static void writeBe64(uint8_t *p, uint64_t value)
{
  writeBe32(p, static_cast<uint32_t>(value >> 32));
  writeBe32(p + 4, static_cast<uint32_t>(value));
}

static uint64_t timevalToNtpTimestamp(const struct timeval &tv)
{
  const uint64_t seconds =
      static_cast<uint64_t>(tv.tv_sec) + NTP_UNIX_EPOCH_OFFSET;
  const uint64_t fraction =
      (static_cast<uint64_t>(tv.tv_usec) << 32) / 1000000ULL;

  return (seconds << 32) | (fraction & 0xFFFFFFFFULL);
}

static int64_t ntpTimestampToUs(uint64_t timestamp)
{
  const uint32_t seconds = static_cast<uint32_t>(timestamp >> 32);
  const uint32_t fraction = static_cast<uint32_t>(timestamp);
  const uint64_t fractionUs =
      (static_cast<uint64_t>(fraction) * 1000000ULL) >> 32;

  return static_cast<int64_t>(seconds) * 1000000LL +
         static_cast<int64_t>(fractionUs);
}

static String formatNtpTimestampUtc(uint64_t timestamp)
{
  if (timestamp == 0) {
    return F("-");
  }

  const uint32_t ntpSeconds = static_cast<uint32_t>(timestamp >> 32);

  if (ntpSeconds < NTP_UNIX_EPOCH_OFFSET) {
    return F("-");
  }

  const time_t unixSeconds =
      static_cast<time_t>(ntpSeconds - NTP_UNIX_EPOCH_OFFSET);
  const uint32_t fraction = static_cast<uint32_t>(timestamp);
  const uint32_t micros = static_cast<uint32_t>(
      (static_cast<uint64_t>(fraction) * 1000000ULL) >> 32);

  struct tm utcTime;
  gmtime_r(&unixSeconds, &utcTime);

  char date[32];
  char out[48];
  strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &utcTime);
  snprintf(out, sizeof(out), "%s.%06lu UTC", date,
           static_cast<unsigned long>(micros));

  return String(out);
}

static void storeIpBytes(uint8_t dst[4], const IPAddress &ip)
{
  for (uint8_t i = 0; i < 4; ++i) {
    dst[i] = ip[i];
  }
}

static IPAddress ipFromBytes(const uint8_t src[4])
{
  return IPAddress(src[0], src[1], src[2], src[3]);
}

static String ipBytesText(const uint8_t src[4])
{
  const IPAddress ip = ipFromBytes(src);
  return ip.toString();
}

static String rawNtpStateText()
{
  switch (rawNtpProbe.state) {
    case RAW_NTP_IDLE:    return F("IDLE");
    case RAW_NTP_WAITING: return F("WAITING");
    case RAW_NTP_DONE:    return F("DONE");
    case RAW_NTP_ERROR:   return F("ERROR");
    default:              return F("UNKNOWN");
  }
}

static String rawNtpErrorText()
{
  switch (rawNtpProbe.error) {
    case RAW_NTP_ERR_NONE:     return F("");
    case RAW_NTP_ERR_WIFI:     return F("Wi-Fi is not connected");
    case RAW_NTP_ERR_TIME:     return F("System time is not synchronised yet");
    case RAW_NTP_ERR_BUSY:     return F("A raw NTP probe is already in progress");
    case RAW_NTP_ERR_COOLDOWN: return F("Please wait a few seconds before probing again");
    case RAW_NTP_ERR_DNS:      return F("DNS lookup failed");
    case RAW_NTP_ERR_UDP:      return F("Could not open the diagnostic UDP socket");
    case RAW_NTP_ERR_SEND:     return F("Could not send the NTP request");
    case RAW_NTP_ERR_TIMEOUT:  return F("No matching NTP response before timeout");
    default:                   return F("Unknown probe error");
  }
}

static String rawNtpLeapText(uint8_t leap)
{
  switch (leap) {
    case 0: return F("No warning");
    case 1: return F("Positive leap second pending");
    case 2: return F("Negative leap second pending");
    case 3: return F("Server unsynchronised");
    default: return F("Unknown");
  }
}

static String rawNtpModeText(uint8_t mode)
{
  switch (mode) {
    case 1: return F("Symmetric active");
    case 2: return F("Symmetric passive");
    case 3: return F("Client");
    case 4: return F("Server");
    case 5: return F("Broadcast");
    default: return String(F("Mode ")) + String(mode);
  }
}

static String rawNtpReferenceIdText()
{
  char hex[16];
  snprintf(hex, sizeof(hex), "0x%08lX",
           static_cast<unsigned long>(rawNtpProbe.referenceId));

  const char chars[5] = {
    static_cast<char>((rawNtpProbe.referenceId >> 24) & 0xFF),
    static_cast<char>((rawNtpProbe.referenceId >> 16) & 0xFF),
    static_cast<char>((rawNtpProbe.referenceId >> 8) & 0xFF),
    static_cast<char>(rawNtpProbe.referenceId & 0xFF),
    '\0'
  };

  bool printable = true;
  for (uint8_t i = 0; i < 4; ++i) {
    if (chars[i] < 32 || chars[i] > 126) {
      printable = false;
      break;
    }
  }

  String out = hex;
  if (printable) {
    out += F(" ('");
    out += chars;
    out += F("')");
  }

  return out;
}

static double rawNtpRootDelayMs()
{
  return static_cast<double>(rawNtpProbe.rootDelayRaw) *
         1000.0 / 65536.0;
}

static double rawNtpRootDispersionMs()
{
  return static_cast<double>(rawNtpProbe.rootDispersionRaw) *
         1000.0 / 65536.0;
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

static void addNtpHistorySample()
{
  if (!ntpQualityValid || lastNtpSyncEpoch < MIN_VALID_EPOCH) {
    return;
  }

  NtpHistorySample &sample = ntpHistory[ntpHistoryHead];

  sample.epoch = static_cast<uint32_t>(lastNtpSyncEpoch);
  sample.correctionMs =
      static_cast<float>(static_cast<double>(lastNtpCorrectionUs) / 1000.0);
  sample.driftPpm = static_cast<float>(lastNtpDriftPpm);
  sample.intervalSeconds =
      static_cast<uint32_t>(lastNtpIntervalUs / 1000000ULL);
  sample.serverIndex = lastNtpServerIndex;
  memset(sample.serverIp, 0, sizeof(sample.serverIp));

  if (lastNtpServerIndex >= 0 &&
      lastNtpServerIndex < static_cast<int8_t>(NTP_SERVER_COUNT)) {
    const ip_addr_t *address =
        sntp_getserver(static_cast<uint8_t>(lastNtpServerIndex));

    if (address != nullptr) {
      const IPAddress ip = *address;
      if (ip.isSet()) {
        storeIpBytes(sample.serverIp, ip);
      }
    }
  }

  ntpHistoryHead =
      static_cast<uint8_t>((ntpHistoryHead + 1U) % NTP_HISTORY_CAPACITY);

  if (ntpHistoryCount < NTP_HISTORY_CAPACITY) {
    ++ntpHistoryCount;
  }
}

static void clearNtpHistory()
{
  memset(ntpHistory, 0, sizeof(ntpHistory));
  ntpHistoryHead = 0;
  ntpHistoryCount = 0;

  logPrefix("HIST");
  Serial.println(F("In-RAM NTP history cleared."));
}

static void scheduleRestart(uint32_t delayMs = 1500)
{
  restartAtMs = millis() + delayMs;
}

static String wifiSleepModeText(WiFiSleepType_t mode)
{
  switch (mode) {
    case WIFI_NONE_SLEEP:  return F("NONE");
    case WIFI_LIGHT_SLEEP: return F("LIGHT");
    case WIFI_MODEM_SLEEP: return F("MODEM");
    default:               return F("UNKNOWN");
  }
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

static void setRawNtpProbeError(RawNtpProbeError error, uint8_t serverIndex)
{
  rawNtpUdp.stop();
  rawNtpProbe = {};
  rawNtpProbe.state = RAW_NTP_ERROR;
  rawNtpProbe.error = error;
  rawNtpProbe.serverIndex = serverIndex;

  const char *host = configuredNtpServerName(serverIndex);
  copyText(rawNtpProbe.host, sizeof(rawNtpProbe.host), host);

  logPrefix("NTPR");
  Serial.print(F("Probe error: "));
  Serial.println(rawNtpErrorText());
}

static bool startRawNtpProbe(uint8_t serverIndex)
{
  if (serverIndex >= NTP_SERVER_COUNT) {
    return false;
  }

  if (rawNtpProbe.state == RAW_NTP_WAITING) {
    logPrefix("NTPR");
    Serial.println(F("Probe request ignored: another probe is already running."));
    return false;
  }

  const uint32_t nowMs = millis();

  if (lastRawNtpProbeStartMs != 0 &&
      static_cast<uint32_t>(nowMs - lastRawNtpProbeStartMs) < RAW_NTP_MIN_GAP_MS) {
    setRawNtpProbeError(RAW_NTP_ERR_COOLDOWN, serverIndex);
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    setRawNtpProbeError(RAW_NTP_ERR_WIFI, serverIndex);
    return false;
  }

  if (time(nullptr) < MIN_VALID_EPOCH) {
    setRawNtpProbeError(RAW_NTP_ERR_TIME, serverIndex);
    return false;
  }

  const char *host = configuredNtpServerName(serverIndex);

  if (host == nullptr || host[0] == '\0') {
    setRawNtpProbeError(RAW_NTP_ERR_DNS, serverIndex);
    return false;
  }

  IPAddress targetIp;
  if (WiFi.hostByName(host, targetIp) != 1 || !targetIp.isSet()) {
    setRawNtpProbeError(RAW_NTP_ERR_DNS, serverIndex);
    return false;
  }

  rawNtpUdp.stop();
  if (!rawNtpUdp.begin(RAW_NTP_LOCAL_PORT)) {
    setRawNtpProbeError(RAW_NTP_ERR_UDP, serverIndex);
    return false;
  }

  uint8_t packet[48] = {0};

  // LI=0, VN=4, Mode=3 (client). Poll=6 and precision=-20 are descriptive
  // client fields; the server response supplies the values we actually study.
  packet[0] = 0x23;
  packet[2] = 6;
  packet[3] = static_cast<uint8_t>(-20);

  if (!rawNtpUdp.beginPacket(targetIp, 123)) {
    setRawNtpProbeError(RAW_NTP_ERR_SEND, serverIndex);
    return false;
  }

  struct timeval tv;
  gettimeofday(&tv, nullptr);
  const uint64_t monoNowUs = micros64();
  const uint64_t t1Raw = timevalToNtpTimestamp(tv);
  const int64_t t1NtpUs = ntpTimestampToUs(t1Raw);
  writeBe64(packet + 40, t1Raw);

  const size_t written = rawNtpUdp.write(packet, sizeof(packet));
  const bool sent = written == sizeof(packet) && rawNtpUdp.endPacket() == 1;

  if (!sent) {
    setRawNtpProbeError(RAW_NTP_ERR_SEND, serverIndex);
    return false;
  }

  rawNtpProbe = {};
  rawNtpProbe.state = RAW_NTP_WAITING;
  rawNtpProbe.error = RAW_NTP_ERR_NONE;
  rawNtpProbe.serverIndex = serverIndex;
  copyText(rawNtpProbe.host, sizeof(rawNtpProbe.host), host);
  storeIpBytes(rawNtpProbe.ip, targetIp);
  rawNtpProbe.t1Raw = t1Raw;
  rawNtpProbe.t1NtpUs = t1NtpUs;
  rawNtpProbe.t1MonoUs = monoNowUs;
  rawNtpProbe.startedMs = nowMs;
  lastRawNtpProbeStartMs = nowMs;

  logPrefix("NTPR");
  Serial.print(F("Probe sent server="));
  Serial.print(serverIndex + 1);
  Serial.print(F(" host="));
  Serial.print(rawNtpProbe.host);
  Serial.print(F(" ip="));
  Serial.println(targetIp);

  return true;
}

static void serviceRawNtpProbe()
{
  if (rawNtpProbe.state != RAW_NTP_WAITING) {
    return;
  }

  const int packetSize = rawNtpUdp.parsePacket();

  if (packetSize > 0) {
    const uint64_t receiveMonoUs = micros64();
    const IPAddress remoteIp = rawNtpUdp.remoteIP();
    const IPAddress expectedIp = ipFromBytes(rawNtpProbe.ip);
    const uint16_t remotePort = rawNtpUdp.remotePort();

    uint8_t packet[48] = {0};
    const int bytesRead = rawNtpUdp.read(packet, sizeof(packet));
    while (rawNtpUdp.available()) {
      rawNtpUdp.read();
    }

    // Ignore unrelated datagrams. Matching the source, mode and echoed
    // originate timestamp keeps a stray UDP packet from becoming a result.
    if (!(remoteIp == expectedIp) || remotePort != 123 || bytesRead < 48) {
      return;
    }

    const uint8_t leap = packet[0] >> 6;
    const uint8_t version = (packet[0] >> 3) & 0x07U;
    const uint8_t mode = packet[0] & 0x07U;
    const uint64_t originate = readBe64(packet + 24);

    if (mode != 4 || version < 3 || originate != rawNtpProbe.t1Raw) {
      return;
    }

    rawNtpProbe.leap = leap;
    rawNtpProbe.version = version;
    rawNtpProbe.mode = mode;
    rawNtpProbe.stratum = packet[1];
    rawNtpProbe.pollExponent = static_cast<int8_t>(packet[2]);
    rawNtpProbe.precisionExponent = static_cast<int8_t>(packet[3]);
    rawNtpProbe.rootDelayRaw = static_cast<int32_t>(readBe32(packet + 4));
    rawNtpProbe.rootDispersionRaw = readBe32(packet + 8);
    rawNtpProbe.referenceId = readBe32(packet + 12);
    rawNtpProbe.referenceTimestamp = readBe64(packet + 16);
    rawNtpProbe.receiveTimestamp = readBe64(packet + 32);
    rawNtpProbe.transmitTimestamp = readBe64(packet + 40);
    rawNtpProbe.originateMatched = true;
    rawNtpProbe.completedEpoch = static_cast<uint32_t>(time(nullptr));

    const bool usableServer =
        rawNtpProbe.stratum >= 1 && rawNtpProbe.stratum <= 15 && leap != 3;
    const bool usableTimestamps =
        rawNtpProbe.receiveTimestamp != 0 && rawNtpProbe.transmitTimestamp != 0;

    rawNtpProbe.timingValid = usableServer && usableTimestamps;

    if (rawNtpProbe.timingValid) {
      const int64_t t2Us = ntpTimestampToUs(rawNtpProbe.receiveTimestamp);
      const int64_t t3Us = ntpTimestampToUs(rawNtpProbe.transmitTimestamp);
      const int64_t elapsedLocalUs = static_cast<int64_t>(
          receiveMonoUs - rawNtpProbe.t1MonoUs);
      const int64_t t4Us = rawNtpProbe.t1NtpUs + elapsedLocalUs;

      rawNtpProbe.serverProcessingUs = t3Us - t2Us;
      rawNtpProbe.rttUs = elapsedLocalUs - rawNtpProbe.serverProcessingUs;
      rawNtpProbe.offsetUs =
          ((t2Us - rawNtpProbe.t1NtpUs) + (t3Us - t4Us)) / 2LL;
    }

    rawNtpProbe.state = RAW_NTP_DONE;
    rawNtpProbe.error = RAW_NTP_ERR_NONE;
    rawNtpUdp.stop();

    logPrefix("NTPR");
    Serial.print(F("Reply server="));
    Serial.print(rawNtpProbe.serverIndex + 1);
    Serial.print(F(" host="));
    Serial.print(rawNtpProbe.host);
    Serial.print(F(" ip="));
    Serial.print(expectedIp);
    Serial.print(F(" stratum="));
    Serial.print(rawNtpProbe.stratum);

    if (rawNtpProbe.timingValid) {
      Serial.print(F(" offset="));
      if (rawNtpProbe.offsetUs >= 0) {
        Serial.print('+');
      }
      Serial.print(static_cast<double>(rawNtpProbe.offsetUs) / 1000.0, 3);
      Serial.print(F(" ms rtt="));
      Serial.print(static_cast<double>(rawNtpProbe.rttUs) / 1000.0, 3);
      Serial.print(F(" ms server_proc="));
      Serial.print(static_cast<double>(rawNtpProbe.serverProcessingUs) / 1000.0, 3);
      Serial.print(F(" ms"));
    } else {
      Serial.print(F(" timing=not-usable"));
    }

    Serial.print(F(" refid="));
    Serial.println(rawNtpReferenceIdText());
    return;
  }

  if (static_cast<uint32_t>(millis() - rawNtpProbe.startedMs) >=
      RAW_NTP_TIMEOUT_MS) {
    const uint8_t serverIndex = rawNtpProbe.serverIndex;
    setRawNtpProbeError(RAW_NTP_ERR_TIMEOUT, serverIndex);
  }
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

  Serial.print(F(" mqtt="));
  Serial.print(config.mqttEnabled ? (mqttConnected ? F("connected") : F("disconnected")) : F("disabled"));

  Serial.print(F(" heap="));
  Serial.print(ESP.getFreeHeap());
  Serial.println(F("B"));
}

// -----------------------------------------------------------------------------
// Wi-Fi
// -----------------------------------------------------------------------------

static void enforceLowLatencyWifi()
{
  const WiFiSleepType_t before = WiFi.getSleepMode();
  const bool setOk = WiFi.setSleepMode(WIFI_NONE_SLEEP);
  const WiFiSleepType_t after = WiFi.getSleepMode();

  logPrefix("WIFI");
  Serial.print(F("Sleep mode "));
  Serial.print(wifiSleepModeText(before));
  Serial.print(F(" -> "));
  Serial.print(wifiSleepModeText(after));
  Serial.print(F(" requested=NONE result="));
  Serial.println(setOk && after == WIFI_NONE_SLEEP ? F("ok") : F("FAILED"));
}

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
  enforceLowLatencyWifi();

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
// MQTT telemetry (MQTT 3.1.1, QoS 0)
// -----------------------------------------------------------------------------

static void mqttDisconnect(const String &reason)
{
  if (mqttClient.connected()) mqttClient.stop();
  mqttConnected = false;
  mqttLastError = reason;
}

static size_t mqttEncodeRemainingLength(uint32_t value, uint8_t *out)
{
  size_t n = 0;
  do {
    uint8_t digit = value % 128U;
    value /= 128U;
    if (value > 0) digit |= 0x80;
    out[n++] = digit;
  } while (value > 0 && n < 4);
  return n;
}

static bool mqttWriteString(uint8_t *buf, size_t cap, size_t &pos, const String &value)
{
  const size_t len = value.length();
  if (len > 65535 || pos + 2 + len > cap) return false;
  buf[pos++] = static_cast<uint8_t>(len >> 8);
  buf[pos++] = static_cast<uint8_t>(len);
  memcpy(buf + pos, value.c_str(), len);
  pos += len;
  return true;
}

static bool mqttPublish(const String &topic, const String &payload, bool retain = false)
{
  if (!mqttClient.connected()) return false;
  uint8_t header[5];
  header[0] = retain ? 0x31 : 0x30;
  const uint32_t remaining = 2U + topic.length() + payload.length();
  const size_t rl = mqttEncodeRemainingLength(remaining, header + 1);
  if (mqttClient.write(header, 1 + rl) != 1 + rl) return false;
  uint8_t tlen[2] = { static_cast<uint8_t>(topic.length() >> 8), static_cast<uint8_t>(topic.length()) };
  if (mqttClient.write(tlen, 2) != 2) return false;
  if (mqttClient.write(reinterpret_cast<const uint8_t *>(topic.c_str()), topic.length()) != topic.length()) return false;
  if (mqttClient.write(reinterpret_cast<const uint8_t *>(payload.c_str()), payload.length()) != payload.length()) return false;
  mqttLastIoMs = millis();
  return true;
}

static String mqttBaseTopic()
{
  String topic(config.mqttTopic);
  while (topic.endsWith("/")) topic.remove(topic.length() - 1);
  return topic;
}

static bool mqttConnectBroker()
{
  if (!config.mqttEnabled || config.mqttHost[0] == '\0' || WiFi.status() != WL_CONNECTED) return false;
  mqttClient.stop();
  if (!mqttClient.connect(config.mqttHost, config.mqttPort)) {
    mqttLastError = F("TCP connection failed");
    return false;
  }

  uint8_t packet[512];
  size_t pos = 5; // reserve fixed header + max remaining-length bytes
  if (!mqttWriteString(packet, sizeof(packet), pos, F("MQTT"))) return false;
  packet[pos++] = 4;
  uint8_t flags = 0x02; // clean session
  if (config.mqttUsername[0]) flags |= 0x80;
  if (config.mqttPassword[0]) flags |= 0x40;
  flags |= 0x24; // will flag + retained will, QoS 0
  packet[pos++] = flags;
  packet[pos++] = 0; packet[pos++] = 60; // keepalive

  String clientId = F("hu058d-"); clientId += String(ESP.getChipId(), HEX);
  if (!mqttWriteString(packet, sizeof(packet), pos, clientId) ||
      !mqttWriteString(packet, sizeof(packet), pos, mqttBaseTopic() + F("/availability")) ||
      !mqttWriteString(packet, sizeof(packet), pos, F("offline"))) { mqttClient.stop(); return false; }
  if (config.mqttUsername[0] && !mqttWriteString(packet, sizeof(packet), pos, String(config.mqttUsername))) { mqttClient.stop(); return false; }
  if (config.mqttPassword[0] && !mqttWriteString(packet, sizeof(packet), pos, String(config.mqttPassword))) { mqttClient.stop(); return false; }

  const size_t bodyStart = 5;
  const uint32_t remaining = pos - bodyStart;
  uint8_t fixed[5]; fixed[0] = 0x10;
  const size_t rl = mqttEncodeRemainingLength(remaining, fixed + 1);
  if (mqttClient.write(fixed, 1 + rl) != 1 + rl || mqttClient.write(packet + bodyStart, remaining) != remaining) {
    mqttClient.stop(); mqttLastError = F("CONNECT write failed"); return false;
  }

  const uint32_t deadline = millis() + 1200UL;
  while (mqttClient.connected() && mqttClient.available() < 4 && static_cast<int32_t>(millis() - deadline) < 0) delay(1);
  if (mqttClient.available() < 4) { mqttClient.stop(); mqttLastError = F("CONNACK timeout"); return false; }
  uint8_t reply[4]; mqttClient.read(reply, 4);
  if (reply[0] != 0x20 || reply[1] != 0x02 || reply[3] != 0x00) {
    mqttClient.stop(); mqttLastError = String(F("CONNACK error ")) + String(reply[3]); return false;
  }

  mqttConnected = true;
  mqttConnectCount++;
  mqttLastError = F("None");
  mqttLastIoMs = millis();
  mqttPublish(mqttBaseTopic() + F("/availability"), F("online"), true);
  logPrefix("MQTT"); Serial.print(F("Connected broker=")); Serial.print(config.mqttHost);
  Serial.print(':'); Serial.print(config.mqttPort); Serial.print(F(" topic=")); Serial.println(mqttBaseTopic());
  return true;
}

static String mqttTelemetryJson()
{
  const time_t now = time(nullptr);
  const uint32_t age = ntpSyncAgeSeconds();
  String j; j.reserve(520);
  j += F("{\"epoch\":"); j += now >= MIN_VALID_EPOCH ? String(static_cast<uint32_t>(now)) : String(0);
  j += F(",\"local_time\":\""); j += jsonEscape(currentLocalTime()); j += '"';
  j += F(",\"uptime_s\":"); j += String(millis()/1000UL);
  j += F(",\"rssi_dbm\":"); j += WiFi.status()==WL_CONNECTED ? String(WiFi.RSSI()) : String(0);
  j += F(",\"free_heap\":"); j += String(ESP.getFreeHeap());
  j += F(",\"ntp_state\":\""); j += jsonEscape(ntpStateText()); j += '"';
  j += F(",\"ntp_age_s\":"); j += age==UINT32_MAX ? String(F("null")) : String(age);
  j += F(",\"ntp_sync_count\":"); j += String(ntpSyncCount);
  j += F(",\"ntp_failure_count\":"); j += String(ntpObservedFailureCount);
  j += F(",\"history_count\":"); j += String(ntpHistoryCount);
  j += F(",\"correction_ms\":"); j += ntpQualityValid ? String((double)lastNtpCorrectionUs/1000.0,3) : String(F("null"));
  j += F(",\"drift_ppm\":"); j += ntpQualityValid ? String(lastNtpDriftPpm,3) : String(F("null"));
  j += F(",\"ntp_server\":\""); j += jsonEscape(lastNtpServerDisplay()); j += '"';
  j += F(",\"wifi_sleep\":\""); j += jsonEscape(wifiSleepModeText(WiFi.getSleepMode())); j += F("\"}");
  return j;
}

static bool mqttPublishTelemetry()
{
  if (!mqttConnected || !mqttClient.connected()) return false;
  const bool ok = mqttPublish(mqttBaseTopic() + F("/telemetry"), mqttTelemetryJson(), false);
  if (ok) { mqttPublishCount++; mqttLastPublishMs = millis(); }
  else mqttDisconnect(F("Publish failed"));
  return ok;
}

static void serviceMqtt()
{
  if (!config.mqttEnabled) { if (mqttClient.connected()) mqttClient.stop(); mqttConnected=false; return; }
  if (WiFi.status() != WL_CONNECTED) { mqttDisconnect(F("Wi-Fi disconnected")); return; }
  while (mqttClient.available()) mqttClient.read();
  if (!mqttClient.connected()) {
    mqttConnected = false;
    if (millis() - mqttLastConnectAttemptMs >= 15000UL) {
      mqttLastConnectAttemptMs = millis();
      if (mqttConnectBroker()) mqttPublishTelemetry();
    }
    return;
  }
  mqttConnected = true;
  const uint32_t intervalSeconds = config.mqttIntervalSeconds < 10UL ? 10UL : config.mqttIntervalSeconds;
  const uint32_t intervalMs = intervalSeconds * 1000UL;
  if (millis() - mqttLastPublishMs >= intervalMs) mqttPublishTelemetry();
  if (millis() - mqttLastIoMs >= 30000UL) {
    const uint8_t ping[2] = {0xC0,0x00};
    if (mqttClient.write(ping,2)==2) mqttLastIoMs=millis(); else mqttDisconnect(F("PING failed"));
  }
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
            "<a class='btn' href='/probe'>NTP Probe</a>"
            "<a class='btn' href='/mqtt'>MQTT</a>"
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

  html += F("<div class='k'>Wi-Fi sleep</div><div class='v'>");
  html += htmlEscape(wifiSleepModeText(WiFi.getSleepMode()));
  html += F(" <span class='ok'>(forced NONE for timing)</span></div>");

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
      "<div class='metric'><div class='k'>Weighted drift</div><div class='mv' id='hist-weighted'>-</div></div>"
      "<div class='metric'><div class='k'>Drift range</div><div class='mv' id='hist-range'>-</div></div>"
      "<div class='metric'><div class='k'>Latest correction</div><div class='mv' id='hist-latest'>-</div></div>"
    "</div>"
    "</div>"

    "<div class='card'><h2>Estimated oscillator drift</h2>"
    "<div class='chart-wrap'><canvas class='chart' id='drift-chart'></canvas></div>"
    "<p class='hint'>Positive values mean the ESP8266 free-running clock was fast; negative values mean slow. Weighted drift uses the net correction over the total measured interval, so it is less misleading when sample intervals differ.</p>"
    "</div>"

    "<div class='card'><h2>NTP clock correction</h2>"
    "<div class='chart-wrap'><canvas class='chart' id='correction-chart'></canvas></div>"
    "<p class='hint'>Positive values mean SNTP moved the system clock forward; negative values moved it backward.</p>"
    "</div>"

    "<div class='card'><h2>Recent samples</h2>"
    "<div class='table-scroll'><table>"
    "<thead><tr><th>Time</th><th>Correction</th><th>Drift</th><th>Interval</th><th>Server</th><th>Peer IP</th></tr></thead>"
    "<tbody id='history-rows'><tr><td colspan='6'>Loading...</td></tr></tbody>"
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
          "const totalS=d.reduce((a,s)=>a+Number(s.interval_s||0),0),totalC=d.reduce((a,s)=>a+Number(s.correction_ms||0),0);"
          "document.getElementById('hist-weighted').textContent=totalS?fmtSigned(-totalC*1000/totalS,3,'ppm'):'-';"
          "document.getElementById('hist-range').textContent=Math.min(...dr).toFixed(3)+' to '+Math.max(...dr).toFixed(3)+' ppm';"
          "document.getElementById('hist-latest').textContent=fmtSigned(d[d.length-1].correction_ms,3,'ms');"
        "}else{document.getElementById('hist-mean').textContent='-';document.getElementById('hist-weighted').textContent='-';document.getElementById('hist-range').textContent='-';document.getElementById('hist-latest').textContent='-';}"
        "drawChart('drift-chart',d,'drift_ppm','ppm');drawChart('correction-chart',d,'correction_ms','ms');"
        "const rows=document.getElementById('history-rows');rows.innerHTML='';"
        "if(!d.length){rows.innerHTML=\"<tr><td colspan='6'>Waiting for the second suitable NTP sync.</td></tr>\";return;}"
        "d.slice().reverse().slice(0,12).forEach(s=>{const tr=document.createElement('tr');"
          "const vals=[new Date(s.epoch*1000).toLocaleString(),fmtSigned(s.correction_ms,3,'ms'),fmtSigned(s.drift_ppm,3,'ppm'),fmtDur(s.interval_s),s.server||'Unknown',s.server_ip||'-'];"
          "vals.forEach(v=>{const td=document.createElement('td');td.textContent=v;tr.appendChild(td);});rows.appendChild(tr);"
        "});"
      "}catch(e){console.log('history refresh failed',e);}"
    "}"
    "window.addEventListener('resize',()=>loadHistory());loadHistory();setInterval(loadHistory,60000);"
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

static void handleRawNtpProbePage()
{
  String html = pageStart(F("HU-058D Raw NTP Probe"));
  html.reserve(8200);

  html += F(
    "<div class='card'><h2>Raw four-timestamp NTP probe</h2>"
    "<p>This is a diagnostic-only NTP client. A button press sends exactly one UDP "
    "request to the selected configured server. It does not set the clock, restart "
    "SNTP, or change the normal hourly synchronisation schedule.</p>"
    "<p class='hint'>Offset uses the standard four timestamps T1/T2/T3/T4. Network RTT "
    "uses the ESP8266 monotonic timer for the local elapsed interval. Because packet "
    "receipt is noticed by the main event loop rather than hardware timestamping, the "
    "RTT and offset are useful diagnostics rather than laboratory measurements.</p>"
    "</div>"
    "<div class='card'><h2>Probe a configured server</h2>"
  );

  for (uint8_t i = 0; i < NTP_SERVER_COUNT; ++i) {
    const char *host = configuredNtpServerName(i);

    html += F("<form class='inline' method='post' action='/probe/start'>"
              "<button type='submit' name='server' value='");
    html += String(i);
    html += F("'>Probe ");
    html += String(i + 1);
    html += F("</button></form> <code>");
    html += htmlEscape(String((host != nullptr && host[0] != '\0') ? host : "(empty)"));
    html += F("</code><br><br>");
  }

  html += F(
    "</div>"
    "<div class='card'><h2>Last probe</h2><div class='grid'>"
    "<div class='k'>State</div><div class='v' id='rp-state'>-</div>"
    "<div class='k'>Error</div><div class='v' id='rp-error'>-</div>"
    "<div class='k'>Server</div><div class='v' id='rp-server'>-</div>"
    "<div class='k'>Peer IP</div><div class='v' id='rp-ip'>-</div>"
    "<div class='k'>Offset</div><div class='v' id='rp-offset'>-</div>"
    "<div class='k'>Network RTT</div><div class='v' id='rp-rtt'>-</div>"
    "<div class='k'>Server processing</div><div class='v' id='rp-processing'>-</div>"
    "<div class='k'>Stratum</div><div class='v' id='rp-stratum'>-</div>"
    "<div class='k'>Leap indicator</div><div class='v' id='rp-leap'>-</div>"
    "<div class='k'>NTP version / mode</div><div class='v' id='rp-version'>-</div>"
    "<div class='k'>Poll exponent</div><div class='v' id='rp-poll'>-</div>"
    "<div class='k'>Precision exponent</div><div class='v' id='rp-precision'>-</div>"
    "<div class='k'>Root delay</div><div class='v' id='rp-root-delay'>-</div>"
    "<div class='k'>Root dispersion</div><div class='v' id='rp-root-disp'>-</div>"
    "<div class='k'>Reference ID</div><div class='v' id='rp-refid'>-</div>"
    "<div class='k'>Reference timestamp</div><div class='v' id='rp-ref-time'>-</div>"
    "<div class='k'>Server receive (T2)</div><div class='v' id='rp-t2'>-</div>"
    "<div class='k'>Server transmit (T3)</div><div class='v' id='rp-t3'>-</div>"
    "<div class='k'>Originate match</div><div class='v' id='rp-origin'>-</div>"
    "<div class='k'>Completed</div><div class='v' id='rp-completed'>-</div>"
    "</div>"
    "<p class='hint'>A positive offset means the NTP server says the ESP clock is behind; "
    "a negative offset means the ESP clock is ahead. This is not the same quantity as "
    "the hourly correction shown on the History page.</p></div>"
    "<script>"
    "function put(id,v){const e=document.getElementById(id);if(e)e.textContent=(v===null||v===undefined||v==='')?'-':v;}"
    "function ms(v,signed){if(v===null||v===undefined)return '-';return (signed&&Number(v)>=0?'+':'')+Number(v).toFixed(3)+' ms';}"
    "async function loadProbe(){try{const r=await fetch('/api/ntp-probe',{cache:'no-store'});const p=await r.json();"
      "put('rp-state',p.state);put('rp-error',p.error);put('rp-server',p.server);put('rp-ip',p.ip);"
      "put('rp-offset',p.timing_valid?ms(p.offset_ms,true):'-');"
      "put('rp-rtt',p.timing_valid?ms(p.rtt_ms,false):'-');"
      "put('rp-processing',p.timing_valid?ms(p.server_processing_ms,false):'-');"
      "put('rp-stratum',p.has_reply?p.stratum:'-');put('rp-leap',p.has_reply?p.leap:'-');"
      "put('rp-version',p.has_reply?('v'+p.version+' / '+p.mode):'-');"
      "put('rp-poll',p.has_reply?p.poll_exponent:'-');put('rp-precision',p.has_reply?p.precision_exponent:'-');"
      "put('rp-root-delay',p.has_reply?ms(p.root_delay_ms,false):'-');put('rp-root-disp',p.has_reply?ms(p.root_dispersion_ms,false):'-');"
      "put('rp-refid',p.has_reply?p.reference_id:'-');put('rp-ref-time',p.has_reply?p.reference_time_utc:'-');"
      "put('rp-t2',p.has_reply?p.receive_time_utc:'-');put('rp-t3',p.has_reply?p.transmit_time_utc:'-');"
      "put('rp-origin',p.has_reply?(p.originate_match?'Matched':'MISMATCH'):'-');put('rp-completed',p.completed_utc);"
    "}catch(e){}}loadProbe();setInterval(loadProbe,1000);"
    "</script>"
  );

  sendHtml(html + pageEnd());
}

static void handleRawNtpProbeStart()
{
  if (!server.hasArg(F("server"))) {
    sendHtml(pageStart(F("NTP Probe Error")) +
             F("<div class='card'><h2>Missing server</h2>"
               "<a class='btn' href='/probe'>Back</a></div>") +
             pageEnd(), 400);
    return;
  }

  const int requested = server.arg(F("server")).toInt();

  if (requested < 0 || requested >= static_cast<int>(NTP_SERVER_COUNT)) {
    sendHtml(pageStart(F("NTP Probe Error")) +
             F("<div class='card'><h2>Invalid server</h2>"
               "<a class='btn' href='/probe'>Back</a></div>") +
             pageEnd(), 400);
    return;
  }

  startRawNtpProbe(static_cast<uint8_t>(requested));

  server.sendHeader(F("Location"), F("/probe"), true);
  server.send(303, F("text/plain"), F("Probe requested"));
}

static void handleApiRawNtpProbe()
{
  String json;
  json.reserve(1500);

  const bool hasReply = rawNtpProbe.state == RAW_NTP_DONE;

  json += F("{\"state\":\"");
  json += jsonEscape(rawNtpStateText());
  json += F("\",\"error\":\"");
  json += jsonEscape(rawNtpErrorText());
  json += F("\",\"server_index\":");
  json += String(rawNtpProbe.serverIndex);
  json += F(",\"server\":\"");
  if (rawNtpProbe.host[0] != '\0') {
    json += jsonEscape(String(rawNtpProbe.host));
  }
  json += F("\",\"ip\":\"");
  if (rawNtpProbe.ip[0] != 0 || rawNtpProbe.ip[1] != 0 ||
      rawNtpProbe.ip[2] != 0 || rawNtpProbe.ip[3] != 0) {
    json += ipBytesText(rawNtpProbe.ip);
  }
  json += F("\",\"has_reply\":");
  json += hasReply ? F("true") : F("false");
  json += F(",\"timing_valid\":");
  json += rawNtpProbe.timingValid ? F("true") : F("false");

  if (rawNtpProbe.timingValid) {
    json += F(",\"offset_ms\":");
    json += String(static_cast<double>(rawNtpProbe.offsetUs) / 1000.0, 3);
    json += F(",\"rtt_ms\":");
    json += String(static_cast<double>(rawNtpProbe.rttUs) / 1000.0, 3);
    json += F(",\"server_processing_ms\":");
    json += String(static_cast<double>(rawNtpProbe.serverProcessingUs) / 1000.0, 3);
  } else {
    json += F(",\"offset_ms\":null,\"rtt_ms\":null,\"server_processing_ms\":null");
  }

  json += F(",\"stratum\":");
  json += String(rawNtpProbe.stratum);
  json += F(",\"leap\":\"");
  json += hasReply ? jsonEscape(rawNtpLeapText(rawNtpProbe.leap)) : String();
  json += F("\",\"version\":");
  json += String(rawNtpProbe.version);
  json += F(",\"mode\":\"");
  json += hasReply ? jsonEscape(rawNtpModeText(rawNtpProbe.mode)) : String();
  json += F("\",\"poll_exponent\":");
  json += String(static_cast<int>(rawNtpProbe.pollExponent));
  json += F(",\"precision_exponent\":");
  json += String(static_cast<int>(rawNtpProbe.precisionExponent));
  json += F(",\"root_delay_ms\":");
  json += hasReply ? String(rawNtpRootDelayMs(), 3) : String(F("null"));
  json += F(",\"root_dispersion_ms\":");
  json += hasReply ? String(rawNtpRootDispersionMs(), 3) : String(F("null"));
  json += F(",\"reference_id\":\"");
  json += hasReply ? jsonEscape(rawNtpReferenceIdText()) : String();
  json += F("\",\"reference_time_utc\":\"");
  json += hasReply ? jsonEscape(formatNtpTimestampUtc(rawNtpProbe.referenceTimestamp)) : String();
  json += F("\",\"receive_time_utc\":\"");
  json += hasReply ? jsonEscape(formatNtpTimestampUtc(rawNtpProbe.receiveTimestamp)) : String();
  json += F("\",\"transmit_time_utc\":\"");
  json += hasReply ? jsonEscape(formatNtpTimestampUtc(rawNtpProbe.transmitTimestamp)) : String();
  json += F("\",\"originate_match\":");
  json += rawNtpProbe.originateMatched ? F("true") : F("false");
  json += F(",\"completed_utc\":\"");
  if (rawNtpProbe.completedEpoch >= static_cast<uint32_t>(MIN_VALID_EPOCH)) {
    json += jsonEscape(formatEpochUtc(static_cast<time_t>(rawNtpProbe.completedEpoch)));
  }
  json += F("\"}");

  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), json);
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

static void handleMqttPage()
{
  String html = pageStart(F("HU-058D MQTT"));
  html += F("<div class='card'><h2>MQTT telemetry</h2><div class='grid'>"
            "<div class='k'>State</div><div class='v'>");
  html += config.mqttEnabled ? (mqttConnected ? F("Connected") : F("Disconnected")) : F("Disabled");
  html += F("</div><div class='k'>Connections</div><div class='v'>"); html += String(mqttConnectCount);
  html += F("</div><div class='k'>Telemetry publishes</div><div class='v'>"); html += String(mqttPublishCount);
  html += F("</div><div class='k'>Last error</div><div class='v'>"); html += htmlEscape(mqttLastError.length()?mqttLastError:String(F("None")));
  html += F("</div></div></div><div class='card'><h2>Configuration</h2>"
            "<form method='post' action='/mqtt/save'>"
            "<label><input style='width:auto' type='checkbox' name='enabled' value='1'");
  if (config.mqttEnabled) html += F(" checked");
  html += F("> Enable MQTT</label><label for='host'>Broker hostname or IP</label><input id='host' name='host' maxlength='63' value='");
  html += htmlEscape(String(config.mqttHost));
  html += F("'><label for='port'>Port</label><input id='port' name='port' type='number' min='1' max='65535' value='"); html += String(config.mqttPort);
  html += F("'><label for='username'>Username</label><input id='username' name='username' maxlength='32' value='"); html += htmlEscape(String(config.mqttUsername));
  html += F("'><label for='password'>Password</label><input id='password' name='password' maxlength='64' type='password' autocomplete='new-password'>"
            "<p class='hint'>Leave password blank to keep the existing password.</p>"
            "<label for='topic'>Base topic</label><input id='topic' name='topic' maxlength='64' value='"); html += htmlEscape(String(config.mqttTopic));
  html += F("'><p class='hint'>Telemetry: <code>&lt;base&gt;/telemetry</code>; retained availability: <code>&lt;base&gt;/availability</code>.</p>"
            "<label for='interval'>Publish interval (seconds)</label><input id='interval' name='interval' type='number' min='10' max='86400' value='"); html += String(config.mqttIntervalSeconds);
  html += F("'><div class='actions'><button type='submit'>Save MQTT settings</button></div></form></div>");
  sendHtml(html + pageEnd());
}

static void handleMqttSave()
{
  String host=server.arg(F("host")); String user=server.arg(F("username")); String pass=server.arg(F("password")); String topic=server.arg(F("topic"));
  host.trim(); user.trim(); topic.trim();
  const long port=server.arg(F("port")).toInt(); const long interval=server.arg(F("interval")).toInt(); const bool enabled=server.hasArg(F("enabled"));
  if ((enabled && host.length()==0) || host.length()>63 || user.length()>32 || pass.length()>64 || topic.length()==0 || topic.length()>64 || port<1 || port>65535 || interval<10 || interval>86400) {
    sendHtml(pageStart(F("MQTT Error"))+F("<div class='card'><h2>Invalid MQTT settings</h2><a class='btn' href='/mqtt'>Back</a></div>")+pageEnd(),400); return;
  }
  config.mqttEnabled=enabled?1:0; copyText(config.mqttHost,sizeof(config.mqttHost),host); config.mqttPort=(uint16_t)port; copyText(config.mqttUsername,sizeof(config.mqttUsername),user);
  if (pass.length()>0) copyText(config.mqttPassword,sizeof(config.mqttPassword),pass);
  copyText(config.mqttTopic,sizeof(config.mqttTopic),topic); config.mqttIntervalSeconds=(uint32_t)interval;
  if (!saveConfig()) { sendHtml(pageStart(F("Save Error"))+F("<div class='card'><h2>Save failed</h2></div>")+pageEnd(),500); return; }
  mqttDisconnect(F("Configuration changed")); mqttLastConnectAttemptMs = millis()-15000UL;
  sendHtml(pageStart(F("MQTT Saved"))+F("<div class='card'><h2>MQTT settings saved</h2><p>The clock will apply them immediately.</p><a class='btn' href='/mqtt'>MQTT status</a></div>")+pageEnd());
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

  html += F("<div class='k'>Wi-Fi sleep mode</div><div class='v'>");
  html += htmlEscape(wifiSleepModeText(WiFi.getSleepMode()));
  html += F(" (firmware target: NONE)</div>");

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
  json += F(",\"wifi_sleep\":\"");
  json += jsonEscape(wifiSleepModeText(WiFi.getSleepMode()));
  json += F("\"");

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

  json += F(",\"mqtt_enabled\":");
  json += config.mqttEnabled ? F("true") : F("false");
  json += F(",\"mqtt_connected\":");
  json += mqttConnected ? F("true") : F("false");
  json += F(",\"mqtt_publish_count\":");
  json += String(mqttPublishCount);

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
  json.reserve(9000);

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
    } else {
      json += F("Unknown");
    }

    json += F("\",\"server_ip\":\"");
    const IPAddress sampleIp = ipFromBytes(sample.serverIp);
    if (sampleIp.isSet()) {
      json += sampleIp.toString();
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

  server.on("/probe", HTTP_GET, handleRawNtpProbePage);
  server.on("/probe/start", HTTP_POST, handleRawNtpProbeStart);

  server.on("/mqtt", HTTP_GET, handleMqttPage);
  server.on("/mqtt/save", HTTP_POST, handleMqttSave);

  server.on("/system", HTTP_GET, handleSystemPage);
  server.on("/firmware", HTTP_GET, handleFirmwarePage);
  server.on("/system/reboot", HTTP_POST, handleReboot);
  server.on("/system/reconnect-wifi", HTTP_POST, handleReconnectWifi);
  server.on("/system/reset-wifi", HTTP_POST, handleResetWifi);
  server.on("/system/factory-reset", HTTP_POST, handleFactoryReset);

  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/ntp-history", HTTP_GET, handleApiNtpHistory);
  server.on("/api/ntp-probe", HTTP_GET, handleApiRawNtpProbe);

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
}

void loop()
{
  server.handleClient();
  serviceRawNtpProbe();
  serviceMqtt();

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

    enforceLowLatencyWifi();
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
    // Re-assert after every association as well as before WiFi.begin().
    // This covers SDK auto-reconnects and AP/STA mode transitions.
    enforceLowLatencyWifi();

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

    enforceLowLatencyWifi();
    WiFi.begin(config.wifiSsid, config.wifiPassword);
  }

  // Reachability can change on failed SNTP polls without a time-set callback,
  // so sample the RFC 5905 registers separately.
  if (millis() - lastNtpReachSampleMs >= NTP_REACH_SAMPLE_INTERVAL_MS) {
    lastNtpReachSampleMs = millis();
    sampleNtpReachability(false);
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
