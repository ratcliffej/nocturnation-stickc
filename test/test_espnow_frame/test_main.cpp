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

    const size_t n = encode_heartbeat(buf, sizeof(buf), in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kHeartbeatPayloadLen, n);
    assert_header_bytes(buf, MessageType::Heartbeat, kHeartbeatPayloadLen);

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    TEST_ASSERT_EQUAL_UINT8(kProtocolVersion,        decoded.protocol_version);
    TEST_ASSERT_EQUAL_UINT8(in.source_id,            decoded.source_id);
    TEST_ASSERT_EQUAL_UINT8(in.sequence_number,      decoded.sequence_number);
    TEST_ASSERT_EQUAL_UINT8(in.hop_count,            decoded.hop_count);
    TEST_ASSERT_EQUAL(MessageType::Heartbeat,        decoded.message_type);
    TEST_ASSERT_EQUAL_UINT8(kHeartbeatPayloadLen,    decoded.payload_len);

    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_heartbeat(decoded,
                                                        buf + kHeaderSize,
                                                        decoded.payload_len)));
}

static void test_beat_detected_round_trip(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    const BeatDetectedPayload p_in{ /*strength=*/200, /*bpm_x10=*/1380 };

    const size_t n = encode_beat_detected(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kBeatDetectedPayloadLen, n);
    assert_header_bytes(buf, MessageType::BeatDetected, kBeatDetectedPayloadLen);
    // Spot-check little-endian payload bytes: bpm_x10 = 1380 = 0x0564
    TEST_ASSERT_EQUAL_UINT8(200,  buf[kHeaderSize + 0]);  // strength
    TEST_ASSERT_EQUAL_UINT8(0x64, buf[kHeaderSize + 1]);  // bpm_x10 LSB
    TEST_ASSERT_EQUAL_UINT8(0x05, buf[kHeaderSize + 2]);  // bpm_x10 MSB

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    BeatDetectedPayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_beat_detected(decoded,
                                                            buf + kHeaderSize,
                                                            decoded.payload_len,
                                                            p_out)));
    TEST_ASSERT_EQUAL_UINT8(p_in.strength, p_out.strength);
    TEST_ASSERT_EQUAL_UINT16(p_in.bpm_x10, p_out.bpm_x10);
}

static void test_mode_change_round_trip(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    const ModeChangePayload p_in{ /*new_mode=*/3, /*palette_id=*/9 };

    const size_t n = encode_mode_change(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kModeChangePayloadLen, n);
    assert_header_bytes(buf, MessageType::ModeChange, kModeChangePayloadLen);
    TEST_ASSERT_EQUAL_UINT8(3, buf[kHeaderSize + 0]);
    TEST_ASSERT_EQUAL_UINT8(9, buf[kHeaderSize + 1]);

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    ModeChangePayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_mode_change(decoded,
                                                          buf + kHeaderSize,
                                                          decoded.payload_len,
                                                          p_out)));
    TEST_ASSERT_EQUAL_UINT8(p_in.new_mode,   p_out.new_mode);
    TEST_ASSERT_EQUAL_UINT8(p_in.palette_id, p_out.palette_id);
}

static void test_light_command_round_trip(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    const LightCommandPayload p_in{
        /*target_group=*/5,
        /*r=*/0xFF, /*g=*/0x80, /*b=*/0x10,
        /*attack=*/2, /*sustain=*/4, /*release=*/6,
        /*chance=*/3,
    };

    const size_t n = encode_light_command(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kLightCommandPayloadLen, n);
    assert_header_bytes(buf, MessageType::LightCommand, kLightCommandPayloadLen);
    TEST_ASSERT_EQUAL_UINT8(5,    buf[kHeaderSize + 0]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, buf[kHeaderSize + 1]);
    TEST_ASSERT_EQUAL_UINT8(0x80, buf[kHeaderSize + 2]);
    TEST_ASSERT_EQUAL_UINT8(0x10, buf[kHeaderSize + 3]);
    TEST_ASSERT_EQUAL_UINT8(2,    buf[kHeaderSize + 4]);
    TEST_ASSERT_EQUAL_UINT8(4,    buf[kHeaderSize + 5]);
    TEST_ASSERT_EQUAL_UINT8(6,    buf[kHeaderSize + 6]);
    TEST_ASSERT_EQUAL_UINT8(3,    buf[kHeaderSize + 7]);

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    LightCommandPayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_light_command(decoded,
                                                            buf + kHeaderSize,
                                                            decoded.payload_len,
                                                            p_out)));
    TEST_ASSERT_EQUAL_UINT8(p_in.target_group, p_out.target_group);
    TEST_ASSERT_EQUAL_UINT8(p_in.r,            p_out.r);
    TEST_ASSERT_EQUAL_UINT8(p_in.g,            p_out.g);
    TEST_ASSERT_EQUAL_UINT8(p_in.b,            p_out.b);
    TEST_ASSERT_EQUAL_UINT8(p_in.attack,       p_out.attack);
    TEST_ASSERT_EQUAL_UINT8(p_in.sustain,      p_out.sustain);
    TEST_ASSERT_EQUAL_UINT8(p_in.release,      p_out.release);
    TEST_ASSERT_EQUAL_UINT8(p_in.chance,       p_out.chance);
}

static void test_clock_sync_round_trip(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    const ClockSyncPayload p_in{
        /*phase_in_bar=*/0xBEEF,  // 48879
        /*bpm_x10=*/1380,
    };

    const size_t n = encode_clock_sync(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kClockSyncPayloadLen, n);
    assert_header_bytes(buf, MessageType::ClockSync, kClockSyncPayloadLen);
    // 0xBEEF little-endian: 0xEF, 0xBE
    TEST_ASSERT_EQUAL_UINT8(0xEF, buf[kHeaderSize + 0]);
    TEST_ASSERT_EQUAL_UINT8(0xBE, buf[kHeaderSize + 1]);
    TEST_ASSERT_EQUAL_UINT8(0x64, buf[kHeaderSize + 2]);
    TEST_ASSERT_EQUAL_UINT8(0x05, buf[kHeaderSize + 3]);

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    ClockSyncPayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_clock_sync(decoded,
                                                         buf + kHeaderSize,
                                                         decoded.payload_len,
                                                         p_out)));
    TEST_ASSERT_EQUAL_UINT16(p_in.phase_in_bar, p_out.phase_in_bar);
    TEST_ASSERT_EQUAL_UINT16(p_in.bpm_x10,      p_out.bpm_x10);
}

static void test_time_sync_round_trip(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    const TimeSyncPayload p_in{
        /*days_since_2026=*/128,            // 0x0080
        /*centiseconds_today=*/0x123456,    // u24 within u32
    };

    const size_t n = encode_time_sync(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kTimeSyncPayloadLen, n);
    assert_header_bytes(buf, MessageType::TimeSync, kTimeSyncPayloadLen);
    TEST_ASSERT_EQUAL_UINT8(0x80, buf[kHeaderSize + 0]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[kHeaderSize + 1]);
    TEST_ASSERT_EQUAL_UINT8(0x56, buf[kHeaderSize + 2]);
    TEST_ASSERT_EQUAL_UINT8(0x34, buf[kHeaderSize + 3]);
    TEST_ASSERT_EQUAL_UINT8(0x12, buf[kHeaderSize + 4]);

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    TimeSyncPayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_time_sync(decoded,
                                                        buf + kHeaderSize,
                                                        decoded.payload_len,
                                                        p_out)));
    TEST_ASSERT_EQUAL_UINT16(p_in.days_since_2026,    p_out.days_since_2026);
    TEST_ASSERT_EQUAL_UINT32(p_in.centiseconds_today, p_out.centiseconds_today);
}

// ---------------------------------------------------------------------------
// Encoder rejects buffer-too-small without writing past the end
// ---------------------------------------------------------------------------

static void test_encode_buffer_too_small(void) {
    uint8_t buf[kHeaderSize] = {};  // one byte short for any non-empty payload
    const Header in = make_header();

    TEST_ASSERT_EQUAL_size_t(0,
        encode_beat_detected(buf, sizeof(buf), in, BeatDetectedPayload{0, 0}));
    TEST_ASSERT_EQUAL_size_t(0,
        encode_light_command(buf, sizeof(buf), in, LightCommandPayload{}));
    TEST_ASSERT_EQUAL_size_t(0,
        encode_time_sync(buf, sizeof(buf), in, TimeSyncPayload{0, 0}));

    // Heartbeat fits exactly in kHeaderSize (zero payload).
    TEST_ASSERT_EQUAL_size_t(kHeaderSize,
        encode_heartbeat(buf, sizeof(buf), in));
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
    // Header claims 10 bytes of payload but only 0 follow.
    uint8_t buf[kHeaderSize] = { kProtocolVersion, 1, 2, 0,
                                 static_cast<uint8_t>(MessageType::BeatDetected), 10 };
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
    encode_beat_detected(buf, sizeof(buf), in, BeatDetectedPayload{0, 0});

    Header h{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, sizeof(buf), h)));

    // Ask the LightCommand decoder to decode a BeatDetected frame.
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
    buf[4] = static_cast<uint8_t>(MessageType::BeatDetected);
    buf[5] = 5;  // wrong; expected 3
    // 5 bytes of "payload" (zeros).
    const size_t total = kHeaderSize + 5;

    Header h{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, total, h)));

    BeatDetectedPayload p{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::PayloadLenMismatch),
                      static_cast<int>(decode_beat_detected(h,
                                                            buf + kHeaderSize,
                                                            h.payload_len,
                                                            p)));
}

static void test_payload_decoder_caller_payload_len_argument_mismatch(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    encode_clock_sync(buf, sizeof(buf), in, ClockSyncPayload{0, 0});

    Header h{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, sizeof(buf), h)));

    ClockSyncPayload p{};
    // Caller passes a wrong payload_len (e.g. truncated by an outer protocol).
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::PayloadLenMismatch),
                      static_cast<int>(decode_clock_sync(h,
                                                         buf + kHeaderSize,
                                                         /*payload_len=*/2,
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
// Wire-format spot check: a fully hand-encoded BEAT_DETECTED frame matches the
// spec layout byte-for-byte. Guards against accidental field-order regressions.
// ---------------------------------------------------------------------------

static void test_beat_detected_wire_format_byte_for_byte(void) {
    uint8_t buf[kMaxFrameSize] = {};
    Header in{};
    in.protocol_version = kProtocolVersion;  // overwritten by encoder anyway
    in.source_id        = 0x21;
    in.sequence_number  = 0x07;
    in.hop_count        = 0x02;

    const BeatDetectedPayload p{ /*strength=*/0xCA, /*bpm_x10=*/0x1234 };
    const size_t n = encode_beat_detected(buf, sizeof(buf), in, p);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kBeatDetectedPayloadLen, n);

    const uint8_t expected[kHeaderSize + kBeatDetectedPayloadLen] = {
        0x01,  // protocol_version
        0x21,  // source_id
        0x07,  // sequence_number
        0x02,  // hop_count
        0x01,  // message_type = BEAT_DETECTED
        0x03,  // payload_len = 3
        0xCA,  // strength
        0x34,  // bpm_x10 LSB
        0x12,  // bpm_x10 MSB
    };
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(buf, expected, sizeof(expected)));
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_heartbeat_round_trip);
    RUN_TEST(test_beat_detected_round_trip);
    RUN_TEST(test_mode_change_round_trip);
    RUN_TEST(test_light_command_round_trip);
    RUN_TEST(test_clock_sync_round_trip);
    RUN_TEST(test_time_sync_round_trip);
    RUN_TEST(test_encode_buffer_too_small);
    RUN_TEST(test_decode_header_buffer_too_short);
    RUN_TEST(test_decode_header_bad_protocol_version);
    RUN_TEST(test_decode_header_unknown_message_type);
    RUN_TEST(test_decode_header_payload_len_overruns_buffer);
    RUN_TEST(test_payload_decoder_wrong_message_type);
    RUN_TEST(test_payload_decoder_wrong_payload_len_in_header);
    RUN_TEST(test_payload_decoder_caller_payload_len_argument_mismatch);
    RUN_TEST(test_decode_header_extension_type_recognised);
    RUN_TEST(test_beat_detected_wire_format_byte_for_byte);
    return UNITY_END();
}
