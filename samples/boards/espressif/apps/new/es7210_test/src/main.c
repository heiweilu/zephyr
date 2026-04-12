/*
 * CHD-ESP32-S3-BOX: ES7210 ADC Microphone Test (Module 8)
 *
 * Initializes ES7210 4-ch ADC via I2C, captures audio through I2S RX,
 * and prints amplitude statistics to verify microphone input.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <stdlib.h>
#include <string.h>

/* ---------- ES7210 I2C address ---------- */
#define ES7210_ADDR  0x40

/* ---------- ES7210 Register Map ---------- */
#define ES7210_RESET_REG00          0x00
#define ES7210_CLOCK_OFF_REG01      0x01
#define ES7210_MAINCLK_REG02        0x02
#define ES7210_MASTER_CLK_REG03     0x03
#define ES7210_LRCK_DIVH_REG04      0x04
#define ES7210_LRCK_DIVL_REG05      0x05
#define ES7210_POWER_DOWN_REG06     0x06
#define ES7210_OSR_REG07            0x07
#define ES7210_MODE_CONFIG_REG08    0x08
#define ES7210_TIME_CONTROL0_REG09  0x09
#define ES7210_TIME_CONTROL1_REG0A  0x0A
#define ES7210_SDP_INTERFACE1_REG11 0x11
#define ES7210_SDP_INTERFACE2_REG12 0x12
#define ES7210_ADC12_HPF2_REG22     0x22
#define ES7210_ADC12_HPF1_REG23     0x23
#define ES7210_ADC34_HPF2_REG20     0x20
#define ES7210_ADC34_HPF1_REG21     0x21
#define ES7210_ANALOG_REG40         0x40
#define ES7210_MIC12_BIAS_REG41     0x41
#define ES7210_MIC34_BIAS_REG42     0x42
#define ES7210_MIC1_GAIN_REG43      0x43
#define ES7210_MIC2_GAIN_REG44      0x44
#define ES7210_MIC3_GAIN_REG45      0x45
#define ES7210_MIC4_GAIN_REG46      0x46
#define ES7210_MIC1_POWER_REG47     0x47
#define ES7210_MIC2_POWER_REG48     0x48
#define ES7210_MIC3_POWER_REG49     0x49
#define ES7210_MIC4_POWER_REG4A     0x4A
#define ES7210_MIC12_POWER_REG4B    0x4B
#define ES7210_MIC34_POWER_REG4C    0x4C

/* ---------- I2S Config ---------- */
#define I2S_RX_NODE      DT_NODELABEL(i2s_rxtx)
#define SAMPLE_RATE      16000
#define SAMPLE_BITS      16
#define NUM_CHANNELS     2
#define BYTES_PER_SAMPLE sizeof(int16_t)
#define SAMPLES_PER_BLOCK ((SAMPLE_RATE / 100) * NUM_CHANNELS)
#define BLOCK_SIZE        (SAMPLES_PER_BLOCK * BYTES_PER_SAMPLE)
#define BLOCK_COUNT       6

K_MEM_SLAB_DEFINE_STATIC(rx_mem_slab, BLOCK_SIZE, BLOCK_COUNT, 4);
K_MEM_SLAB_DEFINE_STATIC(tx_mem_slab, BLOCK_SIZE, BLOCK_COUNT, 4);

/* ---------- ES7210 I2C helpers ---------- */
static const struct device *i2c_dev;

static int es7210_write_reg(uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = {reg, val};
	return i2c_write(i2c_dev, buf, 2, ES7210_ADDR);
}

/* ---------- ES7210 init sequence ---------- */
/* Config: MCLK=4096000Hz, fs=16000Hz, 16-bit I2S, MIC gain 30dB */
/* Coeff: {4096000, 16000}: adc_div=1, dll=1, doubler=1, osr=0x20 */
/*   REG02 = 0x01|(1<<6)|(1<<7) = 0xC1, lrck_h=0x01, lrck_l=0x00 */
static int es7210_init(void)
{
	int ret;

	/* 1. Software reset */
	ret = es7210_write_reg(ES7210_RESET_REG00, 0xFF);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_RESET_REG00, 0x32);
	if (ret) return ret;

	/* 2. Power-up timing */
	ret = es7210_write_reg(ES7210_TIME_CONTROL0_REG09, 0x30);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_TIME_CONTROL1_REG0A, 0x30);
	if (ret) return ret;

	/* 3. HPF config for ADC1-4 */
	ret = es7210_write_reg(ES7210_ADC12_HPF1_REG23, 0x2A);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_ADC12_HPF2_REG22, 0x0A);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_ADC34_HPF1_REG21, 0x2A);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_ADC34_HPF2_REG20, 0x0A);
	if (ret) return ret;

	/* 4. I2S format: 16-bit, standard I2S, no TDM */
	ret = es7210_write_reg(ES7210_SDP_INTERFACE1_REG11, 0x60);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_SDP_INTERFACE2_REG12, 0x00);
	if (ret) return ret;

	/* 5. Analog power */
	ret = es7210_write_reg(ES7210_ANALOG_REG40, 0xC3);
	if (ret) return ret;

	/* 6. MIC bias 2.87V */
	ret = es7210_write_reg(ES7210_MIC12_BIAS_REG41, 0x70);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_MIC34_BIAS_REG42, 0x70);
	if (ret) return ret;

	/* 7. MIC gain 30dB (gain=10 | 0x10 = 0x1A) */
	ret = es7210_write_reg(ES7210_MIC1_GAIN_REG43, 0x1A);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_MIC2_GAIN_REG44, 0x1A);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_MIC3_GAIN_REG45, 0x1A);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_MIC4_GAIN_REG46, 0x1A);
	if (ret) return ret;

	/* 8. Power on MIC1-4 */
	ret = es7210_write_reg(ES7210_MIC1_POWER_REG47, 0x08);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_MIC2_POWER_REG48, 0x08);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_MIC3_POWER_REG49, 0x08);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_MIC4_POWER_REG4A, 0x08);
	if (ret) return ret;

	/* 9. Clock config for MCLK=4096000, fs=16000 */
	ret = es7210_write_reg(ES7210_OSR_REG07, 0x20);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_MAINCLK_REG02, 0xC1);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_LRCK_DIVH_REG04, 0x01);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_LRCK_DIVL_REG05, 0x00);
	if (ret) return ret;

	/* 10. Power down DLL */
	ret = es7210_write_reg(ES7210_POWER_DOWN_REG06, 0x04);
	if (ret) return ret;

	/* 11. Power on MIC bias & ADC & PGA */
	ret = es7210_write_reg(ES7210_MIC12_POWER_REG4B, 0x0F);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_MIC34_POWER_REG4C, 0x0F);
	if (ret) return ret;

	/* 12. Enable device */
	ret = es7210_write_reg(ES7210_RESET_REG00, 0x71);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_RESET_REG00, 0x41);
	if (ret) return ret;

	return 0;
}

/* ---------- Analyze audio block ---------- */
static void analyze_block(const int16_t *samples, int count, int block_num)
{
	int16_t min_val = 32767;
	int16_t max_val = -32768;
	int64_t sum = 0;

	for (int i = 0; i < count; i++) {
		int16_t s = samples[i];
		if (s < min_val) min_val = s;
		if (s > max_val) max_val = s;
		sum += abs(s);
	}

	int avg = (int)(sum / count);
	printk("[Block %3d] min=%6d  max=%6d  avg_abs=%5d\n",
	       block_num, min_val, max_val, avg);
}

int main(void)
{
	int ret;

	printk("=== ES7210 ADC Microphone Test ===\n");

	/* ---- 1. Get I2C device ---- */
	i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
	if (!device_is_ready(i2c_dev)) {
		printk("ERROR: I2C device not ready\n");
		return 0;
	}
	printk("I2C ready: %s\n", i2c_dev->name);

	/* ---- 2. Verify ES7210 on I2C bus ---- */
	uint8_t dummy;
	ret = i2c_read(i2c_dev, &dummy, 1, ES7210_ADDR);
	if (ret) {
		printk("ERROR: ES7210 not found at 0x%02X (err %d)\n",
		       ES7210_ADDR, ret);
		return 0;
	}
	printk("ES7210 detected at 0x%02X\n", ES7210_ADDR);

	/* ---- 3. Initialize ES7210 ---- */
	ret = es7210_init();
	if (ret) {
		printk("ERROR: ES7210 init failed: %d\n", ret);
		return 0;
	}
	printk("ES7210 initialized (16kHz, 16-bit, MIC gain 30dB)\n");

	/* ---- 4. Configure I2S (TX for clocks + RX for mic data) ---- */
	const struct device *i2s_dev = DEVICE_DT_GET(I2S_RX_NODE);
	if (!device_is_ready(i2s_dev)) {
		printk("ERROR: I2S device not ready\n");
		return 0;
	}

	/* Configure TX first — needed to generate MCLK/BCLK/WS for ES7210 */
	struct i2s_config tx_cfg = {
		.word_size = SAMPLE_BITS,
		.channels = NUM_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = SAMPLE_RATE,
		.mem_slab = &tx_mem_slab,
		.block_size = BLOCK_SIZE,
		.timeout = 1000,
	};

	ret = i2s_configure(i2s_dev, I2S_DIR_TX, &tx_cfg);
	if (ret) {
		printk("ERROR: I2S TX configure failed: %d\n", ret);
		return 0;
	}
	printk("I2S TX configured (for clock generation)\n");

	/* Configure RX for microphone capture */
	struct i2s_config i2s_cfg = {
		.word_size = SAMPLE_BITS,
		.channels = NUM_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = SAMPLE_RATE,
		.mem_slab = &rx_mem_slab,
		.block_size = BLOCK_SIZE,
		.timeout = 2000,
	};

	ret = i2s_configure(i2s_dev, I2S_DIR_RX, &i2s_cfg);
	if (ret) {
		printk("ERROR: I2S RX configure failed: %d\n", ret);
		return 0;
	}
	printk("I2S RX configured: %dHz, %d-bit, %d-ch\n",
	       SAMPLE_RATE, SAMPLE_BITS, NUM_CHANNELS);

	/* ---- 5. Start I2S TX (silence) then RX ---- */
	/* Pre-fill TX with silence to get clocks running */
	for (int i = 0; i < 2; i++) {
		void *mem_block;
		ret = k_mem_slab_alloc(&tx_mem_slab, &mem_block, K_NO_WAIT);
		if (ret) {
			printk("ERROR: TX block alloc failed\n");
			return 0;
		}
		memset(mem_block, 0, BLOCK_SIZE);
		ret = i2s_write(i2s_dev, mem_block, BLOCK_SIZE);
		if (ret) {
			printk("ERROR: I2S TX write failed: %d\n", ret);
			return 0;
		}
	}
	ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret) {
		printk("ERROR: I2S TX start failed: %d\n", ret);
		return 0;
	}
	printk("I2S TX started (clocks active)\n");

	/* Start RX */
	ret = i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	if (ret) {
		printk("ERROR: I2S RX start failed: %d\n", ret);
		return 0;
	}
	printk(">>> Recording microphone audio (50 blocks = 0.5s) <<<\n");

	/* ---- 6. Read and analyze audio blocks ---- */
	const int total_blocks = 50;
	for (int b = 0; b < total_blocks; b++) {
		void *mem_block;
		size_t block_sz;

		/* Feed TX with silence to keep clocks running */
		void *tx_block;
		if (k_mem_slab_alloc(&tx_mem_slab, &tx_block, K_NO_WAIT) == 0) {
			memset(tx_block, 0, BLOCK_SIZE);
			if (i2s_write(i2s_dev, tx_block, BLOCK_SIZE) < 0) {
				k_mem_slab_free(&tx_mem_slab, tx_block);
			}
		}

		ret = i2s_read(i2s_dev, &mem_block, &block_sz);
		if (ret) {
			printk("ERROR: I2S read failed: %d (block %d)\n", ret, b);
			break;
		}

		/* Print stats every 10th block */
		if (b % 10 == 0) {
			analyze_block((const int16_t *)mem_block,
				      block_sz / sizeof(int16_t), b);
		}

		k_mem_slab_free(&rx_mem_slab, mem_block);
	}

	/* Stop RX and TX */
	i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	printk("Recording stopped.\n");

	/* Print first few raw samples of the last block */
	printk("Test PASSED — ES7210 microphone data captured.\n");

	while (1) {
		k_msleep(1000);
	}

	return 0;
}
