#include "eyes_app.h"

#include <math.h>

namespace {

constexpr float kBlinkSpeed = 9.f;
constexpr float kMinGap = 1.4f;
constexpr float kMaxGap = 4.2f;
constexpr int16_t kMargin = 16;
const uint16_t kEyeColor = Theme::rgb(0, 220, 70);

float frand01() {
  return static_cast<float>(esp_random() & 0xFFFFu) / 65535.f;
}

} // namespace

void EyesApp::updateBlink(float dt) {
  if (blinkDir_ == 0) {
    nextBlinkIn_ -= dt;
    if (nextBlinkIn_ > 0.f) return;
    blinkDir_ = +1;
    blinksLeft_ = (frand01() < 0.28f) ? 1 : 0;
    return;
  }

  blinkAmt_ += static_cast<float>(blinkDir_) * kBlinkSpeed * dt;
  if (blinkAmt_ >= 1.f) {
    blinkAmt_ = 1.f;
    blinkDir_ = -1;
  } else if (blinkAmt_ <= 0.f) {
    blinkAmt_ = 0.f;
    if (blinksLeft_ > 0) {
      blinksLeft_--;
      blinkDir_ = +1;
    } else {
      blinkDir_ = 0;
      nextBlinkIn_ = kMinGap + frand01() * (kMaxGap - kMinGap);
    }
  }
}

void EyesApp::drawEyes(Display &d, const Rect &vp) const {
  const int16_t boxX = static_cast<int16_t>(vp.x + kMargin);
  const int16_t boxY = static_cast<int16_t>(vp.y + kMargin);
  const int16_t boxW = static_cast<int16_t>(vp.w - 2 * kMargin);
  const int16_t boxH = static_cast<int16_t>(vp.h - 2 * kMargin);
  if (boxW < 8 || boxH < 8) return;

  const int16_t gap = static_cast<int16_t>(boxW * 8 / 100);
  const int16_t eyeW = static_cast<int16_t>((boxW - gap) / 2);
  int16_t eyeH = static_cast<int16_t>(eyeW * 70 / 100);
  if (eyeH > boxH) eyeH = boxH;

  const float open = 1.f - blinkAmt_;
  int16_t h = static_cast<int16_t>(lroundf(static_cast<float>(eyeH) * open));
  if (h < 3) h = 3;

  const int16_t cy = static_cast<int16_t>(boxY + (boxH - h) / 2);
  const int16_t leftX = boxX;
  const int16_t rightX = static_cast<int16_t>(boxX + eyeW + gap);

  int16_t rad = static_cast<int16_t>(eyeW * 18 / 100);
  if (rad > h / 2) rad = static_cast<int16_t>(h / 2);
  if (rad < 2) rad = 2;

  d.fillRoundRect(leftX, cy, eyeW, h, rad, kEyeColor);
  d.fillRoundRect(rightX, cy, eyeW, h, rad, kEyeColor);
}

void EyesApp::frame(Canvas &canvas, InputHub &input, float dt) {
  UIEvent e;
  while (input.pop(e)) {
  }

  updateBlink(dt);

  Display &d = canvas.display();
  const Rect vp = page().viewport();

  canvas.setOrigin(0, 0);
  canvas.clearClip();
  d.clear(0x0000);
  drawEyes(d, vp);
  d.present();
}
