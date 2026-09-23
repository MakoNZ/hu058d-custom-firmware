# HU-058D Capture Log

Use this file for concise, repeatable conclusions from hardware captures.

Large raw `.sr`/logic capture files should normally remain in the ignored
`captures/` directory. Commit small decoded exports only when they materially
support the documentation.

## Template

### YYYY-MM-DD — experiment name

**Board**

```text
Model:
PCB revision:
MCU marking:
ESP firmware:
```

**Capture**

```text
Raw file:
Analyser:
Sample rate:
Duration:
Channels:
Power:
```

**Stimulus**

Describe exactly what was changed or pressed and when.

**Observed**

Record measured voltages, decoded bytes, timing, repeated patterns, and other
direct observations. Keep observations separate from interpretation.

**Interpretation**

State the current hypothesis.

**Confidence**

One of:

```text
LOW
MEDIUM
HIGH
CONFIRMED
```

**Next test**

Describe the smallest experiment that could confirm or disprove the current
interpretation.

---

## Captures

_No hardware captures logged yet._
