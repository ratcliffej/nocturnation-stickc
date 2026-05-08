// M5StickS3 Mic backend.
//
// Wraps M5.Mic (ES8311 stereo audio codec via I2S; data on GPIO 14/16/17,
// LRCK 15, MCLK 18; codec configured over I2C SDA 47 / SCL 48). M5Unified
// brings up the codec at runtime via its board_t::board_M5StickS3 path,
// so this backend's call sequence mirrors the Plus2's PDM-mic backend.
//
// The HAL contract is unchanged: this backend produces raw AudioFrame
// samples (band energies + RMS); beat detection is orchestration's job.
//
// First-pass FFT uses arduinoFFT for parity with the Plus2 backend. The
// esp-dsp upgrade (S3 vector / PIE instructions, ~5-10x faster) lands in
// a follow-up step within Block 2; the API stays the same.
//
// Behaviour-preservation notes:
//   - Sample rate hardcoded to 16 kHz, FFT size to 512 samples (~32 ms),
//     identical to the Plus2's prototype.
//   - Bass band defaults to FFT bins 2..7 (the prototype's BASS_BIN_LO/HI).
//   - begin() calls M5.Speaker.end() before M5.Mic.begin() to keep the
//     same enable sequence as the Plus2 backend; the S3's separate AW8737
//     speaker amplifier means this is harmless rather than load-bearing,
//     but matching the pattern simplifies cross-host reasoning.

#pragma once

#include "hal/hal.h"
#include <arduinoFFT.h>

namespace nocturnation {
namespace hal {

class MicStickCS3 : public Mic {
public:
    MicStickCS3();

    void begin(uint16_t sample_rate_hz, uint16_t fft_size) override;
    void end() override;
    bool is_running() const override;
    void set_frame_callback(FrameCallback cb) override;
    void set_bass_band(uint16_t lo_hz, uint16_t hi_hz) override;

    // Backend-specific - called from HAL::loop_tick() in hal_stickcs3.cpp.
    void poll();

private:
    static constexpr size_t   kFftSize    = 512;
    static constexpr uint16_t kSampleRate = 16000;

    bool          running_       = false;
    FrameCallback callback_;

    // Bass-band bin range. Defaults to bins 2..7 (~62..218 Hz at 16 kHz / 512).
    uint16_t bass_bin_lo_ = 2;
    uint16_t bass_bin_hi_ = 7;

    int16_t            mic_buf_[kFftSize];
    double             v_real_ [kFftSize];
    double             v_imag_ [kFftSize];
    ArduinoFFT<double> fft_;
};

}  // namespace hal
}  // namespace nocturnation
