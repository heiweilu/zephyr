/*
 * launcher_full main.c — phase 2b/2c skeleton
 *
 * Boot flow:
 *   1) display init + LVGL ready
 *   2) backlight on
 *   3) home_ui_init()
 *   4) BOOT key short-press → home_ui_focus_next()
 *      BOOT key double-press → home_ui_activate_selected()
 *   5) lv_task_handler loop
 *
 * Camera / AI / TTS code is parked in app_ai_vision.c (not yet compiled).
 * It will be re-enabled in 2d/2e once HOME UI + BOOT navigation are validated.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <lvgl.h>

#include "home_ui.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* ── Backlight (IO47 = gpio1 pin 15) ─────────────────────────────── */
#define BLK_NODE  DT_NODELABEL(gpio1)
#define BLK_PIN   15

/* ── BOOT button: short press = focus next, double press = activate ─ */
#define BOOT_NODE DT_ALIAS(sw0)
static const struct gpio_dt_spec boot_btn = GPIO_DT_SPEC_GET(BOOT_NODE, gpios);

#define DOUBLE_PRESS_WINDOW_MS  400  /* gap between two presses to count as double */
#define DEBOUNCE_MS              30

static int64_t last_press_ms;
static int64_t prev_release_ms;
static bool    waiting_for_double;

/* Returns 1 = short, 2 = double. Polled in main loop. */
static int read_button_event(void)
{
	static bool pressed_prev;
	bool pressed = (gpio_pin_get_dt(&boot_btn) == 1);
	int64_t now  = k_uptime_get();
	int evt = 0;

	if (pressed && !pressed_prev) {
		/* falling edge */
		if (now - last_press_ms > DEBOUNCE_MS) {
			last_press_ms = now;
		}
	} else if (!pressed && pressed_prev) {
		/* rising edge — released */
		if (now - last_press_ms < DEBOUNCE_MS) {
			/* glitch */
		} else if (waiting_for_double &&
			   (now - prev_release_ms) <= DOUBLE_PRESS_WINDOW_MS) {
			evt = 2;            /* double */
			waiting_for_double = false;
		} else {
			waiting_for_double = true;
			prev_release_ms = now;
		}
	} else if (!pressed && waiting_for_double &&
		   (now - prev_release_ms) > DOUBLE_PRESS_WINDOW_MS) {
		evt = 1;                     /* timed-out single */
		waiting_for_double = false;
	}

	pressed_prev = pressed;
	return evt;
}

int main(void)
{
	LOG_INF("==== launcher_full boot (phase 2b/2c) ====");

	/* Display */
	const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display_dev)) {
		LOG_ERR("display not ready");
		return -1;
	}

	/* Backlight on */
	const struct device *gpio1_dev = DEVICE_DT_GET(BLK_NODE);
	if (device_is_ready(gpio1_dev)) {
		gpio_pin_configure(gpio1_dev, BLK_PIN, GPIO_OUTPUT_ACTIVE);
		gpio_pin_set(gpio1_dev, BLK_PIN, 1);
	}

	/* BOOT button */
	if (!gpio_is_ready_dt(&boot_btn) ||
	    gpio_pin_configure_dt(&boot_btn, GPIO_INPUT) < 0) {
		LOG_WRN("BOOT button not available — UI navigation disabled");
	}

	/* HOME UI */
	home_ui_init();
	display_blanking_off(display_dev);

	LOG_INF("ready — BOOT short=focus next, double=activate");

	while (1) {
		int evt = read_button_event();
		if (evt == 1) {
			home_ui_focus_next();
		} else if (evt == 2) {
			home_ui_activate_selected();
		}
		lv_task_handler();
		k_msleep(20);
	}
	return 0;
}
