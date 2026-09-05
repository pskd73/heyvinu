# Local wake word (microWakeWord)

Cloud STT is **not** used for wake. Detection runs on-device with TFLite Micro.

## Current model: Hey Luna

Embedded from `~/Downloads/hey_luna`:

| File | Role |
|------|------|
| `src/wake_models/hey_luna_model_data.h` | FlatBuffer bytes in firmware |
| `src/wake_models/hey_luna.json` | Manifest (cutoff, window, arena) |
| `lib/tflite-micro` | TFLite Micro (PlatformIO port) |
| `lib/esp-micro-speech-features` | Mel frontend matching training |

Wake phrase on the mic is **“Hey Luna”** (not “Hey Chitram” yet). Say that on the
launcher to open Ask.

## Tuning

Edit constants in `src/wake_word.cpp` (sourced from the JSON):

- `kProbCutoff` — lower = more sensitive
- `kSlidingWindow` — more frames = smoother, slower

## Swap to Hey Chitram later

1. Train at https://microwakeword.com / microWakeWord trainer with
   `WAKE_WORD = "Hey Chitram"`.
2. Replace `hey_luna.tflite` / regenerate `hey_luna_model_data.h` (or rename).
3. Update `kProbCutoff` / arena from the new JSON if different.
