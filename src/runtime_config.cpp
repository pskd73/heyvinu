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
    {ImageProvider, "image_provider", "Image provider (elevenlabs|openrouter)",
     16, false},
    {VoiceProvider, "voice_provider", "Voice provider (elevenlabs|deepgram)", 16,
     false},
};

const uint16_t kStoreBytes = 64 + 64 + 160 + 96 + 96 + 64 + 16 + 16;
/** Pre-ImageProvider layout (for NVS migrate). */
constexpr uint16_t kStoreBytesV1 = 64 + 64 + 160 + 96 + 96 + 64;
/** Pre-VoiceProvider layout. */
constexpr uint16_t kStoreBytesV2 = 64 + 64 + 160 + 96 + 96 + 64 + 16;

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
  static constexpr uint16_t kVersion = 3;

  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint16_t reserved = 0;
  char slots[Config::kStoreBytes];
};

/** Header + slots before ImageProvider was added. */
constexpr size_t kV1StoreSize = 8 + Config::kStoreBytesV1;
/** Header + slots before VoiceProvider was added. */
constexpr size_t kV2StoreSize = 8 + Config::kStoreBytesV2;

ConfigStore *gStore = nullptr;
bool gStoreNeedsSave_ = false;

static constexpr const char *kNvsNs = "chitramcfg";
static constexpr const char *kNvsKey = "store";

bool configLoadNvs(ConfigStore &out) {
  gStoreNeedsSave_ = false;
  Preferences prefs;
  if (!prefs.begin(kNvsNs, /*readOnly=*/true)) {
    return false;
  }
  if (!prefs.isKey(kNvsKey)) {
    prefs.end();
    return false;
  }
  const size_t len = prefs.getBytesLength(kNvsKey);

  if (len == sizeof(ConfigStore)) {
    const size_t got = prefs.getBytes(kNvsKey, &out, sizeof(out));
    prefs.end();
    if (got != sizeof(out) || out.magic != ConfigStore::kMagic) return false;
    if (out.version != ConfigStore::kVersion && out.version != 1 &&
        out.version != 2) {
      return false;
    }
    if (out.version != ConfigStore::kVersion) {
      out.version = ConfigStore::kVersion;
      gStoreNeedsSave_ = true;
    }
    return true;
  }

  // Migrate v2 blob (no VoiceProvider slot).
  if (len == kV2StoreSize) {
    uint8_t *raw = static_cast<uint8_t *>(malloc(kV2StoreSize));
    if (!raw) {
      prefs.end();
      return false;
    }
    const size_t got = prefs.getBytes(kNvsKey, raw, kV2StoreSize);
    prefs.end();
    if (got != kV2StoreSize) {
      free(raw);
      return false;
    }
    memset(&out, 0, sizeof(out));
    memcpy(&out, raw, kV2StoreSize);
    free(raw);
    if (out.magic != ConfigStore::kMagic) return false;
    out.version = ConfigStore::kVersion;
    strncpy(out.slots + Config::kStoreBytesV2, "elevenlabs", 15);
    out.slots[Config::kStoreBytesV2 + 15] = '\0';
    gStoreNeedsSave_ = true;
    Serial.println("Config: migrated NVS store (+voice_provider)");
    return true;
  }

  // Migrate v1 blob (no ImageProvider / VoiceProvider slots).
  if (len == kV1StoreSize) {
    uint8_t *raw = static_cast<uint8_t *>(malloc(kV1StoreSize));
    if (!raw) {
      prefs.end();
      return false;
    }
    const size_t got = prefs.getBytes(kNvsKey, raw, kV1StoreSize);
    prefs.end();
    if (got != kV1StoreSize) {
      free(raw);
      return false;
    }
    memset(&out, 0, sizeof(out));
    memcpy(&out, raw, kV1StoreSize);
    free(raw);
    if (out.magic != ConfigStore::kMagic) return false;
    out.version = ConfigStore::kVersion;
    strncpy(out.slots + Config::kStoreBytesV1, "openrouter", 15);
    out.slots[Config::kStoreBytesV1 + 15] = '\0';
    strncpy(out.slots + Config::kStoreBytesV2, "elevenlabs", 15);
    out.slots[Config::kStoreBytesV2 + 15] = '\0';
    gStoreNeedsSave_ = true;
    Serial.println("Config: migrated NVS store (+image_provider +voice_provider)");
    return true;
  }

  prefs.end();
  return false;
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
  case Config::ImageProvider:
    return "openrouter";
  case Config::VoiceProvider:
    return "elevenlabs";
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
    // Force OpenRouter while EL Flows is experimental for device use.
    if (strcmp(slotPtr(Config::ImageProvider), "openrouter") != 0) {
      copySlot(Config::ImageProvider, "openrouter");
      gStoreNeedsSave_ = true;
    }
    if (gStoreNeedsSave_) {
      if (!configSaveNvs()) {
        Serial.println("Config: NVS migrate save failed (RAM ok)");
      } else {
        Serial.println("Config: loaded from NVS (updated)");
      }
    } else {
      Serial.println("Config: loaded from NVS");
    }
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
