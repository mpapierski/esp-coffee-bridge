#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

namespace bridge_runtime_policy {

// Unsigned subtraction keeps duration checks correct across millis() rollover.
constexpr bool elapsedAtLeast(uint32_t nowMs, uint32_t sinceMs, uint32_t durationMs) {
    return static_cast<uint32_t>(nowMs - sinceMs) >= durationMs;
}

// Absolute deadlines used by the bridge are always less than 2^31 ms away.
// Signed subtraction therefore provides a rollover-safe before/after test.
constexpr bool deadlineReached(uint32_t nowMs, uint32_t deadlineAtMs) {
    return static_cast<int32_t>(nowMs - deadlineAtMs) >= 0;
}

constexpr uint32_t deadlineRemaining(uint32_t nowMs, uint32_t deadlineAtMs) {
    const int32_t remaining = static_cast<int32_t>(deadlineAtMs - nowMs);
    return remaining > 0 ? static_cast<uint32_t>(remaining) : 0U;
}

constexpr uint32_t clampWaitToDeadline(uint32_t requestedMs,
                                       uint32_t maximumWaitMs,
                                       uint32_t nowMs,
                                       uint32_t deadlineAtMs) {
    return std::min(std::min(requestedMs, maximumWaitMs), deadlineRemaining(nowMs, deadlineAtMs));
}

constexpr bool watchdogShouldReboot(bool jobActive,
                                    bool transportCallActive,
                                    uint32_t nowMs,
                                    uint32_t heartbeatAtMs,
                                    uint32_t stallTimeoutMs) {
    return jobActive && transportCallActive && elapsedAtLeast(nowMs, heartbeatAtMs, stallTimeoutMs);
}

constexpr bool retainWorkerResultFile(bool resource,
                                      bool spoolResult,
                                      int responseStatus) {
    return responseStatus < 500 && (resource || spoolResult);
}

struct CacheTiming {
    bool hasPayload{false};
    uint32_t sampledAtMs{0};
    uint32_t ttlMs{0};
    uint32_t retryAfterMs{0};
};

struct CacheDecision {
    bool servePayload{false};
    bool stale{false};
    bool enqueueRefresh{false};
    bool rejectColdForBackoff{false};
};

constexpr bool retryBackoffActive(const CacheTiming& cache, uint32_t nowMs) {
    return cache.retryAfterMs != 0 && !deadlineReached(nowMs, cache.retryAfterMs);
}

constexpr CacheDecision decideCacheRequest(const CacheTiming& cache,
                                           uint32_t nowMs,
                                           bool forceRefresh) {
    if (forceRefresh) {
        return {false, false, true, false};
    }

    const bool fresh = cache.hasPayload &&
        !elapsedAtLeast(nowMs, cache.sampledAtMs, cache.ttlMs);
    const bool backingOff = retryBackoffActive(cache, nowMs);
    if (cache.hasPayload) {
        return {true, !fresh, !fresh && !backingOff, false};
    }
    if (backingOff) {
        return {false, false, false, true};
    }
    return {false, false, true, false};
}

// A failed refresh changes only retry metadata. In particular, it must not
// discard a previously published snapshot or alter its sample timestamp.
constexpr CacheTiming noteCacheRefreshFailure(CacheTiming cache,
                                               uint32_t nowMs,
                                               uint32_t backoffMs) {
    cache.retryAfterMs = nowMs + backoffMs;
    return cache;
}

// Compare only fields written by schema 2. Volatile presence and the
// boot-scoped generation intentionally do not participate in this equality.
template <typename Machine>
bool durableMachineFieldsEqual(const Machine& left, const Machine& right) {
    return left.serial == right.serial &&
           left.alias == right.alias &&
           left.address == right.address &&
           left.addressType == right.addressType &&
           left.manufacturer == right.manufacturer &&
           left.model == right.model &&
           left.modelCode == right.modelCode &&
           left.modelName == right.modelName &&
           left.familyKey == right.familyKey &&
           left.hardwareRevision == right.hardwareRevision &&
           left.firmwareRevision == right.firmwareRevision &&
           left.softwareRevision == right.softwareRevision &&
           left.ad06Hex == right.ad06Hex &&
           left.ad06Ascii == right.ad06Ascii &&
           left.savedAtMs == right.savedAtMs;
}

enum class TransportOutcome : uint8_t {
    Success,
    Timeout,
    Failure,
};

struct SessionSnapshot {
    bool clientCreated{false};
    bool connected{false};
    bool handlesDiscovered{false};
    bool huSessionReady{false};
    bool recreateBeforeNextJob{false};
    std::string target;
};

// Small, transport-agnostic state machine for the worker's reuse contract.
// Transport must expose createClient(), connect(target), discoverHandles(),
// establishHuSession(), read(operation), disconnect(), and destroyClient().
template <typename Transport>
class ReusableSession {
public:
    template <typename Iterator>
    TransportOutcome runReadJob(Transport& transport,
                                const std::string& target,
                                bool requireHuSession,
                                Iterator first,
                                Iterator last) {
        const TransportOutcome prepared = prepare(transport, target, requireHuSession);
        if (prepared != TransportOutcome::Success) {
            invalidateAfterFailure();
            return prepared;
        }

        for (; first != last; ++first) {
            const TransportOutcome outcome = transport.read(*first);
            if (outcome != TransportOutcome::Success) {
                // Stop immediately. The next job, rather than this call stack,
                // owns teardown and client recreation.
                invalidateAfterFailure();
                return outcome;
            }
        }
        return TransportOutcome::Success;
    }

    const SessionSnapshot& snapshot() const {
        return state_;
    }

private:
    TransportOutcome prepare(Transport& transport,
                             const std::string& target,
                             bool requireHuSession) {
        if (state_.recreateBeforeNextJob) {
            if (state_.clientCreated) {
                transport.destroyClient();
            }
            state_ = {};
        }

        if (state_.connected && state_.target != target) {
            transport.disconnect();
            state_.connected = false;
            state_.handlesDiscovered = false;
            state_.huSessionReady = false;
        }
        state_.target = target;

        if (!state_.clientCreated) {
            const TransportOutcome outcome = transport.createClient();
            if (outcome != TransportOutcome::Success) {
                return outcome;
            }
            state_.clientCreated = true;
        }
        if (!state_.connected) {
            const TransportOutcome outcome = transport.connect(target);
            if (outcome != TransportOutcome::Success) {
                return outcome;
            }
            state_.connected = true;
        }
        if (!state_.handlesDiscovered) {
            const TransportOutcome outcome = transport.discoverHandles();
            if (outcome != TransportOutcome::Success) {
                return outcome;
            }
            state_.handlesDiscovered = true;
        }
        if (requireHuSession && !state_.huSessionReady) {
            const TransportOutcome outcome = transport.establishHuSession();
            if (outcome != TransportOutcome::Success) {
                return outcome;
            }
            state_.huSessionReady = true;
        }
        return TransportOutcome::Success;
    }

    void invalidateAfterFailure() {
        state_.recreateBeforeNextJob = true;
        state_.connected = false;
        state_.handlesDiscovered = false;
        state_.huSessionReady = false;
    }

    SessionSnapshot state_{};
};

} // namespace bridge_runtime_policy
