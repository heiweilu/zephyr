/*
 * app_camera.h — one-way live preview entry.
 *
 * Once called, owns the LCD forever. There is no return path in
 * launcher_full phase 2d — exiting the camera requires a reset.
 */

#ifndef APP_CAMERA_H_
#define APP_CAMERA_H_

void app_camera_run(void);

#endif /* APP_CAMERA_H_ */
