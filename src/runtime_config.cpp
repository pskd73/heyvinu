#include "runtime_config.h"

#include "secrets.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace Config {

const Entry kEntries[Count] = {
    {WifiSsid, "wifi_ssid", "Wi-Fi SSID", 64, false},
    {WifiPassword, "wifi_password", "Wi-Fi password (leave blank to keep)", 64,
     true},
    {OpenrouterApiKey, "openrouter_api_key", "OpenRouter API key (blank = keep)",
     160, true},
    {DeepgramApiKey, "deepgram_api_key", "Deepgram API key (blank = keep)", 96,
     true},
    {ElevenlabsApiKey, "elevenlabs_api_key", "ElevenLabs API key (blank = keep)",
     96, true},
    {ElevenlabsAgentId, "elevenlabs_agent_id", "ElevenLabs agent ID", 64,
     false},
};

const uint16_t kStoreBytes = 64 + 64 + 160 + 96 + 96 + 64;

Key keyFromName(const char *name) {
  const Entry *e = findEntry(name);
  return e ? e->key : Count;
}

const Entry *findEntry(const char *name) {
  if (!name || !name[0]) {
    return nullptr;
  }
  for (uint8_t i = 0; i < Count; i++) {
    if (strcmp(kEntries[i].name, name) == 0) {
      return &kEntries[i];
    }
  }
  return nullptr;
}

const Entry *findEntry(Key key) {
  if (key >= Count) {
    return nullptr;
  }
  return &kEntries[key];
}

} // namespace Config

namespace {

struct ConfigStore {
  static constexpr uint32_t kMagic = 0x43464731u; // 'CFG1'
  static constexpr uint16_t kVersion = 1;

  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint16_t reserved = 0;
  char slots[Config::kStoreBytes];
};

ConfigStore *gStore = nullptr;

static constexpr const char *kNvsNs = "chitramcfg";
static constexpr const char *kNvsKey = "store";

bool configLoadNvs(ConfigStore &out) {
  Preferences prefs;
  if (!prefs.begin(kNvsNs, /*readOnly=*/true)) {
    return false;
  }
  if (!prefs.isKey(kNvsKey)) {
    prefs.end();
    return false;
  }
  const size_t len = prefs.getBytesLength(kNvsKey);
  if (len != sizeof(ConfigStore)) {
    prefs.end();
    return false;
  }
  const size_t got = prefs.getBytes(kNvsKey, &out, sizeof(out));
  prefs.end();
  return got == sizeof(out) && out.magic == ConfigStore::kMagic &&
         out.version == ConfigStore::kVersion;
}

bool configSaveNvs() {
  if (!gStore || gStore->magic != ConfigStore::kMagic) {
    return false;
  }
  Preferences prefs;
  if (!prefs.begin(kNvsNs, /*readOnly=*/false)) {
    return false;
  }
  const size_t wrote = prefs.putBytes(kNvsKey, gStore, sizeof(ConfigStore));
  prefs.end();
  return wrote == sizeof(ConfigStore);
}

size_t slotOffset(Config::Key key) {
  size_t off = 0;
  for (uint8_t i = 0; i < static_cast<uint8_t>(key); i++) {
    off += Config::kEntries[i].maxLen;
  }
  return off;
}

char *slotPtr(Config::Key key) {
  return gStore->slots + slotOffset(key);
}

size_t slotCap(Config::Key key) {
  return Config::kEntries[static_cast<uint8_t>(key)].maxLen;
}

void copySlot(Config::Key key, const char *src) {
  char *dst = slotPtr(key);
  const size_t cap = slotCap(key);
  if (!src) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

const char *compileDefault(Config::Key key) {
  switch (key) {
  case Config::WifiSsid:
#if defined(WIFI_SSID)
    return WIFI_SSID;
#else
    return "";
#endif
  case Config::WifiPassword:
#if defined(WIFI_PASSWORD)
    return WIFI_PASSWORD;
#else
    return "";
#endif
  case Config::OpenrouterApiKey:
#if defined(OPENROUTER_API_KEY)
    return OPENROUTER_API_KEY;
#else
    return "";
#endif
  case Config::DeepgramApiKey:
#if defined(DEEPGRAM_API_KEY)
    return DEEPGRAM_API_KEY;
#else
    return "";
#endif
  case Config::ElevenlabsApiKey:
#if defined(ELEVENLABS_API_KEY)
    return ELEVENLABS_API_KEY;
#else
    return "";
#endif
  case Config::ElevenlabsAgentId:
#if defined(ELEVENLABS_AGENT_ID)
    return ELEVENLABS_AGENT_ID;
#else
    return "";
#endif
  default:
    return "";
  }
}

void seedFromCompileTime() {
  gStore->magic = ConfigStore::kMagic;
  gStore->version = ConfigStore::kVersion;
  gStore->reserved = 0;
  memset(gStore->slots, 0, sizeof(gStore->slots));
  for (uint8_t i = 0; i < Config::Count; i++) {
    copySlot(static_cast<Config::Key>(i), compileDefault(static_cast<Config::Key>(i)));
  }
}

} // namespace

bool configInit() {
  if (gStore && gStore->magic == ConfigStore::kMagic) {
    return true;
  }
  if (!gStore) {
    gStore = static_cast<ConfigStore *>(heap_caps_malloc(
        sizeof(ConfigStore), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!gStore) {
      gStore = static_cast<ConfigStore *>(malloc(sizeof(ConfigStore)));
    }
    if (!gStore) {
      return false;
    }
  }
  if (!configLoadNvs(*gStore)) {
    seedFromCompileTime();
    if (!configSaveNvs()) {
      Serial.println("Config: NVS save failed (using defaults in RAM)");
    } else {
      Serial.println("Config: seeded defaults → NVS");
    }
  } else {
    Serial.println("Config: loaded from NVS");
  }
  Serial.printf("Config dict ready (%u keys, %u B)\n", (unsigned)Config::Count,
                (unsigned)sizeof(ConfigStore));
  return true;
}

bool configReady() {
  return gStore && gStore->magic == ConfigStore::kMagic;
}

const char *getConfig(const char *name) {
  const Config::Entry *e = Config::findEntry(name);
  if (!e || !configReady()) {
    return "";
  }
  return slotPtr(e->key);
}

const char *getConfig(Config::Key key) {
  if (!Config::findEntry(key) || !configReady()) {
    return "";
  }
  return slotPtr(key);
}

bool setConfig(const char *name, const char *value, bool keepIfEmpty) {
  const Config::Entry *e = Config::findEntry(name);
  if (!e) {
    return false;
  }
  return setConfig(e->key, value, keepIfEmpty);
}

bool setConfig(Config::Key key, const char *value, bool keepIfEmpty) {
  if (!Config::findEntry(key)) {
    return false;
  }
  if (!configInit()) {
    return false;
  }
  if (keepIfEmpty && (!value || !value[0])) {
    return true;
  }
  copySlot(key, value ? value : "");
  if (!configSaveNvs()) {
    Serial.println("Config: NVS save failed");
    return false;
  }
  return true;
}
