#pragma once

#include <stddef.h>
#include <stdint.h>

class Storage;

/**
 * Image generation facade — routes to ElevenLabs Flows or OpenRouter
 * based on Config::ImageProvider (`elevenlabs` | `openrouter`).
 * Currently forced to openrouter for all agents.
 */

struct ImageGenRequest {
  const char *prompt = nullptr;
  /** Provider-specific model slug; null → each backend's default. */
  const char *model = nullptr;
  Storage *storage = nullptr;
  const char *referencePath = nullptr;
  bool attachReference = true;
};

struct ImageGenResult {
  bool ok = false;
  char path[96] = {};
  char error[96] = {};
};

enum class ImageProvider : uint8_t {
  ElevenLabs = 0,
  OpenRouter = 1,
};

/** Currently forced to OpenRouter (config ignored). */
ImageProvider imageGenProvider();

bool imageGenStart(const ImageGenRequest &req, ImageGenResult *earlyErr);
bool imageGenBusy();
bool imageGenTakeResult(ImageGenResult *out);
const char *imageGenStatus();
uint32_t imageGenStatusGen();
void imageGenReset();
