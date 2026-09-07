#include "elevenlabs_image.h"

#include "elevenlabs_agent.h"
#include "gallery_catalog.h"
#include "image_preview.h"

#include <Flow32.h>

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr const char *kDefaultModel = "gemini-3.1-flash-image";
constexpr const char *kHost = "api.elevenlabs.io";
constexpr const char *kDir = "/heyvinu";
constexpr const char *kImgDir = "/heyvinu/images";
constexpr uint32_t kHttpTimeoutMs = 60000;
constexpr uint32_t kPollIntervalMs = 2500;
constexpr uint32_t kPollMaxMs = 180000;
constexpr int16_t kPreviewW = 280;
constexpr int16_t kPreviewH = 240;

portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t task_ = nullptr;
volatile bool busy_ = false;
volatile bool resultReady_ = false;
volatile uint32_t jobEpoch_ = 0;
uint32_t runningEpoch_ = 0;
uint32_t jobStartedMs_ = 0;
constexpr uint32_t kJobStuckMs = 220000;

ElImageResult result_{};
char status_[96] = "Idle";
uint32_t statusGen_ = 0;

char apiKey_[96] = {};
char prompt_[384] = {};
char model_[64] = {};
char refReqPath_[96] = {};
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
    Serial.printf("[el-image] ref open fail %s\n", path);
    return false;
  }
  const size_t fileSize = f.size();
  if (fileSize < 32 || fileSize > 200000) {
    Serial.printf("[el-image] ref size bad %u\n", (unsigned)fileSize);
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

  const uint32_t t0 = millis();
  while (!storage_->exists(out) && millis() - t0 < 4000) {
    delay(50);
  }
  if (storage_->exists(out)) return true;

  if (imagePreviewPath() && strcmp(imagePreviewPath(), src) == 0) {
    if (imagePreviewEnsureSidecar(storage_) && storage_->exists(out)) {
      return true;
    }
  }

  Serial.printf("[el-image] no sidecar for ref %s\n", src);
  out[0] = '\0';
  return false;
}

/** Map OpenRouter-style or bare slugs to ElevenLabs model_id. */
const char *resolveModelId() {
  const char *m = model_[0] ? model_ : kDefaultModel;
  if (strncmp(m, "google/", 7) == 0) m += 7;
  if (strstr(m, "gemini-3.1-flash-lite-image"))
    return "gemini-3.1-flash-lite-image";
  if (strstr(m, "gemini-3.1-flash-image")) return "gemini-3.1-flash-image";
  if (strstr(m, "gemini-3-pro-image")) return "gemini-3-pro-image";
  if (strstr(m, "gemini-2.5-flash-image")) return "gemini-2.5-flash-image";
  if (strstr(m, "gpt-image-2")) return "gpt-image-2";
  if (strstr(m, "gpt-image-1.5")) return "gpt-image-1.5";
  if (strstr(m, "gpt-image-1")) return "gpt-image-1";
  return kDefaultModel;
}

bool buildCreateBody(char *dst, size_t cap, const char *refSidecar) {
  size_t at = 0;
  auto lit = [&](const char *s) -> bool {
    const size_t n = strlen(s);
    if (at + n >= cap) return false;
    memcpy(dst + at, s, n);
    at += n;
    dst[at] = '\0';
    return true;
  };

  const char *model = resolveModelId();
  if (!lit("{\"model_id\":\"")) return false;
  if (!lit(model)) return false;
  if (!lit("\",\"prompt\":\"")) return false;
  if (!jsonEscapeAppend(dst, cap, &at, prompt_)) return false;

  // Panel-friendly; Gemini 3.1 flash supports 512.
  if (strcmp(model, "gemini-2.5-flash-image") == 0) {
    if (!lit("\",\"aspect_ratio\":\"4:3\"")) return false;
  } else if (strcmp(model, "gemini-3.1-flash-lite-image") == 0) {
    if (!lit("\",\"aspect_ratio\":\"4:3\",\"resolution\":\"1K\"")) return false;
  } else if (strncmp(model, "gpt-image", 9) == 0) {
    if (!lit("\",\"aspect_ratio\":\"1:1\"")) return false;
  } else {
    if (!lit("\",\"aspect_ratio\":\"4:3\",\"resolution\":\"512\"")) return false;
  }

  if (refSidecar && refSidecar[0]) {
    if (!lit(",\"images\":[{\"type\":\"inline_base64\",\"mime_type\":"
             "\"image/jpeg\",\"content_base64\":\""))
      return false;
    if (!appendFileBase64(dst, cap, &at, storage_, refSidecar)) return false;
    if (!lit("\"}]")) return false;
  }

  if (!lit("}")) return false;
  return true;
}

bool extractJsonString(const char *json, const char *key, char *out,
                       size_t outLen) {
  if (!json || !key || !out || outLen < 2) return false;
  out[0] = '\0';
  char needle[48];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char *p = strstr(json, needle);
  if (!p) return false;
  p += strlen(needle);
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (*p != ':') return false;
  p++;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (*p != '"') return false;
  p++;
  size_t n = 0;
  while (*p && *p != '"' && n + 1 < outLen) {
    if (*p == '\\' && p[1]) {
      p++;
      out[n++] = *p++;
      continue;
    }
    out[n++] = *p++;
  }
  out[n] = '\0';
  return n > 0;
}

bool httpReadBody(HTTPClient &http, char *buf, size_t cap, size_t *gotOut) {
  if (!buf || cap < 2) return false;
  size_t got = 0;
  WiFiClient *stream = http.getStreamPtr();
  const uint32_t t0 = millis();
  while (got + 1 < cap && millis() - t0 < kHttpTimeoutMs) {
    if (!stream) break;
    const int avail = stream->available();
    if (avail <= 0) {
      if (!http.connected() && avail <= 0) break;
      delay(10);
      continue;
    }
    const int r = stream->readBytes(buf + got, cap - 1 - got);
    if (r <= 0) break;
    got += (size_t)r;
  }
  buf[got] = '\0';
  if (gotOut) *gotOut = got;
  return got > 0;
}

const char *extFromMagic(const uint8_t *b, size_t n) {
  if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "jpg";
  if (n >= 4 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G')
    return "png";
  if (n >= 4 && b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F')
    return "webp";
  return "bin";
}

bool jobIsStale() {
  portENTER_CRITICAL(&mux_);
  const bool stale = (runningEpoch_ != jobEpoch_);
  portEXIT_CRITICAL(&mux_);
  return stale;
}

void finish(bool ok, const char *path, const char *error) {
  bool publish = false;
  portENTER_CRITICAL(&mux_);
  if (runningEpoch_ == jobEpoch_) {
    result_ = ElImageResult{};
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
  portEXIT_CRITICAL(&mux_);
  if (!publish) {
    Serial.printf("[el-image] stale finish ignored ok=%d\n", ok ? 1 : 0);
    return;
  }
  if (ok) {
    char s[96];
    snprintf(s, sizeof(s), "Saved %s", path ? path : "");
    setStatus(s);
    Serial.printf("[el-image] OK %s\n", path ? path : "");
    if (path && path[0] && storage_) {
      GalleryCatalogMeta meta;
      meta.agentId = elAgentActiveId();
      meta.description = prompt_;
      meta.provider = "elevenlabs";
      (void)galleryCatalogAdd(storage_, path, meta);
    }
  } else {
    setStatus(error && error[0] ? error : "Image failed");
    Serial.printf("[el-image] FAIL: %s\n", error && error[0] ? error : "?");
  }
}

bool dnsResolve(const char *host) {
  IPAddress ip;
  for (int i = 0; i < 5; i++) {
    if (WiFi.hostByName(host, ip) == 1) return true;
    Serial.printf("[el-image] DNS fail %s try=%d\n", host, i + 1);
    delay(300 * (i + 1));
  }
  return false;
}

bool downloadUrlToFile(const char *url, char *absPathOut, size_t absPathCap,
                       char *err, size_t errLen) {
  if (!url || !url[0] || !storage_) {
    snprintf(err, errLen, "bad download args");
    return false;
  }

  // Extract host for DNS hint (optional).
  const char *hostStart = strstr(url, "://");
  hostStart = hostStart ? hostStart + 3 : url;
  char host[96] = {};
  size_t hi = 0;
  for (const char *p = hostStart; *p && *p != '/' && *p != ':' && hi + 1 < sizeof(host);
       p++) {
    host[hi++] = *p;
  }
  host[hi] = '\0';
  if (host[0]) (void)dnsResolve(host);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(120000);
  client.setHandshakeTimeout(60);

  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(65535);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, url)) {
    snprintf(err, errLen, "download begin fail");
    return false;
  }

  const int code = http.GET();
  Serial.printf("[el-image] download HTTP %d\n", code);
  if (code < 200 || code >= 300) {
    http.end();
    snprintf(err, errLen, "download HTTP %d", code);
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  if (!stream) {
    http.end();
    snprintf(err, errLen, "no download stream");
    return false;
  }

  uint8_t head[16] = {};
  size_t headLen = 0;
  const uint32_t t0 = millis();
  while (headLen < sizeof(head) && millis() - t0 < 30000) {
    if (stream->available() <= 0) {
      if (!http.connected()) break;
      delay(5);
      continue;
    }
    int r = stream->readBytes(head + headLen, sizeof(head) - headLen);
    if (r <= 0) break;
    headLen += (size_t)r;
  }
  if (headLen < 3) {
    http.end();
    snprintf(err, errLen, "empty image body");
    return false;
  }

  const char *ext = extFromMagic(head, headLen);
  char rel[80];
  snprintf(rel, sizeof(rel), "%s/%lu.%s", kImgDir, (unsigned long)millis(),
           ext);
  if (!storage_->absPath(rel, absPathOut, absPathCap)) {
    http.end();
    snprintf(err, errLen, "path failed");
    return false;
  }

  File f = storage_->open(absPathOut, FILE_WRITE);
  if (!f) {
    http.end();
    snprintf(err, errLen, "open failed");
    return false;
  }
  if (f.write(head, headLen) != headLen) {
    f.close();
    http.end();
    snprintf(err, errLen, "sd write fail");
    return false;
  }

  size_t written = headLen;
  uint8_t buf[1024];
  while (millis() - t0 < 120000) {
    const int avail = stream->available();
    if (avail <= 0) {
      if (!http.connected() && avail <= 0) break;
      delay(5);
      continue;
    }
    const int n = stream->readBytes(buf, sizeof(buf));
    if (n <= 0) break;
    if (f.write(buf, n) != (size_t)n) {
      f.close();
      http.end();
      snprintf(err, errLen, "sd write fail");
      return false;
    }
    written += (size_t)n;
  }
  f.close();
  http.end();
  Serial.printf("[el-image] wrote %s (%u bytes)\n", absPathOut,
                (unsigned)written);
  if (written < 64) {
    snprintf(err, errLen, "image too small");
    return false;
  }
  return true;
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
    Serial.printf("[el-image] attaching ref %s\n", refSide);
    setStatus("Generating (with ref)…");
  }

  size_t bodyCap = 1536;
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
  if (!buildCreateBody(body, bodyCap, haveRef ? refSide : nullptr)) {
    free(body);
    finish(false, nullptr, haveRef ? "ref/body build fail" : "prompt too long");
    return;
  }
  Serial.printf("[el-image] create body %u bytes (ref=%d)\n",
                (unsigned)strlen(body), haveRef ? 1 : 0);

  if (!dnsResolve(kHost)) {
    free(body);
    finish(false, nullptr, "DNS api.elevenlabs.io");
    return;
  }
  if (jobIsStale()) {
    free(body);
    finish(false, nullptr, "cancelled");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(60000);
  client.setHandshakeTimeout(60);

  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(65535);

  char createUrl[64];
  snprintf(createUrl, sizeof(createUrl), "https://%s/v1/flows/image", kHost);

  int code = 0;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) {
      http.end();
      delay(400 * attempt);
      (void)dnsResolve(kHost);
      Serial.printf("[el-image] POST retry %d\n", attempt);
    }
    if (!http.begin(client, createUrl)) {
      code = -1;
      continue;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("xi-api-key", apiKey_);

    Serial.printf("[el-image] POST model=%s prompt_len=%u try=%d\n",
                  resolveModelId(), (unsigned)strlen(prompt_), attempt + 1);
    code = http.POST(reinterpret_cast<uint8_t *>(body), strlen(body));
    if (code > 0) break;
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

  char resp[512];
  size_t respLen = 0;
  (void)httpReadBody(http, resp, sizeof(resp), &respLen);
  http.end();
  Serial.printf("[el-image] create HTTP %d len=%u head=%.120s\n", code,
                (unsigned)respLen, resp);

  if (code == 402) {
    finish(false, nullptr, "Pro plan required");
    return;
  }
  if (code < 200 || code >= 300) {
    char err[96];
    if (strstr(resp, "quota") || strstr(resp, "credit")) {
      snprintf(err, sizeof(err), "Out of credits");
    } else {
      snprintf(err, sizeof(err), "HTTP %d", code);
    }
    finish(false, nullptr, err);
    return;
  }

  char genId[48] = {};
  if (!extractJsonString(resp, "id", genId, sizeof(genId))) {
    finish(false, nullptr, "no generation id");
    return;
  }
  Serial.printf("[el-image] generation id=%s\n", genId);

  // Poll until completed / failed.
  char statusVal[32] = {};
  char contentUrl[1200] = {};
  char failMsg[96] = {};
  const uint32_t pollStart = millis();
  bool terminal = false;

  while (millis() - pollStart < kPollMaxMs) {
    if (jobIsStale()) {
      finish(false, nullptr, "cancelled");
      return;
    }
    delay(kPollIntervalMs);
    setStatus("Waiting for image…");

    char getUrl[96];
    snprintf(getUrl, sizeof(getUrl), "https://%s/v1/flows/image/%s", kHost,
             genId);

    WiFiClientSecure pollClient;
    pollClient.setInsecure();
    pollClient.setTimeout(30000);
    pollClient.setHandshakeTimeout(45);

    HTTPClient pollHttp;
    pollHttp.setReuse(false);
    pollHttp.setTimeout(30000);
    if (!pollHttp.begin(pollClient, getUrl)) {
      Serial.printf("[el-image] poll begin fail\n");
      continue;
    }
    pollHttp.addHeader("xi-api-key", apiKey_);
    const int pcode = pollHttp.GET();
    char *pbody = (char *)psramOrRam(1600);
    if (!pbody) {
      pollHttp.end();
      finish(false, nullptr, "OOM poll");
      return;
    }
    size_t plen = 0;
    (void)httpReadBody(pollHttp, pbody, 1600, &plen);
    pollHttp.end();

    Serial.printf("[el-image] poll HTTP %d status_snip=%.80s\n", pcode, pbody);
    if (pcode < 200 || pcode >= 300) {
      heap_caps_free(pbody);
      continue;
    }

    statusVal[0] = '\0';
    (void)extractJsonString(pbody, "status", statusVal, sizeof(statusVal));
    if (strcmp(statusVal, "completed") == 0) {
      if (!extractJsonString(pbody, "content_url", contentUrl,
                             sizeof(contentUrl))) {
        heap_caps_free(pbody);
        finish(false, nullptr, "no content_url");
        return;
      }
      heap_caps_free(pbody);
      terminal = true;
      break;
    }
    if (strcmp(statusVal, "failed") == 0) {
      if (!extractJsonString(pbody, "error_message", failMsg, sizeof(failMsg))) {
        snprintf(failMsg, sizeof(failMsg), "generation failed");
      }
      heap_caps_free(pbody);
      failMsg[sizeof(failMsg) - 1] = '\0';
      finish(false, nullptr, failMsg);
      return;
    }
    heap_caps_free(pbody);
  }

  if (!terminal || !contentUrl[0]) {
    finish(false, nullptr, "image timed out");
    return;
  }

  setStatus("Downloading image…");
  char absPath[96] = {};
  char err[96];
  if (!downloadUrlToFile(contentUrl, absPath, sizeof(absPath), err,
                         sizeof(err))) {
    finish(false, nullptr, err);
    return;
  }

  if (!imagePreviewLoad(storage_, absPath, kPreviewW, kPreviewH)) {
    Serial.printf("[el-image] saved but preview decode failed\n");
  } else {
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
  task_ = nullptr;
  if (runningEpoch_ != jobEpoch_) {
    busy_ = false;
  }
  portEXIT_CRITICAL(&mux_);
  vTaskDelete(nullptr);
}

void reclaimIfStuck() {
  portENTER_CRITICAL(&mux_);
  const bool stuck =
      busy_ && jobStartedMs_ != 0 && (millis() - jobStartedMs_ > kJobStuckMs);
  if (stuck) {
    jobEpoch_++;
    busy_ = false;
    resultReady_ = false;
    result_ = ElImageResult{};
    Serial.printf("[el-image] reclaim stuck job age=%lums\n",
                  (unsigned long)(millis() - jobStartedMs_));
    jobStartedMs_ = 0;
  }
  portEXIT_CRITICAL(&mux_);
}

} // namespace

bool elImageStart(const ElImageRequest &req, ElImageResult *earlyErr) {
  auto fail = [&](const char *msg) -> bool {
    if (earlyErr) {
      *earlyErr = ElImageResult{};
      earlyErr->ok = false;
      strncpy(earlyErr->error, msg, sizeof(earlyErr->error) - 1);
    }
    return false;
  };

  if (!req.apiKey || !req.apiKey[0]) return fail("no ElevenLabs API key");
  if (!req.prompt || !req.prompt[0]) return fail("missing prompt");
  if (!req.storage) return fail("no storage");

  reclaimIfStuck();

  for (int i = 0; i < 50; i++) {
    portENTER_CRITICAL(&mux_);
    const bool hasTask = task_ != nullptr;
    const bool isBusy = busy_;
    portEXIT_CRITICAL(&mux_);
    if (!hasTask && !isBusy) break;
    if (!hasTask && isBusy) {
      portENTER_CRITICAL(&mux_);
      busy_ = false;
      jobEpoch_++;
      portEXIT_CRITICAL(&mux_);
      Serial.printf("[el-image] cleared orphan busy\n");
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
  model_[0] = '\0';
  if (req.model && req.model[0]) {
    strncpy(model_, req.model, sizeof(model_) - 1);
    model_[sizeof(model_) - 1] = '\0';
  }
  storage_ = req.storage;
  attachReference_ = req.attachReference;
  refReqPath_[0] = '\0';
  if (req.referencePath && req.referencePath[0]) {
    strncpy(refReqPath_, req.referencePath, sizeof(refReqPath_) - 1);
    refReqPath_[sizeof(refReqPath_) - 1] = '\0';
  }

  const BaseType_t ok =
      xTaskCreatePinnedToCore(imageTask, "el_image", 16384, nullptr, 3, &task_,
                              0);
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

bool elImageBusy() {
  reclaimIfStuck();
  portENTER_CRITICAL(&mux_);
  const bool b = busy_ || task_ != nullptr;
  portEXIT_CRITICAL(&mux_);
  return b;
}

bool elImageTakeResult(ElImageResult *out) {
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

const char *elImageStatus() { return status_; }

uint32_t elImageStatusGen() { return statusGen_; }

void elImageReset() {
  portENTER_CRITICAL(&mux_);
  jobEpoch_++;
  busy_ = false;
  resultReady_ = false;
  result_ = ElImageResult{};
  jobStartedMs_ = 0;
  portEXIT_CRITICAL(&mux_);
  lastImagePath_[0] = '\0';
  refReqPath_[0] = '\0';
  setStatus("Idle");
  Serial.printf("[el-image] reset (epoch=%u)\n", (unsigned)jobEpoch_);
}
