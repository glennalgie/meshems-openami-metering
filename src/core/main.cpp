/**
 * @file main.cpp
 * @brief Main application entry point for the Energy IoT Source firmware
 * @author Doug Mendonca, Liam O'Brien
 * @date April 18, 2025
 * 
 * This file contains the setup and main loop for the Energy IoT EMS Dev Platform 
 * controlling the OLED display, MODBUS interfaces, CAN bus interface, 
 * and button interfaces.
 * 
 *  _____                             _____ _____ _____   _____                  _____                          
 * |  ___|                           |_   _|  _  |_   _| |  _  |                /  ___|                         
 * | |__ _ __   ___ _ __ __ _ _   _    | | | | | | | |   | | | |_ __   ___ _ __ \ `--.  ___  _   _ _ __ ___ ___ 
 * |  __| '_ \ / _ \ '__/ _` | | | |   | | | | | | | |   | | | | '_ \ / _ \ '_ \ `--. \/ _ \| | | | '__/ __/ _ \
 * | |__| | | |  __/ | | (_| | |_| |  _| |_\ \_/ / | |   \ \_/ / |_) |  __/ | | /\__/ / (_) | |_| | | | (_|  __/
 * \____/_| |_|\___|_|  \__, |\__, |  \___/ \___/  \_/    \___/| .__/ \___|_| |_\____/ \___/ \__,_|_|  \___\___|
 *                       __/ | __/ |                           | |                                              
 *                      |___/ |___/                            |_|                                              
 *
 * Copyright 2025
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 * ============================================================
 * Feature flags (set in platformio.ini build_flags):
 * ============================================================
 *   -DENABLE_OLED_DISPLAY    SH1106 OLED display over SPI
 *   -DENABLE_WIFI            WiFi connectivity
 *   -DENABLE_MQTT            MQTT client (auto-enables ENABLE_WIFI)
 *   -DENABLE_CAN             CAN bus via MCP2515 (separate SPI instance)
 *   -DENABLE_MODBUS_MASTER   Modbus master / RS485_1
 *   -DENABLE_MODBUS_CLIENT   Modbus client/slave / RS485_2
 *   -DENABLE_RELAYS          Relay control: onboard SSR + I2C 8-channel SSR bank (PCF8574)
 *   -DENABLE_DEBUG           Extra debug Serial logging
 * ============================================================
 */

#include <Arduino.h>

// ---- Core (always included) -------------------------------------------------
#include <config.h>
#include <data_model.h>
#include <pins.h>
#include <buttons.h>

// ---- SPI (shared between display and CAN; include once) --------------------
#if defined(ENABLE_OLED_DISPLAY) || defined(ENABLE_CAN)
  #include <SPI.h>
#endif

// ---- OLED Display -----------------------------------------------------------
#ifdef ENABLE_OLED_DISPLAY
  #include <display.h>
  #include <console.h>
#endif

// ---- CAN Bus ----------------------------------------------------------------
#ifdef ENABLE_CAN
  #include <can.h>
#endif

// ---- WiFi -------------------------------------------------------------------
// MQTT requires WiFi; guard against someone enabling MQTT without WiFi.
#ifdef ENABLE_MQTT
  #ifndef ENABLE_WIFI
    #error "ENABLE_MQTT requires ENABLE_WIFI. Add -DENABLE_WIFI to your build_flags."
  #endif
#endif

#ifdef ENABLE_WIFI
  #include <wifi.h>
#endif

// ---- MQTT -------------------------------------------------------------------
#ifdef ENABLE_MQTT
  #include <mqtt_client.h>
#endif

// ---- Modbus -----------------------------------------------------------------
// Use the specific headers rather than the aggregate modbus.h so that enabling
// only one side does not drag in declarations for the other.
#ifdef ENABLE_MODBUS_MASTER
  #include <modbus_master.h>
#endif

#ifdef ENABLE_MODBUS_CLIENT
  #include <modbus_client.h>
#endif

// ---- Relays (onboard SSR + I2C 8-channel SSR bank) -------------------------
#ifdef ENABLE_RELAYS
  #include <relay.h>
  #include <i2c_ssr_bank.h>
#endif


// ============================================================
// setup()
// ============================================================

// One-time hardware and subsystem initialisation.
// Runs in order: Serial → device ID → display splash → WiFi → MQTT →
// Modbus master → Modbus client → CAN → buttons → relays.
// Each subsystem is compiled in only when its feature flag is defined;
// see the feature-flag table in the file header above.
void setup() {
    Serial.begin(115200);
    Serial.println("INFO - Booting");
    
    generateDeviceID();

#ifdef ENABLE_OLED_DISPLAY
    SPI.begin(DISPLAY_CLK_PIN, -1, DISPLAY_MOSI_PIN, DISPLAY_CS_PIN);
    setup_display();

    _console.addLine(" Display up! next is WiFi/Eth, ");
    _console.addLine(" NTP, MQTT, Modbus, Buttons... ");
    drawBitmap(40, 5, RICK_WIDTH, RICK_HEIGHT, rick);
    delay(1000);
    drawBitmap(0, 0, LOGO_WIDTH, LOGO_HEIGHT, eIOT_logo);
    delay(1000);
#endif

#ifdef ENABLE_WIFI
    setup_wifi();
#endif

#ifdef ENABLE_MQTT
    setup_mqtt_client();
#endif

    // TODO: setup_powerData_caches() — planned per-topic DTM power caches for
    // the 4-6 OpenAMI subtopics (3-phase totals, per-phase, per-meter, leakage,
    // harmonics, environmental).  Uncomment once the cache structs are defined.

#ifdef ENABLE_MODBUS_MASTER
    // Modbus master / "server" on RS485_1 — SHT20 temp/humidity and energy meters
    setup_modbus_master();
#endif

#ifdef ENABLE_MODBUS_CLIENT
    // Modbus slave / "client" on RS485_2
    setup_modbus_client();
#endif

#ifdef ENABLE_CAN
    setup_can();
#endif

    setup_buttons();

#ifdef ENABLE_RELAYS
    setup_relays();
    setup_i2c_ssr_bank();
#endif

#ifdef ENABLE_DEBUG
    Serial.println("INFO - Debug logging enabled");
#endif

#ifdef ENABLE_OLED_DISPLAY
    _console.addLine(" EMS In-service Ready!");
    _console.addLine("  CHECK MQTT @");
    _console.addLine("  public.cloud.shiftr.io"); //TODO pull from config
    _console.addLine("  filter OPENAMI/#");
    _console.addLine("  Push a button?");
#endif
}


// ============================================================
// loop()
// ============================================================

// Timer variables are declared only when the guarded subsystem is active,
// avoiding unused-variable warnings in minimal builds.
#ifdef ENABLE_MODBUS_MASTER
static unsigned long lastModbusMillis = 0;
#endif

#ifdef ENABLE_MQTT
static unsigned long lastMQTTMillis     = 0;  // last MQTT publish timestamp
static unsigned long lastMQTTPollMillis = 0;  // last mqttclient.loop() call timestamp
#endif

// Main firmware loop.  Each subsystem is serviced in priority order:
//   1. Buttons     — always polled for responsive UI
//   2. Modbus      — master and client on their own rate timers
//   3. MQTT        — connection maintenance, then poll and publish on separate rates
//   4. Peripherals — display, CAN, relays at their own internal rates
void loop() {
    loop_buttons();

    // ==================== Modbus Master polling loop ====================
#ifdef ENABLE_MODBUS_MASTER
    if (millis() - lastModbusMillis > (unsigned long)ModbusMaster_pollrate) {
        lastModbusMillis = millis();
        loop_modbus_master();
    }
#endif

    // ==================== Modbus Client polling loop ====================
#ifdef ENABLE_MODBUS_CLIENT
    loop_modbus_client();
#endif

    // ==================== Modbus Client polling loop ====================
#ifdef ENABLE_MODBUS_CLIENT
    loop_modbus_client();
#endif

    // ==================== MQTT polling loop ====================
#ifdef ENABLE_MQTT
    bool poll_due    = false;
    bool publish_due = false;
    unsigned long now = millis();
    if (now - lastMQTTPollMillis > (unsigned long)MQTTPoll_rate) {
        poll_due = true;
    }
    if (now - lastMQTTMillis > (unsigned long)MQTTPublish_rootrate) {
        publish_due = true;
    }

    if (poll_due || publish_due) {
        maintain_mqtt_connection();

        if (poll_due) {
            lastMQTTPollMillis = now;
            poll_mqtt();
        }
        if (publish_due) {
            lastMQTTMillis = now;
            // TODO: implement adaptive publish scheduling.
            // Goal: ~12 per-topic boolean flags control which topics fire each
            // loop_mqtt() call, driven by time-of-day schedule + configurable
            // default periodicities.  Proposed cadences:
            //   - Base readings     : 30 s
            //   - 3-phase summaries : 15 min (staggered from per-meter publishes)
            //   - Per-meter data    : 15 min
            //   - Harmonics         : 15 min
            //   - Environmental     : 15 min
            //   - Leakage           : 60 min
            // 3-phase and meter topics should align to hourly boundaries;
            // stagger other topics to minimise peak upstream bandwidth.
            loop_mqtt();
        }
    }
#endif

    // ==================== Peripheral sub-loops ==========================

#ifdef ENABLE_OLED_DISPLAY
    loop_display();
#endif

#ifdef ENABLE_CAN
    loop_can();
#endif

#ifdef ENABLE_RELAYS
    loop_relays();
    loop_i2c_ssr_bank_serial();
#endif

}
