#pragma once

#include <Flow32.h>

#include "talk_agent.h"

struct AskState {
  static constexpr uint32_t kMagic = 0x41534B31u; // 'ASK1'
  static constexpr uint16_t kVersion = 1;

  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint16_t reserved = 0;
};

/**
 * ElevenLabs ConvAI voice agent (chitram Talk).
 *
 * Opens on the account's agent list and pushes the live conversation once one
 * is chosen, so the device follows whatever exists in ElevenLabs rather than
 * the single agent id baked into config.
 */
class AskApp : public App<AskState> {
public:
  static constexpr uint8_t kPageStatus = 0;
  static constexpr uint8_t kPageAgents = 1;
  static constexpr uint8_t kPageTalk = 2;

  /** UISelect holds 12 options, and the picker is meant to be one glance. */
  static constexpr int kMaxAgents = 12;

  explicit AskApp(const Rect &viewport) : App(viewport) {
    setAppInfo("Ask", "message-circle");
    addPage("Ask");
    addPage("Agents");
    addPage("Ask");
    self_ = this;
  }

  void frame(Canvas &canvas, InputHub &input, float dt) override;
  bool goBack() override;
  bool handleKey(UIEvent &e) override;

  /**
   * Hold off the idle screensaver for the whole talk page, not just a live
   * session: a failed call leaves an error here that the user still needs to
   * read.
   */
  bool allowsIdle() const override { return pageId() != kPageTalk; }

protected:
  const char *nvsNamespace() const override { return "ask"; }

  void onOpen() override;
  void onClose() override;
  void build(Page &page, uint8_t pageId) override;

  const char *pageTitle(uint8_t id) const override;
  uint8_t shellStatusCount() const override;
  const char *shellStatusIcon(uint8_t i) const override;
  uint16_t shellStatusColor(uint8_t i) const override;

private:
  static AskApp *self_;

  /** Cap UI rebuilds — full-page invalidate vs I2S/SPI fights PSRAM. */
  static constexpr uint32_t kUiMinMs = 200;

  // --- Agent list ---
  TalkAgentInfo agents_[kMaxAgents] = {};
  int agentCount_ = 0;
  bool agentsMore_ = false;
  bool pendingFetch_ = false;
  bool listFocusPending_ = false;

  /**
   * Ticks to skip before a blocking call. Wi-Fi association and a TLS
   * handshake stall the tick for seconds, and `drawUI` runs after `tick` in
   * the same frame — without this the "Loading..." frame never reaches the
   * panel and the device looks hung on the previous screen.
   */
  uint8_t warmupFrames_ = 0;
  char statusLine_[64] = "Loading agents...";
  char moreLine_[48] = {};

  void runAgentFetch();
  void buildStatus(Page &page);
  void buildAgents(Page &page);
  static void onStatusTick(UINode &node, float dt);
  static void onAgentSelect(UISelect &sel);

  // --- Live conversation ---
  /** One bar per step; 5 bars → 0/20/40/60/80/100%. */
  static constexpr int16_t kVolumeStep = 20;
  static constexpr int16_t kVolumeBars = 5;
  static constexpr int16_t kVolumeBarW = 10;
  static constexpr int16_t kVolumeBarH = 14;
  static constexpr int16_t kVolumeBarGap = 4;
  static constexpr int16_t kVolumeIcon = 16;
  static constexpr int16_t kVolumeIconGap = 8;
  /** Images generated during the current talk session (SD paths). */
  static constexpr int kMaxSessionImages = 16;
  /** Panel-sized fullscreen preview (UIImage). */
  static constexpr int16_t kPreviewW = 280;
  static constexpr int16_t kPreviewH = 240;

  /** Child order of the default talk page (not used in fullscreen image). */
  static constexpr uint8_t kTalkStatus = 0;
  static constexpr uint8_t kTalkTool = 1;
  /** Volume chrome: speaker icon + bar row. */
  static constexpr uint8_t kTalkVolume = 2;
  /** Optional gallery badge is appended after volume when count > 0. */

  char talkTitle_[40] = {};
  char talkLine_[48] = "Connecting...";
  /** AI `show_text` only — never written by image/status code. */
  char toolLine_[128] = " ";
  /** Gallery loading / badge (not the AI tool line). */
  char galleryStatusLine_[32] = {};
  char galleryBadge_[8] = {};
  char agentId_[48] = {};
  bool volumeDirty_ = false;
  uint16_t healthColor_ = 0;
  uint32_t toolTextGen_ = 0;
  uint32_t previewGen_ = 0;
  /** Fullscreen generated-image overlay; Back dismisses to talk UI. */
  bool showingImage_ = false;
  /**
   * Auto-dismiss preview after this idle (ms). Reset on key while viewing;
   * also dismissed immediately when `show_text` updates.
   */
  static constexpr uint32_t kImagePreviewIdleMs = 10000;
  uint32_t imageIdleSinceMs_ = 0;
  /** True while SD→RGB565 decode is pending (show Loading… first). */
  bool imageLoading_ = false;
  /** >=0 → load this gallery index on the next talk tick. */
  int pendingGalleryIndex_ = -1;
  uint8_t galleryLoadWarmup_ = 0;
  char sessionImages_[kMaxSessionImages][96] = {};
  int sessionImageCount_ = 0;
  int sessionImageIndex_ = -1;
  bool pendingStart_ = false;
  bool started_ = false;
  bool failed_ = false;
  char errMsg_[48] = {};
  uint32_t lastUiMs_ = 0;

  void openTalk(int index);
  void resetTalkState();
  void leaveTalk();
  void dismissImage();
  void noteImageInteraction();
  void clearSessionImages();
  void rememberSessionImage(const char *absPath);
  void formatGalleryBadge();
  bool showSessionImage(int index);
  bool navigateSessionImage(int delta);
  void runPendingGalleryLoad();
  void formatTalkStatus();
  int16_t volumeBarCount() const;
  void paintVolumeBars(UIDiv &row) const;
  bool adjustVolume(int16_t delta);
  uint16_t healthColor() const;
  void buildTalk(Page &page);
  static void onTalkTick(UINode &node, float dt);
};
