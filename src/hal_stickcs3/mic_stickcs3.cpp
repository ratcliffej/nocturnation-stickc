#include "mic_stickcs3.h"
#include "M5Unified.h"
#include <Arduino.h>
#include <math.h>

extern "C" {
#include "dsps_fft2r.h"
}

namespace nocturnation {
namespace hal {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Generate a standard symmetric Hamming window. Matches arduinoFFT's
// FFTWindow::Hamming definition so band-energy values stay comparable to
// the Plus2 backend's output.
void generate_hamming_window(float* window, size_t n) {
    if (n < 2) return;
    for (size_t i = 0; i < n; ++i) {
        window[i] = 0.54f - 0.46f * cosf(2.0f * kPi * static_cast<float>(i)
                                          / static_cast<float>(n - 1));
    }
}

}  // namespace

void MicStickCS3::begin(uint16_t /*sample_rate_hz*/, uint16_t /*fft_size*/) {
    if (running_) return;

    // One-shot FFT init on first begin(). dsps_fft2r_init_fc32(NULL, N)
    // allocates the sin/cos coefficient table internally; subsequent
    // begin() / end() cycles reuse it.
    if (!fft_initialised_) {
        dsps_fft2r_init_fc32(nullptr, static_cast<int>(kFftSize));
        generate_hamming_window(hamming_window_, kFftSize);
        fft_initialised_ = true;
    }

    M5.Speaker.end();          // Match the Plus2 enable sequence; harmless on
                               // the S3 (separate AW8737 amp from the ES8311
                               // codec) but keeps cross-host behaviour aligned.
    M5.Mic.begin();
    running_ = true;
}

void MicStickCS3::end() {
    if (!running_) return;
    M5.Mic.end();
    running_ = false;
    // Speaker is left disabled, matching the Plus2 backend.
    // dsps_fft2r tables remain allocated for the next begin().
}

bool MicStickCS3::is_running() const {
    return running_;
}

void MicStickCS3::set_frame_callback(FrameCallback cb) {
    callback_ = cb;
}

void MicStickCS3::set_bass_band(uint16_t lo_hz, uint16_t hi_hz) {
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

void MicStickCS3::poll() {
    if (!running_)            return;
    if (!M5.Mic.isEnabled())  return;

    // record() blocks for ~32 ms at 16 kHz / 512.
    if (!M5.Mic.record(mic_buf_, kFftSize, kSampleRate)) return;

    // Mean absolute amplitude (overall_rms proxy used by orchestration).
    uint32_t sum = 0;
    for (size_t i = 0; i < kFftSize; ++i) sum += abs(mic_buf_[i]);
    const float overall_rms = (float)sum / (float)kFftSize;

    // Pack real samples into the interleaved complex buffer with windowing.
    // Layout: fft_buf_[2*i] = windowed real, fft_buf_[2*i + 1] = 0.
    for (size_t i = 0; i < kFftSize; ++i) {
        fft_buf_[2 * i]     = static_cast<float>(mic_buf_[i]) * hamming_window_[i];
        fft_buf_[2 * i + 1] = 0.0f;
    }

    // Forward complex FFT in place + bit-reversal to natural ordering.
    // We use the portable ANSI implementations (_ansi) rather than the
    // dsps_fft2r_fc32 macro that would route to _aes3 on the S3. Empirical:
    // the _aes3 PIE-vector path produced erratic per-bin magnitudes for
    // our packed-real-as-complex input layout (B/M/T bars saturating while
    // R, which is computed without the FFT, looked sane). The _ansi path
    // is still substantially faster than arduinoFFT thanks to the
    // float-vs-double type and esp-dsp's better cache behaviour; the
    // _aes3 perf win is recoverable as a follow-up once the layout/init
    // mismatch is understood.
    const uint32_t fft_t0 = micros();
    dsps_fft2r_fc32_ansi(fft_buf_, static_cast<int>(kFftSize));
    dsps_bit_rev_fc32_ansi(fft_buf_, static_cast<int>(kFftSize));
    last_fft_micros_ = micros() - fft_t0;

    // Magnitude per bin from interleaved (real, imag) pairs. We only need
    // bins 0..N/2-1; bins N/2..N-1 are mirror images for real input.
    auto magnitude = [&](size_t bin) -> float {
        const float re = fft_buf_[2 * bin];
        const float im = fft_buf_[2 * bin + 1];
        return sqrtf(re * re + im * im);
    };

    float bass_energy = 0.0f;
    for (uint16_t i = bass_bin_lo_; i <= bass_bin_hi_; ++i) {
        bass_energy += magnitude(i);
    }

    // Mid (~250-2000 Hz at 16 kHz/512: bins 8..64) and treble (bins 65..255)
    // band sums - reserved for future effects.
    float mid_energy = 0.0f;
    for (size_t i = 8; i <= 64 && i < kFftSize / 2; ++i) {
        mid_energy += magnitude(i);
    }
    float treble_energy = 0.0f;
    for (size_t i = 65; i < kFftSize / 2; ++i) {
        treble_energy += magnitude(i);
    }

    if (callback_) {
        AudioFrame frame{};
        frame.timestamp_ms  = millis();
        frame.bass_energy   = bass_energy;
        frame.mid_energy    = mid_energy;
        frame.treble_energy = treble_energy;
        frame.overall_rms   = overall_rms;
        callback_(frame);
    }
}

}  // namespace hal
}  // namespace nocturnation
