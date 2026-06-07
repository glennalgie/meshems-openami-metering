#pragma once
#include <Arduino.h>

#define MAX_DEVICE_ID_CHARS   32
#define DEVICE_ID_PREFIX      "StreetEMS_"

#define MQTT_TOPIC              "openami" // "openami/StreetPoleEMS_<EMSid>"
//#define MQTT_TOPIC              "nesl"
#define MQTT_PUBLISH_INTERVAL   30000
#define MQTT_SERVER             "public.cloud.shiftr.io"  //"test.mosquitto.org"
#define MQTT_USER               "public"                  // leave empty for test.mosquitto.org
#define MQTT_PW                 "public"                  // leave empty for test.mosquitto.org   

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
extern int MQTTPoll_rate;         //in milliseconds, how often to call mqttclient.loop()
extern int RelayDefault_warning_grace_ms;  // delay before marking warning after a threshold is exceeded
extern int RelayDefault_excess_trip_ms;    // delay after warning before opening relay if excess continues



void generateDeviceID();
const char* getDeviceID();
