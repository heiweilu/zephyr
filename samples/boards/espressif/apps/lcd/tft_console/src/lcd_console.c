/*
 * LCD Console Redirect Module Implementation
 * Copyright (c) 2025 Heiweilu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lcd_console.h"
#include "lvgl_wrapper.h"
#include <zephyr/shell/shell.h>
#include <zephyr/console/console.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdio.h>

LV_FONT_DECLARE(lv_font_unscii_8)

LOG_MODULE_REGISTER(lcd_console, LOG_LEVEL_INF);

static lcd_console_t g_lcd_console = {0};
static bool console_initialized = false;

/* Original console output function pointer */
/* Save original console output function to restore if needed */
/* Note: uart_console_out_debug_hook_t is a function type, not pointer type */

/**
 * @brief Custom console output hook - redirects output to LCD
 */
int lcd_console_output_hook(char c)
{
    /* Buffer the character for LCD output */
    static char line_buffer[256];
    static int line_pos = 0;
    
    if (console_initialized && g_lcd_console.console_enabled) {
        if (c == '\n' || c == '\r') {
            /* End of line - add to console */
            if (line_pos > 0) {
                line_buffer[line_pos] = '\0';
                lcd_console_write(line_buffer, line_pos);
                line_pos = 0;
            }
            lcd_console_write("\n", 1);
        } else if (c >= 32 && c <= 126) {  /* Printable characters */
            if (line_pos < sizeof(line_buffer) - 1) {
                line_buffer[line_pos++] = c;
            }
        }
    }
    
    /* Return UART_CONSOLE_DEBUG_HOOK_HANDLED to prevent original output (LCD only) */
    /* Return 0 to allow dual output (both LCD and serial) */
    return UART_CONSOLE_DEBUG_HOOK_HANDLED;  /* Redirect only to LCD */
}

/**
 * @brief Install early UART console debug hook for console redirection
 * Call this as early as possible to capture all output
 */
void lcd_console_install_hook(void)
{
    /* Install our custom UART console debug hook */
    uart_console_out_debug_hook_install(&lcd_console_output_hook);
    
    /* Initial test message */
    printk("LCD Console UART hook installed - checking if redirection works\n");
}

/**
 * @brief Initialize LCD console system
 */
int lcd_console_init(lv_obj_t *parent)
{
    if (console_initialized) {
        return 0;
    }

    memset(&g_lcd_console, 0, sizeof(g_lcd_console));
    
    /* Initialize mutex */
    k_mutex_init(&g_lcd_console.buffer_mutex);
    
    /* Create shell container */
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

    /* Create console area (top area for output) */
    g_lcd_console.console_area = create_container(g_lcd_console.shell_container, 
                                                    230, 
                                                    100, 
                                                    2, 2, 
                                                    0, 
                                                    1, 
                                                    lv_color_make(0x40, 0x40, 0x40), 
                                                    0,
                                                    lv_color_black(), 
                                                    0);

    /* Create console text label */
    g_lcd_console.console_label = create_label(g_lcd_console.console_area,
                                            "LCD Console Ready...\n",
                                            -1,
                                            -1,
                                            200,
                                            lv_color_make(0xFF, 0xFF, 0xFF),
                                            &lv_font_unscii_8,
                                            false
                                            );

    /* Create input area (bottom area for command input) */
    g_lcd_console.input_area = create_textarea(g_lcd_console.shell_container,
                                               210, 30, 5, 103,
                                               lv_color_make(0x20, 0x20, 0x20),
                                               lv_color_white(),
                                               &lv_font_unscii_8,
                                               "> Enter command...",
                                               true);
    
    /* Initialize console buffer */
    strcpy(g_lcd_console.console_buffer, "ESP32-S3 LCD Console\nShell Ready!\n");
    g_lcd_console.current_line = 2;
    g_lcd_console.total_lines = 2;
    g_lcd_console.console_enabled = true;
    g_lcd_console.shell_enabled = true;
    
    console_initialized = true;
    
    LOG_INF("LCD Console initialized successfully");
    
    return 0;
}

/**
 * @brief Enable/disable console output to LCD
 */
void lcd_console_enable(bool enable)
{
    g_lcd_console.console_enabled = enable;
    if (enable) {
        lv_obj_clear_flag(g_lcd_console.shell_container, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_lcd_console.shell_container, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Enable/disable shell interface
 */
void lcd_shell_enable(bool enable)
{
    g_lcd_console.shell_enabled = enable;
    if (enable) {
        lv_obj_clear_flag(g_lcd_console.input_area, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_lcd_console.input_area, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Add text to console buffer and update display
 */
void lcd_console_write(const char *text, size_t length)
{
    if (!console_initialized || !g_lcd_console.console_enabled) {
        return;
    }
    
    k_mutex_lock(&g_lcd_console.buffer_mutex, K_FOREVER);
    
    /* Add text to buffer */
    size_t current_len = strlen(g_lcd_console.console_buffer);
    size_t available = LCD_CONSOLE_BUFFER_SIZE - current_len - 1;
    
    if (length > available) {
        /* Buffer is full, shift content up */
        char *newline = strchr(g_lcd_console.console_buffer, '\n');
        if (newline) {
            size_t shift_len = strlen(newline + 1);
            memmove(g_lcd_console.console_buffer, newline + 1, shift_len + 1);
            current_len = shift_len;
            g_lcd_console.current_line--;
        }
    }
    
    /* Append new text */
    size_t copy_len = MIN(length, LCD_CONSOLE_BUFFER_SIZE - current_len - 1);
    strncat(g_lcd_console.console_buffer, text, copy_len);
    
    /* Count lines */
    for (size_t i = 0; i < copy_len; i++) {
        if (text[i] == '\n') {
            g_lcd_console.current_line++;
        }
    }
    
    /* Update display */
    lv_label_set_text(g_lcd_console.console_label, g_lcd_console.console_buffer);
    
    /* Auto-scroll to bottom */
    lv_obj_scroll_to_y(g_lcd_console.console_area, LV_COORD_MAX, LV_ANIM_ON);
    
    k_mutex_unlock(&g_lcd_console.buffer_mutex);
}

/**
 * @brief Clear console buffer and display
 */
void lcd_console_clear(void)
{
    if (!console_initialized) {
        return;
    }
    
    k_mutex_lock(&g_lcd_console.buffer_mutex, K_FOREVER);
    
    g_lcd_console.console_buffer[0] = '\0';
    g_lcd_console.current_line = 0;
    g_lcd_console.total_lines = 0;
    
    lv_label_set_text(g_lcd_console.console_label, "");
    
    k_mutex_unlock(&g_lcd_console.buffer_mutex);
}

/**
 * @brief Process shell command input (simplified)
 */
void lcd_shell_process_command(const char *cmd)
{
    if (!console_initialized || !g_lcd_console.shell_enabled) {
        return;
    }
    
    char response[256];
    
    /* Add command to console */
    snprintf(response, sizeof(response), "esp32s3:~$ %s\n", cmd);
    lcd_console_write(response, strlen(response));
    
    /* Process simple commands */
    if (strcmp(cmd, "help") == 0) {
        lcd_console_write("Available commands:\n", 20);
        lcd_console_write("  help    - Show this help\n", 27);
        lcd_console_write("  clear   - Clear console\n", 26);
        lcd_console_write("  version - Show version\n", 25);
        lcd_console_write("  uptime  - Show uptime\n", 24);
    } else if (strcmp(cmd, "clear") == 0) {
        lcd_console_clear();
        lcd_console_write("Console cleared.\n", 17);
    } else if (strcmp(cmd, "version") == 0) {
        lcd_console_write("ESP32-S3 LCD Console v1.0\n", 27);
        lcd_console_write("Zephyr RTOS + LVGL\n", 20);
    } else if (strcmp(cmd, "uptime") == 0) {
        uint32_t uptime = k_uptime_get_32() / 1000;
        snprintf(response, sizeof(response), "Uptime: %u seconds\n", uptime);
        lcd_console_write(response, strlen(response));
    } else if (strlen(cmd) > 0) {
        snprintf(response, sizeof(response), "Unknown command: %s\n", cmd);
        lcd_console_write(response, strlen(response));
    }
}

/**
 * @brief Update console display
 */
void lcd_console_update_display(void)
{
    if (!console_initialized) {
        return;
    }
    
    /* Update timestamp or other dynamic content */
    static uint32_t last_update = 0;
    uint32_t now = k_uptime_get_32();
    
    if (now - last_update > 10000) {  /* Update every 10 seconds */
        char time_str[64];
        uint32_t seconds = now / 1000;
        uint32_t minutes = seconds / 60;
        uint32_t hours = minutes / 60;
        
        snprintf(time_str, sizeof(time_str), "[%02u:%02u:%02u] System OK\n", 
                (unsigned)(hours % 24), (unsigned)(minutes % 60), (unsigned)(seconds % 60));
        lcd_console_write(time_str, strlen(time_str));
        
        last_update = now;
    }
}

/**
 * @brief Get console instance
 */
lcd_console_t *lcd_console_get_instance(void)
{
    if (console_initialized) {
        return &g_lcd_console;
    }
    return NULL;
}