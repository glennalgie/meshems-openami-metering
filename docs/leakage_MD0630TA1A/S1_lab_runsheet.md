# S1 — Lab runsheet: unblock real comms + derive the register map

**One goal for this session:** get the MD0630 talking for real and capture its **register map**
(offsets · function code · scaling). This is the last hard blocker — once done, S2 reads real data and
S3 (threshold writes) opens. Everything else can be done off-site.

**Time-box:** ~2 h on communication. If still stuck, switch path (§5B) or fall back (§7) — do not burn
the whole day on one wiring issue.

---

## 0. Bring / borrow (the small stuff the lab may not hand you)
- [ ] **2 × 4.7 kΩ resistors** (pull-ups — the likely fix) + a small **breadboard** (avoids soldering)
- [ ] Dupont jumpers (M-F, F-F), **multimeter**
- [ ] At the lab: **12 V bench PSU** (limit 50–100 mA) + **USB-TTL adapter** + the module
- [ ] Laptop with this repo + **CT.exe** (IVY tool) + **Device Monitoring Studio** (sniffer)

## 1. Wire it (per `wiring_S1_v2.html`)
- 12 V PSU **(+) → VCC**, **(−) → GND**
- USB-TTL **RXD ← module TX**, **TXD → module RX**  (crossed)
- **COMMON GROUND**: PSU(−) + adapter GND + module GND all tied — essential
- **Pull-ups**: 4.7 kΩ from **module TX → logic VCC**, and from **module RX → logic VCC**
  (the adapter's 3.3/5 V pin — **never** 12 V)
- If the adapter has a 3.3V/5V jumper → set **5 V** (module UART is 5 V)

## 2. Power up
- Module at 12 V. Confirm the module LED. Verify VCC = 12 V and GND continuity with the multimeter.

## 3. First contact — CT.exe (fastest sanity check)
Open **CT.exe** → COM port of the USB-TTL → **9600 · 8 · N · 1** → slave address **1** → **Read**.

- **Reads OK** → go to **§5** (capture the map). 🎉
- **No reply** → go to **§4** (troubleshoot).

## 4. Troubleshoot (in this order — stop as soon as it reads)
1. **Swap TX ↔ RX** (most common mistake).
2. Confirm **common ground** (measure 0 V between adapter GND and module GND).
3. Confirm the **pull-ups** are actually connected to the logic VCC (measure ~logic V on idle TX line).
4. Confirm **12 V** on VCC (not sagging — check PSU current limit isn't tripping).
5. Try other bauds: **19200**, then **4800**, then **38400**.
6. Confirm the **COM port** is free (close any other serial monitor / PlatformIO monitor).
7. **Never** press CT.exe's calibration button (factory-only, per IVY manual).

## 5. Capture the register map (once comms work)

### 5A. Sniffer path (PC — recommended, uses the vendor tool)
1. Start **Device Monitoring Studio** on the COM port, hex view.
2. In CT.exe: **Read** → capture the request + response frames.
3. Set a threshold (e.g. DC) then **Read** back → capture the **write** frame too.
4. Decode each frame `[addr][func][data…][CRC]`:
   - request → **start register offset** + count + **function code** (0x03 vs 0x04)
   - response → the **values**; compare raw vs the mA shown on screen → **scaling**
   - write frame → the **writable register offset**
5. Fill the table in §6.

### 5B. Our firmware path (ESP32 — second, independent confirmation)
Wire the module to the ESP32 RS-485 (HW519), then flash a build that calls
`md0630.probeRegisters()` once at boot. It tries **FC04 then FC03** from 0x0000 and prints the raw
registers **without ever writing** — it tells you which function code is live and where the values sit.
(Ask me to add a `-DLEAKAGE_PROBE` flag if you want this ready before you go.)

## 6. Deliverable — fill this (the S1 output)
```
Slave address : ____      Baud/format : 9600 8N1 (confirm)
Read function : 0x__ (03 holding / 04 input)     Write function : 0x06

Field              | Reg offset | Raw→mA scaling | R/W
-------------------|-----------|----------------|----
AC leakage current |  0x____   |   × ____       |  R
DC leakage current |  0x____   |   × ____       |  R
AC threshold       |  0x____   |   × ____       |  W/R
DC threshold       |  0x____   |   × ____       |  W/R
Modbus address     |  0x____   |      —         |  W
(Version / status) |  0x____   |      —         |  R
```
Then: set these in `md0630.reg.*` + `md0630.setReadFunction(...)`, comment out `-DLEAKAGE_MOCK`,
rebuild → **real leakage read**. This resolves the **Dev-Spec vs CT.db ordering** question for good.

## 7. Fallback (if comms still fail after the time-box)
Not a failure — it's data. Record exactly what was tried (bauds, wiring, pull-ups, voltages) and note
it. The register map can still come from **IVY** (Glenn's email). The **mock pipeline stays the working
deliverable** in the meantime — nothing downstream is blocked.

## 8. What to show Glenn afterwards (proof of progress)
Push a short update to `docs/leakage_MD0630TA1A/`:
- the **filled §6 table** (real register map), or
- the fallback note + the exact blocker.
Either way it's concrete, documented hardware progress since the last review.
```
git add docs/leakage_MD0630TA1A/  &&  git commit  &&  git push
```
