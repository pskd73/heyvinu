#pragma once

#include <stdint.h>

struct RemoteServerInfo {
  char apSsid[32];
  char apPassword[32];
  char ip[16];
  bool running = false;
};

bool remoteServerStart(RemoteServerInfo &info);
void remoteServerStop();
void remoteServerLoop();

bool remoteServerRunning();
const char *remoteServerLastMessage();
void remoteServerGetInfo(RemoteServerInfo &info);
