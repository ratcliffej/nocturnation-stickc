---
title: "NocturNation flow diagrams"
status: Draft
notion_url: https://www.notion.so/35ebd0677405807cb34cccefa936d4d9
notion_id: 35ebd0677405807cb34cccefa936d4d9
last_synced: 2026-05-12
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

---

## 1. System topology

The system has three device tiers and two wireless links. The Director listens to audio and broadcasts decisions; Lumes act as IR range extenders; bracelets are passive receivers worn by the audience.

```mermaid
flowchart LR
    Audio((Music<br/>speaker))

    subgraph Director["Director Stick (Director mode)"]
        MMic[Microphone]
        MAnalyser[Audio analyser<br/>+ Show plug-in]
        MIR[IR transmitter<br/>loopback]
        MLCD[Local LCD<br/>pulse]
        MMic --> MAnalyser
        MAnalyser --> MIR
        MAnalyser --> MLCD
    end

    subgraph Slave1["Lume Stick A"]
        S1Recv[ESP-NOW receive]
        S1IR[IR transmitter]
        S1Recv --> S1IR
    end

    subgraph Slave2["Lume Stick B"]
        S2Recv[ESP-NOW receive]
        S2IR[IR transmitter]
        S2Recv --> S2IR
    end

    Bracelets[(PixMob X4<br/>bracelets)]

    Audio -. acoustic .-> MMic
    MAnalyser -- "ESP-NOW LIGHT_COMMAND<br/>3× redundant TX" --> S1Recv
    MAnalyser -- "ESP-NOW LIGHT_COMMAND<br/>3× redundant TX" --> S2Recv

    MIR -- "PixMob IR<br/>(omni / focused)" --> Bracelets
    S1IR -- "PixMob IR" --> Bracelets
    S2IR -- "PixMob IR" --> Bracelets
```

Notes:
- The Director is treated as one of its own Lumes for output purposes (the "loopback"): every `render_fx` call also fires the Director's own IR transmitter and LCD pulse.
- Lumes are receive-only by default. The Lume-as-repeater toggle re-broadcasts accepted frames at hop_count + 1, capped at 3 hops.
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
    Dispatch -. IR loopback .-> MasterIR
    Dispatch -. screen loopback .-> MasterLCD
```

The analyser primitives are all Director-internal events. None of them are broadcast on the wire under spec v0.29 — the only Director-emitted frame types are `HEARTBEAT` (1 Hz, skip-if-recent) and `LIGHT_COMMAND` (per Show render). The DropDetector still runs internally (it stamps `AudioFrameEvent::music_event`), but its output has no consumer in the v0.29 reference firmware; the pre-v0.29 `MUSIC_EVENT` (0x06) broadcast was removed in the protocol trim along with the DROP / BREAKDOWN effect rendering. The Show plug-in consumes the events it cares about, computes colour and envelope, and dispatches `LIGHT_COMMAND` frames.

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

    IRGate -- "yes" --> SendMain[Send IR frame<br/>via PixMob driver]
    IRGate -- "no" --> IRSkip[Skip IR]

    ScrGate -- "yes" --> LCDPulse[Pulse local LCD]
    ScrGate -- "no" --> ScrSkip[Skip screen]
```

---

## 6. Lume receive pipeline

Every ESP-NOW frame goes through these steps. The class-and-group routing is unpacked separately in section 7.

```mermaid
flowchart TD
    Recv[ESP-NOW frame arrives]
    Recv --> Ver{protocol_version<br/>== 0x01?}
    Ver -- "no" --> DropV[Drop silently]
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

    Relay -- "yes (PixMob IR)" --> RelayFire["Fire binding<br/>(target_group passed through<br/>as PixMob protocol group code)"]
    Relay -- "no (Local Display)" --> GroupF{target_group == 0 broadcast<br/>or<br/>target_group == slv_group?}

    GroupF -- "no" --> Skip2[Skip this binding]
    GroupF -- "yes" --> LocalFire[Fire binding]
```

Worth re-stating in prose because it catches people out:

- **Broadcast** is sender-side. `target_group == 0` means "address every receiver in this class, regardless of the receiver's own group". The receiver MUST honour it.
- **Group 0 receiver** is *not* a receive-side wildcard. A device with `slv_group == 0` is in no specific group and only renders broadcasts.
- **Relay bindings** (PixMob IR) bypass the receive-side group filter because the downstream protocol (PixMob IR) does its own group filtering at the bracelet. The PixMob's five-bit group field is set from the inbound NocturNation `target_group`.

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

## Maintaining these diagrams

These diagrams are hand-derived from the firmware code. When the firmware changes any of these flows, update the corresponding diagram in this file. The diagrams are deliberately schematic - they exist to support a reader's mental model, not to be a substitute for reading the code. Specific anchors:

- [src/dal/dal.cpp](../../src/dal/dal.cpp) - dispatch fan-out (section 5).
- [src/modes/lume_mode.cpp](../../src/modes/lume_mode.cpp) - receive pipeline and class-and-group routing (sections 6 and 7).
- [src/modes/mode_machine.cpp](../../src/modes/mode_machine.cpp) - mode finite-state-machine (section 3).
- [src/modes/config_mode.cpp](../../src/modes/config_mode.cpp) - configuration tree (section 8).
- [src/modes/persistence.cpp](../../src/modes/persistence.cpp) - first-boot slv_group assignment (section 2).
- [src/dal/analyser/](../../src/dal/analyser/) - audio analyser primitives (section 4).
