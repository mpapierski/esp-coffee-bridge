#include <unity.h>

#include <vector>

#include "nivona.h"

namespace {

using nivona::ByteVector;

ByteVector decodeHex(const char* hex) {
    ByteVector out;
    String error;
    TEST_ASSERT_TRUE_MESSAGE(nivona::hexDecode(String(hex), out, error), error.c_str());
    return out;
}

void assertHexEquals(const char* expectedHex, const ByteVector& actual) {
    const String actualHex = nivona::hexEncode(actual);
    TEST_ASSERT_EQUAL_STRING(expectedHex, actualHex.c_str());
}

void test_rc4_transform_vectors_match_documented_examples() {
    assertHexEquals("1FC846", nivona::rc4Transform(decodeHex("000047")));
    assertHexEquals("E580D0367981BC", nivona::rc4Transform(decodeHex("FA48D17B7E6EE8")));
    assertHexEquals("0DFC1A", nivona::rc4Transform(decodeHex("12341B")));
    assertHexEquals("0DFC019807EF5090BE", nivona::rc4Transform(decodeHex("123400D5000004D274")));
    assertHexEquals("1F1D014D033DEE", nivona::rc4Transform(decodeHex("00D5000004D2BA")));
}

void test_build_packet_matches_documented_request_vectors() {
    assertHexEquals("53487000004745", nivona::buildPacket("Hp", decodeHex("0000"), nullptr, false));
    assertHexEquals("534855E580D0367981BC45", nivona::buildPacket("HU", decodeHex("FA48D17B7E6E"), nullptr, true));
    assertHexEquals("534855FA48D17B7E6EE845", nivona::buildPacket("HU", decodeHex("FA48D17B7E6E"), nullptr, false));

    const ByteVector session = decodeHex("1234");
    assertHexEquals("5348560DFC1A45", nivona::buildPacket("HV", {}, &session, true));
    assertHexEquals("5348520DFC019807EF5090BE45", nivona::buildPacket("HR", decodeHex("00D5000004D2"), &session, true));
    assertHexEquals("5348521F1D014D033DEE45", nivona::buildPacket("HR", decodeHex("00D5000004D2"), nullptr, true));
}

void test_live_hu_vector_round_trips_through_decoder_and_payload_validator() {
    const ByteVector seed = decodeHex("12314BF7");
    const ByteVector verifier = nivona::deriveHuVerifier(seed, 0, seed.size());
    assertHexEquals("5803", verifier);

    ByteVector huPayload = seed;
    huPayload.insert(huPayload.end(), verifier.begin(), verifier.end());
    assertHexEquals("5348550DF94ABA5FECD645", nivona::buildPacket("HU", huPayload, nullptr, true));

    ByteVector decodedPayload;
    String error;
    TEST_ASSERT_TRUE_MESSAGE(
        nivona::decodePacketAssumed(decodeHex("5348550DF94ABA39FA981D9545"), "HU", nullptr, true, decodedPayload, error),
        error.c_str());
    assertHexEquals("12314BF73E15CC5F", decodedPayload);

    ByteVector sessionKey;
    TEST_ASSERT_TRUE_MESSAGE(nivona::parseHuResponsePayload(decodedPayload, seed, sessionKey, error), error.c_str());
    assertHexEquals("3E15", sessionKey);
}

void test_live_hr_response_decodes_without_session_echo() {
    ByteVector payload;
    String error;
    TEST_ASSERT_TRUE_MESSAGE(
        nivona::decodePacketAssumed(decodeHex("5348521FA0014D07EBAD45"), "HR", nullptr, true, payload, error),
        error.c_str());
    assertHexEquals("006800000004", payload);

    std::vector<ByteVector> chunks{decodeHex("5348521FA0014D07EBAD45")};
    uint16_t registerId = 0;
    int32_t value = 0;
    TEST_ASSERT_TRUE_MESSAGE(nivona::decodeHrNumericResponse(chunks, true, registerId, value, error), error.c_str());
    TEST_ASSERT_EQUAL_UINT16(104, registerId);
    TEST_ASSERT_EQUAL_INT32(4, value);
}

void test_live_hr_response_rejects_incorrect_session_echo_expectation() {
    ByteVector payload;
    String error;
    const ByteVector session = decodeHex("3E15");
    TEST_ASSERT_FALSE(nivona::decodePacketAssumed(
        decodeHex("5348521FA0014D07EBAD45"), "HR", &session, true, payload, error));
    TEST_ASSERT_EQUAL_STRING("session key echo mismatch", error.c_str());
}

} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();
    RUN_TEST(test_rc4_transform_vectors_match_documented_examples);
    RUN_TEST(test_build_packet_matches_documented_request_vectors);
    RUN_TEST(test_live_hu_vector_round_trips_through_decoder_and_payload_validator);
    RUN_TEST(test_live_hr_response_decodes_without_session_echo);
    RUN_TEST(test_live_hr_response_rejects_incorrect_session_echo_expectation);
    return UNITY_END();
}
