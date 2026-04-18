/*
 * NES Emulator OSD (OS-Dependent) layer for Zephyr / ESP32-S3
 *
 * Implements all nofrendo OSD callbacks using:
 *   - Zephyr display API  → ST7789V 320×240 (NES 256×240 centered)
 *   - k_timer             → 60 Hz tick for nofrendo_ticks
 *   - k_malloc            → heap for nofrendo internal allocations
 *
 * ROM loading:  osd_getromdata() returns pointer to INCBIN'd flash data.
 * Audio:        stubbed (Phase 1).
 * Input:        stubbed (Phase 1 — add BLE keyboard mapping in Phase 2).
 *
 * PORTING NOTE — bool/type conflict
 * nofrendo's noftypes.h defines its own bool/uint8/uint16/uint32 types.
 * We include nofrendo headers FIRST so their typedefs are established
 * before Zephyr's stdbool.h (pulled in by kernel.h) redefines 'bool'
 * as a macro.  GCC warns about the redefinition but compiles cleanly.
 */

/* ── nofrendo headers (MUST come before Zephyr / stdbool includes) ──────────*/
#include <noftypes.h>
#include <nofrendo.h>
#include <osd.h>
#include <vid_drv.h>
#include <bitmap.h>
#include <nesinput.h>
#include <nofconfig.h>
#include <event.h>

/*
 * memguard.h (included by noftypes.h) unconditionally redefines malloc, free,
 * and strdup as macros pointing to _my_malloc/_my_free/_my_strdup.
 * Undo these macro definitions here so that picolibc's stdlib.h and
 * Zephyr's kernel.h can be included without parse errors.
 * This is the same technique used internally by memguard.c itself.
 */
#undef malloc
#undef free
#undef strdup

/* ── Zephyr / standard headers ───────────────────────────────────────────────*/
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/init.h>
#include <zephyr/sys/sys_heap.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include "../ble_hid.h"

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  PSRAM heap for nofrendo                                                    */
/*    nofrendo's bmp_create() needs ~60 KB for the 256x240 GUI framebuffer    */
/*    which exceeds CONFIG_HEAP_MEM_POOL_SIZE (16 KB) and we have no DRAM     */
/*    headroom (94.7% used). Provide a 256 KB sys_heap in PSRAM and make     */
/*    nofrendo's malloc/free/strdup (via shadow memguard.h) target it.        */
/* ═══════════════════════════════════════════════════════════════════════════ */
#define NES_PSRAM_HEAP_SIZE (256 * 1024)
static char nes_psram_pool[NES_PSRAM_HEAP_SIZE]
	__attribute__((section(".ext_ram.bss"))) __aligned(8);
static struct sys_heap nes_psram_heap;

static int nes_psram_heap_init(void)
{
	sys_heap_init(&nes_psram_heap, nes_psram_pool, NES_PSRAM_HEAP_SIZE);
	return 0;
}
SYS_INIT(nes_psram_heap_init, APPLICATION, 90);

void *nes_malloc(size_t size)
{
	return sys_heap_alloc(&nes_psram_heap, size);
}

void nes_free(void *p)
{
	if (p) sys_heap_free(&nes_psram_heap, p);
}

char *nes_strdup(const char *s)
{
	if (!s) return NULL;
	size_t n = strlen(s) + 1;
	char *r = sys_heap_alloc(&nes_psram_heap, n);
	if (r) memcpy(r, s, n);
	return r;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  nofrendo log API stubs                                                     */
/*    nofrendo's log.c is excluded from build (its log_init() collides with    */
/*    Zephyr's CONFIG_LOG=y log_init). nofrendo doesn't actually call its own  */
/*    log_init/log_shutdown anywhere, so we don't override Zephyr's version.   */
/*    We only need log_print/log_printf/log_chain_logfunc/log_assert.          */
/* ═══════════════════════════════════════════════════════════════════════════ */

int log_print(const char *s)     { printk("%s", s); return 0; }
int log_printf(const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printk("%s", buf);
	return n;
}
void log_chain_logfunc(int (*fn)(const char *)) { ARG_UNUSED(fn); }
void log_assert(int expr, int line, const char *file, char *msg)
{
	if (!expr) {
		printk("nofrendo ASSERT %s:%d %s\n", file, line,
		       msg ? msg : "");
	}
}


/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Constants                                                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define NES_WIDTH   256
#define NES_HEIGHT  240

/* Backlight: IO47 = GPIO1 pin 15 */
#define BLK_NODE DT_NODELABEL(gpio1)
#define BLK_PIN  15

/*
 * The ST7789V panel is 320 wide.  Centre the 256-pixel NES frame:
 *   left_offset = (320 - 256) / 2 = 32
 */
#define DISPLAY_X_OFFSET  32
#define DISPLAY_WIDTH     320

/* ── Audio: ES8311 via I2C + I2S ─────────────────────────────────────────── */
#define ES8311_ADDR  0x18

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
#define ES8311_ADC_REG1C           0x1C
#define ES8311_DAC_REG31           0x31
#define ES8311_DAC_REG32           0x32
#define ES8311_DAC_REG37           0x37

#define AUDIO_SAMPLE_RATE   22050
#define AUDIO_CHANNELS      2
#define AUDIO_BITS          16
#define AUDIO_BLOCK_MS      20
/* mono samples per block: 22050 / 50 = 441 (exact, no drift) */
#define AUDIO_MONO_SAMPLES  (AUDIO_SAMPLE_RATE / (1000 / AUDIO_BLOCK_MS))
/* stereo samples per block (for I2S: L+R interleaved) */
#define AUDIO_STEREO_SAMPLES (AUDIO_MONO_SAMPLES * AUDIO_CHANNELS)
#define AUDIO_BLOCK_SIZE    (AUDIO_STEREO_SAMPLES * sizeof(int16_t))
#define AUDIO_BLOCK_COUNT   8

#define I2S_TX_NODE   DT_NODELABEL(i2s_rxtx)
/* In launcher_nes the PA-enable GPIO is exposed as nodelabel `pa_enable`
 * (matching the launcher's own audio.c). The original nes_emulator overlay
 * called it `pa_gpio` — keep both names compiling on each variant. */
#define PA_NODE       DT_NODELABEL(pa_enable)

/* Audio TX slab buffer placed in PSRAM (~14 KB) to spare DRAM. */
K_MEM_SLAB_DEFINE_IN_SECT_STATIC(audio_tx_slab,
	Z_GENERIC_SECTION(.ext_ram.bss),
	AUDIO_BLOCK_SIZE, AUDIO_BLOCK_COUNT, 4);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Private state                                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

static const struct device *s_display_dev;

/* ── Audio state ─────────────────────────────────────────────────────────── */
static const struct device *s_i2s_dev;
static void (*volatile s_apu_process)(void *buffer, int num_samples);
static volatile bool s_audio_ready;
static int16_t s_mono_buf[AUDIO_MONO_SAMPLES];

/* k_work + k_timer: timer fires every AUDIO_BLOCK_MS, work handler generates
 * one audio block and pushes it to I2S.  Runs in the system workqueue thread
 * which is independent of the NES emulation main thread.  */
static void audio_work_handler(struct k_work *work);
static K_WORK_DEFINE(audio_work, audio_work_handler);

static void audio_timer_cb(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_work_submit(&audio_work);
}
static K_TIMER_DEFINE(audio_timer, audio_timer_cb, NULL);

/*
 * Large buffers placed in PSRAM (.ext_ram.bss) to avoid overflowing the
 * ESP32-S3's ~320 KB of available DRAM.
 *
 * s_framebuf : RGB565 output (256×240×2 = 120 KB) — written by blit, read
 *              by display_write().
 * s_indexed  : NES 8-bpp palette-indexed pixel data (256×240 = 60 KB) —
 *              the PPU renders into this via bmp_createhw() line pointers.
 *
 * Both are zero-initialised at boot by Zephyr's ESP32-S3 SPIRAM init
 * (see modules/hal/espressif's esp_psram.c → memset(_ext_ram_bss_start…)).
 */
static uint16_t s_framebuf[NES_HEIGHT * NES_WIDTH]
		__attribute__((section(".ext_ram.bss")));
static uint8_t  s_indexed[NES_HEIGHT * NES_WIDTH]
		__attribute__((section(".ext_ram.bss")));

/*
 * 64-entry NES palette in RGB565 (byte-order pre-swapped for ST7789V).
 * The ST7789V expects big-endian RGB565; our little-endian Xtensa CPU
 * stores uint16_t low-byte first, so we swap every entry once at
 * set_palette() time rather than on every pixel blit.
 */
static uint16_t s_palette[64];

/* nofrendo's 8-bpp indexed bitmap — the PPU renders into this */
static bitmap_t *s_bitmap;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  ROM data  (symbols emitted by INCBIN in rom.S)                            */
/* ═══════════════════════════════════════════════════════════════════════════ */

extern const unsigned char nes_rom[];
extern const unsigned char nes_rom_end[];

char *osd_getromdata(void)
{
	return (char *)nes_rom;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  60 Hz timer                                                                */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void (*s_timer_isr)(void);

static void zephyr_timer_cb(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	if (s_timer_isr) {
		s_timer_isr();
	}
}

static K_TIMER_DEFINE(s_nes_timer, zephyr_timer_cb, NULL);

int osd_installtimer(int frequency, void *func, int funcsize,
		     void *counter, int countersize)
{
	ARG_UNUSED(funcsize);
	ARG_UNUSED(counter);
	ARG_UNUSED(countersize);

	s_timer_isr = (void (*)(void))func;
	k_timer_start(&s_nes_timer,
		      K_USEC(1000000 / frequency),
		      K_USEC(1000000 / frequency));
	return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Config stub — no persistent config on embedded target                     */
/* ═══════════════════════════════════════════════════════════════════════════ */

/*
 * nofrendo checks:   if (config.open()) return -1;
 * Returning false (0) means "opened / no error" in nofrendo's convention.
 */
static bool config_open_fn(void)             { return false; }
static void config_close_fn(void)            { }
static int  config_read_int_fn(const char *g, const char *k, int def)
				             { return def; }
static const char *config_read_str_fn(const char *g, const char *k,
				      const char *def)     { return def; }
static void config_write_int_fn(const char *g, const char *k, int v)   { }
static void config_write_str_fn(const char *g, const char *k,
				const char *v)            { }

/* This global is declared extern in nofconfig.h — we provide the definition */
config_t config = {
	.open         = config_open_fn,
	.close        = config_close_fn,
	.read_int     = config_read_int_fn,
	.read_string  = config_read_str_fn,
	.write_int    = config_write_int_fn,
	.write_string = config_write_str_fn,
	.filename     = CONFIG_FILE,
};

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Video driver callbacks                                                     */
/* ═══════════════════════════════════════════════════════════════════════════ */

static int zephyr_vid_init(int width, int height)
{
	/*
	 * s_framebuf and s_indexed are static PSRAM arrays (section
	 * ".ext_ram.bss"), already zero-initialised.  No heap allocation needed
	 * for the pixel data.
	 *
	 * bmp_createhw() allocates only the bitmap_t header + line-pointer
	 * array from the DRAM heap (~width*height*sizeof(ptr) + 20 bytes, i.e.
	 * ~2 KB for 256×240), then wires the line[] pointers into s_indexed.
	 */
	s_bitmap = bmp_createhw((uint8_t *)s_indexed, width, height, width);
	if (!s_bitmap) {
		printk("NES: bmp_createhw failed\n");
		return -1;
	}

	printk("NES: video init %dx%d OK  framebuf=%p indexed=%p\n",
	       width, height, (void *)s_framebuf, (void *)s_indexed);
	return 0;
}

static bitmap_t *zephyr_lock_write(void)
{
	return s_bitmap;
}

static void zephyr_free_write(int num_dirties, rect_t *dirty_rects)
{
	ARG_UNUSED(num_dirties);
	ARG_UNUSED(dirty_rects);
	/* nothing — we keep the single bitmap reference */
}

static void zephyr_set_palette(rgb_t *palette)
{
	for (int i = 0; i < 64; i++) {
		uint8_t r = (uint8_t)(palette[i].r & 0xFF);
		uint8_t g = (uint8_t)(palette[i].g & 0xFF);
		uint8_t b = (uint8_t)(palette[i].b & 0xFF);

		/* Pack into RGB565 */
		uint16_t rgb565 = (uint16_t)(((r >> 3) << 11) |
					     ((g >> 2) << 5)  |
					      (b >> 3));

		/*
		 * ST7789V (big-endian) vs Xtensa little-endian:
		 * Pre-swap bytes so display_write() sends the correct wire order.
		 */
		s_palette[i] = (uint16_t)((rgb565 >> 8) | (rgb565 << 8));
	}
}

static void zephyr_custom_blit(bitmap_t *bitmap, int num_dirties,
			       rect_t *dirty_rects)
{
	ARG_UNUSED(num_dirties);
	ARG_UNUSED(dirty_rects);

	if (!s_display_dev || !bitmap) {
		return;
	}

	/* ── palette-lookup: 8bpp indexed → RGB565 ─────────────────────────── */
	uint16_t *dst = s_framebuf;

      for (int y = 0; y < bitmap->height; y++) {
              const uint8_t *src = bitmap->line[y];

              for (int x = 0; x < bitmap->width; x++) {
                      /* Mask to 6-bit NES palette index (0-63) */
                      *dst++ = s_palette[src[x] & 0x3F];
              }
      }

      /* ── push to display ────────────────────────────────────────────────── */
      struct display_buffer_descriptor buf_desc = {
              .buf_size = (uint32_t)(bitmap->width * bitmap->height * sizeof(uint16_t)),
              .width    = bitmap->width,
              .height   = bitmap->height,
              .pitch    = bitmap->width,
      };

      /* Centre the frame vertically on 240px LCD if height < 240 */
      int y_offset = (240 > bitmap->height) ? (240 - bitmap->height) / 2 : 0;

      display_write(s_display_dev, DISPLAY_X_OFFSET, y_offset, &buf_desc,
                    s_framebuf);

      /* Audio runs via k_timer + k_work — nothing to do here */
}

static viddriver_t s_zephyr_driver = {
      .name        = "Zephyr ST7789V",
      .init        = zephyr_vid_init,
      .shutdown    = NULL,
      .set_mode    = NULL,
      .set_palette = zephyr_set_palette,
      .clear       = NULL,
      .lock_write  = zephyr_lock_write,
      .free_write  = zephyr_free_write,
      .custom_blit = zephyr_custom_blit,
      .invalidate  = false,
};

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Audio: ES8311 DAC + I2S                                                    */
/* ═══════════════════════════════════════════════════════════════════════════ */

static int es8311_write_reg(const struct device *i2c, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = {reg, val};
	return i2c_write(i2c, buf, 2, ES8311_ADDR);
}

static int es8311_codec_init(const struct device *i2c)
{
	int ret;

	/* Reset */
	ret = es8311_write_reg(i2c, ES8311_RESET_REG00, 0x1F);
	if (ret) return ret;
	k_msleep(20);
	es8311_write_reg(i2c, ES8311_RESET_REG00, 0x00);
	es8311_write_reg(i2c, ES8311_RESET_REG00, 0x80);
	k_msleep(5);

	/* Clock: MCLK from pin, enable all */
	es8311_write_reg(i2c, ES8311_CLK_MANAGER_REG01, 0x3F);
	es8311_write_reg(i2c, ES8311_CLK_MANAGER_REG02, 0x00);
	es8311_write_reg(i2c, ES8311_CLK_MANAGER_REG03, 0x10);
	es8311_write_reg(i2c, ES8311_CLK_MANAGER_REG04, 0x10);
	es8311_write_reg(i2c, ES8311_CLK_MANAGER_REG05, 0x00);
	es8311_write_reg(i2c, ES8311_CLK_MANAGER_REG06, 0x03);
	es8311_write_reg(i2c, ES8311_CLK_MANAGER_REG07, 0x00);
	es8311_write_reg(i2c, ES8311_CLK_MANAGER_REG08, 0xFF);

	/* I2S: slave, 16-bit */
	es8311_write_reg(i2c, ES8311_SDPIN_REG09, 0x0C);
	es8311_write_reg(i2c, ES8311_SDPOUT_REG0A, 0x0C);

	/* Power up analog */
	es8311_write_reg(i2c, ES8311_SYSTEM_REG0D, 0x01);
	es8311_write_reg(i2c, ES8311_SYSTEM_REG0E, 0x02);
	es8311_write_reg(i2c, ES8311_SYSTEM_REG12, 0x00);
	es8311_write_reg(i2c, ES8311_SYSTEM_REG13, 0x10);

	/* ADC/DAC settings */
	es8311_write_reg(i2c, ES8311_ADC_REG1C, 0x6A);
	es8311_write_reg(i2c, ES8311_DAC_REG37, 0x08);
	es8311_write_reg(i2c, ES8311_DAC_REG32, 0xC0);
	es8311_write_reg(i2c, ES8311_DAC_REG31, 0x00);

	return 0;
}

static int audio_hw_init(void)
{
	int ret;

	/* I2C — ES8311 codec init */
	const struct device *i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
	if (!device_is_ready(i2c)) {
		printk("NES audio: I2C not ready\n");
		return -1;
	}
	ret = es8311_codec_init(i2c);
	if (ret) {
		printk("NES audio: ES8311 init failed: %d\n", ret);
		return -1;
	}
	printk("NES audio: ES8311 OK\n");

	/* PA enable (IO46 = GPIO1 pin 14) */
	static const struct gpio_dt_spec pa = GPIO_DT_SPEC_GET(PA_NODE, gpios);
	if (gpio_is_ready_dt(&pa)) {
		gpio_pin_configure_dt(&pa, GPIO_OUTPUT_ACTIVE);
		gpio_pin_set_dt(&pa, 1);
		printk("NES audio: PA enabled\n");
	}

	/* I2S TX */
	s_i2s_dev = DEVICE_DT_GET(I2S_TX_NODE);
	if (!device_is_ready(s_i2s_dev)) {
		printk("NES audio: I2S not ready\n");
		return -1;
	}

	struct i2s_config cfg = {
		.word_size   = AUDIO_BITS,
		.channels    = AUDIO_CHANNELS,
		.format      = I2S_FMT_DATA_FORMAT_I2S,
		.options     = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = AUDIO_SAMPLE_RATE,
		.mem_slab    = &audio_tx_slab,
		.block_size  = AUDIO_BLOCK_SIZE,
		.timeout     = 0,       /* non-blocking: work handler must not stall */
	};

	ret = i2s_configure(s_i2s_dev, I2S_DIR_TX, &cfg);
	if (ret) {
		printk("NES audio: I2S config failed: %d\n", ret);
		return -1;
	}
	printk("NES audio: I2S TX %dHz %d-bit %d-ch\n",
	       AUDIO_SAMPLE_RATE, AUDIO_BITS, AUDIO_CHANNELS);

	s_audio_ready = true;

	/* Start the periodic audio timer — first tick deferred until the
	 * APU process function is registered via osd_setsound(). */
	k_timer_start(&audio_timer, K_MSEC(AUDIO_BLOCK_MS),
		      K_MSEC(AUDIO_BLOCK_MS));

	printk("NES audio: ready\n");
	return 0;
}

/*
 * Work handler invoked every AUDIO_BLOCK_MS (10 ms) by audio_timer.
 * Runs in the system workqueue thread — guaranteed CPU time independent
 * of the NES emulation main loop.
 */
static void audio_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	static bool i2s_started;

	if (!s_audio_ready || !s_apu_process) {
		return;
	}

	void *block;
	if (k_mem_slab_alloc(&audio_tx_slab, &block, K_NO_WAIT) != 0) {
		return;  /* all blocks in I2S queue — skip, DMA will free them */
	}

	s_apu_process(s_mono_buf, AUDIO_MONO_SAMPLES);

	int16_t *stereo = (int16_t *)block;
	for (int i = 0; i < AUDIO_MONO_SAMPLES; i++) {
		stereo[i * 2]     = s_mono_buf[i];
		stereo[i * 2 + 1] = s_mono_buf[i];
	}

	int ret = i2s_write(s_i2s_dev, block, AUDIO_BLOCK_SIZE);
	if (ret == -EIO) {
		i2s_trigger(s_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
		ret = i2s_write(s_i2s_dev, block, AUDIO_BLOCK_SIZE);
		if (ret) {
			k_mem_slab_free(&audio_tx_slab, block);
			return;
		}
		i2s_trigger(s_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
		i2s_started = true;
		return;
	} else if (ret) {
		k_mem_slab_free(&audio_tx_slab, block);
		return;
	}

	if (!i2s_started) {
		i2s_trigger(s_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
		i2s_started = true;
	}
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  OSD interface implementations                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

void osd_setsound(void (*playfunc)(void *buffer, int size))
{
	s_apu_process = playfunc;
}

void osd_getvideoinfo(vidinfo_t *info)
{
	info->default_width  = NES_WIDTH;
	info->default_height = NES_HEIGHT;
	info->driver         = &s_zephyr_driver;
}

void osd_getsoundinfo(sndinfo_t *info)
{
	info->sample_rate = AUDIO_SAMPLE_RATE;
	info->bps         = AUDIO_BITS;
}

int osd_init(void)
{
	/* Backlight ON */
	const struct device *gpio1 = DEVICE_DT_GET(BLK_NODE);
	if (device_is_ready(gpio1)) {
		gpio_pin_configure(gpio1, BLK_PIN, GPIO_OUTPUT_ACTIVE);
		gpio_pin_set(gpio1, BLK_PIN, 1);
	} else {
		printk("NES: backlight gpio not ready\n");
	}

	s_display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(s_display_dev)) {
		printk("NES: display not ready\n");
		return -1;
	}
	display_blanking_off(s_display_dev);

	/* Clear the side strips (32px each) to black so no garbage pixels show */
	{
		static uint16_t black_strip[DISPLAY_X_OFFSET]
			__attribute__((section(".ext_ram.bss")));
		memset(black_strip, 0, sizeof(black_strip));

		struct display_buffer_descriptor strip_desc = {
			.buf_size = sizeof(black_strip),
			.width    = DISPLAY_X_OFFSET,
			.height   = 1,
			.pitch    = DISPLAY_X_OFFSET,
		};
		for (int y = 0; y < NES_HEIGHT; y++) {
			display_write(s_display_dev, 0, y,
				      &strip_desc, black_strip);
			display_write(s_display_dev,
				      DISPLAY_X_OFFSET + NES_WIDTH, y,
				      &strip_desc, black_strip);
		}
	}

	printk("NES: display ready\n");

	/* BLE HID already initialized at boot by launcher_nes main.c.
	 * In standalone nes_emulator sample, ble_hid_init() is called here.
	 * Inside launcher_nes we skip the second init.
	 */
#ifndef NES_SKIP_BLE_INIT
	ble_hid_init();
#endif

	/* Initialize Audio (ES8311 + I2S) */
	audio_hw_init();
	
	return 0;
}

void osd_shutdown(void)
{
	k_timer_stop(&s_nes_timer);
	/* s_framebuf and s_indexed are static PSRAM arrays — no free needed */
	if (s_bitmap) {
		bmp_destroy(&s_bitmap);
	}
}

/*
 * osd_main is called by nofrendo_main() after basic init.
 * "osd:" is the magic filename that tells the esp32-nesemu patched nofrendo
 * to call osd_getromdata() instead of fopen().
 */
int osd_main(int argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return main_loop("osd:", system_nes);
}

void osd_fullname(char *fullname, const char *shortname)
{
	strncpy(fullname, shortname, PATH_MAX - 1);
	fullname[PATH_MAX - 1] = '\0';
}

char *osd_newextension(char *string, char *ext)
{
	char *dot = strrchr(string, '.');

	if (dot) {
		strcpy(dot, ext);
	} else {
		strncat(string, ext, PATH_MAX - strlen(string) - 1);
	}
	return string;
}

static void handle_kb_event(struct kb_event *evt)
{
	int ev_idx = -1;
	switch (evt->key) {
	/* D-pad: WSAD or Arrow Keys */
	case 17: /* LV_KEY_UP */
	case 'w': ev_idx = event_joypad1_up; break;
	case 18: /* LV_KEY_DOWN */
	case 's': ev_idx = event_joypad1_down; break;
	case 20: /* LV_KEY_LEFT */
	case 'a': ev_idx = event_joypad1_left; break;
	case 19: /* LV_KEY_RIGHT */
	case 'd': ev_idx = event_joypad1_right; break;
	/* Actions */
	case 'k':
	case 'z': ev_idx = event_joypad1_a; break;
	case 'j':
	case 'x': ev_idx = event_joypad1_b; break;
	case 10: /* LV_KEY_ENTER */
		ev_idx = event_joypad1_start; break;
	case ' ': /* Space */
		ev_idx = event_joypad1_select; break;
	case 27: /* LV_KEY_ESC */
		ev_idx = event_hard_reset; break;
	}

	if (ev_idx != -1) {
		event_t evh = event_get(ev_idx);
		if (evh) {
			evh(evt->pressed ? INP_STATE_MAKE : INP_STATE_BREAK);
		}
	}
}

void osd_getinput(void)
{
	struct kb_event evt;
	/* Drain the BLE keyboard event queue */
	while (k_msgq_get(&kb_events, &evt, K_NO_WAIT) == 0) {
		handle_kb_event(&evt);
	}
}

void osd_getmouse(int *x, int *y, int *button)
{
	*x      = 0;
	*y      = 0;
	*button = 0;
}

int osd_makesnapname(char *filename, int len)
{
	/* screenshots not supported on embedded target */
	ARG_UNUSED(filename);
	ARG_UNUSED(len);
	return -1;
}
