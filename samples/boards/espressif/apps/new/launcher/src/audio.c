/*
 * audio.c — ES8311 DAC + ES7210 ADC initialization, I2S record & playback
 *
 * ES8311 (0x18): DAC for speaker output via I2S TX
 * ES7210 (0x40): ADC for microphone input via I2S RX
 * I2S0: shared bus, master mode (generates MCLK/BCLK/WS)
 * PA: GPIO46 (gpio1 pin 14), controls speaker amplifier
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "audio.h"

LOG_MODULE_REGISTER(audio, LOG_LEVEL_INF);

/* ── ES8311 registers ── */
#define ES8311_ADDR              0x18
#define ES8311_RESET_REG00       0x00
#define ES8311_CLK_MANAGER_REG01 0x01
#define ES8311_CLK_MANAGER_REG02 0x02
#define ES8311_CLK_MANAGER_REG03 0x03
#define ES8311_CLK_MANAGER_REG04 0x04
#define ES8311_CLK_MANAGER_REG05 0x05
#define ES8311_CLK_MANAGER_REG06 0x06
#define ES8311_CLK_MANAGER_REG07 0x07
#define ES8311_CLK_MANAGER_REG08 0x08
#define ES8311_SDPIN_REG09       0x09
#define ES8311_SDPOUT_REG0A      0x0A
#define ES8311_SYSTEM_REG0D      0x0D
#define ES8311_SYSTEM_REG0E      0x0E
#define ES8311_SYSTEM_REG12      0x12
#define ES8311_SYSTEM_REG13      0x13
#define ES8311_ADC_REG16         0x16
#define ES8311_ADC_REG1C         0x1C
#define ES8311_DAC_REG31         0x31
#define ES8311_DAC_REG32         0x32
#define ES8311_DAC_REG37         0x37
#define ES8311_CHIPID1_REG_FD    0xFD
#define ES8311_CHIPID2_REG_FE    0xFE

/* ── ES7210 registers ── */
#define ES7210_ADDR                 0x40
#define ES7210_RESET_REG00          0x00
#define ES7210_CLOCK_OFF_REG01      0x01
#define ES7210_MAINCLK_REG02        0x02
#define ES7210_LRCK_DIVH_REG04      0x04
#define ES7210_LRCK_DIVL_REG05      0x05
#define ES7210_POWER_DOWN_REG06     0x06
#define ES7210_OSR_REG07            0x07
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

/* ── I2S config ── */
#define I2S_NODE          DT_NODELABEL(i2s_rxtx)
#define SAMPLES_PER_BLOCK ((RECORD_SAMPLE_RATE / 100) * NUM_CHANNELS)  /* 320 */
#define BLOCK_SIZE        (SAMPLES_PER_BLOCK * sizeof(int16_t))        /* 640 */
#define BLOCK_COUNT       12

/* I2S DMA slab buffers in PSRAM — prevents DMA race conditions from
 * corrupting DRAM structures (free list, LVGL pool, etc).
 * Slabs are re-initialized before each recording/playback session. */
__attribute__((section(".ext_ram.bss"), aligned(4)))
static uint8_t tx_slab_buf[BLOCK_SIZE * BLOCK_COUNT];
__attribute__((section(".ext_ram.bss"), aligned(4)))
static uint8_t rx_slab_buf[BLOCK_SIZE * BLOCK_COUNT];

static struct k_mem_slab tx_mem_slab;
static struct k_mem_slab rx_mem_slab;

/* ── PA GPIO ── */
static const struct gpio_dt_spec pa_gpio =
	GPIO_DT_SPEC_GET(DT_NODELABEL(pa_enable), gpios);

/* ── Devices ── */
static const struct device *i2c_dev;
static const struct device *i2s_dev;

/* ── I2C helpers ── */
static int es8311_write_reg(uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = {reg, val};
	return i2c_write(i2c_dev, buf, 2, ES8311_ADDR);
}

static int es7210_write_reg(uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = {reg, val};
	return i2c_write(i2c_dev, buf, 2, ES7210_ADDR);
}

/* ── ES8311 init: 16kHz, 16-bit, I2S slave, MCLK=4.096MHz ── */
static int es8311_init(void)
{
	int ret;

	/* Reset — must go through 0x1F → 0x00 → 0x80 sequence */
	ret = es8311_write_reg(ES8311_RESET_REG00, 0x1F);
	if (ret) return ret;
	k_msleep(20);
	ret = es8311_write_reg(ES8311_RESET_REG00, 0x00);
	if (ret) return ret;
	k_msleep(20);
	ret = es8311_write_reg(ES8311_RESET_REG00, 0x80);
	if (ret) return ret;

	/* Clock config — match known-working es8311_test/nes_emulator values
	 * MCLK from external pin, BCLK from MCLK, dividers for 16kHz */
	ret = es8311_write_reg(ES8311_CLK_MANAGER_REG01, 0x3F);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_CLK_MANAGER_REG02, 0x00);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_CLK_MANAGER_REG03, 0x10);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_CLK_MANAGER_REG04, 0x10);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_CLK_MANAGER_REG05, 0x00);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_ADC_REG16, 0x24);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_CLK_MANAGER_REG06, 0x03);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_CLK_MANAGER_REG07, 0x00);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_CLK_MANAGER_REG08, 0xFF);
	if (ret) return ret;

	/* I2S format: slave mode, 16-bit */
	ret = es8311_write_reg(ES8311_SDPIN_REG09, 0x0C);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_SDPOUT_REG0A, 0x0C);
	if (ret) return ret;

	/* Power up analog */
	ret = es8311_write_reg(ES8311_SYSTEM_REG0D, 0x01);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_SYSTEM_REG0E, 0x02);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_SYSTEM_REG12, 0x00);
	if (ret) return ret;
	ret = es8311_write_reg(ES8311_SYSTEM_REG13, 0x10);
	if (ret) return ret;

	/* ADC EQ bypass, cancel DC offset */
	ret = es8311_write_reg(ES8311_ADC_REG1C, 0x6A);
	if (ret) return ret;

	/* Bypass DAC equalizer */
	ret = es8311_write_reg(ES8311_DAC_REG37, 0x08);
	if (ret) return ret;

	/* DAC volume ~80% */
	ret = es8311_write_reg(ES8311_DAC_REG32, 0xC0);
	if (ret) return ret;

	/* Unmute DAC */
	ret = es8311_write_reg(ES8311_DAC_REG31, 0x00);
	if (ret) return ret;

	return 0;
}

/* ── ES7210 init: 16kHz, 16-bit, 4-ch ADC, MIC gain 30dB ── */
static int es7210_init(void)
{
	int ret;

	/* Software reset */
	ret = es7210_write_reg(ES7210_RESET_REG00, 0xFF);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_RESET_REG00, 0x32);
	if (ret) return ret;

	/* Power-up timing */
	ret = es7210_write_reg(ES7210_TIME_CONTROL0_REG09, 0x30);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_TIME_CONTROL1_REG0A, 0x30);
	if (ret) return ret;

	/* HPF config */
	ret = es7210_write_reg(ES7210_ADC12_HPF1_REG23, 0x2A);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_ADC12_HPF2_REG22, 0x0A);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_ADC34_HPF1_REG21, 0x2A);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_ADC34_HPF2_REG20, 0x0A);
	if (ret) return ret;

	/* I2S format: 16-bit, standard I2S */
	ret = es7210_write_reg(ES7210_SDP_INTERFACE1_REG11, 0x60);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_SDP_INTERFACE2_REG12, 0x00);
	if (ret) return ret;

	/* Analog power */
	ret = es7210_write_reg(ES7210_ANALOG_REG40, 0xC3);
	if (ret) return ret;

	/* MIC bias 2.87V */
	ret = es7210_write_reg(ES7210_MIC12_BIAS_REG41, 0x70);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_MIC34_BIAS_REG42, 0x70);
	if (ret) return ret;

	/* MIC gain 30dB */
	ret = es7210_write_reg(ES7210_MIC1_GAIN_REG43, 0x1A);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_MIC2_GAIN_REG44, 0x1A);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_MIC3_GAIN_REG45, 0x1A);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_MIC4_GAIN_REG46, 0x1A);
	if (ret) return ret;

	/* Power on MIC1-4 */
	ret = es7210_write_reg(ES7210_MIC1_POWER_REG47, 0x08);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_MIC2_POWER_REG48, 0x08);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_MIC3_POWER_REG49, 0x08);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_MIC4_POWER_REG4A, 0x08);
	if (ret) return ret;

	/* Clock: MCLK=4096000, fs=16000 */
	ret = es7210_write_reg(ES7210_OSR_REG07, 0x20);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_MAINCLK_REG02, 0xC1);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_LRCK_DIVH_REG04, 0x01);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_LRCK_DIVL_REG05, 0x00);
	if (ret) return ret;

	/* Power down DLL */
	ret = es7210_write_reg(ES7210_POWER_DOWN_REG06, 0x04);
	if (ret) return ret;

	/* Power on MIC bias & ADC & PGA */
	ret = es7210_write_reg(ES7210_MIC12_POWER_REG4B, 0x0F);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_MIC34_POWER_REG4C, 0x0F);
	if (ret) return ret;

	/* Enable device */
	ret = es7210_write_reg(ES7210_RESET_REG00, 0x71);
	if (ret) return ret;
	ret = es7210_write_reg(ES7210_RESET_REG00, 0x41);
	if (ret) return ret;

	return 0;
}

/* ================================================================
 *  Public API
 * ================================================================ */

/* ── I2S configured once flag ── */
static bool i2s_configured;

int audio_codec_init(void)
{
	int ret;

	/* I2C */
	i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
	if (!device_is_ready(i2c_dev)) {
		LOG_ERR("I2C device not ready");
		return -ENODEV;
	}

	/* I2S */
	i2s_dev = DEVICE_DT_GET(I2S_NODE);
	if (!device_is_ready(i2s_dev)) {
		LOG_ERR("I2S device not ready");
		return -ENODEV;
	}

	/* ES8311 DAC */
	ret = es8311_init();
	if (ret) {
		LOG_ERR("ES8311 init failed: %d", ret);
		return ret;
	}
	LOG_INF("ES8311 DAC initialized");

	/* ES7210 ADC */
	ret = es7210_init();
	if (ret) {
		LOG_ERR("ES7210 init failed: %d", ret);
		return ret;
	}
	LOG_INF("ES7210 ADC initialized");

	/* PA GPIO */
	if (!gpio_is_ready_dt(&pa_gpio)) {
		LOG_ERR("PA GPIO not ready");
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&pa_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		LOG_ERR("PA GPIO config failed: %d", ret);
		return ret;
	}

	/* Initialize I2S memory slabs (PSRAM buffers) */
	k_mem_slab_init(&tx_mem_slab, tx_slab_buf, BLOCK_SIZE, BLOCK_COUNT);
	k_mem_slab_init(&rx_mem_slab, rx_slab_buf, BLOCK_SIZE, BLOCK_COUNT);

	/* Configure I2S TX + RX once — both share 16kHz, 16-bit, stereo */
	struct i2s_config tx_cfg = {
		.word_size = SAMPLE_BITS,
		.channels = NUM_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = RECORD_SAMPLE_RATE,
		.mem_slab = &tx_mem_slab,
		.block_size = BLOCK_SIZE,
		.timeout = 1000,
	};
	ret = i2s_configure(i2s_dev, I2S_DIR_TX, &tx_cfg);
	if (ret) {
		LOG_ERR("I2S TX config failed: %d", ret);
		return ret;
	}

	struct i2s_config rx_cfg = {
		.word_size = SAMPLE_BITS,
		.channels = NUM_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = RECORD_SAMPLE_RATE,
		.mem_slab = &rx_mem_slab,
		.block_size = BLOCK_SIZE,
		.timeout = 2000,
	};
	ret = i2s_configure(i2s_dev, I2S_DIR_RX, &rx_cfg);
	if (ret) {
		LOG_ERR("I2S RX config failed: %d", ret);
		return ret;
	}

	i2s_configured = true;
	LOG_INF("Audio codecs ready (16kHz, 16-bit)");
	return 0;
}

void audio_pa_enable(int on)
{
	gpio_pin_set_dt(&pa_gpio, on ? 1 : 0);
}

int audio_record(int16_t *pcm_buf, int max_samples, volatile bool *stop_flag)
{
	int ret;
	int mono_idx = 0;

	/* Force I2S to NOT_READY state regardless of current state.
	 * DROP is valid from RUNNING, STOPPING, READY, ERROR — only
	 * NOT_READY is rejected, which we silently ignore. */
	i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	k_msleep(100);

	/* Reinitialize mem slabs with fresh PSRAM buffers — this repairs any
	 * DMA race conditions that corrupted the free list during previous use */
	k_mem_slab_init(&tx_mem_slab, tx_slab_buf, BLOCK_SIZE, BLOCK_COUNT);
	k_mem_slab_init(&rx_mem_slab, rx_slab_buf, BLOCK_SIZE, BLOCK_COUNT);

	/* Re-configure both directions (from NOT_READY → READY) */
	struct i2s_config tx_cfg = {
		.word_size = SAMPLE_BITS,
		.channels = NUM_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = RECORD_SAMPLE_RATE,
		.mem_slab = &tx_mem_slab,
		.block_size = BLOCK_SIZE,
		.timeout = 1000,
	};
	ret = i2s_configure(i2s_dev, I2S_DIR_TX, &tx_cfg);
	if (ret) {
		LOG_ERR("I2S TX config failed: %d", ret);
		return ret;
	}

	struct i2s_config rx_cfg = {
		.word_size = SAMPLE_BITS,
		.channels = NUM_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = RECORD_SAMPLE_RATE,
		.mem_slab = &rx_mem_slab,
		.block_size = BLOCK_SIZE,
		.timeout = 2000,
	};
	ret = i2s_configure(i2s_dev, I2S_DIR_RX, &rx_cfg);
	if (ret) {
		LOG_ERR("I2S RX config failed: %d", ret);
		return ret;
	}

	/* Additional settle time for DMA hardware */
	k_msleep(50);

	/* Pre-fill TX with silence for clock generation */
	for (int i = 0; i < 2; i++) {
		void *blk;
		ret = k_mem_slab_alloc(&tx_mem_slab, &blk, K_NO_WAIT);
		if (ret) {
			LOG_ERR("TX block alloc failed");
			return ret;
		}
		memset(blk, 0, BLOCK_SIZE);
		ret = i2s_write(i2s_dev, blk, BLOCK_SIZE);
		if (ret) {
			LOG_ERR("I2S TX write failed: %d", ret);
			return ret;
		}
	}

	/* Start TX then RX */
	ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret) {
		LOG_ERR("I2S TX start failed: %d", ret);
		return ret;
	}
	ret = i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	if (ret) {
		LOG_ERR("I2S RX start failed: %d", ret);
		return ret;
	}

	LOG_INF("Recording started...");

	/* Read blocks until stop_flag or max_samples reached */
	while (!(*stop_flag) && mono_idx < max_samples) {
		void *rx_blk;
		size_t rx_sz;
		void *tx_blk;

		/* Feed TX with silence to keep clocks running */
		if (k_mem_slab_alloc(&tx_mem_slab, &tx_blk, K_NO_WAIT) == 0) {
			memset(tx_blk, 0, BLOCK_SIZE);
			if (i2s_write(i2s_dev, tx_blk, BLOCK_SIZE) < 0) {
				k_mem_slab_free(&tx_mem_slab, tx_blk);
			}
		}

		/* Read one block of stereo data */
		ret = i2s_read(i2s_dev, &rx_blk, &rx_sz);
		if (ret) {
			LOG_WRN("I2S read error: %d", ret);
			break;
		}

		/* Extract left channel (mono) from interleaved stereo */
		int16_t *stereo = (int16_t *)rx_blk;
		int stereo_samples = rx_sz / sizeof(int16_t);
		for (int i = 0; i < stereo_samples && mono_idx < max_samples; i += 2) {
			pcm_buf[mono_idx++] = stereo[i];  /* left channel */
		}

		k_mem_slab_free(&rx_mem_slab, rx_blk);
	}

	/* Stop I2S */
	i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);

	/* Allow DMA to settle */
	k_msleep(100);

	/* Drain any remaining RX blocks */
	while (1) {
		void *blk;
		size_t sz;
		if (i2s_read(i2s_dev, &blk, &sz) != 0) break;
		k_mem_slab_free(&rx_mem_slab, blk);
	}

	LOG_INF("Recording stopped: %d samples (%.1fs)",
		mono_idx, (float)mono_idx / RECORD_SAMPLE_RATE);

	return mono_idx;
}

int audio_play(const int16_t *pcm_buf, int num_samples)
{
	int ret;

	/* Stop any previous I2S activity */
	i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	k_msleep(100);

	/* Reinit slabs to ensure clean state */
	k_mem_slab_init(&tx_mem_slab, tx_slab_buf, BLOCK_SIZE, BLOCK_COUNT);

	/* Re-configure I2S TX */
	struct i2s_config tx_cfg = {
		.word_size = SAMPLE_BITS,
		.channels = NUM_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = PLAYBACK_SAMPLE_RATE,
		.mem_slab = &tx_mem_slab,
		.block_size = BLOCK_SIZE,
		.timeout = 1000,
	};
	ret = i2s_configure(i2s_dev, I2S_DIR_TX, &tx_cfg);
	if (ret) {
		LOG_ERR("I2S TX reconfig failed: %d", ret);
		return ret;
	}

	audio_pa_enable(1);

	/* Mono samples per block (stereo block has SAMPLES_PER_BLOCK/2 mono) */
	int mono_per_block = SAMPLES_PER_BLOCK / NUM_CHANNELS;  /* 160 */
	int pcm_idx = 0;
	int blocks_written = 0;

	/* Pre-fill 2 blocks */
	for (int pf = 0; pf < 2 && pcm_idx < num_samples; pf++) {
		void *blk;
		ret = k_mem_slab_alloc(&tx_mem_slab, &blk, K_NO_WAIT);
		if (ret) {
			LOG_ERR("TX block alloc failed");
			audio_pa_enable(0);
			return ret;
		}
		int16_t *out = (int16_t *)blk;
		int count = 0;
		while (count < mono_per_block && pcm_idx < num_samples) {
			out[count * 2] = pcm_buf[pcm_idx];      /* Left */
			out[count * 2 + 1] = pcm_buf[pcm_idx];  /* Right */
			pcm_idx++;
			count++;
		}
		/* Zero-fill remainder if not enough samples */
		while (count < mono_per_block) {
			out[count * 2] = 0;
			out[count * 2 + 1] = 0;
			count++;
		}
		ret = i2s_write(i2s_dev, blk, BLOCK_SIZE);
		if (ret) {
			LOG_ERR("I2S write failed");
			k_mem_slab_free(&tx_mem_slab, blk);
			audio_pa_enable(0);
			return ret;
		}
		blocks_written++;
	}

	/* Start TX */
	ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret) {
		LOG_ERR("I2S TX start failed: %d", ret);
		audio_pa_enable(0);
		return ret;
	}

	LOG_INF("Playing %d samples (%.1fs)...",
		num_samples, (float)num_samples / PLAYBACK_SAMPLE_RATE);

	/* Continue feeding blocks */
	while (pcm_idx < num_samples) {
		void *blk;
		ret = k_mem_slab_alloc(&tx_mem_slab, &blk, K_MSEC(500));
		if (ret) {
			LOG_WRN("TX alloc timeout");
			continue;
		}
		int16_t *out = (int16_t *)blk;
		int count = 0;
		while (count < mono_per_block && pcm_idx < num_samples) {
			out[count * 2] = pcm_buf[pcm_idx];
			out[count * 2 + 1] = pcm_buf[pcm_idx];
			pcm_idx++;
			count++;
		}
		while (count < mono_per_block) {
			out[count * 2] = 0;
			out[count * 2 + 1] = 0;
			count++;
		}
		ret = i2s_write(i2s_dev, blk, BLOCK_SIZE);
		if (ret) {
			k_mem_slab_free(&tx_mem_slab, blk);
			break;
		}
		blocks_written++;
	}

	/* Let last blocks drain, then stop */
	k_msleep(200);
	i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	k_msleep(50);
	audio_pa_enable(0);

	LOG_INF("Playback complete: %d blocks", blocks_written);
	return 0;
}

/* ================================================================
 *  Streaming accumulate-then-play
 *
 *  SSE chunks are decoded and resampled in real-time as they arrive
 *  (saving decode time), but playback only starts after all data is
 *  received.  This avoids I2S underrun caused by network delivery
 *  being slower than real-time playback rate.
 * ================================================================ */

static volatile int stream_write_idx;   /* mono samples accumulated so far */
static volatile bool stream_active;     /* accumulation in progress */
static int16_t *stream_buf;            /* pointer to playback_pcm */
static int stream_buf_max;              /* max samples in buffer */

int audio_stream_start(void)
{
	if (stream_active) {
		LOG_WRN("Stream already active");
		return -EBUSY;
	}

	stream_write_idx = 0;
	stream_active = true;

	extern int16_t playback_pcm[];
	stream_buf = playback_pcm;
	stream_buf_max = MAX_PLAYBACK_SAMPLES;

	return 0;
}

int audio_stream_feed(const int16_t *samples, int count)
{
	if (!stream_active) return -EINVAL;

	int space = stream_buf_max - stream_write_idx;
	if (count > space) count = space;
	if (count <= 0) return 0;

	memcpy(&stream_buf[stream_write_idx], samples, count * sizeof(int16_t));
	stream_write_idx += count;

	return count;
}

int audio_stream_stop(void)
{
	if (!stream_active) return 0;

	stream_active = false;

	/* Play all accumulated audio */
	if (stream_write_idx > 0) {
		LOG_INF("Playing %d accumulated samples (%.1fs)",
			stream_write_idx,
			(float)stream_write_idx / PLAYBACK_SAMPLE_RATE);
		audio_play(stream_buf, stream_write_idx);
	}

	/* Reinitialize mem slabs for clean state before next recording */
	k_mem_slab_init(&tx_mem_slab, tx_mem_slab.buffer, BLOCK_SIZE, BLOCK_COUNT);
	k_mem_slab_init(&rx_mem_slab, rx_mem_slab.buffer, BLOCK_SIZE, BLOCK_COUNT);

	return 0;
}

int audio_resample_24k_to_16k(const int16_t *in, int in_samples,
			      int16_t *out, int max_out)
{
	/* 24kHz → 16kHz = ratio 3:2
	 * For every 3 input samples, output 2 using linear interpolation.
	 * Output position i maps to input position i * 1.5
	 */
	int out_samples = (in_samples * 2) / 3;
	if (out_samples > max_out) {
		out_samples = max_out;
	}

	for (int i = 0; i < out_samples; i++) {
		/* Fixed-point: input position = i * 3 / 2 */
		int pos_x2 = i * 3;  /* position * 2 */
		int idx = pos_x2 / 2;
		int frac = pos_x2 % 2;  /* 0 or 1 (fraction * 2) */

		if (idx + 1 < in_samples && frac) {
			/* Linear interpolation: (in[idx] + in[idx+1]) / 2 */
			out[i] = (int16_t)(((int32_t)in[idx] + in[idx + 1]) / 2);
		} else if (idx < in_samples) {
			out[i] = in[idx];
		} else {
			out[i] = 0;
		}
	}

	return out_samples;
}
