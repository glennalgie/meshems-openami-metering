#pragma once
//
// Driver for the IVY MD0630 (e.g. MD0630TA1A-1) Type-B residual-current / leakage monitor
// over RS-485 Modbus RTU. Reports AC and DC leakage current in milliamps.
// Modelled on Modbus_SHT20 (include/metering/modbus_sht20.h).
//
// Confirmed module specs (IVY datasheet / product pages):
//   - Type B RCD/RCM: AC + DC residual current
//   - Default thresholds: DC 6 mA, AC 30 mA
//   - Range: DC 2-15 mA, AC 3-100 mA
//   - Communication: Modbus RTU over UART, 9600 8N1, factory slave ID 0x01
//
// ---------------------------------------------------------------------------
// DESIGN NOTE (Glenn, "path of least blockage"):
//   The exact register map is NOT yet confirmed. So this driver is built to be
//   UN-blocked:
//     1. CONFIGURABLE register offsets (RegMap reg) + CONFIGURABLE function
//        code (holding FC03 / input FC04) -> change one field when the real
//        map is known (from probeRegisters() on real hardware, or IVY's reply).
//     2. MOCK mode -> the whole pipeline (data model, MQTT, Insights) runs with
//        no hardware. MOCK_RAMP simulates a rising leakage, which is exactly the
//        test bench for the MQTT leakage-isolation schema (multi-EMS coordination).
//     3. probeRegisters() -> SAFE reads (FC04 then FC03) BEFORE any write, so we
//        never write to an unknown offset and corrupt calibration.
//
//   Default RegMap below = the "Preliminary Modbus Development Spec" hypothesis.
//   ** It differs from the CT.db-derived order — treat as UNCONFIRMED. **
// ---------------------------------------------------------------------------
//
#include <metering/modbus_master.h>

class Modbus_MD0630 : public ModbusMaster {
  public:
    Modbus_MD0630();
    virtual ~Modbus_MD0630() {};

    // ===================================================================
    // 1. Function code — "holding or input registers reads" (Glenn)
    // ===================================================================
    enum ReadFC : uint8_t { FC_INPUT = 4, FC_HOLDING = 3 };
    void   setReadFunction(ReadFC fc) { read_fc = fc; }
    ReadFC getReadFunction() const    { return read_fc; }

    // ===================================================================
    // 2. Configurable register map — *** UNCONFIRMED (hypothesis) ***
    //    Defaults from the Preliminary Modbus Dev Spec. Overwrite any field
    //    once the real offsets are known (probeRegisters() / IVY protocol doc).
    // ===================================================================
    // *** CONFIRMED 2026-07-20 by live device + IVY CT.exe frames (FC03) ***
    // CT.exe reads 3 groups: 0x0000..0x0005, 0x0100, 0x0110..0x0111.
    // *** CONFIRMED 2026-08-04 by DC resistor-ladder bench (Glenn's emulator): ***
    //   - DC/AC map confirmed: a known DC leak moves 0x0000 while 0x0001 stays 0
    //     -> DC = 0x0000, AC = 0x0001.
    //   - LEAKAGE-CURRENT registers are x0.01 mA (NOT x0.1): injecting 5.06 / 7.13 /
    //     10.13 mA read raw 513 / 719 / 1018 (raw x 0.01 = 5.13 / 7.19 / 10.18 mA,
    //     ~1% match over 3 points, all within the 2-15 mA sensor range).
    //   - THRESHOLD registers stay x0.1 mA (CT.exe frames: 003C=6.0 mA, 012C=30.0 mA).
    // *** VENDOR-CONFIRMED 2026-08-12 (IVY official protocol reply) ***
    //   - Register map + both scales confirmed (currents x0.01 mA, thresholds x0.1 mA).
    //   - 0x0100 = Modbus slave address (R/W); 0x0111 = firmware version (R).
    //   - Absolute value ONLY: no polarity/sign/phase-angle register (IEC 62955)
    //     -> leakage direction is NOT observable from this sensor.
    //   - No Modbus status/self-test register: a trip shows ONLY on the hardware
    //     DO/AO/DA pins (this is what the S6-A alarm-sense path reads).
    //   - Comm-enabled variant = MD0630T41A; the "-1" suffix omits serial comms.
    struct RegMap {
        uint16_t dc_leakage = 0x0000;   // R    DC leakage current  (x0.01 mA)  [bench-confirmed]
        uint16_t ac_leakage = 0x0001;   // R    AC leakage current  (x0.01 mA)  [bench-confirmed]
        uint16_t dc_thresh  = 0x0002;   // R/W  DC alarm threshold  (x0.1 mA, default  60 =  6.0 mA)
        uint16_t ac_thresh  = 0x0003;   // R/W  AC alarm threshold  (x0.1 mA, default 300 = 30.0 mA)
        uint16_t version    = 0x0100;   // (IVY: 0x0100 is the R/W slave-address reg; reads 1 = default addr)
        uint16_t minor_ver  = 0x0111;   // R    firmware version (IVY-confirmed)
    };
    RegMap reg;                         // edit fields to re-map when offsets confirmed
    float  leak_scale = 0.01f;          // leakage-current raw -> mA (bench-confirmed 2026-08-04)
    float  ma_scale   = 0.1f;           // threshold raw -> mA (Glenn spec: 1 unit = 0.1 mA)

    // ===================================================================
    // 3. Mock: the base class holds NO mock code/data. The mock lives in the
    //    Modbus_MD0630_Mock sub-class below, which overrides poll() to
    //    synthesise leakage (Doug's suggestion; keeps this class device-only).
    // ===================================================================

    // ===================================================================
    // 4. Poll (real or mock) + safe register probe
    // ===================================================================
    virtual uint8_t poll();                // read AC + DC leakage; 0 (ku8MBSuccess) = OK  (mock overrides)
    void     probeRegisters();             // SAFE reads (FC04 then FC03) before any write

    uint8_t  get_modbus_address();
    void     set_modbus_address(uint8_t addr);

    // confirmed default safety thresholds (datasheet)
    static constexpr float AC_THRESHOLD_MA        = 30.0f;
    static constexpr float DC_THRESHOLD_MA        = 6.0f;
    static constexpr float AC_THRESHOLD_EXPORT_MA = 27.0f;  // reverse power flow (S6, Achim/Gismo)

    // ===================================================================
    // 5. CONFIG-TIME threshold writes (S3) — FC06, unlock → write → read-back
    //    *** CONFIG TIME ONLY — never call from the poll loop ***
    // ===================================================================
    enum Channel : uint8_t { CH_DC, CH_AC };

    // Optional unlock before a write. CT.exe shows an "unLock ID" step; the exact
    // sequence is NOT yet known — derive it by sniffing a CT.exe "Set" and fill
    // these in. Until then leave unlock_required=false and try a direct write
    // (if it returns an exception, unlock is needed).
    bool     unlock_required = false;
    uint16_t unlock_reg      = 0x0000;     // TODO: from CT.exe write-frame sniff
    uint16_t unlock_value    = 0x0000;     // TODO: from CT.exe write-frame sniff

    // Write a threshold in mA (raw = mA / ma_scale, i.e. mA×10), then READ BACK
    // and confirm. Returns ku8MBSuccess only if the read-back matches. Only ever
    // writes reg.dc_thresh / reg.ac_thresh (never an unknown register).
    uint8_t  writeThreshold_mA(Channel ch, float mA);

    // Read one threshold back, in mA (-1 on failure).
    float    readThreshold_mA(Channel ch);

    // Seed life-safety defaults (DC 6 mA, AC 30 mA). Call ONCE at commissioning.
    uint8_t  applySafetyDefaults();

    float    getAcLeakage_mA();
    float    getDcLeakage_mA();
    uint16_t getFailCount();
    uint16_t getSuccessCount();

  protected:
    // Shared telemetry state. Written by the real poll() here, or by a mock
    // subclass's poll() override (Modbus_MD0630_Mock) which injects synthesised
    // readings. Protected so no mock code has to live in this base class.
    float    ac_mA;
    float    dc_mA;
    uint16_t success_count;
    unsigned long timestamp_last_report;

  private:
    uint8_t  read_regs(uint16_t addr, uint8_t count);  // dispatch to FC03 or FC04
    uint8_t  unlock();                                  // optional pre-write unlock (S3)
    uint8_t  poll_real();

    ReadFC   read_fc;
    uint8_t  modbus_address;
    unsigned long timestamp_last_failure;
    uint16_t fail_count;
};

// ---------------------------------------------------------------------------
// Mock sub-class (Doug's suggestion). The base class above holds no mock
// code/data; this sub-class overrides poll() to synthesise leakage. The
// constructor seeds a rising ramp that crosses the life-safety thresholds
// (AC 30 / DC 6 mA) at ~60 s, so it produces data out of the box. Extend here
// for special testable behaviours (e.g. inject spikes / random events).
// ---------------------------------------------------------------------------
class Modbus_MD0630_Mock : public Modbus_MD0630 {
  public:
    enum MockMode : uint8_t { MOCK_STEADY, MOCK_RAMP };

    Modbus_MD0630_Mock() {
        setRampData(0.0f, 0.5f, 45.0f,    // AC: 0 -> 45 mA @ 0.5 mA/s
                    0.0f, 0.1f,  9.0f);    // DC: 0 ->  9 mA @ 0.1 mA/s
    }

    // Repopulate this mock directly (thin, mock-only entry points).
    void setSteadyData(float ac_mA_, float dc_mA_) {
        mock_ac = ac_mA_; mock_dc = dc_mA_; mode_ = MOCK_STEADY;
    }
    void setRampData(float ac_start, float ac_rate, float ac_ceil,
                     float dc_start, float dc_rate, float dc_ceil) {
        mock_ac = ac_start; mock_ac_rate = ac_rate; mock_ac_ceil = ac_ceil;
        mock_dc = dc_start; mock_dc_rate = dc_rate; mock_dc_ceil = dc_ceil;
        mock_last_ms = 0;                 // 0 => first poll() starts the ramp clock
        mode_ = MOCK_RAMP;
    }

    uint8_t poll() override;              // synthesise leakage (defined in the .cpp)

  private:
    MockMode      mode_        = MOCK_RAMP;
    float         mock_ac = 0, mock_ac_rate = 0, mock_ac_ceil = 0;
    float         mock_dc = 0, mock_dc_rate = 0, mock_dc_ceil = 0;
    unsigned long mock_last_ms = 0;
};
