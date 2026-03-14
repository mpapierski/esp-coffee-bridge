# esp-coffee-bridge

`esp-coffee-bridge` is an ESP32 Wi-Fi/BLE bridge for coffee machines that use a proprietary Bluetooth Low Energy protocol.

The official mobile apps are constrained by the same short-range BLE link as the machine itself, so in practice they are most useful when you are already standing right next to the coffee machine. This project takes the opposite approach: put a small ESP32 next to the machine permanently, let it handle the BLE conversation locally, and expose the machine over normal Wi-Fi through a local web UI and HTTP API.

That only works because the bridge reimplements the vendor protocol from reverse-engineered traffic, APK analysis, and family-specific register mapping. The reverse-engineering notes live in [docs/NIVONA.md](docs/NIVONA.md).

Core pieces:

- Arduino framework
- pinned `NimBLE-Arduino` via PlatformIO `lib_deps`
- onboard saved-machine web UI
- JSON API for pairing, remembered machines, recipes, settings, stats, and diagnostics
- HTTP OTA upload for remote firmware updates

## Why This Exists

- The vendor app is limited by BLE range, so it still expects somebody to be physically close to the machine.
- A dedicated bridge can stay near the machine all the time and do the short-range BLE work once, reliably, in one place.
- Everything else can then happen over Wi-Fi: browser UI, automation, AI agents, and low-level protocol experiments.
- This is a direct machine integration, not UI automation around the mobile app.

The intended workflow is:

1. flash once over USB
2. place the ESP32 near the coffee machine
3. connect the ESP32 to Wi-Fi
4. use the onboard web UI or HTTP API to:
   - scan and probe nearby BLE devices
   - pair and save supported machines by alias in bridge memory
   - watch online/offline state and last-seen presence from idle scans
   - browse standard drinks and `MyCoffee` saved recipes
   - brew drinks quickly or with temporary machine-valid customizations
   - inspect beverage counters, maintenance counters, and settings
   - use diagnostics pages for raw proprietary protocol work when needed
5. build a new firmware locally
6. upload it over HTTP OTA
7. repeat

## Supported Brands And Models

- Currently supported brand: `NIVONA`
- Current model-family coverage in the bridge protocol module: `600`, `700`, `79x`, `8000`, `900`, `900 Light`, `1030`, `1040`
- Coverage varies by family and page. See [docs/BRIDGE.md](docs/BRIDGE.md) for implemented bridge support and [docs/NIVONA.md](docs/NIVONA.md) for reverse-engineered packet and register details.

## Web App Features

- `Dashboard`: lists remembered machines with alias/model/family, online or offline state, last-seen presence, and quick open or forget actions.
- `Add machine flow`: scans nearby BLE devices, highlights likely supported coffee machines, probes a device before saving it, and also supports manual offline add by BLE address, serial number, and optional model.
- `Live machine summary`: shows current status summary, process label/code, operator message label/code, progress, and whether the APK-backed `HY` host-confirm path is currently suggested.
- `Standard drinks`: lists the built-in drink selectors, supports quick brew, and opens a per-drink customization view.
- `Temporary brew customization`: refreshes current standard drink values from the machine, can warm the full standard-drink cache from the machine, and sends temporary overrides such as strength, aroma, temperature, cup mode, and amount fields without overwriting the machine's saved recipe.
- `Brew history`: stores a bounded per-machine history in LittleFS with the final applied recipe snapshot, a stable recipe fingerprint, optional source or actor metadata, UTC timestamps from either NTP or the fallback client-seeded clock, and a runtime-adjustable cap from the system page.
- `Counter history`: stores a separate bounded per-machine timeline of beverage and maintenance counters, snapshots only when live values change, and captures local machine use started from the front panel.
- `MyCoffee / saved recipes`: stores saved custom recipe snapshots in LittleFS too, exposes explicit refresh buttons, shows recipe details, and edits persisted custom recipes where the machine family supports them.
- `Statistics`: reads beverage counters, maintenance counters, and serial or firmware details.
- `Settings`: reads supported machine settings, writes updated values, and exposes factory reset actions for settings and recipe defaults.
- `Diagnostics`: manages cached session keys, raw characteristic reads and writes, encrypted frame send, app-style probes, settings probe, stats probe, bridge logs, and the last raw diagnostics response.
- `Bridge admin`: saves Wi-Fi credentials, configures bridge time mode (`ntp` by default or `no time` fallback), shows NTP diagnostics from the live bridge, adjusts the brew-history cap, downloads or restores bridge backups, uploads OTA firmware, reboots the bridge, resets the remembered-machine store, and exposes raw bridge status.

## Build

```bash
pio run
```

## First Flash Over USB

Adjust `upload_port` in `platformio.ini` if needed or pass it on the command line:

```bash
pio run -t upload
```

## Network / Access

The firmware always starts an AP for fallback access:

- SSID: `esp-coffee-maker`
- password: `coffee-setup`

If no STA credentials are saved, open:

- `http://192.168.4.1/`

From the UI, save your Wi-Fi credentials. The bridge will try to join your network and keep the AP up as fallback.

If STA connect succeeds, the bridge also advertises:

- `http://esp-coffee-bridge.local/`

## OTA Workflow

Build a new firmware:

```bash
pio run
```

Upload the resulting binary over HTTP:

```bash
curl -f \
  -F "firmware=@.pio/build/esp32dev/firmware.bin" \
  http://esp-coffee-bridge.local/api/ota
```

Or against the AP address:

```bash
curl -f \
  -F "firmware=@.pio/build/esp32dev/firmware.bin" \
  http://192.168.4.1/api/ota
```

The bridge reboots automatically after a successful OTA.

## Web UI

The root page `/` is a small single-page app that calls the JSON API below.

It implements the dashboard, add-machine, per-machine, recipe-detail, diagnostics, and bridge-maintenance flows summarized above.

## AI Agents

You can also let an AI agent drive the bridge over HTTP. A custom OpenClaw skill is available here:

- <https://gist.github.com/mpapierski/7a2e9b19ee8c11dba35a65455050cd57>

That skill teaches the agent to:

- discover remembered machines with `GET /api/machines`
- preflight machine state with `GET /api/machines/{serial}/summary`
- enumerate drinks and machine-valid override options with `GET /api/machines/{serial}/recipes` and `GET /api/machines/{serial}/recipes/{selector}`
- read beverage counters with `GET /api/machines/{serial}/stats`
- issue temporary brew commands with `POST /api/machines/{serial}/brew`

Because it uses the bridge's live `writableFields` and `options` data, the agent can stay inside model-specific limits instead of guessing bean counts, aroma codes, or temperature options. The skill also checks `/summary` first and avoids brewing when the machine is offline, busy, or reporting a non-zero operator message.

Example prompts:

- `surprise me with a coffee recipe`
- `what drinks can this machine make right now?`
- `brew the strongest valid lungo this machine supports`
- `how many coffees has this machine made in total?`
- `if the machine is ready, make me a cappuccino with hotter milk`

Temporary overrides issued through `/brew` only apply to the started drink. They do not overwrite saved `MyCoffee` slots.

## JSON API

### General

- `GET /api/status`
- `GET /api/devices`
- `GET /api/details`
- `GET /api/logs`
- `POST /api/logs/clear`
- `POST /api/reboot`

### Remembered machines

- `GET /api/machines`
- `POST /api/machines/probe`
- `POST /api/machines`
- `POST /api/machines/manual`
- `POST /api/machines/reset`
- `DELETE /api/machines/{serial}`
- `GET /api/machines/{serial}/summary`
  - `status.hostConfirmSuggested` flags app-backed workflow prompts that can be acknowledged with `POST /api/machines/{serial}/confirm`
- `GET /api/machines/{serial}/recipes`
- `GET /api/machines/{serial}/recipes/{selector}`
  - add `?refresh=1` to force a live reread from the machine; otherwise the bridge may serve the LittleFS-backed cached snapshot
  - the `recipe` object now includes `writableFields` and enumerated `options` for machine-capped fields like `strength`, `strengthBeans`, `aroma`, `temperature`, and `twoCups`
- `POST /api/machines/{serial}/recipes/refresh`
  - warms or refreshes the cached detail snapshots for all standard drinks on that machine in one live session
- `POST /api/machines/{serial}/brew`
  - accepts optional metadata fields such as `source`, `actor`, `label`, `note`, and `correlationId`
  - successful accepted brews are appended to the bounded per-machine LittleFS history log
  - if the next history entry would overflow that fixed per-machine history budget, the bridge rejects the brew before dispatch instead of evicting older history
- `GET /api/machines/{serial}/history`
  - returns the newest stored brew entries first
  - optional `?limit=20&offset=40` style query supports pagination from newest to oldest
  - each returned entry includes `entryId`, a stable oldest-first line index for patching one persisted entry later
- `POST /api/machines/{serial}/history/import`
  - accepts either a single entry object, an `{ "entry": { ... } }` body, or an `{ "entries": [ ... ] }` batch
  - recomputes `recipeFingerprint` server-side from the submitted compacted `recipe` snapshot and ignores any caller-supplied fingerprint
- `POST /api/machines/{serial}/history/clear`
- `PATCH /api/machines/{serial}/history/{entryId}`
  - updates timestamp metadata for one persisted brew-history entry
  - accepts `timeUnix` in seconds or `timeUnixMs` in milliseconds
  - optional fields: `timeIsoUtc`, `timeSource`, `timeSynced`
  - defaults `timeSource` to `patched` and auto-generates `timeIsoUtc` when it is omitted
- `DELETE /api/machines/{serial}/history/{entryId}`
  - deletes one persisted brew-history entry identified by `entryId`
  - returns the deleted entry body for confirmation
- `POST /api/history/config`
  - updates the runtime per-machine brew-history cap in bytes
  - clamps the requested value between the configured minimum and the mounted LittleFS size
  - compacts existing history files immediately if you lower the cap
- `GET /api/backup/export`
  - downloads an NDJSON bundle containing the current saved-machine store, the configured brew-history budget, all persisted brew-history entries, and all persisted counter-history snapshots
  - intentionally excludes Wi-Fi credentials, protocol-session cache, and LittleFS recipe caches
- `POST /api/backup/restore`
  - accepts a multipart upload with a backup bundle file and fully replaces the current saved-machine store, brew history, and counter history from that bundle
  - clears recipe caches and stored protocol-session cache before restoring
  - clamps the restored brew-history budget to the mounted LittleFS size on the target bridge
- `POST /api/machines/{serial}/confirm`
  - sends the APK-backed `HY` host-confirmation command for the selected machine
- `GET /api/machines/{serial}/mycoffee`
  - add `?refresh=1` to force a live reread of all saved recipe slots; otherwise the bridge may serve the LittleFS-backed saved-recipe snapshot
- `GET /api/machines/{serial}/mycoffee/{slot}`
  - add `?refresh=1` to force a live reread of one saved recipe slot; otherwise the bridge may serve the cached slot from the saved-recipe snapshot
- `POST /api/machines/{serial}/mycoffee/{slot}`
- `GET /api/machines/{serial}/stats`
- `GET /api/machines/{serial}/stats/history`
  - returns the newest stored counter snapshots first
  - optional `?limit=20&offset=40` style query supports pagination from newest to oldest
- `POST /api/machines/{serial}/stats/history/clear`
- `GET /api/machines/{serial}/settings`
- `POST /api/machines/{serial}/settings`

### Wi-Fi

- `POST /api/wifi/save`

Body:

```json
{
  "ssid": "your-wifi",
  "password": "your-password"
}
```

### Time

- `POST /api/time/config`

Body:

```json
{
  "mode": "ntp",
  "ntpServerPrimary": "pool.ntp.org",
  "ntpServerSecondary": "time.google.com",
  "ntpServerTertiary": "time.cloudflare.com"
}
```

Notes:

- `mode: "ntp"` is the default and causes the bridge to request UTC from NTP whenever Wi-Fi connects.
- `mode: "no_time"` disables NTP completely. In that mode the bridge only learns UTC from an HTTP request carrying the client time header, so each boot needs at least one HTTP request before history can store real timestamps.
- warm-reboot time restore is only taken from RTC-retained state; a cold boot after power loss no longer reuses an old persisted wall clock
- `/api/status` also reports `ntpDiagnostic*` fields so the System page can distinguish DNS failure from a missing UDP/123 reply.

### BLE lifecycle

- `POST /api/scan`
- `POST /api/connect`
- `POST /api/disconnect`
- `POST /api/pair`
- `POST /api/notifications`

`/api/connect` and `/api/pair` accept an optional address body:

```json
{
  "address": "AA:BB:CC:DD:EE:FF"
}
```

`/api/notifications` body:

```json
{
  "enabled": true,
  "mode": "notify"
}
```

`mode` may be `notify` or `indicate`.

### Diagnostics / low-level iteration

- `POST /api/protocol/hu`
- `POST /api/protocol/session`
- `POST /api/protocol/send-frame`
- `POST /api/protocol/app-probe`
- `POST /api/protocol/settings-probe`
- `POST /api/protocol/stats-probe`
- `POST /api/protocol/worker-probe`
- `POST /api/protocol/verify`
- `POST /api/protocol/raw-read`
- `POST /api/protocol/raw-write`
- `POST /api/gatt/services`
- `POST /api/gatt/read`
- `POST /api/gatt/write`

`/api/protocol/session` body:

```json
{
  "serial": "756573071020106-----",
  "sessionHex": "1234",
  "source": "manual"
}
```

Or clear it:

```json
{
  "clear": true,
  "serial": "756573071020106-----"
}
```

This stores a 2-byte proprietary protocol session key for one saved machine target. `GET /api/status` exposes only the number of cached protocol sessions; the actual session value is available from that machine’s summary/diagnostics view.

`/api/protocol/send-frame` body:

```json
{
  "address": "AA:BB:CC:DD:EE:FF",
  "command": "HV",
  "payloadHex": "",
  "encrypt": true,
  "useStoredSession": true,
  "reconnectAfterPair": true,
  "reconnectDelayMs": 750,
  "notificationMode": "notify",
  "waitMs": 3000
}
```

Optional fields:

- `pair`
- `details`
- `sessionHex`
- `rememberSession`
- `chunked`
- `interChunkDelayMs`
- `reconnectAfterPair`
- `reconnectDelayMs`
- `notificationMode`

It builds a proprietary coffee-machine frame on-device, sends it to `AD03`, captures every `AD02` notification chunk, and returns a best-effort decode attempt using the supplied or stored 2-byte session key.

The new web UI keeps the raw packet path under each machine's Diagnostics page, but these low-level endpoints remain available directly for protocol work.

`/api/protocol/app-probe` body:

```json
{
  "address": "AA:BB:CC:DD:EE:FF",
  "pair": true,
  "reconnectAfterPair": true,
  "reconnectDelayMs": 750,
  "warmupPing": true,
  "notificationMode": "notify",
  "waitMs": 3000,
  "settleMs": 500,
  "hrRegisterId": 200,
  "useStoredSession": true
}
```

It keeps one live connection open and runs an app-style sequence:

- connect / optionally pair
- optionally disconnect and reconnect after pairing to force a fresh bonded link
- enable `AD02` notifications
- fetch DIS / `AD06` details
- optional plain `Hp` warmup
- then direct `HV`, `HL`, `HX`, and `HR` requests over the same connection

Optional fields:

- `encrypt` (default `true`)
- `chunked`
- `interChunkDelayMs`
- `sessionHex`
- `rememberSession`
- `reconnectAfterPair`
- `reconnectDelayMs`
- `notificationMode`

`/api/protocol/worker-probe` body:

```json
{
  "address": "AA:BB:CC:DD:EE:FF",
  "pair": true,
  "reconnectAfterPair": true,
  "reconnectDelayMs": 750,
  "notificationMode": "notify",
  "waitMs": 2500,
  "continueOnHuFailure": false
}
```

It emulates the inherited library worker path more closely:

- connect / optionally pair
- optional disconnect/reconnect after pairing
- enable `AD02` notifications once
- run internal library-style `HU`
- on `HU` success, store the 2-byte session key for the current target machine
- then send public-frame style `HV` / `HL` / `HX` / `HR` requests using that target-scoped stored session automatically when available
- on `HU` failure, optionally disconnect immediately like the library worker

`/api/protocol/hu` body:

```json
{
  "waitMs": 5000
}
```

It sends a best-effort `HU` request and returns:

- random seed
- request packet hex
- first raw notification hex
- best-effort parse status

`/api/protocol/verify` body:

```json
{
  "address": "AA:BB:CC:DD:EE:FF",
  "pair": true,
  "reconnectAfterPair": true,
  "reconnectDelayMs": 750,
  "notificationMode": "notify",
  "waitMs": 3000
}
```

It performs a more structured live verification pass:

- connect
- optionally pair / secure the link
- optionally disconnect and reconnect after pairing to force a fresh bonded link
- enable `AD02` notifications
- read DIS / `AD06` details
- run a small packet matrix:
  - plain `Hp`
  - encrypted `HU` chunked
  - encrypted `HU` one-shot
  - plain `HU` chunked
- `Hp` followed by encrypted `HU`
- return every captured notification chunk, not just the last one

`/api/protocol/raw-read` body:

```json
{
  "charAlias": "aux1"
}
```

It reads one connected proprietary-service characteristic directly and returns:

- capability flags (`canRead`, `canWrite`, `canNotify`, `canIndicate`)
- raw hex
- printable ASCII

`/api/protocol/raw-write` body:

```json
{
  "charAlias": "tx",
  "hex": "534855E580D0367981BC45",
  "response": true,
  "waitMs": 5000
}
```

Supported `charAlias` values:

- `ctrl`
- `rx`
- `tx`
- `aux1`
- `aux2`
- `name`

### Generic GATT inspection

`/api/gatt/services` body:

```json
{
  "address": "AA:BB:CC:DD:EE:FF",
  "includeDescriptors": false
}
```

It connects if needed, refreshes the full remote service table, and dumps all services and characteristics with capability flags.

`/api/gatt/read` body:

```json
{
  "address": "AA:BB:CC:DD:EE:FF",
  "serviceUuid": "00001530-b089-11e4-ad45-0002a5d5c51b",
  "charUuid": "00001534-b089-11e4-ad45-0002a5d5c51b"
}
```

`serviceUuid` is optional when `charUuid` is globally unique.

`/api/gatt/write` body:

```json
{
  "address": "AA:BB:CC:DD:EE:FF",
  "serviceUuid": "00001530-b089-11e4-ad45-0002a5d5c51b",
  "charUuid": "00001531-b089-11e4-ad45-0002a5d5c51b",
  "hex": "0000",
  "response": true,
  "subscribe": true,
  "notificationMode": "notify",
  "waitMs": 1000
}
```

Useful for probing live-only services like `1530` without hardcoding them into the firmware.

## Suggested Iteration Loop

Once the bridge is near the coffee machine:

1. open `/` and scan from the Add coffee machine page
2. probe the likely supported advert you care about
3. save it with an alias
4. use the machine pages for recipes, saved recipes, stats, and settings
5. drop into the Diagnostics tab when low-level packet work is needed
6. inspect `GET /api/logs`
7. OTA a new build when firmware behavior needs changing

## Current Protocol Scope

This scaffold is transport-first. It already covers:

- supported proprietary service discovery
- bonding attempt
- DIS reads
- AD06 reads
- raw `AD02` capture
- raw `AD03` / other characteristic writes
- request-side `HU` packet generation

The remaining protocol uncertainty is still the exact live receive-side encoding for some encrypted responses, so the bridge intentionally exposes raw captures instead of pretending the protocol is fully closed.
