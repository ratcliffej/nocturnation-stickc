# NocturNation

> Open-source crowd lighting, conjured from cheap silicon.

NocturNation is a modular open-source crowd-lighting system: one Director Stick listens to music, detects beats and structural events, and broadcasts light commands to a swarm of Lume Sticks that fire infra-red at PixMob bracelets worn by the audience. A full deployment costs roughly the price of a meal out, and gives smaller bands and art installations the same kind of platform that proprietary stadium fan-lighting systems sell to touring acts at five-figure prices.

This repository is the reference firmware. It runs on the M5StickC Plus2 and the M5StickS3 (the "Sticks"). Both Sticks share a single firmware codebase with hardware-specific abstraction underneath.

---

## Documentation

Full documentation lives in the [**nocturnation-docs**](https://github.com/ratcliffej/nocturnation-docs) repository (Notion is the master copy; that repo is the public mirror).

| Document | Audience |
|---|---|
| [User manual](https://github.com/ratcliffej/nocturnation-docs/blob/main/manuals/user-manual.md) | Operators setting up a venue. Theory of operation, hardware, firmware install, configuration walk-through, modes and shows, troubleshooting, glossary. |
| [QLC+ beginner's guide](https://github.com/ratcliffej/nocturnation-docs/blob/main/qlc-plus-beginners-guide.md) | Operators driving NocturNation live from a DMX console. From-zero walkthrough: install QLC+, plug a StickC in, programme scenes and chasers. |
| [Music orchestrator guide](https://github.com/ratcliffej/nocturnation-docs/blob/main/orchestrator-guide.md) | Operators running a programmed show synchronised to music. Watches the OS now-playing source, walks per-track `.cues` files, drives the same StickC over USB or Art-Net. |
| [FX library](https://github.com/ratcliffej/nocturnation-docs/blob/main/fx-library.md) | Reference for every FX the orchestrator can fire: parameters, units, defaults. Generated from the FX classes. |
| [Protocol manual](https://github.com/ratcliffej/nocturnation-docs/blob/main/manuals/protocol-manual.md) | Implementers building a third-party transmitter or receiver. Wireless layer, frame formats, class-and-group addressing, PixMob IR annex, conformance, test vectors. |
| [Developer guide](https://github.com/ratcliffej/nocturnation-docs/blob/main/developing-shows.md) | Contributors writing new `Show` plug-ins. The `Show` base class, analyser hooks, `render_fx` API, widget composition, persistence, testing. |
| [Architecture spec](https://github.com/ratcliffej/nocturnation-docs/blob/main/architecture.md) | The full system design that the manuals distil. |

---

## Hardware

| Item | Notes |
|---|---|
| **M5StickC Plus2** or **M5StickS3** | The Stick. Either can run as Director, Lume, or both. The S3 is the current first-class reference; the Plus2 (now end-of-life from M5Stack) remains fully supported. See the [hardware section of the user manual](https://github.com/ratcliffej/nocturnation-docs/blob/main/manuals/user-manual.md#2-hardware) for the comparison. |
| **PixMob Aurora bracelets** | The reference target. Distributed at Coldplay's *Music of the Spheres* tour (2022-2024) and widely available second-hand. Other PixMob product lines are partially compatible but have not been bench-tested. |
| **USB-C cable** | For flashing the Sticks. |
| **A speaker playing music** | Anything with a clear kick drum. The reference test track is Vengaboys, *We Like to Party* (138 BPM). |
| **Optional: M5Stack IR Transmitter unit** | Plus2 only. Plugged onto the **GPIO 26** header pin, it acts as a second IR emitter alongside the built-in LED, roughly doubling coverage. Toggled from `Config > IR > External`. Not supported on the S3 (header pinout differs and the unit drives a strapping pin → boot mode). |

A useful first deployment is one Director plus one Lume, in a small room with a handful of bracelets. For larger venues see the [IR radiation-pattern guidance in the user manual](https://github.com/ratcliffej/nocturnation-docs/blob/main/manuals/user-manual.md#23-ir-radiation-patterns).

---

## Quick start

```bash
git clone https://github.com/ratcliffej/nocturnation-stickc.git
cd nocturnation-stickc
pio run -e m5stack-stickcs3 -t upload
```

Substitute `m5stack-stickcplus2` for a Plus2. Open the folder in VS Code with the [PlatformIO IDE extension](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide) for the integrated build/upload flow.

> **If `pio` is "command not found"**, you installed PlatformIO via the VS Code extension rather than as a standalone tool - it bundles its own CLI rather than putting `pio` on your `PATH`. Either invoke it by full path (`~/.platformio/penv/bin/pio run …` on macOS) or use the extension's build/upload buttons instead of the terminal.

> **First flash of a fresh Stick** may need the lower side button held down while you plug in the USB cable, to enter the ROM bootloader; later flashes reset into the bootloader automatically.

## First light - see it fire IR

You can confirm a **single** freshly-flashed Stick is firing IR - no second device or bracelet required. In Director mode a Stick loops its own light commands back through its IR transmitter, so it lights nearby bracelets *and* gives you something to verify.

1. **Power on.** After the boot splash the Stick lands in **Director** mode by default.
2. **Play music with a clear kick** near the Stick's microphone. The reference test track is Vengaboys, *We Like to Party* (138 BPM). On each detected beat the Stick's screen pulses - that is the render path firing.
3. **Confirm the IR.** Infra-red is invisible to the eye, so point a phone camera at the Stick: on each beat the IR LED shows up as a faint purple-white flicker on screen. Most **front (selfie) cameras** see IR well; many rear cameras filter it out, so switch to the selfie camera if you see nothing. (Same trick as checking a TV remote.)
4. **Optional - light a bracelet.** A PixMob Aurora held in front of the Stick wakes and lights on each beat.

Buttons: **Button 1** (the lower button) cycles between the **Simple Beat** and **Dynamic** shows; long-press **Button 2** (upper) opens the show picker. For the full button/mode reference, adding a second Lume, and venue setup, read the [user manual quickstart](https://github.com/ratcliffej/nocturnation-docs/blob/main/manuals/user-manual.md#quickstart).

---

## Architecture at a glance

The firmware has a six-layer plug-in architecture:

1. **HAL** - hardware abstraction (mic, IR, display, buttons, BLE, ESP-NOW). Plus2 and S3 backends.
2. **DAL** - device-abstraction layer. Holds the audio analyser core (BeatDetector, DropDetector, music descriptors), event bus, and the canonical `render_fx` dispatch.
3. **Plug-ins** - `Plugin` base class with property bags and per-plug-in NVS namespaces.
4. **Analyser** - sits on the DAL's mic pipeline; produces beat, drop, and music-descriptor events that the Director's Show consumes.
5. **Shows** - operator-selectable performances. Currently `SimpleBeatShow` (a faithful beat-pulse) and `DynamicShow` (FFT-driven HSV with per-drum group routing).
6. **OutputBindings** - Lume-side render targets. Currently `LocalDisplayBinding` (LCD pulse) and `PixMobIrBinding` (infra-red wire encoder, a pure relay).

Every render call flows through `render_fx("<class>:<group>", ev)` with structured class+group targets. The Director's dispatch fans every call out to ESP-NOW broadcast, the Director's own infra-red transmitter, and the Director's screen pulse - so the Director is treated as one of its own Lumes for output purposes. This loopback is dispatch-side behaviour and is described in detail in the [user manual's theory of operation](https://github.com/ratcliffej/nocturnation-docs/blob/main/manuals/user-manual.md#1-theory-of-operation) and the [protocol manual's class-and-group addressing](https://github.com/ratcliffej/nocturnation-docs/blob/main/manuals/protocol-manual.md#4-class-and-group-addressing).

The architecture has settled enough that protocol-level documentation is now public-facing rather than internal design notes - hence the [protocol manual](https://github.com/ratcliffej/nocturnation-docs/blob/main/manuals/protocol-manual.md). Third-party implementations are welcome.

---

## Testing

Three layers.

**Native unit tests** run on the host - no hardware needed. The suite covers the analyser, the transport, the plug-in surfaces, the show framework, and bit-for-bit IR encoder parity against [jamesw343/PixMob_IR](https://github.com/jamesw343/PixMob_IR)'s Python reference. Each native environment is declared in `platformio.ini` (prefixed `native`); run them with:

```bash
pio test -e native        # and the other native_* environments listed in platformio.ini
```

**Build verification** ensures both firmware environments compile clean:

```bash
pio run -e m5stack-stickcs3 -e m5stack-stickcplus2
```

Warnings are treated as signal; the current source compiles clean.

**Hardware verification** is the only way to verify the audio pipeline, the IR-side rendering, and bracelet response. The recommended ritual is in the [user manual](https://github.com/ratcliffej/nocturnation-docs/blob/main/manuals/user-manual.md#5-modes-and-shows).

---

## Project layout

```
include/                  HAL/DAL/plug-in/transport public interfaces.
src/hal/                  HAL backends (m5stickc-plus2, m5stickc-s3).
src/dal/                  DAL implementation, analyser, render dispatch.
src/transport/espnow/     ESP-NOW frame encode/decode.
src/modes/                Runtime mode finite-state-machine.
src/shows/                Show plug-ins (simple_beat, dynamic).
src/output_bindings/      Lume-side render targets (local_display, pixmob_ir).
src/visualisations/       Legacy visualisations (kept for migration).
include/pixmob_protocol.h PixMob IR encoder (header-only port from jamesw343/PixMob_IR).
test/                     Native unit tests, one folder per native env.
boards/                   PlatformIO board definitions.
platformio.ini            Build, library, and test configuration.
```

Documentation (manuals, architecture spec, developer guide) lives in the separate [nocturnation-docs](https://github.com/ratcliffej/nocturnation-docs) repository.

---

## Status and roadmap

NocturNation runs today as a full Director/Lume system on the M5 Sticks: beat- and section-reactive shows, multi-Lume ESP-NOW fan-out with redundant transmission and signal-quality feedback, and class+group addressing so an operator can light a chosen subset of the audience. A companion receiver and manual-Director app for the EMF Tildagon badge lives in [nocturnation-tildagon](https://github.com/ratcliffej/nocturnation-tildagon).

Next:

- **Public launch (EMF 2026)** - open-source repositories, a landing page, and the Tildagon app submitted to the EMF badge app store.
- **DMX / QLC+ integration** - a `DmxOutputBinding` so NocturNation can drive conventional stage fixtures alongside bracelets.

---

## Known issues

- [`pixmob::buildCycleProfiles`](include/pixmob_protocol.h) interprets its `profileMask` argument as an 8-bit profile mask, but the upstream Python reference treats the same bits as a `profile_id_lo`/`profile_id_hi` range. The function is unused in the current firmware, so runtime behaviour is unaffected. Reconciliation tracked as a long-standing carry-forward.

---

## Acknowledgements

This project would not exist without the prior reverse-engineering work of Daniel Weidman and James Wilson - both are credited in [REFERENCES.md](REFERENCES.md) in Harvard format, alongside the canonical PixMob protocol documentation in [jamesw343/PixMob_IR](https://github.com/jamesw343/PixMob_IR).

---

## Licences

- **Code:** MIT (see [LICENSE](LICENSE)).
- **Documentation:** CC BY-SA 4.0 (per the project architecture specification §13).
- **Hardware designs (when published):** CERN-OHL-S 2.0.

---

## Contributing

Issues and pull requests welcome. Major changes - particularly to the IR encoder, the beat-detection thresholds, or the protocol surface - should reference the [protocol manual](https://github.com/ratcliffej/nocturnation-docs/blob/main/manuals/protocol-manual.md) to confirm the change is in scope before any code lands.

For protocol changes, please regenerate the parity-test reference vectors against jamesw343's Python encoder rather than against the local C++ output, to preserve the upstream-as-truth invariant.

### Adding a new Show

The Director's performance is implemented by a Show plug-in. To add your own, see the [developer guide](https://github.com/ratcliffej/nocturnation-docs/blob/main/developing-shows.md) - it covers the `Show` base class API, the analyser hook surface, class+group routing via `render_fx`, screen rendering, widget composition, NVS persistence, and the testing pattern, with `DynamicShow` as the worked example.
