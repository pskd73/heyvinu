#include "mp3_player.h"

#include "audio_pins.h"
#include "audio_volume.h"

#include <Flow32.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include "mp3dec.h"
}

namespace {

constexpr i2s_port_t kPort = I2S_NUM_0;
constexpr size_t kWriteChunk = 256;

} // namespace

void Mp3Player::setError(const char *msg) {
  snprintf(lastError_, sizeof(lastError_), "%s", msg ? msg : "");
}

bool Mp3Player::ensureI2s(int sampleRate) {
  if (i2sRunning_ && sampleRate_ == sampleRate) {
    return true;
  }

  releaseI2s();

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = sampleRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;

  if (i2s_driver_install(kPort, &cfg, 0, nullptr) != ESP_OK) {
    setError("I2S install failed");
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.bck_io_num = HeyvinuAudio::kI2sBclk;
  pins.ws_io_num = HeyvinuAudio::kI2sWs;
  pins.data_out_num = HeyvinuAudio::kI2sDout;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  if (i2s_set_pin(kPort, &pins) != ESP_OK) {
    i2s_driver_uninstall(kPort);
    setError("I2S pin failed");
    return false;
  }

  i2s_zero_dma_buffer(kPort);
  i2sRunning_ = true;
  sampleRate_ = sampleRate;
  return true;
}

void Mp3Player::releaseI2s() {
  if (!i2sRunning_) {
    return;
  }

  int16_t silence[kWriteChunk] = {};
  size_t written = 0;
  i2s_write(kPort, silence, sizeof(silence), &written, pdMS_TO_TICKS(50));
  i2s_driver_uninstall(kPort);
  i2sRunning_ = false;
  sampleRate_ = 0;
}

bool Mp3Player::writeSample(int16_t left, int16_t right) {
  (void)right;
  const int16_t sample = HeyvinuAudio::applyVolume(left);
  size_t written = 0;
  return i2s_write(kPort, &sample, sizeof(sample), &written,
                   pdMS_TO_TICKS(10)) == ESP_OK &&
         written == sizeof(sample);
}

bool Mp3Player::fillBufferWithFrame() {
  inBuf_[0] = 0;
  int nextSync = -1;

  do {
    nextSync = MP3FindSyncWord(inBuf_ + lastFrameEnd_,
                               inValid_ - lastFrameEnd_);
    if (nextSync >= 0) {
      nextSync += lastFrameEnd_;
    }
    lastFrameEnd_ = 0;
    if (nextSync == -1) {
      if (inValid_ > 0 && inBuf_[inValid_ - 1] == 0xff) {
        inBuf_[0] = 0xff;
        inValid_ = file_.read(inBuf_ + 1, sizeof(inBuf_) - 1) + 1;
      } else {
        inValid_ = file_.read(inBuf_, sizeof(inBuf_));
      }
      if (inValid_ <= 0) {
        return false;
      }
    }
  } while (nextSync == -1);

  inValid_ -= nextSync;
  memmove(inBuf_, inBuf_ + nextSync, inValid_);
  inValid_ += file_.read(inBuf_ + inValid_, sizeof(inBuf_) - inValid_);
  return true;
}

bool Mp3Player::begin(Storage *storage, const char *path) {
  stop();

  if (!storage || !storage->ready()) {
    setError("SD not ready");
    Serial.println("Sound: SD not ready");
    return false;
  }
  if (!path || !path[0]) {
    setError("No path");
    return false;
  }

  file_ = storage->open(path, FILE_READ);
  if (!file_) {
    setError("Open failed");
    Serial.printf("Sound: open failed %s\n", path);
    return false;
  }

  HMP3Decoder dec = MP3InitDecoder();
  if (!dec) {
    file_.close();
    setError("Decoder OOM");
    Serial.printf("Sound: MP3 decoder OOM (free=%u)\n",
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
    return false;
  }

  decoder_ = dec;
  memset(inBuf_, 0, sizeof(inBuf_));
  memset(pcm_, 0, sizeof(pcm_));
  inValid_ = 0;
  lastFrameEnd_ = 0;
  validSamples_ = 0;
  curSample_ = 0;
  channels_ = 0;
  sampleRate_ = 0;
  running_ = true;
  setError("Playing MP3");
  Serial.printf("Sound: playing %s (%u bytes)\n", path,
                static_cast<unsigned>(file_.size()));
  return true;
}

void Mp3Player::stop() {
  running_ = false;
  releaseI2s();

  if (decoder_) {
    MP3FreeDecoder(static_cast<HMP3Decoder>(decoder_));
    decoder_ = nullptr;
  }
  if (file_) {
    file_.close();
  }

  inValid_ = 0;
  lastFrameEnd_ = 0;
  validSamples_ = 0;
  curSample_ = 0;
  channels_ = 0;
  sampleRate_ = 0;
}

bool Mp3Player::pump() {
  if (!running_ || !decoder_ || !file_) {
    return false;
  }

  while (validSamples_ > 0) {
    const int16_t left = pcm_[curSample_ * 2];
    const int16_t right = pcm_[curSample_ * 2 + 1];
    if (!ensureI2s(sampleRate_ > 0 ? sampleRate_ : 44100)) {
      running_ = false;
      return false;
    }
    if (!writeSample(left, right)) {
      return true;
    }
    validSamples_--;
    curSample_++;
  }

  if (!fillBufferWithFrame()) {
    running_ = false;
    return false;
  }

  unsigned char *in = inBuf_;
  int bytesLeft = inValid_;
  const int ret =
      MP3Decode(static_cast<HMP3Decoder>(decoder_), &in, &bytesLeft, pcm_, 0);
  if (ret != 0) {
    lastFrameEnd_ = inValid_;
    return true;
  }

  lastFrameEnd_ = inValid_ - bytesLeft;
  MP3FrameInfo info = {};
  MP3GetLastFrameInfo(static_cast<HMP3Decoder>(decoder_), &info);
  if (info.samprate > 0 && info.samprate != sampleRate_) {
    sampleRate_ = info.samprate;
    ensureI2s(sampleRate_);
  }
  channels_ = info.nChans > 0 ? info.nChans : 1;
  curSample_ = 0;
  validSamples_ = info.outputSamps / channels_;
  return true;
}
