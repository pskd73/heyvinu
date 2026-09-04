#pragma once

#include <stddef.h>
#include <stdint.h>

class Storage;

/**
 * Load a JPEG/PNG from SD into an RGB565 PSRAM buffer for UIImage.
 * Decodes with Cover into exactly maxW×maxH (panel-sized fullscreen).
 */
bool imagePreviewLoad(Storage *storage, const char *absPath, int16_t maxW,
                      int16_t maxH);
void imagePreviewClear();

const uint16_t *imagePreviewPixels();
int16_t imagePreviewWidth();
int16_t imagePreviewHeight();
uint32_t imagePreviewGen();
const char *imagePreviewPath();
