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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Private state                                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

static const struct device *s_display_dev;

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
/*  OSD interface implementations                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

void osd_setsound(void (*playfunc)(void *buffer, int size))
{
	/* Phase 1: audio not implemented */
	ARG_UNUSED(playfunc);
}

void osd_getvideoinfo(vidinfo_t *info)
{
	info->default_width  = NES_WIDTH;
	info->default_height = NES_HEIGHT;
	info->driver         = &s_zephyr_driver;
}

void osd_getsoundinfo(sndinfo_t *info)
{
	info->sample_rate = 22050;
	info->bps         = 16;
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
	printk("NES: display ready\n");
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

void osd_getinput(void)
{
	/*
	 * Phase 1: no controller input (game runs but cannot be controlled).
	 * Phase 2: read from BLE keyboard k_msgq and map HID keycodes:
	 *   0x52 Up    → INP_PAD_UP,   0x51 Down  → INP_PAD_DOWN
	 *   0x50 Left  → INP_PAD_LEFT, 0x4F Right → INP_PAD_RIGHT
	 *   0x1D Z     → INP_PAD_A,    0x1B X     → INP_PAD_B
	 *   0x29 Enter → INP_PAD_START, 0x2C Space → INP_PAD_SELECT
	 */
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
