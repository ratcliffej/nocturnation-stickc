// RepeaterCensus - Director-side tally of headless repeaters on air.
//
// Headless repeaters broadcast a REPEATER_HEARTBEAT (msg 0x0D) ~1 Hz. The
// Director's ESP-NOW receive path feeds each one here, keyed on the 3-byte
// uid the repeater derives from its STA MAC. count_online() then answers
// "how many repeaters are relaying my signal right now" for the console.
//
// Pure logic - no radio, no clock of its own (the caller passes a
// millisecond timestamp). Fixed-size, no heap. Host-testable.

#pragma once

#include <cstddef>
#include <cstdint>

namespace nocturnation {
namespace dal {

class RepeaterCensus {
public:
    // Capacity of the tracked set. More repeaters than this and the
    // least-recently-heard entry is recycled - fine for a census whose
    // job is a live count, not a permanent registry.
    static constexpr size_t kMaxRepeaters = 16;

    // A repeater not heard from within this window is treated as offline.
    // Repeaters beacon at ~1 Hz, so 5 s tolerates a few dropped census
    // frames before a repeater drops off the count.
    static constexpr uint32_t kOnlineWindowMs = 5000;

    struct Entry {
        uint8_t  uid[3]       = {0, 0, 0};
        uint8_t  channel      = 0;
        uint32_t relayed      = 0;
        uint16_t uptime_s     = 0;
        uint32_t last_seen_ms = 0;
        bool     used         = false;
    };

    // Record a census beacon from a repeater. Updates the matching uid
    // entry, or claims a free / least-recently-heard slot for a new one.
    void note(const uint8_t uid[3], uint8_t channel, uint32_t relayed,
              uint16_t uptime_s, uint32_t now_ms);

    // How many distinct repeaters have beaconed within kOnlineWindowMs.
    size_t count_online(uint32_t now_ms) const;

    // Read access for the console listing. Iterate [0, capacity); skip
    // entries whose `used` is false or that are stale per count_online's
    // window (caller decides).
    const Entry* entries() const { return entries_; }
    static constexpr size_t capacity() { return kMaxRepeaters; }

private:
    static bool uid_eq(const uint8_t a[3], const uint8_t b[3]) {
        return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
    }

    Entry entries_[kMaxRepeaters];
};

// Process-wide singleton, mirroring the other dal driver instances.
RepeaterCensus& repeater_census_instance();

}  // namespace dal
}  // namespace nocturnation
