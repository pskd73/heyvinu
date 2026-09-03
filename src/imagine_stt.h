#pragma once

#include <stdint.h>

/**
 * Imagine listen path: Deepgram nova-3 @ 8 kHz linear16.
 * RX-only I2S on Chitram pins (BCLK=47 WS=48 DIN=3).
 * Stops I2S on imagineSttStop / imagineSttAbort.
 *
 * Do not run Ask + Imagine together — they share I2S_NUM_0.
 */
enum class ImagineSttState : uint8_t {
  Idle,
  Connecting,
  Listening,
  Done,
  Error,
};

bool imagineSttStart();
void imagineSttTick();
void imagineSttStop(bool finalize);
void imagineSttAbort();

ImagineSttState imagineSttState();
bool imagineSttListening();
const char *imagineSttText();
const char *imagineSttError();
