#include "talk_aec.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

namespace {

/** ~0.75 s of block history, comfortably past the I2S round trip. */
constexpr size_t kHist = 48;
constexpr size_t kCorrWin = 20;

/**
 * Plausible range for how far the mic trails the speaker. I2S buffers
 * dma_buf_count * dma_buf_len frames on TX and again on RX, so with 8 x 256
 * the echo lands roughly 8 blocks back; the range covers drift around that.
 */
constexpr size_t kLagMin = 1;
constexpr size_t kLagMax = 20;
constexpr size_t kDefaultLag = 8;

constexpr float kRefActive = 250.0f;  // reference RMS that counts as playback
constexpr float kEchoGain = 0.005f;   // gain while only echo is present
constexpr float kBargeGain = 0.5f;    // gain once barge-in is confirmed
constexpr float kNoiseSpeech = 1.5f;  // mic-over-noise ratio that means speech
constexpr float kBargeRatio = 3.0f;   // mic must beat predicted echo by this

/**
 * Asymmetric slew. The reference is seen the moment it is queued to the TX
 * DMA, about kDefaultLag blocks before it reaches the room, so ducking hard on
 * the way down is free and puts the gain at the floor before the echo lands. A
 * symmetric rate spent that head start and leaked a decaying burst of the
 * agent's voice at the start of every utterance. Coming back up stays gentle
 * so a brief lull in the reference cannot reopen the mic onto live echo.
 */
constexpr float kGainAttack = 0.6f;
constexpr float kGainRelease = 0.25f;

/** Consecutive blocks of evidence to open and then to close the barge-in gate. */
constexpr uint8_t kBargeAttack = 4;  // ~64 ms
constexpr uint8_t kBargeRelease = 12;

/** Keep suppressing briefly after the reference goes quiet, for the room tail. */
constexpr uint8_t kEchoHangover = 8;

constexpr float kCouplingMax = 8.0f;
constexpr float kCouplingRise = 0.30f;
constexpr float kCouplingFall = 0.01f;
constexpr float kCorrAccept = 0.5f;

/**
 * Blocks of unbroken playback before the coupling estimate is fed. refPeak
 * goes high the instant playback is queued, but the matching echo does not
 * reach the mic for about one lag, so micRms/refPeak reads far too low across
 * both edges of an utterance. Seeding on one of those onset samples left
 * coupling so small that plain echo cleared the barge-in threshold, and since
 * the estimate freezes during barge-in it could not recover. Held to the
 * nominal lag rather than kLagMax so a clipped word still yields a sample.
 */
constexpr size_t kRefSettle = kDefaultLag;

float refHist[kHist];
float micHist[kHist];
size_t histPos;
size_t lagBlocks = kDefaultLag;
float coupling;
bool couplingInit;
size_t refRun;
float noiseFloor;
float curGain = 1.0f;
uint32_t blockCount;
uint8_t bargeUp;
uint8_t bargeDown;
bool bargeActive;
uint8_t echoHold;
uint32_t lastLogMs;

float blockRms(const int16_t *x, size_t n) {
  if (!x || n == 0) return 0.0f;
  float acc = 0.0f;
  for (size_t i = 0; i < n; i++) {
    const float v = static_cast<float>(x[i]);
    acc += v * v;
  }
  return sqrtf(acc / static_cast<float>(n));
}

/** back=0 is the newest entry. */
float histAt(const float *h, size_t back) {
  const size_t idx = (histPos + kHist - 1 - (back % kHist)) % kHist;
  return h[idx];
}

/**
 * Loudest reference block anywhere in the plausible lag range.
 *
 * Detection deliberately does not trust the lag estimate: if it is off, a
 * single-lag lookup reads silence while the mic is full of echo and nothing
 * gets suppressed. Taking the window maximum means a wrong lag can only make
 * us suppress slightly too eagerly, which is the safe direction.
 */
float refWindowMax() {
  float peak = 0.0f;
  for (size_t back = kLagMin; back <= kLagMax; back++) {
    const float v = histAt(refHist, back);
    if (v > peak) peak = v;
  }
  return peak;
}

/**
 * Re-estimate the lag by correlating the two envelopes. Only called while the
 * agent is speaking and the user is not, so echo is the only thing in the mic
 * that tracks the reference.
 */
void refineLag() {
  if (blockCount < kHist) return;

  float bestScore = -2.0f;
  size_t bestLag = lagBlocks;

  for (size_t lag = kLagMin; lag <= kLagMax; lag++) {
    if (lag + kCorrWin >= kHist) break;

    float sumMic = 0.0f, sumRef = 0.0f;
    for (size_t i = 0; i < kCorrWin; i++) {
      sumMic += histAt(micHist, i);
      sumRef += histAt(refHist, i + lag);
    }
    const float meanMic = sumMic / kCorrWin;
    const float meanRef = sumRef / kCorrWin;

    float num = 0.0f, varMic = 0.0f, varRef = 0.0f;
    for (size_t i = 0; i < kCorrWin; i++) {
      const float a = histAt(micHist, i) - meanMic;
      const float b = histAt(refHist, i + lag) - meanRef;
      num += a * b;
      varMic += a * a;
      varRef += b * b;
    }
    if (varMic <= 1e-6f || varRef <= 1e-6f) continue;

    const float score = num / sqrtf(varMic * varRef);
    if (score > bestScore) {
      bestScore = score;
      bestLag = lag;
    }
  }

  if (bestScore > kCorrAccept) lagBlocks = bestLag;
}

} // namespace

void talkAecReset() {
  memset(refHist, 0, sizeof(refHist));
  memset(micHist, 0, sizeof(micHist));
  histPos = 0;
  lagBlocks = kDefaultLag;
  coupling = 0.0f;
  couplingInit = false;
  refRun = 0;
  noiseFloor = 0.0f;
  curGain = 1.0f;
  blockCount = 0;
  bargeUp = 0;
  bargeDown = 0;
  bargeActive = false;
  echoHold = 0;
  lastLogMs = 0;
}

void talkAecPushReference(const int16_t *ref, size_t count) {
  refHist[histPos] = blockRms(ref, count);
}

bool talkAecProcess(int16_t *mic, size_t count) {
  if (!mic || count == 0) return false;

  const float micRms = blockRms(mic, count);
  micHist[histPos] = micRms;
  histPos = (histPos + 1) % kHist;
  blockCount++;

  const float refPeak = refWindowMax();
  const bool refActive = refPeak > kRefActive;

  // Counts the newest reference block rather than the window max, so it both
  // waits out the leading edge and drops to zero the moment playback stops.
  if (histAt(refHist, 0) > kRefActive) {
    if (refRun < kRefSettle) refRun++;
  } else {
    refRun = 0;
  }

  if (refActive) {
    echoHold = kEchoHangover;
  } else if (echoHold > 0) {
    echoHold--;
  }
  const bool echoPossible = refActive || echoHold > 0;

  if (!echoPossible) {
    const float rate = (micRms < noiseFloor || noiseFloor <= 0.0f) ? 0.20f : 0.0015f;
    noiseFloor += rate * (micRms - noiseFloor);
    if (noiseFloor < 1.0f) noiseFloor = 1.0f;
  }

  // Track the upper envelope of the observed ratio: rise fast, fall slowly.
  // Overestimating coupling costs a little barge-in sensitivity, while
  // underestimating it lets the agent hear itself, so bias upward. Frozen
  // during confirmed barge-in, when the ratio reflects the user, not echo.
  if (refRun >= kRefSettle && !bargeActive) {
    const float observed = micRms / refPeak;
    if (!couplingInit) {
      coupling = observed;
      couplingInit = true;
    } else {
      const float rate = (observed > coupling) ? kCouplingRise : kCouplingFall;
      coupling += rate * (observed - coupling);
    }
    if (coupling > kCouplingMax) coupling = kCouplingMax;
    refineLag();
  }

  // Before the first measurement there is nothing to compare the mic against,
  // so treat everything under playback as echo instead of guessing. That only
  // withholds barge-in for the first kRefSettle blocks of a session.
  const float predictedEcho = coupling * refPeak;
  const bool loudEnough = couplingInit &&
                          micRms > predictedEcho * kBargeRatio &&
                          micRms > noiseFloor * kNoiseSpeech;

  // Barge-in needs sustained evidence, so a transient echo peak cannot open
  // the gate, and sustained absence to close it, so a pause mid-word does not
  // chop the user off.
  if (echoPossible) {
    if (loudEnough) {
      bargeDown = 0;
      if (bargeUp < kBargeAttack) bargeUp++;
      if (bargeUp >= kBargeAttack) bargeActive = true;
    } else {
      bargeUp = 0;
      if (bargeActive) {
        if (bargeDown < kBargeRelease) bargeDown++;
        if (bargeDown >= kBargeRelease) bargeActive = false;
      }
    }
  } else {
    bargeUp = 0;
    bargeDown = 0;
    bargeActive = false;
  }

  // Only echo is suppressed here. micSlotToPcm() already runs an AGC and a
  // noise gate, and gating again on top of that compressed signal clamped
  // ordinary speech as well.
  const float target = echoPossible ? (bargeActive ? kBargeGain : kEchoGain) : 1.0f;

  const float startGain = curGain;
  curGain += (target < curGain ? kGainAttack : kGainRelease) * (target - curGain);
  const float step = (curGain - startGain) / static_cast<float>(count);

  float g = startGain;
  for (size_t i = 0; i < count; i++) {
    float v = static_cast<float>(mic[i]) * g;
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    mic[i] = static_cast<int16_t>(v);
    g += step;
  }

  // Idle blocks are skipped, but a gain stuck low while the agent is quiet is
  // exactly the failure worth seeing, so that case still prints.
  const bool worthLogging = echoPossible || curGain < 0.95f || micRms > 0.0f;
  if (worthLogging && millis() - lastLogMs > 3000) {
    lastLogMs = millis();
    Serial.printf(
        "[aec] ref=%.0f mic=%.0f pred=%.0f coup=%.2f lag=%u g=%.3f barge=%d\n",
        refPeak, micRms, predictedEcho, coupling, (unsigned)lagBlocks, curGain,
        bargeActive ? 1 : 0);
  }

  return bargeActive;
}
