# NocturNation

> Open-source crowd lighting, conjured from cheap silicon.

NocturNation is a modular open-source crowd-lighting system: one Director Stick listens to music, detects beats and structural events, and broadcasts light commands to a swarm of Lume Sticks that fire infra-red at PixMob bracelets worn by the audience. A full deployment costs roughly the price of a meal out, and gives smaller bands and art installations the same kind of platform that proprietary stadium fan-lighting systems sell to touring acts at five-figure prices.

This repository is the reference firmware. It runs on the M5StickC Plus2 and the M5StickS3 (the "Sticks"). Both Sticks share a single firmware codebase with hardware-specific abstraction underneath.

---

## Documentation

| Document | Audience |
|---|---|
| [User manual](docs/manuals/user-manual.md) | Operators setting up a venue. Theory of operation, hardware, firmware install, configuration walk-through, modes and shows, troubleshooting, glossary. |
| [Protocol manual](docs/manuals/protocol-manual.md) | Implementers building a third-party transmitter or receiver. Wireless layer, frame formats, class-and-group addressing, PixMob IR annex, conformance, test vectors. |
| [Developer guide](docs/developing-shows.md) | Contributors writing new `Show` plug-ins for the firmware. The `Show` base class, analyser hooks, `render_fx` API, widget composition, persistence, testing. |
| [Architecture spec](docs/architecture.md) | Internal design notes. Bidirectionally synced with the Notion source-of-truth page. |
| [Active Epics](docs/epics/) | Per-Epic plans and close-outs. |

---

## Hardware

| Item | Notes |
|---|---|
| **M5StickC Plus2** or **M5StickS3** | The Stick. Either can run as Director, Lume, or both. The S3 is the current first-class reference; the Plus2 (now end-of-life from M5Stack) remains fully supported. See the [hardware section of the user manual](docs/manuals/user-manual.md#2-hardware) for the comparison. |
| **PixMob X4 Gen 3.1 bracelets** | The reference target. Distributed at Coldplay's *Music of the Spheres* tour (2022-2024) and widely available second-hand. Earlier generations are partially compatible; later generations have not been bench-tested. |
| **USB-C cable** | For flashing the Sticks. |
| **A speaker playing music** | Anything with a clear kick drum. The reference test track is Vengaboys, *We Like to Party* (138 BPM). |

A useful first deployment is one Director plus one Lume, in a small room with a handful of bracelets. For larger venues see [hardware deployment guidance in the user manual](docs/manuals/user-manual.md#23-ir-radiation-patterns).

---

## Quick start

```bash
git clone https://github.com/ratcliffej/nocturnation-m5.git
cd nocturnation-m5
pio run -e m5stack-stickcs3 -t upload
```

Substitute `m5stack-stickcplus2` for a Plus2. Open the folder in VS Code with the [PlatformIO IDE extension](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide) for the integrated build/upload flow. The PlatformIO extension ships its own CLI, so a system-wide `pio` install is not required; on macOS it lives at `~/.platformio/penv/bin/pio`.

For a more thorough quickstart - including button layout, mode flow, and how to add a second Lume - read the [user manual quickstart](docs/manuals/user-manual.md#quickstart).

---

## Architecture at a glance

The firmware has a six-layer plug-in architecture:

1. **HAL** - hardware abstraction (mic, IR, display, buttons, BLE, ESP-NOW). Plus2 and S3 backends.
2. **DAL** - device-abstraction layer. Holds the audio analyser core (BeatDetector, DropDetector, music descriptors), event bus, and the canonical `render_fx` dispatch.
3. **Plug-ins** - `Plugin` base class with property bags and per-plug-in NVS namespaces.
4. **Analyser** - sits on the DAL's mic pipeline; produces beat, drop, and music-descriptor events that the Director's Show consumes.
5. **Shows** (Epic 4.7) - operator-selectable performances. Currently `SimpleBeatShow` (faithful pre-4.7 BeatPulse behaviour) and `DynamicShow` (FFT-driven HSV with per-drum group routing).
6. **OutputBindings** - Lume-side render targets. Currently `LocalDisplayBinding` (LCD pulse) and `PixMobIrBinding` (infra-red wire encoder, a pure relay).

Every render call flows through `render_fx("<class>:<group>", ev)` with structured class+group targets. The Director's dispatch fans every call out to ESP-NOW broadcast, the Director's own infra-red transmitter, and the Director's screen pulse - so the Director is treated as one of its own Lumes for output purposes. This loopback is dispatch-side behaviour and is described in detail in the [user manual's theory of operation](docs/manuals/user-manual.md#1-theory-of-operation) and the [protocol manual's class-and-group addressing](docs/manuals/protocol-manual.md#4-class-and-group-addressing).

The architecture has settled enough that protocol-level documentation is now public-facing rather than internal design notes - hence the [protocol manual](docs/manuals/protocol-manual.md). Third-party implementations are welcome.

---

## Testing

Three layers.

**Native unit tests** run on the host - no hardware needed. The current suite has 348 tests across 17 native environments and covers the analyser, the transport, the plug-in surfaces, the show framework, and bit-for-bit IR encoder parity against [jamesw343/PixMob_IR](https://github.com/jamesw343/PixMob_IR)'s Python reference. Run all native suites with:

```bash
pio test -e native -e native_dal -e native_modes -e native_effects -e native_espnow -e native_audio -e native_analyser -e native_plugin -e native_input_action -e native_visualisation -e native_output_binding -e native_beat_pulse -e native_output_binding_concrete -e native_master_overlay -e native_spectrum_bars -e native_pixmob_parity -e native_widgets
```

(Or build a wrapper script; the explicit `-e` list is awkward but is what PlatformIO requires.)

**Build verification** ensures both firmware environments compile clean:

```bash
pio run -e m5stack-stickcs3 -e m5stack-stickcplus2
```

Warnings are treated as signal; the current source compiles clean.

**Hardware verification** is the only way to verify the audio pipeline, the IR-side rendering, and bracelet response. The recommended ritual is in the [user manual](docs/manuals/user-manual.md#5-modes-and-shows).

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
src/visualisations/       Legacy visualisations (pre-Epic-4.7; kept for migration).
include/pixmob_protocol.h PixMob IR encoder (header-only port from jamesw343/PixMob_IR).
test/                     Native unit tests, one folder per native env.
boards/                   PlatformIO board definitions.
platformio.ini            Build, library, and test configuration.
docs/                     Architecture spec, manuals, Epic plans, developer guide.
```

---

## Roadmap

Closed Epics:

- **Epic 1** - PlatformIO baseline + byte-identical IR encoder parity vs jamesw343's reference.
- **Epic 2** - Hardware abstraction layer + Device abstraction layer + Effect classes + Mode FSM + TestDevice extensibility.
- **Epic 3** - Boot countdown, mode menu, Test Mode, Config tree, status display.
- **Epic 4** - ESP-NOW transport on Plus2 + S3, redundant TX, dedup ring, signal-quality bars, Lume-as-repeater, two-channel architecture.
- **Epic 4.5** - Capability-aware audio analyser. Sub-band adaptive BeatDetector, DropDetector with arm/disarm gate, `MUSIC_EVENT` wire format (the wire frame and the DROP/BREAKDOWN effect rendering were both removed in the v0.29 spec protocol trim; the detectors remain internal).
- **Epic 4.6** - Clean plug-in architecture. `Visualisation` and `OutputBinding` plug-in surfaces, semantic `InputAction` layer, per-plug-in NVS namespaces.
- **Epic 4.65** - Class+group device addressing. `render_fx("<class>:<group>")` structured targets; `LightCommandPayload` carries both bytes on the wire.
- **Epic 4.7** - Show plug-in framework + DynamicShow. `Show` base class atop `Plugin`; widget library (BeatBarWidget, SpectrumBarsWidget); analyser primitives (snare/hi-hat onset, music descriptors, section detection); Director-IR loopback in dispatch. (The Epic-4.7 IR reset primer was rolled back in Epic 4.8 after bench testing — see [user manual §1.5](docs/manuals/user-manual.md#15-bracelet-timing-and-residual-state).)

In progress:

- **Epic 4.8** - User manual and NocturNation protocol manual (this Epic). Doc-only; the manuals link from this README and live under [docs/manuals/](docs/manuals/).

Next on the roadmap:

- **Epic 5** - Tildagon receiver. Second host backend; pressure-tests the HAL contract on a non-M5Unified host (ESP32-C3 + MicroPython). The protocol manual is the implementation specification.
- **Epic 7** - DMX / QLC+ integration. Show-composer bridge via a `DmxOutputBinding`.

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

Issues and pull requests welcome. Major changes - particularly to the IR encoder, the beat-detection thresholds, or the protocol surface - should reference the architecture specification or the [protocol manual](docs/manuals/protocol-manual.md) to confirm the change is in scope before any code lands.

For protocol changes, please regenerate the parity-test reference vectors against jamesw343's Python encoder rather than against the local C++ output, to preserve the upstream-as-truth invariant.

### Adding a new Show

The Director's performance is implemented by a Show plug-in. To add your own performance see [docs/developing-shows.md](docs/developing-shows.md) for the developer guide - it covers the `Show` base class API, the analyser hook surface, class+group routing via `render_fx`, screen rendering, widget composition, NVS persistence, and the testing pattern, with `DynamicShow` as the worked example.
