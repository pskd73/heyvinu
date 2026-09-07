#include "battery.h"

#include "power_pins.h"

#include <Arduino.h>

Max17048 Battery::gauge_;
BatterySnapshot Battery::snap_{};
float Battery::pollAccum_ = 0.f;
bool Battery::begun_ = false;

namespace {

constexpr float kPollSec = 0.5f;
constexpr float kChargeCrateThreshold = 1.5f;
constexpr float kDischargeCrateThreshold = -1.5f;

} // namespace

void Battery::begin() {
  if (begun_) {
    return;
  }
  begun_ = true;
  gauge_.begin();
  gauge_.sample(snap_.raw);
  snap_.ok = snap_.raw.ok;
  if (snap_.ok) {
    snap_.percent = snap_.raw.socPercent;
    snap_.volts = snap_.raw.cellVolts;
    snap_.cratePercentHr = snap_.raw.cratePercentHr;
    snap_.charge = inferCharge(snap_.raw);
  }
}

void Battery::poll(float dtSec) {
  if (!begun_) {
    begin();
  }
  pollAccum_ += dtSec;
  if (pollAccum_ < kPollSec) {
    return;
  }
  pollAccum_ = 0.f;

  gauge_.sample(snap_.raw);
  snap_.ok = snap_.raw.ok;
  if (!snap_.ok) {
    snap_.charge = ChargeState::Unknown;
    return;
  }

  snap_.percent = snap_.raw.socPercent;
  snap_.volts = snap_.raw.cellVolts;
  snap_.cratePercentHr = snap_.raw.cratePercentHr;
  snap_.charge = inferCharge(snap_.raw);
}

bool Battery::ready() { return begun_ && snap_.ok; }

BatterySnapshot Battery::snapshot() { return snap_; }

const char *Battery::chargeLabel(ChargeState state) {
  switch (state) {
  case ChargeState::Charging:
    return "Charging";
  case ChargeState::Full:
    return "Full";
  case ChargeState::Discharging:
    return "Discharging";
  case ChargeState::Idle:
    return "On battery";
  default:
    return "Unknown";
  }
}

const char *Battery::chargeIcon(ChargeState state) {
  switch (state) {
  case ChargeState::Charging:
    return "battery-charging";
  case ChargeState::Full:
    return "battery-full";
  default:
    return "battery";
  }
}

bool Battery::readChargerGpio(ChargeState &out) {
  if (HeyvinuPower::kChrgPin < 0) {
    return false;
  }

  pinMode(HeyvinuPower::kChrgPin, INPUT_PULLUP);
  if (digitalRead(HeyvinuPower::kChrgPin) == LOW) {
    out = ChargeState::Charging;
    return true;
  }

  if (HeyvinuPower::kStdbyPin >= 0) {
    pinMode(HeyvinuPower::kStdbyPin, INPUT_PULLUP);
    if (digitalRead(HeyvinuPower::kStdbyPin) == LOW) {
      out = ChargeState::Full;
      return true;
    }
  }

  return false;
}

ChargeState Battery::inferCharge(const Max17048Sample &s) {
  if (!s.ok) {
    return ChargeState::Unknown;
  }

  ChargeState gpioState = ChargeState::Unknown;
  if (readChargerGpio(gpioState)) {
    return gpioState;
  }

  if (s.cratePercentHr >= kChargeCrateThreshold) {
    return ChargeState::Charging;
  }
  if (s.cratePercentHr <= kDischargeCrateThreshold) {
    return ChargeState::Discharging;
  }
  if (s.socPercent >= 97.f && s.cellVolts >= 4.10f) {
    return ChargeState::Full;
  }
  return ChargeState::Idle;
}
