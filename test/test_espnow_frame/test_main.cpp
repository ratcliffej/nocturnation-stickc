// Round-trip and malformed-input tests for the ESP-NOW frame format
// (include/transport/espnow/frame.h, src/transport/espnow/frame.cpp).
//
// Coverage:
//   - Encoder produces the expected bytes on the wire (header + payload).
//   - Decoder recovers identical fields from those same bytes.
//   - Buffer-too-small encode rejects.
//   - Header rejects: bad protocol version, unknown message type,
//     payload_len exceeds remaining buffer.
//   - Payload-decoder rejects: wrong message_type, wrong payload_len,
//     mismatched payload_len argument.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unity.h>
#include "transport/espnow/frame.h"

using namespace nocturnation::transport::espnow;

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static Header make_header(uint8_t source_id = 7,
                          uint8_t seq = 42,
                          uint8_t hops = 0) {
    Header h{};
    h.protocol_version = kProtocolVersion;
    h.source_id        = source_id;
    h.sequence_number  = seq;
    h.hop_count        = hops;
    return h;
}

static void assert_header_bytes(const uint8_t* buf,
                                MessageType expected_type,
                                uint8_t expected_payload_len) {
    TEST_ASSERT_EQUAL_UINT8(kProtocolVersion, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(7,                buf[1]);  // source_id
    TEST_ASSERT_EQUAL_UINT8(42,               buf[2]);  // sequence_number
    TEST_ASSERT_EQUAL_UINT8(0,                buf[3]);  // hop_count
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected_type), buf[4]);
    TEST_ASSERT_EQUAL_UINT8(expected_payload_len, buf[5]);
}

// ---------------------------------------------------------------------------
// Encoder + decoder round-trips, per message type
// ---------------------------------------------------------------------------

static void test_heartbeat_round_trip(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    // Pick values that exercise each field's byte width:
    //   tick               = 0x12345678
    //   days_since_2026    = 0x0123
    //   centiseconds_today = 0xABCDEF (24-bit ceiling)
    const HeartbeatPayload p_in{
        /*tick=*/               0x12345678u,
        /*days_since_2026=*/    0x0123u,
        /*centiseconds_today=*/ 0xABCDEFu,
    };

    const size_t n = encode_heartbeat(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kHeartbeatPayloadLen, n);
    assert_header_bytes(buf, MessageType::Heartbeat, kHeartbeatPayloadLen);

    // Spot-check little-endian payload bytes.
    TEST_ASSERT_EQUAL_UINT8(0x78, buf[kHeaderSize + 0]);  // tick LSB
    TEST_ASSERT_EQUAL_UINT8(0x56, buf[kHeaderSize + 1]);
    TEST_ASSERT_EQUAL_UINT8(0x34, buf[kHeaderSize + 2]);
    TEST_ASSERT_EQUAL_UINT8(0x12, buf[kHeaderSize + 3]);  // tick MSB
    TEST_ASSERT_EQUAL_UINT8(0x23, buf[kHeaderSize + 4]);  // days_since_2026 LSB
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[kHeaderSize + 5]);  // days_since_2026 MSB
    TEST_ASSERT_EQUAL_UINT8(0xEF, buf[kHeaderSize + 6]);  // centiseconds LSB
    TEST_ASSERT_EQUAL_UINT8(0xCD, buf[kHeaderSize + 7]);
    TEST_ASSERT_EQUAL_UINT8(0xAB, buf[kHeaderSize + 8]);  // centiseconds MSB (u24)

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    TEST_ASSERT_EQUAL_UINT8(kProtocolVersion,        decoded.protocol_version);
    TEST_ASSERT_EQUAL_UINT8(in.source_id,            decoded.source_id);
    TEST_ASSERT_EQUAL_UINT8(in.sequence_number,      decoded.sequence_number);
    TEST_ASSERT_EQUAL_UINT8(in.hop_count,            decoded.hop_count);
    TEST_ASSERT_EQUAL(MessageType::Heartbeat,        decoded.message_type);
    TEST_ASSERT_EQUAL_UINT8(kHeartbeatPayloadLen,    decoded.payload_len);

    HeartbeatPayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_heartbeat(decoded,
                                                        buf + kHeaderSize,
                                                        decoded.payload_len,
                                                        p_out)));
    TEST_ASSERT_EQUAL_UINT32(p_in.tick,               p_out.tick);
    TEST_ASSERT_EQUAL_UINT16(p_in.days_since_2026,    p_out.days_since_2026);
    TEST_ASSERT_EQUAL_UINT32(p_in.centiseconds_today, p_out.centiseconds_today);
}

static void test_light_command_round_trip(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    const LightCommandPayload p_in{
        /*target_class=*/0x01,         // Light class (Epic 4.65)
        /*target_group=*/5,
        /*r=*/0xFF, /*g=*/0x80, /*b=*/0x10,
        /*attack=*/2, /*sustain=*/4, /*release=*/6,
        /*chance=*/3,
    };

    const size_t n = encode_light_command(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kLightCommandPayloadLen, n);
    assert_header_bytes(buf, MessageType::LightCommand, kLightCommandPayloadLen);
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[kHeaderSize + 0]);   // target_class
    TEST_ASSERT_EQUAL_UINT8(5,    buf[kHeaderSize + 1]);   // target_group
    TEST_ASSERT_EQUAL_UINT8(0xFF, buf[kHeaderSize + 2]);
    TEST_ASSERT_EQUAL_UINT8(0x80, buf[kHeaderSize + 3]);
    TEST_ASSERT_EQUAL_UINT8(0x10, buf[kHeaderSize + 4]);
    TEST_ASSERT_EQUAL_UINT8(2,    buf[kHeaderSize + 5]);
    TEST_ASSERT_EQUAL_UINT8(4,    buf[kHeaderSize + 6]);
    TEST_ASSERT_EQUAL_UINT8(6,    buf[kHeaderSize + 7]);
    TEST_ASSERT_EQUAL_UINT8(3,    buf[kHeaderSize + 8]);

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    LightCommandPayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_light_command(decoded,
                                                            buf + kHeaderSize,
                                                            decoded.payload_len,
                                                            p_out)));
    TEST_ASSERT_EQUAL_UINT8(p_in.target_class, p_out.target_class);
    TEST_ASSERT_EQUAL_UINT8(p_in.target_group, p_out.target_group);
    TEST_ASSERT_EQUAL_UINT8(p_in.r,            p_out.r);
    TEST_ASSERT_EQUAL_UINT8(p_in.g,            p_out.g);
    TEST_ASSERT_EQUAL_UINT8(p_in.b,            p_out.b);
    TEST_ASSERT_EQUAL_UINT8(p_in.attack,       p_out.attack);
    TEST_ASSERT_EQUAL_UINT8(p_in.sustain,      p_out.sustain);
    TEST_ASSERT_EQUAL_UINT8(p_in.release,      p_out.release);
    TEST_ASSERT_EQUAL_UINT8(p_in.chance,       p_out.chance);
}

// ---------------------------------------------------------------------------
// Encoder rejects buffer-too-small without writing past the end
// ---------------------------------------------------------------------------

static void test_encode_buffer_too_small(void) {
    uint8_t buf[kHeaderSize] = {};  // one byte short for any non-empty payload
    const Header in = make_header();

    TEST_ASSERT_EQUAL_size_t(0,
        encode_heartbeat(buf, sizeof(buf), in, HeartbeatPayload{}));
    TEST_ASSERT_EQUAL_size_t(0,
        encode_light_command(buf, sizeof(buf), in, LightCommandPayload{}));
}

// ---------------------------------------------------------------------------
// Header decoder rejects malformed input
// ---------------------------------------------------------------------------

static void test_decode_header_buffer_too_short(void) {
    uint8_t buf[3] = { kProtocolVersion, 1, 2 };
    Header h{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::BufferTooShort),
                      static_cast<int>(decode_header(buf, sizeof(buf), h)));
}

static void test_decode_header_bad_protocol_version(void) {
    uint8_t buf[kHeaderSize] = { 0x99, 1, 2, 0,
                                 static_cast<uint8_t>(MessageType::Heartbeat), 0 };
    Header h{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::InvalidProtocolVersion),
                      static_cast<int>(decode_header(buf, sizeof(buf), h)));
}

static void test_decode_header_unknown_message_type(void) {
    uint8_t buf[kHeaderSize] = { kProtocolVersion, 1, 2, 0, 0x42, 0 };
    Header h{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::InvalidMessageType),
                      static_cast<int>(decode_header(buf, sizeof(buf), h)));
}

static void test_decode_header_payload_len_overruns_buffer(void) {
    // Header claims 32 bytes of payload but only 0 follow.
    uint8_t buf[kHeaderSize] = { kProtocolVersion, 1, 2, 0,
                                 static_cast<uint8_t>(MessageType::LightCommand), 32 };
    Header h{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::BufferTooShort),
                      static_cast<int>(decode_header(buf, sizeof(buf), h)));
}

// ---------------------------------------------------------------------------
// Payload-decoder rejects wrong message_type and wrong payload_len
// ---------------------------------------------------------------------------

static void test_payload_decoder_wrong_message_type(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    encode_heartbeat(buf, sizeof(buf), in, HeartbeatPayload{});

    Header h{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, sizeof(buf), h)));

    // Ask the LightCommand decoder to decode a Heartbeat frame.
    LightCommandPayload p{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::InvalidMessageType),
                      static_cast<int>(decode_light_command(h,
                                                            buf + kHeaderSize,
                                                            h.payload_len,
                                                            p)));
}

static void test_payload_decoder_wrong_payload_len_in_header(void) {
    // Hand-craft a frame whose header.payload_len is wrong for the type.
    uint8_t buf[kMaxFrameSize] = {};
    buf[0] = kProtocolVersion;
    buf[1] = 1;
    buf[2] = 1;
    buf[3] = 0;
    buf[4] = static_cast<uint8_t>(MessageType::LightCommand);
    buf[5] = 7;  // wrong; expected 9
    // 7 bytes of "payload" (zeros).
    const size_t total = kHeaderSize + 7;

    Header h{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, total, h)));

    LightCommandPayload p{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::PayloadLenMismatch),
                      static_cast<int>(decode_light_command(h,
                                                            buf + kHeaderSize,
                                                            h.payload_len,
                                                            p)));
}

static void test_payload_decoder_caller_payload_len_argument_mismatch(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    encode_light_command(buf, sizeof(buf), in, LightCommandPayload{});

    Header h{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, sizeof(buf), h)));

    LightCommandPayload p{};
    // Caller passes a wrong payload_len (e.g. truncated by an outer protocol).
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::PayloadLenMismatch),
                      static_cast<int>(decode_light_command(h,
                                                            buf + kHeaderSize,
                                                            /*payload_len=*/5,
                                                            p)));
}

// ---------------------------------------------------------------------------
// Sanity: extension reserved type is recognised by the header decoder, even
// though no payload struct is defined for it. Future-proofs forward compat.
// ---------------------------------------------------------------------------

static void test_decode_header_extension_type_recognised(void) {
    uint8_t buf[kHeaderSize] = { kProtocolVersion, 1, 2, 0,
                                 static_cast<uint8_t>(MessageType::Extension), 0 };
    Header h{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, sizeof(buf), h)));
    TEST_ASSERT_EQUAL(MessageType::Extension, h.message_type);
}

// ---------------------------------------------------------------------------
// Wire-format spot check: a fully hand-encoded HEARTBEAT frame matches the
// spec v0.29 §4.3 layout byte-for-byte. Guards against accidental field-order
// regressions on the only Director-emitted broadcast besides LIGHT_COMMAND.
// ---------------------------------------------------------------------------

static void test_heartbeat_wire_format_byte_for_byte(void) {
    uint8_t buf[kMaxFrameSize] = {};
    Header in{};
    in.protocol_version = kProtocolVersion;  // overwritten by encoder anyway
    in.source_id        = 0x21;
    in.sequence_number  = 0x07;
    in.hop_count        = 0x02;

    const HeartbeatPayload p{
        /*tick=*/               0x12345678u,
        /*days_since_2026=*/    0x0123u,
        /*centiseconds_today=*/ 0xABCDEFu,
    };
    const size_t n = encode_heartbeat(buf, sizeof(buf), in, p);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kHeartbeatPayloadLen, n);

    const uint8_t expected[kHeaderSize + kHeartbeatPayloadLen] = {
        0x01,  // protocol_version
        0x21,  // source_id
        0x07,  // sequence_number
        0x02,  // hop_count
        0x00,  // message_type = HEARTBEAT
        0x09,  // payload_len = 9
        0x78, 0x56, 0x34, 0x12,   // tick LE
        0x23, 0x01,               // days_since_2026 LE
        0xEF, 0xCD, 0xAB,         // centiseconds_today LE u24
    };
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(buf, expected, sizeof(expected)));
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_heartbeat_round_trip);
    RUN_TEST(test_light_command_round_trip);
    RUN_TEST(test_encode_buffer_too_small);
    RUN_TEST(test_decode_header_buffer_too_short);
    RUN_TEST(test_decode_header_bad_protocol_version);
    RUN_TEST(test_decode_header_unknown_message_type);
    RUN_TEST(test_decode_header_payload_len_overruns_buffer);
    RUN_TEST(test_payload_decoder_wrong_message_type);
    RUN_TEST(test_payload_decoder_wrong_payload_len_in_header);
    RUN_TEST(test_payload_decoder_caller_payload_len_argument_mismatch);
    RUN_TEST(test_decode_header_extension_type_recognised);
    RUN_TEST(test_heartbeat_wire_format_byte_for_byte);
    return UNITY_END();
}
