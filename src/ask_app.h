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

protected:
  const char *nvsNamespace() const override { return "ask"; }

  void onOpen() override;
  void onClose() override;
  void build(Page &page, uint8_t pageId) override;

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
  /** Matches the Settings volume slider step. */
  static constexpr int16_t kVolumeStep = 5;

  /** Child order of the talk page, mutated in place from the tick. */
  static constexpr uint8_t kTalkDot = 0;
  static constexpr uint8_t kTalkTitle = 1;
  static constexpr uint8_t kTalkStatus = 2;
  static constexpr uint8_t kTalkVolume = 3;

  char talkTitle_[40] = {};
  char talkLine_[48] = "Connecting...";
  char volumeLine_[24] = {};
  char agentId_[48] = {};
  bool volumeDirty_ = false;
  uint16_t healthColor_ = 0;
  bool pendingStart_ = false;
  bool started_ = false;
  bool failed_ = false;
  char errMsg_[48] = {};
  uint32_t lastUiMs_ = 0;

  void openTalk(int index);
  void resetTalkState();
  void leaveTalk();
  void formatTalkStatus();
  void formatVolumeLine();
  bool adjustVolume(int16_t delta);
  uint16_t healthColor() const;
  void buildTalk(Page &page);
  static void onTalkTick(UINode &node, float dt);
};
