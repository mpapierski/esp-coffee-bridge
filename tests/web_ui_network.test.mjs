import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import test from "node:test";

const html = readFileSync(new URL("../web/index.html", import.meta.url), "utf8");
const helpers = html.slice(
  html.indexOf("  function wifiModeLabel("),
  html.indexOf("  function bridgeJobLabel("),
);
const scope = { escapeHtml: (value) => String(value) };
vm.createContext(scope);
vm.runInContext(helpers, scope);

test("network labels distinguish setup AP from configured station retry", () => {
  assert.equal(scope.wifiModeLabel({ staConnected: true }), "Wi-Fi");
  assert.equal(scope.wifiModeLabel({ apActive: true }), "Setup AP");
  assert.equal(scope.wifiModeLabel({ wifiConfigured: true }), "Wi-Fi reconnecting");
  assert.equal(scope.wifiModeLabel({}), "Network unavailable");
});

test("header badge never claims AP-only mode from station disconnection alone", () => {
  assert.match(scope.wifiStatusBadge({ apActive: true }), /Setup AP/);
  assert.match(scope.wifiStatusBadge({ wifiConfigured: true }), /Wi-Fi reconnecting/);
  assert.doesNotMatch(helpers, /AP only/);
});

test("safe API reads retry once when Retry-After accompanies a 503", () => {
  const apiHelper = html.slice(
    html.indexOf("  async function api("),
    html.indexOf("  function followStaleResourceJob("),
  );
  assert.match(apiHelper, /for \(let attempt = 0; attempt < 2; attempt \+= 1\)/);
  assert.match(apiHelper, /method !== "GET" \|\| error\.status !== 503/);
  assert.match(apiHelper, /error\.retryAfter/);
});
