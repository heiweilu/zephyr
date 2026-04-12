#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define I2C_NODE DT_NODELABEL(i2c0)
#define XCLK_PWM_SPEC PWM_DT_SPEC_GET(DT_PATH(zephyr_user))

#define OV3660_I2C_ADDR 0x3c
#define REG_CHIP_ID_HIGH 0x300a
#define REG_CHIP_ID_LOW  0x300b
#define OV3660_PID       0x3660

static const struct device *const i2c_dev = DEVICE_DT_GET(I2C_NODE);
static const struct pwm_dt_spec xclk_pwm = XCLK_PWM_SPEC;

static int ov3660_read_reg(uint16_t reg, uint8_t *value)
{
	uint8_t addr_buf[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xff)};

	return i2c_write_read(i2c_dev, OV3660_I2C_ADDR, addr_buf, sizeof(addr_buf), value, 1);
}

int main(void)
{
	int ret;

	printk("\n*** CHD-ESP32-S3-BOX OV3660 Probe ***\n");
	printk("I2C0: SDA=IO8 SCL=IO18\n");
	printk("XMCLK: IO39 via LEDC channel 0\n");

	if (!device_is_ready(i2c_dev)) {
		printk("I2C device not ready\n");
		return 0;
	}

	if (!pwm_is_ready_dt(&xclk_pwm)) {
		printk("PWM device for XMCLK not ready\n");
		return 0;
	}

	ret = pwm_set_dt(&xclk_pwm, 100U, 50U);
	if (ret < 0) {
		printk("Failed to start XMCLK: %d\n", ret);
		return 0;
	}

	printk("XMCLK started at 10MHz\n");
	k_msleep(50);

	while (1) {
		uint8_t pid_high = 0;
		uint8_t pid_low = 0;
		uint16_t pid = 0;

		printk("\n[probe] checking OV3660 at 0x%02x\n", OV3660_I2C_ADDR);

		ret = i2c_write(i2c_dev, NULL, 0, OV3660_I2C_ADDR);
		if (ret < 0) {
			printk("[probe] no ACK: %d\n", ret);
			k_msleep(2000);
			continue;
		}

		printk("[probe] SCCB ACK received\n");

		ret = ov3660_read_reg(REG_CHIP_ID_HIGH, &pid_high);
		if (ret < 0) {
			printk("[probe] CHIP_ID high read failed: %d\n", ret);
			k_msleep(2000);
			continue;
		}

		ret = ov3660_read_reg(REG_CHIP_ID_LOW, &pid_low);
		if (ret < 0) {
			printk("[probe] CHIP_ID low read failed: %d\n", ret);
			k_msleep(2000);
			continue;
		}

		pid = ((uint16_t)pid_high << 8) | pid_low;
		printk("[probe] CHIP ID: 0x%04x\n", pid);

		if (pid == OV3660_PID) {
			printk("[probe] OV3660 detection OK\n");
		} else {
			printk("[probe] unexpected sensor ID, expected 0x%04x\n", OV3660_PID);
		}

		k_msleep(2000);
	}

	return 0;
}