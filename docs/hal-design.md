---
title: NocturNation HAL design
status: cross-project (interface contract is shared; backend specifics are per-host)
notion_url: null
notion_id: null
last_synced: 2026-05-07
sync_direction: not synced (design doc, lives in repo only for now)
---

# NocturNation HAL design

This document defines the interface contract for the Hardware Abstraction Layer (HAL). Each supported host (M5StickC Plus2, Tildagon, generic ESP32 dev kits, future microcontrollers) provides one HAL backend that implements this contract. Layers above the HAL never call vendor SDKs directly.

The interface lands in Epic 2 of the architecture refactor. The first concrete backend - M5StickC Plus2 - lands in the same Epic. ESP-NOW interface stubs are defined here, but the StickC backend does not declare the `esp-now` capability until Epic 4 ships the actual implementation.

---

## 1. Design goals

- **One contract, many backends.** Every supported host implements the same interface. Code above the HAL is identical across hosts; only the backend changes.
- **Capabilities, not assumptions.** A backend declares what it offers. Layers above adapt to the declared set rather than assuming a fixed feature surface.
- **Minimal interfaces, deep implementations.** The HAL exposes the smallest reasonable interface for each peripheral. Complexity belongs in implementations and in the layers above, not in the contract.
- **Single-threaded, loop-driven, no surprises.** All callbacks fire synchronously from a `poll()` call. No interrupts leaking into application code. No hidden tasks. The Arduino mental model holds.
- **Forward-compatible.** Interface stubs for not-yet-implemented capabilities (ESP-NOW, IR receive) ship with the contract so future Epics plug in rather than retrofit.

---

## 2. Capability declaration

Each backend exposes a flat list of named capabilities. The list is compile-time-fixed per backend - no runtime probing. The DAL queries the list at boot to compose the host's device profile.

```cpp
namespace nocturnation::hal {

enum class Capability : uint8_t {
    Mic,        // microphone samples + on-board FFT, emits AudioFrames
    IRTx,       // IR LED transmitter (raw pulse send)
    IRRx,       // IR receiver (Epic 4+; not declared by Epic 2 StickC backend)
    ESPNow,     // ESP-NOW broadcast/peer (Epic 4+; not declared by Epic 2 StickC backend)
    Display,    // framebuffer + drawing primitives
    Buttons,    // discrete buttons with click/long-press semantics
    IMU,        // 3- or 6-axis accelerometer/gyro
    Battery,    // battery level + voltage + charge state
};

}
```

The backend exposes its declared list:

```cpp
namespace nocturnation::hal {
class HAL {
public:
    // Capability discovery
    static const Capability* capabilities();   // pointer to backend's array
    static size_t capability_count();
    static bool has(Capability c);

    // Lifecycle
    static void begin();                       // init all declared capabilities
    static void loop_tick();                   // called every loop(); advances polled capabilities

    // Per-capability accessors. Return nullptr if the capability is not declared.
    static class Mic*      mic();
    static class IRTx*     ir_tx();
    static class IRRx*     ir_rx();
    static class ESPNow*   esp_now();
    static class Display*  display();
    static class Buttons*  buttons();
    static class IMU*      imu();
    static class Battery*  battery();
};
}
```

`HAL::has(Capability::ESPNow)` returns false on the Epic 2 StickC backend; it returns true once Epic 4 lands. Code that depends on a capability checks `has()` (or accepts the nullptr from the accessor) and degrades gracefully.

There is no `Clock` capability. `millis()` and `micros()` are framework primitives every supported platform provides; they're imported directly where needed rather than wrapped.

---

## 3. Per-capability interfaces

Each interface is a small abstract class. Backends inherit and implement.

### 3.1 Mic

Produces audio analysis frames at a fixed cadence. The mic capability does **raw FFT only** - no beat detection, no thresholding. The orchestration layer subscribes to frames and decides what they mean.

```cpp
struct AudioFrame {
    uint32_t timestamp_ms;     // millis() at frame end
    float    bass_energy;      // sum of FFT bins ~62-187 Hz
    float    mid_energy;       // sum of FFT bins ~250-2000 Hz
    float    treble_energy;    // sum of FFT bins ~2000-8000 Hz
    float    overall_rms;      // mean absolute amplitude over the window
};

class Mic {
public:
    virtual ~Mic() = default;

    virtual void begin(uint16_t sample_rate_hz = 16000,
                       uint16_t fft_size = 512) = 0;
    virtual void end() = 0;
    virtual bool is_running() const = 0;

    // Subscribe to frames. Callback fires synchronously from HAL::loop_tick().
    using FrameCallback = std::function<void(const AudioFrame&)>;
    virtual void set_frame_callback(FrameCallback cb) = 0;

    // Optional: expose raw band-bin range to callers that want to override defaults
    virtual void set_bass_band(uint16_t lo_hz, uint16_t hi_hz) = 0;
};
```

Notes:

- Default cadence is `sample_rate_hz / fft_size` ≈ 31 Hz at 16 kHz / 512. Backend may run faster if it wants overlapping windows; orchestration treats frame timestamps as the authority.
- `bass_energy` is the metric the prototype's beat detection consumes. Other bands are exposed for future effects (treble-driven sparkle, mid-driven hue cycling).
- The mic capability bundles FFT compute. A future host without enough CPU for real-time FFT can simply not declare the `Mic` capability; orchestration sees beat-mode features as unavailable on that host.

### 3.2 IRTx

Sends IR pulse trains. Pulse format matches the `irsend.sendRaw` convention used in the Epic 1 prototype.

```cpp
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
```

### 3.3 IRRx (stub - not declared in Epic 2)

Defined now so Epic 4 (or later, depending on need) plugs in without an interface change.

```cpp
struct IRPulses {
    uint32_t timestamp_ms;
    const uint16_t* pulses_us;
    size_t count;
};

class IRRx {
public:
    virtual ~IRRx() = default;

    virtual void begin() = 0;
    virtual void end() = 0;

    using PulsesCallback = std::function<void(const IRPulses&)>;
    virtual void set_pulses_callback(PulsesCallback cb) = 0;
};
```

### 3.4 ESPNow (stub - not declared in Epic 2)

Defined now so Epic 4 plugs in without an interface change.

```cpp
struct ESPNowMessage {
    uint32_t timestamp_ms;
    uint8_t  peer_mac[6];
    const uint8_t* data;
    size_t   len;
    int8_t   rssi;          // -128 if not available
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
```

### 3.5 Display

Drawing primitives sufficient for the Epic 1 prototype's idle and beat-mode UIs. Colour-space convention: 16-bit RGB565.

```cpp
class Display {
public:
    virtual ~Display() = default;

    virtual void begin() = 0;
    virtual void set_rotation(uint8_t rotation) = 0;        // 0-3
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
```

Standard colour constants (`Display::Black`, `Red`, `Green`, etc.) can live as `static constexpr uint16_t` on the base class.

### 3.6 Buttons

Polled debouncing + click/long-press detection. Backend handles raw GPIO and debounce; consumer only sees semantic events.

Button IDs are **generic and numeric** so application code stays host-independent. `Btn1` is conventionally **the primary / "main" button** on every host that declares the `Buttons` capability. Higher-numbered buttons are secondary; their semantics are up to the application. Each HAL backend documents its physical mapping (which physical button is `Btn1`, etc.) but layers above the HAL never need to know.

```cpp
enum class ButtonId : uint8_t {
    Btn1 = 1,       // primary/main; always present if Buttons capability is declared
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

    // How many buttons this backend exposes (1..8). Buttons above this count
    // simply do not fire events and is_pressed() returns false for them.
    virtual uint8_t count() const = 0;

    using ButtonCallback = std::function<void(ButtonId id, ButtonEvent ev)>;
    virtual void set_callback(ButtonCallback cb) = 0;

    // Optional polling helpers
    virtual bool is_pressed(ButtonId id) = 0;

    // Backend may expose long-press threshold for tuning
    virtual void set_long_press_ms(uint16_t ms) = 0;
};
```

`Buttons::poll()` is implicit - called from `HAL::loop_tick()`.

Application code that wants "the main button" uses `Btn1` and stays portable. Application code that wants secondary functions either uses `Btn2`/`Btn3`/etc. directly (knowing it might no-op on hosts with fewer buttons) or queries `count()` and adapts.

### 3.7 IMU (capability declared if present)

```cpp
struct IMUSample {
    uint32_t timestamp_ms;
    float    ax, ay, az;       // m/s^2
    float    gx, gy, gz;       // rad/s, or 0 on accelerometer-only devices
    bool     has_gyro;
};

class IMU {
public:
    virtual ~IMU() = default;

    virtual void begin() = 0;
    virtual bool read(IMUSample& out) = 0;     // returns false if no fresh sample
};
```

The StickC Plus2 has an MPU6886; declaring `IMU` is fine. Devices without an IMU simply don't declare the capability.

### 3.8 Battery

```cpp
class Battery {
public:
    virtual ~Battery() = default;

    virtual int   level_percent() = 0;     // 0-100, or -1 if not known
    virtual float voltage() = 0;           // volts, or 0 if not measurable
    virtual bool  is_charging() = 0;
};
```

---

## 4. Initialisation lifecycle

The HAL is owned by the DAL. The DAL is the only caller of `HAL::begin()` and `HAL::loop_tick()`; application code (`main.cpp`'s `setup()`/`loop()`) talks to the DAL, never directly to the HAL. This keeps the layering honest - replacing or refactoring the HAL doesn't require touching anything above the DAL.

```cpp
// dal/dal.cpp - inside DAL::begin() and DAL::loop_tick()
void DAL::begin() {
    hal::HAL::begin();           // initialise declared HAL backends first
    // ...then DAL-level work: compose host profile, register drivers, etc.
}

void DAL::loop_tick() {
    hal::HAL::loop_tick();       // pull fresh hardware events
    // ...then DAL-level work: deliver events to subscribers, advance drivers.
}
```

Order matters: HAL must initialise before any DAL-level work in `begin()` because the DAL queries `HAL::capabilities()` to compose profiles. Same ordering in `loop_tick()` so hardware events propagate up before driver state advances.

Each backend's `HAL::begin()` is responsible for ordering its own capability inits internally (e.g., I2C bus before IMU, AXP power-management before everything on the StickC Plus2).

---

## 5. Event delivery model

NocturNation runs single-threaded on Arduino. There are no threads, no real callbacks-from-interrupts in application code. Every callback fires synchronously from `HAL::loop_tick()` (or, for output-only capabilities like `IRTx`, from the call site).

For the mic specifically: `Mic::loop_tick()` (called from `HAL::loop_tick()`) checks if the I2S DMA has filled a buffer. If yes, computes FFT, fires the registered frame callback synchronously, returns. Orchestration's beat-detection logic runs in that same call stack.

This means orchestration must not block in its frame handler. Heavy work (e.g. emitting many `fire_event` calls in response to a beat) runs in the same iteration; if it takes too long, the next FFT frame is delayed. For Epic 2's behaviour-preserving target this matches the prototype's existing tight loop. If timing later becomes a problem, queueing can be added at the DAL level without changing the HAL contract.

---

## 6. StickC Plus2 backend specifics

The Plus2 backend declares: `Mic`, `IRTx`, `Display`, `Buttons`, `IMU`, `Battery`.
The Plus2 backend does **not** declare in Epic 2: `IRRx` (hardware exists but not used yet), `ESPNow` (Epic 4).

Pin and peripheral assignments per the M5Stack Plus2 documentation. Backend constructor pins these; layers above never see GPIO numbers.

| Capability | Implementation hint |
| --- | --- |
| `Mic` | PDM mic via I2S, 16 kHz / 512-sample window. FFT via `kosme/arduinoFFT` (already a project dep). Emits frames at ~31 Hz. |
| `IRTx` | GPIO 19 (built-in IR LED) via `crankyoldgit/IRremoteESP8266`'s `IRsend::sendRaw`. |
| `Display` | 1.14" 240×135 ST7789V2 via LovyanGFX (bundled with M5Unified). |
| `Buttons` | `count() == 3`. Mapping: `Btn1` = front button (GPIO 37, the primary/main button you push to fire), `Btn2` = side button (GPIO 39), `Btn3` = power button (read via AXP/PEK chip). Backend does its own debounce. |
| `IMU` | MPU6886 over I2C. |
| `Battery` | AXP192 ADC for voltage; charge state from AXP status register. |

The Plus2 backend may use M5Unified internally (it's already a dep, and saves boilerplate), but exposes nothing of M5Unified's API to layers above. The capability accessors return pointers to plain C++ objects; the abstraction is sealed at the HAL boundary.

---

## 7. What's deliberately not in the HAL

- **Protocol logic.** The PixMob IR encoder lives in `lib/pixmob/` (already there, header-only). The ESP-NOW frame format will live in its own driver module under the DAL when Epic 4 lands. The HAL only exposes raw transports (`IRTx::send_raw`, `ESPNow::send_broadcast`).
- **Beat detection.** Orchestration's job. The mic capability emits raw spectrum frames; orchestration decides what's a beat.
- **Show timeline / mode state machine / FX engine.** Orchestration. None of these need to know what host they're running on; the HAL is one level too low for them.
- **Threads and async.** Single-threaded loop model. If we ever need real concurrency, that lives in a layer above the HAL (a scheduler primitive in the DAL or orchestration), not in the HAL itself.
- **Filesystem, OTA, networking config.** Out of scope for the audio-and-IR pipeline. Will be added as separate capabilities in later Epics if needed.

---

## 8. Open questions

- **Capability accessor pattern.** Static methods (current sketch) vs. an `HAL` singleton instance vs. a templated `HAL::get<Mic>()`. Static is simplest to read but couples capability names into the `HAL` class header. Consider revisiting once we have two backends.
- **Display abstraction depth.** Wrapping LovyanGFX with primitives is enough for Epic 1's UIs but may not be enough for Epic 3's richer screens. Decide on extending `Display` (more primitives) vs. exposing a typed sub-interface (e.g. `Display::sprite()`) at Epic 3 time.
- **ESP-NOW receive ordering.** ESP-NOW callbacks on ESP32 fire from a vendor task. The HAL will need to bridge that into the polled `loop_tick()` model via a small lock-free queue. Sketch the queue when Epic 4 starts; not a contract concern now.
- **Tildagon constraints.** When the Tildagon backend drafts, the IMU/buttons/display interfaces may need optional fields the StickC didn't surface. Plan for backwards-compatible additions.

---

## 9. Out-of-band notes for implementation (Epic 2)

When Feature 2.0 (HAL implementation) starts:

1. Create `lib/hal/include/hal.h` with the interfaces above (one header per capability is fine, or one combined header for now).
2. Create `lib/hal/stickc/` for the Plus2 backend implementations.
3. Use `build_src_filter` in `platformio.ini` to select which backend the firmware compiles against. Default in this repo: `stickc`. Future backends sit in `lib/hal/<host>/`.
4. Add a native `test/test_hal_capability_query/` that links against a stub backend declaring a known capability set, verifies `HAL::has()` and `HAL::capabilities()` return the right list. This is the only HAL test that runs on the host without hardware; everything else is verified by Block 3-style hardware ritual.
