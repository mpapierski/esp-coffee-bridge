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

void test_hx_ready_vector_decodes_to_apk_backed_labels() {
    std::vector<ByteVector> chunks{
        nivona::buildPacket("HX", decodeHex("0008000000000000"), nullptr, true),
    };

    nivona::ProcessStatus status;
    String error;
    TEST_ASSERT_TRUE_MESSAGE(nivona::decodeHxResponse(chunks, true, status, error), error.c_str());
    TEST_ASSERT_TRUE(status.ok);
    TEST_ASSERT_EQUAL_INT16(8, status.process);
    TEST_ASSERT_EQUAL_INT16(0, status.subProcess);
    TEST_ASSERT_EQUAL_INT16(0, status.message);
    TEST_ASSERT_EQUAL_INT16(0, status.progress);
    TEST_ASSERT_EQUAL_STRING("ready", status.summary.c_str());
    TEST_ASSERT_EQUAL_STRING("ready", status.processLabel.c_str());
    TEST_ASSERT_EQUAL_STRING("none", status.messageLabel.c_str());
}

void test_unknown_hx_message_code_stays_raw_and_unlabeled() {
    std::vector<ByteVector> chunks{
        nivona::buildPacket("HX", decodeHex("000B0000002A0000"), nullptr, true),
    };

    nivona::ProcessStatus status;
    String error;
    TEST_ASSERT_TRUE_MESSAGE(nivona::decodeHxResponse(chunks, true, status, error), error.c_str());
    TEST_ASSERT_TRUE(status.ok);
    TEST_ASSERT_EQUAL_INT16(11, status.process);
    TEST_ASSERT_EQUAL_INT16(42, status.message);
    TEST_ASSERT_EQUAL_STRING("preparing drink", status.processLabel.c_str());
    TEST_ASSERT_EQUAL_STRING("", status.messageLabel.c_str());
}

void test_hx_decoder_ignores_leading_ack_frame_in_mixed_batch() {
    std::vector<ByteVector> chunks{
        decodeHex("5341BE45"),
        nivona::buildPacket("HX", decodeHex("000B000000000000"), nullptr, true),
    };

    nivona::ProcessStatus status;
    String error;
    TEST_ASSERT_TRUE_MESSAGE(nivona::decodeHxResponse(chunks, true, status, error), error.c_str());
    TEST_ASSERT_TRUE(status.ok);
    TEST_ASSERT_EQUAL_INT16(11, status.process);
    TEST_ASSERT_EQUAL_INT16(0, status.message);
    TEST_ASSERT_EQUAL_STRING("preparing drink", status.processLabel.c_str());
}

void test_family_700_standard_recipe_lookup_and_layout_match_apk_offsets() {
    nivona::ModelInfo modelInfo;
    modelInfo.familyKey = "700";

    const nivona::StandardRecipeDescriptor* recipe = nivona::findStandardRecipeBySelector(modelInfo, 2);
    TEST_ASSERT_NOT_NULL(recipe);
    TEST_ASSERT_EQUAL_UINT8(2, recipe->selector);
    TEST_ASSERT_EQUAL_STRING("lungo", recipe->name);
    TEST_ASSERT_EQUAL_STRING("Lungo", recipe->title);

    uint16_t baseRegister = 0;
    TEST_ASSERT_TRUE(nivona::resolveStandardRecipeBaseRegister(modelInfo, recipe->selector, baseRegister));
    TEST_ASSERT_EQUAL_UINT16(10200, baseRegister);

    nivona::StandardRecipeLayout layout;
    TEST_ASSERT_TRUE(nivona::resolveStandardRecipeLayout(modelInfo, layout));
    TEST_ASSERT_EQUAL_STRING("700", layout.familyKey.c_str());
    TEST_ASSERT_FALSE(layout.fluidWriteScale10);
    TEST_ASSERT_EQUAL_UINT16(1, layout.strengthOffset);
    TEST_ASSERT_EQUAL_UINT16(2, layout.profileOffset);
    TEST_ASSERT_EQUAL_UINT16(3, layout.temperatureOffset);
    TEST_ASSERT_EQUAL_UINT16(4, layout.twoCupsOffset);
    TEST_ASSERT_EQUAL_UINT16(5, layout.coffeeAmountOffset);
    TEST_ASSERT_EQUAL_UINT16(6, layout.waterAmountOffset);
    TEST_ASSERT_EQUAL_UINT16(7, layout.milkAmountOffset);
    TEST_ASSERT_EQUAL_UINT16(8, layout.milkFoamAmountOffset);
}

void test_family_900_standard_recipe_layout_tracks_split_temperatures_and_scaled_fluids() {
    nivona::ModelInfo modelInfo;
    modelInfo.familyKey = "900";
    modelInfo.fluidWriteScale10 = true;

    nivona::StandardRecipeLayout layout;
    TEST_ASSERT_TRUE(nivona::resolveStandardRecipeLayout(modelInfo, layout));
    TEST_ASSERT_EQUAL_STRING("900", layout.familyKey.c_str());
    TEST_ASSERT_TRUE(layout.fluidWriteScale10);
    TEST_ASSERT_EQUAL_UINT16(1, layout.strengthOffset);
    TEST_ASSERT_EQUAL_UINT16(2, layout.profileOffset);
    TEST_ASSERT_EQUAL_UINT16(3, layout.preparationOffset);
    TEST_ASSERT_EQUAL_UINT16(4, layout.twoCupsOffset);
    TEST_ASSERT_EQUAL_UINT16(5, layout.coffeeTemperatureOffset);
    TEST_ASSERT_EQUAL_UINT16(6, layout.waterTemperatureOffset);
    TEST_ASSERT_EQUAL_UINT16(7, layout.milkTemperatureOffset);
    TEST_ASSERT_EQUAL_UINT16(8, layout.milkFoamTemperatureOffset);
    TEST_ASSERT_EQUAL_UINT16(9, layout.coffeeAmountOffset);
    TEST_ASSERT_EQUAL_UINT16(10, layout.waterAmountOffset);
    TEST_ASSERT_EQUAL_UINT16(11, layout.milkAmountOffset);
    TEST_ASSERT_EQUAL_UINT16(12, layout.milkFoamAmountOffset);
    TEST_ASSERT_EQUAL_UINT16(13, layout.overallTemperatureOffset);
}

void test_standard_recipe_base_register_rejects_unknown_selector() {
    nivona::ModelInfo modelInfo;
    modelInfo.familyKey = "700";

    TEST_ASSERT_NULL(nivona::findStandardRecipeBySelector(modelInfo, 99));
    uint16_t baseRegister = 0;
    TEST_ASSERT_FALSE(nivona::resolveStandardRecipeBaseRegister(modelInfo, 99, baseRegister));
}

void test_model_detection_exposes_strength_and_profile_caps() {
    nivona::DeviceDetails basic700;
    basic700.serial = "756573071020106-----";
    const nivona::ModelInfo basic700Info = nivona::detectModelInfo(basic700);
    TEST_ASSERT_EQUAL_STRING("756", basic700Info.modelCode.c_str());
    TEST_ASSERT_EQUAL_UINT8(3, basic700Info.strengthLevelCount);
    TEST_ASSERT_EQUAL_UINT8(3, basic700Info.maxProfileCode);

    nivona::DeviceDetails advanced79x;
    advanced79x.serial = "790000000000000-----";
    const nivona::ModelInfo advanced79xInfo = nivona::detectModelInfo(advanced79x);
    TEST_ASSERT_EQUAL_STRING("790", advanced79xInfo.modelCode.c_str());
    TEST_ASSERT_EQUAL_UINT8(5, advanced79xInfo.strengthLevelCount);
    TEST_ASSERT_EQUAL_UINT8(3, advanced79xInfo.maxProfileCode);
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
    RUN_TEST(test_hx_ready_vector_decodes_to_apk_backed_labels);
    RUN_TEST(test_unknown_hx_message_code_stays_raw_and_unlabeled);
    RUN_TEST(test_hx_decoder_ignores_leading_ack_frame_in_mixed_batch);
    RUN_TEST(test_family_700_standard_recipe_lookup_and_layout_match_apk_offsets);
    RUN_TEST(test_family_900_standard_recipe_layout_tracks_split_temperatures_and_scaled_fluids);
    RUN_TEST(test_standard_recipe_base_register_rejects_unknown_selector);
    RUN_TEST(test_model_detection_exposes_strength_and_profile_caps);
    return UNITY_END();
}
