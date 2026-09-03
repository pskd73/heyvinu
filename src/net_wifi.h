#pragma once

#include <IPAddress.h>
#include <stdint.h>

bool connectWifi();
void disconnectWifi();
bool wifiConnected();
/** Resolve host with public DNS + retries. */
bool wifiResolveHost(const char *host, IPAddress &out, uint32_t timeoutMs = 8000);
