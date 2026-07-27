# S6 — Bench evidence (raw serial captures)

Permanent record of the on-hardware S6 verification, so the report can be written/polished
**offline** (no ESP32 needed). Captured 2026-07-27.

- **Board:** ESP32-S3-DevKitC-1 on **COM4**, USB-powered.
- **Module:** IVY MD0630 (real), 12 V-powered, on the RS-485/Modbus bus (leakage in MOCK for these
  runs so the loop runs; the **alarm lines are the real module outputs**).
- **Wiring (S6-A):** `AO → 4.7 kΩ → GPIO39`, `DO → 4.7 kΩ → GPIO40`, `AD` left free (GPIO21),
  **common GND** module↔ESP32. Single 4.7 kΩ **series** resistor per line (no 3V3/5V rail wired;
  internal pull used). Module 5 V header outputs.
- **Build for the alarm test:** `-DLEAKAGE_ALARM_SENSE` (WiFi/MQTT off so the loop doesn't block:
  `$env:PLATFORMIO_BUILD_FLAGS="-UENABLE_WIFI -UENABLE_MQTT -DLEAKAGE_ALARM_SENSE"`).

## Build validation

| Configuration | Result |
|---|---|
| Default (S6 flags off) | SUCCESS — RAM 18.8 %, Flash 27.6 % |
| `-DLEAKAGE_ALARM_SENSE -DLEAKAGE_REVERSE_FLOW_DEMO -DLEAKAGE_S5_DEMO` | SUCCESS — RAM 18.8 %, Flash 27.9 % |

## Capture 1 — polarity discovery (first flash, assumed active-low → WRONG)

```text
SETUP: MODBUS: MD0630 alarm sense on GPIO 39/40/21 (AC/DC/fault, active-low)
SETUP: MODBUS: MD0630 in MOCK RAMP mode — no hardware required
MD0630 [MOCK RAMP]: AC=1.44 mA  DC=0.29 mA  (ok:1)
S6 ALARM edge: AC=1 DC=1 FAULT=0          # <-- at rest, no leakage: FALSE alarm
```

`AC=1 DC=1` at rest = the AO/DO outputs sit **LOW at rest** and the active-low logic misread that as
"active". Conclusion: the MD0630 alarm outputs are **ACTIVE-HIGH** (LOW at rest, HIGH on alarm).

## Capture 2 — after the fix (active-high, INPUT_PULLDOWN)

```text
SETUP: MODBUS: MD0630 alarm sense on GPIO 39/40/21 (AC/DC/fault, active-high)
S6 ALARM edge: AC=0 DC=0 FAULT=0          # <-- at rest: correctly quiet
```

FAULT=0 throughout (AD unwired, held inactive by the internal pull-down).

## Capture 3 — live AC alarm (3V3 pulse on GPIO39, standing in for a real trip)

With alarm sampling moved to `service_leakage_alarms()` at ~50 Hz (decoupled from the slow Modbus
poll), each edge is caught in < 200 ms:

```text
[  9.7] S6 ALARM edge: AC=1 DC=0 FAULT=0   # AO asserted (contact)
[  9.9] S6 ALARM edge: AC=0 DC=0 FAULT=0   # AO released
[ 10.1] S6 ALARM edge: AC=1 DC=0 FAULT=0
[ 10.1] S6 ALARM edge: AC=0 DC=0 FAULT=0
[ 10.2] S6 ALARM edge: AC=1 DC=0 FAULT=0
[ 10.3] S6 ALARM edge: AC=0 DC=0 FAULT=0
```

`DC`/`FAULT` correctly stay 0 (only the AC line was pulsed). **End-to-end S6-A proof: module alarm
output → 4.7 kΩ → GPIO → firmware logs `AC=1`.**

## What is NOT yet captured (next bench session)

- **S6-B** (reverse-power-flow threshold): flash `-DLEAKAGE_REVERSE_FLOW_DEMO` with **MOCK off** and
  the module answering on Modbus (RX/TX) → capture the AC threshold switching **30 → 27 → 30 mA**
  (serial + CT.exe read-back of register 0x0003).
- **S6-A real trip**: induce a real >30 mA residual current (or N turns through the CT) so the module
  itself asserts AO → same `AC=1` line, but from a genuine trip.

## Commits

- `a4212fd` — S6 firmware (A + B)
- `3258168` — pins moved to GPIO 39/40/21 (DevKitC-1)
- `ddbd270` — polarity fixed to active-high (from Capture 1)
- `aaca0b6` — fast ~50 Hz decoupled alarm sampling + live proof (Capture 3)
