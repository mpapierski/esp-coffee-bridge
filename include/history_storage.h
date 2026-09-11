#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

namespace history_storage {

inline constexpr size_t MAX_JSON_LINE_BYTES = 8192;
inline constexpr size_t OPERATIONAL_HEADROOM_BYTES = 192 * 1024;

inline constexpr size_t transactionalFileLimit(size_t filesystemBytes) {
    return filesystemBytes > OPERATIONAL_HEADROOM_BYTES
        ? (filesystemBytes - OPERATIONAL_HEADROOM_BYTES) / 4
        : filesystemBytes / 4;
}

inline constexpr size_t operationalReserveBytes(size_t filesystemBytes) {
    return OPERATIONAL_HEADROOM_BYTES + transactionalFileLimit(filesystemBytes);
}

inline constexpr size_t writableHistoryLimit(size_t filesystemBytes) {
    const size_t reserve = operationalReserveBytes(filesystemBytes);
    return filesystemBytes > reserve ? filesystemBytes - reserve : 0;
}

struct HistoryUsage {
    size_t fileCount{0};
    size_t totalBytes{0};
    size_t brewFileCount{0};
    size_t brewTotalBytes{0};
    size_t largestBrewFileBytes{0};
    size_t statsFileCount{0};
    size_t statsTotalBytes{0};
    size_t largestStatsFileBytes{0};
};

void setBulkRestoreMode(bool enabled);
size_t writeReserveBytes(size_t filesystemBytes);

struct FileValidation {
    size_t bytes{0};
    size_t physicalLines{0};
    uint32_t checksum{2166136261u};
};

// Initialize the one recursive mutex used to serialize all LittleFS access.
// Calls are idempotent; history operations also initialize it lazily.
bool begin();
inline constexpr uint32_t DEFAULT_LOCK_TIMEOUT_MS = 5000;

bool lock(uint32_t timeoutMs = DEFAULT_LOCK_TIMEOUT_MS);
void unlock();

class Guard {
public:
    explicit Guard(uint32_t timeoutMs = DEFAULT_LOCK_TIMEOUT_MS) : locked_(lock(timeoutMs)) {}
    ~Guard() {
        if (locked_) {
            unlock();
        }
    }

    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

    explicit operator bool() const {
        return locked_;
    }

private:
    bool locked_{false};
};

bool inspectFile(const String& path, FileValidation& validationOut, String& error);

// Inspect only history filenames and file sizes. The scan is serialized by the
// shared filesystem mutex and never parses history contents.
bool inspectHistoryUsage(HistoryUsage& usageOut, String& error);

// Finish recovery from an interrupted swap. If only the rollback file exists,
// restore it; if both files exist, the installed original won the prior swap.
bool recoverFile(const String& originalPath, String& error);

// Validate a completed temporary file, swap it into place, validate the
// installed copy, and restore the original from .bak if any swap step fails.
// The caller must have closed all handles for originalPath and temporaryPath.
bool commitTemporaryFile(const String& originalPath,
                         const String& temporaryPath,
                         size_t maximumBytes,
                         size_t expectedPhysicalLines,
                         String& error);

} // namespace history_storage
