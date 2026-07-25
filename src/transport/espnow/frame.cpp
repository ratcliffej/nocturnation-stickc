// NocturNation ESP-NOW frame encoder/decoder.
//
// See include/transport/espnow/frame.h for the wire-format reference and API
// contract. Pure logic; this translation unit must compile without any ESP32 /
// Arduino headers so the native test env can link it.

#include "transport/espnow/frame.h"

#include <cstring>

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

// Serialise the fixed 9-byte header (spec v3). Forces magic + protocol_version
// and writes message_type / payload_len chosen by the caller (the per-type
// encoder). v3 widens source_id to a 2-byte LE field at offset 3-4; every
// field after it shifted by 1.
void write_header(uint8_t* buf, const Header& hdr,
                  MessageType message_type, uint8_t payload_len) {
    buf[0] = kMagic0;
    buf[1] = kMagic1;
    buf[2] = kProtocolVersion;
    write_u16_le(buf + 3, hdr.source_id);
    buf[5] = hdr.sequence_number;
    buf[6] = hdr.hop_count;
    buf[7] = static_cast<uint8_t>(message_type);
    buf[8] = payload_len;
}

bool is_known_message_type(uint8_t raw) {
    // Spec v0.29 §4.3 + Epic 6C Phase D + Epic 13 additions. Heartbeat
    // + Light family + Display family + EXTENSION slot. IDs 0x01,
    // 0x02, 0x04, 0x05 remain reserved; spec forward-compatibility
    // note says inbound frames with unknown types should be silently
    // discarded.
    switch (raw) {
        case static_cast<uint8_t>(MessageType::Heartbeat):
        case static_cast<uint8_t>(MessageType::LightPulse):
        case static_cast<uint8_t>(MessageType::LightWash):
        case static_cast<uint8_t>(MessageType::LightWashEnd):
        case static_cast<uint8_t>(MessageType::LightWashPulse):
        case static_cast<uint8_t>(MessageType::TextDisplay):
        case static_cast<uint8_t>(MessageType::BitmapHeader):
        case static_cast<uint8_t>(MessageType::BitmapPlane):
        case static_cast<uint8_t>(MessageType::ClearScreen):
        case static_cast<uint8_t>(MessageType::RepeaterHeartbeat):
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

size_t encode_repeater_heartbeat(uint8_t* buf, size_t buf_len, const Header& hdr,
                                 const RepeaterHeartbeatPayload& p) {
    constexpr size_t total = kHeaderSize + kRepeaterHeartbeatPayloadLen;
    if (buf_len < total) return 0;
    write_header(buf, hdr, MessageType::RepeaterHeartbeat,
                 kRepeaterHeartbeatPayloadLen);
    buf[kHeaderSize + 0] = p.uid[0];
    buf[kHeaderSize + 1] = p.uid[1];
    buf[kHeaderSize + 2] = p.uid[2];
    buf[kHeaderSize + 3] = p.channel;
    write_u16_le(buf + kHeaderSize + 4, p.uptime_s);
    write_u32_le(buf + kHeaderSize + 6, p.relayed_count);
    return total;
}

size_t encode_light_pulse(uint8_t* buf, size_t buf_len, const Header& hdr,
                            const LightPulsePayload& p) {
    constexpr size_t total = kHeaderSize + kLightPulsePayloadLen;
    if (buf_len < total) return 0;
    write_header(buf, hdr, MessageType::LightPulse, kLightPulsePayloadLen);
    buf[kHeaderSize + 0] = p.target_class;
    write_u16_le(buf + kHeaderSize + 1, p.target_group);   // v3: 2 bytes
    buf[kHeaderSize + 3] = p.r;
    buf[kHeaderSize + 4] = p.g;
    buf[kHeaderSize + 5] = p.b;
    buf[kHeaderSize + 6] = p.attack;
    buf[kHeaderSize + 7] = p.sustain;
    buf[kHeaderSize + 8] = p.release;
    buf[kHeaderSize + 9] = p.chance;
    return total;
}

size_t encode_light_wash(uint8_t* buf, size_t buf_len, const Header& hdr,
                         const LightWashPayload& p) {
    constexpr size_t total = kHeaderSize + kLightWashPayloadLen;
    if (buf_len < total) return 0;
    write_header(buf, hdr, MessageType::LightWash, kLightWashPayloadLen);
    buf[kHeaderSize +  0] = p.target_class;
    write_u16_le(buf + kHeaderSize + 1, p.target_group);   // v3: 2 bytes
    buf[kHeaderSize +  3] = p.r1;
    buf[kHeaderSize +  4] = p.g1;
    buf[kHeaderSize +  5] = p.b1;
    buf[kHeaderSize +  6] = p.r2;
    buf[kHeaderSize +  7] = p.g2;
    buf[kHeaderSize +  8] = p.b2;
    buf[kHeaderSize +  9] = p.attack;
    buf[kHeaderSize + 10] = p.release;
    buf[kHeaderSize + 11] = p.intensity;
    write_u16_le(buf + kHeaderSize + 12, p.cycle_ms);
    write_u16_le(buf + kHeaderSize + 14, p.ttl_seconds);
    buf[kHeaderSize + 16] = p.pulse_response;
    return total;
}

size_t encode_light_wash_end(uint8_t* buf, size_t buf_len, const Header& hdr,
                             const LightWashEndPayload& p) {
    constexpr size_t total = kHeaderSize + kLightWashEndPayloadLen;
    if (buf_len < total) return 0;
    write_header(buf, hdr, MessageType::LightWashEnd, kLightWashEndPayloadLen);
    buf[kHeaderSize + 0] = p.target_class;
    write_u16_le(buf + kHeaderSize + 1, p.target_group);   // v3: 2 bytes
    buf[kHeaderSize + 3] = p.release_time;
    return total;
}

size_t encode_light_wash_pulse(uint8_t* buf, size_t buf_len, const Header& hdr,
                               const LightWashPulsePayload& p) {
    constexpr size_t total = kHeaderSize + kLightWashPulsePayloadLen;
    if (buf_len < total) return 0;
    write_header(buf, hdr, MessageType::LightWashPulse, kLightWashPulsePayloadLen);
    buf[kHeaderSize + 0] = p.target_class;
    write_u16_le(buf + kHeaderSize + 1, p.target_group);   // v3: 2 bytes
    buf[kHeaderSize + 3] = p.r;
    buf[kHeaderSize + 4] = p.g;
    buf[kHeaderSize + 5] = p.b;
    buf[kHeaderSize + 6] = p.attack;
    buf[kHeaderSize + 7] = p.sustain;
    buf[kHeaderSize + 8] = p.release;
    buf[kHeaderSize + 9] = p.chance;
    return total;
}

// Epic 13 encoders. The variable-length encoders (TEXT_DISPLAY,
// BITMAP_PLANE) compute their payload_len from the in-struct length
// fields and write only the valid byte ranges.

size_t encode_text_display(uint8_t* buf, size_t buf_len, const Header& hdr,
                           const TextDisplayPayload& p) {
    if (p.header_len > kTextDisplayMaxHeaderLen) return 0;
    if (p.body_len   > kTextDisplayMaxBodyLen)   return 0;
    const size_t payload_len = kTextDisplayMinPayloadLen
                              + p.header_len + p.body_len;
    if (payload_len > kMaxPayloadSize) return 0;
    const size_t total = kHeaderSize + payload_len;
    if (buf_len < total) return 0;
    write_header(buf, hdr, MessageType::TextDisplay,
                 static_cast<uint8_t>(payload_len));
    size_t o = kHeaderSize;
    write_u16_le(buf + o, p.target_group); o += 2;   // v3: 2 bytes
    buf[o++] = p.r;
    buf[o++] = p.g;
    buf[o++] = p.b;
    write_u16_le(buf + o, p.ttl_ms); o += 2;
    buf[o++] = p.header_len;
    if (p.header_len) std::memcpy(buf + o, p.header, p.header_len);
    o += p.header_len;
    buf[o++] = p.body_len;
    if (p.body_len) std::memcpy(buf + o, p.body, p.body_len);
    o += p.body_len;
    return total;
}

size_t encode_bitmap_header(uint8_t* buf, size_t buf_len, const Header& hdr,
                            const BitmapHeaderPayload& p) {
    if (p.plane_count == 0 || p.plane_count > kBitmapMaxPlanes) return 0;
    if (p.width  == 0 || p.width  > kBitmapMaxDimension) return 0;
    if (p.height == 0 || p.height > kBitmapMaxDimension) return 0;
    constexpr size_t total = kHeaderSize + kBitmapHeaderPayloadLen;
    if (buf_len < total) return 0;
    write_header(buf, hdr, MessageType::BitmapHeader, kBitmapHeaderPayloadLen);
    size_t o = kHeaderSize;
    write_u16_le(buf + o, p.target_group); o += 2;   // v3: 2 bytes
    buf[o++] = p.width;
    buf[o++] = p.height;
    buf[o++] = p.plane_count;
    for (size_t i = 0; i < kBitmapMaxPlanes; ++i) {
        buf[o++] = p.colours[i][0];
        buf[o++] = p.colours[i][1];
        buf[o++] = p.colours[i][2];
    }
    buf[o++] = p.fit;
    buf[o++] = p.zoom_pct;
    buf[o++] = p.overwrite;
    write_u32_le(buf + o, p.checksum); o += 4;
    write_u16_le(buf + o, p.ttl_ms);   o += 2;
    return total;
}

size_t encode_bitmap_plane(uint8_t* buf, size_t buf_len, const Header& hdr,
                           const BitmapPlanePayload& p) {
    if (p.data_len > kBitmapPlaneMaxDataLen) return 0;
    const size_t payload_len = kBitmapPlaneFixedPrefixLen + p.data_len;
    if (payload_len > kMaxPayloadSize) return 0;
    const size_t total = kHeaderSize + payload_len;
    if (buf_len < total) return 0;
    write_header(buf, hdr, MessageType::BitmapPlane,
                 static_cast<uint8_t>(payload_len));
    size_t o = kHeaderSize;
    write_u16_le(buf + o, p.target_group); o += 2;   // v3: 2 bytes
    buf[o++] = p.plane_index;
    write_u16_le(buf + o, p.byte_offset); o += 2;
    buf[o++] = p.data_len;
    if (p.data_len) std::memcpy(buf + o, p.data, p.data_len);
    o += p.data_len;
    return total;
}

size_t encode_clear_screen(uint8_t* buf, size_t buf_len, const Header& hdr,
                           const ClearScreenPayload& p) {
    constexpr size_t total = kHeaderSize + kClearScreenPayloadLen;
    if (buf_len < total) return 0;
    write_header(buf, hdr, MessageType::ClearScreen, kClearScreenPayloadLen);
    write_u16_le(buf + kHeaderSize + 0, p.target_group);   // v3: 2 bytes
    buf[kHeaderSize + 2] = p.clear_text ? 1 : 0;
    buf[kHeaderSize + 3] = p.clear_bitmap ? 1 : 0;
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
    if (!is_known_message_type(buf[7])) {
        return DecodeResult::InvalidMessageType;
    }
    const uint8_t payload_len = buf[8];
    if (static_cast<size_t>(kHeaderSize) + payload_len > buf_len) {
        return DecodeResult::BufferTooShort;
    }

    out_hdr.protocol_version = buf[2];
    out_hdr.source_id        = read_u16_le(buf + 3);
    out_hdr.sequence_number  = buf[5];
    out_hdr.hop_count        = buf[6];
    out_hdr.message_type     = static_cast<MessageType>(buf[7]);
    out_hdr.payload_len      = payload_len;
    return DecodeResult::Ok;
}

void set_hop_count(uint8_t* buf, size_t buf_len, uint8_t new_hops) {
    if (buf == nullptr || buf_len < kHeaderSize) return;
    buf[kHopCountOffset] = new_hops;
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

DecodeResult decode_repeater_heartbeat(const Header& hdr,
                                       const uint8_t* payload, size_t payload_len,
                                       RepeaterHeartbeatPayload& out) {
    if (hdr.message_type != MessageType::RepeaterHeartbeat) {
        return DecodeResult::InvalidMessageType;
    }
    if (hdr.payload_len != kRepeaterHeartbeatPayloadLen ||
        payload_len    != kRepeaterHeartbeatPayloadLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    out.uid[0]        = payload[0];
    out.uid[1]        = payload[1];
    out.uid[2]        = payload[2];
    out.channel       = payload[3];
    out.uptime_s      = read_u16_le(payload + 4);
    out.relayed_count = read_u32_le(payload + 6);
    return DecodeResult::Ok;
}

DecodeResult decode_light_pulse(const Header& hdr,
                                  const uint8_t* payload, size_t payload_len,
                                  LightPulsePayload& out) {
    if (hdr.message_type != MessageType::LightPulse) {
        return DecodeResult::InvalidMessageType;
    }
    if (hdr.payload_len != kLightPulsePayloadLen ||
        payload_len    != kLightPulsePayloadLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    out.target_class = payload[0];
    out.target_group = read_u16_le(payload + 1);   // v3: 2 bytes
    out.r            = payload[3];
    out.g            = payload[4];
    out.b            = payload[5];
    out.attack       = payload[6];
    out.sustain      = payload[7];
    out.release      = payload[8];
    out.chance       = payload[9];
    return DecodeResult::Ok;
}

DecodeResult decode_light_wash(const Header& hdr,
                               const uint8_t* payload, size_t payload_len,
                               LightWashPayload& out) {
    if (hdr.message_type != MessageType::LightWash) {
        return DecodeResult::InvalidMessageType;
    }
    if (hdr.payload_len != kLightWashPayloadLen ||
        payload_len    != kLightWashPayloadLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    out.target_class   = payload[ 0];
    out.target_group   = read_u16_le(payload + 1);   // v3: 2 bytes
    out.r1             = payload[ 3];
    out.g1             = payload[ 4];
    out.b1             = payload[ 5];
    out.r2             = payload[ 6];
    out.g2             = payload[ 7];
    out.b2             = payload[ 8];
    out.attack         = payload[ 9];
    out.release        = payload[10];
    out.intensity      = payload[11];
    out.cycle_ms       = read_u16_le(payload + 12);
    out.ttl_seconds    = read_u16_le(payload + 14);
    out.pulse_response = payload[16];
    return DecodeResult::Ok;
}

DecodeResult decode_light_wash_end(const Header& hdr,
                                   const uint8_t* payload, size_t payload_len,
                                   LightWashEndPayload& out) {
    if (hdr.message_type != MessageType::LightWashEnd) {
        return DecodeResult::InvalidMessageType;
    }
    if (hdr.payload_len != kLightWashEndPayloadLen ||
        payload_len    != kLightWashEndPayloadLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    out.target_class = payload[0];
    out.target_group = read_u16_le(payload + 1);   // v3: 2 bytes
    out.release_time = payload[3];
    return DecodeResult::Ok;
}

DecodeResult decode_light_wash_pulse(const Header& hdr,
                                     const uint8_t* payload, size_t payload_len,
                                     LightWashPulsePayload& out) {
    if (hdr.message_type != MessageType::LightWashPulse) {
        return DecodeResult::InvalidMessageType;
    }
    if (hdr.payload_len != kLightWashPulsePayloadLen ||
        payload_len    != kLightWashPulsePayloadLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    out.target_class = payload[0];
    out.target_group = read_u16_le(payload + 1);   // v3: 2 bytes
    out.r            = payload[3];
    out.g            = payload[4];
    out.b            = payload[5];
    out.attack       = payload[6];
    out.sustain      = payload[7];
    out.release      = payload[8];
    out.chance       = payload[9];
    return DecodeResult::Ok;
}

// Epic 13 decoders. Variable-length decoders validate the wire's
// length fields against payload_len step by step, populate the
// struct's *_len fields with the decoded values, and copy the valid
// bytes (without padding) into the in-struct buffers.

DecodeResult decode_text_display(const Header& hdr,
                                 const uint8_t* payload, size_t payload_len,
                                 TextDisplayPayload& out) {
    if (hdr.message_type != MessageType::TextDisplay) {
        return DecodeResult::InvalidMessageType;
    }
    if (hdr.payload_len != payload_len) {
        return DecodeResult::PayloadLenMismatch;
    }
    // Minimum is "both strings empty" (kTextDisplayMinPayloadLen = 9 in v3).
    if (payload_len < kTextDisplayMinPayloadLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    out.target_group = read_u16_le(payload + 0);   // v3: 2 bytes
    out.r            = payload[2];
    out.g            = payload[3];
    out.b            = payload[4];
    out.ttl_ms       = read_u16_le(payload + 5);

    const uint8_t header_len = payload[7];
    if (header_len > kTextDisplayMaxHeaderLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    // After header_len we need room for header_bytes + body_len byte.
    const size_t body_len_offset = 8 + header_len;
    if (payload_len < body_len_offset + 1) {
        return DecodeResult::PayloadLenMismatch;
    }
    out.header_len = header_len;
    if (header_len) std::memcpy(out.header, payload + 8, header_len);

    const uint8_t body_len = payload[body_len_offset];
    if (body_len > kTextDisplayMaxBodyLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    const size_t body_start = body_len_offset + 1;
    if (payload_len != body_start + body_len) {
        return DecodeResult::PayloadLenMismatch;
    }
    out.body_len = body_len;
    if (body_len) std::memcpy(out.body, payload + body_start, body_len);
    return DecodeResult::Ok;
}

DecodeResult decode_bitmap_header(const Header& hdr,
                                  const uint8_t* payload, size_t payload_len,
                                  BitmapHeaderPayload& out) {
    if (hdr.message_type != MessageType::BitmapHeader) {
        return DecodeResult::InvalidMessageType;
    }
    if (hdr.payload_len != kBitmapHeaderPayloadLen ||
        payload_len    != kBitmapHeaderPayloadLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    size_t o = 0;
    out.target_group = read_u16_le(payload + o); o += 2;   // v3: 2 bytes
    out.width        = payload[o++];
    out.height       = payload[o++];
    out.plane_count  = payload[o++];
    if (out.plane_count == 0 || out.plane_count > kBitmapMaxPlanes) {
        return DecodeResult::PayloadLenMismatch;
    }
    if (out.width == 0 || out.width > kBitmapMaxDimension ||
        out.height == 0 || out.height > kBitmapMaxDimension) {
        return DecodeResult::PayloadLenMismatch;
    }
    for (size_t i = 0; i < kBitmapMaxPlanes; ++i) {
        out.colours[i][0] = payload[o++];
        out.colours[i][1] = payload[o++];
        out.colours[i][2] = payload[o++];
    }
    out.fit       = payload[o++];
    out.zoom_pct  = payload[o++];
    out.overwrite = payload[o++];
    out.checksum  = read_u32_le(payload + o); o += 4;
    out.ttl_ms    = read_u16_le(payload + o); o += 2;
    return DecodeResult::Ok;
}

DecodeResult decode_bitmap_plane(const Header& hdr,
                                 const uint8_t* payload, size_t payload_len,
                                 BitmapPlanePayload& out) {
    if (hdr.message_type != MessageType::BitmapPlane) {
        return DecodeResult::InvalidMessageType;
    }
    if (hdr.payload_len != payload_len) {
        return DecodeResult::PayloadLenMismatch;
    }
    if (payload_len < kBitmapPlaneMinPayloadLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    out.target_group = read_u16_le(payload + 0);   // v3: 2 bytes
    out.plane_index  = payload[2];
    out.byte_offset  = read_u16_le(payload + 3);
    const uint8_t data_len = payload[5];
    if (data_len > kBitmapPlaneMaxDataLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    if (payload_len != static_cast<size_t>(kBitmapPlaneFixedPrefixLen) + data_len) {
        return DecodeResult::PayloadLenMismatch;
    }
    out.data_len = data_len;
    if (data_len) std::memcpy(out.data, payload + 6, data_len);
    return DecodeResult::Ok;
}

DecodeResult decode_clear_screen(const Header& hdr,
                                 const uint8_t* payload, size_t payload_len,
                                 ClearScreenPayload& out) {
    if (hdr.message_type != MessageType::ClearScreen) {
        return DecodeResult::InvalidMessageType;
    }
    if (hdr.payload_len != kClearScreenPayloadLen ||
        payload_len    != kClearScreenPayloadLen) {
        return DecodeResult::PayloadLenMismatch;
    }
    out.target_group = read_u16_le(payload + 0);   // v3: 2 bytes
    out.clear_text   = payload[2];
    out.clear_bitmap = payload[3];
    return DecodeResult::Ok;
}

}  // namespace espnow
}  // namespace transport
}  // namespace nocturnation
