---
title: NocturNation DAL design
status: cross-project (interface contract is shared; backend specifics are per-host)
notion_url: https://www.notion.so/35abd0677405814cb1a9f97caee4179e
notion_id: 35abd0677405814cb1a9f97caee4179e
last_synced: 2026-05-08
sync_direction: bidirectional
---

# NocturNation DAL design

This document defines the interface contract for the Device Abstraction Layer (DAL). The DAL sits above the HAL (`docs/hal-design.md`) and below the Orchestration layer; it is the single uniform, bidirectional interface for everything that produces events or accepts commands - regardless of underlying protocol or hardware.

The DAL lands in Epic 2 alongside the HAL. The first concrete drivers - the StickC Plus2's local input/output handlers and the PixMob IR driver - land in the same Epic. ESP-NOW driver stubs (interfaces only, no implementation) ship for Epic 4 to plug into.

---

## 1. Design goals

- **One interface, all devices.** Whether a target is a PixMob bracelet over IR, a NocturNation peer over ESP-NOW, a DMX fixture, or the host itself - it's addressed the same way and dispatches through the same code path.
- **DAL is the only thing above the HAL.** Orchestration calls only into the DAL; the DAL owns the HAL's lifecycle and is the only caller of `hal::HAL::*`. Application code (`main.cpp`'s `setup()`/`loop()`) talks to the DAL and does not include `hal/hal.h`. This keeps the layering honest and makes the HAL replaceable without touching anything above the DAL.
- **Typed events.** Each capability is a C++ struct with explicit fields. Dispatch is by event type; no stringly-typed key-value bags. Compile-time safety, IDE autocomplete, no fat-finger keys.
- **Capabilities, not promises.** A device profile declares what it supports. Calls to unsupported capabilities fail silently (return `false`) - substitution policy is the show file composer's responsibility, not the DAL's.
- **Static registry, dynamic-friendly later.** Epic 2's active-device registry is hardcoded at boot. The interface is shaped so future Epics can add dynamic registration (peer discovery, pairing) without contract changes.
- **Drivers wrap protocols, not hardware.** Drivers translate typed events into and out of wire-format bytes; they reach hardware via the HAL, not vendor SDKs. A driver for one protocol works on every host whose HAL exposes the prerequisite transport.

---

## 2. Device profiles

A profile describes a **device type** - what a class of device can do. Profiles are C++ structs declared at compile time. JSON-loaded profiles may be added later; for Epic 2 every profile is a `static constexpr` in code, since adding a new device type requires re-flashing firmware anyway.

```cpp
namespace nocturnation::dal {

// Semantic capability identifiers used in profiles. Distinct from
// hal::Capability (which is hardware-level).
enum class CapabilityId : uint16_t {
    // ----- Output capabilities -----
    RgbPulse,           // RGB with attack/sustain/release envelope (PixMob et al.)
    RgbStatic,          // Plain RGB, no envelope
    DisplayShowText,    // Render text on a host display
    DisplayClear,
    DisplayFillRect,
    DisplayMeter,       // Draw a horizontal meter (used by beat-mode UI)

    // ----- Input capabilities -----
    AudioFrame,         // Spectrum frames from the host's mic
    ButtonPress,        // Button events from the host's buttons
    EspNowInbound,      // Inbound ESP-NOW peer messages (Epic 4+)
    DmxInbound,         // Inbound DMX instructions (Epic 7+)

    // Sentinel
    _Count
};

struct DeviceProfile {
    const char*         type_id;        // e.g. "PixMobX4Gen3_1"
    const char*         version;        // profile schema version
    const char*         transport;      // "ir-pixmob", "esp-now-nocturnation", "local"
    const CapabilityId* output_capabilities;
    size_t              output_capability_count;
    const CapabilityId* input_capabilities;
    size_t              input_capability_count;
    bool                supports_groups;
    uint8_t             max_group_id;   // inclusive; 0 = broadcast/all
};

}
```

### Example: the PixMob X4 Gen3.1 profile

```cpp
namespace nocturnation::dal::profiles {

static constexpr CapabilityId pixmob_x4_outputs[] = {
    CapabilityId::RgbPulse,
};
// no input capabilities - PixMob is IR-receive-only

static constexpr DeviceProfile PixMobX4Gen3_1 = {
    .type_id                  = "PixMobX4Gen3_1",
    .version                  = "1.0",
    .transport                = "ir-pixmob",
    .output_capabilities      = pixmob_x4_outputs,
    .output_capability_count  = 1,
    .input_capabilities       = nullptr,
    .input_capability_count   = 0,
    .supports_groups          = true,
    .max_group_id             = 31,
};

}
```

### Example: the NocturNationStickCplus2 (host) profile

The host profile is **composed at boot** from the HAL's capability declarations plus protocol-layer additions. The composition rule: every HAL capability maps to one or more DAL capabilities; the DAL registers the host with whichever subset is currently lit up.

```cpp
// Composed at runtime in DAL::begin() based on hal::HAL::capabilities():
//
// HAL has Mic           -> DAL adds AudioFrame to inputs
// HAL has Buttons       -> DAL adds ButtonPress to inputs
// HAL has Display       -> DAL adds DisplayShowText, DisplayClear, etc. to outputs
// HAL has IRTx          -> DAL registers PixMobIRDriver (and possibly other IR drivers)
// HAL has ESPNow        -> DAL registers EspNowDriver (Epic 4+)

```

The host itself is registered as a single device, conventionally named `"local"` (configurable). Orchestration calls `fire_display_text("local", ...)` or subscribes to `AudioFrame` events from `"local"` the same way it would for any other device.

---

## 3. Active-device registry

The registry maps **logical names** to (profile, group ID) pairs. Names are arbitrary strings chosen by the deployer.

```cpp
struct ActiveDevice {
    const char*         name;       // "left-bracelet", "all-pixmobs", "local"
    const DeviceProfile* profile;
    uint8_t             group_id;   // 0 = broadcast/wildcard for that profile
};
```

A logical name can refer to:

- **A specific physical device** in a specific group: `"left-bracelet" -> (PixMobX4Gen3_1, group 5)`.
- **A whole group** of devices of one type: `"group-5-pixmobs" -> (PixMobX4Gen3_1, group 5)` (same as above; group addressing means the IR command targets all devices in that group).
- **All devices of one type**: `"all-pixmobs" -> (PixMobX4Gen3_1, group 0)`.
- **The host itself**: `"local" -> (NocturNationStickCplus2, 0)`.

Group IDs are stored as `uint8_t` internally (PixMob uses 0-31; spec §4.5 carves up the range). The string in the public API is the device's logical name, not the group number - orchestration calls `fire_rgb_pulse("group-5-pixmobs", ...)` and the DAL resolves the name to (profile, group_id) and dispatches via the appropriate driver.

For Epic 2, the registry is statically populated in `DAL::begin()`:

```cpp
DAL::register_device("local",          &profiles::NocturNationStickCplus2, 0);
DAL::register_device("all-pixmobs",    &profiles::PixMobX4Gen3_1,     0);  // group 0 = broadcast
DAL::register_device("group-5",        &profiles::PixMobX4Gen3_1,     5);
// More as the deployer wishes.
```

A future Epic can add `DAL::register_device(...)` calls at runtime (e.g. when a new ESP-NOW peer joins) without changing the interface.

---

## 4. Events: typed structs per capability

Each `CapabilityId` has a corresponding C++ event struct. Output capabilities define a struct that gets passed to `fire_*` helpers; input capabilities define a struct delivered to subscribers. Field types match the underlying protocol where reasonable (e.g. `RgbPulseEvent` reuses `pixmob::Time` and `pixmob::Chance` enums for envelope and chance values).

```cpp
namespace nocturnation::dal {

// =============================================================================
// Output events
// =============================================================================

struct RgbPulseEvent {
    uint8_t        r, g, b;
    pixmob::Time   attack;
    pixmob::Time   sustain;
    pixmob::Time   release;
    pixmob::Chance chance;          // default 100% on hosts that lack chance
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
    uint16_t color;     // 0 = black
};

struct DisplayFillRectEvent {
    int x, y, w, h;
    uint16_t color;
};

struct DisplayMeterEvent {
    int x, y, w, h;
    float ratio;            // 0.0 .. 1.0
    uint16_t bar_color, frame_color, threshold_color;
    float threshold_ratio;  // optional vertical-line marker; <0 to skip
};

// =============================================================================
// Input events
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

}
```

---

## 5. The bidirectional API

The DAL exposes one named helper per capability for outputs, plus typed subscription helpers for inputs. All helpers route through a common internal dispatcher; the public surface is what callers see.

```cpp
namespace nocturnation::dal {

class DAL {
public:
    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------
    static void begin();        // composes profiles, registers drivers, populates registry
    static void loop_tick();    // delivers input events; advances driver state

    // -------------------------------------------------------------------------
    // Registry / discovery
    // -------------------------------------------------------------------------
    static bool register_device(const char* name,
                                const DeviceProfile* profile,
                                uint8_t group_id);

    static bool                 has_device(const char* name);
    static const DeviceProfile* profile_of(const char* name);
    static size_t               active_device_count();
    static const char*          active_device_name(size_t index);

    static bool supports(const char* device_name, CapabilityId cap);

    // -------------------------------------------------------------------------
    // Output: per-capability helpers (return false on unsupported / unknown target)
    // -------------------------------------------------------------------------
    static bool fire_rgb_pulse       (const char* target, const RgbPulseEvent& ev);
    static bool fire_rgb_static      (const char* target, const RgbStaticEvent& ev);
    static bool fire_display_show_text(const char* target, const DisplayShowTextEvent& ev);
    static bool fire_display_clear   (const char* target, const DisplayClearEvent& ev);
    static bool fire_display_fill_rect(const char* target, const DisplayFillRectEvent& ev);
    static bool fire_display_meter   (const char* target, const DisplayMeterEvent& ev);

    // -------------------------------------------------------------------------
    // Input: typed subscription helpers
    // -------------------------------------------------------------------------
    using AudioFrameCallback   = std::function<void(const char* source, const AudioFrameEvent&)>;
    using ButtonPressCallback  = std::function<void(const char* source, const ButtonPressEvent&)>;
    using EspNowInboundCallback= std::function<void(const char* source, const EspNowInboundEvent&)>;
    using DmxInboundCallback   = std::function<void(const char* source, const DmxInboundEvent&)>;

    static bool subscribe_audio_frames  (const char* target, AudioFrameCallback   cb);
    static bool subscribe_button_presses(const char* target, ButtonPressCallback  cb);
    static bool subscribe_esp_now_inbound(const char* target, EspNowInboundCallback cb);
    static bool subscribe_dmx_inbound   (const char* target, DmxInboundCallback   cb);
};

}
```

Return-value semantics: `true` on success, `false` on (a) unknown target name, (b) target's profile doesn't declare the requested capability, or (c) the underlying driver isn't registered (e.g. ESP-NOW driver in Epic 2). Callers may check the return value where it matters; the prototype's existing call sites mostly trust the call.

The "named helper per capability" approach gets verbose as capabilities multiply; once we hit ~15 capabilities it's worth considering a templated `fire_event<EventType>(target, event)` overload set as a refactor. Not yet.

---

## 6. Drivers

A driver translates a transport name (`"ir-pixmob"`, `"esp-now-nocturnation"`, `"local"`) into actual wire-format bytes by consuming HAL primitives. Drivers live in `src/dal/drivers/` and are registered statically at boot.

```cpp
namespace nocturnation::dal {

class Driver {
public:
    virtual ~Driver() = default;
    virtual const char* transport_name() const = 0;        // "ir-pixmob"
    virtual bool        begin() = 0;
    virtual void        loop_tick() = 0;

    // Output dispatch - one method per CapabilityId the driver supports.
    // Default implementations return false (capability not supported).
    virtual bool send(uint8_t group_id, const RgbPulseEvent&)         { return false; }
    virtual bool send(uint8_t group_id, const RgbStaticEvent&)        { return false; }
    virtual bool send(uint8_t group_id, const DisplayShowTextEvent&)  { return false; }
    virtual bool send(uint8_t group_id, const DisplayClearEvent&)     { return false; }
    virtual bool send(uint8_t group_id, const DisplayFillRectEvent&)  { return false; }
    virtual bool send(uint8_t group_id, const DisplayMeterEvent&)     { return false; }
};

}
```

Each driver overrides only the capabilities it supports. The `PixMobIRDriver` overrides `send(group_id, RgbPulseEvent)` and translates to a `pixmob::buildSingleColor` + `hal::IRTx::send_raw` sequence. The "local" driver overrides the display-related sends and translates to `hal::Display::*` calls. The `EspNowDriver` (Epic 4+) overrides `send(group_id, ...)` for ESP-NOW-targeted profiles - or wraps an internal frame format and overrides specific capabilities once the frame protocol is settled.

Driver registration at boot:

```cpp
void DAL::begin() {
    if (hal::HAL::has(hal::Capability::IRTx)) {
        register_driver(&pixmob_ir_driver_instance);
    }
    if (hal::HAL::has(hal::Capability::Display)) {
        register_driver(&local_driver_instance);
    }
    // ESP-NOW driver guarded similarly when Epic 4 lands:
    // if (hal::HAL::has(hal::Capability::ESPNow)) { register_driver(...); }
}
```

If a HAL prerequisite is missing, the driver isn't registered, so devices whose transport requires it will fail-silent on every `fire_*` call. That's the right behaviour - the device just doesn't work, the firmware doesn't crash.

### Driver/profile/transport relationship

- A **profile** declares its `transport` string ("ir-pixmob", "local", etc.).
- The DAL's dispatch loop, given a target name, looks up the device's profile, finds its transport, then finds the registered driver for that transport, then calls `driver->send(group_id, event)`.
- If no driver is registered for the transport, the call returns `false`.

The transport string is the join key. Multiple profiles can share a transport (e.g. multiple PixMob hardware revisions all use "ir-pixmob"); each driver handles exactly one transport.

---

## 7. Capability composition

When `DAL::begin()` runs, it builds the host profile (`NocturNationStickCplus2`) by inspecting `hal::HAL`:

| HAL capability declared | DAL adds to host profile |
| --- | --- |
| `Mic`     | input: `AudioFrame`                                         |
| `Buttons` | input: `ButtonPress`                                        |
| `Display` | output: `DisplayShowText`, `DisplayClear`, `DisplayFillRect`, `DisplayMeter` |
| `IMU`     | (no DAL capability yet - reserved for a future motion-events capability)    |
| `Battery` | (no DAL capability yet - reserved for a future battery-status capability)   |
| `IRTx`    | (registers the PixMobIRDriver; not a host capability per se)                |
| `IRRx`    | (registers a future IR-listener driver; Epic 4+)                            |
| `ESPNow`  | input: `EspNowInbound`; registers the `EspNowDriver` (Epic 4+)              |

Host profile composition is one-shot at boot. Hot-pluggable HAL capabilities aren't planned.

---

## 8. Lifecycle

Application code calls only the DAL. The DAL drives the HAL underneath.

```cpp
// main.cpp - the only place the application talks to the framework.
// Note: only dal/dal.h is included here. hal/hal.h is not.

void setup() {
    DAL::begin();              // DAL internally calls HAL::begin(),
                               // then composes profiles, registers drivers,
                               // populates the active-device registry.
    show_setup();              // orchestration: subscribe to events, init state
}

void loop() {
    DAL::loop_tick();          // DAL internally calls HAL::loop_tick(),
                               // then drains input events to subscribers and
                               // advances driver state.
    show_loop();               // orchestration: react to delivered events,
                               // run the show state machine, fire commands.
}
```

Inside `DAL::begin()` and `DAL::loop_tick()` the DAL is the sole caller of the HAL. HAL-level callbacks (e.g. `hal::Mic::set_frame_callback`) are wired up internally by the DAL's host-side driver, which translates the HAL's `AudioFrame` struct into the DAL's `AudioFrameEvent` and dispatches to subscribers. Application code never touches HAL callbacks, HAL types, or HAL accessors directly.

```cpp
// dal/dal.cpp - implementation
void DAL::begin() {
    hal::HAL::begin();                 // initialise hardware first
    compose_host_profile();            // read hal::HAL::capabilities()
    register_drivers();                // gated on hal::HAL::has(...)
    populate_active_device_registry();
    wire_hal_callbacks_to_dal_dispatch();
}

void DAL::loop_tick() {
    hal::HAL::loop_tick();   // pulls fresh HAL events; fires HAL callbacks
                             // which our wired-up handlers translate into
                             // DAL events queued for subscriber delivery.
    deliver_pending_events();
    drivers_loop_tick();
}
```

---

## 9. StickC Plus2-specific: Epic 2 starting state

The StickC Plus2 HAL backend (`src/hal_stickcplus2/`) declares: `Mic`, `IRTx`, `Display`, `Buttons`, `IMU`, `Battery`. It does **not** declare `IRRx` or `ESPNow` (interfaces exist but no implementation yet).

Therefore at Epic 2 boot, the DAL on a StickC Plus2 will:

- Compose `NocturNationStickCplus2` profile with inputs `[AudioFrame, ButtonPress]` and outputs `[DisplayShowText, DisplayClear, DisplayFillRect, DisplayMeter]`.
- Register the `PixMobIRDriver` (because HAL has `IRTx`).
- Register the `LocalDriver` (because HAL has `Display`).
- **Not** register the `EspNowDriver` (because HAL doesn't declare `ESPNow`).
- Populate the active-device registry with `"local"` (the StickC Plus2 itself) and a small set of PixMob entries (see below).

Initial active-device set in code:

```cpp
DAL::register_device("local",       &profiles::NocturNationStickCplus2, 0);
DAL::register_device("all-pixmobs", &profiles::PixMobX4Gen3_1,     0);
DAL::register_device("group-1",     &profiles::PixMobX4Gen3_1,     1);
DAL::register_device("group-2",     &profiles::PixMobX4Gen3_1,     2);
DAL::register_device("group-3",     &profiles::PixMobX4Gen3_1,     3);
// ... extend per the deployer's setup
```

Orchestration uses these names verbatim:

```cpp
DAL::fire_rgb_pulse("all-pixmobs", {255, 0, 0,
    pixmob::T_32_MS, pixmob::T_96_MS, pixmob::T_96_MS, pixmob::CHANCE_100});

DAL::subscribe_audio_frames("local", [](const char*, const AudioFrameEvent& ev) {
    // beat detection logic here
});

DAL::subscribe_button_presses("local", [](const char*, const ButtonPressEvent& ev) {
    if (ev.id == hal::ButtonId::Btn1 && ev.kind == hal::ButtonEvent::Clicked) {
        // ... fire test pulse
    }
});
```

---

## 10. What's deliberately not in the DAL

- **Show files / animation timelines.** Orchestration's job. The DAL provides verbs (`fire_*`); the show file decides which to call when.
- **Beat detection.** Also orchestration. The DAL surfaces audio frames; the deciding-this-is-a-beat logic lives above.
- **Capability fallback / substitution.** The DAL fails silently on unsupported capabilities; the show file composer handles "if this doesn't work, do that instead". Adding a fallback registry to the DAL would muddle responsibilities.
- **Network frame format.** When ESP-NOW lands in Epic 4, the frame format is the driver's concern - it lives inside `EspNowDriver`, not at the DAL surface. Orchestration sees typed events; the wire format is invisible.
- **Device pairing / discovery.** Static registry for Epic 2. Dynamic registration is a future Epic; the `register_device` interface accommodates it.
- **Per-call telemetry / acknowledgements.** Some transports can ack (ESP-NOW, DMX-receive); some can't (PixMob IR). Per-call ack is deferred; a "how many devices have we heard from in the last N seconds?" counter is a candidate future capability (would live in the driver and surface as a query, not a per-call return).

---

## 11. Open questions

- **Helper-per-capability vs templated `fire_event<T>`.** Once we have ~15 capabilities the helper boilerplate gets noisy. Consider switching to a templated overload set then. Not yet.
- **Group-name vs group-number addressing in the public API.** Logical-name-only is cleaner; numeric group helpers (e.g. `fire_rgb_pulse_group(profile_type, group_n, ev)`) might be useful for the constellation art piece where groups are dynamic. Decide when the constellation piece starts.
- **Subscription scope.** `subscribe_audio_frames("local", cb)` is fine when there's one mic. If a future host has multiple mics, do we register them as separate device names? Probably yes; flag if it becomes awkward.
- **Multi-callback subscriptions.** Current sketch allows one callback per (target, event-type) pair. If multiple subsystems want to subscribe to `AudioFrame` events, do we allow a list of callbacks? Easy to add later; not in v1.
- **JSON-loaded profiles.** Deferred; revisit when there's a real "users want to add device types without recompiling" use case.
- **Telemetry channel.** Per Jason's note: "being able to understand how many devices are responding etc might be useful in future". Could live as a per-driver `responding_count(group_id)` accessor that drivers with ack capability implement; PixMob's IR driver returns -1 (unknown). Defer until first real use case.

---

## 12. Out-of-band notes for implementation (Epic 2)

When Feature 2.1 (DAL implementation) starts, after the HAL is in place:

1. Create `src/dal/dal.h` (public DAL interface) and `src/dal/dal.cpp` (registry, dispatch).
2. Create `src/dal/profiles/` with one file per device type (`pixmob_x4.cpp`, `nocturnation_stickcplus2.cpp`).
3. Create `src/dal/drivers/` with one file per driver (`pixmob_ir_driver.cpp`, `local_driver.cpp`, `esp_now_driver_stub.cpp`).
4. Add a native test in `test/test_dal_dispatch/` that links a fake HAL backend declaring known capabilities, fires events at known device names, and verifies dispatch / silent-fail behaviour. The PixMob parity tests already cover the byte-encoding side; this DAL test covers the routing side.
5. Refactor `src/main.cpp` incrementally to use DAL helpers where it currently uses `irsend.sendRaw` and `M5.Display.*`. Each migration is its own commit + Block 3-style hardware verification.
