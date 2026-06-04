// DmxBridgeMode - Epic 7 B3.
//
// The Stick acts as an Enttec DMX USB Pro receiver: a laptop running
// QLC+ writes DMX frames down the USB-C cable, the mode parses the
// Enttec Pro framing (B1), interprets the 12-channel-per-Lume-group
// fixture layout (B2), and broadcasts the resulting LIGHT_PULSE /
// LIGHT_WASH events over ESP-NOW to the fleet.
//
// Wholly mutually exclusive with Director / Show mode: only one
// surface drives the wire at a time. Mode entry switches Serial off
// the 115 200 console rate to 921 600 (the Enttec Pro standard); mode
// exit restores the console + emits LIGHT_WASH_END to clear the wash
// the LD left on the fleet.
//
// LCD chrome (heritage Notion Epic 7 AC): "DMX Active" / "DMX Idle"
// header, last-frame age, total bytes read, frame counter. The LCD is
// pure UI here - no lighting visualisation on the Stick's own screen
// while in DMX mode (the wire output is the visual; the Stick is the
// fixture).

#pragma once

#include "modes/mode_machine.h"
#include "dal/drivers/dmx_channel_mapper.h"

namespace nocturnation {
namespace modes {

class DmxBridgeMode : public Mode {
public:
    ModeId       id()   const override { return ModeId::DmxBridge; }
    const char*  name() const override { return "DMX Bridge"; }

    void enter() override;
    void exit()  override;
    void loop_tick() override;
    void on_button_event(const dal::ButtonPressEvent& ev) override;

    // Target string the mapper fires events on. Broadcast everywhere
    // (class 0, group 0) for v1; per-group address management lands
    // in a follow-up commit alongside a Config-mode submenu.
    static constexpr const char* kBroadcastTarget = "00:00";

    // LCD redraw cadence; matches other modes.
    static constexpr uint32_t kDrawIntervalMs = 100;

    // "DMX Idle" threshold: if no frame has arrived for this many ms
    // the header switches from "DMX Active" to "DMX Idle". 750 ms is
    // ~30 missed QLC+ frames at the default 35-44 Hz rate.
    static constexpr uint32_t kIdleAfterMs = 750;

private:
    // The mapper holds per-group state (anchor history, trigger arm,
    // strobe cadence). v1 owns exactly one mapper at base address 1
    // / target "00:00"; multi-group is a follow-up.
    dal::DmxChannelMapper mapper_;

    // Mode-active state for the LCD + lifecycle. The adapter itself
    // lives in a local-static accessor in the .cpp (Arduino-only;
    // native build skips it) to avoid static-init dependencies on
    // the global Serial.
    bool      active_              = false;
    uint32_t  enter_ms_             = 0;
    uint32_t  last_draw_ms_        = 0;
    uint32_t  last_frame_seen_ms_  = 0;
    uint32_t  last_frame_count_    = 0;

    void draw_status();
};

}  // namespace modes
}  // namespace nocturnation
