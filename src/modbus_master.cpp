#ifdef ENABLE_MODBUS_MASTER
/**
 * @file modbus_master.cpp
 * @brief Modbus RTU master over RS485_1 — polls SHT20 temperature/humidity
 *        sensors and DDS238 single-phase energy meters.
 *
 * Polling is driven by loop_modbus_master(), which is called from main.cpp on
 * the ModbusMaster_pollrate interval.  After each poll the results are copied
 * into the shared `readings[]` and `inputRegisters[]` arrays defined in
 * data_model.h so that the MQTT and display subsystems can consume them without
 * directly talking to hardware.
 *
 * Hardware: HW519 RS-485 transceiver module wired to RS485_1_RX / RS485_1_TX
 * (defined in pins.h).  All devices share the same half-duplex RS-485 bus.
 */

#include <SoftwareSerial.h>
#include <modbus.h>
#include <pins.h>
#include <data_model.h>
#include <config.h>
#include <math.h>

// ==================== Modbus Device Addresses ====================
// Each device must be staged with its assigned node number before deployment.
// In future a QR code sticker per meter could speed up subpanel staging.

// RS-485 baud rate — must match the factory default on all attached devices.
// DDS238 and SHT20 both default to 9600 8N1; change only if devices are
// re-programmed via their own configuration interfaces.
#define RS485_1_BAUD 9600

#define THERMOSTAT_1_ADDR 0x01

// Meter addresses: assign sequentially in groups of 3 per subpanel tier:
//   3-meter:  0x01-0x03
//   6-meter:  0x04-0x06
//   9-meter:  0x07-0x09
#define DDS238_1_ADDR 0x50
#define DDS238_2_ADDR 0x51
#define DDS238_3_ADDR 0x52

// ==================== Serial Interface ====================
SoftwareSerial _modbus1(RS485_1_RX, RS485_1_TX); // HW519 module pinout

// ==================== Modbus Devices ====================
Modbus_SHT20 sht20;

Modbus_DDS238 dds238_1;
Modbus_DDS238 dds238_2;
Modbus_DDS238 dds238_3;

// Active meter array — update MODBUS_NUM_METERS in data_model.h to match
Modbus_DDS238* dds238_meters[MODBUS_NUM_METERS] = {&dds238_1, &dds238_2, &dds238_3};

// Poll timestamp — compared against millis() in loop_modbus_master() to enforce
// the ModbusMaster_pollrate interval.
unsigned long lastPollMillis = 0;
// Reserved for a future EVSE charge-session state machine; not yet used.
unsigned long lastEVSEMillis = 0;
unsigned long lastEVSEChargingMillis = 0;

// ==================== Setup ====================

// Initialise the SHT20 temperature/humidity sensor at THERMOSTAT_1_ADDR.
// set_modbus_address() stores the address in the driver object before begin()
// opens the underlying SoftwareSerial link.
void setup_sht20() {
    Serial.printf("SETUP: MODBUS: SHT20 #1: address:%d\n", THERMOSTAT_1_ADDR);
    sht20.set_modbus_address(THERMOSTAT_1_ADDR);
    sht20.begin(THERMOSTAT_1_ADDR, _modbus1);
}

// Initialise all DDS238 energy meters on the RS485_1 bus.
// Each meter must be pre-staged with its assigned Modbus node address
// (DDS238_1_ADDR … DDS238_3_ADDR) before installation; see address map above.
void setup_dds238() {
    Serial.printf("SETUP: MODBUS: DDS238 meters: addresses 0x%02X, 0x%02X, 0x%02X\n",
                  DDS238_1_ADDR, DDS238_2_ADDR, DDS238_3_ADDR);
    dds238_1.begin(DDS238_1_ADDR, _modbus1);
    dds238_2.begin(DDS238_2_ADDR, _modbus1);
    dds238_3.begin(DDS238_3_ADDR, _modbus1);
}

// Initialise all Modbus client devices on RS485_1.
// Called from setup_modbus_master() after the serial port is open.
void setup_modbus_clients() {
    setup_sht20();
    setup_dds238();
}

// Initialise the Modbus master subsystem.
// gpio_reset_pin() clears any previous peripheral mux assignments on the
// RS-485 TX/RX pins so SoftwareSerial can take ownership cleanly.
// Both RS485_1 and RS485_2 pin pairs are reset here even though RS485_2 is
// managed by the Modbus client subsystem, to ensure a clean initial state.
void setup_modbus_master() {
    gpio_reset_pin(RS485_1_RX);
    gpio_reset_pin(RS485_1_TX);
    gpio_reset_pin(RS485_2_RX);
    gpio_reset_pin(RS485_2_TX);

    _modbus1.begin(RS485_1_BAUD);

    setup_modbus_clients();
}

// ==================== Data Model Update ====================

/**
 * Copy latest cached readings from all devices into the shared data model.
 * Call after polling — does not trigger any Modbus transactions.
 */
void update() {
    inputRegisters[1] = sht20.getRawTemperature();
    inputRegisters[2] = sht20.getRawHumidity();

    for (int i = 0; i < MODBUS_NUM_METERS; i++) {
        readings[i].current       = dds238_meters[i]->getCurrent();
        readings[i].voltage       = dds238_meters[i]->getVoltage();
        readings[i].active_power  = dds238_meters[i]->getActivePower();
        readings[i].power_factor  = dds238_meters[i]->getPowerFactor();
        readings[i].frequency     = dds238_meters[i]->getFrequency();
        readings[i].total_energy  = dds238_meters[i]->getTotalEnergy();
        readings[i].export_energy = dds238_meters[i]->getExportEnergy();
        readings[i].import_energy = dds238_meters[i]->getImportEnergy();
    }

    // TODO: extend history buffer and CSV output to all meters, not just index 0
    addCurrentReading(readings[0].current);
    Serial.printf("DATA,%lu,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                  millis(), readings[0].current, readings[0].voltage,
                  readings[0].active_power, readings[0].power_factor, readings[0].frequency);
}

// ==================== Polling ====================

/**
 * Dump any bytes still in the UART RX buffer — called on SHT20 poll failure
 * to help diagnose partial / no response from the sensor.
 */
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

// Request a fresh register snapshot from every DDS238 energy meter in the
// active meter array, then push the results to the shared data model.
void poll_energy_meters() {
    for (int i = 0; i < MODBUS_NUM_METERS; i++) {
        dds238_meters[i]->poll();
    }
    update();
}

// Request temperature and humidity from the SHT20 thermostat.
// On a non-zero return code (Modbus exception or timeout) dump_uart_rx() is
// called to capture any stray bytes still in the UART FIFO for diagnosis.
// TODO: extend to an sht20_thermostats[] array for multiple sensors.
void poll_thermostats() {
    uint8_t result = sht20.poll();
    if (result != 0x00) dump_uart_rx();
    update();
}

// ==================== Main Loop ====================

// Modbus master periodic service routine — call from main loop().
// Fires a poll cycle when ModbusMaster_pollrate milliseconds have elapsed since
// the last cycle.  poll_energy_meters() is temporarily disabled while SHT20
// stability is being validated; re-enable when confirmed stable.
void loop_modbus_master() {
    if (millis() - lastPollMillis > ModbusMaster_pollrate) {
        Serial.println("Starting poll cycle...");
        poll_thermostats();
        // poll_energy_meters();  // TODO re-enable once SHT20 is stable
        lastPollMillis = millis();
    }
}

#endif // ENABLE_MODBUS_MASTER
