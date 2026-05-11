// ShowContext - services surface exposed to the active Show (Epic 4.7).
//
// Mirrors VisualisationContext from Epic 4.6: thin forwarding surface
// that lets a Show reach DAL render_fx, read/write its property bag,
// query host analyser capabilities, and ask the framework for time /
// pause state. Show code never reaches HAL/DAL directly through other
// paths - everything flows through this surface so future hosts
// (Tildagon, ...) can swap in without touching show code.
//
// Show-as-orchestrator pattern: a Show fires render_fx() targets
// explicitly with class+group routing (Epic 4.65), queries the host's
// analyser capabilities to gate optional codepaths, and reads/writes
// its property bag for persisted operator settings.

#pragma once

#include <cstdint>

#include "plugins/property_bag.h"
#include "hal/capability_mask.h"
#include "dal/dal.h"

namespace nocturnation {
namespace shows {

class Show;

class ShowContext {
public:
    ShowContext(Show& show, plugins::PropertyBag& bag);

    // -- Output -----------------------------------------------------------
    // Forward to DAL::render_fx. Shows fire targets with class+group
    // routing per Epic 4.65:
    //   ctx.render_fx("00:00", ev);   // broadcast to all classes / groups
    //   ctx.render_fx("01:00", ev);   // all Light-class devices, all groups
    //   ctx.render_fx("01:01", ev);   // Light-class, group 1 only
    bool render_fx(const char* target, const dal::RgbPulseEvent& ev);

    // -- Property bag -----------------------------------------------------
    plugins::PropertyValue get_property(const char* key) const;
    bool                   set_property(const char* key, plugins::PropertyValue value);

    // -- Capability query -------------------------------------------------
    // CapabilityMask of analyser features the host actually has. Shows
    // query this in enter() to decide which optional codepaths it can
    // engage. Complements required_capabilities() which the registry
    // uses to gate Show selection.
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
    Show&                 show()         const { return *show_; }
    plugins::PropertyBag& property_bag() const { return *bag_; }

private:
    Show*                 show_;
    plugins::PropertyBag* bag_;
    bool                  paused_         = false;
    uint32_t              entered_at_ms_  = 0;
};

}  // namespace shows
}  // namespace nocturnation
