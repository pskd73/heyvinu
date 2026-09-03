#pragma once

/**
 * Route mbedTLS allocations to PSRAM.
 *
 * The Arduino ESP32 core ships prebuilt ESP-IDF libraries, so
 * CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC in sdkconfig.defaults is ignored and every
 * TLS handshake takes its two ~16 KB content buffers from internal RAM. With
 * the Flow32 shell (icon/emoji indexes, framebuffer), I2S DMA and two task
 * stacks resident, the largest internal block sits near 32-36 KB and the
 * handshake fails with "SSL - Memory allocation failed".
 *
 * mbedTLS keeps the runtime allocator hook available, which gives the same
 * result as the missing sdkconfig option. Call once before any TLS use.
 */
void tlsUsePsram();
