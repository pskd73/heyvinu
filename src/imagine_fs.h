#pragma once

#include <Flow32.h>
#include <SD_MMC.h>
#include <string.h>

/**
 * Card-rooted paths ("/gallery/..."). Bind from ImagineApp::onOpen so
 * helpers can wrap host()->storage() without a global App.
 */
inline Storage *&imgStorageSlot() {
  static Storage *s = nullptr;
  return s;
}

inline void imgBindStorage(Storage *s) { imgStorageSlot() = s; }

inline Storage *imgStorage() { return imgStorageSlot(); }

inline bool imgReady() {
  Storage *s = imgStorage();
  return s && s->ready();
}

inline bool imgExists(const char *p) {
  if (!p || !p[0] || !imgReady()) {
    return false;
  }
  return imgStorage()->exists(p);
}

inline File imgOpen(const char *p, const char *mode = FILE_READ) {
  if (!p || !p[0] || !imgReady()) {
    return File();
  }
  return imgStorage()->open(p, mode);
}

inline bool imgMkdir(const char *p) {
  if (!p || !p[0] || !imgReady()) {
    return false;
  }
  return imgStorage()->mkdir(p);
}

inline bool imgRemove(const char *p) {
  if (!p || !p[0] || !imgReady()) {
    return false;
  }
  if (!imgStorage()->exists(p)) {
    return true;
  }
  return SD_MMC.remove(p);
}

inline bool imgRename(const char *from, const char *to) {
  if (!from || !to || !imgReady()) {
    return false;
  }
  return SD_MMC.rename(from, to);
}

inline fs::FS &imgFs() { return SD_MMC; }
