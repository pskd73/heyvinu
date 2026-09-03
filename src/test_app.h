#pragma once

#include <Flow32.h>

#include "battery.h"

struct TestState {
  static constexpr uint32_t kMagic = 0x54535431u; // 'TST1'
  static constexpr uint16_t kVersion = 1;

  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint16_t reserved = 0;
};

/**
 * Hardware test hub — menu with Mic, Speaker, Joystick, and Battery pages.
 */
class TestApp : public App<TestState> {
public:
  static constexpr uint8_t kPageMenu = 0;
  static constexpr uint8_t kPageMic = 1;
  static constexpr uint8_t kPageSound = 2;
  static constexpr uint8_t kPageJoy = 3;
  static constexpr uint8_t kPageBattery = 4;
  static constexpr uint8_t kPageWifi = 5;

  explicit TestApp(const Rect &viewport) : App(viewport) {
    setAppInfo("Test", "wrench");
    addPage("Test");
    addPage("Mic");
    addPage("Speaker");
    addPage("Joystick");
    addPage("Battery");
    addPage("Wi-Fi");
    self_ = this;
  }

  void frame(Canvas &canvas, InputHub &input, float dt) override;
  bool goBack() override;

protected:
  const char *nvsNamespace() const override { return "test"; }

  void onOpen() override;
  void onClose() override;
  void build(Page &page, uint8_t pageId) override;

private:
  static TestApp *self_;

  static constexpr int16_t kWaveW = 224;
  static constexpr int16_t kWaveImgW = 240;
  static constexpr int16_t kWaveImgH = 100;

  // --- Mic ---
  int16_t wave_[kWaveW] = {};
  int16_t waveHead_ = 0;
  // 48 KB. Held only while the mic page is open, and out of internal RAM:
  // as a static member it starved the TLS/AES path in Ask of DMA memory.
  uint16_t *wavePixels_ = nullptr;
  bool i2sRunning_ = false;
  bool micError_ = false;
  float level_ = 0.f;
  float peakHold_ = 0.f;
  int32_t lastPeak_ = 0;
  char statusMsg_[40] = {};

  bool ensureWaveBuffer();
  void freeWaveBuffer();
  bool startI2s();
  void stopI2s();
  void readSamples();
  void renderWaveImage();
  void formatMicLabels();
  void enterMic();
  void leaveMic();
  void buildMic(Page &page);
  static void onMicTick(UINode &node, float dt);

  // --- Speaker ---
  void pumpAudio();
  void enterSound();
  void leaveSound();
  void buildSound(Page &page);

  // --- Joystick ---
  char joySummary_[64] = "Joystick\n--";
  int lastX_ = -1;
  int lastY_ = -1;
  bool lastSw_ = false;
  bool lastOk_ = false;

  void formatJoyLines();
  void enterJoy();
  void buildJoy(Page &page);
  static void onJoyTick(UINode &node, float dt);

  // --- Battery ---
  char battSummary_[96] = "Battery\n--%";
  char battDetail_[768] = {};
  bool lastBattOk_ = false;
  uint16_t lastSocRaw_ = 0xFFFF;
  uint16_t lastVcellRaw_ = 0xFFFF;
  uint16_t lastCrate_ = 0xFFFF;
  ChargeState lastCharge_ = ChargeState::Unknown;

  void formatBatterySummary();
  void formatBatteryDetail();
  void enterBattery();
  void buildBattery(Page &page);
  static void onBatteryTick(UINode &node, float dt);

  // --- Wi-Fi ---
  enum class WifiTestPhase : uint8_t {
    Connecting,
    Connected,
    HttpsTesting,
    Pass,
    Fail,
  };

  char wifiSummary_[64] = "Wi-Fi\n--";
  char wifiDetail_[512] = {};
  WifiTestPhase wifiPhase_ = WifiTestPhase::Connecting;
  uint32_t wifiPhaseMs_ = 0;
  int wifiHttpCode_ = 0;

  void formatWifiSummary();
  void formatWifiDetail();
  void enterWifi();
  void runWifiStep();
  bool runHttpsProbe(int &httpCode);
  void buildWifi(Page &page);
  static void onWifiTick(UINode &node, float dt);

  // --- Menu / nav ---
  void leaveActiveTest();
  void openMic();
  void openSound();
  void openJoy();
  void openBattery();
  void openWifi();
  void buildMenu(Page &page);

  static void onMenuSelect(UISelect &s);

  bool menuFocusPending_ = false;
};
