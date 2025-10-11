/*
 * Based on ST7789V sample with LVGL label display:
 * Copyright (c) 2025 Heiweilu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include <lvgl_input_device.h>

#ifdef CONFIG_LVGL
#include <lvgl.h>
#endif

LV_FONT_DECLARE(lv_font_unscii_8)


#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define TFT_NODE DT_CHOSEN(zephyr_display)

#if DT_NODE_EXISTS(TFT_NODE)
static const struct gpio_dt_spec backlight = GPIO_DT_SPEC_GET(DT_ALIAS(backlight), gpios);
#endif

/* Test Mode Configuration Macros
 * Usage:
 * 1. Only test LCD functionality: ENABLE_BASIC_LCD_TEST=1, ENABLE_LVGL_TEST=0
 * 2. Only test LVGL functionality: ENABLE_BASIC_LCD_TEST=0, ENABLE_LVGL_TEST=1
 * 3. Disabling both will cause an error
 */
#define ENABLE_LVGL_TEST      1 /* 1: Enable LVGL test with standard APIs, 0: Disable */
#define ENABLE_BASIC_LCD_TEST 0 /* 1: Enable basic LCD test, 0: Disable */

#if ENABLE_BASIC_LCD_TEST
/* Draw a border */
static void draw_border(uint16_t *buf, int width, int height, uint16_t color)
{
	/* Draw a thicker top and bottom border (2 pixels thick) */
	for (int x = 0; x < width; x++) {
		buf[x] = color;                        /* Top row */
		buf[width + x] = color;                /* Second row */
		buf[(height - 2) * width + x] = color; /* Second to last row */
		buf[(height - 1) * width + x] = color; /* Last row */
	}
	/* Draw a thicker left and right border (2 pixels thick) */
	for (int y = 0; y < height; y++) {
		buf[y * width] = color;               /* Left column */
		buf[y * width + 1] = color;           /* Second column */
		buf[y * width + (width - 2)] = color; /* Second to last column */
		buf[y * width + (width - 1)] = color; /* Last column */
	}
}

/* Simple 8x8 pixel font data (0-9) */
static const uint8_t font_8x8_digits[10][8] = {
	{0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00}, /* 0 */
	{0x18, 0x18, 0x38, 0x18, 0x18, 0x18, 0x7E, 0x00}, /* 1 */
	{0x3C, 0x66, 0x06, 0x0C, 0x30, 0x60, 0x7E, 0x00}, /* 2 */
	{0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00}, /* 3 */
	{0x06, 0x0E, 0x1E, 0x66, 0x7F, 0x06, 0x06, 0x00}, /* 4 */
	{0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00}, /* 5 */
	{0x3C, 0x66, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00}, /* 6 */
	{0x7E, 0x66, 0x0C, 0x18, 0x18, 0x18, 0x18, 0x00}, /* 7 */
	{0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00}, /* 8 */
	{0x3C, 0x66, 0x66, 0x3E, 0x06, 0x66, 0x3C, 0x00}, /* 9 */
};

/* Simple letter font data (partial letters A-Z) */
static const uint8_t font_8x8_letters[26][8] = {
	{0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00}, /* A */
	{0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00}, /* B */
	{0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00}, /* C */
	{0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00}, /* D */
	{0x7E, 0x60, 0x60, 0x78, 0x60, 0x60, 0x7E, 0x00}, /* E */
	{0x7E, 0x60, 0x60, 0x78, 0x60, 0x60, 0x60, 0x00}, /* F */
	{0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00}, /* G */
	{0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00}, /* H */
	{0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00}, /* I */
	{0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x6C, 0x38, 0x00}, /* J */
	{0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00}, /* K */
	{0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00}, /* L */
	{0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00}, /* M */
	{0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x00}, /* N */
	{0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}, /* O */
	{0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00}, /* P */
	{0x3C, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x0E, 0x00}, /* Q */
	{0x7C, 0x66, 0x66, 0x7C, 0x78, 0x6C, 0x66, 0x00}, /* R */
	{0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00}, /* S */
	{0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, /* T */
	{0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}, /* U */
	{0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00}, /* V */
	{0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00}, /* W */
	{0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00}, /* X */
	{0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00}, /* Y */
	{0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00}, /* Z */
};

/* Draw an 8x8 character */
static void draw_char_8x8(uint16_t *buf, int width, int height, char c, int x, int y,
			  uint16_t color)
{
	const uint8_t *font_data = NULL;

	if (c >= '0' && c <= '9') {
		font_data = font_8x8_digits[c - '0'];
	} else if (c >= 'A' && c <= 'Z') {
		font_data = font_8x8_letters[c - 'A'];
	} else if (c >= 'a' && c <= 'z') {
		font_data = font_8x8_letters[c - 'a']; /* Lowercase is translated to uppercase */
	} else {
		return; /* Unsupported character */
	}

	for (int row = 0; row < 8; row++) {
		uint8_t line = font_data[row];
		for (int col = 0; col < 8; col++) {
			if (line & (0x80 >> col)) {
				int px = x + col;
				int py = y + row;
				if (px >= 0 && px < width && py >= 0 && py < height) {
					buf[py * width + px] = color;
				}
			}
		}
	}
}

/* Draw a number */
static void draw_number(uint16_t *buf, int width, int height, int number, int x, int y,
			uint16_t color)
{
	char num_str[16];
	snprintf(num_str, sizeof(num_str), "%d", number);

	int char_x = x;
	for (int i = 0; num_str[i] != '\0'; i++) {
		draw_char_8x8(buf, width, height, num_str[i], char_x, y, color);
		char_x += 9; /* 8 pixel character width + 1 pixel spacing */
	}
}

/* Draw a float */
static void draw_float(uint16_t *buf, int width, int height, float number, int x, int y,
		       uint16_t color)
{
	char num_str[16];
	snprintf(num_str, sizeof(num_str), "%.1f", (double)number);

	int char_x = x;
	for (int i = 0; num_str[i] != '\0'; i++) {
		if (num_str[i] == '.') {
			/* Draw point */
			if (char_x + 3 < width && y + 7 < height) {
				buf[(y + 7) * width + (char_x + 3)] = color;
				buf[(y + 7) * width + (char_x + 4)] = color;
			}
			char_x += 6;
		} else {
			draw_char_8x8(buf, width, height, num_str[i], char_x, y, color);
			char_x += 9;
		}
	}
}

/* Draw text */
static void draw_text(uint16_t *buf, int width, int height, const char *text, int x, int y,
		      uint16_t color)
{
	int char_x = x;
	for (int i = 0; text[i] != '\0'; i++) {
		if (text[i] == ' ') {
			char_x += 9; /* Space width */
		} else {
			draw_char_8x8(buf, width, height, text[i], char_x, y, color);
			char_x += 9;
		}
	}
}
#endif /* ENABLE_BASIC_LCD_TEST */

#if ENABLE_BASIC_LCD_TEST
/**
 * @brief Basic LCD test function
 * @param disp Display device pointer
 * @return 0 on success, negative error code on failure
 */
static int test_basic_lcd(const struct device *disp)
{
	LOG_INF("===== Running basic LCD test =====");

	/* Get display capabillities */
	struct display_capabilities caps;
	display_get_capabilities(disp, &caps);
	LOG_INF("Display: %dx%d, pixel format: %d", caps.x_resolution, caps.y_resolution,
		caps.current_pixel_format);

	/* Check if the pixel format is RGB565(16 bits per pixel) */
	if (caps.current_pixel_format != PIXEL_FORMAT_RGB_565) {
		LOG_ERR("Unsupported pixel format: %d (expected RGB565)",
			caps.current_pixel_format);
		return -ENOTSUP;
	}

	/* Use static buffers */
	static uint16_t test_buf[240 * 135]; /* RGB565: 2 bytes per pixel, Landscape mode */
	size_t buf_size = sizeof(test_buf);

	LOG_INF("Using static buffer of %zu bytes", buf_size);

	/* Set the buffer descriptor */
	struct display_buffer_descriptor desc = {
		.buf_size = buf_size,
		.width = caps.x_resolution,
		.height = caps.y_resolution,
		.pitch = caps.x_resolution,
		.frame_incomplete = false,
	};

	/* Turn off display blanking mode */
	display_blanking_off(disp);
	LOG_INF("Display blanking turned off");

	/* Define color array (RGB565 format) */
	uint16_t colors[] = {0xF800, 0x001F, 0x07E0, 0xFFFF,
			     0x0000}; /* Red, Blue, Green, White, Black */
	const char *color_names[] = {"Red", "Blue", "Green", "White", "Black"};

	/* Test each color one by one */
	for (int color_idx = 0; color_idx < 5; color_idx++) {
		/* Fill the entire buffer with the specified color */
		for (int i = 0; i < caps.x_resolution * caps.y_resolution; i++) {
			test_buf[i] = colors[color_idx];
		}

		LOG_INF("Testing %s screen...", color_names[color_idx]);
		int ret = display_write(disp, 0, 0, &desc, test_buf);
		if (ret != 0) {
			LOG_ERR("Display write failed: %d", ret);
			return ret;
		} else {
			LOG_INF("%s screen displayed successfully", color_names[color_idx]);
		}

		k_msleep(2000);
	}

	/* Test landscape pattern */
	LOG_INF("Testing landscape pattern...");

	/* Create a horizontal stripes pattern to confirm orientation */
	for (int y = 0; y < caps.y_resolution; y++) {
		for (int x = 0; x < caps.x_resolution; x++) {
			int idx = y * caps.x_resolution + x;

			if (y < caps.y_resolution / 4) {
				test_buf[idx] = 0xF800; /* Red */
			} else if (y < caps.y_resolution / 2) {
				test_buf[idx] = 0x07E0; /* Green */
			} else if (y < 3 * caps.y_resolution / 4) {
				test_buf[idx] = 0x001F; /* Blue */
			} else {
				test_buf[idx] = 0xFFFF; /* White */
			}
		}
	}

	int ret = display_write(disp, 0, 0, &desc, test_buf);
	LOG_INF("Horizontal stripes pattern displayed: %d", ret);
	k_msleep(3000);

	/* Create a portrait pattern to confirm orientation */
	LOG_INF("Testing portrait pattern...");
	for (int y = 0; y < caps.y_resolution; y++) {
		for (int x = 0; x < caps.x_resolution; x++) {
			int idx = y * caps.x_resolution + x;

			if (x < caps.x_resolution / 4) {
				test_buf[idx] = 0xF800; /* Red */
			} else if (x < caps.x_resolution / 2) {
				test_buf[idx] = 0x07E0; /* Green */
			} else if (x < 3 * caps.x_resolution / 4) {
				test_buf[idx] = 0x001F; /* Blue */
			} else {
				test_buf[idx] = 0xFFFF; /* White */
			}
		}
	}

	ret = display_write(disp, 0, 0, &desc, test_buf);
	LOG_INF("Vertical stripes pattern displayed: %d", ret);
	k_msleep(3000);

	/* Main loop for number and text display */
	int counter = 0;
	float number = 123.456;
	const char *messages[] = {"Hello ESP32-S3!", "Display Works!", "Numbers & Text",
				  "ST7789V TFT OK"};

	while (1) {
		/* Clear the buffer to black */
		for (int i = 0; i < caps.x_resolution * caps.y_resolution; i++) {
			test_buf[i] = 0x0000; /* Black background */
		}

		/* Draw a simple border */
		draw_border(test_buf, caps.x_resolution, caps.y_resolution, 0xFFFF);

		/* Display counter number - top left, avoiding border */
		draw_number(test_buf, caps.x_resolution, caps.y_resolution, counter, 10, 20,
			    0xF800); /* Red */

		/* Display float - middle top */
		draw_float(test_buf, caps.x_resolution, caps.y_resolution, number + counter * 0.1f,
			   10, 40, 0x07E0); /* Green */

		/* Display text message - middle */
		int msg_idx = (counter / 10) % 4; /* Change message every 10 loops */
		draw_text(test_buf, caps.x_resolution, caps.y_resolution, messages[msg_idx], 10, 70,
			  0x001F); /* Blue */

		/* Display status information - bottom but within border */
		char status[32];
		snprintf(status, sizeof(status), "Loop: %d", counter);
		draw_text(test_buf, caps.x_resolution, caps.y_resolution, status, 10, 100,
			  0xFFE0); /* Yellow */

		/* Add more landscape layout information - right side, avoiding border */
		char info[32];
		snprintf(info, sizeof(info), "240x135");
		draw_text(test_buf, caps.x_resolution, caps.y_resolution, info, 150, 20,
			  0xF81F); /* Purple */

		/* Write to display */
		ret = display_write(disp, 0, 0, &desc, test_buf);
		if (ret == 0) {
			LOG_INF("Frame %d: Counter=%d, Number=%.1f, Message='%s'", counter, counter,
				(double)(number + counter * 0.1f), messages[msg_idx]);
		} else {
			LOG_ERR("Display write failed: %d", ret);
		}

		counter++;
		k_msleep(1000); /* 1 second */
	}

	return 0;
}
#endif /* ENABLE_BASIC_LCD_TEST */

/* Simple icon data - 16x16 pixels using Unicode symbols */
#define ICON_SETTINGS "\xE2\x9A\x99"    /* Gear symbol */ 
#define ICON_BATTERY "\xF0\x9F\x94\x8B"  /* Battery symbol */
#define ICON_CLOCK   "\xF0\x9F\x95\x90"  /* Clock symbol */
#define ICON_TEMP    "\xF0\x9F\x8C\xA1"  /* Thermometer */

/* Simple image patterns (16x16 icons) - RGB565 format */
static const uint16_t icon_home[16*16] = {
    /* Simple house icon pattern */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x07E0,0x07E0,0x07E0,0x07E0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x0000,0x0000,0x0000,
    0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x0000,0x0000,
    0x0000,0x0000,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x07E0,0x07E0,0x07E0,0xF800,0xF800,0x07E0,0x07E0,0xF800,0xF800,0x07E0,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x07E0,0x07E0,0x07E0,0xF800,0xF800,0x07E0,0x07E0,0xF800,0xF800,0x07E0,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x07E0,0x07E0,0x07E0,0x07E0,0xFFE0,0xFFE0,0xFFE0,0x07E0,0x07E0,0x07E0,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x07E0,0x07E0,0x07E0,0x07E0,0xFFE0,0xFFE0,0xFFE0,0x07E0,0x07E0,0x07E0,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
};

/* Weather icon patterns */
static const uint16_t icon_sun[16*16] = {
    0x0000,0x0000,0x0000,0x0000,0xFFE0,0x0000,0x0000,0x0000,0x0000,0x0000,0xFFE0,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0xFFE0,0x0000,0x0000,0x0000,0x0000,0x0000,0xFFE0,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0xFFE0,0x0000,0x0000,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0x0000,0x0000,0xFFE0,0x0000,
    0x0000,0x0000,0x0000,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0x0000,0x0000,0x0000,
    0x0000,0x0000,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0x0000,0x0000,0x0000,
    0x0000,0x0000,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0x0000,0x0000,0x0000,
    0x0000,0x0000,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0x0000,0x0000,0x0000,
    0x0000,0x0000,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0x0000,0x0000,0x0000,0x0000,
    0xFFE0,0x0000,0x0000,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0xFFE0,0x0000,0x0000,0xFFE0,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0xFFE0,0x0000,0x0000,0x0000,0x0000,0x0000,0xFFE0,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0xFFE0,0x0000,0x0000,0x0000,0x0000,0x0000,0xFFE0,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
};

/* GUI Application Structure */
typedef struct {
    lv_obj_t *main_screen;	
    lv_obj_t *status_bar;
    lv_obj_t *content_area;
    lv_obj_t *bottom_nav;
    
    /* Status bar widgets */
    lv_obj_t *time_label;
    lv_obj_t *battery_icon;
    
    /* Content widgets */
    lv_obj_t *temp_card;
    
    /* Navigation buttons */
    lv_obj_t *home_btn;			
    
    uint32_t last_update;
    int current_page;
} gui_app_t;

static gui_app_t app = {0};

/* Event handlers */
static void nav_btn_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        lv_obj_t *btn = lv_event_get_target_obj(e);
        int page = (int)(intptr_t)lv_event_get_user_data(e);
        app.current_page = page;
        LOG_INF("Navigation: Page %d selected", page);
        
        /* Update button styles to show active state */
        lv_obj_set_style_bg_color(app.home_btn, lv_color_make(0x40, 0x40, 0x40), 0);
        
        lv_obj_set_style_bg_color(btn, lv_color_make(0x00, 0x80, 0xFF), 0);
    }
}

/* Create a simple colored rectangle as icon placeholder */
static lv_obj_t *create_icon_image(lv_obj_t *parent, const uint16_t *icon_data, int x, int y)
{
    /* Create a simple colored rectangle for now */
    lv_obj_t *icon_rect = lv_obj_create(parent);
    lv_obj_set_size(icon_rect, 16, 16);
    lv_obj_set_pos(icon_rect, x, y);
    
    /* Set color based on icon type */
    lv_color_t icon_color = lv_color_hex(0x07E0); /* Default green */
	if (icon_data == icon_sun) {
        icon_color = lv_color_hex(0xFFE0); /* Yellow for sun */
    }
    
    lv_obj_set_style_bg_color(icon_rect, icon_color, 0);
    lv_obj_set_style_radius(icon_rect, 2, 0);
    lv_obj_set_style_border_width(icon_rect, 1, 0);
    lv_obj_set_style_border_color(icon_rect, lv_color_white(), 0);
    
    /* Add a simple text label to identify the icon */
    lv_obj_t *icon_label = lv_label_create(icon_rect);
	if (icon_data == icon_sun) {
        lv_label_set_text(icon_label, "S");
    } else {
        lv_label_set_text(icon_label, "?");
    }
    lv_obj_set_style_text_color(icon_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(icon_label, &lv_font_unscii_8, 0);
    lv_obj_center(icon_label);
    
    return icon_rect;
}

/* Create status bar */
static void create_status_bar(void)
{
    app.status_bar = lv_obj_create(app.main_screen);

	/* Set style */
    lv_obj_set_size(app.status_bar, 240, 25);
    lv_obj_set_pos(app.status_bar, 0, 0);
    lv_obj_set_style_bg_color(app.status_bar, lv_color_make(0x20, 0x20, 0x20), 0);
    lv_obj_set_style_radius(app.status_bar, 30, 0);				
    lv_obj_set_style_border_width(app.status_bar, 0, 0);
    lv_obj_set_style_pad_all(app.status_bar, 2, 0);
    
    /* Time label */
    app.time_label = lv_label_create(app.status_bar);
	lv_obj_set_style_text_font(app.time_label, &lv_font_unscii_8, 0);  
    lv_label_set_text(app.time_label, "12:34");
    lv_obj_set_style_text_color(app.time_label, lv_color_white(), 0);
    lv_obj_set_pos(app.time_label, 5, 3);
    
    /* Sun icon */
    create_icon_image(app.status_bar, icon_sun, 120, 5);
    
    /* Battery text (simple) */
    app.battery_icon = lv_label_create(app.status_bar);
	lv_obj_set_style_text_font(app.battery_icon, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(app.battery_icon, lv_color_make(0xFF, 0xFF, 0xFF), 0);
    lv_label_set_text(app.battery_icon, "85%");
    lv_obj_set_pos(app.battery_icon, 200, 3);
}

/* Create content area with cards */
static void create_content_area(void)
{
    app.content_area = lv_obj_create(app.main_screen);
    lv_obj_set_size(app.content_area, 240, 85);
    lv_obj_set_pos(app.content_area, 0, 25);
    lv_obj_set_style_bg_color(app.content_area, lv_color_make(0x10, 0x10, 0x10), 0);
    lv_obj_set_style_radius(app.content_area, 0, 0);
    lv_obj_set_style_border_width(app.content_area, 0, 0);
    lv_obj_set_style_pad_all(app.content_area, 5, 0);
    
    /* Temperature Card */
    app.temp_card = lv_obj_create(app.content_area);
    lv_obj_set_size(app.temp_card, 70, 60);
    lv_obj_set_pos(app.temp_card, 5, 5);
    lv_obj_set_style_bg_color(app.temp_card, lv_color_make(0xFF, 0x40, 0x40), 0);
    lv_obj_set_style_radius(app.temp_card, 8, 0);
    lv_obj_set_style_border_width(app.temp_card, 0, 0);
    
    lv_obj_t *temp_label = lv_label_create(app.temp_card);
    lv_label_set_text(temp_label, "TEMP\n23.5°C");
	lv_obj_set_style_text_font(temp_label, &lv_font_unscii_8, LV_PART_MAIN);
    lv_obj_set_style_text_color(temp_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(temp_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(temp_label);
    
}

/* Create bottom navigation */
static void create_bottom_navigation(void)
{
    app.bottom_nav = lv_obj_create(app.main_screen);
    lv_obj_set_size(app.bottom_nav, 240, 25);
    lv_obj_set_pos(app.bottom_nav, 0, 110);
    lv_obj_set_style_bg_color(app.bottom_nav, lv_color_make(0x30, 0x30, 0x30), 0);
    lv_obj_set_style_radius(app.bottom_nav, 0, 0);
    lv_obj_set_style_border_width(app.bottom_nav, 0, 0);
    lv_obj_set_style_pad_all(app.bottom_nav, 2, 0);
    
    /* Home button */
    app.home_btn = lv_btn_create(app.bottom_nav);
    lv_obj_set_size(app.home_btn, 60, 20);
    lv_obj_set_pos(app.home_btn, 10, 2);
    lv_obj_set_style_bg_color(app.home_btn, lv_color_make(0x00, 0x80, 0xFF), 0);
    lv_obj_set_style_radius(app.home_btn, 3, 0);
    lv_obj_add_event_cb(app.home_btn, nav_btn_event_handler, LV_EVENT_CLICKED, (void*)0);
    
    lv_obj_t *home_label = lv_label_create(app.home_btn);
    lv_label_set_text(home_label, "HOME");
    lv_obj_set_style_text_color(home_label, lv_color_white(), 0);
	lv_obj_set_style_text_font(home_label, &lv_font_unscii_8, LV_PART_MAIN);
    lv_obj_center(home_label);
    
}

/* Update GUI data */
static void update_gui_data(uint32_t counter)
{
    /* Update time */
    uint32_t seconds = counter / 10;
    uint32_t minutes = (seconds / 60) % 60;
    uint32_t hours = (minutes / 60) % 24;
    lv_label_set_text_fmt(app.time_label, "%02d:%02d", (int)hours, (int)minutes);
    
    /* Update sensor data simulation */
    float temp = 20.0f + (counter % 100) * 0.1f;
    float humidity = 50.0f + (counter % 50) * 0.8f;
    int pressure = 1000 + (counter % 50);
    
    /* Find and update labels in cards */
    lv_obj_t *temp_child = lv_obj_get_child(app.temp_card, 0);
    if (temp_child) {
        lv_label_set_text_fmt(temp_child, "TEMP\n%.1f°C", (double)temp);
    }
    
    /* Update battery percentage */
    int battery = 100 - (counter % 100);
    lv_color_t bat_color = lv_color_make(0x00, 0xFF, 0x00); /* Green */
    if (battery < 30) bat_color = lv_color_make(0xFF, 0x80, 0x00); /* Orange */
    if (battery < 15) bat_color = lv_color_make(0xFF, 0x00, 0x00); /* Red */
    
    lv_label_set_text_fmt(app.battery_icon, "%d%%", battery);
    lv_obj_set_style_text_color(app.battery_icon, bat_color, 0);
}

/* Modern GUI Application with icons and images */
static int test_modern_gui(const struct device *disp)
{
    struct display_capabilities caps;
    display_get_capabilities(disp, &caps);
    LOG_INF("GUI Display: %dx%d, pixel format: %d", caps.x_resolution, caps.y_resolution,
        caps.current_pixel_format);

    display_blanking_off(disp);
    k_msleep(500);

    /* Initialize application */
    app.main_screen = lv_screen_active();
    lv_obj_set_style_bg_color(app.main_screen, lv_color_black(), LV_PART_MAIN);	/* background  Color  */
    lv_obj_set_style_bg_opa(app.main_screen, LV_OPA_COVER, LV_PART_MAIN);		/* Full background */ 
    
    app.current_page = 0;
    app.last_update = k_uptime_get_32();  	/* Initialize last update time */

    /* Create GUI components */
    create_status_bar();
    create_content_area();
    create_bottom_navigation();

    uint32_t counter = 0;
    
    /* Initial refresh */
    lv_timer_handler();

    while (1) {
        /* Update GUI every second */
        if (counter % 10 == 0) {
            update_gui_data(counter);
        }
        
        /* Page-specific updates */
        switch (app.current_page) {
        case 0: /* Home page */
            /* Already handled in update_gui_data */
            break;
        case 1: /* Settings page */
            /* Could add settings-specific updates here */
            LOG_DBG("Settings page active");
            break;
        case 2: /* Info page */
            /* Could add info-specific updates here */
            LOG_DBG("Info page active");
            break;
        }

        counter++;
        lv_timer_handler();
        k_msleep(100);
    }

    return 0;
}

int main(void)
{
	const struct device *disp = DEVICE_DT_GET(TFT_NODE);
	if (!device_is_ready(disp)) {
		LOG_ERR("Display not ready");
		return -ENODEV;
	}

	LOG_INF("Display device ready: %s", disp->name);

#if DT_NODE_EXISTS(TFT_NODE) && DT_NODE_EXISTS(DT_ALIAS(backlight))
	if (gpio_is_ready_dt(&backlight)) {
		gpio_pin_configure_dt(&backlight, GPIO_OUTPUT_ACTIVE); /* open backlight */
		LOG_INF("Backlight on pin: %d", backlight.pin);
	}
#endif

#if ENABLE_BASIC_LCD_TEST && !ENABLE_LVGL_TEST
	return test_basic_lcd(disp);

#elif ENABLE_LVGL_TEST && !ENABLE_BASIC_LCD_TEST && CONFIG_LVGL
	return test_modern_gui(disp);

#else
	LOG_ERR("Both tests are disabled! Enable at least one test.");
	return -EINVAL;

#endif
}
