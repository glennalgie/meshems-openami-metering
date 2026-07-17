# S5 — MQTT Leakage-Isolation Schema (design brief)

**Author:** Allahaddje Bonheur — IoT Fellow, 10Power (with NESL)
**Reviewer:** Glenn (supervisor) · Design review 2026-07-17
**Status:** DRAFT FOR REVIEW — design only, no code yet

> Glenn's ask: *"The ultimate use case is to coordinate amongst EMS's on a shared 3 phase LV feeder to
> isolate a leakage (AC or DC) before it gets to life threatening RCB breaker level. What MQTT schema
> for leakage isolation would enable this? … 3 EMS on an LV feeder at different cabinet takeoffs, each
> sees leakage creeping up in a phase — do they see different levels? … the EMS closest to the leakage
> is nominated. A service alert to ops is issued."*

---

## 0. Scope and the safety boundary (read first)

**This schema never performs the life-safety trip.** The sub-300 ms disconnection stays in the
MD0630's **hardware fault line** driving the contactor, exactly as today. MQTT coordination is for
**monitoring, localisation, and *pre-emptive* isolation** — acting in the seconds *before* leakage
reaches RCB level, so we isolate **one takeoff** instead of tripping the **whole feeder**.

| Layer | Latency | Owner | Purpose |
|---|---|---|---|
| MD0630 hardware fault line | < 300 ms | the module | life safety — non-negotiable |
| **MQTT isolation schema (this doc)** | seconds | the EMS mesh | localise + isolate the smallest section |
| Ops alert | seconds–minutes | humans | dispatch / investigate |

**Why MQTT is the right transport here (and why we are not competing with GOOSE).** In substations,
IEC 61850 **GOOSE** carries sub-4 ms protection tripping — but it is deliberately **LAN-bound**, and
extending it across a wide area introduces latency and reliability problems that compromise
protection. Our fast trip is *already* solved in hardware (< 300 ms, no network in the path). What
remains is **seconds-scale, feeder-wide, many-node coordination** — which is exactly MQTT's domain,
and is why MQTT is studied as the messaging layer for IEC 61850-based microgrid architectures. The
three tiers are complementary, not alternatives.

In the literature our function is called **FLISR** — *Fault Location, Isolation and Service
Restoration*. This brief is a **leakage-specific, decentralised FLISR** for LV feeders (§12).

## 1. The problem

A radial 3-phase LV feeder (village minigrid) with several cabinets; each cabinet has an EMS with an
RCM CT measuring AC+DC residual current on its takeoff. When insulation fails somewhere, residual
current escapes to earth. Today each EMS sees only its own number and can only wait for the RCB to
trip — which kills the **whole feeder**. We want the EMS mesh to answer, together:

1. **Is there a leak?** (vs noise)
2. **Where is it?** (which cabinet / which phase)
3. **Who acts?** (nominate one EMS)
4. **Is it a leak at all — or theft?** (stretch)

## 2. Physical basis — why levels differ between EMS

Glenn's question *"do they see different levels?"* is the crux. Yes, and that is the signal:

- Residual current returns to the source through earth. The EMS **electrically closest** to the fault
  sees the **largest** residual magnitude; EMS further away see a **diluted** share (parallel earth
  return paths, cable capacitance, and the CT's position relative to the fault current path).
- EMS **downstream** of the fault see little or nothing. The **boundary** between "sees it" and
  "doesn't" brackets the fault.
- All EMS observing the *same* physical fault share a **correlated ramp onset** — a common time
  signature. This separates one real fault from several unrelated small leaks.

⚠️ **Hard constraint (from the datasheet):** the MD0630 measures **absolute value, no current
direction polarity**. So *direction* cannot be measured today — we can only rank **magnitudes** and
correlate **ramps**. This is precisely why Glenn's note about **phase-angle tracking** matters: with
the phase angle of the leakage current vs the phase voltage (and vs neutral current), upstream /
downstream discrimination becomes possible. See §8 (ask to IVY).

⚠️ **Standing residual current is real.** Recent work on distributed earth-fault detection in LV
networks (§12) stresses two failure modes we must design against: (a) residual current **exists in
non-fault states** (cable capacitance, filters, PV inverter EMI) — so an absolute threshold alone
produces false rounds; and (b) poor **selectivity** causes **mal-tripping of healthy feeders**.
Mitigations adopted here: each EMS **baselines its own standing residual** and reports `ramp` and
`onset_ts` (change-based, not level-based) as the round trigger; nomination compares peers on the
**same feeder and phase**; and low confidence **refuses to isolate** (§6).

## 3. Topology and transport

Cloudless and distributed, per Glenn's rule (*"we'd never build a Modbus TCP server on the EMS …
central cloud modbus tcp wont scale for distributed cloudless intelligent LV feeders"*):

- One **local broker per feeder** (or a bridged pair for redundancy). No cloud in the decision path.
- **No coordinator / no master.** Every EMS subscribes to the feeder topic, receives everyone's
  telemetry, and applies the **same deterministic rule** (§6) — so all nodes independently reach the
  **same** nomination. The nominated EMS acts; the others stand down. This keeps the mesh
  autonomous and avoids a single point of failure.

## 4. Topic tree

Existing per-EMS telemetry stays as-is (`subpanel_RCMleaks`). We add a feeder coordination subtree:

```
lvfeeder/{feeder_id}/leakage/telemetry/{ems_id}   # QoS0, 1 Hz in watch, 0.1 Hz idle — each EMS publishes
lvfeeder/{feeder_id}/leakage/isolation            # QoS1, retained=false — nomination rounds
lvfeeder/{feeder_id}/leakage/alert                # QoS1, retained=true  — ops-facing service alert
lvfeeder/{feeder_id}/leakage/topology             # QoS1, retained=true  — commissioning: feeder order
```

Rate is **adaptive**: idle nodes publish slowly; once a node enters `watch` it publishes at 1 Hz and
the others follow (they see the round open). This keeps the bus quiet in normal operation.

## 5. Message schemas

### 5.1 Telemetry — `…/leakage/telemetry/{ems_id}`
```json
{
  "ems_id": "street-ems-07",
  "feeder_id": "lv-12",
  "node": 100,
  "phase": "L2",
  "ts": 1752750000,
  "seq": 42,
  "state": "watch",
  "ac_mA": 28.6,
  "dc_mA": 5.7,
  "ac_threshold_mA": 30.0,
  "dc_threshold_mA": 6.0,
  "ramp_ac_mA_s": 0.50,
  "ramp_dc_mA_s": 0.10,
  "onset_ts": 1752749940,
  "in_fault": false,
  "powerflow": { "direction": "import", "p_W": 3200 },
  "phase_angle_deg": null,
  "sensor_ok": true
}
```
- `state` ∈ `normal | watch | isolating | fault | restored` (§7).
- `ramp_*` = mA per second over a short window — **the ramp rate Glenn asked for**.
- `onset_ts` = when the channel first left `normal` — the **correlation key** across EMS.
- `powerflow` = the node's own import/export at that moment — required to interpret levels under
  reverse flow (§8).
- `phase_angle_deg` = `null` today (MD0630 is absolute-value only) — reserved for direction isolation.

### 5.2 Isolation round — `…/leakage/isolation`
```json
{
  "feeder_id": "lv-12", "round_id": "lv-12:1752749940:L2", "ts": 1752750002,
  "phase": "L2",
  "participants": [
    { "ems_id": "street-ems-07", "ac_mA": 28.6, "ramp_ac_mA_s": 0.50, "rank": 1 },
    { "ems_id": "street-ems-04", "ac_mA": 11.2, "ramp_ac_mA_s": 0.19, "rank": 2 },
    { "ems_id": "street-ems-01", "ac_mA":  2.1, "ramp_ac_mA_s": 0.04, "rank": 3 }
  ],
  "nominated_ems": "street-ems-07",
  "rule": "max_ac_mA__correlated_onset",
  "confidence": 0.82,
  "action": "isolate_takeoff",
  "quorum": { "expected": 3, "reporting": 3 }
}
```

### 5.3 Ops alert — `…/leakage/alert`
```json
{
  "severity": "warning", "feeder_id": "lv-12", "ts": 1752750002,
  "located_at": { "ems_id": "street-ems-07", "phase": "L2" },
  "ac_mA": 28.6, "dc_mA": 5.7,
  "headroom_to_trip_mA": 1.4, "projected_trip_in_s": 3,
  "classification": "leakage",
  "confidence": 0.82,
  "recommended_action": "Inspect L2 takeoff at cabinet 07; leakage ramping 0.5 mA/s since 10:19:00Z"
}
```

## 6. The nomination rule (deterministic, runs identically on every EMS)

1. **Open a round** when any node's leakage crosses the **watch threshold** (proposed: **50 % of the
   trip threshold** → AC 15 mA, DC 3 mA). `round_id = feeder:onset_ts:phase`.
2. **Collect** telemetry for a **settling window** (proposed **2 s**) — enough for all nodes to report.
3. **Filter** to the same `phase` with a **correlated onset**: `|onset_ts − round.onset_ts| ≤ W`
   (proposed W = 5 s). Uncorrelated nodes are separate events, not this fault.
4. **Rank** by `ac_mA` (resp. `dc_mA`) descending → **rank 1 = electrically closest**.
5. **Tie-break** deterministically: higher `ramp` wins; then lowest `ems_id` lexicographically — so
   every node computes the identical winner with no negotiation.
6. **Confidence** = separation between rank 1 and rank 2, e.g.
   `conf = (v1 − v2) / v1`, clamped to [0,1].
7. **Act:**
   - `conf ≥ 0.3` **and** quorum met → **nominated EMS isolates its takeoff**, others hold.
   - `conf < 0.3` (levels too close — fault likely *between* two cabinets) → **do not auto-isolate**;
     raise the ops alert with both candidates.
   - quorum not met (a node silent) → alert only. **Never isolate on partial information.**

**Fail-safe:** if the mesh is silent or the broker is unreachable, nothing changes — the hardware
fault line still protects life. Degradation is graceful.

## 7. Per-EMS state machine

```
normal ──leak > watch──► watch ──round opened──► isolating ──nominated & isolated──► fault
   ▲                       │                          │                                │
   └──────leak < watch─────┴──────not nominated───────┘◄────────leak cleared───────────┘
                                                            (restored → normal)
```
`restored` is published when leakage clears, so ops (or the EMS autonomously) can re-close — matching
the existing note in `leakage_model_ivy41a.h` that *"these leakage sensors report when leakage is
removed and each StreetPoleEMS independently can remotely or autonomously restore local power flow"*.

## 8. Reverse power flow (the Achim / GISMO · Daniel Gerber / LBNL challenge)

*Detect leakage when power flow is reversed at an EMS — e.g. a tenant's export exceeds consumption at
the multi-tenant cabinet into a phase.*

Two distinct problems:

1. **Sensitivity.** Under export the field study calls for lowering the **AC threshold 30 → 27 mA**
   (already specified in S6). The EMS knows its own flow direction, so it adapts its own threshold and
   **publishes it** (`ac_threshold_mA` in telemetry) — peers must not assume a fixed 30 mA when
   ranking, so ranking uses **absolute mA**, not "percent of threshold".
2. **Interpretation.** An exporting node is a **source**: it changes the earth-return path, so the
   "highest level = closest" heuristic can mis-rank. Mitigation in this schema:
   - every telemetry message carries `powerflow.direction` + `p_W`;
   - when **any** participant reports `export`, confidence is **de-rated** and the round prefers
     **alert-only** over auto-isolation until phase angle is available;
   - this is exactly the case where **phase-angle tracking** resolves the ambiguity.

**Prior art supports this exactly.** Established practice for *faulty-feeder selection* declares a
feeder faulted when the **phase angle between residual voltage and residual current is close to 90°**
— i.e. direction discrimination is a **phase-angle** problem, not a magnitude problem. Magnitude
ranking (what we can do today) is therefore a **usable approximation**, and phase angle is the
principled fix. This is strong support for Glenn's remark that IVY could upgrade the Modbus RCM.

**Ask to IVY (Glenn is already in contact):** can the RCM report the **phase angle of the leakage
current** (vs phase voltage / neutral current)? That single field turns magnitude-ranking into true
**direction isolation** and fixes the reverse-flow case. Field reserved as
`phase_angle_deg` today.

## 9. Stretch — leakage vs energy **theft**

Both show up as a non-zero residual, but they separate cleanly on three axes:

| Signal | Insulation leakage | Energy theft |
|---|---|---|
| Magnitude | **mA** (2–100 mA) | **A** (orders larger) |
| Shape | slow **ramp**, weather/humidity correlated | abrupt **step**, follows the thief's load |
| Energy balance | none — mA is invisible in kWh | feeder meter **>** Σ tenant meters |
| Coincidence | no matching energy event | leakage step **coincides** with an energy step |

This is what the **S4 energy-change correlation cache** is for: on every leakage step, look back ±X ms
and ask *"what energy event happened at that instant?"* A leakage step with **no** energy event ⇒
insulation. A leakage step **coincident** with a load step **and** a feeder-vs-tenants energy gap ⇒
`classification: "suspected_theft"` in the alert. The classifier therefore needs **energy telemetry**
in the same coordination round — a natural extension of the schema, not a separate system.

## 10. The leak test (Glenn's experiment) — and how the mock is the test bench

**Setup:** 3 EMS at different cabinet takeoffs on one feeder, one phase under test.

**Stimulus:** a simulated leakage ramp — no energised fault needed. `Modbus_MD0630::setMockRamp()`
(already validated on hardware, see `S2_mock_validation.md`) drives each EMS with a **different
ceiling and rate to emulate electrical distance**:

| EMS | Emulated position | Mock ramp | Expected |
|---|---|---|---|
| street-ems-07 | closest to fault | 0 → 45 mA @ 0.50 mA/s | **nominated** |
| street-ems-04 | mid feeder | 0 → 18 mA @ 0.19 mA/s | rank 2 |
| street-ems-01 | far / upstream | 0 → 3 mA @ 0.04 mA/s | rank 3 |

**Measure:** (a) do levels separate as predicted? (b) does the round nominate ems-07? (c) latency from
onset → nomination; (d) confidence value; (e) behaviour when two nodes are made nearly equal
(should refuse to isolate and alert instead); (f) repeat with one node in `export`.

**Then** repeat on real hardware once the register map lands, using an injected mA-level leak.

## 11. Open questions for review

- [ ] **Feeder topology**: should each EMS know its **position / upstream neighbour** (set at
      commissioning, published on `…/leakage/topology`)? Ordered topology makes localisation far
      stronger than magnitude-ranking alone. Worth the commissioning cost?
- [ ] **Watch threshold** at 50 % of trip (15 mA AC / 3 mA DC) — right value?
- [ ] **Settling window** 2 s and **onset correlation window** 5 s — acceptable vs ramp speed?
- [ ] **Auto-isolate or alert-only** for v1? (Proposal: alert-only until the leak test validates
      nomination accuracy.)
- [ ] **Broker topology** on a cloudless feeder: one local broker per feeder, bridged? What happens
      during a broker partition — is `alert-only` the right degraded mode?
- [ ] **Phase angle** from IVY — is it on the table for the EV sample/eval hardware?
- [ ] Does this belong to the **repeatable Insights-Manager framework** as `LeakageInsights`,
      alongside `NeutralInsights` (the DTM-EMS neutral-balancer files Glenn will share)? The
      nomination rule feels like a generic *"rank peers, nominate, alert"* pattern reusable for
      neutral unbalance too.

## 12. Prior art — how this brief is grounded

Per Glenn's suggestion to search fault-isolation schemas. The literature says our approach is
**recognised, named, and already validated in adjacent settings** — while the *leakage-specific,
mA-scale, LV-feeder* variant is where we add something.

**Our function has a standard name: FLISR** (Fault Location, Isolation and Service Restoration).
Framing the brief this way lets us talk to utilities in their own vocabulary.

| # | Source | What it gives us |
|---|---|---|
| 1 | [Distributed earth fault detection for LV distribution networks using phase voltage synchronization](https://www.sciencedirect.com/science/article/pii/S0142061525003199) (Elsevier, **2025**) | Closest prior art: **residual current from multiple measuring points** on **LV** networks. Source of our two design warnings — **residual current exists in non-fault states**, and **selectivity** must prevent **mal-tripping healthy feeders** (§2). |
| 2 | [A Fully Decentralized Multi-Agent Fault Location and Isolation for Distribution Networks with DGs](https://www.researchgate.net/publication/349186516_A_Fully_Decentralized_Multi-Agent_Fault_Location_and_Isolation_for_Distribution_Networks_With_DGs) | Validates our **no-master, fully decentralised** nomination (§3) — including **with distributed generation**, i.e. the export case. |
| 3 | [Adaptive protection based on multi-agent systems for AC microgrids: a review](https://www.sciencedirect.com/science/article/pii/S0306261924020567) (Applied Energy, 2024) | MAS taxonomy (hierarchical / hybrid / **decentralised**). Notes MAS **removes single points of failure** but **may not be economical for small microgrids** — a caveat to answer for a village minigrid. |
| 4 | [Protection challenges and emerging solutions in renewable-integrated microgrids: a critical review](https://link.springer.com/article/10.1186/s42162-026-00646-5) (Energy Informatics, **2026**) | Most recent review; names our exact obstacles: **bidirectional power flow**, **low fault currents**, **communication dependency** — i.e. the Achim/GISMO reverse-flow challenge is a known open problem, not our oversight. |
| 5 | [High impedance fault detection using signal correlations](https://www.researchgate.net/publication/378572095_High_impedance_faults_detection_in_distribution_systems_using_signal_correlations) | Supports **onset/ramp correlation across nodes** (§6 step 3) as a discriminator — and high-impedance faults are the low-current regime closest to leakage. |
| 6 | Faulty-feeder selection by **residual voltage vs residual current phase angle ≈ 90°** (see §8) | Direction discrimination is a **phase-angle** problem → backs the **IVY phase-angle ask**. |
| 7 | [Performance of XMPP and MQTT on IEC 61850-based microgrid communication](https://www.mdpi.com/1996-1073/14/16/5024) (Energies) · [IEC 61850 + MQTT](https://emqx.medium.com/iec-61850-protocol-features-information-model-and-combination-with-mqtt-143ab622b535) · [GOOSE for protection coordination](https://ifluids.com/blog/iec-61850-goose-protection-coordination/) | Justifies the **transport split** (§0): GOOSE = sub-4 ms but **LAN-bound**; MQTT = the studied messaging layer for **distributed** microgrid coordination. We are not competing with GOOSE. |
| 8 | [Non-technical loss detection using smart-meter data / ML](https://www.nature.com/articles/s41598-025-20048-z) (Sci. Reports, **2025**) · [Critical review of theft-detection case studies](https://www.sciencedirect.com/science/article/pii/S2590174525000972) (2025) | The theft stretch (§9) maps onto the established **Technical Loss vs Non-Technical Loss** split: theft = **NTL**, detected via **AMI consumption patterns + energy balance**. Confirms the **energy-balance axis**; our contribution is adding the **leakage channel** as a second, independent axis. |

**Where we differ from the literature.** Published FLISR/MAS work targets **short-circuit / earth
faults at amp-to-kiloamp scale** on MV networks, using node voltages and feeder currents. We target
**mA-scale residual current** on an **LV** feeder, with a **Type B (AC+DC) RCM per cabinet**, acting
**pre-emptively before** the RCB threshold rather than after a trip. The gap our design must respect:
at mA scale the **standing residual** is comparable to the signal, so we trigger on **change (ramp +
correlated onset)** rather than level — this is the main adaptation of the prior art to our problem.
