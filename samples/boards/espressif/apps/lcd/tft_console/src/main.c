/*
 * ESP32-S3 TFT Display with Real Interactive Shell
 * Copyright (c) 2025 Heiweilu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/console/console.h>
#include <zephyr/input/input.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/shell/shell_uart.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
// #include <zephyr/usb/usb_host.h>  /* 待启用 */

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* Real interactive shell includes */
// #include "usb_keyboard.h"    /* USB键盘支持待启用 */
#include "lcd_shell_backend.h"

/* ================= LCD initialization Start================= */
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>

#define TFT_NODE DT_CHOSEN(zephyr_display)

#if DT_NODE_EXISTS(TFT_NODE)
static const struct gpio_dt_spec backlight = GPIO_DT_SPEC_GET(DT_ALIAS(backlight), gpios);
#endif

static void lcd_init(void)
{ 
	const struct device *disp = DEVICE_DT_GET(TFT_NODE);
	if (!device_is_ready(disp)) {
		LOG_ERR("Display not ready");
		return;
	}

	LOG_INF("Display device ready: %s", disp->name);

#if DT_NODE_EXISTS(TFT_NODE) && DT_NODE_EXISTS(DT_ALIAS(backlight))
	if (gpio_is_ready_dt(&backlight)) {
		gpio_pin_configure_dt(&backlight, GPIO_OUTPUT_ACTIVE);
		LOG_INF("Backlight on pin: %d", backlight.pin);
	}
#endif

	/* Get display capabilities and log them */
	struct display_capabilities caps;
	display_get_capabilities(disp, &caps);
	LOG_INF("GUI Display: %dx%d, pixel format: %d", caps.x_resolution, caps.y_resolution,
		caps.current_pixel_format);

	/* Turn off display blanking */
	display_blanking_off(disp);
	k_msleep(500);
}

/* ================= LVGL & GUI Setup ================= */
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_unscii_8)

/* LVGL helper functions */
static lv_obj_t *create_container(lv_obj_t *parent, int width, int height, int x, int y, 
                                 int pad_all, int border_width, lv_color_t bg_color, 
                                 int radius, lv_color_t border_color, int bg_opa)
{
	lv_obj_t *container = lv_obj_create(parent);
	lv_obj_set_size(container, width, height);
	lv_obj_set_pos(container, x, y);
	lv_obj_set_style_pad_all(container, pad_all, 0);
	lv_obj_set_style_border_width(container, border_width, 0);
	lv_obj_set_style_bg_color(container, bg_color, 0);
	lv_obj_set_style_radius(container, radius, 0);
	lv_obj_set_style_border_color(container, border_color, 0);
	lv_obj_set_style_bg_opa(container, bg_opa ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	return container;
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, int x, int y, int width,
                              lv_color_t color, const lv_font_t *font, bool center_align)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, text);
	lv_obj_set_pos(label, x, y);
	lv_obj_set_width(label, width);
	lv_obj_set_style_text_color(label, color, 0);
	lv_obj_set_style_text_font(label, font, 0);
	if (center_align) {
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	}
	return label;
}

/* GUI Application Structure */
typedef struct {
	lv_obj_t *main_screen;
	lv_obj_t *console_container;
	lv_obj_t *status_bar;
	lv_obj_t *status_label;
	lv_obj_t *console_label;
} gui_app_t;

static gui_app_t app = {0};

/* Shell display buffer */
#define SHELL_DISPLAY_BUFFER_SIZE 2048
static char shell_display_buffer[SHELL_DISPLAY_BUFFER_SIZE] = {0};
static size_t shell_display_len = 0;

/* Display update tracking */
static bool display_update_needed = false;

/* Shell output callback - called when shell produces output */
static void shell_output_callback(const char *data, size_t len)
{
	if (!data || len == 0) {
		return;
	}
	
	/* Debug: Log that callback was called (commented to avoid recursion) */
	// LOG_INF("LCD callback: received %zu bytes", len);
	
	/* Simple buffer update (single threaded for now) */
	for (size_t i = 0; i < len && shell_display_len < sizeof(shell_display_buffer) - 1; i++) {
		if (data[i] == '\n') {
			shell_display_buffer[shell_display_len++] = '\n';
		} else if (data[i] >= 32 || data[i] == '\t') {
			shell_display_buffer[shell_display_len++] = data[i];
		}
	}
	shell_display_buffer[shell_display_len] = '\0';
	
	/* Keep only last ~1500 chars to prevent overflow */
	if (shell_display_len > 1500) {
		size_t keep_len = 1000;
		memmove(shell_display_buffer, 
		        shell_display_buffer + (shell_display_len - keep_len), 
		        keep_len);
		shell_display_len = keep_len;
		shell_display_buffer[shell_display_len] = '\0';
	}
	
	/* Mark that display needs update */
	display_update_needed = true;
	// LOG_INF("LCD update flag set, buffer len: %zu", shell_display_len);
}

/* USB HID Keyboard support - 待实现 */
static bool keyboard_connected = false;

/* USB键盘输入处理 */
static void usb_keyboard_callback(uint8_t *keys, uint8_t modifier, uint8_t key_count)
{
	if (!keys || key_count == 0) return;
	
	/* 处理按键输入 */
	for (int i = 0; i < key_count && i < 6; i++) {
		uint8_t key = keys[i];
		if (key == 0) continue;
		
		/* 简单的键码转换 */
		char ch = 0;
		if (key >= 4 && key <= 29) {
			/* A-Z keys */
			ch = 'a' + (key - 4);
			if (modifier & 0x22) { /* Shift keys */
				ch = ch - 'a' + 'A';
			}
		} else if (key >= 30 && key <= 38) {
			/* 1-9 keys */
			ch = '1' + (key - 30);
		} else if (key == 39) {
			/* 0 key */
			ch = '0';
		} else if (key == 40) {
			/* Enter key */
			ch = '\r';
		} else if (key == 44) {
			/* Space key */
			ch = ' ';
		}
		
		/* 发送到shell */
		if (ch != 0) {
			/* 发送字符到默认shell输入 */
			const struct shell *default_shell = shell_backend_uart_get_ptr();
			if (default_shell) {
				/* TODO: 实现向shell发送输入字符 */
				LOG_INF("USB Keyboard input: '%c'", ch);
			}
		}
	}
}

/* Demo command execution for testing */
static void demo_shell_commands(void)
{
	/* Wait a bit for system to stabilize */
	k_msleep(3000);
	
	/* Show initial message */
	shell_output_callback("=== ESP32-S3 Interactive Shell Demo ===\n", 41);
	shell_output_callback("System ready for commands!\n", 27);
	shell_output_callback("Try: help, sysinfo, lcd_test, demo\n", 36);
	shell_output_callback("Type commands in serial terminal to see them here!\n", 51);
	shell_output_callback("esp32s3:~$ ", 11);
}

/* Shell output capture mechanism */
static bool shell_capture_active = false;

/* Transport write function interception */
static int (*original_uart_write)(const struct shell_transport *transport, 
                                  const void *data, size_t length, size_t *cnt) = NULL;

/* Intercepted UART transport write function */
static int intercepted_uart_write(const struct shell_transport *transport, 
                                 const void *data, size_t length, size_t *cnt)
{
	/* Forward to LCD if capture is active */
	if (shell_capture_active && data && length > 0) {
		const char *str = (const char *)data;
		
		/* Filter out LOG messages to avoid infinite recursion */
		if (length < 20 || strncmp(str, "[00:", 4) != 0) {
			/* This is not a LOG message, forward to LCD */
			shell_output_callback(str, length);
		}
	}
	
	/* Call original write function */
	if (original_uart_write) {
		return original_uart_write(transport, data, length, cnt);
	} else {
		*cnt = length;
		return 0;
	}
}

/* 已移除：不需要的UART设备级拦截器 */

/* 已移除：不需要的Console hook拦截器 */

/* 已移除：不需要的stdio函数拦截器 */

/* 已移除：不需要的puts、fputs拦截器 */

/* 已移除：不需要的fwrite、fprintf、printk hook拦截器 */

static void lvgl_init(void)
{
	/* Initialize main screen */
	app.main_screen = lv_screen_active();
	lv_obj_set_style_bg_color(app.main_screen, lv_color_black(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(app.main_screen, LV_OPA_COVER, LV_PART_MAIN);

	/* Create status bar */
	app.status_bar = create_container(app.main_screen, 240, 20, 0, 0, 0, 1, 
					 lv_color_make(0x20, 0x20, 0x20), 2, 
					 lv_color_make(0x40, 0x40, 0x40), 1);
	
	app.status_label = create_label(app.status_bar, "ESP32-S3 Interactive Shell", 2, 2, 230,
				       lv_color_make(0x00, 0xFF, 0x00), &lv_font_unscii_8, false);

	/* Create console container */
	app.console_container = create_container(app.main_screen, 240, 115, 0, 20, 0, 1, 
						lv_color_black(), 2, lv_color_make(0x00, 0x40, 0x00), 1);

	/* Create console label for shell output */
	app.console_label = create_label(app.console_container, "", 3, 3, 225,
	                                lv_color_make(0x00, 0xFF, 0x00), &lv_font_unscii_8, false);

	/* Configure console label for scrollable multi-line text */
	lv_label_set_long_mode(app.console_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(app.console_label, 225);
	lv_obj_set_height(app.console_label, 105);
	lv_obj_set_style_text_align(app.console_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_set_scrollbar_mode(app.console_label, LV_SCROLLBAR_MODE_AUTO);

	/* Initial welcome message */
	strcpy(shell_display_buffer, "ESP32-S3 Interactive Shell\nUSB Keyboard Ready!\nType commands with USB keyboard...\nesp32s3:~$ ");
	shell_display_len = strlen(shell_display_buffer);
	lv_label_set_text(app.console_label, shell_display_buffer);
	
	LOG_INF("LVGL GUI initialized");
}

/* ========== Custom Shell Commands ========== */
static int cmd_lcd_test(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	
	/* Print to both serial AND LCD */
	shell_print(sh, "=== LCD Console Test ===");
	shell_output_callback("=== LCD Console Test ===\n", 26);
	
	shell_print(sh, "Display: 240x135 RGB565");
	shell_output_callback("Display: 240x135 RGB565\n", 24);
	
	shell_print(sh, "LVGL: v9.x Active");
	shell_output_callback("LVGL: v9.x Active\n", 18);
	
	shell_print(sh, "Real Shell: Working");
	shell_output_callback("Real Shell: Working\n", 20);
	
	shell_print(sh, "USB Keyboard: Not yet ready");
	shell_output_callback("USB Keyboard: Not ready\n", 24);
	
	shell_print(sh, "Status: OK - LCD Mirror Active!");
	shell_output_callback("Status: OK - LCD Mirror!\n", 25);
	
	shell_print(sh, "========================");
	shell_output_callback("========================\n", 25);
	
	return 0;
}

static int cmd_system_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	
	/* Print to both serial AND LCD */
	shell_print(sh, "ESP32-S3 System Information:");
	shell_output_callback("=== ESP32-S3 System Info ===\n", 30);
	
	shell_print(sh, "  CPU: ESP32-S3 @ 240MHz");
	shell_output_callback("CPU: ESP32-S3 @ 240MHz\n", 24);
	
	shell_print(sh, "  PSRAM: 8MB");
	shell_output_callback("PSRAM: 8MB\n", 11);
	
	shell_print(sh, "  Flash: 16MB");
	shell_output_callback("Flash: 16MB\n", 12);
	
	shell_print(sh, "  Zephyr: v4.2.99");
	shell_output_callback("Zephyr: v4.2.99\n", 16);
	
	shell_print(sh, "  LVGL: v9.x");
	shell_output_callback("LVGL: v9.x\n", 11);
	
	/* Uptime */
	uint32_t uptime_ms = k_uptime_get_32();
	uint32_t seconds = uptime_ms / 1000;
	uint32_t minutes = seconds / 60;
	uint32_t hours = minutes / 60;
	
	char uptime_buf[64];
	snprintf(uptime_buf, sizeof(uptime_buf), "Uptime: %02lu:%02lu:%02lu\n", 
	         (unsigned long)(hours % 24), 
	         (unsigned long)(minutes % 60), 
	         (unsigned long)(seconds % 60));
	
	shell_print(sh, "  Uptime: %02lu:%02lu:%02lu", 
	           (unsigned long)(hours % 24), 
	           (unsigned long)(minutes % 60), 
	           (unsigned long)(seconds % 60));
	shell_output_callback(uptime_buf, strlen(uptime_buf));
	
	return 0;
}

static int cmd_lcd_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	
	/* Clear display buffer */
	shell_display_len = 0;
	strcpy(shell_display_buffer, "esp32s3:~$ ");
	shell_display_len = strlen(shell_display_buffer);
	
	if (app.console_label) {
		lv_label_set_text(app.console_label, shell_display_buffer);
		lv_obj_invalidate(app.console_label);
	}
	
	shell_print(sh, "Console cleared");
	
	return 0;
}

static int cmd_demo(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	
	shell_print(sh, "Running demo sequence...");
	shell_output_callback("Running demo sequence...\n", 25);
	
	for (int i = 1; i <= 5; i++) {
		char step_buf[32];
		snprintf(step_buf, sizeof(step_buf), "Demo step %d/5\n", i);
		
		shell_print(sh, "Demo step %d/5", i);
		shell_output_callback(step_buf, strlen(step_buf));
		k_msleep(500);
	}
	
	shell_print(sh, "Demo completed!");
	shell_output_callback("Demo completed!\n", 16);
	
	return 0;
}

static int cmd_lcd_show(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	
	if (argc < 2) {
		shell_output_callback("Usage: lcd_show <message>\n", 26);
		return -1;
	}
	
	/* Show message on LCD */
	shell_output_callback(argv[1], strlen(argv[1]));
	shell_output_callback("\n", 1);
	
	return 0;
}

/* Register shell commands */
SHELL_CMD_REGISTER(lcd_test, NULL, "Run LCD test", cmd_lcd_test);
SHELL_CMD_REGISTER(sysinfo, NULL, "Show system information", cmd_system_info);
SHELL_CMD_REGISTER(clear, NULL, "Clear console", cmd_lcd_clear);
SHELL_CMD_REGISTER(demo, NULL, "Run demo sequence", cmd_demo);
SHELL_CMD_REGISTER(lcd_show, NULL, "Show message on LCD", cmd_lcd_show);

int main(void)
{
	LOG_INF("Starting ESP32-S3 Interactive Shell Console...");
	
	/* Initialize hardware */
	lcd_init();
	lvgl_init();
	
	/* USB 键盘支持 - 暂时保留框架代码 */
	LOG_INF("USB keyboard support will be added in future versions");
	keyboard_connected = false;
	
	/* Initialize shell backend */
	int ret = lcd_shell_backend_init();
	if (ret != 0) {
		LOG_ERR("Failed to initialize shell backend: %d", ret);
		return ret;
	}
	
	/* Set shell output callback */
	lcd_shell_set_output_callback(shell_output_callback);
	
	/* 已移除：不需要的printk和console hook */
	
	/* Hook into default UART shell transport */
	const struct shell *default_shell = shell_backend_uart_get_ptr();
	if (default_shell && default_shell->iface && default_shell->iface->api) {
		LOG_INF("Found UART shell, installing transport interceptor");
		
		/* Replace transport write function with our interceptor */
		struct shell_transport_api *api = (struct shell_transport_api *)default_shell->iface->api;
		original_uart_write = api->write;
		api->write = intercepted_uart_write;
		
		LOG_INF("UART shell transport write function intercepted");
	} else {
		LOG_WRN("Could not hook into UART shell transport");
	}
	
	/* Activate shell output capture */
	shell_capture_active = true;
	LOG_INF("Shell output capture activated");
	
	/* Enable LCD shell */
	const struct shell *shell = lcd_shell_get_instance();
	if (shell) {
		LOG_INF("LCD shell backend enabled");
		
		/* Shell is automatically enabled through backend */
		
		/* Send initial prompt to LCD */
		shell_output_callback("Interactive Shell Ready!\n", 26);
		shell_output_callback("Serial commands will appear here!\n", 34);
		shell_output_callback("esp32s3:~$ ", 11);
	}
	
	/* USB keyboard support - to be implemented */
	LOG_INF("USB keyboard support: Not yet implemented");
	
	LOG_INF("ESP32-S3 Interactive Shell Console Ready!");
	LOG_INF("LCD will show shell output. Use serial terminal for commands.");
	
	/* Test LCD output immediately */
	shell_output_callback("=== ESP32-S3 LCD Console Ready ===\n", 36);
	shell_output_callback("Serial shell commands will appear here!\n", 40);
	shell_output_callback("Available commands:\n", 20);
	shell_output_callback("help, sysinfo, lcd_test, demo, clear\n", 37);
	shell_output_callback("Type in serial terminal now...\n", 31);
	
	/* Start demo commands in background */
	demo_shell_commands();

	uint32_t counter = 0;
	
	while (1) {
		/* Handle LVGL tasks */
		lv_timer_handler();
		
		/* High priority: Check for display update immediately */
		if (display_update_needed && app.console_label) {
			lv_label_set_text(app.console_label, shell_display_buffer);
			lv_obj_scroll_to_y(app.console_label, LV_COORD_MAX, LV_ANIM_OFF);
			display_update_needed = false;
			
			/* Debug: Log that LCD was updated */
			LOG_DBG("LCD display updated with %zu chars", shell_display_len);
		}
		
		/* Update status bar every 5 seconds */
		if (counter % 200 == 0) {
			uint32_t uptime_ms = k_uptime_get_32();
			uint32_t seconds = uptime_ms / 1000;
			uint32_t minutes = seconds / 60;
			uint32_t hours = minutes / 60;
			
			char status_text[64];
			snprintf(status_text, sizeof(status_text), 
			        "Shell Active | Up: %02lu:%02lu:%02lu", 
			        (unsigned long)(hours % 24), 
			        (unsigned long)(minutes % 60), 
			        (unsigned long)(seconds % 60));
			
			if (app.status_label) {
				lv_label_set_text(app.status_label, status_text);
			}
		}

		counter++;
		/* Reduced sleep time for faster LCD updates */
		k_msleep(25);
	}

	return 0;
}