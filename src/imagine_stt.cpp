#include "imagine_stt.h"
#include "audio_pins.h"
#include "imagine_config.h"
#include "net_wifi.h"
#include "runtime_config.h"

#include <WebSocketsClient.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>

/** Do not run Ask + Imagine together — they share the mic I2S bus. */

namespace {

constexpr i2s_port_t kPort = I2S_NUM_0;
constexpr uint8_t kDmaBufCount = 4;
constexpr uint16_t kDmaBufLen = 128;
constexpr float kAgcTargetPeak = 7000.f;
constexpr float kAgcGainMin = 1.f;
constexpr float kAgcGainMax = 5.f;
constexpr float kAgcGainSlew = 0.0012f;

ImagineSttState state_ = ImagineSttState::Idle;
char errMsg_[48] = {};
char finalText_[224] = {};
char interimText_[224] = {};
char lastFinal_[224] = {};
char liveBuf_[224] = {};
char textSnap_[224] = {};

SemaphoreHandle_t textMu_ = nullptr;

WebSocketsClient ws_;
volatile bool wsConnected_ = false;
volatile bool ioRunning_ = false;
volatile bool ioFinalize_ = false;
TaskHandle_t ioTask_ = nullptr;

int16_t *txBuf_ = nullptr;
size_t txCap_ = 0;
size_t txHead_ = 0;
size_t txTail_ = 0;
size_t txUsed_ = 0;
portMUX_TYPE txMux_ = portMUX_INITIALIZER_UNLOCKED;

int16_t *micRing_ = nullptr;
size_t micCap_ = 0;
size_t micHead_ = 0;
size_t micTail_ = 0;
size_t micUsed_ = 0;
portMUX_TYPE micMux_ = portMUX_INITIALIZER_UNLOCKED;

TaskHandle_t capTask_ = nullptr;
volatile bool capRunning_ = false;
bool i2sReady_ = false;
bool useRight_ = false;
int i2sFormat_ = 0;

float dspDcX_ = 0.f;
float dspDcY_ = 0.f;
float dspHpfX_ = 0.f;
float dspHpfY_ = 0.f;
float dspPeakEma_ = 200000.f;
float dspGain_ = 2.f;

void *psramOrRam(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) {
    p = malloc(n);
  }
  return p;
}

void textLock() {
  if (textMu_) {
    xSemaphoreTake(textMu_, portMAX_DELAY);
  }
}
void textUnlock() {
  if (textMu_) {
    xSemaphoreGive(textMu_);
  }
}

void setErr(const char *msg) {
  snprintf(errMsg_, sizeof(errMsg_), "%s", msg ? msg : "STT error");
}

void resetDsp() {
  dspDcX_ = dspDcY_ = 0.f;
  dspHpfX_ = dspHpfY_ = 0.f;
  dspPeakEma_ = 200000.f;
  dspGain_ = 2.f;
}

int16_t micSlotToPcm(int32_t raw) {
  const float x = static_cast<float>(raw >> 8);
  float y = x - dspDcX_ + 0.995f * dspDcY_;
  dspDcX_ = x;
  dspDcY_ = y;
  float hp = 0.961f * (dspHpfY_ + y - dspHpfX_);
  dspHpfX_ = y;
  dspHpfY_ = hp;
  const float absv = fabsf(hp);
  if (absv > dspPeakEma_) {
    dspPeakEma_ = 0.85f * dspPeakEma_ + 0.15f * absv;
  } else {
    dspPeakEma_ = 0.9997f * dspPeakEma_ + 0.0003f * absv;
  }
  float desired = (kAgcTargetPeak * 256.f) / (dspPeakEma_ + 1.f);
  if (desired < kAgcGainMin) {
    desired = kAgcGainMin;
  }
  if (desired > kAgcGainMax) {
    desired = kAgcGainMax;
  }
  dspGain_ += kAgcGainSlew * (desired - dspGain_);
  float out = hp * dspGain_ / 256.f;
  if (out > 32767.f) {
    out = 32767.f;
  }
  if (out < -32768.f) {
    out = -32768.f;
  }
  return static_cast<int16_t>(out);
}

void appendFinal(const char *segIn) {
  char seg[224];
  strncpy(seg, segIn ? segIn : "", sizeof(seg) - 1);
  seg[sizeof(seg) - 1] = '\0';
  // trim
  char *s = seg;
  while (*s == ' ') {
    s++;
  }
  size_t n = strlen(s);
  while (n > 0 && s[n - 1] == ' ') {
    s[--n] = '\0';
  }
  if (!s[0]) {
    return;
  }
  if (strcmp(s, lastFinal_) == 0 || strcmp(s, finalText_) == 0) {
    return;
  }
  if (finalText_[0] && strncmp(s, finalText_, strlen(finalText_)) == 0) {
    const char *rest = s + strlen(finalText_);
    while (*rest == ' ') {
      rest++;
    }
    if (!rest[0] || strcmp(rest, lastFinal_) == 0) {
      return;
    }
    size_t used = strlen(finalText_);
    if (used + 1 < sizeof(finalText_)) {
      finalText_[used++] = ' ';
      strncpy(finalText_ + used, rest, sizeof(finalText_) - used - 1);
    }
    strncpy(lastFinal_, rest, sizeof(lastFinal_) - 1);
    lastFinal_[sizeof(lastFinal_) - 1] = '\0';
    return;
  }
  if (finalText_[0]) {
    size_t used = strlen(finalText_);
    if (used + 1 < sizeof(finalText_)) {
      finalText_[used++] = ' ';
      strncpy(finalText_ + used, s, sizeof(finalText_) - used - 1);
    }
  } else {
    strncpy(finalText_, s, sizeof(finalText_) - 1);
  }
  strncpy(lastFinal_, s, sizeof(lastFinal_) - 1);
  lastFinal_[sizeof(lastFinal_) - 1] = '\0';
}

void rebuildLive() {
  liveBuf_[0] = '\0';
  strncpy(liveBuf_, finalText_, sizeof(liveBuf_) - 1);
  if (interimText_[0]) {
    size_t used = strlen(liveBuf_);
    if (used && used + 1 < sizeof(liveBuf_)) {
      liveBuf_[used++] = ' ';
    }
    strncpy(liveBuf_ + used, interimText_, sizeof(liveBuf_) - used - 1);
  }
}

bool extractField(const char *json, const char *key, char *out, size_t outLen) {
  if (!json || !key || !out || outLen < 2) {
    return false;
  }
  char needle[48];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char *p = strstr(json, needle);
  if (!p) {
    return false;
  }
  p = strchr(p + strlen(needle), ':');
  if (!p) {
    return false;
  }
  p++;
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  if (*p != '"') {
    return false;
  }
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i + 1 < outLen) {
    if (*p == '\\' && p[1]) {
      char n = *++p;
      if (n == 'n') {
        out[i++] = ' ';
      } else if (n == 't') {
        out[i++] = ' ';
      } else {
        out[i++] = n;
      }
      p++;
      continue;
    }
    out[i++] = *p++;
  }
  out[i] = '\0';
  return true;
}

bool jsonBoolTrue(const char *json, const char *key) {
  char needle[48];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char *p = strstr(json, needle);
  if (!p) {
    return false;
  }
  p = strchr(p + strlen(needle), ':');
  if (!p) {
    return false;
  }
  p++;
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  return strncmp(p, "true", 4) == 0;
}

void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_CONNECTED) {
    Serial.println("Deepgram WS open");
    wsConnected_ = true;
    return;
  }
  if (type == WStype_DISCONNECTED) {
    Serial.println("Deepgram WS closed");
    wsConnected_ = false;
    return;
  }
  if (type != WStype_TEXT || !payload || length == 0) {
    return;
  }

  // Deepgram JSON is small; keep a bounded copy.
  char json[1536];
  size_t n = length < sizeof(json) - 1 ? length : sizeof(json) - 1;
  memcpy(json, payload, n);
  json[n] = '\0';

  if (strstr(json, "UtteranceEnd")) {
    textLock();
    if (interimText_[0]) {
      appendFinal(interimText_);
      interimText_[0] = '\0';
    }
    rebuildLive();
    textUnlock();
    return;
  }

  char transcript[224];
  if (!extractField(json, "transcript", transcript, sizeof(transcript))) {
    return;
  }
  if (!transcript[0]) {
    return;
  }

  textLock();
  if (jsonBoolTrue(json, "is_final")) {
    appendFinal(transcript);
    interimText_[0] = '\0';
    Serial.printf("FINAL: %s\n", transcript);
  } else {
    strncpy(interimText_, transcript, sizeof(interimText_) - 1);
    interimText_[sizeof(interimText_) - 1] = '\0';
  }
  rebuildLive();
  textUnlock();
}

bool txAlloc() {
  if (txBuf_) {
    return true;
  }
  txCap_ = IMAGINE_MIC_RING_SAMPLES;
  txBuf_ = static_cast<int16_t *>(psramOrRam(txCap_ * sizeof(int16_t)));
  if (!txBuf_) {
    txCap_ = 0;
    return false;
  }
  txHead_ = txTail_ = txUsed_ = 0;
  return true;
}

void txReset() {
  portENTER_CRITICAL(&txMux_);
  txHead_ = txTail_ = txUsed_ = 0;
  portEXIT_CRITICAL(&txMux_);
}

size_t txPush(const int16_t *samples, size_t count) {
  if (!txBuf_ || !samples || count == 0) {
    return 0;
  }
  size_t pushed = 0;
  portENTER_CRITICAL(&txMux_);
  while (pushed < count && txUsed_ < txCap_) {
    txBuf_[txHead_] = samples[pushed++];
    txHead_ = (txHead_ + 1) % txCap_;
    txUsed_++;
  }
  portEXIT_CRITICAL(&txMux_);
  return pushed;
}

size_t txPop(int16_t *dest, size_t maxCount) {
  size_t popped = 0;
  portENTER_CRITICAL(&txMux_);
  while (popped < maxCount && txUsed_ > 0) {
    dest[popped++] = txBuf_[txTail_];
    txTail_ = (txTail_ + 1) % txCap_;
    txUsed_--;
  }
  portEXIT_CRITICAL(&txMux_);
  return popped;
}

size_t txAvail() {
  portENTER_CRITICAL(&txMux_);
  size_t n = txUsed_;
  portEXIT_CRITICAL(&txMux_);
  return n;
}

bool micRingAlloc() {
  if (micRing_) {
    return true;
  }
  micCap_ = IMAGINE_MIC_RING_SAMPLES;
  micRing_ = static_cast<int16_t *>(psramOrRam(micCap_ * sizeof(int16_t)));
  if (!micRing_) {
    micCap_ = 0;
    return false;
  }
  micHead_ = micTail_ = micUsed_ = 0;
  return true;
}

void micRingReset() {
  portENTER_CRITICAL(&micMux_);
  micHead_ = micTail_ = micUsed_ = 0;
  portEXIT_CRITICAL(&micMux_);
}

size_t micPush(const int16_t *samples, size_t count) {
  if (!micRing_ || !samples) {
    return 0;
  }
  size_t pushed = 0;
  portENTER_CRITICAL(&micMux_);
  while (pushed < count && micUsed_ < micCap_) {
    micRing_[micHead_] = samples[pushed++];
    micHead_ = (micHead_ + 1) % micCap_;
    micUsed_++;
  }
  portEXIT_CRITICAL(&micMux_);
  return pushed;
}

size_t micPop(int16_t *dest, size_t maxCount) {
  size_t popped = 0;
  portENTER_CRITICAL(&micMux_);
  while (popped < maxCount && micUsed_ > 0) {
    dest[popped++] = micRing_[micTail_];
    micTail_ = (micTail_ + 1) % micCap_;
    micUsed_--;
  }
  portEXIT_CRITICAL(&micMux_);
  return popped;
}

size_t micAvail() {
  portENTER_CRITICAL(&micMux_);
  size_t n = micUsed_;
  portEXIT_CRITICAL(&micMux_);
  return n;
}

void stopCaptureInternal() {
  if (!capTask_) {
    return;
  }
  capRunning_ = false;
  for (int i = 0; i < 100 && capTask_; ++i) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (capTask_) {
    vTaskDelete(capTask_);
    capTask_ = nullptr;
  }
}

void captureTask(void *arg) {
  (void)arg;
  int32_t raw[128];
  int16_t pcm[128];
  while (capRunning_) {
    if (!i2sReady_) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    size_t bytesRead = 0;
    esp_err_t err =
        i2s_read(kPort, raw, sizeof(raw), &bytesRead, pdMS_TO_TICKS(50));
    if (!capRunning_) {
      break;
    }
    if (err != ESP_OK || bytesRead < sizeof(int32_t)) {
      continue;
    }
    size_t n = bytesRead / sizeof(int32_t);
    size_t pcmN = 0;
    for (size_t i = 0; i < n; ++i) {
      pcm[pcmN++] = micSlotToPcm(raw[i]);
    }
    if (pcmN > 0) {
      micPush(pcm, pcmN);
    }
  }
  capTask_ = nullptr;
  vTaskDelete(nullptr);
}

void releaseI2s() {
  stopCaptureInternal();
  if (i2sReady_) {
    i2s_driver_uninstall(kPort);
    i2sReady_ = false;
    delay(10);
  }
}

bool initMicCapture(bool rightChannel, int i2sFormat) {
  releaseI2s();

  i2s_config_t cfg = {};
  cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = IMAGINE_SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format =
      rightChannel ? I2S_CHANNEL_FMT_ONLY_RIGHT : I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = (i2sFormat == 1) ? I2S_COMM_FORMAT_STAND_MSB
                                                : I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = kDmaBufCount;
  cfg.dma_buf_len = kDmaBufLen;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = false;

  if (i2s_driver_install(kPort, &cfg, 0, nullptr) != ESP_OK) {
    Serial.println("Imagine STT: I2S install failed");
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.bck_io_num = ChitramAudio::kI2sBclk;
  pins.ws_io_num = ChitramAudio::kI2sWs;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = ChitramAudio::kI2sDin;

  if (i2s_set_pin(kPort, &pins) != ESP_OK) {
    i2s_driver_uninstall(kPort);
    return false;
  }
  i2s_zero_dma_buffer(kPort);
  i2sReady_ = true;
  useRight_ = rightChannel;
  i2sFormat_ = i2sFormat;
  return true;
}

struct ProbeStats {
  int16_t pcmPeak = 0;
  uint32_t samples = 0;
};

ProbeStats measureChannel(bool right, int fmt) {
  ProbeStats st;
  resetDsp();
  if (!initMicCapture(right, fmt)) {
    return st;
  }
  delay(40);
  i2s_zero_dma_buffer(kPort);
  const uint32_t endMs = millis() + 180;
  int32_t raw[128];
  while (millis() < endMs) {
    size_t bytesRead = 0;
    if (i2s_read(kPort, raw, sizeof(raw), &bytesRead, pdMS_TO_TICKS(40)) !=
        ESP_OK) {
      continue;
    }
    const size_t n = bytesRead / sizeof(int32_t);
    for (size_t i = 0; i < n; ++i) {
      int16_t s = micSlotToPcm(raw[i]);
      int16_t a = s < 0 ? static_cast<int16_t>(-s) : s;
      if (a > st.pcmPeak) {
        st.pcmPeak = a;
      }
      st.samples++;
    }
  }
  return st;
}

bool chooseMic() {
  int bestScore = 0;
  bool bestRight = false;
  int bestFmt = 0;
  for (int fmt = 0; fmt < 2; ++fmt) {
    for (int ch = 0; ch < 2; ++ch) {
      ProbeStats st = measureChannel(ch == 1, fmt);
      const int score = st.pcmPeak >= 50 ? static_cast<int>(st.pcmPeak) : 0;
      Serial.printf("Imagine STT probe %c%d peak=%d n=%lu\n", ch ? 'R' : 'L',
                    fmt, (int)st.pcmPeak, (unsigned long)st.samples);
      if (score > bestScore) {
        bestScore = score;
        bestRight = ch == 1;
        bestFmt = fmt;
      }
      releaseI2s();
    }
  }
  if (bestScore <= 0) {
    setErr("No mic");
    return false;
  }
  if (!initMicCapture(bestRight, bestFmt)) {
    setErr("Mic init fail");
    return false;
  }
  delay(20);
  i2s_zero_dma_buffer(kPort);
  resetDsp();
  Serial.printf("Imagine STT mic %s fmt=%s din=%d\n",
                bestRight ? "RIGHT" : "LEFT", bestFmt ? "MSB" : "Philips",
                ChitramAudio::kI2sDin);
  return true;
}

void startCapture() {
  if (capTask_ || !i2sReady_) {
    return;
  }
  if (!micRingAlloc()) {
    return;
  }
  micRingReset();
  capRunning_ = true;
  xTaskCreatePinnedToCore(captureTask, "img_mic", 4096, nullptr, 20, &capTask_,
                          1);
}

void ioTask(void *arg) {
  (void)arg;
  int16_t sendBuf[IMAGINE_STREAM_CHUNK_SAMPLES];
  uint32_t lastAudioMs = millis();
  uint32_t lastKeepMs = millis();

  while (ioRunning_ || txAvail() > 0) {
    bool did = false;
    if (wsConnected_) {
      ws_.loop();
      for (int i = 0; i < 8; ++i) {
        size_t n = txPop(sendBuf, IMAGINE_STREAM_CHUNK_SAMPLES);
        if (n == 0) {
          break;
        }
        if (!ws_.sendBIN(reinterpret_cast<uint8_t *>(sendBuf),
                          n * sizeof(int16_t))) {
          Serial.println("ERR Deepgram sendBIN");
          wsConnected_ = false;
          break;
        }
        lastAudioMs = millis();
        did = true;
        ws_.loop();
      }
      uint32_t now = millis();
      if (wsConnected_ && now - lastAudioMs > 3000 && now - lastKeepMs > 3000) {
        ws_.sendTXT("{\"type\":\"KeepAlive\"}");
        lastKeepMs = now;
      }
    } else if (!ioRunning_) {
      break;
    }
    if (!did) {
      // Drain mic into tx while waiting.
      int16_t chunk[IMAGINE_STREAM_CHUNK_SAMPLES];
      size_t n = micPop(chunk, IMAGINE_STREAM_CHUNK_SAMPLES);
      if (n > 0) {
        txPush(chunk, n);
        did = true;
      }
    }
    if (!did) {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }

  if (ioFinalize_ && ws_.isConnected()) {
    while (txAvail() > 0) {
      size_t n = txPop(sendBuf, IMAGINE_STREAM_CHUNK_SAMPLES);
      if (n == 0) {
        break;
      }
      ws_.sendBIN(reinterpret_cast<uint8_t *>(sendBuf), n * sizeof(int16_t));
      ws_.loop();
    }
    ws_.sendTXT("{\"type\":\"Finalize\"}");
    uint32_t deadline = millis() + 800;
    while (millis() < deadline && ws_.isConnected()) {
      ws_.loop();
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    ws_.sendTXT("{\"type\":\"CloseStream\"}");
    deadline = millis() + 400;
    while (millis() < deadline && ws_.isConnected()) {
      ws_.loop();
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    ws_.disconnect();
  } else if (ws_.isConnected()) {
    ws_.disconnect();
  }

  wsConnected_ = false;
  ioTask_ = nullptr;
  vTaskDelete(nullptr);
}

void stopIo(bool finalize) {
  ioFinalize_ = finalize;
  ioRunning_ = false;
  for (int i = 0; i < 200 && ioTask_; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (ioTask_) {
    vTaskDelete(ioTask_);
    ioTask_ = nullptr;
  }
  wsConnected_ = false;
  if (ws_.isConnected()) {
    ws_.disconnect();
  }
}

bool connectDeepgram() {
  const char *dgKey = getConfig(Config::DeepgramApiKey);
  if (!dgKey || strlen(dgKey) < 8 || strcmp(dgKey, "REPLACE_ME") == 0 ||
      strcmp(dgKey, "your-deepgram-api-key") == 0) {
    setErr("No Deepgram key");
    return false;
  }

  if (ioTask_) {
    stopIo(false);
  }

  IPAddress dgIp;
  if (!wifiResolveHost(DG_HOST, dgIp, 10000)) {
    setErr("DNS failed");
    return false;
  }

  static char auth[96];
  snprintf(auth, sizeof(auth), "Token %s", dgKey);
  ws_.onEvent(onWsEvent);
  ws_.setReconnectInterval(0);
  ws_.setAuthorization(auth);

  Serial.printf("Deepgram WSS connect %s heap=%u\n", dgIp.toString().c_str(),
                (unsigned)ESP.getFreeHeap());
  wsConnected_ = false;
  // Connect by IP to avoid a second flaky DNS inside beginSSL; SNI still needs host.
  // links2004 beginSSL(host,...) — pass hostname (SNI). DNS already warmed above.
  ws_.beginSSL(DG_HOST, 443, DG_PATH);

  uint32_t t0 = millis();
  while (!ws_.isConnected() && millis() - t0 < 15000) {
    ws_.loop();
    delay(10);
  }
  if (!ws_.isConnected()) {
    Serial.println("Deepgram connect failed");
    ws_.disconnect();
    setErr("STT connect failed");
    return false;
  }
  wsConnected_ = true;
  Serial.printf("Deepgram connected %lums heap=%u\n",
                (unsigned long)(millis() - t0), (unsigned)ESP.getFreeHeap());

  if (!txAlloc()) {
    ws_.disconnect();
    setErr("STT OOM");
    return false;
  }
  txReset();
  ioFinalize_ = false;
  ioRunning_ = true;
  if (xTaskCreatePinnedToCore(ioTask, "img_dg", 8192, nullptr, 16, &ioTask_,
                              0) != pdPASS) {
    ioRunning_ = false;
    ioTask_ = nullptr;
    ws_.disconnect();
    setErr("STT task fail");
    return false;
  }
  return true;
}

void promoteInterim() {
  textLock();
  if (interimText_[0]) {
    appendFinal(interimText_);
    interimText_[0] = '\0';
  }
  rebuildLive();
  textUnlock();
}

void teardown(bool finalize) {
  stopCaptureInternal();
  // Drain remaining mic into tx.
  int16_t chunk[IMAGINE_STREAM_CHUNK_SAMPLES];
  for (int i = 0; i < 8; ++i) {
    size_t n = micPop(chunk, IMAGINE_STREAM_CHUNK_SAMPLES);
    if (n == 0) {
      break;
    }
    txPush(chunk, n);
  }
  stopIo(finalize);
  releaseI2s();
  if (finalize) {
    promoteInterim();
  }
}

} // namespace

bool imagineSttStart() {
  if (!textMu_) {
    textMu_ = xSemaphoreCreateMutex();
  }
  state_ = ImagineSttState::Connecting;
  errMsg_[0] = '\0';
  textLock();
  finalText_[0] = interimText_[0] = lastFinal_[0] = liveBuf_[0] = '\0';
  textUnlock();

  if (!connectWifi()) {
    setErr("WiFi failed");
    state_ = ImagineSttState::Error;
    return false;
  }

  if (!chooseMic()) {
    releaseI2s();
    state_ = ImagineSttState::Error;
    return false;
  }

  if (!connectDeepgram()) {
    releaseI2s();
    state_ = ImagineSttState::Error;
    return false;
  }

  startCapture();
  state_ = ImagineSttState::Listening;
  Serial.println("Imagine STT: listening");
  return true;
}

void imagineSttTick() {
  if (state_ != ImagineSttState::Listening) {
    return;
  }
  // Move mic samples into the WS TX ring (IO task also does this).
  int16_t chunk[IMAGINE_STREAM_CHUNK_SAMPLES];
  size_t n = micPop(chunk, IMAGINE_STREAM_CHUNK_SAMPLES);
  if (n > 0) {
    txPush(chunk, n);
  }
  if (!wsConnected_ && ioTask_ == nullptr) {
    setErr("STT dropped");
    state_ = ImagineSttState::Error;
    teardown(false);
  }
}

void imagineSttStop(bool finalize) {
  if (state_ != ImagineSttState::Listening &&
      state_ != ImagineSttState::Connecting) {
    return;
  }
  teardown(finalize);
  state_ = finalize ? ImagineSttState::Done : ImagineSttState::Idle;
}

void imagineSttAbort() {
  if (state_ == ImagineSttState::Idle) {
    return;
  }
  teardown(false);
  state_ = ImagineSttState::Idle;
}

ImagineSttState imagineSttState() { return state_; }

bool imagineSttListening() { return state_ == ImagineSttState::Listening; }

const char *imagineSttText() {
  textLock();
  rebuildLive();
  memcpy(textSnap_, liveBuf_, sizeof(textSnap_));
  textUnlock();
  return textSnap_;
}

const char *imagineSttError() { return errMsg_[0] ? errMsg_ : "STT error"; }
