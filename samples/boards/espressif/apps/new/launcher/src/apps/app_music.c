/*
 * app_music.c — Embedded PCM player
 *
 * Plays a 30-second 16 kHz / mono / 16-bit PCM clip baked into firmware
 * via generate_inc_file_for_target() (see CMakeLists.txt). Streaming via
 * audio_stream_* in 320-sample (20 ms) chunks from a worker thread.
 * UI uses the same light-neumorphic palette as the AI assistant screen.
 */

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "../app_manager.h"
#include "../audio.h"

/* ── Embedded PCM (raw 16-bit signed mono @16 kHz, header stripped) ── */
static const uint8_t song1_pcm[] = {
#include "song1_30s.pcm.inc"
};
#define SONG1_TOTAL_SAMPLES (sizeof(song1_pcm) / sizeof(int16_t))
#define SONG1_TOTAL_MS      (SONG1_TOTAL_SAMPLES * 1000U / 16000U)

/* ── Light neumorphic palette (matches AI assistant screen) ── */
#define CLR_BG          0xE6E9EF
#define CLR_CARD        0xF3F5F9
#define CLR_TITLE       0x1F2937
#define CLR_SUBTITLE    0x6B7280
#define CLR_ACCENT      0xA855F7   /* Music app brand purple */
#define CLR_ACCENT_SOFT 0xC084FC
#define CLR_ACCENT_DARK 0x7E22CE
#define CLR_BTN_BG      0xFFFFFF
#define CLR_PROGRESS_BG 0xD1D5DB

/* ── Player worker ── */
#define CHUNK_SAMPLES 320  /* 20 ms @16 kHz */
static K_THREAD_STACK_DEFINE(music_stack, 4096);
static struct k_thread music_thread;
static volatile bool music_should_stop;
static volatile bool music_running;
static volatile uint32_t music_pos_samples;

/* ── UI handles ── */
static lv_obj_t *play_btn;
static lv_obj_t *play_label;
static lv_obj_t *progress_bar;
static lv_obj_t *time_label;
static lv_timer_t *ui_timer;

static void format_time(uint32_t ms, char *out, size_t cap)
{
	uint32_t s = ms / 1000U;
	snprintk(out, cap, "%02u:%02u / 00:%02u",
		 (unsigned)(s / 60U), (unsigned)(s % 60U),
		 (unsigned)(SONG1_TOTAL_MS / 1000U));
}

static void ui_tick(lv_timer_t *t)
{
	(void)t;
	if (!progress_bar || !time_label) {
		return;
	}
	uint32_t pos = music_pos_samples;
	int pct = (int)((uint64_t)pos * 100 / SONG1_TOTAL_SAMPLES);
	if (pct > 100) {
		pct = 100;
	}
	lv_bar_set_value(progress_bar, pct, LV_ANIM_OFF);

	char buf[32];
	uint32_t ms = (uint32_t)((uint64_t)pos * 1000U / 16000U);
	if (ms > SONG1_TOTAL_MS) {
		ms = SONG1_TOTAL_MS;
	}
	format_time(ms, buf, sizeof(buf));
	lv_label_set_text(time_label, buf);

	/* Auto-revert button label when worker finishes */
	if (!music_running && play_label) {
		lv_label_set_text(play_label, LV_SYMBOL_PLAY);
	}
}

static void music_play_thread(void *a, void *b, void *c)
{
	const int16_t *samples = (const int16_t *)song1_pcm;
	size_t total = SONG1_TOTAL_SAMPLES;
	size_t pos = 0;

	if (audio_stream_start() != 0) {
		printk("[Music] audio_stream_start failed\n");
		music_running = false;
		return;
	}

	while (pos < total && !music_should_stop) {
		size_t left = total - pos;
		int n = (left > CHUNK_SAMPLES) ? CHUNK_SAMPLES : (int)left;
		int written = audio_stream_feed(&samples[pos], n);
		if (written < 0) {
			printk("[Music] feed err %d @%u\n", written, (unsigned)pos);
			break;
		}
		if (written == 0) {
			/* Stream became inactive (likely stop requested) */
			break;
		}
		pos += written;
		music_pos_samples = pos;
	}

	audio_stream_stop();
	music_running = false;
	printk("[Music] playback ended (%u/%u samples)\n",
	       (unsigned)pos, (unsigned)total);
}

static void music_start(void)
{
	if (music_running) {
		return;
	}
	int ret = audio_codec_init();
	if (ret) {
		printk("[Music] audio_codec_init failed: %d\n", ret);
		return;
	}
	music_should_stop = false;
	music_running = true;
	music_pos_samples = 0;
	k_thread_create(&music_thread, music_stack,
			K_THREAD_STACK_SIZEOF(music_stack),
			music_play_thread, NULL, NULL, NULL,
			5, 0, K_NO_WAIT);
	k_thread_name_set(&music_thread, "music_play");
	if (play_label) {
		lv_label_set_text(play_label, LV_SYMBOL_STOP);
	}
}

static void music_stop(void)
{
	if (!music_running) {
		return;
	}
	music_should_stop = true;
	k_thread_join(&music_thread, K_SECONDS(2));
	if (play_label) {
		lv_label_set_text(play_label, LV_SYMBOL_PLAY);
	}
}

/* ── LVGL callbacks ── */
static void play_cb(lv_event_t *e)
{
	if (music_running) {
		music_stop();
	} else {
		music_start();
	}
}

static void back_cb(lv_event_t *e)
{
	music_stop();
	app_manager_back_to_home();
}

static void on_create(lv_obj_t *screen)
{
	lv_obj_set_style_bg_color(screen, lv_color_hex(CLR_BG), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

	/* Title */
	lv_obj_t *title = lv_label_create(screen);
	lv_label_set_text(title, "Music");
	lv_obj_set_style_text_color(title, lv_color_hex(CLR_TITLE), 0);
	lv_obj_set_style_text_font(title, &lv_font_source_han_sans_sc_16_cjk, 0);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

	/* Track name */
	lv_obj_t *track = lv_label_create(screen);
	lv_label_set_text(track, LV_SYMBOL_AUDIO "  song1  \xC2\xB7  30s");
	lv_obj_set_style_text_color(track, lv_color_hex(CLR_TITLE), 0);
	lv_obj_set_style_text_font(track, &lv_font_source_han_sans_sc_16_cjk, 0);
	lv_obj_align(track, LV_ALIGN_TOP_MID, 0, 60);

	/* Progress bar */
	progress_bar = lv_bar_create(screen);
	lv_obj_set_size(progress_bar, 240, 6);
	lv_obj_align(progress_bar, LV_ALIGN_TOP_MID, 0, 96);
	lv_obj_set_style_bg_color(progress_bar, lv_color_hex(CLR_PROGRESS_BG), 0);
	lv_obj_set_style_bg_color(progress_bar, lv_color_hex(CLR_ACCENT), LV_PART_INDICATOR);
	lv_bar_set_range(progress_bar, 0, 100);
	lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);

	/* Time */
	time_label = lv_label_create(screen);
	lv_label_set_text(time_label, "00:00 / 00:30");
	lv_obj_set_style_text_color(time_label, lv_color_hex(CLR_SUBTITLE), 0);
	lv_obj_set_style_text_font(time_label, &lv_font_source_han_sans_sc_16_cjk, 0);
	lv_obj_align(time_label, LV_ALIGN_TOP_MID, 0, 112);

	/* Round Play/Stop */
	play_btn = lv_button_create(screen);
	lv_obj_set_size(play_btn, 80, 80);
	lv_obj_align(play_btn, LV_ALIGN_TOP_MID, 0, 138);
	lv_obj_set_style_radius(play_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(play_btn, lv_color_hex(CLR_ACCENT), 0);
	lv_obj_set_style_bg_color(play_btn, lv_color_hex(CLR_ACCENT_DARK), LV_STATE_PRESSED);
	lv_obj_set_style_border_width(play_btn, 0, 0);
	lv_obj_add_event_cb(play_btn, play_cb, LV_EVENT_CLICKED, NULL);

	play_label = lv_label_create(play_btn);
	lv_label_set_text(play_label,
			  music_running ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
	lv_obj_set_style_text_color(play_label, lv_color_hex(0xFFFFFF), 0);
	lv_obj_set_style_text_font(play_label, &lv_font_source_han_sans_sc_16_cjk, 0);
	lv_obj_center(play_label);

	/* Back pill */
	lv_obj_t *back_btn = lv_button_create(screen);
	lv_obj_set_size(back_btn, 100, 32);
	lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
	lv_obj_set_style_radius(back_btn, 16, 0);
	lv_obj_set_style_bg_color(back_btn, lv_color_hex(CLR_BTN_BG), 0);
	lv_obj_set_style_border_width(back_btn, 0, 0);
	lv_obj_add_event_cb(back_btn, back_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *back_lbl = lv_label_create(back_btn);
	lv_label_set_text(back_lbl, LV_SYMBOL_LEFT "  Home");
	lv_obj_set_style_text_color(back_lbl, lv_color_hex(CLR_TITLE), 0);
	lv_obj_set_style_text_font(back_lbl, &lv_font_source_han_sans_sc_16_cjk, 0);
	lv_obj_center(back_lbl);

	lv_group_t *g = app_manager_get_kb_group();
	if (g) {
		lv_group_add_obj(g, play_btn);
		lv_group_add_obj(g, back_btn);
	}

	ui_timer = lv_timer_create(ui_tick, 200, NULL);
}

static void on_destroy(void)
{
	music_stop();
	if (ui_timer) {
		lv_timer_del(ui_timer);
		ui_timer = NULL;
	}
	play_btn = NULL;
	play_label = NULL;
	progress_bar = NULL;
	time_label = NULL;
}

const app_info_t app_music = {
	.name = "Music",
	.icon_color = LV_COLOR_MAKE(0xA8, 0x55, 0xF7),
	.icon_symbol = LV_SYMBOL_AUDIO,
	.on_create = on_create,
	.on_destroy = on_destroy,
};
