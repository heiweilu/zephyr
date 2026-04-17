/*
 * launcher_ui.c — Desktop home screen: status bar + icon grid
 *
 * Layout (320×240):
 *   ┌─────────────────────────────────────┐
 *   │  Status Bar (24px)  BLE: ...        │
 *   ├─────────────────────────────────────┤
 *   │  ┌──┐  ┌──┐  ┌──┐  ┌──┐           │
 *   │  │AI│  │CAM│  │NES│  │PH│  Row 1   │
 *   │  └──┘  └──┘  └──┘  └──┘           │
 *   │  ┌──┐  ┌──┐  ┌──┐  ┌──┐           │
 *   │  │TER│ │MUS│ │FAC│ │IMU│  Row 2   │
 *   │  └──┘  └──┘  └──┘  └──┘           │
 *   └─────────────────────────────────────┘
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <lvgl.h>
#include "launcher_ui.h"
#include "app_manager.h"
#include "resource.h"

static lv_obj_t *home_screen;
static lv_obj_t *status_label;

/* ── Icon click handler ──────────────────────────────── */

static void icon_click_cb(lv_event_t *e)
{
	int app_id = (int)(intptr_t)lv_event_get_user_data(e);
	app_manager_launch(app_id);
}

/* ── Create one icon tile ────────────────────────────── */

static lv_obj_t *create_icon(lv_obj_t *parent, int app_id,
			     lv_coord_t x, lv_coord_t y)
{
	const app_info_t *app = app_manager_get(app_id);
	if (!app) {
		return NULL;
	}

	/* Container for icon + label */
	lv_obj_t *cont = lv_obj_create(parent);
	lv_obj_remove_style_all(cont);
	lv_obj_set_size(cont, ICON_SIZE + 8, ICON_SIZE + ICON_LABEL_H + 4);
	lv_obj_set_pos(cont, x, y);
	lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

	/* Colored square icon */
	lv_obj_t *icon = lv_obj_create(cont);
	lv_obj_remove_style_all(icon);
	lv_obj_set_size(icon, ICON_SIZE, ICON_SIZE);
	lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_style_bg_color(icon, app->icon_color, 0);
	lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(icon, 10, 0);
	lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(icon, icon_click_cb, LV_EVENT_CLICKED,
			    (void *)(intptr_t)app_id);

	/* Symbol on icon */
	lv_obj_t *sym = lv_label_create(icon);
	lv_label_set_text(sym, app->icon_symbol ? app->icon_symbol : "?");
	lv_obj_set_style_text_color(sym, lv_color_white(), 0);
	lv_obj_set_style_text_font(sym, &lv_font_montserrat_18, 0);
	lv_obj_center(sym);

	/* Name label below icon */
	lv_obj_t *lbl = lv_label_create(cont);
	lv_label_set_text(lbl, app->name);
	lv_obj_set_style_text_color(lbl, lv_color_hex(0xcccccc), 0);
	lv_obj_set_style_text_font(lbl, &lv_font_source_han_sans_sc_16_cjk, 0);
	lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, 0);

	/* Add icon to keyboard group for focus navigation */
	lv_group_t *group = app_manager_get_kb_group();
	if (group) {
		lv_group_add_obj(group, icon);
	}

	return cont;
}

/* ── Public API ──────────────────────────────────────── */

void launcher_ui_init(void)
{
	int count = app_manager_get_count();

	/* Create home screen */
	home_screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(home_screen, lv_color_hex(0x0f0f23), 0);
	lv_obj_set_style_bg_opa(home_screen, LV_OPA_COVER, 0);
	lv_obj_clear_flag(home_screen, LV_OBJ_FLAG_SCROLLABLE);

	/* Status bar background */
	lv_obj_t *bar = lv_obj_create(home_screen);
	lv_obj_remove_style_all(bar);
	lv_obj_set_size(bar, SCREEN_WIDTH, STATUS_BAR_HEIGHT);
	lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_set_style_bg_color(bar, lv_color_hex(0x16213e), 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

	/* Status label */
	status_label = lv_label_create(bar);
	lv_label_set_text(status_label, "BLE: Scanning...");
	lv_obj_set_style_text_color(status_label, lv_color_hex(0x00ff88), 0);
	lv_obj_set_style_text_font(status_label, &lv_font_source_han_sans_sc_16_cjk, 0);
	lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 8, 0);

	/* Title on right side of status bar */
	lv_obj_t *title = lv_label_create(bar);
	lv_label_set_text(title, "Zephyr-Card");
	lv_obj_set_style_text_color(title, lv_color_hex(0x8888aa), 0);
	lv_obj_set_style_text_font(title, &lv_font_source_han_sans_sc_16_cjk, 0);
	lv_obj_align(title, LV_ALIGN_RIGHT_MID, -8, 0);

	/* ── Icon Grid ── */
	int cols = ICON_COLS;
	int icon_total_w = ICON_SIZE + 8;
	int icon_total_h = ICON_SIZE + ICON_LABEL_H + 4;
	int grid_w = cols * icon_total_w + (cols - 1) * ICON_PADDING;
	int grid_h = ICON_ROWS * icon_total_h + (ICON_ROWS - 1) * ICON_PADDING;
	int x_start = (SCREEN_WIDTH - grid_w) / 2;
	int y_start = STATUS_BAR_HEIGHT +
		      (SCREEN_HEIGHT - STATUS_BAR_HEIGHT - grid_h) / 2;

	for (int i = 0; i < count && i < ICON_COLS * ICON_ROWS; i++) {
		int col = i % cols;
		int row = i / cols;
		int x = x_start + col * (icon_total_w + ICON_PADDING);
		int y = y_start + row * (icon_total_h + ICON_PADDING);
		create_icon(home_screen, i, x, y);
	}

	/* Register home screen with app manager */
	app_manager_set_home_screen(home_screen);

	/* Load home screen */
	lv_screen_load(home_screen);

	printk("[UI] Launcher home screen ready (%d apps)\n", count);
}

void launcher_ui_update_status(const char *ble_status)
{
	if (status_label && !app_manager_is_app_active()) {
		lv_label_set_text(status_label, ble_status);
	}
}
