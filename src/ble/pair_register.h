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
// the oldest entry by captured_at (LRU). For fleets bigger than ~10
// entries the StickC UI becomes awkward anyway ([[project-device-
// uid-design]] + Jason 2026-10-07 note) and the fleet should migrate
// to a phone/laptop register as a near-term follow-on epic.
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
struct RegisterEntry {
    uint32_t uid;                         // CRC32-of-STA-MAC (identity)
    uint8_t  secret[16];                  // HMAC-SHA256 key
    uint8_t  role;                        // ble::Role as u8
    uint8_t  host;                        // ble::Host as u8
    char     friendly_name[21];           // NUL-terminated, 20 chars max
    uint32_t captured_at;                 // monotonic sequence; higher = more recent
    uint32_t last_configured_at;          // 0 if never written to; monotonic sequence
};

class PairRegister {
public:
    static constexpr size_t kMaxEntries = 50;

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
    // name are overwritten, captured_at bumps to the new monotonic
    // sequence (so re-capture promotes it in the LRU ordering), and
    // last_configured_at is preserved. If the register is at capacity
    // and the uid is new, the entry with the lowest captured_at is
    // evicted to make room. Returns true on success.
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

    // Mark the given UID as just-configured. Updates last_configured_at
    // to a fresh monotonic sequence, writes NVS. Returns true if found.
    bool mark_configured(uint32_t uid);

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
