#include "talk_agent.h"
#include "talk_aec.h"
#include "talk_audio.h"
#include "talk_config.h"
#include "talk_ulaw.h"
#include "net_wifi.h"
#include "runtime_config.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

#include <mbedtls/base64.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static WebSocketsClient ws;
static volatile bool agentActive = false;
static volatile bool agentReady = false;
static volatile bool wsTaskRun = false;
static TaskHandle_t wsTaskHandle = nullptr;
static int agentOutRate = TALK_SAMPLE_RATE;
static bool agentOutUlaw = false;

// Uplink format, taken from conversation_initiation_metadata rather than
// chosen here — see sendInit().
static int agentInRate = TALK_SAMPLE_RATE;
static bool agentInUlaw = false;

static char *txJsonBuf = nullptr;
static size_t txJsonCap = 0;
static uint8_t *pcmScratch = nullptr;
static size_t pcmScratchCap = 0;

static int16_t playScratch[TALK_I2S_BUF_SAMPLES];
static int16_t micTmp[TALK_I2S_BUF_SAMPLES];

static char signedHost[96];
static char signedPath[768];

static volatile bool pendingInit = false;
static volatile int pendingPongId = -1;
static volatile bool pendingPong = false;
static uint32_t audioChunksRx = 0;
static uint32_t readyAtMs = 0;
static uint32_t lastAgentAudioMs = 0;
static uint32_t lastNonSilentPlayMs = 0;
static volatile bool playPriming = true;
static uint32_t playUnderruns = 0;
static uint32_t playRingDrops = 0;
static uint32_t playStatMs = 0;

static const int kWaveBars = 24;
static uint8_t waveBars[kWaveBars];
static float playLevelEma = 0.0f;

static volatile bool audioTaskRun = false;
static TaskHandle_t audioTaskHandle = nullptr;

static void startAudioTask();
static void stopAudioTask();

static int16_t *micUplink = nullptr;
static size_t micUpCap = 0, micUpHead = 0, micUpTail = 0, micUpUsed = 0;
static portMUX_TYPE micUpMux = portMUX_INITIALIZER_UNLOCKED;

static char statusBuf[96] = "Idle";
static char lastUserBuf[256] = "";
static char lastReplyBuf[384] = "";

static void *psramOrRam(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = malloc(n);
  return p;
}

static void setStatus(const char *s) {
  strncpy(statusBuf, s ? s : "", sizeof(statusBuf) - 1);
  statusBuf[sizeof(statusBuf) - 1] = 0;
}

static void logMem(const char *tag) {
  // dma= is the number that matters: mbedTLS buffers live in PSRAM, so esp-aes
  // bounces each record through a DMA-capable internal buffer and dies here
  // first.
  Serial.printf("[talk %s] heap=%u contig=%u dma=%u psram_free=%u\n", tag,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void logf(const char *fmt, ...) {
  char msg[240];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  Serial.print(msg);
}

static bool ensurePcmScratch(size_t need) {
  if (pcmScratchCap >= need) return true;
  void *p = heap_caps_realloc(pcmScratch, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = realloc(pcmScratch, need);
  if (!p) return false;
  pcmScratch = (uint8_t *)p;
  pcmScratchCap = need;
  return true;
}

static int16_t *agentDecBuf = nullptr;
static size_t agentDecCap = 0;

static bool ensureAgentDec(size_t samples) {
  if (agentDecCap >= samples) return true;
  const size_t bytes = samples * sizeof(int16_t);
  void *p = heap_caps_realloc(agentDecBuf, bytes,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = realloc(agentDecBuf, bytes);
  if (!p) return false;
  agentDecBuf = (int16_t *)p;
  agentDecCap = samples;
  return true;
}

static bool ensureTxJson(size_t need) {
  if (txJsonCap >= need) return true;
  void *p = heap_caps_realloc(txJsonBuf, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = realloc(txJsonBuf, need);
  if (!p) return false;
  txJsonBuf = (char *)p;
  txJsonCap = need;
  return true;
}

static bool micUplinkInit() {
  if (micUplink) return true;
  micUpCap = TALK_MIC_UPLINK_SAMPLES;
  micUplink = (int16_t *)psramOrRam(micUpCap * sizeof(int16_t));
  if (!micUplink) {
    micUpCap = 0;
    return false;
  }
  micUpHead = micUpTail = micUpUsed = 0;
  return true;
}

/** Backpressure: stop accepting when full instead of overwriting speech. */
static size_t micUplinkPush(const int16_t *data, size_t count) {
  size_t pushed = 0;
  portENTER_CRITICAL(&micUpMux);
  while (pushed < count && micUpUsed < micUpCap) {
    micUplink[micUpHead] = data[pushed++];
    micUpHead = (micUpHead + 1) % micUpCap;
    micUpUsed++;
  }
  portEXIT_CRITICAL(&micUpMux);
  return pushed;
}

static size_t micUplinkPop(int16_t *out, size_t maxCount) {
  size_t popped = 0;
  portENTER_CRITICAL(&micUpMux);
  while (popped < maxCount && micUpUsed > 0) {
    out[popped++] = micUplink[micUpTail];
    micUpTail = (micUpTail + 1) % micUpCap;
    micUpUsed--;
  }
  portEXIT_CRITICAL(&micUpMux);
  return popped;
}

static size_t micUplinkUsed() {
  portENTER_CRITICAL(&micUpMux);
  size_t n = micUpUsed;
  portEXIT_CRITICAL(&micUpMux);
  return n;
}

static void micUplinkClear() {
  portENTER_CRITICAL(&micUpMux);
  micUpHead = micUpTail = micUpUsed = 0;
  portEXIT_CRITICAL(&micUpMux);
}

static bool allocSessionBuffers() {
  if (!ensureTxJson(16 * 1024)) return false;
  // Sized off the largest frame actually seen on the wire, 489593 bytes of
  // base64 needing ~367 KB decoded — the old 320 KB fell short and paid for a
  // realloc mid-reply, which lands straight in the playback gap. ulaw_8000
  // output quarters this, but the pcm_16000 path has to stay safe.
  if (!ensurePcmScratch(512 * 1024)) return false;
  if (!micUplinkInit()) return false;
  return true;
}

static bool parseWssUrl(const char *url, char *host, size_t hostLen, char *path,
                        size_t pathLen) {
  if (strncmp(url, "wss://", 6) != 0 && strncmp(url, "https://", 8) != 0) {
    return false;
  }
  const char *p = strstr(url, "://");
  if (!p) return false;
  p += 3;
  const char *slash = strchr(p, '/');
  if (!slash) return false;
  size_t hlen = (size_t)(slash - p);
  if (hlen + 1 > hostLen) return false;
  memcpy(host, p, hlen);
  host[hlen] = 0;
  strncpy(path, slash, pathLen - 1);
  path[pathLen - 1] = 0;
  return true;
}

static bool fetchSignedUrl(String &signedUrl) {
  const char *agentId = getConfig(Config::ElevenlabsAgentId);
  const char *apiKey = getConfig(Config::ElevenlabsApiKey);
  if (!apiKey || !apiKey[0] || !agentId || !agentId[0]) {
    logf("ElevenLabs config missing (Remote or defaults)\n");
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url =
      String("https://api.elevenlabs.io/v1/convai/conversation/get-signed-url?agent_id=") +
      agentId;
  if (!http.begin(client, url)) return false;
  http.addHeader("xi-api-key", apiKey);
  int code = http.GET();
  if (code != 200) {
    logf("signed-url HTTP %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  const char *su = doc["signed_url"];
  if (!su || !su[0]) return false;
  signedUrl = su;
  return true;
}

static uint32_t lastSendOkMs = 0;
static uint32_t lastSlowSendLogMs = 0;

static void sendJson(const char *s) {
  if (!ws.isConnected()) return;
  const uint32_t t0 = millis();
  const bool ok = ws.sendTXT(s);
  const uint32_t elapsed = millis() - t0;
  if (ok) {
    // A send into a send window with room returns in single-digit ms, so the
    // gate is far above the noise floor and a healthy session never logs. Once
    // the window is full, send_ssl_data() spins on MBEDTLS_ERR_SSL_WANT_WRITE
    // at vTaskDelay(2) until socket_timeout, so elapsed climbing is the first
    // warning that congestion is back.
    if (elapsed > 250 && (millis() - lastSlowSendLogMs) > 2000) {
      lastSlowSendLogMs = millis();
      logf("WS send slow %lums len=%u rssi=%d\n", (unsigned long)elapsed,
           (unsigned)strlen(s), WiFi.RSSI());
    }
    lastSendOkMs = millis();
    return;
  }
  // still_conn=0 means the peer went away mid-send, so the send is a symptom
  // rather than the cause; =1 means sendFrame itself refused or timed out.
  // ms= is the discriminator between the two write failures that both end in
  // WiFiClientSecure::stop(): near WEBSOCKETS_TCP_TIMEOUT is the write-timeout
  // path, near zero alongside an [E] send_ssl_data() line is a peer reset.
  logf("WS send failed len=%u ms=%lu since_ok=%lums still_conn=%d rssi=%d\n",
       (unsigned)strlen(s), (unsigned long)elapsed,
       (unsigned long)(lastSendOkMs ? millis() - lastSendOkMs : 0),
       ws.isConnected() ? 1 : 0, WiFi.RSSI());
}

static void sendInit() {
  // No uplink format is requested here on purpose. user_input_audio_format
  // lives in the agent's conversation_config.asr, and the client-side
  // conversation_config_override schema only exposes agent/tts/conversation —
  // an unrecognised override key is answered with override_error and a 1008
  // close. So the format is negotiated: whatever comes back in
  // conversation_initiation_metadata picks the encoder.
  sendJson("{\"type\":\"conversation_initiation_client_data\"}");
  logf("sent initiation\n");
}

static void sendPong(int eventId) {
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"type\":\"pong\",\"event_id\":%d}", eventId);
  sendJson(buf);
}

static int16_t *micSendBuf = nullptr;
static uint8_t micUlawBuf[TALK_MIC_ULAW_CHUNK_BYTES];

// Uplink continuity accounting. ElevenLabs warns about "audio duration
// mismatch" when the stream it receives is shorter than wall clock, so these
// track every place a mic sample can go missing.
static uint32_t micCapturedSamples = 0;
static uint32_t micSentSamples = 0;
static uint32_t micDroppedSamples = 0;
static uint32_t micReadGaps = 0;
static uint32_t lastUplinkStatMs = 0;

/**
 * The ring and the wire can run at different rates, so keep one meaning per
 * counter: TALK_MIC_CHUNK_SAMPLES and micSentSamples are both in 16 kHz ring
 * samples, which is what keeps `sent` in the uplink stat line comparable to
 * wall clock whichever encoder is active. Only bodyBytes is in wire units.
 */
static bool sendOneMicChunk() {
  if (micUplinkUsed() < (size_t)TALK_MIC_CHUNK_SAMPLES) return false;

  size_t got = micUplinkPop(micSendBuf, TALK_MIC_CHUNK_SAMPLES);
  if (got < (size_t)TALK_MIC_CHUNK_SAMPLES) return false;

  const uint8_t *body;
  size_t bodyBytes;
  if (agentInUlaw) {
    body = micUlawBuf;
    bodyBytes = talkUlawEncodeFrom16k(micSendBuf, got, micUlawBuf);
  } else {
    body = (const uint8_t *)micSendBuf;
    bodyBytes = got * sizeof(int16_t);
  }

  size_t b64Need = 0;
  mbedtls_base64_encode(nullptr, 0, &b64Need, body, bodyBytes);
  if (!ensureTxJson(b64Need + 40)) return false;

  const char *prefix = "{\"user_audio_chunk\":\"";
  size_t prefixLen = strlen(prefix);
  memcpy(txJsonBuf, prefix, prefixLen);
  size_t written = 0;
  if (mbedtls_base64_encode((unsigned char *)txJsonBuf + prefixLen,
                            txJsonCap - prefixLen - 4, &written, body,
                            bodyBytes) != 0) {
    return false;
  }
  memcpy(txJsonBuf + prefixLen + written, "\"}", 3);
  sendJson(txJsonBuf);
  micSentSamples += got;
  return true;
}

static void sendMicChunkFromUplink() {
  if (!agentReady) return;
  if (audioChunksRx == 0 && (millis() - readyAtMs) < 1200) return;
  if (!micSendBuf) {
    micSendBuf = (int16_t *)psramOrRam(TALK_MIC_CHUNK_SAMPLES * sizeof(int16_t));
    if (!micSendBuf) return;
  }

  // Drain backlog rather than one chunk per tick: ws.loop() can block for
  // seconds on a large agent frame, and at one 32 ms chunk per iteration the
  // uplink would never catch up afterwards — the ring fills and pushes get
  // dropped, which is what ElevenLabs sees as a duration mismatch.
  //
  // Bounded by wall time, not just count. LWIP's send buffer holds four
  // segments, so chaining sixteen blocking send_ssl_data() calls keeps the
  // window pinned and turns one lost segment into a socket_timeout that
  // WiFiClientSecure answers by closing the connection outright. A healthy
  // send returns in single-digit ms, so this budget only bites once the
  // socket is already congested — which is when backing off is the point.
  const uint32_t budgetStart = millis();
  for (int i = 0; i < 16; i++) {
    if (!sendOneMicChunk()) return;
    if (millis() - budgetStart > 60) return;
  }
}

static void pushAgentSamples(const int16_t *src, size_t nSrc, int srcRate) {
  if (!nSrc) return;
  if (srcRate != TALK_SAMPLE_RATE && srcRate > 0) {
    size_t nDst =
        (size_t)((uint64_t)nSrc * (uint64_t)TALK_SAMPLE_RATE / (uint64_t)srcRate);
    if (!nDst) return;
    int16_t *dst = (int16_t *)psramOrRam(nDst * sizeof(int16_t));
    if (!dst) {
      talkPlayRingPush(src, nSrc);
      return;
    }
    for (size_t i = 0; i < nDst; i++) {
      float pos = (float)i * (float)srcRate / (float)TALK_SAMPLE_RATE;
      size_t i0 = (size_t)pos;
      size_t i1 = (i0 + 1 < nSrc) ? i0 + 1 : nSrc - 1;
      if (i0 >= nSrc) i0 = nSrc - 1;
      float t = pos - (float)i0;
      dst[i] = (int16_t)((1.0f - t) * (float)src[i0] + t * (float)src[i1]);
    }
    size_t pushed = talkPlayRingPush(dst, nDst);
    free(dst);
    if (pushed < nDst) {
      playRingDrops += (uint32_t)(nDst - pushed);
      logf("play ring drop %u\n", (unsigned)(nDst - pushed));
    }
    return;
  }
  size_t pushed = talkPlayRingPush(src, nSrc);
  if (pushed < nSrc) {
    playRingDrops += (uint32_t)(nSrc - pushed);
    logf("play ring drop %u\n", (unsigned)(nSrc - pushed));
  }
}

static void pushAgentPcmBytes(const uint8_t *data, size_t nbytes, int srcRate) {
  if (agentOutUlaw) {
    // One byte per sample, so expand before anything else looks at it. Worth
    // the extra buffer: ulaw_8000 is a quarter the bytes on the wire, which is
    // what stops a single reply arriving as multi-hundred-KB frames.
    if (!nbytes) return;
    if (!ensureAgentDec(nbytes)) {
      logf("ulaw scratch OOM %u\n", (unsigned)nbytes);
      return;
    }
    talkUlawDecode(data, nbytes, agentDecBuf);
    pushAgentSamples(agentDecBuf, nbytes, srcRate);
    return;
  }
  if (nbytes < 2) return;
  pushAgentSamples((const int16_t *)data, nbytes / 2, srcRate);
}

static int parseRate(const char *fmt) {
  if (!fmt) return TALK_SAMPLE_RATE;
  if (strstr(fmt, "48000")) return 48000;
  if (strstr(fmt, "44100")) return 44100;
  if (strstr(fmt, "24000")) return 24000;
  if (strstr(fmt, "22050")) return 22050;
  if (strstr(fmt, "16000")) return 16000;
  if (strstr(fmt, "8000")) return 8000;
  return TALK_SAMPLE_RATE;
}

static void handleAudioInPlace(char *payload, size_t length) {
  // Every bail-out below discards a whole chunk — up to a second of speech —
  // so none of them may be silent. A truncated frame lands on the unterminated
  // case, which is otherwise indistinguishable from audio that never arrived.
  char *key = strstr(payload, "audio_base_64");
  if (!key) {
    logf("audio parse: no key (len=%u)\n", (unsigned)length);
    return;
  }
  char *q1 = strchr(key + 13, '"');
  if (!q1) {
    logf("audio parse: no colon quote (len=%u)\n", (unsigned)length);
    return;
  }
  q1 = strchr(q1 + 1, '"');
  if (!q1) {
    logf("audio parse: no open quote (len=%u)\n", (unsigned)length);
    return;
  }
  q1++;
  char *q2 = strchr(q1, '"');
  if (!q2) {
    logf("audio parse: unterminated b64, frame truncated? (len=%u)\n",
         (unsigned)length);
    return;
  }

  size_t b64Len = (size_t)(q2 - q1);
  char saved = *q2;
  *q2 = 0;

  size_t need = (b64Len * 3) / 4 + 16;
  if (!ensurePcmScratch(need)) {
    *q2 = saved;
    logf("pcm scratch OOM %u\n", (unsigned)need);
    return;
  }

  const uint32_t decodeStart = millis();
  size_t outLen = 0;
  int rc = mbedtls_base64_decode(pcmScratch, pcmScratchCap, &outLen,
                                 (const unsigned char *)q1, b64Len);
  *q2 = saved;
  if (rc != 0) {
    logf("audio b64 fail rc=%d len=%u\n", rc, (unsigned)b64Len);
    return;
  }
  const uint32_t decodeMs = millis() - decodeStart;

  audioChunksRx++;
  const uint32_t prevAudioMs = lastAgentAudioMs;
  lastAgentAudioMs = millis();
  // gap= is time since the previous audio frame landed and dec= is decode
  // cost, which together say whether a playback stall is network or CPU.
  const uint32_t gapMs = prevAudioMs ? (lastAgentAudioMs - prevAudioMs) : 0;
  const size_t ringBefore = talkPlayRingUsed();
  if (audioChunksRx <= 5 || (audioChunksRx % 25) == 0 || ringBefore == 0) {
    logf("audio #%lu bytes=%u ring=%u gap=%lums dec=%lums under=%lu\n",
         (unsigned long)audioChunksRx, (unsigned)outLen, (unsigned)ringBefore,
         (unsigned long)gapMs, (unsigned long)decodeMs,
         (unsigned long)playUnderruns);
  }
  pushAgentPcmBytes(pcmScratch, outLen, agentOutRate);
}

static void copyTrunc(char *dst, size_t dstLen, const char *src) {
  if (!dst || dstLen == 0) return;
  if (!src) {
    dst[0] = 0;
    return;
  }
  strncpy(dst, src, dstLen - 1);
  dst[dstLen - 1] = 0;
}

static void handleWsMessage(uint8_t *payload, size_t length) {
  payload[length] = 0;
  char *msg = (char *)payload;

  if (strstr(msg, "\"type\":\"audio\"") || strstr(msg, "\"type\": \"audio\"")) {
    handleAudioInPlace(msg, length);
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, msg, length)) {
    logf("json fail (%u) %.40s\n", (unsigned)length, msg);
    return;
  }
  const char *type = doc["type"] | "";
  if (!type[0]) return;

  if (!strcmp(type, "conversation_initiation_metadata")) {
    JsonObject ev = doc["conversation_initiation_metadata_event"];
    const char *outFmt = ev["agent_output_audio_format"] | "pcm_16000";
    const char *inFmt = ev["user_input_audio_format"] | "pcm_16000";
    agentOutRate = parseRate(outFmt);
    agentOutUlaw = strstr(outFmt, "ulaw") != nullptr;
    agentInRate = parseRate(inFmt);
    agentInUlaw = strstr(inFmt, "ulaw") != nullptr && agentInRate == 8000;
    if (!agentInUlaw && agentInRate != TALK_SAMPLE_RATE) {
      // Nothing better to do than keep the pcm_16000 path: a format we cannot
      // produce would be garbage either way, and the mismatch is visible here.
      logf("uplink fmt %s unsupported, sending pcm_16000\n", inFmt);
      agentInRate = TALK_SAMPLE_RATE;
    }
    talkUlawReset();
    agentReady = true;
    readyAtMs = millis();
    setStatus("Listening");
    logf("agent ready out=%s -> %s %dHz / uplink %s -> %s %dHz\n", outFmt,
         agentOutUlaw ? "ulaw" : "pcm16", agentOutRate, inFmt,
         agentInUlaw ? "ulaw" : "pcm16", agentInRate);
    logMem("ready");
  } else if (!strcmp(type, "ping")) {
    pendingPongId = doc["ping_event"]["event_id"] | 0;
    pendingPong = true;
  } else if (!strcmp(type, "user_transcript")) {
    const char *t = doc["user_transcription_event"]["user_transcript"] | "";
    copyTrunc(lastUserBuf, sizeof(lastUserBuf), t);
    logf("you: %s\n", t);
  } else if (!strcmp(type, "agent_response")) {
    const char *t = doc["agent_response_event"]["agent_response"] | "";
    copyTrunc(lastReplyBuf, sizeof(lastReplyBuf), t);
    setStatus("Speaking");
    logf("agent: %s\n", t);
  } else if (!strcmp(type, "interruption")) {
    // How much buffered speech this throws away is the whole question when the
    // interruption was spurious — the prime buffer means it is never zero.
    const uint32_t lostMs =
        (uint32_t)(talkPlayRingUsed() * 1000 / TALK_SAMPLE_RATE);
    talkPlayRingClear();
    playPriming = true;
    lastAgentAudioMs = 0;
    lastNonSilentPlayMs = 0;
    setStatus("Listening");
    logf("(interrupted) discarded=%lums\n", (unsigned long)lostMs);
  } else if (!strcmp(type, "error")) {
    setStatus("Error");
    logf("EL error: %.160s\n", msg);
  } else {
    logf("el: %s (%u)\n", type, (unsigned)length);
  }
}

static void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
  case WStype_CONNECTED:
    logf("WS connected\n");
    logMem("ws-on");
    agentReady = false;
    audioChunksRx = 0;
    lastNonSilentPlayMs = 0;
    playPriming = true;
    playUnderruns = 0;
    playRingDrops = 0;
    playStatMs = 0;
    talkAudioResetWriteStats();
    agentInUlaw = false;
    agentInRate = TALK_SAMPLE_RATE;
    agentOutUlaw = false;
    agentOutRate = TALK_SAMPLE_RATE;
    micUplinkClear();
    talkUlawReset();
    pendingInit = true;
    setStatus("Connected");
    break;
  case WStype_TEXT:
    handleWsMessage(payload, length);
    break;
  case WStype_DISCONNECTED:
    if (payload && length) {
      logf("WS disconnected: %.*s\n", (int)length, (const char *)payload);
    } else {
      logf("WS disconnected\n");
    }
    logMem("ws-off");
    agentReady = false;
    agentActive = false;
    setStatus("Disconnected");
    break;
  case WStype_ERROR:
    logf("WS error len=%u\n", (unsigned)length);
    setStatus("WS error");
    break;
  default:
    break;
  }
}

static void wsTask(void *arg) {
  (void)arg;
  logf("ws task on core %d\n", xPortGetCoreID());
  while (wsTaskRun) {
    if (agentActive) {
      ws.loop();
      if (pendingInit) {
        pendingInit = false;
        sendInit();
      }
      if (pendingPong) {
        pendingPong = false;
        sendPong(pendingPongId);
      }
      sendMicChunkFromUplink();

      // cap/sent are the two numbers ElevenLabs is really comparing: if sent
      // trails wall clock the stream has holes. backlog rising means the drain
      // is too slow, drop>0 means the ring overflowed, gaps>0 means the I2S
      // read itself came up empty. rssi is the baseline to read backlog
      // against: a rising backlog at -75 dBm is the radio, not the encoder.
      if (agentReady && millis() - lastUplinkStatMs > 5000) {
        lastUplinkStatMs = millis();
        logf("uplink cap=%lus sent=%lus backlog=%u drop=%lu gaps=%lu rssi=%d\n",
             (unsigned long)(micCapturedSamples / TALK_SAMPLE_RATE),
             (unsigned long)(micSentSamples / TALK_SAMPLE_RATE),
             (unsigned)micUplinkUsed(), (unsigned long)micDroppedSamples,
             (unsigned long)micReadGaps, WiFi.RSSI());
      }
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  wsTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

static void startWsTask() {
  if (wsTaskHandle) return;
  wsTaskRun = true;
  xTaskCreatePinnedToCore(wsTask, "el_ws", 8192, nullptr, 5, &wsTaskHandle, 0);
}

static void stopWsTask() {
  wsTaskRun = false;
  for (int i = 0; i < 50 && wsTaskHandle; i++) delay(10);
}

bool talkAgentStart(AppHost *host) {
  (void)host;
  if (agentActive) return true;

  setStatus("Wi-Fi...");
  if (WiFi.status() != WL_CONNECTED) {
    if (!connectWifi()) {
      setStatus("Wi-Fi failed");
      return false;
    }
  }
  WiFi.setSleep(false);

  logMem("boot");
  configInit();
  if (!getConfig(Config::ElevenlabsApiKey)[0] ||
      !getConfig(Config::ElevenlabsAgentId)[0]) {
    setStatus("No EL config");
    logf("ElevenLabs config missing\n");
    return false;
  }

  if (!talkAudioInit()) {
    setStatus("Audio init fail");
    logf("audio init failed\n");
    return false;
  }
  if (!allocSessionBuffers()) {
    setStatus("OOM buffers");
    logf("session buffer alloc failed\n");
    return false;
  }

  lastUserBuf[0] = 0;
  lastReplyBuf[0] = 0;

  String signedUrl;
  setStatus("Signed URL...");
  logf("fetching signed URL...\n");
  if (!fetchSignedUrl(signedUrl)) {
    setStatus("Signed URL fail");
    return false;
  }
  if (!parseWssUrl(signedUrl.c_str(), signedHost, sizeof(signedHost), signedPath,
                   sizeof(signedPath))) {
    setStatus("Bad signed URL");
    logf("bad signed url\n");
    return false;
  }
  logf("WS host=%s path_len=%u\n", signedHost, (unsigned)strlen(signedPath));

  talkPlayRingClear();
  talkAudioResetDsp();
  talkAecReset();
  talkUlawReset();
  micUplinkClear();
  agentInRate = TALK_SAMPLE_RATE;
  agentInUlaw = false;
  agentOutUlaw = false;
  agentOutRate = TALK_SAMPLE_RATE;
  micCapturedSamples = 0;
  micSentSamples = 0;
  micDroppedSamples = 0;
  micReadGaps = 0;
  lastUplinkStatMs = 0;
  pendingInit = false;
  pendingPong = false;
  lastAgentAudioMs = 0;
  lastNonSilentPlayMs = 0;
  playPriming = true;
  playUnderruns = 0;
  playRingDrops = 0;
  playStatMs = 0;
  talkAudioResetWriteStats();
  lastSendOkMs = 0;
  lastSlowSendLogMs = 0;
  playLevelEma = 0;
  memset(waveBars, 0, sizeof(waveBars));

  if (!talkAudioStartDuplex()) {
    setStatus("I2S duplex fail");
    logf("duplex I2S failed\n");
    return false;
  }

  logMem("pre-ws");
  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(0);
  ws.beginSSL(signedHost, 443, signedPath);

  agentActive = true;
  agentReady = false;
  startWsTask();
  startAudioTask();
  setStatus("Connecting...");
  logf("talk started\n");
  logMem("talk");
  return true;
}

void talkAgentStop() {
  pendingInit = false;
  pendingPong = false;
  stopAudioTask();
  stopWsTask();
  if (ws.isConnected()) ws.disconnect();
  agentActive = false;
  agentReady = false;
  talkAudioStop();
  setStatus("Idle");
  logf("talk stopped\n");
}

bool talkAgentIsActive() { return agentActive; }
bool talkAgentIsReady() { return agentReady; }
const char *talkAgentStatus() { return statusBuf; }
const char *talkAgentLastUser() { return lastUserBuf; }
const char *talkAgentLastReply() { return lastReplyBuf; }

static bool agentIsSpeaking() {
  // Any queued TTS, recent network audio, or recent non-silent DAC output.
  if (talkPlayRingUsed() > 0) return true;
  const uint32_t now = millis();
  if (lastAgentAudioMs != 0 &&
      (now - lastAgentAudioMs) < TALK_AGENT_SPEAKING_TAIL_MS) {
    return true;
  }
  if (lastNonSilentPlayMs != 0 &&
      (now - lastNonSilentPlayMs) < TALK_AGENT_SPEAKING_TAIL_MS) {
    return true;
  }
  return false;
}

bool talkAgentIsSpeaking() { return agentActive && agentIsSpeaking(); }

float talkAgentPlayLevel() { return playLevelEma; }

int talkAgentWaveBars(uint8_t *out, int maxBars) {
  if (!out || maxBars <= 0) return 0;
  int n = maxBars < kWaveBars ? maxBars : kWaveBars;
  memcpy(out, waveBars, (size_t)n);
  return n;
}

static int16_t blockPeakAbs(const int16_t *data, int n) {
  int16_t peak = 0;
  for (int i = 0; i < n; i++) {
    int16_t a = data[i] < 0 ? (int16_t)(-data[i]) : data[i];
    if (a > peak) peak = a;
  }
  return peak;
}

static void updateWaveBars(const int16_t *mic, int nMic, const int16_t *play,
                           int nPlay, int16_t playPeak) {
  float lvl = (float)playPeak / 22000.0f;
  if (lvl > 1.0f) lvl = 1.0f;
  if (lvl < 0.0f) lvl = 0.0f;
  playLevelEma = 0.72f * playLevelEma + 0.28f * lvl;

  const int n = (nMic > nPlay) ? nMic : nPlay;
  if (n <= 0) {
    for (int b = 0; b < kWaveBars; b++) {
      waveBars[b] = (uint8_t)((waveBars[b] * 5) / 6);
    }
    return;
  }

  for (int b = 0; b < kWaveBars; b++) {
    int start = (b * n) / kWaveBars;
    int end = ((b + 1) * n) / kWaveBars;
    if (end <= start) end = start + 1;
    int16_t pk = 0;
    for (int i = start; i < end; i++) {
      if (mic && i < nMic) {
        int16_t a = mic[i] < 0 ? (int16_t)(-mic[i]) : mic[i];
        if (a > pk) pk = a;
      }
      if (play && i < nPlay) {
        int16_t a = play[i] < 0 ? (int16_t)(-play[i]) : play[i];
        if (a > pk) pk = a;
      }
    }
    // Mic is quieter than TTS — boost mic contribution a bit for the UI.
    int target = (int)pk * 255 / 14000;
    if (target > 255) target = 255;
    waveBars[b] = (uint8_t)((waveBars[b] * 2 + target) / 3);
  }
}

// Real-time duplex pump — must not share the UI thread (TFT stalls = audio stutter).
static void talkAgentPumpAudio() {
  if (!agentActive) return;

  int n = talkAudioReadPcmTimeout(micTmp, TALK_I2S_BUF_SAMPLES, 20);
  if (n <= 0) {
    micReadGaps++;
    return;
  }

  // While priming, playScratch stays silent and the ring keeps filling. The
  // silence still goes to I2S and to the AEC reference, so the echo estimate
  // continues to see exactly what the speaker emits.
  size_t have = 0;
  if (playPriming) {
    const size_t queued = talkPlayRingUsed();
    const uint32_t lastRx = lastAgentAudioMs;
    const bool primed = queued >= (size_t)TALK_PLAY_PRIME_SAMPLES;
    const bool streamIdle =
        lastRx != 0 && (millis() - lastRx) >= TALK_PLAY_PRIME_FLUSH_MS;
    if (queued > 0 && (primed || streamIdle)) playPriming = false;
  }
  if (!playPriming) {
    have = talkPlayRingPop(playScratch, (size_t)n);
    // Drained: either the turn ended or delivery fell behind. Re-arm so the
    // next burst refills before it reaches the DAC.
    if (have == 0) {
      playPriming = true;
      playUnderruns++;
    }
  }
  if (have < (size_t)n) {
    memset(playScratch + have, 0, ((size_t)n - have) * sizeof(int16_t));
  }
  const int16_t playPeak = blockPeakAbs(playScratch, n);
  talkAudioWritePcmTimeout(playScratch, n, 20);
  talkAecPushReference(playScratch, n);
  updateWaveBars(micTmp, n, playScratch, n, playPeak);

  const uint32_t now = millis();
  if (playPeak > TALK_PLAY_SILENCE_PEAK) {
    lastNonSilentPlayMs = now;
  }

  if (!agentReady) return;

  // Playback health, from the audio task. wrdrop is the decisive field: those
  // samples were popped off the ring and then never reached the DAC, so they
  // are a hole in the audio that no other counter sees. under/ovf separate a
  // starved ring from an overflowing one.
  if (now - playStatMs > 5000) {
    playStatMs = now;
    logf("play ring=%ums under=%lu ovf=%lu wrdrop=%lu wrstall=%lu wrmax=%lums\n",
         (unsigned)(talkPlayRingUsed() * 1000 / TALK_SAMPLE_RATE),
         (unsigned long)playUnderruns, (unsigned long)playRingDrops,
         (unsigned long)talkAudioWriteDropped(),
         (unsigned long)talkAudioWriteStalls(),
         (unsigned long)talkAudioWriteMaxMs());
  }

  if (agentIsSpeaking()) {
    if (strcmp(statusBuf, "Listening") == 0) {
      setStatus("Speaking");
    }
  } else if (strcmp(statusBuf, "Speaking") == 0) {
    setStatus("Listening");
  }

  // The mic stays open through agent speech: ElevenLabs wants one continuous
  // stream and closes the socket after 60 s without user audio. talkAecProcess
  // strips the speaker bleed so the agent does not hear itself, while leaving
  // enough through during double talk to allow barge-in.
  talkAecProcess(micTmp, (size_t)n);
  const size_t pushed = micUplinkPush(micTmp, (size_t)n);
  micCapturedSamples += (uint32_t)n;
  if (pushed < (size_t)n) micDroppedSamples += (uint32_t)((size_t)n - pushed);
}

static void talkAudioTask(void *arg) {
  (void)arg;
  logf("audio task on core %d\n", xPortGetCoreID());
  while (audioTaskRun) {
    if (agentActive) {
      talkAgentPumpAudio();
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  audioTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

static void startAudioTask() {
  if (audioTaskHandle) return;
  audioTaskRun = true;
  xTaskCreatePinnedToCore(talkAudioTask, "el_audio", 4096, nullptr, 20,
                          &audioTaskHandle, 1);
}

static void stopAudioTask() {
  audioTaskRun = false;
  for (int i = 0; i < 50 && audioTaskHandle; i++) delay(10);
}

void talkAgentLoop() {
  // Audio runs on el_audio task. UI thread only needs status/wave samples.
}
