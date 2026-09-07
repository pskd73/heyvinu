#include "launcher_app.h"

#include "ask_app.h"
#include "settings_app.h"
#include "wake_word.h"

#include <string.h>

LauncherApp *LauncherApp::self_ = nullptr;

void LauncherApp::onOpen() {
  self_ = this;
  if (state().magic != LauncherState::kMagic ||
      state().version != LauncherState::kVersion) {
    data() = LauncherState{};
  }
  if (settingsWakeWordEnabled()) {
    wakeWordStart(host());
  } else {
    wakeWordStop();
  }
}

void LauncherApp::onClose() {
  wakeWordStop();
  if (self_ == this) self_ = nullptr;
}

void LauncherApp::openAskFromWake() {
  if (!host()) return;
  wakeWordStop();
  AskApp::requestWakeResume();
  AppInfo all[Flow32::kMaxApps];
  const uint8_t n = host()->getApps(all, Flow32::kMaxApps);
  for (uint8_t i = 0; i < n; i++) {
    if (all[i].name && strcmp(all[i].name, "Ask") == 0) {
      host()->openApp(all[i].index);
      return;
    }
  }
}

void LauncherApp::frame(Canvas &canvas, InputHub &input, float dt) {
  App<LauncherState>::frame(canvas, input, dt);
  if (wakeWordTakeDetected()) {
    openAskFromWake();
  }
}

void LauncherApp::refreshListed() {
  listedCount_ = 0;
  if (!host()) return;

  AppInfo all[Flow32::kMaxApps];
  const uint8_t n = host()->getApps(all, Flow32::kMaxApps);
  for (uint8_t i = 0; i < n && listedCount_ < kMaxListed; i++) {
    if (all[i].name && strcmp(all[i].name, appName()) == 0) continue;
    const uint8_t slot = listedCount_;
    listed_[slot] = all[i];
    iconUtf8_[slot][0] = '\0';
    const char *iconName =
        (all[i].icon && all[i].icon[0]) ? all[i].icon : "app-window";
    if (icons_ && icons_->ready()) {
      icons_->utf8(iconName, iconUtf8_[slot], sizeof(iconUtf8_[slot]));
    }
    listedCount_++;
  }
  clampSelected();
}

void LauncherApp::clampSelected() {
  if (listedCount_ == 0) {
    if (state().selected != 0) set(data().selected, static_cast<int16_t>(0));
    return;
  }
  int16_t sel = state().selected;
  if (sel < 0) sel = 0;
  if (sel >= static_cast<int16_t>(listedCount_)) {
    sel = static_cast<int16_t>(listedCount_ - 1);
  }
  if (sel != state().selected) set(data().selected, sel);
}

void LauncherApp::selectPrev() {
  if (!self_ || self_->listedCount_ == 0) return;
  const int16_t sel = self_->state().selected;
  if (sel <= 0) return;
  self_->set(self_->data().selected, static_cast<int16_t>(sel - 1));
}

void LauncherApp::selectNext() {
  if (!self_ || self_->listedCount_ == 0) return;
  const int16_t sel = self_->state().selected;
  if (sel + 1 >= static_cast<int16_t>(self_->listedCount_)) return;
  self_->set(self_->data().selected, static_cast<int16_t>(sel + 1));
}

void LauncherApp::launchSelected() {
  if (!self_ || !self_->host()) return;
  const int16_t sel = self_->state().selected;
  if (sel < 0 || sel >= static_cast<int16_t>(self_->listedCount_)) return;
  self_->host()->openApp(self_->listed_[static_cast<uint8_t>(sel)].index);
}

void LauncherApp::build(Page &page, uint8_t /*pageId*/) {
  const Theme::ThemeTokens &th = Theme::active();
  const uint16_t muted = Theme::lerp(th.baseContent, th.base100, 0.4f);

  refreshListed();

  const int16_t sel = state().selected;
  const bool canPrev = listedCount_ > 0 && sel > 0;
  const bool canNext =
      listedCount_ > 0 && sel + 1 < static_cast<int16_t>(listedCount_);

  UINode *center = nullptr;
  if (listedCount_ == 0) {
    center = &page.div()
                  .style(Style()
                             .setWidth(Length::Pct(100))
                             .setPadding(Edges(24, 12))
                             .setColumns(1)
                             .setAlignH(Align::Center)
                             .setAlignV(Align::Center))
                  .add(page.text("No apps").style(
                      Style()
                          .setFont(FontRole::Title)
                          .setColor(muted)
                          .setAlign(Align::Center)
                          .setWidth(Length::Pct(100))));
  } else {
    const uint8_t idx = static_cast<uint8_t>(sel);
    const char *name =
        (listed_[idx].name && listed_[idx].name[0]) ? listed_[idx].name
                                                    : "App";
    const char *glyph = iconUtf8_[idx][0] ? iconUtf8_[idx] : "";

    center = &page.div()
                  .style(Style()
                             .setWidth(Length::Pct(100))
                             .setPadding(Edges(20, 16))
                             .setColumns(1)
                             .setGap(12)
                             .setAlignH(Align::Center)
                             .setAlignV(Align::Center))
                  .add(page.text(glyph).style(
                      Style()
                          .setIconSize(72)
                          .setColor(th.baseContent)
                          .setAlign(Align::Center)
                          .setWidth(Length::Pct(100))))
                  .add(page.text(name).style(
                      Style()
                          .setFont(FontRole::Title)
                          .setColor(th.baseContent)
                          .setAlign(Align::Center)
                          .setWidth(Length::Pct(100))));
  }

  auto &stage =
      page.div()
          .style(Style().setWidth(Length::Pct(100)).setColumns(1))
          .add(*center);

  if (canPrev) {
    stage.add(page.text(chevronL_[0] ? chevronL_ : "<")
                  .style(Style()
                             .setPosition(Position::Absolute)
                             .setLeft(Length::Px(4))
                             .setWidth(Length::Px(32))
                             .setIconSize(26)
                             .setColor(th.baseContent)
                             .setAlign(Align::Center)));
  }

  if (canNext) {
    stage.add(page.text(chevronR_[0] ? chevronR_ : ">")
                  .style(Style()
                             .setPosition(Position::Absolute)
                             .setRight(Length::Px(4))
                             .setWidth(Length::Px(32))
                             .setIconSize(26)
                             .setColor(th.baseContent)
                             .setAlign(Align::Center)));
  }

  page.add(page.div()
               .style(Style()
                          .setWidth(Length::Pct(100))
                          .setPadding(Edges(28, 8))
                          .setColumns(1)
                          .setAlignH(Align::Center)
                          .setAlignV(Align::Center))
               .add(stage));
}
