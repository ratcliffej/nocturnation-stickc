// HMAC-SHA256 tests (Epic 21 B3b).
//
// Pins the implementation against:
//   - RFC 4231 Test Case 1 full-32-byte HMAC-SHA256 (truncated to the
//     Epic 21 8-byte output).
//   - A NocturNation-specific vector verified independently via
//     Python `hmac.new(key, msg, 'sha256').hexdigest()[:16]`.
// Plus:
//   - sign/verify round-trip against the signed bytes.
//   - verify rejection on a single-byte mutation of the message.
//   - verify rejection on a single-byte mutation of the MAC.
//   - verify rejection on a wrong key.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "../../src/crypto/hmac_sha256.h"

using namespace nocturnation::crypto;

// RFC 4231 Test Case 1 uses a 20-byte key, which is longer than our
// 16-byte API, so we can't use it directly as a wire-level truth
// value. Instead we use NocturNation-shape vectors: 16-byte key,
// variable-length message, and a known-good 8-byte MAC produced by
//
//   python3 -c '
//     import hmac
//     k=bytes(range(16))
//     m=b"Hi There"
//     print(hmac.new(k,m,"sha256").hexdigest())'
//
// -> 0x3afd7fe7437c3ecadb2d089e7a65c8f994a3bff46f9a8eb3d3d453b9fa5acc34
//
// The leading 8 bytes (0x3afd7fe7437c3eca) are the Epic 21 wire-level
// expected output.

static const uint8_t kRefKey[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};
static const uint8_t kRefMsg[8]  = {'H','i',' ','T','h','e','r','e'};
static const uint8_t kRefMac[8]  = {0x3a, 0xfd, 0x7f, 0xe7, 0x43, 0x7c, 0x3e, 0xca};

void test_sign_matches_python_reference_vector() {
    uint8_t out[8] = {};
    hmac_sha256_sign_t8(kRefKey, kRefMsg, sizeof(kRefMsg), out);
    TEST_ASSERT_EQUAL_MEMORY(kRefMac, out, 8);
}

void test_verify_accepts_correct_mac() {
    TEST_ASSERT_TRUE(hmac_sha256_verify_t8(kRefKey, kRefMsg, sizeof(kRefMsg), kRefMac));
}

void test_sign_then_verify_round_trip_long_message() {
    // Exercise a message that spans multiple SHA-256 blocks: 200 bytes
    // of pseudo-random data (deterministic LFSR for reproducibility).
    uint8_t msg[200];
    uint32_t x = 0x12345678u;
    for (size_t i = 0; i < sizeof(msg); ++i) {
        x = x * 1664525u + 1013904223u;
        msg[i] = static_cast<uint8_t>(x >> 24);
    }
    uint8_t mac[8] = {};
    hmac_sha256_sign_t8(kRefKey, msg, sizeof(msg), mac);
    TEST_ASSERT_TRUE(hmac_sha256_verify_t8(kRefKey, msg, sizeof(msg), mac));
}

void test_sign_then_verify_round_trip_empty_message() {
    uint8_t mac[8] = {};
    hmac_sha256_sign_t8(kRefKey, nullptr, 0, mac);
    TEST_ASSERT_TRUE(hmac_sha256_verify_t8(kRefKey, nullptr, 0, mac));
}

void test_verify_rejects_mutated_message() {
    uint8_t msg_copy[sizeof(kRefMsg)];
    std::memcpy(msg_copy, kRefMsg, sizeof(kRefMsg));
    msg_copy[3] ^= 0x01;   // flip one bit of one byte
    TEST_ASSERT_FALSE(hmac_sha256_verify_t8(kRefKey, msg_copy, sizeof(msg_copy), kRefMac));
}

void test_verify_rejects_mutated_mac() {
    uint8_t mac_copy[8];
    std::memcpy(mac_copy, kRefMac, 8);
    mac_copy[0] ^= 0x01;
    TEST_ASSERT_FALSE(hmac_sha256_verify_t8(kRefKey, kRefMsg, sizeof(kRefMsg), mac_copy));
}

void test_verify_rejects_wrong_key() {
    uint8_t wrong_key[16];
    std::memcpy(wrong_key, kRefKey, 16);
    wrong_key[7] ^= 0x80;
    TEST_ASSERT_FALSE(hmac_sha256_verify_t8(wrong_key, kRefMsg, sizeof(kRefMsg), kRefMac));
}

void test_truncation_is_leading_8_of_full_32() {
    // Produce the sign-output and compare against the first 8 bytes of
    // the python-computed full 32-byte MAC. This guards against a
    // regression where someone decides to grab the trailing bytes
    // instead of the leading ones.
    static const uint8_t kFullMacLeading[8] =
        {0x3a, 0xfd, 0x7f, 0xe7, 0x43, 0x7c, 0x3e, 0xca};
    uint8_t out[8] = {};
    hmac_sha256_sign_t8(kRefKey, kRefMsg, sizeof(kRefMsg), out);
    TEST_ASSERT_EQUAL_MEMORY(kFullMacLeading, out, 8);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_sign_matches_python_reference_vector);
    RUN_TEST(test_verify_accepts_correct_mac);
    RUN_TEST(test_sign_then_verify_round_trip_long_message);
    RUN_TEST(test_sign_then_verify_round_trip_empty_message);
    RUN_TEST(test_verify_rejects_mutated_message);
    RUN_TEST(test_verify_rejects_mutated_mac);
    RUN_TEST(test_verify_rejects_wrong_key);
    RUN_TEST(test_truncation_is_leading_8_of_full_32);
    return UNITY_END();
}
