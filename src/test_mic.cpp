#include "test_app.h"

#include "audio_pins.h"
#include "talk_config.h"

#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr i2s_port_t kPort = I2S_NUM_0;
constexpr uint32_t kRate = TALK_SAMPLE_RATE;
constexpr size_t kRawCap = TALK_I2S_BUF_SAMPLES;

int32_t rawBuf_[kRawCap];
int16_t pcmBuf_[kRawCap];
int32_t silent32_[128] = {};

bool i2sReady_ = false;

float dspDcX_ = 0.f;
float dspDcY_ = 0.f;
float dspHpfX_ = 0.f;
float dspHpfY_ = 0.f;
float dspLpfY_ = 0.f;
float dspEnv_ = 0.f;
float dspPeakEma_ = 200000.f;
float dspGain_ = 2.f;
constexpr float kGateOpen = 250.f;
constexpr float kGateFull = 700.f;
constexpr int kUserGain = 4;

void resetMicDsp() {
  dspDcX_ = dspDcY_ = 0.f;
  dspHpfX_ = dspHpfY_ = 0.f;
  dspLpfY_ = 0.f;
  dspEnv_ = 0.f;
  dspPeakEma_ = 200000.f;
  dspGain_ = 2.f;
}

int16_t micSlotToPcm(int32_t raw) {
  float x = static_cast<float>(raw >> 8);

  float y = x - dspDcX_ + 0.995f * dspDcY_;
  dspDcX_ = x;
  dspDcY_ = y;

  float hp = 0.961f * (dspHpfY_ + y - dspHpfX_);
  dspHpfX_ = y;
  dspHpfY_ = hp;

  dspLpfY_ += 0.45f * (hp - dspLpfY_);
  float band = dspLpfY_;

  float absv = fabsf(band);
  if (absv > dspPeakEma_) {
    dspPeakEma_ = 0.85f * dspPeakEma_ + 0.15f * absv;
  } else {
    dspPeakEma_ = 0.9997f * dspPeakEma_ + 0.0003f * absv;
  }

  float desired = (AGC_TARGET_PEAK * 256.f) / (dspPeakEma_ + 1.f);
  if (desired < AGC_GAIN_MIN) desired = AGC_GAIN_MIN;
  if (desired > AGC_GAIN_MAX) desired = AGC_GAIN_MAX;
  dspGain_ += AGC_GAIN_SLEW * (desired - dspGain_);

  float out = band * dspGain_ / 256.f;
  out *= (0.5f + static_cast<float>(kUserGain) * 0.15f);

  float a = fabsf(out);
  dspEnv_ = 0.95f * dspEnv_ + 0.05f * a;
  if (dspEnv_ < kGateFull) {
    float g = (dspEnv_ - kGateOpen) / (kGateFull - kGateOpen);
    if (g < 0.f) g = 0.f;
    if (g > 1.f) g = 1.f;
    out *= g;
  }

  if (out > 28000.f) out = 28000.f + (out - 28000.f) * 0.25f;
  else if (out < -28000.f) out = -28000.f + (out + 28000.f) * 0.25f;
  if (out > 32767.f) out = 32767.f;
  if (out < -32768.f) out = -32768.f;
  return static_cast<int16_t>(out);
}

void releaseI2sDriver() {
  if (!i2sReady_) return;
  i2s_driver_uninstall(kPort);
  i2sReady_ = false;
  delay(10);
}

void pumpSilentTx() {
  size_t written = 0;
  i2s_write(kPort, silent32_, sizeof(silent32_), &written, 0);
}

bool initI2sDuplex() {
  releaseI2sDriver();
  resetMicDsp();

  i2s_config_t cfg = {};
  cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
  cfg.sample_rate = kRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = kRawCap;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;

  if (i2s_driver_install(kPort, &cfg, 0, nullptr) != ESP_OK) {
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.bck_io_num = HeyvinuAudio::kI2sBclk;
  pins.ws_io_num = HeyvinuAudio::kI2sWs;
  pins.data_out_num = HeyvinuAudio::kI2sDout;
  pins.data_in_num = HeyvinuAudio::kI2sDin;

  if (i2s_set_pin(kPort, &pins) != ESP_OK) {
    i2s_driver_uninstall(kPort);
    return false;
  }

  i2s_zero_dma_buffer(kPort);
  pumpSilentTx();
  i2sReady_ = true;
  return true;
}

bool readPcm(size_t &outN, int32_t &peak) {
  pumpSilentTx();
  size_t bytesRead = 0;
  if (i2s_read(kPort, rawBuf_, sizeof(rawBuf_), &bytesRead,
               pdMS_TO_TICKS(40)) != ESP_OK ||
      bytesRead < sizeof(int32_t)) {
    return false;
  }
  outN = bytesRead / sizeof(int32_t);
  if (outN > kRawCap) outN = kRawCap;
  peak = 0;
  for (size_t i = 0; i < outN; i++) {
    const int16_t s = micSlotToPcm(rawBuf_[i]);
    pcmBuf_[i] = s;
    const int32_t a = s < 0 ? -s : s;
    if (a > peak) peak = a;
  }
  return outN > 0;
}

void putPixel(uint16_t *px, int16_t w, int16_t h, int16_t x, int16_t y,
              uint16_t color) {
  if (x < 0 || y < 0 || x >= w || y >= h) return;
  px[static_cast<int32_t>(y) * w + x] = color;
}

void drawVLine(uint16_t *px, int16_t w, int16_t h, int16_t x, int16_t y0,
               int16_t y1, uint16_t color) {
  if (y0 > y1) {
    const int16_t t = y0;
    y0 = y1;
    y1 = t;
  }
  for (int16_t y = y0; y <= y1; y++) putPixel(px, w, h, x, y, color);
}

} // namespace

bool TestApp::startI2s() {
  if (i2sRunning_) return true;
  const bool ok = initI2sDuplex();
  i2sRunning_ = i2sReady_;
  return ok && i2sRunning_;
}

void TestApp::stopI2s() {
  if (!i2sRunning_) return;
  releaseI2sDriver();
  i2sRunning_ = false;
}

void TestApp::readSamples() {
  if (!i2sRunning_) return;

  size_t n = 0;
  int32_t peak = 0;
  if (!readPcm(n, peak)) {
    micError_ = true;
    snprintf(statusMsg_, sizeof(statusMsg_), "Read fail");
    return;
  }

  micError_ = false;
  int32_t chunkPeak = 0;
  int32_t sumAbs = 0;

  const size_t step = n > 32 ? n / 32 : 1;
  for (size_t i = 0; i < n; i++) {
    const int16_t s = pcmBuf_[i];
    if ((i % step) == 0) {
      wave_[static_cast<uint16_t>(waveHead_)] = s;
      waveHead_ = static_cast<int16_t>((waveHead_ + 1) % kWaveW);
    }
    const int32_t a = s < 0 ? -s : s;
    if (a > chunkPeak) chunkPeak = a;
    sumAbs += a;
  }

  lastPeak_ = chunkPeak;
  const float chunkLevel =
      static_cast<float>(sumAbs) / static_cast<float>(n) / 32768.f;
  level_ = level_ * 0.7f + chunkLevel * 0.3f;
  if (level_ > peakHold_) {
    peakHold_ = level_;
  } else {
    peakHold_ *= 0.992f;
  }
}

bool TestApp::ensureWaveBuffer() {
  if (wavePixels_) return true;
  const size_t bytes =
      static_cast<size_t>(kWaveImgW) * kWaveImgH * sizeof(uint16_t);
  wavePixels_ = static_cast<uint16_t *>(
      heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!wavePixels_) {
    wavePixels_ = static_cast<uint16_t *>(malloc(bytes));
  }
  if (!wavePixels_) return false;
  memset(wavePixels_, 0, bytes);
  return true;
}

void TestApp::freeWaveBuffer() {
  if (!wavePixels_) return;
  heap_caps_free(wavePixels_);
  wavePixels_ = nullptr;
}

void TestApp::renderWaveImage() {
  if (!wavePixels_) return;
  const uint16_t bg = Theme::active().base200;
  const uint16_t mid = Theme::active().base300;
  const uint16_t ink = Theme::brand(ButtonColor::Primary);
  const int16_t w = kWaveImgW;
  const int16_t h = kWaveImgH;
  const int16_t midY = static_cast<int16_t>(h / 2);

  for (int32_t i = 0; i < static_cast<int32_t>(w) * h; i++) {
    wavePixels_[i] = bg;
  }
  for (int16_t x = 0; x < w; x++) {
    putPixel(wavePixels_, w, h, x, midY, mid);
  }

  constexpr float kFullScale = 9000.f;
  const float scale = 0.9f * static_cast<float>(h / 2 - 2) / kFullScale;

  int16_t prevY = midY;
  for (int16_t col = 0; col < w; col++) {
    const int16_t src =
        static_cast<int16_t>((col * static_cast<int32_t>(kWaveW)) / w);
    const int16_t idx = static_cast<int16_t>((waveHead_ + src) % kWaveW);
    const int16_t sample = wave_[static_cast<uint16_t>(idx)];
    int16_t y =
        static_cast<int16_t>(midY - lroundf(static_cast<float>(sample) * scale));
    if (y < 1) y = 1;
    if (y > h - 2) y = static_cast<int16_t>(h - 2);
    drawVLine(wavePixels_, w, h, col, prevY, y, ink);
    prevY = y;
  }
}

void TestApp::formatMicLabels() {
  if (micError_) {
    snprintf(statusMsg_, sizeof(statusMsg_), "Mic error");
  } else if (lastPeak_ < 80) {
    snprintf(statusMsg_, sizeof(statusMsg_), "Quiet — speak or tap");
  } else {
    snprintf(statusMsg_, sizeof(statusMsg_), "Listening");
  }
}

void TestApp::enterMic() {
  ensureWaveBuffer();
  memset(wave_, 0, sizeof(wave_));
  waveHead_ = 0;
  level_ = 0.f;
  peakHold_ = 0.f;
  lastPeak_ = 0;
  statusMsg_[0] = '\0';
  micError_ = false;

  if (!startI2s()) {
    micError_ = true;
    snprintf(statusMsg_, sizeof(statusMsg_), "I2S fail");
  } else {
    for (int i = 0; i < 8; i++) {
      size_t n = 0;
      int32_t peak = 0;
      readPcm(n, peak);
    }
  }
  formatMicLabels();
  renderWaveImage();
}

void TestApp::leaveMic() {
  stopI2s();
  freeWaveBuffer();
}

void TestApp::onMicTick(UINode &node, float dt) {
  (void)dt;
  if (!self_ || self_->pageId() != kPageMic) return;

  self_->readSamples();
  self_->renderWaveImage();
  self_->formatMicLabels();

  UIDiv &root = static_cast<UIDiv &>(node);
  if (root.childCount() >= 1 && root.child(0)) {
    static_cast<UIText *>(root.child(0))->setText(self_->statusMsg_);
  }

  self_->page().invalidateContent();
}

void TestApp::buildMic(Page &page) {
  const Theme::ThemeTokens &th = Theme::active();
  const uint16_t muted = Theme::lerp(th.baseContent, th.base100, 0.4f);
  if (!ensureWaveBuffer()) {
    snprintf(statusMsg_, sizeof(statusMsg_), "Wave OOM");
  }
  formatMicLabels();
  renderWaveImage();

  page.add(page.div()
               .onTick(onMicTick)
               .style(Style()
                          .setWidth(Length::Pct(100))
                          .setPadding(Edges(16, 12))
                          .setGap(12)
                          .setColumns(1))
               .add(page.text(statusMsg_).style(
                   Style()
                       .setWidth(Length::Pct(100))
                       .setFont(FontRole::Small)
                       .setColor(muted)
                       .setAlign(Align::Center)))
               .add(page.image(wavePixels_, kWaveImgW, kWaveImgH)
                        .style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setRadius(10)
                                   .setFit(ImageFit::Fill))));
}
