#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bridge_http {

constexpr size_t EVENT_JOB_ID_BYTES = 32;
constexpr size_t EVENT_CHANGE_CAPACITY = 16;

enum class EventChangeKind : uint8_t {
    Job,
    Brew,
};

struct EventChangeBatch {
    bool statusDirty{false};
    bool resync{false};
    size_t count{0};
    std::array<EventChangeKind, EVENT_CHANGE_CAPACITY> kinds{};
    std::array<std::array<char, EVENT_JOB_ID_BYTES>, EVENT_CHANGE_CAPACITY> ids{};
};

// The caller supplies synchronization. Keeping this type platform-neutral
// makes queue pressure/coalescing behavior testable in native builds.
class EventChanges {
public:
    void markStatus() {
        statusDirty_ = true;
    }

    void markJob(const char* id) {
        mark(EventChangeKind::Job, id);
    }

    void markBrew(const char* id) {
        mark(EventChangeKind::Brew, id);
    }

    bool pending() const {
        return statusDirty_ || resync_ || count_ != 0;
    }

    EventChangeBatch take() {
        EventChangeBatch batch;
        batch.statusDirty = statusDirty_;
        batch.resync = resync_;
        batch.count = count_;
        for (size_t index = 0; index < count_; ++index) {
            batch.kinds[index] = kinds_[index];
            batch.ids[index] = ids_[index];
            ids_[index][0] = '\0';
        }
        statusDirty_ = false;
        resync_ = false;
        count_ = 0;
        return batch;
    }

private:
    void mark(EventChangeKind kind, const char* id) {
        statusDirty_ = true;
        if (id == nullptr || id[0] == '\0' || std::strlen(id) >= EVENT_JOB_ID_BYTES) {
            resync_ = true;
            return;
        }
        for (size_t index = 0; index < count_; ++index) {
            if (kinds_[index] == kind && std::strcmp(ids_[index].data(), id) == 0) return;
        }
        if (count_ == EVENT_CHANGE_CAPACITY) {
            resync_ = true;
            return;
        }
        kinds_[count_] = kind;
        std::strncpy(ids_[count_].data(), id, EVENT_JOB_ID_BYTES - 1);
        ids_[count_][EVENT_JOB_ID_BYTES - 1] = '\0';
        count_++;
    }

    bool statusDirty_{false};
    bool resync_{false};
    size_t count_{0};
    std::array<EventChangeKind, EVENT_CHANGE_CAPACITY> kinds_{};
    std::array<std::array<char, EVENT_JOB_ID_BYTES>, EVENT_CHANGE_CAPACITY> ids_{};
};

} // namespace bridge_http
