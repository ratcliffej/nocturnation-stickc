// DmxInputParser - pure state machine that parses Enttec DMX USB Pro
// framing one byte at a time.
//
// Epic 7 B1. Consumes bytes from a USB-CDC stream and emits "frame
// complete" events when a well-formed Enttec Pro frame arrives. The
// parser holds no Arduino / hardware dependency so it host-tests
// directly against captured byte sequences; the Arduino USB-CDC
// adapter that pumps bytes into it is a separate layer (Epic 7 B1
// follow-up commit / B3 DmxBridge mode wiring).
//
// Frame format (Enttec API spec section 4.2):
//
//   start_byte (0x7E)
//   label      (1 byte; QLC+ uses 0x06 = Output Only Send DMX Packet)
//   length_lo  (1 byte)
//   length_hi  (1 byte; little-endian length, 0..600)
//   data       (length bytes)
//   end_byte   (0xE7)
//
// For a standard DMX-512 packet the data segment is 513 bytes: 1 start
// code (usually 0x00) followed by 512 channel values. Other Enttec
// messages (Get Widget Serial, etc.) use shorter payloads with
// different labels; the parser accepts them all and labels them via
// last_label(), leaving label-specific handling to the caller.
//
// State machine survives corruption: an unexpected byte in any state
// reverts to WaitStart so the next 0x7E re-anchors. The length field
// is authoritative for the data segment (so 0x7E / 0xE7 inside data
// are valid DMX values, not escape characters).

#pragma once

#include <cstddef>
#include <cstdint>

namespace nocturnation {
namespace dal {

namespace enttec_pro {
// Framing delimiters.
constexpr uint8_t kStartByte = 0x7E;
constexpr uint8_t kEndByte   = 0xE7;

// Labels we recognise (full label table is wider; we only special-case
// the ones the bridge actually does anything with).
constexpr uint8_t kLabelOutputOnlySendDmx     = 0x06;   // QLC+'s send-DMX
constexpr uint8_t kLabelGetWidgetSerialReq    = 0x0A;   // handshake request
constexpr uint8_t kLabelGetWidgetSerialReply  = 0x0A;   // (same label is reused for reply)

// Standard DMX-512 universe channel count.
constexpr uint16_t kDmxUniverseChannels = 512;
}  // namespace enttec_pro

// Result of one feed_byte() call.
enum class DmxParseResult : uint8_t {
    NeedMoreBytes = 0,   // mid-frame; keep feeding bytes
    FrameComplete = 1,   // valid frame just completed; read via last_*()
    Reset         = 2,   // unexpected byte; parser reverted to WaitStart
};

class DmxInputParser {
public:
    DmxInputParser();

    // Reset to initial state. Counters are preserved (so diagnostic code
    // can see total resets across a session); call reset_counters() if
    // you also want those cleared.
    void reset();
    void reset_counters();

    // Feed one byte into the parser. Returns NeedMoreBytes /
    // FrameComplete / Reset. After FrameComplete, the last_* accessors
    // describe the completed frame; on the next feed_byte() the parser
    // starts looking for the next start byte.
    DmxParseResult feed_byte(uint8_t b);

    // Description of the most-recently-completed frame. Undefined if
    // feed_byte() has never returned FrameComplete in this session.
    uint8_t  last_label()       const { return last_label_; }
    uint16_t last_payload_len() const { return last_payload_len_; }
    const uint8_t* last_payload() const { return payload_buf_; }

    // Convenience: was the last completed frame an "Output Only Send
    // DMX Packet" with start code 0? (QLC+'s default. Returns true iff
    // label == 0x06, payload non-empty, and payload[0] == 0x00.)
    bool last_was_dmx_packet() const;

    // Copy DMX channel values out of the last completed frame.
    //
    // Iff last_was_dmx_packet() is true, copies channels starting at
    // channel 1 (payload[1]) into out[0..n-1]. n is min(out_size,
    // channels_in_frame). Returns the number of bytes copied. Returns
    // 0 if the last frame wasn't a DMX packet.
    //
    // Channels not covered by the frame's payload are NOT touched in
    // out - the caller should zero or initialise the buffer between
    // frames if they want absent channels to read as 0.
    uint16_t copy_dmx_channels(uint8_t* out, uint16_t out_size) const;

    // Diagnostics + tests.
    uint32_t frame_count()      const { return frame_count_; }
    uint32_t reset_count()      const { return reset_count_; }
    uint32_t oversize_count()   const { return oversize_count_; }

    // Payload buffer ceiling. 600 covers Enttec API spec max plus
    // headroom; chosen rather than a tight 513 so the parser doesn't
    // reject any standards-compliant Enttec message type.
    static constexpr size_t kPayloadBufSize = 600;

private:
    enum class State : uint8_t {
        WaitStart  = 0,   // looking for 0x7E
        ReadLabel  = 1,   // got start; reading label
        ReadLenLo  = 2,   // reading length LSB
        ReadLenHi  = 3,   // reading length MSB
        ReadData   = 4,   // reading <current_len_> payload bytes
        ReadEnd    = 5,   // payload complete; expecting 0xE7
    };

    State    state_           = State::WaitStart;
    uint8_t  current_label_   = 0;
    uint16_t current_len_     = 0;
    uint16_t data_read_       = 0;

    uint8_t  last_label_         = 0;
    uint16_t last_payload_len_   = 0;
    uint8_t  payload_buf_[kPayloadBufSize] = {0};

    uint32_t frame_count_     = 0;
    uint32_t reset_count_     = 0;
    uint32_t oversize_count_  = 0;

    // Helper: any unexpected byte reverts to WaitStart. If the rejecting
    // byte itself is 0x7E we treat it as a re-anchor (skip WaitStart,
    // go straight to ReadLabel) so a mid-frame corruption recovers
    // immediately on the next valid frame. Returns Reset.
    DmxParseResult reset_to_wait_start(uint8_t rejecting_byte);
};

}  // namespace dal
}  // namespace nocturnation
