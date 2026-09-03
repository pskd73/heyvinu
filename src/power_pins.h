#pragma once

#include <stdint.h>

/**
 * Chitram charger status (TP4056). Schematic leaves CHRG/STDBY NC — set GPIO when wired.
 * CHRG: active LOW while charging. STDBY: active LOW when charge complete.
 */
namespace ChitramPower {
static constexpr int8_t kChrgPin = -1;
static constexpr int8_t kStdbyPin = -1;
} // namespace ChitramPower
