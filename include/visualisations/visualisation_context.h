// VisualisationContext - services surface exposed to the active vis.
//
// The framework owns one context per active visualisation and threads
// it into every hook on Visualisation. Vis code never reaches HAL/DAL
// directly through other paths - everything flows through this surface
// so future hosts (Tildagon, ...) can be swapped in without touching
// vis code.
//
// Vis-as-orchestrator pattern: a vis fires render_fx() targets
// explicitly (see the user's "explicit multi-target, no auto-forward"
// decision), queries the host's analyser capabilities to gate optional
// codepaths, and reads/writes its property bag for persisted user
// settings.

#pragma once

#include <cstdint>

#include "plugins/property_bag.h"
#include "hal/capability_mask.h"
#include "dal/dal.h"

namespace nocturnation {
namespace visualisations {

class Visualisation;

class VisualisationContext {
public:
    VisualisationContext(Visualisation& vis, plugins::PropertyBag& bag);

    // -- Output -----------------------------------------------------------
    // Forward to DAL::render_fx. Vis fires multiple targets explicitly:
    //   ctx.render_fx("local",             ev);   // Director screen pulse
    //   ctx.render_fx("all-pixmobs",       ev);   // IR PixMob fan-out
    //   ctx.render_fx("esp-now-broadcast", ev);   // wire to Lumes
    bool render_fx(const char* target, const dal::RgbPulseEvent& ev);

    // -- Property bag -----------------------------------------------------
    plugins::PropertyValue get_property(const char* key) const;
    bool                   set_property(const char* key, plugins::PropertyValue value);

    // -- Capability query -------------------------------------------------
    // CapabilityMask of analyser features the host actually has. Vis
    // queries this in enter() to decide which optional codepaths it
    // can engage. (This complements the static required_capabilities()
    // that the registry uses to gate vis selection.)
    hal::CapabilityMask analyser_caps() const;

    // -- Framework-managed state ------------------------------------------
    bool paused()           const { return paused_; }
    void set_paused(bool p)       { paused_ = p; }

    // Time helpers. now_ms uses the same source as the framework's
    // loop_tick (millis() on Arduino, the test seam in mode_machine on
    // native). since_enter_ms uses the framework-recorded entered_at_ms.
    uint32_t now_ms()         const;
    uint32_t since_enter_ms() const;

    void mark_entered(uint32_t now_ms);

    // -- Identity ---------------------------------------------------------
    Visualisation&         visualisation() const { return *vis_; }
    plugins::PropertyBag&  property_bag()  const { return *bag_; }

private:
    Visualisation*        vis_;
    plugins::PropertyBag* bag_;
    bool                  paused_         = false;
    uint32_t              entered_at_ms_  = 0;
};

}  // namespace visualisations
}  // namespace nocturnation
