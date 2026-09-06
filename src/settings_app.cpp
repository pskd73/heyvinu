#include "settings_app.h"

#include "runtime_config.h"
#include "voice_context.h"

#include <stdio.h>
#include <string.h>

SettingsApp *SettingsApp::self_ = nullptr;

namespace {
bool gVisualiseEnabled = true;
bool gWakeWordEnabled = true;
}

bool settingsVisualiseEnabled() { return gVisualiseEnabled; }

void settingsCacheVisualise(bool on) { gVisualiseEnabled = on; }

bool settingsWakeWordEnabled() { return gWakeWordEnabled; }

void settingsCacheWakeWord(bool on) { gWakeWordEnabled = on; }

const char *SettingsApp::pageTitle(uint8_t id) const {
  if (id == kPageClearContext) return "Clear context";
  return App<SettingsState>::pageTitle(id);
}

void SettingsApp::frame(Canvas &canvas, InputHub &input, float dt) {
  App<SettingsState>::frame(canvas, input, dt);
  if (listFocusPending_ && pageId() == kPageClearContext &&
      page().focusCount() > 0) {
    page().setFocusIndex(0);
    listFocusPending_ = false;
  }
}

void SettingsApp::onThemeSelect(UISelect &s) {
  if (!self_) return;
  self_->setTheme(s.selected());
  Serial.printf("Settings: theme=%d (%s)\n", (int)self_->state().theme,
                Theme::active().name ? Theme::active().name : "?");
}

void SettingsApp::onVisualiseChange(UIToggle &t) {
  if (!self_) return;
  self_->setVisualise(t.checked());
  Serial.printf("Settings: visualise=%d\n", t.checked() ? 1 : 0);
}

void SettingsApp::onWakeWordChange(UIToggle &t) {
  if (!self_) return;
  self_->setWakeWord(t.checked());
  Serial.printf("Settings: wake_word=%d\n", t.checked() ? 1 : 0);
}

void SettingsApp::onVoiceProviderSelect(UISelect &s) {
  if (!self_) return;
  const char *v = (s.selected() == 1) ? "deepgram" : "elevenlabs";
  setConfig(Config::VoiceProvider, v);
  Serial.printf("Settings: voice_provider=%s\n", v);
  if (self_->host()) {
    char toast[40];
    snprintf(toast, sizeof(toast), "Voice: %s",
             s.selected() == 1 ? "Deepgram" : "ElevenLabs");
    self_->host()->showToast(toast, ToastKind::Success);
  }
}

void SettingsApp::onMainMenu(UISelect &s) {
  if (!self_) return;
  if (s.selectedValue() == kPageClearContext) {
    self_->refreshContextList();
    self_->listFocusPending_ = true;
    self_->goTo(kPageClearContext);
  }
}

void SettingsApp::onClearContextSelect(UISelect &s) {
  if (!self_) return;
  const int16_t idx = s.selectedValue();
  if (idx < 0 || idx >= self_->contextCount_) return;

  Storage *st = self_->host() ? self_->host()->storage() : nullptr;
  const char *id = self_->contextNames_[idx];
  const bool ok = voiceContextClear(st, id);
  Serial.printf("Settings: clear context %s %s\n", id, ok ? "ok" : "fail");

  char toast[48];
  snprintf(toast, sizeof(toast), ok ? "Cleared %s" : "Failed %s", id);
  if (self_->host()) {
    self_->host()->showToast(toast,
                             ok ? ToastKind::Success : ToastKind::Error);
  }

  self_->refreshContextList();
  self_->listFocusPending_ = self_->contextCount_ > 0;
  self_->requestRebuild();
}

void SettingsApp::refreshContextList() {
  contextCount_ = 0;
  for (int i = 0; i < kMaxContexts; i++) {
    contextNames_[i][0] = '\0';
  }
  Storage *st = host() ? host()->storage() : nullptr;
  contextCount_ = voiceContextList(st, &contextNames_[0][0], kContextNameLen,
                                  kMaxContexts);
}

void SettingsApp::buildMain(Page &page) {
  const SettingsState &st = state();
  const Theme::ThemeTokens &th = Theme::active();
  const uint16_t muted = Theme::lerp(th.baseContent, th.base100, 0.4f);
  const char *vp = getConfig(Config::VoiceProvider);
  const int16_t voiceSel =
      (vp && (strcmp(vp, "deepgram") == 0 || strcmp(vp, "dg") == 0)) ? 1 : 0;

  auto sectionLabel = [&](const char *label) -> UIText & {
    return page.text(label).style(Style()
                                      .setWidth(Length::Pct(100))
                                      .setFont(FontRole::Small)
                                      .setColor(muted)
                                      .setAlign(Align::Start));
  };

  auto &chooser =
      page.select()
          .selected(st.theme)
          .onChange(onThemeSelect)
          .style(Style().setWidth(Length::Pct(100)).setGap(6))
          .add(page.selectOption()
                   .icon("sun")
                   .title("Flow")
                   .description("Warm dark surfaces")
                   .value(0))
          .add(page.selectOption()
                   .icon("snowflake")
                   .title("Winter")
                   .description("Cool light surfaces")
                   .value(1));

  auto &voiceChooser =
      page.select()
          .selected(voiceSel)
          .onChange(onVoiceProviderSelect)
          .style(Style().setWidth(Length::Pct(100)).setGap(6))
          .add(page.selectOption()
                   .icon("mic")
                   .title("ElevenLabs")
                   .description("ConvAI voice agent")
                   .value(0))
          .add(page.selectOption()
                   .icon("radio")
                   .title("Deepgram")
                   .description("Voice Agent (Aura + Flux)")
                   .value(1));

  auto &vizRow =
      page.div()
          .style(Style()
                     .setWidth(Length::Pct(100))
                     .setHeight(Length::Px(30)))
          .add(page.text("Visualise")
                   .style(Style()
                              .setPosition(Position::Absolute)
                              .setLeft(Length::Px(0))
                              .setFont(FontRole::Small)
                              .setColor(th.baseContent)
                              .setAlign(Align::Start)))
          .add(page.toggle()
                   .checked(st.visualise != 0)
                   .onChange(onVisualiseChange)
                   .style(Style()
                              .setPosition(Position::Absolute)
                              .setRight(Length::Px(0))
                              .setWidth(Length::Px(52))
                              .setHeight(Length::Px(30))));

  auto &wakeRow =
      page.div()
          .style(Style()
                     .setWidth(Length::Pct(100))
                     .setHeight(Length::Px(30)))
          .add(page.text("Wake word")
                   .style(Style()
                              .setPosition(Position::Absolute)
                              .setLeft(Length::Px(0))
                              .setFont(FontRole::Small)
                              .setColor(th.baseContent)
                              .setAlign(Align::Start)))
          .add(page.toggle()
                   .checked(st.wakeWord != 0)
                   .onChange(onWakeWordChange)
                   .style(Style()
                              .setPosition(Position::Absolute)
                              .setRight(Length::Px(0))
                              .setWidth(Length::Px(52))
                              .setHeight(Length::Px(30))));

  auto &tools =
      page.select()
          .onChange(onMainMenu)
          .style(Style().setWidth(Length::Pct(100)).setGap(6))
          .add(page.selectOption()
                   .icon("trash-2")
                   .title("Clear context")
                   .description("Wipe saved Talk history")
                   .value(kPageClearContext));

  page.add(page.div()
               .style(Style()
                          .setWidth(Length::Pct(100))
                          .setPadding(Edges(16, 12))
                          .setGap(14)
                          .setColumns(1))
               .add(page.div()
                        .style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setGap(6)
                                   .setColumns(1))
                        .add(sectionLabel("THEME"))
                        .add(chooser))
               .add(page.div()
                        .style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setGap(6)
                                   .setColumns(1))
                        .add(sectionLabel("AI"))
                        .add(voiceChooser)
                        .add(vizRow)
                        .add(wakeRow)
                        .add(tools)));
}

void SettingsApp::buildClearContext(Page &page) {
  const Theme::ThemeTokens &th = Theme::active();
  const uint16_t muted = Theme::lerp(th.baseContent, th.base100, 0.4f);

  auto &root =
      page.div().style(Style()
                           .setWidth(Length::Pct(100))
                           .setPadding(Edges(16, 12))
                           .setGap(10)
                           .setColumns(1));

  if (contextCount_ <= 0) {
    root.add(page.text("No saved contexts")
                 .style(Style()
                            .setWidth(Length::Pct(100))
                            .setFont(FontRole::Small)
                            .setColor(muted)
                            .setAlign(Align::Start)));
    page.add(root);
    return;
  }

  auto &list =
      page.select()
          .onChange(onClearContextSelect)
          .style(Style().setWidth(Length::Pct(100)).setGap(6));

  for (int i = 0; i < contextCount_; i++) {
    list.add(page.selectOption()
                 .icon("message-circle")
                 .title(contextNames_[i])
                 .description("Select to clear")
                 .value(static_cast<int16_t>(i)));
  }

  root.add(list);
  page.add(root);
}

void SettingsApp::build(Page &page, uint8_t pageId) {
  if (pageId == kPageClearContext) {
    buildClearContext(page);
  } else {
    buildMain(page);
  }
}
