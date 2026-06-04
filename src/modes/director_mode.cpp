// DirectorMode implementation (Epic 4.7 Block 1).
//
// Thin shell over the active Show. Mode owns: pause flag, music_event
// broadcast (transport concern, separate from the LIGHT_PULSE
// fan-out), lifecycle (audio input + ESP-NOW broadcast), and the
// picker / settings overlays. The per-beat render fan-out, BPM
// tracking, flux meter, and full screen rendering live in the Show.

#include "director_mode.h"

#include "persistence.h"
#include "dal/dal.h"
#include "../dal/drivers/espnow_broadcast_driver.h"
#include "transport/espnow/frame.h"
#include "shows/show.h"
#include "shows/show_context.h"
#include "shows/show_registry.h"
#include "shows/simple_beat_show.h"
#include "hal/input_action.h"
#include "hal/hal.h"
#include "plugins/plugin.h"

#include <cstdio>
#include <cstring>

#ifdef ARDUINO
#include <Arduino.h>
#else
extern "C" uint32_t millis();
#endif

// Spectrum-frame routing callback. Defined at namespace scope in
// mode_machine.cpp so DirectorMode can pass it to
// DAL::subscribe_spectrum_frames at show-activation time without
// ModeMachine itself needing to subscribe at begin() (which would pin
// has_spectrum_frame_subscribers() to true and defeat Block 7's gate).
namespace nocturnation { namespace modes {
void on_dal_spectrum_frame(const char*, const dal::SpectrumFrameEvent&);
} }

namespace nocturnation {
namespace modes {

using namespace nocturnation::dal;
using nocturnation::hal::InputAction;
using nocturnation::hal::InputEvent;
using nocturnation::shows::show_registry;
using nocturnation::plugins::PropertyDef;
using nocturnation::plugins::PropertyType;
using nocturnation::plugins::PropertyValue;

namespace {

// True when the host satisfies every capability the show declares.
bool show_capability_gate_open(const shows::Show& s) {
    const hal::CapabilityMask req = s.required_capabilities();
    if (req.empty()) return true;
    hal::CapabilityMask host;
    for (size_t i = 0; i < hal::HAL::capability_count(); ++i) {
        host.set(hal::HAL::capabilities()[i]);
    }
    return req.subset_of(host);
}

}  // namespace

void DirectorMode::enter() {
    last_draw_ms_              = 0;
    paused_                    = false;
    overlay_                   = Overlay::None;
    overlay_cursor_            = 0;
    overlay_view_offset_       = 0;
    descriptor_delivered_      = false;
    last_centroid_delivered_   = 0;
    last_energy_delivered_     = 0;
    last_density_delivered_    = 0;
    section_delivered_         = false;
    last_section_delivered_    = 0;

    resolve_active_show_from_nvs();

    if (ctx_) ctx_->set_paused(paused_);
    if (ctx_) ctx_->mark_entered(millis());
    if (active_show_ && ctx_) active_show_->enter(*ctx_);

    // Subscribe to spectrum frames iff the active show declares it
    // needs them. Drives Block 7's pipeline gate live - shows without
    // needs_spectrum_frame leave the FFT path's per-frame fan-out
    // skipped in LocalDriver.
    sync_spectrum_subscription(/*active=*/true);

    DAL::start_audio_input("local", 16000, 512);
    // Channel from NVS. DAL's EspNowBroadcastDriver owns the radio
    // lifecycle; this mode just toggles it on enter / off on exit.
    esp_now_broadcast_driver_instance()->start_broadcast(
        persistence::load_director_channel());
    draw();
}

void DirectorMode::exit() {
    if (active_show_ && ctx_) active_show_->exit(*ctx_);
    // Always drop any spectrum subscription installed in enter() /
    // picker_confirm. The mode loses the radio + audio pipeline on
    // exit; subscribers leaking past would pin Block 7's gate.
    sync_spectrum_subscription(/*active=*/false);
    DAL::stop_audio_input("local");
    esp_now_broadcast_driver_instance()->stop_broadcast();
}

void DirectorMode::loop_tick() {
    const uint32_t now = millis();
    if (now - last_draw_ms_ > 50) {
        draw();
        last_draw_ms_ = now;
    }
    // Epic 6C Phase E: re-broadcast active washes periodically. The DAL
    // helper enforces its own ~10 s per-target cadence + capability gate,
    // so this call is cheap from the 20 Hz loop.
    DAL::refresh_active_washes(now);
}

void DirectorMode::resolve_active_show_from_nvs() {
    const char* saved = persistence::load_active_show_id();
    shows::Show* s = show_registry().find(saved);
    if (!s) {
        // Saved id no longer resolves (uninstalled / renamed). Fall
        // back to the canonical default; persist nothing - the saved
        // id stays valid for the next boot in case the original show
        // is re-added.
        s = show_registry().find("simple-beat");
    }
    active_show_ = s;
    ctx_ = (s != nullptr) ? &s->context() : nullptr;
}

#ifndef ARDUINO
const char* DirectorMode::active_show_id_for_tests() const {
    return (active_show_ != nullptr) ? active_show_->id() : "";
}
const char* DirectorMode::status_label_for_tests() const {
    return (active_show_ != nullptr) ? active_show_->display_name() : "";
}
#endif

void DirectorMode::on_audio_frame(const AudioFrameEvent& ev) {
    // MUSIC_EVENT (0x06) broadcasting was removed in the spec v0.29
    // protocol trim - DROP / BREAKDOWN / BUILD no longer have a wire
    // message type. The DropDetector still runs internally (Director-
    // side state for any local UI that wants it) but nothing is sent
    // to Lumes for these events.

    // Render gating: while an overlay is open the Director's local
    // display is taken over by the overlay UI. We skip event dispatch
    // to the show so the BPM tracking / fan-out aligns with what the
    // operator sees on screen. Audio analysis + ESP-NOW broadcast
    // still run upstream so the music_event path above keeps firing.
    if (overlay_ != Overlay::None) return;

    // Forward raw frame to show (flux trackers, level readouts, etc).
    if (active_show_ && ctx_) {
        active_show_->on_audio_frame(*ctx_, ev);

        // Onset hooks - fire when the corresponding analyser detector
        // stamped a strength on the frame. is_beat is the legacy
        // boolean kept for back-compat; beat_strength is the Block 3
        // quantised intensity. snare / hihat are new Block 3 surfaces.
        if (ev.beat_strength > 0) {
            active_show_->on_beat_detected(*ctx_, ev.beat_strength);
        }
        if (ev.snare_strength > 0) {
            active_show_->on_snare_detected(*ctx_, ev.snare_strength);
        }
        if (ev.hihat_strength > 0) {
            active_show_->on_hihat_detected(*ctx_, ev.hihat_strength);
        }

        // Continuous descriptor - rate-limited to >= 5 % change in any
        // component, so a show that updates a colour palette per
        // descriptor doesn't get every-frame churn at ~30 Hz.
        const int dc = static_cast<int>(ev.centroid) - static_cast<int>(last_centroid_delivered_);
        const int de = static_cast<int>(ev.energy)   - static_cast<int>(last_energy_delivered_);
        const int dd = static_cast<int>(ev.density)  - static_cast<int>(last_density_delivered_);
        const int abs_dc = dc < 0 ? -dc : dc;
        const int abs_de = de < 0 ? -de : de;
        const int abs_dd = dd < 0 ? -dd : dd;
        if (!descriptor_delivered_
            || abs_dc >= kMusicDescriptorDelta
            || abs_de >= kMusicDescriptorDelta
            || abs_dd >= kMusicDescriptorDelta) {
            active_show_->on_music_descriptor(*ctx_,
                                               ev.centroid,
                                               ev.energy,
                                               ev.density);
            last_centroid_delivered_ = ev.centroid;
            last_energy_delivered_   = ev.energy;
            last_density_delivered_  = ev.density;
            descriptor_delivered_    = true;
        }

        // Section transition: fire only when the section changes from
        // the last delivered value. SectionDetector applies hysteresis
        // upstream so consumers see clean transitions.
        if (!section_delivered_ || ev.section != last_section_delivered_) {
            active_show_->on_section_change(*ctx_, ev.section);
            last_section_delivered_ = ev.section;
            section_delivered_      = true;
        }
    }
}

void DirectorMode::on_spectrum_frame(const SpectrumFrameEvent& ev) {
    if (overlay_ != Overlay::None) return;

    if (active_show_ && ctx_) {
        active_show_->on_spectrum_frame(*ctx_, ev);
    }
}

void DirectorMode::sync_spectrum_subscription(bool active) {
    // Always unsubscribe first so we start from a known state and the
    // subscriber count is monotonic per (active_show, active) pair.
    DAL::unsubscribe_spectrum_frames("local");

    if (!active) return;
    if (!active_show_) return;
    if (!active_show_->power().needs_spectrum_frame) return;

    DAL::subscribe_spectrum_frames("local", &on_dal_spectrum_frame);
}

void DirectorMode::on_input_action(const InputEvent& ev) {
    // Picker / Settings act as toggles: a second press of the same
    // action closes the overlay.
    if (overlay_ == Overlay::Picker) {
        switch (ev.action) {
            case InputAction::Picker:
                overlay_ = Overlay::None;
                draw();
                return;
            case InputAction::Cycle: {
                const size_t n = picker_row_count();
                if (n > 0) overlay_cursor_ = (overlay_cursor_ + 1) % n;
                update_overlay_view_offset(n);
                draw();
                return;
            }
            case InputAction::Confirm:
                on_picker_confirm();
                return;
            default:
                return;
        }
    }
    if (overlay_ == Overlay::Settings) {
        switch (ev.action) {
            case InputAction::Settings:
                overlay_ = Overlay::None;
                draw();
                return;
            case InputAction::Cycle: {
                const size_t n = settings_row_count();
                if (n > 0) overlay_cursor_ = (overlay_cursor_ + 1) % n;
                update_overlay_view_offset(n);
                draw();
                return;
            }
            case InputAction::Confirm:
                on_settings_confirm();
                return;
            default:
                return;
        }
    }

    // Overlay::None - base action set.
    switch (ev.action) {
        case InputAction::Picker:
            overlay_             = Overlay::Picker;
            overlay_cursor_      = 0;
            overlay_view_offset_ = 0;
            draw();
            return;
        case InputAction::Settings:
            overlay_             = Overlay::Settings;
            overlay_cursor_      = 0;
            overlay_view_offset_ = 0;
            draw();
            return;
        case InputAction::Pause:
            paused_ = !paused_;
            if (ctx_) ctx_->set_paused(paused_);
            draw();
            return;
        case InputAction::Confirm:
        case InputAction::Cycle:
        case InputAction::CyclePrev:
            // Route to the show. The show decides whether to act on it.
            if (active_show_ && ctx_) {
                active_show_->on_input_action(*ctx_, ev);
            }
            draw();
            return;
        default:
            return;
    }
}

// ---------------------------------------------------------------------------
// Picker overlay
// ---------------------------------------------------------------------------

size_t DirectorMode::picker_row_count() const {
    return show_registry().count() + 1;   // + "<- Menu"
}

bool DirectorMode::picker_row_is_back(size_t row) const {
    return row == show_registry().count();
}

void DirectorMode::on_picker_confirm() {
    if (picker_row_is_back(overlay_cursor_)) {
        // "<- Menu" sentinel. Close overlay first so re-entry to
        // Director starts cleanly, then switch.
        overlay_ = Overlay::None;
        ModeMachine::switch_to(ModeId::Menu);
        return;
    }
    shows::Show* picked = show_registry().at(overlay_cursor_);
    if (!picked) {
        return;
    }
    if (!show_capability_gate_open(*picked)) {
        // Greyed entry. Consume the Confirm without changing state.
        return;
    }
    if (picked == active_show_) {
        overlay_ = Overlay::None;
        draw();
        return;
    }
    // Swap active show. Tear down the old, set new, persist, bring up
    // new with a fresh entered_at_ms.
    if (active_show_ && ctx_) active_show_->exit(*ctx_);
    sync_spectrum_subscription(/*active=*/false);

    active_show_ = picked;
    persistence::save_active_show_id(picked->id());
    ctx_ = &picked->context();
    if (ctx_) ctx_->set_paused(paused_);
    if (ctx_) ctx_->mark_entered(millis());
    if (ctx_) active_show_->enter(*ctx_);
    sync_spectrum_subscription(/*active=*/true);
    overlay_ = Overlay::None;
    draw();
}

// ---------------------------------------------------------------------------
// Settings overlay
// ---------------------------------------------------------------------------

size_t DirectorMode::settings_row_count() const {
    if (!active_show_) return 1;             // just "<- Back"
    const auto props = active_show_->properties();
    return props.size + 1;                   // + "<- Back"
}

bool DirectorMode::settings_row_is_back(size_t row) const {
    if (!active_show_) return row == 0;
    return row == active_show_->properties().size;
}

void DirectorMode::on_settings_confirm() {
    if (settings_row_is_back(overlay_cursor_)) {
        overlay_ = Overlay::None;
        draw();
        return;
    }
    if (!active_show_ || !ctx_) return;
    const auto props = active_show_->properties();
    if (overlay_cursor_ >= props.size) return;
    const PropertyDef& def = props[overlay_cursor_];
    const PropertyValue cur = ctx_->get_property(def.key);
    PropertyValue next = cur;
    switch (def.type) {
        case PropertyType::Bool:
            next = PropertyValue::from_bool(!cur.as_bool());
            break;
        case PropertyType::Enum: {
            const uint8_t mn  = def.min_value.as_enum();
            const uint8_t mx  = def.max_value.as_enum();
            const uint8_t cur_e = cur.as_enum();
            const uint8_t range = static_cast<uint8_t>(mx - mn + 1);
            const uint8_t rel   = static_cast<uint8_t>((cur_e - mn + 1) % range);
            next = PropertyValue::from_enum(static_cast<uint8_t>(mn + rel));
            break;
        }
        case PropertyType::U8: {
            const uint8_t mn = def.min_value.as_u8();
            const uint8_t mx = def.max_value.as_u8();
            const uint8_t cv = cur.as_u8();
            const uint8_t nv = (cv >= mx) ? mn : static_cast<uint8_t>(cv + 1);
            next = PropertyValue::from_u8(nv);
            break;
        }
        case PropertyType::U16: {
            const uint16_t mn = def.min_value.as_u16();
            const uint16_t mx = def.max_value.as_u16();
            const uint16_t cv = cur.as_u16();
            const uint16_t nv = (cv >= mx) ? mn : static_cast<uint16_t>(cv + 1);
            next = PropertyValue::from_u16(nv);
            break;
        }
        case PropertyType::Colour: {
            const uint32_t cv = cur.as_colour();
            const uint8_t  r  = static_cast<uint8_t>((cv >> 16) & 0xFF);
            const uint8_t  g  = static_cast<uint8_t>((cv >>  8) & 0xFF);
            const uint8_t  b  = static_cast<uint8_t>( cv        & 0xFF);
            const uint8_t  nr = static_cast<uint8_t>(r + 0x40);
            const uint32_t nv = (static_cast<uint32_t>(nr) << 16)
                              | (static_cast<uint32_t>(g)  <<  8)
                              |  static_cast<uint32_t>(b);
            next = PropertyValue::from_colour(nv);
            break;
        }
    }
    ctx_->set_property(def.key, next);
    active_show_->on_property_changed(*ctx_, def.key);
    draw();
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void DirectorMode::draw() {
    if (overlay_ == Overlay::Picker)   { draw_picker();   return; }
    if (overlay_ == Overlay::Settings) { draw_settings(); return; }

    // Show owns the screen during normal operation.
    if (active_show_ && ctx_) active_show_->on_render(*ctx_);

    // Source_id status label (Epic 5.5 B5) overlays the bottom-right
    // corner so the operator can verify which ID the audience is
    // locking to. Drawn at size 1 (~8 px tall, ~6 px wide); a 5-char
    // label ("P:4F?") fits inside ~30 px at the right edge. Renders
    // every draw tick on top of whatever the Show painted; Shows
    // following the developing-shows.md "reserve ~14 px at the bottom
    // for a footer hint" convention will not collide.
    auto* drv = dal::esp_now_broadcast_driver_instance();
    char status[16];
    const size_t n = dal::EspNowBroadcastDriver::format_status_label(
        drv->startup_state(), drv->source_id(), drv->listen_candidate(),
        status, sizeof(status));
    if (n > 0) {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            /*x=*/200, /*y=*/126, /*text=*/status,
            /*fg=*/WHITE, /*bg=*/BLACK, /*size=*/1});
    }
}

void DirectorMode::update_overlay_view_offset(size_t row_count) {
    if (row_count <= kOverlayMaxVisible) {
        overlay_view_offset_ = 0;
        return;
    }
    // Keep the cursor inside [view_offset, view_offset + kOverlayMaxVisible - 1].
    if (overlay_cursor_ < overlay_view_offset_) {
        overlay_view_offset_ = overlay_cursor_;
    } else if (overlay_cursor_ >= overlay_view_offset_ + kOverlayMaxVisible) {
        overlay_view_offset_ = overlay_cursor_ - kOverlayMaxVisible + 1;
    }
    if (overlay_view_offset_ + kOverlayMaxVisible > row_count) {
        overlay_view_offset_ = row_count - kOverlayMaxVisible;
    }
}

void DirectorMode::draw_picker() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "Show", WHITE, BLACK, 2});

    const size_t show_count = show_registry().count();
    const size_t rows = picker_row_count();
    const size_t end = (overlay_view_offset_ + kOverlayMaxVisible < rows)
        ? overlay_view_offset_ + kOverlayMaxVisible
        : rows;
    int y = 30;
    for (size_t i = overlay_view_offset_; i < end; ++i) {
        const bool sel = (i == overlay_cursor_);
        char buf[40];
        if (i < show_count) {
            shows::Show* s = show_registry().at(i);
            const bool gated = !show_capability_gate_open(*s);
            std::snprintf(buf, sizeof(buf), "%s %s%s",
                          sel ? ">" : " ",
                          s->display_name(),
                          gated ? " (-)" : "");
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, y, buf,
                sel   ? YELLOW : (gated ? RED : WHITE),
                BLACK, 2});
        } else {
            std::snprintf(buf, sizeof(buf), "%s %s",
                          sel ? ">" : " ", "<- Menu");
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, y, buf, sel ? YELLOW : WHITE, BLACK, 2});
        }
        y += 18;
    }

    // Scroll indicators: small chevrons at the top-right / bottom-right
    // when content extends past the visible window in either direction.
    if (overlay_view_offset_ > 0) {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            225, 30, "^", WHITE, BLACK, 1});
    }
    if (end < rows) {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            225, 118, "v", WHITE, BLACK, 1});
    }

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 128, "B: cycle  A: select  B-hold: close",
        WHITE, BLACK, 1});
}

void DirectorMode::draw_settings() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "Settings", WHITE, BLACK, 2});

    if (!active_show_ || !ctx_) {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 128, "B: cycle  A: edit  A-hold: close",
            WHITE, BLACK, 1});
        return;
    }

    const auto props = active_show_->properties();
    const size_t total_rows = settings_row_count();
    const size_t end = (overlay_view_offset_ + kOverlayMaxVisible < total_rows)
        ? overlay_view_offset_ + kOverlayMaxVisible
        : total_rows;
    int y = 30;
    for (size_t i = overlay_view_offset_; i < end; ++i) {
        const bool sel = (i == overlay_cursor_);
        char buf[40];
        if (i < props.size) {
            const PropertyDef& def = props[i];
            const PropertyValue v  = ctx_->get_property(def.key);
            switch (def.type) {
                case PropertyType::Bool:
                    std::snprintf(buf, sizeof(buf), "%s %s: %s",
                                  sel ? ">" : " ",
                                  def.display_name,
                                  v.as_bool() ? "ON" : "OFF");
                    break;
                case PropertyType::Enum: {
                    const uint8_t idx = v.as_enum();
                    const char* nm = (def.enum_names != nullptr)
                        ? def.enum_names[idx - def.min_value.as_enum()]
                        : "?";
                    std::snprintf(buf, sizeof(buf), "%s %s: %s",
                                  sel ? ">" : " ", def.display_name, nm);
                    break;
                }
                case PropertyType::U8:
                    std::snprintf(buf, sizeof(buf), "%s %s: %u%s",
                                  sel ? ">" : " ", def.display_name,
                                  (unsigned)v.as_u8(),
                                  def.unit ? def.unit : "");
                    break;
                case PropertyType::U16:
                    std::snprintf(buf, sizeof(buf), "%s %s: %u%s",
                                  sel ? ">" : " ", def.display_name,
                                  (unsigned)v.as_u16(),
                                  def.unit ? def.unit : "");
                    break;
                case PropertyType::Colour:
                    std::snprintf(buf, sizeof(buf), "%s %s: 0x%06lX",
                                  sel ? ">" : " ", def.display_name,
                                  (unsigned long)(v.as_colour() & 0xFFFFFF));
                    break;
            }
        } else {
            // The "<- Back" sentinel row (always the last index).
            std::snprintf(buf, sizeof(buf), "%s %s",
                          sel ? ">" : " ", "<- Back");
        }
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, y, buf, sel ? YELLOW : WHITE, BLACK, 2});
        y += 18;
    }

    // Scroll indicators - chevrons when content extends past the window.
    if (overlay_view_offset_ > 0) {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            225, 30, "^", WHITE, BLACK, 1});
    }
    if (end < total_rows) {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            225, 118, "v", WHITE, BLACK, 1});
    }

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 128, "B: cycle  A: edit  A-hold: close",
        WHITE, BLACK, 1});
}

}  // namespace modes
}  // namespace nocturnation
