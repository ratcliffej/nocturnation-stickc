// DmxBridgeMode implementation - Epic 7 B3.

#include "dmx_bridge_mode.h"

#include "dal/dal.h"
#include "dal/drivers/dmx_usb_cdc_adapter.h"
#include "../dal/drivers/espnow_broadcast_driver.h"
#include "persistence.h"

#include <cstdio>

#ifdef ARDUINO
#include <Arduino.h>
#else
extern "C" uint32_t millis();
#endif

namespace nocturnation {
namespace modes {

using namespace nocturnation::dal;
using nocturnation::dal::DmxChannelMapper;

namespace {

// Adapter accessor. Local-static so the global Serial isn't touched
// at static-init time; the Arduino USB-CDC peripheral is brought up
// lazily on the first mode entry. Native build returns nullptr from
// dmx_adapter_or_null() so the .cpp compiles on the host test envs
// (e.g. native_modes which pulls all of src/modes/ in).
#ifdef ARDUINO
DmxUsbCdcAdapter& dmx_adapter() {
    static DmxUsbCdcAdapter inst;
    return inst;
}
DmxUsbCdcAdapter* dmx_adapter_or_null() { return &dmx_adapter(); }
#else
DmxUsbCdcAdapter* dmx_adapter_or_null() { return nullptr; }
#endif

// Sink that forwards mapper events to DAL::render_fx / render_wash.
// One instance per mode; lifetime matches the mode singleton.
class DalRenderSink : public DmxChannelMapper::Sink {
public:
    void on_pulse(const char* target, const RgbPulseEvent& ev) override {
        DAL::render_fx(target, ev);
    }
    void on_wash(const char* target, const LightWashEvent& ev) override {
        DAL::render_wash(target, ev);
    }
};
DalRenderSink s_sink;

// Restoring the console baud is a separate concern from any one mode;
// declare it here so the on_button_event back-gesture and exit() can
// both call it.
#ifdef ARDUINO
constexpr uint32_t kConsoleBaud = 115200;
void restore_console_serial() {
    Serial.end();
    Serial.begin(kConsoleBaud);
}
#else
void restore_console_serial() {}
#endif

}  // namespace

void DmxBridgeMode::enter() {
    enter_ms_           = millis();
    last_draw_ms_       = 0;
    last_frame_seen_ms_ = 0;
    last_frame_count_   = 0;

    mapper_.reset();
    mapper_.set_target(kBroadcastTarget);

    // Bring up the radio so mapper-emitted LIGHT_PULSE / LIGHT_WASH
    // events reach the fleet. Reuses the same broadcast driver
    // DirectorMode uses.
    esp_now_broadcast_driver_instance()->start_broadcast(
        persistence::load_director_channel());

    // Switch the USB-CDC peripheral to the Enttec Pro baud + start
    // the parser pump.
    if (DmxUsbCdcAdapter* adapter = dmx_adapter_or_null()) {
        adapter->begin();
    }

    active_ = true;
    draw_status();
}

void DmxBridgeMode::exit() {
    // Clear the last wash on the fleet so they don't get stuck holding
    // an LD-painted baseline after the mode unloads. 1.0 s release.
    DAL::render_wash_end(kBroadcastTarget, /*release_time=*/10);

    if (DmxUsbCdcAdapter* adapter = dmx_adapter_or_null()) {
        adapter->end();
    }
    restore_console_serial();

    esp_now_broadcast_driver_instance()->stop_broadcast();
    active_ = false;
}

void DmxBridgeMode::loop_tick() {
    const uint32_t now = millis();

    // Drain whatever bytes QLC+ has written since the last tick. Each
    // returned FrameComplete is a fully-parsed Enttec Pro frame.
    DmxUsbCdcAdapter* adapter = dmx_adapter_or_null();
    if (adapter != nullptr) {
        const size_t fresh = adapter->poll();
        if (fresh > 0) {
            last_frame_seen_ms_ = now;

            // Each fresh frame potentially updates the 512-byte channel
            // table on the receive side; the parser exposes the most
            // recently completed frame's payload directly. Walk channels
            // 1..12 (skipping the start code at payload[0]) and feed the
            // mapper.
            const DmxInputParser& parser = adapter->parser();
            if (parser.last_was_dmx_packet()) {
                uint8_t ch[DmxChannelMapper::kChannelsPerGroup] = {0};
                const uint16_t copied =
                    parser.copy_dmx_channels(ch, sizeof(ch));
                if (copied >= DmxChannelMapper::kChannelsPerGroup) {
                    mapper_.process(ch, copied, now, s_sink);
                }
            }
        } else if (last_frame_seen_ms_ != 0) {
            // No fresh frame this tick, but the strobe cadence still
            // wants to fire on schedule. Re-run process() against the
            // last-known channel state so timed-strobe pulses keep
            // emitting. (Note: parser's last_payload is preserved across
            // ticks even when no new frame arrives.)
            const DmxInputParser& parser = adapter->parser();
            if (parser.last_was_dmx_packet()) {
                uint8_t ch[DmxChannelMapper::kChannelsPerGroup] = {0};
                const uint16_t copied =
                    parser.copy_dmx_channels(ch, sizeof(ch));
                if (copied >= DmxChannelMapper::kChannelsPerGroup) {
                    mapper_.process(ch, copied, now, s_sink);
                }
            }
        }
    }

    if (now - last_draw_ms_ > kDrawIntervalMs) {
        draw_status();
        last_draw_ms_ = now;
    }
}

void DmxBridgeMode::on_button_event(const dal::ButtonPressEvent& ev) {
    // Single back-gesture: long-press B exits to Menu. Matches the
    // back-gesture other runtime modes use.
    if (ev.kind == hal::ButtonEvent::LongPressed && ev.id == hal::ButtonId::Btn2) {
        ModeMachine::switch_to(ModeId::Menu);
        return;
    }
}

void DmxBridgeMode::draw_status() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 0, "DMX Bridge", YELLOW, BLACK, 1});

    const uint32_t now = millis();
    const DmxUsbCdcAdapter* adapter = dmx_adapter_or_null();
    const uint32_t frames =
        (adapter != nullptr) ? adapter->parser().frame_count() : 0;
    const uint32_t bytes  =
        (adapter != nullptr) ? adapter->bytes_read() : 0;
    const bool ever_seen   = last_frame_seen_ms_ != 0;
    const bool currently_active =
        ever_seen && ((now - last_frame_seen_ms_) < kIdleAfterMs);

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 18,
        currently_active ? "ACTIVE" : "IDLE",
        currently_active ? GREEN    : RED,
        BLACK, 3});

    // Last-frame age (ms since last) - 0 if none ever observed.
    char age_buf[32];
    if (ever_seen) {
        const uint32_t age = now - last_frame_seen_ms_;
        std::snprintf(age_buf, sizeof(age_buf), " age: %lu ms",
                      (unsigned long)age);
    } else {
        std::snprintf(age_buf, sizeof(age_buf), " age: --");
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 55, age_buf, WHITE, BLACK, 2});

    // Frame / byte counters.
    char counters_buf[40];
    std::snprintf(counters_buf, sizeof(counters_buf),
                  " f:%lu  b:%lu",
                  (unsigned long)frames, (unsigned long)bytes);
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 85, counters_buf, WHITE, BLACK, 2});

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        4, 128, "B-hold: exit  Target 00:00",
        WHITE, BLACK, 1});
}

}  // namespace modes
}  // namespace nocturnation
