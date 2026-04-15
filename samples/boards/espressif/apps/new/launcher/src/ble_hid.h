/*
 * BLE HID Host — shared state header
 * Provides mouse/keyboard state for LVGL input integration.
 * (Reused from lvgl_input module, screen dimensions moved to resource.h)
 */

#ifndef BLE_HID_H
#define BLE_HID_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include "resource.h"

/* ── Mouse shared state (written by BT thread, read by LVGL) ── */

struct mouse_input_state {
	int16_t x;
	int16_t y;
	uint16_t buttons;
	int8_t wheel;
	bool connected;
	char name[32];
};

extern struct mouse_input_state g_mouse;

/* ── Keyboard event queue (BT thread → LVGL thread) ── */

struct kb_event {
	uint32_t key;       /* LVGL key code or ASCII char */
	uint8_t pressed;    /* 1 = pressed, 0 = released */
};

#define KB_EVENT_QUEUE_SIZE 16

extern struct k_msgq kb_events;
extern volatile bool g_kb_connected;

int ble_hid_init(void);

#endif
