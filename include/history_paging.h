#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>

namespace history_paging {

inline constexpr size_t MAX_PAGE_SIZE = 100;

struct Window {
    size_t totalLines{0};
    size_t offset{0};
    size_t limit{0};
    size_t startInclusive{0};
    size_t endExclusive{0};
    size_t selectedCount{0};
    bool hasOlder{false};
    bool hasNewer{false};
    size_t nextOffset{0};
    size_t prevOffset{0};
};

inline Window makeWindow(size_t totalLines, size_t requestedOffset, size_t requestedLimit) {
    Window window;
    window.totalLines = totalLines;
    window.limit = std::min(requestedLimit, MAX_PAGE_SIZE);
    window.offset = std::min(requestedOffset, totalLines);
    window.endExclusive = totalLines - window.offset;
    window.startInclusive = window.limit > 0 && window.endExclusive > window.limit
        ? window.endExclusive - window.limit
        : 0;
    window.selectedCount = window.endExclusive - window.startInclusive;
    window.hasOlder = window.limit > 0 && window.startInclusive > 0;
    window.hasNewer = window.offset > 0;
    window.nextOffset = window.offset;
    if (window.hasOlder) {
        const size_t available = std::numeric_limits<size_t>::max() - window.offset;
        window.nextOffset += std::min(window.limit, available);
    }
    window.prevOffset = window.hasNewer
        ? (window.offset > window.limit ? window.offset - window.limit : 0)
        : 0;
    return window;
}

// Counts physical lines without retaining their contents. A newline terminates
// one physical line; a non-empty unterminated tail is also one line.
class PhysicalLineCounter {
public:
    void consume(const char* data, size_t length) {
        for (size_t index = 0; index < length; ++index) {
            const char ch = data[index];
            bytes_++;
            tailOpen_ = ch != '\n';
            if (ch == '\n') {
                lines_++;
            }
        }
    }

    size_t lineCount() const {
        return lines_ + (tailOpen_ ? 1U : 0U);
    }

    size_t byteCount() const {
        return bytes_;
    }

private:
    size_t lines_{0};
    size_t bytes_{0};
    bool tailOpen_{false};
};

// Records byte offsets only for the selected page. Storage is fixed regardless
// of file size, and cannot exceed MAX_PAGE_SIZE entries.
class OffsetCollector {
public:
    explicit OffsetCollector(const Window& window) : window_(window) {}

    void consume(const char* data, size_t length) {
        for (size_t index = 0; index < length; ++index) {
            if (atLineStart_) {
                if (lineIndex_ >= window_.startInclusive &&
                    lineIndex_ < window_.endExclusive &&
                    count_ < offsets_.size()) {
                    offsets_[count_++] = byteOffset_;
                }
                atLineStart_ = false;
            }

            const char ch = data[index];
            byteOffset_++;
            if (ch == '\n') {
                lineIndex_++;
                atLineStart_ = true;
            }
        }
    }

    size_t count() const {
        return count_;
    }

    size_t offsetAt(size_t index) const {
        return offsets_[index];
    }

private:
    Window window_;
    std::array<size_t, MAX_PAGE_SIZE> offsets_{};
    size_t count_{0};
    size_t lineIndex_{0};
    size_t byteOffset_{0};
    bool atLineStart_{true};
};

} // namespace history_paging
