#include <unity.h>

#include <cstdio>
#include <string>

#include "bridge_event_changes.h"

namespace {

void test_job_changes_coalesce_and_mark_status() {
    bridge_http::EventChanges changes;
    changes.markJob("boot-1");
    changes.markJob("boot-1");
    changes.markJob("boot-2");
    const bridge_http::EventChangeBatch batch = changes.take();
    TEST_ASSERT_TRUE(batch.statusDirty);
    TEST_ASSERT_FALSE(batch.resync);
    TEST_ASSERT_EQUAL_UINT32(2, batch.count);
    TEST_ASSERT_EQUAL_STRING("boot-1", batch.ids[0].data());
    TEST_ASSERT_EQUAL_STRING("boot-2", batch.ids[1].data());
    TEST_ASSERT_FALSE(changes.pending());
}

void test_queue_pressure_requests_resync_without_growing() {
    bridge_http::EventChanges changes;
    char id[16];
    for (size_t index = 0; index < bridge_http::EVENT_CHANGE_CAPACITY + 4; ++index) {
        std::snprintf(id, sizeof(id), "job-%u", static_cast<unsigned>(index));
        changes.markJob(id);
    }
    const bridge_http::EventChangeBatch batch = changes.take();
    TEST_ASSERT_TRUE(batch.resync);
    TEST_ASSERT_EQUAL_UINT32(bridge_http::EVENT_CHANGE_CAPACITY, batch.count);
}

void test_oversized_identifier_requests_resync() {
    bridge_http::EventChanges changes;
    const std::string oversized(bridge_http::EVENT_JOB_ID_BYTES, 'x');
    changes.markJob(oversized.c_str());
    const bridge_http::EventChangeBatch batch = changes.take();
    TEST_ASSERT_TRUE(batch.statusDirty);
    TEST_ASSERT_TRUE(batch.resync);
    TEST_ASSERT_EQUAL_UINT32(0, batch.count);
}

void test_brew_changes_are_coalesced_independently() {
    bridge_http::EventChanges changes;
    changes.markBrew("brew-1");
    changes.markBrew("brew-1");
    changes.markJob("brew-1");
    const bridge_http::EventChangeBatch batch = changes.take();
    TEST_ASSERT_TRUE(batch.statusDirty);
    TEST_ASSERT_EQUAL_UINT32(2, batch.count);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bridge_http::EventChangeKind::Brew),
                            static_cast<uint8_t>(batch.kinds[0]));
    TEST_ASSERT_EQUAL_STRING("brew-1", batch.ids[0].data());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bridge_http::EventChangeKind::Job),
                            static_cast<uint8_t>(batch.kinds[1]));
}

} // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_job_changes_coalesce_and_mark_status);
    RUN_TEST(test_queue_pressure_requests_resync_without_growing);
    RUN_TEST(test_oversized_identifier_requests_resync);
    RUN_TEST(test_brew_changes_are_coalesced_independently);
    return UNITY_END();
}
