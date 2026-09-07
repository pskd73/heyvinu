#include "remote_server.h"

#include "runtime_config.h"

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>

namespace {

static constexpr const char *kApSsid = "Heyvinu-Remote";
static constexpr const char *kApPassword = "heyvinusetup";

WebServer *server = nullptr;
bool running = false;
char lastMsg[96] = "Ready";

void setMsg(const char *msg) {
  strncpy(lastMsg, msg ? msg : "", sizeof(lastMsg) - 1);
  lastMsg[sizeof(lastMsg) - 1] = '\0';
}

static String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    const char c = in.charAt(i);
    if (c == '&') {
      out += "&amp;";
    } else if (c == '<') {
      out += "&lt;";
    } else if (c == '"') {
      out += "&quot;";
    } else {
      out += c;
    }
  }
  return out;
}

static String buildFormPage() {
  String page;
  page.reserve(4096);
  page += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Heyvinu Remote</title>"
            "<style>"
            "body{font-family:system-ui,sans-serif;margin:16px;max-width:520px}"
            "label{display:block;margin-top:12px;font-weight:600}"
            "input{width:100%;box-sizing:border-box;padding:8px;margin-top:4px}"
            "button{margin-top:18px;padding:10px 16px;font-size:16px}"
            ".hint{color:#666;font-size:14px;margin-top:8px}"
            "</style></head><body>"
            "<h1>Heyvinu Remote</h1>"
            "<p class='hint'>Config is saved on device and persists across reboots.</p>"
            "<form method='POST' action='/save'>");

  for (uint8_t i = 0; i < Config::Count; i++) {
    const Config::Entry &e = Config::kEntries[i];
    page += "<label for='";
    page += e.name;
    page += "'>";
    page += e.label;
    page += "</label><input id='";
    page += e.name;
    page += "' name='";
    page += e.name;
    page += "' type='";
    page += e.masked ? "password" : "text";
    page += "' value='";
    if (!e.masked) {
      page += htmlEscape(getConfig(e.name));
    }
    page += "' autocomplete='off'>";
  }

  page += F("<button type='submit'>Save config</button></form></body></html>");
  return page;
}

static void handleRoot() {
  server->send(200, "text/html", buildFormPage());
}

static void handleSave() {
  bool any = false;
  for (uint8_t i = 0; i < Config::Count; i++) {
    const Config::Entry &e = Config::kEntries[i];
    if (!server->hasArg(e.name)) {
      continue;
    }
    any = true;
    if (!setConfig(e.name, server->arg(e.name).c_str(), true)) {
      server->send(500, "text/plain", "Config save failed");
      setMsg("Save failed");
      return;
    }
  }

  if (!any) {
    server->send(400, "text/plain", "Missing form fields");
    return;
  }

  Serial.println("Config updated");
  setMsg("Config saved");
  String ok = F("<!DOCTYPE html><html><body><h1>Saved</h1>"
                "<p>Config saved — persists across reboots.</p>"
                "<p><a href='/'>Back</a></p></body></html>");
  server->send(200, "text/html", ok);
}

} // namespace

bool remoteServerStart(RemoteServerInfo &info) {
  if (running) {
    strncpy(info.apSsid, kApSsid, sizeof(info.apSsid) - 1);
    strncpy(info.apPassword, kApPassword, sizeof(info.apPassword) - 1);
    strncpy(info.ip, WiFi.softAPIP().toString().c_str(), sizeof(info.ip) - 1);
    info.running = true;
    return true;
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(kApSsid, kApPassword)) {
    Serial.println("Remote: softAP failed");
    setMsg("AP failed");
    info.running = false;
    return false;
  }

  if (!server) {
    server = new WebServer(80);
  }
  server->on("/", HTTP_GET, handleRoot);
  server->on("/save", HTTP_POST, handleSave);
  server->begin();

  running = true;
  setMsg("AP running");

  strncpy(info.apSsid, kApSsid, sizeof(info.apSsid) - 1);
  info.apSsid[sizeof(info.apSsid) - 1] = '\0';
  strncpy(info.apPassword, kApPassword, sizeof(info.apPassword) - 1);
  info.apPassword[sizeof(info.apPassword) - 1] = '\0';
  strncpy(info.ip, WiFi.softAPIP().toString().c_str(), sizeof(info.ip) - 1);
  info.ip[sizeof(info.ip) - 1] = '\0';
  info.running = true;

  Serial.printf("Remote AP: ssid=%s ip=%s\n", info.apSsid, info.ip);
  return true;
}

void remoteServerStop() {
  if (server) {
    server->stop();
  }
  if (running) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  running = false;
  setMsg("Stopped");
}

void remoteServerLoop() {
  if (server && running) {
    server->handleClient();
  }
}

bool remoteServerRunning() { return running; }

void remoteServerGetInfo(RemoteServerInfo &info) {
  memset(&info, 0, sizeof(info));
  if (!running) {
    info.running = false;
    return;
  }
  strncpy(info.apSsid, kApSsid, sizeof(info.apSsid) - 1);
  strncpy(info.apPassword, kApPassword, sizeof(info.apPassword) - 1);
  strncpy(info.ip, WiFi.softAPIP().toString().c_str(), sizeof(info.ip) - 1);
  info.running = true;
}

const char *remoteServerLastMessage() { return lastMsg; }
