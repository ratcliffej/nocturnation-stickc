---
title: "NocturNation flow diagrams"
status: Draft
notion_url: https://www.notion.so/35ebd0677405807cb34cccefa936d4d9
notion_id: 35ebd0677405807cb34cccefa936d4d9
last_synced: 2026-05-17
sync_direction: bidirectional
---

# NocturNation flow diagrams

Reference diagrams for the reference firmware (v0.5, StickC Plus2 + StickS3). Read alongside the [user manual](user-manual.md) for theory of operation context, or alongside the [protocol manual](protocol-manual.md) for byte-level specifications.

Diagrams use [Mermaid](https://mermaid.js.org/) notation. GitHub, Notion, and most modern Markdown renderers display Mermaid blocks natively.

## Contents

1. [System topology](#1-system-topology) - Director, Lumes, bracelets, wires
2. [Boot flow](#2-boot-flow) - power-on through mode resume
3. [Mode finite-state-machine](#3-mode-finite-state-machine) - the runtime modes
4. [Director audio analyser pipeline](#4-director-audio-analyser-pipeline) - mic in, events out
5. [Director render dispatch](#5-director-render-dispatch) - render_fx fan-out
6. [Lume receive pipeline](#6-lume-receive-pipeline) - frame arrival through binding fire
7. [Class-and-group routing](#7-class-and-group-routing) - the addressing decision matrix
8. [Configuration menu tree](#8-configuration-menu-tree) - operator-reachable settings
9. [Channel discovery and re-scan](#9-channel-discovery-and-re-scan) - auto-scan + signal-loss recovery

---

## 1. System topology

The Director runs the active Show, which decides what to send. A Show may consume audio analyser events (microphone), DMX cues (Epic 7), both, or neither. Decisions go out over ESP-NOW to Lumes and locally over IR + LCD; Lumes act as IR range extenders; downstream IR receivers (bracelets) are passive.

```mermaid
flowchart LR
    Audio((Music<br/>speaker))
    DMX[(DMX<br/>controller)]

    subgraph Director["Director Stick (Director mode)"]
        MMic[Microphone]
        MAnalyser[Audio analyser]
        MDMX[DMX input<br/>Epic 7 stub]
        MShow[Active Show plug-in]
        MIR["IR transmitter<br/>loopback (if ir_en)"]
        MLCD[Local LCD<br/>pulse]
        MMic --> MAnalyser
        MAnalyser -. on_audio_frame<br/>on_beat_detected .-> MShow
        MDMX -. cue events .-> MShow
        MShow --> MIR
        MShow --> MLCD
    end

    subgraph Lume1["Lume Device A"]
        L1Recv[ESP-NOW receive]
        L1LED[Perimeter LEDs]
        L1IR[IR transmitter]
        L1Recv --> L1LED
        L1Recv --> L1IR
    end

    subgraph Lume2["Lume Device B"]
        L2Recv[ESP-NOW receive]
        L2LED[Perimeter LEDs]
        L2Recv --> L2LED
    end

    subgraph Lume3["Lume Tildagon"]
        L3Recv[ESP-NOW receive]
        L3LED[Perimeter LEDs]
        L3LCD[LCD]
        L3Recv --> L3LED
        L3Recv --> L3LCD
    end

    Receivers[(Bracelets /<br/>IR receivers)]

    Audio -. acoustic .-> MMic
    DMX -. DMX-512 .-> MDMX
    MShow -- "ESP-NOW LIGHT_COMMAND<br/>3× redundant TX" --> L1Recv
    MShow -- "ESP-NOW LIGHT_COMMAND<br/>3× redundant TX" --> L2Recv
    MShow -- "ESP-NOW LIGHT_COMMAND<br/>3× redundant TX" --> L3Recv

    MIR -- "IR (PixMob today,<br/>protocol-pluggable)" --> Receivers
    L1IR -- IR --> Receivers
```

Notes:
- The Director is treated as one of its own Lumes for output purposes (the "loopback"): every `render_fx` call also fires the Director's own IR transmitter and LCD pulse.
- IR transmission is gated by the `ir_en` config — a Show can run without IR at all (LCD + ESP-NOW only).
- The IR encoder is protocol-pluggable. PixMob is the reference implementation today; future Lumes can carry different IR encoders without a wire-protocol change.
- A Lume's outputs depend on its hardware. The three example Lumes span the range: Device A renders to its own perimeter LEDs **and** re-fires the command over IR to bracelets; Device B renders to its own perimeter LEDs and terminates there (no IR, no ESP-NOW out); the Lume Tildagon renders to its own perimeter LEDs and LCD but has no IR transmitter. The ESP-NOW frame arrives and renders locally in every case; what differs is what flows onward.
- Lumes are receive-only by default. The Lume-as-repeater toggle re-broadcasts accepted frames at hop_count + 1, capped at 3 hops, where the Lume's hardware and firmware support it. Device B in the diagram has no transmission at all and so cannot relay.
- Bracelets are passive: they wake on an IR command, render the envelope, then return to standby.

---

## 2. Boot flow

```mermaid
flowchart TD
    Power[Power on] --> Splash[Splash screen<br/>3 s breathing N]
    Splash --> Interrupt{Lower button<br/>pressed?}
    Interrupt -- "yes" --> Menu[Enter Menu mode]
    Interrupt -- "no" --> Migrate[migrate_legacy_nvs_keys<br/>+ first-boot slv_group<br/>random 1, 2 or 3]
    Migrate --> Load[Load last_mode from NVS<br/>default: Director]
    Load --> Enter[Enter saved mode]
```

The first-boot path picks a random group in {1, 2, 3} and persists it; subsequent boots see the key set and skip the random assignment. The operator can override the group via Config > Group at any time, including setting it to 0 (broadcast-only).

---

## 3. Mode finite-state-machine

```mermaid
stateDiagram-v2
    [*] --> Boot
    Boot --> Menu: lower-button interrupt
    Boot --> Director: default boot
    Boot --> Lume: last_mode resumes
    Boot --> Config: last_mode resumes
    Boot --> Test: last_mode resumes

    Menu --> Director
    Menu --> Lume
    Menu --> Config
    Menu --> Test

    Director --> Menu: long-press BtnA+BtnB
    Lume --> Menu: long-press BtnA+BtnB
    Config --> Menu: long-press BtnA+BtnB
    Test --> Menu: long-press BtnA+BtnB

    Director --> Config: long-press BtnA
    Lume --> Config: long-press BtnA
```

Mode IDs are stable across releases (see [protocol manual annex B](protocol-manual.md#annex-b-non-volatile-storage-schema)):
- `0` Boot, `1` Menu, `2` Director, `3` Lume, `4` Config, `5` Test.

---

## 4. Director audio analyser pipeline

```mermaid
flowchart TD
    Mic[Microphone<br/>PDM / I2S 16 kHz] --> Frame[Audio frame<br/>512 samples ~32 ms]
    Frame --> FFT[FFT<br/>32-band log spectrum<br/>30 Hz to Nyquist]
    Frame --> Bands[Band aggregation<br/>3-band B/M/T<br/>8-band perceptual]

    FFT --> BeatDet[BeatDetector<br/>watch bands 0-7 ~30-150 Hz<br/>threshold k=2.2<br/>refractory 200 ms]
    FFT --> DropDet[DropDetector<br/>2 s vs 10 s ratio<br/>drop > 1.8 / breakdown < 0.4<br/>cooldown 4 s]
    FFT --> Desc[MusicDescriptors<br/>spectral centroid<br/>broadband energy<br/>density]

    BeatDet -- on_beat_detected --> Show
    Desc -- on_music_descriptor --> Show
    DropDet -. "drop / breakdown<br/>Director-internal only" .-> Show

    Show[Active Show plug-in<br/>SimpleBeat / Dynamic]
    Show -- render_fx --> Dispatch[dispatch_output_class_group]
    Dispatch --> ESPNow[ESP-NOW broadcast]
    Dispatch -. IR loopback .-> DirectorIR
    Dispatch -. screen loopback .-> DirectorLCD
```

The analyser primitives are all Director-internal events. None of them are broadcast on the wire under spec v0.29 — the only Director-emitted frame types are `HEARTBEAT` (1 Hz, skip-if-recent) and `LIGHT_COMMAND` (per Show render). The DropDetector still runs and stamps its output onto `AudioFrameEvent::music_event` (`0` = none, `1` = DROP, `2` = BREAKDOWN, `3` = BUILD reserved). The field is **available as part of the Show toolset** — any Show that wants DROP- or BREAKDOWN-responsive behaviour can read it on `on_audio_frame` and compose a richer `LIGHT_COMMAND` accordingly. No current Show consumes it, but the wiring is intentional. The pre-v0.29 `MUSIC_EVENT` (0x06) standalone-wire broadcast was removed in the protocol trim, but the *Director-internal signal* it once carried is still here, just consumed differently.

---

## 5. Director render dispatch

One `render_fx` call fans out to three sinks: ESP-NOW broadcast, Director IR loopback, Director LCD pulse. Exactly one IR frame is sent per dispatch call. (An Epic 4.7 build inserted a zero-RGB "primer" frame ahead of the main IR fire on idle gaps; bench testing in Epic 4.8 found the extra traffic overloaded the bracelet receivers, so the primer was removed.)

```mermaid
flowchart TD
    Call["render_fx(target_class:target_group, ev)"]
    Call --> Dispatch[dispatch_output_class_group]

    Dispatch --> ESPNow[ESP-NOW broadcast<br/>always; 3× redundant TX]
    Dispatch --> IRGate{target_class == 0 All<br/>or 1 Light?}
    Dispatch --> ScrGate{target_class == 0 All<br/>or 2 Screen?}

    IRGate -- "yes" --> SendMain[Send IR frame<br/>via configured IR driver]
    IRGate -- "no" --> IRSkip[Skip IR]

    ScrGate -- "yes" --> LCDPulse[Pulse local LCD]
    ScrGate -- "no" --> ScrSkip[Skip screen]
```

---

## 6. Lume receive pipeline

Every ESP-NOW frame arriving on the channel goes through these checks in order. The magic-prefix check fires first as the cheapest reject for non-NocturNation ESP-NOW traffic sharing the band. Class-and-group routing is unpacked separately in section 7.

```mermaid
flowchart TD
    Recv[ESP-NOW frame arrives]
    Recv --> Magic{magic[0..1]<br/>== 0x4E 0x4E?}
    Magic -- "no" --> DropM[Drop silently<br/>foreign vendor traffic]
    Magic -- "yes" --> Ver{protocol_version<br/>== 0x02?}
    Ver -- "no" --> DropV[Drop silently<br/>wrong NocturNation version]
    Ver -- "yes" --> Dedup{seq in 16-deep<br/>dedup ring?}
    Dedup -- "yes" --> DropD[Drop silently]
    Dedup -- "no" --> Hop{hop_count > 3?}
    Hop -- "yes" --> DropH[Drop silently]
    Hop -- "no" --> Live[Update NO-SIGNAL timer]
    Live --> Type{message_type?}

    Type -- "0x00 HEARTBEAT" --> NoOp[No further action<br/>liveness already updated]
    Type -- "0x03 LIGHT_COMMAND" --> ForEach[For each registered<br/>OutputBinding]
    Type -- "other / 0xFF / reserved" --> DropU[Drop silently<br/>forward-compatible]

    ForEach --> Route[See class-and-group<br/>routing diagram]

    Repeater{Lume-as-repeater<br/>enabled?}
    Hop -- "after dedup OK" --> Repeater
    Repeater -- "yes" --> ReTX[Rebroadcast at<br/>hop_count + 1]
    Repeater -- "no" --> End[Done]
```

---

## 7. Class-and-group routing

The decision applied per binding when a `LIGHT_COMMAND` arrives. The rule is documented normatively in [protocol manual §4.2](protocol-manual.md#42-group-filtering).

```mermaid
flowchart TD
    Start[LIGHT_COMMAND<br/>target_class, target_group<br/>arrives at binding] --> ClassF{target_class == 0 All<br/>or target_class<br/>== binding.class?}
    ClassF -- "no" --> Skip1[Skip this binding]
    ClassF -- "yes" --> Relay{binding.is_relay?}

    Relay -- "yes (IR relay)" --> RelayFire["Fire binding<br/>(target_group passed through<br/>as downstream IR group code)"]
    Relay -- "no (Local Display)" --> GroupF{target_group == 0 broadcast<br/>or<br/>target_group == slv_group?}

    GroupF -- "no" --> Skip2[Skip this binding]
    GroupF -- "yes" --> LocalFire[Fire binding]
```

Worth re-stating in prose because it catches people out:

- **Broadcast** is sender-side. `target_group == 0` means "address every receiver in this class, regardless of the receiver's own group". The receiver MUST honour it.
- **Group 0 receiver** is *not* a receive-side wildcard. A device with `slv_group == 0` is in no specific group and only renders broadcasts.
- **Relay bindings** (IR relay) bypass the receive-side group filter because the downstream protocol does its own group filtering at the receiver. The downstream group field is set from the inbound NocturNation `target_group` (with PixMob, that's the five-bit group code; other IR encoders carry an equivalent field).

---

## 8. Configuration menu tree

The full operator-reachable settings tree. Reached by long-pressing the lower button from any mode.

```mermaid
flowchart LR
    Top[Top level<br/>Config tree]
    Top --> G["Group: N<br/>direct increment<br/>NVS: slv_group"]
    Top --> SH[Show<br/>picker over<br/>registered Shows]
    Top --> D[Display]
    Top --> C[Connectivity<br/>picker]
    Top --> U[Utilities<br/>picker]
    Top --> SY[System]

    D --> DP["Pulse Enable<br/>(toggle, NVS: scr_puls_en)"]

    C --> CI[IR]
    C --> CE[ESP-NOW]
    C --> CW[WiFi: stub for future]
    C --> CD[DMX: stub for Epic 7]

    CI --> CIE["Enable / Disable<br/>(toggle, NVS: ir_en)"]
    CI --> CIP[Protocol: PixMob only]

    CE --> CEM["Director Channel<br/>{1, 6, 11}<br/>NVS: mst_chan"]
    CE --> CES["Lume Channel<br/>{0 auto, 1, 6, 11}<br/>NVS: slv_chan"]
    CE --> CER["Lume Repeat<br/>(toggle, NVS: slv_repeat)"]

    U --> UP[PixMob]
    U --> UL[Level Tuning]

    UP --> UPS[Set Group ID<br/>via IR Rx on S3]
    UP --> UPG[Group Target Test<br/>fire to one group]

    UL --> ULL[Live<br/>real-time bars]
    UL --> UL25[25 %]
    UL --> UL50[50 %]
    UL --> UL75[75 %]
    UL --> UL100[100 %]

    SY --> SYB[Battery readout]
    SY --> SYF[Firmware Version]
    SY --> SYR["Factory Reset<br/>(clears 'noct' NVS namespace)"]
```

Top-level cycle order is `Group → Show → Display → Connectivity → Utilities → System`. Pickers (Connectivity, Utilities) lead to second-level pickers; sub-menus (Display, System) lead directly to leaf items. Long-press BtnB pops one level.

---

## 9. Channel discovery and re-scan

Two related flows: how a fresh Lume finds the Director's channel from cold (auto-scan), and how a locked Lume decides whether to abandon a channel after signal loss (re-scan).

### 9.1 Auto-scan from cold

```mermaid
flowchart TD
    Boot[Lume boots<br/>slv_chan loaded from NVS]
    Boot --> ChanCheck{slv_chan == 0?}
    ChanCheck -- "no (1, 6, or 11)" --> Lock["Lock to slv_chan<br/>(operator chose this channel)"]
    ChanCheck -- "yes (auto-scan)" --> Scan11[Set channel 11<br/>listen 2 s]

    Scan11 --> Got11{frame received?}
    Got11 -- "yes" --> Lock11[Lock to channel 11]
    Got11 -- "no" --> Scan1[Set channel 1<br/>listen 2 s]

    Scan1 --> Got1{frame received?}
    Got1 -- "yes" --> Lock1[Lock to channel 1]
    Got1 -- "no" --> Scan6[Set channel 6<br/>listen 2 s]

    Scan6 --> Got6{frame received?}
    Got6 -- "yes" --> Lock6[Lock to channel 6]
    Got6 -- "no" --> Scan11

    Lock --> Receive[Main receive loop]
    Lock11 --> Receive
    Lock1 --> Receive
    Lock6 --> Receive
```

Channel 11 (show) → 1 (hobby) → 6 (advanced override) → repeat. Worst-case discovery latency from cold is 6 seconds. The order is priority-by-likelihood: channel 11 is most likely to carry a show, channel 6 is least likely.

### 9.2 Re-scan on signal loss

```mermaid
flowchart TD
    Receive[Main receive loop<br/>frame just arrived]
    Receive --> Tick[Tick: check age_since_rx]
    Tick --> NoSig{age > 3 s<br/>kNoSignalMs?}
    NoSig -- "no" --> Receive
    NoSig -- "yes" --> Display[Display NO SIGNAL<br/>on LCD]
    Display --> Rescan{age > 10 s<br/>kRescanMs?}
    Rescan -- "no" --> Tick
    Rescan -- "yes" --> Mode{slv_chan == 0<br/>originally?}
    Mode -- "no (operator-locked)" --> StayPut[Stay on channel<br/>NO SIGNAL keeps showing<br/>operator must intervene]
    Mode -- "yes (auto-mode)" --> Resume[Resume auto-scan<br/>see §9.1]
    StayPut --> Tick
    Resume --> Receive
```

Two thresholds, deliberately decoupled:
- `kNoSignalMs = 3 s` — display NO SIGNAL for operator awareness. Fires fast because operators want to know quickly that something's wrong.
- `kRescanMs = 10 s` — give up on the current channel and start hunting. Fires slow because most signal losses are transient (Director reboot, brief congestion, line of sight blocked). A ~7-second window of "stay on this channel" usually catches recovery faster than a full multi-channel rescan would.

**Operator-locked Lumes never re-scan.** If the operator set `slv_chan ∈ {1, 6, 11}`, the Lume respects that choice indefinitely — NO SIGNAL still displays so the operator notices the outage, but no channel change follows. Only auto-mode Lumes (`slv_chan == 0`) ever re-enter the scan loop.

---

## Maintaining these diagrams

These diagrams are hand-derived from the firmware code. When the firmware changes any of these flows, update the corresponding diagram in this file. The diagrams are deliberately schematic - they exist to support a reader's mental model, not to be a substitute for reading the code. Specific anchors:

- [src/dal/dal.cpp](../../src/dal/dal.cpp) - dispatch fan-out (section 5).
- [src/transport/espnow/frame.cpp](../../src/transport/espnow/frame.cpp) - magic + version + dedup gate at the very top of receive (section 6).
- [src/modes/lume_mode.cpp](../../src/modes/lume_mode.cpp) - receive pipeline, class-and-group routing, channel scan + re-scan (sections 6, 7, 9).
- [src/modes/mode_machine.cpp](../../src/modes/mode_machine.cpp) - mode finite-state-machine (section 3).
- [src/modes/config_mode.cpp](../../src/modes/config_mode.cpp) - configuration tree (section 8).
- [src/modes/persistence.cpp](../../src/modes/persistence.cpp) - first-boot slv_group assignment (section 2).
- [src/dal/analyser/](../../src/dal/analyser/) - audio analyser primitives (section 4).
