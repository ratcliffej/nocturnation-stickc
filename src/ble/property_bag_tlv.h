// Property-bag TLV codec (Epic 20 B3b) — the wire format used by both the
// BLE `config` characteristic (Docs/manuals/ble-service.md §4) and, in a
// follow-on epic, the ESP-NOW CONFIG_WRITE frame's payload. One codec,
// two channels, one key namespace.
//
// Wire format (packed, no padding):
//
//   u8   entry_count
//   repeat entry_count times:
//     u8    key_len              // 1..255 bytes
//     u8[]  key                  // ASCII, NOT null-terminated
//     u8    value_type           // see ValueType enum below
//     u8    value_len            // bytes (0..255)
//     u8[]  value                // encoding per value_type
//
// Reader semantics: malformed TLV aborts the parse and returns
// DecodeError::Malformed. Unrecognised keys are handled by the caller (the
// callback simply doesn't recognise them). Value-type mismatch on a
// recognised key is a caller-side rejection, not a codec concern — the
// codec surfaces every entry as (key, type, value_slice) and lets the
// caller decide.
//
// Native-testable: no Arduino / NimBLE dependencies. Header-only would
// have worked but the .cpp keeps template pressure down + gives tests
// a stable ABI.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nocturnation {
namespace ble {

enum class ValueType : uint8_t {
    U8    = 0x00,
    U16   = 0x01,
    U32   = 0x02,
    Bytes = 0x03,
    Utf8  = 0x04,
    Bool  = 0x05,
};

enum class DecodeError : uint8_t {
    Ok              = 0x00,
    Malformed       = 0x01,   // key_len or value_len overruns the buffer
    EmptyBuffer     = 0x02,   // buffer too small to hold the entry count
    CallbackAbort   = 0x03,   // decoder callback returned false
};

// One entry surfaced to the decoder callback. Views into the caller-owned
// buffer; do not outlive the decode() call.
struct TlvEntry {
    const char*  key;          // NOT null-terminated
    uint8_t      key_len;
    ValueType    type;
    const uint8_t* value;      // may be null if value_len == 0
    uint8_t      value_len;
};

// Callback signature: return true to continue iterating, false to abort
// the decode with DecodeError::CallbackAbort. `ctx` is user-provided
// state (avoids allocating std::function on embedded targets).
using DecodeCallback = bool (*)(const TlvEntry& entry, void* ctx);

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------

class TlvEncoder {
public:
    // Wraps a caller-owned buffer. Nothing is written until an add_* call
    // succeeds; the buffer is untouched on overflow (previous entries stay
    // intact, entry_count reflects only entries actually persisted).
    TlvEncoder(uint8_t* buf, size_t buflen);

    // Return the total number of bytes written so far (including the
    // entry_count header). 0 if nothing has been added yet (still writes
    // the zero-entry header — a buffer of size 0 returns 0 and cannot be
    // added to).
    size_t size() const { return len_; }
    uint8_t entry_count() const { return count_; }

    // Add primitives. Return true on success, false if the entry would
    // overflow the buffer (caller can check and either grow the buffer or
    // finalise with what fits). Overflow is atomic: no partial write.
    bool add_u8   (const char* key, uint8_t value);
    bool add_u16  (const char* key, uint16_t value);   // encoded little-endian
    bool add_u32  (const char* key, uint32_t value);   // encoded little-endian
    bool add_bool (const char* key, bool value);
    bool add_bytes(const char* key, const uint8_t* value, uint8_t value_len);
    bool add_utf8 (const char* key, const char* value);           // NUL-terminated
    bool add_utf8 (const char* key, const char* value, uint8_t value_len);

private:
    bool write_entry(const char* key, ValueType type,
                     const uint8_t* value, uint8_t value_len);

    uint8_t* buf_;
    size_t   buflen_;
    size_t   len_;    // current write offset
    uint8_t  count_;  // entries persisted
};

// -----------------------------------------------------------------------------
// Decoder
// -----------------------------------------------------------------------------

// Iterate the TLV entries in `buf` (up to `buflen` bytes). Calls `cb` per
// entry with a view into the buffer. Returns DecodeError::Ok on a clean
// walk, or an error status on malformed input / callback abort.
//
// The decoder is defensive: it validates each entry's key_len and
// value_len against the remaining buffer before advancing, so a hostile
// or corrupt input can't read past the end. On error, the caller has
// already seen any entries the callback accepted up to the failure point.
DecodeError decode_property_bag(const uint8_t* buf, size_t buflen,
                                DecodeCallback cb, void* ctx);

// -----------------------------------------------------------------------------
// Well-known keys (Docs/manuals/ble-service.md §5)
// -----------------------------------------------------------------------------
//
// Header-only constants — one source of truth for firmware + tests + the
// spec doc. Adding a key is a matter of appending here and updating §5.
//
// Naming: ASCII, lowercase snake_case, ≤20 bytes. Prefixes starting with
// underscore are reserved for future service-internal use.

namespace key {

// Lume role
constexpr const char* kGroup          = "group";           // u8
constexpr const char* kLedPower       = "led_power";       // u8 (0..100)
constexpr const char* kBoundSid       = "bound_sid";       // u16 (0xFFFF = TOFU)
constexpr const char* kChannelPref    = "channel_pref";    // u8 (0/1/6/11)

// Director role
constexpr const char* kDirSidPerf     = "dir_sid_perf";    // u8 (0x40..0xFE)
constexpr const char* kRetxCount      = "retx_count";      // u8 (1..5)

// Both roles
constexpr const char* kPairWinS       = "pair_win_s";      // u8 (5..255 seconds)
constexpr const char* kFriendlyName   = "friendly_name";   // utf8 (0..20 bytes)

}  // namespace key

}  // namespace ble
}  // namespace nocturnation
