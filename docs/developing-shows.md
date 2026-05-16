---
title: "Developing a Show"
status: Draft
notion_url: https://www.notion.so/362bd0677405812393d7c0f9eee52788
notion_id: 362bd0677405812393d7c0f9eee52788
last_synced: 2026-05-17
sync_direction: bidirectional
---

# Developing a Show

Reference for adding a new Show plug-in to the NocturNation firmware. A Show is the Director-side performance unit — it consumes events from the audio analyser and other sources (e.g. DMX) and decides what to send to bracelets, what to paint on the Director screen, and how to respond to operator input.

This guide covers the API surface, the conventions, and a worked example. Read it once before you write your first Show; reach for the section index when you need to look something up later.

## Table of contents

- [Concept](#concept)
- [File layout](#file-layout)
- [The `Show` base class](#the-show-base-class)
- [Registration](#registration)
- [Analyser hooks](#analyser-hooks)
- [Sending light commands](#sending-light-commands)
- [Drawing to the screen](#drawing-to-the-screen)
- [Button handling](#button-handling)
- [Composing widgets](#composing-widgets)
- [Persistence (properties + NVS)](#persistence-properties--nvs)
- [Testing](#testing)
- [Worked example: `DynamicShow`](#worked-example-dynamicshow)
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

## File layout

A Show needs four things on disk:

| File                                | What it holds                                |
|-------------------------------------|----------------------------------------------|
| `include/shows/<your_show>.h`       | Class declaration + singleton accessor       |
| `src/shows/<your_show>.cpp`         | Implementation + TU-static singleton         |
| `src/main.cpp`                      | One `show_registry().register_plugin(...)` call |
| `test/test_<your_show>/test_main.cpp` | Native unit tests (optional but expected) |

A test environment in `platformio.ini` is usually added too. The existing `[env:native_show]` covers both `test_show` (framework + SimpleBeatShow) and `test_dynamic_show`; new Shows can join that env's `test_filter` if they share its source-tree (`+<plugins/>`, `+<shows/>`, `+<widgets/>`, `+<effects/>`, `+<dal/>`, `+<hal/>`, `+<transport/>`, `+<modes/persistence.cpp>`).

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

### NVS namespace

Each property bag stores its values in the NVS namespace `ns_<your-id>` where `<your-id>` is exactly what `id()` returns. The namespace string is composed inside `src/plugins/property_bag.cpp`'s `compose_namespace()`; you don't need to touch it. Just keep your `id()` ≤ 12 ASCII chars.

The 15-char namespace cap is an ESP-IDF Preferences API limit, not a NocturNation choice. NVS keys (the property `key` field) are capped at 15 chars too; the host validates at runtime.

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

## Submitting your Show

1. Open a PR with `include/shows/<your_show>.h`, `src/shows/<your_show>.cpp`, `test/test_<your_show>/test_main.cpp`, the registration in `src/main.cpp`, and any `platformio.ini` additions.
2. Run `pio test -e native_show` (or your test env) and paste the pass output into the PR.
3. Run `pio run -e m5stack-stickcplus2 -e m5stack-stickcs3` and paste the build sizes (RAM / Flash).
4. If you tuned anything against music, list what you tried and the final values in the PR description so others can reproduce.

Reviewers will check that:
- `id()` is unique against existing Shows
- Capability requirements are honest (`Mic` if you read audio, etc.)
- Property schema entries are bounded for U8/U16/Enum types
- Tests cover at least routing + colour math + paused suppression

Welcome to the show stack.
