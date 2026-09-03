#include "remote_app.h"

#include "remote_server.h"

#include <stdio.h>
#include <string.h>

RemoteApp *RemoteApp::self_ = nullptr;

void RemoteApp::formatLines() {
  if (started_ && !failed_) {
    RemoteServerInfo info{};
    remoteServerGetInfo(info);
    snprintf(ssidLine_, sizeof(ssidLine_), "Wi-Fi: %s", info.apSsid);
    snprintf(ipLine_, sizeof(ipLine_), "IP: %s", info.ip);
    snprintf(passLine_, sizeof(passLine_), "Pass: %s", info.apPassword);
    const char *msg = remoteServerLastMessage();
    if (msg && msg[0] && strcmp(msg, "AP running") != 0 &&
        strcmp(msg, "Ready") != 0) {
      snprintf(statusLine_, sizeof(statusLine_), "%s", msg);
    } else {
      snprintf(statusLine_, sizeof(statusLine_), "Open http://%s", info.ip);
    }
  } else if (failed_) {
    snprintf(ssidLine_, sizeof(ssidLine_), "Remote failed");
    ipLine_[0] = '\0';
    passLine_[0] = '\0';
    snprintf(statusLine_, sizeof(statusLine_), "%s",
             remoteServerLastMessage());
  } else {
    snprintf(ssidLine_, sizeof(ssidLine_), "Starting AP...");
    ipLine_[0] = '\0';
    passLine_[0] = '\0';
    statusLine_[0] = '\0';
  }
}

void RemoteApp::onOpen() {
  self_ = this;
  if (state().magic != RemoteState::kMagic ||
      state().version != RemoteState::kVersion) {
    data() = RemoteState{};
  }

  started_ = false;
  failed_ = false;
  lastUiMs_ = 0;

  RemoteServerInfo info{};
  if (remoteServerStart(info)) {
    started_ = true;
  } else {
    failed_ = true;
  }
  formatLines();
}

void RemoteApp::onClose() {
  remoteServerStop();
  started_ = false;
  if (self_ == this) {
    self_ = nullptr;
  }
}

void RemoteApp::onRemoteTick(UINode &node, float dt) {
  (void)dt;
  if (!self_) {
    return;
  }

  remoteServerLoop();

  const uint32_t now = millis();
  if (now - self_->lastUiMs_ < kUiMinMs) {
    return;
  }

  char prevStatus[sizeof(self_->statusLine_)];
  memcpy(prevStatus, self_->statusLine_, sizeof(prevStatus));
  self_->formatLines();

  if (strcmp(prevStatus, self_->statusLine_) == 0 &&
      self_->started_ && !self_->failed_) {
    return;
  }

  self_->lastUiMs_ = now;

  UIDiv &div = static_cast<UIDiv &>(node);
  if (div.childCount() >= 4) {
    static_cast<UIText *>(div.child(0))->setText(self_->ssidLine_);
    static_cast<UIText *>(div.child(1))->setText(self_->ipLine_);
    static_cast<UIText *>(div.child(2))->setText(self_->passLine_);
    static_cast<UIText *>(div.child(3))->setText(self_->statusLine_);
  }

  self_->page().invalidateContent();
}

void RemoteApp::build(Page &page, uint8_t /*pageId*/) {
  const Theme::ThemeTokens &th = Theme::active();
  const uint16_t muted = Theme::lerp(th.baseContent, th.base100, 0.4f);
  const uint16_t accent = failed_ ? th.warning : th.primary;

  formatLines();

  page.add(page.div()
               .onTick(onRemoteTick)
               .style(Style()
                          .setWidth(Length::Pct(100))
                          .setHeight(Length::Pct(100))
                          .setPadding(Edges(12, 10))
                          .setGap(10)
                          .setColumns(1)
                          .setAlignH(Align::Center)
                          .setAlignV(Align::Center))
               .add(page.text(ssidLine_)
                        .style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setFont(FontRole::Body)
                                   .setColor(th.baseContent)
                                   .setAlign(Align::Center)))
               .add(page.text(ipLine_)
                        .style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setFont(FontRole::BodyLarge)
                                   .setColor(accent)
                                   .setAlign(Align::Center)))
               .add(page.text(passLine_)
                        .style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setFont(FontRole::Small)
                                   .setColor(muted)
                                   .setAlign(Align::Center)))
               .add(page.text(statusLine_)
                        .style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setFont(FontRole::Small)
                                   .setColor(muted)
                                   .setAlign(Align::Center))));
}
