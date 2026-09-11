#include <unity.h>

#include "bridge_jobs.h"

using bridge_jobs::Job;
using bridge_jobs::Priority;
using bridge_jobs::Scheduler;
using bridge_jobs::State;
using bridge_jobs::Submission;
using bridge_jobs::SubmitStatus;

namespace {

struct ChangeCapture {
    size_t count{0};
    std::string lastId;
};

void captureChange(const char* id, void* context) {
    auto* capture = static_cast<ChangeCapture*>(context);
    capture->count++;
    capture->lastId = id != nullptr ? id : "";
}

Submission job(const char* kind,
               Priority priority,
               const char* target = "machine-a",
               const char* key = "") {
    Submission value;
    value.kind = kind;
    value.operationCode = 1;
    value.target = target;
    value.priority = priority;
    value.coalesceKey = key;
    value.deadlineMs = 1000;
    return value;
}

void test_priority_and_fifo() {
    Scheduler scheduler(0x1234);
    scheduler.submit(job("background", Priority::Background), 10);
    scheduler.submit(job("forced-1", Priority::ForcedRead), 11);
    scheduler.submit(job("mutation", Priority::Mutation), 12);
    scheduler.submit(job("forced-2", Priority::ForcedRead), 13);

    Job next;
    TEST_ASSERT_TRUE(scheduler.startNext(14, next));
    TEST_ASSERT_EQUAL_STRING("mutation", next.kind.c_str());
    TEST_ASSERT_TRUE(scheduler.finish(next.id, true, 15));
    TEST_ASSERT_TRUE(scheduler.startNext(16, next));
    TEST_ASSERT_EQUAL_STRING("forced-1", next.kind.c_str());
    TEST_ASSERT_TRUE(scheduler.finish(next.id, true, 17));
    TEST_ASSERT_TRUE(scheduler.startNext(18, next));
    TEST_ASSERT_EQUAL_STRING("forced-2", next.kind.c_str());
}

void test_reads_coalesce_but_mutations_do_not() {
    Scheduler scheduler(1);
    auto first = scheduler.submit(job("stats", Priority::StaleRefresh, "A", "stats:A"), 1);
    auto second = scheduler.submit(job("stats", Priority::ForcedRead, "A", "stats:A"), 2);
    TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Accepted), static_cast<int>(first.status));
    TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Coalesced), static_cast<int>(second.status));
    TEST_ASSERT_EQUAL_STRING(first.id.c_str(), second.id.c_str());
    Job promoted;
    TEST_ASSERT_TRUE(scheduler.get(first.id, promoted));
    TEST_ASSERT_EQUAL(static_cast<int>(Priority::ForcedRead), static_cast<int>(promoted.priority));
    TEST_ASSERT_EQUAL_UINT32(1002U, promoted.deadlineAtMs);

    Submission mutation = job("write", Priority::Mutation, "A", "same-write");
    auto writeOne = scheduler.submit(mutation, 3);
    auto writeTwo = scheduler.submit(mutation, 4);
    TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Accepted), static_cast<int>(writeOne.status));
    TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Accepted), static_cast<int>(writeTwo.status));
    TEST_ASSERT_NOT_EQUAL(0, writeOne.id.compare(writeTwo.id));
}

void test_admission_key_rejects_duplicate_until_job_is_terminal() {
    Scheduler scheduler(15);
    Submission firstBrew = job("brew", Priority::Mutation, "machine-a");
    firstBrew.admissionKey = "brew:machine-a";
    const auto first = scheduler.submit(firstBrew, 1);
    TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Accepted), static_cast<int>(first.status));

    Submission duplicate = job("brew", Priority::Mutation, "machine-a");
    duplicate.admissionKey = "brew:machine-a";
    const auto queuedConflict = scheduler.submit(duplicate, 2);
    TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Conflict),
                      static_cast<int>(queuedConflict.status));
    TEST_ASSERT_EQUAL_STRING(first.id.c_str(), queuedConflict.id.c_str());

    Submission otherMachine = job("brew", Priority::Mutation, "machine-b");
    otherMachine.admissionKey = "brew:machine-b";
    TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Accepted),
                      static_cast<int>(scheduler.submit(otherMachine, 2).status));

    Job running;
    TEST_ASSERT_TRUE(scheduler.startNext(3, running));
    const auto runningConflict = scheduler.submit(duplicate, 4);
    TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Conflict),
                      static_cast<int>(runningConflict.status));
    TEST_ASSERT_EQUAL_STRING(first.id.c_str(), runningConflict.id.c_str());

    TEST_ASSERT_TRUE(scheduler.finish(running.id, true, 5));
    const auto afterCompletion = scheduler.submit(duplicate, 6);
    TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Accepted),
                      static_cast<int>(afterCompletion.status));
    TEST_ASSERT_NOT_EQUAL(0, first.id.compare(afterCompletion.id));
    TEST_ASSERT_TRUE(scheduler.cancel(afterCompletion.id, 7));
    TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Accepted),
                      static_cast<int>(scheduler.submit(duplicate, 8).status));
    TEST_ASSERT_EQUAL_UINT32(2, scheduler.counters().rejected);
}

void test_pressure_evicts_only_queued_background() {
    Scheduler scheduler(2);
    for (size_t index = 0; index < bridge_jobs::ACTIVE_CAPACITY; ++index) {
        auto result = scheduler.submit(job("background", Priority::Background, "A"), static_cast<uint32_t>(index));
        TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Accepted), static_cast<int>(result.status));
    }
    auto interactive = scheduler.submit(job("brew", Priority::Mutation, "A"), 20);
    TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Accepted), static_cast<int>(interactive.status));
    TEST_ASSERT_FALSE(interactive.evictedId.empty());
    TEST_ASSERT_EQUAL_UINT32(1, scheduler.counters().backgroundEvicted);

    Scheduler noBackground(3);
    for (size_t index = 0; index < bridge_jobs::ACTIVE_CAPACITY; ++index) {
        noBackground.submit(job("forced", Priority::ForcedRead, "A"), static_cast<uint32_t>(index));
    }
    auto rejected = noBackground.submit(job("brew", Priority::Mutation, "A"), 20);
    TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Rejected), static_cast<int>(rejected.status));
}

void test_cancelled_running_job_cannot_complete() {
    Scheduler scheduler(4);
    const auto submitted = scheduler.submit(job("stats", Priority::ForcedRead, "deleted"), 1);
    Job running;
    TEST_ASSERT_TRUE(scheduler.startNext(2, running));
    TEST_ASSERT_EQUAL_UINT32(1, scheduler.cancelTarget("deleted", 3));
    TEST_ASSERT_FALSE(scheduler.finish(running.id, true, 4, "/result"));
    Job snapshot;
    TEST_ASSERT_TRUE(scheduler.get(submitted.id, snapshot));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Cancelled), static_cast<int>(snapshot.state));
}

void test_running_progress_is_published_and_bounded() {
    Scheduler scheduler(14);
    const auto submitted = scheduler.submit(job("stats", Priority::ForcedRead), 1);
    Job running;
    TEST_ASSERT_TRUE(scheduler.startNext(2, running));
    TEST_ASSERT_EQUAL_UINT8(1, running.progress);
    TEST_ASSERT_TRUE(scheduler.setProgress(running.id, 40));
    Job snapshot;
    TEST_ASSERT_TRUE(scheduler.get(submitted.id, snapshot));
    TEST_ASSERT_EQUAL_UINT8(40, snapshot.progress);
    TEST_ASSERT_TRUE(scheduler.setProgress(running.id, 100));
    TEST_ASSERT_TRUE(scheduler.get(submitted.id, snapshot));
    TEST_ASSERT_EQUAL_UINT8(99, snapshot.progress);
    TEST_ASSERT_TRUE(scheduler.finish(running.id, true, 3));
    TEST_ASSERT_TRUE(scheduler.get(submitted.id, snapshot));
    TEST_ASSERT_EQUAL_UINT8(100, snapshot.progress);
    TEST_ASSERT_FALSE(scheduler.setProgress(running.id, 50));
}

void test_terminal_expiry_and_wraparound() {
    Scheduler scheduler(5);
    const uint32_t nearWrap = UINT32_MAX - 100;
    const auto submitted = scheduler.submit(job("summary", Priority::ForcedRead), nearWrap);
    Job running;
    TEST_ASSERT_TRUE(scheduler.startNext(nearWrap + 1, running));
    TEST_ASSERT_TRUE(scheduler.finish(running.id, true, nearWrap + 2));
    scheduler.expire(50);
    Job snapshot;
    TEST_ASSERT_TRUE(scheduler.get(submitted.id, snapshot));
    scheduler.expire(static_cast<uint32_t>(nearWrap + 2 + bridge_jobs::TERMINAL_RETENTION_MS));
    TEST_ASSERT_FALSE(scheduler.get(submitted.id, snapshot));
}

void test_terminal_records_are_never_evicted_before_retention() {
    Scheduler scheduler(7);
    std::string firstId;
    constexpr uint32_t finishedAt = 100;

    for (size_t index = 0; index < bridge_jobs::RECORD_CAPACITY; ++index) {
        const auto submitted = scheduler.submit(job("summary", Priority::ForcedRead), finishedAt);
        TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Accepted), static_cast<int>(submitted.status));
        if (index == 0) {
            firstId = submitted.id;
        }
        Job running;
        TEST_ASSERT_TRUE(scheduler.startNext(finishedAt, running));
        TEST_ASSERT_TRUE(scheduler.finish(running.id, true, finishedAt));
    }

    const auto rejected = scheduler.submit(job("another", Priority::Mutation), finishedAt + 1);
    TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Rejected), static_cast<int>(rejected.status));

    scheduler.expire(finishedAt + bridge_jobs::TERMINAL_RETENTION_MS - 1);
    Job snapshot;
    TEST_ASSERT_TRUE(scheduler.get(firstId, snapshot));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Succeeded), static_cast<int>(snapshot.state));
    TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Rejected),
                      static_cast<int>(scheduler.submit(job("still-full", Priority::Mutation),
                                                        finishedAt + bridge_jobs::TERMINAL_RETENTION_MS - 1).status));
}

void test_terminal_expiry_recovers_registry_capacity() {
    Scheduler scheduler(8);
    constexpr uint32_t finishedAt = 200;
    for (size_t index = 0; index < bridge_jobs::RECORD_CAPACITY; ++index) {
        const auto submitted = scheduler.submit(job("summary", Priority::ForcedRead), finishedAt);
        TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Accepted), static_cast<int>(submitted.status));
        Job running;
        TEST_ASSERT_TRUE(scheduler.startNext(finishedAt, running));
        TEST_ASSERT_TRUE(scheduler.finish(running.id, true, finishedAt));
    }

    const auto accepted = scheduler.submit(job("after-expiry", Priority::Mutation),
                                           finishedAt + bridge_jobs::TERMINAL_RETENTION_MS);
    TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Accepted), static_cast<int>(accepted.status));
}

void test_full_registry_rejection_does_not_cancel_background_work() {
    Scheduler scheduler(10);
    constexpr size_t terminalCount = bridge_jobs::RECORD_CAPACITY - bridge_jobs::ACTIVE_CAPACITY;
    for (size_t index = 0; index < terminalCount; ++index) {
        TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Accepted),
                          static_cast<int>(scheduler.submit(job("old", Priority::ForcedRead), 100).status));
        Job running;
        TEST_ASSERT_TRUE(scheduler.startNext(100, running));
        TEST_ASSERT_TRUE(scheduler.finish(running.id, true, 100));
    }
    for (size_t index = 0; index < bridge_jobs::ACTIVE_CAPACITY; ++index) {
        TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Accepted),
                          static_cast<int>(scheduler.submit(job("background", Priority::Background), 100).status));
    }

    const auto rejected = scheduler.submit(job("interactive", Priority::Mutation), 101);
    TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Rejected), static_cast<int>(rejected.status));
    TEST_ASSERT_EQUAL_UINT32(bridge_jobs::ACTIVE_CAPACITY, scheduler.queuedCount());
    TEST_ASSERT_EQUAL_UINT32(0, scheduler.counters().backgroundEvicted);
    TEST_ASSERT_EQUAL_UINT32(0, scheduler.counters().cancelled);
}

void test_registry_storage_is_not_embedded_in_scheduler_object() {
    TEST_ASSERT_LESS_THAN_UINT32(256, sizeof(Scheduler));
}

void test_terminalization_releases_large_request_fields() {
    Scheduler scheduler(9);
    Submission large = job("diagnostic", Priority::ForcedRead, "machine", "diagnostic:machine");
    large.request.assign(2048, 'r');
    large.identity.assign(1024, 'i');
    large.argument.assign(1024, 'a');
    large.coalesceKey.assign(1024, 'c');
    const auto submitted = scheduler.submit(large, 1);
    Job running;
    TEST_ASSERT_TRUE(scheduler.startNext(2, running));
    TEST_ASSERT_TRUE(scheduler.finish(running.id, true, 3));

    Job snapshot;
    TEST_ASSERT_TRUE(scheduler.get(submitted.id, snapshot));
    TEST_ASSERT_TRUE(snapshot.request.empty());
    TEST_ASSERT_TRUE(snapshot.identity.empty());
    TEST_ASSERT_TRUE(snapshot.argument.empty());
    TEST_ASSERT_TRUE(snapshot.coalesceKey.empty());
    TEST_ASSERT_LESS_THAN_UINT32(256, snapshot.request.capacity());
    TEST_ASSERT_LESS_THAN_UINT32(256, snapshot.identity.capacity());
    TEST_ASSERT_LESS_THAN_UINT32(256, snapshot.argument.capacity());
    TEST_ASSERT_LESS_THAN_UINT32(256, snapshot.coalesceKey.capacity());
}

void test_deadline_expires_while_queued() {
    Scheduler scheduler(6);
    Submission blocked = job("blocked", Priority::Background);
    blocked.deadlineMs = 10;
    const auto submitted = scheduler.submit(blocked, 100);
    Job next;
    TEST_ASSERT_FALSE(scheduler.startNext(111, next));
    Job snapshot;
    TEST_ASSERT_TRUE(scheduler.get(submitted.id, snapshot));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Failed), static_cast<int>(snapshot.state));
    TEST_ASSERT_EQUAL_STRING("deadline_exceeded", snapshot.errorCode.c_str());
}

void test_combined_refresh_reserves_resources_and_supersedes_background() {
    Scheduler scheduler(11);
    Submission background = job("machine_stats", Priority::Background, "A", "stats:A");
    const auto backgroundResult = scheduler.submit(background, 1);
    TEST_ASSERT_TRUE(scheduler.hasActiveResource("A", "stats"));
    TEST_ASSERT_FALSE(scheduler.hasActiveResource("A", "settings"));

    TEST_ASSERT_EQUAL_UINT32(
        1U,
        scheduler.cancelQueuedKindBelow("A", "machine_stats", Priority::ForcedRead, 2));
    Job cancelled;
    TEST_ASSERT_TRUE(scheduler.get(backgroundResult.id, cancelled));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Cancelled), static_cast<int>(cancelled.state));

    Submission combined = job("machine_refresh", Priority::ForcedRead, "A", "refresh:A");
    combined.request = "{\"resources\":[\"stats\",\"settings\"]}";
    scheduler.submit(combined, 3);
    TEST_ASSERT_TRUE(scheduler.hasActiveResource("A", "stats"));
    TEST_ASSERT_TRUE(scheduler.hasActiveResource("A", "settings"));
    TEST_ASSERT_FALSE(scheduler.hasActiveResource("A", "summary"));
    TEST_ASSERT_TRUE(scheduler.hasActiveCombinedResource("A", "stats"));
    TEST_ASSERT_FALSE(scheduler.hasActiveCombinedResource("A", "summary"));
    TEST_ASSERT_FALSE(scheduler.hasActiveCoalesceKey("missing"));

    Submission follower = job("machine_stats", Priority::ForcedRead, "A", "stats-follower:A");
    follower.deadlineMs = 75000;
    follower.executionDeadlineMs = 15000;
    const auto followerResult = scheduler.submit(follower, 4);
    Job followerSnapshot;
    TEST_ASSERT_TRUE(scheduler.get(followerResult.id, followerSnapshot));
    TEST_ASSERT_EQUAL_UINT32(15000U, followerSnapshot.executionDeadlineMs);
    TEST_ASSERT_TRUE(scheduler.hasActiveCoalesceKey("stats-follower:A"));
    TEST_ASSERT_TRUE(scheduler.canAcceptBackground());
}

void test_retention_capacity_covers_sixteen_machine_poll_rate() {
    Scheduler scheduler(12);
    constexpr size_t summaryJobs = 16U * 5U;
    constexpr size_t maintenanceAndDeepJobs = 16U;
    for (size_t index = 0; index < summaryJobs + maintenanceAndDeepJobs; ++index) {
        const auto submitted = scheduler.submit(
            job(index < summaryJobs ? "machine_summary" : "maintenance",
                Priority::ForcedRead,
                "fleet"),
            static_cast<uint32_t>(index));
        TEST_ASSERT_EQUAL(static_cast<int>(SubmitStatus::Accepted),
                          static_cast<int>(submitted.status));
        Job running;
        TEST_ASSERT_TRUE(scheduler.startNext(static_cast<uint32_t>(index), running));
        TEST_ASSERT_TRUE(scheduler.finish(
            running.id, true, static_cast<uint32_t>(index)));
    }
    TEST_ASSERT_EQUAL_UINT32(summaryJobs + maintenanceAndDeepJobs,
                             scheduler.counters().completed);
    TEST_ASSERT_TRUE(scheduler.canAcceptBackground());
}

void test_explicit_restore_can_discard_jobs_and_spooled_results() {
    Scheduler scheduler(13);
    const auto first = scheduler.submit(job("mutation", Priority::Mutation), 1);
    Job running;
    TEST_ASSERT_TRUE(scheduler.startNext(2, running));
    TEST_ASSERT_TRUE(scheduler.finish(running.id, true, 3, "/job-result.json"));
    const auto second = scheduler.submit(job("summary", Priority::ForcedRead), 4);
    TEST_ASSERT_EQUAL_UINT32(1U, scheduler.cancelAll(5));
    const auto countersBeforeDiscard = scheduler.counters();

    TEST_ASSERT_EQUAL_UINT32(2U, scheduler.discardAll());
    TEST_ASSERT_EQUAL_UINT32(0U, scheduler.activeCount());
    Job missing;
    TEST_ASSERT_FALSE(scheduler.get(first.id, missing));
    TEST_ASSERT_FALSE(scheduler.get(second.id, missing));
    TEST_ASSERT_EQUAL_UINT32(countersBeforeDiscard.completed,
                             scheduler.counters().completed);
    TEST_ASSERT_EQUAL_UINT32(countersBeforeDiscard.cancelled,
                             scheduler.counters().cancelled);
    std::string discardedPath;
    TEST_ASSERT_TRUE(scheduler.popDiscardedResultPath(discardedPath));
    TEST_ASSERT_EQUAL_STRING("/job-result.json", discardedPath.c_str());
}

void test_change_callback_covers_the_published_lifecycle() {
    Scheduler scheduler(16);
    ChangeCapture capture;
    scheduler.setChangeCallback(captureChange, &capture);
    const auto submitted = scheduler.submit(job("summary", Priority::ForcedRead), 1);
    TEST_ASSERT_EQUAL_UINT32(1U, capture.count);
    TEST_ASSERT_EQUAL_STRING(submitted.id.c_str(), capture.lastId.c_str());

    Job running;
    TEST_ASSERT_TRUE(scheduler.startNext(2, running));
    TEST_ASSERT_EQUAL_UINT32(2U, capture.count);
    TEST_ASSERT_TRUE(scheduler.setProgress(running.id, 40));
    TEST_ASSERT_EQUAL_UINT32(3U, capture.count);
    // Publishing the same progress twice must not create event churn.
    TEST_ASSERT_TRUE(scheduler.setProgress(running.id, 40));
    TEST_ASSERT_EQUAL_UINT32(3U, capture.count);
    TEST_ASSERT_TRUE(scheduler.finish(running.id, true, 3, {}, std::string(4096, 'x')));
    TEST_ASSERT_EQUAL_UINT32(4U, capture.count);

    bridge_jobs::PublicJob publicJob;
    TEST_ASSERT_TRUE(scheduler.getPublic(running.id, publicJob));
    TEST_ASSERT_EQUAL_STRING(running.id.c_str(), publicJob.id.c_str());
    TEST_ASSERT_EQUAL(static_cast<int>(State::Succeeded), static_cast<int>(publicJob.state));
    TEST_ASSERT_EQUAL_UINT8(100, publicJob.progress);
}

} // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_priority_and_fifo);
    RUN_TEST(test_reads_coalesce_but_mutations_do_not);
    RUN_TEST(test_admission_key_rejects_duplicate_until_job_is_terminal);
    RUN_TEST(test_pressure_evicts_only_queued_background);
    RUN_TEST(test_cancelled_running_job_cannot_complete);
    RUN_TEST(test_running_progress_is_published_and_bounded);
    RUN_TEST(test_terminal_expiry_and_wraparound);
    RUN_TEST(test_terminal_records_are_never_evicted_before_retention);
    RUN_TEST(test_terminal_expiry_recovers_registry_capacity);
    RUN_TEST(test_full_registry_rejection_does_not_cancel_background_work);
    RUN_TEST(test_registry_storage_is_not_embedded_in_scheduler_object);
    RUN_TEST(test_terminalization_releases_large_request_fields);
    RUN_TEST(test_deadline_expires_while_queued);
    RUN_TEST(test_combined_refresh_reserves_resources_and_supersedes_background);
    RUN_TEST(test_retention_capacity_covers_sixteen_machine_poll_rate);
    RUN_TEST(test_explicit_restore_can_discard_jobs_and_spooled_results);
    RUN_TEST(test_change_callback_covers_the_published_lifecycle);
    return UNITY_END();
}
