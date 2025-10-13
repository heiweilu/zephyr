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

LOG_MODULE_REGISTER(lcd_shell_backend, LOG_LEVEL_INF);

static lcd_shell_output_cb_t output_callback = NULL;

/* Shell input buffer */
#define SHELL_INPUT_BUFFER_SIZE 256
static char shell_input_buffer[SHELL_INPUT_BUFFER_SIZE];
static size_t shell_input_pos = 0;

/* Shell output function */
static int shell_lcd_write(const struct shell_transport *transport,
                          const void *data, size_t length, size_t *cnt)
{
    ARG_UNUSED(transport);
    
    if (output_callback && data && length > 0) {
        output_callback((const char *)data, length);
        LOG_INF("LCD backend captured %zu bytes", length);
    }
    
    *cnt = length;
    return 0;
}

/* Shell read function (not used for output-only backend) */
static int shell_lcd_read(const struct shell_transport *transport,
                         void *data, size_t length, size_t *cnt)
{
    ARG_UNUSED(transport);
    ARG_UNUSED(data);
    ARG_UNUSED(length);
    
    *cnt = 0;
    return 0;
}

/* Shell initialization function */
static int shell_lcd_init(const struct shell_transport *transport,
                         const void *config,
                         shell_transport_handler_t evt_handler,
                         void *context)
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
static struct shell_transport shell_lcd_transport = {
    .api = &shell_lcd_transport_api,
    .ctx = NULL
};

/* Shell instance */
SHELL_DEFINE(lcd_shell, "esp32s3:~$ ", &shell_lcd_transport, 10,
             SHELL_FLAG_OLF_CRLF, 1024);

int lcd_shell_backend_init(void)
{
    shell_input_pos = 0;
    memset(shell_input_buffer, 0, sizeof(shell_input_buffer));
    
    LOG_INF("LCD Shell backend initialized");
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
    
    for (size_t i = 0; i < len; i++) {
        char ch = input[i];
        
        if (ch == '\b' || ch == 127) { /* Backspace */
            if (shell_input_pos > 0) {
                shell_input_pos--;
                shell_input_buffer[shell_input_pos] = '\0';
                
                /* Echo backspace to display */
                if (output_callback) {
                    output_callback("\b \b", 3);
                }
            }
        } else if (ch == '\n' || ch == '\r') { /* Enter */
            shell_input_buffer[shell_input_pos] = '\0';
            
            /* Echo newline */
            if (output_callback) {
                output_callback("\n", 1);
            }
            
            /* Execute command */
            if (shell_input_pos > 0) {
                shell_execute_cmd(&lcd_shell, shell_input_buffer);
            } else {
                /* Empty command, just show prompt */
                if (output_callback) {
                    output_callback("esp32s3:~$ ", 11);
                }
            }
            
            /* Clear input buffer */
            shell_input_pos = 0;
            memset(shell_input_buffer, 0, sizeof(shell_input_buffer));
            
        } else if (ch >= 32 && ch <= 126) { /* Printable characters */
            if (shell_input_pos < sizeof(shell_input_buffer) - 1) {
                shell_input_buffer[shell_input_pos++] = ch;
                
                /* Echo character */
                if (output_callback) {
                    output_callback(&ch, 1);
                }
            }
        }
    }
}

void lcd_shell_set_output_callback(lcd_shell_output_cb_t callback)
{
    output_callback = callback;
    LOG_INF("Shell output callback set");
}