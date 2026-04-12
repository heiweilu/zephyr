/*
 * ICM-42607-C IMU Test — Phase 4: Module 9
 * Bare-metal I2C register access: WHO_AM_I + accel + gyro reading
 * Register map from TDK HAL: modules/hal/tdk/icm42x7x/imu/inv_imu_regmap_rev_a.h
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>

/* ICM-42607-C I2C address (AD0=0) */
#define ICM42607_ADDR          0x68

/* Register addresses — Bank 0 (ICM-42607/42670 family) */
#define REG_MCLK_RDY           0x00
#define REG_DEVICE_CONFIG      0x01
#define REG_SIGNAL_PATH_RESET  0x02
#define REG_INT_CONFIG         0x06
#define REG_TEMP_DATA1         0x09
#define REG_TEMP_DATA0         0x0A
#define REG_ACCEL_DATA_X1      0x0B
#define REG_GYRO_DATA_X1       0x11
#define REG_PWR_MGMT0          0x1F
#define REG_GYRO_CONFIG0       0x20
#define REG_ACCEL_CONFIG0      0x21
#define REG_INT_STATUS_DRDY    0x39
#define REG_WHO_AM_I           0x75

/* SIGNAL_PATH_RESET bit for soft reset */
#define BIT_SOFT_RESET         (1 << 4)

/* PWR_MGMT0: GYRO_MODE[3:2]=11(LN), ACCEL_MODE[1:0]=11(LN) */
#define PWR_GYRO_LN_ACCEL_LN  0x0F

/* ACCEL_CONFIG0: FS_SEL[6:5]=10(±4g), ODR[3:0]=9(100Hz) */
#define ACCEL_FS_4G_ODR_100   ((2 << 5) | 0x09)

/* GYRO_CONFIG0: FS_SEL[6:5]=10(±500dps), ODR[3:0]=9(100Hz) */
#define GYRO_FS_500_ODR_100   ((2 << 5) | 0x09)

static const struct device *i2c_dev;

static int imu_read_reg(uint8_t reg, uint8_t *val)
{
	return i2c_reg_read_byte(i2c_dev, ICM42607_ADDR, reg, val);
}

static int imu_write_reg(uint8_t reg, uint8_t val)
{
	return i2c_reg_write_byte(i2c_dev, ICM42607_ADDR, reg, val);
}

static int imu_read_burst(uint8_t start_reg, uint8_t *buf, uint8_t len)
{
	return i2c_burst_read(i2c_dev, ICM42607_ADDR, start_reg, buf, len);
}

static int imu_init(void)
{
	uint8_t who_am_i, val;
	int ret;

	/* Step 1: Read WHO_AM_I */
	ret = imu_read_reg(REG_WHO_AM_I, &who_am_i);
	if (ret) {
		printk("ERR: WHO_AM_I read failed: %d\n", ret);
		return ret;
	}
	printk("WHO_AM_I: 0x%02X\n", who_am_i);

	/* Step 2: Soft reset via SIGNAL_PATH_RESET[4] */
	ret = imu_write_reg(REG_SIGNAL_PATH_RESET, BIT_SOFT_RESET);
	if (ret) {
		printk("ERR: soft reset failed: %d\n", ret);
		return ret;
	}
	k_msleep(20);  /* Wait for reset */

	/* Step 3: Wait for MCLK_RDY */
	for (int i = 0; i < 10; i++) {
		ret = imu_read_reg(REG_MCLK_RDY, &val);
		if (ret) {
			printk("ERR: MCLK_RDY read failed: %d\n", ret);
			return ret;
		}
		if (val & 0x08) {  /* MCLK_RDY bit */
			printk("MCLK ready (0x%02X) after %d ms\n", val, (i + 1) * 5);
			break;
		}
		k_msleep(5);
	}

	/* Step 4: Verify WHO_AM_I after reset */
	ret = imu_read_reg(REG_WHO_AM_I, &who_am_i);
	if (ret) {
		printk("ERR: WHO_AM_I re-read failed: %d\n", ret);
		return ret;
	}
	printk("WHO_AM_I after reset: 0x%02X\n", who_am_i);

	/* Step 5: Configure accel — ±4g, 100Hz */
	ret = imu_write_reg(REG_ACCEL_CONFIG0, ACCEL_FS_4G_ODR_100);
	if (ret) {
		printk("ERR: accel config failed: %d\n", ret);
		return ret;
	}

	/* Step 6: Configure gyro — ±500°/s, 100Hz */
	ret = imu_write_reg(REG_GYRO_CONFIG0, GYRO_FS_500_ODR_100);
	if (ret) {
		printk("ERR: gyro config failed: %d\n", ret);
		return ret;
	}

	/* Step 7: Enable accel + gyro in low-noise mode */
	ret = imu_write_reg(REG_PWR_MGMT0, PWR_GYRO_LN_ACCEL_LN);
	if (ret) {
		printk("ERR: pwr mgmt failed: %d\n", ret);
		return ret;
	}
	k_msleep(50);  /* Wait for sensors to stabilize */

	printk("IMU init OK — accel ±4g, gyro ±500°/s, 100Hz\n");
	return 0;
}

int main(void)
{
	int ret;
	uint8_t data[12];
	int16_t ax, ay, az, gx, gy, gz;

	printk("\n=== ICM-42607-C IMU Test ===\n");

	i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
	if (!device_is_ready(i2c_dev)) {
		printk("ERR: I2C not ready\n");
		return -1;
	}
	printk("I2C ready\n");

	ret = imu_init();
	if (ret) {
		printk("IMU init failed\n");
		return -1;
	}

	/* Read 50 samples then stop */
	for (int i = 0; i < 50; i++) {
		/* Burst read 12 bytes: accel XYZ (6) + gyro XYZ (6) */
		ret = imu_read_burst(REG_ACCEL_DATA_X1, data, 12);
		if (ret) {
			printk("[%d] read err: %d\n", i, ret);
			k_msleep(100);
			continue;
		}

		ax = (int16_t)((data[0] << 8) | data[1]);
		ay = (int16_t)((data[2] << 8) | data[3]);
		az = (int16_t)((data[4] << 8) | data[5]);
		gx = (int16_t)((data[6] << 8) | data[7]);
		gy = (int16_t)((data[8] << 8) | data[9]);
		gz = (int16_t)((data[10] << 8) | data[11]);

		printk("[%2d] A: %6d %6d %6d  G: %6d %6d %6d\n",
		       i, ax, ay, az, gx, gy, gz);

		k_msleep(100);  /* ~10Hz print rate */
	}

	printk("=== IMU Test Done ===\n");
	return 0;
}
