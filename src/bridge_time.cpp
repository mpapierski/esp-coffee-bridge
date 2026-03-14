#include "bridge_time.h"

#include <algorithm>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_sntp.h>
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

struct NtpDiagnosticState {
    String code;
    String message;
    String server;
    String address;
    uint32_t atMs{0};
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
volatile bool syncNotificationPending = false;
NtpDiagnosticState ntpDiagnostic;
Preferences clockPrefs;
bool clockPrefsReady = false;

RTC_DATA_ATTR uint32_t rtcMagic = 0;
RTC_DATA_ATTR int64_t rtcEpoch = 0;

void cacheRtcEpoch(time_t epoch);

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
    syncNotificationPending = true;
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

void runNtpDiagnostics(uint32_t nowMs, LogFn logFn) {
    const String servers[] = {ntpServerPrimary, ntpServerSecondary, ntpServerTertiary};
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
            setNtpDiagnostic("server_replied",
                             String("NTP server replied; waiting for system sync"),
                             nowMs,
                             server,
                             resolvedAddress.toString(),
                             roundTripMs);
            logMessage(logFn, String("NTP probe reply from ") + server + " (" + resolvedAddress.toString() +
                                   ") in " + roundTripMs + " ms");
            return;
        }

        lastServer = server;
        lastAddress = resolvedAddress.toString();
        lastFailureCode = failureCode;
        lastFailureMessage = failureMessage;
        if (failureCode == "udp_no_response" || failureCode == "udp_error") {
            anyUdpError = true;
        }
    }

    if (anyUdpError) {
        setNtpDiagnostic(lastFailureCode.isEmpty() ? "udp_no_response" : lastFailureCode,
                         lastFailureMessage.isEmpty() ? String("Configured NTP servers resolved but did not answer on UDP/123")
                                                      : lastFailureMessage,
                         nowMs,
                         lastServer,
                         lastAddress);
        logMessage(logFn, ntpDiagnostic.message);
        return;
    }

    setNtpDiagnostic("dns_failed",
                     String("DNS failed for all configured NTP servers"),
                     nowMs,
                     lastServer,
                     lastAddress);
    logMessage(logFn, ntpDiagnostic.message);
}

void recordSuccessfulSync(time_t epoch, uint32_t nowMs, LogFn logFn) {
    if (!hasValidEpoch(epoch)) {
        return;
    }

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
    restored = true;
    clientSeeded = false;
    return true;
}

void requestSync(uint32_t nowMs, LogFn logFn, const String& reason) {
    configTzTime(NTP_TZ, ntpServerPrimary.c_str(), ntpServerSecondary.c_str(), ntpServerTertiary.c_str());
    configured = true;
    lastAttemptMs = nowMs;
    logMessage(logFn, reason);
    runNtpDiagnostics(nowMs, logFn);
}

void loadConfigFromPrefs() {
    timeMode = String(TIME_MODE_NTP);
    ntpServerPrimary = DEFAULT_NTP_SERVER_PRIMARY;
    ntpServerSecondary = DEFAULT_NTP_SERVER_SECONDARY;
    ntpServerTertiary = DEFAULT_NTP_SERVER_TERTIARY;
    if (!clockPrefsReady) {
        return;
    }

    timeMode = normalizeMode(clockPrefs.getString(PREFS_TIME_MODE, TIME_MODE_NTP));
    ntpServerPrimary = sanitizeServerValue(clockPrefs.getString(PREFS_NTP_PRIMARY, DEFAULT_NTP_SERVER_PRIMARY),
                                           DEFAULT_NTP_SERVER_PRIMARY);
    ntpServerSecondary = sanitizeServerValue(clockPrefs.getString(PREFS_NTP_SECONDARY, DEFAULT_NTP_SERVER_SECONDARY),
                                             DEFAULT_NTP_SERVER_SECONDARY);
    ntpServerTertiary = sanitizeServerValue(clockPrefs.getString(PREFS_NTP_TERTIARY, DEFAULT_NTP_SERVER_TERTIARY),
                                            DEFAULT_NTP_SERVER_TERTIARY);
}

} // namespace

void begin(LogFn logFn) {
    configured = false;
    synced = false;
    restored = false;
    clientSeeded = false;
    staConnectedLastTick = false;
    lastAttemptMs = 0;
    lastSuccessMs = 0;
    lastPollMs = 0;
    lastSyncedEpoch = 0;
    syncNotificationPending = false;
    setNtpDiagnostic("idle", "Waiting for Wi-Fi to request NTP", 0);

    if (clockPrefsReady) {
        clockPrefs.end();
        clockPrefsReady = false;
    }

    clockPrefsReady = clockPrefs.begin(PREFS_NAMESPACE, false);
    loadConfigFromPrefs();
    lastSyncedEpoch = clockPrefsReady ? static_cast<time_t>(clockPrefs.getLong64(PREFS_LAST_NTP_UNIX, 0))
                                      : static_cast<time_t>(0);
    if (!hasValidEpoch(lastSyncedEpoch)) {
        lastSyncedEpoch = 0;
    }
    esp_sntp_set_time_sync_notification_cb(timeSyncNotification);
    const time_t rtcSavedEpoch = (rtcMagic == RTC_MAGIC) ? static_cast<time_t>(rtcEpoch) : static_cast<time_t>(0);
    if (restoreEpoch(rtcSavedEpoch)) {
        logMessage(logFn, String("Restored warm-reboot UTC clock at ") + formatIso8601Utc(rtcSavedEpoch));
    }
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

ConfigSnapshot config() {
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

    timeMode = normalizedMode;
    ntpServerPrimary = nextPrimary;
    ntpServerSecondary = nextSecondary;
    ntpServerTertiary = nextTertiary;
    configured = false;
    syncNotificationPending = false;
    lastAttemptMs = 0;
    if (!ntpModeEnabled() && esp_sntp_enabled()) {
        esp_sntp_stop();
    } else if (ntpModeEnabled()) {
        synced = false;
    }

    if (timeMode == TIME_MODE_NO_TIME) {
        setNtpDiagnostic("disabled", "NTP is disabled in no time mode", millis());
        logMessage(logFn, "Updated time mode to no_time (client-seeded clock only)");
    } else {
        setNtpDiagnostic("idle", "Waiting for Wi-Fi to request NTP", millis());
        logMessage(logFn, String("Updated time mode to ntp with servers: ") +
                           ntpServerPrimary + ", " + ntpServerSecondary + ", " + ntpServerTertiary);
    }
    return true;
}

bool seedFromUnixTime(time_t epoch, uint32_t nowMs, LogFn logFn) {
    if (!hasValidEpoch(epoch)) {
        return false;
    }
    if (timeMode != TIME_MODE_NO_TIME) {
        return false;
    }

    const time_t currentEpoch = time(nullptr);
    if (synced && hasValidEpoch(currentEpoch)) {
        return true;
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
    restored = false;
    clientSeeded = true;
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
    if (syncNotificationPending) {
        syncNotificationPending = false;
        recordSuccessfulSync(time(nullptr), nowMs, logFn);
    }

    const bool gainedConnectivity = staConnected && !staConnectedLastTick;
    staConnectedLastTick = staConnected;

    if (!staConnected) {
        if (ntpModeEnabled() && ntpDiagnostic.code != "disabled") {
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

    if (!configured) {
        requestSync(nowMs, logFn, gainedConnectivity ? "Requested NTP sync after Wi-Fi connect"
                                                     : "Requested initial NTP sync (UTC)");
    } else if (!synced && (nowMs - lastAttemptMs) >= NTP_RETRY_INTERVAL_MS) {
        requestSync(nowMs, logFn, "Retrying NTP sync");
    }

    if ((nowMs - lastPollMs) < CLOCK_POLL_INTERVAL_MS && !gainedConnectivity) {
        return;
    }
    lastPollMs = nowMs;

    const time_t epoch = time(nullptr);
    if (!hasValidEpoch(epoch)) {
        return;
    }

    cacheRtcEpoch(epoch);
    const sntp_sync_status_t syncStatus = sntp_get_sync_status();
    if (!synced && syncStatus == SNTP_SYNC_STATUS_COMPLETED) {
        recordSuccessfulSync(epoch, nowMs, logFn);
        return;
    }

    if (!synced && syncStatus == SNTP_SYNC_STATUS_IN_PROGRESS) {
        setNtpDiagnostic("sync_in_progress", "SNTP reported that time adjustment is still in progress", nowMs);
    }
}

StatusSnapshot snapshot() {
    StatusSnapshot status;
    status.configured = configured;
    status.synced = synced;
    status.lastAttemptMs = lastAttemptMs;
    status.lastSuccessMs = lastSuccessMs;
    status.ntpDiagnosticAtMs = ntpDiagnostic.atMs;
    status.ntpDiagnosticRoundTripMs = ntpDiagnostic.roundTripMs;
    status.ntpDiagnosticCode = ntpDiagnostic.code;
    status.ntpDiagnosticMessage = ntpDiagnostic.message;
    status.ntpDiagnosticServer = ntpDiagnostic.server;
    status.ntpDiagnosticAddress = ntpDiagnostic.address;
    if (hasValidEpoch(lastSyncedEpoch)) {
        status.lastSuccessUnix = lastSyncedEpoch;
        status.lastSuccessIsoUtc = formatIso8601Utc(lastSyncedEpoch);
    }

    const time_t epoch = time(nullptr);
    status.available = hasValidEpoch(epoch);
    status.restored = status.available && restored && !synced;
    status.clientSeeded = status.available && clientSeeded && !synced;
    if (status.available) {
        status.unixTime = epoch;
        status.iso8601Utc = formatIso8601Utc(epoch);
    }

    return status;
}

} // namespace bridge_time
