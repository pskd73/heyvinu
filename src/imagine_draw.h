#pragma once

#include <Arduino.h>
#include <stdint.h>

/** Decode JPEG (TJpg_Decoder) scaled to cover outW×outH RGB565. */
bool imagineLoadCover(const char *path, uint16_t *outRgb, int16_t outW,
                      int16_t outH);
