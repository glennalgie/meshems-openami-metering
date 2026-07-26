#pragma once
//
// S4 — Energy-change correlation cache for leakage insights.
//
// A ring buffer of recent energy snapshots. When the leakage steps up or down,
// look back ±window in the ring to answer: "what energy event happened when the
// leakage changed?" (generate / store / consume / transform). Reuses the
// CurrentHistory circular-buffer pattern (see include/core/data_model.h).
//
// Feeds the S5 Leakage Insights Manager; serialisable for MQTT (subpanel_RCMleaks).
//
#include <Arduino.h>
#include <ArduinoJson.h>
#include <math.h>
#include <core/data_model.h>

// One timestamped energy snapshot (the PowerData subset relevant to leakage).
struct EnergySample {
    uint32_t ts_ms          = 0;
    float    voltage_V      = 0;
    float    current_A      = 0;
    float    activeP_kW     = 0;
    float    reactiveP_kVAr = 0;
    float    energy_kWh     = 0;
    float    phase_angle_deg= 0;   // reserved — leakage direction (future IVY RCM upgrade)
    float    ramp_kW_s      = 0;   // active-power ramp since the previous sample
    bool     valid          = false;

    void fromPowerData(const PowerData& p, uint32_t now, float ramp_kW_s_ = 0.0f) {
        ts_ms = now;
        voltage_V      = p.voltage;
        current_A      = p.current;
        activeP_kW     = p.active_power;
        reactiveP_kVAr = p.reactive_power;
        energy_kWh     = p.total_energy;
        ramp_kW_s      = ramp_kW_s_;
        valid          = true;
    }
    void toJson(JsonObject o) const {
        o["ts_ms"]           = ts_ms;
        o["V"]               = voltage_V;
        o["I_A"]             = current_A;
        o["P_kW"]            = activeP_kW;
        o["Q_kVAr"]          = reactiveP_kVAr;
        o["E_kWh"]           = energy_kWh;
        o["ramp_kW_s"]       = ramp_kW_s;
        o["phase_angle_deg"] = phase_angle_deg;
    }
};

// Ring buffer of energy snapshots (N slots; 128 mirrors CURRENT_HISTORY_SIZE).
template <int N>
struct EnergyRing {
    EnergySample buf[N];
    int      head  = 0;      // next slot to write
    int      count = 0;      // valid entries (0..N)
    float    last_active_kW = 0.0f;
    uint32_t last_ts        = 0;

    // Record one snapshot; ramp is derived from the previous sample's active power.
    void record(const PowerData& p, uint32_t now) {
        float dt   = (last_ts && now > last_ts) ? (now - last_ts) / 1000.0f : 0.0f;
        float ramp = (dt > 0.0f) ? (p.active_power - last_active_kW) / dt : 0.0f;
        EnergySample s; s.fromPowerData(p, now, ramp);
        buf[head] = s;
        head = (head + 1) % N;
        if (count < N) count++;
        last_active_kW = p.active_power;
        last_ts        = now;
    }

    // Sample closest in time to ts within ±window_ms (result.valid=false if none).
    EnergySample lookback(uint32_t ts, uint32_t window_ms) const {
        EnergySample best;                 // valid == false
        uint32_t bestDelta = window_ms + 1;
        for (int i = 0; i < count; i++) {
            uint32_t t = buf[i].ts_ms;
            uint32_t d = (t > ts) ? (t - ts) : (ts - t);
            if (d <= window_ms && d < bestDelta) { bestDelta = d; best = buf[i]; }
        }
        return best;
    }

    int size() const { return count; }
};

// A detected leakage step, correlated with the nearest energy event.
struct LeakageStep {
    bool         valid   = false;
    uint32_t     ts_ms   = 0;
    float        from_mA = 0, to_mA = 0;
    bool         rising  = false;
    EnergySample energy;                    // energy.valid tells if a match was found

    void toJson(JsonObject o) const {
        o["ts_ms"]             = ts_ms;
        o["from_mA"]           = from_mA;
        o["to_mA"]             = to_mA;
        o["delta_mA"]          = to_mA - from_mA;
        o["rising"]            = rising;
        o["energy_correlated"] = energy.valid;
        if (energy.valid) { JsonObject e = o["energy"].to<JsonObject>(); energy.toJson(e); }
    }
};

// Detects leakage steps (up/down beyond step_mA) and correlates each with the
// energy ring within ±window_ms. Keeps the most recent step for reporting.
struct LeakageInsightsCache {
    float       step_mA   = 1.0f;    // minimum leakage change counted as a step
    uint32_t    window_ms = 2000;    // ± lookback window around a step
    float       last_mA   = 0.0f;
    LeakageStep lastStep;            // most recent detected step

    template <int N>
    bool update(float mA, uint32_t now, const EnergyRing<N>& ring) {
        float delta   = mA - last_mA;
        bool  stepped = fabsf(delta) >= step_mA;
        if (stepped) {
            lastStep.valid   = true;
            lastStep.ts_ms   = now;
            lastStep.from_mA = last_mA;
            lastStep.to_mA   = mA;
            lastStep.rising  = (delta > 0.0f);
            lastStep.energy  = ring.lookback(now, window_ms);
        }
        last_mA = mA;
        return stepped;
    }
};
