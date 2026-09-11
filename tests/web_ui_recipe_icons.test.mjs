import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import test from "node:test";

const html = readFileSync(new URL("../web/index.html", import.meta.url), "utf8");
const helpers = html.slice(
  html.indexOf("  const RECIPE_ICON_ALIASES"),
  html.indexOf("  function isRouteActive("),
);

function load() {
  const scope = {
    encodeURIComponent,
    escapeHtml: (value) => String(value),
  };
  vm.createContext(scope);
  vm.runInContext(helpers, scope);
  return scope;
}

test("recipe image URLs preserve the resolved key and bust the generic-icon cache", () => {
  const scope = load();
  assert.equal(scope.recipeIconUrl("espresso"), "/icons/recipe?name=espresso&v=2");
  assert.equal(scope.recipeIconUrl("lungo"), "/icons/recipe?name=lungo&v=2");
  assert.notEqual(scope.recipeIconUrl("espresso"), scope.recipeIconUrl("lungo"));
});

test("recipe metadata resolves distinct canonical image keys", () => {
  const scope = load();
  assert.equal(scope.resolveRecipeIconKey({ iconKey: "espresso" }), "espresso");
  assert.equal(scope.resolveRecipeIconKey({ iconKey: "lungo" }), "lungo");
  assert.equal(scope.resolveRecipeIconKey({ typeName: "Hot water" }), "water");
});
