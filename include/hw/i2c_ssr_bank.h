#pragma once

#include <stdint.h>
#include <core/data_model.h>

#ifndef I2C_SSR_BOARD_COUNT
#define I2C_SSR_BOARD_COUNT 2
#endif

#ifndef I2C_SSR_CHANNELS_PER_BOARD
#define I2C_SSR_CHANNELS_PER_BOARD 8
#endif

#define I2C_SSR_MAX_CHANNELS (I2C_SSR_BOARD_COUNT * I2C_SSR_CHANNELS_PER_BOARD)

#ifndef SSR_TENANT_RELAY_COUNT
#define SSR_TENANT_RELAY_COUNT 10
#endif

enum TenantServiceClass : uint8_t {
    TENANT_SERVICE_NULL = 0,
    TENANT_SERVICE_BEST_EFFORT,
    TENANT_SERVICE_NORMAL,
    TENANT_SERVICE_URGENT,
    TENANT_SERVICE_IMPORTANT,
    TENANT_SERVICE_ESSENTIAL,
    TENANT_SERVICE_CRITICAL
};

/** Per-tenant relay policy. A threshold of -1 means the limit is disabled. */
struct RelayRule {
    float kwh_limit = -1.0f;  // daily/interval energy threshold for this iteration; future TOU zones can replace this source.
    float kw_limit  = -1.0f;  // instantaneous power threshold
    uint32_t warning_grace_ms = 0;  // 0 = use RelayDefault_warning_grace_ms
    uint32_t excess_trip_ms = 0;    // 0 = use RelayDefault_excess_trip_ms
    TenantServiceClass service_class = TENANT_SERVICE_NORMAL;
    bool warning_alert_given = false;
    bool tripped = false;
    bool closed = false;
    unsigned long exceeded_since_ms = 0;
    unsigned long warning_since_ms = 0;
};

/** 8-bit I2C GPIO expander (PCF8574 family) driving an 8-channel SSR / relay bank. */
void setup_i2c_ssr_bank();
/** Poll USB serial: keys 0-7 toggle channels, 'a' all off, '?' help. */
void loop_i2c_ssr_bank_serial();
/** Blink SSR channel 1 on/off every 500ms for testing. */
void loop_i2c_ssr_bank_blink_test();

/**
 * Directly command a tenant SSR channel.
 *
 * These outputs drive opto-isolated AC SSRs. Most SSR modules are zero-crossing
 * devices: they turn on/off around the AC waveform zero crossing, so actuation
 * is not an instantaneous DC-style disconnect.
 */
bool set_i2c_ssr_channel(uint8_t channel, bool closed);
bool get_i2c_ssr_channel_closed(uint8_t channel);
bool get_i2c_ssr_board_ok(uint8_t board);
uint8_t get_i2c_ssr_channel_count();
void enforce_relay_rules(const PowerData readings[], int count);

/** Store a relay rule for the given channel. Returns false if channel is out of range. */
bool set_relay_rule(uint8_t channel, RelayRule rule);
/** Retrieve the relay rule for the given channel. Returns a default (all -1) rule if out of range. */
const RelayRule get_relay_rule(uint8_t channel);
TenantServiceClass tenant_service_class_from_string(const char* value);
const char* tenant_service_class_name(TenantServiceClass value);
const char* relay_role_name(uint8_t relayId);
