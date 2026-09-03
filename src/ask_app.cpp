#include "ask_app.h"

#include "talk_agent.h"

#include <stdio.h>
#include <string.h>

AskApp *AskApp::self_ = nullptr;

void AskApp::onOpen() {
  self_ = this;
  if (state().magic != AskState::kMagic ||
      state().version != AskState::kVersion) {
    data() = AskState{};
  }
  started_ = false;
  failed_ = false;
  pendingStart_ = true;
  errMsg_[0] = '\0';
  lastUiMs_ = 0;
  snprintf(statusLine_, sizeof(statusLine_), "Connecting...");
}

void AskApp::onClose() {
  pendingStart_ = false;
  talkAgentStop();
  started_ = false;
  if (self_ == this) {
    self_ = nullptr;
  }
}

void AskApp::formatStatus() {
  if (failed_) {
    snprintf(statusLine_, sizeof(statusLine_), "%s",
             errMsg_[0] ? errMsg_ : "Failed");
  } else if (pendingStart_ || !started_) {
    snprintf(statusLine_, sizeof(statusLine_), "Connecting...");
  } else {
    snprintf(statusLine_, sizeof(statusLine_), "%s", talkAgentStatus());
  }
}

void AskApp::onAskTick(UINode &node, float dt) {
  (void)dt;
  if (!self_) {
    return;
  }

  // Let the first UI frame paint before WiFi/TLS/I2S allocate heavily.
  if (self_->pendingStart_) {
    self_->pendingStart_ = false;
    if (!talkAgentStart(self_->host())) {
      self_->failed_ = true;
      snprintf(self_->errMsg_, sizeof(self_->errMsg_), "%s", talkAgentStatus());
    } else {
      self_->started_ = true;
    }
    self_->formatStatus();
    self_->page().invalidateContent();
    return;
  }

  talkAgentLoop();

  if (self_->started_ && !self_->failed_ && !talkAgentIsActive()) {
    self_->failed_ = true;
    snprintf(self_->errMsg_, sizeof(self_->errMsg_), "%s", talkAgentStatus());
  }

  const uint32_t now = millis();
  if (now - self_->lastUiMs_ < kUiMinMs) {
    return;
  }

  char prevStatus[sizeof(self_->statusLine_)];
  memcpy(prevStatus, self_->statusLine_, sizeof(prevStatus));
  self_->formatStatus();

  if (strcmp(prevStatus, self_->statusLine_) == 0) {
    return;
  }

  // Avoid full-page SPI presents while TTS is playing (PSRAM bus fight).
  if (talkAgentIsSpeaking()) {
    return;
  }

  self_->lastUiMs_ = now;

  UIDiv &div = static_cast<UIDiv &>(node);
  if (div.childCount() >= 1 && div.child(0)) {
    static_cast<UIText *>(div.child(0))->setText(self_->statusLine_);
  }

  self_->page().invalidateContent();
}

void AskApp::build(Page &page, uint8_t /*pageId*/) {
  const Theme::ThemeTokens &th = Theme::active();
  const uint16_t muted = Theme::lerp(th.baseContent, th.base100, 0.4f);

  page.add(page.div()
               .onTick(onAskTick)
               .style(Style()
                          .setWidth(Length::Pct(100))
                          .setHeight(Length::Pct(100))
                          .setPadding(Edges(12, 10))
                          .setGap(12)
                          .setColumns(1)
                          .setAlignH(Align::Center)
                          .setAlignV(Align::Center))
               .add(page.text(statusLine_)
                        .style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setFont(FontRole::Small)
                                   .setColor(failed_ ? th.warning : muted)
                                   .setAlign(Align::Center))));
}
