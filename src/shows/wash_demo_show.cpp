#include "shows/wash_demo_show.h"
#include "shows/show_context.h"

#include "dal/dal.h"
#include "pulse/envelope.h"

namespace nocturnation {
namespace shows {

using namespace nocturnation::dal;
using nocturnation::plugins::PropertyBag;

// ---------------------------------------------------------------------------
// Section IDs used by SectionDetector. We only need to distinguish a handful
// for palette swaps; the rest fall through to the default.
// ---------------------------------------------------------------------------
namespace {
constexpr uint8_t kSectionVerse    = 1;
constexpr uint8_t kSectionChorus   = 2;
constexpr uint8_t kSectionBuildUp  = 3;
constexpr uint8_t kSectionBreakdown = 4;
constexpr uint8_t kSectionDrop     = 7;

// Build a "warm orange <-> deep purple" baseline wash (the canonical demo
// palette). Used on entry and as the fall-through palette on section
// changes that don't have their own theme.
LightWashEvent default_wash() {
    LightWashEvent w{};
    w.r1 = 255; w.g1 =  60; w.b1 =   0;     // warm orange
    w.r2 = 120; w.g2 =  30; w.b2 = 200;     // deep purple
    w.attack         = 20;                  // 2.0 s ramp-in
    w.release        = 10;                  // 1.0 s default fade-out
    w.intensity      = 200;
    w.cycle_ms       = 5000;                // 5 s A<->B<->A drift
    w.ttl_seconds    = 0;                   // infinite (we own cleanup via render_wash_end)
    w.pulse_response = 1;                   // accept PULSE as overlay
    return w;
}

LightWashEvent chorus_wash() {
    LightWashEvent w{};
    w.r1 =   0; w.g1 = 200; w.b1 = 255;     // cool cyan
    w.r2 = 255; w.g2 =   0; w.b2 = 100;     // hot magenta
    w.attack         = 15; w.release = 10;
    w.intensity      = 230;                 // brighter for chorus
    w.cycle_ms       = 3000;                // faster drift than verse
    w.ttl_seconds    = 0;
    w.pulse_response = 1;
    return w;
}

LightWashEvent breakdown_wash() {
    LightWashEvent w{};
    w.r1 =  20; w.g1 =  30; w.b1 =  80;     // deep cool both ends; near-static
    w.r2 =  40; w.g2 =  50; w.b2 = 120;
    w.attack         = 30; w.release = 30;
    w.intensity      = 100;                 // dimmer for breakdown
    w.cycle_ms       = 0;                   // hold (no drift; quiet)
    w.ttl_seconds    = 0;
    w.pulse_response = 0;                   // ignore pulses; let the wash breathe
    return w;
}

}  // namespace

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------

hal::CapabilityMask WashDemoShow::required_capabilities() const {
    // Mic is needed for beats + sections. ESPNow is implicit on the Director.
    return hal::make_capability_mask(hal::Capability::Mic);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void WashDemoShow::enter(ShowContext& ctx) {
    ctx.render_wash("00:00", default_wash());
}

void WashDemoShow::exit(ShowContext& ctx) {
    // 1.0 s fade to black on the way out.
    ctx.render_wash_end("00:00", 10);
}

// ---------------------------------------------------------------------------
// Beat -> overlay pulse on the moving wash baseline
// ---------------------------------------------------------------------------

void WashDemoShow::on_beat_detected(ShowContext& ctx, uint8_t /*strength*/) {
    RgbPulseEvent p{};
    p.r = 255; p.g = 255; p.b = 255;
    p.attack  = pulse::T_0_MS;
    p.sustain = pulse::T_96_MS;
    p.release = pulse::T_192_MS;
    p.chance  = pulse::CHANCE_100;
    ctx.render_fx("00:00", p);
}

// ---------------------------------------------------------------------------
// Section change -> swap the wash palette (supersedes the previous wash)
// ---------------------------------------------------------------------------

void WashDemoShow::on_section_change(ShowContext& ctx, uint8_t section) {
    switch (section) {
        case kSectionChorus:
        case kSectionBuildUp:
        case kSectionDrop:
            ctx.render_wash("00:00", chorus_wash());
            break;
        case kSectionBreakdown:
            ctx.render_wash("00:00", breakdown_wash());
            break;
        case kSectionVerse:
        default:
            ctx.render_wash("00:00", default_wash());
            break;
    }
}

// ---------------------------------------------------------------------------
// Render the Director's own LCD label
// ---------------------------------------------------------------------------

void WashDemoShow::on_render(ShowContext&) {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        /*x=*/40, /*y=*/55,
        /*text=*/"Wash Demo",
        /*fg=*/WHITE, /*bg=*/BLACK,
        /*size=*/2});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        /*x=*/30, /*y=*/85,
        /*text=*/"(integration test)",
        /*fg=*/WHITE, /*bg=*/BLACK,
        /*size=*/1});
}

// ---------------------------------------------------------------------------
// Singletons + accessors
// ---------------------------------------------------------------------------

namespace {
WashDemoShow s_instance;
PropertyBag  s_bag(s_instance);
ShowContext  s_ctx(s_instance, s_bag);
}  // namespace

WashDemoShow* wash_demo_show_instance()     { return &s_instance; }
PropertyBag&  wash_demo_show_property_bag() { return s_bag; }
ShowContext&  wash_demo_show_context()      { return s_ctx; }
ShowContext&  WashDemoShow::context()       { return s_ctx; }

}  // namespace shows
}  // namespace nocturnation
