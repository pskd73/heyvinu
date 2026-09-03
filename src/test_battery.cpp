#include "test_app.h"

#include "battery.h"

#include <stdio.h>

void TestApp::formatBatterySummary() {
  const BatterySnapshot snap = Battery::snapshot();
  if (!snap.ok) {
    snprintf(battSummary_, sizeof(battSummary_), "Battery\nNo gauge");
    return;
  }

  snprintf(battSummary_, sizeof(battSummary_), "Battery  %.0f%%\n%s",
           snap.percent, Battery::chargeLabel(snap.charge));
}

void TestApp::formatBatteryDetail() {
  const BatterySnapshot snap = Battery::snapshot();
  if (!snap.ok) {
    snprintf(battDetail_, sizeof(battDetail_),
             "Status: %s\nExpected: 0x36\nI2C scan: %s",
             snap.raw.error[0] ? snap.raw.error : "No data",
             snap.raw.scanSummary);
    return;
  }

  snprintf(battDetail_, sizeof(battDetail_),
           "%.3f V\n"
           "SOC raw 0x%04X\n"
           "CRATE %+.2f %%/hr (0x%04X)\n"
           "MODE 0x%04X  VER 0x%04X\n"
           "HIBRT 0x%04X  CFG 0x%04X\n"
           "VALRT 0x%04X  VRESET 0x%04X\n"
           "CHIPID 0x%04X  STATUS 0x%04X\n"
           "I2C %s",
           snap.volts, snap.raw.socRaw, snap.cratePercentHr, snap.raw.crate,
           snap.raw.mode, snap.raw.version, snap.raw.hibrt, snap.raw.config,
           snap.raw.valrt, snap.raw.vreset, snap.raw.chipId, snap.raw.status,
           snap.raw.scanSummary);
}

void TestApp::enterBattery() {
  formatBatterySummary();
  formatBatteryDetail();
}

void TestApp::onBatteryTick(UINode &node, float dt) {
  (void)dt;
  if (!self_) {
    return;
  }

  const BatterySnapshot snap = Battery::snapshot();
  const bool changed =
      snap.ok != self_->lastBattOk_ ||
      snap.raw.socRaw != self_->lastSocRaw_ ||
      snap.raw.vcellRaw != self_->lastVcellRaw_ ||
      snap.raw.crate != self_->lastCrate_ || snap.charge != self_->lastCharge_;

  self_->formatBatterySummary();
  self_->formatBatteryDetail();

  UIDiv &div = static_cast<UIDiv &>(node);
  if (div.childCount() >= 1 && div.child(0)) {
    static_cast<UIText *>(div.child(0))->setText(self_->battSummary_);
  }
  if (div.childCount() >= 2 && div.child(1)) {
    static_cast<UIText *>(div.child(1))->setText(self_->battDetail_);
  }

  if (changed) {
    self_->lastBattOk_ = snap.ok;
    self_->lastSocRaw_ = snap.raw.socRaw;
    self_->lastVcellRaw_ = snap.raw.vcellRaw;
    self_->lastCrate_ = snap.raw.crate;
    self_->lastCharge_ = snap.charge;
    self_->page().invalidateContent();
  }
}

void TestApp::buildBattery(Page &page) {
  const Theme::ThemeTokens &th = Theme::active();
  const uint16_t muted = Theme::lerp(th.baseContent, th.base100, 0.35f);

  page.add(page.div()
               .onTick(onBatteryTick)
               .style(Style()
                          .setWidth(Length::Pct(100))
                          .setPadding(Edges(12, 10))
                          .setGap(8)
                          .setColumns(1))
               .add(page.text(battSummary_)
                        .style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setFont(FontRole::Body)
                                   .setColor(th.baseContent)
                                   .setAlign(Align::Start)))
               .add(page.text(battDetail_)
                        .style(Style()
                                   .setWidth(Length::Pct(100))
                                   .setFont(FontRole::Small)
                                   .setColor(muted)
                                   .setAlign(Align::Start))));
}
