// NocturNation Hardware Abstraction Layer (HAL)
//
// Interface contract per the HAL design notes (nocturnation-docs repo).
// Each supported host (M5StickC
// Plus2, Tildagon, generic ESP32 dev kits, future microcontrollers) provides
// one backend that implements this contract. Layers above the HAL never call
// vendor SDKs directly.
//
// Each backend declares a flat capability list (the `Capability` enum below).
// Callers query `HAL::has(Capability::X)` or accept the nullptr from the
// per-capability accessor to adapt to what's available.
//
// Capabilities defined here whose backend implementations are not yet in
// place (IRRx, ESPNow on the StickC Plus2 backend) ship with full interface
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
    Bluetooth,  // BLE peripheral (declaration-only at present; future Epic
                // wires phone-app pairing on top - StickC Plus2 has BLE 4.2,
                // StickS3 has BLE 5.0)

    // -------------------------------------------------------------------------
    // Audio analyser sub-capabilities (Epic 4.5)
    // -------------------------------------------------------------------------
    //
    // Sub-capabilities that compose what an analyser produces. A host
    // with Mic + an analyser implementation declares the subset of
    // these that its current operating point actually supports.
    // Lumes consume events over the wire and need none of these
    // declared - the analyser is Director-side only.
    //
    // Lit by Epic 4.5 (this Epic):
    AnalyserBeatDetection,    // produces BEAT_DETECTED events (bass-band onset)
    AnalyserDropDetection,    // produces MUSIC_EVENT (DROP / BREAKDOWN) events
    AnalyserSpectrumFrame,    // emits 32-band log-spaced SpectrumFrameEvent
    AnalyserBandSummary,      // emits 3-band B/M/T + 8-band perceptual summary
    // -------------------------------------------------------------------------
    // RESERVED for Epic 4.7. **No host should declare any of these today.**
    //
    // These four enum constants are placeholders for analyser sub-capabilities
    // that Epic 4.7 will introduce. The analysers that *produce* the
    // corresponding events do not yet exist anywhere in the codebase, so
    // declaring one of these in a HAL backend's capability set today is
    // meaningless: the framework will believe the capability is available, but
    // no data will ever flow. Worse, a future visualisation whose PowerProfile
    // requires one of these capabilities will appear enabled in the picker yet
    // silently never get the data it asks for.
    //
    // They are listed here (rather than added in Epic 4.7) only so the enum
    // numbering stays stable: visualisations and tests can reference the names
    // today without recompiling the whole tree when Epic 4.7 lands the
    // analysers that finally back them.
    // -------------------------------------------------------------------------
    AnalyserMultiBandOnset,   // SNARE/HIHAT events alongside BEAT_DETECTED
    AnalyserSpectralCentroid, // continuous centroid descriptor
    AnalyserEnergyEnvelope,   // continuous smoothed-RMS descriptor
    AnalyserSectionDetection, // SECTION_CHANGE events (verse/chorus/bridge)

    // -------------------------------------------------------------------------
    // IMU sub-capabilities (Epic 6B)
    // -------------------------------------------------------------------------
    //
    // Compose what an IMU backend produces. A host with Capability::IMU
    // declared also declares the subset of these its IMU driver fires
    // events for. Tap is the highest-value sub-capability for Director-
    // mode tap-to-beat; Motion supports gesture-driven shows.
    //
    // No M5 backend currently produces these events; the flags are
    // reserved-but-unwired (same posture as the Epic-4.7-reserved
    // analyser sub-flags above) so Shows that consume them can declare
    // a stable required_capabilities() mask in the cross-platform
    // shape. Tildagon (where the IMU adapter does fire these events)
    // declares them via its own HAL.
    ImuTap,                   // produces TapDetectedEvent (high-pass-filtered Z onset)
    ImuMotion,                // produces MotionEvent (3-axis magnitude / per-axis)

    // -------------------------------------------------------------------------
    // DMX input (Epic 7)
    // -------------------------------------------------------------------------
    //
    // The host has a wire over which a DMX console can deliver Enttec
    // DMX USB Pro framing. v1 (StickC Plus2 + S3) means USB-CDC at
    // 921 600 baud. A future Tildagon backend (sACN over WiFi) declares
    // the same capability once the radio-coexistence work lands.
    //
    // Modes / Shows that require DMX input declare this capability and
    // ModeMachine refuses to enter them on a host that doesn't have it.
    DmxInput,                 // host can receive Enttec Pro DMX over its primary serial wire
};

// =============================================================================
// Mic - audio analysis frames at a fixed cadence (raw FFT + analyser surface)
// =============================================================================

// Per-FFT-cycle output of the host's audio analyser. Carries the
// 3-band B/M/T summary (existing API, restandardised in Epic 4.5 to
// evidence-based ranges), the 8-band perceptual summary (Audible
// Genius reference - new in Epic 4.5), and the 32-band log-spaced
// spectrum (new in Epic 4.5; consumed by Diagnostic UI in 4.6 and FX
// modulators in 4.7).
//
// All fields default-initialise to 0.0f so a backend that doesn't fill
// the new surface yet (e.g. before Block 2c lights up the integration)
// returns sane zeros rather than uninitialised garbage. Existing
// consumers reading bass_energy / mid_energy / treble_energy continue
// to work; new consumers can opt into the richer surface.
struct AudioFrame {
    uint32_t timestamp_ms = 0;        // millis() at frame end

    // 3-band B/M/T summary. Boundaries restandardised in Epic 4.5 to
    // perceptual splits taken from the Audible Genius reference.
    float    bass_energy   = 0.0f;    // [0, 250) Hz   (was ~62-187)
    float    mid_energy    = 0.0f;    // [250, 2000) Hz
    float    treble_energy = 0.0f;    // [2000, Nyquist) Hz

    // 8-band perceptual summary. Internally consistent with the 3-band
    // roll-up: bass_energy = mud + sub_bass + bass; mid_energy =
    // low_mids + midrange; treble_energy = high_mids + presence + air.
    float    mud           = 0.0f;    // [0, 20) Hz
    float    sub_bass      = 0.0f;    // [20, 60) Hz
    float    bass          = 0.0f;    // [60, 250) Hz
    float    low_mids      = 0.0f;    // [250, 500) Hz
    float    midrange      = 0.0f;    // [500, 2000) Hz
    float    high_mids     = 0.0f;    // [2000, 4000) Hz
    float    presence      = 0.0f;    // [4000, 6000) Hz
    float    air           = 0.0f;    // [6000, 20000) Hz, truncated at Nyquist

    // 32-band log-spaced spectrum covering [30 Hz, Nyquist). Director-
    // local; never broadcast over ESP-NOW (too heavy at FFT rate). At
    // the canonical 16 kHz / 512 FFT operating point this spans 30 Hz
    // to 8 kHz; at higher operating points the upper edge follows
    // Nyquist.
    static constexpr size_t kSpectrumBands = 32;
    float    spectrum[kSpectrumBands] = {};

    float    overall_rms   = 0.0f;    // mean abs amplitude over the window
};

// A valid (sample_rate_hz, fft_size) tuple the host's audio pipeline
// supports. Hosts declare a static list of these; orchestration picks
// one via configure_audio_pipeline(). The first entry in a host's list
// is its default operating point.
struct AudioOperatingPoint {
    uint32_t sample_rate_hz;
    uint16_t fft_size;
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

    // -------------------------------------------------------------------------
    // Audio pipeline operating points (Epic 4.5)
    // -------------------------------------------------------------------------
    //
    // Each backend declares a const static list of valid operating
    // points it can run at, in priority order with the default first.
    // Plus2 declares one (codec-limited); S3 declares several (capable
    // of higher sample rates and larger FFTs); future phone or PC HALs
    // declare their own.
    //
    // configure_audio_pipeline() asks the backend to switch to one of
    // its declared points. **In Epic 4.5 only the canonical default
    // (16000, 512) is implemented across all hosts**; any other tuple
    // returns false (explicit not-supported). Future Epics light up
    // the additional declared operating points on capable hosts.
    virtual const AudioOperatingPoint* operating_points() const = 0;
    virtual size_t                     operating_point_count() const = 0;
    virtual AudioOperatingPoint        current_operating_point() const = 0;
    virtual bool                       configure_audio_pipeline(uint32_t sample_rate_hz,
                                                                 uint16_t fft_size) = 0;
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
// IRRx - receive raw IR pulse trains (stub; not declared by the StickC Plus2 backend)
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
// ESPNow - broadcast and peer messaging (stub; not declared by the StickC Plus2 backend)
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

    // Switch the radio to a different WiFi channel without tearing down
    // ESP-NOW. Required for Lume dual-channel scanning per spec §4.5.
    // Returns false if the radio isn't running or the channel switch fails.
    virtual bool set_channel(uint8_t wifi_channel) = 0;

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

    // -------------------------------------------------------------------------
    // Buffered paint sessions
    // -------------------------------------------------------------------------
    //
    // begin_buffered_paint() asks the backend to allocate an off-screen
    // sprite covering the (x, y, w, h) screen-space rectangle. All
    // subsequent draw operations (fill_rect, draw_rect, draw_text, etc.)
    // route into the sprite using the same screen-space coordinates -
    // the backend translates internally. end_buffered_paint() pushes
    // the sprite to the panel as a single SPI burst and releases the
    // sprite. Use case: batching many small draws (status strip with
    // multiple icons + text) into one transfer to the panel, both
    // reducing SPI overhead and shrinking the window for scan-out
    // tearing.
    //
    // Backends that don't support buffering (or where the allocation
    // failed - e.g. RAM tight) silently route draws directly to the
    // panel; the begin call returns false in that case but draws still
    // work. Nesting is not supported - calling begin twice without an
    // intervening end ends the previous session first.
    //
    // Note: this won't eliminate tearing on smooth fades of a single
    // solid-colour fill_rect, since that's already a one-shot SPI
    // burst racing the panel scan. The win is for multi-element
    // updates (the status strip's ~13 fill_rect + text ops collapse
    // into one push).
    virtual bool begin_buffered_paint(int x, int y, int w, int h) = 0;
    virtual void end_buffered_paint() = 0;
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
    // Optional second IR transmitter for an external emitter (e.g. an
    // M5Stack IR unit on the Plus2's GPIO 26 header pin). Returns nullptr
    // on backends with no external transmitter, so callers must null-check.
    static IRTx*     ir_tx_ext();
    static IRRx*     ir_rx();
    static ESPNow*   esp_now();
    static Display*  display();
    static Buttons*  buttons();
    static IMU*      imu();
    static Battery*  battery();
};

}  // namespace hal
}  // namespace nocturnation
