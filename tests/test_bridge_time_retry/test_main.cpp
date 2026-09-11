#include <unity.h>

#include "bridge_time_retry.h"

using bridge_time::detail::SyncDecision;
using bridge_time::detail::decideSync;
using bridge_time::detail::intervalElapsed;

namespace {

void test_interval_gate_is_wraparound_safe() {
    TEST_ASSERT_FALSE(intervalElapsed(0x00000010U, 0xfffffff0U, 33U));
    TEST_ASSERT_TRUE(intervalElapsed(0x00000011U, 0xfffffff0U, 33U));
}

void test_disconnected_and_disabled_time_never_request_sync() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncDecision::none),
                          static_cast<int>(decideSync(false, true, true, false, false, 10U, 0U, 60000U)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncDecision::none),
                          static_cast<int>(decideSync(true, true, false, false, false, 10U, 0U, 60000U)));
}

void test_connectivity_and_initial_configuration_request_once() {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(SyncDecision::connectivity_gained),
        static_cast<int>(decideSync(true, true, true, false, false, 10U, 0U, 60000U)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncDecision::initial),
                          static_cast<int>(decideSync(true, false, true, false, false, 10U, 0U, 60000U)));
}

void test_unsynced_clock_retries_only_after_interval() {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(SyncDecision::none),
        static_cast<int>(decideSync(true, false, true, true, false, 60099U, 100U, 60000U)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(SyncDecision::retry),
        static_cast<int>(decideSync(true, false, true, true, false, 60100U, 100U, 60000U)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(SyncDecision::none),
        static_cast<int>(decideSync(true, false, true, true, true, 120100U, 100U, 60000U)));
}

} // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_interval_gate_is_wraparound_safe);
    RUN_TEST(test_disconnected_and_disabled_time_never_request_sync);
    RUN_TEST(test_connectivity_and_initial_configuration_request_once);
    RUN_TEST(test_unsynced_clock_retries_only_after_interval);
    return UNITY_END();
}
