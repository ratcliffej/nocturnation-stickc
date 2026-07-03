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

static void test_repeater_heartbeat_round_trip(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    const RepeaterHeartbeatPayload p_in{
        /*uid=*/           {0xDE, 0xAD, 0xBE},
        /*channel=*/       11,
        /*uptime_s=*/      0x1234u,
        /*relayed_count=*/ 0x89ABCDEFu,
    };

    const size_t n = encode_repeater_heartbeat(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kRepeaterHeartbeatPayloadLen, n);
    assert_header_bytes(buf, MessageType::RepeaterHeartbeat,
                        kRepeaterHeartbeatPayloadLen);

    // Spot-check wire bytes: uid (3), channel, uptime u16 LE, relayed u32 LE.
    TEST_ASSERT_EQUAL_UINT8(0xDE, buf[kHeaderSize + 0]);
    TEST_ASSERT_EQUAL_UINT8(0xAD, buf[kHeaderSize + 1]);
    TEST_ASSERT_EQUAL_UINT8(0xBE, buf[kHeaderSize + 2]);
    TEST_ASSERT_EQUAL_UINT8(11,   buf[kHeaderSize + 3]);
    TEST_ASSERT_EQUAL_UINT8(0x34, buf[kHeaderSize + 4]);  // uptime LSB
    TEST_ASSERT_EQUAL_UINT8(0x12, buf[kHeaderSize + 5]);  // uptime MSB
    TEST_ASSERT_EQUAL_UINT8(0xEF, buf[kHeaderSize + 6]);  // relayed LSB
    TEST_ASSERT_EQUAL_UINT8(0xCD, buf[kHeaderSize + 7]);
    TEST_ASSERT_EQUAL_UINT8(0xAB, buf[kHeaderSize + 8]);
    TEST_ASSERT_EQUAL_UINT8(0x89, buf[kHeaderSize + 9]);  // relayed MSB

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    TEST_ASSERT_EQUAL(MessageType::RepeaterHeartbeat, decoded.message_type);
    TEST_ASSERT_EQUAL_UINT8(kRepeaterHeartbeatPayloadLen, decoded.payload_len);

    RepeaterHeartbeatPayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_repeater_heartbeat(
                          decoded, buf + kHeaderSize, decoded.payload_len,
                          p_out)));
    TEST_ASSERT_EQUAL_UINT8(p_in.uid[0],        p_out.uid[0]);
    TEST_ASSERT_EQUAL_UINT8(p_in.uid[1],        p_out.uid[1]);
    TEST_ASSERT_EQUAL_UINT8(p_in.uid[2],        p_out.uid[2]);
    TEST_ASSERT_EQUAL_UINT8(p_in.channel,       p_out.channel);
    TEST_ASSERT_EQUAL_UINT16(p_in.uptime_s,     p_out.uptime_s);
    TEST_ASSERT_EQUAL_UINT32(p_in.relayed_count, p_out.relayed_count);
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
// Epic 13: display-content payloads (TEXT_DISPLAY, BITMAP_HEADER,
// BITMAP_PLANE, CLEAR_SCREEN). Variable-length round-trips, fixed-size
// round-trips, and the new payload-length validation paths.
// ---------------------------------------------------------------------------

static void test_text_display_round_trip(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    TextDisplayPayload p_in{};
    p_in.target_group = 3;
    p_in.r            = 0xFF;
    p_in.g            = 0x80;
    p_in.b            = 0x10;
    p_in.ttl_ms       = 2500;
    const char hdr_str[] = "Coldplay";
    const char body_str[] = "Adventure of a Lifetime";
    p_in.header_len = static_cast<uint8_t>(sizeof(hdr_str) - 1);
    std::memcpy(p_in.header, hdr_str, p_in.header_len);
    p_in.body_len = static_cast<uint8_t>(sizeof(body_str) - 1);
    std::memcpy(p_in.body, body_str, p_in.body_len);

    const size_t n = encode_text_display(buf, sizeof(buf), in, p_in);
    const size_t expected_total = kHeaderSize + kTextDisplayMinPayloadLen
                                + p_in.header_len + p_in.body_len;
    TEST_ASSERT_EQUAL_size_t(expected_total, n);
    assert_header_bytes(buf, MessageType::TextDisplay,
                        static_cast<uint8_t>(n - kHeaderSize));

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    TextDisplayPayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_text_display(decoded,
                                                            buf + kHeaderSize,
                                                            decoded.payload_len,
                                                            p_out)));
    TEST_ASSERT_EQUAL_UINT8 (p_in.target_group, p_out.target_group);
    TEST_ASSERT_EQUAL_UINT8 (p_in.r, p_out.r);
    TEST_ASSERT_EQUAL_UINT8 (p_in.g, p_out.g);
    TEST_ASSERT_EQUAL_UINT8 (p_in.b, p_out.b);
    TEST_ASSERT_EQUAL_UINT16(p_in.ttl_ms, p_out.ttl_ms);
    TEST_ASSERT_EQUAL_UINT8 (p_in.header_len, p_out.header_len);
    TEST_ASSERT_EQUAL_INT   (0, std::memcmp(p_in.header, p_out.header, p_in.header_len));
    TEST_ASSERT_EQUAL_UINT8 (p_in.body_len, p_out.body_len);
    TEST_ASSERT_EQUAL_INT   (0, std::memcmp(p_in.body, p_out.body, p_in.body_len));
}

static void test_text_display_empty_strings_round_trip(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    TextDisplayPayload p_in{};
    p_in.target_group = 0;
    p_in.r = 0; p_in.g = 0; p_in.b = 0;
    p_in.ttl_ms = 0;
    p_in.header_len = 0;
    p_in.body_len = 0;

    const size_t n = encode_text_display(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kTextDisplayMinPayloadLen, n);

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    TextDisplayPayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_text_display(decoded,
                                                            buf + kHeaderSize,
                                                            decoded.payload_len,
                                                            p_out)));
    TEST_ASSERT_EQUAL_UINT8(0, p_out.header_len);
    TEST_ASSERT_EQUAL_UINT8(0, p_out.body_len);
}

// Maximum-size text frame: header 64 bytes + body 128 bytes.
// 8 + 8 + 64 + 128 = 208 bytes total - well inside the 250-byte MTU.
static void test_text_display_max_strings_round_trip(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    TextDisplayPayload p_in{};
    p_in.target_group = 0;
    p_in.ttl_ms = 0;
    p_in.header_len = kTextDisplayMaxHeaderLen;
    for (size_t i = 0; i < p_in.header_len; ++i) p_in.header[i] = 'A' + (i & 0x0F);
    p_in.body_len = kTextDisplayMaxBodyLen;
    for (size_t i = 0; i < p_in.body_len; ++i) p_in.body[i] = 'a' + (i & 0x0F);

    const size_t n = encode_text_display(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kTextDisplayMaxPayloadLen, n);

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    TextDisplayPayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_text_display(decoded,
                                                            buf + kHeaderSize,
                                                            decoded.payload_len,
                                                            p_out)));
    TEST_ASSERT_EQUAL_UINT8(kTextDisplayMaxHeaderLen, p_out.header_len);
    TEST_ASSERT_EQUAL_UINT8(kTextDisplayMaxBodyLen, p_out.body_len);
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(p_in.header, p_out.header, p_in.header_len));
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(p_in.body, p_out.body, p_in.body_len));
}

static void test_text_display_rejects_oversize_header(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    TextDisplayPayload p_in{};
    p_in.header_len = kTextDisplayMaxHeaderLen + 1;   // 65 - over the cap
    const size_t n = encode_text_display(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(0, n);
}

static void test_bitmap_header_round_trip(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    BitmapHeaderPayload p_in{};
    p_in.target_group = 2;
    p_in.width  = 32;
    p_in.height = 32;
    p_in.plane_count = 3;
    p_in.colours[0][0] = 0xFF; p_in.colours[0][1] = 0x00; p_in.colours[0][2] = 0xFF;
    p_in.colours[1][0] = 0x00; p_in.colours[1][1] = 0xFF; p_in.colours[1][2] = 0xFF;
    p_in.colours[2][0] = 0xFF; p_in.colours[2][1] = 0xFF; p_in.colours[2][2] = 0xFF;
    p_in.fit       = 1;     // FIT
    p_in.zoom_pct  = 100;
    p_in.overwrite = 1;
    p_in.checksum  = 0xDEADBEEFu;
    p_in.ttl_ms    = 5000;

    const size_t n = encode_bitmap_header(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kBitmapHeaderPayloadLen, n);
    assert_header_bytes(buf, MessageType::BitmapHeader, kBitmapHeaderPayloadLen);

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    BitmapHeaderPayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_bitmap_header(decoded,
                                                             buf + kHeaderSize,
                                                             decoded.payload_len,
                                                             p_out)));
    TEST_ASSERT_EQUAL_UINT8 (p_in.target_group, p_out.target_group);
    TEST_ASSERT_EQUAL_UINT8 (p_in.width,        p_out.width);
    TEST_ASSERT_EQUAL_UINT8 (p_in.height,       p_out.height);
    TEST_ASSERT_EQUAL_UINT8 (p_in.plane_count,  p_out.plane_count);
    TEST_ASSERT_EQUAL_INT   (0, std::memcmp(p_in.colours, p_out.colours,
                                            sizeof(p_in.colours)));
    TEST_ASSERT_EQUAL_UINT8 (p_in.fit,           p_out.fit);
    TEST_ASSERT_EQUAL_UINT8 (p_in.zoom_pct,      p_out.zoom_pct);
    TEST_ASSERT_EQUAL_UINT8 (p_in.overwrite,     p_out.overwrite);
    TEST_ASSERT_EQUAL_UINT32(p_in.checksum,      p_out.checksum);
    TEST_ASSERT_EQUAL_UINT16(p_in.ttl_ms,        p_out.ttl_ms);
}

static void test_bitmap_header_rejects_invalid_dimensions(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    BitmapHeaderPayload p_in{};
    p_in.plane_count = 1;
    p_in.colours[0][0] = 0xFF;
    p_in.width  = 0;     // invalid
    p_in.height = 16;
    TEST_ASSERT_EQUAL_size_t(0, encode_bitmap_header(buf, sizeof(buf), in, p_in));
    p_in.width  = 16;
    p_in.height = kBitmapMaxDimension + 1;   // invalid
    TEST_ASSERT_EQUAL_size_t(0, encode_bitmap_header(buf, sizeof(buf), in, p_in));
    p_in.height = 16;
    p_in.plane_count = kBitmapMaxPlanes + 1;   // invalid
    TEST_ASSERT_EQUAL_size_t(0, encode_bitmap_header(buf, sizeof(buf), in, p_in));
}

static void test_bitmap_plane_round_trip(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    BitmapPlanePayload p_in{};
    p_in.target_group = 2;
    p_in.plane_index  = 1;
    p_in.byte_offset  = 0x1234;
    p_in.data_len     = 128;   // 32x32 binary plane = 128 bytes - fits in one frame
    for (size_t i = 0; i < p_in.data_len; ++i) {
        p_in.data[i] = static_cast<uint8_t>(0xA0 + (i & 0x1F));
    }

    const size_t n = encode_bitmap_plane(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kBitmapPlaneFixedPrefixLen + p_in.data_len, n);
    assert_header_bytes(buf, MessageType::BitmapPlane,
                        static_cast<uint8_t>(n - kHeaderSize));

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    BitmapPlanePayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_bitmap_plane(decoded,
                                                            buf + kHeaderSize,
                                                            decoded.payload_len,
                                                            p_out)));
    TEST_ASSERT_EQUAL_UINT8 (p_in.target_group, p_out.target_group);
    TEST_ASSERT_EQUAL_UINT8 (p_in.plane_index,  p_out.plane_index);
    TEST_ASSERT_EQUAL_UINT16(p_in.byte_offset,  p_out.byte_offset);
    TEST_ASSERT_EQUAL_UINT8 (p_in.data_len,     p_out.data_len);
    TEST_ASSERT_EQUAL_INT   (0, std::memcmp(p_in.data, p_out.data, p_in.data_len));
}

static void test_bitmap_plane_rejects_oversize_data(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    BitmapPlanePayload p_in{};
    p_in.data_len = kBitmapPlaneMaxDataLen + 1;   // one past the cap
    TEST_ASSERT_EQUAL_size_t(0, encode_bitmap_plane(buf, sizeof(buf), in, p_in));
}

static void test_clear_screen_round_trip(void) {
    uint8_t buf[kMaxFrameSize] = {};
    const Header in = make_header();
    const ClearScreenPayload p_in{
        /*target_group=*/0,
        /*clear_text=*/1,      /*clear_bitmap=*/0,
    };

    const size_t n = encode_clear_screen(buf, sizeof(buf), in, p_in);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kClearScreenPayloadLen, n);
    assert_header_bytes(buf, MessageType::ClearScreen, kClearScreenPayloadLen);

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    ClearScreenPayload p_out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_clear_screen(decoded,
                                                            buf + kHeaderSize,
                                                            decoded.payload_len,
                                                            p_out)));
    TEST_ASSERT_EQUAL_UINT8(0, p_out.target_group);
    TEST_ASSERT_EQUAL_UINT8(1, p_out.clear_text);
    TEST_ASSERT_EQUAL_UINT8(0, p_out.clear_bitmap);
}

// Capability-required map: verify the four Display family entries map
// to the correct HAL capabilities, and that the wash family / Heartbeat
// return the "no-specific-capability" sentinel.
static void test_message_type_required_capability_map(void) {
    using nocturnation::hal::Capability;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Capability::DisplayText),
                          static_cast<int>(message_type_required_capability(MessageType::TextDisplay)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Capability::DisplayBitmap),
                          static_cast<int>(message_type_required_capability(MessageType::BitmapHeader)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Capability::DisplayBitmap),
                          static_cast<int>(message_type_required_capability(MessageType::BitmapPlane)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Capability::DisplayText),
                          static_cast<int>(message_type_required_capability(MessageType::ClearScreen)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kNoSpecificCapability),
                          static_cast<int>(message_type_required_capability(MessageType::Heartbeat)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kNoSpecificCapability),
                          static_cast<int>(message_type_required_capability(MessageType::LightPulse)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kNoSpecificCapability),
                          static_cast<int>(message_type_required_capability(MessageType::LightWash)));
}

// ---------------------------------------------------------------------------
// Epic 15 bench-found bug: relay hop_count byte offset
// ---------------------------------------------------------------------------
//
// 2026-06-28: lume_mode.cpp relay path was writing the incremented
// hop_count to byte offset 3 (the source_id position) instead of
// byte 5 (the actual hop_count position). The relay went out on the
// wire with a corrupted source_id, and receivers' TOFU lock rejected
// every relayed frame. Symptom at the bench: Tildagon's debug
// overlay showed Hop: 0 () despite the repeater's R counter
// climbing - i.e. relay TX was firing but never accepted.
//
// This test pins the byte offset by simulating exactly the relay's
// rewrite (memcpy + buf[5] = hop+1) and asserting the decoded frame
// retains the original source_id + has the incremented hop_count.

static void test_relay_hop_increment_preserves_source_id(void) {
    uint8_t buf[kHeaderSize + kLightPulsePayloadLen] = {};
    const Header in = make_header(/*source_id=*/0x42,
                                  /*seq=*/42,
                                  /*hops=*/0);
    const LightPulsePayload payload{};
    const size_t n = encode_light_pulse(buf, sizeof(buf), in, payload);
    TEST_ASSERT_EQUAL_size_t(kHeaderSize + kLightPulsePayloadLen, n);

    // Exercise the relay's in-place bump via the same helper the
    // repeater uses (lume_mode.cpp) - not a raw byte-index rewrite -
    // so this test pins the helper's behaviour, not just the current
    // offset value.
    set_hop_count(buf, n, in.hop_count + 1);

    Header decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decode_header(buf, n, decoded)));
    // source_id MUST be preserved - the receiver's TOFU lock keys
    // off this byte. Pre-fix the relay overwrote this with
    // hop_count+1, corrupting the value.
    TEST_ASSERT_EQUAL_UINT8(in.source_id, decoded.source_id);
    // hop_count MUST be incremented so receivers can track relay
    // depth + the kMaxHopCount loop-prevention works.
    TEST_ASSERT_EQUAL_UINT8(in.hop_count + 1, decoded.hop_count);
    // Sequence number unchanged - dedup works mesh-wide on
    // (source_id, sequence_number).
    TEST_ASSERT_EQUAL_UINT8(in.sequence_number, decoded.sequence_number);
}

// Direct unit test of set_hop_count's contract: it touches ONLY the
// hop_count byte at kHopCountOffset and leaves every other header byte
// alone. Guards against a future refactor that widens the write and
// silently corrupts an adjacent field.
static void test_set_hop_count_only_touches_hop_byte(void) {
    uint8_t buf[kHeaderSize + kLightPulsePayloadLen] = {};
    const Header in = make_header(/*source_id=*/0x42,
                                  /*seq=*/99,
                                  /*hops=*/0);
    const LightPulsePayload payload{};
    const size_t n = encode_light_pulse(buf, sizeof(buf), in, payload);

    uint8_t before[sizeof(buf)];
    std::memcpy(before, buf, sizeof(buf));

    set_hop_count(buf, n, 2);

    // Every byte OTHER THAN kHopCountOffset is unchanged.
    for (size_t i = 0; i < n; ++i) {
        if (i == kHopCountOffset) continue;
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(before[i], buf[i],
                                         "set_hop_count modified a byte outside the hop_count slot");
    }
    // hop_count byte is the value we wrote.
    TEST_ASSERT_EQUAL_UINT8(2, buf[kHopCountOffset]);
}

// set_hop_count is a no-op on short buffers - the caller may pass a
// truncated frame and must not get a stray write past the buffer.
static void test_set_hop_count_no_op_on_short_buffer(void) {
    uint8_t buf[kHeaderSize] = {};
    for (size_t len = 0; len < kHeaderSize; ++len) {
        uint8_t snapshot[kHeaderSize];
        std::memcpy(snapshot, buf, sizeof(buf));
        set_hop_count(buf, len, 0xAA);
        // No byte was written.
        for (size_t i = 0; i < sizeof(buf); ++i) {
            TEST_ASSERT_EQUAL_UINT8(snapshot[i], buf[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_relay_hop_increment_preserves_source_id);
    RUN_TEST(test_set_hop_count_only_touches_hop_byte);
    RUN_TEST(test_set_hop_count_no_op_on_short_buffer);
    RUN_TEST(test_heartbeat_round_trip);
    RUN_TEST(test_repeater_heartbeat_round_trip);
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
