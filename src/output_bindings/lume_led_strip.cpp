// LumeLedStripBinding - implementation (Epic 12 B3).

#include "output_bindings/lume_led_strip.h"
#include "output_bindings/output_binding_context.h"

#include "dal/dal.h"
#include "dal/drivers/led_strip_driver.h"
#include "transport/espnow/frame.h"

namespace nocturnation {
namespace output_bindings {

using namespace nocturnation::dal;
using nocturnation::plugins::PropertyBag;

hal::CapabilityMask LumeLedStripBinding::required_capabilities() const {
    return hal::make_capability_mask(hal::Capability::AddressableLeds);
}

void LumeLedStripBinding::on_light_pulse(OutputBindingContext& ctx,
                                          const RgbPulseEvent& ev) {
    auto* drv = led_strip_driver_instance();
    if (!drv || !drv->enabled()) return;
    drv->send(ctx.current_target_group(), ev);
    drv->increment_send_count();
}

void LumeLedStripBinding::on_light_wash(
    OutputBindingContext& ctx,
    const transport::espnow::LightWashPayload& p) {
    auto* drv = led_strip_driver_instance();
    if (!drv || !drv->enabled()) return;

    LightWashEvent ev{};
    ev.r1 = p.r1; ev.g1 = p.g1; ev.b1 = p.b1;
    ev.r2 = p.r2; ev.g2 = p.g2; ev.b2 = p.b2;
    ev.attack         = p.attack;
    ev.release        = p.release;
    ev.intensity      = p.intensity;
    ev.cycle_ms       = p.cycle_ms;
    ev.ttl_seconds    = p.ttl_seconds;
    ev.pulse_response = p.pulse_response;
    drv->send_wash(ctx.current_target_group(), ev);
    drv->increment_send_count();
}

void LumeLedStripBinding::on_light_wash_end(OutputBindingContext& ctx,
                                              uint8_t release_time) {
    auto* drv = led_strip_driver_instance();
    if (!drv || !drv->enabled()) return;
    drv->send_wash_end(ctx.current_target_group(), release_time);
    drv->increment_send_count();
}

void LumeLedStripBinding::on_light_wash_pulse(OutputBindingContext& ctx,
                                                const RgbPulseEvent& ev) {
    auto* drv = led_strip_driver_instance();
    if (!drv || !drv->enabled()) return;
    drv->send_wash_pulse(ctx.current_target_group(), ev);
    drv->increment_send_count();
}

// =============================================================================
// Singletons
// =============================================================================

namespace {
LumeLedStripBinding  s_instance;
PropertyBag          s_bag(s_instance);    // empty for Phase 1
OutputBindingContext s_ctx(s_instance, s_bag);
}  // namespace

LumeLedStripBinding*  lume_led_strip_instance()    { return &s_instance; }
PropertyBag&          lume_led_strip_property_bag() { return s_bag; }
OutputBindingContext& lume_led_strip_context()     { return s_ctx; }

}  // namespace output_bindings
}  // namespace nocturnation
