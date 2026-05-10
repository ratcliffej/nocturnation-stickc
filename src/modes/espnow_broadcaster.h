// EspNowBroadcaster - shared by AutonomousMaster and TestMode (and any future
// mode that wants to push frames over ESP-NOW). Each instance holds its own
// source_id / seq_num / radio-active state and calls HAL::esp_now() under
// the hood. begin/end follow the owning mode's lifecycle; HAL::esp_now()->
// begin() is idempotent so concurrent owners across mode transitions work
// fine.
//
// Stepping stone toward a proper EspNowDriver registered with the DAL behind
// render_fx("esp-now-broadcast", ev). When that lands, per-mode lifecycle
// goes away and the radio sits up for the firmware's life. Until then this
// keeps the broadcast logic in one place rather than duplicated across modes.

#pragma once

#include <cstddef>
#include <cstdint>

#include "dal/dal.h"
#include "effects/effects.h"
#include "transport/espnow/frame.h"

namespace nocturnation {
namespace modes {

struct EspNowBroadcaster {
    static constexpr uint32_t kHeartbeatPeriodMs = 1000;   // 1 Hz alive signal

    // Per spec §4.3 reliability strategy: each frame goes out 3 times
    // total (initial + 2 retransmits) with the SAME sequence number,
    // separated by 5-15 ms of pseudo-random jitter. Slave dedup catches
    // the duplicates; the redundancy buys airtime resilience against
    // collisions and brief interference. Total send burst is ~20-30 ms,
    // well under the inter-beat interval at any musical tempo.
    static constexpr uint8_t  kRedundantSends     = 3;
    static constexpr uint8_t  kRedundantGapMinMs  = 5;
    static constexpr uint8_t  kRedundantGapMaxMs  = 15;

    // Maximum frame size we ever buffer for retransmit. Matches the
    // transport-level cap so the LIGHT_COMMAND frame (largest at 14
    // bytes including header) fits comfortably.
    static constexpr size_t   kRetransmitBufSize  = 32;

    bool      active_      = false;
    uint8_t   source_id_   = 1;
    uint8_t   seq_num_     = 1;
    uint32_t  last_tx_ms_  = 0;

    // Pending-retransmit state. When a frame is sent for the first time
    // we copy its bytes here and schedule the next 2 sends; pump_retransmits()
    // (called from the owning mode's loop_tick) drains them in order.
    uint8_t   retransmit_buf_[kRetransmitBufSize] = {};
    size_t    retransmit_len_       = 0;
    uint8_t   retransmits_remaining_ = 0;
    uint32_t  next_retransmit_ms_   = 0;

    bool begin(uint8_t channel);
    void end();

    static uint8_t derive_source_id();

    uint8_t next_seq();

    void send_frame_bytes(const uint8_t* buf, size_t n, const char* label);

    // Drain any pending retransmits whose time has come. Called from the
    // owning mode's loop_tick (so we run on the main task, never the
    // ESP-NOW callback).
    void pump_retransmits();

    static uint32_t redundant_gap_ms();

    void send_beat(float strength_rms, float bpm);

    void send_heartbeat();

    // MUSIC_EVENT (0x06): macro-level musical events fired by the
    // master's drop detector (Epic 4.5 Block 4). Wire-byte values
    // match analyser::DropEvent and transport::espnow::MusicEventType,
    // so callers can pass the analyser's enum directly.
    void send_music_event(transport::espnow::MusicEventType event_type);

    void send_light_command(uint8_t target_group,
                            uint8_t r, uint8_t g, uint8_t b,
                            effects::PulseEnvelope env,
                            pixmob::Chance chance);

    // Convenience: encode + send LIGHT_COMMAND from an RgbPulseEvent. Used
    // by Test mode test fires - same RgbPulseEvent that drives the local
    // screen + IR also hits the wire so slaves render in sync.
    void send_light_command(uint8_t target_group, const dal::RgbPulseEvent& ev);

    // Master loop_tick calls this every iteration. If no frame has gone
    // out within kHeartbeatPeriodMs, sends one. During music with
    // BEAT_DETECTED firing every 350-500 ms, this short-circuits and
    // heartbeat traffic stays at zero.
    bool maybe_send_heartbeat();
};

}  // namespace modes
}  // namespace nocturnation
