//
// IVY MD0630 (MD0630TA1A-1) Type-B residual-current / leakage monitor driver — implementation.
// Modelled on src/metering/modbus_sht20.cpp.
//
// Strategy (Glenn, "path of least blockage"): configurable offsets + function
// code + a mock mode so the pipeline runs before the register map is confirmed.
//
#include <metering/modbus_md0630.h>
#include <Arduino.h>

static const char* mbErrStr(uint8_t err) {
    switch (err) {
        case 0x01: return "ILLEGAL_FUNCTION";
        case 0x02: return "ILLEGAL_DATA_ADDRESS";
        case 0x03: return "ILLEGAL_DATA_VALUE";
        case 0x04: return "SLAVE_DEVICE_FAILURE";
        case 0xE0: return "INVALID_SLAVE_ID";
        case 0xE1: return "INVALID_FUNCTION";
        case 0xE2: return "TIMEOUT";
        case 0xE3: return "INVALID_CRC";
        default:   return "UNKNOWN";
    }
}

Modbus_MD0630::Modbus_MD0630() {
    read_fc                = FC_HOLDING; // confirmed: CT.exe uses FC03 (FC04 also answers)
    mock                   = MOCK_OFF;
    mock_ac = mock_dc      = 0.0f;
    mock_ac_rate = mock_dc_rate = 0.0f;
    mock_ac_ceil = mock_dc_ceil = 0.0f;
    mock_last_ms           = 0;
    modbus_address         = 1;          // datasheet factory default slave ID 0x01
    ac_mA                  = 0.0f;
    dc_mA                  = 0.0f;
    fail_count             = 0;
    success_count          = 0;
    timestamp_last_report  = 0;
    timestamp_last_failure = 0;
}

uint8_t  Modbus_MD0630::get_modbus_address()            { return modbus_address; }
void     Modbus_MD0630::set_modbus_address(uint8_t addr){ modbus_address = addr; }
float    Modbus_MD0630::getAcLeakage_mA()               { return ac_mA; }
float    Modbus_MD0630::getDcLeakage_mA()               { return dc_mA; }
uint16_t Modbus_MD0630::getFailCount()                  { return fail_count; }
uint16_t Modbus_MD0630::getSuccessCount()               { return success_count; }

// Dispatch a read to the configured function code (FC03 holding or FC04 input).
uint8_t Modbus_MD0630::read_regs(uint16_t addr, uint8_t count) {
    if (read_fc == FC_HOLDING) return readHoldingRegisters(addr, count);
    return readInputRegisters(addr, count);
}

// ---- mock configuration --------------------------------------------------
void Modbus_MD0630::setMockSteady(float ac_mA_, float dc_mA_) {
    mock_ac = ac_mA_;
    mock_dc = dc_mA_;
    mock    = MOCK_STEADY;
}

void Modbus_MD0630::setMockRamp(float ac_start, float ac_rate, float ac_ceil,
                                float dc_start, float dc_rate, float dc_ceil) {
    mock_ac = ac_start; mock_ac_rate = ac_rate; mock_ac_ceil = ac_ceil;
    mock_dc = dc_start; mock_dc_rate = dc_rate; mock_dc_ceil = dc_ceil;
    mock_last_ms = millis();
    mock = MOCK_RAMP;
}

// ---- poll ----------------------------------------------------------------
uint8_t Modbus_MD0630::poll() {
    return (mock == MOCK_OFF) ? poll_real() : poll_mock();
}

// Real read: AC and DC leakage via the configured function code + offsets.
// Read separately so the two offsets need not be consecutive.
uint8_t Modbus_MD0630::poll_real() {
    uint8_t  rAc   = read_regs(reg.ac_leakage, 1);
    uint16_t acRaw = (rAc == ku8MBSuccess) ? getResponseBuffer(0) : 0;
    uint8_t  rDc   = read_regs(reg.dc_leakage, 1);
    uint16_t dcRaw = (rDc == ku8MBSuccess) ? getResponseBuffer(0) : 0;

    if (rAc == ku8MBSuccess && rDc == ku8MBSuccess) {
        success_count++;
        timestamp_last_report = millis();
        ac_mA = acRaw * ma_scale;
        dc_mA = dcRaw * ma_scale;
        Serial.printf("MD0630 [addr:%d fc:0x%02X]: AC=%.2f mA  DC=%.2f mA  (ok:%d fail:%d)\n",
                      modbus_address, read_fc, ac_mA, dc_mA, success_count, fail_count);
        return ku8MBSuccess;
    }

    fail_count++;
    timestamp_last_failure = millis();
    uint8_t err = (rAc != ku8MBSuccess) ? rAc : rDc;
    Serial.printf("MD0630 [addr:%d fc:0x%02X]: POLL FAIL  err=0x%02X (%s)  (ok:%d fail:%d)\n",
                  modbus_address, read_fc, err, mbErrStr(err), success_count, fail_count);
    return err;
}

// Mock read: synthesise plausible leakage so the pipeline runs with no hardware.
// MOCK_RAMP integrates the climb rate over elapsed time (test bench for the
// leakage-isolation schema); MOCK_STEADY holds a fixed value.
uint8_t Modbus_MD0630::poll_mock() {
    unsigned long nowms = millis();
    if (mock == MOCK_RAMP) {
        float dt = (nowms - mock_last_ms) / 1000.0f;   // seconds since last poll
        if (dt < 0) dt = 0;
        mock_ac += mock_ac_rate * dt;
        mock_dc += mock_dc_rate * dt;
        if (mock_ac > mock_ac_ceil) mock_ac = mock_ac_ceil;
        if (mock_dc > mock_dc_ceil) mock_dc = mock_dc_ceil;
    }
    mock_last_ms = nowms;
    ac_mA = mock_ac;
    dc_mA = mock_dc;
    success_count++;
    timestamp_last_report = nowms;
    Serial.printf("MD0630 [MOCK %s]: AC=%.2f mA  DC=%.2f mA  (ok:%d)\n",
                  (mock == MOCK_RAMP ? "RAMP" : "STEADY"), ac_mA, dc_mA, success_count);
    return ku8MBSuccess;
}

// Safe register verification (Preliminary Spec section 2): READ before any WRITE,
// so we never write to an unknown offset and corrupt calibration. Run once with a
// real module attached; the log tells us FC03 vs FC04 and where the values live.
void Modbus_MD0630::probeRegisters() {
    Serial.println("MD0630 PROBE: safe read verification (no writes) -----------------");

    // Step A: FC04 input registers, 4 words from 0x0000
    uint8_t rA = readInputRegisters(0x0000, 4);
    Serial.printf("  [A] FC04 readInputRegisters(0x0000,4): ");
    if (rA == ku8MBSuccess) {
        Serial.printf("OK  %04X %04X %04X %04X\n",
                      getResponseBuffer(0), getResponseBuffer(1),
                      getResponseBuffer(2), getResponseBuffer(3));
    } else {
        Serial.printf("err=0x%02X (%s)\n", rA, mbErrStr(rA));
    }

    // Step B: FC03 holding registers, 7 words from 0x0000
    uint8_t rB = readHoldingRegisters(0x0000, 7);
    Serial.printf("  [B] FC03 readHoldingRegisters(0x0000,7): ");
    if (rB == ku8MBSuccess) {
        Serial.printf("OK  %04X %04X %04X %04X %04X %04X %04X\n",
                      getResponseBuffer(0), getResponseBuffer(1), getResponseBuffer(2),
                      getResponseBuffer(3), getResponseBuffer(4), getResponseBuffer(5),
                      getResponseBuffer(6));
        Serial.println("      hint: if map is holding, offs 4/5 = thresholds"
                       " (012C=30.0mA AC, 003C=6.0mA DC)");
    } else {
        Serial.printf("err=0x%02X (%s)\n", rB, mbErrStr(rB));
    }

    if (rA == ku8MBSuccess && rB != ku8MBSuccess)
        Serial.println("  => telemetry is INPUT regs: setReadFunction(FC_INPUT)");
    if (rB == ku8MBSuccess && rA != ku8MBSuccess)
        Serial.println("  => telemetry is HOLDING regs: setReadFunction(FC_HOLDING)");
    if (rA == ku8MBSuccess && rB == ku8MBSuccess)
        Serial.println("  => both answer: induce a small leakage, re-read, and see which words move");
    Serial.println("MD0630 PROBE: done ----------------------------------------------");
}

/* ------------------------------------------------------------------------
 INTEGRATION (add in src/metering/modbus_master.cpp once ready):

   #include <metering/modbus_md0630.h>
   #include <metering/leakage_model_ivy41a.h>

   Modbus_MD0630 md0630;
   LeakageModel  leakageModel;
   #define MD0630_ADDR 100            // Glenn's node numbering (100/101/102 per phase)

   // in setup_modbus_clients():
   md0630.set_modbus_address(MD0630_ADDR);
   md0630.begin(MD0630_ADDR, _modbus1);
   leakageModel.acSinusoidal.threshold_mA = Modbus_MD0630::AC_THRESHOLD_MA;  // 30 mA
   leakageModel.dc.threshold_mA           = Modbus_MD0630::DC_THRESHOLD_MA;  //  6 mA (Type B)

   // --- choose ONE path of least blockage ---
   // (a) NO hardware yet -> mock a rising leakage to exercise the whole pipeline:
   md0630.setMockRamp(/ac/ 0, 2.0f, 45.0f,   /dc/ 0, 0.4f, 9.0f);  // mA/s, ceilings
   // (b) real module attached -> confirm the map ONCE, then read live:
   // md0630.probeRegisters();                 // safe reads, prints FC03/FC04 + words
   // md0630.setReadFunction(Modbus_MD0630::FC_INPUT);   // or FC_HOLDING
   // md0630.reg.ac_leakage = 0x00; md0630.reg.dc_leakage = 0x01;  // set real offsets

   // in the poll cycle (loop_modbus_master):
   if (md0630.poll() == ku8MBSuccess) {
       leakageModel.updateAll(md0630.getAcLeakage_mA(), 0.0f, md0630.getDcLeakage_mA());
   }
   // leakageModel is then published by mqtt_publish_Leakage() -> subpanel_RCMleaks

 NOTE: the < 300 ms life-safety trip is done by the module's HARDWARE fault output,
       not by this Modbus polling loop (which is for monitoring/reporting).
 ------------------------------------------------------------------------ */
