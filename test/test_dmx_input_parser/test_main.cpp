// Host tests for DmxInputParser (Epic 7 B1).
//
// Pure state-machine coverage: framing happy path, junk-before-start,
// empty + full payloads, bad end-byte rejection, resync on stray
// 0x7E mid-frame, oversize length rejection, multiple back-to-back
// frames, and the convenience accessors (last_was_dmx_packet,
// copy_dmx_channels).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unity.h>

#include "dal/drivers/dmx_input_parser.h"

using namespace nocturnation::dal;
using namespace nocturnation::dal::enttec_pro;

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Feed a byte stream into the parser and return the result of the last
// feed_byte() call. Useful for "expect this stream to leave the parser
// with one frame committed".
static DmxParseResult feed_all(DmxInputParser& p, const uint8_t* bytes, size_t n) {
    DmxParseResult r = DmxParseResult::NeedMoreBytes;
    for (size_t i = 0; i < n; ++i) {
        r = p.feed_byte(bytes[i]);
    }
    return r;
}

// Build an Enttec Pro frame in `out`. Caller passes label, payload, and
// payload_len. Writes (1 + 1 + 2 + payload_len + 1) bytes. Returns
// total bytes written.
static size_t build_frame(uint8_t* out,
                          uint8_t label,
                          const uint8_t* payload,
                          uint16_t payload_len) {
    size_t i = 0;
    out[i++] = kStartByte;
    out[i++] = label;
    out[i++] = static_cast<uint8_t>(payload_len & 0xFF);
    out[i++] = static_cast<uint8_t>((payload_len >> 8) & 0xFF);
    for (uint16_t k = 0; k < payload_len; ++k) out[i++] = payload[k];
    out[i++] = kEndByte;
    return i;
}

// ---------------------------------------------------------------------------
// Trivial happy path: minimal valid frame
// ---------------------------------------------------------------------------

static void test_minimal_valid_frame(void) {
    DmxInputParser p;

    // Label 0x06, single-byte payload (just a start code 0x00).
    const uint8_t payload[] = {0x00};
    uint8_t frame[8];
    const size_t n = build_frame(frame, kLabelOutputOnlySendDmx, payload, 1);

    // All but the last byte are NeedMoreBytes; the last (end byte)
    // returns FrameComplete.
    for (size_t i = 0; i + 1 < n; ++i) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(DmxParseResult::NeedMoreBytes),
                              static_cast<int>(p.feed_byte(frame[i])));
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DmxParseResult::FrameComplete),
                          static_cast<int>(p.feed_byte(frame[n - 1])));

    TEST_ASSERT_EQUAL_UINT8(kLabelOutputOnlySendDmx, p.last_label());
    TEST_ASSERT_EQUAL_UINT16(1, p.last_payload_len());
    TEST_ASSERT_EQUAL_UINT8(0x00, p.last_payload()[0]);
    TEST_ASSERT_EQUAL_UINT32(1, p.frame_count());
    TEST_ASSERT_EQUAL_UINT32(0, p.reset_count());
}

// ---------------------------------------------------------------------------
// Junk before the start byte is silently dropped
// ---------------------------------------------------------------------------

static void test_junk_before_start_is_ignored(void) {
    DmxInputParser p;

    // Five bytes of garbage that aren't 0x7E. They should leave the
    // parser in WaitStart with no resets logged (the WaitStart "skip
    // non-start" path does not increment reset_count_).
    const uint8_t junk[] = {0x00, 0xFF, 0xE7, 0x12, 0x34};
    for (size_t i = 0; i < sizeof(junk); ++i) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(DmxParseResult::NeedMoreBytes),
                              static_cast<int>(p.feed_byte(junk[i])));
    }
    TEST_ASSERT_EQUAL_UINT32(0, p.reset_count());

    // Then a clean frame parses.
    const uint8_t payload[] = {0x00, 0xAB};
    uint8_t frame[9];
    const size_t n = build_frame(frame, kLabelOutputOnlySendDmx, payload, 2);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DmxParseResult::FrameComplete),
                          static_cast<int>(feed_all(p, frame, n)));
    TEST_ASSERT_EQUAL_UINT16(2, p.last_payload_len());
    TEST_ASSERT_EQUAL_UINT8(0xAB, p.last_payload()[1]);
}

// ---------------------------------------------------------------------------
// Empty payload (length=0) skips the data segment and goes straight
// from ReadLenHi to ReadEnd.
// ---------------------------------------------------------------------------

static void test_empty_payload(void) {
    DmxInputParser p;

    // 0x7E, label 0x0A (Get Widget Serial Number request), length 0, end.
    const uint8_t frame[] = {
        kStartByte, kLabelGetWidgetSerialReq, 0x00, 0x00, kEndByte
    };
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DmxParseResult::FrameComplete),
                          static_cast<int>(feed_all(p, frame, sizeof(frame))));
    TEST_ASSERT_EQUAL_UINT8(kLabelGetWidgetSerialReq, p.last_label());
    TEST_ASSERT_EQUAL_UINT16(0, p.last_payload_len());
}

// ---------------------------------------------------------------------------
// Full DMX universe: 513-byte payload (1 start code + 512 channels).
// ---------------------------------------------------------------------------

static void test_full_dmx_universe(void) {
    DmxInputParser p;

    // Build a 513-byte payload: start code 0x00 then channels
    // 1..512 with values (i % 256).
    uint8_t payload[513];
    payload[0] = 0x00;
    for (uint16_t i = 0; i < kDmxUniverseChannels; ++i) {
        payload[1 + i] = static_cast<uint8_t>(i & 0xFF);
    }
    uint8_t frame[5 + 513];
    const size_t n = build_frame(frame, kLabelOutputOnlySendDmx, payload, 513);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(DmxParseResult::FrameComplete),
                          static_cast<int>(feed_all(p, frame, n)));
    TEST_ASSERT_TRUE(p.last_was_dmx_packet());

    // Channels round-trip exactly.
    uint8_t out[kDmxUniverseChannels];
    std::memset(out, 0xCD, sizeof(out));   // poison sentinel
    const uint16_t copied = p.copy_dmx_channels(out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT16(kDmxUniverseChannels, copied);
    for (uint16_t i = 0; i < kDmxUniverseChannels; ++i) {
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(i & 0xFF), out[i]);
    }
}

// ---------------------------------------------------------------------------
// Bad end byte: parser rejects + bumps reset_count_, frame_count_ stays.
// ---------------------------------------------------------------------------

static void test_bad_end_byte_rejects(void) {
    DmxInputParser p;

    const uint8_t bad_frame[] = {
        kStartByte, kLabelOutputOnlySendDmx, 0x01, 0x00, 0x00, 0xFF   // ends with 0xFF not 0xE7
    };
    DmxParseResult last = DmxParseResult::NeedMoreBytes;
    for (size_t i = 0; i < sizeof(bad_frame); ++i) {
        last = p.feed_byte(bad_frame[i]);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DmxParseResult::Reset),
                          static_cast<int>(last));
    TEST_ASSERT_EQUAL_UINT32(0, p.frame_count());
    TEST_ASSERT_EQUAL_UINT32(1, p.reset_count());
}

// ---------------------------------------------------------------------------
// Mid-frame 0x7E re-anchors the parser without losing the new frame.
// ---------------------------------------------------------------------------

static void test_resync_on_stray_start_byte(void) {
    DmxInputParser p;

    // Start a frame, then hit a stray 0x7E where the end byte would be.
    // After resync the parser should be in ReadLabel and accept a fresh
    // frame starting from the next byte.
    const uint8_t prefix[] = {
        kStartByte, kLabelOutputOnlySendDmx, 0x01, 0x00, 0x00, kStartByte   // 0x7E instead of 0xE7
    };
    DmxParseResult last = DmxParseResult::NeedMoreBytes;
    for (size_t i = 0; i < sizeof(prefix); ++i) {
        last = p.feed_byte(prefix[i]);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DmxParseResult::Reset),
                          static_cast<int>(last));
    TEST_ASSERT_EQUAL_UINT32(1, p.reset_count());

    // Now feed the rest of a fresh frame: label, len, data, end. No
    // need for another 0x7E - the resync logic re-anchored on the
    // stray one.
    const uint8_t rest[] = {
        kLabelOutputOnlySendDmx, 0x01, 0x00, 0x00, kEndByte
    };
    last = DmxParseResult::NeedMoreBytes;
    for (size_t i = 0; i < sizeof(rest); ++i) {
        last = p.feed_byte(rest[i]);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DmxParseResult::FrameComplete),
                          static_cast<int>(last));
    TEST_ASSERT_EQUAL_UINT32(1, p.frame_count());
}

// ---------------------------------------------------------------------------
// Oversize length is rejected before any data buffer is touched.
// ---------------------------------------------------------------------------

static void test_oversize_length_rejected(void) {
    DmxInputParser p;

    // Length 0x0FFF (4095) > kPayloadBufSize (600). Parser must reject
    // at the ReadLenHi step.
    const uint8_t over[] = {
        kStartByte, kLabelOutputOnlySendDmx, 0xFF, 0x0F
    };
    DmxParseResult last = DmxParseResult::NeedMoreBytes;
    for (size_t i = 0; i < sizeof(over); ++i) {
        last = p.feed_byte(over[i]);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DmxParseResult::Reset),
                          static_cast<int>(last));
    TEST_ASSERT_EQUAL_UINT32(1, p.oversize_count());
    TEST_ASSERT_EQUAL_UINT32(1, p.reset_count());
}

// ---------------------------------------------------------------------------
// Back-to-back frames: parser handles a continuous stream cleanly.
// ---------------------------------------------------------------------------

static void test_multiple_frames_back_to_back(void) {
    DmxInputParser p;

    const uint8_t payload_a[] = {0x00, 0xAA};
    const uint8_t payload_b[] = {0x00, 0xBB};
    const uint8_t payload_c[] = {0x00, 0xCC};
    uint8_t stream[3 * 7];
    size_t off = 0;
    off += build_frame(stream + off, kLabelOutputOnlySendDmx, payload_a, 2);
    off += build_frame(stream + off, kLabelOutputOnlySendDmx, payload_b, 2);
    off += build_frame(stream + off, kLabelOutputOnlySendDmx, payload_c, 2);

    uint32_t complete_count = 0;
    for (size_t i = 0; i < off; ++i) {
        if (p.feed_byte(stream[i]) == DmxParseResult::FrameComplete) {
            ++complete_count;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(3, complete_count);
    TEST_ASSERT_EQUAL_UINT32(3, p.frame_count());
    TEST_ASSERT_EQUAL_UINT32(0, p.reset_count());

    // Most recently completed is the third frame.
    TEST_ASSERT_EQUAL_UINT8(0xCC, p.last_payload()[1]);
}

// ---------------------------------------------------------------------------
// 0x7E + 0xE7 are valid DMX channel values inside the data segment.
// Length is authoritative; the parser must not mistake them for
// framing bytes.
// ---------------------------------------------------------------------------

static void test_framing_bytes_in_data_are_data(void) {
    DmxInputParser p;

    // Payload: start code 0, then channel 1 = 0x7E, channel 2 = 0xE7.
    const uint8_t payload[] = {0x00, kStartByte, kEndByte};
    uint8_t frame[8];
    const size_t n = build_frame(frame, kLabelOutputOnlySendDmx, payload, 3);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(DmxParseResult::FrameComplete),
                          static_cast<int>(feed_all(p, frame, n)));
    uint8_t channels[2];
    const uint16_t copied = p.copy_dmx_channels(channels, sizeof(channels));
    TEST_ASSERT_EQUAL_UINT16(2, copied);
    TEST_ASSERT_EQUAL_UINT8(kStartByte, channels[0]);
    TEST_ASSERT_EQUAL_UINT8(kEndByte,   channels[1]);
}

// ---------------------------------------------------------------------------
// last_was_dmx_packet() semantics
// ---------------------------------------------------------------------------

static void test_last_was_dmx_packet_true_for_label6_start0(void) {
    DmxInputParser p;
    const uint8_t payload[] = {0x00, 0x01, 0x02};
    uint8_t frame[8];
    const size_t n = build_frame(frame, kLabelOutputOnlySendDmx, payload, 3);
    feed_all(p, frame, n);
    TEST_ASSERT_TRUE(p.last_was_dmx_packet());
}

static void test_last_was_dmx_packet_false_for_other_label(void) {
    DmxInputParser p;
    const uint8_t payload[] = {0x00};  // start code present but label wrong
    uint8_t frame[6];
    const size_t n = build_frame(frame, kLabelGetWidgetSerialReq, payload, 1);
    feed_all(p, frame, n);
    TEST_ASSERT_FALSE(p.last_was_dmx_packet());
}

static void test_last_was_dmx_packet_false_for_nonzero_start_code(void) {
    DmxInputParser p;
    const uint8_t payload[] = {0xFF, 0x00};  // start code 0xFF (RDM response)
    uint8_t frame[7];
    const size_t n = build_frame(frame, kLabelOutputOnlySendDmx, payload, 2);
    feed_all(p, frame, n);
    TEST_ASSERT_FALSE(p.last_was_dmx_packet());
}

// ---------------------------------------------------------------------------
// copy_dmx_channels() semantics
// ---------------------------------------------------------------------------

static void test_copy_dmx_truncates_to_out_size(void) {
    DmxInputParser p;
    // 10 channels.
    uint8_t payload[11];
    payload[0] = 0x00;
    for (uint8_t i = 0; i < 10; ++i) payload[1 + i] = i + 1;
    uint8_t frame[16];
    const size_t n = build_frame(frame, kLabelOutputOnlySendDmx, payload, 11);
    feed_all(p, frame, n);

    // Only space for 5 in out.
    uint8_t out[5] = {0xCD, 0xCD, 0xCD, 0xCD, 0xCD};
    const uint16_t copied = p.copy_dmx_channels(out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT16(5, copied);
    for (uint8_t i = 0; i < 5; ++i) {
        TEST_ASSERT_EQUAL_UINT8(i + 1, out[i]);
    }
}

static void test_copy_dmx_returns_zero_for_non_dmx_frame(void) {
    DmxInputParser p;
    const uint8_t payload[] = {0x00};
    uint8_t frame[6];
    const size_t n = build_frame(frame, kLabelGetWidgetSerialReq, payload, 1);
    feed_all(p, frame, n);

    uint8_t out[16] = {0};
    TEST_ASSERT_EQUAL_UINT16(0, p.copy_dmx_channels(out, sizeof(out)));
}

static void test_copy_dmx_zero_out_size_returns_zero(void) {
    DmxInputParser p;
    const uint8_t payload[] = {0x00, 0xAB};
    uint8_t frame[7];
    const size_t n = build_frame(frame, kLabelOutputOnlySendDmx, payload, 2);
    feed_all(p, frame, n);

    uint8_t out[1];
    TEST_ASSERT_EQUAL_UINT16(0, p.copy_dmx_channels(out, 0));
}

// ---------------------------------------------------------------------------
// reset() clears parse state but preserves counters and last_*; the
// dedicated reset_counters() clears the diagnostic counters.
// ---------------------------------------------------------------------------

static void test_reset_preserves_counters_and_last(void) {
    DmxInputParser p;
    const uint8_t payload[] = {0x00, 0xAA};
    uint8_t frame[7];
    const size_t n = build_frame(frame, kLabelOutputOnlySendDmx, payload, 2);
    feed_all(p, frame, n);

    TEST_ASSERT_EQUAL_UINT32(1, p.frame_count());
    TEST_ASSERT_EQUAL_UINT8(0xAA, p.last_payload()[1]);

    p.reset();
    TEST_ASSERT_EQUAL_UINT32(1, p.frame_count());          // preserved
    TEST_ASSERT_EQUAL_UINT8(0xAA, p.last_payload()[1]);    // preserved

    p.reset_counters();
    TEST_ASSERT_EQUAL_UINT32(0, p.frame_count());          // cleared now
}

// ---------------------------------------------------------------------------
// Unity main
// ---------------------------------------------------------------------------

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_minimal_valid_frame);
    RUN_TEST(test_junk_before_start_is_ignored);
    RUN_TEST(test_empty_payload);
    RUN_TEST(test_full_dmx_universe);
    RUN_TEST(test_bad_end_byte_rejects);
    RUN_TEST(test_resync_on_stray_start_byte);
    RUN_TEST(test_oversize_length_rejected);
    RUN_TEST(test_multiple_frames_back_to_back);
    RUN_TEST(test_framing_bytes_in_data_are_data);
    RUN_TEST(test_last_was_dmx_packet_true_for_label6_start0);
    RUN_TEST(test_last_was_dmx_packet_false_for_other_label);
    RUN_TEST(test_last_was_dmx_packet_false_for_nonzero_start_code);
    RUN_TEST(test_copy_dmx_truncates_to_out_size);
    RUN_TEST(test_copy_dmx_returns_zero_for_non_dmx_frame);
    RUN_TEST(test_copy_dmx_zero_out_size_returns_zero);
    RUN_TEST(test_reset_preserves_counters_and_last);
    return UNITY_END();
}
