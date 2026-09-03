#include "gallery.h"
#include "imagine_config.h"
#include "imagine_draw.h"
#include "imagine_fs.h"

#include <JPEGENC.h>
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool isGalleryImageName(const char *name) {
  if (!name || name[0] == '.') {
    return false;
  }
  const char *dot = strrchr(name, '.');
  if (!dot || dot == name) {
    return false;
  }
  for (const char *p = name; p < dot; ++p) {
    if (*p < '0' || *p > '9') {
      return false;
    }
  }
  return strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0 ||
         strcasecmp(dot, ".png") == 0;
}

static int parseGalleryIndex(const char *name) {
  if (!name) {
    return -1;
  }
  int v = 0;
  const char *p = name;
  if (*p < '0' || *p > '9') {
    return -1;
  }
  while (*p >= '0' && *p <= '9') {
    v = v * 10 + (*p - '0');
    ++p;
    if (v > 999999) {
      return -1;
    }
  }
  if (*p != '.') {
    return -1;
  }
  return v;
}

static const char *fileBaseName(const char *name) {
  if (!name) {
    return "";
  }
  const char *base = strrchr(name, '/');
  return base ? base + 1 : name;
}

bool galleryEnsureDir() {
  if (!imgReady()) {
    return false;
  }
  if (!imgExists(GALLERY_DIR)) {
    if (!imgMkdir(GALLERY_DIR)) {
      Serial.println("ERR mkdir /gallery");
      return false;
    }
  }
  if (!imgExists(GALLERY_THUMB_DIR)) {
    if (!imgMkdir(GALLERY_THUMB_DIR)) {
      Serial.println("ERR mkdir /gallery/thumbnails");
      return false;
    }
  }
  return true;
}

static int galleryMaxSeq() {
  int maxSeq = 0;
  File dir = imgOpen(GALLERY_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) {
      dir.close();
    }
    return 0;
  }
  File f = dir.openNextFile();
  while (f) {
    int seq = parseGalleryIndex(fileBaseName(f.name()));
    if (seq > maxSeq) {
      maxSeq = seq;
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  return maxSeq;
}

static void *psramOrRam(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) {
    p = malloc(n);
  }
  return p;
}

static bool copyTempToDest(const char *tempPath, const char *outPath) {
  File in = imgOpen(tempPath, FILE_READ);
  if (!in) {
    return false;
  }
  const size_t total = in.size();
  if (total == 0) {
    in.close();
    return false;
  }

  uint8_t *mem = static_cast<uint8_t *>(psramOrRam(total));
  if (mem) {
    const size_t got = in.read(mem, total);
    in.close();
    if (got != total) {
      free(mem);
      return false;
    }
    imgRemove(outPath);
    File out = imgOpen(outPath, FILE_WRITE);
    if (!out) {
      free(mem);
      return false;
    }
    const size_t wrote = out.write(mem, total);
    out.close();
    free(mem);
    return wrote == total;
  }
  in.close();

  // One-file-open chunked copy (SD_MMC 1-bit).
  imgRemove(outPath);
  File out = imgOpen(outPath, FILE_WRITE);
  if (!out) {
    return false;
  }
  out.close();

  uint8_t buf[1024];
  size_t off = 0;
  while (off < total) {
    in = imgOpen(tempPath, FILE_READ);
    if (!in) {
      imgRemove(outPath);
      return false;
    }
    if (!in.seek(off)) {
      in.close();
      imgRemove(outPath);
      return false;
    }
    const size_t n = in.read(buf, sizeof(buf));
    in.close();
    if (n == 0) {
      break;
    }
    out = imgOpen(outPath, FILE_APPEND);
    if (!out) {
      imgRemove(outPath);
      return false;
    }
    const size_t wrote = out.write(buf, n);
    out.close();
    if (wrote != n) {
      imgRemove(outPath);
      return false;
    }
    off += n;
    yield();
  }
  return off == total;
}

bool gallerySaveFromTemp(const char *tempPath, bool isJpeg, char *outPath,
                         size_t outLen) {
  if (!galleryEnsureDir() || !tempPath || !outPath || outLen < 20) {
    return false;
  }
  int seq = galleryMaxSeq() + 1;
  if (seq > 99999) {
    seq = 1;
  }
  snprintf(outPath, outLen, "%s/%05d.%s", GALLERY_DIR, seq,
           isJpeg ? "jpg" : "png");
  imgRemove(outPath);
  if (!imgRename(tempPath, outPath)) {
    if (!copyTempToDest(tempPath, outPath)) {
      Serial.println("ERR gallery save copy");
      return false;
    }
    imgRemove(tempPath);
  }
  Serial.printf("gallery saved %s\n", outPath);
  if (!galleryMakeThumb(outPath)) {
    Serial.println("WARN gallery thumb failed");
  }
  return true;
}

bool galleryThumbPathFor(const char *imagePath, char *outPath, size_t outLen) {
  if (!imagePath || !outPath || outLen < 28) {
    return false;
  }
  const char *base = fileBaseName(imagePath);
  char stem[16];
  size_t i = 0;
  while (base[i] && base[i] != '.' && i + 1 < sizeof(stem)) {
    stem[i] = base[i];
    ++i;
  }
  stem[i] = '\0';
  if (i == 0) {
    return false;
  }
  snprintf(outPath, outLen, "%s/%s.jpg", GALLERY_THUMB_DIR, stem);
  return true;
}

bool galleryThumbPathAt(int index, char *outPath, size_t outLen) {
  char full[48];
  if (!galleryPathAt(index, full, sizeof(full))) {
    return false;
  }
  if (!galleryThumbPathFor(full, outPath, outLen)) {
    return false;
  }
  return imgExists(outPath);
}

bool galleryMakeThumb(const char *imagePath) {
  if (!imagePath || !imagePath[0] || !galleryEnsureDir()) {
    return false;
  }
  char thumbPath[48];
  if (!galleryThumbPathFor(imagePath, thumbPath, sizeof(thumbPath))) {
    return false;
  }

  static JPEGENC sJpg;
  static uint16_t *sRgb = nullptr;
  static uint8_t *sJpegBuf = nullptr;
  const int tw = GALLERY_THUMB_W;
  const int th = GALLERY_THUMB_H;
  const size_t rgbBytes =
      static_cast<size_t>(tw) * static_cast<size_t>(th) * sizeof(uint16_t);
  const size_t jpegCap = 24 * 1024;

  if (!sRgb) {
    sRgb = static_cast<uint16_t *>(psramOrRam(rgbBytes));
  }
  if (!sJpegBuf) {
    sJpegBuf = static_cast<uint8_t *>(psramOrRam(jpegCap));
  }
  if (!sRgb || !sJpegBuf) {
    Serial.println("ERR thumb buf");
    return false;
  }
  memset(sRgb, 0, rgbBytes);

  Serial.printf("thumb: decode %s heap=%u\n", imagePath,
                (unsigned)ESP.getFreeHeap());
  yield();
  if (!imagineLoadCover(imagePath, sRgb, static_cast<int16_t>(tw),
                        static_cast<int16_t>(th))) {
    Serial.printf("ERR thumb decode %s\n", imagePath);
    return false;
  }
  yield();

  JPEGENCODE enc;
  if (sJpg.open(sJpegBuf, static_cast<int>(jpegCap)) != JPEGE_SUCCESS) {
    Serial.println("ERR thumb jpeg open");
    return false;
  }
  if (sJpg.encodeBegin(&enc, tw, th, JPEGE_PIXEL_RGB565, JPEGE_SUBSAMPLE_420,
                       JPEGE_Q_LOW) != JPEGE_SUCCESS) {
    sJpg.close();
    Serial.println("ERR thumb jpeg begin");
    return false;
  }

  const int pitch = tw * static_cast<int>(sizeof(uint16_t));
  yield();
  if (sJpg.addFrame(&enc, reinterpret_cast<uint8_t *>(sRgb), pitch) !=
      JPEGE_SUCCESS) {
    sJpg.close();
    Serial.println("ERR thumb jpeg encode");
    return false;
  }
  int jpegLen = sJpg.close();
  if (jpegLen <= 0) {
    Serial.println("ERR thumb jpeg encode");
    return false;
  }

  imgRemove(thumbPath);
  File out = imgOpen(thumbPath, FILE_WRITE);
  if (!out) {
    Serial.printf("ERR thumb write open %s\n", thumbPath);
    return false;
  }
  const size_t wrote = out.write(sJpegBuf, static_cast<size_t>(jpegLen));
  out.close();
  if (wrote != static_cast<size_t>(jpegLen)) {
    imgRemove(thumbPath);
    Serial.println("ERR thumb write short");
    return false;
  }
  Serial.printf("gallery thumb %s (%d bytes)\n", thumbPath, jpegLen);
  return true;
}

bool galleryDelete(const char *path) {
  if (!path || !path[0]) {
    return false;
  }
  const size_t dirLen = strlen(GALLERY_DIR);
  if (strncmp(path, GALLERY_DIR, dirLen) != 0 || path[dirLen] != '/') {
    Serial.printf("ERR gallery delete bad path %s\n", path);
    return false;
  }
  if (!imgReady()) {
    return false;
  }
  if (!imgExists(path)) {
    Serial.printf("ERR gallery delete missing %s\n", path);
    return false;
  }
  char thumbPath[48];
  if (galleryThumbPathFor(path, thumbPath, sizeof(thumbPath))) {
    imgRemove(thumbPath);
  }
  if (!imgRemove(path)) {
    Serial.printf("ERR gallery delete fail %s\n", path);
    return false;
  }
  Serial.printf("gallery deleted %s\n", path);
  return true;
}

static int clearDirImages(const char *dirPath, bool thumbsOnly) {
  int removed = 0;
  for (;;) {
    File dir = imgOpen(dirPath);
    if (!dir || !dir.isDirectory()) {
      if (dir) {
        dir.close();
      }
      break;
    }

    char victim[GALLERY_NAME_LEN] = {};
    File f = dir.openNextFile();
    while (f) {
      const char *base = fileBaseName(f.name());
      bool take = false;
      if (!f.isDirectory()) {
        if (thumbsOnly) {
          take = base[0] && base[0] != '.';
        } else {
          take = isGalleryImageName(base);
        }
      }
      if (take) {
        strncpy(victim, base, GALLERY_NAME_LEN - 1);
        victim[GALLERY_NAME_LEN - 1] = '\0';
        f.close();
        break;
      }
      f.close();
      f = dir.openNextFile();
    }
    dir.close();

    if (!victim[0]) {
      break;
    }

    char path[64];
    snprintf(path, sizeof(path), "%s/%s", dirPath, victim);
    if (!imgRemove(path)) {
      Serial.printf("ERR gallery clear remove %s\n", path);
      break;
    }
    ++removed;
    yield();
  }
  return removed;
}

int galleryClearAll() {
  if (!imgReady()) {
    return -1;
  }
  int thumbs = 0;
  int photos = 0;
  if (imgExists(GALLERY_THUMB_DIR)) {
    thumbs = clearDirImages(GALLERY_THUMB_DIR, true);
  }
  if (imgExists(GALLERY_DIR)) {
    photos = clearDirImages(GALLERY_DIR, false);
  }
  Serial.printf("gallery clear photos=%d thumbs=%d\n", photos, thumbs);
  return photos;
}

int galleryCount() {
  if (!galleryEnsureDir()) {
    return 0;
  }
  int n = 0;
  File dir = imgOpen(GALLERY_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) {
      dir.close();
    }
    return 0;
  }
  File f = dir.openNextFile();
  while (f) {
    const char *base = fileBaseName(f.name());
    if (!f.isDirectory() && isGalleryImageName(base)) {
      ++n;
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  return n;
}

static int fillSortedIndices(int *idxs, int maxN) {
  int n = 0;
  File dir = imgOpen(GALLERY_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) {
      dir.close();
    }
    return 0;
  }
  File f = dir.openNextFile();
  while (f) {
    const char *base = fileBaseName(f.name());
    int seq = parseGalleryIndex(base);
    if (!f.isDirectory() && seq >= 0 && isGalleryImageName(base)) {
      if (n < maxN) {
        idxs[n++] = seq;
      }
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  for (int i = 1; i < n; ++i) {
    int key = idxs[i];
    int j = i - 1;
    while (j >= 0 && idxs[j] > key) {
      idxs[j + 1] = idxs[j];
      --j;
    }
    idxs[j + 1] = key;
  }
  return n;
}

bool galleryPathAt(int index, char *outPath, size_t outLen) {
  if (!outPath || outLen < 24 || index < 0) {
    return false;
  }
  int idxs[GALLERY_MAX_FILES];
  int n = fillSortedIndices(idxs, GALLERY_MAX_FILES);
  if (index >= n) {
    return false;
  }
  int seq = idxs[index];
  char jpg[GALLERY_NAME_LEN];
  char png[GALLERY_NAME_LEN];
  snprintf(jpg, sizeof(jpg), "%s/%05d.jpg", GALLERY_DIR, seq);
  snprintf(png, sizeof(png), "%s/%05d.png", GALLERY_DIR, seq);
  if (imgExists(jpg)) {
    strncpy(outPath, jpg, outLen - 1);
    outPath[outLen - 1] = '\0';
    return true;
  }
  if (imgExists(png)) {
    strncpy(outPath, png, outLen - 1);
    outPath[outLen - 1] = '\0';
    return true;
  }
  return false;
}

int galleryLatestIndex() {
  int n = galleryCount();
  return n > 0 ? n - 1 : -1;
}
