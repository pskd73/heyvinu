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

/**
 * Speaker-write accounting. i2s_write() reports a short write only through its
 * `written` argument — it still returns ESP_OK — so samples lost to a full TX
 * DMA leave no other trace. Those samples have already been popped off the
 * play ring by then, which makes this a permanent hole in the audio.
 */
uint32_t talkAudioWriteDropped();
uint32_t talkAudioWriteStalls();
uint32_t talkAudioWriteMaxMs();
void talkAudioResetWriteStats();
