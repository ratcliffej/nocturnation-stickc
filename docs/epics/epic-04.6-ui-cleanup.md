---
title: "Epic 4.6: M5 firmware UI cleanup pass"
status: Proposed
notion_url: https://www.notion.so/35cbd067740581e4ba55f79eb168ec9d
notion_id: 35cbd067740581e4ba55f79eb168ec9d
notion_status: Proposed
last_synced: 2026-05-10
sync_direction: bidirectional
---

## Related Documents

- [NocturNation Architecture Specification](https://www.notion.so/357bd0677405800b891beab0f4e0a976) - particularly §7 (Display surfaces) and §8 (Node operating modes and UI)
- [Brand and visual identity](https://www.notion.so/358bd0677405811b8eb7eaa3c80e2a06) - matte black `#0A0A0A`, indicator amber `#FFAA00`, Inter sans-serif
- [Epic 4.5: Sub-band adaptive-threshold beat detection](https://www.notion.so/35bbd0677405816fa9fbd0306100c794) - upstream Epic; UI cleanup happens after 4.5 ships
- [Epic 5: Tildagon receiver app](https://www.notion.so/358bd067740581b19551d158d658df76) - downstream Epic; the M5 UI is the design reference Tildagon's own UI builds on (conceptually, not literally - Epic 5 is a fresh codebase)

## Goal

A focused cleanup pass on the M5 firmware UI screens to address accumulated cruft from Epics 1-4. The aim is **consistent, brand-aligned, professional-feeling** UI across every screen the operator sees, before Epic 5 begins fresh-coding the Tildagon receiver app. Tildagon will benefit from a cleaner reference to design against; the M5 firmware will be more defensible as the canonical reference deployment for hackspace gigs and the Coldplay tribute act.

Another important part of this epic is to perform an achitecture review and ensure we're using abstraction and encapsulation concepts properly. There should be clean methods to call effects by group with appropriate fallbacks in place for heardware limitations where appropriate.

## Why this Epic exists

Four Epics of additive work (HAL, FX abstraction, mode state machine, ESP-NOW transport with RSSI display) means UI screens accumulated organically: each new screen was added when the feature behind it shipped, with whatever visual treatment felt right at the time. The result is the typical pattern after fast-moving development - the firmware works, but the UI feels like it was assembled in layers rather than designed.

This is not a feature Epic. No new capability ships. The work is purely about making what's already there look and feel coherent. That makes it easy to scope, easy to test (by eye), and easy to bound - if it starts expanding into "and while we're here let's add X", the scope is wrong.

Doing this **before** Epic 5 (Tildagon) matters for two reasons:

1. The Tildagon app will be built fresh, and having a polished M5 UI as the design reference means Tildagon doesn't inherit cruft.
2. The Tildagon app submission to the EMF app store has external review - any visual sloppiness in the project's reference hardware reflects badly on the brand the Tildagon app represents.

## Operational model

Laptop-driven, same as Epics 1-4. UI work is well-suited to native rendering tests where possible (unit tests for layout calculations, deterministic snapshot tests for screen content) plus hardware verification on both Plus2 and S3 for visual fidelity.

Verification ownership: **(L)** = laptop / native test, **(B)** = build-time check, **(H)** = hardware verification by Jason (the only reliable test for "does it look right?" is looking at it).

## Scope

**Included:**

- **Brand-alignment audit**: every screen reviewed against the Brand and visual identity page. Matte black `#0A0A0A` background, indicator amber `#FFAA00` accent, Inter sans-serif (or closest available bitmap font given the StickC's display constraints).
- **Visual consistency pass**: standardise text sizes, line spacing, padding, button-prompt placement, status-indicator positioning across all screens.
- **Status display unification**: battery icon and RSSI signal indicator (introduced in Epic 4) styled consistently as a coherent status bar. Same icon family, same proportions, same position. Reads as one component, not two unrelated ones.
- **Mode-transition polish**: boot countdown screen, mode-selection menu, Test Mode entry, Config tree navigation - all reviewed end-to-end as a single experience.
- **Edge-case handling**: explicit treatment for no-signal, low-battery, master-heartbeat-loss, audio-silence states. Each should show *something* sensible rather than just a frozen previous state.
- **Diagnostic-mode separation**: any debug readouts from development isolated to a single Diagnostic screen (reachable from Test Mode per spec §8.5) rather than scattered across normal-operation screens.
- **Removal of orphaned screens**: any screen that's no longer reachable, or reachable but no longer functional, gets either removed or restored.
- **Button-prompt consistency**: same conventions across all screens for indicating what each button does (e.g., always "A: Select   B: Back   PWR: Menu" in the same position with the same style).
- **Cross-device parity**: the same screens render visually equivalently on Plus2 and S3. Hardware differences (slightly different display drivers, slightly different colour reproduction) shouldn't produce visibly different UI.

**Explicitly excluded:**

- New features - if it's not already on a screen in the firmware, it's not appearing on a screen in this Epic.
- Display HAL changes - rendering primitives stay the same; only what's drawn through them changes.
- Tildagon UI work - that's Epic 5.
- M5Stack Atom Lite UI (single LED) - out of scope; no display, nothing to clean up.
- Animation or motion design - the StickC's display refresh rate makes elaborate animation more trouble than it's worth. Static layouts done well are better than animated layouts done poorly.
- Localisation / i18n - English (UK) only.
- Display abstraction layer (§7.5 of spec) - that's a future Epic; this one stays platform-specific.
- Brand-asset additions (favicon, app store screenshots, etc.) - those belong with the relevant downstream Epics.

## Acceptance Criteria

- [ ] **(B)** Code builds cleanly under both `[env:m5stick-plus2]` and `[env:m5stick-s3]` PlatformIO environments.
- [ ] **(L)** Native tests for any layout calculation functions still pass.
- [ ] **(H)** Every screen in the firmware reviewed against the Brand and visual identity page; deviations either justified (with a note) or corrected.
- [ ] **(H)** Battery and RSSI indicators styled consistently as a unified status bar; both icons use the same family and size, positioned together at a fixed location.
- [ ] **(H)** Boot → mode menu → active mode flow walked through end-to-end on Plus2 and on S3; both feel polished and consistent.
- [ ] **(H)** Test Mode and Config tree fully navigated; every leaf screen reviewed.
- [ ] **(H)** Edge cases tested: device boots with no master heartbeat available; device's battery drops below 20%; device's RSSI drops to no-signal mid-show. Each produces a sensible visible state, not a frozen previous state or a glitchy redraw.
- [ ] **(H)** Diagnostic readouts (FFT spectrum, IBI history, frame counters, etc.) are reachable from Test Mode but not visible during normal operation.
- [ ] **(H)** Plus2 and S3 placed side-by-side running the same firmware: visible UI is equivalent. Any differences are colour-reproduction artefacts of the displays themselves, not firmware-level differences.
- [ ] **(H)** No regression on existing functionality: the Coldplay tribute act's existing show still works correctly (this is the canonical "didn't break anything" test).
- [ ] **(H)** A short walkthrough video (phone-recorded, not produced) exists showing the cleaned-up UI flow on both Plus2 and S3, captured for the open-source repo's README and any future demo materials.

## Next blocks of work

### Block 1: UI inventory and audit

Before changing anything, list every screen the firmware currently renders. For each: where it's reached from, what it shows, how it interacts with buttons. Capture the current state with photos so the before/after is documentable. Walk through each against the Brand and visual identity page; mark deviations.

- Enumerate every screen reachable from boot (boot countdown, mode menu, Slave Mode active, Master Mode active, Config tree leaves, Test Mode tests, Idle).
- Photograph each screen on both Plus2 and S3.
- Create a checklist mapping each screen to its brand-alignment status (compliant / minor deviations / major deviations).
- Identify orphaned or dead screens.
- Identify screens that don't handle their edge cases well.
- Commit: "UI inventory and brand-alignment audit"

### Block 2: Status-bar unification

The battery icon (existing since Epic 1-2) and the RSSI signal indicator (introduced Epic 4) likely don't currently feel like a coherent component. Treat them as the single highest-value cleanup target because they appear on most screens and inconsistency here is felt everywhere.

- Define a single Status Bar component: battery icon, RSSI icon, source ID indicator, mode glyph, all rendered in a single horizontal strip at a consistent position (top of screen recommended).
- Standardise icon family: same artist style, same proportions, same colour treatment (cream for normal, amber for warning, red for critical).
- Apply across all screens that need it.
- Commit: "Unified status bar across all screens"

### Block 3: Mode-flow polish

Walk the boot → mode-selection → active-mode flow as a single connected experience and polish it as a unit.

- Boot countdown screen: brand-aligned wordmark, indicator amber for the countdown number, clean transition to mode menu.
- Mode-selection menu: consistent layout, clear button prompts, last-used mode highlighted as default per spec §8.1.
- Active-mode entry: smooth transition from menu to the chosen mode's main screen.
- Master/Slave/Test/Config/Idle entry screens all visually consistent with one another.
- Commit: "Boot-to-active mode flow polished end-to-end"

### Block 4: Edge-case treatment

Walk every edge case and ensure each produces a sensible visible state.

- No master heartbeat (Slave Mode): subtle hue cycle on display + clear "no signal" indication.
- Master heartbeat lost mid-show: graceful transition into the no-signal state.
- Low battery: warning indicator in status bar; below 10%, persistent visible warning.
- Audio silence (Master Mode): treat as valid state per spec §8.2; UI shows "listening" rather than implying a fault.
- Failed IR transmission: not visible in normal use but reflected in Diagnostic mode counters.
- ESP-NOW frame received but malformed: silently dropped per protocol; counter incremented in Diagnostic mode.
- Commit: "Edge-case visible states unified and polished"

### Block 5: Diagnostic-mode separation

Isolate development-era debug readouts to a single Diagnostic screen reachable only from Test Mode.

- Audit normal-operation screens for debug values that should be hidden.
- Build a single Diagnostic screen consolidating: FFT spectrum (live), IBI history, ESP-NOW frame counters per source ID, last RSSI per source ID, beat-detection threshold values.
- Reachable from Test Mode menu per spec §8.5.
- Commit: "Diagnostic-mode consolidation"

### Block 6: Cross-device parity verification

Final pass: Plus2 and S3 side by side, walking through every screen, verifying visible equivalence.

- Side-by-side comparison: every screen on both devices.
- Document any unavoidable differences (colour reproduction, refresh-rate artefacts) for the brand page's known-differences note.
- Phone-recorded walkthrough video for the README and future demo material.
- Commit: "Cross-device parity verified; walkthrough recorded"

## Dependencies

| Dependency | Type | Status | Owner |
|---|---|---|---|
| Epic 4 (ESP-NOW transport) | Internal | Done | Jason |
| Epic 4.5 (sub-band beat detection) | Internal | In Progress | Jason |
| Brand and visual identity page | Internal | Done (v0.3) | Jason |
| Plus2 and S3 hardware on desk | External | Available | Jason |

## Status Notes

Proposed 2026-05-09 to slot between Epic 4.5 (beat detection, in flight) and Epic 5 (Tildagon fresh codebase). Walk-before-run priority: get the M5 firmware UI consistent and brand-aligned before starting fresh work on the Tildagon receiver app.

The Epic is intentionally narrow. No new features, no new capabilities, no new abstractions. Just a focused cleanup pass on what's already there. If during execution it looks like the work wants to expand ("and while we're here let's add X"), the scope is wrong - capture X as a future Epic and stay disciplined about this one.

Processing Type stays Hybrid because layout work is well-suited to laptop-driven coding (with visual tests where they apply), but hardware verification on both Plus2 and S3 is genuinely the only reliable test for "does it look right?" - that's manual.

This Epic was not in the original roadmap (1-9). It's a quality-of-foundation pass earned by Epics 1-4 having shipped enough for cruft to have accumulated. Worth recording as such.
