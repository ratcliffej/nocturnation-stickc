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
    DmxBridgeMode();

    ModeId       id()   const override { return ModeId::DmxBridge; }
    const char*  name() const override { return "DMX Bridge"; }

    void enter() override;
    void exit()  override;
    void loop_tick() override;
    void on_button_event(const dal::ButtonPressEvent& ev) override;

    // LCD redraw cadence; matches other modes.
    static constexpr uint32_t kDrawIntervalMs = 100;

    // "DMX Idle" threshold: if no frame has arrived for this many ms
    // the header switches from "DMX Active" to "DMX Idle". 750 ms is
    // ~30 missed QLC+ frames at the default 35-44 Hz rate.
    static constexpr uint32_t kIdleAfterMs = 750;

    // B7: per-group block layout. The DMX universe is sliced into
    // seven uniform 40-channel blocks (broadcast + 6 groups). Each
    // block has its own mapper instance with its own change-detection
    // state; only blocks whose channels move emit. 23 active channels
    // per block (the rest reserved for future expansion); 280
    // addresses consumed of the 512-channel universe (232 spare).
    //
    //   Addresses 1-40    -> broadcast (target_group=0)
    //   Addresses 41-80   -> group 1
    //   Addresses 81-120  -> group 2
    //   Addresses 121-160 -> group 3
    //   Addresses 161-200 -> group 4
    //   Addresses 201-240 -> group 5
    //   Addresses 241-280 -> group 6
    static constexpr size_t kBlockCount = 7;

    // Base offset of each block in the parser's 0-indexed channel
    // buffer (universe[0] = DMX channel 1). Block N at offset N*40.
    static constexpr uint16_t kBlockBase[kBlockCount] = {
        0,    // broadcast: addresses 1..40
        40,   // group 1: addresses 41..80
        80,   // group 2
        120,  // group 3
        160,  // group 4
        200,  // group 5
        240,  // group 6 - last DMX channel is 280
    };

    // Per-block channel count. Uniform 40 channels per block - no
    // off-by-one between broadcast and groups, which simplifies both
    // the QLC+ fixture file (single mode) and the operator mental
    // model.
    static constexpr uint16_t kBlockSize[kBlockCount] = {
        40, 40, 40, 40, 40, 40, 40,
    };

private:
    // One mapper per addressable block. Construction order matches
    // kBlockBase / kBlockSize; each mapper is initialised with its
    // target_group at construction.
    dal::DmxChannelMapper mapper_broadcast_;
    dal::DmxChannelMapper mapper_g1_;
    dal::DmxChannelMapper mapper_g2_;
    dal::DmxChannelMapper mapper_g3_;
    dal::DmxChannelMapper mapper_g4_;
    dal::DmxChannelMapper mapper_g5_;
    dal::DmxChannelMapper mapper_g6_;

    // Indexed access for iteration in loop_tick().
    dal::DmxChannelMapper* mapper_at(size_t i);

    // Fan one freshly-parsed DMX frame across all 7 mapper blocks.
    // Reads the parser slice in `universe_buf`, applies per-block
    // change detection (the mapper does the actual work), and emits
    // only for blocks whose channels moved. `copied` is the parser's
    // returned channel count from copy_dmx_channels().
    void run_mappers(const uint8_t* universe_buf,
                     uint16_t copied,
                     uint32_t now);

    // Buffer reused across ticks. 280 bytes covers all 7 blocks
    // (last block ends at channel 280). Static-sized to avoid heap.
    static constexpr uint16_t kUniverseBufferSize = 280;

    // Target the bridge LIGHT_WASH_END's at on mode exit so the wash
    // is cleared on EVERY group, not just broadcast. "00:00" reaches
    // all Lumes regardless of configured group via target_group=0.
    static constexpr const char* kExitClearTarget = "00:00";

    // Mode-active state for the LCD + lifecycle. The adapter itself
    // lives in a local-static accessor in the .cpp (Arduino-only;
    // native build skips it) to avoid static-init dependencies on
    // the global Serial.
    bool      active_              = false;
    uint32_t  enter_ms_             = 0;
    uint32_t  last_draw_ms_        = 0;
    uint32_t  last_frame_seen_ms_  = 0;
    uint32_t  last_frame_count_    = 0;

    // Universe slice cache. Refilled from the parser's last_payload on
    // every tick that sees a fresh frame; passed to all 10 mappers.
    uint8_t   universe_[kUniverseBufferSize] = {0};

    void draw_status();
};

}  // namespace modes
}  // namespace nocturnation
