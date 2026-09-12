import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import test from "node:test";

const html = readFileSync(new URL("../web/index.html", import.meta.url), "utf8");
const badgeSource = html.slice(
  html.indexOf("  function machinePresenceBadge("),
  html.indexOf("  function statusBadges("),
);

test("machine badges distinguish protocol session state from scan presence", () => {
  const scope = {};
  vm.createContext(scope);
  vm.runInContext(badgeSource, scope);

  assert.match(scope.machinePresenceBadge({ online: false, nearby: true, lastSeenRssi: -61 }), /Offline/);
  assert.match(scope.machinePresenceBadge({ online: false, nearby: true, lastSeenRssi: -61 }), /Nearby · -61 dBm/);
  assert.match(scope.machinePresenceBadge({ online: true, nearby: false }), /Online/);
  assert.match(scope.machinePresenceBadge({ online: true, nearby: false }), /Not seen nearby/);
});

test("opening any machine route forces summary when its session is offline", () => {
  assert.equal((html.match(/summary = await loadMachineSummary\(serial, !machine\.online\);/g) || []).length, 3);
  assert.match(html, /data-action="reconnect-machine"/);
  assert.doesNotMatch(html, /X-Machine-Lease/);
});
