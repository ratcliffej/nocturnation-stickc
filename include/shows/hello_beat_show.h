// HelloBeatShow - your first Show: fires a warm orange pulse on every
// kick beat, paints its name on the Director's LCD.

#pragma once

#include "shows/show.h"
#include "plugins/property_bag.h"

namespace nocturnation {
namespace shows {

class HelloBeatShow : public Show {
public:
    // Identity. id() is your Show's permanent key (keep it <= 12 ASCII
    // chars; this is used for NVS keys). display_name() is what the
    // operator sees in the picker.
    const char* id()           const override { return "hello-beat"; }
    const char* display_name() const override { return "Hello Beat"; }

    // What hardware features your Show needs.
    hal::CapabilityMask required_capabilities() const override;

    // Hooks - the framework calls these for you.
    void on_beat_detected(ShowContext&, uint8_t strength) override;
    void on_render       (ShowContext&) override;

    // Boilerplate that gives the host access to your Show's singletons.
    ShowContext& context() override;
};

// Accessors for the singletons defined in the .cpp.
HelloBeatShow*           hello_beat_show_instance();
plugins::PropertyBag&    hello_beat_show_property_bag();
ShowContext&             hello_beat_show_context();

}  // namespace shows
}  // namespace nocturnation