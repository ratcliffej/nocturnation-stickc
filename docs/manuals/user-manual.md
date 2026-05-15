---
title: "NocturNation user manual"
status: Draft
firmware_version: "v0.5"
notion_url: https://www.notion.so/35ebd067740580369c67c6738fe3f6d0
notion_id: 35ebd067740580369c67c6738fe3f6d0
last_synced: 2026-05-12
sync_direction: bidirectional
---

# NocturNation user manual

> A practical guide to running NocturNation at a venue: what it is, how it works, how to set it up, how to configure it, and what to do when it misbehaves.

**Firmware version covered**: v0.5 (`include/firmware_version.h`).
**Reference hardware**: M5StickC Plus2 and M5StickS3 (the Sticks); PixMob X4 Gen 3.1 bracelets.

---

## Quickstart

If you only have five minutes:

1. Flash one Stick as the master, one or more Sticks as slaves. Same firmware on every device.
2. Power on the master. Wait through the boot splash; it lands in **AutonomousMaster** mode by default.
3. Power on each slave. They auto-scan and find the master. The status pip on the slave screen turns solid when traffic is being received.
4. Play music. Wave a slave in the rough direction of the bracelets. Press Button 1 on the master to cycle between **Simple Beat** and **Dynamic** shows.
5. If you need to change settings, long-press Button 1 from any mode to enter the configuration tree. Long-press Button 2 from the show screen to pick a different show.

For anything beyond this - including why some bracelets do not respond to a particular show, why the slave shows NO SIGNAL, or how to address a subset of bracelets - read on.

## Contents

1. [Theory of operation](#1-theory-of-operation)
2. [Hardware](#2-hardware)
3. [Installing the firmware](#3-installing-the-firmware)
4. [Configuration](#4-configuration)
5. [Modes and shows](#5-modes-and-shows)
6. [Troubleshooting](#6-troubleshooting)
7. [Glossary](#7-glossary)
8. [Index](#8-index)

---

## 1. Theory of operation

For a visual reference alongside this chapter, see [flow-diagrams.md](flow-diagrams.md) - Mermaid diagrams covering system topology, the master analyser pipeline, dispatch fan-out, and the slave receive flow.

### 1.1 What NocturNation is

NocturNation is a distributed crowd-lighting system. One device (the **master**) listens to music through its microphone, detects beats and structural events (drops, breakdowns), and decides moment by moment what lights should do. It broadcasts those decisions over a short-range radio link to one or more **slaves**, which act as range-extending repeaters and infra-red transmitters. Each slave fires those decisions at the crowd as infra-red light commands, which are picked up by **bracelets** worn by the audience.

The system is deliberately one-way: the master talks, the slaves and bracelets listen. There is no audio routing from the front-of-house mixer, no network back to a control surface, and no per-bracelet identity. Everything the system does is driven by what the master microphone hears, in real time, with under a hundred milliseconds of end-to-end latency.

### 1.2 Why distributed

Three reasons.

**Coverage.** A single Stick's infra-red LED reaches roughly five to fifteen metres of clear line of sight, depending on which Stick and how it is oriented (see [section 2.3](#23-ir-radiation-patterns)). A medium-sized venue needs three or four slaves spaced around the room to reach every bracelet. The slaves do not need to hear the music; they only need to be in radio range of the master and within infra-red line of sight of part of the audience.

**Redundancy.** ESP-NOW, the radio protocol used between Sticks, is a broadcast medium with no acknowledgements. Each light command is transmitted three times in quick succession to absorb the occasional lost frame. A slave that receives any of the three copies fires the command; a deduplication ring on the receive side ensures it only fires once.

**Operator division of labour.** One operator can run the master from front of house while helpers stationed around the room hold or stand the slaves. The slaves need no configuration during a show; they keep running on whatever channel and group filter you set in advance.

### 1.3 How the master decides

The master runs an audio analyser continuously. It samples the microphone at 16 kHz, computes a thirty-two-band logarithmic spectrum every twenty-five milliseconds, and feeds the spectrum into three classifiers:

- A **BeatDetector** that watches the lowest eight bands (roughly 30 Hz to 150 Hz, where kick drums live) for sudden energy spikes. The detector keeps a one-second history of band energy and fires when the current frame exceeds the running mean by 2.2 standard deviations, with a 200 millisecond refractory period to suppress double-fires.
- A **DropDetector** that compares short-window energy (two seconds, "right now") to long-window energy (ten seconds, "the recent past"). When the ratio crosses 1.8 the master emits a DROP event; when it falls below 0.4 it emits BREAKDOWN. A four-second cooldown prevents the detector from re-firing across sustained passages.
- A set of **music descriptors** that summarise the spectrum each frame: spectral centroid (centre of energy on the frequency axis, a proxy for brightness), broad-band energy, and density (how peaky versus how smeared the spectrum is). These feed into the Dynamic show's colour mapping.

The detectors run on the master only; slaves never analyse audio. Tuning history for the detectors lives in [include/dal/analyser/beat_detector.h](../../include/dal/analyser/beat_detector.h) and is captured in the architecture spec.

### 1.4 How the bracelets work

PixMob X4 Gen 3.1 bracelets are off-the-shelf passive infra-red receivers. They wake on an infra-red command, render the light envelope embedded in the command (attack, sustain, release, with a probabilistic "chance" gate), and then return to standby. They have no on-board state between commands beyond a brief residual envelope; if you fire a new command before the previous envelope finishes, the new envelope replaces the old.

Bracelets ship from the factory pre-assigned to one of thirty-one groups (the group number is encoded into the bracelet's electronics at random and is not user-configurable). The infra-red command carries a five-bit group filter byte: a bracelet only responds to a command whose filter byte is zero (broadcast) or matches its own group. This means the operator cannot target a specific bracelet, but can target a subset of the audience: at a venue with bracelets pre-distributed to all groups uniformly, addressing group 1 will light up roughly one bracelet in thirty-one.

The PixMob infra-red encoding is reverse-engineered from upstream work by James Wilson (see [acknowledgements](#acknowledgements)). NocturNation parity-tests every transmitted byte against a Python reference encoder to keep behaviour locked to that upstream.

### 1.5 Bracelet timing and residual state

Bracelets render the envelope encoded in each infra-red command and then drop back to standby, but there is a brief window during the fade-out where they remain receptive. If a new command lands inside that window, the bracelet stitches the two envelopes together; the operator sees colour artefacts in fades or truncated tails on twinkles. This is why high-cadence shows (Rainbow at ~25 ms cycle) look cleaner than sparse ones (Whiteout, Sparkle): consecutive fires keep bracelets in a fresh, fully-overwritten state.

NocturNation manages residue in the **show**, not the dispatch layer: each show picks an envelope duration that fits inside its fire cadence, so the envelope completes naturally before the next fire arrives. Sparkle, for example, runs a 960 ms envelope on a 1100 ms cadence (a ~140 ms safety gap).

An earlier Epic 4.7 build inserted a zero-RGB "reset primer" command in the dispatch layer before every non-trivial fire, on the theory that an extra clear command would scrub residue. Bench testing in Epic 4.8 showed the opposite: the extra IR traffic overloaded the bracelet receivers, and only Rainbow (which already skipped the primer via an idle gate) rendered reliably. The primer was removed; today the dispatch sends exactly one IR command per `render_fx` call.

### 1.6 Class-and-group addressing

Every render command on NocturNation is addressed by a pair of bytes: **target class** and **target group**. The class identifies what kind of device should render the command:

- `0x00` All - every class accepts the command (this is the value used for "broadcast" in everyday operation).
- `0x01` Light - infra-red light bracelets such as PixMob.
- `0x02` Screen - the local LCD on a Stick.
- `0x03` MultiLedScreen - reserved for the Tildagon-class targets that arrive in [Epic 5](https://www.notion.so/358bd067740581b19551d158d658df76).

Values 0x04 to 0xFF are reserved for future device classes (accelerometer sticks, smoke machines, large-format LED panels).

The group is a one-byte filter from `0x00` to `0xFF`. Zero is the **broadcast group**: every device renders a command addressed to group zero, regardless of which group the device is itself configured for. Any other group number addresses only devices whose own group matches exactly. For PixMob bracelets the group range is constrained to 0..31 by the IR protocol; for slaves on the radio link the range is the full byte. A slave's group is set by the [Group menu item](#41-top-level) at the top of the configuration tree.

A slave whose own group is set to zero is in no specific group and only renders broadcasts; it does not act as a receive-side wildcard. Fresh devices are assigned a random group from 1, 2, or 3 at first boot (see [section 4.1](#41-top-level) below) so that a fleet of newly-flashed Sticks naturally distributes across the three drum groups that the Dynamic show routes kick / snare / hi-hat to.

The combined `class:group` form is shown in the developer documentation as a four-hex-digit string (`"00:00"` for global broadcast, `"01:07"` for light-class group 7, `"02:00"` for every screen). Operators rarely interact with the syntax directly; it appears in serial diagnostics and in show authoring.

### 1.7 The wireless link

The Sticks talk to each other over **ESP-NOW**, a connection-less broadcast protocol on the 2.4 GHz band that piggybacks on the 802.11 Wi-Fi physical layer without joining an access point. Each frame is sent as a vendor-specific action frame and is received by every Stick on the same Wi-Fi channel. NocturNation uses three of the standard 2.4 GHz channels:

| Channel | Role | Notes |
|---|---|---|
| 1 | Hobby / open community | Default master channel; suggested for everyday work and small gatherings |
| 6 | Advanced operator override | Available but rarely used |
| 11 | Show / commercial | Suggested for large public deployments |

The master picks a channel in [Connectivity > ESP-NOW > Master Channel](#43-connectivity). The slave defaults to **auto-scan**, in which case it cycles through channels 11 and 1 and locks onto whichever is currently broadcasting (channel 11 is checked first because show traffic takes priority). A slave can also be locked to a single channel if you want predictability.

Every frame the master sends is transmitted three times in quick succession on the same channel. Each frame carries a sequence number; slaves keep a sixteen-deep ring of recently-seen sequence numbers and ignore duplicates. The signal-quality bars on the slave screen show how many of the recent expected sequences were actually seen. The system never reports raw RSSI; it reports delivered fidelity, which is what an operator actually cares about.

### 1.8 The heartbeat

The master broadcasts a one-byte **heartbeat** at 1 Hz so that idle slaves know it is still alive. To keep duty cycle low, the heartbeat is suppressed whenever the master has sent any other frame in the last second; during a noisy passage with frequent kicks, no explicit heartbeats are needed.

A slave that goes more than three seconds with no traffic at all displays **NO SIGNAL** and runs no local effect. It does not promote itself to master; it does not improvise. The master is the single source of truth, and a missing master is shown as a clear failure mode rather than masked by a local fallback.

### 1.9 Why bracelets are pre-grouped, not paired

It is tempting to imagine bracelets being paired to specific operators or to seats. They are not. Bracelets are factory-programmed to random groups and remain that way; there is no return path for the bracelet to tell the master anything about itself.

In practice this means the addressing you can do at a venue is **statistical**: addressing group 7 lights up roughly one in thirty-one bracelets, distributed uniformly across the audience. Addressing all groups in sequence creates a "sparkle" pattern. The Dynamic show uses this to route kick drums to group 1, snare hits to group 2, and hi-hat onsets to group 3, producing visibly different reactions from different segments of the crowd to different parts of the song.

A future NocturNation-native bracelet (planned but not in this firmware) will support operator-driven group assignment via a paired return path.

---

## 2. Hardware

### 2.1 Supported Sticks

NocturNation runs on two M5Stack form-factor boards. They share the same firmware and the same configuration tree; the build system selects the right hardware abstraction layer per board.

| Property | M5StickC Plus2 | M5StickS3 |
|---|---|---|
| MCU | ESP32-PICO-V3-02 | ESP32-S3-PICO-1-N8R8 |
| Microphone | PDM (SPM1423) | I2S (ES8311 codec) |
| IR transmitter | GPIO 19 | GPIO 46 |
| IR receiver | None | GPIO 42 |
| Bluetooth | BLE 4.2 | BLE 5.0 |
| PSRAM | None | 8 MB |
| Form factor | 24 x 48 x 14 mm | 24 x 48 x 14 mm |
| Status | First-class (legacy hardware) | First-class (current reference) |

Both Sticks have a small LCD, two user buttons (BtnA and BtnB), and a Lithium battery. The S3's power button is owned by its PMIC chip in hardware (short press = soft reset, long press = power off) and is not reachable by firmware; the Plus2's BtnPWR is dropped from the firmware-side mapping for cross-host consistency.

The Plus2 is end-of-life from M5Stack but remains fully supported. Buy the S3 for new deployments; keep the Plus2 in service for as long as the existing ones keep working.

### 2.2 PixMob bracelets

NocturNation targets PixMob X4 Gen 3.1 bracelets. These are widely available second-hand from concert merchandise channels, typically in batches of dozens to hundreds. Earlier PixMob generations use the same infra-red encoding and should work but have not been bench-verified by the project; report any compatibility issues against [the repository](https://github.com/ratcliffej/nocturnation-m5).

Each bracelet has a CR1632 coin cell, a single RGB LED behind a diffuser, and an infra-red photodiode on the visible face. They wake on an infra-red command, render the command, and return to deep sleep within a few seconds. Battery life on fresh cells is approximately one large event (eight hours of intermittent activity).

The bracelets cannot be re-grouped from the host. The group assignment is factory-set and uniformly distributed across thirty-one groups within a batch.

### 2.3 IR radiation patterns

The two Sticks have markedly different infra-red radiation patterns and this affects how you deploy them.

- The **Plus2 IR LED** is nearly omnidirectional. It throws a roughly spherical pattern with strong response within five metres in all directions, falling off gracefully out to ten metres. One Plus2 can illuminate a small room from any orientation.
- The **S3 IR LED** is more focused. The strongest response is in a roughly thirty-degree cone in front of the device, with usable range out to fifteen metres or so on-axis. Off-axis falloff is sharp; bracelets behind the S3 receive almost nothing.

In practice:

- **Small venues** (one room, fewer than fifty bracelets): one Plus2 master placed centrally works on its own.
- **Medium venues** (large room, fifty to two hundred bracelets): one master plus two to four slaves, ideally distributed at corners or along long walls. Mix Plus2 and S3 freely; orient the S3s toward the densest crowd areas.
- **Large venues** (multiple rooms, hundreds of bracelets): one master at front of house, slaves at every aisle and corner; use the slave-as-repeater toggle to extend radio range without adding IR sources.

### 2.4 Antenna orientation

The Sticks transmit Wi-Fi (and hence ESP-NOW) via a small ceramic patch antenna on the rear of the board. The radiation pattern is broadly hemispherical, biased toward the rear. Two Sticks placed face-to-face on a table will have weaker radio coupling than two Sticks placed back-to-back or both screen-side-up. For maximum radio range, stand slaves on their butts (USB-C connector down, screen vertical) and orient them so the rear face points roughly toward the master.

A slave with the slave-as-repeater toggle enabled retransmits every accepted frame. Chained repeaters extend radio range significantly at the cost of additional radio latency per hop (around five milliseconds per hop). The default is no repeating; turn it on only when you have measured a coverage gap.

---

## 3. Installing the firmware

### 3.1 Prerequisites

- A USB-C cable that supports data (cheap charging-only cables will not work).
- A clone of the repository: `git clone https://github.com/ratcliffej/nocturnation-m5`.
- [PlatformIO](https://platformio.org/) installed. The project assumes the CLI tool is reachable; on macOS the executable is typically at `~/.platformio/penv/bin/pio`.
- An M5StickC Plus2 or M5StickS3.

### 3.2 Building

The project ships two PlatformIO environments, one per Stick:

| Environment | Target |
|---|---|
| `m5stack-stickcplus2` | M5StickC Plus2 |
| `m5stack-stickcs3` | M5StickS3 |

Build:

```sh
pio run -e m5stack-stickcs3
```

Or to build both:

```sh
pio run
```

Build artefacts end up under `.pio/build/<env>/firmware.elf` and `.bin`.

### 3.3 Flashing

With the Stick connected by USB-C:

```sh
pio run -e m5stack-stickcs3 -t upload
```

The first flash on a fresh Stick may require holding the lower side button while you plug in the USB cable, to enter the ROM bootloader. Subsequent flashes can be done while the firmware is running; PlatformIO will reset the chip into bootloader mode automatically.

To watch the serial console after flash:

```sh
pio device monitor -e m5stack-stickcs3
```

Press `Ctrl-C` to exit.

### 3.4 Recovering a soft-bricked Stick

If you flash bad firmware and the Stick will not respond, hold the lower side button (BtnA on the Plus2, ButtonA on the S3) for ten seconds with the USB cable disconnected; this triggers a hard reset. If that fails, plug in USB while holding the lower button to force the ROM bootloader, then re-flash.

The firmware never writes to flash regions outside of its own partition table. Bricking the bootloader itself is not possible from a normal `pio run -t upload`.

---

## 4. Configuration

The configuration tree is reached by long-pressing Button 1 from any mode. Use Button 1 to advance through items (or to increment a value), Button 2 to drill into a sub-menu or commit a value, and a long press on Button 2 to back out one level.

### 4.1 Top level

The top of the tree has six items:

| Item | Type | NVS key | Default |
|---|---|---|---|
| `Group: N` | Direct action: cycles the slave receive-filter group | `slv_group` | Random 1, 2, or 3 (assigned on first boot) |
| `Show` | Picker over registered shows | `active_show` | "simple-beat" |
| `Display` | Sub-menu | - | - |
| `Connectivity` | Picker over transports | - | - |
| `Utilities` | Picker over auxiliary tools | - | - |
| `System` | Sub-menu | - | - |

The `Group: N` item is the most-used setting. It is the device-wide receive filter for the slave (or for the master when the master is itself rendering as a slave under loopback). It interacts with the master's target group as follows:

- A master broadcast (`target_group = 0`) renders on every device regardless of the device's own group setting.
- A targeted master fire (`target_group = N`, with N from 1 to 255) renders only on devices whose `Group: N` setting matches.
- A device whose own `Group` is 0 only renders broadcasts; it does not act as a receive-side wildcard.

First-boot devices are assigned a random group from 1, 2, or 3 so a fleet of newly-flashed Sticks distributes naturally across the three drum groups used by the Dynamic show (kick → group 1, snare → group 2, hi-hat → group 3). The operator can override this from the menu - cycle through 0 to 255, or factory-reset to re-roll.

### 4.2 Display

A single toggle:

- **Pulse Enable** - whether the local LCD shows a pulse animation in sync with light commands. NVS key `scr_puls_en`, default on.

### 4.3 Connectivity

A picker leading to four sub-menus:

**IR** (active):
- `Enable / Disable` - master IR transmit and slave IR transmit toggle. NVS key `ir_en`, default on.
- `Protocol` - currently PixMob only; reserved for future protocols.

**ESP-NOW** (active):
- `Master Channel` - selects 1, 6, or 11. NVS key `mst_chan`, default 1.
- `Slave Channel` - 0 (auto-scan), 1, 6, or 11. NVS key `slv_chan`, default 0.
- `Slave Repeat` - whether the slave retransmits accepted frames as a repeater. NVS key `slv_repeat`, default off.

**WiFi** (stub, reserved for future Epic):
- Enable, SSID, Password, Soft-AP mode. Not functional in v0.5.

**DMX** (stub, reserved for Epic 7):
- Carrier, Universe ID, Channel mapping. Not functional in v0.5.

### 4.4 Utilities

A picker leading to two sub-menus:

**PixMob**:
- `Set Group ID` - tools for verifying and re-confirming a bracelet's factory group (passive listening via the IR receiver on the S3).
- `Group Target Test` - fire a known colour at a single group, useful for confirming coverage.

**Level Tuning**:
- A multi-mode microphone calibration tool. Choose Live (real-time audio bars), or one of the fixed-percentage modes (25%, 50%, 75%, 100%) for calibration sweep work.

### 4.5 System

- Battery readout
- Firmware version (currently `v0.5`)
- Factory reset (clears the entire `noct` NVS namespace; requires a long confirmation press)

### 4.6 Persistence model

All configuration lives in a single non-volatile-storage namespace called `noct`. Power-cycling preserves every setting. The `System > Factory Reset` action erases the namespace; the firmware then comes up with the defaults shown in the tables above.

Some legacy keys are migrated on first boot after a firmware upgrade. The `slv_ir_grp` key from before Epic 4.65 is dropped (its function moved to the slave's `slv_group` filter); the legacy visualisation id keys (`active_vis` with values "beat-pulse" or "spectrum-bars") are migrated to the new `active_show` key with value "simple-beat".

---

## 5. Modes and shows

### 5.1 Boot flow

When the Stick powers on, the firmware runs through a fixed sequence:

1. **Splash** - the NocturNation brand-mark with a breathing N (orange-yellow at roughly 2 second period). Three seconds.
2. **Last-mode resume** - the firmware reads the `last_mode` non-volatile-storage key and enters that mode. The factory default is `AutonomousMaster`.

To interrupt the boot resume and pick a different mode, press the lower button during the splash. The firmware will drop into the **Menu** mode (the navigation hub) instead.

### 5.2 Modes

The mode finite-state-machine has six entries:

| Mode | Numeric id | Role |
|---|---|---|
| `Boot` | 0 | Transient countdown |
| `Menu` | 1 | Navigation hub |
| `AutonomousMaster` | 2 | Master-side performance (the default) |
| `Slave` | 3 | ESP-NOW receiver |
| `Config` | 4 | Settings tree (described in [section 4](#4-configuration)) |
| `Test` | 5 | Manual test harness |

To switch between modes, enter `Menu` (either at boot via the splash interrupt or by long-pressing both buttons together from any mode) and pick.

### 5.3 AutonomousMaster mode

This is where most of the action happens. The master analyses microphone audio, runs the currently-selected show, and fires light commands. The screen displays a label for the active show plus the master overlay (heartbeat indicator, battery, currently-active show name).

To pick a different show, long-press Button 2; the show picker rotates through every registered show. To open the per-show settings page (where you can change show-specific properties like Simple Beat's colour or Dynamic's groups property), long-press Button 1 once a show is active.

The system currently ships two shows:

**Simple Beat** (id `simple-beat`):
A faithful re-implementation of the pre-Epic-4.7 BeatPulse behaviour. Single colour, fires a one-shot envelope on every kick. The colour is operator-selectable via the show settings page; values are Off, Red, Green, Blue, Yellow, White. When Off is selected the show fires an RGB-zero command on every kick, which renders as nothing visible to the bracelets but exercises the IR path so latency is identical to the lit colours.

**Dynamic** (id `dynamic`):
The FFT-driven show from Epic 4.7. Maps spectral centroid to hue (warm to cool as the spectrum brightens), broadband energy to value (dim to bright), and density to the chance gate (sparse to dense). Routes kick detections to PixMob group 1, snare detections to group 2, hi-hat detections to group 3, when its `groups` property is set to 3. With `groups` set to 1 (the default), all detections are routed to group 0 (broadcast), which works on any deployment without bracelet pre-programming.

The Dynamic show's `groups` property is the most useful setting to know about: leave it at 1 for ordinary deployments where bracelet groups have not been controlled; raise it to 3 if you have manually distributed bracelets across groups 1, 2, and 3 and want to see the kick-snare-hihat split.

### 5.4 Slave mode

The slave does very little. It listens on its configured channel (or auto-scans), accepts light commands whose target class and group match its configured filter, and fires them through its local infra-red transmitter. The screen shows a small status pip (solid when receiving, hollow when idle) and a sequence-loss signal-quality strip across the top. If no traffic arrives for three seconds, the strip clears and the screen reads NO SIGNAL.

The slave does not improvise on master loss. It does not promote itself to master. It does not run any audio analyser locally.

### 5.5 Test mode

A grid of manual fires for verifying hardware. Each tile fires a single test pattern through the canonical render path; the underlying transmission is identical to what a show would do.

Test patterns include Pulse (single colour, finite envelope), Fade (gentle envelope), Rainbow (continuous high-cadence hue cycle), Sparkle (white twinkle, roughly 0.9 Hz, twenty percent chance per fire), and Whiteout (one-shot bright white). Use Test mode to verify a fresh deployment - if Pulse and Sparkle work, the entire transmission path is healthy.

### 5.6 Config mode

Described in [section 4](#4-configuration).

### 5.7 Menu mode

A scrolling list of every mode plus the entries for Test and Config. Used when you need to jump between modes without rebooting.

---

## 6. Troubleshooting

### 6.1 No bracelet response

**Symptom**: bracelets are receiving the infra-red command (you can see the bracelet wake briefly) but the colour is wrong or the envelope is truncated.

Most often a transient state-residue problem on the bracelet ([section 1.5](#15-bracelet-timing-and-residual-state)). Verify:

- Test mode's Rainbow pattern looks clean. Rainbow has the highest fire cadence and is the most forgiving of residue, so if Rainbow is also misbehaving the issue is elsewhere (low batteries, wrong group, master IR transmitter blocked).
- Whichever show is misbehaving is using an envelope length that fits inside its fire cadence. If you have customised envelope or cadence values, lengthening the fire cadence or shortening the envelope usually clears the artefacts.

If a particular show looks fine on most bracelets but wrong on one or two, those bracelets may have low batteries; swap the coin cells.

**Symptom**: no bracelets respond at all.

Check, in this order:

1. **Master is firing**: serial console shows `[espnow TX LIGHT]` lines on every kick.
2. **Slave is receiving**: status pip on the slave screen is solid, not hollow.
3. **IR is enabled on the slave**: `Connectivity > IR > Enable` is on.
4. **Group filter matches**: the master is broadcasting to group 0 (this is the default; check that no over-zealous configuration set the Dynamic show's `groups` to 3 with no group 1/2/3 bracelets distributed).
5. **Line of sight**: the slave's IR LED must see the bracelets. Walls, large bodies, and even fabric strongly attenuate infra-red.

### 6.2 NO SIGNAL on the slave

**Symptom**: slave screen reads NO SIGNAL.

The slave has gone three seconds with no traffic. Causes, in rough order of likelihood:

1. **Master is off or in a different mode**: enter AutonomousMaster on the master.
2. **Channel mismatch**: master is on channel 1, slave is locked to channel 11 (or vice versa). Either set both to the same channel, or set the slave to 0 (auto-scan).
3. **Master is very quiet**: AutonomousMaster only broadcasts on beats, and heartbeats are at 1 Hz with a skip-if-recent rule. A silent room with no detected beats and no recent fires can briefly trip the NO SIGNAL threshold; this is benign and resolves as soon as music plays.
4. **Radio range exceeded**: try moving the slave closer. If the slave starts working at half the distance, you have a range issue. Solutions: orient the slave for a clearer line of sight to the master; enable `Slave Repeat` on an intermediate slave; reduce concrete walls in the path.

### 6.3 Wrong show running

The currently-active show is shown on the master overlay. To change it, long-press Button 2 in AutonomousMaster mode. The selection persists in non-volatile storage as `active_show`.

### 6.4 Bracelets respond to one show but not another

Almost always a `chance` gate issue. Some show patterns deliberately fire with a low probability (the Sparkle pattern fires at sixteen percent chance; the Dynamic show modulates chance with spectral density and can drop to roughly four percent on smooth pads). The bracelets that "did not respond" rolled against the chance gate and lost. This is a feature.

If you want to verify a deployment without any chance gating, use Test mode's `Pulse` or `Whiteout` patterns, which fire at one hundred percent chance.

### 6.5 Slave repeater not working

The slave-as-repeater toggle requires the slave to actually have received a frame before it can rebroadcast. If the slave is not receiving (signal-quality strip is clear), it cannot repeat. Verify direct master-to-slave reception first; turn the repeater on after.

### 6.6 Low battery behaviour

The Stick's PMIC manages low-battery cutout. When the cell drops below a safety threshold, the chip resets and refuses to boot until external power is applied. The bracelets have no such cut-out; they just fade. A bracelet that responds dimly is on a flat cell.

### 6.7 Audio not detected

If AutonomousMaster is running but no `[espnow TX LIGHT]` lines appear on the serial console even with loud music nearby, the microphone is not seeing audio. Causes:

- The Plus2's PDM microphone is on the back face; if the device is screen-down on a table the microphone is muffled. Stand the Stick on its butt.
- The S3's I2S microphone is on the top edge; less directional but still benefits from a clear sound path.
- Long press into Config and visit `Utilities > Level Tuning > Live`. The on-screen bars should respond to ambient noise. If they are flat, the microphone path is broken at a hardware level.

---

## 7. Glossary

**AutonomousMaster** - the master-side runtime mode. Listens to audio, runs a Show, fires light commands. Default boot mode. See [section 5.3](#53-autonomousmaster-mode).

**BeatDetector** - the master's kick-drum onset detector. Watches low-frequency bands for energy spikes against a one-second running mean. See [section 1.3](#13-how-the-master-decides).

**BtnA, BtnB** - the two user buttons on a Stick. Button 1 is the lower button; Button 2 is the upper button. The S3's power button is owned by hardware and unreachable.

**Bracelet** - a passive infra-red receiver worn by an audience member. Reference target is the PixMob X4 Gen 3.1. See [section 2.2](#22-pixmob-bracelets).

**Chance gate** - a probabilistic filter applied to each infra-red command. A bracelet receiving a command with chance 16 rolls a sixteen-percent die and only renders on a hit. Independent dice per bracelet.

**Class** - one byte (`target_class`) carried in every render command. Identifies the device kind that should accept the command. See [section 1.6](#16-class-and-group-addressing).

**DropDetector** - the master's structural-event detector. Compares two-second short-window energy to ten-second long-window energy, fires DROP or BREAKDOWN on threshold crossings. See [section 1.3](#13-how-the-master-decides).

**Dynamic** - the FFT-driven show. Maps spectrum analysis to HSV colour and per-drum-group routing. See [section 5.3](#53-autonomousmaster-mode).

**ESP-NOW** - the wireless protocol used between Sticks. Connection-less broadcast on the 2.4 GHz band; vendor-specific 802.11 action frames. See [section 1.7](#17-the-wireless-link).

**Frame** - one transmission unit on either the ESP-NOW link (between Sticks) or the infra-red link (between Sticks and bracelets). See the [protocol manual](protocol-manual.md) for byte-level specifications.

**Group** - one byte (`target_group`) carried in every render command. Identifies a subset of devices within a class. See [section 1.6](#16-class-and-group-addressing).

**Heartbeat** - a one-byte transmission from master to slaves at 1 Hz (suppressed if other traffic was recent), indicating master is alive. See [section 1.8](#18-the-heartbeat).

**HSV** - Hue, Saturation, Value colour model. The Dynamic show works in HSV internally and converts to RGB at the wire.

**LIGHT_COMMAND** - one of the seven ESP-NOW message types. Nine-byte payload: class, group, RGB, attack/sustain/release/chance. See the [protocol manual](protocol-manual.md).

**Loopback** - the master's habit of treating itself as one of its own slaves. The dispatch path routes every light command back through the master's own infra-red transmitter and screen pulse, so the master can illuminate nearby bracelets and show a pulse on its own LCD.

**Master** - the Stick that listens to audio and decides what lights should do. Exactly one master per deployment. See [section 1.1](#11-what-nocturnation-is).

**MUSIC_EVENT** - one of the seven ESP-NOW message types. Carries DROP (1), BREAKDOWN (2), or BUILD (3, reserved). See the [protocol manual](protocol-manual.md).

**M5StickC Plus2** - the first-generation reference Stick. ESP32-PICO-V3-02, PDM microphone, omnidirectional IR. End-of-life from M5Stack but fully supported.

**M5StickS3** - the second-generation reference Stick. ESP32-S3-PICO-1-N8R8, I2S codec microphone, focused IR. Current recommended hardware.

**NocturNation** - this project. An open-source distributed crowd-lighting system. Repository: [github.com/ratcliffej/nocturnation-m5](https://github.com/ratcliffej/nocturnation-m5).

**NO SIGNAL** - the slave-screen indication that no master traffic has arrived for three seconds. See [section 6.2](#62-no-signal-on-the-slave).

**NVS** - Non-Volatile Storage. The ESP32's flash-backed key-value store. NocturNation uses the namespace `noct`. See [section 4.6](#46-persistence-model).

**PixMob** - the manufacturer of the reference bracelets. Their X4 Gen 3.1 is the target.

**Show** - a plug-in that produces light commands from analyser events. Lives in `src/shows/`. Operator-selectable from AutonomousMaster mode via long-press Button 2.

**Simple Beat** - the faithful re-implementation of the pre-Epic-4.7 BeatPulse behaviour. Single colour, fires on every kick.

**Slave** - a Stick that listens on the ESP-NOW link and re-fires light commands as infra-red. See [section 1.1](#11-what-nocturnation-is) and [section 5.4](#54-slave-mode).

**Spectrum** - the thirty-two-band log-spaced output of the master's FFT. Updated every twenty-five milliseconds; consumed by the BeatDetector, DropDetector, and music descriptors.

**Stick** - colloquial for either the M5StickC Plus2 or M5StickS3.

**Test mode** - the manual fire harness. See [section 5.5](#55-test-mode).

---

## 8. Index

This index lists significant defined terms and concepts. For run-time configuration items, see also [section 4](#4-configuration).

| Term | Section |
|---|---|
| AutonomousMaster | [5.3](#53-autonomousmaster-mode) |
| BeatDetector | [1.3](#13-how-the-master-decides) |
| Bracelet (PixMob X4 Gen 3.1) | [1.4](#14-how-the-bracelets-work), [2.2](#22-pixmob-bracelets) |
| Bracelet residue | [1.5](#15-bracelet-timing-and-residual-state) |
| Chance gate | [glossary](#7-glossary) |
| Class+group addressing | [1.6](#16-class-and-group-addressing) |
| Configuration menu | [4](#4-configuration) |
| DropDetector | [1.3](#13-how-the-master-decides) |
| Dynamic show | [5.3](#53-autonomousmaster-mode) |
| ESP-NOW transport | [1.7](#17-the-wireless-link) |
| Firmware flashing | [3.3](#33-flashing) |
| Group filter | [4.1](#41-top-level) |
| Heartbeat | [1.8](#18-the-heartbeat) |
| IR radiation patterns | [2.3](#23-ir-radiation-patterns) |
| Modes (boot, master, slave, etc.) | [5.2](#52-modes) |
| NO SIGNAL | [6.2](#62-no-signal-on-the-slave) |
| NVS persistence | [4.6](#46-persistence-model) |
| Quickstart | [quickstart](#quickstart) |
| Repeater (slave) | [4.3](#43-connectivity) |
| Show picker | [5.3](#53-autonomousmaster-mode) |
| Simple Beat show | [5.3](#53-autonomousmaster-mode) |
| Slave mode | [5.4](#54-slave-mode) |
| Splash | [5.1](#51-boot-flow) |
| Test mode | [5.5](#55-test-mode) |
| Troubleshooting | [6](#6-troubleshooting) |

---

## Acknowledgements

The PixMob infra-red encoding is reverse-engineered from upstream work by [James Wilson (jamesw343)](https://github.com/jamesw343/PixMob_IR) and contributors. NocturNation parity-tests every transmitted byte against a Python reference encoder to keep behaviour locked to that upstream.

Hardware abstraction patterns and many small implementation details follow the conventions of the [M5Unified](https://github.com/m5stack/M5Unified) library by M5Stack.

The audio analyser tuning history (BeatDetector and DropDetector) reflects bench iteration against a deliberately mixed-genre playlist; thanks to the test listeners who sat through the longer parameter sweeps.
