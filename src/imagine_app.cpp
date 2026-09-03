#include "imagine_app.h"

#include "gallery.h"
#include "imagine_draw.h"
#include "imagine_fs.h"
#include "imagine_gen.h"
#include "imagine_stt.h"

#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

ImagineApp *ImagineApp::self_ = nullptr;

bool ImagineApp::allocView() {
  if (viewPixels_) {
    return true;
  }
  const size_t bytes = static_cast<size_t>(kViewW) * static_cast<size_t>(kViewH) *
                       sizeof(uint16_t);
  viewPixels_ = static_cast<uint16_t *>(
      heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!viewPixels_) {
    viewPixels_ = static_cast<uint16_t *>(malloc(bytes));
  }
  return viewPixels_ != nullptr;
}

void ImagineApp::freeView() {
  if (viewPixels_) {
    free(viewPixels_);
    viewPixels_ = nullptr;
  }
  viewLoaded_ = false;
}

bool ImagineApp::loadView(const char *path) {
  if (!path || !path[0] || !allocView()) {
    return false;
  }
  if (!imagineLoadCover(path, viewPixels_, kViewW, kViewH)) {
    viewLoaded_ = false;
    return false;
  }
  viewLoaded_ = true;
  strncpy(currentPath_, path, sizeof(currentPath_) - 1);
  currentPath_[sizeof(currentPath_) - 1] = '\0';
  return true;
}

void ImagineApp::scanGallery() {
  galleryShowCount_ = 0;
  const int total = galleryCount();
  snprintf(galleryCountLine_, sizeof(galleryCountLine_), "%d image%s", total,
           total == 1 ? "" : "s");
  const int n = total < static_cast<int>(kGalleryShowMax) ? total
                                                            : kGalleryShowMax;
  for (int i = 0; i < n; ++i) {
    const int idx = total - 1 - i; // newest first
    if (!galleryPathAt(idx, galleryPaths_[i], sizeof(galleryPaths_[i]))) {
      continue;
    }
    const char *base = strrchr(galleryPaths_[i], '/');
    base = base ? base + 1 : galleryPaths_[i];
    strncpy(galleryNames_[i], base, sizeof(galleryNames_[i]) - 1);
    galleryNames_[i][sizeof(galleryNames_[i]) - 1] = '\0';
    galleryShowCount_++;
  }
}

void ImagineApp::onOpen() {
  self_ = this;
  if (state().magic != ImagineState::kMagic ||
      state().version != ImagineState::kVersion) {
    data() = ImagineState{};
  }
  imgBindStorage(host() ? host()->storage() : nullptr);
  prompt_[0] = currentPath_[0] = editRefPath_[0] = '\0';
  listening_ = listenAttempted_ = listenFailed_ = false;
  generating_ = genArmed_ = genRan_ = genFailed_ = false;
  fromGallery_ = false;
  viewLoaded_ = false;
  statusLine_[0] = transcriptLine_[0] = '\0';
  allocView();
}

void ImagineApp::onClose() {
  imagineSttAbort();
  listening_ = false;
  freeView();
  imgBindStorage(nullptr);
  if (self_ == this) {
    self_ = nullptr;
  }
}

void ImagineApp::stopListenIfLeft() {
  if (listening_ && pageId() != kPageListen) {
    imagineSttAbort();
    listening_ = false;
  }
}

void ImagineApp::frame(Canvas &canvas, InputHub &input, float dt) {
  stopListenIfLeft();
  App::frame(canvas, input, dt);
}

void ImagineApp::openListen(bool editing) {
  if (!editing) {
    editRefPath_[0] = '\0';
  }
  listenAttempted_ = false;
  listenFailed_ = false;
  listening_ = false;
  transcriptLine_[0] = '\0';
  snprintf(statusLine_, sizeof(statusLine_), "%s",
           editing ? "Edit — speak" : "Connecting...");
  goTo(kPageListen, editing ? NavMode::Replace : NavMode::Push);
}

void ImagineApp::confirmListen() {
  const char *text = transcriptLine_[0] ? transcriptLine_ : imagineSttText();
  if (!text || !text[0]) {
    return;
  }
  strncpy(prompt_, text, sizeof(prompt_) - 1);
  prompt_[sizeof(prompt_) - 1] = '\0';
  imagineSttStop(true);
  listening_ = false;
  const char *finalText = imagineSttText();
  if (finalText && finalText[0]) {
    strncpy(prompt_, finalText, sizeof(prompt_) - 1);
    prompt_[sizeof(prompt_) - 1] = '\0';
  }
  genArmed_ = false;
  genRan_ = false;
  genFailed_ = false;
  generating_ = true;
  snprintf(statusLine_, sizeof(statusLine_), "Generating...");
  goTo(kPageGenerating, NavMode::Replace);
}

void ImagineApp::runGenerate() {
  char saved[48] = {};
  const char *ref = editRefPath_[0] ? editRefPath_ : nullptr;
  Serial.printf("Imagine gen prompt=\"%s\" ref=%s\n", prompt_,
                ref ? ref : "-");
  const bool ok = imagineGenerate(prompt_, saved, sizeof(saved), ref);
  editRefPath_[0] = '\0';
  generating_ = false;
  if (!ok || !saved[0]) {
    genFailed_ = true;
    snprintf(statusLine_, sizeof(statusLine_), "Generate failed");
    return;
  }
  strncpy(currentPath_, saved, sizeof(currentPath_) - 1);
  currentPath_[sizeof(currentPath_) - 1] = '\0';
  if (!loadView(saved)) {
    genFailed_ = true;
    snprintf(statusLine_, sizeof(statusLine_), "Decode failed");
    return;
  }
  fromGallery_ = false;
  goTo(kPageViewer, NavMode::Replace);
}

void ImagineApp::openViewer(const char *path, bool replace) {
  if (!loadView(path)) {
    snprintf(statusLine_, sizeof(statusLine_), "Load failed");
    return;
  }
  goTo(kPageViewer, replace ? NavMode::Replace : NavMode::Push);
}

void ImagineApp::openGallery() {
  scanGallery();
  fromGallery_ = true;
  goTo(kPageGallery);
}

void ImagineApp::tickListen() {
  if (!listenAttempted_) {
    listenAttempted_ = true;
    snprintf(statusLine_, sizeof(statusLine_), "Connecting...");
    return;
  }
  if (!listening_ && !listenFailed_) {
    if (!imagineSttStart()) {
      listenFailed_ = true;
      snprintf(statusLine_, sizeof(statusLine_), "%s", imagineSttError());
      return;
    }
    listening_ = true;
    snprintf(statusLine_, sizeof(statusLine_),
             editRefPath_[0] ? "Editing — speak" : "Listening...");
  }
  if (!listening_) {
    return;
  }
  imagineSttTick();
  if (imagineSttState() == ImagineSttState::Error) {
    listening_ = false;
    listenFailed_ = true;
    snprintf(statusLine_, sizeof(statusLine_), "%s", imagineSttError());
    return;
  }
  const char *t = imagineSttText();
  if (t && strcmp(transcriptLine_, t) != 0) {
    strncpy(transcriptLine_, t, sizeof(transcriptLine_) - 1);
    transcriptLine_[sizeof(transcriptLine_) - 1] = '\0';
  }
}

void ImagineApp::tickGenerating() {
  if (!genArmed_) {
    genArmed_ = true;
    return;
  }
  if (genRan_) {
    return;
  }
  genRan_ = true;
  runGenerate();
}

void ImagineApp::onGeneratePress(UIButton &) {
  if (self_) {
    self_->openListen(false);
  }
}

void ImagineApp::onGalleryPress(UIButton &) {
  if (self_) {
    self_->openGallery();
  }
}

void ImagineApp::onListenGeneratePress(UIButton &) {
  if (self_) {
    self_->confirmListen();
  }
}

void ImagineApp::onEditPress(UIButton &) {
  if (!self_ || !self_->currentPath_[0]) {
    return;
  }
  strncpy(self_->editRefPath_, self_->currentPath_,
          sizeof(self_->editRefPath_) - 1);
  self_->editRefPath_[sizeof(self_->editRefPath_) - 1] = '\0';
  self_->openListen(true);
}

void ImagineApp::onDeletePress(UIButton &) {
  if (!self_) {
    return;
  }
  if (self_->currentPath_[0]) {
    galleryDelete(self_->currentPath_);
  }
  self_->currentPath_[0] = '\0';
  self_->viewLoaded_ = false;
  while (self_->canGoBack()) {
    self_->back();
  }
  self_->scanGallery();
  if (self_->galleryShowCount_ > 0) {
    self_->goTo(kPageGallery);
  }
}

void ImagineApp::onMenuGalleryPress(UIButton &) {
  if (!self_) {
    return;
  }
  while (self_->canGoBack()) {
    self_->back();
  }
  self_->openGallery();
}

void ImagineApp::onGalleryItemPress(UIButton &) {
  if (!self_) {
    return;
  }
  const uint8_t fi = self_->page().focusIndex();
  if (fi < self_->galleryShowCount_) {
    self_->fromGallery_ = true;
    self_->openViewer(self_->galleryPaths_[fi], false);
  }
}

void ImagineApp::onListenTick(UINode &node, float dt) {
  (void)dt;
  if (!self_) {
    return;
  }
  char prevStatus[sizeof(self_->statusLine_)];
  char prevText[sizeof(self_->transcriptLine_)];
  memcpy(prevStatus, self_->statusLine_, sizeof(prevStatus));
  memcpy(prevText, self_->transcriptLine_, sizeof(prevText));
  self_->tickListen();

  UIDiv &div = static_cast<UIDiv &>(node);
  if (div.childCount() >= 1 && div.child(0)) {
    static_cast<UIText *>(div.child(0))->setText(self_->statusLine_);
  }
  if (div.childCount() >= 2 && div.child(1)) {
    static_cast<UIText *>(div.child(1))->setText(
        self_->transcriptLine_[0] ? self_->transcriptLine_ : " ");
  }
  if (strcmp(prevStatus, self_->statusLine_) != 0 ||
      strcmp(prevText, self_->transcriptLine_) != 0) {
    self_->page().invalidateContent();
  }
}

void ImagineApp::onGeneratingTick(UINode &node, float dt) {
  (void)dt;
  if (!self_) {
    return;
  }
  self_->tickGenerating();
  UIDiv &div = static_cast<UIDiv &>(node);
  if (div.childCount() >= 1 && div.child(0)) {
    static_cast<UIText *>(div.child(0))->setText(self_->statusLine_);
  }
}

bool ImagineApp::handleKey(UIEvent &e) {
  if (e.phase != UIKeyPhase::Down && e.phase != UIKeyPhase::Hold) {
    return false;
  }
  if (pageId() == kPageViewer && e.key == UIKey::Select &&
      e.phase == UIKeyPhase::Down) {
    goTo(kPageMenu);
    return true;
  }
  if (pageId() == kPageListen && e.key == UIKey::Select) {
    if (e.phase == UIKeyPhase::Hold && !transcriptLine_[0]) {
      strncpy(prompt_, "a small red balloon in a blue sky", sizeof(prompt_) - 1);
      prompt_[sizeof(prompt_) - 1] = '\0';
      imagineSttAbort();
      listening_ = false;
      genArmed_ = false;
      genRan_ = false;
      genFailed_ = false;
      generating_ = true;
      snprintf(statusLine_, sizeof(statusLine_), "Generating...");
      goTo(kPageGenerating, NavMode::Replace);
      return true;
    }
    if (e.phase == UIKeyPhase::Down && transcriptLine_[0]) {
      confirmListen();
      return true;
    }
  }
  return false;
}

void ImagineApp::build(Page &page, uint8_t pageId) {
  const Theme::ThemeTokens &th = Theme::active();
  const uint16_t muted = Theme::lerp(th.baseContent, th.base100, 0.4f);

  if (pageId == kPageHome) {
    auto &genBtn =
        page.button()
            .color(ButtonColor::Primary)
            .variant(ButtonVariant::Solid)
            .icon("mic")
            .onPress(onGeneratePress)
            .add(page.text("Generate").style(
                Style().setFont(FontRole::Body).setWidth(Length::Pct(100))));
    auto &galBtn =
        page.button()
            .color(ButtonColor::Secondary)
            .variant(ButtonVariant::Soft)
            .icon("images")
            .onPress(onGalleryPress)
            .add(page.text("Gallery").style(
                Style().setFont(FontRole::Body).setWidth(Length::Pct(100))));
    page.add(page.div()
                 .style(Style()
                            .setWidth(Length::Pct(100))
                            .setPadding(Edges(16, 12))
                            .setGap(12)
                            .setColumns(1))
                 .add(page.text("Imagine").style(
                     Style()
                         .setFont(FontRole::BodyLarge)
                         .setColor(th.baseContent)
                         .setWidth(Length::Pct(100))))
                 .add(genBtn)
                 .add(galBtn));
    return;
  }

  if (pageId == kPageListen) {
    page.add(
        page.div()
            .onTick(onListenTick)
            .style(Style()
                       .setWidth(Length::Pct(100))
                       .setPadding(Edges(12, 10))
                       .setGap(8)
                       .setColumns(1))
            .add(page.text(statusLine_)
                     .style(Style()
                                .setWidth(Length::Pct(100))
                                .setFont(FontRole::Small)
                                .setColor(listenFailed_ ? th.warning : muted)))
            .add(page.text(transcriptLine_[0] ? transcriptLine_ : " ")
                     .style(Style()
                                .setWidth(Length::Pct(100))
                                .setFont(FontRole::Body)
                                .setColor(th.baseContent)))
            .add(page.button()
                     .color(ButtonColor::Primary)
                     .variant(ButtonVariant::Solid)
                     .onPress(onListenGeneratePress)
                     .add(page.text("Generate").style(
                         Style()
                             .setFont(FontRole::Body)
                             .setWidth(Length::Pct(100)))))
            .add(page.text("Select = go  ·  Hold = test")
                     .style(Style()
                                .setWidth(Length::Pct(100))
                                .setFont(FontRole::Small)
                                .setColor(muted))));
    return;
  }

  if (pageId == kPageGenerating) {
    page.add(page.div()
                 .onTick(onGeneratingTick)
                 .style(Style()
                            .setWidth(Length::Pct(100))
                            .setHeight(Length::Pct(100))
                            .setPadding(Edges(16, 12))
                            .setColumns(1)
                            .setAlignH(Align::Center)
                            .setAlignV(Align::Center))
                 .add(page.text(statusLine_)
                          .style(Style()
                                     .setWidth(Length::Pct(100))
                                     .setFont(FontRole::Body)
                                     .setColor(genFailed_ ? th.warning
                                                           : th.baseContent)
                                     .setAlign(Align::Center))));
    return;
  }

  if (pageId == kPageViewer) {
    const uint16_t *px = viewLoaded_ && viewPixels_ ? viewPixels_ : nullptr;
    page.add(page.div()
                 .style(Style()
                            .setWidth(Length::Pct(100))
                            .setHeight(Length::Pct(100))
                            .setColumns(1))
                 .add(page.image(px, kViewW, kViewH)
                          .style(Style()
                                     .setWidth(Length::Pct(100))
                                     .setHeight(Length::Pct(100))
                                     .setFit(ImageFit::Cover))));
    return;
  }

  if (pageId == kPageMenu) {
    page.add(page.div()
                 .style(Style()
                            .setWidth(Length::Pct(100))
                            .setPadding(Edges(16, 12))
                            .setGap(10)
                            .setColumns(1))
                 .add(page.button()
                          .color(ButtonColor::Primary)
                          .variant(ButtonVariant::Solid)
                          .icon("pencil")
                          .onPress(onEditPress)
                          .add(page.text("Edit").style(
                              Style()
                                  .setFont(FontRole::Body)
                                  .setWidth(Length::Pct(100)))))
                 .add(page.button()
                          .color(ButtonColor::Secondary)
                          .variant(ButtonVariant::Soft)
                          .icon("trash-2")
                          .onPress(onDeletePress)
                          .add(page.text("Delete").style(
                              Style()
                                  .setFont(FontRole::Body)
                                  .setWidth(Length::Pct(100)))))
                 .add(page.button()
                          .color(ButtonColor::Secondary)
                          .variant(ButtonVariant::Soft)
                          .icon("images")
                          .onPress(onMenuGalleryPress)
                          .add(page.text("Gallery").style(
                              Style()
                                  .setFont(FontRole::Body)
                                  .setWidth(Length::Pct(100))))));
    return;
  }

  if (pageId == kPageGallery) {
    scanGallery();
    auto &col =
        page.div().style(Style()
                             .setWidth(Length::Pct(100))
                             .setPadding(Edges(12, 12))
                             .setGap(8)
                             .setColumns(1));
    if (galleryShowCount_ == 0) {
      col.add(page.text("No images yet").style(
          Style()
              .setFont(FontRole::Body)
              .setColor(th.baseContent)
              .setWidth(Length::Pct(100))));
      col.add(page.text("Generate something first.").style(
          Style()
              .setFont(FontRole::Small)
              .setColor(muted)
              .setWidth(Length::Pct(100))));
    } else {
      col.add(page.text(galleryCountLine_).style(
          Style()
              .setFont(FontRole::Small)
              .setColor(muted)
              .setWidth(Length::Pct(100))));
      for (uint8_t i = 0; i < galleryShowCount_; i++) {
        col.add(page.button()
                    .color(ButtonColor::Secondary)
                    .variant(ButtonVariant::Soft)
                    .icon("image")
                    .onPress(onGalleryItemPress)
                    .add(page.text(galleryNames_[i])
                             .style(Style()
                                        .setFont(FontRole::Small)
                                        .setWidth(Length::Pct(100)))));
      }
    }
    page.add(col);
  }
}
