#include "talk_audio.h"
#include "talk_config.h"
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

static int16_t *playRing = nullptr;
static size_t playCap = 0, playHead = 0, playTail = 0, playUsed = 0;
static portMUX_TYPE playMux = portMUX_INITIALIZER_UNLOCKED;

static int32_t i2sRaw[TALK_I2S_BUF_SAMPLES];

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
    off += n;
    left -= n;
  }
  i2sEndOp();
  return ok;
}

bool talkPlayRingInit() {
  if (playRing) return true;
  playCap = TALK_PLAY_RING_SAMPLES;
  size_t bytes = playCap * sizeof(int16_t);
  playRing = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!playRing) {
    playRing = (int16_t *)malloc(bytes);
  }
  if (!playRing) {
    playCap = 0;
    return false;
  }
  playHead = playTail = playUsed = 0;
  return true;
}

void talkPlayRingClear() {
  portENTER_CRITICAL(&playMux);
  playHead = playTail = playUsed = 0;
  portEXIT_CRITICAL(&playMux);
}

size_t talkPlayRingPush(const int16_t *data, size_t count) {
  size_t pushed = 0;
  portENTER_CRITICAL(&playMux);
  while (pushed < count && playUsed < playCap) {
    playRing[playHead] = data[pushed++];
    playHead = (playHead + 1) % playCap;
    playUsed++;
  }
  portEXIT_CRITICAL(&playMux);
  return pushed;
}

size_t talkPlayRingPop(int16_t *out, size_t maxCount) {
  size_t popped = 0;
  portENTER_CRITICAL(&playMux);
  while (popped < maxCount && playUsed > 0) {
    out[popped++] = playRing[playTail];
    playTail = (playTail + 1) % playCap;
    playUsed--;
  }
  portEXIT_CRITICAL(&playMux);
  return popped;
}

size_t talkPlayRingUsed() {
  portENTER_CRITICAL(&playMux);
  size_t n = playUsed;
  portEXIT_CRITICAL(&playMux);
  return n;
}

bool talkAudioInit() {
  talkAudioResetDsp();
  return talkPlayRingInit();
}

void talkAudioStop() {
  talkAudioStopI2s();
  talkPlayRingClear();
}
