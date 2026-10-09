// Pairing register — Director-local store of captured Lume identities
// and secrets (Epic 21 B5).
//
// Each entry represents one Lume the Director has paired with. The
// operator captures entries through either a BLE pairing session
// (Epic 20 B10 flow extended at B6) or an ESP-NOW UID_ANNOUNCE
// pairing-burst (Epic 21 B3e + B7). Once an entry is in the register,
// the Director can target that Lume for authenticated CONFIG_WRITE
// over ESP-NOW without further physical access.
//
// Storage: a 50-slot array held in RAM + persisted to NVS as a single
// blob. 50 entries × 51 bytes = ~2.6 KB — comfortable headroom in the
// StickC Plus2's "noct" NVS namespace. On saturation, add() evicts
// the oldest entry (FIFO - earliest insertion goes first, re-pairing
// does NOT promote). The eviction order is "the 51st knocks out the
// 1st I paired, period" - predictable mental model for the operator.
// For fleets bigger than ~10 entries the StickC UI becomes awkward
// anyway ([[project-device-uid-design]] + Jason 2026-10-07 note) and
// the fleet should migrate to a phone/laptop register as a near-term
// follow-on epic.
//
// `captured_at` is a monotonic sequence number (NOT millis()) that is
// persisted alongside the array in NVS, so eviction ordering survives
// power cycles. There's no wall clock on the StickC - a reboot-safe
// counter is the right primitive.
//
// Pure logic, native-safe. The Arduino build loads/saves via NVS;
// the native stub keeps everything in process-static memory so the
// test env round-trips without a real Preferences backend.

#pragma once

#include <cstddef>
#include <cstdint>

namespace nocturnation {
namespace ble {

// One pairing-register entry. Stored packed on-wire (no padding).
// Format version bumped at Epic 21 B6a hotfix 2026-10-09 to carry
// the last-known config snapshot; see kBlobVersion in pair_register.cpp.
struct RegisterEntry {
    uint32_t uid;                         // CRC32-of-STA-MAC (identity)
    uint8_t  secret[16];                  // HMAC-SHA256 key
    uint8_t  role;                        // ble::Role as u8
    uint8_t  host;                        // ble::Host as u8
    char     friendly_name[21];           // NUL-terminated, 20 chars max
    uint32_t captured_at;                 // monotonic sequence; higher = more recent
    uint32_t last_configured_at;          // 0 if never written to; monotonic sequence
    // Last-known property-bag snapshot. Populated by Config Lumes on
    // successful BLE read / BLE write / ESP-NOW CONFIG_WRITE, so the
    // Paired-fleet editor prefills with what the operator last pushed
    // instead of generic defaults. snap_valid=0 when the entry has
    // never had its config captured through a path that saw the values.
    uint8_t  snap_group;
    uint8_t  snap_led_power;
    uint8_t  snap_channel_pref;
    uint16_t snap_strip_chain;
    uint8_t  snap_strip_group_size;
    uint8_t  snap_pair_win_s;
    uint8_t  snap_valid;                  // 0 = never populated; 1 = has real values
};

// Config snapshot mirror of the Paired-fleet editor fields.
struct ConfigSnapshot {
    uint8_t  group;
    uint8_t  led_power;
    uint8_t  channel_pref;
    uint16_t strip_chain;
    uint8_t  strip_group_size;
    uint8_t  pair_win_s;
};

class PairRegister {
public:
    // Capped at 20 for v1 (Jason 2026-10-09): we haven't measured
    // actual "noct" NVS namespace headroom with the latest key set
    // (dev_uid, dev_secret, cfg_numonce, snapshot fields), so 50
    // risked silent putBytes failures. 20 × ~60 B = 1.2 KB blob,
    // comfortable margin. The StickC's two-button UI is awkward past
    // ~10 entries anyway, so this doubles that for safety. Bump back
    // up here if a future NVS audit shows room.
    static constexpr size_t kMaxEntries = 20;

    PairRegister();

    // Load the persisted register from NVS. Idempotent; safe to call
    // more than once (second call reloads from NVS, discarding any
    // in-memory changes that weren't saved).
    void load();

    // Persist the current in-memory register. Called internally on
    // every mutation; exposed for tests + explicit flush.
    void save() const;

    // Current entry count (0..kMaxEntries).
    size_t count() const { return count_; }

    // Add or update an entry. If an entry with the same `uid` already
    // exists, it is updated in place: secret / role / host / friendly_
    // name are overwritten; captured_at AND last_configured_at are
    // preserved (FIFO - re-pairing does NOT promote). If the register
    // is at capacity and the uid is new, the entry with the lowest
    // captured_at is evicted to make room. Returns true on success.
    bool add(const RegisterEntry& in);

    // Remove the entry matching `uid`. Returns true if found + removed.
    bool remove(uint32_t uid);

    // Lookup by UID. Returns nullptr if not present. The pointer is
    // valid until the next mutation.
    const RegisterEntry* find(uint32_t uid) const;

    // Sort entries by captured_at descending (most-recent-first) into
    // the caller's buffer. Returns the number of entries written
    // (min(count_, cap)).
    size_t list_sorted_by_captured_at(RegisterEntry* out, size_t cap) const;

    // Return the entry at `sorted_index` in the captured-at-descending
    // ordering, or nullptr if out of range. O(count_) per call but no
    // large caller-provided buffer required - avoid the 50-entry stack
    // allocation that caused a canary trigger in Config Lumes (bench
    // 2026-10-09). Pointer valid until the next mutation.
    const RegisterEntry* nth_sorted(size_t sorted_index) const;

    // Mark the given UID as just-configured. Updates last_configured_at
    // to a fresh monotonic sequence, writes NVS. Returns true if found.
    bool mark_configured(uint32_t uid);

    // Overwrite the last-known config snapshot for `uid`. Also bumps
    // last_configured_at. Writes NVS. Returns true if the entry was
    // found. Called from the Paired-fleet write path (ESP-NOW) and the
    // Live-scan write + read path (BLE) so the UI's prefill matches
    // what's actually on the device.
    bool update_snapshot(uint32_t uid, const ConfigSnapshot& snap);

    // Clear in-memory state only (test seam; does NOT touch NVS).
    void clear_in_memory();

private:
    uint32_t next_sequence_();

    RegisterEntry entries_[kMaxEntries] = {};
    size_t        count_          = 0;
    uint32_t      next_sequence_val_ = 1;   // monotonic; 0 reserved for "never"
};

// Process-wide accessor. Lazily loads from NVS on first call.
PairRegister& pair_register();

}  // namespace ble
}  // namespace nocturnation
