// Per-sender numonce replay-guard (Epic 21 B3c).
//
// Each CONFIG_WRITE frame carries a monotonic numonce. The receiver
// remembers the highest numonce ever seen from a given Director's
// source_id and rejects any later frame whose numonce isn't strictly
// greater. That stops a passive sniffer from capturing a legitimate
// CONFIG_WRITE off the air and replaying it later - the signature
// stays valid but the numonce check fails.
//
// 16 slots is enough headroom for the realistic fleet: a Lume
// typically talks to 1-2 Directors (primary + backup); 16 covers
// multi-Director venues comfortably. Once full, slot eviction is
// strict LRU on access time.
//
// Known limitation: if a Director's slot gets evicted (16+ unique
// senders have touched this Lume) and then an attacker replays an
// old frame from the evicted sender, the Lume treats it as a fresh
// source and accepts. For the audience-merch deployment model
// ([[project-audience-merch-model]]) this isn't practically
// exploitable; if the attack ever matters we persist the whole map
// to NVS (expensive on every CONFIG_WRITE) or grow the table.
//
// Pure logic, no Arduino/ESP-IDF - native tests link the same TU.

#pragma once

#include <cstddef>
#include <cstdint>

namespace nocturnation {
namespace transport {
namespace espnow {

class NumonceLru {
public:
    static constexpr size_t kSlots = 16;

    // Returns true if `numonce` is strictly greater than the last-seen
    // numonce from `source_id`, OR if `source_id` is new (no slot).
    // On accept, updates the slot (and promotes its LRU timestamp).
    // On reject (replay, equal, or sentinel 0), state is unchanged.
    //
    // numonce == 0 is rejected as a reserved sentinel - the Director
    // starts counting from 1 (B4.2), so an incoming 0 is either
    // garbage or a confused sender; drop defensively.
    bool check_and_update(uint16_t source_id, uint64_t numonce);

    // Test seams. Not for firmware use.
    size_t   slots_in_use() const;
    uint64_t last_seen_for(uint16_t source_id) const;   // 0 if not present

private:
    struct Slot {
        uint16_t source_id;
        uint64_t last_seen;
        uint32_t last_accessed;   // monotonic tick; higher = more recent
        bool     valid;
    };
    Slot     slots_[kSlots] = {};
    uint32_t tick_          = 0;   // incremented on every call for LRU ordering
};

}  // namespace espnow
}  // namespace transport
}  // namespace nocturnation
