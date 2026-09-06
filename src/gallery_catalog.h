#pragma once

#include <stddef.h>
#include <stdint.h>

class Storage;

/** Newest-last JSONL at `/chitram/images/catalog.jsonl`. */
struct GalleryCatalogMeta {
  const char *agentId = nullptr;
  const char *description = nullptr;
  const char *provider = nullptr;
};

struct GalleryCatalogEntry {
  static constexpr size_t kPathLen = 96;
  static constexpr size_t kAgentLen = 40;
  static constexpr size_t kDescLen = 96;
  static constexpr size_t kProviderLen = 16;

  char path[kPathLen] = {};
  char agent[kAgentLen] = {};
  char desc[kDescLen] = {};
  char provider[kProviderLen] = {};
  uint32_t t = 0;
};

static constexpr int kGalleryCatalogMaxEntries = 1000;

/**
 * Append one record (newest last). Safe from the image-gen task.
 * Skips sidecars / empty paths. Returns false on SD failure.
 */
bool galleryCatalogAdd(Storage *storage, const char *absPath,
                       const GalleryCatalogMeta &meta = {});

/**
 * Load up to `maxEntries` newest records into a PSRAM array (newest first).
 * Builds the catalog from the images dir if missing/empty.
 * Caller must `galleryCatalogFree`. Sets *outCount. Null on hard failure.
 */
GalleryCatalogEntry *galleryCatalogLoad(Storage *storage, int *outCount,
                                        int maxEntries = kGalleryCatalogMaxEntries);

void galleryCatalogFree(GalleryCatalogEntry *entries);
