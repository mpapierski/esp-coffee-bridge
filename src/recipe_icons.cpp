#include "recipe_icons.h"

#include <cctype>

namespace recipe_icons {
namespace {

struct CanonicalIcon {
    const char* key;
    const char* title;
};

struct Alias {
    const char* token;
    const char* key;
};

constexpr CanonicalIcon CANONICAL_ICONS[] = {
    {"espresso", "Espresso"},
    {"creme", "Creme"},
    {"lungo", "Lungo"},
    {"americano", "Americano"},
    {"cappuccino", "Cappuccino"},
    {"macchiato", "Latte macchiato"},
    {"milk", "Milk"},
    {"water", "Hot water"},
    {"caffe-latte", "Caffe latte"},
    {"coffee", "Coffee"},
    {"frothy-milk", "Frothy milk"},
    {"hot-milk", "Hot milk"},
    {"warm-milk", "Warm milk"},
    {"my-coffee", "MyCoffee"},
    {"flower", "Flower"},
    {"heart", "Heart"},
    {"smily", "Smily"},
    {"sun", "Sun"},
    {"star", "Star"},
    {"cloud", "Cloud"},
    {"chilled-espresso", "Chilled espresso"},
    {"chilled-lungo", "Chilled lungo"},
    {"chilled-americano", "Chilled americano"},
    {"undefined", "Undefined"},
};

constexpr Alias ICON_ALIASES[] = {
    {"espresso", "espresso"},
    {"creme", "creme"},
    {"cream", "creme"},
    {"lungo", "lungo"},
    {"americano", "americano"},
    {"cappuccino", "cappuccino"},
    {"macchiato", "macchiato"},
    {"latte_macchiato", "macchiato"},
    {"milk", "milk"},
    {"water", "water"},
    {"hot_water", "water"},
    {"caffe_latte", "caffe-latte"},
    {"coffee", "coffee"},
    {"frothy_milk", "frothy-milk"},
    {"hot_milk", "hot-milk"},
    {"warm_milk", "warm-milk"},
    {"mycoffee", "my-coffee"},
    {"my_coffee", "my-coffee"},
    {"flower", "flower"},
    {"heart", "heart"},
    {"smily", "smily"},
    {"smiley", "smily"},
    {"sun", "sun"},
    {"star", "star"},
    {"cloud", "cloud"},
    {"chilled_espresso", "chilled-espresso"},
    {"chilled_lungo", "chilled-lungo"},
    {"chilled_americano", "chilled-americano"},
    {"undefined", "undefined"},
    {"unknown", "undefined"},
};

String normalizeToken(const String& input) {
    String token;
    token.reserve(input.length());
    bool lastWasSeparator = false;
    for (size_t i = 0; i < input.length(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(input[i]);
        if (std::isalnum(ch)) {
            token += static_cast<char>(std::tolower(ch));
            lastWasSeparator = false;
        } else if (!lastWasSeparator && !token.isEmpty()) {
            token += '_';
            lastWasSeparator = true;
        }
    }
    while (token.endsWith("_")) {
        token.remove(token.length() - 1);
    }
    return token;
}

const char* keyForFamily600(int32_t rawIconValue) {
    switch (rawIconValue) {
        case 1:
            return "espresso";
        case 4:
            return "americano";
        case 5:
            return "cappuccino";
        case 8:
            return "water";
        case 9:
            return "my-coffee";
        case 12:
            return "flower";
        case 13:
            return "heart";
        case 14:
            return "smily";
        case 15:
            return "sun";
        case 16:
            return "star";
        case 19:
            return "coffee";
        case 20:
            return "frothy-milk";
        default:
            return nullptr;
    }
}

const char* keyForFamily700(int32_t rawIconValue) {
    switch (rawIconValue) {
        case 1:
            return "espresso";
        case 2:
            return "creme";
        case 3:
            return "lungo";
        case 4:
            return "americano";
        case 5:
            return "cappuccino";
        case 6:
            return "macchiato";
        case 7:
            return "milk";
        case 8:
            return "water";
        case 9:
            return "my-coffee";
        case 12:
            return "flower";
        case 13:
            return "heart";
        case 14:
            return "smily";
        case 15:
            return "sun";
        case 16:
            return "star";
        default:
            return nullptr;
    }
}

const char* keyForFamily900(int32_t rawIconValue) {
    switch (rawIconValue) {
        case 1:
            return "espresso";
        case 2:
            return "coffee";
        case 3:
            return "americano";
        case 4:
            return "cappuccino";
        case 5:
            return "caffe-latte";
        case 6:
            return "macchiato";
        case 7:
            return "hot-milk";
        case 8:
            return "water";
        case 9:
            return "my-coffee";
        default:
            return nullptr;
    }
}

const char* keyForFamily1000(int32_t rawIconValue) {
    switch (rawIconValue) {
        case 1:
            return "espresso";
        case 2:
            return "coffee";
        case 3:
            return "americano";
        case 4:
            return "cappuccino";
        case 5:
            return "caffe-latte";
        case 6:
            return "macchiato";
        case 7:
            return "warm-milk";
        case 8:
            return "water";
        case 9:
            return "my-coffee";
        case 13:
            return "hot-milk";
        case 14:
            return "frothy-milk";
        case 15:
            return "star";
        case 16:
            return "flower";
        case 17:
            return "sun";
        case 18:
            return "heart";
        case 19:
            return "smily";
        case 20:
            return "cloud";
        default:
            return nullptr;
    }
}

const char* keyForFamily8000(int32_t rawIconValue) {
    switch (rawIconValue) {
        case 0:
            return "espresso";
        case 1:
            return "coffee";
        case 2:
            return "americano";
        case 3:
            return "cappuccino";
        case 4:
            return "caffe-latte";
        case 5:
            return "macchiato";
        case 6:
            return "milk";
        case 7:
            return "water";
        case 21:
            return "chilled-espresso";
        case 22:
            return "chilled-lungo";
        case 23:
            return "chilled-americano";
        default:
            return nullptr;
    }
}

const char* keyFromRawSavedIcon(const String& familyKey, int32_t rawIconValue) {
    if (familyKey == "600") {
        return keyForFamily600(rawIconValue);
    }
    if (familyKey == "700") {
        return keyForFamily700(rawIconValue);
    }
    if (familyKey == "900" || familyKey == "900-light") {
        return keyForFamily900(rawIconValue);
    }
    if (familyKey == "1030" || familyKey == "1040") {
        return keyForFamily1000(rawIconValue);
    }
    if (familyKey == "8000") {
        return keyForFamily8000(rawIconValue);
    }
    return nullptr;
}

const char* keyFromSelector(const nivona::ModelInfo& modelInfo, int32_t selector) {
    if (selector < 0 || selector > 255) {
        return nullptr;
    }
    const nivona::StandardRecipeDescriptor* descriptor =
        nivona::findStandardRecipeBySelector(modelInfo, static_cast<uint8_t>(selector));
    if (descriptor == nullptr) {
        return nullptr;
    }
    if (descriptor->name != nullptr && descriptor->name[0] != '\0') {
        return keyFromText(String(descriptor->name));
    }
    if (descriptor->title != nullptr && descriptor->title[0] != '\0') {
        return keyFromText(String(descriptor->title));
    }
    return nullptr;
}

} // namespace

const char* defaultKey() {
    return "undefined";
}

const char* keyFromText(const String& text) {
    const String token = normalizeToken(text);
    if (token.isEmpty()) {
        return nullptr;
    }
    for (const auto& alias : ICON_ALIASES) {
        if (token == alias.token) {
            return alias.key;
        }
    }
    return nullptr;
}

const char* keyForStandardRecipe(const nivona::ModelInfo& modelInfo, int32_t selector) {
    const char* key = keyFromSelector(modelInfo, selector);
    return key != nullptr ? key : defaultKey();
}

const char* keyForSavedRecipe(const nivona::ModelInfo& modelInfo,
                              bool hasTypeSelector,
                              int32_t typeSelector,
                              bool hasIconValue,
                              int32_t rawIconValue) {
    if (modelInfo.is79xModel && hasTypeSelector) {
        const char* key = keyFromSelector(modelInfo, typeSelector);
        if (key != nullptr) {
            return key;
        }
    }
    if (hasIconValue) {
        const char* key = keyFromRawSavedIcon(modelInfo.familyKey, rawIconValue);
        if (key != nullptr) {
            return key;
        }
    }
    if (hasTypeSelector) {
        const char* key = keyFromSelector(modelInfo, typeSelector);
        if (key != nullptr) {
            return key;
        }
    }
    return defaultKey();
}

const char* titleForKey(const char* key) {
    if (key == nullptr || key[0] == '\0') {
        return "Undefined";
    }
    const String token = normalizeToken(String(key));
    for (const auto& icon : CANONICAL_ICONS) {
        if (token == normalizeToken(String(icon.key))) {
            return icon.title;
        }
    }
    return "Undefined";
}

void appendMetadata(JsonObject target, const char* key) {
    const char* resolvedKey = (key != nullptr && key[0] != '\0') ? key : defaultKey();
    target["iconKey"] = resolvedKey;
    target["iconName"] = titleForKey(resolvedKey);
}

const Asset* findAsset(const String& key) {
    const String normalized = normalizeToken(key);
    size_t count = 0;
    const Asset* assets = embeddedAssets(count);
    for (size_t i = 0; i < count; ++i) {
        if (normalized == normalizeToken(String(assets[i].key))) {
            return &assets[i];
        }
    }
    return nullptr;
}

} // namespace recipe_icons
