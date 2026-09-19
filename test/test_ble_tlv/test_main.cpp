// Property-bag TLV codec tests (Epic 20 B3b).
//
// Round-trip: build a bag via the encoder, decode it, assert each entry
// surfaces with the exact bytes we wrote.
// Overflow: the encoder must refuse a write that wouldn't fit and leave
// the buffer intact.
// Malformed: the decoder must reject inputs whose entry claims a length
// past the buffer end, without reading past it.
// Well-known keys: sanity check that the key constants match the spec.

#include <unity.h>
#include <cstring>

#include "../../src/ble/property_bag_tlv.h"

using namespace nocturnation::ble;

namespace {

// Callback context capturing entries into a caller-owned array.
struct CaptureCtx {
    struct Entry {
        char        key[32];
        uint8_t     key_len;
        ValueType   type;
        uint8_t     value[64];
        uint8_t     value_len;
    };
    Entry   entries[8] = {};
    uint8_t count      = 0;
    // Return-false sentinel: if set to N, the (N+1)th callback returns false.
    int     abort_after = -1;
};

bool capture_cb(const TlvEntry& e, void* raw_ctx) {
    auto* ctx = static_cast<CaptureCtx*>(raw_ctx);
    if (ctx->count >= 8) return false;
    auto& slot = ctx->entries[ctx->count];
    slot.key_len   = e.key_len;
    slot.type      = e.type;
    slot.value_len = e.value_len;
    for (uint8_t i = 0; i < e.key_len && i < sizeof(slot.key) - 1; ++i) slot.key[i] = e.key[i];
    slot.key[e.key_len < sizeof(slot.key) - 1 ? e.key_len : sizeof(slot.key) - 1] = '\0';
    for (uint8_t i = 0; i < e.value_len && i < sizeof(slot.value); ++i) slot.value[i] = e.value[i];
    ++ctx->count;
    if (ctx->abort_after >= 0 && ctx->count > ctx->abort_after) return false;
    return true;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// Encoder / decoder round-trip
// -----------------------------------------------------------------------------

static void test_empty_bag_encodes_as_single_zero_byte(void) {
    uint8_t buf[16] = {};
    TlvEncoder enc(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8(0, enc.entry_count());
    TEST_ASSERT_EQUAL_size_t(1, enc.size());
    TEST_ASSERT_EQUAL_UINT8(0, buf[0]);
}

static void test_u8_round_trip(void) {
    uint8_t buf[32] = {};
    TlvEncoder enc(buf, sizeof(buf));
    TEST_ASSERT_TRUE(enc.add_u8("group", 42));

    CaptureCtx ctx;
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeError::Ok),
                      static_cast<int>(decode_property_bag(buf, enc.size(), capture_cb, &ctx)));
    TEST_ASSERT_EQUAL_UINT8(1, ctx.count);
    TEST_ASSERT_EQUAL_STRING("group", ctx.entries[0].key);
    TEST_ASSERT_EQUAL(static_cast<int>(ValueType::U8), static_cast<int>(ctx.entries[0].type));
    TEST_ASSERT_EQUAL_UINT8(1, ctx.entries[0].value_len);
    TEST_ASSERT_EQUAL_UINT8(42, ctx.entries[0].value[0]);
}

static void test_u16_encoded_little_endian(void) {
    uint8_t buf[32] = {};
    TlvEncoder enc(buf, sizeof(buf));
    TEST_ASSERT_TRUE(enc.add_u16(key::kBoundSid, 0x4A5B));

    CaptureCtx ctx;
    decode_property_bag(buf, enc.size(), capture_cb, &ctx);
    TEST_ASSERT_EQUAL_UINT8(1, ctx.count);
    TEST_ASSERT_EQUAL_UINT8(2, ctx.entries[0].value_len);
    // little-endian: 0x4A5B -> [0x5B, 0x4A]
    TEST_ASSERT_EQUAL_UINT8(0x5B, ctx.entries[0].value[0]);
    TEST_ASSERT_EQUAL_UINT8(0x4A, ctx.entries[0].value[1]);
}

static void test_u32_encoded_little_endian(void) {
    uint8_t buf[32] = {};
    TlvEncoder enc(buf, sizeof(buf));
    TEST_ASSERT_TRUE(enc.add_u32("val", 0x12345678u));

    CaptureCtx ctx;
    decode_property_bag(buf, enc.size(), capture_cb, &ctx);
    TEST_ASSERT_EQUAL_UINT8(1, ctx.count);
    TEST_ASSERT_EQUAL_UINT8(4, ctx.entries[0].value_len);
    TEST_ASSERT_EQUAL_UINT8(0x78, ctx.entries[0].value[0]);
    TEST_ASSERT_EQUAL_UINT8(0x56, ctx.entries[0].value[1]);
    TEST_ASSERT_EQUAL_UINT8(0x34, ctx.entries[0].value[2]);
    TEST_ASSERT_EQUAL_UINT8(0x12, ctx.entries[0].value[3]);
}

static void test_bool_round_trip(void) {
    uint8_t buf[32] = {};
    TlvEncoder enc(buf, sizeof(buf));
    TEST_ASSERT_TRUE(enc.add_bool("flag_on",  true));
    TEST_ASSERT_TRUE(enc.add_bool("flag_off", false));

    CaptureCtx ctx;
    decode_property_bag(buf, enc.size(), capture_cb, &ctx);
    TEST_ASSERT_EQUAL_UINT8(2, ctx.count);
    TEST_ASSERT_EQUAL_UINT8(0x01, ctx.entries[0].value[0]);
    TEST_ASSERT_EQUAL_UINT8(0x00, ctx.entries[1].value[0]);
}

static void test_utf8_round_trip(void) {
    uint8_t buf[64] = {};
    TlvEncoder enc(buf, sizeof(buf));
    TEST_ASSERT_TRUE(enc.add_utf8(key::kFriendlyName, "Front Left Puppet"));

    CaptureCtx ctx;
    decode_property_bag(buf, enc.size(), capture_cb, &ctx);
    TEST_ASSERT_EQUAL_UINT8(1, ctx.count);
    TEST_ASSERT_EQUAL(static_cast<int>(ValueType::Utf8), static_cast<int>(ctx.entries[0].type));
    TEST_ASSERT_EQUAL_UINT8(17, ctx.entries[0].value_len);   // "Front Left Puppet"
    TEST_ASSERT_EQUAL_UINT8_ARRAY(reinterpret_cast<const uint8_t*>("Front Left Puppet"),
                                  ctx.entries[0].value, 17);
}

static void test_utf8_empty_string_valid(void) {
    uint8_t buf[32] = {};
    TlvEncoder enc(buf, sizeof(buf));
    TEST_ASSERT_TRUE(enc.add_utf8("name", ""));

    CaptureCtx ctx;
    decode_property_bag(buf, enc.size(), capture_cb, &ctx);
    TEST_ASSERT_EQUAL_UINT8(1, ctx.count);
    TEST_ASSERT_EQUAL_UINT8(0, ctx.entries[0].value_len);
}

static void test_bytes_round_trip(void) {
    uint8_t buf[32] = {};
    TlvEncoder enc(buf, sizeof(buf));
    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    TEST_ASSERT_TRUE(enc.add_bytes("blob", payload, sizeof(payload)));

    CaptureCtx ctx;
    decode_property_bag(buf, enc.size(), capture_cb, &ctx);
    TEST_ASSERT_EQUAL_UINT8(1, ctx.count);
    TEST_ASSERT_EQUAL_UINT8(4, ctx.entries[0].value_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, ctx.entries[0].value, 4);
}

static void test_multi_entry_bag_preserves_order(void) {
    uint8_t buf[64] = {};
    TlvEncoder enc(buf, sizeof(buf));
    TEST_ASSERT_TRUE(enc.add_u8 (key::kGroup,     3));
    TEST_ASSERT_TRUE(enc.add_u8 (key::kLedPower, 40));
    TEST_ASSERT_TRUE(enc.add_u16(key::kBoundSid, 0x4A00));

    CaptureCtx ctx;
    decode_property_bag(buf, enc.size(), capture_cb, &ctx);
    TEST_ASSERT_EQUAL_UINT8(3, ctx.count);
    TEST_ASSERT_EQUAL_STRING("group",     ctx.entries[0].key);
    TEST_ASSERT_EQUAL_STRING("led_power", ctx.entries[1].key);
    TEST_ASSERT_EQUAL_STRING("bound_sid", ctx.entries[2].key);
}

// -----------------------------------------------------------------------------
// Encoder overflow behaviour
// -----------------------------------------------------------------------------

static void test_overflow_refuses_write_atomically(void) {
    // Tiny buffer: just enough for the header + one u8 entry (1 header + 1
    // key_len + 5 key + 1 type + 1 value_len + 1 value = 10 bytes).
    uint8_t buf[10] = {};
    TlvEncoder enc(buf, sizeof(buf));
    TEST_ASSERT_TRUE (enc.add_u8("group", 1));
    // Second u8 would need another 10 bytes and overflow.
    TEST_ASSERT_FALSE(enc.add_u8("group", 2));

    // First entry survived: entry_count still 1, decode still finds it.
    TEST_ASSERT_EQUAL_UINT8(1, enc.entry_count());
    CaptureCtx ctx;
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeError::Ok),
                      static_cast<int>(decode_property_bag(buf, enc.size(), capture_cb, &ctx)));
    TEST_ASSERT_EQUAL_UINT8(1, ctx.count);
}

static void test_zero_length_buffer_rejects_every_add(void) {
    TlvEncoder enc(nullptr, 0);
    TEST_ASSERT_FALSE(enc.add_u8("group", 1));
    TEST_ASSERT_EQUAL_UINT8(0, enc.entry_count());
}

static void test_empty_key_rejected(void) {
    uint8_t buf[32] = {};
    TlvEncoder enc(buf, sizeof(buf));
    TEST_ASSERT_FALSE(enc.add_u8("", 1));
    TEST_ASSERT_EQUAL_UINT8(0, enc.entry_count());
}

// -----------------------------------------------------------------------------
// Decoder resilience
// -----------------------------------------------------------------------------

static void test_decode_empty_buffer_is_empty_buffer_error(void) {
    CaptureCtx ctx;
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeError::EmptyBuffer),
                      static_cast<int>(decode_property_bag(nullptr, 0, capture_cb, &ctx)));
}

static void test_decode_zero_entries_buffer_is_ok(void) {
    const uint8_t buf[1] = {0};
    CaptureCtx ctx;
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeError::Ok),
                      static_cast<int>(decode_property_bag(buf, 1, capture_cb, &ctx)));
    TEST_ASSERT_EQUAL_UINT8(0, ctx.count);
}

static void test_decode_malformed_key_len_past_buffer(void) {
    // entry_count=1, key_len=99, but the buffer only holds 3 more bytes.
    const uint8_t buf[5] = {0x01, 0x63, 'x', 'y', 'z'};
    CaptureCtx ctx;
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeError::Malformed),
                      static_cast<int>(decode_property_bag(buf, sizeof(buf), capture_cb, &ctx)));
}

static void test_decode_malformed_value_len_past_buffer(void) {
    // entry_count=1, key_len=1, key='k', type=0x00, value_len=99, no value.
    const uint8_t buf[5] = {0x01, 0x01, 'k', 0x00, 0x63};
    CaptureCtx ctx;
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeError::Malformed),
                      static_cast<int>(decode_property_bag(buf, sizeof(buf), capture_cb, &ctx)));
}

static void test_decode_zero_key_len_rejected(void) {
    // entry_count=1, key_len=0 — malformed per spec.
    const uint8_t buf[3] = {0x01, 0x00, 0x00};
    CaptureCtx ctx;
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeError::Malformed),
                      static_cast<int>(decode_property_bag(buf, sizeof(buf), capture_cb, &ctx)));
}

static void test_decode_callback_abort_propagates(void) {
    uint8_t buf[64] = {};
    TlvEncoder enc(buf, sizeof(buf));
    enc.add_u8("a", 1);
    enc.add_u8("b", 2);
    enc.add_u8("c", 3);

    CaptureCtx ctx;
    ctx.abort_after = 1;   // callback returns false after second entry
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeError::CallbackAbort),
                      static_cast<int>(decode_property_bag(buf, enc.size(), capture_cb, &ctx)));
    TEST_ASSERT_EQUAL_UINT8(2, ctx.count);   // saw 'a' and 'b' before aborting
}

// -----------------------------------------------------------------------------
// Well-known keys — sanity check
// -----------------------------------------------------------------------------

static void test_well_known_keys_match_spec(void) {
    TEST_ASSERT_EQUAL_STRING("group",         key::kGroup);
    TEST_ASSERT_EQUAL_STRING("led_power",     key::kLedPower);
    TEST_ASSERT_EQUAL_STRING("bound_sid",     key::kBoundSid);
    TEST_ASSERT_EQUAL_STRING("channel_pref",  key::kChannelPref);
    TEST_ASSERT_EQUAL_STRING("dir_sid_perf",  key::kDirSidPerf);
    TEST_ASSERT_EQUAL_STRING("retx_count",    key::kRetxCount);
    TEST_ASSERT_EQUAL_STRING("pair_win_s",    key::kPairWinS);
    TEST_ASSERT_EQUAL_STRING("friendly_name", key::kFriendlyName);
}

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_bag_encodes_as_single_zero_byte);
    RUN_TEST(test_u8_round_trip);
    RUN_TEST(test_u16_encoded_little_endian);
    RUN_TEST(test_u32_encoded_little_endian);
    RUN_TEST(test_bool_round_trip);
    RUN_TEST(test_utf8_round_trip);
    RUN_TEST(test_utf8_empty_string_valid);
    RUN_TEST(test_bytes_round_trip);
    RUN_TEST(test_multi_entry_bag_preserves_order);
    RUN_TEST(test_overflow_refuses_write_atomically);
    RUN_TEST(test_zero_length_buffer_rejects_every_add);
    RUN_TEST(test_empty_key_rejected);
    RUN_TEST(test_decode_empty_buffer_is_empty_buffer_error);
    RUN_TEST(test_decode_zero_entries_buffer_is_ok);
    RUN_TEST(test_decode_malformed_key_len_past_buffer);
    RUN_TEST(test_decode_malformed_value_len_past_buffer);
    RUN_TEST(test_decode_zero_key_len_rejected);
    RUN_TEST(test_decode_callback_abort_propagates);
    RUN_TEST(test_well_known_keys_match_spec);
    return UNITY_END();
}
