// NocturNation ESP-NOW frame format (v1)
//
// Wire format per docs/architecture.md §4.3. Pure logic; no ESP32 / radio
// dependencies, so this layer compiles and tests on native (laptop).
//
// The radio transport (Block 3+ of Epic 4) consumes these encoders to fill
// ESP-NOW packets, and feeds received bytes through the decoders.
//
// Frame layout:
//   Offset  Field             Size  Notes
//   0       protocol_version  1     0x01 for v1
//   1       source_id         1     1-254; 0xFF = broadcast
//   2       sequence_number   1     1-255 wraps; 0 = sequencing disabled
//   3       hop_count         1     0 = original; cap at kMaxHopCount
//   4       message_type      1     See MessageType enum
//   5       payload_len       1     Bytes of payload following header
//   6+      payload           N     Type-specific (little-endian)

#pragma once

#include <cstddef>
#include <cstdint>

namespace nocturnation {
namespace transport {
namespace espnow {

// Header constants (spec §4.3)
constexpr uint8_t kProtocolVersion   = 0x01;
constexpr uint8_t kBroadcastSourceId = 0xFF;
constexpr uint8_t kHeaderSize        = 6;
constexpr uint8_t kMaxHopCount       = 3;

// ESP-NOW supports up to 250-byte payloads. We cap NocturNation frames much
// smaller; the largest defined v1 payload is LIGHT_COMMAND at 8 bytes, so a
// 32-byte working ceiling leaves comfortable room for v2 message types.
constexpr uint8_t kMaxFrameSize   = 32;
constexpr uint8_t kMaxPayloadSize = kMaxFrameSize - kHeaderSize;

// Per spec v0.29 §4.3, the protocol has exactly two active message
// types: HEARTBEAT and LIGHT_COMMAND. Numeric IDs 0x01, 0x02, 0x04,
// 0x05, 0x06 are RESERVED (do not reuse) - they were assigned to
// earlier-draft message types that never had a real consumer or
// were folded into HEARTBEAT's payload. See spec §4.3 "Messages
// removed from earlier drafts" for the per-ID rationale. 0x07-0xFE
// are unassigned; 0xFF is the Extension slot for v2.
enum class MessageType : uint8_t {
    Heartbeat    = 0x00,
    // 0x01 reserved - was BEAT_DETECTED (no Lume-side consumer ever existed)
    // 0x02 reserved - was MODE_CHANGE (mode transitions are implicit in LIGHT_COMMAND traffic)
    LightCommand = 0x03,
    // 0x04 reserved - was CLOCK_SYNC (folded into HEARTBEAT.tick)
    // 0x05 reserved - was TIME_SYNC (folded into HEARTBEAT.days_since_2026 + .centiseconds_today)
    // 0x06 reserved - was MUSIC_EVENT (Director no longer emits DROP/BREAKDOWN/BUILD)
    Extension    = 0xFF,
};

struct Header {
    uint8_t     protocol_version;  // forced to kProtocolVersion by encoders
    uint8_t     source_id;         // 1-254; 0xFF = broadcast
    uint8_t     sequence_number;   // 1-255 wrapping; 0 = sequencing disabled
    uint8_t     hop_count;         // 0 = original; cap at kMaxHopCount
    MessageType message_type;      // set by encoders, read by callers
    uint8_t     payload_len;       // set by encoders, read by callers
};

// =============================================================================
// Per-message-type payload structs
// =============================================================================

// HEARTBEAT payload (spec v0.29 §4.3). Director broadcasts at 1 Hz
// (skipped when other traffic implicitly proves the Director is
// alive). Tier 0/1/2 Lumes consume only `tick` (for ASR envelope
// clock anchoring); Tier 3 Lumes additionally consume the date
// fields for replay-protection. Lumes that do not need a field
// simply ignore it.
struct HeartbeatPayload {
    uint32_t tick;                 // Director clock tick (u32 little-endian)
    uint16_t days_since_2026;      // u16 little-endian; 0 if Director has no wall clock
    uint32_t centiseconds_today;   // u24 little-endian; high byte ignored on encode; 0 if no wall clock
};
constexpr uint8_t kHeartbeatPayloadLen = 9;   // 4 + 2 + 3

struct LightCommandPayload {
    uint8_t target_class;          // 0 = all classes; see hal::DeviceClass enum (Epic 4.65)
    uint8_t target_group;          // 0 = all groups; 1-255 specific (PixMob enforces its own 0-31 cap)
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t attack;                // pixmob::Time index
    uint8_t sustain;               // pixmob::Time index
    uint8_t release;               // pixmob::Time index
    uint8_t chance;                // pixmob::Chance index
};
constexpr uint8_t kLightCommandPayloadLen = 9;

// =============================================================================
// Result codes
// =============================================================================

enum class DecodeResult : uint8_t {
    Ok,
    BufferTooShort,            // buf_len < kHeaderSize, or < header+payload
    InvalidProtocolVersion,
    InvalidMessageType,
    PayloadLenMismatch,        // payload_len != expected for the message type
};

// =============================================================================
// Encoders
//
// Each encoder writes the full frame (header + payload) into `buf`, returning
// the total number of bytes written. Returns 0 if `buf_len` is insufficient.
//
// The encoder forces protocol_version = kProtocolVersion and writes the correct
// message_type and payload_len for the call. Callers must populate source_id,
// sequence_number, and hop_count on the Header passed in.
// =============================================================================

size_t encode_heartbeat    (uint8_t* buf, size_t buf_len, const Header& hdr,
                            const HeartbeatPayload& p);
size_t encode_light_command(uint8_t* buf, size_t buf_len, const Header& hdr,
                            const LightCommandPayload& p);

// =============================================================================
// Decoders
//
// decode_header validates protocol_version, message_type and that
// buf_len >= kHeaderSize + payload_len. It does NOT check that payload_len
// matches the expected size for the message type - that is the per-payload
// decoder's responsibility.
//
// Per-payload decoders take the payload bytes only (i.e. buf + kHeaderSize)
// and validate payload_len against the type's expected size.
// =============================================================================

DecodeResult decode_header(const uint8_t* buf, size_t buf_len, Header& out_hdr);

DecodeResult decode_heartbeat   (const Header& hdr,
                                 const uint8_t* payload, size_t payload_len,
                                 HeartbeatPayload& out);
DecodeResult decode_light_command(const Header& hdr,
                                  const uint8_t* payload, size_t payload_len,
                                  LightCommandPayload& out);

}  // namespace espnow
}  // namespace transport
}  // namespace nocturnation
