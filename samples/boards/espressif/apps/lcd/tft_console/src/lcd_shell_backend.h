/*
 * LCD Shell Backend - Real Interactive Shell
 * Copyright (c) 2025 Heiweilu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LCD_SHELL_BACKEND_H
#define LCD_SHELL_BACKEND_H

#include <zephyr/shell/shell.h>
#include <stddef.h>
#include <stdint.h>

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

/* Message types delivered by backend -> main
 * MSG_TYPE_INPUT: normal input update; payload is NUL-terminated current input line
 * MSG_TYPE_ENTER: Enter pressed; payload is ignored. Main should clear the last
 *                 printed command line from the display buffer.
 */
#define MSG_TYPE_INPUT 0x1F
#define MSG_TYPE_ENTER 0x1E

/* Try to get an input update from backend queue. If an update is available,
 * writes its type to *out_type (MSG_TYPE_*) and copies optional payload into
 * out_buf (NUL-terminated). Returns length of payload copied (0 if none), or
 * 0 if no message available.
 */
int lcd_shell_try_get_input(uint8_t *out_type, char *out_buf, size_t max_len);

#endif /* LCD_SHELL_BACKEND_H */