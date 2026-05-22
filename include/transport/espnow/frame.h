// NocturNation ESP-NOW frame format (v2)
//
// Wire format per the protocol manual §3.1 (nocturnation-docs repo). Pure logic; no ESP32
// / radio dependencies, so this layer compiles and tests on native (laptop).
//
// The radio transport (Block 3+ of Epic 4) consumes these encoders to fill
// ESP-NOW packets, and feeds received bytes through the decoders.
//
// Frame layout (v2 - protocol_version == 0x02):
//   Offset  Field             Size  Notes
//   0       magic[0]          1     0x4E ('N')
//   1       magic[1]          1     0x4E ('N')
//   2       protocol_version  1     0x02 for v2
//   3       source_id         1     1-254; 0xFF = broadcast
//   4       sequence_number   1     1-255 wraps; 0 = sequencing disabled
//   5       hop_count         1     0 = original; cap at kMaxHopCount
//   6       message_type      1     See MessageType enum
//   7       payload_len       1     Bytes of payload following header
//   8+      payload           N     Type-specific (little-endian)
//
// The 2-byte magic prefix is the cheapest discriminator against other
// ESP-NOW users sharing the channel (a real concern at events like
// EMF where many devices broadcast on the same band). A receiver
// rejects any inbound frame whose first two bytes are not "NN"
// before doing any further header validation.

#pragma once

#include <cstddef>
#include <cstdint>

namespace nocturnation {
namespace transport {
namespace espnow {

// Header constants (spec §3.1)
constexpr uint8_t kMagic0            = 0x4E;  // 'N' - NocturNation discriminator byte 0
constexpr uint8_t kMagic1            = 0x4E;  // 'N' - NocturNation discriminator byte 1
constexpr uint8_t kProtocolVersion   = 0x02;  // bumped from 0x01 for the magic-prefix wire change
constexpr uint8_t kBroadcastSourceId = 0xFF;

// Source-id partitioning per protocol manual §3.4. Channel 1 uses the
// community range (stable per device, persisted to NVS). Channel 11 uses
// the Performance range (random per boot, listen-before-broadcast).
// 0xFF (kBroadcastSourceId, above) is reserved for broadcast / anonymous.
constexpr uint8_t kSourceIdCommunityMin   = 0x00;
constexpr uint8_t kSourceIdCommunityMax   = 0x3F;  // 64 slots
constexpr uint8_t kSourceIdPerformanceMin = 0x40;
constexpr uint8_t kSourceIdPerformanceMax = 0xFE;  // 191 slots

constexpr bool is_community_range(uint8_t source_id) {
    return source_id <= kSourceIdCommunityMax;
}
constexpr bool is_performance_range(uint8_t source_id) {
    return source_id >= kSourceIdPerformanceMin &&
           source_id <= kSourceIdPerformanceMax;
}

constexpr uint8_t kHeaderSize        = 8;     // 2 magic + 1 version + 5 metadata
constexpr uint8_t kMaxHopCount       = 3;

// ESP-NOW supports up to 250-byte payloads. We cap NocturNation frames much
// smaller; the largest active v2 payload is LIGHT_COMMAND at 9 bytes, so a
// 32-byte working ceiling leaves room for future message types.
constexpr uint8_t kMaxFrameSize   = 32;
constexpr uint8_t kMaxPayloadSize = kMaxFrameSize - kHeaderSize;   // = 24

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
    InvalidMagic,              // buf[0..1] != "NN" - not a NocturNation frame at all
    InvalidProtocolVersion,    // magic OK but version byte not recognised
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
