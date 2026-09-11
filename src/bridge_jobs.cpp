#include "bridge_jobs.h"

#include <algorithm>
#include <cstdio>
#include <new>

namespace bridge_jobs {

Scheduler::Scheduler(uint32_t bootNonce)
    : jobs_(new (std::nothrow) Job[RECORD_CAPACITY]),
      discardedPaths_(new (std::nothrow) std::string[RECORD_CAPACITY]) {
    reset(bootNonce);
}

void Scheduler::reset(uint32_t bootNonce) {
    if (jobs_) {
        for (size_t index = 0; index < RECORD_CAPACITY; ++index) {
            jobs_[index] = {};
        }
    }
    counters_ = {};
    bootNonce_ = bootNonce;
    nextSequence_ = 1;
    nextCounter_ = 1;
    if (discardedPaths_) {
        for (size_t index = 0; index < RECORD_CAPACITY; ++index) {
            discardedPaths_[index].clear();
        }
    }
    discardedRead_ = 0;
    discardedWrite_ = 0;
    discardedCount_ = 0;
}

bool Scheduler::terminal(State state) {
    return state == State::Succeeded || state == State::Failed || state == State::Cancelled;
}

const char* Scheduler::stateName(State state) {
    switch (state) {
        case State::Queued:
            return "queued";
        case State::Running:
            return "running";
        case State::Succeeded:
            return "succeeded";
        case State::Failed:
            return "failed";
        case State::Cancelled:
            return "cancelled";
    }
    return "failed";
}

bool Scheduler::elapsedAtLeast(uint32_t nowMs, uint32_t thenMs, uint32_t durationMs) {
    return static_cast<uint32_t>(nowMs - thenMs) >= durationMs;
}

std::string Scheduler::nextId() {
    char value[32];
    std::snprintf(value, sizeof(value), "%08lx-%lu",
                  static_cast<unsigned long>(bootNonce_),
                  static_cast<unsigned long>(nextCounter_++));
    return value;
}

int Scheduler::findById(const std::string& id) const {
    if (!jobs_) {
        return -1;
    }
    for (size_t index = 0; index < RECORD_CAPACITY; ++index) {
        if (jobs_[index].occupied && jobs_[index].id == id) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int Scheduler::findFreeRecord() const {
    if (!jobs_) {
        return -1;
    }
    for (size_t index = 0; index < RECORD_CAPACITY; ++index) {
        if (!jobs_[index].occupied) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int Scheduler::findEvictableBackground() const {
    if (!jobs_) {
        return -1;
    }
    int selected = -1;
    uint64_t selectedSequence = 0;
    for (size_t index = 0; index < RECORD_CAPACITY; ++index) {
        const Job& job = jobs_[index];
        if (!job.occupied || job.state != State::Queued || job.priority != Priority::Background) {
            continue;
        }
        if (selected < 0 || job.sequence < selectedSequence) {
            selected = static_cast<int>(index);
            selectedSequence = job.sequence;
        }
    }
    return selected;
}

void Scheduler::makeTerminal(Job& job, State state, uint32_t nowMs) {
    job.state = state;
    job.finishedAtMs = nowMs;
    job.progress = state == State::Succeeded ? 100 : job.progress;
    // clear() retains a string's allocation. These fields can contain complete
    // request payloads and machine identities, so release their capacity as
    // soon as the worker no longer needs them.
    std::string{}.swap(job.request);
    std::string{}.swap(job.identity);
    std::string{}.swap(job.targetAddress);
    std::string{}.swap(job.argument);
    std::string{}.swap(job.coalesceKey);
}

void Scheduler::rememberDiscardedPath(const Job& job) {
    if (job.resultPath.empty()) {
        return;
    }
    if (!discardedPaths_) {
        return;
    }
    if (discardedCount_ == RECORD_CAPACITY) {
        discardedRead_ = (discardedRead_ + 1) % RECORD_CAPACITY;
        discardedCount_--;
    }
    discardedPaths_[discardedWrite_] = job.resultPath;
    discardedWrite_ = (discardedWrite_ + 1) % RECORD_CAPACITY;
    discardedCount_++;
}

SubmitResult Scheduler::submit(const Submission& submission, uint32_t nowMs) {
    expire(nowMs);

    if (!submission.coalesceKey.empty() && submission.priority != Priority::Mutation) {
        for (size_t index = 0; jobs_ && index < RECORD_CAPACITY; ++index) {
            Job& job = jobs_[index];
            if (job.occupied && (job.state == State::Queued || job.state == State::Running) &&
                job.coalesceKey == submission.coalesceKey) {
                if (job.state == State::Queued &&
                    static_cast<uint8_t>(submission.priority) > static_cast<uint8_t>(job.priority)) {
                    job.priority = submission.priority;
                    job.deadlineAtMs = nowMs + submission.deadlineMs;
                }
                counters_.coalesced++;
                return {SubmitStatus::Coalesced, job.id, {}};
            }
        }
    }

    // A terminal record must never be overwritten during its retention window.
    // Resolve registry pressure before cancelling a background job so a rejected
    // interactive submission cannot have side effects.
    const int slot = findFreeRecord();
    if (slot < 0) {
        counters_.rejected++;
        return {SubmitStatus::Rejected, {}, {}};
    }

    std::string evictedId;
    if (activeCount() >= ACTIVE_CAPACITY) {
        const int evictable = submission.priority == Priority::Background ? -1 : findEvictableBackground();
        if (evictable < 0) {
            counters_.rejected++;
            return {SubmitStatus::Rejected, {}, {}};
        }
        evictedId = jobs_[evictable].id;
        makeTerminal(jobs_[evictable], State::Cancelled, nowMs);
        jobs_[evictable].errorCode = "evicted";
        jobs_[evictable].errorMessage = "background job evicted for interactive work";
        counters_.cancelled++;
        counters_.backgroundEvicted++;
    }

    Job job;
    job.occupied = true;
    job.id = nextId();
    job.kind = submission.kind;
    job.target = submission.target;
    job.operationCode = submission.operationCode;
    job.argument = submission.argument;
    job.request = submission.request;
    job.identity = submission.identity;
    job.targetAddress = submission.targetAddress;
    job.targetAddressType = submission.targetAddressType;
    job.coalesceKey = submission.coalesceKey;
    job.resultUrl = submission.resultUrl;
    job.priority = submission.priority;
    job.state = State::Queued;
    job.sequence = nextSequence_++;
    job.submittedAtMs = nowMs;
    job.deadlineAtMs = nowMs + submission.deadlineMs;
    job.executionDeadlineMs = submission.executionDeadlineMs;
    job.resource = submission.resource;
    jobs_[slot] = std::move(job);
    counters_.submitted++;
    return {SubmitStatus::Accepted, jobs_[slot].id, evictedId};
}

bool Scheduler::startNext(uint32_t nowMs, Job& out) {
    expire(nowMs);
    int selected = -1;
    for (size_t index = 0; jobs_ && index < RECORD_CAPACITY; ++index) {
        const Job& candidate = jobs_[index];
        if (!candidate.occupied || candidate.state != State::Queued) {
            continue;
        }
        if (selected < 0 || static_cast<uint8_t>(candidate.priority) > static_cast<uint8_t>(jobs_[selected].priority) ||
            (candidate.priority == jobs_[selected].priority && candidate.sequence < jobs_[selected].sequence)) {
            selected = static_cast<int>(index);
        }
    }
    if (selected < 0) {
        return false;
    }

    Job& job = jobs_[selected];
    if (static_cast<int32_t>(nowMs - job.deadlineAtMs) >= 0) {
        makeTerminal(job, State::Failed, nowMs);
        job.errorCode = "deadline_exceeded";
        job.errorMessage = "job deadline expired while queued";
        counters_.failed++;
        return startNext(nowMs, out);
    }

    job.state = State::Running;
    job.startedAtMs = nowMs;
    job.progress = 1;
    out = job;
    return true;
}

bool Scheduler::finish(const std::string& id,
                       bool succeeded,
                       uint32_t nowMs,
                       const std::string& resultPath,
                       const std::string& resultBody,
                       int resultStatus,
                       const std::string& errorCode,
                       const std::string& errorMessage) {
    const int index = findById(id);
    if (index < 0) {
        return false;
    }
    Job& job = jobs_[index];
    // Deletion/cancellation wins over a late completion from an in-flight call.
    if (job.state != State::Running) {
        return false;
    }
    job.resultPath = resultPath;
    job.resultBody = resultBody;
    job.resultStatus = resultStatus;
    job.errorCode = errorCode;
    job.errorMessage = errorMessage;
    makeTerminal(job, succeeded ? State::Succeeded : State::Failed, nowMs);
    if (succeeded) {
        counters_.completed++;
    } else {
        counters_.failed++;
    }
    return true;
}

bool Scheduler::setProgress(const std::string& id, uint8_t progress) {
    const int index = findById(id);
    if (index < 0 || jobs_[index].state != State::Running) {
        return false;
    }
    jobs_[index].progress = std::min<uint8_t>(progress, 99);
    return true;
}

bool Scheduler::cancel(const std::string& id, uint32_t nowMs) {
    const int index = findById(id);
    if (index < 0 || terminal(jobs_[index].state)) {
        return false;
    }
    makeTerminal(jobs_[index], State::Cancelled, nowMs);
    jobs_[index].errorCode = "cancelled";
    jobs_[index].errorMessage = "job cancelled";
    counters_.cancelled++;
    return true;
}

size_t Scheduler::cancelTarget(const std::string& target,
                               uint32_t nowMs,
                               const std::string& exceptId) {
    size_t count = 0;
    for (size_t index = 0; jobs_ && index < RECORD_CAPACITY; ++index) {
        Job& job = jobs_[index];
        if (!job.occupied || terminal(job.state) || job.target != target ||
            (!exceptId.empty() && job.id == exceptId)) {
            continue;
        }
        makeTerminal(job, State::Cancelled, nowMs);
        job.errorCode = "target_deleted";
        job.errorMessage = "target machine was deleted";
        counters_.cancelled++;
        count++;
    }
    return count;
}

size_t Scheduler::cancelAll(uint32_t nowMs) {
    size_t count = 0;
    for (size_t index = 0; jobs_ && index < RECORD_CAPACITY; ++index) {
        Job& job = jobs_[index];
        if (!job.occupied || terminal(job.state)) {
            continue;
        }
        makeTerminal(job, State::Cancelled, nowMs);
        job.errorCode = "cancelled_for_restore";
        job.errorMessage = "job cancelled for bridge restore";
        counters_.cancelled++;
        count++;
    }
    return count;
}

size_t Scheduler::discardAll() {
    size_t count = 0;
    for (size_t index = 0; jobs_ && index < RECORD_CAPACITY; ++index) {
        Job& job = jobs_[index];
        if (!job.occupied) {
            continue;
        }
        rememberDiscardedPath(job);
        job = {};
        count++;
    }
    return count;
}

void Scheduler::expire(uint32_t nowMs) {
    for (size_t index = 0; jobs_ && index < RECORD_CAPACITY; ++index) {
        Job& job = jobs_[index];
        if (job.occupied && terminal(job.state) &&
            elapsedAtLeast(nowMs, job.finishedAtMs, TERMINAL_RETENTION_MS)) {
            rememberDiscardedPath(job);
            job = {};
        }
    }
}

bool Scheduler::get(const std::string& id, Job& out) const {
    const int index = findById(id);
    if (index < 0) {
        return false;
    }
    out = jobs_[index];
    return true;
}

bool Scheduler::popDiscardedResultPath(std::string& out) {
    if (!discardedPaths_ || discardedCount_ == 0) {
        return false;
    }
    out = std::move(discardedPaths_[discardedRead_]);
    discardedPaths_[discardedRead_].clear();
    discardedRead_ = (discardedRead_ + 1) % RECORD_CAPACITY;
    discardedCount_--;
    return true;
}

size_t Scheduler::queuedCount() const {
    size_t count = 0;
    for (size_t index = 0; jobs_ && index < RECORD_CAPACITY; ++index) {
        count += jobs_[index].occupied && jobs_[index].state == State::Queued ? 1U : 0U;
    }
    return count;
}

size_t Scheduler::runningCount() const {
    size_t count = 0;
    for (size_t index = 0; jobs_ && index < RECORD_CAPACITY; ++index) {
        count += jobs_[index].occupied && jobs_[index].state == State::Running ? 1U : 0U;
    }
    return count;
}

size_t Scheduler::activeCount() const {
    return queuedCount() + runningCount();
}

bool Scheduler::hasQueuedAbove(Priority priority) const {
    for (size_t index = 0; jobs_ && index < RECORD_CAPACITY; ++index) {
        const Job& job = jobs_[index];
        if (job.occupied && job.state == State::Queued &&
            static_cast<uint8_t>(job.priority) > static_cast<uint8_t>(priority)) {
            return true;
        }
    }
    return false;
}

bool Scheduler::hasActiveResource(const std::string& target, const std::string& resource) const {
    const std::string resourceKind = "machine_" + resource;
    const std::string requestToken = "\"" + resource + "\"";
    for (size_t index = 0; jobs_ && index < RECORD_CAPACITY; ++index) {
        const Job& job = jobs_[index];
        if (!job.occupied || (job.state != State::Queued && job.state != State::Running) ||
            job.target != target) {
            continue;
        }
        if (job.kind == resourceKind ||
            (job.kind == "machine_refresh" && job.request.find(requestToken) != std::string::npos)) {
            return true;
        }
    }
    return false;
}

bool Scheduler::hasActiveCombinedResource(const std::string& target,
                                          const std::string& resource) const {
    const std::string requestToken = "\"" + resource + "\"";
    for (size_t index = 0; jobs_ && index < RECORD_CAPACITY; ++index) {
        const Job& job = jobs_[index];
        if (job.occupied && (job.state == State::Queued || job.state == State::Running) &&
            job.target == target && job.kind == "machine_refresh" &&
            job.request.find(requestToken) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool Scheduler::hasActiveCoalesceKey(const std::string& coalesceKey) const {
    if (coalesceKey.empty()) {
        return false;
    }
    for (size_t index = 0; jobs_ && index < RECORD_CAPACITY; ++index) {
        const Job& job = jobs_[index];
        if (job.occupied && (job.state == State::Queued || job.state == State::Running) &&
            job.coalesceKey == coalesceKey) {
            return true;
        }
    }
    return false;
}

bool Scheduler::canAcceptBackground() const {
    return activeCount() < ACTIVE_CAPACITY && findFreeRecord() >= 0;
}

size_t Scheduler::cancelQueuedKindBelow(const std::string& target,
                                        const std::string& kind,
                                        Priority threshold,
                                        uint32_t nowMs) {
    size_t count = 0;
    for (size_t index = 0; jobs_ && index < RECORD_CAPACITY; ++index) {
        Job& job = jobs_[index];
        if (!job.occupied || job.state != State::Queued || job.target != target ||
            job.kind != kind ||
            static_cast<uint8_t>(job.priority) >= static_cast<uint8_t>(threshold)) {
            continue;
        }
        makeTerminal(job, State::Cancelled, nowMs);
        job.errorCode = "superseded";
        job.errorMessage = "queued refresh was superseded by a combined forced refresh";
        counters_.cancelled++;
        count++;
    }
    return count;
}

const Counters& Scheduler::counters() const {
    return counters_;
}

} // namespace bridge_jobs
