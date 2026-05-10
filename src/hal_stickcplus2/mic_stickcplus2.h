// M5StickC Plus2 Mic backend.
//
// Wraps M5.Mic (PDM mic via I2S) and arduinoFFT to emit raw audio analysis
// frames at a fixed cadence. Per the HAL contract this backend does NOT
// perform beat detection - that lives in the orchestration layer.
//
// Behaviour-preservation notes (matching the Epic 1 prototype's detectBeat):
//   - Sample rate hardcoded to 16 kHz, FFT size to 512 samples (~32 ms window).
//   - Bass band defaults to FFT bins 2..7 (the prototype's BASS_BIN_LO/HI).
//   - begin() calls M5.Speaker.end() before M5.Mic.begin() because the StickC
//     Plus2 shares mic/speaker hardware; end() does not re-enable the speaker
//     (matches the prototype's setBeatMode flow).

#pragma once

#include "hal/hal.h"
#include <arduinoFFT.h>

namespace nocturnation {
namespace hal {

class MicStickCplus2 : public Mic {
public:
    MicStickCplus2();

    void begin(uint16_t sample_rate_hz, uint16_t fft_size) override;
    void end() override;
    bool is_running() const override;
    void set_frame_callback(FrameCallback cb) override;
    void set_bass_band(uint16_t lo_hz, uint16_t hi_hz) override;

    // Audio pipeline operating-point API (Epic 4.5 Block 2). Plus2's
    // PDM mic and the I2S clock setup are fixed at 16 kHz; the backend
    // declares a single operating point and rejects any other.
    const AudioOperatingPoint* operating_points() const override;
    size_t                     operating_point_count() const override;
    AudioOperatingPoint        current_operating_point() const override;
    bool                       configure_audio_pipeline(uint32_t sample_rate_hz,
                                                         uint16_t fft_size) override;

    // Backend-specific - called from HAL::loop_tick() in hal_stickcplus2.cpp.
    void poll();

private:
    static constexpr size_t   kFftSize    = 512;
    static constexpr uint16_t kSampleRate = 16000;

    bool          running_       = false;
    FrameCallback callback_;

    // Bass-band bin range. Defaults to the prototype's bins 2..7 (~62..218 Hz
    // at 16 kHz / 512). set_bass_band() converts Hz to bin numbers.
    uint16_t bass_bin_lo_ = 2;
    uint16_t bass_bin_hi_ = 7;

    // Buffers and FFT object. Stable references for ArduinoFFT.
    int16_t            mic_buf_[kFftSize];
    double             v_real_ [kFftSize];
    double             v_imag_ [kFftSize];
    ArduinoFFT<double> fft_;
};

}  // namespace hal
}  // namespace nocturnation
