#pragma once

#include <stdint.h>

/** Chitram I2S — MAX98357A amp (DOUT) + INMP441 mic (DIN). BCLK/WS shared.
 *
 * DIN = GPIO3 (JOY_SW on schematic; stick uses IO4 in firmware — IO3 free).
 * Rev-A J3 pin5 → IO15 is broken — bodgewire mic SD (J3 pin5) to ESP32 IO3.
 * Do not use IO5 (VR1 volume pot soldered there).
 */
namespace ChitramAudio {
static constexpr int8_t kI2sBclk = 47;
static constexpr int8_t kI2sWs = 48;
static constexpr int8_t kI2sDout = 7;
static constexpr int8_t kI2sDin = 3;
} // namespace ChitramAudio
