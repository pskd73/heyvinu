#pragma once

#define OR_HOST "openrouter.ai"
#define OR_IMAGE_PATH "/api/v1/images"
#define IMAGE_MODEL "google/gemini-3.1-flash-lite-image"
#define IMAGE_FS_PATH "/gen.bin"
#define B64_WRITE_CHUNK 256

#define DG_HOST "api.deepgram.com"
#define DG_PATH                                                                \
  "/v1/listen?encoding=linear16&sample_rate=8000&channels=1&model=nova-3&"     \
  "language=en&interim_results=true&punctuate=true&smart_format=true&"         \
  "endpointing=1000&utterance_end_ms=2500&vad_events=true"

#define IMAGINE_SAMPLE_RATE 8000
#define IMAGINE_MIC_RING_SECONDS 1
#define IMAGINE_MIC_RING_SAMPLES (IMAGINE_SAMPLE_RATE * IMAGINE_MIC_RING_SECONDS)
#define IMAGINE_STREAM_CHUNK_SAMPLES 800
