#pragma once

#include <stdbool.h>
#include <stdint.h>

class AppHost;

bool talkAgentStart(AppHost *host = nullptr);
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
