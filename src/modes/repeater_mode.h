// RepeaterMode - headless ESP-NOW repeater (Atom repeater).
//
// A dedicated, screenless firmware role for an M5 AtomS3 (the same board
// the AtomS3-PoE Director uses): its whole job is to re-broadcast the
// fleet's ESP-NOW traffic one hop further into the crowd, extending
// coverage where body-absorption shadows the far half of the audience.
//
// Behaviour:
//   - ESP-NOW Long Range RX on the fleet channel (fixed 1/6/11, or auto:
//     probe 1/6/11 and lock to whichever carries Director traffic).
//   - Every unique frame is re-broadcast verbatim (dedup ring on
//     (source_id, sequence)). Hop-transparent: hop_count is NOT
//     incremented, so Atom coverage never spends the fleet's 3-hop
//     budget - Lume dynamic routing keeps full audience depth. Seq-0
//     frames bypass dedup and are therefore never relayed (loop guard).
//   - Emits a REPEATER_HEARTBEAT census ~1 Hz so the Director can count
//     how many repeaters are online. Census frames are never relayed.
//   - The onboard RGB LED (GPIO 35) is the only output: channel colour
//     when idle, a cyan flash on each relay, red if the radio is down,
//     and a solid confirm colour after a channel change.
//   - The board's button (M5.BtnA) double-click cycles the channel
//     auto -> 1 -> 6 -> 11 -> auto, persisted to NVS and LED-confirmed.
//
// Reuses the AtomS3-PoE HAL (ESP-NOW + M5 button/LED) minus Ethernet.
// Entered directly at boot under -DNOCT_HEADLESS_REPEATER, mirroring how
// -DNOCT_HEADLESS_DMX_BRIDGE boots straight into DmxBridge.

#pragma once

#include "modes/mode_machine.h"
#include "hal/hal.h"

#include <cstddef>
#include <cstdint>

namespace nocturnation {
namespace modes {

class RepeaterMode : public Mode {
public:
    RepeaterMode() = default;

    ModeId       id()   const override { return ModeId::Repeater; }
    const char*  name() const override { return "Repeater"; }

    void enter() override;
    void exit()  override;
    void loop_tick() override;

    // Census broadcast cadence.
    static constexpr uint32_t kCensusIntervalMs = 1000;
    // How long the LED holds its bright relay flash after a relay.
    static constexpr uint32_t kRelayFlashMs = 60;
    // Auto-scan: dwell per channel while probing for Director traffic.
    static constexpr uint32_t kScanDwellMs = 700;
    // Auto-scan: silence on the locked channel before we re-scan.
    static constexpr uint32_t kScanRelockMs = 8000;
    // Two button clicks within this window count as a double-click.
    static constexpr uint32_t kDoubleClickMs = 400;
    // How long the channel-change confirm colour holds on the LED.
    static constexpr uint32_t kConfirmMs = 1000;

    static constexpr size_t  kDedupRingSize = 16;
    static constexpr size_t  kRepeatBufSize = 250;   // == kMaxFrameSize

private:
    // RX callback (runs on the WiFi task). Kept minimal: validate, dedup,
    // stage one relay for loop_tick. Matches the Lume-repeat WiFi-task
    // safety rule - never TX or block from the receive callback.
    void on_recv(const hal::ESPNowMessage& m);

    // Dedup ring on (source_id, sequence_number), FIFO, seq 0 bypasses.
    // v3: source_id widened to u16 to match the wire.
    struct DedupEntry { uint16_t source_id; uint8_t sequence_number; };
    DedupEntry dedup_ring_[kDedupRingSize] = {};
    size_t     dedup_head_ = 0;
    bool seen_recently(uint16_t src, uint8_t seq) const;
    void mark_seen(uint16_t src, uint8_t seq);

    // Staged relay handed WiFi-task -> main-task, single-buffer (newest
    // wins under burst, same as Lume repeat).
    volatile bool pending_repeat_     = false;
    size_t        pending_repeat_len_ = 0;
    uint8_t       pending_repeat_buf_[kRepeatBufSize] = {};

    // Radio / channel state.
    uint8_t channel_setting_ = 0;   // 0 = auto, else 1/6/11 (persisted)
    uint8_t active_channel_  = 1;   // channel the radio is currently on
    bool    radio_up_        = false;
    bool    locked_          = false;   // auto-scan: found traffic?

    // Auto-scan cursor.
    size_t   scan_idx_             = 0;
    uint32_t scan_probe_started_ms_ = 0;

    // Timing / telemetry.
    uint32_t boot_ms_        = 0;
    volatile uint32_t last_rx_ms_ = 0;   // written from the WiFi task
    uint32_t last_relay_ms_  = 0;
    uint32_t last_census_ms_ = 0;
    uint32_t relayed_count_  = 0;
    uint8_t  uid_[3]         = {0, 0, 0};

    // Button double-click detection.
    uint32_t last_click_ms_ = 0;

    // LED channel-change confirmation.
    uint32_t confirm_until_ms_ = 0;

    void start_radio(uint8_t channel);
    void apply_channel_setting(uint8_t setting);   // cycle: persist + apply
    void poll_button(uint32_t now);
    void service_scan(uint32_t now);
    void emit_census(uint32_t now);
    void update_led(uint32_t now);
};

}  // namespace modes
}  // namespace nocturnation
