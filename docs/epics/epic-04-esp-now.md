---
title: "Epic 4: ESP-NOW transport on Stick reference platforms (Plus2 + S3, Tier 0)"
status: In progress
notion_url: https://www.notion.so/358bd067740581e3afa8fd061b821638
notion_id: 358bd067740581e3afa8fd061b821638
notion_status: In progress
last_synced: 2026-05-08
sync_direction: bidirectional
---

## Related Documents

- [NocturNation Architecture Specification](https://www.notion.so/357bd0677405800b891beab0f4e0a976) - particularly §4.3 (Custom ESP-NOW frame format), §4.5 (Group ID semantics), §8.4 (Config tree)
- [Security Architecture (RFC)](https://www.notion.so/358bd0677405817b8a60de0834511ce5) - background on Tier 0 vs higher-tier deployments; this Epic is Tier 0 only
- [Brand and visual identity](https://www.notion.so/358bd0677405811b8eb7eaa3c80e2a06) - for any UI screens introduced for ESP-NOW status / signal-strength display

## Goal

Implement ESP-NOW transport on the Stick reference platform (StickC Plus2 *and* M5StickS3, sharing the HAL layer established in Epic 2): master broadcast, slave receive, deduplication, repeat-mode toggle, dual-channel scanning with show/hobby channel separation, and signal-strength reporting for operator awareness. After this Epic, two Sticks - of either model - configured as Master and Slave coordinate beat-locked IR firing across the room with no wires between them, and the operator can see whether the radio link is healthy via a familiar battery-style indicator.

## Operational model: laptop-driven, multi-target

Following the same operational pattern as Epics 1-3 (laptop-driven development with hardware in the loop, not autonomous AIOS dispatch), this Epic must additionally produce code that builds and runs cleanly on **both** the StickC Plus2 (legacy reference) and the M5StickS3 (current and future reference). The HAL layer from Epic 2 is the place where chip-specific differences live; the ESP-NOW transport above that layer should be common.

Verification ownership: **(L)** = laptop / native test, **(B)** = build-time check, **(H)** = hardware verification by Jason, with the additional convention **(H-Plus2)** / **(H-S3)** when a hardware test must specifically be performed on each device.

## Business Value

ESP-NOW is the protocol layer that elevates NocturNation from "one device, one bracelet" to "distributed crowd lighting system". This Epic delivers:

- Multi-Stick deployments at NullSector and similar spaces
- The protocol foundation for the Tildagon receiver (Epic 5) - the badge speaks the same wire format
- Repeater capability for venues larger than a single ESP-NOW radio range
- The audio-master pattern: one Stick analyses audio, others fire IR locally
- Cross-platform interop: Plus2 and S3 devices coexist transparently in the same deployment, since ESP-NOW is identical at the radio firmware level across all ESP32 variants
- Operator visibility into link health via signal-strength reporting (per the new Feature 4.8 below)

This Epic ships only Tier 0 (open/unencrypted) per the Security RFC. Tier 1+ (whitelist, PSK, signed certs) is explicitly future work - not in scope here. The Tier 0 ship is enough to unblock everything downstream.

## Channel architecture: two-channel social contract

**Channel 1 = hobby/community/open. Channel 11 = show/commercial.**

NocturNation operates on a deliberate two-channel split that encodes a social contract for the project's deployment ecosystem:

- **Channel 1 - Hobby/Community channel.** Open by definition. Tier 0 traffic (no encryption). Available for anyone to transmit on. Used by hackspace gigs, EMF deployments, makers playing about with the protocol, the constellation art piece, learning and experimentation. Channel 1 is permanently the project's open territory.
- **Channel 11 - Show/Commercial channel.** Reserved for production deployments. Tier 1+ encrypted traffic when those Epics ship; Tier 0 traffic during this Epic since encryption hasn't been built yet. Used by touring bands, ticketed events, art installations where disruption would matter. Channel 11 is permanently the project's professional territory.

**Why channel 11 specifically:**

- Microwave-oven leak band (≈2.4-2.45 GHz) clips channels 6-9 hard. Channel 11 sits above this leak band - critical at venues where snack-bar microwaves run during shows.
- Most consumer routers default to channel 6, making channel 11 consistently quieter on average.

**Why channel 1 specifically:**

- Lower frequency means slightly better wall/body penetration (humans are ≈60% water; lower frequencies are absorbed marginally less). This favours hackspace and EMF environments where bodies are between transmitter and receiver.
- Distinct from channel 11 by enough spacing that the two coexist cleanly without adjacent-channel interference.

**No traffic on channels 2-10 or 12-13.** Channels 2-10 cause adjacent-channel interference with 1, 6, 11, which is *worse* than co-channel congestion. Channels 12-13 are not legal in all regions and we maintain global compatibility by not using them. The scan utility may report on these channels for operator information, but cannot select them as defaults.

## Slave-side dual-channel scan and lock

**Behaviour for receivers (Slaves, Tildagon receivers, future bracelets):**

On boot, the slave performs a 2-second scan of both channels 1 and 11 looking for ESP-NOW heartbeat traffic. The scan-and-lock decision logic:

```
scan both channels for ~2 seconds at boot:
  if channel 11 heartbeat detected: lock to channel 11 (show takes priority)
  else if channel 1 heartbeat detected: lock to channel 1
  else: default to channel 1 and continue listening

while locked to a channel:
  listen normally on that channel only
  if heartbeat timeout (1 second silence per spec §4.3):
    return to dual-channel scan mode

while in dual-channel scan mode (no master heard):
  alternate listening between channels 1 and 11 every ~200ms
  any heartbeat seen → lock to that channel (channel 11 priority if both seen)
```

**Rationale**: ESP-NOW radios are single-tuner. They cannot listen on two channels simultaneously. Time-slicing between channels has overhead (5-15ms per channel switch, so 5-15% of airtime is lost during scanning) but redundant transmission (3-5 sends per frame) absorbs missed frames during switch windows. Once locked to a channel, the slave stays locked - no periodic check of the other channel - which means no missed show frames once the show is found.

**Implication for slave-only devices** (bracelets without UI, future custom hardware, anything where the user can't manually choose a channel): this auto-scan logic means *they don't need to*. Show traffic always wins over hobby traffic, automatically, with no user input. This is exactly what a wearable or zero-config receiver needs.

**Implication for Stick devices with UI**: the auto-scan is the default behaviour. Operators can override it via Config Mode (§8.4 of architecture spec) to lock the device to a specific channel - useful when a hobbyist deliberately wants to ignore concurrent show traffic and play with their own deployment. The override is sticky across reboots.

## Master-side channel selection

**Behaviour for transmitters (Masters):**

Masters do not auto-scan. They transmit on a single channel chosen at startup based on their role:

- **Hobby Master** (default for personal/community use, default mode of any new install): channel 1.
- **Show Master** (configured via Config Mode for commercial use): channel 11.
- **Custom channel** (advanced operators, research): any of {1, 6, 11} via key combination.

The Master's role is selected during Master Mode startup per architecture spec §8.3. The default is hobby/channel 1; switching to show/channel 11 requires explicit operator action - reflecting that show deployment is a more deliberate decision than hobby playing.

## Signal strength: operator awareness

**RSSI is presented to operators in a familiar battery-style indicator** alongside the actual battery icon, not as raw dBm in normal operation.

The Slave's status display shows two icons:

- **Battery icon** (existing): how much device power is left
- **Signal icon** (new): how strong the master's signal is

Mapping from raw RSSI to display bars:

| Bars | RSSI range | Meaning |
| --- | --- | --- |
| 4 (full) | -30 to -55 dBm | Excellent. Master is close, no risk of missed frames. |
| 3 | -55 to -70 dBm | Good. Normal operating distance. Reliable. |
| 2 | -70 to -82 dBm | Workable. Will likely deliver but vulnerable to fading. Consider repositioning or adding a repeater. |
| 1 | -82 to -90 dBm | Poor. Frames will start to be missed. Move closer to a master or repeater. |
| 0 (empty) | Below -90 dBm or no recent frame | No signal. Device will fall back to local idle effect after heartbeat timeout. |

A diagnostic mode (accessible from Test Mode per spec §8.5) shows the underlying numeric dBm value and per-source-id RSSI history, useful for radio debugging.

## Range expectations

With 3 hops and properly-placed repeaters at +21 dBm transmit power, NocturNation's mesh covers approximately:

- **Open-air, line-of-sight**: 200-250m (3 hops × 70-85m typical)
- **Festival field with crowd**: 150-200m (3 hops × 50-70m through bodies)
- **Indoor / through walls**: 60-150m (3 hops × 20-50m)

This covers a typical festival field with one Master and a handful of Slave-with-Repeat-enabled devices placed strategically. Multi-room enclosed venues (large warehouses, multi-stage festivals) may need additional repeaters or a redesigned topology. The signal-strength display above is exactly the diagnostic tool operators need to position repeaters intelligently.

## Plus2 vs S3: relevant differences

The HAL from Epic 2 hides most chip differences. For Epic 4 specifically, these differences exist but should not require code outside the HAL:

| Aspect | StickC Plus2 | M5StickS3 | Impact on Epic 4 |
| --- | --- | --- | --- |
| Chip | ESP32-PICO-V3-02 (Xtensa LX6) | ESP32-S3-PICO-1-N8R8 (Xtensa LX7) | Different binary; PlatformIO env handles via separate `[env:*]` blocks. Code is shared above the HAL. |
| WiFi/ESP-NOW radio firmware | Espressif standard | Espressif standard | **Identical at protocol level.** Frames are byte-for-byte interoperable. |
| Max TX power | +21 dBm | +21 dBm | Same on both. No code difference. |
| PSRAM | 2 MB | 8 MB | Both fine for ring buffers; S3 has more headroom for future expansion (e.g., longer dedup history). |
| USB | CH9102 chip | Native USB-OTG | S3 flashes faster and presents cleaner serial port; same code on both. |
| BLE | BLE 4.2 | BLE 5.0 | Not used in Epic 4 (ESP-NOW is on WiFi radio); not relevant. |

**Net result**: ESP-NOW code in Epic 4 is genuinely shared between Plus2 and S3. The HAL handles pin differences for IR/screen/buttons/audio (already done in Epic 2). The radio layer Just Works on both. The mixed-pair test (Plus2 Master + S3 Slave) verifies this empirically.

## Scope

**Included:**

- Frame format encoder and decoder per spec §4.3 (6-byte header, message types 0x00-0x05)
- ESP-NOW broadcast send (Master Mode)
- ESP-NOW receive callback (Slave Mode)
- Deduplication via `(source_id, sequence_number)` ring buffer of last 16 frames
- Repeat flag implementation per §8.4 (slave-also-rebroadcasts; default OFF)
- **Two-channel social contract**: channel 1 hobby, channel 11 show
- **Slave dual-channel scan with show priority** at boot and after heartbeat loss
- **Master single-channel transmit** with hobby (default) / show / custom mode selection
- Heartbeat at 4 Hz from Master; Slave detects master-loss within 1s and falls back to local idle effect
- Sequence number wrap handling at 64s window (per §4.3)
- Group ID propagation: LIGHT_COMMAND respects target_group field per §4.5
- **RSSI capture and battery-style signal display** on Slave status screen (per spec §8.6 status display section)
- **Diagnostic RSSI mode** in Test Mode showing numeric dBm and per-source history
- TX power configurable; default +21 dBm for Masters, lower for Slaves/Repeats
- Redundant transmission: each Master frame sent 3-5 times with 5-15ms jitter
- **Multi-target HAL discipline**: any chip-specific code goes in the HAL (per Epic 2). Main ESP-NOW code path is shared between Plus2 and S3.

**Explicitly excluded:**

- Any Tier 1+ security (MAC whitelist, PSK encryption, signed certs) - that's a future Epic. Channel 11 traffic remains Tier 0 in this Epic; encryption arrives in later Epics.
- TIME_SYNC message broadcast or processing - Tier 3 only, future Epic
- Channel 6 as a runtime option - architecture forbids it (microwave leakage, default-router congestion)
- Channels 12-13 - not legal in all regions; maintained for global compatibility
- Adaptive bitrate / dynamic power management beyond fixed configuration
- Audio-silence failover demoting Master to Slave (config option exists but defaults OFF; §8.2)
- Tildagon receiver implementation - that's Epic 5 (will use the same protocol with the same channel scan)
- DMX or Art-Net (those are Epics 7 and 8)
- Bracelet receiver hardware - future, post-EMF

## Acceptance Criteria

- [ ] **(B)** Code builds cleanly under both `[env:m5stick-plus2]` and `[env:m5stick-s3]` PlatformIO environments without HAL leakage into the ESP-NOW transport layer
- [ ] **(B)** Native unit tests for frame format encoder/decoder pass; round-trip serialise-then-deserialise for all message types
- [ ] **(L)** Frame format byte-level conformance verified with packet capture (Wireshark on a sniffing ESP32 in promiscuous mode)
- [ ] **(H-Plus2)** Two StickC Plus2 devices in the same room: one in Master Mode broadcasts beats on channel 1, one in Slave Mode receives and fires IR locally on every beat
- [ ] **(H-S3)** Two M5StickS3 devices: same test, byte-identical wire behaviour
- [ ] **(H)** Mixed pair test: Plus2 Master + S3 Slave, and S3 Master + Plus2 Slave, both work transparently (proves protocol is genuinely chip-agnostic)
- [ ] **(H)** Slave dual-channel scan: with no traffic, Slave defaults to channel 1; with channel 11 traffic present, Slave locks to channel 11 (show priority verified)
- [ ] **(H)** Slave channel-lock recovery: after channel 11 master goes silent for >1 second, Slave returns to dual-channel scan and finds channel 1 traffic if present
- [ ] **(H)** Master role selection: hobby mode defaults to channel 1, show mode requires explicit Config Mode selection of channel 11
- [ ] **(H)** Signal-strength battery-style indicator on Slave screen shows correct number of bars at known distances; verified by walking a slave away from a stationary master
- [ ] **(H)** Diagnostic RSSI mode (Test Mode) shows numeric dBm and per-source RSSI history
- [ ] **(H)** Slave with Repeat enabled rebroadcasts received frames; a third device in radio range of the repeater but out of range of master receives correctly
- [ ] **(H)** Deduplication confirmed: master sending each frame 3-5 times produces only one IR fire per logical beat
- [ ] **(H)** Slave detects master loss within 1 second of master being powered off; falls back to subtle hue cycle
- [ ] **(H)** Group ID targeting works: Master sends LIGHT_COMMAND with target_group=2; only Slaves assigned to group 2 fire IR
- [ ] **(H)** TX power is set correctly per device role; verified by measuring effective range at default vs maximum power
- [ ] **(H)** Range estimate validation: 3-hop mesh with line-of-sight covers ≥200m in open-air conditions (single empirical test session)
- [ ] **(H)** Hackspace stress test: at least one session with 5+ devices in active broadcast mode in a deliberately-noisy 2.4GHz environment (microwave running, streaming devices on WiFi, multiple Bluetooth speakers active). Acceptable degradation = <5% missed beats over a 5-minute period.

## Next blocks of work

A pragmatic ordering, not a formal SDLC decomposition. Each block is roughly one or two focused work sessions; sub-tasks within a block are quick wins.

### Block 1: Frame format and unit-tested encode/decode

Get the wire-format struct and round-trip native unit tests working before any radio transmission. This is the cheapest, most-isolating piece of work and runs entirely on the laptop.

- Define frame format struct in shared header (matches spec §4.3 byte-for-byte)
- Encoder: struct → byte buffer
- Decoder: byte buffer → struct, with header validation (protocol version check, length sanity, message type validity)
- Native unit tests: round-trip every message type, including malformed-input rejection
- Commit: "ESP-NOW frame format with encoder, decoder, native tests"

### Block 2: M5StickS3 first-class HAL

Get the S3 building, flashing, and running as a **first-class HAL implementation**, not just a parity port. The S3 is the project's future reference platform (Plus2 is EOL); future contributors will arrive directly on S3, so the HAL should expose the chip's genuine advantages from day one rather than treating them as a future optimisation Epic.

The S3 advantages worth exploiting **in this Epic**:

1. **`esp-dsp` library for FFT** - Espressif's own DSP library uses the S3's vector (PIE) instructions for SIMD-accelerated FFT. Roughly 5-10x faster than `arduinoFFT` on the same chip. The HAL exposes a single `fft()` capability; the Plus2 implementation uses `arduinoFFT`, the S3 implementation uses `esp-dsp`. The orchestration layer above the HAL is unaware of which is in use.
2. **ES8311 codec for audio input** - the S3 has a proper stereo audio codec (configured over I²C, audio data on I²S) with a high-sensitivity MEMS microphone, rather than the Plus2's basic PDM mic on a single GPIO. Cleaner SNR, lower noise floor, better signal for the FFT pipeline. The HAL's `audio_input` capability hides which physical path is in use.
3. **8MB PSRAM (vs Plus2's 2MB)** - room for longer dedup ring buffers, larger event history for diagnostics, more elaborate effect state. The HAL is designed to *allow* this larger headroom rather than mandating its use; current code can stay within Plus2 limits and S3 deployments simply have more headroom.
4. **Native USB-OTG** - no CH9102 USB-to-UART quirks; flashing is faster and the serial port presents more reliably to PlatformIO. No code change needed; just nice to have.
5. **IR receiver capability** - the S3 has both an IR transmitter and receiver, where the Plus2 only had transmit. The HAL exposes `ir-rx` as a capability declared by the S3 profile (and absent from the Plus2 profile). Not used in Epic 4 itself, but the capability is wired up so future Epics (group ID verification by hearing your own bracelet's ack, decoding other PixMob deployments, etc.) can use it without HAL changes.
6. **BLE 5.0** - higher bandwidth, longer range BLE for future device pairing scenarios. Out of Epic 4 scope; the HAL exposes `ble` as a capability without exercising it.

Work:

- Add `[env:m5stick-s3]` to `platformio.ini` if not already present from earlier multi-target work; resolve PlatformIO board reference (likely `esp32-s3-pico` or M5Stack's published platformio template)
- Update HAL to declare both `M5StickCPlus2` and `M5StickS3` host profiles per §2 architecture
- **GPIO map**: source canonical pin assignments from M5Stack's official StickS3 docs. IR LED, screen pins, button pins, audio in pins, power-hold pin all differ from Plus2. Each pin number lives in one place in the HAL; everything above is shared.
- **Audio-input HAL implementation for S3**: the S3's audio path is genuinely different from the Plus2's PDM mic - it routes through an ES8311 codec configured over I²C with audio data on I²S. The HAL needs an audio-input abstraction that exposes the same conceptual API regardless of which physical path is used. **If Epic 2's HAL already established this abstraction**, the S3's job is to provide the ES8311 implementation. **If Epic 2 left audio input as direct PDM access**, a small refactor is needed first to introduce the abstraction. Confirm which case applies before committing to estimate.
- **FFT HAL implementation for S3**: introduce a `fft()` capability in the HAL that produces magnitude spectra. Plus2 backend uses `arduinoFFT`. S3 backend uses `esp-dsp` to exploit the chip's SIMD vector instructions. The orchestration layer above receives spectrum frames at the same conceptual rate from either backend.
- **Display HAL implementation for S3**: same physical screen (1.14" ST7789V2 at 135x240) so the framebuffer code from Plus2 should largely work; just confirm the SPI pin assignments and rotation in the HAL.
- **Buttons HAL implementation for S3**: pin numbers differ; behavioural API stays identical (A/B/PWR press and long-press events).
- **IR transmit HAL implementation for S3**: the S3 has a different IR LED pin assignment but the same 38 kHz modulated carrier work. Existing IR encoder code should be unchanged; only the GPIO number differs.
- **IR receive HAL capability for S3**: declare `ir-rx` as a capability of the S3 profile. Implement a basic receive callback that decodes incoming IR packets per the PixMob protocol. Not exercised by Epic 4 logic, but available for future use.
- **PSRAM-aware data structures**: where the existing code uses fixed-size buffers sized for Plus2's 2MB PSRAM, expose a HAL constant for available PSRAM so future code can size structures appropriately on S3 without a rewrite.
- **Verification on real hardware**: flash the S3, confirm it boots into the existing menu, fires IR at a PixMob bracelet correctly using the existing Test Mode "Pulse Test" (§8.5), and the Vengaboys beat-detection works through its mic. Verify FFT performance: the S3 should complete a 512-point FFT noticeably faster than the Plus2.
- Commit: "S3 first-class HAL: builds, boots, fires IR, beat-detects audio, exploits esp-dsp FFT and ES8311 codec"

**Acceptance for Block 2 specifically**:

- [ ] **(B)** `pio run -e m5stick-s3` succeeds without warnings under `-Wformat -Wformat-security`
- [ ] **(L)** Native unit tests (which don't touch HAL) still pass after HAL refactor
- [ ] **(H-S3)** Flashed S3 boots, displays the existing UI, and fires IR at a PixMob bracelet using Test Mode
- [ ] **(H-S3)** Beat detection works on Vengaboys playing at 138 BPM, identical visible behaviour to the Plus2 reference
- [ ] **(H-S3)** FFT cycle time is measurably faster on S3 than Plus2 (using `esp-dsp` vs `arduinoFFT`); approximate figures captured for the spec
- [ ] **(H-S3)** Audio noise floor is measurably lower on S3 than Plus2 (cleaner ES8311 vs basic PDM); subjective check that beat detection works at lower playback volume
- [ ] **(H-S3)** `ir-rx` capability declared in S3 profile and basic decode of an inbound PixMob frame demonstrated (not used by Epic 4, just verified as a capability)
- [ ] **(H)** No regression on Plus2: same firmware codebase still builds and runs cleanly under `[env:m5stick-plus2]` after the HAL changes

### Block 3: Master broadcast on Plus2 (channel 1, hobby)

Now that we have two physical test devices (Plus2 + S3 from Block 2), get the Master role transmitting on the Plus2, verified by sniffing in promiscuous mode on the S3. Default to channel 1 / hobby mode for the entirety of this block - we'll add the show-mode override in Block 6.

- HAL: `radio_init(channel)`, `radio_send_frame(buf, len)` for ESP-NOW
- Master Mode integration on Plus2: existing beat detection produces BEAT_DETECTED frames at heartbeat rate
- Configurable TX power (default +21 dBm)
- Verification: S3 in promiscuous mode logs received frames; Wireshark capture validates byte structure
- Commit: "Master broadcast on channel 1 (hobby) from Plus2; S3 sniffer validates wire format"

### Block 4: Slave receive end-to-end (Plus2 master, S3 slave)

Get a Slave receiving frames from a Master and firing IR locally **and lighting up its own display as a visible light point**. Single-channel for this block - dual-channel scan comes in Block 6. With both devices working, this block proves the cross-platform interop story almost incidentally.

A crucial piece often missed: when a Stick is in Slave Mode, **its display is itself a light point in the show**, behaving like a PixMob bracelet. On a beat firing, the screen flashes the same colour with the same envelope as the IR command sent to bracelets. This makes a Stick on a tripod alongside bracelets a coherent piece of installation gear - it isn't just a transmitter that drives lights, it *is* one of the lights. For the constellation art piece, a single Stick can function as both transmitter AND visible light point, reducing the bracelet count needed.

- HAL: `radio_register_recv_callback()` for ESP-NOW (on both Plus2 and S3 since both will need it)
- Slave Mode integration: received BEAT_DETECTED triggers existing IR fire path
- **Display-as-light**: same beat event also drives the screen via the existing display HAL. Full-screen colour wash with matching ASR envelope. Behaves like the PixMob bracelet group it's targeting (or a configurable own-group, per Slave-Mode config).
- Deduplication ring buffer
- Master-loss detection at 1 second, fallback to idle effect (also visible on the screen as the device's own ambient state)
- Verification: Plus2 Master + S3 Slave, IR fires on S3 in time with Master-detected beats, **and** the S3's screen flashes in sync
- Bonus: swap roles (S3 Master + Plus2 Slave); confirm interop is genuinely symmetric
- Commit: "Slave receive end-to-end with display-as-light-point; Plus2↔S3 cross-platform interop verified"

### Block 5: Repeat mode

The last functional piece of the basic mesh.

- Slave-with-Repeat config option (default OFF)
- Repeat increments hop_count, drops on hop_count >= 3
- Sanity test: 3-device chain (Master → Repeater → Slave-out-of-range-of-Master). Could be Plus2 + S3 + a borrowed third device; or chain just two devices and verify the hop_count increment in packet captures.
- Commit: "Repeat mode with 3-hop limit"

### Block 6: Two-channel architecture (hobby + show)

Introduce the channel-priority dual-scan logic. This is the architecturally meaningful block.

- Master role selection in Config Mode: hobby (channel 1, default) / show (channel 11) / custom
- Slave dual-channel scan at boot per architecture spec §4.5: channel 11 for ~2 seconds first, then channel 1 for ~2 seconds, loop until heartbeat detected
- Slave reverts to scan loop after 1-second heartbeat loss
- Test scenario: bring up channel 1 master, slave locks to channel 1; bring up channel 11 master concurrently, slave switches to channel 11 (show priority); kill channel 11 master, slave returns to channel 1 within heartbeat-loss window
- Commit: "Two-channel hobby/show architecture with show-priority slave scan"

### Block 7: RSSI capture and battery-style display

Capture and display per-frame signal strength so operators can position devices intelligently.

- Detect ESP-IDF version: if v5.3+, use `esp_now_recv_info_t` for RSSI; if older, fall back to promiscuous-mode sniffer pattern (filter for Espressif OUI `18:fe:34` action frames)
- Track RSSI per source_id in a small in-memory table
- Slave UI: 4-bar signal indicator on status screen, paired with battery icon. Visual style matches the brand (per Brand and visual identity page).
- Diagnostic mode (Test Mode): numeric dBm value and per-source RSSI history
- Master UI: RSSI of any repeater traffic overheard, useful for mesh-quality monitoring
- Commit: "RSSI tracking and battery-style operator display"

### Block 8: Range validation and stress testing

The deliberate empirical session that validates the design.

- Open-air range test: line-of-sight 3-hop chain, measure achievable distance. Target ≥200m.
- Hackspace stress test: 5+ devices in deliberately-noisy environment (microwave on, WiFi streaming, Bluetooth active), measure missed-beat rate over 5 minutes
- Document findings in spec §4.7 (new section: "Empirical performance characteristics") - to be added to architecture spec
- Commit: "Range and stress test results documented"

## Features

Anticipated Features (Isambard or Jason can decompose into Tasks before dispatch):

- Feature 4.1: ESP-NOW frame format (encoder + decoder + header validation + native tests)
- Feature 4.2: ESP-NOW master broadcast (Master Mode integration)
- Feature 4.3: ESP-NOW slave receive (Slave Mode integration + deduplication)
- Feature 4.4: Repeat mode (slave-also-rebroadcasts)
- Feature 4.5: Two-channel architecture (hobby/show, master role selection, slave dual-channel scan)
- Feature 4.6: Heartbeat and master-loss timeout handling
- Feature 4.7: Group ID targeting in LIGHT_COMMAND
- Feature 4.8: RSSI capture and battery-style signal display
- Feature 4.9: Multi-target HAL audit (ensure Plus2 and S3 both build and run)
- Feature 4.10: Range validation and stress testing

## Dependencies

| Dependency | Type | Status | Owner |
| --- | --- | --- | --- |
| Epic 2 (architecture refactor with transport abstraction) | Internal | Done | Jason |
| Epic 3 (UI for ESP-NOW config menus) | Internal | Done | Jason |
| Architecture spec v0.16+ (§4.3, §4.5, §8.4) | Internal | Done | Jason |
| Two or more StickC devices for testing | External | Available (Jason has multiple) | Jason |

## Target Sprint Range

- **Start sprint:** After Epic 2 (3 can run in parallel)
- **End sprint:** 2-3 sprints later
- **Indicative complexity total:** 20-25 points across child Features

## Status Notes

Proposed 2026-05-06; refined 2026-05-08 multiple times.

**v3 refinement (2026-05-08)**: Block 2 promoted to S3 HAL work (was Block 4) since the S3 is now a physical device on Jason's desk; pre-S3 ESP-NOW work would mean dragging an old Plus2 out of a drawer for two-device tests when there's a fresh S3 sitting there. The block ordering now reflects: format → second device working → ESP-NOW → protocol features.

**v4 refinement (2026-05-08)**: two important architectural clarifications.

1. **S3 HAL is first-class, not parity-only.** Block 2 now exploits the S3's genuine advantages (esp-dsp FFT with vector instructions, ES8311 codec audio quality, 8MB PSRAM headroom, native USB-OTG, IR receive capability declaration, BLE 5.0 capability declaration) rather than treating these as future enhancements. The S3 is the project's future reference platform (Plus2 EOL); future contributors will arrive directly on S3, so the HAL should look right from day one. The Plus2 backend continues to work and test against the same protocol; the S3 backend doesn't degrade Plus2 behaviour.
2. **Slave Mode display behaves as a visible light point.** Block 4 now explicitly includes display-as-light behaviour: when a Stick is in Slave Mode, its screen flashes in sync with the IR command, like a PixMob bracelet. This makes a Stick on a tripod a coherent piece of installation gear (it's a light, not just a transmitter), and means the constellation art piece could use a single Stick as both transmitter and visible light point.

The two-channel architecture is genuinely meaningful: it makes slave-only devices (future bracelets, Tildagons in audience) zero-configuration in the most useful way, since show traffic always wins over hobby traffic with no user input needed. It also encodes a clear social contract for the project's deployment ecosystem - hobbyists know channel 1 is theirs to play with, professional operators know channel 11 is reserved for production. This is good architecture not just for technical reasons but for community reasons.

Key technical risks:

1. **Audio-input HAL abstraction.** The Plus2's PDM mic and the S3's ES8311 codec are genuinely different driver paths. If Epic 2 already established an audio-input abstraction in the HAL, Block 2's S3 audio work is straightforward. If Epic 2 left audio as direct PDM access, a small refactor is needed first. Worth confirming the state of Epic 2's HAL before committing to a Block 2 estimate.
2. **`esp-dsp` library compatibility.** First time the project has used `esp-dsp`. PlatformIO library availability and Arduino-framework compatibility need a quick sanity check before committing the FFT path. If it doesn't drop in cleanly, the S3 backend can fall back to `arduinoFFT` for now and earn the optimisation later.
3. **ESP-NOW behaviour under spectrum congestion** (NullSector / EMF environments). The 4 Hz max frame rate, the 3-5 redundant transmissions, channel separation (1 hobby / 11 show), and TX-power maximisation are all mitigations. Block 8's stress test exists specifically to validate these empirically before committing the design to NullSector deployment.

The Plus2 EOL situation creates a small additional task: at the end of this Epic, the architecture spec should be updated to reflect that the S3 is now the preferred reference platform, with the Plus2 supported as legacy. Current spec §3 still leans Plus2-primary; the work in this Epic to prove S3 first-class behaviour is what unlocks that spec change.

Processing Type stays Hybrid because most of this is laptop-driven coding work where Claude Code is genuinely a useful pair. The exception is Block 8's stress test, which is genuinely Manual - Jason and a deliberately-noisy environment.

**Block 3 hardware-verification update (2026-05-09)**: ESP-NOW transmit + receive HAL backends landed on both Plus2 and S3. AutonomousMaster broadcasts BEAT_DETECTED + LIGHT_COMMAND on each detected beat. Slave mode receives, decodes header, defers payload work to main-loop (essential - calling IRsend::sendRaw from the WiFi callback context crashes the S3). Slave's screen renders the broadcast colour with attack/sustain/release fade through a new `LocalDriver` RgbPulse handler; orchestration just calls `DAL::render_fx("local", ev)` and the driver owns the per-frame paint at ~30 Hz.

Several architectural clarifications adopted during Block 3 (now in spec §4.3, §4.7-Bluetooth, §7.4):

1. **`render_fx(target, event)` is the canonical effect entry point** for orchestration. Existing per-capability `fire_*` helpers stay for legacy call sites; new effect types (text overlay, simple graphics, scripted animations - per §6 future work) ship as `render_fx` overloads on new event structs.
2. **Slave-as-target-device**: a slave is just one of the lights in the show, not just an ESP-NOW relay. On a beat it lights its own screen AND forwards IR to nearby bracelets - both via explicit `render_fx` calls. No auto-forwarding inside `render_fx`.
3. **No auto-promote on master loss**: corrects spec §4.3's original "fall back to autonomous mode" wording. Slaves stay slaves and run a local idle effect; promotion only via explicit operator action.
4. **Heartbeat 4 Hz → 1 Hz with skip-if-recent**: lower TX/RX duty cycle for battery-powered receivers; `BEAT_DETECTED` traffic during music covers the alive-signal so heartbeat traffic drops to roughly zero.
5. **Bluetooth as control plane**: future Epic adds BLE so a phone app can drive any host within Bluetooth range; the host fans `render_fx()` out over ESP-NOW. `Capability::Bluetooth` is declared by the HAL on Plus2 (BLE 4.2) and S3 (BLE 5.0) now so the API surface is ready.
6. **Per-capability driver gates**: `Config → Display → Pulse Enable` (NVS-backed) gates the LocalDriver's `RgbPulse` handler only - status text and other display events stay unaffected. Finer-grained than driver-level enable.
7. **Test mode joins the broadcast**: Test sub-tests (Pulse / Fade / Rainbow / Sparkle / WhiteOut) now `render_fx("local", ev)` for the screen AND call a shared `EspNowBroadcaster` helper to broadcast LIGHT_COMMAND, so any slave on the channel renders the same colour during a test the same way it does during a real show.

Outstanding follow-ups carried into Block 4 / Block 5 / Block 6:

- Replace the `EspNowBroadcaster` helper struct with a proper `EspNowDriver` registered in the DAL behind `render_fx("esp-now-broadcast", ev)`. The struct is a stepping stone; once it lands, the per-mode radio lifecycle goes away.
- Slave forwards IR to its **own configured group**, not `"all-pixmobs"` broadcast. Slave NVS-stores its group ID; target string built dynamically. Avoids two slaves at opposite ends of a venue both broadcast-firing into the same airspace.
- Brand-independent target naming sweep (`"all-pixmobs"` etc.) - deferred until a second IR protocol or NocturNation-native bracelet code arrives so the abstraction has a real second consumer.
- esp-dsp `_aes3` SIMD FFT path on S3 - currently using `_ansi` because `_aes3` produced erratic per-bin magnitudes for our packed-real-as-complex input layout. ANSI path is still meaningfully faster than arduinoFFT (float vs double, better cache behaviour); recovering the SIMD win is a focused investigation.
- Empirical IR radiation patterns: Plus2 IR LED is omnidirectional, S3 is more focused. Useful for deployment planning - operator can stack a Plus2 + S3 in the same venue with the S3 aimed at the dance floor and the Plus2 doing general fill.
