#include "image_preview.h"

#include <Flow32.h>

#include <JPEGDEC.h>
// JPEGDEC and JPEGENC both define these endian helpers — drop DEC's before ENC.
#ifdef INTELSHORT
#undef INTELSHORT
#endif
#ifdef INTELLONG
#undef INTELLONG
#endif
#ifdef MOTOSHORT
#undef MOTOSHORT
#endif
#ifdef MOTOLONG
#undef MOTOLONG
#endif
#include <JPEGENC.h>
#include <PNGdec.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

JPEGDEC jpeg_;
PNG *png_ = nullptr;
File file_;
Storage *storage_ = nullptr;

uint16_t *pixels_ = nullptr;
int16_t pixW_ = 0;
int16_t pixH_ = 0;
uint32_t gen_ = 0;
/** Canonical full-image path (gallery / OpenRouter source). */
char path_[96] = {};

// JPEG decode lands here first, then cover-blitted to pixels_.
uint16_t *jpegTmp_ = nullptr;
int16_t jpegTmpW_ = 0;
int16_t jpegTmpH_ = 0;

// PNG scale state.
int pngSrcW_ = 0;
int pngSrcH_ = 0;
uint16_t *pngLine_ = nullptr;
int pngLineCap_ = 0;
float coverScale_ = 1.f;
float coverOffX_ = 0.f;
float coverOffY_ = 0.f;

// Async sidecar encode (low-priority task; owns its pixel copy).
TaskHandle_t saveTask_ = nullptr;
volatile bool saveBusy_ = false;
Storage *saveStorage_ = nullptr;
char savePath_[96] = {};
uint16_t *savePixels_ = nullptr;
int16_t saveW_ = 0;
int16_t saveH_ = 0;
File encFile_;

void *psramOrRam(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = malloc(n);
  return p;
}

void freePixels() {
  if (pixels_) {
    heap_caps_free(pixels_);
    pixels_ = nullptr;
  }
  pixW_ = pixH_ = 0;
}

void freeJpegTmp() {
  if (jpegTmp_) {
    heap_caps_free(jpegTmp_);
    jpegTmp_ = nullptr;
  }
  jpegTmpW_ = jpegTmpH_ = 0;
}

void freePng() {
  if (pngLine_) {
    heap_caps_free(pngLine_);
    pngLine_ = nullptr;
  }
  pngLineCap_ = 0;
  if (png_) {
    heap_caps_free(png_);
    png_ = nullptr;
  }
}

bool endsWithIgnoreCase(const char *s, const char *suffix) {
  if (!s || !suffix) return false;
  const size_t n = strlen(s);
  const size_t m = strlen(suffix);
  if (m > n) return false;
  return strcasecmp(s + (n - m), suffix) == 0;
}

bool isPreviewSidecarPath(const char *p) {
  return endsWithIgnoreCase(p, ".preview.jpg");
}

bool makeSidecarPath(const char *src, char *dst, size_t dstLen) {
  if (!src || !src[0] || !dst || dstLen < 16) return false;
  if (isPreviewSidecarPath(src)) {
    if (strlen(src) + 1 > dstLen) return false;
    strncpy(dst, src, dstLen - 1);
    dst[dstLen - 1] = '\0';
    return true;
  }
  const char *slash = strrchr(src, '/');
  const char *base = slash ? slash + 1 : src;
  const char *dot = strrchr(base, '.');
  const size_t stemLen = dot ? (size_t)(dot - src) : strlen(src);
  static constexpr char kSuffix[] = ".preview.jpg";
  if (stemLen + sizeof(kSuffix) > dstLen) return false;
  memcpy(dst, src, stemLen);
  memcpy(dst + stemLen, kSuffix, sizeof(kSuffix));
  return true;
}

void *fileOpen(const char *filename, int32_t *size) {
  (void)filename;
  if (!storage_ || !path_[0]) return nullptr;
  file_ = storage_->open(path_, FILE_READ);
  if (!file_) return nullptr;
  *size = (int32_t)file_.size();
  return &file_;
}

void fileClose(void *handle) {
  (void)handle;
  if (file_) file_.close();
}

int32_t fileRead(JPEGFILE *file, uint8_t *buf, int32_t len) {
  (void)file;
  if (!file_) return 0;
  return (int32_t)file_.read(buf, len);
}

int32_t fileSeek(JPEGFILE *file, int32_t pos) {
  (void)file;
  if (!file_) return 0;
  return file_.seek(pos) ? pos : 0;
}

int32_t pngRead(PNGFILE *file, uint8_t *buf, int32_t len) {
  (void)file;
  if (!file_) return 0;
  return (int32_t)file_.read(buf, len);
}

int32_t pngSeek(PNGFILE *file, int32_t pos) {
  (void)file;
  if (!file_) return 0;
  return file_.seek(pos) ? pos : 0;
}

int jpegDraw(JPEGDRAW *draw) {
  if (!jpegTmp_ || !draw || !draw->pPixels) return 0;
  const int x0 = draw->x;
  const int y0 = draw->y;
  const int bw = draw->iWidth;
  const int bh = draw->iHeight;
  if (x0 >= jpegTmpW_ || y0 >= jpegTmpH_) return 1;
  const uint16_t *src = draw->pPixels;
  for (int row = 0; row < bh; row++) {
    const int dy = y0 + row;
    if (dy < 0 || dy >= jpegTmpH_) continue;
    const int copyW = (x0 + bw > jpegTmpW_) ? (jpegTmpW_ - x0) : bw;
    if (copyW <= 0 || x0 < 0) continue;
    memcpy(jpegTmp_ + (size_t)dy * (size_t)jpegTmpW_ + (size_t)x0,
           src + (size_t)row * (size_t)bw, (size_t)copyW * sizeof(uint16_t));
  }
  return 1;
}

int pickJpegScale(int srcW, int srcH, int16_t maxW, int16_t maxH) {
  const int opts[] = {0, JPEG_SCALE_HALF, JPEG_SCALE_QUARTER,
                      JPEG_SCALE_EIGHTH};
  const int divs[] = {1, 2, 4, 8};
  int best = JPEG_SCALE_EIGHTH;
  for (int i = 0; i < 4; i++) {
    const int w = srcW / divs[i];
    const int h = srcH / divs[i];
    best = opts[i];
    if (w >= maxW && h >= maxH) return opts[i];
    if (w <= maxW * 2 && h <= maxH * 2) return opts[i];
  }
  return best;
}

void coverBlit(const uint16_t *src, int srcW, int srcH, uint16_t *dst,
               int dstW, int dstH) {
  const float sx = (float)dstW / (float)srcW;
  const float sy = (float)dstH / (float)srcH;
  const float scale = sx > sy ? sx : sy;
  const float offX = (srcW * scale - (float)dstW) * 0.5f;
  const float offY = (srcH * scale - (float)dstH) * 0.5f;
  for (int dy = 0; dy < dstH; dy++) {
    for (int dx = 0; dx < dstW; dx++) {
      int ix = (int)((dx + offX) / scale);
      int iy = (int)((dy + offY) / scale);
      if (ix < 0) ix = 0;
      if (iy < 0) iy = 0;
      if (ix >= srcW) ix = srcW - 1;
      if (iy >= srcH) iy = srcH - 1;
      dst[(size_t)dy * (size_t)dstW + (size_t)dx] =
          src[(size_t)iy * (size_t)srcW + (size_t)ix];
    }
  }
}

void setupCover(int srcW, int srcH, int16_t dstW, int16_t dstH) {
  const float sx = (float)dstW / (float)srcW;
  const float sy = (float)dstH / (float)srcH;
  coverScale_ = sx > sy ? sx : sy;
  coverOffX_ = (srcW * coverScale_ - (float)dstW) * 0.5f;
  coverOffY_ = (srcH * coverScale_ - (float)dstH) * 0.5f;
}

int pngDraw(PNGDRAW *draw) {
  if (!pixels_ || !png_ || !draw || !pngLine_) return 0;
  if (draw->iWidth > pngLineCap_) return 0;
  png_->getLineAsRGB565(draw, pngLine_, PNG_RGB565_LITTLE_ENDIAN, 0xFFFFFFFFu);

  const float y0f = (float)draw->y * coverScale_ - coverOffY_;
  const float y1f = (float)(draw->y + 1) * coverScale_ - coverOffY_;
  int y0 = (int)floorf(y0f);
  int y1 = (int)ceilf(y1f);
  if (y0 < 0) y0 = 0;
  if (y1 > pixH_) y1 = pixH_;
  for (int dy = y0; dy < y1; dy++) {
    for (int dx = 0; dx < pixW_; dx++) {
      int sx = (int)((dx + coverOffX_) / coverScale_);
      if (sx < 0) sx = 0;
      if (sx >= pngSrcW_) sx = pngSrcW_ - 1;
      pixels_[(size_t)dy * (size_t)pixW_ + (size_t)dx] = pngLine_[sx];
    }
  }
  return 1;
}

bool allocDst(int16_t maxW, int16_t maxH) {
  pixW_ = maxW;
  pixH_ = maxH;
  const size_t bytes = (size_t)pixW_ * (size_t)pixH_ * sizeof(uint16_t);
  pixels_ = (uint16_t *)psramOrRam(bytes);
  if (!pixels_) {
    Serial.printf("[preview] OOM %u bytes\n", (unsigned)bytes);
    pixW_ = pixH_ = 0;
    return false;
  }
  memset(pixels_, 0, bytes);
  return true;
}

bool loadJpeg(int16_t maxW, int16_t maxH) {
  if (!jpeg_.open(path_, fileOpen, fileClose, fileRead, fileSeek, jpegDraw)) {
    Serial.printf("[preview] jpeg open failed %s err=%d\n", path_,
                  jpeg_.getLastError());
    return false;
  }

  const int srcW = jpeg_.getWidth();
  const int srcH = jpeg_.getHeight();
  const int scale = pickJpegScale(srcW, srcH, maxW, maxH);
  const int div = (scale == JPEG_SCALE_HALF)      ? 2
                  : (scale == JPEG_SCALE_QUARTER) ? 4
                  : (scale == JPEG_SCALE_EIGHTH)  ? 8
                                                   : 1;
  jpegTmpW_ = (int16_t)(srcW / div);
  jpegTmpH_ = (int16_t)(srcH / div);
  if (jpegTmpW_ < 1 || jpegTmpH_ < 1) {
    jpeg_.close();
    return false;
  }

  const size_t tmpBytes =
      (size_t)jpegTmpW_ * (size_t)jpegTmpH_ * sizeof(uint16_t);
  jpegTmp_ = (uint16_t *)psramOrRam(tmpBytes);
  if (!jpegTmp_ || !allocDst(maxW, maxH)) {
    jpeg_.close();
    freeJpegTmp();
    freePixels();
    return false;
  }
  memset(jpegTmp_, 0, tmpBytes);

  jpeg_.setPixelType(RGB565_LITTLE_ENDIAN);
  const int ok = jpeg_.decode(0, 0, scale);
  const int err = jpeg_.getLastError();
  jpeg_.close();
  if (!ok) {
    Serial.printf("[preview] jpeg decode failed err=%d\n", err);
    freeJpegTmp();
    freePixels();
    return false;
  }

  coverBlit(jpegTmp_, jpegTmpW_, jpegTmpH_, pixels_, pixW_, pixH_);
  freeJpegTmp();
  return true;
}

bool loadPng(int16_t maxW, int16_t maxH) {
  png_ = (PNG *)psramOrRam(sizeof(PNG));
  if (!png_) {
    Serial.printf("[preview] OOM PNG object\n");
    return false;
  }
  memset(png_, 0, sizeof(PNG));

  if (png_->open(path_, fileOpen, fileClose, pngRead, pngSeek, pngDraw) !=
      PNG_SUCCESS) {
    Serial.printf("[preview] png open failed %s err=%d\n", path_,
                  png_->getLastError());
    freePng();
    return false;
  }

  pngSrcW_ = png_->getWidth();
  pngSrcH_ = png_->getHeight();
  if (pngSrcW_ < 1 || pngSrcH_ < 1 || pngSrcW_ > 2048 || pngSrcH_ > 2048) {
    Serial.printf("[preview] png size %dx%d unsupported\n", pngSrcW_,
                  pngSrcH_);
    png_->close();
    freePng();
    return false;
  }
  if (png_->isInterlaced()) {
    Serial.printf("[preview] interlaced png not supported\n");
    png_->close();
    freePng();
    return false;
  }

  setupCover(pngSrcW_, pngSrcH_, maxW, maxH);
  pngLineCap_ = pngSrcW_;
  pngLine_ = (uint16_t *)psramOrRam((size_t)pngLineCap_ * sizeof(uint16_t));
  if (!pngLine_ || !allocDst(maxW, maxH)) {
    Serial.printf("[preview] OOM png buffers\n");
    png_->close();
    freePixels();
    freePng();
    return false;
  }

  const int rc = png_->decode(nullptr, 0);
  const int err = png_->getLastError();
  png_->close();
  freePng();
  if (rc != PNG_SUCCESS) {
    Serial.printf("[preview] png decode failed err=%d\n", err);
    freePixels();
    return false;
  }
  return true;
}

void *encOpen(const char *szFilename) {
  if (!saveStorage_ || !szFilename || !szFilename[0]) return nullptr;
  encFile_ = saveStorage_->open(szFilename, "w");
  if (!encFile_) return nullptr;
  return &encFile_;
}

void encClose(JPEGE_FILE *pFile) {
  (void)pFile;
  if (encFile_) encFile_.close();
}

int32_t encRead(JPEGE_FILE *pFile, uint8_t *pBuf, int32_t iLen) {
  (void)pFile;
  (void)pBuf;
  (void)iLen;
  return 0;
}

int32_t encWrite(JPEGE_FILE *pFile, uint8_t *pBuf, int32_t iLen) {
  (void)pFile;
  if (!encFile_ || !pBuf || iLen <= 0) return 0;
  return (int32_t)encFile_.write(pBuf, (size_t)iLen);
}

int32_t encSeek(JPEGE_FILE *pFile, int32_t iPosition) {
  (void)pFile;
  if (!encFile_) return 0;
  return encFile_.seek((uint32_t)iPosition) ? iPosition : 0;
}

void freeSavePixels() {
  if (savePixels_) {
    heap_caps_free(savePixels_);
    savePixels_ = nullptr;
  }
  saveW_ = saveH_ = 0;
}

bool encodeRgb565ToJpegFile(Storage *st, const char *sidePath,
                            const uint16_t *rgb, int16_t w, int16_t h,
                            int *outBytes) {
  if (outBytes) *outBytes = 0;
  if (!st || !st->ready() || !sidePath || !sidePath[0] || !rgb || w < 8 ||
      h < 8) {
    return false;
  }
  saveStorage_ = st;
  JPEGENC *jpg = (JPEGENC *)psramOrRam(sizeof(JPEGENC));
  if (!jpg) {
    saveStorage_ = nullptr;
    return false;
  }
  memset(jpg, 0, sizeof(JPEGENC));
  JPEGENCODE jpe;
  int rc = jpg->open(sidePath, encOpen, encClose, encRead, encWrite, encSeek);
  if (rc == JPEGE_SUCCESS) {
    rc = jpg->encodeBegin(&jpe, w, h, JPEGE_PIXEL_RGB565, JPEGE_SUBSAMPLE_420,
                          JPEGE_Q_MED);
    if (rc == JPEGE_SUCCESS) {
      rc = jpg->addFrame(&jpe, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(rgb)),
                         w * (int)sizeof(uint16_t));
    }
    const int n = jpg->close();
    if (outBytes) *outBytes = n;
    if (rc == JPEGE_SUCCESS && n > 0) {
      heap_caps_free(jpg);
      saveStorage_ = nullptr;
      return true;
    }
  }
  heap_caps_free(jpg);
  saveStorage_ = nullptr;
  return false;
}

void saveTaskFn(void *arg) {
  (void)arg;
  const uint32_t t0 = millis();
  int outBytes = 0;
  const bool ok =
      encodeRgb565ToJpegFile(saveStorage_, savePath_, savePixels_, saveW_,
                             saveH_, &outBytes);

  Serial.printf("[preview] sidecar %s %s (%d bytes, %lums)\n", savePath_,
                ok ? "ok" : "fail", outBytes,
                (unsigned long)(millis() - t0));

  freeSavePixels();
  saveStorage_ = nullptr;
  savePath_[0] = '\0';
  saveBusy_ = false;
  saveTask_ = nullptr;
  vTaskDelete(nullptr);
}

void scheduleSidecarSave(Storage *st, const char *canonicalPath) {
  if (!st || !st->ready() || !pixels_ || pixW_ < 8 || pixH_ < 8) return;
  if (!canonicalPath || !canonicalPath[0]) return;
  if (isPreviewSidecarPath(canonicalPath)) return;
  if (saveBusy_) return;

  char side[96];
  if (!makeSidecarPath(canonicalPath, side, sizeof(side))) return;
  if (st->exists(side)) return;

  const size_t bytes = (size_t)pixW_ * (size_t)pixH_ * sizeof(uint16_t);
  uint16_t *copy = (uint16_t *)psramOrRam(bytes);
  if (!copy) {
    Serial.printf("[preview] sidecar OOM copy %u\n", (unsigned)bytes);
    return;
  }
  memcpy(copy, pixels_, bytes);

  saveBusy_ = true;
  saveStorage_ = st;
  strncpy(savePath_, side, sizeof(savePath_) - 1);
  savePath_[sizeof(savePath_) - 1] = '\0';
  savePixels_ = copy;
  saveW_ = pixW_;
  saveH_ = pixH_;

  const BaseType_t ok =
      xTaskCreatePinnedToCore(saveTaskFn, "prev_jpg", 12288, nullptr, 1,
                              &saveTask_, 0);
  if (ok != pdPASS) {
    Serial.printf("[preview] sidecar task create failed\n");
    freeSavePixels();
    saveStorage_ = nullptr;
    savePath_[0] = '\0';
    saveBusy_ = false;
    saveTask_ = nullptr;
  }
}

bool loadFromPath(const char *readPath, int16_t maxW, int16_t maxH) {
  strncpy(path_, readPath, sizeof(path_) - 1);
  path_[sizeof(path_) - 1] = '\0';

  const char *dot = strrchr(readPath, '.');
  if (dot && (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0)) {
    return loadJpeg(maxW, maxH);
  }
  if (dot && strcasecmp(dot, ".png") == 0) {
    return loadPng(maxW, maxH);
  }

  File f = storage_->open(readPath, FILE_READ);
  uint8_t mag[4] = {};
  if (f) {
    f.read(mag, 4);
    f.close();
  }
  if (mag[0] == 0xFF && mag[1] == 0xD8) return loadJpeg(maxW, maxH);
  if (mag[0] == 0x89 && mag[1] == 'P') return loadPng(maxW, maxH);
  Serial.printf("[preview] unknown format %s\n", readPath);
  return false;
}

} // namespace

bool imagePreviewSidecarPath(char *out, size_t outLen, const char *srcAbs) {
  const char *src = srcAbs && srcAbs[0] ? srcAbs : path_;
  return makeSidecarPath(src, out, outLen);
}

bool imagePreviewLoad(Storage *storage, const char *absPath, int16_t maxW,
                      int16_t maxH) {
  imagePreviewClear();
  if (!storage || !storage->ready() || !absPath || !absPath[0]) return false;
  if (maxW < 8 || maxH < 8) return false;

  storage_ = storage;

  char canonical[96];
  strncpy(canonical, absPath, sizeof(canonical) - 1);
  canonical[sizeof(canonical) - 1] = '\0';

  bool fromSidecar = false;
  char readPath[96];
  strncpy(readPath, canonical, sizeof(readPath) - 1);
  readPath[sizeof(readPath) - 1] = '\0';

  if (!isPreviewSidecarPath(canonical)) {
    char side[96];
    if (makeSidecarPath(canonical, side, sizeof(side)) &&
        storage->exists(side)) {
      strncpy(readPath, side, sizeof(readPath) - 1);
      readPath[sizeof(readPath) - 1] = '\0';
      fromSidecar = true;
    }
  }

  const bool ok = loadFromPath(readPath, maxW, maxH);
  strncpy(path_, canonical, sizeof(path_) - 1);
  path_[sizeof(path_) - 1] = '\0';

  if (!ok) {
    path_[0] = '\0';
    return false;
  }

  gen_++;
  Serial.printf("[preview] ready %dx%d (%s%s) gen=%u\n", pixW_, pixH_, path_,
                fromSidecar ? " via sidecar" : "", (unsigned)gen_);

  if (!fromSidecar && !isPreviewSidecarPath(canonical)) {
    scheduleSidecarSave(storage, canonical);
  }
  return true;
}

bool imagePreviewEnsureSidecar(Storage *storage) {
  if (!storage || !storage->ready() || !path_[0] || !pixels_ || pixW_ < 8 ||
      pixH_ < 8) {
    return false;
  }
  if (isPreviewSidecarPath(path_)) {
    return storage->exists(path_);
  }
  char side[96];
  if (!makeSidecarPath(path_, side, sizeof(side))) return false;
  if (storage->exists(side)) return true;

  // Prefer waiting for an in-flight async encode of this path.
  if (saveBusy_ && strcmp(savePath_, side) == 0) {
    const uint32_t t0 = millis();
    while (saveBusy_ && millis() - t0 < 4000) {
      delay(40);
    }
    if (storage->exists(side)) return true;
  }

  int outBytes = 0;
  const uint32_t t0 = millis();
  const bool ok =
      encodeRgb565ToJpegFile(storage, side, pixels_, pixW_, pixH_, &outBytes);
  Serial.printf("[preview] sidecar sync %s %s (%d bytes, %lums)\n", side,
                ok ? "ok" : "fail", outBytes,
                (unsigned long)(millis() - t0));
  return ok && storage->exists(side);
}

void imagePreviewClear() {
  freePixels();
  freeJpegTmp();
  freePng();
  path_[0] = '\0';
  storage_ = nullptr;
  gen_++;
}

const uint16_t *imagePreviewPixels() { return pixels_; }
int16_t imagePreviewWidth() { return pixW_; }
int16_t imagePreviewHeight() { return pixH_; }
uint32_t imagePreviewGen() { return gen_; }
const char *imagePreviewPath() { return path_; }
