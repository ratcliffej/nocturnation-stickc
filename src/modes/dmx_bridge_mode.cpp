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

// Definitions of the static layout arrays so they get a single linkage
// home; the in-class initialisers are only declarations under C++17.
constexpr uint16_t DmxBridgeMode::kBlockBase[DmxBridgeMode::kBlockCount];
constexpr uint16_t DmxBridgeMode::kBlockSize[DmxBridgeMode::kBlockCount];

DmxBridgeMode::DmxBridgeMode()
    : mapper_broadcast_(0),
      mapper_g1_(1),
      mapper_g2_(2),
      mapper_g3_(3),
      mapper_g4_(4),
      mapper_g5_(5),
      mapper_g6_(6) {}

DmxChannelMapper* DmxBridgeMode::mapper_at(size_t i) {
    switch (i) {
        case 0: return &mapper_broadcast_;
        case 1: return &mapper_g1_;
        case 2: return &mapper_g2_;
        case 3: return &mapper_g3_;
        case 4: return &mapper_g4_;
        case 5: return &mapper_g5_;
        case 6: return &mapper_g6_;
        default: return nullptr;
    }
}

void DmxBridgeMode::run_mappers(const uint8_t* universe_buf,
                                uint16_t copied,
                                uint32_t now) {
    for (size_t i = 0; i < kBlockCount; ++i) {
        const uint16_t base = kBlockBase[i];
        const uint16_t size = kBlockSize[i];
        // Skip blocks the parser slice doesn't reach. A short slice
        // means a partial DMX-512 packet (rare with QLC+, which always
        // fills the universe); skipping is safer than reading past.
        if (copied < base + size) continue;
        mapper_at(i)->process(universe_buf + base, size, now, s_sink);
    }
}

void DmxBridgeMode::enter() {
    enter_ms_           = millis();
    last_draw_ms_       = 0;
    last_frame_seen_ms_ = 0;
    last_frame_count_   = 0;

    for (size_t i = 0; i < kBlockCount; ++i) {
        mapper_at(i)->reset();
    }

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
    // Broadcast target (target_group=0) reaches every Lume regardless
    // of which group they're configured for.
    DAL::render_wash_end(kExitClearTarget, /*release_time=*/10);

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
        const bool   have_data =
            (fresh > 0) || (last_frame_seen_ms_ != 0);
        if (fresh > 0) {
            last_frame_seen_ms_ = now;
        }
        if (have_data) {
            // Pull the latest channel slice from the parser into our
            // own buffer. parser.last_payload survives across ticks,
            // so strobe cadence keeps firing on schedule even when no
            // new frame arrived.
            const DmxInputParser& parser = adapter->parser();
            if (parser.last_was_dmx_packet()) {
                const uint16_t copied =
                    parser.copy_dmx_channels(universe_, kUniverseBufferSize);
                run_mappers(universe_, copied, now);
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
