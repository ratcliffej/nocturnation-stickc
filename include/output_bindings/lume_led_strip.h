// LumeLedStripBinding - Lume-side render of inbound wash + pulse events
// onto an addressable LED strip (SK6812 / WS2812 family). Epic 12 B3.
//
// One binding instance per host. Registered automatically when the host
// declares Capability::AddressableLeds - the binding's required_capabilities()
// guards activation, so a StickC with no Grove strip wired in (Config
// "LED Strip" disabled) silently skips registration.
//
// Routing model: thin forward to LedStripDriver. The driver owns the
// wash state machine (anchor RGB, drift phase, attack/release, intensity,
// TTL) and the sparkle slot table; the binding just hands events off.
// Mirrors the PixMobIrBinding -> PixMobIRDriver split.
//
// DeviceClass: Light. The strip and the PixMob bracelets are both
// "light-only multi-emitter" devices to the orchestrator. Phase 1 lands
// them on the same target axis so a Director cue addressing Light:0
// reaches every strip and every bracelet in IR range with the same
// wash colour. A separate DeviceClass::LightStrip would land in Phase 2
// when per-pixel events (crawl, rainbow, gradient) need distinct
// addressing.
//
// Group filter: local binding (is_relay() == false). The host's
// slv_group filter applies upstream of this binding via LumeMode's
// per-binding dispatch.
//
// Power profile: defaults (event-driven; the driver's loop_tick handles
// the per-frame render cadence, not the binding's tick()).

#pragma once

#include <cstdint>

#include "output_bindings/output_binding.h"
#include "plugins/property_bag.h"

namespace nocturnation {
namespace output_bindings {

class LumeLedStripBinding : public OutputBinding {
public:
    const char* id()           const override { return "led-strip"; }
    const char* display_name() const override { return "LED Strip"; }

    hal::DeviceClass device_class() const override {
        return hal::DeviceClass::Light;
    }

    hal::CapabilityMask required_capabilities() const override;

    BindingCapabilities capabilities() const override {
        return BindingCapabilities{
            /*can_pulse=*/   true,
            /*can_wash=*/    true,
            /*can_overlay=*/ true,
        };
    }

    // Epic 13: handlers update in-RAM state on LedStripDriver
    // (CHANCE roll + pixel envelope start-time stamps for pulses;
    // wash anchor RGB + drift phase for wash). No SPI / NeoPixel
    // bit-bang here - that's deferred to LedStripDriver::render() on
    // loop_tick. Safe to call from the WiFi receive callback.
    // CRITICAL: stamping pulse start-time here (vs in the deferred
    // loop_tick fan-out) eliminates inter-Lume render variance: all
    // strips anchor to the same broadcast-receipt moment, so
    // simultaneous sparkles land in unison across the fleet.
    bool can_render_in_callback() const override { return true; }

    // Hooks. Each one forwards to LedStripDriver::send_* - the driver
    // owns the wash state machine and the sparkle slot table. The
    // binding has no per-instance state today.
    void on_light_pulse     (OutputBindingContext&,
                              const dal::RgbPulseEvent&) override;
    void on_light_wash      (OutputBindingContext&,
                              const transport::espnow::LightWashPayload&) override;
    void on_light_wash_end  (OutputBindingContext&, uint8_t release_time,
                             uint8_t led_mode = 0,
                             uint8_t led_modifier1 = 0,
                             uint8_t led_modifier2 = 0) override;
    void on_light_wash_pulse(OutputBindingContext&,
                              const dal::RgbPulseEvent&) override;
};

// Singletons. main.cpp registers via:
//   output_binding_registry().register_plugin(lume_led_strip_instance());
LumeLedStripBinding*  lume_led_strip_instance();
plugins::PropertyBag& lume_led_strip_property_bag();
OutputBindingContext& lume_led_strip_context();

}  // namespace output_bindings
}  // namespace nocturnation
