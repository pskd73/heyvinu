#pragma once

#include <Flow32.h>
#include <Preferences.h>

#include "audio_volume.h"
#include "launcher_app.h"
#include "settings_app.h"
#include "test_app.h"
#include "ask_app.h"
#include "remote_app.h"

namespace {

template <typename S>
inline void ensureNvsBlob(const char *ns, const S &defaults) {
  Preferences prefs;
  if (!prefs.begin(ns, /*readOnly=*/false)) return;
  if (!prefs.isKey("state")) {
    prefs.putBytes("state", &defaults, sizeof(defaults));
  }
  prefs.end();
}

template <typename S>
inline bool loadNvsBlob(const char *ns, S *out) {
  if (!out) return false;
  Preferences prefs;
  if (!prefs.begin(ns, /*readOnly=*/true)) return false;
  if (!prefs.isKey("state")) {
    prefs.end();
    return false;
  }
  const size_t len = prefs.getBytesLength("state");
  const bool ok =
      len == sizeof(S) &&
      prefs.getBytes("state", out, sizeof(S)) == sizeof(S);
  prefs.end();
  return ok;
}

} // namespace

/** Seed NVS defaults for all Chitram apps and apply the Flow theme. */
inline void ensureNvsDefaults() {
  ensureNvsBlob("settings", SettingsState{});
  ensureNvsBlob("launch", LauncherState{});
  ensureNvsBlob("test", TestState{});
  ensureNvsBlob("ask", AskState{});
  ensureNvsBlob("remote", RemoteState{});

  SettingsState settings{};
  bool upgraded = false;
  bool ok = false;
  {
    Preferences prefs;
    if (prefs.begin("settings", /*readOnly=*/true) && prefs.isKey("state")) {
      const size_t len = prefs.getBytesLength("state");
      if (len == sizeof(SettingsState)) {
        ok = prefs.getBytes("state", &settings, sizeof(settings)) ==
                 sizeof(settings) &&
             settings.magic == SettingsState::kMagic &&
             settings.version == SettingsState::kVersion &&
             settings.theme >= 0 && settings.theme <= 1 &&
             settings.volume >= ChitramAudio::kVolumeMin &&
             settings.volume <= ChitramAudio::kVolumeMax &&
             (settings.visualise == 0 || settings.visualise == 1) &&
             (settings.wakeWord == 0 || settings.wakeWord == 1);
      } else if (len == sizeof(SettingsStateV3)) {
        SettingsStateV3 v3{};
        if (prefs.getBytes("state", &v3, sizeof(v3)) == sizeof(v3) &&
            v3.magic == SettingsState::kMagic && v3.version == 3 &&
            v3.theme >= 0 && v3.theme <= 1 &&
            v3.volume >= ChitramAudio::kVolumeMin &&
            v3.volume <= ChitramAudio::kVolumeMax &&
            (v3.visualise == 0 || v3.visualise == 1)) {
          settings = SettingsState{};
          settings.theme = v3.theme;
          settings.volume = v3.volume;
          settings.visualise = v3.visualise;
          settings.wakeWord = 1;
          ok = true;
          upgraded = true;
        }
      } else if (len == sizeof(SettingsStateV2)) {
        SettingsStateV2 v2{};
        if (prefs.getBytes("state", &v2, sizeof(v2)) == sizeof(v2) &&
            v2.magic == SettingsState::kMagic && v2.version == 2 &&
            v2.theme >= 0 && v2.theme <= 1 &&
            v2.volume >= ChitramAudio::kVolumeMin &&
            v2.volume <= ChitramAudio::kVolumeMax) {
          settings = SettingsState{};
          settings.theme = v2.theme;
          settings.volume = v2.volume;
          settings.visualise = 1;
          settings.wakeWord = 1;
          ok = true;
          upgraded = true;
        }
      }
    }
    prefs.end();
  }
  if (!ok) {
    settings = SettingsState{};
    upgraded = true;
  }
  if (upgraded) {
    Preferences prefs;
    if (prefs.begin("settings", /*readOnly=*/false)) {
      prefs.putBytes("state", &settings, sizeof(settings));
      prefs.end();
    }
  }

  Theme::setActive(settings.theme == 1 ? Theme::WinterTheme()
                                       : Theme::FlowTheme());
  ChitramAudio::setVolumePercent(settings.volume);
  settingsCacheVisualise(settings.visualise != 0);
  settingsCacheWakeWord(settings.wakeWord != 0);
}
