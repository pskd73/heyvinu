#pragma once

#include <stddef.h>
#include <stdint.h>

class Storage;

/** Max stored conversation text (PSRAM). Internal DRAM stays small. */
constexpr size_t kTalkContextCap = 5 * 1024;

/**
 * Per-agent conversation continuity on the SD card.
 *
 * ElevenLabs cannot resume a WebSocket conversation_id. Continuity means:
 * save recent turns, start a new session, and reinject via dynamic_variables
 * (conversation_context / previous_conversation_id).
 *
 * Path: /chitram/context/<agent_id>.json + .txt  (card-rooted)
 * Text lives in PSRAM; only a pointer + metadata sit in internal RAM.
 */
struct TalkContext {
  char conversationId[48] = {};
  char *text = nullptr; // PSRAM, kTalkContextCap bytes
  size_t len = 0;
  uint32_t updatedMs = 0;
};

enum class TalkTurnRole : uint8_t { User, Agent };

bool talkContextEnsure(TalkContext *state);
void talkContextReset(TalkContext *state);
void talkContextFree(TalkContext *state);

bool talkContextLoad(Storage *storage, const char *agentId, TalkContext *out);
bool talkContextSave(Storage *storage, const char *agentId,
                     const TalkContext &state);
bool talkContextClear(Storage *storage, const char *agentId);

/**
 * Append a labeled turn (`User: …\n` / `Agent: …\n`). Drops from the front
 * when full so the newest turns remain.
 */
void talkContextAppend(TalkContext *state, TalkTurnRole role, const char *text);
