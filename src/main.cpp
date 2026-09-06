#include <Arduino.h>
#include <Flow32.h>

#include "panels.h"
#include "storage.h"
#include "audio_pins.h"
#include "input_pins.h"
#include "audio_volume.h"
#include "nvs_defaults.h"
#include "battery.h"
#include "tls_mem.h"
#include "launcher_app.h"
#include "settings_app.h"
#include "ask_app.h"
#include "gallery_app.h"
#include "test_app.h"
#include "remote_app.h"
#include "runtime_config.h"

static const DisplayPanel kPanel = Panel169();
static const Rect kPanelRect(0, 0, kPanel.width, kPanel.height);

static LauncherApp launcher(kPanelRect);
static SettingsApp settings(kPanelRect);
static AskApp ask(kPanelRect);
static GalleryApp gallery(kPanelRect);
static TestApp test(kPanelRect);
static RemoteApp remote(kPanelRect);
static JoystickInput joystick(ChitramInput::kJoyPinX, ChitramInput::kJoyPinY,
                              ChitramInput::kJoyPinSw);
static Flow32 flow(kPanel);

void setup() {
  Serial.begin(115200);
  delay(1500);

  tlsUsePsram();

  ensureNvsDefaults();

  configInit();

  JoystickInput::initAdcEarly(ChitramInput::kJoyPinX, ChitramInput::kJoyPinY);
  ChitramAudio::initVolume();
  Battery::begin();

  joystick.deadzone = ChitramInput::kJoyDeadzone;
  joystick.tracker().holdDelayMs = ChitramInput::kJoyHoldDelayMs;
  joystick.tracker().holdRepeatMs = ChitramInput::kJoyHoldRepeatMs;

  flow.apps({&launcher, &settings, &ask, &gallery, &remote, &test})
      .config(FlowConfig{}
                  .theme(Theme::FlowTheme())
                  .storage(SdChitram())
                  .debugBorders(false)
                  .idleEyes(30))
      .input(joystick)
      .begin();
}

void loop() {
  Battery::poll(0.016f);
  flow.tick();
}
