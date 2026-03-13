#include "bridge_time.h"

#include <time.h>

namespace bridge_time {

namespace {

constexpr char NTP_TZ[] = "UTC0";
constexpr char NTP_SERVER_PRIMARY[] = "pool.ntp.org";
constexpr char NTP_SERVER_SECONDARY[] = "time.google.com";
constexpr char NTP_SERVER_TERTIARY[] = "time.cloudflare.com";
constexpr time_t MIN_VALID_EPOCH = 1704067200; // 2024-01-01T00:00:00Z
constexpr uint32_t NTP_RETRY_INTERVAL_MS = 60000;
constexpr uint32_t CLOCK_POLL_INTERVAL_MS = 1000;

bool configured = false;
bool synced = false;
bool staConnectedLastTick = false;
uint32_t lastAttemptMs = 0;
uint32_t lastSuccessMs = 0;
uint32_t lastPollMs = 0;

bool hasValidEpoch(time_t epoch) {
    return epoch >= MIN_VALID_EPOCH;
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

void requestSync(uint32_t nowMs, LogFn logFn, const String& reason) {
    configTzTime(NTP_TZ, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY, NTP_SERVER_TERTIARY);
    configured = true;
    lastAttemptMs = nowMs;
    logMessage(logFn, reason);
}

} // namespace

void begin() {
    configured = false;
    synced = false;
    staConnectedLastTick = false;
    lastAttemptMs = 0;
    lastSuccessMs = 0;
    lastPollMs = 0;
}

void tick(bool staConnected, uint32_t nowMs, LogFn logFn) {
    const bool gainedConnectivity = staConnected && !staConnectedLastTick;
    staConnectedLastTick = staConnected;

    if (!staConnected) {
        return;
    }

    if (!configured) {
        requestSync(nowMs, logFn, "Requested initial NTP sync (UTC)");
    } else if (!synced && gainedConnectivity) {
        requestSync(nowMs, logFn, "Retrying NTP sync after Wi-Fi reconnect");
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

    if (!synced) {
        lastSuccessMs = nowMs;
        logMessage(logFn, String("NTP synchronized at ") + formatIso8601Utc(epoch));
    }
    synced = true;
}

StatusSnapshot snapshot() {
    StatusSnapshot status;
    status.configured = configured;
    status.lastAttemptMs = lastAttemptMs;
    status.lastSuccessMs = lastSuccessMs;

    const time_t epoch = time(nullptr);
    status.synced = hasValidEpoch(epoch);
    if (status.synced) {
        status.unixTime = epoch;
        status.iso8601Utc = formatIso8601Utc(epoch);
    }

    return status;
}

} // namespace bridge_time
