#pragma once

// Modbus infrastructure headers — always included when Modbus is enabled.
#include <modbus_master.h>
#include <modbus_client.h>
#include <modbus_sht20.h>

// Meter-type-specific driver headers.
// Exactly one METER_TYPE_* flag should be set in platformio.ini.
// If none is set, DDS238 is the default (see modbus_master.cpp).
#include <modbus_dds238.h>
#include <modbus_chd130.h>
#include <modbus_ddsu666.h>
// ATM90E32 is SPI-based; included separately via meter_atm90e32.h when needed.
