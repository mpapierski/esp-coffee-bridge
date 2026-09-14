#include <unity.h>

#include "brew_lifecycle.h"

using namespace brew_lifecycle;

void test_known_attention_takes_priority_over_preparing() {
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Observation::KnownAttention),
                            static_cast<uint8_t>(classifyStatus(4, 4)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Observation::KnownAttention),
                            static_cast<uint8_t>(classifyStatus(11, 20)));
}

void test_completion_requires_preparing_and_two_separated_ready_samples() {
    Tracker tracker;
    noteCommandMayBeSent(tracker);
    noteAccepted(tracker);
    observe(tracker, Observation::Ready, 1000);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Accepted),
                            static_cast<uint8_t>(tracker.state));
    observe(tracker, Observation::Preparing, 1500);
    observe(tracker, Observation::Ready, 2000);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Preparing),
                            static_cast<uint8_t>(tracker.state));
    observe(tracker, Observation::Ready, 3999);
    TEST_ASSERT_FALSE(terminal(tracker.state));
    observe(tracker, Observation::Ready, 4000);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Completed),
                            static_cast<uint8_t>(tracker.state));
}

void test_unknown_message_creates_manual_barrier_after_it_clears() {
    Tracker tracker;
    noteAccepted(tracker);
    observe(tracker, Observation::UnknownAttention, 100);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::AttentionRequired),
                            static_cast<uint8_t>(tracker.state));
    observe(tracker, Observation::Ready, 200);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Unknown),
                            static_cast<uint8_t>(tracker.state));
    TEST_ASSERT_TRUE(blocksQueue(tracker.state, false));
}

void test_attention_must_resume_preparing_before_completion() {
    Tracker tracker;
    noteAccepted(tracker);
    observe(tracker, Observation::Preparing, 100);
    observe(tracker, Observation::KnownAttention, 200);
    observe(tracker, Observation::Ready, 300);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Interrupted),
                            static_cast<uint8_t>(tracker.state));

    Tracker resumed;
    noteAccepted(resumed);
    observe(resumed, Observation::Preparing, 100);
    observe(resumed, Observation::KnownAttention, 200);
    observe(resumed, Observation::Preparing, 300);
    observe(resumed, Observation::Ready, 400);
    observe(resumed, Observation::Ready, 2400);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Completed),
                            static_cast<uint8_t>(resumed.state));
}

void test_restart_never_replays_possible_delivery() {
    Tracker preSend;
    preSend.state = State::Dispatching;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Queued),
                            static_cast<uint8_t>(recoverAfterRestart(preSend)));

    Tracker possibleSend;
    noteCommandMayBeSent(possibleSend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Unknown),
                            static_cast<uint8_t>(recoverAfterRestart(possibleSend)));
}

void test_only_unsent_items_are_safely_cancellable() {
    Tracker tracker;
    tracker.state = State::WaitingForMachine;
    TEST_ASSERT_TRUE(safelyCancellable(tracker));
    noteCommandMayBeSent(tracker);
    TEST_ASSERT_FALSE(safelyCancellable(tracker));
}

void test_possible_delivery_is_never_dispatched_again() {
    Tracker tracker;
    tracker.state = State::Dispatching;
    TEST_ASSERT_TRUE(shouldDispatch(tracker));

    noteCommandMayBeSent(tracker);
    TEST_ASSERT_FALSE(shouldDispatch(tracker));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_known_attention_takes_priority_over_preparing);
    RUN_TEST(test_completion_requires_preparing_and_two_separated_ready_samples);
    RUN_TEST(test_unknown_message_creates_manual_barrier_after_it_clears);
    RUN_TEST(test_attention_must_resume_preparing_before_completion);
    RUN_TEST(test_restart_never_replays_possible_delivery);
    RUN_TEST(test_only_unsent_items_are_safely_cancellable);
    RUN_TEST(test_possible_delivery_is_never_dispatched_again);
    return UNITY_END();
}
