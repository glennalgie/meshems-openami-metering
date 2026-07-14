# DESIGN SPEC — Continuous Leakage Detection (IVY MD0630 RCM CT)

**Author:** Allahaddje Bonheur - IoT Fellow, 10Power (in collaboration with NESL)
**Reviewer:** Glenn (supervisor)
**Status:** ANALYSIS / DESIGN - for review **before** coding (per Glenn's directive)
**Module:** IVY Metering **MD0630T41A-1** (Type-B AC+DC leakage / RCM CT)
**Date:** _(fill in)_

> Glenn's directive: *"AI ingest the IVY Metering files, have it first just ANALYZE and REPORT in a
> design spec we review, THEN move to code. Get RS485/Modbus talking PC→IVY RCM CT, derive the
> register suite, expand the energy data model to hold continuous leakage, watch for reverse power
> flow and alert."*

---

## 0. Mission (from Glenn)

1. Get RS-485 / Modbus serial talking, **first PC → IVY RCM CT**, then ESP32 for production.
2. **Derive the Modbus register map** (7 registers, 3 writable) — by sniffing / from docs / ask IVY.
3. Write **config-time** threshold registers (DC/AC), always **read-back to confirm**.
4. **Read** DC & AC leakage current regularly into the **energy data model** (continuous leakage).
5. **Leakage Insights Manager** that correlates leakage steps with an **energy-change cache**.
6. Reverse-power-flow (export) → **lower AC threshold to 27 mA** (Achim/Gismo field study).
7. Follow a **repeatable Insights-Manager framework** (Neutral insights, Leakage insights, …).

## 1. Module — confirmed facts (from the IVY documents analyzed)

| Item                   | Value                                                                                                                                                                                                                                                                        |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Model                  | **MD0630T41A-1**, Type-B AC+DC RCM CT, IEC62955 / TUV / CE-EMC                                                                                                                                                                                                         |
| **Power supply** | **12 V ±20 % (~12 mA)** ⚠️ (NOT 5 V — different from the SHT20)                                                                                                                                                                                                    |
| Measuring range        | DC**2–15 mA**, AC **3–100 mA**                                                                                                                                                                                                                                 |
| Default thresholds     | DC**6 mA**, AC **30 mA**                                                                                                                                                                                                                                         |
| Accuracy               | DC ±0.4 mA, AC ±0.4 mA @25 °C (5 min warm-up recommended)                                                                                                                                                                                                                 |
| Measurement            | **absolute value, no current-direction polarity**                                                                                                                                                                                                                      |
| Comms                  | **Modbus RTU** over UART, **5 V logic** (datasheet p.2: "UART output 5 V") — TX/RX need pull-ups. On the **7-pin SIP header only** (the 6-wire cable has no TX/RX).                                                                                       |
| Connector to use       | **7-pin SIP header** = Vcc, GND, RX, TX, AD, AO, DO → has the Modbus **TX/RX**. The **6-wire cable** (red/blue/yellow/green/white+black) is **power + alarm outputs only, no Modbus**. → For S1/S2 use the **7-pin header** (VCC/GND/TX/RX). |
| Hardware alarm outputs | **7-pin header:** AD/AO/DO @ **5 V** digital. **6-wire cable:** AO(yellow)/DO(green) @ **12 V** + AD(white+black) isolated opto switch (4 kVac, closes on fault). ESP32 GPIO will need a level-shift.                                                |
| Pinout (wire colours)  | Red=12 V, Blue=GND, Yellow=AC-alarm, Green=DC-alarm, White+Black=opto out (+ ribbon TX/RX on the`-1` cable variant)                                                                                                                                                        |

## 2. Register suite (derived from `CT.db` `SaveData` table + PC manual)

The IVY PC tool (`CT.exe`) stores exactly these 7 fields → the module's 7 registers:

| # | Field (from CT.db)   | R/W           | Meaning                  | Default         |
| - | -------------------- | ------------- | ------------------------ | --------------- |
| 1 | `ModbusID`         | **W**   | Modbus slave/node number | —              |
| 2 | `Version`          | R             | firmware version         | —              |
| 3 | `SmallVersion`     | R             | firmware minor version   | —              |
| 4 | `DCThreshold`      | **W/R** | DC alarm threshold (mA)  | **6 mA**  |
| 5 | `ACThreshold`      | **W/R** | AC alarm threshold (mA)  | **30 mA** |
| 6 | `DCLeakageCurrent` | R             | measured DC leakage (mA) | —              |
| 7 | `ACLeakageCurrent` | R             | measured AC leakage (mA) | —              |

> **STILL TO DERIVE:** the exact Modbus **register offsets**, **function code** (0x03 holding / 0x04 input),
> and **scaling** (raw → mA). Not in the datasheet. Plan: **sniff the CT.exe ↔ module serial frames**
> (Phase 0b) and/or ask IVY. The CT.db field order above is the most likely register order.

## 3. Bring-up plan — PC first, then ESP32 (per Glenn)

- **Phase 0 — PC bring-up:** power module at **12 V**; connect via USB↔serial; run `CT.exe`
  (COM port, baud from `softconfig.ini`). Confirm comms: read ModbusID / version / thresholds /
  leakage. Set a threshold, **read back** to confirm.
- **Phase 0b — Sniff & derive register map:** capture the serial frames while `CT.exe` reads/writes
  (serial sniffer). Record: slave addr, baud, function code, register offsets, scaling. → **This is the
  output that unblocks the driver.**
- **Phase 1 — ESP32 production:** connect the module to the **RS-485 bus** via the RS-485↔TTL module
  (3.3 V TTL → RS-485; **termination-resistor solder pad left open** per Glenn). Implement the
  `Modbus_MD0630` driver with the derived register map.

## 4. Modbus node-numbering scheme (from Glenn)

- Leakage CT: **node 100** (single-phase) or **100 / 101 / 102** (3-phase, one CT per phase).
- Cabinet temp/humidity sensor: node **99** (or 1).
- The node number is a **writable register** → set during EMS commissioning (future commissioning stack).

## 5. Config-time writes (at setup only)

- Write **DC threshold = 6 mA**, **AC threshold = 30 mA** (life-safety defaults) at boot — the model
  currently defaults everything to 30 mA, so DC must be corrected to 6 mA.
- **Unlock → write → read-back → confirm** (ack if the caller needs it).
- **Reverse-power-flow adaptation:** when the EMS measures export (reverse flow), **lower AC threshold
  to 27 mA** (Achim/Gismo field study). Re-raise to 30 mA when back to import.

## 6. Read cycle (continuous leakage)

- Read **DCLeakageCurrent** + **ACLeakageCurrent** at a configurable sense rate.
- **Double-read to confirm steady** before committing a step change into the model.
- Store into the (expanded) energy data model — see §7.

## 7. Energy data model expansion (continuous leakage + correlation)

Today `LeakageData` in `include/core/data_model.h` is an empty stub. Proposed expansion:

- **Continuous leakage fields:** AC/DC value_mA, thresholds, `inFault`, and **step-up / step-down
  timestamps** (when leakage crossed a step). Reuse / wrap the existing `LeakageModel`
  (`leakage_model_ivy41a.h`).
- **Energy-change correlation cache:** a circular buffer (reuse the `CurrentHistory` pattern, 128 slots)
  of **timestamped energy events** — V, I, active/reactive power, energy, phase angle, and ramp
  up/down over a short window. When a leakage step is detected, we can look back ±X ms and answer
  *"what energy event happened when the leakage changed?"* (generate/store/consume/transform).

## 8. Leakage Insights Manager (overwatch) + repeatable framework

- A **separate manager** that overwatches the leakage stream and **correlates** each leakage step with
  the energy-change cache (§7).
- Design it as one instance of a **repeatable Insights-Manager framework** (common base
  interface): `NeutralInsights`, `LeakageInsights`, `…Insights`. **Reuse the DTM-EMS neutral-balancer
  insights manager** pattern (Glenn to share the files to ingest/review).

## 9. MQTT / LV-feeder coordination

- Continuous leakage keeps publishing on `subpanel_RCMleaks` (already implemented).
- Add **leakage alerts + call-to-action** on an **LV-feeder coordination subtopic** so EMS nodes
  can help **isolate leakage to a phase and a feeder connection point**; same channel carries
  neutral-unbalance alerts and phase-rebalance requests.

## 10. Hardware alarm path (fast, autonomous)

- The module's **3 alarm outputs (AC / DC / AC+DC) are 12 V, 100 mA digital** → they must be
  **level-shifted to 3.3 V** (divider or opto) before reaching ESP32 GPIO inputs. ⚠️
- These give the **autonomous < 300 ms life-safety** signal; the opto switch output drives the trip
  contactor. The ESP32 GPIO reads them for fast **sensing / logging**, not for the trip itself.

## 11. Open questions / decisions to review with Glenn

- [ ] Exact register **offsets + function code + scaling** → sniff (Phase 0b) or ask IVY.
- [ ] **12 V supply** availability on the EMS PCB for the RCM CT.
- [ ] **Level-shifting** the 3 × 12 V alarm lines to 3.3 V GPIO — which GPIOs.
- [ ] **Baud rate**: `softconfig.ini` has `BaudRate=5` (an index) — decode / confirm default.
- [ ] Confirm our exact unit variant (MD0630T41A-1 vs MD0630STA-C) and its comms option.
- [ ] Insights-Manager **base interface** design (with the DTM-EMS reference files).

## 12. Sprint steps

| Sprint       | Deliverable                                                            | Gate                   |
| ------------ | ---------------------------------------------------------------------- | ---------------------- |
| **S1** | PC bring-up + serial sniff → register map +**this design spec** | ←**review now** |
| S2           | ESP32`Modbus_MD0630` driver: read leakage / version / thresholds     |                        |
| S3           | Config-time threshold writes (6/30 mA) + read-back confirm             |                        |
| S4           | Energy data-model expansion + energy-change correlation cache          |                        |
| S5           | Leakage Insights Manager + MQTT alerts / LV-feeder subtopic            |                        |
| S6           | Alarm-line GPIO integration + reverse-power-flow threshold (27 mA)     |                        |

## 13. Files in this folder

- `modbus_md0630.h` / `modbus_md0630.cpp` — **draft** driver skeleton (AC+DC read), to be finalized
  after S1 with the sniffed register offsets.
- `email_to_ivy.md` — email requesting the Modbus protocol document (backup to sniffing).

## Sources analysed (local IVY files)

- `IVY RCM PC Software/…/CT.db` → `SaveData` table = the 7-register field suite.
- `PC OPERATION INSTRUCITIONS of AC DC leakage sensor-20220301.docx` → PC-tool workflow (write→read-back).
- `MD0630T41A-1/…规格书C V1.1-2023.4.13.docx` → 12 V supply, ranges, thresholds, pinout, 3 alarm outputs.
- `…/softconfig.ini` → PC-tool serial config (COM, baud index).
- Project: `include/metering/leakage_model_ivy41a.h`, `include/core/data_model.h`
  (`LeakageData` stub, `CurrentHistory` circular-buffer pattern).
