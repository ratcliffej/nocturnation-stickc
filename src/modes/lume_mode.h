// LumeMode - ESP-NOW receive (Epic 4 Block 3, RX side).
//
// Pulls the radio up on channel 1 (matching the Director's hobby default;
// channel-priority dual-scan lands in Block 6) and registers a receive
// callback. Each inbound frame is decoded into its typed payload and
// logged via Serial - this is the byte-for-byte verification path the
// Epic Block 3 acceptance criterion calls for.
//
// Higher-level orchestration (deduplication, Director-loss timeout +
// idle-effect fallback, display-as-light, IR re-fire on the Lume) is
// Block 4's work. For now LumeMode just shows a counter of received
// frames and the last source_id / message type seen.
//
// **Audio analyser stays dormant in Lume mode** (spec v0.29 §8.2 power
// optimisation). LumeMode never calls DAL::start_audio_input(); without
// it the LocalDriver does not enable the mic, no spectrum frames are
// produced, BeatDetector / DropDetector / SectionDetector never run,
// and the FFT / energy-roll-up pipeline is dormant. This is the only
// gate Lumes need - the analyser pipeline is mic-driven, not
// free-running, so withholding mic capture cuts off the whole chain.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modes/mode_machine.h"
#include "hal/hal.h"
#include "output_bindings/output_binding.h"
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
    // No-signal threshold: 3x the Director's heartbeat period (1 Hz). Three
    // missed heartbeats with no other traffic = Director almost certainly
    // gone. Lumes do NOT auto-promote to Director on this transition - the
    // Director might be momentarily out of range or paused, and a rogue
    // Lume-promoted-to-Director would compete with the real Director and
    // ruin show coordination. The NO SIGNAL text surfaces at 3 s so the
    // operator gets quick visual feedback during bench debugging.
    static constexpr uint32_t kNoSignalMs          = 3000;

    // Signal-loss fallback wash. EMF artist-stage prep: when a Lume loses
    // the Director for long enough that a transient outage isn't a
    // plausible explanation, surface a calm idle effect so the audience
    // sees a coherent fleet rather than a row of dead badges. Three
    // phases, each anchored to age_since_rx (last frame -> now):
    //
    //   3 s  -> NO SIGNAL diagnostic text appears (operator-facing)
    //  10 s  -> fallback wash kicks in: muted blue<->purple cycle over
    //           kFallbackCyclePeriodMs, attack=kFallbackAttackMs (smooth
    //           fade-in). Synthesised LOCALLY as a LIGHT_WASH event
    //           dispatched through fan_out_light_wash - reuses every
    //           binding's existing wash state machine, no parallel
    //           render path. Low intensity so the badge reads as
    //           "alive but quiet", not "live show".
    //  40 s  -> emit LIGHT_WASH_END with release_time = kFallbackFadeMs
    //           in 100 ms units. The wash fades to black over the next
    //           30 s.
    //
    // Any inbound frame in on_recv cancels the fallback by emitting a
    // short-release LIGHT_WASH_END so the binding fades out and the
    // returning Director's wash/pulse traffic isn't competing with
    // the synthetic baseline.
    static constexpr uint32_t kFallbackEnterMs       = 10000;   // ms silence before fade-in
    static constexpr uint32_t kFallbackFadeStartMs   = 40000;   // ms silence before fade-to-black starts
    static constexpr uint16_t kFallbackCyclePeriodMs = 10000;   // blue<->purple ping-pong period
    static constexpr uint8_t  kFallbackAttackTicks   = 30;      // 100 ms units = 3 s gentle fade-in
    static constexpr uint8_t  kFallbackFadeTicks     = 255;     // 100 ms units = ~25.5 s (cap); see _emit_fallback_end
    static constexpr uint8_t  kFallbackIntensity     = 60;      // 0..255; ~24 % brightness for "alive but quiet"
    static constexpr uint8_t  kFallbackRecoveryTicks = 5;       // 100 ms units = 500 ms quick fade-out on signal return

    // Fallback colours (cycled by the binding via cycle_ms ping-pong).
    // Both deliberately muted - the fallback should read as a gentle
    // breathing field, not a show effect.
    static constexpr uint8_t  kFallbackColourA[3] = { 20,  0, 80 };   // dark violet
    static constexpr uint8_t  kFallbackColourB[3] = {  0, 20, 80 };   // dark navy

    // Status pip (always-visible 38x12 px overlay anchored to the top-
    // right corner with a signal-strength dot + battery glyph). Block 13
    // replaced the full-width 12 px strip with a compact pip so the
    // pulse rect can run full-screen for maximum colour impact during a
    // show. The pip paints OVER the pulse rect on each refresh - the
    // pulse may briefly paint underneath between pip refreshes, accepted
    // trade-off given the alternative was losing always-on status. Pip
    // refresh cadence is 100 ms (10 Hz): fast enough to read as steady
    // when full-screen pulses repaint at ~30 Hz; slow enough to keep the
    // SPI write budget bounded (~7 fill_rects per refresh, batched into
    // one burst via begin_buffered_paint).
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
    uint8_t   last_source_id_     = 0;
    uint8_t   last_msg_type_      = 0xFF;
    bool      no_signal_          = false;   // sticky once threshold crossed

    // Fallback wash state. fallback_active_ is set when we crossed the
    // kFallbackEnterMs threshold and emitted the synthetic blue/purple
    // LIGHT_WASH; fallback_faded_ is set when we crossed kFallbackFadeStartMs
    // and emitted the LIGHT_WASH_END. Both clear on signal recovery
    // (on_recv emits a short-release LIGHT_WASH_END and resets the flags).
    bool      fallback_active_    = false;
    bool      fallback_faded_     = false;

    // Helpers that build a synthetic LightWashPayload / LightWashEndPayload
    // and fan it out as if it had arrived from the Director. Pure local
    // dispatch - never broadcast onto the radio.
    void emit_fallback_wash_start();
    void emit_fallback_wash_fade();
    void emit_fallback_wash_recovery();

    // Lock-acquire timestamp. Stamped in on_recv whenever signal is
    // (re-)acquired - either the very first frame, or the recovery
    // edge out of a NO SIGNAL window. Drives the brief "freshly
    // locked" indication on the LED-strip status overlay (and could
    // be reused for a future LCD lock-acquire flash). 0 = never
    // acquired yet (still Searching from cold boot).
    uint32_t  lock_acquired_ms_   = 0;
    static constexpr uint32_t kFreshLockMs           = 1000;
    static constexpr uint32_t kIndicatorFlashPeriodMs = 1000;     // 1 Hz, 50 % duty

    // Active output bindings. Walked at enter() against
    // output_binding_registry(): each binding whose
    // required_capabilities() are a subset of host_caps() gets added
    // here paired with its own context, and its enter() called. Drained
    // in exit(). Inbound LIGHT_PULSE frames decoded in on_recv fan
    // out via loop_tick() to every active binding's on_light_pulse.
    // Soft cap of 8 - we ship PixMobIrBinding + LumeLedStripBinding
    // today; Epic 13 adds LumeTextBinding + LumeBitmapBinding; the
    // headroom covers near-future additions (DMX-out, etc.) without
    // resizing.
    struct ActiveBinding {
        output_bindings::OutputBinding*        binding = nullptr;
        output_bindings::OutputBindingContext* ctx     = nullptr;
    };
    static constexpr size_t kMaxActiveBindings = 8;
    std::array<ActiveBinding, kMaxActiveBindings> active_bindings_{};
    size_t                                        active_binding_count_ = 0;

    // Channel preference: 0 = auto (3-channel scan with show priority),
    // 1 / 6 / 11 = locked to that channel. Loaded from NVS on enter().
    // Auto-scan order is 11 (show) -> 1 (hobby) -> 6 (advanced override) -> repeat,
    // 2 s dwell per channel; the first channel that produces a valid
    // NocturNation frame locks the Lume there. Locked Lumes never
    // re-scan on signal loss (operator picked that channel deliberately;
    // we respect that). Auto-mode Lumes re-scan after kRescanMs of
    // no traffic - decoupled from kNoSignalMs so a brief Director
    // outage doesn't force a full channel hunt.
    uint8_t   lume_channel_pref_   = 0;
    uint8_t   current_listen_chan_  = 1;
    uint32_t  last_chan_switch_ms_  = 0;
    static constexpr uint32_t kChannelDwellMs = 2000;
    // Auto-scan ordered priority - show channel first, hobby second,
    // advanced override last. Iterated in the dual-channel scan logic.
    static constexpr uint8_t  kScanOrder[3]      = {11, 1, 6};
    static constexpr size_t   kScanOrderCount    = 3;
    // Re-scan threshold (auto-mode Lumes only). 10 s vs the 3 s NO SIGNAL
    // display threshold: most signal losses are transient (Director
    // reboot, brief congestion). Staying on the current channel for
    // ~7 s past the NO SIGNAL prompt catches transient outages without
    // forcing a multi-channel hunt; only after 10 s do we genuinely
    // assume the Director has gone (or has changed channel) and start
    // looking elsewhere.
    static constexpr uint32_t kRescanMs           = 10000;

    // NocturNation group ID for receive filtering (Epic 4.65 Block 5).
    // Loaded from NVS on enter() via persistence::load_lume_group();
    // operator sets via Config > Lume > Group. Default 0 means
    // "respond to everything". Distinct from the per-PixMobIrBinding
    // `group` property which is the PixMob protocol's IR group code.
    uint8_t   lume_group_            = 0;

    // Sequence-loss-rate signal quality. Transport-agnostic; could feed
    // off any sequenced protocol (future BLE / IR ack channels) the same
    // way it does ESP-NOW today.
    transport::SignalQuality quality_;

    // TOFU lock - partitions the Lume between potentially-multiple
    // Directors broadcasting on the same channel. Port of the
    // Tildagon's TofuLock for EMF 2026 (multi-show partitioning).
    // First-eligible-frame establishes the lock; subsequent frames
    // from any other source_id are silently dropped. Expires after
    // 10 s of silence from the locked source (matches kRescanMs so
    // channel-rescan and TOFU re-lock fire on the same edge).
    transport::espnow::TofuLock tofu_;

    // Deferred LIGHT_PULSE queue (single slot; new arrivals replace).
    // The ESP-NOW receive callback runs on the WiFi task; calling
    // IRsend::sendRaw (~30 ms blocking GPIO loop) from that context
    // crashes the chip. We copy the payload here in on_recv and drain
    // in loop_tick (main task context, safe for the IR send timing).
    volatile bool                            pending_light_ = false;
    transport::espnow::LightPulsePayload   pending_light_payload_{};

    // Repeater mode (per spec §4.3, configurable per-Lume via
    // Config > ESP-NOW > Repeat). When enabled, each unique inbound
    // frame is rebroadcast once with hop_count incremented by 1, up
    // to a 3-hop ceiling. Source_id and sequence_number are preserved
    // exactly so dedup works across the mesh - other Lumes receiving
    // both the original and the repeat see them as duplicates.
    //
    // Queue same shape as the LIGHT_PULSE queue: copy in on_recv,
    // drain in loop_tick (off the WiFi callback context).
    bool          lume_repeat_en_      = false;
    volatile bool pending_repeat_       = false;
    size_t        pending_repeat_len_   = 0;
    // Bumped to 250 in Epic 13 to match the new wire-side
    // transport::espnow::kMaxFrameSize (Epic 13 introduces BITMAP_PLANE
    // payloads up to ~240 bytes). Costs a single buffer's worth of
    // RAM per Lume - immaterial against the device's 320 KB.
    static constexpr size_t kRepeatBufSize  = 250;
    static constexpr uint8_t kMaxHopCount   = 3;
    uint8_t       pending_repeat_buf_[kRepeatBufSize] = {};

    // Epic 15 bench follow-up: visible repeater diagnostic so the
    // operator can confirm at a glance that the relay path is firing
    // (i.e. read the Stick standing in front of it, instead of
    // needing to USB-monitor its serial). repeats_emitted_ counts
    // successful send_broadcast calls from the rebroadcast drain;
    // last_drawn_repeats_ tracks what the LCD currently shows so we
    // only repaint on change (every relay would hammer the SPI bus).
    uint32_t      repeats_emitted_      = 0;
    uint32_t      last_drawn_repeats_   = 0xFFFFFFFFu;   // forces first paint

    // Repeater talkback: a repeat-enabled Lume beacons the same ~1 Hz
    // REPEATER_HEARTBEAT the headless Atom repeater emits, so it shows
    // up in the Director's census (console `show` / `mon`, and the
    // AtomS3R dashboard's `rpt N`) alongside the dedicated repeaters.
    // uid = low 3 bytes of the STA MAC, same stable identity scheme.
    // Same interval as RepeaterMode::kCensusIntervalMs; kept as a
    // separate constant so the modes stay dependency-free of each other.
    static constexpr uint32_t kCensusIntervalMs = 1000;
    uint32_t      last_census_ms_       = 0;
    uint8_t       census_uid_[3]        = {0, 0, 0};
    void          emit_repeat_census(uint32_t now);

    // Epic 15 bench-test bug fix. Set in on_recv (WiFi task) when the
    // signal-recovered edge fires; drained in loop_tick (main task)
    // by clearing the stale NO SIGNAL paint and repainting the pip.
    // SPI from on_recv would crash the S3 (same reasoning as
    // pending_repeat_); defer through this flag.
    volatile bool signal_recovered_needs_repaint_ = false;

    // Deduplication ring (architecture spec §4.3): receivers track the
    // last 16 (source_id, sequence_number) tuples and drop repeats. The
    // Director sends each frame 2-3 times for airtime resilience (Block 5
    // adds the redundant TX); without this gate the Lume would paint
    // and fire IR twice per logical beat.
    //
    // Sequence number 0 is reserved as "sequencing disabled" per spec -
    // we treat those frames as always fresh (never deduped). All other
    // values 1-255 wrap normally.
    //
    // Only the WiFi-task on_recv reads/writes this; loop_tick only
    // observes pending_light_ + pending_light_payload_.
    struct DedupEntry { uint8_t source_id; uint8_t sequence_number; };
    static constexpr size_t kDedupRingSize = 16;
    DedupEntry dedup_ring_[kDedupRingSize] = {};
    size_t     dedup_head_ = 0;

    bool seen_recently(uint8_t src, uint8_t seq) const;
    void mark_seen(uint8_t src, uint8_t seq);

    // Convert a decoded LIGHT_PULSE payload into an RgbPulseEvent
    // and fan out to every active OutputBinding (PixMobIrBinding,
    // LumeLedStripBinding, etc. — each owns one render surface).
    void fan_out_light_pulse(const transport::espnow::LightPulsePayload& p);

    // Epic 13: callback-context fan-out for LIGHT_PULSE. Called from
    // on_recv (WiFi task) for bindings that declared
    // can_render_in_callback() == true. Stamping the pulse start-time
    // in this path (vs the deferred loop_tick path) eliminates
    // inter-Lume render variance for the sparkle-on-beat sync the
    // operator's eye keys on. Bindings that block (PixMobIrBinding
    // → IRsend::sendRaw) keep their declaration as false and are
    // handled by fan_out_light_pulse() out of the pending queue
    // instead. The two paths skip each other by checking the flag,
    // so a given binding is dispatched exactly once per pulse.
    void fan_out_light_pulse_inline(const transport::espnow::LightPulsePayload& p);

    // Epic 13: Display-family fan-out. Filters bindings by
    // device_class() == Display + the local-binding group rule (no
    // target_class on the wire; no relay concept for Display). Each
    // call delivers to the per-class on_X hook on each matching
    // binding (LumeTextBinding for text+clear, LumeBitmapBinding for
    // bitmap+clear).
    void fan_out_text_display  (const transport::espnow::TextDisplayPayload& p);
    void fan_out_bitmap_header (const transport::espnow::BitmapHeaderPayload& p);
    void fan_out_bitmap_plane  (const transport::espnow::BitmapPlanePayload& p);
    void fan_out_clear_screen  (const transport::espnow::ClearScreenPayload& p);

    // Epic 6C Phase F: WASH-family fan-out. Same class+group filter as
    // fan_out_light_pulse + a capability gate (binding's can_wash). The
    // binding owns its own wash state; this just routes.
    void fan_out_light_wash      (const transport::espnow::LightWashPayload& p);
    void fan_out_light_wash_end  (const transport::espnow::LightWashEndPayload& p);
    void fan_out_light_wash_pulse(const transport::espnow::LightWashPulsePayload& p);

    void on_recv(const hal::ESPNowMessage& m);

    int signal_bars_from_age() const;
    int signal_bars() const;

    void draw_status_pip();
    // Epic 15 bench follow-up. Renders "R:NNNN" in the bottom-left of
    // the LCD when lume_repeat_en_ is true. Repaints lazily on counter
    // change. No-op when repeat mode is off (operator sees a clean
    // LCD).
    void draw_repeat_count();
    void draw_no_signal_body();
};

}  // namespace modes
}  // namespace nocturnation
