# HU-058D Logic Analyser Plan

This document defines the first capture campaign for the HU-058D once the
8-channel USB logic analyser arrives.

The goal is to make captures repeatable. Randomly clipping eight colourful
wires onto a live PCB and then wondering what happened is technically also
reverse engineering, but only in the archaeological sense.

## Safety first

- Connect analyser **GND first**.
- Do not connect the analyser's VCC/power pin to the clock.
- Power the clock through its normal USB input.
- Verify the analyser input-voltage tolerance before probing the approximately
  5 V STC/display side.
- The ESP-01S side is 3.3 V logic.
- Do not assume P3 is 3.3 V; previous measurements showed approximately 4.5 V.
- Avoid attaching hooks while the board is powered when adjacent pins can be
  shorted.
- Photograph every probe setup before capturing. Wire colours are not evidence.

## Initial analyser checkout

After plugging the analyser into Nitro:

```bash
lsusb
```

Install/use sigrok and PulseView:

```bash
sudo pacman -S sigrok-cli pulseview
```

Then check device discovery:

```bash
sigrok-cli --scan
```

Record the exact USB identification and sigrok driver in the capture log.

Do not assume `/dev/ttyUSBx` numbering is relevant to the logic analyser; most
FX2 analysers are accessed through libusb rather than as a serial port.

## Sampling strategy

For the known 9600-baud UART, 1 MHz sampling is already generous:

```text
9600 baud bit time ~= 104.2 us
1 MHz sample time  =   1.0 us
~104 samples/bit
```

Suggested starting rates:

| Target | Initial sample rate | Notes |
| --- | ---: | --- |
| ESP <-> STC UART | 1 MHz | ample for 9600 baud |
| P3 unknown UART | 2 MHz | enough for common MCU UART rates |
| AiP33628 display buses | 8-12 MHz | increase only if edges/timing require it |
| maximum exploratory capture | 24 MHz | use shorter captures; USB/data volume rises quickly |

Use the slowest rate that clearly resolves the signal. Long captures at 24 MHz
mostly produce heroic quantities of evidence about nothing.

## Channel naming

### Phase 1: serial/control capture

Start with only the serial/control lines:

| Channel | Signal | State |
| --- | --- | --- |
| D0 | ESP GPIO2 -> STC | known 9600-baud stream |
| D1 | ESP GPIO3 <- STC | suspected return path |
| D2 | P3 RXD | purpose unknown |
| D3 | P3 TXD | purpose unknown |
| D4 | spare / button | optional |
| D5 | spare / button | optional |
| D6 | spare | optional |
| D7 | spare | optional |

### Phase 2: display-bus capture

After identifying the AiP33628 pins/nets:

| Channel | Signal |
| --- | --- |
| D0 | display DATA |
| D1 | display CLK |
| D2 | display DATA_1 |
| D3 | display CLK_1 |
| D4 | ESP GPIO2 |
| D5 | ESP GPIO3 |
| D6 | button/event reference |
| D7 | spare |

The exact display-channel assignment stays provisional until continuity testing
maps the HU-058D revision.

## PulseView UART decoder

For ESP GPIO2 use:

```text
Protocol: UART
Baud:     9600
Data:     8 bits
Parity:   none
Stop:     1
Bit order: LSB first
Polarity: normal / idle high
```

A valid one-second time frame should appear as nine bytes with the final byte
equal to:

```text
sum(bytes 0..7) & 0xff
```

Seeing valid checksums is a useful way to confirm that the sample rate, channel,
polarity, and UART settings are correct.

## Capture naming

Keep raw captures outside Git by default:

```text
captures/
```

Suggested filename format:

```text
YYYYMMDD-HHMMSS_<board>_<experiment>.<ext>
```

Examples:

```text
20261005-191500_hu058d_normal-runtime.sr
20261005-192200_hu058d_cold-boot.sr
20261005-193000_hu058d_ldr-cover-uncover.sr
20261005-194500_hu058d_button-top.sr
20261005-201000_hu058-spare_display-scan.sr
```

Track conclusions and small decoded exports in Git; do not automatically commit
large raw captures.

## Capture campaign

### Capture 1 — normal runtime baseline

Purpose:

- verify D0 decoding;
- establish whether D1/P3 are active during ordinary operation.

Setup:

```text
D0 ESP GPIO2
D1 ESP GPIO3
D2 P3 RXD
D3 P3 TXD
```

Capture:

- 30 seconds
- clock already booted and NTP-synchronised
- no buttons pressed
- stable room lighting

Record:

- activity/idle state on each channel;
- observed baud rates if any;
- repeated message lengths/patterns;
- whether D1 is silent.

### Capture 2 — complete cold boot

Purpose:

- capture ESP/STC startup sequencing;
- identify P3 boot/ISP/debug activity;
- compare against the previously observed command `0x02` startup frames.

Procedure:

1. start capture;
2. remove clock power for several seconds;
3. restore power;
4. continue recording through Wi-Fi association and NTP sync;
5. stop after normal once-per-second frames are established.

Use a longer capture at a modest sample rate rather than 24 MHz.

### Capture 3 — top button

Purpose:

- find button-generated traffic from STC to ESP/P3;
- determine press/release/hold behaviour.

Capture separate events:

- short press;
- two quick presses;
- ~2 second hold;
- release.

Do not combine both buttons in the first capture.

### Capture 4 — bottom button

Repeat the top-button procedure independently.

### Capture 5 — LDR response

Purpose:

- determine whether light readings/events reach ESP or P3.

Keep the clock otherwise idle and perform a deliberate sequence:

```text
10 s normal room light
10 s completely covered
10 s normal light
10 s bright illumination
10 s normal light
```

At the same time measure the LDR divider voltage with a multimeter if practical.

The stepped sequence makes correlations much easier to recognise in a byte
stream.

### Capture 6 — thermistor response

Purpose:

- determine whether temperature readings/events reach ESP or P3.

Procedure:

```text
baseline at room temperature
gently warm thermistor with finger
hold warm
release
allow to cool
```

Record divider voltage/temperature estimate alongside timestamps.

Avoid heating it aggressively. We are characterising an alarm clock, not
qualifying aerospace components.

### Capture 7 — P3 boot investigation

Purpose:

- determine whether P3 behaves as STC UART/ISP/debug.

Capture P3 RXD/TXD during repeated cold boots before attaching a USB-UART
adapter.

Questions:

- Does TXD emit a boot signature?
- Does RXD remain idle?
- Are voltage levels compatible with the intended adapter?
- Does activity line up with the STC reset/power-up rather than ESP boot?

Only after passive capture should we attempt active ISP detection.

### Capture 8 — display scan baseline

Purpose:

- identify both AiP33628 buses;
- establish scan rate and frame shape.

Capture the four candidate display signals while showing a stable time.

Then repeat with a deliberately distinctive display state if the factory UI
allows it, for example a different colour/brightness/style.

### Capture 9 — display-difference captures

Change one thing at a time:

1. brightness only;
2. colour only;
3. one display style/mode;
4. colon state;
5. alarm indicator if available.

Single-variable changes are vastly easier to reverse engineer than a capture
where the time, brightness, colour, colon, alarm and buttons all change at once.

## Capture log

For every experiment record:

```text
date/time:
board:
PCB revision:
firmware:
analyser:
sample rate:
channels:
power source:
clock state:
experiment:
expected event:
observed event:
capture filename:
decoded export:
notes:
conclusion:
confidence:
```

Use `docs/capture-log.md` for conclusions and metadata.

## Analysis workflow

A useful progression for each unknown signal is:

```text
electrical idle/activity
        |
        v
measure edge timing / likely baud or clock
        |
        v
apply protocol decoder if appropriate
        |
        v
look for repeated frame boundaries
        |
        v
change exactly one physical input
        |
        v
compare captures
        |
        v
form hypothesis
        |
        v
repeat experiment designed to falsify it
```

A hypothesis becomes `CONFIRMED` in `docs/hardware.md` only after the evidence
is repeatable.

## Future tooling

Once the first captures exist, useful additions may include:

- a small Python decoder for the known 9-byte ESP -> STC frames;
- a script to validate checksums and render timestamps;
- automatic before/after comparison of exported logic data;
- a custom sigrok protocol decoder if the extended STC protocol becomes rich
  enough to justify one;
- AiP33628 frame decoding once the exact bus format on this revision is
  confirmed.

Do not write these before we have captures. Premature protocol-decoder
development is an efficient way to automate the wrong assumption.
