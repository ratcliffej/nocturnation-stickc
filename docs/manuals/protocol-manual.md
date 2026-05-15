---
title: "NocturNation protocol manual"
status: Draft
protocol_version: 0x01
firmware_version: "v0.5"
notion_url: https://www.notion.so/35ebd067740580378400ec3e0e8a0ca0
notion_id: 35ebd067740580378400ec3e0e8a0ca0
last_synced: 2026-05-12
sync_direction: bidirectional
---

# NocturNation protocol manual

> Normative specification of the NocturNation protocol: ESP-NOW transport, frame formats, class-and-group addressing, the PixMob infra-red encoding annex, channel discovery, the firmware non-volatile-storage schema, conformance requirements, and reference test vectors.

This is the implementer-facing document. If you are an operator setting up a venue, read the [user manual](user-manual.md) instead. If you are designing show plug-ins for the NocturNation firmware, read [developing-shows.md](../developing-shows.md). For visual reference alongside this spec, the [flow-diagrams document](flow-diagrams.md) has Mermaid renderings of the receive pipeline and class-and-group routing.

**Protocol version specified by this document**: `0x01`.
**Reference firmware version**: v0.5 (`include/firmware_version.h`).
**Reference encoder for the PixMob IR annex**: [jamesw343/PixMob_IR](https://github.com/jamesw343/PixMob_IR).

---

## Contents

1. [Scope and conventions](#1-scope-and-conventions)
2. [Wireless layer](#2-wireless-layer)
3. [Frame format](#3-frame-format)
4. [Class-and-group addressing](#4-class-and-group-addressing)
5. [Channel discovery](#5-channel-discovery)
6. [Heartbeat and liveness](#6-heartbeat-and-liveness)
7. [Conformance](#7-conformance)
8. [Annex A: PixMob infra-red encoding](#annex-a-pixmob-infra-red-encoding)
9. [Annex B: Non-volatile-storage schema](#annex-b-non-volatile-storage-schema)
10. [Annex C: Reference test vectors](#annex-c-reference-test-vectors)
11. [Annex D: Protocol version history](#annex-d-protocol-version-history)

---

## 1. Scope and conventions

### 1.1 Scope

This document specifies the wire-visible behaviour of a NocturNation node: every byte transmitted on the radio link, every byte transmitted on the infra-red link, and every byte stored in non-volatile storage that influences either. It does not specify firmware internals such as plug-in architecture, the audio analyser tuning, or the configuration-menu structure; those are operator-facing concerns described in the user manual and architectural concerns described in [docs/architecture.md](../architecture.md).

An implementation conforming to this document interoperates with the reference firmware over both the radio link and the infra-red link.

### 1.2 Normative language

Throughout this document, the words **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY** are to be interpreted as defined in IETF RFC 2119 (Bradner, 1997).

### 1.3 Notation

- Hexadecimal values are prefixed `0x`. Byte ranges are inclusive on both ends.
- Multi-byte integer fields are **little-endian** unless explicitly stated otherwise.
- Bit numbering in bitfield diagrams is most-significant-bit first when read left-to-right; the lowest-numbered bit is the least significant.
- Field offsets are zero-based byte offsets from the start of the enclosing structure.

### 1.4 Versioning

Every frame begins with a one-byte `protocol_version` field. The value specified by this document is `0x01`. A receiver MUST validate this byte and discard frames whose version it does not recognise. Future revisions of the protocol MAY introduce new message types within the same version (using reserved opcodes) or MAY bump the version byte if a wire-incompatible change is required.

The protocol version is independent of the firmware version. Firmware version `v0.5` implements protocol version `0x01`.

### 1.5 Licence

This document is licensed under [Creative Commons Attribution-ShareAlike 4.0](https://creativecommons.org/licenses/by-sa/4.0/) (CC BY-SA 4.0). Implementations are MIT-licensed (see the reference firmware's [LICENSE.code](../../LICENSE.code) file). Hardware designs derived from the reference platforms are CERN-OHL-S 2.0.

---

## 2. Wireless layer

### 2.1 Carrier

NocturNation operates on the 2.4 GHz ISM band using Espressif's **ESP-NOW** transport, which carries vendor-specific action frames in the IEEE 802.11 management category. ESP-NOW is connection-less and broadcast-friendly; the wireless medium is unencrypted and unauthenticated at this protocol version (Tier 0 security; see [security RFC](https://www.notion.so/) for the deferred Tier 1 path).

A NocturNation frame is encapsulated as the payload of one ESP-NOW vendor action frame. The destination address is the broadcast MAC `ff:ff:ff:ff:ff:ff`. Receivers process every frame that arrives on the current channel; no association is required.

### 2.2 Channels

NocturNation uses three of the standard non-overlapping 2.4 GHz Wi-Fi channels. A master MUST be configured on exactly one of these channels at any moment:

| Channel | Centre frequency | Role |
|---|---|---|
| 1 | 2412 MHz | Hobby / open community (default) |
| 6 | 2437 MHz | Advanced operator override |
| 11 | 2462 MHz | Show / commercial |

Channel 11 is suggested for high-density public deployments because it is least congested in venue environments dominated by 2.4 GHz Wi-Fi infrastructure on channels 1 and 6.

Slaves MAY be locked to a single channel or MAY auto-scan. See [section 5](#5-channel-discovery).

### 2.3 Redundancy

A master transmitting a frame MUST send it **three** times in immediate succession on the same channel. The three transmissions carry identical bytes; in particular, they carry the **same** sequence number (see [section 3](#3-frame-format)). This redundancy absorbs the occasional ESP-NOW packet loss without requiring acknowledgement.

A receiver MUST deduplicate against a ring of at least sixteen recently-seen `(source_id, sequence_number)` pairs. A frame matching any entry in the ring MUST be dropped silently (no processing, no further forwarding). A frame not matching any entry MUST be processed and the pair MUST be appended to the ring, evicting the oldest entry if the ring is full.

### 2.4 Repeater behaviour

A slave MAY operate as a **repeater**, in which case every accepted frame (one that passed deduplication) is retransmitted with the `hop_count` field incremented by one. A receiver MUST drop frames with `hop_count` greater than 3 to bound the relay topology. The default behaviour is no repeating.

### 2.5 No acknowledgement, no return path

NocturNation is unidirectional. A slave never transmits a frame back to the master; a bracelet never transmits anything at all. Master state (sequence numbers, mode, channel) is the single source of truth; slaves derive their behaviour from received frames and from local configuration.

---

## 3. Frame format

### 3.1 Header

Every frame begins with a six-byte header:

| Offset | Field | Size | Description |
|---:|---|---:|---|
| 0 | `protocol_version` | 1 | Always `0x01` at this revision |
| 1 | `source_id` | 1 | 1..254 = sender id; `0xFF` = broadcast / anonymous |
| 2 | `sequence_number` | 1 | Wraps 1..255 in monotonic order per source; `0x00` indicates no sequencing |
| 3 | `hop_count` | 1 | 0 = original transmission; receiver MUST drop frames where hop_count > 3 |
| 4 | `message_type` | 1 | See [section 3.2](#32-message-types) |
| 5 | `payload_len` | 1 | Bytes of payload following the header |
| 6..N | `payload` | `payload_len` | Type-specific (see [section 3.3](#33-payloads)) |

`kHeaderSize = 6`. `kMaxFrameSize = 32`. `kMaxPayloadSize = kMaxFrameSize - kHeaderSize = 26`.

A receiver MUST verify that `payload_len` matches the expected length for the given `message_type` (see [section 3.3](#33-payloads)) and SHOULD silently discard frames whose `payload_len` is inconsistent.

### 3.2 Message types

| Code | Name | Payload size | Direction |
|---:|---|---:|---|
| `0x00` | `HEARTBEAT` | 0 | Master to all |
| `0x01` | `BEAT_DETECTED` | 3 | Master to all (currently not emitted by reference firmware) |
| `0x02` | `MODE_CHANGE` | 2 | Master to all |
| `0x03` | `LIGHT_COMMAND` | 9 | Master to all |
| `0x04` | `CLOCK_SYNC` | 4 | Master to all |
| `0x05` | `TIME_SYNC` | 5 | Master to all |
| `0x06` | `MUSIC_EVENT` | 1 | Master to all |
| `0xFF` | `EXTENSION` | variable | Reserved for future use |

Codes `0x07..0xFE` are reserved. A receiver MUST treat any reserved or unrecognised code as a request to silently discard the frame.

A receiver MUST honour at minimum: `HEARTBEAT`, `LIGHT_COMMAND`. A receiver SHOULD honour `MODE_CHANGE` and `MUSIC_EVENT` if it has any locally interpretable behaviour for them. A receiver MAY honour the remaining types.

### 3.3 Payloads

#### 3.3.1 `HEARTBEAT` (`0x00`)

Zero-byte payload. See [section 6](#6-heartbeat-and-liveness) for semantics.

#### 3.3.2 `BEAT_DETECTED` (`0x01`)

| Offset | Field | Size | Description |
|---:|---|---:|---|
| 0 | `strength` | 1 | 0..255, monotonic-ish measure of beat strength |
| 1 | `bpm_x10` | 2 LE | Instantaneous tempo in BPM × 10 (e.g. 1280 = 128.0 BPM) |

Reserved for future use. The reference firmware does not currently emit this type (the `BEAT_DETECTED` broadcast was removed in Epic 4.5 once `LIGHT_COMMAND` became the canonical fire); the wire format is retained against future re-enabling.

#### 3.3.3 `MODE_CHANGE` (`0x02`)

| Offset | Field | Size | Description |
|---:|---|---:|---|
| 0 | `new_mode` | 1 | Target runtime-mode id (see firmware-internal `ModeId` enum) |
| 1 | `palette_id` | 1 | Reserved; carries 0 in reference firmware |

A receiver MAY use this to follow the master into a coordinated mode change.

#### 3.3.4 `LIGHT_COMMAND` (`0x03`)

The most-emitted message type; carries every render fire on the system.

| Offset | Field | Size | Description |
|---:|---|---:|---|
| 0 | `target_class` | 1 | See [section 4](#4-class-and-group-addressing); 0 = all classes, 1 = Light, 2 = Screen, 3 = MultiLedScreen |
| 1 | `target_group` | 1 | 0 = broadcast within class; 1..255 = specific group (PixMob receivers further constrain to 1..31) |
| 2 | `r` | 1 | Red 0..255 |
| 3 | `g` | 1 | Green 0..255 |
| 4 | `b` | 1 | Blue 0..255 |
| 5 | `attack` | 1 | Envelope attack stage; PixMob `Time` enum index 0..7 (see [annex A.3](#a3-time-and-chance-enumerations)) |
| 6 | `sustain` | 1 | Envelope sustain stage; PixMob `Time` enum index 0..7 |
| 7 | `release` | 1 | Envelope release stage; PixMob `Time` enum index 0..7 |
| 8 | `chance` | 1 | Probability gate; PixMob `Chance` enum index 0..7 (see [annex A.3](#a3-time-and-chance-enumerations)) |

A receiver whose configured `device_class` matches `target_class` (or `target_class == 0x00`), and whose configured `group` matches `target_group` (or `target_group == 0x00`), MUST render this command according to its own device class. See [section 4](#4-class-and-group-addressing) for the full routing semantics.

#### 3.3.5 `CLOCK_SYNC` (`0x04`)

| Offset | Field | Size | Description |
|---:|---|---:|---|
| 0 | `phase_in_bar` | 2 LE | Current phase in the bar, in milliseconds since the most recent down-beat |
| 2 | `bpm_x10` | 2 LE | Tempo in BPM × 10 |

Reserved for future bar-locked behaviour. Reference firmware does not currently emit.

#### 3.3.6 `TIME_SYNC` (`0x05`)

| Offset | Field | Size | Description |
|---:|---|---:|---|
| 0 | `days_since_2026` | 2 LE | Day count since 2026-01-01 |
| 2 | `centiseconds_today` | 3 LE | Centiseconds since local midnight (0..8639999) |

Reserved for future time-of-day-aware behaviour. Reference firmware does not currently emit.

#### 3.3.7 `MUSIC_EVENT` (`0x06`)

| Offset | Field | Size | Description |
|---:|---|---:|---|
| 0 | `event_type` | 1 | 0 = Unknown, 1 = Drop, 2 = Breakdown, 3 = Build (reserved) |

Emitted by the master's DropDetector. A receiver MAY interpret this to alter local behaviour (e.g. a different palette during a breakdown).

#### 3.3.8 `EXTENSION` (`0xFF`)

Reserved for future use. A receiver MUST silently discard frames of this type at protocol version `0x01`.

---

## 4. Class-and-group addressing

### 4.1 Device classes

Every NocturNation receiver advertises a **device class** in the range `0x00..0xFF`. The class identifies the kind of device the receiver presents on the wire.

| Code | Name | Description |
|---:|---|---|
| `0x00` | `All` | Addressing wildcard; never returned by a receiver, only used as a `target_class` for global broadcast |
| `0x01` | `Light` | Discrete-LED bracelet or wristband; e.g. PixMob X4 Gen 3.1 |
| `0x02` | `Screen` | Framebuffer-bearing device; e.g. the Stick's LCD |
| `0x03` | `MultiLedScreen` | Device with both discrete LEDs and a framebuffer; e.g. the Tildagon (Epic 5) |
| `0x04..0xFF` | reserved | Future use |

A receiver MUST advertise a single class. A receiver running multiple bindings (the reference firmware running both a screen display binding and a PixMob infra-red binding) MAY treat each binding as a separate logical receiver, each with its own class and group.

### 4.2 Group filtering

Every NocturNation receiver also advertises a **group** in the range `0x00..0xFF`. The group is a one-byte filter; multiple receivers MAY share a group. Bracelet receivers in the PixMob class further constrain the group to the five-bit subset `0x00..0x1F` because the on-wire PixMob protocol carries only five bits of group; see [annex A.1](#a1-frame-format).

A receiver MUST accept a `LIGHT_COMMAND` if and only if:

- `target_class == 0x00` OR `target_class == receiver.class`, AND
- `target_group == 0x00` OR `target_group == receiver.group`.

`target_group == 0x00` is the **broadcast group**: every receiver MUST accept it regardless of its own configured group, including a receiver whose configured group is itself `0x00`. The broadcast group is the canonical "address everyone in this class" form and is the default routing for `render_fx` calls on the reference master.

A receiver whose configured group is `0x00` is treated as "in no specific group". It MUST accept the broadcast group (`target_group == 0x00`) but MUST NOT accept any non-zero `target_group`. This mirrors the way a PixMob bracelet whose factory-programmed group is 0 only responds to the broadcast group on its infra-red link.

A receiver SHOULD therefore advertise a non-zero group in deployment. A receiver whose group is intentionally `0x00` is opting out of all per-group fan-out routing and only sees broadcasts.

### 4.3 Worked routing examples

| `target_class` | `target_group` | Routes to |
|---:|---:|---|
| `0x00` | `0x00` | Every receiver of every class - global broadcast |
| `0x00` | `0x07` | Every receiver in group 7, regardless of class |
| `0x01` | `0x00` | Every Light-class receiver in any group |
| `0x01` | `0x07` | Light-class receivers configured for group 7 only |
| `0x02` | `0x00` | Every Screen-class receiver - typically the operator's own LCD |
| `0x03` | `0x01` | Every MultiLedScreen-class receiver (Tildagon, Epic 5) in group 1 |

### 4.4 Master-side dispatch

The reference firmware's dispatch function `dispatch_output_class_group` (`src/dal/dal.cpp`) fans every render call out to three sinks:

1. **ESP-NOW broadcast** - always, regardless of `target_class`. Every slave on the channel sees the frame.
2. **Local infra-red transmitter** - only when `target_class` is `0x00` (All) or `0x01` (Light). This is the master's habit of treating itself as one of its own slaves (the "loopback"). Exactly one IR frame is sent per dispatch call.
3. **Local screen pulse** - only when `target_class` is `0x00` (All) or `0x02` (Screen). Drives the LCD pulse animation.

This is dispatch-side behaviour and is not visible to the wire; a third-party master implementation MAY adopt the same loopback or omit it.

---

## 5. Channel discovery

### 5.1 Master

The master is configured for a fixed channel (1, 6, or 11) via non-volatile storage (`mst_chan`; see [annex B](#annex-b-non-volatile-storage-schema)). It MUST NOT change channels during a deployment.

### 5.2 Slave - locked mode

A slave configured with `slv_chan ∈ {1, 6, 11}` MUST set its Wi-Fi to that channel and remain there.

### 5.3 Slave - auto-scan mode

A slave configured with `slv_chan == 0x00` MUST perform an auto-scan, defined as the following sequence:

1. Set channel to 11. Listen for up to two seconds for any NocturNation frame with valid `protocol_version`.
2. If a frame is received, lock to channel 11 and exit scan.
3. Otherwise, set channel to 1. Listen for up to two seconds.
4. If a frame is received, lock to channel 1 and exit scan.
5. Otherwise, repeat from step 1.

Channel 11 is checked first because it is the suggested show channel and is presumed higher priority. Channel 6 is not auto-scanned; a slave on channel 6 MUST be explicitly locked.

A slave that has locked to a channel SHOULD re-enter auto-scan if it loses traffic for longer than the NO SIGNAL threshold (see [section 6](#6-heartbeat-and-liveness)) and the slave was originally configured for auto-scan.

---

## 6. Heartbeat and liveness

### 6.1 Master heartbeat

The master MUST emit `HEARTBEAT` frames at no slower than 1 Hz when there is no other traffic. The master MAY suppress a heartbeat if it has transmitted any other frame within the heartbeat period; this is the "skip-if-recent" rule and minimises duty cycle during active music.

The heartbeat carries no payload (`payload_len == 0`). It serves only to demonstrate master liveness on the wire.

### 6.2 Receiver liveness check

A receiver MUST consider the master alive whenever it has received any frame within the last `kNoSignalGapMs` milliseconds. The reference firmware uses `kNoSignalGapMs = 3000` (three times the maximum heartbeat period).

A receiver that detects master loss SHOULD indicate this clearly to a local operator (the reference firmware shows NO SIGNAL on the LCD). A receiver MUST NOT promote itself to master, MUST NOT improvise a local effect that imitates master output, and MUST NOT begin transmitting any NocturNation frames.

A receiver that detects master return (the first received frame after a NO SIGNAL state) MUST resume normal operation immediately.

### 6.3 No reverse heartbeat

NocturNation has no slave-to-master heartbeat. A master has no on-wire knowledge of which slaves are alive; the operator visually checks each slave's NO SIGNAL indicator.

---

## 7. Conformance

### 7.1 Receiver MUST honour

A conforming receiver MUST honour the following:

- The frame header layout and validation in [section 3.1](#31-header) and [section 3.2](#32-message-types).
- Deduplication on `(source_id, sequence_number)` against a ring of at least sixteen entries ([section 2.3](#23-redundancy)).
- The hop-count limit of 3 ([section 2.3](#23-redundancy)).
- The class-and-group routing rules in [section 4.2](#42-group-filtering).
- The `LIGHT_COMMAND` payload semantics: RGB triplet, attack/sustain/release envelope stages, chance gate.
- The protocol-version validation rule in [section 1.4](#14-versioning).

### 7.2 Receiver SHOULD honour

A conforming receiver SHOULD honour:

- The NO SIGNAL liveness behaviour in [section 6.2](#62-receiver-liveness-check).
- Channel auto-scan if it offers the capability ([section 5.3](#53-slave-auto-scan-mode)).
- The `MUSIC_EVENT` payload (DROP / BREAKDOWN / BUILD) if it has any locally interpretable behaviour for it.

### 7.3 Receiver MAY honour

A conforming receiver MAY:

- Operate as a repeater ([section 2.4](#24-repeater-behaviour)).
- Honour `BEAT_DETECTED`, `MODE_CHANGE`, `CLOCK_SYNC`, `TIME_SYNC` if it has locally interpretable behaviour.
- Implement screen-class rendering for `target_class == 0x02` frames.

### 7.4 Receiver MUST NOT

A conforming receiver MUST NOT:

- Auto-promote to master on master loss ([section 6.2](#62-receiver-liveness-check)).
- Transmit any NocturNation frame other than to forward an accepted frame as a repeater ([section 2.4](#24-repeater-behaviour)) or to render the local infra-red representation of a `LIGHT_COMMAND` ([annex A](#annex-a-pixmob-infra-red-encoding) for the PixMob case).
- Process frames whose `protocol_version` does not match a recognised version.

### 7.5 Master MUST honour

A conforming master MUST honour:

- Three-times redundant transmission with identical sequence numbers ([section 2.3](#23-redundancy)).
- The heartbeat rule and skip-if-recent suppression ([section 6.1](#61-master-heartbeat)).
- The protocol-version byte at offset 0 of every frame.
- Channel fixity for the duration of a deployment ([section 5.1](#51-master)).

---

## Annex A: PixMob infra-red encoding

This annex describes the on-wire infra-red encoding for PixMob X4 Gen 3.1 bracelets, derived from upstream reverse-engineering work by [James Wilson (jamesw343)](https://github.com/jamesw343/PixMob_IR). The NocturNation firmware parity-tests every byte of every transmitted infra-red frame against a Python reference encoder maintained against the upstream; conformance to that upstream is the load-bearing invariant.

A receiver implementing this annex interoperates with PixMob bracelets manufactured in the X4 Gen 3.1 generation. Earlier and later generations MAY use compatible encodings but have not been verified by this project.

### A.1 Frame format

The single-colour PixMob infra-red frame is **nine bytes**:

| Offset | Field | Description |
|---:|---|---|
| 0 | `sync_byte` | Always `0x80` |
| 1 | `checksum` | `ENCODING_MAP[(checksum_sum >> 2) & 0x3F]`; see [A.2](#a2-checksum) |
| 2 | `type_and_on_start` | `(type << 1) \| on_start`; single-colour fire uses `type == 0` and `on_start == 0`, giving `0x00` |
| 3 | `g_val_6bit` | `(g & 0xFF) >> 2`; high six bits of the eight-bit green channel |
| 4 | `r_val_6bit` | `(r & 0xFF) >> 2`; high six bits of red |
| 5 | `b_val_6bit` | `(b & 0xFF) >> 2`; high six bits of blue |
| 6 | `attack_and_chance` | `(attack << 3) \| (chance & 0x07)` |
| 7 | `release_and_sustain` | `(release << 3) \| (sustain & 0x07)` |
| 8 | `restrict_group_id` | `restrict_group_id & 0x1F`; 0 = broadcast, 1..31 = group filter |

The bracelet inspects byte 8 to decide whether to render. If `restrict_group_id == 0`, the bracelet renders unconditionally. If `restrict_group_id != 0`, the bracelet renders only when its own factory-assigned group matches.

Each byte is then mapped through the `ENCODING_MAP` lookup table (defined upstream in `jamesw343/PixMob_IR`) before being transmitted as an infra-red pulse train at the bracelet's expected carrier frequency. See the upstream repository for the transmit-side encoding pulse timing.

### A.2 Checksum

The checksum at offset 1 is computed as follows. Let `S` be the unsigned eight-bit sum of bytes at offsets 2 through 8 (seven bytes). The checksum byte is:

```
checksum = ENCODING_MAP[(S >> 2) & 0x3F]
```

A receiver MUST verify this checksum and discard frames whose computed value does not match. Note that the high two bits of the sum are deliberately discarded; the protocol does not require a strong integrity check, only enough to reject obvious bit-flips.

### A.3 `Time` and `Chance` enumerations

The three-bit `attack`, `sustain`, and `release` fields each index a `Time` enumeration:

| Index | Symbolic name | Duration |
|---:|---|---:|
| 0 | `T_0_MS` | 0 ms |
| 1 | `T_32_MS` | 32 ms |
| 2 | `T_96_MS` | 96 ms |
| 3 | `T_192_MS` | 192 ms |
| 4 | `T_480_MS` | 480 ms |
| 5 | `T_960_MS` | 960 ms |
| 6 | `T_2400_MS` | 2400 ms |
| 7 | `T_3840_MS` | 3840 ms |

The three-bit `chance` field indexes a `Chance` enumeration. Each value is a probability that an individual bracelet rolls **for itself** when the command arrives; the dice are independent across bracelets:

| Index | Symbolic name | Probability |
|---:|---|---:|
| 0 | `CHANCE_100` | 100% |
| 1 | `CHANCE_88` | 88% |
| 2 | `CHANCE_67` | 67% |
| 3 | `CHANCE_50` | 50% |
| 4 | `CHANCE_32` | 32% |
| 5 | `CHANCE_16` | 16% |
| 6 | `CHANCE_10` | 10% |
| 7 | `CHANCE_4` | 4% |

The envelope semantics on the bracelet are: ramp up to peak brightness over `attack` milliseconds; hold at peak for `sustain` milliseconds; ramp down to zero over `release` milliseconds. Total visible duration is the sum.

### A.4 Reference encoder

The authoritative encoder for this annex is [`jamesw343/PixMob_IR`](https://github.com/jamesw343/PixMob_IR). NocturNation's `test_pixmob_parity` test suite (`test/test_pixmob_parity/`) regenerates canonical byte sequences from the Python upstream and compares against the firmware's C++ encoder. Any disagreement is a defect in the C++ encoder, not in the upstream.

When generating new reference vectors for a protocol-level test, an implementer SHOULD run the upstream Python encoder and capture its byte output; do not bootstrap test fixtures from the C++ side.

---

## Annex B: Non-volatile-storage schema

This annex documents the keys that the reference firmware persists in non-volatile storage on the ESP32. It is informative; a third-party implementation MAY use any persistence layout it likes, provided the wire-visible behaviour conforms to the rest of this manual.

### B.1 Namespace

All keys live in a single namespace named `noct`. The reference firmware uses Espressif's `nvs_flash` library.

### B.2 Keys

| Key | Type | Default | Range | Purpose |
|---|---|---|---|---|
| `last_mode` | `u8` | `2` (AutonomousMaster) | 0..5 (`ModeId`) | Runtime mode to resume at boot |
| `ir_en` | `bool` | `true` | - | IR transmitter enabled |
| `scr_puls_en` | `bool` | `true` | - | Local LCD pulse animation enabled |
| `mst_chan` | `u8` | `1` | {1, 6, 11} | Master Wi-Fi channel |
| `slv_chan` | `u8` | `0` (auto) | {0, 1, 6, 11} | Slave Wi-Fi channel; 0 = auto-scan |
| `slv_repeat` | `bool` | `false` | - | Slave operates as repeater |
| `slv_group` | `u8` | `0` (broadcast) | 0..255 | Slave receive-filter group |
| `active_show` | `string` | `"simple-beat"` | up to 16 bytes | Currently selected Show plug-in id |

### B.3 Per-plug-in namespaces

Show plug-ins, output bindings, and visualisations (legacy) each receive a sub-namespace for their own properties. The convention is:

- `ns_<id>` for Show plug-ins (e.g. `ns_dynamic` for the Dynamic show's `groups` property).
- `nb_<id>` for OutputBinding plug-ins.
- `nv_<id>` for Visualisation plug-ins (legacy; deprecated as of Epic 4.7).

The plug-in id MUST be twelve bytes or fewer; longer ids are truncated.

### B.4 Migrations

The reference firmware applies one-shot migrations on first boot after a firmware upgrade. As of v0.5 the migrations are:

- Drop the legacy `slv_ir_grp` key (the function moved to the slave's `slv_group` filter and per-binding namespaces).
- Map the legacy `active_vis` value `"beat-pulse"` or `"spectrum-bars"` to `active_show = "simple-beat"`.

Migrations MUST be idempotent.

---

## Annex C: Reference test vectors

This annex provides canonical byte sequences for parity testing against the reference firmware. Implementers building a NocturNation transmitter or receiver SHOULD verify against these vectors before deploying.

### C.1 ESP-NOW `LIGHT_COMMAND` frame

A `LIGHT_COMMAND` from source_id 1, sequence 42, broadcast (`target_class = 0x00`, `target_group = 0x00`), red `(255, 0, 0)`, envelope (attack=`T_96_MS`, sustain=`T_0_MS`, release=`T_480_MS`), chance `CHANCE_100`:

```
Offset  Byte    Field
0x00    0x01    protocol_version
0x01    0x01    source_id
0x02    0x2A    sequence_number (42)
0x03    0x00    hop_count
0x04    0x03    message_type (LIGHT_COMMAND)
0x05    0x09    payload_len
0x06    0x00    target_class (All)
0x07    0x00    target_group (broadcast)
0x08    0xFF    r
0x09    0x00    g
0x0A    0x00    b
0x0B    0x02    attack (T_96_MS)
0x0C    0x00    sustain (T_0_MS)
0x0D    0x04    release (T_480_MS)
0x0E    0x00    chance (CHANCE_100)
```

Total frame length: fifteen bytes.

### C.2 ESP-NOW `HEARTBEAT` frame

A `HEARTBEAT` from source_id 1, sequence 43:

```
Offset  Byte    Field
0x00    0x01    protocol_version
0x01    0x01    source_id
0x02    0x2B    sequence_number (43)
0x03    0x00    hop_count
0x04    0x00    message_type (HEARTBEAT)
0x05    0x00    payload_len
```

Total frame length: six bytes. No payload.

### C.3 ESP-NOW `MUSIC_EVENT` frame (DROP)

A `MUSIC_EVENT` carrying DROP from source_id 1, sequence 44:

```
Offset  Byte    Field
0x00    0x01    protocol_version
0x01    0x01    source_id
0x02    0x2C    sequence_number (44)
0x03    0x00    hop_count
0x04    0x06    message_type (MUSIC_EVENT)
0x05    0x01    payload_len
0x06    0x01    event_type (DROP)
```

Total frame length: seven bytes.

### C.4 PixMob infra-red frame

Single-colour red `(255, 0, 0)` to group 0 (broadcast), envelope (attack=`T_96_MS`, sustain=`T_0_MS`, release=`T_480_MS`), chance `CHANCE_100`:

Pre-encoding byte sequence (before `ENCODING_MAP` lookup):

```
Offset  Byte    Field
0x00    0x80    sync_byte
0x01    [chk]   checksum (computed; see A.2)
0x02    0x00    type_and_on_start (single colour, on_start=0)
0x03    0x00    g_val_6bit (0 >> 2)
0x04    0x3F    r_val_6bit (255 >> 2 = 63)
0x05    0x00    b_val_6bit (0 >> 2)
0x06    0x10    attack_and_chance (attack=2 << 3 | chance=0 = 0x10)
0x07    0x20    release_and_sustain (release=4 << 3 | sustain=0 = 0x20)
0x08    0x00    restrict_group_id (broadcast)
```

The checksum at offset 1 is the sum `0x00 + 0x00 + 0x3F + 0x00 + 0x10 + 0x20 + 0x00 = 0x6F`, then `(0x6F >> 2) & 0x3F = 0x1B`, then `ENCODING_MAP[0x1B]` (consult upstream `jamesw343/PixMob_IR` for the encoding map).

### C.5 Authoritative source

These hand-derived vectors are illustrative. The authoritative reference vectors are generated by running `python3 tools/pixmob_reference_encoder.py` (against the upstream Python encoder) and recorded in `test/test_pixmob_parity/`. Implementers MUST verify against the in-tree test fixtures rather than the inline vectors above.

---

## Annex D: Protocol version history

| Version | Date | Spec doc | Notable changes |
|---:|---|---|---|
| 0x01 | 2026 | This document | Initial public protocol. ESP-NOW transport, 6-byte header, 7 message types, class-and-group addressing, PixMob IR annex. |

Future revisions will be appended to this table.

---

## References

Bradner, S. (1997) *Key words for use in RFCs to indicate requirement levels*. RFC 2119, IETF. Available at: <https://www.rfc-editor.org/rfc/rfc2119> (Accessed: 12 May 2026).

Espressif Systems (no date) *ESP-NOW protocol reference*. Available at: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_now.html> (Accessed: 12 May 2026).

Wilson, J. (no date) *PixMob_IR: reverse-engineered PixMob infra-red encoder*. GitHub. Available at: <https://github.com/jamesw343/PixMob_IR> (Accessed: 12 May 2026).
