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
#include <math.h>
#include <SPI.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <config.h>
#if !HACK_LAB_SKIP_MODBUS
#include <modbus.h>     // Modbus communication protocols
#endif
#include <buttons.h>    // Button input handling
#include <display.h>    // SH1106 OLED display
#include <console.h>    // Console UI for the display
#include <can.h>        // Implementation of CAN bus communication
#include <i2c_ssr_bank.h>
#include <wifi.h>
#include <mqtt_client.h>
#include <data_model.h>
#include <circuitsetup_meter.h>
#include <pins.h>
#include "circuit_paths_generated.h"

static AsyncWebServer server(80);
static const uint8_t kBoards = 1;
static const uint8_t kChannelsPerBoard = 10;
static const uint8_t kHardwareRelayChannels = 8;
/** IoTMug SSR channels 0-5: three tenant households × (Primary + Secondary). Ch 6-7 unused for this model. */
static const uint8_t kCircuitRelayCount = 6;
static const uint8_t kHistoryHours = 24;
static bool relayShadow[kBoards][kChannelsPerBoard] = {};
static const char* kTenantLabels[kChannelsPerBoard] = {
    "Kato Family", "Nankya Family", "Ssewankambo Family", "Nabirye Family", "Okello Family",
    "Achieng Family", "Mugisha Family", "Namusoke Family", "Byaruhanga Family", "Atim Family"
};
static const double kHouseLatLon[kChannelsPerBoard][2] = {
    {0.3626654865345586, 32.53551516882119},
    {0.3626195191078696, 32.53559312333386},
    {0.36257188497217857, 32.53567085303205},
    {0.36251902970261446, 32.53575027281464},
    {0.3625701573901022, 32.53580467541863},
    {0.3625489462228964, 32.53590189711509},
    {0.3624890839504084, 32.535863006262296},
    {0.36242760831352216, 32.53582958827747},
    {0.3623963489621337, 32.535910395715796},
    {0.36228129520905056, 32.535983770700994}
};

/** Apply logical ON/OFF to every house tied to SSR channel `ssrCh` (0-5). */
static void apply_circuit_state_to_house_shadow(uint8_t board, uint8_t ssrCh, bool state) {
    if (board >= kBoards || ssrCh >= kCircuitRelayCount) {
        return;
    }
    switch (ssrCh) {
        case 0:
            relayShadow[board][0] = state;
            break;
        case 1:
            relayShadow[board][1] = state;
            relayShadow[board][2] = state;
            break;
        case 2:
            relayShadow[board][3] = state;
            break;
        case 3:
            relayShadow[board][4] = state;
            relayShadow[board][5] = state;
            relayShadow[board][6] = state;
            relayShadow[board][7] = state;
            break;
        case 4:
            relayShadow[board][8] = state;
            break;
        case 5:
            relayShadow[board][9] = state;
            break;
        default:
            break;
    }
}

static bool ssr_channel_state_from_shadow(uint8_t board, uint8_t ssrCh) {
    if (board >= kBoards || ssrCh >= kCircuitRelayCount) {
        return false;
    }
    switch (ssrCh) {
        case 0:
            return relayShadow[board][0];
        case 1:
            return relayShadow[board][1];
        case 2:
            return relayShadow[board][3];
        case 3:
            return relayShadow[board][4];
        case 4:
            return relayShadow[board][8];
        case 5:
            return relayShadow[board][9];
        default:
            return false;
    }
}

static void sync_house_shadow_from_hardware_ssr(uint8_t board) {
    if (board >= kBoards) {
        return;
    }
    for (uint8_t h = 0; h < kChannelsPerBoard; h++) {
        relayShadow[board][h] = false;
    }
    for (uint8_t ch = 0; ch < kCircuitRelayCount; ch++) {
        bool st = false;
        if (board == 0 && ch < kHardwareRelayChannels) {
            st = get_i2c_ssr_channel(ch);
        }
        apply_circuit_state_to_house_shadow(board, ch, st);
    }
}

static float gaussian_bell(float x, float mu, float sigma) {
    const float z = (x - mu) / sigma;
    return expf(-0.5f * z * z);
}

/** Typical residential dual peak (morning ~8h, evening ~19–20h); trough mid‑afternoon. */
static float simulated_kwh(uint8_t board, uint8_t meter, uint8_t hour) {
    const float m = float(meter);
    const float b = float(board);
    const float t = float(hour) + 0.5f;  // center of clock hour 0..23
    const float phase = m * 0.22f + b * 0.08f;
    const float morning = gaussian_bell(t + phase * 0.08f, 8.0f, 2.1f);
    const float evening = gaussian_bell(t - phase * 0.06f, 19.4f, 2.4f);
    const float dual = morning * 1.05f + evening * 1.28f;
    const float floor = 0.055f + 0.010f * m + 0.06f * b;
    const float peakAmp = 0.22f + 0.045f * fmodf(m, 5.0f) + 0.05f * b;
    float y = floor + peakAmp * dual;
    y *= 1.0f + 0.035f * sinf(t * 0.55f + m * 0.7f);
    return max(0.04f, y);
}

static float daily_kwh_total(uint8_t board, uint8_t meter) {
    float total = 0.0f;
    for (uint8_t hour = 0; hour < kHistoryHours; hour++) {
        total += simulated_kwh(board, meter, hour);
    }
    return total;
}

static constexpr double kDegToRad = 0.017453292519943295;
static constexpr double kPi = 3.14159265358979323846;

/** Closed GeoJSON ring approximating a horizontal circle (meters radius) at (lat, lon). */
static void append_circle_footprint_ring(JsonArray ring, double lat, double lon, double radiusM, int segments) {
    const double cosLat = cos(lat * kDegToRad);
    const double clampedCos = (cosLat > 0.2) ? cosLat : 0.2;
    for (int i = 0; i <= segments; i++) {
        const double ang = (2.0 * kPi * i) / segments;
        const double e = radiusM * cos(ang);
        const double n = radiusM * sin(ang);
        const double la = lat + n / 111320.0;
        const double lo = lon + e / (111320.0 * clampedCos);
        JsonArray pt = ring.add<JsonArray>();
        pt.add(lo);
        pt.add(la);
    }
}

/** Narrow horizontal ribbon in the sky (fill-extrusion base..height) along a ground segment.
 *  If ribbonCircuitId < 255, sets properties.circuitId and a stable GeoJSON feature id for dashboard highlighting. */
static void append_ribbon_segment(JsonArray features, double lat0, double lon0, double lat1, double lon1,
                                  double halfWidthM, float baseM, float topM, const char* assetType,
                                  const char* ribbonName, uint8_t ribbonCircuitId, uint16_t ribbonSegmentIdx) {
    const double midLat = (lat0 + lat1) * 0.5;
    const double cosLat = cos(midLat * kDegToRad);
    const double clampedCos = (cosLat > 0.2) ? cosLat : 0.2;

    const double e1 = (lon1 - lon0) * 111320.0 * clampedCos;
    const double n1 = (lat1 - lat0) * 111320.0;
    const double len = sqrt(e1 * e1 + n1 * n1);
    if (len < 0.05) {
        return;
    }
    const double pe = (-n1 / len) * halfWidthM;
    const double pn = (e1 / len) * halfWidthM;

    const double eA = pe;
    const double nA = pn;
    const double eB = -pe;
    const double nB = -pn;
    const double eC = e1 - pe;
    const double nC = n1 - pn;
    const double eD = e1 + pe;
    const double nD = n1 + pn;

    JsonObject f = features.add<JsonObject>();
    f["type"] = "Feature";
    JsonObject props = f["properties"].to<JsonObject>();
    props["assetType"] = assetType;
    if (ribbonName != nullptr) {
        props["name"] = ribbonName;
    }
    if (ribbonCircuitId < 255) {
        props["circuitId"] = ribbonCircuitId;
        f["id"] = String("car-") + String((unsigned)ribbonCircuitId) + "-s" + String((unsigned)ribbonSegmentIdx);
    }
    props["extrudeBase"] = baseM;
    props["extrudeHeight"] = topM;

    JsonObject geom = f["geometry"].to<JsonObject>();
    geom["type"] = "Polygon";
    JsonArray rings = geom["coordinates"].to<JsonArray>();
    JsonArray ring = rings.add<JsonArray>();

    auto enu_corner = [&](double e, double n) {
        const double la = lat0 + n / 111320.0;
        const double lo = lon0 + e / (111320.0 * clampedCos);
        JsonArray pt = ring.add<JsonArray>();
        pt.add(lo);
        pt.add(la);
    };
    enu_corner(eA, nA);
    enu_corner(eB, nB);
    enu_corner(eC, nC);
    enu_corner(eD, nD);
    enu_corner(eA, nA);
}

static void append_house_extrusion(JsonArray features, double lat, double lon, uint8_t board, uint8_t meter,
                                   bool relayState, float dailyKwh) {
    /** Narrow vertical service-pole style cylinder (small footprint, tall extrusion) — many segments so it reads round in 3D. */
    constexpr double kCylinderRadiusM = 1.75;
    constexpr int kCylinderSegments = 48;

    JsonObject f = features.add<JsonObject>();
    f["type"] = "Feature";
    f["id"] = String("hs-") + String((int)board) + "-" + String((int)meter);
    JsonObject p = f["properties"].to<JsonObject>();
    p["assetType"] = "houseExtrusion";
    p["boardId"] = board;
    p["meterId"] = meter;
    p["relayChannel"] = meter;
    p["name"] = String("House-") + (meter + 1);
    p["tenantLabel"] = kTenantLabels[meter];
    p["dailyKwh"] = dailyKwh;
    p["relayState"] = relayState;
    p["extrudeBase"] = 0.0f;
    p["extrudeHeight"] = 9.5f;

    JsonObject geom = f["geometry"].to<JsonObject>();
    geom["type"] = "Polygon";
    JsonArray rings = geom["coordinates"].to<JsonArray>();
    JsonArray ring = rings.add<JsonArray>();
    append_circle_footprint_ring(ring, lat, lon, kCylinderRadiusM, kCylinderSegments);
}

static void append_pole_extrusion(JsonArray features, double lat, double lon, uint8_t board, const String& name,
                                  float heightM) {
    constexpr double kPoleCylinderRadiusM = 0.45;
    constexpr int kPoleCylinderSegments = 20;

    JsonObject f = features.add<JsonObject>();
    f["type"] = "Feature";
    f["id"] = String("ps-") + String((int)board);
    JsonObject p = f["properties"].to<JsonObject>();
    p["assetType"] = "poleExtrusion";
    p["boardId"] = board;
    p["name"] = name;
    p["height_m"] = heightM;
    p["extrudeBase"] = 0.0f;
    p["extrudeHeight"] = heightM;

    JsonObject geom = f["geometry"].to<JsonObject>();
    geom["type"] = "Polygon";
    JsonArray rings = geom["coordinates"].to<JsonArray>();
    JsonArray ring = rings.add<JsonArray>();
    append_circle_footprint_ring(ring, lat, lon, kPoleCylinderRadiusM, kPoleCylinderSegments);
}

/** Ground circuit LineString from KMZ-derived path (lon, lat, alt); z uses altitude if > 0.5 m else 6 m. */
static void append_circuit_line_from_kmz(JsonArray features, const double (*pts)[3], size_t n, const char* name,
                                         uint8_t circuitId) {
    if (n < 2) {
        return;
    }
    JsonObject lineFeature = features.add<JsonObject>();
    lineFeature["type"] = "Feature";
    lineFeature["id"] = String("cln-") + String((int)circuitId);
    JsonObject props = lineFeature["properties"].to<JsonObject>();
    props["assetType"] = "line";
    props["name"] = name;
    props["circuitId"] = circuitId;

    JsonObject geom = lineFeature["geometry"].to<JsonObject>();
    geom["type"] = "LineString";
    JsonArray coords = geom["coordinates"].to<JsonArray>();
    for (size_t k = 0; k < n; k++) {
        JsonArray c = coords.add<JsonArray>();
        c.add(pts[k][0]); // lon
        c.add(pts[k][1]); // lat
        const double z = (pts[k][2] > 0.5) ? pts[k][2] : 6.0;
        c.add(z);
    }
}

/** Aerial LV ribbons along consecutive vertices of a KMZ path. */
static void append_circuit_air_along_kmz(JsonArray features, const double (*pts)[3], size_t n, const char* ribbonName,
                                         uint8_t circuitId) {
    for (size_t k = 0; k + 1 < n; k++) {
        append_ribbon_segment(features, pts[k][1], pts[k][0], pts[k + 1][1], pts[k + 1][0], 0.35, 4.0f, 4.4f, "courtAir",
                              ribbonName, circuitId, (uint16_t)k);
    }
}

static void append_geojson(JsonArray features) {
    append_circuit_line_from_kmz(features, kCircuitKmz1Points, kCircuitKmz1PointCount, "Circuit 1", 0);
    append_circuit_air_along_kmz(features, kCircuitKmz1Points, kCircuitKmz1PointCount, "Circuit 1 LV", 0);
    append_circuit_line_from_kmz(features, kCircuitKmz2Points, kCircuitKmz2PointCount, "Circuit 2", 1);
    append_circuit_air_along_kmz(features, kCircuitKmz2Points, kCircuitKmz2PointCount, "Circuit 2 LV", 1);
    append_circuit_line_from_kmz(features, kCircuitKmz3Points, kCircuitKmz3PointCount, "Circuit 3", 2);
    append_circuit_air_along_kmz(features, kCircuitKmz3Points, kCircuitKmz3PointCount, "Circuit 3 LV", 2);

    for (uint8_t board = 0; board < kBoards; board++) {
        double centerLon = 0.0;
        double centerLat = 0.0;
        for (uint8_t house = 0; house < kChannelsPerBoard; house++) {
            centerLat += kHouseLatLon[house][0];
            centerLon += kHouseLatLon[house][1];
        }
        centerLat /= (double)kChannelsPerBoard;
        centerLon /= (double)kChannelsPerBoard;

        JsonObject courtFeature = features.add<JsonObject>();
        courtFeature["type"] = "Feature";
        JsonObject courtProps = courtFeature["properties"].to<JsonObject>();
        courtProps["assetType"] = "court";
        courtProps["boardId"] = board;
        courtProps["name"] = "Site outline (all houses)";
        JsonObject courtGeom = courtFeature["geometry"].to<JsonObject>();
        courtGeom["type"] = "LineString";
        JsonArray courtCoords = courtGeom["coordinates"].to<JsonArray>();
        for (uint8_t house = 0; house < kChannelsPerBoard; house++) {
            JsonArray p = courtCoords.add<JsonArray>();
            p.add(kHouseLatLon[house][1]); // lon
            p.add(kHouseLatLon[house][0]); // lat
            p.add(0.5f);
        }
        JsonArray p0 = courtCoords.add<JsonArray>();
        p0.add(kHouseLatLon[0][1]);
        p0.add(kHouseLatLon[0][0]);
        p0.add(0.5f);

        JsonObject poleFeature = features.add<JsonObject>();
        poleFeature["type"] = "Feature";
        poleFeature["id"] = String("pn-") + String((int)board);
        JsonObject props = poleFeature["properties"].to<JsonObject>();
        props["assetType"] = "streetPoleEMS";
        props["boardId"] = board;
        props["name"] = String("StreetPoleEMS-") + board;
        props["height_m"] = 7.5;

        JsonObject geom = poleFeature["geometry"].to<JsonObject>();
        geom["type"] = "Point";
        JsonArray coord = geom["coordinates"].to<JsonArray>();
        coord.add(centerLon);
        coord.add(centerLat);
        coord.add(7.5);

        append_pole_extrusion(features, centerLat, centerLon, board, String("StreetPoleEMS-") + board, 7.5f);

        for (uint8_t meter = 0; meter < kChannelsPerBoard; meter++) {
            JsonObject meterFeature = features.add<JsonObject>();
            meterFeature["type"] = "Feature";
            meterFeature["id"] = String("hn-") + String((int)board) + "-" + String((int)meter);
            JsonObject meterProps = meterFeature["properties"].to<JsonObject>();
            meterProps["assetType"] = "house";
            meterProps["boardId"] = board;
            meterProps["meterId"] = meter;
            meterProps["relayChannel"] = meter;
            meterProps["name"] = String("House-") + (meter + 1);
            meterProps["tenantLabel"] = kTenantLabels[meter];
            meterProps["dailyKwh"] = daily_kwh_total(board, meter);
            meterProps["relayState"] = relayShadow[board][meter];

            JsonObject meterGeom = meterFeature["geometry"].to<JsonObject>();
            meterGeom["type"] = "Point";
            JsonArray meterCoord = meterGeom["coordinates"].to<JsonArray>();
            meterCoord.add(kHouseLatLon[meter][1]); // lon
            meterCoord.add(kHouseLatLon[meter][0]); // lat
            meterCoord.add(1.8);

            append_house_extrusion(features, kHouseLatLon[meter][0], kHouseLatLon[meter][1], board, meter,
                                   relayShadow[board][meter], daily_kwh_total(board, meter));
        }
    }
}

static String build_dashboard_json() {
    DynamicJsonDocument doc(49152);

    doc["siteId"] = getDeviceID();
    doc["generatedAtMs"] = millis();
    doc["meterSource"] = "circuitsetup";
    JsonArray chipsOk = doc["circuitSetupChipsOk"].to<JsonArray>();
    chipsOk.add(circuitsetup_chip_init_ok(0));
    chipsOk.add(circuitsetup_chip_init_ok(1));
    doc["activeBoardForHardware"] = 0;
    JsonObject villageCenter = doc["villageCenter"].to<JsonObject>();
    villageCenter["lon"] = 32.53578;
    villageCenter["lat"] = 0.36251;
    villageCenter["zoom"] = 18;

    JsonObject geojson = doc["utilityGeoJson"].to<JsonObject>();
    geojson["type"] = "FeatureCollection";
    JsonArray features = geojson["features"].to<JsonArray>();
    append_geojson(features);

    mqtt_fill_dashboard_relay_config(doc["mqttRelay"].to<JsonObject>());

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
            meter["tenantLabel"] = kTenantLabels[channel];
            meter["relayState"] = relayShadow[board][channel];
            uint8_t ssrCh = 255;
            const char* role = "";
            if (channel == 0) {
                ssrCh = 0;
                role = "primary";
            } else if (channel == 1 || channel == 2) {
                ssrCh = 1;
                role = "secondary";
            } else if (channel == 3) {
                ssrCh = 2;
                role = "primary";
            } else if (channel >= 4 && channel <= 7) {
                ssrCh = 3;
                role = "secondary";
            } else if (channel == 8) {
                ssrCh = 4;
                role = "primary";
            } else {
                ssrCh = 5;
                role = "secondary";
            }
            meter["ssrChannel"] = ssrCh;
            meter["circuitRole"] = role;
            meter["relayChannel"] = ssrCh;
            meter["dailyKwhTotal"] = daily_kwh_total(board, channel);

            const int ct = static_cast<int>(channel) % 6;
            meter["ctChannel"] = ct;
            meter["ctLabel"] = String("CT") + (ct + 1);
            {
                const float la = circuitsetup_latest_amps(ct);
                const float lv = circuitsetup_phase_voltage(ct);
                meter["liveAmps"] = la;
                meter["liveVolts"] = lv;
                meter["liveKwEst"] = (la * lv) / 1000.0f;
            }

            JsonArray history = meter["historyKwhByHour"].to<JsonArray>();
            for (uint8_t hour = 0; hour < kHistoryHours; hour++) {
                history.add(simulated_kwh(board, channel, hour));
            }
        }

        JsonArray tenantHouseholds = boardObj["tenantHouseholds"].to<JsonArray>();
        struct HouseholdUi {
            const char* title;
            uint8_t priCh;
            uint8_t secCh;
            uint8_t priHouse;
            uint8_t secHouses[5];
            uint8_t secCount;
        };
        static const HouseholdUi kHouseholds[] = {
            {"Household 1 — Houses 1-3", 0, 1, 0, {1, 2, 0, 0, 0}, 2},
            {"Household 2 — Houses 4-8", 2, 3, 3, {4, 5, 6, 7, 0}, 4},
            {"Household 3 — Houses 9-10", 4, 5, 8, {9, 0, 0, 0, 0}, 1},
        };
        for (unsigned hi = 0; hi < 3; hi++) {
            const HouseholdUi& h = kHouseholds[hi];
            JsonObject hh = tenantHouseholds.add<JsonObject>();
            hh["householdId"] = (int)hi;
            hh["title"] = h.title;

            JsonObject pri = hh["primary"].to<JsonObject>();
            pri["label"] = "Primary";
            pri["relayChannel"] = h.priCh;
            pri["relayState"] = ssr_channel_state_from_shadow(board, h.priCh);
            JsonArray priIds = pri["houseIds"].to<JsonArray>();
            priIds.add(h.priHouse);
            pri["summary"] = String(kTenantLabels[h.priHouse]);

            JsonObject sec = hh["secondary"].to<JsonObject>();
            sec["label"] = "Secondary";
            sec["relayChannel"] = h.secCh;
            sec["relayState"] = ssr_channel_state_from_shadow(board, h.secCh);
            JsonArray secIds = sec["houseIds"].to<JsonArray>();
            String sum;
            for (uint8_t k = 0; k < h.secCount; k++) {
                const uint8_t hid = h.secHouses[k];
                secIds.add(hid);
                if (sum.length()) {
                    sum += ", ";
                }
                sum += kTenantLabels[hid];
            }
            sec["summary"] = sum;
        }
    }

    String payload;
    serializeJson(doc, payload);
    return payload;
}

static bool set_relay_state(uint8_t board, uint8_t channel, bool state) {
    if (board >= kBoards || channel >= kCircuitRelayCount) {
        Serial.printf("Relay API reject: board=%u SSR=%u (valid SSR 0-%u)\n", board, channel, (unsigned)(kCircuitRelayCount - 1));
        return false;
    }
    if (board == 0 && channel < kHardwareRelayChannels) {
        if (!set_i2c_ssr_channel(channel, state)) {
            Serial.printf("Relay API write failed: board=%u channel=%u state=%u\n", board, channel, state ? 1 : 0);
            return false;
        }
    }
    apply_circuit_state_to_house_shadow(board, channel, state);
    Serial.printf("Relay API applied: board=%u SSR=%u state=%u (houses updated)\n", board, channel, state ? 1 : 0);
    return true;
}

bool apply_dashboard_relay(uint8_t board, uint8_t channel, bool state) {
    return set_relay_state(board, channel, state);
}

static void setup_webserver() {
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS mount failed");
        return;
    }

    sync_house_shadow_from_hardware_ssr(0);
    /** Lab default: all tenant SSR circuits (0-5) start ON; dashboard toggles only change future behavior in UI. */
    for (uint8_t ch = 0; ch < kCircuitRelayCount; ch++) {
        (void)set_relay_state(0, ch, true);
    }
    for (uint8_t h = 0; h < kChannelsPerBoard; h++) {
        relayShadow[1][h] = false;
    }

    // API routes first — must match before the static file handler.
    server.on("/api/dashboard", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "application/json", build_dashboard_json());
    });

    server.on("/api/circuitsetup", HTTP_GET, [](AsyncWebServerRequest *request) {
        int house = 0;
        if (request->hasParam("house")) {
            house = request->getParam("house")->value().toInt();
        }
        request->send(200, "application/json", circuitsetup_api_json(house));
    });

    server.on("/api/circuitsetup-spi-probe", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "application/json", circuitsetup_spi_probe_json());
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

    // All other GETs: serve from SPIFFS root (index.html, assets). Uses AsyncFileResponse
    // internally — avoids ad-hoc send() paths that returned HTTP 500 on some ESP32 + AsyncWebServer builds.
    server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

    server.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Not found: " + request->url());
    });

    server.begin();
    Serial.println("Web server started");
}

void setup() {
    Serial.begin(115200);   // Initialize serial communication for debugging
    Serial.println("INFO - Booting...Setup in 3s");
    delay(3000);
    
    // Shared SPI: OLED, MCP2515 CAN, and CircuitSetup ATM90E32 all use SCK/MISO/MOSI (see pins.h).
    // ATM90 library uses global SPI — it must match the PCB routing (not the core default VSPI pins).
    SPI.begin(CIRCUITSETUP_SPI_SCK, CIRCUITSETUP_SPI_MISO, CIRCUITSETUP_SPI_MOSI);
#if defined(CAN0_CS)
    pinMode(CAN0_CS, OUTPUT);
    digitalWrite(CAN0_CS, HIGH);
#endif
#if defined(DISPLAY_CS_PIN)
    pinMode(DISPLAY_CS_PIN, OUTPUT);
    digitalWrite(DISPLAY_CS_PIN, HIGH);
#endif

    generateDeviceID();
    setup_display();
    _console.addLine(" Display up! next is WiFi/Eth, ");
    _console.addLine(" NTP, MQTT, CircuitSetup, Buttons... ");
    // Display startup splash screen (Rick image)
    drawBitmap(40, 5, RICK_WIDTH, RICK_HEIGHT, rick);
    delay(1000);
    
    drawBitmap(0, 0, LOGO_WIDTH, LOGO_HEIGHT, eIOT_logo); // Render Logo
    delay(1000);

    setup_wifi();
    setup_mqtt_client();
    //setup_powerData_caches(); //TODO maybe? openami to have 4-6 subtopics each with a cache planned , needs cache/buffered data for detecting a pattern change 
    //      for optional use each the time its polled or before each time it gets published  - iterate also for  rules and automation
    
#if !HACK_LAB_SKIP_MODBUS
    setup_modbus_master();
    setup_modbus_client();
#else
    Serial.println("HACK_LAB_SKIP_MODBUS: RS-485 Modbus disabled; using CircuitSetup for meter data.");
#endif
    //setup_gpio  // ssr, temp_humid, door contact/tamper. shock, imaging)
    setup_can(); // Initialize CAN bus communication

    setup_buttons();
    setup_i2c_ssr_bank();  // I2C PCF8574 — always on; not gated by HACK_LAB_SKIP_MODBUS (RS-485 only)
    setup_circuitsetup_meter();
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
 * - Modbus (optional, off in hack lab)
 * - Handle MQTT Publish
 * - Handle MQTT cmd responses 
 * 
 */

unsigned long lastMQTTMillis = 0;
unsigned long lastMQTTPollMillis = 0;


void loop() {
   loop_buttons();
   
#if !HACK_LAB_SKIP_MODBUS
   static unsigned long lastModbusMillis = 0;
   if (millis() - lastModbusMillis >= (unsigned long)ModbusMaster_pollrate) {
        lastModbusMillis = millis();
        loop_modbus_master();
   }
#endif

   loop_circuitsetup_meter();
   circuitsetup_sync_powerdata_readings();

#if SERIAL_STATUS_INTERVAL_MS > 0
   static unsigned long lastSerialStatusMs = 0;
   {
       const unsigned long nowMs = millis();
       if (nowMs - lastSerialStatusMs >= SERIAL_STATUS_INTERVAL_MS) {
           lastSerialStatusMs = nowMs;
           Serial.printf(
               "STATUS ms=%lu CS_ok=%d/%d CT0=%.2fA CT1=%.2fA r0_I=%.3f WiFi=%s\n",
               nowMs,
               circuitsetup_chip_init_ok(0) ? 1 : 0,
               circuitsetup_chip_init_ok(1) ? 1 : 0,
               circuitsetup_latest_amps(0),
               circuitsetup_latest_amps(1),
               readings[0].current,
               wifi_client_connected() ? "up" : "down");
       }
   }
#endif

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
  
#if !HACK_LAB_SKIP_MODBUS
    loop_modbus_client();
#endif
    loop_buttons(); 
    //loop_display();
    //loop_can();
    //loop_i2c_ssr_bank_serial();
    //loop_i2c_ssr_bank_blink_test();
    // TODO loop_IFTTT();
    // TODO loop_alerts();
    
}