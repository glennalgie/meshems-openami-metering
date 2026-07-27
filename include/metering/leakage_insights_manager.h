#pragma once
//
// S5 — Leakage Insights Manager.
//
// Per-EMS leakage state + telemetry, and the deterministic multi-EMS nomination
// that isolates a leak on a shared LV feeder. Nomination combines **magnitude**
// ranking with a **peer-correlation "odd one out"** criterion (grounded in the
// faulty-feeder-ID literature — see S5_literature_findings.md), gated by a
// confidence threshold. Serialisable for the MQTT LV-feeder isolation subtopics.
//
// The sub-300 ms life-safety trip is NOT done here — it stays on the module's
// hardware fault line. This layer is seconds-scale localisation + isolation.
//
#include <Arduino.h>
#include <ArduinoJson.h>
#include <math.h>

// One node's leakage telemetry — this EMS ("self") or a peer received over MQTT.
struct LeakageNode {
    char     ems_id[24]   = {0};
    int8_t   phase        = 1;      // 0/1/2 = L1/L2/L3
    float    ac_mA        = 0;
    float    dc_mA        = 0;
    float    ramp_ac_mA_s = 0;
    uint32_t onset_ts     = 0;
    bool     exporting    = false;
    float    ac_thresh_mA = 30.0f;
    float    sig[8]       = {0};     // recent ramp signature (for correlation)
    int      sigN         = 0;
    bool     valid        = false;

    void pushSig(float v) {
        if (sigN < 8) { sig[sigN++] = v; }
        else { for (int i = 1; i < 8; i++) sig[i-1] = sig[i]; sig[7] = v; }
    }
};

enum LeakState : uint8_t { LK_NORMAL, LK_WATCH, LK_ISOLATING, LK_FAULT, LK_RESTORED };
const char* leakStateName(LeakState s);

// Outcome of one nomination round.
struct LeakageRound {
    bool  ran         = false;
    char  nominated[24] = {0};
    float confidence  = 0.0f;
    bool  isolate     = false;      // true = auto-isolate; false = alert-only
    int   reporting   = 0;
    float top1_mA     = 0, top2_mA = 0;
    bool  magCorrAgree= false;      // magnitude leader == min-correlation node?
    char  reason[40]  = {0};
};

class LeakageInsightsManager {
  public:
    // identity / config
    const char* ems_id    = "street-ems-01";
    const char* feeder_id = "lv-12";
    int8_t      phase     = 1;
    uint16_t    node      = 100;

    float    ac_trip        = 30.0f;   // mA
    float    dc_trip        = 6.0f;    // mA
    float    ac_trip_export = 27.0f;   // reverse power flow (S6)
    float    watch_frac     = 0.5f;    // watch level = 50 % of trip
    uint32_t onset_window_ms= 5000;    // correlated-onset window
    float    conf_min       = 0.30f;   // isolate only above this confidence

    // --- self ---
    void      updateSelf(float ac_mA, float dc_mA, bool exporting, uint32_t now);
    LeakState state() const { return _state; }
    void      selfTelemetry(JsonObject o) const;
    const LeakageNode& self() const { return _self; }

    // --- peers (from MQTT, or injected for the leak test) ---
    void clearPeers() { _nPeers = 0; }
    void addPeer(const LeakageNode& p);
    int  peerCount() const { return _nPeers; }

    // --- nomination round over self + peers ---
    LeakageRound nominate(uint32_t now);
    void roundJson(const LeakageRound& r, JsonObject o) const;   // …/leakage/isolation
    void alertJson(const LeakageRound& r, JsonObject o, uint32_t now) const; // …/leakage/alert

    float effTrip()    const { return _self.exporting ? ac_trip_export : ac_trip; }
    float watchLevel() const { return effTrip() * watch_frac; }

  private:
    LeakageNode _self;
    LeakState   _state    = LK_NORMAL;
    uint32_t    _onset_ts = 0;
    float       _last_ac  = 0;
    uint32_t    _last_ms  = 0;

    LeakageNode _peers[6];
    int         _nPeers   = 0;

    // Pearson correlation of two signatures (returns 0 if too short/degenerate).
    static float pearson(const float* a, int an, const float* b, int bn);
};
