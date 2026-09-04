#include "image_preview.h"

#include <Flow32.h>

#include <JPEGDEC.h>
#include <PNGdec.h>
#include <esp_heap_caps.h>
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
  // Prefer a decode size that still covers the panel after Cover.
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

} // namespace

bool imagePreviewLoad(Storage *storage, const char *absPath, int16_t maxW,
                      int16_t maxH) {
  imagePreviewClear();
  if (!storage || !storage->ready() || !absPath || !absPath[0]) return false;
  if (maxW < 8 || maxH < 8) return false;

  storage_ = storage;
  strncpy(path_, absPath, sizeof(path_) - 1);
  path_[sizeof(path_) - 1] = '\0';

  const char *dot = strrchr(absPath, '.');
  bool ok = false;
  if (dot && (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0)) {
    ok = loadJpeg(maxW, maxH);
  } else if (dot && strcasecmp(dot, ".png") == 0) {
    ok = loadPng(maxW, maxH);
  } else {
    File f = storage->open(absPath, FILE_READ);
    uint8_t mag[4] = {};
    if (f) {
      f.read(mag, 4);
      f.close();
    }
    if (mag[0] == 0xFF && mag[1] == 0xD8) ok = loadJpeg(maxW, maxH);
    else if (mag[0] == 0x89 && mag[1] == 'P') ok = loadPng(maxW, maxH);
    else {
      Serial.printf("[preview] unknown format %s\n", absPath);
      path_[0] = '\0';
      return false;
    }
  }

  if (!ok) {
    path_[0] = '\0';
    return false;
  }

  gen_++;
  Serial.printf("[preview] ready %dx%d (%s) gen=%u\n", pixW_, pixH_, path_,
                (unsigned)gen_);
  return true;
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
