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

#ifdef CONFIG_LVGL
#include <lvgl.h>
#endif

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

/* LVGL: Enhanced display with standard APIs for text and dynamic counter */
static int test_lvgl_widgets(const struct device *disp)
{
	LOG_INF("===== Running LVGL test with standard APIs =====");

	struct display_capabilities caps;
	display_get_capabilities(disp, &caps);
	LOG_INF("LVGL Display: %dx%d, pixel format: %d", caps.x_resolution, caps.y_resolution,
		caps.current_pixel_format);

	/* 验证显示分辨率 */
	if (caps.x_resolution != 240 || caps.y_resolution != 135) {
		LOG_WRN("Unexpected resolution: %dx%d, expected 240x135", 
			caps.x_resolution, caps.y_resolution);
	}

	/* Turn off monitor blanking mode */
	display_blanking_off(disp);

	/* Wait for LVGL to be ready */
	k_msleep(500);

	/* Get the active screen object */
	lv_obj_t *scr = lv_screen_active();

	/* Set black background */
	lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

	/* Create a white border container on black background */
	lv_obj_t *border_container = lv_obj_create(scr);
	lv_obj_set_size(border_container, caps.x_resolution - 4, caps.y_resolution - 4);
	lv_obj_align(border_container, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_bg_color(border_container, lv_color_black(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(border_container, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_border_color(border_container, lv_color_white(), LV_PART_MAIN);
	lv_obj_set_style_border_width(border_container, 3, LV_PART_MAIN);
	lv_obj_set_style_border_opa(border_container, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_radius(border_container, 0, LV_PART_MAIN);
	lv_obj_set_style_pad_all(border_container, 8, LV_PART_MAIN);

	/* Create title (bright cyan text) */
	lv_obj_t *title = lv_label_create(border_container);
	lv_label_set_text(title, "ESP32-S3 LVGL");
	lv_obj_set_style_text_color(title, lv_color_make(0x00, 0xFF, 0xFF), LV_PART_MAIN); /* Cyan */
	lv_obj_set_style_text_font(title, &lv_font_unscii_8, LV_PART_MAIN);
	lv_obj_set_style_text_opa(title, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_shadow_width(title, 0, LV_PART_MAIN);
	lv_obj_set_style_outline_width(title, 0, LV_PART_MAIN);
	lv_obj_set_style_text_decor(title, LV_TEXT_DECOR_NONE, LV_PART_MAIN);
	lv_obj_set_pos(title, 5, 5);

	/* Create counter label (bright green text) */
	lv_obj_t *counter_label = lv_label_create(border_container);
	lv_obj_set_style_text_color(counter_label, lv_color_make(0x00, 0xFF, 0x00), LV_PART_MAIN); /* Green */
	lv_obj_set_style_text_font(counter_label, &lv_font_unscii_8, LV_PART_MAIN);
	lv_obj_set_style_text_opa(counter_label, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_shadow_width(counter_label, 0, LV_PART_MAIN);
	lv_obj_set_style_outline_width(counter_label, 0, LV_PART_MAIN);
	lv_obj_set_style_text_decor(counter_label, LV_TEXT_DECOR_NONE, LV_PART_MAIN);
	lv_obj_set_pos(counter_label, 5, 25);

	/* Create number display (bright yellow text) */
	lv_obj_t *number_label = lv_label_create(border_container);
	lv_obj_set_style_text_color(number_label, lv_color_make(0xFF, 0xFF, 0x00), LV_PART_MAIN); /* Yellow */
	lv_obj_set_style_text_font(number_label, &lv_font_unscii_8, LV_PART_MAIN);
	lv_obj_set_style_text_opa(number_label, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_shadow_width(number_label, 0, LV_PART_MAIN);
	lv_obj_set_style_outline_width(number_label, 0, LV_PART_MAIN);
	lv_obj_set_style_text_decor(number_label, LV_TEXT_DECOR_NONE, LV_PART_MAIN);
	lv_obj_set_pos(number_label, 5, 45);

	/* Create floating point display (bright magenta text) */
	lv_obj_t *float_label = lv_label_create(border_container);
	lv_obj_set_style_text_color(float_label, lv_color_make(0xFF, 0x00, 0xFF), LV_PART_MAIN); /* Magenta */
	lv_obj_set_style_text_font(float_label, &lv_font_unscii_8, LV_PART_MAIN);
	lv_obj_set_style_text_opa(float_label, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_shadow_width(float_label, 0, LV_PART_MAIN);
	lv_obj_set_style_outline_width(float_label, 0, LV_PART_MAIN);
	lv_obj_set_style_text_decor(float_label, LV_TEXT_DECOR_NONE, LV_PART_MAIN);
	lv_obj_set_pos(float_label, 5, 65);

	/* Create time display (bright red text) */
	lv_obj_t *time_label = lv_label_create(border_container);
	lv_obj_set_style_text_color(time_label, lv_color_make(0xFF, 0x00, 0x00), LV_PART_MAIN); /* Red */
	lv_obj_set_style_text_font(time_label, &lv_font_unscii_8, LV_PART_MAIN);
	lv_obj_set_style_text_opa(time_label, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_shadow_width(time_label, 0, LV_PART_MAIN);
	lv_obj_set_style_outline_width(time_label, 0, LV_PART_MAIN);
	lv_obj_set_style_text_decor(time_label, LV_TEXT_DECOR_NONE, LV_PART_MAIN);
	lv_obj_set_pos(time_label, 120, 25);

	/* Create FPS display (bright blue text) */
	lv_obj_t *fps_label = lv_label_create(border_container);
	lv_obj_set_style_text_color(fps_label, lv_color_make(0x00, 0x80, 0xFF), LV_PART_MAIN); /* Light Blue */
	lv_obj_set_style_text_font(fps_label, &lv_font_unscii_8, LV_PART_MAIN);
	lv_obj_set_style_text_opa(fps_label, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_shadow_width(fps_label, 0, LV_PART_MAIN);
	lv_obj_set_style_outline_width(fps_label, 0, LV_PART_MAIN);
	lv_obj_set_style_text_decor(fps_label, LV_TEXT_DECOR_NONE, LV_PART_MAIN);
	lv_obj_set_pos(fps_label, 120, 45);

	/* Create text display (bright orange text) */
	lv_obj_t *text_label = lv_label_create(border_container);
	lv_obj_set_style_text_color(text_label, lv_color_make(0xFF, 0x80, 0x00), LV_PART_MAIN); /* Orange */
	lv_obj_set_style_text_font(text_label, &lv_font_unscii_8, LV_PART_MAIN);
	lv_obj_set_style_text_opa(text_label, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_shadow_width(text_label, 0, LV_PART_MAIN);
	lv_obj_set_style_outline_width(text_label, 0, LV_PART_MAIN);
	lv_obj_set_style_text_decor(text_label, LV_TEXT_DECOR_NONE, LV_PART_MAIN);
	lv_obj_set_pos(text_label, 120, 65);

	/* Create status label (bright lime green text) */
	lv_obj_t *status_label = lv_label_create(border_container);
	lv_obj_set_style_text_color(status_label, lv_color_make(0x80, 0xFF, 0x00), LV_PART_MAIN); /* Lime Green */
	lv_obj_set_style_text_font(status_label, &lv_font_unscii_8, LV_PART_MAIN);
	lv_obj_set_style_text_opa(status_label, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_shadow_width(status_label, 0, LV_PART_MAIN);
	lv_obj_set_style_outline_width(status_label, 0, LV_PART_MAIN);
	lv_obj_set_style_text_decor(status_label, LV_TEXT_DECOR_NONE, LV_PART_MAIN);
	lv_obj_set_pos(status_label, 5, 85);
	lv_label_set_text(status_label, "STAT");

	/* Create button with colorful design */
	lv_obj_t *btn = lv_btn_create(border_container);
	lv_obj_set_size(btn, 60, 20);
	lv_obj_set_pos(btn, 160, 55);
	lv_obj_set_style_bg_color(btn, lv_color_make(0x40, 0x00, 0x80), LV_PART_MAIN); /* Dark Purple */
	lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
	lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN);
	lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);

	lv_obj_t *btn_label = lv_label_create(btn);
	lv_label_set_text(btn_label, "Demo");
	lv_obj_set_style_text_color(btn_label, lv_color_make(0xFF, 0xFF, 0x80), LV_PART_MAIN); /* Light Yellow */
	lv_obj_set_style_text_font(btn_label, &lv_font_unscii_8, LV_PART_MAIN);
	lv_obj_set_style_text_opa(btn_label, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_shadow_width(btn_label, 0, LV_PART_MAIN);
	lv_obj_set_style_outline_width(btn_label, 0, LV_PART_MAIN);
	lv_obj_set_style_text_decor(btn_label, LV_TEXT_DECOR_NONE, LV_PART_MAIN);
	lv_obj_center(btn_label);
	lv_obj_center(btn_label);

	int counter = 0;
	float base_number = 123.456f;
	uint32_t start_time = k_uptime_get_32();
	uint32_t frame_count = 0;

	/* Initial refresh */
	lv_timer_handler();

	while (1) {
		uint32_t current_time = k_uptime_get_32();
		
		/* Update counter display */
		lv_label_set_text_fmt(counter_label, "%d", counter);

		/* Update integer number display */
		lv_label_set_text_fmt(number_label, "%d", counter * 7 + 100);

		/* Update floating point display */
		lv_label_set_text_fmt(float_label, "%.1f", 
				      (double)(base_number + (counter * 0.1f)));

		/* Update time display */
		lv_label_set_text_fmt(time_label, "%d.%ds", 
				      (int)(current_time / 1000), 
				      (int)((current_time % 1000) / 100));

		/* Calculate and display FPS */
		frame_count++;
		if (current_time - start_time > 0) {
			uint32_t fps = (frame_count * 1000) / (current_time - start_time);
			lv_label_set_text_fmt(fps_label, "%d", (int)fps);
		}

		/* Rotate simple text */
		if (counter % 8 == 0) {
			const char *texts[] = {
				"API", 
				"OK", 
				"NUM",
				"RUN"
			};
			lv_label_set_text(text_label, texts[counter / 8 % 4]);
		}

		/* Change status */
		if (counter % 20 == 0) {
			const char *statuses[] = {
				"OK", 
				"GO", 
				"UP", 
				"ON"
			};
			lv_label_set_text(status_label, statuses[(counter / 20) % 4]);
		}

		/* Update button text */
		if (counter % 30 == 0) {
			lv_label_set_text_fmt(btn_label, "%d", counter / 30);
		}

		counter++;

		/* Handle LVGL tasks */
		lv_timer_handler();

		/* Adjust sleep time for smoother animation */
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
	return test_lvgl_widgets(disp);

#else
	LOG_ERR("Both tests are disabled! Enable at least one test.");
	return -EINVAL;

#endif
}
