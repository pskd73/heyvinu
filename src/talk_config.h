#pragma once

#include "audio_pins.h"

#include <driver/i2s.h>
#include <stdint.h>

/** Talk / Ask — ElevenLabs ConvAI duplex @ 16 kHz (chitram talk_*). */
#define TALK_SAMPLE_RATE 16000
#define TALK_I2S_BUF_SAMPLES 256
/**
 * 32 ms of captured audio per WS message, counted in 16 kHz ring samples. The
 * encoded frame has to stay under 1400 bytes whichever format ElevenLabs
 * negotiates — `{"user_audio_chunk":"` is 21 chars and `"}` is 2:
 *   pcm_16000: 512 samples -> 1024 B -> 1368 b64; 21 + 1368 + 2 = 1391
 *   ulaw_8000: 256 samples ->  256 B ->  344 b64; 21 +  344 + 2 =  367
 *
 * That threshold matters. Above it the WebSockets library skips its intern
 * buffer, which is the only path that generates a random mask key — larger
 * frames go out with an all-zero key and an unmasked payload, split across two
 * write() calls. Staying below it gets RFC-compliant masking and one TCP
 * segment per frame.
 *
 * 32 ms rather than something longer on the μ-law path even though the frame
 * has the room: per-frame overhead is 37 bytes of WS header plus TLS record,
 * so 31 frames/s costs ~12.5 KB/s against ~11.6 at 64 ms. Buying 1 KB/s back
 * by tripling the frame size would undo the reason for the change, which is to
 * stop keeping the four-segment send window full.
 */
#define TALK_MIC_CHUNK_SAMPLES 512
#define TALK_MIC_ULAW_CHUNK_BYTES (TALK_MIC_CHUNK_SAMPLES / 2)
#define TALK_PLAY_RING_SECONDS 12
#define TALK_PLAY_RING_SAMPLES (TALK_SAMPLE_RATE * TALK_PLAY_RING_SECONDS)
/**
 * The ws task can stall for seconds receiving a single ~300 KB agent-audio
 * frame, and no mic chunks go out while it does. The ring has to cover that
 * whole stall or pushes get dropped and the uplink goes discontinuous again.
 * It lives in PSRAM, so the headroom is cheap.
 */
#define TALK_MIC_UPLINK_SECONDS 8
#define TALK_MIC_UPLINK_SAMPLES (TALK_SAMPLE_RATE * TALK_MIC_UPLINK_SECONDS)
#define TALK_PLAY_SILENCE_PEAK 180
#define TALK_AGENT_SPEAKING_TAIL_MS 1200

#define AGC_TARGET_PEAK 7000.0f
#define AGC_GAIN_MIN 1.0f
#define AGC_GAIN_MAX 5.0f
#define AGC_GAIN_SLEW 0.0012f

#define I2S_SCK ChitramAudio::kI2sBclk
#define I2S_WS ChitramAudio::kI2sWs
#define I2S_DOUT ChitramAudio::kI2sDout
#define I2S_SD ChitramAudio::kI2sDin
#define I2S_PORT I2S_NUM_0
