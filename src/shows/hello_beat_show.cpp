#include "shows/hello_beat_show.h"
#include "shows/show_context.h"

#include "dal/dal.h"
#include "pulse/envelope.h"          // pulse::T_* / pulse::CHANCE_* descriptors

namespace nocturnation {
namespace shows {

using namespace nocturnation::dal;
using nocturnation::plugins::PropertyBag;

// ---------------------------------------------------------------------------
// Capability declaration
// ---------------------------------------------------------------------------
//
// We only need the microphone (so the analyser delivers beats to us).
// ESP-NOW and Display are required by the Director mode itself, so the
// Show doesn't have to redeclare them.

hal::CapabilityMask HelloBeatShow::required_capabilities() const {
    return hal::make_capability_mask(hal::Capability::Mic);
}

// ---------------------------------------------------------------------------
// Hook: a kick beat was detected
// ---------------------------------------------------------------------------
//
// `strength` is 0-255 (1-255 in practice, since 0 means "no beat").
// We ignore it for now and always fire at full intensity.

void HelloBeatShow::on_beat_detected(ShowContext& ctx,
                                      uint8_t /*strength*/) {
    (void)ctx;   // we don't need the context this time

    RgbPulseEvent ev{};
    ev.r = 255; ev.g = 60; ev.b = 0;        // warm orange (try your own)
    ev.attack  = pulse::T_0_MS;            // jump straight to full
    ev.sustain = pulse::T_96_MS;           // hold for ~100 ms
    ev.release = pulse::T_480_MS;          // fade for ~half a second
    ev.chance  = pulse::CHANCE_100;        // every bracelet responds

    // "00:00" means "broadcast - every device class, every group".
    // One call reaches: ESP-NOW (every Lume), this Stick's own IR LED,
    // and this Stick's own LCD (a colour flash).
    DAL::render_fx("00:00", ev);
}

// ---------------------------------------------------------------------------
// Hook: redraw the LCD (~20 Hz, when no overlay is open)
// ---------------------------------------------------------------------------
//
// The host clears nothing for you - paint the whole canvas.

void HelloBeatShow::on_render(ShowContext& ctx) {
    (void)ctx;

    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        /*x=*/40, /*y=*/55,
        /*text=*/"Hello Beat",
        /*fg=*/WHITE, /*bg=*/BLACK,
        /*size=*/2});
}

// ---------------------------------------------------------------------------
// Singletons + accessors (boilerplate; don't edit)
// ---------------------------------------------------------------------------
//
// The framework needs one instance of your Show, one PropertyBag for
// its settings (empty for now), and one ShowContext bound to both.

namespace {
HelloBeatShow s_instance;
PropertyBag   s_bag(s_instance);
ShowContext   s_ctx(s_instance, s_bag);
}  // namespace

HelloBeatShow* hello_beat_show_instance()     { return &s_instance; }
PropertyBag&   hello_beat_show_property_bag() { return s_bag; }
ShowContext&   hello_beat_show_context()      { return s_ctx; }
ShowContext&   HelloBeatShow::context()       { return s_ctx; }

}  // namespace shows
}  // namespace nocturnation