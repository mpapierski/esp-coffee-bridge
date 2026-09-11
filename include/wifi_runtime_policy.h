#pragma once

#include <cstdint>

namespace wifi_runtime_policy {

constexpr bool shouldRunSetupAccessPoint(bool configured) {
    return !configured;
}

// Absolute retry deadlines are always less than 2^31 ms away. Signed
// subtraction keeps this decision correct across millis() rollover.
constexpr bool shouldStartStationAttempt(bool configured,
                                         bool connected,
                                         bool attemptActive,
                                         uint32_t nowMs,
                                         uint32_t retryAtMs) {
    return configured && !connected && !attemptActive &&
        (retryAtMs == 0 || static_cast<int32_t>(nowMs - retryAtMs) >= 0);
}

} // namespace wifi_runtime_policy
