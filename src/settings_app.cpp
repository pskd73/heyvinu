#include "settings_app.h"

#include <stdio.h>

SettingsApp *SettingsApp::self_ = nullptr;

void SettingsApp::formatVolumeLabel() {
  snprintf(volumeLabel_, sizeof(volumeLabel_), "Volume  %d%%",
           (int)state().volume);
}

void SettingsApp::onThemeSelect(UISelect &s) {
  if (!self_) return;
  self_->setTheme(s.selected());
  Serial.printf("Settings: theme=%d (%s)\n", (int)self_->state().theme,
                Theme::active().name ? Theme::active().name : "?");
}

void SettingsApp::onVolumeChange(UIRange &r) {
  if (!self_) return;
  self_->setVolume(r.value());
}

void SettingsApp::build(Page &page, uint8_t /*pageId*/) {
  const SettingsState &st = state();
  const Theme::ThemeTokens &th = Theme::active();
  const uint16_t muted = Theme::lerp(th.baseContent, th.base100, 0.4f);

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

  auto &vol =
      page.range()
          .min(ChitramAudio::kVolumeMin)
          .max(ChitramAudio::kVolumeMax)
          .step(5)
          .value(st.volume)
          .onChange(onVolumeChange)
          .style(Style().setWidth(Length::Pct(100)));

  page.add(page.div()
               .style(Style()
                          .setWidth(Length::Pct(100))
                          .setPadding(Edges(16, 12))
                          .setGap(14)
                          .setColumns(1))
               .add(chooser)
               .add(page.div()
                        .style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setGap(8)
                                   .setColumns(1))
                        .add(page.text(volumeLabel_)
                                 .style(Style()
                                            .setWidth(Length::Pct(100))
                                            .setFont(FontRole::Small)
                                            .setColor(muted)
                                            .setAlign(Align::Start)))
                        .add(vol)));
}
