# Local wake word (microWakeWord)

Cloud STT is **not** used for wake. Detection runs on-device with TFLite Micro.

## Current model: Hey Vinu

Embedded from `~/Downloads/hey_vinu`:

| File | Role |
|------|------|
| `src/wake_models/hey_vinu_model_data.h` | FlatBuffer bytes in firmware |
| `src/wake_models/hey_vinu.json` | Manifest (cutoff, window, arena) |
| `lib/tflite-micro` | TFLite Micro (PlatformIO port) |
| `lib/esp-micro-speech-features` | Mel frontend matching training |

Wake phrase on the mic is **“Hey Vinu”**. Say that on the launcher to open Ask.

## Tuning

Edit constants in `src/wake_word.cpp` (sourced from the JSON):

- `kProbCutoff` — lower = more sensitive
- `kSlidingWindow` — more frames = smoother, slower
