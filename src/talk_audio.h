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
/** Keep TX DMA fed with digital silence (avoids MAX98357 underrun noise). */
bool talkAudioWriteSilence(int samples, uint32_t timeoutMs);

/**
 * Playback jitter buffer, holding audio in the format it arrived in.
 *
 * Buffering undecoded wire bytes rather than finished PCM is what makes a
 * whole agent turn fit: ulaw_8000 expands 4x on the way to 16 kHz PCM16, so
 * decoding on the producer side spent the entire saving that choosing ulaw on
 * the wire had just bought. Pop decodes and resamples instead, which also
 * keeps that work off the ws task where it was competing with socket reads.
 */
bool talkPlayRingInit();
void talkPlayRingClear();

/**
 * Wire format of everything pushed from here on, from
 * conversation_initiation_metadata. Constant for a session, so the ring holds
 * it rather than tagging each chunk. Resets the resampler.
 */
void talkPlayRingSetFormat(int srcRate, bool ulaw);

/** Push wire bytes verbatim. Returns bytes accepted; short means overflow. */
size_t talkPlayRingPush(const uint8_t *data, size_t nbytes);

/** Decode and resample up to maxCount samples at TALK_SAMPLE_RATE. */
size_t talkPlayRingPop(int16_t *out, size_t maxCount);

/**
 * Buffered playback time. In ms rather than samples so callers never have to
 * know the storage format — the units stay right if the format changes.
 */
uint32_t talkPlayRingUsedMs();

/** Distinct from UsedMs() == 0: one ulaw byte is 0.125 ms and still audible. */
bool talkPlayRingEmpty();

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
