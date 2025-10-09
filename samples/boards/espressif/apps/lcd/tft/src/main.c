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

/* 简单的绘制函数 */

/* 绘制边框 */
static void draw_border(uint16_t *buf, int width, int height, uint16_t color) {
	/* 绘制更厚的上下边框 (2像素厚) */
	for (int x = 0; x < width; x++) {
		buf[x] = color;                           /* 第1行 */
		buf[width + x] = color;                   /* 第2行 */
		buf[(height-2) * width + x] = color;      /* 倒数第2行 */
		buf[(height-1) * width + x] = color;      /* 倒数第1行 */
	}
	/* 绘制更厚的左右边框 (2像素厚) */
	for (int y = 0; y < height; y++) {
		buf[y * width] = color;                   /* 最左列 */
		buf[y * width + 1] = color;               /* 第2列 */
		buf[y * width + (width-2)] = color;      /* 倒数第2列 */
		buf[y * width + (width-1)] = color;      /* 最右列 */
	}
}

/* 简单的8x8像素字体数据 (0-9) */
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

/* 简单字母字体数据 (A-Z的部分字母) */
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

/* 绘制8x8字符 */
static void draw_char_8x8(uint16_t *buf, int width, int height, char c, int x, int y, uint16_t color) {
	const uint8_t *font_data = NULL;
	
	if (c >= '0' && c <= '9') {
		font_data = font_8x8_digits[c - '0'];
	} else if (c >= 'A' && c <= 'Z') {
		font_data = font_8x8_letters[c - 'A'];
	} else if (c >= 'a' && c <= 'z') {
		font_data = font_8x8_letters[c - 'a']; /* 小写按大写处理 */
	} else {
		return; /* 不支持的字符 */
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

/* 绘制数字 */
static void draw_number(uint16_t *buf, int width, int height, int number, int x, int y, uint16_t color) {
	char num_str[16];
	snprintf(num_str, sizeof(num_str), "%d", number);
	
	int char_x = x;
	for (int i = 0; num_str[i] != '\0'; i++) {
		draw_char_8x8(buf, width, height, num_str[i], char_x, y, color);
		char_x += 9; /* 8像素字符宽度 + 1像素间距 */
	}
}

/* 绘制浮点数 */
static void draw_float(uint16_t *buf, int width, int height, float number, int x, int y, uint16_t color) {
	char num_str[16];
	snprintf(num_str, sizeof(num_str), "%.1f", number);
	
	int char_x = x;
	for (int i = 0; num_str[i] != '\0'; i++) {
		if (num_str[i] == '.') {
			/* 绘制点 */
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

/* 绘制文本 */
static void draw_text(uint16_t *buf, int width, int height, const char *text, int x, int y, uint16_t color) {
	int char_x = x;
	for (int i = 0; text[i] != '\0'; i++) {
		if (text[i] == ' ') {
			char_x += 9; /* 空格宽度 */
		} else {
			draw_char_8x8(buf, width, height, text[i], char_x, y, color);
			char_x += 9;
		}
	}
}

/* 测试模式配置宏 
 * 使用方法：
 * 1. 仅测试基础LCD功能（不依赖LVGL）: ENABLE_BASIC_LCD_TEST=1, ENABLE_LVGL_TEST=0
 * 2. 仅测试LVGL功能: ENABLE_BASIC_LCD_TEST=0, ENABLE_LVGL_TEST=1  
 * 3. 先测试LCD再测试LVGL: ENABLE_BASIC_LCD_TEST=1, ENABLE_LVGL_TEST=1
 * 4. 两个都禁用会报错
 */
#define ENABLE_LVGL_TEST    0    /* 1: 启用LVGL测试, 0: 禁用 */
#define ENABLE_BASIC_LCD_TEST 1  /* 1: 启用基础LCD测试, 0: 禁用 */

#define TFT_NODE DT_CHOSEN(zephyr_display)

#if DT_NODE_EXISTS(TFT_NODE)
static const struct gpio_dt_spec backlight = GPIO_DT_SPEC_GET(DT_ALIAS(backlight), gpios);
#endif

/* 基础LCD测试函数 - 显示纯色屏幕 */
static int test_basic_lcd(const struct device *disp)
{
	/* 获取显示器参数 */
	struct display_capabilities caps;
	display_get_capabilities(disp, &caps);
	LOG_INF("Display: %dx%d, pixel format: %d", caps.x_resolution, caps.y_resolution, caps.current_pixel_format);
	
	/* 检查像素格式是否为RGB565 */
	if (caps.current_pixel_format != PIXEL_FORMAT_RGB_565) {
		LOG_ERR("Unsupported pixel format: %d (expected RGB565)", caps.current_pixel_format);
		return -ENOTSUP;
	}
	
	/* 使用静态缓冲区避免动态分配失败 */
	static uint16_t test_buf[240 * 135]; /* RGB565: 2 bytes per pixel, 横屏模式 */
	size_t buf_size = sizeof(test_buf);
	
	LOG_INF("Using static buffer of %zu bytes", buf_size);
	
	/* 设置缓冲区描述符 */
	struct display_buffer_descriptor desc = {
		.buf_size = buf_size,
		.width = caps.x_resolution,
		.height = caps.y_resolution,
		.pitch = caps.x_resolution,
		.frame_incomplete = false,
	};
	
	/* 关闭显示器的blanking模式 */
	display_blanking_off(disp);
	LOG_INF("Display blanking turned off");
	
	/* 定义颜色数组 (RGB565格式) */
	uint16_t colors[] = {0xF800, 0x001F, 0x07E0, 0xFFFF, 0x0000}; /* 红蓝绿白黑 */
	const char *color_names[] = {"Red", "Blue", "Green", "White", "Black"};
	
	/* 逐个测试每种颜色 */
	for (int color_idx = 0; color_idx < 5; color_idx++) {
		/* 填充整个缓冲区为指定颜色 */
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
		
		k_msleep(2000);  /* 等待2秒查看颜色 */
	}
	
	/* 测试横屏模式的图案 */
	LOG_INF("Testing landscape pattern...");
	
	/* 创建一个横向条纹图案来确认方向 */
	for (int y = 0; y < caps.y_resolution; y++) {
		for (int x = 0; x < caps.x_resolution; x++) {
			int idx = y * caps.x_resolution + x;
			
			if (y < caps.y_resolution / 4) {
				test_buf[idx] = 0xF800; /* 红色 */
			} else if (y < caps.y_resolution / 2) {
				test_buf[idx] = 0x07E0; /* 绿色 */
			} else if (y < 3 * caps.y_resolution / 4) {
				test_buf[idx] = 0x001F; /* 蓝色 */
			} else {
				test_buf[idx] = 0xFFFF; /* 白色 */
			}
		}
	}
	
	int ret = display_write(disp, 0, 0, &desc, test_buf);
	LOG_INF("Horizontal stripes pattern displayed: %d", ret);
	k_msleep(3000);
	
	/* 创建一个竖向条纹图案来确认方向 */
	LOG_INF("Testing portrait pattern...");
	for (int y = 0; y < caps.y_resolution; y++) {
		for (int x = 0; x < caps.x_resolution; x++) {
			int idx = y * caps.x_resolution + x;
			
			if (x < caps.x_resolution / 4) {
				test_buf[idx] = 0xF800; /* 红色 */
			} else if (x < caps.x_resolution / 2) {
				test_buf[idx] = 0x07E0; /* 绿色 */
			} else if (x < 3 * caps.x_resolution / 4) {
				test_buf[idx] = 0x001F; /* 蓝色 */
			} else {
				test_buf[idx] = 0xFFFF; /* 白色 */
			}
		}
	}
	
	ret = display_write(disp, 0, 0, &desc, test_buf);
	LOG_INF("Vertical stripes pattern displayed: %d", ret);
	k_msleep(3000);

	/* 开始数字和字符显示测试 */
	LOG_INF("Starting number and text display demo...");
	
	/* 数字和字符显示的主循环 */
	int counter = 0;
	float number = 123.456;
	const char *messages[] = {
		"Hello ESP32-S3!",
		"Display Works!",
		"Numbers & Text",
		"ST7789V TFT OK"
	};
	
	while (1) {
		/* 清空缓冲区为黑色 */
		for (int i = 0; i < caps.x_resolution * caps.y_resolution; i++) {
			test_buf[i] = 0x0000; /* 黑色背景 */
		}
		
		/* 绘制一个简单的边框 */
		draw_border(test_buf, caps.x_resolution, caps.y_resolution, 0xFFFF);
		
		/* 横屏模式下重新布局文本位置 (240x135) */
		/* 显示计数器数字 - 左上角，避开边框 */
		draw_number(test_buf, caps.x_resolution, caps.y_resolution, counter, 10, 20, 0xF800); /* 红色 */
		
		/* 显示浮点数 - 中上部 */
		draw_float(test_buf, caps.x_resolution, caps.y_resolution, number + counter * 0.1f, 10, 40, 0x07E0); /* 绿色 */
		
		/* 显示文本消息 - 中部 */
		int msg_idx = (counter / 10) % 4; /* 每10次循环切换消息 */
		draw_text(test_buf, caps.x_resolution, caps.y_resolution, messages[msg_idx], 10, 70, 0x001F); /* 蓝色 */
		
		/* 显示状态信息 - 底部但在边框内 */
		char status[32];
		snprintf(status, sizeof(status), "Loop: %d", counter);
		draw_text(test_buf, caps.x_resolution, caps.y_resolution, status, 10, 100, 0xFFE0); /* 黄色 */
		
		/* 添加更多横屏布局信息 - 右侧，避开边框 */
		char info[32];
		snprintf(info, sizeof(info), "240x135");
		draw_text(test_buf, caps.x_resolution, caps.y_resolution, info, 150, 20, 0xF81F); /* 紫色 */
		
		/* 写入显示器 */
		ret = display_write(disp, 0, 0, &desc, test_buf);
		if (ret == 0) {
			LOG_INF("Frame %d: Counter=%d, Number=%.1f, Message='%s'", 
					counter, counter, number + counter * 0.1f, messages[msg_idx]);
		} else {
			LOG_ERR("Display write failed: %d", ret);
		}
		
		counter++;
		k_msleep(1000);  /* 每秒更新一次 */
	}
	
	return 0;
}

#if defined(CONFIG_LVGL) && ENABLE_LVGL_TEST
/* LVGL测试函数 - 显示文字和动态计数器 */
static int test_lvgl_widgets(const struct device *disp)
{
	/* 首先确保显示器正确设置 */
	struct display_capabilities caps;
	display_get_capabilities(disp, &caps);
	LOG_INF("LVGL Display: %dx%d, pixel format: %d", caps.x_resolution, caps.y_resolution, caps.current_pixel_format);
	
	/* 关闭显示器的blanking模式 - 这很重要！ */
	display_blanking_off(disp);
	LOG_INF("Display blanking turned off for LVGL");
	
	/* 等待 LVGL 完全准备好 */
	k_msleep(500);
	
	LOG_INF("Creating LVGL test interface...");

	/* 不再需要软件旋转，因为设备树已经配置为横屏 */
	LOG_INF("Display configured for landscape mode in device tree");

	/* 获取屏幕对象 */
	lv_obj_t *scr = lv_screen_active();
	
	/* 设置黑色背景 */
	lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
	
	/* 创建标题 */
	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "ESP32-S3 TFT Demo");
	lv_obj_set_style_text_color(title, lv_palette_main(LV_PALETTE_CYAN), LV_PART_MAIN);
	lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
	
	/* 创建状态标签 */
	lv_obj_t *status_label = lv_label_create(scr);
	lv_label_set_text(status_label, "System Ready");
	lv_obj_set_style_text_color(status_label, lv_palette_main(LV_PALETTE_GREEN), LV_PART_MAIN);
	lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -20);
	
	/* 创建计数器标签 */
	lv_obj_t *counter_label = lv_label_create(scr);
	lv_obj_set_style_text_color(counter_label, lv_palette_main(LV_PALETTE_YELLOW), LV_PART_MAIN);
	lv_obj_align(counter_label, LV_ALIGN_CENTER, 0, 10);
	
	/* 创建数字显示区域 */
	lv_obj_t *number_label = lv_label_create(scr);
	lv_label_set_text(number_label, "Number: 123.456");
	lv_obj_set_style_text_color(number_label, lv_palette_main(LV_PALETTE_ORANGE), LV_PART_MAIN);
	lv_obj_align(number_label, LV_ALIGN_CENTER, 0, 30);
	
	/* 创建字符显示区域 */
	lv_obj_t *text_label = lv_label_create(scr);
	lv_label_set_text(text_label, "Text: Hello World!");
	lv_obj_set_style_text_color(text_label, lv_palette_main(LV_PALETTE_PINK), LV_PART_MAIN);
	lv_obj_align(text_label, LV_ALIGN_CENTER, 0, 50);
	
	/* 创建一个按钮 */
	lv_obj_t *btn = lv_button_create(scr);
	lv_obj_set_size(btn, 100, 40);
	lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -10);
	lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), LV_PART_MAIN);
	
	lv_obj_t *btn_label = lv_label_create(btn);
	lv_label_set_text(btn_label, "Test Button");
	lv_obj_set_style_text_color(btn_label, lv_color_white(), LV_PART_MAIN);
	lv_obj_center(btn_label);
	
	/* 进入主循环 */
	int counter = 0;
	LOG_INF("Starting main loop with improved UI...");

	/* 初始刷新 */
	lv_timer_handler();

	while (1) {
		/* 更新计数器和动态数字 */
		lv_label_set_text_fmt(counter_label, "Count: %d", counter);
		lv_label_set_text_fmt(number_label, "Number: %d.%03d", counter, (counter * 123) % 1000);
		
		/* 每隔一段时间改变字符内容 */
		if (counter % 8 == 0) {
			const char *texts[] = {
				"Text: Hello World!", 
				"Text: ESP32-S3 OK!", 
				"Text: LVGL Works!", 
				"Text: Display Test"
			};
			lv_label_set_text(text_label, texts[counter / 8 % 4]);
		}
		
		/* 每隔一段时间改变状态文本颜色 */
		if (counter % 20 == 0) {
			lv_color_t colors[] = {
				lv_palette_main(LV_PALETTE_GREEN),
				lv_palette_main(LV_PALETTE_ORANGE),
				lv_palette_main(LV_PALETTE_RED),
				lv_palette_main(LV_PALETTE_PURPLE)
			};
			lv_obj_set_style_text_color(status_label, colors[counter / 20 % 4], LV_PART_MAIN);
		}
		
		counter++;
		
		/* 调用 LVGL 定时器处理 - 这是标准做法 */
		lv_timer_handler();
		
		/* 每10次循环记录一次状态 */
		if (counter % 10 == 0) {
			LOG_INF("LVGL Loop %d: UI updated, landscape mode", counter);
		}
		
		k_msleep(500);
	}
	
	return 0;
}
#endif /* defined(CONFIG_LVGL) && ENABLE_LVGL_TEST */

int main(void)
{
	const struct device *disp = DEVICE_DT_GET(TFT_NODE);
	if (!device_is_ready(disp)) {
		LOG_ERR("Display not ready");
		return -ENODEV;
	}
	
	LOG_INF("Display device ready: %s", disp->name);

#if DT_NODE_EXISTS(TFT_NODE) && DT_NODE_EXISTS(DT_ALIAS(backlight))
	/* 开启背光 */
	if (gpio_is_ready_dt(&backlight)) {
		gpio_pin_configure_dt(&backlight, GPIO_OUTPUT_ACTIVE);
		LOG_INF("Backlight on");
	}
#endif

#if ENABLE_BASIC_LCD_TEST && !ENABLE_LVGL_TEST
	/* 仅运行基础LCD测试 */
	LOG_INF("Running basic LCD test only...");
	return test_basic_lcd(disp);
	
#elif ENABLE_LVGL_TEST && !ENABLE_BASIC_LCD_TEST
	/* 仅运行LVGL测试 */
#ifdef CONFIG_LVGL
	LOG_INF("Running LVGL test only...");
	return test_lvgl_widgets(disp);
#else
	LOG_ERR("LVGL test requested but CONFIG_LVGL not enabled!");
	return -ENOTSUP;
#endif

#elif ENABLE_BASIC_LCD_TEST && ENABLE_LVGL_TEST
	/* 先运行基础LCD测试，然后运行LVGL测试 */
	LOG_INF("Running basic LCD test first...");
	
	/* 基础LCD测试 - 只测试3秒然后继续 */
	struct display_capabilities caps;
	display_get_capabilities(disp, &caps);
	LOG_INF("Display: %dx%d, pixel format: %d", caps.x_resolution, caps.y_resolution, caps.current_pixel_format);
	
	/* 关闭显示器的blanking模式 */
	display_blanking_off(disp);
	
	/* 使用静态缓冲区 */
	static uint16_t test_buf[240 * 135];
	size_t buf_size = sizeof(test_buf);
	
	struct display_buffer_descriptor desc = {
		.buf_size = buf_size,
		.width = caps.x_resolution,
		.height = caps.y_resolution,
		.pitch = caps.x_resolution,
		.frame_incomplete = false,
	};
	
	/* 快速颜色测试 */
	uint16_t colors[] = {0xF800, 0x001F, 0x07E0};  /* 红蓝绿 */
	const char *color_names[] = {"Red", "Blue", "Green"};
	
	for (int c = 0; c < 3; c++) {
		for (int i = 0; i < caps.x_resolution * caps.y_resolution; i++) {
			test_buf[i] = colors[c];
		}
		int ret = display_write(disp, 0, 0, &desc, test_buf);
		LOG_INF("Quick %s screen test: %d", color_names[c], ret);
		k_msleep(1000);
	}
	
	/* 切换到LVGL测试 */
#ifdef CONFIG_LVGL
	LOG_INF("Switching to LVGL test...");
	return test_lvgl_widgets(disp);
#else
	LOG_ERR("LVGL test requested but CONFIG_LVGL not enabled!");
	LOG_INF("Continuing with basic LCD test...");
	return test_basic_lcd(disp);
#endif

#else
	/* 两个测试都被禁用 */
	LOG_ERR("Both tests are disabled! Enable at least one test.");
	return -EINVAL;
#endif
}
