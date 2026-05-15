// NocturNation Mode state machine.
//
// Implements the runtime mode FSM defined in architecture spec §8.1-8.2.
// Modes are top-level operating states (Boot countdown, Menu, Autonomous
// Director, Lume, Config, Test). Transitions are driven by button events
// from the DAL and by per-mode timers.
//
// The DAL is the only thing this layer talks to. ModeMachine subscribes to
// "local" button and audio events at begin() and routes each event to the
// currently active mode. Modes fire UI through DAL::fire_display_* and IR
// through DAL::fire_rgb_pulse, exactly as orchestration always has.
//
// Application code calls only ModeMachine::begin() and ::loop_tick(). The
// concrete mode classes are an implementation detail of mode_machine.cpp.

#pragma once

#include <cstdint>
#include "dal/dal.h"
#include "hal/input_action.h"

namespace nocturnation {
namespace modes {

// =============================================================================
// Mode identifiers
// =============================================================================
//
// Boot is transient (countdown to default mode). Menu is the navigation hub
// reached by interrupting Boot or by long-pressing PWR from any runtime
// mode. Director, Lume, Config, and Test are the persisted runtime
// modes; ModeMachine remembers which was last selected and defaults the
// next boot's countdown to it.
//
// Spec §8.2 lists Idle as a separate mode; in this implementation the Menu
// serves as the long-press-PWR destination, since pre-Epic-4 there's no
// behavioural difference between an "idle" device and one sitting at the
// menu screen. Idle can be reintroduced later if there's a real low-power
// requirement.

enum class ModeId : uint8_t {
    Boot              = 0,
    Menu              = 1,
    Director          = 2,
    Lume              = 3,
    Config            = 4,
    Test              = 5,
};

// =============================================================================
// Mode base class
// =============================================================================
//
// Concrete modes override the hooks they care about. Default implementations
// are no-ops, so a stub mode can be added with an empty class body.
//
// Lifetime: ModeMachine owns one instance of each concrete mode (static
// storage). enter()/exit() bracket the time a mode is active; loop_tick()
// fires every iteration; on_*() fire when DAL delivers the matching event.

class Mode {
public:
    virtual ~Mode() = default;

    virtual ModeId id() const = 0;
    virtual const char* name() const = 0;

    virtual void enter() {}
    virtual void exit()  {}
    virtual void loop_tick() {}

    virtual void on_button_event(const dal::ButtonPressEvent&) {}
    virtual void on_audio_frame (const dal::AudioFrameEvent&)  {}
    // Spectrum-frame routing (Epic 4.6 Block 11). Mirrors on_audio_frame:
    // ModeMachine subscribes once to DAL::subscribe_spectrum_frames at
    // begin() and routes incoming events to the active mode. Modes that
    // host a Visualisation declaring needs_spectrum_frame in its
    // PowerProfile forward to the vis from this hook.
    virtual void on_spectrum_frame(const dal::SpectrumFrameEvent&) {}
    // Semantic input action delivered via the host's InputActionMapper
    // (Block 4) and routed by ModeMachine's DAL::subscribe_input_actions
    // facade. Modes that have migrated to semantic input (Block 10
    // onwards) override this; legacy modes continue to handle raw
    // ButtonPressEvents until they migrate.
    virtual void on_input_action(const hal::InputEvent&)       {}
};

// =============================================================================
// ModeMachine - the public facade
// =============================================================================

class ModeMachine {
public:
    // Bring up the FSM. Subscribes to DAL button + audio events for "local",
    // loads the last-used runtime mode from NVS (defaults to Director
    // first boot), then enters Boot for the 5s countdown.
    static void begin();

    // Advance the active mode. Call once per main loop().
    static void loop_tick();

    // Switch modes. Calls exit() on the current mode then enter() on the
    // target. If the target is a persisted runtime mode (Director,
    // Lume, Config, Test), the choice is also written to NVS so the next
    // boot defaults to it.
    static void switch_to(ModeId target);

    // Diagnostic / test accessors.
    static ModeId current();
    static const char* current_name();
};

// =============================================================================
// Test seam (native builds only)
// =============================================================================
//
// On hardware millis() is provided by the Arduino framework. In native test
// builds mode_machine.cpp ships a tiny stub whose value tests can advance
// via this hook. Not part of the firmware ABI.

#ifndef ARDUINO
namespace test_seam {
void set_millis(uint32_t v);
}
#endif

}  // namespace modes
}  // namespace nocturnation
