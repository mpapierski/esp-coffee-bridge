#include <unity.h>

#include "bridge_json_object.h"

namespace {

void test_json_object_inspection_rejects_empty_and_invalid_payloads() {
    bridge_json::ObjectExtent extent;
    const char populated[] = "{\"ok\":true}";
    TEST_ASSERT_TRUE(bridge_json::inspectObject(populated, sizeof(populated) - 1, extent));
    TEST_ASSERT_TRUE(extent.hasMembers);
    TEST_ASSERT_EQUAL_UINT32(0, extent.openingBrace);
    TEST_ASSERT_EQUAL_UINT32(10, extent.closingBrace);

    const char empty[] = " \r\n { } \t";
    TEST_ASSERT_TRUE(bridge_json::inspectObject(empty, sizeof(empty) - 1, extent));
    TEST_ASSERT_FALSE(extent.hasMembers);

    TEST_ASSERT_FALSE(bridge_json::inspectObject("[]", 2, extent));
    TEST_ASSERT_FALSE(bridge_json::inspectObject("{", 1, extent));
}

} // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_json_object_inspection_rejects_empty_and_invalid_payloads);
    return UNITY_END();
}
