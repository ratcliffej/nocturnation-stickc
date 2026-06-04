// Host tests for DmxChannelMapper (Epic 7 B2).
//
// Pure-logic coverage: 12-channel layout interpretation, rising-edge
// trigger detection, wash anchor change detection + debounce, strobe
// cadence, master intensity scaling, short-buffer defence.

#include <cstdint>
#include <cstring>
#include <vector>
#include <unity.h>

#include "dal/drivers/dmx_channel_mapper.h"
#include "pulse/envelope.h"

using nocturnation::dal::DmxChannelMapper;
using nocturnation::dal::LightWashEvent;
using nocturnation::dal::RgbPulseEvent;
namespace pulse = nocturnation::pulse;

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Recording sink: captures every on_pulse / on_wash call so tests can
// assert against the sequence + values.
// ---------------------------------------------------------------------------

namespace {

struct RecordedPulse {
    const char* target;
    RgbPulseEvent ev;
};
struct RecordedWash {
    const char* target;
    LightWashEvent ev;
};

class RecordingSink : public DmxChannelMapper::Sink {
public:
    std::vector<RecordedPulse> pulses;
    std::vector<RecordedWash>  washes;

    void on_pulse(const char* target, const RgbPulseEvent& ev) override {
        pulses.push_back({target, ev});
    }
    void on_wash(const char* target, const LightWashEvent& ev) override {
        washes.push_back({target, ev});
    }

    void clear() { pulses.clear(); washes.clear(); }
};

// Build a 12-channel buffer with everything zeroed plus selective
// overrides. Less ceremony in each test than declaring 12 named bytes.
struct ChannelBuf {
    uint8_t b[DmxChannelMapper::kChannelsPerGroup] = {0};
    ChannelBuf& set(uint8_t idx, uint8_t value) { b[idx] = value; return *this; }
    const uint8_t* data() const { return b; }
};

}  // namespace

// ---------------------------------------------------------------------------
// Initial wash on first call
// ---------------------------------------------------------------------------

static void test_first_call_emits_initial_wash(void) {
    DmxChannelMapper m;
    m.set_target("01:01");
    RecordingSink sink;

    ChannelBuf ch;
    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 0, sink);

    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());
    TEST_ASSERT_EQUAL_STRING("01:01", sink.washes[0].target);
    // All anchors zero, intensity zero, cycle_ms zero, pulse_response 1.
    TEST_ASSERT_EQUAL_UINT8(0, sink.washes[0].ev.r1);
    TEST_ASSERT_EQUAL_UINT8(0, sink.washes[0].ev.intensity);
    TEST_ASSERT_EQUAL_UINT16(0, sink.washes[0].ev.cycle_ms);
    TEST_ASSERT_EQUAL_UINT8(1, sink.washes[0].ev.pulse_response);
}

// ---------------------------------------------------------------------------
// Unchanged channels: no re-emit
// ---------------------------------------------------------------------------

static void test_unchanged_channels_no_reemit(void) {
    DmxChannelMapper m;
    RecordingSink sink;

    ChannelBuf ch;
    ch.set(DmxChannelMapper::kMasterIntensity, 200)
      .set(DmxChannelMapper::kWashAR, 255);

    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 0, sink);
    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());

    // Second call, same channels, well after the debounce window.
    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 1000, sink);
    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());   // no new wash
}

// ---------------------------------------------------------------------------
// Anchor change triggers wash re-emit
// ---------------------------------------------------------------------------

static void test_anchor_change_reemits_wash(void) {
    DmxChannelMapper m;
    RecordingSink sink;

    ChannelBuf ch;
    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 0, sink);
    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());

    ch.set(DmxChannelMapper::kWashBR, 0x80);
    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 1000, sink);
    TEST_ASSERT_EQUAL_size_t(2, sink.washes.size());
    TEST_ASSERT_EQUAL_UINT8(0x80, sink.washes[1].ev.r2);
}

// ---------------------------------------------------------------------------
// Master intensity change triggers wash re-emit (it's the wash's
// intensity scalar; LD turning brightness up/down should propagate)
// ---------------------------------------------------------------------------

static void test_master_change_reemits_wash(void) {
    DmxChannelMapper m;
    RecordingSink sink;

    ChannelBuf ch;
    ch.set(DmxChannelMapper::kMasterIntensity, 100);
    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 0, sink);
    TEST_ASSERT_EQUAL_UINT8(100, sink.washes[0].ev.intensity);

    ch.set(DmxChannelMapper::kMasterIntensity, 200);
    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 1000, sink);
    TEST_ASSERT_EQUAL_size_t(2, sink.washes.size());
    TEST_ASSERT_EQUAL_UINT8(200, sink.washes[1].ev.intensity);
}

// ---------------------------------------------------------------------------
// Wash re-emit is debounced: rapid changes within the gap produce one
// emit only.
// ---------------------------------------------------------------------------

static void test_wash_reemit_debounced(void) {
    DmxChannelMapper m;
    RecordingSink sink;

    ChannelBuf ch;
    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 0, sink);
    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());

    // Three rapid changes within 50 ms - none should re-emit.
    ch.set(DmxChannelMapper::kWashAR, 0x10);
    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 10, sink);
    ch.set(DmxChannelMapper::kWashAR, 0x20);
    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 30, sink);
    ch.set(DmxChannelMapper::kWashAR, 0x30);
    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 49, sink);
    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());

    // 100 ms past last emit: the debounce window has passed; next
    // change re-emits.
    ch.set(DmxChannelMapper::kWashAR, 0x40);
    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 200, sink);
    TEST_ASSERT_EQUAL_size_t(2, sink.washes.size());
    TEST_ASSERT_EQUAL_UINT8(0x40, sink.washes[1].ev.r1);
}

// ---------------------------------------------------------------------------
// Pulse trigger rising edge fires one pulse
// ---------------------------------------------------------------------------

static void test_pulse_trigger_rising_edge_fires(void) {
    DmxChannelMapper m;
    m.set_target("00:00");
    RecordingSink sink;

    ChannelBuf ch;
    ch.set(DmxChannelMapper::kMasterIntensity, 255)
      .set(DmxChannelMapper::kPulseR, 255)
      .set(DmxChannelMapper::kPulseG, 0)
      .set(DmxChannelMapper::kPulseB, 0)
      .set(DmxChannelMapper::kPulseTrigger, 200);   // fire

    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 0, sink);
    TEST_ASSERT_EQUAL_size_t(1, sink.pulses.size());
    TEST_ASSERT_EQUAL_STRING("00:00", sink.pulses[0].target);
    TEST_ASSERT_EQUAL_UINT8(255, sink.pulses[0].ev.r);
    TEST_ASSERT_EQUAL_UINT8(0,   sink.pulses[0].ev.g);
    TEST_ASSERT_EQUAL_UINT8(0,   sink.pulses[0].ev.b);
    TEST_ASSERT_EQUAL_INT((int)pulse::T_96_MS, (int)sink.pulses[0].ev.sustain);
    TEST_ASSERT_EQUAL_INT((int)pulse::CHANCE_100, (int)sink.pulses[0].ev.chance);
}

// ---------------------------------------------------------------------------
// Trigger held high: only the rising edge fires; subsequent calls with
// trigger still high do not re-fire.
// ---------------------------------------------------------------------------

static void test_held_trigger_fires_once(void) {
    DmxChannelMapper m;
    RecordingSink sink;

    ChannelBuf ch;
    ch.set(DmxChannelMapper::kMasterIntensity, 255)
      .set(DmxChannelMapper::kPulseR, 255)
      .set(DmxChannelMapper::kPulseTrigger, 200);

    for (int i = 0; i < 5; ++i) {
        m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup,
                  static_cast<uint32_t>(i * 10), sink);
    }
    TEST_ASSERT_EQUAL_size_t(1, sink.pulses.size());
}

// ---------------------------------------------------------------------------
// Trigger drops and re-rises: second pulse fires.
// ---------------------------------------------------------------------------

static void test_trigger_drop_and_rise_refires(void) {
    DmxChannelMapper m;
    RecordingSink sink;

    ChannelBuf ch;
    ch.set(DmxChannelMapper::kMasterIntensity, 255)
      .set(DmxChannelMapper::kPulseR, 255)
      .set(DmxChannelMapper::kPulseTrigger, 200);

    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 0, sink);   // fire 1
    TEST_ASSERT_EQUAL_size_t(1, sink.pulses.size());

    ch.set(DmxChannelMapper::kPulseTrigger, 0);
    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 10, sink);  // re-arm
    TEST_ASSERT_EQUAL_size_t(1, sink.pulses.size());

    ch.set(DmxChannelMapper::kPulseTrigger, 200);
    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 20, sink);  // fire 2
    TEST_ASSERT_EQUAL_size_t(2, sink.pulses.size());
}

// ---------------------------------------------------------------------------
// Master intensity scales pulse RGB
// ---------------------------------------------------------------------------

static void test_master_intensity_scales_pulse_rgb(void) {
    DmxChannelMapper m;
    RecordingSink sink;

    ChannelBuf ch;
    // Channels at full, master at 50 % = pulse RGB scaled to ~127.
    ch.set(DmxChannelMapper::kMasterIntensity, 128)
      .set(DmxChannelMapper::kPulseR, 255)
      .set(DmxChannelMapper::kPulseG, 200)
      .set(DmxChannelMapper::kPulseB, 100)
      .set(DmxChannelMapper::kPulseTrigger, 200);

    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 0, sink);
    TEST_ASSERT_EQUAL_size_t(1, sink.pulses.size());
    // (channel * master) / 255: 255*128/255=128, 200*128/255=100, 100*128/255=50.
    TEST_ASSERT_EQUAL_UINT8(128, sink.pulses[0].ev.r);
    TEST_ASSERT_EQUAL_UINT8(100, sink.pulses[0].ev.g);
    TEST_ASSERT_EQUAL_UINT8(50,  sink.pulses[0].ev.b);
}

// ---------------------------------------------------------------------------
// Strobe rate 0 doesn't fire pulses
// ---------------------------------------------------------------------------

static void test_strobe_rate_zero_no_pulses(void) {
    DmxChannelMapper m;
    RecordingSink sink;

    ChannelBuf ch;
    ch.set(DmxChannelMapper::kMasterIntensity, 255)
      .set(DmxChannelMapper::kPulseR, 255);

    // Multiple calls over 5 seconds, strobe=0 the whole time.
    for (uint32_t t = 0; t < 5000; t += 50) {
        m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, t, sink);
    }
    TEST_ASSERT_EQUAL_size_t(0, sink.pulses.size());
}

// ---------------------------------------------------------------------------
// Strobe rate change fires immediately
// ---------------------------------------------------------------------------

static void test_strobe_rate_change_fires_immediately(void) {
    DmxChannelMapper m;
    RecordingSink sink;

    ChannelBuf ch;
    ch.set(DmxChannelMapper::kMasterIntensity, 255)
      .set(DmxChannelMapper::kPulseR, 255);

    // Settle: first call with no strobe.
    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 0, sink);
    TEST_ASSERT_EQUAL_size_t(0, sink.pulses.size());

    // Strobe set to max (255 -> 4 Hz / 250 ms interval).
    ch.set(DmxChannelMapper::kStrobeRate, 255);
    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 100, sink);
    TEST_ASSERT_EQUAL_size_t(1, sink.pulses.size());   // immediate fire on rate change
}

// ---------------------------------------------------------------------------
// Strobe periodic firing at the configured cadence (255 -> 4 Hz -> ~250 ms)
// ---------------------------------------------------------------------------

static void test_strobe_periodic_at_max_rate(void) {
    DmxChannelMapper m;
    RecordingSink sink;

    ChannelBuf ch;
    ch.set(DmxChannelMapper::kMasterIntensity, 255)
      .set(DmxChannelMapper::kPulseR, 255)
      .set(DmxChannelMapper::kStrobeRate, 255);   // 4 Hz max

    // Process every 50 ms for 1100 ms. Should see fires at t=0 (rate
    // change), t=250, t=500, t=750, t=1000 = 5 pulses total.
    for (uint32_t t = 0; t <= 1100; t += 50) {
        m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, t, sink);
    }
    TEST_ASSERT_EQUAL_size_t(5, sink.pulses.size());
}

// ---------------------------------------------------------------------------
// Strobe and trigger are independent emitters - both can fire in the
// same call.
// ---------------------------------------------------------------------------

static void test_strobe_and_trigger_independent(void) {
    DmxChannelMapper m;
    RecordingSink sink;

    ChannelBuf ch;
    ch.set(DmxChannelMapper::kMasterIntensity, 255)
      .set(DmxChannelMapper::kPulseR, 255)
      .set(DmxChannelMapper::kStrobeRate, 255)    // 4 Hz; rate-change fires
      .set(DmxChannelMapper::kPulseTrigger, 200); // rising edge also fires

    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 0, sink);
    TEST_ASSERT_EQUAL_size_t(2, sink.pulses.size());
}

// ---------------------------------------------------------------------------
// Short buffer (<12 channels) is a no-op (defensive)
// ---------------------------------------------------------------------------

static void test_short_buffer_is_noop(void) {
    DmxChannelMapper m;
    RecordingSink sink;

    uint8_t partial[8] = {255, 255, 255, 0, 0, 200, 0, 0};
    m.process(partial, sizeof(partial), 0, sink);
    TEST_ASSERT_EQUAL_size_t(0, sink.pulses.size());
    TEST_ASSERT_EQUAL_size_t(0, sink.washes.size());
}

// ---------------------------------------------------------------------------
// reset() clears state - next call emits the initial wash again
// ---------------------------------------------------------------------------

static void test_reset_clears_state(void) {
    DmxChannelMapper m;
    RecordingSink sink;

    ChannelBuf ch;
    ch.set(DmxChannelMapper::kWashAR, 200);

    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 0, sink);
    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());

    m.reset();
    sink.clear();
    // Same channels - first call after reset should emit again.
    m.process(ch.data(), DmxChannelMapper::kChannelsPerGroup, 0, sink);
    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());
}

// ---------------------------------------------------------------------------
// Unity main
// ---------------------------------------------------------------------------

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_first_call_emits_initial_wash);
    RUN_TEST(test_unchanged_channels_no_reemit);
    RUN_TEST(test_anchor_change_reemits_wash);
    RUN_TEST(test_master_change_reemits_wash);
    RUN_TEST(test_wash_reemit_debounced);
    RUN_TEST(test_pulse_trigger_rising_edge_fires);
    RUN_TEST(test_held_trigger_fires_once);
    RUN_TEST(test_trigger_drop_and_rise_refires);
    RUN_TEST(test_master_intensity_scales_pulse_rgb);
    RUN_TEST(test_strobe_rate_zero_no_pulses);
    RUN_TEST(test_strobe_rate_change_fires_immediately);
    RUN_TEST(test_strobe_periodic_at_max_rate);
    RUN_TEST(test_strobe_and_trigger_independent);
    RUN_TEST(test_short_buffer_is_noop);
    RUN_TEST(test_reset_clears_state);
    return UNITY_END();
}
