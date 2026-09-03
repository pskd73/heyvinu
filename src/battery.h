#pragma once

#include "max17048.h"

#include <stdint.h>

enum class ChargeState : uint8_t {
  Unknown,
  Charging,
  Full,
  Discharging,
  Idle,
};

struct BatterySnapshot {
  bool ok = false;
  float percent = 0.f;
  float volts = 0.f;
  float cratePercentHr = 0.f;
  ChargeState charge = ChargeState::Unknown;
  Max17048Sample raw{};
};

/** MAX17048-backed battery percent + charge state. Poll from main loop. */
class Battery {
public:
  static void begin();
  static void poll(float dtSec);

  static bool ready();
  static BatterySnapshot snapshot();
  static const char *chargeLabel(ChargeState state);
  /** Lucide icon for nav bar. */
  static const char *chargeIcon(ChargeState state);

private:
  static ChargeState inferCharge(const Max17048Sample &s);
  static bool readChargerGpio(ChargeState &out);

  static Max17048 gauge_;
  static BatterySnapshot snap_;
  static float pollAccum_;
  static bool begun_;
};
