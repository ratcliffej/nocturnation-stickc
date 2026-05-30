// Protocol-neutral pulse envelope descriptors.
//
// `Time` and `Chance` describe the *intent* of a render command: how a
// pulse rises, holds, and decays, and the per-target probability that it
// fires. They're how a Show, an Effect, or any other render-side caller
// expresses what it wants the room to do.
//
// The values currently double as PixMob 3-bit wire fields - that's a
// historical convenience, not the contract. Future output bindings
// (DMX, BLE, custom hardware) will map the same abstract levels to
// their own wire formats. The encoder for each binding owns the
// abstract→wire mapping; callers stay protocol-neutral.
//
// These are `using`-aliases of the existing `pixmob::` enums, so the
// underlying type is unchanged and the wire encoder accepts them
// without modification. `pulse::T_0_MS` and `pixmob::T_0_MS` are the
// same enumerator; both names will compile against any function taking
// either type. The aliases are the canonical names for render-side
// callers; `pixmob::` stays as the wire-side name in the encoder TU.

#pragma once

#include "pixmob_protocol.h"

namespace nocturnation {
namespace pulse {

// Abstract pulse envelope timings (attack / sustain / release).
using Time = ::pixmob::Time;

// Abstract per-target firing probabilities.
using Chance = ::pixmob::Chance;

// Re-export enumerators so callers can write `pulse::T_0_MS` instead of
// `pulse::Time(::pixmob::T_0_MS)`. Plain enums, so the enumerators sit
// in the enclosing namespace; a `using`-declaration brings them across
// without changing the underlying value.
using ::pixmob::T_0_MS;
using ::pixmob::T_32_MS;
using ::pixmob::T_96_MS;
using ::pixmob::T_192_MS;
using ::pixmob::T_480_MS;
using ::pixmob::T_960_MS;
using ::pixmob::T_2400_MS;
using ::pixmob::T_3840_MS;

using ::pixmob::CHANCE_100;
using ::pixmob::CHANCE_88;
using ::pixmob::CHANCE_67;
using ::pixmob::CHANCE_50;
using ::pixmob::CHANCE_32;
using ::pixmob::CHANCE_16;
using ::pixmob::CHANCE_10;
using ::pixmob::CHANCE_4;

}  // namespace pulse
}  // namespace nocturnation
