Import("env")
import pathlib

MAX_DATA = "(2 * 1024 * 1024)"  # 2MB — large TTS frames land in PSRAM


def patch_websockets(source, target, env):
    root = pathlib.Path(env["PROJECT_LIBDEPS_DIR"]) / env["PIOENV"] / "WebSockets"
    hdr = root / "src" / "WebSockets.h"
    cpp = root / "src" / "WebSockets.cpp"
    if not hdr.exists():
        print("patch_websockets: WebSockets.h not found yet")
        return

    h = hdr.read_text()
    if "ifndef WEBSOCKETS_MAX_DATA_SIZE" not in h:
        old = """#if defined(ESP8266) || defined(ESP32)

#define WEBSOCKETS_MAX_DATA_SIZE (15 * 1024)
#define WEBSOCKETS_USE_BIG_MEM"""
        new = f"""#if defined(ESP8266) || defined(ESP32)

#ifndef WEBSOCKETS_MAX_DATA_SIZE
#define WEBSOCKETS_MAX_DATA_SIZE {MAX_DATA}
#endif
#define WEBSOCKETS_USE_BIG_MEM"""
        if old in h:
            h = h.replace(old, new)
            print("patch_websockets: patched WebSockets.h MAX_DATA_SIZE")
        else:
            print("patch_websockets: WARN header pattern not found")
    else:
        print("patch_websockets: WebSockets.h already patched")

    if "#define WEBSOCKETS_TCP_TIMEOUT (5000)" in h:
        h = h.replace(
            "#define WEBSOCKETS_TCP_TIMEOUT (5000)",
            "#ifndef WEBSOCKETS_TCP_TIMEOUT\n#define WEBSOCKETS_TCP_TIMEOUT (20000)\n#endif",
        )
        print("patch_websockets: TCP timeout guard added")

    hdr.write_text(h)

    if not cpp.exists():
        return

    c = cpp.read_text()
    changed = False

    if "payload too big! (%u)" in c and "DROP payload" not in c:
        c = c.replace(
            """    if(header->payloadLen > WEBSOCKETS_MAX_DATA_SIZE) {
        DEBUG_WEBSOCKETS("[WS][%d][handleWebsocket] payload too big! (%u)\\n", client->num, header->payloadLen);
        clientDisconnect(client, 1009);
        return;
    }""",
            """    if(header->payloadLen > WEBSOCKETS_MAX_DATA_SIZE) {
        Serial.printf("[WS] DROP payload %u > max %u\\n", (unsigned)header->payloadLen, (unsigned)WEBSOCKETS_MAX_DATA_SIZE);
        DEBUG_WEBSOCKETS("[WS][%d][handleWebsocket] payload too big! (%u)\\n", client->num, header->payloadLen);
        clientDisconnect(client, 1009);
        return;
    }""",
        )
        changed = True
        print("patch_websockets: added payload-too-big Serial log")

    # RX payload → PSRAM (ElevenLabs TTS frames reach ~200KB).
    needle = "        payload = (uint8_t *)malloc(header->payloadLen + 1);"
    if needle in c and "ps_malloc(header->payloadLen" not in c:
        c = c.replace(
            needle,
            """#if defined(ESP32)
        payload = (uint8_t *)ps_malloc(header->payloadLen + 1);
        if(!payload) {
            payload = (uint8_t *)malloc(header->payloadLen + 1);
        }
#else
        payload = (uint8_t *)malloc(header->payloadLen + 1);
#endif""",
        )
        changed = True
        print("patch_websockets: RX payload → ps_malloc")

    if "WS RX frame" not in c and "payload = (uint8_t *)ps_malloc" in c:
        c = c.replace(
            """        if(!payload) {
            DEBUG_WEBSOCKETS("[WS][%d][handleWebsocket] to less memory to handle payload %d!\\n", client->num, header->payloadLen);
            clientDisconnect(client, 1011);
            return;
        }
        readCb(client, payload, header->payloadLen, std::bind(&WebSockets::handleWebsocketPayloadCb, this, std::placeholders::_1, std::placeholders::_2, payload));""",
            """        if(!payload) {
            Serial.printf("[WS] OOM payload %u\\n", (unsigned)header->payloadLen);
            DEBUG_WEBSOCKETS("[WS][%d][handleWebsocket] to less memory to handle payload %d!\\n", client->num, header->payloadLen);
            clientDisconnect(client, 1011);
            return;
        }
        if(header->payloadLen > 2048) {
            Serial.printf("[WS] RX frame %u bytes\\n", (unsigned)header->payloadLen);
        }
        readCb(client, payload, header->payloadLen, std::bind(&WebSockets::handleWebsocketPayloadCb, this, std::placeholders::_1, std::placeholders::_2, payload));""",
        )
        changed = True
        print("patch_websockets: added RX frame size log")

    # TX pack buffer → PSRAM. Only frames < 1400 bytes take this path, and it is
    # also the only path that masks: bigger frames ship an all-zero mask key.
    # TALK_MIC_CHUNK_SAMPLES is sized to keep us here.
    tx_old = "        uint8_t * dataPtr = (uint8_t *)malloc(length + WEBSOCKETS_MAX_HEADER_SIZE);"
    if tx_old in c and "ps_malloc(length + WEBSOCKETS_MAX_HEADER_SIZE)" not in c:
        c = c.replace(
            tx_old,
            """#if defined(ESP32)
        uint8_t * dataPtr = (uint8_t *)ps_malloc(length + WEBSOCKETS_MAX_HEADER_SIZE);
        if(!dataPtr) {
            dataPtr = (uint8_t *)malloc(length + WEBSOCKETS_MAX_HEADER_SIZE);
        }
#else
        uint8_t * dataPtr = (uint8_t *)malloc(length + WEBSOCKETS_MAX_HEADER_SIZE);
#endif""",
        )
        changed = True
        print("patch_websockets: TX pack buffer → ps_malloc")

    if changed:
        cpp.write_text(c)
    else:
        print("patch_websockets: WebSockets.cpp already patched")


patch_websockets(None, None, env)
