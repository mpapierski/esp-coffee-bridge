# ESP Coffee Bridge API and Implementation Reference

This document covers the ESP32 bridge firmware, saved-machine API, embedded web UI, and bridge-side probe coverage.

For raw reverse-engineered BLE protocol details, payload layouts, session setup, and family register mappings, see [NIVONA.md](NIVONA.md).

## API v2 Nonblocking Architecture

Firmware API v2 uses ESP-IDF `esp_http_server` on core 1. Every scan, connection, pairing operation, GATT transaction, proprietary-protocol operation, live resource read, and machine mutation is submitted to one NimBLE-owning FreeRTOS worker on core 0 with priority 1 and a 12 KiB stack. HTTP handlers only validate and copy request data, inspect cache state, submit work, and return a response. This keeps BLE waits off both the HTTP server and the Arduino loop while preserving one owner for NimBLE state.

`GET /api/status` advertises:

```json
{
  "apiVersion": 2,
  "capabilities": {
    "asyncBleJobs": true,
    "websocketEvents": true,
    "eventProtocolVersion": 1,
    "eventsUrl": "/api/events",
    "crashDumpDownload": true
  }
}
```

`crashDumpDownload` is `true` only when the running partition table contains
the dedicated core-dump partition. Installing that partition table is a
one-time USB operation; an application-only OTA cannot add it.

API v2 deliberately has no blocking BLE fallback. A client that needs to work with both firmware generations must retain its API v1 response handling and add transparent API v2 job resolution. The companion Home Assistant integration does this so it can be released before the firmware upgrade.

### BLE scheduler and recovery

- The scheduler admits at most eight active jobs. It serves mutations first, then forced reads, stale refreshes, and background work; FIFO order is preserved within each priority.
- Identical reads are coalesced onto the existing job. Writes are never coalesced. Interactive work may evict queued background work; if no slot can be made available, the handler returns `503 Service Unavailable` with `Retry-After: 1`.
- A job owns copied machine identity and normalized request data. It never retains a `SavedMachine*` or HTTP-server request state.
- The first interactive operation for a saved machine implicitly connects, enables notifications, establishes `HU`, and validates the link with a plain `Hp` round trip. No session token or explicit connect call is required from HTTP clients.
- A validated machine session stays connected and receives a coalesced `Hp` liveness probe every ten seconds. A failed probe or BLE disconnect marks the machine offline and clears the client, handles, and `HU` key; the next interactive request establishes a new session. Diagnostic or background-only connections still use the ten-second idle disconnect.
- Logical deadlines are 12 seconds for summary/features, 15 seconds for stats/settings, 20 seconds for one recipe/slot, 30 seconds for mutations, and 60 seconds for bulk refreshes and diagnostics. Notification waits are capped at three seconds and at the remaining job time.
- A supervisor watches the BLE worker and an independently queued HTTP-task heartbeat. It also watches for a recent failed allocation followed by an HTTP stall, and for free internal heap below 12 KiB or a largest free internal block below 4 KiB continuously for five seconds. Normal HTTP stalls use a 45-second limit; a stall immediately following an allocation failure uses ten seconds.
- Automatic recovery records its reason, job identity, heap state, HTTP-heartbeat age, and latest failed allocation in RTC memory, then enters the ESP panic path. This is intentionally different from the administrative reboot and OTA paths: a panic lets ESP-IDF persist all task stacks to the core-dump partition before restarting. The older `bleWatchdog` marker remains available for BLE-specific stalls, while `bridgeRecovery` covers every automatic recovery reason.
- The worker owns three-second idle scans scheduled once per minute while no interactive machine session is active. Scan interval/window are configured at `100`/`30`, duplicate filtering is enabled, and a background scan yields at a safe boundary when interactive work arrives.
- One coalesced, low-priority statistics refresh is scheduled per remembered machine every 15 minutes. A counter-history entry is appended only after a successful sample whose values differ from the last stored sample.

Remembered-machine persistence uses schema 2. Durable identity/model fields and `savedAtMs` are persisted; presence timestamps and RSSI remain in RAM. The loader accepts the schema 1 payload shape, while equality checking prevents unchanged polls and scans from rewriting NVS. `/api/status.durableMachineWriteCount` exposes actual writes since boot.

### Cache-first resources

The live-resource cache policy is:

| Resource | TTL | Ordinary `GET` behavior |
| --- | ---: | --- |
| `summary` | 60 seconds | Establish a session when offline; otherwise return fresh cache or stale cache while one refresh is queued |
| `stats` | 15 minutes | Return fresh cache, or return stale cache while one refresh is queued |
| `settings` | 15 minutes | Return fresh cache, or return stale cache while one refresh is queued |
| `features` | 24 hours | Return fresh cache, or return stale cache while one refresh is queued |

The four resources use the following response rules:

- Fresh cache: `200 OK` with the existing domain payload plus a `cache` object.
- Stale cache: `200 OK` with the last good data, `cache.state = "stale"`, the most recent refresh error when applicable, and job metadata when a refresh is queued.
- Cold cache or `?refresh=1`: `202 Accepted` with `Location: /api/jobs/{id}` and `Retry-After: 1`.
- Refresh failure: retain the last good cache and publish the structured error in both the failed job and subsequent stale responses.

Standard-recipe and `MyCoffee` snapshots keep their persistent LittleFS caches. A cache hit remains synchronous; a cold or explicitly forced BLE read is a job. Full `MyCoffee` lists are assembled one bounded slot at a time directly into temporary LittleFS files, then streamed to HTTP in 1 KiB chunks; the firmware never allocates a list-sized JSON document.

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

REST clients may poll `GET /api/jobs/{id}` until `job.state` is `succeeded`, `failed`, or `cancelled`. The embedded UI instead watches the job on `/api/events` and performs no job-status polling. Running jobs also expose `startedAtMs`; terminal jobs expose `finishedAtMs`. Failures and cancellations contain `job.error.code` and `job.error.message`. Successful jobs contain a same-origin `job.resultUrl`:

- Resource jobs point back to the corresponding cache-backed resource URL.
- Mutations and diagnostic operations point to `GET /api/jobs/{id}/result`, which returns the former synchronous response body once the job is complete. This endpoint is deliberately immediate-only: before completion it returns the current pending/conflict response rather than occupying the single HTTP server task. The UI waits for the terminal WebSocket event and then fetches `/result`; when the job is already complete, it fetches the result immediately. Large diagnostic responses are spooled to temporary LittleFS files instead of being retained in RAM; their lifecycle is bounded by the retained job record.

Terminal records are boot-scoped and retained for five minutes. An expired ID, a discarded result, or any ID from before a reboot returns `404`. Consumers should honor `pollAfterMs`, reject cross-origin result URLs, and impose their own total deadline; the embedded UI and Home Assistant client use 60 seconds.

Deleting or resetting a remembered machine cancels its queued jobs and clears its live and persistent recipe caches. An in-flight job checks that it is still current before publishing, so completion cannot restore deleted cached metadata.

### Immediate and asynchronous routes

The following work stays synchronous because it does not require BLE: status, logs, machine-list reads, manual/offline machine creation, history operations, backup/restore, Wi-Fi/time/history configuration, OTA, reboot, and static assets.

BLE-backed routes return jobs, including scans and probes; connect, disconnect, pairing, and notification operations; low-level GATT/protocol diagnostics; brews and confirmations; settings and `MyCoffee` writes; recipe refreshes; and any cold or forced live-resource read.

### WebSocket event protocol

`GET /api/events` upgrades to a WebSocket when `capabilities.websocketEvents` is true. The bridge supports four concurrent event clients and up to eight watched jobs per client. Browser clients send `{"type":"watch_job","jobId":"..."}` and receive a current job snapshot immediately followed by change events until the job is terminal. `unwatch_job` releases a subscription and `ping` receives `pong`.

The first server message is `hello`, containing `eventProtocolVersion`, `apiVersion`, a boot-scoped sequence value, and the full status snapshot. Subsequent `status`, `job`, and `resync` messages use the same sequence scope. Changes are coalesced in a fixed-capacity buffer; overflow emits `resync`, after which clients re-establish outstanding watches. Status updates are limited to once per second while busy and once every five seconds while idle. Status JSON uses a bounded document allocated once at boot and a fixed response/event buffer, so concurrent BLE work and WebSocket clients do not create a large transient allocation for every status request.

The bridge accepts an absent `Origin` for non-browser tools. When browsers provide it, the value must equal the request's `http://Host` origin; a mismatch is closed immediately. WebSocket payloads expose only public job metadata and never request bodies, credentials, machine secrets, or retained diagnostic result contents. Broken or slow clients are disconnected rather than allowed to stall the event stream.

### Status telemetry

`GET /api/status` reads a published health snapshot and does not call NimBLE or scan history files. In addition to existing bridge/time/storage fields it exposes:

- `bleQueue`: capacity, queued/running counts, and submitted, completed, failed, cancelled, rejected, coalesced, and background-eviction counters.
- `bleWorker`: readiness, busy state, current job identity and age, heartbeat age, and stack high-water mark.
- `bleWatchdog`: the retained BLE-stall marker with the affected job identity.
- `bridgeRecovery`: the retained reason and resource context for the last automatic recovery.
- `memory`: free internal heap, minimum free internal heap, largest free internal
  8-bit-capable block, N16R8 PSRAM size/free/minimum/largest-block values, and
  count/details of the latest failed allocation.
- `resetReason`, `durableMachineWriteCount`, and cached LittleFS/history totals.
- `http`: readiness, handler timing, heartbeat age, task stack high-water mark, pending health-probe state, and probe queue failures.
- `crashDump`: partition availability, dump presence/integrity, byte length, crashed task/PC, and the crashing application's ELF SHA when a summary is available.
- `machineSession`: active saved-machine serial, `offline`/`connecting`/`online` state, last successful `Hp` time, and the ten-second probe interval.
- asynchronous NTP diagnostic state, including pending/running flags and diagnostic worker stack high-water mark.

### Crash-dump diagnostics

The ESP32-S3-N16R8 target uses 16 MB quad-I/O flash and 8 MB octal-I/O PSRAM.
Its partition layout reserves 256 KiB at `0xfc0000`, the end of flash, for an
ESP-IDF ELF core dump. NVS and both OTA slots are unchanged. LittleFS retains
its `0x310000` start address and grows from 960 KiB to 8 MiB; the remaining
4.6875 MiB between it and the core-dump partition is unallocated. The bundled
LittleFS driver grows an existing filesystem on first mount without formatting
it. After an abnormal recovery or another panic, the dump survives reboot and
is never erased automatically.

- `GET /api/crash-dump` streams the validated raw partition image. It requires
  `X-Bridge-Diagnostics-Confirm: download-crash-dump`.
- `DELETE /api/crash-dump` erases the retained image. It requires
  `X-Bridge-Diagnostics-Confirm: erase-crash-dump`.

These routes deliberately omit permissive CORS headers, and the non-simple
confirmation header prevents an ordinary cross-origin browser request. This is
a safety guard, not authentication against another device already on the LAN.
A dump may contain credentials, protocol material, and other RAM contents, so
store and transfer it as sensitive data. Retain the exact
`.pio/build/esp32dev/firmware.elf` whose ELF SHA matches `crashDump.appElfSha256`;
symbolication with a different build is unreliable. Power removal, a brownout,
or a hard failure before the panic handler runs may leave no dump.

### Embedded UI and storage behavior

The browser UI resolves `202` jobs through one helper, watches their terminal state over `/api/events`, validates same-origin result URLs, and applies per-request abort timeouts within a 60-second logical deadline. It never resubmits a mutation after a dropped event connection: outstanding watches are restored after reconnect. Stale resources render immediately with refresh/error state. Periodic machine-list refreshes are scheduled only after the preceding request completes, so slow requests cannot accumulate overlapping polls.

Wi-Fi uses mutually exclusive modes. With no saved station SSID, the bridge exposes the password-protected setup AP. Once credentials are saved it disables the AP and runs station-only, retrying the configured network after a bounded connection attempt instead of exposing a fallback hotspot. `/api/status` reports `wifiConfigured`, `wifiMode`, and `apActive` so the UI can distinguish setup mode from a temporary station outage.

A persistent Bridge activity strip and header badge are driven by WebSocket `status` events after the initial page load. While work is active they show a human-readable operation, machine model/alias, elapsed time, worker progress, and queued-job count. Loss of the live event channel is visible and temporarily disables BLE-backed actions; synchronous configuration, history, backup, and static routes remain usable.

Standard, customized, and replayed brews explicitly refresh summary with `?refresh=1` before rerendering. A failed follow-up read warns that the brew was already sent and never retries the mutation. The worker invalidates the previous summary after an acknowledged brew or confirmation, including an acknowledged action whose final result was incomplete.

Only one brew job per machine may be queued or running. A second `POST /api/machines/{serial}/brew` during that window returns `409 Conflict` with `code: "brew_job_active"`, a `Location` header for the existing job, and that job's metadata. The guard is released when the existing job succeeds, fails, or is cancelled. It prevents overlapping bridge brew commands; it does not represent a physical drink backlog or keep the guard until the machine finishes dispensing.

The editable UI source is [`../web/index.html`](../web/index.html). PlatformIO deterministically gzips it into a generated build header and the root route serves it with `Content-Encoding: gzip`; the generated payload must decompress byte-for-byte to the source.

Manual NTP UDP probing runs in a separate low-priority task. `bridge_time::tick()` only schedules asynchronous SNTP/diagnostic work and coalesces its one-minute retry; DNS and UDP waits are not performed in the HTTP loop.

Brew and counter history reads use two bounded passes: count physical lines, retain at most 100 selected byte offsets, then seek and parse those entries newest-first. `entryId` remains the stable oldest-first physical-line index, and malformed or oversized selected lines increment `skippedEntries`. Responses are streamed in bounded chunks. Patch and deletion write and validate a temporary file before replacement, retaining a rollback copy until the replacement is verified. All LittleFS access shares one recursive filesystem mutex. Counter history defaults to a 192 KiB per-machine ceiling (clamped to the filesystem's transaction-safe single-file limit); upgrades preserve existing files and old backups carrying the legacy 32 KiB ceiling are promoted when restored.

Firmware updates are lossless for history. Startup never compacts history and a non-empty LittleFS partition is never auto-formatted after a mount failure. The configured history limit is not divided when machines are added; the largest existing file becomes a preservation floor even when it exceeds a limit introduced by newer firmware. Once a file reaches its limit, a new brew/history append is rejected before dispatch/write and statistics sampling reports a storage error instead of removing old entries. Lowering the runtime cap below the largest persisted brew-history file returns `409` with `minimumLosslessBudgetBytes`.

History growth also observes a global writable limit that retains 192 KiB of operational headroom plus transaction workspace. Backup export is read-only: it takes an immutable, parser-verified snapshot without first changing live history. Restore remains an explicit state-replacement operation. Multipart uploads are staged directly into independent 32,640-byte files (at most 23), with a 512-byte forward reader spanning chunk and record boundaries. Validation and machine loading leave staging intact. Once a complete record is in RAM, restoration deletes only fully consumed chunk files before appending normalized entries. It never shifts or rewrites the unread upload tail. Peak-space preflight includes unread chunks, normalized output, original history retained for rollback, allocation slack, and operational/transaction reserves. Insufficient capacity is rejected before replacing history; aborted/interrupted staging is cleaned up without touching the rollback originals.

## ESP32 Bridge Saved-Machine API

Current embedded bridge UI is now organized around remembered machines rather than the old one-page debug console.

Machine list objects distinguish protocol readiness from scan presence:

- `online` and `sessionState` (`offline`, `connecting`, or `online`) describe the bridge-owned BLE/`HU` session.
- `nearby`, `lastSeenAtMs`, and `lastSeenRssi` describe the most recent idle scan and do not imply that commands can be sent.
- Opening a machine page forces a summary read when no session is online. The UI shows connection progress and offers an explicit retry after failure, but session establishment remains implicit in the underlying request.

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
  - `GET /api/crash-dump`
    - requires `X-Bridge-Diagnostics-Confirm: download-crash-dump`
    - streams the retained, validated ESP-IDF core-dump image without erasing it
  - `DELETE /api/crash-dump`
    - requires `X-Bridge-Diagnostics-Confirm: erase-crash-dump`
    - explicitly erases the retained core dump after it has been archived and investigated
  - `GET /api/backup/export`
    - preflights and streams an immutable NDJSON backup snapshot with saved machines, the configured history budgets, and all valid persisted brew/counter-history entries
    - validates each history entry independently, groups normalized entries into bounded array records, streams complete NDJSON records through a bounded output buffer, skips malformed/torn physical lines, and refuses to start an oversized response
    - bridge-generated bundles are capped at 7,500 KiB; a compile-time and native-test boundary calculation covers the smallest valid entries, maximum escaped record envelopes, all 32 history files, and non-history records at the full 6,000 KiB writable-history limit on the 8 MiB partition
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
    - stores one bounded file per slot plus a streamed aggregate list in LittleFS
    - serves the cached aggregate directly without reconstructing it in heap memory
    - `?refresh=1` forces a live reread of all saved recipe slots and atomically refreshes the aggregate cache
    - the bridge stores full saved-recipe details in this cache, not just slot names
  - `GET /api/machines/{serial}/mycoffee/{slot}`
    - streams the independent bounded slot cache by default when available
    - `?refresh=1` forces a live reread of that slot, refreshes its cache, and invalidates the aggregate list
  - `POST /api/machines/{serial}/mycoffee/{slot}`
    - after a successful write, the bridge updates that slot cache and invalidates the aggregate list
  - `GET /api/machines/{serial}/stats`
  - `GET /api/machines/{serial}/features`
    - for models that answer `HI`, opens a live saved-machine session, performs internal `HU`, then issues encrypted `HI`
    - returns the raw `10`-byte feature payload plus an APK-derived named-flag list when available
    - current APK-backed semantic coverage is limited to byte `0`, mask `0x01` = `ImageTransfer`
    - all remaining non-zero bits are surfaced as raw unknowns so the web UI can expose them without overclaiming meaning
    - live observation on March 13, 2026: a `NICR 756` (`EF_1.00R4__386`) stayed silent on `HI` even though `HU` and `HX` succeeded
    - the bridge therefore returns and caches `supported: false` for model `756` without opening BLE; this prevents every features refresh from waiting for a known timeout
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
    - `GET /api/machines/{serial}/summary` implicitly establishes and validates the machine session when it is offline
    - connection, `HU`, `Hp`, or live `HX` failures fail the asynchronous job and leave the remembered machine offline
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
    - validated protocol-session online/offline state
    - separate nearby state and RSSI from the current scan window
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
  - returns cached `supported: false` without BLE for model `756`, which is known not to answer `HI`
  - on other models, performs encrypted internal `HU`, reads `HI`, and exposes the raw `10`-byte payload, APK-known flags, and unknown non-zero bits for the web app's machine-features page
  - untested machines may still return no `HI` notification; only models confirmed silent are added to the skip list

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
- ESP-IDF HTTP/WebSocket adapter: [`../src/bridge_http_server.cpp`](../src/bridge_http_server.cpp)
- Bounded streaming multipart parser: [`../src/bridge_multipart.cpp`](../src/bridge_multipart.cpp)
- Embedded machine dashboard source: [`../web/index.html`](../web/index.html)
- Deterministic web-asset generator: [`../tools/generate_web_ui.py`](../tools/generate_web_ui.py)
- API v2 deployment and acceptance runbook: [API_V2_DEPLOYMENT_RUNBOOK.md](API_V2_DEPLOYMENT_RUNBOOK.md)
