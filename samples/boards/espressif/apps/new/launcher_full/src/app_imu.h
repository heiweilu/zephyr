/*
 * app_imu.h — one-way IMU dashboard.
 *
 * Owns the LCD forever once entered. Reads ICM-42607-C over I2C0,
 * shows live accel/gyro values via LVGL. Reset to leave.
 */

#ifndef APP_IMU_H_
#define APP_IMU_H_

void app_imu_run(void);

#endif /* APP_IMU_H_ */
