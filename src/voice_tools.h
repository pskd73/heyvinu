#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Shared client-tool registry for voice agents (ElevenLabs + Deepgram).
 *
 * Independent of the WebSocket and the Ask UI: each agent module routes its
 * provider-specific tool call into `voiceToolsDispatch` and sends the
 * provider-specific result frame. The UI polls `voiceToolsLastText()` /
 * `voiceToolsTextGen()`.
 */

/** Return false to set is_error on the tool result. */
using VoiceToolFn = bool (*)(JsonObjectConst params, char *resultOut,
                            size_t resultLen);

/** Drop the registry and clear shared tool state. */
void voiceToolsReset();

/** Register built-in tools (`show_text`, …). Call after reset / on session start. */
void voiceToolsRegisterDefaults();

void voiceToolsRegister(const char *name, VoiceToolFn fn);

/**
 * Run a registered tool. Writes a short result string (always null-terminated
 * when resultLen > 0). Unknown tools fail with a message in resultOut.
 */
bool voiceToolsDispatch(const char *name, JsonVariantConst params,
                       char *resultOut, size_t resultLen);

/** Latest `show_text` payload; empty string when none. */
const char *voiceToolsLastText();

/** Bumps each time `show_text` lands — cheap dirty check for the UI. */
uint32_t voiceToolsTextGen();

void voiceToolsClearLastText();
