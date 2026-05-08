---
title: "Epic 3: StickC UI implementation per spec §8"
status: stickc-specific
notion_url: https://www.notion.so/358bd0677405817b8ba3dcd31a23bf50
notion_id: 358bd0677405817b8ba3dcd31a23bf50
notion_status: In progress
complexity: 8 - Large
last_synced: 2026-05-08
sync_direction: bidirectional
---

## Related Documents

- [NocturNation Architecture Specification](https://www.notion.so/357bd0677405800b891beab0f4e0a976) - particularly §8 (Node operating modes and UI), §8.4 (Config tree), §8.5 (Test mode), §8.6 (UI implementation per platform)

## Goal

Implement the boot countdown, mode-selection menu, Test Mode, and Config tree as specified in §8.1-8.6 of the architecture spec. After this Epic, the StickC behaves like a proper Nocturnation device - boots into Slave Mode by default, lets the operator choose other modes via the menu, exposes config for IR/ESP-NOW/Audio settings.

## Business Value

The v0.16 spec describes a richer device experience than the prototype's three-button "test/cycle/mode" hack. Without this Epic:

- Multi-StickC deployments require re-flashing firmware to switch a device between Master and Slave roles
- Group ID assignment for the constellation art piece has no UI path
- Operators have no visibility into what the device is doing (current mode, BPM, battery, network state)
- Test Mode (which the spec considers a primary feature for discoverability and play) doesn't exist

This Epic also produces UX learnings transferable to the Tildagon receiver (Epic 5). The mode-selection menu pattern, the config tree shape, the operator-feedback display - all of these are reused, just rendered differently on the round Tildagon LCD.

## Scope

**Included:**

- Boot flow with 5-second countdown to Slave Mode (§8.1)
- Mode-selection menu with Slave / Master / Test / Config options (§8.3)
- Test Mode with all six sub-tests per §8.5 (Pulse, Fade, Rainbow, Sparkle, White Out, Group Targeting)
- Config tree per §8.4 (Audio, IR, ESP-NOW, WiFi, DMX/Art-Net, System)
- Status display during runtime: current mode, BPM, battery, IR fires, network heartbeat
- StickC button mapping per §8.6 (Btn-A=select, Btn-B=cycle, BtnPWR=back/long-press-power)

**Explicitly excluded:**

- ESP-NOW config actually doing anything (the menus exist but ESP-NOW transport is Epic 4)
- DMX config actually doing anything (Epic 7)
- Calm Mode safety implementation (deliberate - that's part of Epic 5 Tildagon work where it matters most)
- Any non-StickC UI (Tildagon UI is Epic 5)

## Acceptance Criteria

- [x] Boot countdown works: 5 seconds to default mode, any button interrupts to menu. Closed by Epic 2 mode FSM work (`BootMode` in `src/modes/mode_machine.cpp`). Note: default boot mode is currently **AutonomousMaster** rather than the spec's Slave, on the basis that pre-Epic-4 there's no ESP-NOW for Slave to actually listen on. Slave is selectable from the menu. Revisit when Epic 4 ships.
- [x] Mode-selection menu navigable with Btn-A/Btn-B, BtnPWR for back. Closed by Epic 2 (`MenuMode`). Bindings: Btn-A (Btn1) selects, Btn-B (Btn2) cycles, BtnPWR long-press returns to menu from any runtime mode.
- [ ] Test Mode reaches all six sub-tests (Pulse, Fade, Rainbow, Sparkle, White Out, Group Targeting)
- [ ] Config tree navigable to all leaf settings; settings persist to NVM and survive reboot
- [ ] Status display shows current mode, BPM (when audio active), battery, IR fire count
- [x] Returning to a previously-used mode after reboot takes one button press (last-used remembered). Closed by Epic 2 NVS-backed `noct/last_mode` persistence; Menu cursor pre-selects the last-used mode so a single Btn1 confirms it.

## Features

Anticipated Features:

- Feature 3.1: Boot flow and countdown
- Feature 3.2: Mode-selection menu and navigation
- Feature 3.3: Test Mode with six sub-tests
- Feature 3.4: Config tree and NVM-backed settings persistence
- Feature 3.5: Runtime status display

## Dependencies

| Dependency | Type | Status | Owner |
| --- | --- | --- | --- |
| Epic 2 (architecture refactor) | Internal | Done (2026-05-08) | Jason |
| Architecture spec v0.16+ (§8) | Internal | Done | Jason |

## Target Sprint Range

- **Start sprint:** After Epic 2 Done
- **End sprint:** 1-2 sprints later
- **Indicative complexity total:** 15-20 points across child Features

## Status Notes

Proposed 2026-05-06 from spec v0.16 epic decomposition.

The one-press-to-resume-last-mode behaviour matters more than it might seem - it's what makes the StickC pleasant to use as a regular tool rather than a fiddly device that needs careful reconfiguration every time. Worth the implementation effort.

**Updated 2026-05-08 (after Epic 2 close):** roughly half of Epic 3 is already done as a side-effect of Epic 2's mode state machine work. Closed ACs: boot countdown, mode-selection menu, last-used-mode persistence. Remaining work:

- **Test Mode sub-tests** per spec §8.5 (six patterns: Pulse, Fade, Rainbow, Sparkle, White Out, Group Targeting). The spec'd six are well-defined and three of them (Rainbow, Sparkle) can lean on the Effect classes already shipped in Epic 2.
- **Config tree** per spec §8.4 (Audio, IR, ESP-NOW, WiFi, DMX/Art-Net, System). Most leaves under ESP-NOW/WiFi/DMX won't actually do anything pre-Epic-4/Epic-7; they exist for future-proofing the menu shape. Settings persistence uses the same NVS pattern as the Mode FSM.
- **Status display polish** - AutonomousMaster already shows mode/BPM/battery/flux meter; need to add an IR fire counter and a "no network yet" placeholder for the network heartbeat.
- **Spec deviations to reconcile** - default boot mode (Slave per spec vs. AutonomousMaster as built); Idle/Off mode (spec lists it separately, implementation collapsed it into Menu).

**Updated 2026-05-08 (Test Mode sub-tests landed):** the §8.5 Test Mode is now implemented. Test Mode opens to a sub-test list; Btn2 cycles, Btn1 launches. PWR-hold from a sub-test returns to the sub-test list; PWR-hold from the sub-test list returns to the main mode menu. Implemented sub-tests: Pulse, Fade, Rainbow, Sparkle, White Out, Group Targeting (cycles groups 1-5), and a seventh **Set Group ID** entry that's not strictly part of §8.5 but added now to support the hardware day on 2026-05-09 (new PixMob bracelets arriving). Set Group ID writes to PixMob slot 0 via `pixmob::buildSetGroupId`, surfaced through a new `AssignDeviceGroup` DAL capability and `DAL::fire_assign_device_group` helper. The Set Group ID UI should move to Config > IR > Group ID assignment when Config lands; for now Test Mode is the most accessible home.
