#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace bridge_jobs {

constexpr size_t ACTIVE_CAPACITY = 8;
// Lifecycle records remain in the registry after completion. The registry is
// allocated at runtime so retaining a useful number of terminal jobs does not
// consume the ESP32's static RAM budget.
// Five minutes of retained IDs must cover 16 machines polling summary once a
// minute, deep refreshes, scans, and a useful interactive burst without
// blocking the independent eight-slot active queue.
constexpr size_t RECORD_CAPACITY = 128;
constexpr uint32_t TERMINAL_RETENTION_MS = 5U * 60U * 1000U;

enum class Priority : uint8_t {
    Background = 0,
    StaleRefresh = 1,
    ForcedRead = 2,
    Mutation = 3,
};

enum class State : uint8_t {
    Queued,
    Running,
    Succeeded,
    Failed,
    Cancelled,
};

struct Submission {
    std::string kind;
    std::string target;
    uint16_t operationCode{0};
    std::string argument;
    std::string request;
    std::string identity;
    std::string targetAddress;
    uint8_t targetAddressType{0};
    std::string coalesceKey;
    // At most one queued or running job may hold a non-empty admission key.
    // Unlike coalescing, a duplicate submission is rejected and retains its
    // own request semantics.
    std::string admissionKey;
    std::string resultUrl;
    Priority priority{Priority::StaleRefresh};
    uint32_t deadlineMs{12000};
    // Optional run-time deadline used by dependency/follower jobs whose queue
    // wait may legitimately exceed the operation's own logical deadline.
    uint32_t executionDeadlineMs{0};
    bool resource{false};
    // Internal worker jobs can opt out of the public five-minute terminal
    // retention window. Their durable domain record remains the source of
    // truth after the worker finishes.
    bool retainTerminal{true};
};

struct Job {
    bool occupied{false};
    std::string id;
    std::string kind;
    std::string target;
    uint16_t operationCode{0};
    std::string argument;
    std::string request;
    std::string identity;
    std::string targetAddress;
    uint8_t targetAddressType{0};
    std::string coalesceKey;
    std::string admissionKey;
    std::string resultUrl;
    std::string resultPath;
    std::string resultBody;
    std::string errorCode;
    std::string errorMessage;
    Priority priority{Priority::StaleRefresh};
    State state{State::Queued};
    uint64_t sequence{0};
    uint32_t submittedAtMs{0};
    uint32_t startedAtMs{0};
    uint32_t finishedAtMs{0};
    uint32_t deadlineAtMs{0};
    uint32_t executionDeadlineMs{0};
    uint8_t progress{0};
    int resultStatus{200};
    bool resource{false};
    bool retainTerminal{true};
};

// Bounded lifecycle data safe to copy while publishing events. It deliberately
// excludes request/identity strings and potentially large retained results.
struct PublicJob {
    std::string id;
    std::string kind;
    std::string target;
    std::string resultUrl;
    std::string errorCode;
    std::string errorMessage;
    State state{State::Queued};
    uint32_t submittedAtMs{0};
    uint32_t startedAtMs{0};
    uint32_t finishedAtMs{0};
    uint8_t progress{0};
};

struct Counters {
    uint32_t submitted{0};
    uint32_t completed{0};
    uint32_t failed{0};
    uint32_t cancelled{0};
    uint32_t rejected{0};
    uint32_t coalesced{0};
    uint32_t backgroundEvicted{0};
};

enum class SubmitStatus : uint8_t {
    Accepted,
    Coalesced,
    Conflict,
    Rejected,
};

struct SubmitResult {
    SubmitStatus status{SubmitStatus::Rejected};
    std::string id;
    std::string evictedId;
};

class Scheduler {
public:
    using ChangeCallback = void (*)(const char* id, void* context);

    explicit Scheduler(uint32_t bootNonce = 0);

    void reset(uint32_t bootNonce);
    void setChangeCallback(ChangeCallback callback, void* context = nullptr);
    SubmitResult submit(const Submission& submission, uint32_t nowMs);
    bool startNext(uint32_t nowMs, Job& out);
    bool finish(const std::string& id,
                bool succeeded,
                uint32_t nowMs,
                const std::string& resultPath = {},
                const std::string& resultBody = {},
                int resultStatus = 200,
                const std::string& errorCode = {},
                const std::string& errorMessage = {});
    bool setProgress(const std::string& id, uint8_t progress);
    size_t cancelTarget(const std::string& target,
                        uint32_t nowMs,
                        const std::string& exceptId = {});
    size_t cancelAll(uint32_t nowMs);
    size_t discardAll();
    bool cancel(const std::string& id, uint32_t nowMs);
    void expire(uint32_t nowMs);

    bool get(const std::string& id, Job& out) const;
    bool getPublic(const std::string& id, PublicJob& out) const;
    bool popDiscardedResultPath(std::string& out);
    size_t queuedCount() const;
    size_t runningCount() const;
    size_t activeCount() const;
    bool hasQueuedAbove(Priority priority) const;
    bool hasActiveResource(const std::string& target, const std::string& resource) const;
    bool hasActiveCombinedResource(const std::string& target, const std::string& resource) const;
    bool hasActiveCoalesceKey(const std::string& coalesceKey) const;
    bool canAcceptBackground() const;
    size_t cancelQueuedKindBelow(const std::string& target,
                                 const std::string& kind,
                                 Priority threshold,
                                 uint32_t nowMs);
    const Counters& counters() const;

    static bool terminal(State state);
    static const char* stateName(State state);

private:
    static bool elapsedAtLeast(uint32_t nowMs, uint32_t thenMs, uint32_t durationMs);
    int findById(const std::string& id) const;
    int findFreeRecord() const;
    int findEvictableBackground() const;
    void makeTerminal(Job& job, State state, uint32_t nowMs);
    void releaseUnretainedTerminal(Job& job);
    void notifyChanged(const Job& job) const;
    void rememberDiscardedPath(const Job& job);
    std::string nextId();

    std::unique_ptr<Job[]> jobs_;
    Counters counters_{};
    uint32_t bootNonce_{0};
    uint64_t nextSequence_{1};
    uint32_t nextCounter_{1};
    std::unique_ptr<std::string[]> discardedPaths_;
    size_t discardedRead_{0};
    size_t discardedWrite_{0};
    size_t discardedCount_{0};
    ChangeCallback changeCallback_{nullptr};
    void* changeContext_{nullptr};
};

} // namespace bridge_jobs
