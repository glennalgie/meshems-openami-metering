#pragma once

#include <stdint.h>

/** Apply tenant SSR / relay shadow (same logic as HTTP /api/relay). Used by MQTT command handler. */
bool apply_dashboard_relay(uint8_t board, uint8_t channel, bool state);
