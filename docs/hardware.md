# HU-058D Hardware Reverse Engineering

This document tracks the hardware side of the HU-058D custom-firmware project.

The aim is to separate **observed facts**, **family/reference information**, and
**working hypotheses** so that later logic-analyser captures and continuity
measurements refine the record instead of turning guesses into folklore.

## Evidence labels

The following labels are used throughout this document:

- **CONFIRMED** — measured, captured, or directly verified on the current HU-058D.
- **REFERENCE** — supported by documentation or reverse engineering of the closely
  related HU-058/HU-058D family, but not yet electrically verified on this board.
- **TO VERIFY** — plausible working hypothesis that still needs measurement.

## Current architecture

```text
                         HU-058D

                     +----------------+
                     |    ESP-01S     |
 Wi-Fi / NTP / HTTP  |    ESP8266     |
 MQTT / OTA          +----------------+
                       |            ^
             GPIO2 TX  |            | GPIO3 RX
             9600 8N1  |            | TO VERIFY
                       v            |
                  +----------------------+
                  |   display MCU / STC  |
                  |                      |
                  |  display scanning    |
                  |  buttons             |
                  |  LDR / thermistor    |
                  |  buzzer              |
                  +----------------------+
                    |      |        |
                    |      |        +---- sensors / controls
                    |      |
                    v      v
                AiP33628  AiP33628
                    \      /
                     \    /
                      RGB display
```

The exact STC and display-driver routing on the current Wi-Fi PCB will be
confirmed once the spare boards and logic analyser arrive.

## Board identification

### Current HU-058D Wi-Fi board

**CONFIRMED**

- PCB marking: `HU-058D V1.0 240516`
- ESP module: ESP-01S
- ESP silicon: ESP8266EX
- ESP crystal: 26 MHz
- ESP flash: 1 MiB
- ESP MAC observed during serial recovery: `5c:cf:7f:af:e5:32`
- ESP firmware-to-display link uses GPIO2 / `Serial1`
- known link speed: 9600 baud, 8 data bits, no parity, 1 stop bit

### Related HU-058 / HU-058D family

**REFERENCE**

The readily available HU-058 kit schematic identifies:

- U4: `STC8G1K17-38I-DIP16`
- U1/U2: two `AiP33628` LED-driver ICs
- U3: DS1302 RTC on the non-Wi-Fi HU-058 variant
- R3: GL5539 photoresistor
- R4: NTC thermistor
- Q1: S8550 buzzer transistor
- two push buttons
- 5 V main logic/display rail

The cheap non-Wi-Fi HU-058 board is particularly useful as a development
platform because the STC is supplied in a DIP-16 socket.

The Wi-Fi HU-058D omits/replaces the RTC time source with the ESP-01S/NTP
arrangement. Do not assume every HU-058 net or pin is identical to the HU-058D
until checked.

## ESP-01S connections

Looking at the back of the ESP-01S with the clock buttons toward the bottom:

```text
toward centre of clock

RX/GPIO3   GPIO0   GPIO2   GND
VCC        RST     EN      TX/GPIO1

toward buttons
```

### Known roles

| ESP pin | Current role | State |
| --- | --- | --- |
| GPIO0 | ROM download-mode strap | CONFIRMED |
| GPIO1 / TX | UART0 firmware diagnostics at 115200 baud | CONFIRMED |
| GPIO2 | ESP -> display MCU time/control stream at 9600 baud | CONFIRMED |
| GPIO3 / RX | possible display MCU -> ESP stream | TO VERIFY |
| RST | ESP reset | CONFIRMED |
| EN | ESP enable | CONFIRMED |

The custom firmware deliberately keeps GPIO2 compatible with the original
display-controller protocol.

## ESP -> display-controller protocol

**CONFIRMED**

The normal time frame is nine bytes:

| Byte | Meaning |
| ---: | --- |
| 0 | year - 1900 |
| 1 | month, 0-11 |
| 2 | day |
| 3 | hour, 0-23 |
| 4 | minute |
| 5 | second |
| 6 | weekday, Sunday = 0 |
| 7 | command |
| 8 | checksum: sum of bytes 0-7 modulo 256 |

Normal frames use command `0x00`.

A startup/control frame has been observed as:

```text
00 00 00 00 00 00 00 02 02
```

Command `0x02` is repeatedly sent during startup before usable time is
available.

Example normal frame:

```text
7e 08 0d 11 00 01 00 00 a5
```

decodes as:

```text
2026-09-13 17:00:01, Sunday
```

### Boot behaviour already observed

**CONFIRMED**

A previous UART capture showed this broad sequence:

1. valid time frames before reset;
2. reset/boot garbage around the GPIO2 transition;
3. command `0x02` frames repeated several times;
4. an early frame before NTP has established correct current time;
5. normal local-time frames once NTP synchronisation completes;
6. one normal frame per second thereafter.

A possible command `0x01` was noticed during factory-firmware static analysis,
but its meaning has not been confirmed.

## Possible display MCU -> ESP channel

### ESP GPIO3 / UART0 RX

**TO VERIFY**

The original firmware contains evidence suggesting the display controller may
be capable of sending information back to the ESP. GPIO3 is therefore a
priority capture target.

Questions:

- Is there continuous traffic?
- Is traffic generated only by button presses?
- Are LDR or thermistor readings reported?
- Is a command/request from the ESP required before the STC replies?
- Does the factory firmware receive configuration/reset requests here?

A test firmware branch can later leave GPIO3 as a receive-only sniffer while
GPIO1 continues carrying serial diagnostics.

## P3 header

The HU-058D board has a separate four-pin header marked:

```text
RXD  TXD  GND  VCC
```

### Measurements already made

**CONFIRMED**

With the clock powered normally:

- `VCC` measured about 4.5 V
- idle `TXD` measured about 4.45 V

This strongly suggests that P3 belongs to the 5 V side of the design.

### Purpose

**TO VERIFY**

The STC8 family supports serial ISP and its primary UART uses P3.0/RXD and
P3.1/TXD. This makes P3 an obvious candidate for STC programming, factory test,
or debug access.

However, the HU-058 reference schematic also uses P3.0/P3.1 for its two
buttons. Therefore the actual HU-058D P3 routing must be confirmed by
continuity testing and capture before assuming that the header goes directly to
the STC primary UART.

Do not connect an analyser or USB-UART adapter to P3 until its logic level and
the analyser/adapter input tolerance have both been verified.

## Sensors

### Reference schematic

**REFERENCE**

The HU-058 schematic shows two 10 kΩ dividers feeding STC ADC inputs:

```text
+5V -- 10k --+-- LDR -- GND
             |
             +---- R_P -> P1.7 / ADC7

+5V -- 10k --+-- NTC -- GND
             |
             +---- R_T -> P1.6 / ADC6
```

The exact orientation/naming should be confirmed directly against the Wi-Fi
HU-058D PCB before relying on it in replacement firmware.

### Planned checks

1. Power off the clock.
2. Continuity-test each sensor-divider midpoint to the display MCU pins.
3. Power the board normally.
4. Measure the LDR divider while:
   - exposed to room light;
   - covered completely;
   - illuminated brightly.
5. Measure the NTC divider while:
   - at room temperature;
   - gently warmed by a finger;
   - allowed to cool.
6. Capture GPIO3 and P3 simultaneously during those changes.
7. Look for values or events correlated with the analogue measurements.

If the stock STC already reports the values to the ESP, no replacement MCU is
needed merely to obtain sensor telemetry.

## Display subsystem

**REFERENCE**

The HU-058 family documentation identifies two AiP33628 common-anode
constant-current LED-driver ICs.

AiP33628 characteristics relevant to this project include:

- 3.0-5.5 V supply range
- 8 common-anode outputs
- 16 constant-current cathode outputs
- two-wire serial interface
- programmable current levels

Independent reverse engineering of the HU-058D family reports that the STC
performs the display scan and drives two separate AiP33628 buses. That work also
documents the unusual latch behaviour and the LED/segment map.

We will still capture our own board before implementing replacement STC
firmware. The external work is a useful reference, not a substitute for
confirming the exact PCB revision in front of us.

## Replacement STC development

The planned development hardware is:

```text
spare HU-058 kit
    +
socketed factory STC retained untouched
    +
replacement STC8G1K17-38I-DIP16
    +
USB-UART adapter
    +
logic analyser
```

The factory STC should be preserved. Do not erase it unless a verified readable
backup exists.

Suggested replacement-firmware bring-up order:

1. establish STC ISP detection with a spare MCU;
2. flash a minimal program that toggles an output or emits UART text;
3. read LDR and NTC ADC values;
4. read both buttons;
5. drive the buzzer;
6. drive a single known display element;
7. reproduce normal four-digit display scanning;
8. add RGB colour/brightness control;
9. implement an ESP <-> STC command protocol;
10. add alarms, display effects, sensor reporting, and button events.

This keeps display timing on the STC while the ESP remains responsible for
networking, NTP, web configuration, MQTT, and OTA.

## Candidate future ESP <-> STC protocol

This is intentionally only a design sketch.

```text
ESP -> STC
  SET_TIME
  SET_BRIGHTNESS
  SET_COLOUR
  SET_DISPLAY_MODE
  DISPLAY_VALUE
  SET_ALARM
  SENSOR_REQUEST

STC -> ESP
  SENSOR_VALUES
  BUTTON_EVENT
  ALARM_STATE
  DISPLAY_STATE
  ERROR/STATUS
```

The existing nine-byte time frame should remain supported during early
development so the replacement STC can be tested against the current v0.07 ESP
firmware before introducing a richer protocol.

## Open questions

- [ ] Confirm the exact MCU marking on the current HU-058D.
- [ ] Map ESP GPIO3 electrically to the display MCU.
- [ ] Determine whether GPIO3 carries runtime traffic.
- [ ] Map all four P3 pins to MCU pins/nets.
- [ ] Determine P3 logic voltage and idle polarity precisely.
- [ ] Test whether P3 exposes STC ISP.
- [ ] Confirm LDR divider midpoint and MCU ADC pin.
- [ ] Confirm thermistor divider midpoint and MCU ADC pin.
- [ ] Determine whether sensor values are sent to the ESP.
- [ ] Identify both AiP33628 DATA/CLK buses on the HU-058D.
- [ ] Capture display traffic during normal time display.
- [ ] Capture display traffic during colour/brightness/style changes.
- [ ] Map buttons and buzzer on the Wi-Fi PCB.
- [ ] Preserve an untouched factory STC from the spare HU-058 board.
- [ ] Bring up custom firmware on a replacement STC.

## References

- STC8G series datasheet:
  <https://www.stcmicro.com/datasheet/STC8G-en.pdf>
- HU-058 kit manual/schematic:
  <https://m.media-amazon.com/images/I/A1RMbuddasL.pdf>
- AiP33628 datasheet:
  <https://dfsimg1.hqewimg.com/group5/M00/03/7F/wKhk3mMFnkaAaBT8ABBg9oPRpo8519.pdf>
- Independent HU-058/HU-058D display reverse engineering:
  <https://imtaqin.id/misterblack1-hu-058-esphome-open-firmware-for-the-hu-058d-wifi-clock>
