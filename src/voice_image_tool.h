#pragma once

#include <ArduinoJson.h>
#include <stddef.h>

class AppHost;
class Storage;

/**
 * Image-generation tool adapter (ElevenLabs Flows or OpenRouter via image_gen).
 *
 * Kept separate from backend HTTP modules and from `voice_tools` builtins
 * (`show_text`). Register once per voice session; the active agent polls and
 * delivers the provider-specific tool result when the job finishes.
 */

void voiceImageToolReset();
void voiceImageToolSetStorage(Storage *storage);
/** Optional — used to show a Shell toast when generation starts. */
void voiceImageToolSetHost(AppHost *host);
void voiceImageToolRegister();

/**
 * Handle `generate_image`. Returns:
 *  - true + *pending=true  → job started; do not send tool result yet
 *  - true + *pending=false → sync (unused)
 *  - false                 → failed; resultOut has error for immediate reply
 */
bool voiceImageToolStart(const char *toolCallId, JsonObjectConst params,
                        char *resultOut, size_t resultLen, bool *pending);

/**
 * If a generate_image job completed, fills outs and returns true (one-shot).
 */
bool voiceImageToolTakeResult(char *callIdOut, size_t callIdLen,
                             char *resultOut, size_t resultLen, bool *isError);
