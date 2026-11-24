# Street-EMS / OPENAMI / MPM Manager integration - design draft v0.1

Status: draft for the Monday design review Glenn asked for. Not yet implemented.
Scope: firmware (device) side only. Dashboard/frontend is a separate track, see
"Frontend / dashboard" below.

## 1. What's actually being asked for

From Glenn's notes and the REIC call with Jude:

- Street-EMS (this firmware) should keep publishing to OPENAMI over MQTT exactly
  as it does today - unchanged, always-on, no dependency on anything below.
- Add a second, independently-toggleable path that pushes the same basic metering
  (the per-tenant `PowerData` the firmware already collects) into **MPM Manager**,
  EnAccess Foundation's open-source utility-management backend
  (`github.com/EnAccess/micropowermanager`), over REST.
- Longer term: show that both a GroundBolt/ThunderCloud-managed feeder (EarthSpark's
  software for the legacy stranded SparkMeter hardware REIC already has ~400-500k
  units of) and a Street-EMS-managed feeder can land in the *same* MPM Manager
  backend, so REIC can migrate/expand onto a common system village by village.

Resolves a name that was unclear from the REIC call transcript: "Ground Bold" /
"ground bulbs" = **GroundBolt**, EarthSpark's base-station deployment of their
`thundercloud` web app (`github.com/EarthSpark/thundercloud`, dev workspace at
`github.com/EarthSpark/groundbolt-dev`).

## 2. Confirmed vs. unconfirmed (checked against the real repos, not assumed)

This matters because the generic architecture ChatGPT produced (attached) assumes
a "MPM REST meter-reading interface" is just sitting there waiting to be called.
Having now pulled the actual MPM and GroundBolt repos, that's not confirmed:

**MPM Manager** (PHP 8.5 / Laravel 12 / Vue.js 2.6, MIT-ish open source, docs at
`micropowermanager.io`):
- It does expose a REST API (`micropowermanager.io/installation/api.html`), auth
  via JWT (user or Agent-App login) or an API key from the settings dashboard.
- Every existing meter-vendor integration documented (SparkMeter, Angaza, Steama,
  Stron, Gomelong) is **pull-based**: MPM syncs readings *from* the vendor's own
  cloud API on a schedule. None of them show a device *pushing* a reading in.
- No documented endpoint, path, or payload shape for "device pushes a reading into
  MPM" was found. The OpenAPI reference page at `micropowermanager.io/openapi/`
  renders client-side JS; the raw spec isn't published at a guessable URL
  (`/openapi/openapi.json` and `.yaml` both 404). The repo's `collections/`
  folder is a set of Bruno (`.bru`) API request files, not Postman - folders
  cover Agent App, Agent Web, Devices, Appliances, Reports, Solar Home Systems,
  etc. The `Devices/` folder has `Get All Devices`, `Get Devices by Serial
  Number`, `Get Meter Detail` - all reads, nothing named like a push-in. The
  `.bru` file bodies (exact paths) haven't been pulled yet.
- Customers are tied to devices by a "device number"
  (`micropowermanager.io/usage-guide/customers.html`), and a device is added
  "through... the customer registration app... or the website" - i.e. an
  admin/agent provisioning flow, not a documented device self-registration API.
- A plugin system exists (`micropowermanager.io/development/plugins`) that lets a
  plugin register its own routes - this is plausibly how a push-ingestion endpoint
  would be added if one doesn't exist yet, but that's a plugin someone would write,
  not something already there.
- **Confirmed live in the demo (`demo.micropowermanager.io`, open access, no
  login wall - "Demo Company" seeded tenant):** a meter's detail page has a
  **Manufacturer Control** block with two buttons, "Verify Device Mapping" and
  "Generate Token", plus a "Device Mapping" status field ("Not supported" /
  "Not checked" / presumably "Supported" for a working driver) and the Meter
  Types list tags each meter type "Online" or "Offline" by connectivity. This
  confirms the real shape of MPM's remote-control concept:
  - **Generate Token** = standard prepaid STS token generation, works for any
    meter (online or offline), already seen in the Meter Transaction history
    (`pesapal_transaction`, `agent_transaction`, `cash_transaction`, …). Not
    related to live alarms or control.
  - **Device Mapping** = a live link to an "Online" meter, gated **per
    manufacturer driver** - "Demo Meter Manufacturer" shows "Not supported"
    because no real driver backs it in the demo. This is the same mechanism the
    SparkMeter/Angaza/Steama integrations use. **It is not a generic endpoint a
    new device can push into - it's a driver slot.** To get Street-EMS meters
    to "Device Mapping: supported," Street-EMS needs its own manufacturer
    driver inside MPM, built the same way SparkMeter's already is.
  - No alarm/fault indicator was found anywhere in the meter detail page, the
    Tickets module (confirmed to be manually-logged support issues, e.g. "No
    Power/Power Went OFF" as a customer-reported ticket subject, not an
    automatic device alarm), or the Meters list.
  - **This reframes the real blocking question**: not "what's MPM's push
    endpoint" but "what PHP interface does an existing manufacturer driver
    (e.g. SparkMeter's) implement inside the MPM source?" That interface *is*
    the real integration contract - finding it in the repo is more useful than
    anything in the public docs.

**Found it, and it changes the picture again.** Pulled the actual source
(`src/backend/app/Plugins/<Manufacturer>/...`, Laravel). Per-manufacturer
plugins (`SparkMeter`, `SteamaMeter`, `StronMeter`, `GomeLongMeter`,
`AngazaSHS`, `DemoMeterManufacturer`, …) implement one confirmed interface:

```php
// src/backend/app/Lib/ITokenIssuer.php
interface ITokenIssuer {
    public function capabilities(): array;            // list<VendingCapability>
    public function chargeDevice(TransactionDataContainer $t): array;
    public function unlockDevice(TransactionDataContainer $t): array;  // throws if unsupported
    public function clearDevice(Device $device): ?array;              // throws if unsupported
}
// src/backend/app/Enums/VendingCapability.php
enum VendingCapability: string {
    case CreditToken = 'credit_token';   // add credit / issue a token
    case UnlockToken = 'unlock_token';   // release from PAYG permanently
    case ResetToken  = 'reset_token';    // zero out remaining credit
}
```

**`ITokenIssuer` is the whole manufacturer-plugin contract, and it is a
token-vending interface, not a metering or control interface.** Confirmed from
the actual `SparkMeterApi` and `DemoMeterManufacturerApi` implementations:
- No inbound method anywhere - core always calls *out* to the plugin
  (`chargeDevice`/`unlockDevice`/`clearDevice`). Nothing receives a reading or a
  webhook callback from the meter side.
- No remote switch/relay command. The closest things are `unlockDevice` (PAYG
  unlock, SparkMeter throws "not supported" on this) and `clearDevice` (zero
  credit) - both are *billing* actions, not a live on/off trip.
- "Device Mapping: Not supported" in the demo UI traces to `capabilities()`
  only returning `[CreditToken]` for `DemoMeterManufacturerApi` - it's a
  vending-capability flag, confirming §2's UI read: this whole subsystem is
  about how a customer's purchased credit reaches the meter, not about alarms
  or telemetry.
- **Still genuinely unconfirmed**: where actual kWh consumption readings enter
  MPM at all (the number behind "Sold Energy: 1826.8 kWh" on the dashboard).
  Not in `ITokenIssuer`. Likely in `Plugins/SparkMeter/{Http,Listeners,Jobs}` or
  a scheduled command pulling the vendor's API - not yet pulled from source.

**Sharper question for the Enaccess 1:1, replacing the vaguer one in §6:** is
there a meter-*reading* ingestion interface analogous to `ITokenIssuer` but for
consumption data - or does every existing integration work by MPM polling each
vendor's own cloud on a schedule, with no equivalent slot for a reading to be
pushed in from a device that doesn't have its own cloud? If the latter, a
Street-EMS integration may need either (a) a small cloud-side relay that looks
like a "vendor API" MPM can poll, or (b) a genuinely new plugin interface
Enaccess would need to add - two very different asks to bring Monday.

**GroundBolt / ThunderCloud** (EarthSpark, Apache-2.0):
- Explicit disclaimer in the README: **"Developer preview - not for production
  use... must not be deployed to production or used for real customer metering or
  billing."** Worth saying out loud in the Monday review - whatever REIC's stranded
  meters run on today, it isn't this new open-sourced codebase yet.
- Its meter integration is the **Meter Driver Spec v1.4.0**
  (`github.com/EarthSpark/meter-driver-spec`, `docs/spec/index.md`): a pluggable
  driver *service* that ThunderCloud/GroundBolt calls as a client, over HTTP with
  a long-lived SSE stream at `GET /v1/events` for readings/status (`POST
  /v1/nodes/register`, `DELETE /v1/nodes/{node_id}`, `GET /v1/status`, `GET
  /v1/healthz`, …). The spec explicitly frames this as a driver managing a *set*
  of nodes behind "any internal gateway, scheduling model, RF technology...",
  with required events like `heartbeat_statistics` carrying
  `total_registered_nodes`. It never documents a single-node device serving this
  contract itself - an ESP32 playing driver would be implementing an HTTP+SSE
  server matching this spec for its own one meter, which the spec doesn't rule
  out but was never written with in mind.
- No mention anywhere in the GroundBolt/ThunderCloud docs of MicroPowerManager,
  MPM, or EnAccess. The "GroundBolt feeders and Street-EMS feeders both land in one
  MPM backend" idea is a real integration gap today, not an existing solved path.
  Jude said this "may already be in a plan of record at Enaccess" - worth asking
  directly at the Enaccess 1:1 Glenn already proposed for later this week, rather
  than assuming it from our side.

**Bottom line for Monday:** before anyone writes the MPM-facing HTTP code, someone
needs the real MPM ingestion contract - either from Enaccess directly, or by
pulling the repo's `collections/` folder and the "Meters" usage-guide page we
haven't read yet. That's a blocking dependency, not a detail to fill in later.

## 3. Firmware-side design (the part we control regardless of #2)

This repo already has the exact pattern Glenn's ask needs - a build-time flag plus
a runtime-gated, rate-limited sub-loop, same shape as every other subsystem in
`src/core/main.cpp` (`ENABLE_WIFI`, `ENABLE_MQTT`, `ENABLE_MODBUS_MASTER`, …) and
`platformio.ini`. New code should follow it exactly rather than introduce a new
style:

```
include/comms/mpm_rest_client.h
src/comms/mpm_rest_client.cpp
```

mirroring `comms/mqtt_client.h` / `.cpp`.

**Build-time flag** (`platformio.ini`, next to the other `ENABLE_*` lines):
```
;-DMPM_REST_ENABLE          ; MPM Manager REST metering side-channel (off by default)
```

**Runtime flag** (`config.h` or a runtime settings struct, independent of the
build flag so it can be toggled without reflashing):
```cpp
extern bool mpm_enabled;   // default false; can be flipped at runtime
```

**Data source - no new polling.** `include/core/data_model.h` already has exactly
the "shared metering snapshot" ChatGPT's doc calls for:
```cpp
extern PowerData readings[];   // PowerData already carries meterid, phase,
                                // voltage, current, active/reactive/apparent
                                // power, energy totals, timestamp
```
`MODBUS_NUM_METERS` meters are already polled once by `modbus_master.cpp`
regardless of who reads the result. MPM's worker reads the same `readings[]`
array OPENAMI already publishes from - no duplicate Modbus polling, satisfying
requirement #3 in the attached doc for free, because that's just how the array
already works.

**Hook into `loop()`** (`src/core/main.cpp`), same shape as the existing MQTT
poll/publish split (lines ~265-301):
```cpp
#ifdef MPM_REST_ENABLE
    if (mpm_enabled && millis() - lastMpmMillis > (unsigned long)MpmUpload_rate) {
        lastMpmMillis = millis();
        loop_mpm_rest();   // non-blocking state machine - see §4
    }
#endif
```

**Per-meter mapping.** `PowerData.meterid` (Modbus node / CT channel index) needs
a small lookup table to the MPM meter identity MPM expects (likely a numeric MPM
meter ID once we have the real provisioning flow - see open question in §2). A
`uint8_t meterid -> mpm_meter_id` map, loaded from a config block, is enough; no
need to invent anything fancier until MPM's actual identity scheme is confirmed.

## 4. The one real architectural decision for Monday

`src/core/main.cpp`'s `loop()` is a single-threaded, `millis()`-gated cooperative
loop - there is **no FreeRTOS task anywhere in this codebase today** (confirmed by
grep). That's fine for MQTT (PubSubClient's non-blocking loop) but HTTPS/TLS calls
via Arduino's `HTTPClient` **block** for hundreds of ms to seconds, especially over
a flaky link. Requirement #6 in the attached doc - "MPM outages, TLS failures and
full queues cannot interfere with local metering, OPENAMI publishing, contactor
control or phase-transfer logic" - cannot be satisfied by just calling an HTTPS
POST inline in `loop()` the way `loop_mqtt()` does today. A stalled MPM upload
would stall the leakage alarm sensing and the contactor logic right alongside it.

Two real options, worth deciding together Monday rather than each engineer
guessing independently when the work gets split in two:

1. **Dedicated FreeRTOS task** (`xTaskCreatePinnedToCore`, pinned to the spare
   core), with a small queue to hand it `PowerData` snapshots to upload. Real
   isolation, matches requirement #6 properly. First FreeRTOS task in this
   codebase - a bigger step than it sounds, worth flagging explicitly rather than
   quietly introducing it.
2. **Non-blocking state machine in `loop()`**, same style as the rest of the file,
   using `WiFiClientSecure` + manual chunked writes instead of blocking
   `HTTPClient`, with a hard per-step time budget and a watchdog-friendly bailout.
   Cheaper to reason about, consistent with how everything else here works, but
   isolation is "good enough" rather than guaranteed - a wedged TLS handshake
   could still eat a loop iteration's worth of time budget.

Recommendation to bring to Monday: start with option 2 for the demo / first cut
(lower risk, fits the existing code), and treat option 1 as the real answer if/when
MPM uploads need to run on a schedule independent of the main loop's cadence
(e.g. the "every 5 minutes regardless of OPENAMI's own rate" requirement). Don't
build the FreeRTOS task until it's needed.

## 5. Frontend / dashboard - confirmed, not assumed: MPM is billing, not alarms

This connects back to what Jude asked for on the REIC call: a dashboard with
alarm indicators and command buttons, not just MQTT Explorer. Now confirmed live
in the demo (`demo.micropowermanager.io`, open access, "Demo Company" tenant),
not just from marketing screenshots: Dashboards (revenue/cluster analytics),
Customers, Transactions, **Tickets** (confirmed to be manually-logged
support issues - "No Power/Power Went OFF" is a customer-reported ticket
subject, not an automatic alarm), Meters (a device registry with "Verify Device
Mapping" / "Generate Token" - both billing/vending actions per §2's source-level
finding, not live telemetry or switching), Solar Home Systems, Targets,
Messages, Maintenance. **No alarm indicator or live remote on/off control
exists anywhere in the live app.** MPM is the right *system of record* for
customer/billing data, not the live operations dashboard Jude described -
confirmed, not just suspected. That dashboard is still the open piece you
volunteered to design, and it's a real gap MPM doesn't fill.

## 6. Open questions to bring to Monday / the Enaccess 1:1

1. **Sharpened by §2's source-level finding**: `ITokenIssuer` (the only
   manufacturer-plugin interface found) is vending-only - no inbound reading or
   control method exists in it. Where does actual kWh *consumption* data enter
   MPM for an existing integration like SparkMeter - a scheduled pull job
   inside MPM calling the vendor's cloud, or something else? Is there any
   interface at all for a *new* manufacturer to push a reading in, or would
   Street-EMS need to look like a pollable vendor API for MPM to pull from?
2. How does MPM identify/provision an individual meter - and is there a
   registration step we need to call first, or is a meter ID assigned out of band?
3. Is "GroundBolt feeders + Street-EMS feeders -> one shared MPM backend" already
   a plan of record at Enaccess, as Jude suggested? If yes, what's already decided
   about village-level migration (Glenn's 500 GroundBolt + 500 Street-EMS -> 1000
   on MPM scenario)?
4. ~~Does MPM's own Vue.js admin dashboard have a remote meter switch / live
   alarm feature anywhere?~~ **Answered - no, confirmed live (§5).** The alarm
   dashboard is a real gap, not something to reuse from MPM.
5. Split of the two code tasks Glenn proposed: suggest splitting along the
   boundary in §3/§4 - (a) the MPM REST client + queue + per-meter mapping, once
   the real ingestion contract is confirmed, and (b) the `loop()` integration +
   build/runtime flag wiring + failure isolation (whichever of the two options in
   §4 gets picked). Both can be built against a mocked MPM endpoint in parallel
   without blocking on each other or on #1-2 being answered.

## 7. Research plan - exactly what's left, where, and how

What's been checked already (this draft): MPM's public docs site, its GitHub
README, its `collections/` API-request folder (names only, not bodies), three
usage-guide pages, a guessable OpenAPI URL (404), EarthSpark's
`meter-driver-spec` repo including the actual spec document, the live demo app
(`demo.micropowermanager.io` - open access, no login needed, confirmed by
actually using it), and the real `ITokenIssuer` source interface. What's still
open, split by who can actually get the answer:

**Can be pulled from public sources (fetch/read, no login needed):**

| # | Question | Where | How |
|---|---|---|---|
| 1 | Where actual kWh *consumption* readings enter MPM for an existing integration (not `ITokenIssuer` - confirmed vending-only) | `src/backend/app/Plugins/SparkMeter/{Http,Listeners,Jobs}`, `app/Models/Device.php`, `app/Services/` in `github.com/EnAccess/micropowermanager` | Fetch raw file content from the repo tree |
| 2 | GroundBolt meter-driver-spec conformance details | `github.com/EarthSpark/meter-driver-spec` - the `.proto` file and `/conformance` suite (not yet read) | Fetch raw file content from the repo |
| 3 | Whether MPM's Angaza/Steama/Stron/GomeLong plugins implement any interface beyond `ITokenIssuer` | `src/backend/app/Plugins/{AngazaSHS,SteamaMeter,StronMeter,GomeLongMeter}/` | Fetch raw file content from the repo tree |

**Can only come from a person at Enaccess, not from any doc or repo:**

| # | Question | Who to ask |
|---|---|---|
| 4 | Is GroundBolt+Street-EMS -> shared MPM already a plan of record, as Jude said? | Enaccess, at the 1:1 Glenn already proposed |
| 5 | Is there any plugin interface for a reading to be pushed in from a device, or does every integration require Street-EMS to look like a pollable vendor cloud API? Would Enaccess accept a new plugin/interface contribution? | Same 1:1, or the EnAccess/OSEA Discord `#micropowermanager` channel |

Items 1-3 are things I can keep pulling right now if useful. Items 4-5 need a
human on the Enaccess side - no amount of searching closes those.

## 8. Sources

- EnAccess/MicroPowerManager: https://github.com/EnAccess/micropowermanager ,
  https://micropowermanager.io/ , API collection at
  https://github.com/EnAccess/micropowermanager/tree/main/collections
- MPM dashboard screenshots (sent separately): main admin dashboard, customer
  detail, Solar Home System detail, field-agent app - all from
  `micropowermanager.io/features/*.png` / `.webp`
- GroundBolt/ThunderCloud: https://github.com/EarthSpark/thundercloud ,
  https://github.com/EarthSpark/groundbolt-dev
- Meter Driver Spec v1.4.0: https://github.com/EarthSpark/meter-driver-spec ,
  spec text at `docs/spec/index.md`
