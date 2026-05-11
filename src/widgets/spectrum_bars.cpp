// SpectrumBarsWidget implementation (Epic 4.7 Block 2).
//
// Direct lift of the rendering / band-roll-up logic from the Epic 4.6
// Block 11 SpectrumBarsVisualisation, refactored into a reusable
// library widget. The constants (band map, labels, colours, log-mag
// floor / sensitivity scale) match the pre-Block-2 vis byte-for-byte
// so an operator sees no change in the bars region.

#include "widgets/spectrum_bars.h"

#include "dal/dal.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace nocturnation {
namespace widgets {

namespace {

using namespace nocturnation::dal;

// =============================================================================
// Perceptual band layout (7 bands, Audible Genius reference)
// =============================================================================

enum class Perceptual : uint8_t {
    SubBass = 0, Bass, LowMids, Midrange, HighMids, Presence, Air
};

constexpr size_t kSpectrumBandsRaw = 32;

// Hard-coded 32-band-log -> 7-perceptual mapping for the canonical
// 16 kHz / 32 log-spaced bands operating point. See spectrum_bars.cpp
// (pre-Block-2 vis) for the centre-frequency derivation.
constexpr Perceptual kBandMap[kSpectrumBandsRaw] = {
    Perceptual::SubBass, Perceptual::SubBass, Perceptual::SubBass, Perceptual::SubBass,
    Perceptual::Bass,    Perceptual::Bass,    Perceptual::Bass,    Perceptual::Bass,
    Perceptual::Bass,    Perceptual::Bass,    Perceptual::Bass,    Perceptual::Bass,
    Perceptual::LowMids, Perceptual::LowMids, Perceptual::LowMids, Perceptual::LowMids,
    Perceptual::Midrange,Perceptual::Midrange,Perceptual::Midrange,Perceptual::Midrange,
    Perceptual::Midrange,Perceptual::Midrange,Perceptual::Midrange,Perceptual::Midrange,
    Perceptual::HighMids,Perceptual::HighMids,Perceptual::HighMids,Perceptual::HighMids,
    Perceptual::Presence,Perceptual::Presence,Perceptual::Air,     Perceptual::Air,
};

const char* const kPerceptualLabels[kSpectrumBandCount] = {
    "Sub", "Bass", "Lows", "Mid", "Hi", "Pres", "Air"
};

// RGB565 colour per band - warm-to-cool rainbow mapping low frequencies
// to warm and high frequencies to cool. Matches the pre-Block-2 vis.
constexpr uint16_t kPerceptualColours[kSpectrumBandCount] = {
    0x801F,   // Sub Bass  - deep purple
    0xF800,   // Bass      - red
    0xFB60,   // Low Mids  - orange
    0xFFE0,   // Midrange  - yellow
    0x07E0,   // High Mids - green
    0x07FF,   // Presence  - cyan
    0x051F,   // Air       - light blue
};

// Magnitude-to-bar-height calibration. Matches the pre-Block-2 vis:
// log_mag = log2(1 + raw); v = (log_mag - kMagFloorLog2) * sens * kSensScale.
constexpr float kMagFloorLog2 = 13.0f;
constexpr float kSensScale    = 0.025f;

inline float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

}  // namespace

void SpectrumBarsWidget::update(const float values[kSpectrumBandCount]) {
    for (size_t i = 0; i < kSpectrumBandCount; ++i) {
        values_[i] = clamp01(values[i]);
    }
}

void SpectrumBarsWidget::roll_up_spectrum_to_perceptual(
        const float* magnitudes_32band,
        uint8_t      sensitivity,
        float        out_values_7band[kSpectrumBandCount]) {
    // Sum raw magnitudes by perceptual band.
    float band_sums[kSpectrumBandCount] = {0, 0, 0, 0, 0, 0, 0};
    if (magnitudes_32band) {
        for (size_t i = 0; i < kSpectrumBandsRaw; ++i) {
            band_sums[static_cast<size_t>(kBandMap[i])] += magnitudes_32band[i];
        }
    }

    const uint8_t sens     = (sensitivity == 0) ? 1 : sensitivity;
    const float   sens_f   = static_cast<float>(sens);

    for (size_t b = 0; b < kSpectrumBandCount; ++b) {
        const float log_mag = std::log2(1.0f + band_sums[b]);
        float v = (log_mag - kMagFloorLog2) * sens_f * kSensScale;
        out_values_7band[b] = clamp01(v);
    }
}

void SpectrumBarsWidget::draw(int x, int y, int w, int h) const {
    using namespace nocturnation::dal;
    if (w < 24 || h < 16) return;

    // Reserve a label strip at the bottom (10 px is enough for size-1
    // text at ~7 px ascender + 3 px descender / leading).
    constexpr int kLabelStripH = 14;
    const int bars_h    = h - kLabelStripH;
    if (bars_h < 4) return;

    const int label_y   = y + h - kLabelStripH + 2;
    const int bars_top  = y;
    const int bars_bot  = y + bars_h;

    // Layout: bar_width such that 7 bars + 6 gaps fits w. Floor to int.
    constexpr int kBarGap = 4;
    const int bar_w = (w - (static_cast<int>(kSpectrumBandCount) - 1) * kBarGap)
                      / static_cast<int>(kSpectrumBandCount);
    if (bar_w < 2) return;

    // Clear the bars + label strip with a black background so leftover
    // pixels from a prior draw don't ghost behind the new bars. Cheaper
    // than letting the caller clear; widgets like this are typically
    // the dominant content of their region.
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        x, y, w, h, BLACK});

    for (size_t b = 0; b < kSpectrumBandCount; ++b) {
        const int bar_x = x + static_cast<int>(b) * (bar_w + kBarGap);
        const int bar_h = static_cast<int>(values_[b] * static_cast<float>(bars_h));

        // 2 px floor so the colour bands are visible even on silence
        // (acts as a legend / sanity check that the widget is alive).
        constexpr int floor_h = 2;
        const int floor_y     = bars_bot - floor_h;
        DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
            bar_x, floor_y, bar_w, floor_h, kPerceptualColours[b]});

        if (bar_h > floor_h) {
            const int top_y = bars_bot - bar_h;
            DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
                bar_x, top_y, bar_w, bar_h - floor_h, kPerceptualColours[b]});
        }

        // Label centred-ish under each bar. Size-1 chars are ~6 px;
        // truncate to fit bar_w if necessary.
        constexpr int kCharW = 6;
        const int max_chars = bar_w / kCharW;
        char buf[8] = {0};
        const size_t label_len = std::strlen(kPerceptualLabels[b]);
        const size_t take = (max_chars > 0
                              && static_cast<size_t>(max_chars) < label_len)
                              ? static_cast<size_t>(max_chars)
                              : label_len;
        std::memcpy(buf, kPerceptualLabels[b], take);
        buf[take] = '\0';

        const int label_x = bar_x + (bar_w - static_cast<int>(take) * kCharW) / 2;
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            label_x, label_y, buf, kPerceptualColours[b], BLACK, 1});
    }

    // Suppress unused-warning on bars_top - it's logically used by the
    // bars_bot computation but the compiler is sensitive in some envs.
    (void)bars_top;
}

}  // namespace widgets
}  // namespace nocturnation
