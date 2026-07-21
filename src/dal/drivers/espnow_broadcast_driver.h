// EspNowBroadcastDriver - DAL driver for the "esp-now-broadcast" transport.
//
// Encodes the NocturNation ESP-NOW wire format (transport/espnow/frame.h)
// and broadcasts over hal::HAL::esp_now()->send_broadcast(). Replaces the
// per-mode EspNowBroadcaster helper that lived in src/modes/ before Epic
// 4.6 Block 2: Director broadcast logic now lives behind a single DAL
// driver registered under transport "esp-now-broadcast", and orchestration
// reaches the wire via DAL::render_fx("esp-now-broadcast", ev) for
// LIGHT_PULSE, plus driver-specific entry points for protocol concepts
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

    // Per spec §4.3 reliability strategy: each frame goes out N times
    // (initial + N-1 retransmits) with the SAME sequence number,
    // separated by 5-15 ms of pseudo-random jitter. Lume dedup catches
    // the duplicates; the redundancy buys airtime resilience against
    // collisions and brief interference.
    //
    // Bench finding 2026-06-23: bumping from 3 to 5 INCREASED visible
    // pulse dropouts on the M5 fleet at the bench. Counter-intuitive
    // but consistent with airtime saturation - each 5-send burst eats
    // ~40-70 ms of radio time, and at the 7 Hz sparkle rate of
    // sparkle_on_beat the bursts overlap each other plus the wash-
    // refresh traffic. Lumes' WiFi receive queues fill before the
    // main task can drain them, and frames get dropped at the radio
    // IDF layer. 3 sends (~20-30 ms burst) leaves enough quiet
    // between bursts for the receive path to keep up. The EMF noise-
    // floor concern that motivated the bump remains valid for the
    // venue, but 5 isn't the right knob - if RF reliability needs
    // more, the answer is the Lume-repeat mesh setting (Spec §4.3),
    // not louder Director retransmits.
    //
    // Epic 15 2026-06-27: dropped 3 -> 2 alongside the move to
    // ESP-NOW Long Range mode. LR doubles per-frame airtime (500
    // kbps vs 1 Mbps), so 3 retransmits would eat ~80-140 ms per
    // burst at the 7 Hz sparkle rate, deeper into the saturation
    // territory the 5-send bench finding identified. 2 retransmits
    // gives us 1 initial + 1 redundant copy with comfortable margin.
    // If the EMF lighting engineer's park test shows dropouts, the
    // answer is Lume-repeat mesh, not increasing this back to 3.
    static constexpr uint8_t  kRedundantSends     = 2;
    static constexpr uint8_t  kRedundantGapMinMs  = 5;
    static constexpr uint8_t  kRedundantGapMaxMs  = 15;

    // Maximum frame size we ever buffer for retransmit. Was 32 (sized
    // for LIGHT_PULSE at 14 bytes including header); Epic 13 raised
    // the wire ceiling to 250 for TextDisplay (~200 bytes payload at
    // the max) and bitmap planes, and the DMX-bridge passthrough now
    // routes display frames through this driver too (so the retransmit
    // protection applies uniformly to wash/pulse/heartbeat AND display).
    // Set to the transport-level ceiling so no frame size silently
    // skips retransmit.
    static constexpr size_t   kRetransmitBufSize  = 250;

    // Channel 11 Performance-mode listen-before-broadcast (Epic 5.5 B4).
    // Spec §3.4: Director MUST listen for at least one second on its
    // chosen source_id before transmitting; if a HEARTBEAT carrying the
    // same id arrives during that window the candidate must be
    // re-rolled. After kListenMaxAttempts unsuccessful attempts the
    // Director MAY proceed with the last pick and SHOULD log a warning.
    static constexpr uint32_t kListenWindowMs   = 1000;
    static constexpr uint8_t  kListenMaxAttempts = 3;

    // Startup phase of the driver. See B4a design notes (Epic working copy).
    //   Idle      - stop_broadcast / never-started state.
    //   Listening - ch 11 only: radio is RX-capable, TX gated off,
    //               listen_candidate_ held while we watch for collisions.
    //   Active    - normal operation; send_broadcast / heartbeat / retransmits
    //               are all live.
    enum class StartupState : uint8_t {
        Idle      = 0,
        Listening = 1,
        Active    = 2,
    };

    const char* transport_name() const override { return "esp-now-broadcast"; }

    bool begin() override;
    void loop_tick() override;

    // Generic RgbPulse dispatch: encodes LIGHT_PULSE with the active
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
    // LightPulsePayload; target_group at offset 1.
    bool send(uint8_t target_class, uint8_t target_group, const RgbPulseEvent& ev);

    // WASH-family senders (Epic 6C Phase E). Each builds the corresponding
    // wire payload and broadcasts; the receiver-side dispatch (Phase F)
    // gates on BindingCapabilities.can_wash. Returns false when the radio
    // is not currently started or the encode fails.
    bool send_wash      (uint8_t target_class, uint8_t target_group,
                         const LightWashEvent& ev);
    bool send_wash_end  (uint8_t target_class, uint8_t target_group,
                         uint8_t release_time);
    bool send_wash_pulse(uint8_t target_class, uint8_t target_group,
                         const RgbPulseEvent& ev);

    // ---- Driver-specific lifecycle / protocol entry points ----
    //
    // These aren't part of the generic Driver contract; they expose
    // protocol concepts (radio lifecycle, heartbeat, macro music events)
    // that don't have a typed-event home on Driver yet.

    bool start_broadcast(uint8_t channel);
    void stop_broadcast();

    bool active() const { return active_; }
    StartupState startup_state() const { return startup_state_; }

    // Currently-allocated source_id for the active broadcast. Valid
    // once start_broadcast has been called; returns 0 before that.
    // Used by the Director UI (Epic 5.5 B5) to show the operator
    // which ID the audience is locking to.
    uint8_t source_id() const { return source_id_; }

    // The candidate source_id under evaluation during a channel-11
    // listen window. Meaningful only when startup_state_ == Listening;
    // returns 0 otherwise. Used by the status formatter so the
    // operator UI can show the candidate ID (with a tentative suffix)
    // while the driver is still listening for collisions.
    uint8_t listen_candidate() const { return listen_candidate_; }

    // Compose a short status label for the operator screen
    // (Epic 5.5 B5). Format:
    //   Idle      -> empty string, returns 0
    //   Active    -> "C:nn" (community-range source_id)
    //             or "P:nn" (Performance-range source_id)
    //   Listening -> "P:nn?" (candidate, tentative; trailing '?' means
    //                         still listening for collisions)
    //   Out-of-range -> "?:nn" (defensive; shouldn't happen for a
    //                           conforming Director)
    // Returns the number of characters written (excluding the trailing
    // NUL), or 0 if nothing was written (state Idle or buffer too small).
    // Pure function for unit testability; callers pass driver state in.
    static size_t format_status_label(StartupState state,
                                      uint8_t source_id_value,
                                      uint8_t listen_candidate_value,
                                      char* buf,
                                      size_t buflen);

    // Per Epic 5.5 B3: source_id allocation depends on the configured
    // channel. Channel 1 uses the community range (stable per device,
    // persisted via persistence::load_director_source_id). Other
    // channels currently keep the MAC-derived legacy behaviour; B4
    // replaces channel 11 with a random Performance-range pick +
    // listen-before-broadcast. Exposed public for direct unit testing
    // without spinning up the radio or HAL.
    static uint8_t  derive_source_id(uint8_t channel);

    // Epic 13: send an arbitrary pre-encoded ESP-NOW frame buffer
    // (typically a TextDisplay / BitmapHeader / BitmapPlane /
    // ClearScreen that the orchestrator built client-side and the
    // DMX bridge unwrapped from its Enttec passthrough envelope).
    // Reuses the per-spec-§4.3 reliability strategy: initial send +
    // jittered retransmits via the same retransmit queue wash and
    // pulse traffic use. Non-blocking; the queue drains via
    // pump_retransmits() during loop_tick. Label is logged so bench
    // diagnostics can distinguish passthrough from native sends.
    void send_passthrough(const uint8_t* buf, size_t n);

#ifndef ARDUINO
    // ----- Native test seam (compiled only in host builds) -----
    // Listening-state plumbing is awkward to drive through start_broadcast
    // because native test envs don't ship an ESPNow HAL stub - start_broadcast
    // bails on a nullptr radio. These hooks let tests put the driver into
    // Listening directly, then advance the state machine with synthetic
    // time + synthetic inbound HEARTBEATs.
    void test_enter_listening(uint8_t candidate, uint32_t started_ms);
    void test_inject_listen_heartbeat(uint8_t source_id);
    uint8_t test_listen_candidate()          const { return listen_candidate_; }
    bool    test_listen_collision_heard()    const { return listen_collision_heard_; }
    uint8_t test_listen_attempts_remaining() const { return listen_attempts_remaining_; }
    uint32_t test_listen_started_ms()        const { return listen_started_ms_; }
#endif

private:
    static uint32_t redundant_gap_ms();
    static uint8_t  pick_performance_id_random();

    uint8_t next_seq();

    void send_frame_bytes(const uint8_t* buf, size_t n, const char* label);

    // Drain any pending retransmits whose time has come. Runs from
    // loop_tick() so retransmits happen on the main task, never on the
    // ESP-NOW callback.
    void pump_retransmits();

    void send_heartbeat();

    // Director loop_tick equivalent: if no frame has gone out within
    // kHeartbeatPeriodMs, sends one. During music with BEAT_DETECTED
    // / LIGHT_PULSE firing every 350-500 ms, this short-circuits and
    // heartbeat traffic stays at zero.
    bool maybe_send_heartbeat();

    // Channel-11 listen-before-broadcast helpers (Epic 5.5 B4).
    // listen_tick() runs from loop_tick() while startup_state_ == Listening.
    // on_listen_recv() is installed as the HAL recv callback for the
    // duration of the listen window and watches for HEARTBEAT frames
    // carrying our candidate source_id.
    void listen_tick();
    void on_listen_recv(const hal::ESPNowMessage& m);
    void log_listen_collision_warning() const;

    // Unified HAL recv callback installed for the whole broadcast (all
    // channels). Drives listen-window collision detection when Listening,
    // and always tallies headless-repeater census beacons (feed_census).
    void on_recv(const hal::ESPNowMessage& m);
    void feed_census(const hal::ESPNowMessage& m);

    bool         active_      = false;
    StartupState startup_state_ = StartupState::Idle;
    uint8_t   source_id_   = 1;
    uint8_t   seq_num_     = 1;
    uint32_t  last_tx_ms_  = 0;
    // Heartbeat cadence gate. Bumped only after a HEARTBEAT actually
    // fires - independent of general TX traffic. Pre-v0.6 the gate was
    // last_tx_ms_ (skip-if-recent), which suppressed heartbeats under
    // continuous DMX-bridge traffic and left Lumes without a stable
    // 1 Hz anchor. Now heartbeat fires every kHeartbeatPeriodMs
    // regardless of other TX activity (§4.3 tick anchor guarantee).
    uint32_t  last_hb_ms_  = 0;

    // Listen-window state. Only meaningful while startup_state_ == Listening;
    // listen_attempts_remaining_ counts down per re-roll.
    uint8_t   listen_candidate_          = 0;
    uint8_t   listen_attempts_remaining_ = 0;
    bool      listen_collision_heard_    = false;
    uint32_t  listen_started_ms_         = 0;

    // Pending-retransmit state. When a frame is sent for the first time
    // we copy its bytes here and schedule the next 2 sends; pump_retransmits()
    // (called from loop_tick) drains them in order.
    uint8_t   retransmit_buf_[kRetransmitBufSize] = {};
    size_t    retransmit_len_       = 0;
    uint8_t   retransmits_remaining_ = 0;
    uint32_t  next_retransmit_ms_   = 0;
};

EspNowBroadcastDriver* esp_now_broadcast_driver_instance();

#ifndef ARDUINO
namespace test_seam {
// Drive the driver's native-build "synthetic clock" used by now_ms().
// The driver defaults to 0 (time doesn't advance under tests unless
// a test explicitly calls this).
void set_now_ms(uint32_t ms);

// Queue successive return values for pick_performance_id_random().
// First queued value is returned on the first call; empties FIFO.
// When the queue is empty pick_performance_id_random() falls back to
// 0x40 (deterministic floor of the Performance range) so a test that
// forgets to queue doesn't roll random in native.
void queue_next_performance_pick(uint8_t id_in_performance_range);

// Reset all native-test state (now_ms back to 0, pick queue cleared).
void clear_native_driver_state();
}  // namespace test_seam
#endif

}  // namespace dal
}  // namespace nocturnation
