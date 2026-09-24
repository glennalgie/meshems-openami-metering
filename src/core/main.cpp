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
 *   -DENABLE_WIFI            WiFi connectivity (mutually exclusive with ENABLE_ETHERNET)
 *   -DENABLE_ETHERNET        W5500 SPI Ethernet via HR961160C (BOARD_VER_V3 only; mutually exclusive with ENABLE_WIFI)
 *   -DENABLE_MQTT            MQTT client (requires ENABLE_WIFI or ENABLE_ETHERNET)
 *   -DENABLE_CAN             CAN bus via MCP2515 (separate SPI instance)
 *   -DENABLE_MODBUS_MASTER   Modbus master / RS485_1
 *   -DENABLE_MODBUS_CLIENT   Modbus client/slave / RS485_2
 *   -DENABLE_RELAYS          Relay control: onboard SSR + I2C 8-channel SSR bank (PCF8574)
 *   -DENABLE_SD_CARD         SD card reader via SPI (BOARD_VER_V3 only)
 *   -DENABLE_SD_SHT20_LOG    Log SHT20 readings to /environmental_log.csv; interval set by SD_SHT20_LOG_INTERVAL_MS (default 60 s)
 *   -DENABLE_DEBUG           Extra debug Serial logging
 * ============================================================
 */

#include <Arduino.h>

// ---- Core (always included) -------------------------------------------------
#include <core/config.h>
#include <core/data_model.h>
#include <core/pins.h>
#include <hw/buttons.h>

// ---- SPI (shared between display, CAN, SD card, and Ethernet; include once) -
#if defined(ENABLE_OLED_DISPLAY) || defined(ENABLE_CAN) || defined(ENABLE_SD_CARD) || defined(ENABLE_ETHERNET)
  #include <SPI.h>
#endif

// ---- OLED Display -----------------------------------------------------------
#ifdef ENABLE_OLED_DISPLAY
  #include <hw/display.h>
  #include <core/console.h>
#endif

// ---- CAN Bus ----------------------------------------------------------------
#ifdef ENABLE_CAN
  #include <comms/can.h>
#endif

// ---- Network interface ------------------------------------------------------
// Exactly one of ENABLE_WIFI or ENABLE_ETHERNET must be defined when networking
// is needed. ENABLE_MQTT requires at least one.
#if defined(ENABLE_WIFI) && defined(ENABLE_ETHERNET)
  #error "ENABLE_WIFI and ENABLE_ETHERNET are mutually exclusive. Enable only one."
#endif

#ifdef ENABLE_MQTT
  #if !defined(ENABLE_WIFI) && !defined(ENABLE_ETHERNET)
    #error "ENABLE_MQTT requires ENABLE_WIFI or ENABLE_ETHERNET."
  #endif
#endif

#ifdef ENABLE_WIFI
  #include <comms/wifi.h>
#endif

#ifdef ENABLE_ETHERNET
  #include <comms/ethernet.h>
#endif

// ---- MQTT -------------------------------------------------------------------
#ifdef ENABLE_MQTT
  #include <comms/mqtt_client.h>
#endif

// ---- Modbus -----------------------------------------------------------------
// Use the specific headers rather than the aggregate modbus.h so that enabling
// only one side does not drag in declarations for the other.
#ifdef ENABLE_MODBUS_MASTER
  #include <metering/modbus_master.h>
#endif

#ifdef ENABLE_MODBUS_CLIENT
  #include <metering/modbus_client.h>
#endif

// ---- Relays (onboard SSR + I2C 8-channel SSR bank) -------------------------
#ifdef ENABLE_RELAYS
  #include <hw/relay.h>
  #include <hw/i2c_ssr_bank.h>
#endif

// ---- SD Card ----------------------------------------------------------------
#ifdef ENABLE_SD_CARD
  #include <hw/sd_card.h>
#endif

// ---- SD SHT20 CSV Logger ----------------------------------------------------
#ifdef ENABLE_SD_SHT20_LOG
  #include <hw/sd_logger.h>
#endif


// ============================================================
// spi_cs_init()
// ============================================================

/**
 * Deassert every chip select on the shared SPI bus before any device is
 * initialised.
 *
 * SPI chip selects are ACTIVE LOW. Until a GPIO is explicitly configured it is
 * a floating input, and a floating CS can read as LOW — which SELECTS that
 * device. The W5500, the SD card and all ATM90E32 ICs sit on one bus (MOSI 11 /
 * MISO 13 / CLK 12), so a single floating CS lets a second device drive MISO at
 * the same time as whichever chip is being addressed. Every byte read comes
 * back corrupted.
 *
 * This bit the Ethernet bring-up: setup_ethernet() runs early and performs a
 * blocking DHCP exchange, but the ATM90E32 chip selects are not configured
 * until setup_atm90e32() runs much later, and SD_CS not until setup_sd_card()
 * later still. DHCP therefore ran with several CS lines floating and failed.
 *
 * Called first in setup(), before any SPI.begin() — matching EVPE, which does
 * the same thing in its own spi_cs_init() (EVPE/src/main.cpp:131) for exactly
 * this reason.
 *
 * NOTE: only pins for compiled-in features are driven here. If an ATM90E32
 * stack board is PHYSICALLY attached but excluded by ATM90E32_NUM_BOARDS, its
 * CS lines still float and can still corrupt the bus. Match
 * ATM90E32_NUM_BOARDS to the hardware actually fitted.
 */
static void spi_cs_init() {
    // Set all SPI chip-select pins HIGH (they are active low).
#ifdef ENABLE_ETHERNET
    pinMode(ETH_CS, OUTPUT);
    digitalWrite(ETH_CS, HIGH);
#endif

#ifdef ENABLE_SD_CARD
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
#endif

#ifdef ENABLE_OLED_DISPLAY
    pinMode(DISPLAY_CS_PIN, OUTPUT);
    digitalWrite(DISPLAY_CS_PIN, HIGH);
#endif

#ifdef ENABLE_CAN
    pinMode(CAN0_CS, OUTPUT);
    digitalWrite(CAN0_CS, HIGH);
#endif

#ifdef METER_TYPE_ATM90E32
    // Two ICs per stacked board; only the boards actually configured.
    pinMode(ATM90E32_IC1_CS, OUTPUT);   digitalWrite(ATM90E32_IC1_CS, HIGH);
    pinMode(ATM90E32_IC2_CS, OUTPUT);   digitalWrite(ATM90E32_IC2_CS, HIGH);
  #if ATM90E32_NUM_BOARDS >= 2
    pinMode(ATM90E32_IC1_CS_1, OUTPUT); digitalWrite(ATM90E32_IC1_CS_1, HIGH);
    pinMode(ATM90E32_IC2_CS_1, OUTPUT); digitalWrite(ATM90E32_IC2_CS_1, HIGH);
  #endif
  #if ATM90E32_NUM_BOARDS >= 3
    pinMode(ATM90E32_IC1_CS_2, OUTPUT); digitalWrite(ATM90E32_IC1_CS_2, HIGH);
    pinMode(ATM90E32_IC2_CS_2, OUTPUT); digitalWrite(ATM90E32_IC2_CS_2, HIGH);
  #endif
#endif

    Serial.println("INFO - SPI chip selects deasserted");
}

// ============================================================
// setup()
// ============================================================

// One-time hardware and subsystem initialisation.
// Runs in order: Serial → SPI CS deassert → device ID → display splash →
// WiFi/Ethernet → MQTT → Modbus master → Modbus client → CAN → buttons → relays.
// Each subsystem is compiled in only when its feature flag is defined;
// see the feature-flag table in the file header above.
void setup() {
    Serial.begin(115200);
    Serial.println("INFO - Booting");

    // MUST run before any SPI device is touched — see spi_cs_init() above.
    spi_cs_init();

    generateDeviceID();

#ifdef ENABLE_OLED_DISPLAY
    // SPI.begin removed for I2C OLED bring-up — GPIO11/12 are now driven as I2C (SDA/SCL) by the display lib's Wire.begin()
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

#ifdef ENABLE_ETHERNET
    // Return value matters: without a lease there is no IP and no DNS, so every
    // later subsystem that dials out (MQTT especially) will fail in a way that
    // looks like its own bug rather than a network one.
    if (!setup_ethernet()) {
        Serial.println("INFO - Ethernet did not come up; network features will fail until it does");
    }
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

#ifdef ENABLE_SD_CARD
    setup_sd_card();
#endif

#ifdef ENABLE_SD_SHT20_LOG
    setup_sht20_csv_log();
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

#ifdef ENABLE_WIFI
    loop_wifi();
#endif

    // ==================== Modbus Master polling loop ====================
#ifdef ENABLE_MODBUS_MASTER
    if (millis() - lastModbusMillis > (unsigned long)ModbusMaster_pollrate) {
        lastModbusMillis = millis();
        loop_modbus_master();
    }
  #if defined(ENABLE_LEAKAGE_MD0630)
    // S6-A: sense the MD0630 hardware alarm lines fast (every loop), decoupled from
    // the slow Modbus poll above so a trip is logged within ~20 ms.
    service_leakage_alarms();
  #endif
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

#ifdef ENABLE_ETHERNET
    loop_ethernet();
#endif

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

#ifdef ENABLE_SD_SHT20_LOG
    loop_sht20_csv_log();
#endif

}
