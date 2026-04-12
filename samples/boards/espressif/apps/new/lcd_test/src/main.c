#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>

/* Backlight on IO47 */
#define BLK_NODE DT_NODELABEL(gpio1)
#define BLK_PIN  (47 - 32) /* GPIO47 is on gpio1, pin 15 */

static void fill_screen(const struct device *display, uint16_t color)
{
	struct display_buffer_descriptor desc;
	struct display_capabilities caps;

	display_get_capabilities(display, &caps);

	uint16_t w = caps.x_resolution;
	uint16_t h = caps.y_resolution;

	/* Fill one row at a time to save RAM */
	uint16_t buf[320];

	for (int i = 0; i < w; i++) {
		buf[i] = color;
	}

	desc.buf_size = w * sizeof(uint16_t);
	desc.width = w;
	desc.height = 1;
	desc.pitch = w;

	for (int y = 0; y < h; y++) {
		display_write(display, 0, y, &desc, buf);
	}
}

int main(void)
{
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));

	printk("\n*** CHD-ESP32-S3-BOX LCD Test ***\n");

	/* Turn on backlight (IO47 = gpio1 pin 15) */
	if (device_is_ready(gpio1)) {
		gpio_pin_configure(gpio1, BLK_PIN, GPIO_OUTPUT_ACTIVE);
		gpio_pin_set(gpio1, BLK_PIN, 1);
		printk("Backlight ON (IO47)\n");
	} else {
		printk("WARNING: gpio1 not ready, backlight may be off\n");
	}

	if (!device_is_ready(display)) {
		printk("ERROR: Display device not ready\n");
		return -1;
	}

	printk("Display: %s\n", display->name);

	struct display_capabilities caps;
	display_get_capabilities(display, &caps);
	printk("Resolution: %dx%d, pixel_format=%d\n",
	       caps.x_resolution, caps.y_resolution,
	       caps.current_pixel_format);

	display_blanking_off(display);

	/* Color cycle: Red, Green, Blue */
	const struct {
		uint16_t color;
		const char *name;
	} colors[] = {
		{0xF800, "RED"},
		{0x07E0, "GREEN"},
		{0x001F, "BLUE"},
		{0xFFFF, "WHITE"},
		{0x0000, "BLACK"},
	};

	while (1) {
		for (int i = 0; i < ARRAY_SIZE(colors); i++) {
			printk("Filling: %s (0x%04x)\n",
			       colors[i].name, colors[i].color);
			fill_screen(display, colors[i].color);
			k_msleep(2000);
		}
	}

	return 0;
}
