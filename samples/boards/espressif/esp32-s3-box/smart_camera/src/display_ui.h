#ifndef SMART_CAMERA_DISPLAY_UI_H_
#define SMART_CAMERA_DISPLAY_UI_H_

#include <stdint.h>
#include <stddef.h>

/* Initialise LVGL + LCD; show a status screen.
 * Returns 0 on success.
 */
int display_ui_init(void);

/* Replace the status text shown at the top of the screen. */
void display_ui_set_status(const char *text);

/* Switch the active screen to the live MJPEG canvas (call once after Wi-Fi
 * is up and before mjpeg_client_start()).
 */
void display_ui_show_canvas(void);

/* Submit a JPEG frame for decode + display. The buffer is consumed before
 * this call returns (the caller may free/reuse it).
 */
void display_ui_push_jpeg(const uint8_t *jpeg, size_t len, uint32_t seq);

/* Last measured display FPS (decoded + drawn). 0 until first sample. */
uint32_t display_ui_get_fps_x10(void);

/* Set crosshair position over the live canvas. cx<0 hides the crosshair.
 * Coordinates are in canvas pixels (0..319 / 0..239).
 */
void display_ui_set_target(int cx, int cy);

#endif /* SMART_CAMERA_DISPLAY_UI_H_ */
