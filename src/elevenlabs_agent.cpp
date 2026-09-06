#include "elevenlabs_agent.h"
#include "talk_aec.h"
#include "talk_audio.h"
#include "talk_config.h"
#include "voice_context.h"
#include "voice_image_tool.h"
#include "voice_tools.h"
#include "settings_app.h"
#include "talk_ulaw.h"
#include "net_wifi.h"
#include "runtime_config.h"

#include <Arduino.h>
#include <Flow32.h>
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

// Agent chosen for this session. Empty means fall back to the configured id.
static char activeAgentId[48];

static AppHost *sessionHost = nullptr;
static VoiceContext talkCtx{};
static bool contextDirty = false;
/** True if the last initiation included conversation_context. */
static bool lastInitHadContext = false;

static volatile bool pendingInit = false;
static volatile int pendingPongId = -1;
static volatile bool pendingPong = false;
/**
 * Agent invoked system tool `end_call`. Mirror the ElevenLabs client SDK:
 * hang up after farewell audio drains (UI thread via elAgentLoop — never from
 * the ws task, or stopWsTask deadlocks waiting on itself).
 */
static volatile bool pendingEndCall = false;
static volatile bool endedByAgent = false;
static uint32_t endCallAtMs = 0;
static uint32_t audioChunksRx = 0;
static uint32_t readyAtMs = 0;
static uint32_t lastAgentAudioMs = 0;
static uint32_t lastNonSilentPlayMs = 0;
static volatile bool playPriming = true;
static uint32_t playUnderruns = 0;
static uint32_t playRingDrops = 0;
static uint32_t playStatMs = 0;

/**
 * Select "I want to talk": mute agent audio locally until ElevenLabs confirms
 * barge-in with `interruption` (or the agent stream goes idle). I2S stays up.
 * This is the SDK pattern — interrupt is server-driven; the client only stops
 * playback. `user_activity` is the wrong tool (typing keepalive, not barge-in).
 */
static volatile bool holdListening = false;
static uint32_t holdStartedMs = 0;
static uint32_t lastHoldDropMs = 0;
/**
 * ElevenLabs SDK pattern: after an interruption, ignore audio chunks whose
 * event_id is still from the interrupted turn.
 */
static int lastInterruptEventId = 0;

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
  const char *agentId =
      activeAgentId[0] ? activeAgentId : getConfig(Config::ElevenlabsAgentId);
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

static const char *sessionAgentId() {
  if (activeAgentId[0]) return activeAgentId;
  return getConfig(Config::ElevenlabsAgentId);
}

static Storage *sessionStorage() {
  return sessionHost ? sessionHost->storage() : nullptr;
}

static void persistContext() {
  if (!contextDirty) return;
  Storage *st = sessionStorage();
  const char *aid = sessionAgentId();
  if (!st || !st->ready() || !aid || !aid[0]) return;
  talkCtx.updatedMs = millis();
  if (voiceContextSave(st, aid, talkCtx)) {
    contextDirty = false;
    logf("context saved id=%s len=%u\n", talkCtx.conversationId,
         (unsigned)talkCtx.len);
  } else {
    logf("context save failed\n");
  }
}

/** Append JSON-escaped bytes into dst; returns new length or (size_t)-1. */
static size_t jsonEscapeAppend(char *dst, size_t cap, size_t at, const char *src,
                               size_t n) {
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)src[i];
    const char *esc = nullptr;
    char u[7];
    size_t elen = 0;
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
      if (c < 0x20) {
        snprintf(u, sizeof(u), "\\u%04x", c);
        esc = u;
        elen = 6;
      } else {
        if (at + 1 >= cap) return (size_t)-1;
        dst[at++] = (char)c;
        continue;
      }
      break;
    }
    if (at + elen >= cap) return (size_t)-1;
    memcpy(dst + at, esc, elen);
    at += elen;
  }
  return at;
}

static bool appendLit(char *dst, size_t cap, size_t *at, const char *s) {
  const size_t n = strlen(s);
  if (*at + n >= cap) return false;
  memcpy(dst + *at, s, n);
  *at += n;
  dst[*at] = '\0';
  return true;
}

/**
 * Events this client needs. Agent configs can omit `agent_tool_response`,
 * which is the official hang-up signal for system tool `end_call` (see
 * ElevenLabs SDK BaseConversation.handleAgentToolResponse). Override on
 * initiation so we always receive it — farewell TTS alone does not end the WS.
 */
static constexpr const char kInitClientEventsJson[] =
    "\"conversation_config_override\":{"
    "\"conversation\":{"
    "\"client_events\":["
    "\"audio\","
    "\"agent_response\","
    "\"agent_response_correction\","
    "\"interruption\","
    "\"user_transcript\","
    "\"conversation_initiation_metadata\","
    "\"client_tool_call\","
    "\"agent_tool_request\","
    "\"agent_tool_response\","
    "\"ping\","
    "\"guardrail_triggered\""
    "]}}";

static constexpr const char kInitFreshJson[] =
    "{\"type\":\"conversation_initiation_client_data\","
    "\"conversation_config_override\":{"
    "\"conversation\":{"
    "\"client_events\":["
    "\"audio\","
    "\"agent_response\","
    "\"agent_response_correction\","
    "\"interruption\","
    "\"user_transcript\","
    "\"conversation_initiation_metadata\","
    "\"client_tool_call\","
    "\"agent_tool_request\","
    "\"agent_tool_response\","
    "\"ping\","
    "\"guardrail_triggered\""
    "]}}}";

static void scheduleEndCall(const char *why) {
  if (pendingEndCall) return;
  pendingEndCall = true;
  endCallAtMs = millis();
  micUplinkClear();
  setStatus("Ending");
  logf("%s — draining playback then hang up\n", why ? why : "end");
}

static void sendInit() {
  // Continuity: each WS session is a *new* conversation. Reinject transcript
  // text only via dynamic_variables. Never send previous_conversation_id.
  const char *full = (talkCtx.text && talkCtx.len) ? talkCtx.text : "";
  size_t fullLen = talkCtx.len;
  const char *text = full;
  size_t textLen = fullLen;

  constexpr size_t kMaxInitContext = 1800;
  if (textLen > kMaxInitContext) {
    size_t off = textLen - kMaxInitContext;
    while (off < textLen && full[off] != '\n') off++;
    if (off < textLen && full[off] == '\n') off++;
    text = full + off;
    textLen = textLen - off;
  }

  // Prefer a fixed flash-resident initiation for cold start — building the
  // same JSON in PSRAM (and adding user_id) correlated with immediate peer
  // closes before agent-ready. Always request agent_tool_response so end_call
  // is not dropped when the agent config's client_events list omits it.
  if (textLen == 0) {
    lastInitHadContext = false;
    logf("init fresh\n");
    sendJson(kInitFreshJson);
    logf("sent initiation bytes=%u (literal)\n",
         (unsigned)(sizeof(kInitFreshJson) - 1));
    return;
  }

  const size_t need = 512 + textLen * 6 + sizeof(kInitClientEventsJson);
  if (!ensureTxJson(need < 4096 ? 4096 : need)) {
    logf("init OOM need=%u — falling back to fresh\n", (unsigned)need);
    lastInitHadContext = false;
    sendJson(kInitFreshJson);
    return;
  }

  size_t at = 0;
  if (!appendLit(txJsonBuf, txJsonCap, &at,
                 "{\"type\":\"conversation_initiation_client_data\",") ||
      !appendLit(txJsonBuf, txJsonCap, &at, kInitClientEventsJson) ||
      !appendLit(txJsonBuf, txJsonCap, &at,
                 ",\"dynamic_variables\":{"
                 "\"has_conversation_context\":\"true\","
                 "\"conversation_context\":\"")) {
    logf("init build fail (head)\n");
    return;
  }
  const size_t next =
      jsonEscapeAppend(txJsonBuf, txJsonCap, at, text, textLen);
  if (next == (size_t)-1) {
    logf("init escape fail — falling back to fresh\n");
    lastInitHadContext = false;
    sendJson(kInitFreshJson);
    return;
  }
  at = next;
  if (!appendLit(txJsonBuf, txJsonCap, &at, "\"}}")) return;

  lastInitHadContext = true;
  logf("init continue len=%u (of %u)\n", (unsigned)textLen, (unsigned)fullLen);
  char preview[96];
  size_t p = 0;
  for (; p + 1 < sizeof(preview) && p < textLen && text[p] != '\n'; p++) {
    preview[p] = text[p];
  }
  preview[p] = '\0';
  logf("init context head: %s%s\n", preview, textLen > p ? "…" : "");

  // Copy into DRAM for the first post-connect frame — some TLS paths have been
  // flaky reading the initiation payload straight from PSRAM.
  char stackInit[1024];
  if (at + 1 <= sizeof(stackInit)) {
    memcpy(stackInit, txJsonBuf, at + 1);
    sendJson(stackInit);
  } else {
    sendJson(txJsonBuf);
  }
  logf("sent initiation bytes=%u\n", (unsigned)at);
}

static void sendPong(int eventId) {
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"type\":\"pong\",\"event_id\":%d}", eventId);
  sendJson(buf);
}

static void sendToolResult(const char *callId, const char *result, bool isError) {
  if (!callId || !callId[0]) return;
  JsonDocument doc;
  doc["type"] = "client_tool_result";
  doc["tool_call_id"] = callId;
  doc["result"] = result ? result : "";
  doc["is_error"] = isError;
  String body;
  serializeJson(doc, body);
  sendJson(body.c_str());
}

static JsonObjectConst toolCallObject(JsonDocument &doc) {
  JsonObjectConst call = doc["client_tool_call"];
  if (!call.isNull()) return call;
  call = doc["client_tool_call_event"];
  if (!call.isNull()) return call;
  return doc.as<JsonObjectConst>();
}

static void handleClientToolCall(JsonDocument &doc) {
  JsonObjectConst call = toolCallObject(doc);
  const char *name = call["tool_name"] | "";
  const char *id = call["tool_call_id"] | "";
  // Default true: if the key is missing we still reply. A spurious result is
  // cheaper than a hung agent that expected one.
  const bool expects = call["expects_response"] | true;
  JsonVariantConst params = call["parameters"];

  // Some agents deliver system end_call as client_tool_call instead of
  // agent_tool_response. Acknowledge then hang up after farewell audio drains.
  if (!strcmp(name, "end_call")) {
    logf("tool end_call id=%s expects=%d (client_tool_call)\n",
         id[0] ? id : "?", expects ? 1 : 0);
    if (expects) sendToolResult(id, "ended", /*isError=*/false);
    scheduleEndCall("end_call");
    return;
  }

  // Async tools (image gen): start work and reply later from the WS task so
  // we do not block pings / mic uplink for tens of seconds.
  if (!strcmp(name, "generate_image")) {
    if (!settingsVisualiseEnabled()) {
      // Always reply immediately so the agent learns Visualise is off and
      // does not wait on a pending image job.
      constexpr const char *kDisabled =
          "Visualise is disabled in device settings. Image generation is "
          "unavailable until the user turns Visualise on.";
      logf("tool generate_image blocked (visualise off)\n");
      sendToolResult(id, kDisabled, /*isError=*/false);
      return;
    }
    char result[160];
    bool pending = false;
    const bool ok = voiceImageToolStart(id, params.as<JsonObjectConst>(), result,
                                       sizeof(result), &pending);
    logf("tool %s id=%s expects=%d ok=%d pending=%d %s\n", name,
         id[0] ? id : "?", expects ? 1 : 0, ok ? 1 : 0, pending ? 1 : 0,
         result);
    if (expects && !pending) sendToolResult(id, result, !ok);
    return;
  }

  char result[160];
  const bool ok = voiceToolsDispatch(name, params, result, sizeof(result));
  logf("tool %s id=%s expects=%d ok=%d %s\n", name[0] ? name : "?",
       id[0] ? id : "?", expects ? 1 : 0, ok ? 1 : 0, result);
  if (expects) sendToolResult(id, result, !ok);
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

static void pushAgentPcmBytes(const uint8_t *data, size_t nbytes) {
  if (!nbytes) return;
  const size_t pushed = talkPlayRingPush(data, nbytes);
  if (pushed < nbytes) {
    // Counted in wire bytes, so the magnitude depends on the negotiated
    // format. Only whether it moves matters: nonzero means a reply outran the
    // ring and the agent will be heard skipping.
    playRingDrops += (uint32_t)(nbytes - pushed);
    logf("play ring drop %u\n", (unsigned)(nbytes - pushed));
  }
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

  // Select hold: drop the rest of this agent turn until `interruption` (or idle).
  // Do this before decode so we do not revive Speaking via lastAgentAudioMs.
  if (holdListening) {
    lastHoldDropMs = millis();
    return;
  }

  // Match the JS SDK: drop audio that belongs to a turn already interrupted.
  {
    const char *eidKey = strstr(payload, "\"event_id\"");
    if (eidKey && lastInterruptEventId > 0) {
      const char *p = eidKey + 10;
      while (*p == ':' || *p == ' ' || *p == '\t') p++;
      const int eid = atoi(p);
      if (eid > 0 && eid <= lastInterruptEventId) {
        return;
      }
    }
  }

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
  const uint32_t ringBeforeMs = talkPlayRingUsedMs();
  if (audioChunksRx <= 5 || (audioChunksRx % 25) == 0 || ringBeforeMs == 0) {
    logf("audio #%lu bytes=%u ring=%lums gap=%lums dec=%lums under=%lu\n",
         (unsigned long)audioChunksRx, (unsigned)outLen,
         (unsigned long)ringBeforeMs, (unsigned long)gapMs,
         (unsigned long)decodeMs, (unsigned long)playUnderruns);
  }
  pushAgentPcmBytes(pcmScratch, outLen);
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
  if (!type[0]) {
    logf("ws json no type (%u) %.80s\n", (unsigned)length, msg);
    return;
  }
  // Audio is the firehose; everything else is rare and worth a line so a
  // missed client_tool_call cannot hide as silence.
  if (strcmp(type, "audio") != 0 && strcmp(type, "ping") != 0) {
    logf("ws %s (%u)\n", type, (unsigned)length);
  }

  if (!strcmp(type, "conversation_initiation_metadata")) {
    JsonObject ev = doc["conversation_initiation_metadata_event"];
    const char *cid = ev["conversation_id"] | "";
    if (cid[0]) {
      copyTrunc(talkCtx.conversationId, sizeof(talkCtx.conversationId), cid);
      contextDirty = true;
      logf("conversation_id=%s\n", talkCtx.conversationId);
    }
    const char *outFmt = ev["agent_output_audio_format"] | "pcm_16000";
    const char *inFmt = ev["user_input_audio_format"] | "pcm_16000";
    agentOutRate = parseRate(outFmt);
    agentOutUlaw = strstr(outFmt, "ulaw") != nullptr;
    talkPlayRingSetFormat(agentOutRate, agentOutUlaw);
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
    voiceContextAppend(&talkCtx, VoiceTurnRole::User, t);
    contextDirty = true;
    persistContext();
    logf("you: %s\n", t);
  } else if (!strcmp(type, "agent_response")) {
    const char *t = doc["agent_response_event"]["agent_response"] | "";
    copyTrunc(lastReplyBuf, sizeof(lastReplyBuf), t);
    voiceContextAppend(&talkCtx, VoiceTurnRole::Agent, t);
    contextDirty = true;
    persistContext();
    if (!holdListening) setStatus("Speaking");
    logf("agent: %s\n", t);
  } else if (!strcmp(type, "interruption")) {
    // Official barge-in signal. Same cleanup as the ElevenLabs SDK
    // (audioInterface.interrupt / fadeOutAudio).
    JsonObject ev = doc["interruption_event"];
    const int eid = ev["event_id"] | 0;
    if (eid > lastInterruptEventId) lastInterruptEventId = eid;
    const uint32_t lostMs = talkPlayRingUsedMs();
    talkPlayRingClear();
    playPriming = true;
    holdListening = false;
    lastAgentAudioMs = 0;
    lastNonSilentPlayMs = 0;
    setStatus("Listening");
    logf("(interrupted) id=%d discarded=%lums\n", eid, (unsigned long)lostMs);
  } else if (!strcmp(type, "client_tool_call")) {
    handleClientToolCall(doc);
  } else if (!strcmp(type, "agent_tool_request")) {
    JsonObjectConst req = doc["agent_tool_request"];
    const char *tool = req["tool_name"] | "";
    logf("tool-req %s type=%s id=%s\n", tool, req["tool_type"] | "?",
         req["tool_call_id"] | "?");
    // System end_call: request can arrive before the farewell TTS finishes;
    // hang up on response (below) so audio can drain. Log only here.
  } else if (!strcmp(type, "agent_tool_response") ||
             !strcmp(type, "agent_tool_response_full_payload")) {
    // Official hang-up signal — same as @elevenlabs/client
    // BaseConversation.handleAgentToolResponse.
    const char *key = !strcmp(type, "agent_tool_response")
                          ? "agent_tool_response"
                          : "agent_tool_response_full_payload";
    JsonObjectConst resp = doc[key];
    const char *tool = resp["tool_name"] | "";
    const bool isErr = resp["is_error"] | false;
    logf("tool-resp %s type=%s err=%d\n", tool, resp["tool_type"] | "?",
         isErr ? 1 : 0);
    if (!isErr && !strcmp(tool, "end_call")) {
      scheduleEndCall("end_call");
    }
  } else if (!strcmp(type, "guardrail_triggered")) {
    scheduleEndCall("guardrail_triggered");
  } else if (!strcmp(type, "error")) {
    JsonObjectConst ev = doc["error_event"];
    const char *errType = ev["error_type"] | "";
    if (!strcmp(errType, "max_duration_exceeded")) {
      scheduleEndCall("max_duration_exceeded");
    } else {
      setStatus("Error");
      logf("EL error: %.160s\n", msg);
    }
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
    holdListening = false;
    holdStartedMs = 0;
    lastHoldDropMs = 0;
    lastInterruptEventId = 0;
    talkAudioResetWriteStats();
    agentInUlaw = false;
    agentInRate = TALK_SAMPLE_RATE;
    agentOutUlaw = false;
    agentOutRate = TALK_SAMPLE_RATE;
    talkPlayRingSetFormat(TALK_SAMPLE_RATE, false);
    micUplinkClear();
    talkUlawReset();
    pendingInit = true;
    setStatus("Connected");
    break;
  case WStype_TEXT:
  case WStype_BIN:
    handleWsMessage(payload, length);
    break;
  case WStype_DISCONNECTED:
    if (payload && length) {
      logf("WS disconnected: %.*s\n", (int)length, (const char *)payload);
    } else {
      logf("WS disconnected\n");
    }
    if (pendingEndCall) {
      // Peer closed while we were draining farewell — treat as clean hang-up.
      pendingEndCall = false;
      endedByAgent = true;
      setStatus("Idle");
    } else if (!agentReady && lastInitHadContext) {
      // Peer closed before conversation_initiation_metadata — almost always
      // the continue payload (undeclared dynamic vars / oversized context).
      // Drop saved context so the next Talk start uses init fresh.
      logf("continue init rejected — clearing saved context for next start\n");
      voiceContextClear(sessionStorage(), sessionAgentId());
      voiceContextReset(&talkCtx);
      contextDirty = false;
      lastInitHadContext = false;
      setStatus("Init rejected");
    } else if (!agentReady) {
      // Typical: close 3000 quota_exceeded (see [WS] peer close on serial).
      logf("disconnect before agent ready\n");
      setStatus("Out of credits");
    } else {
      setStatus("Disconnected");
    }
    logMem("ws-off");
    agentReady = false;
    agentActive = false;
    break;
  case WStype_ERROR:
    logf("WS error len=%u\n", (unsigned)length);
    setStatus("WS error");
    break;
  case WStype_FRAGMENT:
  case WStype_FRAGMENT_TEXT_START:
  case WStype_FRAGMENT_BIN_START:
  case WStype_FRAGMENT_FIN:
    logf("ws fragment type=%d len=%u\n", (int)type, (unsigned)length);
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
      {
        char callId[64];
        char result[160];
        bool isError = false;
        if (voiceImageToolTakeResult(callId, sizeof(callId), result,
                                    sizeof(result), &isError)) {
          sendToolResult(callId, result, isError);
          logf("tool generate_image done err=%d %s\n", isError ? 1 : 0, result);
        }
      }
      // Farewell may still be in the play ring; stop uplink so we do not open
      // another turn after end_call.
      if (!pendingEndCall) sendMicChunkFromUplink();

      // Select hold: once the agent stops sending audio, release so the next
      // turn can play. Real barge-in clears hold earlier via `interruption`.
      if (!pendingEndCall && holdListening && lastHoldDropMs != 0 &&
          (millis() - lastHoldDropMs) > 800 &&
          (millis() - holdStartedMs) > 400) {
        holdListening = false;
        logf("hold released (agent audio idle)\n");
      }

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

/**
 * Parsed straight off the socket behind a filter instead of via getString().
 *
 * The raw listing carries access_info, trust_context and per-agent call stats
 * we never read; buffering all of it as a String and then again as a
 * JsonDocument puts tens of KB into internal heap right before the session
 * needs DMA-capable blocks for TLS. The filter keeps the document to a dozen
 * short strings.
 */
int elAgentFetchList(ElAgentInfo *out, int maxCount, bool *hasMore,
                       char *errOut, size_t errLen) {
  auto fail = [&](const char *msg) {
    logf("agent list: %s\n", msg);
    if (errOut && errLen) copyTrunc(errOut, errLen, msg);
    return -1;
  };

  if (hasMore) *hasMore = false;
  if (!out || maxCount <= 0) return fail("Bad args");

  configInit();
  const char *apiKey = getConfig(Config::ElevenlabsApiKey);
  if (!apiKey || !apiKey[0]) return fail("No API key");

  if (WiFi.status() != WL_CONNECTED && !connectWifi()) {
    return fail("Wi-Fi failed");
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  // One past the cap so has_more is right even when the API would have fit
  // the page exactly.
  char url[96];
  snprintf(url, sizeof(url),
           "https://api.elevenlabs.io/v1/convai/agents?page_size=%d",
           maxCount + 1);
  if (!http.begin(client, url)) return fail("HTTP begin failed");
  http.addHeader("xi-api-key", apiKey);

  const int code = http.GET();
  if (code != 200) {
    http.end();
    char msg[24];
    snprintf(msg, sizeof(msg), "Agents HTTP %d", code);
    return fail(msg);
  }

  JsonDocument filter;
  filter["has_more"] = true;
  filter["agents"][0]["agent_id"] = true;
  filter["agents"][0]["name"] = true;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(
      doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) return fail("Agents parse fail");

  int n = 0;
  for (JsonObject a : doc["agents"].as<JsonArray>()) {
    if (n >= maxCount) break;
    const char *id = a["agent_id"] | "";
    if (!id[0]) continue;
    const char *name = a["name"] | "";
    copyTrunc(out[n].id, sizeof(out[n].id), id);
    copyTrunc(out[n].name, sizeof(out[n].name), name[0] ? name : "(unnamed)");
    n++;
  }

  if (hasMore) *hasMore = (doc["has_more"] | false) || n > maxCount;
  logf("agent list: %d agent(s)\n", n);
  return n;
}

bool elAgentStart(AppHost *host, const char *agentId) {
  if (agentActive) return true;

  sessionHost = host;
  if (agentId && agentId[0]) {
    copyTrunc(activeAgentId, sizeof(activeAgentId), agentId);
  } else {
    activeAgentId[0] = 0;
  }

  voiceContextReset(&talkCtx);
  contextDirty = false;
  {
    Storage *st = sessionStorage();
    const char *aid = sessionAgentId();
    if (st && st->ready() && aid && aid[0] &&
        voiceContextLoad(st, aid, &talkCtx)) {
      logf("context loaded prev=%s len=%u\n",
           talkCtx.conversationId[0] ? talkCtx.conversationId : "-",
           (unsigned)talkCtx.len);
    } else {
      logf("context none (sd=%d agent=%s)\n",
           (st && st->ready()) ? 1 : 0, aid && aid[0] ? aid : "-");
    }
  }

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
  if (!getConfig(Config::ElevenlabsApiKey)[0]) {
    setStatus("No EL API key");
    logf("ElevenLabs API key missing\n");
    return false;
  }
  if (!activeAgentId[0] && !getConfig(Config::ElevenlabsAgentId)[0]) {
    setStatus("No agent");
    logf("no agent selected and no configured default\n");
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

  talkAudioResetDsp();
  talkAecReset();
  talkUlawReset();
  voiceToolsReset();
  voiceToolsRegisterDefaults();
  voiceImageToolReset();
  voiceImageToolSetStorage(sessionStorage());
  voiceImageToolSetHost(sessionHost);
  // Always register so the agent can call generate_image and get an immediate
  // "disabled" result when Visualise is off (instead of an unknown tool).
  voiceImageToolRegister();
  logf("tools registered (visualise=%d)\n",
       settingsVisualiseEnabled() ? 1 : 0);
  micUplinkClear();
  agentInRate = TALK_SAMPLE_RATE;
  agentInUlaw = false;
  agentOutUlaw = false;
  agentOutRate = TALK_SAMPLE_RATE;
  talkPlayRingSetFormat(TALK_SAMPLE_RATE, false);
  micCapturedSamples = 0;
  micSentSamples = 0;
  micDroppedSamples = 0;
  micReadGaps = 0;
  lastUplinkStatMs = 0;
  pendingInit = false;
  pendingPong = false;
  pendingEndCall = false;
  endedByAgent = false;
  endCallAtMs = 0;
  holdListening = false;
  holdStartedMs = 0;
  lastHoldDropMs = 0;
  lastInterruptEventId = 0;
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

void elAgentStop() {
  pendingInit = false;
  pendingPong = false;
  pendingEndCall = false;
  holdListening = false;
  persistContext();
  stopAudioTask();
  stopWsTask();
  if (ws.isConnected()) ws.disconnect();
  agentActive = false;
  agentReady = false;
  talkAudioStop();
  voiceToolsReset();
  voiceImageToolReset();
  sessionHost = nullptr;
  setStatus("Idle");
  logf("talk stopped\n");
}

bool elAgentIsActive() { return agentActive; }
bool elAgentIsReady() { return agentReady; }

bool elAgentTakeEndedByAgent() {
  if (!endedByAgent) return false;
  endedByAgent = false;
  return true;
}
ElHealth elAgentHealth() {
  if (!agentActive) return ElHealth::Offline;
  if (!agentReady) return ElHealth::Connecting;

  // The uplink is the canary. A healthy send lands every ~32 ms, the slow-send
  // log already fires at 250 ms, and a write that never completes is what
  // finally kills the socket at WEBSOCKETS_TCP_TIMEOUT. Flagging Stuck well
  // short of that turns a silent 5 s death into visible warning.
  static constexpr uint32_t kDegradedMs = 400;
  static constexpr uint32_t kStuckMs = 1500;
  // Backlog idles at a chunk or two, so half a second means the drain is
  // losing rather than jittering.
  static constexpr size_t kBacklogDegraded = TALK_SAMPLE_RATE / 2;

  if (lastSendOkMs) {
    const uint32_t since = millis() - lastSendOkMs;
    if (since >= kStuckMs) return ElHealth::Stuck;
    if (since >= kDegradedMs) return ElHealth::Degraded;
  }
  if (micUplinkUsed() >= kBacklogDegraded) return ElHealth::Degraded;
  return ElHealth::Ok;
}

const char *elAgentStatus() { return statusBuf; }
const char *elAgentLastUser() { return lastUserBuf; }
const char *elAgentLastReply() { return lastReplyBuf; }

static bool agentIsSpeaking() {
  // Any queued TTS, recent network audio, or recent non-silent DAC output.
  if (!talkPlayRingEmpty()) return true;
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

bool elAgentIsSpeaking() {
  return agentActive && !holdListening && agentIsSpeaking();
}

void elAgentUserActivity() {
  if (!agentActive || !agentReady) return;

  // ElevenLabs barge-in is server-side (VAD on your mic uplink → `interruption`).
  // Select only mutes local playback so you can speak into an open mic; I2S
  // keeps running. Do not send `user_activity` — that is a typing keepalive and
  // does not cancel in-flight TTS (which is why status flipped back to Speaking).
  const uint32_t lostMs = talkPlayRingUsedMs();
  talkPlayRingClear();
  playPriming = true;
  lastAgentAudioMs = 0;
  lastNonSilentPlayMs = 0;
  holdListening = true;
  holdStartedMs = millis();
  lastHoldDropMs = holdStartedMs;
  setStatus("Listening");
  logf("take floor ring_cleared=%lums — speak to barge in\n",
       (unsigned long)lostMs);
}

float elAgentPlayLevel() { return playLevelEma; }

int elAgentWaveBars(uint8_t *out, int maxBars) {
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
static void elAgentPumpAudio() {
  if (!agentActive) return;

  int n = talkAudioReadPcmTimeout(micTmp, TALK_I2S_BUF_SAMPLES, 20);
  if (n <= 0) {
    micReadGaps++;
    // Still feed the speaker — a dry TX DMA on the MAX98357 is loud static.
    talkAudioWriteSilence(TALK_I2S_BUF_SAMPLES, 20);
    return;
  }

  // While priming, playScratch stays silent and the ring keeps filling. The
  // silence still goes to I2S and to the AEC reference, so the echo estimate
  // continues to see exactly what the speaker emits.
  size_t have = 0;
  if (playPriming) {
    const uint32_t queuedMs = talkPlayRingUsedMs();
    const uint32_t lastRx = lastAgentAudioMs;
    const bool primed = queuedMs >= TALK_PLAY_PRIME_MS;
    const bool streamIdle =
        lastRx != 0 && (millis() - lastRx) >= TALK_PLAY_PRIME_FLUSH_MS;
    if (!talkPlayRingEmpty() && (primed || streamIdle)) playPriming = false;
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
    logf("play ring=%lums under=%lu ovf=%lu wrdrop=%lu wrstall=%lu wrmax=%lums\n",
         (unsigned long)talkPlayRingUsedMs(),
         (unsigned long)playUnderruns, (unsigned long)playRingDrops,
         (unsigned long)talkAudioWriteDropped(),
         (unsigned long)talkAudioWriteStalls(),
         (unsigned long)talkAudioWriteMaxMs());
  }

  if (pendingEndCall) {
    // Keep "Ending" — do not bounce back to Listening after the goodbye TTS.
  } else if (holdListening) {
    if (strcmp(statusBuf, "Listening") != 0) setStatus("Listening");
  } else if (agentIsSpeaking()) {
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
  if (pendingEndCall) {
    // Still run I2S so farewell audio drains; do not capture for uplink.
    return;
  }
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
      elAgentPumpAudio();
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

void elAgentLoop() {
  // Audio runs on el_audio task. UI thread finishes end_call teardown here so
  // stopWsTask does not wait on the ws task from inside itself.
  if (!pendingEndCall || !agentActive) return;

  static constexpr uint32_t kEndCallDrainTailMs = 400;
  static constexpr uint32_t kEndCallTimeoutMs = 12000;
  const uint32_t now = millis();
  const bool drained =
      talkPlayRingEmpty() &&
      (lastAgentAudioMs == 0 ||
       (now - lastAgentAudioMs) >= kEndCallDrainTailMs) &&
      (lastNonSilentPlayMs == 0 ||
       (now - lastNonSilentPlayMs) >= kEndCallDrainTailMs);
  const bool timedOut =
      endCallAtMs != 0 && (now - endCallAtMs) >= kEndCallTimeoutMs;
  if (!drained && !timedOut) return;

  logf("end_call hang up drained=%d timed_out=%d ring=%lums\n", drained ? 1 : 0,
       timedOut ? 1 : 0, (unsigned long)talkPlayRingUsedMs());
  endedByAgent = true;
  pendingEndCall = false;
  elAgentStop();
}
