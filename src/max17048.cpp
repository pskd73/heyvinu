#include "max17048.h"

#include "i2c_pins.h"

#include <Arduino.h>
#include <Wire.h>
#include <stdio.h>
#include <string.h>

bool Max17048::probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void Max17048::scanBus(char *out, size_t outLen) {
  if (!out || outLen == 0) {
    return;
  }
  out[0] = '\0';
  size_t used = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    if (!probe(addr)) {
      continue;
    }
    const int n = snprintf(out + used, outLen - used, used ? " 0x%02X" : "0x%02X",
                           addr);
    if (n <= 0 || static_cast<size_t>(n) >= outLen - used) {
      break;
    }
    used += static_cast<size_t>(n);
  }
  if (used == 0) {
    snprintf(out, outLen, "none");
  }
}

uint16_t Max17048::readReg(uint8_t reg, bool *ok) {
  if (ok) {
    *ok = false;
  }
  if (!ready_) {
    return 0xFFFF;
  }

  Wire.beginTransmission(addr_);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return 0xFFFF;
  }

  if (Wire.requestFrom(static_cast<int>(addr_), 2) != 2) {
    return 0xFFFF;
  }

  const uint8_t hi = Wire.read();
  const uint8_t lo = Wire.read();
  if (ok) {
    *ok = true;
  }
  return static_cast<uint16_t>((hi << 8) | lo);
}

bool Max17048::begin() {
  Wire.begin(HeyvinuI2c::kSda, HeyvinuI2c::kScl);
  Wire.setClock(100000);

  addr_ = HeyvinuI2c::kGaugeAddr;
  ready_ = probe(addr_);
  if (ready_) {
    Serial.printf("Gauge: MAX17048 @ 0x%02X\n", addr_);
    return true;
  }

  Serial.printf("Gauge: not found @ 0x%02X\n", addr_);
  return false;
}

bool Max17048::sample(Max17048Sample &out) {
  out = Max17048Sample{};
  scanBus(out.scanSummary, sizeof(out.scanSummary));

  if (!ready_) {
    snprintf(out.error, sizeof(out.error), "Not on I2C");
    return false;
  }

  out.addr = addr_;
  bool ok = false;

  out.vcellRaw = readReg(0x02, &ok);
  if (!ok) {
    snprintf(out.error, sizeof(out.error), "Read fail");
    return false;
  }
  out.socRaw = readReg(0x04, &ok);
  out.mode = readReg(0x06, &ok);
  out.version = readReg(0x08, &ok);
  out.hibrt = readReg(0x0A, &ok);
  out.config = readReg(0x0C, &ok);
  out.valrt = readReg(0x14, &ok);
  out.crate = readReg(0x16, &ok);
  out.vreset = readReg(0x18, &ok);
  out.chipId = readReg(0x19, &ok);
  out.status = readReg(0x1A, &ok);

  out.cellVolts = static_cast<float>(out.vcellRaw) * 78.125e-6f;
  out.socPercent = static_cast<float>(out.socRaw) / 256.f;
  out.cratePercentHr = static_cast<int16_t>(out.crate) * 0.208f;

  out.ok = true;
  return true;
}
