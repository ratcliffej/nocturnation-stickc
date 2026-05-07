// NocturNation Device Abstraction Layer (DAL)
//
// Interface contract per docs/dal-design.md. Sits above the HAL and below
// the Orchestration layer. The DAL is the only thing application code talks
// to; it owns the HAL's lifecycle and is the only caller of hal::HAL::*.
//
// Design highlights:
//   - C++ structs at compile time (JSON-loaded profiles deferred).
//   - Strongly typed event structs per capability; no stringly-typed bags.
//   - Static active-device registry populated in DAL::begin(); dynamic
//     registration is a non-breaking future addition.
//   - Helper-per-capability fire_*() methods (templated dispatch is a
//     possible refactor when we hit ~15 capabilities).
//   - Drivers are per-transport; each overrides only the capability sends
//     it actually supports. Default returns false (silent fail).

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include "hal/hal.h"
#include "pixmob_protocol.h"

namespace nocturnation {
namespace dal {

// =============================================================================
// Capability identifiers (semantic, distinct from hal::Capability)
// =============================================================================

enum class CapabilityId : uint16_t {
    // ----- Output -----
    RgbPulse = 0,         // RGB with attack/sustain/release envelope
    RgbStatic,            // Plain RGB, no envelope
    DisplayShowText,
    DisplayClear,
    DisplayFillRect,
    DisplayMeter,

    // ----- Input -----
    AudioFrame,           // Spectrum frames from a host's mic
    ButtonPress,          // Button events from a host's buttons
    EspNowInbound,        // Inbound ESP-NOW peer messages (Epic 4+)
    DmxInbound,           // Inbound DMX instructions (Epic 7+)

    _Count
};

// =============================================================================
// Device profile
// =============================================================================

struct DeviceProfile {
    const char*         type_id;        // "PixMobX4Gen3_1", "NocturNationHost", ...
    const char*         version;
    const char*         transport;      // "ir-pixmob", "local", "esp-now-nocturnation"
    const CapabilityId* output_capabilities;
    size_t              output_capability_count;
    const CapabilityId* input_capabilities;
    size_t              input_capability_count;
    bool                supports_groups;
    uint8_t             max_group_id;   // inclusive; 0 = broadcast/all

    bool has_output(CapabilityId cap) const;
    bool has_input(CapabilityId cap) const;
    bool has(CapabilityId cap) const;
};

// =============================================================================
// Output event types (passed to fire_*())
// =============================================================================

struct RgbPulseEvent {
    uint8_t        r, g, b;
    pixmob::Time   attack;
    pixmob::Time   sustain;
    pixmob::Time   release;
    pixmob::Chance chance;
};

struct RgbStaticEvent {
    uint8_t r, g, b;
};

struct DisplayShowTextEvent {
    int         x, y;
    const char* text;
    uint16_t    fg_color, bg_color;
    uint8_t     size;
};

struct DisplayClearEvent {
    uint16_t color;
};

struct DisplayFillRectEvent {
    int      x, y, w, h;
    uint16_t color;
};

struct DisplayMeterEvent {
    int      x, y, w, h;
    float    ratio;            // 0.0 .. 1.0
    uint16_t bar_color, frame_color, threshold_color;
    float    threshold_ratio;  // <0 to skip the threshold marker
};

// =============================================================================
// Input event types (delivered to subscribers)
// =============================================================================

struct AudioFrameEvent {
    uint32_t timestamp_ms;
    float    bass_energy, mid_energy, treble_energy, overall_rms;
};

struct ButtonPressEvent {
    hal::ButtonId    id;
    hal::ButtonEvent kind;
};

struct EspNowInboundEvent {
    uint32_t       timestamp_ms;
    uint8_t        peer_mac[6];
    const uint8_t* data;
    size_t         len;
    int8_t         rssi;
};

struct DmxInboundEvent {
    uint32_t       timestamp_ms;
    uint16_t       universe;
    uint16_t       address;
    const uint8_t* data;
    size_t         len;
};

// =============================================================================
// Driver base class
// =============================================================================
//
// One driver per transport. Each driver overrides send() only for the
// capabilities its transport actually supports; the default returns false
// (silent fail), which is what bubbles up through DAL::fire_*() to callers.
//
// Drivers reach hardware via hal::HAL::*, not vendor SDKs.

class Driver {
public:
    virtual ~Driver() = default;

    virtual const char* transport_name() const = 0;   // "ir-pixmob", "local", ...
    virtual bool        begin()                = 0;
    virtual void        loop_tick()            {}     // optional

    // Output dispatch: one overload per output capability. Default = unsupported.
    virtual bool send(uint8_t /*group_id*/, const RgbPulseEvent&)         { return false; }
    virtual bool send(uint8_t /*group_id*/, const RgbStaticEvent&)        { return false; }
    virtual bool send(uint8_t /*group_id*/, const DisplayShowTextEvent&)  { return false; }
    virtual bool send(uint8_t /*group_id*/, const DisplayClearEvent&)     { return false; }
    virtual bool send(uint8_t /*group_id*/, const DisplayFillRectEvent&)  { return false; }
    virtual bool send(uint8_t /*group_id*/, const DisplayMeterEvent&)     { return false; }

    // Input lifecycle hooks. Called by DAL::start_audio_input / stop_audio_input
    // on the driver registered for the target's transport. Drivers that do
    // not source audio frames default to no-op (returns false).
    virtual bool start_audio_input(uint16_t /*sample_rate_hz*/,
                                   uint16_t /*fft_size*/) { return false; }
    virtual bool stop_audio_input() { return false; }
};

// =============================================================================
// DAL - the public facade
// =============================================================================

class DAL {
public:
    // -------------------------------------------------------------------------
    // Lifecycle. main.cpp calls begin() once and loop_tick() each iteration.
    // The DAL internally calls hal::HAL::begin()/loop_tick() at the start of
    // each. Application code should not include hal/hal.h.
    // -------------------------------------------------------------------------
    static void begin();
    static void loop_tick();

    // -------------------------------------------------------------------------
    // Active-device registry
    // -------------------------------------------------------------------------
    static bool                 register_device(const char* name,
                                                 const DeviceProfile* profile,
                                                 uint8_t group_id);
    static bool                 has_device(const char* name);
    static const DeviceProfile* profile_of(const char* name);
    static size_t               active_device_count();
    static const char*          active_device_name(size_t index);

    // True iff the named device's profile declares the given capability
    // (input or output). False on unknown device or unsupported capability.
    static bool supports(const char* device_name, CapabilityId cap);

    // -------------------------------------------------------------------------
    // Driver registration. Internal callers (DAL::begin) register protocol
    // drivers conditional on hal::HAL::has(...). Tests can register their
    // own drivers too.
    // -------------------------------------------------------------------------
    static bool   register_driver(Driver* driver);
    static size_t registered_driver_count();

    // -------------------------------------------------------------------------
    // Output dispatch. Returns false on unknown target, unsupported
    // capability for that target's profile, or no driver registered for
    // that target's transport.
    // -------------------------------------------------------------------------
    static bool fire_rgb_pulse        (const char* target, const RgbPulseEvent&);
    static bool fire_rgb_static       (const char* target, const RgbStaticEvent&);
    static bool fire_display_show_text(const char* target, const DisplayShowTextEvent&);
    static bool fire_display_clear    (const char* target, const DisplayClearEvent&);
    static bool fire_display_fill_rect(const char* target, const DisplayFillRectEvent&);
    static bool fire_display_meter    (const char* target, const DisplayMeterEvent&);

    // -------------------------------------------------------------------------
    // Input subscription. Returns false on unknown target or capability not
    // declared as an input on that target's profile. Callbacks fire from
    // DAL::loop_tick().
    // -------------------------------------------------------------------------
    using AudioFrameCallback     = std::function<void(const char* source, const AudioFrameEvent&)>;
    using ButtonPressCallback    = std::function<void(const char* source, const ButtonPressEvent&)>;
    using EspNowInboundCallback  = std::function<void(const char* source, const EspNowInboundEvent&)>;
    using DmxInboundCallback     = std::function<void(const char* source, const DmxInboundEvent&)>;

    static bool subscribe_audio_frames    (const char* target, AudioFrameCallback   cb);
    static bool subscribe_button_presses  (const char* target, ButtonPressCallback  cb);
    static bool subscribe_esp_now_inbound (const char* target, EspNowInboundCallback cb);
    static bool subscribe_dmx_inbound     (const char* target, DmxInboundCallback   cb);

    // -------------------------------------------------------------------------
    // Input lifecycle. Some inputs (audio mic) need to be enabled/disabled by
    // orchestration; sample-rate / fft-size hint the underlying backend if it
    // supports configurable parameters. Returns false on unknown target,
    // unsupported capability for that target, or no driver available.
    // -------------------------------------------------------------------------
    static bool start_audio_input(const char* target,
                                  uint16_t sample_rate_hz = 16000,
                                  uint16_t fft_size       = 512);
    static bool stop_audio_input (const char* target);

    // -------------------------------------------------------------------------
    // Internal: drivers and tests use these to deliver input events to
    // subscribers. Application code does not call these directly.
    // -------------------------------------------------------------------------
    static void deliver_audio_frame      (const char* source, const AudioFrameEvent&);
    static void deliver_button_press     (const char* source, const ButtonPressEvent&);
    static void deliver_esp_now_inbound  (const char* source, const EspNowInboundEvent&);
    static void deliver_dmx_inbound      (const char* source, const DmxInboundEvent&);
};

// =============================================================================
// Built-in profile constants
// =============================================================================
//
// The PixMobX4Gen3_1 profile is a static constant; deployers reference it by
// pointer when registering active devices.
//
// The host profile (transport "local") is composed at runtime in DAL::begin()
// from hal::HAL::capabilities(). It is reachable via DAL::profile_of("local")
// after begin(); there is no header-level constant for it because its
// capability list depends on which HAL backend is linked.

namespace profiles {

extern const DeviceProfile PixMobX4Gen3_1;

}  // namespace profiles

}  // namespace dal
}  // namespace nocturnation
