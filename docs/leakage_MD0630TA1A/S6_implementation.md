# S6 — Hardware Alarm Sense (A) + Reverse-Power-Flow Adaptive Threshold (B)

Closes the loop between the MD0630 and the EMS in both directions. Reuses S1 (register map),
S2 (live read), S3 (FC16 write + read-back), and feeds S5 (nomination confidence).

## (B) Reverse-power-flow adaptive threshold — 100 % firmware

- **Constant:** `Modbus_MD0630::AC_THRESHOLD_EXPORT_MA = 27.0f` (`include/metering/modbus_md0630.h`).
- **Detection:** export when `readings[0].active_power < -0.05f` (< −50 W). Bench override:
  `-DLEAKAGE_REVERSE_FLOW_DEMO` toggles export every 20 s.
- **Write discipline:** `update_reverse_flow_threshold()` in `src/metering/modbus_master.cpp` writes
  **only on a debounced transition** (3 poll cycles) — 27 mA on import→export, 30 mA on
  export→import — via the S3 `writeThreshold_mA(CH_AC, …)` FC16 + read-back path. Never writes every
  cycle (protects the module's threshold store).
- **S5 tie-in:** the live export flag is passed into `leakageInsights5.updateSelf(…, exporting_now,
  now)` so `nominate()` de-rates confidence under reverse flow.

## (A) Hardware alarm sense — read-only observer

- **Driver:** `include/metering/leakage_alarm_sense.h` (`LeakageAlarmSense`) — samples AC / DC / fault
  alarm outputs, reports **only on an edge**.
- **Pins (ESP32-S3-DevKitC-1):** `MD0630_ALARM_AC_PIN = GPIO39 (AO)`, `_DC_PIN = GPIO40 (DO)`,
  `_FAULT_PIN = GPIO21 (AD)` (`include/core/pins.h`). GPIO 33/34 are **not** broken out on the bare
  devkit (they were 865B-only) — 39/40/21 are free, non-strapping header pins. On a custom PCB, remap.
- **Polarity (measured 2026-07-27):** the real MD0630 drives AO/DO **LOW at rest, HIGH on alarm** →
  **active-high**, so `begin(..., active_low=false)` → `INPUT_PULLDOWN` (also keeps an unwired line
  such as AD reading inactive, not floating). Confirmed on the bench: after the fix the rest state
  reads `S6 ALARM edge: AC=0 DC=0 FAULT=0` (before it falsely read `AC=1 DC=1`).
- **Level shift (no multimeter needed):** one **4.7 kΩ resistor in series** per line into the GPIO.
  Safe for any output type — open-collector reads LOW when active; a push-pull 5 V high is clamped by
  the GPIO's internal ESD diode with the 4.7 kΩ limiting the current to ~0.36 mA. Use the module's
  **5 V header** outputs (not the 12 V cable), share **GND**, pull-up is **internal** (no 3V3/5V wire).
- **Safety:** ESP32 GPIO are **not** 5 V-tolerant. This path only **observes** the module's own trip;
  it never drives it.
- **Wired in** `poll_leakage()` → `S6 ALARM edge: AC=.. DC=.. FAULT=..` on each transition.

## Build flags (`platformio.ini`, both off by default)

- `-DLEAKAGE_REVERSE_FLOW_DEMO` — fake export toggle to exercise the 30↔27 mA write path.
- `-DLEAKAGE_ALARM_SENSE` — enable the alarm-line observer (needs the opto wiring).

## Validation

- Default build (both flags off): **SUCCESS**, no regression.
- `-DLEAKAGE_ALARM_SENSE -DLEAKAGE_REVERSE_FLOW_DEMO -DLEAKAGE_S5_DEMO`: **SUCCESS**.

## Bench runsheet (module on ESP32)

1. Flash `-DLEAKAGE_REVERSE_FLOW_DEMO` (module connected, `LEAKAGE_MOCK` off) → AC threshold switches
   30↔27 mA every ~20 s (serial + CT.exe read-back).
2. Flash `-DLEAKAGE_ALARM_SENSE` with the opto board → induce a leak > 30 mA → confirm
   `S6 ALARM edge: AC=1 …` as the module relay trips.

Report: `report/S6_report.pdf` (`report/S6_report.qmd`, `report/s6_architecture.py`).
