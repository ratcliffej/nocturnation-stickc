#include "transport/espnow/tofu_lock.h"

#include <cstdio>

namespace nocturnation {
namespace transport {
namespace espnow {

TofuLock::TofuLock() = default;

bool TofuLock::admit(MessageType msg_type,
                     uint8_t     source_id,
                     uint8_t     channel,
                     uint32_t    now_ms) {
    (void)msg_type;   // display-family no longer needs special handling

    // Broadcast / wildcard source_id is never a valid Director peer.
    // A Director never emits 0xFF as its own id; broadcast is for
    // senders that intentionally identify as anonymous, which a Lume
    // MUST NOT lock to. The Director's bridge passthrough re-stamps
    // upstream-orchestrator display frames with the Director's own
    // source_id before broadcasting (EMF multi-show phase 3) so
    // display traffic now arrives identified - and only frames that
    // match the locked Director's id get admitted via the normal
    // post-lock filter below. This removes the previous "admit
    // BROADCAST display family when locked" carve-out, which would
    // have admitted display frames from ALL Directors at a multi-
    // show venue instead of just the locked one.
    if (source_id == kBroadcastSourceId) {
        return false;
    }

    // Cross-range filter on channel 11: only Performance-range IDs
    // are eligible. A community-range id on ch 11 is a misconfigured
    // Director announcing on the wrong channel; silently drop.
    if (channel == 11 && !is_performance_range(source_id)) {
        return false;
    }

    if (!has_lock_) {
        // First eligible frame establishes the lock.
        has_lock_      = true;
        locked_id_     = source_id;
        last_frame_ms_ = now_ms;
        return true;
    }

    // Already locked: only frames from the locked source proceed.
    if (source_id != locked_id_) {
        return false;
    }

    last_frame_ms_ = now_ms;
    return true;
}

bool TofuLock::tick(uint32_t now_ms) {
    if (!has_lock_) return false;
    // Saturating subtract guards against the on_recv-vs-loop_tick race
    // where last_frame_ms_ briefly leads now_ms by a tick (the same
    // hazard kNoSignalMs guards against in LumeMode).
    const uint32_t age = (now_ms >= last_frame_ms_)
                            ? (now_ms - last_frame_ms_)
                            : 0;
    if (age >= kDefaultTimeoutMs) {
        has_lock_      = false;
        locked_id_     = 0;
        last_frame_ms_ = 0;
        return true;   // expired this tick
    }
    return false;
}

void TofuLock::clear() {
    has_lock_      = false;
    locked_id_     = 0;
    last_frame_ms_ = 0;
}

size_t format_tofu_lock_label(bool    is_locked,
                              uint8_t locked_id_value,
                              char*   buf,
                              size_t  buflen) {
    if (!buf || buflen == 0) return 0;
    if (!is_locked) {
        buf[0] = '\0';
        return 0;
    }
    const char* prefix;
    if (is_performance_range(locked_id_value)) {
        prefix = "P";
    } else if (is_community_range(locked_id_value)) {
        prefix = "C";
    } else {
        prefix = "?";   // defensive - shouldn't happen for a conforming Director
    }
    const int n = std::snprintf(buf, buflen, "%s:%02X",
                                prefix, static_cast<unsigned>(locked_id_value));
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(n);
}

}  // namespace espnow
}  // namespace transport
}  // namespace nocturnation
