#include "mic_stickc.h"
#include "M5Unified.h"
#include <Arduino.h>
#include <math.h>

namespace nocturnation {
namespace hal {

MicStickC::MicStickC()
    : fft_(v_real_, v_imag_, kFftSize, (double)kSampleRate) {}

void MicStickC::begin(uint16_t /*sample_rate_hz*/, uint16_t /*fft_size*/) {
    if (running_) return;
    // Sample rate / FFT size are fixed in this backend (see header). The
    // prototype's detectBeat hardcoded these values; honouring requested
    // overrides is a future refinement.
    M5.Speaker.end();          // shared mic/speaker hardware on the Plus2
    M5.Mic.begin();
    running_ = true;
}

void MicStickC::end() {
    if (!running_) return;
    M5.Mic.end();
    running_ = false;
    // Speaker is left disabled, matching the prototype's setBeatMode flow.
}

bool MicStickC::is_running() const {
    return running_;
}

void MicStickC::set_frame_callback(FrameCallback cb) {
    callback_ = cb;
}

void MicStickC::set_bass_band(uint16_t lo_hz, uint16_t hi_hz) {
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

void MicStickC::poll() {
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

    // Bass band sum (matches the Epic 1 prototype's bins 2..7).
    float bass_energy = 0.0f;
    for (uint16_t i = bass_bin_lo_; i <= bass_bin_hi_; ++i) {
        bass_energy += (float)v_real_[i];
    }

    // Mid (~250-2000 Hz at 16 kHz/512: bins 8..64) and treble (bins 65..255)
    // band sums - reserved for future effects, not used by the current
    // beat-detection logic.
    float mid_energy = 0.0f;
    for (size_t i = 8; i <= 64 && i < kFftSize / 2; ++i) {
        mid_energy += (float)v_real_[i];
    }
    float treble_energy = 0.0f;
    for (size_t i = 65; i < kFftSize / 2; ++i) {
        treble_energy += (float)v_real_[i];
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
