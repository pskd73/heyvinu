#pragma once

#include <ArduinoJson.h>
#include <stddef.h>

class Storage;

/**
 * ElevenLabs client-tool adapter for image generation (ElevenLabs Flows or
 * OpenRouter via image_gen).
 *
 * Kept separate from backend HTTP modules and from `talk_tools`
 * builtins (`show_text`). Register once per talk session; poll from the WS
 * task to deliver `client_tool_result` when the job finishes.
 */

void talkImageToolReset();
void talkImageToolSetStorage(Storage *storage);
void talkImageToolRegister();

/**
 * Handle `generate_image`. Returns:
 *  - true + *pending=true  → job started; do not send tool result yet
 *  - true + *pending=false → sync (unused)
 *  - false                 → failed; resultOut has error for immediate reply
 */
bool talkImageToolStart(const char *toolCallId, JsonObjectConst params,
                        char *resultOut, size_t resultLen, bool *pending);

/**
 * If a generate_image job completed, fills outs and returns true (one-shot).
 */
bool talkImageToolTakeResult(char *callIdOut, size_t callIdLen,
                             char *resultOut, size_t resultLen, bool *isError);
