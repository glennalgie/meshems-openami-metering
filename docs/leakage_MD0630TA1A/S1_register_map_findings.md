# S1 — DERIVED register map (IVY MD0630T41A-1)  ✅ RESOLVED

**Author:** Allahaddje Bonheur — 10Power / NESL
**Date:** 2026-07-20
**Method:** live bench — ESP32 5 V rail powering pull-ups, module on a USB-TTL to the PC;
PowerShell Modbus prober + confirmation via IVY's own `CT.exe` (its Modbus frames + labelled fields).

## Serial / link (confirmed)
| Item | Value |
|---|---|
| Baud / format | **9600 · 8 · N · 1** |
| Slave address | **1** (CT.exe uses 0x00 broadcast in its frames; the module also answers addr 1) |
| Function code | **FC 0x03 (holding)** — FC 0x04 (input) returns the same values |
| Scaling | **raw × 0.1 mA** (60 → 6.0 mA, 300 → 30.0 mA) |

## Register map (derived — CT.exe reads 3 groups)

`Write 00 03 00 00 00 06` → reads **0x0000–0x0005**:

| Register | Raw | ×0.1 mA | Field | R/W |
|---|---|---|---|---|
| **0x0000** | 0 | 0.00 | **DC Leakage Current** | R |
| **0x0001** | 0 | 0.00 | **AC Leakage Current** | R |
| **0x0002** | 60 (0x003C) | 6.0 | **DC Threshold** (default 6 mA) | R/W |
| **0x0003** | 300 (0x012C) | 30.0 | **AC Threshold** (default 30 mA) | R/W |
| 0x0004 | 301 (0x012D) | — | internal (not shown by CT.exe) | R |
| 0x0005 | 0 | — | internal | R |

`Write 00 03 01 00 00 01` → **0x0100 = 1 = Version Number** (R)
`Write 00 03 01 10 00 02` → **0x0110 = 0**, **0x0111 = 5 = Minor Version Number** (R)
**Modbus ID** = the slave address itself (displayed 001).

> DC-vs-AC assignment of 0x0000/0x0001 is inferred from the DC-before-AC ordering of the
> thresholds (0x0002 DC, 0x0003 AC); both read 0 at rest, so confirm by inducing a known DC or AC
> leak and re-reading.

## Raw evidence — CT.exe "Communication information" log
```
Write : 00 03 00 00 00 06 C4 19
Read  : 00 03 0C 00 00 00 00 00 3C 01 2C 01 2D 00 00 AF 91   (0,0,60,300,301,0)
Write : 00 03 01 00 00 01 84 27
Read  : 00 03 02 00 01 44 44                                 (0x0100 = 1 = Version)
Write : 00 03 01 10 00 02 C5 E3
Read  : 00 03 04 00 00 00 05 7B 30                            (0x0110=0, 0x0111=5 = MinorVer)
```
CT.exe labelled read-out: Modbus ID 001 · Version 0001 · Minor Version 0005 ·
DC Threshold 6.0 mA · AC Threshold 30.0 mA · DC Leakage 0.00 mA · AC Leakage 0.00 mA.

## Writing thresholds (for S3)
Thresholds are the writable registers **0x0002 (DC)** and **0x0003 (AC)**. Use **FC 0x06 (write single
register)**, value = mA × 10 (e.g. 6.0 mA → 60 = 0x003C). CT.exe's `unLock ID` step suggests an unlock
before write may be required; always **read back to confirm** (per Glenn). Not exercised yet — do NOT
write blindly; verify the unlock sequence first.

## Status
S1 register map **DERIVED and confirmed**. Driver `md0630.reg.*` updated to these offsets (FC03,
×0.1 mA). This resolves Glenn's "register-map conflict" question: the real layout is **neither** the
AI-proposed spec (AC leak @0x00, thresholds @0x04/05) **nor** the raw CT.db field order — it is the
table above. S2 (driver read of live leakage) is unblocked; next real ESP32 read needs the module on
the ESP32 RS-485/UART bus (production wiring), not the PC USB-TTL used here to derive the map.
