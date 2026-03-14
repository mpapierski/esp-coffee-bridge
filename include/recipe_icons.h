#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>

#include "nivona.h"

namespace recipe_icons {

struct Asset {
    const char* key;
    const uint8_t* data;
    size_t size;
    uint16_t width;
    uint16_t height;
};

const char* defaultKey();
const char* keyFromText(const String& text);
const char* keyForStandardRecipe(const nivona::ModelInfo& modelInfo, int32_t selector);
const char* keyForSavedRecipe(const nivona::ModelInfo& modelInfo,
                              bool hasTypeSelector,
                              int32_t typeSelector,
                              bool hasIconValue,
                              int32_t rawIconValue);
const char* titleForKey(const char* key);
void appendMetadata(JsonObject target, const char* key);
const Asset* findAsset(const String& key);
const Asset* embeddedAssets(size_t& countOut);

} // namespace recipe_icons
