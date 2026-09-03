#pragma once

#include <Flow32.h>

struct ImagineState {
  static constexpr uint32_t kMagic = 0x494D4731u; // 'IMG1'
  static constexpr uint16_t kVersion = 1;

  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint16_t reserved = 0;
};

/**
 * Voice-prompt image generation (Deepgram STT → OpenRouter) + SD gallery.
 * Do not run Ask at the same time (shared I2S).
 */
class ImagineApp : public App<ImagineState> {
public:
  static constexpr uint8_t kPageHome = 0;
  static constexpr uint8_t kPageListen = 1;
  static constexpr uint8_t kPageGenerating = 2;
  static constexpr uint8_t kPageViewer = 3;
  static constexpr uint8_t kPageMenu = 4;
  static constexpr uint8_t kPageGallery = 5;

  static constexpr int16_t kViewW = 280;
  static constexpr int16_t kViewH = 240;
  static constexpr uint8_t kGalleryShowMax = 12;

  explicit ImagineApp(const Rect &viewport) : App(viewport) {
    setAppInfo("Imagine", "sparkles");
    addPage("Imagine");
    addPage("Listen");
    addPage("Imagine");
    addPage("Photo", /*fullscreen=*/true);
    addPage("Menu");
    addPage("Gallery");
    self_ = this;
  }

  void frame(Canvas &canvas, InputHub &input, float dt) override;

protected:
  const char *nvsNamespace() const override { return "imagine"; }

  bool handleKey(UIEvent &e) override;
  void onOpen() override;
  void onClose() override;
  void build(Page &page, uint8_t pageId) override;

private:
  static ImagineApp *self_;

  char prompt_[224] = {};
  char currentPath_[48] = {};
  char editRefPath_[48] = {};
  uint16_t *viewPixels_ = nullptr;
  bool viewLoaded_ = false;

  bool listening_ = false;
  bool listenAttempted_ = false;
  bool listenFailed_ = false;
  bool generating_ = false;
  bool genArmed_ = false;
  bool genRan_ = false;
  bool genFailed_ = false;
  bool fromGallery_ = false;

  char statusLine_[48] = {};
  char transcriptLine_[224] = {};
  char galleryCountLine_[32] = {};
  char galleryPaths_[kGalleryShowMax][48] = {};
  char galleryNames_[kGalleryShowMax][16] = {};
  uint8_t galleryShowCount_ = 0;

  bool allocView();
  void freeView();
  bool loadView(const char *path);
  void scanGallery();
  void stopListenIfLeft();
  void openListen(bool editing);
  void confirmListen();
  void runGenerate();
  void openViewer(const char *path, bool replace);
  void openGallery();
  void tickListen();
  void tickGenerating();

  static void onGeneratePress(UIButton &b);
  static void onGalleryPress(UIButton &b);
  static void onListenGeneratePress(UIButton &b);
  static void onEditPress(UIButton &b);
  static void onDeletePress(UIButton &b);
  static void onMenuGalleryPress(UIButton &b);
  static void onGalleryItemPress(UIButton &b);
  static void onListenTick(UINode &node, float dt);
  static void onGeneratingTick(UINode &node, float dt);
};
