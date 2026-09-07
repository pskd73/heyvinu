#pragma once

class AppHost;

/**
 * Always-on local wake for the launcher (microWakeWord / Hey Vinu).
 *
 * Fully on-device TFLite Micro — no cloud STT. Stop before Ask / Talk so I2S
 * is free for the voice agent.
 */

bool wakeWordStart(AppHost *host = nullptr);
void wakeWordStop();
bool wakeWordActive();

/** One-shot: true after a wake hit until consumed. */
bool wakeWordTakeDetected();
