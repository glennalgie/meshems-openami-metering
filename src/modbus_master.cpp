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
 * For Modbus meters (DDS238 / CHD130 / DDSU666), communication uses the
 * RS485_1 SoftwareSerial bus at 9600 8N1.  For the ATM90E32, SPI is used
 * instead; the RS-485 bus is not initialised in that mode.
 *
 * Hardware: HW519 RS-485 transceiver on RS485_1_RX / RS485_1_TX (pins.h).
 */

#include <modbus.h>
#include <pins.h>
#include <data_model.h>
#include <config.h>
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
#if defined(METER_TYPE_ATM90E32)
    #include <meter_atm90e32.h>
#else
    // All Modbus meters share the SoftwareSerial RS-485 bus.
    #include <SoftwareSerial.h>
    #include <modbus_sht20.h>
    #if defined(METER_TYPE_DDS238)
        #include <modbus_dds238.h>
    #elif defined(METER_TYPE_CHD130)
        #include <modbus_chd130.h>
    #elif defined(METER_TYPE_DDSU666)
        #include <modbus_ddsu666.h>
    #endif
#endif

// --------------------------------------------------------------------------
// Constants
// --------------------------------------------------------------------------

// RS-485 baud rate for all Modbus RTU meters; DDS238, CHD130, and DDSU666
// all default to 9600 8N1 from the factory.
#define RS485_1_BAUD 9600

#define THERMOSTAT_1_ADDR 0x01

// Modbus node addresses.  Assign sequentially in groups of three per
// subpanel tier and programme each meter before installation.
#define METER_1_ADDR 0x50
#define METER_2_ADDR 0x51
#define METER_3_ADDR 0x52

// --------------------------------------------------------------------------
// Shared objects (Modbus meters only)
// --------------------------------------------------------------------------
#if !defined(METER_TYPE_ATM90E32)
SoftwareSerial _modbus1(RS485_1_RX, RS485_1_TX); // HW519 module
Modbus_SHT20   sht20;

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

#endif // !METER_TYPE_ATM90E32

// Poll timestamp for loop_modbus_master() rate limiting.
static unsigned long lastPollMillis = 0;

// Reserved for a future EVSE charge-session state machine; not yet used.
static unsigned long lastEVSEMillis         = 0;
static unsigned long lastEVSEChargingMillis = 0;

// --------------------------------------------------------------------------
// SHT20 thermostat setup (Modbus meters only)
// --------------------------------------------------------------------------
#if !defined(METER_TYPE_ATM90E32)
// Initialise the SHT20 temperature/humidity sensor at THERMOSTAT_1_ADDR.
static void setup_sht20() {
    Serial.printf("SETUP: MODBUS: SHT20 #1: address:0x%02X\n", THERMOSTAT_1_ADDR);
    sht20.set_modbus_address(THERMOSTAT_1_ADDR);
    sht20.begin(THERMOSTAT_1_ADDR, _modbus1);
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
#if defined(METER_TYPE_ATM90E32)
    // SPI meter: no SHT20 on RS-485; initialise ATM90E32 ICs directly.
    setup_atm90e32();
#else
    setup_sht20();
    setup_meters();
#endif
}

// --------------------------------------------------------------------------
// setup_modbus_master
// --------------------------------------------------------------------------

/**
 * Initialise the energy meter subsystem.
 *
 * For Modbus meters: resets RS-485 GPIO mux assignments, opens SoftwareSerial,
 * then initialises all devices on the bus.
 *
 * For ATM90E32: skips RS-485 setup and delegates directly to setup_atm90e32()
 * which handles SPI initialisation internally.
 */
void setup_modbus_master() {
#if !defined(METER_TYPE_ATM90E32)
    // Reset RS-485 pin mux assignments so SoftwareSerial can claim them cleanly.
    gpio_reset_pin(RS485_1_RX);
    gpio_reset_pin(RS485_1_TX);
    gpio_reset_pin(RS485_2_RX);
    gpio_reset_pin(RS485_2_TX);

    _modbus1.begin(RS485_1_BAUD);
#endif

    setup_modbus_clients();
}

// --------------------------------------------------------------------------
// Data model update — copy cached meter readings into readings[]
// --------------------------------------------------------------------------

/**
 * Copy the latest meter readings into the shared data model.
 * Does not issue any Modbus or SPI transactions; call after polling.
 */
void update() {
#if !defined(METER_TYPE_ATM90E32)
    // SHT20 temperature/humidity into input registers.
    inputRegisters[1] = sht20.getRawTemperature();
    inputRegisters[2] = sht20.getRawHumidity();

    // Energy meter readings — one entry per meter.
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
        readings[i].total_energy  = meters[i]->getTotalEnergy();
        readings[i].export_energy = meters[i]->getExportEnergy();
        readings[i].import_energy = meters[i]->getImportEnergy();
    }

#endif // !METER_TYPE_ATM90E32
    // ATM90E32: poll_atm90e32() writes directly to readings[] — no copy needed.

    // TODO: extend history buffer and CSV output to all meters, not just index 0
    addCurrentReading(readings[0].current);
    Serial.printf("DATA,%lu,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                  millis(), readings[0].current, readings[0].voltage,
                  readings[0].active_power, readings[0].power_factor,
                  readings[0].frequency);
}

// --------------------------------------------------------------------------
// Polling helpers
// --------------------------------------------------------------------------

/**
 * Dump any bytes still in the UART RX buffer — called on SHT20 poll failure
 * to help diagnose partial or absent responses from the sensor.
 */
#if !defined(METER_TYPE_ATM90E32)
static void dump_uart_rx() {
    delay(200);
    uint8_t buf[16];
    uint8_t n = 0;
    while (_modbus1.available() && n < sizeof(buf)) buf[n++] = _modbus1.read();
    if (n > 0) {
        Serial.printf("MODBUS RAW rx (%d bytes):", n);
        for (uint8_t i = 0; i < n; i++) Serial.printf(" 0x%02X", buf[i]);
        Serial.println();
    } else {
        Serial.println("MODBUS RAW rx: nothing — SHT20 sent no response");
    }
}
#endif

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
 * Request temperature and humidity from the SHT20 thermostat (Modbus meters
 * only — ATM90E32 does not share the RS-485 bus).
 * On failure, dump_uart_rx() captures any stray bytes for diagnosis.
 * TODO: extend to an sht20_thermostats[] array for multiple sensors.
 */
void poll_thermostats() {
#if !defined(METER_TYPE_ATM90E32)
    uint8_t result = sht20.poll();
    if (result != 0x00) dump_uart_rx();
    update();
#endif
}

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
#if !defined(METER_TYPE_ATM90E32)
        poll_thermostats();
#endif
        poll_energy_meters();
        lastPollMillis = millis();
    }
}

#endif // ENABLE_MODBUS_MASTER
