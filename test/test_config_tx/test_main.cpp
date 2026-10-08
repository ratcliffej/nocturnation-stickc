// config_tx round-trip tests (Epic 21 B4).
//
// The important invariant: bytes produced by build_signed_config_write
// MUST be accepted by config_rx::process_config_write when the same
// secret and target UID are used on both sides. These tests pin that
// contract so a wire-layer regression on either side can't ship
// without a native-test failure.
//
// Also covers:
//   - Empty bag round-trips.
//   - Max-size bag round-trips.
//   - Buffer-too-small / bag-too-big returns 0.
//   - A frame signed with secret A is REJECTED by a receiver with
//     secret B (demonstrates the HMAC gate works end-to-end).

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "../../src/ble/config_tx.h"
#include "../../src/ble/config_rx.h"
#include "../../src/ble/property_bag_tlv.h"
#include "transport/espnow/frame.h"
#include "transport/espnow/numonce_lru.h"

using namespace nocturnation::ble;
using namespace nocturnation::transport::espnow;

void setUp(void) {}
void tearDown(void) {}

static constexpr uint8_t kSecret[16] = {
    0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
    0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0x00,
};
static constexpr uint32_t kTargetUid = 0xDEADBEEFu;

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

void test_signed_frame_round_trips_through_dispatcher() {
    const uint8_t bag[] = {
        0x01,                          // entry_count = 1
        0x05, 'g','r','o','u','p',     // key
        0x00,                          // U8
        0x01,                          // value_len
        0x03,                          // value
    };
    uint8_t frame[80] = {};
    const size_t n = build_signed_config_write(
        frame, sizeof(frame),
        /*self_source_id=*/0x0040,
        kTargetUid, kSecret,
        bag, sizeof(bag),
        /*numonce=*/42);
    TEST_ASSERT_GREATER_THAN_size_t(0, n);

    NumonceLru lru;
    CaptureCtx cap = {};
    const auto detail = process_config_write(
        frame, n, kTargetUid, kSecret, lru, capture_cb, &cap);
    TEST_ASSERT_EQUAL(ConfigRxResult::Accepted, detail.result);
    TEST_ASSERT_EQUAL_UINT64(42u, detail.numonce);
    TEST_ASSERT_EQUAL_STRING("group", cap.last_key);
    TEST_ASSERT_EQUAL_size_t(1, cap.count);
}

void test_empty_bag_round_trips() {
    uint8_t frame[64] = {};
    const size_t n = build_signed_config_write(
        frame, sizeof(frame),
        0x0040, kTargetUid, kSecret,
        /*bag=*/nullptr, /*bag_len=*/0,
        /*numonce=*/1);
    TEST_ASSERT_GREATER_THAN_size_t(0, n);
    NumonceLru lru;
    CaptureCtx cap = {};
    const auto detail = process_config_write(
        frame, n, kTargetUid, kSecret, lru, capture_cb, &cap);
    // Empty bag = zero-byte input to decode_property_bag. Its contract
    // is that an empty buffer surfaces as EmptyBuffer, which the
    // dispatcher currently maps to MalformedBag. Document the current
    // behaviour; if we ever want "accept empty bag as a no-op CONFIG_
    // WRITE for pure liveness-ping use, this test flips to Accepted.
    TEST_ASSERT_EQUAL(ConfigRxResult::MalformedBag, detail.result);
}

void test_max_bag_round_trips() {
    // Build a bag that's a single entry with a long bytes value so
    // the TLV body fills close to the 220-byte limit. The inner
    // decoder validates length prefixes, so an actually-walkable bag
    // is sufficient.
    uint8_t bag[kConfigWriteMaxBagLen];
    // Header: 1 entry_count byte + 1 key_len + 5 key + 1 type + 1 value_len = 9
    // Fill the rest with value bytes up to the cap minus a comfortable margin.
    uint8_t value_len = kConfigWriteMaxBagLen - 9;
    bag[0] = 0x01;
    bag[1] = 0x05;
    bag[2] = 'f'; bag[3] = 'o'; bag[4] = 'o'; bag[5] = 'o'; bag[6] = '!';
    bag[7] = 0x03;             // Bytes
    bag[8] = value_len;        // value_len
    for (size_t i = 0; i < value_len; ++i) bag[9 + i] = static_cast<uint8_t>(i & 0xFF);
    const uint8_t total_bag_len = static_cast<uint8_t>(9 + value_len);

    uint8_t frame[kHeaderSize + kConfigWriteMaxPayloadLen];
    const size_t n = build_signed_config_write(
        frame, sizeof(frame),
        0x0040, kTargetUid, kSecret,
        bag, total_bag_len,
        /*numonce=*/7);
    TEST_ASSERT_GREATER_THAN_size_t(0, n);

    NumonceLru lru;
    CaptureCtx cap = {};
    const auto detail = process_config_write(
        frame, n, kTargetUid, kSecret, lru, capture_cb, &cap);
    TEST_ASSERT_EQUAL(ConfigRxResult::Accepted, detail.result);
    TEST_ASSERT_EQUAL_size_t(1, cap.count);
    TEST_ASSERT_EQUAL_STRING("fooo!", cap.last_key);
}

void test_bag_too_big_returns_zero() {
    uint8_t frame[kHeaderSize + kConfigWriteMaxPayloadLen];
    const size_t n = build_signed_config_write(
        frame, sizeof(frame),
        0x0040, kTargetUid, kSecret,
        /*bag=*/nullptr, /*bag_len=*/kConfigWriteMaxBagLen + 1,   // too big
        /*numonce=*/1);
    TEST_ASSERT_EQUAL_size_t(0, n);
}

void test_small_buffer_returns_zero() {
    uint8_t tiny[10] = {};
    const uint8_t bag[] = {0x00};   // empty bag (entry_count=0)
    const size_t n = build_signed_config_write(
        tiny, sizeof(tiny),
        0x0040, kTargetUid, kSecret,
        bag, sizeof(bag),
        /*numonce=*/1);
    TEST_ASSERT_EQUAL_size_t(0, n);
}

void test_wrong_secret_at_receiver_rejects() {
    const uint8_t bag[] = {
        0x01, 0x05, 'g','r','o','u','p', 0x00, 0x01, 0x03,
    };
    uint8_t frame[80] = {};
    const size_t n = build_signed_config_write(
        frame, sizeof(frame),
        0x0040, kTargetUid, kSecret,
        bag, sizeof(bag),
        /*numonce=*/42);
    TEST_ASSERT_GREATER_THAN_size_t(0, n);

    // Receiver with a different secret rejects the HMAC.
    uint8_t wrong_secret[16];
    std::memcpy(wrong_secret, kSecret, 16);
    wrong_secret[0] ^= 0xFF;
    NumonceLru lru;
    CaptureCtx cap = {};
    const auto detail = process_config_write(
        frame, n, kTargetUid, wrong_secret, lru, capture_cb, &cap);
    TEST_ASSERT_EQUAL(ConfigRxResult::BadHmac, detail.result);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_signed_frame_round_trips_through_dispatcher);
    RUN_TEST(test_empty_bag_round_trips);
    RUN_TEST(test_max_bag_round_trips);
    RUN_TEST(test_bag_too_big_returns_zero);
    RUN_TEST(test_small_buffer_returns_zero);
    RUN_TEST(test_wrong_secret_at_receiver_rejects);
    return UNITY_END();
}
