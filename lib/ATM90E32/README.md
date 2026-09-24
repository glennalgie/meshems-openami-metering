<!-- NESL LOCAL COPY -- READ BEFORE UPGRADING -->
# Local vendored copy

Source: https://github.com/CircuitSetup/ATM90E32 (registry `circuitsetup/ATM90E32`, version 2.0)

This library is vendored rather than pulled from the PlatformIO registry so it
can carry two additions upstream does not have.

## Local modifications

- **`ClearSysStatus0()` / `ClearSysStatus1()`** (`ATM90E32.h`, `ATM90E32.cpp`)
  Write-1-to-clear for the latched `EMMIntState0`/`EMMIntState1` registers.
  Upstream exposes the getters but keeps `Write16Register()` private, so the
  latched flags can never be acknowledged and read as "seen since boot" rather
  than "happened this cycle". Ported from the EVPE vendored fork
  (`EVPE/lib/ATM90E32/ATM90E32.cpp:426`) so both firmwares report identically.

## Why not EVPE's fork instead

EVPE's fork has the clear methods but exposes only lumped `GetImportEnergy()` /
`GetExportEnergy()` -- no per-phase `GetImportEnergyA/B/C`. This project bills
per CT channel, so per-phase energy is mandatory. Upstream 2.0 also carries the
calibration persistence API (`RunGainCalibration`, `SaveGainPreferences`,
`CalibrateVI`, offset calibration) that the staging workflow needs.

## Upgrading

Diff the new upstream release against this copy and re-apply the additions
above. They are marked with `---- LOCAL ADDITION ----` comments in both files.

---

# ATM90E32 Arduino Library

This library powers the [CircuitSetup Split Single Phase Energy Meter](https://github.com/CircuitSetup/Split-Single-Phase-Energy-Meter) and the [6 Channel Expandable Energy Meter](https://github.com/CircuitSetup/Expandable-6-Channel-ESP32-Energy-Meter). 

## Highlights

- Keeps the legacy `begin(...)` API for existing sketches.
- Adds `ATM90E32::Config` and per-phase configuration for ESPHome-style setup.
- Supports 50 Hz / 60 Hz setup, 2-phase / 3-phase mode, harmonic power, peak current, and signed phase angles.
- Adds cumulative per-phase forward/reverse active energy in Wh.
- Adds phase and frequency status helpers.
- Adds gain, voltage/current offset, and power offset calibration flows.
- Saves calibration data on ESP32 with `Preferences`; other Arduino targets keep calibration runtime-only.

## Legacy Setup

Existing sketches can keep using the original `begin(...)` signature:

```cpp
#include <ATM90E32.h>

ATM90E32 energyMeter;

void setup() {
  energyMeter.begin(CS_pin, lineFreq, PGAGain, VoltageGain, CurrentGainCT1, CurrentGainCT2, CurrentGainCT3);
}
```

## Config-Driven Setup

The new API mirrors ESPHome's configuration model more closely:

```cpp
#include <ATM90E32.h>

ATM90E32 energyMeter;

void setup() {
  ATM90E32::Config config;
  config.csPin = 5;
  config.lineFrequency = ATM90E32::LINE_FREQUENCY_60HZ;
  config.currentPhases = ATM90E32::CURRENT_PHASES_2;
  config.pgaGain = ATM90E32::PGA_GAIN_1X;
  config.enableGainCalibration = true;
  config.enableOffsetCalibration = true;

  config.phase[ATM90E32::PHASE_A].voltageGain = 7305;
  config.phase[ATM90E32::PHASE_A].currentGain = 27961;
  config.phase[ATM90E32::PHASE_A].referenceVoltage = 120.0f;
  config.phase[ATM90E32::PHASE_A].referenceCurrent = 5.0f;

  config.phase[ATM90E32::PHASE_B].voltageGain = 7305;
  config.phase[ATM90E32::PHASE_B].currentGain = 27961;

  config.phase[ATM90E32::PHASE_C].voltageGain = 7305;
  config.phase[ATM90E32::PHASE_C].currentGain = 27961;

  if (!energyMeter.begin(config)) {
    Serial.println("ATM90E32 init failed");
  }
}
```

## Common Measurements

```cpp
void loop() {
  double voltageA = energyMeter.GetLineVoltageA();
  double currentA = energyMeter.GetLineCurrentA();
  double powerA = energyMeter.GetActivePowerA();
  double totalPower = energyMeter.GetTotalActivePower();
  double harmonicPowerA = energyMeter.GetHarmonicActivePowerA();
  double peakCurrentA = energyMeter.GetPeakCurrentA();
  double signedAngleA = energyMeter.GetSignedPhaseAngleA();
}
```

Other available getters include:

- `GetReactivePowerA/B/C()` and `GetTotalReactivePower()`
- `GetApparentPowerA/B/C()` and `GetTotalApparentPower()`
- `GetPowerFactorA/B/C()` and `GetTotalPowerFactor()`
- `GetPhaseA/B/C()` for raw phase angle and `GetSignedPhaseAngleA/B/C()` for `[-180, 180]`
- `GetTotalActiveFundPower()` and `GetTotalActiveHarPower()`
- `GetFrequency()`
- `GetTemperature()`

## Energy Counters

Total energy getters keep the legacy behavior and return kWh from the ATM90E32 total registers:

- `GetImportEnergy()`
- `GetImportReactiveEnergy()`
- `GetImportApparentEnergy()`
- `GetExportEnergy()`
- `GetExportReactiveEnergy()`

Per-phase forward/reverse active energy getters are cumulative Wh counters built on the chip's read-and-clear phase registers:

- `GetForwardActiveEnergyA/B/C()`
- `GetReverseActiveEnergyA/B/C()`
- `GetImportEnergyA/B/C()`
- `GetExportEnergyA/B/C()`

You can clear the cumulative per-phase counters in software with:

- `ResetEnergyAccumulators()`

## Status Helpers

Raw status registers are still available:

- `GetSysStatus0()`
- `GetSysStatus1()`
- `GetMeterStatus0()`
- `GetMeterStatus1()`

The new decoded helpers are:

- `GetPhaseStatus(ATM90E32::PHASE_A/B/C)`
- `GetPhaseStatusText(ATM90E32::PHASE_A/B/C)`
- `GetFrequencyStatus()`
- `GetFrequencyStatusText()`

## Calibration

Reference values can be updated in code:

```cpp
energyMeter.SetReferenceVoltage(ATM90E32::PHASE_A, 120.0f);
energyMeter.SetReferenceCurrent(ATM90E32::PHASE_A, 5.0f);
```

Calibration helpers:

- `RunGainCalibration()`
- `RunOffsetCalibration()`
- `RunPowerOffsetCalibration()`
- `ClearGainCalibration()`
- `ClearOffsetCalibration()`
- `ClearPowerOffsetCalibration()`

Legacy helpers are still present:

- `CalculateVIOffset(...)`
- `CalculatePowerOffset(...)`
- `CalibrateVI(...)`

On ESP32, the run/clear calibration methods save and restore calibration values with `Preferences`, keyed by the chip-select pin. On non-ESP32 Arduino targets, the same methods apply calibration values to the ATM90E32 for the current boot only.

## Raw Register Access

Any ATM90E32 register defined in `ATM90E32.h` can still be read directly:

```cpp
double raw = energyMeter.GetValueRegister(UrmsA);
```
