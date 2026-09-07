#include "wake_word.h"

#include "talk_audio.h"
#include "talk_config.h"
#include "wake_models/hey_vinu_model_data.h"

#include <Arduino.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include <frontend.h>
#include <frontend_util.h>

#include "tensorflow/lite/kernels/internal/tensor_ctypes.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

/**
 * microWakeWord streaming detector for "Hey Vinu" (hey_vinu.tflite).
 * Feature frontend + stride/sliding-window match ESPHome micro_wake_word.
 */

namespace {

constexpr uint8_t kFeatureSize = 40;
constexpr uint8_t kFeatureDurationMs = 30;
constexpr uint8_t kFeatureStepMs = 10; // from hey_vinu.json
constexpr size_t kTensorArenaSize = 32000;
constexpr size_t kVarArenaSize = 2048;
constexpr size_t kSlidingWindow = 5;
// hey_vinu.json probability_cutoff
constexpr uint8_t kProbCutoff =
    static_cast<uint8_t>(0.5f * 255.0f + 0.5f);
constexpr int16_t kMinSlicesBeforeDetect = 100;
constexpr UBaseType_t kTaskPriority = 3;
constexpr uint32_t kTaskStack = 8192;

AppHost *host_ = nullptr;
volatile bool active_ = false;
volatile bool detected_ = false;
volatile bool stopReq_ = false;
TaskHandle_t task_ = nullptr;

struct FrontendConfig frontendCfg_{};
struct FrontendState frontendState_{};
bool frontendReady_ = false;

tflite::MicroMutableOpResolver<20> opResolver_;
bool opsReady_ = false;
uint8_t *tensorArena_ = nullptr;
uint8_t *varArena_ = nullptr;
tflite::MicroAllocator *varAllocator_ = nullptr;
tflite::MicroResourceVariables *resourceVars_ = nullptr;
std::unique_ptr<tflite::MicroInterpreter> interpreter_;
bool modelReady_ = false;
uint8_t strideStep_ = 0;
uint8_t inputStride_ = 1;
size_t slidingIdx_ = 0;
int16_t ignoreWindows_ = -kMinSlicesBeforeDetect;
uint8_t recentProb_[kSlidingWindow] = {};

bool registerOps() {
  if (opsReady_) return true;
  if (opResolver_.AddCallOnce() != kTfLiteOk) return false;
  if (opResolver_.AddVarHandle() != kTfLiteOk) return false;
  if (opResolver_.AddReshape() != kTfLiteOk) return false;
  if (opResolver_.AddReadVariable() != kTfLiteOk) return false;
  if (opResolver_.AddStridedSlice() != kTfLiteOk) return false;
  if (opResolver_.AddConcatenation() != kTfLiteOk) return false;
  if (opResolver_.AddAssignVariable() != kTfLiteOk) return false;
  if (opResolver_.AddConv2D() != kTfLiteOk) return false;
  if (opResolver_.AddMul() != kTfLiteOk) return false;
  if (opResolver_.AddAdd() != kTfLiteOk) return false;
  if (opResolver_.AddMean() != kTfLiteOk) return false;
  if (opResolver_.AddFullyConnected() != kTfLiteOk) return false;
  if (opResolver_.AddLogistic() != kTfLiteOk) return false;
  if (opResolver_.AddQuantize() != kTfLiteOk) return false;
  if (opResolver_.AddDepthwiseConv2D() != kTfLiteOk) return false;
  if (opResolver_.AddAveragePool2D() != kTfLiteOk) return false;
  if (opResolver_.AddMaxPool2D() != kTfLiteOk) return false;
  if (opResolver_.AddPad() != kTfLiteOk) return false;
  if (opResolver_.AddPack() != kTfLiteOk) return false;
  if (opResolver_.AddSplitV() != kTfLiteOk) return false;
  opsReady_ = true;
  return true;
}

void unloadModel() {
  interpreter_.reset();
  resourceVars_ = nullptr;
  varAllocator_ = nullptr;
  if (tensorArena_) {
    free(tensorArena_);
    tensorArena_ = nullptr;
  }
  if (varArena_) {
    free(varArena_);
    varArena_ = nullptr;
  }
  modelReady_ = false;
}

bool loadModel() {
  unloadModel();
  if (!registerOps()) return false;

  const tflite::Model *model = tflite::GetModel(kHeyVinuModelData);
  if (model->version() != TFLITE_SCHEMA_VERSION) return false;

  tensorArena_ = (uint8_t *)heap_caps_malloc(
      kTensorArenaSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!tensorArena_) {
    tensorArena_ = (uint8_t *)malloc(kTensorArenaSize);
  }
  varArena_ = (uint8_t *)heap_caps_malloc(
      kVarArenaSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!varArena_) {
    varArena_ = (uint8_t *)malloc(kVarArenaSize);
  }
  if (!tensorArena_ || !varArena_) {
    unloadModel();
    return false;
  }

  varAllocator_ = tflite::MicroAllocator::Create(varArena_, kVarArenaSize);
  if (!varAllocator_) {
    unloadModel();
    return false;
  }
  resourceVars_ = tflite::MicroResourceVariables::Create(varAllocator_, 20);
  if (!resourceVars_) {
    unloadModel();
    return false;
  }
  interpreter_ = std::unique_ptr<tflite::MicroInterpreter>(
      new tflite::MicroInterpreter(model, opResolver_, tensorArena_,
                                   kTensorArenaSize, resourceVars_));
  if (interpreter_->AllocateTensors() != kTfLiteOk) {
    unloadModel();
    return false;
  }

  TfLiteTensor *input = interpreter_->input(0);
  TfLiteTensor *output = interpreter_->output(0);
  if (!input || !output || input->type != kTfLiteInt8 ||
      output->type != kTfLiteUInt8) {
    unloadModel();
    return false;
  }
  if (input->dims->size != 3 || input->dims->data[0] != 1 ||
      input->dims->data[2] != kFeatureSize) {
    unloadModel();
    return false;
  }
  if (output->dims->size != 2 || output->dims->data[0] != 1 ||
      output->dims->data[1] != 1) {
    unloadModel();
    return false;
  }

  inputStride_ = (uint8_t)input->dims->data[1];
  if (inputStride_ < 1) inputStride_ = 1;
  strideStep_ = 0;
  slidingIdx_ = 0;
  ignoreWindows_ = -kMinSlicesBeforeDetect;
  memset(recentProb_, 0, sizeof(recentProb_));
  modelReady_ = true;
  return true;
}

bool setupFrontend() {
  if (frontendReady_) {
    FrontendReset(&frontendState_);
    return true;
  }
  FrontendFillConfigWithDefaults(&frontendCfg_);
  frontendCfg_.window.size_ms = kFeatureDurationMs;
  frontendCfg_.window.step_size_ms = kFeatureStepMs;
  frontendCfg_.filterbank.num_channels = kFeatureSize;
  frontendCfg_.filterbank.lower_band_limit = 125.0f;
  frontendCfg_.filterbank.upper_band_limit = 7500.0f;
  frontendCfg_.noise_reduction.smoothing_bits = 10;
  // Softer NR than stock — stock adaptation was wiping speech features (maxp≈0
  // while mic lvl spiked). Still matches training topology; just less aggressive.
  frontendCfg_.noise_reduction.even_smoothing = 0.025f;
  frontendCfg_.noise_reduction.odd_smoothing = 0.06f;
  frontendCfg_.noise_reduction.min_signal_remaining = 0.05f;
  frontendCfg_.pcan_gain_control.enable_pcan = 1;
  frontendCfg_.pcan_gain_control.strength = 0.95f;
  frontendCfg_.pcan_gain_control.offset = 80.0f;
  frontendCfg_.pcan_gain_control.gain_bits = 21;
  frontendCfg_.log_scale.enable_log = 1;
  frontendCfg_.log_scale.scale_shift = 6;

  if (!FrontendPopulateState(&frontendCfg_, &frontendState_, TALK_SAMPLE_RATE)) {
    return false;
  }
  frontendReady_ = true;
  return true;
}

void teardownFrontend() {
  if (frontendReady_) {
    FrontendFreeStateContents(&frontendState_);
    frontendReady_ = false;
  }
}

bool quantizeFeatures(const struct FrontendOutput &out, int8_t *dst) {
  if (out.size != kFeatureSize || !out.values) return false;
  // Match ESPHome / TFLite audio frontend int8 scaling.
  constexpr int32_t valueScale = 256;
  constexpr int32_t valueDiv = 666; // 25.6 * 26.0
  for (size_t i = 0; i < kFeatureSize; ++i) {
    const uint16_t u = out.values[i];
    int32_t value =
        ((static_cast<int32_t>(u) * valueScale) + (valueDiv / 2)) / valueDiv;
    value += INT8_MIN;
    if (value < INT8_MIN) value = INT8_MIN;
    if (value > INT8_MAX) value = INT8_MAX;
    dst[i] = static_cast<int8_t>(value);
  }
  return true;
}

bool runInference(const int8_t features[kFeatureSize]) {
  if (!modelReady_ || !interpreter_) return false;
  TfLiteTensor *input = interpreter_->input(0);
  strideStep_ = strideStep_ % inputStride_;
  int8_t *in = tflite::GetTensorData<int8_t>(input);
  memcpy(in + (size_t)kFeatureSize * strideStep_, features, kFeatureSize);
  ++strideStep_;
  if (strideStep_ < inputStride_) return true;

  if (interpreter_->Invoke() != kTfLiteOk) return false;

  TfLiteTensor *output = interpreter_->output(0);
  recentProb_[slidingIdx_] = output->data.uint8[0];
  slidingIdx_ = (slidingIdx_ + 1) % kSlidingWindow;

  if (recentProb_[(slidingIdx_ + kSlidingWindow - 1) % kSlidingWindow] <
      kProbCutoff) {
    ignoreWindows_ = (int16_t)std::min<int>(ignoreWindows_ + 1, 0);
  }

  if (ignoreWindows_ < 0) return true;

  uint32_t sum = 0;
  for (size_t i = 0; i < kSlidingWindow; ++i) sum += recentProb_[i];
  if (sum > (uint32_t)kProbCutoff * kSlidingWindow) {
    detected_ = true;
    ignoreWindows_ = -kMinSlicesBeforeDetect;
    memset(recentProb_, 0, sizeof(recentProb_));
  }
  return true;
}

bool generateFeature(const int16_t *audio, size_t available,
                     int8_t features[kFeatureSize], size_t *consumed) {
  *consumed = 0;
  struct FrontendOutput out =
      FrontendProcessSamples(&frontendState_, audio, available, consumed);
  if (out.size == 0) return false;
  return quantizeFeatures(out, features);
}

void wakeTask(void * /*arg*/) {
  if (!talkAudioInit() || !talkAudioStartDuplex()) {
    active_ = false;
    task_ = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  if (!setupFrontend() || !loadModel()) {
    talkAudioStop();
    active_ = false;
    task_ = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  int16_t pcm[TALK_I2S_BUF_SAMPLES];
  int8_t features[kFeatureSize];

  while (!stopReq_) {
    // Keep TX DMA fed so the amp does not underrun.
    talkAudioWriteSilence(TALK_I2S_BUF_SAMPLES, 5);
    const int n =
        talkAudioReadPcmWakeTimeout(pcm, TALK_I2S_BUF_SAMPLES, 20);
    if (n <= 0) continue;

    size_t offset = 0;
    while (offset < (size_t)n && !stopReq_) {
      size_t consumed = 0;
      if (!generateFeature(pcm + offset, (size_t)n - offset, features,
                           &consumed)) {
        if (consumed == 0) break;
        offset += consumed;
        continue;
      }
      offset += consumed;
      if (!runInference(features)) {
        stopReq_ = true;
        break;
      }
      if (detected_) {
        while (detected_ && !stopReq_) {
          talkAudioWriteSilence(TALK_I2S_BUF_SAMPLES, 5);
          vTaskDelay(pdMS_TO_TICKS(20));
        }
        FrontendReset(&frontendState_);
        strideStep_ = 0;
        ignoreWindows_ = -kMinSlicesBeforeDetect;
        memset(recentProb_, 0, sizeof(recentProb_));
      }
    }
  }

  unloadModel();
  teardownFrontend();
  talkAudioStop();
  active_ = false;
  task_ = nullptr;
  vTaskDelete(nullptr);
}

} // namespace

bool wakeWordStart(AppHost *host) {
  host_ = host;
  detected_ = false;
  if (active_ && task_) return true;
  if (task_) {
    // Wait for previous task to exit.
    for (int i = 0; i < 50 && task_; ++i) delay(10);
  }
  stopReq_ = false;
  active_ = true;
  BaseType_t ok =
      xTaskCreatePinnedToCore(wakeTask, "wake", kTaskStack, nullptr,
                              kTaskPriority, &task_, 0);
  if (ok != pdPASS) {
    active_ = false;
    task_ = nullptr;
    return false;
  }
  (void)host_;
  return true;
}

void wakeWordStop() {
  stopReq_ = true;
  detected_ = false;
  for (int i = 0; i < 100 && task_; ++i) delay(10);
  if (task_) {
    task_ = nullptr;
  }
  active_ = false;
  host_ = nullptr;
}

bool wakeWordActive() { return active_; }

bool wakeWordTakeDetected() {
  const bool hit = detected_;
  if (hit) detected_ = false;
  return hit;
}
