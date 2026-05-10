// NocturNation ESP-NOW frame encoder/decoder.
//
// See include/transport/espnow/frame.h for the wire-format reference and API
// contract. Pure logic; this translation unit must compile without any ESP32 /
// Arduino headers so the native test env can link it.

#include "transport/espnow/frame.h"

namespace nocturnation {
namespace transport {
namespace espnow {

namespace {

// Little-endian helpers. ESP32 is natively little-endian and the spec
// (TIME_SYNC) explicitly specifies LE; we apply it consistently.

inline void write_u16_le(uint8_t* dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

inline void write_u24_le(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
}

inline uint16_t read_u16_le(const uint8_t* src) {
    return static_cast<uint16_t>(src[0]) |
           (static_cast<uint16_t>(src[1]) << 8);
}

inline uint32_t read_u24_le(const uint8_t* src) {
    return static_cast<uint32_t>(src[0]) |
           (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16);
}

// Serialise the fixed 6-byte header. Forces protocol_version and writes
// message_type / payload_len chosen by the caller (the per-type encoder).
void write_header(uint8_t* buf, const Header& hdr,
                  MessageType message_type, uint8_t payload_len) {
    buf[0] = kProtocolVersion;
    buf[1] = hdr.source_id;
    buf[2] = hdr.sequence_number;
    buf[3] = hdr.hop_count;
    buf[4] = static_cast<uint8_t>(message_type);
    buf[5] = payload_len;
}

bool is_known_message_type(uint8_t raw) {
    switch (raw) {
        case static_cast<uint8_t>(MessageType::Heartbeat):
        case static_cast<uint8_t>(MessageType::BeatDetected):
        case static_cast<uint8_t>(MessageType::ModeChange):
        case static_cast<uint8_t>(MessageType::LightCommand):
        case static_cast<uint8_t>(MessageType::ClockSync):
        case static_cast<uint8_t>(MessageType::TimeSync):
        case static_cast<uint8_t>(MessageType::MusicEvent):
        case static_cast<uint8_t>(MessageType::Extension):
            return true;
        default:
            return false;
    }
}

}  // namespace

// =============================================================================
// Encoders
// =============================================================================

size_t encode_heartbeat(uint8_t* buf, size_t buf_len, const Header& hdr) {
    constexpr size_t total = kHeaderSize + kHeartbeatPayloadLen;
    if (buf_len < total) return 0;
    write_header(buf, hdr, MessageType::Heartbeat, kHeartbeatPayloadLen);
    return total;
}

size_t encode_beat_detected(uint8_t* buf, size_t buf_len, const Header& hdr,
                            const BeatDetectedPayload& p) {
    constexpr size_t total = kHeaderSize + kBeatDetectedPayloadLen;
    if (buf_len < total) return 0;
    write_header(buf, hdr, MessageType::BeatDetected, kBeatDetectedPayloadLen);
    buf[kHeaderSize + 0] = p.strength;
    write_u16_le(buf + kHeaderSize + 1, p.bpm_x10);
    return total;
}

size_t encode_mode_change(uint8_t* buf, size_t buf_len, const Header& hdr,
                          const ModeChangePayload& p) {
    constexpr size_t total = kHeaderSize + kModeChangePayloadLen;
    if (buf_len < total) return 0;
    write_header(buf, hdr, MessageType::ModeChange, kModeChangePayloadLen);
    buf[kHeaderSize + 0] = p.new_mode;
    buf[kHeaderSize + 1] = p.palette_id;
    return total;
}

size_t encode_light_command(uint8_t* buf, size_t buf_len, const Header& hdr,
                            const LightCommandPayload& p) {
    constexpr size_t total = kHeaderSize + kLightCommandPayloadLen;
    if (buf_len < total) return 0;
    write_header(buf, hdr, MessageType::LightCommand, kLightCommandPayloadLen);
    buf[kHeaderSize + 0] = p.target_group;
    buf[kHeaderSize + 1] = p.r;
    buf[kHeaderSize + 2] = p.g;
    buf[kHeaderSize + 3] = p.b;
    buf[kHeaderSize + 4] = p.attack;
    buf[kHeaderSize + 5] = p.sustain;
    buf[kHeaderSize + 6] = p.release;
    buf[kHeaderSize + 7] = p.chance;
    return total;
}

size_t encode_clock_sync(uint8_t* buf, size_t buf_len, const Header& hdr,
                         const ClockSyncPayload& p) {
    constexpr size_t total = kHeaderSize + kClockSyncPayloadLen;
    if (buf_len < total) return 0;
    write_header(buf, hdr, MessageType::ClockSync, kClockSyncPayloadLen);
    write_u16_le(buf + kHeaderSize + 0, p.phase_in_bar);
    write_u16_le(buf + kHeaderSize + 2, p.bpm_x10);
    return total;
}

size_t encode_time_sync(uint8_t* buf, size_t buf_len, const Header& hdr,
                        const TimeSyncPayload& p) {
    constexpr size_t total = kHeaderSize + kTimeSyncPayloadLen;
    if (buf_len < total) return 0;
    write_header(buf, hdr, MessageType::TimeSync, kTimeSyncPayloadLen);
    write_u16_le(buf + kHeaderSize + 0, p.days_since_2026);
    write_u24_le(buf + kHeaderSize + 2, p.centiseconds_today);
    return total;
}

size_t encode_music_event(uint8_t* buf, size_t buf_len, const Header& hdr,
                          const MusicEventPayload& p) {
    constexpr size_t total = kHeaderSize + kMusicEventPayloadLen;
    if (buf_len < total) return 0;
    write_header(buf, hdr, MessageType::MusicEvent, kMusicEventPayloadLen);
    buf[kHeaderSize + 0] = static_cast<uint8_t>(p.event_type);
    return total;
}

// =============================================================================
// Decoders
// =============================================================================

DecodeResult decode_header(const uint8_t* buf, size_t buf_len, Header& out_hdr) {
    if (buf_len < kHeaderSize) {
        return DecodeResult::BufferTooShort;
    }
    if (buf[0] != kProtocolVersion) {
        return DecodeResult::InvalidProtocolVersion;
    }
    if (!is_known_message_type(buf[4])) {
        return DecodeResult::InvalidMessageType;
    }
    const uint8_t payload_len = buf[5];
    if (static_cast<size_t>(kHeaderSize) + payload_len > buf_len) {
        return DecodeResult::BufferTooShort;
    }

    out_hdr.protocol_version = buf[0];
    out_hdr.source_id        = buf[1];
    out_hdr.sequence_number  = buf[2];
    out_hdr.hop_count        = buf[3];
    out_hdr.message_type     = static_cast<MessageType>(buf[4]);
    out_hdr.payload_len      = payload_len;
    return DecodeResult::Ok;
}

DecodeResult decode_heartbeat(const Header& hdr,
                              const uint8_t* /*payload*/, size_t payload_len) {
    if (hdr.message_type != MessageType::Heartbeat) {
        return DecodeResult::InvalidMessageType;
    }
    if (hdr.payload_len != kHeartbeatPayloadLen ||
        payload_len    != kHeartbeatPayloadLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    return DecodeResult::Ok;
}

DecodeResult decode_beat_detected(const Header& hdr,
                                  const uint8_t* payload, size_t payload_len,
                                  BeatDetectedPayload& out) {
    if (hdr.message_type != MessageType::BeatDetected) {
        return DecodeResult::InvalidMessageType;
    }
    if (hdr.payload_len != kBeatDetectedPayloadLen ||
        payload_len    != kBeatDetectedPayloadLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    out.strength = payload[0];
    out.bpm_x10  = read_u16_le(payload + 1);
    return DecodeResult::Ok;
}

DecodeResult decode_mode_change(const Header& hdr,
                                const uint8_t* payload, size_t payload_len,
                                ModeChangePayload& out) {
    if (hdr.message_type != MessageType::ModeChange) {
        return DecodeResult::InvalidMessageType;
    }
    if (hdr.payload_len != kModeChangePayloadLen ||
        payload_len    != kModeChangePayloadLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    out.new_mode   = payload[0];
    out.palette_id = payload[1];
    return DecodeResult::Ok;
}

DecodeResult decode_light_command(const Header& hdr,
                                  const uint8_t* payload, size_t payload_len,
                                  LightCommandPayload& out) {
    if (hdr.message_type != MessageType::LightCommand) {
        return DecodeResult::InvalidMessageType;
    }
    if (hdr.payload_len != kLightCommandPayloadLen ||
        payload_len    != kLightCommandPayloadLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    out.target_group = payload[0];
    out.r            = payload[1];
    out.g            = payload[2];
    out.b            = payload[3];
    out.attack       = payload[4];
    out.sustain      = payload[5];
    out.release      = payload[6];
    out.chance       = payload[7];
    return DecodeResult::Ok;
}

DecodeResult decode_clock_sync(const Header& hdr,
                               const uint8_t* payload, size_t payload_len,
                               ClockSyncPayload& out) {
    if (hdr.message_type != MessageType::ClockSync) {
        return DecodeResult::InvalidMessageType;
    }
    if (hdr.payload_len != kClockSyncPayloadLen ||
        payload_len    != kClockSyncPayloadLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    out.phase_in_bar = read_u16_le(payload + 0);
    out.bpm_x10      = read_u16_le(payload + 2);
    return DecodeResult::Ok;
}

DecodeResult decode_time_sync(const Header& hdr,
                              const uint8_t* payload, size_t payload_len,
                              TimeSyncPayload& out) {
    if (hdr.message_type != MessageType::TimeSync) {
        return DecodeResult::InvalidMessageType;
    }
    if (hdr.payload_len != kTimeSyncPayloadLen ||
        payload_len    != kTimeSyncPayloadLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    out.days_since_2026     = read_u16_le(payload + 0);
    out.centiseconds_today  = read_u24_le(payload + 2);
    return DecodeResult::Ok;
}

DecodeResult decode_music_event(const Header& hdr,
                                const uint8_t* payload, size_t payload_len,
                                MusicEventPayload& out) {
    if (hdr.message_type != MessageType::MusicEvent) {
        return DecodeResult::InvalidMessageType;
    }
    if (hdr.payload_len != kMusicEventPayloadLen ||
        payload_len    != kMusicEventPayloadLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    // Map raw byte to enum. Unknown raw values land as Unknown so
    // receivers can drop the frame without misinterpreting future-
    // protocol additions (forward-compatible per spec §4.3).
    const uint8_t raw = payload[0];
    switch (raw) {
        case static_cast<uint8_t>(MusicEventType::Drop):
            out.event_type = MusicEventType::Drop; break;
        case static_cast<uint8_t>(MusicEventType::Breakdown):
            out.event_type = MusicEventType::Breakdown; break;
        case static_cast<uint8_t>(MusicEventType::Build):
            out.event_type = MusicEventType::Build; break;
        default:
            out.event_type = MusicEventType::Unknown; break;
    }
    return DecodeResult::Ok;
}

}  // namespace espnow
}  // namespace transport
}  // namespace nocturnation
