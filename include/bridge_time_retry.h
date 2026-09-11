#pragma once

#include <cstdint>

namespace bridge_time::detail {

enum class SyncDecision : uint8_t {
    none,
    initial,
    connectivity_gained,
    retry,
};

// Unsigned subtraction intentionally makes this safe across millis() rollover.
constexpr bool intervalElapsed(uint32_t nowMs, uint32_t sinceMs, uint32_t intervalMs) {
    return static_cast<uint32_t>(nowMs - sinceMs) >= intervalMs;
}

constexpr SyncDecision decideSync(bool staConnected,
                                  bool gainedConnectivity,
                                  bool ntpEnabled,
                                  bool configured,
                                  bool synced,
                                  uint32_t nowMs,
                                  uint32_t lastAttemptMs,
                                  uint32_t retryIntervalMs) {
    if (!staConnected || !ntpEnabled) {
        return SyncDecision::none;
    }
    if (gainedConnectivity) {
        return SyncDecision::connectivity_gained;
    }
    if (!configured) {
        return SyncDecision::initial;
    }
    if (!synced && intervalElapsed(nowMs, lastAttemptMs, retryIntervalMs)) {
        return SyncDecision::retry;
    }
    return SyncDecision::none;
}

} // namespace bridge_time::detail
