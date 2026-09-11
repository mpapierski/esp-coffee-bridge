import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import test from "node:test";

const html = readFileSync(new URL("../web/index.html", import.meta.url), "utf8");
const helper = html.slice(html.indexOf("  async function refreshAfterBrew("), html.indexOf("  function looksLikeCoffeeMachine("));
const loader = html.slice(html.indexOf("  async function loadMachineSummary("), html.indexOf("  async function loadMachineRecipes("));

function environment(fail = false) {
  const calls = [];
  const state = { diagnostics: {}, machineCache: { serial: { summary: { status: "ready" } } } };
  const scope = {
    state,
    getMachineCache: (serial) => state.machineCache[serial] ??= {},
    invalidateMachine: (serial) => { calls.push(["invalidate", serial]); delete state.machineCache[serial]; },
    api: async (path) => {
      calls.push(["GET", path]);
      if (fail) throw new Error("refresh timeout");
      return { status: "brewing" };
    },
    followStaleResourceJob: () => { throw new Error("Explicit refresh must resolve its job directly"); },
    setFlash: (message) => calls.push(["warning", message]),
    renderRoute: async () => calls.push(["render"]),
  };
  vm.createContext(scope);
  vm.runInContext(`${loader}\n${helper}`, scope);
  return { scope, calls, state };
}

test("post-brew summary bypasses the fresh pre-brew cache before rendering", async () => {
  const { scope, calls, state } = environment();
  const response = { ok: true, selector: 0 };
  await scope.refreshAfterBrew("serial", response);
  assert.deepEqual(calls, [["invalidate", "serial"], ["GET", "/api/machines/serial/summary?refresh=1"], ["render"]]);
  assert.equal(state.machineCache.serial.summary.status, "brewing");
  assert.equal(state.diagnostics.output, response);
});

test("failed summary refresh does not retry or turn an accepted brew into an error", async () => {
  const { scope, calls, state } = environment(true);
  await scope.refreshAfterBrew("serial", { ok: true });
  assert.equal(calls.filter(([kind]) => kind === "GET").length, 1);
  assert.match(calls.find(([kind]) => kind === "warning")[1], /Brew command sent.*Do not retry the brew/);
  assert.equal(calls.at(-1)[0], "render");
  assert.equal(state.diagnostics.output.ok, true);
});

test("standard, replay and customized brews all use the follow-up helper", () => {
  assert.equal((html.match(/await refreshAfterBrew\(serial, response\);/g) || []).length, 3);
});
