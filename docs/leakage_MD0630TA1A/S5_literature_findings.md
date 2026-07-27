# S5 — Literature findings (what actually grounds the isolation schema)

**Date:** 2026-07-27
**Honesty note:** the four publisher articles cited in the S5 brief (Elsevier, Nature, Springer,
MDPI) **could not be read** — their sites return HTTP 403 / auth redirects to automated fetching.
The findings below come from **two open-access sources read in full** plus targeted searches that
returned concrete method statements. Sources listed at the end.

## 1. Nomination — compare *trends*, nominate the "odd one out" (not raw amplitude)
From the faulty-feeder-identification method (PMC7795609, read in full):

- It does **not** compare absolute magnitudes across feeders. It compares the **shape/trend** of the
  (zero-sequence) transient using a **correlation metric** (grey T-type), computed on **increments**
  (change), not values.
- The **faulted feeder is the one that correlates LEAST with the others**:
  `μk = mean correlation of feeder k with all others`; **faulted = argmin(μk)**, with `μk ≤ μset`
  (empirical `μset = 0.2`). A bus fault (all similar) gives `μ ≈ 0.7–0.8`.
- **Why it matters for us:** the EMS sit at different positions (like mixed cable/overhead feeders),
  so absolute levels are heterogeneous. Trend-correlation is robust to that — the EMS nearest the
  leak has the distinctive signature and correlates least with its peers.

**→ S5 change:** nomination combines **(a) magnitude ranking** *and* **(b) lowest cross-correlation
with peers** (the odd-one-out), gated by confidence. Trigger on **change**, not level.

## 2. Direction — the residual voltage / residual current phase angle (~90°)
Established faulty-feeder selection criterion:

- Faulted feeder when **|∠V_res − ∠I_res − 90°| < Δθ**.
- **Non-faulted:** I_res *leads* V_res by ~90°. **Faulted:** I_res *lags* V_res by ~90°.
- **Upstream vs downstream:** downstream of the fault, V_res *leads* I_res by ~90°; upstream, V_res
  *lags* I_res. → this is exactly **direction discrimination**.

**→ S5 change:** this is the principled basis for the reserved `phase_angle_deg` field and the IVY RCM
upgrade ask — with phase angle, magnitude ranking becomes true **direction isolation**.

## 3. Isolation — open switches on BOTH sides (smallest segment)
From the FLISR workflow (read):

- Sequence: **Detect → Locate → Isolate → Restore**. Locate uses distributed sensors measuring
  current flow, voltage loss and fault signatures.
- **Isolation opens switches on both sides** of the faulted section → only the damaged segment is
  de-energised. Restoration closes a normally-open **tie switch** to an alternate source.
- Timescale **seconds–minutes**; runs **fully automatic** or **operator-approved**.

**→ S5 change:** isolation is **between the two EMS that bracket** the leak (upstream + downstream),
not merely "the closest EMS opens". The auto-vs-operator-approved mode matches our confidence gate
(auto-isolate vs alert-only).

## 4. Trigger on change (confirmed)
Detection fires on a **change threshold** (e.g. zero-sequence voltage > 15 % of max phase voltage),
i.e. change-based, not absolute-level — confirming the S5 design choice.

## Sources
- ✅ read in full: [PMC7795609 — Faulty Feeder Identification by data analysis & similarity comparison](https://pmc.ncbi.nlm.nih.gov/articles/PMC7795609/)
- ✅ read: [FLISR — Fault Location, Isolation and Service Restoration (workflow)](https://vividcomm.com/2026/04/05/flisr-fault-location-isolation-and-service-restoration/)
- extracts: [Faulty-feeder selection by V_res/I_res phase angle ~90°](https://www.researchgate.net/publication/325562076_An_Additional_Criterion_for_Faulty_Feeder_Selection_During_Ground_Faults_in_Compensated_Distribution_Networks) · [SEL protection app note](https://selinc.com/api/download/3191/)
- ❌ blocked (403 / paywall, not read): Elsevier `S0142061525003199`, Nature `s41598-025-20048-z`,
  Springer `s42162-026-00646-5`, MDPI `1996-1073/14/16/5024`.
