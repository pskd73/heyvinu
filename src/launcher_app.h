#pragma once

#include <Flow32.h>

/**
 * Home launcher — horizontal carousel of installed apps (one at a time).
 * While open, listens for "Hey Vinu" and opens the last Ask agent.
 */
struct LauncherState {
  static constexpr uint32_t kMagic = 0x4C4E4352u; // 'LNCR'
  static constexpr uint16_t kVersion = 2;

  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint16_t reserved = 0;

  /** Index into the listed (non-launcher) apps. */
  int16_t selected = 0;
};

class LauncherApp : public App<LauncherState> {
public:
  static constexpr uint8_t kMaxListed = 12;
  static constexpr uint8_t kIconLabelCap = 12;

  explicit LauncherApp(const Rect &viewport) : App(viewport) {
    setAppInfo("Launcher", "layout-grid");
    addPage("Apps");
    self_ = this;
  }

  void onAssets(IconSd *icons, ColorEmojiSd * /*emoji*/) override {
    icons_ = icons;
    if (icons_ && icons_->ready()) {
      icons_->utf8("chevron-left", chevronL_, sizeof(chevronL_));
      icons_->utf8("chevron-right", chevronR_, sizeof(chevronR_));
    }
  }

  void frame(Canvas &canvas, InputHub &input, float dt) override;

protected:
  const char *nvsNamespace() const override { return "launch"; }

  bool handleKey(UIEvent &e) override {
    if (e.phase != UIKeyPhase::Down && e.phase != UIKeyPhase::Hold) {
      return false;
    }
    // Horizontal carousel — ignore up/down so they don't fall through to Page
    // (which would also move selection).
    if (e.key == UIKey::Up || e.key == UIKey::Down) {
      return true;
    }
    if (e.key == UIKey::Left) {
      selectPrev();
      return true;
    }
    if (e.key == UIKey::Right) {
      selectNext();
      return true;
    }
    if (e.key == UIKey::Select) {
      launchSelected();
      return true;
    }
    return false;
  }

  void onOpen() override;
  void onClose() override;

  void build(Page &page, uint8_t /*pageId*/) override;

private:
  static LauncherApp *self_;
  IconSd *icons_ = nullptr;

  AppInfo listed_[kMaxListed] = {};
  uint8_t listedCount_ = 0;
  char iconUtf8_[kMaxListed][kIconLabelCap] = {};
  char chevronL_[kIconLabelCap] = {};
  char chevronR_[kIconLabelCap] = {};

  void refreshListed();
  void clampSelected();
  void openAskFromWake();
  static void selectPrev();
  static void selectNext();
  static void launchSelected();
};
