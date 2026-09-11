#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_system.h>

#include <NimBLEDevice.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "backup_staging.h"
#include "brew_history.h"
#include "bridge_http_server.h"
#include "bridge_json_object.h"
#include "bridge_jobs.h"
#include "bridge_runtime_policy.h"
#include "bridge_time.h"
#include "history_capacity.h"
#include "history_retention.h"
#include "history_storage.h"
#include "nivona.h"
#include "recipe_icons.h"
#include "stats_history.h"
#include "wifi_runtime_policy.h"
#include "web_ui_gzip.h"

namespace {

using namespace nivona;

constexpr char APP_HOSTNAME[]   = "esp-coffee-bridge";
constexpr char AP_SSID[]        = "esp-coffee-maker";
constexpr char AP_PASSWORD[]    = "coffee-setup";
constexpr char APP_BUILD_TIME[] = __DATE__ " " __TIME__;
constexpr uint32_t API_VERSION  = 2;
// A serialized status snapshot is currently below 3 KiB. Reserve enough
// ArduinoJson metadata without requiring an 8 KiB contiguous heap block while
// the BLE worker owns its larger bounded response document.
constexpr size_t STATUS_JSON_CAPACITY = 4096;
constexpr uint32_t SCAN_MS      = 3000;
constexpr uint32_t HTTP_TIMEOUT = 10000;
constexpr uint32_t DEFAULT_RECONNECT_DELAY_MS = 750;
constexpr uint32_t NOTIFICATION_BATCH_SETTLE_MS = 60;
constexpr size_t LOG_CAPACITY   = 128;
constexpr size_t BREW_HISTORY_PRECHECK_RESERVE_BYTES = 448;
constexpr uint32_t STATS_HISTORY_POLL_INTERVAL_MS = 15 * 60 * 1000;
constexpr uint32_t IDLE_SCAN_INTERVAL_MS = 60 * 1000;
constexpr uint32_t BLE_IDLE_DISCONNECT_MS = 10 * 1000;
constexpr uint32_t WORKER_STALL_WATCHDOG_MS = 45 * 1000;
constexpr uint32_t JOB_POLL_AFTER_MS = 500;
constexpr uint32_t SUMMARY_CACHE_TTL_MS = 60 * 1000;
constexpr uint32_t DEEP_CACHE_TTL_MS = 15 * 60 * 1000;
constexpr uint32_t FEATURES_CACHE_TTL_MS = 24 * 60 * 60 * 1000;
constexpr uint32_t CACHE_FAILURE_BACKOFF_MS = 5000;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t WIFI_RECONNECT_DELAY_MS = 5000;
constexpr size_t MACHINE_GENERATION_CAPACITY = 16;
constexpr size_t CACHEABLE_RESOURCE_COUNT = 4;
constexpr size_t RESOURCE_CACHE_CAPACITY = MACHINE_GENERATION_CAPACITY * CACHEABLE_RESOURCE_COUNT;
constexpr size_t MAX_RESOURCE_CACHE_BYTES = 48 * 1024;
constexpr size_t MAX_JOB_RESULT_BYTES = 128 * 1024;
constexpr size_t MAX_JOB_SPOOL_TOTAL_BYTES = 512 * 1024;
constexpr size_t MUTATION_RESULT_RESERVATION_BYTES = MAX_JOB_RESULT_BYTES;
constexpr uint16_t TEMP_RECIPE_TYPE_REGISTER = 9001;
constexpr char STANDARD_RECIPE_CACHE_PREFIX[] = "/stdrec-";
constexpr uint32_t STANDARD_RECIPE_CACHE_SCHEMA = 2;
constexpr char SAVED_RECIPE_CACHE_PREFIX[] = "/mycoffee-";
constexpr uint32_t SAVED_RECIPE_CACHE_SCHEMA = 4;
constexpr uint32_t SAVED_RECIPE_SLOT_CACHE_SCHEMA = 1;
constexpr size_t SAVED_RECIPE_ITEM_JSON_CAPACITY = 12 * 1024;
constexpr uint32_t BACKUP_BUNDLE_SCHEMA = 1;
constexpr char BACKUP_RESTORE_UPLOAD_PATH[] = "/restore-upload.ndjson";
constexpr char BACKUP_RESTORE_ACTIVE_MARKER[] = "/restore-active.marker";
constexpr char BACKUP_RESTORE_STAGED_MARKER[] = "/restore-staged.marker";
constexpr char BACKUP_RESTORE_COMMIT_MARKER[] = "/restore-commit.marker";
constexpr char BACKUP_RESTORE_STATE_PATH[] = "/restore-state.bak";
constexpr uint32_t BACKUP_RESTORE_STATE_MAGIC = 0x42525332;
// Uploads are staged directly into independently reclaimable chunk files.
constexpr size_t MAX_BACKUP_RESTORE_UPLOAD_BYTES =
    history_capacity::MAX_GENERATED_BACKUP_BYTES;
constexpr size_t MAX_BACKUP_JSON_LINE_BYTES = history_storage::MAX_JSON_LINE_BYTES + 1024;

constexpr size_t MAX_MACHINE_SERIAL_BYTES = 48;
constexpr size_t MAX_MACHINE_ALIAS_BYTES = 64;
constexpr size_t MAX_MACHINE_MANUFACTURER_BYTES = 32;
constexpr size_t MAX_MACHINE_MODEL_BYTES = 96;
constexpr size_t MAX_MACHINE_MODEL_CODE_BYTES = 32;
constexpr size_t MAX_MACHINE_MODEL_NAME_BYTES = 96;
constexpr size_t MAX_MACHINE_FAMILY_KEY_BYTES = 32;
constexpr size_t MAX_MACHINE_REVISION_BYTES = 48;
constexpr size_t MAX_MACHINE_AD06_HEX_BYTES = 192;
constexpr size_t MAX_MACHINE_AD06_ASCII_BYTES = 96;

void rxNotifyCallback(NimBLERemoteCharacteristic* characteristic, uint8_t* data, size_t length, bool isNotify);

constexpr char PREFS_WIFI[] = "wifi";
constexpr char PREFS_SSID[] = "ssid";
constexpr char PREFS_PASS[] = "pass";
constexpr char PREFS_HISTORY_MAX_BYTES[] = "hist_max";
constexpr char PREFS_MACHINES[] = "machines";
constexpr char PREFS_MACHINE_STORE[] = "store";
constexpr char PREFS_MACHINE_STORE_BLOB[] = "store_blob";
constexpr char PREFS_MACHINE_SCHEMA[] = "schema";
constexpr char CLIENT_TIME_HEADER[] = "X-Bridge-Time-Unix-Ms";

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
    uint32_t generation{0};
};

struct ProtocolSessionEntry {
    String serial;
    String address;
    uint8_t addressType{BLE_ADDR_PUBLIC};
    ByteVector sessionKey;
    String source;
    uint32_t setAtMs{0};
};

struct BackupHistorySize {
    String serial;
    size_t bytes{0};
};

struct BackupBundleSummary {
    size_t machineCount{0};
    size_t historyEntryCount{0};
    size_t statsHistoryEntryCount{0};
    size_t restoredHistoryBytes{0};
    size_t restoredStatsHistoryBytes{0};
    size_t peakRestoreDataBytes{0};
    size_t requestedBudgetBytes{0};
    size_t requestedStatsBudgetBytes{stats_history::DEFAULT_HISTORY_BYTES};
    std::vector<String> machineSerials;
    std::vector<String> historySerials;
    std::vector<String> statsHistorySerials;
    std::vector<BackupHistorySize> historyBytesBySerial;
    std::vector<BackupHistorySize> statsHistoryBytesBySerial;
};

struct ResourceCacheEntry {
    bool occupied{false};
    String serial;
    String resource;
    String path;
    String lastJobId;
    String lastErrorJobId;
    String lastErrorCode;
    String lastErrorMessage;
    uint32_t sampledAtMs{0};
    uint32_t ttlMs{0};
    uint32_t retryAfterMs{0};
};

struct MachineGenerationEntry {
    bool occupied{false};
    String serial;
    uint32_t generation{0};
    uint32_t lastStatsScheduledAtMs{0};
};

enum class BleOperation : uint16_t {
    MachineSummary = 1,
    MachineStats,
    MachineSettings,
    MachineFeatures,
    MachineRecipesRefresh,
    MachineRecipeDetail,
    MachineBrew,
    MachineConfirm,
    MachineMyCoffeeList,
    MachineMyCoffeeDetail,
    MachineMyCoffeeUpdate,
    MachineSettingsPost,
    MachineRefresh,
    Scan,
    MachineProbe,
    MachinesCreate,
    Connect,
    Disconnect,
    Pair,
    Details,
    Notifications,
    ProtocolSession,
    ProtocolHu,
    ProtocolSendFrame,
    ProtocolAppProbe,
    ProtocolVerify,
    ProtocolStatsProbe,
    ProtocolSettingsProbe,
    ProtocolWorkerProbe,
    GattServices,
    GattRead,
    GattWrite,
    ProtocolRawRead,
    ProtocolRawWrite,
};

struct WorkerExecutionContext {
    bool active{false};
    bool machineLoaded{false};
    bool responded{false};
    bool resource{false};
    bool spoolResult{false};
    bool mutationJob{false};
    bool mutationWriteArmed{false};
    bool mutationCommitted{false};
    bool forceRefresh{false};
    bool backgroundJob{false};
    SavedMachine machine;
    String jobId;
    String target;
    BleOperation operation{BleOperation::MachineSummary};
    String argument;
    String requestBody;
    String targetAddress;
    uint8_t targetAddressType{BLE_ADDR_PUBLIC};
    String resultPath;
    String resultReservationPath;
    String responseBody;
    String errorCode;
    String errorMessage;
    uint32_t deadlineAtMs{0};
    int responseStatus{500};
};

struct BridgeHealthSnapshot {
    bool workerReady{false};
    bool workerBusy{false};
    bool clientCreated{false};
    bool clientConnected{false};
    bool scanInProgress{false};
    bool watchdogMarkerPresent{false};
    uint8_t currentProgress{0};
    uint32_t queuedJobs{0};
    uint32_t runningJobs{0};
    uint32_t currentJobAgeMs{0};
    uint32_t workerHeartbeatAgeMs{0};
    uint32_t workerStackHighWaterMark{0};
    uint32_t submittedJobs{0};
    uint32_t completedJobs{0};
    uint32_t failedJobs{0};
    uint32_t cancelledJobs{0};
    uint32_t rejectedJobs{0};
    uint32_t coalescedJobs{0};
    uint32_t backgroundEvictedJobs{0};
    uint32_t durableMachineWrites{0};
    uint32_t httpLastDurationUs{0};
    uint32_t httpMaxDurationUs{0};
    uint32_t resetReason{0};
    uint32_t deviceCount{0};
    uint32_t supportedDeviceCount{0};
    uint32_t savedMachineCount{0};
    uint32_t protocolSessionCount{0};
    uint8_t selectedAddressType{BLE_ADDR_PUBLIC};
    int32_t lastScanReason{0};
    uint32_t lastScanResultCount{0};
    uint32_t lastScanAtMs{0};
    uint32_t watchdogAtMs{0};
    uint32_t freeHeap{0};
    uint32_t minimumFreeHeap{0};
    uint32_t largestFreeHeapBlock{0};
    size_t littleFsTotalBytes{0};
    size_t littleFsUsedBytes{0};
    size_t historyFileCount{0};
    size_t historyTotalBytes{0};
    size_t largestBrewHistoryFileBytes{0};
    size_t largestStatsHistoryFileBytes{0};
    char currentJobId[32]{};
    char currentJobKind[40]{};
    char currentJobTarget[48]{};
    char peerAddress[24]{};
    char selectedAddress[24]{};
    char notificationMode[16]{};
    char pairingStatus[32]{};
    char lastError[128]{};
    char watchdogJobId[32]{};
    char watchdogJobKind[40]{};
    char watchdogJobTarget[48]{};
};

struct RtcWatchdogMarker {
    uint32_t magic{0};
    uint32_t atMs{0};
    char jobId[32]{};
    char kind[40]{};
    char target[48]{};
};

template <size_t Capacity>
class SharedStatusText {
public:
    SharedStatusText() {
        value_[0] = '\0';
    }

    explicit SharedStatusText(const char* initial) {
        value_[0] = '\0';
        assign(initial);
    }

    SharedStatusText& operator=(const String& value) {
        assign(value.c_str());
        return *this;
    }

    SharedStatusText& operator=(const char* value) {
        assign(value);
        return *this;
    }

    String snapshot() const {
        char copy[Capacity];
        portENTER_CRITICAL(&mutex_);
        strlcpy(copy, value_, sizeof(copy));
        portEXIT_CRITICAL(&mutex_);
        return String(copy);
    }

private:
    void assign(const char* value) {
        portENTER_CRITICAL(&mutex_);
        strlcpy(value_, value != nullptr ? value : "", sizeof(value_));
        portEXIT_CRITICAL(&mutex_);
    }

    mutable portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;
    char value_[Capacity]{};
};

constexpr uint32_t RTC_WATCHDOG_MAGIC = 0x42574447;
RTC_DATA_ATTR RtcWatchdogMarker rtcWatchdogMarker;

bridge_http::BridgeHttpServer server(80);
Preferences preferences;
Preferences machinePreferences;
uint32_t bootNonce = 0;

SemaphoreHandle_t logMutex          = nullptr;
SemaphoreHandle_t notifyDataMutex   = nullptr;
SemaphoreHandle_t notificationLatch = nullptr;
SemaphoreHandle_t scanDataMutex     = nullptr;
SemaphoreHandle_t jobMutex          = nullptr;
SemaphoreHandle_t cacheMutex        = nullptr;
SemaphoreHandle_t healthMutex       = nullptr;
SemaphoreHandle_t machineMutex      = nullptr;
SemaphoreHandle_t machineGenerationMutex = nullptr;

std::vector<ScanRecord> scannedDevices;
std::vector<ScanRecord> scanScratchDevices;
std::vector<LogEntry> logs;
std::vector<SavedMachine> savedMachines;
std::array<MachineGenerationEntry, MACHINE_GENERATION_CAPACITY> machineGenerations{};

ByteVector lastNotificationBytes;
std::vector<ByteVector> notificationHistory;
std::vector<uint32_t> notificationHistoryMs;
String selectedAddress;
uint8_t selectedAddressType = BLE_ADDR_PUBLIC;
String selectedMachineSerial;
String wifiStaSsid;
SharedStatusText<32> pairingStatus("idle");
SharedStatusText<128> lastError;
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
constexpr uint32_t ACTIVE_SCAN_SUPPRESS_MS = 15000;
uint32_t scanSuppressedUntilMs = 0;
bool idleScanInProgress = false;
bool blockingScanInProgress = false;
uint32_t idleScanStartedAtMs = 0;
bool littleFsReady = false;

class BackupChunkStore {
public:
    static String path(size_t index) {
        return String(BACKUP_RESTORE_UPLOAD_PATH) + "." + String(index) + ".part";
    }
    bool openWrite(size_t index) { close(); file_ = LittleFS.open(path(index), "w"); return bool(file_); }
    bool openRead(size_t index) { close(); file_ = LittleFS.open(path(index), "r"); return bool(file_); }
    size_t size() const { return file_.size(); }
    size_t read(uint8_t* buffer, size_t bytes) { return file_.read(buffer, bytes); }
    size_t write(const uint8_t* buffer, size_t bytes) { return file_.write(buffer, bytes); }
    void close() { if (file_) file_.close(); }
    bool exists(size_t index) const { return LittleFS.exists(path(index)); }
    bool remove(size_t index) { return LittleFS.remove(path(index)); }
private:
    File file_;
};

BackupChunkStore backupRestoreUploadStore;
backup_staging::Writer<BackupChunkStore> backupRestoreUploadWriter(backupRestoreUploadStore);
using BackupReader = backup_staging::Reader<BackupChunkStore>;
String backupRestoreUploadError;
String backupRestoreUploadFilename;
size_t backupRestoreUploadBytes = 0;
bool backupRestoreUploadComplete = false;
bool wifiConnectionAttemptActive = false;
uint32_t wifiConnectionAttemptStartedAtMs = 0;
uint32_t wifiNextReconnectAtMs = 0;
bool wifiAccessPointActive = false;
bool wifiStationWasConnected = false;
String lastPersistedMachinePayload;
bool machinePersistenceDirty = false;
uint32_t durableMachineWriteCount = 0;
size_t backgroundStatsCursor = 0;
uint32_t nextBackgroundStatsDispatchAtMs = 0;
uint32_t nextIdleScanSubmitAtMs = 0;
uint32_t nextMachineGeneration = 1;

bridge_jobs::Scheduler jobScheduler;
std::unique_ptr<ResourceCacheEntry[]> resourceCaches(
    new (std::nothrow) ResourceCacheEntry[RESOURCE_CACHE_CAPACITY]);
std::atomic<WorkerExecutionContext*> workerExecution{nullptr};
BridgeHealthSnapshot bridgeHealth;
TaskHandle_t bleWorkerTaskHandle = nullptr;
TaskHandle_t workerSupervisorTaskHandle = nullptr;
std::atomic<bool> workerJobActive{false};
std::atomic<bool> workerBleCallActive{false};
std::atomic<bool> workerMaintenanceCallActive{false};
std::atomic<uint32_t> workerBleCallDepth{0};
std::atomic<uint32_t> workerHeartbeatAtMs{0};
std::atomic<uint32_t> workerLastBleCallDurationMs{0};
std::atomic<uint32_t> workerCurrentStartedAtMs{0};
std::atomic<uint32_t> workerCurrentDeadlineAtMs{0};
std::atomic<uint8_t> workerCurrentProgress{0};
std::atomic<bool> workerResetRequested{false};
std::atomic<bool> clientDisconnectedEvent{false};
std::atomic<bool> clientDisconnectPending{false};
std::atomic<bool> wifiReconnectRequested{false};
String workerCurrentJobId;
String workerCurrentJobKind;
String workerCurrentJobTarget;
String activeResultStreamPath;
uint32_t lastBleActivityAtMs = 0;

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
bool cancelTargetJobsAndCacheMetadata(const String& serial);
void removeTargetResourceCacheFiles(const String& serial);
void requestBleWorkerReset();
size_t jobSpoolBytesLocked(const String& replacingPath);
bool littleFsCanAllocateLocked(size_t incomingBytes);
void invalidateResourceCache(const String& serial, const String& resource);
bool streamJsonFileResponse(const String& path,
                            size_t maximumBytes,
                            int status,
                            bool& responseStarted,
                            String& error);

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

void addTimeLog(const String& message) {
    addLog("time", message);
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

void invalidateRemoteHandlesForFullDiscovery() {
    // NimBLE's refresh=true discovery deletes its cached remote objects. Drop
    // every application pointer before those objects are released.
    disService    = nullptr;
    nivonaService = nullptr;
    nivonaCtrl    = nullptr;
    nivonaRx      = nullptr;
    nivonaTx      = nullptr;
    nivonaAux1    = nullptr;
    nivonaAux2    = nullptr;
    nivonaName    = nullptr;
}

void applyClientDisconnectedEvent() {
    if (!clientDisconnectedEvent.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    clearRemoteHandles();
    cachedDetails = DeviceDetails{};
    protocolSessions.clear();
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

WorkerExecutionContext* currentWorkerExecution() {
    if (bleWorkerTaskHandle == nullptr || xTaskGetCurrentTaskHandle() != bleWorkerTaskHandle) {
        return nullptr;
    }
    WorkerExecutionContext* execution = workerExecution.load(std::memory_order_acquire);
    if (execution == nullptr || !execution->active) {
        return nullptr;
    }
    return execution;
}

class WorkerBleCallScope {
public:
    WorkerBleCallScope()
        : active_(bleWorkerTaskHandle != nullptr &&
                  xTaskGetCurrentTaskHandle() == bleWorkerTaskHandle),
          startedAtMs_(millis()) {
        if (active_) {
            workerHeartbeatAtMs.store(millis(), std::memory_order_release);
            if (workerBleCallDepth.fetch_add(1, std::memory_order_acq_rel) == 0) {
                workerMaintenanceCallActive.store(
                    currentWorkerExecution() == nullptr, std::memory_order_release);
                workerBleCallActive.store(true, std::memory_order_release);
            }
        }
    }
    ~WorkerBleCallScope() {
        if (active_) {
            workerLastBleCallDurationMs.store(
                static_cast<uint32_t>(millis() - startedAtMs_), std::memory_order_release);
            workerHeartbeatAtMs.store(millis(), std::memory_order_release);
            if (workerBleCallDepth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                workerBleCallActive.store(false, std::memory_order_release);
                workerMaintenanceCallActive.store(false, std::memory_order_release);
            }
        }
    }
    WorkerBleCallScope(const WorkerBleCallScope&) = delete;
    WorkerBleCallScope& operator=(const WorkerBleCallScope&) = delete;

private:
    bool active_{false};
    uint32_t startedAtMs_{0};
};

uint32_t allocateMachineGeneration() {
    uint32_t generation = nextMachineGeneration++;
    if (generation == 0) {
        generation = nextMachineGeneration++;
    }
    return generation;
}

class MachineGenerationLock {
public:
    explicit MachineGenerationLock(uint32_t timeoutMs = 1000)
        : locked_(machineGenerationMutex != nullptr &&
                  xSemaphoreTakeRecursive(machineGenerationMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {}
    ~MachineGenerationLock() {
        if (locked_) {
            xSemaphoreGiveRecursive(machineGenerationMutex);
        }
    }
    MachineGenerationLock(const MachineGenerationLock&) = delete;
    MachineGenerationLock& operator=(const MachineGenerationLock&) = delete;
    explicit operator bool() const { return locked_; }

private:
    bool locked_{false};
};

bool machineGenerationCurrentLocked(const String& serial, uint32_t generation) {
    for (const MachineGenerationEntry& entry : machineGenerations) {
        if (entry.occupied && entry.serial.equalsIgnoreCase(serial)) {
            return entry.generation == generation;
        }
    }
    return false;
}

bool setMachineGenerationLocked(const SavedMachine& machine) {
    MachineGenerationEntry* freeEntry = nullptr;
    for (MachineGenerationEntry& entry : machineGenerations) {
        if (entry.occupied && entry.serial.equalsIgnoreCase(machine.serial)) {
            entry.generation = machine.generation;
            entry.lastStatsScheduledAtMs = millis();
            return true;
        }
        if (!entry.occupied && freeEntry == nullptr) {
            freeEntry = &entry;
        }
    }
    if (freeEntry != nullptr) {
        freeEntry->occupied = true;
        freeEntry->serial = machine.serial;
        freeEntry->generation = machine.generation;
        freeEntry->lastStatsScheduledAtMs = millis();
        return true;
    }
    return false;
}

void removeMachineGenerationLocked(const String& serial) {
    for (MachineGenerationEntry& entry : machineGenerations) {
        if (entry.occupied && entry.serial.equalsIgnoreCase(serial)) {
            entry = {};
            return;
        }
    }
}

class WorkerMachineWriteGuard {
public:
    WorkerMachineWriteGuard()
        : lock_(1000) {
        WorkerExecutionContext* execution = currentWorkerExecution();
        current_ = static_cast<bool>(lock_) &&
            (execution == nullptr || !execution->machineLoaded ||
             machineGenerationCurrentLocked(execution->machine.serial, execution->machine.generation));
    }
    explicit operator bool() const { return current_; }

private:
    MachineGenerationLock lock_;
    bool current_{false};
};

bool workerTargetStillCurrent() {
    WorkerExecutionContext* execution = currentWorkerExecution();
    if (execution == nullptr || !execution->machineLoaded) {
        return true;
    }
    MachineGenerationLock lock(1000);
    if (!lock) {
        return false;
    }
    return machineGenerationCurrentLocked(execution->machine.serial, execution->machine.generation);
}

void noteWorkerProgress(uint8_t progress = 0) {
    WorkerExecutionContext* execution = currentWorkerExecution();
    if (execution == nullptr) {
        return;
    }
    workerHeartbeatAtMs.store(millis(), std::memory_order_release);
    if (progress == 0) {
        return;
    }
    progress = std::min<uint8_t>(progress, 99);
    uint8_t current = workerCurrentProgress.load(std::memory_order_acquire);
    while (progress > current &&
           !workerCurrentProgress.compare_exchange_weak(
               current, progress, std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
    if (progress > current && jobMutex != nullptr &&
        xSemaphoreTake(jobMutex, 0) == pdTRUE) {
        jobScheduler.setProgress(execution->jobId.c_str(), progress);
        xSemaphoreGive(jobMutex);
    }
}

bool workerDeadlineExceeded() {
    WorkerExecutionContext* execution = currentWorkerExecution();
    return execution != nullptr && bridge_runtime_policy::deadlineReached(millis(), execution->deadlineAtMs);
}

class WorkerMutationWriteScope {
public:
    WorkerMutationWriteScope()
        : execution_(currentWorkerExecution()) {
        if (execution_ != nullptr && execution_->mutationJob) {
            previousArmed_ = execution_->mutationWriteArmed;
            execution_->mutationWriteArmed = true;
        }
    }
    ~WorkerMutationWriteScope() {
        if (execution_ != nullptr && execution_->mutationJob) {
            execution_->mutationWriteArmed = previousArmed_;
        }
    }
    WorkerMutationWriteScope(const WorkerMutationWriteScope&) = delete;
    WorkerMutationWriteScope& operator=(const WorkerMutationWriteScope&) = delete;

private:
    WorkerExecutionContext* execution_{nullptr};
    bool previousArmed_{false};
};

void noteMutationApplied() {
    WorkerExecutionContext* execution = currentWorkerExecution();
    if (execution != nullptr && execution->mutationJob) {
        execution->mutationCommitted = true;
    }
}

void noteMutationWriteAcknowledged() {
    WorkerExecutionContext* execution = currentWorkerExecution();
    if (execution != nullptr && execution->mutationWriteArmed) {
        noteMutationApplied();
    }
}

uint32_t clampNotificationWait(uint32_t requestedMs) {
    uint32_t bounded = std::min<uint32_t>(requestedMs, 3000);
    WorkerExecutionContext* execution = currentWorkerExecution();
    if (execution == nullptr) {
        return bounded;
    }
    return bridge_runtime_policy::clampWaitToDeadline(
        bounded, 3000, millis(), execution->deadlineAtMs);
}

bool boundedWorkerDelay(uint32_t requestedMs, uint32_t maximumMs = 3000) {
    uint32_t waitMs = std::min(requestedMs, maximumMs);
    WorkerExecutionContext* execution = currentWorkerExecution();
    if (execution != nullptr) {
        waitMs = bridge_runtime_policy::clampWaitToDeadline(
            waitMs, maximumMs, millis(), execution->deadlineAtMs);
    }
    const uint32_t startedAtMs = millis();
    while (static_cast<uint32_t>(millis() - startedAtMs) < waitMs) {
        const uint32_t elapsedMs = static_cast<uint32_t>(millis() - startedAtMs);
        delay(std::min<uint32_t>(50, waitMs - elapsedMs));
        noteWorkerProgress();
    }
    return execution == nullptr || !workerDeadlineExceeded();
}

bool disconnectClientAndWait(String& error,
                             uint32_t timeoutMs = 1500,
                             bool ignoreJobDeadline = false) {
    error = "";
    if (client == nullptr) {
        clientDisconnectPending.store(false, std::memory_order_release);
        applyClientDisconnectedEvent();
        return true;
    }

    if (!client->isConnected() &&
        !clientDisconnectPending.load(std::memory_order_acquire)) {
        applyClientDisconnectedEvent();
        return true;
    }

    if (client->isConnected() &&
        !clientDisconnectPending.exchange(true, std::memory_order_acq_rel)) {
        bool disconnectStarted = false;
        {
            WorkerBleCallScope bleCall;
            disconnectStarted = client->disconnect();
        }
        // A false return can also mean that the peer already initiated
        // termination. Keep waiting for the callback/connection-handle pair
        // to settle instead of issuing another lifecycle operation.
        (void)disconnectStarted;
    }

    const uint32_t startedAtMs = millis();
    while (client != nullptr &&
           (client->isConnected() ||
            clientDisconnectPending.load(std::memory_order_acquire)) &&
           static_cast<uint32_t>(millis() - startedAtMs) < timeoutMs) {
        if (!ignoreJobDeadline && workerDeadlineExceeded()) {
            break;
        }
        delay(20);
        noteWorkerProgress();
    }

    applyClientDisconnectedEvent();
    if (client != nullptr &&
        (client->isConnected() ||
         clientDisconnectPending.load(std::memory_order_acquire))) {
        error = !ignoreJobDeadline && workerDeadlineExceeded()
            ? String("BLE disconnect exceeded the job deadline")
            : String("timed out waiting for BLE disconnect");
        return false;
    }
    return true;
}

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
    timeoutMs = clampNotificationWait(timeoutMs);
    if (timeoutMs == 0) {
        return false;
    }
    noteWorkerProgress();
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
    noteWorkerProgress();
    return true;
}

bool waitForNotificationBatch(uint32_t timeoutMs,
                              std::vector<ByteVector>& chunksOut,
                              std::vector<uint32_t>& timesOut) {
    chunksOut.clear();
    timesOut.clear();
    timeoutMs = clampNotificationWait(timeoutMs);
    if (timeoutMs == 0) {
        if (currentWorkerExecution() != nullptr) {
            return false;
        }
        copyNotificationHistory(chunksOut, timesOut);
        return !chunksOut.empty();
    }
    if (notificationLatch == nullptr || notifyDataMutex == nullptr) {
        return false;
    }

    const uint32_t startedAt = millis();
    ByteVector firstChunk;
    if (!waitForNotification(timeoutMs, firstChunk)) {
        return false;
    }

    uint32_t lastNotificationAt = millis();
    while (static_cast<uint32_t>(millis() - startedAt) < timeoutMs &&
           !workerDeadlineExceeded()) {
        const uint32_t elapsed = millis() - startedAt;
        const uint32_t remaining = timeoutMs > elapsed ? timeoutMs - elapsed : 0;
        if (remaining == 0) {
            break;
        }
        const uint32_t waitSlice = remaining < NOTIFICATION_BATCH_SETTLE_MS ? remaining : NOTIFICATION_BATCH_SETTLE_MS;
        if (xSemaphoreTake(notificationLatch, pdMS_TO_TICKS(waitSlice)) == pdTRUE) {
            lastNotificationAt = millis();
            noteWorkerProgress();
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
    WorkerExecutionContext* execution = currentWorkerExecution();
    if (execution != nullptr) {
        if (execution->machineLoaded) {
            selectedMachineSerial = execution->machine.serial;
            selectedAddress = execution->machine.address;
            selectedAddressType = execution->machine.addressType;
            return;
        }
        if (!execution->targetAddress.isEmpty()) {
            selectedMachineSerial = "";
            selectedAddress = execution->targetAddress;
            selectedAddressType = execution->targetAddressType;
            return;
        }
    }
    selectedMachineSerial = "";
    selectedAddress = address;
    selectedAddressType = addressType;
}

void selectMachineTarget(const SavedMachine& machine) {
    WorkerExecutionContext* execution = currentWorkerExecution();
    const SavedMachine& target = execution != nullptr && execution->machineLoaded
        ? execution->machine
        : machine;
    selectedMachineSerial = target.serial;
    selectedAddress = target.address;
    selectedAddressType = target.addressType;
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
    WorkerExecutionContext* execution = currentWorkerExecution();
    if (execution != nullptr) {
        execution->responded = true;
        execution->responseStatus = code;
        const String responseError = doc["error"] | "";
        if (!responseError.isEmpty()) {
            execution->errorMessage = responseError;
            if (execution->errorCode.isEmpty()) {
                execution->errorCode = code == 400 ? "invalid_request" : "bridge_operation_failed";
            }
        }

        auto retainCommittedMutationResultInMemory = [&](const String& warning) -> bool {
            if (!execution->mutationCommitted) {
                return false;
            }
            String fallback;
            const size_t expected = measureJson(doc);
            if (!doc.overflowed() && expected > 2 && expected <= MAX_JOB_RESULT_BYTES &&
                serializeJson(doc, fallback) == expected) {
                execution->responseBody = std::move(fallback);
            } else {
                execution->responseBody =
                    "{\"ok\":true,\"mutationApplied\":true,"
                    "\"resultUnavailable\":true}";
            }
            execution->spoolResult = false;
            execution->responseStatus = code;
            execution->errorCode = "result_storage_fallback";
            execution->errorMessage = warning;
            return true;
        };

        if (doc.overflowed()) {
            if (retainCommittedMutationResultInMemory(
                    "mutation applied; result JSON exceeded its bounded capacity")) {
                return;
            }
            execution->responseStatus = 507;
            execution->errorCode = "result_json_overflow";
            execution->errorMessage = "job result JSON exceeded its bounded capacity";
            return;
        }

        if (execution->resource || execution->spoolResult) {
            const size_t resultBytes = measureJson(doc);
            const size_t resultLimit = execution->resource
                ? MAX_RESOURCE_CACHE_BYTES
                : MAX_JOB_RESULT_BYTES;
            if (resultBytes <= 2 || resultBytes > resultLimit) {
                if (retainCommittedMutationResultInMemory(
                        "mutation applied; full result exceeded the storage limit")) {
                    return;
                }
                execution->responseStatus = 507;
                execution->errorCode = "result_too_large";
                execution->errorMessage = "job result exceeds its bounded storage limit";
                return;
            }
            history_storage::Guard filesystem(5000);
            if (!filesystem) {
                if (retainCommittedMutationResultInMemory(
                        "mutation applied; result retained in memory because the filesystem was busy")) {
                    return;
                }
                execution->responseStatus = 503;
                execution->errorCode = "filesystem_busy";
                execution->errorMessage = "filesystem is busy";
                return;
            }
            if (!execution->resultReservationPath.isEmpty() &&
                LittleFS.exists(execution->resultReservationPath)) {
                LittleFS.remove(execution->resultReservationPath);
            }
            if (execution->spoolResult && !execution->resource &&
                jobSpoolBytesLocked(execution->resultPath) + resultBytes > MAX_JOB_SPOOL_TOTAL_BYTES) {
                if (retainCommittedMutationResultInMemory(
                        "mutation applied; result retained in memory because the spool was full")) {
                    return;
                }
                execution->responseStatus = 507;
                execution->errorCode = "result_storage_full";
                execution->errorMessage = "retained job results reached the bounded spool budget";
                return;
            }
            if (!littleFsCanAllocateLocked(resultBytes)) {
                if (retainCommittedMutationResultInMemory(
                        "mutation applied; result retained in memory to preserve history headroom")) {
                    return;
                }
                execution->responseStatus = 507;
                execution->errorCode = "result_storage_full";
                execution->errorMessage =
                    "job result would consume reserved history transaction headroom";
                return;
            }
            File resultFile = LittleFS.open(execution->resultPath, "w");
            if (!resultFile) {
                if (retainCommittedMutationResultInMemory(
                        "mutation applied; result retained in memory after a storage error")) {
                    return;
                }
                execution->responseStatus = 500;
                execution->errorCode = "result_storage_failed";
                execution->errorMessage = "failed to create job result file";
                return;
            }
            if (serializeJson(doc, resultFile) != resultBytes) {
                if (!retainCommittedMutationResultInMemory(
                        "mutation applied; result retained in memory after a write error")) {
                    execution->responseStatus = 500;
                    execution->errorCode = "result_storage_failed";
                    execution->errorMessage = "failed to serialize job result";
                }
            }
            resultFile.close();
            if (!bridge_runtime_policy::retainWorkerResultFile(
                    execution->resource, execution->spoolResult, execution->responseStatus) &&
                LittleFS.exists(execution->resultPath)) {
                LittleFS.remove(execution->resultPath);
            }
        } else {
            execution->responseBody = "";
            serializeJson(doc, execution->responseBody);
        }
        return;
    }

    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    if (doc.overflowed()) {
        server.send(507,
                    "application/json",
                    "{\"ok\":false,\"error\":\"response JSON exceeded its bounded capacity\"}");
        return;
    }
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

void maybeApplyClientTimeHeader() {
    if (currentWorkerExecution() != nullptr) {
        return;
    }
    if (!server.hasHeader(CLIENT_TIME_HEADER)) {
        return;
    }

    const String value = server.header(CLIENT_TIME_HEADER);
    if (value.isEmpty()) {
        return;
    }

    char* end = nullptr;
    const int64_t unixMs = strtoll(value.c_str(), &end, 10);
    if (end == value.c_str() || (end != nullptr && *end != '\0') || unixMs <= 0) {
        return;
    }

    bridge_time::seedFromUnixTime(static_cast<time_t>(unixMs / 1000), millis(), addTimeLog);
}

bool parseJsonBody(DynamicJsonDocument& doc, String& error) {
    WorkerExecutionContext* execution = currentWorkerExecution();
    if (execution != nullptr) {
        if (execution->requestBody.isEmpty()) {
            error = "missing request body";
            return false;
        }
        DeserializationError workerError = deserializeJson(doc, execution->requestBody);
        if (workerError) {
            error = String("invalid json: ") + workerError.c_str();
            return false;
        }
        return true;
    }

    maybeApplyClientTimeHeader();
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

uint32_t jobKeyHash(const String& value) {
    uint32_t hash = 2166136261u;
    for (size_t index = 0; index < value.length(); ++index) {
        hash ^= static_cast<uint8_t>(value.charAt(index));
        hash *= 16777619u;
    }
    return hash;
}

String defaultJobCoalesceKey(const String& kind,
                             const String& target,
                             BleOperation operation,
                             const String& argument,
                             const String& normalizedBody) {
    char suffix[48];
    std::snprintf(suffix,
                  sizeof(suffix),
                  ":%u:%08lx:%u:%08lx:%u",
                  static_cast<unsigned>(operation),
                  static_cast<unsigned long>(jobKeyHash(argument)),
                  static_cast<unsigned>(argument.length()),
                  static_cast<unsigned long>(jobKeyHash(normalizedBody)),
                  static_cast<unsigned>(normalizedBody.length()));
    return kind + ":" + target + suffix;
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

String savedRecipeSlotCachePath(const String& serial, uint8_t slotNumber) {
    return String(SAVED_RECIPE_CACHE_PREFIX) + cacheSafeToken(serial) + "-slot-" +
        slotNumber + ".json";
}

bool parseRefreshArg() {
    WorkerExecutionContext* execution = currentWorkerExecution();
    if (execution != nullptr) {
        return execution->forceRefresh;
    }
    if (!server.hasArg("refresh")) {
        return false;
    }
    const String rawValue = server.arg("refresh");
    return !(rawValue.isEmpty() || rawValue == "0" || rawValue.equalsIgnoreCase("false") || rawValue.equalsIgnoreCase("no"));
}

size_t parseLimitArg(const char* name, size_t defaultValue, size_t maxValue) {
    if (!server.hasArg(name)) {
        return defaultValue;
    }
    const String rawValue = server.arg(name);
    const long parsedValue = rawValue.toInt();
    if (parsedValue <= 0) {
        return defaultValue;
    }
    return std::min(static_cast<size_t>(parsedValue), maxValue);
}

size_t parseOffsetArg(const char* name, size_t defaultValue) {
    if (!server.hasArg(name)) {
        return defaultValue;
    }
    const String rawValue = server.arg(name);
    const long parsedValue = rawValue.toInt();
    if (parsedValue < 0) {
        return defaultValue;
    }
    return static_cast<size_t>(parsedValue);
}

bool parseUnsignedPathIndex(const String& value, size_t& parsedOut) {
    parsedOut = 0;
    if (value.isEmpty()) {
        return false;
    }

    char* end = nullptr;
    const unsigned long parsed = strtoul(value.c_str(), &end, 10);
    if (end == value.c_str() || (end != nullptr && *end != '\0')) {
        return false;
    }
    parsedOut = static_cast<size_t>(parsed);
    return true;
}

bool initializeLittleFs(String& error) {
    if (littleFsReady) {
        return true;
    }
    if (LittleFS.begin(false)) {
        littleFsReady = true;
        return true;
    }
    LittleFS.end();

    // Never auto-format a partition that may contain recoverable history. A
    // brand-new, fully erased partition is the only safe automatic format
    // case; every other mount failure leaves the bytes untouched.
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (partition == nullptr) {
        error = "failed to find the LittleFS partition; existing data was not modified";
        return false;
    }
    std::array<uint8_t, 256> buffer{};
    bool fullyErased = true;
    for (size_t offset = 0; offset < partition->size && fullyErased;
         offset += buffer.size()) {
        const size_t readBytes = std::min(buffer.size(), partition->size - offset);
        if (esp_partition_read(partition, offset, buffer.data(), readBytes) != ESP_OK) {
            error = "failed to inspect the LittleFS partition; existing data was not modified";
            return false;
        }
        for (size_t index = 0; index < readBytes; ++index) {
            if (buffer[index] != 0xFF) {
                fullyErased = false;
                break;
            }
        }
    }
    if (!fullyErased) {
        error = "failed to mount LittleFS; automatic formatting was refused to preserve history";
        return false;
    }
    if (!LittleFS.format() || !LittleFS.begin(false)) {
        error = "failed to initialize the erased LittleFS partition";
        return false;
    }
    littleFsReady = true;
    return true;
}

bool littleFsExistsLocked(const String& path) {
    history_storage::Guard filesystem(5000);
    return filesystem && LittleFS.exists(path);
}

bool littleFsRemoveLocked(const String& path) {
    history_storage::Guard filesystem(5000);
    return filesystem && (!LittleFS.exists(path) || LittleFS.remove(path));
}

size_t jobSpoolBytesLocked(const String& replacingPath = "") {
    size_t totalBytes = 0;
    File root = LittleFS.open("/");
    if (!root) {
        return 0;
    }
    File entry = root.openNextFile();
    while (entry) {
        String path = entry.name();
        if (!path.startsWith("/")) {
            path = String("/") + path;
        }
        if (path.startsWith("/job-") && path != replacingPath) {
            totalBytes += entry.size();
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();
    return totalBytes;
}

bool littleFsCanAllocateLocked(size_t incomingBytes) {
    const size_t totalBytes = LittleFS.totalBytes();
    const size_t usedBytes = LittleFS.usedBytes();
    if (usedBytes > totalBytes || incomingBytes > totalBytes - usedBytes) {
        return false;
    }
    const size_t freeAfterWrite = totalBytes - usedBytes - incomingBytes;
    return freeAfterWrite >= history_storage::writeReserveBytes(totalBytes);
}

bool prepareMutationResultStorage(WorkerExecutionContext& execution, String& error) {
    if (!littleFsReady || execution.resultPath.isEmpty()) {
        error = "mutation result storage is unavailable";
        return false;
    }
    execution.resultReservationPath = execution.resultPath + ".reserve";
    history_storage::Guard filesystem(5000);
    if (!filesystem) {
        error = "filesystem is busy";
        return false;
    }
    LittleFS.remove(execution.resultReservationPath);
    if (jobSpoolBytesLocked() + MUTATION_RESULT_RESERVATION_BYTES > MAX_JOB_SPOOL_TOTAL_BYTES) {
        error = "retained job results leave no bounded mutation result slot";
        return false;
    }
    if (!littleFsCanAllocateLocked(MUTATION_RESULT_RESERVATION_BYTES)) {
        error = "mutation result reservation would consume history transaction headroom";
        return false;
    }
    File reservation = LittleFS.open(execution.resultReservationPath, "w");
    if (!reservation) {
        error = "failed to reserve mutation result storage";
        return false;
    }
    static const uint8_t zeros[1024] = {};
    size_t remaining = MUTATION_RESULT_RESERVATION_BYTES;
    while (remaining > 0) {
        const size_t chunk = std::min(remaining, sizeof(zeros));
        if (reservation.write(zeros, chunk) != chunk) {
            reservation.close();
            LittleFS.remove(execution.resultReservationPath);
            error = "failed to allocate the bounded mutation result slot";
            return false;
        }
        remaining -= chunk;
        noteWorkerProgress();
    }
    reservation.close();
    return true;
}

String normalizedLittleFsPath(const String& rawPath) {
    return rawPath.startsWith("/") ? rawPath : String("/") + rawPath;
}

bool isHistoryArtifactPath(const String& rawPath) {
    const String path = normalizedLittleFsPath(rawPath);
    return path.startsWith("/brewhist-") || path.startsWith("/stathist-");
}

bool isPrimaryHistoryPath(const String& rawPath) {
    const String path = normalizedLittleFsPath(rawPath);
    return isHistoryArtifactPath(path) && path.endsWith(".jsonl");
}

bool collectHistoryPaths(std::vector<String>& pathsOut,
                         bool primaryOnly,
                         String& error) {
    pathsOut.clear();
    File root = LittleFS.open("/");
    if (!root) {
        error = "failed to open the history storage root";
        return false;
    }
    File entry = root.openNextFile();
    while (entry) {
        const String path = normalizedLittleFsPath(entry.name());
        entry.close();
        if (isHistoryArtifactPath(path) && (!primaryOnly || isPrimaryHistoryPath(path))) {
            if (pathsOut.size() >= MACHINE_GENERATION_CAPACITY * 4) {
                root.close();
                error = "history file count exceeds the bounded restore registry";
                return false;
            }
            pathsOut.push_back(path);
        }
        entry = root.openNextFile();
    }
    root.close();
    return true;
}

bool writeRestoreMarker(const char* path, const char* value, String& error) {
    File marker = LittleFS.open(path, "w");
    if (!marker) {
        error = String("failed to create restore marker ") + path;
        return false;
    }
    const size_t length = strlen(value);
    const bool written = marker.write(
        reinterpret_cast<const uint8_t*>(value), length) == length;
    marker.close();
    if (!written) {
        LittleFS.remove(path);
        error = String("failed to write restore marker ") + path;
        return false;
    }
    marker = LittleFS.open(path, "r");
    const bool verified = marker && marker.size() == length;
    if (marker) {
        marker.close();
    }
    if (!verified) {
        LittleFS.remove(path);
        error = String("failed to verify restore marker ") + path;
        return false;
    }
    return true;
}

bool restoreMarkerEquals(const char* path, const char* expected) {
    File marker = LittleFS.open(path, "r");
    if (!marker) {
        return false;
    }
    const size_t expectedLength = strlen(expected);
    if (marker.size() != expectedLength || expectedLength >= 16) {
        marker.close();
        return false;
    }
    char value[16]{};
    const bool read = marker.read(reinterpret_cast<uint8_t*>(value), expectedLength) ==
        expectedLength;
    marker.close();
    return read && memcmp(value, expected, expectedLength) == 0;
}

struct RestoreStateHeader {
    uint32_t magic{BACKUP_RESTORE_STATE_MAGIC};
    uint32_t historyBudgetBytes{0};
    uint32_t machinePayloadBytes{0};
    uint32_t machinePayloadChecksum{2166136261u};
};

uint32_t restoreStateChecksum(const uint8_t* data, size_t length) {
    uint32_t checksum = 2166136261u;
    for (size_t index = 0; index < length; ++index) {
        checksum ^= data[index];
        checksum *= 16777619u;
    }
    return checksum;
}

bool writeRestoreStateBackup(size_t historyBudgetBytes, String& error) {
    const String payload = lastPersistedMachinePayload;
    if (payload.isEmpty() || payload.length() > 24576) {
        error = "current machine store cannot be staged for restore rollback";
        return false;
    }
    RestoreStateHeader header;
    header.historyBudgetBytes = static_cast<uint32_t>(historyBudgetBytes);
    header.machinePayloadBytes = static_cast<uint32_t>(payload.length());
    header.machinePayloadChecksum = restoreStateChecksum(
        reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length());

    File state = LittleFS.open(BACKUP_RESTORE_STATE_PATH, "w");
    if (!state) {
        error = "failed to create the restore rollback state";
        return false;
    }
    const bool written =
        state.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
        state.write(reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length()) ==
            payload.length();
    state.close();
    if (!written) {
        LittleFS.remove(BACKUP_RESTORE_STATE_PATH);
        error = "failed to write the restore rollback state";
        return false;
    }

    state = LittleFS.open(BACKUP_RESTORE_STATE_PATH, "r");
    RestoreStateHeader verified;
    bool valid = state && state.size() == sizeof(header) + payload.length() &&
        state.read(reinterpret_cast<uint8_t*>(&verified), sizeof(verified)) == sizeof(verified) &&
        verified.magic == BACKUP_RESTORE_STATE_MAGIC &&
        verified.historyBudgetBytes == header.historyBudgetBytes &&
        verified.machinePayloadBytes == header.machinePayloadBytes &&
        verified.machinePayloadChecksum == header.machinePayloadChecksum;
    uint32_t storedChecksum = 2166136261u;
    size_t remaining = payload.length();
    uint8_t buffer[256];
    while (valid && remaining > 0) {
        const size_t chunk = std::min(remaining, sizeof(buffer));
        if (state.read(buffer, chunk) != chunk) {
            valid = false;
            break;
        }
        for (size_t index = 0; index < chunk; ++index) {
            storedChecksum ^= buffer[index];
            storedChecksum *= 16777619u;
        }
        remaining -= chunk;
    }
    valid = valid && storedChecksum == header.machinePayloadChecksum;
    if (state) {
        state.close();
    }
    if (!valid) {
        LittleFS.remove(BACKUP_RESTORE_STATE_PATH);
        error = "failed to verify the restore rollback state";
        return false;
    }
    return true;
}

bool restoreDurableStateBackup(String& error) {
    File state = LittleFS.open(BACKUP_RESTORE_STATE_PATH, "r");
    RestoreStateHeader header;
    if (!state || state.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header) ||
        header.magic != BACKUP_RESTORE_STATE_MAGIC || header.machinePayloadBytes == 0 ||
        header.machinePayloadBytes > 24576 ||
        state.size() != sizeof(header) + header.machinePayloadBytes) {
        if (state) {
            state.close();
        }
        error = "restore rollback state is missing or invalid";
        return false;
    }
    std::unique_ptr<char[]> payload(new (std::nothrow) char[header.machinePayloadBytes + 1]);
    if (!payload ||
        state.read(reinterpret_cast<uint8_t*>(payload.get()), header.machinePayloadBytes) !=
            header.machinePayloadBytes) {
        state.close();
        error = "failed to read the restore rollback state";
        return false;
    }
    state.close();
    payload[header.machinePayloadBytes] = '\0';
    if (restoreStateChecksum(reinterpret_cast<const uint8_t*>(payload.get()),
                             header.machinePayloadBytes) != header.machinePayloadChecksum) {
        error = "restore rollback state checksum failed";
        return false;
    }

    const size_t blobBytes = header.machinePayloadBytes + 1;
    std::unique_ptr<char[]> verification(new (std::nothrow) char[blobBytes]);
    if (!verification ||
        machinePreferences.putBytes(PREFS_MACHINE_STORE_BLOB, payload.get(), blobBytes) != blobBytes ||
        machinePreferences.getBytesLength(PREFS_MACHINE_STORE_BLOB) != blobBytes ||
        machinePreferences.getBytes(PREFS_MACHINE_STORE_BLOB,
                                    verification.get(),
                                    blobBytes) != blobBytes ||
        memcmp(verification.get(), payload.get(), blobBytes) != 0 ||
        machinePreferences.putUInt(PREFS_MACHINE_SCHEMA, 2) != sizeof(uint32_t) ||
        machinePreferences.getUInt(PREFS_MACHINE_SCHEMA, 0) != 2 ||
        preferences.putUInt(PREFS_HISTORY_MAX_BYTES, header.historyBudgetBytes) != sizeof(uint32_t) ||
        preferences.getUInt(PREFS_HISTORY_MAX_BYTES, 0) != header.historyBudgetBytes) {
        error = "failed to restore durable machine/history settings after interrupted restore";
        return false;
    }
    return true;
}

bool rollbackHistoryRestoreFiles(const std::vector<String>& originalPaths, String& error) {
    history_storage::Guard filesystem(5000);
    if (!filesystem) {
        error = "filesystem is busy during history restore rollback";
        return false;
    }
    std::vector<String> artifacts;
    if (!collectHistoryPaths(artifacts, false, error)) {
        return false;
    }
    bool ok = true;
    for (const String& path : artifacts) {
        if (path.endsWith(".restorebak")) {
            continue;
        }
        if (LittleFS.exists(path) && !LittleFS.remove(path)) {
            ok = false;
            error = String("failed to remove partial restored history file ") + path;
        }
    }
    for (const String& originalPath : originalPaths) {
        const String backupPath = originalPath + ".restorebak";
        if (!LittleFS.exists(backupPath)) {
            ok = false;
            error = String("missing history rollback file ") + backupPath;
            continue;
        }
        if (!LittleFS.rename(backupPath, originalPath)) {
            ok = false;
            error = String("failed to restore history rollback file ") + originalPath;
        }
    }
    return ok;
}

bool recoverInterruptedBundleRestore(String& error) {
    history_storage::Guard filesystem(5000);
    if (!filesystem) {
        error = "filesystem is busy during bundle-restore recovery";
        return false;
    }
    const bool active = LittleFS.exists(BACKUP_RESTORE_ACTIVE_MARKER);
    const bool staged = restoreMarkerEquals(BACKUP_RESTORE_STAGED_MARKER, "staged");
    const bool committed = restoreMarkerEquals(BACKUP_RESTORE_COMMIT_MARKER, "commit");
    std::vector<String> artifacts;
    if (!collectHistoryPaths(artifacts, false, error)) {
        return false;
    }
    std::vector<String> rollbackPaths;
    rollbackPaths.reserve(artifacts.size());
    for (const String& path : artifacts) {
        if (path.endsWith(".restorebak")) {
            rollbackPaths.push_back(path.substring(0, path.length() - 11));
        }
    }
    if (!active && !committed && rollbackPaths.empty()) {
        LittleFS.remove(BACKUP_RESTORE_STATE_PATH);
        LittleFS.remove(BACKUP_RESTORE_STAGED_MARKER);
        LittleFS.remove(BACKUP_RESTORE_COMMIT_MARKER);
        return true;
    }
    if (active && !committed && rollbackPaths.empty() &&
        !LittleFS.exists(BACKUP_RESTORE_STATE_PATH)) {
        LittleFS.remove(BACKUP_RESTORE_ACTIVE_MARKER);
        LittleFS.remove(BACKUP_RESTORE_STAGED_MARKER);
        return true;
    }
    if (committed) {
        bool ok = true;
        for (const String& originalPath : rollbackPaths) {
            if (!LittleFS.remove(originalPath + ".restorebak")) {
                ok = false;
            }
        }
        if (ok) {
            LittleFS.remove(BACKUP_RESTORE_STATE_PATH);
            LittleFS.remove(BACKUP_RESTORE_ACTIVE_MARKER);
            LittleFS.remove(BACKUP_RESTORE_STAGED_MARKER);
            LittleFS.remove(BACKUP_RESTORE_COMMIT_MARKER);
        } else {
            error = "failed to finish committed bundle-restore cleanup";
        }
        return ok;
    }

    // Before the staged marker is durable, any primary without a matching
    // rollback file is an untouched original. Once staged, every primary is a
    // newly installed restore output and must be removed on rollback.
    if (staged) {
        for (const String& path : artifacts) {
            if (!path.endsWith(".restorebak") && LittleFS.exists(path)) {
                LittleFS.remove(path);
            }
        }
    }
    bool ok = true;
    for (const String& originalPath : rollbackPaths) {
        if (!LittleFS.rename(originalPath + ".restorebak", originalPath)) {
            ok = false;
        }
    }
    String durableRollbackError;
    if (!restoreDurableStateBackup(durableRollbackError)) {
        ok = false;
        error = durableRollbackError;
    }
    if (ok) {
        LittleFS.remove(BACKUP_RESTORE_STATE_PATH);
        LittleFS.remove(BACKUP_RESTORE_ACTIVE_MARKER);
        LittleFS.remove(BACKUP_RESTORE_STAGED_MARKER);
        LittleFS.remove(BACKUP_RESTORE_COMMIT_MARKER);
    } else {
        error = "failed to roll back an interrupted bundle restore";
    }
    return ok;
}

void cleanupBootTemporaryFiles() {
    if (!littleFsReady) {
        return;
    }
    history_storage::Guard filesystem(5000);
    if (!filesystem) {
        addLog("fs", "Could not lock LittleFS for boot cleanup");
        return;
    }

    String restoreRecoveryError;
    if (!recoverInterruptedBundleRestore(restoreRecoveryError) && !restoreRecoveryError.isEmpty()) {
        addLog("backup", restoreRecoveryError);
        lastError = restoreRecoveryError;
        // Do not expose a partially recovered history store. Machine metadata
        // can still load from NVS, while all LittleFS-backed routes stay
        // unavailable until an operator repairs/reboots the bridge.
        littleFsReady = false;
        return;
    }

    std::vector<String> temporaryPaths;
    std::vector<String> liveBackupPaths;
    std::vector<String> historyRollbackOriginalPaths;
    std::vector<String> historyRewriteTemporaryPaths;
    temporaryPaths.reserve(16);
    liveBackupPaths.reserve(4);
    historyRollbackOriginalPaths.reserve(4);
    historyRewriteTemporaryPaths.reserve(4);
    File root = LittleFS.open("/");
    if (root) {
        File entry = root.openNextFile();
        while (entry) {
            String path = entry.name();
            if (!path.startsWith("/")) {
                path = String("/") + path;
            }
            entry.close();
            if (path.startsWith("/job-") || path.startsWith("/cache-") ||
                path == BACKUP_RESTORE_UPLOAD_PATH ||
                (path.startsWith(String(BACKUP_RESTORE_UPLOAD_PATH) + ".") && path.endsWith(".part"))) {
                temporaryPaths.push_back(path);
            } else if (path.startsWith("/live-") && path.endsWith(".bak")) {
                liveBackupPaths.push_back(path);
            } else if ((path.startsWith("/brewhist-") || path.startsWith("/stathist-")) &&
                       path.endsWith(".jsonl.bak")) {
                historyRollbackOriginalPaths.push_back(
                    path.substring(0, path.length() - 4));
            } else if ((path.startsWith("/brewhist-") || path.startsWith("/stathist-")) &&
                       path.endsWith(".jsonl.tmp")) {
                historyRewriteTemporaryPaths.push_back(path);
            }
            entry = root.openNextFile();
        }
        root.close();
    }
    for (const String& path : temporaryPaths) {
        LittleFS.remove(path);
    }
    for (const String& backupPath : liveBackupPaths) {
        String finalPath = backupPath.substring(0, backupPath.length() - 4);
        if (LittleFS.exists(finalPath)) {
            LittleFS.remove(backupPath);
        } else {
            LittleFS.rename(backupPath, finalPath);
        }
    }
    for (const String& originalPath : historyRollbackOriginalPaths) {
        String recoveryError;
        if (!history_storage::recoverFile(originalPath, recoveryError)) {
            lastError = recoveryError;
            addLog("history", String("Failed to recover history rollback: ") + recoveryError);
            littleFsReady = false;
            return;
        }
    }
    for (const String& temporaryPath : historyRewriteTemporaryPaths) {
        if (LittleFS.exists(temporaryPath)) {
            LittleFS.remove(temporaryPath);
        }
    }
}

void refreshCachedStorageTotals();

struct ConfiguredHistoryBudgets {
    size_t brewBytesPerMachine{brew_history::DEFAULT_HISTORY_BYTES};
    size_t statsBytesPerMachine{stats_history::DEFAULT_HISTORY_BYTES};
};

ConfiguredHistoryBudgets calculateHistoryBudgets(size_t requestedBrewBytes,
                                                 size_t filesystemBytes) {
    return {
        brew_history::clampBudgetBytes(requestedBrewBytes, filesystemBytes),
        stats_history::clampBudgetBytes(
            stats_history::DEFAULT_HISTORY_BYTES, filesystemBytes),
    };
}

void applyHistoryBudgets(const ConfiguredHistoryBudgets& budgets,
                         size_t filesystemBytes,
                         size_t preservedBrewFileBytes = 0,
                         size_t preservedStatsFileBytes = 0) {
    brew_history::configureBudget(
        budgets.brewBytesPerMachine, filesystemBytes, preservedBrewFileBytes);
    stats_history::configureBudget(
        budgets.statsBytesPerMachine, filesystemBytes, preservedStatsFileBytes);
}

void configureHistoryBudget() {
    history_storage::Guard filesystem;
    const size_t upperBytes = littleFsReady ? LittleFS.totalBytes() : brew_history::DEFAULT_HISTORY_BYTES;
    const size_t storedBytes = static_cast<size_t>(preferences.getUInt(PREFS_HISTORY_MAX_BYTES,
                                                                       static_cast<uint32_t>(brew_history::DEFAULT_HISTORY_BYTES)));
    const ConfiguredHistoryBudgets budgets = calculateHistoryBudgets(storedBytes, upperBytes);
    history_storage::HistoryUsage usage;
    String usageError;
    if (littleFsReady && !history_storage::inspectHistoryUsage(usage, usageError)) {
        addLog("history", String("Could not inspect preservation floor: ") + usageError);
    }
    applyHistoryBudgets(budgets,
                        upperBytes,
                        usage.largestBrewFileBytes,
                        usage.largestStatsFileBytes);
}

void refreshConfiguredHistoryBudgets() {
    if (!littleFsReady) {
        return;
    }
    configureHistoryBudget();
    refreshCachedStorageTotals();
}

void refreshCachedStorageTotals() {
    size_t filesystemTotal = 0;
    size_t filesystemUsed = 0;
    size_t historyFileCount = 0;
    size_t historyTotalBytes = 0;
    size_t largestBrewHistoryFileBytes = 0;
    size_t largestStatsHistoryFileBytes = 0;
    {
        history_storage::Guard filesystem(100);
        if (!filesystem || !littleFsReady) {
            return;
        }
        filesystemTotal = LittleFS.totalBytes();
        filesystemUsed = LittleFS.usedBytes();
        history_storage::HistoryUsage usage;
        String usageError;
        if (history_storage::inspectHistoryUsage(usage, usageError)) {
            historyFileCount = usage.fileCount;
            historyTotalBytes = usage.totalBytes;
            largestBrewHistoryFileBytes = usage.largestBrewFileBytes;
            largestStatsHistoryFileBytes = usage.largestStatsFileBytes;
        }
    }
    if (healthMutex != nullptr && xSemaphoreTake(healthMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bridgeHealth.littleFsTotalBytes = filesystemTotal;
        bridgeHealth.littleFsUsedBytes = filesystemUsed;
        bridgeHealth.historyFileCount = historyFileCount;
        bridgeHealth.historyTotalBytes = historyTotalBytes;
        bridgeHealth.largestBrewHistoryFileBytes = largestBrewHistoryFileBytes;
        bridgeHealth.largestStatsHistoryFileBytes = largestStatsHistoryFileBytes;
        xSemaphoreGive(healthMutex);
    }
}

void clearStandardRecipeCachesByPrefix(const String& prefix) {
    if (!littleFsReady) {
        return;
    }
    history_storage::Guard filesystem;
    if (!filesystem) {
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
    clearStandardRecipeCachesByPrefix(
        String(SAVED_RECIPE_CACHE_PREFIX) + cacheSafeToken(serial));
}

void clearAllSavedRecipeCaches() {
    if (!littleFsReady) {
        return;
    }
    clearStandardRecipeCachesByPrefix(String(SAVED_RECIPE_CACHE_PREFIX));
}

void purgeEphemeralFilesForRestore() {
    if (!littleFsReady) {
        return;
    }
    clearAllStandardRecipeCaches();
    clearAllSavedRecipeCaches();
    clearStandardRecipeCachesByPrefix("/live-");
    clearStandardRecipeCachesByPrefix("/cache-");
    clearStandardRecipeCachesByPrefix("/job-");
    if (cacheMutex != nullptr && xSemaphoreTake(cacheMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (resourceCaches) {
            for (size_t index = 0; index < RESOURCE_CACHE_CAPACITY; ++index) {
                resourceCaches[index] = {};
            }
        }
        xSemaphoreGive(cacheMutex);
    }
}

void clearBrewHistoryForMachine(const String& serial) {
    if (!littleFsReady || serial.isEmpty()) {
        return;
    }
    String error;
    if (!brew_history::clear(serial, error) && !error.isEmpty()) {
        addLog("history", String("Brew history clear skipped: ") + error);
    }
}

void clearAllBrewHistory() {
    if (!littleFsReady) {
        return;
    }
    String error;
    if (!brew_history::clearAll(error) && !error.isEmpty()) {
        addLog("history", String("Brew history reset skipped: ") + error);
    }
}

void clearStatsHistoryForMachine(const String& serial) {
    if (!littleFsReady || serial.isEmpty()) {
        return;
    }
    String error;
    if (!stats_history::clear(serial, error) && !error.isEmpty()) {
        addLog("stats-history", String("Stats history clear skipped: ") + error);
    }
}

void clearAllStatsHistory() {
    if (!littleFsReady) {
        return;
    }
    String error;
    if (!stats_history::clearAll(error) && !error.isEmpty()) {
        addLog("stats-history", String("Stats history reset skipped: ") + error);
    }
}

bool readTextLine(BackupReader& file, String& lineOut, String& error) {
    error = "";
    while (file.available()) {
        lineOut = "";
        lineOut.reserve(512);
        while (file.available()) {
            const int value = file.read();
            if (value < 0) {
                error = "failed to read backup bundle";
                return false;
            }
            if (value == '\n') {
                break;
            }
            if (lineOut.length() >= MAX_BACKUP_JSON_LINE_BYTES) {
                error = "backup record exceeds the bounded line limit";
                return false;
            }
            if (!lineOut.concat(static_cast<char>(value))) {
                error = "insufficient memory for bounded backup record";
                return false;
            }
        }
        if (lineOut.endsWith("\r")) {
            lineOut.remove(lineOut.length() - 1);
        }
        if (!lineOut.isEmpty()) {
            return true;
        }
    }
    lineOut = "";
    return false;
}

bool removeBackupRestoreUpload() {
    history_storage::Guard filesystem;
    if (!filesystem || !littleFsReady) return false;
    const bool chunksRemoved = backup_staging::removeAll(backupRestoreUploadStore);
    const bool legacyRemoved = !LittleFS.exists(BACKUP_RESTORE_UPLOAD_PATH) ||
        LittleFS.remove(BACKUP_RESTORE_UPLOAD_PATH);
    return chunksRemoved && legacyRemoved;
}

void resetBackupRestoreUploadState(bool removeFiles = true) {
    history_storage::Guard filesystem;
    if (!filesystem) {
        backupRestoreUploadError = "filesystem is busy";
        return;
    }
    backupRestoreUploadWriter.reset();
    backupRestoreUploadError = "";
    backupRestoreUploadFilename = "";
    backupRestoreUploadBytes = 0;
    backupRestoreUploadComplete = false;
    if (removeFiles && littleFsReady && !removeBackupRestoreUpload()) {
        backupRestoreUploadError = "failed to remove prior backup staging chunks";
    }
}

String backupDownloadFilename() {
    String suffix = "unsynced";
    const bridge_time::StatusSnapshot timeStatus = bridge_time::snapshot();
    if (timeStatus.synced) {
        suffix = timeStatus.iso8601Utc;
        suffix.replace(":", "");
        suffix.replace("-", "");
    }
    suffix.replace("T", "-");
    suffix.replace("Z", "Z");
    return String(APP_NAME) + "-backup-" + suffix + ".ndjson";
}

bool loadStandardRecipeCache(const SavedMachine& machine,
                             uint8_t selector,
                             DynamicJsonDocument& cacheDoc,
                             String& error) {
    if (!littleFsReady) {
        error = "standard recipe cache is unavailable";
        return false;
    }
    history_storage::Guard filesystem;
    if (!filesystem) {
        error = "filesystem is busy";
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
    WorkerMachineWriteGuard machineGuard;
    if (!machineGuard) {
        error = "machine was deleted or replaced while the job was running";
        return false;
    }
    history_storage::Guard filesystem;
    if (!filesystem) {
        error = "filesystem is busy";
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

bool writeGeneratedJsonLiteral(const String& path,
                               const String& literal,
                               bool truncate,
                               size_t maximumBytes,
                               bool countTowardJobSpool,
                               String& error) {
    if (!littleFsReady) {
        error = "LittleFS is unavailable";
        return false;
    }
    history_storage::Guard filesystem;
    if (!filesystem) {
        error = "filesystem is busy";
        return false;
    }
    if (truncate) {
        LittleFS.remove(path);
    }
    size_t currentBytes = 0;
    if (!truncate && LittleFS.exists(path)) {
        File existing = LittleFS.open(path, "r");
        if (!existing) {
            error = "failed to inspect generated JSON file";
            return false;
        }
        currentBytes = existing.size();
        existing.close();
    }
    const size_t projectedBytes = currentBytes + literal.length();
    if (projectedBytes > maximumBytes) {
        error = "generated JSON exceeds its bounded file size";
        return false;
    }
    if (countTowardJobSpool &&
        jobSpoolBytesLocked(path) + projectedBytes > MAX_JOB_SPOOL_TOTAL_BYTES) {
        error = "retained job results reached the bounded spool budget";
        return false;
    }
    if (!littleFsCanAllocateLocked(literal.length())) {
        error = "generated JSON would consume reserved history headroom";
        return false;
    }
    File output = LittleFS.open(path, truncate ? "w" : "a");
    if (!output) {
        error = "failed to open generated JSON file";
        return false;
    }
    const size_t written = output.write(
        reinterpret_cast<const uint8_t*>(literal.c_str()), literal.length());
    output.close();
    if (written != literal.length()) {
        error = "failed to write generated JSON file";
        return false;
    }
    return true;
}

bool appendGeneratedJsonValue(const String& path,
                              JsonVariantConst value,
                              bool prependComma,
                              size_t maximumBytes,
                              bool countTowardJobSpool,
                              String& error) {
    const size_t jsonBytes = measureJson(value);
    if (jsonBytes == 0) {
        error = "generated JSON value is empty";
        return false;
    }
    history_storage::Guard filesystem;
    if (!filesystem) {
        error = "filesystem is busy";
        return false;
    }
    File existing = LittleFS.open(path, "r");
    if (!existing) {
        error = "generated JSON file is missing";
        return false;
    }
    const size_t currentBytes = existing.size();
    existing.close();
    const size_t appendBytes = jsonBytes + (prependComma ? 1 : 0);
    const size_t projectedBytes = currentBytes + appendBytes;
    if (projectedBytes > maximumBytes) {
        error = "generated JSON exceeds its bounded file size";
        return false;
    }
    if (countTowardJobSpool &&
        jobSpoolBytesLocked(path) + projectedBytes > MAX_JOB_SPOOL_TOTAL_BYTES) {
        error = "retained job results reached the bounded spool budget";
        return false;
    }
    if (!littleFsCanAllocateLocked(appendBytes)) {
        error = "generated JSON would consume reserved history headroom";
        return false;
    }
    File output = LittleFS.open(path, "a");
    if (!output) {
        error = "failed to append generated JSON file";
        return false;
    }
    bool ok = true;
    if (prependComma) {
        ok = output.write(static_cast<uint8_t>(',')) == 1;
    }
    if (ok) {
        ok = serializeJson(value, output) == jsonBytes;
    }
    output.close();
    if (!ok) {
        error = "failed to serialize generated JSON value";
        return false;
    }
    return true;
}

bool installGeneratedCacheFile(const String& temporaryPath,
                               const String& finalPath,
                               size_t maximumBytes,
                               String& error) {
    history_storage::Guard filesystem;
    if (!filesystem) {
        error = "filesystem is busy";
        return false;
    }
    File candidate = LittleFS.open(temporaryPath, "r");
    if (!candidate || candidate.size() <= 2 || candidate.size() > maximumBytes) {
        if (candidate) {
            candidate.close();
        }
        error = "generated saved-recipe cache is empty or oversized";
        return false;
    }
    const size_t expectedBytes = candidate.size();
    const int first = candidate.read();
    candidate.seek(expectedBytes - 1);
    const int last = candidate.read();
    candidate.close();
    if (first != '{' || last != '}') {
        error = "generated saved-recipe cache is not a JSON object";
        return false;
    }

    const String backupPath = finalPath + ".bak";
    LittleFS.remove(backupPath);
    const bool hadOriginal = LittleFS.exists(finalPath);
    if (hadOriginal && !LittleFS.rename(finalPath, backupPath)) {
        error = "failed to preserve the previous saved-recipe cache";
        return false;
    }
    if (!LittleFS.rename(temporaryPath, finalPath)) {
        if (hadOriginal) {
            LittleFS.rename(backupPath, finalPath);
        }
        error = "failed to publish the saved-recipe cache";
        return false;
    }
    File installed = LittleFS.open(finalPath, "r");
    const bool valid = installed && installed.size() == expectedBytes;
    if (installed) {
        installed.close();
    }
    if (!valid) {
        LittleFS.remove(finalPath);
        if (hadOriginal) {
            LittleFS.rename(backupPath, finalPath);
        }
        error = "published saved-recipe cache failed validation";
        return false;
    }
    LittleFS.remove(backupPath);
    return true;
}

String savedRecipeListHeader(const SavedMachine& machine, bool cached, String& error) {
    DynamicJsonDocument header(1024);
    header["ok"] = true;
    header["source"] = cached ? "cache" : "live";
    header["cached"] = cached;
    if (cached) {
        header["schema"] = SAVED_RECIPE_CACHE_SCHEMA;
        header["cacheKind"] = "mycoffee_list";
        header["serial"] = machine.serial;
        header["familyKey"] = machine.familyKey;
    }
    header.createNestedArray("recipes");
    String serializedHeader;
    serializeJson(header, serializedHeader);
    if (header.overflowed() || !serializedHeader.endsWith("[]}")) {
        error = "failed to build the bounded saved-recipe list header";
        return "";
    }
    serializedHeader.remove(serializedHeader.length() - 2);
    return serializedHeader;
}

String savedRecipeSlotHeader(const SavedMachine& machine,
                             uint8_t slotNumber,
                             String& error) {
    DynamicJsonDocument header(1024);
    header["ok"] = true;
    header["source"] = "cache";
    header["cached"] = true;
    header["schema"] = SAVED_RECIPE_SLOT_CACHE_SCHEMA;
    header["cacheKind"] = "mycoffee_slot";
    header["serial"] = machine.serial;
    header["familyKey"] = machine.familyKey;
    header["slot"] = slotNumber;
    String serializedHeader;
    serializeJson(header, serializedHeader);
    if (header.overflowed() || !serializedHeader.endsWith("}")) {
        error = "failed to build the bounded saved-recipe slot header";
        return "";
    }
    serializedHeader.remove(serializedHeader.length() - 1);
    serializedHeader += ",\"recipe\":";
    return serializedHeader;
}

bool writeSavedRecipeSlotCacheTemporary(const SavedMachine& machine,
                                         JsonObjectConst recipe,
                                         const String& temporaryPath,
                                         String& error) {
    const int slotNumber = recipe["slot"] | 0;
    if (slotNumber <= 0 || slotNumber > 255) {
        error = "saved recipe cache entry is missing a valid slot number";
        return false;
    }
    const String header = savedRecipeSlotHeader(
        machine, static_cast<uint8_t>(slotNumber), error);
    return !header.isEmpty() &&
        writeGeneratedJsonLiteral(
            temporaryPath, header, true, MAX_RESOURCE_CACHE_BYTES, false, error) &&
        appendGeneratedJsonValue(
            temporaryPath, recipe, false, MAX_RESOURCE_CACHE_BYTES, false, error) &&
        writeGeneratedJsonLiteral(
            temporaryPath, "}", false, MAX_RESOURCE_CACHE_BYTES, false, error);
}

bool validateSavedRecipeCache(const SavedMachine& machine,
                              const String& path,
                              const char* expectedKind,
                              uint32_t expectedSchema,
                              uint8_t expectedSlot,
                              String& error) {
    if (!littleFsReady) {
        error = "saved recipe cache is unavailable";
        return false;
    }
    history_storage::Guard filesystem(1000);
    if (!filesystem) {
        error = "filesystem is busy";
        return false;
    }
    File cacheFile = LittleFS.open(path, "r");
    if (!cacheFile || cacheFile.size() <= 2 || cacheFile.size() > MAX_JOB_RESULT_BYTES) {
        if (cacheFile) {
            cacheFile.close();
        }
        error = "saved recipe cache miss";
        return false;
    }
    DynamicJsonDocument filter(384);
    filter["schema"] = true;
    filter["cacheKind"] = true;
    filter["serial"] = true;
    filter["familyKey"] = true;
    filter["slot"] = true;
    DynamicJsonDocument metadata(768);
    const DeserializationError parseError = deserializeJson(
        metadata, cacheFile, DeserializationOption::Filter(filter));
    cacheFile.close();
    if (parseError) {
        error = String("failed to parse saved recipe cache: ") + parseError.c_str();
        return false;
    }
    const String cachedSerial = metadata["serial"] | "";
    const String cachedFamily = metadata["familyKey"] | "";
    const String cacheKind = metadata["cacheKind"] | "";
    if ((metadata["schema"] | 0U) != expectedSchema ||
        cacheKind != expectedKind ||
        !cachedSerial.equalsIgnoreCase(machine.serial) ||
        cachedFamily != machine.familyKey ||
        (expectedSlot != 0 && (metadata["slot"] | 0U) != expectedSlot)) {
        error = "saved recipe cache contents mismatch";
        return false;
    }
    return true;
}

bool upsertSavedRecipeCacheEntry(const SavedMachine& machine,
                                 JsonObjectConst recipe,
                                 String& error) {
    const int slotNumber = recipe["slot"] | 0;
    if (slotNumber <= 0 || slotNumber > 255) {
        error = "saved recipe cache entry is missing a valid slot number";
        return false;
    }
    WorkerExecutionContext* execution = currentWorkerExecution();
    const String token = execution != nullptr ? execution->jobId : String(millis());
    const String finalPath = savedRecipeSlotCachePath(
        machine.serial, static_cast<uint8_t>(slotNumber));
    const String temporaryPath = finalPath + "." + token + ".tmp";
    if (!writeSavedRecipeSlotCacheTemporary(machine, recipe, temporaryPath, error)) {
        littleFsRemoveLocked(temporaryPath);
        return false;
    }
    WorkerMachineWriteGuard machineGuard;
    if (!machineGuard) {
        littleFsRemoveLocked(temporaryPath);
        error = "machine was deleted or replaced while the job was running";
        return false;
    }
    if (!installGeneratedCacheFile(
            temporaryPath, finalPath, MAX_RESOURCE_CACHE_BYTES, error)) {
        littleFsRemoveLocked(temporaryPath);
        return false;
    }
    // A single changed slot cannot safely update the streamed full-list cache
    // in place. Invalidate only that aggregate; the refreshed slot remains
    // available from its independent bounded cache.
    if (!littleFsRemoveLocked(savedRecipeCachePath(machine.serial))) {
        error = "failed to invalidate the saved-recipe list cache";
        return false;
    }
    return true;
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
    {
        WorkerBleCallScope bleCall;
        scan.stop();
        scan.clearResults();
    }
    scan.setScanCallbacks(scanCallbacksPtr, false);
    scan.setActiveScan(true);
    scan.setInterval(100);
    scan.setWindow(30);
    scan.setDuplicateFilter(1);
    scan.setMaxResults(0);
}

void publishScanResults(uint32_t startedAtMs, bool updateIdleTimestamp, const String& label) {
    copyScanScratchToPublished();
    lastScanAtMs = millis();
    if (scanDataMutex != nullptr && xSemaphoreTake(scanDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        lastScanResultCount = scannedDevices.size();
        xSemaphoreGive(scanDataMutex);
    }
    if (updateIdleTimestamp) {
        lastIdleScanAtMs = lastScanAtMs;
    }
    if (machineMutex != nullptr && xSemaphoreTake(machineMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        refreshAllSavedMachinePresence();
        xSemaphoreGive(machineMutex);
    }
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
        WorkerBleCallScope bleCall;
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
    bool started = false;
    {
        WorkerBleCallScope bleCall;
        started = scan->start(SCAN_MS, false, false);
    }
    if (!started) {
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

SavedMachine* findSavedMachineBySerialGlobal(const String& serial) {
    for (auto& machine : savedMachines) {
        if (machine.serial.equalsIgnoreCase(serial)) {
            return &machine;
        }
    }
    return nullptr;
}

SavedMachine* findSavedMachineBySerial(const String& serial) {
    WorkerExecutionContext* execution = currentWorkerExecution();
    if (execution != nullptr) {
        if (execution->machineLoaded && execution->machine.serial.equalsIgnoreCase(serial)) {
            return &execution->machine;
        }
        return nullptr;
    }
    return findSavedMachineBySerialGlobal(serial);
}

SavedMachine* findSavedMachineByAddressGlobal(const String& address) {
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

SavedMachine* findSavedMachineByAddress(const String& address) {
    WorkerExecutionContext* execution = currentWorkerExecution();
    if (execution != nullptr) {
        if (execution->machineLoaded && execution->machine.address.equalsIgnoreCase(address)) {
            return &execution->machine;
        }
        return nullptr;
    }
    return findSavedMachineByAddressGlobal(address);
}

void refreshSavedMachinePresence(SavedMachine& machine) {
    machine.lastSeenAtMs = 0;
    machine.lastSeenRssi = 0;
    const bool locked = scanDataMutex != nullptr &&
        xSemaphoreTake(scanDataMutex, pdMS_TO_TICKS(100)) == pdTRUE;
    if (scanDataMutex != nullptr && !locked) {
        return;
    }
    for (const auto& record : scannedDevices) {
        if (record.address.equalsIgnoreCase(machine.address)) {
            machine.lastSeenAtMs = record.seenAtMs;
            machine.lastSeenRssi = record.rssi;
            machine.addressType = record.addressType;
            break;
        }
    }
    if (locked) {
        xSemaphoreGive(scanDataMutex);
    }
}

void markSavedMachineReachableFromLiveSession(SavedMachine& machine) {
    refreshSavedMachinePresence(machine);
    if (machine.lastSeenAtMs > 0) {
        return;
    }

    machine.lastSeenAtMs = millis();
    machine.lastSeenRssi = 0;
    if (machine.address.equalsIgnoreCase(selectedAddress)) {
        machine.addressType = selectedAddressType;
    }
}

void refreshAllSavedMachinePresence() {
    for (auto& machine : savedMachines) {
        refreshSavedMachinePresence(machine);
    }
}

bool validateSavedMachineField(const String& value,
                               size_t maximumBytes,
                               const char* name,
                               String& error) {
    if (value.length() <= maximumBytes) {
        return true;
    }
    error = String("machine ") + name + " exceeds " + maximumBytes + " bytes";
    return false;
}

bool validateSavedMachineDurableFields(const SavedMachine& machine, String& error) {
    error = "";
    return validateSavedMachineField(machine.serial, MAX_MACHINE_SERIAL_BYTES, "serial", error) &&
        validateSavedMachineField(machine.alias, MAX_MACHINE_ALIAS_BYTES, "alias", error) &&
        validateSavedMachineField(machine.manufacturer,
                                  MAX_MACHINE_MANUFACTURER_BYTES,
                                  "manufacturer",
                                  error) &&
        validateSavedMachineField(machine.model, MAX_MACHINE_MODEL_BYTES, "model", error) &&
        validateSavedMachineField(machine.modelCode,
                                  MAX_MACHINE_MODEL_CODE_BYTES,
                                  "modelCode",
                                  error) &&
        validateSavedMachineField(machine.modelName,
                                  MAX_MACHINE_MODEL_NAME_BYTES,
                                  "modelName",
                                  error) &&
        validateSavedMachineField(machine.familyKey,
                                  MAX_MACHINE_FAMILY_KEY_BYTES,
                                  "familyKey",
                                  error) &&
        validateSavedMachineField(machine.hardwareRevision,
                                  MAX_MACHINE_REVISION_BYTES,
                                  "hardwareRevision",
                                  error) &&
        validateSavedMachineField(machine.firmwareRevision,
                                  MAX_MACHINE_REVISION_BYTES,
                                  "firmwareRevision",
                                  error) &&
        validateSavedMachineField(machine.softwareRevision,
                                  MAX_MACHINE_REVISION_BYTES,
                                  "softwareRevision",
                                  error) &&
        validateSavedMachineField(machine.ad06Hex,
                                  MAX_MACHINE_AD06_HEX_BYTES,
                                  "ad06Hex",
                                  error) &&
        validateSavedMachineField(machine.ad06Ascii,
                                  MAX_MACHINE_AD06_ASCII_BYTES,
                                  "ad06Ascii",
                                  error);
}

bool persistSavedMachines(String* errorOut = nullptr) {
    if (errorOut != nullptr) {
        *errorOut = "";
    }
    DynamicJsonDocument doc(2048 + (savedMachines.size() * 1280));
    JsonArray items = doc.createNestedArray("machines");
    for (const auto& machine : savedMachines) {
        String validationError;
        if (!validateSavedMachineDurableFields(machine, validationError)) {
            machinePersistenceDirty = true;
            if (errorOut != nullptr) {
                *errorOut = validationError;
            }
            addLog("machines", validationError);
            return false;
        }
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
        item["savedAtMs"] = machine.savedAtMs;
    }

    if (doc.overflowed()) {
        machinePersistenceDirty = true;
        if (errorOut != nullptr) {
            *errorOut = "machine-store JSON capacity was exceeded";
        }
        addLog("machines", "Machine-store JSON capacity was exceeded");
        return false;
    }

    String payload;
    serializeJson(doc, payload);
    if (payload.length() + 1 > 24576) {
        machinePersistenceDirty = true;
        if (errorOut != nullptr) {
            *errorOut = "machine-store blob exceeds the supported 24 KiB limit";
        }
        addLog("machines", "Machine-store blob exceeds the supported 24 KiB limit");
        return false;
    }
    if (payload == lastPersistedMachinePayload && !machinePersistenceDirty) {
        machinePersistenceDirty = false;
        return true;
    }
    const size_t payloadBytes = payload.length() + 1;
    const size_t written = machinePreferences.putBytes(
        PREFS_MACHINE_STORE_BLOB, payload.c_str(), payloadBytes);
    if (written != payloadBytes) {
        machinePersistenceDirty = true;
        if (errorOut != nullptr) {
            *errorOut = "failed to persist the schema-2 machine-store blob";
        }
        addLog("machines", "Failed to persist the schema-2 machine-store blob");
        return false;
    }
    std::unique_ptr<char[]> verification(new (std::nothrow) char[payloadBytes]);
    if (!verification ||
        machinePreferences.getBytesLength(PREFS_MACHINE_STORE_BLOB) != payloadBytes ||
        machinePreferences.getBytes(PREFS_MACHINE_STORE_BLOB,
                                    verification.get(),
                                    payloadBytes) != payloadBytes ||
        memcmp(verification.get(), payload.c_str(), payloadBytes) != 0) {
        machinePersistenceDirty = true;
        if (errorOut != nullptr) {
            *errorOut = "machine-store blob verification failed";
        }
        addLog("machines", "Machine-store blob verification failed");
        return false;
    }
    if (machinePreferences.putUInt(PREFS_MACHINE_SCHEMA, 2) != sizeof(uint32_t) ||
        machinePreferences.getUInt(PREFS_MACHINE_SCHEMA, 0) != 2) {
        machinePersistenceDirty = true;
        if (errorOut != nullptr) {
            *errorOut = "failed to persist the machine-store schema marker";
        }
        addLog("machines", "Failed to persist the machine-store schema marker");
        return false;
    }
    lastPersistedMachinePayload = payload;
    machinePersistenceDirty = false;
    durableMachineWriteCount++;
    return true;
}

void loadSavedMachines() {
    savedMachines.clear();
    String payload;
    const size_t blobBytes = machinePreferences.getBytesLength(PREFS_MACHINE_STORE_BLOB);
    if (blobBytes > 0 && blobBytes <= 24576) {
        std::unique_ptr<char[]> stored(new (std::nothrow) char[blobBytes]);
        if (stored &&
            machinePreferences.getBytes(PREFS_MACHINE_STORE_BLOB,
                                        stored.get(),
                                        blobBytes) == blobBytes &&
            stored[blobBytes - 1] == '\0') {
            payload = stored.get();
        } else {
            addLog("machines", "Failed to read the schema-2 machine-store blob");
        }
    }
    if (payload.isEmpty()) {
        // Schema 1 and early schema-2 builds used an NVS string. Keep it as a
        // read-only migration fallback; the next durable change writes a blob.
        payload = machinePreferences.getString(PREFS_MACHINE_STORE, "");
    }
    lastPersistedMachinePayload = payload;
    machinePersistenceDirty = false;
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
        // Schema 1 persisted volatile presence. Schema 2 deliberately starts
        // presence cold so routine scans never cause NVS writes.
        machine.lastSeenRssi = 0;
        machine.lastSeenAtMs = 0;
        machine.savedAtMs = item["savedAtMs"] | 0;
        String validationError;
        if (!validateSavedMachineDurableFields(machine, validationError)) {
            addLog("machines", String("Ignored invalid saved-machine record: ") + validationError);
            continue;
        }
        if (!machine.serial.isEmpty()) {
            if (savedMachines.size() >= MACHINE_GENERATION_CAPACITY) {
                addLog("machines", "Saved-machine store exceeds the supported capacity; extra records were ignored");
                break;
            }
            machine.generation = allocateMachineGeneration();
            savedMachines.push_back(machine);
            MachineGenerationLock generationLock;
            if (!generationLock || !setMachineGenerationLocked(savedMachines.back())) {
                savedMachines.pop_back();
                addLog("machines", "Failed to register a saved-machine generation");
                break;
            }
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
    if (machine.savedAtMs == 0) {
        machine.savedAtMs = millis();
    }
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

void appendBackupMachineJson(JsonObject target, const SavedMachine& machine) {
    target["serial"] = machine.serial;
    target["alias"] = machine.alias;
    target["address"] = machine.address;
    target["addressType"] = machine.addressType;
    target["manufacturer"] = machine.manufacturer;
    target["model"] = machine.model;
    target["modelCode"] = machine.modelCode;
    target["modelName"] = machine.modelName;
    target["familyKey"] = machine.familyKey;
    target["hardwareRevision"] = machine.hardwareRevision;
    target["firmwareRevision"] = machine.firmwareRevision;
    target["softwareRevision"] = machine.softwareRevision;
    target["ad06Hex"] = machine.ad06Hex;
    target["ad06Ascii"] = machine.ad06Ascii;
    target["savedAtMs"] = machine.savedAtMs;
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

String formatByteHex(uint8_t value) {
    String text = String(value, HEX);
    text.toUpperCase();
    if (text.length() < 2) {
        text = "0" + text;
    }
    return String("0x") + text;
}

String bridgeId() {
    const uint64_t efuseMac = ESP.getEfuseMac() & 0xFFFFFFFFFFFFULL;
    char suffix[13];
    std::snprintf(suffix, sizeof(suffix), "%012llX", static_cast<unsigned long long>(efuseMac));
    return String(APP_NAME) + "-" + suffix;
}

void appendSettingOptions(JsonArray options, const nivona::SettingProbeDescriptor& probe) {
    for (size_t index = 0; index < probe.optionCount; ++index) {
        JsonObject option = options.createNestedObject();
        option["code"] = probe.options[index].code;
        option["label"] = probe.options[index].label != nullptr ? probe.options[index].label : "";
    }
}

void appendMachineFeaturesJson(JsonObject target, const nivona::MachineFeatures& features) {
    target["ok"] = features.ok;
    target["payloadSize"] = features.payload.size();
    target["rawHex"] = nivona::hexEncode(features.payload);
    target["imageTransfer"] = features.imageTransfer;
    target["source"] = "de.nivona.mobileapp 3.8.6";

    JsonArray bytes = target.createNestedArray("bytes");
    for (uint8_t value : features.payload) {
        bytes.add(value);
    }

    JsonObject appKnown = target.createNestedObject("appKnown");
    appKnown["imageTransfer"] = features.imageTransfer;

    uint8_t knownMasks[nivona::HI_FEATURE_PAYLOAD_SIZE] = {0};
    size_t knownEnabledCount = 0;
    std::vector<const nivona::MachineFeatureDescriptor*> descriptors;
    nivona::selectMachineFeatures(descriptors);
    JsonArray knownFlags = target.createNestedArray("knownFlags");
    for (const auto* descriptor : descriptors) {
        if (descriptor == nullptr) {
            continue;
        }
        const bool enabled = nivona::hiFeatureEnabled(features, *descriptor);
        JsonObject item = knownFlags.createNestedObject();
        item["key"] = descriptor->key;
        item["title"] = descriptor->title;
        item["byteIndex"] = descriptor->byteIndex;
        item["mask"] = descriptor->mask;
        item["maskHex"] = formatByteHex(descriptor->mask);
        item["enabled"] = enabled;
        item["source"] = "apk-3.8.6";
        if (descriptor->byteIndex < nivona::HI_FEATURE_PAYLOAD_SIZE) {
            knownMasks[descriptor->byteIndex] = static_cast<uint8_t>(knownMasks[descriptor->byteIndex] | descriptor->mask);
        }
        if (enabled) {
            ++knownEnabledCount;
        }
    }
    target["knownFlagCount"] = descriptors.size();
    target["knownEnabledCount"] = knownEnabledCount;

    size_t unknownNonZeroBitCount = 0;
    JsonArray unknownBits = target.createNestedArray("unknownNonZeroBits");
    for (size_t index = 0; index < features.payload.size(); ++index) {
        const uint8_t value = features.payload[index];
        const uint8_t unknownMask = static_cast<uint8_t>(value & static_cast<uint8_t>(~knownMasks[index]));
        for (uint8_t bit = 0; bit < 8; ++bit) {
            const uint8_t mask = static_cast<uint8_t>(1u << bit);
            if ((unknownMask & mask) == 0) {
                continue;
            }
            JsonObject item = unknownBits.createNestedObject();
            item["byteIndex"] = index;
            item["mask"] = mask;
            item["maskHex"] = formatByteHex(mask);
            item["value"] = value;
            item["valueHex"] = formatByteHex(value);
            ++unknownNonZeroBitCount;
        }
    }
    target["unknownNonZeroBitCount"] = unknownNonZeroBitCount;
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

void appendSavedRecipeIconMetadata(JsonObject target, const nivona::ModelInfo& modelInfo) {
    const bool hasTypeSelector = !target["typeSelector"].isNull();
    const int32_t typeSelector = hasTypeSelector ? target["typeSelector"].as<int32_t>() : -1;
    const bool hasIconValue = !target["icon"].isNull();
    const int32_t rawIconValue = hasIconValue ? target["icon"].as<int32_t>() : -1;
    recipe_icons::appendMetadata(target,
                                 recipe_icons::keyForSavedRecipe(modelInfo,
                                                                 hasTypeSelector,
                                                                 typeSelector,
                                                                 hasIconValue,
                                                                 rawIconValue));
}

void appendStandardRecipeIconMetadata(JsonObject target, const nivona::ModelInfo& modelInfo, int32_t selector) {
    recipe_icons::appendMetadata(target, recipe_icons::keyForStandardRecipe(modelInfo, selector));
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

void startWifiStationAttempt(uint32_t nowMs) {
    const String wifiPass = loadPrefString(PREFS_PASS);
    WiFi.begin(wifiStaSsid.c_str(), wifiPass.c_str());
    wifiConnectionAttemptStartedAtMs = nowMs;
    wifiConnectionAttemptActive = true;
    wifiNextReconnectAtMs = 0;
    addLog("wifi", String("Started nonblocking connection to ") + wifiStaSsid);
}

void connectWifi() {
    wifiStaSsid = loadPrefString(PREFS_SSID);
    wifiConnectionAttemptActive = false;
    wifiNextReconnectAtMs = 0;
    wifiStationWasConnected = false;

    const bool configured = !wifiStaSsid.isEmpty();
    if (wifi_runtime_policy::shouldRunSetupAccessPoint(configured)) {
        WiFi.mode(WIFI_AP);
        wifiAccessPointActive = WiFi.softAP(AP_SSID, AP_PASSWORD);
        addLog("wifi", wifiAccessPointActive
            ? String("No saved STA credentials; setup AP started")
            : String("No saved STA credentials; failed to start setup AP"));
        return;
    }

    WiFi.softAPdisconnect(true);
    wifiAccessPointActive = false;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    startWifiStationAttempt(millis());
    bridge_time::tick(WiFi.status() == WL_CONNECTED, millis(), addTimeLog);
}

void tickWifiConnection(uint32_t nowMs) {
    if (wifiReconnectRequested.exchange(false, std::memory_order_acq_rel)) {
        WiFi.disconnect(true, false);
        connectWifi();
        return;
    }
    if (wifiStaSsid.isEmpty()) {
        return;
    }

    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected) {
        const bool newlyConnected = !wifiStationWasConnected;
        wifiConnectionAttemptActive = false;
        wifiNextReconnectAtMs = 0;
        wifiStationWasConnected = true;
        if (newlyConnected) {
            addLog("wifi", String("Connected to ") + wifiStaSsid + " as " + WiFi.localIP().toString());
            bridge_time::tick(true, nowMs, addTimeLog);
        }
        return;
    }

    if (wifiStationWasConnected) {
        wifiStationWasConnected = false;
        wifiConnectionAttemptActive = false;
        wifiNextReconnectAtMs = nowMs;
        addLog("wifi", String("Lost connection to ") + wifiStaSsid +
            "; retrying without setup AP");
    }
    if (wifiConnectionAttemptActive) {
        if (!bridge_runtime_policy::elapsedAtLeast(
                nowMs, wifiConnectionAttemptStartedAtMs, WIFI_CONNECT_TIMEOUT_MS)) {
            return;
        }
        wifiConnectionAttemptActive = false;
        wifiNextReconnectAtMs = nowMs + WIFI_RECONNECT_DELAY_MS;
        addLog("wifi", String("Failed to connect to ") + wifiStaSsid +
            "; setup AP is disabled and station retry is scheduled");
        return;
    }
    if (wifi_runtime_policy::shouldStartStationAttempt(
            !wifiStaSsid.isEmpty(), connected, wifiConnectionAttemptActive,
            nowMs, wifiNextReconnectAtMs)) {
        startWifiStationAttempt(nowMs);
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
bool appendServiceInfo(JsonObject target,
                       NimBLERemoteService* service,
                       bool includeDescriptors,
                       String& error);
bool appendServiceList(JsonArray servicesArray, bool includeDescriptors, String& error);
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
    if (cachedDetails.serial.isEmpty()) {
        fetchDeviceDetails(detailsError);
    }
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
        if (!boundedWorkerDelay(settleMs)) {
            error = "probe settle delay exceeded the job deadline";
            return false;
        }
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
    if (cachedDetails.serial.isEmpty()) {
        fetchDeviceDetails(detailsError);
    }
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
    if (resolveStoredSessionIfAvailable() == nullptr) {
        if (!establishHuSessionForProbe(waitMs, "stats_hu_internal", "stats-hu", scenarios, error)) {
            return false;
        }
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
        if (workerDeadlineExceeded()) {
            error = "job deadline exceeded while reading statistics";
            return false;
        }
        noteWorkerProgress();
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
            error = scenarioError;
            result["error"] = scenarioError;
            result["requestHex"] = hexEncode(requestPacket);
            result["notifyCount"] = chunks.size();
            JsonArray notifications = result.createNestedArray("notifications");
            appendNotificationChunks(notifications, chunks, times, startedAt);
            appendDecodeAttempt(result.createNestedObject("decode"), "HR", encrypt, chunks, sessionKey);
            result["writeWithResponse"] = writeWithResponse;
            result["canWrite"] = canWrite;
            result["canWriteNoResponse"] = canWriteNoResponse;
            return false;
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
    if (cachedDetails.serial.isEmpty()) {
        fetchDeviceDetails(detailsError);
    }
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
    if (resolveStoredSessionIfAvailable() == nullptr) {
        if (!establishHuSessionForProbe(waitMs, "settings_hu_internal", "settings-hu", scenarios, error)) {
            return false;
        }
    }
    const ByteVector* sessionKey = resolveStoredSessionIfAvailable();
    response["sessionHexUsed"] = sessionKey != nullptr ? hexEncode(*sessionKey) : "";

    JsonObject values = response.createNestedObject("values");
    bool overallOk = true;
    for (const auto* probe : probes) {
        if (workerDeadlineExceeded()) {
            error = "job deadline exceeded while reading settings";
            return false;
        }
        noteWorkerProgress();
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
            error = scenarioError;
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
            return false;
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
            JsonArray options = valueItem.createNestedArray("options");
            appendSettingOptions(options, *probe);
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
    if (cachedDetails.serial.isEmpty()) {
        fetchDeviceDetails(detailsError);
    }
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
        clientDisconnectedEvent.store(true, std::memory_order_release);
        // Publish the event before releasing the waiter. An acquire-load of
        // pending=false can then safely consume this exact callback event
        // before any replacement connection starts.
        clientDisconnectPending.store(false, std::memory_order_release);
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

    {
        WorkerBleCallScope bleCall;
        client = NimBLEDevice::createClient();
    }
    if (client == nullptr) {
        error = "failed to create NimBLE client";
        return false;
    }

    client->setClientCallbacks(&clientCallbacks, false);
    client->setConnectionParams(12, 12, 0, 150);
    client->setConnectTimeout(5000);
    clientDisconnectPending.store(false, std::memory_order_release);
    return true;
}

bool initializeBleStack(String& error) {
    if (!NimBLEDevice::isInitialized()) {
        {
            WorkerBleCallScope bleCall;
            NimBLEDevice::init("");
        }
        NimBLEDevice::setSecurityAuth(true, false, true);
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
        addLog("ble", "NimBLE stack initialized");
    }
    error = "";
    return true;
}

bool bleTransportReady(const char* operation, String& error) {
    if (workerDeadlineExceeded()) {
        error = String(operation) + " exceeded the job deadline";
        return false;
    }
    if (client == nullptr || !client->isConnected()) {
        error = String("BLE disconnected during ") + operation;
        return false;
    }
    return true;
}

bool bleDiscoveryCallCompleted(const char* operation, String& error) {
    noteWorkerProgress();
    if (!bleTransportReady(operation, error)) {
        return false;
    }
    // NimBLE's last-error field is sticky, so it cannot identify the outcome
    // of the call that just returned. A discovery operation approaching the
    // library's ~30 second GATT timeout is treated as a transport timeout even
    // in a 60 second diagnostic job, preventing another long call from being
    // started behind it.
    if (workerLastBleCallDurationMs.load(std::memory_order_acquire) >= 25000) {
        error = String(operation) + " hit the BLE transport timeout";
        return false;
    }
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

    if (!bleTransportReady("service discovery", error)) {
        return false;
    }
    {
        WorkerBleCallScope bleCall;
        disService = client->getService(DEVICE_INFORMATION_SERVICE);
    }
    if (!bleDiscoveryCallCompleted("device-information service discovery", error)) {
        return false;
    }
    {
        WorkerBleCallScope bleCall;
        nivonaService = client->getService(NIVONA_SERVICE);
    }
    if (!bleDiscoveryCallCompleted("coffee-machine service discovery", error)) {
        return false;
    }
    if (nivonaService == nullptr) {
        error = "supported coffee-machine service not found on connected device";
        return false;
    }

    auto discoverCharacteristic = [&](const char* uuid,
                                      NimBLERemoteCharacteristic*& destination,
                                      const char* label) -> bool {
        if (!bleTransportReady(label, error)) {
            return false;
        }
        {
            WorkerBleCallScope bleCall;
            destination = nivonaService->getCharacteristic(uuid);
        }
        return bleDiscoveryCallCompleted(label, error);
    };
    if (!discoverCharacteristic(NIVONA_CTRL, nivonaCtrl, "control characteristic discovery") ||
        !discoverCharacteristic(NIVONA_RX, nivonaRx, "rx characteristic discovery") ||
        !discoverCharacteristic(NIVONA_TX, nivonaTx, "tx characteristic discovery") ||
        !discoverCharacteristic(NIVONA_AUX1, nivonaAux1, "aux1 characteristic discovery") ||
        !discoverCharacteristic(NIVONA_AUX2, nivonaAux2, "aux2 characteristic discovery") ||
        !discoverCharacteristic(NIVONA_NAME, nivonaName, "name characteristic discovery")) {
        return false;
    }

    if (nivonaCtrl == nullptr || nivonaRx == nullptr || nivonaTx == nullptr || nivonaName == nullptr) {
        error = "missing one or more required proprietary service characteristics";
        return false;
    }

    if (hadNotifications) {
        bool subscribed = false;
        {
            WorkerBleCallScope bleCall;
            subscribed = nivonaRx->subscribe(notificationModeUsesNotify(modeToRestore), rxNotifyCallback, true);
        }
        if (!subscribed) {
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
    applyClientDisconnectedEvent();
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
            if (nivonaService != nullptr && nivonaCtrl != nullptr && nivonaRx != nullptr &&
                nivonaTx != nullptr && nivonaName != nullptr) {
                lastBleActivityAtMs = millis();
                return true;
            }
            return refreshRemoteHandles(error);
        }
        if (!disconnectClientAndWait(error)) {
            return false;
        }
        if (!boundedWorkerDelay(200)) {
            error = "reconnect delay exceeded the job deadline";
            return false;
        }
    }

    if (currentWorkerExecution() == nullptr) {
        ScanRecord* record = findScannedDevice(selectedAddress);
        if (record != nullptr) {
            selectedAddressType = record->addressType;
        }
    }

    NimBLEAddress address(std::string(selectedAddress.c_str()), selectedAddressType);
    bool connected = false;
    {
        WorkerBleCallScope bleCall;
        connected = client->connect(address, true, false, true);
    }
    if (!connected || workerDeadlineExceeded()) {
        error = String("failed to connect to ") + selectedAddress;
        return false;
    }
    if (client->isConnected()) {
        clientDisconnectPending.store(false, std::memory_order_release);
        clientDisconnectedEvent.store(false, std::memory_order_release);
    }

    if (!refreshRemoteHandles(error)) {
        return false;
    }
    lastBleActivityAtMs = millis();
    return true;
}

bool disconnectFromDevice(String& error) {
    if (client == nullptr || !client->isConnected()) {
        error = "not connected";
        return false;
    }
    if (!selectedAddress.isEmpty() &&
        !String(client->getPeerAddress().toString().c_str()).equalsIgnoreCase(selectedAddress)) {
        error = "the requested target is not the currently connected device";
        return false;
    }
    return disconnectClientAndWait(error);
}

bool reconnectToSelectedDevice(uint32_t delayMs, String& error) {
    if (!ensureClient(error)) {
        return false;
    }

    if (client != nullptr && client->isConnected()) {
        if (!disconnectClientAndWait(error)) {
            return false;
        }
        if (!boundedWorkerDelay(delayMs > 0 ? delayMs : 200)) {
            error = "reconnect delay exceeded the job deadline";
            return false;
        }
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
    bool secured = false;
    {
        WorkerBleCallScope bleCall;
        secured = client->secureConnection();
    }
    if (!secured || workerDeadlineExceeded()) {
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
        bool resecured = false;
        {
            WorkerBleCallScope bleCall;
            resecured = client->secureConnection();
        }
        if (!resecured || workerDeadlineExceeded()) {
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
    if (!bleTransportReady("characteristic lookup", error)) {
        return false;
    }
    NimBLERemoteCharacteristic* characteristic = nullptr;
    {
        WorkerBleCallScope bleCall;
        characteristic = service->getCharacteristic(uuid);
    }
    if (!bleDiscoveryCallCompleted("characteristic lookup", error)) {
        return false;
    }
    if (characteristic == nullptr) {
        error = "characteristic not available";
        return false;
    }

    std::string value;
    {
        WorkerBleCallScope bleCall;
        value = characteristic->readValue();
    }
    if (!bleDiscoveryCallCompleted("characteristic read", error)) {
        return false;
    }
    out               = String(value.c_str());
    return true;
}

bool readRemoteCharacteristicBytes(NimBLERemoteService* service, const char* uuid, ByteVector& out, String& error) {
    out.clear();
    if (service == nullptr) {
        error = "service not available";
        return false;
    }
    if (!bleTransportReady("characteristic lookup", error)) {
        return false;
    }
    NimBLERemoteCharacteristic* characteristic = nullptr;
    {
        WorkerBleCallScope bleCall;
        characteristic = service->getCharacteristic(uuid);
    }
    if (!bleDiscoveryCallCompleted("characteristic lookup", error)) {
        return false;
    }
    if (characteristic == nullptr) {
        error = "characteristic not available";
        return false;
    }

    std::string value;
    {
        WorkerBleCallScope bleCall;
        value = characteristic->readValue();
    }
    if (!bleDiscoveryCallCompleted("characteristic read", error)) {
        return false;
    }
    out.assign(value.begin(), value.end());
    return true;
}

bool readCharacteristicValue(NimBLERemoteCharacteristic* characteristic, ByteVector& out, String& error) {
    out.clear();
    if (characteristic == nullptr) {
        error = "characteristic not available";
        return false;
    }

    std::string value;
    {
        WorkerBleCallScope bleCall;
        value = characteristic->readValue();
    }
    if (!bleDiscoveryCallCompleted("characteristic read", error)) {
        return false;
    }
    out.assign(value.begin(), value.end());
    return true;
}

bool isTransportReadFailure(const String& error) {
    return workerDeadlineExceeded() || client == nullptr || !client->isConnected() ||
        error.indexOf("deadline") >= 0 || error.indexOf("disconnect") >= 0 ||
        error.indexOf("timeout") >= 0 || error.indexOf("NimBLE error") >= 0;
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

bool prepareFullGattDiscovery(bool& restoreNotifications,
                              String& restoreMode,
                              String& error) {
    restoreNotifications = notificationsEnabled;
    restoreMode = notificationMode;

    // refresh=true deletes NimBLE's service/characteristic objects. A live
    // notification callback traverses those same vectors, so first quiesce it
    // with a completed disconnect and reconnect without subscriptions.
    if (restoreNotifications && client != nullptr && client->isConnected()) {
        if (!disconnectClientAndWait(error)) {
            error = String("failed to quiesce notifications for GATT discovery: ") + error;
            return false;
        }
        if (!connectToSelectedDevice(error)) {
            return false;
        }
    }

    invalidateRemoteHandlesForFullDiscovery();
    return true;
}

bool finishFullGattDiscovery(bool restoreNotifications,
                             const String& restoreMode,
                             String& error) {
    if (!restoreNotifications) {
        return true;
    }
    String restoreError;
    if (!setNotificationsEnabled(true, restoreError, restoreMode)) {
        error = restoreError.isEmpty()
            ? String("failed to restore notifications after GATT discovery")
            : restoreError;
        return false;
    }
    return true;
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

    if (!bleTransportReady("service discovery", error)) {
        return nullptr;
    }
    bool restoreNotifications = false;
    String restoreMode;
    if (!prepareFullGattDiscovery(restoreNotifications, restoreMode, error)) {
        return nullptr;
    }
    const auto* services = [&]() {
        WorkerBleCallScope bleCall;
        return &client->getServices(true);
    }();
    if (!bleDiscoveryCallCompleted("service discovery", error)) {
        finishFullGattDiscovery(restoreNotifications, restoreMode, error);
        return nullptr;
    }
    for (auto* service : *services) {
        if (!bleTransportReady("service discovery traversal", error)) {
            finishFullGattDiscovery(restoreNotifications, restoreMode, error);
            return nullptr;
        }
        if (service == nullptr) {
            continue;
        }
        if (String(service->getUUID().toString().c_str()).equalsIgnoreCase(serviceUuid)) {
            error = "";
            if (!finishFullGattDiscovery(restoreNotifications, restoreMode, error)) {
                return nullptr;
            }
            return service;
        }
    }

    error = "service not found";
    finishFullGattDiscovery(restoreNotifications, restoreMode, error);
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

    if (!bleTransportReady("service discovery", error)) {
        return nullptr;
    }
    bool restoreNotifications = false;
    String restoreMode;
    if (!prepareFullGattDiscovery(restoreNotifications, restoreMode, error)) {
        return nullptr;
    }
    const auto* services = [&]() {
        WorkerBleCallScope bleCall;
        return &client->getServices(true);
    }();
    if (!bleDiscoveryCallCompleted("service discovery", error)) {
        finishFullGattDiscovery(restoreNotifications, restoreMode, error);
        return nullptr;
    }
    for (auto* service : *services) {
        if (!bleTransportReady("characteristic discovery", error)) {
            finishFullGattDiscovery(restoreNotifications, restoreMode, error);
            return nullptr;
        }
        if (service == nullptr) {
            continue;
        }
        if (!serviceUuid.isEmpty() && !String(service->getUUID().toString().c_str()).equalsIgnoreCase(serviceUuid)) {
            continue;
        }
        const auto* characteristics = [&]() {
            WorkerBleCallScope characteristicsCall;
            return &service->getCharacteristics(true);
        }();
        if (!bleDiscoveryCallCompleted("characteristic discovery", error)) {
            finishFullGattDiscovery(restoreNotifications, restoreMode, error);
            return nullptr;
        }
        for (auto* characteristic : *characteristics) {
            if (!bleTransportReady("characteristic discovery traversal", error)) {
                finishFullGattDiscovery(restoreNotifications, restoreMode, error);
                return nullptr;
            }
            if (characteristic == nullptr) {
                continue;
            }
            if (String(characteristic->getUUID().toString().c_str()).equalsIgnoreCase(characteristicUuid)) {
                error = "";
                if (!finishFullGattDiscovery(restoreNotifications, restoreMode, error)) {
                    return nullptr;
                }
                if (serviceOut != nullptr) {
                    *serviceOut = service;
                }
                return characteristic;
            }
        }
    }

    error = "characteristic not found";
    finishFullGattDiscovery(restoreNotifications, restoreMode, error);
    return nullptr;
}

bool appendServiceInfo(JsonObject target,
                       NimBLERemoteService* service,
                       bool includeDescriptors,
                       String& error) {
    if (service == nullptr) {
        target["available"] = false;
        return true;
    }

    target["available"] = true;
    target["uuid"] = String(service->getUUID().toString().c_str());
    JsonArray characteristics = target.createNestedArray("characteristics");
    if (!bleTransportReady("characteristic discovery", error)) {
        return false;
    }
    const auto* remoteCharacteristics = [&]() {
        WorkerBleCallScope characteristicsCall;
        return &service->getCharacteristics(true);
    }();
    if (!bleDiscoveryCallCompleted("characteristic discovery", error)) {
        return false;
    }
    for (auto* characteristic : *remoteCharacteristics) {
        if (!bleTransportReady("characteristic discovery traversal", error)) {
            return false;
        }
        JsonObject item = characteristics.createNestedObject();
        appendCharacteristicInfo(item, characteristic);
        if (characteristic == nullptr || !includeDescriptors) {
            continue;
        }

        JsonArray descriptors = item.createNestedArray("descriptors");
        const auto* remoteDescriptors = [&]() {
            WorkerBleCallScope descriptorsCall;
            return &characteristic->getDescriptors(true);
        }();
        if (!bleDiscoveryCallCompleted("descriptor discovery", error)) {
            return false;
        }
        for (auto* descriptor : *remoteDescriptors) {
            if (!bleTransportReady("descriptor discovery traversal", error)) {
                return false;
            }
            JsonObject desc = descriptors.createNestedObject();
            if (descriptor == nullptr) {
                desc["available"] = false;
                continue;
            }
            desc["available"] = true;
            desc["uuid"] = String(descriptor->getUUID().toString().c_str());
        }
    }
    return true;
}

bool appendServiceList(JsonArray servicesArray, bool includeDescriptors, String& error) {
    if (client == nullptr || !client->isConnected()) {
        error = "not connected";
        return false;
    }
    if (!bleTransportReady("service discovery", error)) {
        return false;
    }
    bool restoreNotifications = false;
    String restoreMode;
    if (!prepareFullGattDiscovery(restoreNotifications, restoreMode, error)) {
        return false;
    }
    const auto* services = [&]() {
        WorkerBleCallScope bleCall;
        return &client->getServices(true);
    }();
    if (!bleDiscoveryCallCompleted("service discovery", error)) {
        finishFullGattDiscovery(restoreNotifications, restoreMode, error);
        return false;
    }
    for (auto* service : *services) {
        if (!bleTransportReady("service discovery traversal", error)) {
            finishFullGattDiscovery(restoreNotifications, restoreMode, error);
            return false;
        }
        JsonObject item = servicesArray.createNestedObject();
        if (!appendServiceInfo(item, service, includeDescriptors, error)) {
            finishFullGattDiscovery(restoreNotifications, restoreMode, error);
            return false;
        }
    }
    return finishFullGattDiscovery(restoreNotifications, restoreMode, error);
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
        if (isTransportReadFailure(tmpError)) {
            error = details.lastError;
            return false;
        }
    }
    if (!readRemoteCharacteristicText(disService, DIS_MODEL, details.model, tmpError)) {
        details.lastError = details.lastError + (details.lastError.isEmpty() ? "" : "; ") + "model: " + tmpError;
        if (isTransportReadFailure(tmpError)) {
            error = details.lastError;
            return false;
        }
    }
    if (!readRemoteCharacteristicText(disService, DIS_SERIAL, details.serial, tmpError)) {
        details.lastError = details.lastError + (details.lastError.isEmpty() ? "" : "; ") + "serial: " + tmpError;
        if (isTransportReadFailure(tmpError)) {
            error = details.lastError;
            return false;
        }
    }
    if (!readRemoteCharacteristicText(disService, DIS_HARDWARE, details.hardwareRevision, tmpError)) {
        details.lastError = details.lastError + (details.lastError.isEmpty() ? "" : "; ") + "hardware: " + tmpError;
        if (isTransportReadFailure(tmpError)) {
            error = details.lastError;
            return false;
        }
    }
    if (!readRemoteCharacteristicText(disService, DIS_FIRMWARE, details.firmwareRevision, tmpError)) {
        details.lastError = details.lastError + (details.lastError.isEmpty() ? "" : "; ") + "firmware: " + tmpError;
        if (isTransportReadFailure(tmpError)) {
            error = details.lastError;
            return false;
        }
    }
    if (!readRemoteCharacteristicText(disService, DIS_SOFTWARE, details.softwareRevision, tmpError)) {
        details.lastError = details.lastError + (details.lastError.isEmpty() ? "" : "; ") + "software: " + tmpError;
        if (isTransportReadFailure(tmpError)) {
            error = details.lastError;
            return false;
        }
    }

    ByteVector ad06;
    if (readRemoteCharacteristicBytes(nivonaService, NIVONA_NAME, ad06, tmpError)) {
        details.ad06Hex   = hexEncode(ad06);
        details.ad06Ascii = printableAscii(ad06);
    } else {
        details.lastError = details.lastError + (details.lastError.isEmpty() ? "" : "; ") + "AD06: " + tmpError;
        if (isTransportReadFailure(tmpError)) {
            error = details.lastError;
            return false;
        }
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
        if (workerDeadlineExceeded()) {
            error = "job deadline reached before characteristic write";
            return false;
        }
        bool wrote = false;
        {
            WorkerBleCallScope bleCall;
            wrote = characteristic->writeValue(payload.data(), payload.size(), response);
        }
        if (!wrote) {
            error = "writeValue failed";
            return false;
        }
        noteMutationWriteAcknowledged();
        addHexLog("tx-" + alias, payload.data(), payload.size());
        return true;
    }

    for (size_t offset = 0; offset < payload.size(); offset += chunkSize) {
        if (workerDeadlineExceeded()) {
            error = String("job deadline reached before chunk offset ") + offset;
            return false;
        }
        const size_t partSize = std::min(chunkSize, payload.size() - offset);
        bool wrote = false;
        {
            WorkerBleCallScope bleCall;
            wrote = characteristic->writeValue(payload.data() + offset, partSize, response);
        }
        if (!wrote) {
            error = String("writeValue failed at chunk offset ") + offset;
            return false;
        }
        noteMutationWriteAcknowledged();
        addHexLog("tx-" + alias + "-chunk", payload.data() + offset, partSize);
        if (interChunkDelayMs > 0 && offset + partSize < payload.size()) {
            if (!boundedWorkerDelay(interChunkDelayMs, 1000)) {
                error = "inter-chunk delay exceeded the job deadline";
                return false;
            }
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
        bool unsubscribed = true;
        if (notificationsEnabled) {
            WorkerBleCallScope bleCall;
            unsubscribed = nivonaRx->unsubscribe(true);
        }
        if (!unsubscribed) {
            error = "failed to switch rx notification mode";
            return false;
        }
        bool subscribed = false;
        {
            WorkerBleCallScope bleCall;
            subscribed = nivonaRx->subscribe(useNotify, rxNotifyCallback, true);
        }
        if (!subscribed || workerDeadlineExceeded()) {
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
    bool unsubscribed = false;
    {
        WorkerBleCallScope bleCall;
        unsubscribed = nivonaRx->unsubscribe(true);
    }
    if (!unsubscribed) {
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
        if (!boundedWorkerDelay(500)) {
            error = "preflight delay exceeded the job deadline";
            return false;
        }
    }

    if (packet != nullptr) {
        result["requestHex"] = hexEncode(*packet);
        if (!writeRemoteCharacteristic(nivonaTx, "tx", *packet, true, chunked, 10, interChunkDelayMs, error)) {
            result["error"] = error;
            return false;
        }
    }

    if (waitMs > 0) {
        if (!boundedWorkerDelay(waitMs, 3000)) {
            error = "notification wait exceeded the job deadline";
            return false;
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

bool mergeWorkerMachineDetailsIfChanged(const WorkerExecutionContext& execution);

void updateSavedMachineFromCachedDetails(SavedMachine& machine) {
    WorkerExecutionContext* execution = currentWorkerExecution();
    const String stableSerial = execution != nullptr && execution->machineLoaded
        ? machine.serial
        : String("");
    if (!cachedDetails.serial.isEmpty()) {
        populateSavedMachine(machine, machine.alias, machine.address, machine.addressType, cachedDetails);
        if (!stableSerial.isEmpty()) {
            machine.serial = stableSerial;
        }
    }
    markSavedMachineReachableFromLiveSession(machine);
    syncProtocolSessionTarget(machine);
    if (execution == nullptr || !execution->machineLoaded) {
        String persistenceError;
        if (!persistSavedMachines(&persistenceError)) {
            lastError = persistenceError;
        }
    } else {
        // Try while the job still has most of its deadline available. A final
        // completion-time retry handles a transient long HTTP stream.
        mergeWorkerMachineDetailsIfChanged(*execution);
    }
}

bool mergeWorkerMachineDetailsIfChanged(const WorkerExecutionContext& execution) {
    if (!execution.machineLoaded) {
        return true;
    }
    if (machineMutex == nullptr) {
        return false;
    }
    while (xSemaphoreTake(machineMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        noteWorkerProgress();
        if (workerDeadlineExceeded()) {
            return false;
        }
    }

    bool changed = false;
    bool reconciled = false;
    {
        MachineGenerationLock generationLock;
        if (generationLock &&
            machineGenerationCurrentLocked(execution.machine.serial, execution.machine.generation)) {
            SavedMachine* stored = findSavedMachineBySerialGlobal(execution.machine.serial);
            if (stored != nullptr) {
                reconciled = true;
                SavedMachine merged = *stored;
                merged.address = execution.machine.address;
                merged.addressType = execution.machine.addressType;
                merged.manufacturer = execution.machine.manufacturer;
                merged.model = execution.machine.model;
                merged.modelCode = execution.machine.modelCode;
                merged.modelName = execution.machine.modelName;
                merged.familyKey = execution.machine.familyKey;
                merged.hardwareRevision = execution.machine.hardwareRevision;
                merged.firmwareRevision = execution.machine.firmwareRevision;
                merged.softwareRevision = execution.machine.softwareRevision;
                merged.ad06Hex = execution.machine.ad06Hex;
                merged.ad06Ascii = execution.machine.ad06Ascii;
                if (!bridge_runtime_policy::durableMachineFieldsEqual(*stored, merged)) {
                    *stored = merged;
                    changed = true;
                }
                if (changed || machinePersistenceDirty) {
                    String persistenceError;
                    if (!persistSavedMachines(&persistenceError)) {
                        reconciled = false;
                        lastError = persistenceError;
                    }
                }
            }
        } else if (generationLock) {
            // Deletion/replacement intentionally wins over late enrichment.
            reconciled = true;
        }
    }
    xSemaphoreGive(machineMutex);
    return reconciled;
}

bool beginMachineProtocolSession(SavedMachine& machine, String& error, bool clearSession = true) {
    if (!selectSavedMachine(machine, error)) {
        return false;
    }
    const bool sameConnectedTarget = client != nullptr && client->isConnected() &&
        String(client->getPeerAddress().toString().c_str()).equalsIgnoreCase(machine.address) &&
        nivonaService != nullptr && nivonaRx != nullptr && nivonaTx != nullptr;
    const bool reusableSession = sameConnectedTarget &&
        resolveStoredSessionIfAvailable(machine.serial, machine.address) != nullptr;
    if (clearSession && !reusableSession) {
        clearStoredSessionKey(machine.serial, machine.address);
    }
    if (!pairWithDevice(error, true, DEFAULT_RECONNECT_DELAY_MS)) {
        return false;
    }
    if (!setNotificationsEnabled(true, error, "notify")) {
        return false;
    }
    suppressIdleScans();
    if (cachedDetails.serial.isEmpty() || !cachedDetails.serial.equalsIgnoreCase(machine.serial)) {
        String detailsError;
        if (!fetchDeviceDetails(detailsError)) {
            error = detailsError.isEmpty() ? String("failed to read machine details") : detailsError;
            return false;
        }
    }
    updateSavedMachineFromCachedDetails(machine);
    lastBleActivityAtMs = millis();
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
                        uint32_t waitMs = 0,
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
    bool started = false;
    {
        WorkerBleCallScope bleCall;
        started = scan->start(SCAN_MS, false, false);
    }
    if (!started) {
        blockingScanInProgress = false;
        addLog("scan", "Failed to start BLE scan");
        lastScanReason = -1;
        lastScanAtMs   = millis();
        return;
    }

    while (scan->isScanning() && (millis() - startedAt) < (SCAN_MS + 1000)) {
        bool cancelForInteractive = false;
        WorkerExecutionContext* execution = currentWorkerExecution();
        if (execution != nullptr && execution->backgroundJob && jobMutex != nullptr &&
            xSemaphoreTake(jobMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            cancelForInteractive = jobScheduler.hasQueuedAbove(bridge_jobs::Priority::Background);
            xSemaphoreGive(jobMutex);
        }
        if (cancelForInteractive) {
            WorkerBleCallScope bleCall;
            scan->stop();
            addLog("scan", "Background scan yielded to interactive BLE work");
            break;
        }
        const uint32_t elapsedMs = static_cast<uint32_t>(millis() - startedAt);
        noteWorkerProgress(static_cast<uint8_t>(10 +
            (std::min<uint32_t>(elapsedMs, SCAN_MS) * 80U) / SCAN_MS));
        delay(50);
    }

    if (scan->isScanning()) {
        WorkerBleCallScope bleCall;
        scan->stop();
    }
    delay(150);

    publishScanResults(startedAt, true, "Completed BLE scan");
    blockingScanInProgress = false;
}

void updateWorkerOwnedHealth();

void appendStatus(JsonDocument& doc) {
    if (currentWorkerExecution() != nullptr) {
        // Mutation results retain the former synchronous status shape. The
        // worker is the sole NimBLE owner, so publish its just-completed state
        // before copying the immutable snapshot into the result document.
        updateWorkerOwnedHealth();
    }
    const bridge_time::StatusSnapshot timeStatus = bridge_time::snapshot();
    const bridge_time::ConfigSnapshot timeConfig = bridge_time::config();
    BridgeHealthSnapshot health;
    if (healthMutex != nullptr && xSemaphoreTake(healthMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        health = bridgeHealth;
        xSemaphoreGive(healthMutex);
    }

    doc["appName"]       = APP_NAME;
    doc["appVersion"]    = APP_VERSION;
    doc["apiVersion"]    = API_VERSION;
    doc["bridgeId"]      = bridgeId();
    doc["buildTime"]     = APP_BUILD_TIME;
    doc["hostname"]      = APP_HOSTNAME;
    doc["uptimeMs"]      = millis();
    const bool staConnected = WiFi.status() == WL_CONNECTED;
    const bool wifiConfigured = !wifiStaSsid.isEmpty();
    doc["wifiConfigured"] = wifiConfigured;
    doc["wifiMode"] = wifiAccessPointActive
        ? "setup_ap"
        : (wifiConfigured ? "station" : "unavailable");
    doc["apActive"]      = wifiAccessPointActive;
    doc["apSsid"]        = wifiAccessPointActive ? AP_SSID : "";
    doc["apPassword"]    = wifiAccessPointActive ? AP_PASSWORD : "";
    doc["apIp"]          = wifiAccessPointActive ? WiFi.softAPIP().toString() : String("");
    doc["staConnected"]  = staConnected;
    doc["staSsid"]       = wifiStaSsid;
    doc["staIp"]         = staConnected ? WiFi.localIP().toString() : String("");
    doc["selectedAddress"] = health.selectedAddress;
    doc["notificationsEnabled"] = String(health.notificationMode) != "off";
    doc["notificationMode"] = health.notificationMode;
    doc["pairingStatus"] = health.pairingStatus;
    doc["lastError"]     = health.lastError;
    doc["protocolSessionCount"] = health.protocolSessionCount;
    doc["standardRecipeCacheReady"] = littleFsReady;
    doc["littleFsReady"] = littleFsReady;
    doc["littleFsTotalBytes"] = health.littleFsTotalBytes;
    doc["littleFsUsedBytes"] = health.littleFsUsedBytes;
    JsonObject capabilities = doc.createNestedObject("capabilities");
    capabilities["asyncBleJobs"] = true;
    capabilities["websocketEvents"] = true;
    capabilities["eventProtocolVersion"] = 1;
    capabilities["eventsUrl"] = "/api/events";
    doc["timeConfigured"] = timeStatus.configured;
    doc["timeAvailable"] = timeStatus.available;
    doc["timeSynced"] = timeStatus.synced;
    doc["timeRestored"] = timeStatus.restored;
    doc["timeClientSeeded"] = timeStatus.clientSeeded;
    doc["timeLastAttemptMs"] = timeStatus.lastAttemptMs;
    doc["timeLastSuccessMs"] = timeStatus.lastSuccessMs;
    doc["timeLastSuccessUnix"] = static_cast<int64_t>(timeStatus.lastSuccessUnix);
    doc["timeLastSuccessIsoUtc"] = timeStatus.lastSuccessIsoUtc;
    doc["ntpDiagnosticAtMs"] = timeStatus.ntpDiagnosticAtMs;
    doc["ntpDiagnosticRoundTripMs"] = timeStatus.ntpDiagnosticRoundTripMs;
    doc["ntpDiagnosticCode"] = timeStatus.ntpDiagnosticCode;
    doc["ntpDiagnosticMessage"] = timeStatus.ntpDiagnosticMessage;
    doc["ntpDiagnosticServer"] = timeStatus.ntpDiagnosticServer;
    doc["ntpDiagnosticAddress"] = timeStatus.ntpDiagnosticAddress;
    doc["ntpDiagnosticPending"] = timeStatus.ntpDiagnosticPending;
    doc["ntpDiagnosticRunning"] = timeStatus.ntpDiagnosticRunning;
    doc["ntpDiagnosticStackHighWaterBytes"] = timeStatus.ntpDiagnosticStackHighWaterBytes;
    doc["timeUnix"] = static_cast<int64_t>(timeStatus.unixTime);
    doc["timeIsoUtc"] = timeStatus.iso8601Utc;
    doc["timeSource"] = timeStatus.synced ? "ntp" : (timeStatus.restored ? "restored" : (timeStatus.clientSeeded ? "client" : ""));
    JsonObject timeConfigJson = doc.createNestedObject("timeConfig");
    timeConfigJson["mode"] = timeConfig.mode;
    timeConfigJson["ntpServerPrimary"] = timeConfig.ntpServerPrimary;
    timeConfigJson["ntpServerSecondary"] = timeConfig.ntpServerSecondary;
    timeConfigJson["ntpServerTertiary"] = timeConfig.ntpServerTertiary;

    doc["supportedAdvertised"] = health.supportedDeviceCount > 0;
    doc["deviceCount"] = health.deviceCount;
    doc["supportedDeviceCount"] = health.supportedDeviceCount;
    doc["savedMachineCount"] = health.savedMachineCount;
    doc["lastScanReason"] = health.lastScanReason;
    doc["lastScanResultCount"] = health.lastScanResultCount;
    doc["lastScanAtMs"] = health.lastScanAtMs;
    doc["scanInProgress"] = health.scanInProgress;
    doc["idleScanInProgress"] = health.scanInProgress;

    JsonObject historyStorage = doc.createNestedObject("historyStorage");
    historyStorage["budgetBytes"] = brew_history::budgetBytes();
    historyStorage["budgetMinBytes"] = brew_history::budgetMinBytes();
    historyStorage["budgetUpperBytes"] = brew_history::budgetUpperBytes();
    historyStorage["defaultBudgetBytes"] = brew_history::DEFAULT_HISTORY_BYTES;
    historyStorage["statsBudgetBytes"] = stats_history::budgetBytes();
    historyStorage["statsBudgetUpperBytes"] = stats_history::budgetUpperBytes();
    historyStorage["writableAggregateLimitBytes"] =
        history_storage::writableHistoryLimit(health.littleFsTotalBytes);
    historyStorage["fileCount"] = health.historyFileCount;
    historyStorage["totalBytes"] = health.historyTotalBytes;
    historyStorage["largestBrewFileBytes"] = health.largestBrewHistoryFileBytes;
    historyStorage["largestStatsFileBytes"] = health.largestStatsHistoryFileBytes;
    historyStorage["losslessAcrossFirmwareUpdates"] = true;

    doc["clientCreated"] = health.clientCreated;
    doc["clientConnected"] = health.clientConnected;
    doc["peerAddress"] = health.peerAddress;

    JsonObject queue = doc.createNestedObject("bleQueue");
    queue["capacity"] = bridge_jobs::ACTIVE_CAPACITY;
    queue["queued"] = health.queuedJobs;
    queue["running"] = health.runningJobs;
    queue["submitted"] = health.submittedJobs;
    queue["completed"] = health.completedJobs;
    queue["failed"] = health.failedJobs;
    queue["cancelled"] = health.cancelledJobs;
    queue["rejected"] = health.rejectedJobs;
    queue["coalesced"] = health.coalescedJobs;
    queue["backgroundEvicted"] = health.backgroundEvictedJobs;

    JsonObject worker = doc.createNestedObject("bleWorker");
    worker["ready"] = health.workerReady;
    worker["busy"] = health.workerBusy;
    worker["currentJobId"] = health.currentJobId;
    worker["currentKind"] = health.currentJobKind;
    worker["currentTarget"] = health.currentJobTarget;
    worker["currentJobAgeMs"] = health.currentJobAgeMs;
    worker["progress"] = health.currentProgress;
    worker["heartbeatAgeMs"] = health.workerHeartbeatAgeMs;
    worker["stackHighWaterBytes"] = health.workerStackHighWaterMark;

    JsonObject watchdog = doc.createNestedObject("bleWatchdog");
    watchdog["markerPresent"] = health.watchdogMarkerPresent;
    watchdog["atMs"] = health.watchdogAtMs;
    watchdog["jobId"] = health.watchdogJobId;
    watchdog["kind"] = health.watchdogJobKind;
    watchdog["target"] = health.watchdogJobTarget;

    JsonObject memory = doc.createNestedObject("memory");
    memory["freeHeap"] = health.freeHeap;
    memory["minimumFreeHeap"] = health.minimumFreeHeap;
    memory["largestFreeHeapBlock"] = health.largestFreeHeapBlock;
    doc["resetReason"] = health.resetReason;
    doc["durableMachineWriteCount"] = health.durableMachineWrites;
    JsonObject http = doc.createNestedObject("http");
    http["lastDurationUs"] = health.httpLastDurationUs;
    http["maxDurationUs"] = health.httpMaxDurationUs;
    http["websocketClients"] = server.websocketClientCount();
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
    markSavedMachineReachableFromLiveSession(machineOut);
    if (!validateSavedMachineDurableFields(machineOut, error)) {
        return false;
    }
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
    return validateSavedMachineDurableFields(machineOut, error);
}

bool containsSerial(const std::vector<String>& serials, const String& serial) {
    for (const String& candidate : serials) {
        if (candidate.equalsIgnoreCase(serial)) {
            return true;
        }
    }
    return false;
}

bool addBackupHistoryBytes(std::vector<BackupHistorySize>& totals,
                           const String& serial,
                           size_t bytes,
                           String& error) {
    for (BackupHistorySize& total : totals) {
        if (!total.serial.equalsIgnoreCase(serial)) {
            continue;
        }
        if (bytes > SIZE_MAX - total.bytes) {
            error = "backup history size overflow";
            return false;
        }
        total.bytes += bytes;
        return true;
    }
    if (totals.size() >= MACHINE_GENERATION_CAPACITY) {
        error = "too many distinct backup history serials";
        return false;
    }
    totals.push_back({serial, bytes});
    return true;
}

bool parseBackupMachineRecord(JsonObjectConst record, SavedMachine& machineOut, String& error) {
    error = "";
    machineOut = SavedMachine{};

    const JsonObjectConst machine = record["machine"].as<JsonObjectConst>();
    if (machine.isNull()) {
        error = "machine record requires a machine object";
        return false;
    }

    machineOut.serial = machine["serial"] | "";
    machineOut.serial.trim();
    if (machineOut.serial.isEmpty()) {
        error = "machine serial is required";
        return false;
    }

    machineOut.alias = machine["alias"] | "";
    machineOut.alias.trim();
    machineOut.address = machine["address"] | "";
    if (!normalizeBleAddress(machineOut.address)) {
        error = "machine address must be a BLE MAC like C8:B4:17:D8:A3:8C";
        return false;
    }

    if (!parseAddressTypeRequest(machine["addressType"], machineOut.addressType)) {
        error = "machine addressType must be public, random, 0, or 1";
        return false;
    }

    machineOut.manufacturer = machine["manufacturer"] | "NIVONA";
    machineOut.model = machine["model"] | "";
    machineOut.modelCode = machine["modelCode"] | "";
    machineOut.modelName = machine["modelName"] | "";
    machineOut.familyKey = machine["familyKey"] | "";
    machineOut.hardwareRevision = machine["hardwareRevision"] | "";
    machineOut.firmwareRevision = machine["firmwareRevision"] | "";
    machineOut.softwareRevision = machine["softwareRevision"] | "";
    machineOut.ad06Hex = machine["ad06Hex"] | "";
    machineOut.ad06Ascii = machine["ad06Ascii"] | "";
    machineOut.savedAtMs = machine["savedAtMs"] | 0U;

    if (machineOut.familyKey.isEmpty() || machineOut.modelCode.isEmpty() || machineOut.modelName.isEmpty()) {
        SavedMachine derived;
        String derivedError;
        if (!buildManualSavedMachine(machineOut.alias,
                                     machineOut.address,
                                     machineOut.addressType,
                                     machineOut.serial,
                                     machineOut.model,
                                     derived,
                                     derivedError)) {
            error = String("machine family metadata is incomplete: ") + derivedError;
            return false;
        }
        if (machineOut.alias.isEmpty()) {
            machineOut.alias = derived.alias;
        }
        if (machineOut.manufacturer.isEmpty()) {
            machineOut.manufacturer = derived.manufacturer;
        }
        if (machineOut.model.isEmpty()) {
            machineOut.model = derived.model;
        }
        if (machineOut.modelCode.isEmpty()) {
            machineOut.modelCode = derived.modelCode;
        }
        if (machineOut.modelName.isEmpty()) {
            machineOut.modelName = derived.modelName;
        }
        if (machineOut.familyKey.isEmpty()) {
            machineOut.familyKey = derived.familyKey;
        }
    }

    machineOut.lastSeenAtMs = 0;
    machineOut.lastSeenRssi = 0;
    if (machineOut.savedAtMs == 0) {
        machineOut.savedAtMs = millis();
    }
    return validateSavedMachineDurableFields(machineOut, error);
}

bool validateBackupBundle(size_t uploadBytes, BackupBundleSummary& summaryOut, String& error) {
    error = "";
    summaryOut = BackupBundleSummary{};
    history_storage::Guard filesystem;
    if (!filesystem) {
        error = "filesystem is busy";
        return false;
    }

    BackupChunkStore store;
    BackupReader file(store, uploadBytes);
    if (!file) {
        error = "failed to open backup bundle";
        return false;
    }

    backup_staging::PeakUsage peakUsage(uploadBytes);
    auto recordPeakUsage = [&]() {
        return summaryOut.restoredHistoryBytes <= SIZE_MAX - summaryOut.restoredStatsHistoryBytes &&
            peakUsage.observe(file.position(),
                summaryOut.restoredHistoryBytes + summaryOut.restoredStatsHistoryBytes);
    };
    bool sawMeta = false;
    size_t lineNumber = 0;
    String line;
    String lineError;
    while (readTextLine(file, line, lineError)) {
        lineNumber++;
        DynamicJsonDocument recordDoc(12288);
        const DeserializationError parseError = deserializeJson(recordDoc, line);
        if (parseError) {
            file.close();
            error = String("backup line ") + lineNumber + ": invalid json: " + parseError.c_str();
            return false;
        }

        const JsonObjectConst record = recordDoc.as<JsonObjectConst>();
        const String kind = record["kind"] | "";
        if (kind == "meta") {
            if (sawMeta) {
                file.close();
                error = String("backup line ") + lineNumber + ": duplicate meta record";
                return false;
            }
            const uint32_t schema = record["schema"] | 0U;
            if (schema != BACKUP_BUNDLE_SCHEMA) {
                file.close();
                error = String("backup line ") + lineNumber + ": unsupported backup schema";
                return false;
            }
            const uint32_t budgetBytes = record["historyBudgetBytes"] | 0U;
            if (budgetBytes == 0U) {
                file.close();
                error = String("backup line ") + lineNumber + ": historyBudgetBytes is required";
                return false;
            }
            summaryOut.requestedBudgetBytes = budgetBytes;
            summaryOut.requestedStatsBudgetBytes =
                record["statsHistoryBudgetBytes"] |
                static_cast<uint32_t>(stats_history::DEFAULT_HISTORY_BYTES);
            sawMeta = true;
            continue;
        }

        if (kind == "machine") {
            SavedMachine machine;
            String machineError;
            if (!parseBackupMachineRecord(record, machine, machineError)) {
                file.close();
                error = String("backup line ") + lineNumber + ": " + machineError;
                return false;
            }
            if (containsSerial(summaryOut.machineSerials, machine.serial)) {
                file.close();
                error = String("backup line ") + lineNumber + ": duplicate machine serial";
                return false;
            }
            if (summaryOut.machineCount >= MACHINE_GENERATION_CAPACITY) {
                file.close();
                error = String("backup line ") + lineNumber + ": too many saved machines";
                return false;
            }
            summaryOut.machineSerials.push_back(machine.serial);
            summaryOut.machineCount++;
            continue;
        }

        if (kind == "history") {
            String serial = record["serial"] | "";
            serial.trim();
            if (serial.isEmpty()) {
                file.close();
                error = String("backup line ") + lineNumber + ": history serial is required";
                return false;
            }

            std::vector<String> importedLines;
            size_t importedCount = 0;
            String importError;
            if (!brew_history::buildImportedLines(
                    record["entry"],
                    importedLines,
                    importedCount,
                    importError,
                    history_storage::MAX_JSON_LINE_BYTES)) {
                file.close();
                error = String("backup line ") + lineNumber + ": " + importError;
                return false;
            }
            if (!containsSerial(summaryOut.historySerials, serial)) {
                if (summaryOut.historySerials.size() >= MACHINE_GENERATION_CAPACITY) {
                    file.close();
                    error = String("backup line ") + lineNumber +
                        ": too many distinct brew-history serials";
                    return false;
                }
                summaryOut.historySerials.push_back(serial);
            }
            summaryOut.historyEntryCount += importedCount;
            for (const String& importedLine : importedLines) {
                const size_t lineBytes = importedLine.length() + 1;
                if (lineBytes > SIZE_MAX - summaryOut.restoredHistoryBytes ||
                    !addBackupHistoryBytes(
                        summaryOut.historyBytesBySerial, serial, lineBytes, importError)) {
                    file.close();
                    error = String("backup line ") + lineNumber + ": " +
                        (importError.isEmpty() ? String("brew history size overflow") : importError);
                    return false;
                }
                summaryOut.restoredHistoryBytes += lineBytes;
            }
            if (!recordPeakUsage()) {
                file.close();
                error = "backup restore peak size overflow";
                return false;
            }
            continue;
        }

        if (kind == "stats_history") {
            String serial = record["serial"] | "";
            serial.trim();
            if (serial.isEmpty()) {
                file.close();
                error = String("backup line ") + lineNumber + ": stats history serial is required";
                return false;
            }
            std::vector<String> importedLines;
            size_t importedCount = 0;
            String entryError;
            if (!stats_history::buildImportedLines(
                    record["entry"], importedLines, importedCount, entryError)) {
                file.close();
                error = String("backup line ") + lineNumber + ": " +
                    (entryError.isEmpty()
                         ? String("invalid stats history entry")
                         : entryError);
                return false;
            }
            if (!containsSerial(summaryOut.statsHistorySerials, serial)) {
                if (summaryOut.statsHistorySerials.size() >= MACHINE_GENERATION_CAPACITY) {
                    file.close();
                    error = String("backup line ") + lineNumber +
                        ": too many distinct stats-history serials";
                    return false;
                }
                summaryOut.statsHistorySerials.push_back(serial);
            }
            summaryOut.statsHistoryEntryCount += importedCount;
            for (const String& importedLine : importedLines) {
                const size_t entryBytes = importedLine.length() + 1;
                if (entryBytes > SIZE_MAX - summaryOut.restoredStatsHistoryBytes ||
                    !addBackupHistoryBytes(
                        summaryOut.statsHistoryBytesBySerial, serial, entryBytes, entryError)) {
                    file.close();
                    error = String("backup line ") + lineNumber + ": " +
                        (entryError.isEmpty()
                             ? String("stats history size overflow")
                             : entryError);
                    return false;
                }
                summaryOut.restoredStatsHistoryBytes += entryBytes;
            }
            if (!recordPeakUsage()) {
                file.close();
                error = "backup restore peak size overflow";
                return false;
            }
            continue;
        }

        file.close();
        error = String("backup line ") + lineNumber + ": unsupported record kind";
        return false;
    }
    if (!lineError.isEmpty()) {
        file.close();
        error = String("backup line ") + (lineNumber + 1) + ": " + lineError;
        return false;
    }
    file.close();

    summaryOut.peakRestoreDataBytes = peakUsage.bytes();

    if (!sawMeta) {
        error = "backup bundle is missing the meta record";
        return false;
    }

    for (const String& serial : summaryOut.historySerials) {
        if (!containsSerial(summaryOut.machineSerials, serial)) {
            error = String("backup history references an unknown machine serial: ") + serial;
            return false;
        }
    }
    for (const String& serial : summaryOut.statsHistorySerials) {
        if (!containsSerial(summaryOut.machineSerials, serial)) {
            error = String("backup stats history references an unknown machine serial: ") + serial;
            return false;
        }
    }
    return true;
}

bool loadBackupMachinesFromBundle(size_t uploadBytes, std::vector<SavedMachine>& machinesOut, String& error) {
    error = "";
    machinesOut.clear();
    history_storage::Guard filesystem;
    if (!filesystem) {
        error = "filesystem is busy";
        return false;
    }

    BackupChunkStore store;
    BackupReader file(store, uploadBytes);
    if (!file) {
        error = "failed to reopen backup bundle";
        return false;
    }

    size_t lineNumber = 0;
    String line;
    String lineError;
    while (readTextLine(file, line, lineError)) {
        lineNumber++;
        DynamicJsonDocument recordDoc(12288);
        const DeserializationError parseError = deserializeJson(recordDoc, line);
        if (parseError) {
            file.close();
            error = String("backup line ") + lineNumber + ": invalid json: " + parseError.c_str();
            return false;
        }
        const JsonObjectConst record = recordDoc.as<JsonObjectConst>();
        const String kind = record["kind"] | "";
        if (kind != "machine") {
            continue;
        }

        SavedMachine machine;
        String machineError;
        if (!parseBackupMachineRecord(record, machine, machineError)) {
            file.close();
            error = String("backup line ") + lineNumber + ": " + machineError;
            return false;
        }
        machinesOut.push_back(machine);
    }
    if (!lineError.isEmpty()) {
        file.close();
        error = String("backup line ") + (lineNumber + 1) + ": " + lineError;
        return false;
    }
    file.close();
    return true;
}

bool restoreHistoriesFromBundleProgressively(size_t uploadBytes,
                                             size_t& restoredBrewEntryCount,
                                             size_t& restoredStatsEntryCount,
                                             String& error) {
    error = "";
    restoredBrewEntryCount = 0;
    restoredStatsEntryCount = 0;
    history_storage::Guard filesystem(5000);
    if (!filesystem) {
        error = "filesystem is busy while restoring history";
        return false;
    }
    BackupChunkStore store;
    BackupReader file(store, uploadBytes);
    if (!file) {
        error = "invalid backup bundle size";
        return false;
    }

    size_t lineNumber = 0;
    String line;
    String lineError;
    while (readTextLine(file, line, lineError)) {
        ++lineNumber;
        DynamicJsonDocument recordDoc(12288);
        const DeserializationError parseError = deserializeJson(recordDoc, line);
        if (parseError) {
            error = String("backup line ") + lineNumber + ": invalid json: " + parseError.c_str();
            return false;
        }
        const JsonObjectConst record = recordDoc.as<JsonObjectConst>();
        const String kind = record["kind"] | "";
        // The complete record is now in RAM, and the original histories remain
        // in .restorebak until commit. Reclaim only fully consumed chunk files;
        // never overwrite, seek backwards in, or truncate an unread upload.
        if (!file.releaseConsumed()) {
            error = "failed to reclaim a consumed backup staging chunk";
            return false;
        }
        if (kind == "history" || kind == "stats_history") {
            String serial = record["serial"] | "";
            serial.trim();
            std::vector<String> importedLines;
            size_t importedCount = 0;
            String entryError;
            const bool imported = kind == "history"
                ? brew_history::buildImportedLines(
                      record["entry"], importedLines, importedCount, entryError)
                : stats_history::buildImportedLines(
                      record["entry"], importedLines, importedCount, entryError);
            const bool appended = imported && (kind == "history"
                ? brew_history::appendSerializedLines(serial, importedLines, entryError)
                : stats_history::appendSerializedLines(serial, importedLines, entryError));
            if (!appended) {
                error = String("backup line ") + lineNumber + ": " + entryError;
                return false;
            }
            if (kind == "history") restoredBrewEntryCount += importedCount;
            else restoredStatsEntryCount += importedCount;
        } else if (kind != "meta" && kind != "machine") {
            error = String("backup line ") + lineNumber + ": unsupported record kind";
            return false;
        }
    }
    if (!lineError.isEmpty()) {
        error = String("backup line ") + (lineNumber + 1) + ": " + lineError;
        return false;
    }
    // Also reclaim a final chunk containing only blank lines.
    if (!file.releaseConsumed()) {
        error = "failed to reclaim the final backup staging chunk";
        return false;
    }
    return true;
}

class BulkHistoryRestoreScope {
public:
    BulkHistoryRestoreScope() {
        history_storage::setBulkRestoreMode(true);
    }
    ~BulkHistoryRestoreScope() {
        history_storage::setBulkRestoreMode(false);
    }
    BulkHistoryRestoreScope(const BulkHistoryRestoreScope&) = delete;
    BulkHistoryRestoreScope& operator=(const BulkHistoryRestoreScope&) = delete;
};

bool beginHistoryRestoreTransaction(std::vector<String>& originalPathsOut,
                                    size_t previousBudgetBytes,
                                    String& error) {
    originalPathsOut.clear();
    history_storage::Guard filesystem(5000);
    if (!filesystem) {
        error = "filesystem is busy while starting the history restore";
        return false;
    }
    if (LittleFS.exists(BACKUP_RESTORE_ACTIVE_MARKER) ||
        LittleFS.exists(BACKUP_RESTORE_STAGED_MARKER) ||
        LittleFS.exists(BACKUP_RESTORE_COMMIT_MARKER)) {
        error = "a prior history restore still requires recovery";
        return false;
    }
    if (!writeRestoreStateBackup(previousBudgetBytes, error)) {
        return false;
    }
    // The rollback snapshot must be durable before the active marker can make
    // boot recovery treat this as a transaction. A crash before the marker is
    // therefore only an orphaned state file, which boot cleanup can discard.
    if (!writeRestoreMarker(BACKUP_RESTORE_ACTIVE_MARKER, "active", error)) {
        LittleFS.remove(BACKUP_RESTORE_STATE_PATH);
        return false;
    }
    if (!collectHistoryPaths(originalPathsOut, true, error)) {
        LittleFS.remove(BACKUP_RESTORE_STATE_PATH);
        LittleFS.remove(BACKUP_RESTORE_ACTIVE_MARKER);
        return false;
    }
    std::vector<String> stagedPaths;
    stagedPaths.reserve(originalPathsOut.size());
    for (const String& originalPath : originalPathsOut) {
        const String backupPath = originalPath + ".restorebak";
        if (LittleFS.exists(backupPath) || !LittleFS.rename(originalPath, backupPath)) {
            error = String("failed to stage history rollback file ") + originalPath;
            bool rollbackOk = true;
            for (auto staged = stagedPaths.rbegin(); staged != stagedPaths.rend(); ++staged) {
                if (!LittleFS.rename(*staged + ".restorebak", *staged)) {
                    rollbackOk = false;
                }
            }
            if (rollbackOk) {
                LittleFS.remove(BACKUP_RESTORE_STATE_PATH);
                LittleFS.remove(BACKUP_RESTORE_ACTIVE_MARKER);
                LittleFS.remove(BACKUP_RESTORE_STAGED_MARKER);
            } else {
                error += "; failed to restore one or more staged history files";
            }
            return false;
        }
        stagedPaths.push_back(originalPath);
    }
    if (!writeRestoreMarker(BACKUP_RESTORE_STAGED_MARKER, "staged", error)) {
        bool rollbackOk = true;
        for (auto staged = stagedPaths.rbegin(); staged != stagedPaths.rend(); ++staged) {
            if (!LittleFS.rename(*staged + ".restorebak", *staged)) {
                rollbackOk = false;
            }
        }
        if (rollbackOk) {
            LittleFS.remove(BACKUP_RESTORE_STATE_PATH);
            LittleFS.remove(BACKUP_RESTORE_ACTIVE_MARKER);
        } else {
            error += "; failed to restore staged history files";
        }
        return false;
    }
    return true;
}

bool commitHistoryRestoreTransaction(const std::vector<String>& originalPaths, String& error) {
    history_storage::Guard filesystem(5000);
    if (!filesystem) {
        error = "filesystem is busy while committing the history restore";
        return false;
    }
    if (!writeRestoreMarker(BACKUP_RESTORE_COMMIT_MARKER, "commit", error)) {
        return false;
    }

    // Once the verified commit marker exists, boot recovery must preserve the
    // new files. Cleanup failure is therefore deferred, not treated as an
    // application failure that could mix old and new state.
    bool cleanupComplete = true;
    for (const String& originalPath : originalPaths) {
        const String backupPath = originalPath + ".restorebak";
        if (LittleFS.exists(backupPath) && !LittleFS.remove(backupPath)) {
            cleanupComplete = false;
        }
    }
    if (cleanupComplete) {
        LittleFS.remove(BACKUP_RESTORE_STATE_PATH);
        LittleFS.remove(BACKUP_RESTORE_ACTIVE_MARKER);
        LittleFS.remove(BACKUP_RESTORE_STAGED_MARKER);
        LittleFS.remove(BACKUP_RESTORE_COMMIT_MARKER);
    } else {
        addLog("backup", "Committed restore cleanup was deferred until reboot");
    }
    return true;
}

bool applyBackupBundle(size_t uploadBytes,
                       const BackupBundleSummary& summary,
                       size_t& appliedBudgetBytes,
                       size_t& restoredMachineCount,
                       size_t& restoredHistoryEntryCount,
                       size_t& restoredStatsHistoryEntryCount,
                       String& error) {
    error = "";
    size_t totalBytes = 0;
    size_t usedBytes = 0;
    {
        history_storage::Guard filesystem(5000);
        if (!filesystem) {
            error = "filesystem is busy";
            return false;
        }
        totalBytes = LittleFS.totalBytes();
        usedBytes = LittleFS.usedBytes();
    }
    appliedBudgetBytes = 0;
    restoredMachineCount = 0;
    restoredHistoryEntryCount = 0;
    restoredStatsHistoryEntryCount = 0;

    std::vector<SavedMachine> restoredMachines;
    if (!loadBackupMachinesFromBundle(uploadBytes, restoredMachines, error)) {
        return false;
    }
    ConfiguredHistoryBudgets restoredBudgets = calculateHistoryBudgets(
        summary.requestedBudgetBytes, totalBytes);
    // Stats history did not have a configurable budget before API v2. Promote
    // backups carrying the legacy 32 KiB ceiling so a restore cannot silently
    // make the counter log full again.
    restoredBudgets.statsBytesPerMachine = stats_history::clampBudgetBytes(
        std::max(summary.requestedStatsBudgetBytes,
                 stats_history::DEFAULT_HISTORY_BYTES),
        totalBytes);
    appliedBudgetBytes = restoredBudgets.brewBytesPerMachine;
    String currentPersistenceError;
    if (!persistSavedMachines(&currentPersistenceError)) {
        error = String("current machine store is not durable: ") + currentPersistenceError;
        return false;
    }
    for (const BackupHistorySize& history : summary.historyBytesBySerial) {
        if (history.bytes > appliedBudgetBytes) {
            error = String("restored brew history for ") + history.serial +
                " exceeds the applied per-machine history budget";
            return false;
        }
    }
    for (const BackupHistorySize& history : summary.statsHistoryBytesBySerial) {
        if (history.bytes > restoredBudgets.statsBytesPerMachine) {
            error = String("restored statistics history for ") + history.serial +
                " exceeds the per-machine statistics-history budget";
            return false;
        }
    }
    if (summary.restoredHistoryBytes > SIZE_MAX - summary.restoredStatsHistoryBytes) {
        error = "restored history size overflow";
        return false;
    }
    const size_t peakDataBytes = summary.peakRestoreDataBytes;
    const size_t restoredFileCount = summary.historySerials.size() +
        summary.statsHistorySerials.size();
    const size_t fixedReserveBytes = history_storage::OPERATIONAL_HEADROOM_BYTES +
        history_capacity::RESTORE_METADATA_RESERVE_BYTES;
    if (uploadBytes > usedBytes) {
        error = "LittleFS usage changed while preparing the restore";
        return false;
    }
    // Keep original history charged throughout the transaction: rollback
    // files are renamed, not deleted, until the verified commit marker exists.
    // Subtract only logical upload bytes so its block/metadata slack remains
    // conservatively included in retainedBytes throughout restoration.
    const size_t requiredPeakBytes = backup_staging::requiredCapacity(
        usedBytes, uploadBytes, peakDataBytes, restoredFileCount, fixedReserveBytes);
    if (requiredPeakBytes > totalBytes) {
        error = String("insufficient LittleFS capacity for restored history; need ") +
            requiredPeakBytes +
            " bytes, have " + totalBytes;
        return false;
    }

    const std::vector<SavedMachine> previousMachines = savedMachines;
    const auto previousGenerations = machineGenerations;
    const size_t previousBudgetBytes = brew_history::budgetBytes();
    const size_t previousStatsBudgetBytes = stats_history::budgetBytes();
    std::vector<String> originalHistoryPaths;
    if (!beginHistoryRestoreTransaction(originalHistoryPaths, previousBudgetBytes, error)) {
        return false;
    }

    auto rollback = [&](const String& primaryError) -> bool {
        String combinedError = primaryError;
        String historyRollbackError;
        bool rollbackComplete = rollbackHistoryRestoreFiles(
            originalHistoryPaths, historyRollbackError);
        if (!rollbackComplete && !historyRollbackError.isEmpty()) {
            combinedError += String("; history rollback failed: ") + historyRollbackError;
        }
        {
            MachineGenerationLock generationLock;
            if (generationLock) {
                savedMachines = previousMachines;
                machineGenerations = previousGenerations;
            } else {
                rollbackComplete = false;
                combinedError += "; machine registry rollback lock failed";
            }
        }
        brew_history::configureBudget(previousBudgetBytes, totalBytes);
        stats_history::configureBudget(previousStatsBudgetBytes, totalBytes);
        machinePersistenceDirty = true;
        String persistenceRollbackError;
        if (!persistSavedMachines(&persistenceRollbackError)) {
            rollbackComplete = false;
            combinedError += String("; machine-store rollback failed: ") +
                (persistenceRollbackError.isEmpty()
                     ? String("unknown persistence error")
                     : persistenceRollbackError);
        }
        if (preferences.putUInt(PREFS_HISTORY_MAX_BYTES,
                                static_cast<uint32_t>(previousBudgetBytes)) != sizeof(uint32_t) ||
            preferences.getUInt(PREFS_HISTORY_MAX_BYTES, 0) != previousBudgetBytes) {
            rollbackComplete = false;
            combinedError += "; history-budget rollback failed";
        }
        if (rollbackComplete) {
            history_storage::Guard filesystem(5000);
            if (filesystem) {
                LittleFS.remove(BACKUP_RESTORE_STATE_PATH);
                LittleFS.remove(BACKUP_RESTORE_ACTIVE_MARKER);
                LittleFS.remove(BACKUP_RESTORE_STAGED_MARKER);
                LittleFS.remove(BACKUP_RESTORE_COMMIT_MARKER);
            } else {
                combinedError += "; restore marker cleanup deferred";
            }
        }
        error = combinedError;
        return false;
    };

    applyHistoryBudgets(restoredBudgets, totalBytes);
    {
        MachineGenerationLock generationLock;
        if (!generationLock) {
            return rollback("machine generation registry is busy");
        }
        savedMachines = restoredMachines;
        machineGenerations = {};
        for (SavedMachine& machine : savedMachines) {
            machine.generation = allocateMachineGeneration();
            if (!setMachineGenerationLocked(machine)) {
                return rollback("restored machine count exceeds the generation registry");
            }
        }
    }
    refreshAllSavedMachinePresence();
    String persistenceError;
    if (!persistSavedMachines(&persistenceError)) {
        return rollback(persistenceError);
    }
    restoredMachineCount = savedMachines.size();

    {
        BulkHistoryRestoreScope bulkRestore;
        String restoreError;
        if (!restoreHistoriesFromBundleProgressively(uploadBytes,
                                                     restoredHistoryEntryCount,
                                                     restoredStatsHistoryEntryCount,
                                                     restoreError)) {
            return rollback(restoreError);
        }
    }
    if (preferences.putUInt(PREFS_HISTORY_MAX_BYTES,
                            static_cast<uint32_t>(appliedBudgetBytes)) != sizeof(uint32_t) ||
        preferences.getUInt(PREFS_HISTORY_MAX_BYTES, 0) != appliedBudgetBytes) {
        return rollback("failed to persist restored history budget");
    }
    String commitError;
    if (!commitHistoryRestoreTransaction(originalHistoryPaths, commitError)) {
        return rollback(commitError);
    }
    clearAllStandardRecipeCaches();
    clearAllSavedRecipeCaches();
    requestBleWorkerReset();
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

    if (detail) {
        if (!readOptionalNumeric("icon", "Icon", layout.iconOffset, false, "")) {
            return false;
        }
    }
    appendSavedRecipeIconMetadata(target, modelInfo);

    if (!detail) {
        return true;
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
    appendStandardRecipeIconMetadata(target, modelInfo, selector);

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

struct HistoryStreamContext {
    bool first{true};
    size_t returned{0};
};

bool streamHistoryEntry(JsonObjectConst entry, size_t, void* opaque, String& error) {
    auto* context = static_cast<HistoryStreamContext*>(opaque);
    String serialized;
    serialized.reserve(measureJson(entry) + 1);
    if (serializeJson(entry, serialized) == 0) {
        error = "failed to serialize history entry";
        return false;
    }
    if (!context->first) {
        if (!server.sendContent(",", 1)) {
            error = "history client disconnected";
            return false;
        }
    }
    if (!server.sendContent(serialized.c_str(), serialized.length())) {
        error = "history client disconnected";
        return false;
    }
    context->first = false;
    context->returned++;
    return true;
}

bool beginHistoryStream(const String& serial, const String& alias = "", const String& familyKey = "") {
    DynamicJsonDocument prefixDoc(768);
    prefixDoc["ok"] = true;
    prefixDoc["serial"] = serial;
    if (!alias.isEmpty()) {
        prefixDoc["alias"] = alias;
    }
    if (!familyKey.isEmpty()) {
        prefixDoc["familyKey"] = familyKey;
    }
    String prefix;
    serializeJson(prefixDoc, prefix);
    prefix.remove(prefix.length() - 1);
    prefix += ",\"entries\":[";
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    return server.sendContent(prefix.c_str(), prefix.length());
}

template <typename StatsType, typename PageType>
void finishHistoryStream(const StatsType& stats,
                         const PageType& page,
                         const HistoryStreamContext& context,
                         const String& error) {
    DynamicJsonDocument suffixDoc(1536);
    suffixDoc["count"] = stats.entryCount;
    suffixDoc["returned"] = context.returned;
    suffixDoc["fileBytes"] = stats.fileBytes;
    suffixDoc["maxBytes"] = stats.maxBytes;
    suffixDoc["skippedEntries"] = stats.skippedEntries;
    suffixDoc["offset"] = page.offset;
    suffixDoc["limit"] = page.limit;
    suffixDoc["hasOlder"] = page.hasOlder;
    suffixDoc["hasNewer"] = page.hasNewer;
    suffixDoc["nextOffset"] = page.nextOffset;
    suffixDoc["prevOffset"] = page.prevOffset;
    if (!error.isEmpty()) {
        suffixDoc["ok"] = false;
        suffixDoc["streamError"] = error;
    }
    String suffix;
    serializeJson(suffixDoc, suffix);
    suffix.remove(0, 1);
    if (!server.sendContent("],", 2)) {
        return;
    }
    if (!server.sendContent(suffix.c_str(), suffix.length())) {
        return;
    }
    server.sendContent("");
}

void handleMachineHistory(const String& serial) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }

    const size_t limit = parseLimitArg("limit", 40, 100);
    const size_t offset = parseOffsetArg("offset", 0);
    brew_history::Stats stats;
    brew_history::Page page;
    String error;
    HistoryStreamContext stream;
    if (!beginHistoryStream(machine->serial, machine->alias, machine->familyKey)) {
        return;
    }
    if (!brew_history::visitPage(machine->serial,
                                 offset,
                                 limit,
                                 streamHistoryEntry,
                                 &stream,
                                 stats,
                                 page,
                                 error)) {
        lastError = error;
    }
    finishHistoryStream(stats, page, stream, error);
}

void handleMachineHistoryClear(const String& serial) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }

    String error;
    if (!brew_history::clear(machine->serial, error)) {
        lastError = error;
        sendError(500, error);
        return;
    }
    refreshCachedStorageTotals();

    DynamicJsonDocument response(8192);
    response["ok"] = true;
    response["serial"] = machine->serial;
    response["count"] = 0;
    response["fileBytes"] = 0;
    response["maxBytes"] = brew_history::budgetBytes();
    appendStatus(response);
    sendJson(response);
}

void handleMachineHistoryImport(const String& serial) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }
    if (!littleFsReady) {
        lastError = "LittleFS is unavailable";
        sendError(503, lastError.snapshot());
        return;
    }

    DynamicJsonDocument requestDoc(65536);
    String error;
    if (!parseJsonBody(requestDoc, error)) {
        sendError(400, error);
        return;
    }

    std::vector<String> importedLines;
    size_t importedCount = 0;
    if (!brew_history::buildImportedLines(requestDoc.as<JsonVariantConst>(), importedLines, importedCount, error)) {
        sendError(400, error);
        return;
    }
    if (!brew_history::appendSerializedLines(machine->serial, importedLines, error)) {
        lastError = error;
        sendError(500, error);
        return;
    }
    refreshCachedStorageTotals();

    DynamicJsonDocument response(8192);
    response["ok"] = true;
    response["serial"] = machine->serial;
    response["alias"] = machine->alias;
    response["imported"] = importedCount;
    response["maxBytes"] = brew_history::budgetBytes();
    appendStatus(response);
    sendJson(response);
}

void handleMachineHistoryPatch(const String& serial, const String& entryIdRaw) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }
    if (!littleFsReady) {
        lastError = "LittleFS is unavailable";
        sendError(503, lastError.snapshot());
        return;
    }

    size_t entryId = 0;
    if (!parseUnsignedPathIndex(entryIdRaw, entryId)) {
        sendError(400, "history entry id must be an unsigned integer");
        return;
    }

    DynamicJsonDocument requestDoc(2048);
    String error;
    if (!parseJsonBody(requestDoc, error)) {
        sendError(400, error);
        return;
    }

    DynamicJsonDocument response(8192);
    JsonObject entry = response.createNestedObject("entry");
    if (!brew_history::patchTimestamp(machine->serial, entryId, requestDoc.as<JsonObjectConst>(), entry, error)) {
        if (error == "brew history entry not found") {
            sendError(404, error);
            return;
        }
        if (error == "patched brew history exceeds the configured size limit") {
            sendError(409, error);
            return;
        }
        lastError = error;
        sendError(400, error);
        return;
    }
    refreshCachedStorageTotals();

    response["ok"] = true;
    response["serial"] = machine->serial;
    response["alias"] = machine->alias;
    response["entryId"] = static_cast<uint32_t>(entryId);
    appendStatus(response);
    sendJson(response);
}

void handleMachineHistoryDelete(const String& serial, const String& entryIdRaw) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }
    if (!littleFsReady) {
        lastError = "LittleFS is unavailable";
        sendError(503, lastError.snapshot());
        return;
    }

    size_t entryId = 0;
    if (!parseUnsignedPathIndex(entryIdRaw, entryId)) {
        sendError(400, "history entry id must be an unsigned integer");
        return;
    }

    DynamicJsonDocument response(8192);
    JsonObject entry = response.createNestedObject("entry");
    String error;
    if (!brew_history::deleteEntry(machine->serial, entryId, entry, error)) {
        if (error == "brew history entry not found") {
            sendError(404, error);
            return;
        }
        lastError = error;
        sendError(500, error);
        return;
    }
    refreshCachedStorageTotals();

    response["ok"] = true;
    response["serial"] = machine->serial;
    response["alias"] = machine->alias;
    response["entryId"] = static_cast<uint32_t>(entryId);
    appendStatus(response);
    sendJson(response);
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

    DynamicJsonDocument response(8192);
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
    if (alias.length() > MAX_MACHINE_ALIAS_BYTES) {
        sendError(400, "alias exceeds 64 bytes");
        return;
    }
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

    const bool needsMachineLock = currentWorkerExecution() != nullptr;
    if (needsMachineLock &&
        (machineMutex == nullptr || xSemaphoreTake(machineMutex, pdMS_TO_TICKS(500)) != pdTRUE)) {
        sendError(503, "machine store is busy");
        return;
    }
    const std::vector<SavedMachine> previousMachines = savedMachines;
    const auto previousGenerations = machineGenerations;
    SavedMachine* existing = findSavedMachineBySerialGlobal(preview.serial);
    const bool replacingExisting = existing != nullptr;
    String storeError;
    {
        MachineGenerationLock generationLock;
        if (!generationLock) {
            storeError = "machine generation registry is busy";
        } else if (existing == nullptr && savedMachines.size() >= MACHINE_GENERATION_CAPACITY) {
            storeError = "saved-machine capacity reached";
        } else if (existing != nullptr && !cancelTargetJobsAndCacheMetadata(existing->serial)) {
            storeError = "could not cancel existing machine jobs";
        } else {
            SavedMachine updated = existing != nullptr ? *existing : preview;
            if (existing == nullptr) {
                updated.alias = alias;
                updated.savedAtMs = millis();
            } else {
                populateSavedMachine(updated, alias, preview.address, preview.addressType, cachedDetails);
                updated.lastSeenAtMs = preview.lastSeenAtMs;
                updated.lastSeenRssi = preview.lastSeenRssi;
            }
            updated.generation = allocateMachineGeneration();
            if (!setMachineGenerationLocked(updated)) {
                storeError = "machine generation registry is full";
            } else if (existing == nullptr) {
                savedMachines.push_back(updated);
                existing = &savedMachines.back();
            } else {
                *existing = updated;
            }
        }
    }
    if (!storeError.isEmpty()) {
        if (needsMachineLock) {
            xSemaphoreGive(machineMutex);
        }
        sendError(storeError == "saved-machine capacity reached" ? 507 : 503, storeError);
        return;
    }
    String persistenceError;
    if (!persistSavedMachines(&persistenceError)) {
        MachineGenerationLock rollbackLock;
        if (rollbackLock) {
            savedMachines = previousMachines;
            machineGenerations = previousGenerations;
            machinePersistenceDirty = true;
            String rollbackError;
            if (!persistSavedMachines(&rollbackError) && !rollbackError.isEmpty()) {
                persistenceError += String("; rollback persistence failed: ") + rollbackError;
            }
        } else {
            persistenceError += "; machine registry rollback lock failed";
        }
        if (needsMachineLock) {
            xSemaphoreGive(machineMutex);
        }
        sendError(500, persistenceError);
        return;
    }
    if (replacingExisting) {
        removeTargetResourceCacheFiles(existing->serial);
    } else {
        refreshConfiguredHistoryBudgets();
    }
    if (WorkerExecutionContext* execution = currentWorkerExecution(); execution != nullptr) {
        execution->machine = *existing;
        execution->machineLoaded = true;
        execution->target = existing->serial;
    }

    syncProtocolSessionTarget(*existing);
    noteMutationApplied();

    DynamicJsonDocument response(8192);
    response["ok"] = true;
    JsonObject machine = response.createNestedObject("machine");
    appendSavedMachineJson(machine, *existing);
    appendStatus(response);
    if (needsMachineLock) {
        xSemaphoreGive(machineMutex);
    }
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

    const std::vector<SavedMachine> previousMachines = savedMachines;
    const auto previousGenerations = machineGenerations;
    SavedMachine* existing = findSavedMachineBySerial(manualMachine.serial);
    const bool replacingExisting = existing != nullptr;
    String storeError;
    {
        MachineGenerationLock generationLock;
        if (!generationLock) {
            storeError = "machine generation registry is busy";
        } else if (existing == nullptr && savedMachines.size() >= MACHINE_GENERATION_CAPACITY) {
            storeError = "saved-machine capacity reached";
        } else if (existing != nullptr && !cancelTargetJobsAndCacheMetadata(existing->serial)) {
            storeError = "could not cancel existing machine jobs";
        } else {
            if (existing != nullptr) {
                manualMachine.savedAtMs = existing->savedAtMs;
            }
            manualMachine.generation = allocateMachineGeneration();
            if (!setMachineGenerationLocked(manualMachine)) {
                storeError = "machine generation registry is full";
            } else if (existing == nullptr) {
                savedMachines.push_back(manualMachine);
                existing = &savedMachines.back();
            } else {
                *existing = manualMachine;
            }
        }
    }
    if (!storeError.isEmpty()) {
        server.sendHeader("Retry-After", "1");
        sendError(storeError == "saved-machine capacity reached" ? 507 : 503, storeError);
        return;
    }
    String persistenceError;
    if (!persistSavedMachines(&persistenceError)) {
        MachineGenerationLock rollbackLock;
        if (rollbackLock) {
            savedMachines = previousMachines;
            machineGenerations = previousGenerations;
            machinePersistenceDirty = true;
            String rollbackError;
            if (!persistSavedMachines(&rollbackError) && !rollbackError.isEmpty()) {
                persistenceError += String("; rollback persistence failed: ") + rollbackError;
            }
        } else {
            persistenceError += "; machine registry rollback lock failed";
        }
        sendError(500, persistenceError);
        return;
    }
    if (replacingExisting) {
        removeTargetResourceCacheFiles(existing->serial);
    } else {
        refreshConfiguredHistoryBudgets();
    }

    DynamicJsonDocument response(8192);
    response["ok"] = true;
    JsonObject machine = response.createNestedObject("machine");
    appendSavedMachineJson(machine, *existing);
    appendStatus(response);
    sendJson(response);
}

void handleMachinesReset() {
    const std::vector<SavedMachine> previousMachines = savedMachines;
    const auto previousGenerations = machineGenerations;
    bool cancelled = true;
    std::vector<String> resetSerials;
    resetSerials.reserve(savedMachines.size());
    for (const auto& machine : savedMachines) {
        resetSerials.push_back(machine.serial);
    }
    {
        MachineGenerationLock generationLock;
        if (!generationLock) {
            cancelled = false;
        } else {
            for (const auto& machine : savedMachines) {
                if (!cancelTargetJobsAndCacheMetadata(machine.serial)) {
                    cancelled = false;
                    break;
                }
            }
            if (cancelled) {
                machineGenerations = {};
                savedMachines.clear();
            }
        }
    }
    if (!cancelled) {
        server.sendHeader("Retry-After", "1");
        sendError(503, "could not cancel machine jobs");
        return;
    }
    String persistenceError;
    if (!persistSavedMachines(&persistenceError)) {
        MachineGenerationLock rollbackLock;
        if (rollbackLock) {
            savedMachines = previousMachines;
            machineGenerations = previousGenerations;
            machinePersistenceDirty = true;
            String rollbackError;
            if (!persistSavedMachines(&rollbackError) && !rollbackError.isEmpty()) {
                persistenceError += String("; rollback persistence failed: ") + rollbackError;
            }
        } else {
            persistenceError += "; machine registry rollback lock failed";
        }
        sendError(500, persistenceError);
        return;
    }
    for (const String& serial : resetSerials) {
        removeTargetResourceCacheFiles(serial);
    }
    requestBleWorkerReset();
    clearAllStandardRecipeCaches();
    clearAllSavedRecipeCaches();
    clearAllBrewHistory();
    clearAllStatsHistory();
    refreshCachedStorageTotals();
    machinePreferences.remove(PREFS_MACHINE_STORE);
    DynamicJsonDocument doc(8192);
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
    if (!beginMachineProtocolSession(*machine, error)) {
        lastError = error;
        sendError(500, error);
        return;
    }
    noteWorkerProgress(45);
    if (!readMachineProcessStatus(processStatus, processError)) {
        lastError = processError;
        sendError(500, processError.isEmpty() ? String("failed to read machine status") : processError);
        return;
    }
    noteWorkerProgress(90);

    DynamicJsonDocument response(8192);
    response["ok"] = true;
    JsonObject item = response.createNestedObject("machine");
    appendSavedMachineJson(item, *machine);
    JsonObject details = response.createNestedObject("details");
    const DeviceDetails detailsSource = cachedDetails;
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
    bool cachePersisted = true;
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
            cachePersisted = false;
        }
    }
    if (!cachePersisted) {
        clearStandardRecipeCachesForMachine(machine->serial);
    }
    response["cachePersisted"] = cachePersisted;
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
        clearStandardRecipeCachesForMachine(machine->serial);
        response["cachePersisted"] = false;
        response["cacheError"] = cacheWriteError;
    } else {
        response["cachePersisted"] = true;
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
    noteWorkerProgress(20);

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
    noteWorkerProgress(50);
    JsonObjectConst recipeView = recipe;

    if (littleFsReady) {
        DynamicJsonDocument historyPreflightDoc(3072);
        JsonObject historyPreflightEntry = historyPreflightDoc.to<JsonObject>();
        brew_history::buildAcceptedEntry(request.as<JsonVariantConst>(),
                                         recipeView,
                                         nivona::ProcessStatus{},
                                         "",
                                         bridge_time::snapshot(),
                                         millis(),
                                         historyPreflightEntry);
        brew_history::CapacityCheck capacity;
        if (!brew_history::canAppendWithoutCompaction(machine->serial,
                                                      historyPreflightEntry,
                                                      BREW_HISTORY_PRECHECK_RESERVE_BYTES,
                                                      capacity,
                                                      error)) {
            if (error.isEmpty()) {
                DynamicJsonDocument response(8192);
                response["ok"] = false;
                response["error"] = "brew history storage is full; clear or export history before brewing again";
                response["historyStorageRejected"] = true;
                response["historyFileBytes"] = capacity.fileBytes;
                response["historyEntryBytes"] = capacity.entryBytes;
                response["historyReserveBytes"] = capacity.reserveBytes;
                response["historyProjectedBytes"] = capacity.projectedBytes;
                response["historyMaxBytes"] = capacity.maxBytes;
                appendStatus(response);
                sendJson(response, 507);
                return;
            }
            lastError = error;
            sendError(500, error);
            return;
        }
    }

    if (!uploadTemporaryStandardRecipe(layout, recipeView, static_cast<uint8_t>(selector), error)) {
        lastError = error;
        sendError(500, error);
        return;
    }
    noteWorkerProgress(75);

    const ByteVector* sessionKey = resolveStoredSessionIfAvailable();
    bool brewCommandSent = false;
    {
        WorkerMutationWriteScope mutationWrite;
        brewCommandSent = sendMachineCommand(
            "HE",
            nivona::buildHeMakeCoffeePayload(modelInfo, static_cast<uint8_t>(selector)),
            sessionKey,
            true,
            error);
    }
    if (!brewCommandSent) {
        lastError = error;
        sendError(500, error);
        return;
    }
    noteWorkerProgress(90);

    nivona::ProcessStatus processStatus;
    String processError;
    readMachineProcessStatus(processStatus, processError);

    DynamicJsonDocument response(8192);
    response["ok"] = true;
    response["selector"] = selector;
    response["temporaryRecipeUploaded"] = true;
    JsonObject recipeResponse = response.createNestedObject("recipe");
    recipeResponse.set(recipe);
    JsonObject status = response.createNestedObject("status");
    appendProcessStatusJson(status, processStatus, processError);
    DynamicJsonDocument historyDoc(4096);
    JsonObject historyEntry = historyDoc.to<JsonObject>();
    brew_history::buildAcceptedEntry(request.as<JsonVariantConst>(),
                                     recipeView,
                                     processStatus,
                                     processError,
                                     bridge_time::snapshot(),
                                     millis(),
                                     historyEntry);
    String historyError;
    bool historyLogged = false;
    if (littleFsReady) {
        WorkerMachineWriteGuard machineGuard;
        if (machineGuard) {
            historyLogged = brew_history::append(machine->serial, historyEntry, historyError);
        } else {
            historyError = "machine was deleted or replaced while the job was running";
        }
    }
    response["historyLogged"] = historyLogged;
    JsonObject history = response.createNestedObject("history");
    history["recipeFingerprint"] = historyEntry["recipeFingerprint"] | "";
    history["timeSynced"] = historyEntry["timeSynced"] | false;
    history["timeUnix"] = historyEntry["timeUnix"] | static_cast<int64_t>(0);
    history["timeIsoUtc"] = historyEntry["timeIsoUtc"] | "";
    history["timeSource"] = historyEntry["timeSource"] | "";
    if (!historyLogged) {
        if (historyError.isEmpty() && !littleFsReady) {
            historyError = "LittleFS is unavailable";
        }
        response["historyError"] = historyError;
        addLog("history", String("Brew history append skipped: ") + historyError);
    }
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
    bool confirmSent = false;
    {
        WorkerMutationWriteScope mutationWrite;
        confirmSent = sendMachineCommand(
            "HY", nivona::buildHyConfirmPayload(), sessionKey, true, error);
    }
    if (!confirmSent) {
        lastError = error;
        sendError(500, error);
        return;
    }

    nivona::ProcessStatus processStatus;
    String processError;
    readMachineProcessStatus(processStatus, processError);

    DynamicJsonDocument response(8192);
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

    String error;
    if (!beginMachineProtocolSession(*machine, error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    WorkerExecutionContext* execution = currentWorkerExecution();
    if (execution == nullptr || !execution->spoolResult || execution->resultPath.isEmpty()) {
        sendError(500, "saved-recipe list requires bounded worker result storage");
        return;
    }

    const String resultHeader = savedRecipeListHeader(*machine, false, error);
    if (resultHeader.isEmpty() ||
        !writeGeneratedJsonLiteral(execution->resultPath,
                                   resultHeader,
                                   true,
                                   MAX_JOB_RESULT_BYTES,
                                   true,
                                   error)) {
        lastError = error;
        sendError(507, error);
        return;
    }

    const String listCachePath = savedRecipeCachePath(machine->serial);
    const String listCacheTemporaryPath =
        listCachePath + "." + execution->jobId + ".tmp";
    const String cacheHeader = savedRecipeListHeader(*machine, true, error);
    bool cacheWritable = !cacheHeader.isEmpty() &&
        writeGeneratedJsonLiteral(listCacheTemporaryPath,
                                  cacheHeader,
                                  true,
                                  MAX_JOB_RESULT_BYTES,
                                  false,
                                  error);
    String cacheWriteError = cacheWritable ? String("") : error;
    error = "";
    std::vector<String> slotTemporaryPaths;
    std::vector<String> slotFinalPaths;
    auto removeCacheTemporaries = [&]() {
        littleFsRemoveLocked(listCacheTemporaryPath);
        for (const String& path : slotTemporaryPaths) {
            littleFsRemoveLocked(path);
        }
    };
    auto failResult = [&](int status, const String& message) {
        removeCacheTemporaries();
        littleFsRemoveLocked(execution->resultPath);
        lastError = message;
        sendError(status, message);
    };

    bool firstRecipe = true;
    for (uint8_t slot = 0; slot < layout.slotCount; ++slot) {
        DynamicJsonDocument itemDocument(SAVED_RECIPE_ITEM_JSON_CAPACITY);
        JsonObject item = itemDocument.to<JsonObject>();
        const bool itemSucceeded = appendMyCoffeeSlot(item, *machine, slot, true, error);
        if (!itemSucceeded) {
            if (isTransportReadFailure(error)) {
                failResult(500,
                           error.isEmpty() ? String("saved recipe refresh timed out") : error);
                return;
            }
            item["ok"] = false;
            item["error"] = error;
            error = "";
        } else {
            item["ok"] = true;
        }
        if (itemDocument.overflowed()) {
            failResult(507, "saved recipe slot JSON exceeded its bounded capacity");
            return;
        }
        const JsonVariantConst itemValue = itemDocument.as<JsonVariantConst>();
        if (!appendGeneratedJsonValue(execution->resultPath,
                                      itemValue,
                                      !firstRecipe,
                                      MAX_JOB_RESULT_BYTES,
                                      true,
                                      error)) {
            failResult(507, error);
            return;
        }
        if (cacheWritable &&
            !appendGeneratedJsonValue(listCacheTemporaryPath,
                                      itemValue,
                                      !firstRecipe,
                                      MAX_JOB_RESULT_BYTES,
                                      false,
                                      error)) {
            cacheWritable = false;
            cacheWriteError = error;
            removeCacheTemporaries();
        }
        if (cacheWritable && itemSucceeded) {
            const String slotFinalPath = savedRecipeSlotCachePath(machine->serial, slot + 1);
            const String slotTemporaryPath =
                slotFinalPath + "." + execution->jobId + ".tmp";
            if (!writeSavedRecipeSlotCacheTemporary(
                    *machine, item, slotTemporaryPath, error)) {
                cacheWritable = false;
                cacheWriteError = error;
                removeCacheTemporaries();
            } else {
                slotTemporaryPaths.push_back(slotTemporaryPath);
                slotFinalPaths.push_back(slotFinalPath);
            }
        }
        firstRecipe = false;
        noteWorkerProgress(static_cast<uint8_t>(10 +
            ((static_cast<uint32_t>(slot) + 1U) * 75U) / layout.slotCount));
    }

    if (cacheWritable &&
        !writeGeneratedJsonLiteral(listCacheTemporaryPath,
                                   "]}",
                                   false,
                                   MAX_JOB_RESULT_BYTES,
                                   false,
                                   error)) {
        cacheWritable = false;
        cacheWriteError = error;
        removeCacheTemporaries();
    }

    bool cachePersisted = false;
    if (cacheWritable) {
        WorkerMachineWriteGuard machineGuard;
        if (!machineGuard) {
            cacheWriteError = "machine was deleted or replaced while the job was running";
        } else if (!installGeneratedCacheFile(listCacheTemporaryPath,
                                               listCachePath,
                                               MAX_JOB_RESULT_BYTES,
                                               cacheWriteError)) {
            // The old aggregate cache remains in place on an installation
            // failure. Slot caches are not published unless the aggregate is.
        } else {
            cachePersisted = true;
            for (size_t index = 0; index < slotTemporaryPaths.size(); ++index) {
                String slotError;
                if (!installGeneratedCacheFile(slotTemporaryPaths[index],
                                               slotFinalPaths[index],
                                               MAX_RESOURCE_CACHE_BYTES,
                                               slotError)) {
                    cachePersisted = false;
                    cacheWriteError = slotError;
                    littleFsRemoveLocked(slotFinalPaths[index]);
                }
            }
        }
    }
    removeCacheTemporaries();
    if (!cachePersisted) {
        addLog("cache", String("Saved recipe cache write skipped: ") + cacheWriteError);
    }

    DynamicJsonDocument trailer(768);
    trailer["cachePersisted"] = cachePersisted;
    if (!cachePersisted) {
        trailer["cacheError"] = cacheWriteError;
    }
    String serializedTrailer;
    serializeJson(trailer, serializedTrailer);
    if (trailer.overflowed() || serializedTrailer.length() < 2 ||
        serializedTrailer.charAt(0) != '{') {
        failResult(507, "failed to build the bounded saved-recipe result trailer");
        return;
    }
    serializedTrailer.remove(0, 1);
    serializedTrailer = "]," + serializedTrailer;
    if (!writeGeneratedJsonLiteral(execution->resultPath,
                                   serializedTrailer,
                                   false,
                                   MAX_JOB_RESULT_BYTES,
                                   true,
                                   error)) {
        failResult(507, error);
        return;
    }

    // The result now exists as a complete bounded JSON file. The worker will
    // retain that path in the terminal job and the HTTP task streams it in
    // chunks without recreating the full response in heap memory.
    execution->responded = true;
    execution->responseStatus = 200;
    execution->errorCode = "";
    execution->errorMessage = "";
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
        WorkerMutationWriteScope mutationWrites;
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
        clearSavedRecipeCachesForMachine(machine->serial);
        response["cachePersisted"] = false;
        response["cacheError"] = cacheWriteError;
    } else {
        response["cachePersisted"] = true;
    }
    sendJson(response);
}

void appendCompactMachineDetails(JsonObject details, const SavedMachine& machine) {
    details["manufacturer"] = machine.manufacturer;
    details["model"] = machine.model;
    details["serial"] = machine.serial;
    details["hardwareRevision"] = machine.hardwareRevision;
    details["firmwareRevision"] = machine.firmwareRevision;
    details["softwareRevision"] = machine.softwareRevision;
    details["ad06Hex"] = machine.ad06Hex;
    details["ad06Ascii"] = machine.ad06Ascii;
}

bool buildCompactStatsSnapshot(SavedMachine& machine,
                               DynamicJsonDocument& response,
                               String& error) {
    if (!beginMachineProtocolSession(machine, error) || !ensureMachineHuSession(2500, error)) {
        return false;
    }
    const nivona::ModelInfo modelInfo = nivona::detectModelInfo(toNivonaDetails(machine));
    std::vector<const nivona::RegisterProbe*> metrics;
    nivona::selectStatsDescriptors(modelInfo, metrics);
    if (metrics.empty()) {
        error = "statistics are not supported for this machine family";
        return false;
    }
    noteWorkerProgress(15);

    response["ok"] = true;
    response["supported"] = true;
    response["modelFamily"] = modelInfo.familyKey;
    appendCompactMachineDetails(response.createNestedObject("details"), machine);
    JsonObject values = response.createNestedObject("values");
    const ByteVector* sessionKey = resolveStoredSessionIfAvailable(machine.serial, machine.address);
    size_t metricIndex = 0;
    for (const nivona::RegisterProbe* metric : metrics) {
        if (workerDeadlineExceeded()) {
            error = "job deadline exceeded while reading statistics";
            return false;
        }
        std::vector<ByteVector> chunks;
        if (!sendMachineCommand("HR",
                                buildRegisterPayload(metric->id),
                                sessionKey,
                                true,
                                error,
                                3000,
                                &chunks)) {
            return false;
        }
        uint16_t echoedRegisterId = 0;
        int32_t rawValue = 0;
        if (!nivona::decodeHrNumericResponse(chunks, true, echoedRegisterId, rawValue, error) ||
            echoedRegisterId != metric->id) {
            if (error.isEmpty()) {
                error = String("statistics register echo mismatch for ") + metric->id;
            }
            return false;
        }
        JsonObject item = values.createNestedObject(metric->name);
        item["title"] = metric->title != nullptr ? metric->title : metric->name;
        item["section"] = metric->section != nullptr ? metric->section : "maintenance";
        item["unit"] = metric->unit != nullptr ? metric->unit : "count";
        item["registerId"] = metric->id;
        item["rawValue"] = rawValue;
        ++metricIndex;
        noteWorkerProgress(static_cast<uint8_t>(15 + (metricIndex * 75) / metrics.size()));
    }
    return true;
}

bool buildCompactSettingsSnapshot(SavedMachine& machine,
                                  DynamicJsonDocument& response,
                                  String& error) {
    if (!beginMachineProtocolSession(machine, error) || !ensureMachineHuSession(2500, error)) {
        return false;
    }
    nivona::SettingsProbeContext context;
    if (!nivona::resolveSettingsProbeContext(
            toNivonaDetails(machine), nivona::SettingsFamily::Unknown, context, error)) {
        return false;
    }
    std::vector<const nivona::SettingProbeDescriptor*> probes;
    nivona::selectSettingsDescriptors(context, probes);
    if (probes.empty()) {
        error = String("settings family \"") + context.familyKey + "\" is not supported yet";
        return false;
    }
    noteWorkerProgress(15);

    response["ok"] = true;
    response["supported"] = true;
    response["resolvedFamily"] = context.familyKey;
    response["familySource"] = context.familySource;
    response["modelCodeHint"] = context.modelCodeHint;
    response["is79xModel"] = context.is79xModel;
    response["hasAromaBalanceProfile"] = context.hasAromaBalanceProfile;
    appendCompactMachineDetails(response.createNestedObject("details"), machine);
    JsonObject values = response.createNestedObject("values");
    const ByteVector* sessionKey = resolveStoredSessionIfAvailable(machine.serial, machine.address);
    size_t probeIndex = 0;
    for (const nivona::SettingProbeDescriptor* probe : probes) {
        if (workerDeadlineExceeded()) {
            error = "job deadline exceeded while reading settings";
            return false;
        }
        std::vector<ByteVector> chunks;
        if (!sendMachineCommand("HR",
                                buildRegisterPayload(probe->id),
                                sessionKey,
                                true,
                                error,
                                3000,
                                &chunks)) {
            return false;
        }
        uint16_t echoedRegisterId = 0;
        int32_t rawValue = 0;
        if (!nivona::decodeHrNumericResponse(chunks, true, echoedRegisterId, rawValue, error) ||
            echoedRegisterId != probe->id) {
            if (error.isEmpty()) {
                error = String("settings register echo mismatch for ") + probe->id;
            }
            return false;
        }
        const uint16_t code = static_cast<uint16_t>(rawValue & 0xFFFF);
        JsonObject item = values.createNestedObject(probe->name);
        item["title"] = probe->title;
        item["registerId"] = probe->id;
        item["rawValue"] = rawValue;
        item["valueCodeHex"] = nivona::formatCodeHex(code);
        const char* label = nivona::findSettingValueLabel(*probe, code);
        item["valueLabel"] = label != nullptr ? label : "";
        appendSettingOptions(item.createNestedArray("options"), *probe);
        ++probeIndex;
        noteWorkerProgress(static_cast<uint8_t>(15 + (probeIndex * 75) / probes.size()));
    }
    return true;
}

bool appendStatsHistorySnapshot(DynamicJsonDocument& response,
                                const String& serial,
                                const String& source,
                                String& error) {
    if (!littleFsReady) {
        return true;
    }

    const JsonObjectConst values = response["values"].as<JsonObjectConst>();
    if (values.isNull() || values.size() == 0) {
        return true;
    }

    stats_history::AppendResult appendResult;
    WorkerMachineWriteGuard machineGuard;
    if (!machineGuard) {
        error = "machine was deleted or replaced while the job was running";
        return false;
    }
    if (!stats_history::recordSnapshotIfChanged(serial,
                                                values,
                                                bridge_time::snapshot(),
                                                millis(),
                                                source,
                                                appendResult,
                                                error)) {
        return false;
    }

    JsonObject statsHistory = response.createNestedObject("statsHistory");
    statsHistory["appended"] = appendResult.appended;
    statsHistory["baseline"] = appendResult.baseline;
    statsHistory["changedCount"] = appendResult.changedMetricCount;
    if (appendResult.hasTotalDelta) {
        statsHistory["totalDelta"] = appendResult.totalDelta;
    }
    return true;
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

    DynamicJsonDocument response(16384);
    if (!buildCompactStatsSnapshot(*machine, response, error)) {
        lastError = error;
        response["error"] = error;
        sendJson(response, 500);
        return;
    }
    updateSavedMachineFromCachedDetails(*machine);
    String snapshotError;
    if (!appendStatsHistorySnapshot(response, machine->serial, "api-stats", snapshotError)) {
        response["statsHistoryError"] = snapshotError;
        addLog("stats-history", String("Stats history append skipped: ") + snapshotError);
    }
    sendJson(response);
}

void handleMachineFeaturesGet(const String& serial) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }

    const nivona::ModelInfo modelInfo = nivona::detectModelInfo(toNivonaDetails(*machine));
    if (nivona::isHiFeatureReadKnownUnavailable(modelInfo)) {
        DynamicJsonDocument response(2048);
        response["ok"] = true;
        response["supported"] = false;
        response["reasonCode"] = "hi_unavailable_for_model";
        response["reason"] = "This machine model is known not to answer the HI capability request; the bridge skips the live probe.";
        JsonObject machineJson = response.createNestedObject("machine");
        appendSavedMachineJson(machineJson, *machine);
        JsonObject featureJson = response.createNestedObject("features");
        featureJson["ok"] = true;
        featureJson["available"] = false;
        featureJson["command"] = nivona::CMD_HI;
        featureJson["source"] = "live-validation";
        sendJson(response);
        return;
    }

    String error;
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

    std::vector<ByteVector> chunks;
    const ByteVector* sessionKey = resolveStoredSessionIfAvailable();
    if (!sendMachineCommand(nivona::CMD_HI, ByteVector{}, sessionKey, true, error, 2500, &chunks)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    nivona::MachineFeatures features;
    if (!nivona::decodeHiResponse(chunks, true, features, error)) {
        lastError = error;
        sendError(500, error);
        return;
    }

    DynamicJsonDocument response(8192);
    response["ok"] = true;
    response["supported"] = true;
    JsonObject machineJson = response.createNestedObject("machine");
    appendSavedMachineJson(machineJson, *machine);
    JsonObject featureJson = response.createNestedObject("features");
    appendMachineFeaturesJson(featureJson, features);
    JsonObject protocolSession = response.createNestedObject("protocolSession");
    appendProtocolSessionJson(protocolSession, resolveStoredSessionEntry(machine->serial, machine->address));
    sendJson(response);
}

void handleMachineStatsHistory(const String& serial) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }
    if (!littleFsReady) {
        lastError = "LittleFS is unavailable";
        sendError(503, lastError.snapshot());
        return;
    }

    const int limitArg = server.hasArg("limit") ? server.arg("limit").toInt() : 20;
    const int offsetArg = server.hasArg("offset") ? server.arg("offset").toInt() : 0;
    const size_t limit = static_cast<size_t>(std::max(1, std::min(limitArg, 100)));
    const size_t offset = static_cast<size_t>(std::max(0, offsetArg));

    stats_history::Stats stats;
    stats_history::Page page;
    String error;
    HistoryStreamContext stream;
    if (!beginHistoryStream(machine->serial)) {
        return;
    }
    if (!stats_history::visitPage(machine->serial,
                                  offset,
                                  limit,
                                  streamHistoryEntry,
                                  &stream,
                                  stats,
                                  page,
                                  error)) {
        lastError = error;
    }
    finishHistoryStream(stats, page, stream, error);
}

void handleMachineStatsHistoryClear(const String& serial) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }
    if (!littleFsReady) {
        lastError = "LittleFS is unavailable";
        sendError(503, lastError.snapshot());
        return;
    }

    String error;
    if (!stats_history::clear(machine->serial, error)) {
        lastError = error;
        sendError(500, error);
        return;
    }
    refreshCachedStorageTotals();

    DynamicJsonDocument response(1024);
    response["ok"] = true;
    response["serial"] = machine->serial;
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

    // The compact payload is capped at ten settings and ten options per
    // setting. Keeping this below the largest observed free heap block avoids
    // turning an allocation failure into an empty successful cache entry.
    DynamicJsonDocument response(16384);
    if (!buildCompactSettingsSnapshot(*machine, response, error)) {
        lastError = error;
        response["error"] = error;
        sendJson(response, 500);
        return;
    }
    updateSavedMachineFromCachedDetails(*machine);
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
        bool resetCommandSent = false;
        {
            WorkerMutationWriteScope mutationWrite;
            resetCommandSent = sendMachineCommand(
                "HE", nivona::buildHeCommandPayload(commandId), sessionKey, true, error);
        }
        if (!resetCommandSent) {
            lastError = error;
            sendError(500, error);
            return;
        }

        nivona::ProcessStatus processStatus;
        String processError;
        readMachineProcessStatus(processStatus, processError);
        if (action == "factory_reset_settings") {
            invalidateResourceCache(machine->serial, "settings");
        } else {
            clearStandardRecipeCachesForMachine(machine->serial);
            clearSavedRecipeCachesForMachine(machine->serial);
            refreshCachedStorageTotals();
        }

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

    bool settingWritten = false;
    {
        WorkerMutationWriteScope mutationWrite;
        settingWritten = writeMachineNumericRegister(
            probe->id, static_cast<int32_t>(code), error);
    }
    if (!settingWritten) {
        lastError = error;
        sendError(500, error);
        return;
    }
    invalidateResourceCache(machine->serial, "settings");

    DynamicJsonDocument response(2048);
    response["ok"] = true;
    response["key"] = key;
    response["code"] = code;
    response["label"] = nivona::findSettingValueLabel(*probe, code);
    sendJson(response);
}

bool cancelTargetJobsAndCacheMetadata(const String& serial);
void removeTargetResourceCacheFiles(const String& serial);

void handleMachineDelete(const String& serial) {
    for (auto it = savedMachines.begin(); it != savedMachines.end(); ++it) {
        if (it->serial.equalsIgnoreCase(serial)) {
            const String canonicalSerial = it->serial;
            const std::vector<SavedMachine> previousMachines = savedMachines;
            const auto previousGenerations = machineGenerations;
            bool cancelled = false;
            {
                MachineGenerationLock generationLock;
                if (generationLock && cancelTargetJobsAndCacheMetadata(canonicalSerial)) {
                    removeMachineGenerationLocked(canonicalSerial);
                    cancelled = true;
                }
            }
            if (!cancelled) {
                server.sendHeader("Retry-After", "1");
                sendError(503, "could not cancel machine jobs");
                return;
            }
            savedMachines.erase(it);
            String persistenceError;
            if (!persistSavedMachines(&persistenceError)) {
                MachineGenerationLock rollbackLock;
                if (rollbackLock) {
                    savedMachines = previousMachines;
                    machineGenerations = previousGenerations;
                    machinePersistenceDirty = true;
                    String rollbackError;
                    if (!persistSavedMachines(&rollbackError) && !rollbackError.isEmpty()) {
                        persistenceError += String("; rollback persistence failed: ") + rollbackError;
                    }
                } else {
                    persistenceError += "; machine registry rollback lock failed";
                }
                sendError(500, persistenceError);
                return;
            }
            removeTargetResourceCacheFiles(canonicalSerial);
            requestBleWorkerReset();
            clearStandardRecipeCachesForMachine(canonicalSerial);
            clearSavedRecipeCachesForMachine(canonicalSerial);
            clearBrewHistoryForMachine(canonicalSerial);
            clearStatsHistoryForMachine(canonicalSerial);
            refreshCachedStorageTotals();
            DynamicJsonDocument response(8192);
            response["ok"] = true;
            appendStatus(response);
            sendJson(response);
            return;
        }
    }
    sendError(404, "saved machine not found");
}

void handleMachineResourceRequest(const String& serial, const String& resource);
void handleMachineRefreshRequest(const String& serial);
void enqueueMachineOperation(const String& serial,
                             const String& kind,
                             BleOperation operation,
                             bridge_jobs::Priority priority,
                             uint32_t deadlineMs,
                             const String& resultUrl = "",
                             bool resource = false,
                             bool requireBody = false,
                             const String& coalesceKey = "",
                             const String& argument = "");

bool trySendCachedStandardRecipe(const String& serial, const String& selectorText) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    size_t selector = 0;
    if (machine == nullptr || !parseUnsignedPathIndex(selectorText, selector) || selector > 255) {
        return false;
    }
    DynamicJsonDocument cachedResponse(16384);
    String cacheError;
    if (!loadStandardRecipeCache(*machine, static_cast<uint8_t>(selector), cachedResponse, cacheError)) {
        return false;
    }
    DynamicJsonDocument response(16384);
    response["ok"] = true;
    response["source"] = "cache";
    response["cached"] = true;
    response.createNestedObject("recipe").set(cachedResponse["recipe"].as<JsonObject>());
    sendJson(response);
    return true;
}

bool trySendCachedMyCoffeeList(const String& serial) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        return false;
    }
    const String cachePath = savedRecipeCachePath(machine->serial);
    String cacheError;
    if (!validateSavedRecipeCache(*machine,
                                  cachePath,
                                  "mycoffee_list",
                                  SAVED_RECIPE_CACHE_SCHEMA,
                                  0,
                                  cacheError)) {
        if (cacheError == "filesystem is busy") {
            server.sendHeader("Retry-After", "1");
            sendError(503, cacheError);
            return true;
        }
        littleFsRemoveLocked(cachePath);
        return false;
    }
    bool responseStarted = false;
    if (!streamJsonFileResponse(
            cachePath, MAX_JOB_RESULT_BYTES, 200, responseStarted, cacheError) &&
        !responseStarted) {
        server.sendHeader("Retry-After", "1");
        sendError(503, cacheError);
    }
    return true;
}

bool trySendCachedMyCoffeeSlot(const String& serial, const String& slotText) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    size_t slot = 0;
    if (machine == nullptr || !parseUnsignedPathIndex(slotText, slot) ||
        slot == 0 || slot > 255) {
        return false;
    }
    const String cachePath = savedRecipeSlotCachePath(
        machine->serial, static_cast<uint8_t>(slot));
    String cacheError;
    if (!validateSavedRecipeCache(*machine,
                                  cachePath,
                                  "mycoffee_slot",
                                  SAVED_RECIPE_SLOT_CACHE_SCHEMA,
                                  static_cast<uint8_t>(slot),
                                  cacheError)) {
        if (cacheError == "filesystem is busy") {
            server.sendHeader("Retry-After", "1");
            sendError(503, cacheError);
            return true;
        }
        littleFsRemoveLocked(cachePath);
        return false;
    }
    bool responseStarted = false;
    if (!streamJsonFileResponse(
            cachePath, MAX_RESOURCE_CACHE_BYTES, 200, responseStarted, cacheError) &&
        !responseStarted) {
        server.sendHeader("Retry-After", "1");
        sendError(503, cacheError);
    }
    return true;
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
        handleMachineResourceRequest(serial, "summary");
        return true;
    }
    if (section == "refresh" && server.method() == HTTP_POST) {
        handleMachineRefreshRequest(serial);
        return true;
    }
    if (section == "recipes" && server.method() == HTTP_POST && tail == "refresh") {
        enqueueMachineOperation(serial,
                                "recipes_refresh",
                                BleOperation::MachineRecipesRefresh,
                                bridge_jobs::Priority::ForcedRead,
                                60000);
        return true;
    }
    if (section == "recipes" && server.method() == HTTP_GET) {
        if (tail.isEmpty()) {
            handleMachineRecipes(serial);
        } else {
            if (!parseRefreshArg() && trySendCachedStandardRecipe(serial, tail)) {
                return true;
            } else {
                enqueueMachineOperation(serial,
                                        "recipe",
                                        BleOperation::MachineRecipeDetail,
                                        bridge_jobs::Priority::ForcedRead,
                                        20000,
                                        "",
                                        false,
                                        false,
                                        "",
                                        tail);
            }
        }
        return true;
    }
    if (section == "brew" && server.method() == HTTP_POST) {
        enqueueMachineOperation(serial,
                                "brew",
                                BleOperation::MachineBrew,
                                bridge_jobs::Priority::Mutation,
                                30000,
                                "",
                                false,
                                true);
        return true;
    }
    if (section == "history" && server.method() == HTTP_GET && tail.isEmpty()) {
        handleMachineHistory(serial);
        return true;
    }
    if (section == "history" && server.method() == HTTP_POST && tail == "clear") {
        handleMachineHistoryClear(serial);
        return true;
    }
    if (section == "history" && server.method() == HTTP_POST && tail == "import") {
        handleMachineHistoryImport(serial);
        return true;
    }
    if (section == "history" && server.method() == HTTP_PATCH && !tail.isEmpty()) {
        handleMachineHistoryPatch(serial, tail);
        return true;
    }
    if (section == "history" && server.method() == HTTP_DELETE && !tail.isEmpty()) {
        handleMachineHistoryDelete(serial, tail);
        return true;
    }
    if (section == "stats" && server.method() == HTTP_GET && tail == "history") {
        handleMachineStatsHistory(serial);
        return true;
    }
    if (section == "stats" && server.method() == HTTP_POST && tail == "history/clear") {
        handleMachineStatsHistoryClear(serial);
        return true;
    }
    if (section == "confirm" && server.method() == HTTP_POST) {
        enqueueMachineOperation(serial,
                                "confirm",
                                BleOperation::MachineConfirm,
                                bridge_jobs::Priority::Mutation,
                                30000);
        return true;
    }
    if (section == "mycoffee" && server.method() == HTTP_GET && tail.isEmpty()) {
        if (!parseRefreshArg() && trySendCachedMyCoffeeList(serial)) {
            return true;
        } else {
            enqueueMachineOperation(serial,
                                    "mycoffee",
                                    BleOperation::MachineMyCoffeeList,
                                    bridge_jobs::Priority::ForcedRead,
                                    60000);
        }
        return true;
    }
    if (section == "mycoffee" && !tail.isEmpty()) {
        const bool update = server.method() == HTTP_POST;
        if (!update && !parseRefreshArg() && trySendCachedMyCoffeeSlot(serial, tail)) {
            return true;
        } else {
            enqueueMachineOperation(serial,
                                    update ? "mycoffee_write" : "mycoffee_slot",
                                    update ? BleOperation::MachineMyCoffeeUpdate : BleOperation::MachineMyCoffeeDetail,
                                    update ? bridge_jobs::Priority::Mutation : bridge_jobs::Priority::ForcedRead,
                                    update ? 30000 : 20000,
                                    "",
                                    false,
                                    update,
                                    "",
                                    tail);
        }
        return true;
    }
    if (section == "stats" && server.method() == HTTP_GET) {
        handleMachineResourceRequest(serial, "stats");
        return true;
    }
    if (section == "features" && server.method() == HTTP_GET) {
        handleMachineResourceRequest(serial, "features");
        return true;
    }
    if (section == "settings" && server.method() == HTTP_GET) {
        handleMachineResourceRequest(serial, "settings");
        return true;
    }
    if (section == "settings" && server.method() == HTTP_POST) {
        enqueueMachineOperation(serial,
                                "settings_write",
                                BleOperation::MachineSettingsPost,
                                bridge_jobs::Priority::Mutation,
                                30000,
                                "",
                                false,
                                true);
        return true;
    }
    return false;
}

void handleRoot() {
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200,
                  "text/html; charset=utf-8",
                  reinterpret_cast<PGM_P>(web_ui::kPageGzip),
                  web_ui::kPageGzipSize);
}

void handleRecipeIconAsset() {
    const String requestedKey = server.arg("name");
    const recipe_icons::Asset* asset = recipe_icons::findAsset(requestedKey);
    if (asset == nullptr) {
        asset = recipe_icons::findAsset(recipe_icons::defaultKey());
    }
    if (asset == nullptr || asset->data == nullptr || asset->size == 0) {
        sendError(404, "recipe icon not found");
        return;
    }
    server.sendHeader("Cache-Control", "public, max-age=604800");
    server.send_P(200,
                  "image/webp",
                  reinterpret_cast<PGM_P>(asset->data),
                  asset->size);
}

void handleStatus() {
    maybeApplyClientTimeHeader();
    DynamicJsonDocument doc(STATUS_JSON_CAPACITY);
    doc["ok"] = true;
    appendStatus(doc);
    sendJson(doc);
}

void handleDevices() {
    DynamicJsonDocument doc(8192);
    doc["ok"] = true;
    JsonArray devices = doc.createNestedArray("devices");
    if (scanDataMutex == nullptr || xSemaphoreTake(scanDataMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        sendError(503, "scan results are busy");
        return;
    }
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
    xSemaphoreGive(scanDataMutex);
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

    wifiReconnectRequested.store(true, std::memory_order_release);

    DynamicJsonDocument response(8192);
    response["ok"] = true;
    response["reconnectPending"] = true;
    appendStatus(response);
    sendJson(response);
}

void handleTimeConfigSave() {
    DynamicJsonDocument request(1024);
    String error;
    if (!parseJsonBody(request, error)) {
        sendError(400, error);
        return;
    }

    if (!bridge_time::saveConfig(request["mode"] | "ntp",
                                 request["ntpServerPrimary"] | "",
                                 request["ntpServerSecondary"] | "",
                                 request["ntpServerTertiary"] | "",
                                 error,
                                 addTimeLog)) {
        sendError(400, error.isEmpty() ? String("failed to save time configuration") : error);
        return;
    }

    bridge_time::tick(WiFi.status() == WL_CONNECTED, millis(), addTimeLog);

    DynamicJsonDocument response(8192);
    response["ok"] = true;
    appendStatus(response);
    sendJson(response);
}

void handleHistoryConfigSave() {
    DynamicJsonDocument request(1024);
    String error;
    if (!parseJsonBody(request, error)) {
        sendError(400, error);
        return;
    }
    if (!littleFsReady) {
        lastError = "LittleFS is unavailable";
        sendError(503, lastError.snapshot());
        return;
    }
    const uint32_t requestedBytes = request["budgetBytes"] | 0U;
    if (requestedBytes == 0U) {
        sendError(400, "budgetBytes is required");
        return;
    }

    size_t totalBytes = 0;
    {
        history_storage::Guard filesystem(5000);
        if (!filesystem) {
            sendError(503, "filesystem is busy");
            return;
        }
        totalBytes = LittleFS.totalBytes();
    }
    const ConfiguredHistoryBudgets appliedBudgets = calculateHistoryBudgets(
        requestedBytes, totalBytes);
    const size_t appliedBytes = appliedBudgets.brewBytesPerMachine;
    const size_t previousBytes = brew_history::budgetBytes();
    const size_t previousStatsBytes = stats_history::budgetBytes();
    history_storage::HistoryUsage usage;
    String usageError;
    if (!history_storage::inspectHistoryUsage(usage, usageError)) {
        sendError(500, usageError);
        return;
    }
    if (!history_retention::canLowerWithoutDataLoss(
            appliedBytes, usage.largestBrewFileBytes)) {
        DynamicJsonDocument response(1024);
        response["ok"] = false;
        response["error"] = "budget is below the largest persisted history file";
        response["minimumLosslessBudgetBytes"] = usage.largestBrewFileBytes;
        sendJson(response, 409);
        return;
    }
    applyHistoryBudgets(appliedBudgets,
                        totalBytes,
                        usage.largestBrewFileBytes,
                        usage.largestStatsFileBytes);
    if (preferences.putUInt(PREFS_HISTORY_MAX_BYTES,
                            static_cast<uint32_t>(appliedBytes)) != sizeof(uint32_t) ||
        preferences.getUInt(PREFS_HISTORY_MAX_BYTES, 0) != appliedBytes) {
        brew_history::configureBudget(previousBytes, totalBytes);
        stats_history::configureBudget(previousStatsBytes, totalBytes);
        sendError(500, "failed to persist the history budget");
        return;
    }
    refreshCachedStorageTotals();

    DynamicJsonDocument response(8192);
    response["ok"] = true;
    response["requestedBudgetBytes"] = requestedBytes;
    response["budgetBytes"] = appliedBytes;
    appendStatus(response);
    sendJson(response);
}

class BufferedBackupLineReader {
public:
    BufferedBackupLineReader(File& source, const String& path)
        : source_(source), path_(path) {}

    bool next(String& line, bool& available, String& error) {
        line = "";
        available = false;
        while (true) {
            if (bufferOffset_ >= bufferLength_) {
                const int remaining = source_.available();
                if (remaining <= 0) {
                    available = !line.isEmpty();
                    return true;
                }
                bufferLength_ = source_.read(
                    buffer_, std::min(sizeof(buffer_), static_cast<size_t>(remaining)));
                bufferOffset_ = 0;
                if (bufferLength_ == 0) {
                    error = String("failed to read backup source ") + path_;
                    return false;
                }
                bytesSinceYield_ += bufferLength_;
                if (bytesSinceYield_ >= 4096) {
                    // Backup generation runs on the HTTP task. Give the
                    // core's idle task a scheduling window while scanning a
                    // large file so an export cannot trip the task watchdog.
                    vTaskDelay(1);
                    bytesSinceYield_ = 0;
                }
            }

            const char value = static_cast<char>(buffer_[bufferOffset_++]);
            if (value == '\n') {
                available = true;
                return true;
            }
            if (line.length() >= history_storage::MAX_JSON_LINE_BYTES) {
                error = String("backup source line exceeds limit in ") + path_;
                return false;
            }
            if (!line.concat(value)) {
                error = "insufficient memory for a backup source line";
                return false;
            }
        }
    }

private:
    File& source_;
    const String& path_;
    uint8_t buffer_[512]{};
    size_t bufferOffset_{0};
    size_t bufferLength_{0};
    size_t bytesSinceYield_{0};
};

bool processBackupHistory(const String& path,
                          const String& kind,
                          const String& serial,
                          bool emit,
                          size_t& bundleBytes,
                          String& error) {
    error = "";
    DynamicJsonDocument recordDoc(256);
    recordDoc["kind"] = kind;
    recordDoc["serial"] = serial;
    String prefix;
    serializeJson(recordDoc, prefix);
    if (!prefix.endsWith("}")) {
        error = "failed to serialize backup record prefix";
        return false;
    }
    prefix.remove(prefix.length() - 1);
    prefix += ",\"entry\":";

    // Keep the output buffer bounded, but do not build and reparse a second
    // multi-entry JSON document. On fragmented ESP32 heaps that duplicate
    // 12 KiB allocation could fail and make every otherwise-valid history
    // line look unexportable.
    String outputBatch;
    if (emit && !outputBatch.reserve(MAX_BACKUP_JSON_LINE_BYTES + 1)) {
        error = "insufficient memory for the backup output buffer";
        return false;
    }
    DynamicJsonDocument sourceEntryDoc(12288);
    if (sourceEntryDoc.capacity() == 0) {
        error = "insufficient memory for the backup history parser";
        return false;
    }
    auto flushOutput = [&]() -> bool {
        if (!emit || outputBatch.isEmpty()) {
            return true;
        }
        if (!server.sendContent(outputBatch)) {
            error = "backup client disconnected";
            return false;
        }
        outputBatch = "";
        return true;
    };

    if (!LittleFS.exists(path)) {
        return true;
    }
    File source = LittleFS.open(path, "r");
    if (!source) {
        error = String("failed to open backup source ") + path;
        return false;
    }

    BufferedBackupLineReader reader(source, path);
    String entry;
    if (!entry.reserve(512)) {
        source.close();
        error = "insufficient memory for a backup source line";
        return false;
    }
    while (true) {
        bool lineAvailable = false;
        if (!reader.next(entry, lineAvailable, error)) {
            source.close();
            return false;
        }
        if (!lineAvailable) {
            break;
        }
        if (entry.endsWith("\r")) {
            entry.remove(entry.length() - 1);
        }
        if (entry.isEmpty()) {
            continue;
        }

        sourceEntryDoc.clear();
        const DeserializationError parseError = deserializeJson(sourceEntryDoc, entry);
        if (parseError == DeserializationError::NoMemory) {
            source.close();
            error = "insufficient memory while parsing backup history";
            return false;
        }
        if (parseError) {
            // Preserve physical-line accounting in the history file itself,
            // but never let a torn/corrupt line poison an otherwise valid
            // bridge backup.
            continue;
        }
        std::vector<String> normalizedLines;
        size_t normalizedCount = 0;
        String normalizationError;
        const bool normalized = kind == "history"
            ? brew_history::buildImportedLines(sourceEntryDoc.as<JsonVariantConst>(),
                                               normalizedLines,
                                               normalizedCount,
                                               normalizationError)
            : stats_history::buildImportedLines(sourceEntryDoc.as<JsonVariantConst>(),
                                                normalizedLines,
                                                normalizedCount,
                                                normalizationError);
        if (!normalized || normalizedCount != 1 || normalizedLines.size() != 1) {
            source.close();
            error = normalizationError.isEmpty()
                ? String("failed to normalize a backup history entry")
                : normalizationError;
            return false;
        }
        entry = normalizedLines.front();

        String record;
        const size_t recordBytes = prefix.length() + entry.length() + 2;
        if (recordBytes > MAX_BACKUP_JSON_LINE_BYTES ||
            !record.reserve(recordBytes + 1)) {
            source.close();
            error = recordBytes > MAX_BACKUP_JSON_LINE_BYTES
                ? String("backup history record exceeds the bounded line limit")
                : String("insufficient memory for a backup history record");
            return false;
        }
        record += prefix;
        record += entry;
        record += "}\n";
        if (record.length() > history_capacity::MAX_GENERATED_BACKUP_BYTES -
                std::min(bundleBytes, history_capacity::MAX_GENERATED_BACKUP_BYTES)) {
            source.close();
            error = String("generated backup exceeds ") +
                history_capacity::MAX_GENERATED_BACKUP_BYTES + " bytes";
            return false;
        }
        bundleBytes += record.length();
        if (emit) {
            if (!outputBatch.isEmpty() &&
                outputBatch.length() + record.length() > MAX_BACKUP_JSON_LINE_BYTES &&
                !flushOutput()) {
                source.close();
                return false;
            }
            if (!outputBatch.concat(record)) {
                source.close();
                error = "insufficient memory while buffering backup output";
                return false;
            }
        }
    }
    source.close();
    return flushOutput();
}

void handleBackupExport() {
    std::vector<SavedMachine> exportMachines;
    {
        MachineGenerationLock registry;
        if (!registry) {
            server.sendHeader("Retry-After", "1");
            sendError(503, "machine registry is busy");
            return;
        }
        exportMachines = savedMachines;

        if (littleFsReady) {
            configureHistoryBudget();
            refreshCachedStorageTotals();
        }
    }

    // Hold the shared filesystem lock for the explicit backup operation. This
    // makes the size preflight and emitted bundle the same immutable snapshot.
    history_storage::Guard exportFilesystem(5000);
    if (!exportFilesystem) {
        server.sendHeader("Retry-After", "1");
        sendError(503, "filesystem is busy");
        return;
    }

    DynamicJsonDocument metaDoc(2048);
    metaDoc["kind"] = "meta";
    metaDoc["schema"] = BACKUP_BUNDLE_SCHEMA;
    metaDoc["appName"] = APP_NAME;
    metaDoc["appVersion"] = APP_VERSION;
    metaDoc["buildTime"] = APP_BUILD_TIME;
    metaDoc["historyBudgetBytes"] = brew_history::budgetBytes();
    metaDoc["statsHistoryBudgetBytes"] = stats_history::budgetBytes();
    metaDoc["writableAggregateLimitBytes"] =
        history_storage::writableHistoryLimit(LittleFS.totalBytes());
    metaDoc["savedMachineCount"] = exportMachines.size();
    metaDoc["littleFsReady"] = littleFsReady;
    metaDoc["exportedAtMs"] = millis();
    const bridge_time::StatusSnapshot timeStatus = bridge_time::snapshot();
    metaDoc["timeSynced"] = timeStatus.synced;
    if (timeStatus.synced) {
        metaDoc["exportedAtUnix"] = static_cast<int64_t>(timeStatus.unixTime);
        metaDoc["exportedAtIsoUtc"] = timeStatus.iso8601Utc;
    }
    JsonObject includes = metaDoc.createNestedObject("includes");
    includes["savedMachines"] = true;
    includes["historyBudget"] = true;
    includes["brewHistory"] = littleFsReady;
    includes["statsHistory"] = littleFsReady;
    includes["wifi"] = false;
    includes["protocolSessions"] = false;
    includes["standardRecipeCaches"] = false;
    includes["savedRecipeCaches"] = false;

    String metaLine;
    serializeJson(metaDoc, metaLine);
    metaLine += '\n';

    size_t bundleBytes = metaLine.length();
    String preflightError;
    for (const SavedMachine& machine : exportMachines) {
        DynamicJsonDocument machineDoc(4096);
        machineDoc["kind"] = "machine";
        JsonObject item = machineDoc.createNestedObject("machine");
        appendBackupMachineJson(item, machine);
        String machineLine;
        serializeJson(machineDoc, machineLine);
        machineLine += '\n';
        if (machineLine.length() > history_capacity::MAX_GENERATED_BACKUP_BYTES -
                std::min(bundleBytes, history_capacity::MAX_GENERATED_BACKUP_BYTES)) {
            preflightError = String("generated backup exceeds ") +
                history_capacity::MAX_GENERATED_BACKUP_BYTES + " bytes";
            break;
        }
        bundleBytes += machineLine.length();

        if (!littleFsReady) {
            continue;
        }

        if (!processBackupHistory(brew_history::filePath(machine.serial),
                                  "history",
                                  machine.serial,
                                  false,
                                  bundleBytes,
                                  preflightError)) {
            break;
        }
        if (!processBackupHistory(stats_history::filePath(machine.serial),
                                  "stats_history",
                                  machine.serial,
                                  false,
                                  bundleBytes,
                                  preflightError)) {
            break;
        }
    }
    if (!preflightError.isEmpty()) {
        lastError = preflightError;
        sendError(preflightError.startsWith("generated backup exceeds") ? 507 : 500,
                  preflightError);
        return;
    }

    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Content-Disposition", String("attachment; filename=\"") + backupDownloadFilename() + "\"");
    server.setContentLength(bundleBytes);
    server.send(200, "application/x-ndjson", "");
    if (!server.sendContent(metaLine)) {
        addLog("backup", "Backup client disconnected before history export");
        return;
    }

    size_t emittedBytes = metaLine.length();
    String emitError;
    for (const SavedMachine& machine : exportMachines) {
        DynamicJsonDocument machineDoc(4096);
        machineDoc["kind"] = "machine";
        JsonObject item = machineDoc.createNestedObject("machine");
        appendBackupMachineJson(item, machine);
        String machineLine;
        serializeJson(machineDoc, machineLine);
        machineLine += '\n';
        emittedBytes += machineLine.length();
        if (!server.sendContent(machineLine)) {
            emitError = "backup client disconnected";
            break;
        }

        if (!littleFsReady) {
            continue;
        }
        if (!processBackupHistory(brew_history::filePath(machine.serial),
                                  "history",
                                  machine.serial,
                                  true,
                                  emittedBytes,
                                  emitError) ||
            !processBackupHistory(stats_history::filePath(machine.serial),
                                  "stats_history",
                                  machine.serial,
                                  true,
                                  emittedBytes,
                                  emitError)) {
            break;
        }
    }

    if (!emitError.isEmpty()) {
        if (emitError != "backup client disconnected") {
            lastError = emitError;
        }
        addLog("backup", String("Backup stream aborted: ") + emitError);
        server.client().stop();
        return;
    }
    if (emittedBytes != bundleBytes) {
        lastError = "backup snapshot size changed after immutable preflight";
        addLog("backup", lastError.snapshot());
        server.client().stop();
        return;
    }

    server.sendContent("");
}

void handleBackupRestoreFinished() {
    const String uploadedFilename = backupRestoreUploadFilename;
    const size_t uploadedBytes = backupRestoreUploadBytes;
    const bool uploadComplete = backupRestoreUploadComplete;
    const String uploadError = backupRestoreUploadError;
    resetBackupRestoreUploadState(false);

    if (!littleFsReady) {
        lastError = "LittleFS is unavailable";
        sendError(503, lastError.snapshot());
        return;
    }
    if (!uploadError.isEmpty()) {
        lastError = uploadError;
        removeBackupRestoreUpload();
        const int status = uploadError.startsWith("backup upload exceeds") ? 413
            : (uploadError.startsWith("insufficient LittleFS") ? 507
               : (uploadError.indexOf("busy") >= 0 ||
                  uploadError.indexOf("still stopping") >= 0 ? 503 : 500));
        if (status == 503) {
            server.sendHeader("Retry-After", "1");
        }
        sendError(status, uploadError);
        return;
    }
    if (!uploadComplete || uploadedBytes == 0) {
        removeBackupRestoreUpload();
        sendError(400, "backup upload did not complete");
        return;
    }

    BackupBundleSummary summary;
    String error;
    if (!validateBackupBundle(uploadedBytes, summary, error)) {
        removeBackupRestoreUpload();
        sendError(400, error);
        return;
    }

    std::vector<String> replacedMachineSerials;
    replacedMachineSerials.reserve(savedMachines.size());
    for (const SavedMachine& machine : savedMachines) {
        replacedMachineSerials.push_back(machine.serial);
    }

    bool workerInFlight = false;
    String schedulerError;
    {
        MachineGenerationLock generationLock;
        if (!generationLock) {
            schedulerError = "machine generation registry is busy";
        } else if (jobMutex == nullptr ||
                   xSemaphoreTake(jobMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            schedulerError = "job registry is busy";
        } else {
            jobScheduler.cancelAll(millis());
            workerInFlight = !workerCurrentJobId.isEmpty();
            if (!workerInFlight) {
                jobScheduler.discardAll();
            }
            xSemaphoreGive(jobMutex);
        }
    }
    if (!schedulerError.isEmpty()) {
        removeBackupRestoreUpload();
        server.sendHeader("Retry-After", "1");
        sendError(503, schedulerError);
        return;
    }
    if (workerInFlight) {
        requestBleWorkerReset();
        removeBackupRestoreUpload();
        server.sendHeader("Retry-After", "1");
        sendError(503, "a BLE job is still stopping; retry the restore");
        return;
    }

    // These files are excluded from the backup and boot-scoped jobs are no
    // longer meaningful after replacement of the saved-machine registry.
    purgeEphemeralFilesForRestore();

    size_t appliedBudgetBytes = 0;
    size_t restoredMachineCount = 0;
    size_t restoredHistoryEntryCount = 0;
    size_t restoredStatsHistoryEntryCount = 0;
    if (!applyBackupBundle(uploadedBytes,
                           summary,
                           appliedBudgetBytes,
                           restoredMachineCount,
                           restoredHistoryEntryCount,
                           restoredStatsHistoryEntryCount,
                           error)) {
        lastError = error;
        removeBackupRestoreUpload();
        sendError(error.startsWith("insufficient LittleFS") ? 507 : 500, error);
        return;
    }

    for (const String& serial : replacedMachineSerials) {
        removeTargetResourceCacheFiles(serial);
    }
    if (cacheMutex != nullptr && xSemaphoreTake(cacheMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (resourceCaches) {
            for (size_t index = 0; index < RESOURCE_CACHE_CAPACITY; ++index) {
                resourceCaches[index] = {};
            }
        }
        xSemaphoreGive(cacheMutex);
    }

    removeBackupRestoreUpload();
    refreshCachedStorageTotals();

    DynamicJsonDocument response(8192);
    response["ok"] = true;
    response["backupSchema"] = BACKUP_BUNDLE_SCHEMA;
    response["filename"] = uploadedFilename;
    response["uploadBytes"] = uploadedBytes;
    response["requestedBudgetBytes"] = summary.requestedBudgetBytes;
    response["appliedBudgetBytes"] = appliedBudgetBytes;
    response["restoredMachineCount"] = restoredMachineCount;
    response["restoredHistoryEntryCount"] = restoredHistoryEntryCount;
    response["restoredStatsHistoryEntryCount"] = restoredStatsHistoryEntryCount;
    appendStatus(response);
    sendJson(response);
}

void handleBackupRestoreUpload() {
    history_storage::Guard filesystem;
    if (!filesystem) {
        backupRestoreUploadError = "filesystem is busy";
        return;
    }
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        resetBackupRestoreUploadState();
        backupRestoreUploadFilename = upload.filename;
        if (!backupRestoreUploadError.isEmpty()) return;
        if (!littleFsReady) {
            backupRestoreUploadError = "LittleFS is unavailable";
            return;
        }

        bool workerInFlight = false;
        if (jobMutex == nullptr ||
            xSemaphoreTake(jobMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            backupRestoreUploadError = "job registry is busy";
            return;
        }
        jobScheduler.cancelAll(millis());
        workerInFlight = !workerCurrentJobId.isEmpty();
        if (!workerInFlight) {
            jobScheduler.discardAll();
        }
        xSemaphoreGive(jobMutex);
        if (workerInFlight) {
            requestBleWorkerReset();
            backupRestoreUploadError =
                "a BLE job is still stopping; retry the restore upload";
            return;
        }

        // Restore explicitly invalidates boot-scoped jobs and derived caches.
        // Purging them before staging preserves enough room for the uploaded
        // bundle, rollback state, replacement histories, and headroom.
        purgeEphemeralFilesForRestore();
        addLog("backup", String("Starting backup restore upload: ") + upload.filename);
        return;
    }

    if (upload.status == UPLOAD_FILE_WRITE) {
        if (!backupRestoreUploadError.isEmpty()) {
            return;
        }
        if (upload.currentSize > MAX_BACKUP_RESTORE_UPLOAD_BYTES -
                std::min(backupRestoreUploadBytes, MAX_BACKUP_RESTORE_UPLOAD_BYTES)) {
            backupRestoreUploadError = String("backup upload exceeds ") +
                MAX_BACKUP_RESTORE_UPLOAD_BYTES + " bytes";
            backupRestoreUploadWriter.close();
            removeBackupRestoreUpload();
            return;
        }
        const size_t totalBytes = LittleFS.totalBytes();
        const size_t usedBytes = LittleFS.usedBytes();
        const size_t freeBytes = usedBytes <= totalBytes ? totalBytes - usedBytes : 0;
        constexpr size_t restoreStagingReserve =
            history_storage::OPERATIONAL_HEADROOM_BYTES + 24576 + 256;
        if (freeBytes < restoreStagingReserve ||
            upload.currentSize > freeBytes - restoreStagingReserve) {
            backupRestoreUploadError =
                "insufficient LittleFS headroom for transactional restore staging";
            backupRestoreUploadWriter.close();
            removeBackupRestoreUpload();
            return;
        }
        if (!backupRestoreUploadWriter.write(upload.buf, upload.currentSize)) {
            backupRestoreUploadError = "failed to write backup restore upload";
            backupRestoreUploadWriter.close();
            removeBackupRestoreUpload();
            return;
        }
        backupRestoreUploadBytes = backupRestoreUploadWriter.size();
        return;
    }

    if (upload.status == UPLOAD_FILE_END) {
        backupRestoreUploadWriter.close();
        if (!backupRestoreUploadError.isEmpty()) {
            return;
        }
        if (backupRestoreUploadBytes == 0) {
            backupRestoreUploadError = "backup upload is empty";
            removeBackupRestoreUpload();
            return;
        }
        backupRestoreUploadComplete = true;
        addLog("backup", String("Backup restore upload received, bytes=") + backupRestoreUploadBytes);
        return;
    }

    if (upload.status == UPLOAD_FILE_ABORTED) {
        backupRestoreUploadWriter.close();
        removeBackupRestoreUpload();
        backupRestoreUploadComplete = false;
        backupRestoreUploadError = "backup upload aborted";
        addLog("backup", "Backup restore upload aborted");
    }
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
    DynamicJsonDocument response(8192);
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
    DynamicJsonDocument response(8192);
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

    DynamicJsonDocument response(8192);
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

    DynamicJsonDocument response(8192);
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

    DynamicJsonDocument response(8192);
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
    bool frameSent = false;
    {
        WorkerMutationWriteScope mutationWrite;
        frameSent = runFrameScenario("send_frame",
                                     command,
                                     payload,
                                     sessionKey,
                                     encrypt,
                                     chunked,
                                     interChunkDelayMs,
                                     waitMs,
                                     notificationModeForRequest,
                                     result,
                                     error);
    }
    if (!frameSent) {
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

    if (SavedMachine* machine = !selectedMachineSerial.isEmpty() ? findSavedMachineBySerial(selectedMachineSerial)
                                                                 : findSavedMachineByAddress(selectedAddress);
        machine != nullptr) {
        updateSavedMachineFromCachedDetails(*machine);
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

    if (SavedMachine* machine = !selectedMachineSerial.isEmpty() ? findSavedMachineBySerial(selectedMachineSerial)
                                                                 : findSavedMachineByAddress(selectedAddress);
        machine != nullptr) {
        updateSavedMachineFromCachedDetails(*machine);
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
    if (!appendServiceList(services, includeDescriptors, error)) {
        lastError = error;
        response["ok"] = false;
        response["error"] = error;
        appendStatus(response);
        sendJson(response, 500);
        return;
    }
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
        sendError(isTransportReadFailure(error) ? 500 : 400, error);
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
        sendError(isTransportReadFailure(error) ? 500 : 400, error);
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
        bool didSubscribe = false;
        {
            WorkerBleCallScope bleCall;
            didSubscribe = characteristic->subscribe(useNotify, genericNotifyCallback, true);
        }
        if (!didSubscribe) {
            sendError(500, "failed to subscribe to characteristic");
            return;
        }
        subscribed = true;
    }

    std::vector<ByteVector> chunks;
    std::vector<uint32_t> times;
    const uint32_t startedAt = millis();
    bool characteristicWritten = false;
    {
        WorkerMutationWriteScope mutationWrite;
        characteristicWritten = writeRemoteCharacteristic(characteristic,
                                                           "gatt",
                                                           payload,
                                                           response,
                                                           chunked,
                                                           10,
                                                           interChunkDelayMs,
                                                           error);
    }
    if (!characteristicWritten) {
        if (subscribed) {
            WorkerBleCallScope bleCall;
            characteristic->unsubscribe(true);
        }
        lastError = error;
        sendError(500, error);
        return;
    }

    if (waitMs > 0) {
        waitForNotificationBatch(waitMs, chunks, times);
    } else {
        copyNotificationHistory(chunks, times);
    }

    if (subscribed) {
        WorkerBleCallScope bleCall;
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
    bool rawWriteCompleted = false;
    {
        WorkerMutationWriteScope mutationWrite;
        rawWriteCompleted = rawWriteNivona(
            alias, payload, response, waitMs, notifyBytes, error);
    }
    if (!rawWriteCompleted) {
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
    bridge_time::persist(millis(), addTimeLog);
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
        bridge_time::persist(millis(), addTimeLog);
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

uint32_t cacheTtlForResource(const String& resource) {
    if (resource == "summary") {
        return SUMMARY_CACHE_TTL_MS;
    }
    if (resource == "features") {
        return FEATURES_CACHE_TTL_MS;
    }
    return DEEP_CACHE_TTL_MS;
}

BleOperation operationForResource(const String& resource) {
    if (resource == "summary") {
        return BleOperation::MachineSummary;
    }
    if (resource == "stats") {
        return BleOperation::MachineStats;
    }
    if (resource == "settings") {
        return BleOperation::MachineSettings;
    }
    return BleOperation::MachineFeatures;
}

String resourceCachePath(const String& serial, const String& resource) {
    return String("/live-") + cacheSafeToken(serial) + "-" + cacheSafeToken(resource) + ".json";
}

ResourceCacheEntry* findResourceCacheLocked(const String& serial, const String& resource, bool create) {
    if (!resourceCaches) {
        return nullptr;
    }
    ResourceCacheEntry* freeEntry = nullptr;
    for (size_t index = 0; index < RESOURCE_CACHE_CAPACITY; ++index) {
        ResourceCacheEntry& entry = resourceCaches[index];
        if (entry.occupied && entry.serial.equalsIgnoreCase(serial) && entry.resource == resource) {
            return &entry;
        }
        if (!entry.occupied && freeEntry == nullptr) {
            freeEntry = &entry;
        }
    }
    if (!create || freeEntry == nullptr) {
        return nullptr;
    }
    *freeEntry = {};
    freeEntry->occupied = true;
    freeEntry->serial = serial;
    freeEntry->resource = resource;
    freeEntry->path = resourceCachePath(serial, resource);
    freeEntry->ttlMs = cacheTtlForResource(resource);
    return freeEntry;
}

bool copyResourceCache(const String& serial, const String& resource, ResourceCacheEntry& out) {
    if (cacheMutex == nullptr || xSemaphoreTake(cacheMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    ResourceCacheEntry* entry = findResourceCacheLocked(serial, resource, false);
    const bool found = entry != nullptr;
    if (found) {
        out = *entry;
    }
    xSemaphoreGive(cacheMutex);
    return found;
}

void updateCacheQueuedJob(const String& serial, const String& resource, const String& jobId) {
    if (cacheMutex == nullptr || xSemaphoreTake(cacheMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    ResourceCacheEntry* entry = findResourceCacheLocked(serial, resource, true);
    if (entry != nullptr) {
        entry->lastJobId = jobId;
    }
    xSemaphoreGive(cacheMutex);
}

void invalidateResourceCache(const String& serial, const String& resource) {
    WorkerMachineWriteGuard machineGuard;
    if (!machineGuard || cacheMutex == nullptr ||
        xSemaphoreTake(cacheMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    ResourceCacheEntry* entry = findResourceCacheLocked(serial, resource, false);
    if (entry != nullptr) {
        const uint32_t ttlMs = entry->ttlMs != 0 ? entry->ttlMs : cacheTtlForResource(resource);
        entry->sampledAtMs = millis() - ttlMs;
        entry->ttlMs = ttlMs;
        entry->retryAfterMs = 0;
        entry->lastJobId = "";
        entry->lastErrorJobId = "";
        entry->lastErrorCode = "invalidated_by_mutation";
        entry->lastErrorMessage = "resource changed and requires refresh";
    }
    xSemaphoreGive(cacheMutex);
}

void updateCacheFailure(const String& serial,
                        const String& resource,
                        const String& jobId,
                        const String& code,
                        const String& message) {
    WorkerMachineWriteGuard machineGuard;
    if (!machineGuard) {
        return;
    }
    if (cacheMutex == nullptr || xSemaphoreTake(cacheMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    ResourceCacheEntry* entry = findResourceCacheLocked(serial, resource, true);
    if (entry != nullptr) {
        entry->lastJobId = jobId;
        entry->lastErrorJobId = jobId;
        entry->lastErrorCode = code;
        entry->lastErrorMessage = message;
        entry->retryAfterMs = millis() + CACHE_FAILURE_BACKOFF_MS;
    }
    xSemaphoreGive(cacheMutex);
}

void reconcileTerminalResourceJob(const String& serial,
                                  const String& resource,
                                  ResourceCacheEntry& cache) {
    if (cache.lastJobId.isEmpty() || cache.lastErrorJobId == cache.lastJobId ||
        jobMutex == nullptr) {
        return;
    }

    bridge_jobs::Job job;
    bool terminalFailure = false;
    bool supersededByCombined = false;
    if (xSemaphoreTake(jobMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (jobScheduler.get(std::string(cache.lastJobId.c_str()), job)) {
            terminalFailure = job.state == bridge_jobs::State::Failed ||
                job.state == bridge_jobs::State::Cancelled;
            supersededByCombined = job.errorCode == "superseded" &&
                jobScheduler.hasActiveCombinedResource(
                    std::string(serial.c_str()), std::string(resource.c_str()));
        }
        xSemaphoreGive(jobMutex);
    }
    if (!terminalFailure || supersededByCombined) {
        return;
    }

    updateCacheFailure(serial,
                       resource,
                       cache.lastJobId,
                       job.errorCode.empty() ? String("refresh_failed")
                                             : String(job.errorCode.c_str()),
                       job.errorMessage.empty() ? String("resource refresh failed before execution")
                                                : String(job.errorMessage.c_str()));
    copyResourceCache(serial, resource, cache);
}

bool stageAtomicFileReplacement(const String& temporaryPath,
                                const String& finalPath,
                                String& backupPathOut,
                                bool& hadOriginalOut,
                                String& error) {
    history_storage::Guard filesystem(5000);
    if (!filesystem) {
        error = "filesystem is busy";
        return false;
    }
    File candidate = LittleFS.open(temporaryPath, "r");
    if (!candidate || candidate.size() <= 2 || candidate.size() > MAX_RESOURCE_CACHE_BYTES) {
        if (candidate) {
            candidate.close();
        }
        error = "job result file is empty or exceeds the resource cache limit";
        return false;
    }
    const size_t candidateSize = candidate.size();
    candidate.seek(0);
    const int first = candidate.read();
    candidate.seek(candidateSize - 1);
    int last = candidate.read();
    size_t tail = candidateSize;
    while (tail > 0 && (last == ' ' || last == '\n' || last == '\r' || last == '\t')) {
        tail--;
        if (tail == 0) {
            break;
        }
        candidate.seek(tail - 1);
        last = candidate.read();
    }
    candidate.close();
    if (first != '{' || last != '}') {
        error = "job result file is not a JSON object";
        return false;
    }

    backupPathOut = finalPath + ".bak";
    LittleFS.remove(backupPathOut);
    hadOriginalOut = LittleFS.exists(finalPath);
    if (hadOriginalOut && !LittleFS.rename(finalPath, backupPathOut)) {
        error = "failed to preserve previous cache";
        return false;
    }
    if (!LittleFS.rename(temporaryPath, finalPath)) {
        if (hadOriginalOut) {
            LittleFS.rename(backupPathOut, finalPath);
        }
        error = "failed to publish refreshed cache";
        return false;
    }
    return true;
}

void finishAtomicFileReplacement(const String& finalPath,
                                 const String& backupPath,
                                 bool hadOriginal,
                                 bool commit) {
    history_storage::Guard filesystem(5000);
    if (!filesystem) {
        return;
    }
    if (commit) {
        if (hadOriginal) {
            LittleFS.remove(backupPath);
        }
        return;
    }
    LittleFS.remove(finalPath);
    if (hadOriginal) {
        LittleFS.rename(backupPath, finalPath);
    }
}

bool publishResourceResult(const bridge_jobs::Job& job,
                           const String& resource,
                           const String& temporaryPath,
                           String& error) {
    WorkerMachineWriteGuard machineGuard;
    if (!machineGuard) {
        littleFsRemoveLocked(temporaryPath);
        error = "machine was deleted or replaced while the job was running";
        return false;
    }
    if (jobMutex == nullptr || xSemaphoreTake(jobMutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        error = "job registry is busy";
        return false;
    }
    bridge_jobs::Job current;
    const bool stillRunning = jobScheduler.get(job.id, current) && current.state == bridge_jobs::State::Running;
    if (!stillRunning) {
        xSemaphoreGive(jobMutex);
        littleFsRemoveLocked(temporaryPath);
        error = "job was cancelled";
        return false;
    }
    xSemaphoreGive(jobMutex);

    const String finalPath = resourceCachePath(String(job.target.c_str()), resource);
    String backupPath;
    bool hadOriginal = false;
    const bool replaced = stageAtomicFileReplacement(
        temporaryPath, finalPath, backupPath, hadOriginal, error);
    if (!replaced) {
        return false;
    }

    if (xSemaphoreTake(jobMutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        finishAtomicFileReplacement(finalPath, backupPath, hadOriginal, false);
        error = "job registry is busy";
        return false;
    }
    const bool stillRunningAfterWrite = jobScheduler.get(job.id, current) &&
        current.state == bridge_jobs::State::Running;
    if (!stillRunningAfterWrite) {
        xSemaphoreGive(jobMutex);
        finishAtomicFileReplacement(finalPath, backupPath, hadOriginal, false);
        error = "job was cancelled";
        return false;
    }

    bool cacheUpdated = false;
    if (cacheMutex != nullptr && xSemaphoreTake(cacheMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        ResourceCacheEntry* entry = findResourceCacheLocked(String(job.target.c_str()), resource, true);
        if (entry != nullptr) {
            entry->path = finalPath;
            entry->sampledAtMs = millis();
            entry->ttlMs = cacheTtlForResource(resource);
            entry->lastJobId = String(job.id.c_str());
            entry->lastErrorJobId = "";
            entry->lastErrorCode = "";
            entry->lastErrorMessage = "";
            entry->retryAfterMs = 0;
            cacheUpdated = true;
        }
        xSemaphoreGive(cacheMutex);
    }
    xSemaphoreGive(jobMutex);
    if (!cacheUpdated) {
        finishAtomicFileReplacement(finalPath, backupPath, hadOriginal, false);
        error = "resource cache registry is busy";
        return false;
    }
    finishAtomicFileReplacement(finalPath, backupPath, hadOriginal, true);
    return true;
}

String serializeMachineIdentity(const SavedMachine& machine) {
    String validationError;
    if (!validateSavedMachineDurableFields(machine, validationError)) {
        addLog("jobs", String("Refused invalid machine identity: ") + validationError);
        return "";
    }
    DynamicJsonDocument doc(3072);
    doc["serial"] = machine.serial;
    doc["alias"] = machine.alias;
    doc["address"] = machine.address;
    doc["addressType"] = machine.addressType;
    doc["manufacturer"] = machine.manufacturer;
    doc["model"] = machine.model;
    doc["modelCode"] = machine.modelCode;
    doc["modelName"] = machine.modelName;
    doc["familyKey"] = machine.familyKey;
    doc["hardwareRevision"] = machine.hardwareRevision;
    doc["firmwareRevision"] = machine.firmwareRevision;
    doc["softwareRevision"] = machine.softwareRevision;
    doc["ad06Hex"] = machine.ad06Hex;
    doc["ad06Ascii"] = machine.ad06Ascii;
    doc["lastSeenRssi"] = machine.lastSeenRssi;
    doc["lastSeenAtMs"] = machine.lastSeenAtMs;
    doc["savedAtMs"] = machine.savedAtMs;
    doc["generation"] = machine.generation;
    if (doc.overflowed()) {
        addLog("jobs", "Machine identity JSON capacity was exceeded");
        return "";
    }
    String result;
    if (serializeJson(doc, result) == 0) {
        return "";
    }
    return result;
}

bool deserializeMachineIdentity(const std::string& encoded, SavedMachine& machine) {
    if (encoded.empty()) {
        return false;
    }
    DynamicJsonDocument doc(3072);
    if (deserializeJson(doc, encoded.c_str())) {
        return false;
    }
    machine.serial = doc["serial"] | "";
    machine.alias = doc["alias"] | "";
    machine.address = doc["address"] | "";
    machine.addressType = doc["addressType"] | BLE_ADDR_PUBLIC;
    machine.manufacturer = doc["manufacturer"] | "";
    machine.model = doc["model"] | "";
    machine.modelCode = doc["modelCode"] | "";
    machine.modelName = doc["modelName"] | "";
    machine.familyKey = doc["familyKey"] | "";
    machine.hardwareRevision = doc["hardwareRevision"] | "";
    machine.firmwareRevision = doc["firmwareRevision"] | "";
    machine.softwareRevision = doc["softwareRevision"] | "";
    machine.ad06Hex = doc["ad06Hex"] | "";
    machine.ad06Ascii = doc["ad06Ascii"] | "";
    machine.lastSeenRssi = doc["lastSeenRssi"] | 0;
    machine.lastSeenAtMs = doc["lastSeenAtMs"] | 0;
    machine.savedAtMs = doc["savedAtMs"] | 0;
    machine.generation = doc["generation"] | 0;
    String validationError;
    return !machine.serial.isEmpty() && machine.generation != 0 &&
        validateSavedMachineDurableFields(machine, validationError);
}

void appendJobJson(JsonObject target, const bridge_jobs::Job& job) {
    target["id"] = job.id.c_str();
    target["state"] = bridge_jobs::Scheduler::stateName(job.state);
    target["kind"] = job.kind.c_str();
    target["target"] = job.target.c_str();
    target["pollAfterMs"] = JOB_POLL_AFTER_MS;
    target["submittedAtMs"] = job.submittedAtMs;
    if (job.startedAtMs != 0) {
        target["startedAtMs"] = job.startedAtMs;
    }
    if (job.finishedAtMs != 0) {
        target["finishedAtMs"] = job.finishedAtMs;
    }
    target["progress"] = job.progress;
    if (job.state == bridge_jobs::State::Succeeded) {
        const String resultUrl = !job.resultUrl.empty()
            ? String(job.resultUrl.c_str())
            : String("/api/jobs/") + job.id.c_str() + "/result";
        target["resultUrl"] = resultUrl;
    }
    if (!job.errorCode.empty() || !job.errorMessage.empty()) {
        JsonObject diagnostic = target.createNestedObject(
            job.state == bridge_jobs::State::Succeeded ? "warning" : "error");
        diagnostic["code"] = job.errorCode.c_str();
        diagnostic["message"] = job.errorMessage.c_str();
    }
}

void appendJobJson(JsonObject target, const bridge_jobs::PublicJob& job) {
    target["id"] = job.id.c_str();
    target["state"] = bridge_jobs::Scheduler::stateName(job.state);
    target["kind"] = job.kind.c_str();
    target["target"] = job.target.c_str();
    target["pollAfterMs"] = JOB_POLL_AFTER_MS;
    target["submittedAtMs"] = job.submittedAtMs;
    if (job.startedAtMs != 0) {
        target["startedAtMs"] = job.startedAtMs;
    }
    if (job.finishedAtMs != 0) {
        target["finishedAtMs"] = job.finishedAtMs;
    }
    target["progress"] = job.progress;
    if (job.state == bridge_jobs::State::Succeeded) {
        const String resultUrl = !job.resultUrl.empty()
            ? String(job.resultUrl.c_str())
            : String("/api/jobs/") + job.id.c_str() + "/result";
        target["resultUrl"] = resultUrl;
    }
    if (!job.errorCode.empty() || !job.errorMessage.empty()) {
        JsonObject diagnostic = target.createNestedObject(
            job.state == bridge_jobs::State::Succeeded ? "warning" : "error");
        diagnostic["code"] = job.errorCode.c_str();
        diagnostic["message"] = job.errorMessage.c_str();
    }
}

enum class JobLookupResult : uint8_t {
    Found,
    Missing,
    Busy,
};

JobLookupResult getJobSnapshot(const String& id, bridge_jobs::Job& out) {
    if (jobMutex == nullptr || xSemaphoreTake(jobMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return JobLookupResult::Busy;
    }
    jobScheduler.expire(millis());
    const bool found = jobScheduler.get(std::string(id.c_str()), out);
    xSemaphoreGive(jobMutex);
    return found ? JobLookupResult::Found : JobLookupResult::Missing;
}

void sendAcceptedJob(const bridge_jobs::Job& job) {
    const String location = String("/api/jobs/") + job.id.c_str();
    server.sendHeader("Location", location);
    server.sendHeader("Retry-After", "1");
    DynamicJsonDocument response(1536);
    response["ok"] = true;
    response["pending"] = true;
    JsonObject jobJson = response.createNestedObject("job");
    appendJobJson(jobJson, job);
    sendJson(response, 202);
}

void sendActiveJobConflict(const bridge_jobs::Job& job) {
    const String location = String("/api/jobs/") + job.id.c_str();
    server.sendHeader("Location", location);
    DynamicJsonDocument response(1536);
    response["ok"] = false;
    response["pending"] = true;
    response["code"] = "brew_job_active";
    response["error"] = "a brew request is already queued or running for this machine";
    JsonObject jobJson = response.createNestedObject("job");
    appendJobJson(jobJson, job);
    sendJson(response, 409);
}

bool submitJob(const bridge_jobs::Submission& submission,
               bridge_jobs::Job& jobOut,
               bool sendFailureResponse = true) {
    const String submissionTarget = submission.target.c_str();
    const bool savedMachineTarget = !submissionTarget.isEmpty() &&
        submissionTarget != "bridge" && !submissionTarget.startsWith("address:");
    if (savedMachineTarget && submission.identity.empty()) {
        if (sendFailureResponse) {
            sendError(500, "saved machine identity could not be serialized");
        }
        return false;
    }
    if (bleWorkerTaskHandle == nullptr) {
        if (sendFailureResponse) {
            server.sendHeader("Retry-After", "1");
            sendError(503, "BLE worker is unavailable");
        }
        return false;
    }
    if (jobMutex == nullptr || xSemaphoreTake(jobMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (sendFailureResponse) {
            server.sendHeader("Retry-After", "1");
            sendError(503, "job scheduler is unavailable");
        }
        return false;
    }
    const bridge_jobs::SubmitResult result = jobScheduler.submit(submission, millis());
    const bool conflict = result.status == bridge_jobs::SubmitStatus::Conflict &&
        jobScheduler.get(result.id, jobOut);
    const bool available =
        (result.status == bridge_jobs::SubmitStatus::Accepted ||
         result.status == bridge_jobs::SubmitStatus::Coalesced) &&
        jobScheduler.get(result.id, jobOut);
    xSemaphoreGive(jobMutex);
    if (conflict) {
        if (sendFailureResponse) {
            sendActiveJobConflict(jobOut);
        }
        return false;
    }
    if (!available) {
        if (sendFailureResponse) {
            server.sendHeader("Retry-After", "1");
            sendError(503, "BLE job queue is full");
        }
        return false;
    }
    if (bleWorkerTaskHandle != nullptr) {
        xTaskNotifyGive(bleWorkerTaskHandle);
    }
    return true;
}

void enqueueMachineOperation(const String& serial,
                             const String& kind,
                             BleOperation operation,
                             bridge_jobs::Priority priority,
                             uint32_t deadlineMs,
                             const String& resultUrl,
                             bool resource,
                             bool requireBody,
                             const String& coalesceKey,
                             const String& argument) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }

    String normalizedBody;
    if (requireBody || server.hasArg("plain")) {
        if (!server.hasArg("plain") || server.arg("plain").length() > 8192) {
            sendError(400, requireBody ? "missing or oversized request body" : "request body is too large");
            return;
        }
        DynamicJsonDocument requestDoc(8192);
        const DeserializationError parseError = deserializeJson(requestDoc, server.arg("plain"));
        if (parseError) {
            sendError(400, String("invalid json: ") + parseError.c_str());
            return;
        }
        serializeJson(requestDoc, normalizedBody);
    }

    bridge_jobs::Submission submission;
    const String canonicalSerial = machine->serial;
    submission.kind = kind.c_str();
    submission.target = canonicalSerial.c_str();
    submission.operationCode = static_cast<uint16_t>(operation);
    submission.argument = argument.c_str();
    submission.request = normalizedBody.c_str();
    submission.identity = serializeMachineIdentity(*machine).c_str();
    submission.priority = priority;
    submission.deadlineMs = deadlineMs;
    submission.resource = resource;
    submission.resultUrl = resultUrl.c_str();
    if (operation == BleOperation::MachineBrew) {
        submission.admissionKey = (String("brew:") + canonicalSerial).c_str();
    }
    if (!coalesceKey.isEmpty()) {
        submission.coalesceKey = coalesceKey.c_str();
    } else if (priority != bridge_jobs::Priority::Mutation) {
        submission.coalesceKey = defaultJobCoalesceKey(
            kind, canonicalSerial, operation, argument, normalizedBody).c_str();
    }

    bridge_jobs::Job job;
    if (!submitJob(submission, job)) {
        return;
    }
    if (resource) {
        updateCacheQueuedJob(serial, kind.startsWith("machine_") ? kind.substring(8) : kind, String(job.id.c_str()));
    }
    sendAcceptedJob(job);
}

bool streamCachedResource(const ResourceCacheEntry& cache,
                          bool stale,
                          const bridge_jobs::Job* refreshJob = nullptr,
                          bool* busyOut = nullptr) {
    if (busyOut != nullptr) {
        *busyOut = false;
    }
    String payload;
    {
        history_storage::Guard filesystem(100);
        if (!filesystem) {
            if (busyOut != nullptr) {
                *busyOut = true;
            }
            return false;
        }
        File file = LittleFS.open(cache.path, "r");
        if (!file || file.size() <= 2 || file.size() > MAX_RESOURCE_CACHE_BYTES) {
            if (file) {
                file.close();
            }
            return false;
        }
        payload.reserve(file.size() + 2048);
        uint8_t buffer[512];
        while (file.available()) {
            const size_t read = file.read(buffer, sizeof(buffer));
            if (read == 0 || !payload.concat(reinterpret_cast<const char*>(buffer), read)) {
                file.close();
                return false;
            }
        }
        file.close();
    }

    bridge_json::ObjectExtent resourceObject;
    if (!bridge_json::inspectObject(payload.c_str(), payload.length(), resourceObject) ||
        !resourceObject.hasMembers) {
        return false;
    }

    DynamicJsonDocument metadata(2048);
    JsonObject cacheJson = metadata.createNestedObject("cache");
    cacheJson["state"] = stale ? "stale" : "fresh";
    cacheJson["sampledAtMs"] = cache.sampledAtMs;
    cacheJson["ageMs"] = static_cast<uint32_t>(millis() - cache.sampledAtMs);
    cacheJson["ttlMs"] = cache.ttlMs;
    cacheJson["refreshing"] = refreshJob != nullptr;
    if (!cache.lastErrorCode.isEmpty() || !cache.lastErrorMessage.isEmpty()) {
        JsonObject cacheError = cacheJson.createNestedObject("error");
        cacheError["code"] = cache.lastErrorCode;
        cacheError["message"] = cache.lastErrorMessage;
    }
    if (refreshJob != nullptr) {
        JsonObject jobJson = metadata.createNestedObject("job");
        appendJobJson(jobJson, *refreshJob);
    }
    String suffix;
    serializeJson(metadata, suffix);
    suffix.remove(0, 1);
    payload.remove(static_cast<unsigned>(resourceObject.closingBrace));
    payload += ',';
    payload += suffix;
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", payload);
    return true;
}

void handleMachineResourceRequest(const String& serial, const String& resource) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }
    const String canonicalSerial = machine->serial;
    const bool forced = parseRefreshArg();
    ResourceCacheEntry cache;
    const bool hasMetadata = copyResourceCache(canonicalSerial, resource, cache);
    if (hasMetadata) {
        reconcileTerminalResourceJob(canonicalSerial, resource, cache);
    }
    bool hasPayload = false;
    if (hasMetadata && cache.sampledAtMs != 0 && littleFsReady) {
        history_storage::Guard filesystem(100);
        if (!filesystem) {
            server.sendHeader("Retry-After", "1");
            sendError(503, "filesystem is busy");
            return;
        }
        File cached = LittleFS.open(cache.path, "r");
        hasPayload = cached && cached.size() > 2;
        if (cached) {
            cached.close();
        }
    }
    const uint32_t nowMs = millis();
    const bridge_runtime_policy::CacheDecision cacheDecision =
        bridge_runtime_policy::decideCacheRequest(
            {hasPayload, cache.sampledAtMs, cache.ttlMs, cache.retryAfterMs},
            nowMs,
            forced);

    if (cacheDecision.servePayload && !cacheDecision.stale) {
        bool filesystemBusy = false;
        if (!streamCachedResource(cache, false, nullptr, &filesystemBusy)) {
            if (filesystemBusy) {
                server.sendHeader("Retry-After", "1");
            }
            sendError(filesystemBusy ? 503 : 500,
                      filesystemBusy ? String("filesystem is busy") : String("cached resource is invalid"));
        }
        return;
    }

    bridge_jobs::Job refreshJob;
    bool enqueued = false;
    if (cacheDecision.rejectColdForBackoff) {
        server.sendHeader("Retry-After", "1");
        sendError(503,
                  cache.lastErrorMessage.isEmpty()
                      ? String("resource refresh is backing off after a failure")
                      : cache.lastErrorMessage);
        return;
    }
    if (cacheDecision.enqueueRefresh) {
        bridge_jobs::Submission submission;
        submission.kind = (String("machine_") + resource).c_str();
        submission.target = canonicalSerial.c_str();
        const BleOperation operation = operationForResource(resource);
        submission.operationCode = static_cast<uint16_t>(operation);
        submission.identity = serializeMachineIdentity(*machine).c_str();
        submission.priority = forced || !hasPayload ? bridge_jobs::Priority::ForcedRead
                                                    : bridge_jobs::Priority::StaleRefresh;
        const uint32_t resourceDeadlineMs =
            resource == "summary" || resource == "features" ? 12000 : 15000;
        bool followsCombinedRefresh = false;
        if (jobMutex != nullptr && xSemaphoreTake(jobMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            followsCombinedRefresh = jobScheduler.hasActiveCombinedResource(
                std::string(canonicalSerial.c_str()), std::string(resource.c_str()));
            xSemaphoreGive(jobMutex);
        }
        // A component request admitted behind a combined refresh is a cache
        // barrier, not duplicate BLE work. Give its queue wait enough room for
        // the 60-second parent while preserving the component's own 12/15s
        // execution deadline once the worker starts it.
        submission.deadlineMs = followsCombinedRefresh
            ? 60000U + resourceDeadlineMs
            : resourceDeadlineMs;
        submission.executionDeadlineMs = followsCombinedRefresh ? resourceDeadlineMs : 0;
        submission.resource = true;
        submission.resultUrl = (String("/api/machines/") + canonicalSerial + "/" + resource).c_str();
        submission.coalesceKey = defaultJobCoalesceKey(
            String("machine_") + resource, canonicalSerial, operation, "", "").c_str();
        enqueued = submitJob(submission, refreshJob, forced || !hasPayload);
        if (enqueued) {
            updateCacheQueuedJob(canonicalSerial, resource, String(refreshJob.id.c_str()));
        }
    }

    if (cacheDecision.servePayload) {
        bool filesystemBusy = false;
        if (!streamCachedResource(cache,
                                  true,
                                  enqueued ? &refreshJob : nullptr,
                                  &filesystemBusy)) {
            if (filesystemBusy) {
                server.sendHeader("Retry-After", "1");
            }
            sendError(filesystemBusy ? 503 : 500,
                      filesystemBusy ? String("filesystem is busy") : String("cached resource is invalid"));
        }
        return;
    }
    if (enqueued) {
        sendAcceptedJob(refreshJob);
    }
}

void handleMachineRefreshRequest(const String& serial) {
    SavedMachine* machine = findSavedMachineBySerial(serial);
    if (machine == nullptr) {
        sendError(404, "saved machine not found");
        return;
    }
    DynamicJsonDocument request(1024);
    String error;
    if (!parseJsonBody(request, error)) {
        sendError(400, error);
        return;
    }
    JsonArrayConst resources = request["resources"].as<JsonArrayConst>();
    if (resources.isNull() || resources.size() == 0 || resources.size() > 4) {
        sendError(400, "resources must contain one to four resource names");
        return;
    }
    const char* resourceOrder[CACHEABLE_RESOURCE_COUNT] = {
        "summary", "stats", "settings", "features"
    };
    bool requested[CACHEABLE_RESOURCE_COUNT]{};
    for (JsonVariantConst value : resources) {
        const String resource = value.as<String>();
        bool recognized = false;
        for (size_t index = 0; index < CACHEABLE_RESOURCE_COUNT; ++index) {
            if (resource == resourceOrder[index]) {
                requested[index] = true;
                recognized = true;
                break;
            }
        }
        if (!recognized) {
            sendError(400, String("unsupported refresh resource: ") + resource);
            return;
        }
    }
    DynamicJsonDocument normalizedRequest(512);
    JsonArray normalizedResources = normalizedRequest.createNestedArray("resources");
    for (size_t index = 0; index < CACHEABLE_RESOURCE_COUNT; ++index) {
        if (requested[index]) {
            normalizedResources.add(resourceOrder[index]);
        }
    }
    String normalized;
    serializeJson(normalizedRequest, normalized);
    const String canonicalSerial = machine->serial;
    bridge_jobs::Submission submission;
    submission.kind = "machine_refresh";
    submission.target = canonicalSerial.c_str();
    submission.operationCode = static_cast<uint16_t>(BleOperation::MachineRefresh);
    submission.request = normalized.c_str();
    submission.identity = serializeMachineIdentity(*machine).c_str();
    submission.priority = bridge_jobs::Priority::ForcedRead;
    submission.deadlineMs = 60000;
    submission.coalesceKey = defaultJobCoalesceKey(
        "machine_refresh", canonicalSerial, BleOperation::MachineRefresh, "", normalized).c_str();
    bridge_jobs::Job job;
    if (submitJob(submission, job)) {
        sendAcceptedJob(job);
    }
}

bool cancelTargetJobsAndCacheMetadata(const String& serial) {
    if (jobMutex == nullptr || xSemaphoreTake(jobMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    String exceptJobId;
    WorkerExecutionContext* execution = currentWorkerExecution();
    if (execution != nullptr && execution->target.equalsIgnoreCase(serial)) {
        exceptJobId = execution->jobId;
    }
    jobScheduler.cancelTarget(
        std::string(serial.c_str()), millis(), std::string(exceptJobId.c_str()));
    xSemaphoreGive(jobMutex);
    if (cacheMutex == nullptr || xSemaphoreTake(cacheMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    if (!resourceCaches) {
        xSemaphoreGive(cacheMutex);
        return false;
    }
    for (size_t cacheIndex = 0; cacheIndex < RESOURCE_CACHE_CAPACITY; ++cacheIndex) {
        ResourceCacheEntry& entry = resourceCaches[cacheIndex];
        if (!entry.occupied || !entry.serial.equalsIgnoreCase(serial)) {
            continue;
        }
        entry = {};
    }
    xSemaphoreGive(cacheMutex);
    return true;
}

void removeTargetResourceCacheFiles(const String& serial) {
    const char* resourceNames[CACHEABLE_RESOURCE_COUNT] = {
        "summary", "stats", "settings", "features"
    };
    for (size_t index = 0; index < CACHEABLE_RESOURCE_COUNT; ++index) {
        const String path = resourceCachePath(serial, resourceNames[index]);
        littleFsRemoveLocked(path);
        littleFsRemoveLocked(path + ".bak");
    }
}

bool streamJsonFileResponse(const String& path,
                            size_t maximumBytes,
                            int status,
                            bool& responseStarted,
                            String& error) {
    responseStarted = false;
    error = "";
    size_t resultBytes = 0;
    {
        history_storage::Guard filesystem(1000);
        if (!filesystem) {
            error = "filesystem is busy";
            return false;
        }
        File file = LittleFS.open(path, "r");
        if (!file) {
            error = "job result expired";
            return false;
        }
        resultBytes = file.size();
        if (resultBytes <= 2 || resultBytes > maximumBytes) {
            file.close();
            error = "JSON file is invalid or exceeds its bounded size";
            return false;
        }
        const int first = file.read();
        file.seek(resultBytes - 1);
        const int last = file.read();
        file.close();
        if (first != '{' || last != '}') {
            error = "JSON file is not a complete object";
            return false;
        }
    }

    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.setContentLength(resultBytes);
    server.send(status, "application/json", "");
    responseStarted = true;

    size_t offset = 0;
    uint8_t buffer[1024];
    while (offset < resultBytes) {
        size_t readCount = 0;
        String readError;
        {
            history_storage::Guard filesystem(1000);
            if (!filesystem) {
                readError = "filesystem became busy while streaming the job result";
            } else {
                File file = LittleFS.open(path, "r");
                if (!file || file.size() != resultBytes || !file.seek(offset)) {
                    readError = "JSON file changed while it was being streamed";
                } else {
                    readCount = file.read(buffer, std::min(sizeof(buffer), resultBytes - offset));
                }
                if (file) {
                    file.close();
                }
            }
        }
        if (!readError.isEmpty() || readCount == 0) {
            error = !readError.isEmpty() ? readError : String("failed to read the stored JSON file");
            server.client().stop();
            return false;
        }
        if (!server.sendContent(reinterpret_cast<const char*>(buffer), readCount)) {
            error = "JSON client disconnected while streaming the stored file";
            return false;
        }
        offset += readCount;
    }
    return true;
}

bool streamStoredJobResult(const String& path,
                           int status,
                           bool& responseStarted,
                           String& error) {
    return streamJsonFileResponse(
        path, MAX_JOB_RESULT_BYTES, status, responseStarted, error);
}

bool beginJobResultStream(const String& id, const String& path) {
    if (jobMutex == nullptr || xSemaphoreTake(jobMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    bridge_jobs::Job current;
    const bool available = activeResultStreamPath.isEmpty() &&
        jobScheduler.get(std::string(id.c_str()), current) &&
        current.state == bridge_jobs::State::Succeeded &&
        current.resultPath == std::string(path.c_str());
    if (available) {
        activeResultStreamPath = path;
    }
    xSemaphoreGive(jobMutex);
    return available;
}

void endJobResultStream(const String& id, const String& path) {
    bool recordStillExists = true;
    if (jobMutex != nullptr) {
        while (xSemaphoreTake(jobMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            delay(1);
        }
        bridge_jobs::Job current;
        recordStillExists = jobScheduler.get(std::string(id.c_str()), current);
        if (activeResultStreamPath == path) {
            activeResultStreamPath = "";
        }
        xSemaphoreGive(jobMutex);
    }
    if (!recordStillExists) {
        littleFsRemoveLocked(path);
    }
}

void handleJobApiRoute(const String& id, bool resultRequested) {
    bridge_jobs::Job job;
    const JobLookupResult lookup = getJobSnapshot(id, job);
    if (lookup == JobLookupResult::Busy) {
        server.sendHeader("Retry-After", "1");
        sendError(503, "job registry is busy");
        return;
    }
    if (lookup == JobLookupResult::Missing) {
        sendError(404, "job not found or expired");
        return;
    }
    if (!resultRequested) {
        DynamicJsonDocument response(2048);
        response["ok"] = job.state != bridge_jobs::State::Failed &&
            job.state != bridge_jobs::State::Cancelled;
        response["pending"] = job.state == bridge_jobs::State::Queued || job.state == bridge_jobs::State::Running;
        JsonObject jobJson = response.createNestedObject("job");
        appendJobJson(jobJson, job);
        sendJson(response);
        return;
    }
    if (job.state != bridge_jobs::State::Succeeded) {
        sendError(job.state == bridge_jobs::State::Failed ? 409 : 425, "job result is not available");
        return;
    }
    if (!job.resultBody.empty()) {
        server.sendHeader("Cache-Control", "no-store");
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(job.resultStatus, "application/json", job.resultBody.c_str());
        return;
    }
    if (!job.resultPath.empty()) {
        const String resultPath = job.resultPath.c_str();
        if (!beginJobResultStream(id, resultPath)) {
            sendError(404, "job result expired");
            return;
        }
        bool responseStarted = false;
        String streamError;
        const bool streamed = streamStoredJobResult(resultPath,
                                   job.resultStatus,
                                   responseStarted,
                                   streamError);
        endJobResultStream(id, resultPath);
        if (!streamed && !responseStarted) {
            const int errorStatus = streamError == "job result expired" ? 404 : 503;
            if (errorStatus == 503) {
                server.sendHeader("Retry-After", "1");
            }
            sendError(errorStatus, streamError);
        }
        return;
    }
    sendError(404, "job has no direct result");
}

void restartAfterStuckBleCleanup(const String& reason) {
    addLog("ble", String("Restarting after unsafe BLE cleanup state: ") + reason);
    rtcWatchdogMarker.magic = RTC_WATCHDOG_MAGIC;
    rtcWatchdogMarker.atMs = millis();
    rtcWatchdogMarker.jobId[0] = '\0';
    rtcWatchdogMarker.kind[0] = '\0';
    rtcWatchdogMarker.target[0] = '\0';
    if (jobMutex != nullptr && xSemaphoreTake(jobMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strlcpy(rtcWatchdogMarker.jobId,
                workerCurrentJobId.c_str(),
                sizeof(rtcWatchdogMarker.jobId));
        strlcpy(rtcWatchdogMarker.kind,
                workerCurrentJobKind.c_str(),
                sizeof(rtcWatchdogMarker.kind));
        strlcpy(rtcWatchdogMarker.target,
                workerCurrentJobTarget.c_str(),
                sizeof(rtcWatchdogMarker.target));
        xSemaphoreGive(jobMutex);
    }
    delay(20);
    ESP.restart();
}

void resetBleClientAfterFailure() {
    if (client != nullptr) {
        // Never ask NimBLE to delete a client whose asynchronous termination
        // is still pending. The callback owns the transition to disconnected;
        // only then is immediate deletion safe.
        String disconnectError;
        if (!disconnectClientAndWait(disconnectError, 3000, true)) {
            restartAfterStuckBleCleanup(disconnectError);
            return;
        }
        NimBLEClient* clientToDelete = client;
        clearRemoteHandles();
        clearStoredSessionKey();
        cachedDetails = DeviceDetails{};
        bool deletionAccepted = false;
        {
            WorkerBleCallScope deleteCall;
            deletionAccepted = NimBLEDevice::deleteClient(clientToDelete);
        }
        if (!deletionAccepted) {
            restartAfterStuckBleCleanup("NimBLE rejected disconnected client deletion");
            return;
        }
        client = nullptr;
    }
    clientDisconnectPending.store(false, std::memory_order_release);
    clientDisconnectedEvent.store(false, std::memory_order_release);
    clearRemoteHandles();
    clearStoredSessionKey();
    cachedDetails = DeviceDetails{};
    lastBleActivityAtMs = millis();
}

void requestBleWorkerReset() {
    workerResetRequested.store(true, std::memory_order_release);
    if (bleWorkerTaskHandle != nullptr) {
        xTaskNotifyGive(bleWorkerTaskHandle);
    }
}

void invokeQueuedHandler(WorkerExecutionContext& context) {
    context.forceRefresh = true;
    if (context.machineLoaded) {
        selectMachineTarget(context.machine);
    } else if (!context.targetAddress.isEmpty()) {
        selectAddressTarget(context.targetAddress, context.targetAddressType);
    }
    switch (context.operation) {
        case BleOperation::MachineSummary: handleMachineSummary(context.target); break;
        case BleOperation::MachineStats: handleMachineStats(context.target); break;
        case BleOperation::MachineSettings: handleMachineSettingsGet(context.target); break;
        case BleOperation::MachineFeatures: handleMachineFeaturesGet(context.target); break;
        case BleOperation::MachineRecipesRefresh: handleMachineRecipesRefresh(context.target); break;
        case BleOperation::MachineRecipeDetail: handleMachineRecipeDetail(context.target, context.argument); break;
        case BleOperation::MachineBrew: handleMachineBrew(context.target); break;
        case BleOperation::MachineConfirm: handleMachineConfirm(context.target); break;
        case BleOperation::MachineMyCoffeeList: handleMachineMyCoffeeList(context.target); break;
        case BleOperation::MachineMyCoffeeDetail:
            handleMachineMyCoffeeDetail(context.target, context.argument, false);
            break;
        case BleOperation::MachineMyCoffeeUpdate:
            handleMachineMyCoffeeDetail(context.target, context.argument, true);
            break;
        case BleOperation::MachineSettingsPost: handleMachineSettingsPost(context.target); break;
        case BleOperation::Scan: handleScan(); break;
        case BleOperation::MachineProbe: handleMachineProbe(); break;
        case BleOperation::MachinesCreate: handleMachinesCreate(); break;
        case BleOperation::Connect: handleConnect(); break;
        case BleOperation::Disconnect: handleDisconnect(); break;
        case BleOperation::Pair: handlePair(); break;
        case BleOperation::Details: handleDetails(); break;
        case BleOperation::Notifications: handleNotifications(); break;
        case BleOperation::ProtocolSession: handleSession(); break;
        case BleOperation::ProtocolHu: handleHu(); break;
        case BleOperation::ProtocolSendFrame: handleSendFrame(); break;
        case BleOperation::ProtocolAppProbe: handleAppProbe(); break;
        case BleOperation::ProtocolVerify: handleVerify(); break;
        case BleOperation::ProtocolStatsProbe: handleStatsProbe(); break;
        case BleOperation::ProtocolSettingsProbe: handleSettingsProbe(); break;
        case BleOperation::ProtocolWorkerProbe: handleWorkerProbe(); break;
        case BleOperation::GattServices: handleGattServices(); break;
        case BleOperation::GattRead: handleGattRead(); break;
        case BleOperation::GattWrite: handleGattWrite(); break;
        case BleOperation::ProtocolRawRead: handleRawRead(); break;
        case BleOperation::ProtocolRawWrite: handleRawWrite(); break;
        case BleOperation::MachineRefresh:
            sendError(500, "combined refresh was dispatched incorrectly");
            break;
    }
}

bool runCombinedRefresh(const bridge_jobs::Job& job, WorkerExecutionContext& context) {
    const String combinedResultPath = context.resultPath;
    DynamicJsonDocument request(1024);
    if (deserializeJson(request, job.request.c_str())) {
        context.errorCode = "invalid_request";
        context.errorMessage = "stored refresh request is invalid";
        context.responseStatus = 400;
        return false;
    }
    JsonArrayConst resources = request["resources"].as<JsonArrayConst>();
    size_t resourceIndex = 0;
    for (JsonVariantConst value : resources) {
        if (workerDeadlineExceeded() || !workerTargetStillCurrent()) {
            context.responseStatus = workerDeadlineExceeded() ? 504 : 409;
            context.errorCode = workerDeadlineExceeded() ? "deadline_exceeded" : "target_changed";
            context.errorMessage = workerDeadlineExceeded()
                ? "combined refresh exceeded its logical deadline"
                : "machine was deleted or replaced while the refresh was running";
            return false;
        }
        const String resource = value.as<String>();
        ResourceCacheEntry existingCache;
        if (copyResourceCache(context.target, resource, existingCache) &&
            existingCache.sampledAtMs != 0 &&
            bridge_runtime_policy::deadlineReached(existingCache.sampledAtMs, job.submittedAtMs)) {
            ++resourceIndex;
            noteWorkerProgress(static_cast<uint8_t>(5 +
                (resourceIndex * 90) / resources.size()));
            continue;
        }
        context.operation = operationForResource(resource);
        context.resource = true;
        context.responded = false;
        context.responseStatus = 500;
        context.errorCode = "";
        context.errorMessage = "";
        context.resultPath = String("/cache-") + job.id.c_str() + "-" + resource + ".tmp";
        invokeQueuedHandler(context);
        if (!context.responded || context.responseStatus >= 400 || workerDeadlineExceeded() ||
            !workerTargetStillCurrent()) {
            if (workerDeadlineExceeded()) {
                context.responseStatus = 504;
                context.errorCode = "deadline_exceeded";
                context.errorMessage = "combined refresh exceeded its logical deadline";
            } else if (!workerTargetStillCurrent()) {
                context.responseStatus = 409;
                context.errorCode = "target_changed";
                context.errorMessage = "machine was deleted or replaced while the refresh was running";
            }
            littleFsRemoveLocked(context.resultPath);
            updateCacheFailure(context.target,
                               resource,
                               context.jobId,
                               context.errorCode.isEmpty() ? "refresh_failed" : context.errorCode,
                               context.errorMessage);
            return false;
        }
        String publishError;
        if (!publishResourceResult(job, resource, context.resultPath, publishError)) {
            context.responseStatus = 500;
            context.errorCode = "cache_publish_failed";
            context.errorMessage = publishError;
            return false;
        }
        ++resourceIndex;
        noteWorkerProgress(static_cast<uint8_t>(5 +
            (resourceIndex * 90) / resources.size()));
    }
    context.resource = false;
    context.resultPath = combinedResultPath;
    context.responded = false;
    DynamicJsonDocument response(1024);
    response["ok"] = true;
    response["refreshed"] = resources;
    sendJson(response);
    return true;
}

void copyHealthText(char* destination, size_t capacity, const String& source) {
    if (capacity == 0) {
        return;
    }
    strlcpy(destination, source.c_str(), capacity);
}

void updateWorkerOwnedHealth() {
    size_t deviceCount = 0;
    size_t supportedDeviceCount = 0;
    if (scanDataMutex != nullptr && xSemaphoreTake(scanDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        deviceCount = scannedDevices.size();
        for (const auto& record : scannedDevices) {
            if (record.likelySupported) {
                supportedDeviceCount++;
            }
        }
        xSemaphoreGive(scanDataMutex);
    }
    if (healthMutex == nullptr || xSemaphoreTake(healthMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    bridgeHealth.workerReady = true;
    bridgeHealth.workerBusy = workerJobActive.load(std::memory_order_acquire);
    bridgeHealth.clientCreated = client != nullptr;
    bridgeHealth.clientConnected = client != nullptr && client->isConnected();
    bridgeHealth.scanInProgress = idleScanInProgress || blockingScanInProgress;
    bridgeHealth.deviceCount = deviceCount;
    bridgeHealth.supportedDeviceCount = supportedDeviceCount;
    bridgeHealth.protocolSessionCount = protocolSessions.size();
    bridgeHealth.lastScanReason = lastScanReason;
    bridgeHealth.lastScanResultCount = lastScanResultCount;
    bridgeHealth.lastScanAtMs = lastScanAtMs;
    copyHealthText(bridgeHealth.selectedAddress, sizeof(bridgeHealth.selectedAddress), selectedAddress);
    bridgeHealth.selectedAddressType = selectedAddressType;
    copyHealthText(bridgeHealth.notificationMode,
                   sizeof(bridgeHealth.notificationMode),
                   notificationsEnabled ? notificationMode : String("off"));
    copyHealthText(bridgeHealth.pairingStatus,
                   sizeof(bridgeHealth.pairingStatus),
                   pairingStatus.snapshot());
    copyHealthText(bridgeHealth.lastError, sizeof(bridgeHealth.lastError), lastError.snapshot());
    if (bridgeHealth.clientConnected) {
        copyHealthText(bridgeHealth.peerAddress,
                       sizeof(bridgeHealth.peerAddress),
                       String(client->getPeerAddress().toString().c_str()));
    } else {
        bridgeHealth.peerAddress[0] = '\0';
    }
    // ESP-IDF reports this value in bytes (unlike upstream FreeRTOS ports that
    // traditionally report StackType_t words).
    bridgeHealth.workerStackHighWaterMark = uxTaskGetStackHighWaterMark(nullptr);
    xSemaphoreGive(healthMutex);
}

bool shouldSpoolJobResult(BleOperation operation) {
    (void)operation;
    // Keep terminal metadata compact and avoid retaining response-sized String
    // allocations for five minutes. Every mutation/diagnostic result is served
    // from its bounded LittleFS spool file.
    return true;
}

void bleWorkerTask(void*) {
    String initializeError;
    if (!initializeBleStack(initializeError)) {
        addLog("worker", String("BLE initialization failed: ") + initializeError);
    }
    updateWorkerOwnedHealth();

    for (;;) {
        applyClientDisconnectedEvent();
        if (workerResetRequested.exchange(false, std::memory_order_acq_rel)) {
            resetBleClientAfterFailure();
            protocolSessions.clear();
            selectedMachineSerial = "";
            selectedAddress = "";
            selectedAddressType = BLE_ADDR_PUBLIC;
        }
        bridge_jobs::Job job;
        bool haveJob = false;
        std::string discardedPath;
        if (jobMutex != nullptr && xSemaphoreTake(jobMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            jobScheduler.expire(millis());
            haveJob = jobScheduler.startNext(millis(), job);
            if (haveJob) {
                workerCurrentJobId = job.id.c_str();
                workerCurrentJobKind = job.kind.c_str();
                workerCurrentJobTarget = job.target.c_str();
            }
            jobScheduler.popDiscardedResultPath(discardedPath);
            xSemaphoreGive(jobMutex);
        }
        if (!discardedPath.empty() && littleFsReady) {
            bool streamActive = false;
            if (jobMutex != nullptr) {
                while (xSemaphoreTake(jobMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
                    vTaskDelay(1);
                }
                streamActive = activeResultStreamPath == discardedPath.c_str();
                xSemaphoreGive(jobMutex);
            }
            if (!streamActive) {
                littleFsRemoveLocked(discardedPath.c_str());
            }
        }
        if (!haveJob) {
            if (client != nullptr && client->isConnected() &&
                static_cast<uint32_t>(millis() - lastBleActivityAtMs) >= BLE_IDLE_DISCONNECT_MS) {
                String disconnectError;
                if (!disconnectClientAndWait(disconnectError, 1500, true)) {
                    addLog("ble", String("Idle disconnect failed: ") + disconnectError);
                    resetBleClientAfterFailure();
                }
                clearRemoteHandles();
                clearStoredSessionKey();
                lastBleActivityAtMs = millis();
            }
            updateWorkerOwnedHealth();
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
            continue;
        }

        WorkerExecutionContext context;
        context.active = true;
        context.jobId = job.id.c_str();
        context.target = job.target.c_str();
        context.operation = static_cast<BleOperation>(job.operationCode);
        context.argument = job.argument.c_str();
        context.requestBody = job.request.c_str();
        context.targetAddress = job.targetAddress.c_str();
        context.targetAddressType = job.targetAddressType;
        context.deadlineAtMs = job.executionDeadlineMs != 0
            ? millis() + job.executionDeadlineMs
            : job.deadlineAtMs;
        context.resource = job.resource;
        context.mutationJob = job.priority == bridge_jobs::Priority::Mutation;
        context.spoolResult = !job.resource && shouldSpoolJobResult(context.operation);
        context.resultPath = job.resource
            ? String("/cache-") + job.id.c_str() + ".tmp"
            : (context.spoolResult ? String("/job-") + job.id.c_str() + ".json" : String(""));
        context.machineLoaded = deserializeMachineIdentity(job.identity, context.machine);
        context.backgroundJob = job.priority == bridge_jobs::Priority::Background;

        workerExecution.store(&context, std::memory_order_release);
        workerJobActive.store(true, std::memory_order_release);
        workerBleCallDepth.store(0, std::memory_order_release);
        workerBleCallActive.store(false, std::memory_order_release);
        workerMaintenanceCallActive.store(false, std::memory_order_release);
        workerCurrentStartedAtMs.store(millis(), std::memory_order_release);
        workerCurrentDeadlineAtMs.store(context.deadlineAtMs, std::memory_order_release);
        workerHeartbeatAtMs.store(millis(), std::memory_order_release);
        workerCurrentProgress.store(1, std::memory_order_release);
        updateWorkerOwnedHealth();
        noteWorkerProgress(5);

        bool operationOk = false;
        bool resetClientAfterCompletion = false;
        bool resourceSatisfiedByNewerCache = false;
        if (context.machineLoaded && !workerTargetStillCurrent()) {
            context.responded = true;
            context.responseStatus = 409;
            context.errorCode = "target_changed";
            context.errorMessage = "machine was deleted or replaced before the job started";
        } else if (context.mutationJob) {
            String preflightError;
            if (!prepareMutationResultStorage(context, preflightError)) {
                context.responded = true;
                context.responseStatus = 507;
                context.errorCode = "result_storage_unavailable";
                context.errorMessage = preflightError;
            } else if (workerDeadlineExceeded()) {
                context.responded = true;
                context.responseStatus = 504;
                context.errorCode = "deadline_exceeded";
                context.errorMessage = "job deadline expired during mutation preflight";
            } else {
                invokeQueuedHandler(context);
                noteWorkerProgress(90);
                operationOk = context.responded && context.responseStatus < 400;
            }
        } else if (context.operation == BleOperation::MachineRefresh) {
            operationOk = runCombinedRefresh(job, context);
        } else {
            if (job.resource) {
                const String resource = String(job.kind.c_str()).startsWith("machine_")
                    ? String(job.kind.c_str()).substring(8)
                    : String(job.kind.c_str());
                ResourceCacheEntry currentCache;
                resourceSatisfiedByNewerCache =
                    copyResourceCache(context.target, resource, currentCache) &&
                    currentCache.sampledAtMs != 0 &&
                    bridge_runtime_policy::deadlineReached(
                        currentCache.sampledAtMs, job.submittedAtMs);
            }
            if (resourceSatisfiedByNewerCache) {
                context.responded = true;
                context.responseStatus = 200;
                operationOk = true;
                noteWorkerProgress();
            } else {
                invokeQueuedHandler(context);
                noteWorkerProgress(90);
                operationOk = context.responded && context.responseStatus < 400;
            }
            if (operationOk && workerDeadlineExceeded() && !context.mutationCommitted) {
                operationOk = false;
                context.responseStatus = 504;
                context.errorCode = "deadline_exceeded";
                context.errorMessage = "job completed after its logical deadline";
            }
            if (operationOk && context.machineLoaded && !workerTargetStillCurrent()) {
                operationOk = false;
                context.responseStatus = 409;
                context.errorCode = "target_changed";
                context.errorMessage = "machine was deleted or replaced while the job was running";
            }
            if (job.resource) {
                const String resource = String(job.kind.c_str()).startsWith("machine_")
                    ? String(job.kind.c_str()).substring(8)
                    : String(job.kind.c_str());
                if (operationOk && !resourceSatisfiedByNewerCache) {
                    String publishError;
                    operationOk = publishResourceResult(job, resource, context.resultPath, publishError);
                    if (!operationOk) {
                        context.responseStatus = 500;
                        context.errorCode = "cache_publish_failed";
                        context.errorMessage = publishError;
                    }
                } else {
                    littleFsRemoveLocked(context.resultPath);
                }
                if (!operationOk) {
                    updateCacheFailure(context.target,
                                       resource,
                                       context.jobId,
                                       context.errorCode.isEmpty() ? "refresh_failed" : context.errorCode,
                                       context.errorMessage);
                }
            }
        }

        if (!operationOk && context.mutationJob && context.mutationCommitted) {
            const int originalStatus = context.responseStatus;
            const String originalCode = context.errorCode.isEmpty()
                ? String("ble_operation_failed")
                : context.errorCode;
            const String originalMessage = context.errorMessage.isEmpty()
                ? String("mutation was acknowledged but its result did not complete")
                : context.errorMessage;
            context.errorCode = "";
            context.errorMessage = "";

            DynamicJsonDocument uncertainResult(1024);
            uncertainResult["ok"] = true;
            uncertainResult["mutationApplied"] = true;
            uncertainResult["resultIncomplete"] = true;
            uncertainResult["retrySafe"] = false;
            uncertainResult["partialApplicationPossible"] = true;
            JsonObject warning = uncertainResult.createNestedObject("warning");
            warning["code"] = "mutation_result_incomplete";
            warning["message"] =
                "BLE acknowledged at least one write, but the final response was incomplete; do not retry automatically";
            warning["causeCode"] = originalCode;
            warning["causeMessage"] = originalMessage;
            warning["causeStatus"] = originalStatus;
            sendJson(uncertainResult, 200);
            operationOk = context.responded && context.responseStatus < 400;
            context.errorCode = "mutation_result_incomplete";
            context.errorMessage =
                "mutation was acknowledged but its final response was incomplete; automatic retry is unsafe";
            resetClientAfterCompletion = true;
        }

        if (context.mutationCommitted &&
            (context.operation == BleOperation::MachineBrew ||
             context.operation == BleOperation::MachineConfirm)) {
            // An acknowledged action changes HX even when the subsequent
            // status read/result fails. Do not advertise a pre-action summary
            // as fresh to cache-first consumers after the job completes.
            invalidateResourceCache(context.target, "summary");
        }

        if (operationOk && workerDeadlineExceeded() && !context.mutationCommitted) {
            operationOk = false;
            context.responseStatus = 504;
            context.errorCode = "deadline_exceeded";
            context.errorMessage = "job completed after its logical deadline";
        }
        if (operationOk && workerDeadlineExceeded() && context.mutationCommitted) {
            if (context.errorCode.isEmpty()) {
                context.errorCode = "completed_after_deadline";
                context.errorMessage =
                    "mutation was acknowledged before its result completed after the logical deadline";
            }
        }
        if (operationOk && context.machineLoaded && !context.mutationCommitted &&
            !workerTargetStillCurrent()) {
            operationOk = false;
            context.responseStatus = 409;
            context.errorCode = "target_changed";
            context.errorMessage = "machine was deleted or replaced while the job was running";
        }
        if (operationOk && context.machineLoaded && workerTargetStillCurrent()) {
            if (!mergeWorkerMachineDetailsIfChanged(context) && context.errorCode.isEmpty()) {
                context.errorCode = "machine_details_persistence_deferred";
                context.errorMessage =
                    "machine details could not be merged before the logical deadline and will retry on the next live job";
            }
        }

        if (!operationOk && context.errorMessage.isEmpty()) {
            context.errorMessage = context.responded ? "BLE operation failed" : "BLE operation produced no response";
        }
        if (!operationOk && context.errorCode.isEmpty()) {
            context.errorCode = workerDeadlineExceeded() ? "deadline_exceeded" : "ble_operation_failed";
        }
        if (!operationOk && context.spoolResult && !context.resultPath.isEmpty()) {
            littleFsRemoveLocked(context.resultPath);
        }
        if (!context.resultReservationPath.isEmpty()) {
            littleFsRemoveLocked(context.resultReservationPath);
        }

        lastBleActivityAtMs = millis();
        if (resetClientAfterCompletion || (!operationOk && context.responseStatus >= 500)) {
            resetBleClientAfterFailure();
        }

        bool completionAccepted = false;
        if (jobMutex != nullptr) {
            while (xSemaphoreTake(jobMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
                noteWorkerProgress();
                vTaskDelay(1);
            }
            completionAccepted = jobScheduler.finish(
                job.id,
                operationOk,
                millis(),
                operationOk && context.spoolResult ? std::string(context.resultPath.c_str()) : std::string{},
                operationOk && !job.resource ? std::string(context.responseBody.c_str()) : std::string{},
                context.responseStatus,
                std::string(context.errorCode.c_str()),
                std::string(context.errorMessage.c_str()));
            workerCurrentJobId = "";
            workerCurrentJobKind = "";
            workerCurrentJobTarget = "";
            xSemaphoreGive(jobMutex);
        }
        if (!completionAccepted) {
            if (!context.resultPath.isEmpty()) {
                littleFsRemoveLocked(context.resultPath);
            }
            resetBleClientAfterFailure();
        }
        workerBleCallDepth.store(0, std::memory_order_release);
        workerBleCallActive.store(false, std::memory_order_release);
        workerMaintenanceCallActive.store(false, std::memory_order_release);
        workerJobActive.store(false, std::memory_order_release);
        workerCurrentProgress.store(0, std::memory_order_release);
        workerExecution.store(nullptr, std::memory_order_release);
        refreshCachedStorageTotals();
        updateWorkerOwnedHealth();
    }
}

void workerSupervisorTask(void*) {
    for (;;) {
        const uint32_t nowMs = millis();
        const bool supervisedWorkActive = workerJobActive.load(std::memory_order_acquire) ||
            workerMaintenanceCallActive.load(std::memory_order_acquire);
        if (bridge_runtime_policy::watchdogShouldReboot(
                supervisedWorkActive,
                workerBleCallActive.load(std::memory_order_acquire),
                nowMs,
                workerHeartbeatAtMs.load(std::memory_order_acquire),
                WORKER_STALL_WATCHDOG_MS)) {
            rtcWatchdogMarker.magic = RTC_WATCHDOG_MAGIC;
            rtcWatchdogMarker.atMs = nowMs;
            rtcWatchdogMarker.jobId[0] = '\0';
            rtcWatchdogMarker.kind[0] = '\0';
            rtcWatchdogMarker.target[0] = '\0';
            if (jobMutex != nullptr && xSemaphoreTake(jobMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                strlcpy(rtcWatchdogMarker.jobId,
                        workerCurrentJobId.c_str(),
                        sizeof(rtcWatchdogMarker.jobId));
                strlcpy(rtcWatchdogMarker.kind,
                        workerCurrentJobKind.c_str(),
                        sizeof(rtcWatchdogMarker.kind));
                strlcpy(rtcWatchdogMarker.target,
                        workerCurrentJobTarget.c_str(),
                        sizeof(rtcWatchdogMarker.target));
                xSemaphoreGive(jobMutex);
            }
            delay(20);
            ESP.restart();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void publishBridgeHealth() {
    BridgeHealthSnapshot next;
    if (healthMutex != nullptr && xSemaphoreTake(healthMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        next = bridgeHealth;
        xSemaphoreGive(healthMutex);
    }
    const bool workerBusy = workerJobActive.load(std::memory_order_acquire);
    next.workerBusy = workerBusy;
    next.currentProgress = workerBusy
        ? workerCurrentProgress.load(std::memory_order_acquire)
        : 0;
    next.currentJobAgeMs = workerBusy
        ? static_cast<uint32_t>(millis() - workerCurrentStartedAtMs.load(std::memory_order_acquire))
        : 0;
    next.workerHeartbeatAgeMs = workerBusy
        ? static_cast<uint32_t>(millis() - workerHeartbeatAtMs.load(std::memory_order_acquire))
        : 0;
    next.freeHeap = ESP.getFreeHeap();
    next.minimumFreeHeap = ESP.getMinFreeHeap();
    next.largestFreeHeapBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    next.durableMachineWrites = durableMachineWriteCount;
    if (machineMutex != nullptr && xSemaphoreTake(machineMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        next.savedMachineCount = savedMachines.size();
        xSemaphoreGive(machineMutex);
    }
    next.watchdogMarkerPresent = rtcWatchdogMarker.magic == RTC_WATCHDOG_MAGIC;
    next.watchdogAtMs = rtcWatchdogMarker.atMs;
    next.resetReason = static_cast<uint32_t>(esp_reset_reason());
    if (next.watchdogMarkerPresent) {
        strlcpy(next.watchdogJobId, rtcWatchdogMarker.jobId, sizeof(next.watchdogJobId));
        strlcpy(next.watchdogJobKind, rtcWatchdogMarker.kind, sizeof(next.watchdogJobKind));
        strlcpy(next.watchdogJobTarget, rtcWatchdogMarker.target, sizeof(next.watchdogJobTarget));
    }
    if (jobMutex != nullptr && xSemaphoreTake(jobMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        next.queuedJobs = jobScheduler.queuedCount();
        next.runningJobs = jobScheduler.runningCount();
        const bridge_jobs::Counters& counters = jobScheduler.counters();
        next.submittedJobs = counters.submitted;
        next.completedJobs = counters.completed;
        next.failedJobs = counters.failed;
        next.cancelledJobs = counters.cancelled;
        next.rejectedJobs = counters.rejected;
        next.coalescedJobs = counters.coalesced;
        next.backgroundEvictedJobs = counters.backgroundEvicted;
        copyHealthText(next.currentJobId, sizeof(next.currentJobId), workerCurrentJobId);
        copyHealthText(next.currentJobKind, sizeof(next.currentJobKind), workerCurrentJobKind);
        copyHealthText(next.currentJobTarget, sizeof(next.currentJobTarget), workerCurrentJobTarget);
        xSemaphoreGive(jobMutex);
    }
    if (healthMutex != nullptr && xSemaphoreTake(healthMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bridgeHealth = next;
        xSemaphoreGive(healthMutex);
    }
}

void enqueueGenericHttpJob(const String& kind,
                           BleOperation operation,
                           bridge_jobs::Priority priority,
                           uint32_t deadlineMs,
                           bool requireBody = false,
                           const String& target = "bridge") {
    String normalizedBody;
    DynamicJsonDocument request(8192);
    bool hasRequest = false;
    if (requireBody || server.hasArg("plain")) {
        if (!server.hasArg("plain") || server.arg("plain").length() > 8192) {
            sendError(400, "missing or oversized request body");
            return;
        }
        const DeserializationError parseError = deserializeJson(request, server.arg("plain"));
        if (parseError) {
            sendError(400, String("invalid json: ") + parseError.c_str());
            return;
        }
        hasRequest = true;
    }

    String requestedSerial = hasRequest ? String(request["serial"] | "") : String("");
    String requestedAddress = hasRequest ? String(request["address"] | "") : String("");
    requestedSerial.trim();
    requestedAddress.trim();
    SavedMachine* jobMachine = nullptr;
    uint8_t targetAddressType = BLE_ADDR_PUBLIC;
    bool explicitAddressType = false;
    if (hasRequest && !request["addressType"].isNull()) {
        if (!parseAddressTypeRequest(request["addressType"], targetAddressType)) {
            sendError(400, "addressType must be public, random, 0, or 1");
            return;
        }
        explicitAddressType = true;
    }
    if (!requestedAddress.isEmpty() && !normalizeBleAddress(requestedAddress)) {
        sendError(400, "address must be a BLE MAC like C8:B4:17:D8:A3:8C");
        return;
    }
    if (!requestedSerial.isEmpty()) {
        jobMachine = findSavedMachineBySerial(requestedSerial);
        if (jobMachine == nullptr) {
            sendError(404, "saved machine not found");
            return;
        }
        if (!requestedAddress.isEmpty() &&
            !requestedAddress.equalsIgnoreCase(jobMachine->address)) {
            sendError(400, "serial and address identify different machines");
            return;
        }
    } else if (!requestedAddress.isEmpty()) {
        jobMachine = findSavedMachineByAddress(requestedAddress);
        if (jobMachine == nullptr && !explicitAddressType && scanDataMutex != nullptr &&
            xSemaphoreTake(scanDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (const ScanRecord& record : scannedDevices) {
                if (record.address.equalsIgnoreCase(requestedAddress)) {
                    targetAddressType = record.addressType;
                    break;
                }
            }
            xSemaphoreGive(scanDataMutex);
        }
    } else {
        String selectedSnapshot;
        if (healthMutex != nullptr && xSemaphoreTake(healthMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            selectedSnapshot = bridgeHealth.selectedAddress;
            targetAddressType = bridgeHealth.selectedAddressType;
            xSemaphoreGive(healthMutex);
        }
        if (!selectedSnapshot.isEmpty()) {
            requestedAddress = selectedSnapshot;
            jobMachine = findSavedMachineByAddress(selectedSnapshot);
        }
    }

    String effectiveTarget = target;
    if (jobMachine != nullptr) {
        if (explicitAddressType && targetAddressType != jobMachine->addressType) {
            sendError(400, "addressType conflicts with the saved machine identity");
            return;
        }
        effectiveTarget = jobMachine->serial;
        requestedAddress = jobMachine->address;
        targetAddressType = jobMachine->addressType;
    } else if (!requestedAddress.isEmpty()) {
        requestedAddress.toLowerCase();
        effectiveTarget = String("address:") + requestedAddress;
    }
    if (hasRequest) {
        if (jobMachine != nullptr) {
            request["serial"] = jobMachine->serial;
        }
        if (!requestedAddress.isEmpty()) {
            request["address"] = requestedAddress;
            request["addressType"] = targetAddressType;
        }
        serializeJson(request, normalizedBody);
    }
    bridge_jobs::Submission submission;
    submission.kind = kind.c_str();
    submission.target = effectiveTarget.c_str();
    submission.operationCode = static_cast<uint16_t>(operation);
    submission.request = normalizedBody.c_str();
    submission.targetAddress = requestedAddress.c_str();
    submission.targetAddressType = targetAddressType;
    if (jobMachine != nullptr) {
        submission.identity = serializeMachineIdentity(*jobMachine).c_str();
    }
    submission.priority = priority;
    submission.deadlineMs = deadlineMs;
    if (priority != bridge_jobs::Priority::Mutation) {
        submission.coalesceKey = defaultJobCoalesceKey(
            kind, effectiveTarget, operation, "", normalizedBody).c_str();
    }
    bridge_jobs::Job job;
    if (submitJob(submission, job)) {
        sendAcceptedJob(job);
    }
}

enum class BackgroundSubmitResult : uint8_t {
    Accepted,
    Covered,
    Deferred,
};

BackgroundSubmitResult submitBackgroundJob(const bridge_jobs::Submission& submission,
                                           bool requireIdleQueue = false) {
    if (jobMutex == nullptr || xSemaphoreTake(jobMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return BackgroundSubmitResult::Deferred;
    }
    if (submission.resource) {
        const std::string kindPrefix = "machine_";
        if (submission.kind.compare(0, kindPrefix.size(), kindPrefix) == 0) {
            const std::string resource = submission.kind.substr(kindPrefix.size());
            if (jobScheduler.hasActiveResource(submission.target, resource)) {
                xSemaphoreGive(jobMutex);
                return BackgroundSubmitResult::Covered;
            }
        }
    }
    if (jobScheduler.hasActiveCoalesceKey(submission.coalesceKey)) {
        xSemaphoreGive(jobMutex);
        return BackgroundSubmitResult::Covered;
    }
    if (requireIdleQueue && jobScheduler.activeCount() != 0) {
        xSemaphoreGive(jobMutex);
        return BackgroundSubmitResult::Deferred;
    }
    // Background maintenance is optional and must not inflate rejection
    // telemetry or churn the same trailing machines while the fixed queue or
    // retained-job registry is full. Interactive submissions retain their
    // normal background-eviction behavior in submitJob().
    if (!jobScheduler.canAcceptBackground()) {
        xSemaphoreGive(jobMutex);
        return BackgroundSubmitResult::Deferred;
    }
    const bridge_jobs::SubmitResult result = jobScheduler.submit(submission, millis());
    xSemaphoreGive(jobMutex);
    const bool accepted = result.status == bridge_jobs::SubmitStatus::Accepted ||
        result.status == bridge_jobs::SubmitStatus::Coalesced;
    if (accepted && bleWorkerTaskHandle != nullptr) {
        xTaskNotifyGive(bleWorkerTaskHandle);
    }
    return accepted ? BackgroundSubmitResult::Accepted : BackgroundSubmitResult::Deferred;
}

bool backgroundStatsDue(const SavedMachine& machine, uint32_t nowMs) {
    MachineGenerationLock generationLock(100);
    if (!generationLock) {
        return false;
    }
    for (const MachineGenerationEntry& entry : machineGenerations) {
        if (entry.occupied && entry.serial.equalsIgnoreCase(machine.serial) &&
            entry.generation == machine.generation) {
            return bridge_runtime_policy::elapsedAtLeast(
                nowMs, entry.lastStatsScheduledAtMs, STATS_HISTORY_POLL_INTERVAL_MS);
        }
    }
    return false;
}

void markBackgroundStatsScheduled(const SavedMachine& machine, uint32_t nowMs) {
    MachineGenerationLock generationLock(100);
    if (!generationLock) {
        return;
    }
    for (MachineGenerationEntry& entry : machineGenerations) {
        if (entry.occupied && entry.serial.equalsIgnoreCase(machine.serial) &&
            entry.generation == machine.generation) {
            entry.lastStatsScheduledAtMs = nowMs;
            return;
        }
    }
}

void scheduleBackgroundBleJobs(uint32_t nowMs) {
    const bool idleScanRetryDue = nextIdleScanSubmitAtMs == 0 ||
        bridge_runtime_policy::deadlineReached(nowMs, nextIdleScanSubmitAtMs);
    if (idleScanRetryDue &&
        static_cast<uint32_t>(nowMs - lastIdleScanAtMs) >= IDLE_SCAN_INTERVAL_MS) {
        bridge_jobs::Submission scan;
        scan.kind = "scan";
        scan.target = "bridge";
        scan.operationCode = static_cast<uint16_t>(BleOperation::Scan);
        scan.coalesceKey = "background-scan";
        scan.priority = bridge_jobs::Priority::Background;
        scan.deadlineMs = 12000;
        const BackgroundSubmitResult scanResult = submitBackgroundJob(scan);
        if (scanResult == BackgroundSubmitResult::Deferred) {
            nextIdleScanSubmitAtMs = nowMs + 250;
        } else {
            lastIdleScanAtMs = nowMs;
            nextIdleScanSubmitAtMs = 0;
        }
    }

    if (nextBackgroundStatsDispatchAtMs != 0 &&
        !bridge_runtime_policy::deadlineReached(nowMs, nextBackgroundStatsDispatchAtMs)) {
        return;
    }
    std::vector<SavedMachine> machines;
    if (machineMutex != nullptr && xSemaphoreTake(machineMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        machines = savedMachines;
        xSemaphoreGive(machineMutex);
    }
    if (machines.empty()) {
        backgroundStatsCursor = 0;
        nextBackgroundStatsDispatchAtMs = nowMs + 1000;
        return;
    }

    const size_t startIndex = backgroundStatsCursor % machines.size();
    for (size_t attempt = 0; attempt < machines.size(); ++attempt) {
        const size_t machineIndex = (startIndex + attempt) % machines.size();
        const SavedMachine& machine = machines[machineIndex];
        if (!backgroundStatsDue(machine, nowMs)) {
            continue;
        }
        ResourceCacheEntry statsCache;
        if (copyResourceCache(machine.serial, "stats", statsCache) &&
            statsCache.sampledAtMs != 0 &&
            !bridge_runtime_policy::elapsedAtLeast(
                nowMs, statsCache.sampledAtMs, STATS_HISTORY_POLL_INTERVAL_MS)) {
            continue;
        }
        const nivona::ModelInfo modelInfo = nivona::detectModelInfo(toNivonaDetails(machine));
        std::vector<const nivona::RegisterProbe*> metrics;
        nivona::selectStatsDescriptors(modelInfo, metrics);
        if (metrics.empty()) {
            markBackgroundStatsScheduled(machine, nowMs);
            continue;
        }
        bridge_jobs::Submission stats;
        stats.kind = "machine_stats";
        stats.target = machine.serial.c_str();
        stats.operationCode = static_cast<uint16_t>(BleOperation::MachineStats);
        stats.identity = serializeMachineIdentity(machine).c_str();
        stats.coalesceKey = defaultJobCoalesceKey(
            "machine_stats", machine.serial, BleOperation::MachineStats, "", "").c_str();
        stats.resultUrl = (String("/api/machines/") + machine.serial + "/stats").c_str();
        stats.priority = bridge_jobs::Priority::Background;
        stats.deadlineMs = 15000;
        stats.resource = true;
        // Start at most one background statistics job at a time. Its logical
        // deadline begins at admission, so pre-filling the eight-slot queue
        // would make later machines expire before they ever reached BLE.
        const BackgroundSubmitResult result = submitBackgroundJob(stats, true);
        backgroundStatsCursor = (machineIndex + 1) % machines.size();
        if (result == BackgroundSubmitResult::Deferred) {
            nextBackgroundStatsDispatchAtMs = nowMs + 250;
        } else {
            markBackgroundStatsScheduled(machine, nowMs);
            nextBackgroundStatsDispatchAtMs = nowMs + 250;
        }
        return;
    }
    backgroundStatsCursor = (startIndex + 1) % machines.size();
    nextBackgroundStatsDispatchAtMs = nowMs + 1000;
}

bool beginHttpRequest(void*) {
    return machineMutex == nullptr ||
        xSemaphoreTake(machineMutex, pdMS_TO_TICKS(100)) == pdTRUE;
}

void finishHttpRequest(uint32_t durationUs, void*) {
    if (machineMutex != nullptr) {
        xSemaphoreGive(machineMutex);
    }
    if (healthMutex != nullptr && xSemaphoreTake(healthMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        bridgeHealth.httpLastDurationUs = durationUs;
        bridgeHealth.httpMaxDurationUs = std::max(bridgeHealth.httpMaxDurationUs, durationUs);
        xSemaphoreGive(healthMutex);
    }
}

bool renderStatusEvent(String& jsonOut, void*) {
    DynamicJsonDocument status(STATUS_JSON_CAPACITY);
    status["ok"] = true;
    appendStatus(status);
    if (status.overflowed()) {
        return false;
    }
    jsonOut = "";
    return serializeJson(status, jsonOut) != 0;
}

bridge_http::EventJobLookup renderJobEvent(const String& id,
                                           String& jsonOut,
                                           bool& terminalOut,
                                           void*) {
    terminalOut = false;
    if (jobMutex == nullptr || xSemaphoreTake(jobMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return bridge_http::EventJobLookup::Busy;
    }
    jobScheduler.expire(millis());
    bridge_jobs::PublicJob job;
    const bool found = jobScheduler.getPublic(std::string(id.c_str()), job);
    xSemaphoreGive(jobMutex);
    if (!found) {
        return bridge_http::EventJobLookup::Missing;
    }
    terminalOut = bridge_jobs::Scheduler::terminal(job.state);
    DynamicJsonDocument document(2048);
    JsonObject jobJson = document.to<JsonObject>();
    appendJobJson(jobJson, job);
    if (document.overflowed()) {
        return bridge_http::EventJobLookup::Busy;
    }
    jsonOut = "";
    if (serializeJson(document, jsonOut) == 0) {
        return bridge_http::EventJobLookup::Busy;
    }
    return bridge_http::EventJobLookup::Found;
}

void publishJobEvent(const char* id, void*) {
    server.notifyJobChanged(id);
}

void registerRoutes() {
    const char* trackedHeaders[] = {CLIENT_TIME_HEADER};
    server.collectHeaders(trackedHeaders, 1);
    server.on("/", HTTP_GET, handleRoot);
    server.on("/icons/recipe", HTTP_GET, handleRecipeIconAsset);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/devices", HTTP_GET, handleDevices);
    server.on("/api/details", HTTP_GET, []() {
        enqueueGenericHttpJob("details", BleOperation::Details, bridge_jobs::Priority::ForcedRead, 12000);
    });
    server.on("/api/logs", HTTP_GET, handleLogs);
    server.on("/api/machines", HTTP_GET, handleMachinesList);
    server.on("/api/machines", HTTP_POST, []() {
        enqueueGenericHttpJob("machine_create", BleOperation::MachinesCreate, bridge_jobs::Priority::Mutation, 30000, true);
    });
    server.on("/api/machines/manual", HTTP_POST, handleMachinesManualCreate);
    server.on("/api/machines/probe", HTTP_POST, []() {
        enqueueGenericHttpJob("machine_probe", BleOperation::MachineProbe, bridge_jobs::Priority::ForcedRead, 30000, true);
    });
    server.on("/api/machines/reset", HTTP_POST, handleMachinesReset);

    server.on("/api/backup/export", HTTP_GET, handleBackupExport);
    server.on("/api/backup/restore", HTTP_POST, handleBackupRestoreFinished, handleBackupRestoreUpload);
    server.on("/api/wifi/save", HTTP_POST, handleWifiSave);
    server.on("/api/time/config", HTTP_POST, handleTimeConfigSave);
    server.on("/api/history/config", HTTP_POST, handleHistoryConfigSave);
    server.on("/api/scan", HTTP_POST, []() {
        enqueueGenericHttpJob("scan", BleOperation::Scan, bridge_jobs::Priority::ForcedRead, 12000);
    });
    server.on("/api/connect", HTTP_POST, []() {
        enqueueGenericHttpJob("connect", BleOperation::Connect, bridge_jobs::Priority::Mutation, 30000, true);
    });
    server.on("/api/disconnect", HTTP_POST, []() {
        enqueueGenericHttpJob("disconnect", BleOperation::Disconnect, bridge_jobs::Priority::Mutation, 30000);
    });
    server.on("/api/pair", HTTP_POST, []() {
        enqueueGenericHttpJob("pair", BleOperation::Pair, bridge_jobs::Priority::Mutation, 30000, true);
    });
    server.on("/api/notifications", HTTP_POST, []() {
        enqueueGenericHttpJob("notifications", BleOperation::Notifications, bridge_jobs::Priority::Mutation, 30000, true);
    });
    server.on("/api/protocol/session", HTTP_POST, []() {
        enqueueGenericHttpJob("protocol_session", BleOperation::ProtocolSession, bridge_jobs::Priority::Mutation, 60000, true);
    });
    server.on("/api/protocol/hu", HTTP_POST, []() {
        enqueueGenericHttpJob("protocol_hu", BleOperation::ProtocolHu, bridge_jobs::Priority::ForcedRead, 60000, true);
    });
    server.on("/api/protocol/send-frame", HTTP_POST, []() {
        enqueueGenericHttpJob("protocol_send_frame", BleOperation::ProtocolSendFrame, bridge_jobs::Priority::Mutation, 60000, true);
    });
    server.on("/api/protocol/app-probe", HTTP_POST, []() {
        enqueueGenericHttpJob("protocol_app_probe", BleOperation::ProtocolAppProbe, bridge_jobs::Priority::ForcedRead, 60000, true);
    });
    server.on("/api/protocol/verify", HTTP_POST, []() {
        enqueueGenericHttpJob("protocol_verify", BleOperation::ProtocolVerify, bridge_jobs::Priority::ForcedRead, 60000, true);
    });
    server.on("/api/protocol/stats-probe", HTTP_POST, []() {
        enqueueGenericHttpJob("protocol_stats_probe", BleOperation::ProtocolStatsProbe, bridge_jobs::Priority::ForcedRead, 60000, true);
    });
    server.on("/api/protocol/settings-probe", HTTP_POST, []() {
        enqueueGenericHttpJob("protocol_settings_probe", BleOperation::ProtocolSettingsProbe, bridge_jobs::Priority::ForcedRead, 60000, true);
    });
    server.on("/api/protocol/worker-probe", HTTP_POST, []() {
        enqueueGenericHttpJob("protocol_worker_probe", BleOperation::ProtocolWorkerProbe, bridge_jobs::Priority::ForcedRead, 60000, true);
    });
    server.on("/api/gatt/services", HTTP_POST, []() {
        enqueueGenericHttpJob("gatt_services", BleOperation::GattServices, bridge_jobs::Priority::ForcedRead, 60000, true);
    });
    server.on("/api/gatt/read", HTTP_POST, []() {
        enqueueGenericHttpJob("gatt_read", BleOperation::GattRead, bridge_jobs::Priority::ForcedRead, 60000, true);
    });
    server.on("/api/gatt/write", HTTP_POST, []() {
        enqueueGenericHttpJob("gatt_write", BleOperation::GattWrite, bridge_jobs::Priority::Mutation, 60000, true);
    });
    server.on("/api/protocol/raw-read", HTTP_POST, []() {
        enqueueGenericHttpJob("protocol_raw_read", BleOperation::ProtocolRawRead, bridge_jobs::Priority::ForcedRead, 60000, true);
    });
    server.on("/api/protocol/raw-write", HTTP_POST, []() {
        enqueueGenericHttpJob("protocol_raw_write", BleOperation::ProtocolRawWrite, bridge_jobs::Priority::Mutation, 60000, true);
    });
    server.on("/api/logs/clear", HTTP_POST, handleLogsClear);
    server.on("/api/reboot", HTTP_POST, handleReboot);
    server.on("/api/ota", HTTP_POST, handleOtaFinished, handleOtaUpload);

    server.onNotFound([]() {
        const String uri = server.uri();
        const String jobPrefix = "/api/jobs/";
        if (uri.startsWith(jobPrefix)) {
            String remainder = uri.substring(jobPrefix.length());
            bool resultRequested = false;
            if (remainder.endsWith("/result")) {
                remainder = remainder.substring(0, remainder.length() - 7);
                resultRequested = true;
            }
            if (!remainder.isEmpty() && remainder.indexOf('/') < 0 && server.method() == HTTP_GET) {
                handleJobApiRoute(remainder, resultRequested);
                return;
            }
        }
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
    jobMutex          = xSemaphoreCreateMutex();
    cacheMutex        = xSemaphoreCreateMutex();
    healthMutex       = xSemaphoreCreateMutex();
    machineMutex      = xSemaphoreCreateMutex();
    machineGenerationMutex = xSemaphoreCreateRecursiveMutex();
    bootNonce = esp_random();
    jobScheduler.reset(bootNonce);
    jobScheduler.setChangeCallback(publishJobEvent);
    server.setRequestLifecycle(beginHttpRequest, finishHttpRequest);
    server.configureEvents(bootNonce, renderStatusEvent, renderJobEvent);

    preferences.begin(PREFS_WIFI, false);
    machinePreferences.begin(PREFS_MACHINES, false);
    bridge_time::begin(addTimeLog);
    String filesystemError;
    if (!initializeLittleFs(filesystemError)) {
        lastError = filesystemError;
        addLog("fs", String("Failed to initialize LittleFS: ") + filesystemError);
    } else {
        history_storage::begin();
        cleanupBootTemporaryFiles();
        if (littleFsReady) {
            addLog("fs", "LittleFS ready");
        } else {
            addLog("fs", "LittleFS disabled after restore recovery failure");
        }
    }
    loadSavedMachines();
    configureHistoryBudget();
    refreshCachedStorageTotals();
    connectWifi();
    setupMdns();

    registerRoutes();
    if (!server.begin()) {
        lastError = "failed to start ESP-IDF HTTP server";
        addLog("http", lastError.snapshot());
    }
    if (xTaskCreatePinnedToCore(bleWorkerTask,
                                "ble-worker",
                                12 * 1024,
                                nullptr,
                                1,
                                &bleWorkerTaskHandle,
                                0) != pdPASS) {
        addLog("worker", "Failed to create BLE worker task");
    }
    if (xTaskCreatePinnedToCore(workerSupervisorTask,
                                "ble-supervisor",
                                3072,
                                nullptr,
                                2,
                                &workerSupervisorTaskHandle,
                                1) != pdPASS) {
        addLog("worker", "Failed to create BLE supervisor task");
    }
    publishBridgeHealth();
    addLog("http", "HTTP server started");
}

void loop() {
    const uint32_t nowMs = millis();
    tickWifiConnection(nowMs);
    bridge_time::tick(WiFi.status() == WL_CONNECTED, nowMs, addTimeLog);
    scheduleBackgroundBleJobs(nowMs);
    publishBridgeHealth();
    bool eventActivity = workerJobActive.load(std::memory_order_acquire);
    if (healthMutex != nullptr && xSemaphoreTake(healthMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        eventActivity = eventActivity || bridgeHealth.queuedJobs != 0;
        xSemaphoreGive(healthMutex);
    }
    server.tickEvents(nowMs, eventActivity);
    delay(2);
}
