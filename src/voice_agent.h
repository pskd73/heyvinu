#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

class AppHost;

/**
 * Voice session facade — routes to ElevenLabs ConvAI or Deepgram Voice Agent
 * based on Config::VoiceProvider (`elevenlabs` | `deepgram`).
 */

enum class VoiceProvider : uint8_t {
  ElevenLabs = 0,
  Deepgram = 1,
};

struct VoiceStartOpts {
  AppHost *host = nullptr;
  /** ElevenLabs ConvAI agent id; ignored for Deepgram. */
  const char *agentId = nullptr;
};

VoiceProvider voiceProvider();

bool voiceStart(const VoiceStartOpts &opts = {});
void voiceStop();
void voiceLoop();
bool voiceActive();
bool voiceReady();

const char *voiceStatus();
const char *voiceLastUser();
const char *voiceLastReply();
uint32_t voiceGen();

bool voiceSupportsAgentPicker();
bool voiceSupportsTools(); // both true; registry is shared (EL wired; DG soon)

void voiceUserActivity();
