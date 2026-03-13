#pragma once

#include <Arduino.h>

#include <ctime>

namespace bridge_time {

struct StatusSnapshot {
    bool configured{false};
    bool synced{false};
    uint32_t lastAttemptMs{0};
    uint32_t lastSuccessMs{0};
    time_t unixTime{0};
    String iso8601Utc;
};

using LogFn = void (*)(const String& message);

void begin();
void tick(bool staConnected, uint32_t nowMs, LogFn logFn = nullptr);
StatusSnapshot snapshot();

} // namespace bridge_time
