#include "history_storage.h"

#include <LittleFS.h>

#include <algorithm>
#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "history_file_transaction.h"
#include "history_paging.h"

namespace history_storage {

namespace {

SemaphoreHandle_t gFilesystemMutex = nullptr;
portMUX_TYPE gFilesystemMutexInitLock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> gBulkRestoreMode{false};

bool ensureMutex() {
    if (gFilesystemMutex != nullptr) {
        return true;
    }

    SemaphoreHandle_t candidate = xSemaphoreCreateRecursiveMutex();
    if (candidate == nullptr) {
        return false;
    }

    portENTER_CRITICAL(&gFilesystemMutexInitLock);
    if (gFilesystemMutex == nullptr) {
        gFilesystemMutex = candidate;
        candidate = nullptr;
    }
    portEXIT_CRITICAL(&gFilesystemMutexInitLock);

    if (candidate != nullptr) {
        vSemaphoreDelete(candidate);
    }
    return gFilesystemMutex != nullptr;
}

struct LittleFsTransactionBackend {
    bool exists(const String& path) const {
        return LittleFS.exists(path);
    }

    bool remove(const String& path) const {
        return LittleFS.remove(path);
    }

    bool rename(const String& source, const String& destination) const {
        return LittleFS.rename(source, destination);
    }
};

} // namespace

bool begin() {
    return ensureMutex();
}

void setBulkRestoreMode(bool enabled) {
    gBulkRestoreMode.store(enabled, std::memory_order_release);
}

size_t writeReserveBytes(size_t filesystemBytes) {
    if (gBulkRestoreMode.load(std::memory_order_acquire)) {
        // A validated bulk restore only appends pre-sized files. Its rollback
        // copies and upload staging file are already
        // included in usedBytes(), so only the emergency headroom remains.
        return OPERATIONAL_HEADROOM_BYTES;
    }
    return operationalReserveBytes(filesystemBytes);
}

bool lock(uint32_t timeoutMs) {
    if (!ensureMutex()) {
        return false;
    }
    const TickType_t waitTicks = timeoutMs == UINT32_MAX
        ? portMAX_DELAY
        : pdMS_TO_TICKS(timeoutMs);
    return xSemaphoreTakeRecursive(gFilesystemMutex, waitTicks) == pdTRUE;
}

void unlock() {
    if (gFilesystemMutex != nullptr) {
        xSemaphoreGiveRecursive(gFilesystemMutex);
    }
}

bool inspectFile(const String& path, FileValidation& validationOut, String& error) {
    error = "";
    validationOut = FileValidation{};

    Guard filesystem;
    if (!filesystem) {
        error = "failed to lock history storage";
        return false;
    }

    File file = LittleFS.open(path, "r");
    if (!file) {
        error = String("failed to open temporary history file ") + path;
        return false;
    }

    history_paging::PhysicalLineCounter counter;
    char buffer[256];
    while (file.available()) {
        const size_t readCount = file.read(reinterpret_cast<uint8_t*>(buffer), sizeof(buffer));
        if (readCount == 0) {
            file.close();
            error = String("failed to read temporary history file ") + path;
            return false;
        }
        counter.consume(buffer, readCount);
        for (size_t index = 0; index < readCount; ++index) {
            validationOut.checksum ^= static_cast<uint8_t>(buffer[index]);
            validationOut.checksum *= 16777619u;
        }
    }
    file.close();

    validationOut.bytes = counter.byteCount();
    validationOut.physicalLines = counter.lineCount();
    return true;
}

bool inspectHistoryUsage(HistoryUsage& usageOut, String& error) {
    error = "";
    usageOut = HistoryUsage{};

    Guard filesystem;
    if (!filesystem) {
        error = "failed to lock history storage";
        return false;
    }

    File root = LittleFS.open("/");
    if (!root) {
        error = "failed to open history storage root";
        return false;
    }

    File entry = root.openNextFile();
    while (entry) {
        String path = entry.name();
        const size_t bytes = entry.size();
        const bool brew = path.startsWith("/brewhist-") ||
            path.startsWith("brewhist-");
        const bool stats = path.startsWith("/stathist-") ||
            path.startsWith("stathist-");
        entry.close();

        // Rewrite/rollback artifacts do not form part of the durable history
        // preservation floor. Boot recovery handles those separately.
        if ((brew || stats) && path.endsWith(".jsonl")) {
            usageOut.fileCount++;
            usageOut.totalBytes += bytes;
            if (brew) {
                usageOut.brewFileCount++;
                usageOut.brewTotalBytes += bytes;
                usageOut.largestBrewFileBytes =
                    std::max(usageOut.largestBrewFileBytes, bytes);
            } else {
                usageOut.statsFileCount++;
                usageOut.statsTotalBytes += bytes;
                usageOut.largestStatsFileBytes =
                    std::max(usageOut.largestStatsFileBytes, bytes);
            }
        }
        entry = root.openNextFile();
    }
    root.close();
    return true;
}

bool recoverFile(const String& originalPath, String& error) {
    error = "";
    Guard filesystem;
    if (!filesystem) {
        error = "failed to lock history storage";
        return false;
    }
    const String backupPath = originalPath + ".bak";
    LittleFsTransactionBackend backend;
    const history_file_transaction::RecoveryResult result =
        history_file_transaction::recover(backend, originalPath, backupPath);
    switch (result) {
        case history_file_transaction::RecoveryResult::Unchanged:
        case history_file_transaction::RecoveryResult::RestoredBackup:
            return true;
        case history_file_transaction::RecoveryResult::RemoveInstalledFileFailed:
            error = "failed to remove an uncommitted history replacement";
            return false;
        case history_file_transaction::RecoveryResult::RestoreBackupFailed:
            error = "failed to recover history file from rollback copy";
            return false;
    }
    error = "unknown history rollback recovery result";
    return false;
}

bool commitTemporaryFile(const String& originalPath,
                         const String& temporaryPath,
                         size_t maximumBytes,
                         size_t expectedPhysicalLines,
                         String& error) {
    error = "";
    Guard filesystem;
    if (!filesystem) {
        error = "failed to lock history storage";
        return false;
    }
    FileValidation temporary;
    if (!inspectFile(temporaryPath, temporary, error)) {
        LittleFS.remove(temporaryPath);
        return false;
    }
    if (temporary.bytes > maximumBytes) {
        LittleFS.remove(temporaryPath);
        error = "temporary history file exceeds the configured size limit";
        return false;
    }
    if (temporary.physicalLines != expectedPhysicalLines) {
        LittleFS.remove(temporaryPath);
        error = "temporary history file failed physical-line validation";
        return false;
    }

    const String backupPath = originalPath + ".bak";
    FileValidation installed;
    String validationError;
    LittleFsTransactionBackend backend;
    const history_file_transaction::ReplaceResult result = history_file_transaction::replace(
        backend,
        originalPath,
        temporaryPath,
        backupPath,
        [&]() {
            return inspectFile(originalPath, installed, validationError) &&
                installed.bytes == temporary.bytes &&
                installed.physicalLines == temporary.physicalLines &&
                installed.checksum == temporary.checksum;
        });
    switch (result) {
        case history_file_transaction::ReplaceResult::Succeeded:
            return true;
        case history_file_transaction::ReplaceResult::RecoveryFailed:
            error = "failed to recover a prior history rollback file";
            break;
        case history_file_transaction::ReplaceResult::CreateBackupFailed:
            error = "failed to create history rollback file";
            break;
        case history_file_transaction::ReplaceResult::InstallFailed:
            error = "failed to install rewritten history file";
            break;
        case history_file_transaction::ReplaceResult::InstallFailedAndRollbackFailed:
            error = "failed to install rewritten history file; failed to restore original from backup";
            break;
        case history_file_transaction::ReplaceResult::InstalledValidationFailed:
            error = validationError.isEmpty()
                ? String("installed history file failed checksum validation")
                : validationError;
            break;
        case history_file_transaction::ReplaceResult::InstalledValidationFailedAndRollbackFailed:
            error = validationError.isEmpty()
                ? String("installed history file failed checksum validation")
                : validationError;
            error += "; failed to restore original from backup";
            break;
        case history_file_transaction::ReplaceResult::BackupCleanupFailed:
            error = "failed to finalize rewritten history file; original was restored";
            break;
        case history_file_transaction::ReplaceResult::BackupCleanupFailedAndRollbackFailed:
            error = "failed to finalize rewritten history file; rollback remains pending";
            break;
    }
    return false;
}

} // namespace history_storage
