import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const source = readFileSync(new URL("../src/main.cpp", import.meta.url), "utf8");

test("a blocked terminal brew persists history completion only once", () => {
  const coordinator = source.slice(
    source.indexOf("BrewCoordinatorActivity scheduleBrewQueue("),
    source.indexOf("void scheduleBackgroundBleJobs("),
  );
  assert.match(
    coordinator,
    /if \(!wasHistoryLogged && head\.historyLogged &&\s*!persistBrewQueueLocked\(error\)\)/,
  );
  assert.match(coordinator, /head\.historyLogged = false;/);
  assert.match(coordinator, /head\.historyDeduplicationRequired = true;/);
});

test("ordinary brew completion skips the recovery-only history scan", () => {
  const finalizer = source.slice(
    source.indexOf("bool finalizeBrewHistoryLocked("),
    source.indexOf("bool removeBrewDurablyLocked("),
  );
  const loader = source.slice(
    source.indexOf("void loadBrewQueue("),
    source.indexOf("void handleBrewQueueList("),
  );

  assert.match(finalizer, /if \(item\.historyDeduplicationRequired\)/);
  assert.match(finalizer, /brew_history::findNewestByStringField/);
  assert.match(loader, /item\.historyDeduplicationRequired = !item\.historyLogged;/);
});
