// DmxInputParser implementation - Epic 7 B1.

#include "dmx_input_parser.h"

namespace nocturnation {
namespace dal {

DmxInputParser::DmxInputParser() {
    // Defaults are already correct via in-class initialisers; an
    // explicit ctor is here so callers can construct without including
    // anything else.
}

void DmxInputParser::reset() {
    state_         = State::WaitStart;
    current_label_ = 0;
    current_len_   = 0;
    data_read_     = 0;
    // last_* preserved so callers can still read the most-recent frame
    // through a reset.
}

void DmxInputParser::reset_counters() {
    frame_count_    = 0;
    reset_count_    = 0;
    oversize_count_ = 0;
}

DmxParseResult DmxInputParser::reset_to_wait_start(uint8_t rejecting_byte) {
    ++reset_count_;
    if (rejecting_byte == enttec_pro::kStartByte) {
        // The byte that broke us was itself a valid start byte. Re-anchor
        // immediately on it so we don't lose the frame it begins.
        state_         = State::ReadLabel;
    } else {
        state_         = State::WaitStart;
    }
    current_label_ = 0;
    current_len_   = 0;
    data_read_     = 0;
    return DmxParseResult::Reset;
}

DmxParseResult DmxInputParser::feed_byte(uint8_t b) {
    switch (state_) {
        case State::WaitStart:
            if (b == enttec_pro::kStartByte) {
                state_ = State::ReadLabel;
            }
            // Otherwise silently drop junk between frames.
            return DmxParseResult::NeedMoreBytes;

        case State::ReadLabel:
            current_label_ = b;
            state_ = State::ReadLenLo;
            return DmxParseResult::NeedMoreBytes;

        case State::ReadLenLo:
            current_len_ = b;
            state_ = State::ReadLenHi;
            return DmxParseResult::NeedMoreBytes;

        case State::ReadLenHi: {
            current_len_ |= (static_cast<uint16_t>(b) << 8);
            if (current_len_ > kPayloadBufSize) {
                // Refuse to consume payloads we couldn't store. Bump
                // the dedicated counter so diagnostics distinguish
                // oversize from other resync events.
                ++oversize_count_;
                // Drop straight to WaitStart - we'll resync on the next
                // 0x7E. The reset_to_wait_start helper bumps reset_count_
                // too, but oversize is the more specific signal so we
                // double-count deliberately for visibility.
                return reset_to_wait_start(/*rejecting_byte=*/0);
            }
            data_read_ = 0;
            state_ = (current_len_ == 0) ? State::ReadEnd : State::ReadData;
            return DmxParseResult::NeedMoreBytes;
        }

        case State::ReadData:
            payload_buf_[data_read_++] = b;
            if (data_read_ >= current_len_) {
                state_ = State::ReadEnd;
            }
            return DmxParseResult::NeedMoreBytes;

        case State::ReadEnd:
            if (b == enttec_pro::kEndByte) {
                // Valid frame. Commit to last_*.
                last_label_       = current_label_;
                last_payload_len_ = current_len_;
                ++frame_count_;
                state_         = State::WaitStart;
                current_label_ = 0;
                current_len_   = 0;
                data_read_     = 0;
                return DmxParseResult::FrameComplete;
            }
            // Wrong end byte. Resync, treating this byte as a possible
            // new start.
            return reset_to_wait_start(b);
    }
    // Unreachable.
    return DmxParseResult::NeedMoreBytes;
}

bool DmxInputParser::last_was_dmx_packet() const {
    return last_label_ == enttec_pro::kLabelOutputOnlySendDmx
        && last_payload_len_ >= 1
        && payload_buf_[0] == 0x00;
}

uint16_t DmxInputParser::copy_dmx_channels(uint8_t* out, uint16_t out_size) const {
    if (!last_was_dmx_packet() || out == nullptr || out_size == 0) {
        return 0;
    }
    // payload[0] is the start code; channels start at payload[1].
    const uint16_t channels_in_frame =
        static_cast<uint16_t>(last_payload_len_ - 1);
    const uint16_t to_copy =
        (channels_in_frame < out_size) ? channels_in_frame : out_size;
    for (uint16_t i = 0; i < to_copy; ++i) {
        out[i] = payload_buf_[1 + i];
    }
    return to_copy;
}

}  // namespace dal
}  // namespace nocturnation
