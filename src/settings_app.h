#pragma once

#include <Flow32.h>
#include <Preferences.h>

#include "audio_volume.h"

/**
 * System settings — theme + Visualise + Wake word in NVS-backed state.
 * Voice provider is Config::VoiceProvider (runtime NVS), edited here.
 * Volume lives in the same blob for boot restore; Talk adjusts it live.
 */
struct SettingsState {
  static constexpr uint32_t kMagic = 0x53455431u; // 'SET1'
  static constexpr uint16_t kVersion = 4;

  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint16_t reserved = 0;

  /** 0 = Flow (dark), 1 = Winter (light). */
  int16_t theme = 0;
  /** Speaker level 0–100 (default 75 ≈ previous 1.5× gain). */
  int16_t volume = ChitramAudio::kVolumeDefault;
  /** 1 = Talk image tool enabled (default on). */
  int16_t visualise = 1;
  /** 1 = Hey Luna wake listening on launcher (default on). */
  int16_t wakeWord = 1;
};

/** Live Visualise flag for Talk (updated when Settings load/change). */
bool settingsVisualiseEnabled();
/** Seed / refresh the Live Visualise cache (boot + Settings). */
void settingsCacheVisualise(bool on);

/** Live Wake word flag for launcher (updated when Settings load/change). */
bool settingsWakeWordEnabled();
void settingsCacheWakeWord(bool on);

/** Pre-Visualise settings blob (theme + volume). Used for NVS upgrade. */
struct SettingsStateV2 {
  uint32_t magic = SettingsState::kMagic;
  uint16_t version = 2;
  uint16_t reserved = 0;
  int16_t theme = 0;
  int16_t volume = ChitramAudio::kVolumeDefault;
};
static_assert(sizeof(SettingsStateV2) == 12, "SettingsState v2 size");

/** Pre-Wake-word settings blob (theme + volume + visualise). */
struct SettingsStateV3 {
  uint32_t magic = SettingsState::kMagic;
  uint16_t version = 3;
  uint16_t reserved = 0;
  int16_t theme = 0;
  int16_t volume = ChitramAudio::kVolumeDefault;
  int16_t visualise = 1;
};

class SettingsApp : public App<SettingsState> {
public:
  static constexpr uint8_t kPageMain = 0;
  static constexpr uint8_t kPageClearContext = 1;
  static constexpr int kMaxContexts = 12;
  static constexpr size_t kContextNameLen = 48;

  explicit SettingsApp(const Rect &viewport, const char *name = "Settings",
                       const char *icon = "settings")
      : App(viewport) {
    setAppInfo(name, icon);
    addPage(name);
    addPage("Clear context");
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
  }

  void setVisualise(bool on) {
    set(data().visualise, on ? static_cast<int16_t>(1) : static_cast<int16_t>(0));
    cacheVisualise(on);
  }

  void setWakeWord(bool on) {
    set(data().wakeWord, on ? static_cast<int16_t>(1) : static_cast<int16_t>(0));
    cacheWakeWord(on);
  }

  void frame(Canvas &canvas, InputHub &input, float dt) override;

protected:
  const char *nvsNamespace() const override { return "settings"; }
  const char *pageTitle(uint8_t id) const override;

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
    if (state().visualise != 0 && state().visualise != 1) {
      data().visualise = 1;
    }
    if (state().wakeWord != 0 && state().wakeWord != 1) {
      data().wakeWord = 1;
    }
    applyThemeId(state().theme);
    ChitramAudio::setVolumePercent(state().volume);
    cacheVisualise(state().visualise != 0);
    cacheWakeWord(state().wakeWord != 0);
  }

  void onClose() override {
    if (self_ == this) self_ = nullptr;
  }

  void build(Page &page, uint8_t pageId) override;

private:
  static SettingsApp *self_;

  char contextNames_[kMaxContexts][kContextNameLen] = {};
  int contextCount_ = 0;
  bool listFocusPending_ = false;

  void refreshContextList();
  void buildMain(Page &page);
  void buildClearContext(Page &page);

  static void cacheVisualise(bool on) { settingsCacheVisualise(on); }
  static void cacheWakeWord(bool on) { settingsCacheWakeWord(on); }

  static void applyThemeId(int16_t id) {
    if (id == 1) {
      Theme::setActive(Theme::WinterTheme());
    } else {
      Theme::setActive(Theme::FlowTheme());
    }
  }

  static bool loadValid(SettingsState *out, bool *upgraded) {
    if (upgraded) *upgraded = false;
    if (!out) return false;
    Preferences prefs;
    if (!prefs.begin("settings", /*readOnly=*/true)) return false;
    if (!prefs.isKey("state")) {
      prefs.end();
      return false;
    }
    const size_t len = prefs.getBytesLength("state");

    if (len == sizeof(SettingsState)) {
      SettingsState loaded{};
      const bool ok =
          prefs.getBytes("state", &loaded, sizeof(loaded)) == sizeof(loaded) &&
          loaded.magic == SettingsState::kMagic &&
          loaded.version == SettingsState::kVersion && loaded.theme >= 0 &&
          loaded.theme <= 1 &&
          loaded.volume >= ChitramAudio::kVolumeMin &&
          loaded.volume <= ChitramAudio::kVolumeMax &&
          (loaded.visualise == 0 || loaded.visualise == 1) &&
          (loaded.wakeWord == 0 || loaded.wakeWord == 1);
      prefs.end();
      if (ok) *out = loaded;
      return ok;
    }

    if (len == sizeof(SettingsStateV3)) {
      SettingsStateV3 v3{};
      const bool ok =
          prefs.getBytes("state", &v3, sizeof(v3)) == sizeof(v3) &&
          v3.magic == SettingsState::kMagic && v3.version == 3 &&
          v3.theme >= 0 && v3.theme <= 1 &&
          v3.volume >= ChitramAudio::kVolumeMin &&
          v3.volume <= ChitramAudio::kVolumeMax &&
          (v3.visualise == 0 || v3.visualise == 1);
      prefs.end();
      if (!ok) return false;
      *out = SettingsState{};
      out->theme = v3.theme;
      out->volume = v3.volume;
      out->visualise = v3.visualise;
      out->wakeWord = 1;
      if (upgraded) *upgraded = true;
      return true;
    }

    if (len == sizeof(SettingsStateV2)) {
      SettingsStateV2 v2{};
      const bool ok =
          prefs.getBytes("state", &v2, sizeof(v2)) == sizeof(v2) &&
          v2.magic == SettingsState::kMagic && v2.version == 2 &&
          v2.theme >= 0 && v2.theme <= 1 &&
          v2.volume >= ChitramAudio::kVolumeMin &&
          v2.volume <= ChitramAudio::kVolumeMax;
      prefs.end();
      if (!ok) return false;
      *out = SettingsState{};
      out->theme = v2.theme;
      out->volume = v2.volume;
      out->visualise = 1;
      out->wakeWord = 1;
      if (upgraded) *upgraded = true;
      return true;
    }

    prefs.end();
    return false;
  }

  static void applyFromNvs() {
    SettingsState loaded{};
    bool upgraded = false;
    if (loadValid(&loaded, &upgraded)) {
      applyThemeId(loaded.theme);
      ChitramAudio::setVolumePercent(loaded.volume);
      cacheVisualise(loaded.visualise != 0);
      cacheWakeWord(loaded.wakeWord != 0);
      if (upgraded) {
        Preferences prefs;
        if (prefs.begin("settings", /*readOnly=*/false)) {
          prefs.putBytes("state", &loaded, sizeof(loaded));
          prefs.end();
        }
      }
    } else {
      applyThemeId(0);
      ChitramAudio::setVolumePercent(ChitramAudio::kVolumeDefault);
      cacheVisualise(true);
      cacheWakeWord(true);
    }
  }

  static void onThemeSelect(UISelect &s);
  static void onVisualiseChange(UIToggle &t);
  static void onWakeWordChange(UIToggle &t);
  static void onVoiceProviderSelect(UISelect &s);
  static void onMainMenu(UISelect &s);
  static void onClearContextSelect(UISelect &s);
};
