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

inline void write_u32_le(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
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

inline uint32_t read_u32_le(const uint8_t* src) {
    return static_cast<uint32_t>(src[0]) |
           (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) |
           (static_cast<uint32_t>(src[3]) << 24);
}

// Serialise the fixed 8-byte header (spec v2). Forces magic + protocol_version
// and writes message_type / payload_len chosen by the caller (the per-type
// encoder).
void write_header(uint8_t* buf, const Header& hdr,
                  MessageType message_type, uint8_t payload_len) {
    buf[0] = kMagic0;
    buf[1] = kMagic1;
    buf[2] = kProtocolVersion;
    buf[3] = hdr.source_id;
    buf[4] = hdr.sequence_number;
    buf[5] = hdr.hop_count;
    buf[6] = static_cast<uint8_t>(message_type);
    buf[7] = payload_len;
}

bool is_known_message_type(uint8_t raw) {
    // Spec v0.29 §4.3 - two active message types plus the EXTENSION
    // slot. IDs 0x01, 0x02, 0x04, 0x05, 0x06 are reserved (do not
    // reuse) but are not recognised by current receivers; spec
    // forward-compatibility note says inbound frames with unknown
    // types should be silently discarded.
    switch (raw) {
        case static_cast<uint8_t>(MessageType::Heartbeat):
        case static_cast<uint8_t>(MessageType::LightCommand):
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

size_t encode_heartbeat(uint8_t* buf, size_t buf_len, const Header& hdr,
                        const HeartbeatPayload& p) {
    constexpr size_t total = kHeaderSize + kHeartbeatPayloadLen;
    if (buf_len < total) return 0;
    write_header(buf, hdr, MessageType::Heartbeat, kHeartbeatPayloadLen);
    write_u32_le(buf + kHeaderSize + 0, p.tick);
    write_u16_le(buf + kHeaderSize + 4, p.days_since_2026);
    write_u24_le(buf + kHeaderSize + 6, p.centiseconds_today);
    return total;
}

size_t encode_light_command(uint8_t* buf, size_t buf_len, const Header& hdr,
                            const LightCommandPayload& p) {
    constexpr size_t total = kHeaderSize + kLightCommandPayloadLen;
    if (buf_len < total) return 0;
    write_header(buf, hdr, MessageType::LightCommand, kLightCommandPayloadLen);
    buf[kHeaderSize + 0] = p.target_class;
    buf[kHeaderSize + 1] = p.target_group;
    buf[kHeaderSize + 2] = p.r;
    buf[kHeaderSize + 3] = p.g;
    buf[kHeaderSize + 4] = p.b;
    buf[kHeaderSize + 5] = p.attack;
    buf[kHeaderSize + 6] = p.sustain;
    buf[kHeaderSize + 7] = p.release;
    buf[kHeaderSize + 8] = p.chance;
    return total;
}

// =============================================================================
// Decoders
// =============================================================================

DecodeResult decode_header(const uint8_t* buf, size_t buf_len, Header& out_hdr) {
    if (buf_len < kHeaderSize) {
        return DecodeResult::BufferTooShort;
    }
    // Magic check first - cheapest rejection path for non-NocturNation
    // ESP-NOW chatter sharing the channel. Bytes 0-1 must be "NN".
    if (buf[0] != kMagic0 || buf[1] != kMagic1) {
        return DecodeResult::InvalidMagic;
    }
    if (buf[2] != kProtocolVersion) {
        return DecodeResult::InvalidProtocolVersion;
    }
    if (!is_known_message_type(buf[6])) {
        return DecodeResult::InvalidMessageType;
    }
    const uint8_t payload_len = buf[7];
    if (static_cast<size_t>(kHeaderSize) + payload_len > buf_len) {
        return DecodeResult::BufferTooShort;
    }

    out_hdr.protocol_version = buf[2];
    out_hdr.source_id        = buf[3];
    out_hdr.sequence_number  = buf[4];
    out_hdr.hop_count        = buf[5];
    out_hdr.message_type     = static_cast<MessageType>(buf[6]);
    out_hdr.payload_len      = payload_len;
    return DecodeResult::Ok;
}

DecodeResult decode_heartbeat(const Header& hdr,
                              const uint8_t* payload, size_t payload_len,
                              HeartbeatPayload& out) {
    if (hdr.message_type != MessageType::Heartbeat) {
        return DecodeResult::InvalidMessageType;
    }
    if (hdr.payload_len != kHeartbeatPayloadLen ||
        payload_len    != kHeartbeatPayloadLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    out.tick                = read_u32_le(payload + 0);
    out.days_since_2026     = read_u16_le(payload + 4);
    out.centiseconds_today  = read_u24_le(payload + 6);
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
    out.target_class = payload[0];
    out.target_group = payload[1];
    out.r            = payload[2];
    out.g            = payload[3];
    out.b            = payload[4];
    out.attack       = payload[5];
    out.sustain      = payload[6];
    out.release      = payload[7];
    out.chance       = payload[8];
    return DecodeResult::Ok;
}

}  // namespace espnow
}  // namespace transport
}  // namespace nocturnation
