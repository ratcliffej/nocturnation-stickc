---
title: "Epic 4.5: Capability-aware audio analyser with sub-band adaptive-threshold beat detection"
status: Done
notion_url: https://www.notion.so/35bbd0677405816fa9fbd0306100c794
notion_id: 35bbd0677405816fa9fbd0306100c794
notion_status: Done
last_synced: 2026-05-10
sync_direction: bidirectional
---

## Closed 2026-05-10

Cross-device hardware verification (Plus2 + S3 in Director mode listening to the same audio) confirms behavioural equivalence: same beat pattern, same response. BPM tracking matches actual song tempo (was reading 155 from a 112 BPM track on tuning round 1; reads correctly after round 2). DROP fires on chorus drops; BREAKDOWN fires on quiet sections.

**Blocks shipped:**

- **Block 1** — Audio sample harness: synthetic generators (sine / kick-train / noise / silence) + 16-bit PCM mono WAV I/O + `native_audio` test env (`2c8e11d`).
- **Block 2** — Capability-aware analyser surface: pure DAL analyser core, Capability flags + extended AudioFrame, mic-backend wiring + SpectrumFrameEvent + DAL stubs (`484adaa`, `801d3ab`, `46131c6`).
- **Block 3** — Sub-band adaptive-threshold beat detection: `BeatDetector` class + native tests, integration into LocalDriver, two rounds of hardware-driven tuning (`61eee9c`, `98a687b`, `ee96830`, `ddf7f3a`).
- **Block 4** — Drop and breakdown detection + MUSIC_EVENT 0x06: `DropDetector` with arm/disarm gate, ESP-NOW wire format, Director-side broadcast (`42cef9b`, `69e3d52`, `06f7996`).
- **Block 5** — Cross-device hardware verification (Plus2 + S3 side-by-side).
- **Block 6** — Architecture spec v0.22 (`48857dc`, plus follow-up additions in `10a6a7b`).

**Settled tuning parameters:**

- BeatDetector: `threshold_k=2.2`, `refractory_ms=200`, `watch_count=8` (bands 0-7 covering ~30-150 Hz), `warmup_frames=8`.
- DropDetector: `drop_ratio=1.8`, `breakdown_ratio=0.4`, `cooldown_ms=4000`, `short_window=80` (~2 s), `long_window=400` (~10 s); arm/disarm gate prevents sustained-state re-firing.

**Late additions during Block 5 verification:**

- **MUSIC_EVENT serial logging** (`653366b`): grep-friendly `[MUSIC] DROP at <ms>` line so DROP/BREAKDOWN fires are visible during live testing without trawling hex dumps.
- **Heartbeat-fire diagnostic** (`e680c61`): `[HBEAT] firing after <ms> gap` so the skip-if-recent contract is observable from serial output.
- **BeatDetector tuning round 2** (`ddf7f3a`): `threshold_k` 2.5 → 2.2 to catch soft kicks following louder ones (mechanism: post-kick history elevation raises threshold for ~1 s; structural fix flagged as future direction).
- **BEAT_DETECTED Director broadcast removed** (`10a6a7b`): Lumes consume LIGHT_COMMAND for show rendering; BEAT_DETECTED's BPM/strength metadata had no current consumer, so doubling per-kick airtime was net negative. Wire format definition retained for future re-enable.
- **Spec future-extensions additions** (`10a6a7b`): detector-layer placement (DAL vs orchestration architectural retrospective), source-separation directions (Demucs / Spleeter / Neuraliser-class with phone-side / Mac-bridge / cue-file architectures captured).

**Honest list of known incomplete:**

- **DROP detection is sporadic on tracks without clear bass dynamics.** Works reliably on EDM / pop chorus drops; less reliable on tracks with uniform energy or genres where "drop" doesn't manifest as a bass-energy ratio shift. The proper "what is a drop" answer needs the multi-band onset + spectral centroid + section-detection machinery in Epic 4.7. Documented in spec §5.4.
- **Beat detection occasionally misses softer kicks following louder ones.** Round 2 tuning (k=2.2) reduces but doesn't eliminate. The structural fix (outlier-rejecting mean: exclude individual loud spikes from contributing to the history's mean while still counting them for variance) is flagged in `beat_detector.h`'s tuning-history block as out-of-scope; revisit in Epic 4.7 or as a focused refinement.
- **Coldplay tribute regression test** not specifically run during Block 5; planned as a smoke test before the next gig.
- **Frequency-response calibration** not implemented (Config Mode tool that drives a sweep through a calibration speaker to measure raw mic response per host). Captured in spec §5.4.
- **Higher operating points** (S3 declares 32k/48k variants) not implemented; only canonical 16k/512 is wired up. Future Epic.
- **Spectrum-frame consumers** — the 32-band log-spaced spectrum is computed and delivered as `SpectrumFrameEvent` every FFT cycle but no current consumer subscribes. Epic 4.6 (Diagnostic UI hifi-style 8-band rendering) and Epic 4.7 (FX modulators) are the natural homes.

**Wire-protocol surface change** (vs Epic 4):

- **Added**: MUSIC_EVENT (0x06) carrying `event_type: u8` (1=DROP, 2=BREAKDOWN, 3=BUILD reserved). Forward-compatible: receivers decode unknown event_type bytes as Unknown and silently drop.
- **Removed from broadcast**: BEAT_DETECTED (0x01). Encoder/decoder retained; type/payload reserved for future re-enable.

Total native test count: 140 across 7 envs (`native`, `native_dal`, `native_modes`, `native_effects`, `native_espnow`, `native_audio`, `native_analyser`), all passing. Both firmware envs (`m5stack-stickcplus2`, `m5stack-stickcs3`) build cleanly.

## Related Documents

- [NocturNation Architecture Specification](https://www.notion.so/357bd0677405800b891beab0f4e0a976) - particularly §5 (Audio analysis pipeline), §6 (Effects catalogue), §3.1 (host capability declarations)
- [Epic 4: ESP-NOW transport](https://www.notion.so/358bd067740581e3afa8fd061b821638) - upstream Epic; this Epic depends on Epic 4 having shipped basic ESP-NOW so beats are actually broadcastable
- [Epic 4.7: Dynamic show generation from FFT](https://www.notion.so/35cbd067740581feb287ff7023202c19) - downstream Epic; 4.7 adds further analyser capabilities (multi-band onset, centroid, energy, sections) on top of the surface this Epic lands

## External references

- Audible Genius (n.d.) *Audio Frequency Range Bands Chart*. Available at: [https://audiblegenius.com/blog/audio-frequency-range-bands-chart](https://audiblegenius.com/blog/audio-frequency-range-bands-chart) (Accessed: 10 May 2026). Music-production reference for 8-band perceptual frequency split (Mud / Sub Bass / Bass / Low Mids / Midrange / High Mids / Presence / Air). Used as evidence base for this Epic's standardised B/M/T split-points (250 Hz at Bass→Low Mids, 2 kHz at Midrange→High Mids).

## Goal

Build a **capability-aware audio analyser** whose output surface is consistent across hosts (Plus2 and S3 today, Tildagon and future hosts tomorrow), with the **sub-band adaptive-threshold** algorithm as the concrete deliverable that gets us cross-device-consistent kick-beat detection. Land the analyser interface as the stable surface 4.7+ extends with richer descriptors, rather than re-architecting per Epic. Add **drop and breakdown detection** as a longer-window pass on the same FFT output, providing a `MUSIC_EVENT` message type alongside the existing `BEAT_DETECTED`. Restandardise the 3-band B/M/T summary to hifi-conventional ranges and surface the per-FFT-cycle spectrum frame as a first-class event so future renderers and modulators consume a stable data shape.

## Why this Epic exists

The single-threshold beat-detection failure during Epic 4 was the immediate trigger, but the underlying issue is broader: every host's audio analyser was implemented ad-hoc with no consistent capability surface, no consistent band layout, and no first-class event for the FFT output itself.

The cross-device divergence:

- Plus2 felt **too unreactive** - real beats with energy similar to ambient noise failed to cross the threshold
- S3 felt **too sensitive** - ambient transients (claps, door slams, keyboard clicks) crossed the threshold and triggered false beats

The root cause is genuinely hardware-driven: the Plus2's PDM mic has substantially poorer SNR than the S3's ES8311 codec + MEMS mic combination. Same algorithm + worse hardware = missed beats; same algorithm + cleaner hardware = false positives. A single-threshold approach inherently doesn't scale across this kind of hardware variation.

The deeper problem is structural: replacing the algorithm without fixing the analyser surface buys a year of pain when 4.7 lands richer descriptors. Each new descriptor would need its own ad-hoc surface; orchestration would have to special-case per host; the Diagnostic UI would need its own pathway. The lazy fix would be per-device threshold constants. The mid-effort fix is the algorithm swap alone. The proper fix - which is the right size for its own focused work session - is to land both the algorithm AND the capability-aware analyser surface that everything downstream consumes.

## Operational model

Laptop-driven, same as Epics 1-4. Algorithm work is genuinely well-suited to native unit testing on Jason's laptop using captured audio samples; hardware verification then confirms behavioural parity across Plus2 and S3.

Verification ownership: **(L)** = laptop / native test on captured audio, **(B)** = build-time check, **(H)** = hardware verification by Jason, with the additional convention **(H-Plus2)** / **(H-S3)** when a hardware test must specifically be performed on each device.

## Audio analyser capability model

The audio analyser is treated as a HAL+DAL capability cluster, same pattern as `mic` / `display` / `esp-now`. A host's analyser declares a flat set of feature flags describing what its output surface produces. Orchestration queries these and adapts; ESP-NOW Lumes consume whatever the Director broadcasts (their own analyser features are Director-side only and irrelevant for Lume behaviour).

**Features landed in this Epic:**

- `analyser.beat_detection` — produces `BEAT_DETECTED` events from the bass sub-bands via sub-band adaptive-threshold
- `analyser.drop_detection` — produces `MUSIC_EVENT` (DROP / BREAKDOWN) on a separate longer-window pass
- `analyser.spectrum_frame` — emits per-FFT-cycle 32-band log-spaced magnitudes as a first-class `SpectrumFrameEvent`
- `analyser.band_summary` — emits **both** the 3-band B/M/T roll-up and the 8-band perceptual split (Mud / Sub Bass / Bass / Low Mids / Midrange / High Mids / Presence / Air) per the Audible Genius reference, computed concurrently from the same FFT output — always-available concurrent fields, not a preset switch

**Features reserved for future Epics (declared as constants but not lit up):**

- `analyser.multi_band_onset` — SNARE/HIHAT events (Epic 4.7)
- `analyser.spectral_centroid` — centroid descriptor (Epic 4.7)
- `analyser.energy_envelope` — energy descriptor (Epic 4.7)
- `analyser.section_detection` — SECTION_CHANGE events (Epic 4.7)

**Parameters orthogonal to features** (host-declared, queried by orchestration where relevant):

- `audio_pipeline_operating_points` — list of valid `(sample_rate_hz, fft_size)` tuples the host supports. Plus2 declares `[(16000, 512)]`; S3 declares `[(16000, 512), (32000, 1024), (48000, 1024), (48000, 2048)]`; a future phone or PC HAL declares its own valid set. The first tuple is the host's default operating point.
- `current_operating_point` — `(sample_rate_hz, fft_size)` currently active. Default `(16000, 512)` across all hosts in this Epic for cross-device consistency.
- `sub_band_count` (default 32)
- `fft_hw_accelerated` (true on S3 with esp-dsp; false on Plus2 with ANSI fallback)

**Operating point trade-offs.** Sample rate and FFT size are independent parameters with different effects:

- **Sample rate** controls frequency *range* (Nyquist = sample_rate / 2). Higher sample rate captures higher frequencies — at 48 kHz the full Air band (6-20 kHz) is in scope; at 16 kHz it's truncated at 8 kHz.
- **FFT size** controls frequency *resolution* (bin width = sample_rate / fft_size). Larger FFT separates close frequencies more sharply at the cost of a longer time window per analysis (slower update rate, more latency).

S3 has the headroom for higher operating points (8 MB PSRAM, HW-accelerated FFT); Plus2 doesn't. The DAL exposes `configure_audio_pipeline(sample_rate_hz, fft_size)` for orchestration to pick a host-declared operating point. **In this Epic, only the default `(16000, 512)` is implemented across all hosts**; non-default operating points return explicit not-supported, leaving them as a future-Epic landing zone. Cross-device consistency in 4.5 means cross-device behavioural equivalence at the canonical operating point, not different operating points per host.

The API is host-agnostic by design: the same `audio_pipeline_operating_points` declaration and the same `configure_audio_pipeline()` setter work for ESP32 hosts now, and for phone or PC HALs in future. No ESP32-specific assumptions in the contract.

Plus2 and S3 declare the same feature set in this Epic; they differ only in the `fft_hw_accelerated` parameter and in the size of their `audio_pipeline_operating_points` list. Orchestration code is identical regardless of host; the difference shows up only in available CPU headroom and is invisible to the consumer when both are running at the canonical default. Tildagon (no microphone) declares no analyser capability at all — it consumes events from the Director via ESP-NOW.

The capability model is purely additive. Future Epics light up reserved feature flags or implement non-default operating points by extending the analyser implementation; nothing about the interface or the existing flags changes.

### Frequency band layout

**Two band-summary surfaces, always concurrent, evidence-based.** The current code emits `bass_energy / mid_energy / treble_energy` with ad-hoc bin assignments (the spec only documents bass at bins 2-7, ~62-220 Hz; mid and treble bin ranges have drifted in the source). This Epic restandardises and ships **two summary surfaces side-by-side**, both computed from the same FFT output in a single pass.

Band boundaries are specified in **Hz, not bin numbers**, so the API is portable across operating points and host platforms. Bin numbers are computed at runtime from the current `(sample_rate_hz, fft_size)` operating point; the bin examples below are illustrative for the canonical default `(16000, 512)`.

**3-band B/M/T summary** — the cheap roll-up surface kept for kick onset, future energy-envelope inputs, and any consumer that doesn't need richer perceptual detail:

- **Bass**: <250 Hz (bins 0-8 at default operating point). Where kick fundamentals and main basslines live.
- **Mid**: 250 Hz – 2 kHz (bins 8-64 at default). Where the human voice and most melodic instruments are most prominent — the band human ears are most sensitive to.
- **Treble**: 2 kHz – Nyquist (bins 64-256 at default; cut at 8 kHz at the canonical operating point, but ranges higher when the host is running a higher sample rate). Where percussive attacks, vocal presence, and clarity live.

**8-band perceptual summary** — the richer surface taken from the Audible Genius music-production reference, named per the standard music-production convention so future modulators and Diagnostic UI can bind to perceptually meaningful bands without waiting for preset infrastructure:

- **Mud**: 0-20 Hz. Below human hearing threshold; reads near-zero in practice. Carried for completeness so consumers that want it (low-frequency spike detection, sub-sonic content audit) have it without re-architecting later.
- **Sub Bass**: 20-60 Hz. Felt rather than heard; provides energy and bottom-end weight to bass elements.
- **Bass**: 60-250 Hz. Contains fundamental rhythms, kicks, and main basslines.
- **Low Mids**: 250-500 Hz. Houses low-order harmonics and bass presence.
- **Midrange**: 500 Hz – 2 kHz. Where the human voice and trumpet reside; humans most sensitive here.
- **High Mids**: 2 kHz – 4 kHz. Contains percussive attacks and vocal presence; excess causes listening fatigue.
- **Presence**: 4 kHz – 6 kHz. Represents clarity and rhythmic definition.
- **Air**: 6 kHz – 20 kHz. Provides sparkle and shine; where hi-hat shimmer and cymbal sparkle live. **At the canonical 16 kHz sample rate this band is truncated at 8 kHz Nyquist**; running at 48 kHz on capable hosts captures the full perceptual range. Operating-point-dependent: the band field is always populated, but its content quality scales with the host's current sample rate.

**Internal consistency.** The 3-band roll-up is a strict aggregation of the 8-band: `bass_energy = mud + sub_bass + bass; mid_energy = low_mids + midrange; treble_energy = high_mids + presence + air`. Computed once, presented twice. No drift possible.

**Honest limitations at the canonical 16 kHz / 512 default operating point:**

- Mud band reads near-zero (DC + sub-audible content, mostly artefacts).
- Air band is truncated at 8 kHz; full perceptual Air content (8-20 kHz) requires the analyser to be configured to a higher operating point on a host that supports it (S3 at 48 kHz can; Plus2 cannot, by codec limit). Implementation of non-default operating points is reserved for a future Epic — this Epic ships the API surface and the canonical default.

These limitations don't break anything — they just mean the analyser honestly reports what's in the audio it's actually receiving at the operating point it's currently running.

**Why ship 8-band now.** The compute cost is trivial (eight bin-range sums per FFT cycle, all from already-computed magnitudes), the API cost is small (eight extra fields on the band summary struct), and the value to downstream Epics is real: Epic 4.6's Diagnostic UI can render hifi-style 8-band metering immediately, and Epic 4.7's modulators can bind hue/brightness/density to perceptually meaningful bands without preset infrastructure. The named-preset framework (`set_band_layout()`) still ships as a stub for future flexibility, but the two canonical surfaces are always available.

**Future-Epic territory.** The `set_band_layout()` interface stub ships with the default `hifi+production` layout (3-band + 8-band concurrent). Alternative presets — `dnb-4band-with-subbass` (sub-bass + kick band + body + air, weighted for drum & bass), `vocal-emphasis` (narrowed mid-band split around the 500 Hz - 4 kHz vocal-formant region), arbitrary JSON-defined ranges supplied via Config Mode — are reserved for a future Epic. The stub means future-you doesn't re-architect.

## Algorithm choice: sub-band adaptive threshold

The current single-threshold energy detection is a 1990s-era technique that doesn't scale across hardware variants. The proper modern approach for live-music beat detection on microcontrollers is **sub-band energy with adaptive threshold**, which has the following properties:

1. **Divide FFT output into multiple sub-bands** (default 32, logarithmically spaced from ~30Hz to ~10kHz). Each band is independently analysed.
2. **Maintain a per-band rolling history** of recent energy values, typically the last 1 second worth (~40 frames at 40Hz FFT rate). Use a circular buffer.
3. **Compute per-band running mean and variance** continuously across the history window.
4. **Fire a beat candidate** when *any* sub-band's current energy exceeds (mean + k×std_dev) for that band. The constant k is the only true tuning parameter (typically 1.0-2.0).
5. **The threshold is adaptive per band**: quieter bands have correspondingly lower thresholds, louder bands have higher thresholds. The algorithm self-calibrates to the input signal's character without device-specific tuning.
6. **Per-frame refractory period** prevents multiple beat candidates within (e.g.) 80ms of each other. The first candidate wins.
7. **Sub-band selection focuses on bass region** for kick-drum-style beats (sub-bands 0-4 covering ~30-200Hz), which is where most pop/dance music's beat actually lives.

References for the algorithm:

- Patin, F. (2003) *Beat Detection Algorithms*. Available at: [https://www.gamedev.net/tutorials/_/technical/math-and-physics/beat-detection-algorithms-r1952/](https://www.gamedev.net/tutorials/_/technical/math-and-physics/beat-detection-algorithms-r1952/) (Accessed: 8 May 2026).
- Parallelcube (2018) *Beat detection algorithm*. Available at: [https://www.parallelcube.com/2018/03/30/beat-detection-algorithm/](https://www.parallelcube.com/2018/03/30/beat-detection-algorithm/) (Accessed: 8 May 2026).
- Krzyzaniak, M. (2018) *Beat-and-Tempo-Tracking*. GitHub. Available at: [https://github.com/michaelkrzyzaniak/Beat-and-Tempo-Tracking](https://github.com/michaelkrzyzaniak/Beat-and-Tempo-Tracking) (Accessed: 8 May 2026). [Reference for spectral flux as alternative onset measure]

## Algorithm choice: drop and build detection

Drop detection runs as a separate longer-window pass on the same FFT output. Two energy windows are maintained:

- **Short window**: ~2 seconds (80 frames at 40Hz). Mean energy across all bass sub-bands.
- **Long window**: ~10 seconds (400 frames). Same metric over a longer history.

Macro-level events are detected by comparing the two:

- **Drop / build-peak**: short window mean > long window mean × 1.8 (or similar). The energy has just risen significantly above its medium-term baseline, indicating the song has just dropped into a chorus or peak.
- **Breakdown / thin-out**: short window mean < long window mean × 0.4. The energy has just fallen significantly below its baseline, indicating a breakdown or quiet section.
- **Steady-state**: short window mean is comparable to long window. No event fires.

Drop events fire roughly every 8-32 bars in typical pop/dance music, which maps well to musical structure. They're consumed by the effects pipeline as triggers for visually distinctive transitions (whiteouts, palette swaps, brief 100% intensity holds).

### Drop detection pseudocode

```javascript
short_window: circular buffer of 80 frames (2 sec at 40Hz)
long_window: circular buffer of 400 frames (10 sec at 40Hz)
last_drop_time: timestamp of last drop event

on each FFT frame:
  bass_energy = sum of magnitudes in sub-bands 0-4
  push bass_energy onto both short_window and long_window
  short_mean = mean(short_window)
  long_mean = mean(long_window)
  
  ratio = short_mean / long_mean
  
  if ratio > 1.8 and (now - last_drop_time) > 4_seconds:
    fire DROP event
    last_drop_time = now
  elif ratio < 0.4 and (now - last_drop_time) > 4_seconds:
    fire BREAKDOWN event
    last_drop_time = now
```

The 4-second cooldown between events prevents drop events firing in rapid succession during noisy transitions.

## Architectural integration

The new algorithm sits behind the existing `audio_analyser` interface in the orchestration layer. The orchestrator is unaware of which algorithm is in use; it receives `BEAT_DETECTED` events, the new `MUSIC_EVENT` events, and the new `SpectrumFrameEvent` continuous stream at the same conceptual cadence the host's parameters allow.

A new ESP-NOW message type is added: **MUSIC_EVENT (0x06)** carrying a single byte `event_type` (DROP=1, BREAKDOWN=2, BUILD=3 reserved for future use). Existing `BEAT_DETECTED` (0x02) is unchanged. Receivers that don't understand 0x06 simply ignore it (forward-compatible).

The `SpectrumFrameEvent` is *local-only* in this Epic — it is delivered to local subscribers (Diagnostic UI in Epic 4.6, future modulators in Epic 4.7), but is **not** broadcast over ESP-NOW. The 32-band magnitudes at FFT rate would be heavy traffic; Epic 4.7 introduces the rate-limited `MUSIC_DESCRIPTOR (0x09)` message to carry the smaller continuous-descriptor set across the wire. Lumes that need spectrum-derived effects either run their own analyser (Director-class hosts only) or consume the more compact `MUSIC_DESCRIPTOR` stream when 4.7 ships.

The architecture spec needs updates:

- §3.1: declare per-host analyser feature set in the platform tables
- §4.3 Frame format: add MUSIC_EVENT (0x06) message type
- §5 Audio analysis pipeline: replace single-threshold energy detection with sub-band adaptive-threshold + drop detection description; document the capability model and standardised B/M/T ranges; document `set_band_layout()` and the `hifi-3band` default
- §6 Effects catalogue: existing effects continue to fire on BEAT_DETECTED; new effects can additionally bind to MUSIC_EVENT triggers

## Scope

**Included:**

- Sub-band adaptive-threshold beat detection algorithm (replaces existing single-threshold detection)
- Configurable sub-band count (default 32), history window (default 1 second), and threshold multiplier (default 1.5)
- Per-frame refractory period (default 80ms) preventing rapid double-firing
- Drop and breakdown detection on a separate energy-window pass
- New MUSIC_EVENT message type (0x06) in the ESP-NOW protocol
- **Audio analyser capability model**: feature flags declared per host (`analyser.beat_detection`, `analyser.drop_detection`, `analyser.spectrum_frame`, `analyser.band_summary`); future-Epic flags reserved as constants; parameters orthogonal to features (sample rate, FFT size, sub-band count, HW acceleration)
- **Spectrum-frame as a first-class local event**: orchestration subscribes to per-FFT-cycle 32-band magnitudes; not broadcast over ESP-NOW in this Epic
- **B/M/T range audit and restandardisation** to evidence-based ranges (Bass <250Hz, Mid 250Hz-2kHz, Treble 2kHz-8kHz)
- **8-band perceptual summary as a concurrent surface** (Mud / Sub Bass / Bass / Low Mids / Midrange / High Mids / Presence / Air per the Audible Genius reference); computed alongside the 3-band roll-up from the same FFT output, always available, internally consistent (3-band is a strict roll-up of 8-band)
- **`set_band_layout()` interface stub** with default `hifi+production` layout (3-band + 8-band concurrent) — only default implemented; alternative presets (`dnb-4band-with-subbass`, `vocal-emphasis`, custom JSON-defined ranges) reserved for a future Epic, return explicit not-implemented error so future-you knows where to extend
- **`configure_audio_pipeline(sample_rate_hz, fft_size)` interface stub** with `(16000, 512)` as the canonical default operating point — only default implemented; non-default operating points return explicit not-supported. Hosts declare their valid operating points via `audio_pipeline_operating_points` (Plus2 declares one; S3 declares several; future phone/PC HALs declare their own). API is portable across host platforms — no ESP32-specific assumptions in the contract. Band layouts are specified in Hz so they remain valid regardless of operating point; bin assignments are computed at runtime.
- Native unit tests against captured audio samples, including: clean kick drum at 120 BPM, noisy stadium ambient, dance track with drop, ambient track with no obvious beats
- Hardware verification: Plus2 and S3 produce equivalent behavioural output AND declare equivalent capability surfaces (only `fft_hw_accelerated` parameter differs)
- Architecture spec update reflecting the new algorithm and the analyser capability model

**Explicitly excluded:**

- Multi-band onset detection (snare/hi-hat as separate event streams) — Epic 4.7
- Spectral centroid, energy envelope, onset density — Epic 4.7
- Section detection (verse/chorus/build/breakdown labels) — Epic 4.7
- **Spectrum peak-hold tracking and hifi-style on-screen rendering** — orchestration / UI concern; the spectrum-frame surface this Epic ships is consumed by Epic 4.6 (Diagnostic UI rendering) and Epic 4.7 (FX modulators); peak-hold is a presentation concern with consumer-specific decay times and is not part of the analyser
- **Named band-layout presets** beyond the default `hifi+production` layout (e.g., `dnb-4band-with-subbass`, `vocal-emphasis`); custom JSON-defined ranges via Config Mode — future Epic (interface stub lands here so future Epic doesn't re-architect). Note: `production-8band` is *not* future work — it's part of the default layout shipped in this Epic.
- **Implementation of non-default audio pipeline operating points** — the `configure_audio_pipeline()` API ships in this Epic with only the canonical `(16000, 512)` default implemented; lighting up the higher operating points hosts declare (e.g., S3's `(48000, 1024)` for full Air-band capture, `(32000, 1024)` for extended-range studies, `(48000, 2048)` for high-resolution work) is a future Epic. The API surface, operating-point declarations, and band-layout-in-Hz design all ship now so the future Epic is purely an implementation step on a stable interface. Same Epic likely hosts the phone/PC HAL backend the API was designed to be portable to.
- Spectral flux or complex-domain onset detection (more sophisticated than needed; sub-band energy is sufficient)
- Tempo-aware autocorrelation tracking (post-EMF stretch goal if rhythmic accuracy matters)
- Genre-specific tuning profiles (audiences don't perceive these differences strongly enough to justify the complexity)
- Build event detection (fired only on the *peak* of a build; reserved as event_type=3 in the protocol but not implemented in this Epic)
- Audio fingerprinting / song identification (out of scope, different problem)
- ML-based beat detection (overkill for this use case)
- Broadcasting `SpectrumFrameEvent` over ESP-NOW (too heavy at FFT rate; Epic 4.7's compact `MUSIC_DESCRIPTOR` is the right wire surface)

## Acceptance Criteria

- [ ] **(L)** Native unit tests pass against captured audio samples: kick drum at 120 BPM produces ~120 beats/min with <5% miss rate, ambient noise produces <3 false beats/minute, dance track with drop produces 1 DROP event at the correct moment.
- [ ] **(B)** Code builds cleanly under `[env:m5stick-plus2]`, `[env:m5stick-s3]`, and `[env:native]` PlatformIO environments.
- [ ] **(B)** Plus2 and S3 declare identical analyser feature sets at boot: `analyser.beat_detection`, `analyser.drop_detection`, `analyser.spectrum_frame`, `analyser.band_summary` all present; future-Epic flags declared as constants but reporting unsupported. Only the `fft_hw_accelerated` parameter differs between hosts.
- [ ] **(L)** Native test verifies orchestration's `has_capability("analyser.spectrum_frame")` query returns true on both Plus2 and S3 builds.
- [ ] **(L)** Native test verifies a subscriber to `SpectrumFrameEvent` receives 32-band log-spaced magnitudes per FFT cycle with the expected band layout.
- [ ] **(L)** Native test for B/M/T 3-band restandardisation: a 1 kHz sine wave produces near-zero `bass_energy`, dominant `mid_energy`, near-zero `treble_energy`. A 100 Hz sine produces dominant `bass_energy`. A 4 kHz sine produces dominant `treble_energy`.
- [ ] **(L)** Native test for 8-band perceptual summary: a 40 Hz sine produces dominant `sub_bass`; a 150 Hz sine produces dominant `bass`; a 350 Hz sine produces dominant `low_mids`; a 1 kHz sine produces dominant `midrange`; a 3 kHz sine produces dominant `high_mids`; a 5 kHz sine produces dominant `presence`; a 7 kHz sine produces dominant `air`. Mud band reads near-zero on any musical input (verified-empty band per sample-rate constraint).
- [ ] **(L)** Native test for internal consistency: for any input audio, `bass_energy ≈ mud + sub_bass + bass`, `mid_energy ≈ low_mids + midrange`, `treble_energy ≈ high_mids + presence + air` (within floating-point tolerance). The 3-band roll-up is a strict aggregation, not a separate computation.
- [ ] **(B)** `set_band_layout("hifi+production")` is the default at boot; calling with any other preset name returns an explicit not-implemented error so future Epics know where to extend.
- [ ] **(B)** `configure_audio_pipeline(16000, 512)` is the canonical default operating point on both Plus2 and S3 at boot; calling with any other operating point returns an explicit not-supported error. Plus2 declares `audio_pipeline_operating_points = [(16000, 512)]`; S3 declares the longer list (`[(16000, 512), (32000, 1024), (48000, 1024), (48000, 2048)]`) so future-you knows the host capacity is there, even though only the first is lit up in this Epic.
- [ ] **(L)** Native test verifies band-layout correctness is *operating-point-agnostic*: a future operating point of `(48000, 2048)` would, when implemented, produce correct band assignments without API changes. Tested via a stubbed mock operating point that just exercises the bin-mapping computation from Hz boundaries.
- [ ] **(H-Plus2)** Live test with Vengaboys at 138 BPM: beat detection rate matches actual BPM within ±5%, no spurious beats during quiet passages, 1 DROP event fires on the chorus drop.
- [ ] **(H-S3)** Same test on S3: behavioural output equivalent to Plus2 (same beat count, same drop event count, similar timing).
- [ ] **(H)** Cross-device consistency: Plus2 and S3 placed side-by-side listening to the same audio source produce equivalent beat-fire patterns. Subjectively, both "feel right" for the music.
- [ ] **(H)** False-positive rejection: ambient room noise (no music) produces fewer than 3 spurious beats per minute on either device.
- [ ] **(H)** Drop detection: a track with a clear chorus drop (suggested test: any commercial dance track) produces exactly one DROP event at the actual drop moment, not before, not after.
- [ ] **(H)** No regression on existing functionality: the Coldplay tribute act's existing show still works correctly.
- [ ] Architecture spec updated to v0.22 reflecting the new algorithm, the analyser capability model, and the standardised B/M/T ranges.

## Next blocks of work

### Block 1: Capture reference audio samples and write native tests

Before any algorithm work, capture the test data we'll measure against. This makes the rest of the work measurable rather than subjective.

- Record 30-60 seconds each of: clean kick drum at 60/90/120/150 BPM (drum machine output), Vengaboys (canonical test track), a clear-drop dance track, ambient room noise, a podcast (no music, just speech), single sine waves at 100 Hz / 1 kHz / 4 kHz (for B/M/T restandardisation tests).
- Save as 16-bit PCM WAV at the analyser's native sample rate (16 kHz).
- Write native unit tests that load these samples, run them through the audio analyser, and assert beat counts, drop events, and band-summary energies match expected values.
- Tests run under `pio test -e native` and produce concrete pass/fail signal for the algorithm work and the band-layout work.
- Commit: "Reference audio samples and native test harness for analyser work"

### Block 2: Analyser capability surface, restandardised B/M/T, and 8-band perceptual summary

Land the structural foundation before the algorithm swap. Doing this first means the new algorithm slots into a clean interface rather than the existing ad-hoc one.

- Audit current `AudioFrame.bass_energy / mid_energy / treble_energy` bin assignments in HAL backends; restandardise to evidence-based ranges (Bass <250Hz / bins 0-8, Mid 250Hz-2kHz / bins 8-64, Treble 2kHz-8kHz / bins 64-256).
- Add 8-band perceptual summary fields to `AudioFrame`: `mud / sub_bass / bass / low_mids / midrange / high_mids / presence / air`, computed in the same pass over the FFT output. The 3-band roll-up becomes a strict aggregation of the 8-band (`bass_energy = mud + sub_bass + bass`, etc.) so the two surfaces are internally consistent.
- Add `SpectrumFrameEvent` first-class event type carrying all 32 sub-band log-spaced magnitudes per FFT cycle, delivered to local subscribers via the existing `subscribe_events("local", ...)` pattern.
- Declare analyser capability flags in the HAL: `analyser.beat_detection`, `analyser.drop_detection`, `analyser.spectrum_frame`, `analyser.band_summary` lit up on both Plus2 and S3; `analyser.multi_band_onset`, `analyser.spectral_centroid`, `analyser.energy_envelope`, `analyser.section_detection` declared as constants but reporting unsupported.
- Declare analyser parameters: `sample_rate_hz`, `fft_size`, `sub_band_count`, `fft_hw_accelerated` (true on S3, false on Plus2).
- Add `set_band_layout()` method with `hifi+production` default (3-band + 8-band concurrent); non-default preset names return explicit not-implemented error.
- Add `audio_pipeline_operating_points` declaration on each host's HAL (Plus2: one tuple; S3: four tuples). Add `configure_audio_pipeline(sample_rate_hz, fft_size)` setter on the DAL with canonical `(16000, 512)` as the only currently-implemented operating point; non-default tuples return explicit not-supported.
- Specify all band-layout boundaries in Hz, not bin numbers. Bin assignments are computed at runtime from the current operating point. The same code path produces correct bin mappings whether the host is running `(16000, 512)`, `(48000, 1024)`, or any future operating point — no per-operating-point conditionals.
- Native tests verify: capability queries return expected results on both hosts; both 3-band and 8-band summaries produce correct energies for known sine inputs; 3-band roll-up is internally consistent with 8-band aggregation; subscriber to `SpectrumFrameEvent` receives 32 magnitudes per cycle; bin-mapping computation produces correct results for stubbed mock operating points (validates the Hz-first design).
- Commit: "Analyser capability surface: spectrum frame, Hz-first band layouts, 8-band perceptual summary, operating-point API"

### Block 3: Sub-band adaptive-threshold beat detection

Replace the existing single-threshold beat detection with the sub-band adaptive-threshold algorithm, slotting into the capability surface Block 2 just landed.

- Implement 32 sub-bands with logarithmic frequency spacing (already produced by Block 2's `SpectrumFrameEvent`)
- Implement per-band history buffers with running mean and variance
- Implement adaptive threshold (mean + k×std_dev per band)
- Implement refractory period (default 80ms)
- Wire `BEAT_DETECTED` events through the analyser's existing event channel; verify the `analyser.beat_detection` capability flag accurately describes what the host now produces.
- Run native tests; tune k and refractory period until target beat detection rates are met across all reference samples.
- Commit: "Sub-band adaptive-threshold beat detection on the new analyser surface"

### Block 4: Drop and breakdown detection

Add the longer-window energy-comparison pass for macro-level events.

- Implement short window (2 sec) and long window (10 sec) energy histories on the bass sub-bands
- Implement ratio comparison with 4-second cooldown
- Add MUSIC_EVENT message type (0x06) to ESP-NOW protocol per architecture spec §4.3
- Wire up DROP and BREAKDOWN events to fire MUSIC_EVENT frames over ESP-NOW; verify the `analyser.drop_detection` capability flag accurately describes what the host now produces.
- Native test: track with known drop produces exactly one DROP event at the right moment
- Commit: "Drop and breakdown detection with MUSIC_EVENT protocol message"

### Block 5: Hardware cross-device verification

The meaningful empirical test that this Epic exists to pass.

- Set up Plus2 and S3 side-by-side, both in Director mode, listening to the same audio source
- Verify capability surfaces declare identically on both hosts (Block 2's contract holds end-to-end)
- Play the test tracks from Block 1; verify both devices produce equivalent beat counts and drop events
- Tune any per-device sample-rate or normalization quirks until cross-device parity holds
- Document any residual differences in the architecture spec §5
- Commit: "Cross-device verification: Plus2 and S3 produce equivalent analyser surfaces and behavioural output"

### Block 6: Architecture spec update

- Update spec §3.1 platform table to declare per-host analyser feature set
- Update spec §4.3 with MUSIC_EVENT (0x06) message type
- Update spec §5 with: new sub-band adaptive-threshold algorithm description; analyser capability model; standardised B/M/T ranges; `set_band_layout()` interface and `hifi-3band` default
- Update spec §6 with notes on effects that can bind to MUSIC_EVENT triggers
- Bump spec to v0.22
- Commit: "Architecture spec v0.22 reflecting capability-aware analyser and sub-band beat detection"

## Dependencies

| Dependency | Type | Status | Owner |
|---|---|---|---|
| Epic 4 (ESP-NOW transport, basic Director/Lume) | Internal | Done | Jason |
| Architecture spec v0.21+ (§5 Audio analysis pipeline) | Internal | Done | Jason |
| Captured audio samples (or ability to record them) | External | Available (Mac + audio interface) | Jason |

## Status Notes

Proposed 2026-05-08 in response to empirical Plus2/S3 sensitivity divergence observed during Epic 4 implementation. The current single-threshold energy-based detection doesn't scale across hardware variants with different microphone SNR characteristics; the proper fix is a self-calibrating algorithm rather than per-device threshold constants.

This Epic was originally going to be deferred until post-EMF, but the empirical evidence from Epic 4 makes the case for doing it sooner: the cross-device inconsistency is observable and frustrating, and any further work on Epic 4 (including the constellation art piece) will produce visibly inconsistent results on Plus2 vs S3 deployment until this is fixed.

The algorithm choice (sub-band adaptive threshold) is well-tested in the maker community for ESP32 audio work specifically, with multiple reference implementations and documented tuning approaches. Implementation risk is low; tuning effort is the unknown variable.

Processing Type stays Hybrid because algorithm work is well-suited to laptop-driven coding (with native unit tests against captured audio). The exception is Block 5's cross-device verification, which is genuinely Manual - Jason and two devices listening to the same audio source.

**Refinement 2026-05-09**: scope of Block 2 (originally) widened slightly to ensure the audio analyser interface exposes per-band magnitudes as a first-class output, not just beat events. This is a small architectural addition (exposing data Epic 4.5 already computes internally) that unblocks Epic 4.7 (dynamic shows from FFT) without requiring re-architecture.

**Refinement 2026-05-10**: Epic restructured around the analyser's *output surface*, not the algorithm swap. The headline goal becomes "build a capability-aware analyser whose output surface is consistent across hosts, with sub-band adaptive-threshold as the deliverable that gets us there for kick-beats". Three concrete additions:

1. **Audio analyser capability model**: hosts declare flat feature flags (`analyser.beat_detection`, `analyser.drop_detection`, `analyser.spectrum_frame`, `analyser.band_summary`) plus parameters (sample rate, FFT size, sub-band count, HW acceleration). Future Epic flags reserved as constants. Plus2 and S3 declare identical feature surfaces; only `fft_hw_accelerated` differs. Tildagon (no mic) declares no analyser capability.
2. **Spectrum frame as first-class local event**: 32-band log-spaced magnitudes delivered to local subscribers via `SpectrumFrameEvent`. Not broadcast over ESP-NOW (too heavy at FFT rate; Epic 4.7's `MUSIC_DESCRIPTOR` carries the compact wire descriptor when needed). Diagnostic UI in Epic 4.6 and FX modulators in Epic 4.7 are the consumers.
3. **Restandardise B/M/T to evidence-based ranges** (Bass <250Hz, Mid 250Hz-2kHz, Treble 2kHz-8kHz) and **ship the 8-band perceptual summary as a concurrent surface**. The 250 Hz and 2 kHz split-points are the perceptual boundaries between Audible Genius's named music-production bands (Bass→Low Mids, Midrange→High Mids); the 8 kHz cap on Treble/Air at the canonical operating point is a Nyquist constraint at 16 kHz sample rate, not a perceptual choice. The 8-band split (Mud / Sub Bass / Bass / Low Mids / Midrange / High Mids / Presence / Air) ships alongside the 3-band roll-up — both surfaces always available, computed in one pass, internally consistent (3-band is a strict aggregation of 8-band). Compute cost is trivial (eight bin-range sums); value to downstream Epics is real (Diagnostic UI in 4.6 can render 8-band hifi-style metering immediately; modulators in 4.7 can bind to perceptually meaningful bands without preset infrastructure). Add `set_band_layout()` interface stub with `hifi+production` default. Named alternative presets (`dnb-4band-with-subbass`, `vocal-emphasis`) and custom JSON-defined ranges reserved for a future Epic; the stub means future-you doesn't re-architect.

4. **Audio pipeline operating points as a declared, configurable parameter**. Sample rate and FFT size are independent: sample rate controls range (Nyquist), FFT size controls resolution. Hosts declare their valid operating points as a list of `(sample_rate_hz, fft_size)` tuples — Plus2 declares one (codec-limited); S3 declares several (capable of 48 kHz native + larger FFTs); future phone/PC HALs declare their own valid sets. Add `configure_audio_pipeline()` setter on the DAL, stub-implemented with only the canonical `(16000, 512)` default actually working — non-default operating points return explicit not-supported. Band layouts are specified in Hz so they remain valid regardless of operating point; bin assignments computed at runtime. The whole API is host-agnostic — no ESP32-specific assumptions in the contract — so the same surface works for the future phone or PC backend Jason wants to be able to target. Implementation of non-default operating points is a future Epic; this Epic ships the API and the canonical default.

The reframe also clarifies that Epic 4.7 is purely additive on this surface — multi-band onset, centroid, energy envelope, section detection are all *new capability flags* lit up in 4.7, not an interface rewrite. Spec target moves from v0.20 to v0.22 to reflect the wider scope (capability model + standardised ranges, on top of the algorithm swap and MUSIC_EVENT). Effort estimate increases proportionally — Block 2 lands a substantive architectural surface before the algorithm work begins.

The peak-hold tracker discussion (a hifi-style 2-second peak-hold for the spectrum-frame display) was confirmed as orchestration / UI layer concern, not analyser concern. It belongs in Epic 4.6 (Diagnostic UI rendering) or wherever the consumer needs it, with the appropriate decay time for that consumer. Not part of 4.5.
