# Changelog

## [Unreleased]

### Added
- `docs/mpm_manager_integration/Street-EMS_MPM_design_v0.1.md`: design draft for
  an optional Street-EMS -> MPM Manager (EnAccess Foundation) REST metering
  side-channel alongside OPENAMI, for the Monday design review with Glenn.
  Covers the firmware-side build/runtime flag pattern, the single-threaded
  `loop()` vs. FreeRTOS task decision for failure isolation, and a research
  plan for the still-unconfirmed MPM ingestion API and GroundBolt/EarthSpark
  interop questions. Updated with source-level findings from MPM's actual
  `ITokenIssuer` manufacturer-plugin interface (vending-only, no reading-ingest
  or remote-switch method) and live confirmation from the open demo instance
  that MPM's dashboard has no alarm/control UI.
- `include/comms/mpm_rest_client.h`, `src/comms/mpm_rest_client.cpp`: first-cut
  MPM Manager REST side-channel, gated by new build flag `MPM_REST_ENABLE`
  (`platformio.ini`, off by default) and independent runtime flag
  (`mpm_rest_set_enabled()`). Uploads the shared per-meter metering snapshot
  (`readings[]`) on its own schedule (`MPMUpload_rate`, `config.h`/`config.cpp`,
  default 5 min) without polling Modbus again or touching OPENAMI/MQTT.
  Transport is a logged mock (`[MPM mock] ...` to Serial) rather than a real
  HTTPS POST, since MPM's actual ingestion contract isn't confirmed yet (see
  the design doc above) - `mpm_send_reading()` is the single function to
  replace once it is. Wired into `src/core/main.cpp` setup()/loop() following
  the existing `ENABLE_*` subsystem pattern.
- `docs/reic_ops_dashboard_mockup/ops_dashboard_mockup.html`: clickable, no-backend
  concept mockup of the live alarm/command dashboard Jude (REI Cameroon) asked for
  on the Nov 20 call - status pills, AC/DC leakage meters against the real firmware
  thresholds (30 mA AC / 6 mA DC), acknowledge/trip/restore actions, and an activity
  log, all client-side with sample data. Explicitly scoped as the safety/alarm layer
  alongside MPM Manager (billing system of record), not a replacement for it.
- `docs/mpm_manager_integration/Street-EMS_OPENAMI_MPM_Detailed_Design_Proposal_v0.3.md`:
  markdown conversion of Glenn Algie's own authored v0.3 design proposal
  (source: `Street-EMS_OPENAMI_MPM_Detailed_Design_Proposal_v0.3.docx`), now
  the primary reference for Monday's review - full architecture, data
  contract, reliability/security requirements, 36-question decision register,
  work packages and phased rollout, preserved as authored. Added a new §3a
  answering several of its open questions (notably Q05: no existing
  manufacturer plugin can be reused for inbound ingestion, confirmed by
  reading `ITokenIssuer` and two plugin implementations directly) plus
  reconciliation notes against this repo's actual conventions (no FreeRTOS
  tasks anywhere today, no C++ classes anywhere today) and against the
  already-built mock scaffold above.
- `include/comms/mpm_rest_client.h`, `src/comms/mpm_rest_client.cpp`: upgraded
  the mock payload to match the design doc's §7 data contract field-for-field
  (`schema`, `gateway_id` via the existing `getDeviceID()`, `meter_id`,
  `mpm_meter_id`, `mapping_version`, `sequence`, UTC ISO8601 `timestamp` via
  `TimeLib`, `phase`, nested `readings.*`, `quality`), serialized with
  `ArduinoJson` matching `mqtt_client.cpp`'s existing convention.
  `mpm_rest_set_mapping()` now takes the string `meter_id`/`mpm_meter_id`/
  `mapping_version` §6 proposes instead of a bare numeric ID. `sequence` is a
  monotonic in-RAM counter only - not yet persisted across reboot, which is
  explicitly out of scope until design doc §8's outbox work starts. Build
  verified both with `MPM_REST_ENABLE` forced on and at its off-by-default
  setting.
