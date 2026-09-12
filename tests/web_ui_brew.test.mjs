import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import test from "node:test";

const html = readFileSync(new URL("../web/index.html", import.meta.url), "utf8");
const helper = html.slice(html.indexOf("  async function refreshAfterBrew("), html.indexOf("  function looksLikeCoffeeMachine("));
function environment() {
  const calls = [];
  const state = { diagnostics: {}, brewQueue: { brews: [] } };
  const scope = {
    state,
    refreshCore: async () => calls.push(["refresh"]),
    setFlash: (message, kind) => calls.push(["flash", message, kind]),
    renderRoute: async () => calls.push(["render"]),
  };
  vm.createContext(scope);
  vm.runInContext(helper, scope);
  return { scope, calls, state };
}

test("post-brew handling refreshes the durable queue without probing the machine", async () => {
  const { scope, calls, state } = environment();
  const response = { ok: true, brew: { id: "brew-1", state: "queued" } };
  await scope.refreshAfterBrew("serial", response);
  assert.deepEqual(calls, [["refresh"], ["flash", "Brew queued as brew-1.", "ok"], ["render"]]);
  assert.equal(state.diagnostics.output, response);
});

test("standard, replay and customized brews all use the follow-up helper", () => {
  assert.equal((html.match(/await refreshAfterBrew\(serial, response\);/g) || []).length, 3);
});

test("every UI brew supplies a fresh correlation id and the UI exposes resolution", () => {
  assert.match(html, /function newCorrelationId\(\)/);
  assert.ok((html.match(/correlationId: newCorrelationId\(\)/g) || []).length >= 3);
  assert.match(html, /data-action="continue-brew"/);
  assert.match(html, /data-action="cancel-remaining-brews"/);
  assert.match(html, /acknowledgeRisk: risky/);
  assert.match(html, /It cannot detect whether a cup is present/);
  assert.match(html, /Durable brew queue recovery failed/);
});
