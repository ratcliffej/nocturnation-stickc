// NocturNation ESP-NOW frame format (v3)
//
// Wire format per the protocol manual §3.1 (nocturnation-docs repo). Pure logic; no ESP32
// / radio dependencies, so this layer compiles and tests on native (laptop).
//
// The radio transport (Block 3+ of Epic 4) consumes these encoders to fill
// ESP-NOW packets, and feeds received bytes through the decoders.
//
// Frame layout (v3 - protocol_version == 0x03):
//   Offset  Field             Size  Notes
//   0       magic[0]          1     0x4E ('N')
//   1       magic[1]          1     0x4E ('N')
//   2       protocol_version  1     0x03 for v3
//   3       source_id         2     LE u16; 0x0001..0xFFFE; 0xFFFF = broadcast
//   5       sequence_number   1     1-255 wraps; 0 = sequencing disabled
//   6       hop_count         1     0 = original; cap at kMaxHopCount
//   7       message_type      1     See MessageType enum
//   8       payload_len       1     Bytes of payload following header
//   9+      payload           N     Type-specific (little-endian)
//
// The 2-byte magic prefix is the cheapest discriminator against other
// ESP-NOW users sharing the channel (a real concern at events like
// EMF where many devices broadcast on the same band). A receiver
// rejects any inbound frame whose first two bytes are not "NN"
// before doing any further header validation.
//
// v2 -> v3 change: source_id and target_group both widened from
// 1 byte to 2 bytes (LE). Motivation is futureproofing headroom -
// stadium-scale addressing for target_group, birthday-paradox safety
// for source_id (191 slots -> 65534 slots). NVS storage + config UI
// stay at u8; the top byte of source_id and target_group is zero for
// current-generation Directors, and the wider space becomes usable
// the day the UI is widened to expose it. Old-firmware devices
// reject v3 frames at the version check; v3 is a hard cutover, not
// a compatible extension.

#pragma once

#include <cstddef>
#include <cstdint>

#include "hal/hal.h"   // for hal::Capability (used by message_type_required_capability)

namespace nocturnation {
namespace transport {
namespace espnow {

// Header constants (spec §3.1)
constexpr uint8_t  kMagic0            = 0x4E;  // 'N' - NocturNation discriminator byte 0
constexpr uint8_t  kMagic1            = 0x4E;  // 'N' - NocturNation discriminator byte 1
constexpr uint8_t  kProtocolVersion   = 0x03;  // bumped from 0x02 for the u16 source_id / target_group widening
constexpr uint16_t kBroadcastSourceId = 0xFFFF;

// Source-id partitioning per protocol manual §3.4. Channel 1 uses the
// community range (stable per device, persisted to NVS). Channel 11 uses
// the Performance range (random per boot, listen-before-broadcast).
// 0xFFFF (kBroadcastSourceId, above) is reserved for broadcast / anonymous.
// Partition boundaries stay at u8-natural values in v3 to preserve the
// existing hex-editor UI and NVS-persisted values; the extended range
// (0x0100..0xFFFE) is future headroom carried on the wire so a later NVS/UI
// widening can populate it without another wire break.
constexpr uint16_t kSourceIdCommunityMin   = 0x0000;
constexpr uint16_t kSourceIdCommunityMax   = 0x003F;  // 64 slots
constexpr uint16_t kSourceIdPerformanceMin = 0x0040;
constexpr uint16_t kSourceIdPerformanceMax = 0x00FE;  // 191 slots

constexpr bool is_community_range(uint16_t source_id) {
    return source_id <= kSourceIdCommunityMax;
}
constexpr bool is_performance_range(uint16_t source_id) {
    return source_id >= kSourceIdPerformanceMin &&
           source_id <= kSourceIdPerformanceMax;
}

constexpr uint8_t kHeaderSize        = 9;     // 2 magic + 1 version + 6 metadata (source_id widened to 2 bytes)
constexpr uint8_t kMaxHopCount       = 3;

// Wire byte offset of hop_count within the v3 header (see write_header /
// decode_header). Exposed so a repeater can bump hop_count in place
// without re-encoding the whole frame, and so the offset lives next to
// the layout it depends on. v3 shifted this by 1 (was 5 in v2) as
// source_id widened from 1 to 2 bytes; keep this in lock-step with
// write_header() / decode_header() if the layout ever changes again.
constexpr size_t kHopCountOffset = 6;

// ESP-NOW supports up to 250-byte payloads. Pre-Epic-13 the firmware
// capped frames at 32 bytes because no message type exceeded 16 bytes
// of payload. Epic 13 introduces TEXT_DISPLAY (up to ~200 bytes for
// header+body strings) and BITMAP_PLANE (up to ~240 bytes of pixel
// data per frame), so the ceiling now matches the radio's physical
// maximum. The wash-family payloads are unchanged on the wire.
constexpr uint16_t kMaxFrameSize   = 250;
constexpr uint16_t kMaxPayloadSize = kMaxFrameSize - kHeaderSize;   // = 242

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
    // Epic 13: display-content family. Routed to DeviceClass::Display
    // bindings on hosts that declare Capability::DisplayText /
    // Capability::DisplayBitmap. The wash family targets Light;
    // the display family targets Display - addressing is independent.
    TextDisplay    = 0x09,   // header + body strings on the Lume's screen
    BitmapHeader   = 0x0A,   // bitmap framing: dimensions, planes, colours, checksum
    BitmapPlane    = 0x0B,   // bitmap plane data chunk (multiple frames per plane allowed)
    ClearScreen    = 0x0C,   // clear text and/or bitmap on the Lume's screen
    // Headless repeater census (Atom repeater). A headless repeater
    // broadcasts this ~1 Hz so the Director can count how many repeaters
    // are online and on which channel. Not a light/display type - Lumes
    // ignore it, and repeaters never relay it (census is point-to-Director
    // telemetry, not mesh traffic).
    RepeaterHeartbeat = 0x0D,
    Extension      = 0xFF,
};

// =============================================================================
// Message-type → required-capability map (Epic 13).
//
// Each message type that targets a SPECIFIC surface maps to the HAL
// capability a host needs to render it. The Lume's on_recv path uses
// this to quick-drop frames before decoding the payload - a Lume that
// has no display silently discards every TEXT_DISPLAY / BITMAP_* /
// CLEAR_SCREEN frame at the radio gate, no decode work, no fan-out.
//
// Wash-family / Heartbeat / Extension don't map to a single capability
// (a LIGHT_PULSE can be rendered on PixMob via IRTx, a strip via
// LedStrip, etc.); they return kNoSpecificCapability and binding-level
// filtering decides which surfaces actually fire.
//
// PixMob relay note: this gate is on the Lume's INBOUND path. PixMob
// IR-side relay decisions (which PixMob capabilities exist downstream)
// remain owned by PixMobIrBinding and run against bracelet IR codes,
// not this map.
constexpr hal::Capability kNoSpecificCapability =
    static_cast<hal::Capability>(0xFF);

constexpr hal::Capability message_type_required_capability(MessageType t) {
    switch (t) {
        case MessageType::TextDisplay:    return hal::Capability::DisplayText;
        case MessageType::BitmapHeader:   return hal::Capability::DisplayBitmap;
        case MessageType::BitmapPlane:    return hal::Capability::DisplayBitmap;
        // ClearScreen: cheapest reasonable gate is "host has a display
        // text/bitmap surface to clear". Hosts that declare DisplayText
        // also declare DisplayBitmap (and vice versa) in current fleet
        // bringup, so this single capability suffices as a Display-
        // family proxy. A future split (text-only host) wouldn't see
        // bitmap clears, which is the correct behaviour.
        case MessageType::ClearScreen:    return hal::Capability::DisplayText;

        case MessageType::Heartbeat:
        case MessageType::LightPulse:
        case MessageType::LightWash:
        case MessageType::LightWashEnd:
        case MessageType::LightWashPulse:
        case MessageType::Extension:
        default:
            return kNoSpecificCapability;
    }
}

struct Header {
    uint8_t     protocol_version;  // forced to kProtocolVersion by encoders
    uint16_t    source_id;         // 0x0001..0xFFFE; 0xFFFF = broadcast (LE on wire)
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

// REPEATER_HEARTBEAT payload (headless repeater census). Emitted ~1 Hz
// by a headless repeater. `uid` is the low 3 bytes of the repeater's STA
// MAC - a stable per-device identity the Director dedups the census on,
// independent of the header source_id (which the repeater sets to its own
// derived id). `channel` lets the Director confirm the repeater is on the
// fleet channel; `relayed_count` and `uptime_s` are liveness/telemetry.
struct RepeaterHeartbeatPayload {
    uint8_t  uid[3];          // low 3 bytes of STA MAC (stable device id)
    uint8_t  channel;         // WiFi channel the repeater is relaying on
    uint16_t uptime_s;        // seconds since boot, saturating at 0xFFFF
    uint32_t relayed_count;   // frames relayed since boot
};
constexpr uint8_t kRepeaterHeartbeatPayloadLen = 10;   // 3 + 1 + 2 + 4

struct LightPulsePayload {
    uint8_t  target_class;         // 0 = all classes; see hal::DeviceClass enum (Epic 4.65)
    uint16_t target_group;         // 0 = all groups; 1-65534 specific (LE on wire; PixMob enforces its own 0-31 cap)
    uint8_t  r;
    uint8_t  g;
    uint8_t  b;
    uint8_t  attack;               // pixmob::Time index
    uint8_t  sustain;              // pixmob::Time index
    uint8_t  release;              // pixmob::Time index
    uint8_t  chance;               // pixmob::Chance index
};
constexpr uint8_t kLightPulsePayloadLen = 10;   // v3: +1 byte for u16 target_group

// LIGHT_WASH payload (Epic 6C Phase D). A persistent background wash on
// capable Lumes - the §1.2 lighting-design baseline that PULSE punctuates.
// Two RGB triplets + cycle_ms produce a cosine-eased ping-pong drift
// between the two colours; cycle_ms = 0 holds r1/g1/b1 only. Attack and
// release are in 100 ms units (0..25.5 s) - wash-scale durations, longer
// than the pulse Time enum's 3.84 s cap. See lume-capabilities-design.md
// §4.1 for the renderer contract.
struct LightWashPayload {
    uint8_t  target_class;
    uint16_t target_group;         // LE on wire (v3)
    uint8_t  r1, g1, b1;           // start colour
    uint8_t  r2, g2, b2;           // end colour (ignored when cycle_ms == 0)
    uint8_t  attack;               // 100 ms units; ramp from current to wash baseline
    uint8_t  release;              // 100 ms units; default fade-out (may be overridden by LIGHT_WASH_END)
    uint8_t  intensity;            // 0-255 brightness scalar applied to the wash baseline
    uint16_t cycle_ms;             // little-endian; one full A<->B<->A oscillation; 0 = no cycle (hold r1g1b1)
    uint16_t ttl_seconds;          // little-endian; 0 = infinite
    uint8_t  pulse_response;       // 0 = ignore PULSE while washing; 1 = accept PULSE as additive overlay
};
// Wire layout (17 bytes): class(1) + group(2 LE) + r1g1b1(3) + r2g2b2(3)
// + attack(1) + release(1) + intensity(1) + cycle_ms(2 LE) + ttl_seconds(2 LE)
// + pulse_response(1). v3: +1 byte for the u16 target_group widening.
constexpr uint8_t kLightWashPayloadLen = 17;

// LIGHT_WASH_END payload (Epic 6C Phase D). Explicit cancel of an active
// wash on the addressed target(s). release_time (100 ms units) overrides
// the active wash's own release field.
struct LightWashEndPayload {
    uint8_t  target_class;
    uint16_t target_group;         // LE on wire (v3)
    uint8_t  release_time;         // 100 ms units; fade from instantaneous wash to black over this duration
};
constexpr uint8_t kLightWashEndPayloadLen = 4;   // v3: +1 byte for u16 target_group

// LIGHT_WASH_PULSE payload (Epic 6C Phase D). Identical to LightPulsePayload
// on the wire; differs only in dispatch semantics - it fires only on Lumes
// currently in wash state (non-washing Lumes silently drop). The struct
// is a separate type from LightPulsePayload to keep the call sites
// type-distinguishable; the byte layout is intentionally the same so the
// encoder can share format with light_pulse if desirable later.
struct LightWashPulsePayload {
    uint8_t  target_class;
    uint16_t target_group;         // LE on wire (v3)
    uint8_t  r;
    uint8_t  g;
    uint8_t  b;
    uint8_t  attack;
    uint8_t  sustain;
    uint8_t  release;
    uint8_t  chance;
};
constexpr uint8_t kLightWashPulsePayloadLen = 10;   // v3: +1 byte for u16 target_group

// =============================================================================
// Epic 13: display-content payloads (TEXT_DISPLAY, BITMAP_HEADER,
// BITMAP_PLANE, CLEAR_SCREEN).
//
// Variable-length payloads (TEXT_DISPLAY, BITMAP_PLANE) use a length-
// prefixed in-band encoding. The struct itself holds max-sized buffers
// + an explicit length field; encoders write only the valid portion;
// decoders validate length fields against payload_len and copy the
// valid bytes back into the struct's buffer. The wash family stays
// fixed-size on the wire - the variable-length pattern is introduced
// for Epic 13 only.
// =============================================================================

// TEXT_DISPLAY payload. The message type itself denotes the surface
// (DeviceClass::Display + Capability::DisplayText); we don't carry a
// target_class byte here - it would be dead weight, see the
// message_type_required_capability() helper below. target_group stays
// for per-Lume addressing within the Display class (group 0 = all).
//
// header and body are independent strings - either may be empty
// (length 0). Empty header means "no header line"; empty body means
// "no body text". Empty both is legal (acts as a no-op on a surface
// that has no current content).
//
// Wire layout (variable, 9..201 bytes):
//   target_group(2 LE) + r(1) + g(1) + b(1)
//   + ttl_ms(2 LE) + header_len(1) + header_bytes(header_len)
//   + body_len(1) + body_bytes(body_len)
//
// ttl_ms == 0 means "sticky" (persists until CLEAR_SCREEN); non-zero is
// auto-clear after that many milliseconds (Lume side).
constexpr uint8_t kTextDisplayMaxHeaderLen = 64;
constexpr uint8_t kTextDisplayMaxBodyLen   = 128;
constexpr uint16_t kTextDisplayFixedPrefixLen = 7;  // group(2 LE)+r+g+b+ttl_ms_LE (v3)
constexpr uint16_t kTextDisplayMinPayloadLen  =     // both strings empty
    kTextDisplayFixedPrefixLen + 1 /*header_len*/ + 1 /*body_len*/;
constexpr uint16_t kTextDisplayMaxPayloadLen  =
    kTextDisplayMinPayloadLen + kTextDisplayMaxHeaderLen + kTextDisplayMaxBodyLen;

struct TextDisplayPayload {
    uint16_t target_group;    // LE on wire (v3)
    uint8_t  r;
    uint8_t  g;
    uint8_t  b;
    uint16_t ttl_ms;          // 0 = sticky; non-zero = auto-clear after N ms

    uint8_t  header_len;      // 0..kTextDisplayMaxHeaderLen
    char     header[kTextDisplayMaxHeaderLen];  // UTF-8; bytes 0..header_len-1 valid

    uint8_t  body_len;        // 0..kTextDisplayMaxBodyLen
    char     body[kTextDisplayMaxBodyLen];      // UTF-8; bytes 0..body_len-1 valid
};

// BITMAP_HEADER payload (fixed-size, 37 bytes). One emission per logical
// bitmap; the receiver allocates a staging buffer sized to width*height
// bits per plane * plane_count, accumulates bytes from BITMAP_PLANE
// frames, verifies the CRC32 once the expected total has arrived, then
// commits the bitmap to the render surface.
//
// As with TEXT_DISPLAY, the message type denotes the surface
// (Capability::DisplayBitmap) - no target_class byte.
//
// Wire layout (38 bytes, v3):
//   target_group(2 LE) + width(1) + height(1)
//   + plane_count(1) + colours[8] (24, three bytes each)
//   + fit(1) + zoom_pct(1) + overwrite(1)
//   + checksum(4 LE) + ttl_ms(2 LE)
//
// fit:
//   0 = ACTUAL (plot at native dimensions, top-left origin)
//   1 = FIT    (preserve aspect, fit to display largest dimension)
//   2 = ZOOM   (preserve aspect, fill display + crop)
// zoom_pct multiplies the FIT/ZOOM scale (100 = baseline; 50 = half; 200 = 2x).
// overwrite:
//   0 = ADDITIVE (compose onto whatever is currently on the surface)
//   1 = REPLACE  (clear surface at plane 0 of this header set)
constexpr uint8_t  kBitmapMaxPlanes       = 8;
constexpr uint8_t  kBitmapMaxDimension    = 64;   // hard cap to keep wire airtime sane
constexpr uint8_t  kBitmapHeaderPayloadLen = 38;  // v3: +1 byte for u16 target_group

struct BitmapHeaderPayload {
    uint16_t target_group;      // LE on wire (v3)
    uint8_t  width;             // 1..kBitmapMaxDimension
    uint8_t  height;            // 1..kBitmapMaxDimension
    uint8_t  plane_count;       // 1..kBitmapMaxPlanes
    uint8_t  colours[kBitmapMaxPlanes][3];   // RGB per plane slot
    uint8_t  fit;               // 0=ACTUAL, 1=FIT, 2=ZOOM
    uint8_t  zoom_pct;          // baseline 100
    uint8_t  overwrite;         // 0=ADDITIVE, 1=REPLACE
    uint32_t checksum;          // CRC32 over all plane bytes concatenated
    uint16_t ttl_ms;            // 0 = sticky
};

// BITMAP_PLANE payload (variable-length, 5..242 bytes). Carries a chunk
// of one plane's pixel bytes. Multiple BITMAP_PLANE frames per plane are
// allowed and distinguished by byte_offset; the receiver assembles them
// into the staging buffer at the plane's base offset + byte_offset.
//
// Wire layout (variable, 6..241 bytes, v3):
//   target_group(2 LE) + plane_index(1)
//   + byte_offset(2 LE) + data_len(1) + data_bytes(data_len)
constexpr uint16_t kBitmapPlaneFixedPrefixLen = 6;  // v3: +1 byte for u16 target_group
constexpr uint16_t kBitmapPlaneMinPayloadLen  = kBitmapPlaneFixedPrefixLen;
constexpr uint16_t kBitmapPlaneMaxDataLen     =
    kMaxPayloadSize - kBitmapPlaneFixedPrefixLen;   // v3: = kMaxPayloadSize - 6

struct BitmapPlanePayload {
    uint16_t target_group;      // LE on wire (v3)
    uint8_t  plane_index;       // 0..plane_count-1 (per the preceding BITMAP_HEADER)
    uint16_t byte_offset;       // offset into this plane's pixel-byte stream
    uint8_t  data_len;          // 0..kBitmapPlaneMaxDataLen
    uint8_t  data[kBitmapPlaneMaxDataLen];
};

// CLEAR_SCREEN payload (fixed-size, 3 bytes). Per-surface clear request.
// clear_text and clear_bitmap are independent so an author can clear
// one without disturbing the other (e.g. drop the song title but keep
// the band logo visible underneath).
//
// Wire layout (4 bytes, v3):
//   target_group(2 LE) + clear_text(1) + clear_bitmap(1)
constexpr uint8_t kClearScreenPayloadLen = 4;   // v3: +1 byte for u16 target_group

struct ClearScreenPayload {
    uint16_t target_group;      // LE on wire (v3)
    uint8_t  clear_text;        // 0 = leave text alone, 1 = clear text layer
    uint8_t  clear_bitmap;      // 0 = leave bitmap alone, 1 = clear bitmap layer
};

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

// Epic 13 encoders. Variable-length encoders write the in-band length
// prefixes and only the valid bytes; payload_len in the header is
// computed by the encoder per-call from the variable lengths.
size_t encode_text_display    (uint8_t* buf, size_t buf_len, const Header& hdr,
                               const TextDisplayPayload& p);
size_t encode_bitmap_header   (uint8_t* buf, size_t buf_len, const Header& hdr,
                               const BitmapHeaderPayload& p);
size_t encode_bitmap_plane    (uint8_t* buf, size_t buf_len, const Header& hdr,
                               const BitmapPlanePayload& p);
size_t encode_clear_screen    (uint8_t* buf, size_t buf_len, const Header& hdr,
                               const ClearScreenPayload& p);

// Headless repeater census.
size_t encode_repeater_heartbeat(uint8_t* buf, size_t buf_len, const Header& hdr,
                                 const RepeaterHeartbeatPayload& p);

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

// Rewrite the hop_count byte of an already-encoded frame in place. Used
// by a repeater that rebroadcasts a frame unchanged except for
// incrementing the hop counter (source_id + sequence_number are preserved
// so mesh-wide dedup still recognises the original and its repeats as
// one frame). No-op if buf_len < kHeaderSize.
void set_hop_count(uint8_t* buf, size_t buf_len, uint8_t new_hops);

DecodeResult decode_heartbeat       (const Header& hdr,
                                     const uint8_t* payload, size_t payload_len,
                                     HeartbeatPayload& out);
DecodeResult decode_repeater_heartbeat(const Header& hdr,
                                     const uint8_t* payload, size_t payload_len,
                                     RepeaterHeartbeatPayload& out);
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

// Epic 13 decoders. Variable-length decoders validate the length
// prefixes against payload_len; out_struct's *_len fields reflect the
// actual decoded length, and only those bytes of out_struct.header /
// out_struct.body / out_struct.data are populated.
DecodeResult decode_text_display    (const Header& hdr,
                                     const uint8_t* payload, size_t payload_len,
                                     TextDisplayPayload& out);
DecodeResult decode_bitmap_header   (const Header& hdr,
                                     const uint8_t* payload, size_t payload_len,
                                     BitmapHeaderPayload& out);
DecodeResult decode_bitmap_plane    (const Header& hdr,
                                     const uint8_t* payload, size_t payload_len,
                                     BitmapPlanePayload& out);
DecodeResult decode_clear_screen    (const Header& hdr,
                                     const uint8_t* payload, size_t payload_len,
                                     ClearScreenPayload& out);

}  // namespace espnow
}  // namespace transport
}  // namespace nocturnation
