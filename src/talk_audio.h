#pragma once

#include <stddef.h>
#include <stdint.h>

bool talkAudioInit();
void talkAudioStop();

bool talkAudioStartDuplex();
void talkAudioStopI2s();

void talkAudioResetDsp();

int talkAudioReadPcmTimeout(int16_t *out, int maxSamples, uint32_t timeoutMs);
bool talkAudioWritePcmTimeout(const int16_t *data, int samples,
                              uint32_t timeoutMs);

bool talkPlayRingInit();
void talkPlayRingClear();
size_t talkPlayRingPush(const int16_t *data, size_t count);
size_t talkPlayRingPop(int16_t *out, size_t maxCount);
size_t talkPlayRingUsed();
