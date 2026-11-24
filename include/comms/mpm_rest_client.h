#pragma once

// MPM Manager REST metering side-channel (EnAccess Foundation
// MicroPowerManager, github.com/EnAccess/micropowermanager).
//
// Independent, off-by-default side channel that uploads the same per-tenant
// metering snapshot OPENAMI already publishes (readings[] in data_model.h)
// over REST to MPM Manager. Does not replace, depend on, or share failure
// state with OPENAMI/MQTT - see
// docs/mpm_manager_integration/Street-EMS_OPENAMI_MPM_Detailed_Design_Proposal_v0.3.md
// (§6/§7) for the data contract this follows, and
// docs/mpm_manager_integration/Street-EMS_MPM_design_v0.1.md for the research
// behind it.
//
// Build-time gate: MPM_REST_ENABLE (platformio.ini). Runtime gate:
// mpm_rest_set_enabled() - independent of the build flag so uploads can be
// started/stopped without reflashing once a remote-config path exists.
//
// MPM's real device-reading-ingestion contract is not yet confirmed (design
// doc §3/§3a) - the current implementation is a mock transport that logs the
// payload it would send instead of opening a TLS connection. The payload
// shape matches the design doc's §7 proposal exactly, so the only thing left
// to change once the real contract is known is mpm_send_reading()'s body.

#include <Arduino.h>
#include <core/data_model.h>

#ifndef MPM_MAPPING_ID_LEN
#define MPM_MAPPING_ID_LEN 24   // fits "tenant-007" / "MPM-1007"-style identities
#endif

void setup_mpm_rest();
void loop_mpm_rest();

bool mpm_rest_enabled();
void mpm_rest_set_enabled(bool on);

// Maps a local meter index (PowerData.meterid, 0..MODBUS_NUM_METERS-1) to the
// identity MPM expects for that meter, per the design doc's §6 mapping
// fields. meter_id is the stable logical channel name (e.g. "tenant-007"),
// deliberately not the same as the local array index, so re-numbering
// meters locally can never silently re-attribute a tenant's history.
// mapping_version is incremented by the caller whenever a mapping changes
// (meter replacement, tenant turnover, phase move) so MPM can audit it.
// Unmapped meters are skipped on upload.
void mpm_rest_set_mapping(uint8_t local_meterid, const char* meter_id,
                           const char* mpm_meter_id, uint16_t mapping_version);
