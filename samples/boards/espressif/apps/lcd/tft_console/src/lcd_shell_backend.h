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

/**
 * Try to pop an input update message from backend queue. Returns length of
 * input (0 if empty). out_buf will be NUL-terminated.
 * @param out_type Pointer to uint8_t to receive message type (MSG_TYPE_INPUT or
 * MSG_TYPE_ENTER). Can be NULL if type not needed.
 * @param out_buf Pointer to buffer to receive input string. Must be at least
 * SHELL_INPUT_BUFFER_SIZE bytes.
 * @param max_len Maximum length of out_buf (including NUL). Must be >
 * 0.
 * @return Length of input string (excluding NUL), or 0 if no message available
 */
int lcd_shell_try_get_input(uint8_t *out_type, char *out_buf, size_t max_len);

#endif /* LCD_SHELL_BACKEND_H */