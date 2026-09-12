#pragma once

#include <cstdint>

namespace brew_lifecycle {

enum class State : uint8_t {
    Queued,
    WaitingForMachine,
    Dispatching,
    Accepted,
    Preparing,
    AttentionRequired,
    Reconnecting,
    Completed,
    Failed,
    Interrupted,
    Unknown,
    Cancelled,
};

enum class Observation : uint8_t {
    Ready,
    Preparing,
    KnownAttention,
    UnknownAttention,
    Other,
};

struct Tracker {
    State state{State::Queued};
    bool commandMayHaveBeenSent{false};
    bool commandAccepted{false};
    bool preparationObserved{false};
    bool ambiguousStatusObserved{false};
    bool attentionInterrupted{false};
    uint8_t readyObservations{0};
    uint32_t firstReadyAtMs{0};
};

const char* stateName(State state);
bool parseState(const char* value, State& stateOut);
bool terminal(State state);
bool blocksQueue(State state, bool holdAfter);
bool safelyCancellable(const Tracker& tracker);
bool shouldDispatch(const Tracker& tracker);
Observation classifyStatus(int16_t process, int16_t message);
void noteCommandMayBeSent(Tracker& tracker);
void noteAccepted(Tracker& tracker);
void noteDisconnected(Tracker& tracker);
void noteDispatchFailure(Tracker& tracker);
void observe(Tracker& tracker,
             Observation observation,
             uint32_t nowMs,
             uint32_t readySeparationMs = 2000);
State recoverAfterRestart(const Tracker& tracker);

} // namespace brew_lifecycle
