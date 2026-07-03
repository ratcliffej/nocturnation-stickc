// RepeaterCensus implementation.

#include "repeater_census.h"

namespace nocturnation {
namespace dal {

void RepeaterCensus::note(const uint8_t uid[3], uint8_t channel,
                          uint32_t relayed, uint16_t uptime_s,
                          uint32_t now_ms) {
    // Update an existing entry if we already know this uid.
    for (size_t i = 0; i < kMaxRepeaters; ++i) {
        if (entries_[i].used && uid_eq(entries_[i].uid, uid)) {
            entries_[i].channel      = channel;
            entries_[i].relayed      = relayed;
            entries_[i].uptime_s     = uptime_s;
            entries_[i].last_seen_ms = now_ms;
            return;
        }
    }

    // New uid: claim a free slot, else recycle the least-recently-heard.
    size_t victim   = 0;
    uint32_t oldest = 0;
    bool found_free = false;
    for (size_t i = 0; i < kMaxRepeaters; ++i) {
        if (!entries_[i].used) {
            victim     = i;
            found_free = true;
            break;
        }
        const uint32_t age = now_ms - entries_[i].last_seen_ms;
        if (age >= oldest) {
            oldest = age;
            victim = i;
        }
    }
    (void)found_free;

    entries_[victim].uid[0]       = uid[0];
    entries_[victim].uid[1]       = uid[1];
    entries_[victim].uid[2]       = uid[2];
    entries_[victim].channel      = channel;
    entries_[victim].relayed      = relayed;
    entries_[victim].uptime_s     = uptime_s;
    entries_[victim].last_seen_ms = now_ms;
    entries_[victim].used         = true;
}

size_t RepeaterCensus::count_online(uint32_t now_ms) const {
    size_t n = 0;
    for (size_t i = 0; i < kMaxRepeaters; ++i) {
        if (entries_[i].used &&
            (now_ms - entries_[i].last_seen_ms) <= kOnlineWindowMs) {
            ++n;
        }
    }
    return n;
}

RepeaterCensus& repeater_census_instance() {
    static RepeaterCensus s_instance;
    return s_instance;
}

}  // namespace dal
}  // namespace nocturnation
