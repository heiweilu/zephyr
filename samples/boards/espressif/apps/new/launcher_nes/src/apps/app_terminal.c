/*
 * app_terminal.c — Interactive shell using BLE HID keyboard input.
 *
 * UI: full-screen black-on-green textarea, single buffer (input and output
 * mixed). Press Enter to execute the line under the prompt.
 *
 * Input: consumes the global ``kb_events`` msgq populated by ble_hid.c.
 * A 50 ms LVGL timer drains the queue and updates the textarea.
 *
 * Built-in commands: help, clear, echo <text>, version, mem, ps, ble,
 * exit/home.
 */

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/version.h>
#include <stdio.h>
#include <string.h>
#include "../app_manager.h"
#include "../ble_hid.h"

#define CMD_BUF_SZ   96
#define POLL_MS      15

static lv_obj_t *ta;
static lv_timer_t *kb_timer;
static char cmd_buf[CMD_BUF_SZ];
static size_t cmd_len;

static void term_print(const char *s)
{
	if (ta && s) {
		lv_textarea_add_text(ta, s);
	}
}

static void term_prompt(void)
{
	term_print("\n$ ");
	cmd_len = 0;
}

static void cmd_help(void)
{
	term_print("\nhelp     show this list");
	term_print("\nclear    clear screen");
	term_print("\necho ... print arguments");
	term_print("\nversion  kernel version");
	term_print("\nmem      heap stats (printk)");
	term_print("\nps       thread list (printk)");
	term_print("\nble      HID connection state");
	term_print("\nexit     back to launcher");
}

static void cmd_clear(void)
{
	if (ta) {
		lv_textarea_set_text(ta, "");
	}
}

static void cmd_version(void)
{
	char line[64];
	snprintf(line, sizeof(line), "\nZephyr %d.%d.%d",
		 SYS_KERNEL_VER_MAJOR(KERNELVERSION),
		 SYS_KERNEL_VER_MINOR(KERNELVERSION),
		 SYS_KERNEL_VER_PATCHLEVEL(KERNELVERSION));
	term_print(line);
}

static void thread_cb(const struct k_thread *t, void *u)
{
	int *count = u;
	(*count)++;
	const char *name = k_thread_name_get((struct k_thread *)t);
	printk("  [%2d] %-16s prio=%d state=0x%02x\n",
	       *count, name ? name : "?", t->base.prio, t->base.thread_state);
}

static void cmd_ps(void)
{
	int count = 0;
	printk("\n--- threads ---\n");
	k_thread_foreach(thread_cb, &count);
	char line[48];
	snprintf(line, sizeof(line), "\n%d threads (UART log)", count);
	term_print(line);
}

static void cmd_mem(void)
{
	printk("\n--- mem (k_thread stack stats via UART) ---\n");
	term_print("\nmem stats printed to UART");
}

static void cmd_ble(void)
{
	char line[48];
	snprintf(line, sizeof(line), "\nKB connected: %s",
		 g_kb_connected ? "yes" : "no");
	term_print(line);
}

static void cmd_echo(const char *args)
{
	term_print("\n");
	term_print(args);
}

static void execute(void)
{
	cmd_buf[cmd_len] = '\0';
	char *p = cmd_buf;
	while (*p == ' ') {
		p++;
	}
	if (*p == '\0') {
		/* empty */
	} else if (strcmp(p, "help") == 0) {
		cmd_help();
	} else if (strcmp(p, "clear") == 0) {
		cmd_clear();
	} else if (strcmp(p, "version") == 0) {
		cmd_version();
	} else if (strcmp(p, "mem") == 0) {
		cmd_mem();
	} else if (strcmp(p, "ps") == 0) {
		cmd_ps();
	} else if (strcmp(p, "ble") == 0) {
		cmd_ble();
	} else if (strcmp(p, "exit") == 0 || strcmp(p, "home") == 0) {
		app_manager_back_to_home();
		return;
	} else if (strncmp(p, "echo ", 5) == 0) {
		cmd_echo(p + 5);
	} else if (strcmp(p, "echo") == 0) {
		term_print("\n");
	} else {
		char line[CMD_BUF_SZ + 16];
		snprintf(line, sizeof(line), "\nunknown: %s", p);
		term_print(line);
	}
	term_prompt();
}

static void kb_timer_cb(lv_timer_t *t)
{
	ARG_UNUSED(t);
	struct kb_event evt;
	while (k_msgq_get(&kb_events, &evt, K_NO_WAIT) == 0) {
		if (!evt.pressed) {
			continue;
		}
		uint32_t k = evt.key;
		if (k == LV_KEY_ENTER || k == '\n' || k == '\r') {
			execute();
		} else if (k == LV_KEY_BACKSPACE || k == '\b' || k == 0x7F) {
			if (cmd_len > 0) {
				cmd_len--;
				if (ta) {
					lv_textarea_delete_char(ta);
				}
			}
		} else if (k >= 0x20 && k < 0x7F &&
			   cmd_len < sizeof(cmd_buf) - 1) {
			cmd_buf[cmd_len++] = (char)k;
			char s[2] = {(char)k, '\0'};
			term_print(s);
		}
	}
}

static void back_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	app_manager_back_to_home();
}

static void on_create(lv_obj_t *screen)
{
	lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

	ta = lv_textarea_create(screen);
	lv_obj_set_size(ta, 320, 220);
	lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_textarea_set_one_line(ta, false);
	lv_obj_set_style_bg_color(ta, lv_color_black(), 0);
	lv_obj_set_style_text_color(ta, lv_color_hex(0x00FF00), 0);
	lv_obj_set_style_border_width(ta, 0, 0);
	lv_obj_set_style_radius(ta, 0, 0);
	lv_obj_set_style_pad_all(ta, 4, 0);
	lv_textarea_set_text(ta, "");
	term_print("Terminal v1 - type 'help'");
	term_prompt();

	lv_obj_t *btn = lv_button_create(screen);
	lv_obj_set_size(btn, 80, 20);
	lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
	lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), 0);
	lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *lbl = lv_label_create(btn);
	lv_label_set_text(lbl, "Home");
	lv_obj_set_style_text_color(lbl, lv_color_hex(0x00FF00), 0);
	lv_obj_center(lbl);

	/* NOTE: deliberately NOT adding `btn` to the LVGL keyboard group.
	 * If we did, the keyboard ENTER key (used to execute commands)
	 * would also activate the focused Home button and bounce us back
	 * to the launcher. Touch/mouse can still click the button.
	 */

	cmd_len = 0;
	if (!kb_timer) {
		kb_timer = lv_timer_create(kb_timer_cb, POLL_MS, NULL);
	}
}

static void on_destroy(void)
{
	if (kb_timer) {
		lv_timer_del(kb_timer);
		kb_timer = NULL;
	}
	ta = NULL;
	cmd_len = 0;
}

const app_info_t app_terminal = {
	.name = "Term",
	.icon_color = LV_COLOR_MAKE(0x1F, 0x29, 0x37),
	.icon_symbol = ">_",
	.on_create = on_create,
	.on_destroy = on_destroy,
};
