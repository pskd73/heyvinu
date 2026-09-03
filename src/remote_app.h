#pragma once

#include <Flow32.h>

struct RemoteState {
  static constexpr uint32_t kMagic = 0x524D5431u; // 'RMT1'
  static constexpr uint16_t kVersion = 1;

  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint16_t reserved = 0;
};

/** Wi-Fi AP + HTTP form to edit persisted runtime config. */
class RemoteApp : public App<RemoteState> {
public:
  explicit RemoteApp(const Rect &viewport) : App(viewport) {
    setAppInfo("Remote", "wifi");
    addPage("Remote");
    self_ = this;
  }

protected:
  const char *nvsNamespace() const override { return "remote"; }

  void onOpen() override;
  void onClose() override;
  void build(Page &page, uint8_t /*pageId*/) override;

private:
  static RemoteApp *self_;

  void formatLines();
  static void onRemoteTick(UINode &node, float dt);

  char ssidLine_[48] = {};
  char ipLine_[32] = {};
  char passLine_[40] = {};
  char statusLine_[48] = {};

  bool started_ = false;
  bool failed_ = false;
  uint32_t lastUiMs_ = 0;

  static constexpr uint32_t kUiMinMs = 400;
};
