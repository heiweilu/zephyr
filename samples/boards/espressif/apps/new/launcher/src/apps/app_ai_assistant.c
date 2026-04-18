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
#include "../resource.h"

/* ── UI object pointers (valid only while app is active) ── */
static lv_obj_t *center_area;       /* Transparent full-screen container */
static lv_obj_t *circle_obj;        /* Central status orb (also click target) */
static lv_obj_t *circle_icon;       /* Icon inside orb */
static lv_obj_t *arc_obj;           /* Spinner shown only in PROCESSING state */
static lv_obj_t *close_btn;         /* Tiny X in the top-right corner */
static lv_timer_t *poll_timer;
static enum ai_state prev_state;

/* ── Orb sizing ── */
#define CIRCLE_BASE_SIZE  80
#define CIRCLE_BREATH_MIN 70
#define CIRCLE_BREATH_MAX 96
#define ARC_SIZE          110

/* ── Colors (light neumorphic palette) ── */
#define CLR_TITLE_BAR     0xFFFFFF
#define CLR_BG            0xE6E9EF   /* Light gray surface */
#define CLR_CIRCLE_IDLE   0x6366F1   /* Indigo accent — ready */
#define CLR_CIRCLE_REC    0xDC2626   /* Deep red — recording */
#define CLR_CIRCLE_PROC   0xF59E0B   /* Amber — processing */
#define CLR_CIRCLE_PLAY   0x10B981   /* Green — speaking */
#define CLR_CIRCLE_OFF    0x9CA3AF   /* Mid gray — init/error */
#define CLR_MIC_READY     0x6366F1
#define CLR_MIC_RECORDING 0xDC2626
#define CLR_MIC_DISABLED  0x9CA3AF
#define CLR_END_BTN       0xDC2626

static void place_circle(int size)
{
	if (!circle_obj) {
		return;
	}

	lv_obj_set_size(circle_obj, size, size);
	lv_obj_center(circle_obj);

	if (circle_icon) {
		lv_obj_center(circle_icon);
	}
}

/* ================================================================
 *  Animation engine — smooth orb breathing + halos + spinner
 * ================================================================ */

/* Animation exec callbacks — must take (void *, int32_t) */
static void anim_orb_size_cb(void *var, int32_t v)
{
	lv_obj_set_size((lv_obj_t *)var, v, v);
	lv_obj_center((lv_obj_t *)var);
	if (var == circle_obj && circle_icon) {
		lv_obj_center(circle_icon);
	}
}

static void anim_arc_rotate_cb(void *var, int32_t v)
{
	lv_arc_set_rotation((lv_obj_t *)var, v);
}

static void stop_all_anims(void)
{
	if (circle_obj) {
		lv_anim_delete(circle_obj, anim_orb_size_cb);
	}
	if (arc_obj) {
		lv_anim_delete(arc_obj, anim_arc_rotate_cb);
	}
}

/* Start a continuous breath on circle_obj using sine easing.
 * period_ms: full cycle (e.g. 1500ms ⇒ shrink 750 + grow 750).
 */
static void start_breath(int period_ms, int min_size, int max_size)
{
	if (!circle_obj) {
		return;
	}
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, circle_obj);
	lv_anim_set_exec_cb(&a, anim_orb_size_cb);
	lv_anim_set_values(&a, min_size, max_size);
	lv_anim_set_duration(&a, period_ms / 2);
	lv_anim_set_playback_duration(&a, period_ms / 2);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
	lv_anim_start(&a);
}

static void start_spinner_arc(uint32_t color)
{
	if (!arc_obj) {
		return;
	}
	lv_obj_clear_flag(arc_obj, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_style_arc_color(arc_obj, lv_color_hex(color), LV_PART_INDICATOR);
	lv_arc_set_bg_angles(arc_obj, 0, 360);
	lv_arc_set_angles(arc_obj, 0, 70);

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, arc_obj);
	lv_anim_set_exec_cb(&a, anim_arc_rotate_cb);
	lv_anim_set_values(&a, 0, 360);
	lv_anim_set_duration(&a, 1500);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_set_path_cb(&a, lv_anim_path_linear);
	lv_anim_start(&a);
}

/* Apply animation set for a given AI state (called only on state change).
 *
 * CPU-economy policy: only the idle READY state runs a continuous animation.
 * RECORDING / PROCESSING / PLAYING / STREAMING are network-heavy and any
 * extra LVGL redraw competes with I2S DMA and WiFi RX buffers (we observed
 * "Failed to allocate net buffer" storms when animating during TTS). For
 * those states we just change the orb color statically.
 */
static void apply_state_animation(enum ai_state state, uint32_t color)
{
	stop_all_anims();
	if (arc_obj) {
		lv_obj_add_flag(arc_obj, LV_OBJ_FLAG_HIDDEN);
	}
	if (circle_obj) {
		lv_obj_clear_flag(circle_obj, LV_OBJ_FLAG_HIDDEN);
		place_circle(CIRCLE_BASE_SIZE);
	}

	switch (state) {
	case AI_STATE_READY:
		/* Slow, low-amplitude breath — only when idle */
		start_breath(3000, 76, 88);
		break;
	case AI_STATE_PROCESSING:
		/* Hide orb, show slow spinner — light CPU cost (stroke-only) */
		if (circle_obj) {
			lv_obj_add_flag(circle_obj, LV_OBJ_FLAG_HIDDEN);
		}
		start_spinner_arc(color);
		break;
	case AI_STATE_RECORDING:
	case AI_STATE_STREAMING:
	case AI_STATE_PLAYING:
		/* No animation — free CPU for I2S + WiFi. Color change only. */
		break;
	default:
		/* INIT / WIFI / ERROR — static, no animation */
		break;
	}
}

/* ================================================================
 *  Callbacks
 * ================================================================ */

static void close_cb(lv_event_t *e)
{
	enum ai_state state = ai_service_get_state();

	if (state == AI_STATE_RECORDING) {
		ai_service_stop_recording();
	}
	app_manager_back_to_home();
}

static void orb_cb(lv_event_t *e)
{
	enum ai_state state = ai_service_get_state();

	if (state == AI_STATE_READY) {
		ai_service_start_recording();
	} else if (state == AI_STATE_RECORDING) {
		ai_service_stop_recording();
	}
	/* PROCESSING / PLAYING / STREAMING / INIT — click ignored */
}

/* ================================================================
 *  Poll timer — syncs ai_service state → UI
 * ================================================================ */

static void poll_cb(lv_timer_t *timer)
{
	enum ai_state state = ai_service_get_state();

	/* ── Pick orb color + glyph for the current state ── */
	uint32_t clr;
	const char *icon;

	switch (state) {
	case AI_STATE_INIT:
		clr = CLR_CIRCLE_OFF;
		icon = LV_SYMBOL_SETTINGS;
		break;
	case AI_STATE_WIFI_CONNECTING:
		clr = CLR_CIRCLE_OFF;
		icon = LV_SYMBOL_WIFI;
		break;
	case AI_STATE_READY:
		clr = CLR_CIRCLE_IDLE;
		icon = LV_SYMBOL_BELL;
		break;
	case AI_STATE_RECORDING:
		clr = CLR_CIRCLE_REC;
		icon = LV_SYMBOL_STOP;
		break;
	case AI_STATE_PROCESSING:
		clr = CLR_CIRCLE_PROC;
		icon = LV_SYMBOL_LOOP;
		break;
	case AI_STATE_STREAMING:
	case AI_STATE_PLAYING:
		clr = CLR_CIRCLE_PLAY;
		icon = LV_SYMBOL_VOLUME_MAX;
		break;
	case AI_STATE_ERROR:
		clr = CLR_CIRCLE_REC;
		icon = LV_SYMBOL_WARNING;
		break;
	default:
		clr = CLR_CIRCLE_OFF;
		icon = LV_SYMBOL_BELL;
		break;
	}

	if (circle_obj) {
		lv_obj_set_style_bg_color(circle_obj, lv_color_hex(clr), 0);
	}
	if (circle_icon) {
		lv_label_set_text(circle_icon, icon);
		lv_obj_center(circle_icon);
	}

	/* ── State change → restart animations with new profile ── */
	if (state != prev_state) {
		apply_state_animation(state, clr);
	}

	prev_state = state;
}

/* ================================================================
 *  App lifecycle
 * ================================================================ */

static void on_create(lv_obj_t *screen)
{
	/* Keep BLE HID active so the mouse can drive the orb / close button. */

	/* Start AI service on first use */
	ai_service_init();

	/* ── Full-screen dark background ── */
	lv_obj_set_style_bg_color(screen, lv_color_hex(CLR_BG), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

	/* ── Central area: full-screen transparent container hosting orb + arc.
	 *    Children align LV_ALIGN_CENTER, so animating size auto-recenters.
	 */
	center_area = lv_obj_create(screen);
	lv_obj_remove_style_all(center_area);
	lv_obj_set_size(center_area, SCREEN_WIDTH, SCREEN_HEIGHT);
	lv_obj_set_pos(center_area, 0, 0);
	lv_obj_clear_flag(center_area, LV_OBJ_FLAG_SCROLLABLE);

	/* ── Central orb (also the click target for record/stop) ── */
	circle_obj = lv_obj_create(center_area);
	lv_obj_remove_style_all(circle_obj);
	lv_obj_set_size(circle_obj, CIRCLE_BASE_SIZE, CIRCLE_BASE_SIZE);
	lv_obj_set_style_radius(circle_obj, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(circle_obj, lv_color_hex(CLR_CIRCLE_OFF), 0);
	lv_obj_set_style_bg_opa(circle_obj, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(circle_obj, 2, 0);
	lv_obj_set_style_border_color(circle_obj, lv_color_hex(0xC4C8D2), 0);
	lv_obj_set_style_border_opa(circle_obj, LV_OPA_70, 0);
	lv_obj_clear_flag(circle_obj, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(circle_obj, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(circle_obj, orb_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_center(circle_obj);

	/* Icon inside orb */
	circle_icon = lv_label_create(circle_obj);
	lv_label_set_text(circle_icon, LV_SYMBOL_SETTINGS);
	lv_obj_set_style_text_color(circle_icon, lv_color_white(), 0);
	lv_obj_set_style_text_font(circle_icon, &lv_font_montserrat_18, 0);
	lv_obj_remove_flag(circle_icon, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_center(circle_icon);

	/* ── Spinner arc (PROCESSING state only) ── */
	arc_obj = lv_arc_create(center_area);
	lv_obj_set_size(arc_obj, ARC_SIZE, ARC_SIZE);
	lv_arc_set_bg_angles(arc_obj, 0, 360);
	lv_arc_set_angles(arc_obj, 0, 70);
	lv_obj_remove_style(arc_obj, NULL, LV_PART_KNOB);
	lv_obj_clear_flag(arc_obj, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_arc_width(arc_obj, 5, LV_PART_MAIN);
	lv_obj_set_style_arc_width(arc_obj, 5, LV_PART_INDICATOR);
	lv_obj_set_style_arc_opa(arc_obj, LV_OPA_30, LV_PART_MAIN);
	lv_obj_set_style_arc_color(arc_obj, lv_color_hex(CLR_CIRCLE_PROC), LV_PART_INDICATOR);
	lv_obj_add_flag(arc_obj, LV_OBJ_FLAG_HIDDEN);
	lv_obj_center(arc_obj);

	/* ── Top-right close (X) button ── */
	close_btn = lv_button_create(screen);
	lv_obj_remove_style_all(close_btn);
	lv_obj_set_size(close_btn, 28, 28);
	lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -4, 4);
	lv_obj_set_style_radius(close_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(close_btn, lv_color_hex(0xFFFFFF), 0);
	lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
	lv_obj_add_event_cb(close_btn, close_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *close_label = lv_label_create(close_btn);
	lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
	lv_obj_set_style_text_color(close_label, lv_color_hex(0x6B7280), 0);
	lv_obj_set_style_text_font(close_label, &lv_font_montserrat_14, 0);
	lv_obj_center(close_label);

	/* Keyboard / encoder navigation — just orb + close */
	lv_group_t *g = app_manager_get_kb_group();
	if (g) {
		lv_group_add_obj(g, circle_obj);
		lv_group_add_obj(g, close_btn);
	}

	/* ── Start poll timer (100ms) + kick initial animation ── */
	prev_state = (enum ai_state)-1;   /* force first poll_cb to apply animation */
	poll_timer = lv_timer_create(poll_cb, 100, NULL);
}

static void on_destroy(void)
{
	if (poll_timer) {
		lv_timer_delete(poll_timer);
		poll_timer = NULL;
	}
	stop_all_anims();
	/* Nullify all pointers — screen objects are deleted by app_manager */
	center_area = NULL;
	circle_obj = NULL;
	circle_icon = NULL;
	arc_obj = NULL;
	close_btn = NULL;
}

/* ── App registration info ── */
const app_info_t app_ai_assistant = {
	.name = "AI",
	.icon_color = LV_COLOR_MAKE(0x63, 0x66, 0xF1),
	.icon_symbol = LV_SYMBOL_BELL,
	.on_create = on_create,
	.on_destroy = on_destroy,
};
