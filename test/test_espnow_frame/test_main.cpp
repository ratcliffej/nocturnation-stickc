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
    TEST_ASSERT_EQUAL_UINT8(kMagic0,          buf[0]);
    TEST_ASSERT_EQUAL_UINT8(kMagic1,          buf[1]);
    TEST_ASSERT_EQUAL_UINT8(kProtocolVersion, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(7,                buf[3]);  // source_id
    TEST_ASSERT_EQUAL_UINT8(42,               buf[4]);  // sequence_number
    TEST_ASSERT_EQUAL_UINT8(0,                buf[5]);  // hop_count
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected_type), buf[6]);
    TEST_ASSERT_EQUAL_UINT8(expected_payload_len, buf[7]);
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

static void test_light_pulse_round_trip(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    const LightPulsePayload p_in{
        /*target_class=*/0x01,         // Light class (Epic 4.65)
        /*target_group=*/5,
        /*r=*/0xFF, /*g=*/0x80, /*b=*/0x10,
        /*attack=*/2, /*sustain=*/4, /*release=*/6,
        /*chance=*/3,
    };

    const size_t n = encode_light_pulse(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kLightPulsePayloadLen, n);
    assert_header_bytes(buf, MessageType::LightPulse, kLightPulsePayloadLen);
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
    LightPulsePayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_light_pulse(decoded,
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
// Epic 6C Phase D: LIGHT_WASH family round-trips
// ---------------------------------------------------------------------------

static void test_light_wash_round_trip_with_drift(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    const LightWashPayload p_in{
        /*target_class=*/0x01, /*target_group=*/5,
        /*r1=*/255, /*g1=*/60,  /*b1=*/  0,
        /*r2=*/120, /*g2=*/30,  /*b2=*/200,
        /*attack=*/   20,
        /*release=*/  10,
        /*intensity=*/200,
        /*cycle_ms=*/  5000,
        /*ttl_seconds=*/0,
        /*pulse_response=*/1,
    };

    const size_t n = encode_light_wash(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kLightWashPayloadLen, n);
    assert_header_bytes(buf, MessageType::LightWash, kLightWashPayloadLen);
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[kHeaderSize +  0]);
    TEST_ASSERT_EQUAL_UINT8(   5, buf[kHeaderSize +  1]);
    TEST_ASSERT_EQUAL_UINT8( 255, buf[kHeaderSize +  2]);
    TEST_ASSERT_EQUAL_UINT8(  60, buf[kHeaderSize +  3]);
    TEST_ASSERT_EQUAL_UINT8(   0, buf[kHeaderSize +  4]);
    TEST_ASSERT_EQUAL_UINT8( 120, buf[kHeaderSize +  5]);
    TEST_ASSERT_EQUAL_UINT8(  30, buf[kHeaderSize +  6]);
    TEST_ASSERT_EQUAL_UINT8( 200, buf[kHeaderSize +  7]);
    TEST_ASSERT_EQUAL_UINT8(  20, buf[kHeaderSize +  8]);
    TEST_ASSERT_EQUAL_UINT8(  10, buf[kHeaderSize +  9]);
    TEST_ASSERT_EQUAL_UINT8( 200, buf[kHeaderSize + 10]);
    // cycle_ms 5000 = 0x1388 -> little-endian: 0x88, 0x13
    TEST_ASSERT_EQUAL_UINT8(0x88, buf[kHeaderSize + 11]);
    TEST_ASSERT_EQUAL_UINT8(0x13, buf[kHeaderSize + 12]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[kHeaderSize + 13]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[kHeaderSize + 14]);
    TEST_ASSERT_EQUAL_UINT8(   1, buf[kHeaderSize + 15]);

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    LightWashPayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_light_wash(decoded,
                                                          buf + kHeaderSize,
                                                          decoded.payload_len,
                                                          p_out)));
    TEST_ASSERT_EQUAL_UINT8 (p_in.target_class,   p_out.target_class);
    TEST_ASSERT_EQUAL_UINT8 (p_in.target_group,   p_out.target_group);
    TEST_ASSERT_EQUAL_UINT8 (p_in.r1, p_out.r1);
    TEST_ASSERT_EQUAL_UINT8 (p_in.g1, p_out.g1);
    TEST_ASSERT_EQUAL_UINT8 (p_in.b1, p_out.b1);
    TEST_ASSERT_EQUAL_UINT8 (p_in.r2, p_out.r2);
    TEST_ASSERT_EQUAL_UINT8 (p_in.g2, p_out.g2);
    TEST_ASSERT_EQUAL_UINT8 (p_in.b2, p_out.b2);
    TEST_ASSERT_EQUAL_UINT8 (p_in.attack,         p_out.attack);
    TEST_ASSERT_EQUAL_UINT8 (p_in.release,        p_out.release);
    TEST_ASSERT_EQUAL_UINT8 (p_in.intensity,      p_out.intensity);
    TEST_ASSERT_EQUAL_UINT16(p_in.cycle_ms,       p_out.cycle_ms);
    TEST_ASSERT_EQUAL_UINT16(p_in.ttl_seconds,    p_out.ttl_seconds);
    TEST_ASSERT_EQUAL_UINT8 (p_in.pulse_response, p_out.pulse_response);
}

static void test_light_wash_round_trip_cycle_ms_zero(void) {
    // cycle_ms=0 means "no cycle, hold r1/g1/b1". r2/g2/b2 are still on
    // the wire (the renderer ignores them); round-trip must preserve them.
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    const LightWashPayload p_in{
        /*target_class=*/0x00, /*target_group=*/0,
        /*r1=*/40, /*g1=*/80, /*b1=*/160,
        /*r2=*/99, /*g2=*/77, /*b2=*/ 55,
        /*attack=*/30, /*release=*/30,
        /*intensity=*/255,
        /*cycle_ms=*/0,
        /*ttl_seconds=*/3600,
        /*pulse_response=*/0,
    };

    const size_t n = encode_light_wash(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kLightWashPayloadLen, n);

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    LightWashPayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_light_wash(decoded,
                                                          buf + kHeaderSize,
                                                          decoded.payload_len,
                                                          p_out)));
    TEST_ASSERT_EQUAL_UINT16(0,    p_out.cycle_ms);
    TEST_ASSERT_EQUAL_UINT16(3600, p_out.ttl_seconds);
    TEST_ASSERT_EQUAL_UINT8 (99,   p_out.r2);
    TEST_ASSERT_EQUAL_UINT8 (77,   p_out.g2);
    TEST_ASSERT_EQUAL_UINT8 (55,   p_out.b2);
}

static void test_light_wash_end_round_trip(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    const LightWashEndPayload p_in{0x00, 0, 10};

    const size_t n = encode_light_wash_end(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kLightWashEndPayloadLen, n);
    assert_header_bytes(buf, MessageType::LightWashEnd, kLightWashEndPayloadLen);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[kHeaderSize + 0]);
    TEST_ASSERT_EQUAL_UINT8(   0, buf[kHeaderSize + 1]);
    TEST_ASSERT_EQUAL_UINT8(  10, buf[kHeaderSize + 2]);

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    LightWashEndPayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_light_wash_end(decoded,
                                                              buf + kHeaderSize,
                                                              decoded.payload_len,
                                                              p_out)));
    TEST_ASSERT_EQUAL_UINT8(p_in.target_class, p_out.target_class);
    TEST_ASSERT_EQUAL_UINT8(p_in.target_group, p_out.target_group);
    TEST_ASSERT_EQUAL_UINT8(p_in.release_time, p_out.release_time);
}

static void test_light_wash_pulse_round_trip(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    const LightWashPulsePayload p_in{
        0x01, 2,
        255, 255, 255,
        0, 1, 3,
        0,
    };

    const size_t n = encode_light_wash_pulse(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kLightWashPulsePayloadLen, n);
    assert_header_bytes(buf, MessageType::LightWashPulse, kLightWashPulsePayloadLen);

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    LightWashPulsePayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_light_wash_pulse(decoded,
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
        encode_light_pulse(buf, sizeof(buf), in, LightPulsePayload{}));
}

// ---------------------------------------------------------------------------
// Header decoder rejects malformed input
// ---------------------------------------------------------------------------

static void test_decode_header_buffer_too_short(void) {
    uint8_t buf[3] = { kMagic0, kMagic1, kProtocolVersion };
    Header h{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::BufferTooShort),
                      static_cast<int>(decode_header(buf, sizeof(buf), h)));
}

static void test_decode_header_invalid_magic_rejects_non_nocturnation_frame(void) {
    // A frame whose first two bytes are not "NN" is foreign ESP-NOW
    // traffic (or random RF noise that lined up). Reject before any
    // further validation - this is the cheapest disambiguator at
    // event-density channels where multiple ESP-NOW users coexist.
    uint8_t buf[kHeaderSize] = { 0x18, 0xFE, kProtocolVersion, 1, 2, 0,
                                 static_cast<uint8_t>(MessageType::Heartbeat), 0 };
    Header h{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::InvalidMagic),
                      static_cast<int>(decode_header(buf, sizeof(buf), h)));
}

static void test_decode_header_bad_protocol_version(void) {
    uint8_t buf[kHeaderSize] = { kMagic0, kMagic1, 0x99, 1, 2, 0,
                                 static_cast<uint8_t>(MessageType::Heartbeat), 0 };
    Header h{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::InvalidProtocolVersion),
                      static_cast<int>(decode_header(buf, sizeof(buf), h)));
}

static void test_decode_header_unknown_message_type(void) {
    uint8_t buf[kHeaderSize] = { kMagic0, kMagic1, kProtocolVersion, 1, 2, 0, 0x42, 0 };
    Header h{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::InvalidMessageType),
                      static_cast<int>(decode_header(buf, sizeof(buf), h)));
}

static void test_decode_header_payload_len_overruns_buffer(void) {
    // Header claims 32 bytes of payload but only 0 follow.
    uint8_t buf[kHeaderSize] = { kMagic0, kMagic1, kProtocolVersion, 1, 2, 0,
                                 static_cast<uint8_t>(MessageType::LightPulse), 32 };
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

    // Ask the LightPulse decoder to decode a Heartbeat frame.
    LightPulsePayload p{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::InvalidMessageType),
                      static_cast<int>(decode_light_pulse(h,
                                                            buf + kHeaderSize,
                                                            h.payload_len,
                                                            p)));
}

static void test_payload_decoder_wrong_payload_len_in_header(void) {
    // Hand-craft a frame whose header.payload_len is wrong for the type.
    uint8_t buf[kMaxFrameSize] = {};
    buf[0] = kMagic0;
    buf[1] = kMagic1;
    buf[2] = kProtocolVersion;
    buf[3] = 1;  // source_id
    buf[4] = 1;  // sequence_number
    buf[5] = 0;  // hop_count
    buf[6] = static_cast<uint8_t>(MessageType::LightPulse);
    buf[7] = 7;  // wrong; expected 9
    // 7 bytes of "payload" (zeros).
    const size_t total = kHeaderSize + 7;

    Header h{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, total, h)));

    LightPulsePayload p{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::PayloadLenMismatch),
                      static_cast<int>(decode_light_pulse(h,
                                                            buf + kHeaderSize,
                                                            h.payload_len,
                                                            p)));
}

static void test_payload_decoder_caller_payload_len_argument_mismatch(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    encode_light_pulse(buf, sizeof(buf), in, LightPulsePayload{});

    Header h{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, sizeof(buf), h)));

    LightPulsePayload p{};
    // Caller passes a wrong payload_len (e.g. truncated by an outer protocol).
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::PayloadLenMismatch),
                      static_cast<int>(decode_light_pulse(h,
                                                            buf + kHeaderSize,
                                                            /*payload_len=*/5,
                                                            p)));
}

// ---------------------------------------------------------------------------
// Sanity: extension reserved type is recognised by the header decoder, even
// though no payload struct is defined for it. Future-proofs forward compat.
// ---------------------------------------------------------------------------

static void test_decode_header_extension_type_recognised(void) {
    uint8_t buf[kHeaderSize] = { kMagic0, kMagic1, kProtocolVersion, 1, 2, 0,
                                 static_cast<uint8_t>(MessageType::Extension), 0 };
    Header h{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, sizeof(buf), h)));
    TEST_ASSERT_EQUAL(MessageType::Extension, h.message_type);
}

// ---------------------------------------------------------------------------
// Wire-format spot check: a fully hand-encoded HEARTBEAT frame matches the
// spec v2 §3.1 layout byte-for-byte. Guards against accidental field-order
// regressions on the only Director-emitted broadcast besides LIGHT_PULSE.
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
        0x4E,  // magic byte 0 ('N')
        0x4E,  // magic byte 1 ('N')
        0x02,  // protocol_version
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
// Source-id partitioning (protocol manual §3.4)
// ---------------------------------------------------------------------------

static void test_source_id_partition_boundary_values(void) {
    TEST_ASSERT_EQUAL_UINT8(0x00, kSourceIdCommunityMin);
    TEST_ASSERT_EQUAL_UINT8(0x3F, kSourceIdCommunityMax);
    TEST_ASSERT_EQUAL_UINT8(0x40, kSourceIdPerformanceMin);
    TEST_ASSERT_EQUAL_UINT8(0xFE, kSourceIdPerformanceMax);
    TEST_ASSERT_EQUAL_UINT8(0xFF, kBroadcastSourceId);

    // The two ranges are contiguous (no gap) and don't overlap.
    TEST_ASSERT_EQUAL_UINT8(kSourceIdCommunityMax + 1, kSourceIdPerformanceMin);
    // Performance range stops one short of broadcast.
    TEST_ASSERT_EQUAL_UINT8(kSourceIdPerformanceMax + 1, kBroadcastSourceId);
}

static void test_is_community_range(void) {
    TEST_ASSERT_TRUE (is_community_range(0x00));
    TEST_ASSERT_TRUE (is_community_range(0x01));
    TEST_ASSERT_TRUE (is_community_range(0x20));
    TEST_ASSERT_TRUE (is_community_range(0x3F));   // upper boundary
    TEST_ASSERT_FALSE(is_community_range(0x40));   // first Performance
    TEST_ASSERT_FALSE(is_community_range(0xFE));
    TEST_ASSERT_FALSE(is_community_range(0xFF));   // broadcast
}

static void test_is_performance_range(void) {
    TEST_ASSERT_FALSE(is_performance_range(0x00));
    TEST_ASSERT_FALSE(is_performance_range(0x3F));   // last community
    TEST_ASSERT_TRUE (is_performance_range(0x40));   // lower boundary
    TEST_ASSERT_TRUE (is_performance_range(0x7F));
    TEST_ASSERT_TRUE (is_performance_range(0xFE));   // upper boundary
    TEST_ASSERT_FALSE(is_performance_range(0xFF));   // broadcast
}

// ---------------------------------------------------------------------------
// set_hop_count: a repeater bumps hop_count in place without disturbing the
// rest of the frame. Regression guard for the v1->v2 magic-prefix migration
// bug where the repeater wrote hop_count to byte 3 (source_id) instead of
// byte 5, corrupting source_id and never incrementing the hop counter.
// ---------------------------------------------------------------------------

static void test_set_hop_count_targets_the_hop_byte_only(void) {
    // Encode a real LIGHT_PULSE: source_id=7, seq=42, hop_count=0.
    Header h = make_header(/*source_id=*/7, /*seq=*/42, /*hops=*/0);
    LightPulsePayload p{};
    p.target_class = 0; p.target_group = 0;
    p.r = 255; p.g = 128; p.b = 0;
    p.attack = 1; p.sustain = 2; p.release = 3; p.chance = 4;
    uint8_t buf[kHeaderSize + kLightPulsePayloadLen];
    const size_t n = encode_light_pulse(buf, sizeof(buf), h, p);
    TEST_ASSERT_EQUAL_UINT(kHeaderSize + kLightPulsePayloadLen, n);

    // Sanity: the constant points where the layout says it does.
    TEST_ASSERT_EQUAL_UINT(5, kHopCountOffset);
    TEST_ASSERT_EQUAL_UINT8(0, buf[kHopCountOffset]);
    TEST_ASSERT_EQUAL_UINT8(7, buf[3]);   // source_id lives at byte 3

    // A repeater rebroadcasting at hop 1.
    set_hop_count(buf, n, 1);

    Header out{};
    TEST_ASSERT_EQUAL(DecodeResult::Ok, decode_header(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(1, out.hop_count);        // incremented
    TEST_ASSERT_EQUAL_UINT8(7, out.source_id);        // NOT corrupted
    TEST_ASSERT_EQUAL_UINT8(42, out.sequence_number); // preserved
}

static void test_set_hop_count_short_buffer_is_noop(void) {
    uint8_t tiny[kHeaderSize - 1] = {0};
    // Must not write out of bounds; returns the requested value unchanged.
    TEST_ASSERT_EQUAL_UINT8(2, set_hop_count(tiny, sizeof(tiny), 2));
    for (size_t i = 0; i < sizeof(tiny); ++i) {
        TEST_ASSERT_EQUAL_UINT8(0, tiny[i]);
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_set_hop_count_targets_the_hop_byte_only);
    RUN_TEST(test_set_hop_count_short_buffer_is_noop);
    RUN_TEST(test_heartbeat_round_trip);
    RUN_TEST(test_light_pulse_round_trip);
    RUN_TEST(test_light_wash_round_trip_with_drift);
    RUN_TEST(test_light_wash_round_trip_cycle_ms_zero);
    RUN_TEST(test_light_wash_end_round_trip);
    RUN_TEST(test_light_wash_pulse_round_trip);
    RUN_TEST(test_encode_buffer_too_small);
    RUN_TEST(test_decode_header_buffer_too_short);
    RUN_TEST(test_decode_header_invalid_magic_rejects_non_nocturnation_frame);
    RUN_TEST(test_decode_header_bad_protocol_version);
    RUN_TEST(test_decode_header_unknown_message_type);
    RUN_TEST(test_decode_header_payload_len_overruns_buffer);
    RUN_TEST(test_payload_decoder_wrong_message_type);
    RUN_TEST(test_payload_decoder_wrong_payload_len_in_header);
    RUN_TEST(test_payload_decoder_caller_payload_len_argument_mismatch);
    RUN_TEST(test_decode_header_extension_type_recognised);
    RUN_TEST(test_heartbeat_wire_format_byte_for_byte);
    RUN_TEST(test_source_id_partition_boundary_values);
    RUN_TEST(test_is_community_range);
    RUN_TEST(test_is_performance_range);
    // Epic 13 display-content codecs
    RUN_TEST(test_text_display_round_trip);
    RUN_TEST(test_text_display_empty_strings_round_trip);
    RUN_TEST(test_text_display_max_strings_round_trip);
    RUN_TEST(test_text_display_rejects_oversize_header);
    RUN_TEST(test_bitmap_header_round_trip);
    RUN_TEST(test_bitmap_header_rejects_invalid_dimensions);
    RUN_TEST(test_bitmap_plane_round_trip);
    RUN_TEST(test_bitmap_plane_rejects_oversize_data);
    RUN_TEST(test_clear_screen_round_trip);
    RUN_TEST(test_message_type_required_capability_map);
    return UNITY_END();
}
