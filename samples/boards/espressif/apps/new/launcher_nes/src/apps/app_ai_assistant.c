/*
 * launcher_nes — AI Assistant placeholder.
 * Real ai_service is stripped from launcher_nes to free DRAM/Flash for the
 * NES emulator. This stub keeps the `app_ai_assistant` symbol so main.c can
 * still register all 8 home icons.
 */

#include <lvgl.h>
#include "../app_manager.h"

static void back_cb(lv_event_t *e)
{
	app_manager_back_to_home();
}

static void on_create(lv_obj_t *screen)
{
	lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F172A), 0);

	lv_obj_t *title = lv_label_create(screen);
	lv_obj_set_style_text_color(title, lv_color_hex(0xF87171), 0);
	lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
	lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);
	lv_label_set_text(title, "AI Assistant\nNot in launcher_nes");

	lv_obj_t *btn = lv_button_create(screen);
	lv_obj_set_size(btn, 80, 30);
	lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -10);
	lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, NULL);
	lv_group_add_obj(app_manager_get_kb_group(), btn);
	lv_obj_t *lbl = lv_label_create(btn);
	lv_label_set_text(lbl, "< Back");
	lv_obj_center(lbl);
}

static void on_destroy(void)
{
}

const app_info_t app_ai_assistant = {
	.name        = "AI",
	.icon_color  = LV_COLOR_MAKE(0xEF, 0x44, 0x44),
	.icon_symbol = LV_SYMBOL_AUDIO,
	.on_create   = on_create,
	.on_destroy  = on_destroy,
};
