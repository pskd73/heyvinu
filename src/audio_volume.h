#pragma once

#include <stdint.h>

/** Software speaker gain (Settings volume 0–100). */
namespace ChitramAudio {
static constexpr int16_t kVolumeMin = 0;
static constexpr int16_t kVolumeMax = 100;
static constexpr int16_t kVolumeDefault = 75;
/** Gain at 100% — default 75% ≈ 1.5× (previous fixed gain). */
static constexpr float kGainAtFull = 2.0f;

void initVolume();
void setVolumePercent(int16_t percent);
int16_t volumePercent();
float volume01();
int16_t applyVolume(int16_t sample);
} // namespace ChitramAudio
