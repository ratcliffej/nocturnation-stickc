// pixmob_protocol.h
//
// Runtime PixMob IR command builder, ported from
// github.com/jamesw343/PixMob_IR (verified bit-for-bit against the Python encoder)

#pragma once
#include <Arduino.h>

namespace pixmob {

// Time values for attack, sustain, release (3-bit lookup keys)
enum Time : uint8_t {
  T_0_MS    = 0b000,
  T_32_MS   = 0b001,
  T_96_MS   = 0b010,
  T_192_MS  = 0b011,
  T_480_MS  = 0b100,
  T_960_MS  = 0b101,
  T_2400_MS = 0b110,
  T_3840_MS = 0b111
};

enum Chance : uint8_t {
  CHANCE_100 = 0b000,
  CHANCE_88  = 0b001,
  CHANCE_67  = 0b010,
  CHANCE_50  = 0b011,
  CHANCE_32  = 0b100,
  CHANCE_16  = 0b101,
  CHANCE_10  = 0b110,
  CHANCE_4   = 0b111
};

constexpr uint16_t PULSE_US = 694;

// Encoding map: 6-bit raw -> 8-bit encoded
static const uint8_t ENCODING_MAP[64] = {
  0x21, 0x32, 0x54, 0x65, 0xa9, 0x9a, 0x6d, 0x29,
  0x56, 0x92, 0xa1, 0xb4, 0xb2, 0x84, 0x66, 0x2a,
  0x4c, 0x6a, 0xa6, 0x95, 0x62, 0x51, 0x42, 0x24,
  0x35, 0x46, 0x8a, 0xac, 0x8c, 0x6c, 0x2c, 0x4a,
  0x59, 0x86, 0xa4, 0xa2, 0x91, 0x64, 0x55, 0x44,
  0x22, 0x31, 0xb1, 0x52, 0x85, 0x96, 0xa5, 0x69,
  0x5a, 0x2d, 0x4d, 0x89, 0x45, 0x34, 0x61, 0x25,
  0x36, 0xad, 0x94, 0xaa, 0x8d, 0x49, 0x99, 0x26
};

// Encode a raw N-byte buffer (N=6 or N=9) into a microsecond run-length array.
static size_t encodeBufferToUs(uint8_t* buf, size_t bufLen,
                               uint16_t* outBuf, size_t outBufCapacity) {
  uint32_t checksumSum = 0;
  for (size_t i = 2; i < bufLen; i++) {
    buf[i] = ENCODING_MAP[buf[i] & 0x3F];
    checksumSum += buf[i];
  }
  buf[1] = ENCODING_MAP[(checksumSum >> 2) & 0x3F];

  bool bits[80];
  size_t bitCount = 0;
  bool started = false;
  for (size_t i = 0; i < bufLen; i++) {
    uint8_t b = buf[i];
    for (int k = 0; k < 8; k++) {
      bool bit = (b & 1) != 0;
      if (started || bit) {
        bits[bitCount++] = bit;
        started = true;
      }
      b >>= 1;
    }
  }
  while (bitCount > 0 && !bits[bitCount - 1]) bitCount--;

  if (bitCount == 0 || outBufCapacity == 0) return 0;
  size_t outIdx = 0;
  size_t i = 0;
  while (i < bitCount && outIdx < outBufCapacity) {
    size_t j = i;
    while (j < bitCount && bits[j] == bits[i]) j++;
    outBuf[outIdx++] = (uint16_t)((j - i) * PULSE_US);
    i = j;
  }
  return outIdx;
}

// =============================================================================
// Display commands - what to show RIGHT NOW
// =============================================================================

// Display a single colour with full ASR control. Most-used command.
//
// When `restrictGroupId` is 0 the frame is encoded as 8 bytes (no
// group filter byte appended) and bracelets respond regardless of
// their stored group - the protocol's broadcast form. When
// `restrictGroupId` is 1..31 the frame is 9 bytes with the group
// byte appended; only bracelets stored at that group respond.
// The difference matters: the 8-byte form has a different checksum
// (sum over buf[2..7] vs buf[2..8]), so it's not equivalent to a
// 9-byte frame with buf[8]=0 even though encodeBufferToUs trims
// trailing zeros after the group byte's bits.
static size_t buildSingleColor(uint16_t* outBuf, size_t outBufCapacity,
                               uint8_t r, uint8_t g, uint8_t b,
                               Time attack, Time sustain, Time release,
                               Chance chance = CHANCE_100,
                               uint8_t restrictGroupId = 0) {
  uint8_t buf[9];
  buf[0] = 0x80;
  buf[1] = 0x00;
  buf[2] = 0x00;             // type=0b000, on_start=0
  buf[3] = (g >> 2) & 0x3F;
  buf[4] = (r >> 2) & 0x3F;
  buf[5] = (b >> 2) & 0x3F;
  buf[6] = ((uint8_t)attack << 3) | ((uint8_t)chance & 0x07);
  buf[7] = ((uint8_t)release << 3) | ((uint8_t)sustain & 0x07);
  if (restrictGroupId == 0) {
      // Broadcast form: 8-byte frame, no group byte. Every bracelet
      // responds. Checksum covers buf[2..7] only.
      return encodeBufferToUs(buf, 8, outBuf, outBufCapacity);
  }
  buf[8] = restrictGroupId & 0x1F;
  return encodeBufferToUs(buf, 9, outBuf, outBufCapacity);
}

// Flash colour 1 briefly (~25 ms), then colour 2 with default 384 ms sustain.
// Useful for "kick + tail colour" effects that look like one composite event.
static size_t buildTwoColors(uint16_t* outBuf, size_t outBufCapacity,
                             uint8_t r1, uint8_t g1, uint8_t b1,
                             uint8_t r2, uint8_t g2, uint8_t b2) {
  uint8_t buf[9];
  buf[0] = 0x80;
  buf[1] = 0x00;
  buf[2] = (0b010 << 1);     // type=0b010, on_start=0
  buf[3] = (g1 >> 2) & 0x3F;
  buf[4] = (r1 >> 2) & 0x3F;
  buf[5] = (b1 >> 2) & 0x3F;
  buf[6] = (g2 >> 2) & 0x3F;
  buf[7] = (r2 >> 2) & 0x3F;
  buf[8] = (b2 >> 2) & 0x3F;
  return encodeBufferToUs(buf, 9, outBuf, outBufCapacity);
}

// Trigger profile cycling. Bracelet autonomously walks through enabled
// EEPROM profile slots with the given envelope.
//   profileMask: 8 bits, one per profile slot 0-7. 0xFF = all 8 active.
//                e.g. 0x0F = profiles 0,1,2,3 only.
//   random: false = sequential cycling, true = random selection
static size_t buildCycleProfiles(uint16_t* outBuf, size_t outBufCapacity,
                                 uint8_t profileMask, bool random,
                                 Time attack, Time sustain, Time release) {
  uint8_t buf[6];
  buf[0] = 0x80;
  buf[1] = 0x00;
  buf[2] = (0b001 << 1);     // type=0b001, on_start=0
  buf[3] = profileMask & 0x3F;
  buf[4] = ((profileMask >> 6) & 0x03)
         | ((random ? 1 : 0) << 2)
         | (((uint8_t)attack & 0x07) << 3);
  buf[5] = ((uint8_t)sustain & 0x07)
         | (((uint8_t)release & 0x07) << 3);
  return encodeBufferToUs(buf, 6, outBuf, outBufCapacity);
}

// =============================================================================
// EEPROM-write commands - bracelet setup (one-time per bracelet)
// =============================================================================

// Save an RGB value to a profile slot or as the bracelet's idle background.
//   profileId: 0-15 (the bracelet has 16 profile slots in EEPROM)
//   isBackground: true = save as background colour (always-on tint), profileId ignored
//                 false = save into profile slot
//   skipDisplay: true = don't show the colour now, just save it silently (recommended for setup)
static size_t buildSetColor(uint16_t* outBuf, size_t outBufCapacity,
                            uint8_t r, uint8_t g, uint8_t b,
                            uint8_t profileId, bool isBackground = false,
                            bool skipDisplay = true,
                            uint8_t restrictGroupId = 0) {
  uint8_t buf[9];
  buf[0] = 0x80;
  buf[1] = 0x00;
  buf[2] = (0b111 << 1) | 1;  // type=0b111, on_start=1
  buf[3] = (g >> 2) & 0x3F;
  buf[4] = (r >> 2) & 0x3F;
  buf[5] = (b >> 2) & 0x3F;
  buf[6] = (profileId & 0x0F)
         | ((isBackground ? 1 : 0) << 4)
         | ((skipDisplay ? 1 : 0) << 5);
  buf[7] = 0;                 // action_id
  buf[8] = restrictGroupId & 0x1F;
  return encodeBufferToUs(buf, 9, outBuf, outBufCapacity);
}

// Assign a bracelet to a group ID. Use during one-time setup with
// other bracelets physically isolated.
//   groupSel: which of 8 EEPROM slots to write into (0-7); slot 0 normally
//   newGroupId: 1-31 (zero is reserved for broadcast)
//
// IMPORTANT: SetGroupId only writes the value into the slot. To make that
// slot's value the bracelet's ACTIVE group ID (i.e. start filtering on it)
// you must follow up with buildSetGroupSel(slot) - even if the slot was
// already selected. The reference workflow is the two-command sequence:
//   buildSetGroupId(slot, new_id);
//   buildSetGroupSel(slot);
static size_t buildSetGroupId(uint16_t* outBuf, size_t outBufCapacity,
                              uint8_t groupSel, uint8_t newGroupId,
                              uint8_t restrictGroupId = 0) {
  uint8_t buf[9];
  buf[0] = 0x80;
  buf[1] = 0x00;
  buf[2] = (0b111 << 1) | 1;
  buf[3] = 0;
  buf[4] = 0;
  buf[5] = groupSel & 0x07;
  buf[6] = newGroupId & 0x1F;
  buf[7] = 2;                 // action_id
  buf[8] = restrictGroupId & 0x1F;
  return encodeBufferToUs(buf, 9, outBuf, outBufCapacity);
}

// Select which slot's stored group ID is the bracelet's active filter.
// Pair with buildSetGroupId after writing a new value into a slot.
static size_t buildSetGroupSel(uint16_t* outBuf, size_t outBufCapacity,
                               uint8_t groupSel,
                               uint8_t restrictGroupId = 0) {
  uint8_t buf[9];
  buf[0] = 0x80;
  buf[1] = 0x00;
  buf[2] = (0b111 << 1) | 1;
  buf[3] = 0;
  buf[4] = 0;
  buf[5] = groupSel & 0x07;
  buf[6] = 0;
  buf[7] = 1;                 // action_id (SetGroupSel)
  buf[8] = restrictGroupId & 0x1F;
  return encodeBufferToUs(buf, 9, outBuf, outBufCapacity);
}

}  // namespace pixmob