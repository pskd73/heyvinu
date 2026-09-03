#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

class AppHost;

/** One agent from the account listing, trimmed to what the picker shows. */
struct TalkAgentInfo {
  char id[48];
  char name[40];
};

/**
 * List the account's ConvAI agents. Returns the number written to `out`, or
 * -1 with a short reason in `errOut`. `hasMore` reports that the account has
 * agents beyond `maxCount`.
 */
int talkAgentFetchList(TalkAgentInfo *out, int maxCount, bool *hasMore,
                       char *errOut, size_t errLen);

/** `agentId` null or empty falls back to the configured default. */
bool talkAgentStart(AppHost *host = nullptr, const char *agentId = nullptr);
void talkAgentLoop();
void talkAgentStop();
bool talkAgentIsActive();
bool talkAgentIsReady();
bool talkAgentIsSpeaking();

float talkAgentPlayLevel();
int talkAgentWaveBars(uint8_t *out, int maxBars);

const char *talkAgentStatus();
const char *talkAgentLastUser();
const char *talkAgentLastReply();
