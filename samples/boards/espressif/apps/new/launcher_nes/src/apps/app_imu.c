/*
 * app_imu.c — ICM-42607-C live dashboard (real data).
 *
 * Replaces the previous "Phase 5" placeholder. Reads the ICM-42607-C
 * over I2C0 (0x68), drives an LVGL dashboard via lv_timer @ 10 Hz.
 * Plays nicely with launcher's app_manager: on_create builds the UI
 * and starts the timer; on_destroy stops the timer and parks the IMU
 * in low-power so it doesn't keep streaming on the bus.
 *
 * Register map: TDK HAL modules/hal/tdk/icm42x7x/imu/inv_imu_regmap_rev_a.h
 * Math reference: samples/.../new/imu_test/src/main.c
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <lvgl.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#include "../app_manager.h"

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
#define PWR_GYRO_LN_ACCEL_LN   0x0F
#define PWR_OFF                0x00
#define ACCEL_FS_4G_ODR_100    ((2 << 5) | 0x09)
#define GYRO_FS_500_ODR_100    ((2 << 5) | 0x09)

/* ±4 g  → 8192 LSB/g    |    ±500 °/s → 65.5 LSB/(°/s)  (rounded to 65) */
#define ACC_LSB_PER_G    8192
#define GYR_LSB_PER_DPS  65

static const struct device *i2c_dev;
static lv_timer_t *imu_timer;

static lv_obj_t *acc_lbl;
static lv_obj_t *gyr_lbl;
static lv_obj_t *bar_fg;
static lv_obj_t *status_lbl;

/* 3D cube canvas (100x100 RGB565 = 20 KB, sits in PSRAM via .ext_ram.bss) */
#define CUBE_W 100
#define CUBE_H 100
static lv_obj_t *cube_canvas;
static lv_obj_t *axis_lbl_x, *axis_lbl_y, *axis_lbl_z;
static int cube_origin_x, cube_origin_y;  /* canvas position on screen */
static uint8_t cube_buf[CUBE_W * CUBE_H * 2]
	__attribute__((section(".ext_ram.bss"), aligned(4)));

/* Orientation state (radians). Yaw is integrated from gyro Z and will drift. */
static float roll, pitch, yaw;

static int imu_write(uint8_t reg, uint8_t val)
{
	return i2c_reg_write_byte(i2c_dev, ICM42607_ADDR, reg, val);
}
static int imu_read(uint8_t reg, uint8_t *val)
{
	return i2c_reg_read_byte(i2c_dev, ICM42607_ADDR, reg, val);
}
static int imu_burst(uint8_t reg, uint8_t *buf, uint8_t len)
{
	return i2c_burst_read(i2c_dev, ICM42607_ADDR, reg, buf, len);
}

static int imu_chip_init(void)
{
	uint8_t v;
	if (imu_read(REG_WHO_AM_I, &v)) {
		LOG_ERR("WHO_AM_I read failed");
		return -1;
	}
	LOG_INF("WHO_AM_I=0x%02X", v);
	(void)imu_write(REG_SIGNAL_PATH_RESET, BIT_SOFT_RESET);
	k_msleep(20);
	for (int i = 0; i < 10; i++) {
		if (imu_read(REG_MCLK_RDY, &v) == 0 && (v & 0x08)) {
			break;
		}
		k_msleep(5);
	}
	(void)imu_write(REG_ACCEL_CONFIG0, ACCEL_FS_4G_ODR_100);
	(void)imu_write(REG_GYRO_CONFIG0,  GYRO_FS_500_ODR_100);
	(void)imu_write(REG_PWR_MGMT0,     PWR_GYRO_LN_ACCEL_LN);
	k_msleep(50);
	LOG_INF("IMU online (±4g, ±500dps, 100Hz)");
	return 0;
}

/* ── 3D wireframe cube renderer ──
 * Cube vertices in body frame (unit cube around origin), 8 corners and 12 edges.
 * Apply ZYX rotation (yaw → pitch → roll), orthographic project, draw lines
 * onto cube_canvas. Lines are drawn with Bresenham directly into the RGB565
 * framebuffer to avoid pulling in LVGL's draw subsystem on a hot path.
 */
static const int8_t cube_v[8][3] = {
	{-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
	{-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
};
static const uint8_t cube_e[12][2] = {
	{0,1},{1,2},{2,3},{3,0},  /* bottom */
	{4,5},{5,6},{6,7},{7,4},  /* top */
	{0,4},{1,5},{2,6},{3,7},  /* pillars */
};

static inline void put_px(int x, int y, uint16_t color)
{
	if ((unsigned)x >= CUBE_W || (unsigned)y >= CUBE_H) {
		return;
	}
	uint8_t *p = cube_buf + (y * CUBE_W + x) * 2;
	p[0] = color & 0xFF;       /* little-endian, low byte first */
	p[1] = color >> 8;
}

static void draw_line(int x0, int y0, int x1, int y1, uint16_t color)
{
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	for (;;) {
		put_px(x0, y0, color);
		if (x0 == x1 && y0 == y1) break;
		int e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

static void cube_render(void)
{
	/* Background = screen bg 0x0F172A → RGB565 = 0x0865 (LE: 0x65 0x08) */
	uint8_t lo = 0x65, hi = 0x08;
	for (int i = 0; i < CUBE_W * CUBE_H * 2; i += 2) {
		cube_buf[i]     = lo;
		cube_buf[i + 1] = hi;
	}

	float cr = cosf(roll),  sr = sinf(roll);
	float cp = cosf(pitch), sp = sinf(pitch);
	float cy = cosf(yaw),   sy = sinf(yaw);

	/* R = Rz(yaw) * Ry(pitch) * Rx(roll) — projects body axes into screen */
	float R00 = cy * cp;
	float R01 = cy * sp * sr - sy * cr;
	float R02 = cy * sp * cr + sy * sr;
	float R10 = sy * cp;
	float R11 = sy * sp * sr + cy * cr;
	float R12 = sy * sp * cr - cy * sr;

	int sxv[8], syp[8];
	const float scale = 18.0f;
	const int cx = CUBE_W / 2, cyp = CUBE_H / 2;

	for (int i = 0; i < 8; i++) {
		float vx = cube_v[i][0], vy = cube_v[i][1], vz = cube_v[i][2];
		float x = R00 * vx + R01 * vy + R02 * vz;
		float y = R10 * vx + R11 * vy + R12 * vz;
		sxv[i] = cx + (int)(x * scale);
		syp[i] = cyp + (int)(y * scale);
	}

	/* Cube edges: light grey 0xC618 (LE: 0x18 0xC6) */
	const uint16_t edge_le = 0xC618;
	for (int i = 0; i < 12; i++) {
		draw_line(sxv[cube_e[i][0]], syp[cube_e[i][0]],
			  sxv[cube_e[i][1]], syp[cube_e[i][1]], edge_le);
	}

	/* Body-frame axes: X red, Y green, Z blue. Length = 1.6 units. */
	const float al = 1.6f;
	int ax_x = cx + (int)(R00 * al * scale);
	int ax_y = cyp + (int)(R10 * al * scale);
	int ay_x = cx + (int)(R01 * al * scale);
	int ay_y = cyp + (int)(R11 * al * scale);
	int az_x = cx + (int)(R02 * al * scale);
	int az_y = cyp + (int)(R12 * al * scale);

	draw_line(cx, cyp, ax_x, ax_y, 0xF800);  /* X red */
	draw_line(cx, cyp, ay_x, ay_y, 0x07E0);  /* Y green */
	draw_line(cx, cyp, az_x, az_y, 0x001F);  /* Z blue */

	/* Tip dots */
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			put_px(ax_x + dx, ax_y + dy, 0xF800);
			put_px(ay_x + dx, ay_y + dy, 0x07E0);
			put_px(az_x + dx, az_y + dy, 0x001F);
		}
	}

	/* Move X/Y/Z labels to follow each axis tip in screen coordinates */
	if (axis_lbl_x) {
		lv_obj_set_pos(axis_lbl_x, cube_origin_x + ax_x + 2, cube_origin_y + ax_y - 8);
		lv_obj_set_pos(axis_lbl_y, cube_origin_x + ay_x + 2, cube_origin_y + ay_y - 8);
		lv_obj_set_pos(axis_lbl_z, cube_origin_x + az_x + 2, cube_origin_y + az_y - 8);
	}

	if (cube_canvas) {
		lv_obj_invalidate(cube_canvas);
	}
}

static void imu_tick(lv_timer_t *t)
{
	uint8_t d[12];
	if (!i2c_dev || imu_burst(REG_ACCEL_DATA_X1, d, 12)) {
		return;
	}
	int16_t ax = (int16_t)((d[0]  << 8) | d[1]);
	int16_t ay = (int16_t)((d[2]  << 8) | d[3]);
	int16_t az = (int16_t)((d[4]  << 8) | d[5]);
	int16_t gx = (int16_t)((d[6]  << 8) | d[7]);
	int16_t gy = (int16_t)((d[8]  << 8) | d[9]);
	int16_t gz = (int16_t)((d[10] << 8) | d[11]);

	int axm = (int)ax * 1000 / ACC_LSB_PER_G;
	int aym = (int)ay * 1000 / ACC_LSB_PER_G;
	int azm = (int)az * 1000 / ACC_LSB_PER_G;
	int gxm = (int)gx * 1000 / GYR_LSB_PER_DPS;
	int gym = (int)gy * 1000 / GYR_LSB_PER_DPS;
	int gzm = (int)gz * 1000 / GYR_LSB_PER_DPS;

	char buf[80];
	snprintf(buf, sizeof(buf),
		 "ACC mg\n  X %+5d\n  Y %+5d\n  Z %+5d", axm, aym, azm);
	lv_label_set_text(acc_lbl, buf);
	snprintf(buf, sizeof(buf),
		 "GYR mdps\n  X %+6d\n  Y %+6d\n  Z %+6d", gxm, gym, gzm);
	lv_label_set_text(gyr_lbl, buf);

	int cursor = 107 + axm * 100 / 1000;
	if (cursor < 0)   cursor = 0;
	if (cursor > 214) cursor = 214;
	lv_obj_set_x(bar_fg, cursor);

	/* ── Orientation update (MVP, no fusion filter) ──
	 * roll  = atan2(ay, az)        — rotation around X
	 * pitch = atan2(-ax, sqrt(ay²+az²)) — rotation around Y
	 * yaw   += gz_dps * dt          — drift-prone integration
	 */
	float fax = (float)ax / ACC_LSB_PER_G;
	float fay = (float)ay / ACC_LSB_PER_G;
	float faz = (float)az / ACC_LSB_PER_G;
	roll  = atan2f(fay, faz);
	pitch = atan2f(-fax, sqrtf(fay * fay + faz * faz));
	yaw  += ((float)gz / GYR_LSB_PER_DPS) * 0.1f * (M_PI / 180.0f);

	cube_render();
}

static void back_cb(lv_event_t *e)
{
	app_manager_back_to_home();
}

static void on_create(lv_obj_t *screen)
{
	lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F172A), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

	lv_obj_t *title = lv_label_create(screen);
	lv_label_set_text(title, "IMU - ICM42607");
	lv_obj_set_style_text_color(title, lv_color_hex(0xF97316), 0);
	lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

	acc_lbl = lv_label_create(screen);
	lv_obj_set_style_text_color(acc_lbl, lv_color_hex(0x10B981), 0);
	lv_obj_set_style_text_font(acc_lbl, &lv_font_montserrat_16, 0);
	lv_obj_align(acc_lbl, LV_ALIGN_TOP_LEFT, 6, 36);
	lv_label_set_text(acc_lbl, "ACC mg\n  X    --\n  Y    --\n  Z    --");

	gyr_lbl = lv_label_create(screen);
	lv_obj_set_style_text_color(gyr_lbl, lv_color_hex(0x60A5FA), 0);
	lv_obj_set_style_text_font(gyr_lbl, &lv_font_montserrat_16, 0);
	lv_obj_align(gyr_lbl, LV_ALIGN_TOP_RIGHT, -6, 36);
	lv_label_set_text(gyr_lbl, "GYR mdps\n  X     --\n  Y     --\n  Z     --");

	/* 3D cube canvas in the middle */
	cube_canvas = lv_canvas_create(screen);
	lv_canvas_set_buffer(cube_canvas, cube_buf, CUBE_W, CUBE_H, LV_COLOR_FORMAT_RGB565);
	lv_obj_align(cube_canvas, LV_ALIGN_CENTER, 0, -10);
	/* Force layout so we can read screen-space origin for axis labels */
	lv_obj_update_layout(cube_canvas);
	cube_origin_x = lv_obj_get_x(cube_canvas);
	cube_origin_y = lv_obj_get_y(cube_canvas);

	axis_lbl_x = lv_label_create(screen);
	lv_obj_set_style_text_color(axis_lbl_x, lv_color_hex(0xF87171), 0);
	lv_obj_set_style_text_font(axis_lbl_x, &lv_font_montserrat_14, 0);
	lv_label_set_text(axis_lbl_x, "X");

	axis_lbl_y = lv_label_create(screen);
	lv_obj_set_style_text_color(axis_lbl_y, lv_color_hex(0x4ADE80), 0);
	lv_obj_set_style_text_font(axis_lbl_y, &lv_font_montserrat_14, 0);
	lv_label_set_text(axis_lbl_y, "Y");

	axis_lbl_z = lv_label_create(screen);
	lv_obj_set_style_text_color(axis_lbl_z, lv_color_hex(0x60A5FA), 0);
	lv_obj_set_style_text_font(axis_lbl_z, &lv_font_montserrat_14, 0);
	lv_label_set_text(axis_lbl_z, "Z");

	cube_render();

	lv_obj_t *bar_bg = lv_obj_create(screen);
	lv_obj_remove_style_all(bar_bg);
	lv_obj_set_size(bar_bg, 220, 14);
	lv_obj_align(bar_bg, LV_ALIGN_BOTTOM_MID, 0, -56);
	lv_obj_set_style_bg_color(bar_bg, lv_color_hex(0x1F2937), 0);
	lv_obj_set_style_bg_opa(bar_bg, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(bar_bg, 4, 0);

	bar_fg = lv_obj_create(bar_bg);
	lv_obj_remove_style_all(bar_fg);
	lv_obj_set_size(bar_fg, 6, 14);
	lv_obj_set_pos(bar_fg, 107, 0);
	lv_obj_set_style_bg_color(bar_fg, lv_color_hex(0xFBBF24), 0);
	lv_obj_set_style_bg_opa(bar_fg, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(bar_fg, 3, 0);

	status_lbl = lv_label_create(screen);
	lv_obj_set_style_text_color(status_lbl, lv_color_hex(0x6B7280), 0);
	lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_14, 0);
	lv_obj_align(status_lbl, LV_ALIGN_BOTTOM_MID, 0, -34);

	lv_obj_t *btn = lv_button_create(screen);
	lv_obj_set_size(btn, 120, 32);
	lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -2);
	lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *lbl = lv_label_create(btn);
	lv_label_set_text(lbl, "< Back");
	lv_obj_center(lbl);
	lv_group_t *g = app_manager_get_kb_group();
	if (g) {
		lv_group_add_obj(g, btn);
	}

	i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
	if (!device_is_ready(i2c_dev) || imu_chip_init() != 0) {
		lv_label_set_text(status_lbl, "I2C/IMU not ready");
		i2c_dev = NULL;
		return;
	}
	lv_label_set_text(status_lbl, "live @ 10 Hz");
	imu_timer = lv_timer_create(imu_tick, 100, NULL);
}

static void on_destroy(void)
{
	if (imu_timer) {
		lv_timer_delete(imu_timer);
		imu_timer = NULL;
	}
	if (i2c_dev) {
		(void)imu_write(REG_PWR_MGMT0, PWR_OFF);
	}
	i2c_dev = NULL;
	acc_lbl = gyr_lbl = bar_fg = status_lbl = NULL;
	cube_canvas = NULL;
	axis_lbl_x = axis_lbl_y = axis_lbl_z = NULL;
	roll = pitch = yaw = 0.0f;
}

const app_info_t app_imu = {
	.name = "IMU",
	.icon_color = LV_COLOR_MAKE(0x14, 0xB8, 0xA6),
	.icon_symbol = LV_SYMBOL_REFRESH,
	.on_create = on_create,
	.on_destroy = on_destroy,
};
