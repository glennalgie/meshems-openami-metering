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
    ~Modbus_MD0630() {};

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
    // *** CONFIRMED 2026-07-20 by live device + IVY CT.exe frames (FC03, x0.1 mA) ***
    // CT.exe reads 3 groups: 0x0000..0x0005, 0x0100, 0x0110..0x0111.
    struct RegMap {
        uint16_t dc_leakage = 0x0000;   // R    DC leakage current  (x0.1 mA)
        uint16_t ac_leakage = 0x0001;   // R    AC leakage current  (x0.1 mA)
        uint16_t dc_thresh  = 0x0002;   // R/W  DC alarm threshold  (x0.1 mA, default  60 =  6.0 mA)
        uint16_t ac_thresh  = 0x0003;   // R/W  AC alarm threshold  (x0.1 mA, default 300 = 30.0 mA)
        uint16_t version    = 0x0100;   // R    firmware version     (read 1 reg)
        uint16_t minor_ver  = 0x0111;   // R    firmware minor version
        // NOTE: which of 0x0000/0x0001 is DC vs AC assumed from DC-before-AC ordering
        // (matches the threshold layout); confirm by inducing a known DC/AC leak.
    };
    RegMap reg;                         // edit fields to re-map when offsets confirmed
    float  ma_scale = 0.1f;             // raw register value -> mA (Glenn spec: 1 unit = 0.1 mA)

    // ===================================================================
    // 3. Mock mode — "proceed with mock reads for now" (Glenn)
    //    Runs the full pipeline with no hardware. MOCK_RAMP = test bench for
    //    the leakage-isolation schema (simulated rising leakage per phase).
    // ===================================================================
    enum MockMode : uint8_t { MOCK_OFF, MOCK_STEADY, MOCK_RAMP };
    void     setMock(MockMode m) { mock = m; }
    MockMode getMock() const     { return mock; }
    void     setMockSteady(float ac_mA_, float dc_mA_);
    // ramp: start value, climb rate (mA/s) and ceiling for each channel
    void     setMockRamp(float ac_start, float ac_rate, float ac_ceil,
                         float dc_start, float dc_rate, float dc_ceil);

    // ===================================================================
    // 4. Poll (real or mock) + safe register probe
    // ===================================================================
    uint8_t  poll();                       // read AC + DC leakage; 0 (ku8MBSuccess) = OK
    void     probeRegisters();             // SAFE reads (FC04 then FC03) before any write

    uint8_t  get_modbus_address();
    void     set_modbus_address(uint8_t addr);

    // confirmed default safety thresholds (datasheet)
    static constexpr float AC_THRESHOLD_MA = 30.0f;
    static constexpr float DC_THRESHOLD_MA = 6.0f;

    float    getAcLeakage_mA();
    float    getDcLeakage_mA();
    uint16_t getFailCount();
    uint16_t getSuccessCount();

  private:
    uint8_t  read_regs(uint16_t addr, uint8_t count);  // dispatch to FC03 or FC04
    uint8_t  poll_real();
    uint8_t  poll_mock();

    ReadFC   read_fc;
    MockMode mock;
    // mock ramp state
    float    mock_ac, mock_ac_rate, mock_ac_ceil;
    float    mock_dc, mock_dc_rate, mock_dc_ceil;
    unsigned long mock_last_ms;

    uint8_t  modbus_address;
    float    ac_mA;
    float    dc_mA;
    unsigned long timestamp_last_report;
    unsigned long timestamp_last_failure;
    uint16_t fail_count;
    uint16_t success_count;
};
