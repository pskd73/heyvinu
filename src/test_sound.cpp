#include "test_app.h"

#include "mp3_player.h"

#include <stdio.h>

namespace {

constexpr const char *kMp3Path = "/file_example_MP3_700KB.mp3";

Mp3Player player_;

bool startMp3(TestApp *self) {
  player_.stop();
  Storage *sd = self && self->host() ? self->host()->storage() : nullptr;
  if (!player_.begin(sd, kMp3Path)) {
    Serial.printf("Sound: failed (%s)\n", player_.lastError());
    return false;
  }
  return true;
}

} // namespace



void TestApp::buildSound(Page &page) {
  const Theme::ThemeTokens &th = Theme::active();
  const uint16_t muted = Theme::lerp(th.baseContent, th.base100, 0.4f);
  const char *status =
      player_.lastError()[0] ? player_.lastError() : "Loading…";

  page.add(page.div()
               .style(Style()
                          .setWidth(Length::Pct(100))
                          .setPadding(Edges(24, 16))
                          .setGap(10)
                          .setColumns(1)
                          .setAlignH(Align::Center)
                          .setAlignV(Align::Center))
               .add(page.text("Speaker test").style(
                   Style()
                       .setFont(FontRole::Body)
                       .setColor(th.baseContent)
                       .setAlign(Align::Center)
                       .setWidth(Length::Pct(100))))
               .add(page.text("file_example_MP3_700KB.mp3").style(
                   Style()
                       .setFont(FontRole::Small)
                       .setColor(muted)
                       .setAlign(Align::Center)
                       .setWidth(Length::Pct(100))))
               .add(page.text(status).style(
                   Style()
                       .setFont(FontRole::Small)
                       .setColor(muted)
                       .setAlign(Align::Center)
                       .setWidth(Length::Pct(100))))
               .add(page.text("Hold stick = back").style(
                   Style()
                       .setFont(FontRole::Small)
                       .setColor(muted)
                       .setAlign(Align::Center)
                       .setWidth(Length::Pct(100)))));
}

void TestApp::pumpAudio() {
  if (!player_.isRunning()) {
    return;
  }
  if (!player_.pump()) {
    startMp3(this);
  }
}

void TestApp::enterSound() { startMp3(this); }

void TestApp::leaveSound() { player_.stop(); }
