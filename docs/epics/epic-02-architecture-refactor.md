---
title: "Epic 2: StickC architecture refactor (HAL, DAL, FX, transport, mode layers)"
status: cross-project (will move to umbrella repo when Tildagon work begins)
notion_url: https://www.notion.so/358bd06774058156ac2adcf079243661
notion_id: 358bd06774058156ac2adcf079243661
notion_status: Proposed
complexity: 13+ (must be split into Features before dispatch)
last_synced: 2026-05-07
sync_direction: bidirectional
---

## Related Documents

- [NocturNation Architecture Specification](https://www.notion.so/357bd0677405800b891beab0f4e0a976) - particularly §2 (Architecture overview, including the HAL and DAL), §4.5 (Group ID semantics), §6 (Effects catalogue), §8 (Node operating modes and UI), §10.4 (Architectural prerequisites)

## Goal

Refactor the StickC firmware into the layered architecture defined in spec §2: a hardware abstraction layer (HAL) at the bottom decoupling firmware from M5Unified and the M5StickC, a device abstraction layer (DAL) above the per-protocol drivers presenting a uniform `fire_event(target, ...)` interface backed by device-type profiles and an active-device registry, and the FX, mode state machine, and per-protocol driver concerns separated from the existing monolithic prototype. The result is that subsequent Epics (ESP-NOW, DMX, UI) plug into clean interfaces rather than touching beat detection or hardware code, that future host boards (Tildagon, generic ESP32, eventually different microcontroller families) can target the firmware without bespoke board-detection code paths, and that future device types (NocturNation custom, alternate PixMob revisions, DMX fixtures) can land via new DAL profile entries plus driver implementations.

## Business Value

This is the architectural unlock for the entire project. Without this refactor:

- Epic 4 (ESP-NOW) has no transport-driver abstraction to plug into - it would have to bolt onto the existing IR-only code path.
- Epic 5 (Tildagon receiver) would have to invent its own architecture rather than mirroring StickC's.
- Epic 7 (DMX) would have to re-implement everything for a third transport.
- Effect tuning becomes a pile of `if mode == X` branches rather than swappable Effect objects.
- Without a DAL, every effect call site has to know which protocol the target speaks. Adding new device types means scattering protocol logic across the codebase. The DAL converts that to one extension point per type: a JSON profile entry and (if the protocol is new) a driver.
- Without a HAL, every supported host requires its own bespoke firmware fork or fragile runtime board detection. This was confirmed in Epic 1 when a Wokwi simulator experiment refused to initialise M5Unified on a generic ESP32 DevKit; running the same firmware on a Tildagon or any non-StickC board would face the same problem.

Getting this right once means everything downstream is cheap. Getting it wrong means everything downstream is expensive.

The refactor is purposefully behaviour-preserving: at the end of this Epic, the StickC behaves identically to Epic 1's baseline. The change is entirely internal.

## Scope

**Included:**

- Hardware abstraction layer (HAL) per spec §2 (revised) and §10.4: vendor-independent primitives for mic, buttons, display, IR LED, GPIO, battery, clock. **Each HAL backend declares a flat capability set** (`mic`, `fft`, `ir-tx`, `esp-now`, `display`, `buttons-2x`, `imu`, ...) describing what the host can offer; the DAL queries this list to compose the host's device profile dynamically. Single concrete implementation in this Epic (M5StickC Plus2); the interface must be designed to also accept the planned Tildagon and generic-ESP32 implementations without further refactor.
- Device abstraction layer (DAL) per spec §2 (revised) and §10.4: device-type profile schema (JSON: input + output capabilities, params, fallbacks); active-device registry (named instances bound to a profile and a group ID); bidirectional API - `fire_event(target, ...)` for outputs, `subscribe_events(target, ...)` for inputs; silent-failure semantics on unsupported capabilities. The host itself is registered as a device: the `NocturNationStickC` profile declares input capabilities (mic+FFT emitting raw spectrum frames at a fixed cadence, buttons emitting `ButtonPress` events) and output capabilities (display, IR LED transport). The mic capability does **raw FFT only** - it emits band-summary energies (bass / mid / treble + RMS) and lets orchestration handle beat detection. Initial profile for `PixMobX4Gen3_1` declares its full IR-encoder capability set.
- FX abstraction layer (Effect base class, Pulse/Probability Pulse/Rainbow/Starlight as Effect subclasses) per spec §6
- Per-protocol drivers living inside the DAL (`PixMobIRDriver` as the only concrete driver in this Epic; ESP-NOW driver is Epic 4, DMX driver is Epic 7). The Driver base interface must be designed to accept all three without further refactor.
- Mode state machine per spec §8.1-8.2 (boot countdown, Slave/Master/Test/Config/Idle modes)
- Group ID handling per spec §4.5 (group 0=broadcast, 1-3=auto, 4-31=specialist) at the DAL level: active devices are bound to a group at registration time, and `fire_event` resolves group targets to their concrete devices.
- Behaviour-preserving: same audio analysis, same beat detection, same envelope, same default Pulse effect

**Explicitly excluded:**

- ESP-NOW transmission or reception (that's Epic 4)
- New UI screens, menus, or buttons (that's Epic 3)
- DMX integration (that's Epic 7)
- Any change to audio analysis tuning or effect parameters
- Any change to the IR encoder bytes (the wire protocol must stay identical to verify the refactor is non-regressive)

## Acceptance Criteria

- [ ] HAL interface defined per spec §2 with a single concrete implementation for M5StickC Plus2; firmware code outside the HAL contains zero direct references to `M5Unified`, `M5.`, board-specific GPIO numbers, or other vendor identifiers (verified by grep)
- [x] HAL backend exposes a capability-declaration API; the M5StickC Plus2 backend correctly declares its capability set (currently: `Display`, `Buttons`, `IMU`, `Battery`, `Mic`, `IRTx`); orchestration startup queries the DAL for available capabilities and adapts UI/feature flags to the result (idle UI footer surfaces live counts: `DAL: 2 dev, 6 cap, 2 drv`)
- [x] HAL interface designed such that adding a Tildagon or generic-ESP32 backend requires only a new HAL implementation, with no changes elsewhere in the firmware
- [x] DAL interface defined per spec §2 with C++ profiles for `PixMobX4Gen3_1` (output capabilities) and `NocturNationStickC` (composed at boot from the StickC HAL's declared capabilities; currently: AudioFrame + ButtonPress as inputs, DisplayShowText/Clear/FillRect/Meter as outputs); an active-device registry binds named instances to a profile + group ID; `fire_event(target, ...)` dispatches outputs via the right driver or fails silently; `subscribe_events(target, ...)` exposes inputs to orchestration. (JSON-loaded profiles deferred per Epic 2 design decision; revisit when there's a real "users add device types without recompiling" use case.)
- [x] DAL silent-failure semantics verified: requesting a capability not declared in a target's profile (e.g. RGBW on a PixMob profile that only supports RGB) produces no IR output and no exception
- [x] Mic capability emits raw FFT spectrum frames (band-summary energies: bass / mid / treble + RMS) at a fixed cadence through the DAL's `subscribe_events` API; the mic capability does **not** perform beat detection
- [x] Beat detection (baseline flux, threshold, refractory window, BPM tracking via inter-beat-interval median) lives in the orchestration layer, consumes spectrum frames from the DAL, and produces the same Vengaboys BPM tracking and beat-locked firing as the Epic 1 prototype
- [x] Button presses on the StickC are delivered as `ButtonPress` events through the DAL
- [ ] Adding a new device type requires only a new JSON profile entry (and a new driver if its protocol is new), with no changes to orchestration or effects (verified by registering a stub `TestDevice` profile and confirming `fire_event` against it works without orchestration code changes) — mechanism present and exercised by the `all-pixmobs` registration; explicit `TestDevice` verification deferred.
- [ ] Effect class hierarchy implemented per spec §6, with Pulse, Probability Pulse, Rainbow, and Starlight as concrete subclasses — not started.
- [x] Per-protocol drivers (`PixMobIRDriver` in this Epic; ESP-NOW + DMX in later Epics) plug into the DAL via a stable Driver base interface; `PixMobIRDriver` is the only concrete driver shipped in Epic 2
- [ ] Mode state machine implemented per spec §8.1-8.2, with Slave as the default boot mode — not started.
- [x] StickC produces byte-identical IR output to the Epic 1 baseline (verified by behaviour preservation through hardware tests on the Vengaboys reference track plus the four `test_pixmob_parity` reference vectors; no logic analyser capture taken)
- [x] Same audio behaviour: same Vengaboys BPM detection, same beat-locked firing
- [x] Code is now ready to accept ESP-NOW (Epic 4) and DMX (Epic 7) drivers without further refactor (Driver base class with `send` overloads + `start_audio_input` / `stop_audio_input` lifecycle hooks; ESP-NOW + DMX HAL interfaces stubbed; new drivers slot in via `register_driver`)
- [ ] HAL interface defined per spec §2 with a single concrete implementation for M5StickC Plus2; firmware code outside the HAL contains zero direct references to `M5Unified`, `M5.`, board-specific GPIO numbers, or other vendor identifiers (verified by grep) — substantially done. main.cpp retains 4 `M5.*` references all justified: `M5.config()` and `M5.begin()` for framework startup, `M5.update()` because HAL Buttons polling depends on it, `M5.Power.getBatteryLevel()` because there's no DAL Battery capability yet. Closing this AC fully needs a Battery DAL capability + a tiny refactor of the framework startup.

## Features

Link child Feature records here using the Parent task / Sub-task relation. Anticipated Features (Complexity 13+ requires split before dispatch):

- Feature 2.0: Hardware abstraction layer (HAL interface + M5StickC Plus2 backend, with the interface designed to also accept Tildagon and generic-ESP32 backends in later Epics)
- Feature 2.1: Device abstraction layer (device-type profile schema + active-device registry + `fire_event` dispatcher with capability check + silent-failure semantics; initial profiles for `PixMobX4Gen3_1` and a `NocturNationStickC` placeholder)
- Feature 2.2: Effect abstraction layer (Effect base class + Pulse + Probability Pulse + Starlight + Rainbow)
- Feature 2.3: Per-protocol drivers, living inside the DAL (Driver base interface + `PixMobIRDriver` as the only concrete driver in this Epic)
- Feature 2.4: Mode state machine (boot flow + Slave/Master/Test/Config/Idle modes per spec §8)
- Feature 2.5: Group ID handling (assignment, persistence, IR translation; lives at the DAL level)
- Feature 2.6: Refactor verification harness (regression test against Epic 1 baseline)

Isambard to decompose Features into Tasks (Complexity ≤5 each) before dispatch.

## Dependencies

| Dependency | Type | Status | Owner |
| --- | --- | --- | --- |
| Epic 1 (parity baseline) | Internal | Done | Jason |
| Architecture spec v0.16+ (§2, §6, §8, §4.5) | Internal | Done | Jason |
| Logic analyser or IR capture for verification | External | Available (M5StickC has IR receiver capability) | Jason |

## Target Sprint Range

- **Start sprint:** After Epic 1 Done
- **End sprint:** 2-3 sprints later (substantial Epic)
- **Indicative complexity total:** 25-30 points across child Features

## Status Notes

Proposed 2026-05-06 from spec v0.16 epic decomposition. **Complexity 13+ flagged - this Epic must be decomposed into Features by Isambard before any Task dispatch.**

The behaviour-preserving constraint is the key risk: it's tempting during a refactor to also "improve" things in passing. Resist this. Any improvement that's not strictly necessary for the architecture should be filed as a separate Task for after this Epic ships. The verification harness in Feature 2.6 exists specifically to make non-regressions provable rather than hopeful.

**Updated 2026-05-07:** Hardware abstraction layer (HAL) added as a top-level Epic 2 deliverable (Feature 2.0) following an Epic 1 finding. A Wokwi simulator experiment during Epic 1 surfaced M5Unified's tight coupling to the M5StickC: the existing firmware cannot run on any non-StickC ESP32 board without code-level changes. The HAL is intended to fix that and is now a precondition for any of spec §10.2's medium-term roadmap items to claim vendor-neutrality. Complexity stays 13+; if anything the addition strengthens the case for splitting into Features before dispatch. Spec §2 has been revised to show the HAL in the architecture diagram, with supporting rationale in spec §10.4.

**Updated 2026-05-07 (later):** Device abstraction layer (DAL) added as Feature 2.1 following further design conversation. The DAL provides a uniform `fire_event(target, ...)` interface above the per-protocol drivers, backed by a JSON device-type profile catalogue (capabilities, params, fallbacks) and an active-device registry (profile + group ID; group 0 wildcards all devices of that type). Capability negotiation is built in: each device type's profile declares what it can do, and `fire_event` either dispatches via the appropriate driver or fails silently when the requested capability isn't supported. Substitution policy is the show file composer's responsibility, not the DAL's. Spec §2 has been revised to show the DAL alongside the Event sources column, with a fuller layer-responsibilities entry; spec §10.4 has been updated to list the DAL as the second top-level Epic 2 abstraction. Features have been renumbered: 2.0 HAL, 2.1 DAL, 2.2 Effect abstraction, 2.3 Per-protocol drivers (now living inside the DAL), 2.4 Mode state machine, 2.5 Group ID handling (now at DAL level), 2.6 Verification harness. Complexity is now well above 13+; this Epic must be split into Sub-Epics or carefully sequenced Features at planning time.

**Progress snapshot 2026-05-07 (Hello World milestone, commits `43512d9` HAL contract, `29af6b8` DAL contract, `1d1082b` DAL Hello World):** Architectural infrastructure for Epic 2 is in place and verified.

[Original snapshot retained in git history; superseded by the snapshot below.]

**Progress snapshot 2026-05-07 (substantial-completion milestone, commits `c5af654` StickC HAL backends, `010dd71` LocalDriver wire-up, `0272552` UI + buttons migration, `691a1f6` Mic migration, `0291dcc` IRTx + PixMobIRDriver migration):** the architectural refactor is substantially complete. Every active path through the firmware now flows through HAL → DAL → orchestration:

- **Display** rendering: `main.cpp` calls `DAL::fire_display_*` → `LocalDriver` → `hal::Display` → `M5.Display`.
- **Buttons**: `M5.BtnA/B/PWR` → `ButtonsStickC` → `LocalDriver` bridge → `DAL::deliver_button_press` → orchestration callback.
- **Mic + beat detection**: `M5.Mic` → `MicStickC` (FFT, band sums) → `LocalDriver` → `DAL::deliver_audio_frame` → orchestration callback (which holds all the flux / threshold / BPM state and runs the visible beat response).
- **IR send**: `DAL::fire_rgb_pulse("all-pixmobs", ev)` → `PixMobIRDriver` → `pixmob::buildSingleColor` → `hal::IRTx` (owns the only `IRsend(GPIO 19)`).

The StickC HAL backend now declares 6 real capabilities (`Display`, `Buttons`, `IMU`, `Battery`, `Mic`, `IRTx`). The DAL registers two devices at boot (`local`, `all-pixmobs`) and two drivers (`LocalDriver`, `PixMobIRDriver`); the idle-UI footer surfaces this live (`DAL: 2 dev, 6 cap, 2 drv`).

What's left in Epic 2:

- **Effect class hierarchy** (Pulse / Probability Pulse / Rainbow / Starlight as `Effect` subclasses per spec §6) - not started.
- **Mode state machine** (Slave / Master / Test / Config / Idle per spec §8.1-8.2) - not started; there's a single beat-mode boolean today.
- **`TestDevice` AC verification** - the mechanism is exercised by the `all-pixmobs` registration but the explicit "stub TestDevice profile + confirm `fire_event` works without orchestration changes" verification step hasn't been done.
- **Battery DAL capability** - small gap; main.cpp still calls `M5.Power.getBatteryLevel()` directly because there's no DAL counterpart yet. Adding a `BatteryStatus` capability + DAL helper would close the last hold-out reference.
- **Constellation / setup helpers** - `assignBraceletToGroup`, `sendColourToGroup`, `smoothHueCycle`, `starlight` are wrapped in `#if 0` in main.cpp pending an `AssignDeviceGroup` DAL capability and dynamic group addressing. They were already inactive code paths in Epic 1; not regressing anything by leaving them disabled.

`main.cpp` retains 4 `M5.*` references in the firmware: `M5.config()` and `M5.begin()` (top-level framework startup), `M5.update()` (HAL Buttons polling depends on it, called once per loop), `M5.Power.getBatteryLevel()` (no DAL Battery capability yet). All other `M5.*` and the global `IRsend` are gone.

Recommended next migration order to close out Epic 2: **mode state machine** (gives the firmware proper Slave/Master/etc. modes that subsequent transports plug into), then **FX class hierarchy** (each existing colour-pulse path becomes a concrete `Effect` subclass), then **Battery DAL capability** (closes the last grep-clean AC), then the **`TestDevice` verification** as a one-evening exercise. Constellation helper revival can wait for the constellation work itself.
