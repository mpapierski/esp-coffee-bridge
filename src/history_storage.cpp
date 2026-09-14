#include "history_storage.h"

#include <LittleFS.h>

#include <algorithm>
#include <atomic>

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "history_file_transaction.h"
#include "history_paging.h"

namespace history_storage {

namespace {

SemaphoreHandle_t gFilesystemMutex = nullptr;
portMUX_TYPE gFilesystemMutexInitLock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> gBulkRestoreMode{false};
std::atomic<uint32_t> gHistoryGeneration{1};
portMUX_TYPE gLockDiagnosticsMutex = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t gLockOwnerTask = nullptr;
uint32_t gLockOwnerDepth = 0;
int64_t gLockOwnerStartedUs = 0;
uint32_t gLockAcquisitions = 0;
uint32_t gLockContendedAcquisitions = 0;
uint32_t gLockTimedOutAcquisitions = 0;
uint32_t gLockMaxWaitMs = 0;
uint32_t gLockMaxHoldMs = 0;
constexpr size_t LOCK_OPERATION_BYTES = 40;
char gLockOwnerOperation[LOCK_OPERATION_BYTES]{};
char gLockMaxWaitOperation[LOCK_OPERATION_BYTES]{};
char gLockMaxHoldOperation[LOCK_OPERATION_BYTES]{};
char gLockLastTimeoutOperation[LOCK_OPERATION_BYTES]{};
char gLockLastTimeoutBlockedBy[LOCK_OPERATION_BYTES]{};

uint32_t elapsedMilliseconds(int64_t startedUs, int64_t finishedUs) {
    if (finishedUs <= startedUs) return 0;
    const uint64_t elapsedUs = static_cast<uint64_t>(finishedUs - startedUs);
    const uint64_t roundedMs = (elapsedUs + 999U) / 1000U;
    return roundedMs > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(roundedMs);
}

void incrementSaturated(uint32_t& value) {
    if (value != UINT32_MAX) value++;
}

void copyOperation(char* destination, const char* operation) {
    const char* source = operation != nullptr && operation[0] != '\0'
        ? operation : "unlabeled";
    size_t index = 0;
    while (index + 1 < LOCK_OPERATION_BYTES && source[index] != '\0') {
        destination[index] = source[index];
        index++;
    }
    destination[index] = '\0';
}

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

uint32_t historyGeneration() {
    return gHistoryGeneration.load(std::memory_order_acquire);
}

void noteHistoryMutation() {
    gHistoryGeneration.fetch_add(1, std::memory_order_release);
}

bool lockTagged(const char* operation, uint32_t timeoutMs) {
    if (!ensureMutex()) {
        return false;
    }
    const TaskHandle_t requester = xTaskGetCurrentTaskHandle();
    bool contended = false;
    portENTER_CRITICAL(&gLockDiagnosticsMutex);
    contended = gLockOwnerDepth != 0 && gLockOwnerTask != requester;
    portEXIT_CRITICAL(&gLockDiagnosticsMutex);

    const int64_t startedUs = esp_timer_get_time();
    const TickType_t waitTicks = timeoutMs == UINT32_MAX
        ? portMAX_DELAY
        : pdMS_TO_TICKS(timeoutMs);
    const bool acquired = xSemaphoreTakeRecursive(gFilesystemMutex, waitTicks) == pdTRUE;
    const int64_t finishedUs = esp_timer_get_time();
    const uint32_t waitMs = elapsedMilliseconds(startedUs, finishedUs);

    portENTER_CRITICAL(&gLockDiagnosticsMutex);
    if (!acquired) {
        incrementSaturated(gLockTimedOutAcquisitions);
        copyOperation(gLockLastTimeoutOperation, operation);
        copyOperation(gLockLastTimeoutBlockedBy,
                      gLockOwnerDepth != 0 ? gLockOwnerOperation : "released");
        portEXIT_CRITICAL(&gLockDiagnosticsMutex);
        return false;
    }

    incrementSaturated(gLockAcquisitions);
    if (contended || waitMs > 1) incrementSaturated(gLockContendedAcquisitions);
    if (waitMs > gLockMaxWaitMs) {
        gLockMaxWaitMs = waitMs;
        copyOperation(gLockMaxWaitOperation, operation);
    }
    if (gLockOwnerDepth == 0) {
        gLockOwnerTask = requester;
        gLockOwnerDepth = 1;
        gLockOwnerStartedUs = finishedUs;
        copyOperation(gLockOwnerOperation, operation);
    } else if (gLockOwnerTask == requester) {
        incrementSaturated(gLockOwnerDepth);
    }
    portEXIT_CRITICAL(&gLockDiagnosticsMutex);
    return true;
}

bool lock(uint32_t timeoutMs) {
    return lockTagged("unlabeled", timeoutMs);
}

void unlock() {
    if (gFilesystemMutex != nullptr) {
        const TaskHandle_t requester = xTaskGetCurrentTaskHandle();
        const int64_t finishedUs = esp_timer_get_time();
        portENTER_CRITICAL(&gLockDiagnosticsMutex);
        if (gLockOwnerDepth != 0 && gLockOwnerTask == requester) {
            gLockOwnerDepth--;
            if (gLockOwnerDepth == 0) {
                const uint32_t holdMs = elapsedMilliseconds(gLockOwnerStartedUs, finishedUs);
                if (holdMs > gLockMaxHoldMs) {
                    gLockMaxHoldMs = holdMs;
                    copyOperation(gLockMaxHoldOperation, gLockOwnerOperation);
                }
                gLockOwnerTask = nullptr;
                gLockOwnerStartedUs = 0;
                gLockOwnerOperation[0] = '\0';
            }
        }
        portEXIT_CRITICAL(&gLockDiagnosticsMutex);
        xSemaphoreGiveRecursive(gFilesystemMutex);
    }
}

LockDiagnostics lockDiagnostics() {
    LockDiagnostics diagnostics;
    char currentOperation[LOCK_OPERATION_BYTES]{};
    char maxWaitOperation[LOCK_OPERATION_BYTES]{};
    char maxHoldOperation[LOCK_OPERATION_BYTES]{};
    char lastTimeoutOperation[LOCK_OPERATION_BYTES]{};
    char lastTimeoutBlockedBy[LOCK_OPERATION_BYTES]{};
    const int64_t nowUs = esp_timer_get_time();

    portENTER_CRITICAL(&gLockDiagnosticsMutex);
    diagnostics.acquisitions = gLockAcquisitions;
    diagnostics.contendedAcquisitions = gLockContendedAcquisitions;
    diagnostics.timedOutAcquisitions = gLockTimedOutAcquisitions;
    diagnostics.maxWaitMs = gLockMaxWaitMs;
    diagnostics.maxHoldMs = gLockMaxHoldMs;
    diagnostics.held = gLockOwnerDepth != 0;
    diagnostics.currentHoldMs = diagnostics.held
        ? elapsedMilliseconds(gLockOwnerStartedUs, nowUs)
        : 0;
    copyOperation(currentOperation, diagnostics.held ? gLockOwnerOperation : "");
    copyOperation(maxWaitOperation, gLockMaxWaitOperation);
    copyOperation(maxHoldOperation, gLockMaxHoldOperation);
    copyOperation(lastTimeoutOperation, gLockLastTimeoutOperation);
    copyOperation(lastTimeoutBlockedBy, gLockLastTimeoutBlockedBy);
    portEXIT_CRITICAL(&gLockDiagnosticsMutex);

    diagnostics.currentOperation = diagnostics.held ? currentOperation : "";
    diagnostics.maxWaitOperation = diagnostics.maxWaitMs != 0 ? maxWaitOperation : "";
    diagnostics.maxHoldOperation = diagnostics.maxHoldMs != 0 ? maxHoldOperation : "";
    diagnostics.lastTimeoutOperation = diagnostics.timedOutAcquisitions != 0
        ? lastTimeoutOperation : "";
    diagnostics.lastTimeoutBlockedBy = diagnostics.timedOutAcquisitions != 0
        ? lastTimeoutBlockedBy : "";
    return diagnostics;
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
    const bool historyPath = originalPath.startsWith("/brewhist-") ||
        originalPath.startsWith("brewhist-") ||
        originalPath.startsWith("/stathist-") ||
        originalPath.startsWith("stathist-");
    if (historyPath && result != history_file_transaction::RecoveryResult::Unchanged) {
        // Recovery may replace or remove the file even when it ultimately
        // reports an error. Invalidate any optimistic backup snapshot.
        noteHistoryMutation();
    }
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
