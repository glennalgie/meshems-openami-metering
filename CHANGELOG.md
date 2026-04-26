# Changelog

## [Unreleased] - 2026-04-25

### Added

#### Multi-meter type support and ATM90E32 6-channel meter driver

- `include/meter_atm90e32.h`, `src/meter_atm90e32.cpp` — New SPI driver for the CircuitSetup Expandable 6-Channel ATM90E32 energy meter.  Manages one or more board pairs (2× ATM90E32 ICs per board = 6 CT channels), populates `readings[]` with per-channel voltage, current, active power, apparent power, and power factor.  IC status is checked before each read; unresponsive boards are skipped gracefully.  Debug output is guarded by `ENABLE_DEBUG`.  Calibration constants (`ATM90E32_LINE_FREQ`, `ATM90E32_PGA_GAIN`, `ATM90E32_VOLTAGE_GAIN`, `ATM90E32_CURRENT_GAIN`) are overridable via `platformio.ini` build flags.

- `include/modbus_ddsu666.h`, `src/modbus_ddsu666.cpp` — New Modbus RTU driver for the Chint DDSU666 single-phase energy meter.  Implements the full register map (voltage, current, active/reactive power, power factor, frequency, import/export energy) with correct scaling and signed 32-bit register handling for active/reactive power.  Follows the same class structure as `Modbus_DDS238` and `Modbus_CHD130`.

- `platformio.ini` — Added `METER_TYPE_*` build flags section:
  - `-DMETER_TYPE_DDS238` (active default) — DDS238 Modbus RTU
  - `-DMETER_TYPE_CHD130` (commented) — CHD130 Modbus RTU
  - `-DMETER_TYPE_DDSU666` (commented) — DDSU666 Modbus RTU
  - `-DMETER_TYPE_ATM90E32` (commented) — 6-channel ATM90E32 SPI
  - ATM90E32 library entry commented in `lib_deps` (uncomment to install)

- `FEATURE_FLAGS.md` — Documented all four `METER_TYPE_*` flags, ATM90E32 library installation, CS pin configuration, board count override, calibration overrides, and `readings[]` channel layout.

### Changed

- `src/modbus_master.cpp` — Refactored into a meter-type-agnostic entry point.  RS-485 bus initialisation and SHT20 setup are skipped when `METER_TYPE_ATM90E32` is active.  Per-meter-type setup and poll functions are selected via `#if defined(METER_TYPE_*)` guards.  Default fallback to `METER_TYPE_DDS238` if no meter type flag is set.  `poll_energy_meters()` now re-enables meter polling (was commented out) and dispatches to the correct back-end.  `loop_modbus_master()` now calls `poll_energy_meters()` on every cycle in addition to `poll_thermostats()` (except for ATM90E32 which has no SHT20).

- `include/data_model.h` — `MODBUS_NUM_METERS` is now conditionally defined: `ATM90E32_NUM_BOARDS * 6` when `METER_TYPE_ATM90E32` is set, or `3` otherwise, so `readings[]` is correctly sized for all meter types.

- `include/modbus.h` — Added `#include <modbus_chd130.h>` and `#include <modbus_ddsu666.h>` so all Modbus meter headers are reachable through the aggregate include.  Removed stale `extern Modbus_DDS238* dds238_meters[]` declaration (array is now local to `modbus_master.cpp`).

- `include/pins.h` — Added `ATM90E32_IC1_CS`, `ATM90E32_IC2_CS`, `ATM90E32_MOSI`, `ATM90E32_MISO`, `ATM90E32_CLK` pin definitions for `BOARD_VER_V3`; CS values are GPIO 33/34 placeholders with a TODO note to verify against the board schematic before use.

### Changed

- `src/modbus_master.cpp` — expanded file-level doc comment to describe polling architecture and hardware; added function-level comments for all setup, poll, and loop functions; added rationale comment for `RS485_1_BAUD`; documented reserved EVSE timing variables; fixed `setup_dds238()` log message to print all three meter addresses instead of only the first; split combined timing variable declaration into three separately zero-initialised lines to eliminate partial-initialisation hazard
- `src/mqtt_client.cpp`, `src/relay.cpp`, `src/wifi.cpp`, `src/display.cpp` — added missing trailing newline after closing `#endif` guard to silence compiler "no newline at end of file" warnings
- `src/main.cpp` — added function-level doc comments for `setup()` and `loop()`; documented timer variable declarations; cleaned up stream-of-consciousness MQTT adaptive-publish TODO into structured bullet-point form; expanded the commented-out `setup_powerData_caches()` line with a TODO describing planned per-topic cache work; optimized timer variables to cache `millis()` call, reducing drift and improving accuracy
- `src/mqtt_client.cpp` — optimized MQTT topic/message generation to use stack buffers instead of String concatenation, reducing heap allocations and fragmentation; added reserved buffer sizes to `generateTopics()`
- `AGENTS.md` — reformatted agent instructions to use simplified Do/Don't bullet style; added Commenting Style section with rules for C++ embedded code
- `src/main.cpp` — reduced MQTT loop nesting by computing `poll_due`/`publish_due` flags before connection maintenance
- `src/data_model.cpp` — improved `addCurrentReading()` to use incremental min/max tracking (O(1) common case instead of O(n) full scan), reducing CPU load for timeline graph updates
- `include/data_model.h` — fixed typo `MODUBS_NUM_HOLDING_REGISTERS` → `MODBUS_NUM_HOLDING_REGISTERS`; added missing `#include <stdint.h>`
- `include/modbus_dds238.h` — removed duplicate member variables (`voltage`, `current`, etc.) that shadowed `last_reading` struct, reducing memory usage and confusion
- `src/modbus_dds238.cpp` — fixed bit-shift precedence bug in `read_modbus_extended_value()` (parentheses around shift before OR)

### Added

#### Feature flag build system (`platformio.ini`, `src/main.cpp`, feature `.cpp` files)
- Introduced per-feature `build_flags` in `platformio.ini`: `ENABLE_OLED_DISPLAY`, `ENABLE_WIFI`, `ENABLE_MQTT`, `ENABLE_CAN`, `ENABLE_MODBUS_MASTER`, `ENABLE_MODBUS_CLIENT`, `ENABLE_RELAYS`, `ENABLE_DEBUG` — each conditionally compiles and links its feature
- `src/main.cpp` — rewrote includes and `setup()`/`loop()` to use `#ifdef` guards for every feature; consolidated `SPI.h` include; moved timer variables into their feature guards; replaced silent WiFi auto-enable with a `#error` diagnostic when `ENABLE_MQTT` is set without `ENABLE_WIFI`
- Wrapped feature source files in their respective flag so disabled features produce no object code: `src/display.cpp` (`ENABLE_OLED_DISPLAY`), `src/wifi.cpp` (`ENABLE_WIFI`), `src/can.cpp` (`ENABLE_CAN`), `src/modbus_master.cpp` (`ENABLE_MODBUS_MASTER`), `src/modbus_client.cpp` (`ENABLE_MODBUS_CLIENT`), `src/relay.cpp` (`ENABLE_RELAYS`), `src/i2c_ssr_bank.cpp` (`ENABLE_RELAYS`), `src/mqtt_client.cpp` (`ENABLE_MQTT`)
- Merged `ENABLE_I2C_SSR_BANK` and `ENABLE_RELAYS` into a single `ENABLE_RELAYS` flag — both the onboard SSR (GPIO) and the I2C 8-channel SSR bank are relay hardware and logically belong together
- Added `FEATURE_FLAGS.md` documenting all flags, dependency rules, and example configurations
- Enabled `ENABLE_OLED_DISPLAY`, `ENABLE_WIFI`, `ENABLE_MQTT`, `ENABLE_MODBUS_MASTER`, `ENABLE_DEBUG` as the default active feature set

### Fixed

#### Feature flag bug fixes (`src/main.cpp`, `src/mqtt_client.cpp`)
- Removed duplicate `loop_buttons()` call that ran buttons twice per loop iteration
- Changed `#include <modbus.h>` (aggregate header) under each Modbus guard to the specific `modbus_master.h` / `modbus_client.h` — the aggregate pulled in both sides regardless of which flag was set
- Added missing `ENABLE_RELAYS` wiring: `relay.h` include, `setup_relays()`, and `loop_relays()` calls
- `src/mqtt_client.cpp` — guarded `#include <modbus_master.h>` with `ENABLE_MODBUS_MASTER`, `#include <i2c_ssr_bank.h>` and `cmd_relay` with `ENABLE_I2C_SSR_BANK`, and `readings[]`/`MODBUS_NUM_METERS` usage in `loop_mqtt` with `ENABLE_MODBUS_MASTER`
- Fixed pre-existing typo `#define ENABLE_DEBUG_MQTT = 1` → `#define ENABLE_DEBUG_MQTT 1` in `src/mqtt_client.cpp`

#### FastLED integration (`platformio.ini`)
- Added `fastled/FastLED @ 3.7.8` to `lib_deps` — the library was commented out, causing a missing `FastLED.h` compile error
- Pinned FastLED to **3.7.8** to avoid a `operator new` redefinition conflict introduced in 3.8+, where `fl/inplacenew.h` conflicts with GCC 8.4.0's standard `<new>` header on the Xtensa ESP32-S3 toolchain

#### Build flags (`platformio.ini`)
- Removed `-D__AVR__` from `build_flags` — this flag caused FastLED to compile AVR-specific inline assembly using registers `r0`/`r1` which do not exist on the Xtensa core, resulting in a fatal build error

#### ArduinoJson v7 migration
- **`include/DTMPowerCache.h`** — Changed `buildJson()` return type from deprecated `DynamicJsonDocument` to `JsonDocument`; removed stale commented-out duplicate declaration
- **`src/DTMPowerCache.cpp`** — Updated `buildJson()` return type and replaced `DynamicJsonDocument internalDoc(2048)` with `JsonDocument internalDoc` (v7 manages memory dynamically; size argument no longer required)
- **`include/IFTTTAlerts.h`** — Replaced `DynamicJsonDocument doc(1024)` with `JsonDocument doc`; migrated v6 nested creation API to v7 equivalents:
  - `doc.createNestedArray("alerts")` → `doc["alerts"].to<JsonArray>()`
  - `arr.createNestedObject()` → `arr.add<JsonObject>()`

#### ModbusRTUSlave `SoftwareSerial` support (`lib/ModbusRTUSlave`)
- **`src/ModbusRTUSlave.h`** — Added `ModbusRTUSlave(Stream& serial, uint8_t dePin = NO_DE_PIN)` constructor; accepts any `Stream`-derived serial implementation (including `EspSoftwareSerial`) without requiring `__AVR__` to be defined
- **`src/ModbusRTUSlave.cpp`** — Added corresponding `Stream&` constructor implementation; stores the reference in `_serial` with all other serial pointers set to null, consistent with the existing constructor pattern
