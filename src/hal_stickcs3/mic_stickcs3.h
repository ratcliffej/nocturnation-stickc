// M5StickS3 Mic backend.
//
// Wraps M5.Mic (ES8311 stereo audio codec via I2S; data on GPIO 14/16/17,
// LRCK 15, MCLK 18; codec configured over I2C SDA 47 / SCL 48). M5Unified
// brings up the codec at runtime via its board_t::board_M5StickS3 path,
// so this backend's M5.Mic.begin() / record() call sequence mirrors the
// Plus2's PDM-mic backend.
//
// FFT path uses Espressif's esp-dsp library (radix-2 complex FFT,
// dsps_fft2r_fc32) which dispatches to the ESP32-S3's PIE vector
// instructions when CONFIG_DSP_OPTIMIZED is on - roughly 5-10x the
// throughput of the scalar arduinoFFT path on the same chip. esp-dsp's
// pre-built objects ship inside the arduino-esp32 SDK bundle
// (tools/sdk/esp32s3/lib/libespressif__esp-dsp.a), so no separate
// PlatformIO library install is needed; the linker just finds the
// symbols.
//
// HAL contract is unchanged: this backend produces raw AudioFrame
// samples (band energies + RMS); beat detection is orchestration's job.
// The Plus2 backend (MicStickCplus2) keeps using arduinoFFT - the
// per-host choice is HAL-local and orchestration above is unaware.
//
// Behaviour-preservation notes:
//   - Sample rate hardcoded to 16 kHz, FFT size to 512 samples (~32 ms),
//     identical to the Plus2's prototype.
//   - Bass band defaults to FFT bins 2..7 (the prototype's BASS_BIN_LO/HI).
//   - Hamming window precomputed once. esp-dsp ships Hann / Blackman /
//     Nuttall but no Hamming; we generate the coefficients ourselves so
//     the band energies match the Plus2's arduinoFFT path.
//   - begin() calls M5.Speaker.end() before M5.Mic.begin() to keep the
//     same enable sequence as the Plus2 backend.

#pragma once

#include "hal/hal.h"

namespace nocturnation {
namespace hal {

class MicStickCS3 : public Mic {
public:
    void begin(uint16_t sample_rate_hz, uint16_t fft_size) override;
    void end() override;
    bool is_running() const override;
    void set_frame_callback(FrameCallback cb) override;
    void set_bass_band(uint16_t lo_hz, uint16_t hi_hz) override;

    // Backend-specific - called from HAL::loop_tick() in hal_stickcs3.cpp.
    void poll();

    // Most recent FFT cycle time in microseconds (covers windowing +
    // forward FFT + bit-reverse, excluding mic record() and band sums).
    // Read-only convenience for instrumentation; 0 until the first frame.
    uint32_t last_fft_micros() const { return last_fft_micros_; }

private:
    static constexpr size_t   kFftSize    = 512;
    static constexpr uint16_t kSampleRate = 16000;

    bool          running_       = false;
    bool          fft_initialised_ = false;
    FrameCallback callback_;

    // Bass-band bin range. Defaults to bins 2..7 (~62..218 Hz at 16 kHz / 512).
    uint16_t bass_bin_lo_ = 2;
    uint16_t bass_bin_hi_ = 7;

    // Sample buffer (i16 from M5.Mic.record()), and esp-dsp working buffer.
    // fft_buf_ stores 2*N interleaved complex floats (real, imag, real, ...)
    // because dsps_fft2r_fc32 is a complex FFT; we pack our real input as
    // the real component with zero imag, identical to the arduinoFFT path
    // on the Plus2.
    int16_t mic_buf_[kFftSize];
    float   fft_buf_[2 * kFftSize];
    float   hamming_window_[kFftSize];

    uint32_t last_fft_micros_ = 0;
};

}  // namespace hal
}  // namespace nocturnation
