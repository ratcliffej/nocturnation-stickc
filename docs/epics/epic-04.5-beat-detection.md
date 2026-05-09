---
title: "Epic 4.5: Sub-band adaptive-threshold beat detection with drop detection"
status: Proposed
notion_url: https://www.notion.so/35bbd0677405816fa9fbd0306100c794
notion_id: 35bbd0677405816fa9fbd0306100c794
notion_status: Proposed
last_synced: 2026-05-09
sync_direction: bidirectional
---

## Related Documents

- [NocturNation Architecture Specification](https://www.notion.so/357bd0677405800b891beab0f4e0a976) - particularly §5 (Audio analysis pipeline) and §6 (Effects catalogue)
- [Epic 4: ESP-NOW transport](https://www.notion.so/358bd067740581e3afa8fd061b821638) - this Epic depends on Epic 4 having shipped basic ESP-NOW so beats are actually broadcastable

## Goal

Replace the current single-threshold energy-based beat detection with a **sub-band adaptive-threshold** algorithm that produces consistent behavioural output across hardware variants (Plus2 and S3) regardless of microphone SNR or noise floor differences. Add **drop and build detection** as a longer-window energy-comparison pass on the same FFT output, providing a `MUSIC_EVENT` message type alongside the existing `BEAT_DETECTED`.

## Why this Epic exists

During Epic 4 implementation, beat detection sensitivity diverged between Plus2 and S3:

- Plus2 felt **too unreactive** - real beats with energy similar to ambient noise failed to cross the threshold
- S3 felt **too sensitive** - ambient transients (claps, door slams, keyboard clicks) crossed the threshold and triggered false beats

The root cause is genuinely hardware-driven: the Plus2's PDM mic has substantially poorer SNR than the S3's ES8311 codec + MEMS mic combination. Same algorithm + worse hardware = missed beats; same algorithm + cleaner hardware = false positives. A single-threshold approach inherently doesn't scale across this kind of hardware variation.

The lazy fix would be per-device threshold constants. The proper fix is an algorithm that *self-calibrates to its input signal*, producing equivalent behavioural output regardless of input character. This Epic delivers the proper fix, and is the right size for its own focused work session rather than a hack inside Epic 4.

## Operational model

Laptop-driven, same as Epics 1-4. Algorithm work is genuinely well-suited to native unit testing on Jason's laptop using captured audio samples; hardware verification then confirms behavioural parity across Plus2 and S3.

Verification ownership: **(L)** = laptop / native test on captured audio, **(B)** = build-time check, **(H)** = hardware verification by Jason, with the additional convention **(H-Plus2)** / **(H-S3)** when a hardware test must specifically be performed on each device.

## Algorithm choice: sub-band adaptive threshold

The current single-threshold energy detection is a 1990s-era technique that doesn't scale across hardware variants. The proper modern approach for live-music beat detection on microcontrollers is **sub-band energy with adaptive threshold**, which has the following properties:

1. **Divide FFT output into multiple sub-bands** (typically 32 or 64 bands, logarithmically spaced from ~30Hz to ~10kHz). Each band is independently analysed.
2. **Maintain a per-band rolling history** of recent energy values, typically the last 1 second worth (~40 frames at 40Hz FFT rate). Use a circular buffer.
3. **Compute per-band running mean and variance** continuously across the history window.
4. **Fire a beat candidate** when *any* sub-band's current energy exceeds (mean + k×std_dev) for that band. The constant k is the only true tuning parameter (typically 1.0-2.0).
5. **The threshold is adaptive per band**: quieter bands have correspondingly lower thresholds, louder bands have higher thresholds. The algorithm self-calibrates to the input signal's character without device-specific tuning.
6. **Per-frame refractory period** prevents multiple beat candidates within (e.g.) 80ms of each other. The first candidate wins.
7. **Sub-band selection focuses on bass region** for kick-drum-style beats (sub-bands 0-4 covering ~30-200Hz), which is where most pop/dance music's beat actually lives.

References for the algorithm:

- Patin, F. (2003) *Beat Detection Algorithms*. Available at: <https://www.gamedev.net/tutorials/_/technical/math-and-physics/beat-detection-algorithms-r1952/> (Accessed: 8 May 2026).
- Parallelcube (2018) *Beat detection algorithm*. Available at: <https://www.parallelcube.com/2018/03/30/beat-detection-algorithm/> (Accessed: 8 May 2026).
- Krzyzaniak, M. (2018) *Beat-and-Tempo-Tracking*. GitHub. Available at: <https://github.com/michaelkrzyzaniak/Beat-and-Tempo-Tracking> (Accessed: 8 May 2026). [Reference for spectral flux as alternative onset measure]

## Algorithm choice: drop and build detection

Drop detection runs as a separate longer-window pass on the same FFT output. Two energy windows are maintained:

- **Short window**: ~2 seconds (80 frames at 40Hz). Mean energy across all bass sub-bands.
- **Long window**: ~10 seconds (400 frames). Same metric over a longer history.

Macro-level events are detected by comparing the two:

- **Drop / build-peak**: short window mean > long window mean × 1.8 (or similar). The energy has just risen significantly above its medium-term baseline, indicating the song has just dropped into a chorus or peak.
- **Breakdown / thin-out**: short window mean < long window mean × 0.4. The energy has just fallen significantly below its baseline, indicating a breakdown or quiet section.
- **Steady-state**: short window mean is comparable to long window. No event fires.

Drop events fire roughly every 8-32 bars in typical pop/dance music, which maps well to musical structure. They're consumed by the effects pipeline as triggers for visually distinctive transitions (whiteouts, palette swaps, brief 100% intensity holds).

## Drop detection pseudocode

```
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

The new algorithm sits behind the existing `audio_analyser` interface in the orchestration layer. The orchestrator is unaware of which algorithm is in use; it receives `BEAT_DETECTED` events and the new `MUSIC_EVENT` events at the same conceptual rate.

A new ESP-NOW message type is added: **MUSIC_EVENT (0x06)** carrying a single byte `event_type` (DROP=1, BREAKDOWN=2, BUILD=3 reserved for future use). Existing `BEAT_DETECTED` (0x02) is unchanged. Receivers that don't understand 0x06 simply ignore it (forward-compatible).

The architecture spec needs a small update:

- §5 Audio analysis pipeline: replace single-threshold energy detection with sub-band adaptive-threshold + drop detection description
- §4.3 Frame format: add MUSIC_EVENT message type
- §6 Effects catalogue: existing effects continue to fire on BEAT_DETECTED; new effects can additionally bind to MUSIC_EVENT triggers

## Scope

**Included:**

- Sub-band adaptive-threshold beat detection algorithm (replaces existing single-threshold detection)
- Configurable sub-band count (default 32), history window (default 1 second), and threshold multiplier (default 1.5)
- Per-frame refractory period (default 80ms) preventing rapid double-firing
- Drop and breakdown detection on a separate energy-window pass
- New MUSIC_EVENT message type (0x06) in the ESP-NOW protocol
- Native unit tests against captured audio samples, including: clean kick drum at 120 BPM, noisy stadium ambient, dance track with drop, ambient track with no obvious beats
- Hardware verification: Plus2 and S3 produce equivalent behavioural output for the same input audio (within reasonable tolerance)
- Architecture spec update reflecting the new algorithm

**Explicitly excluded:**

- Spectral flux or complex-domain onset detection (more sophisticated than needed; sub-band energy is sufficient)
- Tempo-aware autocorrelation tracking (post-EMF stretch goal if rhythmic accuracy matters)
- Genre-specific tuning profiles (audiences don't perceive these differences strongly enough to justify the complexity)
- Build event detection (fired only on the *peak* of a build; reserved as event_type=3 in the protocol but not implemented in this Epic)
- Audio fingerprinting / song identification (out of scope, different problem)
- ML-based beat detection (overkill for this use case)

## Acceptance Criteria

- [ ] **(L)** Native unit tests pass against captured audio samples: kick drum at 120 BPM produces ~120 beats/min with <5% miss rate, ambient noise produces <3 false beats/minute, dance track with drop produces 1 DROP event at the correct moment.
- [ ] **(B)** Code builds cleanly under `[env:m5stick-plus2]`, `[env:m5stick-s3]`, and `[env:native]` PlatformIO environments.
- [ ] **(H-Plus2)** Live test with Vengaboys at 138 BPM: beat detection rate matches actual BPM within ±5%, no spurious beats during quiet passages, 1 DROP event fires on the chorus drop.
- [ ] **(H-S3)** Same test on S3: behavioural output equivalent to Plus2 (same beat count, same drop event count, similar timing).
- [ ] **(H)** Cross-device consistency: Plus2 and S3 placed side-by-side listening to the same audio source produce equivalent beat-fire patterns. Subjectively, both "feel right" for the music.
- [ ] **(H)** False-positive rejection: ambient room noise (no music) produces fewer than 3 spurious beats per minute on either device.
- [ ] **(H)** Drop detection: a track with a clear chorus drop (suggested test: any commercial dance track) produces exactly one DROP event at the actual drop moment, not before, not after.
- [ ] **(H)** No regression on existing functionality: the Coldplay tribute act's existing show still works correctly.
- [ ] Architecture spec updated to v0.20 reflecting the new algorithm and protocol additions.

## Next blocks of work

### Block 1: Capture reference audio samples and write native tests

Before any algorithm work, capture the test data we'll measure against. This makes the rest of the work measurable rather than subjective.

- Record 30-60 seconds each of: clean kick drum at 60/90/120/150 BPM (drum machine output), Vengaboys (canonical test track), a clear-drop dance track, ambient room noise, a podcast (no music, just speech).
- Save as 16-bit PCM WAV at the Plus2's native sample rate (typically 8kHz).
- Write native unit tests that load these samples, run them through the audio analyser, and assert beat counts and drop events match expected values.
- Tests run under `pio test -e native` and produce concrete pass/fail signal for the algorithm work.
- Commit: "Reference audio samples and native test harness for beat detection"

### Block 2: Sub-band adaptive-threshold beat detection

Replace the existing single-threshold beat detection with the sub-band adaptive-threshold algorithm.

- Refactor the audio analyser HAL interface if needed to expose per-band magnitudes rather than just bass energy
- Implement 32 sub-bands with logarithmic frequency spacing
- Implement per-band history buffers with running mean and variance
- Implement adaptive threshold (mean + k×std_dev per band)
- Implement refractory period
- Run native tests; tune k and refractory period until target beat detection rates are met
- Commit: "Sub-band adaptive-threshold beat detection"

### Block 3: Drop and breakdown detection

Add the longer-window energy-comparison pass for macro-level events.

- Implement short window (2 sec) and long window (10 sec) energy histories
- Implement ratio comparison with 4-second cooldown
- Add MUSIC_EVENT message type (0x06) to ESP-NOW protocol per architecture spec §4.3
- Wire up DROP and BREAKDOWN events to fire MUSIC_EVENT frames over ESP-NOW
- Native test: track with known drop produces exactly one DROP event at the right moment
- Commit: "Drop and breakdown detection with MUSIC_EVENT protocol message"

### Block 4: Hardware cross-device verification

The meaningful empirical test that this Epic exists to pass.

- Set up Plus2 and S3 side-by-side, both in Master mode, listening to the same audio source
- Play the test tracks from Block 1; verify both devices produce equivalent beat counts and drop events
- Tune any per-device sample-rate or normalization quirks until cross-device parity holds
- Document any residual differences in the architecture spec §5
- Commit: "Cross-device verification: Plus2 and S3 produce equivalent behavioural output"

### Block 5: Architecture spec update

- Update spec §5 with the new algorithm description
- Update spec §4.3 with MUSIC_EVENT message type
- Update spec §6 with notes on effects that can bind to MUSIC_EVENT triggers
- Bump spec to v0.20
- Commit: "Architecture spec v0.20 reflecting new beat detection algorithm"

## Dependencies

| Dependency | Type | Status | Owner |
| --- | --- | --- | --- |
| Epic 4 (ESP-NOW transport, basic Master/Slave) | Internal | In Progress | Jason |
| Architecture spec v0.19+ (§5 Audio analysis pipeline) | Internal | Done | Jason |
| Captured audio samples (or ability to record them) | External | Available (Mac + audio interface) | Jason |

## Status Notes

Proposed 2026-05-08 in response to empirical Plus2/S3 sensitivity divergence observed during Epic 4 implementation. The current single-threshold energy-based detection doesn't scale across hardware variants with different microphone SNR characteristics; the proper fix is a self-calibrating algorithm rather than per-device threshold constants.

This Epic was originally going to be deferred until post-EMF, but the empirical evidence from Epic 4 makes the case for doing it sooner: the cross-device inconsistency is observable and frustrating, and any further work on Epic 4 (including the constellation art piece) will produce visibly inconsistent results on Plus2 vs S3 deployment until this is fixed.

The algorithm choice (sub-band adaptive threshold) is well-tested in the maker community for ESP32 audio work specifically, with multiple reference implementations and documented tuning approaches. Implementation risk is low; tuning effort is the unknown variable.

Processing Type stays Hybrid because algorithm work is well-suited to laptop-driven coding (with native unit tests against captured audio). The exception is Block 4's cross-device verification, which is genuinely Manual - Jason and two devices listening to the same audio source.
