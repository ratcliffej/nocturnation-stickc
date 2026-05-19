# Changelog

Notable changes to the NocturNation M5 firmware. Newest first.

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

- [docs/manuals/operator-workflow.md](docs/manuals/operator-workflow.md)
  - operator-facing guide for running Performance Mode deployments,
  source_id verification, residual risk discussion.
- [docs/manuals/protocol-manual.md §3.4](docs/manuals/protocol-manual.md)
  - normative spec for the partition + TOFU rules.
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
