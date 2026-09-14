#pragma once

#include <cstddef>

namespace history_capacity {

inline constexpr size_t LITTLEFS_PARTITION_BYTES = 8 * 1024 * 1024;

// The 8 MiB partition has a 6,000 KiB writable-history limit after
// transactional and operational reserves. Backup history entries are grouped
// into bounded JSON arrays so the per-record kind/serial envelope is amortized
// even for the smallest valid entries. These deliberately conservative schema
// minima and escaped-envelope bounds make the 7,500 KiB cap a checked worst
// case instead of a percentage estimate.
inline constexpr size_t CURRENT_WRITABLE_HISTORY_BYTES = 6000 * 1024;
inline constexpr size_t MAX_GENERATED_BACKUP_BYTES = 7500 * 1024;
inline constexpr size_t MAX_BACKUP_JSON_LINE_BYTES = 9 * 1024;
inline constexpr size_t MAX_BACKUP_RECORD_ENVELOPE_BYTES = 384;
inline constexpr size_t MAX_NON_HISTORY_BACKUP_BYTES = 128 * 1024;
inline constexpr size_t MIN_BREW_HISTORY_ENTRY_BYTES = 128;
inline constexpr size_t MIN_STATS_HISTORY_ENTRY_BYTES = 24;
inline constexpr size_t MAX_BREW_ENTRIES_PER_BACKUP_RECORD = 32;
inline constexpr size_t MAX_STATS_ENTRIES_PER_BACKUP_RECORD = 128;
inline constexpr size_t BACKUP_RECORD_SUFFIX_BYTES = 3; // ]}\n

inline constexpr size_t minimumCountFlushedHistoryBytes() {
    constexpr size_t brewBytes =
        MIN_BREW_HISTORY_ENTRY_BYTES * MAX_BREW_ENTRIES_PER_BACKUP_RECORD;
    constexpr size_t statsBytes =
        MIN_STATS_HISTORY_ENTRY_BYTES * MAX_STATS_ENTRIES_PER_BACKUP_RECORD;
    return brewBytes < statsBytes ? brewBytes : statsBytes;
}

inline constexpr size_t minimumSizeFlushedHistoryBytes() {
    // Two adjacent records share at most one entry at each boundary. Charging
    // half of the payload that forced a split therefore bounds every split.
    return (MAX_BACKUP_JSON_LINE_BYTES - MAX_BACKUP_RECORD_ENVELOPE_BYTES) / 2;
}

inline constexpr size_t divideRoundUp(size_t value, size_t divisor) {
    return value / divisor + (value % divisor != 0 ? 1 : 0);
}

inline constexpr size_t maximumBatchedBackupBytes(size_t historyBytes,
                                                   size_t historyFileCount) {
    const size_t maximumRecordCount =
        divideRoundUp(historyBytes, minimumCountFlushedHistoryBytes()) +
        divideRoundUp(historyBytes, minimumSizeFlushedHistoryBytes()) +
        historyFileCount;
    return historyBytes +
        maximumRecordCount * MAX_BACKUP_RECORD_ENVELOPE_BYTES +
        MAX_NON_HISTORY_BACKUP_BYTES;
}

inline constexpr bool backupRecordEntryFits(size_t recordBytesWithoutSuffix,
                                             size_t currentEntryCount,
                                             size_t entryBytes,
                                             size_t maximumEntryCount) {
    if (currentEntryCount >= maximumEntryCount ||
        recordBytesWithoutSuffix > MAX_BACKUP_JSON_LINE_BYTES) {
        return false;
    }
    const size_t separatorBytes = currentEntryCount == 0 ? 0 : 1;
    const size_t availableBytes =
        MAX_BACKUP_JSON_LINE_BYTES - recordBytesWithoutSuffix;
    return availableBytes >= separatorBytes + BACKUP_RECORD_SUFFIX_BYTES &&
        entryBytes <= availableBytes - separatorBytes - BACKUP_RECORD_SUFFIX_BYTES;
}

inline constexpr size_t LITTLEFS_ALLOCATION_BYTES = 4096;
inline constexpr size_t MAX_HISTORY_FILE_COUNT = 32;
inline constexpr size_t RESTORE_METADATA_RESERVE_BYTES = 48 * 1024;

static_assert(maximumBatchedBackupBytes(CURRENT_WRITABLE_HISTORY_BYTES,
                                        MAX_HISTORY_FILE_COUNT) <=
                  MAX_GENERATED_BACKUP_BYTES,
              "Worst-case valid history backup must fit the export cap");

} // namespace history_capacity
