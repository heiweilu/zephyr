/*
 * app_ai_assistant.c — AI Assistant chat UI
 *
 * Layout (320×240):
 *   [Title bar 28px] AI Assistant | status
 *   [Chat area ~172px] scrollable message bubbles
 *   [Action bar 40px]  [MIC]  status text
 *
 * Polls ai_service state every 100ms via lv_timer to update UI.
 * Recording triggered by MIC button click or BOOT button (hardware).
 */

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <string.h>

#include "../app_manager.h"
#include "../ai_service.h"
#include "../resource.h"

/* ── UI object pointers (valid only while app is active) ── */
static lv_obj_t *status_label;
static lv_obj_t *chat_area;
static lv_obj_t *mic_btn;
static lv_obj_t *mic_label;
static lv_obj_t *action_text;
static lv_timer_t *poll_timer;

/* ── Render tracking ── */
static int rendered_msg_count;
static size_t rendered_live_len;
static lv_obj_t *live_bubble_label;  /* Label inside live streaming bubble */
static enum ai_state prev_state;

/* ── Colors ── */
#define CLR_USER_BUBBLE   0x0096C7
#define CLR_AI_BUBBLE     0x2a2a4a
#define CLR_TITLE_BAR     0x16213e
#define CLR_CHAT_BG       0x0f0f23
#define CLR_MIC_READY     0x0096C7
#define CLR_MIC_RECORDING 0xE0144C
#define CLR_MIC_DISABLED  0x555555

/* ================================================================
 *  Chat bubble creation
 * ================================================================ */

/*
 * Creates a message bubble inside the chat area.
 * Each bubble is a full-width transparent row containing a colored box
 * with a wrapped text label. User bubbles align right, AI bubbles left.
 *
 * Returns the label object (for live-updating during streaming).
 */
static lv_obj_t *create_bubble(lv_obj_t *parent, bool is_user,
			       const char *text)
{
	/* Row container — full width, transparent, no scrollbar */
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_remove_style_all(row);
	lv_obj_set_width(row, LV_PCT(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

	/* Bubble box */
	lv_obj_t *bubble = lv_obj_create(row);
	lv_obj_remove_style_all(bubble);
	lv_obj_set_width(bubble, LV_SIZE_CONTENT);
	lv_obj_set_height(bubble, LV_SIZE_CONTENT);
	lv_obj_set_style_max_width(bubble, 210, 0);
	lv_obj_set_style_pad_all(bubble, 6, 0);
	lv_obj_set_style_radius(bubble, 8, 0);
	lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
	lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

	if (is_user) {
		lv_obj_set_style_bg_color(bubble,
					  lv_color_hex(CLR_USER_BUBBLE), 0);
		lv_obj_align(bubble, LV_ALIGN_RIGHT_MID, -2, 0);
	} else {
		lv_obj_set_style_bg_color(bubble,
					  lv_color_hex(CLR_AI_BUBBLE), 0);
		lv_obj_align(bubble, LV_ALIGN_LEFT_MID, 2, 0);
	}

	/* Text label */
	lv_obj_t *lbl = lv_label_create(bubble);
	lv_label_set_text(lbl, text);
	lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
	lv_obj_set_style_text_font(lbl, &lv_font_source_han_sans_sc_16_cjk, 0);
	lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(lbl, LV_SIZE_CONTENT);
	lv_obj_set_style_max_width(lbl, 196, 0);  /* 210 - 2*6 pad - 2 margin */

	return lbl;
}

/* ================================================================
 *  Callbacks
 * ================================================================ */

static void mic_cb(lv_event_t *e)
{
	enum ai_state state = ai_service_get_state();

	if (state == AI_STATE_READY) {
		ai_service_start_recording();
	} else if (state == AI_STATE_RECORDING) {
		ai_service_stop_recording();
	}
}

/* ================================================================
 *  Poll timer — syncs ai_service state → UI
 * ================================================================ */

static void poll_cb(lv_timer_t *timer)
{
	enum ai_state state = ai_service_get_state();

	/* ── Status text ── */
	const char *st;
	switch (state) {
	case AI_STATE_INIT:             st = "Init..."; break;
	case AI_STATE_WIFI_CONNECTING:  st = "WiFi..."; break;
	case AI_STATE_READY:            st = "Ready"; break;
	case AI_STATE_RECORDING:        st = "REC..."; break;
	case AI_STATE_PROCESSING:       st = "Sending..."; break;
	case AI_STATE_STREAMING:        st = "AI..."; break;
	case AI_STATE_PLAYING:          st = "Playing"; break;
	case AI_STATE_ERROR:            st = "Error!"; break;
	default:                        st = "?"; break;
	}
	if (status_label) {
		lv_label_set_text(status_label, st);
	}
	if (action_text) {
		lv_label_set_text(action_text, st);
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

	/* ── Render finished messages ── */
	int total = ai_service_get_msg_count();
	while (rendered_msg_count < total) {
		const struct ai_chat_msg *msg =
			ai_service_get_msg(rendered_msg_count);
		if (msg && chat_area) {
			/* If an AI msg arrives while live bubble exists,
			 * remove the live bubble first to avoid duplicate. */
			if (!msg->is_user && live_bubble_label) {
				lv_obj_t *row = lv_obj_get_parent(
					lv_obj_get_parent(live_bubble_label));
				if (row) {
					lv_obj_delete(row);
				}
				live_bubble_label = NULL;
				rendered_live_len = 0;
			}
			create_bubble(chat_area, msg->is_user, msg->text);
			lv_obj_scroll_to_y(chat_area, LV_COORD_MAX, LV_ANIM_ON);
		}
		rendered_msg_count++;
	}

	/* ── Live streaming text ── */
	if (state == AI_STATE_STREAMING || state == AI_STATE_PROCESSING) {
		size_t live_len = ai_service_get_live_text_len();
		if (live_len > 0 && live_len != rendered_live_len && chat_area) {
			if (!live_bubble_label) {
				live_bubble_label = create_bubble(chat_area, false,
					ai_service_get_live_text());
			} else {
				lv_label_set_text(live_bubble_label,
					ai_service_get_live_text());
			}
			rendered_live_len = live_len;
			lv_obj_scroll_to_y(chat_area, LV_COORD_MAX, LV_ANIM_ON);
		}
	}

	/* ── When streaming ends, remove live bubble (history msg replaces it) ── */
	if ((prev_state == AI_STATE_STREAMING || prev_state == AI_STATE_PLAYING)
	    && state == AI_STATE_READY) {
		if (live_bubble_label) {
			/* label → bubble → row: delete the row container */
			lv_obj_t *row = lv_obj_get_parent(
					lv_obj_get_parent(live_bubble_label));
			if (row) {
				lv_obj_delete(row);
			}
		}
		live_bubble_label = NULL;
		rendered_live_len = 0;
	}

	prev_state = state;
}

/* ================================================================
 *  App lifecycle
 * ================================================================ */

static void on_create(lv_obj_t *screen)
{
	/* Start AI service on first use */
	ai_service_init();

	/* ── Title bar ── */
	lv_obj_t *title_bar = lv_obj_create(screen);
	lv_obj_remove_style_all(title_bar);
	lv_obj_set_size(title_bar, SCREEN_WIDTH, 28);
	lv_obj_set_pos(title_bar, 0, 0);
	lv_obj_set_style_bg_color(title_bar, lv_color_hex(CLR_TITLE_BAR), 0);
	lv_obj_set_style_bg_opa(title_bar, LV_OPA_COVER, 0);
	lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *title = lv_label_create(title_bar);
	lv_label_set_text(title, LV_SYMBOL_AUDIO " AI Assistant");
	lv_obj_set_style_text_color(title, lv_color_white(), 0);
	lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
	lv_obj_align(title, LV_ALIGN_LEFT_MID, 6, 0);

	status_label = lv_label_create(title_bar);
	lv_label_set_text(status_label, "Init...");
	lv_obj_set_style_text_color(status_label, lv_color_hex(0x888888), 0);
	lv_obj_set_style_text_font(status_label, &lv_font_source_han_sans_sc_16_cjk, 0);
	lv_obj_align(status_label, LV_ALIGN_RIGHT_MID, -6, 0);

	/* ── Chat area (scrollable message list) ── */
	chat_area = lv_obj_create(screen);
	lv_obj_remove_style_all(chat_area);
	lv_obj_set_pos(chat_area, 0, 28);
	lv_obj_set_size(chat_area, SCREEN_WIDTH, SCREEN_HEIGHT - 28 - 40);
	lv_obj_set_style_bg_color(chat_area, lv_color_hex(CLR_CHAT_BG), 0);
	lv_obj_set_style_bg_opa(chat_area, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(chat_area, 4, 0);
	lv_obj_set_style_pad_row(chat_area, 4, 0);
	lv_obj_set_flex_flow(chat_area, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(chat_area, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_add_flag(chat_area, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(chat_area, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(chat_area, LV_SCROLLBAR_MODE_AUTO);

	/* ── Action bar ── */
	lv_obj_t *bar = lv_obj_create(screen);
	lv_obj_remove_style_all(bar);
	lv_obj_set_pos(bar, 0, SCREEN_HEIGHT - 40);
	lv_obj_set_size(bar, SCREEN_WIDTH, 40);
	lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_TITLE_BAR), 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

	/* MIC button */
	mic_btn = lv_button_create(bar);
	lv_obj_set_size(mic_btn, 56, 32);
	lv_obj_align(mic_btn, LV_ALIGN_LEFT_MID, 8, 0);
	lv_obj_set_style_bg_color(mic_btn, lv_color_hex(CLR_MIC_READY), 0);
	lv_obj_set_style_radius(mic_btn, 16, 0);
	lv_obj_add_event_cb(mic_btn, mic_cb, LV_EVENT_CLICKED, NULL);

	mic_label = lv_label_create(mic_btn);
	lv_label_set_text(mic_label, LV_SYMBOL_AUDIO);
	lv_obj_center(mic_label);
	lv_obj_set_style_text_color(mic_label, lv_color_white(), 0);

	/* Status text next to MIC */
	action_text = lv_label_create(bar);
	lv_label_set_text(action_text, "Init...");
	lv_obj_set_style_text_color(action_text, lv_color_hex(0xaaaaaa), 0);
	lv_obj_set_style_text_font(action_text, &lv_font_source_han_sans_sc_16_cjk, 0);
	lv_obj_align(action_text, LV_ALIGN_LEFT_MID, 72, 0);

	/* Add MIC to keyboard group for keyboard navigation */
	lv_group_t *g = app_manager_get_kb_group();
	if (g) {
		lv_group_add_obj(g, mic_btn);
	}

	/* ── Rebuild chat history from service ── */
	rendered_msg_count = 0;
	rendered_live_len = 0;
	live_bubble_label = NULL;

	int total = ai_service_get_msg_count();
	for (int i = 0; i < total; i++) {
		const struct ai_chat_msg *msg = ai_service_get_msg(i);
		if (msg) {
			create_bubble(chat_area, msg->is_user, msg->text);
		}
	}
	rendered_msg_count = total;

	if (total > 0) {
		lv_obj_scroll_to_y(chat_area, LV_COORD_MAX, LV_ANIM_OFF);
	}

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
	/* Nullify all pointers — screen objects are deleted by app_manager */
	status_label = NULL;
	chat_area = NULL;
	mic_btn = NULL;
	mic_label = NULL;
	action_text = NULL;
	live_bubble_label = NULL;
}

/* ── App registration info ── */
const app_info_t app_ai_assistant = {
	.name = "AI",
	.icon_color = LV_COLOR_MAKE(0x00, 0x96, 0xC7),
	.icon_symbol = LV_SYMBOL_AUDIO,
	.on_create = on_create,
	.on_destroy = on_destroy,
};
