#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Echo and noise suppression for the ElevenLabs uplink.
 *
 * The speaker sits next to the mic and there is no hardware AEC, so the mic
 * hears whatever the agent is saying. Uplinking that raw makes the agent
 * interrupt itself, but going silent during playback is worse: ElevenLabs
 * wants one continuous stream and drops the socket after 60 s without user
 * audio.
 *
 * A waveform-level adaptive filter is not an option here. micSlotToPcm()
 * applies AGC and a noise gate before we see a sample, so the mapping from
 * reference to echo is neither linear nor time-invariant and an NLMS filter
 * would not converge. This works on per-block energy envelopes instead, which
 * that front end leaves intact, and estimates the speaker-to-mic coupling and
 * the I2S round-trip lag at runtime.
 *
 * Call order per block, from the audio pump:
 *   talkAecPushReference(play, n);   // what is going to the speaker
 *   talkAecProcess(mic, n);         // then the captured block
 */

/** Drop the learned coupling, lag and noise floor. Call when a session starts. */
void talkAecReset();

/** Register a playback block on its way to the speaker. */
void talkAecPushReference(const int16_t *ref, size_t count);

/**
 * Suppress echo and steady background noise in a mic block, in place.
 *
 * Returns true when the user appears to be talking, including over the agent,
 * so barge-in still reaches ElevenLabs.
 */
bool talkAecProcess(int16_t *mic, size_t count);
