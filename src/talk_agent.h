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

/**
 * Select: mute agent playback and wait for ElevenLabs barge-in.
 * Clears the play ring only (I2S stays up). Speak after pressing — the server
 * sends `interruption` when it hears you; that is the official interrupt path.
 * (`user_activity` is not used; it does not cancel in-flight TTS.)
 */
void talkAgentUserActivity();

float talkAgentPlayLevel();
int talkAgentWaveBars(uint8_t *out, int maxBars);

/**
 * Transport health of a live session, for an at-a-glance indicator.
 *
 * Derived from timers rather than events: every setStatus() string is written
 * at a state transition, so a wedged uplink leaves the status reading
 * "Listening" right up until the socket dies. Lifecycle (never started vs.
 * dropped) stays with the caller, which already tracks it.
 */
enum class TalkHealth : uint8_t {
  Offline,    // no session running
  Connecting, // socket up, format metadata not exchanged yet
  Ok,
  Degraded, // uplink slipping behind
  Stuck,    // uplink wedged; the session dies if this holds
};

TalkHealth talkAgentHealth();

const char *talkAgentStatus();
const char *talkAgentLastUser();
const char *talkAgentLastReply();
