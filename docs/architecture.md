---
title: NocturNation Architecture Specification
status: cross-project (will move to umbrella repo when Tildagon work begins)
notion_url: https://www.notion.so/357bd0677405800b891beab0f4e0a976
notion_id: 357bd0677405800b891beab0f4e0a976
last_synced: 2026-05-09
sync_direction: bidirectional
---

**Status:** Draft v0.17 - early architecture document, expect substantial revision.
**Maintainer:** Jason Ratcliffe
---
## 1. Vision
A modular, open-source crowd-lighting system that scales from one wearer at a tribute act to a multi-node mesh covering a small festival venue, built on commodity hardware and reused commercial bracelets.
NocturNation democratises a class of show-experience that has historically required expensive vendor contracts. PixMob, Xylobands, and similar systems charge bands tens of thousands per show; a NocturNation transmitter costs roughly £30. The shift is economic as well as technical: instead of paying a vendor for fan light experiences, touring bands can sell their own branded receivers as merch and turn the lighting into a profit centre rather than a cost line.
The system has three deployment scales it must support cleanly:
- **Solo**: one device, one wearer, no infrastructure. Personal use at gigs.
- **Installation**: a small fixed rig (\~5 light points) with optional pre-programmed choreography. Art pieces, parties.
- **Distributed**: multiple master/repeater nodes covering a venue, optionally orchestrated from a laptop. Maker festivals, club nights.
Across all three, the core experience is the same: ambient music drives synchronised, beautiful lighting on wearable or installed light points.
Original prototyping work: <mention-page url="https://www.notion.so/358bd067740580bab876cd7c2b7ee6bf"/> 

Pixmob Aurora Teardown: https://goughlui.com/2025/01/14/teardown-pixmob-led-wristband-aurora-v1-7/
https://www.wsj.com/video/series/tech-behind/the-tech-behind-how-concert-led-light-wristbands-work/EAA54145-D07A-4100-8153-2EAF8D671921?mod=Searchresults_pos1&page=1

### Design principles
- **Reuse over manufacture**: existing PixMob bracelets, existing EMF Tildagon badges, existing maker hardware. New devices only when necessary.
- **Layered architecture**: hardware abstraction, audio analysis, device abstraction, orchestration, transport, and protocol are separate layers with clean interfaces. Any layer is swappable.
- **Standards where they exist**: speak Art-Net/DMX to lighting consoles, ESP-NOW to embedded peers, IR to PixMob bracelets. Don't reinvent.
- **Graceful degradation**: every node is independently functional. Loss of a master, mesh, or laptop should not stop the lighting; the worst case is loss of synchronisation.
- **Open and inspectable**: protocols documented, source published, no hidden state.
---
## 2. Architecture overview
```plain text
┌──────────────────────────────────────────────────────────────────┐
│  Application (show file, mode selection, user intent)            │
└──────────────────────────────────────────────────────────────────┘
                              ↕
┌──────────────────────────────────────────────────────────────────┐
│  Orchestration (event consumer → light command producer)         │
└──────────────────────────────────────────────────────────────────┘
                              ↕
┌──────────────────────────────────────────────────────────────────┐
│  Device abstraction layer (DAL)                                  │
│  - Device-type profiles (JSON: capabilities + params + fallbacks)│
│      Input capabilities:  mic+FFT (spectrum frames @ ~30Hz),    │
│                            buttons, IMU, network-in              │
│      Output capabilities: RGB/RGBW LEDs, IR transmit, DMX out    │
│  - Active-device registry (profile + group ID; 0 = all of type)  │
│  - The host itself is a registered device. The Plus2 profile     │
│    declares its mic, buttons, display and IR LED transport;      │
│    PixMob and Tildagon profiles declare their own capabilities.  │
│  - Bidirectional API:                                            │
│      fire_event(target, ...)        - send a command             │
│      subscribe_events(target, ...)  - receive typed events       │
│  - Per-protocol drivers: PixMob IR, ESP-NOW, DMX/Art-Net, ...    │
│    Each driver translates the DAL's typed events and commands    │
│    into / out of its wire protocol.                              │
└──────────────────────────────────────────────────────────────────┘
                              ↕
┌──────────────────────────────────────────────────────────────────┐
│  Hardware abstraction layer (HAL)                                │
│  - Mic, buttons, display, IR LED, radios, GPIO, battery, clock   │
│  - Each backend declares a flat capability set (mic, fft, ir-tx,│
│    ir-rx, esp-now, display, buttons-2x, imu, ...) which the DAL │
│    composes into the host's profile.                             │
│  - Per-host: M5StickC Plus2, Tildagon, generic ESP32 dev kits    │
└──────────────────────────────────────────────────────────────────┘
                              ↕
┌──────────────────────────────────────────────────────────────────┐
│  Hardware (board-specific peripherals)                           │
└──────────────────────────────────────────────────────────────────┘
```
The system is a single conceptual pipeline: events flow in, light commands flow out, both through the DAL. Different deployments populate different combinations of inputs and outputs by registering different devices in the DAL with different capability profiles.
### Layer responsibilities
**Application layer**: human intent. Show files, mode selections, user preferences, UI. Sits above orchestration and tells it what mood/show is currently active.
**Orchestration layer**: the artistic decisions and the live state machine - this is where `loop()` runs. Subscribes to typed events from the DAL, holds all the analysis/decision state (beat-detection thresholds, baseline flux, refractory windows, BPM tracking buffers, mode flags, show timeline position), and produces typed commands back to the DAL. Each `loop()` iteration consumes the latest events, advances state, and emits any commands the new state implies. Different "shows" are different orchestration configurations. The state machine that says "kick → red flash on group 1, snare → blue flash on group 2" lives here, as does the beat-detection logic itself: the mic capability emits raw spectrum frames, and orchestration is the layer that decides which frames represent beats worth responding to.
**Device abstraction layer (DAL)**: a single uniform, bidirectional interface for everything that produces events or accepts commands, regardless of underlying protocol or hardware. The DAL holds two registries: a **device-type catalogue** (one JSON profile per type, declaring input capabilities, output capabilities, parameters, and substitution fallbacks) and an **active-device registry** (named instances bound to a profile and a group ID; group 0 is the wildcard for all devices of that type). Crucially, **the host itself is a registered device**: the M5StickC Plus2's mic, buttons, display, and IR LED transport are declared as capabilities of the `NocturNationStickCplus2` profile, the same way `PixMobX4Gen3_1` declares RGB/ASR/group-addressing as capabilities. The mic input capability does **raw FFT only** - it emits spectrum frames (band-summary energies: bass / mid / treble plus overall RMS) at a fixed cadence (e.g. 30 Hz for a 16 kHz / 512-sample window). It does **not** emit semantic beat events; beat detection (threshold, refractory, BPM tracking) is the orchestration layer's job. This separation means beat-detection algorithms are swappable per show without changing host firmware. Button presses are an input capability of the buttons, emitting `ButtonPress` events directly. Orchestration calls `fire_event(target, ...)` against a logical name to dispatch outputs; the DAL resolves the target to its profile, checks the requested capability, and either dispatches via the appropriate protocol driver or fails silently (substitution policy is the show file composer's responsibility, not the DAL's). Orchestration calls `subscribe_events(target, ...)` to receive inputs - whether those are FFT beat events from the local mic, button presses from local buttons, ESP-NOW peer messages, or DMX instructions when in master mode. Drivers translate between the DAL's typed events/commands and their respective wire protocols (PixMob IR, ESP-NOW, DMX/Art-Net, etc.), reaching the hardware through the HAL rather than against vendor SDKs directly.
**Hardware abstraction layer (HAL)**: vendor-independent primitives for the peripherals every host needs - microphone samples, button GPIO, display framebuffer, IR LED carrier, radios, GPIO, battery state, clock ticks. **Each HAL implementation declares a flat set of named capabilities** (`mic`, `fft`, `ir-tx`, `ir-rx`, `esp-now`, `display`, `buttons-2x`, `imu`, ...) describing what the host can offer. The DAL composes the host's device profile from those HAL declarations and adds protocol-layer capabilities on top (e.g. `mic` + `fft` from HAL, plus `audio-band-summary` from the DAL's audio handler; or `ir-tx` from HAL, plus `pixmob-protocol` from the PixMob driver). Orchestration queries the DAL for what's currently available and adapts to it: a bare ESP32 with just `led` + `esp-now` becomes a NocturNation slave node naturally, same firmware, fewer features lit up; a fully-loaded StickC Plus2 gets the complete feature set. The DAL's per-host profile handlers and per-protocol drivers read and write through the HAL. Each supported host (M5StickC Plus2, Tildagon, generic ESP32 dev kit) provides a HAL implementation. The layers above never reference vendor libraries (M5Unified, Tildagon SDK, etc.) directly. The HAL is the unlock for every other layer being genuinely vendor-neutral.
**Hardware layer**: actual peripherals on the host board. Behind the HAL, this is where IR LEDs, ESP-NOW radios, serial UARTs, Ethernet sockets, microphones, and buttons physically live.
---
## 3. Hardware platforms
### 3.1 Currently supported
<table header-row="true">
<tr>
<td>Platform</td>
<td>Role</td>
<td>Notes</td>
</tr>
<tr>
<td>**M5StickC Plus2**</td>
<td>Solo controller, IR transmitter node</td>
<td>Built-in IR LED (GPIO 19), I2S mic, screen, buttons. ESP32-PICO-V3-02. Production-ready firmware exists.</td>
</tr>
<tr>
<td>**PixMob Aurora-class bracelets**</td>
<td>Wearable light points</td>
<td>IR-receive, 3 RGB LEDs. Multiple model variants.</td>
</tr>
</table>
### 3.2 Planned
<table header-row="true">
<tr>
<td>Platform</td>
<td>Role</td>
<td>Notes</td>
</tr>
<tr>
<td>**EMF Tildagon badge**</td>
<td>Lightweight node</td>
<td>ESP32-C3 with WiFi/BLE, round colour LCD, six perimeter buttons, six addressable RGB LEDs, six hexpansion connectors. MicroPython runtime. Already deployed to thousands of attendees.</td>
</tr>
<tr>
<td>**Mac/PC laptop**</td>
<td>Orchestration master, console host</td>
<td>Runs QLC+ or similar. Bridges to embedded layer via USB-serial-to-ESP32/DMX or Ethernet-to-Art-Net.</td>
</tr>
<tr>
<td>**M5Stack Atom Lite**</td>
<td>Compact transmitter node</td>
<td>£12, sugar-cube-sized. Same ESP32 family as StickC. Ideal for distributed IR transmitter nodes.</td>
</tr>
<tr>
<td>**Hackspace Shieldagon**</td>
<td>Tildagon receiver hexpansion</td>
<td>IR receive + audio in. Repurposed laser-tag boards. Becomes a custom slave device.</td>
</tr>
</table>
### 3.3 Future / under consideration
- **ESP32 with sub-GHz LoRa** (Heltec, LilyGO TTGO): for outdoor / large-area deployments where 2.4 GHz spectrum is unsuitable.
- **Custom DIY bracelet** based on ESP32-C3 SuperMini: for events where attendees don't have Tildagons but want wearables.
---
## 4. Communication layers
### 4.1 Carrier (transport medium)
<table header-row="true">
<tr>
<td>Carrier</td>
<td>Use case</td>
<td>Latency</td>
<td>Range</td>
</tr>
<tr>
<td>IR (940nm, 38kHz modulated)</td>
<td>Master → bracelet</td>
<td>\<1 ms</td>
<td>1-15m depending on transmitter power; line-of-sight</td>
</tr>
<tr>
<td>ESP-NOW (2.4 GHz)</td>
<td>Master ↔ peer nodes</td>
<td>5-30 ms typical</td>
<td>30-150m line-of-sight</td>
</tr>
<tr>
<td>USB-CDC serial</td>
<td>Laptop → embedded master</td>
<td>1-5 ms</td>
<td>Cable length</td>
</tr>
<tr>
<td>Ethernet (UDP)</td>
<td>Console → master, master → master</td>
<td>1-3 ms</td>
<td>Cable / switched network</td>
</tr>
<tr>
<td>WiFi (UDP)</td>
<td>Console → master where wired isn't practical</td>
<td>5-50 ms</td>
<td>Building scale</td>
</tr>
<tr>
<td>Sub-GHz RF (LoRa) *future*</td>
<td>Long-range outdoor</td>
<td>50-200 ms</td>
<td>500m+</td>
</tr>
</table>
### 4.2 Protocols
<table header-row="true">
<tr>
<td>Protocol</td>
<td>Layer</td>
<td>Carrier</td>
<td>Use</td>
</tr>
<tr>
<td>PixMob CommandSingleColorExt</td>
<td>Light driver</td>
<td>IR</td>
<td>Single bracelet/group: RGB + ASR + chance + group restriction</td>
</tr>
<tr>
<td>PixMob CommandSetGroupId</td>
<td>Light driver</td>
<td>IR</td>
<td>One-time bracelet group assignment</td>
</tr>
<tr>
<td>Custom ESP-NOW frame</td>
<td>Event sync</td>
<td>ESP-NOW</td>
<td>Beat events, mode changes, clock sync between peers</td>
</tr>
<tr>
<td>DMX over Enttec Pro framing</td>
<td>Console input</td>
<td>USB-CDC serial</td>
<td>QLC+ / similar console controlling master</td>
</tr>
<tr>
<td>Art-Net (ArtDmx)</td>
<td>Console input</td>
<td>UDP / Ethernet / WiFi</td>
<td>Same role as serial DMX, networked</td>
</tr>
</table>
### 4.3 Custom ESP-NOW frame format (v1)
Fixed-size 6-byte header plus optional payload. Following Art-Net's precedent, sequence numbers and time anchoring are separate concerns - the header carries a small sequence number for deduplication and ordering, and time information (when needed) is carried in a separate `TIME_SYNC` message type broadcast periodically by Tier 3 masters.
```plain text
Offset  Field             Size  Notes
─────────────────────────────────────────────────────────
0       protocol_version  1     0x01 for v1
1       source_id         1     Unique per device (1-254; 0xFF reserved for broadcast)
2       sequence_number   1     1-255 incrementing; wraps at 255 → 1.
                                 0 = sequencing disabled (matches Art-Net semantics).
3       hop_count         1     0 = original; incremented by repeaters; cap at 3
4       message_type      1     See message types below
5       payload_len       1     Bytes of payload following header
6+      payload           N     Type-specific
```
**Note on sequence_number sizing.** A 1-byte field wraps every 255 frames. At our 4 Hz hard cap (§15.1), that's a wrap window of \~64 seconds, comfortably longer than any plausible reordering window in ESP-NOW broadcast. This matches Art-Net's choice of a 1-byte sequence field at 44 Hz refresh rate (5.8s wrap window). The cost of being wrong about this is benign: a duplicate frame from across a wrap boundary is detected by other means (identical payload + same source_id + within ESP-NOW's natural latency window).
**Time anchoring.** Wall-clock time is needed only by Tier 3 (signed-cert) receivers for validating cert validity windows. To avoid imposing this cost on Tier 0/1/2 deployments, time is carried in a dedicated `TIME_SYNC` message type rather than the frame header. Tier 3 masters broadcast `TIME_SYNC` at the heartbeat rate (4 Hz); lower-tier receivers ignore it. See message types table.
### Message types (v1)
<table header-row="true">
<tr>
<td>Type</td>
<td>Name</td>
<td>Payload</td>
</tr>
<tr>
<td>0x00</td>
<td>HEARTBEAT</td>
<td>Empty. Sent at 4-10 Hz so slaves know master is alive.</td>
</tr>
<tr>
<td>0x01</td>
<td>BEAT_DETECTED</td>
<td>`strength: u8` (0-255), `bpm_x10: u16`</td>
</tr>
<tr>
<td>0x02</td>
<td>MODE_CHANGE</td>
<td>`new_mode: u8`, `palette_id: u8`</td>
</tr>
<tr>
<td>0x03</td>
<td>LIGHT_COMMAND</td>
<td>`target_group: u8` (0=broadcast, 1-3=auto-assigned coordination, 4+=specialist), RGB + envelope; allows direct remote driving</td>
</tr>
<tr>
<td>0x04</td>
<td>CLOCK_SYNC</td>
<td>`phase_in_bar: u16` (0-65535 = 0-1.0), `bpm_x10: u16`. Musical timing only - not wall-clock.</td>
</tr>
<tr>
<td>0x05</td>
<td>TIME_SYNC</td>
<td>5 bytes: `days_since_2026: u16`  • `centiseconds_today: u24` (both little-endian). Broadcast by Tier 3 masters at heartbeat rate; carries wall-clock time for cert validity. Tier 0/1/2 receivers may safely ignore. See Security RFC §6.</td>
</tr>
<tr>
<td>0xFF</td>
<td>EXTENSION</td>
<td>Reserved for future use</td>
</tr>
</table>
### Reliability strategy
- Receivers track `(source_id, sequence_number)` for last 16 frames; deduplicate.
- Master sends each event 2-3 times with the same sequence number to spread across airtime gaps.
- Repeaters rebroadcast frames they haven't already seen, with `hop_count + 1`. Cap at 3 hops.
- Heartbeat at 4 Hz lets slaves detect master loss within 1 second and fall back to autonomous mode.
- Tier 3 receivers persist the highest `(days_since_2026, centiseconds_today)` tuple seen to NVM periodically (every 10 seconds maximum). The watermark provides tamper-evidence against replay attacks; see Security RFC §6 for full design.
### 4.4 PixMob protocol (existing, documented)
Pre-existing reverse-engineered protocol implemented in `pixmob_protocol.h`. Verified bit-for-bit against jamesw343's Python encoder. Supports:
- `buildSingleColor(r, g, b, attack, sustain, release, chance, group_id)` - main runtime command
- `buildSetGroupId(group_sel, new_group_id, restrict)` - one-time bracelet setup
- `buildSetColor`, `buildCycleProfiles`, `buildTwoColors` - extended commands (model-dependent support; not all bracelets respond)
Tested working on user's bracelet model. EEPROM-write commands (`buildSetColor`, `buildCycleProfiles`) confirmed unsupported on this specific bracelet variant; rainbow effects implemented in software via repeated `buildSingleColor` calls instead.
**Field-format resolution (2026-05-06):** the `buildCycleProfiles` `profileMask` argument is, per [jamesw343/PixMob_IR/docs/ir_](https://github.com/jamesw343/PixMob_IR/blob/main/docs/ir_protocol.md)[protocol.md](http://protocol.md) §"Set Config" and [docs/](https://github.com/jamesw343/PixMob_IR/blob/main/docs/operation.md)[operation.md](http://operation.md), an 8-bit `profile_range` field - the inclusive `profile_range_lo` (low 4 bits) and `profile_range_hi` (high 4 bits) bounds of the profile id range the bracelet cycles through. The C++ port's current "8-bit mask" framing in `pixmob_protocol.h` is incorrect; the function happens to be unused in the firmware so runtime behaviour is unaffected. Correction is deferred to the §10.4 architectural-prerequisites work.
## 4.5 Group ID semantics
Group ID is a first-class concept in the Nocturnation protocol, not a PixMob-IR-specific feature. Every receiver that understands LIGHT_COMMAND has an assigned group ID, persisted across reboots. The semantics:
- **Group 0** - broadcast. All receivers act on the command. Matches PixMob's protocol semantics for compatibility.
- **Groups 1-3** - automatic coordination groups. Receivers assign themselves a random group from this range at first boot, persisted to NVM. Across an audience this gives natural colour/pattern variety without operator intervention.
- **Groups 4-31** - specialist assignment. Set explicitly via Config Mode (Tildagon UI) or via a SET_GROUP_ID command (PixMob bracelet IR). Used for VIPs, performers' own bracelets, the lighting designer's monitor unit, designated zone sub-groups, etc.
The 5-bit field width (groups 0-31) matches the PixMob protocol's existing constraint, simplifying the IR driver's translation.
Receivers that haven't been assigned a group default to group 1, ensuring something happens out of the box. Operators can verify group assignment by entering Test Mode on the receiver and triggering Group Targeting Test.
For PixMob bracelets specifically, group ID is set via the `buildSetGroupId` IR command per §4.4. For Tildagons, group ID is set via the on-device Config Mode menu and stored in app settings. Both paths produce the same protocol-level behaviour.
---
## 5. Audio analysis pipeline
Currently implemented on M5StickC Plus2 in C++. Architecture is platform-portable.
### 5.1 Pipeline stages
1. **Mic capture**: I2S PDM mic, 16 kHz mono, 512-sample windows (\~32 ms).
2. **Volume gate**: mean absolute amplitude; if below `VOLUME_GATE`, skip remaining stages and reset flux state.
3. **FFT**: 512-point real FFT with Hamming window. Produces 256 magnitude bins.
4. **Bass-band sum**: bins 2-7 (≈62-220 Hz), giving a single bass-energy scalar.
5. **Spectral flux**: rectified positive change in bass energy from previous window.
6. **Adaptive baseline**: asymmetric EMA of flux. Fast attack, slow release.
7. **Beat decision**: flux \> baseline × multiplier AND flux \> absolute floor AND time since last beat \> refractory.
8. **BPM tracking**: rolling buffer of inter-beat intervals (IBIs); reject outliers (50-200 BPM range); compute mean.
### 5.2 Tuning parameters
<table header-row="true">
<tr>
<td>Parameter</td>
<td>Default</td>
<td>Notes</td>
</tr>
<tr>
<td>`SAMPLE_RATE`</td>
<td>16000 Hz</td>
<td>Standard for ESP32 PDM mics</td>
</tr>
<tr>
<td>`FFT_SIZE`</td>
<td>512</td>
<td>\~32 ms window</td>
</tr>
<tr>
<td>`BASS_BIN_LO` / `_HI`</td>
<td>2 / 7</td>
<td>\~62-220 Hz</td>
</tr>
<tr>
<td>`VOLUME_GATE`</td>
<td>200</td>
<td>Mean abs amplitude threshold</td>
</tr>
<tr>
<td>`BASELINE_ALPHA`</td>
<td>0.02</td>
<td>EMA rate for falling baseline</td>
</tr>
<tr>
<td>`BEAT_MULTIPLIER`</td>
<td>2.5</td>
<td>flux must exceed baseline × this</td>
</tr>
<tr>
<td>`FLUX_FLOOR`</td>
<td>2000</td>
<td>Absolute minimum flux for a beat</td>
</tr>
<tr>
<td>`BEAT_REFRACTORY_MS`</td>
<td>200</td>
<td>Minimum gap between detected beats</td>
</tr>
<tr>
<td>`IBI_BUFFER_SIZE`</td>
<td>8</td>
<td>Beats averaged for BPM estimate</td>
</tr>
</table>
### 5.3 Future extensions (not yet implemented)
- **Multi-band onset detection**: separate flux/threshold for snare (1-3 kHz) and hi-hat bands. Highest impact-to-effort upgrade.
- **Structural detection**: rolling 1-4s energy and onset-density tracking for chorus/build/drop recognition.
- **Harmonic analysis (chroma features)**: hue mapping to musical key. Borderline feasible on ESP32.
- **Source separation (HPSS)**: separating drums from melodic content. Likely requires off-device processing.
- **ML-based beat tracking**: out of scope for embedded; only viable on Mac via bridge.
---
## 6. Effects catalogue
The **effect** is the unit of artistic intent that orchestration produces and that drivers translate to hardware. Each effect describes *what* the lights should do, abstracted from *how* a particular device implements it. A single effect renders differently on a PixMob bracelet (RGB+envelope IR command), a Tildagon badge (animation on six addressable LEDs plus optional screen overlay), and a future RGB-strip device (per-pixel state across many LEDs).
The set below is the v1 catalogue. Each is a primitive that orchestration can compose: a "show" is a sequence of effect invocations driven by events.
### 6.1 Effect primitives
<table header-row="true">
<tr>
<td>Name</td>
<td>Trigger</td>
<td>Description</td>
<td>Parameters</td>
<td>Status</td>
</tr>
<tr>
<td>**Pulse**</td>
<td>Per-beat</td>
<td>Single colour fired with ASR envelope on each detected beat. The current default beat-reactive behaviour.</td>
<td>colour, attack, sustain, release, target group</td>
<td>✅ Implemented</td>
</tr>
<tr>
<td>**Probability Pulse**</td>
<td>Per-beat</td>
<td>Pulse with per-target chance gating. Each receiver rolls independently and either fires or stays dark. Across multiple bracelets produces a "popcorn" twinkle effect.</td>
<td>colour, ASR, chance (4-100%), target group</td>
<td>✅ Implemented</td>
</tr>
<tr>
<td>**Random Palette Pulse**</td>
<td>Per-beat</td>
<td>Pulse with colour drawn from a small palette (typically 4-8 colours) at random per beat. All targets show the same colour but it changes between beats.</td>
<td>palette, ASR, target group</td>
<td>⏳ Designed, not implemented</td>
</tr>
<tr>
<td>**Rainbow / Hue Cycle**</td>
<td>Continuous</td>
<td>Smooth HSV cycle over a duration. Software-driven repeated single-colour commands stepping through hue at 15-30 updates/sec.</td>
<td>cycle speed (Hz), brightness, duration, target group</td>
<td>✅ Implemented</td>
</tr>
<tr>
<td>**Starlight**</td>
<td>Continuous (irregular)</td>
<td>Sparse, randomly-timed twinkles using cool/warm white palette and low chance value. Long release for fading-star quality. Designed for ambient/contemplative passages.</td>
<td>palette, mean interval, jitter, ASR, chance, target group</td>
<td>✅ Implemented</td>
</tr>
<tr>
<td>**Wave**</td>
<td>Per-beat</td>
<td>Sequential firing across an ordered group of targets. Bracelet 1 fires on beat, bracelet 2 fires 100ms later, etc. Creates a ripple across the constellation.</td>
<td>colour, ASR, inter-target delay, ordering, target group</td>
<td>⏳ Designed, not implemented</td>
</tr>
<tr>
<td>**Gradient Hold**</td>
<td>One-shot or continuous</td>
<td>Different fixed colour per target across an ordered group. Each target stays at its assigned colour with optional slow breathing. Used for the lunar/constellation gradient where each bracelet is a different shade.</td>
<td>per-target colours, sustain, optional breathe period, target group</td>
<td>⏳ Designed, not implemented</td>
</tr>
<tr>
<td>**Strobe Burst**</td>
<td>One-shot</td>
<td>Rapid succession of short-envelope pulses (typically 4-8 fires over 500-800ms). Used for drops or dramatic moments.</td>
<td>colour, burst count, inter-pulse gap, target group</td>
<td>⏳ Designed, not implemented</td>
</tr>
<tr>
<td>**Background Wash**</td>
<td>Continuous</td>
<td>Low-intensity sustained colour that holds between other effects. Provides a non-dark idle state. PixMob bracelets that support background colour write it directly to EEPROM; others simulate via long-sustain repeated commands.</td>
<td>colour, brightness</td>
<td>⏳ Designed, model-dependent</td>
</tr>
<tr>
<td>**Two-Colour Flash**</td>
<td>Per-beat</td>
<td>Brief flash of colour A followed by sustained colour B. PixMob's native CommandTwoColors. Useful for kick-and-tail effects in one IR send.</td>
<td>flash colour, sustain colour, sustain duration, target group</td>
<td>⏳ Designed, model-dependent</td>
</tr>
</table>
### 6.2 Effect composition
Effects are not mutually exclusive. The orchestration layer can stack them: a Background Wash holds a low-intensity teal across all bracelets while Pulse fires kick events on group 1 and Probability Pulse fires snare events on group 2. The driver layer is responsible for making sure simultaneous commands to overlapping targets are reasonable (typically: most-recent-wins, or per-channel blend).
### 6.3 Effects mapped to musical events
A reasonable default mapping for the orchestration layer:
<table header-row="true">
<tr>
<td>Musical event</td>
<td>Default effect</td>
</tr>
<tr>
<td>Kick (bass-band onset)</td>
<td>Pulse on "primary" group</td>
</tr>
<tr>
<td>Snare (mid-band onset, future)</td>
<td>Pulse on "secondary" group, contrasting colour</td>
</tr>
<tr>
<td>Sustained quiet section</td>
<td>Starlight + Background Wash</td>
</tr>
<tr>
<td>Build-up (rising flux density, future)</td>
<td>Hue Cycle accelerating, brightness rising</td>
</tr>
<tr>
<td>Drop (large bass spike after build, future)</td>
<td>Strobe Burst + palette switch</td>
</tr>
<tr>
<td>Chorus (sustained higher energy, future)</td>
<td>Random Palette Pulse with denser palette</td>
</tr>
<tr>
<td>Verse (lower energy)</td>
<td>Pulse on single colour, lower brightness</td>
</tr>
<tr>
<td>Outro / fadeout</td>
<td>Gradient Hold with slow breathe, fading to black</td>
</tr>
</table>
These are defaults. Per-show or per-deployment overrides are expected.
---
## 7. Display surfaces
Nocturnation runs on a heterogeneous mix of devices, several of which have a screen or other secondary display surface beyond the primary lighting output. These are useful both as **operator UI** (showing state to whoever's running the device) and as **part of the show** (the device itself becomes a visual element). The spec recognises both roles.
### 7.1 Display capabilities by platform
<table header-row="true">
<tr>
<td>Platform</td>
<td>Display</td>
<td>Resolution</td>
<td>Capabilities</td>
</tr>
<tr>
<td>**M5StickC Plus2**</td>
<td>1.14" colour TFT</td>
<td>135 × 240 px</td>
<td>16-bit colour, text, basic graphics primitives, sprites, font rendering. Backlight controllable.</td>
</tr>
<tr>
<td>**EMF Tildagon badge**</td>
<td>Round colour LCD</td>
<td>240 × 240 px (circular)</td>
<td>Full colour, MicroPython framebuffer, SDK font and icon helpers. Plus six perimeter RGB LEDs as a separate "ring" surface.</td>
</tr>
<tr>
<td>**M5Stack Atom Lite**</td>
<td>None</td>
<td>—</td>
<td>Single onboard RGB LED only. No screen.</td>
</tr>
<tr>
<td>**M5Stack Core2** *(future)*</td>
<td>2.0" touchscreen</td>
<td>320 × 240 px</td>
<td>Touch input, full colour, suitable for a portable operator console.</td>
</tr>
<tr>
<td>**Mac/PC**</td>
<td>Laptop display</td>
<td>—</td>
<td>QLC+ console UI, custom Python visualisations via TouchDesigner or similar.</td>
</tr>
</table>
### 7.2 Display roles
Nocturnation treats the display as a separately-addressable resource within a node, not coupled to the lighting effect. A node can simultaneously be running a Pulse effect on its lighting output *and* showing a BPM readout on its screen. Roles include:
- **Operator UI**: status (current mode, BPM, battery, network state), parameter readouts (FFT level meter, beat indicator), error messages, configuration screens. The current StickC Plus2 firmware uses the display almost exclusively for this.
- **Show element**: the screen becomes part of the visual output. Pulses on the lighting output can be accompanied by full-screen colour washes that match. Tildagon badges in the audience can show animations beyond what their six LEDs support: pulsing concentric circles, scrolling text ("NULLSECTOR", song titles), iconography (band logos, EMF logo, lunar phase glyphs).
- **Diagnostic**: live FFT spectrum, IBI history, ESP-NOW frame counters, last-received-from indicator. Useful for development and tuning, can be toggled off for performance.
- **Idle / ambient**: when no music is playing or the device is paused, the screen shows something pleasant - a subtle hue cycle, a clock, the Nocturnation logo. Avoids the "dead device" appearance.
### 7.3 Tildagon-specific considerations
The Tildagon badge is unusual because it's both a *receiver* (in the audience, animating with the show) and a *personal device* (its owner's badge for the festival, with their own apps and identity). The display treatment must respect that:
- The Nocturnation receiver app runs as one of many apps on the badge. It only "owns" the display while it's the foreground app.
- When in the foreground, the show animation runs full-screen on the round LCD plus the six perimeter LEDs.
- When backgrounded, the perimeter LEDs continue to react (ESP-NOW listening continues), but the screen shows whatever foreground app the user has chosen.
- An opt-in "intense mode" gives the show full-screen treatment even when in another app, for users who want the full effect during a known show window.
### 7.4 Display-event abstraction (proposed)
Following the same layered pattern as the light driver abstraction, a future display abstraction would let orchestration emit display intents independent of platform:
```plain text
display.show_palette(colours)         # full-screen colour wash
display.show_text("NULLSECTOR", scroll=true)
display.show_icon("moon_full")
display.show_meter(value, range)      # generic value indicator
display.show_idle()                   # default ambient
```
The StickC Plus2 implements these by drawing on its LCD; the Tildagon implements them on its round screen (and may extend with circle-specific effects); the Atom Lite ignores them (no display); the Core2 might present them as a richer console-style UI.
This abstraction is not yet implemented and is on the medium-term roadmap. For v1 of the spec, display behaviour is platform-specific and not part of the cross-device protocol.
---
## 8. Node operating modes and UI
Every Nocturnation node runs the same conceptual state machine, regardless of hardware platform. The UI surface differs (StickC Plus2 has 3 buttons + screen; Tildagon has 6 buttons + round screen; Atom Lite has 1 button + 1 LED) but the *modes* and the *transitions between them* are common.
### 8.1 Boot flow
On power-on, every node displays a boot screen and starts a **5-second countdown to default mode**. During the countdown, any button press interrupts and presents the mode-selection menu. If no input is received, the node enters **Slave Mode** automatically.
Slave Mode is the universal default because it's the safer assumption: a node that defaults to slave does nothing intrusive on its own; it just listens for a master and acts when one is heard. This makes multi-device deployments trivial (plug them all in, exactly one is promoted to Master via menu) and avoids the failure mode where two co-located devices both think they're the master and broadcast competing event streams.
The single-device autonomous use case (StickC Plus2 at the Coldplay tribute act, no network) is reached by interrupting the countdown and selecting Autonomous Master - one extra interaction at startup in exchange for the simpler default. The boot menu remembers the last-used mode for that device, so a regularly-used Master node just needs Btn-A pressed once during the boot countdown to confirm.
### 8.2 Runtime modes
<table header-row="true">
<tr>
<td>Mode</td>
<td>Behaviour</td>
<td>Required hardware</td>
</tr>
<tr>
<td>**Autonomous Master**</td>
<td>Run local audio analysis, fire local outputs (IR/LED/screen), broadcast beat events on ESP-NOW for any listeners. Continues running indefinitely whether or not audio is present - silence is treated as a valid state, not a failure. Optional **Audio-silence failover** config (default OFF; see §8.4) demotes the node to Slave Mode after a configurable silence period, intended for unattended deployments only.</td>
<td>Mic + at least one output</td>
</tr>
<tr>
<td>**Slave**</td>
<td>Listen on configured ESP-NOW channel for events; fire local outputs in response. Optionally rebroadcasts received frames if the **Repeat** config option is enabled (off by default; see §8.4). Falls back to a local idle effect (subtle hue cycle, starlight, etc.) after N seconds without master heartbeat.</td>
<td>ESP-NOW + at least one output</td>
</tr>
<tr>
<td>**Config**</td>
<td>Configuration menus; outputs muted while in this mode.</td>
<td>Any UI</td>
</tr>
<tr>
<td>**Test**</td>
<td>Manual triggering of effect primitives for hardware validation and play; mic and ESP-NOW listening suspended.</td>
<td>Any UI + at least one output</td>
</tr>
<tr>
<td>**Idle / Off**</td>
<td>All outputs muted; ESP-NOW listening suspended. Display shows clock or logo. Long-press of power button enters this mode from anywhere.</td>
<td>None</td>
</tr>
</table>
Note: "Relay" is no longer a top-level mode. Repeating received frames is a sub-behaviour of Slave Mode, controlled by the **Repeat** config option (§8.4). This means a single device can simultaneously fire local outputs *and* rebroadcast for further reach, without the user having to choose between the two.
### 8.3 Mode-selection menu
Reached by interrupting the boot countdown, or via the in-app menu. Top-level options:
- **Start Slave Mode** *(default after countdown if no interrupt)*
	- Channel ID (default channel, with countdown to auto-select; override with key combination to choose non-default channel)
	- Group ID (which bracelet group to drive on local IR output, if applicable)
- **Start Master Mode**
	- Channel ID (canonical channel selected by default and protected by key combination; non-canonical channels visibly labelled)
	- Start
- **Test Mode** (see §8.5) - top-level, deliberately easy to reach
- **Config** (see §8.4)
### 8.4 Config tree
Reorganised into carrier-then-protocol structure for consistency. Options not relevant to the current hardware are hidden.
- **Audio**
	- Enable / Disable (where mic available)
	- Show live FFT spectrum
	- Show live beat detection meter
	- Tuning parameters (volume gate, beat multiplier, refractory; see §5.2)
- **IR**
	- Enable / Disable
	- Protocol
		- PixMob Aurora (CommandSingleColorExt)
		- Nocturnation native *(future, when bespoke devices defined)*
	- Group ID assignment (one-time bracelet setup workflow; see §10 open questions)
- **ESP-NOW**
	- Enable / Disable
	- Channel number
	- Source ID (for Master/Relay modes)
- **WiFi**
	- Enable / Disable
	- SSID + password (for joining venue WiFi)
	- Or: Soft-AP mode (creates own "Nocturnation" SSID)
- **DMX/Art-Net** (where supported)
	- Carrier (USB-serial Enttec Pro / Art-Net over WiFi or Ethernet)
	- Universe ID
	- Channel mapping (which channels drive which group)
- **System**
	- Firmware version
	- Default boot mode
	- Factory reset
	- Battery / power status
### 8.5 Test mode
For hardware validation and bracelet setup verification. Each test fires a known-good command sequence and shows what should happen on the screen so the operator can confirm the result matches.
- **Pulse Test** - cycle through Red, Green, Blue, White at 1 Hz with the standard punchy envelope.
- **Fade Test** - cycle through Red, Green, Blue, White with long-attack/long-release envelopes.
- **Rainbow Test** - smooth hue cycle for 6 seconds.
- **Sparkle Test** - probabilistic firing at CHANCE_50 with random palette colours, for 10 seconds.
- **White Out** - sustained white at maximum brightness, for testing range and IR LED health.
- **Group Targeting Test** *(IR-equipped masters only)* - fire each group ID 1-5 in turn, with on-screen indication of which group should respond. For verifying group assignment after EEPROM setup.
### 8.6 UI implementation per platform
<table header-row="true">
<tr>
<td>Platform</td>
<td>Input</td>
<td>Display surface</td>
<td>Notes</td>
</tr>
<tr>
<td>**M5StickC Plus2**</td>
<td>Btn A (select), Btn B (cycle), Btn PWR (back/power)</td>
<td>1.14" TFT, \~5 lines of text at size 2</td>
<td>Three buttons sufficient for menu navigation; long-press PWR for idle/off.</td>
</tr>
<tr>
<td>**EMF Tildagon**</td>
<td>6 perimeter buttons</td>
<td>240×240 round LCD</td>
<td>Richer UI possible; could use radial menu following the badge's circular form.</td>
</tr>
<tr>
<td>**M5Stack Atom Lite**</td>
<td>1 button</td>
<td>1 RGB LED</td>
<td>Limited UI; uses LED colour to indicate mode (red=Master, blue=Slave, white=Test). Long-press to cycle modes; single-press for default action within mode. Config done via serial console or one-shot "setup mode" via button held during boot.</td>
</tr>
</table>
### 8.7 Open design questions
- **Boot countdown duration**: 5s fixed, configurable, or platform-dependent?
- **How does a Slave node indicate "I'm not receiving"?** Idle effect after timeout is the default; should there also be a visual cue (flashing red dot on screen, etc.)?
---
## 9. Deployment topologies
### 9.1 Solo (current)
```plain text
┌───────────────┐
│ M5StickC Plus2│ ─── IR ──→ PixMob bracelet (worn)
│ (audio + IR)  │
└───────────────┘
```
Self-contained device. Audio analysis local, IR transmission local, no network. Battery-powered or USB-powered.
### 9.2 Installation (constellation art piece)
```plain text
┌───────────────┐         ┌─────────────┐
│ M5StickC Plus2│ ─ IR ─→ │ Bracelet 1  │
│  + USB power  │         │ (group 1)   │
│               │ ─ IR ─→ │ Bracelet 2..5
└───────────────┘         └─────────────┘
        ↑
  optional: USB-serial DMX from Mac running QLC+
```
Single master driving 5 bracelets via group-targeted IR. Mounted in lanterns or fixed enclosures. Optional QLC+ control from laptop for choreographed performance.
### 9.3 Distributed (NullSector / EMF)
```plain text
                       ┌──────────────┐
                       │  Mac (QLC+)  │
                       └──────────────┘
                              │
                       USB-serial DMX
                              ↓
┌──────────────────────────────────────────────────────┐
│  Master node (StickC Plus2 near speakers)            │
│  - Audio analysis                                    │
│  - Receives DMX from Mac                             │
│  - Broadcasts ESP-NOW + fires local IR               │
└──────────────────────────────────────────────────────┘
            │ ESP-NOW broadcast
   ┌────────┼────────┐
   ↓        ↓        ↓
┌─────┐  ┌─────┐  ┌─────┐
│Node │  │Node │  │Node │  Repeater / IR transmitter nodes
│  A  │  │  B  │  │  C  │  (StickC Plus2, Atom Lite, Tildagon)
└─────┘  └─────┘  └─────┘
   ↓ IR     ↓ IR     ↓ IR
 [bracelets in zone A]
                 ...

Plus: Tildagon badges in audience also receive ESP-NOW
      and animate their onboard LEDs (no IR involvement)
```
Master at fixed location handles audio analysis. Distributed transmitter nodes provide IR coverage to different zones. Tildagon badges in the audience are additional ESP-NOW receivers that light up their own onboard LEDs as part of the show.
---
## 10. Use case roadmap
### 10.1 Short-term (working / immediate)
1. **Local beat analysis and IR control for one group via StickC Plus2.** Status: ✅ working. Standalone, single-bracelet, no networking. The reference deployment for the Coldplay tribute act.
2. **Group ID setting for PixMob bracelets.** Status: ✅ verified - the 2-command `SetGroupId` + `SetGroupSel` workflow lands the new group ID on the bracelet (X4 Gen3.1, hardware-tested 2026-05-09). Implemented in `pixmob_protocol.h` and surfaced through the `AssignDeviceGroup` DAL capability.
3. **Tildagon app with ESP-NOW slave mode, StickC Plus2 master transmitting.** Status: ⏳ next-up. Brings Tildagon receiver development forward as the immediate post-tribute priority, ahead of the constellation. Covers the v1 ESP-NOW frame format end-to-end on real hardware and gives EMF app submission lead time.
### 10.2 Medium-term (EMF 2026)
- **Constellation art piece v1** - 5 bracelets, paper lantern enclosures, bird-feeder mounting, single master. Blocked on item 2 of the short-term roadmap; design contingency required if group setting doesn't work.
- Multi-node ESP-NOW mesh with deduplication and sequence tracking.
- QLC+ integration via serial DMX for pre-programmed shows.
- Distributed transmitter nodes covering NullSector.
- Documentation and open-source release.
### 10.3 Longer-term
- Multi-master support (multiple operators contributing to one show)
- Sub-GHz RF for outdoor deployments
- Custom ESP32-C3 SuperMini bracelet design (when bracelet supply runs out)
- Shieldagon receiver hexpansion as fixed installation lights
- Art-Net integration (in addition to serial DMX)
- Integration with professional lighting consoles via DMX/Art-Net
### 10.4 Architectural prerequisites
Cross-cutting refactors that need to land before the §10.2 items can cleanly proceed. These are not user-facing capabilities; they unblock everything above.
- **Hardware abstraction layer (HAL)** - decouple the firmware from the M5Unified library and from M5StickC Plus2-specific GPIO and peripheral assumptions so alternate ESP32 boards (M5 StickS3, generic ESP32 dev kits, and eventually different microcontroller families) can target the same firmware logic. Surfaced 2026-05-06 when a Wokwi experiment confirmed M5Unified's runtime board detection prevents the firmware from running on any non-StickC-family ESP32 device. The HAL is a top-level Epic 2 abstraction and must land before any of §10.2's work can claim to be vendor-neutral.
- **Device abstraction layer (DAL)** - a uniform `fire_event(target, ...)` interface above the per-protocol drivers, backed by a device-type profile catalogue (one JSON profile per type with capabilities, params, and fallbacks) and an active-device registry (named instances bound to a profile and a group ID; group 0 is the wildcard for that type). Resolves targets, checks capability against the profile, dispatches via the right driver or fails silently; substitution policy is the show file composer's responsibility, not the DAL's. See §2.
- **FX engine, mode state machine, and per-protocol drivers** - the rest of the Epic 2 refactor. Drivers (PixMob IR is the only concrete driver in this Epic; ESP-NOW comes in Epic 4, DMX in Epic 7) live inside the DAL. The mode state machine determines which event sources are active and which DAL commands are wired up at any moment.
---
## 11. Open questions
- **Tildagon app submission**: timeline, review process, opt-out UX.
- **Group ID assignment workflow** for the constellation: physical isolation procedure, labelling, persistence verification.
- **Channel selection** for ESP-NOW: hardcoded vs configurable vs adaptive scan.
- **Show file format**: QLC+ files for laptop-driven, or define a portable JSON format that's runnable from embedded?
- **Synchronisation strategy at scale**: event-based ("BEAT NOW") vs clock-based ("phase X of bar at time T") vs hybrid.
- **Power and weatherproofing** for outdoor lantern deployment.
- **Failure modes**: what does each node do when ESP-NOW drops? When master goes silent? When mic noise floor changes?
---
## 12. References
References use Harvard style. URLs verified at the time of writing (May 2026); links may move.
### 12.1 Reverse-engineering work this project builds on
Weidman, D. (2022) *Hacking the PixMob infrared protocol to enable control of PixMob wristbands at home* \[Online repository\]. GitHub. Available at: [https://github.com/danielweidman/pixmob-ir-reverse-engineering](https://github.com/danielweidman/pixmob-ir-reverse-engineering) (Accessed: 5 May 2026).
W., J. (2024) *PixMob_IR: PixMob IR Reverse Engineering* \[Online repository\]. GitHub. Available at: [https://github.com/jamesw343/PixMob_IR](https://github.com/jamesw343/PixMob_IR) (Accessed: 5 May 2026). The companion documentation files `docs/ir_protocol.md` and `docs/operation.md` in the same repository are the authoritative source for the byte-level protocol structure used in Nocturnation's `pixmob_protocol.h` C++ port.
### 12.2 Lighting control standards
Artistic Licence Engineering Ltd (2023) *Specification for the Art-Net 4 Ethernet Communication Protocol*. Available at: [https://art-net.org.uk/downloads/art-net.pdf](https://art-net.org.uk/downloads/art-net.pdf) (Accessed: 5 May 2026). Royalty-free specification covering the ArtDmx, ArtPoll, and ArtPollReply packet formats used in Nocturnation's console-input driver.
Entertainment Services and Technology Association (2008) *ANSI E1.11-2008 (R2018): Entertainment Technology - USITT DMX512-A - Asynchronous Serial Digital Data Transmission Standard for Controlling Lighting Equipment and Accessories*. ESTA Technical Standards Program. Available at: [https://tsp.esta.org/tsp/documents/docs/ANSI-ESTA_E1-11_2008R2018.pdf](https://tsp.esta.org/tsp/documents/docs/ANSI-ESTA_E1-11_2008R2018.pdf) (Accessed: 5 May 2026). The current edition (ANSI E1.11-2024) is paywalled; the 2008/R2018 PDF is freely available from ESTA's Technical Standards Program and is technically equivalent for our purposes.
Espressif Systems (n.d.) *ESP-NOW Wireless Communication Protocol* \[Product page\]. Available at: [https://www.espressif.com/en/solutions/low-power-solutions/esp-now](https://www.espressif.com/en/solutions/low-power-solutions/esp-now) (Accessed: 5 May 2026). High-level description of the connectionless Wi-Fi protocol used as Nocturnation's embedded mesh transport, including supported chip families (ESP8266, ESP32, ESP32-S, ESP32-C) and indicative range figures (200m+ open-air at +21dBm).
Espressif Systems (2024) *ESP-NOW - ESP-IDF Programming Guide* \[Online\]. Available at: [https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html) (Accessed: 5 May 2026). Authoritative API reference covering the vendor-specific action frame format, v1.0 (250-byte) and v2.0 (1470-byte) maximum payloads, optional CCMP/AES-128 encryption, and the C API (`esp_now_init`, `esp_now_send`, `esp_now_register_recv_cb`) used in Nocturnation's master and repeater implementations.
Espressif Systems (2024) *esp-now: A connectionless Wi-Fi communication protocol - User Guide* \[Online repository\]. GitHub. Available at: [https://github.com/espressif/esp-now/blob/master/User_Guide.md](https://github.com/espressif/esp-now/blob/master/User_Guide.md) (Accessed: 5 May 2026). Application-level guide covering pairing, OTA, and security features layered on the base protocol; relevant context for Nocturnation's deduplication and repeater logic.
### 12.3 Hardware platform documentation
M5Stack Technology Co., Ltd (2024) *M5StickC PLUS2 (SKU: K016-P2) - Product Documentation*. Available at: [https://docs.m5stack.com/en/core/M5StickC%20PLUS2](https://docs.m5stack.com/en/core/M5StickC%20PLUS2) (Accessed: 5 May 2026). Authoritative source for ESP32-PICO-V3-02 specifications, ST7789V2 display driver, GPIO pinout (including the IR LED on GPIO 19 and HOLD pin on GPIO 4), and the PlatformIO configuration used by Nocturnation's StickC Plus2 firmware.
M5Stack Technology Co., Ltd (2024) *M5StickC PLUS2 datasheet (K016-P2)* \[PDF\]. Mouser Electronics. Available at: [https://www.mouser.com/datasheet/2/1117/M5Stack_Technology_01102024_K016_P2-3387216.pdf](https://www.mouser.com/datasheet/2/1117/M5Stack_Technology_01102024_K016_P2-3387216.pdf) (Accessed: 5 May 2026). PDF datasheet covering electrical specifications, dimensions, and connector pinouts.
Electromagnetic Field Ltd (2024) *Tildagon Badge Documentation* \[Online\]. Available at: [https://tildagon.badge.emfcamp.org/](https://tildagon.badge.emfcamp.org/) (Accessed: 5 May 2026). Includes the badge hardware overview, hexpansion creation guide, end-user manual, and gallery of community hexpansions.
Electromagnetic Field Ltd (2024) *badge-2024-hardware: EMF 2024 Tildagon badge hardware design files* \[Online repository\]. GitHub. Available at: [https://github.com/emfcamp/badge-2024-hardware](https://github.com/emfcamp/badge-2024-hardware) (Accessed: 5 May 2026). Contains the schematics, PCB templates, and hexpansion connector specifications referenced by Nocturnation's Tildagon receiver app design.
Electromagnetic Field Ltd (2024) 'Tildagon: The EMF 2024+ badge', *EMF Camp Blog*, 18 March. Available at: [https://blog.emfcamp.org/2024/03/18/tildagon/](https://blog.emfcamp.org/2024/03/18/tildagon/) (Accessed: 5 May 2026). Background context on the reusable-badge philosophy underpinning the Tildagon design, relevant to Nocturnation's reuse-over-manufacture principle.
---
## 13. Power and battery budgets
Indicative runtimes per platform under typical Nocturnation workload. Numbers are pragmatic estimates - measure for the deployment that matters.
<table header-row="true">
<tr>
<td>Platform</td>
<td>Battery</td>
<td>Typical Nocturnation runtime</td>
<td>Notes</td>
</tr>
<tr>
<td>**M5StickC Plus2**</td>
<td>200 mAh internal Li-ion</td>
<td>\~2 hours</td>
<td>FFT and IR transmission are the dominant load. Plug in via USB-C for indefinite operation; small power bank doubles runtime trivially.</td>
</tr>
<tr>
<td>**PixMob bracelet** (X4 Gen3.1)</td>
<td>2 × AAA</td>
<td>Tens of hours of intermittent use; original 2024 Coldplay tour batteries still functional in May 2026</td>
<td>Bracelets sleep between IR commands; only LED illumination during ASR envelopes draws meaningful current. Not a near-term concern.</td>
</tr>
<tr>
<td>**PixMob bracelet** (older CR1632)</td>
<td>2 × CR1632 coin cell</td>
<td>Several hours of active use</td>
<td>Lower capacity than AAA models; replacement coin cells inexpensive and easy to source.</td>
</tr>
<tr>
<td>**EMF Tildagon**</td>
<td>Shared with badge functions (multi-day at idle)</td>
<td>\~6-8 hours under Nocturnation receive workload (estimate)</td>
<td>ESP-NOW listening + screen + LEDs noticeably faster than idle drain. Calm mode and aggressive sleep when no broadcasts heard for \>30s recommended.</td>
</tr>
<tr>
<td>**M5Stack Atom Lite**</td>
<td>None (USB-powered)</td>
<td>Indefinite (mains)</td>
<td>No internal battery. Suitable only for fixed installations or wired deployments.</td>
</tr>
</table>
For longer-running deployments, USB power banks (10,000 mAh, \~£15) extend StickC Plus2 and Atom Lite runtime to a full festival day. The bracelets are the easiest part of the rig to power - the original batteries from the events at which they were handed out routinely outlast the rest of the system.
---
## 14. Licensing
Nocturnation is open source under permissive terms.
- **Source code** (firmware, scripts, reference implementations): MIT licence. Permissive, recognised everywhere, compatible with the broader Tildagon and Arduino ecosystems and with downstream commercial use. Each repository carries a `LICENSE` file with the standard MIT text.
- **Specification and documentation** (this spec, protocol definitions, design notes): Creative Commons Attribution-ShareAlike 4.0 International (CC BY-SA 4.0). Allows free reuse with attribution; derivative works must be shared under the same licence, which keeps protocol forks open.
- **Hardware designs** (where Nocturnation defines bespoke PCBs in future, e.g. ESP32-C3 SuperMini bracelet): CERN Open Hardware Licence Version 2 - Strongly Reciprocal (CERN-OHL-S v2). Aligned with the EMF Tildagon hexpansion ecosystem norm.
Third-party material referenced in section 12 retains its own licensing; this section governs only original Nocturnation output.
The two upstream PixMob reverse-engineering repositories (Weidman; W., J.) are themselves MIT-licensed at time of writing, so Nocturnation's port of the IR protocol encoder is compatible with the MIT terms above. The Espressif documentation, Art-Net specification, and ESTA standard are referenced for interface compliance only - Nocturnation does not redistribute them.
### 13.1 Domains
The following domains have been registered for the project:
- [**nocturnation.com**](http://nocturnation.com) (registered 6 May 2026, 1-year initial term)
- [**nocturnation.net**](http://nocturnation.net) (registered 6 May 2026, 1-year initial term)
Neither domain currently resolves to live content. They are reserved for future use as the canonical project home (likely `.com` for marketing/landing, `.net` for documentation or community resources). Renewal is expected annually pending project continuity.
---
## 15. Safety considerations
Nocturnation drives flashing lights in synchrony with audio across multiple devices, including screens worn or held close to the face on Tildagon badges. This creates a real risk of triggering photosensitive epilepsy (PSE) seizures in susceptible audience members. The risk is not theoretical: roughly 1 in 4,000 people are affected, and the trigger range is well-known (3-60 Hz flashing, peak sensitivity 15-25 Hz, worsened by high contrast and large field-of-view coverage).
This section is normative for Nocturnation implementations.
### 15.1 Frequency cap
The driver layer enforces a hard per-target firing rate limit of **4 Hz**, regardless of what the orchestration layer requests. This covers all common music BPMs:
- Pop / rock / mainstream electronic (90-130 BPM): 1.5-2.2 Hz - well under cap.
- House / techno (120-140 BPM): 2-2.3 Hz - well under cap.
- Drum & bass (160-180 BPM): 2.7-3 Hz - under cap.
- Hardcore / gabber (180-220 BPM): 3-3.7 Hz - under cap.
- Extreme tempos (240 BPM): 4 Hz - at cap.
Genres above 240 BPM are deliberately niche and not a target deployment. The cap is enforced regardless of effect type, including Strobe Burst.
**Important caveat.** 4 Hz is above WCAG 2.3.1's 3-flash-per-second threshold and the equivalent Ofcom guidance. Nocturnation does not claim WCAG conformance for visual output. The 4 Hz cap is a deliberate trade-off: a 3 Hz cap would drop beats above 180 BPM and break the visual-musical sync that's the whole point of the system. We accept the residual photosensitivity risk between 3 and 4 Hz and mitigate it via the venue warnings (§15.4) and Calm Mode (§15.3) which reduces the cap further.
**Strobe Burst** (§6.1) is bound by the same 4 Hz cap as a per-target rate. It can fire 4 short pulses in 1 second; it cannot fire faster. The effect must:
- Be opt-in per show (disabled by default in any new orchestration configuration).
- Carry an explicit `strobe_intent: true` flag in the ESP-NOW frame so receivers in Calm Mode can ignore it.
- Have a maximum total duration of 800 ms per invocation.
- Be followed by at least 2 seconds of refractory before the next Strobe Burst can fire.
### 15.2 Brightness and contrast limits
Low-to-mid transitions trigger fewer seizures than off-to-max. Default behaviours:
- **Background Wash** is enabled by default in any beat-reactive show, providing a non-zero baseline brightness so pulses are mid-to-high transitions rather than off-to-high.
- **Tildagon screens** must not flash full-area between high-contrast colours (e.g., white ↔ black) at \>2 Hz. Animations should use partial-area effects (concentric rings, edge glows) or low-contrast transitions.
- **Brightness caps** in calm mode reduce maximum LED brightness to 50% of full and disable any contrast \>2:1 between consecutive frames.
### 15.3 Calm mode
Every Nocturnation receiver implements a **Calm Mode** that the user can enable locally and that persists across reboots:
- Strobe Burst effects ignored entirely.
- Frequency cap reduced to 2 Hz (further below the WCAG threshold).
- Brightness reduced to 50%.
- Tildagon screen flashing disabled; LEDs only.
Calm mode is on by default for new installations. Users opt **in** to full-effect mode, not out of calm mode. This inverts the usual default to err towards safety - particularly important given the 4 Hz cap is above WCAG threshold.
The Tildagon receiver app exposes Calm Mode as the primary setting, prominent in the UI rather than buried in a menu.
### 15.4 Pre-show warnings
For any deployment beyond solo personal use, venue warnings are **required, not optional**, because the 4 Hz cap is above WCAG threshold:
- **Venue signage** at entry to the affected area: "This installation uses synchronised flashing lights at music tempo, which may exceed photosensitive epilepsy safety thresholds. If you have photosensitive epilepsy or are otherwise susceptible to seizures triggered by flashing lights, please reconsider entering, or consult medical advice."
- **Tildagon app description** on the EMF app store includes the warning prominently, including before the install button.
- **NullSector deployment**: warning included in the camp's pre-event communications and on physical signage at the entry to the dance floor.
### 15.5 References for compliance
- W3C (2023) *Web Content Accessibility Guidelines (WCAG) 2.2, success criterion 2.3.1 "Three Flashes or Below Threshold"*. Available at: [https://www.w3.org/TR/WCAG22/#three-flashes-or-below-threshold](https://www.w3.org/TR/WCAG22/#three-flashes-or-below-threshold) (Accessed: 5 May 2026). The 3-flash-per-second threshold and the 25%-of-field-of-view rule are derived from the same Harding test that informs broadcast regulation.
- Ofcom (2017) *Guidance Note: Flashing images and patterns in television* (Ofcom Broadcasting Code Section 2). UK broadcasting standard that defines the Harding Flash and Pattern Analyser used to certify television content for photosensitive safety. The same thresholds apply by analogy to live lighting installations.
### 15.6 Open safety questions
- **How is calm mode signalled to the master?** Locally-set on each device, or master can broadcast a "calm mode active" event that overrides per-receiver configuration?
- **Should there be a "panic stop"?** A single button combination on any operator device that broadcasts "stop all effects immediately" to every receiver in range. Useful if anyone in the audience shows distress.
- **Liability boundary.** This is a hobby project, not a certified medical or broadcast device. The above measures are best-effort risk reduction, not clinical assurance. Worth being explicit in any public-facing documentation that users assume their own risk.
---
## 16. Security model (overview)
Nocturnation broadcasts plain ESP-NOW frames by default. This is appropriate for the project's primary deployment context (hobbyist, community, low-stakes) but is inadequate for any context where the show *must not* be hijacked - touring bands, ticketed events, art installations where disruption would matter.
The full security architecture - including threat model, deployment tiers, time-bounded certificate design, key management, and compute-cost analysis - is captured in a separate exploratory document because it is **not yet ready for implementation**:
[Security architecture (RFC)](https://www.notion.so/358bd0677405817b8a60de0834511ce5) *(under the Nocturnation parent page)*
The key architectural commitments that affect other parts of this spec:
- **Open algorithms, secret keys.** Nocturnation uses standard, openly-published cryptographic primitives (AES-128 for symmetric, ECDSA P-256 for asymmetric). Security derives from key management, not algorithm secrecy. Consistent with Kerckhoffs's principle.
- **Tiered deployment.** Open / MAC-whitelist / PSK / signed-cert tiers, selectable per deployment via Master Mode config (§8.4). Default for new installations is Open (Tier 0).
- **Time anchoring via timecode field.** The 4-byte `master_timecode` in the ESP-NOW frame format (§4.3) doubles as a deduplication key and as a tamper-evidence mechanism for cert validity, eliminating the need for receivers to have an RTC.
- **Hobbyists are unaffected.** The cert system is fully optional; Tier 0/1 deployments have no crypto overhead, no key management, no expiry concerns. The sophistication exists for those who need it without imposing on those who don't.
- **Calm Mode is per-receiver and unidirectional.** Slaves locally choose to ignore strobe-flagged frames; the master is not informed. Consistent with the no-back-channel design (§4.3).
See the Security RFC for the full design.
---
## 17. Glossary
- **ASR**: Attack/Sustain/Release - the envelope shape of an LED illumination.
- **BPM**: Beats Per Minute.
- **Bracelet**: a wearable IR-controlled RGB LED device (e.g. PixMob).
- **DMX**: standard lighting control protocol; 512 channels per universe, each channel a single 0-255 byte.
- **Art-Net**: DMX over UDP/IP. Allows lighting consoles to drive multiple universes over Ethernet or WiFi.
- **ESP-NOW**: Espressif's low-latency 2.4 GHz peer-to-peer protocol; runs on ESP32 chips.
- **FFT**: Fast Fourier Transform - converts audio time-domain samples to frequency-domain magnitudes.
- **Flux**: positive change in spectral energy; characterises onsets.
- **IBI**: Inter-Beat Interval - milliseconds between consecutive beats.
- **Master**: a node responsible for analysis and event generation.
- **Slave / receiver**: a node that consumes events and drives lights.
- **Group ID**: in PixMob protocol, a 5-bit identifier (1-31) assigning bracelets to addressable groups.
---
## Document history
- **v0.1** (2026-05-05): initial draft from extended design conversation. Captures architecture, protocols, hardware platforms, and roadmap. Open questions section identifies unresolved items.
- **v0.2** (2026-05-05): named MurmurNet. The "Net" suffix signals "this is a networked system" rather than a single device, sits naturally alongside other protocol names (ArtNet, EtherNet/IP, ZigBee), and disambiguates "Murmur" from its less-flattering connotations (grumbling, heart defects). The "Murmur" half evokes the ambient collective sound of a crowd — the *mor-mor* Indo-European root that's onomatopoeic for low continuous sound.
- **v0.3** (2026-05-05): added Effects catalogue (section 6) and Display surfaces (section 7). Effects formalise the artistic primitives the orchestration layer composes; display surfaces add a peer concept to lighting output, recognising that several MurmurNet devices have screens that can be used both as operator UI and as part of the show. Subsequent sections renumbered.
- **v0.4** (2026-05-05): added Node operating modes and UI (section 8). Defines the boot flow, the six runtime modes (Autonomous Master, Slave, Relay, Config, Test, Idle), the menu hierarchy, the config tree (reorganised as carrier-then-protocol), the test sub-modes, and per-platform UI mappings. Captures several open design questions for further work. Subsequent sections renumbered.
- **v0.5** (2026-05-05): added References section (section 12, Harvard style). Cites the two PixMob reverse-engineering repositories the project builds on (Weidman, James W), the Art-Net 4 specification, the ANSI E1.11-2008 (R2018) DMX512-A standard, and the official hardware documentation for both M5StickC Plus2 and EMF Tildagon. Also corrected a factual error in section 3.2: the Tildagon uses an ESP32-C3 with onboard WiFi/BLE, not an RP2040 with ESP32 co-processor. Glossary renumbered to section 13.
- **v0.6** (2026-05-05): added ESP-NOW references to section 12.2 (Espressif product page, ESP-IDF API reference, and esp-now User Guide on GitHub). Added new Licensing section (section 13) confirming MIT for source code, CC BY-SA 4.0 for specification and documentation, and CERN-OHL-S v2 for any future hardware designs. Glossary renumbered to section 14.
- **v0.7** (2026-05-05): added Power and battery budgets (section 13), revised section 10.1 short-term roadmap to reflect three concrete priorities (working solo system, group ID verification gating constellation, Tildagon receiver), moved constellation art piece to medium-term with explicit dependency note, and added Safety considerations (section 15, frequency caps, Calm Mode default-on, pre-show warning requirements, references to WCAG 2.3.1 and Ofcom photosensitivity guidance). Subsequent sections renumbered.
- **v0.8** (2026-05-05): simplified safety frequency cap to a single 4 Hz hard limit (covering all common music BPMs up to 240 BPM), removed multi-tier soft/hard structure. Made explicit that 4 Hz is above WCAG 2.3.1's 3 Hz threshold and that this is a deliberate trade-off mitigated by mandatory venue warnings and Calm Mode (which drops the cap to 2 Hz). Tightened venue-warning language to call out that warnings are required, not optional, given the WCAG-threshold trade-off.
- **v0.9** (2026-05-05): revised section 8 boot/mode design. Default mode is now Slave for all hardware (not capability-dependent) - simpler mental model, supports multi-StickC topologies trivially, makes single-master-many-slaves the natural deployment shape. Relay removed as a top-level mode and folded into Slave Mode as a Repeat config option (default off) - allows a node to drive local outputs and rebroadcast simultaneously. Test Mode promoted to top-level menu item for discoverability. Master Mode now auto-fails over to Slave after 20 minutes of audio silence. Boot menu remembers last-used mode for one-press resumption.
- **v0.10** (2026-05-05): made Audio-silence failover an opt-in config (default OFF) rather than mandatory behaviour. Master Mode now treats silence as a valid state and runs indefinitely; the failover is reserved for unattended deployments only. This avoids the footgun of silently demoting the master during legitimate breaks (between sets, quiet passages, band returning from interval). Removed two open questions resolved by previous revisions.
- **v0.11** (2026-05-06): renamed project to **Nocturnation** (from MurmurNet) and adopted new tagline "Open-source crowd lighting, conjured from cheap silicon". The name is a portmanteau of *nocturnal* and *murmuration*, encoding both the after-dark deployment context and the swarm-coordination metaphor. Murmurations of starlings actually happen at dusk as the birds return to roost, making the name etymologically tight. Updated vision (§1) to articulate the democratisation framing - replacing per-show vendor contracts with commodity hardware - and the merch-revenue economic model that makes the project commercially attractive to touring bands. Added new Security model (§16) covering threat model, three-tier deployment design (Open / Whitelist / PSK+Whitelist / signed-frames-future), PSK distribution mechanics, channel protection, and compromise recovery. Glossary renumbered to §17. All MurmurNet references throughout the document replaced with Nocturnation.
- **v0.12** (2026-05-06): two protocol/architecture refinements. (1) Replaced 16-bit `sequence_number` field in the ESP-NOW frame header with a 32-bit `master_timecode` field that serves dual purpose: deduplication key AND time anchor for cert validity. Receivers persist the highest timecode seen per source_id to NVM, eliminating the need for an internal RTC and providing tamper-evidence for replay attacks. Frame header grows from 12 bytes to 14 bytes. (2) Slimmed §16 Security model from full design to brief overview, with the full architecture moved to a separate Security architecture (RFC) page under the Nocturnation parent - acknowledging that the security design is exploratory and not ready for implementation. The architecture spec retains the architectural commitments (open algorithms, tiered deployment, time anchoring, hobbyist-unaffected, unidirectional Calm Mode) and links out for the detail.
- **v0.13** (2026-05-06): split sequence number from time anchoring, following Art-Net's precedent of using a 1-byte sequence field for deduplication and a separate packet type for timecode. Frame header drops from 14 bytes to 6 bytes - the smallest size in the project's history. Wall-clock time is now carried in a new `TIME_SYNC` message type (0x05) broadcast by Tier 3 masters at heartbeat rate; Tier 0/1/2 receivers ignore it. The split saves bandwidth on every BEAT_DETECTED and LIGHT_COMMAND frame (the vast majority of traffic) while retaining the time-anchoring capability that Tier 3 cert validity needs. Sequence wrap window at 4 Hz is \~64 seconds, comfortably longer than any plausible ESP-NOW reordering window.
- **v0.14** (2026-05-06): added §13.1 Domains, recording the registration of [nocturnation.com](http://nocturnation.com) and [nocturnation.net](http://nocturnation.net) (both 6 May 2026, 1-year initial term) as project-canonical domains pending future use.
<empty-block/>
<empty-block/>
<empty-block/>
-
<empty-block/>
<empty-block/>
