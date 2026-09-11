#include "bridge_time.h"
#include "bridge_time_retry.h"

#include <algorithm>
#include <atomic>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <time.h>

namespace bridge_time {

namespace {

constexpr char NTP_TZ[] = "UTC0";
constexpr char DEFAULT_NTP_SERVER_PRIMARY[] = "pool.ntp.org";
constexpr char DEFAULT_NTP_SERVER_SECONDARY[] = "time.google.com";
constexpr char DEFAULT_NTP_SERVER_TERTIARY[] = "time.cloudflare.com";
constexpr char TIME_MODE_NTP[] = "ntp";
constexpr char TIME_MODE_NO_TIME[] = "no_time";
constexpr char PREFS_NAMESPACE[] = "time";
constexpr char PREFS_LAST_NTP_UNIX[] = "last_ntp";
constexpr char PREFS_TIME_MODE[] = "mode";
constexpr char PREFS_NTP_PRIMARY[] = "ntp1";
constexpr char PREFS_NTP_SECONDARY[] = "ntp2";
constexpr char PREFS_NTP_TERTIARY[] = "ntp3";
constexpr time_t MIN_VALID_EPOCH = 1704067200; // 2024-01-01T00:00:00Z
constexpr uint32_t NTP_RETRY_INTERVAL_MS = 60000;
constexpr uint32_t NTP_PROBE_TIMEOUT_MS = 1200;
constexpr uint32_t CLOCK_POLL_INTERVAL_MS = 1000;
constexpr uint32_t RTC_MAGIC = 0x4254494D;
constexpr uint32_t NTP_DIAGNOSTIC_TASK_STACK_BYTES = 6144;
constexpr UBaseType_t NTP_DIAGNOSTIC_TASK_PRIORITY = 1;
constexpr BaseType_t NTP_DIAGNOSTIC_TASK_CORE = 0;

struct NtpDiagnosticState {
    String code;
    String message;
    String server;
    String address;
    uint32_t atMs{0};
    uint32_t roundTripMs{0};
};

struct NtpDiagnosticRequest {
    bool pending{false};
    uint32_t generation{0};
    uint32_t requestedAtMs{0};
    String primary;
    String secondary;
    String tertiary;
    LogFn logFn{nullptr};
};

struct NtpDiagnosticResult {
    String code;
    String message;
    String server;
    String address;
    uint32_t roundTripMs{0};
};

bool configured = false;
bool synced = false;
bool restored = false;
bool clientSeeded = false;
String timeMode = TIME_MODE_NTP;
String ntpServerPrimary = DEFAULT_NTP_SERVER_PRIMARY;
String ntpServerSecondary = DEFAULT_NTP_SERVER_SECONDARY;
String ntpServerTertiary = DEFAULT_NTP_SERVER_TERTIARY;
bool staConnectedLastTick = false;
uint32_t lastAttemptMs = 0;
uint32_t lastSuccessMs = 0;
uint32_t lastPollMs = 0;
time_t lastSyncedEpoch = 0;
std::atomic<bool> syncNotificationPending{false};
NtpDiagnosticState ntpDiagnostic;
NtpDiagnosticRequest ntpDiagnosticRequest;
SemaphoreHandle_t stateMutex = nullptr;
TaskHandle_t ntpDiagnosticTaskHandle = nullptr;
bool ntpDiagnosticRunning = false;
uint32_t stateGeneration = 0;
Preferences clockPrefs;
bool clockPrefsReady = false;

RTC_DATA_ATTR uint32_t rtcMagic = 0;
RTC_DATA_ATTR int64_t rtcEpoch = 0;

void cacheRtcEpoch(time_t epoch);

class StateLock {
public:
    StateLock() {
        if (stateMutex != nullptr) {
            locked_ = xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE;
        }
    }

    ~StateLock() {
        if (locked_) {
            xSemaphoreGive(stateMutex);
        }
    }

    StateLock(const StateLock&) = delete;
    StateLock& operator=(const StateLock&) = delete;

private:
    bool locked_{false};
};

void ensureStateMutex() {
    if (stateMutex == nullptr) {
        stateMutex = xSemaphoreCreateMutex();
    }
}

bool hasValidEpoch(time_t epoch) {
    return epoch >= MIN_VALID_EPOCH;
}

bool ntpModeEnabled() {
    return timeMode == TIME_MODE_NTP;
}

String normalizeMode(const String& value) {
    String mode = value;
    mode.trim();
    mode.toLowerCase();
    if (mode == TIME_MODE_NO_TIME) {
        return String(TIME_MODE_NO_TIME);
    }
    return String(TIME_MODE_NTP);
}

String sanitizeServerValue(const String& value, const char* fallback) {
    String server = value;
    server.trim();
    if (server.isEmpty()) {
        server = fallback;
    }
    return server;
}

String formatIso8601Utc(time_t epoch) {
    struct tm utcTime;
    if (gmtime_r(&epoch, &utcTime) == nullptr) {
        return "";
    }

    char buffer[32];
    if (strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utcTime) == 0) {
        return "";
    }
    return String(buffer);
}

void logMessage(LogFn logFn, const String& message) {
    if (logFn != nullptr) {
        logFn(message);
    }
}

void setNtpDiagnostic(const String& code,
                      const String& message,
                      uint32_t atMs,
                      const String& server = "",
                      const String& address = "",
                      uint32_t roundTripMs = 0) {
    ntpDiagnostic.code = code;
    ntpDiagnostic.message = message;
    ntpDiagnostic.server = server;
    ntpDiagnostic.address = address;
    ntpDiagnostic.atMs = atMs;
    ntpDiagnostic.roundTripMs = roundTripMs;
}

void timeSyncNotification(struct timeval* tv) {
    (void)tv;
    syncNotificationPending.store(true, std::memory_order_release);
}

bool sendNtpProbe(const String& server,
                  IPAddress& resolvedAddress,
                  uint32_t& roundTripMs,
                  String& failureCode,
                  String& failureMessage) {
    resolvedAddress = IPAddress();
    roundTripMs = 0;
    failureCode = "";
    failureMessage = "";

    if (WiFi.hostByName(server.c_str(), resolvedAddress) != 1) {
        failureCode = "dns_failed";
        failureMessage = String("DNS failed for ") + server;
        return false;
    }

    WiFiUDP udp;
    if (!udp.begin(0)) {
        failureCode = "udp_error";
        failureMessage = "Failed to open UDP socket for NTP probe";
        return false;
    }

    uint8_t packet[48] = {0};
    packet[0] = 0x1B;
    packet[1] = 0;
    packet[2] = 6;
    packet[3] = 0xEC;
    packet[12] = 49;
    packet[13] = 0x4E;
    packet[14] = 49;
    packet[15] = 52;

    if (!udp.beginPacket(resolvedAddress, 123)) {
        udp.stop();
        failureCode = "udp_error";
        failureMessage = String("Failed to start UDP/123 probe for ") + server;
        return false;
    }
    if (udp.write(packet, sizeof(packet)) != sizeof(packet) || !udp.endPacket()) {
        udp.stop();
        failureCode = "udp_error";
        failureMessage = String("Failed to send UDP/123 probe to ") + server;
        return false;
    }

    const uint32_t startedAt = millis();
    while ((millis() - startedAt) < NTP_PROBE_TIMEOUT_MS) {
        const int packetSize = udp.parsePacket();
        if (packetSize >= 48) {
            uint8_t response[48];
            (void)udp.read(response, sizeof(response));
            udp.stop();
            roundTripMs = millis() - startedAt;
            return true;
        }
        delay(10);
    }

    udp.stop();
    failureCode = "udp_no_response";
    failureMessage = String("Resolved ") + server + " but no UDP/123 NTP reply arrived";
    return false;
}

NtpDiagnosticResult runNtpDiagnostics(const NtpDiagnosticRequest& request) {
    const String servers[] = {request.primary, request.secondary, request.tertiary};
    bool anyUdpError = false;
    String lastServer = "";
    String lastAddress = "";
    String lastFailureCode = "";
    String lastFailureMessage = "";

    for (const String& server : servers) {
        if (server.isEmpty()) {
            continue;
        }

        IPAddress resolvedAddress;
        uint32_t roundTripMs = 0;
        String failureCode;
        String failureMessage;
        if (sendNtpProbe(server, resolvedAddress, roundTripMs, failureCode, failureMessage)) {
            NtpDiagnosticResult result;
            result.code = "server_replied";
            result.message = "NTP server replied; waiting for system sync";
            result.server = server;
            result.address = resolvedAddress.toString();
            result.roundTripMs = roundTripMs;
            return result;
        }

        lastServer = server;
        lastAddress = resolvedAddress.toString();
        lastFailureCode = failureCode;
        lastFailureMessage = failureMessage;
        if (failureCode == "udp_no_response" || failureCode == "udp_error") {
            anyUdpError = true;
        }
    }

    NtpDiagnosticResult result;
    if (anyUdpError) {
        result.code = lastFailureCode.isEmpty() ? "udp_no_response" : lastFailureCode;
        result.message = lastFailureMessage.isEmpty()
                             ? String("Configured NTP servers resolved but did not answer on UDP/123")
                             : lastFailureMessage;
        result.server = lastServer;
        result.address = lastAddress;
        return result;
    }

    result.code = "dns_failed";
    result.message = "DNS failed for all configured NTP servers";
    result.server = lastServer;
    result.address = lastAddress;
    return result;
}

void ntpDiagnosticTask(void*) {
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        for (;;) {
            NtpDiagnosticRequest request;
            {
                StateLock lock;
                if (!ntpDiagnosticRequest.pending) {
                    ntpDiagnosticRunning = false;
                    break;
                }
                request = ntpDiagnosticRequest;
                ntpDiagnosticRequest.pending = false;
                ntpDiagnosticRunning = true;
            }

            // All DNS and UDP work is deliberately outside the state mutex and
            // outside the Arduino loop task.
            const NtpDiagnosticResult result = runNtpDiagnostics(request);
            const uint32_t completedAtMs = millis();
            bool published = false;
            {
                StateLock lock;
                ntpDiagnosticRunning = false;
                if (request.generation == stateGeneration) {
                    setNtpDiagnostic(result.code,
                                     result.message,
                                     completedAtMs,
                                     result.server,
                                     result.address,
                                     result.roundTripMs);
                    published = true;
                }
            }

            if (published) {
                if (result.code == "server_replied") {
                    logMessage(request.logFn,
                               String("NTP probe reply from ") + result.server + " (" + result.address +
                                   ") in " + result.roundTripMs + " ms");
                } else {
                    logMessage(request.logFn, result.message);
                }
            }
        }
    }
}

bool ensureNtpDiagnosticTask() {
    if (ntpDiagnosticTaskHandle != nullptr) {
        return true;
    }
    if (stateMutex == nullptr) {
        return false;
    }
    return xTaskCreatePinnedToCore(ntpDiagnosticTask,
                                   "ntp-diagnostic",
                                   NTP_DIAGNOSTIC_TASK_STACK_BYTES,
                                   nullptr,
                                   NTP_DIAGNOSTIC_TASK_PRIORITY,
                                   &ntpDiagnosticTaskHandle,
                                   NTP_DIAGNOSTIC_TASK_CORE) == pdPASS;
}

void enqueueNtpDiagnostics(const NtpDiagnosticRequest& request) {
    TaskHandle_t task = nullptr;
    {
        StateLock lock;
        if (request.generation != stateGeneration) {
            return;
        }
        // One fixed request slot is intentional: a newer retry supersedes a
        // queued retry, while a running probe is left alone.
        ntpDiagnosticRequest = request;
        ntpDiagnosticRequest.pending = true;
        task = ntpDiagnosticTaskHandle;
        if (task == nullptr) {
            setNtpDiagnostic("worker_unavailable",
                             "NTP diagnostic worker is unavailable",
                             request.requestedAtMs);
        }
    }
    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
}

void recordSuccessfulSync(time_t epoch, uint32_t nowMs, LogFn logFn) {
    if (!hasValidEpoch(epoch)) {
        return;
    }

    {
        StateLock lock;
        if (!ntpModeEnabled()) {
            return;
        }
        ++stateGeneration;
        ntpDiagnosticRequest.pending = false;
        lastSuccessMs = nowMs;
        restored = false;
        clientSeeded = false;
        lastSyncedEpoch = epoch;
        synced = true;
        setNtpDiagnostic("synced",
                         String("NTP synchronized successfully"),
                         nowMs,
                         ntpDiagnostic.server,
                         ntpDiagnostic.address,
                         ntpDiagnostic.roundTripMs);
    }
    cacheRtcEpoch(epoch);
    if (clockPrefsReady) {
        if (clockPrefs.putLong64(PREFS_LAST_NTP_UNIX, static_cast<int64_t>(epoch)) == 0) {
            logMessage(logFn, "Failed to persist last successful NTP UTC clock");
        }
    }
    logMessage(logFn, String("NTP synchronized at ") + formatIso8601Utc(epoch));
}

void cacheRtcEpoch(time_t epoch) {
    if (!hasValidEpoch(epoch)) {
        return;
    }
    rtcMagic = RTC_MAGIC;
    rtcEpoch = static_cast<int64_t>(epoch);
}

bool restoreEpoch(time_t epoch) {
    if (!hasValidEpoch(epoch)) {
        return false;
    }

    struct timeval tv;
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) {
        return false;
    }

    cacheRtcEpoch(epoch);
    {
        StateLock lock;
        restored = true;
        clientSeeded = false;
    }
    return true;
}

ConfigSnapshot loadConfigFromPrefs() {
    ConfigSnapshot loaded;
    loaded.mode = TIME_MODE_NTP;
    loaded.ntpServerPrimary = DEFAULT_NTP_SERVER_PRIMARY;
    loaded.ntpServerSecondary = DEFAULT_NTP_SERVER_SECONDARY;
    loaded.ntpServerTertiary = DEFAULT_NTP_SERVER_TERTIARY;
    if (!clockPrefsReady) {
        return loaded;
    }

    loaded.mode = normalizeMode(clockPrefs.getString(PREFS_TIME_MODE, TIME_MODE_NTP));
    loaded.ntpServerPrimary =
        sanitizeServerValue(clockPrefs.getString(PREFS_NTP_PRIMARY, DEFAULT_NTP_SERVER_PRIMARY),
                            DEFAULT_NTP_SERVER_PRIMARY);
    loaded.ntpServerSecondary =
        sanitizeServerValue(clockPrefs.getString(PREFS_NTP_SECONDARY, DEFAULT_NTP_SERVER_SECONDARY),
                            DEFAULT_NTP_SERVER_SECONDARY);
    loaded.ntpServerTertiary =
        sanitizeServerValue(clockPrefs.getString(PREFS_NTP_TERTIARY, DEFAULT_NTP_SERVER_TERTIARY),
                            DEFAULT_NTP_SERVER_TERTIARY);
    return loaded;
}

} // namespace

void begin(LogFn logFn) {
    ensureStateMutex();
    const bool workerReady = ensureNtpDiagnosticTask();
    {
        StateLock lock;
        ++stateGeneration;
        configured = false;
        synced = false;
        restored = false;
        clientSeeded = false;
        staConnectedLastTick = false;
        lastAttemptMs = 0;
        lastSuccessMs = 0;
        lastPollMs = 0;
        lastSyncedEpoch = 0;
        ntpDiagnosticRequest.pending = false;
        setNtpDiagnostic(workerReady ? "idle" : "worker_unavailable",
                         workerReady ? "Waiting for Wi-Fi to request NTP"
                                     : "NTP diagnostic worker is unavailable",
                         0);
    }
    syncNotificationPending.store(false, std::memory_order_release);

    if (clockPrefsReady) {
        clockPrefs.end();
        clockPrefsReady = false;
    }

    clockPrefsReady = clockPrefs.begin(PREFS_NAMESPACE, false);
    const ConfigSnapshot loadedConfig = loadConfigFromPrefs();
    time_t loadedLastSyncedEpoch =
        clockPrefsReady ? static_cast<time_t>(clockPrefs.getLong64(PREFS_LAST_NTP_UNIX, 0))
                        : static_cast<time_t>(0);
    if (!hasValidEpoch(loadedLastSyncedEpoch)) {
        loadedLastSyncedEpoch = 0;
    }
    {
        StateLock lock;
        timeMode = loadedConfig.mode;
        ntpServerPrimary = loadedConfig.ntpServerPrimary;
        ntpServerSecondary = loadedConfig.ntpServerSecondary;
        ntpServerTertiary = loadedConfig.ntpServerTertiary;
        lastSyncedEpoch = loadedLastSyncedEpoch;
    }
    esp_sntp_set_time_sync_notification_cb(timeSyncNotification);
    const time_t rtcSavedEpoch = (rtcMagic == RTC_MAGIC) ? static_cast<time_t>(rtcEpoch) : static_cast<time_t>(0);
    if (restoreEpoch(rtcSavedEpoch)) {
        logMessage(logFn, String("Restored warm-reboot UTC clock at ") + formatIso8601Utc(rtcSavedEpoch));
    }
    {
        StateLock lock;
        if (!ntpModeEnabled()) {
            setNtpDiagnostic("disabled", "NTP is disabled in no time mode", 0);
        } else if (hasValidEpoch(lastSyncedEpoch)) {
            setNtpDiagnostic("idle",
                             String("Waiting for the next NTP sync attempt"),
                             0,
                             "",
                             "",
                             0);
        }
    }
}

ConfigSnapshot config() {
    StateLock lock;
    ConfigSnapshot snapshot;
    snapshot.mode = timeMode;
    snapshot.ntpServerPrimary = ntpServerPrimary;
    snapshot.ntpServerSecondary = ntpServerSecondary;
    snapshot.ntpServerTertiary = ntpServerTertiary;
    return snapshot;
}

bool saveConfig(const String& mode,
                const String& primary,
                const String& secondary,
                const String& tertiary,
                String& error,
                LogFn logFn) {
    error = "";
    const String normalizedMode = normalizeMode(mode);
    const String nextPrimary = sanitizeServerValue(primary, DEFAULT_NTP_SERVER_PRIMARY);
    const String nextSecondary = sanitizeServerValue(secondary, DEFAULT_NTP_SERVER_SECONDARY);
    const String nextTertiary = sanitizeServerValue(tertiary, DEFAULT_NTP_SERVER_TERTIARY);

    if (!clockPrefsReady) {
        clockPrefsReady = clockPrefs.begin(PREFS_NAMESPACE, false);
        if (!clockPrefsReady) {
            error = "failed to open time preferences";
            return false;
        }
    }

    if (clockPrefs.putString(PREFS_TIME_MODE, normalizedMode) == 0 ||
        clockPrefs.putString(PREFS_NTP_PRIMARY, nextPrimary) == 0 ||
        clockPrefs.putString(PREFS_NTP_SECONDARY, nextSecondary) == 0 ||
        clockPrefs.putString(PREFS_NTP_TERTIARY, nextTertiary) == 0) {
        error = "failed to persist time configuration";
        return false;
    }

    {
        StateLock lock;
        ++stateGeneration;
        timeMode = normalizedMode;
        ntpServerPrimary = nextPrimary;
        ntpServerSecondary = nextSecondary;
        ntpServerTertiary = nextTertiary;
        configured = false;
        lastAttemptMs = 0;
        ntpDiagnosticRequest.pending = false;
        if (timeMode == TIME_MODE_NTP) {
            synced = false;
            setNtpDiagnostic("idle", "Waiting for Wi-Fi to request NTP", millis());
        } else {
            setNtpDiagnostic("disabled", "NTP is disabled in no time mode", millis());
        }
    }
    syncNotificationPending.store(false, std::memory_order_release);
    if (normalizedMode == TIME_MODE_NO_TIME && esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    if (normalizedMode == TIME_MODE_NO_TIME) {
        logMessage(logFn, "Updated time mode to no_time (client-seeded clock only)");
    } else {
        logMessage(logFn, String("Updated time mode to ntp with servers: ") +
                           nextPrimary + ", " + nextSecondary + ", " + nextTertiary);
    }
    return true;
}

bool seedFromUnixTime(time_t epoch, uint32_t nowMs, LogFn logFn) {
    if (!hasValidEpoch(epoch)) {
        return false;
    }
    uint32_t generation = 0;
    {
        StateLock lock;
        if (timeMode != TIME_MODE_NO_TIME) {
            return false;
        }
        generation = stateGeneration;
    }

    const time_t currentEpoch = time(nullptr);
    {
        StateLock lock;
        if (synced && hasValidEpoch(currentEpoch)) {
            return true;
        }
    }
    if (hasValidEpoch(currentEpoch)) {
        const int64_t deltaSeconds = llabs(static_cast<int64_t>(epoch) - static_cast<int64_t>(currentEpoch));
        if (deltaSeconds <= 2) {
            cacheRtcEpoch(currentEpoch);
            return true;
        }
    }

    struct timeval tv;
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) {
        return false;
    }

    cacheRtcEpoch(epoch);
    {
        StateLock lock;
        if (generation != stateGeneration || timeMode != TIME_MODE_NO_TIME) {
            return false;
        }
        restored = false;
        clientSeeded = true;
    }
    logMessage(logFn, String("Seeded UTC clock from client request at ") + formatIso8601Utc(epoch));
    return true;
}

void persist(uint32_t nowMs, LogFn logFn) {
    (void)nowMs;
    (void)logFn;
    const time_t epoch = time(nullptr);
    if (!hasValidEpoch(epoch)) {
        return;
    }

    cacheRtcEpoch(epoch);
}

void tick(bool staConnected, uint32_t nowMs, LogFn logFn) {
    const time_t currentEpoch = time(nullptr);
    if (hasValidEpoch(currentEpoch)) {
        cacheRtcEpoch(currentEpoch);
    }
    if (syncNotificationPending.exchange(false, std::memory_order_acq_rel)) {
        recordSuccessfulSync(time(nullptr), nowMs, logFn);
    }

    NtpDiagnosticRequest diagnosticRequest;
    String syncReason;
    bool requestSyncNow = false;
    bool pollClock = false;
    bool gainedConnectivity = false;
    {
        StateLock lock;
        gainedConnectivity = staConnected && !staConnectedLastTick;
        const bool connectivityChanged = staConnected != staConnectedLastTick;
        staConnectedLastTick = staConnected;
        if (connectivityChanged) {
            ++stateGeneration;
            ntpDiagnosticRequest.pending = false;
        }

        if (!staConnected) {
            if (ntpModeEnabled() && ntpDiagnostic.code != "idle") {
                setNtpDiagnostic("idle", "Waiting for Wi-Fi before requesting NTP", nowMs);
            }
            return;
        }
        if (!ntpModeEnabled()) {
            if (ntpDiagnostic.code != "disabled") {
                setNtpDiagnostic("disabled", "NTP is disabled in no time mode", nowMs);
            }
            return;
        }

        if (gainedConnectivity) {
            configured = false;
            synced = false;
            setNtpDiagnostic("pending", "Wi-Fi connected; requesting NTP", nowMs);
        }

        const detail::SyncDecision decision = detail::decideSync(staConnected,
                                                                  gainedConnectivity,
                                                                  true,
                                                                  configured,
                                                                  synced,
                                                                  nowMs,
                                                                  lastAttemptMs,
                                                                  NTP_RETRY_INTERVAL_MS);
        if (decision != detail::SyncDecision::none) {
            configured = true;
            lastAttemptMs = nowMs;
            requestSyncNow = true;
            diagnosticRequest.generation = stateGeneration;
            diagnosticRequest.requestedAtMs = nowMs;
            diagnosticRequest.primary = ntpServerPrimary;
            diagnosticRequest.secondary = ntpServerSecondary;
            diagnosticRequest.tertiary = ntpServerTertiary;
            diagnosticRequest.logFn = logFn;
            if (decision == detail::SyncDecision::connectivity_gained) {
                syncReason = "Requested NTP sync after Wi-Fi connect";
            } else if (decision == detail::SyncDecision::retry) {
                syncReason = "Retrying NTP sync";
            } else {
                syncReason = "Requested initial NTP sync (UTC)";
            }
        }

        pollClock = gainedConnectivity || detail::intervalElapsed(nowMs, lastPollMs, CLOCK_POLL_INTERVAL_MS);
        if (pollClock) {
            lastPollMs = nowMs;
        }
    }

    if (requestSyncNow) {
        // ESP-IDF's SNTP client resolves and communicates asynchronously.
        configTzTime(NTP_TZ,
                     diagnosticRequest.primary.c_str(),
                     diagnosticRequest.secondary.c_str(),
                     diagnosticRequest.tertiary.c_str());
        logMessage(logFn, syncReason);
        enqueueNtpDiagnostics(diagnosticRequest);
    }

    if (!pollClock) {
        return;
    }

    const time_t epoch = time(nullptr);
    if (!hasValidEpoch(epoch)) {
        return;
    }

    cacheRtcEpoch(epoch);
    const sntp_sync_status_t syncStatus = sntp_get_sync_status();
    bool alreadySynced = false;
    {
        StateLock lock;
        alreadySynced = synced;
    }
    if (!alreadySynced && syncStatus == SNTP_SYNC_STATUS_COMPLETED) {
        recordSuccessfulSync(epoch, nowMs, logFn);
        return;
    }

    if (!alreadySynced && syncStatus == SNTP_SYNC_STATUS_IN_PROGRESS) {
        StateLock lock;
        setNtpDiagnostic("sync_in_progress", "SNTP reported that time adjustment is still in progress", nowMs);
    }
}

StatusSnapshot snapshot() {
    StatusSnapshot status;
    {
        StateLock lock;
        status.configured = configured;
        status.synced = synced;
        status.lastAttemptMs = lastAttemptMs;
        status.lastSuccessMs = lastSuccessMs;
        status.ntpDiagnosticAtMs = ntpDiagnostic.atMs;
        status.ntpDiagnosticRoundTripMs = ntpDiagnostic.roundTripMs;
        status.ntpDiagnosticPending = ntpDiagnosticRequest.pending;
        status.ntpDiagnosticRunning = ntpDiagnosticRunning;
        status.ntpDiagnosticStackHighWaterBytes = ntpDiagnosticTaskHandle == nullptr
                                                      ? 0
                                                      : uxTaskGetStackHighWaterMark(ntpDiagnosticTaskHandle);
        status.ntpDiagnosticCode = ntpDiagnostic.code;
        status.ntpDiagnosticMessage = ntpDiagnostic.message;
        status.ntpDiagnosticServer = ntpDiagnostic.server;
        status.ntpDiagnosticAddress = ntpDiagnostic.address;
        if (hasValidEpoch(lastSyncedEpoch)) {
            status.lastSuccessUnix = lastSyncedEpoch;
            status.lastSuccessIsoUtc = formatIso8601Utc(lastSyncedEpoch);
        }
        status.restored = restored && !synced;
        status.clientSeeded = clientSeeded && !synced;
    }

    const time_t epoch = time(nullptr);
    status.available = hasValidEpoch(epoch);
    status.restored = status.available && status.restored;
    status.clientSeeded = status.available && status.clientSeeded;
    if (status.available) {
        status.unixTime = epoch;
        status.iso8601Utc = formatIso8601Utc(epoch);
    }

    return status;
}

} // namespace bridge_time
