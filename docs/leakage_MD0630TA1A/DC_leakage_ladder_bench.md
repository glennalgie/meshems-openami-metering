# DC leakage bench test — resistor-ladder emulator (MD0630TA1A-1)

**Date:** 2026-08-04
**Operator:** Bonheur Allahaddje (NESL / 10Power)
**Method:** Glenn's DC resistor-ladder emulator (inject a known DC leakage current
through the CT window and compare the module reading against Ohm's law).

## Bench setup

- ESP32-S3 **3V3** rail as the DC source (measured 3.30 V across a 100 Ω load).
- A **single wire** passed **once** through the MD0630 CT window (1 turn), then to a
  parallel **resistor ladder** referenced to the **common GND**.
- Each ladder branch = `R_series + 100 Ω` between NODE A and GND. Branch current = `3.3 V / R_total`.
- MD0630: addr 1, 9600 8N1, **FC03**, registers `0x0000` (DC), `0x0001` (AC).
- Firmware flags: `-ULEAKAGE_MOCK -DLEAKAGE_BRINGUP -UENABLE_WIFI -UENABLE_MQTT` (real reads, bench).

| Branch | R_series | R_total | Current (3.3 V / R) |
|--------|---------:|--------:|--------------------:|
| SW1 | 1.5 kΩ | 1.6 kΩ | 2.06 mA |
| SW2 | 1.0 kΩ | 1.1 kΩ | 3.00 mA |
| SW3 | 1.5 kΩ | 1.6 kΩ | 2.06 mA |
| SW4 | 1.0 kΩ | 1.1 kΩ | 3.00 mA |

## Results

Currents cumulate on the shared node (1 turn), so the module sees the sum of the
active branches. Raw register value = `displayed_mA / old_scale(0.1)`.

| Active branches | Injected (Ohm's law) | Raw register | DC leakage read (x0.01 mA) | AC read |
|-----------------|---------------------:|-------------:|---------------------------:|--------:|
| SW1+SW2         | 5.06 mA | 513  | **5.13 mA** | 0.00 mA |
| SW1+SW2+SW3     | 7.13 mA | 719  | **7.19 mA** | 0.00 mA |
| all four        | 10.13 mA | 1018 | **10.18 mA** | 0.00 mA |

Agreement with Ohm's law is within ~1% across three points, all inside the
datasheet DC range (2–15 mA).

## Findings

1. **DC leakage detection works end-to-end** — the module tracks the injected DC
   leakage, first confirmed non-zero induced reading on real hardware.
2. **DC/AC register map confirmed.** Injecting a DC-only current moved `0x0000`
   while `0x0001` stayed at 0.00 mA the whole time → **DC = 0x0000, AC = 0x0001**
   (resolves the S1 "confirm by inducing a known leak" note).
3. **Leakage-current register scaling = ×0.01 mA** (NOT ×0.1). The three points
   (raw 513/719/1018 → 5.13/7.19/10.18 mA) match the injected currents only under
   ×0.01. Under ×0.1 the implied currents (51/72/102 mA) would exceed the sensor's
   2–15 mA range and could not read linearly.
4. **Threshold registers keep ×0.1 mA** (unchanged): CT.exe frames show `003C`=6.0 mA
   (DC) and `012C`=30.0 mA (AC). So the device exposes *current* at 0.01 mA
   resolution and *thresholds* at 0.1 mA resolution — two different register scales.

## Threshold crossing (6 mA DC)

With the firmware corrected to display real mA:

| Config | Displayed DC | vs 6 mA threshold |
|--------|-------------:|-------------------|
| SW1+SW2       | 5.13 mA | below → no alarm |
| SW1+SW2+SW3   | 7.19 mA | above → DC alarm expected |

## Firmware change

`include/metering/modbus_md0630.h` + `src/metering/modbus_md0630.cpp`:
split the single `ma_scale` into two:

- `leak_scale = 0.01f` — leakage-current registers (`0x0000`/`0x0001`), used in `poll_real()`.
- `ma_scale = 0.1f` — threshold registers (`0x0002`/`0x0003`), used in
  `readThreshold_mA()` / `writeThreshold_mA()` (unchanged).

After the fix, the same 2-branch setup reads **5.13 mA** directly (was 51.3), and
3 branches reads **7.19 mA** (was 71.9).
