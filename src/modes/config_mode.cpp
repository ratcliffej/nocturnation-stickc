// ConfigMode implementation.

#include "config_mode.h"

#include "firmware_version.h"
#include "persistence.h"
#include "dal/dal.h"
#include "../dal/drivers/local_driver.h"   // for set_pulse_enabled gating
#include "output_bindings/pixmob_ir.h"
#include "plugins/property_bag.h"
#include "shows/show.h"
#include "shows/show_registry.h"

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
// for the slave route through persistence:: shared helpers (Block 9).
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
constexpr ConfigMode::TopEntry    ConfigMode::kTop[6];
constexpr ConfigMode::PickerEntry ConfigMode::kConnectivity[4];
constexpr ConfigMode::PickerEntry ConfigMode::kUtilities[1];
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
    draw();
}

void ConfigMode::loop_tick() {
    // System submenu has live read-outs (battery percent); refresh
    // every ~500 ms when shown. Confirmation flash for factory reset
    // also clears here.
    const uint32_t now = millis();
    if (confirm_until_ms_ != 0 && now >= confirm_until_ms_) {
        confirm_until_ms_ = 0;
        draw();
        return;
    }
    if (level_ == Level::Sub && active_sub_ == SubMenu::System) {
        const int batt = DAL::battery_level("local");
        if (batt != last_drawn_battery_) {
            last_drawn_battery_ = batt;
            draw();
        }
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
        } else if (level_ == Level::Sub && active_picker_ != SubMenu::None) {
            // Came in via a picker - return to the picker, not Top.
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
            // Increment 0..255 wrapping. A long-press fast-cycle would be
            // a future polish if 256 presses to wrap proves tiresome.
            persistence::save_slv_group(
                static_cast<uint8_t>(persistence::load_slv_group() + 1));
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
                          (unsigned)persistence::load_slv_group());
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
        case SubMenu::Show:    handle_show(ev);    break;
        case SubMenu::System:  handle_system(ev);  break;
        case SubMenu::IR:      handle_ir(ev);      break;
        case SubMenu::Display: handle_display(ev); break;
        case SubMenu::EspNow:  handle_espnow(ev);  break;
        case SubMenu::PixMob:  handle_pixmob(ev);  break;
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
        case SubMenu::Show:    draw_show(); break;
        case SubMenu::Display: draw_display(); break;
        case SubMenu::IR:      draw_ir(); break;
        case SubMenu::EspNow:  draw_espnow(); break;
        case SubMenu::WiFi:    draw_stub("WiFi", kWifiItems, kWifiItemCount, "Epic 4"); break;
        case SubMenu::Dmx:     draw_stub("DMX",  kDmxItems,  kDmxItemCount,  "Epic 7"); break;
        case SubMenu::PixMob:  draw_pixmob(); break;
        case SubMenu::System:  draw_system(); break;
        default: break;
    }
}

size_t ConfigMode::stub_item_count() const {
    switch (active_sub_) {
        case SubMenu::Display: return kDisplayFunctionalItemCount;
        case SubMenu::IR:      return kIrFunctionalItemCount;
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
// AutonomousMasterMode's in-flight picker but reachable from Config
// without entering Master mode. Empty registry is defensive only -
// production builds always register at least SimpleBeatShow.
// -------------------------------------------------------------------------

void ConfigMode::handle_show(const ButtonPressEvent& ev) {
    const size_t count = shows::show_registry().count();
    if (count == 0) return;
    if (ev.id == ButtonId::Btn2) {
        sub_selected_ = (sub_selected_ + 1) % count;
        draw();
        return;
    }
    if (ev.id == ButtonId::Btn1) {
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
    const size_t  first       = scroll_offset(sub_selected_, count, max_visible);
    const size_t  last_excl   = (first + max_visible > count)
                                ? count
                                : first + max_visible;

    for (size_t i = first; i < last_excl; ++i) {
        const bool   sel = (i == sub_selected_);
        shows::Show* s   = shows::show_registry().at(i);
        const bool   active = s && active_id
                              && std::strcmp(s->id(), active_id) == 0;
        char buf[40];
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
// The "Slave Group" item that used to surface PixMobIrBinding's `group`
// property here was dropped post-Epic-4.65: the protocol IR group is
// now relay-driven via inbound LIGHT_COMMAND target_group (Block 5);
// the per-binding group property still exists as broadcast fallback
// (target_group=0) and is reachable via the auto-generated settings
// overlay, but it confused operators on the IR screen.
// -------------------------------------------------------------------------

void ConfigMode::handle_ir(const ButtonPressEvent& ev) {
    if (ev.id == ButtonId::Btn2) {
        sub_selected_ = (sub_selected_ + 1) % kIrFunctionalItemCount;
        draw();
        return;
    }
    if (ev.id == ButtonId::Btn1
     && (IRItem)sub_selected_ == IRItem::EnableDisable) {
        const bool next = !DAL::driver_enabled("ir-pixmob");
        DAL::set_driver_enabled("ir-pixmob", next);
        persistence::save_ir_enabled(next);
        draw();
    }
    // Protocol is info-only - no Btn1 action.
}

void ConfigMode::draw_ir() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "IR", WHITE, BLACK, 2});

    char ena[24];
    std::snprintf(ena, sizeof(ena), "Enable: %s",
                  DAL::driver_enabled("ir-pixmob") ? "ON" : "OFF");
    const char* lines[kIrFunctionalItemCount] = {
        ena,
        "Protocol: PixMob",
    };

    for (size_t i = 0; i < kIrFunctionalItemCount; ++i) {
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
// ESP-NOW submenu (functional: Master Channel + Slave Channel + Repeat)
//
// Per architecture spec §4.5 the project's two-channel social contract
// is channel 1 = hobby/community/open, channel 11 = show/commercial.
// Master picks 1, 11, or 6 (advanced override). Slave picks Auto (dual-
// channel scan with show priority) or locks to a specific channel.
// Both persist to NVS and survive reboot. The receive-filter slv_group
// setting (Epic 4.65 Block 5) moved to the top-level "Group" item in
// the post-Epic-4.65 menu restructure.
// -------------------------------------------------------------------------

const char* ConfigMode::master_channel_label(uint8_t c) {
    switch (c) {
        case 1:  return "1 hobby";
        case 6:  return "6 custom";
        case 11: return "11 show";
        default: return "1 hobby";
    }
}

const char* ConfigMode::slave_channel_label(uint8_t c) {
    switch (c) {
        case 0:  return "auto scan";
        case 1:  return "1 hobby";
        case 6:  return "6 custom";
        case 11: return "11 show";
        default: return "auto scan";
    }
}

uint8_t ConfigMode::cycle_master_channel(uint8_t c) {
    // 1 -> 6 -> 11 -> 1
    switch (c) {
        case 1:  return 6;
        case 6:  return 11;
        case 11: return 1;
        default: return 1;
    }
}

uint8_t ConfigMode::cycle_slave_channel(uint8_t c) {
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
    if (ev.id == ButtonId::Btn2) {
        sub_selected_ = (sub_selected_ + 1) % kEspNowFunctionalItemCount;
        draw();
        return;
    }
    if (ev.id == ButtonId::Btn1) {
        switch ((EspNowItem)sub_selected_) {
            case EspNowItem::MasterChannel:
                persistence::save_master_channel(
                    cycle_master_channel(persistence::load_master_channel()));
                break;
            case EspNowItem::SlaveChannel:
                persistence::save_slave_channel(
                    cycle_slave_channel(persistence::load_slave_channel()));
                break;
            case EspNowItem::SlaveRepeat:
                persistence::save_slave_repeat_enabled(
                    !persistence::load_slave_repeat_enabled());
                break;
        }
        // New value applies on next AutonomousMaster / SlaveMode enter().
        // Operator returns to Menu and re-enters the mode to pick it up.
        draw();
    }
}

void ConfigMode::draw_espnow() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "ESP-NOW", WHITE, BLACK, 2});

    char m_line[28];
    char s_line[28];
    char r_line[28];
    std::snprintf(m_line, sizeof(m_line), "Master: %s",
                  master_channel_label(persistence::load_master_channel()));
    std::snprintf(s_line, sizeof(s_line), "Slave:  %s",
                  slave_channel_label(persistence::load_slave_channel()));
    std::snprintf(r_line, sizeof(r_line), "Repeat: %s",
                  persistence::load_slave_repeat_enabled() ? "ON" : "OFF");
    const char* lines[kEspNowFunctionalItemCount] = { m_line, s_line, r_line };

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
        case ModeId::AutonomousMaster: return "Master";
        case ModeId::Slave:            return "Slave";
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

}  // namespace modes
}  // namespace nocturnation
