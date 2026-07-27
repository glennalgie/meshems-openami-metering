#pragma once
//
// S6-A — SENSE the MD0630's hardware alarm output lines on ESP32 GPIO.
//
// The MD0630 trips on its OWN internal relay when leakage crosses a threshold
// (the life-safety path — independent of this firmware). This module only
// OBSERVES those alarm outputs so the EMS can log the edge and publish it over
// MQTT. It is deliberately READ-ONLY: it never drives the trip.
//
// WIRING (module side 5-12 V, ESP32 GPIO are NOT 5 V-tolerant):
//   MD0630 alarm output ── opto-isolator / resistive divider ──▶ ESP32 GPIO
//   Configure the GPIO as INPUT_PULLUP and treat it ACTIVE-LOW: the opto
//   transistor pulls the pin to GND when the module asserts its alarm. If your
//   level-shift is non-inverting (active-high), pass active_low = false.
//
// Modelled to match the lightweight driver style in this folder.
//
#include <Arduino.h>

class LeakageAlarmSense {
  public:
    static constexpr int COUNT = 3;
    struct Line { uint8_t pin; const char* name; bool active; bool last; };

    // ac_pin/dc_pin = the module's AC / DC leakage alarm outputs; fault_pin = the
    // general fault / self-test output. active_low matches an opto pull-down.
    void begin(uint8_t ac_pin, uint8_t dc_pin, uint8_t fault_pin, bool active_low = true) {
        active_low_ = active_low;
        lines_[0] = { ac_pin,    "ac_alarm", false, false };
        lines_[1] = { dc_pin,    "dc_alarm", false, false };
        lines_[2] = { fault_pin, "fault",    false, false };
        for (auto& l : lines_) pinMode(l.pin, active_low_ ? INPUT_PULLUP : INPUT);
        // prime last-state so the first poll() reports the real level as an edge
        for (auto& l : lines_) l.last = !read_active(l.pin);
    }

    // Sample all lines; returns true if ANY line changed since the last poll,
    // so the caller can log / publish only on edges (not every cycle).
    bool poll() {
        bool changed = false;
        for (auto& l : lines_) {
            l.active = read_active(l.pin);
            if (l.active != l.last) { l.last = l.active; changed = true; }
        }
        return changed;
    }

    bool ac()    const { return lines_[0].active; }
    bool dc()    const { return lines_[1].active; }
    bool fault() const { return lines_[2].active; }
    bool any()   const { return ac() || dc() || fault(); }
    const Line& line(int i) const { return lines_[i]; }

  private:
    bool read_active(uint8_t pin) const {
        int raw = digitalRead(pin);
        return active_low_ ? (raw == LOW) : (raw == HIGH);
    }
    Line lines_[COUNT] = {};
    bool active_low_   = true;
};
