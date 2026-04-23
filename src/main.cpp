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
 */

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <modbus.h>     // Modbus communication protocols
#include <buttons.h>    // Button input handling
#include <display.h>    // SH1106 OLED display
#include <console.h>    // Console UI for the display
#include <SPI.h>        // SPI communication for display/CAN
#include <can.h>        // Implementation of CAN bus communication
#include <i2c_ssr_bank.h>
#include <wifi.h>
#include <mqtt_client.h>
#include <config.h>
#include <data_model.h>

static AsyncWebServer server(80);
static const uint8_t kBoards = 2;
static const uint8_t kChannelsPerBoard = 8;
static const uint8_t kHistoryHours = 24;
static bool relayShadow[kBoards][kChannelsPerBoard] = {};

static float simulated_kwh(uint8_t board, uint8_t meter, uint8_t hour) {
    float base = 0.30f + (0.07f * meter) + (0.12f * board);
    float daywave = 0.15f * sinf((float(hour) / 24.0f) * 6.2831853f);
    float peak = (hour >= 18 && hour <= 22) ? 0.22f : 0.0f;
    return max(0.05f, base + daywave + peak);
}

static float daily_kwh_total(uint8_t board, uint8_t meter) {
    float total = 0.0f;
    for (uint8_t hour = 0; hour < kHistoryHours; hour++) {
        total += simulated_kwh(board, meter, hour);
    }
    return total;
}

static void append_geojson(JsonArray features) {
    {
        JsonObject lineFeature = features.add<JsonObject>();
        lineFeature["type"] = "Feature";
        JsonObject props = lineFeature["properties"].to<JsonObject>();
        props["assetType"] = "line";
        props["name"] = "Village feeder";

        JsonObject geom = lineFeature["geometry"].to<JsonObject>();
        geom["type"] = "LineString";
        JsonArray coords = geom["coordinates"].to<JsonArray>();

        JsonArray c0 = coords.add<JsonArray>();
        c0.add(36.8200); c0.add(-1.3020); c0.add(6.0);
        JsonArray c1 = coords.add<JsonArray>();
        c1.add(36.8213); c1.add(-1.3013); c1.add(6.0);
    }

    for (uint8_t board = 0; board < kBoards; board++) {
        float lon = 36.8203f + (0.0007f * board);
        float lat = -1.3018f + (0.0004f * board);

        JsonObject poleFeature = features.add<JsonObject>();
        poleFeature["type"] = "Feature";
        JsonObject props = poleFeature["properties"].to<JsonObject>();
        props["assetType"] = "streetPoleEMS";
        props["boardId"] = board;
        props["name"] = String("StreetPoleEMS-") + board;
        props["height_m"] = 7.5;

        JsonObject geom = poleFeature["geometry"].to<JsonObject>();
        geom["type"] = "Point";
        JsonArray coord = geom["coordinates"].to<JsonArray>();
        coord.add(lon);
        coord.add(lat);
        coord.add(7.5);

        for (uint8_t meter = 0; meter < kChannelsPerBoard; meter++) {
            JsonObject meterFeature = features.add<JsonObject>();
            meterFeature["type"] = "Feature";
            JsonObject meterProps = meterFeature["properties"].to<JsonObject>();
            meterProps["assetType"] = "meter";
            meterProps["boardId"] = board;
            meterProps["meterId"] = meter;
            meterProps["relayChannel"] = meter;
            meterProps["name"] = String("M-") + board + "-" + meter;
            meterProps["dailyKwh"] = daily_kwh_total(board, meter);

            JsonObject meterGeom = meterFeature["geometry"].to<JsonObject>();
            meterGeom["type"] = "Point";
            JsonArray meterCoord = meterGeom["coordinates"].to<JsonArray>();
            meterCoord.add(lon + 0.00012f + (0.00002f * meter));
            meterCoord.add(lat - 0.00010f + (0.00002f * meter));
            meterCoord.add(1.8);
        }
    }
}

static String build_dashboard_json() {
    DynamicJsonDocument doc(28672);

    doc["siteId"] = getDeviceID();
    doc["generatedAtMs"] = millis();
    doc["activeBoardForHardware"] = 0;
    JsonObject villageCenter = doc["villageCenter"].to<JsonObject>();
    villageCenter["lon"] = 36.8208;
    villageCenter["lat"] = -1.3015;
    villageCenter["zoom"] = 16;

    JsonObject geojson = doc["utilityGeoJson"].to<JsonObject>();
    geojson["type"] = "FeatureCollection";
    JsonArray features = geojson["features"].to<JsonArray>();
    append_geojson(features);

    JsonArray boards = doc["boards"].to<JsonArray>();
    for (uint8_t board = 0; board < kBoards; board++) {
        JsonObject boardObj = boards.add<JsonObject>();
        boardObj["boardId"] = board;
        boardObj["name"] = String("StreetPoleEMS-") + board;
        boardObj["hardwareMapped"] = board == 0;

        JsonArray meters = boardObj["meters"].to<JsonArray>();
        for (uint8_t channel = 0; channel < kChannelsPerBoard; channel++) {
            JsonObject meter = meters.add<JsonObject>();
            meter["meterId"] = channel;
            meter["relayChannel"] = channel;
            meter["tenantLabel"] = String("Tenant ") + char('A' + channel);
            meter["relayState"] = relayShadow[board][channel];
            meter["dailyKwhTotal"] = daily_kwh_total(board, channel);

            JsonArray history = meter["historyKwhByHour"].to<JsonArray>();
            for (uint8_t hour = 0; hour < kHistoryHours; hour++) {
                history.add(simulated_kwh(board, channel, hour));
            }
        }
    }

    String payload;
    serializeJson(doc, payload);
    return payload;
}

static bool set_relay_state(uint8_t board, uint8_t channel, bool state) {
    if (board >= kBoards || channel >= kChannelsPerBoard) {
        Serial.printf("Relay API reject: board=%u channel=%u out of range\n", board, channel);
        return false;
    }
    if (board == 0) {
        if (!set_i2c_ssr_channel(channel, state)) {
            Serial.printf("Relay API write failed: board=%u channel=%u state=%u\n", board, channel, state ? 1 : 0);
            return false;
        }
    }
    relayShadow[board][channel] = state;
    Serial.printf("Relay API applied: board=%u channel=%u state=%u\n", board, channel, state ? 1 : 0);
    return true;
}

static void setup_webserver() {
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS mount failed");
        return;
    }

    for (uint8_t channel = 0; channel < 8; channel++) {
        relayShadow[0][channel] = get_i2c_ssr_channel(channel);
        relayShadow[1][channel] = false;
    }

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(SPIFFS, "/index.html", String(), false);
    });

    server.on("/api/dashboard", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "application/json", build_dashboard_json());
    });

    server.on("/api/relay", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("board") || !request->hasParam("channel") || !request->hasParam("state")) {
            request->send(400, "application/json", "{\"ok\":false,\"error\":\"board,channel,state required\"}");
            return;
        }

        uint8_t board = (uint8_t)request->getParam("board")->value().toInt();
        uint8_t channel = (uint8_t)request->getParam("channel")->value().toInt();
        bool state = request->getParam("state")->value().toInt() != 0;

        if (!set_relay_state(board, channel, state)) {
            request->send(500, "application/json", "{\"ok\":false,\"error\":\"relay write failed\"}");
            return;
        }

        String response = "{\"ok\":true,\"board\":";
        response += String(board);
        response += ",\"channel\":";
        response += String(channel);
        response += ",\"state\":";
        response += state ? "true" : "false";
        response += ",\"mask\":";
        response += String(get_i2c_ssr_mask());
        response += "}";
        request->send(200, "application/json", response);
    });

    server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(204);
    });

    server.begin();
    Serial.println("Web server started");
}

void setup() {
    Serial.begin(115200);   // Initialize serial communication for debugging
    Serial.println("INFO - Booting...Setup in 3s");
    delay(3000);
    
    SPI.begin();

    generateDeviceID();
    setup_display();
    _console.addLine(" Display up! next is WiFi/Eth, ");
    _console.addLine(" NTP, MQTT, Modbus, Buttons... ");
    // Display startup splash screen (Rick image)
    drawBitmap(40, 5, RICK_WIDTH, RICK_HEIGHT, rick);
    delay(1000);
    
    drawBitmap(0, 0, LOGO_WIDTH, LOGO_HEIGHT, eIOT_logo); // Render Logo
    delay(1000);

    setup_wifi();
    setup_mqtt_client();
    //setup_powerData_caches(); //TODO maybe? openami to have 4-6 subtopics each with a cache planned , needs cache/buffered data for detecting a pattern change 
    //      for optional use each the time its polled or before each time it gets published  - iterate also for  rules and automation
    
    // Initialize Modbus RTU master/client communication
    setup_modbus_master(); // This sets up communication with sensors like the SHT20 temp/humidity sensor or other devices
    setup_modbus_client();
    //setup_gpio  // ssr, temp_humid, door contact/tamper. shock, imaging)
    setup_can(); // Initialize CAN bus communication

    setup_buttons();
    setup_i2c_ssr_bank();
    setup_webserver();
    _console.addLine(" EMS In-service Ready!");
    _console.addLine("  CHECK MQTT @");
    _console.addLine("  public.cloud.shiftr.io"); //TODO grab the setup strings from the config file
    _console.addLine("  filter OPENAMI/#");       //TODO grab the setup strings from the config file
    if (wifi_client_connected()) {
        String ipAddress = "  Web UI: http://" + get_wifi_ip();
        _console.addLine(ipAddress.c_str());
        Serial.println(ipAddress);
    }
    _console.addLine("  Push a button?");

}


/**
 * @brief Main program loop that runs continuously
 * 
 * Handles periodic tasks and polling:
 * - Check button inputs
 * - Update display
 * - Process CAN bus messages
 * - Handle Modbus master polling
 * - Handle Modbus client requests
 * - Handle MQTT Publish
 * - Handle MQTT cmd responses 
 * 
 */

unsigned long lastModbusMillis = 0;
unsigned long lastMQTTMillis = 0;
unsigned long lastMQTTPollMillis = 0;


void loop() {
   loop_buttons();
   
   if (millis() - lastModbusMillis > ModbusMaster_pollrate) {
        lastModbusMillis = millis();
        loop_modbus_master();
   }

   bool poll_due    = (millis() - lastMQTTPollMillis) > (unsigned long)MQTTPoll_rate;
   bool publish_due = (millis() - lastMQTTMillis)     > (unsigned long)MQTTPublish_rootrate;

   if (poll_due || publish_due) {
        maintain_mqtt_connection();

        if (poll_due) {
            lastMQTTPollMillis = millis();
            poll_mqtt();
        }
        if (publish_due) {
            lastMQTTMillis = millis();
            /*
                TODO  calculate which are the normal periodic work items for this loop based on configurable MQTT periodic and adaptive
                PUBLISH operations
                perhaps  12 or so adaptive Publish global bools turned on or off per loop, the periodic  tasks in the subloops can then be targeted to run
                this allows for adaptive rate of openami topics to be published , these adaptive rates can have a defualt periodicity
                but then time of day schedule can chnage the periodicity of the tasks.
                for example publish a base rate of 30 seconds, publish leaks at period of hourly , publish 3 ph summaries and single tenant meters
                 every 15 min itervals, publish harmnics every 15 mins, publish environmentals every 15 mins ,
                 dont publish stuff on same 15 min  cadence (other than the 3Phase and meters must be on hourly edge cadence)  to minmize peak bandwidths
            */
            loop_mqtt();
        }
   }
  
    loop_modbus_client();
    loop_buttons(); 
    //loop_display();
    //loop_can();
    //loop_i2c_ssr_bank_serial();
    //loop_i2c_ssr_bank_blink_test();
    // TODO loop_IFTTT();
    // TODO loop_alerts();
    
}