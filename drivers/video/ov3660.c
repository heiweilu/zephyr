/*
 * Minimal OV3660 source driver for CHD-ESP32-S3-BOX bring-up.
 *
 * The current implementation intentionally targets a single verified mode:
 * RGB565 at 320x240. It defers sensor initialization until the first
 * video_set_format() call so the board can start XMCLK from application code.
 */

#define DT_DRV_COMPAT ovti_ov3660

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "video_device.h"

LOG_MODULE_REGISTER(video_ov3660, CONFIG_VIDEO_LOG_LEVEL);

#define OV3660_CHIP_ID_REG_H 0x300a
#define OV3660_CHIP_ID_REG_L 0x300b
#define OV3660_CHIP_ID       0x3660

#define OV3660_SYSTEM_CTRL0     0x3008
#define OV3660_TIMING_TC_REG20  0x3820
#define OV3660_TIMING_TC_REG21  0x3821
#define OV3660_X_ADDR_ST_H      0x3800
#define OV3660_X_ADDR_END_H     0x3804
#define OV3660_X_OUTPUT_SIZE_H  0x3808
#define OV3660_X_TOTAL_SIZE_H   0x380c
#define OV3660_X_OFFSET_H       0x3810
#define OV3660_X_INCREMENT      0x3814
#define OV3660_Y_INCREMENT      0x3815
#define OV3660_ISP_CONTROL_01   0x5001
#define OV3660_FORMAT_CTRL      0x501f
#define OV3660_FORMAT_CTRL00    0x4300
#define OV3660_SC_PLLS_CTRL0    0x303a
#define OV3660_SC_PLLS_CTRL1    0x303b
#define OV3660_SC_PLLS_CTRL2    0x303c
#define OV3660_SC_PLLS_CTRL3    0x303d
#define OV3660_PCLK_RATIO       0x3824
#define OV3660_VFIFO_CTRL0C     0x460c

struct ov3660_reg {
	uint16_t addr;
	uint8_t value;
};

struct ov3660_config {
	struct i2c_dt_spec i2c;
#if DT_INST_NODE_HAS_PROP(0, reset_gpios)
	struct gpio_dt_spec reset_gpio;
#endif
#if DT_INST_NODE_HAS_PROP(0, powerdown_gpios)
	struct gpio_dt_spec powerdown_gpio;
#endif
};

struct ov3660_data {
	struct video_format fmt;
	bool initialized;
};

static const struct video_format_cap ov3660_fmts[] = {
	{
		.pixelformat = VIDEO_PIX_FMT_RGB565,
		.width_min = 320,
		.width_max = 320,
		.height_min = 240,
		.height_max = 240,
		.width_step = 0,
		.height_step = 0,
	},
	{0}
};

static const struct ov3660_reg ov3660_default_regs[] = {
	{OV3660_SYSTEM_CTRL0, 0x82},
	{0x3103, 0x13},
	{OV3660_SYSTEM_CTRL0, 0x42},
	{0x3017, 0xff},
	{0x3018, 0xff},
	{0x302c, 0xc3},
	{0x4740, 0x21},
	{0x3611, 0x01},
	{0x3612, 0x2d},
	{0x3032, 0x00},
	{0x3614, 0x80},
	{0x3619, 0x75},
	{0x3622, 0x80},
	{0x3630, 0x52},
	{0x3704, 0x80},
	{0x3708, 0x66},
	{0x3709, 0x12},
	{0x370b, 0x12},
	{0x371b, 0x60},
	{0x3901, 0x13},
	{0x3600, 0x08},
	{0x3620, 0x43},
	{0x3702, 0x20},
	{0x3739, 0x48},
	{0x370c, 0x0c},
	{0x3a18, 0x00},
	{0x3a19, 0xf8},
	{0x3000, 0x10},
	{0x3004, 0xef},
	{0x3c01, 0x80},
	{0x3c00, 0x04},
	{0x3a08, 0x00},
	{0x3a09, 0x62},
	{0x3a0e, 0x08},
	{0x3a0a, 0x00},
	{0x3a0b, 0x52},
	{0x3a0d, 0x09},
	{0x3a00, 0x3a},
	{0x3a14, 0x09},
	{0x3a15, 0x30},
	{0x3a02, 0x09},
	{0x3a03, 0x30},
	{0x440e, 0x08},
	{0x4520, 0x0b},
	{0x460b, 0x37},
	{0x4713, 0x02},
	{0x471c, 0xd0},
	{0x5002, 0x00},
	{0x501f, 0x00},
	{OV3660_SYSTEM_CTRL0, 0x02},
	{0x5000, 0xa7},
	{OV3660_ISP_CONTROL_01, 0x83},
	{0x5302, 0x28},
	{0x5303, 0x20},
	{0x5306, 0x1c},
	{0x5307, 0x28},
	{0x4002, 0xc5},
	{0x4003, 0x81},
	{0x4005, 0x12},
	{0x5580, 0x06},
	{0x5588, 0x00},
	{0x5583, 0x40},
	{0x5584, 0x2c},
};

static const struct ov3660_reg ov3660_rgb565_regs[] = {
	{OV3660_FORMAT_CTRL, 0x01},
	{OV3660_FORMAT_CTRL00, 0x61},
};

static int ov3660_read_reg(const struct i2c_dt_spec *spec, uint16_t reg, uint8_t *value)
{
	uint8_t addr_buf[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xff)};
	int attempts = CONFIG_VIDEO_I2C_RETRY_NUM + 1;
	int ret = -EIO;

	while (attempts-- > 0) {
		ret = i2c_write_read_dt(spec, addr_buf, sizeof(addr_buf), value, 1);
		if (ret == 0) {
			return 0;
		}
		k_msleep(5);
	}

	return ret;
}

static int ov3660_write_reg(const struct i2c_dt_spec *spec, uint16_t reg, uint8_t value)
{
	uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xff), value};
	int attempts = CONFIG_VIDEO_I2C_RETRY_NUM + 1;
	int ret = -EIO;

	while (attempts-- > 0) {
		ret = i2c_write_dt(spec, buf, sizeof(buf));
		if (ret == 0) {
			return 0;
		}
		k_msleep(5);
	}

	return ret;
}

static int ov3660_write_mask(const struct i2c_dt_spec *spec, uint16_t reg, uint8_t mask, bool enable)
{
	uint8_t value;
	int ret = ov3660_read_reg(spec, reg, &value);

	if (ret) {
		return ret;
	}

	value = enable ? (value | mask) : (value & ~mask);
	return ov3660_write_reg(spec, reg, value);
}

static int ov3660_write_addr_pair(const struct i2c_dt_spec *spec, uint16_t reg, uint16_t x_value,
				  uint16_t y_value)
{
	int ret;

	ret = ov3660_write_reg(spec, reg, (uint8_t)(x_value >> 8));
	ret |= ov3660_write_reg(spec, reg + 1, (uint8_t)(x_value & 0xff));
	ret |= ov3660_write_reg(spec, reg + 2, (uint8_t)(y_value >> 8));
	ret |= ov3660_write_reg(spec, reg + 3, (uint8_t)(y_value & 0xff));

	return ret;
}

static int ov3660_write_table(const struct i2c_dt_spec *spec, const struct ov3660_reg *regs,
			      size_t count)
{
	for (size_t index = 0; index < count; index++) {
		int ret = ov3660_write_reg(spec, regs[index].addr, regs[index].value);

		if (ret) {
			LOG_ERR("reg 0x%04x write failed: %d", regs[index].addr, ret);
			return ret;
		}
	}

	return 0;
}

static int ov3660_check_connection(const struct device *dev)
{
	const struct ov3660_config *cfg = dev->config;
	uint8_t high = 0;
	uint8_t low = 0;
	int ret;

	ret = ov3660_read_reg(&cfg->i2c, OV3660_CHIP_ID_REG_H, &high);
	ret |= ov3660_read_reg(&cfg->i2c, OV3660_CHIP_ID_REG_L, &low);
	if (ret) {
		return ret;
	}

	if ((((uint16_t)high << 8) | low) != OV3660_CHIP_ID) {
		LOG_ERR("unexpected chip id: 0x%02x%02x", high, low);
		return -ENODEV;
	}

	LOG_INF("OV3660 detected");
	return 0;
}

static int ov3660_power_up(const struct device *dev)
{
	const struct ov3660_config *cfg = dev->config;

#if DT_INST_NODE_HAS_PROP(0, powerdown_gpios)
	int ret;

	ret = gpio_pin_configure_dt(&cfg->powerdown_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		return ret;
	}

	gpio_pin_set_dt(&cfg->powerdown_gpio, 0);
	k_msleep(2);
#endif

#if DT_INST_NODE_HAS_PROP(0, reset_gpios)
	int ret;

	ret = gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_OUTPUT_ACTIVE);
	if (ret) {
		return ret;
	}

	gpio_pin_set_dt(&cfg->reset_gpio, 0);
	k_msleep(2);
	gpio_pin_set_dt(&cfg->reset_gpio, 1);
	k_msleep(2);
#endif

	ARG_UNUSED(cfg);

	return 0;
}

static int ov3660_configure_qvga_rgb565(const struct device *dev)
{
	const struct ov3660_config *cfg = dev->config;
	int ret = 0;

	ret |= ov3660_write_table(&cfg->i2c, ov3660_rgb565_regs, ARRAY_SIZE(ov3660_rgb565_regs));
	ret |= ov3660_write_addr_pair(&cfg->i2c, OV3660_X_ADDR_ST_H, 0, 0);
	ret |= ov3660_write_addr_pair(&cfg->i2c, OV3660_X_ADDR_END_H, 2079, 1547);
	ret |= ov3660_write_addr_pair(&cfg->i2c, OV3660_X_OUTPUT_SIZE_H, 320, 240);
	ret |= ov3660_write_addr_pair(&cfg->i2c, OV3660_X_TOTAL_SIZE_H, 2300, 783);
	ret |= ov3660_write_addr_pair(&cfg->i2c, OV3660_X_OFFSET_H, 8, 2);
	ret |= ov3660_write_mask(&cfg->i2c, OV3660_ISP_CONTROL_01, 0x20, true);
	ret |= ov3660_write_reg(&cfg->i2c, OV3660_TIMING_TC_REG20, 0x01);
	ret |= ov3660_write_reg(&cfg->i2c, OV3660_TIMING_TC_REG21, 0x01);
	ret |= ov3660_write_reg(&cfg->i2c, 0x4514, 0xaa);
	ret |= ov3660_write_reg(&cfg->i2c, 0x4520, 0x0b);
	ret |= ov3660_write_reg(&cfg->i2c, OV3660_X_INCREMENT, 0x31);
	ret |= ov3660_write_reg(&cfg->i2c, OV3660_Y_INCREMENT, 0x31);
	ret |= ov3660_write_reg(&cfg->i2c, OV3660_SC_PLLS_CTRL0, 0x00);
	ret |= ov3660_write_reg(&cfg->i2c, OV3660_SC_PLLS_CTRL1, 0x08);
	ret |= ov3660_write_reg(&cfg->i2c, OV3660_SC_PLLS_CTRL2, 0x11);
	ret |= ov3660_write_reg(&cfg->i2c, OV3660_SC_PLLS_CTRL3, 0x02);
	ret |= ov3660_write_reg(&cfg->i2c, OV3660_PCLK_RATIO, 0x04);
	ret |= ov3660_write_reg(&cfg->i2c, OV3660_VFIFO_CTRL0C, 0x22);

	if (ret) {
		LOG_ERR("failed to configure QVGA RGB565 mode");
	}

	return ret;
}

static int ov3660_ensure_initialized(const struct device *dev)
{
	const struct ov3660_config *cfg = dev->config;
	struct ov3660_data *data = dev->data;
	int ret;

	if (data->initialized) {
		return 0;
	}

	ret = ov3660_power_up(dev);
	if (ret) {
		return ret;
	}

	ret = ov3660_check_connection(dev);
	if (ret) {
		return ret;
	}

	ret = ov3660_write_table(&cfg->i2c, ov3660_default_regs, ARRAY_SIZE(ov3660_default_regs));
	if (ret) {
		return ret;
	}

	k_msleep(30);
	ret = ov3660_configure_qvga_rgb565(dev);
	if (ret) {
		return ret;
	}

	data->initialized = true;
	data->fmt = (struct video_format){
		.type = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_RGB565,
		.width = 320,
		.height = 240,
		.pitch = 320 * 2,
	};

	LOG_INF("OV3660 initialized for RGB565 320x240");
	return 0;
}

static int ov3660_set_fmt(const struct device *dev, struct video_format *fmt)
{
	struct ov3660_data *data = dev->data;
	int ret;

	if (fmt->pixelformat != VIDEO_PIX_FMT_RGB565 || fmt->width != 320 || fmt->height != 240) {
		LOG_ERR("unsupported format %s %ux%u", VIDEO_FOURCC_TO_STR(fmt->pixelformat),
			fmt->width, fmt->height);
		return -ENOTSUP;
	}

	ret = ov3660_ensure_initialized(dev);
	if (ret) {
		return ret;
	}

	fmt->type = VIDEO_BUF_TYPE_OUTPUT;
	fmt->pitch = fmt->width * 2;
	data->fmt = *fmt;
	return 0;
}

static int ov3660_get_fmt(const struct device *dev, struct video_format *fmt)
{
	const struct ov3660_data *data = dev->data;

	*fmt = data->fmt;
	return 0;
}

static int ov3660_get_caps(const struct device *dev, struct video_caps *caps)
{
	ARG_UNUSED(dev);

	caps->type = VIDEO_BUF_TYPE_OUTPUT;
	caps->format_caps = ov3660_fmts;
	caps->min_vbuf_count = 1;
	caps->min_line_count = LINE_COUNT_HEIGHT;
	caps->max_line_count = LINE_COUNT_HEIGHT;
	return 0;
}

static int ov3660_set_stream(const struct device *dev, bool enable, enum video_buf_type type)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(enable);
	ARG_UNUSED(type);
	return 0;
}

static DEVICE_API(video, ov3660_driver_api) = {
	.set_format = ov3660_set_fmt,
	.get_format = ov3660_get_fmt,
	.get_caps = ov3660_get_caps,
	.set_stream = ov3660_set_stream,
};

static int ov3660_init(const struct device *dev)
{
	const struct ov3660_config *cfg = dev->config;
	struct ov3660_data *data = dev->data;

	if (!device_is_ready(cfg->i2c.bus)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

#if DT_INST_NODE_HAS_PROP(0, reset_gpios)
	if (!gpio_is_ready_dt(&cfg->reset_gpio)) {
		LOG_ERR("reset GPIO not ready");
		return -ENODEV;
	}
#endif

#if DT_INST_NODE_HAS_PROP(0, powerdown_gpios)
	if (!gpio_is_ready_dt(&cfg->powerdown_gpio)) {
		LOG_ERR("powerdown GPIO not ready");
		return -ENODEV;
	}
#endif

	data->initialized = false;
	data->fmt = (struct video_format){
		.type = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_RGB565,
		.width = 320,
		.height = 240,
		.pitch = 320 * 2,
	};

	return 0;
}

static const struct ov3660_config ov3660_cfg_0 = {
	.i2c = I2C_DT_SPEC_INST_GET(0),
#if DT_INST_NODE_HAS_PROP(0, reset_gpios)
	.reset_gpio = GPIO_DT_SPEC_INST_GET(0, reset_gpios),
#endif
#if DT_INST_NODE_HAS_PROP(0, powerdown_gpios)
	.powerdown_gpio = GPIO_DT_SPEC_INST_GET(0, powerdown_gpios),
#endif
};

static struct ov3660_data ov3660_data_0;

DEVICE_DT_INST_DEFINE(0, ov3660_init, NULL, &ov3660_data_0, &ov3660_cfg_0,
			      POST_KERNEL, CONFIG_VIDEO_INIT_PRIORITY, &ov3660_driver_api);

VIDEO_DEVICE_DEFINE(ov3660_0, DEVICE_DT_INST_GET(0), NULL);