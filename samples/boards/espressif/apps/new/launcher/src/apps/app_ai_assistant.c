/*
 * app_ai_assistant.c — AI Assistant voice-call UI
 *
 * Layout (320×240):
 *   [Title bar 28px]  ← Back  |  AI Assistant
 *   [Center area 172px] circle indicator + status text
 *   [Action bar 40px]  [MIC button]   [END button]
 *
 * Polls ai_service state every 100ms via lv_timer to update UI.
 * Recording triggered by MIC button click or BOOT button (hardware).
 * No text display — AI responses are audio-only via CosyVoice TTS.
 */

#include <lvgl.h>
#include <zephyr/kernel.h>

#include "../app_manager.h"
#include "../ai_service.h"
#include "../ble_hid.h"
#include "../resource.h"

/* ── UI object pointers (valid only while app is active) ── */
static lv_obj_t *circle_obj;        /* Central status circle */
static lv_obj_t *circle_icon;       /* Icon inside circle */
static lv_obj_t *status_text;       /* Status text below circle */
static lv_obj_t *mic_btn;
static lv_obj_t *mic_label;
static lv_timer_t *poll_timer;
static enum ai_state prev_state;

/* ── Pulse animation tracking ── */
static bool pulse_growing;
static int pulse_size;
#define CIRCLE_BASE_SIZE  80
#define CIRCLE_PULSE_MIN  76
#define CIRCLE_PULSE_MAX  84

/* ── Colors ── */
#define CLR_TITLE_BAR     0x16213e
#define CLR_BG            0x0f0f23
#define CLR_CIRCLE_IDLE   0x0096C7   /* Blue — ready */
#define CLR_CIRCLE_REC    0xE0144C   /* Red — recording */
#define CLR_CIRCLE_PROC   0xF59E0B   /* Amber — processing */
#define CLR_CIRCLE_PLAY   0x10B981   /* Green — speaking */
#define CLR_CIRCLE_OFF    0x555555   /* Gray — init/error */
#define CLR_MIC_READY     0x0096C7
#define CLR_MIC_RECORDING 0xE0144C
#define CLR_MIC_DISABLED  0x555555
#define CLR_END_BTN       0xE0144C

static void place_circle(int size)
{
	if (!circle_obj) {
		return;
	}

	lv_obj_set_size(circle_obj, size, size);
	lv_obj_set_pos(circle_obj,
		(SCREEN_WIDTH - size) / 2,
		28 + (SCREEN_HEIGHT - 28 - 40 - size) / 2);

	if (circle_icon) {
		lv_obj_center(circle_icon);
	}
}

/* ================================================================
 *  Callbacks
 * ================================================================ */

static void back_cb(lv_event_t *e)
{
	app_manager_back_to_home();
}

static void mic_cb(lv_event_t *e)
{
	enum ai_state state = ai_service_get_state();

	if (state == AI_STATE_READY) {
		ai_service_start_recording();
	} else if (state == AI_STATE_RECORDING) {
		ai_service_stop_recording();
	}
}

static void end_cb(lv_event_t *e)
{
	enum ai_state state = ai_service_get_state();

	if (state == AI_STATE_RECORDING) {
		ai_service_stop_recording();
	}
	app_manager_back_to_home();
}

/* ================================================================
 *  Poll timer — syncs ai_service state → UI
 * ================================================================ */

static void poll_cb(lv_timer_t *timer)
{
	enum ai_state state = ai_service_get_state();

	/* ── Circle color & icon ── */
	uint32_t clr;
	const char *icon;
	const char *st;

	switch (state) {
	case AI_STATE_INIT:
		clr = CLR_CIRCLE_OFF;
		icon = LV_SYMBOL_SETTINGS;
		st = "Initializing...";
		break;
	case AI_STATE_WIFI_CONNECTING:
		clr = CLR_CIRCLE_OFF;
		icon = LV_SYMBOL_WIFI;
		st = "Connecting WiFi...";
		break;
	case AI_STATE_READY:
		clr = CLR_CIRCLE_IDLE;
		icon = LV_SYMBOL_AUDIO;
		st = "Tap to talk";
		break;
	case AI_STATE_RECORDING:
		clr = CLR_CIRCLE_REC;
		icon = LV_SYMBOL_STOP;
		st = "Listening...";
		break;
	case AI_STATE_PROCESSING:
		clr = CLR_CIRCLE_PROC;
		icon = LV_SYMBOL_LOOP;
		st = "Thinking...";
		break;
	case AI_STATE_STREAMING:
		clr = CLR_CIRCLE_PLAY;
		icon = LV_SYMBOL_VOLUME_MAX;
		st = "Speaking...";
		break;
	case AI_STATE_PLAYING:
		clr = CLR_CIRCLE_PLAY;
		icon = LV_SYMBOL_VOLUME_MAX;
		st = "Speaking...";
		break;
	case AI_STATE_ERROR:
		clr = CLR_CIRCLE_REC;
		icon = LV_SYMBOL_WARNING;
		st = "Error!";
		break;
	default:
		clr = CLR_CIRCLE_OFF;
		icon = LV_SYMBOL_AUDIO;
		st = "...";
		break;
	}

	if (circle_obj) {
		lv_obj_set_style_bg_color(circle_obj, lv_color_hex(clr), 0);
	}
	if (circle_icon) {
		lv_label_set_text(circle_icon, icon);
		lv_obj_center(circle_icon);
	}
	if (status_text) {
		lv_label_set_text(status_text, st);
	}

	/* ── Pulse animation for recording/playing states ── */
	if (circle_obj &&
	    (state == AI_STATE_RECORDING || state == AI_STATE_STREAMING ||
	     state == AI_STATE_PLAYING)) {
		if (pulse_growing) {
			pulse_size += 2;
			if (pulse_size >= CIRCLE_PULSE_MAX) {
				pulse_growing = false;
			}
		} else {
			pulse_size -= 2;
			if (pulse_size <= CIRCLE_PULSE_MIN) {
				pulse_growing = true;
			}
		}
		place_circle(pulse_size);
	} else if (circle_obj) {
		pulse_size = CIRCLE_BASE_SIZE;
		place_circle(CIRCLE_BASE_SIZE);
	}

	/* ── MIC button appearance ── */
	if (mic_btn && mic_label) {
		if (state == AI_STATE_RECORDING) {
			lv_obj_set_style_bg_color(mic_btn,
				lv_color_hex(CLR_MIC_RECORDING), 0);
			lv_label_set_text(mic_label, LV_SYMBOL_STOP);
		} else if (state == AI_STATE_READY) {
			lv_obj_set_style_bg_color(mic_btn,
				lv_color_hex(CLR_MIC_READY), 0);
			lv_label_set_text(mic_label, LV_SYMBOL_AUDIO);
		} else {
			lv_obj_set_style_bg_color(mic_btn,
				lv_color_hex(CLR_MIC_DISABLED), 0);
		}
	}

	prev_state = state;
}

/* ================================================================
 *  App lifecycle
 * ================================================================ */

static void on_create(lv_obj_t *screen)
{
	ble_hid_pause();

	/* Start AI service on first use */
	ai_service_init();

	/* ── Full-screen dark background ── */
	lv_obj_set_style_bg_color(screen, lv_color_hex(CLR_BG), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

	/* ── Title bar ── */
	lv_obj_t *title_bar = lv_obj_create(screen);
	lv_obj_remove_style_all(title_bar);
	lv_obj_set_size(title_bar, SCREEN_WIDTH, 28);
	lv_obj_set_pos(title_bar, 0, 0);
	lv_obj_set_style_bg_color(title_bar, lv_color_hex(CLR_TITLE_BAR), 0);
	lv_obj_set_style_bg_opa(title_bar, LV_OPA_COVER, 0);
	lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);

	/* Back button */
	lv_obj_t *back_btn = lv_button_create(title_bar);
	lv_obj_remove_style_all(back_btn);
	lv_obj_set_size(back_btn, 50, 28);
	lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_add_event_cb(back_btn, back_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *back_label = lv_label_create(back_btn);
	lv_label_set_text(back_label, LV_SYMBOL_LEFT);
	lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
	lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, 0);
	lv_obj_center(back_label);

	lv_obj_t *title = lv_label_create(title_bar);
	lv_label_set_text(title, "AI Assistant");
	lv_obj_set_style_text_color(title, lv_color_white(), 0);
	lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
	lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

	/* ── Central circle indicator ── */
	circle_obj = lv_obj_create(screen);
	lv_obj_remove_style_all(circle_obj);
	lv_obj_set_size(circle_obj, CIRCLE_BASE_SIZE, CIRCLE_BASE_SIZE);
	lv_obj_set_style_radius(circle_obj, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(circle_obj, lv_color_hex(CLR_CIRCLE_OFF), 0);
	lv_obj_set_style_bg_opa(circle_obj, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(circle_obj, 3, 0);
	lv_obj_set_style_border_color(circle_obj, lv_color_hex(0x334155), 0);
	lv_obj_set_style_border_opa(circle_obj, LV_OPA_70, 0);
	lv_obj_clear_flag(circle_obj, LV_OBJ_FLAG_SCROLLABLE);
	/* Position: centered in the area between title bar and action bar */
	place_circle(CIRCLE_BASE_SIZE);

	/* Icon inside circle */
	circle_icon = lv_label_create(circle_obj);
	lv_label_set_text(circle_icon, LV_SYMBOL_SETTINGS);
	lv_obj_set_style_text_color(circle_icon, lv_color_white(), 0);
	lv_obj_set_style_text_font(circle_icon, &lv_font_montserrat_18, 0);
	lv_obj_center(circle_icon);

	/* ── Status text below circle ── */
	status_text = lv_label_create(screen);
	lv_label_set_text(status_text, "Initializing...");
	lv_obj_set_style_text_color(status_text, lv_color_hex(0xaaaaaa), 0);
	lv_obj_set_style_text_font(status_text, &lv_font_montserrat_14, 0);
	lv_obj_set_width(status_text, SCREEN_WIDTH);
	lv_obj_set_style_text_align(status_text, LV_TEXT_ALIGN_CENTER, 0);
	/* Position below the circle area */
	lv_obj_set_pos(status_text, 0,
		       (SCREEN_HEIGHT - 28 - 40 + CIRCLE_BASE_SIZE) / 2 + 28 + 12);

	/* ── Action bar ── */
	lv_obj_t *bar = lv_obj_create(screen);
	lv_obj_remove_style_all(bar);
	lv_obj_set_pos(bar, 0, SCREEN_HEIGHT - 40);
	lv_obj_set_size(bar, SCREEN_WIDTH, 40);
	lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_TITLE_BAR), 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

	/* MIC button (left side) */
	mic_btn = lv_button_create(bar);
	lv_obj_set_size(mic_btn, 56, 32);
	lv_obj_align(mic_btn, LV_ALIGN_LEFT_MID, 60, 0);
	lv_obj_set_style_bg_color(mic_btn, lv_color_hex(CLR_MIC_READY), 0);
	lv_obj_set_style_radius(mic_btn, 16, 0);
	lv_obj_add_event_cb(mic_btn, mic_cb, LV_EVENT_CLICKED, NULL);

	mic_label = lv_label_create(mic_btn);
	lv_label_set_text(mic_label, LV_SYMBOL_AUDIO);
	lv_obj_center(mic_label);
	lv_obj_set_style_text_color(mic_label, lv_color_white(), 0);

	/* END button (right side) */
	lv_obj_t *end_btn = lv_button_create(bar);
	lv_obj_set_size(end_btn, 56, 32);
	lv_obj_align(end_btn, LV_ALIGN_RIGHT_MID, -60, 0);
	lv_obj_set_style_bg_color(end_btn, lv_color_hex(CLR_END_BTN), 0);
	lv_obj_set_style_radius(end_btn, 16, 0);
	lv_obj_add_event_cb(end_btn, end_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *end_label = lv_label_create(end_btn);
	lv_label_set_text(end_label, LV_SYMBOL_CLOSE);
	lv_obj_center(end_label);
	lv_obj_set_style_text_color(end_label, lv_color_white(), 0);

	/* Add buttons to keyboard group for navigation */
	lv_group_t *g = app_manager_get_kb_group();
	if (g) {
		lv_group_add_obj(g, mic_btn);
		lv_group_add_obj(g, end_btn);
		lv_group_add_obj(g, back_btn);
	}

	/* ── Init animation state ── */
	pulse_growing = true;
	pulse_size = CIRCLE_BASE_SIZE;

	/* ── Start poll timer (100ms) ── */
	prev_state = ai_service_get_state();
	poll_timer = lv_timer_create(poll_cb, 100, NULL);
}

static void on_destroy(void)
{
	if (poll_timer) {
		lv_timer_delete(poll_timer);
		poll_timer = NULL;
	}
	ble_hid_resume();
	/* Nullify all pointers — screen objects are deleted by app_manager */
	circle_obj = NULL;
	circle_icon = NULL;
	status_text = NULL;
	mic_btn = NULL;
	mic_label = NULL;
}

/* ── App registration info ── */
const app_info_t app_ai_assistant = {
	.name = "AI",
	.icon_color = LV_COLOR_MAKE(0x00, 0x96, 0xC7),
	.icon_symbol = LV_SYMBOL_AUDIO,
	.on_create = on_create,
	.on_destroy = on_destroy,
};
