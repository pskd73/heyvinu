#include "test_app.h"

void TestApp::onOpen() {
  self_ = this;
  if (state().magic != TestState::kMagic ||
      state().version != TestState::kVersion) {
    data() = TestState{};
  }
  menuFocusPending_ = true;
}

void TestApp::onClose() {
  leaveActiveTest();
  if (self_ == this) {
    self_ = nullptr;
  }
}

bool TestApp::goBack() {
  leaveActiveTest();
  return back();
}

void TestApp::leaveActiveTest() {
  const uint8_t id = pageId();
  if (id == kPageMic) {
    leaveMic();
  } else if (id == kPageSound) {
    leaveSound();
  }
}

void TestApp::openMic() {
  if (!self_) return;
  self_->enterMic();
  self_->goTo(kPageMic);
}

void TestApp::openSound() {
  if (!self_) return;
  self_->enterSound();
  self_->goTo(kPageSound);
}

void TestApp::openJoy() {
  if (!self_) return;
  self_->enterJoy();
  self_->goTo(kPageJoy);
}

void TestApp::openBattery() {
  if (!self_) return;
  self_->enterBattery();
  self_->goTo(kPageBattery);
}

void TestApp::openWifi() {
  if (!self_) return;
  self_->enterWifi();
  self_->goTo(kPageWifi);
}

void TestApp::onMenuSelect(UISelect &s) {
  if (!self_) return;
  switch (s.selectedValue()) {
  case kPageMic:
    self_->openMic();
    break;
  case kPageSound:
    self_->openSound();
    break;
  case kPageJoy:
    self_->openJoy();
    break;
  case kPageBattery:
    self_->openBattery();
    break;
  case kPageWifi:
    self_->openWifi();
    break;
  default:
    break;
  }
}

void TestApp::buildMenu(Page &page) {
  auto &menu =
      page.select()
          .selected(-1)
          .onChange(onMenuSelect)
          .style(Style().setWidth(Length::Pct(100)).setGap(6))
          .add(page.selectOption()
                   .icon("mic")
                   .title("Mic")
                   .description("Live waveform")
                   .value(kPageMic))
          .add(page.selectOption()
                   .icon("volume-2")
                   .title("Speaker")
                   .description("MP3 playback test")
                   .value(kPageSound))
          .add(page.selectOption()
                   .icon("gamepad-2")
                   .title("Joystick")
                   .description("ADC direction readout")
                   .value(kPageJoy))
          .add(page.selectOption()
                   .icon("battery")
                   .title("Battery")
                   .description("MAX17048 gauge + registers")
                   .value(kPageBattery))
          .add(page.selectOption()
                   .icon("wifi")
                   .title("Wi-Fi")
                   .description("Connect + HTTPS reachability")
                   .value(kPageWifi));

  page.add(page.div()
               .style(Style()
                          .setWidth(Length::Pct(100))
                          .setPadding(Edges(16, 12))
                          .setColumns(1))
               .add(menu));
}

void TestApp::build(Page &page, uint8_t pageId) {
  if (pageId == kPageMenu) {
    buildMenu(page);
    return;
  }
  if (pageId == kPageMic) {
    buildMic(page);
    return;
  }
  if (pageId == kPageSound) {
    buildSound(page);
    return;
  }
  if (pageId == kPageJoy) {
    buildJoy(page);
    return;
  }
  if (pageId == kPageBattery) {
    buildBattery(page);
    return;
  }
  if (pageId == kPageWifi) {
    buildWifi(page);
  }
}

void TestApp::frame(Canvas &canvas, InputHub &input, float dt) {
  if (pageId() == kPageSound) {
    pumpAudio();
  }
  App<TestState>::frame(canvas, input, dt);
  if (menuFocusPending_ && pageId() == kPageMenu) {
    page().setFocusIndex(0);
    menuFocusPending_ = false;
  }
}
