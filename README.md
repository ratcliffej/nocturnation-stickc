# NocturNation M5StickC Plus2 firmware

> Open-source crowd lighting, conjured from cheap silicon.

A reference implementation of the [NocturNation](https://github.com/ratcliffej/nocturnation-stickc) crowd-lighting system, built on a £25 M5StickC Plus2. Clap, kick, or aim it at a PixMob bracelet and watch the bracelet flash in time with the music.

NocturNation as a whole aspires to be a modular open-source alternative to the proprietary stadium fan based lighting systems sold to touring bands, giving smaller bands and art installations a similar platform. A NocturNation transmitter costs roughly the price of a takeaway. This repository is the starting point: one transmitter, one wearer. Mesh networking, badge receivers and DMX/Art-Net bridges are on the longer-term roadmap; see the [Roadmap](#roadmap) section below.

---

## What's in the box

The firmware does three things:

1. **Direct-fire mode (idle).** Pick a colour with the side button, hit the front button, the bracelet flashes that colour. Use it as a remote control.
2. **Beat-detection mode.** Press the power button and the StickC Plus2 starts listening through its built-in PDM mic. An FFT picks out kick-drum energy in the 62-187 Hz bass band, and every detected beat fires a synchronised colour pulse at the bracelet. The envelope (attack / sustain / release) adapts to the detected BPM so fast and slow tracks both feel right.
3. **Group setup helper.** A function in the source that assigns a PixMob bracelet to a specific group ID, so multi-wearer shows can address subsets independently. Not exposed in the UI yet.

---

## Hardware

| Item | Notes |
| --- | --- |
| M5StickC Plus2 | ESP32-PICO-V3-02, 1.14" 240 × 135 TFT, PDM mic, IR LED on GPIO 19, ~2 hr battery. |
| PixMob X4 Gen3.1 bracelet | The ones distributed at Coldplay's *Music of the Spheres* tour, 2022-2024. Earlier hardware revisions are partially compatible; later revisions are not yet tested. |
| USB-C cable | For flashing. |
| A speaker playing music | Anything with a strong kick on the downbeat. The reference test track is Vengaboys, *We Like to Party* (138 BPM). |

---

## Quick start

### Prerequisites

- macOS, Linux, or Windows.
- [Visual Studio Code](https://code.visualstudio.com/) with the [PlatformIO IDE extension](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide).
- The PlatformIO extension brings its own bundled CLI; you do not need a system-wide `pio` install.

### Clone, build, flash

```bash
git clone https://github.com/ratcliffej/nocturnation-stickc.git
cd nocturnation-stickc
```

Open the folder in VS Code. PlatformIO will index libraries on first open (~30 seconds).

To build:

```bash
pio run -e m5stack-stickcplus2
```

To flash a connected StickC Plus2:

```bash
pio run -e m5stack-stickcplus2 -t upload
```

Or use the PlatformIO sidebar: **Build** then **Upload** under the `m5stack-stickcplus2` env.

If `pio` isn't on your `PATH`, the PlatformIO extension installs it at `~/.platformio/penv/bin/pio` on macOS and Linux, and `%USERPROFILE%\.platformio\penv\Scripts\pio.exe` on Windows.

### Optional: serial monitor

```bash
pio device monitor -e m5stack-stickcplus2
```

The current build is mostly silent on the serial port; it's useful when developing.

---

## Using it

Once flashed, the StickC Plus2 boots into idle mode.

| Button | Idle | Beat mode |
| --- | --- | --- |
| **A** (front) | Send a single IR pulse to any bracelet in front of the device, in the current colour. | Toggle "muted": keep detecting beats and updating the BPM, but don't transmit. |
| **B** (side) | Cycle through the colour palette: RED → GREEN → BLUE → YELLOW → WHITE → RED ... | Same. |
| **PWR** (top, short click) | Enter beat mode. | Exit beat mode. |

In beat mode the screen shows the current colour, detected BPM, battery percentage, and a live spectral-flux meter with the beat-detection threshold marked in red. Aim the StickC Plus2 at a PixMob bracelet from anywhere up to about three metres in a dark room, play a track with a clear kick drum, and the bracelet should pulse on each detected beat.

A few seconds of music are needed before the BPM estimate stabilises (the firmware needs three valid inter-beat intervals before it commits to a number).

---

## Testing

Two layers of automated verification, plus a third manual one.

### Native unit tests

Pure-logic tests run on the host - no hardware needed. The current suite covers:

- Sanity check that the test toolchain is alive.
- Bit-for-bit IR encoder parity against [jamesw343/PixMob_IR](https://github.com/jamesw343/PixMob_IR)'s Python reference for a representative set of inputs.
- HAL capability declaration / query mechanics.
- DAL registry, capability supports, fail-silent dispatch, and event delivery to subscribers.

Tests live in two native environments: `native` (header-only tests) and `native_dal` (tests that exercise `src/dal/dal.cpp`). Run both with:

```bash
pio test -e native -e native_dal
```

You should see 23 passing tests across the four test suites.

`pio run` (no `-e`) builds the firmware only. `pio test` without `-e` will not pick up the native envs because the firmware env is the only `default_env`; passing both `-e` flags above is the explicit invocation.

### Build verification

Compiler warnings are treated as signal:

```bash
pio run -e m5stack-stickcplus2
```

The build flags include `-Wformat -Wformat-security`. The current source compiles clean.

### Hardware verification

There is no automated test for the audio side, the display, or the bracelet response - the only "is the audio pipeline correct" test is a human watching a bracelet flash to a kick drum. Recommended ritual:

1. Flash the StickC Plus2.
2. Place a PixMob bracelet in front of it.
3. Play *We Like to Party* by Vengaboys at moderate volume on a speaker in the room.
4. Press **PWR** to enter beat mode.
5. Within ten seconds the BPM display should converge on something near 138, and the bracelet should be flashing in time with the kick.

---

## Project layout

```
include/pixmob_protocol.h    PixMob IR encoder (header-only port from jamesw343/PixMob_IR).
src/main.cpp                 Firmware entry point, FFT + UI + IR transmission.
test/test_sanity/            Native sanity check that the test toolchain is alive.
test/test_pixmob_parity/     Bit-for-bit parity tests against the Python reference encoder.
test/support/Arduino.h       Minimal host-side shim so the encoder header compiles natively.
boards/m5stickc_plus2.json   PlatformIO board definition for the Plus2 (8 MB flash variant).
platformio.ini               Build, library, and test configuration.
```

The `[env:native]` section of `platformio.ini` excludes firmware sources from the host build; only the test files compile against the native toolchain.

---

## Roadmap

- **Epic 1 (this milestone, complete).** Establish a clean PlatformIO baseline of the existing prototype, with parity tests against the upstream PixMob protocol reference, published as a public repo with this README.
- **Epic 2 (next).** Architecture refactor: introduce a hardware abstraction layer (HAL) to decouple the firmware from the M5StickC Plus2 and the M5Unified library, then extract the FX engine, the IR transport, and the mode state machine behind clean abstractions. The goal is enabling alternate host boards (vendor-independent), additional transports (ESP-NOW mesh, BLE), and additional receivers (Tildagon badges, DMX/Art-Net bridges) without touching beat detection.
- **Beyond.** Multi-node mesh, group-addressed effects, audience-app integrations.

The full architecture specification lives in [docs/architecture.md](docs/architecture.md), with active Epic plans in [docs/epics/](docs/epics/). The Notion mirror is private; the Markdown copies in this repo are the source of truth.

---

## Known issues

- [`pixmob::buildCycleProfiles`](include/pixmob_protocol.h#L132-L146) interprets its `profileMask` argument as an 8-bit profile mask, but the upstream Python reference treats the same bits as a `profile_id_lo`/`profile_id_hi` range. The function is unused in the current firmware, so runtime behaviour is unaffected. Reconciliation is deferred to Epic 2.

---

## Acknowledgements

This project would not exist without the prior reverse-engineering work of Daniel Weidman and James W. - both are credited in [REFERENCES.md](REFERENCES.md) in Harvard format, alongside the canonical PixMob protocol documentation in jamesw343's repository.

---

## Licences

- **Code:** MIT (see [LICENSE](LICENSE)).
- **Documentation:** CC BY-SA 4.0 (per the project architecture specification §13).
- **Hardware designs (when published):** CERN-OHL-S 2.0.

---

## Contributing

Issues and pull requests welcome. Major changes - particularly to the IR encoder or the beat-detection thresholds - should reference the architecture specification to confirm the change is in scope for the current Epic before any code lands.

For protocol changes, please regenerate the parity-test reference vectors against jamesw343's Python encoder rather than against the local C++ output, to preserve the upstream-as-truth invariant.
