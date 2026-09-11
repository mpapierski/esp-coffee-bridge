#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "history_capacity.h"

// Forward-only staging shared by firmware and native tests. Store owns one
// handle and provides openRead/openWrite(index), size, read/write, close,
// exists(index), and remove(index). The caller serializes filesystem access.
namespace backup_staging {

// Leave room for LittleFS CTZ pointers: a full chunk fits in eight 4 KiB
// blocks instead of spilling into a ninth block for a few pointer bytes.
inline constexpr size_t CHUNK_BYTES = 32 * 1024 - 128;
inline constexpr size_t MAX_BYTES = history_capacity::MAX_GENERATED_BACKUP_BYTES;
inline constexpr size_t MAX_CHUNKS = (MAX_BYTES + CHUNK_BYTES - 1) / CHUNK_BYTES;

constexpr size_t chunkCount(size_t bytes) {
    return (bytes + CHUNK_BYTES - 1) / CHUNK_BYTES;
}

constexpr size_t consumedChunks(size_t position, size_t total) {
    return position == total ? chunkCount(total) : position / CHUNK_BYTES;
}

constexpr size_t remainingBytes(size_t position, size_t total) {
    return total - std::min(total, consumedChunks(position, total) * CHUNK_BYTES);
}

// Peak logical data includes both the unread upload and the normalized output.
// Call at each record boundary, after normalization and before writing output.
// The caller adds retained files (including rollback history), allocation slack
// and transaction/operational reserves before accepting a restore.
class PeakUsage {
public:
    explicit PeakUsage(size_t total) : total_(total), peak_(total) {}
    bool observe(size_t position, size_t restored) {
        if (position > total_) return false;
        const size_t remaining = remainingBytes(position, total_);
        if (restored > SIZE_MAX - remaining) return false;
        peak_ = std::max(peak_, restored + remaining);
        return true;
    }
    size_t bytes() const { return peak_; }
private:
    size_t total_;
    size_t peak_;
};

constexpr size_t requiredCapacity(size_t usedBytes, size_t uploadBytes,
                                  size_t peakDataBytes, size_t outputFiles,
                                  size_t reserveBytes) {
    if (uploadBytes > usedBytes ||
        outputFiles > SIZE_MAX / history_capacity::LITTLEFS_ALLOCATION_BYTES) return SIZE_MAX;
    const size_t slack = outputFiles * history_capacity::LITTLEFS_ALLOCATION_BYTES;
    // Do not subtract the original history: it is retained for rollback.
    const size_t retained = usedBytes - uploadBytes;
    if (slack > SIZE_MAX - reserveBytes ||
        peakDataBytes > SIZE_MAX - reserveBytes - slack ||
        retained > SIZE_MAX - reserveBytes - slack - peakDataBytes) return SIZE_MAX;
    return retained + peakDataBytes + slack + reserveBytes;
}

template <typename Store>
bool removeAll(Store& store) {
    store.close();
    bool ok = true;
    for (size_t index = 0; index < MAX_CHUNKS; ++index) {
        if (store.exists(index) && !store.remove(index)) ok = false;
    }
    return ok;
}

template <typename Store>
class Writer {
public:
    explicit Writer(Store& store) : store_(store) {}
    void reset() { store_.close(); written_ = 0; failed_ = false; }
    void close() { store_.close(); }
    bool write(const uint8_t* data, size_t bytes) {
        if (failed_ || bytes > MAX_BYTES - written_) return fail();
        while (bytes > 0) {
            const size_t offset = written_ % CHUNK_BYTES;
            if (offset == 0 && !store_.openWrite(written_ / CHUNK_BYTES)) return fail();
            const size_t count = std::min(bytes, CHUNK_BYTES - offset);
            if (store_.write(data, count) != count) return fail();
            written_ += count;
            data += count;
            bytes -= count;
            if (written_ % CHUNK_BYTES == 0) store_.close();
        }
        return true;
    }
    size_t size() const { return written_; }
private:
    bool fail() { failed_ = true; store_.close(); return false; }
    Store& store_;
    size_t written_{0};
    bool failed_{false};
};

template <typename Store>
class Reader {
public:
    Reader(Store& store, size_t total) : store_(store), total_(total), failed_(total == 0 || total > MAX_BYTES) {}
    ~Reader() { close(); }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    explicit operator bool() const { return !failed_; }
    bool available() const { return position_ < total_; }
    size_t position() const { return position_; }
    void close() { store_.close(); openChunk_ = MAX_CHUNKS; }
    int read() {
        if (failed_ || !available()) return -1;
        if (bufferPosition_ == buffered_) {
            const size_t index = position_ / CHUNK_BYTES;
            const size_t expected = std::min(CHUNK_BYTES, total_ - index * CHUNK_BYTES);
            if (openChunk_ != index) {
                close();
                if (!store_.openRead(index) || store_.size() != expected) return fail();
                openChunk_ = index;
            }
            buffered_ = std::min(buffer_.size(), expected - position_ % CHUNK_BYTES);
            if (store_.read(buffer_.data(), buffered_) != buffered_) return fail();
            bufferPosition_ = 0;
        }
        ++position_;
        return buffer_[bufferPosition_++];
    }
    // Called only once a complete record is in RAM. Partial chunks (including
    // any prefetched bytes) are retained. Rollback originals are never touched.
    bool releaseConsumed() {
        if (failed_) return false;
        const size_t count = consumedChunks(position_, total_);
        if (openChunk_ < count) close();
        while (released_ < count) {
            if (!store_.remove(released_)) { fail(); return false; }
            ++released_;
        }
        return true;
    }
private:
    int fail() { failed_ = true; close(); return -1; }
    Store& store_;
    size_t total_;
    size_t position_{0};
    size_t released_{0};
    size_t openChunk_{MAX_CHUNKS};
    std::array<uint8_t, 512> buffer_{};
    size_t buffered_{0};
    size_t bufferPosition_{0};
    bool failed_;
};

} // namespace backup_staging
