#pragma once

#include <Flow32.h>

/** App-side panel profiles — not part of the Flow32 library. */

/**
 * 1.83" rounded IPS — 240×284 visible (Panel183 / ST7789).
 *
 * Chitram J2 wiring:
 *
 *   LCD    ESP32-S3
 *   ----   --------
 *   VCC    3V3
 *   GND    GND
 *   DIN    41  (MOSI)
 *   CLK    42  (SCLK)
 *   CS     21
 *   DC     14
 *   RST    2
 *   BL     1
 */
inline DisplayPanel Panel183() {
  DisplayPanel p;
  p.id = "Panel183";
  p.chip = PanelChip::ST7789;
  p.width = 240;
  p.height = 284;
  p.rotation = 0;
  p.cornerRadius = 44;
  p.gramWidth = 240;
  p.gramHeight = 320;
  p.panelYOffset = 36;
  p.spiHz = 80000000;
  p.uiScale = 1.0f;
  p.pinMosi = 41;
  p.pinSclk = 42;
  p.pinCs = 21;
  p.pinDc = 14;
  p.pinRst = 2;
  p.pinBl = 1;
  return p;
}

/**
 * SmartElex 1.69" rounded IPS — 280×240 landscape (ST7789V2/V3).
 *
 * Same class as Waveshare 1.69" / Robu R257187. Controller GRAM is 240×320;
 * visible area starts at row 20 (panelYOffset).
 *
 * Chitram J2 wiring (unchanged from Panel183):
 *
 *   LCD    ESP32-S3
 *   ----   --------
 *   VCC    3V3
 *   GND    GND
 *   DIN    41  (MOSI)
 *   CLK    42  (SCLK)
 *   CS     21
 *   DC     14
 *   RST    2
 *   BL     1
 */
inline DisplayPanel Panel169() {
  DisplayPanel p;
  p.id = "Panel169";
  p.chip = PanelChip::ST7789;
  p.width = 280;
  p.height = 240;
  p.rotation = 1; // landscape (180° from previous)
  p.cornerRadius = 38;
  p.gramWidth = 240;
  p.gramHeight = 280; // visible area — Adafruit centers in 320-row GRAM
  p.panelYOffset = 0;
  // 80 MHz is above the ST7789 rated write clock and glitches when the speaker
  // amp and Wi-Fi radio are both loaded.
  p.spiHz = 40000000;
  p.uiScale = 1.0f;
  p.pinMosi = 41;
  p.pinSclk = 42;
  p.pinCs = 21;
  p.pinDc = 14;
  p.pinRst = 2;
  p.pinBl = 1;
  return p;
}
