#include "imagine_draw.h"
#include "imagine_fs.h"

#include <TJpg_Decoder.h>
#include <string.h>

namespace {

constexpr int kMaxW = 320;
constexpr int kMaxH = 240;

uint16_t *coverOut = nullptr;
int coverDw = 0;
int coverDh = 0;
int coverSampleX0 = 0;
int coverSampleY0 = 0;
int coverSampleW = 0;
int coverSampleH = 0;

void setSampleWindow(int srcW, int srcH, int dstW, int dstH) {
  coverSampleX0 = 0;
  coverSampleY0 = 0;
  coverSampleW = srcW;
  coverSampleH = srcH;
  if (srcW < 1 || srcH < 1 || dstW < 1 || dstH < 1) {
    return;
  }
  if (static_cast<int32_t>(srcW) * dstH > static_cast<int32_t>(srcH) * dstW) {
    coverSampleH = srcH;
    coverSampleW = static_cast<int>((static_cast<int32_t>(srcH) * dstW) / dstH);
    if (coverSampleW < 1) {
      coverSampleW = 1;
    }
    if (coverSampleW > srcW) {
      coverSampleW = srcW;
    }
    coverSampleX0 = (srcW - coverSampleW) / 2;
  } else {
    coverSampleW = srcW;
    coverSampleH = static_cast<int>((static_cast<int32_t>(srcW) * dstH) / dstW);
    if (coverSampleH < 1) {
      coverSampleH = 1;
    }
    if (coverSampleH > srcH) {
      coverSampleH = srcH;
    }
    coverSampleY0 = (srcH - coverSampleH) / 2;
  }
}

bool jpgOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (!coverOut || coverSampleW <= 0 || coverSampleH <= 0 || coverDw <= 0 ||
      coverDh <= 0) {
    return false;
  }
  for (uint16_t row = 0; row < h; ++row) {
    const int sy = y + static_cast<int>(row);
    if (sy < coverSampleY0 || sy >= coverSampleY0 + coverSampleH) {
      continue;
    }
    const int relY = sy - coverSampleY0;
    int dy0 = relY * coverDh / coverSampleH;
    int dy1 = (relY + 1) * coverDh / coverSampleH;
    if (dy1 <= dy0) {
      dy1 = dy0 + 1;
    }
    if (dy0 < 0) {
      dy0 = 0;
    }
    if (dy1 > coverDh) {
      dy1 = coverDh;
    }
    if (dy0 >= dy1) {
      continue;
    }
    uint16_t *src = bitmap + static_cast<size_t>(row) * w;
    for (uint16_t col = 0; col < w; ++col) {
      const int sx = x + static_cast<int>(col);
      if (sx < coverSampleX0 || sx >= coverSampleX0 + coverSampleW) {
        continue;
      }
      const int relX = sx - coverSampleX0;
      int dx0 = relX * coverDw / coverSampleW;
      int dx1 = (relX + 1) * coverDw / coverSampleW;
      if (dx1 <= dx0) {
        dx1 = dx0 + 1;
      }
      if (dx0 < 0) {
        dx0 = 0;
      }
      if (dx1 > coverDw) {
        dx1 = coverDw;
      }
      const uint16_t px = src[col];
      for (int dy = dy0; dy < dy1; ++dy) {
        uint16_t *dst = coverOut + static_cast<size_t>(dy) * static_cast<size_t>(coverDw);
        for (int dx = dx0; dx < dx1; ++dx) {
          dst[dx] = px;
        }
      }
    }
  }
  return true;
}

} // namespace

bool imagineLoadCover(const char *path, uint16_t *outRgb, int16_t outW,
                      int16_t outH) {
  if (!path || !outRgb || outW < 8 || outH < 8 || outW > kMaxW || outH > kMaxH) {
    return false;
  }
  if (!imgReady()) {
    return false;
  }

  File peek = imgOpen(path, FILE_READ);
  if (!peek || peek.size() < 4) {
    if (peek) {
      peek.close();
    }
    return false;
  }
  uint8_t magic[4] = {};
  peek.read(magic, 4);
  peek.close();
  const bool isJpeg = magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF;
  if (!isJpeg) {
    return false;
  }

  TJpgDec.setSwapBytes(false);
  uint16_t jw = 0, jh = 0;
  if (TJpgDec.getFsJpgSize(&jw, &jh, path, imgFs()) != JDR_OK || jw == 0) {
    Serial.println("ERR jpeg header");
    return false;
  }

  uint8_t scale = 1;
  while (scale < 8) {
    const uint8_t next = static_cast<uint8_t>(scale * 2);
    if (static_cast<int>(jw / next) < outW ||
        static_cast<int>(jh / next) < outH) {
      break;
    }
    if (jw / scale <= static_cast<uint16_t>(outW * 2) &&
        jh / scale <= static_cast<uint16_t>(outH * 2)) {
      break;
    }
    scale = next;
  }
  while (scale > 1 && (static_cast<int>(jw / scale) < outW ||
                        static_cast<int>(jh / scale) < outH)) {
    scale = static_cast<uint8_t>(scale / 2);
  }

  TJpgDec.setJpgScale(scale);
  const int srcW = static_cast<int>(jw / scale);
  const int srcH = static_cast<int>(jh / scale);
  if (srcW < 1 || srcH < 1) {
    return false;
  }

  coverOut = outRgb;
  coverDw = outW;
  coverDh = outH;
  setSampleWindow(srcW, srcH, coverDw, coverDh);
  memset(outRgb, 0,
         static_cast<size_t>(outW) * static_cast<size_t>(outH) * sizeof(uint16_t));
  TJpgDec.setCallback(jpgOutput);
  const bool ok = TJpgDec.drawFsJpg(0, 0, path, imgFs()) == JDR_OK;
  coverOut = nullptr;
  return ok;
}
