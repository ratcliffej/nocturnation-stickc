---
title: "Epic 6B: Tildagon Director + Show framework"
status: In progress
notion_url: TBD
notion_id: TBD
notion_status: Not started
last_synced: never
sync_direction: local-canonical-during-implementation
---

# Epic 6B: Tildagon Director + Show framework

> **Working copy.** This file is canonical during Epic 6B implementation. No Notion page exists; per 2026-05-19 sign-off, the Notion page will be created at Epic Done with the final body content (down to Status Notes) — avoids two-way sync drift while the Scope / Acceptance Criteria evolve mid-implementation. Implementation Blocks / Progress Log / Block Notes sections stay local for the lifetime of the Epic and are not synced.

## Related Documents

- [Developing a Show](../developing-shows.md) — the cross-platform Show plug-in spec. Currently written for the M5 C++ surface only; B8 of this Epic refreshes it to be host-agnostic with a host-capabilities matrix.
- [HAL design](../hal-design.md) — the capability model B1 extends.
- [DAL design](../dal-design.md) — context for how HAL capabilities propagate up through the analyser/effect/show stack.
- [Architecture spec](../architecture.md) — load-bearing for the Show framework's place in the firmware stack.
- [Epic 6 (NocturNation public launch — EMF 2026)](https://www.notion.so/358bd06774058159916fed66a3f3aaf4) — the parent release-blocker workstream. Epic 6B is a sibling Epic that lands the Tildagon Director side; Epic 6 proper covers the launch ceremony and EMF app-store submission.
- [Epic 5: Tildagon receiver app](epic-05-tildagon.md) — prior Tildagon work; this Epic extends `apps/nocturnation/` with a Director-mode opt-in alongside the existing Lume mode.

## Goal

Bring the M5 firmware's Show plug-in framework to the Tildagon in MicroPython, with the same hook surface as the C++ `Show` base class on M5. Add IMU tap-to-beat (and richer IMU usage) as the Tildagon Director's primary input source, with a button fallback for show development without an IMU. Ship at least one reference Show and a folder-per-show layout so users can drop a new Show in by adding `apps/nocturnation/shows/<show_id>/`.

The cross-platform commitment is that a developer reading [developing-shows.md](../developing-shows.md) should be able to write a Show on either host without learning a different API; the host-capabilities matrix tells them which hooks fire on which host.

## Business Value

The Tildagon Director closes the loop on a community deployment story: a single badge can run a show without the operator carrying an M5 Stick. For EMF 2026 specifically, this lets attendees with badges run their own micro-shows on channel 1 (per the channel 11 access-control constraint established in Epic 5.5 — Tildagon Directors are confined to the hobby channel) and tap-to-beat on the IMU is a tangible, demoable interaction that makes the product feel alive in a way that the receive-only Lume mode does not.

The Show plug-in framework is also the natural shape for community contribution. A folder-per-show layout means a contributor can add a Show without touching the host or the protocol; that's the bar at which open-source Show contributions become realistic.

## Scope

### Capability model extension (B1)

The HAL `Capability` enum (currently 13 flags on M5: Mic / IRTx / IRRx / ESPNow / Display / Buttons / IMU / Battery / Bluetooth + 4 lit analyser sub-caps + 4 reserved analyser sub-caps) extends with two IMU sub-capabilities:

- `ImuTap` — fires `on_tap_detected(strength)` and `on_beat_detected(strength)` (Director-mode tap-to-beat).
- `ImuMotion` — fires `on_motion_event(axis, magnitude)` for waving / gesture input.

The single coarse `Capability::IMU` flag stays for host-declaration purposes (the host has an IMU); the sub-capability flags say which of the event streams the host's IMU backend actually produces. This mirrors the `Mic` + `AnalyserBeatDetection` pattern from Epic 4.5. Free-fall detection was considered and dropped per 2026-05-19 sign-off — no v1 Show consumes it and speculative reserved sub-flags add cognitive cost without value.

The Tildagon also wants a per-Show **sensitivity** property (low / medium / high) controlling the tap threshold + motion magnitude floor. This is per-Show because different Show concepts want different tap behaviours (a beat-driven show wants brisk taps, a motion-driven show wants slow waves).

### MicroPython mirror of the Plugin / Show surface (B2)

`Plugin` base class equivalent — a Python class with:

- `id()` / `display_name()` — Plugin identity.
- `required_capabilities()` — returns a `CapabilityMask` declaring which hardware/event sources the Show needs to run.
- `properties()` — returns a list of `PropertyDef` (Bool / U8 / Enum / Colour).
- `power()` — power-profile dict equivalent (analogue of M5's `PowerProfile` struct: `needs_audio_frames`, `needs_imu_motion`, `tick_hz`, etc.).

`Show` base class — extends `Plugin` with the analyser/input/render hooks from [developing-shows.md](../developing-shows.md):

- Lifecycle: `enter(ctx)` / `exit(ctx)`.
- Analyser hooks (no-op defaults on Tildagon at v1): `on_beat_detected(ctx, strength)` / `on_snare_detected(...)` / `on_hihat_detected(...)` / `on_music_descriptor(...)` / `on_section_change(...)` / `on_audio_frame(ctx, ev)`.
- New IMU hooks: `on_tap_detected(ctx, strength)` / `on_motion_event(ctx, axis, magnitude)`. Same hooks added to the M5 `Show` base for forward compatibility, with no-op defaults.
- Input: `on_input_action(ctx, ev)`.
- Render: `on_render(ctx)` at ~20 Hz when no overlay is open.
- Property change: `on_property_changed(ctx, key)`; tick: `tick(ctx, now_ms)`.

`ShowContext` — services surface for the Show:

- `ctx.render_fx(target, ev)` — fans to ESP-NOW broadcast + Tildagon's own perimeter LEDs + LCD loopback (B3).
- `ctx.get_property(key)` / `ctx.set_property(key, value)`.
- `ctx.analyser_caps()` / `ctx.imu_caps()` — runtime queries.
- `ctx.paused()` / `ctx.set_paused()`.
- `ctx.now_ms()` / `ctx.since_enter_ms()`.

### Folder-per-show layout (B2)

```
apps/nocturnation/shows/
  __init__.py
  simple_tap/
    __init__.py        # exports `Show` subclass
    README.md          # optional per-show docs
  motion_wave/
    __init__.py
    palettes.json      # optional per-show data
```

`show_registry()` walks `apps/nocturnation/shows/<dir>/` at boot, imports `<dir>/__init__.py`, calls the registered Show factory, and registers the resulting instance. Registration order = directory iteration order, alphabetical. Default Show is the first registered (`simple_tap`).

### Render dispatch on Tildagon Director (B3)

Mirrors the M5 `dispatch_output_class_group` in [src/dal/dal.cpp](../../src/dal/dal.cpp). One `ctx.render_fx("01:01", ev)` call fans to:

1. **ESP-NOW broadcast** — always; encode + send a `LIGHT_COMMAND` frame via the existing Tildagon protocol layer.
2. **Tildagon perimeter LED loopback** — when `target_class` is `0x00` or `0x01` (Light). Reuse the existing `PerimeterRenderer` from the Lume side.
3. **Tildagon LCD loopback** — when `target_class` is `0x00` or `0x02` (Screen). Reuse the existing `LcdRenderer`.

Tildagon Director is its own first Lume — same architecture as M5 from Epic 4.7 onwards.

### IMU input adapter (B4)

A new module `apps/nocturnation/nocturnation/imu.py`:

- Initialises the Tildagon's onboard IMU at app start.
- Polls at ~50 Hz from the `update()` tick; tap detection thresholds the high-pass-filtered Z axis; motion detection thresholds the magnitude of the 3-axis acceleration vector.
- Emits `TapEvent(strength)` / `MotionEvent(axis, magnitude)` via a callback.
- Sensitivity property (low / medium / high) scales the thresholds.

The Director host translates these events into `on_tap_detected` / `on_beat_detected` / `on_motion_event` calls on the active Show. (Tap fires both `on_tap_detected` *and* `on_beat_detected` so beat-driven Shows can be host-agnostic — the same hook fires on M5 from the mic-driven beat detector and on Tildagon from a tap. Whether a Show should be tap-driven or beat-driven is a Show-author decision.)

### Button-as-tap fallback (B5)

Holding button C (or another configurable button) for ≥100 ms produces a synthetic `TapEvent(strength=192)`. Useful when the IMU isn't tuned or the operator wants deterministic taps for show development.

### DirectorMode FSM in app.py (B6)

Reuses the M5 mode-FSM idea. New state: `DirectorMode`. Holds exactly one active Show. Overlays:

- **Show picker**: triggered by `Picker` action (long-press button F? TBD on Tildagon button mapping). Lists registered Shows by `display_name()`. Confirm activates; Cancel backs out.
- **Settings overlay**: triggered by `Settings` action. Auto-generated from the active Show's `properties()`. Writes go to NVS under `ns_<show_id>` (mirroring M5 convention).
- **Pause**: toggles `ctx.paused()`.

Active Show id persists in NVS at `noct/active_show`; loads on next Director-mode entry.

### Reference Show: `simple_tap` (B7)

- Consumes `on_tap_detected(strength)`.
- Fires `render_fx("01:01", RgbPulse(palette_colour, attack=T_0, sustain=T_96, release=T_480))`.
- Property: `colour` enum (Cool / Natural / Warm / Rainbow / Red / Green / Blue / Custom).
- On `Cycle` action: advance palette.
- On `Confirm` action: trigger a synthetic tap (alt path to button-as-tap).
- Renders a simple "tap to beat" prompt on the LCD with the current palette name.

Stretch: `motion_wave` reference Show that consumes `on_motion_event` to drive a colour-by-axis effect. Decision deferred to B7 start; depends on whether B4 lands cleanly enough to justify a second reference within this Epic.

### Cross-platform doc refresh (B8)

`docs/developing-shows.md` is refreshed:

- **Host capabilities matrix**: which hooks fire on which host. M5 has all the analyser hooks; Tildagon has the IMU hooks; both have input + render + lifecycle.
- **MicroPython surface**: Python equivalents of the C++ examples. Side-by-side where useful.
- **Folder layout** for both hosts.
- **Porting your Show across hosts**: short section on what stays the same and what doesn't.

The doc stays single-source. Platform differences are tabular, not separate documents.

### Bench verification (B9)

Live deployment: Tildagon Director on channel 1 + one Tildagon Lume + at least one M5 Lume. Tap a beat on the Director; confirm both Lumes light. Cycle the palette via `Cycle`; confirm the colour changes propagate.

## Acceptance Criteria

- [ ] HAL `Capability` enum extended with `ImuTap`, `ImuMotion` (no `ImuFreeFall` — dropped per 2026-05-19 sign-off). M5 declares the existing `IMU` flag plus whichever sub-caps the M5 IMU backend actually produces (none at v1 — declared but unwired, per the Epic 4.5 analyser-reserved pattern). Tildagon declares `IMU` + `ImuTap` + `ImuMotion`.
- [ ] Tildagon-side `Plugin` / `Show` / `ShowContext` Python classes match the M5 hook surface 1:1 for shared hooks. New IMU hooks added to both.
- [ ] `apps/nocturnation/shows/` folder-per-show layout discovered at boot; a Show can be added by dropping a new folder in without touching the host.
- [ ] `simple_tap` reference Show ships; cycles colour on `Cycle`; fires `render_fx` on tap; renders LCD with current palette.
- [ ] IMU input adapter fires `on_tap_detected` on Director taps; sensitivity property tunes the threshold.
- [ ] Button C fallback produces synthetic taps when held.
- [ ] DirectorMode FSM is selectable from the existing Tildagon app entry; show picker + settings overlay function; active show id persists across reboots.
- [ ] `developing-shows.md` refresh shipped with the host-capabilities matrix and the MicroPython surface.
- [ ] Bench-verified: Tildagon Director taps light a Tildagon Lume and an M5 Lume on channel 1.

## Order of work

B1 → B2 → B3 → B4 → B5 → B6 → B7 → B8 → B9.

B1 is a design pre-pass producing block notes only; no code. B2 is the first code block.

## Dependencies

- Epic 5.5 (Channel 11 access control) — closed 2026-05-17. Establishes the Tildagon-Director-confined-to-channel-1 constraint that B6 honours.
- Epic 4.7 (M5 Show plug-in framework) — closed; provides the C++ reference surface this Epic mirrors.
- Epic 5 (Tildagon receiver app) — closed; provides the perimeter / LCD renderers and the protocol stack B3 reuses.

## Target Sprint Range

Started 2026-05-19. Estimate 1.5-2 weeks of focused work. No external deadline within this Epic itself; Epic 6's EMF deadline is the parent constraint.

## Status Notes

2026-05-19: Epic 6B opened. B1 design pre-pass complete and signed off. `ImuFreeFall` dropped from the proposal; Notion page deferred to Epic Done. B2 (Show framework) complete: MicroPython Plugin/Show/ShowContext + folder-per-show registry on Tildagon, M5 capability/hook extension, both firmware envs green. B3 (render_fx dispatch) complete: frame encoder + RgbPulse + RenderDispatcher (broadcast + perimeter/LCD loopback) + DirectorHost. B4 (IMU adapter) complete: ImuAdapter with gravity-EMA high-pass tap onset + motion envelope + Low/Med/High sensitivity. B5 (button-as-tap fallback) complete: ButtonTapSource rising-edge tap + optional auto-repeat. B6 split into B6a (testable core) + B6b (app.py FSM, bench-only). B6a complete: InputAction, framework-bound Show.context(), Settings.active_show, DirectorController (input routing + show lifecycle/selection + sensitivity + tick). B6b code-complete (foundations host-tested + app.py FSM glue compiling, bench-verified at B9, gated on B7). B7 complete: simple_tap + motion_wave reference Shows, folder-per-show, auto-discovered; 381 Tildagon tests passing. B8 complete: developing-shows.md refreshed cross-platform (hosts-and-capabilities matrix, IMU hooks, MicroPython surface, porting section; Notion sync deferred to Epic Done). Only B9 (bench verification, hardware) remains.

---  

## Implementation blocks

| Block | Title | Status | Notes |
|------:|-------|--------|-------|
| B1 | Capability model design pre-pass | Done | Research-only. Output is the `Block notes / B1` section below. Signed off 2026-05-19. |
| B2 | Plugin / Show / ShowContext + folder-per-show registry | Done | MicroPython framework + M5 enum/hook extension. 62 new Tildagon tests; both M5 firmware envs build clean. |
| B3 | `ctx.render_fx` dispatch on Tildagon Director | Done | Frame encoder + RgbPulse + RenderDispatcher (broadcast + perimeter/LCD loopback) + DirectorHost. 47 new tests; suite 266. |
| B4 | IMU input adapter + sensitivity property | Done | ImuAdapter: gravity-EMA high-pass tap onset + motion envelope + Low/Med/High sensitivity. 18 new tests; suite 284. |
| B5 | Button-as-tap fallback | Done | ButtonTapSource: rising-edge tap + optional auto-repeat metronome. Same on_tap shape as ImuAdapter. 15 new tests; suite 299. |
| B6a | DirectorController orchestration core + active-show persistence | Done | InputAction enum, framework-bound Show.context(), Settings.active_show, DirectorController (lifecycle/selection/input-routing/sensitivity/tick). 32 new tests; suite 331. |
| B6b | app.py DirectorMode FSM + picker + per-show settings overlay + Show-owns-screen draw | Code complete (bench-pending) | Foundations host-tested (commit 0a9afe5); app.py glue compiles + suite green but runs only on hardware. Bench-verified in B9. |
| B7 | `simple_tap` reference Show (+ `motion_wave`) | Done | Two folder-per-show Shows under apps/nocturnation/shows/; auto-discovered. 17 new tests; suite 381. Unblocks Director mode for bench (B9). |
| B8 | `developing-shows.md` cross-platform refresh | Done | Hosts-and-capabilities matrix, IMU-hooks section, MicroPython surface notes throughout, folder layout, button map, porting section. Notion sync deferred to Epic Done. |
| B9 | Bench verification | Not started | Hardware: Tildagon Director + Tildagon Lume + M5 Lume. |

## Progress log

2026-05-19 — Epic 6B opened. Working copy drafted; B1 design pre-pass drafted and signed off in the same session. `ImuFreeFall` dropped from the proposal; Notion page creation deferred to Epic Done. B2 next.

2026-05-19 — B2 done. MicroPython Show framework landed on Tildagon + M5-side capability/hook extension.
  - **M5 (nocturnation-m5)**: `hal::Capability` extended with `ImuTap` (17) + `ImuMotion` (18) at the end of the enum, reserved-but-unwired (no M5 backend produces them yet); `capability_mask.h` headroom comment refreshed. `Show` base class gained `on_tap_detected` / `on_motion_event` hooks with no-op defaults (forward-compatible). Both firmware envs (stickcplus2, stickcs3) build clean; native_plugin + native_show test envs pass (62 cases).
  - **Tildagon (nocturnation-tildagon)**: new packages — `nocturnation/hal/` (Capability enum + CapabilityMask, 1:1 with the C++ surface incl. subset_of), `nocturnation/plugins/` (Plugin base, PropertyType/PropertyDef/PowerProfile/PluginKind, PropertyBag with JSON persistence at `/nocturnation_plugins.json` sectioned per plug-in id), `nocturnation/shows/` (Show base with all hooks + IMU hooks, ShowContext services surface with render_fx/property/cap-query/time stubs, ShowRegistry + `discover_shows()` folder-walker). New top-level `apps/nocturnation/shows/` concrete-show library dir (empty, ready for B7). 62 new host tests; full suite 219 passing (was 157).
  - **Deliberate B2 stubs**: `ShowContext.render_fx` returns False with no host (B3 wires the real ESP-NOW + LED + LCD fan-out); `imu_caps()` empty until B4. Surface is final so concrete Shows written against it stay stable.
  - Show-author contract: a Show is a folder under `apps/nocturnation/shows/<id>/` with `__init__.py` exposing `make_show()`. Discovery is alphabetical; duplicate ids / missing factories / raising factories are skipped without crashing the picker.

2026-05-19 — B3 done (Tildagon-only; no M5 changes). `ctx.render_fx` now has a real dispatch path on the Director.
  - **Frame encoder** (`protocol/frame.py`): the Tildagon was receive-only, so no encoder existed. Added `encode_light_command(...)` → 17-byte wire frame (8-byte header + 9-byte LIGHT_COMMAND payload), inverse of the parser's LIGHT_COMMAND branch; all fields byte-masked so an out-of-range arg can't corrupt frame length. Added `make_light_command_frame(...)` → builds a `Frame` directly for local loopback (no encode→parse round-trip per render). Both exported from `protocol/__init__.py`.
  - **RgbPulse** (`render/pulse.py`): the render event a Show hands to `render_fx`. Mirrors `dal::RgbPulseEvent` — r/g/b + PixMob ASR envelope (Time enum indices) + chance. Sensible default envelope (T_0 / T_96 / T_480 / CHANCE_100). Exported from `render/__init__.py`.
  - **director package** (`nocturnation/director/`): `parse_target("<hex_class>:<hex_group>")` → (class, group), raises ValueError on malformed (Show bug surfaced loudly). `RenderDispatcher` fans one call to (1) ESP-NOW broadcast — always, via injected `send_fn`, radio errors swallowed; (2) perimeter loopback when class ∈ {All, Light, MultiLedScreen}; (3) LCD loopback when class ∈ {All, Screen, MultiLedScreen}. Class-gated only — the Director always sees its own output regardless of group, matching the M5 loopback. Owns a wrapping sequence counter + source_id. `DispatchResult` (truthy if anything happened). `DirectorHost` satisfies the ShowContext host contract (`dispatch_render_fx` / `now_ms` / `analyser_caps` empty / `imu_caps`).
  - **espnow_sender.py**: thin hardware adapter (`make_sender(esp)` registers the broadcast peer + returns a `send_fn`). NOT imported by the package `__init__` so host pytest never needs the badge `espnow` module. Bench-verified in B9, not host-tested.
  - **End-to-end path proven in tests**: a Show's `on_tap_detected` → `ctx.render_fx("01:01", RgbPulse)` → DirectorHost → RenderDispatcher → broadcast frame on the wire + 12 perimeter LEDs armed locally.
  - Deliberate carry-forward: app.py still doesn't instantiate a DirectorHost — wiring the real espnow sender + clock into the app is a B6 concern. B4 (IMU adapter) populates `imu_caps`. 47 new host tests; full suite 266 passing (was 219).

2026-05-19 — B4 done (Tildagon-only). IMU input adapter for the Director.
  - **`director/imu.py` `ImuAdapter`**: accelerometer → tap / motion events, pure logic with the hardware read injected (`acc_read_fn`, default `imu.acc_read` lazy-imported so host tests stay hardware-free — same pattern as PerimeterRenderer's `rng`). Detection pipeline per ~50 Hz poll: (1) slow EMA (α=0.05) of each axis = gravity vector; (2) high-pass = raw − gravity; (3) tap fires when |high-pass| crosses the sensitivity-scaled threshold past a 120 ms refractory, strength = over-threshold magnitude scaled to 0..255 (floored to ≥1 so an edge tap is still visible); (4) a faster EMA (α=0.30) of the magnitude is the motion envelope — motion fires (dominant axis + 0..255 magnitude) when the envelope sits above the motion floor, rate-limited to 100 ms, and **suppressed during the tap refractory** so one tap doesn't double-report as motion.
  - **Sensitivity**: Low/Med/High table scales tap_threshold (9.0 / 6.0 / 3.5 m/s²) + motion_floor (4.0 / 2.5 / 1.5). `set_sensitivity(level)` retunes at runtime (B6 calls it on Show change to honour each Show's `sensitivity` property); unknown level falls back to Medium so a corrupt property can't disable input.
  - **`IMU_ADAPTER_CAPS`** = CapabilityMask(IMU_TAP, IMU_MOTION) exported so B6 can `host.set_imu_caps(IMU_ADAPTER_CAPS)` and Shows requiring IMU_TAP gate on.
  - Thresholds are bench-tuning starting points (refined in B9). Carry-forward: B6 wires `poll(now_ms)` into the app's background_task and routes the tap callback to *both* `on_tap_detected` and `on_beat_detected` on the active Show. 18 new host tests (priming, tap onset, refractory, sensitivity scaling, motion envelope, rate-limit, tap-suppresses-motion, reset); full suite 284 passing (was 266).

2026-05-19 — B5 done (Tildagon-only). Button-as-tap fallback for show development.
  - **`director/button_tap.py` `ButtonTapSource`**: polled button state → synthetic tap events, same `on_tap(strength)` shape as the ImuAdapter so the host routes both identically. Pure logic: `poll(pressed, now_ms)` (caller reads the physical button; B6 reads the badge button in background_task). Two modes: edge-only (default, `repeat_ms=0`) fires one tap per press — tap the button in time like you'd tap the badge; auto-repeat (`repeat_ms>0`) fires every interval while held — a held-button metronome for sound-check. Fixed strength (default 192, a firm ~75 % tap, since a button has no force info; clamped 0..255). `reset()` clears press/timing state for clean Director-mode entry.
  - Carry-forward: B6 chooses which button drives it (CONFIRM/C is taken by settings in Lume mode; the DirectorMode FSM owns its own button map) and feeds `poll()` from the app loop. 15 new host tests (edge fire, no-refire-while-held, release/re-press, auto-repeat + stop-on-release, strength clamping, reset, no-callback safety); full suite 299 passing (was 284).

2026-05-19 — B6 split into B6a (testable orchestration core) + B6b (app.py FSM, bench-only). Decision: the app.py mode-switch / overlays / Show-owns-screen draw can only be verified on hardware, so the host-testable core ships separately first. B6a done (Tildagon-only):
  - **`shows/input_action.py` `InputAction`**: semantic input vocabulary (Picker/Settings/Pause/Confirm/Cycle/CyclePrev) mirroring `hal::InputAction`. Exported from the shows package; Shows compare `action` against it in `on_input_action`.
  - **`Show.context()` relaxed**: was abstract (raised NotImplementedError); now returns a framework-bound context set via `bind_context(ctx)`. On Tildagon the controller owns the active Show's ShowContext and binds it on activation, so concrete Shows (B7) don't need to construct their own — removes boilerplate while keeping M5 API parity.
  - **`Settings.active_show`**: persisted Director active-show id (default ""), coerced to string, round-trips through the JSON store. Empty → controller falls back to the first registered Show.
  - **`director/controller.py` `DirectorController`**: the orchestration heart. Holds one active Show; wires the IMU adapter + button-tap callbacks to it once (they read the *current* active Show at call time, so a Show change needs no re-wire). `enter()` resets inputs + activates the restored/default Show; `exit()` exits it. `select_show(id)` switches (exit old → build PropertyBag+ShowContext → bind → enter new → persist via callback). Tap routing fires **both** `on_tap_detected` and `on_beat_detected` (host-agnostic beat hook). Motion → `on_motion_event`. `on_input_action(action)` toggles pause / returns OPEN_PICKER / OPEN_SETTINGS / delivers Confirm-Cycle-CyclePrev to the Show. Per-Show `sensitivity` property pushed to the IMU adapter on activation (reapplied on Show change; absent property leaves the adapter untouched). `tick(now_ms)` gated on the Show's `power().tick_hz`.
  - 32 new host tests (controller enter/selection/routing/sensitivity/input-action/tick/lifecycle + Settings.active_show); full suite 331 passing (was 299).
  - Carry-forward (B6b, bench-verified in B9): app.py mode switch (Lume↔Director), wire real DirectorHost (espnow sender via `make_sender` + `time.ticks_ms` clock + IMU `acc_read` + badge button reads), poll inputs in `background_task`, button→InputAction map, picker overlay (reuse the Menu pattern), per-Show settings overlay (auto-generate from `properties()`), and the Show-owns-screen draw path (needs a display surface on ShowContext).

2026-05-20 — B6b done in two commits (Tildagon-only). The app.py FSM is code-complete but runs only on the badge (bench-verified at B9).
  - **B6b part 1 (host-tested, commit 0a9afe5)**: `Settings.mode` ("lume"/"director", persisted, unknown→"lume"). `render/display.py CtxDisplay` — the Show's drawing surface wrapping the badge `ctx` (clear/fill_rect/text, origin-centred 240×240, colours 0..255→0..1); the app rebinds the live ctx each frame via `set_ctx`. `ShowContext.display()`/`set_display()`. `director/button_map.py DirectorButtonMapper` — edge-triggered nav→InputAction (UP=Picker, DOWN=Settings, RIGHT=Cycle, LEFT=CyclePrev), self-contained rising-edge detection so the app needn't `button_states.clear()` (which would wipe the held CONFIRM tap). 30 host tests.
  - **B6b part 2 (app.py glue + 2 controller methods)**: `DirectorController.poll_button()` (button-tap driven from foreground `update()`, separate from the always-running IMU poll) + `apply_sensitivity()` (re-push after a settings edit), both host-tested. `app.py`: `self._mode` from settings; `background_task` restructured into a session loop (Lume scan/receive vs Director); the Lume loops gained `and self._mode == "lume"` exits (no behavioural change in pure-Lume operation); `_director_session` builds the runtime via `_ensure_director` (discover_shows + RenderDispatcher with `make_sender(esp)` + perimeter/LCD loopback + DirectorHost(clock=ticks_ms, imu_caps) + ImuAdapter(imu.acc_read) + ButtonTapSource + DirectorController), claims channel 1, then polls IMU + ticks + renders the ring until mode changes; `_update_director` maps buttons (CONFIRM=manual tap, CANCEL=exit, nav→InputAction→picker/settings/cycle); `draw` delegates to the active Show's `on_render` via the bound CtxDisplay (wrapped in try/except — a crashing Show shows "show error", not a dead UI); picker + per-Show settings overlays reuse the `Menu` component; the Lume settings menu gained a "Mode" line to enter Director.
  - **Button map** (6 badge buttons): C/CONFIRM = manual tap (button-tap fallback, edge-detected, never cleared); F/CANCEL = exit Director→Lume; A/UP = picker; D/DOWN = per-Show settings; B/RIGHT = Cycle; E/LEFT = CyclePrev. PAUSE unmapped at v1 (no spare button; the controller supports it if wired later).
  - **Known bench items**: Director mode reverts to Lume until B7 ships the first Show (empty registry); the IMU poll runs in `background_task` (works backgrounded) while button input is foreground-only; mode-switch channel handling (a Lume lock from channel 11 vs the Director's channel-1 claim) needs a bench look. `app.py` `py_compile`s clean; full suite 364 passing (was 331). DirectorMode end-to-end is bench-only (B9), gated on B7.

2026-05-20 — B7 done (Tildagon-only). Two reference Shows, folder-per-show under `apps/nocturnation/shows/`, auto-discovered at boot. Director mode now has content (unblocks the B9 bench session).
  - **`shows/simple_tap/`** `SimpleTap`: the worked example. `on_tap_detected` fires `render_fx("01:01", RgbPulse(...))` with the current palette colour (T_0/T_96/T_480 envelope, CHANCE_100). `palette` enum (Cool/Natural/Warm/Rainbow/Red/Green/Blue, default Rainbow — Rainbow advances hue ~40° per tap via an inline HSV→RGB; the rest are fixed RGB) + `sensitivity` enum (Low/Med/High, default Medium — the controller pushes this to the IMU adapter on activation). Cycle/CyclePrev advance the palette; Confirm triggers a synthetic tap (forward-compat for hosts that route Confirm). `on_render` draws title + palette name + "tap to beat" via `ctx.display()`, with a tap-colour screen flash that decays over 200 ms.
  - **`shows/motion_wave/`** `MotionWave`: motion-driven second Show exercising `on_motion_event` + giving the picker a second entry. Dominant axis picks a colour (X=red, Y=green, Z=blue), magnitude scales brightness; fires `render_fx("01:00", ...)`. `sensitivity` property tunes the IMU motion floor.
  - Each Show is a folder with `__init__.py` exposing `make_show()` — the B2 contract. `id()`s `simple_tap` (10) / `motion_wave` (11), both ≤ 12 chars. `required_capabilities()` is DISPLAY + ESP_NOW (not IMU_TAP — both work via the button-tap fallback on a no-IMU badge; gating isn't enforced on Tildagon yet).
  - 17 new host tests (identity, caps, properties, tap→render_fx colour, rainbow hue advance, palette cycle/wrap, confirm-as-tap, on_render draw calls, motion axis-colour + magnitude scaling, `discover_shows()` finding both). Full suite 381 passing (was 364). B8 (developing-shows.md cross-platform refresh) next; B9 bench verifies the whole Director path.

2026-05-20 — B8 done (StickC docs only). `docs/developing-shows.md` refreshed from M5-only to cross-platform:
  - Intro reframed around two hosts (M5 C++ mic-analyser / Tildagon MicroPython IMU). New **Hosts and capabilities** section with a hook×host matrix (which hooks fire where) + a capability-vocabulary table; documents that a Tildagon tap fires both `on_tap_detected` and `on_beat_detected` (host-agnostic beat hook) and that `ImuTap`/`ImuMotion` are M5-reserved.
  - New **IMU hooks** section: `on_tap_detected` / `on_motion_event`, refractory/rate-limit, the per-Show `sensitivity` property, the button-tap fallback.
  - MicroPython equivalents added inline to ShowContext, Registration (folder + `make_show()`, no singletons), Sending light commands (`RgbPulse` + hobby-channel-only note), Drawing (`ctx.display()`, 240×240 round, 0-255 colours), Button handling (6-button map → InputAction), Persistence (native values + JSON `PropertyBag`), and Testing (`pytest` + fake host/display).
  - New **Porting a Show across hosts** section with a what-differs table and practical advice (key fires on `on_beat_detected` for portability; only `on_render` + registration are genuinely host-specific). Submission section gained Tildagon PR guidance.
  - The doc has `sync_direction: bidirectional` to its own Notion page; per the working-copy pattern the Notion push is deferred to Epic Done (avoids mid-Epic table/code-block sync churn). Local doc is canonical meanwhile.
  - **Only B9 (bench verification) remains** — needs hardware (Tildagon Director + Tildagon Lume + M5 Lume on channel 1) and folds in the bench-pending B6b app.py FSM.

## Block notes

### B1 — Capability model design pre-pass

**Status**: Signed off 2026-05-19. `ImuFreeFall` dropped from the proposal; remaining open questions resolved (see end of section).

**Context.** The M5 firmware's `hal::Capability` enum at [include/hal/hal.h:29-78](../../include/hal/hal.h#L29-L78) already declares 13 flags including a single coarse `IMU` and `Buttons` flag. Both are reserved-but-unwired: no host backend currently emits events from either, no Show currently declares them required. Epic 4.5 established the sub-capability pattern with `AnalyserBeatDetection` / `AnalyserDropDetection` / `AnalyserSpectrumFrame` / `AnalyserBandSummary` — coarse `Mic` plus four sub-flags saying which analyser products are available.

The same pattern fits IMU.

**Proposal — two new sub-capability flags.**

Add to `enum class Capability` immediately after `AnalyserSectionDetection`:

```cpp
// -------------------------------------------------------------------------
// IMU sub-capabilities (Epic 6B)
// -------------------------------------------------------------------------
//
// Composes what an IMU backend produces. A host with Capability::IMU
// declared also declares the subset of these that its IMU driver
// actually fires events for.
//
// Tap is the highest-value sub-capability for the Tildagon Director (and
// any future M5 Director with IMU tap-to-beat); Motion supports
// gesture-driven shows. Free-fall detection was considered and dropped:
// no v1 Show consumes it, and reserved-but-unwired flags accumulate
// cognitive cost without value when there's no near-term consumer.
ImuTap,         // produces TapDetectedEvent (high-pass-filtered Z onset)
ImuMotion,      // produces MotionEvent (3-axis magnitude / per-axis)
```

The coarse `Capability::IMU` stays. A host that declares `IMU` without any of the sub-flags has hardware but no event stream — a Show needing tap-to-beat will not run on that host. The host-gating logic in [src/modes/director_mode.cpp:54-63](../../src/modes/director_mode.cpp#L54-L63) already handles this correctly: `req.subset_of(host)` returns false if the Show requires `ImuTap` and the host doesn't declare it.

**Backend implications.**

- **M5**: a future Epic implements `M5MicroIMUBackend` (or similar) on Plus2 + S3 (both have an MPU6886-class IMU). At Epic 6B v1, M5 declares `IMU` but **none** of the sub-flags — same posture as the analyser-reserved flags from Epic 4.5. No M5 Show requires IMU at v1, so no behavioural change on M5 from this Epic alone. The reserved sub-flag declarations land now for cross-platform mental-model consistency.
- **Tildagon**: declares `IMU` + `ImuTap` + `ImuMotion`.

**MicroPython mirror.**

The Tildagon needs an analogue of `hal::CapabilityMask`. Pure-Python implementation:

```python
# apps/nocturnation/nocturnation/hal/capability.py

class Capability:
    # Coarse hardware (mirrors hal::Capability on M5)
    MIC          = 0
    IR_TX        = 1
    IR_RX        = 2
    ESP_NOW      = 3
    DISPLAY      = 4
    BUTTONS      = 5
    IMU          = 6
    BATTERY      = 7
    BLUETOOTH    = 8

    # Analyser sub-caps (Epic 4.5 set, reserved on Tildagon)
    ANALYSER_BEAT_DETECTION    = 9
    ANALYSER_DROP_DETECTION    = 10
    ANALYSER_SPECTRUM_FRAME    = 11
    ANALYSER_BAND_SUMMARY      = 12

    # Analyser sub-caps reserved on M5
    ANALYSER_MULTI_BAND_ONSET  = 13
    ANALYSER_SPECTRAL_CENTROID = 14
    ANALYSER_ENERGY_ENVELOPE   = 15
    ANALYSER_SECTION_DETECTION = 16

    # IMU sub-caps (Epic 6B)
    IMU_TAP        = 17
    IMU_MOTION     = 18


class CapabilityMask:
    __slots__ = ("_bits",)

    def __init__(self, *caps):
        self._bits = 0
        for cap in caps:
            self._bits |= (1 << cap)

    def set(self, cap):
        self._bits |= (1 << cap)
        return self

    def has(self, cap):
        return bool(self._bits & (1 << cap))

    def subset_of(self, other):
        return (self._bits & ~other._bits) == 0

    def __or__(self, other):
        out = CapabilityMask()
        out._bits = self._bits | other._bits
        return out
```

This is a deliberate 1:1 port of the C++ `CapabilityMask` constexpr bitset, with integer-indexed flags instead of an `enum class`. The `subset_of` semantics match.

A Tildagon Show declares:

```python
class SimpleTap(Show):
    def required_capabilities(self):
        return CapabilityMask(Capability.DISPLAY, Capability.ESP_NOW, Capability.IMU_TAP)
```

Host gating fires identically to M5: `host_caps = build_from_imu_backend()`; `if not show.required_capabilities().subset_of(host_caps): skip_show()`.

**Per-Show sensitivity property.**

Per Jason's note, the IMU is multimodal and the same hardware can be tuned for different show concepts. The right home for this is a per-Show property, not a HAL-level config. Add to `Plugin::properties()`:

```python
PropertyDef(
    key="sensitivity",
    type=PropertyType.ENUM,
    default_value=1,                 # Medium
    enum_names=["Low", "Medium", "High"],
    display_name="Sensitivity",
)
```

The `IMU` adapter (B4) reads the active Show's `sensitivity` property and scales its detection thresholds accordingly. This means cycling Shows in DirectorMode automatically retunes the IMU — a beat-driven Show with `Medium` sensitivity and a motion-driven Show with `Low` sensitivity coexist cleanly because each Show declares its own preferred operating point.

**Spec implications for [developing-shows.md](../developing-shows.md).**

B8 ships the doc refresh. For B1's purposes:

- Add a "Host capabilities matrix" section before the existing "Analyser hooks" section.
- The matrix lists every hook the framework defines and ticks which hosts fire it. M5: all analyser hooks + lifecycle + render + input. Tildagon: all IMU hooks + lifecycle + render + input. Both: analyser hooks declared but no-op (so a Show written for one runs cleanly on the other, just without the hooks the destination host doesn't drive).
- Add an "IMU hooks" section parallel to "Analyser hooks". Tap / Motion, refractory / threshold / sensitivity-property notes.

**No code is written under B1.** B2 starts the MicroPython implementation; the C++ enum extension lands as part of B2's first commit (it's a one-liner in `include/hal/hal.h` plus matching test fixtures, and B2 needs the M5-side reservation before declaring it on Tildagon would be inconsistent).

**Open questions — resolved 2026-05-19.**

1. **Flag naming.** Settled on `ImuTap` / `ImuMotion` — matches the `AnalyserBeatDetection` lower-case-after-prefix style. (Open question on file; resolved to recommendation.)
2. **`ImuFreeFall` worth declaring now?** **No.** Per Jason's sign-off, free-fall detection is dropped from the proposal entirely. No v1 Show consumes it; reserved-but-unwired flags add cognitive cost without near-term value. A later Epic can add it back if a Show that consumes it materialises.
3. **Sensitivity property: per-Show vs global.** Settled on per-Show, with a future Director-level global override that any Show can opt into reading. Per-Show ships in this Epic; global override deferred to a future Epic.
4. **Power profile entries for IMU.** Deferred to B4. Discrete-event surface (tap / motion-onset) covers the v1 use cases; continuous motion frames are speculative until a Show consumes them.
