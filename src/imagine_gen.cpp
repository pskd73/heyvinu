#include "imagine_gen.h"
#include "gallery.h"
#include "imagine_config.h"
#include "imagine_fs.h"
#include "net_wifi.h"
#include "runtime_config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace {

int b64Value(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }
  return -1;
}

const char kB64Enc[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool clientWriteAll(WiFiClientSecure &client, const char *data, size_t len) {
  size_t off = 0;
  uint32_t stallDeadline = millis() + 30000;
  while (off < len) {
    size_t n = client.write(reinterpret_cast<const uint8_t *>(data) + off,
                            len - off);
    if (n > 0) {
      off += n;
      stallDeadline = millis() + 30000;
      continue;
    }
    if (!client.connected()) {
      return false;
    }
    if (millis() > stallDeadline) {
      return false;
    }
    delay(2);
    yield();
  }
  return true;
}

bool streamFileAsBase64(WiFiClientSecure &client, File &f, size_t fileSize) {
  static const size_t kInChunk = 768;
  uint8_t inBuf[kInChunk];
  char outBuf[(kInChunk / 3) * 4];
  size_t remaining = fileSize;
  size_t sentRaw = 0;
  uint32_t lastLog = millis();

  while (remaining > 0) {
    size_t want = remaining > kInChunk ? kInChunk : remaining;
    if (want < remaining) {
      want -= (want % 3);
      if (want == 0) {
        want = 3;
      }
    }
    size_t got = f.read(inBuf, want);
    if (got != want) {
      Serial.printf("ERR ref read want=%u got=%u\n", (unsigned)want,
                    (unsigned)got);
      return false;
    }
    remaining -= got;
    sentRaw += got;

    size_t outLen = 0;
    size_t i = 0;
    while (i + 3 <= got) {
      uint8_t a = inBuf[i], b = inBuf[i + 1], c = inBuf[i + 2];
      outBuf[outLen++] = kB64Enc[a >> 2];
      outBuf[outLen++] = kB64Enc[((a & 0x03) << 4) | (b >> 4)];
      outBuf[outLen++] = kB64Enc[((b & 0x0F) << 2) | (c >> 6)];
      outBuf[outLen++] = kB64Enc[c & 0x3F];
      i += 3;
    }
    if (i < got) {
      uint8_t a = inBuf[i];
      outBuf[outLen++] = kB64Enc[a >> 2];
      if (i + 1 < got) {
        uint8_t b = inBuf[i + 1];
        outBuf[outLen++] = kB64Enc[((a & 0x03) << 4) | (b >> 4)];
        outBuf[outLen++] = kB64Enc[(b & 0x0F) << 2];
        outBuf[outLen++] = '=';
      } else {
        outBuf[outLen++] = kB64Enc[(a & 0x03) << 4];
        outBuf[outLen++] = '=';
        outBuf[outLen++] = '=';
      }
    }

    if (!clientWriteAll(client, outBuf, outLen)) {
      Serial.printf("ERR b64 write at %u/%u\n", (unsigned)sentRaw,
                    (unsigned)fileSize);
      return false;
    }

    if (millis() - lastLog > 1500) {
      lastLog = millis();
      Serial.printf("edit upload %u/%u\n", (unsigned)sentRaw,
                    (unsigned)fileSize);
    }
    yield();
  }
  Serial.printf("edit upload done %u bytes\n", (unsigned)fileSize);
  return true;
}

struct B64ToFile {
  File *f = nullptr;
  size_t len = 0;
  int sextet[4];
  int n = 0;
  bool done = false;
  bool overflow = false;
  uint8_t wbuf[B64_WRITE_CHUNK];
  size_t wlen = 0;

  bool flush() {
    if (!f || wlen == 0) {
      return true;
    }
    size_t wrote = f->write(wbuf, wlen);
    if (wrote != wlen) {
      overflow = true;
      return false;
    }
    wlen = 0;
    return true;
  }

  bool pushBytes(const uint8_t *p, int count) {
    for (int i = 0; i < count; ++i) {
      if (wlen >= sizeof(wbuf)) {
        if (!flush()) {
          return false;
        }
      }
      wbuf[wlen++] = p[i];
      len++;
    }
    return true;
  }

  bool feed(char ch) {
    if (done || overflow) {
      return !overflow;
    }
    if (ch == '"') {
      done = true;
      return flush();
    }
    if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') {
      return true;
    }
    if (ch == '=') {
      if (n >= 2) {
        uint8_t out[2];
        out[0] = static_cast<uint8_t>((sextet[0] << 2) | (sextet[1] >> 4));
        int count = 1;
        if (n > 2) {
          out[1] = static_cast<uint8_t>((sextet[1] << 4) | (sextet[2] >> 2));
          count = 2;
        }
        if (!pushBytes(out, count)) {
          return false;
        }
      }
      done = true;
      n = 0;
      return flush();
    }
    int v = b64Value(ch);
    if (v < 0) {
      return true;
    }
    sextet[n++] = v;
    if (n == 4) {
      uint8_t out[3];
      out[0] = static_cast<uint8_t>((sextet[0] << 2) | (sextet[1] >> 4));
      out[1] = static_cast<uint8_t>((sextet[1] << 4) | (sextet[2] >> 2));
      out[2] = static_cast<uint8_t>((sextet[2] << 6) | sextet[3]);
      n = 0;
      return pushBytes(out, 3);
    }
    return true;
  }
};

bool clientReadChar(WiFiClientSecure &client, char &out, uint32_t deadline) {
  while (millis() < deadline) {
    if (client.available()) {
      int v = client.read();
      if (v < 0) {
        return false;
      }
      out = static_cast<char>(v);
      return true;
    }
    if (!client.connected() && !client.available()) {
      return false;
    }
    delay(1);
    yield();
  }
  return false;
}

bool skipHttpHeaders(WiFiClientSecure &client, bool &chunked, int &contentLen,
                     uint32_t deadline) {
  chunked = false;
  contentLen = -1;
  bool first = true;
  String line;
  line.reserve(96);
  while (millis() < deadline) {
    line = "";
    while (millis() < deadline) {
      char c;
      if (!clientReadChar(client, c, deadline)) {
        return false;
      }
      if (c == '\r') {
        continue;
      }
      if (c == '\n') {
        break;
      }
      if (line.length() < 120) {
        line += c;
      }
    }
    if (line.length() == 0) {
      return true;
    }
    if (first) {
      first = false;
    }
    line.toLowerCase();
    if (line.startsWith("transfer-encoding:") && line.indexOf("chunked") >= 0) {
      chunked = true;
    }
    if (line.startsWith("content-length:")) {
      contentLen = line.substring(15).toInt();
    }
  }
  return false;
}

bool matchNeedleFeed(char c, const char *needle, size_t nlen, size_t &m) {
  if (c == needle[m]) {
    m++;
    if (m == nlen) {
      m = 0;
      return true;
    }
  } else if (c == needle[0]) {
    m = 1;
  } else {
    m = 0;
  }
  return false;
}

bool streamDecodeB64FieldToFile(WiFiClientSecure &client, bool chunked,
                                 int contentLen, B64ToFile &dec,
                                 uint32_t deadline) {
  const char *needle = "\"b64_json\":\"";
  const size_t nlen = 12;
  size_t match = 0;
  bool inValue = false;

  auto consume = [&](char c) -> bool {
    if (!inValue) {
      if (matchNeedleFeed(c, needle, nlen, match)) {
        inValue = true;
      }
      return true;
    }
    return dec.feed(c);
  };

  if (chunked) {
    while (millis() < deadline && !dec.done && !dec.overflow) {
      String sizeLine;
      while (millis() < deadline) {
        char c;
        if (!clientReadChar(client, c, deadline)) {
          return dec.len > 0;
        }
        if (c == '\r') {
          continue;
        }
        if (c == '\n') {
          break;
        }
        sizeLine += c;
      }
      long chunk = strtol(sizeLine.c_str(), nullptr, 16);
      if (chunk <= 0) {
        break;
      }
      for (long i = 0; i < chunk; ++i) {
        char c;
        if (!clientReadChar(client, c, deadline)) {
          return false;
        }
        if (!consume(c)) {
          return false;
        }
        if (dec.done) {
          return dec.flush() && dec.len > 0;
        }
      }
      char drop;
      clientReadChar(client, drop, deadline);
      if (drop == '\r') {
        clientReadChar(client, drop, deadline);
      }
    }
  } else {
    int remaining = contentLen;
    while (millis() < deadline && !dec.done && !dec.overflow) {
      if (contentLen >= 0 && remaining == 0) {
        break;
      }
      char c;
      if (!clientReadChar(client, c, deadline)) {
        break;
      }
      if (remaining > 0) {
        remaining--;
      }
      if (!consume(c)) {
        return false;
      }
      if (dec.done) {
        return dec.flush() && dec.len > 0;
      }
    }
  }
  return dec.flush() && dec.len > 0 && !dec.overflow;
}

} // namespace

bool imagineGenerate(const char *promptIn, char *outPath, size_t outLen,
                    const char *referencePath) {
  if (outPath && outLen) {
    outPath[0] = '\0';
  }
  const char *openrouterKey = getConfig(Config::OpenrouterApiKey);
  if (!openrouterKey || strlen(openrouterKey) < 8 ||
      strcmp(openrouterKey, "REPLACE_ME") == 0 ||
      strncmp(openrouterKey, "sk-or-v1-...", 12) == 0) {
    Serial.println("ERR set OpenRouter config (Remote or defaults)");
    return false;
  }

  char prompt[224];
  {
    String tmp = promptIn ? promptIn : "";
    tmp.trim();
    if (tmp.length() > 220) {
      tmp = tmp.substring(0, 220);
    }
    if (tmp.length() < 2) {
      return false;
    }
    tmp.replace("\\", "\\\\");
    tmp.replace("\"", "\\\"");
    tmp.replace("\n", " ");
    tmp.replace("\r", " ");
    strncpy(prompt, tmp.c_str(), sizeof(prompt) - 1);
    prompt[sizeof(prompt) - 1] = '\0';
  }

  if (WiFi.status() != WL_CONNECTED && !connectWifi()) {
    Serial.println("ERR WiFi for image gen");
    return false;
  }

  const bool editing = referencePath && referencePath[0];
  File refFile;
  size_t refSize = 0;
  bool refJpeg = true;
  if (editing) {
    if (!imgReady()) {
      Serial.println("ERR image FS not ready");
      return false;
    }
    refFile = imgOpen(referencePath, FILE_READ);
    if (!refFile || refFile.size() < 4) {
      Serial.printf("ERR reference open %s\n", referencePath);
      if (refFile) {
        refFile.close();
      }
      return false;
    }
    refSize = refFile.size();
    uint8_t magic[4] = {0};
    refFile.read(magic, 4);
    refFile.seek(0);
    refJpeg = magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF;
    bool refPng =
        magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G';
    if (!refJpeg && !refPng) {
      Serial.println("ERR reference not jpeg/png");
      refFile.close();
      return false;
    }
  }

  String promptField =
      editing ? String(prompt) : (String("Illustration of: ") + prompt);

  String bodyPrefix = String("{\"model\":\"") + IMAGE_MODEL +
                      "\",\"prompt\":\"" + promptField +
                      "\",\"n\":1,\"aspect_ratio\":\"4:3\",\"resolution\":\"1K\""
                      ",\"output_format\":\"jpeg\",\"output_compression\":80";
  String bodySuffix;
  size_t contentLen = 0;
  size_t b64Len = 0;

  if (editing) {
    const char *mime = refJpeg ? "image/jpeg" : "image/png";
    bodyPrefix +=
        String(",\"input_references\":[{\"type\":\"image_url\",\"image_url\":{"
               "\"url\":\"data:") +
        mime + ";base64,";
    bodySuffix = "\"}}]}";
    b64Len = 4 * ((refSize + 2) / 3);
    contentLen = bodyPrefix.length() + b64Len + bodySuffix.length();
  } else {
    bodyPrefix += "}";
    contentLen = bodyPrefix.length();
  }

  Serial.printf("image gen model=%s edit=%d ref=%u body=%u heap=%u\n",
                IMAGE_MODEL, (int)editing, (unsigned)refSize,
                (unsigned)contentLen, (unsigned)ESP.getFreeHeap());

  if (!imgReady()) {
    if (refFile) {
      refFile.close();
    }
    Serial.println("ERR image FS not ready");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30);
  client.setTimeout(120);

  Serial.println("image gen: TLS connect...");
  if (!client.connect(OR_HOST, 443, 20000)) {
    Serial.println("image gen: TLS retry...");
    client.stop();
    delay(250);
    if (!client.connect(OR_HOST, 443, 20000)) {
      Serial.println("ERR OpenRouter TLS failed");
      if (refFile) {
        refFile.close();
      }
      return false;
    }
  }
  Serial.println("image gen: TLS ok, sending body...");

  client.print(String("POST ") + OR_IMAGE_PATH + " HTTP/1.1\r\n");
  client.print(String("Host: ") + OR_HOST + "\r\n");
  client.print(String("Authorization: Bearer ") + openrouterKey + "\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("HTTP-Referer: https://chitram.local\r\n");
  client.print("X-Title: Chitram\r\n");
  client.print(String("Content-Length: ") + contentLen + "\r\n");
  client.print("Connection: close\r\n\r\n");

  if (!clientWriteAll(client, bodyPrefix.c_str(), bodyPrefix.length())) {
    Serial.println("ERR body prefix write");
    if (refFile) {
      refFile.close();
    }
    client.stop();
    return false;
  }
  bodyPrefix = "";
  promptField = "";

  if (editing) {
    if (!streamFileAsBase64(client, refFile, refSize)) {
      Serial.println("ERR reference base64 stream");
      refFile.close();
      client.stop();
      return false;
    }
    refFile.close();
    if (!clientWriteAll(client, bodySuffix.c_str(), bodySuffix.length())) {
      Serial.println("ERR body suffix write");
      client.stop();
      return false;
    }
    bodySuffix = "";
  }

  imgRemove(IMAGE_FS_PATH);
  File out = imgOpen(IMAGE_FS_PATH, FILE_WRITE);
  if (!out) {
    Serial.println("ERR cannot create image file");
    client.stop();
    return false;
  }

  uint32_t deadline = millis() + 180000;
  Serial.println("image gen: waiting for response...");

  String status;
  while (millis() < deadline) {
    char c;
    if (!clientReadChar(client, c, deadline)) {
      break;
    }
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      break;
    }
    if (status.length() < 80) {
      status += c;
    }
  }
  Serial.println(status);
  if (status.indexOf("200") < 0 && status.indexOf("201") < 0) {
    String err;
    while (millis() < deadline && err.length() < 220) {
      char c;
      if (!clientReadChar(client, c, deadline)) {
        break;
      }
      err += c;
    }
    Serial.println(err);
    client.stop();
    out.close();
    imgRemove(IMAGE_FS_PATH);
    return false;
  }

  bool chunked = false;
  int contentLenHdr = -1;
  if (!skipHttpHeaders(client, chunked, contentLenHdr, deadline)) {
    Serial.println("ERR headers");
    client.stop();
    out.close();
    imgRemove(IMAGE_FS_PATH);
    return false;
  }

  static B64ToFile dec;
  dec = B64ToFile{};
  dec.f = &out;
  if (!streamDecodeB64FieldToFile(client, chunked, contentLenHdr, dec,
                                  deadline)) {
    Serial.printf("ERR b64→fs len=%u overflow=%d\n", (unsigned)dec.len,
                  (int)dec.overflow);
    client.stop();
    out.close();
    imgRemove(IMAGE_FS_PATH);
    return false;
  }
  client.stop();
  out.close();

  Serial.printf("image file %u bytes heap=%u\n", (unsigned)dec.len,
                (unsigned)ESP.getFreeHeap());

  File peek = imgOpen(IMAGE_FS_PATH, FILE_READ);
  if (!peek || peek.size() < 4) {
    Serial.println("ERR empty image file");
    if (peek) {
      peek.close();
    }
    imgRemove(IMAGE_FS_PATH);
    return false;
  }
  uint8_t magic[4] = {0};
  peek.read(magic, 4);
  peek.close();

  bool isJpeg = magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF;
  bool isPng =
      magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G';
  Serial.printf("magic jpeg=%d png=%d\n", (int)isJpeg, (int)isPng);

  if (!isJpeg && !isPng) {
    Serial.printf("ERR unknown magic %02X %02X %02X %02X\n", magic[0], magic[1],
                  magic[2], magic[3]);
    imgRemove(IMAGE_FS_PATH);
    return false;
  }

  char saved[40];
  if (!gallerySaveFromTemp(IMAGE_FS_PATH, isJpeg, saved, sizeof(saved))) {
    if (outPath && outLen) {
      strncpy(outPath, IMAGE_FS_PATH, outLen - 1);
      outPath[outLen - 1] = '\0';
    }
    return true;
  }

  if (outPath && outLen) {
    strncpy(outPath, saved, outLen - 1);
    outPath[outLen - 1] = '\0';
  }
  return true;
}
