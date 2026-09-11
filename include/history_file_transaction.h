#pragma once

// Pure rename/install/rollback sequencing for history-file rewrites.  The
// backend only needs exists/remove/rename methods, which keeps the transaction
// behavior host-testable without LittleFS or Arduino dependencies.
namespace history_file_transaction {

enum class RecoveryResult {
    Unchanged,
    RestoredBackup,
    RemoveInstalledFileFailed,
    RestoreBackupFailed,
};

inline constexpr bool recovered(RecoveryResult result) {
    return result == RecoveryResult::Unchanged ||
        result == RecoveryResult::RestoredBackup;
}

template <typename Backend, typename Path>
RecoveryResult recover(Backend& backend, const Path& original, const Path& backup) {
    if (!backend.exists(backup)) {
        return RecoveryResult::Unchanged;
    }
    // The rollback copy is the commit boundary. If it still exists, an
    // installed original may be unvalidated (or validation rollback may have
    // failed), so always prefer the known pre-transaction copy.
    if (backend.exists(original) && !backend.remove(original)) {
        return RecoveryResult::RemoveInstalledFileFailed;
    }
    return backend.rename(backup, original)
        ? RecoveryResult::RestoredBackup
        : RecoveryResult::RestoreBackupFailed;
}

enum class ReplaceResult {
    Succeeded,
    RecoveryFailed,
    CreateBackupFailed,
    InstallFailed,
    InstallFailedAndRollbackFailed,
    InstalledValidationFailed,
    InstalledValidationFailedAndRollbackFailed,
    BackupCleanupFailed,
    BackupCleanupFailedAndRollbackFailed,
};

inline constexpr bool replaced(ReplaceResult result) {
    return result == ReplaceResult::Succeeded;
}

// `validateInstalled` is called only after temporary has been renamed to
// original.  A false result removes that invalid replacement and restores the
// prior original when one existed.
template <typename Backend, typename Path, typename Validator>
ReplaceResult replace(Backend& backend,
                      const Path& original,
                      const Path& temporary,
                      const Path& backup,
                      Validator validateInstalled) {
    if (!recovered(recover(backend, original, backup))) {
        backend.remove(temporary);
        return ReplaceResult::RecoveryFailed;
    }

    const bool hadOriginal = backend.exists(original);
    if (hadOriginal && !backend.rename(original, backup)) {
        backend.remove(temporary);
        return ReplaceResult::CreateBackupFailed;
    }

    if (!backend.rename(temporary, original)) {
        backend.remove(temporary);
        if (hadOriginal && !backend.rename(backup, original)) {
            return ReplaceResult::InstallFailedAndRollbackFailed;
        }
        return ReplaceResult::InstallFailed;
    }

    if (!validateInstalled()) {
        const bool removedInvalid = !backend.exists(original) || backend.remove(original);
        const bool restoredOriginal = !hadOriginal ||
            (!backend.exists(original) && backend.rename(backup, original));
        if (!removedInvalid || !restoredOriginal) {
            return ReplaceResult::InstalledValidationFailedAndRollbackFailed;
        }
        return ReplaceResult::InstalledValidationFailed;
    }

    if (hadOriginal && !backend.remove(backup)) {
        // Do not acknowledge an install whose rollback copy could not be
        // removed: recovery cannot distinguish it from an unvalidated
        // original. Roll back now, preserving the backup on any failure.
        const bool removedInstalled = !backend.exists(original) || backend.remove(original);
        const bool restoredOriginal = removedInstalled && backend.rename(backup, original);
        return removedInstalled && restoredOriginal
            ? ReplaceResult::BackupCleanupFailed
            : ReplaceResult::BackupCleanupFailedAndRollbackFailed;
    }
    return ReplaceResult::Succeeded;
}

} // namespace history_file_transaction
