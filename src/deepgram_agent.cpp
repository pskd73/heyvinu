#include "deepgram_agent.h"

#include "net_wifi.h"
#include "runtime_config.h"
#include "settings_app.h"
#include "talk_aec.h"
#include "talk_audio.h"
#include "talk_config.h"
#include "talk_ulaw.h"
#include "voice_context.h"
#include "voice_image_tool.h"
#include "voice_tools.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Flow32.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr const char *kHost = "agent.deepgram.com";
constexpr const char *kPath = "/v1/agent/converse";

constexpr int kSampleRate = TALK_SAMPLE_RATE;
/** 16 ms uplink frames — smaller TLS writes stall the WS RX less than 32 ms. */
constexpr int kChunkSamples = 256;
constexpr uint32_t kKeepAliveMs = 5000;
/** ~1 s mic cushion so a large WS RX frame does not gap the uplink. */
constexpr size_t kMicRingSamples = TALK_SAMPLE_RATE;
/**
 * Deepgram ships TTS as many small (~32–64 ms) binary frames. Mic sendBIN can
 * stall the WS task for hundreds of ms (TLS), so the play ring must cover that
 * — 350 ms was not enough and sounded like gaps/drops.
 */
constexpr uint32_t kPlayPrimeMs = 900;
constexpr uint32_t kPlayPrimeFlushMs = 400;
/** When the jitter buffer is thinner than this, skip mic TX and only drain RX. */
constexpr uint32_t kPlayLowWaterMs = 350;

// Prompt / models kept as literals; Settings JSON is built at send time so we
// can attach agent.context.messages from voice_context.
constexpr const char *kGreeting = "Hi! I'm your test voice agent.";
constexpr const char *kPrompt =
    "You are a helpful voice assistant on a small handheld device. "
    "Keep answers short — one or two sentences. "
    "You can call show_text to display a short message on the device screen, "
    "and generate_image when the user asks you to visualise or draw something.";

/** Client-side tools advertised in Settings.agent.think.functions. */
void appendThinkFunctions(JsonArray functions) {
  {
    JsonObject f = functions.add<JsonObject>();
    f["name"] = "show_text";
    f["description"] =
        "Display a short message on the device screen for the user to read.";
    JsonObject params = f["parameters"].to<JsonObject>();
    params["type"] = "object";
    JsonObject props = params["properties"].to<JsonObject>();
    JsonObject text = props["text"].to<JsonObject>();
    text["type"] = "string";
    text["description"] = "Message to show on screen (keep under 100 chars).";
    JsonArray req = params["required"].to<JsonArray>();
    req.add("text");
  }
  {
    JsonObject f = functions.add<JsonObject>();
    f["name"] = "generate_image";
    f["description"] =
        "Generate an image from a text prompt and show it on the device.";
    JsonObject params = f["parameters"].to<JsonObject>();
    params["type"] = "object";
    JsonObject props = params["properties"].to<JsonObject>();
    JsonObject prompt = props["prompt"].to<JsonObject>();
    prompt["type"] = "string";
    prompt["description"] = "Image description / prompt.";
    JsonArray req = params["required"].to<JsonArray>();
    req.add("prompt");
  }
}

portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE micMux_ = portMUX_INITIALIZER_UNLOCKED;

TaskHandle_t wsTask_ = nullptr;
TaskHandle_t audioTask_ = nullptr;
AppHost *sessionHost_ = nullptr;
VoiceContext voiceCtx_{};
bool contextDirty_ = false;
volatile bool run_ = false;
volatile bool active_ = false;
volatile bool wsConnected_ = false;
volatile bool gotWelcome_ = false;
volatile bool agentReady_ = false;
volatile bool settingsSent_ = false;
volatile bool duplexUp_ = false;
volatile bool playPriming_ = true;
volatile bool agentSpeaking_ = false;
volatile uint32_t lastAgentAudioMs_ = 0;
volatile uint32_t playUnderruns_ = 0;
volatile uint32_t playLowSkips_ = 0;

WebSocketsClient ws_;
char authHeader_[200] = {};
char extraHdr_[220] = {};

char status_[48] = "Idle";
char userText_[240] = {};
char agentText_[320] = {};
uint32_t gen_ = 0;

int16_t pcmOut_[TALK_I2S_BUF_SAMPLES];
int16_t micTmp_[TALK_I2S_BUF_SAMPLES];
int16_t micSend_[kChunkSamples];
uint8_t wireBuf_[kChunkSamples * 2];

int16_t *micRing_ = nullptr;
size_t micCap_ = 0;
size_t micHead_ = 0;
size_t micTail_ = 0;
size_t micUsed_ = 0;

void bumpGen() { gen_++; }

void logHeap(const char *tag) {
  Serial.printf("[dg-agent] %s heap=%u contig=%u dma=%u psram=%u\n", tag,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                (unsigned)ESP.getFreePsram());
}

void setStatus(const char *s) {
  portENTER_CRITICAL(&mux_);
  strncpy(status_, s ? s : "", sizeof(status_) - 1);
  status_[sizeof(status_) - 1] = '\0';
  bumpGen();
  portEXIT_CRITICAL(&mux_);
  Serial.printf("[dg-agent] %s\n", status_);
}

void setUserText(const char *t) {
  portENTER_CRITICAL(&mux_);
  strncpy(userText_, t ? t : "", sizeof(userText_) - 1);
  userText_[sizeof(userText_) - 1] = '\0';
  bumpGen();
  portEXIT_CRITICAL(&mux_);
}

void setAgentText(const char *t) {
  portENTER_CRITICAL(&mux_);
  strncpy(agentText_, t ? t : "", sizeof(agentText_) - 1);
  agentText_[sizeof(agentText_) - 1] = '\0';
  bumpGen();
  portEXIT_CRITICAL(&mux_);
}

void clearTexts() {
  portENTER_CRITICAL(&mux_);
  userText_[0] = '\0';
  agentText_[0] = '\0';
  bumpGen();
  portEXIT_CRITICAL(&mux_);
}

bool micRingInit() {
  if (micRing_) return true;
  micRing_ = (int16_t *)heap_caps_malloc(kMicRingSamples * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!micRing_) {
    micRing_ = (int16_t *)malloc(kMicRingSamples * sizeof(int16_t));
  }
  if (!micRing_) return false;
  micCap_ = kMicRingSamples;
  micHead_ = micTail_ = micUsed_ = 0;
  return true;
}

void micRingClear() {
  portENTER_CRITICAL(&micMux_);
  micHead_ = micTail_ = micUsed_ = 0;
  portEXIT_CRITICAL(&micMux_);
}

size_t micRingPush(const int16_t *data, size_t n) {
  if (!micRing_ || !data || !n) return 0;
  portENTER_CRITICAL(&micMux_);
  size_t room = micCap_ - micUsed_;
  size_t take = n < room ? n : room;
  for (size_t i = 0; i < take; i++) {
    micRing_[micHead_] = data[i];
    micHead_ = (micHead_ + 1) % micCap_;
  }
  micUsed_ += take;
  portEXIT_CRITICAL(&micMux_);
  return take;
}

size_t micRingPop(int16_t *out, size_t n) {
  if (!micRing_ || !out || !n) return 0;
  portENTER_CRITICAL(&micMux_);
  size_t take = n < micUsed_ ? n : micUsed_;
  for (size_t i = 0; i < take; i++) {
    out[i] = micRing_[micTail_];
    micTail_ = (micTail_ + 1) % micCap_;
  }
  micUsed_ -= take;
  portEXIT_CRITICAL(&micMux_);
  return take;
}

bool startDuplexAudio() {
  if (duplexUp_) return true;
  if (!talkAudioInit()) {
    setStatus("Play ring fail");
    return false;
  }
  talkPlayRingClear();
  // Wire is mulaw @ 8 kHz; play ring expands + resamples to 16 kHz PCM.
  talkPlayRingSetFormat(8000, /*ulaw=*/true);
  talkAudioResetDsp();
  talkUlawReset();
  if (!talkAudioStartDuplex()) {
    setStatus("Mic failed");
    return false;
  }
  if (!micRingInit()) {
    setStatus("Mic ring fail");
    talkAudioStop();
    return false;
  }
  micRingClear();
  talkAecReset();
  playPriming_ = true;
  agentSpeaking_ = false;
  lastAgentAudioMs_ = 0;
  playUnderruns_ = 0;
  playLowSkips_ = 0;
  duplexUp_ = true;
  return true;
}

Storage *sessionStorage() {
  return sessionHost_ ? sessionHost_->storage() : nullptr;
}

void persistContext() {
  if (!contextDirty_) return;
  Storage *st = sessionStorage();
  if (!st) return;
  voiceCtx_.updatedMs = millis();
  if (!voiceCtx_.conversationId[0]) {
    strncpy(voiceCtx_.conversationId, kVoiceContextDeepgramId,
            sizeof(voiceCtx_.conversationId) - 1);
  }
  if (voiceContextSave(st, kVoiceContextDeepgramId, voiceCtx_)) {
    contextDirty_ = false;
    Serial.printf("[dg-agent] context saved len=%u\n", (unsigned)voiceCtx_.len);
  }
}

void sendSettings() {
  if (settingsSent_) return;
  settingsSent_ = true;

  JsonDocument doc;
  doc["type"] = "Settings";
  JsonObject audio = doc["audio"].to<JsonObject>();
  JsonObject in = audio["input"].to<JsonObject>();
  in["encoding"] = "mulaw";
  in["sample_rate"] = 8000;
  JsonObject out = audio["output"].to<JsonObject>();
  out["encoding"] = "mulaw";
  out["sample_rate"] = 8000;
  out["container"] = "none";

  JsonObject agent = doc["agent"].to<JsonObject>();
  // Skip greeting when resuming — avoids re-hearing the hello every connect.
  if (voiceCtx_.len == 0) {
    agent["greeting"] = kGreeting;
  }

  JsonObject listen = agent["listen"].to<JsonObject>();
  JsonObject listenProv = listen["provider"].to<JsonObject>();
  listenProv["type"] = "deepgram";
  listenProv["model"] = "flux-general-en";
  listenProv["version"] = "v2";
  listenProv["eot_threshold"] = 0.7;
  listenProv["eot_timeout_ms"] = 4000;

  JsonObject think = agent["think"].to<JsonObject>();
  JsonObject thinkProv = think["provider"].to<JsonObject>();
  thinkProv["type"] = "open_ai";
  thinkProv["model"] = "gpt-4o-mini";
  thinkProv["temperature"] = 0.6;
  think["prompt"] = kPrompt;
  appendThinkFunctions(think["functions"].to<JsonArray>());

  JsonObject speak = agent["speak"].to<JsonObject>();
  JsonObject speakProv = speak["provider"].to<JsonObject>();
  speakProv["type"] = "deepgram";
  speakProv["version"] = "v1";
  speakProv["model"] = "aura-2-thalia-en";

  if (voiceCtx_.len > 0) {
    JsonArray msgs = agent["context"]["messages"].to<JsonArray>();
    const size_t n = voiceContextFillDgHistory(voiceCtx_, msgs);
    Serial.printf("[dg-agent] Settings + %u history msgs (ctx len=%u)\n",
                  (unsigned)n, (unsigned)voiceCtx_.len);
  }

  String body;
  serializeJson(doc, body);
  Serial.printf("[dg-agent] sending Settings (%u B)\n", (unsigned)body.length());
  ws_.sendTXT(body);
  setStatus("Configuring…");
}

void sendFunctionCallResponse(const char *id, const char *name,
                              const char *content,
                              const char *thoughtSignature = nullptr) {
  if (!id || !id[0] || !ws_.isConnected()) return;
  JsonDocument doc;
  doc["type"] = "FunctionCallResponse";
  doc["id"] = id;
  if (name && name[0]) doc["name"] = name;
  doc["content"] = content ? content : "";
  if (thoughtSignature && thoughtSignature[0]) {
    doc["thought_signature"] = thoughtSignature;
  }
  String body;
  serializeJson(doc, body);
  ws_.sendTXT(body);
  Serial.printf("[dg-agent] FunctionCallResponse id=%s name=%s\n", id,
                name ? name : "?");
}

void handleFunctionCallRequest(JsonDocument &doc) {
  JsonArrayConst fns = doc["functions"].as<JsonArrayConst>();
  if (fns.isNull()) return;

  for (JsonObjectConst fn : fns) {
    const bool clientSide = fn["client_side"] | true;
    if (!clientSide) continue;

    const char *id = fn["id"] | "";
    const char *name = fn["name"] | "";
    const char *argsStr = fn["arguments"] | "{}";
    const char *thought = fn["thought_signature"] | "";
    if (!id[0] || !name[0]) continue;

    JsonDocument argsDoc;
    DeserializationError err = deserializeJson(argsDoc, argsStr);
    JsonObjectConst params =
        err ? JsonObjectConst() : argsDoc.as<JsonObjectConst>();

    if (!strcmp(name, "generate_image")) {
      if (!settingsVisualiseEnabled()) {
        sendFunctionCallResponse(
            id, name,
            "Visualise is disabled in device settings. Image generation is "
            "unavailable until the user turns Visualise on.",
            thought);
        continue;
      }
      char result[160];
      bool pending = false;
      const bool ok =
          voiceImageToolStart(id, params, result, sizeof(result), &pending);
      Serial.printf("[dg-agent] tool %s id=%s ok=%d pending=%d %s\n", name, id,
                    ok ? 1 : 0, pending ? 1 : 0, result);
      if (!pending) sendFunctionCallResponse(id, name, result, thought);
      continue;
    }

    char result[160];
    const bool ok = voiceToolsDispatch(name, params, result, sizeof(result));
    Serial.printf("[dg-agent] tool %s id=%s ok=%d %s\n", name, id, ok ? 1 : 0,
                  result);
    sendFunctionCallResponse(id, name, result, thought);
  }
}

void pollAsyncTools() {
  char callId[64];
  char result[160];
  bool isError = false;
  if (!voiceImageToolTakeResult(callId, sizeof(callId), result, sizeof(result),
                                &isError)) {
    return;
  }
  (void)isError;
  sendFunctionCallResponse(callId, "generate_image", result);
}

void handleWsText(uint8_t *payload, size_t length) {
  if (!payload || length == 0) return;

  JsonDocument filter;
  filter["type"] = true;
  filter["role"] = true;
  filter["content"] = true;
  filter["description"] = true;
  filter["message"] = true;
  // Keep the full functions array (arguments is a JSON string).
  filter["functions"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, payload, length,
                      DeserializationOption::Filter(filter))) {
    return;
  }

  const char *type = doc["type"] | "";
  if (!type[0]) return;

  if (strcmp(type, "FunctionCallRequest") == 0) {
    handleFunctionCallRequest(doc);
    return;
  }

  if (strcmp(type, "Welcome") == 0) {
    gotWelcome_ = true;
    setStatus("Welcome");
    sendSettings();
    return;
  }

  if (strcmp(type, "SettingsApplied") == 0) {
    if (!startDuplexAudio()) return;
    agentReady_ = true;
    setStatus("Listening");
    Serial.printf("[dg-agent] SettingsApplied — streaming\n");
    logHeap("ready");
    return;
  }

  if (strcmp(type, "ConversationText") == 0) {
    const char *role = doc["role"] | "";
    const char *content = doc["content"] | "";
    if (!content[0]) return;
    if (strcmp(role, "user") == 0) {
      setUserText(content);
      voiceContextAppend(&voiceCtx_, VoiceTurnRole::User, content);
      contextDirty_ = true;
      persistContext();
      Serial.printf("[dg-agent] user: %s\n", content);
    } else if (strcmp(role, "assistant") == 0) {
      setAgentText(content);
      voiceContextAppend(&voiceCtx_, VoiceTurnRole::Agent, content);
      contextDirty_ = true;
      persistContext();
      Serial.printf("[dg-agent] agent: %s\n", content);
    }
    return;
  }

  if (strcmp(type, "UserStartedSpeaking") == 0) {
    // Real barge-in (after AEC, echo should not reach Deepgram VAD).
    talkPlayRingClear();
    playPriming_ = true;
    agentSpeaking_ = false;
    setStatus("You speaking…");
    return;
  }

  if (strcmp(type, "AgentThinking") == 0) {
    setStatus("Thinking…");
    return;
  }

  if (strcmp(type, "AgentStartedSpeaking") == 0) {
    // Only re-prime if we are not already playing — resetting mid-burst
    // inserted a hole at the start of many replies.
    if (talkPlayRingEmpty()) playPriming_ = true;
    agentSpeaking_ = true;
    setStatus("Agent speaking");
    return;
  }

  if (strcmp(type, "AgentAudioDone") == 0) {
    agentSpeaking_ = false;
    if (agentReady_) setStatus("Listening");
    return;
  }

  if (strcmp(type, "Error") == 0) {
    const char *msg = doc["description"] | doc["message"] | "Agent error";
    char err[48];
    snprintf(err, sizeof(err), "%.44s", msg);
    setStatus(err);
    Serial.printf("[dg-agent] Error: %s\n", msg);
    return;
  }

  if (strcmp(type, "Warning") == 0) {
    const char *msg = doc["description"] | doc["message"] | "";
    Serial.printf("[dg-agent] Warning: %s\n", msg);
    return;
  }
}

void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
  case WStype_CONNECTED:
    wsConnected_ = true;
    gotWelcome_ = false;
    agentReady_ = false;
    settingsSent_ = false;
    setStatus("Connected");
    Serial.printf("[dg-agent] ws connected — wait Welcome\n");
    logHeap("ws-on");
    break;
  case WStype_DISCONNECTED:
    wsConnected_ = false;
    agentReady_ = false;
    gotWelcome_ = false;
    if (payload && length > 0) {
      Serial.printf("[dg-agent] ws disconnected: %.*s\n", (int)length,
                    (const char *)payload);
    } else {
      Serial.printf("[dg-agent] ws disconnected\n");
    }
    // One-shot session: do not hammer TLS reconnects (interval 0 = every loop).
    if (run_) setStatus("Disconnected");
    run_ = false;
    break;
  case WStype_TEXT:
    handleWsText(payload, length);
    break;
  case WStype_BIN:
    if (payload && length > 0) {
      const size_t got = talkPlayRingPush(payload, length);
      lastAgentAudioMs_ = millis();
      if (got < length) {
        Serial.printf("[dg-agent] play ring drop %u\n",
                      (unsigned)(length - got));
      }
    }
    break;
  case WStype_ERROR:
    wsConnected_ = false;
    agentReady_ = false;
    setStatus("WS error");
    run_ = false;
    break;
  default:
    break;
  }
}

void sendKeepAlive() {
  static const char kMsg[] = "{\"type\":\"KeepAlive\"}";
  if (ws_.isConnected()) ws_.sendTXT(kMsg);
}

bool sendPcmChunk(const int16_t *pcm, int n) {
  if (n <= 0 || !agentReady_ || !ws_.isConnected()) return false;
  // 16 kHz PCM → 8 kHz mulaw (n must be even; kChunkSamples is).
  const size_t bytes = talkUlawEncodeFrom16k(pcm, (size_t)n, wireBuf_);
  if (!bytes) return false;
  return ws_.sendBIN(wireBuf_, bytes);
}

void pumpAudio() {
  if (!duplexUp_) return;

  const int n = talkAudioReadPcmTimeout(micTmp_, TALK_I2S_BUF_SAMPLES, 20);
  if (n <= 0) {
    // Keep TX fed — MAX98357 underruns as loud static, not silence.
    talkAudioWriteSilence(TALK_I2S_BUF_SAMPLES, 20);
    return;
  }

  // Speaker path first so AEC sees the same block that hits the DAC.
  size_t have = 0;
  if (playPriming_) {
    const uint32_t queuedMs = talkPlayRingUsedMs();
    const uint32_t lastRx = lastAgentAudioMs_;
    const bool primed = queuedMs >= kPlayPrimeMs;
    const bool streamIdle =
        lastRx != 0 && (millis() - lastRx) >= kPlayPrimeFlushMs;
    if (!talkPlayRingEmpty() && (primed || streamIdle)) playPriming_ = false;
  }
  if (!playPriming_) {
    have = talkPlayRingPop(pcmOut_, (size_t)n);
    if (have == 0) {
      playUnderruns_++;
      // Between turns only — mid-stream gaps zero-fill without a full re-prime.
      const uint32_t lastRx = lastAgentAudioMs_;
      if (lastRx == 0 || (millis() - lastRx) >= kPlayPrimeFlushMs) {
        playPriming_ = true;
      }
    }
  }
  if (have < (size_t)n) {
    memset(pcmOut_ + have, 0, ((size_t)n - have) * sizeof(int16_t));
  }
  talkAudioWritePcmTimeout(pcmOut_, n, 20);
  talkAecPushReference(pcmOut_, (size_t)n);

  if (!agentReady_) return;

  talkAecProcess(micTmp_, (size_t)n);
  micRingPush(micTmp_, (size_t)n);

  static uint32_t lastStatMs = 0;
  const uint32_t now = millis();
  if (now - lastStatMs > 4000) {
    lastStatMs = now;
    Serial.printf("[dg-agent] play ring=%lums under=%lu lowskip=%lu\n",
                  (unsigned long)talkPlayRingUsedMs(),
                  (unsigned long)playUnderruns_,
                  (unsigned long)playLowSkips_);
  }
}

void audioTaskFn(void *arg) {
  (void)arg;
  Serial.printf("[dg-agent] audio task core %d\n", (int)xPortGetCoreID());
  while (run_ || duplexUp_) {
    if (duplexUp_) {
      pumpAudio();
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!run_ && !agentReady_) break;
  }
  audioTask_ = nullptr;
  vTaskDelete(nullptr);
}

void wsTaskFn(void *arg) {
  (void)arg;
  active_ = true;
  wsConnected_ = false;
  gotWelcome_ = false;
  agentReady_ = false;
  settingsSent_ = false;
  duplexUp_ = false;
  playPriming_ = true;
  agentSpeaking_ = false;
  contextDirty_ = false;
  clearTexts();
  setStatus("Connecting Wi-Fi…");

  voiceContextReset(&voiceCtx_);
  if (Storage *st = sessionStorage()) {
    if (voiceContextLoad(st, kVoiceContextDeepgramId, &voiceCtx_)) {
      Serial.printf("[dg-agent] context loaded len=%u\n",
                    (unsigned)voiceCtx_.len);
    }
  }

  voiceToolsReset();
  voiceToolsRegisterDefaults();
  voiceImageToolReset();
  voiceImageToolSetStorage(sessionStorage());
  voiceImageToolSetHost(sessionHost_);
  voiceImageToolRegister();
  Serial.printf("[dg-agent] tools registered (visualise=%d)\n",
                settingsVisualiseEnabled() ? 1 : 0);

  if (!connectWifi()) {
    setStatus("Wi-Fi failed");
    goto done;
  }

  {
    const char *key = getConfig(Config::DeepgramApiKey);
    if (!key[0]) {
      setStatus("No Deepgram key");
      goto done;
    }
    snprintf(authHeader_, sizeof(authHeader_), "Token %s", key);
    snprintf(extraHdr_, sizeof(extraHdr_), "Authorization: %s", authHeader_);
  }

  {
    IPAddress ip;
    if (!wifiResolveHost(kHost, ip, 8000)) {
      setStatus("DNS failed");
      goto done;
    }
  }

  // TLS before I2S: duplex DMA eats the contiguous internal RAM the handshake
  // needs. Failed reconnects with I2S already up were start_ssl_client: -1.
  logHeap("pre-ws");
  setStatus("Connecting agent…");
  ws_.onEvent(onWsEvent);
  // Reconnect interval 0 = attempt immediately. (A large interval with
  // _lastConnectionFail==0 after begin() blocks connect while millis() <
  // interval — that was the "Connecting… timeout" with no SSL attempt.)
  // Session ends on disconnect (run_=false); we do not stay in a retry loop.
  ws_.setReconnectInterval(0);
  ws_.beginSSL(kHost, 443, kPath, "", "");
  ws_.setExtraHeaders(extraHdr_);
  Serial.printf("[dg-agent] key len=%u\n", (unsigned)strlen(authHeader_) - 6);

  {
    const uint32_t t0 = millis();
    uint32_t lastKeepAlive = millis();
    int pcmFill = 0;
    playUnderruns_ = 0;
    playLowSkips_ = 0;
    while (run_) {
      // Drain RX hard — Deepgram TTS is many tiny frames; one loop() per tick
      // left the play ring starving while sendBIN blocked on TLS.
      for (int i = 0; i < 4; i++) ws_.loop();
      pollAsyncTools();

      if (agentReady_ && ws_.isConnected()) {
        const uint32_t ringMs = talkPlayRingUsedMs();
        const bool playThin =
            agentSpeaking_ ||
            (lastAgentAudioMs_ != 0 &&
             (millis() - lastAgentAudioMs_) < kPlayPrimeFlushMs);
        if (playThin && ringMs < kPlayLowWaterMs) {
          // Prefer filling the jitter buffer over mic uplink this tick.
          playLowSkips_++;
        } else {
          const size_t got = micRingPop(micSend_ + pcmFill,
                                        (size_t)(kChunkSamples - pcmFill));
          pcmFill += (int)got;
          if (pcmFill >= kChunkSamples) {
            sendPcmChunk(micSend_, kChunkSamples);
            pcmFill = 0;
            lastKeepAlive = millis();
            // Pull any audio that arrived during the TLS write.
            for (int i = 0; i < 3; i++) ws_.loop();
          }
        }
        if (millis() - lastKeepAlive >= kKeepAliveMs) {
          sendKeepAlive();
          lastKeepAlive = millis();
        }
      } else if (ws_.isConnected() && millis() - lastKeepAlive >= kKeepAliveMs) {
        sendKeepAlive();
        lastKeepAlive = millis();
      }

      if (!wsConnected_ && !ws_.isConnected() && (millis() - t0) > 20000) {
        setStatus("Connect timeout");
        break;
      }

      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }

  if (ws_.isConnected()) ws_.disconnect();
  ws_.setReconnectInterval(0);

done:
  persistContext();
  voiceToolsReset();
  voiceImageToolReset();
  duplexUp_ = false;
  for (int i = 0; i < 50 && audioTask_; i++) delay(10);
  talkAudioStop();
  micRingClear();
  wsConnected_ = false;
  agentReady_ = false;
  gotWelcome_ = false;
  setStatus("Idle");
  active_ = false;
  wsTask_ = nullptr;
  vTaskDelete(nullptr);
}

} // namespace

bool dgAgentStart(AppHost *host) {
  if (active_ || wsTask_) return false;
  sessionHost_ = host;
  run_ = true;
  duplexUp_ = false;
  agentReady_ = false;

  BaseType_t aok =
      xTaskCreatePinnedToCore(audioTaskFn, "dg_audio", 4096, nullptr, 20,
                              &audioTask_, 1);
  if (aok != pdPASS) {
    run_ = false;
    audioTask_ = nullptr;
    setStatus("Audio task fail");
    return false;
  }

  BaseType_t wok =
      xTaskCreatePinnedToCore(wsTaskFn, "dg_ws", 16384, nullptr, 5, &wsTask_,
                              0);
  if (wok != pdPASS) {
    run_ = false;
    for (int i = 0; i < 50 && audioTask_; i++) delay(10);
    wsTask_ = nullptr;
    setStatus("WS task fail");
    return false;
  }
  return true;
}

void dgAgentStop() {
  run_ = false;
  duplexUp_ = false;
  if (ws_.isConnected()) ws_.disconnect();
  for (int i = 0; i < 100 && (wsTask_ || audioTask_); i++) delay(20);
  persistContext();
  if (!wsTask_ && !audioTask_) {
    active_ = false;
    agentReady_ = false;
    if (strcmp(status_, "Idle") != 0) setStatus("Idle");
  }
}

bool dgAgentActive() { return active_ || wsTask_ != nullptr; }
bool dgAgentReady() { return agentReady_; }
const char *dgAgentStatus() { return status_; }
const char *dgAgentUserText() { return userText_; }
const char *dgAgentAgentText() { return agentText_; }
uint32_t dgAgentGen() { return gen_; }
