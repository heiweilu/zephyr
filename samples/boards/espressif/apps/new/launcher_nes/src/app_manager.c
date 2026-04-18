/*
 * app_manager.c — Application lifecycle manager
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <lvgl.h>
#include "app_manager.h"

static app_info_t apps[APP_MAX_COUNT];
static int app_count;
static int current_app = -1;  /* -1 = home screen */
static lv_obj_t *home_screen;
static lv_obj_t *app_screen;
static lv_group_t *kb_group;

int app_manager_register(const app_info_t *app)
{
	if (app_count >= APP_MAX_COUNT) {
		printk("[AppMgr] Max apps reached\n");
		return -1;
	}
	apps[app_count] = *app;
	printk("[AppMgr] Registered: %s (id=%d)\n", app->name, app_count);
	app_count++;
	return app_count - 1;
}

int app_manager_get_count(void)
{
	return app_count;
}

const app_info_t *app_manager_get(int id)
{
	if (id < 0 || id >= app_count) {
		return NULL;
	}
	return &apps[id];
}

void app_manager_launch(int app_id)
{
	if (app_id < 0 || app_id >= app_count) {
		return;
	}

	/* If another app is active, destroy it first */
	if (current_app >= 0 && apps[current_app].on_destroy) {
		apps[current_app].on_destroy();
	}
	if (app_screen) {
		lv_obj_delete(app_screen);
		app_screen = NULL;
	}

	/* Create new screen for the app */
	app_screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(app_screen, lv_color_hex(0x1a1a2e), 0);
	lv_obj_set_style_bg_opa(app_screen, LV_OPA_COVER, 0);

	current_app = app_id;
	printk("[AppMgr] Launching: %s\n", apps[app_id].name);

	/* Clear keyboard group and let app populate it */
	if (kb_group) {
		lv_group_remove_all_objs(kb_group);
	}

	/* Let app build its UI */
	if (apps[app_id].on_create) {
		apps[app_id].on_create(app_screen);
	}

	/* Switch to app screen */
	lv_screen_load(app_screen);
}

void app_manager_back_to_home(void)
{
	if (current_app < 0) {
		return;  /* Already home */
	}

	printk("[AppMgr] Back to home (from: %s)\n", apps[current_app].name);

	/* Destroy current app */
	if (apps[current_app].on_destroy) {
		apps[current_app].on_destroy();
	}
	current_app = -1;

	if (app_screen) {
		lv_obj_delete(app_screen);
		app_screen = NULL;
	}

	/* Clear keyboard group — launcher_ui will repopulate */
	if (kb_group) {
		lv_group_remove_all_objs(kb_group);
	}

	/* Switch back to home */
	if (home_screen) {
		lv_screen_load(home_screen);
	}
}

bool app_manager_is_app_active(void)
{
	return current_app >= 0;
}

void app_manager_set_home_screen(lv_obj_t *home)
{
	home_screen = home;
}

void app_manager_set_kb_group(lv_group_t *group)
{
	kb_group = group;
}

lv_group_t *app_manager_get_kb_group(void)
{
	return kb_group;
}
