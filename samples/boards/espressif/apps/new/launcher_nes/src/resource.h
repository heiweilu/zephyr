/*
 * resource.h — Shared constants and declarations for Launcher
 */

#ifndef RESOURCE_H
#define RESOURCE_H

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

/* Backlight: IO47 = GPIO1 pin 15 */
#define BLK_NODE DT_NODELABEL(gpio1)
#define BLK_PIN  15

/* Status bar height */
#define STATUS_BAR_HEIGHT 24

/* Icon grid layout */
#define ICON_COLS     4
#define ICON_ROWS     2
#define ICON_SIZE     56
#define ICON_PADDING  12
#define ICON_LABEL_H  16

#endif /* RESOURCE_H */
