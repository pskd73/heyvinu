#include "tls_mem.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

static void *tlsCalloc(size_t count, size_t size) {
  void *p = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) {
    p = heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  return p;
}

static void tlsFree(void *ptr) {
  if (ptr) heap_caps_free(ptr);
}

void tlsUsePsram() {
  if (mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree) != 0) {
    Serial.println("TLS: PSRAM allocator hook unavailable");
    return;
  }
  Serial.println("TLS: mbedTLS heap -> PSRAM");
}
