#include "talk_context.h"

#include <Flow32.h>

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr const char *kDir = "/chitram";
constexpr const char *kContextDir = "/chitram/context";

void *psramOrRam(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = malloc(n);
  return p;
}

bool safeAgentId(const char *agentId, char *out, size_t outLen) {
  if (!agentId || !agentId[0] || !out || outLen < 2) return false;
  size_t n = 0;
  for (const char *p = agentId; *p && n + 1 < outLen; p++) {
    const char c = *p;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    out[n++] = ok ? c : '_';
  }
  out[n] = '\0';
  return n > 0;
}

bool contextBase(Storage *storage, const char *agentId, char *out,
                 size_t outLen) {
  if (!storage || !storage->ready()) return false;
  char safe[48];
  if (!safeAgentId(agentId, safe, sizeof(safe))) return false;
  char rel[80];
  snprintf(rel, sizeof(rel), "%s/%s", kContextDir, safe);
  return storage->absPath(rel, out, outLen);
}

bool ensureDirs(Storage *storage) {
  if (!storage || !storage->ready()) return false;
  if (!storage->exists(kDir) && !storage->mkdir(kDir)) return false;
  if (!storage->exists(kContextDir) && !storage->mkdir(kContextDir))
    return false;
  return true;
}

const char *roleLabel(TalkTurnRole role) {
  return role == TalkTurnRole::User ? "User: " : "Agent: ";
}

/** Append `piece` into rolling buffer; drop oldest bytes (prefer newline). */
void appendRolling(char *buf, size_t cap, size_t *lenInOut, const char *piece,
                   size_t pieceLen) {
  if (!buf || !lenInOut || cap < 2) return;
  size_t len = *lenInOut;
  if (pieceLen >= cap - 1) {
    memcpy(buf, piece + (pieceLen - (cap - 1)), cap - 1);
    buf[cap - 1] = '\0';
    *lenInOut = cap - 1;
    return;
  }
  if (len + pieceLen >= cap) {
    size_t needDrop = len + pieceLen - (cap - 1);
    size_t drop = needDrop;
    while (drop < len && buf[drop] != '\n') drop++;
    if (drop < len) drop++;
    else drop = needDrop;
    while (drop < len &&
           (static_cast<unsigned char>(buf[drop]) & 0xC0) == 0x80) {
      drop++;
    }
    memmove(buf, buf + drop, len - drop);
    len -= drop;
    buf[len] = '\0';
  }
  memcpy(buf + len, piece, pieceLen);
  len += pieceLen;
  buf[len] = '\0';
  *lenInOut = len;
}

} // namespace

bool talkContextEnsure(TalkContext *state) {
  if (!state) return false;
  if (state->text) return true;
  state->text = (char *)psramOrRam(kTalkContextCap);
  if (!state->text) return false;
  state->text[0] = '\0';
  state->len = 0;
  return true;
}

void talkContextReset(TalkContext *state) {
  if (!state) return;
  state->conversationId[0] = '\0';
  state->updatedMs = 0;
  state->len = 0;
  if (state->text) state->text[0] = '\0';
}

void talkContextFree(TalkContext *state) {
  if (!state) return;
  if (state->text) {
    heap_caps_free(state->text);
    state->text = nullptr;
  }
  talkContextReset(state);
}

void talkContextAppend(TalkContext *state, TalkTurnRole role,
                       const char *text) {
  if (!state || !text || !text[0]) return;
  if (!talkContextEnsure(state)) return;

  const char *label = roleLabel(role);
  const size_t labelLen = strlen(label);
  const size_t textLen = strlen(text);

  // Skip a trailing newline on the source; we always add our own.
  size_t bodyLen = textLen;
  while (bodyLen > 0 &&
         (text[bodyLen - 1] == '\n' || text[bodyLen - 1] == '\r')) {
    bodyLen--;
  }
  if (bodyLen == 0) return;

  // Build turn into a small stack buffer when short; otherwise stream in parts.
  char stackTurn[384];
  const size_t need = labelLen + bodyLen + 1; // + '\n'
  if (need < sizeof(stackTurn)) {
    memcpy(stackTurn, label, labelLen);
    memcpy(stackTurn + labelLen, text, bodyLen);
    stackTurn[labelLen + bodyLen] = '\n';
    stackTurn[labelLen + bodyLen + 1] = '\0';
    appendRolling(state->text, kTalkContextCap, &state->len, stackTurn, need);
    return;
  }

  appendRolling(state->text, kTalkContextCap, &state->len, label, labelLen);
  appendRolling(state->text, kTalkContextCap, &state->len, text, bodyLen);
  appendRolling(state->text, kTalkContextCap, &state->len, "\n", 1);
}

bool talkContextLoad(Storage *storage, const char *agentId, TalkContext *out) {
  if (!out) return false;
  if (!talkContextEnsure(out)) return false;
  talkContextReset(out);

  char base[96];
  if (!contextBase(storage, agentId, base, sizeof(base))) return false;

  char metaPath[112];
  char textPath[112];
  snprintf(metaPath, sizeof(metaPath), "%s.json", base);
  snprintf(textPath, sizeof(textPath), "%s.txt", base);

  // Preferred: split meta + raw text (keeps ArduinoJson tiny).
  if (storage->exists(textPath)) {
    if (storage->exists(metaPath)) {
      File mf = storage->open(metaPath, FILE_READ);
      if (mf) {
        JsonDocument doc;
        if (!deserializeJson(doc, mf)) {
          const char *cid = doc["conversation_id"] | "";
          strncpy(out->conversationId, cid, sizeof(out->conversationId) - 1);
          out->updatedMs = doc["updated_ms"] | 0;
        }
        mf.close();
      }
    }
    File tf = storage->open(textPath, FILE_READ);
    if (!tf) return out->conversationId[0] != '\0';
    size_t n = tf.readBytes(out->text, kTalkContextCap - 1);
    tf.close();
    out->text[n] = '\0';
    out->len = n;
    return out->conversationId[0] || out->len > 0;
  }

  // Legacy single JSON with embedded "context" / "progress".
  if (!storage->exists(metaPath)) return false;
  File f = storage->open(metaPath, FILE_READ);
  if (!f) return false;
  const size_t sz = f.size();
  if (sz == 0 || sz > kTalkContextCap + 1024) {
    f.close();
    return false;
  }
  char *raw = (char *)psramOrRam(sz + 1);
  if (!raw) {
    f.close();
    return false;
  }
  const size_t got = f.readBytes(raw, sz);
  f.close();
  raw[got] = '\0';

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, raw);
  free(raw);
  if (err) return false;

  const char *cid = doc["conversation_id"] | "";
  const char *ctx = doc["context"] | "";
  if (!ctx[0]) ctx = doc["progress"] | "";
  strncpy(out->conversationId, cid, sizeof(out->conversationId) - 1);
  const size_t n = strlen(ctx);
  const size_t copy = n < kTalkContextCap - 1 ? n : kTalkContextCap - 1;
  memcpy(out->text, ctx, copy);
  out->text[copy] = '\0';
  out->len = copy;
  out->updatedMs = doc["updated_ms"] | 0;
  return out->conversationId[0] || out->len > 0;
}

bool talkContextSave(Storage *storage, const char *agentId,
                     const TalkContext &state) {
  if (!state.conversationId[0] && state.len == 0) return false;
  if (!state.text && state.len > 0) return false;
  if (!ensureDirs(storage)) return false;

  char base[96];
  if (!contextBase(storage, agentId, base, sizeof(base))) return false;

  char metaPath[112];
  char textPath[112];
  snprintf(metaPath, sizeof(metaPath), "%s.json", base);
  snprintf(textPath, sizeof(textPath), "%s.txt", base);

  {
    File mf = storage->open(metaPath, FILE_WRITE);
    if (!mf) return false;
    JsonDocument doc;
    doc["conversation_id"] = state.conversationId;
    doc["updated_ms"] = state.updatedMs ? state.updatedMs : millis();
    doc["len"] = state.len;
    const size_t n = serializeJson(doc, mf);
    mf.close();
    if (n == 0) return false;
  }

  File tf = storage->open(textPath, FILE_WRITE);
  if (!tf) return false;
  const size_t n =
      state.len ? tf.write((const uint8_t *)state.text, state.len) : 0;
  tf.close();
  return state.len == 0 || n == state.len;
}

bool talkContextClear(Storage *storage, const char *agentId) {
  char base[96];
  if (!contextBase(storage, agentId, base, sizeof(base))) return false;
  char metaPath[112];
  char textPath[112];
  snprintf(metaPath, sizeof(metaPath), "%s.json", base);
  snprintf(textPath, sizeof(textPath), "%s.txt", base);

  bool ok = true;
  if (storage->exists(metaPath)) {
    File f = storage->open(metaPath, FILE_WRITE);
    if (!f) ok = false;
    else {
      f.print("{}");
      f.close();
    }
  }
  if (storage->exists(textPath)) {
    File f = storage->open(textPath, FILE_WRITE);
    if (!f) ok = false;
    else {
      f.print("");
      f.close();
    }
  }
  return ok;
}
