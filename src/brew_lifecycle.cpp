#include "brew_lifecycle.h"

#include <cstring>

namespace brew_lifecycle {

const char* stateName(State state) {
    switch (state) {
        case State::Queued: return "queued";
        case State::WaitingForMachine: return "waiting_for_machine";
        case State::Dispatching: return "dispatching";
        case State::Accepted: return "accepted";
        case State::Preparing: return "preparing";
        case State::AttentionRequired: return "attention_required";
        case State::Reconnecting: return "reconnecting";
        case State::Completed: return "completed";
        case State::Failed: return "failed";
        case State::Interrupted: return "interrupted";
        case State::Unknown: return "unknown";
        case State::Cancelled: return "cancelled";
    }
    return "unknown";
}

bool parseState(const char* value, State& stateOut) {
    if (value == nullptr) return false;
    for (uint8_t raw = static_cast<uint8_t>(State::Queued);
         raw <= static_cast<uint8_t>(State::Cancelled);
         ++raw) {
        const State candidate = static_cast<State>(raw);
        if (std::strcmp(value, stateName(candidate)) == 0) {
            stateOut = candidate;
            return true;
        }
    }
    return false;
}

bool terminal(State state) {
    return state == State::Completed || state == State::Failed ||
        state == State::Interrupted || state == State::Unknown ||
        state == State::Cancelled;
}

bool blocksQueue(State state, bool holdAfter) {
    if (state == State::Failed || state == State::Interrupted ||
        state == State::Unknown) {
        return true;
    }
    return holdAfter && state == State::Completed;
}

bool safelyCancellable(const Tracker& tracker) {
    return !tracker.commandMayHaveBeenSent &&
        (tracker.state == State::Queued ||
         tracker.state == State::WaitingForMachine ||
         tracker.state == State::Dispatching);
}

bool shouldDispatch(const Tracker& tracker) {
    return !tracker.commandMayHaveBeenSent &&
        (tracker.state == State::Queued ||
         tracker.state == State::WaitingForMachine ||
         tracker.state == State::Dispatching);
}

Observation classifyStatus(int16_t process, int16_t message) {
    if (message == 20 || (message >= 1 && message <= 6) || message == 11) {
        return Observation::KnownAttention;
    }
    if (message != 0) return Observation::UnknownAttention;
    if (process == 4 || process == 11) return Observation::Preparing;
    if (process == 3 || process == 8) return Observation::Ready;
    return Observation::Other;
}

void noteCommandMayBeSent(Tracker& tracker) {
    tracker.commandMayHaveBeenSent = true;
    tracker.state = State::Dispatching;
}

void noteAccepted(Tracker& tracker) {
    tracker.commandMayHaveBeenSent = true;
    tracker.commandAccepted = true;
    tracker.state = State::Accepted;
}

void noteDisconnected(Tracker& tracker) {
    if (tracker.commandMayHaveBeenSent && !terminal(tracker.state)) {
        tracker.state = State::Reconnecting;
    } else if (!terminal(tracker.state)) {
        tracker.state = State::WaitingForMachine;
    }
}

void noteDispatchFailure(Tracker& tracker) {
    tracker.state = tracker.commandMayHaveBeenSent ? State::Unknown : State::Failed;
}

void observe(Tracker& tracker,
             Observation observation,
             uint32_t nowMs,
             uint32_t readySeparationMs) {
    if (terminal(tracker.state)) return;
    if (observation == Observation::KnownAttention) {
        tracker.attentionInterrupted = true;
        tracker.state = State::AttentionRequired;
        tracker.readyObservations = 0;
        return;
    }
    if (observation == Observation::UnknownAttention) {
        tracker.ambiguousStatusObserved = true;
        tracker.state = State::AttentionRequired;
        tracker.readyObservations = 0;
        return;
    }
    if (tracker.ambiguousStatusObserved) {
        if (observation == Observation::Ready) tracker.state = State::Unknown;
        return;
    }
    if (observation == Observation::Preparing) {
        tracker.attentionInterrupted = false;
        tracker.preparationObserved = true;
        tracker.readyObservations = 0;
        tracker.state = State::Preparing;
        return;
    }
    if (observation == Observation::Ready && tracker.attentionInterrupted) {
        tracker.state = State::Interrupted;
        return;
    }
    if (observation == Observation::Ready && tracker.preparationObserved) {
        if (tracker.readyObservations == 0) {
            tracker.readyObservations = 1;
            tracker.firstReadyAtMs = nowMs;
            tracker.state = State::Preparing;
        } else if (static_cast<uint32_t>(nowMs - tracker.firstReadyAtMs) >= readySeparationMs) {
            tracker.readyObservations = 2;
            tracker.state = State::Completed;
        }
        return;
    }
    if (observation == Observation::Ready && tracker.state == State::AttentionRequired) {
        tracker.state = State::Accepted;
        return;
    }
    if (tracker.state == State::AttentionRequired && observation == Observation::Other) {
        tracker.state = tracker.preparationObserved ? State::Preparing : State::Accepted;
    }
}

State recoverAfterRestart(const Tracker& tracker) {
    if (terminal(tracker.state)) return tracker.state;
    if (tracker.commandMayHaveBeenSent || tracker.commandAccepted ||
        tracker.state == State::Accepted || tracker.state == State::Preparing ||
        tracker.state == State::AttentionRequired || tracker.state == State::Reconnecting) {
        return State::Unknown;
    }
    return tracker.state == State::Dispatching ? State::Queued : tracker.state;
}

} // namespace brew_lifecycle
