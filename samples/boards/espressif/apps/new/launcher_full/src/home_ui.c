/*
 * home_ui.c — HOME screen for launcher_full
 *
 * Layout 320x240 (landscape):
 *   ┌─────────────────────────────────────────┐
 *   │  Status Bar (24px)  · launcher_full     │
 *   ├─────────────────────────────────────────┤
 *   │  ┌──┐  ┌──┐  ┌──┐  ┌──┐                │
 *   │  │AI│  │CAM│ │NES│ │PH│   row 0        │
 *   │  └──┘  └──┘  └──┘  └──┘                │
 *   │  ┌──┐  ┌──┐  ┌──┐  ┌──┐                │
 *   │  │TER│ │MUS│ │FAC│ │IMU│  row 1        │
 *   │  └──┘  └──┘  └──┘  └──┘                │
 *   └─────────────────────────────────────────┘
 *
 * Only APP_AI_VISION (0) and APP_CAMERA (1) are real (wired in 2d/2e).
 * All other icons trigger a toast "敬请期待" via home_ui_activate_selected().
 *
 * Focus navigation is done manually (no LV input device) so that we can
 * use a single BOOT key. The selected icon gets a thicker accent border.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>

#include "home_ui.h"
#include "app_camera.h"
#include "app_ai_vision.h"
#include "app_imu.h"

LOG_MODULE_REGISTER(home_ui, LOG_LEVEL_INF);

#define SCREEN_W          320
#define SCREEN_H          240
#define STATUS_BAR_H      24
#define ICON_COLS         4
#define ICON_ROWS         2
#define ICON_SIZE         56
#define ICON_PAD          12
#define ICON_LABEL_H      16

struct icon_def {
	const char *name;     /* shown under the icon */
	const char *symbol;   /* LV_SYMBOL_* glyph or short text */
	uint32_t    color;    /* tile background color */
};

/* Order MUST match the APP_* enum in home_ui.h. */
static const struct icon_def icons[APP_COUNT] = {
	[APP_AI_VISION] = { "AI",     LV_SYMBOL_EYE_OPEN, 0x6366F1 },
	[APP_CAMERA]    = { "Camera", LV_SYMBOL_IMAGE,    0x10B981 },
	[APP_NES]       = { "NES",    LV_SYMBOL_PLAY,     0xF59E0B },
	[APP_PHOTOS]    = { "Photos", LV_SYMBOL_DIRECTORY,0x06B6D4 },
	[APP_TERMINAL]  = { "Term",   ">_",               0x64748B },
	[APP_MUSIC]     = { "Music",  LV_SYMBOL_AUDIO,    0xEC4899 },
	[APP_FACE]      = { "Face",   LV_SYMBOL_EYE_OPEN, 0x8B5CF6 },
	[APP_IMU]       = { "IMU",    LV_SYMBOL_REFRESH,  0xF97316 },
};

static lv_obj_t *home_screen;
static lv_obj_t *status_label;
static lv_obj_t *toast_label;
static lv_obj_t *icon_tiles[APP_COUNT];
static int       selected = 0;

static void apply_focus_style(int idx, bool focused)
{
	if (idx < 0 || idx >= APP_COUNT || !icon_tiles[idx]) {
		return;
	}
	lv_obj_set_style_border_width(icon_tiles[idx], focused ? 4 : 0, 0);
	lv_obj_set_style_border_color(icon_tiles[idx], lv_color_hex(0xFBBF24), 0);
}

static lv_obj_t *create_icon(lv_obj_t *parent, int app_id, int x, int y)
{
	const struct icon_def *def = &icons[app_id];

	/* outer container */
	lv_obj_t *cont = lv_obj_create(parent);
	lv_obj_remove_style_all(cont);
	lv_obj_set_size(cont, ICON_SIZE + 8, ICON_SIZE + ICON_LABEL_H + 4);
	lv_obj_set_pos(cont, x, y);
	lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

	/* tile (the colored square) */
	lv_obj_t *tile = lv_obj_create(cont);
	lv_obj_remove_style_all(tile);
	lv_obj_set_size(tile, ICON_SIZE, ICON_SIZE);
	lv_obj_align(tile, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_style_bg_color(tile, lv_color_hex(def->color), 0);
	lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(tile, 12, 0);

	/* glyph */
	lv_obj_t *sym = lv_label_create(tile);
	lv_label_set_text(sym, def->symbol);
	lv_obj_set_style_text_color(sym, lv_color_white(), 0);
	lv_obj_set_style_text_font(sym, &lv_font_montserrat_28, 0);
	lv_obj_center(sym);

	/* label */
	lv_obj_t *lbl = lv_label_create(cont);
	lv_label_set_text(lbl, def->name);
	lv_obj_set_style_text_color(lbl, lv_color_hex(0x374151), 0);
	lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
	lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, 0);

	icon_tiles[app_id] = tile;
	return cont;
}

static void toast_show(const char *text)
{
	if (!toast_label) {
		toast_label = lv_label_create(home_screen);
		lv_obj_set_style_bg_color(toast_label, lv_color_hex(0x1F2937), 0);
		lv_obj_set_style_bg_opa(toast_label, LV_OPA_COVER, 0);
		lv_obj_set_style_text_color(toast_label, lv_color_white(), 0);
		lv_obj_set_style_text_font(toast_label, &lv_font_montserrat_14, 0);
		lv_obj_set_style_radius(toast_label, 6, 0);
		lv_obj_set_style_pad_all(toast_label, 6, 0);
		lv_obj_align(toast_label, LV_ALIGN_BOTTOM_MID, 0, -8);
	}
	lv_label_set_text(toast_label, text);
	lv_obj_clear_flag(toast_label, LV_OBJ_FLAG_HIDDEN);
}

void home_ui_init(void)
{
	home_screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(home_screen, lv_color_hex(0xE6E9EF), 0);
	lv_obj_set_style_bg_opa(home_screen, LV_OPA_COVER, 0);
	lv_obj_clear_flag(home_screen, LV_OBJ_FLAG_SCROLLABLE);

	/* status bar */
	lv_obj_t *bar = lv_obj_create(home_screen);
	lv_obj_remove_style_all(bar);
	lv_obj_set_size(bar, SCREEN_W, STATUS_BAR_H);
	lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

	status_label = lv_label_create(bar);
	lv_label_set_text(status_label, "launcher_full");
	lv_obj_set_style_text_color(status_label, lv_color_hex(0x10B981), 0);
	lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
	lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 8, 0);

	lv_obj_t *title = lv_label_create(bar);
	lv_label_set_text(title, "Zephyr-Card");
	lv_obj_set_style_text_color(title, lv_color_hex(0x6366F1), 0);
	lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
	lv_obj_align(title, LV_ALIGN_RIGHT_MID, -8, 0);

	/* icon grid */
	int tile_w = ICON_SIZE + 8;
	int tile_h = ICON_SIZE + ICON_LABEL_H + 4;
	int grid_w = ICON_COLS * tile_w + (ICON_COLS - 1) * ICON_PAD;
	int grid_h = ICON_ROWS * tile_h + (ICON_ROWS - 1) * ICON_PAD;
	int x0 = (SCREEN_W - grid_w) / 2;
	int y0 = STATUS_BAR_H + (SCREEN_H - STATUS_BAR_H - grid_h) / 2;

	for (int i = 0; i < APP_COUNT; i++) {
		int col = i % ICON_COLS;
		int row = i / ICON_COLS;
		int x = x0 + col * (tile_w + ICON_PAD);
		int y = y0 + row * (tile_h + ICON_PAD);
		create_icon(home_screen, i, x, y);
	}

	apply_focus_style(selected, true);

	lv_screen_load(home_screen);
	LOG_INF("home_ui ready (%d icons), focus=%d", APP_COUNT, selected);
}

void home_ui_focus_next(void)
{
	apply_focus_style(selected, false);
	selected = (selected + 1) % APP_COUNT;
	apply_focus_style(selected, true);
	LOG_INF("focus → %d (%s)", selected, icons[selected].name);
}

void home_ui_activate_selected(void)
{
	LOG_INF("activate %d (%s)", selected, icons[selected].name);
	if (selected == APP_AI_VISION) {
		toast_show("AI Vision starting...");
		lv_refr_now(NULL);
		app_ai_vision_run();
	} else if (selected == APP_CAMERA) {
		toast_show("Camera starting...");
		/* Flush a frame so the toast is visible before camera takes
		 * over the LCD. app_camera_run() never returns.
		 */
		lv_refr_now(NULL);
		app_camera_run();
	} else if (selected == APP_IMU) {
		toast_show("IMU starting...");
		lv_refr_now(NULL);
		app_imu_run();
	} else {
		toast_show("Coming soon");
	}
}

int home_ui_get_selected(void)
{
	return selected;
}
