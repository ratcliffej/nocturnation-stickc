// CONFIG_WRITE dispatcher tests (Epic 21 B3d).
//
// Covers:
//   - Well-formed, correctly-signed frame -> Accepted, bag walked.
//   - target_uid mismatch                 -> BadTarget (bag not walked).
//   - Replayed numonce                     -> Replay.
//   - Flipped HMAC byte                   -> BadHmac.
//   - Malformed envelope header           -> MalformedFrame.
//   - Malformed TLV inside the bag        -> MalformedBag.
//
// Builds the frame the same way the production sender (B4) will:
// encode CONFIG_WRITE with zeros in hmac, compute HMAC over the
// signed region, overwrite the trailing 8 bytes. Native test lets
// us pin that construction before the on-chip signer exists.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "../../src/ble/config_rx.h"
#include "../../src/crypto/hmac_sha256.h"
#include "../../src/ble/property_bag_tlv.h"
#include "transport/espnow/frame.h"
#include "transport/espnow/numonce_lru.h"

using namespace nocturnation::ble;
using namespace nocturnation::transport::espnow;
using nocturnation::crypto::hmac_sha256_sign_t8;

void setUp(void) {}
void tearDown(void) {}

// Capture callback: records each entry's key into a buffer so tests
// can assert the dispatcher walked the bag.
struct CaptureCtx {
    char   last_key[32];
    size_t count;
};
static bool capture_cb(const TlvEntry& e, void* raw) {
    auto* c = static_cast<CaptureCtx*>(raw);
    const size_t n = e.key_len < sizeof(c->last_key) - 1
                     ? e.key_len : sizeof(c->last_key) - 1;
    std::memcpy(c->last_key, e.key, n);
    c->last_key[n] = '\0';
    ++c->count;
    return true;
}

static constexpr uint8_t kTestSecret[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};
static constexpr uint32_t kTestSelfUid = 0x2AF215C8u;

// Encode a CONFIG_WRITE then sign it (zeros in hmac field, HMAC over
// everything except trailing 8 bytes, write result into last 8).
// Returns total frame length.
static size_t encode_and_sign(uint8_t* buf, size_t cap,
                              uint16_t source_id,
                              uint32_t target_uid,
                              uint64_t numonce_value,
                              const uint8_t* bag, uint8_t bag_len,
                              const uint8_t secret[16]) {
    Header hdr = {};
    hdr.source_id = source_id;
    hdr.sequence_number = 1;
    hdr.hop_count = 0;

    ConfigWritePayload cw = {};
    cw.target_uid = target_uid;
    for (int i = 0; i < 8; ++i) cw.numonce[i] = (uint8_t)((numonce_value >> (i * 8)) & 0xFF);
    cw.bag_len = bag_len;
    if (bag_len > 0) std::memcpy(cw.bag_tlv, bag, bag_len);
    std::memset(cw.hmac, 0, 8);   // zeros before signing

    const size_t total = encode_config_write(buf, cap, hdr, cw);
    if (total == 0) return 0;

    // Sign over [0 .. total - 8] and write into the last 8.
    const size_t signed_len = total - 8;
    uint8_t mac[8];
    hmac_sha256_sign_t8(secret, buf, signed_len, mac);
    std::memcpy(buf + signed_len, mac, 8);
    return total;
}

// A minimal well-formed property bag: one entry {group: u8 = 3}.
static constexpr uint8_t kBagOneEntry[] = {
    0x01,                             // entry_count
    0x05, 'g','r','o','u','p',        // key_len=5, key="group"
    0x00,                             // value_type U8
    0x01,                             // value_len=1
    0x03                              // value byte
};

void test_well_formed_frame_accepted_and_bag_walked() {
    NumonceLru lru;
    uint8_t frame[80] = {};
    const size_t n = encode_and_sign(frame, sizeof(frame),
                                      /*source_id=*/0x0040,
                                      /*target_uid=*/kTestSelfUid,
                                      /*numonce=*/42,
                                      kBagOneEntry, sizeof(kBagOneEntry),
                                      kTestSecret);
    TEST_ASSERT_GREATER_THAN_size_t(0, n);

    CaptureCtx cap = {};
    const auto detail = process_config_write(
        frame, n, kTestSelfUid, kTestSecret, lru, capture_cb, &cap);
    TEST_ASSERT_EQUAL(ConfigRxResult::Accepted, detail.result);
    TEST_ASSERT_EQUAL_UINT64(42u, detail.numonce);
    TEST_ASSERT_EQUAL_size_t(1, cap.count);
    TEST_ASSERT_EQUAL_STRING("group", cap.last_key);
    TEST_ASSERT_EQUAL_UINT64(42u, lru.last_seen_for(0x0040));
}

void test_target_mismatch_rejected_bag_not_walked() {
    NumonceLru lru;
    uint8_t frame[80] = {};
    const size_t n = encode_and_sign(frame, sizeof(frame),
                                      /*source_id=*/0x0040,
                                      /*target_uid=*/0xDEADBEEFu,   // not us
                                      /*numonce=*/1,
                                      kBagOneEntry, sizeof(kBagOneEntry),
                                      kTestSecret);
    CaptureCtx cap = {};
    const auto detail = process_config_write(
        frame, n, kTestSelfUid, kTestSecret, lru, capture_cb, &cap);
    TEST_ASSERT_EQUAL(ConfigRxResult::BadTarget, detail.result);
    TEST_ASSERT_EQUAL_size_t(0, cap.count);
    TEST_ASSERT_EQUAL_UINT64(0u, lru.last_seen_for(0x0040));   // untouched
}

void test_replay_rejected() {
    NumonceLru lru;
    uint8_t frame[80] = {};
    const size_t n = encode_and_sign(frame, sizeof(frame),
                                      0x0040, kTestSelfUid, 10,
                                      kBagOneEntry, sizeof(kBagOneEntry),
                                      kTestSecret);
    CaptureCtx cap = {};
    auto first = process_config_write(
        frame, n, kTestSelfUid, kTestSecret, lru, capture_cb, &cap);
    TEST_ASSERT_EQUAL(ConfigRxResult::Accepted, first.result);
    // Second attempt with same frame = same numonce = replay.
    auto second = process_config_write(
        frame, n, kTestSelfUid, kTestSecret, lru, capture_cb, &cap);
    TEST_ASSERT_EQUAL(ConfigRxResult::Replay, second.result);
    // Capture ran only for the first accept.
    TEST_ASSERT_EQUAL_size_t(1, cap.count);
}

void test_mutated_hmac_rejected() {
    NumonceLru lru;
    uint8_t frame[80] = {};
    const size_t n = encode_and_sign(frame, sizeof(frame),
                                      0x0040, kTestSelfUid, 1,
                                      kBagOneEntry, sizeof(kBagOneEntry),
                                      kTestSecret);
    // Flip one bit in the trailing HMAC region.
    frame[n - 1] ^= 0x01;
    CaptureCtx cap = {};
    const auto detail = process_config_write(
        frame, n, kTestSelfUid, kTestSecret, lru, capture_cb, &cap);
    TEST_ASSERT_EQUAL(ConfigRxResult::BadHmac, detail.result);
    TEST_ASSERT_EQUAL_size_t(0, cap.count);
}

void test_mutated_bag_body_rejected_by_hmac() {
    // Flip a byte inside bag_tlv; the HMAC covers it so verification fails.
    NumonceLru lru;
    uint8_t frame[80] = {};
    const size_t n = encode_and_sign(frame, sizeof(frame),
                                      0x0040, kTestSelfUid, 1,
                                      kBagOneEntry, sizeof(kBagOneEntry),
                                      kTestSecret);
    // bag_tlv starts at kHeaderSize + 13 (4 target_uid + 8 numonce + 1 bag_len)
    frame[kHeaderSize + 13 + 3] ^= 0x01;
    CaptureCtx cap = {};
    const auto detail = process_config_write(
        frame, n, kTestSelfUid, kTestSecret, lru, capture_cb, &cap);
    TEST_ASSERT_EQUAL(ConfigRxResult::BadHmac, detail.result);
}

void test_wrong_secret_rejected() {
    NumonceLru lru;
    uint8_t frame[80] = {};
    const size_t n = encode_and_sign(frame, sizeof(frame),
                                      0x0040, kTestSelfUid, 1,
                                      kBagOneEntry, sizeof(kBagOneEntry),
                                      kTestSecret);
    uint8_t wrong_secret[16];
    std::memcpy(wrong_secret, kTestSecret, 16);
    wrong_secret[0] ^= 0xFF;
    CaptureCtx cap = {};
    const auto detail = process_config_write(
        frame, n, kTestSelfUid, wrong_secret, lru, capture_cb, &cap);
    TEST_ASSERT_EQUAL(ConfigRxResult::BadHmac, detail.result);
}

void test_malformed_envelope_rejected() {
    NumonceLru lru;
    uint8_t garbage[20] = {0xFF, 0xFF, 0xFF};   // bad magic + random bytes
    CaptureCtx cap = {};
    const auto detail = process_config_write(
        garbage, sizeof(garbage), kTestSelfUid, kTestSecret, lru, capture_cb, &cap);
    TEST_ASSERT_EQUAL(ConfigRxResult::MalformedFrame, detail.result);
}

void test_malformed_bag_rejected() {
    // Encode a bag that claims entry_count=1 but then truncates
    // mid-entry. The dispatcher's HMAC verifies (we signed this
    // ourselves) but the TLV decoder rejects the shape.
    const uint8_t bad_bag[] = {
        0x01,                 // entry_count = 1
        0x05, 'g','r','o','u' // key_len=5 but only 4 bytes follow
                              // (no value_type / value_len / value either)
    };
    NumonceLru lru;
    uint8_t frame[80] = {};
    const size_t n = encode_and_sign(frame, sizeof(frame),
                                      0x0040, kTestSelfUid, 1,
                                      bad_bag, sizeof(bad_bag),
                                      kTestSecret);
    CaptureCtx cap = {};
    const auto detail = process_config_write(
        frame, n, kTestSelfUid, kTestSecret, lru, capture_cb, &cap);
    TEST_ASSERT_EQUAL(ConfigRxResult::MalformedBag, detail.result);
}

void test_multiple_senders_tracked_independently() {
    // Two Directors (different source_id) can each have the same
    // numonce value without colliding; the LRU keys per sender.
    NumonceLru lru;
    uint8_t frame_a[80] = {};
    uint8_t frame_b[80] = {};
    const size_t na = encode_and_sign(frame_a, sizeof(frame_a),
                                       /*source_id=*/0x0040, kTestSelfUid,
                                       /*numonce=*/5,
                                       kBagOneEntry, sizeof(kBagOneEntry),
                                       kTestSecret);
    const size_t nb = encode_and_sign(frame_b, sizeof(frame_b),
                                       /*source_id=*/0x0041, kTestSelfUid,
                                       /*numonce=*/5,
                                       kBagOneEntry, sizeof(kBagOneEntry),
                                       kTestSecret);
    CaptureCtx cap = {};
    TEST_ASSERT_EQUAL(ConfigRxResult::Accepted,
        process_config_write(frame_a, na, kTestSelfUid, kTestSecret, lru, capture_cb, &cap).result);
    TEST_ASSERT_EQUAL(ConfigRxResult::Accepted,
        process_config_write(frame_b, nb, kTestSelfUid, kTestSecret, lru, capture_cb, &cap).result);
    TEST_ASSERT_EQUAL_UINT64(5u, lru.last_seen_for(0x0040));
    TEST_ASSERT_EQUAL_UINT64(5u, lru.last_seen_for(0x0041));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_well_formed_frame_accepted_and_bag_walked);
    RUN_TEST(test_target_mismatch_rejected_bag_not_walked);
    RUN_TEST(test_replay_rejected);
    RUN_TEST(test_mutated_hmac_rejected);
    RUN_TEST(test_mutated_bag_body_rejected_by_hmac);
    RUN_TEST(test_wrong_secret_rejected);
    RUN_TEST(test_malformed_envelope_rejected);
    RUN_TEST(test_malformed_bag_rejected);
    RUN_TEST(test_multiple_senders_tracked_independently);
    return UNITY_END();
}
