/*
 * Copyright (c) 2025 Hei Weilu
 * SPDX-License-Identifier: Apache-2.0
 */

/* Simple MQTT LED sample placeholder: will be expanded to connect Wi-Fi and MQTT. */

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/util.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>

LOG_MODULE_REGISTER(main);

/* Assuming an alias is defined in the device tree */
#define STRIP_NODE DT_ALIAS(led_strip)

#if DT_NODE_HAS_PROP(DT_ALIAS(led_strip), chain_length)
#define STRIP_NUM_PIXELS DT_PROP(DT_ALIAS(led_strip), chain_length)
#else
#error Unable to determine length of LED strip
#endif
/* Delay between LED updates */
#define DELAY_TIME K_MSEC(CONFIG_SAMPLE_LED_UPDATE_DELAY)

static const struct device *strip = DEVICE_DT_GET(STRIP_NODE);

#define RGB(_r, _g, _b) {.r = (_r), .g = (_g), .b = (_b)}

static const struct led_rgb colors[] = {
	RGB(CONFIG_SAMPLE_LED_BRIGHTNESS, 0x00, 0x00), /* red */
	RGB(0x00, CONFIG_SAMPLE_LED_BRIGHTNESS, 0x00), /* green */
	RGB(0x00, 0x00, CONFIG_SAMPLE_LED_BRIGHTNESS), /* blue */
};

static struct led_rgb pixels[STRIP_NUM_PIXELS];

int main(void)
{
	size_t color = 0;
	int rc;

	LOG_INF(" We will test MQTT LED control\n");
	if (device_is_ready(strip)) {
		LOG_INF("LED strip device is ready\n");
	} else {
		LOG_ERR("LED strip device is not ready\n");
		return 0;
	}

	while (1) {
		for (size_t cursor = 0; cursor < ARRAY_SIZE(pixels); cursor++) {
			memset(&pixels[cursor], 0x00, sizeof(pixels));
			memcpy(&pixels[cursor], &colors[color], sizeof(struct led_rgb));

			rc = led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
			if (rc < 0) {
				LOG_ERR("Failed to update LED strip: %d", rc);
				return 0;
			}

			k_sleep(DELAY_TIME);
		}
		color = (color + 1) % ARRAY_SIZE(colors);
	}

	return 0;
}
