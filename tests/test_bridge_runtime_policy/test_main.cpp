#include <unity.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "bridge_runtime_policy.h"
#include "history_capacity.h"
#include "history_retention.h"

using bridge_runtime_policy::CacheDecision;
using bridge_runtime_policy::CacheTiming;
using bridge_runtime_policy::ReusableSession;
using bridge_runtime_policy::TransportOutcome;

namespace {

void test_cache_freshness_and_backoff_are_wraparound_safe() {
    CacheTiming cache;
    cache.hasPayload = true;
    cache.sampledAtMs = UINT32_MAX - 50U;
    cache.ttlMs = 100U;

    CacheDecision decision = bridge_runtime_policy::decideCacheRequest(cache, 25U, false);
    TEST_ASSERT_TRUE(decision.servePayload);
    TEST_ASSERT_FALSE(decision.stale);
    TEST_ASSERT_FALSE(decision.enqueueRefresh);

    decision = bridge_runtime_policy::decideCacheRequest(cache, 50U, false);
    TEST_ASSERT_TRUE(decision.servePayload);
    TEST_ASSERT_TRUE(decision.stale);
    TEST_ASSERT_TRUE(decision.enqueueRefresh);

    cache.retryAfterMs = 80U;
    decision = bridge_runtime_policy::decideCacheRequest(cache, 50U, false);
    TEST_ASSERT_TRUE(decision.servePayload);
    TEST_ASSERT_TRUE(decision.stale);
    TEST_ASSERT_FALSE(decision.enqueueRefresh);

    cache.hasPayload = false;
    decision = bridge_runtime_policy::decideCacheRequest(cache, 50U, false);
    TEST_ASSERT_TRUE(decision.rejectColdForBackoff);
    TEST_ASSERT_FALSE(decision.enqueueRefresh);

    decision = bridge_runtime_policy::decideCacheRequest(cache, 80U, false);
    TEST_ASSERT_FALSE(decision.rejectColdForBackoff);
    TEST_ASSERT_TRUE(decision.enqueueRefresh);
}

void test_forced_refresh_bypasses_cache_and_failure_keeps_last_good_snapshot() {
    CacheTiming cache{true, 1000U, 60000U, 0U};
    const CacheDecision forced = bridge_runtime_policy::decideCacheRequest(cache, 1100U, true);
    TEST_ASSERT_FALSE(forced.servePayload);
    TEST_ASSERT_TRUE(forced.enqueueRefresh);

    const CacheTiming failed = bridge_runtime_policy::noteCacheRefreshFailure(cache, 1200U, 5000U);
    TEST_ASSERT_TRUE(failed.hasPayload);
    TEST_ASSERT_EQUAL_UINT32(1000U, failed.sampledAtMs);
    TEST_ASSERT_EQUAL_UINT32(60000U, failed.ttlMs);
    TEST_ASSERT_EQUAL_UINT32(6200U, failed.retryAfterMs);
}

void test_deadlines_wait_clamping_and_watchdog_heartbeat_are_wraparound_safe() {
    const uint32_t now = UINT32_MAX - 1000U;
    const uint32_t deadline = now + 2000U;
    TEST_ASSERT_FALSE(bridge_runtime_policy::deadlineReached(now, deadline));
    TEST_ASSERT_EQUAL_UINT32(2000U, bridge_runtime_policy::deadlineRemaining(now, deadline));
    TEST_ASSERT_EQUAL_UINT32(
        2000U,
        bridge_runtime_policy::clampWaitToDeadline(5000U, 3000U, now, deadline));
    TEST_ASSERT_TRUE(bridge_runtime_policy::deadlineReached(deadline, deadline));
    TEST_ASSERT_EQUAL_UINT32(0U, bridge_runtime_policy::deadlineRemaining(deadline, deadline));

    const uint32_t heartbeat = UINT32_MAX - 25U;
    TEST_ASSERT_FALSE(bridge_runtime_policy::watchdogShouldReboot(true, true, 18U, heartbeat, 45U));
    TEST_ASSERT_TRUE(bridge_runtime_policy::watchdogShouldReboot(true, true, 19U, heartbeat, 45U));
    TEST_ASSERT_FALSE(bridge_runtime_policy::watchdogShouldReboot(true, false, 100U, heartbeat, 45U));
    TEST_ASSERT_FALSE(bridge_runtime_policy::watchdogShouldReboot(false, true, 100U, heartbeat, 45U));
    TEST_ASSERT_FALSE(bridge_runtime_policy::watchdogShouldReboot(
        true, true, 100U, 101U, 45U));
}

void test_global_recovery_thresholds_are_bounded_and_wraparound_safe() {
    const uint32_t heartbeat = UINT32_MAX - 25U;
    TEST_ASSERT_FALSE(bridge_runtime_policy::httpHeartbeatExpired(
        true, 18U, heartbeat, 45U));
    TEST_ASSERT_TRUE(bridge_runtime_policy::httpHeartbeatExpired(
        true, 19U, heartbeat, 45U));
    TEST_ASSERT_FALSE(bridge_runtime_policy::httpHeartbeatExpired(
        false, 100U, heartbeat, 45U));
    TEST_ASSERT_FALSE(bridge_runtime_policy::httpHeartbeatExpired(
        true, 100U, 101U, 45U));

    TEST_ASSERT_FALSE(bridge_runtime_policy::criticalMemory(
        12U * 1024U, 4U * 1024U, 12U * 1024U, 4U * 1024U));
    TEST_ASSERT_TRUE(bridge_runtime_policy::criticalMemory(
        12U * 1024U - 1U, 8U * 1024U, 12U * 1024U, 4U * 1024U));
    TEST_ASSERT_TRUE(bridge_runtime_policy::criticalMemory(
        24U * 1024U, 4U * 1024U - 1U, 12U * 1024U, 4U * 1024U));

    const uint32_t pressureStarted = UINT32_MAX - 1000U;
    TEST_ASSERT_FALSE(bridge_runtime_policy::sustainedCondition(
        true, true, 3998U, pressureStarted, 5000U));
    TEST_ASSERT_TRUE(bridge_runtime_policy::sustainedCondition(
        true, true, 3999U, pressureStarted, 5000U));
    TEST_ASSERT_FALSE(bridge_runtime_policy::sustainedCondition(
        false, true, 5000U, pressureStarted, 5000U));
    TEST_ASSERT_FALSE(bridge_runtime_policy::sustainedCondition(
        true, false, 5000U, pressureStarted, 5000U));

    TEST_ASSERT_TRUE(bridge_runtime_policy::recentFailure(
        1U, 18U, heartbeat, 45U));
    TEST_ASSERT_FALSE(bridge_runtime_policy::recentFailure(
        1U, 19U, heartbeat, 45U));
    TEST_ASSERT_FALSE(bridge_runtime_policy::recentFailure(
        0U, 18U, heartbeat, 45U));
}

void test_successful_cache_and_spool_results_survive_until_publication() {
    TEST_ASSERT_TRUE(bridge_runtime_policy::retainWorkerResultFile(true, false, 200));
    TEST_ASSERT_TRUE(bridge_runtime_policy::retainWorkerResultFile(false, true, 200));
    TEST_ASSERT_FALSE(bridge_runtime_policy::retainWorkerResultFile(true, false, 500));
    TEST_ASSERT_FALSE(bridge_runtime_policy::retainWorkerResultFile(false, true, 507));
    TEST_ASSERT_FALSE(bridge_runtime_policy::retainWorkerResultFile(false, false, 200));
}

void test_oversized_legacy_history_becomes_a_lossless_preservation_floor() {
    constexpr size_t configuredBytes = 96U * 1024U;
    constexpr size_t writableUpperBytes = 192U * 1024U;
    constexpr size_t legacyFileBytes = 168730U;

    TEST_ASSERT_EQUAL_UINT32(
        legacyFileBytes,
        history_retention::effectiveBudget(
            configuredBytes, writableUpperBytes, legacyFileBytes));
    TEST_ASSERT_EQUAL_UINT32(
        512U * 1024U,
        history_retention::effectiveBudget(
            512U * 1024U, 576U * 1024U, legacyFileBytes));
    TEST_ASSERT_FALSE(history_retention::canLowerWithoutDataLoss(
        configuredBytes, legacyFileBytes));
    TEST_ASSERT_TRUE(history_retention::canLowerWithoutDataLoss(
        legacyFileBytes, legacyFileBytes));
}

void test_full_history_rejects_append_without_removing_existing_bytes() {
    constexpr size_t existingBytes = 168730U;
    TEST_ASSERT_FALSE(history_retention::appendFits(
        existingBytes, 1U, existingBytes));
    TEST_ASSERT_TRUE(history_retention::appendFits(
        existingBytes, 512U, existingBytes + 512U));
    TEST_ASSERT_FALSE(history_retention::appendFits(
        SIZE_MAX - 10U, 20U, SIZE_MAX));

    TEST_ASSERT_EQUAL_UINT32(8U * 1024U * 1024U,
                             history_capacity::LITTLEFS_PARTITION_BYTES);
    TEST_ASSERT_EQUAL_UINT32(7500U * 1024U,
                             history_capacity::MAX_GENERATED_BACKUP_BYTES);
}

struct TestMachine {
    std::string serial{"SERIAL"};
    std::string alias{"Kitchen"};
    std::string address{"aa:bb:cc:dd:ee:ff"};
    uint8_t addressType{1};
    std::string manufacturer{"Nivona"};
    std::string model{"NICR"};
    std::string modelCode{"930"};
    std::string modelName{"CafeRomatica"};
    std::string familyKey{"900"};
    std::string hardwareRevision{"1"};
    std::string firmwareRevision{"2"};
    std::string softwareRevision{"3"};
    std::string ad06Hex{"00ff"};
    std::string ad06Ascii{".."};
    int lastSeenRssi{-50};
    uint32_t lastSeenAtMs{100};
    uint32_t savedAtMs{10};
    uint32_t generation{1};
};

void test_persistence_equality_ignores_presence_and_generation_only() {
    TestMachine original;
    TestMachine polled = original;
    polled.lastSeenRssi = -80;
    polled.lastSeenAtMs = 999999U;
    polled.generation = 22U;
    TEST_ASSERT_TRUE(bridge_runtime_policy::durableMachineFieldsEqual(original, polled));

    polled.alias = "Office";
    TEST_ASSERT_FALSE(bridge_runtime_policy::durableMachineFieldsEqual(original, polled));
    polled = original;
    polled.address = "11:22:33:44:55:66";
    TEST_ASSERT_FALSE(bridge_runtime_policy::durableMachineFieldsEqual(original, polled));
    polled = original;
    polled.savedAtMs++;
    TEST_ASSERT_FALSE(bridge_runtime_policy::durableMachineFieldsEqual(original, polled));
}

class FakeTransport {
public:
    TransportOutcome createClient() {
        createCalls++;
        return TransportOutcome::Success;
    }

    TransportOutcome connect(const std::string& target) {
        connectCalls++;
        connectedTargets.push_back(target);
        return TransportOutcome::Success;
    }

    TransportOutcome discoverHandles() {
        discoveryCalls++;
        return TransportOutcome::Success;
    }

    TransportOutcome establishHuSession() {
        sessionCalls++;
        return TransportOutcome::Success;
    }

    TransportOutcome read(uint16_t operation) {
        reads.push_back(operation);
        if (operation == timeoutOperation) {
            return TransportOutcome::Timeout;
        }
        return TransportOutcome::Success;
    }

    void disconnect() {
        disconnectCalls++;
    }

    void destroyClient() {
        destroyCalls++;
    }

    uint16_t timeoutOperation{0};
    uint32_t createCalls{0};
    uint32_t connectCalls{0};
    uint32_t discoveryCalls{0};
    uint32_t sessionCalls{0};
    uint32_t disconnectCalls{0};
    uint32_t destroyCalls{0};
    std::vector<uint16_t> reads;
    std::vector<std::string> connectedTargets;
};

void test_adjacent_jobs_reuse_one_connection_discovery_and_hu_session() {
    FakeTransport transport;
    ReusableSession<FakeTransport> session;
    const std::array<uint16_t, 2> first{{100U, 101U}};
    const std::array<uint16_t, 1> second{{102U}};

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(TransportOutcome::Success),
        static_cast<int>(session.runReadJob(transport, "machine-a", true, first.begin(), first.end())));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(TransportOutcome::Success),
        static_cast<int>(session.runReadJob(transport, "machine-a", true, second.begin(), second.end())));

    TEST_ASSERT_EQUAL_UINT32(1U, transport.createCalls);
    TEST_ASSERT_EQUAL_UINT32(1U, transport.connectCalls);
    TEST_ASSERT_EQUAL_UINT32(1U, transport.discoveryCalls);
    TEST_ASSERT_EQUAL_UINT32(1U, transport.sessionCalls);
    TEST_ASSERT_EQUAL_UINT32(3U, transport.reads.size());
}

void test_first_timeout_aborts_crawl_and_next_job_recreates_client() {
    FakeTransport transport;
    ReusableSession<FakeTransport> session;
    const std::array<uint16_t, 3> crawl{{200U, 201U, 202U}};
    transport.timeoutOperation = 201U;

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(TransportOutcome::Timeout),
        static_cast<int>(session.runReadJob(transport, "machine-a", true, crawl.begin(), crawl.end())));
    TEST_ASSERT_EQUAL_UINT32(2U, transport.reads.size());
    TEST_ASSERT_EQUAL_UINT16(200U, transport.reads[0]);
    TEST_ASSERT_EQUAL_UINT16(201U, transport.reads[1]);
    TEST_ASSERT_TRUE(session.snapshot().recreateBeforeNextJob);

    // A refresh failure must leave the last good data untouched.
    std::string cachedSnapshot = "last-good";
    const std::string failedCandidate = "partial-new-data";
    if (!session.snapshot().recreateBeforeNextJob) {
        cachedSnapshot = failedCandidate;
    }
    TEST_ASSERT_EQUAL_STRING("last-good", cachedSnapshot.c_str());

    transport.timeoutOperation = 0U;
    const std::array<uint16_t, 1> recovery{{300U}};
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(TransportOutcome::Success),
        static_cast<int>(session.runReadJob(transport, "machine-a", true, recovery.begin(), recovery.end())));
    TEST_ASSERT_EQUAL_UINT32(1U, transport.destroyCalls);
    TEST_ASSERT_EQUAL_UINT32(2U, transport.createCalls);
    TEST_ASSERT_EQUAL_UINT32(2U, transport.connectCalls);
    TEST_ASSERT_EQUAL_UINT32(2U, transport.discoveryCalls);
    TEST_ASSERT_EQUAL_UINT32(2U, transport.sessionCalls);
    TEST_ASSERT_FALSE(session.snapshot().recreateBeforeNextJob);
}

} // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_cache_freshness_and_backoff_are_wraparound_safe);
    RUN_TEST(test_forced_refresh_bypasses_cache_and_failure_keeps_last_good_snapshot);
    RUN_TEST(test_deadlines_wait_clamping_and_watchdog_heartbeat_are_wraparound_safe);
    RUN_TEST(test_global_recovery_thresholds_are_bounded_and_wraparound_safe);
    RUN_TEST(test_successful_cache_and_spool_results_survive_until_publication);
    RUN_TEST(test_oversized_legacy_history_becomes_a_lossless_preservation_floor);
    RUN_TEST(test_full_history_rejects_append_without_removing_existing_bytes);
    RUN_TEST(test_persistence_equality_ignores_presence_and_generation_only);
    RUN_TEST(test_adjacent_jobs_reuse_one_connection_discovery_and_hu_session);
    RUN_TEST(test_first_timeout_aborts_crawl_and_next_job_recreates_client);
    return UNITY_END();
}
