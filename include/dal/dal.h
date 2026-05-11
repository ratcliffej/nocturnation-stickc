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
#include "hal/input_action.h"
#include "pixmob_protocol.h"

namespace nocturnation {
namespace dal {

// =============================================================================
// RGB565 colour constants
// =============================================================================
//
// Provided here so orchestration code can pass colours into Display*Event
// structs without pulling in M5Unified / LovyanGFX. Values match the standard
// LovyanGFX names that the prototype used, so no on-screen colour drift.

constexpr uint16_t BLACK   = 0x0000;
constexpr uint16_t BLUE    = 0x001F;
constexpr uint16_t GREEN   = 0x07E0;
constexpr uint16_t RED     = 0xF800;
constexpr uint16_t YELLOW  = 0xFFE0;
constexpr uint16_t WHITE   = 0xFFFF;

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
    BatteryLevel,         // Synchronous query: host returns 0..100 or -1
    AssignDeviceGroup,    // Bracelet-setup command: write a new group ID into the target's EEPROM

    // ----- Input -----
    AudioFrame,           // 3-band B/M/T + 8-band perceptual band summaries
    SpectrumFrame,        // 32-band log-spaced spectrum (master-local; not on the wire)
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

struct AssignDeviceGroupEvent {
    uint8_t new_group_id;      // 1..31; 0 is reserved for broadcast
};

// =============================================================================
// Input event types (delivered to subscribers)
// =============================================================================

struct AudioFrameEvent {
    uint32_t timestamp_ms = 0;

    // 3-band B/M/T summary. Restandardised in Epic 4.5 to evidence-
    // based ranges (Bass <250 Hz, Mid 250-2000 Hz, Treble 2-Nyquist Hz).
    float    bass_energy   = 0.0f;
    float    mid_energy    = 0.0f;
    float    treble_energy = 0.0f;

    // 8-band perceptual summary, Audible Genius reference. New in Epic
    // 4.5. Internally consistent with the 3-band roll-up:
    //   bass_energy   = mud + sub_bass + bass
    //   mid_energy    = low_mids + midrange
    //   treble_energy = high_mids + presence + air
    float    mud           = 0.0f;
    float    sub_bass      = 0.0f;
    float    bass          = 0.0f;
    float    low_mids      = 0.0f;
    float    midrange      = 0.0f;
    float    high_mids     = 0.0f;
    float    presence      = 0.0f;
    float    air           = 0.0f;

    float    overall_rms   = 0.0f;

    // True if the analyser's BeatDetector fired a beat candidate on
    // this frame (Epic 4.5 Block 3). Orchestration consumers gate
    // their per-beat actions on this flag rather than running their
    // own flux-threshold logic - the detector is self-calibrating
    // and produces equivalent behaviour across hosts with different
    // mic SNR.
    bool     is_beat       = false;

    // Macro-level music event fired by the analyser's DropDetector
    // on this frame, or 0 (none). Wire-stable values match
    // transport::espnow::MusicEventType (1=DROP, 2=BREAKDOWN, 3=BUILD
    // reserved). Master orchestration broadcasts MUSIC_EVENT frames
    // when this is non-zero. Epic 4.5 Block 4.
    uint8_t  music_event   = 0;

    // Per-onset-band strengths (Epic 4.7 Block 3). Non-zero values
    // signal "an onset fired on this frame in this band"; zero means
    // no onset. The kick path coexists with is_beat for back-compat:
    // is_beat is the legacy boolean from Epic 4.5; beat_strength is
    // the Block 3 quantised intensity that AutonomousMasterMode
    // passes to Show::on_beat_detected. snare / hihat are new in
    // Block 3 - their detectors watch ~200-2000 Hz and ~4-8 kHz
    // sub-bands respectively.
    uint8_t  beat_strength  = 0;
    uint8_t  snare_strength = 0;
    uint8_t  hihat_strength = 0;

    // Continuous music descriptors (Epic 4.7 Block 3). Updated every
    // frame at FFT rate (~30-40 Hz). AutonomousMasterMode applies a
    // 5 %-change rate limit before delivering to Show::on_music_descriptor
    // so Shows don't have to filter every-frame churn.
    //
    //   centroid: tonal centre of the spectrum, 0 = bass, 255 = bright.
    //   energy:   smoothed RMS envelope, 0 = silence, 255 = loud.
    //   density:  events-per-second across all onset bands, 0 = sparse,
    //             255 = >= 16 events / s.
    uint8_t  centroid  = 0;
    uint8_t  energy    = 0;
    uint8_t  density   = 0;
};

// 32-band log-spaced spectrum frame, master-local. Delivered alongside
// AudioFrameEvent via a separate subscription channel so consumers
// that only want band summaries (most effects) don't pay the cost of
// receiving 128 bytes of magnitudes per FFT cycle. Consumers that
// want the rich surface (Diagnostic UI in Epic 4.6, FX modulators in
// Epic 4.7) subscribe specifically.
//
// NOT broadcast over ESP-NOW - too heavy at FFT rate. Slaves consume
// the more compact MUSIC_DESCRIPTOR wire descriptor when Epic 4.7
// ships.
struct SpectrumFrameEvent {
    uint32_t timestamp_ms = 0;
    static constexpr size_t kBands = 32;
    float    magnitudes[kBands] = {};
};

struct ButtonPressEvent {
    hal::ButtonId    id;
    hal::ButtonEvent kind;
};

// Semantic input event - the host's input mapper translates raw
// ButtonPressEvents into these so visualisations and overlay UIs run
// unchanged across hosts with different button layouts. Aliased from
// the HAL definition so app code can stay on dal::InputEvent without
// pulling hal/input_action.h directly.
using InputEvent = hal::InputEvent;

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

    // Generic enable flag. When false, DAL::dispatch_output silently skips
    // every send for this driver - useful for "mute the IR" / "mute the
    // ESP-NOW" toggles in Config without interleaving check logic into
    // every send override. Default is enabled.
    bool enabled() const            { return enabled_; }
    void set_enabled(bool e)        { enabled_ = e; }

    // Lifetime counter of successful sends through this driver. Bumped by
    // DAL::dispatch_output after a send() override returns true; used by
    // orchestration (AutonomousMaster status display) to surface activity.
    uint32_t send_count() const     { return send_count_; }
    void increment_send_count()     { ++send_count_; }

    // Output dispatch: one overload per output capability. Default = unsupported.
    virtual bool send(uint8_t /*group_id*/, const RgbPulseEvent&)         { return false; }
    virtual bool send(uint8_t /*group_id*/, const RgbStaticEvent&)        { return false; }
    virtual bool send(uint8_t /*group_id*/, const DisplayShowTextEvent&)  { return false; }
    virtual bool send(uint8_t /*group_id*/, const DisplayClearEvent&)     { return false; }
    virtual bool send(uint8_t /*group_id*/, const DisplayFillRectEvent&)  { return false; }
    virtual bool send(uint8_t /*group_id*/, const DisplayMeterEvent&)     { return false; }
    virtual bool send(uint8_t /*group_id*/, const AssignDeviceGroupEvent&){ return false; }

    // Class+group dispatch (Epic 4.65 Block 4). Default forwards to the
    // 2-arg send dropping the class - test drivers and protocols that
    // don't carry a class field still see the pulse, just without class
    // discrimination. EspNowBroadcastDriver overrides to write both
    // bytes into the LIGHT_COMMAND payload.
    virtual bool send(uint8_t /*target_class*/, uint8_t target_group,
                       const RgbPulseEvent& ev) {
        return send(target_group, ev);
    }

    // Input lifecycle hooks. Called by DAL::start_audio_input / stop_audio_input
    // on the driver registered for the target's transport. Drivers that do
    // not source audio frames default to no-op (returns false).
    virtual bool start_audio_input(uint16_t /*sample_rate_hz*/,
                                   uint16_t /*fft_size*/) { return false; }
    virtual bool stop_audio_input() { return false; }

    // Synchronous queries. Default returns the "not available" sentinel so
    // drivers without the capability get the right behaviour for free.
    virtual int  battery_level() { return -1; }

private:
    bool     enabled_     = true;
    uint32_t send_count_  = 0;
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

    // Toggle a driver's enable flag by transport name. Returns false if no
    // driver is registered for that transport. Generic across transports
    // ("ir-pixmob", "local", and future "esp-now-nocturnation" / "dmx" /
    // etc.); ConfigMode uses these to surface "Enable / Disable" leaves
    // per spec §8.4.
    static bool     set_driver_enabled (const char* transport_name, bool enabled);
    static bool     driver_enabled     (const char* transport_name);
    static uint32_t driver_send_count  (const char* transport_name);

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
    static bool fire_assign_device_group(const char* target, const AssignDeviceGroupEvent&);

    // -------------------------------------------------------------------------
    // render_fx: canonical "render this effect on this device" entry point.
    //
    // Conceptually identical to the per-event fire_* helpers above for the
    // current set of effect types - both end up dispatching the same typed
    // event to the registered driver for the target's transport. The
    // difference is intent and future extensibility: new effect types
    // (planned: text overlay, simple graphics, scripted animations) will
    // ship as render_fx overloads only, without per-capability fire_*
    // proliferation. Existing fire_* helpers are kept for the call sites
    // that already use them; new code should prefer render_fx.
    //
    // Per the slave-as-target-device model, calling
    //   DAL::render_fx("local", RgbPulseEvent{...})
    // on a StickC paints the screen with an attack/sustain/release fade;
    // on a future LED-only device it drives the LED; on a Tildagon it can
    // drive both screen and on-board LEDs. The host profile composition
    // and the per-device driver decide which physical surface is "the
    // light".
    // -------------------------------------------------------------------------
    static bool render_fx(const char* target, const RgbPulseEvent& ev);

    // -------------------------------------------------------------------------
    // Input subscription. Returns false on unknown target or capability not
    // declared as an input on that target's profile. Callbacks fire from
    // DAL::loop_tick().
    // -------------------------------------------------------------------------
    using AudioFrameCallback     = std::function<void(const char* source, const AudioFrameEvent&)>;
    using SpectrumFrameCallback  = std::function<void(const char* source, const SpectrumFrameEvent&)>;
    using ButtonPressCallback    = std::function<void(const char* source, const ButtonPressEvent&)>;
    using InputActionCallback    = std::function<void(const char* source, const hal::InputEvent&)>;
    using EspNowInboundCallback  = std::function<void(const char* source, const EspNowInboundEvent&)>;
    using DmxInboundCallback     = std::function<void(const char* source, const DmxInboundEvent&)>;

    static bool subscribe_audio_frames     (const char* target, AudioFrameCallback    cb);
    static bool subscribe_spectrum_frames  (const char* target, SpectrumFrameCallback cb);
    static bool subscribe_button_presses   (const char* target, ButtonPressCallback   cb);
    static bool subscribe_input_actions    (const char* target, InputActionCallback   cb);
    static bool subscribe_esp_now_inbound  (const char* target, EspNowInboundCallback cb);
    static bool subscribe_dmx_inbound      (const char* target, DmxInboundCallback    cb);

    // Drop all spectrum-frame subscriptions for `target` (Epic 4.6 Block 11).
    // Added so AutonomousMasterMode can flip Block 7's pipeline gate live
    // when switching between vis with and without
    // PowerProfile::needs_spectrum_frame. Returns the count removed.
    // Subscribe surfaces for other event channels remain one-shot (no
    // unsubscribe) until a similar gate-flip need surfaces - we don't
    // generalise speculatively.
    static size_t unsubscribe_spectrum_frames(const char* target);

    // -------------------------------------------------------------------------
    // Subscriber-count queries (Epic 4.6 Block 7 - pipeline gating).
    //
    // Drivers fan-out spectrum frames per FFT cycle; the LocalDriver guards
    // the SpectrumFrameEvent assembly + dispatch on has_spectrum_frame_subscribers()
    // so the per-frame 32-float copy and delivery loop are skipped when
    // nothing is listening. The underlying FFT roll-up that produces
    // frame.spectrum still runs unconditionally because BeatDetector
    // consumes it in-pipeline as a behaviour-preserved Epic 4.5 surface.
    // -------------------------------------------------------------------------
    static size_t spectrum_frame_subscriber_count();
    static bool   has_spectrum_frame_subscribers();

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
    // Audio analyser configuration (Epic 4.5 Block 2). Forwards to the
    // host's HAL Mic. configure_audio_pipeline() asks the analyser to
    // run at one of the host-declared operating points; in Epic 4.5
    // only the canonical (16000, 512) is implemented across all hosts
    // and any other tuple returns false. set_band_layout() selects a
    // band-summary preset; only "hifi+production" is implemented in
    // Epic 4.5 (3-band B/M/T + 8-band perceptual concurrent).
    // -------------------------------------------------------------------------
    static bool configure_audio_pipeline(const char* target,
                                         uint32_t sample_rate_hz,
                                         uint16_t fft_size);
    static bool set_band_layout         (const char* target,
                                         const char* preset_name);

    // -------------------------------------------------------------------------
    // Synchronous queries. Returns -1 on unknown target, capability not
    // declared on that target's profile, or no driver available.
    // -------------------------------------------------------------------------
    static int  battery_level(const char* target);

    // -------------------------------------------------------------------------
    // Internal: drivers and tests use these to deliver input events to
    // subscribers. Application code does not call these directly.
    // -------------------------------------------------------------------------
    static void deliver_audio_frame      (const char* source, const AudioFrameEvent&);
    static void deliver_spectrum_frame   (const char* source, const SpectrumFrameEvent&);
    static void deliver_button_press     (const char* source, const ButtonPressEvent&);
    static void deliver_input_action     (const char* source, const hal::InputEvent&);
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
extern const DeviceProfile EspNowBroadcast;

}  // namespace profiles

}  // namespace dal
}  // namespace nocturnation
