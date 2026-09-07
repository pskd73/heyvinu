#pragma once

#include <stddef.h>
#include <stdint.h>

class Storage;

/**
 * OpenRouter image generation — independent of Talk / ElevenLabs.
 *
 * POST https://openrouter.ai/api/v1/images → decode b64 → save under
 * /heyvinu/images/ on the SD card. Runs on its own FreeRTOS task so callers
 * (e.g. a client tool) are not blocked for tens of seconds.
 */

struct OrImageRequest {
  const char *apiKey = nullptr;
  const char *prompt = nullptr;
  /** OpenRouter model slug; null → built-in default. */
  const char *model = nullptr;
  Storage *storage = nullptr;
  /**
   * Optional full-image path whose `.preview.jpg` sidecar is sent as
   * input_references. Null → use current preview / last generated image.
   */
  const char *referencePath = nullptr;
  /** When true (default), attach a reference if a sidecar is available. */
  bool attachReference = true;
};

struct OrImageResult {
  bool ok = false;
  char path[96] = {};
  char error[96] = {};
};

/** Non-blocking: queues work. False if busy / bad args (error in out). */
bool orImageStart(const OrImageRequest &req, OrImageResult *earlyErr);

bool orImageBusy();

/**
 * If a job finished, copies the result and returns true (one-shot).
 * Safe to call from the WS / UI thread.
 */
bool orImageTakeResult(OrImageResult *out);

/** Human status for UI: Idle / Generating… / Saved … / error. */
const char *orImageStatus();
uint32_t orImageStatusGen();

/**
 * Force-clear busy/pending state. In-flight work is invalidated (epoch bump);
 * a zombie task may finish later and is ignored. Call on Talk stop/start.
 */
void orImageReset();
