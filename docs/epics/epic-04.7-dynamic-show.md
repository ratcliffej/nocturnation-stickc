---
title: "Epic 4.7: Show plug-in framework and dynamic FFT-driven show"
status: Done
notion_url: https://www.notion.so/35cbd067740581feb287ff7023202c19
notion_id: 35cbd067740581feb287ff7023202c19
notion_status: Done
last_synced: 2026-05-11
sync_direction: bidirectional
---

## Related Documents

- [NocturNation Architecture Specification (v0.22)](https://www.notion.so/357bd0677405800b891beab0f4e0a976) — particularly §5 (Audio analysis pipeline), §7 (Plugin layers).
- [Epic 4.5: Sub-band adaptive-threshold beat detection (Done)](https://www.notion.so/35bbd0677405816fa9fbd0306100c794) — upstream Epic; 4.7 builds on the per-band magnitudes it exposes.
- [Epic 4.6: M5 firmware UI cleanup (Done)](https://www.notion.so/35cbd067740581e4ba55f79eb168ec9d) — established the Visualisation plug-in surface and ConfigMode shape that 4.7 builds on.
- [Epic 4.65: Class+group device addressing (Done)](https://www.notion.so/35dbd06774058075aa66da569ce2aff1) — provides the structured `class:group` routing that the dynamic Show uses to send different effects to different device groups.
- [Epic 5: Tildagon receiver app](https://www.notion.so/358bd067740581b19551d158d658df76) — downstream Epic; 4.7 ships before 5 so EMF gets a properly dynamic show.

## Goal

Two deliverables in one Epic:

**A. Refactor master mode around a Show plug-in framework.** Today AutonomousMasterMode bakes one performance into the firmware (beat → broadcast colour to all groups). A Show is the right unit to make swappable: a developer should be able to drop in a C++ Show class with a defined entry point and analyser-event hook surface, register it, and have it appear in the master-mode show picker. Shows own the screen, button handling, and what gets sent on the wire — they decide whether to draw a beat bar, a spectrum analyser, a custom visual, diagnostic text, or nothing at all. Existing functionality is preserved by reimplementing today's behaviour as `SimpleBeatShow`.

**B. Build a richer Show driven by the FFT and class+group routing.** Use the per-band FFT magnitudes Epic 4.5 already exposes to compute multi-band onset events (kick / snare / hi-hat), spectral centroid, energy envelope, and onset density. Add section detection (verse / chorus / build / breakdown) as a slower-window state machine. A new Show (`DynamicShow`) consumes all of the above and uses the post-Epic-4.65 class+group routing to send different effects to different device groups — kick to group 1, bass to group 2, cymbals to group 3, palette shifts on section change — so the lights track the song's whole shape, not just the kick.

This Epic ships **before Epic 5 (Tildagon)** so EMF 2026 gets a properly dynamic show rather than just beat-flash, and so the Tildagon receiver app lands on a stable richer-protocol baseline.

## Why this Epic exists

Two motivating observations:

1. **The current beat detection uses ~2 % of the FFT's output.** The firmware computes 256 magnitude bins per FFT cycle but only fires events from six of them (bass-band sum). The other 98 % encodes useful information about *what the song is doing right now*, and an audience can feel the difference between lights that flash on kicks versus lights that respond to the whole musical shape. Epic 4.5 closed the cross-device-consistency gap; 4.7 uses the per-band magnitudes 4.5 exposes properly.

2. **There is no Show layer.** Today the active "visualisation" is conflated with the performance generator — BeatPulse-the-class consumes beat events, makes render_fx() calls, *and* draws the screen, with no clean seam. Adding a richer performance means either inflating BeatPulse or duplicating large parts of it. A Show plug-in framework gives developers a clean place to drop in new performances; the existing screen widgets (BeatPulse, SpectrumBars) become a reusable level-tuning widget library that any Show — or `ConfigMode > Utilities` — can compose.

## Terminology (post-4.7)

The conversation that produced this Epic surfaced terminology that the codebase doesn't yet match. The refactor brings the code into line:

| Layer | Who runs it | What it does |
|---|---|---|
| **Show** (new plug-in surface) | Master only | Owns the performance: subscribes to analyser events, makes `render_fx()` calls with class+group targets, owns the screen, owns button handling (except the back gesture). One Show active at a time. |
| **Widget library** (formerly Visualisations) | Master, composed by Shows or by Utilities | Reusable level-tuning UI: beat-bar, spectrum-bars. Renderable inside a Show *or* standalone via `ConfigMode > Utilities > Level Tuning`. Library, not plug-in surface, for now. |
| **Slave mode** | Slave | Unchanged. Inbound render_fx → LocalDisplayBinding (screen) + PixMobIrBinding (IR relay). No shows, no widgets. |

The existing `active_vis` NVS key retires (consumed by `migrate_legacy_nvs_keys`). A new `active_show` NVS key replaces it, persisting the operator's last Show choice. Each Show is free to decide what to draw on the screen — there is no longer a separate "visualisation" concept at the framework level.

## Master-mode show pathways

The new framework supports three pathways for how a master generates a performance:

1. **Simple broadcast** — beat detection drives a single colour event to all devices / all groups. Today's behaviour. Implemented as `SimpleBeatShow` in Block 1.
2. **Class+group routed** — different musical content drives different targets: kick to group 1, bass to group 2, cymbals to group 3; palette changes per section. Implemented as `DynamicShow` in Block 5. Multiple variants of this Show could co-exist over time as developers tune for different music styles (rock, dance, ambient).
3. **External (DMX / QLC+)** — laptop drives the show via QLC+ over DMX/Art-Net. **Out of scope for Epic 4.7** (belongs to Epic 7).

## Operational model

Laptop-driven, same as Epics 1-4.65. Algorithm work is well-suited to native unit testing on captured audio samples; show-tuning work is genuinely subjective and requires hardware verification with real music in a real listening environment.

Verification ownership: **(L)** = laptop / native test, **(B)** = build-time check, **(H)** = hardware verification by Jason listening to music with hardware in hand. Block 6 is entirely (H); the only real test of "does this show feel good?" is playing music through it and watching.

## Algorithmic primitives

Four primitives live in the audio analyser layer (`src/audio_analyser/`), computed once per FFT frame on the master, exposed as events on the Show plug-in API. They are **not** wire payloads — analyser output stays master-local; the wire still only carries `LIGHT_COMMAND` (per Epic 4.65 settle).

### Multi-band onset detection

Extension of Epic 4.5's sub-band adaptive-threshold work. Three independent event streams from three frequency regions:

- **Kick band** (~60-200 Hz, sub-bands 0-4) — already detected in Epic 4.5; produces existing `on_beat_detected()`.
- **Snare band** (~200-2000 Hz, sub-bands 5-15) — new; produces `on_snare_detected()`. In typical pop/rock, snare hits beats 2 and 4. Detecting kick + snare separately tracks the *groove* not just the *pulse*.
- **Hi-hat band** (~5000-8000 Hz, sub-bands 22-30) — new; produces `on_hihat_detected()`. Hi-hat patterns are usually denser (16th notes) and faster than the underlying beat. Maps well to high-frequency sparkle effects.

Each band uses the same adaptive-threshold algorithm Epic 4.5 establishes; the only differences are which bins contribute, the threshold multiplier (typically lower for hi-hat where transients are smaller), and the refractory period (shorter for hi-hat).

### Spectral centroid

The "centre of gravity" of the spectrum — a single scalar per FFT frame indicating where the energy is concentrated.

```
centroid = sum(bin_index * magnitude[bin_index]) / sum(magnitude[bin_index])
```

Low centroid (~5-15 / 256) → bass-heavy / muddy. High (~40-80 / 256) → bright. Mid (~15-40) → balanced.

Exposed as part of `on_music_descriptor(centroid, energy, density)`. Shows decide what to do with it — `DynamicShow` maps centroid to hue (cool → warm as centroid rises) so the palette drifts as the song's tonal character shifts.

### Energy envelope

Rolling RMS across all FFT bins, smoothed over ~0.5-1 second. The song's volume contour minus the per-beat transients. Multi-second changes are typically subtle but cumulatively make the lights feel like they're *part of* the song rather than superimposed on it. Shows map it to brightness or intensity.

### Onset density

Events-per-second across all bands, smoothed. Tracks how busy the music is — a chorus with kick-snare-hihat firing densely has higher density than a sparse verse. Shows map it to effect probability or layer count.

`on_music_descriptor()` carries centroid + energy + density together, fired at FFT rate (master-internal, no wire cost). Rate-limited delivery to Show hooks: only fired when any value changes by more than the configured threshold (default 5 %), to avoid useless every-frame churn.

### Section detection

Rolling 4-8 second analysis of the three continuous descriptors plus onset density. Identifies song-structure transitions and fires `on_section_change(section)`:

- **Verse** — low-to-mid energy, low-to-mid centroid, sparse density
- **Chorus** — high energy, mid-to-high centroid, mid-to-dense density
- **Build-up** — rising energy + rising centroid + rising density over 4-8 seconds
- **Drop** — bass-band spike following a build (already detected in Epic 4.5; surfaced here as `SECTION_DROP`)
- **Breakdown** — low energy, low density, sustained > 2 seconds (already detected in Epic 4.5)
- **Vocals-only** — mid-band energy, very little bass, low density
- **Instrumental-break** — dense low-bass and high-band, sparse vocal mid-band

Lives in the analyser layer alongside the other primitives so Shows that don't care simply don't subscribe.

## Architectural integration

### Show plug-in API (new)

A new `nocturnation::shows::Show` base class atop the existing `Plugin` base. Hook surface:

```cpp
class Show : public Plugin {
public:
    // Identity (Plugin contract)
    virtual const char* id()           const = 0;
    virtual const char* display_name() const = 0;

    // Lifecycle
    virtual void on_enter() {}
    virtual void on_exit()  {}

    // Analyser hooks (default no-op so Shows opt in)
    virtual void on_beat_detected(uint8_t strength) {}
    virtual void on_snare_detected(uint8_t strength) {}
    virtual void on_hihat_detected(uint8_t strength) {}
    virtual void on_music_descriptor(uint8_t centroid,
                                       uint8_t energy,
                                       uint8_t density) {}
    virtual void on_section_change(SectionType section) {}

    // Per-frame screen draw (Show owns the canvas)
    virtual void on_render(Canvas&) {}

    // Button events (Show owns button semantics except back gesture)
    virtual void on_button(ButtonId, ButtonEvent) {}
};
```

`AutonomousMasterMode` becomes a thin host: holds the active Show, dispatches analyser events into it, dispatches button events into it, calls `on_render()` per frame. The Show makes `DAL::render_fx("<class>:<group>", ev)` calls when it wants to drive devices.

### Show selection

Operator picks a Show via the master-mode entry path:

- On entering Master mode, either jump straight to the last-used Show (NVS `active_show`) or surface a brief picker if more than one Show is registered. Final UX settled in Block 1.
- `ConfigMode > Show` provides explicit selection / preview.
- New Shows register at firmware boot via a `register_show()` call alongside the existing plug-in registrations.

### Widget library (refactored)

`BeatPulse` and `SpectrumBars` classes are split:

- Screen-rendering logic becomes `BeatBarWidget` and `SpectrumBarsWidget` in a new `src/widgets/` library. Both expose a small API: `update(level)` + `draw(canvas, x, y, w, h)`. They are pure render helpers — no analyser subscription, no plug-in registration.
- A Show that wants in-show level tuning constructs the widget, feeds it analyser data (or manually-set values), and calls `draw()` from its own `on_render()`.
- `ConfigMode > Utilities > Level Tuning` becomes a small sub-mode that hosts the widgets standalone for bench work, supporting manual level injection so a developer can verify the IR/ESP-NOW path independently of audio.

### Slave mode

Unchanged. The Show framework is master-only. Slaves continue to:

- Receive `LIGHT_COMMAND` on ESP-NOW (target_class + target_group filtering per Epic 4.65)
- Render to `LocalDisplayBinding` (screen mirror) and `PixMobIrBinding` (IR relay)
- Respect `slv_group` device-wide filter on non-relay bindings

### Wire format

**No new wire payloads.** Analyser primitives are master-internal; Shows consume them and produce class+group-targeted `LIGHT_COMMAND` traffic via `render_fx()`. This is the Epic 4.65 settle held intact — the wire stays minimal.

## Scope

**Included**

- Show plug-in framework (`Show` base class, `register_show()` registry, `AutonomousMasterMode` host refactor)
- `SimpleBeatShow` (preserves today's beat → broadcast-to-all behaviour)
- `DynamicShow` (consumes new analyser primitives; routes via class+group)
- Show selection UI (master-mode entry + `ConfigMode > Show`)
- `active_show` NVS key + legacy-`active_vis` consumption in `migrate_legacy_nvs_keys()`
- Widget library extraction: `BeatBarWidget`, `SpectrumBarsWidget` in `src/widgets/`
- `ConfigMode > Utilities > Level Tuning` sub-mode hosting the widgets with manual injection
- Analyser primitives: multi-band onset (snare, hi-hat), spectral centroid, energy envelope, onset density — all in `src/audio_analyser/`
- Section-detection state machine in `src/audio_analyser/`
- Native unit tests for all primitives against captured audio samples
- Hardware verification: Plus2 and S3 produce equivalent dynamic-show output for the same input audio
- Developer documentation (`docs/developing-shows.md`) covering Show plug-in concept, base-class API, registration, analyser hook surface, `render_fx()` targeting, widget composition, NVS, and testing patterns — with `DynamicShow` as the worked example
- Architecture spec update to v0.23 reflecting Show plug-in surface, widget library, analyser primitives

**Explicitly excluded**

- Third-party widget plug-in surface (widgets stay library-only this Epic; promote to plug-in surface later if developer demand surfaces)
- New ESP-NOW message types — analyser output stays master-internal
- DMX / Art-Net / QLC+ integration (Epic 7)
- Tempo-aware autocorrelation tracking (post-EMF stretch)
- ML-based section recognition (rule-based is sufficient)
- Genre-specific parameter profiles (operators retune in Utilities if needed)
- Audio fingerprinting / song identification
- Pre-analysed cue files
- Companion phone-app integration
- BLE work (separate future Epic per spec §4.1)

## Acceptance Criteria

- [ ] **(B)** Code builds cleanly across all PlatformIO environments (`pio run`).
- [ ] **(L)** Native unit tests pass for: multi-band onset (kick-only drum machine fires kicks but not snare/hihat; pop track fires kick + snare alternation; hi-hat-heavy track fires hi-hats densely without false-firing on bass).
- [ ] **(L)** Spectral centroid output verified against reference Python (librosa) for known signals: 100 Hz sine → bin ≈ 3; 4 kHz sine → bin ≈ 128; white noise → bin ≈ 128.
- [ ] **(L)** Energy envelope tracks the volume contour of a quiet-loud-quiet test signal.
- [ ] **(L)** Section detection identifies sections correctly on a labelled test track (manual annotation as ground truth; > 70 % accuracy on transition timing within ±2 seconds).
- [ ] **(L)** Show plug-in framework: stub Show registered with the registry is reachable via the registry API; hook dispatch fires on simulated analyser events; show switching invokes `on_exit()` / `on_enter()` cleanly.
- [ ] **(H)** Master-mode entry shows the active Show by default; the picker / ConfigMode path can change the active Show and the new choice persists across reboots.
- [ ] **(H)** `SimpleBeatShow` produces the same visible behaviour as the pre-refactor firmware (no regression for existing users).
- [ ] **(H)** `DynamicShow` with a varied playlist (pop, dance, drum & bass, ballad, ambient, drum machine): kick events route to group 1; snare to group 2; hi-hat to group 3; centroid drives hue drift; energy drives brightness; section transitions produce visible palette changes.
- [ ] **(H)** Cross-device consistency: Plus2 and S3 placed side-by-side produce visually equivalent output for the same audio under both Shows.
- [ ] **(H)** Widget library: `BeatBarWidget` and `SpectrumBarsWidget` render correctly both standalone (Utilities > Level Tuning) and composed inside `DynamicShow`. Manual level injection in Utilities drives the widgets without audio input.
- [ ] **(H)** Subjective "feels alive" test: Jason plays a varied playlist through `DynamicShow`; the lights produce a visibly different show for each genre without per-genre configuration.
- [ ] **(H)** No regression: Epic 4.5 beat detection still works; Epic 4.6 UI is unchanged; Epic 4.65 class+group routing still works; slave mode still relays IR + renders local-screen mirror.
- [ ] **(L)** Developer documentation at `docs/developing-shows.md` covers: Show base-class API, registration, analyser hook surface, `render_fx()` targeting with class+group, widget composition, NVS persistence, and native + hardware testing patterns. A contributor can write and register a new Show from the doc alone, without reading the framework source.
- [ ] Architecture spec updated to v0.23.

## Blocks of work

### Block 1: Show plug-in framework + `SimpleBeatShow`

Land the framework first, preserve current behaviour, no analyser changes.

- New `include/shows/show.h` with the `Show` base class and registry
- New `src/shows/show_registry.cpp` matching the existing plug-in-registry style
- `AutonomousMasterMode` refactored to host the active Show: subscribes to analyser events on its behalf, dispatches button events, calls `on_render()`
- `SimpleBeatShow` (`src/shows/simple_beat_show.cpp`): reproduces the pre-refactor behaviour by consuming `on_beat_detected()` and calling `DAL::render_fx("00:00", ev)`
- `active_show` NVS key + `migrate_legacy_nvs_keys()` consumes `active_vis`
- Master-mode entry: jump to active Show; surface a picker if more than one is registered
- ConfigMode entry under `> Show` (top level) for explicit selection
- Native unit tests: Show registry / framework
- Commit: "Show plug-in framework with SimpleBeatShow preserving current behaviour"

### Block 2: Widget library extraction

Move screen-rendering logic out of `BeatPulse` / `SpectrumBars` into reusable widgets; add Utilities entry.

- New `include/widgets/beat_bar.h` + `include/widgets/spectrum_bars.h`
- New `src/widgets/beat_bar.cpp` + `src/widgets/spectrum_bars.cpp` with `update()` + `draw()` API
- Retire `BeatPulse` / `SpectrumBars` Plugin registrations (their screen logic now lives in widgets; their analyser→fx logic is absorbed by `SimpleBeatShow` or retired)
- `ConfigMode > Utilities > Level Tuning` sub-mode hosting the widgets with manual injection
- Native unit tests: widget render math, manual-injection path
- Commit: "Widget library + Utilities level-tuning sub-mode"

### Block 3: Analyser primitives — multi-band onset + descriptors

Extend the audio analyser; expose new events on the Show API.

- Identify snare (~200-2000 Hz, sub-bands 5-15) and hi-hat (~5000-8000 Hz, sub-bands 22-30) sub-band ranges given the existing FFT setup
- Apply Epic 4.5's adaptive-threshold algorithm per band, with band-appropriate threshold multipliers and refractory periods
- Implement spectral centroid (per-frame scalar)
- Implement rolling-RMS energy envelope (smoothed over ~0.5-1 s)
- Implement onset-density tracker (events-per-second, smoothed)
- Add `on_snare_detected`, `on_hihat_detected`, `on_music_descriptor` hooks to the `Show` base class
- Rate-limit `on_music_descriptor` delivery: only fire when any value moves > 5 % from previous
- Native unit tests against captured audio samples
- Commit: "Multi-band onset + spectral centroid + energy + density primitives"

### Block 4: Section detection

The longer-window state machine.

- Rolling 4-8 second history buffers for centroid, energy, density
- State machine identifying verse / chorus / build-up / breakdown / vocals-only / instrumental-break / unknown
- Tune transition rules using captured audio samples with manual section labels as ground truth
- Add `on_section_change(SectionType)` hook to the `Show` base class
- Native unit tests: labelled test track produces correct section labels with > 70 % timing accuracy
- Commit: "Section detection state machine"

### Block 5: `DynamicShow`

The Epic's headline Show, consuming everything Blocks 3-4 expose.

- New `src/shows/dynamic_show.cpp`
- Hook routing: kick → group 1, snare → group 2, hi-hat → group 3 (via `render_fx("01:01", ev)` etc.)
- Centroid → hue mapping in the colour palette
- Energy → brightness mapping
- Density → effect probability / layer count
- Section transitions → palette changes
- Optional composition of `BeatBarWidget` + `SpectrumBarsWidget` on screen for in-show level visibility
- Commit: "DynamicShow consuming new analyser primitives with class+group routing"

### Block 6: Hardware tuning

Empirical listening + watching. Pure (H) work; no code changes outside parameter constants.

- Plus2 and S3 side-by-side with three or more slave devices on distinct groups
- Varied test playlist (Vengaboys, Coldplay ballad, drum & bass, dance with build/drop, ambient, drum machine, podcast for silence-test, rock)
- Tune threshold multipliers, smoothing time constants, section-detection rules, palette mappings until each genre produces a visibly distinct and pleasant show
- Document final tuning parameters in the spec for future contributors
- Phone-recorded walkthrough video for README and demo material
- Commit: "Dynamic show tuning verified across genre playlist"

### Block 7: Developer documentation — writing a Show

A standalone developer guide so a contributor can write and register a Show without having to read the framework source. Doubles as a forcing function on the API: if something is awkward to explain in prose, that is a signal to revisit the API in the framework blocks before tuning lands.

- New `docs/developing-shows.md` covering:
  - **Concept**: Show as a master-side performance plug-in; how it fits between Modes, the audio analyser, the widget library, and the DAL.
  - **File layout**: where Show headers / sources / registration live (`include/shows/`, `src/shows/`).
  - **Show base-class API**: every virtual on `Show`, with "when fired / what to do" notes and the default no-op behaviour. Includes `id()`, `display_name()`, `on_enter()`, `on_exit()`, and every analyser, render, and button hook.
  - **Registration**: `register_show()` call site and registry ordering; how Shows appear in the master-mode picker.
  - **Analyser hook surface**: `on_beat_detected`, `on_snare_detected`, `on_hihat_detected`, `on_music_descriptor`, `on_section_change` — with timing characteristics (event rates, smoothing windows, refractory periods).
  - **Sending light commands**: `DAL::render_fx("<class>:<group>", ev)` with the structured-target format from Epic 4.65, the `hal::DeviceClass` enum, group conventions (0 = all, 1..n = specific), and `RgbPulseEvent` field semantics (r, g, b, attack, sustain, release, chance).
  - **Drawing to the screen**: `Canvas` API, frame cadence, what `on_render()` may and may not do.
  - **Button handling**: `ButtonId` / `ButtonEvent`, the reserved back-gesture (returns to mode picker), conventions for level adjustment inside a Show.
  - **Composing widgets**: how to instantiate and drive `BeatBarWidget` / `SpectrumBarsWidget` from inside a Show's `on_render()`.
  - **Persistence**: NVS conventions for per-Show settings (key namespacing under the "noct" namespace, defaults, migration patterns).
  - **Testing**: native-unit-test patterns for Shows using the existing test harness; hardware verification checklist.
- Worked example: annotated walk-through of `DynamicShow` (from Block 5) as the reference implementation, with rationale for its hook handling, target routing, and screen composition decisions.
- Cross-link from `README.md` (developer section) and `docs/architecture.md` (Plugin layers chapter).
- Commit: "Developer guide: writing a Show plug-in"

### Block 8: Architecture spec update

- Update spec §5 (Audio analysis pipeline) with multi-band, centroid, energy, density, section detection
- Add spec §7.x covering the new Show plug-in surface and the widget library; point readers at `docs/developing-shows.md` as the developer reference
- Update spec §3 / §4 references where they mention "visualisations" to distinguish Shows vs widgets
- Bump spec to v0.23
- Sync to Notion
- Commit: "Architecture spec v0.23 reflecting Show framework and dynamic show capabilities"

## Dependencies

| Dependency | Type | Status | Owner |
|---|---|---|---|
| Epic 4.5 (sub-band beat detection with per-band magnitudes exposed) | Internal | Done | Jason |
| Epic 4.6 (M5 UI cleanup, Visualisation plug-in surface) | Internal | Done | Jason |
| Epic 4.65 (class+group device addressing, render_fx structured targets) | Internal | Done | Jason |
| Architecture spec v0.22 (current baseline) | Internal | Done | Jason |
| Captured audio samples (varied genre playlist) | External | Available (Mac + audio interface) | Jason |
| Labelled test track (for section-detection accuracy testing) | External | To be prepared (manual labelling, ~1 hour) | Jason |

## Status Notes

Proposed 2026-05-09, refined 2026-05-11 following the Epic 4.65 close-out and a terminology pass with Jason that surfaced the Show vs widget distinction. The framework was the missing piece: today's BeatPulse / SpectrumBars conflated three concerns (analyser consumer, fx generator, screen renderer) with no seam. Splitting them into a Show plug-in surface + a widget library makes future Show contributions a drop-in exercise rather than a fork-and-modify.

**Sequencing**: 4.7 ships **before Epic 5 (Tildagon)** so EMF 2026 gets a properly dynamic show and so the Tildagon receiver app lands on a stable richer-protocol baseline. If the calendar tightens during execution, swap 4.7 and 5 and ship the dynamic show post-EMF. **The walk-before-run discipline still holds:** 4.5 → 4.6 → 4.65 → 4.7 → 5.

**Key technical risk**: Block 6's tuning tail is open-ended. The difference between "works" and "feels alive" is hours of subjective listening, not algorithm changes. Realistic effort estimate for Blocks 1-5 is 12-18 hours; Block 6 adds an indeterminate listening-and-tuning tail that is best done across multiple sessions.

**EMF 2026 demo angle**: this Epic's deliverable is what makes the EMF demo *demonstrable*. A single Stick on a tripod running `DynamicShow` produces a properly atmospheric installation — the kind of thing visitors stop and watch rather than glance past. That is itself worth showing at EMF, regardless of whether the Tildagon receiver app ships in time.

**Forward-looking note on widget plug-in surface**: widgets stay library-only this Epic. If contributors propose useful new widgets (VU meter, waveform scope, kick/snare/hihat scrolling histogram, etc.), promoting widgets to a plug-in surface is a small, contained follow-up Epic.

Processing Type stays **Hybrid** because algorithm work is well-suited to laptop-driven coding (with native unit tests), but Block 6 is genuinely **Manual** — Jason listening to music with hardware in hand for several hours is the only reliable test for "does this feel right?".

## Close-out (2026-05-11)

Closed with Blocks 1-5, 7, and 8 shipped to main and verified against 348 native tests across 17 envs. Block 6 (hardware tuning) was done inline via a series of bench iterations against bracelets rather than as a discrete tuning pass — the tuning surfaced architectural fixes that fed back into the code rather than just parameter tweaks. Highlights:

- **Master-IR loopback in `dispatch_output_class_group`**. `render_fx` calls fire ESP-NOW broadcast + master's PixMob IR LED + master's screen pulse from one entry point. Shows no longer hand-roll a "fire to all-pixmobs in addition" call - the dispatch path treats the master as its own slave for output purposes. Class+group filtering is honoured (only Light-class targets reach IR; only Screen-class reach LocalDriver).
- **IR reset primer**, idle-gated. Bench observation that Pulse / Fade fires landed but bracelets didn't respond, and that Sparkle / WhiteOut fades picked up colour artefacts and ended abruptly. Diagnosed as residual envelope state on bracelets between commands. Fix: `dispatch_output_class_group` sends an rgb=0 broadcast primer before the main fire when the IR transmitter has been idle for > 300 ms. The primer clears bracelet state; the main fire then runs cleanly. Continuous high-cadence streams (Rainbow at 25 ms cycle) skip the primer via the idle gate. **(Rolled back in Epic 4.8 on 2026-05-12: further bench testing showed the extra IR frame doubled effective traffic on every sparse-cadence show and overloaded the bracelet receivers — only Rainbow, which skipped the primer via the idle gate, rendered reliably. The dispatch now sends exactly one IR frame per `render_fx` call; shows manage residue by sizing envelopes to fit inside their fire cadence.)**
- **`DynamicShow.groups` property**, default 1 = broadcast. Bench testing confirmed bracelets ship at random groups, so per-drum group routing (kick→1, snare→2, hi-hat→3) only works after the operator pre-programmes bracelets. Default 1 fires everything to PixMob group 0 (broadcast), works out of the box on any deployment. Operator bumps to 3 for the full per-drum split.
- **TestMode unification**. Pulse / Fade / Rainbow / Sparkle / WhiteOut all collapse from three render_fx calls each (`"all-pixmobs"` + `"local"` + `"00:00"`) to one (`"00:00"`). The loopback handles fan-out.
- **Sparkle re-tuned**. White-only (was random palette), CHANCE_16 (~20 %), step 1100 ms / ~0.9 Hz cadence with a 1 s fade envelope (T_0 + T_480 + T_480 = 960 ms) so each twinkle fades cleanly before the next fire. Reads as crowd-shimmer. (Originally tuned so the next *primer* would not clip the fade; same gap-arithmetic still holds against the next *main* fire after the Epic-4.8 primer rollback.)
- **PixMob protocol broadcast investigation**. Tried an 8-byte frame variant for `groupId=0` (omitting the group byte). Bench-tested it; broke everything. Reverted - the 9-byte frame with `restrictGroupId=0` is the correct broadcast shape and parity with `jamesw343/PixMob_IR` is preserved.

Outstanding follow-up that didn't gate the close-out: the architecture spec's §7.6 plug-in surfaces text was already aligned in v0.23 (Block 8), but the README and docs/developing-shows.md haven't been re-flowed for the IR primer / screen loopback architecture additions. Worth a sweep when Epic 4.8 (documentation) writes the protocol manual.

**Final state**: 348 tests across 17 envs, Plus2 + S3 firmware builds clean, bench-validated against DynamicShow at the venue. Architecture spec v0.23 (synced to Notion). Developer guide at docs/developing-shows.md (cross-linked from README).
