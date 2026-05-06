// Native-test shim for Arduino.h. pixmob_protocol.h includes <Arduino.h>
// for uint8_t / size_t / uint16_t; on the host we can satisfy that with
// the standard headers and skip everything else Arduino.h drags in.
#pragma once
#include <cstddef>
#include <cstdint>
