import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import test from "node:test";

const html = readFileSync(new URL("../web/index.html", import.meta.url), "utf8");

test("job result waits for the WebSocket lifecycle before fetching REST result", async () => {
  const source = html.slice(
    html.indexOf("  async function resolveBridgeJob("),
    html.indexOf("  async function api("),
  );
  const calls = [];
  const scope = {
    safeSameOriginPath: (value) => value,
    awaitBridgeJob: async (job) => {
      calls.push(["wait", job.id]);
      return { ...job, state: "succeeded", resultUrl: `/api/jobs/${job.id}/result` };
    },
    fetchJson: async (path) => {
      calls.push(["fetch", path]);
      return { data: { ok: true, brewed: true }, response: { status: 200, headers: { get: () => null } } };
    },
    Date,
    Error,
  };
  vm.createContext(scope);
  vm.runInContext(source, scope);
  const result = await scope.resolveBridgeJob(
    { job: { id: "boot-1", state: "queued" } },
    "/api/jobs/boot-1",
    Date.now() + 1000,
  );
  assert.deepEqual(calls, [
    ["wait", "boot-1"],
    ["fetch", "/api/jobs/boot-1/result"],
  ]);
  assert.equal(result.brewed, true);
});

test("completed job metadata returns its result without polling job status", async () => {
  const source = html.slice(
    html.indexOf("  async function resolveBridgeJob("),
    html.indexOf("  async function api("),
  );
  const calls = [];
  const scope = {
    safeSameOriginPath: (value) => value,
    awaitBridgeJob: async (job) => job,
    fetchJson: async (path) => {
      calls.push(path);
      return { data: { ok: true }, response: { status: 200, headers: { get: () => null } } };
    },
    Date,
    Error,
  };
  vm.createContext(scope);
  vm.runInContext(source, scope);
  await scope.resolveBridgeJob(
    { job: { id: "boot-2", state: "succeeded", resultUrl: "/api/jobs/boot-2/result" } },
    "/api/jobs/boot-2",
    Date.now() + 1000,
  );
  assert.deepEqual(calls, ["/api/jobs/boot-2/result"]);
});

test("completed job result retries transient filesystem contention", async () => {
  const source = html.slice(
    html.indexOf("  async function resolveBridgeJob("),
    html.indexOf("  async function api("),
  );
  let attempts = 0;
  const scope = {
    safeSameOriginPath: (value) => value,
    awaitBridgeJob: async (job) => job,
    fetchJson: async () => {
      attempts++;
      if (attempts === 1) {
        const error = new Error("filesystem is busy");
        error.status = 503;
        throw error;
      }
      return { data: { ok: true }, response: { status: 200, headers: { get: () => null } } };
    },
    setTimeout: (callback) => callback(),
    Date,
    Error,
  };
  vm.createContext(scope);
  vm.runInContext(source, scope);
  const result = await scope.resolveBridgeJob(
    { job: { id: "boot-3", state: "succeeded", resultUrl: "/api/jobs/boot-3/result" } },
    "/api/jobs/boot-3",
    Date.now() + 1000,
  );
  assert.equal(result.ok, true);
  assert.equal(attempts, 2);
});

test("socket reconnect renews unresolved watches and BLE actions are gated", () => {
  assert.match(html, /function watchPendingJobs\(\)[\s\S]*type: "watch_job"/);
  assert.match(html, /function requireLiveAction\(action\)[\s\S]*BLE actions are temporarily disabled/);
  assert.match(html, /const BLE_UI_ACTIONS = new Set/);
  assert.match(html, /setTimeout\(checkBridgeEventLiveness, 5000\)/);
  assert.doesNotMatch(html, /setInterval\(/);
});
