#include "ask_app.h"

#include "audio_volume.h"
#include "image_gen.h"
#include "image_preview.h"
#include "talk_agent.h"
#include "talk_tools.h"

#include <stdio.h>
#include <string.h>

AskApp *AskApp::self_ = nullptr;

const char *AskApp::pageTitle(uint8_t id) const {
  if (id == kPageTalk && talkTitle_[0]) return talkTitle_;
  return App<AskState>::pageTitle(id);
}

uint8_t AskApp::shellStatusCount() const {
  if (pageId() != kPageTalk) return 0;
  return imageGenBusy() ? 2 : 1;
}

const char *AskApp::shellStatusIcon(uint8_t i) const {
  if (pageId() != kPageTalk) return nullptr;
  if (imageGenBusy()) {
    if (i == 0) return "images";
    if (i == 1) return "circle";
    return nullptr;
  }
  return i == 0 ? "circle" : nullptr;
}

uint16_t AskApp::shellStatusColor(uint8_t i) const {
  if (pageId() != kPageTalk) return 0;
  const Theme::ThemeTokens &th = Theme::active();
  if (imageGenBusy() && i == 0) {
    return Theme::lerp(th.baseContent, th.base100, 0.35f);
  }
  return healthColor();
}

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
  if (pageId() == kPageTalk && showingImage_) {
    dismissImage();
    return true;
  }
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
  if (showingImage_ || imageLoading_) {
    noteImageInteraction();
  }
  if (e.key == UIKey::Select) {
    // Press: mute agent locally and listen. Real interrupt is voice barge-in
    // (mic uplink → ElevenLabs `interruption`). I2S stays up.
    if (e.phase == UIKeyPhase::Down && started_ && !failed_) {
      talkAgentUserActivity();
    }
    return true;
  }
  if (e.key == UIKey::Left || e.key == UIKey::Right) {
    if (e.phase == UIKeyPhase::Down) {
      navigateSessionImage(e.key == UIKey::Right ? 1 : -1);
    }
    return true;
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

void AskApp::onAgentSelect(UIGridSelect &sel) {
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
                                   .setAlign(Align::Center))));
}

void AskApp::buildAgents(Page &page) {
  const Theme::ThemeTokens &th = Theme::active();
  const uint16_t muted = Theme::lerp(th.baseContent, th.base100, 0.4f);

  // selected(-1): tiles are navigation, not a remembered radio choice.
  auto &grid = page.gridSelect()
                   .selected(-1)
                   .onChange(onAgentSelect)
                   .style(Style()
                              .setWidth(Length::Pct(100))
                              .setColumns(2)
                              .setGap(8));
  for (int i = 0; i < agentCount_; i++) {
    const char *name = agents_[i].name[0] ? agents_[i].name : "Agent";
    grid.add(page.gridSelectOption()
                 .icon("bot-message-square")
                 .title(name)
                 .value((int16_t)i));
  }

  auto &col = page.div().style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setPadding(Edges(12, 10))
                                   .setGap(10)
                                   .setColumns(1));
  col.add(grid);
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
  toolLine_[0] = ' ';
  toolLine_[1] = '\0';
  toolTextGen_ = talkToolsTextGen();
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
  toolLine_[0] = ' ';
  toolLine_[1] = '\0';
  toolTextGen_ = 0;
  previewGen_ = 0;
  showingImage_ = false;
  imageIdleSinceMs_ = 0;
  imageLoading_ = false;
  pendingGalleryIndex_ = -1;
  galleryLoadWarmup_ = 0;
  galleryStatusLine_[0] = '\0';
  clearSessionImages();
  setPageFullscreen(kPageTalk, false);
}

void AskApp::leaveTalk() {
  resetTalkState();
  imagePreviewClear();
  talkAgentStop();
}

void AskApp::clearSessionImages() {
  sessionImageCount_ = 0;
  sessionImageIndex_ = -1;
  for (int i = 0; i < kMaxSessionImages; i++) {
    sessionImages_[i][0] = '\0';
  }
  formatGalleryBadge();
}

void AskApp::formatGalleryBadge() {
  snprintf(galleryBadge_, sizeof(galleryBadge_), "%d", sessionImageCount_);
}

void AskApp::rememberSessionImage(const char *absPath) {
  if (!absPath || !absPath[0]) return;
  for (int i = 0; i < sessionImageCount_; i++) {
    if (strcmp(sessionImages_[i], absPath) == 0) {
      sessionImageIndex_ = i;
      return;
    }
  }
  if (sessionImageCount_ >= kMaxSessionImages) {
    memmove(sessionImages_[0], sessionImages_[1],
            (size_t)(kMaxSessionImages - 1) * sizeof(sessionImages_[0]));
    sessionImageCount_ = kMaxSessionImages - 1;
  }
  strncpy(sessionImages_[sessionImageCount_], absPath,
          sizeof(sessionImages_[0]) - 1);
  sessionImages_[sessionImageCount_][sizeof(sessionImages_[0]) - 1] = '\0';
  sessionImageIndex_ = sessionImageCount_;
  sessionImageCount_++;
  formatGalleryBadge();
}

bool AskApp::showSessionImage(int index) {
  if (index < 0 || index >= sessionImageCount_) return false;
  // Defer the blocking SD decode so "Loading…" can paint for a frame.
  pendingGalleryIndex_ = index;
  imageLoading_ = true;
  galleryLoadWarmup_ = 1;
  sessionImageIndex_ = index;
  showingImage_ = true;
  setPageFullscreen(kPageTalk, true);
  snprintf(galleryStatusLine_, sizeof(galleryStatusLine_), "Loading");
  requestRebuild();
  page().invalidateContent();
  return true;
}

bool AskApp::navigateSessionImage(int delta) {
  if (sessionImageCount_ <= 0) return false;
  if (delta == 0) return false;

  // Carousel: talk ↔ img0 ↔ … ↔ imgN-1 ↔ talk …
  if (!showingImage_) {
    const int idx = (delta > 0) ? 0 : (sessionImageCount_ - 1);
    return showSessionImage(idx);
  }

  int next = sessionImageIndex_ + delta;
  if (next < 0 || next >= sessionImageCount_) {
    dismissImage();
    return true;
  }
  return showSessionImage(next);
}

void AskApp::runPendingGalleryLoad() {
  if (pendingGalleryIndex_ < 0) return;
  if (galleryLoadWarmup_ > 0) {
    galleryLoadWarmup_--;
    return;
  }

  const int index = pendingGalleryIndex_;
  pendingGalleryIndex_ = -1;

  Storage *st = host() ? host()->storage() : nullptr;
  if (!st || !st->ready() || index < 0 || index >= sessionImageCount_) {
    imageLoading_ = false;
    dismissImage();
    return;
  }

  const bool ok =
      imagePreviewLoad(st, sessionImages_[index], kPreviewW, kPreviewH);
  imageLoading_ = false;
  if (!ok) {
    Serial.printf("[ask] gallery load failed %s\n", sessionImages_[index]);
    dismissImage();
    return;
  }

  sessionImageIndex_ = index;
  previewGen_ = imagePreviewGen();
  showingImage_ = true;
  noteImageInteraction();
  setPageFullscreen(kPageTalk, true);
  galleryStatusLine_[0] = '\0';
  requestRebuild();
  page().invalidateContent();
}

void AskApp::dismissImage() {
  pendingGalleryIndex_ = -1;
  galleryLoadWarmup_ = 0;
  imageLoading_ = false;
  showingImage_ = false;
  imageIdleSinceMs_ = 0;
  galleryStatusLine_[0] = '\0';
  setPageFullscreen(kPageTalk, false);
  imagePreviewClear();
  previewGen_ = imagePreviewGen();
  requestRebuild();
  page().invalidateContent();
}

void AskApp::noteImageInteraction() {
  imageIdleSinceMs_ = millis();
}

int16_t AskApp::volumeBarCount() const {
  int16_t bars =
      static_cast<int16_t>(ChitramAudio::volumePercent() / kVolumeStep);
  if (bars < 0) bars = 0;
  if (bars > kVolumeBars) bars = kVolumeBars;
  return bars;
}

void AskApp::paintVolumeBars(UIDiv &wrap) const {
  const Theme::ThemeTokens &th = Theme::active();
  // Softer than pure baseContent — less harsh on dark/light panels.
  const uint16_t on = Theme::lerp(th.baseContent, th.base100, 0.4f);
  const uint16_t off = th.base300;
  UINode *barsNode =
      wrap.childCount() > 1 ? wrap.child(1) : wrap.child(0);
  if (!barsNode) return;
  UIDiv &row = static_cast<UIDiv &>(*barsNode);
  const int16_t filled = volumeBarCount();
  for (uint8_t i = 0; i < kVolumeBars && i < row.childCount(); i++) {
    UINode *bar = row.child(i);
    if (!bar) continue;
    bar->style().setBackground(i < filled ? on : off);
  }
}

bool AskApp::adjustVolume(int16_t delta) {
  const int16_t cur = ChitramAudio::volumePercent();
  int16_t bars = static_cast<int16_t>(cur / kVolumeStep);
  if (bars > kVolumeBars) bars = kVolumeBars;
  if (delta > 0) {
    bars = static_cast<int16_t>(bars + 1);
  } else if (delta < 0) {
    bars = static_cast<int16_t>(bars - 1);
  }
  if (bars < 0) bars = 0;
  if (bars > kVolumeBars) bars = kVolumeBars;
  const int16_t next = static_cast<int16_t>(bars * kVolumeStep);
  if (next == cur) {
    return false;
  }
  ChitramAudio::setVolumePercent(next);
  // Bars repaint through the normal tick, which holds off while the agent is
  // speaking — the gain itself already changed.
  volumeDirty_ = true;
  return true;
}

uint16_t AskApp::healthColor() const {
  const Theme::ThemeTokens &th = Theme::active();
  if (failed_) return th.error;
  if (pendingStart_ || !started_) {
    return Theme::lerp(th.baseContent, th.base100, 0.4f);
  }
  switch (talkAgentHealth()) {
  case TalkHealth::Ok:
    return th.success;
  case TalkHealth::Degraded:
    return th.warning;
  case TalkHealth::Connecting:
    return th.info;
  case TalkHealth::Stuck:
  case TalkHealth::Offline:
  default:
    return th.error;
  }
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

  self->runPendingGalleryLoad();

  if (self->started_ && !self->failed_ && !talkAgentIsActive()) {
    self->failed_ = true;
    snprintf(self->errMsg_, sizeof(self->errMsg_), "%s", talkAgentStatus());
  }

  const uint32_t now = millis();

  const uint32_t toolGen = talkToolsTextGen();
  const uint32_t previewGen = imagePreviewGen();
  const bool toolChanged = toolGen != self->toolTextGen_;
  const bool previewChanged = previewGen != self->previewGen_;
  if (toolChanged) {
    self->toolTextGen_ = toolGen;
    snprintf(self->toolLine_, sizeof(self->toolLine_), "%s",
             talkToolsLastText());
    if (!self->toolLine_[0]) {
      self->toolLine_[0] = ' ';
      self->toolLine_[1] = '\0';
    }
  }
  if (previewChanged) {
    self->previewGen_ = previewGen;
    // New pixels from generation → gallery + fullscreen (cancel pending nav).
    if (imagePreviewPixels() && imagePreviewWidth() > 0 &&
        imagePreviewHeight() > 0) {
      self->pendingGalleryIndex_ = -1;
      self->galleryLoadWarmup_ = 0;
      self->imageLoading_ = false;
      self->galleryStatusLine_[0] = '\0';
      self->rememberSessionImage(imagePreviewPath());
      self->showingImage_ = true;
      self->noteImageInteraction();
      self->setPageFullscreen(kPageTalk, true);
    }
    self->requestRebuild();
    self->page().invalidateContent();
    return;
  }
  if (self->showingImage_ || self->imageLoading_) {
    // New AI text, or 10s idle while the image is up → back to talk chrome.
    if (toolChanged) {
      self->dismissImage();
      return;
    }
    if (self->showingImage_ && !self->imageLoading_ &&
        self->imageIdleSinceMs_ != 0 &&
        (now - self->imageIdleSinceMs_) >= kImagePreviewIdleMs) {
      self->dismissImage();
    }
    return;
  }
  if (toolChanged) {
    self->requestRebuild();
  }

  if (now - self->lastUiMs_ < kUiMinMs && !toolChanged) {
    return;
  }

  char prev[sizeof(self->talkLine_)];
  memcpy(prev, self->talkLine_, sizeof(prev));
  self->formatTalkStatus();

  const uint16_t color = self->healthColor();
  const bool healthChanged = color != self->healthColor_;
  const bool textChanged = strcmp(prev, self->talkLine_) != 0;

  if (!textChanged && !self->volumeDirty_ && !healthChanged && !toolChanged) {
    return;
  }

  self->lastUiMs_ = now;
  self->volumeDirty_ = false;
  self->healthColor_ = color;

  UIDiv &div = static_cast<UIDiv &>(node);
  if (div.childCount() > kTalkStatus && div.child(kTalkStatus)) {
    static_cast<UIText *>(div.child(kTalkStatus))->setText(self->talkLine_);
  }
  if (div.childCount() > kTalkTool && div.child(kTalkTool)) {
    static_cast<UIText *>(div.child(kTalkTool))->setText(self->toolLine_);
  }
  if (div.childCount() > kTalkVolume && div.child(kTalkVolume)) {
    self->paintVolumeBars(static_cast<UIDiv &>(*div.child(kTalkVolume)));
  }

  self->page().invalidateContent();
}

void AskApp::buildTalk(Page &page) {
  const Theme::ThemeTokens &th = Theme::active();
  const uint16_t muted = Theme::lerp(th.baseContent, th.base100, 0.4f);
  healthColor_ = healthColor();

  const uint16_t *pix = imagePreviewPixels();
  const int16_t pw = imagePreviewWidth();
  const int16_t ph = imagePreviewHeight();
  const bool hasPreview =
      showingImage_ && !imageLoading_ && pix && pw > 0 && ph > 0;

  if (showingImage_ && imageLoading_) {
    page.add(page.div()
                 .onTick(onTalkTick)
                 .style(Style()
                            .setWidth(Length::Pct(100))
                            .setHeight(Length::Pct(100))
                            .setPadding(Edges(12, 10))
                            .setColumns(1)
                            .setAlignH(Align::Center)
                            .setAlignV(Align::Center))
                 .add(page.text(galleryStatusLine_)
                          .style(Style()
                                     .setWidth(Length::Pct(100))
                                     .setColor(th.baseContent)
                                     .setAlign(Align::Center))));
    return;
  }

  if (hasPreview) {
    // Full-bleed image; tick stays on the root so talk keeps running.
    // UIImage percent height resolves against 0 — use Auto so h = w * srcH/srcW
    // (280×240 buffer → full panel).
    page.add(page.div()
                 .onTick(onTalkTick)
                 .style(Style()
                            .setWidth(Length::Pct(100))
                            .setHeight(Length::Pct(100))
                            .setPadding(Edges(0))
                            .setGap(0)
                            .setColumns(1)
                            .setAlignH(Align::Center)
                            .setAlignV(Align::Center))
                 .add(page.image(pix, pw, ph)
                          .style(Style()
                                     .setWidth(Length::Pct(100))
                                     .setFit(ImageFit::Fill))));
    return;
  }

  auto &root = page.div()
                   .onTick(onTalkTick)
                   .style(Style()
                              .setWidth(Length::Pct(100))
                              .setHeight(Length::Pct(100))
                              .setPadding(Edges(10, 8))
                              .setGap(6)
                              .setColumns(1)
                              .setAlignH(Align::Center)
                              .setAlignV(Align::Center));

  // Title + health live in the Shell nav. Body: status, AI text, volume, badge.
  const uint16_t barOn = Theme::lerp(th.baseContent, th.base100, 0.4f);
  constexpr int16_t kBarsW =
      kVolumeBars * kVolumeBarW + (kVolumeBars - 1) * kVolumeBarGap;
  constexpr int16_t kVolChromeW = kVolumeIcon + kVolumeIconGap + kBarsW;

  auto &volBars =
      page.div().style(Style()
                           .setPosition(Position::Absolute)
                           .setLeft(Length::Px(kVolumeIcon + kVolumeIconGap))
                           .setTop(Length::Px(0))
                           .setWidth(Length::Px(kBarsW))
                           .setHeight(Length::Px(kVolumeBarH)));
  {
    const int16_t filled = volumeBarCount();
    for (int16_t i = 0; i < kVolumeBars; i++) {
      volBars.add(
          page.div().style(Style()
                               .setPosition(Position::Absolute)
                               .setLeft(Length::Px(
                                   i * (kVolumeBarW + kVolumeBarGap)))
                               .setTop(Length::Px(0))
                               .setWidth(Length::Px(kVolumeBarW))
                               .setHeight(Length::Px(kVolumeBarH))
                               .setRadius(2)
                               .setBackground(i < filled ? barOn
                                                         : th.base300)));
    }
  }

  auto &spk = page.button()
                  .icon("volume-2")
                  .color(ButtonColor::Secondary)
                  .variant(ButtonVariant::Ghost)
                  .style(Style()
                             .setPosition(Position::Absolute)
                             .setLeft(Length::Px(0))
                             .setTop(Length::Px(0))
                             .setWidth(Length::Px(kVolumeIcon + 4))
                             .setHeight(Length::Px(kVolumeBarH))
                             .setPadding(Edges(0))
                             .setIconSize(static_cast<uint8_t>(kVolumeIcon))
                             .setAlignH(Align::Center)
                             .setAlignV(Align::Center));
  spk.setHighlightable(false);

  auto &volWrap =
      page.div()
          .style(Style()
                     .setWidth(Length::Px(kVolChromeW))
                     .setHeight(Length::Px(kVolumeBarH)))
          .add(spk)
          .add(volBars);

  root.add(page.text(talkLine_)
               .style(Style()
                          .setWidth(Length::Pct(100))
                          .setFont(FontRole::Small)
                          .setColor(failed_ ? th.warning : muted)
                          .setAlign(Align::Center)))
      .add(page.text(toolLine_)
               .marquee()
               .style(Style()
                          .setWidth(Length::Pct(100))
                          .setHeight(Length::Px(36))
                          .setFont(FontRole::Small)
                          .setColor(th.baseContent)
                          .setAlign(Align::Center)))
      .add(volWrap);

  if (sessionImageCount_ > 0) {
    formatGalleryBadge();
    auto &badge =
        page.button()
            .icon("images")
            .color(ButtonColor::Secondary)
            .variant(ButtonVariant::Soft)
            .style(Style()
                       .setWidth(Length::Px(64))
                       .setPadding(Edges(4, 8))
                       .setIconSize(16)
                       .setGap(2)
                       .setAlignH(Align::Center)
                       .setAlignV(Align::Center))
            .add(page.text(galleryBadge_)
                     .style(Style()
                                .setFont(FontRole::Small)
                                .setColor(th.baseContent)
                                .setAlign(Align::Center)));
    badge.setHighlightable(false);
    root.add(badge);
  }

  page.add(root);
}
