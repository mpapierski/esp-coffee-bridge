import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import test from "node:test";

const html = readFileSync(new URL("../web/index.html", import.meta.url), "utf8");
const helpers = html.slice(
  html.indexOf("  function bridgeJobLabel("),
  html.indexOf("  function findMachine("),
);

function load(machines = []) {
  const scope = {
    escapeHtml: (value) => String(value),
    findMachine: (serial) => machines.find((machine) => machine.serial === serial) || null,
  };
  vm.createContext(scope);
  vm.runInContext(helpers, scope);
  return scope;
}

test("running statistics show operation, machine, age, progress and queue", () => {
  const scope = load([{ serial: "NIV123", modelName: "NICR 756", alias: "Kitchen" }]);
  const status = {
    bleWorker: {
      busy: true,
      currentKind: "machine_stats",
      currentTarget: "NIV123",
      currentJobAgeMs: 7123,
      progress: 40,
    },
    bleQueue: { running: 1, queued: 2 },
  };
  const activity = scope.bridgeActivityView(status);
  assert.equal(activity.summary, "Reading statistics · NICR 756 (Kitchen) · 7s · 40%");
  assert.equal(activity.queued, 2);
  assert.match(scope.bridgeActivityBadge(status), /Reading statistics · 2 queued/);
});

test("idle and queued-only states stay visible", () => {
  const scope = load();
  assert.equal(scope.bridgeActivityView({ bleQueue: {} }).summary, "Ready for requests");
  const waiting = scope.bridgeActivityView({ bleQueue: { queued: 1 } });
  assert.equal(waiting.summary, "Waiting for the worker");
  assert.match(scope.bridgeActivityBadge({ bleQueue: { queued: 1 } }), /1 queued/);
});

test("unknown diagnostic jobs and address targets remain readable", () => {
  const scope = load();
  const activity = scope.bridgeActivityView({
    bleWorker: {
      busy: true,
      currentKind: "protocol_app_probe",
      currentTarget: "address:c8:b4:17:d8:a3:8c",
      currentJobAgeMs: 999,
      progress: 0,
    },
  });
  assert.equal(activity.summary, "Protocol app probe · c8:b4:17:d8:a3:8c · 0s · 1%");
});

test("activity and jobs use WebSocket events without REST polling", () => {
  assert.match(html, /new WebSocket\(bridgeEventSocketUrl\(\)\)/);
  assert.match(html, /type: "watch_job", jobId/);
  assert.match(html, /event\.type === "status"/);
  assert.doesNotMatch(html, /function pollBridgeActivity/);
  assert.doesNotMatch(html, /fetchJson\(pollPath/);
  assert.equal((html.match(/fetchJson\("\/api\/status"/g) || []).length, 1);
});
