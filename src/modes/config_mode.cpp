// ConfigMode implementation.

#include "config_mode.h"

#include "firmware_version.h"
#include "persistence.h"
#include "dal/dal.h"
#include "../dal/drivers/led_strip_driver.h"
#include "hal/hal.h"                        // for ir_tx_ext() emitter probe
#include "../dal/drivers/local_driver.h"   // for set_pulse_enabled gating
#include "../dal/drivers/pixmob_ir_driver.h"   // for internal/external IR toggles
#include "output_bindings/pixmob_ir.h"
#include "plugins/property_bag.h"
#include "shows/show.h"
#include "shows/show_registry.h"
#include "widgets/beat_bar.h"
#include "widgets/spectrum_bars.h"
#include "pixmob_protocol.h"

#include <cstdio>
#include <cstring>

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#else
extern "C" uint32_t millis();
#endif

namespace nocturnation {
namespace modes {

using namespace nocturnation::dal;
using nocturnation::hal::ButtonId;
using nocturnation::hal::ButtonEvent;

// ConfigMode reads/writes operator-mutable settings. Channel + repeat
// for the Lume route through persistence:: shared helpers (Block 9).
// The NocturNation receive-filter slv_group setting (was Config >
// ESP-NOW > Group) now lives at the top level as a direct-action
// "Group" item per the post-Epic-4.65 menu restructure.
namespace {

// Submenu row geometry (used by draw_top + the variable-row submenus).
// Footer hint lives at y=122 at size 1 (~8 px tall). Body rows are size 2
// at 16 px stride. Most screens use a row origin around y=22 (top menu)
// or y=30 (submenus, leaving room for a size-2 title above).
constexpr int kRowStride       = 16;
constexpr int kFooterTextY     = 122;
constexpr int kBodyBottomLimit = kFooterTextY - 4;   // 4 px gap above footer

// Returns the index of the first visible row given the selected index,
// total item count, and the maximum rows the screen can show. Keeps the
// selected row in view via a minimal sliding window - if the selection
// is past the bottom of the current window, scroll down so it sits on
// the last visible row; otherwise leave the window where it is.
// Stateless: derived purely from the inputs, recomputed each draw.
size_t scroll_offset(size_t selected, size_t total, size_t max_visible) {
    if (total <= max_visible) return 0;
    size_t first = 0;
    if (selected >= max_visible) {
        first = selected - max_visible + 1;
    }
    // Never scroll past the end (avoids empty rows at the bottom).
    if (first + max_visible > total) {
        first = total - max_visible;
    }
    return first;
}

}  // namespace

// Out-of-class definitions for the ODR-used static constexpr members.
constexpr ConfigMode::TopEntry    ConfigMode::kTop[5];
constexpr ConfigMode::PickerEntry ConfigMode::kConnectivity[4];
constexpr ConfigMode::PickerEntry ConfigMode::kUtilities[2];
constexpr const char* ConfigMode::kWifiItems[];
constexpr const char* ConfigMode::kDmxItems[];

void ConfigMode::enter() {
    level_           = Level::Top;
    active_picker_   = SubMenu::None;
    active_sub_      = SubMenu::None;
    top_selected_    = 0;
    picker_selected_ = 0;
    sub_selected_    = 0;
    confirm_until_ms_ = 0;
    last_drawn_battery_ = -2;     // force first battery redraw
    level_tuning_audio_active_ = false;
    dir_id_edit_active_ = false;
    dir_id_edit_cursor_ = DirIdCursor::HiNibble;
    scan_phase_         = ScanPhase::Idle;
    wifi_scanner_.reset();
    draw();
}

void ConfigMode::exit() {
    // Defensive: if the operator switches modes from inside the Level
    // Tuning sub (via Btn2-long -> Menu from Top is the only Config
    // exit path that doesn't go through B-hold-pops), stop the mic so
    // it doesn't leak into the next mode.
    if (level_tuning_audio_active_) {
        level_tuning_audio_exit();
    }
}

void ConfigMode::loop_tick() {
    // System submenu has live read-outs (battery percent); poll at
    // ~3 Hz when shown - the battery ADC jitters by an integer
    // percent between adjacent ticks, and an unthrottled redraw
    // pulled the whole screen on every loop, causing visible flicker.
    // Confirmation flash for factory reset also clears here.
    const uint32_t now = millis();
    if (confirm_until_ms_ != 0 && now >= confirm_until_ms_) {
        confirm_until_ms_ = 0;
        draw();
        return;
    }
    // Level Tuning Live mode: redraw at ~20 Hz so the bars track
    // audio smoothly. Skipped in fixed % modes where the display
    // doesn't change between button presses.
    if (level_tuning_audio_active_
     && level_tuning_mode_ == LevelTuningMode::Live
     && level_ == Level::Sub
     && active_sub_ == SubMenu::LevelTuning) {
        if (now - last_level_tuning_draw_ms_ > 50) {
            draw();
            last_level_tuning_draw_ms_ = now;
            return;
        }
    }
    if (level_ == Level::Sub && active_sub_ == SubMenu::System) {
        if (now - last_system_redraw_ms_ < 333) return;
        last_system_redraw_ms_ = now;
        const int batt = DAL::battery_level("local");
        if (batt != last_drawn_battery_) {
            last_drawn_battery_ = batt;
            draw();
        }
    }
    // Wi-Fi channel scan: two-tick lifecycle so "Scanning..." paints
    // before the blocking scan call kicks off.
    if (level_ == Level::Sub && active_sub_ == SubMenu::EspNow) {
        run_scan_if_needed();
    }
}

void ConfigMode::on_button_event(const ButtonPressEvent& ev) {
    if (ev.kind != ButtonEvent::Pressed
     && ev.kind != ButtonEvent::LongPressed) return;

    // BtnB long-press pops one level per the navigation contract. PixMob's
    // workflow sub-state (SetGroupId / GroupTarget) gets an extra pop
    // before it returns to the PixMob menu. A leaf submenu reached via a
    // picker pops back to that picker; reached directly from Top, it pops
    // back to Top.
    if (ev.id == ButtonId::Btn2 && ev.kind == ButtonEvent::LongPressed) {
        if (level_ == Level::Top) {
            ModeMachine::switch_to(ModeId::Menu);
        } else if (level_ == Level::Sub
                && active_sub_ == SubMenu::PixMob
                && pixmob_state_ != PixMobState::Menu) {
            pixmob_state_     = PixMobState::Menu;
            confirm_until_ms_ = 0;
            draw();
        } else if (level_ == Level::Sub
                && active_sub_ == SubMenu::EspNow
                && dir_id_edit_active_) {
            // Hex-edit drill-down exits back to the EspNow menu.
            // Value is already persisted (saved on every edit), so
            // no save action is needed here.
            dir_id_edit_active_ = false;
            dir_id_edit_cursor_ = DirIdCursor::HiNibble;
            draw();
        } else if (level_ == Level::Sub
                && active_sub_ == SubMenu::EspNow
                && scan_phase_ != ScanPhase::Idle) {
            // Wi-Fi scan drill-down exits back to the EspNow menu.
            // B-hold is honoured in Done/Failed phases; during
            // Requested/Running the caller can still press it but
            // the in-flight scan will complete on the next loop_tick
            // (WiFi.scanNetworks is blocking and non-cancellable).
            scan_phase_ = ScanPhase::Idle;
            wifi_scanner_.reset();
            draw();
        } else if (level_ == Level::Sub && active_picker_ != SubMenu::None) {
            // Came in via a picker - return to the picker, not Top.
            if (active_sub_ == SubMenu::LevelTuning
             && level_tuning_audio_active_) {
                level_tuning_audio_exit();
            }
            level_      = Level::Picker;
            active_sub_ = SubMenu::None;
            draw();
        } else {
            // Either Sub-reached-from-Top or Picker - both pop to Top.
            level_         = Level::Top;
            active_picker_ = SubMenu::None;
            active_sub_    = SubMenu::None;
            draw();
        }
        return;
    }

    if (ev.kind != ButtonEvent::Pressed) return;
    switch (level_) {
        case Level::Top:    handle_top(ev);    break;
        case Level::Picker: handle_picker(ev); break;
        case Level::Sub:    handle_sub(ev);    break;
    }
}

// ------------------------------------------------------------------
// Top level
// ------------------------------------------------------------------

void ConfigMode::handle_top(const ButtonPressEvent& ev) {
    if (ev.id == ButtonId::Btn2) {
        top_selected_ = (top_selected_ + 1) % kTopCount;
        draw();
        return;
    }
    if (ev.id != ButtonId::Btn1) return;

    const TopEntry& entry = kTop[top_selected_];
    switch (entry.action) {
        case TopAction::GroupId:
            // Increment 0..6 wrapping. The wire protocol carries an
            // 8-bit group, but in practice the show only needs a
            // handful for now (DynamicShow routes kick/snare/hi-hat
            // across 3, with headroom for a few more); a tight cycle
            // is far faster to navigate from the front buttons than
            // 256 presses. Operators can persist a wider value via
            // tooling if needed - this is a UI cap, not a wire cap.
            persistence::save_lume_group(
                static_cast<uint8_t>((persistence::load_lume_group() + 1) % 7));
            draw();
            return;
        case TopAction::Drill:
            break;
    }

    // Drill into either a picker (Connectivity / Utilities) or a leaf
    // submenu (Display / System). The picker level is only entered for
    // SubMenu::Connectivity and SubMenu::Utilities; everything else
    // skips straight to Level::Sub with active_picker_ left at None
    // (so B-hold from that leaf goes back to Top, per the contract).
    if (entry.target == SubMenu::Connectivity
     || entry.target == SubMenu::Utilities) {
        level_           = Level::Picker;
        active_picker_   = entry.target;
        picker_selected_ = 0;
    } else {
        level_         = Level::Sub;
        active_picker_ = SubMenu::None;
        active_sub_    = entry.target;
        sub_selected_  = 0;
        last_drawn_battery_ = -2;
        // Fresh entry into PixMob always lands on the menu screen
        // (not in a previous workflow). PixMob isn't currently a
        // direct top-level target but keep the reset here so the
        // invariant holds if Utilities-style routing ever changes.
        pixmob_state_     = PixMobState::Menu;
        pixmob_selected_  = 0;
        confirm_until_ms_ = 0;
    }
    draw();
}

void ConfigMode::draw_top() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "Config", WHITE, BLACK, 2});

    // 5 top-menu items fit comfortably between y=22 and the footer at
    // y=122 at size-2 stride 16 (5 rows = 80 px). scroll_offset() is
    // a no-op at this count but stays in place so further items don't
    // require touching the draw call.
    constexpr int  kRowY0     = 22;
    const size_t   max_visible = static_cast<size_t>(
        (kBodyBottomLimit - kRowY0) / kRowStride);
    const size_t   first      = scroll_offset(top_selected_, kTopCount, max_visible);
    const size_t   last_excl  = (first + max_visible > kTopCount)
                                ? kTopCount
                                : first + max_visible;

    for (size_t i = first; i < last_excl; ++i) {
        const bool sel = (i == top_selected_);
        char buf[28];
        if (kTop[i].action == TopAction::GroupId) {
            std::snprintf(buf, sizeof(buf), "%s %s: %u",
                          sel ? ">" : " ", kTop[i].label,
                          (unsigned)persistence::load_lume_group());
        } else {
            std::snprintf(buf, sizeof(buf), "%s %s",
                          sel ? ">" : " ", kTop[i].label);
        }
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, kRowY0 + static_cast<int>(i - first) * kRowStride, buf,
            sel ? YELLOW : WHITE, BLACK, 2});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "B: cycle  A: select  B-hold: back",
        WHITE, BLACK, 1});
}

// ------------------------------------------------------------------
// Picker level (Connectivity / Utilities)
// ------------------------------------------------------------------

const ConfigMode::PickerEntry* ConfigMode::picker_entries() const {
    switch (active_picker_) {
        case SubMenu::Connectivity: return kConnectivity;
        case SubMenu::Utilities:    return kUtilities;
        default:                    return nullptr;
    }
}

size_t ConfigMode::picker_count() const {
    switch (active_picker_) {
        case SubMenu::Connectivity: return kConnectivityCount;
        case SubMenu::Utilities:    return kUtilitiesCount;
        default:                    return 0;
    }
}

const char* ConfigMode::picker_title() const {
    switch (active_picker_) {
        case SubMenu::Connectivity: return "Connectivity";
        case SubMenu::Utilities:    return "Utilities";
        default:                    return "";
    }
}

void ConfigMode::handle_picker(const ButtonPressEvent& ev) {
    const size_t count = picker_count();
    if (count == 0) return;
    if (ev.id == ButtonId::Btn2) {
        picker_selected_ = (picker_selected_ + 1) % count;
        draw();
        return;
    }
    if (ev.id == ButtonId::Btn1) {
        const PickerEntry& entry = picker_entries()[picker_selected_];
        level_        = Level::Sub;
        active_sub_   = entry.target;
        sub_selected_ = 0;
        last_drawn_battery_ = -2;
        pixmob_state_     = PixMobState::Menu;
        pixmob_selected_  = 0;
        confirm_until_ms_ = 0;
        if (entry.target == SubMenu::LevelTuning) {
            level_tuning_audio_enter();
        }
        draw();
    }
}

void ConfigMode::draw_picker() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, picker_title(), WHITE, BLACK, 2});

    const PickerEntry* entries = picker_entries();
    const size_t       count   = picker_count();
    if (entries == nullptr || count == 0) return;

    constexpr int  kRowY0     = 30;
    const size_t   max_visible = static_cast<size_t>(
        (kBodyBottomLimit - kRowY0) / kRowStride);
    const size_t   first      = scroll_offset(picker_selected_, count, max_visible);
    const size_t   last_excl  = (first + max_visible > count)
                                ? count
                                : first + max_visible;

    for (size_t i = first; i < last_excl; ++i) {
        const bool sel = (i == picker_selected_);
        char buf[28];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      sel ? ">" : " ", entries[i].label);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, kRowY0 + static_cast<int>(i - first) * kRowStride, buf,
            sel ? YELLOW : WHITE, BLACK, 2});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "B: cycle  A: select  B-hold: back",
        WHITE, BLACK, 1});
}

// ------------------------------------------------------------------
// Submenu dispatch
// ------------------------------------------------------------------

void ConfigMode::handle_sub(const ButtonPressEvent& ev) {
    switch (active_sub_) {
        case SubMenu::Show:        handle_show(ev);        break;
        case SubMenu::System:      handle_system(ev);      break;
        case SubMenu::IR:          handle_ir(ev);          break;
        case SubMenu::Display:     handle_display(ev);     break;
        case SubMenu::EspNow:      handle_espnow(ev);      break;
        case SubMenu::PixMob:      handle_pixmob(ev);      break;
        case SubMenu::LevelTuning: handle_level_tuning(ev); break;
        case SubMenu::LedStrip:    handle_led_strip(ev); break;
        case SubMenu::WiFi:
        case SubMenu::Dmx:
            // Stub submenus accept Btn2 cycling for read-only browsing
            // of their planned-items list, but Btn1 is a no-op until
            // the relevant Epic wires real behaviour in.
            if (ev.id == ButtonId::Btn2) {
                sub_selected_ = (sub_selected_ + 1) % stub_item_count();
                draw();
            }
            break;
        default: break;
    }
}

void ConfigMode::draw_sub() {
    switch (active_sub_) {
        case SubMenu::Show:        draw_show(); break;
        case SubMenu::Display:     draw_display(); break;
        case SubMenu::IR:          draw_ir(); break;
        case SubMenu::EspNow:      draw_espnow(); break;
        case SubMenu::WiFi:        draw_stub("WiFi", kWifiItems, kWifiItemCount, "Epic 4"); break;
        case SubMenu::Dmx:         draw_stub("DMX",  kDmxItems,  kDmxItemCount,  "Epic 7"); break;
        case SubMenu::LedStrip:    draw_led_strip(); break;
        case SubMenu::PixMob:      draw_pixmob(); break;
        case SubMenu::LevelTuning: draw_level_tuning(); break;
        case SubMenu::System:      draw_system(); break;
        default: break;
    }
}

size_t ConfigMode::stub_item_count() const {
    switch (active_sub_) {
        case SubMenu::Display: return kDisplayFunctionalItemCount;
        case SubMenu::IR:      return ir_item_count();
        case SubMenu::EspNow:  return kEspNowFunctionalItemCount;
        case SubMenu::WiFi:    return kWifiItemCount;
        case SubMenu::Dmx:     return kDmxItemCount;
        default:               return 1;
    }
}

void ConfigMode::draw_stub(const char* title,
                           const char* const* items, size_t count,
                           const char* epic_tag) {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, title, WHITE, BLACK, 2});
    char banner[24];
    std::snprintf(banner, sizeof(banner), "TBD %s", epic_tag);
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        150, 10, banner, YELLOW, BLACK, 1});

    for (size_t i = 0; i < count; ++i) {
        const bool sel = (i == sub_selected_);
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      sel ? ">" : " ", items[i]);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 30 + (int)i * 16, buf,
            sel ? YELLOW : WHITE, BLACK, 2});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "B: cycle  B-hold: back",
        WHITE, BLACK, 1});
}

// -------------------------------------------------------------------------
// Show submenu (Epic 4.7 Block 1)
//
// Picker over the registered Shows (show_registry). Btn2 cycles the
// cursor; Btn1 persists the selection as active_show in NVS. Mirrors
// DirectorMode's in-flight picker but reachable from Config
// without entering Director mode. Empty registry is defensive only -
// production builds always register at least SimpleBeatShow.
// -------------------------------------------------------------------------

void ConfigMode::handle_show(const ButtonPressEvent& ev) {
    const size_t count = shows::show_registry().count();
    if (count == 0) return;
    // Epic 15 bench follow-up: list "DMX Bridge" as a synthetic row
    // at the end. It's not a Show, so picking it doesn't save an
    // active_show_id - it switches the device to DmxBridge mode
    // immediately (mirroring the Director-mode picker's behaviour
    // for the same entry). DmxBridge is intentionally NOT boot-
    // persisted per persistence::is_persisted_runtime_mode (Q4
    // 2026-06-05 decision); Config-menu pick is one-time.
    const size_t total_rows = count + 1;
    const size_t dmx_bridge_row = count;
    if (ev.id == ButtonId::Btn2) {
        sub_selected_ = (sub_selected_ + 1) % total_rows;
        draw();
        return;
    }
    if (ev.id == ButtonId::Btn1) {
        if (sub_selected_ == dmx_bridge_row) {
            ModeMachine::switch_to(ModeId::DmxBridge);
            return;
        }
        shows::Show* picked = shows::show_registry().at(sub_selected_);
        if (picked) {
            persistence::save_active_show_id(picked->id());
            confirm_until_ms_ = millis() + kConfirmFlashMs;
        }
        draw();
    }
}

void ConfigMode::draw_show() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "Show", WHITE, BLACK, 2});

    const size_t count = shows::show_registry().count();
    if (count == 0) {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 30, "(no shows registered)", WHITE, BLACK, 1});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 122, "B-hold: back", WHITE, BLACK, 1});
        return;
    }

    const char* active_id = persistence::load_active_show_id();

    constexpr int kRowY0 = 22;
    const size_t  max_visible = static_cast<size_t>(
        (kBodyBottomLimit - kRowY0) / kRowStride);
    // Epic 15: + 1 synthetic "DMX Bridge" row at the end.
    const size_t  total_rows  = count + 1;
    const size_t  dmx_row     = count;
    const size_t  first       = scroll_offset(sub_selected_, total_rows, max_visible);
    const size_t  last_excl   = (first + max_visible > total_rows)
                                ? total_rows
                                : first + max_visible;

    for (size_t i = first; i < last_excl; ++i) {
        const bool sel = (i == sub_selected_);
        char buf[40];
        if (i == dmx_row) {
            std::snprintf(buf, sizeof(buf), "%s DMX Bridge",
                          sel ? ">" : " ");
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, kRowY0 + static_cast<int>(i - first) * kRowStride, buf,
                sel ? YELLOW : WHITE, BLACK, 2});
            continue;
        }
        shows::Show* s = shows::show_registry().at(i);
        const bool   active = s && active_id
                              && std::strcmp(s->id(), active_id) == 0;
        std::snprintf(buf, sizeof(buf), "%s %s%s",
                      sel ? ">" : " ",
                      s ? s->display_name() : "(null)",
                      active ? " *" : "");
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, kRowY0 + static_cast<int>(i - first) * kRowStride, buf,
            sel ? YELLOW : (active ? GREEN : WHITE), BLACK, 2});
    }

    // Brief "Saved" linger after Btn1 confirm.
    if (confirm_until_ms_ > millis()) {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 122, "Saved.", GREEN, BLACK, 1});
    } else {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 122, "B: cycle  A: select  B-hold: back",
            WHITE, BLACK, 1});
    }
}

// -------------------------------------------------------------------------
// Level Tuning submenu (Epic 4.7 Block 2)
//
// Live mode: the audio analyser runs while the sub is active and the
// widgets render incoming flux / spectrum data. Btn2 cycles the
// display mode (Live / 25 / 50 / 75 / 100 %); the four percentage
// modes override to fixed values so a developer can verify the
// IR + ESP-NOW path without audio. Btn1 fires a render_fx pulse at
// the displayed level so Lumes pulse and the Director's
// PixMobIrBinding transmits.
//
// Audio input starts on entering the sub and stops on the B-hold
// pop or any other transition out. Spectrum data is taken from the
// AudioFrameEvent's per-band perceptual summary (mud / sub_bass /
// bass / low_mids / midrange / high_mids / presence / air) so the
// sub doesn't need a separate spectrum-frame subscription.
// -------------------------------------------------------------------------

namespace {
// Flux-meter tuning - mirrors SimpleBeatShow exactly so the display
// reads the same on bench and in Director mode.
constexpr float kLevelTuningBeatMultiplier = 2.5f;
constexpr float kLevelTuningBaselineAlpha  = 0.02f;
constexpr float kLevelTuningVolumeGate     = 500.0f;

// Bar widget at 220 px x 14 px - 218 px drawable inside the frame.
constexpr float kLevelTuningBarScale       = 50.0f / 218.0f;
constexpr float kLevelTuningMarkerFraction =
    kLevelTuningBeatMultiplier * 50.0f / 218.0f;

// Spectrum log-normalisation - matches SpectrumBarsWidget defaults.
constexpr float kLevelTuningMagFloorLog2   = 13.0f;
constexpr float kLevelTuningSensScale      = 0.025f;
constexpr uint8_t kLevelTuningSensitivity  = 5;

inline float normalise_band(float band_sum) {
    if (band_sum <= 0.0f) return 0.0f;
    const float log_mag = std::log2(1.0f + band_sum);
    float v = (log_mag - kLevelTuningMagFloorLog2)
            * static_cast<float>(kLevelTuningSensitivity)
            * kLevelTuningSensScale;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}
}  // namespace

float ConfigMode::mode_to_fraction(LevelTuningMode m) {
    switch (m) {
        case LevelTuningMode::Live:   return 0.0f;
        case LevelTuningMode::Pct25:  return 0.25f;
        case LevelTuningMode::Pct50:  return 0.50f;
        case LevelTuningMode::Pct75:  return 0.75f;
        case LevelTuningMode::Pct100: return 1.0f;
    }
    return 0.0f;
}

const char* ConfigMode::mode_label(LevelTuningMode m) {
    switch (m) {
        case LevelTuningMode::Live:   return "Live";
        case LevelTuningMode::Pct25:  return " 25%";
        case LevelTuningMode::Pct50:  return " 50%";
        case LevelTuningMode::Pct75:  return " 75%";
        case LevelTuningMode::Pct100: return "100%";
    }
    return "?";
}

void ConfigMode::level_tuning_audio_enter() {
    level_tuning_prev_bass_   = 0.0f;
    level_tuning_baseline_    = 100.0f;
    level_tuning_flux_        = 0.0f;
    level_tuning_level_rms_   = 0.0f;
    for (size_t i = 0; i < 7; ++i) level_tuning_spectrum_[i] = 0.0f;
    level_tuning_mode_        = LevelTuningMode::Live;
    DAL::start_audio_input("local", 16000, 512);
    level_tuning_audio_active_ = true;
}

void ConfigMode::level_tuning_audio_exit() {
    DAL::stop_audio_input("local");
    level_tuning_audio_active_ = false;
}

void ConfigMode::on_audio_frame(const dal::AudioFrameEvent& ev) {
    if (!level_tuning_audio_active_) return;

    // Flux + baseline - same math as SimpleBeatShow's flux meter.
    level_tuning_level_rms_ = ev.overall_rms;
    if (level_tuning_level_rms_ < kLevelTuningVolumeGate) {
        level_tuning_prev_bass_ = 0.0f;
    } else {
        float flux = ev.bass_energy - level_tuning_prev_bass_;
        if (flux < 0) flux = 0;
        level_tuning_prev_bass_ = ev.bass_energy;
        level_tuning_flux_      = flux;
        level_tuning_baseline_  =
            level_tuning_baseline_ * (1.0f - kLevelTuningBaselineAlpha)
            + flux * kLevelTuningBaselineAlpha;
    }

    // 7 perceptual bands from the AudioFrameEvent's pre-aggregated
    // per-band sums. Maps 1:1 to SpectrumBarsWidget's band order.
    level_tuning_spectrum_[0] = normalise_band(ev.sub_bass);
    level_tuning_spectrum_[1] = normalise_band(ev.bass);
    level_tuning_spectrum_[2] = normalise_band(ev.low_mids);
    level_tuning_spectrum_[3] = normalise_band(ev.midrange);
    level_tuning_spectrum_[4] = normalise_band(ev.high_mids);
    level_tuning_spectrum_[5] = normalise_band(ev.presence);
    level_tuning_spectrum_[6] = normalise_band(ev.air);
}

void ConfigMode::handle_level_tuning(const ButtonPressEvent& ev) {
    if (ev.id == ButtonId::Btn2) {
        level_tuning_mode_ = static_cast<LevelTuningMode>(
            (static_cast<uint8_t>(level_tuning_mode_) + 1)
            % kLevelTuningModeCount);
        draw();
        return;
    }
    if (ev.id == ButtonId::Btn1) {
        // Intensity:
        //   Live -> 255 (loud test pulse for visible verification)
        //   Pct25..100 -> 64 / 128 / 192 / 255
        uint8_t intensity;
        if (level_tuning_mode_ == LevelTuningMode::Live) {
            intensity = 255;
        } else {
            const float frac = mode_to_fraction(level_tuning_mode_);
            intensity = static_cast<uint8_t>(frac * 255.0f + 0.5f);
        }
        dal::RgbPulseEvent pulse{};
        pulse.r = intensity;
        pulse.g = intensity;
        pulse.b = intensity;
        pulse.attack  = pixmob::T_32_MS;
        pulse.sustain = pixmob::T_96_MS;
        pulse.release = pixmob::T_96_MS;
        pulse.chance  = pixmob::CHANCE_100;
        DAL::render_fx("00:00", pulse);
        confirm_until_ms_ = millis() + kConfirmFlashMs;
        draw();
    }
}

void ConfigMode::draw_level_tuning() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "Level Tuning", WHITE, BLACK, 2});

    // Mode readout top-right.
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        195, 6, mode_label(level_tuning_mode_), YELLOW, BLACK, 1});

    // Beat bar (220 px x 14 px).
    {
        widgets::BeatBarWidget bar;
        if (level_tuning_mode_ == LevelTuningMode::Live) {
            const float ratio = (level_tuning_baseline_ > 1.0f)
                                    ? level_tuning_flux_ / level_tuning_baseline_
                                    : 0.0f;
            bar.update(ratio * kLevelTuningBarScale,
                       kLevelTuningMarkerFraction);
        } else {
            const float frac = mode_to_fraction(level_tuning_mode_);
            // Marker still at the audio threshold so the operator can
            // compare a fixed level against where a beat would fire.
            bar.update(frac, kLevelTuningMarkerFraction);
        }
        bar.draw(10, 28, 220, 14);
    }

    // Spectrum bars (full-width band of 70 px height).
    {
        widgets::SpectrumBarsWidget spec;
        float values[widgets::kSpectrumBandCount];
        if (level_tuning_mode_ == LevelTuningMode::Live) {
            for (size_t i = 0; i < widgets::kSpectrumBandCount; ++i) {
                values[i] = level_tuning_spectrum_[i];
            }
        } else {
            const float frac = mode_to_fraction(level_tuning_mode_);
            for (size_t i = 0; i < widgets::kSpectrumBandCount; ++i) {
                values[i] = frac;
            }
        }
        spec.update(values);
        spec.draw(0, 50, 240, 70);
    }

    if (confirm_until_ms_ > millis()) {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 122, "Fired.", GREEN, BLACK, 1});
    } else {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 122, "B: mode  A: fire  B-hold: back",
            WHITE, BLACK, 1});
    }
}

// -------------------------------------------------------------------------
// Display submenu (functional: Pulse Enable toggle + persists)
//
// Pulse Enable gates the LocalDriver's RgbPulse handler only - all
// other Display* output (status text, menus, etc.) keeps working
// when this is OFF. Useful when an operator wants the screen to
// show diagnostics/UI but not flash on beats.
// -------------------------------------------------------------------------

void ConfigMode::handle_display(const ButtonPressEvent& ev) {
    if (ev.id == ButtonId::Btn2) {
        sub_selected_ = (sub_selected_ + 1) % kDisplayFunctionalItemCount;
        draw();
        return;
    }
    if (ev.id == ButtonId::Btn1
     && (DisplayItem)sub_selected_ == DisplayItem::PulseEnable) {
        const bool next = !dal::local_driver_instance()->pulse_enabled();
        dal::local_driver_instance()->set_pulse_enabled(next);
        persistence::save_screen_pulse_enabled(next);
        draw();
    }
}

void ConfigMode::draw_display() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "Display", WHITE, BLACK, 2});

    char ena[24];
    std::snprintf(ena, sizeof(ena), "Pulse: %s",
                  dal::local_driver_instance()->pulse_enabled()
                      ? "ON" : "OFF");
    const char* lines[kDisplayFunctionalItemCount] = { ena };

    for (size_t i = 0; i < kDisplayFunctionalItemCount; ++i) {
        const bool sel = (i == sub_selected_);
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      sel ? ">" : " ", lines[i]);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 30 + (int)i * 16, buf,
            sel ? YELLOW : WHITE, BLACK, 2});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "B: cycle  A: select  B-hold: back",
        WHITE, BLACK, 1});
}

// -------------------------------------------------------------------------
// IR submenu (functional: Enable toggle + Protocol info)
//
// The "Lume Group" item that used to surface PixMobIrBinding's `group`
// property here was dropped post-Epic-4.65: the protocol IR group is
// now relay-driven via inbound LIGHT_PULSE target_group (Block 5);
// the per-binding group property still exists as broadcast fallback
// (target_group=0) and is reachable via the auto-generated settings
// overlay, but it confused operators on the IR screen.
// -------------------------------------------------------------------------

size_t ConfigMode::ir_item_count() {
    // Enable + Protocol always; Internal + External only where a second
    // emitter exists (Plus2). hal::HAL::ir_tx_ext() is nullptr otherwise.
    return (hal::HAL::ir_tx_ext() != nullptr) ? 4 : 2;
}

ConfigMode::IRItem ConfigMode::ir_item_at(size_t pos) {
    if (hal::HAL::ir_tx_ext() != nullptr) {
        // Four items, in enum order: Enable, Internal, External, Protocol.
        return (IRItem)pos;
    }
    // Two items: Enable then Protocol.
    return (pos == 0) ? IRItem::EnableDisable : IRItem::Protocol;
}

void ConfigMode::handle_ir(const ButtonPressEvent& ev) {
    if (ev.id == ButtonId::Btn2) {
        sub_selected_ = (sub_selected_ + 1) % ir_item_count();
        draw();
        return;
    }
    if (ev.id == ButtonId::Btn1) {
        switch (ir_item_at(sub_selected_)) {
            case IRItem::EnableDisable: {
                const bool next = !DAL::driver_enabled("ir-pixmob");
                DAL::set_driver_enabled("ir-pixmob", next);
                persistence::save_ir_enabled(next);
                break;
            }
            case IRItem::InternalEmitter: {
                auto* d = dal::pixmob_ir_driver_instance();
                const bool next = !d->internal_enabled();
                d->set_internal_enabled(next);
                persistence::save_internal_ir_enabled(next);
                break;
            }
            case IRItem::ExternalEmitter: {
                auto* d = dal::pixmob_ir_driver_instance();
                const bool next = !d->external_enabled();
                d->set_external_enabled(next);
                persistence::save_external_ir_enabled(next);
                break;
            }
            case IRItem::Protocol:
                break;   // info-only - no Btn1 action.
        }
        draw();
    }
}

void ConfigMode::draw_ir() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "IR", WHITE, BLACK, 2});

    const size_t count = ir_item_count();

    char ena[24], intl[24], extl[24];
    std::snprintf(ena, sizeof(ena), "Enable: %s",
                  DAL::driver_enabled("ir-pixmob") ? "ON" : "OFF");

    const char* lines[4];
    size_t k = 0;
    lines[k++] = ena;
    if (hal::HAL::ir_tx_ext() != nullptr) {
        auto* d = dal::pixmob_ir_driver_instance();
        std::snprintf(intl, sizeof(intl), "Internal: %s",
                      d->internal_enabled() ? "ON" : "OFF");
        std::snprintf(extl, sizeof(extl), "External: %s",
                      d->external_enabled() ? "ON" : "OFF");
        lines[k++] = intl;
        lines[k++] = extl;
    }
    lines[k++] = "Protocol: PixMob";

    for (size_t i = 0; i < count; ++i) {
        const bool sel = (i == sub_selected_);
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      sel ? ">" : " ", lines[i]);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 30 + (int)i * 16, buf,
            sel ? YELLOW : WHITE, BLACK, 2});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "B: cycle  A: select  B-hold: back",
        WHITE, BLACK, 1});
}

// -------------------------------------------------------------------------
// ESP-NOW submenu (functional: Director Channel + Lume Channel + Repeat)
//
// Per architecture spec §4.5 the project's two-channel social contract
// is channel 1 = hobby/community/open, channel 11 = show/commercial.
// Director picks 1, 11, or 6 (advanced override). Lume picks Auto (dual-
// channel scan with show priority) or locks to a specific channel.
// Both persist to NVS and survive reboot. The receive-filter slv_group
// setting (Epic 4.65 Block 5) moved to the top-level "Group" item in
// the post-Epic-4.65 menu restructure.
// -------------------------------------------------------------------------

const char* ConfigMode::director_channel_label(uint8_t c) {
    switch (c) {
        case 1:  return "1 hobby";
        case 6:  return "6 custom";
        case 11: return "11 show";
        default: return "1 hobby";
    }
}

const char* ConfigMode::lume_channel_label(uint8_t c) {
    switch (c) {
        case 0:  return "auto scan";
        case 1:  return "1 hobby";
        case 6:  return "6 custom";
        case 11: return "11 show";
        default: return "auto scan";
    }
}

uint8_t ConfigMode::cycle_director_channel(uint8_t c) {
    // 1 -> 6 -> 11 -> 1
    switch (c) {
        case 1:  return 6;
        case 6:  return 11;
        case 11: return 1;
        default: return 1;
    }
}

uint8_t ConfigMode::cycle_lume_channel(uint8_t c) {
    // 0 (auto) -> 1 -> 6 -> 11 -> 0
    switch (c) {
        case 0:  return 1;
        case 1:  return 6;
        case 6:  return 11;
        case 11: return 0;
        default: return 0;
    }
}

void ConfigMode::handle_espnow(const ButtonPressEvent& ev) {
    // While the DirectorId hex-edit screen is open, route everything
    // through its dedicated handler. The drill-down is entered when
    // the operator A-clicks the DirID row below; B-hold inside the
    // editor exits back to this menu (handled in handle_root_b_hold).
    if (dir_id_edit_active_) {
        handle_dir_id_edit(ev);
        return;
    }
    // Same pattern for the Wi-Fi scan drill-down.
    if (scan_phase_ != ScanPhase::Idle) {
        handle_scan_channels(ev);
        return;
    }

    if (ev.id == ButtonId::Btn2) {
        sub_selected_ = (sub_selected_ + 1) % kEspNowFunctionalItemCount;
        draw();
        return;
    }
    if (ev.id == ButtonId::Btn1) {
        switch ((EspNowItem)sub_selected_) {
            case EspNowItem::MasterChannel:
                persistence::save_director_channel(
                    cycle_director_channel(persistence::load_director_channel()));
                break;
            case EspNowItem::DirectorId: {
                // Drill into the hex-edit screen. Three cursor
                // positions (high nibble / low nibble / re-roll) -
                // see handle_dir_id_edit. The value the operator
                // sees here was the persisted value at draw time.
                dir_id_edit_active_ = true;
                dir_id_edit_cursor_ = DirIdCursor::HiNibble;
                break;
            }
            case EspNowItem::SlaveChannel:
                persistence::save_lume_channel(
                    cycle_lume_channel(persistence::load_lume_channel()));
                break;
            case EspNowItem::SlaveRepeat:
                persistence::save_lume_repeat_enabled(
                    !persistence::load_lume_repeat_enabled());
                break;
            case EspNowItem::ScanChannels:
                // Kick off the scan workflow. The Requested->Running
                // transition happens in loop_tick so the "Scanning..."
                // splash paints one frame before WiFi.scanNetworks
                // blocks the main task for ~5 s.
                scan_phase_ = ScanPhase::Requested;
                wifi_scanner_.reset();
                break;
        }
        // New value applies on next Director / LumeMode enter().
        // Operator returns to Menu and re-enters the mode to pick it up.
        draw();
    }
}

// -------------------------------------------------------------------------
// Director ID hex editor (drill-down from EspNow > DirectorId)
// -------------------------------------------------------------------------

uint8_t ConfigMode::cycle_dir_id_high_nibble(uint8_t current) {
    // Performance range is 0x40..0xFE, so the high nibble cycles
    // through {4, 5, ..., F}. Increment-only with wrap; the low
    // nibble carries; if the result would be 0xFF (reserved for
    // broadcast) we snap to 0xFE.
    uint8_t hi = (current >> 4) & 0x0F;
    hi = (hi >= 0x0F) ? 0x04 : static_cast<uint8_t>(hi + 1);
    if (hi < 0x04) hi = 0x04;
    const uint8_t lo = current & 0x0F;
    uint8_t v = static_cast<uint8_t>((hi << 4) | lo);
    if (v == 0xFF) v = 0xFE;
    return v;
}

uint8_t ConfigMode::cycle_dir_id_low_nibble(uint8_t current) {
    // Low nibble cycles {0..F}. If the high nibble is F and the low
    // would wrap to F (giving 0xFF), snap to 0 - the operator
    // chooses 0xFE by stepping through {F0..FE}, and the wrap from
    // FE goes to F0 (matching the "skipped" 0xFF).
    uint8_t lo = current & 0x0F;
    const uint8_t hi = (current >> 4) & 0x0F;
    lo = (lo >= 0x0F) ? 0x00 : static_cast<uint8_t>(lo + 1);
    uint8_t v = static_cast<uint8_t>((hi << 4) | lo);
    if (v == 0xFF) v = 0xF0;   // skip broadcast slot
    return v;
}

void ConfigMode::handle_dir_id_edit(const ButtonPressEvent& ev) {
    if (ev.id == ButtonId::Btn2) {
        // Cycle cursor: hi -> lo -> re-roll -> hi.
        const uint8_t next = (static_cast<uint8_t>(dir_id_edit_cursor_) + 1)
                             % kDirIdCursorCount;
        dir_id_edit_cursor_ = static_cast<DirIdCursor>(next);
        draw();
        return;
    }
    if (ev.id == ButtonId::Btn1) {
        switch (dir_id_edit_cursor_) {
            case DirIdCursor::HiNibble: {
                const uint8_t v = cycle_dir_id_high_nibble(
                    persistence::load_director_perf_source_id());
                persistence::save_director_perf_source_id(v);
                break;
            }
            case DirIdCursor::LoNibble: {
                const uint8_t v = cycle_dir_id_low_nibble(
                    persistence::load_director_perf_source_id());
                persistence::save_director_perf_source_id(v);
                break;
            }
            case DirIdCursor::Reroll:
                (void)persistence::reroll_director_perf_source_id();
                break;
        }
        draw();
    }
}

void ConfigMode::draw_espnow() {
    if (dir_id_edit_active_) {
        draw_dir_id_edit();
        return;
    }
    if (scan_phase_ != ScanPhase::Idle) {
        draw_scan_channels();
        return;
    }
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "ESP-NOW", WHITE, BLACK, 2});

    char m_line[28];
    char id_line[28];
    char s_line[28];
    char r_line[28];
    const char* sc_line = "Scan channels";
    std::snprintf(m_line, sizeof(m_line), "Director: %s",
                  director_channel_label(persistence::load_director_channel()));
    // DirectorID is the persisted Performance-range id (channel 11).
    // Selecting this row + A-click re-rolls it (handle_espnow above).
    // Label format mirrors the Lume-side TOFU lock label "P:nn" so
    // the value reads the same on both sides of the wire.
    std::snprintf(id_line, sizeof(id_line), "DirID:    P:%02X",
                  static_cast<unsigned>(persistence::load_director_perf_source_id()));
    std::snprintf(s_line, sizeof(s_line), "Lume:     %s",
                  lume_channel_label(persistence::load_lume_channel()));
    std::snprintf(r_line, sizeof(r_line), "Repeat:   %s",
                  persistence::load_lume_repeat_enabled() ? "ON" : "OFF");
    const char* lines[kEspNowFunctionalItemCount] = {
        m_line, id_line, s_line, r_line, sc_line };

    constexpr int  kRowY0      = 30;
    const size_t   max_visible = static_cast<size_t>(
        (kBodyBottomLimit - kRowY0) / kRowStride);
    const size_t   first       = scroll_offset(
        sub_selected_, kEspNowFunctionalItemCount, max_visible);
    const size_t   last_excl   = (first + max_visible > kEspNowFunctionalItemCount)
                                 ? kEspNowFunctionalItemCount
                                 : first + max_visible;

    for (size_t i = first; i < last_excl; ++i) {
        const bool sel = (i == sub_selected_);
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      sel ? ">" : " ", lines[i]);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, kRowY0 + static_cast<int>(i - first) * kRowStride, buf,
            sel ? YELLOW : WHITE, BLACK, 2});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "B: cycle  A: select  B-hold: back",
        WHITE, BLACK, 1});
}

void ConfigMode::draw_dir_id_edit() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "Set DirID", WHITE, BLACK, 2});

    const uint8_t v  = persistence::load_director_perf_source_id();
    const uint8_t hi = (v >> 4) & 0x0F;
    const uint8_t lo = v & 0x0F;
    static constexpr const char* kHex = "0123456789ABCDEF";

    // Cursor-aware hex display - bracket the active nibble so the
    // operator can see which one Btn1 will increment. "P:" prefix
    // matches the Lume-side TofuLock label convention so the value
    // reads identically on Director Config and Lume screens.
    //
    // Format = "P:" + hi_open + hi_char + hi_close + lo_open + lo_char + lo_close
    //
    // Examples (DirID = 0x4F):
    //   cursor on hi:    "P:[4]F"
    //   cursor on lo:    "P: 4[F]"
    //   cursor on roll:  "P: 4 F"  (neither nibble bracketed)
    const bool on_hi = (dir_id_edit_cursor_ == DirIdCursor::HiNibble);
    const bool on_lo = (dir_id_edit_cursor_ == DirIdCursor::LoNibble);
    const char* hi_open  = on_hi ? "[" : " ";
    const char* hi_close = on_hi ? "]" : " ";
    const char* lo_open  = on_lo ? "[" : "";
    const char* lo_close = on_lo ? "]" : "";
    char hex_line[20];
    // %s + %c + %s + %s + %c + %s - all-matching-types this time.
    // The previous version had %c consuming a const char* and %s
    // consuming a char (which dereferenced an arbitrary integer as
    // a pointer) - reliable LCD-not-rendering + reboot-on-button bug.
    std::snprintf(hex_line, sizeof(hex_line), "P:%s%c%s%s%c%s",
                  hi_open, kHex[hi], hi_close,
                  lo_open, kHex[lo], lo_close);
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 36, hex_line, on_hi || on_lo ? YELLOW : WHITE, BLACK, 3});

    // Re-roll cursor position - a one-liner action labelled like a
    // menu item. Highlighted when the cursor is on it.
    const bool on_roll = (dir_id_edit_cursor_ == DirIdCursor::Reroll);
    char roll_line[24];
    std::snprintf(roll_line, sizeof(roll_line), "%s Re-roll",
                  on_roll ? ">" : " ");
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 86, roll_line, on_roll ? YELLOW : WHITE, BLACK, 2});

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "A: change  B: next  B-hold: back",
        WHITE, BLACK, 1});
}

// -------------------------------------------------------------------------
// Wi-Fi channel scanner drill-down (drives dal::WifiScanner).
//
// UX contract:
//   Requested -> "Scanning..." splash paints this frame; next tick calls
//                the blocking WiFi.scanNetworks (~5 s freeze).
//   Running   -> should be visible for at most one frame in practice;
//                acts as the scan-in-progress phase for run_scan_if_needed.
//   Done      -> 13 vertical bars (per-channel AP count), current ESP-NOW
//                channel highlighted yellow, recommended channel green.
//   Failed    -> error text with a "A: retry" hint.
// -------------------------------------------------------------------------

void ConfigMode::handle_scan_channels(const dal::ButtonPressEvent& ev) {
    if (ev.id == ButtonId::Btn1) {
        // Rescan only when the scan is fully idle at Done/Failed;
        // ignore Btn1 while a scan is mid-flight so the operator
        // can't queue a re-entry into WiFi.scanNetworks.
        if (scan_phase_ == ScanPhase::Done
         || scan_phase_ == ScanPhase::Failed) {
            scan_phase_ = ScanPhase::Requested;
            wifi_scanner_.reset();
            draw();
        }
    }
    // Btn2 has no meaning inside the workflow. B-hold is handled by
    // on_button_event as the workflow-exit path.
}

void ConfigMode::run_scan_if_needed() {
    // Two-tick lifecycle to guarantee "Scanning..." paints one frame
    // before the blocking scan call kicks off.
    switch (scan_phase_) {
        case ScanPhase::Requested:
            scan_phase_ = ScanPhase::Running;
            return;
        case ScanPhase::Running: {
            const bool ok = wifi_scanner_.scan(300);
            scan_phase_ = ok ? ScanPhase::Done : ScanPhase::Failed;
            draw();
            return;
        }
        default:
            return;
    }
}

void ConfigMode::draw_scan_channels() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "Wi-Fi Scan", WHITE, BLACK, 2});

    if (scan_phase_ == ScanPhase::Requested
     || scan_phase_ == ScanPhase::Running) {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 40, "Scanning...", YELLOW, BLACK, 2});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 68, "Radio offline ~5 s.", WHITE, BLACK, 1});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 82, "Buttons unresponsive during", WHITE, BLACK, 1});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 94, "the scan.", WHITE, BLACK, 1});
        return;
    }

    if (scan_phase_ == ScanPhase::Failed) {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 40, "Scan failed", YELLOW, BLACK, 2});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 68, "WiFi driver rejected the", WHITE, BLACK, 1});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 80, "scan request.", WHITE, BLACK, 1});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 122, "A: retry   B-hold: back", WHITE, BLACK, 1});
        return;
    }

    // Done: draw the 13-channel bar chart.
    const uint8_t now_ch = persistence::load_director_channel();
    const uint8_t rec_ch = wifi_scanner_.recommend_channel();

    char status[40];
    std::snprintf(status, sizeof(status), "Now: ch %-3u REC: ch %u",
                  static_cast<unsigned>(now_ch),
                  static_cast<unsigned>(rec_ch));
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 28, status, WHITE, BLACK, 1});

    // Bar-chart geometry - tuned for the Plus2 landscape 240x135 LCD;
    // fits S3 128x128 with a bit of right-margin room to spare. Layout
    // top-to-bottom: title -> status -> AP-count row -> bars ->
    // channel-number row -> footer.
    constexpr int kBarBaseY   = 100;   // bottom of every bar
    constexpr int kBarMaxH    =  48;   // tallest bar (reduced from 56 to
                                       // clear the AP-count row above)
    constexpr int kBarWidth   =  14;
    constexpr int kBarGap     =   2;
    constexpr int kBarXStart  =  16;
    constexpr int kCountRowY  =  40;   // AP-count numerals above bars
    constexpr int kLabelRowY  = kBarBaseY + 4;   // channel numbers below

    // Find the maximum AP count across all channels so heights can
    // scale to fill kBarMaxH. Zero-count channels draw a 1 px baseline
    // so the bar area doesn't look empty when a channel is unseen.
    uint8_t max_count = 0;
    for (uint8_t ch = 1; ch <= 13; ++ch) {
        const uint8_t c = wifi_scanner_.channel(ch).ap_count;
        if (c > max_count) max_count = c;
    }

    for (uint8_t ch = 1; ch <= 13; ++ch) {
        const int x = kBarXStart + (ch - 1) * (kBarWidth + kBarGap);
        const uint8_t count = wifi_scanner_.channel(ch).ap_count;

        int h;
        if (count == 0 || max_count == 0) {
            h = 1;
        } else {
            h = (static_cast<int>(count) * kBarMaxH) / max_count;
            if (h < 2) h = 2;
        }
        const int y = kBarBaseY - h;

        uint16_t colour;
        if (ch == rec_ch)      colour = GREEN;
        else if (ch == now_ch) colour = YELLOW;
        else                   colour = WHITE;

        DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
            x, y, kBarWidth, h, colour});

        // AP-count numeral above the bar, colour-matched so the
        // highlighted channels' counts read at a glance. Size-1 chars
        // are ~6 px wide; centre the label over the 14 px bar.
        char cbuf[4];
        std::snprintf(cbuf, sizeof(cbuf), "%u", static_cast<unsigned>(count));
        const int cw = static_cast<int>(std::strlen(cbuf)) * 6;
        const int cx = x + (kBarWidth - cw) / 2;
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            cx, kCountRowY, cbuf, colour, BLACK, 1});

        // Channel number below the bar, same colour so a highlighted
        // bar's channel ID is unambiguous.
        char lbuf[4];
        std::snprintf(lbuf, sizeof(lbuf), "%u", static_cast<unsigned>(ch));
        const int lw = static_cast<int>(std::strlen(lbuf)) * 6;
        const int lx = x + (kBarWidth - lw) / 2;
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            lx, kLabelRowY, lbuf, colour, BLACK, 1});
    }

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "A: rescan  B-hold: back", WHITE, BLACK, 1});
}

// -------------------------------------------------------------------------
// PixMob submenu (PixMob-protocol-specific commands moved out of Test):
//   Set Group ID    bracelet-setup helper, fires SetGroupId+SetGroupSel
//   Group Target    Btn1 advances + fires to groups 1..5 in turn
//
// Two-level navigation within the submenu: menu lists the items, Btn1
// drills into a workflow screen. B-hold pops one level (workflow ->
// menu -> Utilities picker).
// -------------------------------------------------------------------------

void ConfigMode::handle_pixmob(const ButtonPressEvent& ev) {
    switch (pixmob_state_) {
        case PixMobState::Menu:
            if (ev.id == ButtonId::Btn2) {
                pixmob_selected_ = (pixmob_selected_ + 1) % kPixMobItemCount;
                draw();
            } else if (ev.id == ButtonId::Btn1) {
                pixmob_state_ = (pixmob_selected_ == 0)
                                    ? PixMobState::SetGroupId
                                    : PixMobState::GroupTarget;
                confirm_until_ms_ = 0;
                draw();
            }
            break;
        case PixMobState::SetGroupId:
            if (ev.id == ButtonId::Btn2) {
                pixmob_target_group_ = (pixmob_target_group_ % 5) + 1;
                draw();
            } else if (ev.id == ButtonId::Btn1) {
                // Target name is irrelevant - the AssignDeviceGroup
                // dispatch ignores device group_id and uses the event's
                // new_group_id payload.
                const bool ok = DAL::fire_assign_device_group(
                    "all-pixmobs",
                    AssignDeviceGroupEvent{pixmob_target_group_});
                if (ok) confirm_until_ms_ = millis() + kConfirmFlashMs;
                draw();
            }
            break;
        case PixMobState::GroupTarget:
            if (ev.id == ButtonId::Btn1) {
                char target[16];
                std::snprintf(target, sizeof(target),
                              "group-%u", (unsigned)pixmob_target_group_);
                DAL::fire_rgb_pulse(target, RgbPulseEvent{
                    0xFF, 0xFF, 0xFF,
                    pixmob::T_32_MS, pixmob::T_96_MS, pixmob::T_96_MS,
                    pixmob::CHANCE_100});
                pixmob_target_group_ = (pixmob_target_group_ % 5) + 1;
                draw();
            }
            break;
    }
}

void ConfigMode::draw_pixmob() {
    switch (pixmob_state_) {
        case PixMobState::Menu:        draw_pixmob_menu();      break;
        case PixMobState::SetGroupId:  draw_pixmob_set_group(); break;
        case PixMobState::GroupTarget: draw_pixmob_group_tgt(); break;
    }
}

void ConfigMode::draw_pixmob_menu() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "PixMob", WHITE, BLACK, 2});

    const char* items[kPixMobItemCount] = { "Set Group ID", "Group Test" };
    for (size_t i = 0; i < kPixMobItemCount; ++i) {
        const bool sel = (i == pixmob_selected_);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      sel ? ">" : " ", items[i]);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 30 + (int)i * 16, buf,
            sel ? YELLOW : WHITE, BLACK, 2});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "B: cycle  A: select  B-hold: back",
        WHITE, BLACK, 1});
}

void ConfigMode::draw_pixmob_set_group() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "Set Group ID", WHITE, BLACK, 2});
    char buf[24];
    std::snprintf(buf, sizeof(buf), "New group: %u",
                  (unsigned)pixmob_target_group_);
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 40, buf, WHITE, BLACK, 2});
    if (confirm_until_ms_ != 0) {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 70, "Sent!", GREEN, BLACK, 2});
    } else {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 70, "isolate target!", YELLOW, BLACK, 1});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 128, "B: cycle  A: send  B-hold: back",
        WHITE, BLACK, 1});
}

void ConfigMode::draw_pixmob_group_tgt() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "Group Test", WHITE, BLACK, 2});
    char buf[24];
    std::snprintf(buf, sizeof(buf), "Next: group %u",
                  (unsigned)pixmob_target_group_);
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 50, buf, WHITE, BLACK, 2});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 128, "A: fire + advance  B-hold: back",
        WHITE, BLACK, 1});
}

// -------------------------------------------------------------------------
// System submenu (functional)
// -------------------------------------------------------------------------

void ConfigMode::handle_system(const ButtonPressEvent& ev) {
    if (ev.id == ButtonId::Btn2) {
        sub_selected_ = (sub_selected_ + 1) % kSystemItemCount;
        draw();
        return;
    }
    if (ev.id == ButtonId::Btn1) {
        switch ((SystemItem)sub_selected_) {
            case SystemItem::FactoryReset:
                factory_reset();
                confirm_until_ms_ = millis() + 800;   // "done" linger
                draw();
                break;
            default:
                // Info-only items - no-op on Btn1.
                break;
        }
    }
}

void ConfigMode::factory_reset() {
#ifdef ARDUINO
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.clear();
    prefs.end();
#endif
    // s_last_runtime stays as in-memory state until reboot.
}

void ConfigMode::draw_system() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "System", WHITE, BLACK, 2});

    char fw[28]; std::snprintf(fw, sizeof(fw), "Firmware: %s",
                               ::nocturnation::kFirmwareVersion);
    char dm[28]; std::snprintf(dm, sizeof(dm), "Default: %s",
                               mode_label_short(persistence::current_last_runtime()));
    char br[28];
    const int batt = last_drawn_battery_ >= -1 ? last_drawn_battery_
                                                : DAL::battery_level("local");
    if (batt < 0) std::snprintf(br, sizeof(br), "Batt: --");
    else          std::snprintf(br, sizeof(br), "Batt: %d%%", batt);

    const char* lines[kSystemItemCount] = {
        fw,
        dm,
        confirm_until_ms_ != 0 ? "Reset complete" : "Factory reset",
        br,
    };
    for (size_t i = 0; i < kSystemItemCount; ++i) {
        const bool sel = (i == sub_selected_);
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      sel ? ">" : " ", lines[i]);
        const uint16_t fg = (i == (size_t)SystemItem::FactoryReset
                              && confirm_until_ms_ != 0) ? GREEN
                          : sel                          ? YELLOW
                          :                                WHITE;
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 30 + (int)i * 16, buf, fg, BLACK, 2});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "B: cycle  A: select  B-hold: back",
        WHITE, BLACK, 1});
}

const char* ConfigMode::mode_label_short(ModeId m) {
    switch (m) {
        case ModeId::Director: return "Director";
        case ModeId::Lume:            return "Lume";
        case ModeId::Config:           return "Config";
        case ModeId::Test:             return "Test";
        default:                       return "?";
    }
}

void ConfigMode::draw() {
    switch (level_) {
        case Level::Top:    draw_top();    break;
        case Level::Picker: draw_picker(); break;
        case Level::Sub:    draw_sub();    break;
    }
}

// =============================================================================
// LED Strip submenu (Epic 12)
//
// Enable / Group size / Chain size / Brightness are all live. Brightness
// was reinstated 2026-07-11 after the M5Stack TypeC2Grove Unit (U151) Y
// adapter landed - that splitter routes USB-C 5V directly to the
// strip's V pin (Port 3) while data + device power feed the Stick
// (Port 2), bypassing the device's regulator that used to be the
// brownout bottleneck. The cycle exposes {5, 10, 25, 50} % on Plus2 /
// S3; the 25 and 50 tiers are labelled "(Ext Grove Pwr)" because they
// require the Y adapter. Atom Lite has no Config menu (no LCD) and
// stays HAL-capped at 10 %.
// =============================================================================

namespace {
// Group sizes the operator can pick from. 1 / 6 / 12 / 24.
//   1   - every pixel rolls its own CHANCE die (Tildagon ring-style
//         fine-grain sparkle: each LED independent, matches the way
//         a 12-LED perimeter ring responds to a wash_with_sparkle cue)
//   6   - small chunky groups
//   12  - Tildagon-ring-sized blocks
//   24  - large coarse groups (whole-strip-feel at chain >= 144)
// PixMob bracelets are effectively group_size = pixel_count (the
// whole bracelet responds as one), but we don't offer that as a
// preset - chain-size = group-size has the same effect.
constexpr uint8_t kStripGroupSizes[] = { 1, 6, 12, 24 };
constexpr size_t  kStripGroupSizeCount =
    sizeof(kStripGroupSizes) / sizeof(kStripGroupSizes[0]);

uint8_t cycle_strip_group_size(uint8_t current) {
    for (size_t i = 0; i < kStripGroupSizeCount; ++i) {
        if (kStripGroupSizes[i] == current) {
            return kStripGroupSizes[(i + 1) % kStripGroupSizeCount];
        }
    }
    return kStripGroupSizes[0];
}

// Chain sizes match the M5Stack shop SKUs. Showing the physical
// length is the operator-friendly label (counting LEDs on a 1 m
// strip is a poor UX); the pixel count tags along in brackets.
struct ChainSize {
    uint16_t    pixels;
    const char* label;     // e.g. "1 m (144)"
};
constexpr ChainSize kStripChainSizes[] = {
    { 15,  "10cm (15)"  },
    { 29,  "20cm (29)"  },
    { 72,  "50cm (72)"  },
    { 144, "1m (144)"   },
    { 288, "2m (288)"   },
};
constexpr size_t kStripChainSizeCount =
    sizeof(kStripChainSizes) / sizeof(kStripChainSizes[0]);

uint16_t cycle_strip_chain_size(uint16_t current) {
    for (size_t i = 0; i < kStripChainSizeCount; ++i) {
        if (kStripChainSizes[i].pixels == current) {
            return kStripChainSizes[(i + 1) % kStripChainSizeCount].pixels;
        }
    }
    return kStripChainSizes[0].pixels;
}

const char* strip_chain_size_label(uint16_t pixels) {
    for (size_t i = 0; i < kStripChainSizeCount; ++i) {
        if (kStripChainSizes[i].pixels == pixels) {
            return kStripChainSizes[i].label;
        }
    }
    return "?";
}

// Brightness cycle: 5 -> 10 -> 25 -> 50 -> wrap. 5 % and 10 % are
// safe on internal battery / device-USB power; 25 % and 50 % require
// the M5Stack TypeC2Grove Unit (U151) Y adapter so the strip draws
// current directly off USB-C, not through the device's regulator.
constexpr uint8_t kStripBrightnessLevels[] = { 5, 10, 25, 50 };
constexpr size_t  kStripBrightnessLevelCount =
    sizeof(kStripBrightnessLevels) / sizeof(kStripBrightnessLevels[0]);

// Whether a given brightness value requires external Grove Y USB power.
// True for the 25 and 50 % tiers - anything above the safe internal-
// battery envelope of ~10 %.
bool strip_brightness_needs_ext_power(uint8_t pct) {
    return pct > 10;
}

uint8_t cycle_strip_brightness(uint8_t current) {
    for (size_t i = 0; i < kStripBrightnessLevelCount; ++i) {
        if (kStripBrightnessLevels[i] == current) {
            return kStripBrightnessLevels[(i + 1) % kStripBrightnessLevelCount];
        }
    }
    // Current value isn't a canonical menu step (e.g. NVS carried
    // over an odd number). Snap to the lowest safe step so the very
    // next A-click enters a known cycle position.
    return kStripBrightnessLevels[0];
}
}  // namespace

void ConfigMode::handle_led_strip(const ButtonPressEvent& ev) {
    if (ev.id == ButtonId::Btn2) {
        sub_selected_ = (sub_selected_ + 1) % kLedStripItemCount;
        draw();
        return;
    }
    if (ev.id == ButtonId::Btn1) {
        switch (static_cast<LedStripItem>(sub_selected_)) {
            case LedStripItem::Enable: {
                const bool next = !persistence::load_strip_enabled();
                persistence::save_strip_enabled(next);
                DAL::set_driver_enabled("led-strip", next);
                break;
            }
            case LedStripItem::GroupSize: {
                const uint8_t current = persistence::load_strip_group_size();
                const uint8_t next    = cycle_strip_group_size(current);
                persistence::save_strip_group_size(next);
                dal::led_strip_driver_instance()->set_group_size(next);
                break;
            }
            case LedStripItem::ChainSize: {
                const uint16_t current = persistence::load_strip_chain_size();
                const uint16_t next    = cycle_strip_chain_size(current);
                persistence::save_strip_chain_size(next);
                dal::led_strip_driver_instance()->set_pixel_count(next);
                break;
            }
            case LedStripItem::Brightness: {
                const uint8_t current = persistence::load_strip_brightness();
                const uint8_t next    = cycle_strip_brightness(current);
                persistence::save_strip_brightness(next);
                // Push the new value through set_brightness_percent so
                // the HAL cap + kAbsoluteMaxBrightness clamps apply.
                // On Atom Lite this menu is unreachable (no LCD), but
                // the belt-and-braces cap in the driver + HAL keeps
                // Atom safe if someone reaches here via a bench route.
                dal::led_strip_driver_instance()->set_brightness_percent(next);
                break;
            }
        }
        draw();
    }
}

void ConfigMode::draw_led_strip() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "LED Strip", WHITE, BLACK, 2});

    // Row labels. "Bright: NN %(Ext)" is deliberately compact - the
    // 20-column limit at size 2 (240 px / 12 px per char) rules out
    // the full "(Ext Grove Pwr)" phrasing on the row itself. The
    // sub-line below the item list expands the abbreviation.
    char ena[28], grp[28], chn[28], bri[28];
    std::snprintf(ena, sizeof(ena), "Enable: %s",
                  persistence::load_strip_enabled() ? "ON" : "OFF");
    std::snprintf(grp, sizeof(grp), "Group size: %u",
                  (unsigned)persistence::load_strip_group_size());
    std::snprintf(chn, sizeof(chn), "Chain: %s",
                  strip_chain_size_label(persistence::load_strip_chain_size()));
    const uint8_t bri_pct = persistence::load_strip_brightness();
    if (strip_brightness_needs_ext_power(bri_pct)) {
        std::snprintf(bri, sizeof(bri), "Bright: %u%%(Ext)",
                      (unsigned)bri_pct);
    } else {
        std::snprintf(bri, sizeof(bri), "Bright: %u %%",
                      (unsigned)bri_pct);
    }

    const char* lines[kLedStripItemCount] = { ena, grp, chn, bri };
    for (size_t i = 0; i < kLedStripItemCount; ++i) {
        const bool sel = (i == sub_selected_);
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      sel ? ">" : " ", lines[i]);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 30 + (int)i * 16, buf,
            sel ? YELLOW : WHITE, BLACK, 2});
    }

    // Expansion line for the "(Ext)" tag on the Brightness row. Placed
    // below the last menu item at size 1 so it doesn't compete with
    // the highlighted row's colour.
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 30 + (int)kLedStripItemCount * 16,
        "(Ext) = Ext Grove Pwr", WHITE, BLACK, 1});

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "B: cycle  A: select  B-hold: back",
        WHITE, BLACK, 1});
}

}  // namespace modes
}  // namespace nocturnation
