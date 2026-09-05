#include "voice_tools.h"

#include <Arduino.h>
#include <string.h>

namespace {

constexpr uint8_t kMaxTools = 8;
constexpr size_t kLastTextLen = 128;

struct Entry {
  const char *name;
  VoiceToolFn fn;
};

Entry tools_[kMaxTools] = {};
uint8_t toolCount_ = 0;

char lastText_[kLastTextLen] = {};
uint32_t textGen_ = 0;

bool toolShowText(JsonObjectConst params, char *resultOut, size_t resultLen) {
  const char *text = params["text"] | "";
  if (!text[0]) {
    // Also accept a bare "message" in case the agent prompt uses that name.
    text = params["message"] | "";
  }
  if (!text[0]) {
    snprintf(resultOut, resultLen, "missing text");
    return false;
  }
  strncpy(lastText_, text, kLastTextLen - 1);
  lastText_[kLastTextLen - 1] = '\0';
  textGen_++;
  Serial.printf("[tool] show_text: %s\n", lastText_);
  snprintf(resultOut, resultLen, "ok");
  return true;
}

} // namespace

void voiceToolsReset() {
  toolCount_ = 0;
  voiceToolsClearLastText();
}

void voiceToolsRegister(const char *name, VoiceToolFn fn) {
  if (!name || !name[0] || !fn || toolCount_ >= kMaxTools) return;
  for (uint8_t i = 0; i < toolCount_; i++) {
    if (tools_[i].name && !strcmp(tools_[i].name, name)) {
      tools_[i].fn = fn;
      return;
    }
  }
  tools_[toolCount_].name = name;
  tools_[toolCount_].fn = fn;
  toolCount_++;
}

void voiceToolsRegisterDefaults() {
  voiceToolsRegister("show_text", toolShowText);
}

bool voiceToolsDispatch(const char *name, JsonVariantConst params,
                       char *resultOut, size_t resultLen) {
  if (resultOut && resultLen) resultOut[0] = '\0';
  if (!name || !name[0]) {
    snprintf(resultOut, resultLen, "missing tool name");
    return false;
  }
  for (uint8_t i = 0; i < toolCount_; i++) {
    if (!tools_[i].name || strcmp(tools_[i].name, name) != 0) continue;
    JsonObjectConst obj = params.as<JsonObjectConst>();
    return tools_[i].fn(obj, resultOut, resultLen);
  }
  snprintf(resultOut, resultLen, "unknown tool: %s", name);
  Serial.printf("[tool] unknown: %s\n", name);
  return false;
}

const char *voiceToolsLastText() { return lastText_; }

uint32_t voiceToolsTextGen() { return textGen_; }

void voiceToolsClearLastText() {
  lastText_[0] = '\0';
  textGen_++;
}
