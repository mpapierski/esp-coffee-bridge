#include <unity.h>

#include <cstdint>
#include <string>
#include <vector>

#include "bridge_multipart.h"

namespace {

using bridge_http::MultipartParser;
using bridge_http::MultipartPart;

struct Capture {
    MultipartPart part;
    std::vector<uint8_t> bytes;
    size_t starts{0};
    size_t ends{0};
};

MultipartParser parserFor(const std::string& boundary, Capture& capture) {
    return MultipartParser(
        boundary,
        [&](const MultipartPart& part) {
            capture.part = part;
            capture.starts++;
            return true;
        },
        [&](const uint8_t* bytes, size_t size) {
            capture.bytes.insert(capture.bytes.end(), bytes, bytes + size);
            return true;
        },
        [&]() {
            capture.ends++;
            return true;
        });
}

std::string request(const std::string& boundary,
                    const std::string& payload,
                    const std::string& fieldName = "file") {
    return "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"" + fieldName +
        "\"; filename=\"backup.ndjson\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n" +
        payload + "\r\n--" + boundary + "--\r\n";
}

void test_boundary_extraction_supports_browser_and_curl_shapes() {
    std::string boundary;
    std::string error;
    TEST_ASSERT_TRUE(bridge_http::extractMultipartBoundary(
        "multipart/form-data; boundary=----curl-boundary", boundary, error));
    TEST_ASSERT_EQUAL_STRING("----curl-boundary", boundary.c_str());
    TEST_ASSERT_TRUE(bridge_http::extractMultipartBoundary(
        "multipart/form-data; boundary=\"quoted boundary\"", boundary, error));
    TEST_ASSERT_EQUAL_STRING("quoted boundary", boundary.c_str());
}

void test_single_byte_chunks_preserve_binary_and_false_boundary_prefixes() {
    const std::string boundary = "AaB03x";
    std::string payload("first\0second\r\n--AaB03xXstill-data", 33);
    payload += "\xfftail";
    const std::string body = request(boundary, payload);
    Capture capture;
    MultipartParser parser = parserFor(boundary, capture);
    for (const char byte : body) {
        TEST_ASSERT_TRUE(parser.feed(reinterpret_cast<const uint8_t*>(&byte), 1));
    }
    TEST_ASSERT_TRUE(parser.finish());
    TEST_ASSERT_TRUE(parser.complete());
    TEST_ASSERT_EQUAL_UINT32(1, capture.starts);
    TEST_ASSERT_EQUAL_UINT32(1, capture.ends);
    TEST_ASSERT_EQUAL_STRING("file", capture.part.name.c_str());
    TEST_ASSERT_EQUAL_STRING("backup.ndjson", capture.part.filename.c_str());
    TEST_ASSERT_EQUAL_UINT32(payload.size(), capture.bytes.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload.data(), capture.bytes.data(), payload.size());
}

void test_large_payload_is_emitted_without_retaining_the_whole_body() {
    const std::string boundary = "bounded";
    const std::string payload(64 * 1024, 'z');
    const std::string body = request(boundary, payload);
    Capture capture;
    MultipartParser parser = parserFor(boundary, capture);
    for (size_t offset = 0; offset < body.size(); offset += 257) {
        const size_t count = std::min<size_t>(257, body.size() - offset);
        TEST_ASSERT_TRUE(parser.feed(
            reinterpret_cast<const uint8_t*>(body.data() + offset), count));
    }
    TEST_ASSERT_TRUE(parser.finish());
    TEST_ASSERT_EQUAL_UINT32(payload.size(), capture.bytes.size());
}

void test_documented_firmware_field_name_remains_compatible() {
    const std::string body = request("ota", "firmware-bytes", "firmware");
    Capture capture;
    MultipartParser parser = parserFor("ota", capture);
    TEST_ASSERT_TRUE(parser.feed(
        reinterpret_cast<const uint8_t*>(body.data()), body.size()));
    TEST_ASSERT_TRUE(parser.finish());
    TEST_ASSERT_EQUAL_STRING("firmware", capture.part.name.c_str());
    constexpr char expected[] = "firmware-bytes";
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected) - 1, capture.bytes.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, capture.bytes.data(), sizeof(expected) - 1);
}

void test_truncated_and_multiple_part_uploads_are_rejected() {
    Capture truncatedCapture;
    MultipartParser truncated = parserFor("edge", truncatedCapture);
    const std::string incomplete = "--edge\r\nContent-Disposition: form-data; name=\"file\"; filename=\"x\"\r\n\r\ndata";
    TEST_ASSERT_TRUE(truncated.feed(
        reinterpret_cast<const uint8_t*>(incomplete.data()), incomplete.size()));
    TEST_ASSERT_FALSE(truncated.finish());

    Capture multipleCapture;
    MultipartParser multiple = parserFor("edge", multipleCapture);
    const std::string multipleBody =
        "--edge\r\nContent-Disposition: form-data; name=\"file\"; filename=\"x\"\r\n\r\ndata"
        "\r\n--edge\r\nContent-Disposition: form-data; name=\"file\"; filename=\"y\"\r\n\r\nmore"
        "\r\n--edge--\r\n";
    TEST_ASSERT_FALSE(multiple.feed(
        reinterpret_cast<const uint8_t*>(multipleBody.data()), multipleBody.size()));
}

void test_payload_limit_includes_the_bounded_multipart_envelope() {
    constexpr size_t payloadBytes = 7500 * 1024;
    constexpr size_t requestBytes = bridge_http::multipartRequestLimit(payloadBytes);
    TEST_ASSERT_EQUAL_size_t(
        payloadBytes + bridge_http::MULTIPART_REQUEST_ENVELOPE_ALLOWANCE_BYTES,
        requestBytes);
    TEST_ASSERT_GREATER_THAN_size_t(2 * 1024 * 1024, requestBytes);
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(
        MultipartParser::MAX_HEADER_BYTES +
            2 * MultipartParser::MAX_BOUNDARY_BYTES + 16,
        bridge_http::MULTIPART_REQUEST_ENVELOPE_ALLOWANCE_BYTES);
}

void test_maximum_backup_payload_streams_past_the_old_transport_limit() {
    constexpr size_t payloadBytes = 7500 * 1024;
    const std::string boundary(MultipartParser::MAX_BOUNDARY_BYTES, 'b');
    const std::string body = request(boundary, std::string(payloadBytes, 'z'));
    TEST_ASSERT_LESS_OR_EQUAL_size_t(
        bridge_http::multipartRequestLimit(payloadBytes), body.size());
    TEST_ASSERT_GREATER_THAN_size_t(2 * 1024 * 1024, body.size());

    size_t streamedBytes = 0;
    MultipartParser parser(
        boundary,
        [](const MultipartPart&) { return true; },
        [&](const uint8_t*, size_t size) {
            streamedBytes += size;
            return true;
        },
        []() { return true; });
    for (size_t offset = 0; offset < body.size(); offset += 4096) {
        const size_t count = std::min<size_t>(4096, body.size() - offset);
        TEST_ASSERT_TRUE(parser.feed(
            reinterpret_cast<const uint8_t*>(body.data() + offset), count));
    }
    TEST_ASSERT_TRUE(parser.finish());
    TEST_ASSERT_EQUAL_size_t(payloadBytes, streamedBytes);
}

} // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_boundary_extraction_supports_browser_and_curl_shapes);
    RUN_TEST(test_single_byte_chunks_preserve_binary_and_false_boundary_prefixes);
    RUN_TEST(test_large_payload_is_emitted_without_retaining_the_whole_body);
    RUN_TEST(test_documented_firmware_field_name_remains_compatible);
    RUN_TEST(test_truncated_and_multiple_part_uploads_are_rejected);
    RUN_TEST(test_payload_limit_includes_the_bounded_multipart_envelope);
    RUN_TEST(test_maximum_backup_payload_streams_past_the_old_transport_limit);
    return UNITY_END();
}
