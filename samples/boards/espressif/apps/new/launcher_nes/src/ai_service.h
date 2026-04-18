/*
 * ai_service.h — AI voice assistant backend service
 *
 * Manages WiFi connection, audio codecs, recording, TLS/SSE to DashScope,
 * streaming playback, and chat message history. Runs in a dedicated thread.
 */

#ifndef AI_SERVICE_H
#define AI_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

/* AI service states */
enum ai_state {
	AI_STATE_INIT,
	AI_STATE_WIFI_CONNECTING,
	AI_STATE_READY,
	AI_STATE_RECORDING,
	AI_STATE_PROCESSING,
	AI_STATE_STREAMING,
	AI_STATE_PLAYING,
	AI_STATE_ERROR,
};

/* Chat message */
#define AI_MAX_MESSAGES  20
#define AI_MSG_TEXT_SIZE  512

struct ai_chat_msg {
	bool is_user;
	char text[AI_MSG_TEXT_SIZE];
};

/* Initialize AI service (starts background thread: WiFi + audio + BOOT btn).
 * Safe to call once; subsequent calls are no-ops. */
int ai_service_init(void);

/* Get current state */
enum ai_state ai_service_get_state(void);

/* Trigger recording start (from UI mic button).
 * Only effective when state == AI_STATE_READY. */
void ai_service_start_recording(void);

/* Signal recording stop (from UI mic button or BOOT release). */
void ai_service_stop_recording(void);

/* Chat history access (thread-safe reads) */
int ai_service_get_msg_count(void);
const struct ai_chat_msg *ai_service_get_msg(int idx);

/* Live streaming response text (updated during STREAMING state) */
const char *ai_service_get_live_text(void);
size_t ai_service_get_live_text_len(void);

/* WiFi status */
bool ai_service_wifi_connected(void);

#endif /* AI_SERVICE_H */
