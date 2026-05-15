// LumeMode - ESP-NOW receive (Epic 4 Block 3, RX side).
//
// Pulls the radio up on channel 1 (matching the master's hobby default;
// channel-priority dual-scan lands in Block 6) and registers a receive
// callback. Each inbound frame is decoded into its typed payload and
// logged via Serial - this is the byte-for-byte verification path the
// Epic Block 3 acceptance criterion calls for.
//
// Higher-level orchestration (deduplication, master-loss timeout +
// idle-effect fallback, display-as-light, IR re-fire on the slave) is
// Block 4's work. For now LumeMode just shows a counter of received
// frames and the last source_id / message type seen.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modes/mode_machine.h"
#include "hal/hal.h"
#include "output_bindings/output_binding.h"
#include "output_bindings/output_binding_context.h"
#include "transport/espnow/frame.h"
#include "transport/quality.h"

namespace nocturnation {
namespace modes {

class LumeMode : public Mode {
public:
    ModeId id() const override { return ModeId::Slave; }
    const char* name() const override { return "Slave"; }

    void enter() override;
    void exit() override;
    void loop_tick() override;
    void on_button_event(const dal::ButtonPressEvent& ev) override;

private:
    // No-signal threshold: 3x the master's heartbeat period (1 Hz). Three
    // missed heartbeats with no other traffic = master almost certainly
    // gone. Slaves do NOT auto-promote to master on this transition - the
    // master might be momentarily out of range or paused, and a rogue
    // slave-promoted-to-master would compete with the real master and
    // ruin show coordination. Block 4 will run a subtle local idle
    // effect through this state; for now we just display NO SIGNAL.
    static constexpr uint32_t kNoSignalMs          = 3000;

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

    // Active output bindings. Walked at enter() against
    // output_binding_registry(): each binding whose
    // required_capabilities() are a subset of host_caps() gets added
    // here paired with its own context, and its enter() called. Drained
    // in exit(). Inbound LIGHT_COMMAND frames decoded in on_recv fan
    // out via loop_tick() to every active binding's on_light_command.
    // Soft cap of 4 - we ship two concrete bindings (LocalDisplay +
    // PixMobIr) today; the headroom covers near-future additions
    // (DMX-out, Tildagon LED ring) without resizing.
    struct ActiveBinding {
        output_bindings::OutputBinding*        binding = nullptr;
        output_bindings::OutputBindingContext* ctx     = nullptr;
    };
    static constexpr size_t kMaxActiveBindings = 4;
    std::array<ActiveBinding, kMaxActiveBindings> active_bindings_{};
    size_t                                        active_binding_count_ = 0;

    // Channel preference: 0 = auto (dual-channel scan with show priority),
    // 1 / 6 / 11 = locked to that channel. Loaded from NVS on enter().
    uint8_t   slave_channel_pref_   = 0;
    uint8_t   current_listen_chan_  = 1;
    uint32_t  last_chan_switch_ms_  = 0;
    static constexpr uint32_t kChannelDwellMs = 2000;

    // NocturNation group ID for receive filtering (Epic 4.65 Block 5).
    // Loaded from NVS on enter() via persistence::load_slv_group();
    // operator sets via Config > Slave > Group. Default 0 means
    // "respond to everything". Distinct from the per-PixMobIrBinding
    // `group` property which is the PixMob protocol's IR group code.
    uint8_t   slv_group_            = 0;

    // Sequence-loss-rate signal quality. Transport-agnostic; could feed
    // off any sequenced protocol (future BLE / IR ack channels) the same
    // way it does ESP-NOW today.
    transport::SignalQuality quality_;

    // Deferred LIGHT_COMMAND queue (single slot; new arrivals replace).
    // The ESP-NOW receive callback runs on the WiFi task; calling
    // IRsend::sendRaw (~30 ms blocking GPIO loop) from that context
    // crashes the chip. We copy the payload here in on_recv and drain
    // in loop_tick (main task context, safe for the IR send timing).
    volatile bool                            pending_light_ = false;
    transport::espnow::LightCommandPayload   pending_light_payload_{};

    // Repeater mode (per spec §4.3, configurable per-slave via
    // Config > ESP-NOW > Repeat). When enabled, each unique inbound
    // frame is rebroadcast once with hop_count incremented by 1, up
    // to a 3-hop ceiling. Source_id and sequence_number are preserved
    // exactly so dedup works across the mesh - other slaves receiving
    // both the original and the repeat see them as duplicates.
    //
    // Queue same shape as the LIGHT_COMMAND queue: copy in on_recv,
    // drain in loop_tick (off the WiFi callback context).
    bool          slave_repeat_en_      = false;
    volatile bool pending_repeat_       = false;
    size_t        pending_repeat_len_   = 0;
    static constexpr size_t kRepeatBufSize  = 32;
    static constexpr uint8_t kMaxHopCount   = 3;
    uint8_t       pending_repeat_buf_[kRepeatBufSize] = {};

    // Deduplication ring (architecture spec §4.3): receivers track the
    // last 16 (source_id, sequence_number) tuples and drop repeats. The
    // master sends each frame 2-3 times for airtime resilience (Block 5
    // adds the redundant TX); without this gate the slave would paint
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

    // Convert a decoded LIGHT_COMMAND payload into an RgbPulseEvent
    // and fan out to every active OutputBinding. The fan-out is the
    // pre-migration render_light() body's two render_fx calls, now
    // owned by LocalDisplayBinding + PixMobIrBinding respectively
    // (Block 9).
    void fan_out_light_command(const transport::espnow::LightCommandPayload& p);

    void on_recv(const hal::ESPNowMessage& m);

    int signal_bars_from_age() const;
    int signal_bars() const;

    void draw_status_pip();
    void draw_no_signal_body();
};

}  // namespace modes
}  // namespace nocturnation
