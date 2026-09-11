# ESP Coffee Bridge Implementation Notes

This document covers the ESP32 bridge firmware, saved-machine API, embedded web UI, and bridge-side probe coverage.

For raw reverse-engineered BLE protocol details, payload layouts, session setup, and family register mappings, see [NIVONA.md](NIVONA.md).

## API v2 Nonblocking Architecture

Firmware API v2 keeps the Arduino `WebServer`, but the HTTP loop no longer performs BLE work. Every scan, connection, pairing operation, GATT transaction, proprietary-protocol operation, live resource read, and machine mutation is submitted to one NimBLE-owning FreeRTOS worker on core 0 with priority 1 and a 12 KiB stack. HTTP handlers only validate and copy request data, inspect cache state, submit work, and return a response.

`GET /api/status` advertises:

```json
{
  "apiVersion": 2,
  "capabilities": {
    "asyncBleJobs": true
  }
}
```

API v2 deliberately has no blocking BLE fallback. A client that needs to work with both firmware generations must retain its API v1 response handling and add transparent API v2 job resolution. The companion Home Assistant integration does this so it can be released before the firmware upgrade.

### BLE scheduler and recovery

- The scheduler admits at most eight active jobs. It serves mutations first, then forced reads, stale refreshes, and background work; FIFO order is preserved within each priority.
- Identical reads are coalesced onto the existing job. Writes are never coalesced. Interactive work may evict queued background work; if no slot can be made available, the handler returns `503 Service Unavailable` with `Retry-After: 1`.
- A job owns copied machine identity and normalized request data. It never retains a `SavedMachine*` or HTTP-server request state.
- Adjacent work for one machine reuses a valid connection, discovered handles, notification subscription, and `HU` session. The worker disconnects after ten idle seconds. A failed transport operation clears the client, handles, and session before the next job.
- Logical deadlines are 12 seconds for summary/features, 15 seconds for stats/settings, 20 seconds for one recipe/slot, 30 seconds for mutations, and 60 seconds for bulk refreshes and diagnostics. Notification waits are capped at three seconds and at the remaining job time.
- A supervisor watches the worker heartbeat. If a running BLE call makes no progress for 45 seconds, it records the job in RTC memory and reboots instead of trying to kill the worker or disconnect NimBLE concurrently. The marker and reset reason are published in `/api/status` after boot.
- The worker owns three-second idle scans scheduled once per minute. Scan interval/window are configured at `100`/`30`, duplicate filtering is enabled, and a background scan yields at a safe boundary when interactive work arrives.
- One coalesced, low-priority statistics refresh is scheduled per remembered machine every 15 minutes. A counter-history entry is appended only after a successful sample whose values differ from the last stored sample.

Remembered-machine persistence uses schema 2. Durable identity/model fields and `savedAtMs` are persisted; presence timestamps and RSSI remain in RAM. The loader accepts the schema 1 payload shape, while equality checking prevents unchanged polls and scans from rewriting NVS. `/api/status.durableMachineWriteCount` exposes actual writes since boot.

### Cache-first resources

The live-resource cache policy is:

| Resource | TTL | Ordinary `GET` behavior |
| --- | ---: | --- |
| `summary` | 60 seconds | Return fresh cache, or return stale cache while one refresh is queued |
| `stats` | 15 minutes | Return fresh cache, or return stale cache while one refresh is queued |
| `settings` | 15 minutes | Return fresh cache, or return stale cache while one refresh is queued |
| `features` | 24 hours | Return fresh cache, or return stale cache while one refresh is queued |

The four resources use the following response rules:

- Fresh cache: `200 OK` with the existing domain payload plus a `cache` object.
- Stale cache: `200 OK` with the last good data, `cache.state = "stale"`, the most recent refresh error when applicable, and job metadata when a refresh is queued.
- Cold cache or `?refresh=1`: `202 Accepted` with `Location: /api/jobs/{id}` and `Retry-After: 1`.
- Refresh failure: retain the last good cache and publish the structured error in both the failed job and subsequent stale responses.

Standard-recipe and `MyCoffee` snapshots keep their persistent LittleFS caches. A cache hit remains synchronous; a cold or explicitly forced BLE read is a job.

Consumers can refresh several compact resources in one connection/session:

```http
POST /api/machines/{serial}/refresh
Content-Type: application/json

{"resources":["summary","stats","settings"]}
```

The request accepts one to four names from `summary`, `stats`, `settings`, and `features`. It returns one `202` job; after success, consumers read the requested cache-backed endpoints without `?refresh=1`.

### Job API

Every accepted operation uses the same envelope:

```json
{
  "ok": true,
  "pending": true,
  "job": {
    "id": "boot-nonce-counter",
    "state": "queued",
    "kind": "machine_stats",
    "target": "serial",
    "pollAfterMs": 500,
    "submittedAtMs": 1234,
    "progress": 0
  }
}
```

Poll `GET /api/jobs/{id}` until `job.state` is `succeeded`, `failed`, or `cancelled`. Running jobs also expose `startedAtMs`; terminal jobs expose `finishedAtMs`. Failures and cancellations contain `job.error.code` and `job.error.message`. Successful jobs contain a same-origin `job.resultUrl`:

- Resource jobs point back to the corresponding cache-backed resource URL.
- Mutations and diagnostic operations point to `GET /api/jobs/{id}/result`, which returns the former synchronous response body. Large diagnostic responses are spooled to temporary LittleFS files instead of being retained in RAM; their lifecycle is bounded by the retained job record.

Terminal records are boot-scoped and retained for five minutes. An expired ID, a discarded result, or any ID from before a reboot returns `404`. Consumers should honor `pollAfterMs`, reject cross-origin result URLs, and impose their own total deadline; the embedded UI and Home Assistant client use 60 seconds.

Deleting or resetting a remembered machine cancels its queued jobs and clears its live and persistent recipe caches. An in-flight job checks that it is still current before publishing, so completion cannot restore deleted cached metadata.

### Immediate and asynchronous routes

The following work stays synchronous because it does not require BLE: status, logs, machine-list reads, manual/offline machine creation, history operations, backup/restore, Wi-Fi/time/history configuration, OTA, reboot, and static assets.

BLE-backed routes return jobs, including scans and probes; connect, disconnect, pairing, and notification operations; low-level GATT/protocol diagnostics; brews and confirmations; settings and `MyCoffee` writes; recipe refreshes; and any cold or forced live-resource read.

### Status telemetry

`GET /api/status` reads a published health snapshot and does not call NimBLE or scan history files. In addition to existing bridge/time/storage fields it exposes:

- `bleQueue`: capacity, queued/running counts, and submitted, completed, failed, cancelled, rejected, coalesced, and background-eviction counters.
- `bleWorker`: readiness, busy state, current job identity and age, heartbeat age, and stack high-water mark.
- `bleWatchdog`: the retained stall marker with the affected job identity.
- `memory`: free heap, minimum free heap, and largest free 8-bit-capable block.
- `resetReason`, `durableMachineWriteCount`, and cached LittleFS/history totals.
- `http.lastDurationUs` and `http.maxDurationUs` for on-device handler timing.
- asynchronous NTP diagnostic state, including pending/running flags and diagnostic worker stack high-water mark.

### Embedded UI and storage behavior

The browser UI resolves `202` jobs through one helper, validates same-origin result URLs, honors `pollAfterMs`, and applies per-request abort timeouts within a 60-second logical deadline. Stale resources render immediately with refresh/error state. Periodic refreshes are scheduled only after the preceding request completes, so slow requests cannot accumulate overlapping polls.

Standard, customized, and replayed brews explicitly refresh summary with `?refresh=1` before rerendering. A failed follow-up read warns that the brew was already sent and never retries the mutation. The worker invalidates the previous summary after an acknowledged brew or confirmation, including an acknowledged action whose final result was incomplete.

The editable UI source is [`../web/index.html`](../web/index.html). PlatformIO deterministically gzips it into a generated build header and the root route serves it with `Content-Encoding: gzip`; the generated payload must decompress byte-for-byte to the source.

Manual NTP UDP probing runs in a separate low-priority task. `bridge_time::tick()` only schedules asynchronous SNTP/diagnostic work and coalesces its one-minute retry; DNS and UDP waits are not performed in the HTTP loop.

Brew and counter history reads use two bounded passes: count physical lines, retain at most 100 selected byte offsets, then seek and parse those entries newest-first. `entryId` remains the stable oldest-first physical-line index, and malformed or oversized selected lines increment `skippedEntries`. Responses are streamed in bounded chunks. Patch and deletion write and validate a temporary file before replacement, retaining a rollback copy until the replacement is verified. All LittleFS access shares one recursive filesystem mutex.

Firmware updates are lossless for history. Startup never compacts history and a non-empty LittleFS partition is never auto-formatted after a mount failure. The configured history limit is not divided when machines are added; the largest existing file becomes a preservation floor even when it exceeds a limit introduced by newer firmware. Once a file reaches its limit, a new brew/history append is rejected before dispatch/write and statistics sampling reports a storage error instead of removing old entries. Lowering the runtime cap below the largest persisted brew-history file returns `409` with `minimumLosslessBudgetBytes`.

History growth also observes a global writable limit that retains 192 KiB of operational headroom plus transaction workspace. Backup export is read-only: it takes an immutable, parser-verified snapshot without first changing live history. Restore remains an explicit state-replacement operation. Multipart uploads are staged directly into independent 32,640-byte files (at most 23), with a 512-byte forward reader spanning chunk and record boundaries. Validation and machine loading leave staging intact. Once a complete record is in RAM, restoration deletes only fully consumed chunk files before appending normalized entries. It never shifts or rewrites the unread upload tail. Peak-space preflight includes unread chunks, normalized output, original history retained for rollback, allocation slack, and operational/transaction reserves. Insufficient capacity is rejected before replacing history; aborted/interrupted staging is cleaned up without touching the rollback originals.

## ESP32 Bridge Saved-Machine API

Current embedded bridge UI is now organized around remembered machines rather than the old one-page debug console.

- machine store
  - `GET /api/machines`
  - `POST /api/machines/probe`
  - `POST /api/machines`
  - `POST /api/machines/manual`
  - `POST /api/machines/reset`
  - `DELETE /api/machines/{serial}`
- machine pages
  - `GET /api/machines/{serial}/summary`
    - the `status` object includes `hostConfirmSuggested` for APK-backed prompt states that can be acknowledged with `HY`
  - `GET /api/machines/{serial}/recipes`
  - `GET /api/machines/{serial}/recipes/{selector}`
    - serves the cached standard recipe snapshot from LittleFS by default when available
    - `?refresh=1` forces a live protocol session and rereads the current standard recipe item values from the machine
    - successful live reads refresh the LittleFS cache
    - current bridge UI uses this for the standard-drink "Customize" page
    - the returned `recipe` object now also carries:
      - `writableFields`: bridge-accepted override keys for `/brew`
      - `options`: enumerated option lists for machine-capped discrete fields such as `strength`, `strengthBeans`, `aroma`, `temperature`, and `twoCups`
  - `POST /api/machines/{serial}/recipes/refresh`
    - opens one live session, rereads all supported standard drink definitions, and rewrites the per-machine standard-recipe LittleFS cache in one pass
  - `POST /api/machines/{serial}/brew`
    - current implementation first reads the live standard recipe, applies request overrides, uploads a temporary recipe snapshot into the machine scratch namespace, and only then sends the standard selector-based `HE` payload
    - successful accepted brews are appended to a bounded per-machine JSONL history in LittleFS
    - history timestamps use UTC from the active bridge time mode: `ntp` requests fresh network time on every Wi-Fi connect, while `no_time` disables NTP and relies on client-seeded HTTP requests plus the restored last-known clock
    - the per-machine history budget is runtime-configurable from the system page and persisted in controller preferences
    - optional request metadata fields:
      - `source`
      - `actor`
      - `label`
      - `note`
      - `correlationId`
    - supported override fields:
      - `strength`
      - `strengthBeans`
      - `aroma`
      - `temperature`
      - `coffeeTemperature`
      - `waterTemperature`
      - `milkTemperature`
      - `milkFoamTemperature`
      - `overallTemperature`
      - `preparation`
      - `twoCups`
      - `coffeeAmountMl`
      - `waterAmountMl`
      - `milkAmountMl`
      - `milkFoamAmountMl`
      - `sizeMl` alias
    - recipe editors and writes are capability-gated by detected model
      - example: `NICR 756` is capped to `3` beans and aroma codes `dynamic`, `constant`, `intense`, `individual`
    - those overrides are temporary for the started brew; they do not overwrite persistent `MyCoffee` slots
    - if the next history entry would overflow the fixed per-machine history budget, the bridge rejects the brew before dispatch instead of compacting away older history
  - `GET /api/machines/{serial}/history`
    - returns newest brew log entries first
    - supports optional `limit` and `offset` query parameters for pagination from newest to oldest
    - each entry stores the final applied compact recipe snapshot plus a stable recipe fingerprint and UTC timestamp whenever the bridge has a usable wall clock
    - each returned entry also includes `entryId`, a stable oldest-first line index that can be used with the patch endpoint
  - `POST /api/machines/{serial}/history/import`
    - accepts a single entry object, `{ "entry": { ... } }`, or `{ "entries": [ ... ] }`
    - recomputes `recipeFingerprint` on the bridge from the imported `recipe` object instead of trusting caller input
    - stores only the compact supported recipe fields in the persisted history entry
  - `PATCH /api/machines/{serial}/history/{entryId}`
    - updates the timestamp metadata for one persisted brew-history entry without rewriting recipe fields
    - accepts `timeUnix` in seconds or `timeUnixMs` in milliseconds
    - optional fields: `timeIsoUtc`, `timeSource`, `timeSynced`
    - defaults `timeSource` to `patched` and generates `timeIsoUtc` automatically when omitted
  - `DELETE /api/machines/{serial}/history/{entryId}`
    - deletes one persisted brew-history entry by `entryId`
    - returns the deleted entry payload for confirmation
  - `POST /api/history/config`
    - updates the runtime per-machine brew-history budget in bytes
    - clamps the cap between the configured minimum and the writable LittleFS limit while retaining operational headroom
    - refuses a cap below the largest persisted file with `409`; it never compacts history implicitly
  - `POST /api/time/config`
    - persists the bridge time mode and NTP server list
    - default mode is `ntp` with `pool.ntp.org`, `time.google.com`, and `time.cloudflare.com`
    - `ntp` mode requests UTC after every Wi-Fi connect and keeps retrying until sync succeeds
    - `no_time` mode stops SNTP and accepts UTC only from client HTTP requests
    - warm-reboot time restore only uses RTC-retained state; cold boots after power loss no longer reuse the old flash-persisted wall clock
  - `GET /api/status`
    - includes a stable `bridgeId` derived from the ESP32 eFuse MAC and an integer `apiVersion`
    - includes `ntpDiagnosticCode`, `ntpDiagnosticMessage`, `ntpDiagnosticServer`, `ntpDiagnosticAddress`, and `ntpDiagnosticRoundTripMs`
    - the system page uses those fields to tell DNS failure apart from a missing UDP/123 reply
  - `GET /api/backup/export`
    - preflights and streams an immutable NDJSON backup snapshot with saved machines, the configured history budgets, and all valid persisted brew/counter-history entries
    - batches history entries into bounded parser-verified arrays, skips malformed/torn physical lines, and refuses to start an oversized response; bridge-generated bundles are capped at 720 KiB, enough for the complete writable history set plus bundle metadata on the current partition
    - never compacts or otherwise mutates live history before export
    - excludes Wi-Fi credentials, protocol-session cache, and LittleFS recipe caches
  - `POST /api/backup/restore`
    - accepts a multipart upload containing a backup bundle file
    - quiesces and invalidates boot-scoped jobs, purges derived job/resource/recipe files, and validates the complete bundle before applying it
    - transactionally replaces the saved-machine store, brew history, and counter history, with boot recovery for an interrupted swap; independently staged upload files are deleted as they are consumed, without rewriting the unread bundle tail
    - preserves the bundle's configured limits, validates each history file, and reserves LittleFS allocation-block slack plus rollback/headroom space before changing durable state
  - `POST /api/machines/{serial}/history/clear`
  - `GET /api/machines/{serial}/stats/history`
    - returns the newest stored counter snapshots first
    - snapshots are appended only when live values change, so the log reflects machine activity over time instead of repeating identical polls
  - `POST /api/machines/{serial}/stats/history/clear`
  - `POST /api/machines/{serial}/confirm`
    - sends the APK-backed `HY` host-confirmation command with the current machine-scoped live session
    - intended for machine-driven prompts during a workflow, such as flush-required or move-cup prompts
    - the web UI surfaces this as a contextual `Confirm ...` action when `summary.status.hostConfirmSuggested` is true
  - standard recipe cache
    - per-machine, per-selector JSON snapshots are stored in LittleFS
    - cache files are cleared when a saved machine is forgotten or when the saved-machine store is reset
  - `GET /api/machines/{serial}/mycoffee`
    - serves the cached saved-recipe snapshot from LittleFS by default when available
    - `?refresh=1` forces a live reread of all saved recipe slots and refreshes the cache
    - the bridge now stores full saved-recipe details in this cache, not just slot names
  - `GET /api/machines/{serial}/mycoffee/{slot}`
    - serves the cached slot from the saved-recipe snapshot by default when available
    - `?refresh=1` forces a live reread of that slot and updates the saved-recipe cache entry
  - `POST /api/machines/{serial}/mycoffee/{slot}`
    - after a successful write, the bridge updates the cached saved-recipe snapshot for that slot
  - `GET /api/machines/{serial}/stats`
  - `GET /api/machines/{serial}/features`
    - opens a live saved-machine session, performs internal `HU`, then issues encrypted `HI`
    - returns the raw `10`-byte feature payload plus an APK-derived named-flag list
    - current APK-backed semantic coverage is limited to byte `0`, mask `0x01` = `ImageTransfer`
    - all remaining non-zero bits are surfaced as raw unknowns so the web UI can expose them without overclaiming meaning
    - live observation on March 13, 2026: a `NICR 756` (`EF_1.00R4__386`) stayed silent on `HI` even though `HU` and `HX` succeeded, so this endpoint can legitimately return a timeout on models that do not answer `HI`
  - `GET /api/machines/{serial}/settings`
    - each `values.<key>` item includes an `options` array of `{ "code", "label" }` pairs from the active family descriptor table
  - `POST /api/machines/{serial}/settings`
- diagnostics page
  - the new dashboard scopes diagnostics and protocol-session cache to the saved machine, but it still uses the existing low-level routes:
    - `/api/protocol/send-frame`
    - `/api/protocol/app-probe`
    - `/api/protocol/settings-probe`
    - `/api/protocol/stats-probe`
    - `/api/protocol/raw-read`
    - `/api/protocol/raw-write`
    - `/api/protocol/session`
    - `/api/logs`

All currently supported proprietary coffee-machine framing, model-family detection, recipe tables, settings/stat descriptors, and MyCoffee layout decoding are centralized in the protocol module:

- [`../include/nivona.h`](../include/nivona.h)
- [`../src/nivona.cpp`](../src/nivona.cpp)

Manual saved-machine add:

- `POST /api/machines/manual`
  - does not connect or probe BLE at all
  - intended for machines that are currently offline
  - required fields:
    - `address`
    - `addressType` = `public` / `random` (or `0` / `1`)
    - `serial`
  - optional fields:
    - `alias`
    - `model`
  - bridge behavior:
    - validates BLE MAC formatting
    - derives the supported family/model from the supplied `serial` and optional `model`
    - rejects the request if the supplied data does not map to a supported family
  - related summary behavior:
    - `GET /api/machines/{serial}/summary` now returns `ok: true` even if the bridge cannot connect live
    - in that case the response uses saved metadata and reports status summary `offline` or `unavailable` with the connection error text
    - on live `HX` reads the response now includes both numeric codes and APK-backed labels for `process` and `message`
    - when the app-backed prompt paths are detected, the response also marks `hostConfirmSuggested = true`
    - example on the `756` / family `700` path: `process=8`, `processLabel=ready`, `message=0`, `messageLabel=none`
    - unknown raw message codes remain unlabeled; for example, `message=42` has been observed live after cancel, but the APK does not map it beyond the generic fallback error text

## ESP32 Bridge Probe Coverage By Device Class

Current firmware behavior:

- saved-machine dashboard
  - periodic idle BLE scans keep the remembered machine list fresh
  - remembered machines expose:
    - alias
    - decoded model name from the serial prefix
    - serial
    - family key
    - online/offline state
    - RSSI when seen in the current scan window
- `/api/protocol/settings-probe` and `GET /api/machines/{serial}/settings`
  - pairs if requested
  - enables `AD02` notifications
  - performs encrypted internal `HU`
  - reuses the returned 2-byte session key for all following `HR` reads
  - decodes the low 16-bit setting code to the human-readable UI label
- `/api/protocol/stats-probe` and `GET /api/machines/{serial}/stats`
  - now also performs `HU` first
  - now selects family-specific metric tables from `nivona.cpp`
  - groups values into `beverages`, `maintenance`, and serial/details sections for the web app
- `GET /api/machines/{serial}/features`
  - performs encrypted internal `HU` first
  - reads `HI`
  - exposes the raw `10`-byte payload, APK-known flags, and unknown non-zero bits for the web app's machine-features page
  - some machines may still return no `HI` notification at all; current live example is `NICR 756` on March 13, 2026, where the bridge observed a clean bonded/encrypted session but timed out waiting for any `HI` response

### Family 600

- `settings-probe`
  - `water_hardness`: enum, example values `soft`, `medium`, `hard`, `very hard`
  - `temperature`: enum, example values `normal`, `high`, `max`, `individual`
  - `off_rinse`: boolean enum, example values `off`, `on`
  - `auto_off`: duration enum, example values `10 min`, `4 h`, `off`
  - `profile`: settings enum, example values `dynamic`, `constant`, `intense`, `individual`
- `stats-probe`
  - beverage counts:
    - `espresso`, `coffee`, `cappuccino`, `frothy_milk`, `hot_water`, `my_coffee`
    - unit: count
  - maintenance:
    - `filter_dependency`
    - unit: flag / small numeric status

### Family 700 / 79x

- `settings-probe`
  - `water_hardness`: enum, live-verified example on model `756` = `soft`
  - `temperature`: enum, live-verified example on model `756` = `individual`
  - `off_rinse`: boolean enum, live-verified example on model `756` = `on`
  - `auto_off`: duration enum, live-verified example on model `756` = `4 h`
  - `profile`: settings enum, live-verified example on model `756` = `intense`
  - `79x` models omit `off_rinse`
- `stats-probe`
  - beverage counts:
    - `700`: `espresso`, `cream`, `lungo`, `americano`, `cappuccino`, `latte_macchiato`, `milk`, `hot_water`, `my_coffee`
    - `79x`: `espresso`, `coffee`, `americano`, `cappuccino`, `latte_macchiato`, `milk`, `hot_water`, `my_coffee`
    - unit: count
  - maintenance:
    - `700` live-verified on model `756`:
    - `clean_brewing_unit`, `clean_frother`, `rinse_cycles`, `filter_changes`, `descaling`, `beverages_via_app`
    - `descale_percent`, `descale_warning`, `brew_unit_clean_percent`, `brew_unit_clean_warning`
    - `frother_clean_percent`, `frother_clean_warning`, `filter_percent`, `filter_warning`, `filter_dependency`
    - units: count, percent, or flag depending on field
    - live example values on model `756`:
    - `clean_brewing_unit=31`, `clean_frother=1`, `descaling=3`
    - `descale_percent=59`, `brew_unit_clean_percent=13`, `frother_clean_percent=13`
    - `filter_dependency=0`
    - `79x` maintenance beyond `filter_dependency` is not live-verified in the bridge yet

### Family 8000

- `settings-probe`
  - `water_hardness`: enum, example values `soft`, `medium`, `hard`, `very hard`
  - `off_rinse`: boolean enum, example values `off`, `on`
  - `auto_off`: duration enum, example values `10 min` through `16 h`
  - `coffee_temperature`: boolean enum, example values `off`, `on`
- `stats-probe`
  - beverage / maintenance counters via `HR`
  - units:
    - beverages, clean cycles, and descales are counts
    - percent registers are percent
    - warning / dependency registers are small numeric status values
  - verified examples from prior live bridge traces:
    - `total_beverages = 3333`
    - `espresso = 839`
    - `descale_percent` style registers use percent units

### Family 1030

- `settings-probe`
  - `water_hardness`: enum, example values `soft`, `medium`, `hard`, `very hard`
  - `off_rinse`: boolean enum, example values `off`, `on`
  - `auto_off`: duration enum, example values `10 min`, `4 h`, `off`
  - `profile`: settings enum, example values `dynamic`, `constant`, `intense`, `individual`
  - `coffee_temperature`: enum, example values `normal`, `high`, `max`, `individual`
  - `water_temperature`: enum, example values `normal`, `high`, `max`, `individual`
  - `milk_temperature`: enum, example values `high`, `max`, `individual`
- `stats-probe`
  - beverage counts:
    - `espresso`, `coffee`, `americano`, `cappuccino`, `caffe_latte`, `latte_macchiato`, `warm_milk`, `hot_milk`, `milk_foam`, `hot_water`
    - unit: count
  - maintenance:
    - `filter_dependency`
    - unit: flag / small numeric status

### Family 1040

- `settings-probe`
  - all `1030` bridge fields
  - `profile`: enum also includes `quick`
  - `milk_temperature`: enum also includes `normal` and `hot`
  - `milk_foam_temperature`: enum, example values `warm`, `max`, `individual`
  - `power_on_rinse`: boolean enum, example values `off`, `on`
  - `power_on_frother_time`: duration enum, example values `10 min`, `20 min`, `30 min`, `40 min`
- `stats-probe`
  - beverage counts:
    - `espresso`, `coffee`, `americano`, `cappuccino`, `caffe_latte`, `latte_macchiato`, `warm_milk`, `milk_foam`, `hot_water`
    - unit: count
  - maintenance:
    - `filter_dependency`
    - unit: flag / small numeric status

### Family 900 / 900 Light

- app register mapping is now documented above
- settings support remains pending in the bridge
- stats support:
  - beverage counts:
    - `espresso`, `coffee`, `americano`, `cappuccino`, `caffe_latte`, `latte_macchiato`, `milk`, `hot_water`, `my_coffee`
    - unit: count
  - maintenance:
    - `filter_dependency`
    - unit: flag / small numeric status
- saved recipes:
  - slot count `9`
  - name transport `HA` / `HB`
  - fluid writes multiply displayed `ml` by `10`

## Bridge Open Items

- Extend bridge implementation coverage to app-known but not yet exposed controls:
  - additional `900` / `900 Light` settings beyond the current bridge subset
  - additional `1030` / `1040` settings such as clock, auto-on, touch lock, and cup-heater flags
- Keep bridge-facing capability docs aligned with the actual firmware implementation when new families or fields become supported

## Workspace Artifacts

- ESP32 bridge firmware and embedded web app: [`../src/main.cpp`](../src/main.cpp)
- Current proprietary coffee-machine protocol helper module used by the bridge: [`../src/nivona.cpp`](../src/nivona.cpp)
- BLE job scheduler: [`../src/bridge_jobs.cpp`](../src/bridge_jobs.cpp)
- Embedded machine dashboard source: [`../web/index.html`](../web/index.html)
- Deterministic web-asset generator: [`../tools/generate_web_ui.py`](../tools/generate_web_ui.py)
- API v2 deployment and acceptance runbook: [API_V2_DEPLOYMENT_RUNBOOK.md](API_V2_DEPLOYMENT_RUNBOOK.md)
