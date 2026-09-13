import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const source = readFileSync(new URL("../src/main.cpp", import.meta.url), "utf8");

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
