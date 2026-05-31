// WashDemoShow - integration-test Show for Epic 6C's wash machinery.
//
// Not a user-facing show. It exists to exercise the full DAL/wire/render
// path for the WASH-family API across one continuous "session":
//   - on enter:  render_wash with a slow-drifting orange<->purple baseline.
//   - on beat:   render_fx (regular pulse) - overlays additively on the
//                live wash baseline on capable bindings, fires as a normal
//                pulse on PixMob-class targets.
//   - on section change: a new render_wash to refresh the palette per
//                section, supersedes the previous wash on the same target.
//   - on exit:   render_wash_end with 1.0 s release, cancelling the wash.
//
// Phase E uses this Show to verify the Director side end-to-end on a
// serial monitor or logic analyser; Phase F's hardware integration test
// uses the visible result on a StickC screen (LocalDisplayBinding).

#pragma once

#include "shows/show.h"
#include "plugins/property_bag.h"

namespace nocturnation {
namespace shows {

class WashDemoShow : public Show {
public:
    const char* id()           const override { return "wash-demo"; }
    const char* display_name() const override { return "Wash Demo"; }

    hal::CapabilityMask required_capabilities() const override;

    void enter(ShowContext&) override;
    void exit (ShowContext&) override;

    void on_beat_detected   (ShowContext&, uint8_t strength) override;
    void on_section_change  (ShowContext&, uint8_t section)  override;
    void on_render          (ShowContext&) override;

    ShowContext& context() override;
};

WashDemoShow*           wash_demo_show_instance();
plugins::PropertyBag&   wash_demo_show_property_bag();
ShowContext&            wash_demo_show_context();

}  // namespace shows
}  // namespace nocturnation
