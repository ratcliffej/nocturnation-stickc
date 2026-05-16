# Changelog

Notable changes to the NocturNation M5 firmware. Newest first.

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
