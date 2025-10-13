/*
 * LCD Shell Backend - Real Interactive Shell
 * Copyright (c) 2025 Heiweilu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LCD_SHELL_BACKEND_H
#define LCD_SHELL_BACKEND_H

#include <zephyr/shell/shell.h>

/* Initialize the LCD shell backend */
int lcd_shell_backend_init(void);

/* Get reference to the LCD shell instance */
const struct shell *lcd_shell_get_instance(void);

/* Send input to shell */
void lcd_shell_send_input(const char *input, size_t len);

/* Display output callback type */
typedef void (*lcd_shell_output_cb_t)(const char *data, size_t len);

/* Set output callback */
void lcd_shell_set_output_callback(lcd_shell_output_cb_t callback);

#endif /* LCD_SHELL_BACKEND_H */