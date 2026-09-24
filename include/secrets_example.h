#pragma once

// Copy this file to secrets.h and fill in your credentials.
// secrets.h is gitignored and must never be committed.

#define WIFI_SSID "EMSLab"
#define WIFI_PW "E@rthday2025"

// Broker settings deliberately live in include/core/config.h, not here.
// config.h defaults to the shiftr.io public broker behind #ifndef guards, so
// defining MQTT_SERVER / MQTT_USER / MQTT_PW in your own secrets.h overrides
// them. Duplicating them in this example file only creates two places to
// disagree about which broker is in use.
