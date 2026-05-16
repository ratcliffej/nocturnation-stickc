# Changelog

Notable changes to the NocturNation M5 firmware. Newest first.

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
