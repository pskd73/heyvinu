#pragma once

#include <stddef.h>
#include <stdint.h>

struct Max17048Sample {
  bool ok = false;
  uint8_t addr = 0;
  char scanSummary[96] = {};
  char error[32] = {};

  uint16_t vcellRaw = 0;
  uint16_t socRaw = 0;
  uint16_t mode = 0;
  uint16_t version = 0;
  uint16_t hibrt = 0;
  uint16_t config = 0;
  uint16_t valrt = 0;
  uint16_t crate = 0;
  uint16_t vreset = 0;
  uint16_t chipId = 0;
  uint16_t status = 0;

  float cellVolts = 0.f;
  float socPercent = 0.f;
  float cratePercentHr = 0.f;
};

/** MAX17048 fuel gauge on Heyvinu I2C bus. */
class Max17048 {
public:
  bool begin();
  bool ready() const { return ready_; }
  uint8_t address() const { return addr_; }

  bool sample(Max17048Sample &out);

private:
  bool probe(uint8_t addr);
  uint16_t readReg(uint8_t reg, bool *ok = nullptr);
  void scanBus(char *out, size_t outLen);

  bool ready_ = false;
  uint8_t addr_ = 0;
};
