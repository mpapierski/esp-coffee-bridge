#include <unity.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <limits>

#include "history_paging.h"

namespace {

void test_empty_history_has_empty_page() {
    const history_paging::Window window = history_paging::makeWindow(0, 12, 40);
    TEST_ASSERT_EQUAL_size_t(0, window.totalLines);
    TEST_ASSERT_EQUAL_size_t(0, window.offset);
    TEST_ASSERT_EQUAL_size_t(0, window.selectedCount);
    TEST_ASSERT_FALSE(window.hasOlder);
    TEST_ASSERT_FALSE(window.hasNewer);
}

void test_page_window_is_newest_first_and_clamps_to_one_hundred() {
    const history_paging::Window newest = history_paging::makeWindow(250, 0, 500);
    TEST_ASSERT_EQUAL_size_t(100, newest.limit);
    TEST_ASSERT_EQUAL_size_t(150, newest.startInclusive);
    TEST_ASSERT_EQUAL_size_t(250, newest.endExclusive);
    TEST_ASSERT_EQUAL_size_t(100, newest.selectedCount);
    TEST_ASSERT_TRUE(newest.hasOlder);
    TEST_ASSERT_FALSE(newest.hasNewer);
    TEST_ASSERT_EQUAL_size_t(100, newest.nextOffset);

    const history_paging::Window older = history_paging::makeWindow(250, 100, 100);
    TEST_ASSERT_EQUAL_size_t(50, older.startInclusive);
    TEST_ASSERT_EQUAL_size_t(150, older.endExclusive);
    TEST_ASSERT_TRUE(older.hasOlder);
    TEST_ASSERT_TRUE(older.hasNewer);
    TEST_ASSERT_EQUAL_size_t(200, older.nextOffset);
    TEST_ASSERT_EQUAL_size_t(0, older.prevOffset);
}

void test_page_larger_than_history_returns_every_line() {
    const history_paging::Window window = history_paging::makeWindow(43, 0, 100);
    TEST_ASSERT_EQUAL_size_t(0, window.startInclusive);
    TEST_ASSERT_EQUAL_size_t(43, window.endExclusive);
    TEST_ASSERT_EQUAL_size_t(43, window.selectedCount);
    TEST_ASSERT_FALSE(window.hasOlder);
    TEST_ASSERT_FALSE(window.hasNewer);
    TEST_ASSERT_EQUAL_size_t(0, window.nextOffset);
}

void test_physical_lines_include_blank_and_invalid_lines() {
    constexpr char DATA[] = "{\"ok\":1}\n\nnot-json\n{\"tail\":1}";
    history_paging::PhysicalLineCounter counter;
    counter.consume(DATA, 5);
    counter.consume(DATA + 5, sizeof(DATA) - 1 - 5);
    TEST_ASSERT_EQUAL_size_t(4, counter.lineCount());
    TEST_ASSERT_EQUAL_size_t(sizeof(DATA) - 1, counter.byteCount());
}

void test_selected_offsets_are_stable_physical_line_offsets() {
    constexpr char DATA[] = "a\n\nbad\nlast";
    history_paging::PhysicalLineCounter counter;
    counter.consume(DATA, sizeof(DATA) - 1);
    const history_paging::Window window = history_paging::makeWindow(counter.lineCount(), 0, 3);
    history_paging::OffsetCollector offsets(window);
    offsets.consume(DATA, 3);
    offsets.consume(DATA + 3, sizeof(DATA) - 1 - 3);

    TEST_ASSERT_EQUAL_size_t(3, offsets.count());
    TEST_ASSERT_EQUAL_size_t(2, offsets.offsetAt(0));
    TEST_ASSERT_EQUAL_size_t(3, offsets.offsetAt(1));
    TEST_ASSERT_EQUAL_size_t(7, offsets.offsetAt(2));

    // Consumers traverse these offsets in reverse, yielding physical IDs 3,2,1.
    TEST_ASSERT_EQUAL_size_t(3, window.startInclusive + offsets.count() - 1);
    TEST_ASSERT_EQUAL_size_t(1, window.startInclusive);
}

void test_unterminated_tail_is_one_line_but_trailing_newline_is_not_extra() {
    history_paging::PhysicalLineCounter terminated;
    terminated.consume("one\ntwo\n", 8);
    TEST_ASSERT_EQUAL_size_t(2, terminated.lineCount());

    history_paging::PhysicalLineCounter unterminated;
    unterminated.consume("one\ntwo", 7);
    TEST_ASSERT_EQUAL_size_t(2, unterminated.lineCount());
}

void test_synthetic_512k_file_uses_fixed_offset_storage() {
    std::array<char, 64> block{};
    block.fill('x');
    block.back() = '\n';

    history_paging::PhysicalLineCounter counter;
    for (size_t index = 0; index < 8192; ++index) {
        counter.consume(block.data(), block.size());
    }
    TEST_ASSERT_EQUAL_size_t(512U * 1024U, counter.byteCount());
    TEST_ASSERT_EQUAL_size_t(8192, counter.lineCount());

    const history_paging::Window window = history_paging::makeWindow(counter.lineCount(), 0, 100);
    history_paging::OffsetCollector offsets(window);
    for (size_t index = 0; index < 8192; ++index) {
        offsets.consume(block.data(), block.size());
    }
    TEST_ASSERT_EQUAL_size_t(100, offsets.count());
    TEST_ASSERT_EQUAL_size_t((8192U - 100U) * 64U, offsets.offsetAt(0));
    TEST_ASSERT_TRUE(sizeof(offsets) < 1024U);
}

void test_file_backed_512k_page_keeps_stable_ids_and_counts_invalid_lines() {
    constexpr size_t LINE_BYTES = 64;
    constexpr size_t LINE_COUNT = (512U * 1024U) / LINE_BYTES;
    std::FILE* file = std::tmpfile();
    TEST_ASSERT_NOT_NULL(file);

    std::array<char, LINE_BYTES> line{};
    line.fill(' ');
    line.back() = '\n';
    for (size_t physicalId = 0; physicalId < LINE_COUNT; ++physicalId) {
        // The marker stands in for parse validity. Invalid physical lines must
        // still consume an entryId and count toward pagination offsets.
        line.front() = physicalId % 17U == 0U ? '!' : '{';
        TEST_ASSERT_EQUAL_size_t(
            LINE_BYTES,
            std::fwrite(line.data(), 1, line.size(), file));
    }
    TEST_ASSERT_EQUAL_INT(0, std::fflush(file));
    std::rewind(file);

    history_paging::PhysicalLineCounter counter;
    std::array<char, 257> readBuffer{};
    size_t readCount = 0;
    while ((readCount = std::fread(readBuffer.data(), 1, readBuffer.size(), file)) > 0) {
        counter.consume(readBuffer.data(), readCount);
    }
    TEST_ASSERT_EQUAL_size_t(512U * 1024U, counter.byteCount());
    TEST_ASSERT_EQUAL_size_t(LINE_COUNT, counter.lineCount());

    const history_paging::Window window =
        history_paging::makeWindow(counter.lineCount(), 0, 100);
    history_paging::OffsetCollector offsets(window);
    std::rewind(file);
    while ((readCount = std::fread(readBuffer.data(), 1, readBuffer.size(), file)) > 0) {
        offsets.consume(readBuffer.data(), readCount);
    }
    TEST_ASSERT_EQUAL_size_t(100, offsets.count());

    size_t invalidLines = 0;
    size_t validLines = 0;
    size_t previousPhysicalId = LINE_COUNT;
    for (size_t selectedIndex = offsets.count(); selectedIndex > 0; --selectedIndex) {
        TEST_ASSERT_EQUAL_INT(
            0,
            std::fseek(file, static_cast<long>(offsets.offsetAt(selectedIndex - 1)), SEEK_SET));
        const int marker = std::fgetc(file);
        TEST_ASSERT_NOT_EQUAL(EOF, marker);
        const size_t physicalId = window.startInclusive + selectedIndex - 1;
        TEST_ASSERT_TRUE(physicalId < previousPhysicalId);
        previousPhysicalId = physicalId;
        if (marker == '!') {
            invalidLines++;
        } else {
            validLines++;
        }
    }

    size_t expectedInvalid = 0;
    for (size_t physicalId = window.startInclusive;
         physicalId < window.endExclusive;
         ++physicalId) {
        expectedInvalid += physicalId % 17U == 0U ? 1U : 0U;
    }
    TEST_ASSERT_EQUAL_size_t(expectedInvalid, invalidLines);
    TEST_ASSERT_EQUAL_size_t(window.selectedCount - expectedInvalid, validLines);
    TEST_ASSERT_EQUAL_size_t(window.startInclusive, previousPhysicalId);
    TEST_ASSERT_TRUE(sizeof(counter) + sizeof(offsets) + sizeof(readBuffer) < 2048U);
    std::fclose(file);
}

void test_next_offset_saturates_without_wraparound() {
    const size_t maximum = std::numeric_limits<size_t>::max();
    const history_paging::Window window = history_paging::makeWindow(maximum, maximum - 200, 100);
    TEST_ASSERT_EQUAL_size_t(maximum - 100, window.nextOffset);
    TEST_ASSERT_TRUE(window.nextOffset > window.offset);
}

} // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_history_has_empty_page);
    RUN_TEST(test_page_window_is_newest_first_and_clamps_to_one_hundred);
    RUN_TEST(test_page_larger_than_history_returns_every_line);
    RUN_TEST(test_physical_lines_include_blank_and_invalid_lines);
    RUN_TEST(test_selected_offsets_are_stable_physical_line_offsets);
    RUN_TEST(test_unterminated_tail_is_one_line_but_trailing_newline_is_not_extra);
    RUN_TEST(test_synthetic_512k_file_uses_fixed_offset_storage);
    RUN_TEST(test_file_backed_512k_page_keeps_stable_ids_and_counts_invalid_lines);
    RUN_TEST(test_next_offset_saturates_without_wraparound);
    return UNITY_END();
}
