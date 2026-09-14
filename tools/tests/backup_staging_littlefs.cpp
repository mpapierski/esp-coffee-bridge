// Host integration test using real LittleFS, not POSIX in-place file semantics.
#include "backup_staging.h"
#include "lfs.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
constexpr size_t BLOCK_BYTES = 4096;
constexpr size_t LEGACY_BLOCK_COUNT = (960 * 1024) / BLOCK_BYTES;
constexpr size_t BLOCK_COUNT = history_capacity::LITTLEFS_PARTITION_BYTES / BLOCK_BYTES;
static_assert(history_capacity::LITTLEFS_PARTITION_BYTES % BLOCK_BYTES == 0);
uint8_t flash[BLOCK_COUNT][BLOCK_BYTES]; // Match the 8 MiB partition.
size_t programmed = 0;
int readBlock(const lfs_config*, lfs_block_t block, lfs_off_t offset, void* data, lfs_size_t bytes) {
    memcpy(data, &flash[block][offset], bytes);
    return 0;
}
int programBlock(const lfs_config*, lfs_block_t block, lfs_off_t offset, const void* data, lfs_size_t bytes) {
    const auto* source = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        assert((flash[block][offset + i] & source[i]) == source[i]);
        flash[block][offset + i] = source[i];
    }
    programmed += bytes;
    return 0;
}
int eraseBlock(const lfs_config*, lfs_block_t block) { memset(flash[block], 255, BLOCK_BYTES); return 0; }
int sync(const lfs_config*) { return 0; }

void configure(lfs_config& cfg, size_t blockCount) {
    cfg = {};
    cfg.read = readBlock;
    cfg.prog = programBlock;
    cfg.erase = eraseBlock;
    cfg.sync = sync;
    cfg.read_size = 16;
    cfg.prog_size = 16;
    cfg.block_size = BLOCK_BYTES;
    cfg.block_count = blockCount;
    cfg.block_cycles = 500;
    cfg.cache_size = 512;
    cfg.lookahead_size = 32;
}

class Store {
public:
    explicit Store(lfs_t& fs) : fs_(fs) {}
    std::string path(size_t index) const { return "upload." + std::to_string(index) + ".part"; }
    bool openWrite(size_t index) { return open(index, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL); }
    bool openRead(size_t index) { return open(index, LFS_O_RDONLY); }
    size_t size() { return lfs_file_size(&fs_, &file_); }
    size_t read(uint8_t* data, size_t bytes) { const int n = lfs_file_read(&fs_, &file_, data, bytes); return n < 0 ? 0 : n; }
    size_t write(const uint8_t* data, size_t bytes) { const int n = lfs_file_write(&fs_, &file_, data, bytes); return n < 0 ? 0 : n; }
    void close() { if (opened_) assert(lfs_file_close(&fs_, &file_) == 0); opened_ = false; }
    bool exists(size_t index) { lfs_info info{}; return lfs_stat(&fs_, path(index).c_str(), &info) == 0; }
    bool remove(size_t index) { return lfs_remove(&fs_, path(index).c_str()) == 0; }
private:
    bool open(size_t index, int flags) {
        close();
        opened_ = lfs_file_open(&fs_, &file_, path(index).c_str(), flags) == 0;
        return opened_;
    }
    lfs_t& fs_;
    lfs_file_t file_{};
    bool opened_{false};
};

void append(lfs_t& fs, const char* path, const std::string& bytes) {
    lfs_file_t file{};
    assert(lfs_file_open(&fs, &file, path, LFS_O_CREAT | LFS_O_WRONLY | LFS_O_APPEND) == 0);
    assert(lfs_file_write(&fs, &file, bytes.data(), bytes.size()) == int(bytes.size()));
    assert(lfs_file_close(&fs, &file) == 0);
}

std::string readFile(lfs_t& fs, const char* path) {
    lfs_file_t file{};
    assert(lfs_file_open(&fs, &file, path, LFS_O_RDONLY) == 0);
    std::string bytes(lfs_file_size(&fs, &file), '\0');
    assert(lfs_file_read(&fs, &file, bytes.data(), bytes.size()) == int(bytes.size()));
    assert(lfs_file_close(&fs, &file) == 0);
    return bytes;
}

std::string record(size_t index) {
    // Unique ordered records, each crossing different 32640-byte boundaries.
    std::string bytes = std::to_string(index) + ":";
    bytes.resize(1023, static_cast<char>('a' + index % 26));
    return bytes + '\n';
}

void growLegacyFilesystem() {
    memset(flash, 255, sizeof(flash));
    programmed = 0;
    lfs_config cfg{};
    configure(cfg, LEGACY_BLOCK_COUNT);
    lfs_t fs{};
    assert(lfs_format(&fs, &cfg) == 0);
    // esp_littlefs mounts with block_count=0 so LittleFS reads the legacy
    // count from its superblock, then grow_on_mount supplies the partition's
    // new physical block count to lfs_fs_grow.
    cfg.block_count = 0;
    assert(lfs_mount(&fs, &cfg) == 0);
    const std::string retained(64 * 1024, 'H');
    append(fs, "history.jsonl", retained);
    assert(lfs_unmount(&fs) == 0);

    assert(lfs_mount(&fs, &cfg) == 0);
    assert(lfs_fs_grow(&fs, BLOCK_COUNT) == 0);
    assert(readFile(fs, "history.jsonl") == retained);

    const std::string beyondLegacyCapacity(1024 * 1024, 'G');
    append(fs, "grown.bin", beyondLegacyCapacity);
    assert(lfs_fs_size(&fs) > LEGACY_BLOCK_COUNT);
    assert(lfs_unmount(&fs) == 0);
    assert(lfs_mount(&fs, &cfg) == 0);
    assert(readFile(fs, "history.jsonl") == retained);
    assert(readFile(fs, "grown.bin") == beyondLegacyCapacity);
    assert(lfs_unmount(&fs) == 0);
    printf("growth passed: 960 KiB filesystem expanded to 8 MiB\n");
}

void maximumBackupFits() {
    memset(flash, 255, sizeof(flash));
    programmed = 0;
    lfs_config cfg{};
    configure(cfg, BLOCK_COUNT);
    lfs_t fs{};
    assert(lfs_format(&fs, &cfg) == 0);
    assert(lfs_mount(&fs, &cfg) == 0);

    Store store(fs);
    backup_staging::Writer<Store> writer(store);
    uint8_t buffer[1460];
    memset(buffer, 'U', sizeof(buffer));
    size_t remaining = backup_staging::MAX_BYTES;
    while (remaining > 0) {
        const size_t bytes = std::min(remaining, sizeof(buffer));
        assert(writer.write(buffer, bytes));
        remaining -= bytes;
    }
    writer.close();
    assert(writer.size() == backup_staging::MAX_BYTES);
    const size_t allocated = size_t(lfs_fs_size(&fs)) * BLOCK_BYTES;
    constexpr size_t MINIMUM_STAGING_RESERVE = 192 * 1024 + 24576 + 256;
    assert(sizeof(flash) - allocated >= MINIMUM_STAGING_RESERVE);
    assert(lfs_unmount(&fs) == 0);
    printf("maximum backup passed: %zu bytes staged with %zu bytes free\n",
           backup_staging::MAX_BYTES, sizeof(flash) - allocated);
}

void restore(bool interrupt) {
    memset(flash, 255, sizeof(flash));
    programmed = 0;
    lfs_config cfg{};
    configure(cfg, BLOCK_COUNT);
    lfs_t fs{};
    assert(lfs_format(&fs, &cfg) == 0);
    assert(lfs_mount(&fs, &cfg) == 0);
    const std::string oldBrew(24 * 1024, 'B'), oldStats(8 * 1024, 'S');
    append(fs, "brew.jsonl", oldBrew);
    append(fs, "stats.jsonl", oldStats);
    assert(lfs_rename(&fs, "brew.jsonl", "brew.jsonl.restorebak") == 0);
    assert(lfs_rename(&fs, "stats.jsonl", "stats.jsonl.restorebak") == 0);
    append(fs, "restore-state.bak", std::string(24 * 1024, 'M'));

    constexpr size_t recordCount = 600;
    constexpr size_t uploadBytes = recordCount * 1024;
    Store store(fs);
    backup_staging::Writer<Store> writer(store);
    for (size_t index = 0; index < recordCount; ++index) {
        const auto bytes = record(index);
        assert(writer.write(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    }
    writer.close();
    // Validation and machine loading must be repeatable and nondestructive.
    for (int pass = 0; pass < 2; ++pass) {
        backup_staging::Reader<Store> reader(store, uploadBytes);
        for (size_t index = 0; index < recordCount; ++index) {
            for (unsigned char byte : record(index)) assert(reader.read() == byte);
        }
    }
    const size_t programBefore = programmed;
    size_t peakAllocated = lfs_fs_size(&fs) * 4096;
    std::string expectedBrew, expectedStats;
    {
        backup_staging::Reader<Store> reader(store, uploadBytes);
        const size_t count = interrupt ? 120 : recordCount;
        for (size_t index = 0; index < count; ++index) {
            std::string bytes;
            int byte;
            while ((byte = reader.read()) != '\n') { assert(byte >= 0); bytes += char(byte); }
            bytes += '\n';
            assert(bytes == record(index));
            assert(reader.releaseConsumed());
            // Normalized history is 576 KiB, the full writable history set.
            const size_t normalizedBytes = 983 + (index % 25 == 0 ? 1 : 0);
            const std::string normalized = bytes.substr(0, normalizedBytes - 1) + '\n';
            auto& expected = index % 4 == 0 ? expectedStats : expectedBrew;
            expected += normalized;
            append(fs, index % 4 == 0 ? "stats.jsonl" : "brew.jsonl", normalized);
            peakAllocated = std::max(peakAllocated, size_t(lfs_fs_size(&fs)) * 4096);
        }
    }
    assert(readFile(fs, "brew.jsonl.restorebak") == oldBrew);
    assert(readFile(fs, "stats.jsonl.restorebak") == oldStats);
    if (interrupt) {
        // Remount at a complete-record interruption, then perform the same
        // file-level rollback and orphan cleanup used by boot recovery.
        assert(lfs_unmount(&fs) == 0);
        assert(lfs_mount(&fs, &cfg) == 0);
        assert(lfs_remove(&fs, "brew.jsonl") == 0);
        assert(lfs_remove(&fs, "stats.jsonl") == 0);
        assert(lfs_rename(&fs, "brew.jsonl.restorebak", "brew.jsonl") == 0);
        assert(lfs_rename(&fs, "stats.jsonl.restorebak", "stats.jsonl") == 0);
        assert(backup_staging::removeAll(store));
        assert(readFile(fs, "brew.jsonl") == oldBrew);
        assert(readFile(fs, "stats.jsonl") == oldStats);
    } else {
        assert(expectedBrew.size() + expectedStats.size() == 576 * 1024);
        assert(readFile(fs, "brew.jsonl") == expectedBrew);
        assert(readFile(fs, "stats.jsonl") == expectedStats);
        for (size_t i = 0; i < backup_staging::MAX_CHUNKS; ++i) assert(!store.exists(i));
        assert(programmed - programBefore < 8 * uploadBytes);
        assert(sizeof(flash) - peakAllocated >= 192 * 1024);
    }
    printf("%s: 600 KiB upload, peak=%zu/%zu, restore programmed=%zu bytes, reader=%zu bytes\n",
        interrupt ? "rollback passed" : "restore passed", peakAllocated, sizeof(flash),
        programmed - programBefore, sizeof(backup_staging::Reader<Store>));
    assert(lfs_unmount(&fs) == 0);
}
} // namespace

int main() { growLegacyFilesystem(); maximumBackupFits(); restore(false); restore(true); }
