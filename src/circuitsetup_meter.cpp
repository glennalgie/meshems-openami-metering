#include "circuitsetup_meter.h"

#include <ATM90E32.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <stdio.h>
#include <string.h>

#include "pins.h"

#include <TimeLib.h>
#include <data_model.h>

static constexpr int kNumChips = 2;
static constexpr int kNumChannels = 6;
static constexpr int kLongCap = 600;
static constexpr int kShortCap = 60;

static ATM90E32 g_meters[kNumChips];
static bool g_chipOk[kNumChips] = {false, false};
static ATM90E32::Config g_lastMeterConfigs[kNumChips];
static bool g_haveSavedMeterConfigs = false;

static float g_long[kLongCap][kNumChannels];
static float g_short[kShortCap][kNumChannels];
static int g_longHead = 0;
static int g_longCount = 0;
static int g_shortHead = 0;
static int g_shortCount = 0;

static unsigned long g_lastShortMs = 0;
static unsigned long g_lastLongMs = 0;
static unsigned long g_lastDiagMs = 0;

static uint16_t g_sys0[kNumChips] = {0, 0};
static uint16_t g_sys1[kNumChips] = {0, 0};
static uint16_t g_meter0[kNumChips] = {0, 0};
static uint16_t g_meter1[kNumChips] = {0, 0};
static float g_vA[kNumChips] = {0, 0};
static float g_vB[kNumChips] = {0, 0};
static float g_vC[kNumChips] = {0, 0};
static float g_freq[kNumChips] = {0, 0};

static void pushRow(float (*buf)[kNumChannels], int& head, int& count, int cap, const float* row6) {
    memcpy(buf[head], row6, sizeof(float) * kNumChannels);
    head = (head + 1) % cap;
    if (count < cap) {
        count++;
    }
}

void collectChannelSeries(const float (*buf)[kNumChannels], int head, int count, int cap, int channel,
                          JsonArray out) {
    if (count <= 0 || channel < 0 || channel >= kNumChannels) {
        return;
    }
    const int oldest = (count < cap) ? 0 : head;
    for (int i = 0; i < count; i++) {
        const int idx = (oldest + i) % cap;
        out.add(buf[idx][channel]);
    }
}

static int house_to_ct_channel(int houseMeterId) {
    int ch = houseMeterId % kNumChannels;
    if (ch < 0) {
        ch += kNumChannels;
    }
    return ch;
}

float circuitsetup_latest_amps(int ctChannel) {
    if (g_shortCount <= 0 || ctChannel < 0 || ctChannel >= kNumChannels) {
        return 0.0f;
    }
    const int r = (g_shortHead - 1 + kShortCap) % kShortCap;
    return g_short[r][ctChannel];
}

float circuitsetup_phase_voltage(int ctChannel) {
    if (ctChannel < 0 || ctChannel >= kNumChannels) {
        return 0.0f;
    }
    const int chip = ctChannel / 3;
    const int ph = ctChannel % 3;
    if (chip < 0 || chip >= kNumChips) {
        return 0.0f;
    }
    if (ph == 0) {
        return g_vA[chip];
    }
    if (ph == 1) {
        return g_vB[chip];
    }
    return g_vC[chip];
}

bool circuitsetup_chip_init_ok(int chipIndex) {
    if (chipIndex < 0 || chipIndex >= kNumChips) {
        return false;
    }
    return g_chipOk[chipIndex];
}

void circuitsetup_sync_powerdata_readings() {
    for (int i = 0; i < MODBUS_NUM_METERS; i++) {
        const float amps = circuitsetup_latest_amps(i);
        const float volts = circuitsetup_phase_voltage(i);
        readings[i].current = amps;
        readings[i].voltage = volts;
        readings[i].active_power = (amps * volts) / 1000.0f;
        readings[i].reactive_power = 0.0f;
        readings[i].power_factor = (volts > 1.0f && amps > 0.01f) ? 0.99f : 1.0f;
        readings[i].frequency = g_freq[i / 3];
        readings[i].phase = (i % 3) + 1;
        readings[i].timestamp_last_report = now();
    }
}

void setup_circuitsetup_meter() {
    pinMode(CIRCUITSETUP_IRQ0, INPUT);
    pinMode(CIRCUITSETUP_IRQ1, INPUT);

    // Inactive-high: other SPI devices on the shared bus (CAN, OLED) must not drive MISO during meter init.
#if defined(CAN0_CS)
    pinMode(CAN0_CS, OUTPUT);
    digitalWrite(CAN0_CS, HIGH);
#endif
#if defined(DISPLAY_CS_PIN)
    pinMode(DISPLAY_CS_PIN, OUTPUT);
    digitalWrite(DISPLAY_CS_PIN, HIGH);
#endif

    const int csPins[kNumChips] = {CIRCUITSETUP_METER_CS_A, CIRCUITSETUP_METER_CS_B};
    for (int i = 0; i < kNumChips; i++) {
        pinMode(csPins[i], OUTPUT);
        digitalWrite(csPins[i], HIGH);
    }

    for (int i = 0; i < kNumChips; i++) {
        ATM90E32::Config config;
        config.csPin = csPins[i];
        config.lineFrequency = ATM90E32::LINE_FREQUENCY_60HZ;
        config.currentPhases = ATM90E32::CURRENT_PHASES_3;
        config.pgaGain = ATM90E32::PGA_GAIN_1X;
        config.enableGainCalibration = false;
        config.enableOffsetCalibration = false;
        for (int p = 0; p < 3; p++) {
            config.phase[p].voltageGain = 7305;
            config.phase[p].currentGain = 27961;
            config.phase[p].referenceVoltage = 120.0f;
            config.phase[p].referenceCurrent = 50.0f;
        }
        g_lastMeterConfigs[i] = config;
        g_haveSavedMeterConfigs = true;
        g_chipOk[i] = g_meters[i].begin(config);
        if (!g_chipOk[i]) {
            Serial.printf("CircuitSetup: ATM90E32 chip %d init failed (CS GPIO %d)\n", i, csPins[i]);
        } else {
            Serial.printf("CircuitSetup: ATM90E32 chip %d OK (CS GPIO %d)\n", i, csPins[i]);
        }
    }
}

void loop_circuitsetup_meter() {
    const unsigned long now = millis();
    if (now - g_lastShortMs < 500) {
        return;
    }
    g_lastShortMs = now;

    float cur[kNumChannels];
    for (int chip = 0; chip < kNumChips; chip++) {
        if (!g_chipOk[chip]) {
            cur[chip * 3 + 0] = 0.0f;
            cur[chip * 3 + 1] = 0.0f;
            cur[chip * 3 + 2] = 0.0f;
            continue;
        }
        cur[chip * 3 + 0] = static_cast<float>(g_meters[chip].GetLineCurrentA());
        cur[chip * 3 + 1] = static_cast<float>(g_meters[chip].GetLineCurrentB());
        cur[chip * 3 + 2] = static_cast<float>(g_meters[chip].GetLineCurrentC());
    }
    pushRow(g_short, g_shortHead, g_shortCount, kShortCap, cur);

    if (now - g_lastLongMs >= 1000) {
        g_lastLongMs = now;
        pushRow(g_long, g_longHead, g_longCount, kLongCap, cur);
    }

    if (now - g_lastDiagMs >= 1000) {
        g_lastDiagMs = now;
        for (int chip = 0; chip < kNumChips; chip++) {
            if (!g_chipOk[chip]) {
                g_sys0[chip] = g_sys1[chip] = 0;
                g_meter0[chip] = g_meter1[chip] = 0;
                g_vA[chip] = g_vB[chip] = g_vC[chip] = 0;
                g_freq[chip] = 0;
                continue;
            }
            g_sys0[chip] = g_meters[chip].GetSysStatus0();
            g_sys1[chip] = g_meters[chip].GetSysStatus1();
            g_meter0[chip] = g_meters[chip].GetMeterStatus0();
            g_meter1[chip] = g_meters[chip].GetMeterStatus1();
            g_vA[chip] = static_cast<float>(g_meters[chip].GetLineVoltageA());
            g_vB[chip] = static_cast<float>(g_meters[chip].GetLineVoltageB());
            g_vC[chip] = static_cast<float>(g_meters[chip].GetLineVoltageC());
            g_freq[chip] = static_cast<float>(g_meters[chip].GetFrequency());
        }
    }
}

String circuitsetup_api_json(int houseMeterId) {
    const int channel = house_to_ct_channel(houseMeterId);
    const int chipIndex = channel / 3;
    const int phaseIndex = channel % 3;
    static const char* kPhaseLetters = "ABC";

    DynamicJsonDocument doc(32768);
    doc["houseMeterId"] = houseMeterId;
    doc["ctChannel"] = channel;
    doc["ctLabel"] = String("CT") + String(channel + 1);
    doc["chipIndex"] = chipIndex;
    doc["phaseLetter"] = String(kPhaseLetters[phaseIndex]);
    doc["intervalLongMs"] = 1000;
    doc["intervalShortMs"] = 500;
    doc["longCapacity"] = kLongCap;
    doc["shortCapacity"] = kShortCap;
    doc["longSamples"] = g_longCount;
    doc["shortSamples"] = g_shortCount;
    doc["uptimeMs"] = millis();

    JsonArray longArr = doc["longAmps"].to<JsonArray>();
    collectChannelSeries(g_long, g_longHead, g_longCount, kLongCap, channel, longArr);
    JsonArray shortArr = doc["shortAmps"].to<JsonArray>();
    collectChannelSeries(g_short, g_shortHead, g_shortCount, kShortCap, channel, shortArr);

    doc["latestAmps"] = (g_shortCount > 0)
                            ? g_short[(g_shortHead - 1 + kShortCap) % kShortCap][channel]
                            : 0.0f;

    JsonArray allLatest = doc["latestAllAmps"].to<JsonArray>();
    if (g_shortCount > 0) {
        const int r = (g_shortHead - 1 + kShortCap) % kShortCap;
        for (int c = 0; c < kNumChannels; c++) {
            allLatest.add(g_short[r][c]);
        }
    } else {
        for (int c = 0; c < kNumChannels; c++) {
            allLatest.add(0.0f);
        }
    }

    JsonObject diag = doc["diagnostics"].to<JsonObject>();
    diag["spiMosi"] = CIRCUITSETUP_SPI_MOSI;
    diag["spiMiso"] = CIRCUITSETUP_SPI_MISO;
    diag["spiSck"] = CIRCUITSETUP_SPI_SCK;
    diag["csGpioA"] = CIRCUITSETUP_METER_CS_A;
    diag["csGpioB"] = CIRCUITSETUP_METER_CS_B;
    diag["irq0Level"] = digitalRead(CIRCUITSETUP_IRQ0);
    diag["irq1Level"] = digitalRead(CIRCUITSETUP_IRQ1);
    diag["hint"] =
        "ATM90 uses global SPI on SCK/MISO/MOSI from pins.h (same bus as CAN/OLED). If initOk stays "
        "false: verify CS-A/CS-B GPIOs vs 865B netlist, 3V3 to meter, and line voltage inputs. "
        "Idle CS lines (CAN, OLED, both meter CS) must be high.";
    diag["explicitSpiProbe"] = "GET /api/circuitsetup-spi-probe — soft-reset + register echo log per chip (see JSON).";

    JsonArray chips = diag["chips"].to<JsonArray>();
    const int csPins[kNumChips] = {CIRCUITSETUP_METER_CS_A, CIRCUITSETUP_METER_CS_B};
    for (int i = 0; i < kNumChips; i++) {
        JsonObject c = chips.add<JsonObject>();
        c["index"] = i;
        c["csGpio"] = csPins[i];
        c["initOk"] = g_chipOk[i];
        c["sysStatus0"] = g_sys0[i];
        c["sysStatus1"] = g_sys1[i];
        c["meterStatus0"] = g_meter0[i];
        c["meterStatus1"] = g_meter1[i];
        c["voltageA"] = g_vA[i];
        c["voltageB"] = g_vB[i];
        c["voltageC"] = g_vC[i];
        c["frequencyHz"] = g_freq[i];
    }

    String out;
    serializeJson(doc, out);
    return out;
}

namespace {

/** Bit-for-bit same framing as ATM90E32::CommEnergyIC (ESP32: MODE3, 200 kHz). `rw`: 0=write, 1=read (library macros). */
static uint16_t circuitsetup_atm90_raw_comm(int csPin, unsigned char rw, uint16_t addrIn, uint16_t valueIn) {
    uint16_t value = valueIn;
    uint16_t address = addrIn;
    unsigned char* data = reinterpret_cast<unsigned char*>(&value);
    unsigned char* addressData = reinterpret_cast<unsigned char*>(&address);
    uint16_t output;
    uint16_t swappedAddress;

#if defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
    SPISettings settings(200000, MSBFIRST, SPI_MODE1);
#elif defined(ESP32) || defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_SAMD)
    SPISettings settings(200000, MSBFIRST, SPI_MODE3);
#else
    SPISettings settings(200000, MSBFIRST, SPI_MODE0);
#endif

    output = static_cast<uint16_t>((value >> 8) | (value << 8));
    value = output;

    address |= static_cast<uint16_t>(rw) << 15;
    swappedAddress = static_cast<uint16_t>((address >> 8) | (address << 8));
    address = swappedAddress;

#if !defined(ENERGIA)
    SPI.beginTransaction(settings);
#endif

    digitalWrite(csPin, LOW);
    delayMicroseconds(10);

    for (byte i = 0; i < 2; i++) {
        SPI.transfer(*addressData);
        addressData++;
    }

    delayMicroseconds(4);

    if (rw) {
        for (byte i = 0; i < 2; i++) {
            *data = SPI.transfer(0x00);
            data++;
        }
    } else {
        for (byte i = 0; i < 2; i++) {
            SPI.transfer(*data);
            data++;
        }
    }

    digitalWrite(csPin, HIGH);
    delayMicroseconds(10);

#if !defined(ENERGIA)
    SPI.endTransaction();
#endif

    output = static_cast<uint16_t>((value >> 8) | (value << 8));
    return output;
}

static void circuitsetup_spi_bus_all_cs_high() {
#if defined(CAN0_CS)
    pinMode(CAN0_CS, OUTPUT);
    digitalWrite(CAN0_CS, HIGH);
#endif
#if defined(DISPLAY_CS_PIN)
    pinMode(DISPLAY_CS_PIN, OUTPUT);
    digitalWrite(DISPLAY_CS_PIN, HIGH);
#endif
    const int csPins[kNumChips] = {CIRCUITSETUP_METER_CS_A, CIRCUITSETUP_METER_CS_B};
    for (int i = 0; i < kNumChips; i++) {
        pinMode(csPins[i], OUTPUT);
        digitalWrite(csPins[i], HIGH);
    }
}

static void spi_probe_step(JsonArray steps, const char* id, const char* phase, const char* rwLabel,
                           uint16_t regAddr, uint16_t u16Result, const char* expectNote, bool ok) {
    JsonObject o = steps.add<JsonObject>();
    o["id"] = id;
    o["phase"] = phase;
    o["spiRw"] = rwLabel;
    o["regAddr"] = regAddr;
    char rh[8], vh[8];
    snprintf(rh, sizeof rh, "0x%04X", static_cast<unsigned>(regAddr));
    snprintf(vh, sizeof vh, "0x%04X", static_cast<unsigned>(u16Result));
    o["regHex"] = rh;
    o["returnU16"] = u16Result;
    o["returnHex"] = vh;
    o["expect"] = expectNote;
    o["ok"] = ok;
}

}  // namespace

String circuitsetup_spi_probe_json() {
    JsonDocument doc;
    doc["name"] = "CircuitSetup ATM90E32 explicit SPI probe";
    doc["warning"] =
        "Each chip sequence issues SOFTWARE RESET (reg 0x70 <= 0x789A) then CfgRegAccEn unlock test; metering "
        "glitches during probe. Saved configs are re-applied via ATM90E32::begin() after each chip.";
    doc["framing"] =
        "Same as ATM90E32::CommEnergyIC (ESP32): SPI.beginTransaction(200000 Hz, MSBFIRST, SPI_MODE3); CS LOW "
        "10us; shift 2 address bytes (addr with bit15=1 read / 0 write, bytes big-endian on wire after swap); "
        "4us gap; 2 data bytes; CS HIGH 10us; return value is 16-bit word byte-swapped per library.";
    doc["pins"]["spiSck"] = CIRCUITSETUP_SPI_SCK;
    doc["pins"]["spiMosi"] = CIRCUITSETUP_SPI_MOSI;
    doc["pins"]["spiMiso"] = CIRCUITSETUP_SPI_MISO;
    doc["pins"]["meterCsA"] = CIRCUITSETUP_METER_CS_A;
    doc["pins"]["meterCsB"] = CIRCUITSETUP_METER_CS_B;
    doc["pins"]["irq0"] = CIRCUITSETUP_IRQ0;
    doc["pins"]["irq1"] = CIRCUITSETUP_IRQ1;
#if defined(CAN0_CS)
    doc["pins"]["can0Cs"] = CAN0_CS;
#endif
#if defined(DISPLAY_CS_PIN)
    doc["pins"]["displayCs"] = DISPLAY_CS_PIN;
#endif
#if defined(BOARD_VER_V3) && defined(RS485_RX_2)
    if (static_cast<int>(CIRCUITSETUP_SPI_MISO) == static_cast<int>(RS485_RX_2)) {
        doc["pins"]["misoRs485Conflict"] =
            "Firmware maps CIRCUITSETUP_SPI_MISO and RS485_RX_2 to the same GPIO; if RS485-B is wired, "
            "it can prevent the ATM90 from controlling MISO during meter CS.";
    }
#endif

    if (!g_haveSavedMeterConfigs) {
        doc["error"] = "No saved ATM90E32::Config (setup_circuitsetup_meter not run); refusing destructive probe.";
        String out;
        serializeJson(doc, out);
        return out;
    }

    JsonArray chips = doc["chips"].to<JsonArray>();
    const int csPins[kNumChips] = {CIRCUITSETUP_METER_CS_A, CIRCUITSETUP_METER_CS_B};

    Serial.println(F("--- /api/circuitsetup-spi-probe (explicit ATM90 SPI) ---"));

    bool anyUnlockOk = false;
    bool everyChipAllReadsFFFF = true;

    for (int chip = 0; chip < kNumChips; chip++) {
        const int csGpio = csPins[chip];
        JsonObject ch = chips.add<JsonObject>();
        ch["chipIndex"] = chip;
        ch["targetCsGpio"] = csGpio;
        ch["initOkBeforeProbe"] = g_chipOk[chip];
        ch["irq0Level"] = digitalRead(CIRCUITSETUP_IRQ0);
        ch["irq1Level"] = digitalRead(CIRCUITSETUP_IRQ1);

        JsonArray steps = ch["steps"].to<JsonArray>();

        circuitsetup_spi_bus_all_cs_high();
        spi_probe_step(steps, "01", "Park bus: all known CS outputs HIGH", "—", 0, 0,
                       "CAN, display, meter CS-A/B inactive so MISO should tri-state except selected ATM90", true);

        uint16_t r0 = circuitsetup_atm90_raw_comm(csGpio, static_cast<unsigned char>(READ), LastSPIData, 0xFFFF);
        spi_probe_step(steps, "02", "Baseline read LastSPIData (0x78) before reset", "R", LastSPIData, r0,
                       "Any value; documents bus state before soft reset", true);

        uint16_t rSr =
            circuitsetup_atm90_raw_comm(csGpio, static_cast<unsigned char>(WRITE), SoftReset, static_cast<uint16_t>(0x789A));
        spi_probe_step(steps, "03", "SoftReset write 0x789A to reg 0x70", "W", SoftReset, rSr,
                       "Library returns post-transfer byte-swapped value; chip resets after this", true);
        delay(20);

        uint16_t rPost = circuitsetup_atm90_raw_comm(csGpio, static_cast<unsigned char>(READ), LastSPIData, 0xFFFF);
        spi_probe_step(steps, "04", "Read LastSPIData after reset + 20ms", "R", LastSPIData, rPost, "Informational", true);

        uint16_t rW =
            circuitsetup_atm90_raw_comm(csGpio, static_cast<unsigned char>(WRITE), CfgRegAccEn, static_cast<uint16_t>(0x55AA));
        spi_probe_step(steps, "05", "Write CfgRegAccEn (0x7F) = 0x55AA (unlock configure regs)", "W", CfgRegAccEn, rW,
                       "Write phase return per CommEnergyIC convention", true);

        delayMicroseconds(200);
        JsonArray unlockReads = ch["cfgUnlockLastSpiReads"].to<JsonArray>();
        uint16_t rEcho = 0xFFFFu;
        bool unlockOk = false;
        for (int attempt = 0; attempt < 8; attempt++) {
            if (attempt > 0) {
                delayMicroseconds(300);
            }
            rEcho = circuitsetup_atm90_raw_comm(csGpio, static_cast<unsigned char>(READ), LastSPIData, 0xFFFF);
            JsonObject ar = unlockReads.add<JsonObject>();
            ar["attempt"] = attempt;
            ar["returnU16"] = rEcho;
            char evh[8];
            snprintf(evh, sizeof evh, "0x%04X", static_cast<unsigned>(rEcho));
            ar["returnHex"] = evh;
            ar["matches55AA"] = (rEcho == 0x55AAu);
            if (rEcho == 0x55AAu) {
                unlockOk = true;
                break;
            }
        }
        spi_probe_step(steps, "06", "Read LastSPIData after CfgRegAccEn (multi-try + delays)", "R", LastSPIData, rEcho,
                       "0x55AA required; see cfgUnlockLastSpiReads[] (first try after 200us)", unlockOk);

        uint16_t rEn = circuitsetup_atm90_raw_comm(csGpio, static_cast<unsigned char>(READ), MeterEn, 0xFFFF);
        spi_probe_step(steps, "07", "Read MeterEn (0x00)", "R", MeterEn, rEn, "Typically small non-zero after reset; 0xFFFF often means no MISO", true);

        uint16_t rE0 = circuitsetup_atm90_raw_comm(csGpio, static_cast<unsigned char>(READ), EMMState0, 0xFFFF);
        spi_probe_step(steps, "08", "Read EMMState0 (0x71)", "R", EMMState0, rE0, "EMM status flags", true);

        uint16_t rE1 = circuitsetup_atm90_raw_comm(csGpio, static_cast<unsigned char>(READ), EMMState1, 0xFFFF);
        spi_probe_step(steps, "09", "Read EMMState1 (0x72)", "R", EMMState1, rE1, "EMM status flags", true);

        uint16_t rFreq = circuitsetup_atm90_raw_comm(csGpio, static_cast<unsigned char>(READ), Freq, 0xFFFF);
        spi_probe_step(steps, "10", "Read Freq register (0xF8) raw", "R", Freq, rFreq,
                       "If line present, often ~6000 for 60 Hz scaled; flat 0xFFFF / 0x0000 => check VT wiring", true);

        ch["cfgUnlockEchoOk"] = unlockOk;
        if (unlockOk) {
            anyUnlockOk = true;
        }
        const bool allReadFFFF = (r0 == 0xFFFFu && rPost == 0xFFFFu && rEcho == 0xFFFFu && rEn == 0xFFFFu &&
                                  rE0 == 0xFFFFu && rE1 == 0xFFFFu && rFreq == 0xFFFFu);
        ch["allRegisterReads0xFFFF"] = allReadFFFF;
        if (!allReadFFFF) {
            everyChipAllReadsFFFF = false;
        }
        ch["summary"] = unlockOk ? "CfgRegAccEn echo OK — SPI path to this CS likely good."
                                 : "CfgRegAccEn echo FAILED — expect 0x55AA; check CS GPIO, MISO contention, 3V3, wiring.";

        g_chipOk[chip] = g_meters[chip].begin(g_lastMeterConfigs[chip]);
        ch["reBeginAfterProbeOk"] = g_chipOk[chip];
        {
            JsonObject s99 = steps.add<JsonObject>();
            s99["id"] = "99";
            s99["phase"] = "Restore firmware driver state";
            s99["detail"] = "ATM90E32::begin(last saved config for this chip)";
            s99["reBeginOk"] = g_chipOk[chip];
            s99["ok"] = g_chipOk[chip];
        }

        Serial.printf("SPI probe chip %d CS gpio %d unlockEcho=%s reBegin=%s\n", chip, csGpio,
                      unlockOk ? "OK" : "FAIL", g_chipOk[chip] ? "OK" : "FAIL");
    }

    if (!anyUnlockOk && everyChipAllReadsFFFF) {
        doc["interpretation"] =
            "Every READ returned 0xFFFF on both chips: MISO stayed idle-high — the ATM90 never drove the bus. "
            "Hardware checklist: (1) Continuity ESP MISO (GPIO in pins.spiMiso) to ATM90 MISO / stack header. "
            "(2) Meter 3V3 and grounds. (3) CS-A/CS-B GPIOs match the 865B netlist (firmware uses meterCsA/meterCsB). "
            "(4) If pins.misoRs485Conflict is set, do not use RS485-B on the same pin as SPI MISO. "
            "(5) With only one meter IC populated, tie the unused CS high at the chip or leave unselected — wrong CS still gives 0xFFFF on the probed line.";
    }

    doc["doneMs"] = millis();
    String out;
    serializeJson(doc, out);
    return out;
}
