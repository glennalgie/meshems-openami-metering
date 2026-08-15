# AC leakage bench test — isolated 9 V AC injection (MD0630TA1A-1)

**Date:** 2026-08-15
**Operator:** Bonheur Allahaddje (NESL / 10Power)
**Method:** inject a known AC residual current with an isolated 9 V AC source, read the AC channel,
and cross the 30 mA AC alarm threshold. Companion to the DC resistor-ladder test.

## Bench setup

- Source: **Mascot Type 8310** AC/AC adapter, **230 VAC -> 9 V AC, 340 mA** (isolating -> output is SELV).
- Loop: `9 V AC -> R (1 kohm) -> wire threaded N turns through the CT window -> back to the adapter`.
- The AC loop is **galvanically isolated** from the ESP32/module and couples to the CT **magnetically only**
  (no shared ground). Loop current is small (~9-12 mA); the N turns multiply what the CT sees.
- Readout: **identical to the DC test** (MD0630 at 12 V, UART to the ESP32 on GPIO42/7, 4.7 kohm pull-ups
  to 3V3, common ground). Firmware: real reads (`-DLEAKAGE_BRINGUP`, MOCK off), scale fix applied.
- Wiring schematic: `wiring_AC_leakage_test.html` (and `report/wiring_AC.png`).

## Results

| Turns through CT | AC leakage read | DC channel | vs 30 mA threshold |
|------------------|----------------:|-----------:|--------------------|
| 2 turns          | **24.68 mA**    | 0.00 mA    | below -> no alarm  |
| 3 turns          | **36.97 mA**    | 0.00 mA    | above -> alarm     |

Raw serial capture (`scratchpad/reset_capture.py`, COM4):

```
MD0630 [addr:1 fc:0x03]: AC=24.68 mA  DC=0.00 mA  (ok:1 fail:0)   <- 2 turns
MD0630 LEAKAGE STEP UP: 0.00 -> 24.68 mA | energy corr: yes
MD0630 [addr:1 fc:0x03]: AC=36.97 mA  DC=0.00 mA  (ok:1 fail:0)   <- 3 turns
MD0630 LEAKAGE STEP UP: 0.00 -> 36.97 mA | energy corr: yes
```

## Findings

1. **AC detection works end to end.** The injected AC leakage is read linearly:
   24.68 / 36.97 mA at 2 / 3 turns = **~12.3 mA per turn** (the light-loaded adapter sits a little above
   9 V, so ~12.3 V effective; the module reads the true mA regardless).
2. **Register map re-confirmed (mirror of the DC test).** The AC injection moved **0x0001** while
   **0x0000 (DC) stayed at 0.00 mA** the whole time -> AC = 0x0001, DC = 0x0000.
3. **Threshold crossing demonstrated.** 24.68 mA reads below the 30 mA AC threshold (no alarm),
   36.97 mA reads above it (alarm).
4. **S4 energy-change correlation fired** on each AC step (`energy corr: yes`), exercising that path on
   the AC channel too.

## Status

With the DC bench (2026-08-04) and this AC bench (2026-08-15), **both leakage channels of the MD0630 are
now validated on real hardware**:

- **DC:** 5.13 mA (below) -> 7.19 mA (alarm), threshold 6 mA.
- **AC:** 24.68 mA (below) -> 36.97 mA (alarm), threshold 30 mA.

The MD0630 AC+DC leakage bench validation is complete.
