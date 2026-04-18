/*
 * home_ui.h — launcher_full HOME screen
 *
 * 8 icons in a 4x2 grid (ported from launcher/src/launcher_ui.c, simplified).
 * Navigation is done with a single BOOT key:
 *   short press  → focus moves to next icon
 *   double press → invoke selected app
 *
 * Two real apps are wired in 2d/2e:
 *   APP_CAMERA     — live preview only
 *   APP_AI_VISION  — capture + qwen-vl-plus + CosyVoice TTS
 *
 * The other 6 icons are stubs that show a "敬请期待" toast.
 */

#ifndef HOME_UI_H_
#define HOME_UI_H_

#include <stddef.h>

#define APP_AI_VISION   0
#define APP_CAMERA      1
#define APP_NES         2
#define APP_PHOTOS      3
#define APP_TERMINAL    4
#define APP_MUSIC       5
#define APP_FACE        6
#define APP_IMU         7
#define APP_COUNT       8

/* Build the home screen and load it. Must be called after display init. */
void home_ui_init(void);

/* Move focus to the next icon (called from BOOT short-press handler). */
void home_ui_focus_next(void);

/* Activate the currently focused icon (called from BOOT double-press). */
void home_ui_activate_selected(void);

/* Returns the currently focused app id (0..APP_COUNT-1). */
int  home_ui_get_selected(void);

#endif /* HOME_UI_H_ */
