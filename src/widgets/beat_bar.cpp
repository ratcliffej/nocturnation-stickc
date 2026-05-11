// BeatBarWidget implementation (Epic 4.7 Block 2).
//
// Pre-Block-2 the flux meter rendering lived inside SimpleBeatShow's
// on_render (and AutonomousMasterMode's draw before Block 1). Both
// drew the same frame + fill + threshold-marker pattern with
// hard-coded 220 px width / 14 px height. The widget abstracts the
// geometry so any Show (or Utilities > Level Tuning) can compose the
// bar at arbitrary coordinates.

#include "widgets/beat_bar.h"

#include "dal/dal.h"

namespace nocturnation {
namespace widgets {

namespace {

inline float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

}  // namespace

void BeatBarWidget::update(float bar_fraction, float marker_fraction) {
    bar_fraction_    = clamp01(bar_fraction);
    marker_fraction_ = clamp01(marker_fraction);
}

void BeatBarWidget::draw(int x, int y, int w, int h) const {
    using namespace nocturnation::dal;
    if (w < 4 || h < 4) return;

    // 1 px frame on all four sides.
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        x, y,             w, 1, WHITE});                      // top
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        x, y + h - 1,     w, 1, WHITE});                      // bottom
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        x, y,             1, h, WHITE});                      // left
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        x + w - 1, y,     1, h, WHITE});                      // right

    // Fill bar inside the frame (1 px inset on each side).
    const int inner_w = w - 2;
    const int bar_w   = static_cast<int>(bar_fraction_ * static_cast<float>(inner_w));
    if (bar_w > 0) {
        DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
            x + 1, y + 1, bar_w, h - 2, GREEN});
    }

    // Threshold marker: 1 px vertical line that extends 2 px above
    // and below the frame so it remains visible when the fill bar
    // overlaps it.
    if (marker_fraction_ > 0.0f) {
        const int marker_x = x + static_cast<int>(
            marker_fraction_ * static_cast<float>(inner_w));
        if (marker_x >= x && marker_x < x + w) {
            DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
                marker_x, y - 2, 1, h + 4, RED});
        }
    }
}

}  // namespace widgets
}  // namespace nocturnation
