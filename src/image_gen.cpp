#include "image_gen.h"

#include "elevenlabs_image.h"
#include "openrouter_image.h"
#include "runtime_config.h"

#include <Arduino.h>
#include <string.h>

namespace {

enum class Active : uint8_t { None = 0, ElevenLabs, OpenRouter };

Active active_ = Active::None;

} // namespace

ImageProvider imageGenProvider() {
  // Forced to OpenRouter for all agents for now; ElevenLabs Flows stays wired
  // but is not selected until we flip this back / honor Config::ImageProvider.
  return ImageProvider::OpenRouter;
}

bool imageGenStart(const ImageGenRequest &req, ImageGenResult *earlyErr) {
  auto fail = [&](const char *msg) -> bool {
    if (earlyErr) {
      *earlyErr = ImageGenResult{};
      earlyErr->ok = false;
      strncpy(earlyErr->error, msg, sizeof(earlyErr->error) - 1);
    }
    return false;
  };

  if (imageGenBusy()) return fail("image gen busy");

  const ImageProvider prov = imageGenProvider();
  Serial.printf("[image-gen] provider=%s\n",
                prov == ImageProvider::OpenRouter ? "openrouter"
                                                  : "elevenlabs");

  if (prov == ImageProvider::OpenRouter) {
    const char *key = getConfig(Config::OpenrouterApiKey);
    if (!key[0]) return fail("OpenRouter API key not configured");

    OrImageRequest orReq;
    orReq.apiKey = key;
    orReq.prompt = req.prompt;
    orReq.model = req.model;
    orReq.storage = req.storage;
    orReq.referencePath = req.referencePath;
    orReq.attachReference = req.attachReference;

    OrImageResult early{};
    if (!orImageStart(orReq, &early)) {
      if (earlyErr) {
        earlyErr->ok = false;
        strncpy(earlyErr->error, early.error, sizeof(earlyErr->error) - 1);
      }
      return false;
    }
    active_ = Active::OpenRouter;
    return true;
  }

  const char *key = getConfig(Config::ElevenlabsApiKey);
  if (!key[0]) return fail("ElevenLabs API key not configured");

  ElImageRequest elReq;
  elReq.apiKey = key;
  elReq.prompt = req.prompt;
  elReq.model = req.model;
  elReq.storage = req.storage;
  elReq.referencePath = req.referencePath;
  elReq.attachReference = req.attachReference;

  ElImageResult early{};
  if (!elImageStart(elReq, &early)) {
    if (earlyErr) {
      earlyErr->ok = false;
      strncpy(earlyErr->error, early.error, sizeof(earlyErr->error) - 1);
    }
    return false;
  }
  active_ = Active::ElevenLabs;
  return true;
}

bool imageGenBusy() { return elImageBusy() || orImageBusy(); }

bool imageGenTakeResult(ImageGenResult *out) {
  if (!out) return false;

  if (active_ == Active::ElevenLabs || active_ == Active::None) {
    ElImageResult r{};
    if (elImageTakeResult(&r)) {
      out->ok = r.ok;
      strncpy(out->path, r.path, sizeof(out->path) - 1);
      strncpy(out->error, r.error, sizeof(out->error) - 1);
      active_ = Active::None;
      return true;
    }
  }
  if (active_ == Active::OpenRouter || active_ == Active::None) {
    OrImageResult r{};
    if (orImageTakeResult(&r)) {
      out->ok = r.ok;
      strncpy(out->path, r.path, sizeof(out->path) - 1);
      strncpy(out->error, r.error, sizeof(out->error) - 1);
      active_ = Active::None;
      return true;
    }
  }
  return false;
}

const char *imageGenStatus() {
  if (elImageBusy()) return elImageStatus();
  if (orImageBusy()) return orImageStatus();
  if (active_ == Active::ElevenLabs) return elImageStatus();
  if (active_ == Active::OpenRouter) return orImageStatus();
  const char *el = elImageStatus();
  if (el && el[0] && strcmp(el, "Idle") != 0) return el;
  const char *or_ = orImageStatus();
  if (or_ && or_[0] && strcmp(or_, "Idle") != 0) return or_;
  return "Idle";
}

uint32_t imageGenStatusGen() {
  return elImageStatusGen() + orImageStatusGen();
}

void imageGenReset() {
  elImageReset();
  orImageReset();
  active_ = Active::None;
}
