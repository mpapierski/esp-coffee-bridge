#include <unity.h>

#include <cstring>
#include <map>
#include <string>

#include "backup_staging.h"
#include "history_storage.h"

namespace {

class Files {
public:
    bool openWrite(size_t index) {
        close();
        if (files.count(index)) { ++overwrites; return false; }
        files[index] = {};
        active = index;
        return true;
    }
    bool openRead(size_t index) {
        close();
        if (!files.count(index)) return false;
        active = index;
        return true;
    }
    size_t size() const { return files.at(active).size(); }
    size_t write(const uint8_t* bytes, size_t count) {
        if (failWrite) return 0;
        files.at(active).append(reinterpret_cast<const char*>(bytes), count);
        written += count;
        return count;
    }
    size_t read(uint8_t* bytes, size_t count) {
        count = std::min(count, files.at(active).size() - offset);
        if (shortRead && count) --count;
        memcpy(bytes, files.at(active).data() + offset, count);
        offset += count;
        return count;
    }
    void close() { active = SIZE_MAX; offset = 0; }
    bool exists(size_t index) const { return files.count(index); }
    bool remove(size_t index) {
        if (active == index || index == failRemove) return false;
        return files.erase(index) == 1;
    }
    std::map<size_t, std::string> files;
    size_t active{SIZE_MAX};
    size_t offset{0};
    size_t written{0};
    size_t overwrites{0};
    size_t failRemove{SIZE_MAX};
    bool shortRead{false};
    bool failWrite{false};
};

using Reader = backup_staging::Reader<Files>;
static_assert(sizeof(Reader) < 1024, "Staging reader must use bounded RAM");

std::string payload(size_t bytes) {
    std::string result;
    result.reserve(bytes);
    for (size_t i = 0; i < bytes; ++i) {
        result += i % 997 == 996 ? '\n' : static_cast<char>('a' + i % 26);
    }
    return result;
}

void stage(Files& files, const std::string& source) {
    backup_staging::Writer<Files> writer(files);
    // Deliberately straddle both filesystem and chunk boundaries.
    for (size_t offset = 0; offset < source.size(); offset += 1460) {
        const size_t bytes = std::min(size_t(1460), source.size() - offset);
        TEST_ASSERT_TRUE(writer.write(reinterpret_cast<const uint8_t*>(source.data() + offset), bytes));
    }
    writer.close();
    TEST_ASSERT_EQUAL(source.size(), writer.size());
}

void test_fragmented_upload_is_readable_in_multiple_nondestructive_passes() {
    Files files;
    const auto source = payload(600 * 1024);
    stage(files, source);
    TEST_ASSERT_EQUAL(backup_staging::chunkCount(source.size()), files.files.size());
    for (int pass = 0; pass < 3; ++pass) {
        Reader reader(files, source.size());
        for (const unsigned char expected : source) TEST_ASSERT_EQUAL_INT(expected, reader.read());
        TEST_ASSERT_FALSE(reader.available());
        TEST_ASSERT_EQUAL_INT(-1, reader.read());
        TEST_ASSERT_EQUAL(source.size(), reader.position());
    }
    TEST_ASSERT_EQUAL(source.size(), files.written);
    TEST_ASSERT_EQUAL(0, files.overwrites);
}

void test_record_boundaries_reclaim_chunks_without_rewriting_unread_data() {
    Files files;
    const auto source = payload(backup_staging::MAX_BYTES);
    stage(files, source);
    Reader reader(files, source.size());
    for (size_t index = 0; index < source.size(); ++index) {
        TEST_ASSERT_EQUAL_INT(static_cast<unsigned char>(source[index]), reader.read());
        if (source[index] == '\n') {
            TEST_ASSERT_TRUE(reader.releaseConsumed());
            TEST_ASSERT_EQUAL(backup_staging::chunkCount(source.size()) -
                backup_staging::consumedChunks(index + 1, source.size()), files.files.size());
        }
    }
    TEST_ASSERT_TRUE(reader.releaseConsumed());
    TEST_ASSERT_TRUE(files.files.empty());
    TEST_ASSERT_EQUAL(source.size(), files.written);
    TEST_ASSERT_EQUAL(0, files.overwrites);
}

void test_partial_chunk_is_retained_until_its_last_byte_is_consumed() {
    Files files;
    stage(files, payload(backup_staging::CHUNK_BYTES + 1));
    Reader reader(files, backup_staging::CHUNK_BYTES + 1);
    for (size_t i = 0; i < backup_staging::CHUNK_BYTES - 1; ++i) reader.read();
    TEST_ASSERT_TRUE(reader.releaseConsumed());
    TEST_ASSERT_TRUE(files.exists(0));
    reader.read();
    TEST_ASSERT_TRUE(reader.releaseConsumed());
    TEST_ASSERT_FALSE(files.exists(0));
    TEST_ASSERT_TRUE(files.exists(1));
    reader.read();
    TEST_ASSERT_TRUE(reader.releaseConsumed());
    TEST_ASSERT_TRUE(files.files.empty());
}

void test_missing_truncated_and_short_read_chunks_fail_closed() {
    for (int fault = 0; fault < 3; ++fault) {
        Files files;
        const size_t bytes = backup_staging::CHUNK_BYTES + 20;
        stage(files, payload(bytes));
        if (fault == 0) files.files.erase(1);
        if (fault == 1) files.files[1].pop_back();
        Reader reader(files, bytes);
        for (size_t i = 0; i < backup_staging::CHUNK_BYTES; ++i) TEST_ASSERT_TRUE(reader.read() >= 0);
        if (fault == 2) files.shortRead = true;
        TEST_ASSERT_EQUAL_INT(-1, reader.read());
        TEST_ASSERT_FALSE(bool(reader));
        TEST_ASSERT_FALSE(reader.releaseConsumed());
    }
}

void test_cleanup_handles_partial_upload_and_interrupted_consumption() {
    Files files;
    stage(files, payload(3 * backup_staging::CHUNK_BYTES));
    {
        Reader reader(files, 3 * backup_staging::CHUNK_BYTES);
        for (size_t i = 0; i < backup_staging::CHUNK_BYTES; ++i) reader.read();
        TEST_ASSERT_TRUE(reader.releaseConsumed());
    }
    // Simulate reboot cleanup, with an unrelated retained history artifact.
    files.files[backup_staging::MAX_CHUNKS + 1] = "old rollback history";
    files.failRemove = 1;
    TEST_ASSERT_FALSE(backup_staging::removeAll(files));
    TEST_ASSERT_TRUE(files.exists(1));
    TEST_ASSERT_FALSE(files.exists(2));
    files.failRemove = SIZE_MAX;
    TEST_ASSERT_TRUE(backup_staging::removeAll(files));
    TEST_ASSERT_EQUAL_STRING("old rollback history", files.files.at(backup_staging::MAX_CHUNKS + 1).c_str());
    files.files.clear();
    backup_staging::Writer<Files> writer(files);
    const uint8_t data[] = {1, 2, 3};
    TEST_ASSERT_TRUE(writer.write(data, sizeof(data)));
    files.failWrite = true;
    TEST_ASSERT_FALSE(writer.write(data, sizeof(data)));
    TEST_ASSERT_TRUE(backup_staging::removeAll(files));
    TEST_ASSERT_TRUE(files.files.empty());
}

void test_size_limit_empty_input_and_reclaim_errors() {
    Files files;
    TEST_ASSERT_FALSE(bool(Reader(files, 0)));
    TEST_ASSERT_FALSE(bool(Reader(files, backup_staging::MAX_BYTES + 1)));
    backup_staging::Writer<Files> writer(files);
    const uint8_t byte = 1;
    TEST_ASSERT_FALSE(writer.write(&byte, backup_staging::MAX_BYTES + 1));
    TEST_ASSERT_TRUE(files.files.empty());
    writer.reset();
    TEST_ASSERT_TRUE(writer.write(&byte, 1));
    writer.close();
    Reader reader(files, 1);
    TEST_ASSERT_EQUAL_INT(1, reader.read());
    files.failRemove = 0;
    TEST_ASSERT_FALSE(reader.releaseConsumed());
    TEST_ASSERT_TRUE(files.exists(0));
}

void test_peak_preflight_accounts_for_unread_upload_and_normalization_growth() {
    const size_t total = 600 * 1024;
    backup_staging::PeakUsage peak(total);
    TEST_ASSERT_TRUE(peak.observe(backup_staging::CHUNK_BYTES - 1, 50000));
    TEST_ASSERT_EQUAL(total + 50000, peak.bytes());
    TEST_ASSERT_TRUE(peak.observe(backup_staging::CHUNK_BYTES, 50000));
    TEST_ASSERT_EQUAL(total + 50000, peak.bytes());
    TEST_ASSERT_TRUE(peak.observe(total, 700000));
    TEST_ASSERT_EQUAL(700000, peak.bytes());
    TEST_ASSERT_FALSE(peak.observe(total + 1, 0));
    TEST_ASSERT_FALSE(peak.observe(0, SIZE_MAX));
}

void test_capacity_preflight_keeps_old_history_and_allocation_slack_charged() {
    constexpr size_t upload = 600 * 1024, reserve = 240 * 1024;
    constexpr size_t uploadSlack = 8 * 1024;
    TEST_ASSERT_EQUAL((600 + 8 + 64 + 8 + 240) * 1024,
        backup_staging::requiredCapacity(upload + uploadSlack + 64 * 1024, upload, upload, 2, reserve));
    constexpr size_t maximumUpload = history_capacity::MAX_GENERATED_BACKUP_BYTES;
    TEST_ASSERT_GREATER_THAN(history_capacity::LITTLEFS_PARTITION_BYTES,
        backup_staging::requiredCapacity(maximumUpload + 512 * 1024,
                                         maximumUpload,
                                         maximumUpload,
                                         2,
                                         reserve));
    TEST_ASSERT_EQUAL(SIZE_MAX, backup_staging::requiredCapacity(10, 11, 0, 0, 0));
    TEST_ASSERT_EQUAL(SIZE_MAX, backup_staging::requiredCapacity(upload, upload, upload, SIZE_MAX, reserve));
    TEST_ASSERT_EQUAL(SIZE_MAX, backup_staging::requiredCapacity(upload, upload, SIZE_MAX, 2, reserve));
}

void test_batched_backup_bound_covers_the_full_writable_history_limit() {
    TEST_ASSERT_EQUAL_size_t(
        history_capacity::CURRENT_WRITABLE_HISTORY_BYTES,
        history_storage::writableHistoryLimit(
            history_capacity::LITTLEFS_PARTITION_BYTES));
    const size_t maximumBackupBytes = history_capacity::maximumBatchedBackupBytes(
        history_capacity::CURRENT_WRITABLE_HISTORY_BYTES,
        history_capacity::MAX_HISTORY_FILE_COUNT);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(
        history_capacity::MAX_GENERATED_BACKUP_BYTES,
        maximumBackupBytes);

    constexpr size_t emptyRecordBytes =
        history_capacity::MAX_BACKUP_RECORD_ENVELOPE_BYTES -
        history_capacity::BACKUP_RECORD_SUFFIX_BYTES;
    constexpr size_t exactFitEntryBytes =
        history_capacity::MAX_BACKUP_JSON_LINE_BYTES - emptyRecordBytes -
        history_capacity::BACKUP_RECORD_SUFFIX_BYTES;
    TEST_ASSERT_TRUE(history_capacity::backupRecordEntryFits(
        emptyRecordBytes, 0, exactFitEntryBytes, 1));
    TEST_ASSERT_FALSE(history_capacity::backupRecordEntryFits(
        emptyRecordBytes, 0, exactFitEntryBytes + 1, 1));
    TEST_ASSERT_FALSE(history_capacity::backupRecordEntryFits(
        emptyRecordBytes, 1, 1, 1));
}

} // namespace

void setUp() {}
void tearDown() {}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_fragmented_upload_is_readable_in_multiple_nondestructive_passes);
    RUN_TEST(test_record_boundaries_reclaim_chunks_without_rewriting_unread_data);
    RUN_TEST(test_partial_chunk_is_retained_until_its_last_byte_is_consumed);
    RUN_TEST(test_missing_truncated_and_short_read_chunks_fail_closed);
    RUN_TEST(test_cleanup_handles_partial_upload_and_interrupted_consumption);
    RUN_TEST(test_size_limit_empty_input_and_reclaim_errors);
    RUN_TEST(test_peak_preflight_accounts_for_unread_upload_and_normalization_growth);
    RUN_TEST(test_capacity_preflight_keeps_old_history_and_allocation_slack_charged);
    RUN_TEST(test_batched_backup_bound_covers_the_full_writable_history_limit);
    return UNITY_END();
}
