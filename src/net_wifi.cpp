#include "net_wifi.h"
#include "runtime_config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <lwip/dns.h>
#include <lwip/ip_addr.h>

namespace {

void applyPublicDns() {
  // Router DNS on some Jio/home APs fails for api.deepgram.com.
  ip_addr_t d0;
  ip_addr_t d1;
  IP_ADDR4(&d0, 8, 8, 8, 8);
  IP_ADDR4(&d1, 1, 1, 1, 1);
  dns_setserver(0, &d0);
  dns_setserver(1, &d1);
}

} // namespace

bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    applyPublicDns();
    WiFi.setSleep(false);
    return true;
  }
  Serial.printf("WiFi: connecting to %s...\n", getConfig(Config::WifiSsid));
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(getConfig(Config::WifiSsid), getConfig(Config::WifiPassword));
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 20000) {
    delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi: failed");
    return false;
  }
  applyPublicDns();
  // Optional Arduino helper when available — keeps STA IP, swaps DNS only.
  WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(),
              IPAddress(8, 8, 8, 8), IPAddress(1, 1, 1, 1));
  Serial.printf("WiFi: ok %s dns=8.8.8.8\n", WiFi.localIP().toString().c_str());
  return true;
}

void disconnectWifi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }

bool wifiResolveHost(const char *host, IPAddress &out, uint32_t timeoutMs) {
  out = IPAddress();
  if (!host || !host[0] || WiFi.status() != WL_CONNECTED) {
    return false;
  }
  applyPublicDns();
  const uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (WiFi.hostByName(host, out) == 1 && out != IPAddress()) {
      Serial.printf("DNS: %s -> %s\n", host, out.toString().c_str());
      return true;
    }
    delay(250);
    applyPublicDns();
  }
  Serial.printf("DNS: failed for %s\n", host);
  return false;
}
