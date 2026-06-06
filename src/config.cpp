#include <config.h>
#include <Arduino.h>
#include <esp_mac.h>

char device_id[MAX_DEVICE_ID_CHARS] = {0};

// this data can be downloaded from a linux java policy server aggregation node for every m streetpoleEMS nodes
int ModbusMaster_pollrate = 3000;  // ms between Modbus master cycles (main.cpp); SHT20 timeout is separate (~500 ms)
int MQTTPublish_rootrate = 1000;    // MQTT scheduler tick; per-topic rates below decide what publishes
int MQTTPublish_subpanel_rate = 15000;      // subpanel_3Ph: 15s
int MQTTPublish_meter_group_rate = 30000;   // meter_0..meter_n: 30s
int MQTTPublish_env_rate = 60000;           // subpanel_ENV: 1 min
int MQTTPublish_mfr_rate = 3600000;         // subpanel_MFR: 1 hour
int MQTTPublish_circuitsetup_rate = 300000; // subpanel_circuitsetup: 5 min
int MQTTPublish_leakage_rate = 0000;       // subpanel_RCMleaks: 10 min
int MQTTPublish_harmonics_rate = 300000;    // subpanel_harmonics: 5 min
int MQTTPoll_rate = 30000;           // hack: MQTT client loop / keepalive every 1min
// TODO add different appropriate mqtt publish rates for environmental vs stats vs RCMleaks and harmonics vs real time billing energy usage 
// keep the BPS donw below 300kbps , ideally 100 kbps per multitenant subpanel - reduce string char lengths convert to binary compression etc
// this leaves room for villager-villager low bit rate side channel on the packet control plane shared with energy : PTT store-and-forward clear-voice/txt/imaging 

void generatefullDeviceID() {// includes OUI vendorid of the ethernet MAC inside the ESP32S2 -prefer to not use  full MACid
  uint32_t low = ESP.getEfuseMac() & 0xFFFFFFFF;
  uint32_t high = (ESP.getEfuseMac() >> 32) & 0xFFFFFFFF;
  //uint64_t fullMAC = word(low, high);
  sprintf(device_id, "%s%X%X", DEVICE_ID_PREFIX, high, low);
}

void generateDeviceID() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  //Serial.printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  sprintf(device_id, "%s%02X%02X%02X", DEVICE_ID_PREFIX, mac[3], mac[4], mac[5]);
}

const char* getDeviceID() {
  return (const char*)device_id;
}
