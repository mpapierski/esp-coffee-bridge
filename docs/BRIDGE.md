# ESP Coffee Bridge Implementation Notes

This document covers the ESP32 bridge firmware, saved-machine API, embedded web UI, and bridge-side probe coverage.

For raw reverse-engineered BLE protocol details, payload layouts, session setup, and family register mappings, see [NIVONA.md](NIVONA.md).

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
    - clamps the cap between the configured minimum and the mounted LittleFS size
    - compacts existing history files immediately if the new cap is lower than the current file size
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
    - streams an NDJSON backup bundle with saved machines, the configured brew-history budget, all persisted brew-history entries, and all persisted counter-history snapshots
    - excludes Wi-Fi credentials, protocol-session cache, and LittleFS recipe caches
  - `POST /api/backup/restore`
    - accepts a multipart upload containing a backup bundle file
    - validates the bundle before applying it, then replaces the saved-machine store, brew history, and counter history from that bundle
    - clears recipe caches and stored protocol-session cache before restore
    - clamps the restored brew-history budget to the target bridge's mounted LittleFS size
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
- Embedded machine dashboard UI: [`../include/web_ui.h`](../include/web_ui.h)
