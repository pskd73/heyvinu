#pragma once

#include <stddef.h>
#include <stdint.h>

class Storage;

/**
 * Load a JPEG/PNG from SD into an RGB565 PSRAM buffer for UIImage.
 * Decodes with Cover into exactly maxW×maxH.
 *
 * After the first full-res decode, a panel-sized JPEG sidecar
 * (`foo.png` → `foo.preview.jpg`) is written on a low-priority task so Talk
 * / UI are not blocked. Later loads prefer the sidecar when present.
 * Pass writeSidecar=false for gallery thumbs so panel sidecars are not
 * overwritten at thumb size.
 */
bool imagePreviewLoad(Storage *storage, const char *absPath, int16_t maxW,
                      int16_t maxH, bool writeSidecar = true);
void imagePreviewClear();

const uint16_t *imagePreviewPixels();
int16_t imagePreviewWidth();
int16_t imagePreviewHeight();
uint32_t imagePreviewGen();
/** Canonical source path (full image), not the sidecar. */
const char *imagePreviewPath();
/**
 * Panel JPEG sidecar for `imagePreviewPath()` (or `srcAbs` if given).
 * Writes into `out`; returns false if it cannot be formed.
 */
bool imagePreviewSidecarPath(char *out, size_t outLen,
                             const char *srcAbs = nullptr);
/**
 * Ensure the panel JPEG sidecar exists for the current preview.
 * Safe on a background task (e.g. or_image). No-op if already on disk.
 * Returns true when the sidecar path exists afterward.
 */
bool imagePreviewEnsureSidecar(Storage *storage);
