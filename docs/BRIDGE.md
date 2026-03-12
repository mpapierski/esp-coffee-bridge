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
  - `GET /api/machines/{serial}/recipes`
  - `POST /api/machines/{serial}/brew`
  - `GET /api/machines/{serial}/mycoffee`
  - `GET /api/machines/{serial}/mycoffee/{slot}`
  - `POST /api/machines/{serial}/mycoffee/{slot}`
  - `GET /api/machines/{serial}/stats`
  - `GET /api/machines/{serial}/settings`
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

### Family 600

- `settings-probe`
  - `water_hardness`: enum, example values `soft`, `medium`, `hard`, `very hard`
  - `temperature`: enum, example values `normal`, `high`, `max`, `individual`
  - `off_rinse`: boolean enum, example values `off`, `on`
  - `auto_off`: duration enum, example values `10 min`, `4 h`, `off`
  - `profile`: aroma/profile enum, example values `dynamic`, `constant`, `intense`, `individual`
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
  - `profile`: aroma/profile enum, live-verified example on model `756` = `intense`
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
  - `profile`: aroma/profile enum, example values `dynamic`, `constant`, `intense`, `individual`
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
