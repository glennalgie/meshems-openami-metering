# S2 — Live read validation (ESP32 ↔ real MD0630)  ✅ DONE

**Author:** Allahaddje Bonheur — 10Power / NESL
**Date:** 2026-07-20 (lab)
**Board:** ESP32-S3-DevKitC-1 · `env:esp32-s3-devkitc-1`

## What S2 delivers
The `Modbus_MD0630` driver reads **live AC + DC residual current from the real module** over the
ESP32's RS-485/UART bus (not mock, not the PC), using the register map derived in S1.

## Wiring (bench)
| Module | ESP32 | Note |
|---|---|---|
| TX | **GPIO42** (RS485_1_RX) | + 4.7 kΩ pull-up to **3V3** |
| RX | **GPIO7** (RS485_1_TX) | + 4.7 kΩ pull-up to **3V3** |
| GND | GND | common ground (ESP32 + module + 12 V PSU−) |
| VCC | **12 V** PSU | never the ESP32 |

⚠️ Pull-ups go to **3V3** (ESP32-S3 GPIOs are **not** 5 V tolerant). The module UART connects
**directly** to the ESP32 UART pins (no RS-485 transceiver — the MD0630 is UART, not differential).
The pins are the **GPIO numbers 42 / 7**, *not* the silkscreen "RX"/"TX" (those are the USB console
UART, GPIO44/43).

## Firmware config (this bring-up build)
`platformio.ini`: `LEAKAGE_MOCK` and `LEAKAGE_PROBE` both **off** → real reads. `LEAKAGE_BRINGUP`
**on** → skips the SHT20 poll (the module sits at address 1, same as the SHT20 → polling both
collides on the bus; this caused the initial intermittent reads). WiFi/MQTT **off** for the bench
(the lab WiFi retry loop was blocking the main loop, so the Modbus poll only ran ~once/min).

## Evidence — serial capture (module at rest, no induced leakage)
```
SETUP: MODBUS: MD0630 leakage CT: address:1
MD0630 [addr:1 fc:0x03]: AC=0.00 mA  DC=0.00 mA  (ok:9  fail:0)
MD0630 [addr:1 fc:0x03]: AC=0.00 mA  DC=0.00 mA  (ok:10 fail:0)
MD0630 [addr:1 fc:0x03]: AC=0.00 mA  DC=0.00 mA  (ok:11 fail:0)
MD0630 [addr:1 fc:0x03]: AC=0.00 mA  DC=0.00 mA  (ok:12 fail:0)
```
**Reliable: 12 consecutive successful reads, 0 failures.** The driver reads reg **0x0000 (DC)** and
**0x0001 (AC)** at **FC 0x03**, scales **×0.1 mA**, and feeds `leakageModel` (published on
`subpanel_RCMleaks` when WiFi/MQTT are enabled).

## Debug notes (what it took)
- Initial timeouts on the ESP32 bus were **wrong pins**: the module was on the silkscreen "RX/TX"
  (USB console, GPIO44/43) instead of **GPIO42/GPIO7**. Moving to the numbered pins gave the first
  successful read.
- Intermittent reads after that = the **SHT20 poll at the same address 1**; `LEAKAGE_BRINGUP` skips
  it → reliable.
- Earlier PC-side derivation (S1) used the identical Modbus semantics (FC03, offsets, ×0.1 mA), so
  the driver logic was already proven; S2 confirms it on the ESP32 itself.

## Open / next
- [ ] **Confirm DC vs AC** for regs 0x0000/0x0001 (both 0.00 at rest): induce a known DC or AC leak
      and see which register moves. Current assumption: 0x0000 = DC, 0x0001 = AC (DC-before-AC, as the
      thresholds).
- [ ] Production: commission the CT to node **100** (S3), then remove `LEAKAGE_BRINGUP` so the SHT20
      (addr 1) and the CT (100) coexist; re-enable WiFi/MQTT.
- → **S3**: write thresholds (DC 6 mA @0x0002 / AC 30 mA @0x0003) via **FC06**, unlock + read-back.

## Status: **S2 COMPLETE** — ESP32 reads the live module reliably.
