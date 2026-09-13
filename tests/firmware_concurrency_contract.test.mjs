import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const source = readFileSync(new URL("../src/main.cpp", import.meta.url), "utf8");
const storageHeader = readFileSync(new URL("../include/history_storage.h", import.meta.url), "utf8");
const storageSource = readFileSync(new URL("../src/history_storage.cpp", import.meta.url), "utf8");

test("BLE worker wakeups do not share NimBLE's task notification slot", () => {
  assert.match(source, /SemaphoreHandle_t bleWorkerWake\s*= nullptr;/);
  assert.match(source, /xSemaphoreTake\(bleWorkerWake, pdMS_TO_TICKS\(250\)\)/);
  assert.doesNotMatch(source, /xTaskNotifyGive\(bleWorkerTaskHandle\)/);

  const worker = source.slice(
    source.indexOf("void bleWorkerTask(void*)"),
    source.indexOf("void workerSupervisorTask(void*)"),
  );
  assert.doesNotMatch(worker, /ulTaskNotifyTake\(/);
});

test("cached resources use one bounded filesystem read", () => {
  const handlerStart = source.lastIndexOf("void handleMachineResourceRequest(");
  const loader = source.slice(
    source.indexOf("bool loadCachedResourcePayload("),
    source.indexOf("bool sendCachedResourcePayload("),
  );
  const sender = source.slice(
    source.indexOf("bool sendCachedResourcePayload("),
    source.indexOf("void handleMachineResourceRequest("),
  );
  const handler = source.slice(
    handlerStart,
    source.indexOf("void handleMachineRefreshRequest(", handlerStart),
  );

  assert.match(loader, /history_storage::Guard filesystem\("resource_cache_read", 1000\)/);
  assert.doesNotMatch(sender, /LittleFS|history_storage::Guard/);
  assert.equal((handler.match(/loadCachedResourcePayload\(/g) || []).length, 1);
  assert.doesNotMatch(handler, /history_storage::Guard/);
});

test("known unavailable features bypass the filesystem cache", () => {
  const handlerStart = source.lastIndexOf("void handleMachineResourceRequest(");
  const handler = source.slice(
    handlerStart,
    source.indexOf("void handleMachineRefreshRequest(", handlerStart),
  );
  assert.match(
    handler,
    /if \(resource == "features" && sendKnownUnavailableMachineFeatures\(\*machine\)\) return;/,
  );
});

test("filesystem lock diagnostics identify contention and long holders", () => {
  assert.match(storageHeader, /struct LockDiagnostics/);
  assert.match(storageHeader, /bool lockTagged\(const char\* operation/);
  assert.match(storageSource, /gLockContendedAcquisitions/);
  assert.match(storageSource, /gLockTimedOutAcquisitions/);
  assert.match(storageSource, /gLockMaxWaitMs/);
  assert.match(storageSource, /gLockMaxHoldMs/);

  const status = source.slice(
    source.indexOf("void appendStatus("),
    source.indexOf("bool parseAddressTypeRequest("),
  );
  assert.match(status, /historyStorage\.createNestedObject\("lock"\)/);
  assert.match(status, /lastTimeoutBlockedBy/);
  assert.match(source, /Guard filesystem\("resource_cache_read", 1000\)/);
  assert.match(source, /Guard filesystem\("backup_read", 1000\)/);
});

test("backup export releases LittleFS between bounded reads", () => {
  const reader = source.slice(
    source.indexOf("class BufferedBackupLineReader"),
    source.indexOf("bool processBackupHistory("),
  );
  const exporter = source.slice(
    source.indexOf("void handleBackupExport("),
    source.indexOf("void handleBackupRestoreFinished("),
  );
  assert.match(reader, /Guard filesystem\("backup_read", 1000\)/);
  assert.match(reader, /history_storage::historyGeneration\(\)/);
  assert.doesNotMatch(exporter, /exportFilesystem/);
  assert.match(exporter, /BACKUP_SNAPSHOT_CHANGED_ERROR/);
});
