// LumeTextBinding - implementation.

#include "output_bindings/lume_text.h"
#include "output_bindings/output_binding_context.h"

#include "hal/hal.h"

#include <cstring>

namespace nocturnation {
namespace output_bindings {

using nocturnation::plugins::PropertyBag;

namespace {

// Font tier sizing. M5GFX (M5Unified backing on both Stick variants)
// uses an integer set_text_size scalar over its 6x8 default font.
// kSizeHeader 3 -> 18x24 px per char; kSizeBody 2 -> 12x16 px per char
// (matches the Config menu's "small but readable" tier per the
// stickc_lcd_role_per_mode memory).
constexpr uint8_t  kSizeHeader      = 3;
constexpr uint8_t  kSizeBody        = 2;
constexpr int      kCharW           = 6;   // default-font char width at size 1
constexpr int      kCharH           = 8;   // default-font char height at size 1
constexpr int      kHeaderCharW     = kCharW * kSizeHeader;   // 18
constexpr int      kHeaderCharH     = kCharH * kSizeHeader;   // 24
constexpr int      kBodyCharW       = kCharW * kSizeBody;     // 12
constexpr int      kBodyCharH       = kCharH * kSizeBody;     // 16

// Vertical layout. Header band occupies the top kHeaderBandH rows;
// body fills the remainder. Status pip (Lume system UI) sits in the
// top-right corner and may overpaint the rightmost ~38 px of the
// header row - intentional; pip is small + signal-state matters.
// kHeaderBandH = header char height + 16 px below for visual breathing
// room between the header line and the start of body text.
constexpr int kHeaderTopY        = 4;
constexpr int kHeaderBandH       = kHeaderCharH + 16;   // 40
constexpr int kBodyTopY          = kHeaderTopY + kHeaderBandH;  // 44
constexpr int kBodyBottomMargin  = 4;
constexpr int kRowGap            = 2;
constexpr int kBodyRowStride     = kBodyCharH + kRowGap;   // 18

// Scroll cadence for an over-width header. Pixels per tick at the
// LumeMode loop_tick cadence (~20 Hz). 2 px/tick -> ~40 px/s, a
// readable but unhurried marquee. Single-copy with wrap:
// scroll_x = 0 -> text aligned at left edge; positive scroll_x moves
// text left; when scroll_x exceeds text width, wrap to scroll_x = -dw
// so the text re-enters from the right edge. This avoids the two-
// copies-on-screen-at-once problem of an always-on toroidal marquee
// (a long title overlapping itself in the middle of the wrap-around).
constexpr int kHeaderScrollStepPx = 2;

// 8-8-8 RGB -> 5-6-5 (matches LCD framebuffer format used elsewhere
// in this codebase).
inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

constexpr uint16_t kBlack = 0x0000;

// Word-wrap a body string into lines no wider than max_chars. Buffer
// is a flat character array; each line is null-terminated; lines_out
// receives pointers into the buffer. Returns the number of lines
// written, capped at max_lines (excess words discarded - Phase 1; B9
// polish can add vertical scroll).
size_t word_wrap_body(const char* text, size_t text_len,
                      int max_chars_per_line,
                      char* line_buf, size_t line_buf_size,
                      const char** lines_out, size_t max_lines) {
    if (text == nullptr || text_len == 0 || max_chars_per_line <= 0
        || max_lines == 0 || line_buf_size == 0) {
        return 0;
    }
    size_t buf_pos = 0;
    size_t line_count = 0;
    const char* current_line_start = line_buf;
    size_t current_line_len = 0;   // chars in the building line (exclusive of NUL)

    // Tokenise by whitespace. Words longer than the line width get
    // chopped at the line boundary.
    size_t i = 0;
    while (i < text_len) {
        // Skip whitespace runs between words.
        while (i < text_len && (text[i] == ' ' || text[i] == '\t')) ++i;
        if (i >= text_len) break;
        // Find word end.
        size_t w_start = i;
        while (i < text_len && text[i] != ' ' && text[i] != '\t') ++i;
        size_t w_len = i - w_start;

        // Place the word. If it doesn't fit on the current line, flush.
        const bool need_space = (current_line_len > 0);
        const size_t needed = current_line_len + (need_space ? 1 : 0) + w_len;
        if (static_cast<int>(needed) > max_chars_per_line && current_line_len > 0) {
            // Flush current line.
            if (buf_pos < line_buf_size) {
                line_buf[buf_pos++] = '\0';
                lines_out[line_count++] = current_line_start;
            }
            if (line_count >= max_lines) return line_count;
            if (buf_pos >= line_buf_size) return line_count;
            current_line_start = line_buf + buf_pos;
            current_line_len = 0;
        }
        // Place a separating space if continuing an existing line.
        if (current_line_len > 0 && buf_pos < line_buf_size) {
            line_buf[buf_pos++] = ' ';
            ++current_line_len;
        }
        // Copy the word, chopping if it exceeds the line width.
        size_t copy_n = w_len;
        if (static_cast<int>(current_line_len + copy_n) > max_chars_per_line) {
            copy_n = static_cast<size_t>(max_chars_per_line) - current_line_len;
        }
        for (size_t k = 0; k < copy_n && buf_pos < line_buf_size - 1; ++k) {
            line_buf[buf_pos++] = text[w_start + k];
            ++current_line_len;
        }
        // If the word was longer than what we could fit, flush and
        // continue from the remainder.
        if (copy_n < w_len) {
            if (buf_pos < line_buf_size) {
                line_buf[buf_pos++] = '\0';
                lines_out[line_count++] = current_line_start;
            }
            if (line_count >= max_lines) return line_count;
            if (buf_pos >= line_buf_size) return line_count;
            current_line_start = line_buf + buf_pos;
            current_line_len = 0;
            // Re-enter loop with i set to the unconsumed word remainder.
            // Simplest: push the rest as a fresh word in subsequent iterations.
            i = w_start + copy_n;
        }
    }
    // Flush trailing line.
    if (current_line_len > 0 && buf_pos < line_buf_size && line_count < max_lines) {
        line_buf[buf_pos++] = '\0';
        lines_out[line_count++] = current_line_start;
    }
    return line_count;
}

}  // namespace

hal::CapabilityMask LumeTextBinding::required_capabilities() const {
    return hal::make_capability_mask(hal::Capability::DisplayText);
}

void LumeTextBinding::enter(OutputBindingContext& ctx) {
    // Start blank. Don't paint here - LumeMode::enter() already cleared
    // the screen to black before activating bindings. The status pip is
    // owned by LumeMode and continues painting independently.
    visible_         = false;
    header_len_      = 0;
    body_len_        = 0;
    header_scroll_x_ = 0;
    header_scrolls_  = false;
    content_dirty_   = false;
    scroll_dirty_    = false;
    last_tick_ms_    = ctx.now_ms();
}

void LumeTextBinding::exit(OutputBindingContext& /*ctx*/) {
    // Leave the screen as-is; the next mode owns its own redraw cadence.
}

void LumeTextBinding::on_text_display(OutputBindingContext& ctx,
                                       const transport::espnow::TextDisplayPayload& p) {
    header_len_ = p.header_len;
    if (header_len_) std::memcpy(header_, p.header, header_len_);
    header_[header_len_] = '\0';

    body_len_ = p.body_len;
    if (body_len_) std::memcpy(body_, p.body, body_len_);
    body_[body_len_] = '\0';

    r_ = p.r;
    g_ = p.g;
    b_ = p.b;
    ttl_ms_ = p.ttl_ms;
    content_started_ms_ = ctx.now_ms();
    visible_ = (header_len_ > 0 || body_len_ > 0);

    // Recompute scroll mode for the header.
    auto* d = hal::HAL::display();
    const int dw = (d != nullptr) ? d->width() : 240;
    const int header_pixel_width = static_cast<int>(header_len_) * kHeaderCharW;
    header_scrolls_  = (header_pixel_width > dw);
    header_scroll_x_ = 0;

    content_dirty_ = true;
}

void LumeTextBinding::on_clear_screen(OutputBindingContext& /*ctx*/,
                                       const transport::espnow::ClearScreenPayload& p) {
    if (!p.clear_text) return;
    visible_    = false;
    header_len_ = 0;
    body_len_   = 0;
    header_[0]  = '\0';
    body_[0]    = '\0';
    header_scroll_x_ = 0;
    header_scrolls_  = false;
    content_dirty_   = true;
}

void LumeTextBinding::tick(OutputBindingContext& /*ctx*/, uint32_t now_ms) {
    last_tick_ms_ = now_ms;

    // TTL expiry. Sticky (ttl_ms_ == 0) skips the check entirely.
    if (visible_ && ttl_ms_ != 0
        && (now_ms - content_started_ms_) >= ttl_ms_) {
        visible_         = false;
        header_len_      = 0;
        body_len_        = 0;
        header_[0]       = '\0';
        body_[0]         = '\0';
        header_scroll_x_ = 0;
        header_scrolls_  = false;
        content_dirty_   = true;
    }

    // Scroll advance (only when the header overflows). Single-copy
    // wrap: when the text fully exits left (scroll_x > text_width),
    // reset to -display_width so it re-enters from the right edge on
    // the next tick.
    if (visible_ && header_scrolls_) {
        auto* d = hal::HAL::display();
        const int dw = (d != nullptr) ? d->width() : 240;
        const int text_w = static_cast<int>(header_len_) * kHeaderCharW;
        header_scroll_x_ += kHeaderScrollStepPx;
        if (header_scroll_x_ > text_w) {
            header_scroll_x_ = -dw;
        }
        // Only flag scroll-dirty; the content itself didn't change.
        // Full-screen repaint isn't needed for a scroll - just refresh
        // the header band.
        scroll_dirty_ = true;
    }

    if (!content_dirty_ && !scroll_dirty_) return;

    auto* d = hal::HAL::display();
    if (d == nullptr) return;
    const int dw = d->width();
    const int dh = d->height();

    // Disable M5GFX horizontal text wrap. We render the marquee at
    // negative x and rely on clipping; default wrap=true would push
    // overflow chars to subsequent lines and ruin the layout. Also
    // controls body line draws (body word-wrap algorithm pre-segments
    // every line to fit, so wrap has no effect there - but setting
    // every paint survives any other code flipping the flag).
    d->set_text_wrap(false, false);

    const uint16_t fg = rgb565(r_, g_, b_);

    if (content_dirty_) {
        // Full-screen redraw - content actually changed (new text,
        // TTL expiry, clearscreen). 240x135 fill is ~32 KB SPI; fine
        // for this cadence (max ~10 Hz: TTL expiry + content updates
        // are operator-paced, not per-tick).
        d->fill_rect(0, 0, dw, dh, kBlack);
        if (visible_) {
            d->set_text_color(fg, kBlack);
            // Header.
            if (header_len_ > 0) {
                d->set_text_size(kSizeHeader);
                const int hw = static_cast<int>(header_len_) * kHeaderCharW;
                if (header_scrolls_) {
                    d->draw_text(-header_scroll_x_, kHeaderTopY, header_);
                } else {
                    d->draw_text((dw - hw) / 2, kHeaderTopY, header_);
                }
            }
            // Body.
            if (body_len_ > 0) {
                d->set_text_size(kSizeBody);
                const int max_chars_per_line = dw / kBodyCharW;
                char line_buf[transport::espnow::kTextDisplayMaxBodyLen + 8] = {};
                constexpr size_t kMaxLines = 8;
                const char* lines[kMaxLines] = {};
                const size_t n = word_wrap_body(body_, body_len_,
                                                max_chars_per_line,
                                                line_buf, sizeof(line_buf),
                                                lines, kMaxLines);
                int y = kBodyTopY;
                if (header_len_ == 0) {
                    const int block_h = static_cast<int>(n) * kBodyRowStride - kRowGap;
                    y = (dh - block_h) / 2;
                    if (y < 0) y = 0;
                }
                for (size_t i = 0;
                     i < n && y + kBodyCharH + kBodyBottomMargin <= dh;
                     ++i) {
                    const char* line = lines[i];
                    const int line_len = static_cast<int>(std::strlen(line));
                    const int line_w = line_len * kBodyCharW;
                    const int x = (dw - line_w) / 2;
                    d->draw_text(x, y, line);
                    y += kBodyRowStride;
                }
            }
        }
        content_dirty_ = false;
        scroll_dirty_  = false;   // full redraw already painted the marquee state
    } else if (scroll_dirty_) {
        // Header-band-only redraw for a marquee scroll tick. ~30 px
        // tall x 240 wide = 7.2 KB SPI - way cheaper than the full
        // screen, and crucially NO sprite allocation (the previous
        // buffered-paint path allocated + freed a 64.8 KB sprite at
        // every scroll tick = ~20 Hz, fragmenting the heap on memory-
        // tight hosts).
        d->fill_rect(0, kHeaderTopY, dw, kHeaderBandH, kBlack);
        if (visible_ && header_len_ > 0) {
            d->set_text_color(fg, kBlack);
            d->set_text_size(kSizeHeader);
            d->draw_text(-header_scroll_x_, kHeaderTopY, header_);
        }
        scroll_dirty_ = false;
    }
}

// =============================================================================
// Singletons
// =============================================================================

namespace {
LumeTextBinding     s_instance;
PropertyBag         s_bag(s_instance);
OutputBindingContext s_ctx(s_instance, s_bag);
}  // namespace

LumeTextBinding*      lume_text_instance()      { return &s_instance; }
PropertyBag&          lume_text_property_bag()  { return s_bag; }
OutputBindingContext& lume_text_context()       { return s_ctx; }

}  // namespace output_bindings
}  // namespace nocturnation
