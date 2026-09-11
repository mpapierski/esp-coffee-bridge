#include <unity.h>

#include "wifi_runtime_policy.h"

void test_setup_ap_is_only_allowed_without_station_configuration() {
    TEST_ASSERT_TRUE(wifi_runtime_policy::shouldRunSetupAccessPoint(false));
    TEST_ASSERT_FALSE(wifi_runtime_policy::shouldRunSetupAccessPoint(true));
}

void test_station_retry_requires_configuration_and_an_idle_disconnected_attempt() {
    TEST_ASSERT_TRUE(wifi_runtime_policy::shouldStartStationAttempt(true, false, false, 100, 100));
    TEST_ASSERT_FALSE(wifi_runtime_policy::shouldStartStationAttempt(false, false, false, 100, 100));
    TEST_ASSERT_FALSE(wifi_runtime_policy::shouldStartStationAttempt(true, true, false, 100, 100));
    TEST_ASSERT_FALSE(wifi_runtime_policy::shouldStartStationAttempt(true, false, true, 100, 100));
    TEST_ASSERT_FALSE(wifi_runtime_policy::shouldStartStationAttempt(true, false, false, 99, 100));
}

void test_station_retry_deadline_is_wraparound_safe() {
    const uint32_t deadline = 20;
    TEST_ASSERT_FALSE(wifi_runtime_policy::shouldStartStationAttempt(
        true, false, false, UINT32_MAX - 10, deadline));
    TEST_ASSERT_TRUE(wifi_runtime_policy::shouldStartStationAttempt(
        true, false, false, deadline, deadline));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_setup_ap_is_only_allowed_without_station_configuration);
    RUN_TEST(test_station_retry_requires_configuration_and_an_idle_disconnected_attempt);
    RUN_TEST(test_station_retry_deadline_is_wraparound_safe);
    return UNITY_END();
}
