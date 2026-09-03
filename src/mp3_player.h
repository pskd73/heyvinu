#pragma once

#include <FS.h>
#include <stddef.h>
#include <stdint.h>

class Storage;

/** Stream-decodes an MP3 from SD and writes PCM to Chitram I2S speaker. */
class Mp3Player {
public:
  bool begin(Storage *storage, const char *path);
  void stop();
  bool isRunning() const { return running_; }
  const char *lastError() const { return lastError_; }

  /** Decode/write as much audio as possible; returns false when finished. */
  bool pump();

private:
  bool ensureI2s(int sampleRate);
  void releaseI2s();
  bool fillBufferWithFrame();
  bool writeSample(int16_t left, int16_t right);
  void setError(const char *msg);

  void *decoder_ = nullptr;
  File file_;
  char lastError_[48] = {};
  uint8_t inBuf_[2048] = {};
  int16_t pcm_[2304] = {};
  int inValid_ = 0;
  int lastFrameEnd_ = 0;
  int validSamples_ = 0;
  int curSample_ = 0;
  int sampleRate_ = 0;
  int channels_ = 0;
  bool i2sRunning_ = false;
  bool running_ = false;
};
