// LumeMode - ESP-NOW receive, decode + fan out to active OutputBindings.
//
// Audio analyser stays dormant in Lume mode (spec §8.2 power optimisation).
// LumeMode never calls DAL::start_audio_input(), so LocalDriver never
// enables the mic and the whole FFT / BeatDetector / DropDetector /
// SectionDetector chain stays off - the pipeline is mic-driven, so
// withholding capture cuts off everything downstream.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modes/mode_machine.h"
#include "hal/hal.h"
#include "output_bindings/output_binding.h"

// Compile-time gate on the Atom-Lite Btn1 LongPressed group-cycle
// gesture. Set to 0 in build_flags to lock the group at flash-time -
// the handler compiles out entirely and only NOCT_DEFAULT_LUME_GROUP
// / clearing NVS can change slv_group.
#ifndef NOCT_LUME_GROUP_LONGPRESS_ENABLED
#define NOCT_LUME_GROUP_LONGPRESS_ENABLED 1
#endif
#include "output_bindings/output_binding_context.h"
#include "transport/espnow/frame.h"
#include "transport/espnow/tofu_lock.h"
#include "transport/quality.h"

namespace nocturnation {
namespace modes {

class LumeMode : public Mode {
public:
    ModeId id() const override { return ModeId::Lume; }
    const char* name() const override { return "Lume"; }

    void enter() override;
    void exit() override;
    void loop_tick() override;
    void on_button_event(const dal::ButtonPressEvent& ev) override;

private:
    // 3x the Director's 1 Hz heartbeat. Lumes must never auto-promote
    // on this edge - a rogue promoted Lume would compete with the real
    // Director when it returns and ruin show coordination.
    // See docs/stickc-history.md.
    static constexpr uint32_t kNoSignalMs          = 3000;

    // Signal-loss fallback wash. Anchored to age_since_rx:
    //   3 s  -> NO SIGNAL diagnostic text
    //  10 s -> synthesise a muted blue<->purple LIGHT_WASH locally
    //  40 s -> LIGHT_WASH_END with 25.5 s release (u8 max in 100 ms units)
    // Any inbound frame cancels via a short-release LIGHT_WASH_END so
    // the returning Director's traffic doesn't compete with the
    // synthetic baseline. See docs/stickc-history.md.
    static constexpr uint32_t kFallbackEnterMs       = 10000;
    static constexpr uint32_t kFallbackFadeStartMs   = 40000;
    static constexpr uint16_t kFallbackCyclePeriodMs = 10000;   // blue<->purple ping-pong period
    static constexpr uint8_t  kFallbackAttackTicks   = 30;      // 100 ms units = 3 s fade-in
    static constexpr uint8_t  kFallbackFadeTicks     = 255;     // 100 ms units = ~25.5 s (u8 cap)
    static constexpr uint8_t  kFallbackIntensity     = 60;      // ~24 % - "alive but quiet"
    static constexpr uint8_t  kFallbackRecoveryTicks = 5;       // 100 ms units = 500 ms fade-out

    // Muted so the fallback reads as a gentle breathing field.
    static constexpr uint8_t  kFallbackColourA[3] = { 20,  0, 80 };   // dark violet
    static constexpr uint8_t  kFallbackColourB[3] = {  0, 20, 80 };   // dark navy

    // 38x12 px status pip anchored top-right. Paints OVER the full-screen
    // pulse rect on each refresh; 10 Hz refresh reads as steady against
    // ~30 Hz pulse repaint. See docs/stickc-history.md.
    static constexpr int      kPipWidth            = 38;
    static constexpr int      kPipHeight           = 12;
    static constexpr int      kPipX                = 240 - kPipWidth;
    static constexpr int      kPipY                = 0;
    static constexpr uint32_t kPipRefreshMs        = 100;

    bool      radio_active_       = false;
    uint32_t  rx_count_           = 0;
    uint32_t  last_rx_ms_         = 0;
    uint32_t  last_draw_ms_       = 0;
    uint32_t  last_pip_draw_ms_   = 0;
    uint16_t  last_source_id_     = 0;   // v3: widened to u16
    uint8_t   last_msg_type_      = 0xFF;
    bool      no_signal_          = false;   // sticky once threshold crossed

    // Director-clock offset (Phase 1 of §4.3 tick anchor). Each admitted
    // HEARTBEAT carries the Director's millisecond tick; we track
    // (tick - local_ms) smoothed exponentially so Phase 2 can rewire
    // envelope ASR clocks off it. director_offset_valid_ resets to
    // false on TOFU relock (a new Director's clock is unrelated to
    // the old one's). See docs/stickc-history.md.
    bool      director_offset_valid_     = false;
    int32_t   director_tick_offset_ms_    = 0;
    uint16_t  director_offset_source_id_  = 0;   // v3: widened to u16; detects TOFU relock

    // fallback_active_ set when we synthesised the LIGHT_WASH;
    // fallback_faded_ set when we emitted the LIGHT_WASH_END. Both
    // clear on the signal-recovery edge in on_recv.
    bool      fallback_active_    = false;
    bool      fallback_faded_     = false;

    // Local-only fan-out: builds a synthetic wash payload as if the
    // Director had emitted it. Never touches the radio.
    void emit_fallback_wash_start();
    void emit_fallback_wash_fade();
    void emit_fallback_wash_recovery();

    // Stamped whenever signal is (re-)acquired. 0 = never acquired.
    // Drives the "freshly locked" indication on the LED strip overlay.
    uint32_t  lock_acquired_ms_   = 0;
    static constexpr uint32_t kFreshLockMs           = 1000;
    static constexpr uint32_t kIndicatorFlashPeriodMs = 1000;     // 1 Hz, 50 % duty

    // Bindings whose required_capabilities() are a subset of host_caps()
    // get added at enter() paired with their own context; drained in
    // exit(). LIGHT_PULSE fans out via loop_tick to every entry.
    struct ActiveBinding {
        output_bindings::OutputBinding*        binding = nullptr;
        output_bindings::OutputBindingContext* ctx     = nullptr;
    };
    static constexpr size_t kMaxActiveBindings = 8;
    std::array<ActiveBinding, kMaxActiveBindings> active_bindings_{};
    size_t                                        active_binding_count_ = 0;

    // Channel preference: 0 = auto scan (11 show -> 1 hobby -> 6 custom,
    // 2 s dwell); 1/6/11 = locked. Locked Lumes never re-scan on signal
    // loss. Re-scan gate (kRescanMs) is decoupled from kNoSignalMs so a
    // brief Director outage doesn't force a full channel hunt.
    uint8_t   lume_channel_pref_   = 0;
    uint8_t   current_listen_chan_  = 1;
    uint32_t  last_chan_switch_ms_  = 0;
    static constexpr uint32_t kChannelDwellMs = 2000;
    static constexpr uint8_t  kScanOrder[3]      = {11, 1, 6};
    static constexpr size_t   kScanOrderCount    = 3;
    static constexpr uint32_t kRescanMs           = 10000;

    // NocturNation group ID for receive filtering. 0 = respond to
    // everything. Operator sets via Config > Lume > Group. Distinct
    // from the per-PixMobIrBinding `group` property (which is the
    // PixMob protocol's IR group code).
    uint8_t   lume_group_            = 0;

    // Btn1-LongPressed group-cycle confirmation. flash_group_remaining_
    // = N pulses left in the sequence; each pair of ticks toggles
    // flash_on_ and decrements on the off half.
    uint8_t   flash_group_remaining_  = 0;
    uint32_t  flash_next_edge_ms_     = 0;
    bool      flash_on_               = false;
    static constexpr uint32_t kGroupFlashHalfPeriodMs = 250;

    // Transport-agnostic - could feed off any sequenced protocol.
    transport::SignalQuality quality_;

    // Partitions the Lume between multiple Directors on the same channel.
    // First-eligible-frame establishes the lock; frames from any other
    // source_id are dropped. Expires after 10 s of silence (matches
    // kRescanMs so channel rescan and TOFU relock fire on the same
    // edge). See docs/stickc-history.md.
    transport::espnow::TofuLock tofu_;

    // Deferred queues drained by loop_tick. The ESP-NOW callback runs on
    // the WiFi task; IRsend and radio send_broadcast from that context
    // crash the S3. See docs/stickc-history.md.
    volatile bool                            pending_light_ = false;
    transport::espnow::LightPulsePayload   pending_light_payload_{};

    // Per §4.3: rebroadcast each unique inbound frame once with
    // hop_count incremented, up to kMaxHopCount. Source_id +
    // sequence_number preserved exactly so mesh-wide dedup works.
    bool          lume_repeat_en_      = false;
    volatile bool pending_repeat_       = false;
    size_t        pending_repeat_len_   = 0;
    // Matches transport::espnow::kMaxFrameSize (BITMAP_PLANE peaks ~240 B).
    static constexpr size_t kRepeatBufSize  = 250;
    static constexpr uint8_t kMaxHopCount   = 3;
    uint8_t       pending_repeat_buf_[kRepeatBufSize] = {};

    // repeats_emitted_: successful send_broadcast calls from the drain.
    // last_drawn_repeats_: LCD cache; only repaint on change so every
    // relay doesn't hammer the SPI bus.
    uint32_t      repeats_emitted_      = 0;
    uint32_t      last_drawn_repeats_   = 0xFFFFFFFFu;   // forces first paint

    // Repeater talkback: same REPEATER_HEARTBEAT the headless Atom
    // repeater emits, so a repeat-enabled Lume appears in the Director's
    // census alongside dedicated repeaters. uid = low 3 bytes of STA MAC.
    static constexpr uint32_t kCensusIntervalMs = 1000;
    uint32_t      last_census_ms_       = 0;
    uint8_t       census_uid_[3]        = {0, 0, 0};
    void          emit_repeat_census(uint32_t now);

    volatile bool signal_recovered_needs_repaint_ = false;

    // Deduplication ring per §4.3: last 16 (source_id, sequence_number)
    // tuples so the Director's 2-3x redundant TX doesn't cause double
    // IR fires / double paints per beat. Sequence number 0 is reserved
    // ("sequencing disabled") and treated as always fresh.
    // WiFi-task-owned - loop_tick only reads pending_light_*.
    // v3: source_id widened to u16 to match the wire.
    struct DedupEntry { uint16_t source_id; uint8_t sequence_number; };
    static constexpr size_t kDedupRingSize = 16;
    DedupEntry dedup_ring_[kDedupRingSize] = {};
    size_t     dedup_head_ = 0;

    bool seen_recently(uint16_t src, uint8_t seq) const;
    void mark_seen(uint16_t src, uint8_t seq);

    // Deferred (main-task) fan-out for LIGHT_PULSE. Handles bindings
    // that block (PixMobIrBinding -> IRsend::sendRaw).
    void fan_out_light_pulse(const transport::espnow::LightPulsePayload& p);

    // WiFi-task fan-out for LIGHT_PULSE. Called from on_recv for
    // bindings that declared can_render_in_callback() == true, so the
    // pulse start-time stamps within µs of the broadcast landing on
    // every Lume in the fleet. The two paths skip each other by
    // checking the flag; a binding is dispatched exactly once per pulse.
    // See docs/stickc-history.md.
    void fan_out_light_pulse_inline(const transport::espnow::LightPulsePayload& p);

    // Display-family fan-out. Filters by device_class() == Display; no
    // target_class byte on the wire and no relay concept for Display.
    void fan_out_text_display  (const transport::espnow::TextDisplayPayload& p);
    void fan_out_bitmap_header (const transport::espnow::BitmapHeaderPayload& p);
    void fan_out_bitmap_plane  (const transport::espnow::BitmapPlanePayload& p);
    void fan_out_clear_screen  (const transport::espnow::ClearScreenPayload& p);

    // WASH-family fan-out - same class+group filter as LIGHT_PULSE plus
    // a capability gate on can_wash. The binding owns its wash state.
    void fan_out_light_wash      (const transport::espnow::LightWashPayload& p);
    void fan_out_light_wash_end  (const transport::espnow::LightWashEndPayload& p);
    void fan_out_light_wash_pulse(const transport::espnow::LightWashPulsePayload& p);

    void on_recv(const hal::ESPNowMessage& m);

    int signal_bars_from_age() const;
    int signal_bars() const;

    void draw_status_pip();
    // Renders "R:NNNN" bottom-left when lume_repeat_en_. Repaints
    // lazily on counter change.
    void draw_repeat_count();
    void draw_no_signal_body();
};

}  // namespace modes
}  // namespace nocturnation
