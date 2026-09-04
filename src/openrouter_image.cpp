#include "openrouter_image.h"

#include "image_preview.h"

#include <Flow32.h>

#include <HTTPClient.h>
#include <Stream.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr const char *kDefaultModel = "google/gemini-3.1-flash-image";
constexpr const char *kDir = "/chitram";
constexpr const char *kImgDir = "/chitram/images";
constexpr uint32_t kHttpTimeoutMs = 180000;
/** Panel169 landscape — preview Cover target. */
constexpr int16_t kPreviewW = 280;
constexpr int16_t kPreviewH = 240;

portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t task_ = nullptr;
volatile bool busy_ = false;
volatile bool resultReady_ = false;
/** Bumped on reset/start so a late finish() from a zombie task is ignored. */
volatile uint32_t jobEpoch_ = 0;
uint32_t runningEpoch_ = 0;
uint32_t jobStartedMs_ = 0;
/** Wall-clock budget before start() force-reclaims a stuck job. */
constexpr uint32_t kJobStuckMs = 200000;

OrImageResult result_{};
char status_[96] = "Idle";
uint32_t statusGen_ = 0;

char apiKey_[160] = {};
char prompt_[384] = {};
char model_[80] = {};
/** Optional explicit reference (full image path); empty → auto. */
char refReqPath_[96] = {};
/** Last successful full-image path (for next edit). */
char lastImagePath_[96] = {};
bool attachReference_ = true;
Storage *storage_ = nullptr;

void *psramOrRam(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = malloc(n);
  return p;
}

void setStatus(const char *s) {
  strncpy(status_, s ? s : "", sizeof(status_) - 1);
  status_[sizeof(status_) - 1] = '\0';
  statusGen_++;
}

bool ensureDirs(Storage *st) {
  if (!st || !st->ready()) return false;
  if (!st->exists(kDir) && !st->mkdir(kDir)) return false;
  if (!st->exists(kImgDir) && !st->mkdir(kImgDir)) return false;
  return true;
}

bool jsonEscapeAppend(char *dst, size_t cap, size_t *at, const char *src) {
  for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
    const char c = (char)*p;
    const char *esc = nullptr;
    size_t elen = 1;
    char one[2] = {c, 0};
    switch (c) {
    case '"':
      esc = "\\\"";
      elen = 2;
      break;
    case '\\':
      esc = "\\\\";
      elen = 2;
      break;
    case '\n':
      esc = "\\n";
      elen = 2;
      break;
    case '\r':
      esc = "\\r";
      elen = 2;
      break;
    case '\t':
      esc = "\\t";
      elen = 2;
      break;
    default:
      if ((unsigned char)c < 0x20) continue;
      esc = one;
      elen = 1;
      break;
    }
    if (*at + elen >= cap) return false;
    memcpy(dst + *at, esc, elen);
    *at += elen;
  }
  dst[*at] = '\0';
  return true;
}

bool appendFileBase64(char *dst, size_t cap, size_t *at, Storage *st,
                       const char *path) {
  if (!st || !path || !path[0] || !at) return false;
  File f = st->open(path, FILE_READ);
  if (!f) {
    Serial.printf("[or-image] ref open fail %s\n", path);
    return false;
  }
  const size_t fileSize = f.size();
  if (fileSize < 32 || fileSize > 200000) {
    Serial.printf("[or-image] ref size bad %u\n", (unsigned)fileSize);
    f.close();
    return false;
  }
  uint8_t *raw = (uint8_t *)psramOrRam(fileSize);
  if (!raw) {
    f.close();
    return false;
  }
  const size_t got = f.read(raw, fileSize);
  f.close();
  if (got != fileSize) {
    heap_caps_free(raw);
    return false;
  }

  size_t need = 0;
  mbedtls_base64_encode(nullptr, 0, &need, raw, fileSize);
  if (*at + need >= cap) {
    heap_caps_free(raw);
    return false;
  }
  size_t written = 0;
  const int rc =
      mbedtls_base64_encode((unsigned char *)(dst + *at), cap - *at, &written,
                            raw, fileSize);
  heap_caps_free(raw);
  if (rc != 0 || written == 0) return false;
  *at += written;
  dst[*at] = '\0';
  return true;
}

bool resolveReferenceSidecar(char *out, size_t outLen) {
  if (!out || outLen < 8 || !storage_ || !storage_->ready()) return false;
  out[0] = '\0';
  if (!attachReference_) return false;

  const char *src = nullptr;
  if (refReqPath_[0]) {
    src = refReqPath_;
  } else if (imagePreviewPath() && imagePreviewPath()[0]) {
    src = imagePreviewPath();
  } else if (lastImagePath_[0]) {
    src = lastImagePath_;
  }
  if (!src || !src[0]) return false;

  if (!imagePreviewSidecarPath(out, outLen, src)) return false;

  if (storage_->exists(out)) return true;

  // Sidecar may still be encoding from the previous preview load.
  const uint32_t t0 = millis();
  while (!storage_->exists(out) && millis() - t0 < 4000) {
    delay(50);
  }
  if (storage_->exists(out)) return true;

  // Last chance: current pixels are that image — encode sync on this task.
  if (imagePreviewPath() && strcmp(imagePreviewPath(), src) == 0) {
    if (imagePreviewEnsureSidecar(storage_) && storage_->exists(out)) {
      return true;
    }
  }

  Serial.printf("[or-image] no sidecar for ref %s\n", src);
  out[0] = '\0';
  return false;
}

bool buildRequestBody(char *dst, size_t cap, const char *refSidecar) {
  size_t at = 0;
  auto lit = [&](const char *s) -> bool {
    const size_t n = strlen(s);
    if (at + n >= cap) return false;
    memcpy(dst + at, s, n);
    at += n;
    dst[at] = '\0';
    return true;
  };
  const char *model = model_[0] ? model_ : kDefaultModel;

  if (!lit("{\"model\":\"")) return false;
  if (!lit(model)) return false;
  if (!lit("\",\"prompt\":\"")) return false;
  if (!jsonEscapeAppend(dst, cap, &at, prompt_)) return false;

  // Only send params each slug advertises. Prefer JPEG when the model
  // supports it; Gemini is PNG-only (use resolution 512 when available).
  const bool river = strstr(model, "riverflow") != nullptr;
  const bool flux2 = strstr(model, "flux.2") != nullptr;
  const bool is25 = strstr(model, "gemini-2.5-flash-image") != nullptr;
  const bool isLite = strstr(model, "flash-lite-image") != nullptr;

  if (river) {
    if (!lit("\",\"aspect_ratio\":\"4:3\",\"resolution\":\"1K\","
             "\"output_format\":\"jpeg\""))
      return false;
  } else if (flux2) {
    if (!lit("\",\"aspect_ratio\":\"4:3\",\"output_format\":\"jpeg\""))
      return false;
  } else if (is25) {
    if (!lit("\",\"aspect_ratio\":\"4:3\"")) return false;
  } else if (isLite) {
    if (!lit("\",\"aspect_ratio\":\"4:3\",\"resolution\":\"1K\"")) return false;
  } else {
    // Gemini 3.1 flash-image — PNG; 512 is the smallest advertised tier.
    if (!lit("\",\"aspect_ratio\":\"4:3\",\"resolution\":\"512\"")) return false;
  }

  if (refSidecar && refSidecar[0]) {
    if (!lit(",\"input_references\":[{\"type\":\"image_url\",\"image_url\":{"
             "\"url\":\"data:image/jpeg;base64,"))
      return false;
    if (!appendFileBase64(dst, cap, &at, storage_, refSidecar)) return false;
    if (!lit("\"}}]")) return false;
  }

  if (!lit(",\"n\":1}")) return false;
  return true;
}

bool isB64Char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=' ||
         c == '-' || c == '_'; // base64url
}

char normalizeB64(char c) {
  if (c == '-') return '+';
  if (c == '_') return '/';
  return c;
}

const char *extFromMagic(const uint8_t *b, size_t n) {
  if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "jpg";
  if (n >= 4 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G')
    return "png";
  if (n >= 4 && b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F')
    return "webp";
  return "bin";
}

/**
 * HTTPClient::writeToStream sink — finds "b64_json":"…" and decodes to SD.
 * Must use writeToStream (not raw WiFiClient reads) so chunked Transfer-Encoding
 * framing is stripped; otherwise chunk-size hex contaminates base64 (-44).
 */
class B64JsonSink : public Stream {
public:
  Storage *st = nullptr;
  char *absPathOut = nullptr;
  size_t absPathCap = 0;
  char *err = nullptr;
  size_t errLen = 0;

  enum class Phase : uint8_t { HuntKey, AfterKey, InValue };
  Phase phase = Phase::HuntKey;
  size_t keyAt = 0;
  enum class After : uint8_t { Colon, Quote };
  After after = After::Colon;

  char quad[4] = {};
  int qi = 0;
  uint8_t head[16] = {};
  size_t headLen = 0;
  File f;
  bool opened = false;
  bool done = false;
  bool failed = false;
  size_t written = 0;
  size_t b64Chars = 0;
  size_t totalIn = 0;

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t *buf, size_t size) override {
    for (size_t i = 0; i < size; i++) {
      totalIn++;
      if (failed) continue;
      if (done) continue; // drain remainder of HTTP body
      const int rc = feedByte((char)buf[i]);
      if (rc < 0) failed = true;
      else if (rc == 0) done = true;
    }
    return size;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  bool openWithMagic() {
    const char *ext = extFromMagic(head, headLen);
    char rel[80];
    snprintf(rel, sizeof(rel), "%s/%lu.%s", kImgDir, (unsigned long)millis(),
             ext);
    if (!st->absPath(rel, absPathOut, absPathCap)) {
      snprintf(err, errLen, "path failed");
      return false;
    }
    f = st->open(absPathOut, FILE_WRITE);
    if (!f) {
      snprintf(err, errLen, "open failed");
      return false;
    }
    if (f.write(head, headLen) != headLen) {
      f.close();
      snprintf(err, errLen, "sd write failed");
      return false;
    }
    written = headLen;
    opened = true;
    Serial.printf("[or-image] writing %s (magic %s)\n", absPathOut, ext);
    return true;
  }

  bool flushQuad() {
    if (qi != 4) return true;
    uint8_t out[3];
    size_t outLen = 0;
    const int rc = mbedtls_base64_decode(out, sizeof(out), &outLen,
                                         (const unsigned char *)quad, 4);
    const char qsnap[5] = {quad[0], quad[1], quad[2], quad[3], 0};
    qi = 0;
    if (rc != 0) {
      snprintf(err, errLen, "b64 decode %d", rc);
      Serial.printf("[or-image] mbedtls %d at b64char=%u quad=%.4s\n", rc,
                    (unsigned)b64Chars, qsnap);
      return false;
    }
    if (!opened) {
      size_t copy = outLen;
      if (headLen + copy > sizeof(head)) copy = sizeof(head) - headLen;
      memcpy(head + headLen, out, copy);
      headLen += copy;
      if (headLen >= 8) {
        if (!openWithMagic()) return false;
        if (outLen > copy) {
          const size_t rem = outLen - copy;
          if (f.write(out + copy, rem) != rem) {
            snprintf(err, errLen, "sd write failed");
            return false;
          }
          written += rem;
        }
      }
      return true;
    }
    if (f.write(out, outLen) != outLen) {
      snprintf(err, errLen, "sd write failed");
      return false;
    }
    written += outLen;
    return true;
  }

  int feedValue(char c) {
    if (c == '"') {
      if (qi == 1) {
        // Valid base64 length is never 4k+1; usually chunk corruption.
        snprintf(err, errLen, "b64 length bad");
        return -1;
      }
      if (qi != 0) {
        while (qi < 4) quad[qi++] = '=';
        if (!flushQuad()) return -1;
      }
      return 0;
    }
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') return 1;
    if (!isB64Char(c)) {
      snprintf(err, errLen, "bad b64 char");
      Serial.printf("[or-image] bad char 0x%02x at in=%u b64=%u\n",
                    (unsigned char)c, (unsigned)totalIn, (unsigned)b64Chars);
      return -1;
    }
    quad[qi++] = normalizeB64(c);
    b64Chars++;
    if (qi == 4 && !flushQuad()) return -1;
    return 1;
  }

  int feedByte(char c) {
    static constexpr char kKey[] = "\"b64_json\"";
    switch (phase) {
    case Phase::HuntKey:
      if (c == kKey[keyAt]) {
        keyAt++;
        if (kKey[keyAt] == '\0') {
          phase = Phase::AfterKey;
          after = After::Colon;
          keyAt = 0;
        }
      } else {
        keyAt = (c == kKey[0]) ? 1 : 0;
      }
      return 1;
    case Phase::AfterKey:
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 1;
      if (after == After::Colon) {
        if (c != ':') {
          snprintf(err, errLen, "b64_json missing ':'");
          return -1;
        }
        after = After::Quote;
        return 1;
      }
      if (c == 'n') {
        snprintf(err, errLen, "b64_json is null");
        return -1;
      }
      if (c != '"') {
        snprintf(err, errLen, "b64_json not a string");
        return -1;
      }
      phase = Phase::InValue;
      return 1;
    case Phase::InValue:
      return feedValue(c);
    }
    return 1;
  }

  void closeFile() {
    if (opened && f) f.close();
  }
};

bool streamB64JsonToFile(HTTPClient &http, Storage *st, char *absPathOut,
                         size_t absPathCap, char *err, size_t errLen) {
  B64JsonSink sink;
  sink.st = st;
  sink.absPathOut = absPathOut;
  sink.absPathCap = absPathCap;
  sink.err = err;
  sink.errLen = errLen;

  const uint32_t t0 = millis();
  // Decodes Transfer-Encoding: chunked — raw getStreamPtr()->read does not.
  const int n = http.writeToStream(&sink);
  sink.closeFile();

  Serial.printf("[or-image] stream http=%d in=%u b64chars=%u written=%u "
                "done=%d fail=%d elapsed=%lums\n",
                n, (unsigned)sink.totalIn, (unsigned)sink.b64Chars,
                (unsigned)sink.written, sink.done ? 1 : 0, sink.failed ? 1 : 0,
                (unsigned long)(millis() - t0));

  if (sink.failed) {
    if (!err[0]) snprintf(err, errLen, "decode failed");
    return false;
  }
  if (sink.done && sink.opened && sink.written > 0) {
    Serial.printf("[or-image] wrote %u bytes → %s\n", (unsigned)sink.written,
                  absPathOut);
    return true;
  }
  if (n < 0) {
    snprintf(err, errLen, "http stream %d", n);
    return false;
  }
  if (sink.phase != B64JsonSink::Phase::InValue) {
    snprintf(err, errLen, "no b64_json in response");
  } else {
    snprintf(err, errLen, "b64_json truncated");
  }
  return false;
}

void finish(bool ok, const char *path, const char *error) {
  bool publish = false;
  portENTER_CRITICAL(&mux_);
  if (runningEpoch_ == jobEpoch_) {
    result_ = OrImageResult{};
    result_.ok = ok;
    if (path) {
      strncpy(result_.path, path, sizeof(result_.path) - 1);
    }
    if (error) {
      strncpy(result_.error, error, sizeof(result_.error) - 1);
    }
    resultReady_ = true;
    busy_ = false;
    publish = true;
  }
  // Stale job after orImageReset(): do not publish or touch busy_ (may belong
  // to a newer start).
  portEXIT_CRITICAL(&mux_);
  if (!publish) {
    Serial.printf("[or-image] stale finish ignored ok=%d\n", ok ? 1 : 0);
    return;
  }
  if (ok) {
    char s[96];
    snprintf(s, sizeof(s), "Saved %s", path ? path : "");
    setStatus(s);
    Serial.printf("[or-image] OK %s\n", path ? path : "");
  } else {
    setStatus(error && error[0] ? error : "Image failed");
    Serial.printf("[or-image] FAIL: %s\n", error && error[0] ? error : "?");
  }
}

bool jobIsStale() {
  portENTER_CRITICAL(&mux_);
  const bool stale = (runningEpoch_ != jobEpoch_);
  portEXIT_CRITICAL(&mux_);
  return stale;
}

void runJob() {
  if (jobIsStale()) {
    finish(false, nullptr, "cancelled");
    return;
  }
  setStatus("Generating image…");

  if (WiFi.status() != WL_CONNECTED) {
    finish(false, nullptr, "Wi-Fi down");
    return;
  }
  if (!storage_ || !storage_->ready()) {
    finish(false, nullptr, "SD not ready");
    return;
  }
  if (!ensureDirs(storage_)) {
    finish(false, nullptr, "mkdir failed");
    return;
  }

  char refSide[96] = {};
  const bool haveRef = resolveReferenceSidecar(refSide, sizeof(refSide));
  if (haveRef) {
    Serial.printf("[or-image] attaching ref %s\n", refSide);
    setStatus("Generating (with ref)…");
  }

  size_t bodyCap = 2048;
  if (haveRef) {
    File rf = storage_->open(refSide, FILE_READ);
    if (rf) {
      bodyCap += (rf.size() * 4) / 3 + 128;
      rf.close();
    } else {
      bodyCap += 65536;
    }
  }
  char *body = (char *)psramOrRam(bodyCap);
  if (!body) {
    finish(false, nullptr, "OOM body");
    return;
  }
  if (!buildRequestBody(body, bodyCap, haveRef ? refSide : nullptr)) {
    free(body);
    finish(false, nullptr, haveRef ? "ref/body build fail" : "prompt too long");
    return;
  }
  Serial.printf("[or-image] body %u bytes (ref=%d)\n", (unsigned)strlen(body),
                haveRef ? 1 : 0);

  // Talk's TLS WS often starves lwIP DNS. Pre-resolve, then POST with retries.
  IPAddress orIp;
  bool dnsOk = false;
  for (int i = 0; i < 5; i++) {
    if (WiFi.hostByName("openrouter.ai", orIp) == 1) {
      dnsOk = true;
      break;
    }
    Serial.printf("[or-image] DNS fail try=%d rssi=%d\n", i + 1, WiFi.RSSI());
    delay(300 * (i + 1));
  }
  if (!dnsOk) {
    free(body);
    finish(false, nullptr, "DNS openrouter.ai");
    return;
  }
  Serial.printf("[or-image] DNS openrouter.ai → %s\n", orIp.toString().c_str());

  if (jobIsStale()) {
    finish(false, nullptr, "cancelled");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(180000);
  client.setHandshakeTimeout(60);

  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(65535);

  int code = 0;
  constexpr int kAttempts = 3;
  for (int attempt = 0; attempt < kAttempts; attempt++) {
    if (attempt > 0) {
      http.end();
      delay(400 * attempt);
      // Refresh DNS between retries — cache may have expired under load.
      (void)WiFi.hostByName("openrouter.ai", orIp);
      Serial.printf("[or-image] POST retry %d\n", attempt);
    }
    // Hostname begin (not IP) so TLS SNI stays openrouter.ai.
    if (!http.begin(client, "https://openrouter.ai/api/v1/images")) {
      Serial.printf("[or-image] http begin failed try=%d\n", attempt + 1);
      code = -1;
      continue;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + apiKey_);
    http.addHeader("HTTP-Referer", "https://chitram.device");
    http.addHeader("X-Title", "chitram");

    Serial.printf("[or-image] POST model=%s prompt_len=%u try=%d\n",
                  model_[0] ? model_ : kDefaultModel,
                  (unsigned)strlen(prompt_), attempt + 1);
    code = http.POST(reinterpret_cast<uint8_t *>(body), strlen(body));
    if (code > 0) break;

    String he = http.errorToString(code);
    Serial.printf("[or-image] transport code=%d %s rssi=%d\n", code,
                  he.c_str(), WiFi.RSSI());
  }
  free(body);
  body = nullptr;

  if (code <= 0) {
    http.end();
    char err[96];
    snprintf(err, sizeof(err), "http %d", code);
    finish(false, nullptr, err);
    return;
  }

  const int contentLen = http.getSize();
  Serial.printf("[or-image] HTTP %d contentLen=%d (chunked if -1)\n", code,
                contentLen);

  if (code < 200 || code >= 300) {
    char errBody[400];
    size_t got = 0;
    WiFiClient *stream = http.getStreamPtr();
    while (stream && stream->available() && got + 1 < sizeof(errBody)) {
      int r = stream->readBytes(errBody + got, sizeof(errBody) - 1 - got);
      if (r <= 0) break;
      got += (size_t)r;
    }
    errBody[got] = '\0';
    Serial.printf("[or-image] error body: %.400s\n", errBody);
    http.end();
    char err[96];
    snprintf(err, sizeof(err), "HTTP %d", code);
    finish(false, nullptr, err);
    return;
  }

  char absPath[96] = {};
  char err[96];
  const bool ok =
      streamB64JsonToFile(http, storage_, absPath, sizeof(absPath), err,
                          sizeof(err));
  http.end();
  if (!ok) {
    finish(false, nullptr, err);
    return;
  }

  if (!imagePreviewLoad(storage_, absPath, kPreviewW, kPreviewH)) {
    Serial.printf("[or-image] saved but preview decode failed\n");
  } else {
    // Sync sidecar on this task so the next edit can attach immediately.
    (void)imagePreviewEnsureSidecar(storage_);
  }
  strncpy(lastImagePath_, absPath, sizeof(lastImagePath_) - 1);
  lastImagePath_[sizeof(lastImagePath_) - 1] = '\0';
  finish(true, absPath, nullptr);
}

void imageTask(void *arg) {
  (void)arg;
  runJob();
  portENTER_CRITICAL(&mux_);
  // Only clear the handle if we are still the active task slot.
  task_ = nullptr;
  // If reset cleared busy_ already, leave it; if we were cancelled mid-flight
  // without finish publishing, ensure busy cannot stick.
  if (runningEpoch_ != jobEpoch_) {
    busy_ = false;
  }
  portEXIT_CRITICAL(&mux_);
  vTaskDelete(nullptr);
}

} // namespace

static void reclaimIfStuck() {
  portENTER_CRITICAL(&mux_);
  const bool stuck =
      busy_ && jobStartedMs_ != 0 &&
      (millis() - jobStartedMs_ > kJobStuckMs);
  if (stuck) {
    jobEpoch_++;
    busy_ = false;
    resultReady_ = false;
    result_ = OrImageResult{};
    Serial.printf("[or-image] reclaim stuck job age=%lums\n",
                  (unsigned long)(millis() - jobStartedMs_));
    jobStartedMs_ = 0;
  }
  portEXIT_CRITICAL(&mux_);
}

bool orImageStart(const OrImageRequest &req, OrImageResult *earlyErr) {
  auto fail = [&](const char *msg) -> bool {
    if (earlyErr) {
      *earlyErr = OrImageResult{};
      earlyErr->ok = false;
      strncpy(earlyErr->error, msg, sizeof(earlyErr->error) - 1);
    }
    return false;
  };

  if (!req.apiKey || !req.apiKey[0]) return fail("no OpenRouter API key");
  if (!req.prompt || !req.prompt[0]) return fail("missing prompt");
  if (!req.storage) return fail("no storage");

  reclaimIfStuck();

  // After force-reset, a zombie task may still be winding down — wait briefly.
  for (int i = 0; i < 50; i++) {
    portENTER_CRITICAL(&mux_);
    const bool hasTask = task_ != nullptr;
    const bool isBusy = busy_;
    portEXIT_CRITICAL(&mux_);
    if (!hasTask && !isBusy) break;
    if (!hasTask && isBusy) {
      // busy stuck with no task — reclaim immediately.
      portENTER_CRITICAL(&mux_);
      busy_ = false;
      jobEpoch_++;
      portEXIT_CRITICAL(&mux_);
      Serial.printf("[or-image] cleared orphan busy\n");
      break;
    }
    delay(20);
  }

  portENTER_CRITICAL(&mux_);
  if (busy_ || task_) {
    portEXIT_CRITICAL(&mux_);
    return fail("image gen busy");
  }
  busy_ = true;
  resultReady_ = false;
  runningEpoch_ = ++jobEpoch_;
  jobStartedMs_ = millis();
  portEXIT_CRITICAL(&mux_);

  strncpy(apiKey_, req.apiKey, sizeof(apiKey_) - 1);
  apiKey_[sizeof(apiKey_) - 1] = '\0';
  strncpy(prompt_, req.prompt, sizeof(prompt_) - 1);
  prompt_[sizeof(prompt_) - 1] = '\0';
  // Prefer the built-in Gemini default unless the tool explicitly picks a
  // JPEG-capable slug (riverflow / flux.2).
  if (req.model && req.model[0] &&
      (strstr(req.model, "riverflow") || strstr(req.model, "flux.2") ||
       strstr(req.model, "gemini-3.1-flash-image"))) {
    strncpy(model_, req.model, sizeof(model_) - 1);
    model_[sizeof(model_) - 1] = '\0';
  } else {
    model_[0] = '\0';
  }
  storage_ = req.storage;
  attachReference_ = req.attachReference;
  refReqPath_[0] = '\0';
  if (req.referencePath && req.referencePath[0]) {
    strncpy(refReqPath_, req.referencePath, sizeof(refReqPath_) - 1);
    refReqPath_[sizeof(refReqPath_) - 1] = '\0';
  }

  const BaseType_t ok =
      xTaskCreatePinnedToCore(imageTask, "or_image", 16384, nullptr, 3,
                              &task_, 0);
  if (ok != pdPASS) {
    portENTER_CRITICAL(&mux_);
    busy_ = false;
    task_ = nullptr;
    jobStartedMs_ = 0;
    portEXIT_CRITICAL(&mux_);
    return fail("task create failed");
  }
  setStatus("Generating image…");
  return true;
}

bool orImageBusy() {
  reclaimIfStuck();
  portENTER_CRITICAL(&mux_);
  const bool b = busy_ || task_ != nullptr;
  portEXIT_CRITICAL(&mux_);
  return b;
}

bool orImageTakeResult(OrImageResult *out) {
  if (!out) return false;
  portENTER_CRITICAL(&mux_);
  if (!resultReady_) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }
  *out = result_;
  resultReady_ = false;
  portEXIT_CRITICAL(&mux_);
  return true;
}

const char *orImageStatus() { return status_; }

uint32_t orImageStatusGen() { return statusGen_; }

void orImageReset() {
  // Always reclaim — Talk stop/start must not leave generate_image wedged.
  portENTER_CRITICAL(&mux_);
  jobEpoch_++;
  busy_ = false;
  resultReady_ = false;
  result_ = OrImageResult{};
  jobStartedMs_ = 0;
  // Leave task_ for the worker to null on exit; start() waits briefly.
  portEXIT_CRITICAL(&mux_);
  lastImagePath_[0] = '\0';
  refReqPath_[0] = '\0';
  setStatus("Idle");
  Serial.printf("[or-image] reset (epoch=%u)\n", (unsigned)jobEpoch_);
}
