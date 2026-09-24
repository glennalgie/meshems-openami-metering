#pragma once

/**
 * @file atm90e32_status.h
 * @brief Human-readable decoding of ATM90E32 status registers.
 *
 * The driver reads four status words per IC (SysStatus0/1, MeterStatus0/1).
 * Raw hex is useless in a field log, so these helpers expand a word into a
 * short flag list such as "SAG_A|NOLOAD_C".
 *
 * REGISTER MAPPING — note the library's method names do NOT match the register
 * names, and EVPE's own source comments get this backwards. Verified against
 * both implementations (lib/ATM90E32/ATM90E32.cpp and
 * EVPE/lib/ATM90E32/ATM90E32.cpp:414-424), which agree:
 *
 *   GetSysStatus0()   -> EMMIntState0 (0x73)   latched interrupt events
 *   GetSysStatus1()   -> EMMIntState1 (0x74)   latched interrupt events
 *   GetMeterStatus0() -> EMMState0    (0x71)   live metering state
 *   GetMeterStatus1() -> EMMState1    (0x72)   live metering state
 *
 * So the "SysStatus" pair is the latched/event side and the "MeterStatus" pair
 * is the live-state side, despite what the names suggest.
 *
 * EDGE SEMANTICS: the driver calls ClearSysStatus0()/ClearSysStatus1() right
 * after reading, so the SysStatus flags describe only the interval since the
 * previous poll. Those two methods are a local addition to lib/ATM90E32 (they
 * are not in upstream CircuitSetup 2.0) — see lib/ATM90E32/README.md. The
 * MeterStatus registers are live state and are never cleared.
 *
 * PROVENANCE OF THE BIT TABLES: these came from the EVPE deployment
 * (EVPE/include/energy/energy_util.h) so the two firmwares log identically.
 * They have not been re-checked line-by-line against the ATM90E32AS datasheet.
 * Unrecognised bits are surfaced as "+0xNNNN" rather than dropped, so a wrong
 * or incomplete table shows up in the logs instead of hiding.
 */

#include <Arduino.h>

namespace ATM90E32Status {

struct FlagDesc {
    uint16_t    bit;
    const char *name;
};

// GetSysStatus0() / EMMIntState0 (0x73) — sag / over-voltage / no-load, per phase.
static const FlagDesc kSysStatus0[] = {
    {0x0001, "SAG_A"},    {0x0002, "SAG_B"},    {0x0004, "SAG_C"},
    {0x0010, "OV_A"},     {0x0020, "OV_B"},     {0x0040, "OV_C"},
    {0x0100, "NOLOAD_A"}, {0x0200, "NOLOAD_B"}, {0x0400, "NOLOAD_C"},
};

// GetSysStatus1() / EMMIntState1 (0x74) — zero-crossing, over-current, power fail.
static const FlagDesc kSysStatus1[] = {
    {0x0001, "ZX_A"}, {0x0002, "ZX_B"}, {0x0004, "ZX_C"},
    {0x0010, "OVERCURRENT_A"}, {0x0020, "OVERCURRENT_B"}, {0x0040, "OVERCURRENT_C"},
    {0x0100, "POWER_FAIL"},
};

// GetMeterStatus0() / EMMState0 (0x71) — metering engine health.
static const FlagDesc kMeterStatus0[] = {
    {0x0001, "METER_RUN"}, {0x0002, "DSP_READY"},
    {0x0004, "CALIBRATION"}, {0x0008, "POWER_VALID"},
};

// GetMeterStatus1() / EMMState1 (0x72) — frequency / phase seq / energy direction.
static const FlagDesc kMeterStatus1[] = {
    {0x0001, "FREQ_VALID"}, {0x0002, "PHASE_SEQ"},
    {0x0004, "ENERGY_DIR"}, {0x0008, "ENERGY_ACC"},
};

/**
 * Expand @p value into a '|'-separated flag list in @p buf.
 * Always NUL-terminates. Writes "none" when no known bit is set, and appends
 * "+0xNNNN" when undocumented bits remain, so nothing is silently dropped.
 */
inline void decodeFlags(uint16_t value, const FlagDesc *table, size_t count,
                        char *buf, size_t size) {
    if (buf == nullptr || size == 0) return;
    buf[0] = '\0';

    size_t   used      = 0;
    uint16_t recognised = 0;

    for (size_t i = 0; i < count; i++) {
        if ((value & table[i].bit) == 0) continue;
        recognised |= table[i].bit;

        const char *sep = (used > 0) ? "|" : "";
        int n = snprintf(buf + used, size - used, "%s%s", sep, table[i].name);
        if (n < 0 || (size_t)n >= size - used) return;  // truncated; stop cleanly
        used += (size_t)n;
    }

    const uint16_t leftover = (uint16_t)(value & ~recognised);
    if (leftover != 0) {
        snprintf(buf + used, size - used, "%s+0x%04X", used > 0 ? "" : "", leftover);
    } else if (used == 0) {
        snprintf(buf, size, "none");
    }
}

inline void decodeSysStatus0(uint16_t v, char *buf, size_t size) {
    decodeFlags(v, kSysStatus0, sizeof(kSysStatus0) / sizeof(kSysStatus0[0]), buf, size);
}
inline void decodeSysStatus1(uint16_t v, char *buf, size_t size) {
    decodeFlags(v, kSysStatus1, sizeof(kSysStatus1) / sizeof(kSysStatus1[0]), buf, size);
}
inline void decodeMeterStatus0(uint16_t v, char *buf, size_t size) {
    decodeFlags(v, kMeterStatus0, sizeof(kMeterStatus0) / sizeof(kMeterStatus0[0]), buf, size);
}
inline void decodeMeterStatus1(uint16_t v, char *buf, size_t size) {
    decodeFlags(v, kMeterStatus1, sizeof(kMeterStatus1) / sizeof(kMeterStatus1[0]), buf, size);
}

}  // namespace ATM90E32Status
