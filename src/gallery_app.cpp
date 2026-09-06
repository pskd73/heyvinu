#include "gallery_app.h"

#include "image_preview.h"

#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

namespace {

void *psramOrRam(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = malloc(n);
  return p;
}

} // namespace

GalleryApp *GalleryApp::self_ = nullptr;

const char *GalleryApp::pathAt(int i) const {
  if (!entries_ || i < 0 || i >= count_) return "";
  return entries_[i].path;
}

void GalleryApp::clearList() {
  freeThumbs();
  galleryCatalogFree(entries_);
  entries_ = nullptr;
  count_ = 0;
  index_ = -1;
  windowStart_ = 0;
  for (int i = 0; i < kGridCells; i++) {
    cellBtns_[i] = nullptr;
  }
}

void GalleryApp::freeThumbs() {
  if (thumbs_) {
    for (int i = 0; i < count_; i++) {
      if (thumbs_[i]) {
        heap_caps_free(thumbs_[i]);
        thumbs_[i] = nullptr;
      }
    }
    heap_caps_free(thumbs_);
    thumbs_ = nullptr;
  }
  if (thumbFailed_) {
    heap_caps_free(thumbFailed_);
    thumbFailed_ = nullptr;
  }
  thumbLoadPending_ = false;
  thumbWarmup_ = 0;
  thumbLoadIndex_ = -1;
}

void GalleryApp::trimThumbs() {
  if (!thumbs_) return;
  const int keepLo = windowStart_ - kGridCells;
  const int keepHi = windowStart_ + kGridCells * 2;
  for (int i = 0; i < count_; i++) {
    if (i >= keepLo && i < keepHi) continue;
    if (thumbs_[i]) {
      heap_caps_free(thumbs_[i]);
      thumbs_[i] = nullptr;
    }
  }
}

void GalleryApp::loadCatalog() {
  clearList();
  Storage *st = host() ? host()->storage() : nullptr;
  if (!st || !st->ready()) {
    snprintf(statusLine_, sizeof(statusLine_), "SD not ready");
    return;
  }

  entries_ = galleryCatalogLoad(st, &count_, kMaxImages);
  if (!entries_ || count_ <= 0) {
    count_ = 0;
    entries_ = nullptr;
    snprintf(statusLine_, sizeof(statusLine_), "No images");
    return;
  }

  thumbs_ = static_cast<uint16_t **>(
      psramOrRam(sizeof(uint16_t *) * static_cast<size_t>(count_)));
  thumbFailed_ = static_cast<uint8_t *>(
      psramOrRam(sizeof(uint8_t) * static_cast<size_t>(count_)));
  if (!thumbs_ || !thumbFailed_) {
    freeThumbs();
    galleryCatalogFree(entries_);
    entries_ = nullptr;
    count_ = 0;
    snprintf(statusLine_, sizeof(statusLine_), "Out of memory");
    return;
  }
  memset(thumbs_, 0, sizeof(uint16_t *) * static_cast<size_t>(count_));
  memset(thumbFailed_, 0, sizeof(uint8_t) * static_cast<size_t>(count_));
  statusLine_[0] = '\0';
}

int GalleryApp::maxWindowStart() const {
  if (count_ <= kGridCells) return 0;
  return ((count_ - 3) / 2) * 2;
}

int GalleryApp::visibleCount() const {
  int n = count_ - windowStart_;
  if (n < 0) n = 0;
  if (n > kGridCells) n = kGridCells;
  return n;
}

bool GalleryApp::shiftWindow(int deltaRows, int focusCol) {
  if (deltaRows == 0 || count_ <= kGridCells) return false;
  int next = windowStart_ + deltaRows * kGridCols;
  const int maxStart = maxWindowStart();
  if (next < 0) next = 0;
  if (next > maxStart) next = maxStart;
  if (next == windowStart_) return false;

  windowStart_ = next;
  trimThumbs();
  thumbLoadPending_ = false;
  thumbWarmup_ = 0;
  thumbLoadIndex_ = -1;

  if (focusCol < 0) focusCol = 0;
  if (focusCol >= kGridCols) focusCol = kGridCols - 1;

  const int visible = visibleCount();
  int focus = focusCol;
  if (deltaRows > 0 && visible > kGridCols) {
    focus = kGridCols + focusCol;
  }
  if (focus >= visible) focus = visible - 1;
  if (focus < 0) focus = 0;
  focusRestore_ = focus;
  requestRebuild();
  page().invalidateContent();
  return true;
}

void GalleryApp::formatTitle() {
  if (pageId() == kPageViewer && count_ > 0 && index_ >= 0) {
    snprintf(titleBuf_, sizeof(titleBuf_), "%d / %d", index_ + 1, count_);
    return;
  }
  snprintf(titleBuf_, sizeof(titleBuf_), "Gallery");
}

const char *GalleryApp::pageTitle(uint8_t /*id*/) const {
  return titleBuf_[0] ? titleBuf_ : "Gallery";
}

void GalleryApp::onOpen() {
  self_ = this;
  if (state().magic != GalleryState::kMagic ||
      state().version != GalleryState::kVersion) {
    data() = GalleryState{};
  }

  loading_ = false;
  loadPending_ = false;
  loadWarmup_ = 0;
  focusRestore_ = -1;
  gridFocusPending_ = true;
  windowStart_ = 0;
  previewGen_ = imagePreviewGen();
  imagePreviewClear();

  loadCatalog();
  formatTitle();
  goTo(kPageGrid, NavMode::Replace);
}

void GalleryApp::onClose() {
  loadPending_ = false;
  loading_ = false;
  thumbLoadPending_ = false;
  imagePreviewClear();
  previewGen_ = imagePreviewGen();
  clearList();
  if (self_ == this) self_ = nullptr;
}

void GalleryApp::runThumbLoad() {
  if (pageId() != kPageGrid || loading_ || loadPending_) return;
  if (!thumbs_ || !thumbFailed_ || !entries_) return;

  if (!thumbLoadPending_) {
    const int visible = visibleCount();
    for (int i = 0; i < visible; i++) {
      const int img = windowStart_ + i;
      if (!thumbs_[img] && !thumbFailed_[img]) {
        thumbLoadIndex_ = img;
        thumbLoadPending_ = true;
        thumbWarmup_ = 1;
        return;
      }
    }
    return;
  }

  if (thumbWarmup_ > 0) {
    thumbWarmup_--;
    return;
  }

  thumbLoadPending_ = false;
  const int img = thumbLoadIndex_;
  thumbLoadIndex_ = -1;
  if (img < 0 || img >= count_ || thumbs_[img] || thumbFailed_[img]) return;

  Storage *st = host() ? host()->storage() : nullptr;
  const char *path = pathAt(img);
  if (!st || !st->ready() || !path[0]) {
    thumbFailed_[img] = 1;
    return;
  }

  if (!imagePreviewLoad(st, path, kThumbW, kThumbH, false) ||
      !imagePreviewPixels() || imagePreviewWidth() != kThumbW ||
      imagePreviewHeight() != kThumbH) {
    imagePreviewClear();
    thumbFailed_[img] = 1;
    requestRebuild();
    return;
  }

  const size_t bytes =
      static_cast<size_t>(kThumbW) * static_cast<size_t>(kThumbH) *
      sizeof(uint16_t);
  auto *buf = static_cast<uint16_t *>(psramOrRam(bytes));
  if (!buf) {
    imagePreviewClear();
    thumbFailed_[img] = 1;
    return;
  }

  memcpy(buf, imagePreviewPixels(), bytes);
  imagePreviewClear();
  thumbs_[img] = buf;
  requestRebuild();
  page().invalidateContent();
}

bool GalleryApp::openViewer(int index) {
  if (index < 0 || index >= count_) return false;
  thumbLoadPending_ = false;
  thumbWarmup_ = 0;
  thumbLoadIndex_ = -1;
  imagePreviewClear();

  index_ = index;
  loading_ = true;
  loadPending_ = true;
  loadWarmup_ = 1;
  formatTitle();
  snprintf(statusLine_, sizeof(statusLine_), "Loading");
  goTo(kPageViewer, NavMode::Push);
  return true;
}

bool GalleryApp::showIndex(int index) {
  if (index < 0 || index >= count_) return false;
  index_ = index;
  loading_ = true;
  loadPending_ = true;
  loadWarmup_ = 1;
  formatTitle();
  snprintf(statusLine_, sizeof(statusLine_), "Loading");
  requestRebuild();
  page().invalidateContent();
  return true;
}

bool GalleryApp::navigate(int delta) {
  if (count_ <= 0 || delta == 0) return false;
  int next = index_;
  if (next < 0) next = 0;
  next += delta;
  if (next < 0) next = count_ - 1;
  if (next >= count_) next = 0;
  return showIndex(next);
}

void GalleryApp::runPendingLoad() {
  if (!loadPending_ || pageId() != kPageViewer) return;
  if (loadWarmup_ > 0) {
    loadWarmup_--;
    return;
  }

  loadPending_ = false;
  Storage *st = host() ? host()->storage() : nullptr;
  const char *path = pathAt(index_);
  if (!st || !st->ready() || !path[0]) {
    loading_ = false;
    snprintf(statusLine_, sizeof(statusLine_), "Load failed");
    requestRebuild();
    return;
  }

  const bool ok = imagePreviewLoad(st, path, kPreviewW, kPreviewH);
  loading_ = false;
  if (!ok) {
    snprintf(statusLine_, sizeof(statusLine_), "Load failed");
    requestRebuild();
    page().invalidateContent();
    return;
  }

  previewGen_ = imagePreviewGen();
  statusLine_[0] = '\0';
  formatTitle();
  requestRebuild();
  page().invalidateContent();
}

bool GalleryApp::goBack() {
  if (pageId() == kPageViewer) {
    loadPending_ = false;
    loading_ = false;
    imagePreviewClear();
    previewGen_ = imagePreviewGen();
    if (index_ >= 0) {
      windowStart_ = (index_ / kGridCols) * kGridCols;
      if (windowStart_ > maxWindowStart()) windowStart_ = maxWindowStart();
      focusRestore_ = index_ - windowStart_;
      if (focusRestore_ < 0) focusRestore_ = 0;
      if (focusRestore_ >= visibleCount()) {
        focusRestore_ = visibleCount() > 0 ? visibleCount() - 1 : 0;
      }
    }
    formatTitle();
    return back();
  }
  return false;
}

bool GalleryApp::handleKey(UIEvent &e) {
  if (e.phase != UIKeyPhase::Down && e.phase != UIKeyPhase::Hold) {
    return false;
  }

  if (pageId() == kPageViewer) {
    if (e.key != UIKey::Left && e.key != UIKey::Right) return false;
    if (count_ > 0 && !loading_) {
      navigate(e.key == UIKey::Right ? 1 : -1);
    }
    return true;
  }

  if (pageId() != kPageGrid || count_ <= 0) return false;
  if (e.key != UIKey::Up && e.key != UIKey::Down) return false;

  const uint8_t fi = page().focusIndex();
  const int visible = visibleCount();
  if (visible <= 0) return false;

  int focus = (fi == Page::kNoFocus) ? 0 : static_cast<int>(fi);
  if (focus >= visible) focus = visible - 1;
  const int col = focus % kGridCols;
  const bool topRow = focus < kGridCols;
  const bool bottomRow = focus >= kGridCols || visible <= kGridCols;

  if (e.key == UIKey::Up && topRow) {
    return shiftWindow(-1, col);
  }
  if (e.key == UIKey::Down && bottomRow) {
    return shiftWindow(+1, col);
  }
  return false;
}

void GalleryApp::onCellPress(UIButton &btn) {
  GalleryApp *self = self_;
  if (!self || self->pageId() != kPageGrid) return;
  for (int i = 0; i < kGridCells; i++) {
    if (self->cellBtns_[i] == &btn) {
      self->openViewer(self->windowStart_ + i);
      return;
    }
  }
}

void GalleryApp::onTick(UINode &node, float dt) {
  (void)node;
  (void)dt;
  GalleryApp *self = self_;
  if (!self) return;

  self->runPendingLoad();
  self->runThumbLoad();

  if (self->pageId() == kPageViewer) {
    const uint32_t gen = imagePreviewGen();
    if (gen != self->previewGen_) {
      self->previewGen_ = gen;
      self->requestRebuild();
      self->page().invalidateContent();
    }
  }
}

void GalleryApp::buildGrid(Page &page) {
  const Theme::ThemeTokens &th = Theme::active();
  const uint16_t muted = Theme::lerp(th.baseContent, th.base100, 0.4f);

  auto &root =
      page.div()
          .onTick(onTick)
          .style(Style()
                     .setWidth(Length::Pct(100))
                     .setHeight(Length::Pct(100))
                     .setPadding(Edges(16, 12))
                     .setGap(10)
                     .setColumns(1));
  page.add(root);

  if (count_ <= 0 || !entries_) {
    root.add(page.text(statusLine_[0] ? statusLine_ : "No images")
                 .style(Style()
                            .setWidth(Length::Pct(100))
                            .setColor(muted)
                            .setAlign(Align::Center)));
    return;
  }

  if (windowStart_ > maxWindowStart()) windowStart_ = maxWindowStart();
  const int visible = visibleCount();

  auto &grid = page.div().style(Style()
                                    .setWidth(Length::Pct(100))
                                    .setGap(10)
                                    .setColumns(kGridCols));
  root.add(grid);

  for (int i = 0; i < kGridCells; i++) {
    cellBtns_[i] = nullptr;
  }

  for (int i = 0; i < visible; i++) {
    const int img = windowStart_ + i;
    auto &cell =
        page.button()
            .color(ButtonColor::Secondary)
            .variant(ButtonVariant::Ghost)
            .onPress(onCellPress)
            .style(Style()
                       .setWidth(Length::Pct(100))
                       .setPadding(Edges(0))
                       .setRadius(th.radiusField)
                       .setGap(0)
                       .setColumns(1));

    if (thumbs_ && thumbs_[img]) {
      cell.add(page.image(thumbs_[img], kThumbW, kThumbH)
                   .style(Style()
                              .setWidth(Length::Pct(100))
                              .setRadius(th.radiusField)
                              .setFit(ImageFit::Fill)));
    } else {
      cell.add(page.div().style(Style()
                                    .setWidth(Length::Pct(100))
                                    .setHeight(Length::Px(kThumbH))
                                    .setRadius(th.radiusField)
                                    .setBackground(th.base300)));
    }

    cellBtns_[i] = &cell;
    grid.add(cell);
  }

  if (count_ > kGridCells) {
    snprintf(hintBuf_, sizeof(hintBuf_), "%d-%d / %d", windowStart_ + 1,
             windowStart_ + visible, count_);
    root.add(page.text(hintBuf_).style(Style()
                                           .setWidth(Length::Pct(100))
                                           .setColor(muted)
                                           .setAlign(Align::Center)));
  }

  if (gridFocusPending_) {
    page.setFocusIndex(0);
    gridFocusPending_ = false;
  } else if (focusRestore_ >= 0) {
    int f = focusRestore_;
    if (f >= visible) f = visible > 0 ? visible - 1 : 0;
    page.setFocusIndex(static_cast<uint8_t>(f));
    focusRestore_ = -1;
  }
}

void GalleryApp::buildViewer(Page &page) {
  const Theme::ThemeTokens &th = Theme::active();
  const uint16_t muted = Theme::lerp(th.baseContent, th.base100, 0.4f);

  const uint16_t *pix = imagePreviewPixels();
  const int16_t pw = imagePreviewWidth();
  const int16_t ph = imagePreviewHeight();
  const bool hasImage =
      !loading_ && count_ > 0 && pix && pw > 0 && ph > 0 && index_ >= 0;

  if (hasImage) {
    page.add(page.div()
                 .onTick(onTick)
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

  const char *msg = statusLine_[0] ? statusLine_ : "No images";
  page.add(page.div()
               .onTick(onTick)
               .style(Style()
                          .setWidth(Length::Pct(100))
                          .setHeight(Length::Pct(100))
                          .setPadding(Edges(12, 10))
                          .setColumns(1)
                          .setAlignH(Align::Center)
                          .setAlignV(Align::Center))
               .add(page.text(msg).style(Style()
                                             .setWidth(Length::Pct(100))
                                             .setColor(muted)
                                             .setAlign(Align::Center))));
}

void GalleryApp::build(Page &page, uint8_t pageId) {
  formatTitle();
  if (pageId == kPageViewer) {
    buildViewer(page);
    return;
  }
  buildGrid(page);
}
