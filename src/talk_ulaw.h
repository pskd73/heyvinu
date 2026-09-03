#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * G.711 μ-law uplink encoder: 16 kHz PCM16 in, 8 kHz μ-law out.
 *
 * ElevenLabs takes ulaw_8000 as a user input format and it costs the uplink a
 * quarter of what pcm_16000 does — 1 byte per sample at half the rate instead
 * of 2, so 1.33 base64 bytes per sample at 8 kHz against 2.67 at 16 kHz, about
 * 11 KB/s rather than 43 KB/s. That headroom is the point: the Arduino LWIP
 * prebuilt gives us CONFIG_LWIP_TCP_SND_BUF_DEFAULT / CONFIG_LWIP_TCP_MSS =
 * 5760 / 1436 = four segments in flight, and the PCM path kept all four
 * occupied at all times, so any lost segment stalled the socket into the
 * WiFiClientSecure write timeout and a bare close(). 8 kHz μ-law is also the
 * format telephony ASR has always run on, so transcription is not the trade.
 *
 * Halving the rate means everything above 4 kHz has to be gone before samples
 * are discarded, or it folds down into the speech band at full level. Hence
 * the FIR; state carries across calls so chunk edges stay continuous, which
 * means a session has to reset it.
 */

/** Drop the decimation filter history. Call when a session starts. */
void talkUlawReset();

/**
 * Decode μ-law to PCM16, one sample per input byte, for the DOWNLINK.
 *
 * agent_output_audio_format is a property of the agent, not of the connection:
 * the client-side tts override allowlist is model_id / voice_id /
 * supported_voices / stability / speed / similarity_boost /
 * pronunciation_dictionary_locators, so asking for ulaw_8000 over the socket
 * earns an override_error and a 1008 close. This exists so the firmware can
 * play whatever conversation_initiation_metadata reports, and the format is
 * flipped on the agent instead.
 *
 * Stateless, unlike the encoder — expanding μ-law to linear is a per-sample
 * map with no filter history. Rate conversion up to the playback rate is a
 * separate step.
 */
size_t talkUlawDecode(const uint8_t *in, size_t nIn, int16_t *out);

/**
 * Encode a block of 16 kHz PCM as 8 kHz μ-law, returning the bytes written.
 *
 * Consumes nIn samples and produces nIn / 2 bytes, so out needs that much
 * room. nIn is expected to be even; an odd tail is carried in the filter phase
 * so the output grid does not shift for later blocks.
 */
size_t talkUlawEncodeFrom16k(const int16_t *in, size_t nIn, uint8_t *out);
