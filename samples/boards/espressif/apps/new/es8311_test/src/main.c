/*
 * CHD-ESP32-S3-BOX: ES8311 Audio DAC Test (Module 7)
 *
 * Initializes ES8311 codec via I2C, enables PA on IO46,
 * then plays a 1kHz sine wave through I2S → ES8311 → speaker.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/gpio.h>

/* ---------- ES8311 I2C address ---------- */
#define ES8311_ADDR  0x18

/* ---------- ES8311 Register Map ---------- */
#define ES8311_RESET_REG00         0x00
#define ES8311_CLK_MANAGER_REG01   0x01
#define ES8311_CLK_MANAGER_REG02   0x02
#define ES8311_CLK_MANAGER_REG03   0x03
#define ES8311_CLK_MANAGER_REG04   0x04
#define ES8311_CLK_MANAGER_REG05   0x05
#define ES8311_CLK_MANAGER_REG06   0x06
#define ES8311_CLK_MANAGER_REG07   0x07
#define ES8311_CLK_MANAGER_REG08   0x08
#define ES8311_SDPIN_REG09         0x09
#define ES8311_SDPOUT_REG0A        0x0A
#define ES8311_SYSTEM_REG0D        0x0D
#define ES8311_SYSTEM_REG0E        0x0E
#define ES8311_SYSTEM_REG12        0x12
#define ES8311_SYSTEM_REG13        0x13
#define ES8311_SYSTEM_REG14        0x14
#define ES8311_ADC_REG1C           0x1C
#define ES8311_DAC_REG31           0x31
#define ES8311_DAC_REG32           0x32
#define ES8311_DAC_REG37           0x37
#define ES8311_CHIPID1_REG_FD      0xFD
#define ES8311_CHIPID2_REG_FE      0xFE
#define ES8311_CHIP_VER_REG_FF     0xFF

/* ---------- I2S Config ---------- */
#define I2S_TX_NODE      DT_NODELABEL(i2s_rxtx)
#define SAMPLE_RATE      16000
#define SAMPLE_BITS      16
#define NUM_CHANNELS     2
#define BYTES_PER_SAMPLE sizeof(int16_t)
#define SAMPLES_PER_BLOCK ((SAMPLE_RATE / 100) * NUM_CHANNELS)
#define BLOCK_SIZE        (SAMPLES_PER_BLOCK * BYTES_PER_SAMPLE)
#define BLOCK_COUNT       6

K_MEM_SLAB_DEFINE_STATIC(tx_mem_slab, BLOCK_SIZE, BLOCK_COUNT, 4);

/* ---------- PA GPIO ---------- */
#define PA_NODE DT_NODELABEL(pa_gpio)
static const struct gpio_dt_spec pa_gpio = GPIO_DT_SPEC_GET(PA_NODE, gpios);

/* ---------- 1kHz sine table (one period at 16kHz = 16 samples) ---------- */
#define SINE_TABLE_LEN 16
static const int16_t sine_table[SINE_TABLE_LEN] = {
	0, 12539, 23170, 30273, 32767, 30273, 23170, 12539,
	0, -12539, -23170, -30273, -32767, -30273, -23170, -12539
};

/* ---------- ES8311 I2C helpers ---------- */
static const struct device *i2c_dev;

static int es8311_write_reg(uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = {reg, val};
	return i2c_write(i2c_dev, buf, 2, ES8311_ADDR);
}

static int es8311_read_reg(uint8_t reg, uint8_t *val)
{
	return i2c_write_read(i2c_dev, ES8311_ADDR, &reg, 1, val, 1);
}

/* ---------- ES8311 init sequence ---------- */
/* Config: MCLK from MCLK pin @4096000Hz, fs=16000Hz, 16-bit I2S slave */
/* Coeff table entry {4096000, 16000}:                                  */
/*   pre_div=1, pre_multi=1x, adc_div=1, dac_div=1, fs_mode=SS,       */
/*   lrck_h=0x00, lrck_l=0xFF, bclk_div=4, adc_osr=0x10, dac_osr=0x10 */
static int es8311_init(void)
{
	int ret;

	/* 1. Reset codec */
	ret = es8311_write_reg(ES8311_RESET_REG00, 0x1F);
	if (ret) return ret;
	k_msleep(20);
	ret = es8311_write_reg(ES8311_RESET_REG00, 0x00);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_RESET_REG00, 0x80);
	if (ret) return ret;
	k_msleep(5);

	/* 2. Clock config — MCLK from MCLK pin, enable all clocks */
	ret = es8311_write_reg(ES8311_CLK_MANAGER_REG01, 0x3F);
	if (ret) return ret;

	/* Clock dividers for MCLK=4096000, fs=16000 */
	ret = es8311_write_reg(ES8311_CLK_MANAGER_REG02, 0x00);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_CLK_MANAGER_REG03, 0x10);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_CLK_MANAGER_REG04, 0x10);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_CLK_MANAGER_REG05, 0x00);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_CLK_MANAGER_REG06, 0x03);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_CLK_MANAGER_REG07, 0x00);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_CLK_MANAGER_REG08, 0xFF);
	if (ret) return ret;

	/* 3. I2S format: slave mode, 16-bit */
	ret = es8311_write_reg(ES8311_SDPIN_REG09, 0x0C);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_SDPOUT_REG0A, 0x0C);
	if (ret) return ret;

	/* 4. Power up analog circuitry */
	ret = es8311_write_reg(ES8311_SYSTEM_REG0D, 0x01);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_SYSTEM_REG0E, 0x02);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_SYSTEM_REG12, 0x00);
	if (ret) return ret;
	/* Enable output to HP drive */
	ret = es8311_write_reg(ES8311_SYSTEM_REG13, 0x10);
	if (ret) return ret;

	/* 5. ADC EQ bypass, cancel DC offset */
	ret = es8311_write_reg(ES8311_ADC_REG1C, 0x6A);
	if (ret) return ret;

	/* 6. Bypass DAC equalizer */
	ret = es8311_write_reg(ES8311_DAC_REG37, 0x08);
	if (ret) return ret;

	/* 7. Set DAC volume ~80% */
	ret = es8311_write_reg(ES8311_DAC_REG32, 0xC0);
	if (ret) return ret;

	/* 8. Unmute DAC */
	ret = es8311_write_reg(ES8311_DAC_REG31, 0x00);
	if (ret) return ret;

	return 0;
}

/* ---------- Fill I2S buffer with sine wave ---------- */
static void fill_sine_block(int16_t *buf, int num_samples, int *phase)
{
	for (int i = 0; i < num_samples; i += NUM_CHANNELS) {
		int16_t val = sine_table[*phase % SINE_TABLE_LEN];
		buf[i] = val;         /* Left */
		buf[i + 1] = val;    /* Right (mono mirror) */
		(*phase)++;
		if (*phase >= SINE_TABLE_LEN) {
			*phase = 0;
		}
	}
}

int main(void)
{
	int ret;
	uint8_t chip_id1, chip_id2, chip_ver;

	printk("=== ES8311 Audio DAC Test ===\n");

	/* ---- 1. Get I2C device ---- */
	i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
	if (!device_is_ready(i2c_dev)) {
		printk("ERROR: I2C device not ready\n");
		return 0;
	}
	printk("I2C ready: %s\n", i2c_dev->name);

	/* ---- 2. Read ES8311 Chip ID ---- */
	ret = es8311_read_reg(ES8311_CHIPID1_REG_FD, &chip_id1);
	ret |= es8311_read_reg(ES8311_CHIPID2_REG_FE, &chip_id2);
	ret |= es8311_read_reg(ES8311_CHIP_VER_REG_FF, &chip_ver);
	if (ret) {
		printk("ERROR: Failed to read ES8311 chip ID (I2C err %d)\n", ret);
		return 0;
	}
	printk("ES8311 Chip ID: 0x%02X 0x%02X  Version: 0x%02X\n",
	       chip_id1, chip_id2, chip_ver);

	/* ---- 3. Initialize ES8311 ---- */
	ret = es8311_init();
	if (ret) {
		printk("ERROR: ES8311 init failed: %d\n", ret);
		return 0;
	}
	printk("ES8311 initialized (16kHz, 16-bit, MCLK=4.096MHz)\n");

	/* ---- 4. Enable PA (IO46) ---- */
	if (!gpio_is_ready_dt(&pa_gpio)) {
		printk("ERROR: PA GPIO not ready\n");
		return 0;
	}
	ret = gpio_pin_configure_dt(&pa_gpio, GPIO_OUTPUT_ACTIVE);
	if (ret) {
		printk("ERROR: PA GPIO config failed: %d\n", ret);
		return 0;
	}
	gpio_pin_set_dt(&pa_gpio, 1);
	printk("PA enabled (IO46 HIGH)\n");

	/* ---- 5. Configure I2S TX ---- */
	const struct device *i2s_dev = DEVICE_DT_GET(I2S_TX_NODE);
	if (!device_is_ready(i2s_dev)) {
		printk("ERROR: I2S device not ready\n");
		return 0;
	}

	struct i2s_config i2s_cfg = {
		.word_size = SAMPLE_BITS,
		.channels = NUM_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = SAMPLE_RATE,
		.mem_slab = &tx_mem_slab,
		.block_size = BLOCK_SIZE,
		.timeout = 1000,
	};

	ret = i2s_configure(i2s_dev, I2S_DIR_TX, &i2s_cfg);
	if (ret) {
		printk("ERROR: I2S TX configure failed: %d\n", ret);
		return 0;
	}
	printk("I2S TX configured: %dHz, %d-bit, %d-ch\n",
	       SAMPLE_RATE, SAMPLE_BITS, NUM_CHANNELS);

	/* Pre-fill 2 blocks */
	int phase = 0;
	for (int i = 0; i < 2; i++) {
		void *mem_block;
		ret = k_mem_slab_alloc(&tx_mem_slab, &mem_block, K_NO_WAIT);
		if (ret) {
			printk("ERROR: Block alloc failed\n");
			return 0;
		}
		fill_sine_block(mem_block, SAMPLES_PER_BLOCK, &phase);
		ret = i2s_write(i2s_dev, mem_block, BLOCK_SIZE);
		if (ret) {
			printk("ERROR: I2S write failed\n");
			return 0;
		}
	}

	/* Start I2S TX */
	ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret) {
		printk("ERROR: I2S start failed: %d\n", ret);
		return 0;
	}
	printk(">>> Playing 1kHz sine wave through speaker <<<\n");

	/* ---- 6. Play for 3 seconds then stop ---- */
	uint32_t blocks = 2;
	/* 3 seconds = 3000ms; each block = 10ms → 300 blocks total */
	const uint32_t total_blocks = 300;
	while (blocks < total_blocks) {
		void *mem_block;
		ret = k_mem_slab_alloc(&tx_mem_slab, &mem_block, K_MSEC(500));
		if (ret) {
			printk("WARN: alloc timeout\n");
			continue;
		}
		fill_sine_block(mem_block, SAMPLES_PER_BLOCK, &phase);
		ret = i2s_write(i2s_dev, mem_block, BLOCK_SIZE);
		if (ret) {
			printk("WARN: write err %d\n", ret);
			k_mem_slab_free(&tx_mem_slab, mem_block);
			continue;
		}
		blocks++;
	}

	/* Stop I2S and disable PA */
	i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	gpio_pin_set_dt(&pa_gpio, 0);
	printk("Playback finished: %u blocks (~3s)\n", blocks);
	printk("PA disabled. Test PASSED.\n");

	while (1) {
		k_msleep(1000);
	}

	return 0;
}
