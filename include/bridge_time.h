#pragma once

#include <Arduino.h>

#include <ctime>

namespace bridge_time {

struct ConfigSnapshot {
    String mode;
    String ntpServerPrimary;
    String ntpServerSecondary;
    String ntpServerTertiary;
};

struct StatusSnapshot {
    bool configured{false};
    bool available{false};
    bool synced{false};
    bool restored{false};
    bool clientSeeded{false};
    uint32_t lastAttemptMs{0};
    uint32_t lastSuccessMs{0};
    uint32_t ntpDiagnosticAtMs{0};
    uint32_t ntpDiagnosticRoundTripMs{0};
    time_t lastSuccessUnix{0};
    time_t unixTime{0};
    String ntpDiagnosticCode;
    String ntpDiagnosticMessage;
    String ntpDiagnosticServer;
    String ntpDiagnosticAddress;
    String lastSuccessIsoUtc;
    String iso8601Utc;
};

using LogFn = void (*)(const String& message);

void begin(LogFn logFn = nullptr);
ConfigSnapshot config();
bool saveConfig(const String& mode,
                const String& ntpServerPrimary,
                const String& ntpServerSecondary,
                const String& ntpServerTertiary,
                String& error,
                LogFn logFn = nullptr);
bool seedFromUnixTime(time_t unixTime, uint32_t nowMs, LogFn logFn = nullptr);
void persist(uint32_t nowMs, LogFn logFn = nullptr);
void tick(bool staConnected, uint32_t nowMs, LogFn logFn = nullptr);
StatusSnapshot snapshot();

} // namespace bridge_time
