# S3 — Config-time threshold writes (procedure)

**Goal:** write the life-safety thresholds — **DC 6 mA @ `0x0002`, AC 30 mA @ `0x0003`** — via
**FC 0x06** (write single register), **always with read-back**, at commissioning time only.

## Code (written — ready to test)
Driver methods in `include/metering/modbus_md0630.h` + `src/metering/modbus_md0630.cpp`:

- `writeThreshold_mA(Channel ch, float mA)` — `unlock() → writeSingleRegister(reg, mA×10) → read-back
  → confirm`. Returns `ku8MBSuccess` **only if the read-back matches**. Only ever writes
  `reg.dc_thresh`/`reg.ac_thresh` (never an unknown register).
- `readThreshold_mA(Channel ch)` — read one threshold back, in mA.
- `applySafetyDefaults()` — writes DC 6 mA then AC 30 mA (one-shot commissioning).
- Optional pre-write **unlock**: `unlock_required`, `unlock_reg`, `unlock_value` (see below).

Build flag **`-DLEAKAGE_WRITE_DEFAULTS`** (in `platformio.ini`, off by default): calls
`applySafetyDefaults()` once at `setup_md0630()`. Needs the **real module** (not mock). Run once, then
disable.

## The one unknown: the unlock sequence
`CT.exe` exposes an **"unLock ID"** step, so a write may require an unlock first. The exact frame is
**not yet known**. Two ways to settle it:

**A. Try a direct write first (no unlock).** With `unlock_required = false`, run
`writeThreshold_mA(...)`:
- Read-back **OK** → no unlock needed. Done.
- Write returns an **exception** (e.g. 0x02/0x04) → unlock is required → go to B.

**B. Derive the unlock by sniffing CT.exe.**
1. Module on the PC USB-TTL; open a serial sniffer (Device Monitoring Studio) on the COM port.
2. In `CT.exe`, set a threshold and click **Set/Write**.
3. Capture the frames. The **write** is `01 06 00 0x 0y yy CRC` (FC06 to `0x0002`/`0x0003`); any frame
   **before** it is the **unlock** — note its register + value.
4. Set in firmware: `md0630.unlock_required = true; md0630.unlock_reg = 0x____; md0630.unlock_value =
   0x____;`

## Test checklist (once the 12 V + module are on the bench)
1. Read current thresholds (`readThreshold_mA`) — should be DC 6.0 / AC 30.0 at factory.
2. Write a *different* test value (e.g. AC 27 mA) → read-back must confirm 27.0.
3. Restore defaults with `applySafetyDefaults()` (DC 6 / AC 30) → read-back OK.
4. Confirm nothing else changed (re-read the whole block 0x0000–0x0005).

## Safety (per Glenn)
- **Config time only** — never write from the poll loop.
- **Always read back** and refuse on mismatch (the code returns `0xE4`).
- Writes target only the two confirmed threshold registers.
- Do **not** click CT.exe's **Calibration** button (factory-only).

## Bench result (2026-07-26) — ✅ SOLVED: it was the FUNCTION CODE, not an unlock
First finding: an FC 0x06 (write single) was **accepted but silently ignored** (wrote 27 → stayed 30).
We suspected an unlock — but **sniffing IVY CT.exe** while it wrote a threshold showed the truth:
```
Write : 00 10 00 03 00 01 02 01 0E 2B A7      <- FC 0x10 (write MULTIPLE), reg 0x0003 = 0x010E = 270 = 27.0 mA
Read  : 00 10 00 03 00 01 F0 18               <- success, NO unlock frame anywhere
```
**The MD0630 requires FC 0x10 (write multiple registers); FC 0x06 is silently ignored. There is NO
unlock.** Fixed the driver to use `setTransmitBuffer` + `writeMultipleRegisters(addr, 1)`.

Verified on the live module (direct FC16 writes, read-back each time):
```
AC(0x0003) start : 30 mA   ->  write 4 mA : reads 4 mA (CHANGED)  ->  restore : 30 mA
DC(0x0002)       : 6 mA (unchanged)
```
Read-back matches the written value both ways; module left at the safe factory 6/30 mA.

## Status
**S3 SOLVED.** Threshold writes work via **FC 0x10 + read-back** (40 ms settle delay). `unlock_*`
members are retained but unused (no unlock needed). `applySafetyDefaults()` (DC 6 / AC 30) works.
Optional final touch: run `-DLEAKAGE_WRITE_DEFAULTS` on the ESP32 to re-confirm on-device. Next: S5/S6.
