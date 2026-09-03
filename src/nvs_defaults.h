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
  const bool ok =
      loadNvsBlob("settings", &settings) &&
      settings.magic == SettingsState::kMagic &&
      settings.version == SettingsState::kVersion && settings.theme >= 0 &&
      settings.theme <= 1 &&
      settings.volume >= ChitramAudio::kVolumeMin &&
      settings.volume <= ChitramAudio::kVolumeMax;
  if (!ok) {
    settings = SettingsState{};
    Preferences prefs;
    if (prefs.begin("settings", /*readOnly=*/false)) {
      prefs.putBytes("state", &settings, sizeof(settings));
      prefs.end();
    }
  }

  Theme::setActive(settings.theme == 1 ? Theme::WinterTheme()
                                       : Theme::FlowTheme());
  ChitramAudio::setVolumePercent(settings.volume);
}
