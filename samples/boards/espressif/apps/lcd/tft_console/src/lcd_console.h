/*
 * LCD Console Redirect Module
 * Copyright (c) 2025 Heiweilu
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * This module redirects console output (printk, LOG) to LCD display
 * and provides shell interface functionality
 */

#ifndef LCD_CONSOLE_H
#define LCD_CONSOLE_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <lvgl.h>
#include <zephyr/logging/log.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LCD Console Configuration */
#define LCD_CONSOLE_BUFFER_SIZE 2048
#define LCD_CONSOLE_MAX_LINES   20
#define LCD_CONSOLE_LINE_HEIGHT 12
#define LCD_CONSOLE_FONT_SIZE   8

/* LCD Console structure */
typedef struct {
    lv_obj_t *console_area;       /* Console display area */
    lv_obj_t *console_label;      /* Console text label */
    lv_obj_t *input_area;         /* Input text area */
    lv_obj_t *shell_container;    /* Shell container */
    
    char console_buffer[LCD_CONSOLE_BUFFER_SIZE];  /* Console text buffer */
    char input_buffer[256];       /* Input buffer */
    
    int current_line;             /* Current line number */
    int total_lines;              /* Total lines in buffer */
    bool shell_enabled;           /* Shell interface enabled */
    bool console_enabled;         /* Console output enabled */
    size_t buffer_len;            /* Current length of console buffer */
    size_t input_len;             /* Current length of input buffer */
    enum {
        LCD_ANSI_IDLE = 0,
        LCD_ANSI_ESC,
        LCD_ANSI_CSI
    } ansi_state;                 /* ANSI escape state tracker */
    
    struct k_mutex buffer_mutex;  /* Buffer access mutex */
} lcd_console_t;

/* Function prototypes */

/**
 * @brief Initialize LCD console system
 * @param parent Parent object for console
 * @return 0 on success, negative error code on failure
 */
int lcd_console_init(lv_obj_t *parent);

/**
 * @brief Enable/disable console output to LCD
 * @param enable true to enable, false to disable
 */
void lcd_console_enable(bool enable);

/**
 * @brief Enable/disable shell interface
 * @param enable true to enable, false to disable
 */
void lcd_shell_enable(bool enable);

/**
 * @brief Add text to console buffer and update display
 * @param text Text to add
 * @param length Length of text
 */
void lcd_console_write(const char *text, size_t length);

/**
 * @brief Clear console buffer and display
 */
void lcd_console_clear(void);

/**
 * @brief Process shell command input
 * @param cmd Command string
 */
void lcd_shell_process_command(const char *cmd);

/**
 * @brief Update console display
 */
void lcd_console_update_display(void);

/**
 * @brief Get console instance
 * @return Pointer to console instance
 */
lcd_console_t *lcd_console_get_instance(void);

/* Custom console output hook function */
int lcd_console_output_hook(int c);

#ifdef __cplusplus
}
#endif

#endif /* LCD_CONSOLE_H */