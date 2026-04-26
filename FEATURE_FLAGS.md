# Feature Flags / Build Configuration Guide

This document describes how to enable or disable specific features in the EMS firmware using PlatformIO build flags.

## Overview

The project uses PlatformIO's `build_flags` configuration in `platformio.ini` to conditionally compile features. This allows you to:
- Reduce firmware binary size by excluding unused features
- Save RAM by not loading unused code
- Enable only the peripherals you need for your specific deployment
- Avoid compilation errors when hardware is not present

## Configuration

Edit `platformio.ini` and add `-D<FEATURE_NAME>` to the `build_flags` section to enable a feature, or comment it out (with `;`) to disable:

```ini
build_flags =
    ; ... other flags ...
    -DENABLE_OLED_DISPLAY    ; Uncomment to enable
    ; -DENABLE_WIFI          ; Comment to disable (note the ; prefix)
```

**Note:** By default, all feature flags are commented out (disabled) in the template. You need to explicitly enable the features you want.

## Available Feature Flags

| Flag | Description | Dependencies | Default |
|------|-------------|--------------|---------|
| `ENABLE_OLED_DISPLAY` | SH1106 OLED display (SPI) | `SPI` | Disabled |
| `ENABLE_WIFI` | WiFi connectivity | None | Disabled |
| `ENABLE_MQTT` | MQTT client (pub/sub) | `ENABLE_WIFI` | Disabled |
| `ENABLE_CAN` | CAN bus (MCP2515 via SPI) | `SPI` | Disabled |
| `ENABLE_MODBUS_MASTER` | Modbus master (RS485_1) | None | Disabled |
| `ENABLE_MODBUS_CLIENT` | Modbus client/slave (RS485_2) | None | Disabled |
| `ENABLE_RELAYS` | Onboard SSR + I2C 8-channel SSR bank (PCF8574) | None | Disabled |
| `ENABLE_DEBUG` | Debug logging to Serial | None | Disabled |

## Examples

### Minimal Setup (UART + Buttons only)
```ini
build_flags =
    -DUSE_SOFTWARE_SERIAL
    -DBOARD_VER_V3
    ; No features enabled - just buttons and serial debug
```

### Basic Monitoring (Display + Buttons)
```ini
build_flags =
    -DUSE_SOFTWARE_SERIAL
    -DBOARD_VER_V3
    -DENABLE_OLED_DISPLAY
    -DENABLE_DEBUG
```

### WiFi Monitoring (Display + WiFi + MQTT)
```ini
build_flags =
    -DUSE_SOFTWARE_SERIAL
    -DBOARD_VER_V3
    -DENABLE_OLED_DISPLAY
    -DENABLE_WIFI
    -DENABLE_MQTT
    -DENABLE_DEBUG
```

### Full Metering (All features)
```ini
build_flags =
    -DUSE_SOFTWARE_SERIAL
    -DCONFIG_ETH_ENABLED
    -DENABLE_RGBWLED
    -DBOARD_VER_V3
    -DENABLE_OLED_DISPLAY
    -DENABLE_WIFI
    -DENABLE_MQTT
    -DENABLE_CAN
    -DENABLE_MODBUS_MASTER
    -DENABLE_MODBUS_CLIENT
    -DENABLE_RELAYS
    -DENABLE_DEBUG
```

### Modbus Only (No display, no WiFi)
```ini
build_flags =
    -DUSE_SOFTWARE_SERIAL
    -DBOARD_VER_V3
    -DENABLE_MODBUS_MASTER
    -DENABLE_MODBUS_CLIENT
    -DENABLE_DEBUG
```

## Automatic Dependencies

Some features automatically enable their dependencies:

- **`ENABLE_MQTT`** automatically enables `ENABLE_WIFI` (MQTT requires WiFi)
  - If you enable `ENABLE_MQTT` without `ENABLE_WIFI`, WiFi will be enabled automatically

## Board Configuration

In addition to feature flags, make sure your board revision is correctly set:

```ini
; Uncomment exactly ONE of these:
-DBOARD_VER_V1    ; Board version 1
-DBOARD_VER_V2    ; Board version 2  
-DBOARD_VER_V3    ; Board version 3 (ESP32-S3 DevKit)
```

## Verifying Your Configuration

After building, check the serial output at boot. You should see messages like:

```
INFO - Booting
INFO - Debug logging enabled
Display up! next is WiFi/Eth, NTP, MQTT, Modbus, Buttons...
[...]
INFO - Debug logging enabled
```

Features that are disabled will not produce any output or consume resources.

## Troubleshooting

### Feature not working after enabling
1. Make sure you rebuilt the project: `pio run` or click Build in VSCode
2. Check for compilation errors in the build output
3. Verify the feature's hardware dependencies are connected
4. Check `platformio.ini` for the correct board revision

### Compilation errors
Some features depend on specific header files. If you get errors like `'display.h' file not found`:
1. Verify the include paths are correct
2. Check that required libraries are installed (see `platformio.ini`)
3. Make sure related feature flags are enabled (e.g., `ENABLE_OLED_DISPLAY` requires `SPI`)

### Binary too large
ESP32-S3 has 16MB flash, but you can still run out of space with all features enabled plus debug logging. To reduce size:
1. Disable features you don't need
2. Disable `ENABLE_DEBUG` (removes debug strings)
3. Disable `ENABLE_OLED_DISPLAY` if not using a display

### RAM usage too high
Features like WiFi and MQTT consume significant RAM. Consider:
1. Disabling unused features
2. Reducing MQTT publish rates
3. Not using the OLED display

## Runtime Behavior

### Disabled Features
- No memory is allocated for disabled features
- No CPU cycles are consumed
- Setup functions are not called
- Loop functions are not executed
- Header files are not included (reducing compile time)

### Enabled Features
- All necessary initialization is performed
- Background tasks run as expected
- Resources (SPI, I2C, GPIO) are claimed
- Data structures are allocated

## Integration with Other Configuration

The feature flags work alongside other PlatformIO build options:

```ini
build_flags =
    ; Feature flags
    -DENABLE_OLED_DISPLAY
    -DENABLE_WIFI
    -DENABLE_MQTT
    
    ; Board configuration
    -DBOARD_VER_V3
    
    ; Other options
    -DUSE_SOFTWARE_SERIAL
    -UARDUINO_USB_CDC_ON_BOOT
```

## Best Practices

1. **Start minimal**: Enable only what you need, add features incrementally
2. **Test after each change**: Rebuild and test when enabling new features
3. **Monitor serial output**: Check for boot messages confirming feature status
4. **Document your config**: Comment your `platformio.ini` to explain why features are enabled
5. **Version control**: Commit changes to `platformio.ini` with clear messages

## Advanced: Conditional Code in Your Projects

You can use the same feature flags in your own code:

```cpp
#ifdef ENABLE_WIFI
  // WiFi-specific code
  setup_wifi();
#endif

#ifdef ENABLE_DEBUG
  Serial.println("Debug message");
#endif
```

This ensures your custom code is only compiled when the corresponding feature is enabled.

## Feature Interdependencies

```
Basic System
    ├── Buttons (always enabled)
    ├── Config (always enabled)
    └── Data Model (always enabled)
        ├── Display
        │   └── Console UI
        ├── WiFi
        │   └── MQTT (requires WiFi)
        ├── CAN Bus (separate SPI instance)
        ├── Modbus Master (RS485_1)
        ├── Modbus Client (RS485_2)
        ├── I2C SSR Bank
        └── Relays
```

## Troubleshooting Checklist

- [ ] `platformio.ini` has correct `build_flags`
- [ ] Board revision (`BOARD_VER_*`) is set
- [ ] Project has been rebuilt (`pio run`)
- [ ] Serial output shows expected boot messages
- [ ] Hardware is properly connected
- [ ] No compilation errors
- [ ] Feature dependencies are met (e.g., WiFi for MQTT)
- [ ] SPI/I2C pins are not conflicting (check `pins.h`)

## Support

For issues related to feature flags:
1. Check this guide
2. Review `platformio.ini` for correct syntax
3. Examine serial output for boot messages
4. Verify hardware connections match enabled features
5. Check compilation output for errors

## Revision History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2026-04-25 | Initial feature flag system documentation |
