#include "talk_ulaw.h"

#include <string.h>

namespace {

/**
 * Anti-alias lowpass ahead of the 2:1 decimation — a 23-tap Hamming-windowed
 * sinc at fs/4, in Q15.
 *
 * Half-band, so every second tap either side of centre is zero and the centre
 * tap is half the total. The taps sum to exactly 32768, which makes DC gain
 * unity with no run-time normalisation. Measured response: within 0.2 dB to
 * 3 kHz, -1.1 dB at 3.4 kHz, -6 dB at the 4 kHz fold point, -33 dB at 5 kHz
 * and -54 dB at 6 kHz. The passband flatness is what matters for fricatives;
 * a short binomial kernel would have been ~6 dB down at 3 kHz.
 *
 * 23 MACs per output at 8000 outputs/s is ~184 k MACs/s, noise next to the AEC.
 */
constexpr int kTaps = 23;
constexpr int16_t kFir[kTaps] = {
    -76, 0, 177, 0, -521, 0, 1266, 0, -2932, 0, 10259,
    16422,
    10259, 0, -2932, 0, 1266, 0, -521, 0, 177, 0, -76};

/** Newest sample lives at kTaps - 1; the FIR is symmetric so order is free. */
int16_t hist[kTaps];
int phase;

/**
 * G.711 μ-law: sign bit, 3-bit segment exponent, 4-bit mantissa, complemented
 * on the wire.
 *
 * The 0x84 bias puts the segment boundaries on powers of two, which is what
 * lets the exponent fall out of the position of the top set bit instead of a
 * lookup table. Biasing is also why the clip is 32635 rather than 32767 — it
 * keeps the sum inside 15 bits. Anchors: 0 -> 0xFF, -1 -> 0x7F, positive full
 * scale -> 0x80, negative full scale -> 0x00.
 */
inline uint8_t linearToUlaw(int32_t pcm) {
  const uint8_t sign = (pcm < 0) ? 0x80 : 0x00;
  if (pcm < 0) pcm = -pcm;
  if (pcm > 32635) pcm = 32635;
  const uint32_t biased = (uint32_t)pcm + 0x84;
  const int exponent = 31 - __builtin_clz(biased) - 7;
  const int mantissa = (int)(biased >> (exponent + 3)) & 0x0F;
  return (uint8_t)~(sign | (uint8_t)(exponent << 4) | (uint8_t)mantissa);
}

}  // namespace

void talkUlawReset() {
  memset(hist, 0, sizeof(hist));
  phase = 0;
}

size_t talkUlawEncodeFrom16k(const int16_t *in, size_t nIn, uint8_t *out) {
  if (!in || !out) return 0;

  size_t produced = 0;
  for (size_t i = 0; i < nIn; i++) {
    memmove(hist, hist + 1, (kTaps - 1) * sizeof(int16_t));
    hist[kTaps - 1] = in[i];

    phase ^= 1;
    if (phase) continue;

    // Worst case is 32768 * sum|kFir| = 1.54e9, inside int32.
    int32_t acc = 0;
    for (int t = 0; t < kTaps; t++) {
      acc += (int32_t)kFir[t] * (int32_t)hist[t];
    }
    acc = (acc + (1 << 14)) >> 15;
    if (acc > 32767) acc = 32767;
    if (acc < -32768) acc = -32768;
    out[produced++] = linearToUlaw(acc);
  }
  return produced;
}
