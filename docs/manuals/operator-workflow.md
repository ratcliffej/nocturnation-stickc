---
title: "NocturNation operator workflow"
status: Draft
notion_url: https://www.notion.so/365bd067740581bbace6c5ac7b2c0339
notion_id: 365bd067740581bbace6c5ac7b2c0339
last_synced: 2026-05-17
sync_direction: bidirectional
---

# NocturNation operator workflow

A short, practical guide for operators running a NocturNation deployment. Covers channel selection, Performance Mode (channel 11) operations, source_id verification, and what to do when things go wrong on the night. For the wire-level spec read the [protocol manual](protocol-manual.md). For the audience-facing badge UI read the [user manual](user-manual.md).

## Channels at a glance

NocturNation uses three of the standard non-overlapping 2.4 GHz Wi-Fi channels. Pick one per deployment; the Director is fixed to that channel.

| Channel | Use | source_id allocation | Lume access control |
|---:|---|---|---|
| 1 | Hobby / community / hackspace | Stable per device (community range `0x00-0x3F`, persisted to NVS) | Permissive: Lumes accept any source_id |
| 6 | Advanced operator override | Operator-discretionary; Director SHOULD pick a Performance-range id | Permissive: Lumes accept any source_id |
| 11 | Performance mode (curated shows) | Random per boot (Performance range `0x40-0xFE`, listen-before-broadcast) | Strict: Lumes only accept Performance-range source_ids |

If you're running a hackspace gig, a personal demo, or a recurring community event where the same Director comes back repeatedly, **use channel 1**. The stable source_id means a returning audience Lume recognises the same Director across power-cycles.

If you're running a curated show at a venue like EMF where attendees with badges might be tinkering on their own M5 Sticks, **use channel 11**. The Performance Mode protections (random per-boot source_id + listen-before-broadcast on the Director, cross-range filter + TOFU on the Lume) keep the audience locked to your show.

Channel 6 is reserved for advanced operators who need a third channel — e.g. running two simultaneous deployments in the same venue, or working around interference on 1 or 11. It carries no automatic protection; configure it as you would channel 1, but operators SHOULD pick a Performance-range source_id by convention.

## Performance Mode (channel 11)

### How the Director allocates its source_id

On every boot, the Director picks a fresh random source_id in the Performance range (`0x40-0xFE`, 191 slots) and listens for ~1 second on channel 11 before transmitting. If another Director is already broadcasting on that same source_id, the Director re-rolls and listens again. After three re-rolls with collisions on every attempt it proceeds with the last pick and logs a warning. The probability of three consecutive collisions with three concurrent Directors is well under one percent; in practice every show starts with a unique id.

The chosen id is shown on the Director's M5 Stick screen as `P:nn` (for example `P:4F`) in the bottom-right corner. This is the value the audience will lock to.

### How audience Lumes lock to your Director

When a Tildagon (or future Lume) powers on or rescans, it listens on its configured channel for the first valid frame from a non-broadcast source_id. The first such frame establishes a Trust-On-First-Use (TOFU) lock; subsequent frames from any other source_id are silently dropped for the rest of the session. On channel 11 specifically, only Performance-range source_ids are eligible to be locked — a misconfigured Director announcing a community-range id on channel 11 will be ignored entirely.

The locked id is shown on the Lume's screen as `ch 11 P:4F` (or `ch 11 C:nn` on channel 1). Audience members can verify they're locked to *your* Director by comparing the value on their badge to the value on your Director's screen.

The lock expires after ten seconds of no frames from the locked source. After that, the Lume re-enters listen state (`ch 11 listen`) and will accept the next valid frame as a fresh lock.

## Pre-show checklist

1. **Power the Director first.** Boot it before the audience arrives so it claims its source_id before any badge tries to lock to it. Lumes that arrive first risk locking to a tinkerer's M5 Stick if one happens to be broadcasting on channel 11.
2. **Note the source_id on the Director screen** (`P:4F`, etc.). Keep it visible during the show — operators and audience members can use it to verify locks.
3. **Pre-flight the renderers.** Switch the Director into Test Mode, fire a Rainbow or Sparkle pulse, and confirm a couple of nearby Tildagons light up and their screens show `ch 11 P:4F` matching your Director.
4. **Switch the Director back to Director Mode** before the audience arrives. The Test Mode menu owns the screen until you exit; the Show plug-in takes over once you're back in Director Mode.

## During the show: spot-checks

If something looks off — a section of the audience not lighting up, a single badge stuck dark — walk up and look at the badge's screen. Three states are informative:

- **`ch 11 P:4F`** matching your Director's id: locked correctly to your show. If the LEDs aren't firing, the issue is downstream (group filter, calm mode, IR alignment for bracelets — see the user manual).
- **`ch 11 P:nn`** showing a *different* id than yours: locked to another Director. Most likely a tinkerer on the same channel; ask the operator nicely to switch to channel 1 or stop broadcasting. Alternatively, ask the badge owner to open the settings menu and select "Rescan"; the next valid frame from your Director will establish a fresh lock.
- **`ch 11 listen`** or **`ch 11 scan`**: not currently locked to anyone. Either the badge just powered on (give it a few seconds) or its TOFU lock has expired due to a frame gap. The next valid frame will re-lock it.

## Competing Director on the same channel

You can't fully prevent another operator from booting a Director on the same channel mid-show. The Performance Mode protections defend Lumes against *accidental* disruption (the tinkerer's badge picks a Performance-range id randomly and your Lumes are already locked to your id) but cannot stop a determined attacker reading the open-source firmware and crafting a colliding frame stream.

If you spot a competing Director:

1. **Diagnose by ID**: look at the source_id on the affected badge versus your Director. Mismatched id = competing transmitter; matching id but no LEDs = downstream issue (group, calm mode, IR).
2. **Operational coordination**: ask the tinkerer to stop broadcasting. Most accidental cases will gracefully comply; that's the design assumption.
3. **Rescan**: operator can rescan their badge through the settings menu to break the lock and pick up your stream if it's louder / closer.

There's no cryptographic protection at this protocol version (Tier 0); see the [security RFC](https://www.notion.so/) for the deferred Tier 1+ plans.

## Honest residual risk

A badge that powers on *after* a tinkerer's M5 Stick but *before* your Director's first frame will lock to the tinkerer. The boot-Director-first checklist above is the operational mitigation. There is no technical defence at protocol version `0x02`.

The rescan flow (`Settings → Rescan`) clears the lock and lets the badge re-acquire from the loudest stream nearby. This is the operator's escape hatch when the wrong-lock case occurs in practice.

## Channel 1 (community / hobby)

Channel 1 deployments don't carry these access-control mechanics. The Director allocates a stable community-range id at first boot and reuses it across reboots. Lumes accept any non-broadcast id and TOFU-lock to the first frame they see.

The trade-off is by design: channel 1 prioritises ease of use and "any community member with a NocturNation Director can light up nearby badges" over collision resistance. If two community Directors operate in the same room, the audience badges lock to whichever they hear first — that's not a bug, that's the social contract on channel 1.

## Where to learn more

- [Protocol manual §3.4](protocol-manual.md#34-source-identifier-partitioning) — normative spec for the source_id partition + TOFU rules.
- [Protocol manual §5](protocol-manual.md#5-channel-discovery) — channel selection and scan rules.
- [Flow diagrams §9](flow-diagrams.md#9-channel-discovery-and-re-scan) — Mermaid renderings of the channel discovery and re-scan state machines.
- [User manual](user-manual.md) — audience-facing badge UI.
