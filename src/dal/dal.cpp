// NocturNation DAL implementation.
//
// Static-capacity registries (devices, drivers, subscribers) so we don't
// need dynamic allocation on the embedded target. Capacities are tunable
// constants below; raise them if a deployment hits a ceiling.

#include "dal/dal.h"
#include "dal/render_target.h"
#include "hal/hal.h"
#include "drivers/local_driver.h"
#include "drivers/pixmob_ir_driver.h"
#include "drivers/espnow_broadcast_driver.h"

#include <cstdio>
#include <cstdlib>
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

// Headroom for: local (1) + all-pixmobs (1) + group-1..group-31 (31) +
// esp-now-broadcast (1) = 34 today, plus comfortable room for future
// classes' broadcast targets (e.g. esp-now-tildagon-*) without bumping
// again. Epic 4.65 Block 6 pushed past the prior 32 cap.
constexpr size_t kMaxDevices         = 48;
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
Subscriber<DAL::InputActionCallback>   s_input_subs[kMaxSubscribersEach];
Subscriber<DAL::EspNowInboundCallback> s_esp_subs[kMaxSubscribersEach];
Subscriber<DAL::DmxInboundCallback>    s_dmx_subs[kMaxSubscribersEach];
size_t s_audio_sub_count    = 0;
size_t s_spectrum_sub_count = 0;
size_t s_button_sub_count   = 0;
size_t s_input_sub_count    = 0;
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

// Structured-target dispatch. Routes through the "esp-now-broadcast"
// transport driver (Lumes filter on receive per Epic 4.65 Block 5)
// AND fires the Director's own IR transmit when the target class is
// the addressing wildcard (0) or Light (1) - the Director is "its own
// Lume" with respect to its IR output, so a `render_fx("01:02", ev)`
// fires the Director's IR LED with PixMob group byte 2 (matching what
// a Lume's relay PixMobIrBinding would emit downstream). Gated on:
//   - target_class in {0, 1} so non-Light routes (Screen, etc.) don't
//     fire IR
//   - target rgb non-zero so the Off colour (operator mute) doesn't
//     fire IR (matches Pulse::fire's pre-Epic-4.7 gate)
//   - ir-pixmob driver enabled, so Config > IR > Disable mutes IR
//     without the Show needing to know
// Non-ESPNow drivers degrade gracefully via the Driver base's 3-arg
// send default that forwards to the 2-arg dropping the class.
bool dispatch_output_class_group(uint8_t target_class,
                                  uint8_t target_group,
                                  const RgbPulseEvent& ev) {
    // 1. Wire to Lumes via ESP-NOW broadcast.
    Driver* wire = find_driver_for_transport("esp-now-broadcast");
    bool wire_ok = false;
    if (wire && wire->enabled()) {
        wire_ok = wire->send(target_class, target_group, ev);
        if (wire_ok) wire->increment_send_count();
    }

    // 2. Director-local IR loopback. Treats the Director as if it were its
    // own Lume: target_group is passed through as the PixMob protocol
    // group byte, so the Director's IR LED fires with the same per-group
    // filter the Lume's relay binding would apply.
    if (target_class == 0 || target_class == 1) {
        Driver* ir = find_driver_for_transport("ir-pixmob");
        if (ir && ir->enabled()) {
            ir->send(target_group, ev);
            ir->increment_send_count();
        }
    }

    // 3. Director-local screen loopback. When target_class is wildcard
    // (0) or Screen (2), also fire the LocalDriver so the Director's
    // own screen pulses alongside the wire / IR. LocalDriver gates
    // its RgbPulse handler internally on the Config > Display > Pulse
    // flag, so operators who want a quiet screen can disable it there
    // without the Show or Mode needing to know.
    if (target_class == 0 || target_class == 2) {
        Driver* local = find_driver_for_transport("local");
        if (local && local->enabled()) {
            local->send(target_group, ev);
            local->increment_send_count();
        }
    }
    return wire_ok;
}

}  // anonymous namespace

// Epic 4.65 Block 4: structured "<hex_class>:<hex_group>" target parser.
// Declared in include/dal/render_target.h so native tests can hit it
// directly. Returns true and populates out_class / out_group on a valid
// two-field hex pair (e.g. "00:00", "01:07", "ff:ff"). Returns false for
// any legacy target name ("local", "all-pixmobs", "esp-now-broadcast",
// "group-N") which contain no colon and fall back through to
// dispatch_output's name-based lookup.
bool parse_target_class_group(const char* target,
                               uint8_t&    out_class,
                               uint8_t&    out_group) {
    if (!target || !*target) return false;
    const char* colon = std::strchr(target, ':');
    if (!colon || colon == target || *(colon + 1) == '\0') return false;

    char* end = nullptr;
    const unsigned long c = std::strtoul(target, &end, 16);
    if (end != colon || c > 0xFFul) return false;
    const unsigned long g = std::strtoul(colon + 1, &end, 16);
    if (!end || *end != '\0' || g > 0xFFul) return false;

    out_class = static_cast<uint8_t>(c);
    out_group = static_cast<uint8_t>(g);
    return true;
}

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
    s_input_sub_count    = 0;
    s_esp_sub_count      = 0;
    s_dmx_sub_count      = 0;

    compose_host_profile();

    // Register the host as the "local" device.
    register_device("local", &s_host_profile, 0);

    // Register PixMob bracelets. "all-pixmobs" is broadcast (group 0).
    // "group-1".."group-31" cover the PixMob protocol's full native group
    // range (Epic 4.65 Block 6 extended this from the original 1-5 cap
    // which was a LumeMode::ir_target_name artifact; the protocol always
    // supported the full 5-bit field). Names use string-literal pointers
    // because register_device stores the pointer rather than copying.
    register_device("all-pixmobs", &profiles::PixMobX4Gen3_1, 0);
    static constexpr const char* kGroupDeviceNames[31] = {
        "group-1",  "group-2",  "group-3",  "group-4",  "group-5",
        "group-6",  "group-7",  "group-8",  "group-9",  "group-10",
        "group-11", "group-12", "group-13", "group-14", "group-15",
        "group-16", "group-17", "group-18", "group-19", "group-20",
        "group-21", "group-22", "group-23", "group-24", "group-25",
        "group-26", "group-27", "group-28", "group-29", "group-30",
        "group-31",
    };
    for (uint8_t g = 1; g <= 31; ++g) {
        register_device(kGroupDeviceNames[g - 1], &profiles::PixMobX4Gen3_1, g);
    }

    // Epic 4.65 Block 9: the legacy "esp-now-broadcast" device name is no
    // longer registered. All Director broadcast call sites moved to the
    // structured "<class>:<group>" target form in Block 7, which routes
    // directly via find_driver_for_transport("esp-now-broadcast") and
    // bypasses the device registry. The EspNowBroadcastDriver still
    // claims the "esp-now-broadcast" transport at the driver layer; only
    // the named device entry is gone.

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
    // Epic 4.65 Block 4: structured "<hex_class>:<hex_group>" targets
    // route to the ESP-NOW broadcaster with both fields on the LIGHT_PULSE
    // payload, bypassing the legacy name-based device lookup. Legacy names
    // ("local", "all-pixmobs", "group-N", "esp-now-broadcast") fall
    // through to dispatch_output for now; Block 9 removes them once vis
    // / binding call sites have migrated.
    uint8_t cls = 0, grp = 0;
    if (parse_target_class_group(t, cls, grp)) {
        return dispatch_output_class_group(cls, grp, ev);
    }
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

size_t DAL::unsubscribe_spectrum_frames(const char* target) {
    if (!target) return 0;
    // Compact in place: shift any non-matching entries down, drop matches.
    size_t removed = 0;
    size_t write = 0;
    for (size_t read = 0; read < s_spectrum_sub_count; ++read) {
        if (std::strcmp(s_spectrum_subs[read].target, target) == 0) {
            ++removed;
            continue;
        }
        if (write != read) s_spectrum_subs[write] = s_spectrum_subs[read];
        ++write;
    }
    s_spectrum_sub_count = write;
    return removed;
}

bool DAL::subscribe_button_presses(const char* target, ButtonPressCallback cb) {
    if (!target) return false;
    const DeviceProfile* p = profile_of(target);
    if (!p || !p->has_input(CapabilityId::ButtonPress)) return false;
    if (s_button_sub_count >= kMaxSubscribersEach) return false;
    s_button_subs[s_button_sub_count++] = Subscriber<ButtonPressCallback>{target, cb};
    return true;
}

// Input actions are derived from button presses by the host's input
// mapper - same capability gate as subscribe_button_presses, since a
// host that has buttons is the host that emits InputActions.
bool DAL::subscribe_input_actions(const char* target, InputActionCallback cb) {
    if (!target) return false;
    const DeviceProfile* p = profile_of(target);
    if (!p || !p->has_input(CapabilityId::ButtonPress)) return false;
    if (s_input_sub_count >= kMaxSubscribersEach) return false;
    s_input_subs[s_input_sub_count++] = Subscriber<InputActionCallback>{target, cb};
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
// Subscriber-count queries (Epic 4.6 Block 7 - pipeline gating)
// =============================================================================
//
// The LocalDriver's mic-frame callback uses has_spectrum_frame_subscribers()
// to decide whether to assemble and dispatch a SpectrumFrameEvent. The
// underlying frame.spectrum FFT roll-up still runs unconditionally because
// BeatDetector consumes it in-pipeline.

size_t DAL::spectrum_frame_subscriber_count() {
    return s_spectrum_sub_count;
}

bool DAL::has_spectrum_frame_subscribers() {
    return s_spectrum_sub_count > 0;
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

void DAL::deliver_input_action(const char* source, const hal::InputEvent& ev) {
    if (!source) return;
    for (size_t i = 0; i < s_input_sub_count; ++i) {
        if (std::strcmp(s_input_subs[i].target, source) == 0) {
            s_input_subs[i].cb(source, ev);
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

// EspNowBroadcast: the Director->Lumes wire target. Profile declares
// RgbPulse so render_fx("esp-now-broadcast", RgbPulseEvent{...}) routes
// through EspNowBroadcastDriver and emits a LIGHT_PULSE frame.
// supports_groups + max_group_id=31 lets future code register
// esp-now-broadcast-group-N devices that pass the group id into the
// LIGHT_PULSE target_group field without driver changes.
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
