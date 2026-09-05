#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

class Storage;

/** Max stored conversation text (PSRAM). Internal DRAM stays small. */
constexpr size_t kVoiceContextCap = 5 * 1024;
/** Soft cap when reinjecting history into a new session (EL or DG). */
constexpr size_t kVoiceContextInjectMax = 1800;

/**
 * Per-agent conversation continuity on the SD card.
 *
 * Neither ElevenLabs nor Deepgram resume a live WebSocket across reconnects.
 * Continuity means: save recent turns, open a new session, reinject history.
 *
 * - ElevenLabs: plain transcript via dynamic_variables.conversation_context
 * - Deepgram: structured agent.context.messages History entries
 *
 * Path: /chitram/context/<agent_id>.json + .txt  (card-rooted)
 * Text lives in PSRAM; only a pointer + metadata sit in internal RAM.
 *
 * On-disk text format (shared): lines `User: …\n` / `Agent: …\n`.
 */
struct VoiceContext {
  char conversationId[48] = {};
  char *text = nullptr; // PSRAM, kVoiceContextCap bytes
  size_t len = 0;
  uint32_t updatedMs = 0;
};

enum class VoiceTurnRole : uint8_t { User, Agent };

bool voiceContextEnsure(VoiceContext *state);
void voiceContextReset(VoiceContext *state);
void voiceContextFree(VoiceContext *state);

bool voiceContextLoad(Storage *storage, const char *agentId, VoiceContext *out);
bool voiceContextSave(Storage *storage, const char *agentId,
                      const VoiceContext &state);
/**
 * Delete on-disk context for `agentId` (.json + .txt).
 * Returns true if nothing remained or files were removed.
 */
bool voiceContextClear(Storage *storage, const char *agentId);

/**
 * List agent ids that have a context `.json` under /chitram/context.
 * Writes up to `maxCount` NUL-terminated names (no `.json`) into `names[][nameLen]`.
 * Returns count written.
 */
int voiceContextList(Storage *storage, char *names, size_t nameLen, int maxCount);

/**
 * Append a labeled turn (`User: …\n` / `Agent: …\n`). Drops from the front
 * when full so the newest turns remain.
 */
void voiceContextAppend(VoiceContext *state, VoiceTurnRole role, const char *text);

/**
 * Copy the newest portion of the transcript into `out` (for ElevenLabs
 * conversation_context). Truncates on a line boundary to ≤ maxChars.
 * Returns bytes written (excluding NUL).
 */
size_t voiceContextRecentText(const VoiceContext &state, char *out, size_t outCap,
                              size_t maxChars = kVoiceContextInjectMax);

/**
 * Parse stored `User:` / `Agent:` lines into Deepgram History objects on
 * `messages`. Stops after ~maxChars of content. Returns number of messages added.
 */
size_t voiceContextFillDgHistory(const VoiceContext &state, JsonArray messages,
                                 size_t maxChars = kVoiceContextInjectMax);

/** Default SD key when using the Deepgram Voice Agent (no EL agent id). */
constexpr const char *kVoiceContextDeepgramId = "deepgram";
