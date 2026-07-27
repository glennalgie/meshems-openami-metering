# S5 — Leakage Insights Manager (implementation)

**Goal:** implement the per-EMS leakage insights + the **deterministic multi-EMS nomination** from the
S5 design brief, grounded in the literature (`S5_literature_findings.md`), and prove it end-to-end.

## What was built (code — compiles, proven on the ESP32)

`include/metering/leakage_insights_manager.h` + `src/metering/leakage_insights_manager.cpp`:

- **`LeakageInsightsManager`** — per-EMS state machine (`normal → watch → isolating → fault →
  restored`), builds this node's **telemetry**, tracks **onset** and **ramp**, and keeps a short
  **ramp signature** for correlation.
- **`nominate()`** — the deterministic round over *self + peers*:
  1. candidates = same phase + **correlated onset** (±5 s);
  2. rank by **magnitude** (`ac_mA`);
  3. **peer-correlation "odd one out"** — the node whose ramp signature correlates **least** with the
     others (Pearson), per the faulty-feeder-ID literature;
  4. **confidence** = rank1–rank2 gap, **boosted when magnitude leader == min-correlation node**, cut
     when they disagree, **de-rated under reverse power flow**;
  5. **act**: `conf ≥ 0.30` and quorum → **isolate** (bracket upstream+downstream); else **alert-only**.
- **`selfTelemetry / roundJson / alertJson`** — the three MQTT payloads (`…/leakage/telemetry/{ems_id}`,
  `…/leakage/isolation`, `…/leakage/alert`).

Wired into `poll_leakage()` behind `-DLEAKAGE_S5_DEMO`: a **3-EMS leak test** — this EMS (self, driven
by the mock ramp) plus two simulated peers (mid ≈ 0.4×, far ≈ 0.1×) with distinct signatures — runs a
round each cycle once in `watch`, and prints all three payloads.

## Evidence — 3-EMS leak test (serial, mock ramp rising)

```json
S5 telemetry: {"ems_id":"street-ems-01","state":"watch","ac_mA":16.45,"ramp_ac_mA_s":0.5,
               "onset_ts":33032,"powerflow":{"direction":"import"},"phase_angle_deg":null,...}
S5 isolation: {"nominated_ems":"street-ems-01","rule":"max_ac_mA + min_peer_corr, correlated_onset",
               "confidence":0.82,"action":"isolate_takeoff (bracket upstream+downstream)",
               "reason":"mag+corr agree","quorum":{"reporting":3}}
S5 alert    : {"severity":"warning","located_at":{"ems_id":"street-ems-01","phase":1},
               "ac_mA":16.45,"headroom_to_trip_mA":13.55,"classification":"leakage","confidence":0.82,...}
```

The nominated node is both the **magnitude leader** and the **min-correlation (odd-one-out)** node →
`mag+corr agree` → confidence **0.82** → **isolate**, quorum 3. As the ramp rises, `headroom_to_trip_mA`
shrinks (13.5 → 7.5 mA). The full pipeline runs on **one ESP32** with no broker/peers — proving the
S5 logic.

## Design notes
- **Change-based** trigger + onset correlation (per the literature) — not absolute level.
- **Odd-one-out** nomination is robust to EMS at different feeder positions (heterogeneous levels).
- **Bracketed isolation** (both sides) and **auto-vs-alert** gating mirror FLISR practice.
- `phase_angle_deg` carried but `null` (MD0630 absolute-value only) — the IVY-upgrade field for true
  direction isolation.
- The sub-300 ms life-safety trip stays on the module's **hardware** fault line — never here.

## S5 v2 — MQTT over the real network (proven 2026-07-27)
The ESP32 connects to WiFi (a phone hotspot) and to a **Mosquitto broker on the PC** (broker IP set in
the gitignored `secrets.h`; `config.h` now allows the override). The leakage flows end to end and the
PC subscriber receives it, fault included:
```
openami/StreetEMS_<id>/subpanel_RCMleaks
  {"acSinusoidal":{"value_mA":30.66,"threshold_mA":30,"inFault":true},
   "dc":{"value_mA":6.13,"threshold_mA":6,"inFault":true}, ...}
```
Note: a **WPA3 / PMF** phone hotspot blocked the ESP32 (older WiFi stack) — a **WPA2** hotspot worked.
Remaining v2 wiring: publish the S5 **telemetry / isolation / alert** on the shared `lvfeeder/…`
topics (currently the S5 payloads are built + proven on serial; the EMS leakage flows on
`subpanel_RCMleaks`), and subscribe to **real peers**.

## Status & next
- **S5 core complete** — insights + nomination + 3-EMS proof; compiles, verified on-device.
- Remaining integration (v2): **publish** telemetry/isolation/alert on the real MQTT topics from the
  MQTT loop, and **subscribe** to real peers' telemetry (replace the injected peers). Then a real
  multi-EMS bench (3 units) or a broker-side peer simulator.
- Then **S6**: hardware alarm-line GPIO + reverse-power-flow threshold (27 mA) integration.
