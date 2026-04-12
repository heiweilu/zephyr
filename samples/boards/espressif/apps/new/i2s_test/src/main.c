/*
 * CHD-ESP32-S3-BOX: I2S Basic Test
 * Step 6.1 — I2S peripheral init + sine wave TX
 *
 * Outputs a 1kHz sine wave through I2S0 TX.
 * Without ES8311 codec configured, no audible sound,
 * but verifies I2S peripheral and DMA work correctly.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <string.h>
#include <math.h>

#define I2S_TX_NODE     DT_NODELABEL(i2s_rxtx)

#define SAMPLE_RATE     16000
#define SAMPLE_BITS     16
#define NUM_CHANNELS    2
#define BYTES_PER_SAMPLE sizeof(int16_t)

/* 10ms worth of samples per block */
#define SAMPLES_PER_BLOCK ((SAMPLE_RATE / 100) * NUM_CHANNELS)
#define BLOCK_SIZE        (SAMPLES_PER_BLOCK * BYTES_PER_SAMPLE)
#define BLOCK_COUNT       6

K_MEM_SLAB_DEFINE_STATIC(tx_mem_slab, BLOCK_SIZE, BLOCK_COUNT, 4);

/* Pre-computed 1kHz sine wave table (one period at 16kHz = 16 samples) */
#define SINE_TABLE_LEN 16
static const int16_t sine_table[SINE_TABLE_LEN] = {
	0, 12539, 23170, 30273, 32767, 30273, 23170, 12539,
	0, -12539, -23170, -30273, -32767, -30273, -23170, -12539
};

static void fill_sine_block(int16_t *buf, int num_samples, int *phase)
{
	for (int i = 0; i < num_samples; i += NUM_CHANNELS) {
		int16_t val = sine_table[*phase % SINE_TABLE_LEN];
		buf[i] = val;         /* Left channel */
		buf[i + 1] = val;    /* Right channel */
		(*phase)++;
		if (*phase >= SINE_TABLE_LEN) {
			*phase = 0;
		}
	}
}

int main(void)
{
	const struct device *i2s_dev = DEVICE_DT_GET(I2S_TX_NODE);
	struct i2s_config config;
	int ret;
	int phase = 0;

	printk("=== I2S Basic Test ===\n");

	if (!device_is_ready(i2s_dev)) {
		printk("ERROR: I2S device not ready\n");
		return 0;
	}
	printk("I2S device: %s\n", i2s_dev->name);

	/* Configure I2S TX */
	config.word_size = SAMPLE_BITS;
	config.channels = NUM_CHANNELS;
	config.format = I2S_FMT_DATA_FORMAT_I2S;
	config.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER;
	config.frame_clk_freq = SAMPLE_RATE;
	config.mem_slab = &tx_mem_slab;
	config.block_size = BLOCK_SIZE;
	config.timeout = 1000;

	ret = i2s_configure(i2s_dev, I2S_DIR_TX, &config);
	if (ret < 0) {
		printk("ERROR: I2S TX configure failed: %d\n", ret);
		return 0;
	}
	printk("I2S TX configured: %dHz, %d-bit, %d-ch\n",
	       SAMPLE_RATE, SAMPLE_BITS, NUM_CHANNELS);

	/* Pre-fill 2 blocks before starting */
	for (int i = 0; i < 2; i++) {
		void *mem_block;
		ret = k_mem_slab_alloc(&tx_mem_slab, &mem_block, K_NO_WAIT);
		if (ret < 0) {
			printk("ERROR: Block alloc failed: %d\n", ret);
			return 0;
		}
		fill_sine_block(mem_block, SAMPLES_PER_BLOCK, &phase);
		ret = i2s_write(i2s_dev, mem_block, BLOCK_SIZE);
		if (ret < 0) {
			printk("ERROR: I2S write failed: %d\n", ret);
			return 0;
		}
	}

	/* Start TX stream */
	ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0) {
		printk("ERROR: I2S TX start failed: %d\n", ret);
		return 0;
	}
	printk("I2S TX started — streaming 1kHz sine wave\n");

	/* Continuously feed blocks */
	uint32_t blocks_sent = 2;
	while (1) {
		void *mem_block;
		ret = k_mem_slab_alloc(&tx_mem_slab, &mem_block, K_MSEC(500));
		if (ret < 0) {
			printk("WARN: Block alloc timeout\n");
			continue;
		}
		fill_sine_block(mem_block, SAMPLES_PER_BLOCK, &phase);
		ret = i2s_write(i2s_dev, mem_block, BLOCK_SIZE);
		if (ret < 0) {
			printk("ERROR: I2S write failed: %d\n", ret);
			k_mem_slab_free(&tx_mem_slab, mem_block);
			break;
		}
		blocks_sent++;
		if ((blocks_sent % 1000) == 0) {
			printk("I2S TX blocks sent: %u\n", blocks_sent);
		}
	}

	return 0;
}
