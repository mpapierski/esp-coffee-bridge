#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include <NimBLEDevice.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "nivona.h"
#include "web_ui.h"

namespace {

using namespace nivona;

constexpr char APP_HOSTNAME[]   = "esp-coffee-bridge";
constexpr char AP_SSID[]        = "esp-coffee-maker";
constexpr char AP_PASSWORD[]    = "coffee-setup";
constexpr char APP_BUILD_TIME[] = __DATE__ " " __TIME__;
constexpr uint32_t SCAN_MS      = 5000;
constexpr uint32_t HTTP_TIMEOUT = 10000;
constexpr uint32_t DEFAULT_RECONNECT_DELAY_MS = 750;
constexpr uint32_t NOTIFICATION_BATCH_SETTLE_MS = 60;
constexpr size_t LOG_CAPACITY   = 128;
constexpr uint16_t TEMP_RECIPE_TYPE_REGISTER = 9001;
constexpr char STANDARD_RECIPE_CACHE_PREFIX[] = "/stdrec-";
constexpr uint32_t STANDARD_RECIPE_CACHE_SCHEMA = 2;
constexpr char SAVED_RECIPE_CACHE_PREFIX[] = "/mycoffee-";
constexpr uint32_t SAVED_RECIPE_CACHE_SCHEMA = 2;

void rxNotifyCallback(NimBLERemoteCharacteristic* characteristic, uint8_t* data, size_t length, bool isNotify);

constexpr char PREFS_WIFI[] = "wifi";
constexpr char PREFS_SSID[] = "ssid";
constexpr char PREFS_PASS[] = "pass";
constexpr char PREFS_MACHINES[] = "machines";
constexpr char PREFS_MACHINE_STORE[] = "store";
constexpr char PREFS_MACHINE_SCHEMA[] = "schema";

struct ScanRecord {
    String address;
    uint8_t addressType{BLE_ADDR_PUBLIC};
    String name;
    int rssi{0};
    bool connectable{false};
    bool advertisedSupportedService{false};
    bool likelySupported{false};
    uint32_t seenAtMs{0};
};

struct LogEntry {
    uint32_t timestampMs{0};
    String source;
    String message;
};

struct SavedMachine {
    String serial;
    String alias;
    String address;
    uint8_t addressType{BLE_ADDR_PUBLIC};
    String manufacturer;
    String model;
    String modelCode;
    String modelName;
    String familyKey;
    String hardwareRevision;
    String firmwareRevision;
    String softwareRevision;
    String ad06Hex;
    String ad06Ascii;
    int lastSeenRssi{0};
    uint32_t lastSeenAtMs{0};
    uint32_t savedAtMs{0};
};

struct ProtocolSessionEntry {
    String serial;
    String address;
    uint8_t addressType{BLE_ADDR_PUBLIC};
    ByteVector sessionKey;
    String source;
    uint32_t setAtMs{0};
};

WebServer server(80);
Preferences preferences;
Preferences machinePreferences;

SemaphoreHandle_t logMutex          = nullptr;
SemaphoreHandle_t notifyDataMutex   = nullptr;
SemaphoreHandle_t notificationLatch = nullptr;
SemaphoreHandle_t scanDataMutex     = nullptr;

std::vector<ScanRecord> scannedDevices;
std::vector<ScanRecord> scanScratchDevices;
std::vector<LogEntry> logs;
std::vector<SavedMachine> savedMachines;

ByteVector lastNotificationBytes;
std::vector<ByteVector> notificationHistory;
std::vector<uint32_t> notificationHistoryMs;
String selectedAddress;
uint8_t selectedAddressType = BLE_ADDR_PUBLIC;
String selectedMachineSerial;
String wifiStaSsid;
String pairingStatus        = "idle";
String lastError;
String lastHuSeedHex;
String lastHuRequestHex;
String lastHuResponseHex;
String lastHuParseStatus;
std::vector<ProtocolSessionEntry> protocolSessions;
bool notificationsEnabled = false;
String notificationMode = "notify";
int lastScanReason        = 0;
size_t lastScanResultCount = 0;
uint32_t lastScanAtMs     = 0;
DeviceDetails cachedDetails;
uint32_t lastIdleScanAtMs = 0;
constexpr uint32_t IDLE_SCAN_INTERVAL_MS = 30000;
constexpr uint32_t ACTIVE_SCAN_SUPPRESS_MS = 15000;
uint32_t scanSuppressedUntilMs = 0;
bool idleScanInProgress = false;
bool blockingScanInProgress = false;
uint32_t idleScanStartedAtMs = 0;
bool littleFsReady = false;

NimBLEClient* client                              = nullptr;
NimBLERemoteService* disService                   = nullptr;
NimBLERemoteService* nivonaService                = nullptr;
NimBLERemoteCharacteristic* nivonaCtrl            = nullptr;
NimBLERemoteCharacteristic* nivonaRx              = nullptr;
NimBLERemoteCharacteristic* nivonaTx              = nullptr;
NimBLERemoteCharacteristic* nivonaAux1            = nullptr;
NimBLERemoteCharacteristic* nivonaAux2            = nullptr;
NimBLERemoteCharacteristic* nivonaName            = nullptr;

extern NimBLEScanCallbacks* scanCallbacksPtr;
void refreshAllSavedMachinePresence();

ScanRecord recordFromAdvertisedDevice(const NimBLEAdvertisedDevice* device) {
    ScanRecord record;
    record.address                 = String(device->getAddress().toString().c_str());
    record.addressType             = device->getAddressType();
    record.name                    = device->haveName() ? String(device->getName().c_str()) : String("");
    record.rssi                    = device->getRSSI();
    record.connectable             = device->isConnectable();
    record.advertisedSupportedService = device->isAdvertisingService(NimBLEUUID(NIVONA_SERVICE));
    record.likelySupported            = device->isAdvertisingService(NimBLEUUID(NIVONA_SERVICE)) ||
                            (device->haveName() && nivona::looksLikeNivonaName(String(device->getName().c_str())));
    record.seenAtMs                = millis();
    return record;
}

void upsertScanRecord(std::vector<ScanRecord>& records, const ScanRecord& incoming) {
    for (auto& record : records) {
        if (record.address.equalsIgnoreCase(incoming.address)) {
            record = incoming;
            return;
        }
    }
    records.push_back(incoming);
}

void addLog(const String& source, const String& message) {
    Serial.printf("[%lu] %s: %s\n", millis(), source.c_str(), message.c_str());
    if (logMutex == nullptr) {
        return;
    }
    if (xSemaphoreTake(logMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    logs.push_back({millis(), source, message});
    if (logs.size() > LOG_CAPACITY) {
        logs.erase(logs.begin(), logs.begin() + (logs.size() - LOG_CAPACITY));
    }
    xSemaphoreGive(logMutex);
}

void addHexLog(const String& source, const uint8_t* data, size_t length) {
    addLog(source, hexEncode(data, length));
}

void clearRemoteHandles() {
    disService    = nullptr;
    nivonaService = nullptr;
    nivonaCtrl    = nullptr;
    nivonaRx      = nullptr;
    nivonaTx      = nullptr;
    nivonaAux1    = nullptr;
    nivonaAux2    = nullptr;
    nivonaName    = nullptr;
    notificationsEnabled = false;
}

String normalizeNotificationMode(const String& mode) {
    if (mode.equalsIgnoreCase("indicate")) {
        return "indicate";
    }
    return "notify";
}

bool notificationModeUsesNotify(const String& mode) {
    return !mode.equalsIgnoreCase("indicate");
}

void copyNotificationHistory(std::vector<ByteVector>& chunksOut, std::vector<uint32_t>& timesOut);

void clearNotificationBuffer() {
    if (notifyDataMutex != nullptr && xSemaphoreTake(notifyDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        lastNotificationBytes.clear();
        notificationHistory.clear();
        notificationHistoryMs.clear();
        xSemaphoreGive(notifyDataMutex);
    }
    if (notificationLatch != nullptr) {
        xSemaphoreTake(notificationLatch, 0);
    }
}

bool waitForNotification(uint32_t timeoutMs, ByteVector& out) {
    if (notificationLatch == nullptr || notifyDataMutex == nullptr) {
        return false;
    }
    if (xSemaphoreTake(notificationLatch, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        return false;
    }
    if (xSemaphoreTake(notifyDataMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    out = lastNotificationBytes;
    xSemaphoreGive(notifyDataMutex);
    return true;
}

bool waitForNotificationBatch(uint32_t timeoutMs,
                              std::vector<ByteVector>& chunksOut,
                              std::vector<uint32_t>& timesOut) {
    chunksOut.clear();
    timesOut.clear();
    if (timeoutMs == 0) {
        copyNotificationHistory(chunksOut, timesOut);
        return !chunksOut.empty();
    }
    if (notificationLatch == nullptr || notifyDataMutex == nullptr) {
        return false;
    }

    ByteVector firstChunk;
    if (!waitForNotification(timeoutMs, firstChunk)) {
        return false;
    }

    const uint32_t startedAt = millis();
    uint32_t lastNotificationAt = millis();
    while ((millis() - startedAt) < timeoutMs) {
        const uint32_t elapsed = millis() - startedAt;
        const uint32_t remaining = timeoutMs > elapsed ? timeoutMs - elapsed : 0;
        if (remaining == 0) {
            break;
        }
        const uint32_t waitSlice = remaining < NOTIFICATION_BATCH_SETTLE_MS ? remaining : NOTIFICATION_BATCH_SETTLE_MS;
        if (xSemaphoreTake(notificationLatch, pdMS_TO_TICKS(waitSlice)) == pdTRUE) {
            lastNotificationAt = millis();
            continue;
        }
        if ((millis() - lastNotificationAt) >= NOTIFICATION_BATCH_SETTLE_MS) {
            break;
        }
    }

    copyNotificationHistory(chunksOut, timesOut);
    return !chunksOut.empty();
}

void copyNotificationHistory(std::vector<ByteVector>& chunksOut, std::vector<uint32_t>& timesOut) {
    chunksOut.clear();
    timesOut.clear();
    if (notifyDataMutex == nullptr) {
        return;
    }
    if (xSemaphoreTake(notifyDataMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    chunksOut = notificationHistory;
    timesOut  = notificationHistoryMs;
    xSemaphoreGive(notifyDataMutex);
}

SavedMachine* findSavedMachineBySerial(const String& serial);

ProtocolSessionEntry* findProtocolSessionBySerial(const String& serial) {
    if (serial.isEmpty()) {
        return nullptr;
    }
    for (auto& session : protocolSessions) {
        if (session.serial.equalsIgnoreCase(serial)) {
            return &session;
        }
    }
    return nullptr;
}

ProtocolSessionEntry* findProtocolSessionByAddress(const String& address) {
    if (address.isEmpty()) {
        return nullptr;
    }
    for (auto& session : protocolSessions) {
        if (session.address.equalsIgnoreCase(address)) {
            return &session;
        }
    }
    return nullptr;
}

void selectAddressTarget(const String& address, uint8_t addressType) {
    selectedMachineSerial = "";
    selectedAddress = address;
    selectedAddressType = addressType;
}

void selectMachineTarget(const SavedMachine& machine) {
    selectedMachineSerial = machine.serial;
    selectedAddress = machine.address;
    selectedAddressType = machine.addressType;
}

void clearStoredSessionKey(const String& serial = "", const String& address = "") {
    String targetSerial = serial;
    String targetAddress = address;
    if (targetSerial.isEmpty() && targetAddress.isEmpty()) {
        targetSerial = selectedMachineSerial;
        targetAddress = selectedAddress;
    }
    if (targetAddress.isEmpty() && !targetSerial.isEmpty()) {
        SavedMachine* machine = findSavedMachineBySerial(targetSerial);
        if (machine != nullptr) {
            targetAddress = machine->address;
        }
    }

    if (targetSerial.isEmpty() && targetAddress.isEmpty()) {
        protocolSessions.clear();
        return;
    }

    protocolSessions.erase(
        std::remove_if(protocolSessions.begin(),
                       protocolSessions.end(),
                       [&](const ProtocolSessionEntry& session) {
                           if (!targetSerial.isEmpty() && session.serial.equalsIgnoreCase(targetSerial)) {
                               return true;
                           }
                           return !targetAddress.isEmpty() && session.address.equalsIgnoreCase(targetAddress);
                       }),
        protocolSessions.end());
}

void setStoredSessionKey(const ByteVector& sessionKey,
                         const String& source,
                         const String& serial = "",
                         const String& address = "",
                         uint8_t addressType = BLE_ADDR_PUBLIC) {
    String targetSerial = serial;
    String targetAddress = address;
    uint8_t targetAddressType = addressType;

    if (targetSerial.isEmpty()) {
        targetSerial = selectedMachineSerial;
    }
    if (targetAddress.isEmpty()) {
        targetAddress = selectedAddress;
        targetAddressType = selectedAddressType;
    }

    ProtocolSessionEntry* session = !targetSerial.isEmpty() ? findProtocolSessionBySerial(targetSerial) : nullptr;
    if (session == nullptr && !targetAddress.isEmpty()) {
        session = findProtocolSessionByAddress(targetAddress);
    }
    if (session == nullptr) {
        protocolSessions.push_back({});
        session = &protocolSessions.back();
    }

    session->serial = targetSerial;
    session->address = targetAddress;
    session->addressType = targetAddressType;
    session->sessionKey = sessionKey;
    session->source = source;
    session->setAtMs = millis();
}

ScanRecord* findScannedDevice(const String& address) {
    for (auto& record : scannedDevices) {
        if (record.address.equalsIgnoreCase(address)) {
            return &record;
        }
    }
    return nullptr;
}

void sendJson(const JsonDocument& doc, int code = 200) {
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    String payload;
    serializeJson(doc, payload);
    server.send(code, "application/json", payload);
}

void sendError(int code, const String& error) {
    DynamicJsonDocument doc(1024);
    doc["ok"]    = false;
    doc["error"] = error;
    sendJson(doc, code);
}

bool parseJsonBody(DynamicJsonDocument& doc, String& error) {
    if (!server.hasArg("plain")) {
        error = "missing request body";
        return false;
    }
    DeserializationError desErr = deserializeJson(doc, server.arg("plain"));
    if (desErr) {
        error = String("invalid json: ") + desErr.c_str();
        return false;
    }
    return true;
}

String loadPrefString(const char* key) {
    return preferences.getString(key, "");
}

nivona::DeviceDetails toNivonaDetails(const SavedMachine& machine);
void appendStandardRecipeDiscovery(JsonObject target,
                                   const nivona::ModelInfo& modelInfo,
                                   const nivona::StandardRecipeLayout& layout);

String cacheSafeToken(const String& value) {
    String out;
    out.reserve(value.length());
    for (size_t i = 0; i < value.length(); ++i) {
        const char ch = value.charAt(i);
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
            out += ch;
        } else {
            out += '_';
        }
    }
    return out;
}

String standardRecipeCachePrefix(const String& serial) {
    return String(STANDARD_RECIPE_CACHE_PREFIX) + cacheSafeToken(serial) + "-";
}

String standardRecipeCachePath(const String& serial, uint8_t selector) {
    return standardRecipeCachePrefix(serial) + selector + ".json";
}

String savedRecipeCachePath(const String& serial) {
    return String(SAVED_RECIPE_CACHE_PREFIX) + cacheSafeToken(serial) + ".json";
}

bool parseRefreshArg() {
    if (!server.hasArg("refresh")) {
        return false;
    }
    const String rawValue = server.arg("refresh");
    return !(rawValue.isEmpty() || rawValue == "0" || rawValue.equalsIgnoreCase("false") || rawValue.equalsIgnoreCase("no"));
}

bool initializeLittleFs(String& error) {
    if (littleFsReady) {
        return true;
    }
    if (LittleFS.begin(true)) {
        littleFsReady = true;
        return true;
    }
    error = "failed to mount LittleFS";
    return false;
}

void clearStandardRecipeCachesByPrefix(const String& prefix) {
    if (!littleFsReady) {
        return;
    }
    File root = LittleFS.open("/");
    if (!root) {
        return;
    }
    File entry = root.openNextFile();
    while (entry) {
        const String path = entry.name();
        entry.close();
        if (path.startsWith(prefix)) {
            LittleFS.remove(path);
        }
        entry = root.openNextFile();
    }
}

void clearStandardRecipeCachesForMachine(const String& serial) {
    if (serial.isEmpty()) {
        return;
    }
    clearStandardRecipeCachesByPrefix(standardRecipeCachePrefix(serial));
}

void clearAllStandardRecipeCaches() {
    clearStandardRecipeCachesByPrefix(String(STANDARD_RECIPE_CACHE_PREFIX));
}

void clearSavedRecipeCachesForMachine(const String& serial) {
    if (!littleFsReady || serial.isEmpty()) {
        return;
    }
    LittleFS.remove(savedRecipeCachePath(serial));
}

void clearAllSavedRecipeCaches() {
    if (!littleFsReady) {
        return;
    }
    clearStandardRecipeCachesByPrefix(String(SAVED_RECIPE_CACHE_PREFIX));
}

bool loadStandardRecipeCache(const SavedMachine& machine,
                             uint8_t selector,
                             DynamicJsonDocument& cacheDoc,
                             String& error) {
    if (!littleFsReady) {
        error = "standard recipe cache is unavailable";
        return false;
    }
    File cacheFile = LittleFS.open(standardRecipeCachePath(machine.serial, selector), "r");
    if (!cacheFile) {
        error = "standard recipe cache miss";
        return false;
    }

    DeserializationError parseError = deserializeJson(cacheDoc, cacheFile);
    cacheFile.close();
    if (parseError) {
        LittleFS.remove(standardRecipeCachePath(machine.serial, selector));
        error = String("failed to parse standard recipe cache: ") + parseError.c_str();
        return false;
    }

    if ((cacheDoc["schema"] | 0U) != STANDARD_RECIPE_CACHE_SCHEMA) {
        LittleFS.remove(standardRecipeCachePath(machine.serial, selector));
        error = "standard recipe cache schema mismatch";
        return false;
    }
    const String cachedSerial = cacheDoc["serial"] | "";
    const int cachedSelector = cacheDoc["selector"] | -1;
    JsonObject cachedRecipe = cacheDoc["recipe"].as<JsonObject>();
    if (!cachedSerial.equalsIgnoreCase(machine.serial) || cachedSelector != selector || cachedRecipe.isNull()) {
        LittleFS.remove(standardRecipeCachePath(machine.serial, selector));
        error = "standard recipe cache contents mismatch";
        return false;
    }
    const nivona::ModelInfo modelInfo = nivona::detectModelInfo(toNivonaDetails(machine));
    cachedRecipe["maxStrengthBeans"] = modelInfo.strengthLevelCount;
    cachedRecipe["maxProfileCode"] = modelInfo.maxProfileCode;
    nivona::StandardRecipeLayout layout;
    if (nivona::resolveStandardRecipeLayout(modelInfo, layout)) {
        appendStandardRecipeDiscovery(cachedRecipe, modelInfo, layout);
    }
    return true;
}

bool persistStandardRecipeCache(const SavedMachine& machine,
                                uint8_t selector,
                                JsonObjectConst recipe,
                                String& error) {
    if (!littleFsReady) {
        error = "standard recipe cache is unavailable";
        return false;
    }

    DynamicJsonDocument cacheDoc(16384);
    cacheDoc["schema"] = STANDARD_RECIPE_CACHE_SCHEMA;
    cacheDoc["serial"] = machine.serial;
    cacheDoc["selector"] = selector;
    cacheDoc["familyKey"] = machine.familyKey;
    JsonObject storedRecipe = cacheDoc.createNestedObject("recipe");
    storedRecipe.set(recipe);

    File cacheFile = LittleFS.open(standardRecipeCachePath(machine.serial, selector), "w");
    if (!cacheFile) {
        error = "failed to open standard recipe cache for writing";
        return false;
    }
    if (serializeJson(cacheDoc, cacheFile) == 0) {
        cacheFile.close();
        error = "failed to write standard recipe cache";
        return false;
    }
    cacheFile.close();
    return true;
}

bool loadSavedRecipeCache(const SavedMachine& machine,
                          DynamicJsonDocument& cacheDoc,
                          String& error) {
    if (!littleFsReady) {
        error = "saved recipe cache is unavailable";
        return false;
    }

    File cacheFile = LittleFS.open(savedRecipeCachePath(machine.serial), "r");
    if (!cacheFile) {
        error = "saved recipe cache miss";
        return false;
    }

    DeserializationError parseError = deserializeJson(cacheDoc, cacheFile);
    cacheFile.close();
    if (parseError) {
        LittleFS.remove(savedRecipeCachePath(machine.serial));
        error = String("failed to parse saved recipe cache: ") + parseError.c_str();
        return false;
    }

    if ((cacheDoc["schema"] | 0U) != SAVED_RECIPE_CACHE_SCHEMA) {
        LittleFS.remove(savedRecipeCachePath(machine.serial));
        error = "saved recipe cache schema mismatch";
        return false;
    }
    const String cachedSerial = cacheDoc["serial"] | "";
    JsonArray cachedRecipes = cacheDoc["recipes"].as<JsonArray>();
    if (!cachedSerial.equalsIgnoreCase(machine.serial) || cachedRecipes.isNull()) {
        LittleFS.remove(savedRecipeCachePath(machine.serial));
        error = "saved recipe cache contents mismatch";
        return false;
    }
    return true;
}

bool writeSavedRecipeCacheDoc(const SavedMachine& machine,
                              DynamicJsonDocument& cacheDoc,
                              String& error) {
    if (!littleFsReady) {
        error = "saved recipe cache is unavailable";
        return false;
    }

    File cacheFile = LittleFS.open(savedRecipeCachePath(machine.serial), "w");
    if (!cacheFile) {
        error = "failed to open saved recipe cache for writing";
        return false;
    }
    if (serializeJson(cacheDoc, cacheFile) == 0) {
        cacheFile.close();
        error = "failed to write saved recipe cache";
        return false;
    }
    cacheFile.close();
    return true;
}

bool persistSavedRecipeCache(const SavedMachine& machine,
                             JsonArrayConst recipes,
                             String& error) {
    if (!littleFsReady) {
        error = "saved recipe cache is unavailable";
        return false;
    }

    DynamicJsonDocument cacheDoc(65536);
    cacheDoc["schema"] = SAVED_RECIPE_CACHE_SCHEMA;
    cacheDoc["serial"] = machine.serial;
    cacheDoc["familyKey"] = machine.familyKey;
    JsonArray storedRecipes = cacheDoc.createNestedArray("recipes");
    storedRecipes.set(recipes);
    return writeSavedRecipeCacheDoc(machine, cacheDoc, error);
}

bool upsertSavedRecipeCacheEntry(const SavedMachine& machine,
                                 JsonObjectConst recipe,
                                 String& error) {
    const int slotNumber = recipe["slot"] | 0;
    if (slotNumber <= 0) {
        error = "saved recipe cache entry is missing a slot number";
        return false;
    }

    DynamicJsonDocument cacheDoc(65536);
    String loadError;
    if (!loadSavedRecipeCache(machine, cacheDoc, loadError)) {
        cacheDoc.clear();
        cacheDoc["schema"] = SAVED_RECIPE_CACHE_SCHEMA;
        cacheDoc["serial"] = machine.serial;
        cacheDoc["familyKey"] = machine.familyKey;
        cacheDoc.createNestedArray("recipes");
    }

    JsonArray recipes = cacheDoc["recipes"].as<JsonArray>();
    for (size_t index = 0; index < recipes.size(); ++index) {
        if ((recipes[index]["slot"] | 0) == slotNumber) {
            recipes.remove(index);
            break;
        }
    }

    JsonObject storedRecipe = recipes.createNestedObject();
    storedRecipe.set(recipe);
    return writeSavedRecipeCacheDoc(machine, cacheDoc, error);
}

nivona::DeviceDetails toNivonaDetails(const DeviceDetails& details) {
    nivona::DeviceDetails out;
    out.fetched = details.fetched;
    out.manufacturer = details.manufacturer;
    out.model = details.model;
    out.serial = details.serial;
    out.hardwareRevision = details.hardwareRevision;
    out.firmwareRevision = details.firmwareRevision;
    out.softwareRevision = details.softwareRevision;
    out.ad06Hex = details.ad06Hex;
    out.ad06Ascii = details.ad06Ascii;
    out.lastError = details.lastError;
    return out;
}

nivona::DeviceDetails toNivonaDetails(const SavedMachine& machine) {
    nivona::DeviceDetails out;
    out.fetched = true;
    out.manufacturer = machine.manufacturer;
    out.model = machine.model;
    out.serial = machine.serial;
    out.hardwareRevision = machine.hardwareRevision;
    out.firmwareRevision = machine.firmwareRevision;
    out.softwareRevision = machine.softwareRevision;
    out.ad06Hex = machine.ad06Hex;
    out.ad06Ascii = machine.ad06Ascii;
    return out;
}

void suppressIdleScans(uint32_t durationMs = ACTIVE_SCAN_SUPPRESS_MS) {
    scanSuppressedUntilMs = millis() + durationMs;
}

void clearScanScratch() {
    if (scanDataMutex != nullptr && xSemaphoreTake(scanDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        scanScratchDevices.clear();
        xSemaphoreGive(scanDataMutex);
    }
}

void copyScanScratchToPublished() {
    if (scanDataMutex != nullptr && xSemaphoreTake(scanDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        scannedDevices = scanScratchDevices;
        xSemaphoreGive(scanDataMutex);
    }
}

void configureBleScan(NimBLEScan& scan) {
    scan.stop();
    scan.clearResults();
    scan.setScanCallbacks(scanCallbacksPtr, false);
    scan.setActiveScan(true);
    scan.setInterval(100);
    scan.setWindow(100);
    scan.setDuplicateFilter(0);
    scan.setMaxResults(0);
}

void publishScanResults(uint32_t startedAtMs, bool updateIdleTimestamp, const String& label) {
    copyScanScratchToPublished();
    lastScanAtMs = millis();
    lastScanResultCount = scannedDevices.size();
    if (updateIdleTimestamp) {
        lastIdleScanAtMs = lastScanAtMs;
    }
    refreshAllSavedMachinePresence();
    addLog("scan",
           label + ", devices=" + scannedDevices.size() + ", elapsedMs=" + (millis() - startedAtMs) +
               ", reason=" + lastScanReason);
}

void cancelIdleScan() {
    if (!idleScanInProgress) {
        return;
    }
    idleScanInProgress = false;
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan != nullptr && scan->isScanning()) {
        scan->stop();
        delay(50);
    }
    clearScanScratch();
    addLog("scan", "Cancelled idle BLE scan");
}

bool startIdleScanAsync() {
    if (idleScanInProgress || blockingScanInProgress) {
        return false;
    }
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan == nullptr) {
        addLog("scan", "BLE scan object is unavailable");
        return false;
    }
    if (scan->isScanning()) {
        return false;
    }

    clearScanScratch();
    configureBleScan(*scan);

    addLog("scan", "Starting idle BLE scan");
    lastScanReason = 0;
    idleScanStartedAtMs = millis();
    idleScanInProgress = true;
    if (!scan->start(SCAN_MS, false, false)) {
        idleScanInProgress = false;
        lastScanReason = -1;
        lastScanAtMs = millis();
        addLog("scan", "Failed to start idle BLE scan");
        return false;
    }

    return true;
}

void finalizeIdleScanIfReady() {
    if (!idleScanInProgress) {
        return;
    }
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan != nullptr && scan->isScanning()) {
        return;
    }

    idleScanInProgress = false;
    publishScanResults(idleScanStartedAtMs, true, "Completed idle BLE scan");
}

SavedMachine* findSavedMachineBySerial(const String& serial) {
    for (auto& machine : savedMachines) {
        if (machine.serial.equalsIgnoreCase(serial)) {
            return &machine;
        }
    }
    return nullptr;
}

SavedMachine* findSavedMachineByAddress(const String& address) {
    if (address.isEmpty()) {
        return nullptr;
    }
    for (auto& machine : savedMachines) {
        if (machine.address.equalsIgnoreCase(address)) {
            return &machine;
        }
    }
    return nullptr;
}

void refreshSavedMachinePresence(SavedMachine& machine) {
    machine.lastSeenAtMs = 0;
    machine.lastSeenRssi = 0;
    for (const auto& record : scannedDevices) {
        if (record.address.equalsIgnoreCase(machine.address)) {
            machine.lastSeenAtMs = record.seenAtMs;
            machine.lastSeenRssi = record.rssi;
            machine.addressType = record.addressType;
            return;
        }
    }
}

void refreshAllSavedMachinePresence() {
    for (auto& machine : savedMachines) {
        refreshSavedMachinePresence(machine);
    }
}

void persistSavedMachines() {
    DynamicJsonDocument doc(2048 + (savedMachines.size() * 768));
    JsonArray items = doc.createNestedArray("machines");
    for (const auto& machine : savedMachines) {
        JsonObject item = items.createNestedObject();
        item["serial"] = machine.serial;
        item["alias"] = machine.alias;
        item["address"] = machine.address;
        item["addressType"] = machine.addressType;
        item["manufacturer"] = machine.manufacturer;
        item["model"] = machine.model;
        item["modelCode"] = machine.modelCode;
        item["modelName"] = machine.modelName;
        item["familyKey"] = machine.familyKey;
        item["hardwareRevision"] = machine.hardwareRevision;
        item["firmwareRevision"] = machine.firmwareRevision;
        item["softwareRevision"] = machine.softwareRevision;
        item["ad06Hex"] = machine.ad06Hex;
        item["ad06Ascii"] = machine.ad06Ascii;
        item["lastSeenRssi"] = machine.lastSeenRssi;
        item["lastSeenAtMs"] = machine.lastSeenAtMs;
        item["savedAtMs"] = machine.savedAtMs;
    }

    String payload;
    serializeJson(doc, payload);
    machinePreferences.putString(PREFS_MACHINE_STORE, payload);
    machinePreferences.putUInt(PREFS_MACHINE_SCHEMA, 1);
}

void loadSavedMachines() {
    savedMachines.clear();
    const String payload = machinePreferences.getString(PREFS_MACHINE_STORE, "");
    if (payload.isEmpty()) {
        return;
    }

    DynamicJsonDocument doc(24576);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        addLog("machines", String("Failed to parse saved machine store: ") + err.c_str());
        return;
    }

    JsonArray items = doc["machines"].as<JsonArray>();
    for (JsonObject item : items) {
        SavedMachine machine;
        machine.serial = item["serial"] | "";
        machine.alias = item["alias"] | "";
        machine.address = item["address"] | "";
        machine.addressType = item["addressType"] | BLE_ADDR_PUBLIC;
        machine.manufacturer = item["manufacturer"] | "";
        machine.model = item["model"] | "";
        machine.modelCode = item["modelCode"] | "";
        machine.modelName = item["modelName"] | "";
        machine.familyKey = item["familyKey"] | "";
        machine.hardwareRevision = item["hardwareRevision"] | "";
        machine.firmwareRevision = item["firmwareRevision"] | "";
        machine.softwareRevision = item["softwareRevision"] | "";
        machine.ad06Hex = item["ad06Hex"] | "";
        machine.ad06Ascii = item["ad06Ascii"] | "";
        machine.lastSeenRssi = item["lastSeenRssi"] | 0;
        machine.lastSeenAtMs = item["lastSeenAtMs"] | 0;
        machine.savedAtMs = item["savedAtMs"] | 0;
        if (!machine.serial.isEmpty()) {
            savedMachines.push_back(machine);
        }
    }
}

void populateSavedMachine(SavedMachine& machine,
                          const String& alias,
                          const String& address,
                          uint8_t addressType,
                          const DeviceDetails& details) {
    const nivona::ModelInfo modelInfo = nivona::detectModelInfo(toNivonaDetails(details));
    machine.serial = details.serial;
    machine.alias = alias;
    machine.address = address;
    machine.addressType = addressType;
    machine.manufacturer = details.manufacturer;
    machine.model = details.model;
    machine.modelCode = modelInfo.modelCode;
    machine.modelName = !modelInfo.modelName.isEmpty() ? modelInfo.modelName : details.model;
    machine.familyKey = modelInfo.familyKey;
    machine.hardwareRevision = details.hardwareRevision;
    machine.firmwareRevision = details.firmwareRevision;
    machine.softwareRevision = details.softwareRevision;
    machine.ad06Hex = details.ad06Hex;
    machine.ad06Ascii = details.ad06Ascii;
    machine.savedAtMs = millis();
}

bool selectSavedMachine(const SavedMachine& machine, String& error) {
    if (machine.address.isEmpty()) {
        error = "saved machine has no known BLE address";
        return false;
    }
    selectMachineTarget(machine);
    suppressIdleScans();
    return true;
}

const ProtocolSessionEntry* resolveStoredSessionEntry(const String& serial, const String& address) {
    String targetSerial = serial;
    String targetAddress = address;
    if (targetSerial.isEmpty() && targetAddress.isEmpty()) {
        targetSerial = selectedMachineSerial;
        targetAddress = selectedAddress;
    }

    if (!targetSerial.isEmpty()) {
        const ProtocolSessionEntry* bySerial = findProtocolSessionBySerial(targetSerial);
        if (bySerial != nullptr) {
            return bySerial;
        }
        SavedMachine* machine = findSavedMachineBySerial(targetSerial);
        if (machine != nullptr) {
            targetAddress = machine->address;
        }
    }

    if (!targetAddress.isEmpty()) {
        return findProtocolSessionByAddress(targetAddress);
    }
    return nullptr;
}

const ByteVector* resolveStoredSessionIfAvailable(const String& serial, const String& address) {
    const ProtocolSessionEntry* session = resolveStoredSessionEntry(serial, address);
    if (session != nullptr && session->sessionKey.size() == 2) {
        return &session->sessionKey;
    }
    return nullptr;
}

void appendProtocolSessionJson(JsonObject target, const ProtocolSessionEntry* session) {
    target["hasSession"] = session != nullptr && session->sessionKey.size() == 2;
    target["sessionHex"] = session != nullptr && session->sessionKey.size() == 2 ? hexEncode(session->sessionKey) : "";
    target["source"] = session != nullptr ? session->source : "";
    target["setAtMs"] = session != nullptr ? session->setAtMs : 0;
    target["serial"] = session != nullptr ? session->serial : "";
    target["address"] = session != nullptr ? session->address : "";
}

void syncProtocolSessionTarget(const SavedMachine& machine) {
    ProtocolSessionEntry* session = findProtocolSessionBySerial(machine.serial);
    if (session == nullptr) {
        session = findProtocolSessionByAddress(machine.address);
    }
    if (session == nullptr) {
        return;
    }
    session->serial = machine.serial;
    session->address = machine.address;
    session->addressType = machine.addressType;
}

void appendSavedMachineJson(JsonObject target, const SavedMachine& machine) {
    target["serial"] = machine.serial;
    target["alias"] = machine.alias;
    target["address"] = machine.address;
    target["addressType"] = machine.addressType;
    target["model"] = machine.model;
    target["modelCode"] = machine.modelCode;
    target["modelName"] = machine.modelName;
    target["familyKey"] = machine.familyKey;
    target["manufacturer"] = machine.manufacturer;
    target["hardwareRevision"] = machine.hardwareRevision;
    target["firmwareRevision"] = machine.firmwareRevision;
    target["softwareRevision"] = machine.softwareRevision;
    target["ad06Hex"] = machine.ad06Hex;
    target["ad06Ascii"] = machine.ad06Ascii;
    target["lastSeenRssi"] = machine.lastSeenRssi;
    target["lastSeenAtMs"] = machine.lastSeenAtMs;
    target["savedAtMs"] = machine.savedAtMs;
    target["online"] = machine.lastSeenAtMs > 0;
}

void appendProcessStatusJson(JsonObject target, const nivona::ProcessStatus& status, const String& error) {
    target["ok"] = status.ok;
    target["summary"] = status.summary;
    target["process"] = status.process;
    target["processLabel"] = status.processLabel;
    target["subProcess"] = status.subProcess;
    target["subProcessLabel"] = status.subProcessLabel;
    target["message"] = status.message;
    target["messageLabel"] = status.messageLabel;
    target["progress"] = status.progress;
    target["hostConfirmSuggested"] = status.hostConfirmSuggested;
    target["error"] = error;
}

String recipeTypeTitle(const nivona::ModelInfo& modelInfo, int32_t selector) {
    std::vector<const nivona::StandardRecipeDescriptor*> recipes;
    nivona::selectStandardRecipes(modelInfo, recipes);
    for (const auto* recipe : recipes) {
        if (recipe->selector == selector) {
            return recipe->title;
        }
    }
    return selector >= 0 ? String("Recipe ") + selector : "";
}

String recipeTemperatureLabel(int32_t rawValue) {
    switch (rawValue & 0xFFFF) {
        case 0:
            return "normal";
        case 1:
            return "high";
        case 2:
            return "max";
        case 3:
            return "individual";
        default:
            return "";
    }
}

String recipeProfileLabel(int32_t rawValue) {
    switch (rawValue & 0xFFFF) {
        case 0:
            return "dynamic";
        case 1:
            return "constant";
        case 2:
            return "intense";
        case 3:
            return "individual";
        case 4:
            return "quick";
        default:
            return "";
    }
}

bool parseRecipeTemperatureCode(const JsonVariantConst value, int32_t& codeOut) {
    if (value.is<int>() || value.is<long>() || value.is<float>() || value.is<double>()) {
        codeOut = value.as<int32_t>();
        return true;
    }
    if (value.is<bool>()) {
        codeOut = value.as<bool>() ? 1 : 0;
        return true;
    }
    String text = value.as<String>();
    text.trim();
    text.toLowerCase();
    if (text == "normal") {
        codeOut = 0;
        return true;
    }
    if (text == "high") {
        codeOut = 1;
        return true;
    }
    if (text == "max" || text == "hot") {
        codeOut = 2;
        return true;
    }
    if (text == "individual") {
        codeOut = 3;
        return true;
    }
    return false;
}

bool parseRecipeProfileCode(const JsonVariantConst value, int32_t& codeOut) {
    if (value.is<int>() || value.is<long>() || value.is<float>() || value.is<double>()) {
        codeOut = value.as<int32_t>();
        return true;
    }
    String text = value.as<String>();
    text.trim();
    text.toLowerCase();
    if (text == "dynamic") {
        codeOut = 0;
        return true;
    }
    if (text == "constant") {
        codeOut = 1;
        return true;
    }
    if (text == "intense") {
        codeOut = 2;
        return true;
    }
    if (text == "individual") {
        codeOut = 3;
        return true;
    }
    if (text == "quick") {
        codeOut = 4;
        return true;
    }
    return false;
}

bool parseRecipeBooleanCode(const JsonVariantConst value, int32_t& codeOut) {
    if (value.is<bool>()) {
        codeOut = value.as<bool>() ? 1 : 0;
        return true;
    }
    if (value.is<int>() || value.is<long>() || value.is<float>() || value.is<double>()) {
        codeOut = value.as<int32_t>();
        return true;
    }
    String text = value.as<String>();
    text.trim();
    text.toLowerCase();
    if (text == "on" || text == "true" || text == "yes") {
        codeOut = 1;
        return true;
    }
    if (text == "off" || text == "false" || text == "no") {
        codeOut = 0;
        return true;
    }
    return false;
}

float recipeAmountMlValue(bool fluidWriteScale10, int32_t rawValue) {
    if (fluidWriteScale10) {
        return static_cast<float>(rawValue) / 10.0f;
    }
    return static_cast<float>(rawValue);
}

float recipeAmountMlValue(const nivona::MyCoffeeLayout& layout, int32_t rawValue) {
    return recipeAmountMlValue(layout.fluidWriteScale10, rawValue);
}

String recipeStrengthBeansLabel(int32_t beans) {
    return beans == 1 ? "1 bean" : String(beans) + " beans";
}

void appendRecipeOption(JsonArray options, int32_t value, const String& label, const String& name = "") {
    JsonObject option = options.createNestedObject();
    option["value"] = value;
    option["label"] = label;
    if (!name.isEmpty()) {
        option["name"] = name;
    }
}

void appendRecipeWritableField(JsonArray fields, const char* fieldName) {
    fields.add(fieldName);
}

void appendStandardRecipeDiscovery(JsonObject target,
                                   const nivona::ModelInfo& modelInfo,
                                   const nivona::StandardRecipeLayout& layout) {
    JsonArray writableFields = target["writableFields"].is<JsonArray>()
        ? target["writableFields"].as<JsonArray>()
        : target.createNestedArray("writableFields");
    writableFields.clear();

    JsonObject options = target["options"].is<JsonObject>()
        ? target["options"].as<JsonObject>()
        : target.createNestedObject("options");
    options.clear();

    if (layout.strengthOffset != UINT16_MAX) {
        appendRecipeWritableField(writableFields, "strength");
        appendRecipeWritableField(writableFields, "strengthBeans");

        JsonArray strengthOptions = options.createNestedArray("strength");
        JsonArray strengthBeanOptions = options.createNestedArray("strengthBeans");
        const uint8_t maxStrengthBeans = modelInfo.strengthLevelCount > 0 ? modelInfo.strengthLevelCount : 5;
        for (uint8_t beans = 1; beans <= maxStrengthBeans; ++beans) {
            const String label = recipeStrengthBeansLabel(beans);
            appendRecipeOption(strengthOptions, static_cast<int32_t>(beans - 1), label);
            appendRecipeOption(strengthBeanOptions, beans, label);
        }
    }

    if (layout.profileOffset != UINT16_MAX) {
        appendRecipeWritableField(writableFields, "aroma");

        JsonArray aromaOptions = options.createNestedArray("aroma");
        const uint8_t maxProfileCode = modelInfo.maxProfileCode <= 4 ? modelInfo.maxProfileCode : 4;
        for (uint8_t code = 0; code <= maxProfileCode; ++code) {
            const String label = recipeProfileLabel(code);
            appendRecipeOption(aromaOptions, code, label, label);
        }
    }

    auto appendTemperatureDiscovery = [&](const char* fieldName) {
        appendRecipeWritableField(writableFields, fieldName);
        JsonArray tempOptions = options.createNestedArray(fieldName);
        for (int32_t code = 0; code <= 3; ++code) {
            const String label = recipeTemperatureLabel(code);
            appendRecipeOption(tempOptions, code, label, label);
        }
    };

    if (layout.temperatureOffset != UINT16_MAX) {
        appendTemperatureDiscovery("temperature");
    }
    if (layout.coffeeTemperatureOffset != UINT16_MAX) {
        appendTemperatureDiscovery("coffeeTemperature");
    }
    if (layout.waterTemperatureOffset != UINT16_MAX) {
        appendTemperatureDiscovery("waterTemperature");
    }
    if (layout.milkTemperatureOffset != UINT16_MAX) {
        appendTemperatureDiscovery("milkTemperature");
    }
    if (layout.milkFoamTemperatureOffset != UINT16_MAX) {
        appendTemperatureDiscovery("milkFoamTemperature");
    }
    if (layout.overallTemperatureOffset != UINT16_MAX) {
        appendTemperatureDiscovery("overallTemperature");
    }

    if (layout.twoCupsOffset != UINT16_MAX) {
        appendRecipeWritableField(writableFields, "twoCups");
        JsonArray twoCupOptions = options.createNestedArray("twoCups");
        appendRecipeOption(twoCupOptions, 0, "off", "off");
        appendRecipeOption(twoCupOptions, 1, "on", "on");
    }

    if (layout.preparationOffset != UINT16_MAX) {
        appendRecipeWritableField(writableFields, "preparation");
    }
    if (layout.coffeeAmountOffset != UINT16_MAX) {
        appendRecipeWritableField(writableFields, "coffeeAmountMl");
        appendRecipeWritableField(writableFields, "sizeMl");
    }
    if (layout.waterAmountOffset != UINT16_MAX) {
        appendRecipeWritableField(writableFields, "waterAmountMl");
        if (layout.coffeeAmountOffset == UINT16_MAX) {
            appendRecipeWritableField(writableFields, "sizeMl");
        }
    }
    if (layout.milkAmountOffset != UINT16_MAX) {
        appendRecipeWritableField(writableFields, "milkAmountMl");
    }
    if (layout.milkFoamAmountOffset != UINT16_MAX) {
        appendRecipeWritableField(writableFields, "milkFoamAmountMl");
    }
}

void connectWifi() {
    wifiStaSsid = loadPrefString(PREFS_SSID);
    const String wifiPass = loadPrefString(PREFS_PASS);

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD);

    if (wifiStaSsid.isEmpty()) {
        addLog("wifi", "No saved STA credentials, AP mode only");
        return;
    }

    WiFi.begin(wifiStaSsid.c_str(), wifiPass.c_str());
    const uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 15000) {
        delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
        addLog("wifi", String("Connected to ") + wifiStaSsid + " as " + WiFi.localIP().toString());
    } else {
        addLog("wifi", String("Failed to connect to ") + wifiStaSsid + ", AP remains active");
    }
}

bool resolveSessionKeyForRequest(const String& sessionHex,
                                 bool useStoredSession,
                                 const String& serial,
                                 const String& address,
                                 ByteVector& sessionKeyOut,
                                 const ByteVector*& sessionKeyPtrOut,
                                 String& sessionHexUsedOut,
                                 String& error) {
    sessionKeyOut.clear();
    sessionKeyPtrOut = nullptr;
    sessionHexUsedOut = "";

    if (!sessionHex.isEmpty()) {
        if (!parseSessionHexString(sessionHex, sessionKeyOut, error)) {
            return false;
        }
        sessionKeyPtrOut = &sessionKeyOut;
        sessionHexUsedOut = hexEncode(sessionKeyOut);
        return true;
    }

    if (!useStoredSession) {
        return true;
    }

    const ByteVector* storedSession = resolveStoredSessionIfAvailable(serial, address);
    if (storedSession == nullptr) {
        error = "protocol session key is not available for this target";
        return false;
    }

    sessionKeyPtrOut = storedSession;
    sessionHexUsedOut = hexEncode(*storedSession);
    return true;
}

bool connectToSelectedDevice(String& error);
bool disconnectFromDevice(String& error);
bool pairWithDevice(String& error, bool reconnectAfterPair = false, uint32_t reconnectDelayMs = DEFAULT_RECONNECT_DELAY_MS);
bool fetchDeviceDetails(String& error);
bool setNotificationsEnabled(bool enable, String& error, const String& mode = "notify");
bool writeRemoteCharacteristic(NimBLERemoteCharacteristic* characteristic,
                               const String& label,
                               const ByteVector& payload,
                               bool response,
                               bool chunked,
                               size_t chunkSize,
                               uint32_t interChunkDelayMs,
                               String& error);
void appendNotificationChunks(JsonArray chunksArray,
                              const std::vector<ByteVector>& chunks,
                              const std::vector<uint32_t>& times,
                              uint32_t baseMs);
void appendDecodeAttempt(JsonObject target,
                         const char* expectedCommand,
                         bool encrypted,
                         const std::vector<ByteVector>& chunks,
                         const ByteVector* expectedSessionKey);
bool reconnectToSelectedDevice(uint32_t delayMs, String& error);
bool readCharacteristicValue(NimBLERemoteCharacteristic* characteristic, ByteVector& out, String& error);
void appendCharacteristicInfo(JsonObject target, NimBLERemoteCharacteristic* characteristic);
bool sendPreparedFramePacket(const char* command,
                             const ByteVector& payload,
                             const ByteVector* sessionKey,
                             bool encrypt,
                             bool chunked,
                             uint32_t interChunkDelayMs,
                             uint32_t waitMs,
                             bool& writeWithResponseOut,
                             bool& canWriteOut,
                             bool& canWriteNoResponseOut,
                             ByteVector& requestPacketOut,
                             std::vector<ByteVector>& chunksOut,
                             std::vector<uint32_t>& timesOut,
                             String& error);
const ByteVector* resolveStoredSessionIfAvailable(const String& serial = "", const String& address = "");
NimBLERemoteService* resolveRemoteServiceByUuid(const String& serviceUuid, String& error);
NimBLERemoteCharacteristic* resolveRemoteCharacteristicByUuid(const String& serviceUuid,
                                                              const String& characteristicUuid,
                                                              NimBLERemoteService** serviceOut,
                                                              String& error);
void appendServiceInfo(JsonObject target, NimBLERemoteService* service, bool includeDescriptors);
void appendServiceList(JsonArray servicesArray, bool includeDescriptors);
void genericNotifyCallback(NimBLERemoteCharacteristic* characteristic, uint8_t* data, size_t length, bool isNotify);
bool runWorkerSessionProbe(uint32_t waitMs,
                           bool pairFirst,
                           bool reconnectAfterPair,
                           uint32_t reconnectDelayMs,
                           const String& notificationModeForRequest,
                           bool continueOnHuFailure,
                           DynamicJsonDocument& response,
                           String& error);
bool runSettingsFlowProbe(uint32_t waitMs,
                          bool pairFirst,
                          bool reconnectAfterPair,
                          uint32_t reconnectDelayMs,
                          const String& notificationModeForRequest,
                          bool encrypt,
                          SettingsFamily familyOverride,
                          DynamicJsonDocument& response,
                          String& error);

bool sendFramePacket(const char* command,
                     const ByteVector& payload,
                     const ByteVector* sessionKey,
                     bool encrypt,
                     bool chunked,
                     uint32_t interChunkDelayMs,
                     uint32_t waitMs,
                     const String& notificationModeForRequest,
                     bool& writeWithResponseOut,
                     bool& canWriteOut,
                     bool& canWriteNoResponseOut,
                     ByteVector& requestPacketOut,
                     std::vector<ByteVector>& chunksOut,
                     std::vector<uint32_t>& timesOut,
                     String& error) {
    if (!connectToSelectedDevice(error)) {
        return false;
    }
    if (!setNotificationsEnabled(true, error, notificationModeForRequest)) {
        return false;
    }
    return sendPreparedFramePacket(command,
                                   payload,
                                   sessionKey,
                                   encrypt,
                                   chunked,
                                   interChunkDelayMs,
                                   waitMs,
                                   writeWithResponseOut,
                                   canWriteOut,
                                   canWriteNoResponseOut,
                                   requestPacketOut,
                                   chunksOut,
                                   timesOut,
                                   error);
}

bool sendPreparedFramePacket(const char* command,
                             const ByteVector& payload,
                             const ByteVector* sessionKey,
                             bool encrypt,
                             bool chunked,
                             uint32_t interChunkDelayMs,
                             uint32_t waitMs,
                             bool& writeWithResponseOut,
                             bool& canWriteOut,
                             bool& canWriteNoResponseOut,
                             ByteVector& requestPacketOut,
                             std::vector<ByteVector>& chunksOut,
                             std::vector<uint32_t>& timesOut,
                             String& error) {
    requestPacketOut = buildPacket(command, payload, sessionKey, encrypt);
    chunksOut.clear();
    timesOut.clear();
    writeWithResponseOut = true;
    canWriteOut = false;
    canWriteNoResponseOut = false;

    if (client == nullptr || !client->isConnected()) {
        error = "not connected";
        return false;
    }
    if (!notificationsEnabled) {
        error = "rx notifications are not enabled";
        return false;
    }
    if (nivonaTx == nullptr) {
        error = "tx characteristic not available";
        return false;
    }

    canWriteOut = nivonaTx->canWrite();
    canWriteNoResponseOut = nivonaTx->canWriteNoResponse();
    writeWithResponseOut = !canWriteNoResponseOut;

    size_t maxWritePayload = 20;
    if (client != nullptr) {
        const uint16_t mtu = client->getMTU();
        if (mtu > 3) {
            maxWritePayload = mtu - 3;
        }
    }
    const bool effectiveChunked = chunked || requestPacketOut.size() > maxWritePayload;
    const size_t chunkSize = effectiveChunked ? maxWritePayload : 10;
    const uint32_t effectiveInterChunkDelayMs =
        effectiveChunked && interChunkDelayMs == 0 ? 10 : interChunkDelayMs;

    clearNotificationBuffer();
    if (!writeRemoteCharacteristic(nivonaTx,
                                   "tx",
                                   requestPacketOut,
                                   writeWithResponseOut,
                                   effectiveChunked,
                                   chunkSize,
                                   effectiveInterChunkDelayMs,
                                   error)) {
        return false;
    }

    if (waitMs > 0) {
        if (!waitForNotificationBatch(waitMs, chunksOut, timesOut)) {
            error = "timed out waiting for notification";
            return false;
        }
    } else {
        copyNotificationHistory(chunksOut, timesOut);
    }

    return true;
}

void appendFrameScenarioResult(JsonObject result,
                               const char* command,
                               const ByteVector& payload,
                               const ByteVector* sessionKey,
                               bool encrypt,
                               bool chunked,
                               uint32_t interChunkDelayMs,
                               uint32_t waitMs,
                               bool writeWithResponse,
                               bool canWrite,
                               bool canWriteNoResponse,
                               const ByteVector& requestPacket,
                               const std::vector<ByteVector>& chunks,
                               const std::vector<uint32_t>& times,
                               uint32_t startedAt) {
    result["ok"] = true;
    result["command"] = command;
    result["payloadHex"] = hexEncode(payload);
    result["requestHex"] = hexEncode(requestPacket);
    result["encrypt"] = encrypt;
    result["chunked"] = chunked;
    result["writeWithResponse"] = writeWithResponse;
    result["canWrite"] = canWrite;
    result["canWriteNoResponse"] = canWriteNoResponse;
    result["interChunkDelayMs"] = interChunkDelayMs;
    result["waitMs"] = waitMs;
    result["notifyCount"] = chunks.size();
    if (sessionKey != nullptr) {
        result["sessionHexUsed"] = hexEncode(*sessionKey);
    } else {
        result["sessionHexUsed"] = "";
    }

    JsonArray notifications = result.createNestedArray("notifications");
    appendNotificationChunks(notifications, chunks, times, startedAt);
    // Live validation shows post-HU responses are encrypted but do not echo the
    // 2-byte session token that requests prepend.
    appendDecodeAttempt(result.createNestedObject("decode"), command, encrypt, chunks, nullptr);
}

bool runFrameScenario(const char* name,
                      const char* command,
                      const ByteVector& payload,
                      const ByteVector* sessionKey,
                      bool encrypt,
                      bool chunked,
                      uint32_t interChunkDelayMs,
                      uint32_t waitMs,
                      const String& notificationModeForRequest,
                      JsonObject result,
                      String& error) {
    result["name"] = name;
    result["ok"] = false;

    ByteVector requestPacket;
    std::vector<ByteVector> chunks;
    std::vector<uint32_t> times;
    bool writeWithResponse = true;
    bool canWrite = false;
    bool canWriteNoResponse = false;
    const uint32_t startedAt = millis();
    if (!sendFramePacket(command,
                         payload,
                         sessionKey,
                         encrypt,
                         chunked,
                         interChunkDelayMs,
                         waitMs,
                         notificationModeForRequest,
                         writeWithResponse,
                         canWrite,
                         canWriteNoResponse,
                         requestPacket,
                         chunks,
                         times,
                         error)) {
        result["error"] = error;
        result["requestHex"] = hexEncode(requestPacket);
        result["notifyCount"] = chunks.size();
        JsonArray notifications = result.createNestedArray("notifications");
        appendNotificationChunks(notifications, chunks, times, startedAt);
        appendDecodeAttempt(result.createNestedObject("decode"), command, encrypt, chunks, nullptr);
        result["writeWithResponse"] = writeWithResponse;
        result["canWrite"] = canWrite;
        result["canWriteNoResponse"] = canWriteNoResponse;
        return false;
    }

    appendFrameScenarioResult(result,
                              command,
                              payload,
                              sessionKey,
                              encrypt,
                              chunked,
                              interChunkDelayMs,
                              waitMs,
                              writeWithResponse,
                              canWrite,
                              canWriteNoResponse,
                              requestPacket,
                              chunks,
                              times,
                              startedAt);
    return true;
}

bool runAppStyleProbe(uint32_t waitMs,
                      bool pairFirst,
                      bool reconnectAfterPair,
                      uint32_t reconnectDelayMs,
                      uint32_t settleMs,
                      bool warmupPing,
                      uint16_t hrRegisterId,
                      bool encrypt,
                      bool chunked,
                      uint32_t interChunkDelayMs,
                      const String& notificationModeForRequest,
                      const ByteVector* sessionKey,
                      DynamicJsonDocument& response,
                      String& error) {
    response["ok"] = false;
    if (pairFirst) {
        if (!pairWithDevice(error, reconnectAfterPair, reconnectDelayMs)) {
            return false;
        }
    } else if (!connectToSelectedDevice(error)) {
        return false;
    }

    if (!setNotificationsEnabled(true, error, notificationModeForRequest)) {
        return false;
    }

    String detailsError;
    fetchDeviceDetails(detailsError);
    response["detailsError"] = detailsError;
    JsonObject details = response.createNestedObject("details");
    details["manufacturer"] = cachedDetails.manufacturer;
    details["model"] = cachedDetails.model;
    details["serial"] = cachedDetails.serial;
    details["hardwareRevision"] = cachedDetails.hardwareRevision;
    details["firmwareRevision"] = cachedDetails.firmwareRevision;
    details["softwareRevision"] = cachedDetails.softwareRevision;
    details["ad06Hex"] = cachedDetails.ad06Hex;
    details["ad06Ascii"] = cachedDetails.ad06Ascii;

    JsonArray scenarios = response.createNestedArray("scenarios");
    bool overallOk = true;

    if (warmupPing) {
        JsonObject ping = scenarios.createNestedObject();
        if (!runFrameScenario("warmup_ping",
                              "Hp",
                              ByteVector{0x00, 0x00},
                              nullptr,
                              false,
                              false,
                              0,
                              waitMs,
                              notificationModeForRequest,
                              ping,
                              error)) {
            overallOk = false;
        }
    }

    if (settleMs > 0) {
        delay(settleMs);
    }

    struct ProbeDef {
        const char* name;
        const char* command;
        ByteVector payload;
    };
    const ProbeDef probes[] = {
        {"app_hv", "HV", ByteVector{}},
        {"app_hl", "HL", ByteVector{}},
        {"app_hx", "HX", ByteVector{}},
        {"app_hr", "HR", buildRegisterPayload(hrRegisterId)},
    };

    for (const auto& probe : probes) {
        JsonObject result = scenarios.createNestedObject();
        result["name"] = probe.name;
        result["ok"] = false;
        ByteVector requestPacket;
        std::vector<ByteVector> chunks;
        std::vector<uint32_t> times;
        bool writeWithResponse = true;
        bool canWrite = false;
        bool canWriteNoResponse = false;
        const uint32_t startedAt = millis();
        String scenarioError;
        if (!sendPreparedFramePacket(probe.command,
                                     probe.payload,
                                     sessionKey,
                                     encrypt,
                                     chunked,
                                     interChunkDelayMs,
                                     waitMs,
                                     writeWithResponse,
                                     canWrite,
                                     canWriteNoResponse,
                                     requestPacket,
                                     chunks,
                                     times,
                                     scenarioError)) {
            overallOk = false;
            if (error.isEmpty()) {
                error = scenarioError;
            }
            result["error"] = scenarioError;
            result["requestHex"] = hexEncode(requestPacket);
            result["notifyCount"] = chunks.size();
            JsonArray notifications = result.createNestedArray("notifications");
            appendNotificationChunks(notifications, chunks, times, startedAt);
            appendDecodeAttempt(result.createNestedObject("decode"), probe.command, encrypt, chunks, nullptr);
            result["writeWithResponse"] = writeWithResponse;
            result["canWrite"] = canWrite;
            result["canWriteNoResponse"] = canWriteNoResponse;
            continue;
        }
        appendFrameScenarioResult(result,
                                  probe.command,
                                  probe.payload,
                                  sessionKey,
                                  encrypt,
                                  chunked,
                                  interChunkDelayMs,
                                  waitMs,
                                  writeWithResponse,
                                  canWrite,
                                  canWriteNoResponse,
                                  requestPacket,
                                  chunks,
                                  times,
                                  startedAt);
    }

    response["ok"] = overallOk;
    return overallOk;
}

bool establishHuSessionForProbe(uint32_t waitMs,
                                const char* scenarioName,
                                const char* storedSource,
                                JsonArray scenarios,
                                String& error) {
    clearStoredSessionKey();

    ByteVector huSeed;
    ByteVector huPacket;
    if (!buildHuRequest(huSeed, huPacket)) {
        error = "failed to build HU request";
        return false;
    }

    ByteVector huPayload = huSeed;
    ByteVector huVerifier = deriveHuVerifier(huSeed, 0, huSeed.size());
    huPayload.insert(huPayload.end(), huVerifier.begin(), huVerifier.end());

    JsonObject hu = scenarios.createNestedObject();
    hu["name"] = scenarioName;
    hu["ok"] = false;
    hu["seedHex"] = hexEncode(huSeed);

    ByteVector huRequestPacket;
    std::vector<ByteVector> huChunks;
    std::vector<uint32_t> huTimes;
    bool huWriteWithResponse = true;
    bool huCanWrite = false;
    bool huCanWriteNoResponse = false;
    const uint32_t huStartedAt = millis();
    String huError;
    if (!sendPreparedFramePacket(CMD_HU,
                                 huPayload,
                                 nullptr,
                                 true,
                                 false,
                                 0,
                                 waitMs,
                                 huWriteWithResponse,
                                 huCanWrite,
                                 huCanWriteNoResponse,
                                 huRequestPacket,
                                 huChunks,
                                 huTimes,
                                 huError)) {
        hu["error"] = huError;
        hu["requestHex"] = hexEncode(huRequestPacket);
        hu["notifyCount"] = huChunks.size();
        JsonArray notifications = hu.createNestedArray("notifications");
        appendNotificationChunks(notifications, huChunks, huTimes, huStartedAt);
        appendDecodeAttempt(hu.createNestedObject("decode"), CMD_HU, true, huChunks, nullptr);
        hu["writeWithResponse"] = huWriteWithResponse;
        hu["canWrite"] = huCanWrite;
        hu["canWriteNoResponse"] = huCanWriteNoResponse;
        error = huError;
        return false;
    }

    appendFrameScenarioResult(hu,
                              CMD_HU,
                              huPayload,
                              nullptr,
                              true,
                              false,
                              0,
                              waitMs,
                              huWriteWithResponse,
                              huCanWrite,
                              huCanWriteNoResponse,
                              huRequestPacket,
                              huChunks,
                              huTimes,
                              huStartedAt);

    ByteVector joined;
    for (const auto& chunk : huChunks) {
        joined.insert(joined.end(), chunk.begin(), chunk.end());
    }

    ByteVector decodedPayload;
    String decodeError;
    if (!decodePacketAssumed(joined, CMD_HU, nullptr, true, decodedPayload, decodeError)) {
        hu["sessionParseError"] = decodeError;
        error = decodeError;
        return false;
    }
    if (decodedPayload.size() != 8) {
        error = String("HU response payload must be 8 bytes, got ") + decodedPayload.size();
        hu["sessionParseError"] = error;
        return false;
    }

    ByteVector sessionKey;
    if (!parseHuResponsePayload(decodedPayload, huSeed, sessionKey, decodeError)) {
        hu["sessionParseError"] = decodeError;
        error = decodeError;
        return false;
    }

    setStoredSessionKey(sessionKey, storedSource);
    hu["sessionHexDecoded"] = hexEncode(sessionKey);
    return true;
}

bool runStatsFlowProbe(uint32_t waitMs,
                       bool pairFirst,
                       bool reconnectAfterPair,
                       uint32_t reconnectDelayMs,
                       const String& notificationModeForRequest,
                       bool encrypt,
                       DynamicJsonDocument& response,
                       String& error) {
    response["ok"] = false;
    if (pairFirst) {
        if (!pairWithDevice(error, reconnectAfterPair, reconnectDelayMs)) {
            return false;
        }
    } else if (!connectToSelectedDevice(error)) {
        return false;
    }

    if (!setNotificationsEnabled(true, error, notificationModeForRequest)) {
        return false;
    }

    String detailsError;
    fetchDeviceDetails(detailsError);
    response["detailsError"] = detailsError;
    JsonObject details = response.createNestedObject("details");
    details["manufacturer"] = cachedDetails.manufacturer;
    details["model"] = cachedDetails.model;
    details["serial"] = cachedDetails.serial;
    details["hardwareRevision"] = cachedDetails.hardwareRevision;
    details["firmwareRevision"] = cachedDetails.firmwareRevision;
    details["softwareRevision"] = cachedDetails.softwareRevision;
    details["ad06Hex"] = cachedDetails.ad06Hex;
    details["ad06Ascii"] = cachedDetails.ad06Ascii;

    JsonArray scenarios = response.createNestedArray("scenarios");
    if (!establishHuSessionForProbe(waitMs, "stats_hu_internal", "stats-hu", scenarios, error)) {
        return false;
    }
    const ByteVector* sessionKey = resolveStoredSessionIfAvailable();
    response["sessionHexUsed"] = sessionKey != nullptr ? hexEncode(*sessionKey) : "";

    const nivona::ModelInfo modelInfo = nivona::detectModelInfo(toNivonaDetails(cachedDetails));
    std::vector<const nivona::RegisterProbe*> metrics;
    nivona::selectStatsDescriptors(modelInfo, metrics);
    response["supported"] = !metrics.empty();
    response["modelFamily"] = modelInfo.familyKey;

    bool overallOk = !metrics.empty();
    bool anyMetricOk = false;
    JsonObject groupedValues = response.createNestedObject("values");
    for (const auto* metric : metrics) {
        JsonObject result = scenarios.createNestedObject();
        result["name"] = metric->name;
        result["title"] = metric->title != nullptr ? metric->title : metric->name;
        result["registerId"] = metric->id;
        result["ok"] = false;

        if (metrics.empty()) {
            result["error"] = "statistics are not supported for this machine family";
            continue;
        }

        ByteVector payload = buildRegisterPayload(metric->id);
        ByteVector requestPacket;
        std::vector<ByteVector> chunks;
        std::vector<uint32_t> times;
        bool writeWithResponse = true;
        bool canWrite = false;
        bool canWriteNoResponse = false;
        const uint32_t startedAt = millis();
        String scenarioError;
        if (!sendPreparedFramePacket("HR",
                                     payload,
                                     sessionKey,
                                     encrypt,
                                     false,
                                     0,
                                     waitMs,
                                     writeWithResponse,
                                     canWrite,
                                     canWriteNoResponse,
                                     requestPacket,
                                     chunks,
                                     times,
                                     scenarioError)) {
            overallOk = false;
            if (error.isEmpty()) {
                error = scenarioError;
            }
            result["error"] = scenarioError;
            result["requestHex"] = hexEncode(requestPacket);
            result["notifyCount"] = chunks.size();
            JsonArray notifications = result.createNestedArray("notifications");
            appendNotificationChunks(notifications, chunks, times, startedAt);
            appendDecodeAttempt(result.createNestedObject("decode"), "HR", encrypt, chunks, sessionKey);
            result["writeWithResponse"] = writeWithResponse;
            result["canWrite"] = canWrite;
            result["canWriteNoResponse"] = canWriteNoResponse;
            continue;
        }

        appendFrameScenarioResult(result,
                                  "HR",
                                  payload,
                                  sessionKey,
                                  encrypt,
                                  false,
                                  0,
                                  waitMs,
                                  writeWithResponse,
                                  canWrite,
                                  canWriteNoResponse,
                                  requestPacket,
                                  chunks,
                                  times,
                                  startedAt);

        uint16_t echoedRegisterId = 0;
        int32_t value = 0;
        String decodeError;
        if (nivona::decodeHrNumericResponse(chunks, encrypt, echoedRegisterId, value, decodeError)) {
            result["rawValue"] = value;
            result["echoedRegisterId"] = echoedRegisterId;
            result["ok"] = true;
            anyMetricOk = true;

            JsonObject groupedMetric = groupedValues.createNestedObject(metric->name);
            groupedMetric["title"] = metric->title != nullptr ? metric->title : metric->name;
            groupedMetric["section"] = metric->section != nullptr ? metric->section : "maintenance";
            groupedMetric["unit"] = metric->unit != nullptr ? metric->unit : "count";
            groupedMetric["registerId"] = metric->id;
            groupedMetric["rawValue"] = value;
        } else {
            result["valueParseStatus"] = decodeError;
            overallOk = false;
        }
    }

    response["ok"] = anyMetricOk;
    response["partial"] = anyMetricOk && !overallOk;
    return anyMetricOk;
}

bool runSettingsFlowProbe(uint32_t waitMs,
                          bool pairFirst,
                          bool reconnectAfterPair,
                          uint32_t reconnectDelayMs,
                          const String& notificationModeForRequest,
                          bool encrypt,
                          SettingsFamily familyOverride,
                          DynamicJsonDocument& response,
                          String& error) {
    response["ok"] = false;
    if (pairFirst) {
        if (!pairWithDevice(error, reconnectAfterPair, reconnectDelayMs)) {
            return false;
        }
    } else if (!connectToSelectedDevice(error)) {
        return false;
    }

    if (!setNotificationsEnabled(true, error, notificationModeForRequest)) {
        return false;
    }

    String detailsError;
    fetchDeviceDetails(detailsError);
    response["detailsError"] = detailsError;
    JsonObject details = response.createNestedObject("details");
    details["manufacturer"] = cachedDetails.manufacturer;
    details["model"] = cachedDetails.model;
    details["serial"] = cachedDetails.serial;
    details["hardwareRevision"] = cachedDetails.hardwareRevision;
    details["firmwareRevision"] = cachedDetails.firmwareRevision;
    details["softwareRevision"] = cachedDetails.softwareRevision;
    details["ad06Hex"] = cachedDetails.ad06Hex;
    details["ad06Ascii"] = cachedDetails.ad06Ascii;

    SettingsProbeContext context;
    if (!nivona::resolveSettingsProbeContext(toNivonaDetails(cachedDetails), familyOverride, context, error)) {
        return false;
    }

    response["resolvedFamily"] = context.familyKey;
    response["familySource"] = context.familySource;
    response["modelCodeHint"] = context.modelCodeHint;
    response["is79xModel"] = context.is79xModel;
    response["hasAromaBalanceProfile"] = context.hasAromaBalanceProfile;

    std::vector<const SettingProbeDescriptor*> probes;
    selectSettingsDescriptors(context, probes);
    if (probes.empty()) {
        error = String("settings family \"") + context.familyKey + "\" is not supported yet";
        return false;
    }

    JsonArray scenarios = response.createNestedArray("scenarios");
    if (!establishHuSessionForProbe(waitMs, "settings_hu_internal", "settings-hu", scenarios, error)) {
        return false;
    }
    const ByteVector* sessionKey = resolveStoredSessionIfAvailable();
    response["sessionHexUsed"] = sessionKey != nullptr ? hexEncode(*sessionKey) : "";

    JsonObject values = response.createNestedObject("values");
    bool overallOk = true;
    for (const auto* probe : probes) {
        JsonObject result = scenarios.createNestedObject();
        result["name"] = probe->name;
        result["title"] = probe->title;
        result["registerId"] = probe->id;
        result["ok"] = false;

        ByteVector payload = buildRegisterPayload(probe->id);
        ByteVector requestPacket;
        std::vector<ByteVector> chunks;
        std::vector<uint32_t> times;
        bool writeWithResponse = true;
        bool canWrite = false;
        bool canWriteNoResponse = false;
        const uint32_t startedAt = millis();
        String scenarioError;
        if (!sendPreparedFramePacket("HR",
                                     payload,
                                     sessionKey,
                                     encrypt,
                                     false,
                                     0,
                                     waitMs,
                                     writeWithResponse,
                                     canWrite,
                                     canWriteNoResponse,
                                     requestPacket,
                                     chunks,
                                     times,
                                     scenarioError)) {
            overallOk = false;
            if (error.isEmpty()) {
                error = scenarioError;
            }
            result["error"] = scenarioError;
            result["requestHex"] = hexEncode(requestPacket);
            result["notifyCount"] = chunks.size();
            JsonArray notifications = result.createNestedArray("notifications");
            appendNotificationChunks(notifications, chunks, times, startedAt);
            appendDecodeAttempt(result.createNestedObject("decode"), "HR", encrypt, chunks, sessionKey);
            result["writeWithResponse"] = writeWithResponse;
            result["canWrite"] = canWrite;
            result["canWriteNoResponse"] = canWriteNoResponse;
            appendDecodedSettingResult(result, *probe, chunks, encrypt);
            continue;
        }

        appendFrameScenarioResult(result,
                                  "HR",
                                  payload,
                                  sessionKey,
                                  encrypt,
                                  false,
                                  0,
                                  waitMs,
                                  writeWithResponse,
                                  canWrite,
                                  canWriteNoResponse,
                                  requestPacket,
                                  chunks,
                                  times,
                                  startedAt);
        nivona::appendDecodedSettingResult(result, *probe, chunks, encrypt);

        if (String(result["valueParseStatus"] | "") == "decoded") {
            JsonObject valueItem = values.createNestedObject(probe->name);
            valueItem["title"] = probe->title;
            valueItem["registerId"] = probe->id;
            valueItem["rawValue"] = result["rawValue"];
            valueItem["valueCodeHex"] = result["valueCodeHex"];
            valueItem["valueLabel"] = result["valueLabel"];
        } else {
            overallOk = false;
        }
    }

    response["ok"] = overallOk;
    return overallOk;
}

bool runWorkerSessionProbe(uint32_t waitMs,
                           bool pairFirst,
                           bool reconnectAfterPair,
                           uint32_t reconnectDelayMs,
                           const String& notificationModeForRequest,
                           bool continueOnHuFailure,
                           DynamicJsonDocument& response,
                           String& error) {
    response["ok"] = false;
    clearStoredSessionKey();

    if (pairFirst) {
        if (!pairWithDevice(error, reconnectAfterPair, reconnectDelayMs)) {
            return false;
        }
    } else if (!connectToSelectedDevice(error)) {
        return false;
    }

    if (!setNotificationsEnabled(true, error, notificationModeForRequest)) {
        return false;
    }

    String detailsError;
    fetchDeviceDetails(detailsError);
    response["detailsError"] = detailsError;
    JsonObject details = response.createNestedObject("details");
    details["manufacturer"] = cachedDetails.manufacturer;
    details["model"] = cachedDetails.model;
    details["serial"] = cachedDetails.serial;
    details["hardwareRevision"] = cachedDetails.hardwareRevision;
    details["firmwareRevision"] = cachedDetails.firmwareRevision;
    details["softwareRevision"] = cachedDetails.softwareRevision;
    details["ad06Hex"] = cachedDetails.ad06Hex;
    details["ad06Ascii"] = cachedDetails.ad06Ascii;

    JsonArray scenarios = response.createNestedArray("scenarios");

    ByteVector huSeed;
    ByteVector huPacket;
    if (!buildHuRequest(huSeed, huPacket)) {
        error = "failed to build HU request";
        return false;
    }
    ByteVector huPayload = huSeed;
    ByteVector huVerifier = deriveHuVerifier(huSeed, 0, huSeed.size());
    huPayload.insert(huPayload.end(), huVerifier.begin(), huVerifier.end());

    JsonObject hu = scenarios.createNestedObject();
    hu["name"] = "worker_hu_internal";
    hu["ok"] = false;
    hu["seedHex"] = hexEncode(huSeed);
    ByteVector huRequestPacket;
    std::vector<ByteVector> huChunks;
    std::vector<uint32_t> huTimes;
    bool huWriteWithResponse = true;
    bool huCanWrite = false;
    bool huCanWriteNoResponse = false;
    const uint32_t huStartedAt = millis();
    String huError;
    if (!sendPreparedFramePacket(CMD_HU,
                                 huPayload,
                                 nullptr,
                                 true,
                                 false,
                                 0,
                                 waitMs,
                                 huWriteWithResponse,
                                 huCanWrite,
                                 huCanWriteNoResponse,
                                 huRequestPacket,
                                 huChunks,
                                 huTimes,
                                 huError)) {
        hu["error"] = huError;
        hu["requestHex"] = hexEncode(huRequestPacket);
        hu["notifyCount"] = huChunks.size();
        JsonArray notifications = hu.createNestedArray("notifications");
        appendNotificationChunks(notifications, huChunks, huTimes, huStartedAt);
        appendDecodeAttempt(hu.createNestedObject("decode"), CMD_HU, true, huChunks, nullptr);
        hu["writeWithResponse"] = huWriteWithResponse;
        hu["canWrite"] = huCanWrite;
        hu["canWriteNoResponse"] = huCanWriteNoResponse;

        if (!continueOnHuFailure) {
            String disconnectError;
            disconnectFromDevice(disconnectError);
            error = huError;
            response["workerAction"] = "disconnect-after-hu-failure";
            response["ok"] = false;
            return false;
        }
    } else {
        appendFrameScenarioResult(hu,
                                  CMD_HU,
                                  huPayload,
                                  nullptr,
                                  true,
                                  false,
                                  0,
                                  waitMs,
                                  huWriteWithResponse,
                                  huCanWrite,
                                  huCanWriteNoResponse,
                                  huRequestPacket,
                                  huChunks,
                                  huTimes,
                                  huStartedAt);

        ByteVector joined;
        for (const auto& chunk : huChunks) {
            joined.insert(joined.end(), chunk.begin(), chunk.end());
        }
        ByteVector decodedPayload;
        String decodeError;
        if (decodePacketAssumed(joined, CMD_HU, nullptr, true, decodedPayload, decodeError) && decodedPayload.size() == 8) {
            ByteVector sessionKey;
            if (parseHuResponsePayload(decodedPayload, huSeed, sessionKey, decodeError)) {
                setStoredSessionKey(sessionKey, "worker-hu");
                hu["sessionHexDecoded"] = hexEncode(sessionKey);
            } else {
                hu["sessionHexDecoded"] = "";
                hu["sessionParseError"] = decodeError;
            }
        }
    }

    const ByteVector* currentSession = resolveStoredSessionIfAvailable();
    response["workerSessionHex"] = currentSession != nullptr ? hexEncode(*currentSession) : "";

    struct WorkerProbeDef {
        const char* name;
        const char* command;
        ByteVector payload;
        bool useStoredSession;
    };
    const WorkerProbeDef probes[] = {
        {"worker_hv_public", "HV", ByteVector{}, true},
        {"worker_hl_public", "HL", ByteVector{}, true},
        {"worker_hx_public", "HX", ByteVector{}, true},
        {"worker_hr_total_beverages_public", "HR", buildRegisterPayload(213), true},
    };

    bool overallOk = currentSession != nullptr;
    for (const auto& probe : probes) {
        JsonObject result = scenarios.createNestedObject();
        result["name"] = probe.name;
        result["ok"] = false;
        const ByteVector* sessionKey = probe.useStoredSession ? resolveStoredSessionIfAvailable() : nullptr;
        ByteVector requestPacket;
        std::vector<ByteVector> chunks;
        std::vector<uint32_t> times;
        bool writeWithResponse = true;
        bool canWrite = false;
        bool canWriteNoResponse = false;
        const uint32_t startedAt = millis();
        String scenarioError;
        if (!sendPreparedFramePacket(probe.command,
                                     probe.payload,
                                     sessionKey,
                                     true,
                                     false,
                                     0,
                                     waitMs,
                                     writeWithResponse,
                                     canWrite,
                                     canWriteNoResponse,
                                     requestPacket,
                                     chunks,
                                     times,
                                     scenarioError)) {
            overallOk = false;
            if (error.isEmpty()) {
                error = scenarioError;
            }
            result["error"] = scenarioError;
            result["requestHex"] = hexEncode(requestPacket);
            result["notifyCount"] = chunks.size();
            JsonArray notifications = result.createNestedArray("notifications");
            appendNotificationChunks(notifications, chunks, times, startedAt);
            appendDecodeAttempt(result.createNestedObject("decode"), probe.command, true, chunks, nullptr);
            result["writeWithResponse"] = writeWithResponse;
            result["canWrite"] = canWrite;
            result["canWriteNoResponse"] = canWriteNoResponse;
            continue;
        }

        appendFrameScenarioResult(result,
                                  probe.command,
                                  probe.payload,
                                  sessionKey,
                                  true,
                                  false,
                                  0,
                                  waitMs,
                                  writeWithResponse,
                                  canWrite,
                                  canWriteNoResponse,
                                  requestPacket,
                                  chunks,
                                  times,
                                  startedAt);
    }

    response["workerAction"] = currentSession != nullptr ? "queued-after-hu-success" : "continued-without-session";
    response["ok"] = overallOk;
    return overallOk;
}

bool ensureClient(String& error);
bool initializeBleStack(String& error);
bool connectToSelectedDevice(String& error);
bool disconnectFromDevice(String& error);
bool pairWithDevice(String& error, bool reconnectAfterPair, uint32_t reconnectDelayMs);
bool fetchDeviceDetails(String& error);
bool setNotificationsEnabled(bool enable, String& error, const String& mode);
bool sendPreparedFramePacket(const char* command,
                             const ByteVector& payload,
                             const ByteVector* sessionKey,
                             bool encrypt,
                             bool chunked,
                             uint32_t interChunkDelayMs,
                             uint32_t waitMs,
                             bool& writeWithResponseOut,
                             bool& canWriteOut,
                             bool& canWriteNoResponseOut,
                             ByteVector& requestPacketOut,
                             std::vector<ByteVector>& chunksOut,
                             std::vector<uint32_t>& timesOut,
                             String& error);
const ByteVector* resolveStoredSessionIfAvailable(const String& serial, const String& address);
bool runWorkerSessionProbe(uint32_t waitMs,
                           bool pairFirst,
                           bool reconnectAfterPair,
                           uint32_t reconnectDelayMs,
                           const String& notificationModeForRequest,
                           bool continueOnHuFailure,
                           DynamicJsonDocument& response,
                           String& error);
bool writeRemoteCharacteristic(NimBLERemoteCharacteristic* characteristic,
                               const String& label,
                               const ByteVector& payload,
                               bool response,
                               bool chunked,
                               size_t chunkSize,
                               uint32_t interChunkDelayMs,
                               String& error);
void appendNotificationChunks(JsonArray chunksArray,
                              const std::vector<ByteVector>& chunks,
                              const std::vector<uint32_t>& times,
                              uint32_t baseMs);
void appendDecodeAttempt(JsonObject target,
                         const char* expectedCommand,
                         bool encrypted,
                         const std::vector<ByteVector>& chunks,
                         const ByteVector* expectedSessionKey);

class BridgeClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) override {
        addLog("ble", String("Connected to ") + pClient->getPeerAddress().toString().c_str());
    }

    void onDisconnect(NimBLEClient* pClient, int reason) override {
        addLog("ble", String("Disconnected from ") + pClient->getPeerAddress().toString().c_str() + " reason=" + String(reason));
        pairingStatus = "disconnected";
        clearRemoteHandles();
        cachedDetails = DeviceDetails{};
    }

    void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
        addLog("pair", String("Passkey entry requested for conn handle ") + String(connInfo.getConnHandle()));
        pairingStatus = "passkey-entry-requested";
    }

    void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) override {
        addLog("pair", String("Numeric comparison ") + String(pin) + " for conn handle " + String(connInfo.getConnHandle()) + ", auto-accepting");
        NimBLEDevice::injectConfirmPasskey(connInfo, true);
        pairingStatus = "numeric-comparison-accepted";
    }

    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        if (!connInfo.isEncrypted()) {
            pairingStatus = "authentication-failed";
            addLog("pair", "Authentication completed without encryption");
            return;
        }
        pairingStatus = connInfo.isBonded() ? "bonded" : "encrypted";
        addLog("pair",
               String("Authentication complete encrypted=") + (connInfo.isEncrypted() ? "yes" : "no") +
                   " bonded=" + (connInfo.isBonded() ? String("yes") : String("no")));
    }
} clientCallbacks;

class BridgeScanCallbacks : public NimBLEScanCallbacks {
    void onDiscovered(const NimBLEAdvertisedDevice* advertisedDevice) override {
        const ScanRecord record = recordFromAdvertisedDevice(advertisedDevice);
        if (scanDataMutex != nullptr && xSemaphoreTake(scanDataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            upsertScanRecord(scanScratchDevices, record);
            xSemaphoreGive(scanDataMutex);
        }
        Serial.printf("[scan] discovered %s %s RSSI=%d\n",
                      record.address.c_str(),
                      record.name.c_str(),
                      record.rssi);
    }

    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
        onDiscovered(advertisedDevice);
    }

    void onScanEnd(const NimBLEScanResults&, int reason) override {
        if (!idleScanInProgress && !blockingScanInProgress) {
            return;
        }
        lastScanReason = reason;
        lastScanAtMs   = millis();
        addLog("scan", String("Scan ended, reason=") + reason);
    }
} scanCallbacks;

NimBLEScanCallbacks* scanCallbacksPtr = &scanCallbacks;

bool ensureClient(String& error) {
    if (!NimBLEDevice::isInitialized() && !initializeBleStack(error)) {
        return false;
    }

    if (client != nullptr) {
        return true;
    }

    client = NimBLEDevice::createClient();
    if (client == nullptr) {
        error = "failed to create NimBLE client";
        return false;
    }

    client->setClientCallbacks(&clientCallbacks, false);
    client->setConnectionParams(12, 12, 0, 150);
    client->setConnectTimeout(5000);
    return true;
}

bool initializeBleStack(String& error) {
    if (!NimBLEDevice::isInitialized()) {
        NimBLEDevice::init("");
        NimBLEDevice::setSecurityAuth(true, false, true);
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
        addLog("ble", "NimBLE stack initialized");
    }
    error = "";
    return true;
}

bool refreshRemoteHandles(String& error) {
    const bool hadNotifications = notificationsEnabled;
    const String modeToRestore = normalizeNotificationMode(notificationMode);
    clearRemoteHandles();
    if (client == nullptr || !client->isConnected()) {
        error = "not connected";
        return false;
    }

    disService    = client->getService(DEVICE_INFORMATION_SERVICE);
    nivonaService = client->getService(NIVONA_SERVICE);
    if (nivonaService == nullptr) {
        error = "supported coffee-machine service not found on connected device";
        return false;
    }

    nivonaCtrl = nivonaService->getCharacteristic(NIVONA_CTRL);
    nivonaRx   = nivonaService->getCharacteristic(NIVONA_RX);
    nivonaTx   = nivonaService->getCharacteristic(NIVONA_TX);
    nivonaAux1 = nivonaService->getCharacteristic(NIVONA_AUX1);
    nivonaAux2 = nivonaService->getCharacteristic(NIVONA_AUX2);
    nivonaName = nivonaService->getCharacteristic(NIVONA_NAME);

    if (nivonaCtrl == nullptr || nivonaRx == nullptr || nivonaTx == nullptr || nivonaName == nullptr) {
        error = "missing one or more required proprietary service characteristics";
        return false;
    }

    if (hadNotifications) {
        if (!nivonaRx->subscribe(notificationModeUsesNotify(modeToRestore), rxNotifyCallback, true)) {
            error = "failed to restore rx notifications";
            return false;
        }
        notificationsEnabled = true;
        notificationMode = modeToRestore;
        addLog("ble", String("Restored protocol RX ") + modeToRestore + "s after handle refresh");
    }

    return true;
}

bool connectToSelectedDevice(String& error) {
    if (!ensureClient(error)) {
        return false;
    }

    cancelIdleScan();

    if (selectedAddress.isEmpty()) {
        error = "no device selected";
        return false;
    }

    if (client->isConnected()) {
        if (String(client->getPeerAddress().toString().c_str()).equalsIgnoreCase(selectedAddress)) {
            return refreshRemoteHandles(error);
        }
        client->disconnect();
        delay(200);
    }

    ScanRecord* record = findScannedDevice(selectedAddress);
    if (record != nullptr) {
        selectedAddressType = record->addressType;
    }

    NimBLEAddress address(std::string(selectedAddress.c_str()), selectedAddressType);
    if (!client->connect(address, true, false, true)) {
        error = String("failed to connect to ") + selectedAddress;
        return false;
    }

    return refreshRemoteHandles(error);
}

bool disconnectFromDevice(String& error) {
    if (client == nullptr || !client->isConnected()) {
        error = "not connected";
        return false;
    }
    client->disconnect();
    clearRemoteHandles();
    return true;
}

bool reconnectToSelectedDevice(uint32_t delayMs, String& error) {
    if (!ensureClient(error)) {
        return false;
    }

    if (client != nullptr && client->isConnected()) {
        client->disconnect();
        delay(delayMs > 0 ? delayMs : 200);
    }

    return connectToSelectedDevice(error);
}

bool pairWithDevice(String& error, bool reconnectAfterPair, uint32_t reconnectDelayMs) {
    if (!connectToSelectedDevice(error)) {
        return false;
    }

    NimBLEConnInfo info = client->getConnInfo();
    if (info.isEncrypted()) {
        pairingStatus = info.isBonded() ? "bonded" : "encrypted";
        addLog("pair",
               String("Reusing encrypted connection bonded=") +
                   (info.isBonded() ? String("yes") : String("no")));
        return true;
    }

    pairingStatus = "pairing";
    if (!client->secureConnection()) {
        error = "secureConnection failed";
        pairingStatus = "pairing-failed";
        return false;
    }

    info = client->getConnInfo();
    if (!info.isEncrypted()) {
        error = "connection is not encrypted after pairing";
        pairingStatus = "pairing-failed";
        return false;
    }

    pairingStatus = info.isBonded() ? "bonded" : "encrypted";
    if (!reconnectAfterPair) {
        return true;
    }

    addLog("pair", String("Forcing fresh reconnect after pairing, delayMs=") + reconnectDelayMs);
    if (!reconnectToSelectedDevice(reconnectDelayMs, error)) {
        pairingStatus = "pairing-failed";
        return false;
    }

    NimBLEConnInfo reconnectedInfo = client->getConnInfo();
    if (!reconnectedInfo.isEncrypted()) {
        if (!client->secureConnection()) {
            error = "secureConnection failed after reconnect";
            pairingStatus = "pairing-failed";
            return false;
        }
        reconnectedInfo = client->getConnInfo();
    }
    if (!reconnectedInfo.isEncrypted()) {
        error = "connection is not encrypted after bonded reconnect";
        pairingStatus = "pairing-failed";
        return false;
    }

    pairingStatus = reconnectedInfo.isBonded() ? "bonded" : "encrypted";
    addLog("pair",
           String("Fresh bonded reconnect complete encrypted=") + (reconnectedInfo.isEncrypted() ? "yes" : "no") +
               " bonded=" + (reconnectedInfo.isBonded() ? String("yes") : String("no")));
    return true;
}

bool readRemoteCharacteristicText(NimBLERemoteService* service, const char* uuid, String& out, String& error) {
    out = "";
    if (service == nullptr) {
        error = "service not available";
        return false;
    }
    NimBLERemoteCharacteristic* characteristic = service->getCharacteristic(uuid);
    if (characteristic == nullptr) {
        error = "characteristic not available";
        return false;
    }

    std::string value = characteristic->readValue();
    out               = String(value.c_str());
    return true;
}

bool readRemoteCharacteristicBytes(NimBLERemoteService* service, const char* uuid, ByteVector& out, String& error) {
    out.clear();
    if (service == nullptr) {
        error = "service not available";
        return false;
    }
    NimBLERemoteCharacteristic* characteristic = service->getCharacteristic(uuid);
    if (characteristic == nullptr) {
        error = "characteristic not available";
        return false;
    }

    std::string value = characteristic->readValue();
    out.assign(value.begin(), value.end());
    return true;
}

bool readCharacteristicValue(NimBLERemoteCharacteristic* characteristic, ByteVector& out, String& error) {
    out.clear();
    if (characteristic == nullptr) {
        error = "characteristic not available";
        return false;
    }

    std::string value = characteristic->readValue();
    out.assign(value.begin(), value.end());
    return true;
}

void appendCharacteristicInfo(JsonObject target, NimBLERemoteCharacteristic* characteristic) {
    if (characteristic == nullptr) {
        target["available"] = false;
        return;
    }

    target["available"] = true;
    target["uuid"] = String(characteristic->getUUID().toString().c_str());
    target["canRead"] = characteristic->canRead();
    target["canWrite"] = characteristic->canWrite();
    target["canWriteNoResponse"] = characteristic->canWriteNoResponse();
    target["canNotify"] = characteristic->canNotify();
    target["canIndicate"] = characteristic->canIndicate();
}

NimBLERemoteService* resolveRemoteServiceByUuid(const String& serviceUuid, String& error) {
    if (client == nullptr || !client->isConnected()) {
        error = "not connected";
        return nullptr;
    }
    if (serviceUuid.isEmpty()) {
        error = "serviceUuid is required";
        return nullptr;
    }

    const auto& services = client->getServices(true);
    for (auto* service : services) {
        if (service == nullptr) {
            continue;
        }
        if (String(service->getUUID().toString().c_str()).equalsIgnoreCase(serviceUuid)) {
            error = "";
            return service;
        }
    }

    error = "service not found";
    return nullptr;
}

NimBLERemoteCharacteristic* resolveRemoteCharacteristicByUuid(const String& serviceUuid,
                                                              const String& characteristicUuid,
                                                              NimBLERemoteService** serviceOut,
                                                              String& error) {
    if (serviceOut != nullptr) {
        *serviceOut = nullptr;
    }

    if (client == nullptr || !client->isConnected()) {
        error = "not connected";
        return nullptr;
    }
    if (characteristicUuid.isEmpty()) {
        error = "charUuid is required";
        return nullptr;
    }

    const auto& services = client->getServices(true);
    for (auto* service : services) {
        if (service == nullptr) {
            continue;
        }
        if (!serviceUuid.isEmpty() && !String(service->getUUID().toString().c_str()).equalsIgnoreCase(serviceUuid)) {
            continue;
        }
        const auto& characteristics = service->getCharacteristics(true);
        for (auto* characteristic : characteristics) {
            if (characteristic == nullptr) {
                continue;
            }
            if (String(characteristic->getUUID().toString().c_str()).equalsIgnoreCase(characteristicUuid)) {
                if (serviceOut != nullptr) {
                    *serviceOut = service;
                }
                error = "";
                return characteristic;
            }
        }
    }

    error = "characteristic not found";
    return nullptr;
}

void appendServiceInfo(JsonObject target, NimBLERemoteService* service, bool includeDescriptors) {
    if (service == nullptr) {
        target["available"] = false;
        return;
    }

    target["available"] = true;
    target["uuid"] = String(service->getUUID().toString().c_str());
    JsonArray characteristics = target.createNestedArray("characteristics");
    const auto& remoteCharacteristics = service->getCharacteristics(true);
    for (auto* characteristic : remoteCharacteristics) {
        JsonObject item = characteristics.createNestedObject();
        appendCharacteristicInfo(item, characteristic);
        if (characteristic == nullptr || !includeDescriptors) {
            continue;
        }

        JsonArray descriptors = item.createNestedArray("descriptors");
        const auto& remoteDescriptors = characteristic->getDescriptors(true);
        for (auto* descriptor : remoteDescriptors) {
            JsonObject desc = descriptors.createNestedObject();
            if (descriptor == nullptr) {
                desc["available"] = false;
                continue;
            }
            desc["available"] = true;
            desc["uuid"] = String(descriptor->getUUID().toString().c_str());
        }
    }
}

void appendServiceList(JsonArray servicesArray, bool includeDescriptors) {
    if (client == nullptr || !client->isConnected()) {
        return;
    }
    const auto& services = client->getServices(true);
    for (auto* service : services) {
        JsonObject item = servicesArray.createNestedObject();
        appendServiceInfo(item, service, includeDescriptors);
    }
}

bool fetchDeviceDetails(String& error) {
    if (!connectToSelectedDevice(error)) {
        return false;
    }

    DeviceDetails details;
    details.fetched = true;

    String tmpError;
    if (!readRemoteCharacteristicText(disService, DIS_MANUFACTURER, details.manufacturer, tmpError)) {
        details.lastError = "manufacturer: " + tmpError;
    }
    if (!readRemoteCharacteristicText(disService, DIS_MODEL, details.model, tmpError)) {
        details.lastError = details.lastError + (details.lastError.isEmpty() ? "" : "; ") + "model: " + tmpError;
    }
    if (!readRemoteCharacteristicText(disService, DIS_SERIAL, details.serial, tmpError)) {
        details.lastError = details.lastError + (details.lastError.isEmpty() ? "" : "; ") + "serial: " + tmpError;
    }
    if (!readRemoteCharacteristicText(disService, DIS_HARDWARE, details.hardwareRevision, tmpError)) {
        details.lastError = details.lastError + (details.lastError.isEmpty() ? "" : "; ") + "hardware: " + tmpError;
    }
    if (!readRemoteCharacteristicText(disService, DIS_FIRMWARE, details.firmwareRevision, tmpError)) {
        details.lastError = details.lastError + (details.lastError.isEmpty() ? "" : "; ") + "firmware: " + tmpError;
    }
    if (!readRemoteCharacteristicText(disService, DIS_SOFTWARE, details.softwareRevision, tmpError)) {
        details.lastError = details.lastError + (details.lastError.isEmpty() ? "" : "; ") + "software: " + tmpError;
    }

    ByteVector ad06;
    if (readRemoteCharacteristicBytes(nivonaService, NIVONA_NAME, ad06, tmpError)) {
        details.ad06Hex   = hexEncode(ad06);
        details.ad06Ascii = printableAscii(ad06);
    } else {
        details.lastError = details.lastError + (details.lastError.isEmpty() ? "" : "; ") + "AD06: " + tmpError;
    }

    cachedDetails = details;
    error         = details.lastError;
    return true;
}

void rxNotifyCallback(NimBLERemoteCharacteristic* characteristic, uint8_t* data, size_t length, bool isNotify) {
    (void)characteristic;
    addHexLog(isNotify ? "rx-notify" : "rx-indicate", data, length);

    if (notifyDataMutex != nullptr && xSemaphoreTake(notifyDataMutex, 0) == pdTRUE) {
        lastNotificationBytes.assign(data, data + length);
        notificationHistory.emplace_back(data, data + length);
        notificationHistoryMs.push_back(millis());
        xSemaphoreGive(notifyDataMutex);
    }
    if (notificationLatch != nullptr) {
        xSemaphoreGive(notificationLatch);
    }
}

void genericNotifyCallback(NimBLERemoteCharacteristic* characteristic, uint8_t* data, size_t length, bool isNotify) {
    String source = isNotify ? "gatt-notify" : "gatt-indicate";
    if (characteristic != nullptr) {
        source += ":";
        source += String(characteristic->getUUID().toString().c_str());
    }
    addHexLog(source, data, length);

    if (notifyDataMutex != nullptr && xSemaphoreTake(notifyDataMutex, 0) == pdTRUE) {
        lastNotificationBytes.assign(data, data + length);
        notificationHistory.emplace_back(data, data + length);
        notificationHistoryMs.push_back(millis());
        xSemaphoreGive(notifyDataMutex);
    }
    if (notificationLatch != nullptr) {
        xSemaphoreGive(notificationLatch);
    }
}

bool writeRemoteCharacteristic(NimBLERemoteCharacteristic* characteristic,
                               const String& alias,
                               const ByteVector& payload,
                               bool response,
                               bool chunked,
                               size_t chunkSize,
                               uint32_t interChunkDelayMs,
                               String& error) {
    if (characteristic == nullptr) {
        error = "characteristic not available";
        return false;
    }
    if (!chunked || payload.size() <= chunkSize) {
        if (!characteristic->writeValue(payload.data(), payload.size(), response)) {
            error = "writeValue failed";
            return false;
        }
        addHexLog("tx-" + alias, payload.data(), payload.size());
        return true;
    }

    for (size_t offset = 0; offset < payload.size(); offset += chunkSize) {
        const size_t partSize = std::min(chunkSize, payload.size() - offset);
        if (!characteristic->writeValue(payload.data() + offset, partSize, response)) {
            error = String("writeValue failed at chunk offset ") + offset;
            return false;
        }
        addHexLog("tx-" + alias + "-chunk", payload.data() + offset, partSize);
        if (interChunkDelayMs > 0 && offset + partSize < payload.size()) {
            delay(interChunkDelayMs);
        }
    }

    return true;
}

bool setNotificationsEnabled(bool enable, String& error, const String& mode) {
    if (!connectToSelectedDevice(error)) {
        return false;
    }

    if (nivonaRx == nullptr) {
        error = "rx characteristic not available";
        return false;
    }

    if (enable) {
        const String normalizedMode = normalizeNotificationMode(mode);
        const bool useNotify = notificationModeUsesNotify(normalizedMode);
        if (notificationsEnabled && notificationMode == normalizedMode) {
            return true;
        }
        if (notificationsEnabled && !nivonaRx->unsubscribe(true)) {
            error = "failed to switch rx notification mode";
            return false;
        }
        if (!nivonaRx->subscribe(useNotify, rxNotifyCallback, true)) {
            error = "failed to subscribe to rx notifications";
            return false;
        }
        notificationsEnabled = true;
        notificationMode = normalizedMode;
        addLog("ble", String("Subscribed to protocol RX via ") + normalizedMode);
        return true;
    }

    if (!notificationsEnabled) {
        return true;
    }
    if (!nivonaRx->unsubscribe(true)) {
        error = "failed to unsubscribe from rx notifications";
        return false;
    }
    notificationsEnabled = false;
    notificationMode = "off";
    addLog("ble", "Unsubscribed from protocol RX notifications");
    return true;
}

NimBLERemoteCharacteristic* resolveNivonaCharacteristic(const String& alias) {
    const String key = alias;
    if (key == "ctrl") {
        return nivonaCtrl;
    }
    if (key == "rx") {
        return nivonaRx;
    }
    if (key == "tx") {
        return nivonaTx;
    }
    if (key == "aux1") {
        return nivonaAux1;
    }
    if (key == "aux2") {
        return nivonaAux2;
    }
    if (key == "name") {
        return nivonaName;
    }
    return nullptr;
}

bool rawWriteNivona(const String& alias,
                    const ByteVector& payload,
                    bool response,
                    uint32_t waitMs,
                    ByteVector& notifyBytes,
                    String& error) {
    notifyBytes.clear();
    if (!connectToSelectedDevice(error)) {
        return false;
    }

    NimBLERemoteCharacteristic* characteristic = resolveNivonaCharacteristic(alias);
    if (characteristic == nullptr) {
        error = "unknown or unavailable characteristic alias";
        return false;
    }

    if (waitMs > 0 && !notificationsEnabled) {
        error = "rx notifications are not enabled";
        return false;
    }

    clearNotificationBuffer();
    if (!writeRemoteCharacteristic(characteristic, alias, payload, response, false, 10, 0, error)) {
        return false;
    }

    if (waitMs > 0) {
        if (!waitForNotification(waitMs, notifyBytes)) {
            error = "timed out waiting for notification";
            return false;
        }
    }

    return true;
}

bool rawReadNivona(const String& alias, ByteVector& valueBytes, String& error) {
    valueBytes.clear();
    if (!connectToSelectedDevice(error)) {
        return false;
    }

    NimBLERemoteCharacteristic* characteristic = resolveNivonaCharacteristic(alias);
    if (characteristic == nullptr) {
        error = "unknown or unavailable characteristic alias";
        return false;
    }

    return readCharacteristicValue(characteristic, valueBytes, error);
}

bool runHuExperiment(uint32_t waitMs, String& error) {
    ByteVector seed;
    ByteVector request;
    ByteVector notifyBytes;

    if (!buildHuRequest(seed, request)) {
        error = "failed to build HU request";
        return false;
    }

    if (!rawWriteNivona("tx", request, true, waitMs, notifyBytes, error)) {
        lastHuSeedHex     = hexEncode(seed);
        lastHuRequestHex  = hexEncode(request);
        lastHuResponseHex = "";
        lastHuParseStatus = error;
        return false;
    }

    lastHuSeedHex     = hexEncode(seed);
    lastHuRequestHex  = hexEncode(request);
    lastHuResponseHex = hexEncode(notifyBytes);

    ByteVector decodedPayload;
    String parseError;
    if (!decodePacketAssumed(notifyBytes, CMD_HU, nullptr, true, decodedPayload, parseError)) {
        lastHuParseStatus = "raw notification captured, decode still experimental: " + parseError;
        return true;
    }

    if (decodedPayload.size() != 8) {
        lastHuParseStatus = "decoded HU payload length mismatch";
        return true;
    }

    ByteVector echoedSeed(decodedPayload.begin(), decodedPayload.begin() + 4);
    ByteVector sessionKey(decodedPayload.begin() + 4, decodedPayload.begin() + 6);
    ByteVector verifier(decodedPayload.begin() + 6, decodedPayload.end());
    ByteVector expectedVerifier = deriveHuVerifier(decodedPayload, 0, 6);

    if (echoedSeed != seed) {
        lastHuParseStatus = "decoded HU response had seed mismatch";
        return true;
    }
    if (verifier != expectedVerifier) {
        lastHuParseStatus = "decoded HU response had verifier mismatch";
        return true;
    }

    setStoredSessionKey(sessionKey, "hu-decode");
    lastHuParseStatus = String("decoded session key: ") + hexEncode(sessionKey);
    return true;
}

void appendNotificationChunks(JsonArray chunksArray,
                              const std::vector<ByteVector>& chunks,
                              const std::vector<uint32_t>& times,
                              uint32_t baseMs) {
    for (size_t i = 0; i < chunks.size(); ++i) {
        JsonObject item = chunksArray.createNestedObject();
        item["hex"]     = hexEncode(chunks[i]);
        item["tOffsetMs"] = times.size() > i ? (times[i] >= baseMs ? times[i] - baseMs : 0) : 0;
    }
}

void appendDecodeAttempt(JsonObject target,
                         const char* expectedCommand,
                         bool encrypted,
                         const std::vector<ByteVector>& chunks,
                         const ByteVector* expectedSessionKey) {
    ByteVector joined;
    size_t totalSize = 0;
    for (const auto& chunk : chunks) {
        totalSize += chunk.size();
    }
    joined.reserve(totalSize);
    for (const auto& chunk : chunks) {
        joined.insert(joined.end(), chunk.begin(), chunk.end());
    }

    if (joined.empty()) {
        target["parseStatus"] = "no notifications";
        return;
    }

    target["joinedHex"] = hexEncode(joined);
    ByteVector decodedPayload;
    String decodeError;
    if (!decodePacketAssumed(joined, expectedCommand, expectedSessionKey, encrypted, decodedPayload, decodeError)) {
        target["parseStatus"] = decodeError;
        return;
    }

    target["parseStatus"] = "decoded";
    target["decodedPayloadHex"] = hexEncode(decodedPayload);
}

bool runVerifyScenario(const char* name,
                       const ByteVector* preflightPacket,
                       const char* preflightCommand,
                       bool preflightEncrypted,
                       const ByteVector* packet,
                       const char* expectedCommand,
                       bool encrypted,
                       bool chunked,
                       uint32_t interChunkDelayMs,
                       uint32_t waitMs,
                       JsonObject result,
                       String& error) {
    result["name"]              = name;
    result["chunked"]           = chunked;
    result["interChunkDelayMs"] = interChunkDelayMs;
    result["waitMs"]            = waitMs;
    result["ok"]                = false;

    std::vector<ByteVector> chunks;
    std::vector<uint32_t> times;
    const uint32_t startedAt = millis();
    clearNotificationBuffer();

    if (preflightPacket != nullptr) {
        result["preflightHex"] = hexEncode(*preflightPacket);
        if (!writeRemoteCharacteristic(nivonaTx, "tx", *preflightPacket, true, true, 10, 0, error)) {
            result["error"] = error;
            return false;
        }
        delay(500);
    }

    if (packet != nullptr) {
        result["requestHex"] = hexEncode(*packet);
        if (!writeRemoteCharacteristic(nivonaTx, "tx", *packet, true, chunked, 10, interChunkDelayMs, error)) {
            result["error"] = error;
            return false;
        }
    }

    if (waitMs > 0) {
        const uint32_t deadline = millis() + waitMs;
        while (millis() < deadline) {
            delay(10);
        }
    }

    copyNotificationHistory(chunks, times);
    result["notifyCount"] = chunks.size();
    JsonArray notifications = result.createNestedArray("notifications");
    appendNotificationChunks(notifications, chunks, times, startedAt);
    appendDecodeAttempt(result.createNestedObject("decode"), expectedCommand, encrypted, chunks, nullptr);

    if (preflightPacket != nullptr) {
        appendDecodeAttempt(result.createNestedObject("preflightDecode"), preflightCommand, preflightEncrypted, chunks, nullptr);
    }

    result["ok"] = true;
    return true;
}

bool runCommunicationVerify(uint32_t waitMs,
                            bool pairFirst,
                            bool reconnectAfterPair,
                            uint32_t reconnectDelayMs,
                            const String& notificationModeForRequest,
                            DynamicJsonDocument& response,
                            String& error) {
    response["ok"] = false;
    if (pairFirst) {
        if (!pairWithDevice(error, reconnectAfterPair, reconnectDelayMs)) {
            return false;
        }
    } else if (!connectToSelectedDevice(error)) {
        return false;
    }

    if (!setNotificationsEnabled(true, error, notificationModeForRequest)) {
        return false;
    }

    String detailsError;
    fetchDeviceDetails(detailsError);
    response["detailsError"] = detailsError;
    JsonObject details       = response.createNestedObject("details");
    details["manufacturer"]  = cachedDetails.manufacturer;
    details["model"]         = cachedDetails.model;
    details["serial"]        = cachedDetails.serial;
    details["hardwareRevision"] = cachedDetails.hardwareRevision;
    details["firmwareRevision"] = cachedDetails.firmwareRevision;
    details["softwareRevision"] = cachedDetails.softwareRevision;
    details["ad06Hex"]          = cachedDetails.ad06Hex;
    details["ad06Ascii"]        = cachedDetails.ad06Ascii;

    ByteVector pingPacket = buildPacket("Hp", ByteVector{0x00, 0x00}, nullptr, false);
    ByteVector huSeed;
    ByteVector huEncrypted;
    buildHuRequest(huSeed, huEncrypted);
    ByteVector huPayload = huSeed;
    ByteVector huVerifier = deriveHuVerifier(huSeed, 0, huSeed.size());
    huPayload.insert(huPayload.end(), huVerifier.begin(), huVerifier.end());
    ByteVector huPlain = buildPacket(CMD_HU, huPayload, nullptr, false);

    response["huSeedHex"] = hexEncode(huSeed);
    JsonArray scenarios   = response.createNestedArray("scenarios");

    struct Scenario {
        const char* name;
        const ByteVector* preflight;
        const char* preflightCommand;
        bool preflightEncrypted;
        const ByteVector* packet;
        const char* expectedCommand;
        bool encrypted;
        bool chunked;
        uint32_t interChunkDelayMs;
    };

    const Scenario scenarioList[] = {
        {"ping_plain", nullptr, nullptr, false, &pingPacket, "Hp", false, true, 0},
        {"hu_encrypted_chunked", nullptr, nullptr, false, &huEncrypted, CMD_HU, true, true, 0},
        {"hu_encrypted_one_shot", nullptr, nullptr, false, &huEncrypted, CMD_HU, true, false, 0},
        {"hu_plain_chunked", nullptr, nullptr, false, &huPlain, CMD_HU, false, true, 0},
        {"ping_then_hu_encrypted", &pingPacket, "Hp", false, &huEncrypted, CMD_HU, true, true, 0},
    };

    for (const auto& scenario : scenarioList) {
        JsonObject item = scenarios.createNestedObject();
        String scenarioError;
        if (!runVerifyScenario(scenario.name,
                               scenario.preflight,
                               scenario.preflightCommand,
                               scenario.preflightEncrypted,
                               scenario.packet,
                               scenario.expectedCommand,
                               scenario.encrypted,
                               scenario.chunked,
                               scenario.interChunkDelayMs,
                               waitMs,
                               item,
                               scenarioError)) {
            item["error"] = scenarioError;
            error         = scenarioError;
            return false;
        }
    }

    response["ok"] = true;
    return true;
}

void updateSavedMachineFromCachedDetails(SavedMachine& machine) {
    if (cachedDetails.serial.isEmpty()) {
        return;
    }
    populateSavedMachine(machine, machine.alias, machine.address, machine.addressType, cachedDetails);
    refreshSavedMachinePresence(machine);
    syncProtocolSessionTarget(machine);
    persistSavedMachines();
}

bool beginMachineProtocolSession(SavedMachine& machine, String& error, bool clearSession = true) {
    if (!selectSavedMachine(machine, error)) {
        return false;
    }
    if (clearSession) {
        clearStoredSessionKey(machine.serial, machine.address);
    }
    if (!pairWithDevice(error, true, DEFAULT_RECONNECT_DELAY_MS)) {
        return false;
    }
    if (!setNotificationsEnabled(true, error, "notify")) {
        return false;
    }
    suppressIdleScans();
    String detailsError;
    fetchDeviceDetails(detailsError);
    updateSavedMachineFromCachedDetails(machine);
    return true;
}

bool ensureMachineHuSession(uint32_t waitMs, String& error) {
    if (resolveStoredSessionIfAvailable() != nullptr) {
        return true;
    }
    DynamicJsonDocument scratch(4096);
    JsonArray scenarios = scratch.createNestedArray("scenarios");
    return establishHuSessionForProbe(waitMs, "machine_hu_internal", "machine-hu", scenarios, error);
}

bool readMachineNumericRegister(uint16_t registerId, int32_t& valueOut, String& error, uint32_t waitMs = 2500) {
    valueOut = 0;
    if (!ensureMachineHuSession(waitMs, error)) {
        return false;
    }
    const ByteVector* sessionKey = resolveStoredSessionIfAvailable();
    ByteVector requestPacket;
    std::vector<ByteVector> chunks;
    std::vector<uint32_t> times;
    bool writeWithResponse = true;
    bool canWrite = false;
    bool canWriteNoResponse = false;
    if (!sendPreparedFramePacket("HR",
                                 nivona::buildRegisterPayload(registerId),
                                 sessionKey,
                                 true,
                                 false,
                                 0,
                                 waitMs,
                                 writeWithResponse,
                                 canWrite,
                                 canWriteNoResponse,
                                 requestPacket,
                                 chunks,
                                 times,
                                 error)) {
        return false;
    }
    uint16_t echoedRegisterId = 0;
    if (!nivona::decodeHrNumericResponse(chunks, true, echoedRegisterId, valueOut, error)) {
        return false;
    }
    if (echoedRegisterId != registerId) {
        error = String("register echo mismatch: expected ") + registerId + ", got " + echoedRegisterId;
        return false;
    }
    return true;
}

bool readMachineStringRegister(uint16_t registerId,
                               nivona::RecipeTextEncoding encoding,
                               String& rawValueOut,
                               String& displayValueOut,
                               String& error,
                               uint32_t waitMs = 2500) {
    rawValueOut = "";
    displayValueOut = "";
    if (!ensureMachineHuSession(waitMs, error)) {
        return false;
    }
    const ByteVector* sessionKey = resolveStoredSessionIfAvailable();
    ByteVector requestPacket;
    std::vector<ByteVector> chunks;
    std::vector<uint32_t> times;
    bool writeWithResponse = true;
    bool canWrite = false;
    bool canWriteNoResponse = false;
    if (!sendPreparedFramePacket("HA",
                                 nivona::buildRegisterPayload(registerId),
                                 sessionKey,
                                 true,
                                 false,
                                 0,
                                 waitMs,
                                 writeWithResponse,
                                 canWrite,
                                 canWriteNoResponse,
                                 requestPacket,
                                 chunks,
                                 times,
                                 error)) {
        return false;
    }
    return nivona::decodeHaResponse(chunks, true, registerId, encoding, rawValueOut, displayValueOut, error);
}

bool sendMachineCommand(const char* command,
                        const ByteVector& payload,
                        const ByteVector* sessionKey,
                        bool encrypt,
                        String& error,
                        std::vector<ByteVector>* chunksOut = nullptr) {
    ByteVector requestPacket;
    std::vector<ByteVector> chunks;
    std::vector<uint32_t> times;
    bool writeWithResponse = true;
    bool canWrite = false;
    bool canWriteNoResponse = false;
    if (!sendPreparedFramePacket(command,
                                 payload,
                                 sessionKey,
                                 encrypt,
                                 false,
                                 0,
                                 0,
                                 writeWithResponse,
                                 canWrite,
                                 canWriteNoResponse,
                                 requestPacket,
                                 chunks,
                                 times,
                                 error)) {
        return false;
    }
    if (chunksOut != nullptr) {
        *chunksOut = chunks;
    }
    return true;
}

bool writeMachineNumericRegister(uint16_t registerId, int32_t value, String& error) {
    if (!ensureMachineHuSession(2500, error)) {
        return false;
    }
    const ByteVector* sessionKey = resolveStoredSessionIfAvailable();
    return sendMachineCommand("HW", nivona::buildWriteRegisterPayload(registerId, value), sessionKey, true, error);
}

bool writeMachineStringRegister(uint16_t registerId,
                                nivona::RecipeTextEncoding encoding,
                                const String& value,
                                String& error) {
    if (!ensureMachineHuSession(2500, error)) {
        return false;
    }
    const ByteVector* sessionKey = resolveStoredSessionIfAvailable();
    return sendMachineCommand("HB", nivona::encodeHbNamePayload(registerId, encoding, value), sessionKey, true, error);
}

bool readMachineProcessStatus(nivona::ProcessStatus& statusOut, String& error) {
    if (!ensureMachineHuSession(2500, error)) {
        return false;
    }
    const ByteVector* sessionKey = resolveStoredSessionIfAvailable();
    ByteVector requestPacket;
    std::vector<ByteVector> chunks;
    std::vector<uint32_t> times;
    bool writeWithResponse = true;
    bool canWrite = false;
    bool canWriteNoResponse = false;
    if (!sendPreparedFramePacket("HX",
                                 ByteVector{},
                                 sessionKey,
                                 true,
                                 false,
                                 0,
                                 2500,
                                 writeWithResponse,
                                 canWrite,
                                 canWriteNoResponse,
                                 requestPacket,
                                 chunks,
                                 times,
                                 error)) {
        return false;
    }
    return nivona::decodeHxResponse(chunks, true, statusOut, error);
}

bool parseMachineRoute(String& serialOut, String& sectionOut, String& tailOut) {
    serialOut = "";
    sectionOut = "";
    tailOut = "";

    const String prefix = "/api/machines/";
    const String uri = server.uri();
    if (!uri.startsWith(prefix)) {
        return false;
    }
    String rest = uri.substring(prefix.length());
    const int firstSlash = rest.indexOf('/');
    if (firstSlash < 0) {
        serialOut = rest;
        return !serialOut.isEmpty();
    }
    serialOut = rest.substring(0, firstSlash);
    rest = rest.substring(firstSlash + 1);
    const int secondSlash = rest.indexOf('/');
    if (secondSlash < 0) {
        sectionOut = rest;
        return true;
    }
    sectionOut = rest.substring(0, secondSlash);
    tailOut = rest.substring(secondSlash + 1);
    return true;
}

void performScan() {
    cancelIdleScan();
    clearScanScratch();

    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan == nullptr) {
        addLog("scan", "BLE scan object is unavailable");
        lastScanReason = -1;
        lastScanAtMs = millis();
        return;
    }
    configureBleScan(*scan);

    addLog("scan", "Starting BLE scan");
    lastScanReason = 0;
    const uint32_t startedAt = millis();
    blockingScanInProgress = true;
    if (!scan->start(SCAN_MS, false, false)) {
        blockingScanInProgress = false;
        addLog("scan", "Failed to start BLE scan");
        lastScanReason = -1;
        lastScanAtMs   = millis();
        return;
    }

    while (scan->isScanning() && (millis() - startedAt) < (SCAN_MS + 1000)) {
        delay(50);
    }

    if (scan->isScanning()) {
        scan->stop();
    }
    delay(150);

    publishScanResults(startedAt, true, "Completed BLE scan");
    blockingScanInProgress = false;
}

void appendStatus(JsonDocument& doc) {
    doc["appName"]       = APP_NAME;
    doc["appVersion"]    = APP_VERSION;
    doc["buildTime"]     = APP_BUILD_TIME;
    doc["hostname"]      = APP_HOSTNAME;
    doc["apSsid"]        = AP_SSID;
    doc["apPassword"]    = AP_PASSWORD;
    doc["apIp"]          = WiFi.softAPIP().toString();
    doc["staConnected"]  = WiFi.status() == WL_CONNECTED;
    doc["staSsid"]       = wifiStaSsid;
    doc["staIp"]         = WiFi.localIP().toString();
    doc["selectedAddress"] = selectedAddress;
    doc["notificationsEnabled"] = notificationsEnabled;
    doc["notificationMode"] = notificationsEnabled ? notificationMode : "off";
    doc["pairingStatus"] = pairingStatus;
    doc["lastError"]     = lastError;
    doc["lastHuSeedHex"] = lastHuSeedHex;
    doc["lastHuRequestHex"] = lastHuRequestHex;
    doc["lastHuResponseHex"] = lastHuResponseHex;
    doc["lastHuParseStatus"] = lastHuParseStatus;
    doc["protocolSessionCount"] = protocolSessions.size();
    doc["standardRecipeCacheReady"] = littleFsReady;

    size_t supportedCount = 0;
    for (const auto& record : scannedDevices) {
        if (record.likelySupported) {
            supportedCount++;
        }
    }
    doc["supportedAdvertised"] = supportedCount > 0;
    doc["deviceCount"]         = scannedDevices.size();
    doc["supportedDeviceCount"] = supportedCount;
    doc["savedMachineCount"]   = savedMachines.size();
    doc["lastScanReason"]      = lastScanReason;
    doc["lastScanResultCount"] = lastScanResultCount;
    doc["lastScanAtMs"]     = lastScanAtMs;
    doc["scanInProgress"]   = idleScanInProgress || blockingScanInProgress;
    doc["idleScanInProgress"] = idleScanInProgress;

    if (client != nullptr) {
        doc["clientCreated"] = true;
        doc["clientConnected"] = client->isConnected();
        if (client->isConnected()) {
            NimBLEConnInfo info = client->getConnInfo();
            const String peerAddress = String(client->getPeerAddress().toString().c_str());
            doc["peerAddress"] = peerAddress;
            doc["bonded"]      = info.isBonded();
            doc["encrypted"]   = info.isEncrypted();
            doc["mtu"]         = client->getMTU();
            doc["txCanWrite"] = nivonaTx != nullptr ? nivonaTx->canWrite() : false;
            doc["txCanWriteNoResponse"] = nivonaTx != nullptr ? nivonaTx->canWriteNoResponse() : false;
            doc["rxCanNotify"] = nivonaRx != nullptr ? nivonaRx->canNotify() : false;
            doc["rxCanIndicate"] = nivonaRx != nullptr ? nivonaRx->canIndicate() : false;
        }
    } else {
        doc["clientCreated"]   = false;
        doc["clientConnected"] = false;
    }
}

bool parseAddressTypeRequest(JsonVariantConst value, uint8_t& addressType) {
    if (value.isNull()) {
        return false;
    }
    if (value.is<uint8_t>()) {
        const uint8_t numeric = value.as<uint8_t>();
        if (numeric > BLE_ADDR_RANDOM) {
            return false;
        }
        addressType = numeric;
        return true;
    }
    if (value.is<int>()) {
        const int numeric = value.as<int>();
        if (numeric < 0 || numeric > BLE_ADDR_RANDOM) {
            return false;
        }
        addressType = static_cast<uint8_t>(numeric);
        return true;
    }
    const String rawValue = value.as<String>();
    if (rawValue.isEmpty()) {
        return false;
    }
    String normalized = rawValue;
    normalized.trim();
    normalized.toLowerCase();
    if (normalized == "public" || normalized == "0") {
        addressType = BLE_ADDR_PUBLIC;
        return true;
    }
    if (normalized == "random" || normalized == "1") {
        addressType = BLE_ADDR_RANDOM;
        return true;
    }
    return false;
}

bool normalizeBleAddress(String& address) {
    address.trim();
    address.toUpperCase();
    if (address.length() != 17) {
        return false;
    }
    for (size_t i = 0; i < address.length(); ++i) {
        const char ch = address.charAt(i);
        if ((i + 1) % 3 == 0) {
            if (ch != ':') {
                return false;
            }
            continue;
        }
        if (!isxdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

bool probeMachineAddress(const String& address, SavedMachine& machineOut, String& error, const uint8_t* addressTypeOverride = nullptr) {
    uint8_t targetAddressType = addressTypeOverride != nullptr ? *addressTypeOverride : BLE_ADDR_PUBLIC;
    if (addressTypeOverride == nullptr) {
        if (ScanRecord* record = findScannedDevice(address); record != nullptr) {
            targetAddressType = record->addressType;
        }
    }
    selectAddressTarget(address, targetAddressType);
    suppressIdleScans();
    if (!pairWithDevice(error, true, DEFAULT_RECONNECT_DELAY_MS)) {
        return false;
    }
    if (!fetchDeviceDetails(error)) {
        return false;
    }
    if (cachedDetails.serial.isEmpty()) {
        error = "serial number is missing";
        return false;
    }
    populateSavedMachine(machineOut, "", address, selectedAddressType, cachedDetails);
    refreshSavedMachinePresence(machineOut);
    String disconnectError;
    disconnectFromDevice(disconnectError);
    return true;
}

bool buildManualSavedMachine(const String& alias,
                             const String& address,
                             uint8_t addressType,
                             const String& serial,
                             const String& model,
                             SavedMachine& machineOut,
                             String& error) {
    nivona::DeviceDetails details;
    details.fetched = false;
    details.manufacturer = "NIVONA";
    details.model = model;
    details.serial = serial;

    const nivona::ModelInfo modelInfo = nivona::detectModelInfo(details);
    if (modelInfo.familyKey.isEmpty()) {
        error = "unable to detect a supported coffee machine family from serial/model";
        return false;
    }

    String savedAlias = alias;
    if (savedAlias.isEmpty()) {
        savedAlias = !modelInfo.modelName.isEmpty() ? modelInfo.modelName : serial;
    }

    populateSavedMachine(machineOut, savedAlias, address, addressType, details);
    refreshSavedMachinePresence(machineOut);
    return true;
}

bool appendMyCoffeeSlot(JsonObject target, SavedMachine& machine, uint8_t slotIndex, bool detail, String& error) {
    const nivona::ModelInfo modelInfo = nivona::detectModelInfo(toNivonaDetails(machine));
    nivona::MyCoffeeLayout layout;
    if (!nivona::resolveMyCoffeeLayout(modelInfo, layout)) {
        error = "saved recipes are not supported for this machine";
        return false;
    }
    if (slotIndex >= layout.slotCount) {
        error = "recipe slot is out of range";
        return false;
    }

    const uint16_t baseRegister = nivona::myCoffeeSlotBase(slotIndex);
    target["slot"] = slotIndex + 1;
    target["baseRegister"] = baseRegister;
    target["maxStrengthBeans"] = modelInfo.strengthLevelCount;
    target["maxProfileCode"] = modelInfo.maxProfileCode;

    auto readOptionalNumeric = [&](const char* key,
                                   const char* label,
                                   uint16_t offset,
                                   bool scaleMl,
                                   const String& valueLabel) -> bool {
        if (offset == UINT16_MAX) {
            return true;
        }
        int32_t rawValue = 0;
        if (!readMachineNumericRegister(baseRegister + offset, rawValue, error)) {
            return false;
        }
        target[key] = rawValue;
        if (scaleMl) {
            target[String(key) + "Ml"] = recipeAmountMlValue(layout, rawValue);
        }
        if (!valueLabel.isEmpty()) {
            target[String(key) + "Label"] = valueLabel;
        }
        target[String(key) + "Register"] = baseRegister + offset;
        target[String(key) + "Title"] = label;
        return true;
    };

    int32_t enabled = 0;
    if (!readMachineNumericRegister(baseRegister + layout.enabledOffset, enabled, error)) {
        return false;
    }
    target["enabled"] = enabled;
    target["enabledRegister"] = baseRegister + layout.enabledOffset;

    if (layout.nameOffset != UINT16_MAX) {
        String rawName;
        String displayName;
        if (!readMachineStringRegister(baseRegister + layout.nameOffset, layout.textEncoding, rawName, displayName, error)) {
            return false;
        }
        target["nameRaw"] = rawName;
        target["name"] = displayName;
        target["nameRegister"] = baseRegister + layout.nameOffset;
    }

    int32_t typeSelector = 0;
    if (layout.typeOffset != UINT16_MAX) {
        if (!readMachineNumericRegister(baseRegister + layout.typeOffset, typeSelector, error)) {
            return false;
        }
        target["typeSelector"] = typeSelector;
        target["typeName"] = recipeTypeTitle(modelInfo, typeSelector);
        target["typeRegister"] = baseRegister + layout.typeOffset;
    }

    if (!detail) {
        return true;
    }

    if (!readOptionalNumeric("icon", "Icon", layout.iconOffset, false, "")) {
        return false;
    }
    if (!readOptionalNumeric("strength", "Strength", layout.strengthOffset, false, "")) {
        return false;
    }
    if (target.containsKey("strength")) {
        target["strengthBeans"] = static_cast<int32_t>(target["strength"].as<int32_t>()) + 1;
    }
    if (!readOptionalNumeric("aroma", "Aroma", layout.profileOffset, false, "")) {
        return false;
    }
    if (target.containsKey("aroma")) {
        target["aromaLabel"] = recipeProfileLabel(target["aroma"].as<int32_t>());
    }
    if (!readOptionalNumeric("temperature", "Temperature", layout.temperatureOffset, false, "")) {
        return false;
    }
    if (target.containsKey("temperature")) {
        target["temperatureLabel"] = recipeTemperatureLabel(target["temperature"].as<int32_t>());
    }
    if (!readOptionalNumeric("coffeeTemperature", "Coffee temperature", layout.coffeeTemperatureOffset, false, "")) {
        return false;
    }
    if (target.containsKey("coffeeTemperature")) {
        target["coffeeTemperatureLabel"] = recipeTemperatureLabel(target["coffeeTemperature"].as<int32_t>());
    }
    if (!readOptionalNumeric("waterTemperature", "Water temperature", layout.waterTemperatureOffset, false, "")) {
        return false;
    }
    if (target.containsKey("waterTemperature")) {
        target["waterTemperatureLabel"] = recipeTemperatureLabel(target["waterTemperature"].as<int32_t>());
    }
    if (!readOptionalNumeric("milkTemperature", "Milk temperature", layout.milkTemperatureOffset, false, "")) {
        return false;
    }
    if (!readOptionalNumeric("milkFoamTemperature", "Milk foam temperature", layout.milkFoamTemperatureOffset, false, "")) {
        return false;
    }
    if (!readOptionalNumeric("overallTemperature", "Overall temperature", layout.overallTemperatureOffset, false, "")) {
        return false;
    }
    if (!readOptionalNumeric("twoCups", "Two cups", layout.twoCupsOffset, false, "")) {
        return false;
    }
    if (!readOptionalNumeric("preparation", "Preparation", layout.preparationOffset, false, "")) {
        return false;
    }
    if (!readOptionalNumeric("coffeeAmount", "Coffee amount", layout.coffeeAmountOffset, true, "")) {
        return false;
    }
    if (!readOptionalNumeric("waterAmount", "Water amount", layout.waterAmountOffset, true, "")) {
        return false;
    }
    if (!readOptionalNumeric("milkAmount", "Milk amount", layout.milkAmountOffset, true, "")) {
        return false;
    }
    if (!readOptionalNumeric("milkFoamAmount", "Milk foam amount", layout.milkFoamAmountOffset, true, "")) {
        return false;
    }
    return true;
}

bool appendStandardRecipe(JsonObject target, SavedMachine& machine, uint8_t selector, bool detail, String& error) {
    const nivona::ModelInfo modelInfo = nivona::detectModelInfo(toNivonaDetails(machine));
    const nivona::StandardRecipeDescriptor* descriptor = nivona::findStandardRecipeBySelector(modelInfo, selector);
    if (descriptor == nullptr) {
        error = "standard recipe selector is not supported for this machine";
        return false;
    }

    nivona::StandardRecipeLayout layout;
    if (!nivona::resolveStandardRecipeLayout(modelInfo, layout)) {
        error = "standard recipe details are not supported for this machine";
        return false;
    }

    uint16_t baseRegister = 0;
    if (!nivona::resolveStandardRecipeBaseRegister(modelInfo, selector, baseRegister)) {
        error = "unable to resolve the standard recipe base register";
        return false;
    }

    target["selector"] = selector;
    target["name"] = descriptor->name;
    target["title"] = descriptor->title;
    target["baseRegister"] = baseRegister;
    target["typeSelector"] = selector;
    target["typeName"] = descriptor->title;
    target["maxStrengthBeans"] = modelInfo.strengthLevelCount;
    target["maxProfileCode"] = modelInfo.maxProfileCode;
    appendStandardRecipeDiscovery(target, modelInfo, layout);

    if (!detail) {
        return true;
    }

    auto readOptionalNumeric = [&](const char* key,
                                   const char* label,
                                   uint16_t offset,
                                   bool scaleMl,
                                   const String& valueLabel) -> bool {
        if (offset == UINT16_MAX) {
            return true;
        }
        int32_t rawValue = 0;
        if (!readMachineNumericRegister(baseRegister + offset, rawValue, error)) {
            return false;
        }
        target[key] = rawValue;
        if (scaleMl) {
            target[String(key) + "Ml"] = recipeAmountMlValue(layout.fluidWriteScale10, rawValue);
        }
        if (!valueLabel.isEmpty()) {
            target[String(key) + "Label"] = valueLabel;
        }
        target[String(key) + "Register"] = baseRegister + offset;
        target[String(key) + "Title"] = label;
        return true;
    };

    if (!readOptionalNumeric("strength", "Strength", layout.strengthOffset, false, "")) {
        return false;
    }
    if (target.containsKey("strength")) {
        target["strengthBeans"] = static_cast<int32_t>(target["strength"].as<int32_t>()) + 1;
    }
    if (!readOptionalNumeric("aroma", "Aroma", layout.profileOffset, false, "")) {
        return false;
    }
    if (target.containsKey("aroma")) {
        target["aromaLabel"] = recipeProfileLabel(target["aroma"].as<int32_t>());
    }
    if (!readOptionalNumeric("temperature", "Temperature", layout.temperatureOffset, false, "")) {
        return false;
    }
    if (target.containsKey("temperature")) {
        target["temperatureLabel"] = recipeTemperatureLabel(target["temperature"].as<int32_t>());
    }
    if (!readOptionalNumeric("coffeeTemperature", "Coffee temperature", layout.coffeeTemperatureOffset, false, "")) {
        return false;
    }
    if (target.containsKey("coffeeTemperature")) {
        target["coffeeTemperatureLabel"] = recipeTemperatureLabel(target["coffeeTemperature"].as<int32_t>());
    }
    if (!readOptionalNumeric("waterTemperature", "Water temperature", layout.waterTemperatureOffset, false, "")) {
        return false;
    }
    if (target.containsKey("waterTemperature")) {
        target["waterTemperatureLabel"] = recipeTemperatureLabel(target["waterTemperature"].as<int32_t>());
    }
    if (!readOptionalNumeric("milkTemperature", "Milk temperature", layout.milkTemperatureOffset, false, "")) {
        return false;
    }
    if (target.containsKey("milkTemperature")) {
        target["milkTemperatureLabel"] = recipeTemperatureLabel(target["milkTemperature"].as<int32_t>());
    }
    if (!readOptionalNumeric("milkFoamTemperature", "Milk foam temperature", layout.milkFoamTemperatureOffset, false, "")) {
        return false;
    }
    if (target.containsKey("milkFoamTemperature")) {
        target["milkFoamTemperatureLabel"] = recipeTemperatureLabel(target["milkFoamTemperature"].as<int32_t>());
    }
    if (!readOptionalNumeric("overallTemperature", "Overall temperature", layout.overallTemperatureOffset, false, "")) {
        return false;
    }
    if (target.containsKey("overallTemperature")) {
        target["overallTemperatureLabel"] = recipeTemperatureLabel(target["overallTemperature"].as<int32_t>());
    }
    if (!readOptionalNumeric("twoCups", "Two cups", layout.twoCupsOffset, false, "")) {
        return false;
    }
    if (!readOptionalNumeric("preparation", "Preparation", layout.preparationOffset, false, "")) {
        return false;
    }
    if (!readOptionalNumeric("coffeeAmount", "Coffee amount", layout.coffeeAmountOffset, true, "")) {
        return false;
    }
    if (!readOptionalNumeric("waterAmount", "Water amount", layout.waterAmountOffset, true, "")) {
        return false;
    }
    if (!readOptionalNumeric("milkAmount", "Milk amount", layout.milkAmountOffset, true, "")) {
        return false;
    }
    if (!readOptionalNumeric("milkFoamAmount", "Milk foam amount", layout.milkFoamAmountOffset, true, "")) {
        return false;
    }
    return true;
}

bool applyStandardRecipeOverrides(JsonObject recipe,
                                  JsonVariantConst request,
                                  const nivona::ModelInfo& modelInfo,
                                  const nivona::StandardRecipeLayout& layout,
                                  String& error) {
    auto setNumericField = [&](const char* key, int32_t value) {
        recipe[key] = value;
    };
    const uint8_t maxStrengthBeans = modelInfo.strengthLevelCount > 0 ? modelInfo.strengthLevelCount : 5;
    const uint8_t maxProfileCode = modelInfo.maxProfileCode <= 4 ? modelInfo.maxProfileCode : 4;

    if (request["strength"].is<int>() || request["strength"].is<long>() || request["strength"].is<float>() ||
        request["strength"].is<double>()) {
        const int32_t strengthCode = request["strength"].as<int32_t>();
        if (strengthCode < 0 || strengthCode >= maxStrengthBeans) {
            error = String("strength must be between 0 and ") + (maxStrengthBeans - 1) + " for this machine";
            return false;
        }
        setNumericField("strength", strengthCode);
        recipe["strengthBeans"] = strengthCode + 1;
    }
    if (request["strengthBeans"].is<int>() || request["strengthBeans"].is<long>() || request["strengthBeans"].is<float>() ||
        request["strengthBeans"].is<double>()) {
        const int32_t beans = request["strengthBeans"].as<int32_t>();
        if (beans <= 0 || beans > maxStrengthBeans) {
            error = String("strengthBeans must be between 1 and ") + maxStrengthBeans + " for this machine";
            return false;
        }
        setNumericField("strength", beans - 1);
        recipe["strengthBeans"] = beans;
    }

    int32_t code = 0;
    if (!request["aroma"].isNull()) {
        if (!parseRecipeProfileCode(request["aroma"], code)) {
            error = "unsupported aroma value";
            return false;
        }
        if (code < 0 || code > maxProfileCode) {
            error = "aroma is not supported for this machine";
            return false;
        }
        setNumericField("aroma", code);
        recipe["aroma"] = code;
        recipe["aromaLabel"] = recipeProfileLabel(code);
    }

    auto applyTemperature = [&](const char* requestKey, const char* fieldKey, const char* labelKey) -> bool {
        if (request[requestKey].isNull()) {
            return true;
        }
        if (!parseRecipeTemperatureCode(request[requestKey], code)) {
            error = String("unsupported ") + requestKey + " value";
            return false;
        }
        setNumericField(fieldKey, code);
        recipe[labelKey] = recipeTemperatureLabel(code);
        return true;
    };

    if (!applyTemperature("temperature", "temperature", "temperatureLabel") ||
        !applyTemperature("coffeeTemperature", "coffeeTemperature", "coffeeTemperatureLabel") ||
        !applyTemperature("waterTemperature", "waterTemperature", "waterTemperatureLabel") ||
        !applyTemperature("milkTemperature", "milkTemperature", "milkTemperatureLabel") ||
        !applyTemperature("milkFoamTemperature", "milkFoamTemperature", "milkFoamTemperatureLabel") ||
        !applyTemperature("overallTemperature", "overallTemperature", "overallTemperatureLabel")) {
        return false;
    }

    if (!request["preparation"].isNull()) {
        if (!(request["preparation"].is<int>() || request["preparation"].is<long>() || request["preparation"].is<float>() ||
              request["preparation"].is<double>())) {
            error = "preparation must be numeric";
            return false;
        }
        setNumericField("preparation", request["preparation"].as<int32_t>());
    }

    if (!request["twoCups"].isNull()) {
        if (!parseRecipeBooleanCode(request["twoCups"], code)) {
            error = "twoCups must be on/off, true/false, or numeric";
            return false;
        }
        setNumericField("twoCups", code);
    }

    auto applyAmount = [&](const char* requestKey, const char* fieldKey) -> bool {
        if (request[requestKey].isNull()) {
            return true;
        }
        if (!(request[requestKey].is<int>() || request[requestKey].is<long>() || request[requestKey].is<float>() ||
              request[requestKey].is<double>())) {
            error = String(requestKey) + " must be numeric";
            return false;
        }
        const float value = request[requestKey].as<float>();
        if (value < 0.0f) {
            error = String(requestKey) + " must be non-negative";
            return false;
        }
        recipe[fieldKey] = value;
        return true;
    };

    if (!applyAmount("coffeeAmountMl", "coffeeAmountMl") ||
        !applyAmount("waterAmountMl", "waterAmountMl") ||
        !applyAmount("milkAmountMl", "milkAmountMl") ||
        !applyAmount("milkFoamAmountMl", "milkFoamAmountMl")) {
        return false;
    }

    if (!request["sizeMl"].isNull()) {
        if (!(request["sizeMl"].is<int>() || request["sizeMl"].is<long>() || request["sizeMl"].is<float>() ||
              request["sizeMl"].is<double>())) {
            error = "sizeMl must be numeric";
            return false;
        }
        const float sizeMl = request["sizeMl"].as<float>();
        if (sizeMl < 0.0f) {
            error = "sizeMl must be non-negative";
            return false;
        }
        if (layout.coffeeAmountOffset != UINT16_MAX) {
            recipe["coffeeAmountMl"] = sizeMl;
        } else if (layout.waterAmountOffset != UINT16_MAX) {
            recipe["waterAmountMl"] = sizeMl;
        }
    }

    return true;
}

bool uploadTemporaryStandardRecipe(const nivona::StandardRecipeLayout& layout,
                                   JsonObjectConst recipe,
                                   uint8_t selector,
                                   String& error) {
    auto writeOptionalNumeric = [&](const char* key, uint16_t offset, bool scaleMl) -> bool {
        if (offset == UINT16_MAX || !recipe.containsKey(key)) {
            return true;
        }
        int32_t numericValue = 0;
        if (scaleMl) {
            numericValue = layout.fluidWriteScale10
                ? static_cast<int32_t>(recipe[key].as<float>() * 10.0f)
                : static_cast<int32_t>(recipe[key].as<float>());
        } else {
            numericValue = recipe[key].as<int32_t>();
        }
        return writeMachineNumericRegister(static_cast<uint16_t>(TEMP_RECIPE_TYPE_REGISTER + offset), numericValue, error);
    };

    if (!writeOptionalNumeric("strength", layout.strengthOffset, false) ||
        !writeOptionalNumeric("aroma", layout.profileOffset, false) ||
        !writeOptionalNumeric("temperature", layout.temperatureOffset, false) ||
        !writeOptionalNumeric("preparation", layout.preparationOffset, false) ||
        !writeOptionalNumeric("twoCups", layout.twoCupsOffset, false) ||
        !writeOptionalNumeric("coffeeTemperature", layout.coffeeTemperatureOffset, false) ||
        !writeOptionalNumeric("waterTemperature", layout.waterTemperatureOffset, false) ||
        !writeOptionalNumeric("milkTemperature", layout.milkTemperatureOffset, false) ||
        !writeOptionalNumeric("milkFoamTemperature", layout.milkFoamTemperatureOffset, false) ||
        !writeOptionalNumeric("overallTemperature", layout.overallTemperatureOffset, false) ||
        !writeOptionalNumeric("coffeeAmountMl", layout.coffeeAmountOffset, true) ||
        !writeOptionalNumeric("waterAmountMl", layout.waterAmountOffset, true) ||
        !writeOptionalNumeric("milkAmountMl", layout.milkAmountOffset, true) ||
        !writeOptionalNumeric("milkFoamAmountMl", layout.milkFoamAmountOffset, true)) {
        return false;
    }

    return writeMachineNumericRegister(TEMP_RECIPE_TYPE_REGISTER, selector, error);
}

void handleMachinesList() {
    refreshAllSavedMachinePresence();
    DynamicJsonDocument doc(16384);
    doc["ok"] = true;
    JsonArray items = doc.createNestedArray("machines");
    for (const auto& machine : savedMachines) {
        JsonObject item = items.createNestedObject();
        appendSavedMachineJson(item, machine);
    }
    doc["count"] = savedMachines.size();
    doc["lastScanAtMs"] = lastScanAtMs;
    sendJson(doc);
}

void handleMachineProbe() {
    DynamicJsonDocument request(2048);
    String error;
    if (!parseJsonBody(request, error)) {
        sendError(400, error);
        return;
    }
    String address = request["address"] | "";
    address.trim();
    if (address.isEmpty()) {
        sendError(400, "address is required");
        return;
    }
    uint8_t addressType = BLE_ADDR_PUBLIC;
    const JsonVariantConst addressTypeValue = request["addressType"];
    const uint8_t* addressTypeOverride = nullptr;
    if (!addressTypeValue.isNull()) {
        if (!parseAddressTypeRequest(addressTypeValue, addressType)) {
            sendError(400, "addressType must be public, random, 0, or 1");
            return;
        }
        addressTypeOverride = &addressType;
    }

    SavedMachine preview;
    if (!probeMachineAddress(address, preview, error, addressTypeOverride)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    DynamicJsonDocument response(4096);
    response["ok"] = true;
    JsonObject machine = response.createNestedObject("machine");
    appendSavedMachineJson(machine, preview);
    appendStatus(response);
    sendJson(response);
}

void handleMachinesCreate() {
    DynamicJsonDocument request(4096);
    String error;
    if (!parseJsonBody(request, error)) {
        sendError(400, error);
        return;
    }
    String address = request["address"] | "";
    address.trim();
    String alias = request["alias"] | "";
    if (address.isEmpty()) {
        sendError(400, "address is required");
        return;
    }
    uint8_t addressType = BLE_ADDR_PUBLIC;
    const JsonVariantConst addressTypeValue = request["addressType"];
    const uint8_t* addressTypeOverride = nullptr;
    if (!addressTypeValue.isNull()) {
        if (!parseAddressTypeRequest(addressTypeValue, addressType)) {
            sendError(400, "addressType must be public, random, 0, or 1");
            return;
        }
        addressTypeOverride = &addressType;
    }

    SavedMachine preview;
    if (!probeMachineAddress(address, preview, error, addressTypeOverride)) {
        lastError = error;
        sendError(500, error);
        return;
    }
    if (alias.isEmpty()) {
        alias = !preview.modelName.isEmpty() ? preview.modelName : preview.serial;
    }

    SavedMachine* existing = findSavedMachineBySerial(preview.serial);
    if (existing == nullptr) {
        preview.alias = alias;
        preview.savedAtMs = millis();
        savedMachines.push_back(preview);
        existing = &savedMachines.back();
    } else {
        populateSavedMachine(*existing, alias, preview.address, preview.addressType, cachedDetails);
        existing->lastSeenAtMs = preview.lastSeenAtMs;
        existing->lastSeenRssi = preview.lastSeenRssi;
    }

    syncProtocolSessionTarget(*existing);
    persistSavedMachines();

    DynamicJsonDocument response(4096);
    response["ok"] = true;
    JsonObject machine = response.createNestedObject("machine");
    appendSavedMachineJson(machine, *existing);
    appendStatus(response);
    sendJson(response);
}

void handleMachinesManualCreate() {
    DynamicJsonDocument request(4096);
    String error;
    if (!parseJsonBody(request, error)) {
        sendError(400, error);
        return;
    }

    String address = request["address"] | "";
    if (!normalizeBleAddress(address)) {
        sendError(400, "address must be a BLE MAC like C8:B4:17:D8:A3:8C");
        return;
    }

    uint8_t addressType = BLE_ADDR_PUBLIC;
    if (!parseAddressTypeRequest(request["addressType"], addressType)) {
        sendError(400, "addressType must be public, random, 0, or 1");
        return;
    }

    String serial = request["serial"] | "";
    serial.trim();
    if (serial.isEmpty()) {
        sendError(400, "serial is required");
        return;
    }

    String alias = request["alias"] | "";
    alias.trim();
    String model = request["model"] | "";
    model.trim();

    SavedMachine manualMachine;
    if (!buildManualSavedMachine(alias, address, addressType, serial, model, manualMachine, error)) {
        sendError(400, error);
        return;
    }

    SavedMachine* existing = findSavedMachineBySerial(manualMachine.serial);
    if (existing == nullptr) {
        savedMachines.push_back(manualMachine);
        existing = &savedMachines.back();
    } else {
        *existing = manualMachine;
    }

    syncProtocolSessionTarget(*existing);
    persistSavedMachines();

    DynamicJsonDocument response(4096);
    response["ok"] = true;
    JsonObject machine = response.createNestedObject("machine");
    appendSavedMachineJson(machine, *existing);
    appendStatus(response);
    sendJson(response);
}

void handleMachinesReset() {
    savedMachines.clear();
    clearStoredSessionKey();
    clearAllStandardRecipeCaches();
    clearAllSavedRecipeCaches();
    machinePreferences.remove(PREFS_MACHINE_STORE);
    machinePreferences.putUInt(PREFS_MACHINE_SCHEMA, 1);
    DynamicJsonDocument doc(1024);
    doc["ok"] = true;
    doc["count"] = 0;
    appendStatus(doc);
    sendJson(doc);
}

void handleMachineSummary(const String& serial) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }

    String error;
    nivona::ProcessStatus processStatus;
    String processError;
    bool liveSession = beginMachineProtocolSession(*machine, error);
    if (liveSession) {
        readMachineProcessStatus(processStatus, processError);
    } else {
        lastError = error;
        processStatus.ok = false;
        processStatus.summary = machine->lastSeenAtMs > 0 ? "unavailable" : "offline";
        processStatus.process = 0;
        processStatus.subProcess = 0;
        processStatus.message = 0;
        processStatus.progress = 0;
        processError = error;
    }

    DynamicJsonDocument response(8192);
    response["ok"] = true;
    JsonObject item = response.createNestedObject("machine");
    appendSavedMachineJson(item, *machine);
    JsonObject details = response.createNestedObject("details");
    const DeviceDetails detailsSource = liveSession ? cachedDetails : toNivonaDetails(*machine);
    details["manufacturer"] = detailsSource.manufacturer;
    details["model"] = detailsSource.model;
    details["serial"] = detailsSource.serial;
    details["hardwareRevision"] = detailsSource.hardwareRevision;
    details["firmwareRevision"] = detailsSource.firmwareRevision;
    details["softwareRevision"] = detailsSource.softwareRevision;
    details["ad06Hex"] = detailsSource.ad06Hex;
    details["ad06Ascii"] = detailsSource.ad06Ascii;
    JsonObject status = response.createNestedObject("status");
    appendProcessStatusJson(status, processStatus, processError);
    JsonObject protocolSession = response.createNestedObject("protocolSession");
    appendProtocolSessionJson(protocolSession, resolveStoredSessionEntry(machine->serial, machine->address));
    appendStatus(response);
    sendJson(response);
}

void handleMachineRecipes(const String& serial) {
    const SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }
    const nivona::ModelInfo modelInfo = nivona::detectModelInfo(toNivonaDetails(*machine));
    std::vector<const nivona::StandardRecipeDescriptor*> recipes;
    nivona::selectStandardRecipes(modelInfo, recipes);

    DynamicJsonDocument response(8192);
    response["ok"] = true;
    JsonObject machineJson = response.createNestedObject("machine");
    appendSavedMachineJson(machineJson, *machine);
    response["savedRecipeSlots"] = modelInfo.myCoffeeSlotCount;
    JsonArray items = response.createNestedArray("recipes");
    for (const auto* recipe : recipes) {
        JsonObject item = items.createNestedObject();
        item["selector"] = recipe->selector;
        item["name"] = recipe->name;
        item["title"] = recipe->title;
        uint16_t baseRegister = 0;
        if (nivona::resolveStandardRecipeBaseRegister(modelInfo, recipe->selector, baseRegister)) {
            item["baseRegister"] = baseRegister;
        }
        item["detailsSupported"] = true;
    }
    sendJson(response);
}

void handleMachineRecipesRefresh(const String& serial) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }

    const nivona::ModelInfo modelInfo = nivona::detectModelInfo(toNivonaDetails(*machine));
    std::vector<const nivona::StandardRecipeDescriptor*> recipes;
    nivona::selectStandardRecipes(modelInfo, recipes);

    String error;
    if (!beginMachineProtocolSession(*machine, error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    DynamicJsonDocument response(65536);
    response["ok"] = true;
    response["source"] = "live";
    response["cached"] = false;
    JsonArray items = response.createNestedArray("recipes");
    for (const auto* recipeDescriptor : recipes) {
        JsonObject item = items.createNestedObject();
        if (!appendStandardRecipe(item, *machine, recipeDescriptor->selector, true, error)) {
            lastError = error;
            sendError(500, error);
            return;
        }

        const JsonObjectConst recipeView = item;
        String cacheWriteError;
        if (!persistStandardRecipeCache(*machine, recipeDescriptor->selector, recipeView, cacheWriteError)) {
            addLog("cache", String("Standard recipe cache write skipped: ") + cacheWriteError);
        }
    }
    response["refreshedCount"] = items.size();
    sendJson(response);
}

void handleMachineRecipeDetail(const String& serial, const String& selectorText) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }

    const int selectorValue = selectorText.toInt();
    if (selectorValue < 0 || selectorValue > 255) {
        sendError(400, "selector must be between 0 and 255");
        return;
    }

    const bool forceRefresh = parseRefreshArg();
    if (!forceRefresh) {
        DynamicJsonDocument cachedResponse(16384);
        String cacheError;
        if (loadStandardRecipeCache(*machine, static_cast<uint8_t>(selectorValue), cachedResponse, cacheError)) {
            DynamicJsonDocument response(16384);
            response["ok"] = true;
            response["source"] = "cache";
            response["cached"] = true;
            JsonObject recipe = response.createNestedObject("recipe");
            recipe.set(cachedResponse["recipe"].as<JsonObject>());
            sendJson(response);
            return;
        }
    }

    String error;
    if (!beginMachineProtocolSession(*machine, error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    DynamicJsonDocument response(16384);
    response["ok"] = true;
    response["source"] = "live";
    response["cached"] = false;
    JsonObject recipe = response.createNestedObject("recipe");
    if (!appendStandardRecipe(recipe, *machine, static_cast<uint8_t>(selectorValue), true, error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    String cacheWriteError;
    const JsonObjectConst recipeView = recipe;
    if (!persistStandardRecipeCache(*machine, static_cast<uint8_t>(selectorValue), recipeView, cacheWriteError)) {
        addLog("cache", String("Standard recipe cache write skipped: ") + cacheWriteError);
    }
    sendJson(response);
}

void handleMachineBrew(const String& serial) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }

    DynamicJsonDocument request(4096);
    String error;
    if (!parseJsonBody(request, error)) {
        sendError(400, error);
        return;
    }
    const int selector = request["selector"] | -1;
    if (selector < 0) {
        sendError(400, "selector is required");
        return;
    }

    if (!beginMachineProtocolSession(*machine, error)) {
        lastError = error;
        sendError(500, error);
        return;
    }
    if (!ensureMachineHuSession(2500, error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    const nivona::ModelInfo modelInfo = nivona::detectModelInfo(toNivonaDetails(*machine));
    nivona::StandardRecipeLayout layout;
    if (!nivona::resolveStandardRecipeLayout(modelInfo, layout)) {
        sendError(400, "standard recipe overrides are not supported for this machine");
        return;
    }

    DynamicJsonDocument recipeDoc(12288);
    JsonObject recipe = recipeDoc.createNestedObject("recipe");
    if (!appendStandardRecipe(recipe, *machine, static_cast<uint8_t>(selector), true, error)) {
        lastError = error;
        sendError(500, error);
        return;
    }
    if (!applyStandardRecipeOverrides(recipe, request.as<JsonVariantConst>(), modelInfo, layout, error)) {
        sendError(400, error);
        return;
    }
    JsonObjectConst recipeView = recipe;
    if (!uploadTemporaryStandardRecipe(layout, recipeView, static_cast<uint8_t>(selector), error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    const ByteVector* sessionKey = resolveStoredSessionIfAvailable();
    if (!sendMachineCommand("HE",
                            nivona::buildHeMakeCoffeePayload(modelInfo, static_cast<uint8_t>(selector)),
                            sessionKey,
                            true,
                            error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    nivona::ProcessStatus processStatus;
    String processError;
    readMachineProcessStatus(processStatus, processError);

    DynamicJsonDocument response(4096);
    response["ok"] = true;
    response["selector"] = selector;
    response["temporaryRecipeUploaded"] = true;
    JsonObject recipeResponse = response.createNestedObject("recipe");
    recipeResponse.set(recipe);
    JsonObject status = response.createNestedObject("status");
    appendProcessStatusJson(status, processStatus, processError);
    appendStatus(response);
    sendJson(response);
}

void handleMachineConfirm(const String& serial) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }

    String error;
    if (!beginMachineProtocolSession(*machine, error, false)) {
        lastError = error;
        sendError(500, error);
        return;
    }
    if (!ensureMachineHuSession(2500, error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    const ByteVector* sessionKey = resolveStoredSessionIfAvailable();
    if (!sendMachineCommand("HY", nivona::buildHyConfirmPayload(), sessionKey, true, error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    nivona::ProcessStatus processStatus;
    String processError;
    readMachineProcessStatus(processStatus, processError);

    DynamicJsonDocument response(4096);
    response["ok"] = true;
    response["command"] = "HY";
    JsonObject machineJson = response.createNestedObject("machine");
    appendSavedMachineJson(machineJson, *machine);
    JsonObject status = response.createNestedObject("status");
    appendProcessStatusJson(status, processStatus, processError);
    JsonObject protocolSession = response.createNestedObject("protocolSession");
    appendProtocolSessionJson(protocolSession, resolveStoredSessionEntry(machine->serial, machine->address));
    appendStatus(response);
    sendJson(response);
}

void handleMachineMyCoffeeList(const String& serial) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }

    const nivona::ModelInfo modelInfo = nivona::detectModelInfo(toNivonaDetails(*machine));
    nivona::MyCoffeeLayout layout;
    if (!nivona::resolveMyCoffeeLayout(modelInfo, layout)) {
        sendError(400, "saved recipes are not supported for this machine");
        return;
    }

    const bool forceRefresh = parseRefreshArg();
    if (!forceRefresh) {
        DynamicJsonDocument cachedResponse(65536);
        String cacheError;
        if (loadSavedRecipeCache(*machine, cachedResponse, cacheError)) {
            DynamicJsonDocument response(65536);
            response["ok"] = true;
            response["source"] = "cache";
            response["cached"] = true;
            JsonArray items = response.createNestedArray("recipes");
            items.set(cachedResponse["recipes"].as<JsonArray>());
            sendJson(response);
            return;
        }
    }

    String error;
    if (!beginMachineProtocolSession(*machine, error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    DynamicJsonDocument response(65536);
    response["ok"] = true;
    response["source"] = "live";
    response["cached"] = false;
    JsonArray items = response.createNestedArray("recipes");
    for (uint8_t slot = 0; slot < layout.slotCount; ++slot) {
        JsonObject item = items.createNestedObject();
        if (!appendMyCoffeeSlot(item, *machine, slot, true, error)) {
            item["ok"] = false;
            item["error"] = error;
            error = "";
            continue;
        }
        item["ok"] = true;
    }

    String cacheWriteError;
    if (!persistSavedRecipeCache(*machine, items, cacheWriteError)) {
        addLog("cache", String("Saved recipe cache write skipped: ") + cacheWriteError);
    }
    sendJson(response);
}

void handleMachineMyCoffeeDetail(const String& serial, const String& slotText, bool isUpdate) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }
    const int slotNumber = slotText.toInt();
    if (slotNumber <= 0) {
        sendError(400, "slot must be a positive integer");
        return;
    }

    const nivona::ModelInfo modelInfo = nivona::detectModelInfo(toNivonaDetails(*machine));
    nivona::MyCoffeeLayout layout;
    if (!nivona::resolveMyCoffeeLayout(modelInfo, layout)) {
        sendError(400, "saved recipes are not supported for this machine");
        return;
    }
    const uint8_t slotIndex = static_cast<uint8_t>(slotNumber - 1);
    if (slotIndex >= layout.slotCount) {
        sendError(400, "slot is out of range");
        return;
    }

    const bool forceRefresh = !isUpdate && parseRefreshArg();
    if (!isUpdate && !forceRefresh) {
        DynamicJsonDocument cachedResponse(65536);
        String cacheError;
        if (loadSavedRecipeCache(*machine, cachedResponse, cacheError)) {
            JsonArray cachedRecipes = cachedResponse["recipes"].as<JsonArray>();
            for (JsonVariant recipeVariant : cachedRecipes) {
                JsonObject cachedRecipe = recipeVariant.as<JsonObject>();
                if ((cachedRecipe["slot"] | 0) != slotNumber || !(cachedRecipe["ok"] | true)) {
                    continue;
                }

                DynamicJsonDocument response(16384);
                response["ok"] = true;
                response["source"] = "cache";
                response["cached"] = true;
                JsonObject item = response.createNestedObject("recipe");
                item.set(cachedRecipe);
                sendJson(response);
                return;
            }
        }
    }

    String error;
    if (!beginMachineProtocolSession(*machine, error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    if (isUpdate) {
        DynamicJsonDocument request(4096);
        if (!parseJsonBody(request, error)) {
            sendError(400, error);
            return;
        }
        const uint8_t maxStrengthBeans = modelInfo.strengthLevelCount > 0 ? modelInfo.strengthLevelCount : 5;
        const uint8_t maxProfileCode = modelInfo.maxProfileCode <= 4 ? modelInfo.maxProfileCode : 4;
        if (request.containsKey("strengthBeans")) {
            const int32_t beans = request["strengthBeans"].as<int32_t>();
            if (beans <= 0 || beans > maxStrengthBeans) {
                sendError(400, String("strengthBeans must be between 1 and ") + maxStrengthBeans + " for this machine");
                return;
            }
            request["strength"] = beans - 1;
        }
        if (request.containsKey("strength")) {
            const int32_t strengthCode = request["strength"].as<int32_t>();
            if (strengthCode < 0 || strengthCode >= maxStrengthBeans) {
                sendError(400, String("strength must be between 0 and ") + (maxStrengthBeans - 1) + " for this machine");
                return;
            }
        }
        if (request.containsKey("aroma")) {
            int32_t profileCode = 0;
            if (!parseRecipeProfileCode(request["aroma"], profileCode)) {
                sendError(400, "unsupported aroma value");
                return;
            }
            if (profileCode < 0 || profileCode > maxProfileCode) {
                sendError(400, "aroma is not supported for this machine");
                return;
            }
            request["aroma"] = profileCode;
        }
        const uint16_t baseRegister = nivona::myCoffeeSlotBase(slotIndex);
        if (request.containsKey("name") && layout.nameOffset != UINT16_MAX) {
            if (!writeMachineStringRegister(baseRegister + layout.nameOffset, layout.textEncoding, request["name"].as<String>(), error)) {
                lastError = error;
                sendError(500, error);
                return;
            }
        }
        auto applyNumericField = [&](const char* key, uint16_t offset, bool scaleMl) -> bool {
            if (!request.containsKey(key) || offset == UINT16_MAX) {
                return true;
            }
            float inputValue = request[key].as<float>();
            int32_t numericValue = scaleMl && layout.fluidWriteScale10 ? static_cast<int32_t>(inputValue * 10.0f) : static_cast<int32_t>(inputValue);
            return writeMachineNumericRegister(baseRegister + offset, numericValue, error);
        };

        if (!applyNumericField("icon", layout.iconOffset, false) ||
            !applyNumericField("typeSelector", layout.typeOffset, false) ||
            !applyNumericField("strength", layout.strengthOffset, false) ||
            !applyNumericField("aroma", layout.profileOffset, false) ||
            !applyNumericField("temperature", layout.temperatureOffset, false) ||
            !applyNumericField("coffeeTemperature", layout.coffeeTemperatureOffset, false) ||
            !applyNumericField("waterTemperature", layout.waterTemperatureOffset, false) ||
            !applyNumericField("milkTemperature", layout.milkTemperatureOffset, false) ||
            !applyNumericField("milkFoamTemperature", layout.milkFoamTemperatureOffset, false) ||
            !applyNumericField("overallTemperature", layout.overallTemperatureOffset, false) ||
            !applyNumericField("twoCups", layout.twoCupsOffset, false) ||
            !applyNumericField("preparation", layout.preparationOffset, false) ||
            !applyNumericField("coffeeAmountMl", layout.coffeeAmountOffset, true) ||
            !applyNumericField("waterAmountMl", layout.waterAmountOffset, true) ||
            !applyNumericField("milkAmountMl", layout.milkAmountOffset, true) ||
            !applyNumericField("milkFoamAmountMl", layout.milkFoamAmountOffset, true)) {
            lastError = error;
            sendError(500, error);
            return;
        }
    }

    DynamicJsonDocument response(16384);
    response["ok"] = true;
    response["source"] = "live";
    response["cached"] = false;
    JsonObject item = response.createNestedObject("recipe");
    if (!appendMyCoffeeSlot(item, *machine, slotIndex, true, error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    const JsonObjectConst recipeView = item;
    String cacheWriteError;
    if (!upsertSavedRecipeCacheEntry(*machine, recipeView, cacheWriteError)) {
        addLog("cache", String("Saved recipe cache write skipped: ") + cacheWriteError);
    }
    sendJson(response);
}

void handleMachineStats(const String& serial) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }
    const nivona::ModelInfo modelInfo = nivona::detectModelInfo(toNivonaDetails(*machine));
    std::vector<const nivona::RegisterProbe*> metrics;
    nivona::selectStatsDescriptors(modelInfo, metrics);
    if (metrics.empty()) {
        DynamicJsonDocument response(4096);
        response["ok"] = true;
        response["supported"] = false;
        JsonObject machineJson = response.createNestedObject("machine");
        appendSavedMachineJson(machineJson, *machine);
        sendJson(response);
        return;
    }

    String error;
    if (!selectSavedMachine(*machine, error)) {
        sendError(400, error);
        return;
    }

    DynamicJsonDocument response(32768);
    if (!runStatsFlowProbe(3000, true, true, DEFAULT_RECONNECT_DELAY_MS, "notify", true, response, error)) {
        lastError = error;
        response["error"] = error;
        sendJson(response, 500);
        return;
    }
    response["supported"] = true;
    sendJson(response);
}

void handleMachineSettingsGet(const String& serial) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }
    String error;
    if (!selectSavedMachine(*machine, error)) {
        sendError(400, error);
        return;
    }

    DynamicJsonDocument response(65536);
    if (!runSettingsFlowProbe(3000,
                              true,
                              true,
                              DEFAULT_RECONNECT_DELAY_MS,
                              "notify",
                              true,
                              nivona::SettingsFamily::Unknown,
                              response,
                              error)) {
        lastError = error;
        response["error"] = error;
        sendJson(response, 500);
        return;
    }
    response["supported"] = true;
    sendJson(response);
}

void handleMachineSettingsPost(const String& serial) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }

    DynamicJsonDocument request(4096);
    String error;
    if (!parseJsonBody(request, error)) {
        sendError(400, error);
        return;
    }

    if (!beginMachineProtocolSession(*machine, error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    const String action = request["action"] | "";
    if (!action.isEmpty()) {
        uint16_t commandId = 0;
        if (action == "factory_reset_settings") {
            commandId = nivona::HE_FACTORY_RESET_SETTINGS;
        } else if (action == "factory_reset_recipes") {
            commandId = nivona::HE_FACTORY_RESET_RECIPES;
        } else {
            sendError(400, "unsupported settings action");
            return;
        }

        if (!ensureMachineHuSession(2500, error)) {
            lastError = error;
            sendError(500, error);
            return;
        }

        const ByteVector* sessionKey = resolveStoredSessionIfAvailable();
        if (!sendMachineCommand("HE", nivona::buildHeCommandPayload(commandId), sessionKey, true, error)) {
            lastError = error;
            sendError(500, error);
            return;
        }

        nivona::ProcessStatus processStatus;
        String processError;
        readMachineProcessStatus(processStatus, processError);

        DynamicJsonDocument response(4096);
        response["ok"] = true;
        response["action"] = action;
        response["commandId"] = commandId;
        JsonObject status = response.createNestedObject("status");
        appendProcessStatusJson(status, processStatus, processError);
        sendJson(response);
        return;
    }

    nivona::SettingsProbeContext context;
    if (!nivona::resolveSettingsProbeContext(toNivonaDetails(*machine), nivona::SettingsFamily::Unknown, context, error)) {
        sendError(400, error);
        return;
    }

    const String key = request["key"] | "";
    if (key.isEmpty()) {
        sendError(400, "key is required");
        return;
    }
    const nivona::SettingProbeDescriptor* probe = nivona::findSettingDescriptorByKey(context, key);
    if (probe == nullptr) {
        sendError(400, "setting is not supported for this machine");
        return;
    }

    uint16_t code = 0;
    if (request.containsKey("code")) {
        code = static_cast<uint16_t>(request["code"].as<int>());
    } else if (request.containsKey("value")) {
        if (!nivona::findSettingValueCode(*probe, request["value"].as<String>(), code)) {
            sendError(400, "unsupported setting value");
            return;
        }
    } else {
        sendError(400, "value or code is required");
        return;
    }

    if (!writeMachineNumericRegister(probe->id, static_cast<int32_t>(code), error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    DynamicJsonDocument response(2048);
    response["ok"] = true;
    response["key"] = key;
    response["code"] = code;
    response["label"] = nivona::findSettingValueLabel(*probe, code);
    sendJson(response);
}

void handleMachineDelete(const String& serial) {
    for (auto it = savedMachines.begin(); it != savedMachines.end(); ++it) {
        if (it->serial.equalsIgnoreCase(serial)) {
            clearStoredSessionKey(it->serial, it->address);
            clearStandardRecipeCachesForMachine(it->serial);
            clearSavedRecipeCachesForMachine(it->serial);
            savedMachines.erase(it);
            persistSavedMachines();
            DynamicJsonDocument response(512);
            response["ok"] = true;
            appendStatus(response);
            sendJson(response);
            return;
        }
    }
    sendError(404, "saved machine not found");
}

bool dispatchMachineApiRoute() {
    String serial;
    String section;
    String tail;
    if (!parseMachineRoute(serial, section, tail)) {
        return false;
    }
    if (server.method() == HTTP_DELETE && section.isEmpty()) {
        handleMachineDelete(serial);
        return true;
    }
    if (section == "summary" && server.method() == HTTP_GET) {
        handleMachineSummary(serial);
        return true;
    }
    if (section == "recipes" && server.method() == HTTP_POST && tail == "refresh") {
        handleMachineRecipesRefresh(serial);
        return true;
    }
    if (section == "recipes" && server.method() == HTTP_GET) {
        if (tail.isEmpty()) {
            handleMachineRecipes(serial);
        } else {
            handleMachineRecipeDetail(serial, tail);
        }
        return true;
    }
    if (section == "brew" && server.method() == HTTP_POST) {
        handleMachineBrew(serial);
        return true;
    }
    if (section == "confirm" && server.method() == HTTP_POST) {
        handleMachineConfirm(serial);
        return true;
    }
    if (section == "mycoffee" && server.method() == HTTP_GET && tail.isEmpty()) {
        handleMachineMyCoffeeList(serial);
        return true;
    }
    if (section == "mycoffee" && !tail.isEmpty()) {
        handleMachineMyCoffeeDetail(serial, tail, server.method() == HTTP_POST);
        return true;
    }
    if (section == "stats" && server.method() == HTTP_GET) {
        handleMachineStats(serial);
        return true;
    }
    if (section == "settings" && server.method() == HTTP_GET) {
        handleMachineSettingsGet(serial);
        return true;
    }
    if (section == "settings" && server.method() == HTTP_POST) {
        handleMachineSettingsPost(serial);
        return true;
    }
    return false;
}

void handleRoot() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", web_ui::kPage);
}

void handleStatus() {
    DynamicJsonDocument doc(4096);
    doc["ok"] = true;
    appendStatus(doc);
    sendJson(doc);
}

void handleDevices() {
    DynamicJsonDocument doc(8192);
    doc["ok"] = true;
    JsonArray devices = doc.createNestedArray("devices");
    for (const auto& record : scannedDevices) {
        JsonObject item               = devices.createNestedObject();
        item["address"]               = record.address;
        item["addressType"]           = record.addressType;
        item["name"]                  = record.name;
        item["rssi"]                  = record.rssi;
        item["connectable"]           = record.connectable;
        item["advertisedSupportedService"] = record.advertisedSupportedService;
        item["likelySupported"]            = record.likelySupported;
        item["seenAtMs"]              = record.seenAtMs;
    }
    sendJson(doc);
}

void handleScan() {
    performScan();
    handleDevices();
}

void handleWifiSave() {
    DynamicJsonDocument request(1024);
    String error;
    if (!parseJsonBody(request, error)) {
        sendError(400, error);
        return;
    }

    const String ssid     = request["ssid"] | "";
    const String password = request["password"] | "";
    if (ssid.isEmpty()) {
        sendError(400, "ssid is required");
        return;
    }

    preferences.putString(PREFS_SSID, ssid);
    preferences.putString(PREFS_PASS, password);
    wifiStaSsid = ssid;
    addLog("wifi", String("Saved STA credentials for ") + ssid);

    WiFi.disconnect(true, false);
    delay(250);
    connectWifi();

    DynamicJsonDocument response(2048);
    response["ok"] = true;
    appendStatus(response);
    sendJson(response);
}

void handleConnect() {
    DynamicJsonDocument request(1024);
    String error;
    if (parseJsonBody(request, error)) {
        const String address = request["address"] | "";
        if (!address.isEmpty()) {
            uint8_t targetAddressType = BLE_ADDR_PUBLIC;
            ScanRecord* record = findScannedDevice(address);
            if (record != nullptr) {
                targetAddressType = record->addressType;
            }
            selectAddressTarget(address, targetAddressType);
        }
    }

    if (selectedAddress.isEmpty()) {
        sendError(400, "address is required");
        return;
    }

    if (!connectToSelectedDevice(error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    lastError = "";
    DynamicJsonDocument response(4096);
    response["ok"] = true;
    appendStatus(response);
    sendJson(response);
}

void handleDisconnect() {
    String error;
    if (!disconnectFromDevice(error)) {
        sendError(400, error);
        return;
    }
    DynamicJsonDocument response(2048);
    response["ok"] = true;
    appendStatus(response);
    sendJson(response);
}

void handlePair() {
    DynamicJsonDocument request(1024);
    String error;
    if (parseJsonBody(request, error)) {
        const String address = request["address"] | "";
        if (!address.isEmpty()) {
            uint8_t targetAddressType = BLE_ADDR_PUBLIC;
            ScanRecord* record = findScannedDevice(address);
            if (record != nullptr) {
                targetAddressType = record->addressType;
            }
            selectAddressTarget(address, targetAddressType);
        }
    }

    if (!pairWithDevice(error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    DynamicJsonDocument response(4096);
    response["ok"] = true;
    appendStatus(response);
    sendJson(response);
}

void handleDetails() {
    String error;
    if (!fetchDeviceDetails(error)) {
        DynamicJsonDocument response(2048);
        response["ok"] = false;
        response["manufacturer"] = "";
        response["model"] = "";
        response["serial"] = "";
        response["hardwareRevision"] = "";
        response["firmwareRevision"] = "";
        response["softwareRevision"] = "";
        response["ad06Hex"] = "";
        response["ad06Ascii"] = "";
        response["error"] = error;
        sendJson(response, 500);
        return;
    }

    DynamicJsonDocument response(4096);
    response["ok"] = true;
    response["manufacturer"] = cachedDetails.manufacturer;
    response["model"] = cachedDetails.model;
    response["serial"] = cachedDetails.serial;
    response["hardwareRevision"] = cachedDetails.hardwareRevision;
    response["firmwareRevision"] = cachedDetails.firmwareRevision;
    response["softwareRevision"] = cachedDetails.softwareRevision;
    response["ad06Hex"] = cachedDetails.ad06Hex;
    response["ad06Ascii"] = cachedDetails.ad06Ascii;
    response["error"] = cachedDetails.lastError;
    sendJson(response);
}

void handleNotifications() {
    DynamicJsonDocument request(1024);
    String error;
    if (!parseJsonBody(request, error)) {
        sendError(400, error);
        return;
    }
    const bool enable = request["enabled"] | false;
    const String mode = request["mode"] | "notify";

    if (!setNotificationsEnabled(enable, error, mode)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    DynamicJsonDocument response(2048);
    response["ok"] = true;
    response["notificationsEnabled"] = notificationsEnabled;
    response["notificationMode"] = notificationsEnabled ? notificationMode : "off";
    sendJson(response);
}

void handleHu() {
    DynamicJsonDocument request(1024);
    String error;
    uint32_t waitMs = 5000;
    if (parseJsonBody(request, error)) {
        waitMs = request["waitMs"] | waitMs;
    }

    if (!notificationsEnabled) {
        sendError(400, "rx notifications must be enabled before HU testing");
        return;
    }

    if (!runHuExperiment(waitMs, error)) {
        lastError = error;
        DynamicJsonDocument response(4096);
        response["ok"] = false;
        response["error"] = error;
        response["seedHex"] = lastHuSeedHex;
        response["requestHex"] = lastHuRequestHex;
        response["responseHex"] = lastHuResponseHex;
        response["parseStatus"] = lastHuParseStatus;
        sendJson(response, 500);
        return;
    }

    DynamicJsonDocument response(4096);
    response["ok"] = true;
    response["seedHex"] = lastHuSeedHex;
    response["requestHex"] = lastHuRequestHex;
    response["responseHex"] = lastHuResponseHex;
    response["parseStatus"] = lastHuParseStatus;
    sendJson(response);
}

void handleSession() {
    DynamicJsonDocument request(2048);
    String error;
    if (!parseJsonBody(request, error)) {
        sendError(400, error);
        return;
    }

    const bool clear = request["clear"] | false;
    const String sessionHex = request["sessionHex"] | "";
    const String source = request["source"] | "manual";
    const String serial = request["serial"] | "";
    String address = request["address"] | "";
    uint8_t addressType = BLE_ADDR_PUBLIC;
    const JsonVariantConst addressTypeValue = request["addressType"];
    if (!addressTypeValue.isNull() && !parseAddressTypeRequest(addressTypeValue, addressType)) {
        sendError(400, "addressType must be public, random, 0, or 1");
        return;
    }
    if (!address.isEmpty() && !normalizeBleAddress(address)) {
        sendError(400, "address must be a BLE MAC like C8:B4:17:D8:A3:8C");
        return;
    }

    if (!serial.isEmpty()) {
        SavedMachine* machine = findSavedMachineBySerial(serial);
        if (machine == nullptr) {
            sendError(404, "saved machine not found");
            return;
        }
        selectMachineTarget(*machine);
        if (address.isEmpty()) {
            address = machine->address;
            addressType = machine->addressType;
        }
    } else if (!address.isEmpty()) {
        if (SavedMachine* machine = findSavedMachineByAddress(address); machine != nullptr) {
            selectMachineTarget(*machine);
        } else {
            selectAddressTarget(address, addressType);
        }
    }

    if (clear) {
        clearStoredSessionKey(serial, address);
    } else {
        ByteVector sessionKey;
        if (!parseSessionHexString(sessionHex, sessionKey, error)) {
            sendError(400, error);
            return;
        }
        if (sessionKey.size() != 2) {
            sendError(400, "sessionHex is required unless clear=true");
            return;
        }
        setStoredSessionKey(sessionKey, source, serial, address, addressType);
    }

    DynamicJsonDocument response(4096);
    response["ok"] = true;
    JsonObject protocolSession = response.createNestedObject("protocolSession");
    appendProtocolSessionJson(protocolSession, resolveStoredSessionEntry(serial, address));
    appendStatus(response);
    sendJson(response);
}

void handleSendFrame() {
    DynamicJsonDocument request(4096);
    String error;
    if (!parseJsonBody(request, error)) {
        sendError(400, error);
        return;
    }

    const String serial = request["serial"] | "";
    String address = request["address"] | "";
    if (!address.isEmpty() && !normalizeBleAddress(address)) {
        sendError(400, "address must be a BLE MAC like C8:B4:17:D8:A3:8C");
        return;
    }
    if (!serial.isEmpty()) {
        SavedMachine* machine = findSavedMachineBySerial(serial);
        if (machine == nullptr) {
            sendError(404, "saved machine not found");
            return;
        }
        selectMachineTarget(*machine);
        if (address.isEmpty()) {
            address = machine->address;
        }
    } else if (!address.isEmpty()) {
        uint8_t targetAddressType = BLE_ADDR_PUBLIC;
        ScanRecord* record = findScannedDevice(address);
        if (record != nullptr) {
            targetAddressType = record->addressType;
        }
        if (SavedMachine* machine = findSavedMachineByAddress(address); machine != nullptr) {
            selectMachineTarget(*machine);
        } else {
            selectAddressTarget(address, targetAddressType);
        }
    }
    if (selectedAddress.isEmpty()) {
        sendError(400, "address is required");
        return;
    }

    char command[3];
    if (!parseCommandString(request["command"] | "", command, error)) {
        sendError(400, error);
        return;
    }

    ByteVector payload;
    const String payloadHex = request["payloadHex"] | "";
    if (!payloadHex.isEmpty() && !hexDecode(payloadHex, payload, error)) {
        sendError(400, error);
        return;
    }

    const bool encrypt = request["encrypt"].isNull() ? true : request["encrypt"].as<bool>();
    const bool pairFirst = request["pair"] | false;
    const bool fetchDetailsFirst = request["details"] | false;
    const bool reconnectAfterPair = request["reconnectAfterPair"] | false;
    const bool chunked = request["chunked"] | false;
    const bool useStoredSession = request["useStoredSession"] | false;
    const bool rememberSession = request["rememberSession"] | false;
    const String notificationModeForRequest = request["notificationMode"] | "notify";
    const uint32_t reconnectDelayMs = request["reconnectDelayMs"] | DEFAULT_RECONNECT_DELAY_MS;
    const uint32_t interChunkDelayMs = request["interChunkDelayMs"] | 0;
    const uint32_t waitMs = request["waitMs"] | 0;

    ByteVector sessionKeyOverride;
    const ByteVector* sessionKey = nullptr;
    String sessionHexUsed;
    if (!resolveSessionKeyForRequest(request["sessionHex"] | "",
                                     useStoredSession,
                                     serial,
                                     address,
                                     sessionKeyOverride,
                                     sessionKey,
                                     sessionHexUsed,
                                     error)) {
        sendError(400, error);
        return;
    }

    if (rememberSession && sessionKey != nullptr) {
        setStoredSessionKey(*sessionKey, "manual-send-frame", serial, address, selectedAddressType);
    }

    if (pairFirst) {
        if (!pairWithDevice(error, reconnectAfterPair, reconnectDelayMs)) {
            lastError = error;
            sendError(500, error);
            return;
        }
    } else if (!connectToSelectedDevice(error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    String detailsError;
    if (fetchDetailsFirst) {
        fetchDeviceDetails(detailsError);
    }

    DynamicJsonDocument response(12288);
    response["ok"] = false;
    response["detailsError"] = detailsError;
    JsonObject result = response.createNestedObject("result");
    if (!runFrameScenario("send_frame",
                          command,
                          payload,
                          sessionKey,
                          encrypt,
                          chunked,
                          interChunkDelayMs,
                          waitMs,
                          notificationModeForRequest,
                          result,
                          error)) {
        lastError = error;
        response["error"] = error;
        response["command"] = command;
        response["sessionHexUsed"] = sessionHexUsed;
        appendStatus(response);
        sendJson(response, 500);
        return;
    }

    response["ok"] = true;
    response["command"] = command;
    response["sessionHexUsed"] = sessionHexUsed;
    appendStatus(response);
    sendJson(response);
}

void handleAppProbe() {
    DynamicJsonDocument request(4096);
    String error;
    if (!parseJsonBody(request, error)) {
        sendError(400, error);
        return;
    }

    const String serial = request["serial"] | "";
    String address = request["address"] | "";
    if (!address.isEmpty() && !normalizeBleAddress(address)) {
        sendError(400, "address must be a BLE MAC like C8:B4:17:D8:A3:8C");
        return;
    }
    if (!serial.isEmpty()) {
        SavedMachine* machine = findSavedMachineBySerial(serial);
        if (machine == nullptr) {
            sendError(404, "saved machine not found");
            return;
        }
        selectMachineTarget(*machine);
        if (address.isEmpty()) {
            address = machine->address;
        }
    } else if (!address.isEmpty()) {
        uint8_t targetAddressType = BLE_ADDR_PUBLIC;
        ScanRecord* record = findScannedDevice(address);
        if (record != nullptr) {
            targetAddressType = record->addressType;
        }
        if (SavedMachine* machine = findSavedMachineByAddress(address); machine != nullptr) {
            selectMachineTarget(*machine);
        } else {
            selectAddressTarget(address, targetAddressType);
        }
    }
    if (selectedAddress.isEmpty()) {
        sendError(400, "address is required");
        return;
    }

    const bool pairFirst = request["pair"].isNull() ? true : request["pair"].as<bool>();
    const bool reconnectAfterPair = request["reconnectAfterPair"].isNull() ? true : request["reconnectAfterPair"].as<bool>();
    const bool warmupPing = request["warmupPing"].isNull() ? true : request["warmupPing"].as<bool>();
    const bool encrypt = request["encrypt"].isNull() ? true : request["encrypt"].as<bool>();
    const bool chunked = request["chunked"] | false;
    const bool useStoredSession = request["useStoredSession"] | false;
    const bool rememberSession = request["rememberSession"] | false;
    const String notificationModeForRequest = request["notificationMode"] | "notify";
    const uint32_t reconnectDelayMs = request["reconnectDelayMs"] | DEFAULT_RECONNECT_DELAY_MS;
    const uint32_t waitMs = request["waitMs"] | 3000;
    const uint32_t settleMs = request["settleMs"] | 500;
    const uint32_t interChunkDelayMs = request["interChunkDelayMs"] | 0;
    const uint16_t hrRegisterId = request["hrRegisterId"] | 200;

    ByteVector sessionKeyOverride;
    const ByteVector* sessionKey = nullptr;
    String sessionHexUsed;
    if (!resolveSessionKeyForRequest(request["sessionHex"] | "",
                                     useStoredSession,
                                     serial,
                                     address,
                                     sessionKeyOverride,
                                     sessionKey,
                                     sessionHexUsed,
                                     error)) {
        sendError(400, error);
        return;
    }

    if (rememberSession && sessionKey != nullptr) {
        setStoredSessionKey(*sessionKey, "manual-app-probe", serial, address, selectedAddressType);
    }

    DynamicJsonDocument response(24576);
    if (!runAppStyleProbe(waitMs,
                          pairFirst,
                          reconnectAfterPair,
                          reconnectDelayMs,
                          settleMs,
                          warmupPing,
                          hrRegisterId,
                          encrypt,
                          chunked,
                          interChunkDelayMs,
                          notificationModeForRequest,
                          sessionKey,
                          response,
                          error)) {
        lastError = error;
        response["error"] = error;
        response["sessionHexUsed"] = sessionHexUsed;
        appendStatus(response);
        sendJson(response, 500);
        return;
    }

    response["sessionHexUsed"] = sessionHexUsed;
    appendStatus(response);
    sendJson(response);
}

void handleVerify() {
    DynamicJsonDocument request(2048);
    String error;
    uint32_t waitMs = 3000;
    bool pairFirst  = true;
    bool reconnectAfterPair = true;
    uint32_t reconnectDelayMs = DEFAULT_RECONNECT_DELAY_MS;
    String notificationModeForRequest = "notify";

    if (parseJsonBody(request, error)) {
        const String address = request["address"] | "";
        if (!address.isEmpty()) {
            uint8_t targetAddressType = BLE_ADDR_PUBLIC;
            ScanRecord* record = findScannedDevice(address);
            if (record != nullptr) {
                targetAddressType = record->addressType;
            }
            selectAddressTarget(address, targetAddressType);
        }
        waitMs    = request["waitMs"] | waitMs;
        pairFirst = request["pair"] | pairFirst;
        reconnectAfterPair = request["reconnectAfterPair"].isNull() ? reconnectAfterPair : request["reconnectAfterPair"].as<bool>();
        reconnectDelayMs = request["reconnectDelayMs"] | reconnectDelayMs;
        notificationModeForRequest = request["notificationMode"] | notificationModeForRequest;
    }

    if (selectedAddress.isEmpty()) {
        sendError(400, "address is required");
        return;
    }

    DynamicJsonDocument response(24576);
    if (!runCommunicationVerify(waitMs, pairFirst, reconnectAfterPair, reconnectDelayMs, notificationModeForRequest, response, error)) {
        lastError          = error;
        response["error"]  = error;
        response["waitMs"] = waitMs;
        response["pair"]   = pairFirst;
        sendJson(response, 500);
        return;
    }

    response["waitMs"] = waitMs;
    response["pair"]   = pairFirst;
    sendJson(response);
}

void handleStatsProbe() {
    DynamicJsonDocument request(2048);
    String error;
    bool pairFirst = true;
    bool reconnectAfterPair = true;
    uint32_t reconnectDelayMs = DEFAULT_RECONNECT_DELAY_MS;
    String notificationModeForRequest = "notify";
    bool encrypt = true;
    uint32_t waitMs = 3000;

    if (parseJsonBody(request, error)) {
        const String address = request["address"] | "";
        if (!address.isEmpty()) {
            uint8_t targetAddressType = BLE_ADDR_PUBLIC;
            ScanRecord* record = findScannedDevice(address);
            if (record != nullptr) {
                targetAddressType = record->addressType;
            }
            selectAddressTarget(address, targetAddressType);
        }
        pairFirst = request["pair"].isNull() ? pairFirst : request["pair"].as<bool>();
        reconnectAfterPair = request["reconnectAfterPair"].isNull() ? reconnectAfterPair : request["reconnectAfterPair"].as<bool>();
        reconnectDelayMs = request["reconnectDelayMs"] | reconnectDelayMs;
        notificationModeForRequest = request["notificationMode"] | notificationModeForRequest;
        encrypt = request["encrypt"].isNull() ? encrypt : request["encrypt"].as<bool>();
        waitMs = request["waitMs"] | waitMs;
    }

    if (selectedAddress.isEmpty()) {
        sendError(400, "address is required");
        return;
    }

    DynamicJsonDocument response(32768);
    if (!runStatsFlowProbe(waitMs,
                           pairFirst,
                           reconnectAfterPair,
                           reconnectDelayMs,
                           notificationModeForRequest,
                           encrypt,
                           response,
                           error)) {
        lastError = error;
        response["error"] = error;
        appendStatus(response);
        sendJson(response, 500);
        return;
    }

    appendStatus(response);
    sendJson(response);
}

void handleSettingsProbe() {
    DynamicJsonDocument request(2048);
    String error;
    bool pairFirst = true;
    bool reconnectAfterPair = true;
    uint32_t reconnectDelayMs = DEFAULT_RECONNECT_DELAY_MS;
    String notificationModeForRequest = "notify";
    bool encrypt = true;
    uint32_t waitMs = 3000;
    SettingsFamily familyOverride = SettingsFamily::Unknown;

    if (parseJsonBody(request, error)) {
        const String address = request["address"] | "";
        if (!address.isEmpty()) {
            uint8_t targetAddressType = BLE_ADDR_PUBLIC;
            ScanRecord* record = findScannedDevice(address);
            if (record != nullptr) {
                targetAddressType = record->addressType;
            }
            selectAddressTarget(address, targetAddressType);
        }
        pairFirst = request["pair"].isNull() ? pairFirst : request["pair"].as<bool>();
        reconnectAfterPair = request["reconnectAfterPair"].isNull() ? reconnectAfterPair : request["reconnectAfterPair"].as<bool>();
        reconnectDelayMs = request["reconnectDelayMs"] | reconnectDelayMs;
        notificationModeForRequest = request["notificationMode"] | notificationModeForRequest;
        encrypt = request["encrypt"].isNull() ? encrypt : request["encrypt"].as<bool>();
        waitMs = request["waitMs"] | waitMs;
        familyOverride = parseSettingsFamilyOverride(request["family"] | "");
    }

    if (selectedAddress.isEmpty()) {
        sendError(400, "address is required");
        return;
    }

    DynamicJsonDocument response(65536);
    if (!runSettingsFlowProbe(waitMs,
                              pairFirst,
                              reconnectAfterPair,
                              reconnectDelayMs,
                              notificationModeForRequest,
                              encrypt,
                              familyOverride,
                              response,
                              error)) {
        lastError = error;
        response["error"] = error;
        appendStatus(response);
        sendJson(response, 500);
        return;
    }

    appendStatus(response);
    sendJson(response);
}

void handleWorkerProbe() {
    DynamicJsonDocument request(2048);
    String error;
    bool pairFirst = true;
    bool reconnectAfterPair = true;
    uint32_t reconnectDelayMs = DEFAULT_RECONNECT_DELAY_MS;
    String notificationModeForRequest = "notify";
    bool continueOnHuFailure = false;
    uint32_t waitMs = 2500;

    if (parseJsonBody(request, error)) {
        const String address = request["address"] | "";
        if (!address.isEmpty()) {
            uint8_t targetAddressType = BLE_ADDR_PUBLIC;
            ScanRecord* record = findScannedDevice(address);
            if (record != nullptr) {
                targetAddressType = record->addressType;
            }
            selectAddressTarget(address, targetAddressType);
        }
        pairFirst = request["pair"].isNull() ? pairFirst : request["pair"].as<bool>();
        reconnectAfterPair = request["reconnectAfterPair"].isNull() ? reconnectAfterPair : request["reconnectAfterPair"].as<bool>();
        reconnectDelayMs = request["reconnectDelayMs"] | reconnectDelayMs;
        notificationModeForRequest = request["notificationMode"] | notificationModeForRequest;
        continueOnHuFailure = request["continueOnHuFailure"] | continueOnHuFailure;
        waitMs = request["waitMs"] | waitMs;
    }

    if (selectedAddress.isEmpty()) {
        sendError(400, "address is required");
        return;
    }

    DynamicJsonDocument response(32768);
    if (!runWorkerSessionProbe(waitMs,
                               pairFirst,
                               reconnectAfterPair,
                               reconnectDelayMs,
                               notificationModeForRequest,
                               continueOnHuFailure,
                               response,
                               error)) {
        lastError = error;
        response["error"] = error;
        appendStatus(response);
        sendJson(response, 500);
        return;
    }

    appendStatus(response);
    sendJson(response);
}

void handleGattServices() {
    DynamicJsonDocument request(2048);
    String error;
    if (parseJsonBody(request, error)) {
        const String address = request["address"] | "";
        if (!address.isEmpty()) {
            uint8_t targetAddressType = BLE_ADDR_PUBLIC;
            ScanRecord* record = findScannedDevice(address);
            if (record != nullptr) {
                targetAddressType = record->addressType;
            }
            selectAddressTarget(address, targetAddressType);
        }
    }

    if (!connectToSelectedDevice(error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    const bool includeDescriptors = request["includeDescriptors"] | false;
    DynamicJsonDocument response(32768);
    response["ok"] = true;
    JsonArray services = response.createNestedArray("services");
    appendServiceList(services, includeDescriptors);
    appendStatus(response);
    sendJson(response);
}

void handleGattRead() {
    DynamicJsonDocument request(4096);
    String error;
    if (!parseJsonBody(request, error)) {
        sendError(400, error);
        return;
    }

    const String address = request["address"] | "";
    if (!address.isEmpty()) {
        uint8_t targetAddressType = BLE_ADDR_PUBLIC;
        ScanRecord* record = findScannedDevice(address);
        if (record != nullptr) {
            targetAddressType = record->addressType;
        }
        selectAddressTarget(address, targetAddressType);
    }

    if (!connectToSelectedDevice(error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    NimBLERemoteService* service = nullptr;
    NimBLERemoteCharacteristic* characteristic =
        resolveRemoteCharacteristicByUuid(request["serviceUuid"] | "", request["charUuid"] | "", &service, error);
    if (characteristic == nullptr) {
        lastError = error;
        sendError(400, error);
        return;
    }

    ByteVector value;
    if (!readCharacteristicValue(characteristic, value, error)) {
        lastError = error;
        DynamicJsonDocument response(8192);
        response["ok"] = false;
        response["error"] = error;
        response["serviceUuid"] = service != nullptr ? String(service->getUUID().toString().c_str()) : "";
        appendCharacteristicInfo(response.createNestedObject("characteristic"), characteristic);
        appendStatus(response);
        sendJson(response, 500);
        return;
    }

    DynamicJsonDocument response(8192);
    response["ok"] = true;
    response["serviceUuid"] = service != nullptr ? String(service->getUUID().toString().c_str()) : "";
    response["valueHex"] = hexEncode(value);
    response["valueAscii"] = printableAscii(value);
    appendCharacteristicInfo(response.createNestedObject("characteristic"), characteristic);
    appendStatus(response);
    sendJson(response);
}

void handleGattWrite() {
    DynamicJsonDocument request(8192);
    String error;
    if (!parseJsonBody(request, error)) {
        sendError(400, error);
        return;
    }

    const String address = request["address"] | "";
    if (!address.isEmpty()) {
        uint8_t targetAddressType = BLE_ADDR_PUBLIC;
        ScanRecord* record = findScannedDevice(address);
        if (record != nullptr) {
            targetAddressType = record->addressType;
        }
        selectAddressTarget(address, targetAddressType);
    }

    if (!connectToSelectedDevice(error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    ByteVector payload;
    const String hex = request["hex"] | "";
    if (hex.isEmpty()) {
        sendError(400, "hex is required");
        return;
    }
    if (!hexDecode(hex, payload, error)) {
        sendError(400, error);
        return;
    }

    NimBLERemoteService* service = nullptr;
    NimBLERemoteCharacteristic* characteristic =
        resolveRemoteCharacteristicByUuid(request["serviceUuid"] | "", request["charUuid"] | "", &service, error);
    if (characteristic == nullptr) {
        lastError = error;
        sendError(400, error);
        return;
    }

    const bool response = request["response"].isNull() ? true : request["response"].as<bool>();
    const bool subscribe = request["subscribe"] | false;
    const String mode = normalizeNotificationMode(request["notificationMode"] | "notify");
    const bool useNotify = notificationModeUsesNotify(mode);
    const uint32_t waitMs = request["waitMs"] | 0;
    const bool chunked = request["chunked"] | false;
    const uint32_t interChunkDelayMs = request["interChunkDelayMs"] | 0;
    bool subscribed = false;

    clearNotificationBuffer();
    if (subscribe) {
        if (!(characteristic->canNotify() || characteristic->canIndicate())) {
            sendError(400, "characteristic cannot notify or indicate");
            return;
        }
        if (!characteristic->subscribe(useNotify, genericNotifyCallback, true)) {
            sendError(500, "failed to subscribe to characteristic");
            return;
        }
        subscribed = true;
    }

    std::vector<ByteVector> chunks;
    std::vector<uint32_t> times;
    const uint32_t startedAt = millis();
    if (!writeRemoteCharacteristic(characteristic,
                                   "gatt",
                                   payload,
                                   response,
                                   chunked,
                                   10,
                                   interChunkDelayMs,
                                   error)) {
        if (subscribed) {
            characteristic->unsubscribe(true);
        }
        lastError = error;
        sendError(500, error);
        return;
    }

    if (waitMs > 0) {
        const uint32_t deadline = millis() + waitMs;
        while (millis() < deadline) {
            delay(10);
        }
    }
    copyNotificationHistory(chunks, times);

    if (subscribed) {
        characteristic->unsubscribe(true);
    }

    DynamicJsonDocument responseDoc(16384);
    responseDoc["ok"] = true;
    responseDoc["serviceUuid"] = service != nullptr ? String(service->getUUID().toString().c_str()) : "";
    responseDoc["writeHex"] = hexEncode(payload);
    responseDoc["subscribe"] = subscribe;
    responseDoc["notificationMode"] = subscribe ? mode : "";
    responseDoc["notifyCount"] = chunks.size();
    appendCharacteristicInfo(responseDoc.createNestedObject("characteristic"), characteristic);
    JsonArray notifications = responseDoc.createNestedArray("notifications");
    appendNotificationChunks(notifications, chunks, times, startedAt);
    appendStatus(responseDoc);
    sendJson(responseDoc);
}

void handleRawRead() {
    DynamicJsonDocument request(2048);
    String error;
    if (!parseJsonBody(request, error)) {
        sendError(400, error);
        return;
    }

    const String alias = request["charAlias"] | "";
    if (alias.isEmpty()) {
        sendError(400, "charAlias is required");
        return;
    }

    ByteVector valueBytes;
    if (!rawReadNivona(alias, valueBytes, error)) {
        lastError = error;
        DynamicJsonDocument responseDoc(4096);
        responseDoc["ok"] = false;
        responseDoc["error"] = error;
        responseDoc["charAlias"] = alias;
        appendCharacteristicInfo(responseDoc.createNestedObject("characteristic"),
                                 resolveNivonaCharacteristic(alias));
        sendJson(responseDoc, 500);
        return;
    }

    DynamicJsonDocument responseDoc(4096);
    responseDoc["ok"] = true;
    responseDoc["charAlias"] = alias;
    responseDoc["valueHex"] = hexEncode(valueBytes);
    responseDoc["valueAscii"] = printableAscii(valueBytes);
    appendCharacteristicInfo(responseDoc.createNestedObject("characteristic"),
                             resolveNivonaCharacteristic(alias));
    sendJson(responseDoc);
}

void handleRawWrite() {
    DynamicJsonDocument request(4096);
    String error;
    if (!parseJsonBody(request, error)) {
        sendError(400, error);
        return;
    }

    const String alias   = request["charAlias"] | "tx";
    const String hex     = request["hex"] | "";
    const bool response  = request["response"] | true;
    const uint32_t waitMs = request["waitMs"] | 0;

    if (hex.isEmpty()) {
        sendError(400, "hex is required");
        return;
    }

    ByteVector payload;
    if (!hexDecode(hex, payload, error)) {
        sendError(400, error);
        return;
    }

    ByteVector notifyBytes;
    if (!rawWriteNivona(alias, payload, response, waitMs, notifyBytes, error)) {
        lastError = error;
        DynamicJsonDocument responseDoc(4096);
        responseDoc["ok"] = false;
        responseDoc["error"] = error;
        responseDoc["notifyHex"] = notifyBytes.empty() ? "" : hexEncode(notifyBytes);
        sendJson(responseDoc, 500);
        return;
    }

    DynamicJsonDocument responseDoc(4096);
    responseDoc["ok"] = true;
    responseDoc["notifyHex"] = notifyBytes.empty() ? "" : hexEncode(notifyBytes);
    sendJson(responseDoc);
}

void handleLogs() {
    DynamicJsonDocument doc(12288);
    doc["ok"] = true;
    JsonArray entries = doc.createNestedArray("entries");

    if (logMutex != nullptr && xSemaphoreTake(logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (const auto& entry : logs) {
            JsonObject item = entries.createNestedObject();
            item["timestampMs"] = entry.timestampMs;
            item["source"]      = entry.source;
            item["message"]     = entry.message;
        }
        xSemaphoreGive(logMutex);
    }

    sendJson(doc);
}

void handleLogsClear() {
    if (logMutex != nullptr && xSemaphoreTake(logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        logs.clear();
        xSemaphoreGive(logMutex);
    }
    DynamicJsonDocument doc(512);
    doc["ok"] = true;
    sendJson(doc);
}

void handleReboot() {
    DynamicJsonDocument doc(512);
    doc["ok"] = true;
    doc["message"] = "rebooting";
    sendJson(doc);
    delay(250);
    ESP.restart();
}

void handleOtaFinished() {
    const bool ok = !Update.hasError();
    DynamicJsonDocument doc(1024);
    doc["ok"] = ok;
    doc["message"] = ok ? "OTA upload complete, rebooting" : "OTA upload failed";
    sendJson(doc, ok ? 200 : 500);
    if (ok) {
        addLog("ota", "OTA update complete, rebooting");
        delay(250);
        ESP.restart();
    }
}

void handleOtaUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        addLog("ota", String("Starting OTA upload: ") + upload.filename);
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
        return;
    }

    if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
        return;
    }

    if (upload.status == UPLOAD_FILE_END) {
        if (!Update.end(true)) {
            Update.printError(Serial);
            addLog("ota", "OTA finalize failed");
        } else {
            addLog("ota", String("OTA upload successful, bytes=") + upload.totalSize);
        }
        return;
    }

    if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        addLog("ota", "OTA upload aborted");
    }
}

void registerRoutes() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/devices", HTTP_GET, handleDevices);
    server.on("/api/details", HTTP_GET, handleDetails);
    server.on("/api/logs", HTTP_GET, handleLogs);
    server.on("/api/machines", HTTP_GET, handleMachinesList);
    server.on("/api/machines", HTTP_POST, handleMachinesCreate);
    server.on("/api/machines/manual", HTTP_POST, handleMachinesManualCreate);
    server.on("/api/machines/probe", HTTP_POST, handleMachineProbe);
    server.on("/api/machines/reset", HTTP_POST, handleMachinesReset);

    server.on("/api/wifi/save", HTTP_POST, handleWifiSave);
    server.on("/api/scan", HTTP_POST, handleScan);
    server.on("/api/connect", HTTP_POST, handleConnect);
    server.on("/api/disconnect", HTTP_POST, handleDisconnect);
    server.on("/api/pair", HTTP_POST, handlePair);
    server.on("/api/notifications", HTTP_POST, handleNotifications);
    server.on("/api/protocol/session", HTTP_POST, handleSession);
    server.on("/api/protocol/hu", HTTP_POST, handleHu);
    server.on("/api/protocol/send-frame", HTTP_POST, handleSendFrame);
    server.on("/api/protocol/app-probe", HTTP_POST, handleAppProbe);
    server.on("/api/protocol/verify", HTTP_POST, handleVerify);
    server.on("/api/protocol/stats-probe", HTTP_POST, handleStatsProbe);
    server.on("/api/protocol/settings-probe", HTTP_POST, handleSettingsProbe);
    server.on("/api/protocol/worker-probe", HTTP_POST, handleWorkerProbe);
    server.on("/api/gatt/services", HTTP_POST, handleGattServices);
    server.on("/api/gatt/read", HTTP_POST, handleGattRead);
    server.on("/api/gatt/write", HTTP_POST, handleGattWrite);
    server.on("/api/protocol/raw-read", HTTP_POST, handleRawRead);
    server.on("/api/protocol/raw-write", HTTP_POST, handleRawWrite);
    server.on("/api/logs/clear", HTTP_POST, handleLogsClear);
    server.on("/api/reboot", HTTP_POST, handleReboot);
    server.on("/api/ota", HTTP_POST, handleOtaFinished, handleOtaUpload);

    server.onNotFound([]() {
        if (dispatchMachineApiRoute()) {
            return;
        }
        sendError(404, "not found");
    });
}

void setupMdns() {
    if (MDNS.begin(APP_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        addLog("mdns", String("mDNS started at http://") + APP_HOSTNAME + ".local/");
    } else {
        addLog("mdns", "Failed to start mDNS");
    }
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("\n%s %s booting (%s)\n", APP_NAME, APP_VERSION, APP_BUILD_TIME);

    logMutex          = xSemaphoreCreateMutex();
    notifyDataMutex   = xSemaphoreCreateMutex();
    notificationLatch = xSemaphoreCreateBinary();
    scanDataMutex     = xSemaphoreCreateMutex();

    preferences.begin(PREFS_WIFI, false);
    machinePreferences.begin(PREFS_MACHINES, false);
    if (!initializeLittleFs(lastError)) {
        addLog("fs", String("Failed to initialize LittleFS: ") + lastError);
    } else {
        addLog("fs", "LittleFS ready");
    }
    loadSavedMachines();
    connectWifi();
    setupMdns();

    if (!initializeBleStack(lastError)) {
        addLog("ble", String("Failed to initialize NimBLE stack: ") + lastError);
    }

    registerRoutes();
    server.begin();
    addLog("http", "HTTP server started");
}

void loop() {
    server.handleClient();
    finalizeIdleScanIfReady();
    if ((client == nullptr || !client->isConnected()) && millis() >= scanSuppressedUntilMs &&
        !idleScanInProgress && !blockingScanInProgress &&
        (millis() - lastIdleScanAtMs) >= IDLE_SCAN_INTERVAL_MS) {
        startIdleScanAsync();
    }
    delay(2);
}
