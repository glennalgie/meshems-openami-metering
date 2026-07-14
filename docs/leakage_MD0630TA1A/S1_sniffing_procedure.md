# S1 — Sniffing procedure: derive the MD0630 Modbus register map

**Goal:** while the IVY PC tool `CT.exe` talks to the MD0630T41A-1, capture the raw serial (Modbus RTU)
frames, decode them, and extract the **register offsets + function codes + scaling** → this unblocks the
ESP32 driver (S2).

---

## 0. Shopping / lab checklist (get Monday)
- [ ] **Bench DC power supply** (set to **12 V**, current limit ~50–100 mA)
- [ ] **USB-to-TTL serial adapter** (CP2102 / CH340 / FT232) — set jumper to **5 V** (module UART = 5 V)
- [ ] **2 × 4.7 kΩ resistors** (pull-ups for TX/RX — datasheet requirement)
- [ ] Dupont jumper wires (M-F / F-F) + small breadboard
- [ ] Multimeter
- [ ] _(optional, cleanest)_ **Logic analyzer** (Saleae / USB 8-ch) as an alternative to software sniffing
- Software (install this weekend): the USB-TTL **driver** + a **serial sniffer**
  (**HHD Free Serial Port Monitor**) + `CT.exe` (already in the IVY zip).

---

## 1. Wiring (USB-TTL path — simplest for S1)
The module speaks **Modbus over UART at 5 V** on its **7-pin header** (the 6-wire cable has no TX/RX).
For S1 we use only **VCC, GND, TX, RX** (ignore AD/AO/DO).

```
   12V bench PSU
     (+)───────────────► VCC   (MD0630)
     (−)──────┬────────► GND
              │
   USB-TTL    │
   adapter    │
     GND──────┘ (common ground = PSU− + adapter GND + module GND)   ← ESSENTIAL
     RXD ◄──────────────  TX  (module TX  → adapter RX)   ← crossed!
     TXD ──────────────►  RX  (module RX  ← adapter TX)   ← crossed!
     5V  ──[4.7k]──────►  TX  (pull-up)
     5V  ──[4.7k]──────►  RX  (pull-up)
```
Key points:
- **Common ground** between PSU, adapter and module — without it, nothing works.
- **TX/RX are crossed** (module TX → adapter RXD, module RX → adapter TXD).
- **Pull-ups**: 4.7 kΩ from TX and RX to the adapter's **5 V** (datasheet says TX/RX need pull-ups).
- Set the USB-TTL adapter to **5 V** logic (datasheet: UART output = 5 V).

_Alternative (USB-RS485 path):_ module TX/RX/GND → **RS485-TTL module** TTL side (TXD/RXD/GND) →
its A+/B- → **USB-RS485 adapter** → PC. (Uses the RS485-TTL module you already have.)

---

## 2. Serial settings
Open `CT.exe` → **Serial port** dialog. Use the **exact values shown in the PC manual screenshot**
(`PC OPERATION INSTRUCITIONS…docx`, step 3). Note the **COM port** from Windows Device Manager.
Modbus RTU is typically **9600 8N1** or **9600 8E1** — if unsure, start there.
Record here once known:
```
COM port : COM__       Baud : ______    Data bits : 8    Parity : ____    Stop : __
```

---

## 3. Capture procedure
1. Power the module at **12 V**, plug the USB-TTL adapter into the PC.
2. Start **HHD Serial Port Monitor** → select the COM port → **Start** capture (choose **hex/binary** view).
3. Open **`CT.exe`** → set the COM port + serial params → click **Read (召读)**.
   → sniffer shows a **request** frame then a **response** (Modbus ID, version, thresholds).
4. Apply a small leakage current if the bench allows, click **Read** again → captures the **leakage** values.
5. Set a threshold (e.g. DC) → click **Set** → then **Read** to confirm.
   → captures a **WRITE** frame (this reveals the writable register offset).
6. **Export / save** the sniffer log.

---

## 4. Decode a Modbus RTU frame
Frame = `[slave addr 1B] [function 1B] [data …] [CRC 2B]`

| Function | Request bytes | What it tells us |
|---|---|---|
| **0x03** read holding regs | addr, 03, **startHi startLo**, countHi countLo, CRC | **start register offset** + count |
| **0x04** read input regs | addr, 04, **startHi startLo**, countHi countLo, CRC | offset + count |
| **0x06** write single reg | addr, 06, **regHi regLo**, **valHi valLo**, CRC | writable **register offset** + written value |
| **0x10** write multiple | addr, 10, startHi startLo, count, byteCount, data…, CRC | offsets + values |
| Response (read) | addr, func, **byteCount**, **data…**, CRC | the **values** at those registers |

**How to get the map:**
1. In each **read** request, note the **start register offset** and count.
2. In the matching **response**, read the data words and line them up with the CT.exe display
   (Modbus ID, Version, SmallVersion, DC/AC Threshold, DC/AC LeakageCurrent).
3. Compare the **raw register value** vs the **mA value shown on screen** → that ratio = the **scaling**
   (e.g. raw 60 shown as 6.0 mA ⇒ scale ×0.1).
4. The **write** frame gives the writable register offset (should match ModbusID / DC / AC threshold).

**Example (illustrative only):**
```
TX (PC→module):  01 03 00 05 00 02 D4 0B
                 └slave 1  └FC03  └reg 0x0005  └count 2   └CRC
RX (module→PC):  01 03 04 00 3C 00 1E 7A 51
                 └slave 1 └FC03 └4 bytes └0x003C(=60) └0x001E(=30)   └CRC
   → reg 0x0005 = 60, if screen shows 6.0 mA ⇒ DCLeakageCurrent at 0x0005, scale ×0.1
```

---

## 5. Result to fill (the S1 deliverable)
```
Slave/node address : ____      Baud/format : __________
Read function code : 0x__      Write function code : 0x__

Field              | Reg offset | Raw→value scaling | R/W
-------------------|-----------|-------------------|----
ModbusID           |  0x____   |                   |  W
Version            |  0x____   |                   |  R
SmallVersion       |  0x____   |                   |  R
DCThreshold        |  0x____   |   × ____ (mA)     |  W/R
ACThreshold        |  0x____   |   × ____ (mA)     |  W/R
DCLeakageCurrent   |  0x____   |   × ____ (mA)     |  R
ACLeakageCurrent   |  0x____   |   × ____ (mA)     |  R
```
→ paste this into `README.md` §2 and we finalize `modbus_md0630.*` (S2).

---

## 6. Troubleshooting
- **No response:** swap **TX↔RX**; check **common GND**; check **12 V** on VCC; verify **baud/parity**;
  confirm the **pull-ups** are in place.
- **Garbage bytes:** wrong **baud** — try 9600, then 19200 / 4800.
- **CT.exe "read" fails:** wrong **COM port** or another app holds it (close other serial monitors).
- Do **not** click the **calibration** button in CT.exe (internal factory use — per the IVY manual).
- Reminder: use only VCC/GND/TX/RX now; the DO/AO/DA alarm lines are 12 V — handle later with a
  level-shifter to 3.3 V GPIO.
