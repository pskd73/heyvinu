#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Client tools for ElevenLabs ConvAI.
 *
 * Independent of the WebSocket and the Ask UI: the agent module routes
 * `client_tool_call` here and sends `client_tool_result` from the return
 * value; the UI polls `talkToolsLastText()` / `talkToolsTextGen()`.
 */

/** Return false to set is_error on the tool result. */
using TalkToolFn = bool (*)(JsonObjectConst params, char *resultOut,
                            size_t resultLen);

/** Drop the registry and clear shared tool state. */
void talkToolsReset();

/** Register built-in tools (`show_text`, …). Call after reset / on session start. */
void talkToolsRegisterDefaults();

void talkToolsRegister(const char *name, TalkToolFn fn);

/**
 * Run a registered tool. Writes a short result string (always null-terminated
 * when resultLen > 0). Unknown tools fail with a message in resultOut.
 */
bool talkToolsDispatch(const char *name, JsonVariantConst params,
                       char *resultOut, size_t resultLen);

/** Latest `show_text` payload; empty string when none. */
const char *talkToolsLastText();

/** Bumps each time `show_text` lands — cheap dirty check for the UI. */
uint32_t talkToolsTextGen();

void talkToolsClearLastText();
