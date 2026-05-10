# Reference audio samples

This directory holds **captured reference recordings** that the audio analyser
tests measure against. The contents are large binaries; they're gitignored
and reproduced from a documented capture protocol below.

The synthetic test signals (sine waves, kick trains at known BPM, white
noise, silence) are **not** here - they're generated in code from the
[test_audio_pipeline](../test_audio_pipeline/) sources so they version-control
naturally and a fresh checkout can run a meaningful subset of the analyser
tests without downloading anything.

This directory is for the things that genuinely need to be real-world audio:
recognisable songs, room ambient noise, speech samples with realistic
microphone characteristics.

## File catalogue

The Epic 4.5 acceptance criteria reference these samples by name. Place each
recording at the listed path before running the relevant tests; the tests
will skip with an explicit "sample not present" message when the file is
missing, rather than fail.

| Path | Source | Format | Expected analyser output |
|---|---|---|---|
| `kick_60bpm.wav`  | drum machine, single kick at 60 BPM, 30-60 s | 16-bit PCM mono, 16 kHz | ~30-60 BEAT_DETECTED, no DROP, no false fires in gaps |
| `kick_90bpm.wav`  | drum machine, single kick at 90 BPM, 30-60 s | 16-bit PCM mono, 16 kHz | ~45-90 BEAT_DETECTED |
| `kick_120bpm.wav` | drum machine, single kick at 120 BPM, 30-60 s | 16-bit PCM mono, 16 kHz | ~60-120 BEAT_DETECTED |
| `kick_150bpm.wav` | drum machine, single kick at 150 BPM, 30-60 s | 16-bit PCM mono, 16 kHz | ~75-150 BEAT_DETECTED |
| `vengaboys.wav`   | "We Like To Party" (138 BPM) full track | 16-bit PCM mono, 16 kHz | beat rate matches BPM ±5%, exactly 1 DROP at the chorus drop |
| `dance_drop.wav`  | any commercial dance track with a clean chorus drop | 16-bit PCM mono, 16 kHz | exactly 1 DROP at the drop moment |
| `ambient_room.wav` | room tone with no music, 60 s | 16-bit PCM mono, 16 kHz | <3 BEAT_DETECTED per minute (false-positive rate) |
| `podcast.wav`     | speech-only podcast clip, 30 s | 16-bit PCM mono, 16 kHz | <3 BEAT_DETECTED per minute |

## Capture protocol

Recommended flow (Mac + audio interface):

1. Record into Audacity (or any DAW) at native sample rate (typically 48 kHz).
2. Convert to mono if the source is stereo (Audacity: Tracks → Mix → Mix Stereo Down to Mono).
3. Resample to 16 kHz (Audacity: Tracks → Resample).
4. Export as WAV, **PCM 16-bit signed**. Other encodings (24-bit, 32-bit float, IEEE float, A-law, etc.) won't load - the WAV reader rejects them deliberately so a misconfigured export fails loud rather than silently producing wrong analyser output.
5. Drop into this directory at the path shown in the catalogue.

Higher sample rates are not currently supported by the analyser's canonical
operating point (`16000, 512`); future Epics may unlock 48 kHz recordings on
hosts that can run the higher operating point. For now, 16 kHz mono is the
contract.

## Why these are gitignored

These recordings include third-party content (Vengaboys, dance track) whose
licence does not permit redistribution as part of this repository. The
synthetic kick patterns could in principle be committed, but for consistency
the whole directory stays gitignored - if you want a reproducible drum-machine
sample, generate one with `generate_kick_train()` rather than depending on a
binary in tree.
