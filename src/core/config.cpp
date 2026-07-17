#include <core/config.h>
#include <Arduino.h>
#include <esp_mac.h>

char device_id[MAX_DEVICE_ID_CHARS] = {0};

// These defaults are intentionally simple globals for this merge step; later
// they can move into downloaded policy/config storage.
int ModbusMaster_pollrate = 3000;  // ms between meter poll cycles
int MQTTPublish_rootrate = 1000;   // scheduler tick; per-topic rates below decide what publishes
int MQTTPublish_subpanel_rate = 300000;      // subpanel_3Ph: 5 min
int MQTTPublish_meter_group_rate = 120000;   // meter_0..meter_n: 2 min
int MQTTPublish_env_rate = 10000;            // subpanel_ENV: 10 s (live temperature demo; was 15 min)
int MQTTPublish_mfr_rate = 3600000;          // subpanel_MFR: 1 hour
int MQTTPublish_circuitsetup_rate = 300000;  // subpanel_circuitsetup: 5 min
int MQTTPublish_leakage_rate = 10000;        // subpanel_RCMleaks: 10 s (demo/bring-up; production 600000 = 10 min)
int MQTTPublish_harmonics_rate = 900000;     // subpanel_harmonics: 15 min
int MQTTPublish_ssr_rate = 120000;           // subpanel_ssr: 2 min
int MQTTPoll_rate = 60000;                   // MQTT client loop / keepalive tick
int RelayDefault_warning_grace_ms = 60000;   // warn after 1 minute over threshold
int RelayDefault_excess_trip_ms = 60000;     // open relay after warning persists another minute

void generatefullDeviceID() {
  uint32_t low = ESP.getEfuseMac() & 0xFFFFFFFF;
  uint32_t high = (ESP.getEfuseMac() >> 32) & 0xFFFFFFFF;
  sprintf(device_id, "%s%X%X", DEVICE_ID_PREFIX, high, low);
}

void generateDeviceID() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  sprintf(device_id, "%s%02X%02X%02X", DEVICE_ID_PREFIX, mac[3], mac[4], mac[5]);
}

const char* getDeviceID() {
  return (const char*)device_id;
}
