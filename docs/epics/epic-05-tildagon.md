---
title: "Epic 5: Tildagon receiver app"
status: In progress
notion_url: https://www.notion.so/358bd067740581b19551d158d658df76
notion_id: 358bd067740581b19551d158d658df76
notion_status: In progress
last_synced: 2026-05-13
sync_direction: bidirectional
---

## Related documents

- [NocturNation protocol manual](../manuals/protocol-manual.md) - the authoritative implementation specification for Epic 5. Every byte on the wire is documented here. Where this Epic references protocol behaviour, it points at sections of this manual rather than at the architecture spec.
- [NocturNation user manual](../manuals/user-manual.md) - operator-facing behaviour expected of a NocturNation slave; the Tildagon should match it as closely as the form factor allows.
- [Architecture specification](../architecture.md) - internal design notes. §15 photosensitivity / Calm Mode is the load-bearing reference for Epic 5 safety work.
- [Epic 4 (ESP-NOW transport)](epic-04-esp-now.md) - the transport surface the Tildagon connects to.
- [Epic 4.65 (class+group addressing)](epic-04.65-device-addressing.md) - the addressing model the Tildagon participates in. `MultiLedScreen` (`0x03`) is reserved for Tildagon-class devices in the `DeviceClass` enum.
- [Epic 4.7 (dynamic shows)](epic-04.7-dynamic-show.md) - the show framework whose output the Tildagon consumes. Important context: the analyser primitives (snare, hi-hat, descriptors, sections) feed master-internal show logic and produce `LIGHT_COMMAND` fires - they are not broadcast as separate wire messages.

## Goal

Build the Tildagon receiver app: a MicroPython app that runs on the EMF Tildagon badge, listens for NocturNation ESP-NOW frames, and animates the badge's twelve perimeter LEDs (indexed 1..12 per the Tildagon hardware API) and 240×240 round LCD in time with the music produced by a master Stick. Calm Mode default-on per [architecture spec §15](../architecture.md). **Fresh codebase; not a port of the M5 Stick firmware.**

## Architectural constraint: Tildagon is slave-only

The Tildagon has no microphone and is therefore inherently slave-only. It cannot run autonomous audio analysis; it cannot be a master. This is not a limitation worth working around with hexpansions; it is a clean architectural fact that strengthens the design for the first version.
It should be noted that future versions will likely incorporate a master mode, but it will be architecturally different to the ESP32 version, e.g. responding to button presses, an external mic, or providing Art-Net/DMX compatibility, but this will be for a future epic.

Implications:

- The Tildagon app implements **Slave behaviour only**, not the full M5 mode finite-state-machine. There is no master-mode option to expose at this time. It should be noted, though that this may change.
- The Tildagon **boots directly into receive mode** with channel auto-scan (per [protocol manual §5.3](../manuals/protocol-manual.md#53-slave-auto-scan-mode)). No mode menu at boot - if anything, just channel selection.
- The Tildagon is a pure receiver: ESP-NOW receive only, no transmission. This is now [protocol-required of any receiver](../manuals/protocol-manual.md#74-receiver-must-not) (a conforming receiver MUST NOT transmit NocturNation frames other than as a repeater or to render a local infra-red equivalent - and the Tildagon does neither).

The Tildagon being mic-less validates the same dumb-receiver / smart-upstream-source pattern that any future companion-app device will use. Same protocol, different upstream. The Tildagon is, in effect, a *good prototype* for the eventual mic-less receiver pattern, being validated on real hardware first.

What the Tildagon *cannot* do compared to an M5 Stick:

- Operate standalone in a venue without a master (it would just sit dark with no input).
- Generate beats from ambient music.
- Be used at home with the user's own music *without external infrastructure*.

What the Tildagon *can* do that an M5 Stick cannot, easily:

- Twelve addressable RGB perimeter LEDs (indexed 1..12) around a 240×240 round LCD - a richer rendering surface than the StickC's narrow rectangular screen.
- Sit in thousands of attendees' hands at EMF - the distribution problem is solved.
- Use the established EMF app-store submission path with community goodwill.

## Group ID semantics (canonical, shared with StickC)

Spelled out here because the Notion draft was loose on this and Epic 5 needs the rule exact:

- `target_group == 0` on a `LIGHT_COMMAND` is the **broadcast group**. Every receiver renders, regardless of the receiver's own configured group. This is the canonical "address everyone" form and is the default routing for `render_fx` calls on the reference master.
- `target_group == N` for non-zero N addresses only receivers whose own configured group is exactly N.
- A receiver whose **own** group is 0 has no group and only renders broadcasts; it is not a receive-side wildcard.

First-boot default: receivers (StickC slaves *and* Tildagons) **MUST** persist a random group in {1, 2, 3} on first boot, so a fleet of newly-flashed devices distributes naturally across the three drum groups that DynamicShow routes kick / snare / hi-hat to. The operator can override via the in-app settings (any value 0..255, with 0 explicitly meaning "broadcast only").

This rule is now implemented on the StickC at [src/modes/persistence.cpp::migrate_legacy_nvs_keys()](../../src/modes/persistence.cpp) (Epic 5 prep), and codified normatively in [protocol manual §4.2](../manuals/protocol-manual.md#42-group-filtering). The Tildagon receiver replicates the same behaviour in MicroPython.

## How the Tildagon consumes the wire

This is the most important change from the original Notion draft.

The Tildagon receives **`LIGHT_COMMAND` frames** (message type `0x03`, [protocol manual §3.3.4](../manuals/protocol-manual.md#334-light_command-0x03)) and filters them against its configured device class and group. It is structurally an `OutputBinding`-shaped consumer, not a separate kind of receiver that consumes analyser events directly. There is no `SNARE_DETECTED` / `HIHAT_DETECTED` / `MUSIC_DESCRIPTOR` / `SECTION_CHANGE` message type to consume; those are master-internal analyser primitives that the master's `DynamicShow` plug-in consumes to compute and dispatch `LIGHT_COMMAND` frames.

In practical terms:

- **Kick drum** at the master fires a `LIGHT_COMMAND` to class `0x01` (Light) group 1 (in DynamicShow's `groups=3` config) or group 0 broadcast (in DynamicShow's default `groups=1`).
- **Snare hit** at the master fires `LIGHT_COMMAND` to Light group 2, or also goes to broadcast in the default configuration.
- **Hi-hat onset** at the master fires `LIGHT_COMMAND` to Light group 3, or broadcast.
- **Section change** at the master swaps the palette internally; nothing extra goes on the wire.

The Tildagon participates by advertising itself as a `MultiLedScreen` (`0x03`) class device and choosing how to map class+group LIGHT_COMMANDs to its render surfaces. There are reasonable design choices here that need settling (see [open design questions](#open-design-questions) below) - the broad direction is: every `LIGHT_COMMAND` whose `target_class` matches (`0x00` All or `0x03` MultiLedScreen) and whose `target_group` matches (`0x00` or the Tildagon's configured group) is rendered on the Tildagon's perimeter and LCD.

In addition to `LIGHT_COMMAND`, the Tildagon SHOULD honour:

- **`HEARTBEAT`** (`0x00`) for liveness ([protocol manual §6](../manuals/protocol-manual.md#6-heartbeat-and-liveness)).
- **`MUSIC_EVENT`** (`0x06`) carrying DROP/BREAKDOWN/BUILD if there is locally interpretable behaviour for it - e.g. a palette shift on DROP. Optional.

The richer "music descriptor" behaviour from the original Notion draft is rendered on the M5 master and emitted as a stream of `LIGHT_COMMAND` fires; the Tildagon experiences it as a richer-than-Simple-Beat stream of light commands, not as a stream of new wire message types.

## Operational model: protocol-shared, codebase-separate

Epic 5 is the first Epic that does not extend the M5 Stick codebase. The Tildagon and the M5 Sticks share the **NocturNation ESP-NOW protocol** (defined in [docs/manuals/protocol-manual.md](../manuals/protocol-manual.md)) but have nothing in common at the implementation level:

| Aspect | M5 Sticks (Epics 1-4.8) | Tildagon (Epic 5) |
|---|---|---|
| Chip family | ESP32 / ESP32-S3 (Xtensa) | ESP32-C3 (RISC-V) |
| Language | C++ | MicroPython |
| SDK / libraries | Arduino + M5Unified + esp-dsp | Tildagon OS app framework + MicroPython `espnow` module |
| Build / deploy | PlatformIO compile + flash | `mpremote` file copy (no build step) |
| Iteration loop | Edit → build → flash → monitor (5-15 s) | Edit → `mpremote cp` → run (~2 s) |
| Distribution | GitHub repo, build instructions | EMF app store listing |
| HAL relevance | Yes - Plus2 vs S3 abstraction | Not applicable - single platform |

Trying to share a codebase across this boundary would be the wrong call. The HAL abstraction the M5 line uses solves a real problem (different ESP32 variants in C++) but does not extend cleanly across to MicroPython. Better to write a clean Tildagon implementation that *speaks the same protocol* without sharing code.

This has a useful property: **the protocol is the source of truth, not the codebase**. Now that the [protocol manual](../manuals/protocol-manual.md) exists as a published normative specification, a future receiver implementer (LoRa wristband, ESP32-C6 dongle, anything) just needs to speak NocturNation ESP-NOW protocol version `0x01`. The protocol is the contract; everything else is implementation choice.

What this means in practice:

- A new repository (or a clearly-separated subdirectory) for the Tildagon app, distinct from the M5 firmware.
- The Tildagon app re-implements the protocol from the [protocol manual](../manuals/protocol-manual.md) using the [reference test vectors](../manuals/protocol-manual.md#annex-c-reference-test-vectors) and the upstream Python encoder for any PixMob-related parity testing.
- Native unit tests in Python (using `pytest`) validate the protocol implementation against the byte-level reference vectors. The reference firmware's `test_pixmob_parity` and ESP-NOW frame tests stay the authoritative source for vector generation.
- The cross-platform interop test that already exists at Epic 4 Block 4 (M5↔M5) is extended to M5↔Tildagon in this Epic. Same protocol, two implementations, both must round-trip cleanly.

## Business value

The Tildagon receiver is the moment NocturNation transitions from "clever IR controller plus a few slaves" to "distributed crowd lighting at festival scale, on hardware that's already in attendees' hands". It is also where the [photosensitivity safety constraints](../architecture.md) become real implementation rather than spec text, on what is structurally the highest-risk surface in the system (close to faces, full RGB, high-contrast flash capable).

Without this Epic:

- Attendees with Tildagon badges can't participate in a show without a separate Stick.
- The democratisation pitch (anyone with an ESP-NOW receiver can join) has no concrete demonstration on widely-distributed hardware.
- The architectural separation between transport (ESP-NOW) and effect rendering remains theoretical rather than proven across two implementations.

## Scope

**Included:**

- MicroPython app conforming to the Tildagon OS app framework (`update()`, `draw()`, `__app_export__` per [architecture spec §3.4](../architecture.md)).
- ESP-NOW receive on the Tildagon's ESP32-C3 with deduplication and frame validation, conforming to [protocol manual sections 2 and 3](../manuals/protocol-manual.md#2-wireless-layer).
- Channel auto-scan and locked-channel modes per [protocol manual §5](../manuals/protocol-manual.md#5-channel-discovery).
- **`LIGHT_COMMAND` rendering on the perimeter LEDs** (the primary surface).
- **`LIGHT_COMMAND` rendering on the round LCD** (secondary; richer than the Stick LCD allows).
- Optional **`MUSIC_EVENT`** consumption for palette shifts on DROP / BREAKDOWN.
- **Class+group filtering** on inbound `LIGHT_COMMAND` per the [group ID semantics section above](#group-id-semantics-canonical-shared-with-stickc). Tildagon advertises class `MultiLedScreen` (`0x03`); first-boot group is a random value in {1, 2, 3}; operator overrides via the in-app settings.
- **Configuration menu**. A small in-app settings UI for group ID (mandatory; the EMF audience will want to set this), channel (1 hobby vs 11 show), Calm Mode toggle, and brightness. Reachable from the standard Tildagon navigation; persists to MicroPython NVS.
- **Manual master mode** (stretch goal for EMF). Button-activated fire-an-effect input on the Tildagon: the operator presses a button, the Tildagon emits a `LIGHT_COMMAND` over ESP-NOW, every other receiver in range renders. This makes the Tildagon a *limited* upstream source - not a beat-driven master, but a tinkerer's "I want to fire a colour from my badge" interaction. The EMF audience enjoys tinkering with what is in their hands; this gives them a useful surface. Calm Mode caps still apply. Channel 11 (show) is reserved for the official master; a manual-master Tildagon transmits on channel 1 (hobby) so it does not interfere.
- **Test modes** (stretch goal for EMF). A small in-app test menu that fires fixed colours / envelopes locally on the Tildagon's perimeter LEDs and LCD, without needing a master in range. Useful for verifying the badge is working in a quiet area, and for the same EMF tinkerer audience that will be poking at the manual-master mode.
- **Calm Mode default-on** per architecture spec §15.3: cap fire frequency at 2 Hz, cap brightness at 50 %, disable LCD flashing, require operator opt-in for full-effect mode.
- Brightness / contrast caps per §15.2 (no high-contrast full-screen flashes).
- Frequency cap enforcement per §15.1 (4 Hz hard cap even in full-effect mode).
- **NO SIGNAL** indication after the 3-second heartbeat gap per [protocol manual §6.2](../manuals/protocol-manual.md#62-receiver-liveness-check).
- Backgrounded behaviour: perimeter LEDs continue, LCD reverts to user's foreground app per [architecture spec §7.3](../architecture.md).
- Native Python unit tests (pytest) covering the protocol implementation against the [reference test vectors](../manuals/protocol-manual.md#annex-c-reference-test-vectors).
- M5↔Tildagon interop verification (master Stick + Tildagon in radio range; LIGHT_COMMAND fires from a DynamicShow render on the perimeter LEDs).

**Explicitly excluded:**

- **Master mode** - architecturally not possible (no mic).
- **Any ESP-NOW transmission** by the Tildagon (no slave-as-repeater, no acknowledgements, no upstream telemetry).
- **IR transmission** from the Tildagon (no IR LED hexpansion in this design; Tildagon does not target bracelets).
- **Tier 1 encryption / authentication** - separate security Epic.
- **Custom hardware** (DECT module, audio hexpansion, etc.). Specifically, adding a mic hexpansion to make the Tildagon a master would defeat the architectural pattern this Epic validates.
- **App-store review-and-iterate beyond one revision round** - if the first revision is rejected and a second rejection seems plausible, fall back to the minimal-scope ship described in the [EMF 2026 timeline](#emf-2026-timeline) section.

## Open design questions

These need settling before block-level planning, but are not blockers for Epic 5 to be considered ready.

**Q1: Device class.** Tildagon advertises as `MultiLedScreen` (`0x03`) per the existing `DeviceClass` enum. Implication: a master broadcasting class `0x01` (Light) does not address the Tildagon - only class `0x00` (All) or class `0x03` (MultiLedScreen) does. **Decision needed**: do we want the Tildagon to *also* render `Light`-class commands so it lights up alongside PixMob bracelets on `01:00` broadcasts? Pros: visual consistency with PixMobs. Cons: dilutes the class taxonomy. Recommendation: render `Light`-class commands on the perimeter LEDs (the natural analogue of a wristband), reserve `Screen`-class for LCD-specific behaviour, and treat `MultiLedScreen` as the "address the whole Tildagon as one unit" form.

**Q2: Group default at first boot.** Original Notion draft said "self-assign from 1-3"; that mapped to the pre-Epic-4.65 group scheme. Current model: groups are 0-255 (PixMob constrains to 0-31). **Decision needed**: does the Tildagon default to group 0 (broadcast - accept everything), or auto-self-assign to a random group in 1..31 to participate in sparkle patterns? Recommendation: default group 0 (operator visibility into what is happening matters more than statistical sparkle).

**Q3: Multiple bindings per Tildagon.** The M5 Stick exposes two `OutputBinding`s (LocalDisplayBinding + PixMobIrBinding) - each with its own class and group. The Tildagon could mirror this: a perimeter-LED binding (class Light or MultiLedScreen) plus an LCD-screen binding (class Screen). **Decision needed**: one binding per Tildagon (simpler) or two (more architecturally aligned). Recommendation: start with one logical binding and revisit if a real use-case for split addressing emerges.

**Q4: Channel scan order.** Protocol manual §5.3 specifies channel 11 first, then channel 1. Confirm the Tildagon implements the same order (it should, but worth verifying that the MicroPython `espnow` module's channel-switch timing supports the 2-second-per-channel cadence cleanly).

**Q5: Protected show channel.** Channel 11 is the show channel; channel 1 is the open hobby channel. EMF attendees with Tildagons will tinker, including with the manual-master button-fire mode listed in scope. The show MUST be insulated from this. Asymmetric channel handling on the Tildagon:

- **Receive (slave mode)**: the Tildagon auto-scans both channels with channel 11 checked first per [protocol manual §5.3](../manuals/protocol-manual.md#53-slave-auto-scan-mode). The Tildagon listens on whichever channel the master is broadcasting on, so an attendee whose badge is on auto-scan participates in the show on channel 11 without configuration.
- **Transmit (manual-master mode, if implemented)**: hard rule - MUST transmit on channel 1 only. The firmware physically cannot configure manual-master to use channel 11, even if the operator tries. Channel 11 stays reserved for the official master.

Channel 11 is **not encrypted** at protocol version `0x01`. Any "lock channel 11 to a known master id" or cryptographic-authentication mechanism is out of scope for this Epic and tracked as a separate Tier 1 security Epic. For EMF 2026, the protection is two-layered: (a) firmware-enforced transmit constraint on the Tildagon (no badge can transmit on channel 11), and (b) operator-enforced master uniqueness (only the official master Stick is configured for channel 11 at the venue). The show benefits from frequency isolation without needing cryptographic protection yet.

**Q6: ESP-NOW receive-channel steering on Tildagon hardware** (added 2026-05-12 after Block 2 bench verification). The naïve `wlan.config(channel=N)` on `network.WLAN(STA_IF)` works for *the first* channel change after `wlan.active(True)`, but a subsequent change raises `RuntimeError: Wifi Unknown Error 0xffffffff`. Confirmed on real Tildagon hardware: first call (`channel=11`) succeeded, second call (`channel=1`) failed. The badge's networking layer appears to lock STA on a channel once it has been set, so the auto-scan-across-channels design from protocol manual §5.3 cannot be implemented as written. Block 2 currently falls back to receiving on whichever channel succeeded first (channel 11 in practice, since that is the scan-order head); the operator aligns the master Stick to that channel manually.

Three avenues to investigate at Block 5 (configuration UI is the natural place to surface "current channel" + "scan / fixed" toggle to the operator, so the question wants answering then):

1. Add `wlan.disconnect()` (and/or `wlan.active(False)` + `wlan.active(True)`) before each `wlan.config(channel=...)` call so the driver releases its state-lock.
2. Use `AP_IF` instead of `STA_IF` - AP mode doesn't have STA's "associating, do not disturb" semantics, and channel changes should work freely. Trade-off: the badge would broadcast a Wi-Fi access point, which we'd need to keep hidden / unnamed to avoid confusing users.
3. Drop auto-scan from scope. Operator picks a channel manually in Config and holds it for the deployment. Matches the bench-verified Block 2 fallback. Simplest, but loses the "badge just works, no configuration needed" UX.

## Acceptance criteria

- [ ] App installs on a real Tildagon via `mpremote`.
- [ ] App runs in the Tildagon Simulator (where supported) without errors.
- [ ] Tildagon in radio range of a master Stick (Plus2 or S3, running DynamicShow) animates perimeter LEDs in sync with detected kicks / snares / hi-hats per `LIGHT_COMMAND` arrival.
- [ ] Calm Mode is on by default; operator must opt-in to full-effect mode via the in-app settings.
- [ ] Group filter persists across reboots (operator-set, or auto-assigned per [open design question Q2](#open-design-questions)).
- [ ] Frequency cap enforced: even if the master broadcasts at 8 Hz, the Tildagon never fires faster than 4 Hz (full mode) or 2 Hz (Calm Mode).
- [ ] Backgrounded app continues ESP-NOW listening; perimeter LEDs continue animating; LCD returns to the user's foreground app.
- [ ] Battery impact characterised: how much faster does the badge drain with the receiver app active versus idle?
- [ ] NO SIGNAL behaviour: after a 3-second gap, the Tildagon shows a clear indication that the master is unreachable and runs no local effect.
- [ ] Python unit-test suite passes against the [reference test vectors](../manuals/protocol-manual.md#annex-c-reference-test-vectors).
- [ ] M5↔Tildagon interop verified at the bench: a Plus2 master with a Tildagon in range, playing a DynamicShow against a real music track, with the perimeter LEDs visibly reacting.
- [ ] First-boot group assignment: a freshly-flashed Tildagon persists a random group in {1, 2, 3} on first power-on; the value survives reboot; operator can override via the in-app settings.
- [ ] Manual-master mode (if implemented): button-fire transmits on channel 1 only. Channel 11 is verified rejected at the firmware level (the operator cannot configure manual-master to transmit on the show channel even if they try).
- [ ] In-app settings menu: group ID, channel, Calm Mode toggle, brightness all reachable and persistent.

## Blocks

Verification ownership: **(L)** = laptop / native Python tests, **(B)** = Tildagon simulator, **(H)** = real Tildagon hardware on the bench.

### Block 1: Platform familiarisation and toolchain

- Source a Tildagon badge.
- Install `mpremote`, `pytest`, the Tildagon simulator (if it works on the host OS; spike effort to verify).
- Write a minimal "hello world" app that conforms to the Tildagon OS app framework (`update()`, `draw()`, `__app_export__`) and renders solid colour on the perimeter LEDs.
- Flash and run on real hardware; iterate until the toolchain is reliable.
- Document any surprises (badge quirks, simulator gaps, MicroPython oddities) for [open design questions](#open-design-questions) updates.
- **(H)** real Tildagon runs the hello-world app; one perimeter LED visibly lit.
- **(B)** simulator runs the hello-world app, if simulator works at all.

### Block 2: ESP-NOW receive

- Implement `protocol_version` validation, header parsing, payload-length validation.
- Implement deduplication ring (sixteen-deep on `(source_id, sequence_number)`).
- Implement `hop_count` drop rule.
- Implement channel auto-scan per protocol manual §5.3 (channel 11 then channel 1, 2 s each).
- **(L)** Python `pytest` suite validates frame parsing against reference vectors from protocol manual annex C; deduplication round-trips correctly.
- **(H)** real Tildagon receives `LIGHT_COMMAND` frames from a master Stick; serial console logs the parsed RGB triplet on each arrival.

### Block 3: Perimeter-LED rendering

- Implement an envelope renderer for the twelve perimeter LEDs (attack/sustain/release with chance gate per LED).
- Map a `LIGHT_COMMAND` envelope onto the six-LED ring: the [open design questions](#open-design-questions) Q1 choice of class behaviour determines whether kick/snare/hi-hat fires hit different LED subsets or all six.
- Implement the frequency cap (Calm Mode 2 Hz / full 4 Hz).
- **(L)** Python tests cover envelope timing on a stub LED layer.
- **(H)** real Tildagon perimeter ring fires in sync with a master Stick running DynamicShow on real music.

### Block 4: LCD rendering

- Implement a basic LCD pulse renderer (round 240×240). Start with one solid colour wash that matches `LIGHT_COMMAND` colour; treat it as a secondary surface to the perimeter LEDs.
- Apply [architecture spec §15.2](../architecture.md) brightness / contrast caps - no high-contrast full-screen flashes.
- Honour Calm Mode disable-LCD-flashing setting.
- **(H)** real Tildagon LCD shows a soft pulse in sync with perimeter LEDs.

### Block 5: Configuration UI

- In-app settings menu: Calm Mode toggle, channel selector, group selector, brightness slider.
- Persistence to Tildagon's MicroPython `nvs` equivalent.
- Boot flow: app starts in receive mode; settings reachable via Tildagon's standard navigation.
- **(H)** real Tildagon retains settings across reboot; full-effect mode opt-in works.

### Block 6: NO SIGNAL behaviour, backgrounded operation, MUSIC_EVENT handling

- NO SIGNAL indication after 3-second gap per protocol manual §6.2.
- Backgrounded behaviour: perimeter LEDs continue animating, LCD returns to user's foreground app per architecture spec §7.3.
- Optional `MUSIC_EVENT` consumption: DROP triggers a palette shift; BREAKDOWN reverts.
- **(H)** real Tildagon shows NO SIGNAL when master is powered off; resumes immediately when master returns; backgrounded behaviour verified by switching to another badge app.

### Block 7: Interop verification, EMF app-store submission, close-out

- M5↔Tildagon bench session: master Stick + Tildagon + handful of PixMob bracelets, running DynamicShow against real music. Confirm the Tildagon visibly reacts in coordination with the bracelets.
- Battery drain measurement: idle Tildagon vs receiver-app Tildagon, both over a 2-hour window.
- Documentation: README in the Tildagon app, screenshots, link to the protocol manual.
- **EMF 2026 app-store submission** (hard deadline target: 30 June 2026). Package per the EMF app-store requirements; submit; track review.
- **Review-iteration round if rejected** (budget one revision; if a second rejection looks plausible, fall back to the minimal-scope ship per the [timeline section](#emf-2026-timeline)).
- Close-out: status flip Proposed → Done, observations folded back into the protocol manual if any clarifications surfaced during implementation.

## EMF 2026 timeline

EMF 2026 runs **16-19 July 2026**. As of 2026-05-12 that is **65 days away** - approximately nine working weeks. Epic 5 is sprint-mode against this deadline, not the leisurely platform-validation pace the under-refined Notion draft assumed.

Working backwards from the festival:

- **EMF app-store window** typically closes a week or two before the event. Confirm exact dates with the EMF team early in Block 1, but assume a hard "submitted by 30 June 2026" target.
- **Submission review** at EMF is community-driven and usually fast (days, not weeks), but a rejection-and-iterate cycle eats real time. Plan for one revision round.
- **Bench verification** with real attendees-in-the-crowd is impossible pre-festival; the M5↔Tildagon bench interop session in Block 7 has to substitute.

That puts the realistic dev window at **5-6 weeks** (mid-May through end of June 2026), with submission and any review-iteration cycle in the final 2-3 weeks. The seven-block plan in this Epic is sized for that window, but assumes the MicroPython iteration loop genuinely is ~2 s (Tildagon's claimed iteration time is what makes the timeline plausible at all).

Pragmatic adjustments for the deadline:

- **App-store submission folded into Block 7** (close-out) rather than being a separate follow-up Epic. The deadline pressure makes the single-owner / single-Epic model the right call.
- **Block 1 (platform familiarisation) is capped at one week** elapsed time. If the toolchain isn't reliable in five days, that is itself the decisive signal to escalate (find someone with Tildagon experience, ship a simpler scope, or accept slipping to EMF 2028).
- **Open design questions Q1-Q4** need answering before Block 2 starts. Each has a recommendation - Jason picks, no leisurely consensus-building round.
- **Calm Mode** (architecture spec §15) cannot slip; it is the safety floor and ships at Block 4 the moment LCD rendering exists.

Fallback if the timeline does not hold: ship a minimal "perimeter LEDs only, broadcast group, Calm Mode" Tildagon by submission deadline; defer LCD rendering and per-group filtering to a post-EMF revision. This gives attendees *something* at the festival even if the full feature set slips.

## Dependencies

| Dependency | Type | Status | Owner |
|---|---|---|---|
| Epic 4 (ESP-NOW transport) | Internal | Done | Jason |
| Epic 4.5 (capability-aware analyser) | Internal | Done | Jason |
| Epic 4.6 (clean architecture, plug-in surfaces) | Internal | Done | Jason |
| Epic 4.65 (class+group addressing) | Internal | Done | Jason |
| Epic 4.7 (Show framework + DynamicShow) | Internal | Done | Jason |
| Epic 4.8 (manuals; protocol manual is Epic 5's spec) | Internal | In progress (Block 8 hardware pending) | Jason |
| Tildagon hardware | External | Required (source one) | Jason |
| Tildagon simulator (offline dev) | External | Available ([github.com/emfcamp/badge-2024-software](https://github.com/emfcamp/badge-2024-software)) | Jason |
| `mpremote` tool installed locally | External | Available via pip | Jason |
| Architecture spec v0.23+ §15 (Calm Mode, photosensitivity) | Internal | Done | Jason |
| [Protocol manual](../manuals/protocol-manual.md) (the implementation spec) | Internal | Drafted (Epic 4.8) | Jason |

All upstream firmware Epics are closed. Epic 5 can start as soon as Epic 4.8's final cold-read either passes or surfaces protocol-manual clarifications - if any of those clarifications change the wire format, Epic 5's protocol implementation tracks them via the protocol manual rather than chasing the architecture spec.

## Status notes

Proposed 2026-05-06; refined 2026-05-08; further refined 2026-05-12 (twice) to address the following corrections to the original Notion draft:

1. **Wire-format premise corrected.** The original draft described the Tildagon as consuming `SNARE_DETECTED`, `HIHAT_DETECTED`, `MUSIC_DESCRIPTOR`, and `SECTION_CHANGE` as separate ESP-NOW message types. None of those message types exist on the wire as shipped. The analyser primitives produce master-internal events; the master's `DynamicShow` plug-in consumes them and emits `LIGHT_COMMAND` fires with appropriate class+group routing. The Tildagon is structurally an `OutputBinding`-shaped consumer of `LIGHT_COMMAND`, exactly like every other slave.
2. **`MUSIC_EVENT` vs `MUSIC_DESCRIPTOR`.** The wire-level `MUSIC_EVENT` (`0x06`) carries only DROP / BREAKDOWN / BUILD. Centroid, energy, and density never leave the master.
3. **All upstream Epics closed.** Epics 4 / 4.5 / 4.6 / 4.65 / 4.7 are Done; Epic 4.8 in progress (documentation). The "hybrid parallel sequencing with Epic 4.7" rationale is no longer needed.
4. **Group ID model updated.** Original "1-3" self-assignment mapped to a pre-Epic-4.65 scheme. Current model is class (`MultiLedScreen=0x03` for Tildagon) plus group (`0..255`, PixMob constrains to 0..31). Default behaviour is an [open design question](#open-design-questions).
5. **Protocol manual is now the implementation spec.** Epic 4.8 produced [docs/manuals/protocol-manual.md](../manuals/protocol-manual.md) specifically as the receiver-implementation specification. Epic 5 rebases on it.
6. **`BEAT_DETECTED` is not actually emitted.** Wire format retained from Epic 4.5 onwards but the reference firmware does not broadcast it. Receiver design is `LIGHT_COMMAND`-driven.
7. **Epic 6 references replaced.** The original draft referenced "Epic 6" for app-store submission; the current roadmap (4.8 → 5 → 7) has no Epic 6. App-store submission is now folded into Block 7 of this Epic - the EMF 2026 deadline pressure makes the single-Epic / single-owner model the right call rather than splitting submission into a follow-up Epic.
8. **EMF 2026 deadline preserved.** EMF 2026 runs 16-19 July 2026 - 65 days away from the refinement date. Epic 5 is sprint-mode against this deadline with a hard 30 June 2026 app-store submission target. The "hybrid parallel sequencing with Epic 4.7" rationale is moot (4.7 is closed) but the underlying deadline urgency it was responding to is real, and the block plan is sized for a 5-6 week dev window plus 2-3 weeks of submission and review.
9. **Block plan added.** Original draft deliberately under-refined ("won't be visible until first contact"). First-pass block structure now drafted; Block 1 (platform familiarisation) is the place to validate or refine the rest.
10. **Spec section references mapped to protocol manual where applicable.** Architecture spec §X.Y references are now protocol manual links for protocol behaviour, architecture spec links for cross-cutting concerns (§15 photosensitivity, §3.4 dev tooling, §7.3 backgrounded behaviour).

Second pass on 2026-05-12 (afternoon) addressing user-supplied direction:

11. **Group ID semantics codified explicitly.** New section [Group ID semantics (canonical, shared with StickC)](#group-id-semantics-canonical-shared-with-stickc) captures the rules: `target_group == 0` is the broadcast group (every receiver renders regardless of own group); `target_group == N` only matches receivers with own group N; a receiver whose own group is 0 only renders broadcasts. First-boot default is random {1, 2, 3} on both StickC and Tildagon. Original Notion draft was loose on this and got it wrong in the protocol-manual-as-shipped (which has now been corrected).
12. **First-boot random group implemented on StickC.** [src/modes/persistence.cpp::migrate_legacy_nvs_keys()](../../src/modes/persistence.cpp) now picks a random value in {1, 2, 3} and persists on first boot when the `slv_group` key has not been written. Native test seam allows deterministic verification; three new native unit tests cover the path. Tildagon receiver app replicates the same behaviour in MicroPython per Block 5.
13. **Future Tildagon master mode acknowledged.** Per Jason's revision, current Tildagon implementation is slave-only, but future versions are expected to add a master mode that is architecturally different from the StickC's audio-driven master - likely button-driven, external mic via hexpansion, or Art-Net / DMX upstream. This is a follow-up Epic, not in scope here.
14. **Manual master mode added as stretch goal.** EMF audience will tinker; the Tildagon should offer a button-fire interaction that emits `LIGHT_COMMAND` over ESP-NOW. Hard constraint: manual-master mode MUST transmit on channel 1 (hobby) only. Channel 11 (show) is reserved for the official master.
15. **Test modes added as stretch goal.** A small in-app fixed-effect tester for verifying a badge works in a quiet area without a master in range.
16. **Configuration menu in scope.** Group ID, channel, Calm Mode toggle, brightness - all reachable from the standard Tildagon navigation, all persistent.
17. **Protected show channel surfaced** as open design question Q5. For EMF 2026 the protection is operator-enforced (only the official master transmits on channel 11); cryptographic channel protection tracked as a separate Tier 1 security Epic.

The original "intentionally under-refined" framing still applies to Block 1 and subsequent: the Tildagon platform may surface surprises that change the downstream block plan, and the plan should adapt rather than press on with a stale design.

## Implementation status (2026-05-12)

The Tildagon app is at [ratcliffej/nocturnation-tildagon](https://github.com/ratcliffej/nocturnation-tildagon) (private). Blocks 1-7 shipped at the protocol layer with 120 host-side pytest tests passing.

### Shipped

- **Block 1** (platform familiarisation): minimal App-framework app, `deploy.sh` wraps the `mpremote exec` recursive wipe + `cp -r` + `reset` cycle so the per-deploy gotchas (`cp -r` nesting, `rm -r` not honouring `__pycache__/`) don't recur.
- **Block 2** (ESP-NOW receive): frame parser, 16-deep dedup ring on `(source_id, sequence_number)`, hop-count > 3 drop, channel auto-scan state machine. Bench-verified channel 11 receive on real hardware.
- **Block 3** (perimeter LEDs): 12-LED ring renderer with per-LED ASR envelopes, chance gate, Calm Mode caps. `PatternDisable` emitted at start so the badge's patterndisplay service doesn't fight the renderer.
- **Block 4** (LCD pulse): single full-screen ASR wash, Calm Mode disables entirely, Full mode caps at 60 % peak.
- **Block 5** (settings + menu): persistent `Settings` (Calm Mode, group, channel) at `/nocturnation_settings.json`; in-app `Menu` opens on button C; class+group routing on inbound LIGHT_COMMAND per protocol manual §4.2.
- **Block 6** (NO SIGNAL + backgrounded + MUSIC_EVENT): 3 s gap detection per protocol manual §6.2; perimeter LED tick moved into background_task so the ring keeps animating when backgrounded (§7.3); DROP/BREAKDOWN MUSIC_EVENTs synthesise local fires.
- **Block 7 prep**: `tildagon.toml` manifest written per EMF app-store schema; `CHANGELOG.md` written; README documents the public-repo + topic + tag submission workflow.

### Outstanding hardware verification (deferred to operator-at-bench)

- **M5↔Tildagon interop session**: master Stick + Tildagon + bracelets running DynamicShow against real music. Confirm Tildagon visibly reacts in coordination with the bracelets.
- **Battery drain measurement**: idle Tildagon vs receiver-app Tildagon, 2-hour window.
- **MUSIC_EVENT (DROP/BREAKDOWN) hardware verification**: find a track that fires the M5's DropDetector reliably and confirm Tildagon synthesises the local whiteout / blue fade.
- **NO SIGNAL hardware verification**: power off master mid-show; confirm Tildagon shows NO SIGNAL within 3 s; power master back on, confirm it clears.
- **Backgrounded behaviour hardware verification**: minimise NocturNation, switch to another badge app, confirm the perimeter ring keeps animating; return to NocturNation, confirm clean resumption.

### Outstanding submission steps (operator-driven)

- **Repo visibility flip**: `nocturnation-tildagon` is currently private (Jason's choice for the iteration phase). EMF app-store crawler at <https://apps.badge.emfcamp.org/> only sees public repos. Submission requires going public.
- **GitHub topic**: add `tildagon-app` to the repo's topic list so the crawler picks it up.
- **Release tag**: create a `v1` tag and a GitHub release; the crawler reads the tagged release.
- **Submission tracking**: 15 minutes after release, the app appears at <https://apps.badge.emfcamp.org/>. EMF community-driven review; budget for one revision round.

### Known follow-ups not gating EMF 2026 ship

- **Q6 channel-steering**: `wlan.config(channel=N)` on STA_IF rejects a second channel-change call with `RuntimeError 0xffffffff`. Current behaviour falls back to first-successful-channel; operator aligns master manually. Three avenues remain (`wlan.disconnect()` dance, `AP_IF`, or drop auto-scan entirely) - tackle when post-EMF.
- **Q5 protected show channel**: manual-master mode firmware-constrained to channel 1 if implemented; channel 11 is operator-enforced-exclusive for EMF 2026. Cryptographic protection deferred to a separate Tier 1 security Epic.

### Test counts

120/120 host-side pytest tests passing on CPython 3.10+. Breakdown:
- 8 frame parser (protocol manual annex C reference vectors).
- 26 dedup ring.
- 13 channel scan state machine.
- 10 receive pipeline (parse + dedup + hop check).
- 19 perimeter renderer (envelope, chance gate, freq cap, brightness cap).
- 17 LCD renderer (Calm/Full toggle, envelope, caps).
- 19 settings (coercion, JSON round-trip, corrupt-file fallback).
- 8 signal tracker.
- 13 MUSIC_EVENT synthesis (DROP / BREAKDOWN routing fields).
- 7 (other / misc).
