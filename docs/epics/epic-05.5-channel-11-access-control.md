---
title: "Epic 5.5: Channel 11 access control (source_id partition + TOFU)"
status: Proposed → In progress (planning, 2026-05-17)
notion_url: https://www.notion.so/363bd067740581e3bbe1fb077dcb853f
notion_id: 363bd067740581e3bbe1fb077dcb853f
notion_status: Proposed
last_synced: 2026-05-17
sync_direction: local-canonical-during-implementation
---

# Epic 5.5: Channel 11 access control (source_id partition + TOFU)

> **Working copy.** This is the local canonical Epic 5.5 document during active implementation. Implementation blocks (B1–B8) and the progress log live at the bottom of the file. The Notion page is treated as out-of-date for the duration; when the Epic is Done, this file is synced back to Notion as the final state. This avoids Notion API churn (and the resulting timeouts) during the implementation phase.

## Related Documents

- [NocturNation Architecture Specification](https://www.notion.so/357bd0677405800b891beab0f4e0a976) - particularly §4.3 (frame header, source_id field), §4.5 (two-channel architecture, dual-channel scan), §16 (security model overview)
- [Epic 5: Tildagon receiver app (functional)](epic-05-tildagon.md) - the parent Epic this extends. Epic 5 delivered the functional Tildagon Lume; Epic 5.5 hardens its channel 11 listening posture before the app goes public.
- [Epic 6: NocturNation public launch (EMF 2026)](https://www.notion.so/358bd06774058159916fed66a3f3aaf4) - **Epic 5.5 is a release-blocker for Epic 6.** The Tildagon app cannot ship to the EMF store with channel 11 listening posture until Epic 5.5 lands. Once an app is in the store with hundreds of badges installed, retroactively changing the security model is impractical - app updates are slow and not universally installed. The decision has to be made before submission.
- [Security architecture (RFC)](https://www.notion.so/358bd0677405817b8a60de0834511ce5) - the longer-term tiered security design. Epic 5.5 explicitly does not implement Tier 1+ crypto; it lands a non-cryptographic protection appropriate for the EMF threat model.

## Goal

Provide lightweight protection for channel 11 (Performance mode) against the most common failure mode at EMF and similar tinkerer-heavy events: someone unknowingly enabling Director mode on channel 11 mid-show, or curious tinkering with their own M5 Stick during a curated performance. The protection uses the existing `source_id` field in the frame header (transport-independent, already in v1 protocol) plus Trust-On-First-Use locking on the Lume side. No cryptography, no key entry, no UI for the festival-goer, no governance burden.

This Epic deliberately does **not** attempt cryptographic protection. Determined attackers reading the open-source firmware can defeat any non-crypto scheme; for the EMF threat model that's acceptable. Crypto is a future Epic if commercial deployments emerge that justify the complexity.

## Business Value

Epic 5.5 is the release-blocker for the project's public launch. The published Tildagon app creates a *deployed listening posture* that is hard to change retroactively. Once hundreds of badges are in the wild with v1 of the app, the security model they shipped with is the security model they have - app updates are slow and not universally installed. Decisions about channel 11 protection have to be made *before* the app ships, not after.

Without this Epic:
- A curious EMF attendee enabling Director mode on their M5 Stick mid-NullSector-show competes with the curated performance. Every Tildagon in radio range locks to whoever's heartbeat arrives first.
- NullSector cannot reliably deploy NocturNation as part of a curated performance, because the audience's badges have no defence against unintentional disruption.
- Channel 11 becomes effectively unusable for performances, undermining the whole two-channel architecture (§4.5).

With this Epic:
- 95%+ of accidental disruption is prevented automatically.
- NullSector can deploy NocturNation as part of a curated performance with confidence the audience badges will lock to the intended Director.
- Channel 11 fulfils its design intent without imposing UX burden on festival-goers or governance burden on the project.

**Honest commercial framing**: this scheme deliberately does not establish a NocturNation-controlled venue allowlist or a commercial moat. Anyone running the open-source firmware can be a Performance Mode Director - their source_id is randomly allocated, no registration required. The commercial story for NocturNation lives in services (installation, calibration, custom hardware, per-event show programming, brand licensing, integration work), not in protocol-level gatekeeping.

## Scope

### source_id partition (protocol)

The existing 1-byte `source_id` field (§4.3) is partitioned into two ranges plus the existing broadcast:

- `0x00 - 0x3F` (64 slots) - **Community / hobby**. Used on channel 1. Director picks a stable ID at first boot from this range (random, persisted to NVS) and reuses it across reboots. Suitable for hackspace gigs, personal use, ongoing community deployments where the same Director comes back repeatedly.
- `0x40 - 0xFE` (191 slots) - **Performance mode**. Used on channel 11. Director picks a random ID at every boot. Listen-before-broadcast for ~1 second to detect collisions with other concurrent Directors on the same channel; re-roll if a collision is heard. Collision probability with 191 slots and 3 concurrent Directors is ~1.6% chance of any pair colliding, dropping to near-zero with the listen-before-broadcast check.
- `0xFF` - **Broadcast** (unchanged, existing reserved).

The partition is declared in spec §4.3 (this Epic adds the prose). No wire-format change - just a convention about how the existing field is allocated.

### Director-side behaviour

- **On channel 1 (hobby)**: at first boot, pick a random ID from `0x00-0x3F`, persist to NVS, reuse on subsequent boots. Stable per device so returning Lumes recognise the same Director across reboots.
- **On channel 11 (Performance mode)**: at every boot, pick a random ID from `0x40-0xFE`. Listen for ~1 second before broadcasting; if a heartbeat with the same ID is heard, re-roll. If still colliding after 3 attempts, log a warning and proceed with the third pick (collision is theoretically possible but operationally extremely rare).
- Director's chosen source_id is shown prominently on the Director's screen (M5 Stick UI). The operator can verify which Director the audience locks to by comparing the displayed ID to what Lumes report.
- No UI for manually setting the ID. The randomness is the point; manual configuration would invite collisions and accidental disruption.

**Tildagon Director-mode constraint**: the Tildagon can listen on channel 11 in Lume mode (joining Performance mode performances), but when Director mode is added to the Tildagon (planned for a later Epic), the Tildagon must NOT broadcast on channel 11. Tildagon Directors are restricted to channel 1 (community / hobby) for transmission. Rationale: the Tildagon is a community badge widely distributed in the wild, and keeping it off channel 11 as a transmitter protects the integrity of curated performances at EMF. This restriction is conservative and can be revisited in a future Epic once the access control model has matured in deployment.

### Lume-side behaviour

**Trust-On-First-Use (TOFU)** is the pattern where, on first contact with a previously-unknown peer, the receiver implicitly accepts that peer and then refuses anyone else for the duration of the session. Familiar from SSH host keys: the first time you connect to a new server, the client accepts its key blindly; thereafter it refuses to connect if a different key shows up under the same hostname. The Lume applies the same idea to source_ids on a given channel.

- **Channel scan** (existing dual-channel scan from §4.5) finds heartbeats on either channel.
- **TOFU lock**: the first valid heartbeat heard locks the Lume to that source_id for the session. All subsequent traffic from other source_ids is ignored. Lock persists until:
  - Heartbeat timeout (no heartbeat from locked source_id for N seconds, suggesting Director gone) - resume scan
  - Reboot - lose lock, resume scan
  - User-initiated rescan (Config menu option) - manually relock
- **Cross-range filtering**: on channel 11, Lumes only TOFU-lock to source_ids in the Performance range (`0x40-0xFE`). A heartbeat with a hobby-range source_id on channel 11 is ignored (defends against a misconfigured Director). On channel 1, Lumes accept any source_id (community is permissive by design).
- The locked source_id is shown on the Lume's UI where available (Tildagon screen, M5 Stick screen). Audience members can verify they're locked to the expected Director.

### Operator workflow

A short doc in the repo (`docs/operator-workflow.md`) covering:

- How to run a Director in Performance Mode (channel 11)
- Why the source_id is random and what that means operationally
- How to verify audience Lumes are locked to your Director (visual check)
- What to do if you see a competing Director on the same channel (operational coordination, not a technical defence)
- The honest residual risk: a Lume powering on after a tinkerer but before NullSector locks to the tinkerer first. Mitigation: boot Director before audience arrival; offer rescan UI.

## Acceptance Criteria

- [ ] Spec §4.3 updated with source_id partition (`0x00-0x3F` community, `0x40-0xFE` Performance mode, `0xFF` broadcast)
- [ ] M5 Stick Director firmware: stable community-range ID on channel 1, random performance-range ID on channel 11, listen-before-broadcast collision check
- [ ] M5 Stick Director firmware: source_id visible on Director's screen during operation
- [ ] Tildagon Lume app: TOFU lock to first valid heartbeat, cross-range filtering on channel 11, heartbeat-timeout-triggers-rescan
- [ ] Tildagon Lume app: locked source_id visible in app UI
- [ ] Tildagon Lume app: manual rescan option in Config menu
- [ ] Operator workflow doc in repo
- [ ] Test rig: two M5 Stick Directors on channel 11 simultaneously, verify Lumes lock to one and reject the other
- [ ] Test rig: hobby-range source_id broadcasting on channel 11, verify Lumes ignore it
- [ ] Honest documentation of the residual risk (first-mover lock, no crypto protection against determined attack)

## Order of work

1. Spec update (§4.3 source_id partition). This is the contract everything else implements against.
2. M5 Stick Director firmware changes (random performance-range ID, listen-before-broadcast, source_id display).
3. Tildagon Lume app changes (TOFU lock logic, cross-range filtering, source_id display, manual rescan).
4. Test rig validation (two Directors, hobby-range-on-channel-11 case).
5. Operator workflow documentation.

Most work is on the Tildagon Lume side (TOFU logic + UI). M5 Stick changes are smaller.

## Dependencies

| Dependency | Type | Status | Owner |
|---|---|---|---|
| Architecture spec §4.3 (frame header, source_id field) | Internal | Done (existing, v0.29) | Jason |
| Architecture spec §4.5 (two-channel architecture, dual-channel scan) | Internal | Done (existing) | Jason |
| Epic 5 (functional Tildagon receiver app) | Internal | Done (v0.1 MVP milestone) | Jason |
| Epic 6 (public launch / Tildagon app submission) | Internal blocking | Epic 5.5 blocks Epic 6 - app cannot ship to store without channel 11 access control resolved | Jason |

## Target Sprint Range

- **Start sprint:** Immediately - this is on the EMF 2026 critical path because Epic 6 cannot ship without it.
- **End sprint:** 1-2 sprints later - mechanical work, no crypto, no governance, no infrastructure.
- **Indicative complexity total:** 3-5 points (small Epic).

## Status Notes

Originally scoped as "Epic 6.5" on 2026-05-17. Renumbered to **Epic 5.5** shortly after when it became clear this is a release-blocker for Epic 6 rather than a sub-Epic of it - the Tildagon app cannot go public without this work being complete, so it logically sits before Epic 6 in the dependency chain, alongside Epic 5 which it extends.

Proposed 2026-05-17 after extended design conversation working through the channel 11 protection problem. Several options were considered and rejected:

- **Crypto with public/private keys**: requires Ed25519 support on Tildagon MicroPython (not available in current build); pure-Python implementation too slow; HMAC with PSK fails because keys are public via open source. All forms of crypto run into either the no-UI constraint (festival-goers won't enter keys) or the open-source constraint (firmware-embedded keys are extractable).
- **MAC-based filtering**: transport-dependent (ESP-NOW only), wouldn't work for future IR / BLE / LoRa carriers. Rejected on architectural grounds.
- **Venue allowlist with registered source_ids**: introduces governance burden (who maintains the JSON file, who reviews PRs, what if a venue's source_id leaks) without meaningfully stronger protection than random allocation.

The chosen approach (random allocation + TOFU) is honest about its threat model: it protects against accidental disruption and casual tinkering, not against determined attacks. For the EMF 2026 deployment context this is the right trade-off.

**Commercial concession captured explicitly**: the random-allocation scheme removes the *technical* commercial moat the project could have built (controlled venue allowlist with NocturNation-issued source_ids). This is a deliberate choice. The commercial story lives elsewhere (services, hardware, brand) where it belongs. Worth being clear about so future contributors don't re-debate the question.

**Honest residual risk**: a Lume powering on after a tinkerer but before the intended Director locks to the tinkerer. Mitigation is operational (boot Directors before audience arrival) rather than technical. A future Epic with proper crypto could close this gap if commercial deployments warrant it. Until then, the operational discipline is appropriate to the threat model.

---

## Implementation blocks

Approved chunking for Claude Code processing (2026-05-17). Each block is a self-contained session producing one PR with tests, and (where applicable) hardware verification by Jason. Critical path runs B1 → B2 → {B3, B4a, B4, B5} on the M5 side and B2 → {B6, B7} on the Tildagon side, both feeding into B8.

### B1 — Spec: source_id partition in protocol-manual.md

**Repo:** StickC. **Deps:** none.

Scope:
- Add a subsection to `docs/manuals/protocol-manual.md` (§4.x; pick the right home — likely under "Frame header" / "source_id field" sections) documenting the partition: `0x00-0x3F` community (64 slots), `0x40-0xFE` Performance mode (191 slots), `0xFF` broadcast.
- Document the channel-to-range binding: channel 1 = community range (stable per device), channel 11 = Performance range (random per boot + listen-before-broadcast).
- Note the Tildagon Director-mode-must-not-broadcast-on-ch-11 constraint with rationale, marked as conservative and revisitable.
- Sync to Notion via `replace_content` (covers fenced-code / table edge cases).

Acceptance criteria:
- §updated; renders correctly in both GitHub markdown and Notion.
- Local committed and pushed.
- Notion page reflects new content.

### B2 — Protocol constants + helpers (both repos)

**Repos:** StickC + Tildagon, paired session. **Deps:** B1.

Scope:
- M5 C++ side (StickC repo): add named constants in `include/transport/espnow/` (existing `protocol.h` or new `source_id.h`):
  - `kSourceIdCommunityMin = 0x00`, `kSourceIdCommunityMax = 0x3F`
  - `kSourceIdPerformanceMin = 0x40`, `kSourceIdPerformanceMax = 0xFE`
  - `kSourceIdBroadcast = 0xFF`
  - `inline bool is_community_range(u8 id)`, `inline bool is_performance_range(u8 id)`
- Tildagon Python side: equivalent module (suggest `apps/nocturnation/nocturnation/source_id.py` or extend an existing protocol module) with the same constants + helpers.
- Native unit tests on both sides asserting boundary values and helper correctness (in / out / boundaries / broadcast).

Acceptance criteria:
- Constants present on both sides with matching values.
- Helpers present with consistent semantics.
- Native tests pass on both sides (`pio test -e native` for M5; `pytest tests/` for Tildagon).
- No behaviour change yet — pure declarations + tests.

### B3 — M5 Director: community-range stable ID on channel 1

**Repo:** StickC. **Deps:** B2.

Scope:
- Add NVS key `mst_src_id` under the `noct` namespace. Stores a u8.
- On Director-mode entry with `mst_chan == 1`: load `mst_src_id`. If absent (first boot post-flash), roll random in `[0x00, 0x3F]` and persist before broadcasting.
- Wire the resulting ID into the `source_id` field of HEARTBEAT and LIGHT_COMMAND frames.
- Native tests covering: first-boot roll-and-persist, subsequent-boot reuse, range validation, NVS-load fallback when corrupted.

Acceptance criteria:
- New NVS key documented in `protocol-manual.md` Annex B (NVS schema).
- First-boot test: roll lands in `[0x00, 0x3F]` and gets persisted.
- Subsequent-boot test: persisted value reused, no re-roll.
- Wire trace test (or native equivalent): emitted HEARTBEAT carries the chosen ID.
- `pio run -e m5stack-stickcplus2 -e m5stack-stickcs3` clean.
- 284+ existing tests still pass.

### B4a — M5 Director: ch-11 collision-check design sketch (pre-implementation)

**Repo:** StickC. **Deps:** B2, B3.

Scope (research session, no production code):
- Read the existing Director-mode boot sequence and ESP-NOW receive path (`src/modes/master_mode.cpp` or equivalent, `src/transport/espnow/`).
- Document where in the boot sequence the 1-second listen window goes — specifically: at what point is the radio in RX-capable state, when does TX get armed, where does the existing main-loop tick start emitting HEARTBEATs.
- Sketch the state machine for: "pick ID → listen 1 s → if collision heard, re-roll → after 3 attempts, log warning and proceed".
- Identify how the receive callback delivers HEARTBEATs to the Director (Director-mode normally doesn't *receive* HEARTBEATs; this Epic adds that path) and whether a temporary subscription is needed.
- Output: a "Block notes / B4a" section appended to this file with the design, ready for Jason sign-off before B4 starts.

Acceptance criteria:
- Design note added to "Block notes" section below.
- Jason signs off (records "approved" in the Progress log) before B4 starts.

### B4 — M5 Director: random Performance-range ID + listen-before-broadcast on channel 11

**Repo:** StickC. **Deps:** B2, B3, B4a (signed off).

Scope:
- Implement the state machine from B4a's design note.
- On Director-mode entry with `mst_chan == 11`: roll random in `[0x40, 0xFE]`. Hold TX disabled. Listen for ~1 second for any HEARTBEAT carrying that ID. If detected, re-roll. Max 3 attempts; on third collision, log warning and proceed with attempt 3.
- Chosen ID is not persisted (per-boot only).
- Wire into HEARTBEAT + LIGHT_COMMAND source_id.
- Native tests via synthetic inbound HEARTBEAT injection covering: no-collision path, one-collision-then-clear, three-collisions-warning.

Acceptance criteria:
- Behaviour matches the B4a design.
- Three test paths covered.
- `pio run -e m5stack-stickcplus2 -e m5stack-stickcs3` clean.
- All existing tests still pass.

### B5 — M5 Director: show source_id on screen

**Repo:** StickC. **Deps:** B3, B4.

Scope:
- Display the active source_id in Director-mode UI with mode prefix: `ID: C:03` for community 0x03, `ID: P:4F` for Performance 0x4F.
- Visible during normal Director Mode operation and Test Mode.
- No layout regression for other screen elements (group, channel, status).

Acceptance criteria:
- Source_id visible on screen.
- Format unambiguous (community vs Performance immediately readable).
- Bench-verified by Jason on both `stickcplus2` and `stickcs3` hardware.
- Existing UI tests pass; new test for the formatter if practical.

### B6 — Tildagon: TOFU lock + cross-range filtering

**Repo:** Tildagon. **Deps:** B2.

Scope:
- TOFU state machine in the Lume's frame-observation path. On the first valid HEARTBEAT post-dedup, record its `source_id` as the locked peer. Subsequent frames with a different `source_id` are dropped before fan-out to renderers.
- Cross-range filtering: on channel 11, only Performance-range IDs (`0x40-0xFE`) are eligible to be locked; hobby-range IDs on ch 11 are dropped without locking. Channel 1 accepts any source_id.
- Heartbeat timeout: reuse `kRescanMs = 10000` (10 s) as the lock-expiry threshold (decoupled from `kNoSignalMs = 3000`'s display purpose). On expiry: clear lock, resume scan.
- Pytest coverage: first-lock; second-source-id rejected; ch-11-with-hobby-id ignored; ch-1-with-hobby-id locks; timeout clears lock; reboot clears lock (implicit — fresh state).

Acceptance criteria:
- State machine implemented per spec above.
- 108 existing tests still pass; new tests cover the five paths.
- No regression to channel scan behaviour from Epic 5.

### B7 — Tildagon: UI for locked ID + Rescan menu item

**Repo:** Tildagon. **Deps:** B6.

Scope:
- Display the locked source_id on the LCD when locked (`C:nn` / `P:nn` format, matching M5). Suggested placement: corner, small font, doesn't conflict with NO SIGNAL overlay.
- When unlocked / mid-scan: no ID display (or "Scanning…" — implementer's call, document in PR).
- Config menu: add "Rescan" item before "Back". Selecting it clears the TOFU lock and triggers immediate channel scan.
- New pytest coverage for the menu item behaviour (selecting "Rescan" → state cleared).

Acceptance criteria:
- Locked ID visible on hardware when locked; hidden / replaced on unlock.
- Rescan menu item works: pressing it from a locked state returns the Lume to scanning behaviour.
- Existing tests + new tests pass.
- Bench-verified by Jason on hardware.

### B8 — Bench validation + operator workflow doc + CHANGELOGs

**Repos:** both + StickC docs. **Deps:** B3–B7.

Scope:
- Hardware integration tests:
  - Two M5 Sticks set to Director mode on channel 11 simultaneously. Tildagon locks to whichever HEARTBEAT it sees first; second Director's frames are ignored.
  - One M5 misconfigured (via a test-only build flag or temporary code patch) to emit a hobby-range source_id on channel 11. Tildagon ignores entirely.
- Create `docs/operator-workflow.md` (StickC repo) covering Performance Mode operations, source_id verification visually, what to do on competing Director observed, residual risk, recommended pre-show boot timing. Sync to Notion.
- CHANGELOG entries on both repos summarising the Epic 5.5 change (wire-compat preserved, new behaviour at the partition level).
- Update Notion page status: Proposed → In Review → Done as appropriate, and push the final body of this working-copy file back to Notion (replace_content with the full content of this file's "Goal" → "Status Notes" sections, minus the "Implementation blocks" / "Progress log" / "Block notes" sections which stay local).

Acceptance criteria:
- Two bench scenarios verified; results recorded in the Progress log below.
- `docs/operator-workflow.md` committed and Notion-synced.
- CHANGELOG entries on both repos.
- Notion Epic 5.5 page reflects final state; status updated to Done.

---

## Progress log

Append-only. One entry per state change (block start / block done / decision / bench result / blocker).

- **2026-05-17** — Epic decomposed into 8 blocks (B1–B8) plus B4a design pre-pass. Local working copy created in `docs/epics/`. Notion treated as out-of-date until Epic Done. Next: B1 (spec update).
- **2026-05-17 — B1 done.** New §3.4 "Source identifier partitioning" added to [docs/manuals/protocol-manual.md](../manuals/protocol-manual.md) (commit `5ea160f`). Director-side allocation rules, Lume-side TOFU + cross-range filter, and Tildagon Director-mode constraint all in §3.4. Smaller cross-references: §3.1 header table source_id row, §5.1 Director, §7.1 Receiver MUST (+2 bullets), §7.5 Director MUST (+2 bullets), Annex D non-wire-convention note. Notion mirror synced via `replace_content`. No wire-format change; protocol_version stays at `0x02`. Next: B2 (protocol constants + helpers, both repos paired).
- **2026-05-17 — B2 done.** Paired commits land the partition constants and `is_community_range` / `is_performance_range` predicates on both codebases. StickC: `kSourceIdCommunityMin/Max`, `kSourceIdPerformanceMin/Max` added to [include/transport/espnow/frame.h](../../include/transport/espnow/frame.h) with constexpr predicates; 3 new tests in `test_espnow_frame`; 27/27 native_espnow tests green; both firmware envs build clean (commit `54c199f`). Tildagon: new module `nocturnation/protocol/source_id.py` with `SourceId` class + free functions; re-exported from `nocturnation.protocol`; 22 new pytest cases (including a 256-byte exhaustive partition sweep); 130/130 host tests green (was 108) (commit `ef7bda3`). No behaviour change yet; first consumers land in B3 (M5 Director community-range stable ID on ch 1). Next: B3.
- **2026-05-17 — B3 done.** Director-mode broadcast on channel 1 now uses a stable community-range source_id loaded from NVS (key `mst_src_id` under `noct` namespace). First-boot pick happens inside `migrate_legacy_nvs_keys` (random in `[0x00, 0x3F]`, persisted); subsequent boots reuse. Channels 6 and 11 retain the legacy MAC-derive behaviour - B4 replaces ch 11 with the Performance-range path. Code: [src/modes/persistence.{h,cpp}](../../src/modes/persistence.cpp) gains `load_director_source_id` / `save_director_source_id` + first-boot roll + native test seam (`set_first_boot_director_src_id_rng`, `plant_raw_director_src_id`); [src/dal/drivers/espnow_broadcast_driver.{h,cpp}](../../src/dal/drivers/espnow_broadcast_driver.cpp) `derive_source_id` grows a channel parameter and dispatches to persistence on ch 1, made public for direct unit testability, plus a public `source_id()` getter for the B5 UI. Spec: [protocol-manual.md](../manuals/protocol-manual.md) Annex B gains the `mst_src_id` key row; Notion synced via `replace_content`. Tests: 6 new cases in `test_output_binding_concrete`; 354/354 native tests green; both firmware envs build clean (commit `c98aa42`). Followed by env updates to seven native test envs that linked DAL without persistence (the driver's new persistence dep cascaded). Next: B4a (collision-check design pre-pass).
- **2026-05-17 — B4a done (research-only).** Channel-11 listen-before-broadcast design captured in [Block notes § B4a](#b4a--channel-11-listen-before-broadcast-design-signed-off-2026-05-17). Driver-level state machine (Idle → Listening → Active), 1-second RX-only window enforced via `active_` gate (no HAL primitive for RX-only mode), single-slot recv callback during listen, up to 3 attempts then warn-and-settle. Three sign-off questions resolved: Serial-only warning surface, listen happens on ch 11 directly, channel 6 stays MAC-derive. Ready for B4 implementation.
- **2026-05-17 — B4 done.** Implements the B4a design. [src/dal/drivers/espnow_broadcast_driver.{h,cpp}](../../src/dal/drivers/espnow_broadcast_driver.cpp) gains `StartupState` enum (Idle/Listening/Active), `kListenWindowMs = 1000`, `kListenMaxAttempts = 3`, `pick_performance_id_random` (esp_random in production / queued deterministic in native), `listen_tick`, `on_listen_recv`, and `log_listen_collision_warning`. `start_broadcast(11)` installs the listen-only recv callback, brings the radio up RX-capable, and leaves `active_=false`; `loop_tick` short-circuits during Listening; `listen_tick` settles into Active after the 1-s window with re-roll on heard collision. `stop_broadcast` cleans up recv callback + state from any phase. Public test seam: `test_seam::set_now_ms` (replaces the always-0 native shim), `test_seam::queue_next_performance_pick`, `driver->test_enter_listening` (bypasses radio for native testing), `driver->test_inject_listen_heartbeat`. Five new tests in `test_output_binding_concrete`: no-collision settle, one-collision-then-clear, three-collisions-with-warning, non-matching-heartbeat-ignored, stop_broadcast-mid-Listening cleanup. 359/359 native tests green; both firmware envs build clean (commit `9981020`). B3 + B4 together complete the Director-side AC for the Epic. Next: B5 (display source_id on the Director's screen).
- **2026-05-17 — B5 done (pending bench verification).** Source_id status label drawn bottom-right on the M5 Stick screen in Director Mode (overlaying the active Show) and Test Mode (overlaying the test menu). Format: `C:nn` for community-range IDs, `P:nn` for Performance, `P:nn?` with a tentative-suffix during Listening, `?:nn` for defensive out-of-range. Driver gains a public `listen_candidate()` getter and a pure-function static `format_status_label(state, source_id, listen_candidate, buf, buflen)` with defensive truncation (too-small buffer → empty, never partial). Seven new unit tests in `test_output_binding_concrete`. 366/366 native tests green; both firmware envs build clean (commit `fed3678`). Bench verification (Jason): confirm the label renders bottom-right on both stickcplus2 and stickcs3 hardware in Director Mode and Test Mode. Next: B6 (Tildagon TOFU lock + cross-range filtering).
- **2026-05-17 — B6 done.** Tildagon-side TOFU lock + cross-range filter implemented in new `apps/nocturnation/nocturnation/tofu.py` (`TofuLock` class with `admit(frame, channel, now_ms)`, `tick(now_ms)`, `clear()`). Default timeout 10 s mirrors the M5 firmware's `kRescanMs`. Wired into both receive entry points in `app.py` (`_attempt_scan` to gate the channel-lock decision, `_receive_loop` to gate observation + tick the timeout). Pre-B6 signoff resolved a spec/operational tension: the spec's literal "first valid HEARTBEAT" rule didn't compose with §6.1 skip-if-recent (heartbeats are suppressed during active music, so a Lume joining mid-song would never lock). Relaxed to "first valid frame" and updated [docs/manuals/protocol-manual.md](../manuals/protocol-manual.md) §3.4 + §7.1 to match (StickC commit `69d9820`, Notion synced via `update_content` since edits were prose-only). Tildagon commit `8670c51` adds 22 new pytest cases (initial state, first-lock, post-lock filter, ch 11 cross-range, ch 1 / ch 6 permissive, timeout expiry, clear(), reboot-as-implicit-clear); 152/152 host-side tests pass (was 130). Next: B7 (Tildagon UI for locked ID + Rescan menu item).
- **2026-05-17 — B7 done (pending bench verification).** Tildagon LCD label now shows TOFU lock state using the same `C:nn` / `P:nn` convention as the M5 firmware. States: `ch N scan` (channel hunting), `ch N listen` (channel locked but no TOFU peer), `ch N C:nn` / `ch N P:nn` (TOFU locked, community or Performance range), `ch N ?:nn` (defensive out-of-range). Composed by new pure-function `format_lock_label` in `tofu.py` (5 new pytest cases). Settings menu: new "Rescan" item between Channel and Back; on select, calls `self._tofu.clear()` so the next valid frame on the current channel establishes a fresh TOFU lock. Tildagon's radio doesn't support reliable channel re-scan post-boot (Epic 5 Q6) so this is a TOFU-only reset. Tildagon commit `5ab9333`; 157/157 host tests pass (was 152). Bench-verify by Jason: confirm the label updates on lock/timeout/clear; confirm the Rescan menu item resets TOFU and the next Director frame relocks. Next: B8 (bench validation + operator workflow doc + CHANGELOGs).
- **2026-05-17 — B8 done (Epic implementation complete; bench-verification pending).** New [docs/manuals/operator-workflow.md](../manuals/operator-workflow.md) captures the operator-facing side: channel selection, Performance Mode operations, source_id verification by visual comparison, pre-show checklist, during-show spot-checks, residual-risk discussion, and the channel-1 social contract framing. Notion page created at [notion.so/365bd067740581bbace6c5ac7b2c0339](https://www.notion.so/365bd067740581bbace6c5ac7b2c0339) with bidirectional sync frontmatter. CHANGELOG entries added to both repos (StickC commit `24d9744`, Tildagon commit `3163060`) summarising the full Epic: partition table, Director/Lume behaviour, screen UI, spec-deviation note, new docs, honest non-cryptographic threat model. Notion Epic page synced via `replace_content` with the final body — AC checkboxes ticked for everything except the two bench scenarios, "Implementation complete 2026-05-17" status note appended, and the local-only Implementation Blocks / Progress Log / Block Notes sections deliberately excluded from the Notion mirror. **Epic implementation complete.** Two bench-verification scenarios remain for Jason on hardware: (1) two M5 Sticks in Director mode on ch 11 simultaneously, verify Tildagon locks to first-arriving HEARTBEAT and ignores the second; (2) one M5 misconfigured to emit a hobby-range id on ch 11 (test-only build flag), verify Tildagon ignores entirely. Across both repos: 366 M5 + 157 Tildagon = 523 native tests pass; both firmware envs build clean; no wire-format change (protocol_version stays 0x02).

---

## Block notes

Optional design sketches, decisions, and gotchas surfaced during implementation. Some blocks (e.g., B4a) explicitly produce content here. Others may add notes if non-obvious decisions are made.

### B4a — channel-11 listen-before-broadcast design (signed off 2026-05-17)

**What I read.** `src/modes/director_mode.cpp:94` — Director startup is shows-enter → audio-input → `start_broadcast(channel)` → draw, with the driver bringing the radio up synchronously. `src/dal/drivers/espnow_broadcast_driver.{h,cpp}` — `active_` gates all `send_broadcast`; `loop_tick()` drives retransmits + heartbeat; `start_broadcast` currently does pick-id + `radio->begin` + `active_=true` atomically. `include/hal/hal.h:225-242` — `ESPNow::begin(channel)` brings WiFi STA up + pins channel + registers the receive callback; after it returns the radio is **fully bidirectional**. The Director never calls `set_recv_callback(...)` in current code (Lume does). `src/modes/lume_mode.cpp:113-117` — pattern is `set_recv_callback(cb)` then `radio->begin(channel)`; single-slot replace, most recent caller wins. `src/hal_stickcplus2/esp_now_stickcplus2.cpp:37-77` — `begin()` is ~10 ms in practice, recv callback registered via `esp_now_register_recv_cb` at IRAM level.

**Architectural fact that drove the design.** There is no HAL primitive for "RX-only mode" — the radio is bidirectional the moment `begin()` returns. So the listen window is enforced at the driver level: `active_` stays false, `loop_tick()` short-circuits, no `send_broadcast` call happens. The radio is hot for RX during that 1-second window; we just don't send.

**State machine.** Add to `EspNowBroadcastDriver`:

```
enum class StartupState : uint8_t {
    Idle,        // not started
    Listening,   // ch 11 only: 1-s RX-only window, candidate held in listen_candidate_
    Active,      // normal operation
};
```

`active_` is true only in `Active`. `loop_tick()` short-circuits before retransmits / heartbeat when `startup_state_ == Listening`.

**`start_broadcast(channel)` reshape.**

- Channel 1 or 6: unchanged from B3 — `source_id_ = derive_source_id(channel)`, `radio->begin(channel)`, `active_ = true`, state → `Active`.
- Channel 11 (new): pick a random candidate in `[0x40, 0xFE]`, install our own receive callback, `radio->begin(11)`, leave `active_` false, state → `Listening`, record `listen_started_ms_`. Three attempts available.

**Listen receive callback.** Installed only during `Listening`. Decodes inbound header; if `message_type == HEARTBEAT` and `source_id == listen_candidate_`, sets `listen_collision_heard_ = true`. Drops everything else (no renderer side effects — Director Mode has no inbound consumer anyway).

**`listen_tick()` (called from `loop_tick`):**

```
if (now - listen_started_ms_ < kListenWindowMs) return;     // still listening

if (listen_collision_heard_) {
    if (--listen_attempts_remaining_ == 0) {
        // Spec §3.4: after 3 collisions, proceed with attempt 3 + warn
        log_listen_collision_warning();   // Serial only per signoff
    } else {
        listen_candidate_         = pick_performance_id_random();
        listen_collision_heard_   = false;
        listen_started_ms_        = now;
        return;
    }
}
// Settle.
source_id_      = listen_candidate_;
radio->set_recv_callback(nullptr);
active_         = true;
startup_state_  = StartupState::Active;
```

**New constants.** `kListenWindowMs = 1000` (spec §3.4); `kListenMaxAttempts = 3`.

**Random pick.**

```
static uint8_t pick_performance_id_random() {
#ifdef ARDUINO
    return static_cast<uint8_t>(0x40 + (esp_random() % 191));  // [0x40, 0xFE]
#else
    return s_native_performance_pick_seam;
#endif
}
```

**Test seam (B4 work).** `set_next_performance_pick(uint8_t)` controls the next pick; `queue_next_performance_picks({0x4A, 0x4B, 0x4C})` for multi-attempt determinism; `inject_listen_heartbeat(uint8_t source_id)` synthesises an inbound HEARTBEAT into the listen callback. The existing `now_ms()` shim handles synthetic time.

**`stop_broadcast()` reshape.** If called mid-Listening: `set_recv_callback(nullptr)`, reset `startup_state_ = Idle`, then existing `radio->end()` path.

**Affected files for B4.** `src/dal/drivers/espnow_broadcast_driver.{h,cpp}` (state, listen recv cb, listen_tick, pick helper, seams). `test/test_output_binding_concrete/test_main.cpp` — four new cases: no-collision settles to attempt-1; one-collision-then-clear settles to attempt-2; three-collisions logs warning and settles to attempt-3; `stop_broadcast` mid-Listening cleans up.

**Risks / failure modes.**

1. First-tick race: `loop_tick()` runs from the main loop; first call after `start_broadcast(11)` lands in `Listening` and short-circuits. Safe.
2. Radio "warm-up" silence: ~10 ms typical, but the 1-second window is a comfortable margin.
3. All 3 attempts collide: vanishingly improbable per spec; settle + log.
4. Mode switch mid-listen: handled by `stop_broadcast` cleanup.
5. `radio->begin(11)` fails: same as today — `active_=false`, return false; never enter `Listening`.
6. Single-slot recv callback: Director never installs one in steady state today, so no contention. Restored to `nullptr` on settle.

**Signoff decisions.**

1. **3-attempt warning surface**: Serial only for now. Odds of this happening are really low. LCD warning may be a B5 add-on if needed.
2. **Pre-listen channel timing**: listen on the destination channel (ch 11). `radio->begin(11)` pins to 11 and listens, then settles into Active on the same channel.
3. **Channel 6**: leave alone for now. No listen window for ch 6; keeps MAC-derive behaviour.

