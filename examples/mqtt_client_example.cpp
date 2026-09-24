#include <platform/mqtt_client.h>
#include <TimeLib.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoYaml.h>
#include <platform/config.h>
#include <platform/network.h>
#include <homie/Property.h>
#include <node/NodeEntity.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <LittleFS.h>
#include <mbedtls/base64.h>
#define ENABLE_DEBUG_MQTT = 1

// Transport clients - use secure or insecure based on port
WiFiClient insecureClient;
WiFiClientSecure secureClient;
Client* transportClient = nullptr;  // Pointer to active client
bool mqtt_using_secure = false;

MQTTClient mqtt_client(MAX_DATA_LEN + 200);  // Buffer size for MQTT packets
#define TOPIC_TOKEN_SIZ 80
#define MQTT_DATA_SIZ 128
unsigned long mqtt_interval_ts = 0;
static char mqtt_data[MQTT_DATA_SIZ] = "";
char topic_device[TOPIC_TOKEN_SIZ] = {0};
char topic_cmd[TOPIC_TOKEN_SIZ + 8] = {0};  // topic_device + "/set"
// Static LWT topic buffer - must persist after mqtt_connect() returns
// (pointer corruption fix per arduino-mqtt issue #89)
static char lwt_topic[64] = {0};
int num_subscribers = 0;

// ============================================================================
// MQTT Publish Queue - for thread-safe publishing from other tasks/cores
// ============================================================================
#define MQTT_QUEUE_SIZE 16
#define MQTT_TOPIC_MAX 128
// Sized to match Property::_value, so a value that fits in the property model also fits
// through the queue. It was 64, which silently truncated anything longer — a JSON array
// of cell voltages, an enum list, a long string — and drivers worked around it by
// bypassing the queue entirely (see examples/bms, which published cell_voltages with a
// direct mqtt_client.publish for exactly this reason).
#define MQTT_PAYLOAD_MAX 256

struct MqttQueueItem {
    char topic[MQTT_TOPIC_MAX];
    char payload[MQTT_PAYLOAD_MAX];
    int  length;        // explicit byte count — carries 0x00 (empty-string value, C3)
    bool retained;
};

static QueueHandle_t mqtt_publish_queue = nullptr;

// Initialize the MQTT publish queue (call from setup)
void mqtt_queue_init() {
    if (mqtt_publish_queue == nullptr) {
        mqtt_publish_queue = xQueueCreate(MQTT_QUEUE_SIZE, sizeof(MqttQueueItem));
        if (mqtt_publish_queue != nullptr) {
            Serial.println("MQTT: Publish queue created");
        } else {
            Serial.println("MQTT: ERROR - Failed to create publish queue");
        }
    }
}

// Queue a publish request (thread-safe, can be called from any task/core).
// Null-terminated convenience overload: length is strlen(payload).
bool mqtt_queue_publish(const char* topic, const char* payload, bool retained) {
    return mqtt_queue_publish(topic, payload, (int)strlen(payload), retained);
}

// Length-aware variant: publishes exactly `length` bytes, so a single 0x00 byte
// (the Homie empty-string value, C3) survives instead of being treated as a
// null-terminated zero-length payload. memcpy (not strncpy) preserves embedded NULs.
bool mqtt_queue_publish(const char* topic, const char* payload, int length, bool retained) {
    if (length < 0) length = 0;
    if (length > MQTT_PAYLOAD_MAX) {
        // Truncating a payload silently produces a subtly wrong retained value that looks
        // plausible — worse than a dropped message, because nothing downstream can tell.
        // Say so, and say by how much, so the fix (raise MQTT_PAYLOAD_MAX) is obvious.
        Serial.printf("MQTT: **ERROR -- payload for '%s' is %d bytes, max %d — TRUNCATED "
                      "(raise MQTT_PAYLOAD_MAX)\n", topic, length, MQTT_PAYLOAD_MAX);
        length = MQTT_PAYLOAD_MAX;
    }
    if (mqtt_publish_queue == nullptr) {
        Serial.println("MQTT: Queue not initialized, publishing directly");
        return mqtt_client.publish(topic, payload, length, retained, homie_qos(retained));
    }

    MqttQueueItem item;
    strncpy(item.topic, topic, MQTT_TOPIC_MAX - 1);
    item.topic[MQTT_TOPIC_MAX - 1] = '\0';
    memcpy(item.payload, payload, length);
    item.length = length;
    item.retained = retained;

    // Non-blocking queue send (don't wait if queue is full)
    if (xQueueSend(mqtt_publish_queue, &item, 0) != pdTRUE) {
        Serial.println("MQTT: Queue full, message dropped");
        return false;
    }
    return true;
}

// Process queued publish requests (call from main loop on Core 0)
void mqtt_process_queue() {
    if (mqtt_publish_queue == nullptr) return;

    MqttQueueItem item;
    // Process up to 4 messages per loop iteration to avoid blocking
    int processed = 0;
    while (processed < 4 && xQueueReceive(mqtt_publish_queue, &item, 0) == pdTRUE) {
        if (mqtt_client.connected()) {
            // Length-aware publish so an empty-string value (single 0x00 byte) is sent
            // intact rather than collapsing to a zero-length (retract) payload (C3).
            // Retained -> configured QoS (default 2), non-retained -> QoS 0 (C4).
            mqtt_client.publish(item.topic, item.payload, item.length, item.retained, homie_qos(item.retained));
        }
        processed++;
    }
}

// Heap-allocated on first subscription (not a static array — see Device.cpp's
// _description_json comment). settable_properties[i] indexing works unchanged
// since a pointer subscripts identically to an array.
static subscribed_settable_property_t* settable_properties = nullptr;
static subscribed_settable_property_t* ensure_settable_properties() {
  if (settable_properties == nullptr) {
    // (): value-initialise. Belt-and-braces with the struct's member initializers.
    settable_properties = new subscribed_settable_property_t[128]();
  }
  return settable_properties;
}

// Fallback callback for unhandled messages (used by controller)
static mqtt_fallback_callback_t _fallback_callback = nullptr;

void mqtt_set_fallback_callback(mqtt_fallback_callback_t callback) {
    _fallback_callback = callback;
}

// Reconnect callback (to re-publish state after reconnection)
static mqtt_reconnect_callback_t _reconnect_callback = nullptr;

void mqtt_set_reconnect_callback(mqtt_reconnect_callback_t callback) {
    _reconnect_callback = callback;
}

// Function prototype for mqtt_publish_json
void mqtt_publish_json(const char* subtopic, const JsonDocument* payload);

#ifndef ESP32_POE_ISO
// Fetch root CA cert from broker via TLS on port 443 and save to SPIFFS
// Connects insecurely, extracts cert chain, finds root, saves as PEM
// Reuses post_body_buf (2048 bytes) as scratch for base64 encoding
extern char post_body_buf[2048];

static bool mqtt_fetch_and_save_root_cert() {
  char cert_path[96];
  mqtt_build_cert_path(mqtt_host, cert_path, sizeof(cert_path));

  Serial.printf("TLS: Fetching root CA from %s:%u\n", mqtt_host, mqtt_port);
  Serial.printf("TLS: Free heap before probe: %u bytes\n", ESP.getFreeHeap());

  // Reuse global secureClient for the probe to avoid allocating a second
  // TLS context (~40KB heap). We set it insecure for the probe, then
  // reconfigure it with the real CA cert afterward in mqtt_init_transport().
  secureClient.stop();  // Ensure clean state (free any stale socket/TLS resources)
  delay(100);           // Let LWIP release socket resources
  secureClient.setInsecure();
  secureClient.setTimeout(15);

  // Connect on the MQTTS port directly - same cert chain, guaranteed open
  Serial.printf("TLS: Connecting insecurely to %s:%u...\n", mqtt_host, mqtt_port);
  if (!secureClient.connect(mqtt_host, mqtt_port)) {
    Serial.printf("TLS: ERROR - Failed to connect to %s:%u\n", mqtt_host, mqtt_port);
    secureClient.stop();
    return false;
  }
  Serial.println("TLS: TLS handshake complete");

  // Get the peer certificate chain from the underlying mbedTLS context
  const mbedtls_x509_crt* peer_cert = secureClient.getPeerCertificate();
  if (peer_cert == nullptr) {
    Serial.println("TLS: ERROR - No peer certificate available");
    secureClient.stop();
    return false;
  }

  // Count certs in chain and find the root (last cert)
  int chain_len = 0;
  const mbedtls_x509_crt* cert = peer_cert;
  const mbedtls_x509_crt* root_cert = nullptr;

  while (cert != nullptr) {
    char cn_buf[64] = {0};
    mbedtls_x509_dn_gets(cn_buf, sizeof(cn_buf), &cert->subject);
    Serial.printf("TLS: [%d] %s\n", chain_len, cn_buf);

    root_cert = cert;  // Last cert in chain is the root
    cert = cert->next;
    chain_len++;
  }

  Serial.printf("TLS: Certificate chain has %d certificate(s)\n", chain_len);

  if (root_cert == nullptr) {
    Serial.println("TLS: ERROR - Could not identify root certificate");
    secureClient.stop();
    return false;
  }

  char root_cn[64] = {0};
  mbedtls_x509_dn_gets(root_cn, sizeof(root_cn), &root_cert->subject);
  Serial.printf("TLS: Root CA: %s\n", root_cn);

  // Convert root cert DER to PEM format using post_body_buf as scratch
  size_t der_len = root_cert->raw.len;
  Serial.printf("TLS: Root cert DER size: %d bytes\n", der_len);

  // Base64 encode DER into post_body_buf (scratch)
  size_t b64_len = 0;
  mbedtls_base64_encode(nullptr, 0, &b64_len, root_cert->raw.p, der_len);
  if (b64_len > sizeof(post_body_buf)) {
    Serial.printf("TLS: ERROR - Base64 too large (%d bytes)\n", b64_len);
    secureClient.stop();
    return false;
  }

  size_t b64_written = 0;
  mbedtls_base64_encode((unsigned char*)post_body_buf, sizeof(post_body_buf),
                         &b64_written, root_cert->raw.p, der_len);

  // Done with TLS probe - disconnect and free TLS resources
  secureClient.stop();

  // Build PEM directly into SPIFFS file to avoid needing another large buffer
  if (!LittleFS.begin(true)) {
    Serial.println("TLS: ERROR - Failed to mount SPIFFS to save cert");
    return false;
  }

  File certFile = LittleFS.open(cert_path, FILE_WRITE);
  if (!certFile) {
    Serial.printf("TLS: ERROR - Failed to create cert file '%s'\n", cert_path);
    return false;
  }

  // Write PEM header
  certFile.print("-----BEGIN CERTIFICATE-----\n");

  // Write base64 in 64-char lines (PEM standard)
  for (size_t i = 0; i < b64_written; i += 64) {
    size_t line_len = (b64_written - i < 64) ? (b64_written - i) : 64;
    certFile.write((const uint8_t*)(post_body_buf + i), line_len);
    certFile.print("\n");
  }

  // Write PEM footer
  certFile.print("-----END CERTIFICATE-----\n");

  size_t file_size = certFile.size();
  certFile.close();

  Serial.printf("TLS: Root CA saved (%d bytes) to '%s'\n", file_size, cert_path);
  return true;
}
#endif // ESP32_POE_ISO

// Initialize transport client (secure or insecure based on port)
bool mqtt_init_transport() {
  mqtt_using_secure = mqtt_is_secure();

  if (mqtt_using_secure) {
    Serial.println("MQTT: Configuring TLS/SSL connection (port 8883)");

    // Load CA certificate from SPIFFS
    const char* caCert = mqtt_load_ca_cert();
#ifndef ESP32_POE_ISO
    // On boards with enough heap, auto-fetch the root CA cert if not cached
    if (caCert == nullptr) {
      Serial.println("TLS: No cached cert found, auto-fetching from broker...");
      if (mqtt_fetch_and_save_root_cert()) {
        caCert = mqtt_load_ca_cert();
      }
    }
#endif

    if (caCert != nullptr) {
      // Full TLS with CA verification
      secureClient.setCACert(caCert);
      Serial.println("MQTT: TLS transport configured with CA certificate");
      secureClient.setTimeout(15);  // 15 seconds for TLS handshake
      transportClient = &secureClient;
    } else {
#ifdef ESP32_POE_ISO
      // POE-ISO: insufficient heap for TLS probe; fall back to plain MQTT
      Serial.println("TLS: WARNING - No CA cert found in /certs/ SPIFFS folder");
      Serial.println("TLS: To fix: place cert in data/certs/ and upload filesystem");
      Serial.println("TLS: Falling back to insecure MQTT on port 1883");
      mqtt_port = 1883;
      mqtt_using_secure = false;
      transportClient = &insecureClient;
#else
      // Other boards: cert fetch failed, connect without verification
      Serial.println("TLS: WARNING - No CA cert available, connecting without verification");
      Serial.println("TLS: To fix: place cert in data/certs/ and upload filesystem");
      secureClient.setInsecure();
      secureClient.setTimeout(15);
      transportClient = &secureClient;
#endif
    }
  } else {
    Serial.printf("MQTT: Using insecure connection (port %d)\n", mqtt_port);
    transportClient = &insecureClient;
  }
  return true;
}

void generateTopics() {
  snprintf(topic_device, sizeof(topic_device), "%s/%s", HOMIE_TOPIC_PREFIX, getDeviceID());
  snprintf(topic_cmd, sizeof(topic_cmd), "%s/%s", topic_device, HOMIE_TOPIC_SET);
}

void subscribe_for_callbacks(const char* topic, property_settable_callback_t cb, Property* instance) {
   ensure_settable_properties();
   // Check if topic already registered (avoid duplicates on reconnection)
   for(int i=0; i<num_subscribers; i++) {
     if (strcmp(settable_properties[i].topic, topic) == 0) {
       // Already registered, update callback/instance in case they changed
       settable_properties[i].property_callback = cb;
       settable_properties[i].instance = instance;
       return;
     }
   }
   // New subscription. Null the entity fields explicitly: an entry created here may
   // never be visited by entity_subscribe_for_callbacks(), and dispatch branches on them.
   strcpy(settable_properties[num_subscribers].topic, topic);
   settable_properties[num_subscribers].property_callback = cb;
   settable_properties[num_subscribers].instance = instance;
   settable_properties[num_subscribers].entity_callback = nullptr;
   settable_properties[num_subscribers].entity = nullptr;
   num_subscribers++;
}

// D3: register a per-property fn-ptr handler (+context) for a /set topic. Updates an
// existing entry's handler in place (reconnect-safe), else appends a new subscriber.
void subscribe_settable_handler(const char* topic, settable_handler_t handler, void* ctx) {
   ensure_settable_properties();
   for (int i = 0; i < num_subscribers; i++) {
     if (strcmp(settable_properties[i].topic, topic) == 0) {
       settable_properties[i].handler = handler;
       settable_properties[i].handler_ctx = ctx;
       return;
     }
   }
   if (num_subscribers >= 128) {
     Serial.printf("MQTT: settable subscriber table full — handler for '%s' NOT registered\n", topic);
     return;
   }
   // Null the property/entity fields explicitly. A topic registered ONLY here — a
   // NodeProperty::on_set() handler, or a watch on a device attribute like $state — has
   // no Property or NodeEntity behind it, and dispatch tests both before calling.
   strcpy(settable_properties[num_subscribers].topic, topic);
   settable_properties[num_subscribers].handler = handler;
   settable_properties[num_subscribers].handler_ctx = ctx;
   settable_properties[num_subscribers].property_callback = nullptr;
   settable_properties[num_subscribers].instance = nullptr;
   settable_properties[num_subscribers].entity_callback = nullptr;
   settable_properties[num_subscribers].entity = nullptr;
   num_subscribers++;
}


void entity_subscribe_for_callbacks(const char* topic, entity_settable_callback_t cb, NodeEntity* entity) {
   ensure_settable_properties();
   for(int i=0;i<num_subscribers;i++) {
     if (strcmp(settable_properties[i].topic, topic) == 0) {
       //add entity pointer
       Serial.printf("ENTITY: Callback Subscription: %s SUCCESS\n", topic);
       settable_properties[i].entity_callback = cb;
       settable_properties[i].entity = entity;
       return;
     }
   }
   // Topic not found — create entry (Property::subscribe may not have run yet)
   if (num_subscribers < 128) {
     Serial.printf("ENTITY: Callback Subscription: %s CREATED NEW\n", topic);
     strcpy(settable_properties[num_subscribers].topic, topic);
     settable_properties[num_subscribers].property_callback = nullptr;
     settable_properties[num_subscribers].instance = nullptr;
     settable_properties[num_subscribers].entity_callback = cb;
     settable_properties[num_subscribers].entity = entity;
     num_subscribers++;
     if (mqtt_connected()) {
       mqtt_client.subscribe(topic);
     }
   } else {
     Serial.printf("ENTITY: Callback Subscription: FAIL - table full\n");
   }
}

boolean mqtt_connect() {
  Serial.printf("MQTT Connecting: '%s:%d'%s\r\n", mqtt_host, mqtt_port,
                mqtt_using_secure ? " (TLS)" : "");

  // Initialize transport client if not already done
  if (transportClient == nullptr) {
    if (!mqtt_init_transport()) {
      Serial.println("MQTT: Failed to initialize transport");
      return false;
    }
  }

  // Configure socket timeout based on connection type
  if (mqtt_using_secure) {
    secureClient.setTimeout(15);  // Longer timeout for TLS handshake
  } else {
    insecureClient.setTimeout(10);  // 10 seconds for plain TCP
  }

  // setOptions(keepAlive, cleanSession, timeout)
  // cleanSession: false - maintain session across reconnects
  // timeout: 1000ms - short timeout for operations (2000ms for TLS)
  int operationTimeout = mqtt_using_secure ? 2000 : 1000;
  mqtt_client.setOptions(60, false, operationTimeout);
  Serial.printf("MQTT: configured keepalive=%ds, timeout=%dms\n", 60, operationTimeout);

  // Set Last Will and Testament (LWT) - published by broker if connection lost.
  // ROOT-ONLY by construction: the will is a property of this single MQTT connection,
  // which the whole nested device tree shares, so it targets the ROOT device's $state.
  // Per Homie 5, a "lost" root $state cascades to every child in the tree (consumers
  // treat all children as lost); children neither need nor can have their own will.
  // On reconnect, on_mqtt_reconnect() re-asserts $state for the whole tree.
  // Use static buffer lwt_topic to prevent pointer corruption (arduino-mqtt issue #89)
  // Use QoS 0 - QoS 1 causes connection issues with this library
  snprintf(lwt_topic, sizeof(lwt_topic), "%s/%s/%s", HOMIE_TOPIC_PREFIX, getDeviceID(), HOMIE_$STATE);
  mqtt_client.setWill(lwt_topic, HOMIE_STATE_LOST, true, homie_qos(true));  // retained $state -> QoS 2 (C4)
  Serial.printf("MQTT will topic:'%s' msg:'%s'\n", lwt_topic, HOMIE_STATE_LOST);

  // Configure MQTT client BEFORE connecting
  // begin() sets up the network client and host — also resets the message callback,
  // so re-register it here on every connect (including reconnects).
  mqtt_client.begin(mqtt_host, mqtt_port, *transportClient);
  mqtt_client.onMessageAdvanced(subscriber_callback);

  // Connect with or without credentials
  bool connected = false;
  if (strlen(mqtt_username) == 0) {
    // Anonymous connection
    connected = mqtt_client.connect(getDeviceID());
  } else {
    Serial.printf("MQTT Username: '%s'\n", mqtt_username);
    connected = mqtt_client.connect(getDeviceID(), mqtt_username, mqtt_password);
  }

  if (connected) {
    Serial.printf("MQTT connected: %s\r\n", mqtt_host);
    // Re-subscribe to topics if needed
    for (int i = 0; i < num_subscribers; i++) {
      Serial.printf("MQTT subscribing to topic: %s\n", settable_properties[i].topic);
      mqtt_client.subscribe(settable_properties[i].topic);
    }
    // Call reconnect callback to re-publish state (overrides LWT "lost")
    if (_reconnect_callback) {
      _reconnect_callback();
    }
  } else {
    Serial.printf("MQTT failed: error=%d, returnCode=%d\n",
                  mqtt_client.lastError(), mqtt_client.returnCode());
    return (0);
  }
  return (1);
}

void mqtt_publish_json(const char* subtopic, const JsonDocument * payload) {

    if (measureJson(*payload) >= 1024) {
      Serial.println("MQTT publish: payload too large");
      return;
    }
    char topicBuf[256];
    char jsonString[1024];
    serializeJson(*payload, jsonString);
    snprintf(topicBuf, sizeof(topicBuf), "%s/%s", topic_device, subtopic);
    if (!mqtt_client.publish(topicBuf, jsonString, true, homie_qos(true))) {  // retained -> QoS 2 (C4)
        Serial.println("MQTT publish: failed");
    }
    Serial.printf("topic: %s, data: %s\n", topicBuf, jsonString);
}

//pull apart a comma-sep colon-delim name:value string and publish the name:value pairs under 'subtopic'
void mqtt_publish_comma_sep_colon_delim(const char* subtopic, const char * data) {
    char topicBuf[384];
    char buf[256];
    Serial.printf("MQTT publish: size:%d chars", strlen(data));
    do {
      int pos = strcspn(data, ":");
      strncpy(buf, data, pos);
      buf[pos] = 0;
      snprintf(topicBuf, sizeof(topicBuf), "%s%s/%s", topic_device, subtopic, buf);
      //topic_ptr[pos] = 0;
      data += pos;
      if (*data++ == 0) {
        break;
      }

      pos = strcspn(data, ",");
      strncpy(mqtt_data, data, pos);
      mqtt_data[pos] = 0;
      data += pos;

      if (!mqtt_client.publish(topicBuf, mqtt_data, true, homie_qos(true))) {  // retained -> QoS 2 (C4)
       Serial.println("MQTT publish: failed");
      }
#ifdef ENABLE_DEBUG_MQTT
      Serial.printf("topic: %s, data: %s\n", topicBuf, mqtt_data);
#endif
    } while (*data++ != 0);
}

// Deliver `value` to every settable subscriber registered for `topic`, firing the same
// three callback paths a remote /set does: the D3 fn-ptr handler, the Property* member
// callback, and the entity callback (Property* overload when a Homie Property exists,
// else the name-based one with the property id parsed back out of the topic).
// Returns true if at least one subscriber matched.
//
// Factored out of subscriber_callback() so deliver_local_set() takes the IDENTICAL route
// as an inbound message. One dispatch path, so a driver never needs to care where a set
// came from.
static bool dispatch_settable(const char* topic, const char* value) {
  bool handled = false;
  for(int i=0;i<num_subscribers;i++) {
    //Serial.printf("MQTT CALLBACK: checking subscriber %d: topic '%s'\n", i, settable_properties[i].topic);
    if (strcmp(settable_properties[i].topic, topic) == 0) {
      handled = true;
      // D3: per-property fn-ptr handler (+context) — the STL-free callback path.
      if (settable_properties[i].handler) {
          Serial.printf("MQTT: Invoking settable handler for topic '%s'\n", topic);
          settable_properties[i].handler(settable_properties[i].handler_ctx, value);
      }
      if (settable_properties[i].property_callback && settable_properties[i].instance) {
        Serial.printf("MQTT: Invoking property callback for topic '%s'\n", topic);
          (settable_properties[i].instance->*settable_properties[i].property_callback)(topic, value);
      }
      if (settable_properties[i].entity_callback && settable_properties[i].entity) {
          if (settable_properties[i].instance) {
              // Homie Property exists — use Property* overload
              Serial.printf("MQTT: Invoking entity callback for topic '%s'\n", topic);
              (settable_properties[i].entity->*settable_properties[i].entity_callback)(settable_properties[i].instance);
          } else {
              // NodeProperty settable — extract property ID from topic, call name-based overload
              const char* set_slash = strrchr(topic, '/');  // /set
              if (set_slash && set_slash > topic) {
                  const char* p = set_slash - 1;
                  while (p > topic && *p != '/') p--;
                  if (*p == '/') p++;
                  char pid[32];
                  int plen = set_slash - p;
                  if (plen > 31) plen = 31;
                  strncpy(pid, p, plen);
                  pid[plen] = '\0';
                  Serial.printf("MQTT: Invoking entity callback '%s'='%s'\n", pid, value);
                  settable_properties[i].entity->entity_settable_callback(pid, value);
              }
          }
      }
    }
  }
  return handled;
}

// Set a property from INSIDE the firmware — no broker, no network, no loopback publish.
// Anything that needs to actuate another node by topic uses this, and it keeps working
// with the broker unreachable. The property's own publish() (called by the driver's
// setter) is what reports the resulting value back to MQTT, exactly as it would for a
// remote /set.
//
// `topic` is the full /set topic, e.g. "homie/5/<device-id>/relay1/state/set".
// Returns false if no settable property is registered for it — the caller should treat
// that as a configuration error (bad node/property id), not a transient failure.
bool deliver_local_set(const char* topic, const char* value) {
  Serial.printf("MQTT: local set '%s' = '%s'\n", topic, value);
  bool handled = dispatch_settable(topic, value);
  if (!handled) {
    Serial.printf("MQTT: local set NOT DELIVERED — no settable property registered for '%s'\n", topic);
  }
  return handled;
}

// MQTT Subscriber callback
//
// all subscriptions arrive here
// Note: Homie $description payloads can be large, use a heap buffer (allocated on
// first use, not a static array — see Device.cpp's _description_json comment) to
// avoid stack overflow.
static char* _mqtt_payload_buf = nullptr;

// MQTTClient uses onMessageAdvanced callback with signature:
// void callback(MQTTClient *client, char topic[], char bytes[], int length)
void subscriber_callback(MQTTClient* client, char topic[], char bytes[], int length) {
  (void)client;  // unused

  //sanity
  if (length > MAX_DATA_LEN - 1) {
    Serial.printf("MQTT CALLBACK: not handled: payload len overrun:%d (max:%d)\n", length, MAX_DATA_LEN - 1);
    return;
  }

  if (_mqtt_payload_buf == nullptr) {
    _mqtt_payload_buf = new char[MAX_DATA_LEN];
  }
  char* payload_buf = _mqtt_payload_buf;
  strncpy(payload_buf, bytes, length);
  payload_buf[length] = '\0'; //ensure null-termination
  Serial.printf("\n***MQTT CALLBACK: topic '%s', payload '%s'\n", topic, payload_buf);

  //route to subscriber property
  bool handled = dispatch_settable(topic, payload_buf);

  // If no property handler matched, forward to fallback callback (e.g., controller)
  // Pass the already null-terminated payload_buf to avoid duplicate buffer allocation
  if (!handled && _fallback_callback) {
    _fallback_callback(topic, (uint8_t*)payload_buf, length);
  }
}

void setup_mqtt() {
  // Resolve broker based on mqtt_mode
  if (mqtt_mode == MQTT_MODE_CONFIGURED_ONLY) {
    // Use only the configured broker
    if (!has_mqtt_broker()) {
      Serial.println("MQTT: **ERROR -- no broker configured (mqtt_mode=configured-only)");
      return;
    }
    Serial.println("MQTT: Using configured broker (mqtt_mode=configured-only)");
  } else if (mqtt_mode == MQTT_MODE_DISCOVERY_WITH_FALLBACK) {
    // Prefer mDNS-discovered broker, fall back to config
    if (mdns_has_broker()) {
      strncpy(mqtt_host, mdns_get_broker_host(), sizeof(mqtt_host) - 1);
      mqtt_host[sizeof(mqtt_host) - 1] = '\0';
      mqtt_port = mdns_get_broker_port();
      mqtt_invalidate_ca_cert();
      // Discovered brokers use anonymous connection, ignore config credentials
      //(temporary until credential negotiation)
      mqtt_username[0] = '\0';
      mqtt_password[0] = '\0';
      Serial.printf("MQTT: Using mDNS broker %s:%u anonymous (mqtt_mode=discovery-with-fallback)\n",
                     mqtt_host, mqtt_port);
    } else if (has_mqtt_broker()) {
      Serial.println("MQTT: mDNS found nothing, using configured broker (mqtt_mode=discovery-with-fallback)");
    } else {
      Serial.println("MQTT: **ERROR -- no mDNS broker and no config broker (mqtt_mode=discovery-with-fallback)");
      return;
    }
  } else {
    // MQTT_MODE_DISCOVERY_ONLY (default) - only mDNS, ignore config
    if (mdns_has_broker()) {
      strncpy(mqtt_host, mdns_get_broker_host(), sizeof(mqtt_host) - 1);
      mqtt_host[sizeof(mqtt_host) - 1] = '\0';
      mqtt_port = mdns_get_broker_port();
      mqtt_invalidate_ca_cert();
      // Discovered brokers use anonymous connection, ignore config credentials
      mqtt_username[0] = '\0';
      mqtt_password[0] = '\0';
      Serial.printf("MQTT: Using mDNS broker %s:%u anonymous (mqtt_mode=discovery-only)\n",
                     mqtt_host, mqtt_port);
    } else {
      Serial.println("MQTT: **ERROR -- no broker discovered via mDNS (mqtt_mode=discovery-only)");
      return;
    }
  }

  Serial.printf("MQTT: %s broker %s:%u\n",
                 mqtt_is_secure() ? "MQTTS" : "MQTT", mqtt_host, mqtt_port);

  generateTopics();

  // Initialize transport (secure or insecure based on port)
  if (!mqtt_init_transport()) {
    Serial.println("MQTT: FAILED TO INITIALIZE TRANSPORT");
    return;
  }

  if (!mqtt_connect()) {
    delay(250);
    if (!mqtt_connect()) {
      delay(500);
      if (!mqtt_connect()) {
        Serial.println("MQTT: FAILED TO CONNECT");
        return;
      }
    }
  }
  mqtt_interval_ts = now();
}

unsigned long last_connect_error = 0;
static unsigned long last_loop_success = 0;

static unsigned long last_connected_log = 0;
static bool was_connected = false;

void loop_mqtt() {
    // Must call loop() frequently to handle keepalive pings and receive messages
    // MQTTClient::loop() returns false if connection was lost
    bool loop_ok = mqtt_client.loop();

    bool is_connected = mqtt_client.connected();

    if (is_connected && loop_ok) {
        // Connection is healthy
        last_loop_success = millis();

        // Log connection status periodically (every 30 seconds)
        if (millis() - last_connected_log > 30000) {
            last_connected_log = millis();
            unsigned long uptime = millis() / 1000;
            Serial.printf("MQTT: connected OK%s (uptime: %lus) %s/%s\n",
                          mqtt_using_secure ? " [TLS]" : "", uptime, mqtt_host, topic_device);
        }
        was_connected = true;
    } else {
        // Connection lost or loop failed
        if (was_connected) {
            // Just disconnected - log immediately
            Serial.printf("MQTT: DISCONNECTED! error=%d, loop_ok=%d, uptime=%lus\n",
                          mqtt_client.lastError(), loop_ok, millis() / 1000);
            was_connected = false;
        }

        if (millis() - last_connect_error > MQTTPublish_rootrate) {
            last_connect_error = millis();
            Serial.printf("MQTT not connected (error=%d) - retrying: %s\n",
                          mqtt_client.lastError(), mqtt_host);
            mqtt_connect();
        }
    }
    
    // Process queued publish requests from other tasks (thread-safe)
    // Do this AFTER loop() to prioritize keepalive pings
    mqtt_process_queue();
    mqtt_interval_ts = millis();
}

void mqtt_restart() {
  if (mqtt_client.connected()) {
    mqtt_client.disconnect();
  }
  // Reset transport client to force re-initialization on next connect
  transportClient = nullptr;
}

boolean mqtt_connected() {
  return mqtt_client.connected();
}