/*
 * LCD Shell Backend Implementation - Real Interactive Shell
 * Copyright (c) 2025 Heiweilu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lcd_shell_backend.h"

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(lcd_shell_backend, LOG_LEVEL_INF);

/* Configuration/limits for lcd shell backend
 * REPEAT_THRESHOLD: number of consecutive identical printable characters
 * allowed in the local input buffer before additional repeats are ignored.
 * This is a defensive measure to mitigate spurious repeated bytes that may
 * arrive from fragmented output or terminal noise. Make it a macro so it can
 * be turned into a Kconfig option later if desired.
 */
#ifndef REPEAT_THRESHOLD
#define REPEAT_THRESHOLD 3
#endif

static lcd_shell_output_cb_t output_callback = NULL;

/* Shell input buffer */
#define SHELL_INPUT_BUFFER_SIZE 256
static char shell_input_buffer[SHELL_INPUT_BUFFER_SIZE];
static size_t shell_input_pos = 0;

/* Single latest-message buffer protected by a mutex to avoid queue flooding.
 * Format: [type(1)] [payload NUL-terminated up to SHELL_INPUT_BUFFER_SIZE]
 */
#define INPUT_MSG_SIZE (1 + SHELL_INPUT_BUFFER_SIZE)
static char latest_msg[INPUT_MSG_SIZE];
static struct k_mutex latest_msg_lock;
static bool latest_msg_valid = false;

/* Shell output function */
static int shell_lcd_write(const struct shell_transport *transport, const void *data, size_t length,
			   size_t *cnt)
{
	ARG_UNUSED(transport);

	if (output_callback && data && length > 0) {
		output_callback((const char *)data, length);
		/* keep this silent to avoid flooding logs */
	}

	*cnt = length;
	return 0;
}

/* Shell read function (not used for output-only backend) */
static int shell_lcd_read(const struct shell_transport *transport, void *data, size_t length,
			  size_t *cnt)
{
	ARG_UNUSED(transport);
	ARG_UNUSED(data);
	ARG_UNUSED(length);

	*cnt = 0;
	return 0;
}

/* Shell initialization function */
static int shell_lcd_init(const struct shell_transport *transport, const void *config,
			  shell_transport_handler_t evt_handler, void *context)
{
	ARG_UNUSED(transport);
	ARG_UNUSED(config);
	ARG_UNUSED(evt_handler);
	ARG_UNUSED(context);

	LOG_INF("LCD Shell transport initialized");
	return 0;
}

/* Shell uninitialization function */
static int shell_lcd_uninit(const struct shell_transport *transport)
{
	ARG_UNUSED(transport);
	return 0;
}

/* Shell enable function */
static int shell_lcd_enable(const struct shell_transport *transport, bool blocking)
{
	ARG_UNUSED(transport);
	ARG_UNUSED(blocking);
	return 0;
}

/* Shell transport API */
static const struct shell_transport_api shell_lcd_transport_api = {
	.init = shell_lcd_init,
	.uninit = shell_lcd_uninit,
	.enable = shell_lcd_enable,
	.write = shell_lcd_write,
	.read = shell_lcd_read,
};

/* Shell transport instance */
static struct shell_transport shell_lcd_transport = {.api = &shell_lcd_transport_api, .ctx = NULL};

/* Shell instance */
SHELL_DEFINE(lcd_shell, "s3:~$ ", &shell_lcd_transport, 10, SHELL_FLAG_OLF_CRLF, 1024);

int lcd_shell_backend_init(void)
{
	shell_input_pos = 0;
	memset(shell_input_buffer, 0, sizeof(shell_input_buffer));
	/* Initialize mutex and clear latest message */
	k_mutex_init(&latest_msg_lock);
	memset(latest_msg, 0, sizeof(latest_msg));
	latest_msg_valid = false;

	return 0;
}

const struct shell *lcd_shell_get_instance(void)
{
	return &lcd_shell;
}

void lcd_shell_send_input(const char *input, size_t len)
{
	if (!input || len == 0) {
		return;
	}

/* Debug: Log raw incoming bytes with hex dump - use printk to avoid recursion */
#if 0 /* Disable - "mmm" issue resolved */
    if (len > 0 && len <= 4) {  /* Only log short sequences like Enter key */
        char hex[128] = {0};
        for (size_t i = 0; i < len; i++) {
            int off = strlen(hex);
            snprintf(&hex[off], sizeof(hex) - off, "%02X ", (uint8_t)input[i]);
        }
        printk("[BACKEND_IN] len=%zu hex=[%s]\n", len, hex);
    }
#endif

	for (size_t i = 0; i < len; i++) {
		char ch = input[i];
		if (ch == '\b' || ch == 127) { /* Backspace */
			if (shell_input_pos > 0) {
				shell_input_pos--;
				shell_input_buffer[shell_input_pos] = '\0';
			}
		} else if (ch == '\t') { /* Tab - ignore, shell will handle completion */
			/* Tab completion is handled by shell itself.
			 * The completed text will come back as write output which
			 * we'll capture through the intercepted_uart_write.
			 * Don't add tab to the input buffer.
			 */
			LOG_DBG("lcd_backend: TAB received (completion request)");
		} else if (ch == '\n' || ch == '\r') { /* Enter */
			/* When Enter is received, enqueue an ENTER message to clear display */
			LOG_DBG("lcd_backend: ENTER received; clearing input buffer");
			{
				char msg[INPUT_MSG_SIZE];
				msg[0] = (char)MSG_TYPE_ENTER;
				memset(&msg[1], 0, INPUT_MSG_SIZE - 1);
				/* Overwrite latest_msg atomically */
				k_mutex_lock(&latest_msg_lock, K_FOREVER);
				memcpy(latest_msg, msg, INPUT_MSG_SIZE);
				latest_msg_valid = true;
				k_mutex_unlock(&latest_msg_lock);
			}
			/* clear local buffer for future typing */
			shell_input_pos = 0;
			memset(shell_input_buffer, 0, sizeof(shell_input_buffer));
		} else if (ch >= 32 && ch <= 126) { /* Printable characters */
			/* Protect against spurious repeated characters (e.g., 'mmm')
			 * seen intermittently: only allow up to REPEAT_THRESHOLD
			 * consecutive identical printable characters.
			 */
			/* Use macro so threshold is configurable */
			const int REPEAT_THRESHOLD_LOCAL = REPEAT_THRESHOLD;
			bool allow = true;
			if (shell_input_pos > 0 && shell_input_pos >= REPEAT_THRESHOLD_LOCAL) {
				bool all_same = true;
				for (int r = 1; r <= REPEAT_THRESHOLD_LOCAL; r++) {
					if (shell_input_buffer[shell_input_pos - r] != ch) {
						all_same = false;
						break;
					}
				}
				if (all_same) {
					allow = false;
				}
			}
			if (allow && shell_input_pos < sizeof(shell_input_buffer) - 1) {
				shell_input_buffer[shell_input_pos++] = ch;
			} else if (!allow) {
				/* Log suppressed repeat pattern for debugging (limited) */
				char hex[3 * 64 + 1] = {0};
				size_t hd = (shell_input_pos < 64) ? shell_input_pos : 64;
				for (size_t hi = 0; hi < hd; hi++) {
					int off = (int)strlen(hex);
					snprintf(&hex[off], sizeof(hex) - off, "%02X ",
						 (uint8_t)shell_input_buffer[hi]);
				}
				LOG_DBG("lcd_backend: suppressed repeat char '%c' local "
					"buffer='%s' len=%zu hex=%s",
					ch, shell_input_buffer, shell_input_pos, hex);
			}
		}
	}
	/* Enqueue input-only update (MSG_TYPE_INPUT) for main loop to consume.
	 * This avoids calling display callbacks directly from the shell thread.
	 */
	{
		char msg[INPUT_MSG_SIZE];
		msg[0] = (char)MSG_TYPE_INPUT;
		size_t input_len = strlen(shell_input_buffer);
		if (input_len > SHELL_INPUT_BUFFER_SIZE - 1) {
			input_len = SHELL_INPUT_BUFFER_SIZE - 1;
		}
		memset(&msg[1], 0, INPUT_MSG_SIZE - 1);
		memcpy(&msg[1], shell_input_buffer, input_len);
		/* Overwrite latest_msg atomically (no queue flooding) */
		k_mutex_lock(&latest_msg_lock, K_FOREVER);
		memcpy(latest_msg, msg, INPUT_MSG_SIZE);
		latest_msg_valid = true;
		k_mutex_unlock(&latest_msg_lock);
		LOG_DBG("lcd_backend: latest input update len=%zu stored", input_len);
	}
}

/**
 * Try to pop an input update message from backend queue. Returns length of
 * input (0 if empty). out_buf will be NUL-terminated.
 */
int lcd_shell_try_get_input(uint8_t *out_type, char *out_buf, size_t max_len)
{
	if (out_type) {
		*out_type = 0;
	}

	if (!out_buf || max_len == 0) {
		return 0;
	}

	/* Atomically read and clear latest_msg if present */
	k_mutex_lock(&latest_msg_lock, K_NO_WAIT);

	if (!latest_msg_valid) {
		k_mutex_unlock(&latest_msg_lock);

		if (out_type) {
			*out_type = 0;
		}
		out_buf[0] = '\0';

		return 0;
	}

	/* copy out and clear valid flag */
	uint8_t type = (uint8_t)latest_msg[0];
	size_t in_len = strnlen(&latest_msg[1], INPUT_MSG_SIZE - 1);
	size_t copy = (in_len < max_len - 1) ? in_len : (max_len - 1);

	if (copy > 0) {
		memcpy(out_buf, &latest_msg[1], copy);
	}

	out_buf[copy] = '\0';
	latest_msg_valid = false;

	k_mutex_unlock(&latest_msg_lock);

	if (out_type) {
		*out_type = type;
	}

	return (int)copy;
}

void lcd_shell_set_output_callback(lcd_shell_output_cb_t callback)
{
	output_callback = callback;
	LOG_DBG("Shell output callback set");
}
