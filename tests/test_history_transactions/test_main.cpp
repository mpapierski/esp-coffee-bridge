#include <unity.h>

#include <string>
#include <unordered_map>

#include "history_file_transaction.h"

namespace {

class FakeFiles {
public:
    bool exists(const std::string& path) const {
        return files.find(path) != files.end();
    }

    bool remove(const std::string& path) {
        if (failRemovePath == path) {
            return false;
        }
        return files.erase(path) > 0;
    }

    bool rename(const std::string& source, const std::string& destination) {
        renameCalls++;
        if (failRenameCall > 0 && renameCalls == failRenameCall) {
            return false;
        }
        const auto item = files.find(source);
        if (item == files.end() || exists(destination)) {
            return false;
        }
        files.emplace(destination, item->second);
        files.erase(item);
        return true;
    }

    const std::string& at(const std::string& path) const {
        return files.at(path);
    }

    std::unordered_map<std::string, std::string> files;
    std::string failRemovePath;
    size_t failRenameCall{0};
    size_t renameCalls{0};
};

constexpr const char* ORIGINAL = "/history.jsonl";
constexpr const char* TEMPORARY = "/history.jsonl.tmp";
constexpr const char* BACKUP = "/history.jsonl.bak";

history_file_transaction::ReplaceResult replace(FakeFiles& files, bool valid = true) {
    return history_file_transaction::replace(
        files,
        std::string(ORIGINAL),
        std::string(TEMPORARY),
        std::string(BACKUP),
        [&]() {
            return valid && files.exists(ORIGINAL) && files.at(ORIGINAL) == "new\n";
        });
}

void test_valid_rewrite_installs_new_file_and_removes_artifacts() {
    FakeFiles files;
    files.files[ORIGINAL] = "old\n";
    files.files[TEMPORARY] = "new\n";

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(history_file_transaction::ReplaceResult::Succeeded),
        static_cast<int>(replace(files)));
    TEST_ASSERT_EQUAL_STRING("new\n", files.at(ORIGINAL).c_str());
    TEST_ASSERT_FALSE(files.exists(TEMPORARY));
    TEST_ASSERT_FALSE(files.exists(BACKUP));
}

void test_failed_install_restores_original_and_cleans_temporary() {
    FakeFiles files;
    files.files[ORIGINAL] = "old\n";
    files.files[TEMPORARY] = "new\n";
    files.failRenameCall = 2; // Original-to-backup succeeds; install fails.

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(history_file_transaction::ReplaceResult::InstallFailed),
        static_cast<int>(replace(files)));
    TEST_ASSERT_EQUAL_STRING("old\n", files.at(ORIGINAL).c_str());
    TEST_ASSERT_FALSE(files.exists(TEMPORARY));
    TEST_ASSERT_FALSE(files.exists(BACKUP));
}

void test_failed_installed_validation_rolls_back_original() {
    FakeFiles files;
    files.files[ORIGINAL] = "old\n";
    files.files[TEMPORARY] = "new\n";

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(history_file_transaction::ReplaceResult::InstalledValidationFailed),
        static_cast<int>(replace(files, false)));
    TEST_ASSERT_EQUAL_STRING("old\n", files.at(ORIGINAL).c_str());
    TEST_ASSERT_FALSE(files.exists(TEMPORARY));
    TEST_ASSERT_FALSE(files.exists(BACKUP));
}

void test_failed_validation_reports_rollback_failure_and_preserves_backup() {
    FakeFiles files;
    files.files[ORIGINAL] = "old\n";
    files.files[TEMPORARY] = "new\n";
    files.failRemovePath = ORIGINAL;

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(
            history_file_transaction::ReplaceResult::InstalledValidationFailedAndRollbackFailed),
        static_cast<int>(replace(files, false)));
    TEST_ASSERT_EQUAL_STRING("new\n", files.at(ORIGINAL).c_str());
    TEST_ASSERT_EQUAL_STRING("old\n", files.at(BACKUP).c_str());
}

void test_recovery_restores_backup_after_interrupted_install() {
    FakeFiles files;
    files.files[BACKUP] = "old\n";

    const auto result = history_file_transaction::recover(
        files, std::string(ORIGINAL), std::string(BACKUP));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(history_file_transaction::RecoveryResult::RestoredBackup),
        static_cast<int>(result));
    TEST_ASSERT_EQUAL_STRING("old\n", files.at(ORIGINAL).c_str());
    TEST_ASSERT_FALSE(files.exists(BACKUP));
}

void test_recovery_rolls_back_when_commit_copy_still_exists() {
    FakeFiles files;
    files.files[ORIGINAL] = "new\n";
    files.files[BACKUP] = "old\n";

    const auto result = history_file_transaction::recover(
        files, std::string(ORIGINAL), std::string(BACKUP));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(history_file_transaction::RecoveryResult::RestoredBackup),
        static_cast<int>(result));
    TEST_ASSERT_EQUAL_STRING("old\n", files.at(ORIGINAL).c_str());
    TEST_ASSERT_FALSE(files.exists(BACKUP));
}

void test_failed_validation_backup_survives_until_recovery_can_restore_it() {
    FakeFiles files;
    files.files[ORIGINAL] = "old\n";
    files.files[TEMPORARY] = "new\n";
    files.failRemovePath = ORIGINAL;

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(
            history_file_transaction::ReplaceResult::InstalledValidationFailedAndRollbackFailed),
        static_cast<int>(replace(files, false)));
    files.failRemovePath.clear();
    TEST_ASSERT_TRUE(history_file_transaction::recovered(history_file_transaction::recover(
        files, std::string(ORIGINAL), std::string(BACKUP))));
    TEST_ASSERT_EQUAL_STRING("old\n", files.at(ORIGINAL).c_str());
    TEST_ASSERT_FALSE(files.exists(BACKUP));
}

void test_backup_cleanup_failure_rolls_back_instead_of_acknowledging() {
    FakeFiles files;
    files.files[ORIGINAL] = "old\n";
    files.files[TEMPORARY] = "new\n";
    files.failRemovePath = BACKUP;

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(history_file_transaction::ReplaceResult::BackupCleanupFailed),
        static_cast<int>(replace(files)));
    TEST_ASSERT_EQUAL_STRING("old\n", files.at(ORIGINAL).c_str());
    TEST_ASSERT_FALSE(files.exists(BACKUP));
}

void test_failed_compaction_validation_preserves_uncompacted_file() {
    FakeFiles files;
    constexpr const char* FULL_HISTORY = "line-0\nline-1\nline-2\nline-3\n";
    files.files[ORIGINAL] = FULL_HISTORY;
    files.files[TEMPORARY] = "line-2\nline-3\n";

    const auto result = history_file_transaction::replace(
        files,
        std::string(ORIGINAL),
        std::string(TEMPORARY),
        std::string(BACKUP),
        []() { return false; });
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(history_file_transaction::ReplaceResult::InstalledValidationFailed),
        static_cast<int>(result));
    TEST_ASSERT_EQUAL_STRING(FULL_HISTORY, files.at(ORIGINAL).c_str());
    TEST_ASSERT_FALSE(files.exists(BACKUP));
}

void test_patch_and_delete_rewrites_are_atomic() {
    FakeFiles files;
    files.files[ORIGINAL] = "line-0\nline-1\nline-2\n";
    files.files[TEMPORARY] = "line-0\npatched-1\nline-2\n";
    auto validatePatch = [&]() {
        return files.exists(ORIGINAL) &&
            files.at(ORIGINAL) == "line-0\npatched-1\nline-2\n";
    };
    TEST_ASSERT_TRUE(history_file_transaction::replaced(history_file_transaction::replace(
        files,
        std::string(ORIGINAL),
        std::string(TEMPORARY),
        std::string(BACKUP),
        validatePatch)));

    files.files[TEMPORARY] = "line-0\nline-2\n";
    auto validateDelete = [&]() {
        return files.exists(ORIGINAL) && files.at(ORIGINAL) == "line-0\nline-2\n";
    };
    TEST_ASSERT_TRUE(history_file_transaction::replaced(history_file_transaction::replace(
        files,
        std::string(ORIGINAL),
        std::string(TEMPORARY),
        std::string(BACKUP),
        validateDelete)));
    TEST_ASSERT_EQUAL_STRING("line-0\nline-2\n", files.at(ORIGINAL).c_str());
}

} // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_rewrite_installs_new_file_and_removes_artifacts);
    RUN_TEST(test_failed_install_restores_original_and_cleans_temporary);
    RUN_TEST(test_failed_installed_validation_rolls_back_original);
    RUN_TEST(test_failed_validation_reports_rollback_failure_and_preserves_backup);
    RUN_TEST(test_recovery_restores_backup_after_interrupted_install);
    RUN_TEST(test_recovery_rolls_back_when_commit_copy_still_exists);
    RUN_TEST(test_failed_validation_backup_survives_until_recovery_can_restore_it);
    RUN_TEST(test_backup_cleanup_failure_rolls_back_instead_of_acknowledging);
    RUN_TEST(test_failed_compaction_validation_preserves_uncompacted_file);
    RUN_TEST(test_patch_and_delete_rewrites_are_atomic);
    return UNITY_END();
}
