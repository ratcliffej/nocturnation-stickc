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

enum class MessageType : uint8_t {
    Heartbeat    = 0x00,
    BeatDetected = 0x01,
    ModeChange   = 0x02,
    LightCommand = 0x03,
    ClockSync    = 0x04,
    TimeSync     = 0x05,
    MusicEvent   = 0x06,   // DROP / BREAKDOWN / BUILD; Epic 4.5 Block 4
    Extension    = 0xFF,
};

// MusicEvent payload's event_type field. Wire-stable values; see
// architecture spec §4.3 MUSIC_EVENT row. Receivers that don't
// understand a given event type should leave it as Unknown and
// silently drop the frame (forward-compatible).
enum class MusicEventType : uint8_t {
    Unknown   = 0,
    Drop      = 1,
    Breakdown = 2,
    Build     = 3,   // reserved by spec; not fired by Epic 4.5 producers
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

struct HeartbeatPayload {};
constexpr uint8_t kHeartbeatPayloadLen = 0;

struct BeatDetectedPayload {
    uint8_t  strength;             // 0-255
    uint16_t bpm_x10;              // BPM * 10, little-endian on the wire
};
constexpr uint8_t kBeatDetectedPayloadLen = 3;

struct ModeChangePayload {
    uint8_t new_mode;
    uint8_t palette_id;
};
constexpr uint8_t kModeChangePayloadLen = 2;

struct LightCommandPayload {
    uint8_t target_class;          // 0 = all classes; see plugins::DeviceClass enum (Epic 4.65)
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

struct ClockSyncPayload {
    uint16_t phase_in_bar;         // 0-65535 represents 0.0-1.0 of bar
    uint16_t bpm_x10;
};
constexpr uint8_t kClockSyncPayloadLen = 4;

struct TimeSyncPayload {
    uint16_t days_since_2026;      // u16 little-endian
    uint32_t centiseconds_today;   // u24 little-endian; high byte of u32 ignored on encode
};
constexpr uint8_t kTimeSyncPayloadLen = 5;

struct MusicEventPayload {
    MusicEventType event_type;     // 1-byte enum; see MusicEventType comment
};
constexpr uint8_t kMusicEventPayloadLen = 1;

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

size_t encode_heartbeat    (uint8_t* buf, size_t buf_len, const Header& hdr);
size_t encode_beat_detected(uint8_t* buf, size_t buf_len, const Header& hdr,
                            const BeatDetectedPayload& p);
size_t encode_mode_change  (uint8_t* buf, size_t buf_len, const Header& hdr,
                            const ModeChangePayload& p);
size_t encode_light_command(uint8_t* buf, size_t buf_len, const Header& hdr,
                            const LightCommandPayload& p);
size_t encode_clock_sync   (uint8_t* buf, size_t buf_len, const Header& hdr,
                            const ClockSyncPayload& p);
size_t encode_time_sync    (uint8_t* buf, size_t buf_len, const Header& hdr,
                            const TimeSyncPayload& p);
size_t encode_music_event  (uint8_t* buf, size_t buf_len, const Header& hdr,
                            const MusicEventPayload& p);

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
                                 const uint8_t* payload, size_t payload_len);
DecodeResult decode_beat_detected(const Header& hdr,
                                  const uint8_t* payload, size_t payload_len,
                                  BeatDetectedPayload& out);
DecodeResult decode_mode_change (const Header& hdr,
                                 const uint8_t* payload, size_t payload_len,
                                 ModeChangePayload& out);
DecodeResult decode_light_command(const Header& hdr,
                                  const uint8_t* payload, size_t payload_len,
                                  LightCommandPayload& out);
DecodeResult decode_clock_sync  (const Header& hdr,
                                 const uint8_t* payload, size_t payload_len,
                                 ClockSyncPayload& out);
DecodeResult decode_time_sync   (const Header& hdr,
                                 const uint8_t* payload, size_t payload_len,
                                 TimeSyncPayload& out);
DecodeResult decode_music_event (const Header& hdr,
                                 const uint8_t* payload, size_t payload_len,
                                 MusicEventPayload& out);

}  // namespace espnow
}  // namespace transport
}  // namespace nocturnation
