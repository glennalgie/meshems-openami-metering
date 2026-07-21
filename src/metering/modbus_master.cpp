#ifdef ENABLE_MODBUS_MASTER
/**
 * @file modbus_master.cpp
 * @brief Energy meter master subsystem — polls all attached meter hardware.
 *
 * This file is the single entry-point for meter setup and polling regardless
 * of which meter type is selected.  The active meter back-end is chosen at
 * compile time by exactly one of the METER_TYPE_* build flags:
 *
 *   METER_TYPE_DDS238   — DDS238 single-phase Modbus RTU (default)
 *   METER_TYPE_CHD130   — CHINT CHD130 single-phase Modbus RTU
 *   METER_TYPE_DDSU666  — CHINT DDSU666 single-phase Modbus RTU
 *   METER_TYPE_ATM90E32 — CircuitSetup 6-channel ATM90E32 SPI meter
 *
 * If no METER_TYPE_* flag is defined, METER_TYPE_DDS238 is used as the
 * default to preserve backward compatibility.
 *
 * RS-485 bus and SHT20 thermostat are always initialised regardless of
 * meter type.  When METER_TYPE_ATM90E32 is active, energy metering uses SPI
 * while the SHT20 temperature/humidity sensor continues to use RS-485 — the
 * two buses are fully independent.
 *
 * Hardware: HW519 RS-485 transceiver on RS485_1_RX / RS485_1_TX (pins.h).
 */

#include <metering/modbus.h>
#include <core/pins.h>
#include <core/data_model.h>
#include <core/config.h>
#include <math.h>

// --------------------------------------------------------------------------
// Default meter type — keeps existing deployments working unchanged.
// --------------------------------------------------------------------------
#if !defined(METER_TYPE_DDS238)  && \
    !defined(METER_TYPE_CHD130)  && \
    !defined(METER_TYPE_DDSU666) && \
    !defined(METER_TYPE_ATM90E32)
#define METER_TYPE_DDS238
#endif

// --------------------------------------------------------------------------
// Meter-type-specific includes
// --------------------------------------------------------------------------

// SHT20 is always needed: the RS-485 bus hosts the SHT20 temperature/humidity
// sensor regardless of the energy meter type. Use a hardware UART on ESP32-S3;
// EspSoftwareSerial registers GPIO interrupts through the tiny ESP-IDF IPC task
// stack and can trip a stack canary during boot.
#include <metering/modbus_sht20.h>

#if defined(ENABLE_LEAKAGE_MD0630)
    // IVY MD0630 Type-B AC+DC residual-current monitor — shares the RS-485 bus.
    #include <metering/modbus_md0630.h>
    #include <metering/leakage_model_ivy41a.h>
#endif

#if defined(METER_TYPE_ATM90E32)
    #include <metering/meter_atm90e32.h>
#elif defined(METER_TYPE_DDS238)
    #include <metering/modbus_dds238.h>
#elif defined(METER_TYPE_CHD130)
    #include <metering/modbus_chd130.h>
#elif defined(METER_TYPE_DDSU666)
    #include <metering/modbus_ddsu666.h>
#endif

// --------------------------------------------------------------------------
// Constants
// --------------------------------------------------------------------------

// RS-485 baud rate for all Modbus RTU meters; DDS238, CHD130, and DDSU666
// all default to 9600 8N1 from the factory.
#define RS485_1_BAUD 9600

#define THERMOSTAT_1_ADDR 0x01

// IVY MD0630 leakage CT node. Glenn's numbering: 100 single-phase, or 100/101/102
// with one CT per phase. Must not clash with the SHT20 (1) or the meters (1-3).
#define MD0630_ADDR 100

// Modbus node addresses.  Standard factory default for DDS238, CHD130, and
// DDSU666 meters is 0x01/0x02/0x03.  Reprogramme each meter via its front
// panel or configuration tool before installation if a different address is
// required.
#define METER_1_ADDR 0x01
#define METER_2_ADDR 0x02
#define METER_3_ADDR 0x03

// --------------------------------------------------------------------------
// RS-485 bus objects — always present; shared by SHT20 and Modbus meters.
// --------------------------------------------------------------------------
HardwareSerial _modbus1(1);                       // HW519 RS-485 transceiver
Modbus_SHT20   sht20;                             // temperature/humidity sensor

#if defined(ENABLE_LEAKAGE_MD0630)
Modbus_MD0630  md0630;                            // AC+DC residual-current monitor
LeakageModel   leakageModel;                      // published on subpanel_RCMleaks
#endif

// Modbus energy meter objects — only for RTU meter types.
#if defined(METER_TYPE_DDS238)
Modbus_DDS238  dds238_1;
Modbus_DDS238  dds238_2;
Modbus_DDS238  dds238_3;

#elif defined(METER_TYPE_CHD130)
Modbus_CHD130  chd130_1;
Modbus_CHD130  chd130_2;
Modbus_CHD130  chd130_3;

#elif defined(METER_TYPE_DDSU666)
Modbus_DDSU666 ddsu666_1;
Modbus_DDSU666 ddsu666_2;
Modbus_DDSU666 ddsu666_3;
#endif

// Poll timestamp for loop_modbus_master() rate limiting.
static unsigned long lastPollMillis = 0;

// Reserved for a future EVSE charge-session state machine; not yet used.
static unsigned long lastEVSEMillis         = 0;
static unsigned long lastEVSEChargingMillis = 0;

// --------------------------------------------------------------------------
// SHT20 thermostat setup — always present regardless of energy meter type
// --------------------------------------------------------------------------
// Initialise the SHT20 temperature/humidity sensor at THERMOSTAT_1_ADDR.
// The SHT20 sits on the RS-485 bus that is separate from the ATM90E32 SPI bus.
static void setup_sht20() {
    Serial.printf("SETUP: MODBUS: SHT20 #1: address:0x%02X\n", THERMOSTAT_1_ADDR);
    sht20.set_modbus_address(THERMOSTAT_1_ADDR);
    sht20.begin(THERMOSTAT_1_ADDR, _modbus1);
}

#if defined(ENABLE_LEAKAGE_MD0630)
// --------------------------------------------------------------------------
// IVY MD0630 leakage CT setup
// --------------------------------------------------------------------------
// Seeds the life-safety thresholds into the model (AC 30 mA, DC 6 mA — the model
// defaults DC to 30 mA, which is wrong for Type B).
// With LEAKAGE_MOCK the register map is not needed: the driver synthesises a
// rising leakage so the data model, MQTT and Insights run end to end.
static void setup_md0630() {
#if defined(LEAKAGE_PROBE)
    // Bring-up: talk to the module at its FACTORY DEFAULT address 1 (confirmed
    // from IVY's PC tool), not the future node 100 (module not reprogrammed yet).
    const uint8_t addr = 1;
#else
    const uint8_t addr = MD0630_ADDR;
#endif
    Serial.printf("SETUP: MODBUS: MD0630 leakage CT: address:%d\n", addr);
    md0630.set_modbus_address(addr);
    md0630.begin(addr, _modbus1);
    leakageModel.acSinusoidal.threshold_mA = Modbus_MD0630::AC_THRESHOLD_MA;
    leakageModel.dc.threshold_mA           = Modbus_MD0630::DC_THRESHOLD_MA;
#if defined(LEAKAGE_MOCK)
    md0630.setMockRamp(0.0f, 0.5f, 45.0f,    // AC: 0 -> 45 mA at 0.5 mA/s  (crosses 30 mA at ~60 s)
                       0.0f, 0.1f,  9.0f);   // DC: 0 ->  9 mA at 0.1 mA/s  (crosses  6 mA at ~60 s)
    Serial.println("SETUP: MODBUS: MD0630 in MOCK RAMP mode — no hardware required");
#endif
}
#endif

// --------------------------------------------------------------------------
// Per-meter-type setup helpers
// --------------------------------------------------------------------------
#if defined(METER_TYPE_DDS238)
// Initialise three DDS238 meters at their factory-staged Modbus node addresses.
static void setup_meters() {
    Serial.printf("SETUP: MODBUS: DDS238 meters: 0x%02X, 0x%02X, 0x%02X\n",
                  METER_1_ADDR, METER_2_ADDR, METER_3_ADDR);
    dds238_1.begin(METER_1_ADDR, _modbus1);
    dds238_2.begin(METER_2_ADDR, _modbus1);
    dds238_3.begin(METER_3_ADDR, _modbus1);
}

#elif defined(METER_TYPE_CHD130)
// Initialise three CHD130 meters at their factory-staged Modbus node addresses.
static void setup_meters() {
    Serial.printf("SETUP: MODBUS: CHD130 meters: 0x%02X, 0x%02X, 0x%02X\n",
                  METER_1_ADDR, METER_2_ADDR, METER_3_ADDR);
    chd130_1.begin(METER_1_ADDR, _modbus1);
    chd130_2.begin(METER_2_ADDR, _modbus1);
    chd130_3.begin(METER_3_ADDR, _modbus1);
}

#elif defined(METER_TYPE_DDSU666)
// Initialise three DDSU666 meters at their factory-staged Modbus node addresses.
static void setup_meters() {
    Serial.printf("SETUP: MODBUS: DDSU666 meters: 0x%02X, 0x%02X, 0x%02X\n",
                  METER_1_ADDR, METER_2_ADDR, METER_3_ADDR);
    ddsu666_1.begin(METER_1_ADDR, _modbus1);
    ddsu666_2.begin(METER_2_ADDR, _modbus1);
    ddsu666_3.begin(METER_3_ADDR, _modbus1);
}
#endif

// --------------------------------------------------------------------------
// setup_modbus_clients — called by setup_modbus_master after bus init
// --------------------------------------------------------------------------
void setup_modbus_clients() {
    // SHT20 is always initialised — it lives on RS-485 independent of the
    // energy meter type.
    setup_sht20();

#if defined(ENABLE_LEAKAGE_MD0630)
    setup_md0630();
#endif

#if defined(METER_TYPE_ATM90E32)
    // SPI energy meter: initialise ATM90E32 ICs in addition to SHT20.
    setup_atm90e32();
#else
    // Modbus RTU energy meters share the same RS-485 bus as the SHT20.
    setup_meters();
#endif
}

// --------------------------------------------------------------------------
// setup_modbus_master
// --------------------------------------------------------------------------

/**
 * Initialise the energy meter subsystem.
 *
 * Always resets RS-485 GPIO mux assignments and opens SoftwareSerial so the
 * SHT20 sensor is reachable on all meter-type configurations.  For Modbus
 * energy meters the same bus also carries the meter devices.  For ATM90E32,
 * SPI initialisation is handled inside setup_atm90e32().
 */
void setup_modbus_master() {
    // RS-485 bus is always needed: the SHT20 sensor uses it regardless of
    // which energy meter back-end is selected.
    gpio_reset_pin(RS485_1_RX);
    gpio_reset_pin(RS485_1_TX);
    gpio_reset_pin(RS485_2_RX);
    gpio_reset_pin(RS485_2_TX);
    _modbus1.begin(RS485_1_BAUD, SERIAL_8N1, (int)RS485_1_RX, (int)RS485_1_TX);

    setup_modbus_clients();
}

// --------------------------------------------------------------------------
// Data model update — copy cached meter readings into readings[]
// --------------------------------------------------------------------------

/**
 * Copy the latest meter readings into the shared data model.
 * Does not issue any Modbus or SPI transactions; call after polling.
 *
 * SHT20 data is always written to inputRegisters regardless of meter type.
 * Modbus energy meter readings are only written for RTU meter types.
 * ATM90E32: poll_atm90e32() writes directly to readings[] — no copy needed.
 */
void update() {
    // SHT20 temperature/humidity into input registers — always present.
    inputRegisters[1] = sht20.getRawTemperature();
    inputRegisters[2] = sht20.getRawHumidity();

#if !defined(METER_TYPE_ATM90E32)
    // Energy meter readings — one entry per Modbus RTU meter.
    #if defined(METER_TYPE_DDS238)
    Modbus_DDS238* meters[MODBUS_NUM_METERS] = {&dds238_1, &dds238_2, &dds238_3};
    #elif defined(METER_TYPE_CHD130)
    Modbus_CHD130* meters[MODBUS_NUM_METERS] = {&chd130_1, &chd130_2, &chd130_3};
    #elif defined(METER_TYPE_DDSU666)
    Modbus_DDSU666* meters[MODBUS_NUM_METERS] = {&ddsu666_1, &ddsu666_2, &ddsu666_3};
    #endif

    for (int i = 0; i < MODBUS_NUM_METERS; i++) {
        readings[i].current       = meters[i]->getCurrent();
        readings[i].voltage       = meters[i]->getVoltage();
        readings[i].active_power  = meters[i]->getActivePower();
        readings[i].power_factor  = meters[i]->getPowerFactor();
        readings[i].frequency     = meters[i]->getFrequency();
        readings[i].phase         = (int8_t)(i % 3);
        readings[i].total_energy  = meters[i]->getTotalEnergy();
        readings[i].export_energy = meters[i]->getExportEnergy();
        readings[i].import_energy = meters[i]->getImportEnergy();
        readings[i].timestamp_last_report = millis();
    }
#endif // !METER_TYPE_ATM90E32

#ifdef USB_PLOTTER
    // Record each meter's current in its own history buffer and emit a CSV
    // line to the Arduino Serial Plotter.
    // CSV columns: DATA, meter_index, timestamp_ms, current_A, voltage_V,
    //              active_power_kW, power_factor, frequency_Hz
    for (int i = 0; i < MODBUS_NUM_METERS; i++) {
        addCurrentReading(i, readings[i].current);
        Serial.printf("DATA,%d,%lu,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                      i, millis(),
                      readings[i].current, readings[i].voltage,
                      readings[i].active_power, readings[i].power_factor,
                      readings[i].frequency);
    }
#endif // USB_PLOTTER
}

// --------------------------------------------------------------------------
// SHT20 accessors — available to MQTT and other subsystems
// --------------------------------------------------------------------------

// Returns the last measured temperature in degrees Celsius.
float get_sht20_temperature() {
    return sht20.getTemperature();
}

// Returns the last measured relative humidity as a percentage (0–100).
float get_sht20_humidity() {
    return sht20.getHumidity();
}

// Returns the cumulative number of successful SHT20 poll responses.
// A non-zero value indicates that at least one valid reading has been received.
uint16_t get_sht20_success_count() {
    return sht20.getSuccessCount();
}

// --------------------------------------------------------------------------
// Polling helpers
// --------------------------------------------------------------------------

/**
 * Dump any bytes still in the UART RX buffer — called on SHT20 poll failure
 * to help diagnose partial or absent responses from the sensor.
 */
static void dump_uart_rx() {
    delay(200);
    uint8_t buf[16];
    uint8_t n = 0;
    while (_modbus1.available() && n < sizeof(buf)) buf[n++] = _modbus1.read();
#ifdef ENABLE_DEBUG
    if (n > 0) {
        Serial.printf("MODBUS RAW rx (%d bytes):", n);
        for (uint8_t i = 0; i < n; i++) Serial.printf(" 0x%02X", buf[i]);
        Serial.println();
    } else {
        Serial.println("MODBUS RAW rx: nothing — SHT20 sent no response");
    }
#else
    (void)n; // silence unused-variable warning in release builds
#endif
}

/**
 * Request fresh readings from all energy meters and push results to the
 * shared data model.
 */
void poll_energy_meters() {
#if defined(METER_TYPE_ATM90E32)
    poll_atm90e32();
    update();

#elif defined(METER_TYPE_DDS238)
    dds238_1.poll();
    dds238_2.poll();
    dds238_3.poll();
    update();

#elif defined(METER_TYPE_CHD130)
    chd130_1.poll();
    chd130_2.poll();
    chd130_3.poll();
    update();

#elif defined(METER_TYPE_DDSU666)
    ddsu666_1.poll();
    ddsu666_2.poll();
    ddsu666_3.poll();
    update();
#endif
}

/**
 * Request temperature and humidity from the SHT20 thermostat.
 * Always runs regardless of energy meter type — SHT20 uses RS-485 which is
 * independent from the ATM90E32 SPI bus.
 * On failure, dump_uart_rx() captures any stray bytes for diagnosis.
 * TODO: extend to an sht20_thermostats[] array for multiple sensors.
 */
void poll_thermostats() {
    // Only poll the SHT20; do NOT call update() here.
    // update() is called by poll_energy_meters() after meter data is fresh,
    // which prevents the full register-copy from running twice per cycle.
    uint8_t result = sht20.poll();
    if (result != 0x00) dump_uart_rx();
}

#if defined(ENABLE_LEAKAGE_MD0630)
/**
 * Read AC + DC residual current into the leakage model — from the real module,
 * or synthesised when LEAKAGE_MOCK is set. The model is what
 * mqtt_publish_Leakage() serialises onto subpanel_RCMleaks.
 */
void poll_leakage() {
#if defined(LEAKAGE_PROBE)
    // Hardware bring-up: safe FC04-then-FC03 reads from 0x0000, print raw words.
    // No writes. Tells us which function code answers and where the values live.
    md0630.probeRegisters();
#else
    if (md0630.poll() == ModbusMaster::ku8MBSuccess) {
        leakageModel.updateAll(md0630.getAcLeakage_mA(), 0.0f, md0630.getDcLeakage_mA());
    }
#endif
}

LeakageModel& get_leakage_model() { return leakageModel; }
#endif

// --------------------------------------------------------------------------
// Main loop
// --------------------------------------------------------------------------

/**
 * Modbus master periodic service routine — call from main loop().
 * Fires a full poll cycle when ModbusMaster_pollrate milliseconds have elapsed
 * since the last cycle.
 */
void loop_modbus_master() {
    if (millis() - lastPollMillis > ModbusMaster_pollrate) {
        Serial.println("Starting poll cycle...");
        poll_thermostats();   // always poll SHT20 regardless of energy meter type
#if defined(ENABLE_LEAKAGE_MD0630)
        poll_leakage();       // AC+DC residual current (real module or mock ramp)
#endif
        poll_energy_meters();
        lastPollMillis = millis();
    }
}

#endif // ENABLE_MODBUS_MASTER
