/*
 * launcher_ui.h — Desktop home screen UI
 */

#ifndef LAUNCHER_UI_H
#define LAUNCHER_UI_H

#include <lvgl.h>

/* Initialize the launcher home screen with icon grid + status bar.
 * Must be called after app_manager has all apps registered. */
void launcher_ui_init(void);

/* Update status bar text (called from main loop) */
void launcher_ui_update_status(const char *ble_status);

#endif /* LAUNCHER_UI_H */
