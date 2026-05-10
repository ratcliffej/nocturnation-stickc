// NocturNation DAL implementation.
//
// Static-capacity registries (devices, drivers, subscribers) so we don't
// need dynamic allocation on the embedded target. Capacities are tunable
// constants below; raise them if a deployment hits a ceiling.

#include "dal/dal.h"
#include "hal/hal.h"
#include "drivers/local_driver.h"
#include "drivers/pixmob_ir_driver.h"
#include "drivers/espnow_broadcast_driver.h"

#include <cstring>

namespace nocturnation {
namespace dal {

// =============================================================================
// DeviceProfile helpers
// =============================================================================

bool DeviceProfile::has_output(CapabilityId cap) const {
    for (size_t i = 0; i < output_capability_count; ++i) {
        if (output_capabilities[i] == cap) return true;
    }
    return false;
}

bool DeviceProfile::has_input(CapabilityId cap) const {
    for (size_t i = 0; i < input_capability_count; ++i) {
        if (input_capabilities[i] == cap) return true;
    }
    return false;
}

bool DeviceProfile::has(CapabilityId cap) const {
    return has_output(cap) || has_input(cap);
}

// =============================================================================
// Static storage
// =============================================================================

namespace {

constexpr size_t kMaxDevices         = 32;
constexpr size_t kMaxDrivers         = 8;
constexpr size_t kMaxSubscribersEach = 8;
constexpr size_t kMaxHostCapsEach    = 8;

struct ActiveDevice {
    const char*         name;
    const DeviceProfile* profile;
    uint8_t             group_id;
};

template<typename CB>
struct Subscriber {
    const char* target;
    CB          cb;
};

ActiveDevice s_devices[kMaxDevices];
size_t       s_device_count = 0;

Driver* s_drivers[kMaxDrivers];
size_t  s_driver_count = 0;

Subscriber<DAL::AudioFrameCallback>    s_audio_subs[kMaxSubscribersEach];
Subscriber<DAL::SpectrumFrameCallback> s_spectrum_subs[kMaxSubscribersEach];
Subscriber<DAL::ButtonPressCallback>   s_button_subs[kMaxSubscribersEach];
Subscriber<DAL::EspNowInboundCallback> s_esp_subs[kMaxSubscribersEach];
Subscriber<DAL::DmxInboundCallback>    s_dmx_subs[kMaxSubscribersEach];
size_t s_audio_sub_count    = 0;
size_t s_spectrum_sub_count = 0;
size_t s_button_sub_count   = 0;
size_t s_esp_sub_count      = 0;
size_t s_dmx_sub_count      = 0;

// Composed host profile - capability arrays + the DeviceProfile struct.
CapabilityId s_host_inputs[kMaxHostCapsEach];
CapabilityId s_host_outputs[kMaxHostCapsEach];
size_t       s_host_input_count  = 0;
size_t       s_host_output_count = 0;
DeviceProfile s_host_profile;

const ActiveDevice* find_device(const char* name) {
    if (!name) return nullptr;
    for (size_t i = 0; i < s_device_count; ++i) {
        if (std::strcmp(s_devices[i].name, name) == 0) return &s_devices[i];
    }
    return nullptr;
}

Driver* find_driver_for_transport(const char* transport) {
    if (!transport) return nullptr;
    for (size_t i = 0; i < s_driver_count; ++i) {
        if (std::strcmp(s_drivers[i]->transport_name(), transport) == 0) {
            return s_drivers[i];
        }
    }
    return nullptr;
}

void compose_host_profile() {
    s_host_input_count  = 0;
    s_host_output_count = 0;

    if (hal::HAL::has(hal::Capability::Mic)) {
        s_host_inputs[s_host_input_count++] = CapabilityId::AudioFrame;
    }
    if (hal::HAL::has(hal::Capability::AnalyserSpectrumFrame)) {
        s_host_inputs[s_host_input_count++] = CapabilityId::SpectrumFrame;
    }
    if (hal::HAL::has(hal::Capability::Buttons)) {
        s_host_inputs[s_host_input_count++] = CapabilityId::ButtonPress;
    }
    if (hal::HAL::has(hal::Capability::ESPNow)) {
        s_host_inputs[s_host_input_count++] = CapabilityId::EspNowInbound;
    }

    if (hal::HAL::has(hal::Capability::Display)) {
        s_host_outputs[s_host_output_count++] = CapabilityId::DisplayShowText;
        s_host_outputs[s_host_output_count++] = CapabilityId::DisplayClear;
        s_host_outputs[s_host_output_count++] = CapabilityId::DisplayFillRect;
        s_host_outputs[s_host_output_count++] = CapabilityId::DisplayMeter;
        // The screen IS this host's primary "light" - declare RgbPulse so
        // render_fx("local", RgbPulseEvent{...}) paints the screen with the
        // attack/sustain/release envelope. On a future LED-only host without
        // Display, RgbPulse would still be declared but routed to the LED
        // surface by that host's local driver.
        s_host_outputs[s_host_output_count++] = CapabilityId::RgbPulse;
    }
    if (hal::HAL::has(hal::Capability::Battery)) {
        s_host_outputs[s_host_output_count++] = CapabilityId::BatteryLevel;
    }

    s_host_profile = DeviceProfile{
        /* type_id                  = */ "NocturNationHost",
        /* version                  = */ "1.0",
        /* transport                = */ "local",
        /* output_capabilities      = */ s_host_outputs,
        /* output_capability_count  = */ s_host_output_count,
        /* input_capabilities       = */ s_host_inputs,
        /* input_capability_count   = */ s_host_input_count,
        /* supports_groups          = */ false,
        /* max_group_id             = */ 0,
    };
}

template<typename Event>
bool dispatch_output(const char* target, CapabilityId cap, const Event& ev) {
    const ActiveDevice* device = find_device(target);
    if (!device || !device->profile) return false;
    if (!device->profile->has_output(cap)) return false;
    Driver* driver = find_driver_for_transport(device->profile->transport);
    if (!driver) return false;
    if (!driver->enabled()) return false;     // muted by config
    const bool ok = driver->send(device->group_id, ev);
    if (ok) driver->increment_send_count();
    return ok;
}

}  // anonymous namespace

// =============================================================================
// Lifecycle
// =============================================================================

void DAL::begin() {
    // HAL first - we'll read its capability list to compose the host profile.
    hal::HAL::begin();

    // Reset registries (begin() is idempotent for testing).
    s_device_count = 0;
    s_driver_count = 0;
    s_audio_sub_count    = 0;
    s_spectrum_sub_count = 0;
    s_button_sub_count   = 0;
    s_esp_sub_count      = 0;
    s_dmx_sub_count      = 0;

    compose_host_profile();

    // Register the host as the "local" device.
    register_device("local", &s_host_profile, 0);

    // Register PixMob bracelets. "all-pixmobs" is broadcast (group 0). The
    // numbered groups 1-5 cover the §8.5 Group Targeting Test cycle plus the
    // bracelet-setup flow (Set Group ID writes 1-5 today; range can extend
    // to the protocol's 1-31 maximum when constellation work needs more).
    register_device("all-pixmobs", &profiles::PixMobX4Gen3_1, 0);
    register_device("group-1",     &profiles::PixMobX4Gen3_1, 1);
    register_device("group-2",     &profiles::PixMobX4Gen3_1, 2);
    register_device("group-3",     &profiles::PixMobX4Gen3_1, 3);
    register_device("group-4",     &profiles::PixMobX4Gen3_1, 4);
    register_device("group-5",     &profiles::PixMobX4Gen3_1, 5);

    // Master broadcast target: render_fx("esp-now-broadcast", RgbPulseEvent)
    // hits this device's profile and routes through the EspNowBroadcastDriver,
    // which encodes a LIGHT_COMMAND frame and broadcasts it to any slaves on
    // the configured show channel. Group 0 (broadcast); per-group
    // esp-now-broadcast-group-N variants can be registered later for
    // targeted slave addressing without changing the driver.
    register_device("esp-now-broadcast", &profiles::EspNowBroadcast, 0);

    // Register concrete drivers. Each refuses registration when its HAL
    // prerequisite is absent (e.g. no IRTx -> PixMob driver doesn't register).
    register_driver(local_driver_instance());
    register_driver(pixmob_ir_driver_instance());
    register_driver(esp_now_broadcast_driver_instance());
}

void DAL::loop_tick() {
    hal::HAL::loop_tick();
    for (size_t i = 0; i < s_driver_count; ++i) {
        s_drivers[i]->loop_tick();
    }
}

// =============================================================================
// Registry
// =============================================================================

bool DAL::register_device(const char* name, const DeviceProfile* profile, uint8_t group_id) {
    if (!name || !profile) return false;
    if (s_device_count >= kMaxDevices) return false;
    if (find_device(name) != nullptr) return false;     // duplicate name
    s_devices[s_device_count++] = ActiveDevice{name, profile, group_id};
    return true;
}

bool DAL::has_device(const char* name) {
    return find_device(name) != nullptr;
}

const DeviceProfile* DAL::profile_of(const char* name) {
    const ActiveDevice* d = find_device(name);
    return d ? d->profile : nullptr;
}

size_t DAL::active_device_count() {
    return s_device_count;
}

const char* DAL::active_device_name(size_t index) {
    return (index < s_device_count) ? s_devices[index].name : nullptr;
}

bool DAL::supports(const char* device_name, CapabilityId cap) {
    const DeviceProfile* p = profile_of(device_name);
    return p && p->has(cap);
}

// =============================================================================
// Driver registration
// =============================================================================

bool DAL::register_driver(Driver* driver) {
    if (!driver) return false;
    if (s_driver_count >= kMaxDrivers) return false;
    if (!driver->begin()) return false;
    s_drivers[s_driver_count++] = driver;
    return true;
}

size_t DAL::registered_driver_count() {
    return s_driver_count;
}

bool DAL::set_driver_enabled(const char* transport_name, bool enabled) {
    Driver* d = find_driver_for_transport(transport_name);
    if (!d) return false;
    d->set_enabled(enabled);
    return true;
}

bool DAL::driver_enabled(const char* transport_name) {
    Driver* d = find_driver_for_transport(transport_name);
    return d ? d->enabled() : false;
}

uint32_t DAL::driver_send_count(const char* transport_name) {
    Driver* d = find_driver_for_transport(transport_name);
    return d ? d->send_count() : 0;
}

// =============================================================================
// Output dispatchers
// =============================================================================

bool DAL::fire_rgb_pulse(const char* t, const RgbPulseEvent& ev) {
    return dispatch_output(t, CapabilityId::RgbPulse, ev);
}

bool DAL::render_fx(const char* t, const RgbPulseEvent& ev) {
    return dispatch_output(t, CapabilityId::RgbPulse, ev);
}
bool DAL::fire_rgb_static(const char* t, const RgbStaticEvent& ev) {
    return dispatch_output(t, CapabilityId::RgbStatic, ev);
}
bool DAL::fire_display_show_text(const char* t, const DisplayShowTextEvent& ev) {
    return dispatch_output(t, CapabilityId::DisplayShowText, ev);
}
bool DAL::fire_display_clear(const char* t, const DisplayClearEvent& ev) {
    return dispatch_output(t, CapabilityId::DisplayClear, ev);
}
bool DAL::fire_display_fill_rect(const char* t, const DisplayFillRectEvent& ev) {
    return dispatch_output(t, CapabilityId::DisplayFillRect, ev);
}
bool DAL::fire_display_meter(const char* t, const DisplayMeterEvent& ev) {
    return dispatch_output(t, CapabilityId::DisplayMeter, ev);
}
bool DAL::fire_assign_device_group(const char* t, const AssignDeviceGroupEvent& ev) {
    return dispatch_output(t, CapabilityId::AssignDeviceGroup, ev);
}

// =============================================================================
// Subscriptions
// =============================================================================

bool DAL::subscribe_audio_frames(const char* target, AudioFrameCallback cb) {
    if (!target) return false;
    const DeviceProfile* p = profile_of(target);
    if (!p || !p->has_input(CapabilityId::AudioFrame)) return false;
    if (s_audio_sub_count >= kMaxSubscribersEach) return false;
    s_audio_subs[s_audio_sub_count++] = Subscriber<AudioFrameCallback>{target, cb};
    return true;
}

bool DAL::subscribe_spectrum_frames(const char* target, SpectrumFrameCallback cb) {
    if (!target) return false;
    const DeviceProfile* p = profile_of(target);
    if (!p || !p->has_input(CapabilityId::SpectrumFrame)) return false;
    if (s_spectrum_sub_count >= kMaxSubscribersEach) return false;
    s_spectrum_subs[s_spectrum_sub_count++] = Subscriber<SpectrumFrameCallback>{target, cb};
    return true;
}

bool DAL::subscribe_button_presses(const char* target, ButtonPressCallback cb) {
    if (!target) return false;
    const DeviceProfile* p = profile_of(target);
    if (!p || !p->has_input(CapabilityId::ButtonPress)) return false;
    if (s_button_sub_count >= kMaxSubscribersEach) return false;
    s_button_subs[s_button_sub_count++] = Subscriber<ButtonPressCallback>{target, cb};
    return true;
}

bool DAL::subscribe_esp_now_inbound(const char* target, EspNowInboundCallback cb) {
    if (!target) return false;
    const DeviceProfile* p = profile_of(target);
    if (!p || !p->has_input(CapabilityId::EspNowInbound)) return false;
    if (s_esp_sub_count >= kMaxSubscribersEach) return false;
    s_esp_subs[s_esp_sub_count++] = Subscriber<EspNowInboundCallback>{target, cb};
    return true;
}

bool DAL::subscribe_dmx_inbound(const char* target, DmxInboundCallback cb) {
    if (!target) return false;
    const DeviceProfile* p = profile_of(target);
    if (!p || !p->has_input(CapabilityId::DmxInbound)) return false;
    if (s_dmx_sub_count >= kMaxSubscribersEach) return false;
    s_dmx_subs[s_dmx_sub_count++] = Subscriber<DmxInboundCallback>{target, cb};
    return true;
}

// =============================================================================
// Input lifecycle
// =============================================================================

bool DAL::start_audio_input(const char* target,
                            uint16_t sample_rate_hz,
                            uint16_t fft_size) {
    const ActiveDevice* device = find_device(target);
    if (!device || !device->profile) return false;
    if (!device->profile->has_input(CapabilityId::AudioFrame)) return false;
    Driver* driver = find_driver_for_transport(device->profile->transport);
    if (!driver) return false;
    return driver->start_audio_input(sample_rate_hz, fft_size);
}

bool DAL::stop_audio_input(const char* target) {
    const ActiveDevice* device = find_device(target);
    if (!device || !device->profile) return false;
    if (!device->profile->has_input(CapabilityId::AudioFrame)) return false;
    Driver* driver = find_driver_for_transport(device->profile->transport);
    if (!driver) return false;
    return driver->stop_audio_input();
}

bool DAL::configure_audio_pipeline(const char* target,
                                    uint32_t sample_rate_hz,
                                    uint16_t fft_size) {
    // Forwards to the host's HAL Mic. The Mic backend declares its
    // valid operating points and rejects anything else; in Epic 4.5
    // only the canonical (16000, 512) is implemented across all hosts.
    if (!target) return false;
    const DeviceProfile* p = profile_of(target);
    if (!p || !p->has_input(CapabilityId::AudioFrame)) return false;
    auto* mic = hal::HAL::mic();
    if (!mic) return false;
    return mic->configure_audio_pipeline(sample_rate_hz, fft_size);
}

bool DAL::set_band_layout(const char* target, const char* preset_name) {
    // Only "hifi+production" is implemented in Epic 4.5 - it ships
    // both 3-band B/M/T and 8-band perceptual summaries concurrently.
    // Named alternative presets (dnb-4band-with-subbass, vocal-emphasis,
    // arbitrary JSON-defined ranges) are reserved for a future Epic;
    // the API stub exists now so future Epics extend rather than
    // re-architect.
    if (!target || !preset_name) return false;
    const DeviceProfile* p = profile_of(target);
    if (!p || !p->has_input(CapabilityId::AudioFrame)) return false;
    return std::strcmp(preset_name, "hifi+production") == 0;
}

// =============================================================================
// Synchronous queries
// =============================================================================

int DAL::battery_level(const char* target) {
    const ActiveDevice* device = find_device(target);
    if (!device || !device->profile) return -1;
    if (!device->profile->has_output(CapabilityId::BatteryLevel)) return -1;
    Driver* driver = find_driver_for_transport(device->profile->transport);
    if (!driver) return -1;
    return driver->battery_level();
}

// =============================================================================
// Event delivery (called by drivers, also exposed for tests)
// =============================================================================

void DAL::deliver_audio_frame(const char* source, const AudioFrameEvent& ev) {
    if (!source) return;
    for (size_t i = 0; i < s_audio_sub_count; ++i) {
        if (std::strcmp(s_audio_subs[i].target, source) == 0) {
            s_audio_subs[i].cb(source, ev);
        }
    }
}

void DAL::deliver_spectrum_frame(const char* source, const SpectrumFrameEvent& ev) {
    if (!source) return;
    for (size_t i = 0; i < s_spectrum_sub_count; ++i) {
        if (std::strcmp(s_spectrum_subs[i].target, source) == 0) {
            s_spectrum_subs[i].cb(source, ev);
        }
    }
}

void DAL::deliver_button_press(const char* source, const ButtonPressEvent& ev) {
    if (!source) return;
    for (size_t i = 0; i < s_button_sub_count; ++i) {
        if (std::strcmp(s_button_subs[i].target, source) == 0) {
            s_button_subs[i].cb(source, ev);
        }
    }
}

void DAL::deliver_esp_now_inbound(const char* source, const EspNowInboundEvent& ev) {
    if (!source) return;
    for (size_t i = 0; i < s_esp_sub_count; ++i) {
        if (std::strcmp(s_esp_subs[i].target, source) == 0) {
            s_esp_subs[i].cb(source, ev);
        }
    }
}

void DAL::deliver_dmx_inbound(const char* source, const DmxInboundEvent& ev) {
    if (!source) return;
    for (size_t i = 0; i < s_dmx_sub_count; ++i) {
        if (std::strcmp(s_dmx_subs[i].target, source) == 0) {
            s_dmx_subs[i].cb(source, ev);
        }
    }
}

// =============================================================================
// Built-in profile constants
// =============================================================================

namespace profiles {

namespace {
constexpr CapabilityId pixmob_x4_outputs[] = {
    CapabilityId::RgbPulse,
    CapabilityId::AssignDeviceGroup,
};

constexpr CapabilityId espnow_broadcast_outputs[] = {
    CapabilityId::RgbPulse,
};
}  // anonymous namespace

const DeviceProfile PixMobX4Gen3_1 = DeviceProfile{
    /* type_id                  = */ "PixMobX4Gen3_1",
    /* version                  = */ "1.0",
    /* transport                = */ "ir-pixmob",
    /* output_capabilities      = */ pixmob_x4_outputs,
    /* output_capability_count  = */ sizeof(pixmob_x4_outputs)/sizeof(pixmob_x4_outputs[0]),
    /* input_capabilities       = */ nullptr,
    /* input_capability_count   = */ 0,
    /* supports_groups          = */ true,
    /* max_group_id             = */ 31,
};

// EspNowBroadcast: the master->slaves wire target. Profile declares
// RgbPulse so render_fx("esp-now-broadcast", RgbPulseEvent{...}) routes
// through EspNowBroadcastDriver and emits a LIGHT_COMMAND frame.
// supports_groups + max_group_id=31 lets future code register
// esp-now-broadcast-group-N devices that pass the group id into the
// LIGHT_COMMAND target_group field without driver changes.
const DeviceProfile EspNowBroadcast = DeviceProfile{
    /* type_id                  = */ "EspNowBroadcast",
    /* version                  = */ "1.0",
    /* transport                = */ "esp-now-broadcast",
    /* output_capabilities      = */ espnow_broadcast_outputs,
    /* output_capability_count  = */ sizeof(espnow_broadcast_outputs)/sizeof(espnow_broadcast_outputs[0]),
    /* input_capabilities       = */ nullptr,
    /* input_capability_count   = */ 0,
    /* supports_groups          = */ true,
    /* max_group_id             = */ 31,
};

}  // namespace profiles

}  // namespace dal
}  // namespace nocturnation
