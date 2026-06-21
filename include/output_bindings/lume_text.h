// LumeTextBinding - Lume-side renderer for Epic 13 TEXT_DISPLAY frames.
//
// Renders header + body strings onto the Lume's HAL::display(). Header
// uses the larger font tier (size 3, 18x24 px per character); body uses
// the menu-option tier (size 2, 12x16 px). Both centred horizontally.
// Header at top, body below header (or vertically-centred if header
// empty). Header scrolls left when wider than the display; body
// word-wraps and clips vertically when overflow exceeds the available
// rows (vertical scroll is Phase 2 polish).
//
// Decoupled from LumeBitmapBinding: the bitmap binding handles bitmap-
// and clear-bitmap; this binding handles text and clear-text. A
// CLEAR_SCREEN payload's two flags determine which binding clears.
//
// Bench expectations:
//   - On a Stick with both DisplayText + DisplayBitmap declared, both
//     bindings activate. Text renders ON TOP of any bitmap (text layer
//     last in the draw order).
//   - TTL=0 means sticky - content holds until CLEAR_SCREEN or
//     superseding TEXT_DISPLAY arrives. Non-zero TTL auto-clears after
//     the elapsed window.
//   - Atom Lite never sees TEXT_DISPLAY frames (no DisplayText cap;
//     LumeMode quick-drops at the radio gate).
//
// Power profile: defaults (event-driven; framework ticks every binding
// from loop_tick at the main-loop cadence, no per-binding rate needed).
// Capability requirements: DisplayText. Device class: Display.

#pragma once

#include <cstdint>

#include "output_bindings/output_binding.h"
#include "plugins/property_bag.h"
#include "transport/espnow/frame.h"

namespace nocturnation {
namespace output_bindings {

class LumeTextBinding : public OutputBinding {
public:
    const char* id()           const override { return "lume-text"; }
    const char* display_name() const override { return "Lume Text"; }

    hal::DeviceClass device_class() const override {
        return hal::DeviceClass::Display;
    }

    hal::CapabilityMask required_capabilities() const override;

    // The wash-family capabilities don't apply to display content; the
    // Lume-side dispatch routes the new Display-family hooks based on
    // device_class() instead. Returning all-false here also prevents
    // any future code that walks BindingCapabilities from misclassifying
    // a text surface as a wash renderer.
    BindingCapabilities capabilities() const override {
        return BindingCapabilities{
            /*can_pulse=*/   false,
            /*can_wash=*/    false,
            /*can_overlay=*/ false,
        };
    }

    // Epic 13: handler runs in WiFi-callback context safely - we only
    // update in-RAM state + flip a dirty flag here; the actual SPI
    // burst to the LCD happens later in tick() on the main task.
    bool can_render_in_callback() const override { return true; }

    void enter(OutputBindingContext&) override;
    void exit (OutputBindingContext&) override;

    void on_text_display(OutputBindingContext&,
                          const transport::espnow::TextDisplayPayload&) override;
    void on_clear_screen(OutputBindingContext&,
                          const transport::espnow::ClearScreenPayload&) override;
    void tick(OutputBindingContext&, uint32_t now_ms) override;

private:
    // Owned content state. Buffers are sized one byte beyond the wire
    // limits for the convenience of nul-terminating in-place before
    // each draw_text call (the HAL signature takes a C-string).
    char    header_[transport::espnow::kTextDisplayMaxHeaderLen + 1] = {};
    uint8_t header_len_ = 0;
    char    body_[transport::espnow::kTextDisplayMaxBodyLen + 1] = {};
    uint8_t body_len_ = 0;
    uint8_t r_ = 255, g_ = 255, b_ = 255;

    // Lifecycle.
    bool     visible_ = false;        // set when content holds; cleared by CLEAR_SCREEN / TTL expiry / supersede
    uint32_t content_started_ms_ = 0;
    uint16_t ttl_ms_ = 0;             // 0 = sticky

    // Scroll state (header only; body is wrap+clip in Phase 1).
    int  header_scroll_x_ = 0;        // pixels of leftward scroll
    bool header_scrolls_  = false;    // true if header width > display width
    uint32_t last_tick_ms_ = 0;

    // Dirty flags. content_dirty_ triggers a full-screen repaint
    // (background + header + body); set on receipt of a new
    // TEXT_DISPLAY, on TTL expiry, on enter(), on clearscreen.
    // scroll_dirty_ triggers a header-band-only repaint and is set
    // every tick the marquee advances. Without the split, every
    // scroll tick (~20 Hz) repaints the full 240x135 display - a
    // 64.8 KB SPI burst plus a heap-thrashing sprite alloc in the
    // buffered-paint path that lived here pre-fix. With the split,
    // a scroll tick only repaints ~30 px of header band (~7 KB SPI),
    // and no sprite is ever allocated.
    bool content_dirty_ = true;
    bool scroll_dirty_  = false;
};

// Singletons. Registered in main.cpp; LumeMode's context_for() pairs
// the binding singleton with its OutputBindingContext singleton.
LumeTextBinding*      lume_text_instance();
plugins::PropertyBag& lume_text_property_bag();
OutputBindingContext& lume_text_context();

}  // namespace output_bindings
}  // namespace nocturnation
