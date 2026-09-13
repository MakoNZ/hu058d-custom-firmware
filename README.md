# HU-058D Custom Firmware

Custom ESP8266 firmware for the HU-058D NTP clock PCB.

This project replaces the original ESP-01S firmware while retaining the
factory STC microcontroller firmware responsible for the clock display and
other front-panel functions.

## Current status

Version v0.02 is running successfully on real hardware.

Working features:

- English web interface
- Wi-Fi configuration and network scanning
- fallback setup access point
- NTP time synchronisation
- automatic NZST / NZDT daylight-saving handling
- configurable timezone
- configurable NTP servers
- persistent configuration
- web interface remains available while connected to normal Wi-Fi
- manual NTP synchronisation
- reboot, Wi-Fi reset and factory-reset controls
- status and diagnostic information

## Hardware

PCB:

    HU-058D V1.0 240516

Wi-Fi module:

    ESP-01S
    ESP8266EX
    1 MiB SPI flash

The original clock/display controller is retained.

## ESP to clock protocol

The ESP8266 communicates with the factory STC controller using:

    ESP8266 GPIO2 / UART1 TX
    9600 baud
    8N1
    one 9-byte packet per second

Time packet:

    Byte 0  Year since 1900
    Byte 1  Month, 0-11
    Byte 2  Day, 1-31
    Byte 3  Hour, 0-23
    Byte 4  Minute, 0-59
    Byte 5  Second, 0-59
    Byte 6  Weekday, Sunday = 0
    Byte 7  Command, 0 = normal time
    Byte 8  8-bit additive checksum of bytes 0-7

A startup/waiting packet used by the factory firmware was also identified:

    00 00 00 00 00 00 00 02 02

## Default timezone

The firmware defaults to New Zealand time using:

    NZST-12NZDT,M9.5.0/2,M4.1.0/3

This automatically switches between NZST (UTC+12) and NZDT (UTC+13).

## First-time setup

When no valid Wi-Fi configuration exists, the clock starts:

    SSID: HU058D-Setup
    Password: hu058dclock

Connect to the access point and browse to:

    http://192.168.4.1/

After Wi-Fi is configured, the setup AP shuts down and the web interface
remains accessible at the clock's DHCP address.

## Building

Arduino IDE with the ESP8266 Community board package.

Recommended settings:

    Board:            Generic ESP8266 Module
    Flash Size:       1MB
    CPU Frequency:    80 MHz
    Flash Mode:       QIO
    Flash Frequency:  80 MHz
    Upload Speed:     115200

## Programming

CP2102 wiring:

    CP2102 BLACK  -> ESP GND
    CP2102 GREY   -> ESP TX / GPIO1
    CP2102 WHITE  -> ESP RX / GPIO3
    CP2102 RED    -> NOT CONNECTED

Power the clock normally from its own USB input.

To enter the ESP8266 serial bootloader:

1. Connect GPIO0 to GND.
2. Reset or power-cycle the clock.
3. Upload the firmware.
4. Disconnect GPIO0 from GND.
5. Power-cycle normally.

## Versions

### v0.02

First fully usable release.

- English configuration UI
- persistent Wi-Fi configuration
- NTP synchronisation
- automatic NZST/NZDT
- configurable NTP servers and timezone
- fallback setup AP
- system/status pages

### v0.01

Proof-of-concept replacement firmware.

- hard-coded Wi-Fi credentials
- NTP synchronisation
- automatic NZST/NZDT
- proved the reverse-engineered ESP-to-STC protocol

## License

No license has been selected yet.
