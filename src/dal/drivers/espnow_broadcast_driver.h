// EspNowBroadcastDriver - DAL driver for the "esp-now-broadcast" transport.
//
// Encodes the NocturNation ESP-NOW wire format (transport/espnow/frame.h)
// and broadcasts over hal::HAL::esp_now()->send_broadcast(). Replaces the
// per-mode EspNowBroadcaster helper that lived in src/modes/ before Epic
// 4.6 Block 2: Director broadcast logic now lives behind a single DAL
// driver registered under transport "esp-now-broadcast", and orchestration
// reaches the wire via DAL::render_fx("esp-now-broadcast", ev) for
// LIGHT_COMMAND, plus driver-specific entry points for protocol concepts
// (heartbeat, music events, lifecycle) that don't fit the generic Driver
// surface.
//
// The driver's begin() returns true whenever hal::HAL::esp_now() exists -
// that gates registration only. The radio itself is started by
// start_broadcast(channel) from the mode's enter() and torn down by
// stop_broadcast() from exit(); active_ gates all output. The driver's
// loop_tick() drives the per-spec §4.3 redundant retransmits and the 1 Hz
// heartbeat skip-if-recent logic, so modes no longer need to forward
// those calls from their own loop_tick.

#pragma once

#include <cstddef>
#include <cstdint>

#include "dal/dal.h"
#include "transport/espnow/frame.h"

namespace nocturnation {
namespace dal {

class EspNowBroadcastDriver : public Driver {
public:
    static constexpr uint32_t kHeartbeatPeriodMs = 1000;   // 1 Hz alive signal

    // Per spec §4.3 reliability strategy: each frame goes out 3 times
    // total (initial + 2 retransmits) with the SAME sequence number,
    // separated by 5-15 ms of pseudo-random jitter. Lume dedup catches
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

    const char* transport_name() const override { return "esp-now-broadcast"; }

    bool begin() override;
    void loop_tick() override;

    // Generic RgbPulse dispatch: encodes LIGHT_COMMAND with the active
    // device's group_id and broadcasts. Returns false when the radio is
    // not currently started (active_ == false) or the encode/send fails.
    // Legacy entry point: forwards to send(0, group_id, ev) with
    // target_class set to All. Block 7 migrates vis call sites to the
    // structured "<class>:<group>" target form which routes through the
    // 3-arg overload below.
    bool send(uint8_t group_id, const RgbPulseEvent& ev) override;

    // Class+group dispatch (Epic 4.65 Block 4). The structured target
    // string "<hex_class>:<hex_group>" passed to DAL::render_fx parses
    // into these two bytes and bypasses the device-name lookup, going
    // straight through here. target_class lands at offset 0 of
    // LightCommandPayload; target_group at offset 1.
    bool send(uint8_t target_class, uint8_t target_group, const RgbPulseEvent& ev);

    // ---- Driver-specific lifecycle / protocol entry points ----
    //
    // These aren't part of the generic Driver contract; they expose
    // protocol concepts (radio lifecycle, heartbeat, macro music events)
    // that don't have a typed-event home on Driver yet.

    bool start_broadcast(uint8_t channel);
    void stop_broadcast();

    bool active() const { return active_; }

    // Currently-allocated source_id for the active broadcast. Valid
    // once start_broadcast has been called; returns 0 before that.
    // Used by the Director UI (Epic 5.5 B5) to show the operator
    // which ID the audience is locking to.
    uint8_t source_id() const { return source_id_; }

    // Per Epic 5.5 B3: source_id allocation depends on the configured
    // channel. Channel 1 uses the community range (stable per device,
    // persisted via persistence::load_director_source_id). Other
    // channels currently keep the MAC-derived legacy behaviour; B4
    // replaces channel 11 with a random Performance-range pick +
    // listen-before-broadcast. Exposed public for direct unit testing
    // without spinning up the radio or HAL.
    static uint8_t  derive_source_id(uint8_t channel);

private:
    static uint32_t redundant_gap_ms();

    uint8_t next_seq();

    void send_frame_bytes(const uint8_t* buf, size_t n, const char* label);

    // Drain any pending retransmits whose time has come. Runs from
    // loop_tick() so retransmits happen on the main task, never on the
    // ESP-NOW callback.
    void pump_retransmits();

    void send_heartbeat();

    // Director loop_tick equivalent: if no frame has gone out within
    // kHeartbeatPeriodMs, sends one. During music with BEAT_DETECTED
    // / LIGHT_COMMAND firing every 350-500 ms, this short-circuits and
    // heartbeat traffic stays at zero.
    bool maybe_send_heartbeat();

    bool      active_      = false;
    uint8_t   source_id_   = 1;
    uint8_t   seq_num_     = 1;
    uint32_t  last_tx_ms_  = 0;

    // Pending-retransmit state. When a frame is sent for the first time
    // we copy its bytes here and schedule the next 2 sends; pump_retransmits()
    // (called from loop_tick) drains them in order.
    uint8_t   retransmit_buf_[kRetransmitBufSize] = {};
    size_t    retransmit_len_       = 0;
    uint8_t   retransmits_remaining_ = 0;
    uint32_t  next_retransmit_ms_   = 0;
};

EspNowBroadcastDriver* esp_now_broadcast_driver_instance();

}  // namespace dal
}  // namespace nocturnation
