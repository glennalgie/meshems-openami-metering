Author: Glenn G. Algie | New Energy Solutions Lab LLC (NESL)

Version: 0.3 | 21 November 2026 | Status: Architecture proposal, now partially
verified against upstream source (see §3a)

Purpose: preserve OPENAMI operation while adding a separately controllable MPM
metering upload runtime to Street-EMS. This document captures design
assumptions, source investigation, implementation alternatives, proposed
schemas, risks, verification activities, and explicit prompt questions for
engineering review.

> Markdown conversion of Glenn's `Street-EMS_OPENAMI_MPM_Detailed_Design_Proposal_v0.3.docx`,
> content preserved as authored. Added: **§3a**, a research-findings section
> answering several of §17's open questions against the actual upstream source
> and live demo, pulled and read directly this session (not assumed). A
> companion research log with the full investigation trail - screenshots,
> source excerpts, GroundBolt/meter-driver-spec findings - is at
> [`Street-EMS_MPM_design_v0.1.md`](Street-EMS_MPM_design_v0.1.md) in this
> same folder.

# Introduction - What's Included

This detailed engineering proposal consolidates the proposed optional
MicroPowerManager (MPM) REST metering integration for Street-EMS. OPENAMI
remains the primary, independently operating metering, MQTT telemetry, and
energy-control runtime; MPM is an additional, separately enabled metering
destination. The proposed implementation is additive, not a replacement for
OPENAMI.

- Parallel runtime architecture: OPENAMI continues operating independently
  while the optional MPM REST service runs alongside it.
- EnAccess repository investigation: Candidate source-code locations, plugin
  architecture, and outstanding verification of the actual MPM metering
  ingestion API.
- ESP32-S3 implementation: PlatformIO build flags, runtime configuration, C++
  module structure, snapshot sharing, and isolated upload worker.
- Metering REST design: Proposed per-meter JSON payloads, tenant-to-MPM
  identity mapping, cumulative energy readings, and billing-related semantics.
- Reliability and security: Offline buffering, HTTPS, retries, deduplication,
  credentials, delivery acknowledgements, and failure recovery.
- MPM integration alternatives: Direct use of an existing REST endpoint if
  verified, or a dedicated server-side OPENAMI ingestion plugin if necessary.
- Development plan: Work packages, estimated person-hours, integration
  testing, acceptance criteria, and source-audit decision gates.
- 36 engineering prompt questions: Organized by MPM API, metering semantics,
  Street-EMS runtime, reliability, and deployment, for structured developer
  review.

Document authorship and handling: Glenn G. Algie - New Energy Solutions Lab
LLC (NESL). NESL Confidential headers apply throughout the document.

Critical unresolved technical question: Does the current MicroPowerManager
backend already support authenticated ingestion of cumulative per-meter energy
readings? **Partially resolved - see §3a.** No manufacturer-plugin interface
found in the actual source supports inbound reading ingestion; the one
confirmed interface (`ITokenIssuer`) is vending/billing-only. Whether a
different mechanism exists elsewhere in the backend (a scheduled sync job, a
core API route outside the plugin system) is still open.

Review readiness: This version is intended for a developer architecture
review, including selection of the next upstream source-code investigation
tasks, agreement on the REST contract, and prioritization of implementation
work.

# 1. Executive decision and non-negotiable requirement

OPENAMI MUST CONTINUE TO OPERATE. The proposed MPM REST service is an optional
parallel runtime consumer of the same canonical per-tenant metering snapshots.
It is not a replacement MQTT transport, not a second meter acquisition
subsystem, and not part of the protection or switching control path. It can be
included at build time and enabled or disabled at runtime without interrupting
OPENAMI.

| Existing OPENAMI (unchanged) | Additional MPM side service (new) |
|---|---|
| Primary MQTT/JSON telemetry and existing energy-management interface | Authenticated REST upload of cumulative per-meter readings |
| Existing meter acquisition, scheduling and control behavior | Mapping, upload cadence, queue, retry and delivery acknowledgements |
| Must function even if MPM is unavailable | May be stopped, misconfigured, or offline without affecting OPENAMI |

# 2. Scope, operating context, and assumptions

- Target: Street-EMS ESP32-S3 firmware using PlatformIO / Arduino C++,
  supporting 6, 9, or 12 logical tenant meters (with extensibility to other
  configurations).
- Canonical meter snapshots are already produced by Street-EMS/OPENAMI;
  adapter reuses these snapshots and must not poll metering hardware
  independently.
- MPM means EnAccess Foundation MicroPowerManager, a Laravel-based backend
  with a plugin architecture.
- The existing MPM server API contract, exact endpoint, authorization model,
  and billing semantics remain unverified; sample requests below are
  proposed, not production-compatible claims.
- A field site may have intermittent Internet; local energy policy and tenant
  control must remain cloudless and operational.
- Optional future path: ESP32-P4 or Linux site gateway can forward multiple
  Street-EMS cabinets to MPM.

# 3. Source repository discovery and audit plan

Primary upstream: https://github.com/EnAccess/micropowermanager . Older
inensus/MPManager repository is archived. Documented plugin conventions
suggest backend `src/backend/app/Plugins/{plugin-name}` and frontend
`src/frontend/src/plugins/{plugin-name}`; these are candidate extension
points, not evidence of a specific writable meter reading route.

The earlier attempt to clone the upstream repository did not complete.
Accordingly, no specific POST meter reading route, controller method, DTO,
database schema, or authentication claim is represented as confirmed. Before
code implementation, pin a commit SHA and conduct the following audit.

| Inspection target | Question to resolve | Evidence to capture |
|---|---|---|
| src/backend/routes and controllers | Does authenticated POST/PUT meter reading ingestion exist? | Exact method, path, middleware, handler |
| src/backend/app/Plugins | How do manufacturer meter adapters insert readings? | Class names, service call graph |
| Meter models / database migrations | Where are readings and counters persisted? | Tables, units, constraints, unique keys |
| collections/ and API documentation | Which requests are demonstrated? | Sample request/response and auth |
| Authentication / tenancy | Can site-scoped device tokens ingest meter readings? | Token scope, expiration, tenant isolation |
| Billing / tariff services | How are cumulative values converted into billable usage? | Counter reset and dedupe rules |

Decision gate A: if a supported core per-meter ingestion API exists, use it
directly. Decision gate B: otherwise build a small OpenAmi backend plugin
using established MPM service/model APIs. Do not invent an endpoint by
guessing from REST import/export roadmap status.

## 3a. Research findings - NESL/Claude Code source and live-demo investigation (2026-11-21)

Unlike the earlier attempt, the upstream repo was fully cloned locally
(`D:\10Power\micropowermanager`, shallow clone of `main`) and the live demo
(`demo.micropowermanager.io`, and a locally-run `docker compose` copy of the
same stack) were both actually used, not just documented. This directly closes
part of the gap §3 flags. Status against the audit table above:

| Inspection target | Finding | Status |
|---|---|---|
| src/backend/app/Plugins - manufacturer adapters | Every manufacturer plugin (`SparkMeter`, `SteamaMeter`, `StronMeter`, `GomeLongMeter`, `AngazaSHS`, `DemoMeterManufacturer`, …) implements exactly one interface: `App\Lib\ITokenIssuer` (`src/backend/app/Lib/ITokenIssuer.php`). Its four methods - `capabilities()`, `chargeDevice()`, `unlockDevice()`, `clearDevice()` - are **all outbound, billing/vending actions** (issue a prepaid token, unlock PAYG, zero credit). None receive a reading or a callback. Confirmed by reading `SparkMeterApi.php` and `DemoMeterManufacturerApi.php` directly. | **Answers Q05 directly: no.** An existing manufacturer plugin cannot be reused as an inbound-ingestion pattern - they all push credit *out* to the meter, none accept a reading *in*. |
| src/backend/routes and controllers - reading ingestion route | Not yet located. The `collections/` Bruno request tree (see below) only shows read-only `Devices/Get All Devices`, `Devices/Get Devices by Serial Number`, `Devices/Get Meter Detail` - no write/ingest request. The actual consumption-reading intake for an "Online" manufacturer (how SparkMeter's kWh numbers reach MPM at all) is still unlocated - likely a scheduled sync job pulling the vendor's own cloud, not a plugin method, consistent with every documented integration guide being described as MPM *pulling* from the vendor. | **Still open** (Q02, Q04). Next concrete step: read `Plugins/SparkMeter/{Http,Listeners,Jobs}` and `app/Models/Device.php` in the cloned repo - not yet done. |
| collections/ and API documentation | `collections/` is a set of **Bruno** (`.bru`) request files, not Postman, with folders for Agent App, Agent Web, Devices, Appliances, Customer Registration App, Export, Horizon, Import, Odyssey Data Export, People, Plugins, Reports, Solar Home System. The OpenAPI docs page renders a client-side JS shell with no readable spec; guessed raw spec URLs (`/openapi/openapi.json`, `.yaml`) both 404. | Confirms §3's own caveat that the REST import/export roadmap item is not demonstrated evidence of a writable endpoint (Q03 stays open, but narrowed: nothing in `collections/` demonstrates one either). |
| Live UI - "Device Mapping" / alarm or control surface | Logged into both the public demo and a local `docker compose` copy (full source, not just the API) with the seeded demo account (`demo_company_admin@example.com` / `123123` - found directly in `app/Utils/DemoCompany.php`, used by the seeder). Walked every top-level menu: Dashboards, Reports, Customers, Transactions, **Tickets** (confirmed to be manually-logged support issues, e.g. "No Power/Power Went OFF" as a *customer-reported ticket subject*, not an automatic alarm), Tariffs, **Meters** (a device registry; "Verify Device Mapping" / "Generate Token" buttons are the `ITokenIssuer` actions above, gated per manufacturer - "Demo Meter Manufacturer" shows "Not supported"), Solar Home Systems, Targets, Messages, Maintenance, Appliances, Agents. **No live alarm indicator and no remote on/off control exists anywhere in the product**, confirmed hands-on, not inferred from docs. | Relevant beyond this document's scope, but directly useful to the team: MPM is a billing/customer-management system of record, not the live operations dashboard Jude (REI Cameroon) separately asked for on the Nov 20 call. That dashboard is a different, still-open deliverable - see the mockup at `docs/reic_ops_dashboard_mockup/`. |
| GroundBolt / EarthSpark interop (relevant to the village-scale migration question below) | `github.com/EarthSpark/thundercloud` (deployed as GroundBolt) and `github.com/EarthSpark/meter-driver-spec` (v1.4.0) were also cloned/read. GroundBolt's own README states: **"Developer preview - not for production use... must not be deployed to production or used for real customer metering or billing."** Its meter integration (the Meter Driver Spec) is a pluggable *gateway* contract - `POST /v1/nodes/register`, `GET /v1/events` (SSE) - built for a driver managing a *fleet* of meters behind "any internal gateway, scheduling model, RF technology," never documented for a single node serving the contract itself. **No mention of MicroPowerManager, MPM, or EnAccess anywhere in the GroundBolt/ThunderCloud docs.** | Directly answers the "is GroundBolt+Street-EMS→shared MPM already a plan of record at Enaccess" question raised separately: **no evidence of it in either codebase.** That has to be confirmed by asking Enaccess directly (§17 Q31, Q35) - it is not something source-reading can resolve further. |

**Net effect on this document's Critical unresolved technical question:** the
question is now better-framed, not answered. It is confirmed that MPM's
plugin system is not the mechanism for inbound metering ingestion. It is not
yet confirmed whether a different mechanism exists elsewhere in the backend.
Decision gate A vs. B in §3 cannot yet be closed - but the search space is
narrower than when this document was first drafted.

# 4. Parallel runtime system design

```
Street-EMS meter acquisition (single source)
        |
   immutable snapshots
        |
   +----+----------------------+
   |                           |
OPENAMI publisher        MPM REST side service
Existing MQTT/JSON       [compile-time included]
Existing control path    [runtime enabled/disabled]
   |                           |
MQTT consumers            mapping -> outbox -> HTTPS
                                |
                          MPM backend REST
                                |
                     existing ingestion OR plugin
                                |
                      MPM meter/billing services
```

The side service subscribes to or copies completed meter snapshots, then
serializes them in its own worker context. It must not acquire locks while
performing DNS, TLS, HTTP, filesystem writes, or backoff sleeps. OPENAMI
publishing continues regardless of MPM state.

# 5. Build-time inclusion and runtime lifecycle

```ini
; PlatformIO illustrative flags
build_flags =
    -D OPENAMI_ENABLE=1
    -D MPM_REST_ENABLE=1
    -D MPM_MAX_METERS=12
    -D MPM_UPLOAD_INTERVAL_SEC=300
```

The compile flag adds the MPM module to the image; it does not force
transmission. A separately persisted configuration, provisioned over a
protected local management interface, controls activation and endpoint
credentials. Avoid placing tokens or private keys in build flags or firmware
logs.

```json
{
  "mpm": {
    "enabled": false,
    "base_url": "https://mpm.example.invalid",
    "upload_interval_sec": 300,
    "queue_max_records": 4096,
    "retain_queue_when_disabled": true,
    "meter_map_version": 3
  }
}
```

| State | Entry condition | Required behavior |
|---|---|---|
| NOT_COMPILED | `MPM_REST_ENABLE=0` | No MPM tasks or network operations |
| DISABLED | Compiled; `mpm.enabled=false` | OPENAMI unaffected; configurable pending queue retention |
| WAIT_CONFIG | Enabled but URL/auth/mapping incomplete | No uploads; diagnostic error |
| RUNNING | Enabled and provisioned | Schedule snapshots and upload asynchronously |
| BACKOFF | Timeout, 429 or transient server error | Retry without blocking local operations |
| AUTH_FAULT | 401/403 or invalid credentials | Pause or controlled retry; operator alert |
| QUEUE_PRESSURE | Persistent outbox near capacity | Alarm and documented retention/drop policy |

> **Reconciliation note (added):** a first-cut, mock-transport implementation
> of this lifecycle already exists in the firmware repo
> (`include/comms/mpm_rest_client.h`, `src/comms/mpm_rest_client.cpp`), built
> and build-verified against this exact flag/runtime-flag split before this
> fuller document was available. It currently implements DISABLED and a
> RUNNING state whose payload now matches §7's data contract field-for-field
> (`schema`, `gateway_id` from the firmware's existing `getDeviceID()`,
> `meter_id`, `mpm_meter_id`, `mapping_version`, `sequence`, UTC ISO8601
> `timestamp`, `phase`, `readings.{energy_import_wh,energy_export_wh,
> voltage_v,current_a,active_power_w,power_factor}`, `quality`) - but the
> transport is still mocked (logs the JSON to Serial instead of opening a TLS
> connection), and `sequence` is in-RAM only, not yet persisted across
> reboot. WAIT_CONFIG, BACKOFF, AUTH_FAULT and QUEUE_PRESSURE states are not
> yet modeled and should be added once §11's Decision gate A/B is closed, so
> the retry/backoff/outbox logic is built against a real contract instead of
> a guessed one.

# 6. Meter identity, mapping, and lifecycle

Each logical tenant meter requires an immutable gateway identity and local
meter identity, mapped explicitly to an MPM meter identifier. Phase, customer,
tariff and location may change; historical reading attribution must not
silently change with those relationships.

| Field | Example | Notes |
|---|---|---|
| gateway_id | street-ems-001 | Unique across all deployed cabinets |
| meter_id | tenant-007 | Stable logical channel; not merely array index |
| mpm_meter_id | MPM-1007 | Server-side authoritative mapping |
| phase | B | Snapshot metadata, not billing identity |
| mapping_version | 3 | Auditable mapping changes |
| sequence | 18342 | Persisted monotonic counter or equivalent dedupe identifier |
| timestamp | UTC ISO8601 | Capture time, not HTTP upload time |

Mapping provisioning must validate uniqueness and avoid accidental
one-to-many meter assignments. Define explicit commissioning, replacement,
tenant turnover, phase move, meter reset, and device replacement procedures.

# 7. Proposed metering REST payload (NOT VERIFIED MPM API)

```json
{
  "schema": "openami.mpm.meter-reading.v1",
  "gateway_id": "street-ems-001",
  "meter_id": "tenant-007",
  "mpm_meter_id": "MPM-1007",
  "mapping_version": 3,
  "sequence": 18342,
  "timestamp": "2026-11-21T14:00:00Z",
  "phase": "B",
  "readings": {
    "energy_import_wh": 1538200,
    "energy_export_wh": 0,
    "voltage_v": 231.4,
    "current_a": 4.82,
    "active_power_w": 1092.0,
    "power_factor": 0.98
  },
  "quality": "valid"
}
```

For billing, cumulative import energy in integer Wh should be the
authoritative reading, subject to upstream MPM model compatibility.
Instantaneous W, V, A and power factor are supplemental telemetry, not a
substitute for revenue-grade energy accounting. Export and reactive energy can
be added only if supported by both measurement hardware and MPM semantics.

All numeric units, precision, timestamp format, counter behavior, and billing
period conventions must be resolved against upstream implementation. The
receiving API may require a different field naming convention; an adapter
layer should isolate that difference.

# 8. Data integrity, delivery, and offline buffering

- One low-priority HTTPS worker; no per-meter HTTP task. The worker may batch
  readings only when MPM supports atomic or per-item acknowledgements.
- Snapshot acquisition timestamps remain unchanged during offline replay. The
  outbox persists enough metadata to reproduce exactly the same logical
  reading.
- Use idempotency keys derived from gateway_id + meter_id + sequence (or a
  documented deterministic equivalent). MPM must enforce uniqueness for
  retries to be safe.
- Delete an outbox record only after server-confirmed durable acceptance;
  transport success alone is insufficient if server processing is
  asynchronous.
- Retry 5xx, timeouts, and 429 with exponential backoff, jitter, and
  Retry-After support; handle 4xx validation failures as actionable
  dead-letter records.
- Bound flash writes and outbox storage; define explicit behavior when flash
  space is exhausted. Never block sampling or OPENAMI.
- Handle counter rollovers, resets, replacements, out-of-order readings,
  duplicate timestamps, and invalid negative consumption with explicit
  quality/event markers.

Illustrative sizing: 12 meters × 1 reading per 5 minutes × 24 hours = 3,456
readings/day. At a hypothetical 256 bytes per durable record, raw payload is
about 864 KiB/day, excluding journal metadata, indexes and wear-leveling. This
is a sizing example, not a measured implementation. Decide whether local
flash can support the required outage window or whether an SD card/site
gateway is needed.

# 9. Concurrency and resource isolation

Sampling, protective relay logic, phase-transfer sequencing,
neutral/leakage supervision and OPENAMI MQTT are higher priority than MPM
networking. Prefer a copied immutable snapshot in a bounded queue. Do not hold
shared mutexes while encoding JSON or waiting for network I/O. Record stack
high-water marks, heap fragmentation, TLS memory peaks, queue fill, and
watchdog margin in load testing.

Potential contention exists if OPENAMI MQTT and MPM HTTPS share
Wi-Fi/Ethernet, TLS buffers, DNS resolver, sockets, or flash. The acceptance
criterion is not zero contention but measured proof that MPM failures cannot
violate control timing or impair normal OPENAMI publishing.

> **Reconciliation note (added):** this codebase's `src/core/main.cpp`
> `loop()` is single-threaded and `millis()`-gated with **no FreeRTOS task
> anywhere in it today** (confirmed by grep). Arduino's blocking `HTTPClient`
> cannot satisfy this section's isolation requirement if called inline. Two
> real options for the MPM worker, to decide at the same review as §11's
> gate: a dedicated `xTaskCreatePinnedToCore` task (true isolation, but the
> first FreeRTOS task introduced into this codebase), or a non-blocking
> `WiFiClientSecure` state machine in the existing loop style (fits current
> conventions, isolation is good-enough rather than guaranteed). Recommend
> starting with the latter for the first prototype and only moving to a task
> if §5's BACKOFF/QUEUE_PRESSURE states need to run on a schedule independent
> of the main loop's cadence.

# 10. Security and site provisioning

- TLS with validated server certificate chain and hostname; never silently
  disable verification.
- Least-privilege credentials scoped to site, gateway, or allowed meter
  identities; confirm actual MPM token mechanism before implementation.
- Store secrets using an appropriate protected provisioned mechanism; never
  print tokens or include them in telemetry.
- Define credential rotation, clock synchronization, expired certificate
  behavior, and recovery from loss of trusted time.
- Validate MPM tenant authorization on server: device cannot submit readings
  to another site by changing mpm_meter_id.
- Rate-limit ingestion and enforce payload size, timestamp, and counter
  sanity.
- Consider local site gateway when many cabinets share an intermittent WAN or
  direct MPM exposure is not desirable.

# 11. MPM server integration alternatives

| Option | Description | Advantages | Risks |
|---|---|---|---|
| A - native endpoint | Street-EMS posts to confirmed existing MPM meter reading API | Minimal server maintenance | May not exist or may use incompatible model |
| B - OpenAmi plugin | Add authenticated ingestion route and reuse MPM meter services | Controlled schema and validation | Plugin maintenance and upgrade compatibility |
| C - local gateway | Street-EMS sends to ESP32-P4/Linux collector; collector uploads to MPM | Central buffering, credential concentration | Additional infrastructure and hop |

Preferred sequencing: A if proven by source and integration test; B if core
ingestion does not support externally pushed cumulative readings; C as
deployment-specific transport topology. Option C does not eliminate the need
to determine A versus B at the MPM server.

**Update from §3a: Option A is now less likely than when this document was
first drafted.** The only confirmed manufacturer-facing interface
(`ITokenIssuer`) is outbound/vending-only. Nothing ruling out a separate core
route was found, but nothing confirming one was found either - Option B
remains the working assumption until someone (ideally Enaccess directly)
confirms or rules it out.

# 12. Candidate MPM plugin structure (conditional)

```
src/backend/app/Plugins/OpenAmi/
    Http/Controllers/ReadingIngestController.php
    Http/Requests/ReadingIngestRequest.php
    Services/ReadingIngestService.php
    Services/MeterIdentityResolver.php
    Models/ReadingReceipt.php
    database/migrations/...
    routes/...
    tests/...
```

Illustrative names only: verify actual upstream plugin layout, route
registration, naming conventions and service APIs before creating files. The
plugin should authenticate, authorize site/meter ownership, validate reading
units and time, enforce idempotency, resolve MPM meter entities, call the
canonical MPM reading service, and return an explicit
accepted/duplicate/rejected result.

Avoid creating a separate parallel billing ledger. Any accepted readings must
be visible to existing MPM meter usage, tariff and billing calculations under
the same business rules as native integrations.

> **Reconciliation note (added):** real plugin scaffolding exists upstream -
> `micropowermanager:new-plugin` is a documented Artisan command
> (`cd src/backend && php artisan micropowermanager:new-plugin OpenAmi`), and
> real plugin folders already exist in the cloned source to use as a
> reference layout (`src/backend/app/Plugins/{SparkMeter,SteamaMeter,...}`).
> Worth scaffolding a throwaway plugin locally against the Docker dev
> environment (`D:\10Power\micropowermanager`, `docker compose up -d
> --build`, already confirmed working) before Monday, purely to see what the
> generator actually produces - cheap to try, and turns several of this
> section's "illustrative names only" caveats into confirmed ones.

# 13. Proposed ESP32-S3 module boundaries

```
src/openami/OpenAmiMeterSnapshot.h   // existing/shared snapshot contract
src/integrations/mpm/MpmAdapter.h
src/integrations/mpm/MpmAdapter.cpp
src/integrations/mpm/MpmMeterMap.h
src/integrations/mpm/MpmQueue.cpp
src/integrations/mpm/MpmRestClient.cpp
src/integrations/mpm/MpmConfig.h
src/integrations/mpm/MpmDiagnostics.h
```

The adapter should accept snapshots through a narrow API, e.g.,
`MpmAdapter::onMeterSnapshot(const MeterSnapshot&)`, and never directly
depend on metering chip drivers. The uploader owns serialization and REST
transport. Runtime enablement, mapping and scheduling are separate concerns,
making the module testable with a fake meter stream and fake HTTP server.

> **Open decision, flagged rather than resolved (added):** this module
> boundary assumes a C++ class-based split (`MpmAdapter`, `MpmQueue`,
> `MpmRestClient` as separate classes). The actual Street-EMS codebase has
> **no classes anywhere** - every subsystem (`mqtt_client`, `modbus_master`,
> the leakage model) is free functions plus static/extern state, following
> this repo's own `docs/AGENTS.md` conventions. A first-cut implementation
> already exists following that existing style instead -
> `include/comms/mpm_rest_client.h` / `src/comms/mpm_rest_client.cpp`, with
> `setup_mpm_rest()` / `loop_mpm_rest()` / `mpm_rest_set_mapping()` mirroring
> `setup_mqtt_client()` / `loop_mqtt()` - gated by `MPM_REST_ENABLE` in
> `platformio.ini` (confirmed to build clean both flag-on and flag-off). This
> is a real decision to make together, not something to resolve by guessing:
> match the proposed class split above (more separable, more testable in
> isolation, but a first for this codebase), or extend the existing
> free-function first cut (fits house style, smaller diff, less novel
> surface area to review). Recommend settling this at the same review where
> §11's Decision gate A/B gets settled, since the answer affects how much of
> this section's queue/outbox work is worth building before the real MPM
> contract is confirmed.

# 14. Diagnostics and operational visibility

| Metric / event | Purpose |
|---|---|
| mpm.enabled / mpm.state | Operational state and remote troubleshooting |
| mpm.last_success_utc | Last acknowledged server ingestion |
| mpm.queue_depth / oldest_age_sec | Offline backlog and retention risk |
| mpm.upload_success / retry / reject | Reliability and schema mismatch |
| mpm.auth_error / tls_error | Provisioning and certificate diagnostics |
| mpm.meter_unmapped_count | Missing commissioning configuration |
| mpm.counter_reset_count | Data quality and billing investigation |
| mpm.heap_low_water / task_stack_margin | Resource impact on Street-EMS |

Expose diagnostics through the existing local Street-EMS status interface
and, where appropriate, a separate OPENAMI diagnostic topic. Do not let MPM
operational telemetry modify OPENAMI metering message schemas by default.

# 15. Verification and acceptance test matrix

| Test | Expected result |
|---|---|
| MPM disabled at boot | OPENAMI baseline unchanged; zero MPM network requests |
| Runtime OFF -> ON -> OFF | No reboot; no metering interruption; deterministic queue policy |
| 6 / 9 / 12 mapped meters | All identities correct; no cross-tenant readings |
| Unmapped meter | Skipped with explicit diagnostic; OPENAMI still publishes |
| MPM DNS / TLS / HTTP failure | Local controls and MQTT unaffected; bounded retry |
| 24-hour WAN outage + recovery | Timestamp-preserving replay; no lost accepted records within budget |
| Power loss during journal update | Recovery without corrupting queue or duplicate billing |
| Duplicate submission / timeout after accept | Server idempotency prevents double accounting |
| Meter reset / rollover / replacement | Explicit event and policy; no negative billable delta |
| 401/403 and 429 | Authentication fault and Retry-After handled |
| Queue saturation | Documented alarm and bounded behavior; no task blockage |
| Firmware OTA / version upgrade | Mapping and queued data migration or safe rollback |
| Worst-case TLS and MQTT concurrency | No watchdog faults; acceptable heap and timing margins |

# 16. Work packages, hours and dependencies

| Work package | Hours | Exit deliverable |
|---|---|---|
| Upstream repository audit and API proof | 6–10 | Pinned SHA, route/DTO/auth/model evidence |
| Street-EMS adapter and identity mapping | 14–22 | Optional module, runtime state, 1→12 meter mapping |
| Persistent outbox and HTTPS worker | 12–20 | Replay, retry, idempotency, fault diagnostics |
| MPM OpenAmi plugin (only if required) | 18–32 | Authenticated ingestion and tests |
| Integration tests, field-style faults, documentation | 12–20 | Acceptance report and commissioning guide |
| **TOTAL with plugin** | **62–104** | Engineering planning range |

The estimate excludes external security audit, upstream review cycle,
production commissioning, hardware procurement and extended field trials.
Reusing a verified core MPM ingestion API could remove the 18–32 hour plugin
work package, but only after proving compatibility.

> **Update from §3a:** roughly 2–3 hours of the "Upstream repository audit"
> package (6–10h) can be marked done - the repo is cloned, `ITokenIssuer` and
> two manufacturer plugins are read, the live UI is walked end to end, and
> the Bruno `collections/` tree is enumerated (though not yet read line by
> line). The remaining audit work is narrower: read the Jobs/Listeners
> behind an "Online" manufacturer plugin to find where consumption readings
> actually land, and get a straight answer from Enaccess on Q02/Q03/Q06.

# 17. Engineering prompt questions - decision register

| ID | Area | Prompt question / decision to capture | Status after §3a |
|---|---|---|---|
| Q01 | MPM upstream | Which exact commit SHA and release will be the compatibility baseline? | Open - cloned `main` HEAD as of 2026-11-21, no SHA pinned yet |
| Q02 | MPM upstream | Which controller, route, method and middleware receive meter readings today, if any? | **Narrowed, not closed** - confirmed not `ITokenIssuer`; still unlocated elsewhere |
| Q03 | MPM upstream | Is the REST import/export roadmap item relevant to per-meter time-series ingestion or only master-data exchange? | Open - nothing in `collections/` or the OpenAPI shell demonstrates either |
| Q04 | MPM upstream | Which meter model, reading table and billing service are authoritative? | Open - `Device` model exists (per `collections/Devices`), reading-ingestion path not yet traced |
| Q05 | MPM upstream | Can an existing manufacturer plugin be reused as an ingestion pattern without coupling to its proprietary device model? | **Answered: no** - every plugin implements `ITokenIssuer`, which is outbound/vending-only |
| Q06 | MPM upstream | Does plugin registration permit a new authenticated route with stable upgrade behavior? | Partially - docs confirm plugins can register routes; exact mechanics unread |
| Q07–Q36 | (all areas) | - | Unchanged from original draft; still open, see full table in the source `.docx` / raw.pdf version of this document |

Suggested review method: record Owner, Decision, Evidence, Target Date, and
Status for each Q-ID. Do not convert an unanswered question into an
implementation assumption without explicit approval.

# 18. Suggested phased implementation and go/no-go gates

1. Phase 0 - audit upstream MPM repository; pin SHA; produce endpoint and
   model evidence. Gate: choose native endpoint or plugin.
2. Phase 1 - prove one logical meter end-to-end in a local MPM staging
   instance; reconcile received cumulative energy. Gate: signed-off meter
   semantics.
3. Phase 2 - add runtime state machine, mapping, outbox and 6/9/12 meter
   support; instrument resource budgets. Gate: OPENAMI baseline unchanged.
4. Phase 3 - test WAN failure, power loss, replay, duplicate, counter reset,
   credentials, queue saturation and OTA. Gate: acceptance matrix passed.
5. Phase 4 - pilot one Street-EMS cabinet, then multi-cabinet gateway
   topology if required; review billing reconciliation before broader
   deployment.

# 19. Risks and design mitigations

| Risk | Consequence | Mitigation |
|---|---|---|
| No suitable MPM meter ingestion API | Server work needed | Audit before wire contract; plugin option |
| Double billing after retry | Incorrect customer charges | Server-enforced idempotency and cumulative counters |
| WAN outage exceeds buffer | Missing historical readings | Retention budget and gateway option |
| TLS memory spikes | OPENAMI/control degradation | Single worker, resource testing, limits |
| Mapping error | Wrong tenant billed | Explicit commissioning validation and immutable IDs |
| Clock drift | Misordered readings | Time-quality flag, trusted-time recovery |
| MPM version upgrade | API/plugin breakage | Pin SHA, compatibility tests, versioned contract |

# 20. References and verification status

- https://github.com/EnAccess/micropowermanager
- https://micropowermanager.io/development/plugins.html
- https://micropowermanager.io/development/backend.html
- https://micropowermanager.io/development/development-environment.html
- https://micropowermanager.io/roadmap.html
- https://enaccess.org/openami-open-advanced-metering-infrastructure/

Verification note: these were investigation references at the time this
document was first drafted - repository contents were not fully ingested in
that earlier attempt. **They have since been fully cloned and partially read
- see §3a.** Precise route names, payloads and model calls for the reading
*ingestion* path specifically remain to be verified against pinned upstream
source.

Additional references added in this revision:

- `D:\10Power\micropowermanager` - local clone used for §3a
- https://github.com/EarthSpark/thundercloud (GroundBolt) and
  https://github.com/EarthSpark/meter-driver-spec - see §3a's GroundBolt row
- `docs/mpm_manager_integration/Street-EMS_MPM_design_v0.1.md` - fuller
  research log behind §3a, including live-demo screenshots and the exact
  files read

# Appendix A - Design review meeting prompts

Opening prompt: "Confirm that OPENAMI remains the uninterrupted primary
metering and control service. Are we aligned that MPM is only an optional
parallel billing telemetry sink?"

API prompt: "Show the exact upstream MPM route/controller and persistence
path that accepts a cumulative per-meter kWh reading. If absent, approve a
small plugin rather than changing OPENAMI." **Can now be asked more
precisely: "We've confirmed `ITokenIssuer` isn't it - where does SparkMeter's
own kWh data actually land once MPM pulls it?"**

Reliability prompt: "For 12 tenant meters, how many days of disconnected
operation must be buffered, and what evidence proves replay cannot
double-bill?"

Deployment prompt: "For our rural first-kilometer cloudless installations,
should Street-EMS post directly to MPM, or should a site gateway consolidate
and authenticate uploads?" **Related, now answerable in part: GroundBolt
itself has no confirmed MPM interop today, and is explicitly marked
developer-preview - any "500 GroundBolt + 500 Street-EMS -> 1 shared MPM"
village-scale plan needs Enaccess's direct confirmation, not an assumption
from either codebase.**

Acceptance prompt: "Demonstrate unchanged OPENAMI telemetry and
contactor/phase-transfer operation while MPM is disabled, unreachable, slow,
or rejecting requests."
