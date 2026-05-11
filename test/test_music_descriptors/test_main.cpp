// Native unit tests for MusicDescriptors (Epic 4.7 Block 3).
//
// Strategy: feed the descriptor synthetic spectrum frames + RMS values
// + event flags and assert that centroid, energy, and density behave
// as expected. No FFT or HAL dependency - the descriptors operate on
// the analyser's SpectrumFrame and a scalar overall_rms.

#include "dal/analyser/music_descriptors.h"

#include <unity.h>

#include <cstring>

using namespace nocturnation::dal::analyser;

namespace {

void zero_spec(SpectrumFrame& f) {
    for (size_t i = 0; i < kSpectrumBands; ++i) f.magnitudes[i] = 0.0f;
}

// Helpers to build spectra weighted toward a frequency region.
void bass_heavy_spec(SpectrumFrame& f) {
    zero_spec(f);
    for (size_t i = 0; i < 8; ++i) f.magnitudes[i] = 100.0f;
}

void treble_heavy_spec(SpectrumFrame& f) {
    zero_spec(f);
    for (size_t i = 24; i < kSpectrumBands; ++i) f.magnitudes[i] = 100.0f;
}

void uniform_spec(SpectrumFrame& f, float v) {
    for (size_t i = 0; i < kSpectrumBands; ++i) f.magnitudes[i] = v;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Centroid: low for bass-heavy, high for treble-heavy, mid for uniform.
// ---------------------------------------------------------------------------

static void test_centroid_silence_reads_zero(void) {
    MusicDescriptors md;
    SpectrumFrame spec;
    zero_spec(spec);
    md.process(spec, 0.0f, /*any_event=*/false, /*now=*/0);
    TEST_ASSERT_EQUAL_UINT8(0, md.centroid());
}

static void test_centroid_bass_heavy_reads_low(void) {
    MusicDescriptors md;
    SpectrumFrame spec;
    bass_heavy_spec(spec);
    md.process(spec, 1000.0f, /*any_event=*/false, /*now=*/0);
    // Bands 0-7 dominate; centroid ~= (avg index 3.5) / 31 * 255 ~= 28.
    // Allow some slack for floor inclusion - we expect "low" (< 128).
    TEST_ASSERT_LESS_THAN_UINT8(128, md.centroid());
}

static void test_centroid_treble_heavy_reads_high(void) {
    MusicDescriptors md;
    SpectrumFrame spec;
    treble_heavy_spec(spec);
    md.process(spec, 1000.0f, /*any_event=*/false, /*now=*/0);
    // Bands 24-31 dominate; centroid ~= avg(24..31)/31 * 255 ~= 222.
    TEST_ASSERT_GREATER_THAN_UINT8(128, md.centroid());
}

static void test_centroid_uniform_reads_mid(void) {
    MusicDescriptors md;
    SpectrumFrame spec;
    uniform_spec(spec, 50.0f);
    md.process(spec, 1000.0f, /*any_event=*/false, /*now=*/0);
    // All bands equal; centroid = avg(0..31) / 31 * 255 = 15.5/31 * 255 = 127.
    TEST_ASSERT_INT_WITHIN(5, 127, static_cast<int>(md.centroid()));
}

// ---------------------------------------------------------------------------
// Energy: smoothed RMS, log-normalised. Below floor reads 0; well above
// the span saturates near 255.
// ---------------------------------------------------------------------------

static void test_energy_silence_reads_zero(void) {
    MusicDescriptors md;
    SpectrumFrame spec;
    zero_spec(spec);
    for (uint32_t t = 0; t < 20; ++t) {
        md.process(spec, 0.0f, false, t * 25);
    }
    TEST_ASSERT_EQUAL_UINT8(0, md.energy());
}

static void test_energy_below_floor_reads_zero(void) {
    MusicDescriptors md;
    SpectrumFrame spec;
    zero_spec(spec);
    // RMS=300 < kEnergyLog2Floor (log2(512) = 9), so log2(300) ~= 8.23 < 9
    // and the descriptor should read 0 once smoothed.
    for (uint32_t t = 0; t < 100; ++t) {
        md.process(spec, 300.0f, false, t * 25);
    }
    TEST_ASSERT_EQUAL_UINT8(0, md.energy());
}

static void test_energy_rises_with_rms(void) {
    MusicDescriptors md;
    SpectrumFrame spec;
    zero_spec(spec);

    // Run a steady ~mid RMS until the smoothing settles.
    for (uint32_t t = 0; t < 200; ++t) {
        md.process(spec, 4000.0f, false, t * 25);
    }
    const uint8_t mid_energy = md.energy();
    TEST_ASSERT_GREATER_THAN_UINT8(0, mid_energy);

    // Switch to louder steady RMS; energy must rise.
    md.reset();
    for (uint32_t t = 0; t < 200; ++t) {
        md.process(spec, 15000.0f, false, t * 25);
    }
    const uint8_t loud_energy = md.energy();
    TEST_ASSERT_GREATER_THAN_UINT8(mid_energy, loud_energy);
}

// ---------------------------------------------------------------------------
// Density: events-per-second windowed over kDensityWindowMs.
// ---------------------------------------------------------------------------

static void test_density_no_events_reads_zero(void) {
    MusicDescriptors md;
    SpectrumFrame spec;
    zero_spec(spec);
    for (uint32_t t = 0; t < 200; ++t) {
        md.process(spec, 1000.0f, /*any_event=*/false, t * 25);
    }
    TEST_ASSERT_EQUAL_UINT8(0, md.density());
}

static void test_density_rises_with_event_rate(void) {
    MusicDescriptors md;
    SpectrumFrame spec;
    zero_spec(spec);

    // Fire one event every 250 ms over 2 s = ~4 events / s; with
    // kDensityMaxEvents = 16, that's ~64 / 255 of the scale.
    for (uint32_t t = 0; t <= 2000; t += 25) {
        const bool ev = (t % 250 == 0) && (t > 0);
        md.process(spec, 1000.0f, ev, t);
    }
    const uint8_t slow_density = md.density();

    // Fire one event every 100 ms = 10 events / s.
    md.reset();
    for (uint32_t t = 0; t <= 2000; t += 25) {
        const bool ev = (t % 100 == 0) && (t > 0);
        md.process(spec, 1000.0f, ev, t);
    }
    const uint8_t fast_density = md.density();

    TEST_ASSERT_GREATER_THAN_UINT8(slow_density, fast_density);
}

static void test_density_decays_when_events_stop(void) {
    MusicDescriptors md;
    SpectrumFrame spec;
    zero_spec(spec);

    // Burst of events in the first second.
    for (uint32_t t = 0; t <= 1000; t += 25) {
        const bool ev = (t % 100 == 0) && (t > 0);
        md.process(spec, 1000.0f, ev, t);
    }
    const uint8_t during = md.density();
    TEST_ASSERT_GREATER_THAN_UINT8(0, during);

    // Two more seconds with no events - all entries fall out of the
    // 1 s window.
    for (uint32_t t = 1025; t <= 3000; t += 25) {
        md.process(spec, 1000.0f, false, t);
    }
    TEST_ASSERT_EQUAL_UINT8(0, md.density());
}

// ---------------------------------------------------------------------------
// Reset clears all internal state.
// ---------------------------------------------------------------------------

static void test_reset_clears_everything(void) {
    MusicDescriptors md;
    SpectrumFrame spec;
    bass_heavy_spec(spec);
    for (uint32_t t = 0; t < 100; ++t) {
        md.process(spec, 5000.0f, /*any_event=*/(t % 4 == 0), t * 25);
    }

    md.reset();
    TEST_ASSERT_EQUAL_UINT8(0, md.centroid());
    TEST_ASSERT_EQUAL_UINT8(0, md.energy());
    TEST_ASSERT_EQUAL_UINT8(0, md.density());
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_centroid_silence_reads_zero);
    RUN_TEST(test_centroid_bass_heavy_reads_low);
    RUN_TEST(test_centroid_treble_heavy_reads_high);
    RUN_TEST(test_centroid_uniform_reads_mid);
    RUN_TEST(test_energy_silence_reads_zero);
    RUN_TEST(test_energy_below_floor_reads_zero);
    RUN_TEST(test_energy_rises_with_rms);
    RUN_TEST(test_density_no_events_reads_zero);
    RUN_TEST(test_density_rises_with_event_rate);
    RUN_TEST(test_density_decays_when_events_stop);
    RUN_TEST(test_reset_clears_everything);

    return UNITY_END();
}
