---
title: "Epic 4.6: Clean architecture and UI polish"
status: In Progress
notion_url: https://www.notion.so/35cbd067740581e4ba55f79eb168ec9d
notion_id: 35cbd067740581e4ba55f79eb168ec9d
notion_status: In Progress
last_synced: 2026-05-10
sync_direction: bidirectional
---

## Related Documents

- [NocturNation Architecture Specification](https://www.notion.so/357bd0677405800b891beab0f4e0a976) - particularly §5 (audio analyser surface), §7 (Display surfaces) and §8 (Node operating modes and UI)
- [Brand and visual identity](https://www.notion.so/358bd0677405811b8eb7eaa3c80e2a06) - matte black `#0A0A0A`, indicator amber `#FFAA00`, Inter sans-serif
- [Epic 4.5: Sub-band adaptive-threshold beat detection](https://www.notion.so/35bbd0677405816fa9fbd0306100c794) - upstream Epic; analyser surface this Epic builds on
- [Epic 4.7: Dynamic show from FFT](https://www.notion.so/35bbd0677405816fa9fbd0306100c795) - downstream Epic; lands new visualisations on the plug-in surface this Epic establishes
- [Epic 5: Tildagon receiver app](https://www.notion.so/358bd067740581b19551d158d658df76) - downstream Epic; the architecture defined here is the contract Tildagon ports against (conceptually, not literally - Epic 5 is a fresh codebase)

## Goal

Two-stream Epic, both running before Epic 4.7 and Epic 5:

1. **Clean architecture review.** Refactor the firmware to a properly composable plug-in architecture so future visualisations (Epic 4.7) and future hosts (Tildagon, Epic 5) bolt on without surgery in core code. The structural finds from the architect's review:
   - Lift `EspNowBroadcaster` from a per-mode helper to a proper Driver behind the canonical `render_fx` send.
   - Define a `Visualisation` plug-in contract with a property bag, power profile, and capability gating, so master-side shows are pluggable.
   - Define a sibling `OutputBinding` contract for slave-side rendering (local LCD, PixMob IR, future DMX/BLE) so PixMob stops being special-cased and becomes "just another binding".
   - Replace physical-button events with semantic `InputAction` events, so the same vis code runs on a 2-button StickC and a 6-button Tildagon.
   - Gate the analyser pipeline on declared consumer demand so Plus2 stops doing spectrum-FFT work nobody is reading.

2. **UI polish.** A focused cleanup pass on the screens the operator sees, addressing accumulated cruft from Epics 1-4. Splash, slave-mode at-a-glance status, NO SIGNAL hierarchy, menu footer language, plus the pause-toggle pulse bug.

The architecture stream lands first because the UI work touches the same files. Doing it twice would burn time.

## Why this Epic exists

Five Epics of additive work means architectural debt has accumulated in two specific places: master-side visualisation logic is hardcoded into `AutonomousMasterMode`, and slave-side rendering treats PixMob IR forwarding as a special case rather than an output binding. Both block the next Epics:

- **Epic 4.7** is "dynamic shows from FFT" - multiple visualisations selectable at runtime. Without a plug-in surface, every new vis lands as another patch to a 3447-line `mode_machine.cpp`.
- **Epic 5** is the Tildagon port - 6 buttons, different display, MicroPython. Without input abstraction and an output-binding surface, the port has nothing clean to slot into.

Doing the architecture work *now*, with one shipping vis (beat-pulse) and one slave deployment (PixMob over IR), is the cheapest moment to define the contracts. Every additional vis or binding makes the refactor more expensive.

The UI polish piggybacks because we're already in the relevant files.

## Operational model

Laptop-driven, same as Epics 1-4.5. Native test envs catch regressions on the architecture surface; hardware verification on Plus2 + S3 catches behavioural drift on the IR wire and visual regressions in the UI.

Verification ownership: **(L)** = laptop / native test, **(B)** = build-time check, **(H)** = hardware verification by Jason.

## Scope

**Included:**

- **Plug-in base abstraction**: shared `Plugin` base providing id, display name, capability requirements, property bag (typed key-value with NVS persistence under `noct/<plugin-kind>/<id>/<key>`), and power profile declaration.
- **Visualisation surface**: `Visualisation` interface + `VisualisationContext` (services: `render_fx`, property accessors, analyser capability query, paused state, time) + `VisualisationRegistry`. Vis declares power profile (which analyser surfaces it consumes) so the framework can gate the audio pipeline.
- **OutputBinding surface**: `OutputBinding` interface + `OutputBindingRegistry`. Each binding takes a `RenderEvent` and turns it into a hardware action. Initial bindings: `LocalDisplayBinding` (LCD pulse) and `PixMobIrBinding` (IR + PixMob protocol with group filter). Slaves run any combination simultaneously.
- **Input abstraction**: `InputAction` enum (Picker, Settings, Back, Confirm, Cycle, CyclePrev, Pause, AuxA, AuxB) with per-host mapping in HAL. Visualisations and the framework UI consume `InputAction`; physical button mappings are the host's concern.
- **EspNowBroadcaster as Driver**: lifted from `mode_machine.cpp` helper struct to `src/dal/drivers/espnow_broadcaster.cpp`, registered with DAL, called via `render_fx("esp-now-broadcast", ev)`. Radio lifecycle stops being a per-mode concern.
- **Pipeline gating**: analyser computation (spectrum FFT, 8-band summary) skipped when no active consumer declares need for it. Driven by visualisation power profile + binding declarations.
- **Master-side beat-pulse migration**: existing single-colour beat-pulse becomes `BeatPulseVisualisation` with `primary_colour` as a property persisted via the bag. Wire output byte-identical to current behaviour (PixMob byte-parity tests are the regression net).
- **Slave-side migration**: existing slave display-as-light becomes `LocalDisplayBinding`; existing PixMob IR forward becomes `PixMobIrBinding`. Existing NVS keys (`slv_ir_grp`, `ir_en`, `slv_repeat`) migrated one-shot to per-binding namespace at boot.
- **Spectrum Bars visualisation**: new vis consuming `SpectrumFrameEvent`, rendering 32-band bars on master LCD, with a manual band-fire trigger usable as a sound-check tool (lights up the orphan event).
- **Analyser micro-opts**: hoist transcendental constants in `compute_spectrum_frame()`, precompute bin→Hz LUT, single-pass Welford variance in BeatDetector. Plus2 CPU savings.
- **Vis picker UI**: Btn2-long opens a vis selector overlay listing registered visualisations (greyed where capability requirements unmet). Selection persisted to NVS (`noct/active_vis`).
- **Vis settings UI**: Btn1-long opens settings for the active vis. Auto-generated from `properties()` schema unless the vis overrides `render_settings_ui()`. Spectrum Bars uses the auto-generated UI.
- **UI polish**: splash countdown 5s→3s + version/battery + yellow countdown digit; slave status pip (signal+battery) overlaid on full-screen pulse rect; NO SIGNAL screen reflow with proper hierarchy; standardised menu footer language; pause-toggle pulse-on-resume bug fix.
- **Future-capability enum comments**: `AnalyserMultiBandOnset`, `AnalyserSpectralCentroid`, `AnalyserEnergyEnvelope`, `AnalyserSectionDetection` get a comment block explaining they are reserved for Epic 4.7 and no host should declare them yet.
- **Master display is asymmetric to slave**: master does NOT auto-bind a `LocalDisplayBinding`. Whether the master LCD participates in the show is a per-visualisation choice. Beat Pulse uses the LCD pulse-rect (current behaviour); Spectrum Bars draws bars; a future "headless master" vis could leave the LCD as status-only.

**Explicitly excluded:**

- New audio analyser features (multi-band onset, spectral centroid, etc.) - that's Epic 4.7. The capability flags exist; the analysers don't.
- Tildagon HAL or Tildagon OutputBindings - Epic 5. Architecture is designed *with* Tildagon in mind; nothing Tildagon-specific is implemented here.
- True ESP32 light-sleep windows - power profile *declarations* land here so future vis can't be written assuming infinite CPU; the actual sleep-window scheduler is a focused follow-up Epic.
- Slave-side power optimisation (modem-sleep between heartbeats) - separate Epic.
- DMX, BLE, or other future output bindings - the surface is forward-compatible; no DMX/BLE shipped.
- Per-vis config UI override (custom `render_settings_ui()`) for the BeatPulse vis - auto-generated UI is sufficient.
- Display abstraction layer changes - rendering primitives stay as-is.
- Localisation / i18n - English (UK) only.

## Acceptance Criteria

- [ ] **(B)** Code builds cleanly under both `[env:m5stick-plus2]` and `[env:m5stick-s3]` PlatformIO environments.
- [ ] **(L)** All seven existing native test envs continue to pass (`native`, `native_dal`, `native_modes`, `native_effects`, `native_espnow`, `native_audio`, `native_analyser`).
- [ ] **(L)** New native test coverage for Plugin base (property bag persistence, capability gating, registry), Visualisation contract (registration, switching), OutputBinding contract (binding fan-out from render event), InputAction mapping.
- [ ] **(L)** PixMob byte-parity tests pass after BeatPulseVisualisation migration - wire output is byte-identical to pre-Epic behaviour.
- [ ] **(H)** Plus2 + S3 side-by-side: switching vis at runtime works on both; selected vis persists across reboot; capability-gated vis appear correctly enabled/disabled per host.
- [ ] **(H)** Slave with PixMob IR enabled: receives master broadcasts, displays locally AND forwards over IR to PixMobs in range. Disabling either binding in config disables only that output. Old NVS keys (`slv_ir_grp` etc.) migrate cleanly to new per-binding namespace.
- [ ] **(H)** Spectrum Bars vis renders the live 32-band spectrum on master LCD; manual band-fire trigger fires a beat into the configured band, visible on PixMobs in range.
- [ ] **(H)** Coldplay tribute regression: existing show still works correctly (canonical "didn't break anything" test).
- [ ] **(H)** UI polish verified: splash shows version + battery, 3s countdown, yellow digit; slave-mode status pip visible during full-screen colour rendering; NO SIGNAL screen has clear hierarchy; menu footers consistent across all screens; pause toggle does not fire a pulse on the way in.
- [ ] **(H)** Plus2 CPU headroom: with BeatPulse active, spectrum-FFT path is gated off; with Spectrum Bars active, it's gated on. Verifiable via timing instrumentation in the test envs.

## Blocks of work

Work proceeds in roughly this order. Earlier blocks enable later ones; UI polish lands last so it's not done twice.

### Block 1: Split mode_machine.cpp per mode

`src/modes/mode_machine.cpp` is 3447 lines containing six mode classes plus EspNowBroadcaster plus helpers. Split into one file per mode (`boot_mode.cpp`, `menu_mode.cpp`, `autonomous_master_mode.cpp`, `slave_mode.cpp`, `config_mode.cpp`, `test_mode.cpp`) with a small `mode_machine.cpp` keeping the FSM facade. Pure structural change - no behaviour change. Native tests catch any regression.

- Commit: "Split mode_machine.cpp per concrete mode"

### Block 2: Lift EspNowBroadcaster to a Driver

Move `EspNowBroadcaster` from anonymous namespace in `mode_machine.cpp` to `src/dal/drivers/espnow_broadcaster.{h,cpp}`. Register with DAL. Modes that previously called `broadcaster_.send_*` now call `dal::render_fx("esp-now-broadcast", ev)`. Radio lifecycle managed by the Driver, not the modes.

- Commit: "Promote EspNowBroadcaster to a Driver behind render_fx"

### Block 3: Plugin base (PropertyBag + PowerProfile + Registry mechanics)

Introduce a small `Plugin` base class providing the shared mechanics that both `Visualisation` and `OutputBinding` need:
- `id()`, `display_name()`, `required_capabilities()`
- `properties()` returning a `std::span<const PropertyDef>`
- `power()` returning a `PowerProfile`
- NVS namespace convention (`noct/<plugin-kind>/<id>/<key>`)
- Templated registry mechanics

Defines `PropertyDef`, `PropertyValue`, `PropertyType`, `PowerProfile`, `CapabilityMask`. Native tests for property bag round-trip + bounds clamping.

- Commit: "Plugin base abstraction (property bag, power profile, registry mechanics)"

### Block 4: Input abstraction

Add `InputAction` enum + `InputEvent` struct in HAL. HAL backends (Plus2 + S3) gain a button→action mapping (Btn1-short→Confirm, Btn2-short→Cycle, Btn1-long→Settings, Btn2-long→Picker, Btn1+Btn2-hold→Back, Btn1-double→Pause). DAL gains `subscribe_input_actions()`. Existing `subscribe_button_press` stays for now (modes mid-migration use it). Native tests cover the mapping.

- Commit: "InputAction abstraction with per-host mapping in HAL"

### Block 5: Visualisation surface

Define `Visualisation` interface + `VisualisationContext` (provides `render_fx(target, ev)`, `get/set property`, `analyser_caps()`, `paused()`, `now_ms()`, `since_enter_ms()`) + `VisualisationRegistry`. No concrete vis yet; just the contract and a stub vis used only for tests.

- Commit: "Visualisation interface, context, and registry"

### Block 6: OutputBinding surface

Define `OutputBinding` interface + `OutputBindingRegistry`. Each binding consumes `RenderEvent` and produces hardware action. Sibling to Visualisation; shares Plugin base.

- Commit: "OutputBinding interface and registry"

### Block 7: Pipeline gating

Mic frame backends (Plus2 + S3) query the active vis's `PowerProfile` to decide whether to compute spectrum FFT and 8-band summary. When all active consumers (vis + bindings) declare no need for spectrum, the analyser skips that path. Toggling vis at runtime flips the gating live. Native tests cover the gate.

- Commit: "Gate analyser pipeline on declared consumer demand"

### Block 8: Migrate BeatPulse to Visualisation

Extract the beat-driven single-colour pulse logic from `AutonomousMasterMode` into `src/visualisations/beat_pulse.cpp`. `primary_colour` becomes a property persisted via the bag. AutonomousMasterMode becomes a thin shell that holds the active vis pointer and forwards events. PixMob byte-parity test is the regression gate.

- Commit: "Migrate beat-pulse logic to BeatPulseVisualisation"

### Block 9: Migrate slave outputs to OutputBindings

Extract slave's display-as-light path into `src/output_bindings/local_display.cpp`. Extract slave's PixMob IR forward path into `src/output_bindings/pixmob_ir.cpp` (group as a property). `SlaveMode` becomes a thin shell that holds the active bindings list and fans render events out to all of them. NVS migration shim runs at boot: if `slv_ir_grp` etc. are present, copy their values to the new per-binding keys and clear the old ones.

- Commit: "Migrate slave outputs to OutputBindings (LocalDisplay + PixMobIr)"

### Block 10: Vis picker + settings UI

In `AutonomousMasterMode`: `InputAction::Picker` opens a vis-selector overlay listing registered visualisations (display name, current selection, capability-gated vis greyed). Selection persisted to NVS. `InputAction::Settings` opens a settings screen for the active vis - auto-generated from its property schema unless the vis overrides. Status strip on the active-vis screen gains a small label showing the active vis name (truncated).

- Commit: "Visualisation picker and auto-generated settings UI"

### Block 11: Spectrum Bars visualisation

New vis: `src/visualisations/spectrum_bars.cpp`. Subscribes to `SpectrumFrameEvent` (declared in its `PowerProfile`). Renders 32-band bars on master LCD. Properties: `band_focus` (Bass/Mid/Treble/All), `sensitivity` (U8). `InputAction::Confirm` fires a manual beat into the configured band - usable as a sound-check tool.

- Commit: "Spectrum Bars visualisation with manual band-fire"

### Block 12: Analyser micro-opts

Three small wins in the audio hot path, mostly Plus2-relevant:
- Hoist `std::pow` / `std::log` constants in `compute_spectrum_frame()` to one-shot init.
- Precompute bin→Hz LUT (member of analyser, populated in `begin()`).
- Welford single-pass variance in `BeatDetector` (combine the mean and variance loops).

Native tests already cover these; behavioural output unchanged.

- Commit: "Analyser micro-opts (constant hoist, bin→Hz LUT, Welford variance)"

### Block 13: UI polish

- Splash: countdown 5s→3s, version + battery in bottom-right at 6pt white, countdown digit yellow size-3.
- Slave: 38×12px status pip (signal dot + battery glyph) overlaid on full-screen pulse rect, always visible. NO SIGNAL screen reflowed: size-3 headline, size-1 diagnostics with proper vertical hierarchy.
- Menu: standardised footer language across all screens (`B: cycle  A: select  B-hold: back` for navigational; `B: cycle  A: act  B-hold: back` for interactive).
- Bug: AutonomousMaster pause-toggle no longer fires a pulse on the way into pause (only on the way out).

- Commit: "UI polish: splash, slave status pip, NO SIGNAL reflow, menu footers, pause bug"

### Block 14: Future-cap enum comments + close-out

- Add comment block to `Capability::AnalyserMultiBandOnset` etc. flagging they are reserved for Epic 4.7.
- Update `docs/architecture.md` §5 / §7 / §8 with the new plug-in surfaces.
- Update Notion mirror (architecture spec + this Epic doc).
- Update `memory/project_hal_dal_architecture.md` to reflect the post-Epic-4.6 state.
- Final commit + close.

- Commit: "Epic 4.6 close-out: documentation refresh"

## Behaviour preservation contract

Two things must stay locked across this Epic:

- **PixMob IR wire output**: the bytes on the IR wire from BeatPulseVisualisation must be identical to the bytes the current master emits. PixMob byte-parity tests are the regression net.
- **Heartbeat timing and skip-if-recent semantics**: 1 Hz with skip-if-recent stays exactly as today. Heartbeat lives in the mode wrapper, not the vis, so this should be free.

## Dependencies

| Dependency | Type | Status | Owner |
|---|---|---|---|
| Epic 4 (ESP-NOW transport) | Internal | Done | Jason |
| Epic 4.5 (sub-band beat detection) | Internal | Done | Jason |
| Brand and visual identity page | Internal | Done (v0.3) | Jason |
| Plus2 and S3 hardware on desk | External | Available | Jason |

## Status Notes

Originally proposed as a UI-cleanup-only Epic. Scope expanded 2026-05-10 after architect's review identified three load-bearing structural items that block Epic 4.7 (multiple visualisations) and Epic 5 (Tildagon port): EspNowBroadcaster needing Driver promotion, no Visualisation plug-in surface, and slave-side PixMob being special-cased rather than an OutputBinding.

The architecture stream lands first because the UI work touches the same files and would be redone otherwise. Tildagon-specific implementation is explicitly out of scope; the architecture is *designed with Tildagon in mind* (input abstraction supports its 6 buttons; OutputBinding supports its different display) but no Tildagon code lands here.

Total estimated effort: ~9-10 days of focused work across 14 blocks. Larger than 4.5 but lands a properly composable architecture that future Epics extend rather than patch.
