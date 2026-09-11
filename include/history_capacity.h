#pragma once

#include <cstddef>

namespace history_capacity {

// A generated bundle must be able to contain the complete writable history
// set (576 KiB on the current 960 KiB LittleFS partition), machine records,
// and the NDJSON envelope. Uploads are staged in separate bounded chunk files;
// restore deletes fully consumed chunks without shifting the unread tail.
// Peak-space preflight also includes the old history retained for rollback.
inline constexpr size_t MAX_GENERATED_BACKUP_BYTES = 720 * 1024;
inline constexpr size_t LITTLEFS_ALLOCATION_BYTES = 4096;
inline constexpr size_t MAX_HISTORY_FILE_COUNT = 32;
inline constexpr size_t RESTORE_METADATA_RESERVE_BYTES = 48 * 1024;

} // namespace history_capacity
