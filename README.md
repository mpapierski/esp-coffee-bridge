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

## Home Assistant

Home Assistant integration for this bridge is available here:

- [esp-coffee-bridge-ha](https://github.com/mpapierski/esp-coffee-bridge-ha/)

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
- Coverage varies by family and page. See [docs/API.md](docs/API.md) for the HTTP API, firmware architecture, and implemented bridge support, and [docs/NIVONA.md](docs/NIVONA.md) for reverse-engineered packet and register details.

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

The firmware starts a setup AP only when no station credentials are saved:

- SSID: `esp-coffee-maker`
- password: `coffee-setup`

If no STA credentials are saved, open:

- `http://192.168.4.1/`

From the UI, save your Wi-Fi credentials. The bridge disables the setup AP, switches to station-only mode, and keeps retrying the configured network if it is temporarily unavailable. It does not expose a fallback hotspot once configured.

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

On an unconfigured bridge, uploads can also use the setup AP address:

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

## HTTP API

The authoritative HTTP API and bridge implementation reference is
[docs/API.md](docs/API.md). It covers the complete route catalog, request and
response shapes, API v2 jobs, cache behavior, WebSocket events, status
telemetry, backup and history guarantees, and device-family capability
coverage. This README intentionally does not duplicate that material.

All firmware generations use unversioned `/api/...` paths. Clients should
start with `GET /api/status` and select behavior from `apiVersion` and
`capabilities`. API v2 advertises `asyncBleJobs` and returns `202 Accepted`
for queued BLE work; clients then follow the job lifecycle described in the
reference. Immediate configuration, history, backup, logs, machine-list, and
static routes remain synchronous.

For safe upgrade and post-deployment checks, see the
[API v2 deployment runbook](docs/API_V2_DEPLOYMENT_RUNBOOK.md).

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
