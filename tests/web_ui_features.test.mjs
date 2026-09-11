import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import test from "node:test";

const html = readFileSync(new URL("../web/index.html", import.meta.url), "utf8");
const renderer = html.slice(
  html.indexOf("  function renderMachineFeaturesSection("),
  html.indexOf("  function diagnosticsDefault("),
);

function load() {
  const scope = {
    escapeHtml: (value) => String(value),
    pretty: (value) => JSON.stringify(value),
    renderCacheNotice: () => "",
    formatByteHex: (value) => `0x${Number(value).toString(16).toUpperCase().padStart(2, "0")}`,
    formatByteBits: (value) => Number(value).toString(2).padStart(8, "0"),
  };
  vm.createContext(scope);
  vm.runInContext(renderer, scope);
  return scope;
}

test("known-silent machines render an unavailable state instead of empty feature data", () => {
  const output = load().renderMachineFeaturesSection(
    { serial: "756573071020106-----", modelName: "NICR 756" },
    {
      ok: true,
      supported: false,
      reasonCode: "hi_unavailable_for_model",
      reason: "Known HI timeout",
      features: { command: "HI", available: false },
    },
  );

  assert.match(output, /not available/);
  assert.match(output, /NICR 756/);
  assert.match(output, /Known HI timeout/);
  assert.doesNotMatch(output, /Refresh features/);
  assert.doesNotMatch(output, /No raw HI payload bytes were returned/);
});

test("machines with HI data retain the detailed feature view", () => {
  const output = load().renderMachineFeaturesSection(
    { serial: "790000000000000-----", modelName: "NICR 790" },
    {
      ok: true,
      supported: true,
      features: {
        bytes: [1],
        knownFlags: [{ title: "Image transfer", enabled: true }],
        unknownNonZeroBits: [],
      },
    },
  );

  assert.match(output, /Refresh features/);
  assert.match(output, /Image transfer/);
  assert.match(output, /Raw HI bytes/);
});
