// Per-sender numonce replay-guard — see numonce_lru.h.

#include "transport/espnow/numonce_lru.h"

namespace nocturnation {
namespace transport {
namespace espnow {

bool NumonceLru::check_and_update(uint16_t source_id, uint64_t numonce) {
    if (numonce == 0) return false;   // reserved sentinel

    ++tick_;

    // First pass: find an existing slot for this source.
    for (size_t i = 0; i < kSlots; ++i) {
        if (slots_[i].valid && slots_[i].source_id == source_id) {
            if (numonce <= slots_[i].last_seen) return false;   // replay
            slots_[i].last_seen     = numonce;
            slots_[i].last_accessed = tick_;
            return true;
        }
    }

    // New source — try a free slot first, else evict the LRU slot.
    size_t target = kSlots;
    for (size_t i = 0; i < kSlots; ++i) {
        if (!slots_[i].valid) { target = i; break; }
    }
    if (target == kSlots) {
        uint32_t oldest = slots_[0].last_accessed;
        target = 0;
        for (size_t i = 1; i < kSlots; ++i) {
            if (slots_[i].last_accessed < oldest) {
                oldest = slots_[i].last_accessed;
                target = i;
            }
        }
    }

    slots_[target].source_id     = source_id;
    slots_[target].last_seen     = numonce;
    slots_[target].last_accessed = tick_;
    slots_[target].valid         = true;
    return true;
}

size_t NumonceLru::slots_in_use() const {
    size_t n = 0;
    for (size_t i = 0; i < kSlots; ++i) if (slots_[i].valid) ++n;
    return n;
}

uint64_t NumonceLru::last_seen_for(uint16_t source_id) const {
    for (size_t i = 0; i < kSlots; ++i) {
        if (slots_[i].valid && slots_[i].source_id == source_id) {
            return slots_[i].last_seen;
        }
    }
    return 0;
}

}  // namespace espnow
}  // namespace transport
}  // namespace nocturnation
