# S4 — Energy-change correlation cache

**Goal:** expand the leakage data model with a **ring of recent energy snapshots** so that, when the
leakage steps up/down, we can look back ±window and answer *"what energy event happened when the
leakage changed?"* (generate / store / consume / transform). This is the data foundation for the S5
Leakage Insights Manager.

## What was written (code — compiles, mock-exercised)

`include/metering/leakage_energy_correlation.h` (self-contained, reuses the `CurrentHistory`
circular-buffer pattern):

- **`EnergySample`** — one timestamped snapshot (V, I, active/reactive power, energy, ramp,
  `phase_angle_deg` reserved for leakage-direction). Built from `PowerData`; `toJson()` for MQTT.
- **`EnergyRing<N>`** — ring buffer of snapshots (N = `CURRENT_HISTORY_SIZE` = 128). `record()` also
  derives the active-power **ramp** from the previous sample; `lookback(ts, window_ms)` returns the
  snapshot closest in time within ±window.
- **`LeakageStep`** — a detected step (from/to mA, rising?, ts) with its **correlated `EnergySample`**;
  `toJson()`.
- **`LeakageInsightsCache`** — `update(mA, now, ring)` detects a step when leakage changes by
  ≥ `step_mA` (default 1 mA) and grabs the nearest energy event within ±`window_ms` (default 2 s).

### Integration (`src/metering/modbus_master.cpp`)
Globals `EnergyRing<128> energyRing` + `LeakageInsightsCache leakageInsights`. Each successful leakage
poll:
1. `energyRing.record(readings[0], now)` — snapshot the EMS energy (`readings[0]`; 0 on a bare bench,
   real once the ATM90E32/meters run).
2. `leakageInsights.update(ac_mA, now, energyRing)` — on a step, log it + the correlated energy:
   ```
   MD0630 LEAKAGE STEP UP: 28.61 -> 30.11 mA | energy corr: yes (V=… I=… P=… ramp=…)
   ```

The **mock ramp** (default build) drives the leakage up ~1.5 mA per cycle → steps fire → the
correlation path is exercised end to end without hardware.

## Design notes
- **Change-based**, not level-based (per the S5 literature lesson): we act on *steps*, matching the
  standing-residual reality.
- The ring is **generic** energy data → reusable for other insights (neutral, harmonics) later.
- `phase_angle_deg` is carried but 0 today (MD0630 is absolute-value only); it is the field the IVY
  RCM upgrade would fill for **leakage-direction** isolation (ties into S5/S6).

## Open / next
- [ ] Feed **real** energy once the meters run on the bench (readings[0] non-zero) and tune `step_mA`
      / `window_ms` against real ramps.
- [ ] **S5** — Leakage Insights Manager consumes `LeakageInsightsCache` + publishes correlated
      insights on the MQTT LV-feeder isolation subtopic.

## Status: **S4 code complete** — compiles, exercised by the mock ramp; awaiting real energy data to tune.
