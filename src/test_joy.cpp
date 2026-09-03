#include "test_app.h"

#include "input_pins.h"

#include <stdio.h>

TestApp *TestApp::self_ = nullptr;

namespace {

constexpr int kCenter = 2048;

void mappedDeltas(int rawX, int rawY, int &horiz, int &vert) {
#if defined(CHITRAM_JOY_HORIZ_ON_Y) || defined(CHITRAM_JOY_SWAP_XY)
  horiz = rawY - kCenter;
  vert = rawX - kCenter;
#else
  // HORIZ_ON_X / default: VRx = L/R, VRy = U/D
  horiz = rawX - kCenter;
  vert = rawY - kCenter;
#endif
#if defined(CHITRAM_JOY_INVERT_HORIZ)
  horiz = -horiz;
#endif
#if defined(CHITRAM_JOY_INVERT_VERT)
  vert = -vert;
#endif
}

const char *directionLabel(int horiz, int vert, int deadzone) {
  const bool left = horiz < -deadzone;
  const bool right = horiz > deadzone;
  const bool up = vert < -deadzone;
  const bool down = vert > deadzone;

  if (up && left) {
    return "Up-Left";
  }
  if (up && right) {
    return "Up-Right";
  }
  if (down && left) {
    return "Down-Left";
  }
  if (down && right) {
    return "Down-Right";
  }
  if (up) {
    return "Up";
  }
  if (down) {
    return "Down";
  }
  if (left) {
    return "Left";
  }
  if (right) {
    return "Right";
  }
  return "Center";
}

} // namespace



void TestApp::formatJoyLines() {
  int rawX = 0;
  int rawY = 0;
  const bool ok = JoystickInput::readRawAxes(ChitramInput::kJoyPinX,
                                             ChitramInput::kJoyPinY, rawX, rawY);
  const bool sw = digitalRead(ChitramInput::kJoyPinSw) == LOW;

  if (!ok) {
    snprintf(joySummary_, sizeof(joySummary_), "Joystick\nADC fail");
    return;
  }

  int horiz = 0;
  int vert = 0;
  mappedDeltas(rawX, rawY, horiz, vert);
  const char *dir =
      directionLabel(horiz, vert, static_cast<int>(ChitramInput::kJoyDeadzone));

  if (sw) {
    snprintf(joySummary_, sizeof(joySummary_), "Joystick\n%s\nSelect", dir);
  } else {
    snprintf(joySummary_, sizeof(joySummary_), "Joystick\n%s", dir);
  }
}

void TestApp::onJoyTick(UINode &node, float dt) {
  (void)dt;
  if (!self_) {
    return;
  }

  int rawX = 0;
  int rawY = 0;
  const bool ok = JoystickInput::readRawAxes(ChitramInput::kJoyPinX,
                                             ChitramInput::kJoyPinY, rawX, rawY);
  const bool sw = digitalRead(ChitramInput::kJoyPinSw) == LOW;
  const bool changed = ok != self_->lastOk_ || rawX != self_->lastX_ ||
                       rawY != self_->lastY_ || sw != self_->lastSw_;

  self_->formatJoyLines();

  UIDiv &div = static_cast<UIDiv &>(node);
  if (div.childCount() >= 1 && div.child(0)) {
    static_cast<UIText *>(div.child(0))->setText(self_->joySummary_);
  }

  if (changed) {
    self_->lastOk_ = ok;
    self_->lastX_ = rawX;
    self_->lastY_ = rawY;
    self_->lastSw_ = sw;
    self_->page().invalidateContent();
  }
}

void TestApp::buildJoy(Page &page) {
  const Theme::ThemeTokens &th = Theme::active();

  page.add(page.div()
               .onTick(onJoyTick)
               .style(Style()
                          .setWidth(Length::Pct(100))
                          .setPadding(Edges(12, 10))
                          .setGap(8)
                          .setColumns(1))
               .add(page.text(joySummary_)
                        .style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setFont(FontRole::BodyLarge)
                                   .setColor(th.baseContent)
                                   .setAlign(Align::Center))));
}

void TestApp::enterJoy() {
  pinMode(ChitramInput::kJoyPinSw, INPUT_PULLUP);
  formatJoyLines();
}
