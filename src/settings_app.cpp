#include "settings_app.h"

SettingsApp *SettingsApp::self_ = nullptr;

namespace {
bool gVisualiseEnabled = true;
}

bool settingsVisualiseEnabled() { return gVisualiseEnabled; }

void settingsCacheVisualise(bool on) { gVisualiseEnabled = on; }

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

void SettingsApp::build(Page &page, uint8_t /*pageId*/) {
  const SettingsState &st = state();
  const Theme::ThemeTokens &th = Theme::active();

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

  page.add(page.div()
               .style(Style()
                          .setWidth(Length::Pct(100))
                          .setPadding(Edges(16, 12))
                          .setGap(14)
                          .setColumns(1))
               .add(chooser)
               .add(vizRow));
}
