/*
 * CHD-ESP32-S3-BOX: LVGL + BLE HID Mouse + Keyboard Input
 *
 * Displays a test UI with mouse cursor and keyboard navigation
 * driven by BLE-connected HID devices.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <lvgl.h>

#include "ble_hid.h"

/* Backlight: IO47 = GPIO1 pin 15 */
#define BLK_NODE DT_NODELABEL(gpio1)
#define BLK_PIN  15

static lv_obj_t *status_label;
static lv_obj_t *coord_label;
static lv_obj_t *click_label;
static lv_obj_t *key_label;
static int click_count;

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
		/* Update key display label */
		if (key_label && evt.pressed) {
			if (evt.key >= 0x20 && evt.key < 0x7F) {
				lv_label_set_text_fmt(key_label,
						      "Key: '%c'",
						      (char)evt.key);
			} else {
				lv_label_set_text_fmt(key_label,
						      "Key: 0x%02X",
						      evt.key);
			}
		}
	}
	data->key = last_key;
	data->state = last_state;
}

/* ── UI event callbacks ─────────────────────────────── */

static void btn_click_cb(lv_event_t *e)
{
	click_count++;
	lv_label_set_text_fmt(click_label, "Clicks: %d", click_count);
}

/* ── Main ────────────────────────────────────────────── */

int main(void)
{
	printk("=== CHD-ESP32-S3-BOX LVGL + BLE HID ===\n");

	/* Backlight ON */
	const struct device *gpio1 = DEVICE_DT_GET(BLK_NODE);
	if (device_is_ready(gpio1)) {
		gpio_pin_configure(gpio1, BLK_PIN, GPIO_OUTPUT_ACTIVE);
		gpio_pin_set(gpio1, BLK_PIN, 1);
	}

	/* Display */
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display)) {
		printk("Display not ready!\n");
		return 0;
	}

	/* Init mouse state to screen center */
	g_mouse.x = SCREEN_WIDTH / 2;
	g_mouse.y = SCREEN_HEIGHT / 2;

	/* Init BLE HID Host (async — callbacks handle connection) */
	ble_hid_init();

	/* ── LVGL Mouse Input Device ────────────────────── */

	lv_indev_t *mouse_indev = lv_indev_create();
	lv_indev_set_type(mouse_indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(mouse_indev, mouse_read_cb);

	/* Cursor: small red circle with white border */
	lv_obj_t *cursor = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(cursor);
	lv_obj_set_size(cursor, 12, 12);
	lv_obj_set_style_radius(cursor, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(cursor, lv_palette_main(LV_PALETTE_RED), 0);
	lv_obj_set_style_bg_opa(cursor, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(cursor, 2, 0);
	lv_obj_set_style_border_color(cursor, lv_color_white(), 0);
	lv_indev_set_cursor(mouse_indev, cursor);

	/* ── LVGL Keyboard Input Device ─────────────────── */

	lv_indev_t *kb_indev = lv_indev_create();
	lv_indev_set_type(kb_indev, LV_INDEV_TYPE_KEYPAD);
	lv_indev_set_read_cb(kb_indev, kb_read_cb);

	lv_group_t *group = lv_group_create();
	lv_indev_set_group(kb_indev, group);

	/* ── Simple Test UI ─────────────────────────────── */

	/* Status label (top) */
	status_label = lv_label_create(lv_screen_active());
	lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
	lv_label_set_text(status_label, "BLE: Scanning...");
	lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 5, 5);

	/* Coordinate label */
	coord_label = lv_label_create(lv_screen_active());
	lv_obj_set_style_text_font(coord_label, &lv_font_montserrat_14, 0);
	lv_label_set_text_fmt(coord_label, "X:%d Y:%d",
			      g_mouse.x, g_mouse.y);
	lv_obj_align(coord_label, LV_ALIGN_TOP_RIGHT, -5, 5);

	/* Test button (center) */
	lv_obj_t *btn = lv_button_create(lv_screen_active());
	lv_obj_set_size(btn, 160, 50);
	lv_obj_align(btn, LV_ALIGN_CENTER, 0, -20);
	lv_obj_add_event_cb(btn, btn_click_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *btn_lbl = lv_label_create(btn);
	lv_label_set_text(btn_lbl, "Click Me!");
	lv_obj_center(btn_lbl);
	lv_group_add_obj(group, btn);

	/* Second test button */
	lv_obj_t *btn2 = lv_button_create(lv_screen_active());
	lv_obj_set_size(btn2, 160, 50);
	lv_obj_align(btn2, LV_ALIGN_CENTER, 0, 40);
	lv_obj_add_event_cb(btn2, btn_click_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *btn2_lbl = lv_label_create(btn2);
	lv_label_set_text(btn2_lbl, "Button 2");
	lv_obj_center(btn2_lbl);
	lv_group_add_obj(group, btn2);

	/* Click counter (bottom) */
	click_label = lv_label_create(lv_screen_active());
	lv_obj_set_style_text_font(click_label, &lv_font_montserrat_16, 0);
	lv_label_set_text(click_label, "Clicks: 0");
	lv_obj_align(click_label, LV_ALIGN_BOTTOM_MID, 0, -30);

	/* Key display (bottom) */
	key_label = lv_label_create(lv_screen_active());
	lv_obj_set_style_text_font(key_label, &lv_font_montserrat_14, 0);
	lv_label_set_text(key_label, "Key: --");
	lv_obj_align(key_label, LV_ALIGN_BOTTOM_MID, 0, -10);

	/* First LVGL tick + display on */
	lv_timer_handler();
	display_blanking_off(display);
	printk("LVGL + BLE running...\n");

	/* ── Main loop ──────────────────────────────────── */

	while (1) {
		/* Update status labels */
		if (g_mouse.connected && g_kb_connected) {
			lv_label_set_text(status_label, "Mouse+KB OK");
		} else if (g_mouse.connected) {
			lv_label_set_text_fmt(status_label, "M: %s",
					      g_mouse.name);
		} else if (g_kb_connected) {
			lv_label_set_text(status_label, "KB connected");
		} else {
			lv_label_set_text(status_label, "BLE: Scanning...");
		}

		if (g_mouse.connected) {
			lv_label_set_text_fmt(coord_label, "X:%d Y:%d",
					      g_mouse.x, g_mouse.y);
		}

		uint32_t sleep_ms = lv_timer_handler();
		k_msleep(MIN(sleep_ms, 33));
	}
}
