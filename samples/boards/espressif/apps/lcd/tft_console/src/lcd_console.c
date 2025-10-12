/*
 * LCD Console Redirect Module Implementation
 * Copyright (c) 2025 Heiweilu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lcd_console.h"
#include "lvgl_wrapper.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/console/console.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_backend_std.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/printk-hooks.h>
#include <zephyr/sys/util.h>

LV_FONT_DECLARE(lv_font_unscii_8)

LOG_MODULE_REGISTER(lcd_console, LOG_LEVEL_INF);

#define LCD_CONSOLE_EVENT_DATA_MAX 96
#define LCD_CONSOLE_EVENT_QUEUE_LEN 16

enum lcd_console_event_type {
	LCD_EVENT_OUTPUT = 0,
	LCD_EVENT_INPUT = 1,
};

struct lcd_console_event {
	uint8_t type;
	uint8_t len;
	char data[LCD_CONSOLE_EVENT_DATA_MAX];
};

K_MSGQ_DEFINE(lcd_console_event_queue,
		      sizeof(struct lcd_console_event),
		      LCD_CONSOLE_EVENT_QUEUE_LEN,
		      4);

static lcd_console_t g_lcd_console;
static bool console_initialized;
static printk_hook_fn_t original_console_out;

static void lcd_console_enqueue(enum lcd_console_event_type type,
				       const char *data,
				       size_t len);
static void lcd_console_process_queue(void);
static void lcd_console_handle_output(const char *data, size_t len);
static void lcd_console_handle_input(const char *data, size_t len);
static void lcd_console_reset_input(void);

#if defined(CONFIG_LOG)
static uint8_t lcd_console_log_buffer[CONFIG_LCD_CONSOLE_LOG_BUFFER_SIZE];

static int lcd_console_log_out(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);
	lcd_console_write((const char *)data, length);
	return length;
}

LOG_OUTPUT_DEFINE(lcd_console_log_output,
		      lcd_console_log_out,
		      lcd_console_log_buffer,
		      sizeof(lcd_console_log_buffer));

struct lcd_console_log_ctx {
	const struct log_output *output;
	uint32_t format;
	bool panic_mode;
};

static struct lcd_console_log_ctx lcd_console_log_ctx = {
	.output = &lcd_console_log_output,
	.format = LOG_OUTPUT_TEXT,
	.panic_mode = false,
};

static void lcd_console_log_process(const struct log_backend *const backend,
				        union log_msg_generic *msg)
{
	struct lcd_console_log_ctx *ctx = backend->cb->ctx;
	log_format_func_t formatter = log_format_func_t_get(ctx->format);

	if (formatter == NULL) {
		return;
	}

	uint32_t flags = log_backend_std_get_flags();
	formatter(ctx->output, &msg->log, flags);
}

static void lcd_console_log_panic(const struct log_backend *const backend)
{
	struct lcd_console_log_ctx *ctx = backend->cb->ctx;

	ctx->panic_mode = true;
	log_backend_std_panic(ctx->output);
}

static void lcd_console_log_dropped(const struct log_backend *const backend,
				       uint32_t cnt)
{
	struct lcd_console_log_ctx *ctx = backend->cb->ctx;

	log_backend_std_dropped(ctx->output, cnt);
}

static int lcd_console_log_format_set(const struct log_backend *const backend,
				          uint32_t log_type)
{
	if (log_format_func_t_get(log_type) == NULL) {
		return -EINVAL;
	}

	struct lcd_console_log_ctx *ctx = backend->cb->ctx;

	ctx->format = log_type;
	return 0;
}

static void lcd_console_log_init(struct log_backend const *const backend)
{
	struct lcd_console_log_ctx *ctx = backend->cb->ctx;

	ctx->panic_mode = false;
}

static const struct log_backend_api lcd_console_log_backend_api = {
	.process = lcd_console_log_process,
	.panic = lcd_console_log_panic,
	.dropped = lcd_console_log_dropped,
	.init = lcd_console_log_init,
	.format_set = lcd_console_log_format_set,
};

LOG_BACKEND_DEFINE(log_backend_lcd_console,
		       lcd_console_log_backend_api,
		       true,
		       &lcd_console_log_ctx);
#endif /* CONFIG_LOG */

static void lcd_console_reset_input(void)
{
	g_lcd_console.input_len = 0U;
	g_lcd_console.input_buffer[0] = '\0';
	if (g_lcd_console.input_area != NULL) {
		lv_textarea_set_text(g_lcd_console.input_area, "");
	}
}

static void lcd_console_enqueue(enum lcd_console_event_type type,
				       const char *data,
				       size_t len)
{
	while (len > 0U) {
		struct lcd_console_event evt = {
			.type = (uint8_t)type,
		};
		size_t chunk = MIN(len, (size_t)LCD_CONSOLE_EVENT_DATA_MAX);

		evt.len = (uint8_t)chunk;
		memcpy(evt.data, data, chunk);

		if (k_msgq_put(&lcd_console_event_queue, &evt, K_NO_WAIT) != 0) {
			break;
		}

		data += chunk;
		len -= chunk;
	}
}

static void lcd_console_handle_output(const char *data, size_t len)
{
	lcd_console_t *console = &g_lcd_console;

	k_mutex_lock(&console->buffer_mutex, K_FOREVER);

	for (size_t i = 0U; i < len; i++) {
		char c = data[i];

		switch (console->ansi_state) {
		case LCD_ANSI_IDLE:
			if ((unsigned char)c == 0x1B) {
				console->ansi_state = LCD_ANSI_ESC;
				continue;
			}
			break;
		case LCD_ANSI_ESC:
			console->ansi_state = (c == '[') ? LCD_ANSI_CSI : LCD_ANSI_IDLE;
			continue;
		case LCD_ANSI_CSI:
			if (c >= '@' && c <= '~') {
				console->ansi_state = LCD_ANSI_IDLE;
			}
			continue;
		default:
			console->ansi_state = LCD_ANSI_IDLE;
			break;
		}

		if (c == '\r') {
			continue;
		}

		if (c == '\b' || (unsigned char)c == 0x7F) {
			if (console->buffer_len > 0U) {
				console->buffer_len--;
				console->console_buffer[console->buffer_len] = '\0';
			}
			continue;
		}

		if ((unsigned char)c < 0x08) {
			continue;
		}

		if (console->buffer_len + 1U >= LCD_CONSOLE_BUFFER_SIZE) {
			char *newline = strchr(console->console_buffer, '\n');

			if (newline != NULL) {
				size_t shift = (size_t)(newline - console->console_buffer) + 1U;
				size_t remaining = console->buffer_len - shift;

				memmove(console->console_buffer,
					console->console_buffer + shift,
					remaining);
				console->buffer_len = remaining;
				console->console_buffer[console->buffer_len] = '\0';
				if (console->current_line > 0) {
					console->current_line--;
				}
			} else {
				console->buffer_len = 0U;
				console->console_buffer[0] = '\0';
			}
		}

		console->console_buffer[console->buffer_len++] = c;
		console->console_buffer[console->buffer_len] = '\0';

		if (c == '\n') {
			console->current_line++;
			console->total_lines++;
		}
	}

	if (console->console_label != NULL) {
		lv_label_set_text(console->console_label, console->console_buffer);
		lv_obj_scroll_to_y(console->console_area, LV_COORD_MAX, LV_ANIM_OFF);
	}

	k_mutex_unlock(&console->buffer_mutex);
}

static void lcd_console_handle_input(const char *data, size_t len)
{
	lcd_console_t *console = &g_lcd_console;
	bool changed = false;

	for (size_t i = 0U; i < len; i++) {
		char c = data[i];

		if (c == '\r' || c == '\n') {
			lcd_console_reset_input();
			changed = true;
			continue;
		}

		if (c == '\b' || (unsigned char)c == 0x7F) {
			if (console->input_len > 0U) {
				console->input_len--;
				console->input_buffer[console->input_len] = '\0';
				changed = true;
			}
			continue;
		}

		if (!isprint((unsigned char)c)) {
			continue;
		}

		if (console->input_len + 1U < sizeof(console->input_buffer)) {
			console->input_buffer[console->input_len++] = c;
			console->input_buffer[console->input_len] = '\0';
			changed = true;
		}
	}

	if (changed && console->input_area != NULL && console->shell_enabled) {
		lv_textarea_set_text(console->input_area, console->input_buffer);
	}
}

static void lcd_console_process_queue(void)
{
	struct lcd_console_event evt;

	while (k_msgq_get(&lcd_console_event_queue, &evt, K_NO_WAIT) == 0) {
		if (evt.type == LCD_EVENT_OUTPUT) {
			lcd_console_handle_output(evt.data, evt.len);
		} else {
			lcd_console_handle_input(evt.data, evt.len);
		}
	}
}

int lcd_console_output_hook(int c)
{
	if (!console_initialized || !g_lcd_console.console_enabled) {
		if (original_console_out != NULL) {
			return original_console_out(c);
		}
		return c;
	}

	static char line_buffer[128];
	static size_t line_pos;

	if (c == '\r') {
		return (original_console_out != NULL) ? original_console_out(c) : c;
	}

	if (c == '\n') {
		if (line_pos > 0U) {
			lcd_console_enqueue(LCD_EVENT_OUTPUT, line_buffer, line_pos);
			line_pos = 0U;
		}
		char newline = '\n';
		lcd_console_enqueue(LCD_EVENT_OUTPUT, &newline, 1U);
		if (!k_is_in_isr()) {
			lcd_console_process_queue();
		}
	} else if (isprint((unsigned char)c) && line_pos < ARRAY_SIZE(line_buffer) - 1U) {
		line_buffer[line_pos++] = (char)c;
	}

	if (original_console_out != NULL) {
		return original_console_out(c);
	}

	return c;
}

int lcd_console_init(lv_obj_t *parent)
{
	if (console_initialized) {
		return 0;
	}

	memset(&g_lcd_console, 0, sizeof(g_lcd_console));
	k_mutex_init(&g_lcd_console.buffer_mutex);
	k_msgq_purge(&lcd_console_event_queue);

	g_lcd_console.shell_container = create_container(parent,
						      240,
						      135,
						      0,
						      0,
						      0,
						      2,
						      lv_color_white(),
						      2,
						      lv_color_black(),
						      1);

	g_lcd_console.console_area = create_container(g_lcd_console.shell_container,
						      230,
						      100,
						      2,
						      2,
						      0,
						      1,
						      lv_color_make(0x40, 0x40, 0x40),
						      0,
						      lv_color_black(),
						      0);

	g_lcd_console.console_label = create_label(g_lcd_console.console_area,
					  "",
					  -1,
					  -1,
					  200,
					  lv_color_make(0xFF, 0xFF, 0xFF),
					  &lv_font_unscii_8,
					  false);

	g_lcd_console.input_area = create_textarea(g_lcd_console.shell_container,
						 210,
						 30,
						 5,
						 103,
						 lv_color_make(0x20, 0x20, 0x20),
						 lv_color_white(),
						 &lv_font_unscii_8,
						 "> Enter command...",
						 true);

	g_lcd_console.console_enabled = true;
	g_lcd_console.shell_enabled = true;
	g_lcd_console.buffer_len = 0U;
	g_lcd_console.ansi_state = LCD_ANSI_IDLE;

	lcd_console_reset_input();
	console_initialized = true;

	lcd_console_write("ESP32-S3 LCD Console\n", strlen("ESP32-S3 LCD Console\n"));
	lcd_console_write("Shell Ready!\n", strlen("Shell Ready!\n"));

	original_console_out = __printk_get_hook();
	__printk_hook_install(lcd_console_output_hook);

	LOG_INF("LCD Console initialized successfully");

	return 0;
}

void lcd_console_enable(bool enable)
{
	g_lcd_console.console_enabled = enable;
	if (enable) {
		lv_obj_clear_flag(g_lcd_console.shell_container, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(g_lcd_console.shell_container, LV_OBJ_FLAG_HIDDEN);
	}
}

void lcd_shell_enable(bool enable)
{
	g_lcd_console.shell_enabled = enable;
	if (enable) {
		lv_obj_clear_flag(g_lcd_console.input_area, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(g_lcd_console.input_area, LV_OBJ_FLAG_HIDDEN);
	}
}

void lcd_console_write(const char *text, size_t length)
{
	if (!console_initialized || text == NULL || length == 0U) {
		return;
	}

	lcd_console_enqueue(LCD_EVENT_OUTPUT, text, length);

	if (!k_is_in_isr()) {
		lcd_console_process_queue();
	}
}

void lcd_console_clear(void)
{
	if (!console_initialized) {
		return;
	}

	k_mutex_lock(&g_lcd_console.buffer_mutex, K_FOREVER);

	g_lcd_console.console_buffer[0] = '\0';
	g_lcd_console.buffer_len = 0U;
	g_lcd_console.current_line = 0;
	g_lcd_console.total_lines = 0;

	if (g_lcd_console.console_label != NULL) {
		lv_label_set_text(g_lcd_console.console_label, "");
	}

	k_mutex_unlock(&g_lcd_console.buffer_mutex);
}

void lcd_shell_process_command(const char *cmd)
{
	if (!console_initialized || cmd == NULL) {
		return;
	}

	char response[256];

	snprintf(response, sizeof(response), "esp32s3:~$ %s\n", cmd);
	lcd_console_write(response, strlen(response));

	if (strcmp(cmd, "help") == 0) {
		lcd_console_write("Available commands:\n", 21);
		lcd_console_write("  help    - Show this help\n", 28);
		lcd_console_write("  clear   - Clear console\n", 27);
		lcd_console_write("  version - Show version\n", 26);
		lcd_console_write("  uptime  - Show uptime\n", 25);
	} else if (strcmp(cmd, "clear") == 0) {
		lcd_console_clear();
		lcd_console_write("Console cleared.\n", 18);
	} else if (strcmp(cmd, "version") == 0) {
		lcd_console_write("ESP32-S3 LCD Console v1.0\n", 28);
		lcd_console_write("Zephyr RTOS + LVGL\n", 21);
	} else if (strcmp(cmd, "uptime") == 0) {
		uint32_t uptime = k_uptime_get_32() / 1000U;
		snprintf(response, sizeof(response), "Uptime: %u seconds\n", uptime);
		lcd_console_write(response, strlen(response));
	} else if (strlen(cmd) > 0U) {
		snprintf(response, sizeof(response), "Unknown command: %s\n", cmd);
		lcd_console_write(response, strlen(response));
	}
}

void lcd_console_update_display(void)
{
	if (!console_initialized) {
		return;
	}

	lcd_console_process_queue();

	static uint32_t last_update;
	uint32_t now = k_uptime_get_32();

	if (now - last_update > 10000U) {
		char time_str[48];
		uint32_t seconds = now / 1000U;
		uint32_t minutes = seconds / 60U;
		uint32_t hours = minutes / 60U;

		snprintf(time_str, sizeof(time_str),
			 "[%02u:%02u:%02u] System OK\n",
			 (unsigned int)(hours % 24U),
			 (unsigned int)(minutes % 60U),
			 (unsigned int)(seconds % 60U));
		lcd_console_write(time_str, strlen(time_str));
		last_update = now;
	}
}

lcd_console_t *lcd_console_get_instance(void)
{
	return console_initialized ? &g_lcd_console : NULL;
}

#if defined(CONFIG_LCD_CONSOLE_MIRROR)
void lcd_console_mirror_tx_feed(const uint8_t *data, size_t len)
{
	if (!console_initialized || data == NULL || len == 0U) {
		return;
	}

	lcd_console_enqueue(LCD_EVENT_OUTPUT, (const char *)data, len);

	if (!k_is_in_isr()) {
		lcd_console_process_queue();
	}
}

void lcd_console_mirror_rx_feed(const uint8_t *data, size_t len)
{
	if (!console_initialized || data == NULL || len == 0U) {
		return;
	}

	lcd_console_enqueue(LCD_EVENT_INPUT, (const char *)data, len);

	if (!k_is_in_isr()) {
		lcd_console_process_queue();
	}
}
#endif /* CONFIG_LCD_CONSOLE_MIRROR */

