#include "mic_stickcplus2.h"
#include "dal/analyser/audio_analyser.h"
#include "M5Unified.h"
#include <Arduino.h>
#include <math.h>

namespace nocturnation {
namespace hal {

MicStickCplus2::MicStickCplus2()
    : fft_(v_real_, v_imag_, kFftSize, (double)kSampleRate) {}

void MicStickCplus2::begin(uint16_t /*sample_rate_hz*/, uint16_t /*fft_size*/) {
    if (running_) return;
    // Sample rate / FFT size are fixed in this backend (see header). The
    // prototype's detectBeat hardcoded these values; honouring requested
    // overrides is a future refinement.
    M5.Speaker.end();          // shared mic/speaker hardware on the Plus2
    M5.Mic.begin();
    running_ = true;
}

void MicStickCplus2::end() {
    if (!running_) return;
    M5.Mic.end();
    running_ = false;
    // Speaker is left disabled, matching the prototype's setBeatMode flow.
}

bool MicStickCplus2::is_running() const {
    return running_;
}

void MicStickCplus2::set_frame_callback(FrameCallback cb) {
    callback_ = cb;
}

void MicStickCplus2::set_bass_band(uint16_t lo_hz, uint16_t hi_hz) {
    // Convert Hz to bin numbers: bin = freq * fft_size / sample_rate.
    // Round to nearest, clamp to [0, fft_size/2 - 1].
    auto hz_to_bin = [](uint16_t hz) -> uint16_t {
        const long bin = ((long)hz * (long)kFftSize + (long)kSampleRate / 2)
                         / (long)kSampleRate;
        if (bin < 0)                              return 0;
        if (bin > (long)kFftSize / 2 - 1)         return kFftSize / 2 - 1;
        return (uint16_t)bin;
    };
    uint16_t lo = hz_to_bin(lo_hz);
    uint16_t hi = hz_to_bin(hi_hz);
    if (hi < lo) hi = lo;
    bass_bin_lo_ = lo;
    bass_bin_hi_ = hi;
}

// Plus2's audio pipeline operating points. PDM mic + I2S clock setup
// is fixed at 16 kHz; the backend declares one option and rejects any
// other. Future work could add a 32 kHz path if PDM throughput allows,
// but that's not in Epic 4.5's scope.
namespace {
constexpr AudioOperatingPoint kPlus2OperatingPoints[] = {
    { /*sample_rate_hz=*/16000, /*fft_size=*/512 },
};
constexpr size_t kPlus2OperatingPointCount =
    sizeof(kPlus2OperatingPoints) / sizeof(kPlus2OperatingPoints[0]);
}  // namespace

const AudioOperatingPoint* MicStickCplus2::operating_points() const {
    return kPlus2OperatingPoints;
}

size_t MicStickCplus2::operating_point_count() const {
    return kPlus2OperatingPointCount;
}

AudioOperatingPoint MicStickCplus2::current_operating_point() const {
    // Backend is hard-fixed at the canonical default; return that.
    return kPlus2OperatingPoints[0];
}

bool MicStickCplus2::configure_audio_pipeline(uint32_t sample_rate_hz,
                                               uint16_t fft_size) {
    // Only the canonical default is implemented in Epic 4.5; reject
    // anything else explicitly so future Epics see a clean integration
    // point.
    return sample_rate_hz == kSampleRate && fft_size == kFftSize;
}

void MicStickCplus2::poll() {
    if (!running_)            return;
    if (!M5.Mic.isEnabled())  return;

    // record() is blocking (~32 ms at 16 kHz / 512). This is the same cost
    // the prototype's detectBeat paid; it dominates loop() timing while in
    // beat mode but does not affect idle-mode loop responsiveness.
    if (!M5.Mic.record(mic_buf_, kFftSize, kSampleRate)) return;

    // Mean absolute amplitude (rough volume proxy, used by orchestration's
    // volume gate).
    uint32_t sum = 0;
    for (size_t i = 0; i < kFftSize; ++i) sum += abs(mic_buf_[i]);
    const float overall_rms = (float)sum / (float)kFftSize;

    // Real-valued FFT input.
    for (size_t i = 0; i < kFftSize; ++i) {
        v_real_[i] = (double)mic_buf_[i];
        v_imag_[i] = 0.0;
    }
    fft_.windowing(FFTWindow::Hamming, FFTDirection::Forward);
    fft_.compute(FFTDirection::Forward);
    fft_.complexToMagnitude();

    // Convert arduinoFFT's double-precision magnitudes (in v_real_)
    // into a float buffer the DAL analyser core consumes. The analyser
    // operates on floats throughout so the same pure function path
    // serves Plus2 here, S3 (which already runs floats), and any
    // future host backend.
    float magnitudes[kFftSize / 2];
    for (size_t i = 0; i < kFftSize / 2; ++i) {
        magnitudes[i] = static_cast<float>(v_real_[i]);
    }

    using namespace nocturnation::dal::analyser;
    BandSummary8  b8{};
    BandSummary3  b3{};
    SpectrumFrame sf{};
    compute_band_summaries(magnitudes, kFftSize / 2, kSampleRate, b8, b3);
    compute_spectrum_frame(magnitudes, kFftSize / 2, kSampleRate, sf);

    if (callback_) {
        AudioFrame frame{};
        frame.timestamp_ms  = millis();

        // 3-band B/M/T (restandardised to evidence-based ranges by the
        // analyser core; Bass <250 Hz, Mid 250-2000 Hz, Treble 2-Nyquist Hz).
        frame.bass_energy   = b3.bass;
        frame.mid_energy    = b3.mid;
        frame.treble_energy = b3.treble;

        // 8-band perceptual summary (Audible Genius reference).
        frame.mud           = b8.mud;
        frame.sub_bass      = b8.sub_bass;
        frame.bass          = b8.bass;
        frame.low_mids      = b8.low_mids;
        frame.midrange      = b8.midrange;
        frame.high_mids     = b8.high_mids;
        frame.presence      = b8.presence;
        frame.air           = b8.air;

        // 32-band log-spaced spectrum.
        for (size_t i = 0; i < AudioFrame::kSpectrumBands; ++i) {
            frame.spectrum[i] = sf.magnitudes[i];
        }

        frame.overall_rms   = overall_rms;
        callback_(frame);
    }
}

}  // namespace hal
}  // namespace nocturnation
