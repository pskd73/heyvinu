#pragma once

#include "audio_pins.h"

#include <driver/i2s.h>
#include <stdint.h>

/** Talk / Ask — ElevenLabs ConvAI duplex @ 16 kHz (heyvinu talk_*). */
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
/**
 * Sized to hold an ENTIRE agent turn, not a network hiccup.
 *
 * ElevenLabs ships a reply as fast as the socket drains, so wall-clock time is
 * no guide to how much audio is in flight: single observed frames reached
 * 489 KB of base64, about 11 s of PCM in one callback, and one 40 s answer
 * arrived inside roughly 10 s. At 12 s the ring simply could not hold a reply
 * — a single conversation logged 741344 samples of `play ring drop`, 46 s of
 * speech thrown away, which is heard as the agent skipping mid-sentence.
 *
 * The ring is the only thing standing between burst delivery and real-time
 * playback, so it has to be as long as the longest reply. A byte budget rather
 * than a duration because it stores undecoded wire bytes, so what this buys
 * depends on the negotiated format — 4 min of ulaw_8000, 1 min of pcm_16000.
 *
 * Not larger: tlsUsePsram() puts the whole mbedTLS heap in the same pool, and
 * handshakes need contiguous blocks there, so the ring cannot have all of it.
 */
#define TALK_PLAY_RING_BYTES (1920u * 1000u)
/**
 * Playback jitter buffer. ElevenLabs delivers TTS in bursts, and the
 * WebSockets library only hands a frame over once its last byte has arrived,
 * so a ~300 KB frame yields nothing playable until it is fully received and
 * base64-decoded. With no reservoir the pump zero-fills that window straight
 * into the DAC, which is what the 1-2 s pauses mid-sentence were.
 *
 * Hold this much back at the start of a turn so the next burst gap spends
 * buffer instead of silence. The cost is that much latency on the first word.
 *
 * FLUSH_MS then releases a partial buffer once nothing new has arrived for a
 * while, so the short chunk that ends a turn is not stranded waiting for a
 * fill that is never coming.
 */
#define TALK_PLAY_PRIME_MS 700
#define TALK_PLAY_PRIME_FLUSH_MS 250
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

#define I2S_SCK HeyvinuAudio::kI2sBclk
#define I2S_WS HeyvinuAudio::kI2sWs
#define I2S_DOUT HeyvinuAudio::kI2sDout
#define I2S_SD HeyvinuAudio::kI2sDin
#define I2S_PORT I2S_NUM_0
