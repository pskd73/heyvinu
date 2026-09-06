#include "gallery_catalog.h"

#include <Flow32.h>

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace {

constexpr const char *kImgDir = "/chitram/images";
constexpr const char *kCatalogPath = "/chitram/images/catalog.jsonl";

void *psramOrRam(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = malloc(n);
  return p;
}

bool isPreviewSidecar(const char *name) {
  if (!name || !name[0]) return true;
  const char *dot = strrchr(name, '.');
  if (!dot) return true;
  if (strcasecmp(dot, ".jpg") != 0 && strcasecmp(dot, ".jpeg") != 0) {
    return false;
  }
  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;
  const size_t n = static_cast<size_t>(dot - base);
  return n >= 8 && strncasecmp(dot - 8, ".preview", 8) == 0;
}

bool isImageFile(const char *name) {
  if (!name || !name[0] || isPreviewSidecar(name)) return false;
  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;
  if (strcasecmp(base, "catalog.jsonl") == 0 ||
      strcasecmp(base, "catalog.jsonl.tmp") == 0) {
    return false;
  }
  const char *dot = strrchr(base, '.');
  if (!dot) return false;
  return strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".jpg") == 0 ||
         strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".webp") == 0;
}

void joinPath(char *out, size_t outLen, const char *dir, const char *name) {
  if (!out || outLen == 0) return;
  out[0] = '\0';
  if (!dir || !name) return;
  if (name[0] == '/') {
    snprintf(out, outLen, "%s", name);
    return;
  }
  const size_t dlen = strlen(dir);
  if (dlen > 0 && dir[dlen - 1] == '/') {
    snprintf(out, outLen, "%s%s", dir, name);
  } else {
    snprintf(out, outLen, "%s/%s", dir, name);
  }
}

uint32_t nameStamp(const char *path) {
  if (!path || !path[0]) return 0;
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  return static_cast<uint32_t>(strtoul(base, nullptr, 10));
}

void copyTrunc(char *dst, size_t dstLen, const char *src) {
  if (!dst || dstLen == 0) return;
  dst[0] = '\0';
  if (!src) return;
  strncpy(dst, src, dstLen - 1);
  dst[dstLen - 1] = '\0';
}

bool parseLine(const char *line, GalleryCatalogEntry *out) {
  if (!line || !out) return false;
  while (*line == ' ' || *line == '\t') line++;
  if (*line != '{') return false;

  JsonDocument doc;
  if (deserializeJson(doc, line)) return false;
  const char *path = doc["path"] | "";
  if (!path[0]) return false;

  memset(out, 0, sizeof(*out));
  copyTrunc(out->path, sizeof(out->path), path);
  copyTrunc(out->agent, sizeof(out->agent), doc["agent"] | "");
  copyTrunc(out->desc, sizeof(out->desc), doc["desc"] | "");
  copyTrunc(out->provider, sizeof(out->provider), doc["provider"] | "");
  out->t = doc["t"] | 0u;
  if (out->t == 0) out->t = nameStamp(out->path);
  return out->path[0] != '\0';
}

bool writeEntryLine(File &f, const GalleryCatalogEntry &e) {
  JsonDocument doc;
  doc["v"] = 1;
  doc["path"] = e.path;
  if (e.agent[0]) doc["agent"] = e.agent;
  if (e.desc[0]) doc["desc"] = e.desc;
  if (e.provider[0]) doc["provider"] = e.provider;
  if (e.t) doc["t"] = e.t;
  if (serializeJson(doc, f) == 0) return false;
  return f.print('\n') > 0;
}

struct ScanItem {
  char path[GalleryCatalogEntry::kPathLen];
  time_t mtime;
  uint32_t stamp;
};

int scanCmpAsc(const void *a, const void *b) {
  const auto *ea = static_cast<const ScanItem *>(a);
  const auto *eb = static_cast<const ScanItem *>(b);
  if (ea->mtime != eb->mtime) {
    return (ea->mtime < eb->mtime) ? -1 : 1;
  }
  if (ea->stamp != eb->stamp) {
    return (ea->stamp < eb->stamp) ? -1 : 1;
  }
  return strcmp(ea->path, eb->path);
}

bool rebuildFromDir(Storage *st) {
  if (!st || !st->ready()) return false;
  if (!st->exists(kImgDir) && !st->mkdir(kImgDir)) return false;

  File dir = st->open(kImgDir);
  if (!dir || !dir.isDirectory()) return false;

  ScanItem *items =
      static_cast<ScanItem *>(psramOrRam(sizeof(ScanItem) * kGalleryCatalogMaxEntries));
  if (!items) {
    dir.close();
    return false;
  }

  int n = 0;
  File f = dir.openNextFile();
  while (f && n < kGalleryCatalogMaxEntries) {
    if (!f.isDirectory()) {
      const char *name = f.name();
      if (isImageFile(name)) {
        joinPath(items[n].path, sizeof(items[n].path), kImgDir, name);
        if (items[n].path[0]) {
          items[n].mtime = f.getLastWrite();
          items[n].stamp = nameStamp(items[n].path);
          n++;
        }
      }
    }
    f = dir.openNextFile();
  }
  dir.close();

  if (n > 1) {
    qsort(items, static_cast<size_t>(n), sizeof(ScanItem), scanCmpAsc);
  }

  st->remove(kCatalogPath);
  File out = st->open(kCatalogPath, FILE_WRITE);
  if (!out) {
    heap_caps_free(items);
    return false;
  }

  for (int i = 0; i < n; i++) {
    GalleryCatalogEntry e{};
    copyTrunc(e.path, sizeof(e.path), items[i].path);
    e.t = items[i].mtime ? static_cast<uint32_t>(items[i].mtime)
                          : items[i].stamp;
    if (!writeEntryLine(out, e)) {
      out.close();
      heap_caps_free(items);
      return false;
    }
  }
  out.close();
  heap_caps_free(items);
  Serial.printf("[gallery-cat] rebuilt %d entries\n", n);
  return true;
}

bool ensureCatalog(Storage *st) {
  if (!st || !st->ready()) return false;
  if (st->exists(kCatalogPath)) {
    File f = st->open(kCatalogPath, FILE_READ);
    if (f) {
      const size_t sz = f.size();
      f.close();
      if (sz > 2) return true;
    }
  }
  return rebuildFromDir(st);
}

} // namespace

bool galleryCatalogAdd(Storage *storage, const char *absPath,
                       const GalleryCatalogMeta &meta) {
  if (!storage || !storage->ready() || !absPath || !absPath[0]) return false;
  if (isPreviewSidecar(absPath) || !isImageFile(absPath)) return false;
  if (!storage->exists(kImgDir) && !storage->mkdir(kImgDir)) return false;

  GalleryCatalogEntry e{};
  copyTrunc(e.path, sizeof(e.path), absPath);
  copyTrunc(e.agent, sizeof(e.agent), meta.agentId);
  copyTrunc(e.desc, sizeof(e.desc), meta.description);
  copyTrunc(e.provider, sizeof(e.provider), meta.provider);
  const time_t now = time(nullptr);
  e.t = (now > 0) ? static_cast<uint32_t>(now) : nameStamp(absPath);

  File f = storage->open(kCatalogPath, FILE_APPEND);
  if (!f) {
    f = storage->open(kCatalogPath, FILE_WRITE);
  }
  if (!f) {
    Serial.printf("[gallery-cat] open failed %s\n", kCatalogPath);
    return false;
  }
  const bool ok = writeEntryLine(f, e);
  f.close();
  if (ok) {
    Serial.printf("[gallery-cat] + %s\n", e.path);
  }
  return ok;
}

GalleryCatalogEntry *galleryCatalogLoad(Storage *storage, int *outCount,
                                        int maxEntries) {
  if (outCount) *outCount = 0;
  if (!storage || !storage->ready() || maxEntries < 1) return nullptr;
  if (maxEntries > kGalleryCatalogMaxEntries) {
    maxEntries = kGalleryCatalogMaxEntries;
  }

  if (!ensureCatalog(storage)) {
    Serial.println("[gallery-cat] ensure failed");
    return nullptr;
  }

  File f = storage->open(kCatalogPath, FILE_READ);
  if (!f) return nullptr;

  GalleryCatalogEntry *ring = static_cast<GalleryCatalogEntry *>(
      psramOrRam(sizeof(GalleryCatalogEntry) * static_cast<size_t>(maxEntries)));
  if (!ring) {
    f.close();
    return nullptr;
  }
  memset(ring, 0, sizeof(GalleryCatalogEntry) * static_cast<size_t>(maxEntries));

  int ringCount = 0;
  int ringStart = 0;
  char line[384];
  size_t lineLen = 0;

  auto pushLine = [&](char *buf) {
    GalleryCatalogEntry e{};
    if (!parseLine(buf, &e)) return;
    if (!storage->exists(e.path)) return;
    if (ringCount < maxEntries) {
      ring[ringCount++] = e;
    } else {
      ring[ringStart] = e;
      ringStart = (ringStart + 1) % maxEntries;
    }
  };

  while (f.available()) {
    const int c = f.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      line[lineLen] = '\0';
      if (lineLen > 0) pushLine(line);
      lineLen = 0;
      continue;
    }
    if (lineLen + 1 < sizeof(line)) {
      line[lineLen++] = static_cast<char>(c);
    } else {
      lineLen = sizeof(line) - 1;
    }
  }
  if (lineLen > 0) {
    line[lineLen] = '\0';
    pushLine(line);
  }
  f.close();

  if (ringCount <= 0) {
    heap_caps_free(ring);
    return nullptr;
  }

  GalleryCatalogEntry *out = static_cast<GalleryCatalogEntry *>(
      psramOrRam(sizeof(GalleryCatalogEntry) * static_cast<size_t>(ringCount)));
  if (!out) {
    heap_caps_free(ring);
    return nullptr;
  }

  for (int i = 0; i < ringCount; i++) {
    const int src = (ringStart + ringCount - 1 - i) % maxEntries;
    out[i] = ring[src];
  }
  heap_caps_free(ring);

  if (outCount) *outCount = ringCount;
  Serial.printf("[gallery-cat] loaded %d (newest first)\n", ringCount);
  return out;
}

void galleryCatalogFree(GalleryCatalogEntry *entries) {
  if (entries) heap_caps_free(entries);
}
