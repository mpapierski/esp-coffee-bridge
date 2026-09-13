# API v2 Deployment and Acceptance Runbook

This runbook covers the separately approved deployment of the nonblocking bridge firmware and the two-hour post-deployment acceptance test. Building the artifacts does not authorize flashing a bridge or reloading Home Assistant.

## Release order

1. Release and install the Home Assistant integration that understands both API v1 synchronous responses and API v2 jobs.
2. Confirm the updated integration still operates against the existing API v1 firmware.
3. Back up the bridge and record its baseline health.
4. Only after deployment approval, install the API v2 firmware. Use the
   one-time USB procedure below if the bridge does not yet have the current
   partition layout; otherwise use OTA.
5. Run the immediate checks, controlled-load checks, and two-hour soak below.

This order keeps rollback simple: the updated Home Assistant integration can continue using API v1 if the bridge firmware must be rolled back.

## Pre-deployment verification

Run the firmware checks from the firmware repository:

```bash
pio test -e native
node --test tests/*.test.mjs
python3 tools/test_backup_staging_littlefs.py
pio run -e esp32dev
python3 tools/generate_web_ui.py --verify \
  .pio/build/esp32dev/generated/web_ui_gzip.h
```

Record the PlatformIO size report. The build is eligible for deployment only when:

- PlatformIO identifies the target as ESP32-S3-DevKitC-1-N16R8 with 16 MB
  flash;
- flash use is below 92% of the existing OTA slot;
- the OTA slot retains at least 100 KiB free;
- static RAM use is below 20%; and
- `.pio/build/esp32dev/firmware.bin` exists.

The LittleFS regression downloads hash-pinned upstream test sources into a
temporary directory and uses a simulated 8 MiB flash volume, not a live
bridge. It first grows a populated legacy 960 KiB filesystem to 8 MiB and
verifies its data, then restores a 600 KiB upload into 576 KiB of ordered
brew/counter history, checks bounded reader memory and flash writes, and
verifies old histories survive rollback after interrupted consumption. It
requires a host C/C++ compiler and network access.

Run the Home Assistant checks from its repository before publishing that compatibility release:

```bash
.venv/bin/ruff check .
.venv/bin/pytest -q
```

Preserve both the new firmware and the last known-good firmware, including the
matching ELF files, with hashes:

```bash
openssl dgst -sha256 .pio/build/esp32dev/firmware.bin
openssl dgst -sha256 .pio/build/esp32dev/firmware.elf
```

## Baseline and backup

Set the bridge address and use a temporary directory for captured evidence:

```bash
BRIDGE_URL=http://192.168.10.138
RUN_DIR=$(mktemp -d)
curl -fsS "$BRIDGE_URL/api/status" > "$RUN_DIR/status-before.json"
curl -fsS "$BRIDGE_URL/api/machines" > "$RUN_DIR/machines-before.json"
curl -fsS "$BRIDGE_URL/api/backup/export" > "$RUN_DIR/bridge-backup.ndjson"
jq '{apiVersion, bridgeId, resetReason, durableMachineWriteCount, memory, littleFsTotalBytes, littleFsUsedBytes}' \
  "$RUN_DIR/status-before.json"
```

Confirm the backup is non-empty and retain it with the previous firmware binary. Do not restore it as part of a normal upgrade.

Record the complete pre-upgrade history totals. On API v1, `historyStorage.totalBytes` is the authoritative persisted-byte baseline:

```bash
jq '.historyStorage' "$RUN_DIR/status-before.json" \
  > "$RUN_DIR/history-before.json"
```

API v2 does not compact history at boot or during backup export. Existing history files establish a preservation floor, including files larger than a newer configured limit. The LittleFS mount path also refuses to auto-format any non-erased partition. A pre-upgrade backup remains mandatory as protection against power loss or hardware failure, but the normal firmware update itself does not migrate, rewrite, or truncate history.

API v2 backup export is a read-only parser-verified snapshot capped at 7,500
KiB. It groups normalized history entries into bounded array records; the cap
is checked against the smallest valid entries, maximum escaped record
envelopes, all 32 history files, and bounded non-history records at the full
6,000 KiB writable-history limit on the 8 MiB partition. Restore is an explicit
state-replacement operation: it cancels boot-scoped jobs and removes their
result files plus derived live/recipe caches, stages the upload into independent
bounded files, and validates the complete bundle. Its peak-space preflight
includes the old histories retained for rollback. Restoration deletes fully
consumed staging files while writing normalized history; it never shifts the
remaining upload in place. If the current history plus uploaded replacement
cannot coexist safely, restore refuses before replacing history. Do not erase
history to work around a refusal without a separately verified backup and
explicit approval.

## One-time partition-table installation

The firmware targets the ESP32-S3-N16R8 memory configuration: 16 MB quad-I/O
flash and 8 MB octal-I/O PSRAM. The layout grows LittleFS from 960 KiB to 8
MiB at its existing `0x310000` offset and adds a 256 KiB core-dump partition
at `0xfc0000`, the end of flash. NVS and both OTA slots are unchanged. The
bundled LittleFS driver grows the populated filesystem on first mount without
formatting it, so installing this table does not migrate or rewrite saved
machines or history. It is still a flash-layout operation: take and verify the
backup above first. Once the filesystem has grown, do not reinstall a table
that shrinks it back to 960 KiB.

An OTA upload writes only an application slot and cannot install a new
partition table. If the baseline reports `littleFsTotalBytes != 8388608`, has
no `crashDump` object, or reports `crashDump.supported == false`, connect USB
and, only after explicit deployment approval, flash the current build with
PlatformIO:

```bash
ESP_PORT=/dev/cu.usbmodem-REPLACE_ME
pio run -e esp32dev -t upload --upload-port "$ESP_PORT"
```

That upload installs the generated partition table and application while
leaving NVS and the LittleFS start address at their existing locations. Do not
run an erase-flash command. After reboot, require
`littleFsTotalBytes == 8388608`, `crashDump.supported == true`, and the expected
saved-machine/history totals. This USB step is required only once; future
application releases can use the OTA workflow below.

## Approved OTA deployment

When the current partition layout is already installed, upload the verified
image after explicit deployment approval:

```bash
curl -f \
  -F "firmware=@.pio/build/esp32dev/firmware.bin" \
  "$BRIDGE_URL/api/ota"
```

The successful endpoint response is sent before the bridge reboots. Wait for it to return, but do not repeatedly submit the OTA request:

```bash
until curl -fsS "$BRIDGE_URL/api/status" > "$RUN_DIR/status-after.json"; do
  sleep 2
done
```

## Immediate checks

Inspect the new status document:

```bash
jq '{
  apiVersion,
  capabilities,
  bridgeId,
  bleQueue,
  bleWorker,
  bleWatchdog,
  bridgeRecovery,
  memory,
  http,
  crashDump,
  resetReason,
  durableMachineWriteCount
}' "$RUN_DIR/status-after.json"
```

The immediate pass conditions are:

- `apiVersion` is `2`, `capabilities.asyncBleJobs` and
  `capabilities.websocketEvents` are `true`,
  `capabilities.eventProtocolVersion` is `2`,
  `capabilities.durableBrewQueue` is `true`, and
  `capabilities.eventsUrl` is `/api/events`;
- `bridgeId` matches the baseline;
- `bleWorker.ready` is `true`;
- `bleQueue.capacity` is `8`, with no unexpected queued/running work after settling;
- `bleWatchdog.markerPresent` is `false` for a clean rollout;
- `bridgeRecovery.markerPresent` is `false` for a clean rollout;
- `http.ready` is `true`, its heartbeat remains fresh, and health-probe queue
  failures do not grow;
- `memory.psramAvailable` is `true` and `memory.psramSize` is nonzero, proving
  that the N16R8 octal PSRAM configuration initialized;
- `littleFsTotalBytes` is `8388608`;
- `crashDump.supported` is `true` after the one-time USB installation;
- saved machines are still present; and
- Home Assistant reconnects without configuration changes.

Open the embedded UI in a browser and confirm its persistent Bridge activity
indicator reports live updates as connected. Start one forced resource refresh;
the browser should receive `queued`, `running`, and terminal job updates over
`/api/events`, then fetch the job's same-origin `resultUrl`. The browser must not
poll `/api/status` or `/api/jobs/{id}` after the initial page load. Temporarily
interrupting the WebSocket should visibly disable BLE-backed actions and should
not resubmit an accepted mutation; after reconnect, pending watches resume.

History preservation is a deployment gate. The post-upgrade total must be greater than or equal to the recorded API v1 total, and the status must advertise the lossless policy:

```bash
BEFORE_HISTORY_BYTES=$(jq -r '.totalBytes' "$RUN_DIR/history-before.json")
AFTER_HISTORY_BYTES=$(jq -r '.historyStorage.totalBytes' "$RUN_DIR/status-after.json")
test "$AFTER_HISTORY_BYTES" -ge "$BEFORE_HISTORY_BYTES"
test "$(jq -r '.historyStorage.losslessAcrossFirmwareUpdates' "$RUN_DIR/status-after.json")" = true
```

Also inspect `historyStorage.largestBrewFileBytes`, `largestStatsFileBytes`, `statsBudgetBytes`, `statsBudgetUpperBytes`, and `writableAggregateLimitBytes`. Counter history defaults to the transaction-safe 192 KiB single-file ceiling on the current partition. If an existing file exceeds a normal writable cap it remains readable/exportable; new appends fail closed rather than deleting any older entry.

If a watchdog marker is present, capture the complete status and logs before rebooting again:

```bash
curl -fsS "$BRIDGE_URL/api/status" > "$RUN_DIR/status-watchdog.json"
curl -fsS "$BRIDGE_URL/api/logs" > "$RUN_DIR/logs-watchdog.json"
```

If `crashDump.present` is true, download it before any explicit erase. The dump
can contain credentials and other RAM contents, so keep the file private:

```bash
umask 077
curl -fsS \
  -H 'X-Bridge-Diagnostics-Confirm: download-crash-dump' \
  -D "$RUN_DIR/coredump-headers.txt" \
  "$BRIDGE_URL/api/crash-dump" \
  -o "$RUN_DIR/esp-coffee-bridge-coredump.bin"
```

Use the archived `firmware.elf` matching the SHA reported by
`crashDump.appElfSha256` (also returned as `X-Bridge-App-Elf-Sha256`). With the
ESP-IDF 4.4 core-dump tool, print the task and backtrace summary with:

```bash
espcoredump.py --chip esp32s3 info_corefile \
  -t raw \
  -c "$RUN_DIR/esp-coffee-bridge-coredump.bin" \
  .pio/build/esp32dev/firmware.elf
```

Only after preserving and inspecting the dump, erase it explicitly:

```bash
curl -fsS -X DELETE \
  -H 'X-Bridge-Diagnostics-Confirm: erase-crash-dump' \
  "$BRIDGE_URL/api/crash-dump"
```

The confirmation header and lack of permissive CORS are safeguards against
casual browser access, not LAN authentication. A power cut, brownout, or a
failure that prevents the panic handler from running can still produce no dump.

## Validate one API v2 lifecycle

Choose a saved machine serial from `/api/machines`, request a combined deep refresh, and save the accepted response:

```bash
MACHINE_SERIAL=$(jq -r '.machines[0].serial' "$RUN_DIR/machines-before.json")
curl -fsS -X POST \
  -H 'Content-Type: application/json' \
  -d '{"resources":["stats","settings"]}' \
  "$BRIDGE_URL/api/machines/$MACHINE_SERIAL/refresh" \
  > "$RUN_DIR/deep-accepted.json"
JOB_ID=$(jq -r '.job.id' "$RUN_DIR/deep-accepted.json")
```

Poll at the advertised interval until the job becomes terminal:

```bash
JOB_DEADLINE=$((SECONDS + 60))
JOB_STATE=queued
while (( SECONDS < JOB_DEADLINE )); do
  curl -fsS "$BRIDGE_URL/api/jobs/$JOB_ID" > "$RUN_DIR/deep-job.json"
  JOB_STATE=$(jq -r '.job.state' "$RUN_DIR/deep-job.json")
  case "$JOB_STATE" in
    succeeded) break ;;
    failed|cancelled) jq . "$RUN_DIR/deep-job.json"; exit 1 ;;
  esac
  POLL_MS=$(jq -r '.job.pollAfterMs // 500' "$RUN_DIR/deep-job.json")
  sleep "$(awk -v ms="$POLL_MS" 'BEGIN { printf "%.3f", ms / 1000 }')"
done
test "$JOB_STATE" = succeeded
```

Confirm the terminal response has a same-origin `resultUrl`, then read stats and settings without forcing another refresh. Both should return `200` with fresh cache metadata.

## HTTP responsiveness under BLE load

Start another stats/settings refresh and, while it is queued or running, collect at least 120 `/api/status` timings:

```bash
curl -fsS -X POST \
  -H 'Content-Type: application/json' \
  -d '{"resources":["stats","settings"]}' \
  "$BRIDGE_URL/api/machines/$MACHINE_SERIAL/refresh" \
  > "$RUN_DIR/load-job.json"

: > "$RUN_DIR/status-latencies.txt"
for sample in $(seq 1 120); do
  curl -fsS -o /dev/null -w '%{time_total}\n' "$BRIDGE_URL/api/status" \
    >> "$RUN_DIR/status-latencies.txt"
  sleep 0.1
done

python3 - "$RUN_DIR/status-latencies.txt" <<'PY'
from pathlib import Path
import math
import sys

samples = sorted(float(value) for value in Path(sys.argv[1]).read_text().split())
p95 = samples[max(0, math.ceil(len(samples) * 0.95) - 1)]
print(f"samples={len(samples)} p95={p95:.3f}s max={max(samples):.3f}s")
raise SystemExit(0 if p95 < 0.250 and max(samples) < 1.000 else 1)
PY
```

Acceptance requires `/api/status` p95 below 250 ms and every sample below one second while BLE work is active.

## Duplicate work and NVS checks

During normal Home Assistant operation:

- The bridge metadata coordinator runs every 30 seconds and does not trigger live machine reads.
- Each machine summary coordinator runs every 60 seconds.
- Each machine deep coordinator runs every 900 seconds and submits one combined stats/settings refresh.
- Brew and confirm actions refresh summary only. A setting write refreshes settings once.

Capture `/api/status` before and after at least one 15-minute period. The pass conditions are:

- `bleQueue.rejected` does not increase;
- no pair of independent stats/settings jobs appears for one deep-coordinator cycle;
- identical overlapping reads return the same job ID or increment `bleQueue.coalesced`, rather than consuming two active slots; and
- `machineSession.nimbleClientCount` stays at zero before first use or one after first use, including across repeated connection failures; session retry delay rises to at most 60 seconds instead of creating a client on every poll; and
- `durableMachineWriteCount` does not change solely because of ordinary bridge polling, presence scans, or unchanged live-resource refreshes.

## Bounded history-memory check

Warm one brew-history and one counter-history request, capture status, then repeat paginated reads. The page limit is capped at 100 regardless of file size.

```bash
curl -fsS "$BRIDGE_URL/api/machines/$MACHINE_SERIAL/history?limit=100&offset=0" > /dev/null
curl -fsS "$BRIDGE_URL/api/machines/$MACHINE_SERIAL/stats/history?limit=100&offset=0" > /dev/null
curl -fsS "$BRIDGE_URL/api/status" > "$RUN_DIR/heap-warm.json"

for sample in $(seq 1 100); do
  curl -fsS "$BRIDGE_URL/api/machines/$MACHINE_SERIAL/history?limit=100&offset=0" > /dev/null
  curl -fsS "$BRIDGE_URL/api/machines/$MACHINE_SERIAL/stats/history?limit=100&offset=0" > /dev/null
done

curl -fsS "$BRIDGE_URL/api/status" > "$RUN_DIR/heap-after.json"
jq '{memory, historyStorage}' "$RUN_DIR/heap-warm.json" "$RUN_DIR/heap-after.json"
```

The free internal heap may fluctuate, but it must settle rather than decline
with file length or request count. Investigate a persistent downward trend, a
shrinking largest internal block on every iteration, a reboot, or a watchdog
marker.

## Two-hour Home Assistant soak

Leave Home Assistant enabled for two hours with its normal 30/60/900-second schedules. Sample `/api/status` periodically and preserve Home Assistant logs. The soak passes when:

- the bridge remains reachable and Home Assistant entities continue updating;
- `bleQueue.rejected` does not increase and active work never remains saturated at eight slots;
- deep refreshes occur once per 15-minute coordinator cycle, without immediate-plus-delayed duplicates;
- no routine 30-second bridge refresh causes live BLE calls;
- `durableMachineWriteCount` is stable unless a durable machine field was actually changed;
- heap values remain bounded during repeated history reads;
- no new watchdog marker appears; and
- `/api/status` continues to satisfy the latency target under active BLE work.

Save final evidence:

```bash
curl -fsS "$BRIDGE_URL/api/status" > "$RUN_DIR/status-final.json"
curl -fsS "$BRIDGE_URL/api/logs" > "$RUN_DIR/logs-final.json"
```

## Rollback

If acceptance fails, first capture status, logs, the failed job document, and Home Assistant logs. Then OTA the preserved last known-good firmware through the same endpoint. The already-upgraded Home Assistant integration supports API v1 and should reconnect after the rollback.

If station Wi-Fi is unavailable, use the bridge fallback AP and `http://192.168.4.1/api/ota`; if OTA is unavailable, recover over USB. Restore the data backup only if machine/history data is actually missing or corrupt, because restore intentionally replaces the current saved-machine and history stores.
