#include "test_app.h"

#include "net_wifi.h"
#include "runtime_config.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <stdio.h>
#include <string.h>

namespace {

bool ssidMatchesConfigured() {
  const char *want = getConfig(Config::WifiSsid);
  if (!want || !want[0]) {
    return false;
  }
  return WiFi.SSID().equals(want);
}

} // namespace

void TestApp::formatWifiSummary() {
  switch (wifiPhase_) {
  case WifiTestPhase::Connecting:
    snprintf(wifiSummary_, sizeof(wifiSummary_), "Wi-Fi\nConnecting...");
    break;
  case WifiTestPhase::Connected:
  case WifiTestPhase::HttpsTesting:
    snprintf(wifiSummary_, sizeof(wifiSummary_), "Wi-Fi\nConnected");
    break;
  case WifiTestPhase::Pass:
    snprintf(wifiSummary_, sizeof(wifiSummary_), "Wi-Fi\nPass");
    break;
  case WifiTestPhase::Fail:
    snprintf(wifiSummary_, sizeof(wifiSummary_), "Wi-Fi\nFail");
    break;
  }
}

void TestApp::formatWifiDetail() {
  const char *cfgSsid = getConfig(Config::WifiSsid);
  if (!cfgSsid[0]) {
    snprintf(wifiDetail_, sizeof(wifiDetail_),
             "No SSID in config.\nSet via Remote app.");
    return;
  }

  if (wifiPhase_ == WifiTestPhase::Connecting) {
    snprintf(wifiDetail_, sizeof(wifiDetail_),
             "Config SSID: %s\n\nJoining network...", cfgSsid);
    return;
  }

  if (wifiPhase_ == WifiTestPhase::Fail &&
      WiFi.status() != WL_CONNECTED) {
    snprintf(wifiDetail_, sizeof(wifiDetail_),
             "Config SSID: %s\n\nCould not connect within 20 s.",
             cfgSsid);
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    snprintf(wifiDetail_, sizeof(wifiDetail_),
             "Config SSID: %s\n"
             "Joined SSID: %s\n"
             "IP: %s\n"
             "RSSI: %d dBm\n",
             cfgSsid, WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
             WiFi.RSSI());
  } else {
    snprintf(wifiDetail_, sizeof(wifiDetail_), "Config SSID: %s\nNot connected.",
             cfgSsid);
  }

  const size_t used = strlen(wifiDetail_);
  if (wifiPhase_ == WifiTestPhase::HttpsTesting) {
    snprintf(wifiDetail_ + used, sizeof(wifiDetail_) - used,
             "\nHTTPS google.com\nProbing...");
    return;
  }

  if (wifiPhase_ == WifiTestPhase::Pass) {
    snprintf(wifiDetail_ + used, sizeof(wifiDetail_) - used,
             "\nHTTPS google.com\nOK (HTTP %d)", wifiHttpCode_);
    return;
  }

  if (wifiPhase_ == WifiTestPhase::Fail && wifiHttpCode_ != 0) {
    snprintf(wifiDetail_ + used, sizeof(wifiDetail_) - used,
             "\nHTTPS google.com\nFailed (HTTP %d)", wifiHttpCode_);
    return;
  }

  if (wifiPhase_ == WifiTestPhase::Fail) {
    snprintf(wifiDetail_ + used, sizeof(wifiDetail_) - used,
             "\nJoined SSID does not match config.");
  }
}

void TestApp::enterWifi() {
  configInit();
  wifiPhase_ = WifiTestPhase::Connecting;
  wifiPhaseMs_ = millis();
  wifiHttpCode_ = 0;

  const char *ssid = getConfig(Config::WifiSsid);
  if (!ssid[0]) {
    wifiPhase_ = WifiTestPhase::Fail;
    formatWifiSummary();
    formatWifiDetail();
    return;
  }

  if (WiFi.status() == WL_CONNECTED && ssidMatchesConfigured()) {
    wifiPhase_ = WifiTestPhase::Connected;
    formatWifiSummary();
    formatWifiDetail();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, getConfig(Config::WifiPassword));
  formatWifiSummary();
  formatWifiDetail();
}

bool TestApp::runHttpsProbe(int &httpCode) {
  httpCode = 0;
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, "https://google.com")) {
    return false;
  }
  httpCode = http.GET();
  http.end();
  return httpCode > 0;
}

void TestApp::runWifiStep() {
  switch (wifiPhase_) {
  case WifiTestPhase::Connecting:
    if (WiFi.status() == WL_CONNECTED) {
      if (!ssidMatchesConfigured()) {
        wifiPhase_ = WifiTestPhase::Fail;
        break;
      }
      wifiPhase_ = WifiTestPhase::Connected;
      break;
    }
    if (millis() - wifiPhaseMs_ > 20000) {
      wifiPhase_ = WifiTestPhase::Fail;
    }
    break;

  case WifiTestPhase::Connected:
    wifiPhase_ = WifiTestPhase::HttpsTesting;
    break;

  case WifiTestPhase::HttpsTesting: {
    const bool ok = runHttpsProbe(wifiHttpCode_);
    wifiPhase_ = ok ? WifiTestPhase::Pass : WifiTestPhase::Fail;
    break;
  }

  case WifiTestPhase::Pass:
  case WifiTestPhase::Fail:
    break;
  }
}

void TestApp::onWifiTick(UINode &node, float dt) {
  (void)dt;
  if (!self_) {
    return;
  }

  const WifiTestPhase prev = self_->wifiPhase_;
  self_->runWifiStep();

  self_->formatWifiSummary();
  self_->formatWifiDetail();

  UIDiv &div = static_cast<UIDiv &>(node);
  if (div.childCount() >= 1 && div.child(0)) {
    static_cast<UIText *>(div.child(0))->setText(self_->wifiSummary_);
  }
  if (div.childCount() >= 2 && div.child(1)) {
    static_cast<UIText *>(div.child(1))->setText(self_->wifiDetail_);
  }

  if (self_->wifiPhase_ != prev) {
    self_->page().invalidateContent();
  }
}

void TestApp::buildWifi(Page &page) {
  const Theme::ThemeTokens &th = Theme::active();
  const uint16_t muted = Theme::lerp(th.baseContent, th.base100, 0.35f);
  uint16_t summaryColor = th.baseContent;
  if (wifiPhase_ == WifiTestPhase::Pass) {
    summaryColor = th.success;
  } else if (wifiPhase_ == WifiTestPhase::Fail) {
    summaryColor = th.warning;
  }

  page.add(page.div()
               .onTick(onWifiTick)
               .style(Style()
                          .setWidth(Length::Pct(100))
                          .setPadding(Edges(12, 10))
                          .setGap(8)
                          .setColumns(1))
               .add(page.text(wifiSummary_)
                        .style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setFont(FontRole::Body)
                                   .setColor(summaryColor)
                                   .setAlign(Align::Start)))
               .add(page.text(wifiDetail_)
                        .style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setFont(FontRole::Small)
                                   .setColor(muted)
                                   .setAlign(Align::Start))));
}
