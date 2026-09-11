#pragma once

#include <array>
#include <cstddef>
#include <cstring>

namespace bridge_http {

constexpr size_t EVENT_JOB_ID_BYTES = 32;
constexpr size_t EVENT_CHANGE_CAPACITY = 16;

struct EventChangeBatch {
    bool statusDirty{false};
    bool resync{false};
    size_t jobCount{0};
    std::array<std::array<char, EVENT_JOB_ID_BYTES>, EVENT_CHANGE_CAPACITY> jobIds{};
};

// The caller supplies synchronization. Keeping this type platform-neutral
// makes queue pressure/coalescing behavior testable in native builds.
class EventChanges {
public:
    void markStatus() {
        statusDirty_ = true;
    }

    void markJob(const char* id) {
        statusDirty_ = true;
        if (id == nullptr || id[0] == '\0' || std::strlen(id) >= EVENT_JOB_ID_BYTES) {
            resync_ = true;
            return;
        }
        for (size_t index = 0; index < count_; ++index) {
            if (std::strcmp(jobIds_[index].data(), id) == 0) {
                return;
            }
        }
        if (count_ == EVENT_CHANGE_CAPACITY) {
            resync_ = true;
            return;
        }
        std::strncpy(jobIds_[count_].data(), id, EVENT_JOB_ID_BYTES - 1);
        jobIds_[count_][EVENT_JOB_ID_BYTES - 1] = '\0';
        count_++;
    }

    bool pending() const {
        return statusDirty_ || resync_ || count_ != 0;
    }

    EventChangeBatch take() {
        EventChangeBatch batch;
        batch.statusDirty = statusDirty_;
        batch.resync = resync_;
        batch.jobCount = count_;
        for (size_t index = 0; index < count_; ++index) {
            batch.jobIds[index] = jobIds_[index];
            jobIds_[index][0] = '\0';
        }
        statusDirty_ = false;
        resync_ = false;
        count_ = 0;
        return batch;
    }

private:
    bool statusDirty_{false};
    bool resync_{false};
    size_t count_{0};
    std::array<std::array<char, EVENT_JOB_ID_BYTES>, EVENT_CHANGE_CAPACITY> jobIds_{};
};

} // namespace bridge_http
