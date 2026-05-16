---
title: NocturNation Architecture Specification
status: cross-project (will move to umbrella repo when Tildagon work begins)
notion_url: https://www.notion.so/357bd0677405800b891beab0f4e0a976
notion_id: 357bd0677405800b891beab0f4e0a976
last_synced: 2026-05-11
sync_direction: bidirectional
notion_status: synced (v0.23, Show plug-in framework + multi-band onset + music descriptors + section detection + widget library)
---

**Status:** Draft v0.23 - early architecture document, expect substantial revision.
**Maintainer:** Jason Ratcliffe
---
## 1. Vision
A modular, open-source crowd-lighting system that scales from one wearer at a tribute act to a multi-node mesh covering a small festival venue, built on commodity hardware and reused commercial bracelets.
NocturNation democratises a class of show-experience that has historically required expensive vendor contracts. PixMob, Xylobands, and similar systems charge bands tens of thousands per show; a NocturNation transmitter costs roughly £30. The shift is economic as well as technical: instead of paying a vendor for fan light experiences, touring bands can sell their own branded receivers as merch and turn the lighting into a profit centre rather than a cost line.
The system has three deployment scales it must support cleanly:
- **Solo**: one device, one wearer, no infrastructure. Personal use at gigs.
- **Installation**: a small fixed rig (~5 light points) with optional pre-programmed choreography. Art pieces, parties.
- **Distributed**: multiple Director/repeater nodes covering a venue, optionally orchestrated from a laptop. Maker festivals, club nights.
Across all three, the core experience is the same: ambient music drives synchronised, beautiful lighting on wearable or installed light points.
Original prototyping work: (see https://www.notion.so/358bd067740580bab876cd7c2b7ee6bf) 
### Design principles
- **Reuse over manufacture**: existing PixMob bracelets, existing EMF Tildagon badges, existing maker hardware. New devices only when necessary.
- **Layered architecture**: hardware abstraction, audio analysis, device abstraction, orchestration, transport, and protocol are separate layers with clean interfaces. Any layer is swappable.
- **Standards where they exist**: speak Art-Net/DMX to lighting consoles, ESP-NOW to embedded peers, IR to PixMob bracelets. Don't reinvent.
- **Graceful degradation**: every node is independently functional. Loss of a Director, mesh, or laptop should not stop the lighting; the worst case is loss of synchronisation.
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
**Device abstraction layer (DAL)**: a single uniform, bidirectional interface for everything that produces events or accepts commands, regardless of underlying protocol or hardware. The DAL holds two registries: a **device-type catalogue** (one JSON profile per type, declaring input capabilities, output capabilities, parameters, and substitution fallbacks) and an **active-device registry** (named instances bound to a profile and a group ID; group 0 is the wildcard for all devices of that type). Crucially, **the host itself is a registered device**: the M5StickC Plus2's mic, buttons, display, and IR LED transport are declared as capabilities of the `NocturNationStickCplus2` profile, the same way `PixMobX4Gen3_1` declares RGB/ASR/group-addressing as capabilities. The mic input capability does **raw FFT only** - it emits spectrum frames (band-summary energies: bass / mid / treble plus overall RMS) at a fixed cadence (e.g. 30 Hz for a 16 kHz / 512-sample window). It does **not** emit semantic beat events; beat detection (threshold, refractory, BPM tracking) is the orchestration layer's job. This separation means beat-detection algorithms are swappable per show without changing host firmware. Button presses are an input capability of the buttons, emitting `ButtonPress` events directly. Orchestration calls `fire_event(target, ...)` against a logical name to dispatch outputs; the DAL resolves the target to its profile, checks the requested capability, and either dispatches via the appropriate protocol driver or fails silently (substitution policy is the show file composer's responsibility, not the DAL's). Orchestration calls `subscribe_events(target, ...)` to receive inputs - whether those are FFT beat events from the local mic, button presses from local buttons, ESP-NOW peer messages, or DMX instructions when in Director mode. Drivers translate between the DAL's typed events/commands and their respective wire protocols (PixMob IR, ESP-NOW, DMX/Art-Net, etc.), reaching the hardware through the HAL rather than against vendor SDKs directly.
**Hardware abstraction layer (HAL)**: vendor-independent primitives for the peripherals every host needs - microphone samples, button GPIO, display framebuffer, IR LED carrier, radios, GPIO, battery state, clock ticks. **Each HAL implementation declares a flat set of named capabilities** (`mic`, `fft`, `ir-tx`, `ir-rx`, `esp-now`, `display`, `buttons-2x`, `imu`, ...) describing what the host can offer. The DAL composes the host's device profile from those HAL declarations and adds protocol-layer capabilities on top (e.g. `mic` + `fft` from HAL, plus `audio-band-summary` from the DAL's audio handler; or `ir-tx` from HAL, plus `pixmob-protocol` from the PixMob driver). Orchestration queries the DAL for what's currently available and adapts to it: a bare ESP32 with just `led` + `esp-now` becomes a NocturNation Lume node naturally, same firmware, fewer features lit up; a fully-loaded StickC Plus2 gets the complete feature set. The DAL's per-host profile handlers and per-protocol drivers read and write through the HAL. Each supported host (M5StickC Plus2, Tildagon, generic ESP32 dev kit) provides a HAL implementation. The layers above never reference vendor libraries (M5Unified, Tildagon SDK, etc.) directly. The HAL is the unlock for every other layer being genuinely vendor-neutral.
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
<td>**M5StickS3**</td>
<td>Preferred reference platform; controller, IR Tx + Rx node, ESP-NOW peer</td>
<td>ESP32-S3-PICO-1-N8R8 (Xtensa LX7). Built-in IR LED (GPIO 46) and IR receiver (GPIO 42), ES8311 audio codec + MEMS mic, 1.14" ST7789P3 screen, 2 buttons + PMIC-managed power, BMI270 IMU, 8 MB PSRAM, native USB-OTG, BLE 5.0. Project's future reference platform now that the Plus2 is EOL. Audio analyser declares `beat_detection`, `drop_detection`, `spectrum_frame`, `band_summary` (Epic 4.5); declares operating points `[(16000, 512), (32000, 1024), (48000, 1024), (48000, 2048)]` of which only the canonical default is implemented in v0.22; HW-accelerated FFT via esp-dsp.</td>
</tr>
<tr>
<td>**M5StickC Plus2** (legacy)</td>
<td>Solo controller, IR transmitter node</td>
<td>ESP32-PICO-V3-02 (Xtensa LX6). Built-in IR LED (GPIO 19), PDM mic via I2S, screen, 2 buttons + AXP192-managed power, MPU6886 IMU, BLE 4.2. Manufacturer EOL; supported as legacy for existing deployments. Same firmware codebase via the HAL backend split. Audio analyser declares the same feature set as the S3 (`beat_detection`, `drop_detection`, `spectrum_frame`, `band_summary`); declares one operating point `[(16000, 512)]` (codec-limited); ANSI-fallback FFT via arduinoFFT.</td>
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
<td>Lightweight node, **Lume-only**</td>
<td>ESP32-C3 with WiFi/BLE, round colour LCD, six perimeter buttons, twelve addressable RGB perimeter LEDs (indexed 1..12), six hexpansion connectors. MicroPython runtime. Already deployed to thousands of attendees. **No microphone**, so cannot run autonomous audio analysis - therefore architecturally Lume-only. Not a limitation; a clean fit for the receiver-only role and a prototype for the future mic-less companion-app device pattern (see §4.5 forward direction).</td>
</tr>
<tr>
<td>**Mac/PC laptop**</td>
<td>Orchestration Director, console host</td>
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
<td>IR receive + audio in. Repurposed laser-tag boards. Becomes a custom Lume device.</td>
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
<td>Director → bracelet</td>
<td><1 ms</td>
<td>1-15m depending on transmitter power; line-of-sight</td>
</tr>
<tr>
<td>ESP-NOW (2.4 GHz)</td>
<td>Director ↔ peer nodes</td>
<td>5-30 ms typical</td>
<td>30-150m line-of-sight</td>
</tr>
<tr>
<td>USB-CDC serial</td>
<td>Laptop → embedded Director</td>
<td>1-5 ms</td>
<td>Cable length</td>
</tr>
<tr>
<td>Ethernet (UDP)</td>
<td>Console → Director, Director → Director</td>
<td>1-3 ms</td>
<td>Cable / switched network</td>
</tr>
<tr>
<td>WiFi (UDP)</td>
<td>Console → Director where wired isn't practical</td>
<td>5-50 ms</td>
<td>Building scale</td>
</tr>
<tr>
<td>Bluetooth LE</td>
<td>Phone-app → host (control plane); future Epic</td>
<td>10-100 ms</td>
<td>5-30m typical</td>
</tr>
<tr>
<td>Sub-GHz RF (LoRa) *future*</td>
<td>Long-range outdoor</td>
<td>50-200 ms</td>
<td>500m+</td>
</tr>
</table>
**Bluetooth role.** A future Epic adds BLE to the carrier set so a phone app can act as a control plane for any host within Bluetooth range: pick the show colour, trigger a test pulse, switch Director/Lume mode, view diagnostics. The phone speaks BLE only to the host it's directly paired with; that host then **fans the resulting ****`render_fx()`**** calls out over ESP-NOW** to every other host within radio range. Bluetooth is therefore a personal/local control link, not a show-wide protocol - ESP-NOW remains the show's distribution backbone. The HAL declares `Capability::Bluetooth` on hosts whose chips have a BLE radio (StickC Plus2 BLE 4.2, StickS3 BLE 5.0, future Tildagon BLE per its ESP32-C3); implementation is deferred to its own Epic but the capability is wired now so the API surface is ready.
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
<td>QLC+ / similar console controlling Director</td>
</tr>
<tr>
<td>Art-Net (ArtDmx)</td>
<td>Console input</td>
<td>UDP / Ethernet / WiFi</td>
<td>Same role as serial DMX, networked</td>
</tr>
</table>
### 4.3 Custom ESP-NOW frame format (v1)
Fixed-size 6-byte header plus optional payload. Following Art-Net's precedent, sequence numbers and time anchoring are separate concerns - the header carries a small sequence number for deduplication and ordering, and wall-clock time (when needed) is piggybacked onto the `HEARTBEAT` payload (`days_since_2026`, `centiseconds_today`) so Tier 3 receivers can validate cert windows without a separate dedicated frame type. Earlier spec revisions carried wall-clock in a standalone `TIME_SYNC` (0x05) message; the v0.29 trim folded it into `HEARTBEAT` and reserved 0x05 do-not-reuse.
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
**Note on sequence_number sizing.** A 1-byte field wraps every 255 frames. At our 4 Hz hard cap (§15.1), that's a wrap window of ~64 seconds, comfortably longer than any plausible reordering window in ESP-NOW broadcast. This matches Art-Net's choice of a 1-byte sequence field at 44 Hz refresh rate (5.8s wrap window). The cost of being wrong about this is benign: a duplicate frame from across a wrap boundary is detected by other means (identical payload + same source_id + within ESP-NOW's natural latency window).
**Time anchoring.** Wall-clock time is needed only by Tier 3 (signed-cert) receivers for validating cert validity windows. To avoid imposing this cost on Tier 0/1/2 deployments and to avoid a dedicated message type for what is fundamentally a piggyback signal, wall-clock time travels inside the `HEARTBEAT` payload as `days_since_2026` (`u16`) and `centiseconds_today` (`u24`). A Director without a wall-clock source MUST set both fields to zero; Tier 0/1/2 receivers simply ignore them. Tier 3 receivers treat all-zero as "unknown" and reject cert validity checks accordingly. See message types table.
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
<td>9 bytes: `tick: u32` (monotonic Director uptime tick, units implementation-defined) • `days_since_2026: u16` • `centiseconds_today: u24` (all little-endian). Sent at 1 Hz with skip-if-recent so Lumes know the Director is alive; the `tick` field is the canonical liveness/ordering signal and the date/time fields piggyback Tier 3 wall-clock anchoring. A Director without a wall-clock source sets both date/time fields to zero. Pre-v0.29 spec carried a zero-byte HEARTBEAT and a separate `TIME_SYNC` (0x05) frame; the v0.29 trim merged the two.</td>
</tr>
<tr>
<td>0x03</td>
<td>LIGHT_COMMAND</td>
<td>9 bytes: `target_class: u8` (0=all classes, 0x01=light, 0x02=screen, 0x03=multi-LED+screen, 0x04+ reserved; see §7.6 plug-in surfaces for the OutputBinding::class() taxonomy), `target_group: u8` (0=all groups, 1-255 specific), `r: u8`, `g: u8`, `b: u8`, `attack: u8` (pixmob::Time index), `sustain: u8` (pixmob::Time index), `release: u8` (pixmob::Time index), `chance: u8` (pixmob::Chance index). Lumes filter inbound LIGHT_COMMAND on `(target_class, target_group)` against each active `OutputBinding`'s `(class(), configured group)` pair and only render where both axes match (`0x00` matches anything on either axis). Pre-Epic-4.65 shape was 8 bytes without `target_class`.</td>
</tr>
<tr>
<td>0xFF</td>
<td>EXTENSION</td>
<td>Reserved for future use</td>
</tr>
</table>

**Reserved (do not reuse).** Five message-type code points were defined in earlier spec revisions but never carried real deployment traffic; the v0.29 protocol trim removed them from the wire. The numeric IDs stay reserved so a future revision MUST NOT silently reassign them, but their former semantics can be revived under fresh code points if a real consumer ever appears:

- `0x01` `BEAT_DETECTED` - wire format defined, but Lumes consume `LIGHT_COMMAND` for rendering and BPM/strength metadata had no consumer; doubling per-beat airtime was net negative.
- `0x02` `MODE_CHANGE` - never implemented; mode changes are local to each Stick.
- `0x04` `CLOCK_SYNC` - reserved for bar-locked behaviour that never shipped.
- `0x05` `TIME_SYNC` - wall-clock time is now piggybacked onto `HEARTBEAT` (see above).
- `0x06` `MUSIC_EVENT` - DROP/BREAKDOWN producers shipped in Epic 4.5 but had no consumer in the field; removed from the wire alongside the DROP/BREAKDOWN effect rendering itself (§6.3).
### Reliability strategy
- Receivers track `(source_id, sequence_number)` for last 16 frames; deduplicate.
- Director sends each event 2-3 times with the same sequence number to spread across airtime gaps.
- Repeaters rebroadcast frames they haven't already seen, with `hop_count + 1`. Cap at 3 hops.
- **Heartbeat at 1 Hz** (revised from the original 4 Hz design during Epic 4 Block 3 hardware verification; the lower duty cycle matters for battery-powered receivers and a ~3 second Director-loss detection window is acceptable for the deployment scenarios in scope). Director also **skips heartbeat if any other frame went out within the heartbeat period**, so during music with `LIGHT_COMMAND` traffic the alive-signal is implicit and heartbeat traffic drops to roughly zero. Quiet passages in the song where no kick fires for >1 s legitimately re-trigger the heartbeat so Lumes know the Director is still alive.
- **Director-loss behaviour: Lumes run a local idle effect (subtle hue cycle) and stay in Lume mode.** Lumes do NOT auto-promote to Director - a rogue Lume-promoted-to-Director would compete on the radio with the real Director if it briefly drops or returns, fragmenting the show. Promotion only happens via explicit operator action through the mode menu. (This corrects the original "fall back to autonomous mode" wording above.)
- Tier 3 receivers persist the highest `(days_since_2026, centiseconds_today)` tuple seen to NVM periodically (every 10 seconds maximum). The watermark provides tamper-evidence against replay attacks; see Security RFC §6 for full design.
### 4.4 PixMob protocol (existing, documented)
Pre-existing reverse-engineered protocol implemented in `pixmob_protocol.h`. Verified bit-for-bit against jamesw343's Python encoder. Supports:
- `buildSingleColor(r, g, b, attack, sustain, release, chance, group_id)` - main runtime command
- `buildSetGroupId(group_sel, new_group_id, restrict)` - one-time bracelet setup
- `buildSetColor`, `buildCycleProfiles`, `buildTwoColors` - extended commands (model-dependent support; not all bracelets respond)
Tested working on user's bracelet model. EEPROM-write commands (`buildSetColor`, `buildCycleProfiles`) confirmed unsupported on this specific bracelet variant; rainbow effects implemented in software via repeated `buildSingleColor` calls instead.
**Field-format resolution (2026-05-06):** the `buildCycleProfiles` `profileMask` argument is, per [jamesw343/PixMob_IR/docs/ir_](https://github.com/jamesw343/PixMob_IR/blob/main/docs/ir_protocol.md)[protocol.md](http://protocol.md) §"Set Config" and [docs/](https://github.com/jamesw343/PixMob_IR/blob/main/docs/operation.md)[operation.md](http://operation.md), an 8-bit `profile_range` field - the inclusive `profile_range_lo` (low 4 bits) and `profile_range_hi` (high 4 bits) bounds of the profile id range the bracelet cycles through. The C++ port's current "8-bit mask" framing in `pixmob_protocol.h` is incorrect; the function happens to be unused in the firmware so runtime behaviour is unaffected. Correction is deferred to the §10.4 architectural-prerequisites work.
## 4.5 Two-channel architecture
NocturNation operates on a deliberate two-channel split that encodes a social contract for the project's deployment ecosystem. This replaces the earlier single-canonical-channel design.
**Channel 1 = Hobby/Community/Open.** Permanently the project's open territory. Tier 0 traffic only. Used by hackspace gigs, EMF deployments, makers learning the protocol, the constellation art piece, anyone playing with NocturNation. Channel 1 is where new ideas live and break.
**Channel 11 = Show/Commercial.** Reserved for production deployments. Will carry Tier 1+ encrypted traffic when those Epics ship; remains Tier 0 until then. Used by touring bands, ticketed events, art installations where disruption would matter. Channel 11 is the project's professional territory.
The rationale for the specific channel choice (1 and 11 rather than any other pair from the non-overlapping {1, 6, 11} set):
- **Channel 11**: sits above the microwave-oven leak band (~2.4-2.45 GHz) that clips channels 6-9. Most consumer routers default to channel 6, making channel 11 consistently quieter on average.
- **Channel 1**: lower frequency means slightly better wall/body penetration. Distinct from channel 11 by enough spacing that the two coexist cleanly without adjacent-channel interference.
- **Channel 6 deliberately unused**: microwave leakage and default-router congestion make it the worst of the non-overlapping channels in arena conditions despite being theoretically usable.
- **Channels 2-5, 7-10 forbidden**: they overlap with their neighbours and cause adjacent-channel interference, which is *worse* than co-channel congestion.
- **Channels 12-13 not used as defaults**: not legal in all regions; we maintain global compatibility.
### Lume-side dual-channel scan with show priority
Receivers (Lumes, Tildagon receivers, future bracelets) scan both channels at startup, with channel 11 (show) prioritised over channel 1 (hobby). The scan loops until a heartbeat is heard:
```plain text
On boot, repeat indefinitely until heartbeat detected:
  Listen on channel 11 (show) for ~2 seconds:
    if heartbeat heard → lock to channel 11, exit scan
  Listen on channel 1 (hobby) for ~2 seconds:
    if heartbeat heard → lock to channel 1, exit scan
  Loop back to channel 11.

While locked to a channel:
  Listen normally on that channel only - do not switch.
  if heartbeat timeout (1 second silence per §4.3):
    Resume the scan loop above.
```
ESP-NOW radios are single-tuner devices: they cannot listen on two channels simultaneously. Channel switching takes 5-15ms and frames may be missed during the switch window. The redundant transmission strategy (3-5 sends per frame with jitter, per Epic 4) absorbs missed frames. Once locked to a channel, the receiver does not periodically check the other channel - that would cause missed show frames for no good reason.
**Implication for Lume-only devices** (bracelets without UI, future custom hardware, anything where the user can't manually choose a channel): this auto-scan logic means *they don't need to*. Show traffic always wins over hobby traffic, automatically, with no user input required. This is exactly what a wearable or zero-config receiver needs.
**Implication for Stick devices with UI**: auto-scan is the default. Operators can override via Config Mode (§8.4) to lock to a specific channel - useful when a hobbyist deliberately wants to ignore concurrent show traffic. The override is sticky across reboots.
### Director-side channel selection
Directors do not auto-scan. They transmit on a single channel chosen at startup based on their role:
- **Hobby Director** (default for personal/community use): channel 1
- **Show Director** (configured via Config Mode for commercial use): channel 11
- **Custom channel** (advanced operators, research): any of {1, 11} via key combination; channel 6 not selectable
The Director's role is selected during Director Mode startup per §8.3. Default is hobby/channel 1; switching to show/channel 11 requires explicit operator action - reflecting that show deployment is a more deliberate decision than hobby playing.
### Forward direction: distance-based effects via RSSI
Receivers capture RSSI (received signal strength) on every received frame. The primary use is operator awareness - a battery-style indicator showing whether the device has adequate signal. This is captured in Epic 4.
A secondary, longer-term direction worth noting: RSSI gives a coarse approximation of distance from the transmitter, which in a fixed-stage deployment translates to distance from the stage. This opens a class of distance-based effects that PixMob-style broadcast IR can't achieve:
- **Wave effects**: a colour pulse propagates outward from the stage, with each receiver delaying its own pulse proportionally to its RSSI. The closer to stage, the earlier the pulse fires; further away, later. Visually creates a wave moving across the crowd.
- **Intensity gradients**: receivers near the stage fire at full brightness; receivers further away fire dimmer (or vice versa). Creates a physical sense of depth across the audience.
- **Distance-zoned palettes**: red close to stage, fading through orange/yellow/green/blue as receivers get further away. A heat-map effect.
- **Ambient-only mode for back rows**: receivers below an RSSI threshold default to ambient hue cycling rather than beat-reactive flashing - keeping the energy concentrated near the stage.
- **Repeater self-discovery**: if a receiver's RSSI falls below a threshold, it can promote itself to repeater mode (with operator opt-in), extending the mesh organically.
The honest calibration: RSSI to physical distance is approximately logarithmic but heavily affected by orientation, body absorption, multipath, and antenna characteristics. A free-space path loss model gives roughly -30dBm at 1m, -50dBm at 10m, -70dBm at 30m, -85dBm at 100m, but real-world readings vary by ±10-15dBm from these for the same physical distance. So RSSI is suitable for *coarse distance bands* ("near stage" / "mid-house" / "back of room") but not for precise positioning. This is enough for the effect classes above without being enough for surgical seat-level addressing.
These effects are Tier 0 software work that runs on top of the existing protocol - no hardware changes needed beyond what Epic 4 already establishes. Implementation is deferred to a future Epic but the architectural commitment to capture RSSI in Epic 4 is what enables them.
### Multi-show coexistence: why no universe field
A question that arises naturally when comparing NocturNation to Art-Net: **should the frame header carry a universe (or scope, or namespace) field**, so that receivers can distinguish concurrent independent shows on the same channel?
The scenario worth considering: two commercial shows running concurrently at the same venue (main stage and side stage at a festival, two art installations in adjacent rooms, etc.). Both would naturally want channel 11 / show. Without some discrimination beyond channel and group, every receiver in radio range reacts to both shows simultaneously.
NocturNation's answer is **no universe field in v1**, for several reasons:
- **Existing fields can carry the load.** The `source_id` field (already in every frame header) provides per-Director discrimination. Receivers can be configured to listen only to a specific source. The `target_group` field (in LIGHT_COMMAND payload) provides per-device-collection discrimination. Show A using groups 1-15 and Show B using groups 16-31 is a perfectly serviceable workaround.
- **Frame header is deliberately minimal.** At 6 bytes, the header is already the smallest in the project's history. Adding even a 1-byte universe field is a 17% header-size increase for a feature that may never matter to most deployments.
- **Premature wire-format additions are expensive.** Once a field is in v1, removing it breaks every deployed device. Adding it later (via the EXTENSION message type 0xFF, or a v2 frame format) is much cheaper. The asymmetry favours waiting until the need is real.
- **Operational discipline is sufficient.** If two shows need to coexist on channel 11, the operators agree on group ranges before deployment. This is the same way large festivals coordinate radio frequencies, lighting universes, audio frequencies - human coordination, not automated arbitration.
**Comparison to Art-Net's universe field.** Art-Net's universe is a *channel-space identifier* - it says "this packet contains 512 DMX values for universe N". NocturNation is fundamentally event-driven, not channel-stream-driven; we don't broadcast "channel values", we broadcast "beat detected, fire colour X to group Y". So Art-Net's universe concept doesn't have a clean analogue here. Adding a universe field by analogy would be cargo-culting.
**When this would change**: if real-world deployment surfaces a multi-show-on-same-channel scenario that operational discipline cannot handle (e.g., shows that don't want to coordinate, or where group-range partitioning is too restrictive), a v2 frame format adds a `scope` or `universe` field. Tier 2+ deployments use it; Tier 0 hobby stays on v1 with no overhead. The EXTENSION message type 0xFF in v1 is the migration path. Until that need is real, the simpler frame wins.
### Forward direction: per-device addressing for K-pop-style commercial deploymentsA further commercial direction worth flagging, though significantly further out: **per-device addressing for surgical effects**.
K-pop concerts use considerably more expensive crowd-lighting devices (typically $60-100, sometimes more) than the bracelet model PixMob targets. Audiences are happy to pay these prices because the device is a souvenir they own and bring to every show, not a single-use ticket inclusion. The technical capability that makes these expensive devices interesting is **seat-linked addressing**: each device is registered to a specific seat number for a given show, allowing the show operator to address devices with surgical precision. Effects that become possible:
- **Text in the audience**: spell out words across the field of view by lighting only the devices at specific seat positions.
- **Gradients across the field**: continuous colour transitions that respect physical position rather than broadcast groups.
- **"Wave from front to back" with precision**: not RSSI-approximate, but exact - because the show knows where every device is.
- **Personalised effects**: VIP seats get distinct colours, fan club sections get coordinated patterns.
This is a fundamentally different commercial model from PixMob's:
- *PixMob model*: dumb bracelets, IR projectors do the addressing, single-use mostly, ~£20/each, ticket inclusion or short-term loan.
- *K-pop light stick model*: smart, fan-owned, expensive (£60-100+), brings to every show, customisable, status symbol within fandom culture.
For NocturNation, this is exactly the kind of long-term capability the merch-receiver economic model could support. A £60 fan-owned device justifies BLE pairing, app-based seat mapping, even GPS for outdoor venues. Per-device addressing means truly personalised effects, which justifies the price point. The fan owns the device forever, brings it to every show, becomes part of the band's community. The band makes ongoing revenue from accessory sales, special-edition versions, app-driven features.
This is *not* a commitment to build it - it's a sketch of an ambition the architecture should remain compatible with. Specifically, the protocol's group ID field (5 bits, 0-31) is too narrow for per-device addressing of large audiences, but the message-type space (0xFF) leaves room for a future EXTENSION-class message that carries a wider per-device identifier without breaking existing receivers. The architecture commits to nothing here beyond "don't paint into a corner that forecloses this direction".
### Forward direction: companion app and mic-less devices
A further commercial direction worth flagging that pulls together several existing forward directions into a coherent product vision: **a phone companion app paired with a mic-less NocturNation device**.
The scenario:
- A simpler, cheaper NocturNation device (Stick or wristband form factor) ships **without a microphone**. It receives ESP-NOW frames and lights up. That's it. No autonomous audio analysis capability.
- A **companion phone app** (iOS + Android) connects to the device via BLE. The phone does the audio analysis using its mic, then sends events to the device via BLE. The device fires lights in response to those events.
- At a **show**: app captures show metadata (which show, optionally which seat) and stores it on the device for future reference.
- At **home**: audience member who bought the device at a show takes it home, opens the app, plays Spotify, and the device lights up to their music.
This pulls together three commercial angles the architecture has already flagged separately:
**Cheaper hardware, better margins.** A mic-less device removes one of the more expensive components (high-quality microphone) and one of the more difficult engineering surfaces (FFT pipeline tuning, noise floor calibration). The device becomes simpler to build, cheaper to manufacture, and easier to support. The audio analysis happens on the phone, where it's already a solved problem.
**Home-use retention model.** This is genuinely the most strategically important angle. PixMob's bracelet model is essentially single-use - audience member wears it for the show, takes it home, never uses it again, eventually it goes to landfill. NocturNation with a companion app inverts this: the device is *useful at home*, plugged into the audience member's own music library, on demand. The device retains value to the buyer indefinitely, justifying a higher price point at the show, justifying ongoing engagement with the brand. Bands that sell branded NocturNation devices as merch get a continuing presence in their fans' lives rather than a one-night souvenir.
**Seat-capture for surgical effects.** The app can supply *better* location data than RSSI alone: GPS for outdoor venues, seat number for indoor venues with seat numbering, BLE proximity for intimate spaces. This unlocks the K-pop-style seat-linked addressing flagged earlier in this section, without requiring the show operator to manually configure each device. The audience member's app does the work; the show operator just receives well-located devices.
The technical pieces required, in order of complexity:
1. **BLE service surface on the device** (already declared as Capability::Bluetooth in §4.1, implementation deferred to its own future Epic). The app pairs with the device via standard BLE; a small custom service exposes fire-event-by-type endpoints that the app calls when it detects a beat, drop, etc.
2. **Phone-side audio analysis** (well-trodden territory; libraries exist for both iOS and Android). The app uses standard mic API + FFT, runs the same algorithms NocturNation runs natively (centroid, energy envelope, multi-band onset, section detection per Epic 4.7), and produces the same event types.
3. **Per-device identity surface** (BLE pairing handshake captures device's serial number, optional seat metadata, optional show metadata). Stored on the device persistently.
4. **App backend** (potentially - for show metadata distribution to attendees who scan a QR code at a show, or to sync seat assignments from a venue's seating chart).
The architectural commitments needed to keep this future possible:
- BLE capability declared on hardware, with implementation deferred (already done in §4.1).
- Per-device addressing via the EXTENSION message type 0xFF (already flagged in this section's K-pop subsection).
- Persistent storage on devices for seat/show metadata (small, can be added when needed).
- Protocol stability so the app doesn't need updating every time a new event type is added (the EXTENSION pattern means receivers ignore unknown messages without breaking).
**This is not a committed Epic.** It's captured here because the commercial argument is compelling enough to warrant making sure the architecture stays compatible with it, and because it pulls together several existing forward directions into a single coherent vision. The walk-before-run priority remains: ship beat detection (4.5), UI cleanup (4.6), dynamic shows (4.7), Tildagon (5), and only then revisit whether companion-app territory is the right next direction.
## 4.6 Group ID semantics
Group ID is a first-class concept in the NocturNation protocol, not a PixMob-IR-specific feature. Every receiver that understands LIGHT_COMMAND has an assigned group ID, persisted across reboots. The semantics:
- **Group 0** - broadcast. All receivers act on the command. Matches PixMob's protocol semantics for compatibility.
- **Groups 1-3** - automatic coordination groups. Receivers assign themselves a random group from this range at first boot, persisted to NVM. Across an audience this gives natural colour/pattern variety without operator intervention.
- **Groups 4-31** - specialist assignment. Set explicitly via Config Mode (Tildagon UI) or via a SET_GROUP_ID command (PixMob bracelet IR). Used for VIPs, performers' own bracelets, the lighting designer's monitor unit, designated zone sub-groups, etc.
The 5-bit field width (groups 0-31) matches the PixMob protocol's existing constraint, simplifying the IR driver's translation.
Receivers that haven't been assigned a group default to group 1, ensuring something happens out of the box. Operators can verify group assignment by entering Test Mode on the receiver and triggering Group Targeting Test.
For PixMob bracelets specifically, group ID is set via the `buildSetGroupId` IR command per §4.4. For Tildagons, group ID is set via the on-device Config Mode menu and stored in app settings. Both paths produce the same protocol-level behaviour.
Note that group IDs and the per-device addressing flagged in §4.5 are complementary, not alternative: groups handle coarse "this section vs that section" coordination cheaply, per-device addressing (if and when implemented) handles surgical positioning. A future commercial deployment might use both - group 4 for VIP front rows with per-device addressing within them, group 0 for everyone else as broadcast targets.

**Epic 4.65 update (post-architecture-stream)**: group ID widens from the original 5-bit (0-31) PixMob-derived width to a full 8-bit field (0-255) on the NocturNation wire, with a parallel `target_class` byte added to `LIGHT_COMMAND` for hardware-class addressing (light bracelets / screen devices / multi-LED+screen devices / future classes - see §7.6 for the `OutputBinding::class()` contract that drives Lume-side filtering). PixMob bracelets remain a 5-bit-group device protocol-internally; `PixMobIrBinding` enforces the 0-31 cap and drops frames addressed to higher groups with a debug log. Group 0 still means "all groups" across both PixMob IR and NocturNation-native ESP-NOW. The 32-255 group range is for future deployments needing more than 32 zones (e.g. stadium seating sections); no current consumer.
---
## 5. Audio analysis pipeline
The analyser is a HAL+DAL capability cluster. Each host's HAL provides FFT magnitudes; the DAL analyser composes them into typed events. Implementation: vendor-neutral pure functions in `src/dal/analyser/` linking unchanged into Plus2 (arduinoFFT), S3 (esp-dsp), and native test builds.
### 5.1 Pipeline stages
1. **Mic capture**: HAL backend records `fft_size` samples at the host's current operating point (default 16 kHz / 512 samples = ~32 ms window).
2. **FFT**: real FFT with Hamming window producing `fft_size/2` magnitude bins. Plus2 uses arduinoFFT (double precision); S3 uses esp-dsp ANSI fallback (float, hardware-faster than the Plus2 path).
3. **Band summaries**: the analyser core computes both surfaces in one pass over the magnitudes:
    - **3-band B/M/T roll-up**: Bass <250 Hz, Mid 250-2000 Hz, Treble 2 kHz - Nyquist. Cheap surface for kick onset and the audio meter.
    - **8-band perceptual summary** per Audible Genius music-production reference: Mud (0-20 Hz), Sub Bass (20-60), Bass (60-250), Low Mids (250-500), Midrange (500-2k), High Mids (2k-4k), Presence (4k-6k), Air (6k-20k, truncated at Nyquist below 40 kHz sample rates). Internally consistent: 3-band is a strict aggregation of 8-band.
4. **Spectrum frame**: 32 log-spaced bands covering [30 Hz, Nyquist). Director-local; not broadcast over ESP-NOW (too heavy at FFT rate).
5. **Beat detection** (sub-band adaptive threshold): per-band rolling history (40 frames ~= 1 s), mean and variance computed continuously, a watched bass-region band's magnitude exceeding (mean + k × std_dev) fires a kick beat. Self-calibrating per band so hosts with different mic SNR produce equivalent behavioural output. Tuning history captured in [`include/dal/analyser/beat_detector.h`](https://github.com/ratcliffej/nocturnation-m5/blob/main/include/dal/analyser/beat_detector.h).
6. **Multi-band onset detection** (Epic 4.7 Block 3): the same `BeatDetector` class is instantiated twice more with different watched sub-band ranges and tuning. Snare watches bands 11-23 (~200 Hz - 2 kHz) with `k=2.0` and 150 ms refractory; hi-hat watches bands 27-31 (~4-8 kHz) with `k=1.8` and 80 ms refractory (shorter so 16th-note hi-hat patterns fire cleanly). Each detector exposes `last_strength()`, a 0..255 quantisation of magnitude-vs-threshold ratio; AudioFrameEvent stamps `beat_strength`, `snare_strength`, `hihat_strength`.
7. **Drop detection** (long-window energy ratio): short window (~2 s) and long window (~10 s) of bass-roll-up energy. `ratio = short_mean / long_mean > 1.8` fires DROP; `< 0.4` fires BREAKDOWN. Arm/disarm gate prevents sustained energy re-firing across cooldown cycles - DROP is a transition event, not a persistent-state event.
8. **Music descriptors** (Epic 4.7 Block 3): three continuous per-frame surfaces stamped on every AudioFrameEvent.
    - **Centroid** (`AudioFrameEvent.centroid`): magnitude-weighted average band index of the spectrum frame, normalised to 0..255. Per-frame, no smoothing.
    - **Energy** (`AudioFrameEvent.energy`): smoothed RMS envelope (single-pole IIR, ~0.5 s time constant), log-normalised over a 5-octave span anchored at `log2(512)` so the volume gate floor reads 0 and loud music saturates 255.
    - **Density** (`AudioFrameEvent.density`): events-per-second across all three onset bands windowed over 1 s; 16 events/s saturates 255.
9. **Section detection** (Epic 4.7 Block 4): a state machine consuming the descriptors plus the drop-detector flag emits a section label per frame (`SectionType`: 0 UNKNOWN, 1 VERSE, 2 CHORUS, 3 BUILDUP, 4 BREAKDOWN, 5 VOCALS_ONLY reserved, 6 INSTRUMENTAL_BREAK reserved, 7 DROP). Implementation uses two single-pole IIRs per signal (fast ~0.5 s, slow ~2.5 s); slope = fast − slow drives BUILDUP, slow drives the CHORUS / VERSE level decisions, fast drives the BREAKDOWN sustain check. 8-frame hysteresis on transitions; BREAKDOWN has its own ~2 s sustain check; DROP latches for ~1 s after the underlying drop detector fires.
10. **BPM tracking**: rolling buffer of inter-beat intervals at orchestration layer; reject outliers (50-300 BPM range); compute median.

**Pipeline gating** (Epic 4.6 Block 7, refined through 4.7): the `SpectrumFrameEvent` fan-out is consumer-gated. The active `Show`'s `PowerProfile` declares whether it needs spectrum frames (`needs_spectrum_frame=true`); the spectrum-event dispatch path only fires when at least one consumer is subscribed via that declaration. When no active show asks for spectrum data (e.g. `SimpleBeatShow`, which only needs `on_beat_detected` from the analyser-event hooks), the per-frame 32-float copy and dispatch are skipped in `LocalDriver`; switching to a show that does need it flips the gate live. `DynamicShow` (Epic 4.7 Block 5) doesn't subscribe to spectrum frames either - it reads the 8-band perceptual summary off `AudioFrameEvent` and rolls it into its 7-band on-screen spectrum widget, leaving the spectrum-frame fan-out skipped. The analyser surface itself didn't grow — `compute_spectrum_frame()` is the same pure function — the dispatch gate is what's new.

The underlying FFT roll-up that produces `frame.spectrum` still runs unconditionally inside the mic backend because `BeatDetector` consumes the 32-band magnitudes in-pipeline; gating the FFT itself would silently break beat detection. The larger Plus2 CPU savings observed in Epic 4.6 come from Block 12's analyser micro-optimisations (constant hoisting in the spectrum-frame compositor, precomputed bin→Hz LUT, single-pass Welford variance in BeatDetector), not from this gate.
### 5.2 Capability surface
A host's analyser declares a flat set of feature flags from the `Capability` enum (see [`include/hal/hal.h`](https://github.com/ratcliffej/nocturnation-m5/blob/main/include/hal/hal.h)). Lit by Epic 4.5:
- `AnalyserBeatDetection` - produces beat events for the Show layer to consume; under v0.29 these stay Director-local and drive `LIGHT_COMMAND` fires rather than a dedicated wire frame (the pre-v0.29 `BEAT_DETECTED` wire type was retired).
- `AnalyserDropDetection` - DropDetector continues to run internally and stamp `AudioFrameEvent::music_event`, but its output is no longer broadcast on the wire. The pre-v0.29 `MUSIC_EVENT` (0x06) wire frame and the DROP/BREAKDOWN effect rendering were both removed in the v0.29 trim. The detector is kept against a future Epic that finds a real local consumer (e.g. a Director-side palette switch); on-the-wire revival would need a fresh code point per §4.3.
- `AnalyserSpectrumFrame` - emits 32-band log-spaced SpectrumFrameEvent (Director-local)
- `AnalyserBandSummary` - emits 3-band B/M/T + 8-band perceptual summary

Lit by Epic 4.7:
- `AnalyserMultiBandOnset` - snare + hi-hat onset detection over the same spectrum frame with band-specific tuning (Block 3). Stamps `snare_strength` / `hihat_strength` on AudioFrameEvent.
- `AnalyserSpectralCentroid` - per-frame centroid descriptor (Block 3). Stamps `centroid`.
- `AnalyserEnergyEnvelope` - smoothed-RMS energy descriptor (Block 3). Stamps `energy`.
- `AnalyserOnsetDensity` - events-per-second density descriptor (Block 3). Stamps `density`.
- `AnalyserSectionDetection` - section state machine (Block 4). Stamps `section`.
### 5.3 Operating points
Each host's HAL declares a list of valid `(sample_rate_hz, fft_size)` tuples. Plus2 declares one (codec-limited); S3 declares four (capable of higher sample rates and larger FFTs). The DAL exposes `configure_audio_pipeline(sample_rate_hz, fft_size)` for orchestration to pick a host-declared point. Sample rate controls range (Nyquist); FFT size controls resolution.

In Epic 4.5 only the canonical default `(16000, 512)` is implemented across all hosts; non-default operating points return explicit not-supported. Lighting up the higher operating points (notably S3's 48 kHz / 2048 for full Air-band capture and high resolution) is a future Epic. The API is host-agnostic by design - the same surface serves future phone/PC HAL backends without a rewrite.

Band-layout boundaries are specified in Hz, not bin numbers. Bin assignments are computed at runtime from the current operating point so the same code path produces correct mappings whether the analyser runs at 16 kHz / 512 FFT or 48 kHz / 2048 FFT.

`set_band_layout(preset_name)` ships with `hifi+production` as the only implemented preset (3-band B/M/T + 8-band perceptual concurrent). Named alternative presets (`dnb-4band-with-subbass`, `vocal-emphasis`) and arbitrary JSON-defined ranges via Config Mode are reserved for a future Epic; the stub means future Epics extend rather than re-architect.
### 5.4 Future extensions (not yet implemented)
- **Per-band data for vocals / instrumental sections** (post-Epic-4.7): `VOCALS_ONLY` and `INSTRUMENTAL_BREAK` are declared in `SectionType` but never fire because distinguishing them needs per-band energy that's not in the descriptor surface. The most likely path is to extend `SectionDetector` to consume the AudioFrameEvent's 8-band perceptual values directly when available, rather than the rolled-up energy scalar - vocals-heavy passages have a distinctive mid-band signature that the current single-energy IIR can't pick out.
- **Higher operating points**: implement the S3's declared `(32000, 1024)` / `(48000, 1024)` / `(48000, 2048)` points to capture the 6-20 kHz Air band that's truncated at the 16 kHz default. Same Epic likely hosts the phone or PC HAL backend the API surface was designed to be portable to.
- **Frequency-response calibration**: Config Mode tool that drives a known sweep through a calibration speaker positioned in front of the mic, measures each band's response, and produces a per-host correction curve. Three uses: diagnose mic faults (resonance peaks, dead bands), compensate for raw-response differences across hosts (Plus2 PDM vs S3 ES8311), and provide objective cross-device-consistency verification distinct from the subjective "feel" check Epic 4.5 currently relies on. Stretch: ship factory-calibration curves baked in for the canonical M5 hosts so out-of-the-box deployments get the compensation without per-device measurement.
- **Harmonic analysis (chroma features)**: hue mapping to musical key. Borderline feasible on ESP32; better suited to a Mac-side bridge.
- **Source separation (HPSS)**: separating drums from melodic content. Off-device processing.
- **ML-based beat tracking**: out of scope for embedded; only viable on Mac via bridge.
- **Haptic / IMU-driven inputs**: the architecture's mic-driven beat detection captures audio energy but not the physical sensation of a crowd jumping in time, the bass thump felt through the body, or the bracelet wearer's own movement. Hosts with IMU capability (Plus2 BMI270 equivalent, S3 BMI270, Tildagon's IMU) could supply an "audience kinetic energy" channel as an analyser-adjacent capability, complementing the audio surface. Not currently scoped to any Epic; flagged as a direction the architecture should remain compatible with.
- **Detector layer placement** (architectural retrospective): the BeatDetector and DropDetector currently live in DAL (`src/dal/analyser/`) alongside the Hz-first band layout and FFT-magnitude-to-typed-event composition. There's a reasonable argument that "what counts as a beat" and "what counts as a drop" are artistic / orchestration decisions rather than analysis decisions, and that the detectors might more naturally live in orchestration with the analyser layer producing only raw band data. Counter-argument: keeping the detectors in DAL means they're shared infrastructure across modes and could exploit per-host hardware acceleration if a future chip exposes onset detection in silicon. Either layering can work; the current choice is recorded so a future refactor doesn't relitigate it without remembering both sides.
- **Source separation (Demucs / Spleeter / Neuraliser-class)**: state-of-the-art DJ tools (Algoriddim DJay's Neuraliser, Serato Stems, Virtual DJ's stem isolation) decompose a track into per-stem activity (drums / bass / vocals / other) using neural models. A drum stem analysed for onsets gives much cleaner kick / snare / hi-hat detection than mic-FFT can, and a vocal stem could drive a "vocal-presence" effect channel. **Not feasible on ESP32 in realtime** - even small stem-separation models need tens of MB of weights and substantial floating-point compute; an S3 with 8 MB PSRAM might fit a tiny model but inference would be too slow for live use. The viable architectures are:
    * **Phone-side analysis**: phone runs the model and feeds events to the device via BLE. This is the architecture spec's existing "companion app and mic-less devices" forward direction (§4.5). The companion-app pattern was originally framed around cheaper mic-less hardware; source-separation analysis is a stronger reason to commit to that pattern, since it lets the phone's substantial compute do work the device can't.
    * **Mac/PC bridge**: laptop runs realtime stem analysis and feeds events to a NocturNation Director via USB-CDC or Art-Net. Fits the existing QLC+ professional path described in §10.3.
    * **Pre-rendered cue files**: offline Demucs / Spleeter analysis of a track produces per-stem activity tracks embedded as cue data the device plays back synchronised to the song. Closer to the QLC+-driven professional shows in §10.3 - the show file already has the analysis baked in, the device just plays it back.
  None of these are committed; flagged so the architecture remains compatible with the direction (BLE transport already declared, message-type space has room for stem-specific events, the orchestration layer's event consumption already runs off DAL events that could come from any analyser source).
---
## 6. Effects catalogue
The **effect** is the unit of artistic intent that orchestration produces and that drivers translate to hardware. Each effect describes *what* the lights should do, abstracted from *how* a particular device implements it. A single effect renders differently on a PixMob bracelet (RGB+envelope IR command), a Tildagon badge (animation on twelve addressable perimeter LEDs plus optional screen overlay), and a future RGB-strip device (per-pixel state across many LEDs).
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
<td>Drop (bass-energy ratio crossing; Director-internal only as of v0.29)</td>
<td>No on-the-wire effect. DropDetector still flags drops on `AudioFrameEvent::music_event`, but Epic 4.5's `MUSIC_EVENT` (0x06) broadcast and the Strobe Burst rendering it drove were both removed in the v0.29 protocol trim. A future Epic that finds a real consumer would need to define a fresh wire code point and a corresponding effect.</td>
</tr>
<tr>
<td>Breakdown (sustained low-energy section; Director-internal only as of v0.29)</td>
<td>No on-the-wire effect. Same producer/consumer status as Drop.</td>
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
<td>Full colour, MicroPython framebuffer, SDK font and icon helpers. Plus twelve perimeter RGB LEDs as a separate "ring" surface.</td>
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
- **Show element**: the screen becomes part of the visual output. Pulses on the lighting output can be accompanied by full-screen colour washes that match. Tildagon badges in the audience can show animations beyond what their twelve LEDs support: pulsing concentric circles, scrolling text ("NULLSECTOR", song titles), iconography (band logos, EMF logo, lunar phase glyphs).
- **Diagnostic**: live FFT spectrum, IBI history, ESP-NOW frame counters, last-received-from indicator. Useful for development and tuning, can be toggled off for performance.
- **Idle / ambient**: when no music is playing or the device is paused, the screen shows something pleasant - a subtle hue cycle, a clock, the Nocturnation logo. Avoids the "dead device" appearance.
### 7.3 Tildagon-specific considerations
The Tildagon badge is unusual because it's both a *receiver* (in the audience, animating with the show) and a *personal device* (its owner's badge for the festival, with their own apps and identity). The display treatment must respect that:
- The Nocturnation receiver app runs as one of many apps on the badge. It only "owns" the display while it's the foreground app.
- When in the foreground, the show animation runs full-screen on the round LCD plus the twelve perimeter LEDs.
- When backgrounded, the perimeter LEDs continue to react (ESP-NOW listening continues), but the screen shows whatever foreground app the user has chosen.
- An opt-in "intense mode" gives the show full-screen treatment even when in another app, for users who want the full effect during a known show window.
### 7.4 Display-as-light + render_fx() canonical entry point
A receiver's display is **also a light surface in the show**. When a Stick is in Lume mode, an inbound `LIGHT_COMMAND` (or any locally-fired effect) paints the screen full-bleed with the broadcast colour and the matching attack/sustain/release fade - the Stick on a tripod becomes a coherent piece of installation gear, not just a transmitter that drives lights but one of the lights itself. For the constellation art piece a single Stick can act as both transmitter AND visible light point, reducing the bracelet count needed.
This is implemented via the DAL's canonical render entry point: `DAL::render_fx(target, event)`. Orchestration on a beat (Director mode, Test mode, etc.) issues **multiple ****`render_fx`**** calls** - one per locally-available light surface - and each fails silent if its driver/transport isn't enabled or wired:
```c++
DAL::render_fx("local",         ev);   // host's primary light surface
                                       // (StickC: screen; future LED-only
                                       //  device: LED; Tildagon: screen +
                                       //  on-board LEDs)
DAL::render_fx("all-pixmobs",   ev);   // IR transport to PixMob bracelets
                                       // in range; gated by Config > IR
                                       // > Enable
DAL::render_fx("esp-now-broadcast", ev);  // ESP-NOW broadcast for Lumes
                                       // (rolling out in Block 3+ as a
                                       //  proper EspNowDriver)
```
There is no auto-forwarding inside `render_fx()` itself: each call has one job, which keeps the IR mute toggle clean (`DAL::set_driver_enabled("ir-pixmob", false)` makes IR `render_fx` fail silently without affecting the screen) and respects per-host capability differences.
**Per-capability gates beyond driver enable.** The `LocalDriver` exposes a per-capability gate for `RgbPulseEvent` (Config → Display → Pulse Enable, NVS-backed) so an operator can keep the screen showing status text/UI but suppress beat flashes. Other DisplayShowText / DisplayClear events stay unaffected. This is finer-grained than the driver-level enable; future per-capability gates on other drivers will follow the same pattern.
**Future effect types** (text overlay, simple graphics, scripted animations per §6 future work) ship as additional `render_fx` overloads on new event structs. They will not introduce per-capability `fire_*` helpers - `render_fx` is the single entry point that orchestration learns once and reuses for every new effect.
**Target naming direction.** The currently registered device names (`"all-pixmobs"`, `"group-1"`..`"group-5"`, etc.) are brand-tied for historical reasons. The agreed naming pattern going forward is `transport_protocol_groupfilter`: `IR_pixmob_group3`, `ESPNow_nocturnation`, `Local_Screen`, `Local_LED`, `DMX_universe1`. Bare `"local"` stays as the host-as-target convention. The rename will land in a focused refactor pass when a second IR protocol or NocturNation-native bracelet code arrives - the abstraction earns its keep with a real second consumer rather than speculatively.
### 7.5 Display-event abstraction (proposed)
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
### 7.6 Plug-in surfaces (Epic 4.6 / 4.7)
Epics 4.6 / 4.7 made the rendering pipeline pluggable on both sides of the wire. The host code never bakes in *which* show runs on the Director or *which* render destinations a Lume drives - those are plug-ins discovered from registries at boot. Epic 4.7 split the Director-side surface into a Show plug-in (the performance) and a small widget library (level-tuning helpers a show or sub-mode composes); Epic 4.6's original `Visualisation` plug-in had conflated the two and is retired.

For step-by-step developer guidance on adding a new Show — Show base class API, registration, analyser hooks, widget composition, persistence, testing — see [docs/developing-shows.md](https://github.com/ratcliffej/nocturnation-m5/blob/main/docs/developing-shows.md).

**Director-side: `Show` plug-ins (Epic 4.7).** A Show is the Director-side performance unit. It consumes analyser events (kick / snare / hi-hat onsets, continuous music descriptors, section transitions), decides what to send on the wire via `DAL::render_fx("<class>:<group>", ev)`, owns the Director LCD via `on_render(ctx)`, and handles operator input beyond the reserved back-gesture. The contract lives in [`include/shows/show.h`](https://github.com/ratcliffej/nocturnation-m5/blob/main/include/shows/show.h): every hook has a no-op default so a Show overrides only what it cares about. At runtime each Show receives a `ShowContext` exposing `render_fx`, property-bag accessors, capability query, paused state, and time helpers - the same shape as the retired `VisualisationContext` with a clean rename.

Hook surface:

- Lifecycle: `enter / exit`
- Raw frames: `on_audio_frame / on_spectrum_frame`
- Analyser events: `on_beat_detected / on_snare_detected / on_hihat_detected / on_music_descriptor / on_section_change` (Epic 4.7 Block 3-4 surfaces)
- Input: `on_input_action`
- Render: `on_render`
- Property change: `on_property_changed`
- Tick: `tick`

Two Shows ship today: `SimpleBeatShow` (preserves the pre-Epic-4.7 BeatPulse fan-out byte-for-byte: render_fx "00:00" + screen flash + IR via `effects::Pulse`; one `color` property cycled by `Cycle` action) and `DynamicShow` (the Epic 4.7 Block 5 headline: kick → group 1, snare → group 2, hi-hat → group 3 via `"01:01"` / `"01:02"` / `"01:03"`; HSV colour math driven by centroid + energy + section, per-section palette adjustments — VERSE cools, BUILDUP warms, BREAKDOWN dims, DROP overrides to white; bracelet `chance` mapped from density). The active Show is chosen via the picker overlay or via `ConfigMode > Show`, persisted to NVS under `noct/active_show`, and respects capability gating.

Shows own the Director LCD: there's no Mode-level chrome anymore. `DirectorMode` is a thin host - lifecycle (audio input + ESP-NOW broadcast), pause flag, music_event broadcast (transport concern), picker / settings overlays. The retired `Visualisation::wants_full_screen()` virtual is gone; Shows always own the screen during normal operation, and the host's picker/settings overlays take over the LCD entirely when open.

**Widget library** (Epic 4.7 Block 2). [`include/widgets/`](https://github.com/ratcliffej/nocturnation-m5/tree/main/include/widgets) ships two reusable level-tuning helpers a Show or sub-mode composes inside its `on_render()`:

- `BeatBarWidget` — horizontal level bar with optional threshold marker. `update(bar_fraction, marker_fraction)` clamps to [0, 1]; `draw(x, y, w, h)` paints frame + fill + marker. `SimpleBeatShow` composes it for the flux meter; `ConfigMode > Utilities > Level Tuning` hosts it standalone.
- `SpectrumBarsWidget` — 7-band perceptual spectrum (Sub Bass / Bass / Lows / Mid / Hi / Pres / Air) with permanent warm-to-cool colour coding. `update(values_7band)` takes pre-aggregated fractions; a static `roll_up_spectrum_to_perceptual(magnitudes_32, sensitivity, out)` helper converts raw 32-band log-spectrum data. `DynamicShow` composes it in its on-screen status region; `ConfigMode > Utilities > Level Tuning` hosts it standalone.

Widgets are a library, not a plug-in surface — Shows instantiate them directly, not via a registry. Promoting widgets to a plug-in registry is a deliberate future Epic if third-party widget contributions emerge.

**ConfigMode > Utilities > Level Tuning** (Epic 4.7 Block 2) hosts both widgets in a live-audio bench-work sub-mode. Audio capture starts when the operator drills into the sub and stops on B-hold back. Btn2 cycles a display mode: **Live** (widgets render incoming flux + 7-band perceptual roll-up from `AudioFrameEvent`) and four fixed overrides (25 / 50 / 75 / 100 %). Btn1 fires a `render_fx("00:00")` test pulse at the displayed level for IR + ESP-NOW path verification independent of audio. The live mode is for venue sound-check ("is my mic level high enough that beats cross the threshold marker?"); the fixed modes are for bench rigs verifying transport without a sound source.

**Lume-side: `OutputBinding` plug-ins** (Epic 4.6). An output binding consumes inbound `LIGHT_COMMAND` events and turns them into hardware action. Contract: [`include/output_bindings/output_binding.h`](https://github.com/ratcliffej/nocturnation-m5/blob/main/include/output_bindings/output_binding.h). Same Plugin base as Show, same property-bag and capability machinery. Bindings have no `render_fx` accessor (they *are* the render destination); hook surface is `on_light_command`. `LumeMode` fans inbound LIGHT_COMMAND out to every registered binding, filtering on `(binding.class(), binding.configured_group())` against the frame's target_class + target_group (Epic 4.65).

Two bindings ship today: `LocalDisplayBinding` (paints the Lume's LCD full-bleed with the broadcast colour and ASR envelope — the "display-as-light" behaviour from Epic 4) and `PixMobIrBinding` (IR + PixMob protocol relay; the legacy per-binding `group` property was retired post-Epic-4.65 when the binding became a pure relay that passes the inbound target_group straight through to the PixMob protocol's group byte). Both can run simultaneously; either can be disabled in Config to limit the Lume to one output.

Future hosts add their own bindings without touching Lume code: Tildagon (Epic 5) will ship a `TildagonLedRingBinding` for its twelve perimeter RGB LEDs (indexed 1..12 per the Tildagon hardware API); a DMX deployment (Epic 7) lands a `DmxOutputBinding`; a BLE-controlled bracelet line lands a `BleBinding`. None of those require a recompile of `LumeMode`.

**Device-class taxonomy (Epic 4.65).** Each `OutputBinding` declares a `DeviceClass` via `OutputBinding::device_class()` so a Show can address render targets by *what they are* rather than by transport-specific name strings. The taxonomy is operator-facing (lighting designers think in classes, not capability sets):

- `0x00` **All** - reserved as the addressing wildcard; never returned by a binding.
- `0x01` **Light** - light-only devices (PixMob X4 bracelets via `PixMobIrBinding`; future NocturNation-native light wristbands).
- `0x02` **Screen** - screen-only devices (`LocalDisplayBinding` on a StickC Lume).
- `0x03` **MultiLedScreen** - multi-LED + screen devices (Tildagon-class; `TildagonLedRingBinding` returns this in Epic 5).
- `0x04+` - reserved for future classes (accelerometer-stick, smoke-machine, etc.).

`render_fx` targets become `"<class>:<group>"` in hex: `"00:00"` = everyone-everywhere, `"01:00"` = every light-class device any group, `"01:07"` = light-class group 7, `"03:01"` = Tildagon-class group 1. The Director encodes both bytes into `LIGHT_COMMAND.target_class` and `target_group` (§4.3); Lumes filter inbound frames against `(binding.device_class(), binding.configured_group())` per active binding and only fan out where both axes match. Relay bindings (`PixMobIrBinding`) bypass the Lume-side group filter on inbound LIGHT_COMMAND - the bracelet protocol does its own group filtering at the IR layer.

A StickC Lume with both `LocalDisplayBinding` (class `Screen`) and `PixMobIrBinding` (class `Light`) active is conceptually two classed devices on one host: a Director firing `"01:00"` hits the IR binding only; firing `"02:00"` hits the display only; firing `"00:00"` hits both. The class lives on the binding, not the chassis.

**Shared infrastructure.** A `Plugin` base class provides id, display name, capability requirements, NVS-backed `PropertyBag`, and `PowerProfile` declaration. The NVS namespace prefix depends on plug-in kind: `nv_<id>` for the retired Visualisation surface (TUs still compile but inert), `nb_<id>` for OutputBinding, `ns_<id>` for Show (Epic 4.7). Plug-in id is capped at 12 chars so the namespace stays under NVS's 15-char limit. Templated `Registry<T>` machinery with explicit `register_plugin()` calls in `main.cpp` (no static-init magic); a separate registry per plug-in kind.
---
## 8. Node operating modes and UI
Every Nocturnation node runs the same conceptual state machine, regardless of hardware platform. The UI surface differs (StickC Plus2 has 3 buttons + screen; Tildagon has 6 buttons + round screen; Atom Lite has 1 button + 1 LED) but the *modes* and the *transitions between them* are common.
### 8.1 Boot flow
On power-on, every node displays a boot screen and starts a **5-second countdown to default mode**. During the countdown, any button press interrupts and presents the mode-selection menu. If no input is received, the node enters **Lume Mode** automatically.
Lume Mode is the universal default because it's the safer assumption: a node that defaults to Lume does nothing intrusive on its own; it just listens for a Director and acts when one is heard. This makes multi-device deployments trivial (plug them all in, exactly one is promoted to Director via menu) and avoids the failure mode where two co-located devices both think they're the Director and broadcast competing event streams.
The single-device autonomous use case (StickC Plus2 at the Coldplay tribute act, no network) is reached by interrupting the countdown and selecting Autonomous Director - one extra interaction at startup in exchange for the simpler default. The boot menu remembers the last-used mode for that device, so a regularly-used Director node just needs Btn-A pressed once during the boot countdown to confirm.
### 8.2 Runtime modes
<table header-row="true">
<tr>
<td>Mode</td>
<td>Behaviour</td>
<td>Required hardware</td>
</tr>
<tr>
<td>**Autonomous Director**</td>
<td>Run local audio analysis, fire local outputs (IR/LED/screen), broadcast beat events on ESP-NOW for any listeners. Continues running indefinitely whether or not audio is present - silence is treated as a valid state, not a failure. Optional **Audio-silence failover** config (default OFF; see §8.4) demotes the node to Lume Mode after a configurable silence period, intended for unattended deployments only.</td>
<td>Mic + at least one output. **Hosts without a microphone (e.g. Tildagon) cannot enter Director Mode** and the mode-selection menu should not present it as an option on those platforms.</td>
</tr>
<tr>
<td>**Lume**</td>
<td>Listen on configured ESP-NOW channel for events; fire local outputs in response. Optionally rebroadcasts received frames if the **Repeat** config option is enabled (off by default; see §8.4). Falls back to a local idle effect (subtle hue cycle, starlight, etc.) after N seconds without Director heartbeat.</td>
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
Note: "Relay" is no longer a top-level mode. Repeating received frames is a sub-behaviour of Lume Mode, controlled by the **Repeat** config option (§8.4). This means a single device can simultaneously fire local outputs *and* rebroadcast for further reach, without the user having to choose between the two.
### 8.3 Mode-selection menu
Reached by interrupting the boot countdown, or via the in-app menu. Top-level options:
- **Start Lume Mode** *(default after countdown if no interrupt)*
	- Channel ID (default channel, with countdown to auto-select; override with key combination to choose non-default channel)
	- Group ID (which bracelet group to drive on local IR output, if applicable)
- **Start Director Mode**
	- Channel ID (canonical channel selected by default and protected by key combination; non-canonical channels visibly labelled)
	- Start
- **Test Mode** (see §8.5) - top-level, deliberately easy to reach
- **Config** (see §8.4)
### 8.4 Config tree
Post-Epic-4.65 / 4.7 layout: Group / Show / Display / Connectivity / Utilities / System at the top level (six items, scrolling enabled). Connectivity and Utilities are intermediate pickers; everything else drills straight to a leaf submenu. Options not relevant to the current hardware are hidden.
- **Group** *(direct action)* — increments `slv_group` 0..255; the device-wide NocturNation receive-filter group ID applied by LumeMode upstream of binding fan-out.
- **Show** *(leaf)* — picker over registered `Show` plug-ins; selecting one persists as `active_show` in NVS. Reaches the same registry as the Director-mode picker without entering Director mode.
- **Display**
	- Pulse Enable / Disable (toggles `LocalDriver`'s RgbPulse handler)
- **Connectivity** *(picker)*
	- **IR**
		- Enable / Disable
		- Protocol (PixMob Aurora; NocturNation-native reserved for when bespoke devices are defined)
	- **ESP-NOW**
		- Director Channel (1 / 6 / 11)
		- Lume Channel (Auto / 1 / 6 / 11)
		- Lume Repeat (rebroadcast inbound frames with hop_count + 1)
	- **WiFi** *(stub; Epic 4)*
	- **DMX** *(stub; Epic 7)*
- **Utilities** *(picker)*
	- **PixMob** — set bracelet group ID, send a group-targeted test fire (one-time bracelet setup workflow).
	- **Level Tuning** *(Epic 4.7 Block 2)* — `BeatBarWidget` + `SpectrumBarsWidget` standalone with live audio. Btn2 cycles `{Live, 25%, 50%, 75%, 100%}`; Btn1 fires a `render_fx("00:00")` test pulse at the displayed intensity. Live mode drives the widgets from `AudioFrameEvent` for sound-check / level-tuning at the venue; fixed-percent modes provide IR + ESP-NOW path verification independent of audio.
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
- **Group Targeting Test** *(IR-equipped Directors only)* - fire each group ID 1-5 in turn, with on-screen indication of which group should respond. For verifying group assignment after EEPROM setup.
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
<td>1.14" TFT, ~5 lines of text at size 2</td>
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
<td>Limited UI; uses LED colour to indicate mode (red=Director, blue=Lume, white=Test). Long-press to cycle modes; single-press for default action within mode. Config done via serial console or one-shot "setup mode" via button held during boot.</td>
</tr>
</table>
### 8.7 InputAction abstraction (Epic 4.6 Block 4)
The UI surface differs across hosts (StickC Plus2 + S3 = 2 buttons in practice once the power button is excluded; Tildagon = 6 buttons; Atom Lite = 1 button), so Epic 4.6 introduced a semantic input layer that Shows and the framework UI consume regardless of host. Physical button-to-action mapping is the HAL's concern; everything above the HAL deals in `InputAction` events.

The canonical action set lives in [`include/hal/input_action.h`](https://github.com/ratcliffej/nocturnation-m5/blob/main/include/hal/input_action.h):

```c++
enum class InputAction : uint8_t {
    None,
    Confirm,    // primary "accept / fire" gesture
    Cycle,      // step forward through a choice list
    CyclePrev,  // step backward (unbound on 2-button hosts)
    Pause,      // toggle pause
    Settings,   // open per-vis settings overlay
    Picker,     // open Show-picker overlay
    AuxA,       // reserved for richer hosts
    AuxB,       // reserved for richer hosts
};
```

For 2-button hosts (Plus2 + S3), the mapping lives in [`src/hal/input_action_mapper_2btn.cpp`](https://github.com/ratcliffej/nocturnation-m5/blob/main/src/hal/input_action_mapper_2btn.cpp):

| Physical event | Action |
|---|---|
| Btn1 short click | `Confirm` |
| Btn1 long press | `Settings` |
| Btn1 double-click | `Pause` |
| Btn2 short click | `Cycle` |
| Btn2 long press | `Picker` |

For 6-button hosts (Tildagon, Epic 5) a sibling mapper will land that emits the same canonical action set from a different physical layout (`CyclePrev` / `AuxA` / `AuxB` become reachable). Show code never sees the physical event — it sees only `InputAction` — so a Show written against the StickC layout runs unchanged on the Tildagon.

Both `ButtonPressEvent` and `InputEvent` fire from the same physical event during the migration window, so legacy modes that subscribe directly to button presses (Config, Menu, Test) continue to work alongside vis code that subscribes only to actions.

### 8.8 Modal overlays in Autonomous Director Mode (Epic 4.6 Blocks 10-11)
`DirectorMode` runs the active Show full-screen by default but exposes two operator overlays without leaving the mode:

- **Picker overlay** (opened by `InputAction::Picker`): lists every registered `Show`, marks the active one, greys out any Show whose `required_capabilities` are not present on this host. `InputAction::Cycle` steps through the list; `InputAction::Confirm` selects and dismisses; a second `Picker` press dismisses without changing selection. The chosen Show id persists to NVS (`noct/active_show`) so the same Show returns on next boot. Block 1 of Epic 4.7 added a `ConfigMode > Show` top-level entry that reaches the same registry without entering Director mode.
- **Settings overlay** (opened by `InputAction::Settings`): renders an auto-generated settings UI from the active Show's `properties()` schema (PropertyType=Bool toggles, Colour cycles through the named palette, Enum cycles through `enum_names`, U8/U16 step on Cycle). A Show can override `render_settings_ui()` if the auto-generated form isn't sufficient. A second `Settings` press dismisses.

Both overlays are toggles owned by the mode, not the Show - the Show only learns about gestures it actually receives (`InputAction::Confirm` / `Cycle` / `CyclePrev` while no overlay is open). This keeps each Show's gesture handling minimal while letting the framework UI evolve independently. The fallback "exit to Menu" gesture (Btn2 long historically; now the Picker gesture) is dropped from the Director: the operator returns to the mode menu via `Btn2 long` on the Picker overlay's "back" entry, keeping overlay open/close consistent.

### 8.9 Open design questions
- **Boot countdown duration**: 5s fixed, configurable, or platform-dependent? (Block 13 reduced to 3 s; revisit if user feedback flags it as too short.)
- **How does a Lume node indicate "I'm not receiving"?** Idle effect after timeout is the default; should there also be a visual cue (flashing red dot on screen, etc.)?
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
Single Director driving 5 bracelets via group-targeted IR. Mounted in lanterns or fixed enclosures. Optional QLC+ control from laptop for choreographed performance.
### 9.3 Distributed (NullSector / EMF)
```plain text
                       ┌──────────────┐
                       │  Mac (QLC+)  │
                       └──────────────┘
                              │
                       USB-serial DMX
                              ↓
┌──────────────────────────────────────────────────────┐
│  Director node (StickC Plus2 near speakers)            │
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
Director at fixed location handles audio analysis. Distributed transmitter nodes provide IR coverage to different zones. Tildagon badges in the audience are additional ESP-NOW receivers that light up their own onboard LEDs as part of the show.
---
## 10. Use case roadmap
### 10.1 Short-term (working / immediate)
1. **Local beat analysis and IR control for one group via StickC Plus2.** Status: ✅ working. Standalone, single-bracelet, no networking. The reference deployment for the Coldplay tribute act.
2. **Group ID setting for PixMob bracelets.** Status: ✅ verified - the 2-command `SetGroupId` + `SetGroupSel` workflow lands the new group ID on the bracelet (X4 Gen3.1, hardware-tested 2026-05-09). Implemented in `pixmob_protocol.h` and surfaced through the `AssignDeviceGroup` DAL capability.
3. **Tildagon app with ESP-NOW Lume mode, StickC Plus2 Director transmitting.** Status: ⏳ next-up. Brings Tildagon receiver development forward as the immediate post-tribute priority, ahead of the constellation. Covers the v1 ESP-NOW frame format end-to-end on real hardware and gives EMF app submission lead time.
### 10.2 Medium-term (EMF 2026)
- **Constellation art piece v1** - 5 bracelets, paper lantern enclosures, bird-feeder mounting, single Director. Blocked on item 2 of the short-term roadmap; design contingency required if group setting doesn't work.
- Multi-node ESP-NOW mesh with deduplication and sequence tracking.
- QLC+ integration via serial DMX for pre-programmed shows.
- Distributed transmitter nodes covering NullSector.
- Documentation and open-source release.
### 10.3 Longer-term
- Multi-Director support (multiple operators contributing to one show)
- Sub-GHz RF for outdoor deployments
- Custom ESP32-C3 SuperMini bracelet design (when bracelet supply runs out)
- Shieldagon receiver hexpansion as fixed installation lights
- Art-Net integration (in addition to serial DMX)
- Integration with professional lighting consoles via DMX/Art-Net. The likely canonical professional deployment path is **QLC+ as upstream show controller** (free, open-source, runs on Mac/Windows/Linux) driving NocturNation as a DMX-receiving fixture. This aligns with how the lighting industry actually works: NocturNation becomes a DMX-driven endpoint, not a complete vertical stack. Forward direction worth flagging: an offline tool that takes a song WAV and Essentia analysis output, then emits a `.qxw` show file with beats/drops/sections pre-populated and the audio embedded as the show's audio track. The tool would be useful to anyone using QLC+ for music-synchronised lighting, not just NocturNation users - properly aligned with the project's open-source ethos. Submitting a NocturNation `.qxf` fixture definition to QLC+'s standard fixture library would put the brand in front of lighting designers who'd never otherwise encounter it. None of this is committed; it's the direction the architecture remains compatible with. The walk-before-run priority is beat detection (Epic 4.5) → Tildagon (Epic 5) → UI cleanup.
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
- **Show file format**: QLC+ files for laptop-driven, or define a portable JSON format that's runnable from embedded? See §10.3 forward direction on QLC+ as canonical professional path.
- **Synchronisation strategy at scale**: event-based ("BEAT NOW") vs clock-based ("phase X of bar at time T") vs hybrid.
- **Power and weatherproofing** for outdoor lantern deployment.
- **Failure modes**: what does each node do when ESP-NOW drops? When Director goes silent? When mic noise floor changes?
---
## 12. References
References use Harvard style. URLs verified at the time of writing (May 2026); links may move.
### 12.1 Reverse-engineering work this project builds on
Weidman, D. (2022) *Hacking the PixMob infrared protocol to enable control of PixMob wristbands at home* [Online repository]. GitHub. Available at: [https://github.com/danielweidman/pixmob-ir-reverse-engineering](https://github.com/danielweidman/pixmob-ir-reverse-engineering) (Accessed: 5 May 2026).
W., J. (2024) *PixMob_IR: PixMob IR Reverse Engineering* [Online repository]. GitHub. Available at: [https://github.com/jamesw343/PixMob_IR](https://github.com/jamesw343/PixMob_IR) (Accessed: 5 May 2026). The companion documentation files `docs/ir_protocol.md` and `docs/operation.md` in the same repository are the authoritative source for the byte-level protocol structure used in Nocturnation's `pixmob_protocol.h` C++ port.
### 12.2 Lighting control standards
Artistic Licence Engineering Ltd (2023) *Specification for the Art-Net 4 Ethernet Communication Protocol*. Available at: [https://art-net.org.uk/downloads/art-net.pdf](https://art-net.org.uk/downloads/art-net.pdf) (Accessed: 5 May 2026). Royalty-free specification covering the ArtDmx, ArtPoll, and ArtPollReply packet formats used in Nocturnation's console-input driver.
Entertainment Services and Technology Association (2008) *ANSI E1.11-2008 (R2018): Entertainment Technology - USITT DMX512-A - Asynchronous Serial Digital Data Transmission Standard for Controlling Lighting Equipment and Accessories*. ESTA Technical Standards Program. Available at: [https://tsp.esta.org/tsp/documents/docs/ANSI-ESTA_E1-11_2008R2018.pdf](https://tsp.esta.org/tsp/documents/docs/ANSI-ESTA_E1-11_2008R2018.pdf) (Accessed: 5 May 2026). The current edition (ANSI E1.11-2024) is paywalled; the 2008/R2018 PDF is freely available from ESTA's Technical Standards Program and is technically equivalent for our purposes.
Espressif Systems (n.d.) *ESP-NOW Wireless Communication Protocol* [Product page]. Available at: [https://www.espressif.com/en/solutions/low-power-solutions/esp-now](https://www.espressif.com/en/solutions/low-power-solutions/esp-now) (Accessed: 5 May 2026). High-level description of the connectionless Wi-Fi protocol used as Nocturnation's embedded mesh transport, including supported chip families (ESP8266, ESP32, ESP32-S, ESP32-C) and indicative range figures (200m+ open-air at +21dBm).
Espressif Systems (2024) *ESP-NOW - ESP-IDF Programming Guide* [Online]. Available at: [https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html) (Accessed: 5 May 2026). Authoritative API reference covering the vendor-specific action frame format, v1.0 (250-byte) and v2.0 (1470-byte) maximum payloads, optional CCMP/AES-128 encryption, and the C API (`esp_now_init`, `esp_now_send`, `esp_now_register_recv_cb`) used in Nocturnation's Director and repeater implementations.
Espressif Systems (2024) *esp-now: A connectionless Wi-Fi communication protocol - User Guide* [Online repository]. GitHub. Available at: [https://github.com/espressif/esp-now/blob/Director/User_Guide.md](https://github.com/espressif/esp-now/blob/Director/User_Guide.md) (Accessed: 5 May 2026). Application-level guide covering pairing, OTA, and security features layered on the base protocol; relevant context for Nocturnation's deduplication and repeater logic.
### 12.3 Hardware platform documentation
M5Stack Technology Co., Ltd (2024) *M5StickC PLUS2 (SKU: K016-P2) - Product Documentation*. Available at: [https://docs.m5stack.com/en/core/M5StickC%20PLUS2](https://docs.m5stack.com/en/core/M5StickC%20PLUS2) (Accessed: 5 May 2026). Authoritative source for ESP32-PICO-V3-02 specifications, ST7789V2 display driver, GPIO pinout (including the IR LED on GPIO 19 and HOLD pin on GPIO 4), and the PlatformIO configuration used by Nocturnation's StickC Plus2 firmware.
M5Stack Technology Co., Ltd (2024) *M5StickC PLUS2 datasheet (K016-P2)* [PDF]. Mouser Electronics. Available at: [https://www.mouser.com/datasheet/2/1117/M5Stack_Technology_01102024_K016_P2-3387216.pdf](https://www.mouser.com/datasheet/2/1117/M5Stack_Technology_01102024_K016_P2-3387216.pdf) (Accessed: 5 May 2026). PDF datasheet covering electrical specifications, dimensions, and connector pinouts.
Electromagnetic Field Ltd (2024) *Tildagon Badge Documentation* [Online]. Available at: [https://tildagon.badge.emfcamp.org/](https://tildagon.badge.emfcamp.org/) (Accessed: 5 May 2026). Includes the badge hardware overview, hexpansion creation guide, end-user manual, and gallery of community hexpansions.
Electromagnetic Field Ltd (2024) *badge-2024-hardware: EMF 2024 Tildagon badge hardware design files* [Online repository]. GitHub. Available at: [https://github.com/emfcamp/badge-2024-hardware](https://github.com/emfcamp/badge-2024-hardware) (Accessed: 5 May 2026). Contains the schematics, PCB templates, and hexpansion connector specifications referenced by Nocturnation's Tildagon receiver app design.
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
<td>~2 hours</td>
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
<td>~6-8 hours under Nocturnation receive workload (estimate)</td>
<td>ESP-NOW listening + screen + LEDs noticeably faster than idle drain. Calm mode and aggressive sleep when no broadcasts heard for >30s recommended.</td>
</tr>
<tr>
<td>**M5Stack Atom Lite**</td>
<td>None (USB-powered)</td>
<td>Indefinite (mains)</td>
<td>No internal battery. Suitable only for fixed installations or wired deployments.</td>
</tr>
</table>
For longer-running deployments, USB power banks (10,000 mAh, ~£15) extend StickC Plus2 and Atom Lite runtime to a full festival day. The bracelets are the easiest part of the rig to power - the original batteries from the events at which they were handed out routinely outlast the rest of the system.
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
- **Tildagon screens** must not flash full-area between high-contrast colours (e.g., white ↔ black) at >2 Hz. Animations should use partial-area effects (concentric rings, edge glows) or low-contrast transitions.
- **Brightness caps** in calm mode reduce maximum LED brightness to 50% of full and disable any contrast >2:1 between consecutive frames.
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
- **How is calm mode signalled to the Director?** Locally-set on each device, or Director can broadcast a "calm mode active" event that overrides per-receiver configuration?
- **Should there be a "panic stop"?** A single button combination on any operator device that broadcasts "stop all effects immediately" to every receiver in range. Useful if anyone in the audience shows distress.
- **Liability boundary.** This is a hobby project, not a certified medical or broadcast device. The above measures are best-effort risk reduction, not clinical assurance. Worth being explicit in any public-facing documentation that users assume their own risk.
---
## 16. Security model (overview)
Nocturnation broadcasts plain ESP-NOW frames by default. This is appropriate for the project's primary deployment context (hobbyist, community, low-stakes) but is inadequate for any context where the show *must not* be hijacked - touring bands, ticketed events, art installations where disruption would matter.
The full security architecture - including threat model, deployment tiers, time-bounded certificate design, key management, and compute-cost analysis - is captured in a separate exploratory document because it is **not yet ready for implementation**:
[Security architecture (RFC)](https://www.notion.so/358bd0677405817b8a60de0834511ce5) *(under the Nocturnation parent page)*
The key architectural commitments that affect other parts of this spec:
- **Open algorithms, secret keys.** Nocturnation uses standard, openly-published cryptographic primitives (AES-128 for symmetric, ECDSA P-256 for asymmetric). Security derives from key management, not algorithm secrecy. Consistent with Kerckhoffs's principle.
- **Tiered deployment.** Open / MAC-whitelist / PSK / signed-cert tiers, selectable per deployment via Director Mode config (§8.4). Default for new installations is Open (Tier 0).
- **Time anchoring via timecode field.** The 4-byte `master_timecode` in the ESP-NOW frame format (§4.3) doubles as a deduplication key and as a tamper-evidence mechanism for cert validity, eliminating the need for receivers to have an RTC.
- **Hobbyists are unaffected.** The cert system is fully optional; Tier 0/1 deployments have no crypto overhead, no key management, no expiry concerns. The sophistication exists for those who need it without imposing on those who don't.
- **Calm Mode is per-receiver and unidirectional.** Lumes locally choose to ignore strobe-flagged frames; the Director is not informed. Consistent with the no-back-channel design (§4.3).
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
- **Director**: a node responsible for analysis and event generation.
- **Lume / receiver**: a node that consumes events and drives lights.
- **Group ID**: in PixMob protocol, a 5-bit identifier (1-31) assigning bracelets to addressable groups.
---
## Document history
- **v0.1** (2026-05-05): initial draft from extended design conversation. Captures architecture, protocols, hardware platforms, and roadmap. Open questions section identifies unresolved items.
- **v0.2** (2026-05-05): named MurmurNet. The "Net" suffix signals "this is a networked system" rather than a single device, sits naturally alongside other protocol names (ArtNet, EtherNet/IP, ZigBee), and disambiguates "Murmur" from its less-flattering connotations (grumbling, heart defects). The "Murmur" half evokes the ambient collective sound of a crowd — the *mor-mor* Indo-European root that's onomatopoeic for low continuous sound.
- **v0.3** (2026-05-05): added Effects catalogue (section 6) and Display surfaces (section 7). Effects formalise the artistic primitives the orchestration layer composes; display surfaces add a peer concept to lighting output, recognising that several MurmurNet devices have screens that can be used both as operator UI and as part of the show. Subsequent sections renumbered.
- **v0.4** (2026-05-05): added Node operating modes and UI (section 8). Defines the boot flow, the six runtime modes (Autonomous Director, Lume, Relay, Config, Test, Idle), the menu hierarchy, the config tree (reorganised as carrier-then-protocol), the test sub-modes, and per-platform UI mappings. Captures several open design questions for further work. Subsequent sections renumbered.
- **v0.5** (2026-05-05): added References section (section 12, Harvard style). Cites the two PixMob reverse-engineering repositories the project builds on (Weidman, James W), the Art-Net 4 specification, the ANSI E1.11-2008 (R2018) DMX512-A standard, and the official hardware documentation for both M5StickC Plus2 and EMF Tildagon. Also corrected a factual error in section 3.2: the Tildagon uses an ESP32-C3 with onboard WiFi/BLE, not an RP2040 with ESP32 co-processor. Glossary renumbered to section 13.
- **v0.6** (2026-05-05): added ESP-NOW references to section 12.2 (Espressif product page, ESP-IDF API reference, and esp-now User Guide on GitHub). Added new Licensing section (section 13) confirming MIT for source code, CC BY-SA 4.0 for specification and documentation, and CERN-OHL-S v2 for any future hardware designs. Glossary renumbered to section 14.
- **v0.7** (2026-05-05): added Power and battery budgets (section 13), revised section 10.1 short-term roadmap to reflect three concrete priorities (working solo system, group ID verification gating constellation, Tildagon receiver), moved constellation art piece to medium-term with explicit dependency note, and added Safety considerations (section 15, frequency caps, Calm Mode default-on, pre-show warning requirements, references to WCAG 2.3.1 and Ofcom photosensitivity guidance). Subsequent sections renumbered.
- **v0.8** (2026-05-05): simplified safety frequency cap to a single 4 Hz hard limit (covering all common music BPMs up to 240 BPM), removed multi-tier soft/hard structure. Made explicit that 4 Hz is above WCAG 2.3.1's 3 Hz threshold and that this is a deliberate trade-off mitigated by mandatory venue warnings and Calm Mode (which drops the cap to 2 Hz). Tightened venue-warning language to call out that warnings are required, not optional, given the WCAG-threshold trade-off.
- **v0.9** (2026-05-05): revised section 8 boot/mode design. Default mode is now Lume for all hardware (not capability-dependent) - simpler mental model, supports multi-StickC topologies trivially, makes single-Director-many-Lumes the natural deployment shape. Relay removed as a top-level mode and folded into Lume Mode as a Repeat config option (default off) - allows a node to drive local outputs and rebroadcast simultaneously. Test Mode promoted to top-level menu item for discoverability. Director Mode now auto-fails over to Lume after 20 minutes of audio silence. Boot menu remembers last-used mode for one-press resumption.
- **v0.10** (2026-05-05): made Audio-silence failover an opt-in config (default OFF) rather than mandatory behaviour. Director Mode now treats silence as a valid state and runs indefinitely; the failover is reserved for unattended deployments only. This avoids the footgun of silently demoting the Director during legitimate breaks (between sets, quiet passages, band returning from interval). Removed two open questions resolved by previous revisions.
- **v0.11** (2026-05-06): renamed project to **Nocturnation** (from MurmurNet) and adopted new tagline "Open-source crowd lighting, conjured from cheap silicon". The name is a portmanteau of *nocturnal* and *murmuration*, encoding both the after-dark deployment context and the swarm-coordination metaphor. Murmurations of starlings actually happen at dusk as the birds return to roost, making the name etymologically tight. Updated vision (§1) to articulate the democratisation framing - replacing per-show vendor contracts with commodity hardware - and the merch-revenue economic model that makes the project commercially attractive to touring bands. Added new Security model (§16) covering threat model, three-tier deployment design (Open / Whitelist / PSK+Whitelist / signed-frames-future), PSK distribution mechanics, channel protection, and compromise recovery. Glossary renumbered to §17. All MurmurNet references throughout the document replaced with Nocturnation.
- **v0.12** (2026-05-06): two protocol/architecture refinements. (1) Replaced 16-bit `sequence_number` field in the ESP-NOW frame header with a 32-bit `master_timecode` field that serves dual purpose: deduplication key AND time anchor for cert validity. Receivers persist the highest timecode seen per source_id to NVM, eliminating the need for an internal RTC and providing tamper-evidence for replay attacks. Frame header grows from 12 bytes to 14 bytes. (2) Slimmed §16 Security model from full design to brief overview, with the full architecture moved to a separate Security architecture (RFC) page under the Nocturnation parent - acknowledging that the security design is exploratory and not ready for implementation. The architecture spec retains the architectural commitments (open algorithms, tiered deployment, time anchoring, hobbyist-unaffected, unidirectional Calm Mode) and links out for the detail.
- **v0.13** (2026-05-06): split sequence number from time anchoring, following Art-Net's precedent of using a 1-byte sequence field for deduplication and a separate packet type for timecode. Frame header drops from 14 bytes to 6 bytes - the smallest size in the project's history. Wall-clock time is now carried in a new `TIME_SYNC` message type (0x05) broadcast by Tier 3 Directors at heartbeat rate; Tier 0/1/2 receivers ignore it. The split saves bandwidth on every BEAT_DETECTED and LIGHT_COMMAND frame (the vast majority of traffic) while retaining the time-anchoring capability that Tier 3 cert validity needs. Sequence wrap window at 4 Hz is ~64 seconds, comfortably longer than any plausible ESP-NOW reordering window.
- **v0.14** (2026-05-06): added §13.1 Domains, recording the registration of [nocturnation.com](http://nocturnation.com) and [nocturnation.net](http://nocturnation.net) (both 6 May 2026, 1-year initial term) as project-canonical domains pending future use.
- **v0.18** (2026-05-08): rewrote §4.5 from "Group ID semantics" (now §4.6) into a substantive **"Two-channel architecture"** section covering: the channel 1 hobby / channel 11 show social contract; the deliberate avoidance of channels 6 and 2-13; the Lume-side dual-channel scan algorithm with show priority (show channel scanned first, then hobby, looped indefinitely until heartbeat detected); Director-side channel-and-role selection (hobby default, show explicit). Added a forward-looking subsection on **distance-based effects via RSSI** describing wave effects, intensity gradients, distance-zoned palettes, ambient-only-back-rows mode, and repeater self-discovery as Tier 0 software effects that become possible once Epic 4 lands RSSI capture - with honest calibration about RSSI's coarse-band rather than precise nature. Added a second forward-looking subsection on **per-device addressing for K-pop-style commercial deployments**, contrasting PixMob's dumb-bracelet model with K-pop's expensive-fan-owned-light-stick model where seat-linked addressing enables surgical effects (text in audience, precise gradients, personalised effects). The section commits to nothing beyond "don't paint into a corner that forecloses this direction" but flags it as the most ambitious commercial future the architecture should remain compatible with. §4.5 Group ID semantics renumbered to §4.6.
- **v0.19** (2026-05-08): added "Multi-show coexistence: why no universe field" subsection to §4.5. Captures the design decision to deliberately *not* add an Art-Net-style universe / scope field to the frame header in v1, and the reasoning (existing source_id and target_group fields can carry the discrimination load; operational discipline handles the multi-show-on-same-channel case adequately; premature wire-format additions are expensive; v2 via EXTENSION message type 0xFF is the migration path if the need turns out to be real). Recorded so future-you doesn't re-debate it.
- **v0.20** (2026-05-09): added "Forward direction: companion app and mic-less devices" subsection to §4.5. Pulls together three previously-separate forward directions (BLE carrier in §4.1, K-pop seat-mapping above, distance-based effects via RSSI above) into a single coherent product vision: a mic-less NocturNation device paired with a phone companion app that does audio analysis and sends events via BLE. Captures three commercial angles - cheaper hardware/better margins, home-use retention model (the strategically most important one), and seat-capture for surgical effects. Frames as forward direction only, not committed Epic. Walk-before-run priority captured: 4.5 → 4.6 → 4.7 → 5, then revisit. Also updated §10.3 with the QLC+ canonical-professional-path forward direction (recorded earlier in same session); the longer-term roadmap is now properly structured around what the architecture should remain compatible with rather than what's committed to build.
- **v0.21** (2026-05-10): two refinements. (1) §3.2 Tildagon entry updated to make explicit that the platform is **Lume-only** because it has no microphone. The constraint was previously implicit (derivable from the §8.2 capability requirements) but not stated; making it architectural means the Tildagon receiver app design (Epic 5) is unambiguous about scope and the platform is positioned cleanly as a prototype for the future mic-less companion-app device pattern. (2) §8.2 Autonomous Director row updated to make explicit that hosts without a microphone cannot enter Director Mode and the mode-selection menu should not present it as an option on those platforms - removes a potential UX bug in the Tildagon receiver app where Director Mode could be selected and would then fail silently.
- **v0.22** (2026-05-10): rewrote §5 Audio analysis pipeline to reflect Epic 4.5's capability-aware analyser surface. Headline changes: (1) §5.1 Pipeline stages restructured around the FFT-magnitudes-to-typed-events flow that the new pure-function analyser core in `src/dal/analyser/` produces, including the 8-band perceptual summary (Audible Genius reference) alongside the restandardised 3-band B/M/T roll-up (now <250 / 250-2000 / 2000-Nyquist Hz, evidence-based split-points from the same reference). (2) New §5.2 Capability surface listing the analyser sub-capability flags lit by Epic 4.5 (`AnalyserBeatDetection`, `AnalyserDropDetection`, `AnalyserSpectrumFrame`, `AnalyserBandSummary`) and the four reserved for Epic 4.7. (3) New §5.3 Operating points covering the host-declared `(sample_rate_hz, fft_size)` tuples, `configure_audio_pipeline()` API, Hz-first band layout (bin assignments computed at runtime from the operating point), and `set_band_layout("hifi+production")` default. (4) §5.4 Future extensions reorganised by Epic, adding **frequency-response calibration** as a Config Mode tool that drives a known sweep through a calibration speaker to produce per-host correction curves - three uses: diagnose mic faults, compensate for raw-response differences across hosts, and provide objective cross-device-consistency verification distinct from the subjective "feel" check Epic 4.5 currently relies on. Also adds **haptic / IMU-driven inputs** as an analyser-adjacent direction the architecture should remain compatible with: the audience-jumping kinetic energy and bracelet-wearer movement that audio doesn't capture. (5) §3.1 platform table: Plus2 and S3 entries now declare their analyser feature set + operating points list. (6) §4.3 MUSIC_EVENT row updated from "reserved by spec; not yet implemented" to "DROP and BREAKDOWN producers shipped in Epic 4.5; BUILD reserved for Epic 4.7". (7) §6.3 effects-mapped table: Drop / Breakdown rows updated to reference MUSIC_EVENT 0x06 wire bytes and clarify the Epic 4.5 producer / Epic 4.7 consumer split.



-

