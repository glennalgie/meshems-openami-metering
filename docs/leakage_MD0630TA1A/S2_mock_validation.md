# S2 — Mock validation report (IVY MD0630 leakage driver)

**Author:** Allahaddje Bonheur — IoT Fellow, 10Power (with NESL)
**Reviewer:** Glenn (supervisor)
**Date:** 2026-07-14
**Board:** ESP32-S3-DevKitC-1 · `env:esp32-s3-devkitc-1` · COM4

> Per Glenn's directive: *"Lets use configurable offset and holding or input registers reads for the
> readable rcm AC and DC leakage registers and proceed with mock reads for now of dc and ac.
> You choose the path of least blockage."*

---

## 1. What was done

The `Modbus_MD0630` driver is now **integrated into the firmware build** and validated end-to-end on
real ESP32-S3 hardware using **mock reads** — no leakage module, no 12 V supply, no register map
required. This unblocks the whole pipeline while the register offsets are still unconfirmed.

| Item | Status |
|---|---|
| Driver in build (`include/metering/` + `src/metering/`) | ✅ |
| Configurable register offsets (`md0630.reg.*`) | ✅ |
| Configurable function code (FC03 holding / FC04 input) | ✅ |
| Mock modes (STEADY / RAMP) | ✅ validated |
| Safe read verification `probeRegisters()` (read-before-write) | ✅ implemented, awaiting hardware |
| Wired into `LeakageModel` → `subpanel_RCMleaks` | ✅ |
| Modbus node **100** (Glenn's numbering) | ✅ |
| Thresholds seeded AC **30 mA** / DC **6 mA** | ✅ |

Build: **SUCCESS** — RAM 17.2 % (56 316 B), Flash 27.5 % (865 245 B).

## 2. Evidence — serial capture (mock ramp)

Ramp configured at boot: AC 0 → 45 mA @ 0.5 mA/s, DC 0 → 9 mA @ 0.1 mA/s.

```
SETUP: MODBUS: MD0630 leakage CT: address:100
SETUP: MODBUS: MD0630 in MOCK RAMP mode - no hardware required
MD0630 [MOCK RAMP]: AC=1.55 mA   DC=0.31 mA  (ok:1)
MD0630 [MOCK RAMP]: AC=4.55 mA   DC=0.91 mA  (ok:2)
MD0630 [MOCK RAMP]: AC=8.61 mA   DC=1.72 mA  (ok:3)
MD0630 [MOCK RAMP]: AC=13.61 mA  DC=2.72 mA  (ok:4)
MD0630 [MOCK RAMP]: AC=18.61 mA  DC=3.72 mA  (ok:5)
MD0630 [MOCK RAMP]: AC=23.61 mA  DC=4.72 mA  (ok:6)
MD0630 [MOCK RAMP]: AC=28.61 mA  DC=5.72 mA  (ok:7)     <- approaching thresholds
MD0630 [MOCK RAMP]: AC=43.61 mA  DC=8.72 mA  (ok:10)    <- both thresholds crossed
MD0630 [MOCK RAMP]: AC=45.00 mA  DC=9.00 mA  (ok:11)    <- ceiling, holds steady
MD0630 [MOCK RAMP]: AC=45.00 mA  DC=9.00 mA  (ok:16)
```

**Result:** 16 consecutive successful reads, 0 failures. The ramp crosses **AC 30 mA** and
**DC 6 mA**, so the `LeakageChannel::inFault` transition and the fault timestamps are exercised.

## 3. Known gap — MQTT not yet demonstrated

`mqtt_publish_Leakage()` now serialises the **live** model (`get_leakage_model()`) instead of an
empty stub, and `MQTTPublish_leakage_rate` was lowered 10 min → **10 s** for bring-up. Not yet seen
on the wire because the capture was taken away from the home network:

```
[WIFI] no matching wifi found!  ssid=Helix89
MQTT: connection lost, attempting reconnect
```

→ To be confirmed on `subpanel_RCMleaks` against the local Mosquitto broker (next session).

## 4. Why the mock is not throwaway work

`MOCK_RAMP` is the **test bench for the MQTT leakage-isolation schema**: a rising leakage per phase
is exactly the stimulus needed to exercise multi-EMS coordination (level / ramp-rate / direction
comparison, nomination of the closest EMS, ops alert) without energising a real fault.

## 5. Open questions for review

- [ ] **Register map conflict**: the Preliminary Modbus Dev Spec ordering (AC@0x0000, DC@0x0001,
      status@0x0002, id@0x0003, ACthr@0x0004, DCthr@0x0005, addr@0x0006, ×0.1 mA) **differs** from the
      `CT.db` field order (ModbusID, Version, SmallVersion, DCThreshold, ACThreshold, DCLeakage,
      ACLeakage). Currently the Dev Spec ordering is the **default**; both are unconfirmed.
      → Which should we default to pending IVY's reply?
- [ ] **FC03 vs FC04**: default is currently **FC04 (input registers)**. `probeRegisters()` will
      settle it on first contact with real hardware.
- [ ] Confirm IVY will supply the official protocol document (Glenn's email).

## 6. Next steps

1. Confirm `subpanel_RCMleaks` publishing on the local broker (home network).
2. On hardware: run `probeRegisters()` → set real offsets in `md0630.reg.*` → comment out
   `-DLEAKAGE_MOCK`.
3. S4: expand `LeakageData` + energy-change correlation cache.
4. S5: Leakage Insights Manager + MQTT LV-feeder isolation subtopic.

## 7. How to reproduce

```bash
pio run -e esp32-s3-devkitc-1 -t upload -t monitor
```
Flags in `platformio.ini`: `-DENABLE_LEAKAGE_MD0630`, `-DLEAKAGE_MOCK`.
Requires only the ESP32-S3 + USB cable.
