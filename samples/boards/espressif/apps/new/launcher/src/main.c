/*
 * CHD-ESP32-S3-BOX Launcher — main entry point
 *
 * Flow:
 *   1. Backlight ON
 *   2. BLE HID Host init (async scanning)
 *   3. Register all app modules
 *   4. Build launcher home screen (icon grid)
 *   5. LVGL main loop + mouse/keyboard input processing
 *
 * ESC key or right-click → back to home screen
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <lvgl.h>

#include "resource.h"
#include "ble_hid.h"
#include "app_manager.h"
#include "launcher_ui.h"

/* ── App module declarations ─────────────────────────── */

extern const app_info_t app_ai_assistant;
extern const app_info_t app_camera;
extern const app_info_t app_nes;
extern const app_info_t app_photos;
extern const app_info_t app_terminal;
extern const app_info_t app_music;
extern const app_info_t app_face;
extern const app_info_t app_imu;

/* ── LVGL Mouse input read callback ──────────────────── */

static void mouse_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	data->point.x = g_mouse.x;
	data->point.y = g_mouse.y;
	data->state = (g_mouse.buttons & 0x01)
		      ? LV_INDEV_STATE_PRESSED
		      : LV_INDEV_STATE_RELEASED;
}

/* ── LVGL Keyboard input read callback ──────────────── */

static uint32_t last_key;
static lv_indev_state_t last_state = LV_INDEV_STATE_RELEASED;

static void kb_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	struct kb_event evt;

	if (k_msgq_get(&kb_events, &evt, K_NO_WAIT) == 0) {
		last_key = evt.key;
		last_state = evt.pressed ? LV_INDEV_STATE_PRESSED
					: LV_INDEV_STATE_RELEASED;
	}
	data->key = last_key;
	data->state = last_state;
}

/* ── Global navigation: ESC / right-click → home ───── */

static void check_global_nav(void)
{
	if (!app_manager_is_app_active()) {
		return;
	}

	/* Right-click (button bit 1) → back to home */
	static bool prev_rclick;
	bool rclick = (g_mouse.buttons & 0x02) != 0;
	if (rclick && !prev_rclick) {
		app_manager_back_to_home();
	}
	prev_rclick = rclick;

	/* ESC key is handled via keyboard group event (LV_KEY_ESC = 27) */
	/* We check the msgq peek for ESC specifically */
	struct kb_event evt;
	if (k_msgq_peek(&kb_events, &evt) == 0) {
		if (evt.key == 27 && evt.pressed) {
			/* Consume the ESC event */
			k_msgq_get(&kb_events, &evt, K_NO_WAIT);
			app_manager_back_to_home();
		}
	}
}

/* ── Main ────────────────────────────────────────────── */

int main(void)
{
	printk("\n=== CHD-ESP32-S3-BOX Launcher ===\n");

	/* 1. Backlight ON (IO47 = GPIO1 pin 15) */
	const struct device *gpio1 = DEVICE_DT_GET(BLK_NODE);
	if (device_is_ready(gpio1)) {
		gpio_pin_configure(gpio1, BLK_PIN, GPIO_OUTPUT_ACTIVE);
		gpio_pin_set(gpio1, BLK_PIN, 1);
		printk("[HW] Backlight ON\n");
	}

	/* 2. Display device */
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display)) {
		printk("ERROR: Display not ready!\n");
		return 0;
	}

	/* 3. Init mouse to screen center */
	g_mouse.x = SCREEN_WIDTH / 2;
	g_mouse.y = SCREEN_HEIGHT / 2;

	/* 4. BLE HID Host (async — callbacks handle connection) */
	ble_hid_init();

	/* 5. LVGL Mouse Input */
	lv_indev_t *mouse_indev = lv_indev_create();
	lv_indev_set_type(mouse_indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(mouse_indev, mouse_read_cb);

	/* Mouse cursor: red circle with white border */
	lv_obj_t *cursor = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(cursor);
	lv_obj_set_size(cursor, 12, 12);
	lv_obj_set_style_radius(cursor, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(cursor, lv_palette_main(LV_PALETTE_RED), 0);
	lv_obj_set_style_bg_opa(cursor, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(cursor, 2, 0);
	lv_obj_set_style_border_color(cursor, lv_color_white(), 0);
	lv_indev_set_cursor(mouse_indev, cursor);

	/* 6. LVGL Keyboard Input */
	lv_indev_t *kb_indev = lv_indev_create();
	lv_indev_set_type(kb_indev, LV_INDEV_TYPE_KEYPAD);
	lv_indev_set_read_cb(kb_indev, kb_read_cb);

	lv_group_t *group = lv_group_create();
	lv_indev_set_group(kb_indev, group);
	app_manager_set_kb_group(group);

	/* 7. Register all apps */
	app_manager_register(&app_ai_assistant);
	app_manager_register(&app_camera);
	app_manager_register(&app_nes);
	app_manager_register(&app_photos);
	app_manager_register(&app_terminal);
	app_manager_register(&app_music);
	app_manager_register(&app_face);
	app_manager_register(&app_imu);

	/* 8. Build launcher home screen */
	launcher_ui_init();

	/* 9. First render + display on */
	lv_timer_handler();
	display_blanking_off(display);
	printk("[Launcher] Running...\n");

	/* ── Main loop ──────────────────────────────────── */
	while (1) {
		/* Global navigation check */
		check_global_nav();

		/* Update status bar */
		if (g_mouse.connected && g_kb_connected) {
			launcher_ui_update_status("Mouse+KB");
		} else if (g_mouse.connected) {
			char buf[48];
			snprintf(buf, sizeof(buf), "M: %s", g_mouse.name);
			launcher_ui_update_status(buf);
		} else if (g_kb_connected) {
			launcher_ui_update_status("KB only");
		} else {
			launcher_ui_update_status("BLE: Scanning...");
		}

		/* LVGL tick */
		uint32_t sleep_ms = lv_timer_handler();
		k_msleep(MIN(sleep_ms, 33));  /* Cap at ~30 FPS */
	}
}
