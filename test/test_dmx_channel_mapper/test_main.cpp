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
    TEST_ASSERT_EQUAL_UINT8(0,  sink.washes[0].ev.pulse_response);
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

static void test_ttl_16bit_le_packing(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    m.process(ch.data(), kBlockChannels, 0, sink);
    sink.clear();

    ch.set(DmxChannelMapper::kWashTtlLo, 0x10)
      .set(DmxChannelMapper::kWashTtlHi, 0x27);  // 0x2710 = 10000 seconds
    m.process(ch.data(), kBlockChannels, 200, sink);

    TEST_ASSERT_EQUAL_size_t(1, sink.washes.size());
    TEST_ASSERT_EQUAL_UINT16(10000, sink.washes[0].ev.ttl_seconds);
}

static void test_pulse_response_threshold(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;

    BlockBuf ch;
    ch.set(DmxChannelMapper::kWashPulseResponse, 200);  // >=128 -> 1
    m.process(ch.data(), kBlockChannels, 0, sink);
    TEST_ASSERT_EQUAL_UINT8(1, sink.washes[0].ev.pulse_response);

    sink.clear();
    ch.set(DmxChannelMapper::kWashPulseResponse, 100);  // <128 -> 0
    m.process(ch.data(), kBlockChannels, 200, sink);
    TEST_ASSERT_EQUAL_UINT8(0, sink.washes[0].ev.pulse_response);
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
// Raw-RGB path (EMF artist-stage feature 2026-06-23)
// ---------------------------------------------------------------------------

static void test_raw_disabled_does_not_emit_static_wash(void) {
    // kRawEnable below the 128 threshold -> FX path runs as normal.
    DmxChannelMapper m(0);
    RecordingSink sink;
    BlockBuf b;
    b.set(DmxChannelMapper::kRawR, 255)
     .set(DmxChannelMapper::kRawG, 128)
     .set(DmxChannelMapper::kRawB, 64)
     .set(DmxChannelMapper::kRawEnable, 127);   // just under threshold
    m.process(b.data(), kBlockChannels, /*now_ms=*/0, sink);
    // We expect at most the normal wash-seeding emit; that emit comes
    // from the FX wash channels which are all zero - the contents are
    // not what this test cares about. Critically the emitted wash must
    // NOT be the raw RGB triplet (which would be 255/128/64 on r1/r2).
    for (const auto& rw : sink.washes) { const auto& w = rw.ev;
        TEST_ASSERT_NOT_EQUAL(255, w.r1);
        TEST_ASSERT_NOT_EQUAL(255, w.r2);
    }
}

static void test_raw_enabled_emits_static_wash(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;
    BlockBuf b;
    b.set(DmxChannelMapper::kMasterIntensity, 255)
     .set(DmxChannelMapper::kRawR, 200)
     .set(DmxChannelMapper::kRawG, 100)
     .set(DmxChannelMapper::kRawB, 50)
     .set(DmxChannelMapper::kRawEnable, 200);
    m.process(b.data(), kBlockChannels, /*now_ms=*/0, sink);
    // Exactly one wash event, static (r1 == r2 etc.), with the raw
    // RGB triplet at full master, cycle_ms == 0, infinite TTL.
    TEST_ASSERT_EQUAL_INT(1, sink.washes.size());
    const auto& w = sink.washes[0].ev;
    TEST_ASSERT_EQUAL_UINT8(200, w.r1);  TEST_ASSERT_EQUAL_UINT8(200, w.r2);
    TEST_ASSERT_EQUAL_UINT8(100, w.g1);  TEST_ASSERT_EQUAL_UINT8(100, w.g2);
    TEST_ASSERT_EQUAL_UINT8(50,  w.b1);  TEST_ASSERT_EQUAL_UINT8(50,  w.b2);
    TEST_ASSERT_EQUAL_UINT16(0, w.cycle_ms);
    TEST_ASSERT_EQUAL_UINT16(0, w.ttl_seconds);
    TEST_ASSERT_EQUAL_UINT8(255, w.intensity);
}

static void test_raw_scales_by_master(void) {
    // EMF stage team's LD-fixture convention: Master is the dimmer.
    // Raw RGB at 200 with master at 128 emits ~100 on the wire.
    DmxChannelMapper m(0);
    RecordingSink sink;
    BlockBuf b;
    b.set(DmxChannelMapper::kMasterIntensity, 128)
     .set(DmxChannelMapper::kRawR, 200)
     .set(DmxChannelMapper::kRawG, 200)
     .set(DmxChannelMapper::kRawB, 200)
     .set(DmxChannelMapper::kRawEnable, 200);
    m.process(b.data(), kBlockChannels, /*now_ms=*/0, sink);
    TEST_ASSERT_EQUAL_INT(1, sink.washes.size());
    // 200 * 128 / 255 = 100.39 -> rounds to 100. Allow +/- 1 for
    // integer-division choice.
    const uint8_t scaled = sink.washes[0].ev.r1;
    TEST_ASSERT_TRUE(scaled >= 99 && scaled <= 101);
}

static void test_raw_unchanged_does_not_reemit(void) {
    // After the initial emit, identical raw values across ticks must
    // not produce duplicate washes - same idempotency contract as the
    // FX wash path.
    DmxChannelMapper m(0);
    RecordingSink sink;
    BlockBuf b;
    b.set(DmxChannelMapper::kMasterIntensity, 255)
     .set(DmxChannelMapper::kRawR, 100)
     .set(DmxChannelMapper::kRawG, 100)
     .set(DmxChannelMapper::kRawB, 100)
     .set(DmxChannelMapper::kRawEnable, 200);
    m.process(b.data(), kBlockChannels, /*now_ms=*/0, sink);
    m.process(b.data(), kBlockChannels, /*now_ms=*/1000, sink);
    m.process(b.data(), kBlockChannels, /*now_ms=*/2000, sink);
    TEST_ASSERT_EQUAL_INT(1, sink.washes.size());
}

static void test_raw_change_reemits(void) {
    DmxChannelMapper m(0);
    RecordingSink sink;
    BlockBuf b;
    b.set(DmxChannelMapper::kMasterIntensity, 255)
     .set(DmxChannelMapper::kRawR, 100)
     .set(DmxChannelMapper::kRawEnable, 200);
    m.process(b.data(), kBlockChannels, /*now_ms=*/0,    sink);
    // Past the kMinWashEmitGapMs debounce so the new colour can emit.
    b.set(DmxChannelMapper::kRawR, 200);
    m.process(b.data(), kBlockChannels, /*now_ms=*/1000, sink);
    TEST_ASSERT_EQUAL_INT(2, sink.washes.size());
    TEST_ASSERT_EQUAL_UINT8(100, sink.washes[0].ev.r1);
    TEST_ASSERT_EQUAL_UINT8(200, sink.washes[1].ev.r1);
}

static void test_raw_suppresses_fx_pulse(void) {
    // Pulse-trigger rising edge that would normally fire a LIGHT_PULSE
    // must be SUPPRESSED while raw mode is active - the stage team
    // wants dumb-fixture behaviour, no surprise flashes.
    DmxChannelMapper m(0);
    RecordingSink sink;
    BlockBuf b;
    b.set(DmxChannelMapper::kMasterIntensity, 255)
     .set(DmxChannelMapper::kPulseR, 255)
     .set(DmxChannelMapper::kPulseTrigger, 255)   // rising edge
     .set(DmxChannelMapper::kRawR, 50)
     .set(DmxChannelMapper::kRawEnable, 200);
    m.process(b.data(), kBlockChannels, /*now_ms=*/0, sink);
    TEST_ASSERT_EQUAL_INT(0, sink.pulses.size());
    // Only the raw wash fired.
    TEST_ASSERT_EQUAL_INT(1, sink.washes.size());
}

static void test_raw_disengage_reseeds_fx_wash(void) {
    // Engage raw mode -> raw wash emitted. Disengage raw -> FX wash
    // path must re-emit on the next tick (even if FX channels are
    // unchanged from before raw mode engaged) so receivers don't keep
    // showing the stale raw colour.
    DmxChannelMapper m(0);
    RecordingSink sink;
    BlockBuf b;
    b.set(DmxChannelMapper::kMasterIntensity, 255)
     .set(DmxChannelMapper::kWashAR, 50)
     .set(DmxChannelMapper::kWashAG, 50)
     .set(DmxChannelMapper::kWashAB, 50)
     .set(DmxChannelMapper::kWashIntensity, 200);
    // Tick 1: no raw -> FX wash emits (seeded).
    m.process(b.data(), kBlockChannels, /*now_ms=*/0, sink);
    TEST_ASSERT_EQUAL_INT(1, sink.washes.size());
    // Tick 2: engage raw with a different colour.
    b.set(DmxChannelMapper::kRawR, 200)
     .set(DmxChannelMapper::kRawG, 0)
     .set(DmxChannelMapper::kRawB, 0)
     .set(DmxChannelMapper::kRawEnable, 200);
    m.process(b.data(), kBlockChannels, /*now_ms=*/1000, sink);
    TEST_ASSERT_EQUAL_INT(2, sink.washes.size());
    TEST_ASSERT_EQUAL_UINT8(200, sink.washes[1].ev.r1);
    // Tick 3: disengage raw.
    b.set(DmxChannelMapper::kRawEnable, 0);
    m.process(b.data(), kBlockChannels, /*now_ms=*/2000, sink);
    // FX wash re-emitted with the original FX channel values, even
    // though the FX channels themselves haven't changed since tick 1.
    TEST_ASSERT_EQUAL_INT(3, sink.washes.size());
    TEST_ASSERT_EQUAL_UINT8(50, sink.washes[2].ev.r1);
}


// ---------------------------------------------------------------------------
// preview_rgb - steady-state colour preview (AtomS3R Director dashboard)
// ---------------------------------------------------------------------------

static void test_preview_short_or_null_slice_is_black(void) {
    uint8_t r = 9, g = 9, b = 9;
    BlockBuf buf;
    buf.set(DmxChannelMapper::kWashAR, 255)
       .set(DmxChannelMapper::kMasterIntensity, 255)
       .set(DmxChannelMapper::kWashIntensity, 255);
    DmxChannelMapper::preview_rgb(buf.data(), kBlockChannels - 1, r, g, b);
    TEST_ASSERT_EQUAL_UINT8(0, r);
    TEST_ASSERT_EQUAL_UINT8(0, g);
    TEST_ASSERT_EQUAL_UINT8(0, b);

    r = g = b = 9;
    DmxChannelMapper::preview_rgb(nullptr, kBlockChannels, r, g, b);
    TEST_ASSERT_EQUAL_UINT8(0, r);
    TEST_ASSERT_EQUAL_UINT8(0, g);
    TEST_ASSERT_EQUAL_UINT8(0, b);
}

static void test_preview_wash_scales_by_intensity_and_master(void) {
    // FX path: anchor A x (intensity x master / 255) / 255 - the same
    // collapsed product a Lume displays from the wire's separate anchor +
    // intensity fields.
    BlockBuf buf;
    buf.set(DmxChannelMapper::kMasterIntensity, 128)
       .set(DmxChannelMapper::kWashAR, 200)
       .set(DmxChannelMapper::kWashAG, 100)
       .set(DmxChannelMapper::kWashAB, 0)
       .set(DmxChannelMapper::kWashIntensity, 255);
    uint8_t r, g, b;
    DmxChannelMapper::preview_rgb(buf.data(), kBlockChannels, r, g, b);
    // intensity = 255*128/255 = 128; r = 200*128/255 = 100; g = 100*128/255 = 50.
    TEST_ASSERT_EQUAL_UINT8(100, r);
    TEST_ASSERT_EQUAL_UINT8(50,  g);
    TEST_ASSERT_EQUAL_UINT8(0,   b);
}

static void test_preview_wash_zero_intensity_is_black(void) {
    BlockBuf buf;
    buf.set(DmxChannelMapper::kMasterIntensity, 255)
       .set(DmxChannelMapper::kWashAR, 255)
       .set(DmxChannelMapper::kWashAG, 255)
       .set(DmxChannelMapper::kWashAB, 255);
    // kWashIntensity left at 0.
    uint8_t r, g, b;
    DmxChannelMapper::preview_rgb(buf.data(), kBlockChannels, r, g, b);
    TEST_ASSERT_EQUAL_UINT8(0, r);
    TEST_ASSERT_EQUAL_UINT8(0, g);
    TEST_ASSERT_EQUAL_UINT8(0, b);
}

static void test_preview_raw_overrides_wash(void) {
    // Raw enable high: preview shows the master-scaled raw colour and
    // ignores the wash channels entirely.
    BlockBuf buf;
    buf.set(DmxChannelMapper::kMasterIntensity, 255)
       .set(DmxChannelMapper::kWashAR, 255)
       .set(DmxChannelMapper::kWashIntensity, 255)
       .set(DmxChannelMapper::kRawR, 10)
       .set(DmxChannelMapper::kRawG, 20)
       .set(DmxChannelMapper::kRawB, 30)
       .set(DmxChannelMapper::kRawEnable, DmxChannelMapper::kRawEnableThreshold);
    uint8_t r, g, b;
    DmxChannelMapper::preview_rgb(buf.data(), kBlockChannels, r, g, b);
    TEST_ASSERT_EQUAL_UINT8(10, r);
    TEST_ASSERT_EQUAL_UINT8(20, g);
    TEST_ASSERT_EQUAL_UINT8(30, b);
}

static void test_preview_raw_min1_floor_matches_wire(void) {
    // Non-zero LD input never scales to 0 on the raw path (the anti-
    // flicker floor in maybe_emit_raw_wash_on_change); the preview must
    // agree with the wire byte-for-byte. 5 * 50 / 255 = 0 -> floored to 1.
    BlockBuf buf;
    buf.set(DmxChannelMapper::kMasterIntensity, 50)
       .set(DmxChannelMapper::kRawR, 5)
       .set(DmxChannelMapper::kRawEnable, 255);
    uint8_t r, g, b;
    DmxChannelMapper::preview_rgb(buf.data(), kBlockChannels, r, g, b);
    TEST_ASSERT_EQUAL_UINT8(1, r);
    TEST_ASSERT_EQUAL_UINT8(0, g);  // genuine zero stays zero
    TEST_ASSERT_EQUAL_UINT8(0, b);
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
    RUN_TEST(test_ttl_16bit_le_packing);
    RUN_TEST(test_pulse_response_threshold);
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
    RUN_TEST(test_raw_disabled_does_not_emit_static_wash);
    RUN_TEST(test_raw_enabled_emits_static_wash);
    RUN_TEST(test_raw_scales_by_master);
    RUN_TEST(test_raw_unchanged_does_not_reemit);
    RUN_TEST(test_raw_change_reemits);
    RUN_TEST(test_raw_suppresses_fx_pulse);
    RUN_TEST(test_raw_disengage_reseeds_fx_wash);
    RUN_TEST(test_preview_short_or_null_slice_is_black);
    RUN_TEST(test_preview_wash_scales_by_intensity_and_master);
    RUN_TEST(test_preview_wash_zero_intensity_is_black);
    RUN_TEST(test_preview_raw_overrides_wash);
    RUN_TEST(test_preview_raw_min1_floor_matches_wire);
    return UNITY_END();
}
