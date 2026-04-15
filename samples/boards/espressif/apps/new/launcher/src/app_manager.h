/*
 * app_manager.h — Application lifecycle manager
 *
 * Each "app" is a set of callbacks that create/destroy an LVGL screen.
 * The launcher desktop is the home screen; apps are launched by switching
 * to their screen and destroyed when returning home.
 */

#ifndef APP_MANAGER_H
#define APP_MANAGER_H

#include <lvgl.h>
#include <stdbool.h>

#define APP_MAX_COUNT 12

typedef struct app_info {
	const char *name;            /* Display name under icon */
	lv_color_t  icon_color;      /* Icon background color */
	const char *icon_symbol;     /* Symbol/emoji text on icon */
	void (*on_create)(lv_obj_t *screen);  /* Build app UI on this screen */
	void (*on_destroy)(void);             /* Cleanup resources */
} app_info_t;

/* Register an app (call before launcher_ui_init) */
int  app_manager_register(const app_info_t *app);

/* Get registered app count and info */
int  app_manager_get_count(void);
const app_info_t *app_manager_get(int id);

/* Launch app by id — creates a new screen, calls on_create */
void app_manager_launch(int app_id);

/* Return to home screen — calls on_destroy of current app */
void app_manager_back_to_home(void);

/* Check if an app is currently active */
bool app_manager_is_app_active(void);

/* Set/get the home screen object (set by launcher_ui) */
void app_manager_set_home_screen(lv_obj_t *home);

/* Set the LVGL keyboard input group (for focus management) */
void app_manager_set_kb_group(lv_group_t *group);
lv_group_t *app_manager_get_kb_group(void);

#endif /* APP_MANAGER_H */
