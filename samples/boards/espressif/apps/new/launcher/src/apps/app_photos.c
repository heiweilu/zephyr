/*
 * app_photos.c — Photo gallery placeholder
 */

#include <lvgl.h>
#include "../app_manager.h"

static void back_cb(lv_event_t *e)
{
	app_manager_back_to_home();
}

static void on_create(lv_obj_t *screen)
{
	lv_obj_t *title = lv_label_create(screen);
	lv_label_set_text(title, "Photos");
	lv_obj_set_style_text_color(title, lv_color_white(), 0);
	lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

	lv_obj_t *desc = lv_label_create(screen);
	lv_label_set_text(desc, "SD Card Gallery\n(Phase 5)");
	lv_obj_set_style_text_color(desc, lv_color_hex(0x888888), 0);
	lv_obj_set_style_text_font(desc, &lv_font_montserrat_14, 0);
	lv_obj_align(desc, LV_ALIGN_CENTER, 0, -10);

	lv_obj_t *btn = lv_button_create(screen);
	lv_obj_set_size(btn, 120, 40);
	lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -30);
	lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *lbl = lv_label_create(btn);
	lv_label_set_text(lbl, "< Back");
	lv_obj_center(lbl);

	lv_group_t *g = app_manager_get_kb_group();
	if (g) {
		lv_group_add_obj(g, btn);
	}
}

static void on_destroy(void) {}

const app_info_t app_photos = {
	.name = "Photos",
	.icon_color = LV_COLOR_MAKE(0x2E, 0xCC, 0x71),
	.icon_symbol = LV_SYMBOL_DIRECTORY,
	.on_create = on_create,
	.on_destroy = on_destroy,
};
