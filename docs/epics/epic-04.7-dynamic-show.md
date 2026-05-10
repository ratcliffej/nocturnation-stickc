---
title: "Epic 4.7: Dynamic show generation from FFT (multi-band, centroid, energy, sections)"
status: Proposed
notion_url: https://www.notion.so/35cbd067740581feb287ff7023202c19
notion_id: 35cbd067740581feb287ff7023202c19
notion_status: Proposed
last_synced: 2026-05-10
sync_direction: bidirectional
---

## Related Documents

- [NocturNation Architecture Specification](https://www.notion.so/357bd0677405800b891beab0f4e0a976) - particularly §5 (Audio analysis pipeline) and §6 (Effects catalogue)
- [Epic 4.5: Sub-band adaptive-threshold beat detection](https://www.notion.so/35bbd0677405816fa9fbd0306100c794) - upstream Epic; 4.7 builds on the per-band magnitudes 4.5 exposes
- [Epic 4.6: M5 firmware UI cleanup](https://www.notion.so/35cbd067740581e4ba55f79eb168ec9d) - upstream Epic; UI is settled before 4.7 adds new effect parameters
- [Epic 5: Tildagon receiver app](https://www.notion.so/358bd067740581b19551d158d658df76) - downstream Epic; 4.7 ships before 5 to give EMF a properly dynamic show

## Goal

Elevate NocturNation from beat-reactive to **musically responsive**. Use the FFT data already being computed in Epic 4.5 to drive a continuous stream of musical descriptors (multi-band onset, spectral centroid, energy envelope, onset density, section detection) and bind effects to them. The lights stop being a metronome and start tracking the song's emotional arc minute by minute.

A show driven by Epic 4.7 should feel *alive* - colours drift through the palette as the song's tonal character shifts, brightness breathes with volume, build-ups escalate visually before the drop hits, choruses get a denser palette than verses, drum-fills get sharp pulses while sustained chords get smooth fades. Same firmware, same protocol, dramatically richer output.

## Why this Epic exists

During Epic 4.5 design, Jason flagged that simple beat detection leaves most of the FFT's information on the floor: the firmware computes 256 magnitude bins per FFT cycle but only fires events from six of them (bass-band sum). About 2% of available signal is being used. The other 98% encodes genuinely useful information about *what the song is doing right now*, and an audience can feel the difference between lights that flash on kicks versus lights that respond to the music's whole shape.

Epic 4.5 (in flight at time of Epic 4.7's proposal) addresses cross-device beat-detection consistency, which was the immediate blocker. Epic 4.7 takes the per-band magnitudes Epic 4.5 already exposes via the audio analyser interface and uses them properly.

This Epic ships **before Epic 5 (Tildagon)** because EMF 2026 is the project's first major public deployment and the experience visitors take away should be the best NocturNation can offer at the time. Beat-flash is fine; properly dynamic visuals are memorable. The Tildagon receiver app benefits too - it's the receiver of these richer events, so it lands on a stable richer protocol rather than getting upgraded later.

## Operational model

Laptop-driven, same as Epics 1-4.6. Algorithm work is well-suited to native unit testing on captured audio samples; effect-tuning work is genuinely subjective and requires hardware verification with real music in a real listening environment.

Verification ownership: **(L)** = laptop / native test on captured audio, **(B)** = build-time check, **(H)** = hardware verification by Jason listening to music with hardware in hand. The bulk of acceptance work is hardware-and-music verification: the only real test of "does this show feel good?" is playing music through it and watching.

## Algorithm choices

Four algorithmic primitives derived from FFT output that together drive the dynamic show:

### Multi-band onset detection

Extension of Epic 4.5's sub-band adaptive-threshold work. Three independent event streams from three frequency regions:

- **Kick band** (~60-200 Hz, sub-bands 0-4) - already detected in Epic 4.5; produces existing `BEAT_DETECTED` events
- **Snare band** (~200-2000 Hz, sub-bands 5-15) - new; produces a new `SNARE_DETECTED` event type. In typical pop/rock music, snare hits on beats 2 and 4. Detecting kick + snare separately tracks the *groove* not just the *pulse*.
- **Hi-hat band** (~5000-8000 Hz, sub-bands 22-30) - new; produces a new `HIHAT_DETECTED` event type. Hi-hat patterns are usually denser (16th notes) and faster than the underlying beat. Maps well to high-frequency sparkle effects.

Each band uses the same adaptive-threshold algorithm Epic 4.5 establishes; the only differences are which bins contribute, the threshold multiplier (typically lower for hi-hat where transients are smaller), and the refractory period (shorter for hi-hat to allow denser firing).

### Spectral centroid

The "centre of gravity" of the spectrum - a single scalar value per FFT frame indicating where the energy is concentrated.

```javascript
centroid = sum(bin_index * magnitude[bin_index]) / sum(magnitude[bin_index])
```

Low centroid (~5-15 / 256) indicates bass-heavy / muddy passages. High centroid (~40-80 / 256) indicates bright passages with prominent high-frequency content. Mid centroid (~15-40) indicates balanced full-spectrum passages.

The centroid is mapped to **hue** in the orchestration layer:

- Low centroid → cool colours (blue, purple)
- Mid centroid → neutral colours (white, amber, green)
- High centroid → warm colours (red, orange, pink)

This means the lights drift through the colour wheel as the song's tonal character shifts, without reference to beats at all. A Coldplay verse and a Coldplay chorus look visibly different even though the BPM hasn't changed.

### Energy envelope

Rolling RMS across all FFT bins, smoothed over ~0.5-1 second. This is the song's volume contour minus the per-beat transients.

Mapped to **brightness** in the orchestration layer:

- Low energy → dim, ambient lighting
- Mid energy → normal show brightness
- High energy (chorus, drop) → peak brightness

The envelope changes are typically slow (multi-second) and therefore visually subtle, but cumulatively they make the lights feel like they're *part of* the song rather than just superimposed on it.

### Section detection (longer-window analysis)

Rolling 4-8 second analysis of the above three signals, plus onset density, to identify song-structure events.

- **Verse**: low-to-mid energy, low-to-mid centroid, sparse onset density
- **Chorus**: high energy, mid-to-high centroid, mid-to-dense onset density
- **Build-up**: rising energy + rising centroid + rising onset density over 4-8 seconds
- **Drop**: sudden bass-band spike following a build (already detected in Epic 4.5 as DROP event)
- **Breakdown**: low energy, low onset density, sustained for >2 seconds (already detected in Epic 4.5 as BREAKDOWN event)
- **Vocals-only**: mid-band energy, very little bass, low onset density

Section detection produces a new `SECTION_CHANGE` event type carrying the new section label. The orchestration layer uses these to switch palettes, change effect intensity, change which effects are bound to which musical events.

## Architectural integration

Four new event types alongside the existing BEAT_DETECTED and MUSIC_EVENT (DROP/BREAKDOWN):

- **SNARE_DETECTED (0x07)**: payload `strength: u8`. Same shape as BEAT_DETECTED but for snare onsets.
- **HIHAT_DETECTED (0x08)**: payload `strength: u8`. Same shape, for hi-hat onsets.
- **MUSIC_DESCRIPTOR (0x09)**: payload `centroid: u8, energy: u8, density: u8`. Continuous-stream descriptors fired at FFT rate (~30-40 Hz). Heavier traffic than the discrete-event types but each frame is small (3 bytes payload). May be rate-limited on the wire (e.g., fire only when any value changes by >5% from previous transmission) to reduce load.
- **SECTION_CHANGE (0x0A)**: payload `section_type: u8` (1=VERSE, 2=CHORUS, 3=BUILDUP, 4=BREAKDOWN, 5=VOCALS_ONLY, 6=INSTRUMENTAL_BREAK, 0=UNKNOWN). Fires only on transitions between sections, not continuously.

The Effects catalogue (§6 of architecture spec) is extended to support binding effects to these new event types. Existing effect primitives (Pulse, Probability Pulse, Random Palette Pulse, Rainbow/Hue Cycle, Starlight, etc.) gain optional parameter modulators:

- **Hue follows centroid**: any effect that has a colour parameter can opt into having that colour driven by spectral centroid rather than fixed.
- **Brightness follows energy**: any effect that has a brightness parameter can opt into having that brightness driven by energy envelope.
- **Density follows onset density**: probabilistic effects can opt into having their probability driven by onset density.
- **Palette follows section**: any effect that uses a palette can opt into different palettes per section.

A new effect primitive is added:

- **Continuous Wash**: continuous colour driven by centroid, brightness driven by energy. No discrete-event trigger; just a continuous low-key background that responds to the song. Forms the visual baseline that other effects layer over.

## Scope

**Included:**

- Multi-band onset detection (kick + snare + hi-hat as separate event streams) building on Epic 4.5's sub-band infrastructure
- Spectral centroid computation per FFT frame
- Energy envelope computation (rolling RMS, smoothed)
- Onset density tracker (any-band fires per second, smoothed)
- Section detection state machine consuming the above
- Four new ESP-NOW message types: SNARE_DETECTED (0x07), HIHAT_DETECTED (0x08), MUSIC_DESCRIPTOR (0x09), SECTION_CHANGE (0x0A)
- Continuous Wash effect primitive
- Optional parameter modulators on existing effects (hue-from-centroid, brightness-from-energy, density-from-density, palette-from-section)
- Native unit tests against captured audio samples covering all four primitives
- Hardware verification: Plus2 and S3 produce equivalent dynamic-show output for the same input audio
- Architecture spec update reflecting the new descriptors, message types, and effect parameters
- Documentation of the dynamic-show capabilities for the open-source repo's README

**Explicitly excluded:**

- Tempo-aware autocorrelation tracking (post-EMF stretch goal; not needed for visual feel)
- ML-based section recognition (overkill; rule-based is sufficient)
- Genre-specific parameter profiles (audiences don't perceive these strongly enough)
- Audio fingerprinting / song identification (different problem; would belong to the QLC+ Essentia tooling forward direction)
- Pre-analysed cue files inside NocturNation (deprecated when QLC+ became canonical professional path)
- Companion phone-app integration (forward direction in spec §10.3, not a committed Epic)
- New BLE-related work (BLE is its own future Epic per spec §4.1)
- DMX/Art-Net integration (Epic 7, separate)

## Acceptance Criteria

- [ ] **(L)** Native unit tests pass against captured audio samples covering: kick-only drum machine (kick fires, snare and hi-hat don't); standard pop track (kick + snare alternation visible in event stream); track with prominent hi-hats (hi-hat events fire densely without false-firing on bass).
- [ ] **(L)** Spectral centroid output verified against reference Python implementation (librosa) for a known test signal: a sine wave at 100 Hz produces centroid ≈ bin 3, a sine wave at 4000 Hz produces centroid ≈ bin 128, a white noise produces centroid ≈ bin 128 (mid).
- [ ] **(L)** Energy envelope output tracks the volume contour of a known test signal (a song with a clear quiet-loud-quiet structure produces a visible quiet-loud-quiet envelope curve in unit-test output).
- [ ] **(L)** Section detection state machine identifies sections correctly on a labelled test track (manual annotation of verse/chorus/bridge boundaries used as ground truth; >70% accuracy on transition timing within ±2 seconds).
- [ ] **(B)** Code builds cleanly under `[env:m5stick-plus2]`, `[env:m5stick-s3]`, and `[env:native]` PlatformIO environments.
- [ ] **(H)** Live test with Vengaboys at 138 BPM: kick events match BPM, snare events fire on beats 2/4 of each bar, hi-hat events fire densely throughout, centroid drifts visibly across verse/chorus boundaries.
- [ ] **(H)** Live test with a track having a clear build-and-drop: build-up fires SECTION_CHANGE → BUILDUP within the build, drop fires both DROP (from Epic 4.5) and SECTION_CHANGE → CHORUS at the drop moment.
- [ ] **(H)** Cross-device consistency: Plus2 and S3 placed side-by-side produce visually equivalent dynamic-show output. Subjectively, both "feel right" for the music.
- [ ] **(H)** Continuous Wash effect: with no discrete-event triggers, the lights still respond visibly to the song, drifting through the palette as the song's tonal character changes and dimming/brightening with volume.
- [ ] **(H)** Hue-from-centroid modulator: an existing Pulse effect with hue-from-centroid enabled produces beats whose colour reflects the song's current tonal character, not a fixed colour.
- [ ] **(H)** Subjective "feels alive" test: Jason plays a varied playlist (pop, dance, ambient, drum & bass, ballad, drum-machine simple beat) through the system. The lights should produce a visibly different show for each genre without any genre-specific configuration.
- [ ] **(H)** No regression on existing functionality: Epic 4.5's beat detection still works correctly; Epic 4.6's UI is unchanged; the Coldplay tribute act's existing show still works correctly.
- [ ] Architecture spec updated to v0.21 reflecting the new algorithms, message types, and effect modulators.

## Next blocks of work

### Block 1: Multi-band onset detection (snare + hi-hat)

Extend Epic 4.5's sub-band adaptive-threshold infrastructure to produce snare and hi-hat event streams alongside the existing kick stream.

- Identify which sub-bands cover the snare (~200-2000 Hz) and hi-hat (~5000-8000 Hz) ranges given the FFT setup
- Apply the same adaptive-threshold algorithm to each band group, with band-appropriate threshold multipliers and refractory periods
- Add SNARE_DETECTED (0x07) and HIHAT_DETECTED (0x08) message types to the ESP-NOW protocol
- Wire up the events to fire over ESP-NOW alongside existing BEAT_DETECTED
- Native test: pop track with clear kick-snare-kick-snare pattern produces alternating BEAT_DETECTED and SNARE_DETECTED events on beats 1-2-3-4
- Commit: "Multi-band onset detection: snare + hi-hat alongside kick"

### Block 2: Continuous descriptors (centroid + energy + density)

Compute and broadcast continuous-stream descriptors at FFT rate.

- Implement spectral centroid computation in the audio analyser
- Implement rolling-RMS energy envelope with smoothing
- Implement onset-density tracker (events-per-second across all bands)
- Add MUSIC_DESCRIPTOR (0x09) message type carrying centroid + energy + density as 3 bytes
- Implement rate-limiting on the wire (only fire when any value changes by >5% from previous transmission)
- Native test: known-volume test signal produces stable centroid; volume modulation produces energy modulation; onset modulation produces density modulation
- Commit: "Continuous music descriptors over ESP-NOW"

### Block 3: Section detection state machine

The longer-window analysis that produces section labels.

- Implement rolling 4-8 second history buffers for the three continuous descriptors
- Implement a state machine identifying verse / chorus / build-up / breakdown / vocals-only / instrumental-break / unknown
- Tune transition rules using captured audio samples with manual section labels as ground truth
- Add SECTION_CHANGE (0x0A) message type
- Wire up section transitions to fire over ESP-NOW
- Native test: labelled test track produces correct section labels with >70% timing accuracy
- Commit: "Section detection state machine and SECTION_CHANGE protocol message"

### Block 4: Effect modulators

Extend the existing effect primitives to consume the new descriptors.

- Refactor the effect rendering layer to support optional parameter modulators per effect: hue-from-centroid, brightness-from-energy, density-from-density, palette-from-section
- Update existing Pulse, Probability Pulse, Random Palette Pulse, Hue Cycle, Starlight effect implementations to support the modulators
- Add the new Continuous Wash effect primitive
- Update Test Mode (§8.5 of spec) to include tests for each modulator
- Commit: "Effect modulators consuming continuous descriptors"

### Block 5: Hardware verification and tuning

The meaningful empirical work that this Epic exists to pass. Pure listening + watching effort.

- Set up Plus2 and S3 side-by-side in a normal listening environment
- Play a varied test playlist (Vengaboys, Coldplay ballad, drum & bass, dance with build/drop, ambient, drum machine, podcast as silence-test)
- Tune threshold multipliers, smoothing time constants, section-detection rules until each genre produces a visibly distinct and pleasant show
- Document the final tuning parameters in the spec for future reference and to enable contributors to retune for different deployment contexts
- Phone-recorded walkthrough video for the README and demo material
- Commit: "Dynamic show tuning verified across genre playlist"

### Block 6: Architecture spec update

- Update spec §5 (Audio analysis pipeline) with the multi-band, centroid, energy, density, and section detection algorithms
- Update spec §4.3 (Frame format) with the four new message types
- Update spec §6 (Effects catalogue) with the new Continuous Wash primitive and the modulators concept
- Bump spec to v0.21
- Commit: "Architecture spec v0.21 reflecting dynamic show capabilities"

## Dependencies

| Dependency | Type | Status | Owner |
|---|---|---|---|
| Epic 4.5 (sub-band beat detection with per-band magnitudes exposed) | Internal | In Progress | Jason |
| Epic 4.6 (M5 UI cleanup) | Internal | Proposed | Jason |
| Architecture spec v0.20+ (§5 with sub-band adaptive threshold) | Internal | Pending Epic 4.5 ship | Jason |
| Captured audio samples (varied genre playlist) | External | Available (Mac + audio interface) | Jason |
| Labelled test track (for section-detection accuracy testing) | External | To be prepared (manual labelling, ~1 hour) | Jason |

## Status Notes

Proposed 2026-05-09 in response to Jason's observation during Epic 4.5 design that simple beat detection leaves most of the FFT's information on the floor. The architectural unblock (per-band magnitudes exposed via the audio analyser interface) is captured in Epic 4.5 Block 2, so this Epic builds on a stable interface rather than re-architecting.

**Sequencing decision (2026-05-09)**: Epic 4.7 ships **before Epic 5 (Tildagon)** so EMF 2026 gets a properly dynamic show rather than just beat-flash. The trade-off is honest: this puts more work between now and EMF and pushes Tildagon submission later. The plan only fits if EMF 2026 is comfortably more than ~6 months out when Epic 4.5 ships, or if Tildagon submission to EMF 2027 is acceptable as a fallback. If the calendar gets tight during execution, swap 4.7 and 5 in the queue and ship dynamic shows post-EMF. **The walk-before-run discipline still holds: 4.5 → 4.6 → 4.7 → 5.** Don't start 4.7 until 4.5 has shipped and 4.6 is at least planned.

Key technical risk: this Epic genuinely is more substantive than 4.5. Four new event types, four new descriptors, a new effect primitive, modulators on existing effects, plus the section-detection state machine which has the highest tuning effort. Realistic effort estimate is 12-18 hours of focused work plus an open-ended tuning tail. Don't underestimate the tuning tail - the difference between "works" and "feels alive" is hours of subjective listening, not algorithm changes.

**EMF 2026 demo angle**: this Epic's deliverable is what makes the EMF demo *demonstrable*. A single Stick on a tripod running Continuous Wash with hue-from-centroid produces a properly atmospheric installation - the kind of thing visitors stop and watch rather than glance past. That is itself worth EMF for, regardless of whether the Tildagon receiver app ships in time.

Processing Type stays Hybrid because algorithm work is well-suited to laptop-driven coding (with native unit tests), but Block 5's tuning work is genuinely Manual - Jason listening to music with hardware in hand for several hours is the only reliable test for "does this feel right?".
