#pragma once

#include <stddef.h>
#include <stdint.h>

class Storage;

/**
 * ElevenLabs Flows image generation — independent of OpenRouter.
 *
 * POST /v1/flows/image → poll GET → download content_url →
 * /chitram/images/ on SD. Own FreeRTOS task.
 */

struct ElImageRequest {
  const char *apiKey = nullptr;
  const char *prompt = nullptr;
  /** ElevenLabs model_id; null → gemini-3.1-flash-image. */
  const char *model = nullptr;
  Storage *storage = nullptr;
  const char *referencePath = nullptr;
  bool attachReference = true;
};

struct ElImageResult {
  bool ok = false;
  char path[96] = {};
  char error[96] = {};
};

bool elImageStart(const ElImageRequest &req, ElImageResult *earlyErr);
bool elImageBusy();
bool elImageTakeResult(ElImageResult *out);
const char *elImageStatus();
uint32_t elImageStatusGen();
void elImageReset();
