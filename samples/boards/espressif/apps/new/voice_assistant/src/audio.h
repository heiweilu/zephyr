/*
 * audio.h — ES8311 DAC + ES7210 ADC + I2S record/playback + streaming
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Sample rates */
#define RECORD_SAMPLE_RATE   16000
#define PLAYBACK_SAMPLE_RATE 16000   /* resample 24kHz→16kHz before play */
#define SAMPLE_BITS          16
#define NUM_CHANNELS         2       /* I2S stereo; mono extracted in SW */

/* Recording limits */
#define MAX_RECORD_SECONDS   5
#define MAX_RECORD_SAMPLES   (MAX_RECORD_SECONDS * RECORD_SAMPLE_RATE)  /* mono */
#define MAX_RECORD_PCM_BYTES (MAX_RECORD_SAMPLES * sizeof(int16_t))     /* 160000 */

/* Playback limits (after 24kHz→16kHz resample) */
#define MAX_PLAYBACK_SECONDS 15
#define MAX_PLAYBACK_SAMPLES (MAX_PLAYBACK_SECONDS * PLAYBACK_SAMPLE_RATE)
#define MAX_PLAYBACK_PCM_BYTES (MAX_PLAYBACK_SAMPLES * sizeof(int16_t)) /* 480000 */

/* Initialize audio codecs (ES8311 + ES7210) and configure I2S (once). */
int audio_codec_init(void);

/* Enable/disable PA (speaker amplifier) */
void audio_pa_enable(int on);

/* Record mono PCM (16kHz, 16-bit) while button held.
 * Returns number of mono samples recorded, or negative on error. */
int audio_record(int16_t *pcm_buf, int max_samples, volatile bool *stop_flag);

/* Play mono PCM (16kHz, 16-bit) through ES8311 speaker (blocking).
 * Returns 0 on success. */
int audio_play(const int16_t *pcm_buf, int num_samples);

/* ── Streaming playback API ── */

/* Begin streaming playback. Enables PA and starts I2S TX thread.
 * Call audio_stream_feed() to push samples, then audio_stream_stop(). */
int audio_stream_start(void);

/* Feed 16kHz mono PCM samples into the streaming playback buffer.
 * Can be called from any thread while stream is active.
 * Returns number of samples actually consumed (<= count if buffer is full). */
int audio_stream_feed(const int16_t *samples, int count);

/* Signal that no more data will be fed. Waits for remaining audio to play out. */
int audio_stream_stop(void);

/* Resample 24kHz mono → 16kHz mono (simple linear interpolation).
 * Returns number of output samples. */
int audio_resample_24k_to_16k(const int16_t *in, int in_samples,
			      int16_t *out, int max_out);

#endif /* AUDIO_H */
