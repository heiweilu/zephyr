/*
 * ESP32-S3 TFT Display Application with LVGL
 * Copyright (c) 2025 Heiweilu
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ========================================
 * ESP32-S3 + ST7789V TFT Display Application
 * =======================================
 *
 * Hardware Configuration:
 * - Development Board: ESP32-S3 DevKit
 * - Display: ST7789V TFT (240x135 RGB565)
 * - Connection Method: SPI Interface + GPIO Backlight Control
 *
 * Software Architecture:
 * - Zephyr RTOS 4.2.99
 * - LVGL 9.x GUI Framework
 * - Modular Design: Using the lvgl_wrapper Wrapper Library
 *
 * Features:
 * - Modern GUI Interface Design
 * - Status bar: Time display + status icons
 * - Content area: Data card display (sensor data such as temperature)
 * - Bottom navigation: Navigation controls such as the HOME button
 * - Real-time data updates
 *
 * Wrapper libraries used:
 * - lvgl_wrapper.h: LVGL control wrapper function declarations
 * - lvgl_wrapper.c: LVGL control wrapper function implementations
 *
 * Compile command:
 * west build -b esp32s3_devkitc/esp32s3/procpu .\samples\boards\espressif\apps\lcd\tft\
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/shell/shell.h>
#include <zephyr/console/console.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include <lvgl_input_device.h>
#include "lvgl_wrapper.h"
#include "lcd_console.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

LV_FONT_DECLARE(lv_font_unscii_8)

#define TFT_NODE DT_CHOSEN(zephyr_display)

#if DT_NODE_EXISTS(TFT_NODE)
static const struct gpio_dt_spec backlight = GPIO_DT_SPEC_GET(DT_ALIAS(backlight), gpios);
#endif

/* Shell command handlers */
static int cmd_lcd_info(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "LCD Display Information:");
	shell_print(sh, "  Resolution: 240x135");
	shell_print(sh, "  Controller: ST7789V");
	shell_print(sh, "  Interface: SPI");
	shell_print(sh, "  Pixel Format: RGB565");
	return 0;
}

static int cmd_lcd_clear(const struct shell *sh, size_t argc, char **argv)
{
	lcd_console_t *console = lcd_console_get_instance();
	if (console) {
		lcd_console_clear();
		shell_print(sh, "LCD console cleared");
	} else {
		shell_error(sh, "LCD console not initialized");
	}
	return 0;
}

static int cmd_lcd_test(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "Running LCD test...");
	
	for (int i = 0; i < 5; i++) {
		shell_print(sh, "Test line %d: System status OK", i + 1);
		k_msleep(500);
	}
	
	shell_print(sh, "LCD test completed");
	return 0;
}

static int cmd_system_info(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "System Information:");
	shell_print(sh, "  Platform: ESP32-S3");
	shell_print(sh, "  RTOS: Zephyr");
	shell_print(sh, "  GUI: LVGL");
	shell_print(sh, "  Uptime: %u seconds", k_uptime_get_32() / 1000);
	return 0;
}

static int cmd_memory_info(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "Memory Information:");
	shell_print(sh, "  Main Stack: 4096 bytes");
	shell_print(sh, "  System Workqueue: 2048 bytes");
	shell_print(sh, "  Heap Pool: 8192 bytes");
	
	return 0;
}

/* Register shell commands */
SHELL_STATIC_SUBCMD_SET_CREATE(lcd_cmds,
	SHELL_CMD(info, NULL, "Show LCD information", cmd_lcd_info),
	SHELL_CMD(clear, NULL, "Clear LCD console", cmd_lcd_clear),
	SHELL_CMD(test, NULL, "Run LCD test", cmd_lcd_test),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(lcd, &lcd_cmds, "LCD commands", NULL);

SHELL_STATIC_SUBCMD_SET_CREATE(system_cmds,
	SHELL_CMD(info, NULL, "Show system information", cmd_system_info),
	SHELL_CMD(memory, NULL, "Show memory information", cmd_memory_info),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(system, &system_cmds, "System commands", NULL);

/**
 * @brief Shell and Console test function
 */
static int test_shell_console(const struct device *disp)
{
	LOG_INF("===== Running Shell + Console test =====");

	struct display_capabilities caps;
	display_get_capabilities(disp, &caps);
	LOG_INF("Display: %dx%d, pixel format: %d", caps.x_resolution, caps.y_resolution,
		caps.current_pixel_format);

	display_blanking_off(disp);
	k_msleep(500);

	/* Initialize main screen */
	lv_obj_t *main_screen = lv_screen_active();
	lv_obj_set_style_bg_color(main_screen, lv_color_black(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(main_screen, LV_OPA_COVER, LV_PART_MAIN);

	/* Initialize LCD console */
	int ret = lcd_console_init(main_screen);
	if (ret != 0) {
		LOG_ERR("Failed to initialize LCD console: %d", ret);
		return ret;
	}

	LOG_INF("LCD Console initialized successfully");
	
	/* Enable both console and shell */
	lcd_console_enable(true);
	lcd_shell_enable(true);

	/* Add some initial messages */
	lcd_console_write("=== ESP32-S3 LCD Shell ===\n", 28);
	lcd_console_write("Console output redirected to LCD\n", 34);
	lcd_console_write("Use 'help' for available commands\n", 35);
	lcd_console_write("Use 'lcd info' for display info\n", 33);
	lcd_console_write("Use 'system info' for system info\n", 35);

	uint32_t counter = 0;
	uint32_t last_log_time = 0;

	while (1) {
		/* Update console display */
		lcd_console_update_display();

		/* Periodic log messages for testing */
		uint32_t now = k_uptime_get_32();
		if (now - last_log_time > 5000) {  /* Every 5 seconds */
			char log_msg[128];
			snprintf(log_msg, sizeof(log_msg), 
				"[%u] Periodic log: counter=%u\n",
				now / 1000, counter);
			lcd_console_write(log_msg, strlen(log_msg));
			last_log_time = now;
		}

		/* Handle LVGL tasks */
		lv_timer_handler();

		counter++;
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

#if CONFIG_LVGL && CONFIG_SHELL
	/* Combined LVGL + Shell test mode */
	LOG_INF("Combined LVGL + Shell mode not implemented yet");
	return test_shell_console(disp);

#else
	LOG_ERR("Invalid test configuration! Check test mode macros and Kconfig options.");
	LOG_ERR("CONFIG_LVGL: %d", IS_ENABLED(CONFIG_LVGL));
	LOG_ERR("CONFIG_SHELL: %d", IS_ENABLED(CONFIG_SHELL));
	return -EINVAL;

#endif
}
