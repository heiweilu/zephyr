#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/device.h>

#define I2C_DEV_NODE DT_NODELABEL(i2c0)

/* Known device addresses on CHD-ESP32-S3-BOX I2C bus */
static const struct {
	uint8_t addr;
	const char *name;
} known_devices[] = {
	{0x18, "ES8311 (audio codec)"},
	{0x40, "ES7210 (ADC codec)"},
	{0x68, "ICM-42607-C (IMU)"},
	{0x69, "ICM-42607-C (IMU alt)"},
};

static const char *lookup_device(uint8_t addr)
{
	for (int i = 0; i < ARRAY_SIZE(known_devices); i++) {
		if (known_devices[i].addr == addr) {
			return known_devices[i].name;
		}
	}
	return NULL;
}

int main(void)
{
	const struct device *i2c_dev = DEVICE_DT_GET(I2C_DEV_NODE);
	int found = 0;

	printk("\n*** CHD-ESP32-S3-BOX I2C Bus Scan ***\n");
	printk("I2C bus: %s (SCL=IO18, SDA=IO8)\n\n", i2c_dev->name);

	if (!device_is_ready(i2c_dev)) {
		printk("ERROR: I2C device not ready\n");
		return -1;
	}

	printk("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

	for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
		if ((addr & 0x0f) == 0x00 || addr == 0x03) {
			printk("%02x: ", addr & 0xf0);
			/* Fill leading blanks for first row */
			if (addr == 0x03) {
				printk("         ");
			}
		}

		struct i2c_msg msg;
		uint8_t dummy;
		msg.buf = &dummy;
		msg.len = 0;
		msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

		int ret = i2c_transfer(i2c_dev, &msg, 1, addr);
		if (ret == 0) {
			printk("%02x ", addr);
			found++;
		} else {
			printk("-- ");
		}

		if ((addr & 0x0f) == 0x0f || addr == 0x77) {
			printk("\n");
		}
	}

	printk("\n%d device(s) found.\n\n", found);

	/* Identify known devices */
	printk("=== Device Identification ===\n");
	for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
		struct i2c_msg msg;
		uint8_t dummy;
		msg.buf = &dummy;
		msg.len = 0;
		msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

		if (i2c_transfer(i2c_dev, &msg, 1, addr) == 0) {
			const char *name = lookup_device(addr);
			printk("  0x%02x: %s\n", addr,
			       name ? name : "(unknown)");
		}
	}

	printk("\nScan complete.\n");

	return 0;
}
