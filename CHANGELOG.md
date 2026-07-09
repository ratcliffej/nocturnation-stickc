# Changelog

Notable changes to the NocturNation M5 firmware. Newest first.

## 2026-07-07 — Atom repeater: hop-transparent relay

The headless Atom repeater no longer increments `hop_count` when it
rebroadcasts — frames are relayed verbatim. Suggested by ratcliffej on
PR #27: with the fleet's 3-hop ceiling, an Atom hop stage-side spent
budget the audience-side Lume relays needed, trading depth for breadth.
Now Atoms are free breadth anywhere in the field and Lume dynamic
routing keeps the full 3 hops of depth. Duplicate copies are handled by
receiver dedup (the Director already sends everything twice).

Without a hop ceiling on this path the dedup ring is the only loop
guard, so the repeater now drops unsequenced frames (seq 0 bypasses
dedup) instead of relaying them — two Atoms in mutual range would
otherwise ping-pong a seq-0 frame forever. No real traffic is lost:
every fleet sender sequences (the Director's counter wraps 255 → 1),
and census — the one seq-0 frame in the wild — was already filtered by
type. Lume repeat is unchanged (still hop+1, 3-hop cap).

- `src/modes/repeater_mode.cpp` — relay verbatim: drop the
  `set_hop_count()` bump and the `hop_count < 3` gate; refuse seq-0
  frames pre-dedup.
- `src/modes/repeater_mode.h` — behaviour doc; drop unused
  `kMaxHopCount`.

## 2026-07-03 — Repeater census talkback from Lume repeaters

Repeat-enabled Lumes (StickC Plus2 / StickS3 / Atom Lite — any host
running the shared `LumeMode`) now beacon the same 1 Hz
`REPEATER_HEARTBEAT` the headless Atom repeater emits, so every relay in
the field — dedicated or Lume — shows up in the Director's census
(console `repeatrs: N online` listing and the AtomS3R dashboard's
`rpt N`). Gated on holding a Director lock: an unlocked Lume may be mid
channel-scan and is relaying nothing worth reporting. uid = low 3 bytes
of the STA MAC, the same identity scheme as the headless repeater, so
the census dedups both kinds identically.

Safety of the beacon rests on the existing TofuLock rule that broadcast-
source frames are never admitted: the census can't steal locks, refresh
liveness, or be re-relayed into echoes. `LumeMode::on_recv` additionally
drops `REPEATER_HEARTBEAT` before the TOFU gate so the RXdrop log isn't
spammed at one line per repeater per second.

- `src/modes/lume_mode.{h,cpp}` — `emit_repeat_census()` (mirror of
  `RepeaterMode::emit_census`), census uid capture on radio bring-up,
  early census drop in `on_recv`.

## 2026-07-03 — AtomS3R Director: LCD dashboard

New `m5stack-atoms3r-poe` env: the same Ethernet DMX Director as
`m5stack-atoms3-poe`, on the AtomS3R (which has a 0.85" 128×128 LCD).
`NOCT_ATOMS3R` declares `Capability::Display` in the shared
`hal_atoms3poe` backend and DmxBridge draws a 10 Hz dashboard through
the DAL's `local` display path: health banner (same red/purple/amber/
green taxonomy as the Lite's status LED, plus the ESP-NOW fleet
channel), IP / universes / frame-count / age vitals, the 27
broadcast-block channels as colour-coded bars, and a fleet-colour box
showing the colour the broadcast block currently commands. The
onboard-WS2812 LED path is compiled out on the S3R (no such LED —
GPIO 35 is an in-package PSRAM line).

- `src/hal_atoms3poe/display_atoms3r.{h,cpp}` — M5.Display-backed
  `hal::Display`, compiled only under `NOCT_ATOMS3R`.
- `src/hal_atoms3poe/hal_atoms3poe.cpp` — conditional `Display`
  capability + accessor.
- `src/modes/dmx_bridge_mode.cpp` — `draw_screen_dashboard()` in the
  Ethernet branch; no-ops on Directors without a panel.
- `src/dal/drivers/dmx_channel_mapper.{h,cpp}` — pure static
  `preview_rgb()` mirroring the raw/wash wire-scaling rules (shared
  `scale_raw_byte` helper), so the fleet box shows exactly what the
  fleet is told; 5 new native tests in `test_dmx_channel_mapper`.
- `src/dal/drivers/ethernet_dmx_adapter.{h,cpp}` — `local_ip()`
  accessor so the mode doesn't include `Ethernet.h`.

Bench follow-ups: the banner also carries the headless-repeater online
count (`rpt N`, from `RepeaterCensus`) next to the fleet channel, and
headless builds (`NOCT_HEADLESS_DMX_BRIDGE` / `NOCT_HEADLESS_REPEATER`)
now boot straight into their runtime mode in `ModeMachine::begin()` —
the Boot splash is Stick UX whose layout assumes the 240×135 panel and
rendered mangled on the S3R's square screen for the whole Ethernet
bring-up.

## 2026-07-03 — Fleet-review Tier 1 quick wins

Three targeted robustness fixes surfaced by the 2026-07-02 Fable
multi-agent fleet review; all small, isolated, and closing genuine
deployment-risk failure paths.

- **PixMob wash divide-by-zero guard** (`pixmob_ir_driver.cpp`,
  finding #10). `compute_drift_rgb` re-read `state.cycle_ms` twice
  under a caller-side `cycle_ms > 0` gate, so a mid-cycle
  drifting→static wash swap (WiFi task overwriting the slot's
  `cycle_ms` between the guard and the modulo) produced an Xtensa
  IntegerDivideByZero panic. Fixed by snapshotting `cycle_ms` once
  into a local and holding colour A on the zero path, matching the
  documented static-wash behaviour every caller already pre-seeds
  for.

- **Passthrough sequence-number rewrite** (`espnow_broadcast_driver.cpp`,
  finding #9). `send_passthrough` re-stamped `source_id` (byte 3) to
  claim orchestrator-originated display frames as this Director's,
  but left the orchestrator's independent 1..255 seq counter in
  place. Two unrelated seq streams under one `source_id` collide in
  every receiver's `(source_id, sequence_number)` dedup ring and
  inflate SignalQuality's forward-gap miss count. Fixed by also
  re-stamping `sequence_number` (byte 4) from `next_seq()` in the
  same branch, so passthrough frames join this Director's single
  monotonic stream.

- **Lume session state reset on enter** (`lume_mode.cpp`, finding
  #33). `LumeMode` is a persistent static singleton, so a Config
  round-trip re-entered it with the previous session's dedup ring
  and any staged-but-undrained `pending_light_` / `pending_repeat_`
  frame still present. After the round-trip up to 16 fresh unique
  frames could be silently dropped as "already seen" (or a stale
  queued frame could fire once against a possibly-stale source_id).
  Fixed by zeroing the dedup ring and clearing both pending slots
  in `enter()`, before `radio->begin()` re-arms the WiFi-task recv
  callback.

## 2026-07-02 — Repeater hop_count: pin the byte offset

`LumeMode`'s repeater already writes `hop_count` at the correct wire
offset (byte 5, fixed 2026-06-28 during Epic 15 bench), but the offset
was a bare literal at two sites in `lume_mode.cpp` and lived far from
the header layout it depends on. This adds a named constant
`transport::espnow::kHopCountOffset` and a `set_hop_count()` helper
alongside `write_header()` / `decode_header()`, and routes the
repeater's in-place bump through the helper. Same behaviour, but the
offset can't drift silently from the header again — the v1 → v2
magic-prefix migration missed exactly this once already.

- `include/transport/espnow/frame.h` — declare `kHopCountOffset` +
  `set_hop_count()`.
- `src/transport/espnow/frame.cpp` — implement `set_hop_count()` as a
  buf-length-guarded byte write.
- `src/modes/lume_mode.cpp` — use the helper at the increment site;
  reference `kHopCountOffset` at the debug-print site. Drops the
  long historical comment describing the pre-2026-06-28 buf[3] bug —
  the fix history is now in this CHANGELOG entry rather than inline.
- `test/test_espnow_frame/test_main.cpp` — existing
  `test_relay_hop_increment_preserves_source_id` now exercises the
  helper instead of a raw byte-index rewrite; two new tests cover
  `set_hop_count`'s contract directly (touches only the hop byte;
  short-buffer no-op).

Motivated by ab-gh's PR #23; opened as a fresh branch off current
main to avoid the merge-conflict-resolution regression that ate 300
lines of Epic 13 tests and reverted `kMaxFrameSize` in that PR.

## 2026-05-21 — Epic 6B: Tildagon Director + Show framework (M5-side + docs)

Epic 6B lands the Tildagon Director + Show framework (in the
`nocturnation-tildagon` repo). The M5-firmware contribution is small —
the cross-platform capability + hook surface — plus the documentation
that's shared across both hosts.

- **`hal::Capability`** gains `ImuTap` + `ImuMotion` sub-capabilities
  ([include/hal/hal.h](include/hal/hal.h)), composing what an IMU
  backend produces (mirroring the Epic-4.5 analyser sub-cap pattern).
  **Reserved-but-unwired on M5** — no M5 backend emits them yet — so a
  cross-platform Show can declare a stable `required_capabilities()`
  mask; the Tildagon declares + fires them. `ImuFreeFall` was
  considered and dropped (no consumer).
- **`Show` base class** gains `on_tap_detected(strength)` +
  `on_motion_event(axis, magnitude)` hooks with no-op defaults
  ([include/shows/show.h](include/shows/show.h)) — forward-compatible
  so a Show consuming IMU input compiles on M5 unchanged (they simply
  never fire until an M5 IMU backend exists).
- No behavioural change on M5: nothing declares or fires the new flags
  / hooks yet. Both firmware envs build clean; native plugin/show tests
  pass.
- **Docs**: `docs/developing-shows.md` refreshed cross-platform
  (hosts-and-capabilities matrix, IMU hooks, MicroPython surface,
  porting guide); `docs/manuals/operator-workflow.md` gains a Tildagon
  Director/Lume section (the WiFi-off-while-running constraint, idle
  menu, channel pinning, Help/QR, button map); `docs/hal-design.md`
  documents the sub-capability pattern; `docs/architecture.md` corrects
  the "hosts without a mic can't be a Director" claim (IMU tap-to-beat
  is a valid beat source) and adds §8.9. Working plan: Epic 6B
  (canonical in Notion).

## 2026-05-17 — Epic 5.5: channel 11 access control (source_id partition + TOFU)

Lightweight protection for channel 11 (Performance mode) against the
most common failure mode at EMF: accidental disruption from another
operator's M5 Stick broadcasting on the same channel. Uses the existing
`source_id` field as a partitioned namespace plus listen-before-
broadcast on the Director side and Trust-On-First-Use locking on the
Lume side. **No wire-format change**; `protocol_version` stays at
`0x02`. The convention is layered on top of the existing field.

Partition (protocol manual §3.4):

- `0x00 - 0x3F` (64 slots) - **community / hobby**, channel 1
- `0x40 - 0xFE` (191 slots) - **Performance mode**, channel 11
- `0xFF` - broadcast / anonymous (unchanged)

Director-side behaviour:

- Channel 1: stable per-device source_id. First boot rolls a random
  value in `[0x00, 0x3F]` and persists to the new NVS key `mst_src_id`
  under namespace `noct`; subsequent boots reuse. A returning Lume
  locks back to the same Director across power cycles.
- Channel 11: fresh random Performance-range id at every boot. The
  Director listens for one second on the chosen id before transmitting;
  if a `HEARTBEAT` matching its candidate arrives during the listen
  window the candidate is re-rolled. After three consecutive collisions
  the Director proceeds with the third pick and logs a Serial warning
  (operationally extremely rare given 191 slots).
- Channel 6: legacy MAC-derived behaviour preserved (operator-
  discretionary per spec; no automatic protection).
- New `StartupState` enum (Idle/Listening/Active) on the broadcast
  driver describing phase. `active_` stays as the TX gate.
- Source_id rendered bottom-right on the M5 Stick screen during
  Director Mode and Test Mode: `C:nn` (community), `P:nn` (Performance),
  `P:nn?` (tentative during the channel-11 listen window), `?:nn`
  (defensive out-of-range). Operator and audience can confirm both
  ends locked to the same id visually.

Tildagon-side behaviour lives in the companion repo
(`ratcliffej/nocturnation-tildagon`): TOFU lock to the first valid
frame from a non-broadcast source, cross-range filter on channel 11
(community-range source_ids dropped without locking), 10 s timeout,
operator-driven "Rescan" menu item.

Spec change captured inline in protocol-manual.md §3.4 / §7.1: TOFU
locks on the first valid frame, not specifically the first HEARTBEAT.
The HEARTBEAT-only wording from the initial v0.29 draft didn't compose
with skip-if-recent heartbeat suppression during active music - a
Lume joining mid-song would otherwise sit idle for the duration of
the song. Both Director and Lume sides updated together.

New documentation:

- `operator-workflow.md` (nocturnation-docs repo) - operator-facing
  guide for running Performance Mode deployments, source_id
  verification, residual risk discussion.
- `protocol-manual.md §3.4` (nocturnation-docs repo) - normative spec
  for the partition + TOFU rules.
- Protocol manual Annex B - NVS key `mst_src_id` documented.

Threat model is honestly non-cryptographic: ~95% of *accidental*
disruption is prevented; *determined* attackers reading the open-
source firmware can defeat this scheme. For the EMF 2026 deployment
context this trade-off is the right one. Tier 1+ crypto remains a
future Epic.

366/366 native tests pass; both `m5stack-stickcplus2` and
`m5stack-stickcs3` firmware envs build clean. Hardware bench
verification pending.

## 2026-05-16 — Auto-scan adds channel 6, re-scan threshold decoupled

Two behavioural changes to the Lume's auto-scan loop (spec v0.29
§5.3 / §5.4):

- **Scan order is now 11 → 1 → 6 → repeat** (was 11 → 1 → repeat).
  Channel 6 (advanced operator override) is now a real auto-scan
  target rather than operator-locked-only. Worst-case discovery
  latency goes from 4 s to 6 s; in exchange a Lume on a freshly-
  flashed device with default `slv_chan = 0` will find a Director
  on any of the three configured channels without operator
  intervention. Important for simple Lumes (bracelet form factor)
  that lack a UI to set channel manually.

- **Re-scan threshold is now `kRescanMs = 10000` (10 s)**,
  decoupled from `kNoSignalMs = 3000` (3 s) which still drives the
  NO SIGNAL display. NO SIGNAL still fires quickly so the operator
  sees the outage; re-scan waits longer because most signal losses
  are transient (Director reboot, brief congestion, person blocks
  line of sight) and the existing channel is more likely to recover
  than a new one to appear. Saves a ~6-second multi-channel hunt
  when the Director is just briefly absent.

- A Lume explicitly locked to a channel by operator configuration
  (`slv_chan ∈ {1, 6, 11}`) still does **not** re-scan on signal
  loss. The operator chose that channel; the Lume respects it.

Code: new `kScanOrder[3] = {11, 1, 6}` and `kRescanMs` constants in
`LumeMode`. Scan-rotation logic walks the order array. Scanning
condition swapped from `no_signal_` (3 s) to a fresh `should_rescan`
derived from `age_since_rx > kRescanMs`.

## 2026-05-16 — Protocol v2: magic prefix for ESP-NOW disambiguation

Wire-incompatible bump from protocol version `0x01` to `0x02`.

- Frames now carry a two-byte magic prefix `0x4E 0x4E` (ASCII "NN")
  at offset 0..1, before the existing `protocol_version` byte.
- Header grew from 6 to 8 bytes; all other offsets shift +2.
  `kHeaderSize = 8`, `kMaxPayloadSize = 24`.
- Receivers validate the magic prefix as the very first check and
  silently drop anything that isn't "NN" before touching the rest
  of the header. New `DecodeResult::InvalidMagic` distinguishes
  "not a NocturNation frame at all" from "wrong NocturNation
  version" for diagnostics.

Motivation: NocturNation shares the 2.4 GHz band with anything else
running ESP-NOW vendor action frames on the same channel - a real
concern at event-density deployments like EMF Camp. The previous
`protocol_version`-only check was a single-byte filter, with a
false-positive rate on the order of one in a few million random
ESP-NOW frames - rare but visible as occasional stray flashes in
busy RF environments. The two-byte magic drops the false-positive
rate to roughly one in a few billion, comfortably below the noise
floor.

v1 and v2 receivers cannot interoperate. Director + Lume must be
flashed in lockstep. Spec change is documented in
`docs/manuals/protocol-manual.md` (v0x02) and synced to the
canonical Notion source-of-truth page.

## 2026-05-16 — Protocol trim and Lume power optimisation

ESP-NOW protocol surface trimmed to the two message types the
deployed firmware actually uses, per architecture specification
v0.29 §4.3. `HEARTBEAT` (0x00) gains a 9-byte payload — `tick: u32`
(monotonic Director uptime), `days_since_2026: u16`, and
`centiseconds_today: u24`, all little-endian — folding the
former `TIME_SYNC` Tier 3 wall-clock anchor into the
already-broadcast liveness frame. Five message types that were
defined in earlier spec revisions but never carried real deployment
traffic are removed from the wire: `BEAT_DETECTED` (0x01),
`MODE_CHANGE` (0x02), `CLOCK_SYNC` (0x04), `TIME_SYNC` (0x05) and
`MUSIC_EVENT` (0x06). Their numeric code points stay
**reserved (do not reuse)** so a future revision can revive
equivalent semantics under fresh IDs without colliding with
historical wire traces. Directors no longer emit the removed types;
Lumes silently discard unknown types per the spec forward-compat
rule.

Alongside the wire-level trim, DROP / BREAKDOWN effect rendering
is removed entirely — there is no `LIGHT_COMMAND`-based replacement
in v0.29. `DropDetector` continues to run inside the Director's
analyser (it still stamps `AudioFrameEvent::music_event`) but its
output has no consumer in the reference firmware; revival would
need a real local consumer or a fresh wire code point.

Lume power optimisation: the main loop now yields with `delay(1)`
once per pass when there is no inbound frame to process, which
lets the ESP32 enter modem-sleep between WiFi-rx callbacks and
measurably reduces battery draw on backgrounded Lumes during quiet
passages.

## 2026-05-16 — Director / Lume vocabulary rename

Codebase, comments, log strings, UI labels, tests, and repo
documentation migrated from the legacy *master* / *slave* role
vocabulary to the project's canonical **Director** (the upstream
node that listens to music, runs audio analysis, and broadcasts
events) and **Lume** (any downstream device that listens to a
Director and turns events into light). See architecture
specification §17 glossary for the canonical definitions and the
brand-identity page for the broader rationale.

The rename shipped as branch `rename/director-lume-vocabulary` in
block-per-commit chunks (Block 1 class renames, Block 2 enum +
identifier renames with a follow-up for compound `master_` /
`slave_` forms, Block 3 comments + log strings, Block 4 UI strings,
Block 5 test names, Block 6 repo documentation, Block 7 final
sweep + this note). Native test suite remained green at every block
commit; both firmware envs (`m5stack-stickcplus2`, `m5stack-stickcs3`)
build clean.

Deliberately not changed:

- **NVS keys** (`slv_chan`, `slv_repeat`, `slv_ir_grp`, `slv_group`,
  `mst_chan`). Persistent operator state on deployed devices.
  Renaming would silently wipe configuration on first boot post-flash.
  Renamed helper functions (`load_lume_group`, `save_lume_channel`,
  etc.) wrap the unchanged key strings.
- **ESP-NOW wire-protocol field names** (`target_class`,
  `target_group`, `source_id`, message-type byte values). External
  ABI; receivers in the field decode against this format.
- **PixMob protocol terminology** (group ID, group select). Upstream
  vocabulary from `jamesw343/PixMob_IR`, not ours.
- **Test fixture directory** (`test/test_autonomous_master_overlay/`)
  and its PlatformIO env (`native_master_overlay`). External test
  invocation patterns reference these.
