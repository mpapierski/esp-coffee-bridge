# esp-coffee-bridge

ESP32 PlatformIO bridge for coffee machines that use proprietary Bluetooth Low Energy communication:

- Arduino framework
- pinned `NimBLE-Arduino` via PlatformIO `lib_deps`
- onboard saved-machine web UI
- JSON API for pairing, remembered machines, recipes, settings, stats, and diagnostics
- HTTP OTA upload for remote firmware updates

The intended workflow is:

1. flash once over USB
2. place the ESP32 near the coffee machine
3. connect the ESP32 to Wi-Fi
4. use the onboard web UI or HTTP API to:
   - scan and pair new machines
   - save machines by alias in controller memory
   - watch online/offline state and RSSI from idle scans
   - open per-machine recipes, stats, settings, and diagnostics pages
   - send raw proprietary protocol packets from the diagnostics page when needed
5. build a new firmware locally
6. upload it over HTTP OTA
7. repeat

## Build

```bash
pio run
```

## First Flash Over USB

Adjust `upload_port` in `platformio.ini` if needed or pass it on the command line:

```bash
pio run -t upload
```

## Supported Brands And Models

- Currently supported brand: `NIVONA`
- Current model-family coverage in the bridge protocol module: `600`, `700`, `79x`, `8000`, `900`, `900 Light`, `1030`, `1040`
- Coverage varies by family and page. See [docs/BRIDGE.md](docs/BRIDGE.md) for implemented bridge support and [docs/NIVONA.md](docs/NIVONA.md) for reverse-engineered packet and register details.

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

It supports:

- dashboard of remembered machines
- add-machine pairing flow with scan-time supported-machine emphasis
- per-machine pages for standard recipes, saved recipes, statistics, settings, and diagnostics
- bridge Wi-Fi, logs, and OTA maintenance

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
- `GET /api/machines/{serial}/recipes`
- `GET /api/machines/{serial}/recipes/{selector}`
  - add `?refresh=1` to force a live reread from the machine; otherwise the bridge may serve the LittleFS-backed cached snapshot
- `POST /api/machines/{serial}/brew`
- `GET /api/machines/{serial}/mycoffee`
- `GET /api/machines/{serial}/mycoffee/{slot}`
- `POST /api/machines/{serial}/mycoffee/{slot}`
- `GET /api/machines/{serial}/stats`
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
