/*
 * CHD-ESP32-S3-BOX: LVGL Demo on ST7789V 320x240
 * Supports: Widgets demo, Benchmark, or simple Hello World
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <lvgl.h>

#if defined(CONFIG_LV_USE_DEMO_WIDGETS) || defined(CONFIG_LV_USE_DEMO_BENCHMARK)
#include <lv_demos.h>
#endif

/* Backlight: IO47 = GPIO1 pin 15 */
#define BLK_NODE DT_NODELABEL(gpio1)
#define BLK_PIN  15

int main(void)
{
	const struct device *display_dev;
	const struct device *gpio1_dev;

	printk("=== CHD-ESP32-S3-BOX LVGL Demo ===\n");

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

#if defined(CONFIG_LV_USE_DEMO_WIDGETS)
	printk("Running Widgets demo...\n");
	lv_demo_widgets();
#elif defined(CONFIG_LV_USE_DEMO_BENCHMARK)
	printk("Running Benchmark demo...\n");
	lv_demo_benchmark();
#else
	/* Fallback: simple Hello World */
	lv_obj_t *label = lv_label_create(lv_screen_active());
	lv_label_set_text(label, "Hello World!");
	lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
#endif

	lv_timer_handler();
	display_blanking_off(display_dev);

	printk("LVGL running...\n");

	while (1) {
		uint32_t sleep_ms = lv_timer_handler();
		k_msleep(MIN(sleep_ms, INT32_MAX));
	}
}
