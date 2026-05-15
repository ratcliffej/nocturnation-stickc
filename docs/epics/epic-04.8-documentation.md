---
title: "Epic 4.8: User manual and NocturNation protocol manual"
status: In Progress
notion_url:
notion_id:
notion_status: In Progress
last_synced: 2026-05-12
sync_direction: local-only
---

## Related Documents

- [Epic 4.6: Clean architecture and UI polish](epic-04.6-ui-cleanup.md) - established the plug-in surfaces (`Visualisation`, `OutputBinding`) and `render_fx` canonical send that the protocol manual documents.
- [Epic 4.65: Class+group device addressing](epic-04.65-device-addressing.md) - defined the on-wire `target_class` / `target_group` taxonomy; reproduced verbatim in the protocol manual.
- [Epic 4.7: Show plug-in framework and dynamic FFT-driven show](epic-04.7-dynamic-show.md) - added the `Show` plug-in surface, dispatch loopback behaviour, and DynamicShow per-drum routing; user manual covers operator-facing show selection + tuning. (The Epic-4.7 IR reset primer was rolled back during this Epic - see "IR reset primer rollback" below.)
- [NocturNation Architecture Specification](https://www.notion.so/357bd0677405800b891beab0f4e0a976) - the design document. The protocol manual is the externally-publishable distillation of §4 (transport), §5 (analyser), §6 (rendering), §7 (plug-in surfaces).
- [`jamesw343/PixMob_IR`](https://github.com/jamesw343/PixMob_IR) - upstream-of-truth reference for the PixMob IR encoding section.
- [Epic 5: Tildagon receiver app](https://www.notion.so/358bd067740581b19551d158d658df76) - downstream Epic; the protocol manual is its implementation specification.

## Goal

Produce two operator-and-implementer-facing manuals as the public face of the NocturNation project. The architecture spec stays the internal design document; these manuals are what someone arriving at the repo or attending a deployment actually reads.

1. **User manual** - aimed at the operator setting up a NocturNation deployment at a venue. Covers theory of operation (how the system thinks about beats, sections, addressing, redundancy), hardware setup, configuration walk-through, mode/show selection, troubleshooting, and a glossary. Reads top-to-bottom for a newcomer; serves as reference for an experienced operator.
2. **NocturNation protocol manual** - aimed at the implementer building a new receiver (Tildagon, future hosts, third-party reverse-engineering effort). Specifies the ESP-NOW transport, frame formats, class+group addressing, PixMob IR encoding (with upstream attribution), heartbeat semantics, channel discovery, and the firmware-level NVS schema. Reads as a specification with normative MUST/SHOULD language; every wire-visible behaviour is documented to byte level.

This Epic is the prerequisite for Epic 5 (Tildagon receiver) - the protocol manual *is* the Tildagon implementation spec - and for any external contributor or third-party build of a NocturNation node.

## Why this Epic exists

The codebase has reached the point where the protocol surface is stable enough to publish. Across Epics 1-4.7 the ESP-NOW frame format, class+group addressing, PixMob IR encoding, BeatDetector tuning, and dispatch behaviour have all settled. The architecture spec captures them but is bidirectionally synced internal design notes - it talks about *why* decisions were made and includes deferred items, spec deviations, and historical context that an operator or implementer doesn't need.

Three audiences need a cleaner artefact than the architecture spec:

- **Operators** at deployments who want to know what the menu items do, how to recover from "the slaves all went red and stopped flashing", and what the theory of operation is so they can improvise.
- **Implementers** building a new receiver (Tildagon is imminent, third-party MicroPython / ESP32-IDF / Arduino-Pico builds are encouraged) who need a normative spec they can write code against.
- **Visitors** (potential contributors, conference attendees, the curious) who want to understand what the project actually is without reading the architecture document.

The Epic exists now because (a) Epic 4.7 closed the show framework so the renderable surface is stable, (b) Epic 5 (Tildagon) needs a specification to build against, (c) Jason wants the project externally legible before the next deployment.

## Operational model

Laptop-driven, doc-only. No firmware change, no test change. Verification is editorial: a cold reader (Diane-style walk-through) should be able to install firmware on a fresh Plus2 + S3 pair and run a successful deployment using only the user manual; an implementer should be able to write a working ESP-NOW receiver using only the protocol manual.

Verification ownership: **(L)** = laptop / native (build doc, link-check, spell-check), **(R)** = reader test (cold walk-through by Jason or invited reviewer).

## Scope

**Included - User manual** (`docs/manuals/user-manual.md`):

- **Theory of operation** chapter: what NocturNation is (one master + N slaves + crowd-worn IR receivers), why it's distributed (slaves extend IR coverage at large venues), how the master detects beats / sections / drops, why class+group addressing exists, why ESP-NOW (low-latency broadcast, no AP needed), why bracelets are pre-grouped at random and the operator addresses groups not individuals.
- **Hardware setup**: M5StickC Plus2 vs M5StickS3 (capability differences, when to pick which), bracelet pairing model (groups are pre-programmed - the operator doesn't change them), antenna orientation, IR LED line-of-sight notes (Plus2 omnidirectional vs S3 focused beam).
- **Firmware installation**: PlatformIO build, environment names per host, flashing.
- **Configuration walk-through**: every menu item in the Config tree, what it does, when to change it (`Group: N`, `Display`, `Connectivity` (IR / ESP-NOW / WiFi / DMX picker), `Utilities` (PixMob picker), `System`).
- **Modes and shows**: AutonomousMaster vs Slave vs TestMode vs ConfigMode; show picker (Btn2-long); per-show settings UI (Btn1-long).
- **Troubleshooting**: bracelets not responding, NO SIGNAL flag on slave, channel mismatch, ESP-NOW range, IR coverage gaps.
- **Glossary** + **Index**.

**Included - Protocol manual** (`docs/manuals/protocol-manual.md`):

- **Front matter**: scope, normative language (RFC 2119 MUST/SHOULD/MAY), licence (CC BY-SA 4.0), versioning policy (`PROTOCOL_VERSION` byte at offset 0 of every frame; semver mapping in §A).
- **Wireless layer**: ESP-NOW vendor-neutral specification (802.11 vendor action frames, channels 1 / 6 / 11, broadcast MAC `ff:ff:ff:ff:ff:ff`), why no encryption (Tier 0; security RFC referenced for Tier 1 path), redundancy (3× TX), dedup ring (16-deep on sequence number).
- **Frame format**: `LIGHT_COMMAND` (9 bytes: target_class, target_group, r, g, b, attack, sustain, release, chance); `HEARTBEAT` (master-pulse, 1 Hz with skip-if-recent); `MUSIC_EVENT` (drop=1, breakdown=2, build=3 reserved). Byte-by-byte tables; example dumps for each frame.
- **Class+group addressing**: the `DeviceClass` enum (`All=0x00`, `Light=0x01`, `Screen=0x02`, `MultiLedScreen=0x03`, reserved 0x04..0xFF); routing semantics (`(class, group) == (0, 0)` = everything; `(class, 0)` = all of that class; `(0, group)` = all classes in that group; exact match otherwise).
- **PixMob IR encoding** (informative annex): 9-byte frame, attribution to `jamesw343/PixMob_IR`, byte layout, `restrictGroupId & 0x1F`, parity reference vectors.
- **Channel discovery**: master picks channel 1 / 6 / 11 (default 11 = show); slave auto-scans by default with show priority. Sequence of probes documented.
- **NVS schema** (informative annex, M5Stick reference implementation): `last_mode`, `ir_en`, `cal`, `scr_puls_en`, `slv_ir_grp`, `mst_chan`, `slv_chan`, `slv_repeat`, plus per-plug-in namespaces `nv_<id>` / `nb_<id>` / `ns_<id>`.
- **Conformance**: what a receiver MUST honour (dedup, target_class / target_group routing, attack/sustain/release/chance interpretation), what it SHOULD honour (NO SIGNAL display after 3 s heartbeat gap, channel auto-scan), what it MAY support (slave-as-repeater, screen pulse).
- **Reference test vectors** (annex): canonical LIGHT_COMMAND + PixMob IR byte sequences for parity testing.

**Out of scope** (carry-forwards for later Epics):

- Hardware schematics / fabrication docs (lives with the hardware repo when split out).
- Show authoring tutorial (sits between user manual and developer guide; folded into `docs/developing-shows.md` re-flow rather than its own manual).
- Tier 1 security spec (separate Notion RFC; protocol manual references it but does not include it).
- DMX protocol coverage (deferred to Epic 7 when DmxOutputBinding lands).
- Translated editions (UK English only this Epic; translation framework deferred).

## Deliverables

1. **`docs/manuals/user-manual.md`** - top-to-bottom user manual, target ~30-40 pages of rendered Markdown.
2. **`docs/manuals/protocol-manual.md`** - normative protocol specification, target ~20-30 pages.
3. **`docs/manuals/README.md`** - one-pager index pointing at both manuals + the architecture spec.
4. **Repo README update** - link the two manuals from the top-level `README.md` "Documentation" section. Re-flow the README to absorb the architecture-additions carry-forward from Epic 4.7 (screen loopback / dispatch fan-out).
5. **`docs/developing-shows.md` re-flow** - bring in the dispatch behaviour additions from Epic 4.7 (master loopback); cross-link from the user manual ("for developers") and the protocol manual ("Show plug-in surface").
6. **Notion pages**: one Notion page per manual under the existing NocturNation workspace; status synced via the bidirectional doc-sync mechanism.

## Blocks

Verification ownership: **(L)** = laptop / native build, **(R)** = reader walk-through review.

### Block 1: Manual scaffolding and table of contents

- Create `docs/manuals/` directory with `user-manual.md`, `protocol-manual.md`, `README.md` index.
- Draft top-level table of contents for both manuals (chapter list only, no body).
- Add frontmatter (title, status, notion fields).
- Cross-link from architecture spec §1 to both manuals.
- Update repo top-level `README.md` Documentation section with manual links.
- **(L)** Markdown lint clean; **(R)** Jason confirms TOC structure before body work begins.

### Block 2: User manual - theory of operation and hardware setup

- Write the theory of operation chapter (system topology, beat/section detection in plain English, why distributed, why pre-grouped bracelets, why ESP-NOW).
- Write the hardware setup chapter (Plus2 vs S3, antenna orientation, IR radiation patterns - cross-reference `project_stick_ir_radiation_patterns`).
- Diagrams: system topology block diagram, IR coverage cones (Plus2 omnidirectional + S3 focused), ESP-NOW redundancy.
- **(L)** all diagrams render in GitHub + Notion; **(R)** cold reader (someone who hasn't used NocturNation) can answer "what does it do and how does it work" after reading.

### Block 3: User manual - firmware install, config walk-through, modes and shows

- Write the PlatformIO install chapter (env names per host, build command, flash command, recovering from a soft-bricked stick).
- Write the configuration walk-through (full Config tree, every leaf documented).
- Write the modes-and-shows chapter (mode FSM, show picker UX, per-show settings UI).
- Screenshots of menu screens where helpful.
- **(L)** all linked NVS keys match `src/config_mode/`; **(R)** Jason can hand the manual to a deployment helper and they can configure a slave without supervision.

### Block 4: User manual - troubleshooting, glossary, index

- Troubleshooting chapter organised by symptom: bracelets not flashing, NO SIGNAL on slave, ESP-NOW range, IR coverage gaps, channel mismatch, low battery behaviour, audio not detected.
- Glossary (PixMob, ESP-NOW, M5Unified, BeatDetector, DropDetector, MUSIC_EVENT, target_class, target_group, ...).
- Index (Markdown links to every defined term).
- **(L)** every glossary entry cross-linked in body text; **(R)** Jason reviews troubleshooting against real venue failure modes.

### Block 5: Protocol manual - wireless layer, frame formats, addressing

- Front matter: scope, normative language, licence, versioning.
- Wireless layer chapter: ESP-NOW transport spec, channel plan, redundancy, dedup.
- Frame format chapter: byte-by-byte tables for LIGHT_COMMAND, HEARTBEAT, MUSIC_EVENT; example hex dumps for each.
- Class+group addressing chapter: enum, routing semantics, worked examples.
- **(L)** every documented byte matches the code under `include/transport/` and `src/transport/`; tests pass with no protocol regression.

### Block 6: Protocol manual - PixMob IR annex, channel discovery, NVS schema, conformance

- PixMob IR encoding annex (with explicit attribution to `jamesw343/PixMob_IR`, byte tables, reference vectors).
- Channel discovery chapter (master picks + slave auto-scan sequence).
- NVS schema annex (informative; M5Stick reference implementation).
- Conformance chapter (MUST / SHOULD / MAY for receivers).
- Reference test vectors annex.
- **(L)** reference vectors generated by running the actual native tests against canonical inputs; regenerate against jamesw343's Python encoder for the IR section to preserve upstream-as-truth invariant.

### Block 7: README + developing-shows re-flow

- Update top-level `README.md` to: link both manuals; describe the Epic 4.7 dispatch additions (loopback) at a high level for visitors; bump version markers.
- Re-flow `docs/developing-shows.md` to reflect the dispatch fan-out - show authors no longer hand-roll fan-out, single `render_fx("00:00", ev)` call handles the whole transmission tree.
- **(R)** Jason confirms README reads cleanly cold; **(L)** developing-shows builds cleanly and cross-references the new manual sections.

### Block 8: Multi-slave bench verification + close-out

- **Folded-in Epic 4.65 Block 8**: stand up two slaves in different IR-coverage zones (Plus2 + S3), confirm class+group routing on the wire (master broadcasts to `01:07`, only slaves whose bindings match light-class group 7 fire), confirm the slave-as-repeater toggle behaviour, confirm sequence-loss signal-quality bars. This was deferred from Epic 4.65 and is the natural verification phase for the user manual's troubleshooting / configuration chapters (Block 3 + Block 4 documented these behaviours; this block proves them under deployment).
- Final cold-read by Jason of both manuals end-to-end.
- Update architecture spec §1 to point at the manuals as the canonical operator/implementer entry points.
- (Notion sync deferred per Epic-opening decision: manuals stay local-only until the body stabilises; future Notion sync is its own focused task.)

## Risks and mitigations

- **Doc drift vs code**: a normative protocol manual that lies is worse than no manual. Mitigation: every byte-level claim cross-referenced to the test or header that exercises it; protocol changes go through doc-update gate (block any protocol PR that doesn't update the protocol manual).
- **Scope creep into Epic 5 territory**: tempting to document the Tildagon implementation in the protocol manual. Hold the line - the protocol manual is class-agnostic; Tildagon-specific implementation notes belong in Epic 5.
- **PixMob upstream divergence**: jamesw343's encoder is the source of truth; the IR annex must attribute correctly and stay parity-tested. If jamesw343 changes the encoding, the annex follows.
- **Reader fatigue**: ~50 pages of manuals risks no one reading them. Mitigation: the README "Documentation" section directs each reader-type to their starting chapter; the user manual opens with a 1-page "if you only read this, read this" deployment quickstart.

## Carry-forwards into this Epic

- README re-flow for screen loopback architecture additions (carry-forward from Epic 4.7 close-out).
- `docs/developing-shows.md` re-flow for dispatch behaviour (same).
- Diane-style cold README walk-through (carry-forward since Epic 1) - the user manual is the home for what this carry-forward needed.
- **Epic 4.65 Block 8** (multi-slave bench verification of class+group routing) folded into Block 8 of this Epic - documentation walk-through provides the natural deployment scenario to exercise multi-slave routing end-to-end.

## Forward-looking notes

- The protocol manual is **the** specification Epic 5 (Tildagon) builds against. Any ambiguity discovered during Epic 5 implementation feeds back as a protocol-manual clarification PR.
- A future "hardware repo split" Epic will fork the schematic/fabrication content into its own repo; the protocol manual stays in firmware-repo because it specifies what the firmware sends/receives.
- A future "translated editions" Epic stands up a translation framework; this Epic stays UK English single-language to keep editorial scope tight.

Processing Type: **Documentation-only**. No firmware change, no test change. Every block is editorial; reader-testing is the verification mechanism that matters.

## Status update (2026-05-12)

Blocks 1-7 drafted in a single pass and committed to main. Outstanding work for Epic 4.8 close-out:

- **Block 8 multi-slave bench verification** (folded-in 4.65 Block 8) - awaiting Jason at the bench with two slaves in different IR-coverage zones. Hardware-only verification path; documentation work has surfaced no remaining doc issues that block this verification.
- **Final cold-read** - Jason reads both manuals end-to-end. Expected to surface minor editorial corrections (typos, missing cross-links, occasional clarification). Folds back as a doc patch.
- **Architecture spec §1 cross-link** - point at the manuals as the canonical operator/implementer entry points. Will land as part of the next architecture spec sync.

Deliverables landed so far:

- `docs/manuals/README.md` - index pointing at both manuals.
- `docs/manuals/user-manual.md` - 8 chapters + glossary + index. Quickstart, theory of operation, hardware, install, configuration walk-through, modes and shows, troubleshooting.
- `docs/manuals/protocol-manual.md` - 7 sections + 4 annexes (PixMob IR, NVS schema, reference test vectors, protocol version history). Normative MUST/SHOULD/MAY language throughout.
- Top-level `README.md` re-flowed to v0.5 reality (Plus2 + S3, six-layer architecture, manuals linked, Roadmap reflects closed Epics 1-4.7 + active 4.8 + next 5).
- `docs/developing-shows.md` - added "What dispatch does for you (Epic 4.7 onward)" subsection covering the master loopback (ESP-NOW + IR + screen fan-out from one `render_fx` call) and the bracelet-residue handling that replaced the rolled-back IR primer.

Notion sync wired up 2026-05-12 (afternoon): all four manual documents now have `notion_id` / `notion_url` / `sync_direction: bidirectional` in their frontmatter and live as subpages of the NocturNation project root in Notion. The manuals can now be edited in either VS Code or Notion and synced across.

### IR reset primer rollback (2026-05-12)

Bench testing during this Epic established that the Epic-4.7 IR reset primer (`dispatch_output_class_group` sending a zero-RGB broadcast frame ahead of the main fire when the IR transmitter has been idle for > 300 ms) was net-harmful: it doubled IR traffic for every sparse-cadence show, and only Rainbow - which already skipped the primer via the idle gate - rendered reliably on the bracelets. Diagnosed as receiver-side overload, not bracelet-side residue (the original premise was that residue dominated; in practice, traffic overload dominates).

Rolled back. The primer block, the `s_last_ir_fire_ms` idle-timestamp state, the `reset_ir_primer_state_for_tests()` test seam, and the now-unused `pixmob_protocol.h` / `<Arduino.h>` includes in [src/dal/dal.cpp](../../src/dal/dal.cpp) are removed. Each `render_fx` call now produces exactly one IR frame on the master loopback path. Residue is handled in the **show**, not the dispatch, by sizing envelope durations to fit inside the show's fire cadence (SparkleVis: 960 ms envelope on 1100 ms cadence is the canonical pattern).

Test impact: assertions in `test_beat_pulse`, `test_dynamic_show`, `test_spectrum_bars`, and `test_show` that expected an extra primer fire per beat dropped by one (3 → 2 or 2 → 1, depending on the test). All 61 affected native tests pass; Plus2 + S3 firmware builds clean. Bench verification on hardware confirmed all non-Rainbow shows now render reliably.

Documentation updates landed in the same change: user manual §1.5 (was "The IR reset primer", now "Bracelet timing and residual state"), protocol manual (§4.4 simplified, §A.4 deleted, §A.5 renumbered to A.4), flow-diagrams §5 (Mermaid simplified), developing-shows (dispatch fan-out subsection updated), this Epic's Scope / Deliverables / Carry-forwards / Status, README dispatch paragraph and Epic 4.7 roadmap entry. The closed Epic 4.7 retrospective ([epic-04.7-dynamic-show.md](epic-04.7-dynamic-show.md)) has a rollback annotation on its primer bullet but is otherwise unchanged - the primer was real history at close-out.

| Document | Notion page |
|---|---|
| [docs/manuals/README.md](../manuals/README.md) | [35ebd067740580369084dc6f9b2145e8](https://www.notion.so/35ebd067740580369084dc6f9b2145e8) |
| [docs/manuals/user-manual.md](../manuals/user-manual.md) | [35ebd067740580369c67c6738fe3f6d0](https://www.notion.so/35ebd067740580369c67c6738fe3f6d0) |
| [docs/manuals/protocol-manual.md](../manuals/protocol-manual.md) | [35ebd067740580378400ec3e0e8a0ca0](https://www.notion.so/35ebd067740580378400ec3e0e8a0ca0) |
| [docs/manuals/flow-diagrams.md](../manuals/flow-diagrams.md) | [35ebd0677405807cb34cccefa936d4d9](https://www.notion.so/35ebd0677405807cb34cccefa936d4d9) |
