// EspNowBroadcastDriver - DAL driver for the "esp-now-broadcast" transport.
//
// Encodes the NocturNation ESP-NOW wire format (transport/espnow/frame.h)
// and broadcasts over hal::HAL::esp_now()->send_broadcast(). The radio is
// started by start_broadcast(channel) from a mode's enter() and torn down
// by stop_broadcast() from exit(); active_ gates all output. loop_tick()
// drives the per-spec §4.3 redundant retransmits and the 1 Hz heartbeat.

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

    // Per spec §4.3: each frame goes out N times with the SAME sequence
    // number, separated by 5-15 ms of pseudo-random jitter, for airtime
    // resilience. Lume dedup catches the duplicates. Current N chosen
    // conservatively - louder retransmits saturate airtime; see
    // docs/stickc-history.md for the 3→5→2 bench history and the
    // Lume-repeat-mesh alternative for range.
    static constexpr uint8_t  kRedundantSends     = 2;
    static constexpr uint8_t  kRedundantGapMinMs  = 5;
    static constexpr uint8_t  kRedundantGapMaxMs  = 15;

    // Set to the transport-level ceiling so no frame size silently skips
    // retransmit (TextDisplay + bitmap planes reach ~200 bytes).
    static constexpr size_t   kRetransmitBufSize  = 250;

    // Channel 11 listen-before-broadcast (spec §3.4). Director MUST listen
    // for at least one second on its chosen source_id before transmitting;
    // a HEARTBEAT with the same id during that window forces a re-roll.
    // After kListenMaxAttempts consecutive collisions the Director MAY
    // proceed and SHOULD log a warning.
    static constexpr uint32_t kListenWindowMs   = 1000;
    static constexpr uint8_t  kListenMaxAttempts = 3;

    //   Idle      - stop_broadcast / never-started state.
    //   Listening - ch 11 only: RX-capable, TX gated off, listen_candidate_
    //               held while we watch for collisions.
    //   Active    - send_broadcast / heartbeat / retransmits are live.
    enum class StartupState : uint8_t {
        Idle      = 0,
        Listening = 1,
        Active    = 2,
    };

    const char* transport_name() const override { return "esp-now-broadcast"; }

    bool begin() override;
    void loop_tick() override;

    // Legacy single-group entry: target_class = All. Forwards to the
    // 3-arg overload; kept for existing DAL device-name call sites.
    bool send(uint8_t group_id, const RgbPulseEvent& ev) override;

    // Class+group dispatch. render_fx parses "<hex_class>:<hex_group>"
    // and calls straight through here. target_class = offset 0 of
    // LightPulsePayload; target_group = offset 1.
    bool send(uint8_t target_class, uint8_t target_group, const RgbPulseEvent& ev);

    // WASH-family senders. Return false when the radio is not started or
    // the encode fails. Receiver-side gates on BindingCapabilities.can_wash.
    bool send_wash      (uint8_t target_class, uint8_t target_group,
                         const LightWashEvent& ev);
    bool send_wash_end  (uint8_t target_class, uint8_t target_group,
                         uint8_t release_time);
    bool send_wash_pulse(uint8_t target_class, uint8_t target_group,
                         const RgbPulseEvent& ev);

    // Driver-specific lifecycle / protocol entry points (not part of the
    // generic Driver contract).
    bool start_broadcast(uint8_t channel);
    void stop_broadcast();

    bool active() const { return active_; }
    StartupState startup_state() const { return startup_state_; }

    // Currently-allocated source_id for the active broadcast. Valid once
    // start_broadcast has been called; returns 0 before that.
    uint8_t source_id() const { return source_id_; }

    // Candidate source_id under evaluation during a ch11 listen window.
    // Meaningful only when startup_state_ == Listening.
    uint8_t listen_candidate() const { return listen_candidate_; }

    // Compose a short status label for the operator screen. Format:
    //   Idle      -> empty string, returns 0
    //   Active    -> "C:nn" (community) or "P:nn" (Performance)
    //   Listening -> "P:nn?" (candidate, tentative)
    //   Out-of-range -> "?:nn" (defensive)
    // Returns chars written (excluding NUL), or 0 on Idle / small buffer.
    // Pure function so callers can test it without the driver.
    static size_t format_status_label(StartupState state,
                                      uint8_t source_id_value,
                                      uint8_t listen_candidate_value,
                                      char* buf,
                                      size_t buflen);

    // Channel 1 uses the community range (stable per device, persisted).
    // Channel 6 stays on MAC-derived. Channel 11 uses the random
    // Performance-range + listen-before-broadcast path in start_broadcast.
    // Exposed public for direct unit testing.
    static uint8_t  derive_source_id(uint8_t channel);

    // Send an arbitrary pre-encoded ESP-NOW frame (orchestrator-built
    // TextDisplay / Bitmap* / ClearScreen unwrapped by the DMX bridge).
    // Reuses the §4.3 retransmit queue. Non-blocking; drains via
    // pump_retransmits() during loop_tick. See docs/stickc-history.md
    // for the source_id + sequence_number re-stamp rationale.
    void send_passthrough(const uint8_t* buf, size_t n);

#ifndef ARDUINO
    // ----- Native test seam (compiled only in host builds) -----
    // start_broadcast bails on a nullptr radio in native envs, so these
    // hooks push the driver into Listening directly and advance the state
    // machine with synthetic time + synthetic inbound HEARTBEATs.
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

    // Drains pending retransmits from loop_tick (main task) so we never
    // send from the ESP-NOW callback context.
    void pump_retransmits();

    void send_heartbeat();

    // Unconditional 1 Hz per §4.3 tick-anchor guarantee. Gates on
    // last_hb_ms_ (heartbeat-only) not last_tx_ms_ (any-TX) so
    // continuous traffic can't suppress the anchor.
    bool maybe_send_heartbeat();

    // Channel-11 listen-before-broadcast helpers. listen_tick() runs from
    // loop_tick() while startup_state_ == Listening. on_listen_recv() is
    // installed as the HAL recv callback for the listen window and
    // watches for HEARTBEATs carrying our candidate source_id.
    void listen_tick();
    void on_listen_recv(const hal::ESPNowMessage& m);
    void log_listen_collision_warning() const;

    // Unified HAL recv callback installed for the whole broadcast:
    // drives listen-window collision detection when Listening, and
    // always tallies headless-repeater census beacons.
    void on_recv(const hal::ESPNowMessage& m);
    void feed_census(const hal::ESPNowMessage& m);

    bool         active_      = false;
    StartupState startup_state_ = StartupState::Idle;
    uint8_t   source_id_   = 1;
    uint8_t   seq_num_     = 1;
    uint32_t  last_tx_ms_  = 0;
    // Heartbeat cadence gate: bumped only when a HEARTBEAT actually
    // fires, so general TX traffic can't suppress the 1 Hz anchor.
    uint32_t  last_hb_ms_  = 0;

    uint8_t   listen_candidate_          = 0;
    uint8_t   listen_attempts_remaining_ = 0;
    bool      listen_collision_heard_    = false;
    uint32_t  listen_started_ms_         = 0;

    uint8_t   retransmit_buf_[kRetransmitBufSize] = {};
    size_t    retransmit_len_       = 0;
    uint8_t   retransmits_remaining_ = 0;
    uint32_t  next_retransmit_ms_   = 0;
};

EspNowBroadcastDriver* esp_now_broadcast_driver_instance();

#ifndef ARDUINO
namespace test_seam {
// Drives the native-build synthetic clock used by now_ms() (defaults to 0).
void set_now_ms(uint32_t ms);

// FIFO of return values for pick_performance_id_random(). When empty,
// pick_performance_id_random() falls back to 0x40 (deterministic floor)
// so a test that forgets to queue doesn't roll random in native.
void queue_next_performance_pick(uint8_t id_in_performance_range);

void clear_native_driver_state();
}  // namespace test_seam
#endif

}  // namespace dal
}  // namespace nocturnation
