// NocturNation Hardware Abstraction Layer (HAL)
//
// Interface contract per docs/hal-design.md. Each supported host (M5StickC
// Plus2, Tildagon, generic ESP32 dev kits, future microcontrollers) provides
// one backend that implements this contract. Layers above the HAL never call
// vendor SDKs directly.
//
// Each backend declares a flat capability list (the `Capability` enum below).
// Callers query `HAL::has(Capability::X)` or accept the nullptr from the
// per-capability accessor to adapt to what's available.
//
// Capabilities defined here whose backend implementations are not yet in
// place (IRRx, ESPNow on the Epic 2 StickC backend) ship with full interface
// stubs so future Epics plug in without contract churn.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace nocturnation {
namespace hal {

// =============================================================================
// Capability declaration
// =============================================================================

enum class Capability : uint8_t {
    Mic = 0,    // microphone samples + on-board FFT, emits AudioFrames
    IRTx,       // IR LED transmitter (raw pulse send)
    IRRx,       // IR receiver
    ESPNow,     // ESP-NOW broadcast/peer
    Display,    // framebuffer + drawing primitives
    Buttons,    // discrete buttons (Btn1..Btn8) with click/long-press semantics
    IMU,        // 3- or 6-axis accelerometer/gyro
    Battery,    // battery level + voltage + charge state
};

// =============================================================================
// Mic - audio analysis frames at a fixed cadence (raw FFT only, no beat detect)
// =============================================================================

struct AudioFrame {
    uint32_t timestamp_ms;   // millis() at frame end
    float    bass_energy;    // sum of FFT bins ~62-187 Hz
    float    mid_energy;     // ~250-2000 Hz
    float    treble_energy;  // ~2000-8000 Hz
    float    overall_rms;    // mean abs amplitude over the window
};

class Mic {
public:
    virtual ~Mic() = default;

    virtual void begin(uint16_t sample_rate_hz = 16000,
                       uint16_t fft_size = 512) = 0;
    virtual void end() = 0;
    virtual bool is_running() const = 0;

    // Frames fire synchronously from HAL::loop_tick().
    using FrameCallback = std::function<void(const AudioFrame&)>;
    virtual void set_frame_callback(FrameCallback cb) = 0;

    // Override default bass-band edges if needed.
    virtual void set_bass_band(uint16_t lo_hz, uint16_t hi_hz) = 0;
};

// =============================================================================
// IRTx - send raw IR pulse trains
// =============================================================================

class IRTx {
public:
    virtual ~IRTx() = default;

    virtual void begin() = 0;

    // pulses_us: alternating mark/space durations in microseconds.
    // count: number of entries.
    // carrier_khz: typically 38 for PixMob bracelets.
    virtual void send_raw(const uint16_t* pulses_us, size_t count,
                          uint16_t carrier_khz = 38) = 0;
};

// =============================================================================
// IRRx - receive raw IR pulse trains (stub; not declared by Epic 2 StickC)
// =============================================================================

struct IRPulses {
    uint32_t        timestamp_ms;
    const uint16_t* pulses_us;
    size_t          count;
};

class IRRx {
public:
    virtual ~IRRx() = default;

    virtual void begin() = 0;
    virtual void end() = 0;

    using PulsesCallback = std::function<void(const IRPulses&)>;
    virtual void set_pulses_callback(PulsesCallback cb) = 0;
};

// =============================================================================
// ESPNow - broadcast and peer messaging (stub; not declared by Epic 2 StickC)
// =============================================================================

struct ESPNowMessage {
    uint32_t       timestamp_ms;
    uint8_t        peer_mac[6];
    const uint8_t* data;
    size_t         len;
    int8_t         rssi;     // -128 if not available
};

class ESPNow {
public:
    virtual ~ESPNow() = default;

    virtual bool begin(uint8_t wifi_channel) = 0;
    virtual void end() = 0;

    virtual bool send_broadcast(const uint8_t* data, size_t len) = 0;
    virtual bool send_to(const uint8_t mac[6], const uint8_t* data, size_t len) = 0;

    using RecvCallback = std::function<void(const ESPNowMessage&)>;
    virtual void set_recv_callback(RecvCallback cb) = 0;
};

// =============================================================================
// Display - drawing primitives (RGB565 colour space)
// =============================================================================

class Display {
public:
    virtual ~Display() = default;

    virtual void begin() = 0;
    virtual void set_rotation(uint8_t rotation) = 0;     // 0-3
    virtual int  width() const = 0;
    virtual int  height() const = 0;

    virtual void clear(uint16_t color = 0x0000) = 0;
    virtual void fill_rect(int x, int y, int w, int h, uint16_t color) = 0;
    virtual void draw_rect(int x, int y, int w, int h, uint16_t color) = 0;
    virtual void draw_hline(int x, int y, int w, uint16_t color) = 0;
    virtual void draw_vline(int x, int y, int h, uint16_t color) = 0;

    virtual void set_text_color(uint16_t fg, uint16_t bg) = 0;
    virtual void set_text_size(uint8_t size) = 0;
    virtual void draw_text(int x, int y, const char* text) = 0;

    // No-op on direct backends; meaningful on double-buffered ones.
    virtual void flush() = 0;
};

// =============================================================================
// Buttons - generic numeric IDs (Btn1 = primary/main button on every host)
// =============================================================================

enum class ButtonId : uint8_t {
    Btn1 = 1,   // primary/main; always present if Buttons capability is declared
    Btn2 = 2,
    Btn3 = 3,
    Btn4 = 4,
    Btn5 = 5,
    Btn6 = 6,
    Btn7 = 7,
    Btn8 = 8,
};

enum class ButtonEvent : uint8_t {
    Pressed,        // debounced press edge
    Released,       // debounced release edge
    Clicked,        // short press + release
    DoubleClicked,  // optional - backend may not support
    LongPressed,    // held past long-press threshold
};

class Buttons {
public:
    virtual ~Buttons() = default;

    virtual void begin() = 0;

    // How many buttons this backend exposes (1..8).
    virtual uint8_t count() const = 0;

    // Events fire synchronously from HAL::loop_tick().
    using ButtonCallback = std::function<void(ButtonId, ButtonEvent)>;
    virtual void set_callback(ButtonCallback cb) = 0;

    virtual bool is_pressed(ButtonId id) = 0;

    virtual void set_long_press_ms(uint16_t ms) = 0;
};

// =============================================================================
// IMU - accelerometer (and optionally gyro)
// =============================================================================

struct IMUSample {
    uint32_t timestamp_ms;
    float    ax, ay, az;     // m/s^2
    float    gx, gy, gz;     // rad/s, or 0 on accelerometer-only devices
    bool     has_gyro;
};

class IMU {
public:
    virtual ~IMU() = default;

    virtual void begin() = 0;
    virtual bool read(IMUSample& out) = 0;     // false if no fresh sample
};

// =============================================================================
// Battery
// =============================================================================

class Battery {
public:
    virtual ~Battery() = default;

    virtual int   level_percent() = 0;     // 0-100, or -1 if not known
    virtual float voltage() = 0;           // volts, or 0 if not measurable
    virtual bool  is_charging() = 0;
};

// =============================================================================
// HAL - global accessor and lifecycle
// =============================================================================

class HAL {
public:
    // Capability discovery. The list is backend-specific and compile-time fixed.
    static const Capability* capabilities();
    static size_t            capability_count();
    static bool              has(Capability c);

    // Lifecycle. Call begin() once in setup(); loop_tick() once per loop().
    static void begin();
    static void loop_tick();

    // Per-capability accessors. Return nullptr if the capability is not
    // declared by this backend.
    static Mic*      mic();
    static IRTx*     ir_tx();
    static IRRx*     ir_rx();
    static ESPNow*   esp_now();
    static Display*  display();
    static Buttons*  buttons();
    static IMU*      imu();
    static Battery*  battery();
};

}  // namespace hal
}  // namespace nocturnation
