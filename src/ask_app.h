#pragma once

#include <Flow32.h>

struct AskState {
  static constexpr uint32_t kMagic = 0x41534B31u; // 'ASK1'
  static constexpr uint16_t kVersion = 1;

  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint16_t reserved = 0;
};

/** ElevenLabs ConvAI voice agent (chitram Talk). */
class AskApp : public App<AskState> {
public:
  explicit AskApp(const Rect &viewport) : App(viewport) {
    setAppInfo("Ask", "message-circle");
    addPage("Ask");
    self_ = this;
  }

protected:
  const char *nvsNamespace() const override { return "ask"; }

  void onOpen() override;
  void onClose() override;
  void build(Page &page, uint8_t /*pageId*/) override;

private:
  static AskApp *self_;

  void formatStatus();
  static void onAskTick(UINode &node, float dt);

  /** Cap UI rebuilds — full-page invalidate vs I2S/SPI fights PSRAM. */
  static constexpr uint32_t kUiMinMs = 200;

  char statusLine_[48] = "Starting...";

  bool pendingStart_ = false;
  bool started_ = false;
  bool failed_ = false;
  char errMsg_[48] = {};
  uint32_t lastUiMs_ = 0;
};
