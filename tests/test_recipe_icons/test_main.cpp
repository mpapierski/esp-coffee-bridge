#include <unity.h>

#include <cstdint>
#include <set>
#include <string>

#include "recipe_icons.h"

namespace {

uint32_t assetFingerprint(const recipe_icons::Asset& asset) {
    uint32_t hash = 2166136261U;
    for (size_t index = 0; index < asset.size; ++index) {
        hash ^= asset.data[index];
        hash *= 16777619U;
    }
    return hash;
}

void test_every_canonical_recipe_icon_has_distinct_webp_data() {
    constexpr const char* KEYS[] = {
        "americano", "caffe-latte", "cappuccino", "chilled-americano",
        "chilled-espresso", "chilled-lungo", "cloud", "coffee", "creme",
        "espresso", "flower", "frothy-milk", "heart", "hot-milk", "lungo",
        "macchiato", "milk", "my-coffee", "smily", "star", "sun",
        "undefined", "warm-milk", "water",
    };
    std::set<uint32_t> fingerprints;
    for (const char* key : KEYS) {
        const recipe_icons::Asset* asset = recipe_icons::findAsset(String(key));
        TEST_ASSERT_NOT_NULL_MESSAGE(asset, key);
        TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(12, asset->size, key);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE('R', asset->data[0], key);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE('I', asset->data[1], key);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE('F', asset->data[2], key);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE('F', asset->data[3], key);
        TEST_ASSERT_TRUE_MESSAGE(fingerprints.insert(assetFingerprint(*asset)).second, key);
    }
}

void test_standard_recipe_selectors_resolve_to_different_assets() {
    nivona::DeviceDetails details;
    details.serial = "756573071020106-----";
    const nivona::ModelInfo model = nivona::detectModelInfo(details);

    const char* espressoKey = recipe_icons::keyForStandardRecipe(model, 0);
    const char* lungoKey = recipe_icons::keyForStandardRecipe(model, 2);
    TEST_ASSERT_EQUAL_STRING("espresso", espressoKey);
    TEST_ASSERT_EQUAL_STRING("lungo", lungoKey);
    TEST_ASSERT_NOT_EQUAL(
        assetFingerprint(*recipe_icons::findAsset(String(espressoKey))),
        assetFingerprint(*recipe_icons::findAsset(String(lungoKey))));
}

void test_unknown_icon_uses_the_dedicated_fallback_asset() {
    TEST_ASSERT_NULL(recipe_icons::findAsset(String("not-a-recipe")));
    const recipe_icons::Asset* fallback =
        recipe_icons::findAsset(String(recipe_icons::defaultKey()));
    TEST_ASSERT_NOT_NULL(fallback);
    TEST_ASSERT_EQUAL_STRING("undefined", fallback->key);
}

} // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_every_canonical_recipe_icon_has_distinct_webp_data);
    RUN_TEST(test_standard_recipe_selectors_resolve_to_different_assets);
    RUN_TEST(test_unknown_icon_uses_the_dedicated_fallback_asset);
    return UNITY_END();
}
