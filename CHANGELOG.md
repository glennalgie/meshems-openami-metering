# Changelog

## [Unreleased] - 2026-04-25

### Changed

- `src/modbus_master.cpp` — expanded file-level doc comment to describe polling architecture and hardware; added function-level comments for all setup, poll, and loop functions; added rationale comment for `RS485_1_BAUD`; documented reserved EVSE timing variables; fixed `setup_dds238()` log message to print all three meter addresses instead of only the first; split combined timing variable declaration into three separately zero-initialised lines to eliminate partial-initialisation hazard
- `src/mqtt_client.cpp`, `src/relay.cpp`, `src/wifi.cpp`, `src/display.cpp` — added missing trailing newline after closing `#endif` guard to silence compiler "no newline at end of file" warnings
- `src/main.cpp` — added function-level doc comments for `setup()` and `loop()`; documented timer variable declarations; cleaned up stream-of-consciousness MQTT adaptive-publish TODO into structured bullet-point form; expanded the commented-out `setup_powerData_caches()` line with a TODO describing planned per-topic cache work
- `AGENTS.md` — reformatted agent instructions to use simplified Do/Don't bullet style; added Commenting Style section with rules for C++ embedded code

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

### Fixed

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
