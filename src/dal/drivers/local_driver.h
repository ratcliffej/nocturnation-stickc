// LocalDriver - the DAL driver for the "local" transport.
//
// Bridges the host's own HAL capabilities into the DAL's typed event
// interface. On output, translates display events into hal::Display calls.
// On input, registers a HAL Buttons callback that delivers ButtonPressEvent
// up through DAL::deliver_button_press("local", ...).
//
// Registered automatically by DAL::begin() when at least one local
// capability is wired up (i.e. hal::HAL::display() or hal::HAL::buttons()
// returns a non-null instance). On hosts without those backends, the
// LocalDriver does not register and fire_display_*("local", ...) calls
// return false silently - which is the correct fail-silent behaviour.

#pragma once

#include "dal/dal.h"

namespace nocturnation {
namespace dal {

class LocalDriver : public Driver {
public:
    const char* transport_name() const override { return "local"; }

    bool begin() override;
    void loop_tick() override {}    // HAL polls itself; no per-driver work.

    bool send(uint8_t group_id, const DisplayShowTextEvent&)  override;
    bool send(uint8_t group_id, const DisplayClearEvent&)     override;
    bool send(uint8_t group_id, const DisplayFillRectEvent&)  override;
    bool send(uint8_t group_id, const DisplayMeterEvent&)     override;

    bool start_audio_input(uint16_t sample_rate_hz, uint16_t fft_size) override;
    bool stop_audio_input() override;
};

// Singleton accessor used by DAL::begin() to register the driver.
LocalDriver* local_driver_instance();

}  // namespace dal
}  // namespace nocturnation
