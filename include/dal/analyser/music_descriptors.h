// Continuous music descriptors: centroid, energy envelope, onset
// density (Epic 4.7 Block 3).
//
// Pure analyser class - no HAL, no globals. Takes one spectrum frame
// plus overall_rms plus a boolean "any onset fired this frame" flag,
// updates its smoothing state, and exposes three uint8_t descriptors
// for downstream consumers:
//
//   centroid - "centre of gravity" of the spectrum in 0..255. Low =
//              bass-heavy; mid = balanced; high = bright. No
//              smoothing - the per-frame value is the descriptor.
//   energy   - smoothed RMS envelope in 0..255. Tracks the song's
//              volume contour minus per-beat transients. Smoothing
//              constant ~0.5 s.
//   density  - events-per-second across all onset bands (kick +
//              snare + hi-hat combined), windowed over the last
//              1 s, in 0..255. Tracks how busy the music is.
//
// Block 3 fires the descriptors on every frame; rate-limited delivery
// to the Show hook is done at the consumer (DirectorMode)
// where the wire / Show-event contract lives.

#pragma once

#include "dal/analyser/audio_analyser.h"

#include <cstdint>
#include <cstddef>

namespace nocturnation {
namespace dal {
namespace analyser {

class MusicDescriptors {
public:
    MusicDescriptors() = default;

    // Run one update for the current frame. spec carries the 32-band
    // spectrum (used for centroid). overall_rms is the frame's RMS
    // amplitude (used for energy envelope). any_event is true if any
    // of the onset detectors fired on this frame (used for density).
    // now_ms is the frame's wall time; older events outside the 1 s
    // window are pruned from the density buffer.
    void process(const SpectrumFrame& spec,
                  float                overall_rms,
                  bool                 any_event,
                  uint32_t             now_ms);

    // Reset all smoothing state. Used at mode transitions so a stale
    // history doesn't bias the readings on resume.
    void reset();

    // Most recently computed descriptors.
    uint8_t centroid() const { return centroid_; }
    uint8_t energy()   const { return energy_; }
    uint8_t density()  const { return density_; }

    // Density window in milliseconds. Events older than this in the
    // ring buffer don't count toward the per-second rate. Exposed as
    // a constant so tests can advance time deterministically.
    static constexpr uint32_t kDensityWindowMs = 1000;

    // Density saturation point: events per second that maps to a
    // descriptor value of 255. 16 events/s ≈ 16th notes at 240 BPM,
    // covering the busiest typical drum patterns. Block 6 tuning may
    // refine this.
    static constexpr float    kDensityMaxEvents = 16.0f;

    // Energy log floor / span (log2 of RMS). Silence reads ~log2(500) ≈ 9;
    // loud music reads ~log2(15000) ≈ 13.9. The 9..14 range maps to
    // 0..255 with linear quantisation. Tuned to match the existing
    // SimpleBeatShow volume-gate (500) so a descriptor of 0 corresponds
    // to "below the gate".
    static constexpr float    kEnergyLog2Floor = 9.0f;
    static constexpr float    kEnergyLog2Span  = 5.0f;

    // Energy smoothing alpha (single-pole IIR). 0.05 per frame at
    // 40 Hz FFT ≈ 0.5 s time constant.
    static constexpr float    kEnergyAlpha     = 0.05f;

private:
    // Density ring buffer. Stores up to kDensityRingCap event
    // timestamps; older entries are pruned per-frame.
    static constexpr size_t   kDensityRingCap = 32;
    uint32_t  event_ts_[kDensityRingCap] = {0};
    size_t    event_count_  = 0;          // entries 0..event_count_-1 valid
    size_t    event_head_   = 0;          // next slot to write (oldest if full)

    float     energy_smoothed_ = 0.0f;

    uint8_t   centroid_ = 0;
    uint8_t   energy_   = 0;
    uint8_t   density_  = 0;
};

}  // namespace analyser
}  // namespace dal
}  // namespace nocturnation
