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
// smaller; the largest active v2 payload is LIGHT_PULSE at 9 bytes, so a
// 32-byte working ceiling leaves room for future message types.
constexpr uint8_t kMaxFrameSize   = 32;
constexpr uint8_t kMaxPayloadSize = kMaxFrameSize - kHeaderSize;   // = 24

// Per spec v0.29 §4.3, the protocol has exactly two active message
// types: HEARTBEAT and LIGHT_PULSE. Numeric IDs 0x01, 0x02, 0x04,
// 0x05, 0x06 are RESERVED (do not reuse) - they were assigned to
// earlier-draft message types that never had a real consumer or
// were folded into HEARTBEAT's payload. See spec §4.3 "Messages
// removed from earlier drafts" for the per-ID rationale. 0x07-0xFE
// are unassigned; 0xFF is the Extension slot for v2.
enum class MessageType : uint8_t {
    Heartbeat      = 0x00,
    // 0x01 reserved - was BEAT_DETECTED (no Lume-side consumer ever existed)
    // 0x02 reserved - was MODE_CHANGE (mode transitions are implicit in LIGHT_PULSE traffic)
    LightPulse     = 0x03,
    // 0x04 reserved - was CLOCK_SYNC (folded into HEARTBEAT.tick)
    // 0x05 reserved - was TIME_SYNC (folded into HEARTBEAT.days_since_2026 + .centiseconds_today)
    LightWash      = 0x06,   // Epic 6C Phase D - reclaims the v0.27-deprecated MUSIC_EVENT slot
    LightWashEnd   = 0x07,
    LightWashPulse = 0x08,
    Extension      = 0xFF,
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

struct LightPulsePayload {
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
constexpr uint8_t kLightPulsePayloadLen = 9;

// LIGHT_WASH payload (Epic 6C Phase D). A persistent background wash on
// capable Lumes - the §1.2 lighting-design baseline that PULSE punctuates.
// Two RGB triplets + cycle_ms produce a cosine-eased ping-pong drift
// between the two colours; cycle_ms = 0 holds r1/g1/b1 only. Attack and
// release are in 100 ms units (0..25.5 s) - wash-scale durations, longer
// than the pulse Time enum's 3.84 s cap. See lume-capabilities-design.md
// §4.1 for the renderer contract.
struct LightWashPayload {
    uint8_t  target_class;
    uint8_t  target_group;
    uint8_t  r1, g1, b1;           // start colour
    uint8_t  r2, g2, b2;           // end colour (ignored when cycle_ms == 0)
    uint8_t  attack;               // 100 ms units; ramp from current to wash baseline
    uint8_t  release;              // 100 ms units; default fade-out (may be overridden by LIGHT_WASH_END)
    uint8_t  intensity;            // 0-255 brightness scalar applied to the wash baseline
    uint16_t cycle_ms;             // little-endian; one full A<->B<->A oscillation; 0 = no cycle (hold r1g1b1)
    uint16_t ttl_seconds;          // little-endian; 0 = infinite
    uint8_t  pulse_response;       // 0 = ignore PULSE while washing; 1 = accept PULSE as additive overlay
};
// Wire layout (16 bytes): class(1) + group(1) + r1g1b1(3) + r2g2b2(3)
// + attack(1) + release(1) + intensity(1) + cycle_ms(2 LE) + ttl_seconds(2 LE)
// + pulse_response(1). (The Notion source-prompt v0.3 mis-stated this as
// 17 - arithmetic error; corrected here and in the design doc.)
constexpr uint8_t kLightWashPayloadLen = 16;

// LIGHT_WASH_END payload (Epic 6C Phase D). Explicit cancel of an active
// wash on the addressed target(s). release_time (100 ms units) overrides
// the active wash's own release field.
struct LightWashEndPayload {
    uint8_t target_class;
    uint8_t target_group;
    uint8_t release_time;          // 100 ms units; fade from instantaneous wash to black over this duration
};
constexpr uint8_t kLightWashEndPayloadLen = 3;

// LIGHT_WASH_PULSE payload (Epic 6C Phase D). Identical to LightPulsePayload
// on the wire; differs only in dispatch semantics - it fires only on Lumes
// currently in wash state (non-washing Lumes silently drop). The struct
// is a separate type from LightPulsePayload to keep the call sites
// type-distinguishable; the byte layout is intentionally the same so the
// encoder can share format with light_pulse if desirable later.
struct LightWashPulsePayload {
    uint8_t target_class;
    uint8_t target_group;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t attack;
    uint8_t sustain;
    uint8_t release;
    uint8_t chance;
};
constexpr uint8_t kLightWashPulsePayloadLen = 9;

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

size_t encode_heartbeat       (uint8_t* buf, size_t buf_len, const Header& hdr,
                               const HeartbeatPayload& p);
size_t encode_light_pulse     (uint8_t* buf, size_t buf_len, const Header& hdr,
                               const LightPulsePayload& p);
size_t encode_light_wash      (uint8_t* buf, size_t buf_len, const Header& hdr,
                               const LightWashPayload& p);
size_t encode_light_wash_end  (uint8_t* buf, size_t buf_len, const Header& hdr,
                               const LightWashEndPayload& p);
size_t encode_light_wash_pulse(uint8_t* buf, size_t buf_len, const Header& hdr,
                               const LightWashPulsePayload& p);

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

DecodeResult decode_heartbeat       (const Header& hdr,
                                     const uint8_t* payload, size_t payload_len,
                                     HeartbeatPayload& out);
DecodeResult decode_light_pulse     (const Header& hdr,
                                     const uint8_t* payload, size_t payload_len,
                                     LightPulsePayload& out);
DecodeResult decode_light_wash      (const Header& hdr,
                                     const uint8_t* payload, size_t payload_len,
                                     LightWashPayload& out);
DecodeResult decode_light_wash_end  (const Header& hdr,
                                     const uint8_t* payload, size_t payload_len,
                                     LightWashEndPayload& out);
DecodeResult decode_light_wash_pulse(const Header& hdr,
                                     const uint8_t* payload, size_t payload_len,
                                     LightWashPulsePayload& out);

}  // namespace espnow
}  // namespace transport
}  // namespace nocturnation
