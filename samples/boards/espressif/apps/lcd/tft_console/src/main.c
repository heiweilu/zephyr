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
#include <stdint.h>
#include "lvgl_wrapper.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* Real interactive shell includes */
#include "lcd_shell_backend.h"

/* ================= LCD initialization ================= */
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

/* Current input line received from backend (real-time) */
#define CURRENT_INPUT_LINE_SIZE 256
static char current_input_line[CURRENT_INPUT_LINE_SIZE] = {0};
static size_t current_input_len = 0;

/* Display update tracking */
static bool display_update_needed = false;

/* ================= ANSI Filtering Helper Functions ================= */

/**
 * @brief Check if a short data packet is an ANSI escape sequence fragment
 *
 * Shell may send ANSI sequences in fragments like:
 * - [1B 5B 6D] = ESC[m (complete sequence)
 * - [5B 6D] = [m (fragment missing ESC)
 * - [6D] = m (lone 'm' character)
 * - [3B 33 32 6D] = ;32m (color code fragment)
 *
 * This function detects and rejects all such fragments.
 *
 * @param data Pointer to data buffer
 * @param len Length of data
 * @return true if data is ANSI garbage and should be filtered
 */
static bool is_ansi_escape_fragment(const char *data, size_t len)
{
	if (len < 1 || len > 6) {
		return false;
	}

	/* Pattern 1: Reject any sequence ending with 'm' (0x6D) */
	if (data[len - 1] == 'm') {
		/* Check if this looks like ANSI garbage */
		bool is_ansi_garbage = false;

		/* Pattern: ends with 'm' and contains '[', digits, or ';' */
		for (size_t i = 0; i < len; i++) {
			char c = data[i];
			if (c == '[' || c == ';' || (c >= '0' && c <= '9') || c == 0x1B) {
				is_ansi_garbage = true;
				break;
			}
		}

		/* Also reject standalone 'm' or very short sequences ending in 'm' */
		if (len == 1 || is_ansi_garbage) {
			return true;
		}
	}

	/* Pattern 2: Reject sequences starting with '[' that look like ANSI fragments */
	if (data[0] == '[' && len >= 2) {
		char second = data[1];
		/* If second char is digit, 'm', 'J', 'D', or ';' - likely ANSI */
		if (second == 'm' || second == 'J' || second == 'D' || second == 'H' ||
		    second == ';' || (second >= '0' && second <= '9')) {
			return true;
		}
	}

	return false;
}

/**
 * @brief Check if data contains only control characters (non-printable)
 *
 * @param data Pointer to data buffer
 * @param len Length of data
 * @return true if data contains only unexpected control characters
 */
static bool is_control_only(const char *data, size_t len)
{
	if (len == 0) {
		return true;
	}

	bool has_control_only = true;
	for (size_t i = 0; i < len; i++) {
		uint8_t c = (uint8_t)data[i];
		/* Allow only specific control chars: newline, tab, backspace, carriage return, ESC
		 */
		if (c >= 32 || c == '\n' || c == '\t' || c == '\b' || c == '\r' || c == 0x1B) {
			has_control_only = false;
			break;
		}
	}

	return has_control_only;
}

/**
 * @brief Process a single character for display filtering
 *
 * This function implements aggressive filtering to prevent ANSI escape
 * sequence fragments from appearing on the LCD display.
 *
 * @param c Character to process
 * @param data Full data buffer (for lookahead)
 * @param len Length of full data buffer
 * @param i Current index in data buffer
 * @return true if character should be skipped
 */
static bool should_skip_character(char c, const char *data, size_t len, size_t i)
{
	/* If this is '[', peek ahead to see if it's part of ANSI code */
	if (c == '[' && i + 1 < len) {
		char next = data[i + 1];
		if ((next >= '0' && next <= '9') || next == 'm' || next == ';') {
			return true;
		}
	}

	/* If this is 'm' and previous char in buffer was '[' or digit, skip it */
	if (c == 'm' && shell_display_len > 0) {
		char prev = shell_display_buffer[shell_display_len - 1];
		if (prev == '[' || (prev >= '0' && prev <= '9') || prev == ';') {
			/* This 'm' is likely the end of an ANSI code, skip it */
			/* Also remove any trailing ANSI garbage from buffer */
			while (shell_display_len > 0) {
				char ch = shell_display_buffer[shell_display_len - 1];
				if (ch == '[' || (ch >= '0' && ch <= '9') || ch == ';') {
					shell_display_len--;
				} else {
					break;
				}
			}
			return true;
		}
	}

	/* Skip digits and semicolons if they appear right after '[' */
	if ((c >= '0' && c <= '9') || c == ';') {
		if (shell_display_len > 0 && shell_display_buffer[shell_display_len - 1] == '[') {
			return true;
		}
	}

	return false;
}

/**
 * @brief Process shell output data and add to display buffer
 *
 * Only allows printable ASCII (32-126), newline, tab, and backspace.
 * Filters out all ANSI escape sequences and control characters.
 *
 * @param data Pointer to output data
 * @param len Length of data
 */
static void process_shell_output(const char *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		char c = data[i];

		if (c == '\n') {
			/* Allow newline */
			if (shell_display_len < sizeof(shell_display_buffer) - 1) {
				shell_display_buffer[shell_display_len++] = '\n';
			}
		} else if (c == '\b') {
			/* Handle backspace */
			if (shell_display_len > 0) {
				shell_display_len--;
			}
		} else if (c == '\t') {
			/* Allow tab */
			if (shell_display_len < sizeof(shell_display_buffer) - 1) {
				shell_display_buffer[shell_display_len++] = '\t';
			}
		} else if (c >= 32 && c <= 126) {
			/* Only allow printable ASCII characters (space to ~) */
			if (!should_skip_character(c, data, len, i) &&
			    shell_display_len < sizeof(shell_display_buffer) - 1) {
				shell_display_buffer[shell_display_len++] = c;
			}
		}
		/* All other characters (ESC, control chars, etc.) are silently dropped */
	}

	shell_display_buffer[shell_display_len] = '\0';

	/* Keep only last ~1500 chars to prevent overflow */
	if (shell_display_len > 1500) {
		size_t keep_len = 1000;
		memmove(shell_display_buffer, shell_display_buffer + (shell_display_len - keep_len),
			keep_len);
		shell_display_len = keep_len;
		shell_display_buffer[shell_display_len] = '\0';
	}
}

/* ================= Shell Output Callback ================= */

/* Shell output callback - called when shell produces output */
static void shell_output_callback(const char *data, size_t len)
{
	if (!data || len == 0) {
		return;
	}

	/* ULTRA-AGGRESSIVE FILTER: Block ANY data containing ANSI escape patterns.
	 * Based on hex dump analysis, shell sends fragments like:
	 * - [1B 5B 6D] = ESC[m (complete, should be filtered by state machine)
	 * - [5B 6D] = [m (fragment missing ESC)
	 * - [6D] = m (lone 'm' character)
	 * - [3B 33 32 6D] = ;32m (ANSI color code fragment)
	 * We must reject ALL of these.
	 */
	if (is_ansi_escape_fragment(data, len)) {
		return; /* Skip ANSI fragment */
	}

	/* Filter out control-only data */
	if (is_control_only(data, len)) {
		return;
	}

	/* Special-case: backend may send input-only updates prefixed with 0x1F (unit separator)
	 * Format: 0x1F <input bytes...>
	 * When detected, update current_input_line immediately and return.
	 */
	if (len > 0 && (uint8_t)data[0] == 0x1F) {
		size_t copy_len = len - 1;

		if (copy_len >= CURRENT_INPUT_LINE_SIZE) {
			copy_len = CURRENT_INPUT_LINE_SIZE - 1;
		}

		memcpy(current_input_line, data + 1, copy_len);

		current_input_line[copy_len] = '\0';
		current_input_len = copy_len;
		display_update_needed = true;

		LOG_INF("main: received input update len=%zu content='%s'", copy_len,
			current_input_line);

		return;
	}

	/* Process and filter shell output */
	process_shell_output(data, len);

	/* Mark that display needs update */
	display_update_needed = true;
}

/* Shell output capture mechanism */
static bool shell_capture_active = false;

/* Transport write function interception */
static int (*original_uart_write)(const struct shell_transport *transport, const void *data,
				  size_t length, size_t *cnt) = NULL;
/* Transport read function interception (to capture input bytes) */
static int (*original_uart_read)(const struct shell_transport *transport, void *data, size_t length,
				 size_t *cnt) = NULL;

/* Small buffer to remember last intercepted read (to track tab completion)
 * When user presses Tab, we detect the tab in read, then capture the completion
 * text from the subsequent write and forward it to update the input display.
 */
#define LAST_READ_BUF_SIZE 32
#define ECHO_WINDOW_MS     200

typedef struct {
	uint8_t last_read_buf[LAST_READ_BUF_SIZE];
	size_t last_read_len;
	uint32_t last_read_ts;
	bool last_read_was_tab;
} last_read_t;
last_read_t last_read = {0};

/* Intercepted UART transport write function */
static int intercepted_uart_write(const struct shell_transport *transport, const void *data,
				  size_t length, size_t *cnt)
{
	/* Forward to LCD if capture is active */
	if (shell_capture_active && data && length > 0) {
		const char *str = (const char *)data;

		/* Filter out LOG messages to avoid infinite recursion */
		if (length < 20 || strncmp(str, "[00:", 4) != 0) {
			/* Forward to output callback for display history */
			shell_output_callback(str, length);

			/* Special handling for tab completion:
			 * If the last read was a tab character and this write contains
			 * printable characters (the completion), update the input buffer.
			 */
			if (last_read.last_read_was_tab && length > 0 && length < 64) {
				/* Check if this looks like a completion (printable chars, no
				 * newline) */
				bool looks_like_completion = true;
				for (size_t i = 0; i < length; i++) {
					char c = str[i];
					if (c == '\n' || c == '\r' || c == 0x1B) {
						looks_like_completion = false;
						break;
					}
					if (c < 32 && c != '\t' && c != '\b') {
						looks_like_completion = false;
						break;
					}
				}

				if (looks_like_completion) {
					/* This is likely tab completion output - forward to input
					 * handler */
					LOG_DBG("Tab completion detected, forwarding %zu bytes to "
						"input",
						length);
					lcd_shell_send_input(str, length);
					last_read.last_read_was_tab = false; /* Reset flag */
				}
			}
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
	app.status_bar = create_container(app.main_screen, 240, 20, 0, 0, 2, 1,
					  lv_color_make(0x40, 0x40, 0x40), 0,
					  lv_color_make(0x20, 0x20, 0x20), 0);

	app.status_label = create_label(app.status_bar, "ESP32-S3 Interactive Shell", 2, 2, 230,
					lv_color_make(0x00, 0x00, 0x00), &lv_font_unscii_8, false);

	/* Create console container */
	app.console_container =
		create_container(app.main_screen, 240, 115, 0, 20, 2, 1,
				 lv_color_make(0x00, 0x40, 0x00), 0, lv_color_black(), 0);

	/* Create console label for shell output */
	app.console_label = create_label(app.console_container, "", 3, 3, 225,
					 lv_color_make(0xFF, 0xFF, 0xFF), &lv_font_unscii_8, false);

	/* Configure console label for scrollable multi-line text */
	lv_label_set_long_mode(app.console_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(app.console_label, 225);
	lv_obj_set_height(app.console_label, 105);
	lv_obj_set_style_text_align(app.console_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_set_scrollbar_mode(app.console_label, LV_SCROLLBAR_MODE_AUTO);
}

/* if want to print to serial: shell_print(sh, "text %d", value); */
static int cmd_system_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	static const char *const sys_info[] = {"== Sys Info ==\n",
					       "CPU: ESP32-S3 @ 240MHz\n",
					       "PSRAM: 8MB\n",
					       "Flash: 16MB\n",
					       "Zephyr: v4.2.99\n",
					       "LVGL: v9.x\n",
					       NULL};

	for (int i = 0; sys_info[i] != NULL; i++) {
		shell_output_callback(sys_info[i], strlen(sys_info[i]));
	}

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

	return 0;
}

static int cmd_demo(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_output_callback("Running demo\n", 25);

	for (int i = 1; i <= 5; i++) {
		char step_buf[32];

		snprintf(step_buf, sizeof(step_buf), "Demo step %d/5\n", i);
		shell_output_callback(step_buf, strlen(step_buf));

		k_msleep(500);
	}

	shell_output_callback("Demo completed!\n", 16);

	return 0;
}

/* Register shell commands */
SHELL_CMD_REGISTER(sysinfo, NULL, "Show system information", cmd_system_info);
SHELL_CMD_REGISTER(clear, NULL, "", cmd_lcd_clear);
SHELL_CMD_REGISTER(demo, NULL, "Run demo", cmd_demo);

/* Forward declaration for intercepted read function (definition after main) */
static int intercepted_uart_read(const struct shell_transport *transport, void *data, size_t length,
				 size_t *cnt);

/* Consume any backend input updates from message queue (non-blocking) */
static void consume_backend_input(void)
{
	uint8_t msg_type = 0;
	char inbuf[CURRENT_INPUT_LINE_SIZE];
	int got = 0;

	while ((got = lcd_shell_try_get_input(&msg_type, inbuf, sizeof(inbuf))) >= 0) {
		if (got == 0 && msg_type == 0) {
			break; /* no more messages */
		}

		if (msg_type == MSG_TYPE_INPUT) {
			/* Update current input line for rendering */
			size_t copy_len = (got < CURRENT_INPUT_LINE_SIZE - 1)
						  ? (size_t)got
						  : (CURRENT_INPUT_LINE_SIZE - 1);

			memcpy(current_input_line, inbuf, copy_len);
			current_input_line[copy_len] = '\0';
			current_input_len = copy_len;
			display_update_needed = true;
		} else if (msg_type == MSG_TYPE_ENTER) {
			/* Clear any pending typed text shown at prompt area.
			 * Remove the trailing partial line (prompt + typed)
			 * from shell_display_buffer so console shows only output.
			 */
			/* Find last newline in shell_display_buffer */
			ssize_t last_nl = -1;

			for (ssize_t i = (ssize_t)shell_display_len - 1; i >= 0; i--) {
				if (shell_display_buffer[i] == '\n') {
					last_nl = i;
					break;
				}
			}

			if (last_nl >= 0) {
				/* keep up to and including last_nl */
				shell_display_len = (size_t)last_nl + 1;
				shell_display_buffer[shell_display_len] = '\0';
			} else {
				/* No newline, clear whole buffer */
				shell_display_len = 0;
				shell_display_buffer[0] = '\0';
			}

			/* Also clear the current input line displayed */
			current_input_len = 0;
			current_input_line[0] = '\0';
			display_update_needed = true;
		}
	}
}
/* High priority: Check for display update immediately */
static void check_display_update(void)
{
	if (display_update_needed && app.console_label) {
		/* 如果有当前输入行，把它临时附加到显示缓冲的末尾用于渲染 */
		char tmp_buf[SHELL_DISPLAY_BUFFER_SIZE + CURRENT_INPUT_LINE_SIZE];
		/* Build tmp_buf by copying shell_display_buffer but replace the trailing
		 * partial line (after last '\n') with current_input_line so edits are
		 * immediately visible where the prompt/input sits.
		 */
		size_t base_len = 0;
		/* Find last newline in shell_display_buffer */
		ssize_t last_nl = -1;
		for (ssize_t i = (ssize_t)shell_display_len - 1; i >= 0; i--) {
			if (shell_display_buffer[i] == '\n') {
				last_nl = i;
				break;
			}
		}
		if (last_nl >= 0) {
			/* Keep up to and including the last newline */
			base_len = (size_t)last_nl + 1;
		} else {
			/* No newline found: drop any trailing partial content and start
			 * fresh */
			base_len = 0;
		}
		if (base_len > sizeof(tmp_buf) - 1) {
			base_len = sizeof(tmp_buf) - 1;
		}
		memcpy(tmp_buf, shell_display_buffer, base_len);
		/* Append prompt and current input line after the last newline */
		const char *prompt = "s3:~$ ";
		size_t prompt_len = strlen(prompt);
		/* Ensure prompt fits */
		size_t copy_len = prompt_len + current_input_len;
		if (base_len + copy_len >= sizeof(tmp_buf) - 1) {
			copy_len = sizeof(tmp_buf) - 1 - base_len;
		}
		if (copy_len > 0) {
			/* Copy prompt then input (truncated to fit) */
			size_t to_copy_prompt = prompt_len;
			if (to_copy_prompt > sizeof(tmp_buf) - 1 - base_len) {
				to_copy_prompt = sizeof(tmp_buf) - 1 - base_len;
			}
			memcpy(tmp_buf + base_len, prompt, to_copy_prompt);
			base_len += to_copy_prompt;
			/* Copy input content after prompt */
			size_t avail = sizeof(tmp_buf) - 1 - base_len;
			size_t to_copy_input = current_input_len;
			if (to_copy_input > avail) {
				to_copy_input = avail;
			}
			if (to_copy_input > 0) {
				memcpy(tmp_buf + base_len, current_input_line, to_copy_input);
				base_len += to_copy_input;
			}
		}
		tmp_buf[base_len] = '\0';
		lv_label_set_text(app.console_label, tmp_buf);
		lv_obj_scroll_to_y(app.console_label, LV_COORD_MAX, LV_ANIM_OFF);
		/* Also update status label with current input for quick visual debug */
		if (app.status_label) {
			char in_dbg[512];
			if (current_input_len > 0) {
				snprintk(in_dbg, sizeof(in_dbg), "in:%s", current_input_line);
			} else {
				snprintk(in_dbg, sizeof(in_dbg), "in:");
			}
			lv_label_set_text(app.status_label, in_dbg);
			lv_obj_invalidate(app.status_label);
		}
		lv_obj_invalidate(app.console_label);
		display_update_needed = false;
	}
}
/* Intercepted UART read function */
static int intercepted_uart_read(const struct shell_transport *transport, void *data, size_t length,
				 size_t *cnt)
{
	int ret = 0;
	/* Call original read to let shell consume data as usual */
	if (original_uart_read) {
		ret = original_uart_read(transport, data, length, cnt);
	} else {
		*cnt = 0;
		ret = 0;
	}

	/* If shell_capture_active and we received input bytes, forward to lcd backend input handler
	 */
	if (shell_capture_active && data && cnt && *cnt > 0) {
		const char *input_data = (const char *)data;

		/* Check if this read contains a tab character */
		last_read.last_read_was_tab = false;
		for (size_t i = 0; i < *cnt; i++) {
			if (input_data[i] == '\t') {
				last_read.last_read_was_tab = true;
				last_read.last_read_ts = k_uptime_get_32();
				LOG_DBG("Tab character detected in read");
				break;
			}
		}

		/* Store last read data for echo detection */
		size_t copy_len = (*cnt < LAST_READ_BUF_SIZE) ? *cnt : LAST_READ_BUF_SIZE;
		memcpy(last_read.last_read_buf, data, copy_len);
		last_read.last_read_len = copy_len;

		/* Forward to backend to update LCD input line */
		LOG_DBG("intercepted read: got %zu bytes", *cnt);
		lcd_shell_send_input(input_data, *cnt);
	}

	return ret;
}

/* Update status bar every 5 seconds */
typedef struct {
	uint32_t counter;
	uint32_t uptime_ms;
	uint32_t seconds;
	uint32_t minutes;
	uint32_t hours;
} status_info_t;
static status_info_t status_info = {0};

static void update_status_bar(void)
{
	if (status_info.counter % 200 == 0) {
		status_info.uptime_ms = k_uptime_get_32();
		status_info.seconds = status_info.uptime_ms / 1000;
		status_info.minutes = status_info.seconds / 60;
		status_info.hours = status_info.minutes / 60;

		char status_text[64];
		snprintf(status_text, sizeof(status_text), "Shell Active | Up: %02lu:%02lu:%02lu",
			 (unsigned long)(status_info.hours % 24),
			 (unsigned long)(status_info.minutes % 60),
			 (unsigned long)(status_info.seconds % 60));

		if (app.status_label) {
			lv_label_set_text(app.status_label, status_text);
		}
	}

	status_info.counter++;
}

/**
 * hook_uart_shell_transport
 *
 * Locate the default UART shell transport and replace its read/write
 * function pointers with our interceptors so we can capture both
 * output (for history rendering) and input bytes (for live prompt updates).
 *
 * This function preserves the original function pointers in
 * `original_uart_write` and `original_uart_read` so the shell continues
 * to operate normally after interception.
 */
static void hook_uart_shell_transport(void)
{
	const struct shell *default_shell = shell_backend_uart_get_ptr();
	if (default_shell && default_shell->iface && default_shell->iface->api) {
		struct shell_transport_api *api =
			(struct shell_transport_api *)default_shell->iface->api;

		// Save original function pointers
		original_uart_write = api->write;
		original_uart_read = api->read;

		// Replace with our interceptor functions
		api->write = intercepted_uart_write;
		api->read = intercepted_uart_read;

		LOG_INF("UART shell transport functions intercepted successfully");
	} else {
		LOG_WRN("Could not hook into UART shell transport");
	}
}
int main(void)
{
	int ret;

	ret = lcd_init();
	if (ret != 0) {
		LOG_ERR("Failed to initialize LCD: %d", ret);
		return ret;
	}

	lvgl_init();

	ret = lcd_shell_backend_init();
	if (ret != 0) {
		LOG_ERR("Failed to initialize shell backend: %d", ret);
		return ret;
	}

	/* Set shell output callback */
	lcd_shell_set_output_callback(shell_output_callback);

	/* Hook into default UART shell transport */
	hook_uart_shell_transport();

	/* Activate shell output capture */
	shell_capture_active = true;

	while (1) {
		/* Handle LVGL tasks */
		lv_timer_handler();

		/* Consume any pending input from shell backend */
		consume_backend_input();

		/* Check if display needs update */
		check_display_update();

		/* Update status bar every 5 seconds */
		update_status_bar();

		/* Reduced sleep time for faster LCD updates */
		k_msleep(25);
	}

	return 0;
}
