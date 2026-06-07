#ifdef ENABLE_WIFI
#include <Arduino.h>
#include <WiFiMulti.h>
#include <comms/wifi.h>


#if __has_include(<secrets.h>)
  #include <secrets.h>
#else
  #include <secrets_example.h>
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "EMSLab"
#endif

#ifndef WIFI_PW
#define WIFI_PW "E@rthday2025"
#endif

#ifndef WIFI_IDENTITY
#define WIFI_IDENTITY ""
#endif

#ifndef WIFI_CONNECT_ATTEMPTS
#define WIFI_CONNECT_ATTEMPTS 10
#endif

#ifndef WIFI_RECONNECT_INTERVAL_MS
#define WIFI_RECONNECT_INTERVAL_MS 10000
#endif

#ifndef WIFI_MULTI_RUN_TIMEOUT_MS
#define WIFI_MULTI_RUN_TIMEOUT_MS 5000
#endif

#ifndef WIFI_DIRECT_CONNECT_TIMEOUT_MS
#define WIFI_DIRECT_CONNECT_TIMEOUT_MS 5000
#endif

#ifndef WIFI_DIRECT_RETRY_DELAY_MS
#define WIFI_DIRECT_RETRY_DELAY_MS 5000
#endif

#ifndef WIFI_DIAGNOSTIC_SCAN_ON_BOOT
#define WIFI_DIAGNOSTIC_SCAN_ON_BOOT 0
#endif

static WiFiMulti wifiMulti;
static bool wifiMultiConfigured = false;
static unsigned long lastWifiReconnectAttempt = 0;

static const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "idle";
    case WL_NO_SSID_AVAIL: return "ssid_unavailable";
    case WL_SCAN_COMPLETED: return "scan_completed";
    case WL_CONNECTED: return "connected";
    case WL_CONNECT_FAILED: return "connect_failed";
    case WL_CONNECTION_LOST: return "connection_lost";
    case WL_DISCONNECTED: return "disconnected";
    default: return "unknown";
  }
}

static const char* authModeName(wifi_auth_mode_t authMode) {
  switch (authMode) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK: return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3-PSK";
    case WIFI_AUTH_WAPI_PSK: return "WAPI-PSK";
    default: return "unknown";
  }
}

static void scanAndLogConfiguredNetworks() {
  Serial.println("wifi scan starting");
  const int networkCount = WiFi.scanNetworks(false, true);
  if (networkCount <= 0) {
    Serial.printf("wifi scan found %d networks\n", networkCount);
    WiFi.scanDelete();
    return;
  }

  bool foundDefault = false;
  Serial.printf("wifi scan found %d networks\n", networkCount);
  for (int i = 0; i < networkCount; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid == WIFI_SSID) {
      foundDefault = true;
      const wifi_auth_mode_t authMode = WiFi.encryptionType(i);
      Serial.printf("wifi scan match: ssid=%s rssi=%d channel=%d auth=%s\n",
                    ssid.c_str(), WiFi.RSSI(i), WiFi.channel(i), authModeName(authMode));
      if (authMode == WIFI_AUTH_WPA2_ENTERPRISE) {
        Serial.println("wifi scan note: this AP is WPA2-Enterprise; password-only PSK credentials will not connect.");
      }
    }
  }

  if (!foundDefault) {
    Serial.printf("wifi scan did not find configured default SSID: %s\n", WIFI_SSID);
  }
  WiFi.scanDelete();
}

static bool connectDefaultAccessPointOnce(int attempt, int totalAttempts) {
  Serial.printf("wifi direct connect attempt %d/%d: ssid=%s\n", attempt, totalAttempts, WIFI_SSID);
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PW);

  const unsigned long start = millis();
  unsigned long lastLog = 0;
  wl_status_t status = WiFi.status();
  while (!wifi_client_connected() && millis() - start < WIFI_DIRECT_CONNECT_TIMEOUT_MS) {
    delay(250);
    status = WiFi.status();
    const unsigned long now = millis();
    if (now - lastLog >= 1000) {
      lastLog = now;
      Serial.printf("wifi direct connect pending: status=%s elapsed=%lu ms\n",
                    wifiStatusName(status), now - start);
    }
  }

  if (wifi_client_connected()) {
    Serial.printf("wifi direct connect done: ssid=%s ip=%s\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.printf("wifi direct connect failed: status=%s\n", wifiStatusName(status));
  return false;
}

static bool connectDefaultAccessPoint() {
  for (int attempt = 1; attempt <= WIFI_CONNECT_ATTEMPTS; ++attempt) {
    if (connectDefaultAccessPointOnce(attempt, WIFI_CONNECT_ATTEMPTS)) {
      return true;
    }

    if (attempt < WIFI_CONNECT_ATTEMPTS) {
      Serial.printf("wifi direct retry wait: %lu ms\n", (unsigned long)WIFI_DIRECT_RETRY_DELAY_MS);
      delay(WIFI_DIRECT_RETRY_DELAY_MS);
    }
  }

  return false;
}

static void addConfiguredAccessPoints() {
  if (wifiMultiConfigured) {
    return;
  }

#if CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT && defined(WIFI_USERNAME)
  wifiMulti.addAP(WIFI_SSID, WIFI_PW, WIFI_USERNAME, WIFI_IDENTITY);
#else
  wifiMulti.addAP(WIFI_SSID, WIFI_PW);
#endif

#if defined(WIFI_SSID_2) && defined(WIFI_PW_2)
  wifiMulti.addAP(WIFI_SSID_2, WIFI_PW_2);
#endif
#if defined(WIFI_SSID_3) && defined(WIFI_PW_3)
  wifiMulti.addAP(WIFI_SSID_3, WIFI_PW_3);
#endif
#if defined(WIFI_SSID_4) && defined(WIFI_PW_4)
  wifiMulti.addAP(WIFI_SSID_4, WIFI_PW_4);
#endif

  wifiMultiConfigured = true;
}

bool wifi_client_connected() {
  return WiFi.isConnected() && (WIFI_STA == (WiFi.getMode() & WIFI_STA));
}

// Function to get the IP address as a string
String get_wifi_ip() {
  if (wifi_client_connected()) {
    return WiFi.localIP().toString();
  }
  return "Not Connected";
}

bool setup_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.setSleep(false);

  addConfiguredAccessPoints();

  if (connectDefaultAccessPoint()) {
    lastWifiReconnectAttempt = millis();
    return true;
  }

#if WIFI_DIAGNOSTIC_SCAN_ON_BOOT
  scanAndLogConfiguredNetworks();
#endif

  Serial.printf("wifi falling back to WiFiMulti, default SSID: %s\n", WIFI_SSID);
  int attemptsRemaining = 1;
  wl_status_t status = WiFi.status();
  while (!wifi_client_connected() && (attemptsRemaining-- > 0)) {
    status = (wl_status_t)wifiMulti.run(WIFI_MULTI_RUN_TIMEOUT_MS, true);
    if (wifi_client_connected() || status == WL_CONNECTED) {
      break;
    }
    delay(500);
    Serial.printf("wifi retry: status=%s ssid=%s\n", wifiStatusName(status), WIFI_SSID);
  }

  lastWifiReconnectAttempt = millis();
  Serial.printf("wifi: %s: %s\n",
                WiFi.SSID().length() ? WiFi.SSID().c_str() : WIFI_SSID,
                wifi_client_connected() ? WiFi.localIP().toString().c_str() : "FAILED");
  return wifi_client_connected();
}

void loop_wifi() {
  if (wifi_client_connected()) {
    return;
  }

  const unsigned long now = millis();
  if (now - lastWifiReconnectAttempt < WIFI_RECONNECT_INTERVAL_MS) {
    return;
  }

  lastWifiReconnectAttempt = now;
  addConfiguredAccessPoints();
  Serial.printf("wifi reconnecting with WiFiMulti, default SSID: %s\n", WIFI_SSID);
  wl_status_t status = (wl_status_t)wifiMulti.run(WIFI_MULTI_RUN_TIMEOUT_MS, true);
  if (!wifi_client_connected()) {
    Serial.printf("wifi reconnect pending: status=%s ssid=%s\n", wifiStatusName(status), WIFI_SSID);
  }
}

#endif // ENABLE_WIFI
