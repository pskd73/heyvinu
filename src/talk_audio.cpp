#include "talk_audio.h"
#include "talk_config.h"
#include "talk_ulaw.h"
#include "audio_volume.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <esp_heap_caps.h>
#include "driver/i2s.h"

static const float GATE_OPEN = 250.0f;
static const float GATE_FULL = 700.0f;

enum TalkI2sMode { TALK_I2S_NONE, TALK_I2S_DUPLEX };
static TalkI2sMode i2sMode = TALK_I2S_NONE;
static portMUX_TYPE i2sMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t i2sActiveOps = 0;

static bool i2sBeginOp() {
  portENTER_CRITICAL(&i2sMux);
  if (i2sMode != TALK_I2S_DUPLEX) {
    portEXIT_CRITICAL(&i2sMux);
    return false;
  }
  i2sActiveOps++;
  portEXIT_CRITICAL(&i2sMux);
  return true;
}

static void i2sEndOp() {
  portENTER_CRITICAL(&i2sMux);
  if (i2sActiveOps > 0) {
    i2sActiveOps--;
  }
  portEXIT_CRITICAL(&i2sMux);
}

static float dspDcX = 0, dspDcY = 0;
static float dspHpfX = 0, dspHpfY = 0;
static float dspLpfY = 0;
static float dspEnv = 0;
static float dspPeakEma = 200000.0f;
static float dspGain = 2.0f;
static int userGain = 4;

static uint8_t *playRing = nullptr;
static size_t playCap = 0, playHead = 0, playTail = 0, playUsed = 0;
static portMUX_TYPE playMux = portMUX_INITIALIZER_UNLOCKED;

// Wire format of the bytes currently in the ring.
static int playInRate = TALK_SAMPLE_RATE;
static size_t playInBytes = 2;

/**
 * Pop-side linear resampler. Q16 phase, so any source rate works rather than
 * just the 2x that ulaw_8000 happens to need.
 *
 * prev/cur persist across pops on purpose: the old producer-side resampler
 * clamped to the end of each chunk and so never interpolated across a chunk
 * boundary, putting a small discontinuity at every seam.
 */
static uint32_t playRsStep = 1u << 16;
static int16_t playRsPrev = 0, playRsCur = 0;
static uint32_t playRsFrac = 0;
static bool playRsPrimed = false;

static int32_t i2sRaw[TALK_I2S_BUF_SAMPLES];

static uint32_t playWriteDropped = 0;
static uint32_t playWriteStalls = 0;
static uint32_t playWriteMaxMs = 0;

void talkAudioResetDsp() {
  dspDcX = dspDcY = 0;
  dspHpfX = dspHpfY = 0;
  dspLpfY = 0;
  dspEnv = 0;
  dspPeakEma = 200000.0f;
  dspGain = 2.0f;
}

static int16_t micSlotToPcm(int32_t raw) {
  float x = (float)(raw >> 8);

  float y = x - dspDcX + 0.995f * dspDcY;
  dspDcX = x;
  dspDcY = y;

  float hp = 0.961f * (dspHpfY + y - dspHpfX);
  dspHpfX = y;
  dspHpfY = hp;

  dspLpfY += 0.45f * (hp - dspLpfY);
  float band = dspLpfY;

  float absv = fabsf(band);
  if (absv > dspPeakEma) {
    dspPeakEma = 0.85f * dspPeakEma + 0.15f * absv;
  } else {
    dspPeakEma = 0.9997f * dspPeakEma + 0.0003f * absv;
  }

  float desired = (AGC_TARGET_PEAK * 256.0f) / (dspPeakEma + 1.0f);
  if (desired < AGC_GAIN_MIN) desired = AGC_GAIN_MIN;
  if (desired > AGC_GAIN_MAX) desired = AGC_GAIN_MAX;
  dspGain += AGC_GAIN_SLEW * (desired - dspGain);

  float out = band * dspGain / 256.0f;
  out *= (0.5f + (float)userGain * 0.15f);

  float a = fabsf(out);
  dspEnv = 0.95f * dspEnv + 0.05f * a;
  if (dspEnv < GATE_FULL) {
    float g = (dspEnv - GATE_OPEN) / (GATE_FULL - GATE_OPEN);
    if (g < 0.0f) g = 0.0f;
    if (g > 1.0f) g = 1.0f;
    out *= g;
  }

  if (out > 28000.0f) out = 28000.0f + (out - 28000.0f) * 0.25f;
  else if (out < -28000.0f) out = -28000.0f + (out + 28000.0f) * 0.25f;
  if (out > 32767.0f) out = 32767.0f;
  if (out < -32768.0f) out = -32768.0f;
  return (int16_t)out;
}

void talkAudioStopI2s() {
  portENTER_CRITICAL(&i2sMux);
  if (i2sMode == TALK_I2S_NONE) {
    portEXIT_CRITICAL(&i2sMux);
    return;
  }
  i2sMode = TALK_I2S_NONE;
  portEXIT_CRITICAL(&i2sMux);

  for (int i = 0; i < 100 && i2sActiveOps > 0; i++) {
    delay(5);
  }
  i2s_driver_uninstall(I2S_PORT);
}

bool talkAudioStartDuplex() {
  talkAudioStopI2s();
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
  cfg.sample_rate = TALK_SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  // 8 x 256 frames = 128 ms per direction. The RX cushion is what protects
  // mic samples when the audio task is late; at 4 buffers a 64 ms stall was
  // enough for the driver to start overwriting captured audio.
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = TALK_I2S_BUF_SAMPLES;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = I2S_SCK;
  pins.ws_io_num = I2S_WS;
  pins.data_out_num = I2S_DOUT;
  pins.data_in_num = I2S_SD;

  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) return false;
  if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
    i2s_driver_uninstall(I2S_PORT);
    return false;
  }
  i2s_zero_dma_buffer(I2S_PORT);
  i2sMode = TALK_I2S_DUPLEX;
  Serial.printf("Talk I2S duplex @%u BCLK=%d WS=%d DIN=%d DOUT=%d\n",
                (unsigned)TALK_SAMPLE_RATE, I2S_SCK, I2S_WS, I2S_SD, I2S_DOUT);
  return true;
}

int talkAudioReadPcmTimeout(int16_t *out, int maxSamples, uint32_t timeoutMs) {
  if (!i2sBeginOp()) return 0;
  size_t bytesRead = 0;
  TickType_t ticks = (timeoutMs == 0) ? 0 : pdMS_TO_TICKS(timeoutMs);
  const esp_err_t err =
      i2s_read(I2S_PORT, i2sRaw, sizeof(i2sRaw), &bytesRead, ticks);
  i2sEndOp();
  if (err != ESP_OK) return 0;
  if (bytesRead < sizeof(int32_t)) return 0;
  int n = (int)(bytesRead / sizeof(int32_t));
  if (n > maxSamples) n = maxSamples;
  for (int i = 0; i < n; i++) {
    out[i] = micSlotToPcm(i2sRaw[i]);
  }
  return n;
}

bool talkAudioWritePcmTimeout(const int16_t *data, int samples,
                              uint32_t timeoutMs) {
  if (!i2sBeginOp()) return false;
  TickType_t ticks = (timeoutMs == UINT32_MAX) ? portMAX_DELAY
                     : (timeoutMs == 0)         ? 0
                                                : pdMS_TO_TICKS(timeoutMs);
  static int32_t packed[TALK_I2S_BUF_SAMPLES];
  int left = samples;
  int off = 0;
  bool ok = true;
  const uint32_t startMs = millis();
  while (left > 0) {
    int n = left > TALK_I2S_BUF_SAMPLES ? TALK_I2S_BUF_SAMPLES : left;
    for (int i = 0; i < n; i++) {
      const int16_t s = ChitramAudio::applyVolume(data[off + i]);
      packed[i] = ((int32_t)s) << 16;
    }
    size_t written = 0;
    if (i2s_write(I2S_PORT, packed, (size_t)n * sizeof(int32_t), &written,
                  ticks) != ESP_OK) {
      ok = false;
      break;
    }
    // A timeout with the TX DMA full still returns ESP_OK, so the shortfall
    // shows up here and nowhere else. Report it rather than claiming the whole
    // block went out: the caller has already consumed these samples.
    const int wrote = (int)(written / sizeof(int32_t));
    off += wrote;
    left -= wrote;
    if (wrote < n) {
      playWriteDropped += (uint32_t)(n - wrote);
      playWriteStalls++;
      ok = false;
      break;
    }
  }
  const uint32_t elapsed = millis() - startMs;
  if (elapsed > playWriteMaxMs) playWriteMaxMs = elapsed;
  i2sEndOp();
  return ok;
}

uint32_t talkAudioWriteDropped() { return playWriteDropped; }
uint32_t talkAudioWriteStalls() { return playWriteStalls; }
uint32_t talkAudioWriteMaxMs() { return playWriteMaxMs; }

void talkAudioResetWriteStats() {
  playWriteDropped = 0;
  playWriteStalls = 0;
  playWriteMaxMs = 0;
}

static void playResetResampler() {
  playRsPrev = playRsCur = 0;
  playRsFrac = 0;
  playRsPrimed = false;
}

bool talkPlayRingInit() {
  if (playRing) return true;
  // Halve rather than fail: at ~1.9 MB this block can lose to PSRAM
  // fragmentation, and a short ring still holds a conversation — it only
  // clips long replies the way the 12 s ring did. There is no internal-RAM
  // fallback because nothing this size would ever fit there.
  for (size_t cap = TALK_PLAY_RING_BYTES; cap >= 128u * 1024u; cap /= 2) {
    playRing = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!playRing) continue;
    playCap = cap;
    playHead = playTail = playUsed = 0;
    playResetResampler();
    if (cap != TALK_PLAY_RING_BYTES) {
      Serial.printf("Talk play ring short: %uKB (wanted %uKB)\n",
                    (unsigned)(cap / 1024),
                    (unsigned)(TALK_PLAY_RING_BYTES / 1024));
    }
    return true;
  }
  playCap = 0;
  return false;
}

void talkPlayRingClear() {
  portENTER_CRITICAL(&playMux);
  playHead = playTail = playUsed = 0;
  portEXIT_CRITICAL(&playMux);
  playResetResampler();
}

void talkPlayRingSetFormat(int srcRate, bool ulaw) {
  if (srcRate <= 0) srcRate = TALK_SAMPLE_RATE;
  talkPlayRingClear();
  playInRate = srcRate;
  playInBytes = ulaw ? 1 : 2;
  playRsStep =
      (uint32_t)(((uint64_t)srcRate << 16) / (uint64_t)TALK_SAMPLE_RATE);
  if (!playRsStep) playRsStep = 1;
}

size_t talkPlayRingPush(const uint8_t *data, size_t nbytes) {
  if (!playRing || !data || !nbytes) return 0;

  portENTER_CRITICAL(&playMux);
  const size_t head = playHead;
  const size_t room = playCap - playUsed;
  portEXIT_CRITICAL(&playMux);

  const size_t n = (nbytes < room) ? nbytes : room;
  if (!n) return 0;

  // Copied outside the lock deliberately. The consumer never reads past
  // playUsed, so the free span belongs to this task alone, and a single agent
  // frame runs to hundreds of KB — memcpy of that with interrupts disabled
  // would starve I2S and the radio.
  const size_t contig = playCap - head;
  const size_t first = (n < contig) ? n : contig;
  memcpy(playRing + head, data, first);
  if (n > first) memcpy(playRing, data + first, n - first);

  portENTER_CRITICAL(&playMux);
  playHead = (head + n) % playCap;
  playUsed += n;
  portEXIT_CRITICAL(&playMux);
  return n;
}

size_t talkPlayRingPop(int16_t *out, size_t maxCount) {
  if (!playRing || !out || !maxCount) return 0;

  portENTER_CRITICAL(&playMux);
  const size_t tail = playTail;
  const size_t avail = playUsed;
  portEXIT_CRITICAL(&playMux);

  size_t consumed = 0;
  size_t produced = 0;

  // Walking the snapshot unlocked is safe: only the audio task pops, and the
  // producer only ever grows playUsed.
  const auto nextIn = [&](int16_t *s) -> bool {
    if (avail - consumed < playInBytes) return false;
    const size_t i = (tail + consumed) % playCap;
    if (playInBytes == 1) {
      *s = talkUlawSample(playRing[i]);
    } else {
      const size_t j = (i + 1) % playCap;
      *s = (int16_t)((uint16_t)playRing[i] | ((uint16_t)playRing[j] << 8));
    }
    consumed += playInBytes;
    return true;
  };

  bool starved = false;
  while (produced < maxCount && !starved) {
    if (!playRsPrimed) {
      if (!nextIn(&playRsCur)) break;
      // Flat start rather than a ramp up from zero, which would click.
      playRsPrev = playRsCur;
      playRsFrac = 0;
      playRsPrimed = true;
    }
    // 64-bit because span reaches ±65535 and frac 65535, whose product
    // overflows int32 on a steep loud waveform.
    const int32_t span = (int32_t)playRsCur - (int32_t)playRsPrev;
    const int32_t lerp = (int32_t)(((int64_t)span * (int64_t)playRsFrac) >> 16);
    out[produced++] = (int16_t)((int32_t)playRsPrev + lerp);
    playRsFrac += playRsStep;
    while (playRsFrac >= (1u << 16)) {
      playRsFrac -= (1u << 16);
      playRsPrev = playRsCur;
      if (!nextIn(&playRsCur)) {
        // Hold the last sample and let the caller zero-fill the rest; the pump
        // re-primes the jitter buffer once the ring drains completely.
        playRsCur = playRsPrev;
        playRsFrac = 0;
        starved = true;
        break;
      }
    }
  }

  if (consumed) {
    portENTER_CRITICAL(&playMux);
    // Skip the commit if a clear landed mid-pop: those bytes are already gone.
    if (playTail == tail && playUsed >= consumed) {
      playTail = (tail + consumed) % playCap;
      playUsed -= consumed;
    }
    portEXIT_CRITICAL(&playMux);
  }
  return produced;
}

uint32_t talkPlayRingUsedMs() {
  portENTER_CRITICAL(&playMux);
  const size_t used = playUsed;
  portEXIT_CRITICAL(&playMux);
  if (!playInBytes || playInRate <= 0) return 0;
  const uint64_t samples = (uint64_t)(used / playInBytes);
  return (uint32_t)(samples * 1000u / (uint64_t)playInRate);
}

bool talkPlayRingEmpty() {
  portENTER_CRITICAL(&playMux);
  const size_t used = playUsed;
  portEXIT_CRITICAL(&playMux);
  return used == 0;
}

bool talkAudioInit() {
  talkAudioResetDsp();
  return talkPlayRingInit();
}

void talkAudioStop() {
  talkAudioStopI2s();
  talkPlayRingClear();
}
