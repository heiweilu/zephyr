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
#include "lvgl_wrapper.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* Real interactive shell includes */
#include "lcd_shell_backend.h"

/* ================= LCD initialization Start================= */
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>

#define TFT_NODE DT_CHOSEN(zephyr_display)

#if DT_NODE_EXISTS(TFT_NODE)
static const struct gpio_dt_spec backlight = GPIO_DT_SPEC_GET(DT_ALIAS(backlight), gpios);
#endif

static int lcd_init(void)
{ 
	const struct device *disp = DEVICE_DT_GET(TFT_NODE);
	if (!device_is_ready(disp)) {
		LOG_ERR("Display not ready");
		return -ENODEV;
	}

	LOG_INF("Display device ready: %s", disp->name);

#if DT_NODE_EXISTS(TFT_NODE) && DT_NODE_EXISTS(DT_ALIAS(backlight))
	if (gpio_is_ready_dt(&backlight)) {
		gpio_pin_configure_dt(&backlight, GPIO_OUTPUT_ACTIVE);
	}
#endif

	/* Get display capabilities and log them */
	struct display_capabilities caps;
	display_get_capabilities(disp, &caps);

	/* Turn off display blanking */
	display_blanking_off(disp);
	k_msleep(500);

	return 0;
}

/* ================= LVGL & GUI Setup ================= */
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_unscii_8)

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

/* State machine for ANSI escape sequence filtering */
typedef enum {
	ANSI_NORMAL,      /* Normal text */
	ANSI_ESCAPE,      /* Just read ESC character */
	ANSI_CSI,         /* Control Sequence Introducer */
	ANSI_CSI_PARAM,   /* CSI parameters */
	ANSI_CSI_INTER,   /* CSI intermediate characters */
	ANSI_CSI_FINAL,   /* CSI final character */
	ANSI_OSC,         /* Operating System Command */
	ANSI_OSC_ESC,     /* OSC waiting for final terminator */
	ANSI_SS,          /* Single Shift */
} ansi_state_t;

/* Shell output callback - called when shell produces output */
static void shell_output_callback(const char *data, size_t len)
{
	if (!data || len == 0) {
		return;
	}
	
	/* Process data with ANSI escape sequence filtering */
	ansi_state_t state = ANSI_NORMAL;
	
	for (size_t i = 0; i < len && shell_display_len < sizeof(shell_display_buffer) - 1; i++) {
		char c = data[i];
		
		switch (state) {
		case ANSI_NORMAL:
			if (c == 0x1B) { /* ESC */
				state = ANSI_ESCAPE;
			} else if (c == '\n') {
				shell_display_buffer[shell_display_len++] = '\n';
			} else if (c == '\b') {
				/* Handle backspace */
				if (shell_display_len > 0) {
					shell_display_len--;
				}
			} else if (c == '\r') {
				/* Ignore carriage return */
			} else if (c >= 32 || c == '\t') {
				shell_display_buffer[shell_display_len++] = c;
			}
			break;
			
		case ANSI_ESCAPE:
			if (c == '[') {
				state = ANSI_CSI; /* Control Sequence Introducer */
			} else if (c == ']') {
				state = ANSI_OSC; /* Operating System Command */
			} else if (c == 'N' || c == 'O') {
				state = ANSI_SS; /* Single Shift */
			} else {
				/* Other ESC sequences, just go back to normal */
				state = ANSI_NORMAL;
			}
			break;
			
		case ANSI_CSI:
			if (c >= 0x30 && c <= 0x3F) {
				/* Parameter characters */
				state = ANSI_CSI_PARAM;
			} else if (c >= 0x20 && c <= 0x2F) {
				/* Intermediate characters */
				state = ANSI_CSI_INTER;
			} else if (c >= 0x40 && c <= 0x7E) {
				/* Final character, end of sequence */
				state = ANSI_NORMAL;
			} else {
				/* Invalid character, back to normal */
				state = ANSI_NORMAL;
			}
			break;
			
		case ANSI_CSI_PARAM:
			if (c >= 0x30 && c <= 0x3F) {
				/* More parameter characters */
			} else if (c >= 0x20 && c <= 0x2F) {
				/* Intermediate characters */
				state = ANSI_CSI_INTER;
			} else if (c >= 0x40 && c <= 0x7E) {
				/* Final character, end of sequence */
				state = ANSI_NORMAL;
			} else {
				/* Invalid character, back to normal */
				state = ANSI_NORMAL;
			}
			break;
			
		case ANSI_CSI_INTER:
			if (c >= 0x20 && c <= 0x2F) {
				/* More intermediate characters */
			} else if (c >= 0x40 && c <= 0x7E) {
				/* Final character, end of sequence */
				state = ANSI_NORMAL;
			} else {
				/* Invalid character, back to normal */
				state = ANSI_NORMAL;
			}
			break;
			
		case ANSI_OSC:
			if (c == 0x1B) {
				/* ESC might be the start of OSC terminator */
				state = ANSI_OSC_ESC;
			}
			/* OSC sequences can be long, just wait for terminator */
			break;
			
		case ANSI_OSC_ESC:
			if (c == '\\') {
				/* OSC terminated, back to normal */
				state = ANSI_NORMAL;
			} else {
				/* Not terminator, go back to OSC */
				state = ANSI_OSC;
			}
			break;
			
		case ANSI_SS:
			/* Single Shift, consume one character */
			state = ANSI_NORMAL;
			break;
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

static void lvgl_init(void)
{
	/* Initialize main screen */
	app.main_screen = lv_screen_active();
	lv_obj_set_style_bg_color(app.main_screen, lv_color_black(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(app.main_screen, LV_OPA_COVER, LV_PART_MAIN);

	/* Create status bar */
	app.status_bar = create_container(app.main_screen,
		 240, 
		 20,
		 0, 
		 0,
		 2,
		 1,
		 lv_color_make(0x40, 0x40, 0x40),
		 0,
		 lv_color_make(0x20, 0x20, 0x20),
		 0);

	app.status_label = create_label(app.status_bar,
		"ESP32-S3 Interactive Shell",
		 2, 
		 2, 
		 230,
		 lv_color_make(0x00, 0x00, 0x00),
		 &lv_font_unscii_8,
		 false);

	/* Create console container */
	app.console_container = create_container(app.main_screen,
		 240, 
		 115,
		 0, 
		 20,
		 2,
		 1, 
		 lv_color_make(0x00, 0x40, 0x00), 
		 0,
		 lv_color_black(),
		 0);

	/* Create console label for shell output */
	app.console_label = create_label(app.console_container,
		 "", 
		 3, 
		 3, 
		 225,
	     lv_color_make(0xFF, 0xFF, 0xFF), 
		 &lv_font_unscii_8, 
		 false);


	/* Configure console label for scrollable multi-line text */
	lv_label_set_long_mode(app.console_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(app.console_label, 225);
	lv_obj_set_height(app.console_label, 105);
	lv_obj_set_style_text_align(app.console_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_set_scrollbar_mode(app.console_label, LV_SCROLLBAR_MODE_AUTO);

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
	strcpy(shell_display_buffer, "");
	shell_display_len = strlen(shell_display_buffer);
	
	if (app.console_label) {
		lv_label_set_text(app.console_label, shell_display_buffer);
		lv_obj_invalidate(app.console_label);
	}
	
	// shell_print(sh, "Console cleared");
	
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

/* Register shell commands */
SHELL_CMD_REGISTER(sysinfo, NULL, "Show system information", cmd_system_info);
SHELL_CMD_REGISTER(clear, NULL, "", cmd_lcd_clear);
SHELL_CMD_REGISTER(demo, NULL, "Run demo sequence", cmd_demo);

int main(void)
{
	int ret;

	/* Initialize hardware */
	ret = lcd_init();
	if(ret != 0) {
		LOG_ERR("Failed to initialize LCD: %d", ret);
		return ret;
	}

	lvgl_init();
	
	/* Initialize shell backend */
	ret = lcd_shell_backend_init();
	if (ret != 0) {
		LOG_ERR("Failed to initialize shell backend: %d", ret);
		return ret;
	}
	
	/* Set shell output callback */
	lcd_shell_set_output_callback(shell_output_callback);
	
	/* Hook into default UART shell transport */
	const struct shell *default_shell = shell_backend_uart_get_ptr();
	if (default_shell && default_shell->iface && default_shell->iface->api) {
		
		/* Replace transport write function with our interceptor */
		struct shell_transport_api *api = (struct shell_transport_api *)default_shell->iface->api;
		original_uart_write = api->write;
		api->write = intercepted_uart_write;
	} else {
		LOG_WRN("Could not hook into UART shell transport");
	}
	
	/* Activate shell output capture */
	shell_capture_active = true;

	uint32_t counter = 0;
	
	while (1) {
		/* Handle LVGL tasks */
		lv_timer_handler();
		
		/* High priority: Check for display update immediately */
		if (display_update_needed && app.console_label) {
			lv_label_set_text(app.console_label, shell_display_buffer);
			lv_obj_scroll_to_y(app.console_label, LV_COORD_MAX, LV_ANIM_OFF);
			display_update_needed = false;
			
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