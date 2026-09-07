#pragma once

#include <stdint.h>

/** Heyvinu J5 (JOY) — schematic pins (VRx=8, VRy=9).
 *  Mapping: VRy = L/R (inverted), VRx = U/D
 *  (HORIZ_ON_Y + INVERT_HORIZ).
 */
namespace HeyvinuInput {
static constexpr int8_t kJoyPinX = 8;   // JOY_VRX
static constexpr int8_t kJoyPinY = 9;   // JOY_VRY
static constexpr int8_t kJoyPinSw = 4;  // JOY_SW (active low)

static constexpr uint16_t kJoyDeadzone = 600;
static constexpr uint16_t kJoyHoldDelayMs = 450;
static constexpr uint16_t kJoyHoldRepeatMs = 175;
} // namespace HeyvinuInput
