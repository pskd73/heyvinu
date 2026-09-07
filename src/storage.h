#pragma once

#include <Flow32.h>

/** Heyvinu J1 — 1-bit SDMMC (CLK=39, CMD=38, D0=40). */
inline StorageConfig SdHeyvinu() {
  StorageConfig c;
  c.id = "Heyvinu-J1";
  c.bus = SdBus::Sdmmc;
  c.mountPoint = "/sdcard";
  c.pinClk = 39;
  c.pinCmd = 38;
  c.pinD0 = 40;
  c.sdmmc1bit = true;
  return c;
}
