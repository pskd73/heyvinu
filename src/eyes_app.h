#pragma once

#include <Flow32.h>

/** Minimal NVS blob — Eyes is mostly ephemeral animation. */
struct EyesState {
  static constexpr uint32_t kMagic = 0x45594553u; // 'EYES'
  static constexpr uint16_t kVersion = 1;

  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint16_t reserved = 0;
};

/**
 * Fullscreen black void with a pair of mono green rounded-box eyes that blink.
 */
class EyesApp : public App<EyesState> {
public:
  explicit EyesApp(const Rect &viewport) : App(viewport) {
    setAppInfo("Eyes", "eye");
    addPage("Eyes", /*fullscreen=*/true);
  }

  void frame(Canvas &canvas, InputHub &input, float dt) override;

protected:
  const char *nvsNamespace() const override { return "eyes"; }

  void onOpen() override {
    if (state().magic != EyesState::kMagic ||
        state().version != EyesState::kVersion) {
      data() = EyesState{};
    }
    blinkAmt_ = 0.f;
    blinkDir_ = 0;
    nextBlinkIn_ = 1.2f;
  }

  void build(Page &page, uint8_t /*pageId*/) override {
    page.add(page.div());
  }

private:
  float blinkAmt_ = 0.f;
  int8_t blinkDir_ = 0;
  float nextBlinkIn_ = 2.f;
  uint8_t blinksLeft_ = 0;

  void updateBlink(float dt);
  void drawEyes(Display &d, const Rect &vp) const;
};
