// Host tests for DmxChannelMapper (Epic 7 B7 per-group + extended-
// parameter layout).
//
// Coverage:
//   - 23-channel per-block layout (master / strobe / pulse + envelope /
//     wash + extended params)
//   - target_group baked into the mapper's "00:XX" target string
//   - Per-instance state isolation (Group 1 doesn't leak to Group 2)
//   - Rising-edge pulse trigger + arming
//   - Wash change detection (anchors + every extended param)
//   - Wash debounce
//   - Strobe cadence (preserved from v1)
//   - Master scaling on pulse RGB + wash intensity
//   - Short-buffer defence
//   - Enum quantization (Time + Chance with inversion)

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
// Test fixtures
// ---------------------------------------------------------------------------

namespace {

struct RecordedPulse {
    const char*   target;
    RgbPulseEvent ev;
};
struct RecordedWash {
    const char*    target;
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

// 23-channel block buffer with everything zeroed plus selective overrides.
// Less ceremony in each test than declaring 23 named bytes.
struct BlockBuf {
    uint8_t b[DmxChannelMapper::kActiveChannelsPerBlock] = {0};
    BlockBuf& set(uint8_t idx, uint8_t value) { b[idx] = value; return *this; }
    const uint8_t* data() const { return b; }
};

constexpr uint16_t kBlockChannels = DmxChannelMapper::kActiveChannelsPerBlock;

}  // namespace

// ---------------------------------------------------------------------------
// target_group -> target string format
// ---------------------------------------------------------------------------

static void test_target_string_broadcast(void) {
    DmxChannelMapper m(0);
    TEST_ASSERT_EQUAL_STRING("00:00", m.target());
    TEST_ASSERT_EQUAL_UINT8(0, m.target_group());
}

static void test_target_string_groups(void) {
    DmxChannelMapper m1(1);
    DmxChannelMapper m9(9);
    DmxChannelMapper m15(15);
    TEST_ASSERT_EQUAL_STRING("00:01", m1.target());
    TEST_ASSERT_EQUAL_STRING("00:09", m9.target());
    TEST_ASSERT_EQUAL_STRING("00:0F", m15.target());
}

// ---------------------------------------------------------------------------
// First call seeds wash; unchanged channels do not re-emit
// ---------------------------------------------------------------------------

static void test_first_call_emits_initial_wash(void) {
    DmxChannelMapper m(1);
    RecordingSink sink;

    BlockBuf ch;
    m.process(ch.data(), kBlockChannels, 0, sink);

    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());
    TEST_ASSERT_EQUAL_STRING("00:01", sink.washes[0].target);
    TEST_ASSERT_EQUAL_UINT8(0,  sink.washes[0].ev.r1);
    TEST_ASSERT_EQUAL_UINT8(0,  sink.washes[0].ev.intensity);
    TEST_ASSERT_EQUAL_UINT16(0, sink.washes[0].ev.cycle_ms);
    // Hardcoded receiver-friendly defaults (no longer LD-exposed):
    // ttl=0 (30-min failsafe takes over), pulse_response=1 (sparkle
    // on wash works by default).
    TEST_ASSERT_EQUAL_UINT16(0, sink.washes[0].ev.ttl_seconds);
    TEST_ASSERT_EQUAL_UINT8 (1, sink.washes[0].ev.pulse_response);
}

static void test_unchanged_channels_no_reemit(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    ch.set(DmxChannelMapper::kMasterIntensity, 200)
      .set(DmxChannelMapper::kWashAR, 255);

    m.process(ch.data(), kBlockChannels, 0, sink);
    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());

    m.process(ch.data(), kBlockChannels, 1000, sink);
    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());  // no new wash
}

// ---------------------------------------------------------------------------
// Anchor + extended-param changes re-emit
// ---------------------------------------------------------------------------

static void test_anchor_change_reemits_wash(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    m.process(ch.data(), kBlockChannels, 0, sink);    // initial
    sink.clear();

    ch.set(DmxChannelMapper::kWashAR, 255);
    m.process(ch.data(), kBlockChannels, 200, sink);  // past debounce

    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());
    TEST_ASSERT_EQUAL_UINT8(255, sink.washes[0].ev.r1);
}

static void test_cycle_change_reemits_wash(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    m.process(ch.data(), kBlockChannels, 0, sink);
    sink.clear();

    ch.set(DmxChannelMapper::kWashCycle, 100);  // 100 * 100ms = 10000ms
    m.process(ch.data(), kBlockChannels, 200, sink);

    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());
    TEST_ASSERT_EQUAL_UINT16(10000, sink.washes[0].ev.cycle_ms);
}

static void test_intensity_change_reemits_wash(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    ch.set(DmxChannelMapper::kMasterIntensity, 255);  // pass-through
    m.process(ch.data(), kBlockChannels, 0, sink);
    sink.clear();

    ch.set(DmxChannelMapper::kWashIntensity, 200);
    m.process(ch.data(), kBlockChannels, 200, sink);

    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());
    TEST_ASSERT_EQUAL_UINT8(200, sink.washes[0].ev.intensity);
}

static void test_ttl_and_pulse_response_hardcoded(void) {
    // Wash TTL (formerly channels 20-21) and Wash Pulse Response
    // (formerly channel 22) were removed from the LD surface in
    // favour of receiver-friendly hardcoded defaults so sparkle-on-
    // wash works without the operator having to know about the gate.
    // The wire protocol still carries both fields - this test pins
    // the DMX-bridge-side hardcoded values.
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    // Drive the wash anchors so an emit fires.
    ch.set(DmxChannelMapper::kWashAR, 255);
    m.process(ch.data(), kBlockChannels, 0, sink);

    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());
    TEST_ASSERT_EQUAL_UINT16(0, sink.washes[0].ev.ttl_seconds);
    TEST_ASSERT_EQUAL_UINT8 (1, sink.washes[0].ev.pulse_response);
}

static void test_wash_attack_release_pass_through(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    ch.set(DmxChannelMapper::kWashAttack, 25)   // 2.5 s
      .set(DmxChannelMapper::kWashRelease, 50); // 5.0 s
    m.process(ch.data(), kBlockChannels, 0, sink);

    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());
    TEST_ASSERT_EQUAL_UINT8(25, sink.washes[0].ev.attack);
    TEST_ASSERT_EQUAL_UINT8(50, sink.washes[0].ev.release);
}

static void test_wash_reemit_debounced(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    m.process(ch.data(), kBlockChannels, 0, sink);
    sink.clear();

    ch.set(DmxChannelMapper::kWashAR, 10);
    m.process(ch.data(), kBlockChannels, 10, sink);   // within 50ms
    TEST_ASSERT_EQUAL_size_t(0, sink.washes.size());  // debounced

    m.process(ch.data(), kBlockChannels, 30, sink);   // still within
    TEST_ASSERT_EQUAL_size_t(0, sink.washes.size());

    m.process(ch.data(), kBlockChannels, 200, sink);  // past
    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());
}

// ---------------------------------------------------------------------------
// Pulse: rising-edge trigger + arming
// ---------------------------------------------------------------------------

static void test_pulse_trigger_rising_edge_fires(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    ch.set(DmxChannelMapper::kMasterIntensity, 255)
      .set(DmxChannelMapper::kPulseR, 200)
      .set(DmxChannelMapper::kPulseTrigger, 200);
    m.process(ch.data(), kBlockChannels, 0, sink);

    TEST_ASSERT_EQUAL_size_t(1, sink.pulses.size());
    TEST_ASSERT_EQUAL_UINT8(200, sink.pulses[0].ev.r);
    TEST_ASSERT_EQUAL_STRING("00:00", sink.pulses[0].target);
}

static void test_held_trigger_fires_once(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    ch.set(DmxChannelMapper::kPulseTrigger, 200);
    m.process(ch.data(), kBlockChannels, 0, sink);
    m.process(ch.data(), kBlockChannels, 10, sink);
    m.process(ch.data(), kBlockChannels, 20, sink);

    TEST_ASSERT_EQUAL_size_t(1, sink.pulses.size());
}

static void test_trigger_drop_and_rise_refires(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    ch.set(DmxChannelMapper::kPulseTrigger, 200);
    m.process(ch.data(), kBlockChannels, 0, sink);
    TEST_ASSERT_EQUAL_size_t(1, sink.pulses.size());

    ch.set(DmxChannelMapper::kPulseTrigger, 50);
    m.process(ch.data(), kBlockChannels, 10, sink);    // disarm
    TEST_ASSERT_EQUAL_size_t(1, sink.pulses.size());

    ch.set(DmxChannelMapper::kPulseTrigger, 200);
    m.process(ch.data(), kBlockChannels, 20, sink);    // rearm + fire
    TEST_ASSERT_EQUAL_size_t(2, sink.pulses.size());
}

// ---------------------------------------------------------------------------
// Master intensity scaling on pulse RGB
// ---------------------------------------------------------------------------

static void test_master_intensity_scales_pulse_rgb(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    ch.set(DmxChannelMapper::kMasterIntensity, 128)  // ~50%
      .set(DmxChannelMapper::kPulseR, 200)
      .set(DmxChannelMapper::kPulseG, 100)
      .set(DmxChannelMapper::kPulseB, 50)
      .set(DmxChannelMapper::kPulseTrigger, 200);
    m.process(ch.data(), kBlockChannels, 0, sink);

    TEST_ASSERT_EQUAL_size_t(1, sink.pulses.size());
    // (channel * 128) / 255 with integer truncation
    TEST_ASSERT_EQUAL_UINT8(100, sink.pulses[0].ev.r);  // 200*128/255 = 100
    TEST_ASSERT_EQUAL_UINT8(50,  sink.pulses[0].ev.g);  // 100*128/255 = 50
    TEST_ASSERT_EQUAL_UINT8(25,  sink.pulses[0].ev.b);  // 50*128/255 = 25
}

// ---------------------------------------------------------------------------
// Pulse envelope from channels (attack / sustain / release / probability)
// ---------------------------------------------------------------------------

static void test_pulse_envelope_from_channels(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    ch.set(DmxChannelMapper::kPulseAttack,      0)    // T_0_MS
      .set(DmxChannelMapper::kPulseSustain,    64)    // T_96_MS (idx 2)
      .set(DmxChannelMapper::kPulseRelease,   128)    // T_480_MS (idx 4)
      .set(DmxChannelMapper::kPulseProbability, 255)  // CHANCE_100 (inverted)
      .set(DmxChannelMapper::kPulseTrigger,    200);
    m.process(ch.data(), kBlockChannels, 0, sink);

    TEST_ASSERT_EQUAL_size_t(1, sink.pulses.size());
    TEST_ASSERT_EQUAL_UINT8(pulse::T_0_MS,      sink.pulses[0].ev.attack);
    TEST_ASSERT_EQUAL_UINT8(pulse::T_96_MS,     sink.pulses[0].ev.sustain);
    TEST_ASSERT_EQUAL_UINT8(pulse::T_480_MS,    sink.pulses[0].ev.release);
    TEST_ASSERT_EQUAL_UINT8(pulse::CHANCE_100,  sink.pulses[0].ev.chance);
}

// ---------------------------------------------------------------------------
// Strobe channel (preserved from v1)
// ---------------------------------------------------------------------------

static void test_strobe_rate_zero_no_pulses(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    ch.set(DmxChannelMapper::kStrobeRate, 0)
      .set(DmxChannelMapper::kMasterIntensity, 255);
    m.process(ch.data(), kBlockChannels, 0, sink);
    m.process(ch.data(), kBlockChannels, 1000, sink);

    TEST_ASSERT_EQUAL_size_t(0, sink.pulses.size());
}

static void test_strobe_rate_change_fires_immediately(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    ch.set(DmxChannelMapper::kStrobeRate, 128)
      .set(DmxChannelMapper::kPulseR, 100);
    m.process(ch.data(), kBlockChannels, 0, sink);

    TEST_ASSERT_EQUAL_size_t(1, sink.pulses.size());
}

static void test_strobe_periodic_at_max_rate(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    ch.set(DmxChannelMapper::kStrobeRate, 255)
      .set(DmxChannelMapper::kPulseR, 100);
    m.process(ch.data(), kBlockChannels, 0, sink);
    TEST_ASSERT_EQUAL_size_t(1, sink.pulses.size());

    // At 4 Hz max, the next fire is at +250 ms.
    m.process(ch.data(), kBlockChannels, 100, sink);
    TEST_ASSERT_EQUAL_size_t(1, sink.pulses.size());  // not yet
    m.process(ch.data(), kBlockChannels, 250, sink);
    TEST_ASSERT_EQUAL_size_t(2, sink.pulses.size());
}

// ---------------------------------------------------------------------------
// Defence + reset
// ---------------------------------------------------------------------------

static void test_short_buffer_is_noop(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    m.process(ch.data(), 10, 0, sink);  // < 23 channels
    TEST_ASSERT_EQUAL_size_t(0, sink.washes.size());
    TEST_ASSERT_EQUAL_size_t(0, sink.pulses.size());

    m.process(nullptr, kBlockChannels, 0, sink);
    TEST_ASSERT_EQUAL_size_t(0, sink.washes.size());
}

static void test_reset_clears_state(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    ch.set(DmxChannelMapper::kMasterIntensity, 200);
    m.process(ch.data(), kBlockChannels, 0, sink);
    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());

    sink.clear();
    m.reset();

    // After reset, processing the same channels emits an initial wash
    // again (wash_seeded_ flipped back to false).
    m.process(ch.data(), kBlockChannels, 100, sink);
    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());
}

// ---------------------------------------------------------------------------
// Per-instance state isolation - the architectural reason for B7
// ---------------------------------------------------------------------------

static void test_two_mappers_independent_state(void) {
    DmxChannelMapper m_g1(1);
    DmxChannelMapper m_g2(2);
    RecordingSink sink;

    BlockBuf ch_g1;
    ch_g1.set(DmxChannelMapper::kWashAR, 255);
    BlockBuf ch_g2;
    ch_g2.set(DmxChannelMapper::kWashAB, 200);

    m_g1.process(ch_g1.data(), kBlockChannels, 0, sink);
    m_g2.process(ch_g2.data(), kBlockChannels, 0, sink);

    TEST_ASSERT_EQUAL_size_t(2, sink.washes.size());
    TEST_ASSERT_EQUAL_STRING("00:01", sink.washes[0].target);
    TEST_ASSERT_EQUAL_STRING("00:02", sink.washes[1].target);
    TEST_ASSERT_EQUAL_UINT8(255, sink.washes[0].ev.r1);
    TEST_ASSERT_EQUAL_UINT8(200, sink.washes[1].ev.b1);

    // Now move only g1's channel; g2 must not re-emit.
    sink.clear();
    ch_g1.set(DmxChannelMapper::kWashAG, 128);
    m_g1.process(ch_g1.data(), kBlockChannels, 200, sink);
    m_g2.process(ch_g2.data(), kBlockChannels, 200, sink);
    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());
    TEST_ASSERT_EQUAL_STRING("00:01", sink.washes[0].target);
}

// ---------------------------------------------------------------------------
// Enum quantization (static helpers)
// ---------------------------------------------------------------------------

static void test_quantize_time_buckets(void) {
    TEST_ASSERT_EQUAL_UINT8(pulse::T_0_MS,    DmxChannelMapper::quantize_time(0));
    TEST_ASSERT_EQUAL_UINT8(pulse::T_0_MS,    DmxChannelMapper::quantize_time(31));
    TEST_ASSERT_EQUAL_UINT8(pulse::T_32_MS,   DmxChannelMapper::quantize_time(32));
    TEST_ASSERT_EQUAL_UINT8(pulse::T_32_MS,   DmxChannelMapper::quantize_time(63));
    TEST_ASSERT_EQUAL_UINT8(pulse::T_96_MS,   DmxChannelMapper::quantize_time(64));
    TEST_ASSERT_EQUAL_UINT8(pulse::T_480_MS,  DmxChannelMapper::quantize_time(128));
    TEST_ASSERT_EQUAL_UINT8(pulse::T_3840_MS, DmxChannelMapper::quantize_time(224));
    TEST_ASSERT_EQUAL_UINT8(pulse::T_3840_MS, DmxChannelMapper::quantize_time(255));
}

static void test_quantize_chance_inverted_buckets(void) {
    // Low slider -> low chance (CHANCE_4 is "almost never fires");
    // high slider -> high chance (CHANCE_100 is "every Lume fires").
    TEST_ASSERT_EQUAL_UINT8(pulse::CHANCE_4,   DmxChannelMapper::quantize_chance(0));
    TEST_ASSERT_EQUAL_UINT8(pulse::CHANCE_4,   DmxChannelMapper::quantize_chance(31));
    TEST_ASSERT_EQUAL_UINT8(pulse::CHANCE_10,  DmxChannelMapper::quantize_chance(32));
    TEST_ASSERT_EQUAL_UINT8(pulse::CHANCE_50,  DmxChannelMapper::quantize_chance(128));
    TEST_ASSERT_EQUAL_UINT8(pulse::CHANCE_100, DmxChannelMapper::quantize_chance(224));
    TEST_ASSERT_EQUAL_UINT8(pulse::CHANCE_100, DmxChannelMapper::quantize_chance(255));
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_target_string_broadcast);
    RUN_TEST(test_target_string_groups);
    RUN_TEST(test_first_call_emits_initial_wash);
    RUN_TEST(test_unchanged_channels_no_reemit);
    RUN_TEST(test_anchor_change_reemits_wash);
    RUN_TEST(test_cycle_change_reemits_wash);
    RUN_TEST(test_intensity_change_reemits_wash);
    RUN_TEST(test_ttl_and_pulse_response_hardcoded);
    RUN_TEST(test_wash_attack_release_pass_through);
    RUN_TEST(test_wash_reemit_debounced);
    RUN_TEST(test_pulse_trigger_rising_edge_fires);
    RUN_TEST(test_held_trigger_fires_once);
    RUN_TEST(test_trigger_drop_and_rise_refires);
    RUN_TEST(test_master_intensity_scales_pulse_rgb);
    RUN_TEST(test_pulse_envelope_from_channels);
    RUN_TEST(test_strobe_rate_zero_no_pulses);
    RUN_TEST(test_strobe_rate_change_fires_immediately);
    RUN_TEST(test_strobe_periodic_at_max_rate);
    RUN_TEST(test_short_buffer_is_noop);
    RUN_TEST(test_reset_clears_state);
    RUN_TEST(test_two_mappers_independent_state);
    RUN_TEST(test_quantize_time_buckets);
    RUN_TEST(test_quantize_chance_inverted_buckets);
    return UNITY_END();
}
