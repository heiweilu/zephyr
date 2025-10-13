/*
 * ESP32-S3 TFT Display Simple Test
 * Copyright (c) 2025 Heiweilu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

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
/* ================= LCD initialization End ================= */
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include "lvgl_wrapper.h"

LV_FONT_DECLARE(lv_font_unscii_8)

/* GUI Application Structure - Simplified for console only */
typedef struct {
	lv_obj_t *main_screen;
	lv_obj_t *console_container;  /* Full-screen console area */
	lv_obj_t *console_label;      /* Console text display */
	lv_obj_t *prompt_label;       /* Current command prompt */
} gui_app_t;

static gui_app_t app = {0};

/* Console buffer for storing output */
#define CONSOLE_LINES 8
#define CONSOLE_LINE_LENGTH 80

typedef struct {
	char console_buffer[CONSOLE_LINES][CONSOLE_LINE_LENGTH];
	int console_current_line;
	char current_command[CONSOLE_LINE_LENGTH];
	int command_pos;
} console_state_t;

static console_state_t console_state = {0};

/* Console functions */
static void console_update_display(void)
{
	if (!app.console_label) return;
	
	char display_text[CONSOLE_LINES * CONSOLE_LINE_LENGTH + 200];
	display_text[0] = '\0';
	
	/* Build display text from all lines */
	for (int i = 0; i < console_state.console_current_line && i < CONSOLE_LINES; i++) {
		strcat(display_text, console_state.console_buffer[i]);
		strcat(display_text, "\n");
	}
	
	/* Add current prompt with cursor */
	strcat(display_text, "esp32s3:~$ ");
	strcat(display_text, console_state.current_command);
	strcat(display_text, "_"); /* simple cursor */
	
	lv_label_set_text(app.console_label, display_text);
}

/**
 * Add a line to the console
 * @param text The text to add
 */
static void console_add_line(const char *text)
{
	/* Scroll up if buffer is full */
	if (console_state.console_current_line >= CONSOLE_LINES) {
		for (int i = 0; i < CONSOLE_LINES - 1; i++) {
			strcpy(console_state.console_buffer[i], console_state.console_buffer[i + 1]);
		}
		console_state.console_current_line = CONSOLE_LINES - 1;
	}
	
	/* Add new line */
	strncpy(console_state.console_buffer[console_state.console_current_line], text, CONSOLE_LINE_LENGTH - 1);
	console_state.console_buffer[console_state.console_current_line][CONSOLE_LINE_LENGTH - 1] = '\0';
	console_state.console_current_line++;

	console_update_display();
}

static void console_execute_command(const char *cmd)
{
	/* Add command to console history (but don't display prompt again) */
	
	/* Simple command processing */
	if (strcmp(cmd, "help") == 0) {
		console_add_line("Available commands:");
		console_add_line("  help    - Show this help");
		console_add_line("  clear   - Clear screen");
		console_add_line("  version - Show version");
		console_add_line("  uptime  - System uptime");
		console_add_line("  memory  - Memory info");
		console_add_line("  test    - Run test");
	} else if (strcmp(cmd, "clear") == 0) {
		console_state.console_current_line = 0;
		memset(console_state.console_buffer, 0, sizeof(console_state.console_buffer));
		console_add_line("ESP32-S3 LCD Shell v1.0");
		console_add_line("Type 'help' for commands");
	} else if (strcmp(cmd, "version") == 0) {
		console_add_line("Zephyr OS v4.2.99");
		console_add_line("ESP32-S3 DevKit Console");
		console_add_line("LVGL GUI Framework");
	} else if (strcmp(cmd, "uptime") == 0) {
		uint32_t uptime_ms = k_uptime_get_32();
		uint32_t seconds = uptime_ms / 1000;
		uint32_t minutes = seconds / 60;
		uint32_t hours = minutes / 60;
		char uptime_str[50];
		snprintf(uptime_str, sizeof(uptime_str), "Uptime: %02luh:%02lum:%02lus", 
			 (unsigned long)(hours % 24), 
			 (unsigned long)(minutes % 60), 
			 (unsigned long)(seconds % 60));
		console_add_line(uptime_str);
	} else if (strcmp(cmd, "memory") == 0) {
		console_add_line("Memory Status:");
		console_add_line("  Heap: Available");
		console_add_line("  Stack: Normal");
		console_add_line("  PSRAM: Ready");
	} else if (strcmp(cmd, "test") == 0) {
		console_add_line("System Test Results:");
		console_add_line("  LCD: OK");
		console_add_line("  Touch: Not configured");
		console_add_line("  WiFi: Not initialized");
		console_add_line("  System: Running");
	} else if (strlen(cmd) == 0) {
		/* Empty command, just show new prompt */
	} else {
		char error_msg[CONSOLE_LINE_LENGTH];
		char safe_cmd[CONSOLE_LINE_LENGTH - 32];
		strncpy(safe_cmd, cmd, sizeof(safe_cmd) - 1);
		safe_cmd[sizeof(safe_cmd) - 1] = '\0';
		snprintf(error_msg, sizeof(error_msg), "Command not found: %s", safe_cmd);
		console_add_line(error_msg);
		console_add_line("Type 'help' for available commands");
	}
	
	/* Clear current command */
	console_state.current_command[0] = '\0';
	console_state.command_pos = 0;
	console_update_display();
}

/* Simulate keyboard input */
static void simulate_key_input(char key)
{
	if (key == '\n' || key == '\r') {
		/* Execute command */
		console_execute_command(console_state.current_command);
	} else if (key == '\b' || key == 127) {
		/* Backspace */
		if (console_state.command_pos > 0) {
			console_state.command_pos--;
			console_state.current_command[console_state.command_pos] = '\0';
			console_update_display();
		}
	} else if (key >= 32 && key <= 126) {
		/* Printable character */
		if (console_state.command_pos < CONSOLE_LINE_LENGTH - 1) {
			console_state.current_command[console_state.command_pos++] = key;
			console_state.current_command[console_state.command_pos] = '\0';
			console_update_display();
		}
	}
}

/* ================= Init LVGL ================= */
static void lvgl_init(void)
{
	/* Initialize main screen */
	app.main_screen = lv_screen_active();
	lv_obj_set_style_bg_color(app.main_screen, lv_color_black(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(app.main_screen, LV_OPA_COVER, LV_PART_MAIN);

	/* Create full-screen console area */
	app.console_container = create_container(app.main_screen, 240, 110, 0, 0, 0, 0, 
						lv_color_black(), 2, lv_color_make(0x00, 0x20, 0x00), 1);

	/* Create console text label - terminal style */
	app.console_label = create_label(app.console_container, "", 5, 5, 230,
					lv_color_make(0x00, 0xFF, 0x00), &lv_font_unscii_8, false);
	
	/* Set label to support multiple lines */
	lv_label_set_long_mode(app.console_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(app.console_label, 230);

	LOG_INF("LCD Console initialized");
}
/* ================= Init LVGL End ================= */

int main(void)
{
	lcd_init();

	lvgl_init();

	/* Initialize console */
	console_state.console_current_line = 0;
	console_state.command_pos = 0;
	console_state.current_command[0] = '\0';

	memset(console_state.console_buffer, 0, sizeof(console_state.console_buffer));

	/* Show initial welcome message */
	console_add_line("ESP32-S3 LCD Shell v1.0");
	console_add_line("Type 'help' for commands");
	console_add_line("");

	/* Initial refresh */
	lv_timer_handler();

	uint32_t counter = 0;

	while (1) {
		/* Auto-demo: simulate interactive shell usage every 15 seconds */
		if (counter % 150 == 0) {  /* Every 15 seconds */
			static int demo_step = 0;
			const char *demo_commands[] = {
				"help",
				"version", 
				"uptime",
				"memory",
				"test",
				"clear"
			};
			
			const char *cmd = demo_commands[demo_step % 6];
			
			/* Simulate typing the command character by character */
			for (const char *p = cmd; *p; p++) {
				simulate_key_input(*p);
				k_msleep(150);  /* Slower typing for better visual effect */
				lv_timer_handler();
			}
			
			/* Brief pause before pressing enter */
			k_msleep(500);
			simulate_key_input('\n');
			
			demo_step++;
		}

		/* Handle LVGL tasks */
		lv_timer_handler();
		counter++;
		k_msleep(100);
	}

	return 0;
}