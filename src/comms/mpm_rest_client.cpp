#include <comms/mpm_rest_client.h>
#include <core/config.h>
#include <ArduinoJson.h>
#include <TimeLib.h>

// Local meter index -> MPM meter identity, per the design doc's §6 mapping
// fields (gateway_id is shared, from getDeviceID(), not stored per-mapping).
struct MpmMeterMapping {
    char     meter_id[MPM_MAPPING_ID_LEN]     = {0};  // stable logical channel, e.g. "tenant-007"
    char     mpm_meter_id[MPM_MAPPING_ID_LEN] = {0};  // MPM's own identity, e.g. "MPM-1007"
    uint16_t mapping_version = 0;
    uint32_t sequence        = 0;   // monotonic per meter; NOT yet persisted across reboot
                                     // (design doc §8's outbox/journal work -- pending
                                     // Decision gate A/B, not built until the real
                                     // contract is confirmed)
    bool     mapped          = false;
};

static bool            mpm_enabled         = false;
static unsigned long   lastMpmUploadMillis = 0;
static MpmMeterMapping mpmMapping[MODBUS_NUM_METERS];

// PowerData.phase (0/1/2) -> the single-letter phase code the data contract uses.
static char phaseLetter(int8_t phase) {
    switch (phase) {
        case 0:  return 'A';
        case 1:  return 'B';
        case 2:  return 'C';
        default: return '?';
    }
}

// UTC capture time in the design doc's exact format ("2026-11-21T14:00:00Z").
// Uses TimeLib's now() (already a dependency elsewhere in this codebase, e.g.
// mqtt_client.cpp) rather than PowerData.timestamp_last_report, whose units
// vary by meter driver (millis() for some, epoch seconds for others).
static void isoTimestamp(char* buf, size_t len) {
    time_t t = now();
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

// Mock transport: builds and logs the exact JSON payload the design doc's §7
// proposes, instead of opening a TLS connection. MPM's real ingestion
// contract isn't confirmed yet (design doc §3/§3a) -- this is the one
// function to replace with a real HTTPS POST once it is; everything else
// (mapping, scheduling, payload shape) is already real.
static bool mpm_send_reading(MpmMeterMapping& m, const PowerData& pd) {
    JsonDocument doc;
    doc["schema"]          = "openami.mpm.meter-reading.v1";
    doc["gateway_id"]      = getDeviceID();
    doc["meter_id"]        = m.meter_id;
    doc["mpm_meter_id"]    = m.mpm_meter_id;
    doc["mapping_version"] = m.mapping_version;
    doc["sequence"]        = m.sequence;

    char ts[25];
    isoTimestamp(ts, sizeof(ts));
    doc["timestamp"] = ts;

    char phase[2] = { phaseLetter(pd.phase), '\0' };
    doc["phase"] = phase;

    JsonObject readings = doc["readings"].to<JsonObject>();
    // PowerData energy/power fields are kWh/kW (data_model.h); the data
    // contract wants integer Wh / W, same x1000 convention mqtt_client.cpp
    // already uses for its own SunSpec-style publishes.
    readings["energy_import_wh"] = (uint32_t)(pd.total_energy * 1000.0f);
    readings["energy_export_wh"] = (uint32_t)(pd.export_energy * 1000.0f);
    readings["voltage_v"]        = pd.voltage;
    readings["current_a"]        = pd.current;
    readings["active_power_w"]   = pd.active_power * 1000.0f;
    readings["power_factor"]     = pd.power_factor;

    // Real quality/rollover markers are design doc §8 future work (counter
    // rollover, out-of-order, negative-delta detection) -- not implemented.
    doc["quality"] = "valid";

    Serial.print("[MPM mock] ");
    serializeJson(doc, Serial);
    Serial.println();

    m.sequence++;
    return true;
}

// Initializes the MPM REST side-channel. Off by default even when
// MPM_REST_ENABLE is compiled in; call mpm_rest_set_enabled(true) (or wire it
// to a runtime setting) to actually start uploads.
void setup_mpm_rest() {
    mpm_enabled         = false;
    lastMpmUploadMillis = 0;
}

bool mpm_rest_enabled() { return mpm_enabled; }
void mpm_rest_set_enabled(bool on) { mpm_enabled = on; }

void mpm_rest_set_mapping(uint8_t local_meterid, const char* meter_id,
                           const char* mpm_meter_id, uint16_t mapping_version) {
    if (local_meterid >= MODBUS_NUM_METERS) return;
    MpmMeterMapping& m = mpmMapping[local_meterid];
    strncpy(m.meter_id, meter_id, MPM_MAPPING_ID_LEN - 1);
    m.meter_id[MPM_MAPPING_ID_LEN - 1] = '\0';
    strncpy(m.mpm_meter_id, mpm_meter_id, MPM_MAPPING_ID_LEN - 1);
    m.mpm_meter_id[MPM_MAPPING_ID_LEN - 1] = '\0';
    m.mapping_version = mapping_version;
    m.mapped          = true;
}

// Uploads every mapped meter's current reading from the shared metering
// snapshot (readings[] in data_model.h) -- the same array OPENAMI already
// publishes from, so this never polls Modbus a second time. Rate-gated by
// MPMUpload_rate, independent of OPENAMI's own publish cadence.
void loop_mpm_rest() {
    if (!mpm_enabled) return;
    unsigned long now_ms = millis();
    if (now_ms - lastMpmUploadMillis < (unsigned long)MPMUpload_rate) return;
    lastMpmUploadMillis = now_ms;

    for (uint8_t i = 0; i < MODBUS_NUM_METERS; i++) {
        if (!mpmMapping[i].mapped) continue;
        mpm_send_reading(mpmMapping[i], readings[i]);
    }
}
