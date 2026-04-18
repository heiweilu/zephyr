/*
 * app_imu.c — ICM-42607-C live dashboard (LVGL).
 *
 * Phase 2f port of imu_test:
 *   - I2C0 @ 0x68
 *   - ±4 g, ±500 °/s, 100 Hz sampling
 *   - LVGL screen with 6 live values + tilt bar
 *
 * One-way: never returns. Reset to leave.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <lvgl.h>

#include "app_imu.h"

LOG_MODULE_REGISTER(app_imu, LOG_LEVEL_INF);

/* ── ICM-42607-C registers (subset) ── */
#define ICM42607_ADDR          0x68
#define REG_MCLK_RDY           0x00
#define REG_SIGNAL_PATH_RESET  0x02
#define REG_ACCEL_DATA_X1      0x0B
#define REG_PWR_MGMT0          0x1F
#define REG_GYRO_CONFIG0       0x20
#define REG_ACCEL_CONFIG0      0x21
#define REG_WHO_AM_I           0x75
#define BIT_SOFT_RESET         (1 << 4)
#define PWR_GYRO_LN_ACCEL_LN  0x0F
#define ACCEL_FS_4G_ODR_100   ((2 << 5) | 0x09)
#define GYRO_FS_500_ODR_100   ((2 << 5) | 0x09)

/* Conversion: ±4 g  → 8192 LSB/g    (full-scale 32768)
 *             ±500 °/s → 65.5 LSB/(°/s)
 */
#define ACC_LSB_PER_G   8192
#define GYR_LSB_PER_DPS 65   /* round 65.5 → 65 for integer math */

static const struct device *i2c_dev;

static int imu_init(void)
{
	uint8_t v;
	int ret;

	if (i2c_reg_read_byte(i2c_dev, ICM42607_ADDR, REG_WHO_AM_I, &v)) {
		LOG_ERR("WHO_AM_I read failed");
		return -1;
	}
	LOG_INF("WHO_AM_I=0x%02X", v);

	(void)i2c_reg_write_byte(i2c_dev, ICM42607_ADDR, REG_SIGNAL_PATH_RESET, BIT_SOFT_RESET);
	k_msleep(20);
	for (int i = 0; i < 10; i++) {
		ret = i2c_reg_read_byte(i2c_dev, ICM42607_ADDR, REG_MCLK_RDY, &v);
		if (!ret && (v & 0x08)) {
			break;
		}
		k_msleep(5);
	}

	(void)i2c_reg_write_byte(i2c_dev, ICM42607_ADDR, REG_ACCEL_CONFIG0, ACCEL_FS_4G_ODR_100);
	(void)i2c_reg_write_byte(i2c_dev, ICM42607_ADDR, REG_GYRO_CONFIG0,  GYRO_FS_500_ODR_100);
	(void)i2c_reg_write_byte(i2c_dev, ICM42607_ADDR, REG_PWR_MGMT0,     PWR_GYRO_LN_ACCEL_LN);
	k_msleep(50);
	LOG_INF("IMU online (±4g, ±500dps, 100Hz)");
	return 0;
}

static int imu_read(int16_t *ax, int16_t *ay, int16_t *az,
		    int16_t *gx, int16_t *gy, int16_t *gz)
{
	uint8_t d[12];
	int r = i2c_burst_read(i2c_dev, ICM42607_ADDR, REG_ACCEL_DATA_X1, d, 12);
	if (r) {
		return r;
	}
	*ax = (int16_t)((d[0]  << 8) | d[1]);
	*ay = (int16_t)((d[2]  << 8) | d[3]);
	*az = (int16_t)((d[4]  << 8) | d[5]);
	*gx = (int16_t)((d[6]  << 8) | d[7]);
	*gy = (int16_t)((d[8]  << 8) | d[9]);
	*gz = (int16_t)((d[10] << 8) | d[11]);
	return 0;
}

void app_imu_run(void)
{
	LOG_INF("entering IMU dashboard (one-way)");

	i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
	bool have_imu = device_is_ready(i2c_dev) && imu_init() == 0;

	/* Build a fresh LVGL screen */
	lv_obj_t *scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x0F172A), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "IMU - ICM42607");
	lv_obj_set_style_text_color(title, lv_color_hex(0xF97316), 0);
	lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

	lv_obj_t *acc_lbl = lv_label_create(scr);
	lv_obj_set_style_text_color(acc_lbl, lv_color_hex(0x10B981), 0);
	lv_obj_set_style_text_font(acc_lbl, &lv_font_montserrat_16, 0);
	lv_obj_align(acc_lbl, LV_ALIGN_TOP_LEFT, 14, 40);

	lv_obj_t *gyr_lbl = lv_label_create(scr);
	lv_obj_set_style_text_color(gyr_lbl, lv_color_hex(0x60A5FA), 0);
	lv_obj_set_style_text_font(gyr_lbl, &lv_font_montserrat_16, 0);
	lv_obj_align(gyr_lbl, LV_ALIGN_TOP_LEFT, 14, 110);

	/* Tilt bar — 200 px wide, fills with horizontal accel projection */
	lv_obj_t *bar_bg = lv_obj_create(scr);
	lv_obj_remove_style_all(bar_bg);
	lv_obj_set_size(bar_bg, 220, 14);
	lv_obj_align(bar_bg, LV_ALIGN_BOTTOM_MID, 0, -16);
	lv_obj_set_style_bg_color(bar_bg, lv_color_hex(0x1F2937), 0);
	lv_obj_set_style_bg_opa(bar_bg, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(bar_bg, 4, 0);

	lv_obj_t *bar_fg = lv_obj_create(bar_bg);
	lv_obj_remove_style_all(bar_fg);
	lv_obj_set_size(bar_fg, 6, 14);
	lv_obj_set_pos(bar_fg, 107, 0);
	lv_obj_set_style_bg_color(bar_fg, lv_color_hex(0xFBBF24), 0);
	lv_obj_set_style_bg_opa(bar_fg, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(bar_fg, 3, 0);

	lv_obj_t *foot = lv_label_create(scr);
	lv_label_set_text(foot, "RST to leave");
	lv_obj_set_style_text_color(foot, lv_color_hex(0x6B7280), 0);
	lv_obj_set_style_text_font(foot, &lv_font_montserrat_14, 0);
	lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -2);

	lv_screen_load(scr);

	if (!have_imu) {
		lv_label_set_text(acc_lbl, "I2C/IMU not ready");
		lv_label_set_text(gyr_lbl, "(check overlay)");
		while (1) {
			lv_task_handler();
			k_msleep(100);
		}
	}

	char acc_buf[64], gyr_buf[64];
	int16_t ax, ay, az, gx, gy, gz;

	while (1) {
		if (imu_read(&ax, &ay, &az, &gx, &gy, &gz) == 0) {
			/* milli-g and milli-dps for clean integer print */
			int axm = (int)ax * 1000 / ACC_LSB_PER_G;
			int aym = (int)ay * 1000 / ACC_LSB_PER_G;
			int azm = (int)az * 1000 / ACC_LSB_PER_G;
			int gxm = (int)gx * 1000 / GYR_LSB_PER_DPS;
			int gym = (int)gy * 1000 / GYR_LSB_PER_DPS;
			int gzm = (int)gz * 1000 / GYR_LSB_PER_DPS;

			snprintf(acc_buf, sizeof(acc_buf),
				 "ACC mg\n  X %+5d\n  Y %+5d\n  Z %+5d", axm, aym, azm);
			snprintf(gyr_buf, sizeof(gyr_buf),
				 "GYR mdps\n  X %+6d\n  Y %+6d\n  Z %+6d", gxm, gym, gzm);
			lv_label_set_text(acc_lbl, acc_buf);
			lv_label_set_text(gyr_lbl, gyr_buf);

			/* Tilt bar: AX → cursor position. ±1g maps to ±100 px around centre. */
			int cursor = 107 + axm * 100 / 1000;
			if (cursor < 0)   cursor = 0;
			if (cursor > 214) cursor = 214;
			lv_obj_set_x(bar_fg, cursor);
		}
		lv_task_handler();
		k_msleep(100);
	}
}
