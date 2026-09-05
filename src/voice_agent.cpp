#include "voice_agent.h"

#include "deepgram_agent.h"
#include "elevenlabs_agent.h"
#include "runtime_config.h"

#include <string.h>

VoiceProvider voiceProvider() {
  const char *v = getConfig(Config::VoiceProvider);
  if (v && (strcmp(v, "deepgram") == 0 || strcmp(v, "dg") == 0)) {
    return VoiceProvider::Deepgram;
  }
  return VoiceProvider::ElevenLabs;
}

bool voiceStart(const VoiceStartOpts &opts) {
  if (voiceProvider() == VoiceProvider::Deepgram) {
    return dgAgentStart(opts.host);
  }
  return elAgentStart(opts.host, opts.agentId);
}

void voiceStop() {
  // Stop both so a provider flip mid-session cannot leave a zombie.
  dgAgentStop();
  elAgentStop();
}

void voiceLoop() {
  if (voiceProvider() == VoiceProvider::ElevenLabs) {
    elAgentLoop();
  }
}

bool voiceActive() {
  return dgAgentActive() || elAgentIsActive();
}

bool voiceReady() {
  if (dgAgentActive()) return dgAgentReady();
  if (elAgentIsActive()) return elAgentIsReady();
  return false;
}

bool voiceTakeEndedByAgent() { return elAgentTakeEndedByAgent(); }

const char *voiceStatus() {
  if (dgAgentActive()) return dgAgentStatus();
  if (elAgentIsActive()) return elAgentStatus();
  return "Idle";
}

const char *voiceLastUser() {
  if (dgAgentActive()) return dgAgentUserText();
  if (elAgentIsActive()) return elAgentLastUser();
  return "";
}

const char *voiceLastReply() {
  if (dgAgentActive()) return dgAgentAgentText();
  if (elAgentIsActive()) return elAgentLastReply();
  return "";
}

uint32_t voiceGen() {
  // Prefer whichever session is live; EL has no gen counter — synthesize 0.
  if (dgAgentActive()) return dgAgentGen();
  return 0;
}

bool voiceSupportsAgentPicker() {
  return voiceProvider() == VoiceProvider::ElevenLabs;
}

bool voiceSupportsTools() { return true; }

void voiceUserActivity() {
  if (voiceProvider() == VoiceProvider::ElevenLabs) {
    elAgentUserActivity();
  }
}
