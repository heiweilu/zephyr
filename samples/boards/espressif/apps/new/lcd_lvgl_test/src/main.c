/*
 * CHD-ESP32-S3-BOX: LVGL Hello World on ST7789V 320x240
 * Step 4.3 — LVGL integration test
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

int main(void)
{
	const struct device *display_dev;
	const struct device *gpio1_dev;

	printk("=== LVGL Hello World Test ===\n");

	/* Backlight ON */
	gpio1_dev = DEVICE_DT_GET(BLK_NODE);
	if (device_is_ready(gpio1_dev)) {
		gpio_pin_configure(gpio1_dev, BLK_PIN, GPIO_OUTPUT_ACTIVE);
		gpio_pin_set(gpio1_dev, BLK_PIN, 1);
		printk("Backlight: ON\n");
	}

	/* Get display device */
	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display_dev)) {
		printk("ERROR: Display device not ready\n");
		return 0;
	}
	printk("Display device: %s\n", display_dev->name);

	/* Create LVGL label */
	lv_obj_t *label = lv_label_create(lv_screen_active());
	lv_label_set_text(label, "Hello World!");
	lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

	/* Create counter label */
	lv_obj_t *count_label = lv_label_create(lv_screen_active());
	lv_obj_align(count_label, LV_ALIGN_BOTTOM_MID, 0, -10);

	/* First render + turn on display */
	lv_timer_handler();
	display_blanking_off(display_dev);

	printk("LVGL running...\n");

	uint32_t count = 0;
	char buf[32];

	while (1) {
		if ((count % 100) == 0U) {
			snprintf(buf, sizeof(buf), "Count: %u", count / 100U);
			lv_label_set_text(count_label, buf);
		}
		lv_timer_handler();
		++count;
		k_sleep(K_MSEC(10));
	}
}
