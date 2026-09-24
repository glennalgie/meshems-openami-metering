#pragma once

#ifdef METER_TYPE_ATM90E32

/**
 * @file meter_atm90e32.h
 * @brief Driver for the CircuitSetup Expandable 6-Channel ATM90E32 energy meter.
 *
 * The CircuitSetup 6-channel board carries two ATM90E32 energy-metering ICs.
 * Each IC measures three CT channels (A/B/C) plus one AC voltage reference.
 * Together they provide 6 independent current channels and a shared voltage
 * reading — ideal for sub-circuit (breaker-level) monitoring in a distribution
 * panel.
 *
 * Communication is over SPI; this driver is therefore NOT guarded by
 * ENABLE_MODBUS_MASTER.  Enable it with METER_TYPE_ATM90E32 in platformio.ini.
 *
 * Pin assignments (CS1/CS2 per board, shared MOSI/MISO/CLK) are defined in
 * pins.h.  The SPI bus may be shared with other peripherals (OLED, SD card)
 * provided CS pins are unique.
 *
 * Calibration defaults are taken from the CircuitSetup EmonESP example for
 * meter hardware v1.3+ with a 9 V AC reference transformer.  Override via
 * build flags if different hardware is used:
 *   -DATM90E32_LINE_FREQ=4231        (60 Hz)   or 135 (50 Hz)
 *   -DATM90E32_PGA_GAIN=0            (1×, 21=2×, 42=4×)
 *   -DATM90E32_VOLTAGE_GAIN=7305     (9 V Jameco 157041 — meter v1.3+)
 *   -DATM90E32_CURRENT_GAIN=27961    (100A/50mA SCT-013-000 CT)
 *
 * Readings are stored in the shared readings[] array (data_model.h) with one
 * PowerData entry per channel:
 *   readings[0..2] = IC1 channels A/B/C  (CT1–CT3)
 *   readings[3..5] = IC2 channels A/B/C  (CT4–CT6)
 *
 * Additional boards (add-on pairs) would extend readings[6..11], etc., up to
 * ATM90E32_NUM_BOARDS * 6 channels.  Increase MODBUS_NUM_METERS in
 * data_model.h accordingly.
 *
 * @note Requires the CircuitSetup ATM90E32 Arduino library:
 *       https://github.com/CircuitSetup/ATM90E32_Arduino_Library
 *       Add to platformio.ini lib_deps when METER_TYPE_ATM90E32 is active.
 */

#include <Arduino.h>
#include <ATM90E32.h>
#include "core/data_model.h"
#include "core/pins.h"

// --------------------------------------------------------------------------
// Board count — one "board" = two ATM90E32 ICs = 6 CT channels.
// Override in platformio.ini with -DATM90E32_NUM_BOARDS=N if using add-ons.
// --------------------------------------------------------------------------
#ifndef ATM90E32_NUM_BOARDS
#define ATM90E32_NUM_BOARDS 3
#endif

// Hardware ceiling: pins.h carries exactly three board pairs' chip selects,
// matching the EVPE 3-stack. The CS tables are always declared at this size so
// the driver needs no #ifdef ladder; only the first ATM90E32_NUM_BOARDS entries
// are ever touched.
#define ATM90E32_MAX_BOARDS 3

#if (ATM90E32_NUM_BOARDS < 1) || (ATM90E32_NUM_BOARDS > ATM90E32_MAX_BOARDS)
#error "ATM90E32_NUM_BOARDS must be 1, 2 or 3 (see pins.h for the CS map)."
#endif

// Board 3 IC1 uses GPIO21, which is also MD0630_ALARM_FAULT_PIN. Catch the
// clash at compile time rather than with two drivers fighting over the pin.
#if (ATM90E32_NUM_BOARDS >= 3) && defined(LEAKAGE_ALARM_SENSE)
#error "ATM90E32_NUM_BOARDS=3 needs GPIO21, which LEAKAGE_ALARM_SENSE uses for MD0630_ALARM_FAULT_PIN. Disable one, or remap MD0630_ALARM_FAULT_PIN in pins.h."
#endif

// Total CT channels across all boards.
#define ATM90E32_NUM_CHANNELS (ATM90E32_NUM_BOARDS * 6)

// Two ATM90E32 ICs per board; index into the per-IC calibration tables.
#define ATM90E32_NUM_ICS (ATM90E32_NUM_BOARDS * 2)

// --------------------------------------------------------------------------
// Calibration defaults (see file header for override instructions)
// --------------------------------------------------------------------------

// Line frequency register value: 4231 = 60 Hz, 135 = 50 Hz
#ifndef ATM90E32_LINE_FREQ
#define ATM90E32_LINE_FREQ 4231
#endif

// PGA gain: 0 = 1×, 21 = 2×, 42 = 4×
#ifndef ATM90E32_PGA_GAIN
#define ATM90E32_PGA_GAIN 0
#endif

// Voltage gain for 9 V AC transformer (Jameco 157041) on meter v1.3+
#ifndef ATM90E32_VOLTAGE_GAIN
#define ATM90E32_VOLTAGE_GAIN 7305
#endif

// Current gain for 100A/50mA SCT-013-000 CT at PGA 1×.
// Common alternatives (PGA 1×):
// SCT-006 20A/25mA	11143
// SCT-013-030 30A/1V	 8650
// SCT-013-050 50A/1V	 15420
// SCT-010 50A/16.6mA	 41334
// SCT-010 80A/26.6mA	 41660
// SCT-013-000 100A/50mA 27518
// SCT-016 120A/40mA	 41787
// SCT-024 200A/100mA	 27518
// SCT-024 200A/50mA	 55036

#ifndef ATM90E32_CURRENT_GAIN
#define ATM90E32_CURRENT_GAIN 41660
#endif

// --------------------------------------------------------------------------
// Per-IC voltage gain
// --------------------------------------------------------------------------
// Each CircuitSetup board in a stack has its OWN 9 V AC reference transformer,
// so one global voltage gain cannot be right for more than one board. The
// ATM90E32_VOLTAGE_GAIN above is the fallback; override any individual IC to
// calibrate a stack properly. Index order is IC1,IC2 of board 1, then board 2,
// then board 3 -- i.e. the same order as readings[] groups channels.
//
// Measured EVPE values (MYRRA 44122 230V 9VAC, 60 Hz), kept here as a worked
// example of the spread you should expect between boards:
//     board 1: 10885    board 2: 10013    board 3: 10116
// Calibrate per unit: new_gain = (reference_meter_V / measured_V) * old_gain.
#ifndef ATM90E32_UGAIN_IC0
#define ATM90E32_UGAIN_IC0 ATM90E32_VOLTAGE_GAIN   // board 1, IC1 (CT1-3)
#endif
#ifndef ATM90E32_UGAIN_IC1
#define ATM90E32_UGAIN_IC1 ATM90E32_VOLTAGE_GAIN   // board 1, IC2 (CT4-6)
#endif
#ifndef ATM90E32_UGAIN_IC2
#define ATM90E32_UGAIN_IC2 ATM90E32_VOLTAGE_GAIN   // board 2, IC1 (CT7-9)
#endif
#ifndef ATM90E32_UGAIN_IC3
#define ATM90E32_UGAIN_IC3 ATM90E32_VOLTAGE_GAIN   // board 2, IC2 (CT10-12)
#endif
#ifndef ATM90E32_UGAIN_IC4
#define ATM90E32_UGAIN_IC4 ATM90E32_VOLTAGE_GAIN   // board 3, IC1 (CT13-15)
#endif
#ifndef ATM90E32_UGAIN_IC5
#define ATM90E32_UGAIN_IC5 ATM90E32_VOLTAGE_GAIN   // board 3, IC2 (CT16-18)
#endif

// --------------------------------------------------------------------------
// Per-channel scaling multipliers
// --------------------------------------------------------------------------
// Applied in software after the IC returns a reading. Use these to correct an
// individual CT that reads high/low, or to flip a channel whose CT is clamped
// backwards (negative pow_mul). Defaults are 1.0 = no correction.
//   cur_mul -> current only.  pow_mul -> active AND apparent power.
// Override a single channel with e.g. -DATM90E32_CUR_MUL_3=1.02
#ifndef ATM90E32_CUR_MUL_DEFAULT
#define ATM90E32_CUR_MUL_DEFAULT 1.0f
#endif
#ifndef ATM90E32_POW_MUL_DEFAULT
#define ATM90E32_POW_MUL_DEFAULT 1.0f
#endif

// --------------------------------------------------------------------------
// Public interface
// --------------------------------------------------------------------------

// Initialise SPI and all ATM90E32 ICs.  Call once from setup_modbus_master().
void setup_atm90e32();

// Poll all ATM90E32 ICs and copy results into readings[].
// Call from poll_energy_meters() inside loop_modbus_master().
void poll_atm90e32();

bool atm90e32_board_ok(int boardIndex);
float atm90e32_latest_amps(int channelIndex);

// Latest IC die temperature in degrees C. icIndex is 0..ATM90E32_NUM_ICS-1
// (IC1,IC2 of board 1, then board 2, ...). Returns NAN if out of range or the
// board was skipped on the last poll.
float atm90e32_temperature(int icIndex);

// Latched status words from the last poll, for diagnostics and MQTT. See
// atm90e32_status.h for decoding, and note the flags latch until re-init.
struct ATM90E32IcStatus {
    uint16_t sys0;    // EMMIntState0 (0x73) — sag / over-V / no-load   [cleared each poll]
    uint16_t sys1;    // EMMIntState1 (0x74) — zero-cross / over-I / power fail [cleared]
    uint16_t meter0;  // EMMState0    (0x71) — metering engine health   [live state]
    uint16_t meter1;  // EMMState1    (0x72) — freq / phase seq / energy dir [live state]
};
bool atm90e32_ic_status(int icIndex, ATM90E32IcStatus *out);

// Override the software correction for one CT channel (0..ATM90E32_NUM_CHANNELS-1).
// Pass NAN for either argument to leave that multiplier unchanged. A negative
// powMul flips active/apparent power, correcting a CT clamped on backwards.
void atm90e32_set_channel_scale(int channel, float curMul, float powMul);

#endif // METER_TYPE_ATM90E32
