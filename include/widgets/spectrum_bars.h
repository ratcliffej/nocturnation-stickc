// SpectrumBarsWidget - a 7-band perceptual spectrum visualisation
// (Epic 4.7 Block 2).
//
// Library widget. Shows can compose it inside on_render() and
// `ConfigMode > Utilities > Level Tuning` hosts it standalone. Bands
// are the music-production perceptual layout (Sub Bass / Bass / Low
// Mids / Midrange / High Mids / Presence / Air) per the Audible
// Genius reference used throughout Epic 4.5+.
//
// Pre-Block-2 this lived inside SpectrumBarsVisualisation (Epic 4.6
// Block 11). The widget takes the same 7-band float values it
// previously computed internally; callers that have raw 32-band log-
// spectrum data can use the roll_up_spectrum_to_perceptual() helper
// to convert.

#pragma once

#include <cstddef>
#include <cstdint>

namespace nocturnation {
namespace widgets {

constexpr size_t kSpectrumBandCount = 7;

class SpectrumBarsWidget {
public:
    SpectrumBarsWidget() = default;

    // Update the 7 perceptual band values. Each value is a fraction
    // in [0.0, 1.0] - the bar height as a fraction of the available
    // height. Values outside the range are clamped.
    void update(const float values[kSpectrumBandCount]);

    // Render the widget at the given screen rectangle. The widget
    // claims (x, y, w, h); within that it paints 7 bars + labels.
    // Minimum sensible dimensions: w >= 60 (so the labels fit at the
    // smallest band size), h >= 24 (room for bars + label row).
    void draw(int x, int y, int w, int h) const;

    // Helper: roll up a 32-band log-spectrum frame into 7 perceptual
    // values with the given sensitivity (1..255). The output is in
    // [0..1] - ready to pass into update().
    //
    // sensitivity 0 is clamped to 1 (a 0 sensitivity would silence
    // the widget; not a useful operating point). The default
    // operating point matches the pre-Block-2 SpectrumBarsVisualisation
    // "sensitivity" property default of 5 in its 1..10 range; values
    // above 10 progressively over-saturate but stay clamped to the
    // [0, 1] band-value cap in update().
    static void roll_up_spectrum_to_perceptual(
        const float* magnitudes_32band,
        uint8_t      sensitivity,
        float        out_values_7band[kSpectrumBandCount]);

    // Test accessor.
    float band_value(size_t i) const {
        return (i < kSpectrumBandCount) ? values_[i] : 0.0f;
    }

private:
    float values_[kSpectrumBandCount] = {0, 0, 0, 0, 0, 0, 0};
};

}  // namespace widgets
}  // namespace nocturnation
