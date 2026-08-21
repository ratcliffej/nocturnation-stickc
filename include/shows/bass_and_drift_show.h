// BassAndDriftShow - the reference reactive Show for Epic 6D (B2).
//
// Implements the architecture spec section 1.2 lighting design language
// on the StickC: a continuous wash baseline drifts through a per-section
// palette (verse / chorus / build / drop / breakdown), bass-led beat
// pulses overlay it gated by a `chance` property, and the operator's
// manual drop marker (Btn1 / Confirm) always wins over the analyser.
//
// Companion to ConductorShow on the Tildagon (B3). Both Shows share the
// same palette indices and section semantics so a mixed Lume fleet sees
// visually consistent lighting regardless of which device class is the
// Director on the channel.

#pragma once

#include "shows/show.h"
#include "plugins/property_bag.h"

namespace nocturnation {
namespace shows {

class BassAndDriftShow : public Show {
public:
    const char* id()           const override { return "bass-drift"; }
    const char* display_name() const override { return "Bass & Drift"; }

    hal::CapabilityMask required_capabilities() const override;
    plugins::Span<const plugins::PropertyDef> properties() const override;
    plugins::PowerProfile power() const override;

    void enter(ShowContext&) override;
    void exit (ShowContext&) override;

    void on_audio_frame     (ShowContext&, const dal::AudioFrameEvent&) override;
    void on_beat_detected   (ShowContext&, uint8_t strength)            override;
    void on_section_change  (ShowContext&, uint8_t section)             override;
    void on_input_action    (ShowContext&, const hal::InputEvent&)      override;
    void on_render          (ShowContext&)                              override;
    void tick               (ShowContext&, uint32_t now_ms)             override;
    void on_property_changed(ShowContext&, const char* key)             override;

    ShowContext& context() override;

private:
    // BPM tracking - mirrors SimpleBeatShow's IBI buffer + median.
    static constexpr size_t kIbiBufferSize = 8;
    uint32_t ibi_buffer_[kIbiBufferSize] = {0};
    size_t   ibi_index_  = 0;
    size_t   ibi_count_  = 0;
    uint32_t last_beat_ms_ = 0;
    float    estimated_bpm_ = 0.0f;
    void update_bpm_from_buffer();

    // Current section the wash baseline represents. Default = Verse on
    // entry; updated by on_section_change when responding to sections,
    // by the manual drop marker (Confirm), or held constant when
    // respond_to_sections is false.
    uint8_t current_section_ = 1;       // Verse

    // Wash refresh state. The Show debounces auto-re-emits at the wire:
    // a Beat-locked recompute fires only when BPM drifts >=10 % since
    // the last emitted cycle AND >=3 s have passed (per Epic 6D B0).
    uint32_t last_wash_emit_ms_ = 0;
    uint16_t last_wash_cycle_ms_ = 0;

    // Manual-drop state. On Confirm we render the drop palette + a peak
    // pulse, latch a deadline, and tick() restores the previous section's
    // wash when it expires. Defaults to 6 s per B0 ("queue a render_wash
    // palette swap to the drop palette for ~6 s").
    static constexpr uint32_t kManualDropHoldMs = 6000;
    bool     manual_drop_active_   = false;
    uint32_t manual_drop_until_ms_ = 0;
    uint8_t  manual_drop_prev_sec_ = 1;  // section to restore to

    // Audio descriptors - latched for the LCD readout. The wash's phrase
    // drift is the receiver's own A<->B<->A cycle on the wire, not a
    // Director-side per-frame re-emit; bass energy + level only feed the
    // operator-facing display.
    float bass_energy_   = 0.0f;
    float overall_rms_   = 0.0f;

    // Internal helpers.
    void emit_wash_for_section(ShowContext& ctx, uint8_t section, uint32_t now_ms);
    void fire_pulse_for_section(ShowContext& ctx, uint8_t section, uint8_t strength);
    uint16_t cycle_ms_for_section(uint8_t section) const;

    // LED-effect state (Epic 18 v0x04 follow-up). One counter serves both
    // Walk (steps 0..kLedWalkLength-1) and Alternating (steps 0..1). The
    // fire helper reads and post-increments it per fire, wrapping at the
    // effect's own period.
    //
    // kLedWalkLength is the wire-side walk cap - the sender iterates
    // 0..kLedWalkLength-1 regardless of the receiving Lume's actual
    // pixel count. A Lume with fewer pixels silently drops OOB indices
    // (see LedStripDriver LedMode-1 handler); a Lume with more pixels
    // sees the walk repeat every kLedWalkLength positions. 30 matches
    // the current NocturNation strip standard (6 x 5 = 30 LEDs); a
    // future property can override this if the fleet grows longer.
    static constexpr uint8_t kLedWalkLength = 30;
    uint8_t led_step_ = 0;

    // Deterministic RNG for Sparkle. Sequence-driven from a xorshift
    // seed so unit tests can pin exact pixel picks. Reseeded on enter().
    uint32_t sparkle_rng_state_ = 0x9E3779B9u;
    uint8_t  next_sparkle_pixel();
};

BassAndDriftShow*       bass_and_drift_show_instance();
plugins::PropertyBag&   bass_and_drift_show_property_bag();
ShowContext&            bass_and_drift_show_context();

}  // namespace shows
}  // namespace nocturnation
