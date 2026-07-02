// TofuLock - Trust-On-First-Use lock on Director source_id (port of
// the Tildagon's nocturnation/tofu.py to StickC firmware, EMF 2026
// prep). A Lume locks to the source_id of the first valid frame it
// sees after scan or rescan, then ignores frames from any other
// source_id for the session. The lock clears on:
//
//   * heartbeat timeout (no frames from the locked source for
//     kDefaultTimeoutMs, ten seconds - matches LumeMode::kRescanMs so
//     channel re-scan and TOFU re-lock fire on the same edge)
//   * explicit clear() call (operator "Rescan" menu, channel change,
//     mode re-entry - all expressed through the same hook)
//
// Cross-range filter per protocol manual section 3.4: on channel 11
// only Performance-range source IDs (0x40..0xFE) are eligible to be
// locked; community-range IDs on channel 11 are silently dropped.
// Channels 1 and 6 accept any non-broadcast source_id.
//
// Display-family carve-out (Epic 13): TEXT_DISPLAY, BITMAP_HEADER,
// BITMAP_PLANE, and CLEAR_SCREEN frames have source_id = BROADCAST
// (0xFF) because the orchestrator isn't a Director - it's an
// upstream sender bridged through the Director Stick's passthrough.
// admit() admits these once a Director lock is held, without
// resetting the liveness timer (heartbeat/wash/pulse from the locked
// source remain the only liveness signal).
//
// Why this exists on the StickC at all: previously only the Tildagon
// had TOFU. A StickC-based Lume (Plus2 / S3 / Atom Lite with strip)
// accepted frames from any Director on its channel. At a multi-show
// venue like EMF that meant rendering both shows' content
// simultaneously - "scrambled view". This port partitions StickC
// Lumes the same way Tildagons partition.
//
// Pure logic - no I/O, no clock; the caller injects now_ms and the
// current channel. Unit-tested host-side; runs unchanged on the
// device.

#pragma once

#include <cstdint>

#include "transport/espnow/frame.h"

namespace nocturnation {
namespace transport {
namespace espnow {

class TofuLock {
public:
    // Inactivity timeout. Mirrors the Tildagon's DEFAULT_TIMEOUT_MS
    // and the StickC LumeMode's kRescanMs so the channel re-scan and
    // TOFU lock-expiry fire on the same temporal boundary.
    static constexpr uint32_t kDefaultTimeoutMs = 10000;

    TofuLock();

    // Decide whether `frame_header` should be dispatched to renderers.
    // Returns true if the frame passes TOFU + cross-range checks
    // (caller proceeds with normal observation / rendering). Returns
    // false if it must be silently dropped. Side effects:
    //
    //   * Establishes the lock on the first eligible frame.
    //   * Updates the "last frame from locked source" timestamp on
    //     every admitted frame from the locked source - the
    //     liveness signal that drives kDefaultTimeoutMs expiry.
    //   * Display-family broadcasts (source_id = 0xFF + message
    //     type in {TextDisplay, BitmapHeader, BitmapPlane,
    //     ClearScreen}) are admitted when the lock is held, but do
    //     NOT update the liveness timer (they're orchestrator-
    //     origin content, not Director identity).
    bool admit(MessageType msg_type,
               uint8_t     source_id,
               uint8_t     channel,
               uint32_t    now_ms);

    // Expire the lock on inactivity. Returns true ONCE on the
    // expiring edge (so the caller can log / surface a status
    // change). Returns false thereafter until a new lock is
    // established. Idempotent: calling tick() repeatedly past the
    // timeout returns true once and false after.
    bool tick(uint32_t now_ms);

    // Explicit unlock. Used for operator-initiated "Rescan" menu
    // actions, channel changes, mode re-entry, and similar
    // discontinuities.
    void clear();

    // Observers.
    bool    is_locked() const { return has_lock_; }
    uint8_t locked_id() const { return locked_id_; }   // undefined when !is_locked()

private:
    bool     has_lock_       = false;
    uint8_t  locked_id_      = 0;
    uint32_t last_frame_ms_  = 0;
};

// Compose a short status label for the operator screen. Mirrors the
// Tildagon's format_lock_label so the operator sees the same
// "C:nn" / "P:nn" convention on both Director and Lume screens.
//
//   * Idle (no lock)                              -> "" (empty), returns 0
//   * Locked, Performance-range (0x40..0xFE)      -> "P:nn"
//   * Locked, community-range  (0x00..0x3F)       -> "C:nn"
//   * Locked, out-of-range (defensive)            -> "?:nn"
//
// Returns the number of characters written (excluding the trailing
// NUL), or 0 if nothing was written.
size_t format_tofu_lock_label(bool     is_locked,
                              uint8_t  locked_id_value,
                              char*    buf,
                              size_t   buflen);

}  // namespace espnow
}  // namespace transport
}  // namespace nocturnation
