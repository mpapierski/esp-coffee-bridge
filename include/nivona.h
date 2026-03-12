#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_system.h>

#include <cstdint>
#include <vector>

namespace nivona {

using ByteVector = std::vector<uint8_t>;

inline constexpr char DEVICE_INFORMATION_SERVICE[] = "0000180A-0000-1000-8000-00805F9B34FB";
inline constexpr char DIS_MANUFACTURER[]           = "00002A29-0000-1000-8000-00805F9B34FB";
inline constexpr char DIS_MODEL[]                  = "00002A24-0000-1000-8000-00805F9B34FB";
inline constexpr char DIS_SERIAL[]                 = "00002A25-0000-1000-8000-00805F9B34FB";
inline constexpr char DIS_HARDWARE[]               = "00002A27-0000-1000-8000-00805F9B34FB";
inline constexpr char DIS_FIRMWARE[]               = "00002A26-0000-1000-8000-00805F9B34FB";
inline constexpr char DIS_SOFTWARE[]               = "00002A28-0000-1000-8000-00805F9B34FB";

inline constexpr char NIVONA_SERVICE[] = "0000AD00-B35C-11E4-9813-0002A5D5C51B";
inline constexpr char NIVONA_CTRL[]    = "0000AD01-B35C-11E4-9813-0002A5D5C51B";
inline constexpr char NIVONA_RX[]      = "0000AD02-B35C-11E4-9813-0002A5D5C51B";
inline constexpr char NIVONA_TX[]      = "0000AD03-B35C-11E4-9813-0002A5D5C51B";
inline constexpr char NIVONA_AUX1[]    = "0000AD04-B35C-11E4-9813-0002A5D5C51B";
inline constexpr char NIVONA_AUX2[]    = "0000AD05-B35C-11E4-9813-0002A5D5C51B";
inline constexpr char NIVONA_NAME[]    = "0000AD06-B35C-11E4-9813-0002A5D5C51B";

inline constexpr char CMD_HU[] = "HU";
inline constexpr uint16_t HE_FACTORY_RESET_SETTINGS = 0x0032;
inline constexpr uint16_t HE_FACTORY_RESET_RECIPES  = 0x0033;

struct RegisterProbe {
    const char* name;
    uint16_t id;
    const char* title;
    const char* section;
    const char* unit;
};

extern const RegisterProbe STATS_8000_PROBES[];
extern const size_t STATS_8000_PROBE_COUNT;

enum class SettingsFamily : uint8_t {
    Unknown,
    Family600,
    Family700,
    Family8000,
    Family900,
    Family900Light,
    Family1030,
    Family1040,
};

enum class RecipeTextEncoding : uint8_t {
    LegacySingleByte,
    Utf16Le,
};

struct SettingValueOption {
    uint16_t code;
    const char* label;
};

struct SettingProbeDescriptor {
    const char* name;
    const char* title;
    uint16_t id;
    const SettingValueOption* options;
    size_t optionCount;
};

struct SettingsProbeContext {
    SettingsFamily family{SettingsFamily::Unknown};
    String familyKey;
    String familySource;
    String modelCodeHint;
    bool is79xModel{false};
    bool hasAromaBalanceProfile{false};
};

struct DeviceDetails {
    bool fetched{false};
    String manufacturer;
    String model;
    String serial;
    String hardwareRevision;
    String firmwareRevision;
    String softwareRevision;
    String ad06Hex;
    String ad06Ascii;
    String lastError;
};

struct ModelInfo {
    String appModel;
    String modelCode;
    String modelName;
    String familyKey;
    SettingsFamily settingsFamily{SettingsFamily::Unknown};
    uint8_t myCoffeeSlotCount{0};
    RecipeTextEncoding recipeTextEncoding{RecipeTextEncoding::LegacySingleByte};
    bool is79xModel{false};
    bool hasAromaBalanceProfile{false};
    uint8_t strengthLevelCount{5};
    uint8_t maxProfileCode{3};
    bool fluidWriteScale10{false};
    bool supportsSettings{false};
    bool supportsStats{false};
    bool supportsSavedRecipes{false};
    bool supportsRecipeWrites{false};
    uint8_t brewCommandMode{0x0B};
};

struct StandardRecipeDescriptor {
    uint8_t selector{0};
    const char* name{""};
    const char* title{""};
};

struct StandardRecipeLayout {
    String familyKey;
    bool fluidWriteScale10{false};
    uint16_t strengthOffset{UINT16_MAX};
    uint16_t profileOffset{UINT16_MAX};
    uint16_t temperatureOffset{UINT16_MAX};
    uint16_t preparationOffset{UINT16_MAX};
    uint16_t twoCupsOffset{UINT16_MAX};
    uint16_t coffeeAmountOffset{UINT16_MAX};
    uint16_t waterAmountOffset{UINT16_MAX};
    uint16_t milkAmountOffset{UINT16_MAX};
    uint16_t milkFoamAmountOffset{UINT16_MAX};
    uint16_t coffeeTemperatureOffset{UINT16_MAX};
    uint16_t waterTemperatureOffset{UINT16_MAX};
    uint16_t milkTemperatureOffset{UINT16_MAX};
    uint16_t milkFoamTemperatureOffset{UINT16_MAX};
    uint16_t overallTemperatureOffset{UINT16_MAX};
};

struct MyCoffeeLayout {
    String familyKey;
    RecipeTextEncoding textEncoding{RecipeTextEncoding::LegacySingleByte};
    uint8_t slotCount{0};
    bool fluidWriteScale10{false};
    uint16_t enabledOffset{UINT16_MAX};
    uint16_t iconOffset{UINT16_MAX};
    uint16_t nameOffset{UINT16_MAX};
    uint16_t typeOffset{UINT16_MAX};
    uint16_t strengthOffset{UINT16_MAX};
    uint16_t profileOffset{UINT16_MAX};
    uint16_t preparationOffset{UINT16_MAX};
    uint16_t twoCupsOffset{UINT16_MAX};
    uint16_t temperatureOffset{UINT16_MAX};
    uint16_t coffeeTemperatureOffset{UINT16_MAX};
    uint16_t waterTemperatureOffset{UINT16_MAX};
    uint16_t milkTemperatureOffset{UINT16_MAX};
    uint16_t milkFoamTemperatureOffset{UINT16_MAX};
    uint16_t overallTemperatureOffset{UINT16_MAX};
    uint16_t coffeeAmountOffset{UINT16_MAX};
    uint16_t waterAmountOffset{UINT16_MAX};
    uint16_t milkAmountOffset{UINT16_MAX};
    uint16_t milkFoamAmountOffset{UINT16_MAX};
};

struct ProcessStatus {
    bool ok{false};
    int16_t process{0};
    int16_t subProcess{0};
    int16_t message{0};
    int16_t progress{0};
    bool hostConfirmSuggested{false};
    String summary;
    String processLabel;
    String subProcessLabel;
    String messageLabel;
};

template <typename T, size_t N>
constexpr size_t arraySize(const T (&)[N]) {
    return N;
}

String hexEncode(const uint8_t* data, size_t length);
String hexEncode(const ByteVector& data);
bool hexDecode(const String& text, ByteVector& out, String& error);
String printableAscii(const ByteVector& bytes);
String upperCopy(const String& value);
String formatCodeHex(uint16_t code);
bool looksLikeNivonaName(const String& name);

ByteVector rc4Transform(const ByteVector& data);
uint8_t onesComplementChecksum(const char* command, const ByteVector& body);
ByteVector deriveHuVerifier(const ByteVector& buffer, size_t start, size_t count);
ByteVector buildPacket(const char* command, const ByteVector& payload, const ByteVector* sessionKey, bool encrypt);
ByteVector buildRegisterPayload(uint16_t registerId);
ByteVector buildWriteRegisterPayload(uint16_t registerId, int32_t value);
ByteVector buildHeMakeCoffeePayload(const ModelInfo& modelInfo, uint8_t recipeSelector);
ByteVector buildHeCommandPayload(uint16_t commandId);
ByteVector buildHyConfirmPayload();
ByteVector buildHdPayload(uint16_t registerId);
bool buildHuRequest(ByteVector& seedOut, ByteVector& packetOut);
bool decodePacketAssumed(const ByteVector& packet,
                         const char* expectedCommand,
                         const ByteVector* expectedSessionKey,
                         bool encrypted,
                         ByteVector& payloadOut,
                         String& error);
bool parseHuResponsePayload(const ByteVector& payload, const ByteVector& seed, ByteVector& sessionKeyOut, String& error);
bool parseSessionHexString(const String& sessionHex, ByteVector& sessionKeyOut, String& error);
bool parseCommandString(const String& commandText, char commandOut[3], String& error);

const char* settingsFamilyKey(SettingsFamily family);
SettingsFamily parseSettingsFamilyOverride(const String& familyText);
ModelInfo detectModelInfo(const DeviceDetails& details);
bool resolveSettingsProbeContext(const DeviceDetails& details,
                                 SettingsFamily familyOverride,
                                 SettingsProbeContext& contextOut,
                                 String& error);
void selectSettingsDescriptors(const SettingsProbeContext& context,
                               std::vector<const SettingProbeDescriptor*>& selectedOut);
const SettingProbeDescriptor* findSettingDescriptorByKey(const SettingsProbeContext& context, const String& key);
const SettingProbeDescriptor* findSettingDescriptorById(const SettingsProbeContext& context, uint16_t id);
const char* findSettingValueLabel(const SettingProbeDescriptor& probe, uint16_t code);
bool findSettingValueCode(const SettingProbeDescriptor& probe, const String& value, uint16_t& codeOut);
void appendDecodedSettingResult(JsonObject result,
                                const SettingProbeDescriptor& probe,
                                const std::vector<ByteVector>& chunks,
                                bool encrypted);

bool decodeHrNumericResponse(const std::vector<ByteVector>& chunks,
                             bool encrypted,
                             uint16_t& registerIdOut,
                             int32_t& valueOut,
                             String& error);
const char* describeProcessCode(int16_t process);
const char* describeSubProcessCode(int16_t process, int16_t subProcess);
const char* describeMessageCode(int16_t message);
void annotateProcessStatus(ProcessStatus& status);
bool decodeHxResponse(const std::vector<ByteVector>& chunks, bool encrypted, ProcessStatus& statusOut, String& error);

void selectStandardRecipes(const ModelInfo& modelInfo,
                           std::vector<const StandardRecipeDescriptor*>& selectedOut);
const StandardRecipeDescriptor* findStandardRecipeBySelector(const ModelInfo& modelInfo, uint8_t selector);
bool resolveStandardRecipeLayout(const ModelInfo& modelInfo, StandardRecipeLayout& layoutOut);
bool resolveStandardRecipeBaseRegister(const ModelInfo& modelInfo, uint8_t selector, uint16_t& baseRegisterOut);
void selectStatsDescriptors(const ModelInfo& modelInfo,
                            std::vector<const RegisterProbe*>& selectedOut);
bool resolveMyCoffeeLayout(const ModelInfo& modelInfo, MyCoffeeLayout& layoutOut);
uint16_t myCoffeeSlotBase(uint8_t slotIndex);
bool decodeHaResponse(const std::vector<ByteVector>& chunks,
                      bool encrypted,
                      uint16_t expectedRegisterId,
                      RecipeTextEncoding encoding,
                      String& rawNameOut,
                      String& displayNameOut,
                      String& error);
ByteVector encodeHbNamePayload(uint16_t registerId, RecipeTextEncoding encoding, const String& name);

} // namespace nivona
