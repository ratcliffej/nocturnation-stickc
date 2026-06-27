// DmxBridgeMode implementation - Epic 7 B3.

#include "dmx_bridge_mode.h"

#include "dal/dal.h"
#if defined(NOCT_DMX_ETHERNET)
#include "dal/drivers/ethernet_dmx_adapter.h"
#include "dal/drivers/net_config.h"
#else
#include "dal/drivers/dmx_usb_cdc_adapter.h"
#include "dal/drivers/dmx_input_parser.h"   // kLabelEspNowBroadcast (passthrough)
#endif
#include "../dal/drivers/espnow_broadcast_driver.h"
#include "hal/hal.h"                         // HAL::esp_now() for passthrough
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

// The DMX source differs by board: the Sticks pump Enttec Pro over USB-CDC;
// the AtomS3-PoE Director receives sACN/Art-Net over Ethernet. Both adapters
// expose the same begin/end/poll/parser/bytes_read surface, so the rest of
// the mode is source-agnostic.
#if defined(NOCT_DMX_ETHERNET)
using DmxAdapter = EthernetDmxAdapter;
#else
using DmxAdapter = DmxUsbCdcAdapter;
#endif

namespace {

// Adapter accessor. Local-static so the global Serial isn't touched
// at static-init time; the Arduino USB-CDC peripheral is brought up
// lazily on the first mode entry. Native build returns nullptr from
// dmx_adapter_or_null() so the .cpp compiles on the host test envs
// (e.g. native_modes which pulls all of src/modes/ in).
#ifdef ARDUINO
DmxAdapter& dmx_adapter() {
#if defined(NOCT_DMX_ETHERNET)
    // Shared singleton so the serial console can read the same adapter's
    // live state (frame counts, channel values).
    return ethernet_dmx_adapter_instance();
#else
    static DmxAdapter inst;
    return inst;
#endif
}
DmxAdapter* dmx_adapter_or_null() { return &dmx_adapter(); }
#else
DmxAdapter* dmx_adapter_or_null() { return nullptr; }
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
#if defined(ARDUINO) && !defined(NOCT_DMX_ETHERNET)
constexpr uint32_t kConsoleBaud = 115200;
void restore_console_serial() {
    Serial.end();
    Serial.begin(kConsoleBaud);
}
#else
// The Ethernet DMX source never touches Serial, so there's no console baud
// to restore (and the native build has no Serial).
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

void DmxBridgeMode::on_parsed_frame(const dal::DmxInputParser& parser) {
    // Universe frames are deferred to post-poll (last-wins is correct
    // for "current visible universe state"); only the discrete event
    // labels need inline dispatch.
    if (parser.last_was_dmx_packet()) return;

    if (parser.last_label() == dal::enttec_pro::kLabelEspNowBroadcast) {
        const uint8_t* p = parser.last_payload();
        const uint16_t n = parser.last_payload_len();
        // Route through the broadcast driver's send_passthrough so
        // the spec-§4.3 reliability strategy (initial send + jittered
        // retransmit queue, kRedundantSends total) applies uniformly
        // to display traffic. Previously this was a tight-loop 3x
        // send_broadcast which fired all retransmits within ~3 ms -
        // protected against single-packet collisions but vulnerable
        // to longer interference windows (a phone-WiFi burst that
        // lasts 10 ms could knock out all three). The driver's
        // queue spreads the sends over ~40-70 ms with 5-15 ms
        // pseudo-random jitter and survives that class of outage.
        if (auto* drv = dal::esp_now_broadcast_driver_instance()) {
            drv->send_passthrough(p, n);
        }
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
#if defined(NOCT_DMX_ETHERNET)
    // Atom: the ESP-NOW fleet channel comes from the network config (set on
    // the serial console), not the Stick's button-set director channel.
    esp_now_broadcast_driver_instance()->start_broadcast(
        net_config_load().wifi_channel);
#else
    esp_now_broadcast_driver_instance()->start_broadcast(
        persistence::load_director_channel());
    // Raw-RGB DMX scenes can drive the local strip to full white
    // through this mode's mappers; apply persisted brightness so
    // the operator's Config-menu cap is honoured here too. Without
    // this, the strip ran at the default 100 % regardless of
    // setting - the brownout symptom on raw-white slider scenes.
    // (Sticks only - the AtomS3-PoE Director has no LED strip.)
    DAL::apply_persisted_strip_settings();
#endif

    // Switch the USB-CDC peripheral to the Enttec Pro baud + start
    // the parser pump.
    if (DmxAdapter* adapter = dmx_adapter_or_null()) {
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

    if (DmxAdapter* adapter = dmx_adapter_or_null()) {
        adapter->end();
    }
    restore_console_serial();

    esp_now_broadcast_driver_instance()->stop_broadcast();
    active_ = false;
}

void DmxBridgeMode::loop_tick() {
    const uint32_t now = millis();

    // Drain whatever bytes arrived since the last tick. On the USB wire two
    // frame types share it: label 0x06 DMX universes (last-wins, acted on
    // after poll) and label 0x10 ESP-NOW passthrough (each frame is a
    // discrete display cue - EVERY frame matters, so it's dispatched inline
    // via the poll() callback rather than risk a passthrough being dropped
    // when it lands sandwiched between two universes in one drain). The
    // Ethernet source delivers only DMX universes, so it needs no callback.
    DmxAdapter* adapter = dmx_adapter_or_null();
    if (adapter != nullptr) {
        const uint32_t bytes_before = adapter->bytes_read();
#if defined(NOCT_DMX_ETHERNET)
        const size_t fresh = adapter->poll();
#else
        const size_t fresh = adapter->poll(&DmxBridgeMode::on_parsed_frame_thunk, this);
#endif
        // Activity = a complete frame OR any bytes/packets read this poll.
        // Tracking byte flow (not only completed frames) keeps the active
        // indicator steady when frames complete choppily under a busy loop.
        const bool   activity =
            (fresh > 0) || (adapter->bytes_read() != bytes_before);
        const bool   have_data = activity || (last_frame_seen_ms_ != 0);
        if (activity) {
            last_frame_seen_ms_ = now;
        }
        if (have_data) {
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
#if defined(NOCT_DMX_ETHERNET)
    // Headless AtomS3-PoE Director: no LCD - drive the onboard RGB status
    // LED (red/purple/amber/green diagnostic scheme; health queried inside).
    const uint32_t now    = millis();
    const bool     active = (last_frame_seen_ms_ != 0) &&
        ((now - last_frame_seen_ms_) < kIdleAfterMs);
    update_status_led(active);
#else
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 0, "DMX Bridge", YELLOW, BLACK, 1});

    const uint32_t now = millis();
    const DmxAdapter* adapter = dmx_adapter_or_null();
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

    // Last-frame age (seconds since last) - "--" if none ever observed.
    // Rounds to the nearest whole second; sub-second granularity isn't
    // useful for an idle display where the operator's question is "is
    // the orchestrator still feeding it?".
    char age_buf[32];
    if (ever_seen) {
        const uint32_t age_ms = now - last_frame_seen_ms_;
        const uint32_t age_s  = (age_ms + 500) / 1000;
        std::snprintf(age_buf, sizeof(age_buf), " age: %lu s",
                      (unsigned long)age_s);
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
#endif  // NOCT_DMX_ETHERNET
}

void DmxBridgeMode::update_status_led(bool active) {
#if defined(NOCT_DMX_ETHERNET)
    // AtomS3 Lite onboard WS2812 (GPIO 35). neopixelWrite() is the
    // arduino-esp32 built-in single-pixel driver; low brightness since the
    // LED is bright and this runs continuously. Diagnostic scheme:
    //   red    = completely broken: W5500 not detected, or the ESP-NOW
    //            radio never came up (the fleet can't be driven at all)
    //   purple = networking fault: W5500 OK but no Ethernet link (cable out)
    //   amber  = link + radio up, but no DMX flowing (console stopped,
    //            wrong universe, or nothing patched)
    //   green  = link up and DMX streaming - all good
    constexpr int kStatusLedPin = 35;   // AtomS3 Lite onboard WS2812 (38 was dark; trying 35)

    auto*      a        = dmx_adapter_or_null();
    const bool hw_ok    = (a != nullptr) && a->hardware_present();
    const bool fleet_ok = esp_now_broadcast_driver_instance()->active();

    if (!hw_ok || !fleet_ok) {
        neopixelWrite(kStatusLedPin, 40,  0,  0);   // red
    } else if (!a->link_up()) {
        neopixelWrite(kStatusLedPin, 28,  0, 40);   // purple
    } else if (!active) {
        neopixelWrite(kStatusLedPin, 40, 16,  0);   // amber
    } else {
        neopixelWrite(kStatusLedPin,  0, 40,  0);   // green
    }
#else
    (void)active;
#endif
}

}  // namespace modes
}  // namespace nocturnation
