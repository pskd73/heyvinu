#pragma once

#include <Flow32.h>
#include <Preferences.h>

#include "audio_volume.h"

/**
 * System settings — theme + volume in NVS-backed state.
 */
struct SettingsState {
  static constexpr uint32_t kMagic = 0x53455431u; // 'SET1'
  static constexpr uint16_t kVersion = 2;

  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint16_t reserved = 0;

  /** 0 = Flow (dark), 1 = Winter (light). */
  int16_t theme = 0;
  /** Speaker level 0–100 (default 75 ≈ previous 1.5× gain). */
  int16_t volume = ChitramAudio::kVolumeDefault;
};

class SettingsApp : public App<SettingsState> {
public:
  explicit SettingsApp(const Rect &viewport, const char *name = "Settings",
                       const char *icon = "settings")
      : App(viewport) {
    setAppInfo(name, icon);
    addPage(name);
    self_ = this;
  }

  void onAssets(IconSd * /*icons*/, ColorEmojiSd * /*emoji*/) override {
    applyFromNvs();
  }

  void setTheme(int16_t id) {
    if (id < 0) id = 0;
    if (id > 1) id = 1;
    set(data().theme, id);
    applyThemeId(id);
  }

  void setVolume(int16_t percent) {
    if (percent < ChitramAudio::kVolumeMin) percent = ChitramAudio::kVolumeMin;
    if (percent > ChitramAudio::kVolumeMax) percent = ChitramAudio::kVolumeMax;
    set(data().volume, percent);
    ChitramAudio::setVolumePercent(percent);
    formatVolumeLabel();
  }

protected:
  const char *nvsNamespace() const override { return "settings"; }

  void onOpen() override {
    self_ = this;
    if (state().magic != SettingsState::kMagic ||
        state().version != SettingsState::kVersion) {
      data() = SettingsState{};
    }
    if (state().theme < 0 || state().theme > 1) {
      data().theme = 0;
    }
    if (state().volume < ChitramAudio::kVolumeMin ||
        state().volume > ChitramAudio::kVolumeMax) {
      data().volume = ChitramAudio::kVolumeDefault;
    }
    applyThemeId(state().theme);
    ChitramAudio::setVolumePercent(state().volume);
    formatVolumeLabel();
  }

  void onClose() override {
    if (self_ == this) self_ = nullptr;
  }

  void build(Page &page, uint8_t /*pageId*/) override;

private:
  static SettingsApp *self_;
  char volumeLabel_[24] = "Volume  75%";

  void formatVolumeLabel();

  static void applyThemeId(int16_t id) {
    if (id == 1) {
      Theme::setActive(Theme::WinterTheme());
    } else {
      Theme::setActive(Theme::FlowTheme());
    }
  }

  static bool loadValid(SettingsState *out) {
    if (!out) return false;
    Preferences prefs;
    if (!prefs.begin("settings", /*readOnly=*/true)) return false;
    if (!prefs.isKey("state")) {
      prefs.end();
      return false;
    }
    SettingsState loaded{};
    const size_t len = prefs.getBytesLength("state");
    const bool ok =
        len == sizeof(SettingsState) &&
        prefs.getBytes("state", &loaded, sizeof(loaded)) == sizeof(loaded) &&
        loaded.magic == SettingsState::kMagic &&
        loaded.version == SettingsState::kVersion && loaded.theme >= 0 &&
        loaded.theme <= 1 && loaded.volume >= ChitramAudio::kVolumeMin &&
        loaded.volume <= ChitramAudio::kVolumeMax;
    prefs.end();
    if (ok) *out = loaded;
    return ok;
  }

  static void applyFromNvs() {
    SettingsState loaded{};
    if (loadValid(&loaded)) {
      applyThemeId(loaded.theme);
      ChitramAudio::setVolumePercent(loaded.volume);
    } else {
      applyThemeId(0);
      ChitramAudio::setVolumePercent(ChitramAudio::kVolumeDefault);
    }
  }

  static void onThemeSelect(UISelect &s);
  static void onVolumeChange(UIRange &r);
};
