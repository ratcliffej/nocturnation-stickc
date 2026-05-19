---
title: "Developing a Show"
status: Draft
notion_url: https://www.notion.so/362bd0677405812393d7c0f9eee52788
notion_id: 362bd0677405812393d7c0f9eee52788
last_synced: 2026-05-17
sync_direction: bidirectional
---

# Developing a Show

Reference for adding a new Show plug-in to the NocturNation firmware. A Show is the Director-side performance unit — it consumes events from input sources (the audio analyser on the M5, IMU tap-to-beat on the Tildagon, plus future sources like DMX) and decides what to send to bracelets, what to paint on the Director screen, and how to respond to operator input.

The Show framework runs on **two hosts** that share the same hook names and capability vocabulary:

- **M5 Stick (C++).** The original Director. A microphone + on-board analyser drive the beat / snare / hi-hat / descriptor / section hooks. Shows live in `include/shows/` + `src/shows/`.
- **Tildagon badge (MicroPython).** Added in Epic 6B. No microphone — the IMU drives tap-to-beat (and motion) hooks instead. Shows live in `apps/nocturnation/shows/<id>/`.

A Show written against the shared surface moves between hosts with the same mental model; the [hosts-and-capabilities matrix](#hosts-and-capabilities) below says which hooks actually fire on which host. The code examples are C++ (the reference implementation) with a MicroPython equivalent called out where the surface differs.

This guide covers the API surface, the conventions, and a worked example. Read it once before you write your first Show; reach for the section index when you need to look something up later.

## Table of contents

- [Concept](#concept)
- [Hosts and capabilities](#hosts-and-capabilities)
- [File layout](#file-layout)
- [The `Show` base class](#the-show-base-class)
- [Registration](#registration)
- [Analyser hooks](#analyser-hooks)
- [IMU hooks (Tildagon tap-to-beat + motion)](#imu-hooks-tildagon-tap-to-beat--motion)
- [Sending light commands](#sending-light-commands)
- [Drawing to the screen](#drawing-to-the-screen)
- [Button handling](#button-handling)
- [Composing widgets](#composing-widgets)
- [Persistence (properties + NVS)](#persistence-properties--nvs)
- [Testing](#testing)
- [Worked example: `DynamicShow`](#worked-example-dynamicshow)
- [Porting a Show across hosts](#porting-a-show-across-hosts)
- [Submitting your Show](#submitting-your-show)

## Concept

A **Show** is a class derived from `nocturnation::shows::Show` that runs on the Director device and produces a performance. The Director-mode host (`DirectorMode`) holds exactly one active Show at a time; the operator picks which one via the Director-mode picker overlay or via `ConfigMode > Show`. The active Show:

- subscribes to analyser events (beat / snare / hi-hat / music descriptor / section change) by overriding hooks on the base class;
- decides what to send on the wire by calling `DAL::render_fx("<class>:<group>", ev)` with the Epic 4.65 structured-target format;
- owns the Director's screen — the host's `loop_tick` clears + calls `on_render()` at ~20 Hz when no overlay is open;
- handles input that reaches it (the host intercepts Picker / Settings / Pause; the Show sees Cycle / Confirm / CyclePrev).

Where the Show sits in the bigger picture:

```
Event sources (parallel; DirectorMode fans them to Show)
─ HAL: microphone → DAL audio analyser → AudioFrameEvent
                    (FFT, BeatDetector, SnareDetector,
                     HihatDetector, MusicDescriptors,
                     SectionDetector)
─ HAL: buttons    → InputActionMapper  → InputAction
─ DMX input       → DMX decoder        → (Epic 7, planned)
─ Network         → listener + decoder → (future, e.g. OSC / MQTT cues)
       v
DirectorMode (the host)
       v
Show (your code) ---> DAL::render_fx ---> EspNowBroadcastDriver
                                          + Director's PixMobIrBinding
                                          + local screen
                  ---> DAL::fire_display_* (Director screen only)
                  ---> BeatBarWidget / SpectrumBarsWidget (composed
                                            inside `on_render`)
```

Lumes receive `LIGHT_COMMAND` over ESP-NOW and run their own `OutputBinding` fan-out — they don't run Shows. The Show framework is Director-only.

On the **Tildagon** the picture is the same shape with the host-specific bits swapped:

```
Event sources (DirectorController fans them to the active Show)
─ IMU: accelerometer → ImuAdapter → tap / motion events
─ buttons          → DirectorButtonMapper → InputAction
─ DMX input        → (Epic 7, planned)
       v
DirectorController (the host)  +  DirectorHost (services surface)
       v
Show (your code) ---> ctx.render_fx ---> ESP-NOW broadcast
                                         + Director's perimeter LEDs
                                         + Director's LCD
                  ---> ctx.display() ---> on_render draws the LCD
```

## Hosts and capabilities

The framework defines one set of hooks; a host fires the subset its hardware supports. A Show declares what it needs via `required_capabilities()` and reads what the host actually has at runtime via `ctx.analyser_caps()` / `ctx.imu_caps()`. Hooks for sources a host lacks simply never fire — a Show that only overrides those is inert on that host but still loads.

| Hook | M5 Stick | Tildagon | Notes |
|------|:---:|:---:|---|
| `enter` / `exit` | ✓ | ✓ | Lifecycle, both hosts. |
| `on_audio_frame` / `on_spectrum_frame` | ✓ | — | Mic-only; Tildagon has no microphone. |
| `on_beat_detected` | ✓ (mic) | ✓ (tap alias) | On the Tildagon a tap fires `on_beat_detected` **and** `on_tap_detected`, so a beat-driven Show is host-agnostic. |
| `on_snare_detected` / `on_hihat_detected` | ✓ | — | Mic analyser only. |
| `on_music_descriptor` / `on_section_change` | ✓ | — | Mic analyser only. |
| `on_tap_detected` | — | ✓ | IMU tap (or the button-tap fallback). Forward-declared on M5 (no-op until an M5 host grows an IMU backend). |
| `on_motion_event` | — | ✓ | IMU motion (waving). Forward-declared on M5. |
| `on_input_action` | ✓ | ✓ | Cycle / Confirm / CyclePrev reach the Show on both. |
| `on_render` | ✓ | ✓ | Both; the drawing API differs (see [Drawing](#drawing-to-the-screen)). |
| `on_property_changed` / `tick` | ✓ | ✓ | Both. |

**Capability vocabulary.** `hal::Capability` (C++) / `nocturnation.hal.Capability` (Python) carry the same numeric values on both hosts, so a `required_capabilities()` mask means the same thing everywhere:

| Capability | Meaning | Declared by |
|---|---|---|
| `Mic` | microphone + analyser | M5 |
| `Display` | a screen the Show can draw to | M5, Tildagon |
| `ESPNow` | ESP-NOW broadcast | M5, Tildagon |
| `Buttons` | discrete buttons | M5, Tildagon |
| `ImuTap` | IMU produces tap events | Tildagon (M5 reserved) |
| `ImuMotion` | IMU produces motion events | Tildagon (M5 reserved) |
| `AnalyserBeatDetection`, … | analyser sub-features | M5 |

The IMU sub-capabilities (`ImuTap` / `ImuMotion`) are declared in both enums for cross-platform parity; on M5 they're reserved-but-unwired (no M5 backend fires them yet), exactly like the analyser sub-flags that were reserved before their producers landed.

## File layout

A Show needs four things on disk:

| File                                | What it holds                                |
|-------------------------------------|----------------------------------------------|
| `include/shows/<your_show>.h`       | Class declaration + singleton accessor       |
| `src/shows/<your_show>.cpp`         | Implementation + TU-static singleton         |
| `src/main.cpp`                      | One `show_registry().register_plugin(...)` call |
| `test/test_<your_show>/test_main.cpp` | Native unit tests (optional but expected) |

A test environment in `platformio.ini` is usually added too. The existing `[env:native_show]` covers both `test_show` (framework + SimpleBeatShow) and `test_dynamic_show`; new Shows can join that env's `test_filter` if they share its source-tree (`+<plugins/>`, `+<shows/>`, `+<widgets/>`, `+<effects/>`, `+<dal/>`, `+<hal/>`, `+<transport/>`, `+<modes/persistence.cpp>`).

### Tildagon (MicroPython)

A Tildagon Show is a **folder** under `apps/nocturnation/shows/`, auto-discovered at boot — no registration call to edit:

```
apps/nocturnation/shows/
  simple_tap/
    __init__.py        # defines a Show subclass + make_show()
    README.md          # optional per-Show docs
  motion_wave/
    __init__.py
    palettes.json      # optional per-Show data
```

`discover_shows()` (in `nocturnation.shows.registry`) walks the directory alphabetically, imports each subpackage, and calls its module-level `make_show()` to get an instance. Drop a folder in, expose `make_show()`, and it appears in the picker on the next boot. Host-side tests live in `tests/test_<show>.py` and run under `pytest` with the badge hardware faked (inject the renderer / display / accelerometer read).

## The `Show` base class

Declared in [include/shows/show.h](../include/shows/show.h). Every hook has a no-op default — override only the ones you care about.

```cpp
namespace nocturnation { namespace shows {

class Show : public plugins::Plugin {
public:
    plugins::PluginKind kind() const override {
        return plugins::PluginKind::Show;
    }

    // Identity (Plugin contract)
    virtual const char* id()           const = 0;
    virtual const char* display_name() const = 0;

    // Lifecycle
    virtual void enter(ShowContext&) {}
    virtual void exit (ShowContext&) {}

    // Raw analyser frames
    virtual void on_audio_frame   (ShowContext&,
                                    const dal::AudioFrameEvent&)    {}
    virtual void on_spectrum_frame(ShowContext&,
                                    const dal::SpectrumFrameEvent&) {}

    // Analyser events
    virtual void on_beat_detected   (ShowContext&, uint8_t strength) {}
    virtual void on_snare_detected  (ShowContext&, uint8_t strength) {}
    virtual void on_hihat_detected  (ShowContext&, uint8_t strength) {}
    virtual void on_music_descriptor(ShowContext&,
                                       uint8_t centroid,
                                       uint8_t energy,
                                       uint8_t density)              {}
    virtual void on_section_change  (ShowContext&, uint8_t section)  {}

    // Input
    virtual void on_input_action(ShowContext&,
                                  const hal::InputEvent&) {}

    // Render
    virtual void on_render(ShowContext&) {}

    // Property-change notification + tick
    virtual void on_property_changed(ShowContext&,
                                      const char* key) {}
    virtual void tick(ShowContext&, uint32_t now_ms) {}

    // Per-Show singleton context (see Registration below)
    virtual ShowContext& context() = 0;
};

}}
```

The two pure-virtuals you **must** implement are `id()`, `display_name()`, and `context()`. The host's picker calls `display_name()` to populate its list. `id()` is your Show's stable key for NVS persistence; cap it at 12 ASCII chars (the NVS namespace prefix `ns_` + your id must fit in 15 chars).

### `ShowContext`

The context is your Show's services surface. Declared in [include/shows/show_context.h](../include/shows/show_context.h):

```cpp
class ShowContext {
public:
    // Output - forwards to DAL::render_fx.
    bool render_fx(const char* target, const dal::RgbPulseEvent& ev);

    // Property bag
    plugins::PropertyValue get_property(const char* key) const;
    bool                   set_property(const char* key,
                                         plugins::PropertyValue value);

    // What analyser features the host actually has
    hal::CapabilityMask analyser_caps() const;

    // Framework-managed pause flag
    bool paused() const;
    void set_paused(bool p);

    // Time helpers (millis() on Arduino, test seam on native)
    uint32_t now_ms()         const;
    uint32_t since_enter_ms() const;
};
```

Everything your Show does goes through this surface. Don't reach for DAL or HAL directly — the indirection lets future hosts (Tildagon, custom hardware) swap in without touching your Show code.

**MicroPython equivalent.** The Tildagon `ShowContext` (in `nocturnation.shows.show_context`) is the same surface, idiomatic Python: `ctx.render_fx(target, ev)`, `ctx.get_property(key)` / `ctx.set_property(key, value)` (native values, not a tagged `PropertyValue`), `ctx.analyser_caps()` / `ctx.imu_caps()`, `ctx.paused()` / `ctx.set_paused(p)`, `ctx.now_ms()` / `ctx.since_enter_ms()`, plus `ctx.display()` for drawing (see [Drawing](#drawing-to-the-screen)). The `DirectorController` owns the active Show's context and passes it as the first argument to every hook.

## Registration

Each concrete Show owns three TU-static singletons in its `.cpp`:

```cpp
namespace {
MyShow      s_instance;
PropertyBag s_bag(s_instance);
ShowContext s_ctx(s_instance, s_bag);
}

MyShow*      my_show_instance()     { return &s_instance; }
PropertyBag& my_show_property_bag() { return s_bag; }
ShowContext& my_show_context()      { return s_ctx; }

ShowContext& MyShow::context() { return s_ctx; }
```

Then in `src/main.cpp`'s `setup()`:

```cpp
nocturnation::shows::show_registry().register_plugin(
    nocturnation::shows::my_show_instance());
```

That's it. The host walks the registry on Director-mode entry, resolves `persistence::load_active_show_id()` against `find(id)`, falls back to `"simple-beat"` if the saved id no longer registers, and `enter()`s the chosen Show.

Registry order matters in one place only: the picker UI lists Shows in registration order, which determines the default cursor on a fresh boot before NVS has anything saved. Register the most-useful default first (currently `SimpleBeatShow`).

**MicroPython equivalent.** No registration call and no singletons. The Show's `__init__.py` exposes a `make_show()` factory; `discover_shows()` walks `apps/nocturnation/shows/` alphabetically at boot and registers what it finds. The `DirectorController` builds the `PropertyBag` + `ShowContext` for the active Show and binds it (`show.bind_context(ctx)`), so `context()` works without per-Show boilerplate. Restore-the-last-Show resolves `Settings.active_show` against the registry, falling back to the first registered Show:

```python
from nocturnation.shows import Show

class MyShow(Show):
    def id(self):           return "my_show"      # <= 12 chars
    def display_name(self): return "My Show"
    # ... hooks ...

def make_show():
    return MyShow()
```

## Analyser hooks

The DAL runs one analyser pass per microphone frame and stamps the results onto a single `AudioFrameEvent`. The host then fans the event out to the active Show's hooks. **The DAL is the producer; Shows are pure consumers** — your Show never runs an FFT or a detector itself, it just reads the fields it cares about from the event and composes the response.

| Hook                       | Fires when                            | Rate         | Notes |
|----------------------------|---------------------------------------|--------------|-------|
| `on_audio_frame`           | Every FFT cycle                       | ~30-40 Hz    | Carries the full analyser snapshot in one struct — see [`AudioFrameEvent` fields](#audioframeevent-fields) below. |
| `on_spectrum_frame`        | Every FFT cycle                       | ~30-40 Hz    | 32-band log-spaced spectrum. Only delivered if your `power().needs_spectrum_frame = true`. |
| `on_beat_detected(strength)`  | Kick onset (BeatDetector, ~30-150 Hz watch) | beat-driven | `strength` 1-255; saturates at 255 for hits >= 3× the adaptive threshold. |
| `on_snare_detected(strength)` | Snare onset (~200-2000 Hz)         | beat-driven | Same shape as kick. |
| `on_hihat_detected(strength)` | Hi-hat onset (~4-8 kHz)            | beat-driven | Same shape, shorter refractory so 16th-note hi-hats fire cleanly. |
| `on_music_descriptor(c, e, d)` | Centroid / energy / density change | ≤ FFT rate, rate-limited | Host fires only on >= 5 % change in any component, so you don't churn at 30 Hz. |
| `on_section_change(section)`  | Section state transitions          | event-driven | `section` is a `SectionType` value (see below). |

### `AudioFrameEvent` fields

The single event the DAL hands you on `on_audio_frame`. Read the fields you need; ignore the rest. All values are Director-internal — none of them go on the wire unless your Show composes them into a `render_fx()` call.

| Field                              | Type      | Meaning |
|------------------------------------|-----------|---------|
| `bass_energy`, `mid_energy`, `treble_energy` | `float` | Classic 3-band roll-up; sums of FFT bin magnitudes. |
| `sub_bass`, `bass`, `low_mids`, `midrange`, `high_mids`, `presence`, `air`, `mud` | `float` | 8-band perceptual roll-up (Audible Genius split-points). |
| `overall_rms`                      | `float`   | Frame RMS; usable as a volume gate. |
| `is_beat`                          | `bool`    | True on this frame if `BeatDetector` fired. Mirrors what `on_beat_detected` delivers; useful when you want a single hook. |
| `beat_strength`, `snare_strength`, `hihat_strength` | `uint8_t` | Per-onset strength (0 when the corresponding detector didn't fire this frame). |
| `centroid`, `energy`, `density`    | `uint8_t` | `MusicDescriptors` outputs — same values `on_music_descriptor` delivers. |
| `section`                          | `uint8_t` | Latest `SectionType` from `SectionDetector` (see table below). |
| `music_event`                      | `uint8_t` | Latest `DropDetector` output: `0` = none, `1` = DROP, `2` = BREAKDOWN, `3` = BUILD (reserved). Use to gate one-shot peak-moment responses. **Director-internal only** — the spec v0.29 protocol trim removed the wire `MUSIC_EVENT (0x06)` frame, but the field stays here as part of the Show toolset; if you want a DROP to fire white across the room, your Show composes that as a `render_fx("00:00", whiteout)` call. |
| `timestamp_ms`                     | `uint32_t` | Frame timestamp (Director clock, monotonic since boot). |

### Analyser timing characteristics

- **Beat / snare / hi-hat refractory**: kick 200 ms, snare 150 ms, hi-hat 80 ms. A second hit inside the refractory window is dropped.
- **Descriptor smoothing windows**:
  - centroid: per-frame, no smoothing
  - energy: single-pole IIR, ~0.5 s time constant, log-normalised
  - density: 1 s sliding window; 16 events/s saturates 255
- **Section detection**: 8-frame transition hysteresis (~200 ms); BREAKDOWN needs 80 frames of sustained low energy + low density (~2 s); DROP latches for 40 frames (~1 s) after the underlying `DropDetector` fires.

### `SectionType` values

Defined in [include/dal/analyser/section_detector.h](../include/dal/analyser/section_detector.h). Reach the enum via `dal::analyser::SectionType` or use the raw u8 your hook receives.

| Value | Name                | When |
|-------|---------------------|------|
| 0     | `Unknown`           | Warm-up + gaps between rules |
| 1     | `Verse`             | Mid energy, mid centroid, low-to-mid density |
| 2     | `Chorus`            | Sustained high energy + density |
| 3     | `BuildUp`           | Both energy and density rising over the window |
| 4     | `Breakdown`         | Low energy + low density sustained ~2 s |
| 5     | `VocalsOnly`        | *Reserved* — needs per-band data; not fired yet |
| 6     | `InstrumentalBreak` | *Reserved* — same |
| 7     | `Drop`              | Latched ~1 s after a DROP event |

## IMU hooks (Tildagon tap-to-beat + motion)

The Tildagon has no microphone; its primary input is the on-board IMU. The `ImuAdapter` (in `nocturnation.director.imu`) polls the accelerometer ~50 Hz, removes gravity with a slow EMA high-pass, and turns the residual into two event streams:

| Hook | Fires when | Notes |
|------|------------|-------|
| `on_tap_detected(ctx, strength)` | A sharp tap on the badge | `strength` 0-255 from the over-threshold magnitude (floored to ≥1). A `TAP_REFRACTORY_MS` (120 ms) window debounces double-hits. **Also fires `on_beat_detected(ctx, strength)`** so a Show written for the M5 mic-beat works unchanged. |
| `on_motion_event(ctx, axis, magnitude)` | Sustained movement (waving) | `axis` 0=X / 1=Y / 2=Z (dominant axis), `magnitude` 0-255. Rate-limited to 100 ms and suppressed during the tap refractory so one tap doesn't also read as motion. |

**Sensitivity.** Tap threshold and motion floor scale with a per-Show `sensitivity` enum property (Low / Medium / High, default Medium). The `DirectorController` reads it on activation and pushes it to the adapter, so cycling Shows retunes the IMU automatically. Declare it like any other property:

```python
PropertyDef(
    key="sensitivity",
    type=PropertyType.ENUM,
    default_value=1,                 # Medium
    min_value=0, max_value=2,
    display_name="Sensitivity",
    enum_names=("Low", "Medium", "High"),
)
```

**Button-tap fallback.** When the IMU isn't tuned (or for deterministic beats while developing), the `ButtonTapSource` turns presses of button **C** into the same `on_tap_detected` events. A Show needs no special handling — taps arrive through the one hook regardless of source.

These hooks are forward-declared on the M5 `Show` base (no-op defaults) so a cross-platform Show can override them without `#ifdef`s; they simply never fire on a host with no IMU backend.

## Sending light commands

The wire output API is one function:

```cpp
bool DAL::render_fx(const char* target, const dal::RgbPulseEvent& ev);
```

The `target` is the Epic 4.65 structured `"<hex_class>:<hex_group>"` string. Two-byte fields, hex, separated by a single `:`:

| Target         | Meaning                                          |
|----------------|--------------------------------------------------|
| `"00:00"`      | Broadcast — every class, every group             |
| `"01:00"`      | All Light-class devices, every group             |
| `"01:01"`      | Light class, group 1                             |
| `"02:05"`      | Screen class, group 5                            |
| `"ff:ff"`      | Reserved sentinel                                |

`class` and `group` are u8 each. Class 0 means "any class"; group 0 means "any group". The available classes are defined in [include/hal/device_class.h](../include/hal/device_class.h):

| Class | Name              | Notes |
|-------|-------------------|-------|
| 0x00  | `All`             | Wildcard — every device responds |
| 0x01  | `Light`           | PixMob bracelets, LED strips |
| 0x02  | `Screen`          | Devices with an addressable screen |
| 0x03  | `MultiLedScreen`  | Bridge between the two |
| 0x04+ | Reserved          | Use sparingly; coordinate with the spec |

`RgbPulseEvent` fields:

```cpp
struct RgbPulseEvent {
    uint8_t        r, g, b;       // 0-255 each
    pixmob::Time   attack;        // T_0_MS / T_32_MS / T_96_MS / T_192_MS
                                  //  / T_480_MS / T_960_MS / T_2400_MS
                                  //  / T_3840_MS
    pixmob::Time   sustain;       // same set
    pixmob::Time   release;       // same set
    pixmob::Chance chance;        // CHANCE_100 / 88 / 67 / 50 / 32 / 16
                                  //  / 10 / 4
};
```

The PixMob protocol's 3-bit envelope timings are non-linear; pick attack/sustain/release from the enum, not arbitrary milliseconds. Use `effects::envelope_for_bpm(bpm)` if you've tracked BPM internally and want a sensible auto-envelope:

```cpp
const effects::PulseEnvelope env = effects::envelope_for_bpm(bpm);
RgbPulseEvent wire{};
wire.r = r; wire.g = g; wire.b = b;
wire.attack  = env.attack;
wire.sustain = env.sustain;
wire.release = env.release;
wire.chance  = pixmob::CHANCE_100;
DAL::render_fx("01:01", wire);
```

`chance` is the per-bracelet response probability. Use it as a texture knob: `CHANCE_100` means every bracelet pulses on every fire (intense, uniform); lower chances produce a sparkling / random coverage. Density-driven shows typically scale chance from sparse (CHANCE_10) when music is quiet to dense (CHANCE_100) at a busy chorus.

### What dispatch does for you (Epic 4.7 onward)

One `render_fx` call fans out through `dispatch_output_class_group` in [src/dal/dal.cpp](../src/dal/dal.cpp) to three sinks. You do not need to hand-roll a separate transmission to the Director's own IR LED or screen; the Director is treated as one of its own Lumes for output purposes.

1. **ESP-NOW broadcast**. Always fires, regardless of target_class. Every Lume on the channel sees the frame and applies its own class+group routing.
2. **Director IR loopback**. Fires when `target_class` is `0x00` (All) or `0x01` (Light). Drives the Director's own PixMob infra-red transmitter so bracelets near the operator's Stick also light up.
3. **Director screen loopback**. Fires when `target_class` is `0x00` (All) or `0x02` (Screen). Drives the Director's LCD pulse animation so the operator sees the fire on-screen.

**Bracelet residue: pick cadence > envelope**. Bracelets carry brief residual envelope state between fires. If a new command lands while the previous fade is still rendering, the bracelet stitches the two envelopes together (colour artefacts in fades, truncated twinkle tails). The dispatch does *not* try to scrub residue with an extra reset frame - an Epic 4.7 experiment that did so overloaded the IR receivers and was rolled back in Epic 4.8. Instead, the show is responsible for choosing an envelope duration that fits inside its fire cadence. SparkleVis (T_0 + T_480 + T_480 = 960 ms envelope on an 1100 ms cadence) is the canonical example.

**The single canonical call**. Pre-Epic-4.7 shows had to fire to `"all-pixmobs"` for the Lumes *and* `"local"` for the Director's own LCD - three or more separate calls per beat. From Epic 4.7 onwards the single `render_fx("00:00", ev)` call covers everything. The Director is no longer special.

**MicroPython equivalent.** Identical model — one `ctx.render_fx(target, ev)` call broadcasts over ESP-NOW *and* loops back to the Director's own perimeter LEDs (when `target_class` is All/Light/MultiLedScreen) and LCD (All/Screen/MultiLedScreen). The `ev` is an `RgbPulse` from `nocturnation.render`; the timing fields are the same `Time` enum and `chance` the same `Chance` enum (in `nocturnation.protocol.constants`):

```python
from nocturnation.render import RgbPulse
from nocturnation.protocol.constants import Time, Chance

ctx.render_fx("01:01", RgbPulse(
    r, g, b,
    attack=Time.T_0_MS, sustain=Time.T_96_MS,
    release=Time.T_480_MS, chance=Chance.CHANCE_100,
))
```

The Tildagon Director transmits on the **hobby channel (1) only** — Epic 5.5 reserves the channel-11 Performance band for M5 Directors, so a Tildagon Show can't address a curated channel-11 audience.

## Drawing to the screen

Your Show owns the Director's LCD canvas during normal operation. The host calls `on_render(ctx)` at ~20 Hz when no overlay is open. Your override should:

1. Clear the screen with a background colour
2. Paint whatever you want to display
3. Return

No `Canvas` object — use the DAL display primitives directly:

```cpp
void MyShow::on_render(ShowContext& ctx) {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        /*x=*/10, /*y=*/5,
        /*text=*/"My Show", /*fg=*/WHITE, /*bg=*/BLACK,
        /*size=*/2});

    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        /*x=*/10, /*y=*/30, /*w=*/100, /*h=*/14, GREEN});
}
```

Colour constants (`BLACK`, `WHITE`, `RED`, `GREEN`, `BLUE`, `YELLOW`) are RGB565 and live in [include/dal/dal.h](../include/dal/dal.h). For arbitrary colours, pack to RGB565 manually or use the HSV-to-RGB helper baked into `DynamicShow::compute_colour` (no shared helper yet — feel free to extract one if you need it).

The screen is 240 × 135 px. Reserve ~14 px at the bottom for a footer hint; size-1 text is ~8 px tall, size-2 is ~16 px, size-3 is ~24 px. The host doesn't redraw between your `on_render()` calls, so any pulse-flash effect you do via `fire_display_clear` will be overdrawn at the next ~20 Hz tick.

**MicroPython equivalent.** The Tildagon draws through `ctx.display()`, which returns a `CtxDisplay` wrapping the badge's draw context (the app rebinds the live `ctx` each frame). The screen is **240 × 240 round, origin-centred** (coordinates -120..120), and colours are **0-255 ints** (matching `RgbPulse`), not RGB565. Three primitives:

```python
def on_render(self, ctx):
    d = ctx.display()
    if d is None:        # no display wired (e.g. host tests)
        return
    d.clear(0, 0, 0)                                   # full-screen fill
    d.text(0, -60, "My Show", size=22, r=255, g=255, b=255)  # centred on (x, y)
    d.fill_rect(-50, 20, 100, 14, 0, 255, 0)           # origin-centred rect
```

Because the screen is round, keep text and key elements near the centre; the corners of a full-screen `fill_rect` fall outside the visible circle. `simple_tap` is the reference (`apps/nocturnation/shows/simple_tap/__init__.py`).

## Button handling

The host runs an input-action mapper (`InputActionMapper2Btn` for StickC; future hosts will provide their own). The semantic events that reach your Show through `on_input_action(ctx, ev)` are:

| `InputAction` | Default mapping (StickC) | Reaches Show? |
|---------------|--------------------------|---------------|
| `Picker`      | Btn1 long press           | No — host intercepts |
| `Settings`    | Btn2 long press? (varies) | No — host intercepts |
| `Pause`       | Btn1 + Btn2 simultaneous  | No — host toggles `ctx.paused()` |
| `Confirm`     | Btn1 short press          | Yes |
| `Cycle`       | Btn2 short press          | Yes |
| `CyclePrev`   | n/a yet                   | Yes when bound |

The **back gesture is reserved** — operators expect a long-press exit and it's wired centrally. Use `Cycle` / `Confirm` / `CyclePrev` for your Show's controls.

**MicroPython mapping (Tildagon).** Six buttons, no long-press. The `DirectorButtonMapper` produces the same `InputAction` values (`nocturnation.shows.InputAction`):

| Button | InputAction | Reaches Show? |
|--------|-------------|---------------|
| A (UP)    | `PICKER`   | No — host opens the picker |
| D (DOWN)  | `SETTINGS` | No — host opens per-Show settings |
| B (RIGHT) | `CYCLE`    | Yes |
| E (LEFT)  | `CYCLE_PREV` | Yes |
| C (CONFIRM) | — | Drives the button-tap fallback (a manual tap), not routed as an InputAction |
| F (CANCEL)  | — | Host exits Director mode |

So on the Tildagon a Show sees `CYCLE` / `CYCLE_PREV` (and `CONFIRM` only on hosts that route it). Compare against the shared enum:

```python
from nocturnation.shows import InputAction

def on_input_action(self, ctx, action):
    if action == InputAction.CYCLE:
        self._advance_palette(ctx, +1)
    elif action == InputAction.CYCLE_PREV:
        self._advance_palette(ctx, -1)
```

`SimpleBeatShow` uses `Cycle` to advance through colour presets:

```cpp
void SimpleBeatShow::on_input_action(ShowContext& ctx,
                                      const hal::InputEvent& ev) {
    if (ev.action != hal::InputAction::Cycle) return;
    const uint8_t cur = ctx.get_property("color").as_enum();
    const uint8_t next = (cur + 1) % 6;
    ctx.set_property("color", PropertyValue::from_enum(next));
    sync_pulse_colour(ctx);
}
```

If you handle a button, return after processing — the host doesn't fall through to other handling for actions that reach your Show.

## Composing widgets

`include/widgets/` ships two reusable level helpers you can compose inside `on_render()`:

### `BeatBarWidget`

A horizontal level bar with an optional threshold marker. Used by `SimpleBeatShow` for its flux meter:

```cpp
#include "widgets/beat_bar.h"
...
widgets::BeatBarWidget flux_bar_;   // class member

void MyShow::on_render(ShowContext& ctx) {
    // ... other drawing ...
    flux_bar_.update(/*bar_fraction=*/0.7f, /*marker_fraction=*/0.5f);
    flux_bar_.draw(/*x=*/10, /*y=*/110, /*w=*/220, /*h=*/14);
}
```

`bar_fraction` and `marker_fraction` are both 0..1, clamped. Marker at 0 suppresses the marker entirely.

### `SpectrumBarsWidget`

7-band perceptual spectrum visualisation (Sub Bass / Bass / Lows / Mid / Hi / Pres / Air). Used by `DynamicShow`:

```cpp
#include "widgets/spectrum_bars.h"
...
widgets::SpectrumBarsWidget spectrum_;

void MyShow::on_audio_frame(ShowContext&,
                              const AudioFrameEvent& ev) {
    // Roll AudioFrameEvent's 8-band perceptual values into 7 widget
    // bands (drop Mud below 20 Hz; the widget starts at Sub Bass).
    // Log-normalise the per-band sums to 0..1 - see DynamicShow's
    // on_audio_frame for the reference implementation.
    band_values_[0] = /*normalise(ev.sub_bass)*/;
    ...
    band_values_[6] = /*normalise(ev.air)*/;
}

void MyShow::on_render(ShowContext&) {
    // ... other drawing ...
    spectrum_.update(band_values_);
    spectrum_.draw(/*x=*/0, /*y=*/60, /*w=*/240, /*h=*/60);
}
```

`SpectrumBarsWidget` also exposes a static helper `roll_up_spectrum_to_perceptual(magnitudes_32, sensitivity, out_7band)` if you've subscribed to spectrum frames and want to roll up raw FFT data instead.

Widgets are a library, not a plug-in surface — they live alongside your Show as composable helpers, not in their own registry. Future work (post-Epic-4.7) may promote them if third-party widget contributions emerge.

## Persistence (properties + NVS)

Your Show can declare a property schema for operator-mutable settings. The host's Settings overlay auto-generates a UI from the schema and persists changes to NVS under the namespace `ns_<your-id>`.

Declare a static array of `PropertyDef` and return it from `properties()`:

```cpp
namespace {
const char* const kPaletteNames[] = {
    "Cool", "Natural", "Warm", "Rainbow"
};

const PropertyDef kProps[] = {
    PropertyDef{
        /*key=*/"palette",
        /*type=*/PropertyType::Enum,
        /*default_value=*/PropertyValue::from_enum(1),  // Natural
        /*min_value=*/    PropertyValue::from_enum(0),
        /*max_value=*/    PropertyValue::from_enum(3),
        /*display_name=*/"Palette",
        /*unit=*/nullptr,
        /*enum_names=*/kPaletteNames,
    },
};
}  // namespace

Span<const PropertyDef> MyShow::properties() const {
    return Span<const PropertyDef>{kProps,
                                    sizeof(kProps) / sizeof(kProps[0])};
}
```

Supported property types: `Bool`, `U8`, `U16`, `Colour` (0x00RRGGBB packed), `Enum`. The schema defines min / max bounds for bounded types; the Settings overlay enforces them.

Read / write at runtime via the context:

```cpp
const uint8_t palette = ctx.get_property("palette").as_enum();
ctx.set_property("palette", PropertyValue::from_enum(2));
```

If you cache derived state (e.g. a precomputed colour) override `on_property_changed(ctx, key)` so the cache re-syncs after the Settings overlay writes a new value.

**MicroPython equivalent.** Same `PropertyDef` shape (`nocturnation.plugins`), but values are native Python types — no `PropertyValue` wrapper. Return a tuple from `properties()`; read/write with plain values:

```python
from nocturnation.plugins import PropertyDef, PropertyType

_PROPS = (
    PropertyDef(
        key="palette",
        type=PropertyType.ENUM,
        default_value=1,                 # Natural
        min_value=0, max_value=3,
        display_name="Palette",
        enum_names=("Cool", "Natural", "Warm", "Rainbow"),
    ),
)

def properties(self):
    return _PROPS

# runtime:
palette = ctx.get_property("palette")        # -> int
ctx.set_property("palette", 2)               # clamps, persists, notifies
```

`ctx.set_property` clamps to the schema bounds, persists, and calls your `on_property_changed`. The Director's per-Show settings overlay auto-generates from `properties()` and cycles Bool / Enum / U8 / U16 values.

### NVS namespace

Each property bag stores its values in the NVS namespace `ns_<your-id>` where `<your-id>` is exactly what `id()` returns. The namespace string is composed inside `src/plugins/property_bag.cpp`'s `compose_namespace()`; you don't need to touch it. Just keep your `id()` ≤ 12 ASCII chars.

The 15-char namespace cap is an ESP-IDF Preferences API limit, not a NocturNation choice. NVS keys (the property `key` field) are capped at 15 chars too; the host validates at runtime.

On the **Tildagon** there's no NVS — the `PropertyBag` persists to a single JSON file (`/nocturnation_plugins.json`) with one section per Show id, kept outside `/apps/` so a re-deploy doesn't clobber operator-tuned values. The same `id()` ≤ 12 chars / key ≤ 15 chars conventions apply for cross-platform parity.

### Migration

If you ever change a Show's `id()` (which you generally shouldn't), the old NVS namespace becomes orphaned. Add a migration to `migrate_legacy_nvs_keys()` in `src/modes/persistence.cpp` to read the old keys and write them under the new namespace, then `prefs.remove` the orphans. The function runs once at boot and is idempotent.

The Block 1 `active_vis → active_show` migration is the worked example; mirror its shape.

## Testing

Native unit tests run via `pio test -e native_show`. The pattern mirrors `test/test_dynamic_show/test_main.cpp`:

1. Provide a miniature HAL backend (Mic + Display caps; usually no IRTx / ESPNow so the firmware drivers refuse registration and your recording drivers can claim the transports).
2. Register a `RecordingDriver` for `"esp-now-broadcast"` (and `"ir-pixmob"` if you fire effects through it).
3. Call `DAL::begin()` then register your recording driver.
4. Construct your Show through its singleton accessor, call `enter(ctx)`, drive synthetic events at the hooks, assert what landed in the recording drivers' captured event buffers.

Key tests to write:

- **Routing**: each onset hook fires `render_fx` with the expected target string (verify via `g_espnow_driver.last_group()` since the 3-arg Driver base default forwards to 2-arg with the group field).
- **Colour math**: descriptor combinations produce the right RGB ordering (low centroid → blue dominates; high → red).
- **Section overrides**: e.g. DROP forces white regardless of centroid.
- **Paused state**: `ctx.set_paused(true)` suppresses fires while internal tracking (BPM, etc.) keeps updating.

The `native_show` env compiles `+<plugins/>`, `+<shows/>`, `+<widgets/>`, `+<effects/>`, `+<dal/>`, `+<hal/>`, `+<transport/>`, `+<modes/persistence.cpp>`. If your Show needs anything beyond that set, add it to the `build_src_filter` or create a new env.

**MicroPython equivalent.** Tests run on the host under `pytest` (no badge); the hardware is faked by injection — the same pattern the renderers use. Build a `ShowContext` with a fake host that records `render_fx` calls and a fake display that records draw calls, drive the hooks, and assert. `tests/test_reference_shows.py` is the worked example:

```python
class _FakeHost:                 # records render_fx, supplies now_ms / caps
    def __init__(self): self.renders = []
    def dispatch_render_fx(self, target, ev): self.renders.append((target, ev)); return True
    def now_ms(self): return 0
    def analyser_caps(self): return CapabilityMask()
    def imu_caps(self): return CapabilityMask()

def test_tap_fires_render_fx(tmp_path):
    show = make_show()
    bag = PropertyBag(show, path=str(tmp_path / "p.json"))
    host = _FakeHost()
    ctx = ShowContext(show, bag, host=host)
    show.bind_context(ctx)
    show.on_tap_detected(ctx, 200)
    target, ev = host.renders[0]
    assert target == "01:01"
```

Key tests to write mirror the C++ list: routing (right target per hook), colour math, paused suppression, and — for IMU Shows — that `on_tap_detected` / `on_motion_event` produce the expected fires.

### Hardware verification

Native tests cover the math + plumbing; only hardware tests cover "does the show feel good". The checklist:

- Director + at least one Lume on the same ESP-NOW channel
- Lume configured into one of your Show's groups (via `Config > Group`)
- Bracelet paired to the Lume's PixMobIrBinding group
- Live music or a known reference playlist
- Confirm: kick fires, snare fires, hi-hat fires, palette tracks song mood, sections transition cleanly

Document any per-genre tuning you settle on in your Show's header.

## Worked example: `DynamicShow`

The Block 5 [DynamicShow](../include/shows/dynamic_show.h) is the reference implementation. It demonstrates every Block 1-4 surface:

### What it consumes

- `on_audio_frame` — caches the 8-band perceptual values for its on-screen spectrum widget.
- `on_beat_detected(strength)` — tracks BPM via an IBI buffer, then fires effects to group 1, plus a Director-screen flash.
- `on_snare_detected(strength)` — fires to group 2.
- `on_hihat_detected(strength)` — fires to group 3.
- `on_music_descriptor(c, e, d)` — caches all three values for the next colour computation.
- `on_section_change(section)` — caches the new section for palette selection.

### What it produces

- `render_fx("01:01", ev)` for kicks; `"01:02"` for snares; `"01:03"` for hi-hats. Colours, envelopes, and chance come from the cached descriptor + section state.
- `fire_display_clear("local", ...)` on each kick for a Director-screen pulse.
- Screen layout: title + section label (size 3) + descriptor readout
  + `SpectrumBarsWidget` filling the bottom region.

### Colour math

`DynamicShow::compute_colour()` ([src/shows/dynamic_show.cpp:74](../src/shows/dynamic_show.cpp)) is the reference HSV mapping. The pipeline:

1. `hue = 240 - (centroid * 240 / 255)`  →  0 (red) at high centroid, 240 (blue) at low.
2. Section-specific adjustments: VERSE pushes hue cooler; BUILDUP pushes hue warmer; BREAKDOWN dims `value` and washes `saturation`; DROP overrides to white.
3. Apply a 0.10 brightness floor so silence still produces visible pulses (operators want to *see* events firing during sound check).
4. Pass through standard HSV → RGB conversion.

### Density to chance

`density_to_chance()` ([src/shows/dynamic_show.cpp:131](../src/shows/dynamic_show.cpp)) maps density 0..255 into PixMob's 8-step chance ladder. Higher density → higher chance → more bracelets fire on each event. The mapping isn't linear; the steep end (~density 60+) jumps to 50 % so moderately-busy music already lights most bracelets, with the very top of the range reserved for true chorus moments.

### Things you'd customise

If you wanted a different show character without changing the analyser surface, you'd likely:

- Adjust the hue base (e.g. start at 120° green instead of 240° blue)
- Pick different per-section hue offsets
- Add more groups (group 4 reserved for percussion fills, etc.) — but bracelets need to be configured for those groups too
- Add a property for an operator-tunable palette override

## Porting a Show across hosts

The two hosts share hook names, the capability vocabulary, the `PropertyDef` schema shape, and the `render_fx("<class>:<group>", ev)` output model. Most of a Show's logic — colour maths, palette cycling, group routing, property handling — is identical in spirit. What differs is mechanical:

| Concern | M5 (C++) | Tildagon (MicroPython) |
|---|---|---|
| Input that drives fires | `on_beat_detected` (mic) | `on_tap_detected` (IMU) — but it **also** fires `on_beat_detected`, so a beat show needs no change |
| Output event | `RgbPulseEvent` (struct, RGB565 colour constants for screen) | `RgbPulse` (0-255 ints) |
| Screen drawing | `DAL::fire_display_*`, 240×135 | `ctx.display().clear/text/fill_rect`, 240×240 round, origin-centred |
| Registration | `show_registry().register_plugin(...)` in `main.cpp` | folder + `make_show()`, auto-discovered |
| Property values | `PropertyValue` tagged union | native Python ints/bools |
| Persistence | NVS namespace `ns_<id>` | JSON file, section per id |
| Tests | `pio test`, recording drivers | `pytest`, fake host + fake display |

**Practical advice.** Write the colour / routing logic against the shared hooks. If you want one Show to run on both hosts, key its fire on `on_beat_detected` (fired by both the mic and a tap) and keep the colour maths host-neutral; the only genuinely host-specific code is `on_render` (different drawing primitives) and the file/registration boilerplate. Portable Show definitions (a shared format with per-host adapters) are a deferred future Epic — for now each host keeps its own `shows/` tree with the same API.

## Submitting your Show

1. Open a PR with `include/shows/<your_show>.h`, `src/shows/<your_show>.cpp`, `test/test_<your_show>/test_main.cpp`, the registration in `src/main.cpp`, and any `platformio.ini` additions.
2. Run `pio test -e native_show` (or your test env) and paste the pass output into the PR.
3. Run `pio run -e m5stack-stickcplus2 -e m5stack-stickcs3` and paste the build sizes (RAM / Flash).
4. If you tuned anything against music, list what you tried and the final values in the PR description so others can reproduce.

**For a Tildagon Show** (in the `nocturnation-tildagon` repo): open a PR with `apps/nocturnation/shows/<your_show>/__init__.py` (+ any per-Show data) and `tests/test_<your_show>.py`. Run `pytest` and paste the pass output. No registration to edit — discovery is automatic. If you tuned against the IMU (sensitivity, tap feel), note it.

Reviewers will check that:
- `id()` is unique against existing Shows
- Capability requirements are honest (`Mic` if you read audio, `ImuTap` if you rely on the IMU, etc.)
- Property schema entries are bounded for U8/U16/Enum types
- Tests cover at least routing + colour math + paused suppression (and IMU-hook fires for a Tildagon Show)

Welcome to the show stack.
