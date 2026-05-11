---
title: "Epic 4.65: Class+group device addressing"
status: Done
notion_url:
notion_id:
notion_status:
last_synced:
sync_direction: bidirectional
---

## Related Documents

- [Epic 4.6: Clean architecture and UI polish](epic-04.6-ui-cleanup.md) - the architecture stream this Epic picks up from. `Visualisation` / `OutputBinding` plug-in surfaces and the `render_fx(target, ev)` canonical send shipped there; this Epic generalises the target field.
- [NocturNation Architecture Specification](https://www.notion.so/357bd0677405800b891beab0f4e0a976) - particularly §4.3 (ESP-NOW frame format), §7.6 (plug-in surfaces), and §6 (canonical send `render_fx`).
- [Epic 4.7: Dynamic show from FFT](epic-04.7-dynamic-show.md) - downstream Epic; richer fx types arrive here, addressing them is this Epic's job.
- [Epic 5: Tildagon receiver app](https://www.notion.so/358bd067740581b19551d158d658df76) - downstream Epic; Tildagon-class (`0x03`) devices land there and slot into this Epic's class taxonomy without protocol changes.

## Goal

Replace the free-form device-name strings used by `render_fx` (`"all-pixmobs"`, `"group-1"`, `"esp-now-broadcast"`, ...) with a structured **`"<class>:<group>"`** target taxonomy. Operators and visualisations address devices by *what they are* (light bracelet, screen device, multi-LED+screen device) and *which group they belong to*, regardless of transport, vendor, or wire protocol. The same render call works for PixMob X4 bracelets, future NocturNation-native light wristbands, and a Tildagon LED ring without per-device branching at the call site.

Both fields are bytes; both reserve `0x00` for "all". `"00:00"` is broadcast-to-everyone; `"01:00"` is every light-class device in any group; `"01:07"` is light-class group 7; `"03:01"` is Tildagon-class (multi-LED + screen) group 1.

## Why this Epic exists

Epic 4.6's `Visualisation` / `OutputBinding` plug-in surfaces shipped with a critical hole in the addressing layer. `render_fx` accepts a string target name (`"all-pixmobs"`, `"group-N"`, `"esp-now-broadcast"`, `"local"`) which means:

- Each device class needs its own naming convention (PixMob uses `"all-pixmobs"` and `"group-1..5"`; ESP-NOW slaves use `"esp-now-broadcast"`; future Tildagon class would invent its own); no unified addressing across classes.
- The master can broadcast to all slaves over ESP-NOW (`"esp-now-broadcast"`) but cannot address a subset of slaves. The slave-side `PixMobIrBinding` `group` property is per-slave-static config, not master-driven.
- Adding a new device class (a NocturNation-native light wristband; a stick with an accelerometer) means inventing another target-name convention and threading it through every vis.
- `LightCommandPayload` on the wire already has a `target_group` byte (per spec §4.3) that no one writes or reads - the schema designer left the field; the plumbing was never finished.

The fix is to make `render_fx` targets a two-axis address - class and group - encoded both at the API layer (the target string) and on the wire (extending `LightCommandPayload`). Devices declare their class via `OutputBinding::class()`; the framework's routing matches incoming class+group against each active binding's `(class, configured group)` pair and only fans out matching frames.

This Epic is small in scope - it's a focused refinement of the addressing surface, not a new capability - but it's load-bearing for Epic 5 (Tildagon devices need a class) and for any future "address subset of audience" show choreography.

## Operational model

Laptop-driven, same as Epic 4.6. Native test envs catch routing-matrix regressions; hardware verification on Plus2 + S3 with multiple slaves in different groups confirms the filter logic works end-to-end.

Verification ownership: **(L)** = laptop / native test, **(B)** = build-time check, **(H)** = hardware verification by Jason.

## Scope

**Included:**

- **`DeviceClass` enum** in a host-agnostic header. Initial values: `All` (0x00), `Light` (0x01), `Screen` (0x02), `MultiLedScreen` (0x03). Comment block flags 0x04..0xFF as reserved for future classes (accelerometer-stick, smoke-machine, ...).
- **`OutputBinding::class()` contract addition** - new virtual method on the `OutputBinding` base. `PixMobIrBinding` returns `Light`; `LocalDisplayBinding` returns `Screen`. Future `TildagonLedRingBinding` (Epic 5) returns `MultiLedScreen`.
- **Wire format extension**: `LightCommandPayload` gains a `target_class: u8` field at offset 0 (existing `target_group` shifts to offset 1). Payload grows 8 → 9 bytes. `kLightCommandPayloadLen` bumps to 9. Encoder + decoder updated; native byte-parity tests track the new shape.
- **Master encode path**: `EspNowBroadcastDriver::send` parses the structured target name and writes both `target_class` and `target_group` into the `LightCommandPayload`. The driver-level `device->group_id` carry-through stays (used by PixMob IR driver for its own protocol group byte) but `render_fx` callers now use the structured form.
- **Slave filter**: `SlaveMode::on_recv` (or the per-binding fan-out wrapper) compares the inbound `(target_class, target_group)` against each active binding's `class()` AND the device-wide `slv_group` setting. Calls `on_light_command` only where `target_class == 0x00 OR target_class == binding.class()` AND `target_group == 0x00 OR target_group == slv_group`. The group axis is **device-wide** (one slv_group per slave host), distinct from per-binding group properties: those remain in the OutputBinding for output-protocol-specific reasons (PixMobIrBinding's `group` is the PixMob *protocol's* group code on the IR wire, not the NocturNation receive-filter group).
- **slv_group NVS config**: new device-wide setting persisted under NVS key `slv_group`, default `0` (matches anything). Single Config menu item ("Slave Group", 0-255 manual selection) added under the slave-mode section. Distinct from the existing Test-Mode "Set Group ID" facility which programs PixMob bracelets via `buildSetGroupId`; this one is the local device's own NocturNation identity.
- **PixMob group range**: bump `PixMobIrBinding`'s `group` property max from 5 to 31 (PixMob protocol's native range; current 0-5 cap was a `SlaveMode::ir_target_name` artifact). Device registration adds `"group-6"..."group-31"` in `dal::DAL::begin()`. This is a PixMob-protocol-output concern, orthogonal to slv_group.
- **`render_fx` target parsing**: DAL parses `"<hex>:<hex>"` target strings into `(DeviceClass, uint8_t group)`. Legacy strings (`"all-pixmobs"`, `"group-N"`, `"local"`, `"esp-now-broadcast"`) keep working via a small compatibility shim for the duration of this Epic; removed in close-out.
- **Auto-generated config UI**: each binding's `group` property surfaces in the Settings overlay (the auto-generated UI from Epic 4.6 Block 10 already handles `U8` properties; this Epic widens the value range and adds a small "Class: Light" read-only line above the group field as a sanity-check label).
- **Master-side: visualisations choose their target class**. `BeatPulseVisualisation` and `SpectrumBarsVisualisation` (manual fire) update their `render_fx` calls to `"00:00"` (everyone) by default. A future vis that wants to address only screen-class devices uses `"02:00"`.
- **Native test coverage**: routing-matrix tests asserting every `(target_class, target_group)` × `(binding_class, binding_group)` cell fires or skips correctly. Encoder/decoder byte-parity tests for the new 9-byte `LightCommandPayload` shape.
- **NVS migration**: existing `nb_pixmob-ir/group` keys carry over unchanged - the property name and meaning are the same; only the value range widens.

**Explicitly excluded:**

- Class addressing for `MUSIC_EVENT`, `HEARTBEAT`, `MODE_CHANGE`, or any other message type. `LIGHT_COMMAND` is the only addressed type for v1; the other types stay broadcast-only because their semantics are inherently global (heartbeat is "I'm alive"; music_event is "DROP detected"). Future Epic can extend if a real use case appears.
- Multi-class device hosts. A StickC slave with both `LocalDisplayBinding` AND `PixMobIrBinding` is conceptually *two* classed devices on one chassis; each binding declares its own class and they're addressed independently. No notion of a "host class" - the binding owns the class.
- Capability-aware fx fallbacks (e.g. PixMob receives a "rainbow" fx and falls back to a closest-RGB pulse). Architecturally hooked via the existing `CapabilityMask`; concrete fallback design defers to Epic 4.7 when richer fx types appear.
- Stadium-scale individual addressing (32k+ groups). `u8` cap (256 groups) is plenty for any realistic deployment; widening to `u16` is a one-byte-shaped follow-up if and when stadium-individual addressing materialises.
- Tildagon-side implementation. Epic 5 lands the `TildagonLedRingBinding` returning `MultiLedScreen`; this Epic just reserves `0x03` in the class enum and includes a passing comment.
- Master-to-master class addressing. With one master per deployment today, master-side routing isn't a concern.

## Acceptance Criteria

- [ ] **(B)** Code builds cleanly under both `[env:m5stack-stickcplus2]` and `[env:m5stack-stickcs3]`.
- [ ] **(L)** All existing native test envs continue to pass.
- [ ] **(L)** New native coverage: `target_class` encode/decode round-trip; routing-matrix filter (every `(target_class, target_group)` × `(binding_class, binding_group)` cell either fires `on_light_command` or skips); `render_fx` target-string parser; legacy-target-name shim still resolves `"all-pixmobs"` etc. for the duration of the Epic.
- [ ] **(L)** PixMob byte-parity tests pass: the IR wire output for `"00:00"` (everyone) and `"01:00"` (all light) over the legacy `"all-pixmobs"` path is byte-identical to today's behaviour.
- [ ] **(H)** Plus2 master + two S3 slaves: slave A configured `Light` group 1, slave B configured `Light` group 2. Master fires `"01:01"` → only slave A fires PixMob IR. Master fires `"01:02"` → only slave B. Master fires `"00:00"` → both fire.
- [ ] **(H)** Plus2 master + Plus2 slave with `LocalDisplayBinding` only (IR binding disabled in config). Master fires `"02:00"` → slave's display lights up. Master fires `"01:00"` → slave does nothing (no `Light` binding active).
- [ ] **(H)** Plus2 master + S3 slave with both bindings active, configured to different groups (`LocalDisplay` group 1, `PixMobIr` group 7). Master fires `"02:01"` → only the display lights. Master fires `"01:07"` → only the IR fires.
- [ ] **(H)** Coldplay tribute regression: BeatPulse + PixMob IR path still works as before (`"00:00"` default target preserves Epic 4.6 behaviour).

## Blocks of work

Work proceeds in roughly this order. Earlier blocks introduce the contract; later blocks wire it through and migrate.

### Block 1: Capture the design in docs

Update [`docs/architecture.md`](../architecture.md) §4.3 with the new `LightCommandPayload` shape (9 bytes including `target_class`) and §7.6 with the `OutputBinding::class()` contract addition. Update `docs/dal-design.md` (if it exists) with the `"<class>:<group>"` target parser. Doc-only block; nothing builds.

- Commit: "Epic 4.65 Block 1: spec class+group addressing in architecture"

### Block 2: DeviceClass enum + OutputBinding::class() virtual

Introduce `include/hal/device_class.h` (or `include/plugins/device_class.h` - decide at implementation time; class is a plug-in concept, not a hardware one). Enum values `All=0x00, Light=0x01, Screen=0x02, MultiLedScreen=0x03`; 0x04..0xFF reserved with a comment block. Add `virtual DeviceClass OutputBinding::class() const = 0` (pure-virtual; every binding declares its class). `PixMobIrBinding` returns `Light`; `LocalDisplayBinding` returns `Screen`.

- Commit: "Epic 4.65 Block 2: DeviceClass enum + OutputBinding::class() contract"

### Block 3: Wire format - target_class on LightCommandPayload

Extend `LightCommandPayload` from 8 to 9 bytes: new `target_class: u8` at offset 0; existing fields shift one byte. Bump `kLightCommandPayloadLen` to 9. Update `encode_light_command` / `decode_light_command` in [`src/transport/espnow/frame.cpp`](../../src/transport/espnow/frame.cpp). Update the native byte-parity tests against the new wire shape (one-off shift; tests then stay locked at 9 bytes).

The wire-format break is acceptable - the project's only deployment is the bench, so a coordinated re-flash of master + slaves is free. No protocol-version bump (still v1) since the change is additive within a single payload type.

- Commit: "Epic 4.65 Block 3: LightCommandPayload gains target_class (8 -> 9 bytes)"

### Block 4: Target-string parser + master encode path

`render_fx(target, ev)` learns to parse `"<hex>:<hex>"` strings into `(DeviceClass, uint8_t)`. `EspNowBroadcastDriver::send` writes both bytes into the outbound `LightCommandPayload`. Legacy target names (`"all-pixmobs"`, `"group-N"`, `"local"`, `"esp-now-broadcast"`) stay working via a small lookup table that translates them to the new pair - removed in Block 9 close-out.

- Commit: "Epic 4.65 Block 4: render_fx parses class:group; master encodes target_class"

### Block 5: Slave filter + slv_group config

`SlaveMode::on_recv`'s `LIGHT_COMMAND` decode path now reads `target_class` and `target_group` from the payload. For each active binding, only call `on_light_command` if `target_class == 0x00 OR target_class == binding.class()` AND `target_group == 0x00 OR target_group == slv_group`. The `slv_group` axis is **device-wide** (one value per slave host), loaded from NVS in `SlaveMode::enter()` via a new `persistence::load_slv_group()` helper.

Block 5 also adds the Config menu item for `slv_group`: a single line under the slave-mode section showing the current value with the standard A-click / B-click cycle pattern (0-255 manual selection, persisted on change). Distinct from the existing per-PixMobIrBinding `group` property (which lives in the auto-generated Settings overlay and controls PixMob protocol output, not NocturNation receive filtering).

- Commit: "Epic 4.65 Block 5: slave filter + slv_group config menu"

### Block 6: PixMob group range 0..31; new DAL device registrations

`PixMobIrBinding`'s `group` property `max_value` bumps from 5 to 31 (PixMob protocol's native cap). [`src/dal/dal.cpp`](../../src/dal/dal.cpp) `DAL::begin()` registers `"group-6".."group-31"` against the PixMob profile alongside the existing `"group-1".."group-5"` and `"all-pixmobs"`. Enum names in the property schema (auto-generated UI) update accordingly.

- Commit: "Epic 4.65 Block 6: extend PixMob group range to 0..31"

### Block 7: Master visualisation updates

`BeatPulseVisualisation` and `SpectrumBarsVisualisation`'s manual-fire path move from `"esp-now-broadcast"` / `"all-pixmobs"` to `"00:00"` (everyone). Behavioural equivalence: byte-parity tests still pass; on-the-wire `target_class=0, target_group=0` matches every binding. The legacy target-name shim is what makes this safe to land before Block 9 removes it.

- Commit: "Epic 4.65 Block 7: vis use class:group targets"

### Block 8: Hardware verification + multi-slave test deployment

Plus2 master + two S3 slaves in different groups + selective-binding scenarios from the acceptance criteria. Catch any wire / encoding / filter mistakes the native suite missed.

- Commit: "Epic 4.65 Block 8: multi-slave hardware verification fixes" (if any)

### Block 9: Remove legacy target-name shim; close-out

Drop the legacy-name lookup table from Block 4. `render_fx("all-pixmobs", ev)` becomes a hard error (caller migrated to `"01:00"` or `"01:N"`). Update [`docs/architecture.md`](../architecture.md), the Notion mirror, and [`memory/project_hal_dal_architecture.md`](/Users/jasonratcliffe/.claude/projects/-Users-jasonratcliffe-Documents-NocturNation-StickC/memory/project_hal_dal_architecture.md). Final commit.

- Commit: "Epic 4.65 Block 9: drop legacy target-name shim + close-out"

## Behaviour preservation contract

Two things must stay locked across this Epic:

- **PixMob IR wire output for the existing default path**: when a vis fires `"00:00"` (the new equivalent of today's `"all-pixmobs"` broadcast), the bytes on the IR wire from `PixMobIrBinding` must be identical to the pre-Epic behaviour. PixMob byte-parity tests are the regression net.
- **Heartbeat timing and skip-if-recent semantics**: untouched - heartbeat is broadcast-only and not addressed by this Epic.

## Dependencies

| Dependency | Type | Status | Owner |
|---|---|---|---|
| Epic 4.6 (architecture + UI) | Internal | Done | Jason |
| Plus2 + S3 hardware on desk | External | Available | Jason |
| Optional: second S3 for multi-slave H verification | External | Confirm before Block 8 | Jason |

## Status Notes

Pickup from Epic 4.6's architecture stream. Epic 4.6's `OutputBinding` contract made plug-in render destinations possible; this Epic gives them a proper addressing scheme so a master can talk to "the light-class devices in group 7" rather than "everyone on the wire and let the slaves figure it out".

The design discussion landed three operator-friendly choices over more architecturally pure alternatives: device class as an explicit byte rather than a `CapabilityMask` (operators think in classes, not capability sets); `u8` group rather than `u16` (256 groups is enough for any realistic deployment); PixMob bracelets folded into the `Light` class rather than kept as a parallel target namespace (unified addressing model wins over taxonomic separation). The `"<hex>:<hex>"` target string is the API surface; the structured wire fields are the implementation.

Total estimated effort: **~5-6 hours of coding plus hardware verification**. Each block is mechanical (one enum, one virtual method, one payload byte, one target-string parser, one slave filter, a numeric range bump, two call-site migrations, one cleanup pass). Comfortably half a day heads-down, a day with the H lane.

## Close-out (2026-05-11)

All eight code blocks shipped per-block-committed across `main`. Block 8 (hardware verification on Plus2 + S3 multi-slave scenarios) is the only open item; it's bench work that doesn't gate the architecture changes.

**Final test count: 286 native tests across 15 envs**, all passing. Both firmware envs (`m5stack-stickcplus2`, `m5stack-stickcs3`) build clean. PixMob byte-parity tests held across the wire-format change.

**Headline outcomes:**

- `DeviceClass` enum lives at `hal::DeviceClass` next to `hal::Capability`. Operator-facing taxonomy: `All / Light / Screen / MultiLedScreen`, plus 0x04..0xFF reserved.
- `OutputBinding::device_class()` is pure-virtual; `OutputBinding::is_relay()` defaults false. PixMobIrBinding overrides relay→true so it transmits PixMob IR with the inbound `target_group` regardless of slave membership.
- `LightCommandPayload` carries both `target_class` and `target_group` (9 bytes total, up from 8). Encoder + decoder + round-trip tests updated.
- `DAL::render_fx("<hex_class>:<hex_group>", ev)` parses structured targets; legacy names ("local", "all-pixmobs", "group-N") still resolve via the device registry for the master's own local IR + screen. The redundant `"esp-now-broadcast"` device-name entry was removed in Block 9; the driver still claims the transport name at the driver layer.
- `Driver::send(target_class, target_group, ev)` is the 3-arg overload with a default that forwards to the existing 2-arg dropping the class. Production routes via `find_driver_for_transport("esp-now-broadcast")` so test envs can intercept with recording drivers under the same transport.
- `slv_group` is the slave's NocturNation receive-filter group ID. Persisted under NVS key `slv_group` (default 0 = match anything); operator sets via Config > ESP-NOW > Group.
- Slave filter: local bindings fire on `(target_class == 0 OR matches) AND (target_group == 0 OR matches slv_group)`. Relay bindings bypass the slv_group axis and read `OutputBindingContext::current_target_group()` to thread the inbound group code into their downstream protocol.
- PixMob group range widened from 0..5 (a `SlaveMode::ir_target_name` artifact) to 0..31 (the PixMob protocol's native 5-bit field). `DAL::begin()` registers `group-1`..`group-31`. `kMaxDevices` bumped 32 → 48.
- Master vis (`BeatPulseVisualisation`, `SpectrumBarsVisualisation`) and Test Mode all use `"00:00"` now (broadcast everyone). Master's local IR continues to use `"all-pixmobs"` / `"group-N"` (local PixMob driver, not ESP-NOW).

**Deferred:**
- **(H)** lane: bench verification of multi-slave scenarios in Block 8 (acceptance criteria 5-8 in the spec above).
- Class addressing for `MUSIC_EVENT`, `HEARTBEAT`, `MODE_CHANGE`. Those frames stay broadcast-only by design - global semantics ("I'm alive", "DROP detected").
- Capability-aware fx fallbacks (richer fx types degrading on bindings that can't render them natively). Architectural hooks via `CapabilityMask`; concrete fallback design defers to Epic 4.7 when the first richer fx surfaces.
- Stadium-scale individual addressing (>256 groups). 8-bit group is enough for now; widening to u16 is a follow-up if the use case materialises.
