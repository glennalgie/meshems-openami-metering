#ifdef METER_TYPE_ATM90E32

/**
 * @file meter_atm90e32.cpp
 * @brief ATM90E32 6-channel SPI energy meter driver.
 *
 * Manages one or more CircuitSetup 6-channel board pairs.  Each board pair
 * comprises two ATM90E32 ICs (IC1 on CS1, IC2 on CS2) each measuring three
 * CT channels (A/B/C) plus a single AC voltage reference.
 *
 * Readings are placed into the shared readings[] array (data_model.h):
 *   Board 0: readings[0..5]   (IC1 A/B/C → CT1-3, IC2 A/B/C → CT4-6)
 *   Board 1: readings[6..11]  (if ATM90E32_NUM_BOARDS >= 2), etc.
 *
 * setup_atm90e32() must be called once at startup.
 * poll_atm90e32()  must be called periodically (from loop_modbus_master()).
 */

#include <SPI.h>
#include "metering/meter_atm90e32.h"
#include "metering/atm90e32_status.h"
#include "core/data_model.h"
#include "core/pins.h"

// --------------------------------------------------------------------------
// ATM90E32 IC instances — two per board (IC1 = CT1-3, IC2 = CT4-6)
// --------------------------------------------------------------------------
static ATM90E32 ic1[ATM90E32_NUM_BOARDS];
static ATM90E32 ic2[ATM90E32_NUM_BOARDS];
static bool board_ok[ATM90E32_NUM_BOARDS] = {};

/****** Chip Select Pins ******/
/*
   Each CircuitSetup board carries two ATM90E32 ICs, so two CS lines per board:
   CS1 selects the IC measuring CT1-3, CS2 the IC measuring CT4-6.

   These are flat tables declared at the full ATM90E32_MAX_BOARDS width - the
   same shape EVPE uses in src/energy/energy_meter.cpp - so adding or removing a
   board is a single number in platformio.ini and never an #ifdef here. Only the
   first ATM90E32_NUM_BOARDS entries are ever indexed; the rest cost 4 bytes
   each in flash and are never selected.

   Pin values live in pins.h and are shared with the EVPE deployment.
*/
static const int CS1[ATM90E32_MAX_BOARDS] = {ATM90E32_IC1_CS,
                                             ATM90E32_IC1_CS_1,
                                             ATM90E32_IC1_CS_2};
static const int CS2[ATM90E32_MAX_BOARDS] = {ATM90E32_IC2_CS,
                                             ATM90E32_IC2_CS_1,
                                             ATM90E32_IC2_CS_2};

/* Per-IC voltage gain, same flat layout: index = board*2 + (0 for IC1, 1 IC2).
   Each board has its own 9 V reference transformer, so these differ per board
   on a calibrated stack. See meter_atm90e32.h for how to measure them. */
static const unsigned short UGAIN[ATM90E32_MAX_BOARDS * 2] = {
    ATM90E32_UGAIN_IC0, ATM90E32_UGAIN_IC1,   // board 1
    ATM90E32_UGAIN_IC2, ATM90E32_UGAIN_IC3,   // board 2
    ATM90E32_UGAIN_IC4, ATM90E32_UGAIN_IC5,   // board 3
};

/* Per-channel software correction applied after each read. Seeded from the
   defaults in setup_atm90e32() and adjustable at runtime via
   atm90e32_set_channel_scale() - the hook for config- or MQTT-driven
   calibration, which is how EVPE feeds its cur_mul[]/pow_mul[]. */
static float cur_mul[ATM90E32_NUM_CHANNELS];
static float pow_mul[ATM90E32_NUM_CHANNELS];

/* Diagnostics captured on the last poll, one entry per IC. */
static ATM90E32IcStatus ic_status[ATM90E32_MAX_BOARDS * 2] = {};
static float            ic_temp[ATM90E32_MAX_BOARDS * 2];

// --------------------------------------------------------------------------
// setup_atm90e32
// --------------------------------------------------------------------------

/**
 * Initialise all ATM90E32 ICs on the SPI bus.
 *
 * Line frequency, PGA gain and current gain are shared by every IC; the
 * VOLTAGE gain is taken per IC from UGAIN[], because each board in a stack has
 * its own reference transformer and therefore its own calibration.
 *
 * A short delay after each board's begin() allows the ATM90E32 DSP to stabilise
 * before measurements are requested.
 */
void setup_atm90e32() {
    Serial.printf("SETUP: ATM90E32: initialising %d board(s), %d channels\n",
                  ATM90E32_NUM_BOARDS, ATM90E32_NUM_CHANNELS);

    // Seed the software correction tables; runtime calibration may override
    // individual channels later via atm90e32_set_channel_scale().
    for (int ch = 0; ch < ATM90E32_NUM_CHANNELS; ch++) {
        cur_mul[ch] = ATM90E32_CUR_MUL_DEFAULT;
        pow_mul[ch] = ATM90E32_POW_MUL_DEFAULT;
    }
    for (int ic = 0; ic < ATM90E32_MAX_BOARDS * 2; ic++) {
        ic_temp[ic] = NAN;
    }

    for (int i = 0; i < ATM90E32_NUM_BOARDS; i++) {
        // IC1 handles CT1/CT2/CT3 with voltage reference on Phase A input.
        ic1[i].begin(CS1[i],
                     ATM90E32_LINE_FREQ,
                     ATM90E32_PGA_GAIN,
                     UGAIN[i * 2 + 0],
                     ATM90E32_CURRENT_GAIN,
                     ATM90E32_CURRENT_GAIN,
                     ATM90E32_CURRENT_GAIN);

        // IC2 handles CT4/CT5/CT6 with a second voltage reference on Phase A.
        ic2[i].begin(CS2[i],
                     ATM90E32_LINE_FREQ,
                     ATM90E32_PGA_GAIN,
                     UGAIN[i * 2 + 1],
                     ATM90E32_CURRENT_GAIN,
                     ATM90E32_CURRENT_GAIN,
                     ATM90E32_CURRENT_GAIN);

        Serial.printf("SETUP: ATM90E32: board %d  CS1=%d ugain=%u  CS2=%d ugain=%u\n",
                      i, CS1[i], UGAIN[i * 2 + 0], CS2[i], UGAIN[i * 2 + 1]);

        delay(200); // allow ATM90E32 DSP to settle
    }

    Serial.println("SETUP: ATM90E32: ready");
}

/**
 * Override the software correction for one CT channel.
 * Pass NAN to leave a multiplier unchanged. A negative pow_mul flips the sign
 * of active/apparent power, which is the fix for a CT clamped on backwards.
 */
void atm90e32_set_channel_scale(int channel, float cur, float pow) {
    if (channel < 0 || channel >= ATM90E32_NUM_CHANNELS) return;
    if (!isnan(cur)) cur_mul[channel] = cur;
    if (!isnan(pow)) pow_mul[channel] = pow;
}

// --------------------------------------------------------------------------
// poll_atm90e32
// --------------------------------------------------------------------------

/**
 * Poll all ATM90E32 ICs and populate readings[] with fresh measurements.
 *
 * For each board:
 *   - Reads the IC1 system status to check for a live SPI connection.
 *     If the status word is 0x0000 or 0xFFFF the IC is not responding and
 *     that board is skipped (its readings[] entries are left unchanged).
 *   - Reads voltage, current, active power, apparent power, and power factor
 *     for each of the six CT channels.
 *   - Reads frequency and IC temperature once from IC1 per board and copies
 *     them into all six channel entries for that board.
 *
 * Sign of real power: the ATM90E32 current registers are unsigned, so a
 * negative active power value means the CT is on an export (generation)
 * circuit; the current reading is flipped to match.
 */
void poll_atm90e32() {
    for (int board = 0; board < ATM90E32_NUM_BOARDS; board++) {
        const int ic_a = board * 2;      // IC1 slot in the per-IC tables
        const int ic_b = board * 2 + 1;  // IC2 slot

        // Validate SPI comms — 0x0000 or 0xFFFF indicates no response.
        unsigned short sys0 = ic1[board].GetSysStatus0();
        board_ok[board] = !(sys0 == 0x0000 || sys0 == 0xFFFF);
        if (sys0 == 0x0000 || sys0 == 0xFFFF) {
            #ifdef ENABLE_DEBUG
            Serial.printf("ATM90E32 board %d: no response (SysStatus0=0x%04X), skipping\n",
                          board, sys0);
            #endif
            ic_temp[ic_a] = NAN;
            ic_temp[ic_b] = NAN;
            continue;
        }

        // Full diagnostic status for both ICs.
        //
        // sys0/sys1 come from the latched interrupt registers EMMIntState0/1
        // and are cleared immediately after reading, so each poll reports only
        // what happened since the previous poll — a sag flag means "sagged in
        // the last cycle", not "sagged once, hours ago". The clear depends on
        // ClearSysStatus0/1, which is a local addition to lib/ATM90E32.
        //
        // meter0/meter1 come from EMMState0/1, which are live state rather than
        // latched events and so are never cleared.
        ic_status[ic_a].sys0   = sys0;
        ic_status[ic_a].sys1   = ic1[board].GetSysStatus1();
        ic_status[ic_a].meter0 = ic1[board].GetMeterStatus0();
        ic_status[ic_a].meter1 = ic1[board].GetMeterStatus1();
        ic1[board].ClearSysStatus0();
        ic1[board].ClearSysStatus1();

        ic_status[ic_b].sys0   = ic2[board].GetSysStatus0();
        ic_status[ic_b].sys1   = ic2[board].GetSysStatus1();
        ic_status[ic_b].meter0 = ic2[board].GetMeterStatus0();
        ic_status[ic_b].meter1 = ic2[board].GetMeterStatus1();
        ic2[board].ClearSysStatus0();
        ic2[board].ClearSysStatus1();

        // Per-IC readings — indexed as [0..5] within this board.
        float voltage[6];
        float current[6];
        float active_power[6];
        float apparent_power[6];
        float power_factor[6];

        // IC1 — channels 0/1/2 (CT1/CT2/CT3 on this board)
        voltage[0]       = ic1[board].GetLineVoltageA();
        voltage[1]       = ic1[board].GetLineVoltageB();
        voltage[2]       = ic1[board].GetLineVoltageC();
        current[0]       = ic1[board].GetLineCurrentA();
        current[1]       = ic1[board].GetLineCurrentB();
        current[2]       = ic1[board].GetLineCurrentC();
        active_power[0]  = ic1[board].GetActivePowerA();
        active_power[1]  = ic1[board].GetActivePowerB();
        active_power[2]  = ic1[board].GetActivePowerC();
        apparent_power[0] = ic1[board].GetApparentPowerA();
        apparent_power[1] = ic1[board].GetApparentPowerB();
        apparent_power[2] = ic1[board].GetApparentPowerC();
        power_factor[0]  = ic1[board].GetPowerFactorA();
        power_factor[1]  = ic1[board].GetPowerFactorB();
        power_factor[2]  = ic1[board].GetPowerFactorC();

        // IC2 — channels 3/4/5 (CT4/CT5/CT6 on this board)
        voltage[3]       = ic2[board].GetLineVoltageA();
        voltage[4]       = ic2[board].GetLineVoltageB();
        voltage[5]       = ic2[board].GetLineVoltageC();
        current[3]       = ic2[board].GetLineCurrentA();
        current[4]       = ic2[board].GetLineCurrentB();
        current[5]       = ic2[board].GetLineCurrentC();
        active_power[3]  = ic2[board].GetActivePowerA();
        active_power[4]  = ic2[board].GetActivePowerB();
        active_power[5]  = ic2[board].GetActivePowerC();
        apparent_power[3] = ic2[board].GetApparentPowerA();
        apparent_power[4] = ic2[board].GetApparentPowerB();
        apparent_power[5] = ic2[board].GetApparentPowerC();
        power_factor[3]  = ic2[board].GetPowerFactorA();
        power_factor[4]  = ic2[board].GetPowerFactorB();
        power_factor[5]  = ic2[board].GetPowerFactorC();

        // Frequency is a per-board property (both ICs share the AC reference);
        // die temperature is per IC and worth tracking separately, since a hot
        // IC is an early warning of a loose terminal or an overloaded feeder.
        float freq = ic1[board].GetFrequency();
        ic_temp[ic_a] = ic1[board].GetTemperature();
        ic_temp[ic_b] = ic2[board].GetTemperature();
        float temp = ic_temp[ic_a];

        // Energy accumulators — per-channel absolute Wh since last IC reset.
        // GetImportEnergyA/B/C and GetExportEnergyA/B/C return Wh; divide by
        // 1000 to store in the data model's kWh convention.
        // TODO: add a software interval-energy accumulator (delta kWh between
        // polls) for 15-min / hourly / daily billing intervals.
        // kWh values — double→float truncation done after dividing by 1000.
        float import_energy_kwh[6];
        float export_energy_kwh[6];
        // GetImportEnergy*/GetExportEnergy* return double (Wh).
        // Divide in double arithmetic before truncating to float to preserve
        // sub-Wh resolution up to at least ~16 GWh (double mantissa limit).
        import_energy_kwh[0] = (float)(ic1[board].GetImportEnergyA() / 1000.0);
        import_energy_kwh[1] = (float)(ic1[board].GetImportEnergyB() / 1000.0);
        import_energy_kwh[2] = (float)(ic1[board].GetImportEnergyC() / 1000.0);
        import_energy_kwh[3] = (float)(ic2[board].GetImportEnergyA() / 1000.0);
        import_energy_kwh[4] = (float)(ic2[board].GetImportEnergyB() / 1000.0);
        import_energy_kwh[5] = (float)(ic2[board].GetImportEnergyC() / 1000.0);
        export_energy_kwh[0] = (float)(ic1[board].GetExportEnergyA() / 1000.0);
        export_energy_kwh[1] = (float)(ic1[board].GetExportEnergyB() / 1000.0);
        export_energy_kwh[2] = (float)(ic1[board].GetExportEnergyC() / 1000.0);
        export_energy_kwh[3] = (float)(ic2[board].GetExportEnergyA() / 1000.0);
        export_energy_kwh[4] = (float)(ic2[board].GetExportEnergyB() / 1000.0);
        export_energy_kwh[5] = (float)(ic2[board].GetExportEnergyC() / 1000.0);

        // Populate shared readings[] — base index for this board is board*6.
        int base = board * 6;
        unsigned long now_ms = millis();
        for (int ch = 0; ch < 6; ch++) {
            // Flip current sign on export circuits (negative active power).
            if (active_power[ch] < 0.0f) {
                current[ch] = -current[ch];
            }

            // Software correction. cur_mul trims an individual CT; pow_mul
            // scales both power figures and, if negative, corrects a CT that
            // was clamped on backwards. Apparent power takes |pow_mul| because
            // VA is unsigned by definition.
            const int chan = base + ch;
            current[ch]        *= cur_mul[chan];
            active_power[ch]   *= pow_mul[chan];
            apparent_power[ch] *= fabsf(pow_mul[chan]);

            readings[base + ch].voltage       = voltage[ch];
            readings[base + ch].current       = current[ch];
            // ATM90E32 active power is in watts; convert to kW for data model.
            readings[base + ch].active_power  = active_power[ch] / 1000.0f;
            // ATM90E32 does not natively provide reactive power; zero the field.
            // TODO: derive reactive power from apparent and active if needed.
            readings[base + ch].reactive_power = 0.0f;
            // ATM90E32 apparent power is in VA; convert to kVA for data model.
            readings[base + ch].apparent_power = apparent_power[ch] / 1000.0f;
            readings[base + ch].power_factor   = power_factor[ch];
            readings[base + ch].frequency      = freq;
            readings[base + ch].phase          = (int8_t)(ch % 3);
            // Energy already in kWh (division done in double above).
            readings[base + ch].import_energy  = import_energy_kwh[ch];
            readings[base + ch].export_energy  = export_energy_kwh[ch];
            readings[base + ch].total_energy   =
                readings[base + ch].import_energy + readings[base + ch].export_energy;
            // Assign CT channel index as meter ID and record poll timestamp.
            readings[base + ch].meterid              = (uint8_t)(base + ch);
            readings[base + ch].timestamp_last_report = now_ms;
        }

        #ifdef ENABLE_DEBUG
        Serial.printf("ATM90E32 board %d: V1=%.1fV freq=%.2fHz temp IC1=%.1fC IC2=%.1fC\n",
                      board, voltage[0], freq, ic_temp[ic_a], ic_temp[ic_b]);
        {
            char flags[128];
            for (int half = 0; half < 2; half++) {
                const ATM90E32IcStatus &st = ic_status[board * 2 + half];
                ATM90E32Status::decodeSysStatus0(st.sys0, flags, sizeof(flags));
                Serial.printf("  IC%d SysStatus0 : %s\n", half + 1, flags);
                ATM90E32Status::decodeSysStatus1(st.sys1, flags, sizeof(flags));
                Serial.printf("  IC%d SysStatus1 : %s\n", half + 1, flags);
                ATM90E32Status::decodeMeterStatus0(st.meter0, flags, sizeof(flags));
                Serial.printf("  IC%d MeterStat0 : %s\n", half + 1, flags);
                ATM90E32Status::decodeMeterStatus1(st.meter1, flags, sizeof(flags));
                Serial.printf("  IC%d MeterStat1 : %s\n", half + 1, flags);
            }
        }
        for (int ch = 0; ch < 6; ch++) {
            Serial.printf("  CT%d: %.3fA  %.2fW  PF=%.3f\n",
                          base + ch + 1,
                          readings[base + ch].current,
                          readings[base + ch].active_power * 1000.0f,
                          readings[base + ch].power_factor);
        }
        #endif
    }
}

bool atm90e32_board_ok(int boardIndex) {
    if (boardIndex < 0 || boardIndex >= ATM90E32_NUM_BOARDS) {
        return false;
    }
    return board_ok[boardIndex];
}

float atm90e32_latest_amps(int channelIndex) {
    if (channelIndex < 0 || channelIndex >= MODBUS_NUM_METERS) {
        return 0.0f;
    }
    return readings[channelIndex].current;
}

float atm90e32_temperature(int icIndex) {
    if (icIndex < 0 || icIndex >= ATM90E32_NUM_ICS) {
        return NAN;
    }
    return ic_temp[icIndex];
}

bool atm90e32_ic_status(int icIndex, ATM90E32IcStatus *out) {
    if (out == nullptr || icIndex < 0 || icIndex >= ATM90E32_NUM_ICS) {
        return false;
    }
    *out = ic_status[icIndex];
    return true;
}

#endif // METER_TYPE_ATM90E32
