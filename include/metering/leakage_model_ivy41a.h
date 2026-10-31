#pragma once
#include <ArduinoJson.h>

/*
Key Features of Leakage
TYPE B
DC milliamp leakage to measure, monitor for change and report growing leakage and isolating if possible the powerflor\w characteristics existing during changed leakage conditions
Worst case the fault line on the leakage sensors are set for lfesafety  levels when autonomous sub 300 msec response is to remove power flow and go open circuit protection
t\These leakage sensors report when leakage is removed and each StreetPoleEMS independantly can remotely or autonomously restore local power flow transfers

*/

    
struct LeakageChannel {
    float value_mA = 0.0f;
    // TODO AC lifesafety threshold is defaulted here to 30 milli amp - internal logic ensure less than 300 msec on fault line hi/lo trigger
    // TODO DC lifesafety threshold is to be defaulted to  6 milli amp less than x00 msec.  TBD 100 msec?
    // TODO use updateall to init the thresholds - ivy rcm rs485 modbus rtu meters have some programmability on thresholds before the lifesafety fault line is triggered
    float threshold_mA = 30.0f;  
    float lastFaultValue_mA = 0.0f;
    uint32_t lastFaultTimeMs = 0;
    bool inFault = false;

    void update(float newValue_mA) {
        value_mA = newValue_mA;
        if (value_mA >= threshold_mA) {
            if (!inFault) {
                lastFaultTimeMs = millis();
                lastFaultValue_mA = value_mA;
            }
            inFault = true;
        } else {
            inFault = false;
        }
    }

    void resetFault() {
        inFault = false;
        lastFaultValue_mA = 0.0f;
        lastFaultTimeMs = 0;
    }

    void print(const char* label, Stream& out = Serial) const {
        out.printf("[%s] Value: %.2f mA | Threshold: %.2f mA | In Fault: %s | Last Fault: %.2f mA at %lu ms\n",
                   label, value_mA, threshold_mA,
                   inFault ? "YES" : "NO",
                   lastFaultValue_mA, lastFaultTimeMs);
    }

    void toJson(JsonObject obj) const {
        obj["value_mA"] = value_mA;
        obj["threshold_mA"] = threshold_mA;
        obj["lastFaultValue_mA"] = lastFaultValue_mA;
        obj["lastFaultTimeMs"] = lastFaultTimeMs;
        obj["inFault"] = inFault;
    }
};

struct LeakageModel { // TODO  review leakage specs and consult with SME's
    uint16_t model_id = 999;
    uint16_t length = 30;
    LeakageChannel acSinusoidal;   // Type A AC sinusoidal — the real MD0630 AC reading
    LeakageChannel acPulsating;    // Type A AC pulsating (rectified) — no sensor on this
                                   // bus currently reports this; always updated with 0.0
                                   // from modbus_master.cpp and published as unsupported
                                   // (null) below, not as a plausible zero reading.
    LeakageChannel dc;             // Type B DC residual

    uint32_t lastUpdateMs = 0;
    bool valid = false;

    LeakageModel() {
        dc.threshold_mA = 6.0f;   // Type B life-safety default is 6 mA, not the 30 mA
                                   // the generic LeakageChannel default was built for.
    }

    void clear() {
        acSinusoidal = LeakageChannel();
        acPulsating = LeakageChannel();
        dc = LeakageChannel();
        dc.threshold_mA = 6.0f;
        lastUpdateMs = millis();
        valid = false;
    }

    void updateAll(float acSin_mA, float acPulse_mA, float dc_mA) {
        acSinusoidal.update(acSin_mA);
        acPulsating.update(acPulse_mA);
        dc.update(dc_mA);
        lastUpdateMs = millis();
        valid = true;
    }

    void toJson(JsonDocument& doc) const {
    doc["model_id"] = model_id;
    doc["length"] = length;
    doc["lastUpdateMs"] = lastUpdateMs;

    // Modern way to create nested objects - compiler friendly
    JsonObject acSin = doc["acSinusoidal"].to<JsonObject>();
    acSinusoidal.toJson(acSin);
    // acPulsating has no real sensor behind it on this bus: publish it explicitly as
    // unsupported rather than a fake all-zero reading a UX could mistake for real data.
    doc["acPulsating"] = (char*)0;
    JsonObject dcObj = doc["dc"].to<JsonObject>();
    dc.toJson(dcObj);

    // Maintenance call-to-action: one flat field set the UX can trigger a panel from
    // directly, instead of reading two nested inFault flags itself.
    bool anyFault = acSinusoidal.inFault || dc.inFault;
    JsonObject maint = doc["maintenance"].to<JsonObject>();
    maint["inFault"] = anyFault;
    if (acSinusoidal.inFault && dc.inFault) {
        maint["message"] = "AC and DC leakage both above threshold. Technician dispatch required.";
    } else if (acSinusoidal.inFault) {
        maint["message"] = "AC leakage above threshold. Check wiring and insulation.";
    } else if (dc.inFault) {
        maint["message"] = "DC leakage above threshold. Check wiring and insulation.";
    } else {
        maint["message"] = (char*)0;
    }
    }
};
