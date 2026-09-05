#pragma once

#include <stddef.h>
#include <stdint.h>

class AppHost;

/**
 * Deepgram Voice Agent — single WS for STT + LLM + TTS.
 * wss://agent.deepgram.com/v1/agent/converse
 *
 * Continuity: host->storage() holds /chitram/context/deepgram.* and is
 * reinjected via agent.context.messages on Settings (see voice_context).
 *
 * Client tools: `show_text` / `generate_image` via FunctionCallRequest →
 * voice_tools / voice_image_tool → FunctionCallResponse.
 */

bool dgAgentStart(AppHost *host = nullptr);
void dgAgentStop();
bool dgAgentActive();
bool dgAgentReady();

const char *dgAgentStatus();
/** Last user transcript from ConversationText. */
const char *dgAgentUserText();
/** Last assistant transcript from ConversationText. */
const char *dgAgentAgentText();
uint32_t dgAgentGen();
