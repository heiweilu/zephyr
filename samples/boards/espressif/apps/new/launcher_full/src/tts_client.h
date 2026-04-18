/*
 * tts_client.h — CosyVoice WebSocket TTS over TLS
 *
 * Step 1b-3b: speak the qwen-vl-plus caption via Aliyun CosyVoice
 * (cosyvoice-v3-flash). One call does TLS connect + WS upgrade +
 * run-task / continue-task / finish-task + audio_play().
 */
#ifndef TTS_CLIENT_H_
#define TTS_CLIENT_H_

/* Initialise (currently a no-op; CA cert already registered by ai_client). */
int tts_client_init(void);

/* Speak `text` (UTF-8, plain). Blocks until playback finishes.
 * Returns 0 on success, negative on error. */
int tts_client_speak(const char *text);

#endif /* TTS_CLIENT_H_ */
