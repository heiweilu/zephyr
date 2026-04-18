/*
 * app_ai_vision.h — one-way AI Vision entry point.
 *
 * Repurposed from face_recognize/main.c. Once called, owns the device
 * forever: state machine LIVE → UPLOADING → RESULT, BOOT-driven capture,
 * Qwen-VL HTTPS upload, CosyVoice TTS playback. There is no return path
 * in launcher_full phase 2e — exiting requires a reset.
 */

#ifndef APP_AI_VISION_H_
#define APP_AI_VISION_H_

int app_ai_vision_run(void);

#endif /* APP_AI_VISION_H_ */
