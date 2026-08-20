// BassAndDriftShow - implementation (Epic 6D B2).
//
// The §1.2 normative section made concrete: three layered timescales
// (section -> phrase -> beat) running together, restraint via `chance`,
// bass-led punctuation, operator override always wins.

#include "shows/bass_and_drift_show.h"
#include "shows/show_context.h"

#include "dal/dal.h"
#include "pulse/envelope.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef ARDUINO
#include <Arduino.h>
#else
extern "C" uint32_t millis();
#endif

namespace nocturnation {
namespace shows {

using namespace nocturnation::dal;
using nocturnation::plugins::PluginKind;
using nocturnation::plugins::PowerProfile;
using nocturnation::plugins::PropertyBag;
using nocturnation::plugins::PropertyDef;
using nocturnation::plugins::PropertyType;
using nocturnation::plugins::PropertyValue;
using nocturnation::plugins::Span;

// =============================================================================
// Section IDs (mirror Show::on_section_change comment in include/shows/show.h)
// =============================================================================

namespace {

constexpr uint8_t kSectionUnknown   = 0;
constexpr uint8_t kSectionVerse     = 1;
constexpr uint8_t kSectionChorus    = 2;
constexpr uint8_t kSectionBuildUp   = 3;
constexpr uint8_t kSectionBreakdown = 4;
constexpr uint8_t kSectionDrop      = 7;

// =============================================================================
// Palette tables (Epic 6D B0.1)
// =============================================================================
//
// Four palette sets, each with five section palettes + a pulse colour
// + a starlight overlay colour. Two-anchor wash = (A, B) cycling A->B->A
// on cycle_ms. RGB values 0..255. Indexed by [palette_set][section_idx]
// where section_idx 0..4 maps Verse / Chorus / BuildUp / Drop / Breakdown.

struct PaletteEntry {
    uint8_t r1, g1, b1;     // anchor A
    uint8_t r2, g2, b2;     // anchor B
    uint8_t intensity;      // 0..255 wash brightness scalar
    uint16_t cycle_ms;      // A<->B<->A drift; section may override wash_speed
};

constexpr size_t kPaletteSetCount  = 4;
constexpr size_t kSectionIdxCount  = 5;   // Verse / Chorus / BuildUp / Drop / Breakdown

// Section -> index into the section palette array. Unknown / VocalsOnly /
// InstrumentalBreak fall through to Verse (the safe "default" anchor).
uint8_t section_to_idx(uint8_t section) {
    switch (section) {
        case kSectionChorus:    return 1;
        case kSectionBuildUp:   return 2;
        case kSectionDrop:      return 3;
        case kSectionBreakdown: return 4;
        case kSectionVerse:
        default:                return 0;
    }
}

constexpr PaletteEntry kPalettes[kPaletteSetCount][kSectionIdxCount] = {
    // ---- Warm (default) ------------------------------------------------
    {
        // Verse
        { 120,  30,   0,   60,  10,   0, 140, 8000  },
        // Chorus
        { 255, 140,  30,  255,  60,  80, 200, 5000  },
        // BuildUp
        { 255, 100,   0,  255, 200,   0, 180, 3000  },
        // Drop
        { 255, 220, 120,  255,  80,   0, 255, 4000  },
        // Breakdown
        {  40,  10,   0,    5,   0,   0,  80, 12000 },
    },
    // ---- Cool ----------------------------------------------------------
    {
        // Verse
        {   0,  60, 100,    0,  20,  60, 140, 8000  },
        // Chorus
        {   0, 220, 255,  200,   0, 220, 200, 5000  },
        // BuildUp
        {   0, 180, 255,  100, 100, 255, 180, 3000  },
        // Drop
        { 220, 240, 255,  255,   0, 200, 255, 4000  },
        // Breakdown
        {   0,  10,  40,    0,   0,  10,  80, 12000 },
    },
    // ---- Vivid ---------------------------------------------------------
    {
        // Verse
        {  80,   0, 120,    0,  80, 100, 140, 8000  },
        // Chorus
        { 255,   0, 180,   80, 255,  40, 200, 5000  },
        // BuildUp
        { 255, 120,   0,  255, 220,   0, 180, 3000  },
        // Drop
        { 255,  40, 200,  255, 220,   0, 255, 4000  },
        // Breakdown
        {  30,   0,  40,    0,   0,  10,  80, 12000 },
    },
    // ---- Mono ----------------------------------------------------------
    {
        // Verse
        {  80,  60,  40,   40,  30,  20, 140, 8000  },
        // Chorus
        { 220, 200, 160,  255, 220, 180, 200, 5000  },
        // BuildUp
        { 200, 180, 140,  255, 240, 200, 180, 3000  },
        // Drop
        { 255, 240, 220,  255, 255, 255, 255, 4000  },
        // Breakdown
        {  20,  15,  10,    0,   0,   0,  80, 12000 },
    },
};

struct PulseColour { uint8_t r, g, b; };

constexpr PulseColour kPulseColour[kPaletteSetCount] = {
    { 255, 180,  60 },   // Warm
    {   0, 220, 255 },   // Cool
    { 255,  60, 180 },   // Vivid
    { 255, 240, 220 },   // Mono
};

constexpr PulseColour kStarlightColour[kPaletteSetCount] = {
    { 255, 220, 180 },   // Warm
    { 200, 220, 255 },   // Cool
    { 255, 255, 255 },   // Vivid
    { 255, 240, 220 },   // Mono
};

// =============================================================================
// Wash speed enum + mapping
// =============================================================================
//
// 0..4 are fixed cycle_ms; 5 = Beat x4, 6 = Beat x8 (Epic 6D B0.2).
// Beat-locked computes cycle_ms = clamp(beat_ms * N, 1500, 12000); if
// BPM is unknown (estimated_bpm_ == 0) we fall back to Medium so the
// wash still has a sensible drift on entry.

enum class WashSpeed : uint8_t {
    Glacial = 0,
    Slow    = 1,
    Medium  = 2,
    Fast    = 3,
    Snappy  = 4,
    Beat4   = 5,
    Beat8   = 6,
};

constexpr uint16_t kManualCycleMs[5] = {
    12000,   // Glacial
     8000,   // Slow
     5000,   // Medium  (and Beat-locked fallback when BPM is unknown)
     3000,   // Fast
     2000,   // Snappy
};

constexpr uint16_t kBeatLockedClampLo = 1500;
constexpr uint16_t kBeatLockedClampHi = 12000;

uint16_t cycle_ms_for_speed(WashSpeed speed, float estimated_bpm) {
    if (speed >= WashSpeed::Glacial && speed <= WashSpeed::Snappy) {
        return kManualCycleMs[static_cast<uint8_t>(speed)];
    }
    if (estimated_bpm < 30.0f) {
        // No reliable BPM yet - fall back to Medium so the wash still
        // breathes audibly. Beat-locked picks up on the next recompute
        // once BPM stabilises.
        return kManualCycleMs[static_cast<uint8_t>(WashSpeed::Medium)];
    }
    const float beat_ms = 60000.0f / estimated_bpm;
    const float n = (speed == WashSpeed::Beat4) ? 4.0f : 8.0f;
    float cycle = beat_ms * n;
    if (cycle < kBeatLockedClampLo) cycle = kBeatLockedClampLo;
    if (cycle > kBeatLockedClampHi) cycle = kBeatLockedClampHi;
    return static_cast<uint16_t>(cycle + 0.5f);
}

bool is_beat_locked(WashSpeed speed) {
    return speed == WashSpeed::Beat4 || speed == WashSpeed::Beat8;
}

// Sections that take over the cycle_ms regardless of wash_speed (B0.2:
// "section override - BuildUp / Drop / Breakdown retain their feel").
bool section_overrides_speed(uint8_t section) {
    return section == kSectionBuildUp
        || section == kSectionDrop
        || section == kSectionBreakdown;
}

}  // namespace

// =============================================================================
// Property schema
// =============================================================================

namespace {

const char* const kPaletteSetNames[] = { "Warm", "Cool", "Vivid", "Mono" };
const char* const kWashSpeedNames[]  = {
    "Glacial", "Slow", "Medium", "Fast", "Snappy", "Beat x4", "Beat x8"
};
const char* const kChanceNames[] = {
    "100%", "88%", "67%", "50%", "32%", "16%", "10%", "4%"
};

// `chance` is an Enum picking one of the wire protocol's eight discrete
// Chance steps. Index 0..7 maps directly to the pixmob::Chance enum value
// in descending percentage order, which matches how the property displays
// in the Settings overlay. Default index 4 = CHANCE_32 (~25 %), matching
// the §1.2 "fire on selected beats" guidance ("10-25 %").
constexpr pulse::Chance kChanceTable[] = {
    pulse::CHANCE_100,
    pulse::CHANCE_88,
    pulse::CHANCE_67,
    pulse::CHANCE_50,
    pulse::CHANCE_32,
    pulse::CHANCE_16,
    pulse::CHANCE_10,
    pulse::CHANCE_4,
};
constexpr uint8_t kChanceCount = sizeof(kChanceTable) / sizeof(kChanceTable[0]);
constexpr uint8_t kChanceDefaultIdx = 4;   // CHANCE_32 (~25 %)

pulse::Chance chance_from_idx(uint8_t idx) {
    if (idx >= kChanceCount) idx = kChanceDefaultIdx;
    return kChanceTable[idx];
}

const PropertyDef kProps[] = {
    PropertyDef{
        /*key=*/"palette_set",
        /*type=*/PropertyType::Enum,
        /*default_value=*/PropertyValue::from_enum(0),       // Warm
        /*min_value=*/    PropertyValue::from_enum(0),
        /*max_value=*/    PropertyValue::from_enum(3),
        /*display_name=*/"Palette",
        /*unit=*/nullptr,
        /*enum_names=*/kPaletteSetNames,
    },
    PropertyDef{
        /*key=*/"chance",
        /*type=*/PropertyType::Enum,
        /*default_value=*/PropertyValue::from_enum(kChanceDefaultIdx),  // 32 %
        /*min_value=*/    PropertyValue::from_enum(0),
        /*max_value=*/    PropertyValue::from_enum(kChanceCount - 1),
        /*display_name=*/"Beat chance",
        /*unit=*/nullptr,
        /*enum_names=*/kChanceNames,
    },
    PropertyDef{
        /*key=*/"wash_speed",
        /*type=*/PropertyType::Enum,
        /*default_value=*/PropertyValue::from_enum(static_cast<uint8_t>(WashSpeed::Beat8)),
        /*min_value=*/    PropertyValue::from_enum(0),
        /*max_value=*/    PropertyValue::from_enum(6),
        /*display_name=*/"Wash speed",
        /*unit=*/nullptr,
        /*enum_names=*/kWashSpeedNames,
    },
    PropertyDef{
        /*key=*/"respond_to_sections",
        /*type=*/PropertyType::Bool,
        /*default_value=*/PropertyValue::from_bool(true),
        /*min_value=*/    PropertyValue::from_bool(false),
        /*max_value=*/    PropertyValue::from_bool(true),
        /*display_name=*/"Sections drive wash",
        /*unit=*/nullptr,
        /*enum_names=*/nullptr,
    },
    PropertyDef{
        /*key=*/"starlight",
        /*type=*/PropertyType::Bool,
        /*default_value=*/PropertyValue::from_bool(false),
        /*min_value=*/    PropertyValue::from_bool(false),
        /*max_value=*/    PropertyValue::from_bool(true),
        /*display_name=*/"Starlight",
        /*unit=*/nullptr,
        /*enum_names=*/nullptr,
    },
    PropertyDef{
        /*key=*/"target_group",
        /*type=*/PropertyType::U8,
        /*default_value=*/PropertyValue::from_u8(0),
        /*min_value=*/    PropertyValue::from_u8(0),
        // Cycle 0..3 wrapping back to 0 in the Director-Mode property UI
        // (Btn advance wraps at max_value per the U8 handler in
        // director_mode.cpp). 0 = broadcast; 1..3 = the capability-rich
        // fleet zones set by the first-install random-group [1,3] rule.
        // Groups 4..15 exist for operator hand-assignment via Config-mode
        // but aren't part of the Bass-and-Drift authored palette; PixMobs
        // live in groups 10..12 and don't render the wash coherently anyway.
        /*max_value=*/    PropertyValue::from_u8(3),
        /*display_name=*/"Target group",
        /*unit=*/nullptr,
        /*enum_names=*/nullptr,
    },
};

constexpr size_t kPropCount = sizeof(kProps) / sizeof(kProps[0]);

// Target-string scratch buffer. Built fresh per emit so we don't share
// across calls (target_group can change between renders).
//
// Format mirrors render_fx contract: "<hex_class>:<hex_group>", class
// 0x00 = all classes (Light + Screen + MultiLedScreen). The Show fires
// broadly so a mixed Lume fleet sees a coherent visual; per-class
// targeting is a v2 concern.
void build_target(char* buf, size_t buf_size, uint8_t group) {
    std::snprintf(buf, buf_size, "00:%02x", group);
}

// Beat-locked recompute debounce thresholds (Epic 6D B0.2).
constexpr uint32_t kBeatLockedRecomputeMinGapMs = 3000;
constexpr float    kBeatLockedRecomputeDriftPct = 0.10f;

}  // namespace

// =============================================================================
// Capabilities / power / properties
// =============================================================================

hal::CapabilityMask BassAndDriftShow::required_capabilities() const {
    // Mic is needed for beats + sections + descriptors. ESPNow implicit on
    // the Director.
    return hal::make_capability_mask(hal::Capability::Mic);
}

Span<const PropertyDef> BassAndDriftShow::properties() const {
    return Span<const PropertyDef>{kProps, kPropCount};
}

PowerProfile BassAndDriftShow::power() const {
    PowerProfile p;
    p.needs_audio_frames   = true;
    p.needs_spectrum_frame = false;
    p.needs_8band_summary  = false;
    p.lcd_refresh_hz_max   = 20;
    // tick_hz drives the manual-drop auto-restore; a low cadence is fine
    // since the restore window is 6 s and the renderer's own cycle drives
    // visible drift on the wire.
    p.tick_hz              = 4;
    return p;
}

// =============================================================================
// Lifecycle
// =============================================================================

void BassAndDriftShow::enter(ShowContext& ctx) {
    // Reset per-show state. Property values come from the bag (which
    // falls back to the schema default if never set).
    for (size_t i = 0; i < kIbiBufferSize; ++i) ibi_buffer_[i] = 0;
    ibi_index_      = 0;
    ibi_count_      = 0;
    last_beat_ms_   = 0;
    estimated_bpm_  = 0.0f;
    current_section_       = kSectionVerse;
    last_wash_emit_ms_     = 0;
    last_wash_cycle_ms_    = 0;
    manual_drop_active_    = false;
    manual_drop_until_ms_  = 0;
    manual_drop_prev_sec_  = kSectionVerse;
    bass_energy_           = 0.0f;
    overall_rms_           = 0.0f;

    emit_wash_for_section(ctx, current_section_, millis());
}

void BassAndDriftShow::exit(ShowContext& ctx) {
    // 1.0 s fade to black on the way out (matches WashDemoShow).
    char target[8];
    build_target(target, sizeof(target),
                 ctx.get_property("target_group").as_u8());
    ctx.render_wash_end(target, 10);
}

// =============================================================================
// Audio frame - bass energy + RMS for the LCD only (no wire traffic)
// =============================================================================

void BassAndDriftShow::on_audio_frame(ShowContext& /*ctx*/,
                                       const AudioFrameEvent& ev) {
    bass_energy_ = ev.bass_energy;
    overall_rms_ = ev.overall_rms;
}

// =============================================================================
// Beat - bass-led punctuation, gated by `chance`
// =============================================================================

void BassAndDriftShow::on_beat_detected(ShowContext& ctx, uint8_t strength) {
    const uint32_t now = millis();

    // BPM tracking (verbatim cadence + bounds from SimpleBeatShow).
    if (last_beat_ms_ > 0) {
        const uint32_t ibi = now - last_beat_ms_;
        if (ibi >= 300 && ibi <= 1200) {
            ibi_buffer_[ibi_index_] = ibi;
            ibi_index_ = (ibi_index_ + 1) % kIbiBufferSize;
            if (ibi_count_ < kIbiBufferSize) ibi_count_++;
            update_bpm_from_buffer();
        }
    }
    last_beat_ms_ = now;

    if (ctx.paused()) return;

    // Beat-locked wash recompute: if the current speed is Beat xN and
    // the BPM has drifted >=10 % since the last emitted cycle AND >=3 s
    // have passed, re-emit the wash with the new cycle_ms. Skip while a
    // manual drop is active so we don't trample the drop palette.
    if (!manual_drop_active_) {
        const auto speed = static_cast<WashSpeed>(
            ctx.get_property("wash_speed").as_enum());
        if (is_beat_locked(speed)
            && estimated_bpm_ > 0.0f
            && now - last_wash_emit_ms_ >= kBeatLockedRecomputeMinGapMs) {
            const uint16_t fresh = cycle_ms_for_speed(speed, estimated_bpm_);
            if (last_wash_cycle_ms_ == 0
                || std::abs(static_cast<int>(fresh) - static_cast<int>(last_wash_cycle_ms_))
                   >= static_cast<int>(last_wash_cycle_ms_ * kBeatLockedRecomputeDriftPct)) {
                emit_wash_for_section(ctx, current_section_, now);
            }
        }
    }

    // Pulse layer - gated by `chance`. The `chance` field is the existing
    // protocol gate; it distributes the firings probabilistically across
    // Lumes on receive. We pass it through the RgbPulseEvent rather than
    // dropping the whole render_fx call so PixMob and capable Lumes both
    // honour the same gate.
    fire_pulse_for_section(ctx, current_section_, strength);
}

// =============================================================================
// Section change - palette swap (gated by respond_to_sections)
// =============================================================================

void BassAndDriftShow::on_section_change(ShowContext& ctx, uint8_t section) {
    if (manual_drop_active_) {
        // Operator override is in flight. Latch the analyser's view as
        // the section to restore to when the drop expires, but don't
        // emit a wash now.
        manual_drop_prev_sec_ = section;
        current_section_ = section;
        return;
    }

    if (!ctx.get_property("respond_to_sections").as_bool()) {
        // Analyser keeps running; we just don't act on it.
        current_section_ = section;
        return;
    }

    current_section_ = section;
    emit_wash_for_section(ctx, section, millis());
}

// =============================================================================
// Input action - Btn1 / Confirm = manual drop marker; Btn2 / Cycle = palette
// =============================================================================

void BassAndDriftShow::on_input_action(ShowContext& ctx,
                                        const hal::InputEvent& ev) {
    if (ev.action == hal::InputAction::Confirm) {
        // Manual drop marker - fires the drop palette + a peak pulse,
        // queues a restore after kManualDropHoldMs. Always wins (per
        // §1.2 closing line in architecture.md).
        const uint32_t now = millis();
        manual_drop_prev_sec_ = current_section_;
        manual_drop_active_   = true;
        manual_drop_until_ms_ = now + kManualDropHoldMs;
        current_section_ = kSectionDrop;
        emit_wash_for_section(ctx, kSectionDrop, now);
        fire_pulse_for_section(ctx, kSectionDrop, 255);
        return;
    }
    if (ev.action == hal::InputAction::Cycle) {
        const uint8_t cur = ctx.get_property("palette_set").as_enum();
        const uint8_t next = (cur + 1) % kPaletteSetCount;
        ctx.set_property("palette_set", PropertyValue::from_enum(next));
        // on_property_changed re-emits the wash.
    }
}

// =============================================================================
// Tick - manual-drop auto-restore
// =============================================================================

void BassAndDriftShow::tick(ShowContext& ctx, uint32_t now_ms) {
    if (manual_drop_active_ && now_ms >= manual_drop_until_ms_) {
        manual_drop_active_ = false;
        current_section_ = manual_drop_prev_sec_;
        emit_wash_for_section(ctx, current_section_, now_ms);
    }
}

// =============================================================================
// Property change - palette/wash_speed/respond_to_sections re-emit the wash
// =============================================================================

void BassAndDriftShow::on_property_changed(ShowContext& ctx, const char* key) {
    if (key == nullptr) return;
    // palette_set / wash_speed / target_group all change the visible wash.
    // respond_to_sections doesn't re-emit by itself; the next section
    // change picks up the new value.
    if (std::strcmp(key, "palette_set") == 0
        || std::strcmp(key, "wash_speed") == 0
        || std::strcmp(key, "target_group") == 0) {
        emit_wash_for_section(ctx, current_section_, millis());
    }
}

// =============================================================================
// Wash + pulse emission helpers
// =============================================================================

uint16_t BassAndDriftShow::cycle_ms_for_section(uint8_t section) const {
    const PaletteEntry& palette = kPalettes[0][section_to_idx(section)];
    return palette.cycle_ms;
    // Note: this is the *palette's* baked cycle_ms; emit_wash_for_section()
    // applies the section-override-vs-wash_speed logic on top.
}

void BassAndDriftShow::emit_wash_for_section(ShowContext& ctx,
                                              uint8_t section,
                                              uint32_t now_ms) {
    const uint8_t palette_idx = ctx.get_property("palette_set").as_enum();
    const uint8_t section_idx = section_to_idx(section);
    const PaletteEntry& palette = kPalettes[palette_idx][section_idx];

    // Section override wins for BuildUp / Drop / Breakdown (B0.2). Verse
    // / Chorus / fallback respect the wash_speed property.
    uint16_t cycle_ms;
    if (section_overrides_speed(section)) {
        cycle_ms = palette.cycle_ms;
    } else {
        const auto speed = static_cast<WashSpeed>(
            ctx.get_property("wash_speed").as_enum());
        cycle_ms = cycle_ms_for_speed(speed, estimated_bpm_);
    }

    LightWashEvent w{};
    w.r1 = palette.r1; w.g1 = palette.g1; w.b1 = palette.b1;
    w.r2 = palette.r2; w.g2 = palette.g2; w.b2 = palette.b2;
    w.attack         = 20;     // 2.0 s ramp-in
    w.release        = 10;     // 1.0 s default fade-out
    w.intensity      = palette.intensity;
    w.cycle_ms       = cycle_ms;
    w.ttl_seconds    = 0;      // infinite; Show owns cleanup via exit()
    w.pulse_response = 1;      // overlays accepted

    char target[8];
    build_target(target, sizeof(target),
                 ctx.get_property("target_group").as_u8());
    ctx.render_wash(target, w);

    last_wash_emit_ms_   = now_ms;
    last_wash_cycle_ms_  = cycle_ms;
}

void BassAndDriftShow::fire_pulse_for_section(ShowContext& ctx,
                                               uint8_t section,
                                               uint8_t /*strength*/) {
    const uint8_t palette_idx = ctx.get_property("palette_set").as_enum();
    const PulseColour pc = kPulseColour[palette_idx];

    // Drop section gets the peak pulse (max intensity); other sections
    // use the palette's pulse colour at full saturation. The receiver's
    // `chance` gate distributes across Lumes.
    RgbPulseEvent ev{};
    ev.r = pc.r; ev.g = pc.g; ev.b = pc.b;
    ev.attack  = pulse::T_0_MS;
    ev.sustain = (section == kSectionDrop) ? pulse::T_192_MS : pulse::T_96_MS;
    ev.release = pulse::T_192_MS;
    ev.chance  = chance_from_idx(ctx.get_property("chance").as_enum());

    char target[8];
    build_target(target, sizeof(target),
                 ctx.get_property("target_group").as_u8());
    ctx.render_fx(target, ev);

    // Starlight overlay: when enabled, fire a second low-chance pulse in
    // a contrasting palette colour on top of the bass-driven pulse. This
    // is the §1.2 "occasional accent that isn't the beat" gesture.
    if (ctx.get_property("starlight").as_bool() && section != kSectionBreakdown) {
        const PulseColour sl = kStarlightColour[palette_idx];
        RgbPulseEvent star{};
        star.r = sl.r; star.g = sl.g; star.b = sl.b;
        star.attack  = pulse::T_0_MS;
        star.sustain = pulse::T_96_MS;
        star.release = pulse::T_96_MS;
        star.chance  = pulse::CHANCE_4;
        ctx.render_fx(target, star);
    }
}

// =============================================================================
// BPM helper - identical median-of-IBIs to SimpleBeatShow
// =============================================================================

void BassAndDriftShow::update_bpm_from_buffer() {
    if (ibi_count_ < 3) return;
    uint32_t sorted[kIbiBufferSize];
    for (size_t i = 0; i < ibi_count_; ++i) sorted[i] = ibi_buffer_[i];
    for (size_t i = 1; i < ibi_count_; ++i) {
        uint32_t key = sorted[i];
        size_t j = i;
        while (j > 0 && sorted[j-1] > key) { sorted[j] = sorted[j-1]; --j; }
        sorted[j] = key;
    }
    const uint32_t med = (ibi_count_ % 2 == 1)
        ? sorted[ibi_count_ / 2]
        : (sorted[ibi_count_ / 2 - 1] + sorted[ibi_count_ / 2]) / 2;
    if (med > 50) estimated_bpm_ = 60000.0f / (float)med;
}

// =============================================================================
// LCD - operator-facing UI; lighting visible only on remote Lumes
// =============================================================================

void BassAndDriftShow::on_render(ShowContext& ctx) {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 0, "Bass & Drift", YELLOW, BLACK, 1});

    const uint8_t palette_idx = ctx.get_property("palette_set").as_enum();
    const auto    speed       = static_cast<WashSpeed>(
        ctx.get_property("wash_speed").as_enum());

    char title[32];
    std::snprintf(title, sizeof(title), " %s%s",
                  kPaletteSetNames[palette_idx],
                  ctx.paused() ? " : Paused" : "");
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 12, title, WHITE, BLACK, 3});

    // BPM + wash speed on one line.
    char status[40];
    if (estimated_bpm_ > 0.0f) {
        std::snprintf(status, sizeof(status), " %.0f BPM  %s",
                      estimated_bpm_,
                      kWashSpeedNames[static_cast<uint8_t>(speed)]);
    } else {
        std::snprintf(status, sizeof(status), " --- BPM  %s",
                      kWashSpeedNames[static_cast<uint8_t>(speed)]);
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 45, status, WHITE, BLACK, 2});

    // Section + chance.
    const char* section_name;
    if (manual_drop_active_)              section_name = "DROP*";
    else switch (current_section_) {
        case kSectionChorus:    section_name = "Chorus";    break;
        case kSectionBuildUp:   section_name = "BuildUp";   break;
        case kSectionDrop:      section_name = "Drop";      break;
        case kSectionBreakdown: section_name = "Breakdown"; break;
        case kSectionVerse:     section_name = "Verse";     break;
        default:                section_name = "Unknown";   break;
    }
    const uint8_t chance_idx = ctx.get_property("chance").as_enum();
    const char* chance_name = (chance_idx < kChanceCount)
        ? kChanceNames[chance_idx]
        : "?";
    char sec_line[40];
    std::snprintf(sec_line, sizeof(sec_line), " %s  ch:%s",
                  section_name, chance_name);
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 75, sec_line, WHITE, BLACK, 2});

    // Operator hint footer.
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        4, 128, "A: DROP  B: palette  B-hold: pick",
        WHITE, BLACK, 1});
}

// =============================================================================
// Singletons
// =============================================================================

namespace {
BassAndDriftShow s_instance;
PropertyBag      s_bag(s_instance);
ShowContext      s_ctx(s_instance, s_bag);
}  // namespace

BassAndDriftShow* bass_and_drift_show_instance()     { return &s_instance; }
PropertyBag&      bass_and_drift_show_property_bag() { return s_bag; }
ShowContext&      bass_and_drift_show_context()      { return s_ctx; }

ShowContext& BassAndDriftShow::context() {
    return s_ctx;
}

}  // namespace shows
}  // namespace nocturnation
