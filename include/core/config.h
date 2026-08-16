#pragma once
#include <Arduino.h>

// Local overrides (gitignored). Lets secrets.h set MQTT_SERVER / WiFi creds.
#if __has_include(<secrets.h>)
  #include <secrets.h>
#endif

#define MAX_DEVICE_ID_CHARS   32
#define DEVICE_ID_PREFIX      "StreetEMS_"

#define MQTT_TOPIC              "openami" // "openami/StreetPoleEMS_<EMSid>"
//#define MQTT_TOPIC              "nesl"
#define MQTT_PUBLISH_INTERVAL   30000
// Street-EMS demo: publish to the shiftr.io public MQTT broker so the node appears live in
// the public namespace as StreetEMS_<id>. Each node keeps its own WiFi (and may override the
// broker) in secrets.h; the #ifndef guards below let secrets.h win.
#ifndef MQTT_SERVER
#define MQTT_SERVER             "public.cloud.shiftr.io"
#endif
#ifndef MQTT_USER
#define MQTT_USER               "public"                  // shiftr.io public instance
#endif
#ifndef MQTT_PW
#define MQTT_PW                 "public"
#endif
// Local Mosquitto alternative (set these in secrets.h to use instead of shiftr.io):
//   #define MQTT_SERVER "10.0.0.116"   // PC running Mosquitto
//   #define MQTT_USER   ""             // empty = anonymous
//   #define MQTT_PW     ""

// DTM485 custom Ascii on serial on rsa485 -  Serial Pins and Baud Rate
#define DTM485_SERIAL Serial2
#define DTM485_DE_RE_PIN 23
#define DTM485_BAUDRATE 9600

// Poll and Publish Intervals
#define DTM485POLL_INTERVAL_MS 5000        // 5 seconds
#define DTM_MQTT_PUBLISH_INTERVAL_MS 300000   // 5 minutes

//TODO allow these to 
extern int ModbusMaster_pollrate;    //in 1000's millisecond or seconds
extern int MQTTPublish_rootrate;  // MQTT publish scheduler tick in milliseconds
extern int MQTTPublish_subpanel_rate;      // subpanel_3Ph publish interval
extern int MQTTPublish_meter_group_rate;   // meter_0..meter_n group publish interval
extern int MQTTPublish_env_rate;           // subpanel_ENV publish interval
extern int MQTTPublish_mfr_rate;           // subpanel_MFR publish interval
extern int MQTTPublish_circuitsetup_rate;  // subpanel_circuitsetup publish interval
extern int MQTTPublish_leakage_rate;       // subpanel_RCMleaks publish interval
extern int MQTTPublish_harmonics_rate;     // subpanel_harmonics publish interval
extern int MQTTPublish_ssr_rate;           // subpanel_ssr publish interval
extern int MQTTPoll_rate;         //in milliseconds, how often to call mqttclient.loop()
extern int RelayDefault_warning_grace_ms;  // delay before marking warning after a threshold is exceeded
extern int RelayDefault_excess_trip_ms;    // delay after warning before opening relay if excess continues



void generateDeviceID();
const char* getDeviceID();
