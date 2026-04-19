/*
 * app_nes.c — NES Emulator launcher entry
 *
 * Tapping the NES icon shows a 1-second "Starting NES..." splash, then
 * calls nofrendo_main() which TAKES OVER the display and NEVER returns.
 * Returning to HOME requires a hardware reset.
 */

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include "../app_manager.h"

extern const lv_font_t lv_font_source_han_sans_sc_16_cjk;

/* nofrendo entry point (samples/.../nes_emulator/components/nofrendo) */
extern int nofrendo_main(int argc, char **argv);

static lv_timer_t *s_launch_timer;

static void launch_nofrendo_cb(lv_timer_t *t)
{
	lv_timer_delete(t);
	s_launch_timer = NULL;

	printk("[NES] launch_nofrendo_cb fired, calling nofrendo_main()\n");
	int rc = nofrendo_main(0, NULL);
	printk("[NES] nofrendo_main RETURNED rc=%d, rebooting to recover clean state\n", rc);

	/* nofrendo upstream leaks ~80 KB / session into the PSRAM heap and
	 * eventually corrupts sys_heap bookkeeping, which breaks any
	 * subsequent in-place nofrendo_main() call. A cold reboot is the
	 * only reliable recovery; the launcher itself comes back in ~1 s. */
	k_msleep(50);          /* let printk drain */
	sys_reboot(SYS_REBOOT_COLD);
}

static void on_create(lv_obj_t *screen)
{
	printk("[NES] on_create called\n");
	lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

	lv_obj_t *title = lv_label_create(screen);
	lv_label_set_text(title, "NES Emulator");
	lv_obj_set_style_text_color(title, lv_color_white(), 0);
	lv_obj_set_style_text_font(title, &lv_font_source_han_sans_sc_16_cjk, 0);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

	lv_obj_t *desc = lv_label_create(screen);
	lv_label_set_text(desc, "Starting nofrendo...\nROM: sbp.nes\nReset to exit");
	lv_obj_set_style_text_color(desc, lv_color_hex(0xFFCC00), 0);
	lv_obj_set_style_text_font(desc, &lv_font_source_han_sans_sc_16_cjk, 0);
	lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(desc, LV_ALIGN_CENTER, 0, 0);

	/* Give LVGL one full frame to render the splash, then launch.
	 * 200ms is plenty for the user to see the message before nofrendo
	 * blocks the LVGL thread permanently. */
	s_launch_timer = lv_timer_create(launch_nofrendo_cb, 200, NULL);
	lv_timer_set_repeat_count(s_launch_timer, 1);
}

static void on_destroy(void)
{
	printk("[NES] on_destroy called (s_launch_timer=%p)\n", (void *)s_launch_timer);
	/* on_destroy is unreachable once nofrendo_main has been entered, but
	 * keep it correct for the (theoretical) Back-before-launch case. */
	if (s_launch_timer) {
		lv_timer_delete(s_launch_timer);
		s_launch_timer = NULL;
	}
}

const app_info_t app_nes = {
	.name = "NES",
	.icon_color = LV_COLOR_MAKE(0xEF, 0x44, 0x44),
	.icon_symbol = LV_SYMBOL_PLAY,
	.on_create = on_create,
	.on_destroy = on_destroy,
};
