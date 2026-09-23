# HU-058D Custom Firmware

Replacement firmware for the ESP-01S / ESP8266 used in the HU-058D Wi-Fi NTP clock.

The project began as a reverse-engineering exercise to replace the original Chinese firmware with an English, locally configurable firmware that handles New Zealand daylight saving correctly. It has since grown into a small timekeeping and network-diagnostics platform with browser OTA updates, NTP quality history, raw NTP probes, and MQTT telemetry.

> **Current release:** `v0.07`
>
> `v0.07` was promoted after an approximately 80-hour soak test of the MQTT, NTP, Wi-Fi, history, and memory-stability changes introduced during the `v0.05-dev` through `v0.07-dev` development cycle.

## Hardware target

- HU-058D clock PCB
- ESP-01S / ESP8266EX
- 1 MiB SPI flash
- 26 MHz crystal
- ESP -> display-controller link on GPIO2 / `Serial1`
- UART protocol: 9600 baud, 8N1

The display controller expects a 9-byte time frame once per second:

| Byte | Meaning |
| --- | --- |
| 0 | year - 1900 |
| 1 | month, 0-11 |
| 2 | day |
| 3 | hour, 0-23 |
| 4 | minute |
| 5 | second |
| 6 | weekday, Sunday = 0 |
| 7 | command |
| 8 | checksum: sum of bytes 0-7 modulo 256 |

Normal time frames use command `0x00`. Command `0x02` is used during startup while usable time is not yet available.

## Main features

### Clock and time

- NTP/SNTP time synchronisation
- configurable POSIX timezone rule
- correct NZST/NZDT handling by default
- configurable primary and fallback NTP servers
- manual NTP resynchronisation
- local time, UTC time and Unix epoch diagnostics
- `SYNCED`, `HOLDOVER`, and `UNSYNCED` time states
- time continues locally between NTP updates

Default New Zealand timezone rule:

```text
NZST-12NZDT,M9.5.0/2,M4.1.0/3
```

Default NTP servers:

```text
nz.pool.ntp.org
pool.ntp.org
time.cloudflare.com
```

### Web interface

The clock provides an English web UI for:

- status and diagnostics
- Wi-Fi configuration and scanning
- time zone and NTP configuration
- NTP history and graphs
- raw NTP diagnostics
- MQTT configuration and status
- system information
- browser-based OTA firmware updates
- manual Wi-Fi reconnect
- reboot, Wi-Fi reset, and factory reset

When connected to the normal LAN the clock also advertises:

```text
http://hu058d-clock.local/
```

mDNS availability depends on the local network. The clock can always be accessed directly by its IP address.

If saved Wi-Fi cannot be used, the firmware starts the setup access point:

```text
SSID:     HU058D-Setup
Password: hu058dclock
IP:       192.168.4.1
```

## NTP quality diagnostics

The firmware records information about successful SNTP updates rather than merely treating NTP as a black box that occasionally changes the clock.

Diagnostics include:

- successful sync count
- observed failed-poll count
- age of the most recent successful sync
- server hostname and resolved peer IP
- lwIP SNTP reachability register
- correction applied at each successful update
- elapsed interval since the previous successful update
- estimated oscillator drift in ppm
- weighted long-term drift estimate
- 48-sample in-RAM history

### Correction and oscillator drift

The ESP8266 SNTP interface does not expose the full four timestamps from its internal NTP transaction. Oscillator drift is therefore estimated by comparing the wall-clock time established at one successful update with monotonic elapsed time until the next update.

Sign convention:

- positive drift = the ESP clock was running fast
- negative drift = the ESP clock was running slow
- positive correction = SNTP moved the clock forward
- negative correction = SNTP moved the clock backward

Network delay and path asymmetry can contaminate individual drift samples. The value is therefore a useful diagnostic estimate, not a laboratory-grade oscillator measurement.

### Short resync filtering

Normal successful SNTP updates are approximately one hour apart, but retry or recovery events can occur sooner after network disruption or manual resynchronisation.

Short intervals are useful evidence that a correction occurred, but they are poor oscillator measurements because network timing error can dominate the tiny amount of clock drift accumulated over only a few minutes.

`v0.07` therefore keeps every correction in history but only treats intervals of at least **3000 seconds / 50 minutes** as valid drift samples.

For a short resync:

```json
{
  "correction_ms": -0.158,
  "ntp_interval_s": 60,
  "drift_valid": false,
  "drift_ppm": null
}
```

The weighted drift value is calculated only from qualified samples:

```text
drift_avg_ppm = -sum(correction) / sum(interval)
```

with the required unit conversion from milliseconds to ppm.

## NTP history

Up to 48 successful correction events are retained in RAM. At the normal hourly cadence this is roughly two days, although retry/recovery syncs can shorten the covered period.

Each history entry contains:

- Unix epoch
- correction in milliseconds
- sync interval
- drift in ppm when the interval qualifies
- `drift_valid`
- NTP server index/name
- resolved peer IP

The history is intentionally RAM-only and is cleared by a reboot.

Raw JSON is available at:

```text
/api/ntp-history
```

Example:

```bash
curl -s http://<clock-ip>/api/ntp-history | jq
```

## Raw NTP probe

The **NTP Probe** page performs a manual diagnostic four-timestamp NTP exchange without changing the system clock.

It reports information including:

- NTP offset
- network RTT
- server processing time
- stratum
- leap indicator
- protocol version and mode
- poll interval and precision
- root delay and dispersion
- reference ID and reference timestamp
- receive/transmit timestamps
- resolved peer IP

The probe uses its own UDP transaction and does **not** affect the SNTP clock discipline or NTP history.

This feature was also used to identify the large receive-side latency introduced by ESP8266 modem sleep.

## Wi-Fi sleep behaviour

Station-mode Wi-Fi is deliberately kept in:

```text
WIFI_NONE_SLEEP
```

The clock is USB powered, so reduced power consumption is less valuable than predictable network timing. Disabling modem sleep significantly reduced NTP receive latency and improved consistency during testing.

The configured sleep state is reasserted after association and reconnect events.

## MQTT telemetry

`v0.07` includes a lightweight MQTT 3.1.1 client implemented directly over `WiFiClient`. No external MQTT Arduino library is required.

Configuration is available from the **MQTT** page:

- enable/disable MQTT
- broker hostname or IP
- port, default `1883`
- optional username/password
- base topic, default `hu058d/clock`
- publish interval, 10-86400 seconds, default 60

### Topics

```text
<base>/availability
<base>/telemetry
```

`availability` is retained:

```text
online
```

The MQTT Last Will is retained as:

```text
offline
```

Telemetry is published QoS 0 as JSON.

Current telemetry fields include:

| Field | Description |
| --- | --- |
| `epoch` | current Unix epoch |
| `local_time` | formatted local time |
| `uptime_s` | ESP uptime |
| `rssi_dbm` | Wi-Fi RSSI |
| `free_heap` | current free heap |
| `ntp_state` | `SYNCED`, `HOLDOVER`, or `UNSYNCED` |
| `ntp_age_s` | age of most recent successful sync |
| `ntp_sync_count` | successful SNTP updates since boot |
| `ntp_failure_count` | observed failed polls |
| `history_count` | NTP history entries currently in RAM |
| `correction_ms` | most recent correction |
| `ntp_interval_s` | interval between the last two successful updates |
| `drift_valid` | whether the latest interval qualifies for oscillator drift |
| `drift_ppm` | latest qualified drift estimate, otherwise `null` |
| `drift_avg_ppm` | weighted drift across qualified in-RAM history |
| `drift_avg_samples` | number of samples used in the weighted estimate |
| `ntp_server` | most recent NTP hostname and peer IP |
| `wifi_connected_s` | age of the current Wi-Fi association |
| `wifi_sleep` | current ESP8266 Wi-Fi sleep mode |

Example:

```json
{
  "epoch": 1789855616,
  "local_time": "2026-09-20 10:06:56 NZST",
  "uptime_s": 75,
  "rssi_dbm": -63,
  "free_heap": 38696,
  "ntp_state": "SYNCED",
  "ntp_age_s": 4,
  "ntp_sync_count": 2,
  "ntp_failure_count": 0,
  "history_count": 1,
  "correction_ms": -0.158,
  "ntp_interval_s": 60,
  "drift_valid": false,
  "drift_ppm": null,
  "drift_avg_ppm": null,
  "drift_avg_samples": 0,
  "ntp_server": "nz.pool.ntp.org (202.124.96.215)",
  "wifi_connected_s": 65,
  "wifi_sleep": "NONE"
}
```

Watch all clock topics with Mosquitto:

```bash
mosquitto_sub -h <broker-ip> -v -t 'hu058d/clock/#'
```

Add `-u` and `-P` when broker authentication is enabled.

## JSON APIs

The browser interface uses local JSON endpoints that are also useful for scripts and external monitoring:

```text
/api/status
/api/ntp-history
/api/ntp-probe
```

Examples:

```bash
curl -s http://<clock-ip>/api/status | jq
curl -s http://<clock-ip>/api/ntp-history | jq
curl -s http://<clock-ip>/api/ntp-probe | jq
```

## Serial diagnostics

UART0 / GPIO1 provides event-oriented diagnostics at:

```text
115200 baud
```

A heartbeat is logged every 60 seconds, alongside event logs for Wi-Fi, NTP, MQTT, OTA, and system actions.

Example:

```text
[21:50:42] NTP   Sync #8 time=2026-09-19 21:50:42 NZST correction=-110.403 ms interval=01:00:00 drift=+30.667 ppm fast server=nz.pool.ntp.org (...)
[21:51:42] STAT  time=... wifi=Connected ... ntp=SYNCED ... mqtt=connected heap=39224B
```

Short resyncs are explicitly identified as filtered drift measurements.

## Persistent configuration

Configuration is stored using ESP8266 EEPROM emulation.

`v0.07` configuration format is version 3 and expands the emulated EEPROM area from 512 to 1024 bytes for MQTT settings.

Existing version 2 Wi-Fi/time settings are migrated automatically. MQTT defaults to disabled until configured.

## Building

No third-party Arduino libraries are required beyond the ESP8266 Arduino core.

Tested with ESP8266 Community core `3.1.2`.

Recommended Arduino settings:

```text
Board:            Generic ESP8266 Module
Flash Size:       1MB (FS:none OTA:~502KB)
CPU Frequency:    80 MHz
Flash Mode:       QIO
Flash Frequency:  80 MHz
Upload Speed:     115200
```

### Current v0.07 memory use

After MQTT telemetry and drift filtering:

```text
Global/static RAM:  33024 / 80192 bytes  (41%)
IRAM total:         61099 / 65536 bytes  (93%)
  actual IRAM code: 28331 bytes
  instruction cache:32768 bytes
Flash/IROM code:   380092 / 1048576 bytes (36%)
```

IRAM is the tightest resource and should be checked after every significant feature addition.

## OTA firmware updates

Once the custom firmware is installed and connected to Wi-Fi, subsequent builds can normally be installed from the **Firmware** page in the web UI.

In Arduino IDE use:

```text
Sketch -> Export Compiled Binary
```

and upload the generated `.ino.bin` file.

The `1MB (FS:none OTA:~502KB)` flash layout leaves room to stage an OTA image before rebooting into it.

Do not remove power while the bootloader is copying a successfully uploaded OTA image into place.

## Serial flashing and recovery

The ESP-01S ROM bootloader remains available even if an application build is broken.

To enter download mode, hold GPIO0 low while resetting/powering the ESP, then release GPIO0 after reset.

Typical serial restore/flash command:

```bash
esptool --port /dev/ttyUSBx --baud 115200 write-flash 0x000000 firmware.bin
```

Do not connect a USB-UART adapter's 5 V output directly to ESP8266 power or logic pins. The ESP8266 is a 3.3 V device.

Keeping a verified dump of the original factory flash before installing replacement firmware is strongly recommended.

## Version history

| Version | Main additions |
| --- | --- |
| `v0.02` | working replacement clock protocol, English web UI, Wi-Fi setup, NTP, automatic NZ DST, persistent configuration |
| `v0.03` | browser OTA, mDNS, Wi-Fi reconnect, expanded system/time diagnostics |
| `v0.04` | NTP quality diagnostics, correction/drift estimate, reachability, sync/failure counters, serial heartbeat |
| `v0.05-dev` | 48-entry RAM history, drift/correction graphs, `/api/ntp-history` |
| `v0.06-dev` | raw four-timestamp NTP probe, peer-IP history, no-sleep Wi-Fi timing improvement |
| `v0.07` | MQTT telemetry/availability, configuration migration, short-resync drift filtering, weighted drift, Wi-Fi reconnect timing context; promoted after extended soak testing |

## v0.07 release validation

The `v0.07` release candidate was soak-tested for approximately **79.6 continuous hours** before promotion.

During that run:

- ESP uptime reached approximately 3 days 7 hours 39 minutes with no reset
- Wi-Fi remained associated for effectively the entire run, with no observed reconnect event after startup
- MQTT produced 4,779 telemetry publishes at the configured one-minute cadence
- 81 successful SNTP updates were observed with **0 failed polls**
- the 48-entry NTP history ring filled and rolled over normally
- short resynchronisation filtering continued to exclude sub-3000-second intervals from oscillator-drift calculations
- free heap showed no downward trend or evidence of a memory leak
- long-term weighted oscillator drift remained close to **-4.9 ppm**

Individual hourly drift samples can still contain large positive or negative excursions because network delay and path asymmetry affect the apparent NTP correction. The weighted estimate is intentionally used to make the underlying oscillator behaviour easier to distinguish from those network effects.

The in-RAM history remains useful for local diagnostics, while MQTT allows external systems such as Gladys Assistant, InfluxDB, or another collector to retain longer-term telemetry across ESP reboots.

### External telemetry note

Consumers should use `drift_valid` when interpreting `drift_ppm`. When the latest successful NTP update occurred too soon to qualify as an oscillator measurement, the firmware publishes:

```json
{
  "drift_valid": false,
  "drift_ppm": null
}
```

Some external systems may coerce JSON `null` into numeric zero. A zero stored by such a consumer should therefore not be interpreted as a genuine 0 ppm oscillator measurement unless `drift_valid` is also true.

## Project goal

This project is intentionally more than a minimal clock replacement. The aim is to keep the HU-058D hardware useful while using it as a compact platform for learning about:

- ESP8266 firmware development
- reverse-engineering simple embedded protocols
- NTP/SNTP behaviour
- oscillator drift and network timing error
- MQTT telemetry
- constrained-memory embedded diagnostics

