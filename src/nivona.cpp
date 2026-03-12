#include "nivona.h"

#include <algorithm>
#include <cctype>

namespace nivona {

namespace {

constexpr uint8_t START_BYTE = 0x53;
constexpr uint8_t END_BYTE   = 0x45;

const uint8_t WORKING_STREAM_KEY[32] = {
    0x4E, 0x49, 0x56, 0x5F, 0x30, 0x36, 0x30, 0x36, 0x31, 0x36, 0x5F, 0x56, 0x31, 0x30, 0x5F, 0x31,
    0x2A, 0x39, 0x23, 0x33, 0x21, 0x34, 0x24, 0x36, 0x2B, 0x34, 0x72, 0x65, 0x73, 0x2D, 0x3F, 0x33,
};

const uint8_t HU_TABLE[256] = {
    0x62, 0x06, 0x55, 0x96, 0x24, 0x17, 0x70, 0xA4, 0x87, 0xCF, 0xA9, 0x05, 0x1A, 0x40, 0xA5, 0xDB,
    0x3D, 0x14, 0x44, 0x59, 0x82, 0x3F, 0x34, 0x66, 0x18, 0xE5, 0x84, 0xF5, 0x50, 0xD8, 0xC3, 0x73,
    0x5A, 0xA8, 0x9C, 0xCB, 0xB1, 0x78, 0x02, 0xBE, 0xBC, 0x07, 0x64, 0xB9, 0xAE, 0xF3, 0xA2, 0x0A,
    0xED, 0x12, 0xFD, 0xE1, 0x08, 0xD0, 0xAC, 0xF4, 0xFF, 0x7E, 0x65, 0x4F, 0x91, 0xEB, 0xE4, 0x79,
    0x7B, 0xFB, 0x43, 0xFA, 0xA1, 0x00, 0x6B, 0x61, 0xF1, 0x6F, 0xB5, 0x52, 0xF9, 0x21, 0x45, 0x37,
    0x3B, 0x99, 0x1D, 0x09, 0xD5, 0xA7, 0x54, 0x5D, 0x1E, 0x2E, 0x5E, 0x4B, 0x97, 0x72, 0x49, 0xDE,
    0xC5, 0x60, 0xD2, 0x2D, 0x10, 0xE3, 0xF8, 0xCA, 0x33, 0x98, 0xFC, 0x7D, 0x51, 0xCE, 0xD7, 0xBA,
    0x27, 0x9E, 0xB2, 0xBB, 0x83, 0x88, 0x01, 0x31, 0x32, 0x11, 0x8D, 0x5B, 0x2F, 0x81, 0x3C, 0x63,
    0x9A, 0x23, 0x56, 0xAB, 0x69, 0x22, 0x26, 0xC8, 0x93, 0x3A, 0x4D, 0x76, 0xAD, 0xF6, 0x4C, 0xFE,
    0x85, 0xE8, 0xC4, 0x90, 0xC6, 0x7C, 0x35, 0x04, 0x6C, 0x4A, 0xDF, 0xEA, 0x86, 0xE6, 0x9D, 0x8B,
    0xBD, 0xCD, 0xC7, 0x80, 0xB0, 0x13, 0xD3, 0xEC, 0x7F, 0xC0, 0xE7, 0x46, 0xE9, 0x58, 0x92, 0x2C,
    0xB7, 0xC9, 0x16, 0x53, 0x0D, 0xD6, 0x74, 0x6D, 0x9F, 0x20, 0x5F, 0xE2, 0x8C, 0xDC, 0x39, 0x0C,
    0xDD, 0x1F, 0xD1, 0xB6, 0x8F, 0x5C, 0x95, 0xB8, 0x94, 0x3E, 0x71, 0x41, 0x25, 0x1B, 0x6A, 0xA6,
    0x03, 0x0E, 0xCC, 0x48, 0x15, 0x29, 0x38, 0x42, 0x1C, 0xC1, 0x28, 0xD9, 0x19, 0x36, 0xB3, 0x75,
    0xEE, 0x57, 0xF0, 0x9B, 0xB4, 0xAA, 0xF2, 0xD4, 0xBF, 0xA3, 0x4E, 0xDA, 0x89, 0xC2, 0xAF, 0x6E,
    0x2B, 0x77, 0xE0, 0x47, 0x7A, 0x8E, 0x2A, 0xA0, 0x68, 0x30, 0xF7, 0x67, 0x0F, 0x0B, 0x8A, 0xEF,
};

const SettingValueOption HARDNESS_OPTIONS[] = {
    {0x0000, "soft"},
    {0x0001, "medium"},
    {0x0002, "hard"},
    {0x0003, "very hard"},
};

const SettingValueOption OFF_ON_OPTIONS[] = {
    {0x0000, "off"},
    {0x0001, "on"},
};

const SettingValueOption AUTO_OFF_8000_OPTIONS[] = {
    {0x0000, "10 min"},
    {0x0001, "30 min"},
    {0x0002, "1 h"},
    {0x0003, "2 h"},
    {0x0004, "4 h"},
    {0x0005, "6 h"},
    {0x0006, "8 h"},
    {0x0007, "10 h"},
    {0x0008, "12 h"},
    {0x0009, "14 h"},
    {0x0010, "16 h"},
};

const SettingValueOption AUTO_OFF_STANDARD_OPTIONS[] = {
    {0x0000, "10 min"},
    {0x0001, "30 min"},
    {0x0002, "1 h"},
    {0x0003, "2 h"},
    {0x0004, "4 h"},
    {0x0005, "6 h"},
    {0x0006, "8 h"},
    {0x0007, "10 h"},
    {0x0008, "12 h"},
    {0x0009, "off"},
};

const SettingValueOption TEMPERATURE_ON_OFF_OPTIONS[] = {
    {0x0000, "off"},
    {0x0001, "on"},
};

const SettingValueOption TEMPERATURE_OPTIONS[] = {
    {0x0000, "normal"},
    {0x0001, "high"},
    {0x0002, "max"},
    {0x0003, "individual"},
};

const SettingValueOption PROFILE_STANDARD_OPTIONS[] = {
    {0x0000, "dynamic"},
    {0x0001, "constant"},
    {0x0002, "intense"},
    {0x0003, "individual"},
};

const SettingValueOption PROFILE_1040_OPTIONS[] = {
    {0x0000, "dynamic"},
    {0x0001, "constant"},
    {0x0002, "intense"},
    {0x0003, "quick"},
    {0x0004, "individual"},
};

const SettingValueOption MILK_TEMPERATURE_1030_OPTIONS[] = {
    {0x0000, "high"},
    {0x0001, "max"},
    {0x0002, "individual"},
};

const SettingValueOption MILK_TEMPERATURE_1040_OPTIONS[] = {
    {0x0000, "normal"},
    {0x0001, "high"},
    {0x0002, "hot"},
    {0x0003, "max"},
    {0x0004, "individual"},
};

const SettingValueOption MILK_FOAM_TEMPERATURE_1040_OPTIONS[] = {
    {0x0000, "warm"},
    {0x0001, "max"},
    {0x0002, "individual"},
};

const SettingValueOption POWER_ON_FROTHER_TIME_1040_OPTIONS[] = {
    {0x0000, "10 min"},
    {0x0001, "20 min"},
    {0x0002, "30 min"},
    {0x0003, "40 min"},
};

const SettingProbeDescriptor SETTINGS_8000_PROBES[] = {
    {"water_hardness", "Water hardness", 101, HARDNESS_OPTIONS, arraySize(HARDNESS_OPTIONS)},
    {"off_rinse", "Off-rinse", 103, OFF_ON_OPTIONS, arraySize(OFF_ON_OPTIONS)},
    {"auto_off", "Auto-off", 104, AUTO_OFF_8000_OPTIONS, arraySize(AUTO_OFF_8000_OPTIONS)},
    {"coffee_temperature", "Coffee temperature", 105, TEMPERATURE_ON_OFF_OPTIONS, arraySize(TEMPERATURE_ON_OFF_OPTIONS)},
};

const SettingProbeDescriptor SETTINGS_600_700_BASE_PROBES[] = {
    {"water_hardness", "Water hardness", 101, HARDNESS_OPTIONS, arraySize(HARDNESS_OPTIONS)},
    {"temperature", "Temperature", 102, TEMPERATURE_OPTIONS, arraySize(TEMPERATURE_OPTIONS)},
    {"off_rinse", "Off-rinse", 103, OFF_ON_OPTIONS, arraySize(OFF_ON_OPTIONS)},
    {"auto_off", "Auto-off", 104, AUTO_OFF_STANDARD_OPTIONS, arraySize(AUTO_OFF_STANDARD_OPTIONS)},
    {"profile", "Profile", 106, PROFILE_STANDARD_OPTIONS, arraySize(PROFILE_STANDARD_OPTIONS)},
};

const SettingProbeDescriptor SETTINGS_900_PROBES[] = {
    {"water_hardness", "Water hardness", 102, HARDNESS_OPTIONS, arraySize(HARDNESS_OPTIONS)},
    {"off_rinse", "Off-rinse", 103, OFF_ON_OPTIONS, arraySize(OFF_ON_OPTIONS)},
    {"auto_off", "Auto-off", 109, AUTO_OFF_STANDARD_OPTIONS, arraySize(AUTO_OFF_STANDARD_OPTIONS)},
};

const SettingProbeDescriptor SETTINGS_900_LIGHT_PROBES[] = {
    {"water_hardness", "Water hardness", 102, HARDNESS_OPTIONS, arraySize(HARDNESS_OPTIONS)},
    {"auto_off", "Auto-off", 109, AUTO_OFF_STANDARD_OPTIONS, arraySize(AUTO_OFF_STANDARD_OPTIONS)},
};

const SettingProbeDescriptor SETTINGS_1030_PROBES[] = {
    {"water_hardness", "Water hardness", 102, HARDNESS_OPTIONS, arraySize(HARDNESS_OPTIONS)},
    {"off_rinse", "Off-rinse", 103, OFF_ON_OPTIONS, arraySize(OFF_ON_OPTIONS)},
    {"auto_off", "Auto-off", 109, AUTO_OFF_STANDARD_OPTIONS, arraySize(AUTO_OFF_STANDARD_OPTIONS)},
    {"profile", "Profile", 113, PROFILE_STANDARD_OPTIONS, arraySize(PROFILE_STANDARD_OPTIONS)},
    {"coffee_temperature", "Coffee temperature", 114, TEMPERATURE_OPTIONS, arraySize(TEMPERATURE_OPTIONS)},
    {"water_temperature", "Water temperature", 115, TEMPERATURE_OPTIONS, arraySize(TEMPERATURE_OPTIONS)},
    {"milk_temperature", "Milk temperature", 116, MILK_TEMPERATURE_1030_OPTIONS, arraySize(MILK_TEMPERATURE_1030_OPTIONS)},
};

const SettingProbeDescriptor SETTINGS_1040_PROBES[] = {
    {"water_hardness", "Water hardness", 102, HARDNESS_OPTIONS, arraySize(HARDNESS_OPTIONS)},
    {"off_rinse", "Off-rinse", 103, OFF_ON_OPTIONS, arraySize(OFF_ON_OPTIONS)},
    {"auto_off", "Auto-off", 109, AUTO_OFF_STANDARD_OPTIONS, arraySize(AUTO_OFF_STANDARD_OPTIONS)},
    {"profile", "Profile", 113, PROFILE_1040_OPTIONS, arraySize(PROFILE_1040_OPTIONS)},
    {"coffee_temperature", "Coffee temperature", 114, TEMPERATURE_OPTIONS, arraySize(TEMPERATURE_OPTIONS)},
    {"water_temperature", "Water temperature", 115, TEMPERATURE_OPTIONS, arraySize(TEMPERATURE_OPTIONS)},
    {"milk_temperature", "Milk temperature", 116, MILK_TEMPERATURE_1040_OPTIONS, arraySize(MILK_TEMPERATURE_1040_OPTIONS)},
    {"milk_foam_temperature", "Milk foam temperature", 117, MILK_FOAM_TEMPERATURE_1040_OPTIONS, arraySize(MILK_FOAM_TEMPERATURE_1040_OPTIONS)},
    {"power_on_rinse", "Power-on rinse", 118, OFF_ON_OPTIONS, arraySize(OFF_ON_OPTIONS)},
    {"power_on_frother_time", "Power-on frother time", 119, POWER_ON_FROTHER_TIME_1040_OPTIONS, arraySize(POWER_ON_FROTHER_TIME_1040_OPTIONS)},
};

const StandardRecipeDescriptor RECIPES_600[] = {
    {0, "espresso", "Espresso"},
    {1, "coffee", "Coffee"},
    {2, "americano", "Americano"},
    {3, "cappuccino", "Cappuccino"},
    {4, "frothy_milk", "Frothy milk"},
    {5, "hot_water", "Hot water"},
};

const StandardRecipeDescriptor RECIPES_700[] = {
    {0, "espresso", "Espresso"},
    {1, "cream", "Cream"},
    {2, "lungo", "Lungo"},
    {3, "americano", "Americano"},
    {4, "cappuccino", "Cappuccino"},
    {5, "latte_macchiato", "Latte macchiato"},
    {6, "milk", "Milk"},
    {7, "hot_water", "Hot water"},
};

const StandardRecipeDescriptor RECIPES_79X[] = {
    {0, "espresso", "Espresso"},
    {1, "coffee", "Coffee"},
    {2, "americano", "Americano"},
    {3, "cappuccino", "Cappuccino"},
    {5, "latte_macchiato", "Latte macchiato"},
    {6, "milk", "Milk"},
    {7, "hot_water", "Hot water"},
};

const StandardRecipeDescriptor RECIPES_900[] = {
    {0, "espresso", "Espresso"},
    {1, "coffee", "Coffee"},
    {2, "americano", "Americano"},
    {3, "cappuccino", "Cappuccino"},
    {4, "caffe_latte", "Caffe latte"},
    {5, "latte_macchiato", "Latte macchiato"},
    {6, "hot_milk", "Hot milk"},
    {7, "hot_water", "Hot water"},
};

const StandardRecipeDescriptor RECIPES_1030[] = {
    {0, "espresso", "Espresso"},
    {1, "coffee", "Coffee"},
    {2, "americano", "Americano"},
    {3, "cappuccino", "Cappuccino"},
    {4, "caffe_latte", "Caffe latte"},
    {5, "latte_macchiato", "Latte macchiato"},
    {6, "hot_water", "Hot water"},
    {7, "warm_milk", "Warm milk"},
    {8, "hot_milk", "Hot milk"},
    {9, "frothy_milk", "Frothy milk"},
};

const StandardRecipeDescriptor RECIPES_1040[] = {
    {0, "espresso", "Espresso"},
    {1, "coffee", "Coffee"},
    {2, "americano", "Americano"},
    {3, "cappuccino", "Cappuccino"},
    {4, "caffe_latte", "Caffe latte"},
    {5, "latte_macchiato", "Latte macchiato"},
    {6, "hot_water", "Hot water"},
    {7, "warm_milk", "Warm milk"},
    {8, "frothy_milk", "Frothy milk"},
};

const StandardRecipeDescriptor RECIPES_8000[] = {
    {0, "espresso", "Espresso"},
    {1, "coffee", "Coffee"},
    {2, "americano", "Americano"},
    {3, "cappuccino", "Cappuccino"},
    {4, "caffe_latte", "Caffe latte"},
    {5, "latte_macchiato", "Latte macchiato"},
    {6, "milk", "Milk"},
    {7, "hot_water", "Hot water"},
};

struct ModelRule {
    const char* token;
    const char* appModel;
    const char* modelCode;
    const char* modelName;
    const char* familyKey;
    SettingsFamily settingsFamily;
    uint8_t myCoffeeSlotCount;
    RecipeTextEncoding textEncoding;
    bool is79xModel;
    bool hasAromaBalanceProfile;
    bool fluidWriteScale10;
    bool supportsStats;
    uint8_t brewCommandMode;
};

const ModelRule MODEL_RULES[] = {
    {"8101", "NICR8101", "8101", "NICR 8101", "8000", SettingsFamily::Family8000, 9, RecipeTextEncoding::Utf16Le, false, true, false, true, 0x04},
    {"8103", "NICR8103", "8103", "NICR 8103", "8000", SettingsFamily::Family8000, 9, RecipeTextEncoding::Utf16Le, false, true, false, true, 0x04},
    {"8107", "NICR8107", "8107", "NICR 8107", "8000", SettingsFamily::Family8000, 9, RecipeTextEncoding::Utf16Le, false, true, false, true, 0x04},
    {"040", "NICR1040", "1040", "NICR 1040", "1040", SettingsFamily::Family1040, 18, RecipeTextEncoding::Utf16Le, false, true, false, false, 0x0B},
    {"030", "NICR1030", "1030", "NICR 1030", "1030", SettingsFamily::Family1030, 18, RecipeTextEncoding::Utf16Le, false, true, false, false, 0x0B},
    {"660", "Eugster660", "660", "NICR 660", "600", SettingsFamily::Family600, 1, RecipeTextEncoding::LegacySingleByte, false, true, false, false, 0x0B},
    {"670", "Eugster670", "670", "NICR 670", "600", SettingsFamily::Family600, 5, RecipeTextEncoding::LegacySingleByte, false, true, false, false, 0x0B},
    {"675", "Eugster675", "675", "NICR 675", "600", SettingsFamily::Family600, 5, RecipeTextEncoding::LegacySingleByte, false, true, false, false, 0x0B},
    {"680", "Eugster680", "680", "NICR 680", "600", SettingsFamily::Family600, 5, RecipeTextEncoding::LegacySingleByte, false, true, false, false, 0x0B},
    {"756", "Eugster756", "756", "NICR 756", "700", SettingsFamily::Family700, 1, RecipeTextEncoding::LegacySingleByte, false, true, false, false, 0x0B},
    {"758", "Eugster758", "758", "NICR 758", "700", SettingsFamily::Family700, 1, RecipeTextEncoding::LegacySingleByte, false, false, false, false, 0x0B},
    {"759", "Eugster759", "759", "NICR 759", "700", SettingsFamily::Family700, 1, RecipeTextEncoding::LegacySingleByte, false, true, false, false, 0x0B},
    {"768", "Eugster768", "768", "NICR 768", "700", SettingsFamily::Family700, 1, RecipeTextEncoding::LegacySingleByte, false, true, false, false, 0x0B},
    {"769", "Eugster769", "769", "NICR 769", "700", SettingsFamily::Family700, 1, RecipeTextEncoding::LegacySingleByte, false, true, false, false, 0x0B},
    {"778", "Eugster778", "778", "NICR 778", "700", SettingsFamily::Family700, 1, RecipeTextEncoding::LegacySingleByte, false, true, false, false, 0x0B},
    {"779", "Eugster779", "779", "NICR 779", "700", SettingsFamily::Family700, 1, RecipeTextEncoding::LegacySingleByte, false, true, false, false, 0x0B},
    {"788", "Eugster788", "788", "NICR 788", "700", SettingsFamily::Family700, 5, RecipeTextEncoding::LegacySingleByte, false, true, false, false, 0x0B},
    {"789", "Eugster789", "789", "NICR 789", "700", SettingsFamily::Family700, 5, RecipeTextEncoding::LegacySingleByte, false, true, false, false, 0x0B},
    {"790", "Eugster790", "790", "NICR 790", "79x", SettingsFamily::Family700, 5, RecipeTextEncoding::LegacySingleByte, true, true, false, false, 0x0B},
    {"791", "Eugster791", "791", "NICR 791", "79x", SettingsFamily::Family700, 5, RecipeTextEncoding::LegacySingleByte, true, true, false, false, 0x0B},
    {"792", "Eugster792", "792", "NICR 792", "79x", SettingsFamily::Family700, 5, RecipeTextEncoding::LegacySingleByte, true, true, false, false, 0x0B},
    {"793", "Eugster793", "793", "NICR 793", "79x", SettingsFamily::Family700, 5, RecipeTextEncoding::LegacySingleByte, true, true, false, false, 0x0B},
    {"794", "Eugster794", "794", "NICR 794", "79x", SettingsFamily::Family700, 5, RecipeTextEncoding::LegacySingleByte, true, true, false, false, 0x0B},
    {"795", "Eugster795", "795", "NICR 795", "79x", SettingsFamily::Family700, 5, RecipeTextEncoding::LegacySingleByte, true, true, false, false, 0x0B},
    {"796", "Eugster796", "796", "NICR 796", "79x", SettingsFamily::Family700, 5, RecipeTextEncoding::LegacySingleByte, true, true, false, false, 0x0B},
    {"797", "Eugster797", "797", "NICR 797", "79x", SettingsFamily::Family700, 5, RecipeTextEncoding::LegacySingleByte, true, true, false, false, 0x0B},
    {"799", "Eugster799", "799", "NICR 799", "79x", SettingsFamily::Family700, 5, RecipeTextEncoding::LegacySingleByte, true, true, false, false, 0x0B},
    {"920", "NICR920", "920", "NICR 920", "900", SettingsFamily::Family900, 9, RecipeTextEncoding::Utf16Le, false, true, true, false, 0x0B},
    {"930", "NICR930", "930", "NICR 930", "900", SettingsFamily::Family900, 9, RecipeTextEncoding::Utf16Le, false, true, true, false, 0x0B},
    {"960", "Eugster960", "960", "NICR 960", "900-light", SettingsFamily::Family900Light, 9, RecipeTextEncoding::Utf16Le, false, true, true, false, 0x0B},
    {"965", "Eugster965", "965", "NICR 965", "900-light", SettingsFamily::Family900Light, 9, RecipeTextEncoding::Utf16Le, false, true, true, false, 0x0B},
    {"970", "Eugster970", "970", "NICR 970", "900-light", SettingsFamily::Family900Light, 9, RecipeTextEncoding::Utf16Le, false, true, true, false, 0x0B},
};

bool containsModelToken(const String& haystack, const char* token) {
    return !haystack.isEmpty() && haystack.indexOf(token) >= 0;
}

void applyModelRule(ModelInfo& out, const ModelRule& rule) {
    out.appModel = rule.appModel;
    out.modelCode = rule.modelCode;
    out.modelName = rule.modelName;
    out.familyKey = rule.familyKey;
    out.settingsFamily = rule.settingsFamily;
    out.myCoffeeSlotCount = rule.myCoffeeSlotCount;
    out.recipeTextEncoding = rule.textEncoding;
    out.is79xModel = rule.is79xModel;
    out.hasAromaBalanceProfile = rule.hasAromaBalanceProfile;
    out.fluidWriteScale10 = rule.fluidWriteScale10;
    out.supportsSettings = rule.settingsFamily != SettingsFamily::Unknown;
    out.supportsStats = rule.supportsStats;
    out.supportsSavedRecipes = rule.myCoffeeSlotCount > 0;
    out.supportsRecipeWrites = rule.myCoffeeSlotCount > 0;
    out.brewCommandMode = rule.brewCommandMode;
}

ByteVector joinChunks(const std::vector<ByteVector>& chunks) {
    ByteVector joined;
    size_t totalSize = 0;
    for (const auto& chunk : chunks) {
        totalSize += chunk.size();
    }
    joined.reserve(totalSize);
    for (const auto& chunk : chunks) {
        joined.insert(joined.end(), chunk.begin(), chunk.end());
    }
    return joined;
}

String decodeUtf16Le(const uint8_t* data, size_t length) {
    String out;
    out.reserve(length / 2);
    for (size_t i = 0; i + 1 < length; i += 2) {
        const uint16_t code = static_cast<uint16_t>(data[i]) | (static_cast<uint16_t>(data[i + 1]) << 8);
        if (code == 0) {
            break;
        }
        out += code <= 0x7F ? static_cast<char>(code) : '?';
    }
    return out;
}

String decodeLegacyRaw(const uint8_t* data, size_t length) {
    String out;
    out.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        if (data[i] == 0x00) {
            break;
        }
        out += static_cast<char>(data[i]);
    }
    return out;
}

String sanitizeLegacyDisplay(const String& raw) {
    String out;
    out.reserve(raw.length());
    for (size_t i = 0; i < raw.length(); ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (std::isalnum(c)) {
            out += static_cast<char>(c);
        }
    }
    return out;
}

ByteVector encodeLegacyName(const String& name) {
    ByteVector out(64, 0x00);
    size_t cursor = 0;
    for (size_t i = 0; i < name.length() && cursor < out.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!std::isalnum(c)) {
            continue;
        }
        out[cursor++] = c <= 0xFF ? c : '?';
    }
    return out;
}

ByteVector encodeUtf16LeName(const String& name) {
    ByteVector out(64, 0x00);
    size_t cursor = 0;
    for (size_t i = 0; i < name.length() && cursor + 1 < out.size(); ++i) {
        out[cursor++] = static_cast<uint8_t>(name[i]);
        out[cursor++] = 0x00;
    }
    return out;
}

} // namespace

const RegisterProbe STATS_8000_PROBES[] = {
    {"espresso", 200},
    {"coffee", 201},
    {"americano", 202},
    {"cappuccino", 203},
    {"caffe_latte", 204},
    {"macchiato", 205},
    {"warm_milk", 206},
    {"hot_water", 207},
    {"my_coffee", 208},
    {"steam_drinks", 209},
    {"powder_coffee", 210},
    {"total_beverages", 213},
    {"clean_coffee_system", 214},
    {"clean_frother", 215},
    {"rinse_cycles", 216},
    {"filter_changes", 219},
    {"descaling", 220},
    {"beverages_via_app", 221},
    {"descale_percent", 600},
    {"descale_warning", 601},
    {"brew_unit_clean_percent", 610},
    {"brew_unit_clean_warning", 611},
    {"frother_clean_percent", 620},
    {"frother_clean_warning", 621},
    {"filter_percent", 640},
    {"filter_warning", 641},
    {"filter_dependency", 642},
};

const size_t STATS_8000_PROBE_COUNT = arraySize(STATS_8000_PROBES);

String hexEncode(const uint8_t* data, size_t length) {
    static const char* HEX_DIGITS = "0123456789ABCDEF";
    String out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out += HEX_DIGITS[(data[i] >> 4) & 0x0F];
        out += HEX_DIGITS[data[i] & 0x0F];
    }
    return out;
}

String hexEncode(const ByteVector& data) {
    return hexEncode(data.data(), data.size());
}

bool hexDecode(const String& text, ByteVector& out, String& error) {
    out.clear();
    String compact;
    compact.reserve(text.length());
    for (size_t i = 0; i < text.length(); ++i) {
        const char c = text[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            continue;
        }
        compact += c;
    }

    if ((compact.length() % 2) != 0) {
        error = "hex string length must be even";
        return false;
    }

    out.reserve(compact.length() / 2);
    for (size_t i = 0; i < compact.length(); i += 2) {
        char buf[3] = {compact[i], compact[i + 1], '\0'};
        char* end   = nullptr;
        const long value = strtol(buf, &end, 16);
        if (end == nullptr || *end != '\0' || value < 0 || value > 0xFF) {
            error = "invalid hex byte";
            return false;
        }
        out.push_back(static_cast<uint8_t>(value));
    }
    return true;
}

String printableAscii(const ByteVector& bytes) {
    String out;
    out.reserve(bytes.size());
    for (uint8_t byte : bytes) {
        const char c = static_cast<char>(byte);
        out += (c >= 32 && c < 127) ? c : '.';
    }
    return out;
}

String upperCopy(const String& value) {
    String out = value;
    out.toUpperCase();
    return out;
}

String formatCodeHex(uint16_t code) {
    ByteVector bytes{
        static_cast<uint8_t>((code >> 8) & 0xFF),
        static_cast<uint8_t>(code & 0xFF),
    };
    return hexEncode(bytes);
}

bool looksLikeNivonaName(const String& name) {
    if (name.length() < 10 || !name.endsWith("-----")) {
        return false;
    }
    size_t digitCount = 0;
    for (size_t i = 0; i < name.length(); ++i) {
        if (isDigit(name[i])) {
            digitCount++;
        }
    }
    return digitCount >= 10;
}

ByteVector rc4Transform(const ByteVector& data) {
    uint8_t state[256];
    for (size_t i = 0; i < 256; ++i) {
        state[i] = static_cast<uint8_t>(i);
    }

    uint8_t j = 0;
    for (size_t i = 0; i < 256; ++i) {
        j = static_cast<uint8_t>(j + state[i] + WORKING_STREAM_KEY[i % sizeof(WORKING_STREAM_KEY)]);
        std::swap(state[i], state[j]);
    }

    ByteVector output;
    output.reserve(data.size());
    uint8_t i = 0;
    j = 0;
    for (uint8_t byte : data) {
        i = static_cast<uint8_t>(i + 1);
        j = static_cast<uint8_t>(j + state[i]);
        std::swap(state[i], state[j]);
        const uint8_t k = state[static_cast<uint8_t>(state[i] + state[j])];
        output.push_back(byte ^ k);
    }
    return output;
}

uint8_t onesComplementChecksum(const char* command, const ByteVector& body) {
    uint32_t sum = 0;
    sum += static_cast<uint8_t>(command[0]);
    sum += static_cast<uint8_t>(command[1]);
    for (uint8_t byte : body) {
        sum += byte;
    }
    return static_cast<uint8_t>((~sum) & 0xFF);
}

ByteVector deriveHuVerifier(const ByteVector& buffer, size_t start, size_t count) {
    ByteVector out;
    out.reserve(2);

    uint8_t s = HU_TABLE[buffer[start]];
    for (size_t i = start + 1; i < start + count; ++i) {
        s = HU_TABLE[s ^ buffer[i]];
    }
    out.push_back(static_cast<uint8_t>((s + 0x5D) & 0xFF));

    s = HU_TABLE[static_cast<uint8_t>(buffer[start] + 1)];
    for (size_t i = start + 1; i < start + count; ++i) {
        s = HU_TABLE[s ^ buffer[i]];
    }
    out.push_back(static_cast<uint8_t>((s + 0xA7) & 0xFF));
    return out;
}

ByteVector buildPacket(const char* command, const ByteVector& payload, const ByteVector* sessionKey, bool encrypt) {
    ByteVector bodyPlain;
    if (sessionKey != nullptr) {
        bodyPlain.insert(bodyPlain.end(), sessionKey->begin(), sessionKey->end());
    }
    bodyPlain.insert(bodyPlain.end(), payload.begin(), payload.end());
    const uint8_t crc = onesComplementChecksum(command, bodyPlain);

    ByteVector packet;
    packet.reserve(1 + 2 + bodyPlain.size() + 2);
    packet.push_back(START_BYTE);
    packet.push_back(static_cast<uint8_t>(command[0]));
    packet.push_back(static_cast<uint8_t>(command[1]));
    if (encrypt) {
        ByteVector plain = bodyPlain;
        plain.push_back(crc);
        ByteVector cipher = rc4Transform(plain);
        packet.insert(packet.end(), cipher.begin(), cipher.end());
        packet.push_back(END_BYTE);
        return packet;
    }
    packet.insert(packet.end(), bodyPlain.begin(), bodyPlain.end());
    packet.push_back(crc);
    packet.push_back(END_BYTE);
    return packet;
}

ByteVector buildRegisterPayload(uint16_t registerId) {
    return ByteVector{
        static_cast<uint8_t>((registerId >> 8) & 0xFF),
        static_cast<uint8_t>(registerId & 0xFF),
    };
}

ByteVector buildWriteRegisterPayload(uint16_t registerId, int32_t value) {
    return ByteVector{
        static_cast<uint8_t>((registerId >> 8) & 0xFF),
        static_cast<uint8_t>(registerId & 0xFF),
        static_cast<uint8_t>((value >> 24) & 0xFF),
        static_cast<uint8_t>((value >> 16) & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>(value & 0xFF),
    };
}

ByteVector buildHeMakeCoffeePayload(const ModelInfo& modelInfo, uint8_t recipeSelector) {
    ByteVector payload(18, 0x00);
    payload[1] = modelInfo.brewCommandMode;
    payload[3] = recipeSelector;
    payload[5] = 0x01;
    return payload;
}

ByteVector buildHeCommandPayload(uint16_t commandId) {
    ByteVector payload(18, 0x00);
    payload[0] = static_cast<uint8_t>((commandId >> 8) & 0xFF);
    payload[1] = static_cast<uint8_t>(commandId & 0xFF);
    return payload;
}

ByteVector buildHdPayload(uint16_t registerId) {
    return buildRegisterPayload(registerId);
}

bool buildHuRequest(ByteVector& seedOut, ByteVector& packetOut) {
    seedOut.resize(4);
    esp_fill_random(seedOut.data(), seedOut.size());
    ByteVector payload = seedOut;
    ByteVector verifier = deriveHuVerifier(seedOut, 0, seedOut.size());
    payload.insert(payload.end(), verifier.begin(), verifier.end());
    packetOut = buildPacket(CMD_HU, payload, nullptr, true);
    return true;
}

bool decodePacketAssumed(const ByteVector& packet,
                         const char* expectedCommand,
                         const ByteVector* expectedSessionKey,
                         bool encrypted,
                         ByteVector& payloadOut,
                         String& error) {
    payloadOut.clear();
    if (packet.size() < 5) {
        error = "packet too short";
        return false;
    }
    if (packet.front() != START_BYTE || packet.back() != END_BYTE) {
        error = "invalid frame markers";
        return false;
    }
    if (packet[1] != static_cast<uint8_t>(expectedCommand[0]) || packet[2] != static_cast<uint8_t>(expectedCommand[1])) {
        error = "unexpected command";
        return false;
    }

    ByteVector bodyPlain;
    uint8_t crc = 0;
    if (encrypted) {
        ByteVector encoded(packet.begin() + 3, packet.end() - 1);
        ByteVector decoded = rc4Transform(encoded);
        if (decoded.empty()) {
            error = "decoded payload empty";
            return false;
        }
        crc = decoded.back();
        bodyPlain.assign(decoded.begin(), decoded.end() - 1);
    } else {
        if (packet.size() < 6) {
            error = "plain packet too short";
            return false;
        }
        crc = packet[packet.size() - 2];
        bodyPlain.assign(packet.begin() + 3, packet.end() - 2);
    }

    if (crc != onesComplementChecksum(expectedCommand, bodyPlain)) {
        error = "checksum mismatch";
        return false;
    }

    if (expectedSessionKey != nullptr) {
        if (bodyPlain.size() < expectedSessionKey->size()) {
            error = "body shorter than session key";
            return false;
        }
        if (!std::equal(expectedSessionKey->begin(), expectedSessionKey->end(), bodyPlain.begin())) {
            error = "session key echo mismatch";
            return false;
        }
        payloadOut.assign(bodyPlain.begin() + expectedSessionKey->size(), bodyPlain.end());
        return true;
    }

    payloadOut = bodyPlain;
    return true;
}

bool parseHuResponsePayload(const ByteVector& payload, const ByteVector& seed, ByteVector& sessionKeyOut, String& error) {
    sessionKeyOut.clear();
    if (payload.size() != 8) {
        error = String("HU response payload must be 8 bytes, got ") + payload.size();
        return false;
    }
    ByteVector echoedSeed(payload.begin(), payload.begin() + 4);
    if (echoedSeed != seed) {
        error = "Random key mismatch";
        return false;
    }
    sessionKeyOut.assign(payload.begin() + 4, payload.begin() + 6);
    ByteVector verifier(payload.begin() + 6, payload.end());
    ByteVector expectedVerifier = deriveHuVerifier(payload, 0, 6);
    if (verifier != expectedVerifier) {
        error = "Session key verification failed";
        sessionKeyOut.clear();
        return false;
    }
    return true;
}

bool parseSessionHexString(const String& sessionHex, ByteVector& sessionKeyOut, String& error) {
    sessionKeyOut.clear();
    if (sessionHex.isEmpty()) {
        return true;
    }
    if (!hexDecode(sessionHex, sessionKeyOut, error)) {
        return false;
    }
    if (sessionKeyOut.size() != 2) {
        error = "sessionHex must decode to exactly 2 bytes";
        return false;
    }
    return true;
}

bool parseCommandString(const String& commandText, char commandOut[3], String& error) {
    if (commandText.length() != 2) {
        error = "command must be exactly 2 ASCII characters";
        return false;
    }
    commandOut[0] = commandText[0];
    commandOut[1] = commandText[1];
    commandOut[2] = '\0';
    return true;
}

const char* settingsFamilyKey(SettingsFamily family) {
    switch (family) {
        case SettingsFamily::Family600:
            return "600";
        case SettingsFamily::Family700:
            return "700";
        case SettingsFamily::Family8000:
            return "8000";
        case SettingsFamily::Family900:
            return "900";
        case SettingsFamily::Family900Light:
            return "900-light";
        case SettingsFamily::Family1030:
            return "1030";
        case SettingsFamily::Family1040:
            return "1040";
        default:
            return "unknown";
    }
}

SettingsFamily parseSettingsFamilyOverride(const String& familyText) {
    const String family = upperCopy(familyText);
    if (family.isEmpty() || family == "AUTO") {
        return SettingsFamily::Unknown;
    }
    if (family == "600") {
        return SettingsFamily::Family600;
    }
    if (family == "700" || family == "79X") {
        return SettingsFamily::Family700;
    }
    if (family == "8000" || family == "8101" || family == "8103" || family == "8107") {
        return SettingsFamily::Family8000;
    }
    if (family == "900") {
        return SettingsFamily::Family900;
    }
    if (family == "900-LIGHT" || family == "900LIGHT") {
        return SettingsFamily::Family900Light;
    }
    if (family == "1030") {
        return SettingsFamily::Family1030;
    }
    if (family == "1040") {
        return SettingsFamily::Family1040;
    }
    return SettingsFamily::Unknown;
}

ModelInfo detectModelInfo(const DeviceDetails& details) {
    ModelInfo info;
    const String serial = upperCopy(details.serial);
    const String model = upperCopy(details.model);
    const String ad06 = upperCopy(details.ad06Ascii);

    if (serial.length() >= 4) {
        const String prefix4 = serial.substring(0, 4);
        for (const auto& rule : MODEL_RULES) {
            if (strlen(rule.token) == 4 && prefix4 == rule.token) {
                applyModelRule(info, rule);
                return info;
            }
        }
    }
    if (serial.length() >= 3) {
        const String prefix3 = serial.substring(0, 3);
        for (const auto& rule : MODEL_RULES) {
            if (strlen(rule.token) == 3 && prefix3 == rule.token) {
                applyModelRule(info, rule);
                return info;
            }
        }
    }

    for (const auto& rule : MODEL_RULES) {
        if (containsModelToken(model, rule.token) || containsModelToken(serial, rule.token) || containsModelToken(ad06, rule.token)) {
            applyModelRule(info, rule);
            return info;
        }
    }

    info.modelName = details.model;
    return info;
}

bool resolveSettingsProbeContext(const DeviceDetails& details,
                                 SettingsFamily familyOverride,
                                 SettingsProbeContext& contextOut,
                                 String& error) {
    contextOut = SettingsProbeContext{};
    const ModelInfo modelInfo = detectModelInfo(details);
    contextOut.modelCodeHint = modelInfo.modelCode;
    contextOut.is79xModel = modelInfo.is79xModel;
    contextOut.hasAromaBalanceProfile = modelInfo.hasAromaBalanceProfile;

    if (familyOverride != SettingsFamily::Unknown) {
        contextOut.family = familyOverride;
        contextOut.familyKey = settingsFamilyKey(familyOverride);
        contextOut.familySource = "override";
        if ((familyOverride == SettingsFamily::Family600 || familyOverride == SettingsFamily::Family700) &&
            contextOut.modelCodeHint.isEmpty()) {
            contextOut.hasAromaBalanceProfile = true;
        }
        return true;
    }

    if (modelInfo.settingsFamily == SettingsFamily::Unknown) {
        error = String("unable to detect settings family from details (model=\"") + details.model +
                "\", serial=\"" + details.serial + "\", ad06=\"" + details.ad06Ascii +
                "\"); choose a family override";
        return false;
    }

    contextOut.family = modelInfo.settingsFamily;
    contextOut.familyKey = settingsFamilyKey(modelInfo.settingsFamily);
    contextOut.familySource = "model";
    return true;
}

void selectSettingsDescriptors(const SettingsProbeContext& context,
                               std::vector<const SettingProbeDescriptor*>& selectedOut) {
    selectedOut.clear();
    switch (context.family) {
        case SettingsFamily::Family600:
        case SettingsFamily::Family700:
            selectedOut.push_back(&SETTINGS_600_700_BASE_PROBES[0]);
            selectedOut.push_back(&SETTINGS_600_700_BASE_PROBES[1]);
            if (!context.is79xModel) {
                selectedOut.push_back(&SETTINGS_600_700_BASE_PROBES[2]);
            }
            selectedOut.push_back(&SETTINGS_600_700_BASE_PROBES[3]);
            if (context.hasAromaBalanceProfile) {
                selectedOut.push_back(&SETTINGS_600_700_BASE_PROBES[4]);
            }
            break;
        case SettingsFamily::Family8000:
            for (const auto& probe : SETTINGS_8000_PROBES) {
                selectedOut.push_back(&probe);
            }
            break;
        case SettingsFamily::Family900:
            for (const auto& probe : SETTINGS_900_PROBES) {
                selectedOut.push_back(&probe);
            }
            break;
        case SettingsFamily::Family900Light:
            for (const auto& probe : SETTINGS_900_LIGHT_PROBES) {
                selectedOut.push_back(&probe);
            }
            break;
        case SettingsFamily::Family1030:
            for (const auto& probe : SETTINGS_1030_PROBES) {
                selectedOut.push_back(&probe);
            }
            break;
        case SettingsFamily::Family1040:
            for (const auto& probe : SETTINGS_1040_PROBES) {
                selectedOut.push_back(&probe);
            }
            break;
        default:
            break;
    }
}

const SettingProbeDescriptor* findSettingDescriptorByKey(const SettingsProbeContext& context, const String& key) {
    std::vector<const SettingProbeDescriptor*> probes;
    selectSettingsDescriptors(context, probes);
    for (const auto* probe : probes) {
        if (key == probe->name) {
            return probe;
        }
    }
    return nullptr;
}

const SettingProbeDescriptor* findSettingDescriptorById(const SettingsProbeContext& context, uint16_t id) {
    std::vector<const SettingProbeDescriptor*> probes;
    selectSettingsDescriptors(context, probes);
    for (const auto* probe : probes) {
        if (probe->id == id) {
            return probe;
        }
    }
    return nullptr;
}

const char* findSettingValueLabel(const SettingProbeDescriptor& probe, uint16_t code) {
    for (size_t i = 0; i < probe.optionCount; ++i) {
        if (probe.options[i].code == code) {
            return probe.options[i].label;
        }
    }
    return nullptr;
}

bool findSettingValueCode(const SettingProbeDescriptor& probe, const String& value, uint16_t& codeOut) {
    const String normalized = upperCopy(value);
    for (size_t i = 0; i < probe.optionCount; ++i) {
        if (normalized == upperCopy(probe.options[i].label)) {
            codeOut = probe.options[i].code;
            return true;
        }
    }
    return false;
}

bool decodeHrNumericResponse(const std::vector<ByteVector>& chunks,
                             bool encrypted,
                             uint16_t& registerIdOut,
                             int32_t& valueOut,
                             String& error) {
    registerIdOut = 0;
    valueOut = 0;
    ByteVector joined = joinChunks(chunks);
    if (joined.empty()) {
        error = "no notifications";
        return false;
    }
    ByteVector decodedPayload;
    if (!decodePacketAssumed(joined, "HR", nullptr, encrypted, decodedPayload, error)) {
        return false;
    }
    if (decodedPayload.size() != 6) {
        error = String("unexpected HR payload size ") + decodedPayload.size();
        return false;
    }
    registerIdOut = static_cast<uint16_t>((decodedPayload[0] << 8) | decodedPayload[1]);
    valueOut = (static_cast<int32_t>(decodedPayload[2]) << 24) |
               (static_cast<int32_t>(decodedPayload[3]) << 16) |
               (static_cast<int32_t>(decodedPayload[4]) << 8) |
               static_cast<int32_t>(decodedPayload[5]);
    return true;
}

void appendDecodedSettingResult(JsonObject result,
                                const SettingProbeDescriptor& probe,
                                const std::vector<ByteVector>& chunks,
                                bool encrypted) {
    uint16_t echoedRegisterId = 0;
    int32_t rawValue = 0;
    String valueError;
    if (!decodeHrNumericResponse(chunks, encrypted, echoedRegisterId, rawValue, valueError)) {
        result["valueParseStatus"] = valueError;
        return;
    }

    result["valueParseStatus"] = "decoded";
    result["echoedRegisterId"] = echoedRegisterId;
    result["rawValue"] = rawValue;
    const uint16_t code = static_cast<uint16_t>(rawValue & 0xFFFF);
    result["valueCodeHex"] = formatCodeHex(code);
    if (echoedRegisterId != probe.id) {
        result["valueParseStatus"] = String("register echo mismatch: expected ") + probe.id + ", got " + echoedRegisterId;
        return;
    }
    const char* label = findSettingValueLabel(probe, code);
    result["valueLabel"] = label != nullptr ? label : "";
}

bool decodeHxResponse(const std::vector<ByteVector>& chunks, bool encrypted, ProcessStatus& statusOut, String& error) {
    statusOut = ProcessStatus{};
    ByteVector joined = joinChunks(chunks);
    if (joined.empty()) {
        error = "no notifications";
        return false;
    }
    ByteVector decodedPayload;
    if (!decodePacketAssumed(joined, "HX", nullptr, encrypted, decodedPayload, error)) {
        return false;
    }
    if (decodedPayload.size() != 8) {
        error = String("unexpected HX payload size ") + decodedPayload.size();
        return false;
    }
    auto readInt16 = [&](size_t offset) -> int16_t {
        return static_cast<int16_t>((decodedPayload[offset] << 8) | decodedPayload[offset + 1]);
    };
    statusOut.ok = true;
    statusOut.process = readInt16(0);
    statusOut.subProcess = readInt16(2);
    statusOut.message = readInt16(4);
    statusOut.progress = readInt16(6);
    if (statusOut.process == 0 && statusOut.subProcess == 0 && statusOut.message == 0 && statusOut.progress == 0) {
        statusOut.summary = "idle";
    } else if (statusOut.progress > 0) {
        statusOut.summary = "working";
    } else {
        statusOut.summary = "active";
    }
    return true;
}

void selectStandardRecipes(const ModelInfo& modelInfo,
                           std::vector<const StandardRecipeDescriptor*>& selectedOut) {
    selectedOut.clear();
    const StandardRecipeDescriptor* table = nullptr;
    size_t count = 0;
    if (modelInfo.familyKey == "600") {
        table = RECIPES_600;
        count = arraySize(RECIPES_600);
    } else if (modelInfo.familyKey == "700") {
        table = RECIPES_700;
        count = arraySize(RECIPES_700);
    } else if (modelInfo.familyKey == "79x") {
        table = RECIPES_79X;
        count = arraySize(RECIPES_79X);
    } else if (modelInfo.familyKey == "900" || modelInfo.familyKey == "900-light") {
        table = RECIPES_900;
        count = arraySize(RECIPES_900);
    } else if (modelInfo.familyKey == "1030") {
        table = RECIPES_1030;
        count = arraySize(RECIPES_1030);
    } else if (modelInfo.familyKey == "1040") {
        table = RECIPES_1040;
        count = arraySize(RECIPES_1040);
    } else if (modelInfo.familyKey == "8000") {
        table = RECIPES_8000;
        count = arraySize(RECIPES_8000);
    }
    for (size_t i = 0; i < count; ++i) {
        selectedOut.push_back(&table[i]);
    }
}

bool resolveMyCoffeeLayout(const ModelInfo& modelInfo, MyCoffeeLayout& layoutOut) {
    layoutOut = MyCoffeeLayout{};
    layoutOut.familyKey = modelInfo.familyKey;
    layoutOut.textEncoding = modelInfo.recipeTextEncoding;
    layoutOut.slotCount = modelInfo.myCoffeeSlotCount;
    layoutOut.fluidWriteScale10 = modelInfo.fluidWriteScale10;

    if (modelInfo.familyKey == "600") {
        layoutOut.enabledOffset = 0;
        layoutOut.iconOffset = 1;
        layoutOut.nameOffset = 2;
        layoutOut.typeOffset = 3;
        layoutOut.strengthOffset = 4;
        layoutOut.profileOffset = 5;
        layoutOut.temperatureOffset = 6;
        layoutOut.twoCupsOffset = 7;
        layoutOut.coffeeAmountOffset = 8;
        layoutOut.waterAmountOffset = 9;
        layoutOut.milkFoamAmountOffset = 11;
        layoutOut.preparationOffset = 12;
        return true;
    }
    if (modelInfo.familyKey == "700" || modelInfo.familyKey == "79x") {
        layoutOut.enabledOffset = 0;
        layoutOut.iconOffset = 1;
        layoutOut.nameOffset = 2;
        layoutOut.typeOffset = 3;
        layoutOut.strengthOffset = 4;
        layoutOut.profileOffset = 5;
        layoutOut.temperatureOffset = 6;
        layoutOut.twoCupsOffset = 7;
        layoutOut.coffeeAmountOffset = 8;
        layoutOut.waterAmountOffset = 9;
        layoutOut.milkAmountOffset = 10;
        layoutOut.milkFoamAmountOffset = 11;
        return true;
    }
    if (modelInfo.familyKey == "900" || modelInfo.familyKey == "900-light") {
        layoutOut.enabledOffset = 0;
        layoutOut.iconOffset = 1;
        layoutOut.nameOffset = 2;
        layoutOut.typeOffset = 3;
        layoutOut.strengthOffset = 4;
        layoutOut.profileOffset = 5;
        layoutOut.preparationOffset = 6;
        layoutOut.twoCupsOffset = 7;
        layoutOut.coffeeTemperatureOffset = 8;
        layoutOut.waterTemperatureOffset = 9;
        layoutOut.milkTemperatureOffset = 10;
        layoutOut.milkFoamTemperatureOffset = 11;
        layoutOut.coffeeAmountOffset = 12;
        layoutOut.waterAmountOffset = 13;
        layoutOut.milkAmountOffset = 14;
        layoutOut.milkFoamAmountOffset = 15;
        layoutOut.overallTemperatureOffset = 16;
        return true;
    }
    if (modelInfo.familyKey == "1030") {
        layoutOut.enabledOffset = 0;
        layoutOut.iconOffset = 1;
        layoutOut.nameOffset = 2;
        layoutOut.typeOffset = 3;
        layoutOut.strengthOffset = 4;
        layoutOut.profileOffset = 5;
        layoutOut.preparationOffset = 6;
        layoutOut.twoCupsOffset = 7;
        layoutOut.coffeeTemperatureOffset = 8;
        layoutOut.waterTemperatureOffset = 9;
        layoutOut.milkTemperatureOffset = 10;
        layoutOut.milkFoamTemperatureOffset = 11;
        layoutOut.coffeeAmountOffset = 12;
        layoutOut.waterAmountOffset = 13;
        layoutOut.milkAmountOffset = 14;
        layoutOut.milkFoamAmountOffset = 15;
        return true;
    }
    if (modelInfo.familyKey == "8000") {
        layoutOut.enabledOffset = 0;
        layoutOut.nameOffset = 2;
        layoutOut.iconOffset = 3;
        layoutOut.typeOffset = 3;
        layoutOut.strengthOffset = 4;
        layoutOut.profileOffset = 5;
        layoutOut.temperatureOffset = 6;
        layoutOut.twoCupsOffset = 7;
        layoutOut.coffeeAmountOffset = 8;
        layoutOut.waterAmountOffset = 9;
        layoutOut.milkAmountOffset = 10;
        layoutOut.milkFoamAmountOffset = 11;
        return true;
    }
    return false;
}

uint16_t myCoffeeSlotBase(uint8_t slotIndex) {
    return static_cast<uint16_t>(20000 + (slotIndex * 100));
}

bool decodeHaResponse(const std::vector<ByteVector>& chunks,
                      bool encrypted,
                      uint16_t expectedRegisterId,
                      RecipeTextEncoding encoding,
                      String& rawNameOut,
                      String& displayNameOut,
                      String& error) {
    rawNameOut = "";
    displayNameOut = "";
    ByteVector joined = joinChunks(chunks);
    if (joined.empty()) {
        error = "no notifications";
        return false;
    }
    ByteVector decodedPayload;
    if (!decodePacketAssumed(joined, "HA", nullptr, encrypted, decodedPayload, error)) {
        return false;
    }
    if (decodedPayload.size() != 66) {
        error = String("unexpected HA payload size ") + decodedPayload.size();
        return false;
    }
    const uint16_t registerId = static_cast<uint16_t>((decodedPayload[0] << 8) | decodedPayload[1]);
    if (registerId != expectedRegisterId) {
        error = String("register echo mismatch: expected ") + expectedRegisterId + ", got " + registerId;
        return false;
    }
    const uint8_t* field = decodedPayload.data() + 2;
    if (encoding == RecipeTextEncoding::Utf16Le) {
        rawNameOut = decodeUtf16Le(field, 64);
        displayNameOut = rawNameOut;
    } else {
        rawNameOut = decodeLegacyRaw(field, 64);
        displayNameOut = sanitizeLegacyDisplay(rawNameOut);
    }
    return true;
}

ByteVector encodeHbNamePayload(uint16_t registerId, RecipeTextEncoding encoding, const String& name) {
    ByteVector payload = buildRegisterPayload(registerId);
    ByteVector field = encoding == RecipeTextEncoding::Utf16Le ? encodeUtf16LeName(name) : encodeLegacyName(name);
    payload.insert(payload.end(), field.begin(), field.end());
    return payload;
}

} // namespace nivona
