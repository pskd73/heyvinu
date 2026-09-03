#include "ask_app.h"

#include "audio_volume.h"
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
  agentCount_ = 0;
  agentsMore_ = false;
  moreLine_[0] = '\0';
  listFocusPending_ = false;
  pendingFetch_ = true;
  warmupFrames_ = 1;
  snprintf(statusLine_, sizeof(statusLine_), "Loading agents...");
  // Flags only — onClose already tore down any previous session, and calling
  // stop here would log a spurious teardown on every open.
  resetTalkState();
}

void AskApp::onClose() {
  leaveTalk();
  if (self_ == this) {
    self_ = nullptr;
  }
}

bool AskApp::goBack() {
  if (pageId() == kPageTalk) {
    leaveTalk();
  }
  return back();
}

bool AskApp::handleKey(UIEvent &e) {
  // Only during a conversation: on the picker Up/Down has to stay list
  // navigation. Every phase is consumed so Page never sees a stray release
  // and scrolls the page out from under the reading.
  if (pageId() != kPageTalk) {
    return false;
  }
  if (e.key != UIKey::Up && e.key != UIKey::Down) {
    return false;
  }
  if (e.phase != UIKeyPhase::Up) {
    adjustVolume(e.key == UIKey::Up ? kVolumeStep : -kVolumeStep);
  }
  return true;
}

void AskApp::frame(Canvas &canvas, InputHub &input, float dt) {
  App<AskState>::frame(canvas, input, dt);

  // The list is the whole point of the page, so put the ring on it rather than
  // making Select-to-enter-focus the first step.
  if (listFocusPending_ && pageId() == kPageAgents && page().focusCount() > 0) {
    page().setFocusIndex(0);
    listFocusPending_ = false;
  }
}

void AskApp::build(Page &page, uint8_t pageId) {
  if (pageId == kPageAgents) {
    buildAgents(page);
  } else if (pageId == kPageTalk) {
    buildTalk(page);
  } else {
    buildStatus(page);
  }
}

// --- Agent list ---------------------------------------------------------

void AskApp::runAgentFetch() {
  char err[48] = {};
  bool more = false;
  const int n = talkAgentFetchList(agents_, kMaxAgents, &more, err, sizeof(err));

  if (n < 0) {
    snprintf(statusLine_, sizeof(statusLine_), "%s",
             err[0] ? err : "Agent list failed");
    return;
  }
  if (n == 0) {
    snprintf(statusLine_, sizeof(statusLine_), "No agents on this account");
    return;
  }

  agentCount_ = n;
  agentsMore_ = more;
  if (more) {
    snprintf(moreLine_, sizeof(moreLine_), "First %d of more", n);
  } else {
    moreLine_[0] = '\0';
  }
  listFocusPending_ = true;
  // Replace, not push: Back from the list should leave the app, not return to
  // a loading screen that would fetch all over again.
  goTo(kPageAgents, NavMode::Replace);
}

void AskApp::onStatusTick(UINode &node, float dt) {
  (void)dt;
  AskApp *self = self_;
  if (!self || !self->pendingFetch_) {
    return;
  }
  if (self->warmupFrames_ > 0) {
    self->warmupFrames_--;
    return;
  }

  self->pendingFetch_ = false;
  self->runAgentFetch();
  if (self->pageId() != kPageStatus) {
    return; // moved on to the list; that page rebuilds next frame
  }

  UIDiv &div = static_cast<UIDiv &>(node);
  if (div.childCount() >= 1 && div.child(0)) {
    static_cast<UIText *>(div.child(0))->setText(self->statusLine_);
  }
  self->page().invalidateContent();
}

void AskApp::onRetryPress(UIButton &btn) {
  (void)btn;
  AskApp *self = self_;
  if (!self || self->pendingFetch_) {
    return;
  }
  snprintf(self->statusLine_, sizeof(self->statusLine_), "Loading agents...");
  self->pendingFetch_ = true;
  self->warmupFrames_ = 1;
  self->page().invalidateContent();
}

void AskApp::onAgentSelect(UISelect &sel) {
  if (!self_) {
    return;
  }
  self_->openTalk(sel.selectedValue());
}

void AskApp::buildStatus(Page &page) {
  const Theme::ThemeTokens &th = Theme::active();
  const uint16_t muted = Theme::lerp(th.baseContent, th.base100, 0.4f);

  page.add(page.div()
               .onTick(onStatusTick)
               .style(Style()
                          .setWidth(Length::Pct(100))
                          .setHeight(Length::Pct(100))
                          .setPadding(Edges(12, 10))
                          .setGap(14)
                          .setColumns(1)
                          .setAlignH(Align::Center)
                          .setAlignV(Align::Center))
               .add(page.text(statusLine_)
                        .style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setFont(FontRole::Small)
                                   .setColor(muted)
                                   .setAlign(Align::Center)))
               .add(page.button()
                        .color(ButtonColor::Secondary)
                        .variant(ButtonVariant::Soft)
                        .icon("refresh-cw")
                        .onPress(onRetryPress)
                        .add(page.text("Retry").style(
                            Style().setFont(FontRole::Small)))));
}

void AskApp::buildAgents(Page &page) {
  const Theme::ThemeTokens &th = Theme::active();
  const uint16_t muted = Theme::lerp(th.baseContent, th.base100, 0.4f);

  // selected(-1): these rows are navigation, not a remembered radio choice.
  auto &list = page.select()
                   .selected(-1)
                   .onChange(onAgentSelect)
                   .style(Style().setWidth(Length::Pct(100)).setGap(6));
  for (int i = 0; i < agentCount_; i++) {
    list.add(page.selectOption()
                 .icon("bot-message-square")
                 .title(agents_[i].name)
                 .value((int16_t)i));
  }

  // Centered while the list is short; axisAlign yields nothing once the rows
  // outgrow the viewport, so a full list still starts at the top and scrolls.
  auto &col = page.div().style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setHeight(Length::Pct(100))
                                   .setPadding(Edges(16, 12))
                                   .setGap(10)
                                   .setColumns(1)
                                   .setAlignV(Align::Center));
  col.add(list);
  if (moreLine_[0]) {
    col.add(page.text(moreLine_)
                .style(Style()
                           .setWidth(Length::Pct(100))
                           .setFont(FontRole::Small)
                           .setColor(muted)
                           .setAlign(Align::Center)));
  }
  page.add(col);
}

// --- Live conversation --------------------------------------------------

void AskApp::openTalk(int index) {
  if (index < 0 || index >= agentCount_) {
    return;
  }
  snprintf(agentId_, sizeof(agentId_), "%s", agents_[index].id);
  snprintf(talkTitle_, sizeof(talkTitle_), "%s", agents_[index].name);
  snprintf(talkLine_, sizeof(talkLine_), "Connecting...");
  formatVolumeLine();
  volumeDirty_ = false;
  started_ = false;
  failed_ = false;
  errMsg_[0] = '\0';
  lastUiMs_ = 0;
  pendingStart_ = true;
  warmupFrames_ = 1;
  goTo(kPageTalk);
}

void AskApp::resetTalkState() {
  pendingStart_ = false;
  started_ = false;
  failed_ = false;
  errMsg_[0] = '\0';
  lastUiMs_ = 0;
}

void AskApp::leaveTalk() {
  resetTalkState();
  talkAgentStop();
}

void AskApp::formatVolumeLine() {
  snprintf(volumeLine_, sizeof(volumeLine_), "Volume %d%%",
           (int)ChitramAudio::volumePercent());
}

bool AskApp::adjustVolume(int16_t delta) {
  const int16_t cur = ChitramAudio::volumePercent();
  int16_t next = static_cast<int16_t>(cur + delta);
  if (next < ChitramAudio::kVolumeMin) next = ChitramAudio::kVolumeMin;
  if (next > ChitramAudio::kVolumeMax) next = ChitramAudio::kVolumeMax;
  if (next == cur) {
    return false;
  }
  ChitramAudio::setVolumePercent(next);
  formatVolumeLine();
  // The label repaints through the normal tick, which holds off while the
  // agent is speaking — the gain itself already changed.
  volumeDirty_ = true;
  return true;
}

void AskApp::formatTalkStatus() {
  if (failed_) {
    snprintf(talkLine_, sizeof(talkLine_), "%s",
             errMsg_[0] ? errMsg_ : "Failed");
  } else if (pendingStart_ || !started_) {
    snprintf(talkLine_, sizeof(talkLine_), "Connecting...");
  } else {
    snprintf(talkLine_, sizeof(talkLine_), "%s", talkAgentStatus());
  }
}

void AskApp::onTalkTick(UINode &node, float dt) {
  (void)dt;
  AskApp *self = self_;
  if (!self) {
    return;
  }

  if (self->pendingStart_) {
    if (self->warmupFrames_ > 0) {
      self->warmupFrames_--;
      return;
    }
    self->pendingStart_ = false;
    if (!talkAgentStart(self->host(), self->agentId_)) {
      self->failed_ = true;
      snprintf(self->errMsg_, sizeof(self->errMsg_), "%s", talkAgentStatus());
    } else {
      self->started_ = true;
    }
    self->formatTalkStatus();
    self->page().invalidateContent();
    return;
  }

  talkAgentLoop();

  if (self->started_ && !self->failed_ && !talkAgentIsActive()) {
    self->failed_ = true;
    snprintf(self->errMsg_, sizeof(self->errMsg_), "%s", talkAgentStatus());
  }

  const uint32_t now = millis();
  if (now - self->lastUiMs_ < kUiMinMs) {
    return;
  }

  char prev[sizeof(self->talkLine_)];
  memcpy(prev, self->talkLine_, sizeof(prev));
  self->formatTalkStatus();

  if (strcmp(prev, self->talkLine_) == 0 && !self->volumeDirty_) {
    return;
  }

  // Avoid full-page SPI presents while TTS is playing (PSRAM bus fight).
  if (talkAgentIsSpeaking()) {
    return;
  }

  self->lastUiMs_ = now;
  self->volumeDirty_ = false;

  UIDiv &div = static_cast<UIDiv &>(node);
  if (div.childCount() >= 2 && div.child(1)) {
    static_cast<UIText *>(div.child(1))->setText(self->talkLine_);
  }
  if (div.childCount() >= 3 && div.child(2)) {
    static_cast<UIText *>(div.child(2))->setText(self->volumeLine_);
  }

  self->page().invalidateContent();
}

void AskApp::buildTalk(Page &page) {
  const Theme::ThemeTokens &th = Theme::active();
  const uint16_t muted = Theme::lerp(th.baseContent, th.base100, 0.4f);

  page.add(page.div()
               .onTick(onTalkTick)
               .style(Style()
                          .setWidth(Length::Pct(100))
                          .setHeight(Length::Pct(100))
                          .setPadding(Edges(12, 10))
                          .setGap(8)
                          .setColumns(1)
                          .setAlignH(Align::Center)
                          .setAlignV(Align::Center))
               .add(page.text(talkTitle_)
                        .style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setColor(th.baseContent)
                                   .setAlign(Align::Center)))
               .add(page.text(talkLine_)
                        .style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setFont(FontRole::Small)
                                   .setColor(failed_ ? th.warning : muted)
                                   .setAlign(Align::Center)))
               .add(page.text(volumeLine_)
                        .style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setFont(FontRole::Small)
                                   .setColor(muted)
                                   .setAlign(Align::Center))));
}
