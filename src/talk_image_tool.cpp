#include "talk_image_tool.h"

#include "openrouter_image.h"
#include "runtime_config.h"
#include "talk_tools.h"

#include <Flow32.h>

#include <Arduino.h>
#include <string.h>

namespace {

Storage *storage_ = nullptr;
char pendingCallId_[64] = {};
bool pending_ = false;

bool toolGenerateImage(JsonObjectConst params, char *resultOut,
                       size_t resultLen) {
  // Sync entry is unused — talk_agent routes generate_image through
  // talkImageToolStart so the call id is available for the async result.
  (void)params;
  snprintf(resultOut, resultLen, "use async path");
  return false;
}

} // namespace

void talkImageToolReset() {
  pending_ = false;
  pendingCallId_[0] = '\0';
  orImageReset();
}

void talkImageToolSetStorage(Storage *storage) { storage_ = storage; }

void talkImageToolRegister() {
  talkToolsRegister("generate_image", toolGenerateImage);
}

bool talkImageToolStart(const char *toolCallId, JsonObjectConst params,
                        char *resultOut, size_t resultLen, bool *pending) {
  if (pending) *pending = false;
  if (resultOut && resultLen) resultOut[0] = '\0';

  if (!toolCallId || !toolCallId[0]) {
    snprintf(resultOut, resultLen, "missing tool_call_id");
    return false;
  }
  if (pending_ || orImageBusy()) {
    snprintf(resultOut, resultLen, "image generation already running");
    return false;
  }

  const char *prompt = params["prompt"] | "";
  if (!prompt[0]) prompt = params["text"] | "";
  if (!prompt[0]) prompt = params["description"] | "";
  if (!prompt[0]) {
    snprintf(resultOut, resultLen, "missing prompt");
    return false;
  }

  const char *model = params["model"] | "";
  const char *apiKey = getConfig(Config::OpenrouterApiKey);
  if (!apiKey[0]) {
    snprintf(resultOut, resultLen, "OpenRouter API key not configured");
    return false;
  }
  if (!storage_ || !storage_->ready()) {
    snprintf(resultOut, resultLen, "SD card not ready");
    return false;
  }

  OrImageRequest req;
  req.apiKey = apiKey;
  req.prompt = prompt;
  req.model = model[0] ? model : nullptr;
  req.storage = storage_;

  OrImageResult early{};
  if (!orImageStart(req, &early)) {
    snprintf(resultOut, resultLen, "%s",
             early.error[0] ? early.error : "start failed");
    return false;
  }

  strncpy(pendingCallId_, toolCallId, sizeof(pendingCallId_) - 1);
  pendingCallId_[sizeof(pendingCallId_) - 1] = '\0';
  pending_ = true;
  if (pending) *pending = true;
  snprintf(resultOut, resultLen, "started");
  Serial.printf("[tool] generate_image started id=%s\n", pendingCallId_);
  return true;
}

bool talkImageToolTakeResult(char *callIdOut, size_t callIdLen,
                             char *resultOut, size_t resultLen, bool *isError) {
  if (!pending_) return false;
  OrImageResult r{};
  if (!orImageTakeResult(&r)) return false;

  pending_ = false;
  if (callIdOut && callIdLen) {
    strncpy(callIdOut, pendingCallId_, callIdLen - 1);
    callIdOut[callIdLen - 1] = '\0';
  }
  pendingCallId_[0] = '\0';

  if (isError) *isError = !r.ok;
  if (resultOut && resultLen) {
    if (r.ok) {
      snprintf(resultOut, resultLen, "image saved: %s", r.path);
    } else {
      snprintf(resultOut, resultLen, "%s",
               r.error[0] ? r.error : "image generation failed");
    }
  }
  Serial.printf("[tool] generate_image done err=%d result=%s\n", !r.ok ? 1 : 0,
                resultOut && resultOut[0] ? resultOut
                : (r.ok ? r.path : r.error));
  return true;
}
