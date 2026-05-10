// AutonomousMasterMode implementation (Epic 4.6 Block 10).
//
// Thin shell over the active Visualisation. Mode owns: pause flag,
// flux/baseline tracking for the on-screen meter, music_event broadcast,
// status display, lifecycle (audio input + ESP-NOW broadcast), and the
// picker / settings overlays. The per-beat render fan-out + BPM tracking
// + Colour enum live in BeatPulseVisualisation.
//
// Block 10 changes since Block 8:
//   - Active vis is resolved from NVS via persistence::load_active_vis_id
//     (falls back to "beat-pulse" if the id no longer registers).
//   - Input handling is InputAction-driven (Mode::on_input_action) rather
//     than raw button events. The 2-button mapper in HAL emits the
//     semantic actions; mode_machine.cpp's facade routes them here.
//   - Picker and Settings overlays consume InputActions and gate the
//     vis's render fan-out so on-screen overlays don't fight pulse-rect
//     repaints.

#include "autonomous_master_mode.h"

#include "persistence.h"
#include "dal/dal.h"
#include "../dal/drivers/espnow_broadcast_driver.h"
#include "transport/espnow/frame.h"
#include "visualisations/visualisation.h"
#include "visualisations/visualisation_context.h"
#include "visualisations/visualisation_registry.h"
#include "visualisations/beat_pulse.h"
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

// Spectrum-frame routing callback (Epic 4.6 Block 11). Defined at
// namespace scope in mode_machine.cpp so AutonomousMasterMode can pass
// it to DAL::subscribe_spectrum_frames at vis-activation time without
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
using nocturnation::visualisations::visualisation_registry;
using nocturnation::visualisations::beat_pulse_colour_label;
using nocturnation::visualisations::beat_pulse_estimated_bpm;
using nocturnation::plugins::PropertyDef;
using nocturnation::plugins::PropertyType;
using nocturnation::plugins::PropertyValue;

namespace {

constexpr float    kBeatMultiplier = 2.5f;
constexpr float    kBaselineAlpha  = 0.02f;
constexpr float    kVolumeGate     = 500.0f;

// True when the host satisfies every capability the vis declares.
bool vis_capability_gate_open(const visualisations::Visualisation& v) {
    const hal::CapabilityMask req = v.required_capabilities();
    if (req.empty()) return true;
    hal::CapabilityMask host;
    // Walk the HAL's declared capability list.
    for (size_t i = 0; i < hal::HAL::capability_count(); ++i) {
        host.set(hal::HAL::capabilities()[i]);
    }
    return req.subset_of(host);
}

}  // namespace

void AutonomousMasterMode::enter() {
    // Reset display-only flux/baseline state.
    baseline_flux_     = 100.0f;
    prev_bass_energy_  = 0.0f;
    current_flux_      = 0.0f;
    current_level_     = 0.0f;
    last_draw_ms_      = 0;
    paused_            = false;
    overlay_           = Overlay::None;
    overlay_cursor_    = 0;

    resolve_active_vis_from_nvs();

    if (ctx_) ctx_->set_paused(paused_);
    if (ctx_) ctx_->mark_entered(millis());
    if (active_vis_ && ctx_) active_vis_->enter(*ctx_);

    refresh_status_label();
    // Subscribe to spectrum frames iff the active vis declares it
    // needs them. Drives Block 7's pipeline gate live - vis without
    // needs_spectrum_frame leave the FFT path's per-frame fan-out
    // skipped in LocalDriver.
    sync_spectrum_subscription(/*active=*/true);

    DAL::start_audio_input("local", 16000, 512);
    // Channel from NVS. DAL's EspNowBroadcastDriver owns the radio lifecycle
    // now; this mode just toggles it on enter / off on exit. Retransmit
    // pumping + skip-if-recent heartbeat run from the driver's loop_tick.
    esp_now_broadcast_driver_instance()->start_broadcast(
        persistence::load_master_channel());
    draw();
}

void AutonomousMasterMode::exit() {
    if (active_vis_ && ctx_) active_vis_->exit(*ctx_);
    // Always drop any spectrum subscription we may have installed in
    // enter() / picker_confirm. The mode loses the radio + audio
    // pipeline on exit; spectrum subscribers leaking past would pin
    // Block 7's gate.
    sync_spectrum_subscription(/*active=*/false);
    DAL::stop_audio_input("local");
    esp_now_broadcast_driver_instance()->stop_broadcast();
}

void AutonomousMasterMode::loop_tick() {
    const uint32_t now = millis();
    if (now - last_draw_ms_ > 50) {
        draw();
        last_draw_ms_ = now;
    }
}

void AutonomousMasterMode::resolve_active_vis_from_nvs() {
    const char* saved = persistence::load_active_vis_id();
    visualisations::Visualisation* v =
        visualisation_registry().find(saved);
    if (!v) {
        // Saved id no longer resolves (uninstalled / renamed). Fall back
        // to the canonical default; persist nothing - the saved id stays
        // valid for the next boot in case the original vis is re-added.
        v = visualisation_registry().find("beat-pulse");
    }
    active_vis_ = v;
    // Per-vis context lookup (Block 11). Each concrete vis owns its
    // own VisualisationContext singleton and exposes it through the
    // base-class context() accessor; the mode no longer hardcodes a
    // specific accessor. Picker switch path (on_picker_confirm) goes
    // through the same call.
    ctx_ = (v != nullptr) ? &v->context() : nullptr;
}

void AutonomousMasterMode::refresh_status_label() {
    const char* name = (active_vis_ != nullptr)
                       ? active_vis_->display_name()
                       : "";
    if (!name) name = "";

    // Cap at 10 visible chars + "..." (status strip is narrow so we
    // truncate aggressively rather than spill into adjacent labels).
    constexpr size_t kMaxVisible = 10;
    const size_t n = std::strlen(name);
    if (n <= kMaxVisible) {
        std::strncpy(status_label_buf_, name, kStatusLabelCap);
        status_label_buf_[kStatusLabelCap - 1] = '\0';
    } else {
        std::strncpy(status_label_buf_, name, kMaxVisible);
        status_label_buf_[kMaxVisible]     = '.';
        status_label_buf_[kMaxVisible + 1] = '.';
        status_label_buf_[kMaxVisible + 2] = '.';
        status_label_buf_[kMaxVisible + 3] = '\0';
    }
}

#ifndef ARDUINO
const char* AutonomousMasterMode::active_vis_id_for_tests() const {
    return (active_vis_ != nullptr) ? active_vis_->id() : "";
}
#endif

void AutonomousMasterMode::on_audio_frame(const AudioFrameEvent& ev) {
    current_level_ = ev.overall_rms;

    if (current_level_ < kVolumeGate) {
        prev_bass_energy_ = 0.0f;
        return;
    }

    // Track flux + baseline for the on-screen meter only. Beat firing
    // is owned by the DAL analyser's BeatDetector (Epic 4.5 Block 3);
    // these values have no bearing on which frames fire beats.
    float flux = ev.bass_energy - prev_bass_energy_;
    if (flux < 0) flux = 0;
    prev_bass_energy_ = ev.bass_energy;
    current_flux_     = flux;

    baseline_flux_ = baseline_flux_ * (1.0f - kBaselineAlpha)
                   + flux * kBaselineAlpha;

    // Broadcast macro-level musical events (DROP / BREAKDOWN / BUILD)
    // as MUSIC_EVENT (0x06) frames. Independent of the beat path -
    // a drop can fire on any frame, beat or not, and at most once per
    // genuine transition (the analyser's DropDetector arms / disarms
    // internally). Skipped during pause so the entire deployment stays
    // silent on a single mute press. Mode-level concern (not vis).
    if (ev.music_event != 0 && !paused_) {
#ifdef ARDUINO
        const char* name = (ev.music_event == 1) ? "DROP"
                         : (ev.music_event == 2) ? "BREAKDOWN"
                         : (ev.music_event == 3) ? "BUILD"
                         :                         "?";
        Serial.printf("[MUSIC] %s at %lu ms (bass_energy=%.1f)\n",
                      name,
                      static_cast<unsigned long>(millis()),
                      ev.bass_energy);
#endif
        esp_now_broadcast_driver_instance()->send_music_event(
            static_cast<transport::espnow::MusicEventType>(ev.music_event));
    }

    // Render gating: while a picker / settings overlay is open the
    // master's local display is taken over by the overlay UI. We skip
    // the vis's render fan-out so the screen-pulse fade doesn't fight
    // overlay repaints. The vis's BPM tracking stays internal (it sees
    // the gate via ctx.paused()-equivalent semantics: we just don't
    // call on_audio_frame at all). Audio analysis and ESP-NOW broadcast
    // still run upstream so slaves keep receiving frames.
    if (overlay_ != Overlay::None) return;

    // Forward the frame to the active vis. The vis owns BPM tracking +
    // per-beat render fan-out; pause is mirrored into ctx so the vis
    // can early-return on render without missing BPM tracking.
    if (active_vis_ && ctx_) {
        active_vis_->on_audio_frame(*ctx_, ev);
    }
}

void AutonomousMasterMode::on_spectrum_frame(const SpectrumFrameEvent& ev) {
    // Mirror the overlay guard from on_audio_frame: while a picker /
    // settings overlay is open the screen is owned by the overlay UI,
    // so the vis's spectrum render would fight repaints. We just drop
    // the event - no internal state to update (unlike BPM tracking on
    // the audio path).
    if (overlay_ != Overlay::None) return;

    if (active_vis_ && ctx_) {
        active_vis_->on_spectrum_frame(*ctx_, ev);
    }
}

void AutonomousMasterMode::sync_spectrum_subscription(bool active) {
    // Always unsubscribe first so we start from a known state and the
    // subscriber count is monotonic per (active_vis, active) pair.
    // The DAL's unsubscribe is idempotent (returns 0 if nothing matches).
    DAL::unsubscribe_spectrum_frames("local");

    if (!active) return;
    if (!active_vis_) return;
    if (!active_vis_->power().needs_spectrum_frame) return;

    // (Re-)subscribe via the namespace-scope routing callback in
    // mode_machine.cpp; the callback fans out to s_active_mode-> on_spectrum_frame
    // which lands back in this class's override above.
    DAL::subscribe_spectrum_frames("local", &on_dal_spectrum_frame);
}

void AutonomousMasterMode::on_input_action(const InputEvent& ev) {
    // Picker / Settings act as toggles: a second press of the same
    // action closes the overlay. Per the input mapper's header
    // comment ("Picker/Settings as a toggle; menus and overlays
    // handle their own back semantics") there is no Back gesture.
    if (overlay_ == Overlay::Picker) {
        switch (ev.action) {
            case InputAction::Picker:
                overlay_ = Overlay::None;
                draw();
                return;
            case InputAction::Cycle: {
                const size_t n = picker_row_count();
                if (n > 0) overlay_cursor_ = (overlay_cursor_ + 1) % n;
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
            overlay_        = Overlay::Picker;
            overlay_cursor_ = 0;
            draw();
            return;
        case InputAction::Settings:
            overlay_        = Overlay::Settings;
            overlay_cursor_ = 0;
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
            // Route to the vis. The vis decides whether to act on it.
            if (active_vis_ && ctx_) {
                active_vis_->on_input_action(*ctx_, ev);
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

size_t AutonomousMasterMode::picker_row_count() const {
    return visualisation_registry().count() + 1;   // + "<- Menu"
}

bool AutonomousMasterMode::picker_row_is_back(size_t row) const {
    return row == visualisation_registry().count();
}

void AutonomousMasterMode::on_picker_confirm() {
    if (picker_row_is_back(overlay_cursor_)) {
        // "<- Menu" sentinel. Close overlay first so re-entry to
        // AutonomousMaster starts cleanly, then switch.
        overlay_ = Overlay::None;
        ModeMachine::switch_to(ModeId::Menu);
        return;
    }
    visualisations::Visualisation* picked =
        visualisation_registry().at(overlay_cursor_);
    if (!picked) {
        // Out-of-range: shouldn't happen but guard defensively.
        return;
    }
    if (!vis_capability_gate_open(*picked)) {
        // Greyed entry. Consume the Confirm without changing state.
        return;
    }
    if (picked == active_vis_) {
        overlay_ = Overlay::None;
        draw();
        return;
    }
    // Swap active vis. Tear down the old, set new, persist, bring up
    // new with a fresh entered_at_ms. Keep ctx_ in sync (today only
    // BeatPulse ships, so beat_pulse_context() is always the right
    // surface; Block 11+ will route per-vis contexts).
    if (active_vis_ && ctx_) active_vis_->exit(*ctx_);
    // Drop the outgoing vis's spectrum subscription before swapping so
    // the gate goes false transiently between vis (and stays false if
    // the incoming vis doesn't need spectrum).
    sync_spectrum_subscription(/*active=*/false);

    active_vis_ = picked;
    persistence::save_active_vis_id(picked->id());
    // Pick up the incoming vis's own singleton context (Block 11).
    ctx_ = &picked->context();
    if (ctx_) ctx_->set_paused(paused_);
    if (ctx_) ctx_->mark_entered(millis());
    if (ctx_) active_vis_->enter(*ctx_);
    sync_spectrum_subscription(/*active=*/true);
    refresh_status_label();
    overlay_ = Overlay::None;
    draw();
}

// ---------------------------------------------------------------------------
// Settings overlay
// ---------------------------------------------------------------------------

size_t AutonomousMasterMode::settings_row_count() const {
    if (!active_vis_) return 1;             // just "<- Back"
    const auto props = active_vis_->properties();
    return props.size + 1;                  // + "<- Back"
}

bool AutonomousMasterMode::settings_row_is_back(size_t row) const {
    if (!active_vis_) return row == 0;
    return row == active_vis_->properties().size;
}

void AutonomousMasterMode::on_settings_confirm() {
    if (settings_row_is_back(overlay_cursor_)) {
        overlay_ = Overlay::None;
        draw();
        return;
    }
    if (!active_vis_ || !ctx_) return;
    const auto props = active_vis_->properties();
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
            // Step the high byte (R channel) by 0x40. Cheap, visible,
            // distinguishable; bracelet hardware just sees a different
            // packed RGB. No swatch in v1.
            const uint32_t cv = cur.as_colour();
            const uint8_t  r  = static_cast<uint8_t>((cv >> 16) & 0xFF);
            const uint8_t  g  = static_cast<uint8_t>((cv >>  8) & 0xFF);
            const uint8_t  b  = static_cast<uint8_t>( cv        & 0xFF);
            const uint8_t  nr = static_cast<uint8_t>(r + 0x40);  // wraps at 256
            const uint32_t nv = (static_cast<uint32_t>(nr) << 16)
                              | (static_cast<uint32_t>(g)  <<  8)
                              |  static_cast<uint32_t>(b);
            next = PropertyValue::from_colour(nv);
            break;
        }
    }
    ctx_->set_property(def.key, next);
    // Block 11: notify the active vis so it can re-sync any cached
    // state derived from the property value (e.g. BeatPulse's
    // effects::Pulse colour cache). Without this, an edit to a
    // property like BeatPulse's "color" via Settings would stay stale
    // inside the vis until the next Cycle action. Vis that don't
    // cache anything ignore the hook (base-class default no-op).
    active_vis_->on_property_changed(*ctx_, def.key);
    draw();
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void AutonomousMasterMode::draw() {
    if (overlay_ == Overlay::Picker)   { draw_picker();   return; }
    if (overlay_ == Overlay::Settings) { draw_settings(); return; }

    // When the active vis owns the full screen (SpectrumBars and any
    // future vis that paints over the whole LCD), the mode skips its
    // BeatPulse-era chrome entirely. Without this guard the 50 ms
    // loop_tick clear-and-repaint cycle clobbers the vis's bars at
    // 20 Hz and the operator sees BeatPulse UI over invisible bars.
    if (active_vis_ != nullptr && active_vis_->wants_full_screen()) return;

    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});

    // Status strip: small active-vis name at the very top. Size 1 so it
    // doesn't fight the size-3 colour title below it.
    if (status_label_buf_[0] != '\0') {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 0, status_label_buf_, YELLOW, BLACK, 1});
    }

    char title[32];
    std::snprintf(title, sizeof(title), " %s%s",
                  beat_pulse_colour_label(), paused_ ? " : Muted" : "");
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 12, title, WHITE, BLACK, 3});

    char bpm[24];
    const float est = beat_pulse_estimated_bpm();
    if (est > 0.0f) {
        std::snprintf(bpm, sizeof(bpm), " BPM: %.0f", est);
    } else {
        std::snprintf(bpm, sizeof(bpm), " BPM: ---");
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 45, bpm, WHITE, BLACK, 2});

    // Batt + IR fire counter on one line. IR count is <=4 chars with k/M
    // suffix above 10000 so the line stays within 240 px at size 2.
    const uint32_t ir = DAL::driver_send_count("ir-pixmob");
    char ir_buf[8];
    if      (ir >= 1000000) std::snprintf(ir_buf, sizeof(ir_buf), "%luM",
                                          (unsigned long)(ir / 1000000));
    else if (ir >=   10000) std::snprintf(ir_buf, sizeof(ir_buf), "%luk",
                                          (unsigned long)(ir / 1000));
    else                    std::snprintf(ir_buf, sizeof(ir_buf), "%lu",
                                          (unsigned long)ir);
    char batt[40];
    std::snprintf(batt, sizeof(batt), "Batt: %d%% IR: %s",
                  DAL::battery_level("local"), ir_buf);
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 75, batt, WHITE, BLACK, 2});

    // Flux meter (frame + bar + threshold marker), composed from
    // FillRect primitives.
    const int meterX = 10, meterY = 110, meterW = 220, meterH = 14;
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        meterX, meterY,             meterW, 1,      WHITE});
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        meterX, meterY + meterH-1,  meterW, 1,      WHITE});
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        meterX, meterY,             1,      meterH, WHITE});
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        meterX + meterW-1, meterY,  1,      meterH, WHITE});

    const float ratio = (baseline_flux_ > 1.0f)
                            ? current_flux_ / baseline_flux_
                            : 0.0f;
    int barW = (int)(ratio * 50.0f);
    if (barW < 0)            barW = 0;
    if (barW > meterW - 2)   barW = meterW - 2;
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        meterX + 1, meterY + 1, barW, meterH - 2, GREEN});

    const int thrX = meterX + (int)(kBeatMultiplier * 50.0f);
    if (thrX < meterX + meterW) {
        DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
            thrX, meterY - 2, 1, meterH + 4, RED});
    }

    // Operator hint footer for the active vis root. Block 13 footer
    // standardisation: state the actual gesture mapping for this layer
    // (B = vis-specific cycle, A-hold = open settings overlay, B-hold =
    // open picker). The pulse-rect repaint will overdraw this between
    // loop_tick frames; the 50 ms draw cadence puts it back. Size 1 to
    // keep the line under 240 px (~38 char budget).
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        4, 128, "B: cycle  A-hold: set  B-hold: pick",
        WHITE, BLACK, 1});
}

void AutonomousMasterMode::draw_picker() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "Visualisation", WHITE, BLACK, 2});

    const size_t vis_count = visualisation_registry().count();
    const size_t rows = picker_row_count();
    int y = 30;
    for (size_t i = 0; i < rows; ++i) {
        const bool sel = (i == overlay_cursor_);
        char buf[40];
        if (i < vis_count) {
            visualisations::Visualisation* v = visualisation_registry().at(i);
            const bool gated = !vis_capability_gate_open(*v);
            std::snprintf(buf, sizeof(buf), "%s %s%s",
                          sel ? ">" : " ",
                          v->display_name(),
                          gated ? " (-)" : "");
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, y, buf,
                sel   ? YELLOW : (gated ? RED : WHITE),
                BLACK, 2});
        } else {
            // "<- Menu" sentinel.
            std::snprintf(buf, sizeof(buf), "%s %s",
                          sel ? ">" : " ", "<- Menu");
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, y, buf, sel ? YELLOW : WHITE, BLACK, 2});
        }
        y += 18;
    }

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 128, "B: cycle  A: select  B-hold: close",
        WHITE, BLACK, 1});
}

void AutonomousMasterMode::draw_settings() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "Settings", WHITE, BLACK, 2});

    int y = 30;
    if (active_vis_ && ctx_) {
        const auto props = active_vis_->properties();
        if (props.size == 0) {
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, y, "No settings", WHITE, BLACK, 2});
            y += 18;
        }
        for (size_t i = 0; i < props.size; ++i) {
            const PropertyDef& def = props[i];
            const PropertyValue v  = ctx_->get_property(def.key);
            const bool sel = (i == overlay_cursor_);
            char buf[40];
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
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, y, buf, sel ? YELLOW : WHITE, BLACK, 2});
            y += 18;
        }
    }

    // "<- Back" row sentinel.
    {
        const size_t back_row = settings_row_count() - 1;
        const bool sel = (overlay_cursor_ == back_row);
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      sel ? ">" : " ", "<- Back");
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, y, buf, sel ? YELLOW : WHITE, BLACK, 2});
    }

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 128, "B: cycle  A: edit  A-hold: close",
        WHITE, BLACK, 1});
}

}  // namespace modes
}  // namespace nocturnation
