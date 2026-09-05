#pragma once

#include <stddef.h>
#include <stdint.h>

/** Runtime config dict in PSRAM, persisted to NVS across boots. */
namespace Config {

enum Key : uint8_t {
  WifiSsid = 0,
  WifiPassword,
  OpenrouterApiKey,
  DeepgramApiKey,
  ElevenlabsApiKey,
  ElevenlabsAgentId,
  /** `openrouter` (forced for now) or `elevenlabs` — image gen backend. */
  ImageProvider,
  /** `elevenlabs` | `deepgram` — live voice session backend. */
  VoiceProvider,
  Count,
};

struct Entry {
  Key key;
  const char *name;
  const char *label;
  uint16_t maxLen;
  bool masked;
};

extern const Entry kEntries[Count];
extern const uint16_t kStoreBytes;

Key keyFromName(const char *name);
const Entry *findEntry(const char *name);
const Entry *findEntry(Key key);

} // namespace Config

bool configInit();
bool configReady();

/** Dict lookup by key name (e.g. "wifi_ssid"). Returns "" if missing. */
const char *getConfig(const char *name);
/** Dict lookup by Config::Key. Returns "" if missing. */
const char *getConfig(Config::Key key);

/** keepIfEmpty: blank value leaves the existing entry unchanged. */
bool setConfig(const char *name, const char *value, bool keepIfEmpty = false);
bool setConfig(Config::Key key, const char *value, bool keepIfEmpty = false);
