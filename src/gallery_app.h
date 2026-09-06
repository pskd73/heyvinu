#pragma once

#include "gallery_catalog.h"

#include <Flow32.h>

struct GalleryState {
  static constexpr uint32_t kMagic = 0x47414C32u; // 'GAL2'
  static constexpr uint16_t kVersion = 2;

  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint16_t reserved = 0;
};

class GalleryApp : public App<GalleryState> {
public:
  static constexpr uint8_t kPageGrid = 0;
  static constexpr uint8_t kPageViewer = 1;

  static constexpr int kMaxImages = kGalleryCatalogMaxEntries;
  static constexpr int16_t kPreviewW = 280;
  static constexpr int16_t kPreviewH = 240;
  static constexpr int16_t kThumbW = 128;
  static constexpr int16_t kThumbH = 96;
  static constexpr int kGridCols = 2;
  static constexpr int kGridRows = 2;
  static constexpr int kGridCells = kGridCols * kGridRows;

  explicit GalleryApp(const Rect &viewport) : App(viewport) {
    setAppInfo("Gallery", "images");
    addPage("Gallery");
    addPage("Photo");
    setPageFullscreen(kPageViewer, true);
    self_ = this;
  }

  bool handleKey(UIEvent &e) override;
  bool goBack() override;
  bool allowsIdle() const override { return false; }

protected:
  const char *nvsNamespace() const override { return "gallery"; }
  const char *pageTitle(uint8_t id) const override;

  void onOpen() override;
  void onClose() override;
  void build(Page &page, uint8_t pageId) override;

private:
  static GalleryApp *self_;

  GalleryCatalogEntry *entries_ = nullptr;
  int count_ = 0;
  uint16_t **thumbs_ = nullptr;
  uint8_t *thumbFailed_ = nullptr;

  int index_ = -1;
  int windowStart_ = 0;
  int focusRestore_ = -1;
  bool gridFocusPending_ = false;

  bool thumbLoadPending_ = false;
  uint8_t thumbWarmup_ = 0;
  int thumbLoadIndex_ = -1;

  UIButton *cellBtns_[kGridCells] = {};

  bool loading_ = false;
  bool loadPending_ = false;
  uint8_t loadWarmup_ = 0;
  uint32_t previewGen_ = 0;
  char titleBuf_[24] = {};
  char statusLine_[40] = {};
  char hintBuf_[24] = {};

  const char *pathAt(int i) const;
  void clearList();
  void freeThumbs();
  void trimThumbs();
  void loadCatalog();
  int maxWindowStart() const;
  int visibleCount() const;
  bool shiftWindow(int deltaRows, int focusCol);
  void runThumbLoad();
  bool openViewer(int index);
  bool showIndex(int index);
  bool navigate(int delta);
  void runPendingLoad();
  void formatTitle();
  void buildGrid(Page &page);
  void buildViewer(Page &page);

  static void onTick(UINode &node, float dt);
  static void onCellPress(UIButton &btn);
};
