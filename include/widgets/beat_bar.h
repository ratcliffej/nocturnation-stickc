// BeatBarWidget - a horizontal level bar with an optional threshold
// marker (Epic 4.7 Block 2).
//
// Library widget, not a plug-in: Shows compose it inside their
// on_render() and `ConfigMode > Utilities > Level Tuning` hosts it
// standalone for bench work. The pre-Block-1 flux meter that lived
// inside AutonomousMasterMode (and then SimpleBeatShow's on_render
// from Block 1) is replaced by this widget; the visible shape stays
// identical so operators see no change.
//
// The widget is pure render: callers compute their own fraction
// (e.g. flux / baseline-flux clamped to a sensible max) and pass it
// to update(). The widget clamps to [0, 1] internally and renders a
// frame + fill + marker at the screen coordinates supplied to draw().
//
// No DAL ownership: draw() emits fire_display_fill_rect through DAL
// directly. The widget does not clear the bounding box itself - the
// caller is expected to either have a clean background or compose
// the widget over an opaque region they've already cleared.

#pragma once

namespace nocturnation {
namespace widgets {

class BeatBarWidget {
public:
    BeatBarWidget() = default;

    // Update the bar's current fill fraction and optional marker
    // fraction. Both are clamped to [0.0, 1.0]. marker_fraction <= 0
    // suppresses the marker.
    //
    // For SimpleBeatShow's flux meter equivalence: the pre-Block-1
    // bar fill was `ratio * 50` pixels in a 218-px wide drawable; map
    // to fraction with `ratio * 50.0f / 218.0f`. The threshold marker
    // sat at `kBeatMultiplier (2.5) * 50 / 218 = 0.573`.
    void update(float bar_fraction, float marker_fraction = 0.0f);

    // Render the widget at the given screen rectangle. The widget
    // claims (x, y, w, h); within that it paints a 1 px frame, a
    // fill bar inside the frame, and (if marker_fraction > 0) a
    // 1 px vertical marker line that extends 2 px above and below
    // the frame for visibility.
    //
    // Minimum sensible dimensions: w >= 8, h >= 6. Below those the
    // frame eats the entire drawable and the bar isn't visible.
    void draw(int x, int y, int w, int h) const;

    // Test accessors.
    float bar_fraction()    const { return bar_fraction_; }
    float marker_fraction() const { return marker_fraction_; }

private:
    float bar_fraction_    = 0.0f;
    float marker_fraction_ = 0.0f;
};

}  // namespace widgets
}  // namespace nocturnation
