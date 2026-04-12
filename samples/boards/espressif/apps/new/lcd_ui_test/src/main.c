/*
 * CHD-ESP32-S3-BOX: LVGL Multi-Button UI Test
 * Step 4.4 — Buttons with event callbacks
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <lvgl.h>

/* Backlight: IO47 = GPIO1 pin 15 */
#define BLK_NODE DT_NODELABEL(gpio1)
#define BLK_PIN  15

static lv_obj_t *status_label;
static int counter = 0;
static const lv_color_t bg_colors[] = {
	LV_COLOR_MAKE(0x00, 0x00, 0x00),  /* Black */
	LV_COLOR_MAKE(0x00, 0x00, 0x80),  /* Dark Blue */
	LV_COLOR_MAKE(0x00, 0x80, 0x00),  /* Dark Green */
	LV_COLOR_MAKE(0x80, 0x00, 0x00),  /* Dark Red */
	LV_COLOR_MAKE(0x40, 0x40, 0x40),  /* Dark Gray */
};
static int bg_index = 0;

static void btn_counter_cb(lv_event_t *e)
{
	counter++;
	char buf[32];
	snprintf(buf, sizeof(buf), "Count: %d", counter);
	lv_label_set_text(status_label, buf);
	printk("Button: Counter = %d\n", counter);
}

static void btn_reset_cb(lv_event_t *e)
{
	counter = 0;
	lv_label_set_text(status_label, "Count: 0");
	printk("Button: Counter reset\n");
}

static void btn_color_cb(lv_event_t *e)
{
	bg_index = (bg_index + 1) % (sizeof(bg_colors) / sizeof(bg_colors[0]));
	lv_obj_set_style_bg_color(lv_screen_active(), bg_colors[bg_index], 0);
	printk("Button: BG color index = %d\n", bg_index);
}

static lv_obj_t *create_button(lv_obj_t *parent, const char *text,
			       lv_coord_t x, lv_coord_t y,
			       lv_coord_t w, lv_coord_t h,
			       lv_event_cb_t cb)
{
	lv_obj_t *btn = lv_button_create(parent);
	lv_obj_set_size(btn, w, h);
	lv_obj_align(btn, LV_ALIGN_CENTER, x, y);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, text);
	lv_obj_center(label);

	return btn;
}

int main(void)
{
	const struct device *display_dev;
	const struct device *gpio1_dev;

	printk("=== LVGL UI Test ===\n");

	/* Backlight ON */
	gpio1_dev = DEVICE_DT_GET(BLK_NODE);
	if (device_is_ready(gpio1_dev)) {
		gpio_pin_configure(gpio1_dev, BLK_PIN, GPIO_OUTPUT_ACTIVE);
		gpio_pin_set(gpio1_dev, BLK_PIN, 1);
	}

	/* Get display device */
	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display_dev)) {
		printk("ERROR: Display device not ready\n");
		return 0;
	}

	/* Set dark background */
	lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);

	/* Title */
	lv_obj_t *title = lv_label_create(lv_screen_active());
	lv_label_set_text(title, "CHD-ESP32-S3-BOX");
	lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
	lv_obj_set_style_text_color(title, lv_color_white(), 0);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

	/* Status label */
	status_label = lv_label_create(lv_screen_active());
	lv_label_set_text(status_label, "Count: 0");
	lv_obj_set_style_text_color(status_label, lv_color_hex(0x00FF00), 0);
	lv_obj_set_style_text_font(status_label, &lv_font_montserrat_18, 0);
	lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -30);

	/* Buttons row */
	create_button(lv_screen_active(), "+1", -100, 40, 80, 50, btn_counter_cb);
	create_button(lv_screen_active(), "Reset", 0, 40, 80, 50, btn_reset_cb);
	create_button(lv_screen_active(), "Color", 100, 40, 80, 50, btn_color_cb);

	/* First render + turn on display */
	lv_timer_handler();
	display_blanking_off(display_dev);

	printk("UI ready. Buttons: +1, Reset, Color\n");

	while (1) {
		lv_timer_handler();
		k_sleep(K_MSEC(10));
	}
}
