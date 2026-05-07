---
title: "Epic 1: StickC reference implementation parity"
status: cross-project (will move to umbrella repo when Tildagon work begins)
notion_url: https://www.notion.so/358bd0677405814f9491df2ee822e342
notion_id: 358bd0677405814f9491df2ee822e342
notion_status: Done
last_synced: 2026-05-07
sync_direction: bidirectional
---

## Related Documents

- [NocturNation Architecture Specification](https://www.notion.so/357bd0677405800b891beab0f4e0a976) - particularly §2 (Architecture overview), §3.4 (Development tooling), §13 (Licensing)
- [Original prototype code (MixMobFX Files)](https://www.notion.so/358bd067740580bab876cd7c2b7ee6bf)

## Goal

Take the existing prototype StickC firmware (FFT beat detection + IR transmission to PixMob bracelets), establish a properly-structured PlatformIO project for it in VS Code, publish it as the canonical NocturNation GitHub repository with a README, and confirm the existing functionality still works under the new project structure with no regressions.

## Operational model: laptop-driven, not autonomous

NocturNation is embedded firmware with hardware in the loop. Unlike server-side projects in the AIOS where Ada writes code and Alan QA-gates against deterministic curl-able endpoints, NocturNation work involves real photons, real microphone input, real timing feel, and real bracelets. Most acceptance criteria genuinely cannot be verified without Jason at his laptop with hardware in front of him.

This Epic (and the rest of NocturNation) therefore follows a **laptop-driven pattern**:

- Jason does the actual development at his laptop, with the StickC plugged in, using Claude Code interactively as a coding partner rather than as an autonomous agent
- The AIOS agent squad provides supporting work via direct invocation: Florence drafts docs and READMEs, Edward researches libraries and components, Charles maintains the Notion knowledge base, Isambard helps decompose new chunks of work as they come up, Alan reviews PRs that Jason has pushed
- Status flow is simplified: **Proposed → Ready → In Progress → Done**. No `In Review` / `Ada ↔ Alan` cycle, no `MAX_REVIEW_CYCLES`, no heartbeat-dispatcher routing
- Tasks are checklist items Jason ticks off as he works through them, not jobs the dispatcher claims and runs
- The Features below are **work organisation**, not autonomous-agent contracts. They tell Jason what to do next; they don't get promoted to Ready for Ada

This is the right shape for the work. AGP Cloud's autonomous-Ada pattern shines on backend services with deterministic verification; embedded firmware with aesthetic-judgement acceptance criteria wants the human at the keyboard. Trying to force the wrong pattern is how good projects die from process fatigue.

## Testing strategy

Three practical layers, with honest sign-off ownership:

1. **Native unit tests on Jason's laptop** (`pio test -e native`). For pure logic with deterministic outputs: PixMob byte encoder against reference vectors, FFT post-processing functions, mode state machine transitions, frame format encoder/decoder when added in later Epics. Jason runs these locally; failures are hard fails.
2. **PlatformIO build validation** (`pio run -e stickc -Wformat -Wformat-security`). Compiler warnings treated as errors. Catches the 80% of "obvious" mistakes before flashing.
3. **Hardware verification on the StickC + PixMob bracelet**. Done physically by Jason. Covers everything visual, audio-driven, timing-feel-related, or usability-related. The fact that this can't be automated is not a problem to solve - it's a property of the work.

No Wokwi, no autonomous Ada, no headless simulation pipeline. If something turns out to genuinely need cycle-time-saving simulation later, we add it then. Premature tooling is its own kind of waste.

## Scope

**Included:**

- **Establish a new PlatformIO project in VS Code** following NocturNation conventions (project structure, `platformio.ini` config with both `[env:stickc]` and `[env:native]` defined, libraries declared explicitly). Confirm PlatformIO extension is installed and working - first build of the empty skeleton must succeed before any code is migrated.
- **Create the canonical NocturNation GitHub repository** under a personal or organisation account. Public visibility, MIT licence per spec §13, sensible `.gitignore` covering `.pio/`, `build/`, IDE-specific cruft.
- **Author a [README.md](http://README.md)** at the repo root covering: project tagline, what NocturNation is in 2-3 sentences, link to the architecture spec, hardware requirements (StickC Plus2 + PixMob bracelet), build instructions (`pio run`, `pio run -t upload`), brief usage notes, links to related documentation. Aim: a stranger can go from `git clone` to working device in under 15 minutes.
- Migrate existing FFT beat-detection code into the new project, preserved bit-for-bit
- Migrate existing PixMob IR encoder (`pixmob_protocol.h`) into the new project, preserved bit-for-bit
- Existing UI (Btn-A test, Btn-B colour cycle, BtnPWR mode toggle) preserved
- One or two native unit tests written for the PixMob encoder against known reference vectors, just to prove the testing pattern works
- Build configuration in `platformio.ini` documented and reproducible across machines
- Initial commit pushed to GitHub with a meaningful first-commit message

**Explicitly excluded:**

- Architecture refactor (FX abstraction, transport abstraction, mode state machine) - that's Epic 2
- New UI per spec §8 - that's Epic 3
- ESP-NOW work - that's Epic 4
- DMX work - that's Epic 7
- Wokwi simulation, MCP integration, autonomous-agent dispatch - genuinely out of scope; revisit only if a real need emerges
- Any change to behaviour. Same beats detected, same colours fired, same envelope. If it looked different at the tribute act, this Epic has failed.

## Acceptance Criteria

Verification ownership in brackets - **(L)** = laptop / native test, **(B)** = build-time check, **(H)** = hardware verification by Jason.

- [x] **(L)** PlatformIO project skeleton exists under VS Code; `pio run -e stickc` builds successfully on an empty `setup() / loop()` skeleton (proves toolchain works before code migration)
- [x] **(L)** `pio test -e native` runs successfully against a one-liner test (proves native test environment is set up)
- [x] **(B)** PlatformIO build succeeds without warnings under `-Wformat -Wformat-security` flags
- [x] **(H)** Existing prototype code migrated into the new structure; firmware built from the new repo flashes to a StickC and produces visible behaviour matching the existing prototype: same Vengaboys BPM tracking, same beat-locked IR transmission, same UI
- [x] Canonical NocturNation GitHub repository created, publicly visible, MIT-licensed, with sensible `.gitignore`
- [x] [README.md](http://README.md) present at repo root covering project intro, hardware requirements, build steps, link to architecture spec
- [ ] **(H)** README walk-through validated: `git clone` followed by README's instructions takes Jason (or a willing victim like Diane) from clone to working device in <15 minutes
- [x] Initial commit pushed to GitHub `main` branch with descriptive commit message
- [x] At least one native unit test exists for the PixMob encoder, asserting bytes match a known reference vector from jamesw343's repository

## Next three things to do

A pragmatic ordering of work, not a formal SDLC decomposition. Each block is roughly one focused work session.

### 1. Project foundation (one evening)

Create the GitHub repo, establish the PlatformIO project, get an empty `setup()/loop()` skeleton building cleanly under both the `[env:stickc]` and `[env:native]` environments, verify the native test environment runs, push the initial commit.

- New GitHub repo at `nocturnation/nocturnation-firmware` (or similar). Public, MIT-licenced, sensible `.gitignore`
- VS Code workspace with PlatformIO extension confirmed working
- `platformio.ini` with both `[env:stickc]` (target hardware) and `[env:native]` (laptop unit tests) defined
- One trivial native unit test (`TEST_ASSERT_EQUAL(2, 1+1)`) confirming `pio test -e native` works
- One trivial firmware build (blink the screen backlight) confirming `pio run -e stickc -t upload` works on actual StickC
- Initial commit pushed: "Project foundation: PlatformIO + native test env + empty firmware skeleton"

Deliverable: a buildable, flashable, testable empty project. The next two blocks just add code into this skeleton.

### 2. Code migration (one evening)

Bring the existing prototype code into the new structure, preserving behaviour byte-for-byte where possible.

- Copy `pixmob_protocol.h` from the prototype into `lib/pixmob/` or `src/`, no changes
- Copy the FFT beat-detection code, no changes
- Copy the existing UI handling (Btn-A test, Btn-B cycle, BtnPWR toggle), no changes
- Add one native unit test asserting `pixmob_protocol.h` produces bytes matching a known reference vector from jamesw343's repo
- Build under `-Wformat -Wformat-security`; fix any warnings (likely none if the prototype was clean)
- Commit: "Migrate prototype code: FFT beat detection, PixMob encoder, basic UI"

Deliverable: working firmware that ought to behave identically to the prototype. Verification is the next block.

### 3. Verification and README (one evening, hands-on with hardware)

Flash the new build to a StickC, point it at a PixMob bracelet, play Vengaboys, confirm everything works the same as before. Author the README from the perspective of someone who's just done this themselves.

- Flash, watch, listen, confirm: same BPM tracking, same beat-locked IR, same UI behaviour
- If anything has drifted: fix it before moving on (drift is the only thing this Epic exists to prevent)
- Write [README.md](http://README.md) covering: tagline, project intro, hardware requirements, build steps verified just-now, troubleshooting notes for things that tripped you up, link to architecture spec
- Optional: ask Diane to follow the README cold and time how long it takes her to get to a working device. Whatever she struggles with, the README needs more detail there.
- Commit: "Verify parity, add README"

Deliverable: Epic 1 done, Epic 2 (architecture refactor) can now begin from a known-good baseline.

## Dependencies

| Dependency | Type | Status | Owner |
| --- | --- | --- | --- |
| Existing prototype source code | Internal | Available | Jason |
| StickC Plus2 hardware | External | Available | Jason |
| PixMob bracelet for verification | External | Available | Jason |

## Target Sprint Range

- **Start sprint:** TBD (next available)
- **End sprint:** Same sprint (small Epic)
- **Indicative complexity total:** 8-10 points across child Features

## Status Notes

Proposed 2026-05-06 from spec v0.16 epic decomposition. Operational model clarified 2026-05-06: laptop-driven rather than autonomous-Ada-dispatch, after honest reflection on the limits of autonomous testing for embedded hardware-in-the-loop work.

**Closed 2026-05-06** with commit `9cbe945`. All four Block-1/2/3 commits public at <https://github.com/ratcliffej/nocturnation-stickc>. Hardware verification passed on a Plus2 + PixMob X4 Gen3.1 against the Vengaboys reference track; no behaviour drift from the prototype. The Diane-style cold README walk-through has not been performed; that one acceptance criterion remains open as follow-up work and is the only AC not ticked above. Two findings carried forward into Epic 2 planning: (a) `pixmob::buildCycleProfiles` semantic divergence from the upstream Python reference - function is unused in the firmware so runtime behaviour is unaffected, captured in the repository's README under Known Issues; (b) a hardware abstraction layer (HAL) was added to Epic 2's abstraction stack after a Wokwi experiment in this Epic exposed how tightly M5Unified is coupled to the M5StickC.

No Features will be created as child records under this Epic. The "Next three things to do" section above is the working breakdown - it's a checklist for Jason at his laptop, not a set of tickets to be claimed by the dispatcher. If a particular evening's work surfaces something that genuinely warrants its own Notion record (a meaty design decision, a research spike, a follow-up bug), file it then; don't pre-create scaffolding.

Note: Despite being the cheapest Epic, this is genuinely valuable because it establishes the project structure that every subsequent Epic builds on. Skipping it leads to subtle drift - inconsistent file layouts, different build configs, README divergence - that compounds over time.
