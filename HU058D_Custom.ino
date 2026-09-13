/*
  HU-058D Custom Firmware v0.02
  ============================

  Target:
    ESP-01S / ESP8266EX
    1 MiB flash
    HU-058D clock PCB

  v0.02:
    - ESP -> STC clock protocol on GPIO2 / Serial1, 9600 8N1
    - NTP time
    - automatic New Zealand NZST/NZDT by default
    - persistent configuration in EEPROM
    - English web UI available while connected to normal Wi-Fi
    - Wi-Fi scan / configuration
    - fallback setup AP if Wi-Fi is not configured or cannot connect
    - configurable POSIX timezone rule
    - configurable NTP servers
    - NTP "sync now"
    - reboot
    - reset Wi-Fi
    - factory reset
    - status / diagnostics page

  No external Arduino libraries are required beyond the ESP8266 Arduino core.
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <coredecls.h>
#include <time.h>

// -----------------------------------------------------------------------------
// Firmware identity
// -----------------------------------------------------------------------------

static const char *FW_NAME    = "HU-058D Custom Firmware";
static const char *FW_VERSION = "v0.02";

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
static DNSServer dnsServer;

static bool setupApActive = false;
static bool ntpEverSynced = false;

static time_t lastNtpSyncEpoch = 0;
static time_t lastPacketEpoch = 0;

static uint32_t restartAtMs = 0;
static uint32_t lastReconnectAttemptMs = 0;
static uint32_t lastWaitingPacketMs = 0;

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

static void onTimeSet(bool fromSntp)
{
  if (!fromSntp) {
    return;
  }

  lastNtpSyncEpoch = time(nullptr);
  ntpEverSynced = true;

  Serial.print(F("NTP synchronised: "));
  Serial.println(formatEpochLocal(lastNtpSyncEpoch));
}

static void applyTimeConfiguration()
{
  Serial.print(F("Timezone rule: "));
  Serial.println(config.timezone);

  Serial.print(F("NTP servers: "));
  Serial.print(config.ntp1);
  Serial.print(F(", "));
  Serial.print(config.ntp2);
  Serial.print(F(", "));
  Serial.println(config.ntp3);

  configTime(config.timezone, config.ntp1, config.ntp2, config.ntp3);
}

static void requestNtpResync()
{
  ntpEverSynced = false;
  applyTimeConfiguration();
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

  Serial.print(F("Connecting to Wi-Fi SSID: "));
  Serial.println(config.wifiSsid);

  WiFi.begin(config.wifiSsid, config.wifiPassword);
}

static void startSetupAp()
{
  if (setupApActive) {
    return;
  }

  Serial.println(F("Starting setup access point..."));

  if (WiFi.getMode() == WIFI_STA) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_AP);
  }

  WiFi.softAPConfig(SETUP_AP_IP, SETUP_AP_GW, SETUP_AP_MASK);

  if (!WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD)) {
    Serial.println(F("ERROR: failed to start setup AP."));
    return;
  }

  dnsServer.start(53, "*", SETUP_AP_IP);
  setupApActive = true;

  Serial.print(F("Setup AP: "));
  Serial.println(SETUP_AP_SSID);

  Serial.print(F("Setup AP IP: "));
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
  Serial.println(F("Setup AP stopped."));
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
    Serial.print(F("Wi-Fi connected, IP: "));
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println(F("Wi-Fi connection timed out."));
  return false;
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
    "</style></head><body><div class='wrap'>"
  );

  html += F("<h1>HU-058D Clock <small style='color:var(--muted);font-size:.65em'>");
  html += FW_VERSION;
  html += F("</small></h1><nav>"
            "<a class='btn' href='/'>Status</a>"
            "<a class='btn' href='/wifi'>Wi-Fi</a>"
            "<a class='btn' href='/time'>Time</a>"
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

  html += F("<div class='card'><h2>Clock</h2><div class='grid'>");

  html += F("<div class='k'>Current time</div><div class='v'>");
  html += htmlEscape(currentLocalTime());
  html += F("</div>");

  html += F("<div class='k'>NTP status</div><div class='v'>");
  html += timeValid ? F("<span class='ok'>Synchronised</span>")
                    : F("<span class='bad'>Waiting for NTP</span>");
  html += F("</div>");

  html += F("<div class='k'>Last NTP sync</div><div class='v'>");
  html += htmlEscape(formatEpochLocal(lastNtpSyncEpoch));
  html += F("</div>");

  html += F("<div class='k'>Timezone rule</div><div class='v'><code>");
  html += htmlEscape(config.timezone);
  html += F("</code></div>");

  html += F("</div></div>");

  html += F("<div class='card'><h2>Network</h2><div class='grid'>");

  html += F("<div class='k'>Wi-Fi</div><div class='v'>");
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

  html += F("<div class='k'>Signal</div><div class='v'>");
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

  html += F("<div class='k'>Uptime</div><div class='v'>");
  html += htmlEscape(formatUptime());
  html += F("</div>");

  html += F("<div class='k'>Free heap</div><div class='v'>");
  html += String(ESP.getFreeHeap());
  html += F(" bytes</div>");

  html += F("</div></div>");

  sendHtml(html + pageEnd());
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

  html += F("<div class='k'>Chip ID</div><div class='v'>");
  html += String(ESP.getChipId(), HEX);
  html += F("</div>");

  html += F("<div class='k'>Flash size</div><div class='v'>");
  html += String(ESP.getFlashChipRealSize());
  html += F(" bytes</div>");

  html += F("<div class='k'>Free heap</div><div class='v'>");
  html += String(ESP.getFreeHeap());
  html += F(" bytes</div>");

  html += F("<div class='k'>Uptime</div><div class='v'>");
  html += htmlEscape(formatUptime());
  html += F("</div></div></div>");

  html += F(
    "<div class='card'><h2>Actions</h2>"
    "<div class='actions'>"
    "<form class='inline' method='post' action='/system/reboot'>"
    "<button type='submit'>Reboot</button></form>"
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

static void handleReboot()
{
  sendHtml(pageStart(F("Reboot")) +
           F("<div class='card'><h2>Rebooting</h2><p>The clock will restart in a moment.</p></div>") +
           pageEnd());

  scheduleRestart();
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
  String json;
  json.reserve(600);

  json += F("{\"firmware\":\"");
  json += FW_VERSION;
  json += F("\",\"wifi_status\":\"");
  json += jsonEscape(wifiStatusText(WiFi.status()));
  json += F("\",\"ssid\":\"");
  json += jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String());
  json += F("\",\"ip\":\"");
  json += WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String();
  json += F("\",\"rssi\":");
  json += WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : String(0);
  json += F(",\"time\":\"");
  json += jsonEscape(currentLocalTime());
  json += F("\",\"last_ntp_sync\":\"");
  json += jsonEscape(formatEpochLocal(lastNtpSyncEpoch));
  json += F("\",\"setup_ap\":");
  json += setupApActive ? F("true") : F("false");
  json += F(",\"uptime_ms\":");
  json += String(millis());
  json += '}';

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

static void configureWebServer()
{
  server.on("/", HTTP_GET, handleRoot);

  server.on("/wifi", HTTP_GET, handleWifiPage);
  server.on("/wifi/save", HTTP_POST, handleWifiSave);

  server.on("/time", HTTP_GET, handleTimePage);
  server.on("/time/save", HTTP_POST, handleTimeSave);
  server.on("/time/sync", HTTP_POST, handleSyncNow);

  server.on("/system", HTTP_GET, handleSystemPage);
  server.on("/system/reboot", HTTP_POST, handleReboot);
  server.on("/system/reset-wifi", HTTP_POST, handleResetWifi);
  server.on("/system/factory-reset", HTTP_POST, handleFactoryReset);

  server.on("/api/status", HTTP_GET, handleApiStatus);

  server.on("/favicon.ico", HTTP_GET, []() {
    server.send(204);
  });

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println(F("HTTP server started on port 80."));
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
  Serial.print(FW_NAME);
  Serial.print(' ');
  Serial.println(FW_VERSION);

  const bool configWasValid = loadConfig();

  if (!configWasValid) {
    Serial.println(F("No valid v0.02 configuration found; defaults loaded."));
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
    Serial.print(F("Web UI: http://"));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("Web UI: http://192.168.4.1"));
    Serial.print(F("Setup AP password: "));
    Serial.println(SETUP_AP_PASSWORD);
  }
}

void loop()
{
  server.handleClient();

  if (setupApActive) {
    dnsServer.processNextRequest();
  }

  // Handle a delayed restart so the HTTP response has time to reach the browser.
  if (restartAtMs != 0 &&
      static_cast<int32_t>(millis() - restartAtMs) >= 0) {
    delay(50);
    ESP.restart();
  }

  const wl_status_t wifiStatus = WiFi.status();

  // If we are in setup/fallback mode and later manage to connect, keep the web
  // UI on the station interface and stop advertising the setup AP.
  static bool wasConnected = false;
  const bool connectedNow = wifiStatus == WL_CONNECTED;

  if (connectedNow && !wasConnected) {
    Serial.print(F("Wi-Fi connected, IP: "));
    Serial.println(WiFi.localIP());

    if (setupApActive) {
      // Give any just-completed association a moment to settle.
      delay(100);
      stopSetupAp();
    }
  }

  wasConnected = connectedNow;

  // Periodically retry a saved network when disconnected.
  if (hasSavedWifi() &&
      wifiStatus != WL_CONNECTED &&
      millis() - lastReconnectAttemptMs >= 30000UL) {
    lastReconnectAttemptMs = millis();

    Serial.println(F("Retrying Wi-Fi connection..."));

    if (WiFi.getMode() == WIFI_AP) {
      WiFi.mode(WIFI_AP_STA);
    }

    WiFi.begin(config.wifiSsid, config.wifiPassword);
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
