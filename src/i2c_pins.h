#pragma once

#include <stdint.h>

/** Heyvinu I2C — MAX17048 fuel gauge (schematic: IO17=SDA, IO18=SCL). */
namespace HeyvinuI2c {
static constexpr int8_t kSda = 17;
static constexpr int8_t kScl = 18;
static constexpr uint8_t kGaugeAddr = 0x36;
} // namespace HeyvinuI2c
