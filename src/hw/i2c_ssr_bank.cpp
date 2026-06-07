#ifdef ENABLE_RELAYS
#include <Arduino.h>
#include <Wire.h>
#include <hw/i2c_ssr_bank.h>
#include <core/config.h>
#include <core/pins.h>
#include <string.h>

// Many opto-isolated AC SSR boards are active-low on the expander outputs
// (LOW = SSR on). Most AC SSRs are zero-crossing devices; commands request
// open/close, but physical switching occurs near the AC waveform zero crossing.
#ifndef I2C_SSR_ACTIVE_LOW
#define I2C_SSR_ACTIVE_LOW 1
#endif

static uint8_t s_shadow = 0x00;
static RelayRule relay_rules[8];

static uint8_t to_bus_value(uint8_t logical) {
#if I2C_SSR_ACTIVE_LOW
    return (uint8_t)~logical;
#else
    return logical;
#endif
}

static bool pcf8574_write(uint8_t value) {
    Wire.beginTransmission(PCF8574_I2C_ADDR);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

static bool relay_threshold_exceeded(const RelayRule& rule, const PowerData& reading) {
    const bool energyExceeded = (rule.kwh_limit >= 0.0f && reading.total_energy > rule.kwh_limit);
    const bool powerExceeded = (rule.kw_limit >= 0.0f && reading.active_power > rule.kw_limit);
    return energyExceeded || powerExceeded;
}

static uint32_t warning_grace_ms(const RelayRule& rule) {
    return rule.warning_grace_ms > 0 ? rule.warning_grace_ms : (uint32_t)RelayDefault_warning_grace_ms;
}

static uint32_t excess_trip_ms(const RelayRule& rule) {
    return rule.excess_trip_ms > 0 ? rule.excess_trip_ms : (uint32_t)RelayDefault_excess_trip_ms;
}

static bool equals_ignore_case(const char* lhs, const char* rhs) {
    if (lhs == nullptr || rhs == nullptr) return false;
    while (*lhs && *rhs) {
        char a = *lhs++;
        char b = *rhs++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return *lhs == '\0' && *rhs == '\0';
}

static void i2c_scan_bus() {
    Serial.printf("I2C scan SDA=GPIO%d SCL=GPIO%d:\n", I2C_SSR_SDA_GPIO, I2C_SSR_SCL_GPIO);
    int found = 0;
    for (uint8_t addr = 8; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  0x%02X ACK\n", addr);
            found++;
        }
    }
    if (!found) {
        Serial.println("  (no devices) - check 4 wires, +5V at SSR, and I2C_SSR_*_GPIO in pins.h");
    }
}

void setup_i2c_ssr_bank() {
    Wire.begin(I2C_SSR_SDA_GPIO, I2C_SSR_SCL_GPIO);
    Wire.setClock(100000);
    i2c_scan_bus();
    s_shadow = 0x00;  // all channels logically off
    for (uint8_t ch = 0; ch < 8; ch++) {
        relay_rules[ch].closed = false;
    }
    if (!pcf8574_write(to_bus_value(s_shadow))) {
        Serial.printf("I2C SSR: no ACK at 0x%02X - set PCF8574_I2C_ADDR to a address from the scan, or fix GPIOs\n",
                      PCF8574_I2C_ADDR);
    } else {
        Serial.println("I2C SSR: PCF8574 ok. Serial: 0-7=toggle, a=all off, ?=help");
    }
}

bool set_i2c_ssr_channel(uint8_t channel, bool closed) {
    if (channel > 7) return false;
    if (closed) {
        s_shadow |= (uint8_t)(1u << channel);
    } else {
        s_shadow &= (uint8_t)~(1u << channel);
    }
    const bool ok = pcf8574_write(to_bus_value(s_shadow));
    if (ok) {
        relay_rules[channel].closed = closed;
        if (closed) {
            relay_rules[channel].tripped = false;
            relay_rules[channel].warning_alert_given = false;
            relay_rules[channel].exceeded_since_ms = 0;
            relay_rules[channel].warning_since_ms = 0;
        }
    }
    return ok;
}

bool get_i2c_ssr_channel_closed(uint8_t channel) {
    if (channel > 7) return false;
    return (s_shadow & (uint8_t)(1u << channel)) != 0;
}

void loop_i2c_ssr_bank_blink_test() {
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 500) {
        lastBlink = millis();
        s_shadow ^= (uint8_t)(1u << 7);
        if (pcf8574_write(to_bus_value(s_shadow))) {
            Serial.printf("SSR1 -> %s\n", (s_shadow & (1u << 7)) ? "ON" : "OFF");
        } else {
            Serial.println("I2C write failed");
        }
    }
}

bool set_relay_rule(uint8_t channel, RelayRule rule) {
    if (channel > 7) return false;
    rule.warning_alert_given = false;
    rule.tripped = false;
    rule.closed = get_i2c_ssr_channel_closed(channel);
    rule.exceeded_since_ms = 0;
    rule.warning_since_ms = 0;
    relay_rules[channel] = rule;
    return true;
}

const RelayRule get_relay_rule(uint8_t channel) {
    if (channel > 7) return RelayRule{};
    return relay_rules[channel];
}

void enforce_relay_rules(const PowerData readings[], int count) {
    const unsigned long now = millis();
    const int limit = (count < 8) ? count : 8;
    for (int ch = 0; ch < limit; ch++) {
        RelayRule& rule = relay_rules[ch];
        if (rule.tripped || (rule.kwh_limit < 0.0f && rule.kw_limit < 0.0f)) {
            continue;
        }

        if (!relay_threshold_exceeded(rule, readings[ch])) {
            rule.exceeded_since_ms = 0;
            rule.warning_since_ms = 0;
            rule.warning_alert_given = false;
            continue;
        }

        if (rule.exceeded_since_ms == 0) {
            rule.exceeded_since_ms = now;
        }

        if (!rule.warning_alert_given && now - rule.exceeded_since_ms >= warning_grace_ms(rule)) {
            rule.warning_alert_given = true;
            rule.warning_since_ms = now;
            Serial.printf("relay warning: tenant %d class=%s kWh=%.3f limit=%.3f kW=%.3f limit=%.3f\n",
                          ch, tenant_service_class_name(rule.service_class),
                          readings[ch].total_energy, rule.kwh_limit,
                          readings[ch].active_power, rule.kw_limit);
        }

        if (rule.warning_alert_given && now - rule.warning_since_ms >= excess_trip_ms(rule)) {
            if (set_i2c_ssr_channel((uint8_t)ch, false)) {
                rule.tripped = true;
                Serial.printf("relay trip: tenant %d opened after sustained excess, class=%s\n",
                              ch, tenant_service_class_name(rule.service_class));
            } else {
                Serial.printf("relay trip failed: tenant %d I2C write failed\n", ch);
            }
        }
    }
}

TenantServiceClass tenant_service_class_from_string(const char* value) {
    if (value == nullptr) return TENANT_SERVICE_NORMAL;
    if (equals_ignore_case(value, "critical")) return TENANT_SERVICE_CRITICAL;
    if (equals_ignore_case(value, "essential")) return TENANT_SERVICE_ESSENTIAL;
    if (equals_ignore_case(value, "important")) return TENANT_SERVICE_IMPORTANT;
    if (equals_ignore_case(value, "urgent")) return TENANT_SERVICE_URGENT;
    return TENANT_SERVICE_NORMAL;
}

const char* tenant_service_class_name(TenantServiceClass value) {
    switch (value) {
        case TENANT_SERVICE_CRITICAL: return "critical";
        case TENANT_SERVICE_ESSENTIAL: return "essential";
        case TENANT_SERVICE_IMPORTANT: return "important";
        case TENANT_SERVICE_URGENT: return "urgent";
        case TENANT_SERVICE_NORMAL:
        default:
            return "normal";
    }
}

void loop_i2c_ssr_bank_serial() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c >= '0' && c <= '7') {
            int bit = c - '0';
            bool closed = !get_i2c_ssr_channel_closed((uint8_t)bit);
            if (set_i2c_ssr_channel((uint8_t)bit, closed)) {
                Serial.printf("ch %d -> %s (mask 0x%02X)\n", bit,
                              (s_shadow & (1u << bit)) ? "ON" : "OFF", s_shadow);
            } else {
                Serial.println("I2C write failed");
            }
        } else if (c == 'a' || c == 'A') {
            s_shadow = 0x00;
            if (pcf8574_write(to_bus_value(s_shadow))) {
                for (uint8_t ch = 0; ch < 8; ch++) {
                    relay_rules[ch].closed = false;
                }
                Serial.println("all channels off");
            }
        } else if (c == '?' || c == 'h') {
            Serial.println("I2C SSR: 0-7 toggle | a all off | ? this help");
        }
    }
}

#endif // ENABLE_RELAYS
