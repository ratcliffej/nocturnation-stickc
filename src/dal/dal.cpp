// NocturNation DAL implementation.
//
// Static-capacity registries (devices, drivers, subscribers) so we don't
// need dynamic allocation on the embedded target. Capacities are tunable
// constants below; raise them if a deployment hits a ceiling.

#include "dal/dal.h"
#include "hal/hal.h"
#include "drivers/local_driver.h"

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
Subscriber<DAL::ButtonPressCallback>   s_button_subs[kMaxSubscribersEach];
Subscriber<DAL::EspNowInboundCallback> s_esp_subs[kMaxSubscribersEach];
Subscriber<DAL::DmxInboundCallback>    s_dmx_subs[kMaxSubscribersEach];
size_t s_audio_sub_count  = 0;
size_t s_button_sub_count = 0;
size_t s_esp_sub_count    = 0;
size_t s_dmx_sub_count    = 0;

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
    return driver->send(device->group_id, ev);
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
    s_audio_sub_count  = 0;
    s_button_sub_count = 0;
    s_esp_sub_count    = 0;
    s_dmx_sub_count    = 0;

    compose_host_profile();

    // Register the host as the "local" device.
    register_device("local", &s_host_profile, 0);

    // Register the LocalDriver so that fire_display_* and subscribe_button_*
    // calls actually reach the host's HAL backends. The driver's begin()
    // refuses registration on hosts where neither display nor buttons are
    // wired (returns false), keeping the fail-silent semantics intact.
    register_driver(local_driver_instance());
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

// =============================================================================
// Output dispatchers
// =============================================================================

bool DAL::fire_rgb_pulse(const char* t, const RgbPulseEvent& ev) {
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

}  // namespace profiles

}  // namespace dal
}  // namespace nocturnation
