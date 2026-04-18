/*
 * Linux-style dmesg boot splash.
 * - Black background, green monospace text
 * - Fake plausible kernel log lines with rising timestamps and [ OK ]/[FAIL]/[WARN] tags
 * - Lines are appended every ~LINE_PERIOD_MS via lv_timer_handler loop
 * - Returns after all lines are printed + a short hold
 */

#include "boot_splash.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/display.h>
#include <stdio.h>
#include <string.h>
#include <lvgl.h>

#define LINE_PERIOD_MS  60      /* delay between lines */
#define HOLD_MS         400     /* extra dwell after last line */

/* Color tag helpers -- LVGL textarea supports recolor via lv_label set, but
 * for textarea we keep monochrome green and embed [ OK ] etc as plain text.
 * Keep it simple: one fg color for everything.
 */

struct boot_line {
	const char *text;       /* "%t" placeholder is replaced with timestamp */
	uint32_t    bump_ms;    /* fake elapsed-ms increment for this line */
};

static const struct boot_line k_lines[] = {
	{ "[%t] Booting CHD-ESP32-S3-BOX (Zephyr RTOS)",                    0   },
	{ "[%t] CPU0: Xtensa LX7 @ 240 MHz",                                15  },
	{ "[%t] CPU1: Xtensa LX7 @ 240 MHz",                                3   },
	{ "[%t] Memory: 320 KiB SRAM + 8 MiB PSRAM (octal, 80MHz)",         12  },
	{ "[%t] Flash: 16 MiB QIO @ 80MHz                          [ OK ]", 22  },
	{ "[%t] PSRAM probe...                                     [ OK ]", 41  },
	{ "[%t] mmu: mapped EXT_RAM .data + heap                   [ OK ]", 8   },
	{ "[%t] gpio@60004000: 22 pins ready                       [ OK ]", 31  },
	{ "[%t] i2c@60013000: bus0 @ 400kHz                        [ OK ]", 14  },
	{ "[%t] spi3: octal-PSRAM controller bound                 [ OK ]", 9   },
	{ "[%t] lcd-cam: ST7789V 240x320, RGB565                   [ OK ]", 27  },
	{ "[%t] backlight: PWM IO47 -> 100%                        [ OK ]", 11  },
	{ "[%t] fs/littlefs: mounting /lfs ...                     [ OK ]", 19  },
	{ "[%t] codec: ES7210 ADC @ 16kHz mono                     [ OK ]", 17  },
	{ "[%t] codec: ES8311 DAC @ 44.1kHz stereo                 [ OK ]", 12  },
	{ "[%t] i2s0: bound to ES8311                              [ OK ]", 8   },
	{ "[%t] WiFi: phy_init, country=CN, channel=1..13          [ OK ]", 33  },
	{ "[%t] WiFi: held off  (LCD-CAM bus contention)           [WARN]", 7   },
	{ "[%t] Bluetooth: HCI ESP32 controller v4.2               [ OK ]", 24  },
	{ "[%t] Bluetooth: SMP/IRK loaded from settings            [ OK ]", 11  },
	{ "[%t] Bluetooth: HID-Host service registered             [ OK ]", 9   },
	{ "[%t] LVGL 9.x: tick=10ms, double-buffer DMA             [ OK ]", 18  },
	{ "[%t] systemd-launcher[1]: registering 8 apps...         [ OK ]", 26  },
	{ "[%t]   * AI / Camera / NES / Photos / Term / Music ...  [ OK ]", 5   },
	{ "[%t]   * Face / IMU                                     [ OK ]", 4   },
	{ "[%t] launcher_ui: building icon grid                    [ OK ]", 13  },
	{ "[%t] reached target: graphical.target                   [ OK ]", 21  },
	{ "[%t] welcome to CHD-OS  (tty1 ready)",                            7  },
};

#define LINE_COUNT (sizeof(k_lines) / sizeof(k_lines[0]))

static void format_line(const struct boot_line *l, uint32_t ts_us, char *out, size_t outsz)
{
	char ts[16];
	uint32_t sec = ts_us / 1000000U;
	uint32_t us  = ts_us % 1000000U;
	snprintf(ts, sizeof(ts), "%4u.%06u", sec, us);

	/* Replace first occurrence of "%t" with the timestamp. */
	const char *p = strstr(l->text, "%t");
	if (!p) {
		snprintf(out, outsz, "%s", l->text);
		return;
	}
	size_t prefix = (size_t)(p - l->text);
	if (prefix >= outsz) {
		prefix = outsz - 1;
	}
	memcpy(out, l->text, prefix);
	out[prefix] = '\0';
	size_t left = outsz - prefix;
	snprintf(out + prefix, left, "%s%s", ts, p + 2);
}

void boot_splash_run(const struct device *display)
{
	(void)display;

	/* Fullscreen black canvas */
	lv_obj_t *scr = lv_obj_create(NULL);
	lv_obj_remove_style_all(scr);
	lv_obj_set_size(scr, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_screen_load(scr);

	/* Log textarea: full-screen, black bg, green text, no scrollbar visual */
	lv_obj_t *log = lv_textarea_create(scr);
	lv_obj_remove_style_all(log);
	lv_obj_set_size(log, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_bg_color(log, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(log, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(log, lv_color_hex(0x00FF00), 0);
	lv_obj_set_style_text_font(log, &lv_font_montserrat_14, 0);
	lv_obj_set_style_pad_all(log, 4, 0);
	lv_obj_set_style_border_width(log, 0, 0);
	lv_textarea_set_one_line(log, false);
	lv_textarea_set_cursor_click_pos(log, false);
	lv_obj_clear_flag(log, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_clear_flag(log, LV_OBJ_FLAG_SCROLL_ELASTIC);
	/* Hide cursor by setting opacity to 0 */
	lv_obj_set_style_opa(log, LV_OPA_TRANSP, LV_PART_CURSOR);

	/* Initial header */
	lv_textarea_add_text(log,
		"CHD-OS 1.0.0 (zephyr-4.x) on esp32s3 - tty1\n\n");

	uint32_t fake_us = 1234U;       /* start at ~0.001s */
	char line_buf[96];

	for (size_t i = 0; i < LINE_COUNT; i++) {
		fake_us += k_lines[i].bump_ms * 1000U + (k_uptime_get_32() % 900U);
		format_line(&k_lines[i], fake_us, line_buf, sizeof(line_buf));

		/* Append line + newline */
		lv_textarea_add_text(log, line_buf);
		lv_textarea_add_text(log, "\n");

		/* Auto-scroll to bottom -- textarea cursor is at end after add_text */
		lv_obj_scroll_to_y(log, lv_obj_get_scroll_bottom(log) + 1000, LV_ANIM_OFF);

		/* Flush LVGL for this line over LINE_PERIOD_MS */
		uint32_t deadline = k_uptime_get_32() + LINE_PERIOD_MS;
		while ((int32_t)(deadline - k_uptime_get_32()) > 0) {
			uint32_t s = lv_timer_handler();
			k_msleep(MIN(s, 16));
		}
	}

	/* Hold the final frame briefly */
	uint32_t deadline = k_uptime_get_32() + HOLD_MS;
	while ((int32_t)(deadline - k_uptime_get_32()) > 0) {
		uint32_t s = lv_timer_handler();
		k_msleep(MIN(s, 16));
	}

	/* Splash screen will be replaced by launcher_ui_init() loading its own
	 * screen, but delete our textarea explicitly to free LVGL memory.
	 */
	lv_obj_delete(log);
	/* Don't delete scr -- launcher_ui will load a different screen and LVGL
	 * cleans the old active screen automatically when a new one loads.
	 */
}
