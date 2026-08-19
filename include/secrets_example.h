#pragma once

// Copy this file to secrets.h and fill in your credentials.
// secrets.h is gitignored and must never be committed.

#define WIFI_SSID "EMSLab1"
#define WIFI_PW "E@rthday2025"

// note defaults to local LAN mosquitto broker all blanks "" can loop if not deployed, anonymous connection. 
//  Override in secrets.h for private broker and/or shiftr.io, note MQTTS TLS option is separate and not implemented in this example.
#define MQTT_SERVER "public.cloud.shiftr.io"
#define MQTT_USER  "public"
#define MQTT_PW "public"
