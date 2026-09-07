#include "audio_volume.h"

#include <Arduino.h>
#include <math.h>

namespace HeyvinuAudio {
namespace {

int16_t gVolumePercent = kVolumeDefault;
float gGain = (static_cast<float>(kVolumeDefault) / 100.0f) * kGainAtFull;

int16_t clampPercent(int16_t p) {
  if (p < kVolumeMin) return kVolumeMin;
  if (p > kVolumeMax) return kVolumeMax;
  return p;
}

} // namespace

void setVolumePercent(int16_t percent) {
  gVolumePercent = clampPercent(percent);
  gGain = (static_cast<float>(gVolumePercent) / 100.0f) * kGainAtFull;
}

int16_t volumePercent() { return gVolumePercent; }

float volume01() { return gGain; }

void initVolume() {
  Serial.printf("Volume: %d%% gain=%.2f\n", (int)gVolumePercent, gGain);
}

int16_t applyVolume(int16_t sample) {
  const int32_t v =
      static_cast<int32_t>(lroundf(static_cast<float>(sample) * gGain));
  if (v > 32767) {
    return 32767;
  }
  if (v < -32768) {
    return -32768;
  }
  return static_cast<int16_t>(v);
}

} // namespace HeyvinuAudio
