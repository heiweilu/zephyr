/*
 * Face recognize firmware — Step 1a (Option C dynamic switching skeleton).
 *
 * State machine (driven by BOOT button):
 *   MODE_LIVE        : camera streaming, WiFi off, LCD shows preview
 *      └─ BOOT short ──▶ MODE_UPLOADING
 *   MODE_UPLOADING   : cam stopped, WiFi connecting/connected, blue LCD
 *      └─ wifi up + 2s ──▶ MODE_RESULT (Phase 1b will replace 2s with HTTP POST)
 *   MODE_RESULT      : WiFi up but idle, LCD shows captured snapshot
 *      └─ BOOT short ──▶ MODE_LIVE (cam restart, WiFi down)
 *
 * Phase 1b will add: OV3660 JPEG mode, base64 encode, TLS, HTTPS POST to
 * Aliyun bailian qwen-vl-plus, JSON parse + LVGL result text overlay.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/video.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "secrets.h"
#include "jpeg_enc.h"
#include "ai_client.h"
#include "audio.h"
#include "tts_client.h"

/* Step 1b-1a: encode a synthetic gradient at boot, dump JPEG hex over UART.
 * Set to 0 to skip (re-enable normal LIVE mode without delay). */
#define JPEG_SELFTEST 0

LOG_MODULE_REGISTER(face_recog, LOG_LEVEL_INF);

#define CAM_W        320
#define CAM_H        240
#define CAM_BPP      2
#define CAM_BYTES    (CAM_W * CAM_H * CAM_BPP)
#define CAM_BUF_NUM  2
#define BL_PIN       (47 - 32)
#define BTN_STACK    1024
#define MAIN_STACK   8192
#define DEBOUNCE_MS  30
#define LONG_PRESS_MS 1500

enum mode {
	MODE_LIVE = 0,
	MODE_UPLOADING,
	MODE_RESULT,
};

static volatile enum mode g_mode = MODE_LIVE;
static volatile bool      g_btn_event;

/* PSRAM-backed snapshot of the last frame captured by the user. */
static uint8_t snapshot[CAM_BYTES] __attribute__((section(".ext_ram.bss"), aligned(4)));
/* Pre-filled solid-blue full-screen buffer for the UPLOADING state. */
static uint8_t blue_screen[CAM_BYTES] __attribute__((section(".ext_ram.bss"), aligned(4)));

/* PSRAM scratch for RGB565→RGB888 conversion + JPEG output of the snapshot.
 * Always allocated (selftest reuses the same backing pages via overlap if
 * JPEG_SELFTEST is enabled — but selftest is now off by default). */
#define SNAP_RGB888_BYTES  (CAM_W * CAM_H * 3)
#define SNAP_JPEG_BYTES    (48 * 1024)
static uint8_t snap_rgb888[SNAP_RGB888_BYTES]
	__attribute__((section(".ext_ram.bss"), aligned(4)));
static uint8_t snap_jpeg[SNAP_JPEG_BYTES]
	__attribute__((section(".ext_ram.bss"), aligned(4)));

/* Convert one RGB565 (big-endian, as written by LCD-CAM DMA) frame to RGB888
 * and run jpeg_enc on it.  Dumps the JPEG hex over UART between the same
 * JPEG_BEGIN/JPEG_END markers used by the selftest, so the host tools
 * (capture_selftest.py + hex_to_jpg.ps1) work unchanged. */
static int dump_snapshot_as_jpeg(int quality)
{
	const uint8_t *src = snapshot;
	uint8_t *dst = snap_rgb888;
	for (int i = 0; i < CAM_W * CAM_H; i++) {
		uint8_t hi = src[2 * i + 0];
		uint8_t lo = src[2 * i + 1];
		uint16_t p = ((uint16_t)hi << 8) | lo;
		uint8_t r5 = (p >> 11) & 0x1F;
		uint8_t g6 = (p >> 5)  & 0x3F;
		uint8_t b5 =  p        & 0x1F;
		dst[3 * i + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
		dst[3 * i + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
		dst[3 * i + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
	}

	int64_t t0 = k_uptime_get();
	int n = jpeg_encode_rgb888(snap_jpeg, SNAP_JPEG_BYTES,
				   snap_rgb888, CAM_W, CAM_H, quality);
	int64_t dt = k_uptime_get() - t0;

	printk("\n=== JPEG SNAPSHOT ===\n");
	printk("encode result=%d bytes, took %lld ms (q=%d)\n", n, dt, quality);
	if (n <= 0) {
		printk("=== JPEG SNAPSHOT FAILED ===\n");
		return n;
	}
	printk("JPEG_BEGIN size=%d\n", n);
	for (int i = 0; i < n; i++) {
		printk("%02x", snap_jpeg[i]);
		if ((i % 32) == 31) {
			printk("\n");
		}
	}
	if ((n % 32) != 0) {
		printk("\n");
	}
	printk("JPEG_END\n=== JPEG SNAPSHOT DONE ===\n\n");
	return n;
}

#if JPEG_SELFTEST
/* RGB888 test pattern + JPEG output (PSRAM-backed). */
#define SELFTEST_RGB_BYTES  (CAM_W * CAM_H * 3)
#define SELFTEST_JPG_BYTES  (48 * 1024)
static uint8_t selftest_rgb[SELFTEST_RGB_BYTES]
	__attribute__((section(".ext_ram.bss"), aligned(4)));
static uint8_t selftest_jpg[SELFTEST_JPG_BYTES]
	__attribute__((section(".ext_ram.bss"), aligned(4)));

static void jpeg_selftest(void)
{
	/* Gradient: R = x ramp, G = y ramp, B = combined diagonal */
	for (int y = 0; y < CAM_H; y++) {
		for (int x = 0; x < CAM_W; x++) {
			uint8_t *p = &selftest_rgb[(y * CAM_W + x) * 3];
			p[0] = (uint8_t)(x * 255 / (CAM_W - 1));
			p[1] = (uint8_t)(y * 255 / (CAM_H - 1));
			p[2] = (uint8_t)(((x + y) * 255) / (CAM_W + CAM_H - 2));
		}
	}
	int64_t t0 = k_uptime_get();
	int n = jpeg_encode_rgb888(selftest_jpg, SELFTEST_JPG_BYTES,
				   selftest_rgb, CAM_W, CAM_H, 50);
	int64_t dt = k_uptime_get() - t0;

	printk("\n=== JPEG SELFTEST ===\n");
	printk("encode result=%d bytes, took %lld ms\n", n, dt);
	if (n <= 0) {
		printk("=== JPEG SELFTEST FAILED ===\n");
		return;
	}
	printk("JPEG_BEGIN size=%d\n", n);
	for (int i = 0; i < n; i++) {
		printk("%02x", selftest_jpg[i]);
		if ((i % 32) == 31) {
			printk("\n");
		}
	}
	if ((n % 32) != 0) {
		printk("\n");
	}
	printk("JPEG_END\n=== JPEG SELFTEST DONE ===\n\n");
}
#endif /* JPEG_SELFTEST */

/* ── Camera buffers ── */
static struct video_buffer *frame_buffers[CAM_BUF_NUM];

/* ── BOOT button (sw0) ── */
static const struct gpio_dt_spec boot_btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static K_THREAD_STACK_DEFINE(btn_stack, BTN_STACK);
static struct k_thread btn_thread;

static void btn_thread_entry(void *p1, void *p2, void *p3)
{
	bool was_pressed = false;
	int64_t press_start = 0;

	if (!gpio_is_ready_dt(&boot_btn)) {
		LOG_ERR("BOOT button not ready");
		return;
	}
	gpio_pin_configure_dt(&boot_btn, GPIO_INPUT);

	while (1) {
		bool pressed = gpio_pin_get_dt(&boot_btn) == 1;
		int64_t now  = k_uptime_get();

		if (pressed && !was_pressed) {
			press_start = now;
			was_pressed = true;
		} else if (!pressed && was_pressed) {
			int64_t held = now - press_start;
			was_pressed = false;
			if (held < DEBOUNCE_MS) {
				/* glitch */
			} else if (held < LONG_PRESS_MS) {
				LOG_INF("BOOT short (%lldms) in mode=%d",
					held, (int)g_mode);
				g_btn_event = true;
			} else {
				LOG_INF("BOOT long (%lldms) — reserved", held);
			}
		}
		k_msleep(10);
	}
}

/* ── WiFi ── */

static struct net_if *sta_iface;
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;
static K_SEM_DEFINE(wifi_connected_sem, 0, 1);
static K_SEM_DEFINE(ipv4_ready_sem, 0, 1);
static K_SEM_DEFINE(scan_done_sem, 0, 1);
static bool wifi_cb_added;

static void wifi_evt(struct net_mgmt_event_callback *cb, uint64_t e, struct net_if *iface)
{
	ARG_UNUSED(iface);
	if (e == NET_EVENT_WIFI_SCAN_DONE) {
		k_sem_give(&scan_done_sem);
	} else if (e == NET_EVENT_WIFI_CONNECT_RESULT) {
		if (cb->info && cb->info_length == sizeof(struct wifi_status)) {
			const struct wifi_status *s = cb->info;
			if (s->status == WIFI_STATUS_CONN_SUCCESS) {
				LOG_INF("WiFi connected to %s", WIFI_SSID);
				k_sem_give(&wifi_connected_sem);
			} else {
				LOG_ERR("WiFi connect failed: %d", s->status);
			}
		}
	}
}

static void ipv4_evt(struct net_mgmt_event_callback *cb, uint64_t e, struct net_if *iface)
{
	ARG_UNUSED(cb); ARG_UNUSED(iface);
	if (e == NET_EVENT_IPV4_ADDR_ADD) {
		LOG_INF("IPv4 acquired");
		k_sem_give(&ipv4_ready_sem);
	}
}

static int wifi_up(void)
{
	struct wifi_connect_req_params p = {0};
	int ret;

	if (!sta_iface) {
		sta_iface = net_if_get_default();
	}
	if (!sta_iface) {
		return -ENODEV;
	}
	if (!wifi_cb_added) {
		net_mgmt_init_event_callback(&wifi_cb, wifi_evt,
			NET_EVENT_WIFI_SCAN_DONE | NET_EVENT_WIFI_CONNECT_RESULT |
			NET_EVENT_WIFI_DISCONNECT_RESULT);
		net_mgmt_add_event_callback(&wifi_cb);
		net_mgmt_init_event_callback(&ipv4_cb, ipv4_evt, NET_EVENT_IPV4_ADDR_ADD);
		net_mgmt_add_event_callback(&ipv4_cb);
		wifi_cb_added = true;
	}
	k_sem_reset(&wifi_connected_sem);
	k_sem_reset(&ipv4_ready_sem);
	k_sem_reset(&scan_done_sem);

	(void)net_if_up(sta_iface);

	LOG_INF("WiFi: scanning ...");
	for (int i = 0; i < 10; i++) {
		ret = net_mgmt(NET_REQUEST_WIFI_SCAN, sta_iface, NULL, 0);
		if (ret == 0) break;
		k_msleep(500);
	}
	if (ret) return ret;
	if (k_sem_take(&scan_done_sem, K_SECONDS(15))) return -ETIMEDOUT;

	p.ssid        = (const uint8_t *)WIFI_SSID;
	p.ssid_length = strlen(WIFI_SSID);
	p.psk         = (const uint8_t *)WIFI_PSK;
	p.psk_length  = strlen(WIFI_PSK);
	p.band        = WIFI_FREQ_BAND_UNKNOWN;
	p.channel     = WIFI_CHANNEL_ANY;
	p.security    = WIFI_SECURITY_TYPE_PSK;
	p.mfp         = WIFI_MFP_OPTIONAL;
	p.timeout     = SYS_FOREVER_MS;

	LOG_INF("WiFi: connecting to %s", WIFI_SSID);
	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &p, sizeof(p));
	if (ret) return ret;
	if (k_sem_take(&wifi_connected_sem, K_SECONDS(30))) return -ETIMEDOUT;
	if (k_sem_take(&ipv4_ready_sem, K_SECONDS(30))) return -ETIMEDOUT;
	return 0;
}

static void wifi_down(void)
{
	if (!sta_iface) return;
	(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, sta_iface, NULL, 0);
	(void)net_if_down(sta_iface);
	LOG_INF("WiFi: down");
}

/* ── Helpers: paint solid color / blit raw frame ── */

static void fill_screen_blue(const struct device *display_dev)
{
	struct display_buffer_descriptor d = {
		.buf_size = CAM_BYTES,
		.width    = CAM_W,
		.pitch    = CAM_W,
		.height   = CAM_H,
	};
	display_write(display_dev, 0, 0, &d, blue_screen);
}

static void blit_snapshot(const struct device *display_dev)
{
	struct display_buffer_descriptor d = {
		.buf_size = CAM_BYTES,
		.width    = CAM_W,
		.pitch    = CAM_W,
		.height   = CAM_H,
	};
	display_write(display_dev, 0, 0, &d, snapshot);
}

/* ── Camera control ── */

static int cam_start(const struct device *cam)
{
	for (int i = 0; i < CAM_BUF_NUM; i++) {
		int r = video_enqueue(cam, frame_buffers[i]);
		if (r < 0) {
			LOG_ERR("re-enqueue %d err %d", i, r);
			return r;
		}
	}
	return video_stream_start(cam, VIDEO_BUF_TYPE_OUTPUT);
}

static void cam_stop(const struct device *cam)
{
	(void)video_stream_stop(cam, VIDEO_BUF_TYPE_OUTPUT);
	/* Drain any in-flight buffers so the next start can re-enqueue cleanly. */
	struct video_buffer *b;
	while (video_dequeue(cam, &b, K_MSEC(50)) == 0) {
		/* discard */
	}
}

/* ── main: drives the state machine ── */

/* Step 1b-3a: 1kHz square-wave beep (0.5s @ 16kHz mono, 16-bit) */
#define BEEP_SAMPLES 8000
static int16_t beep_pcm[BEEP_SAMPLES] __attribute__((section(".ext_ram.bss")));

static void play_boot_beep(void)
{
	if (audio_codec_init() < 0) {
		LOG_WRN("audio_codec_init failed; skipping beep");
		return;
	}
	/* 1 kHz square wave: at 16 kHz that's 16 samples per period (8 hi / 8 lo) */
	for (int i = 0; i < BEEP_SAMPLES; i++) {
		beep_pcm[i] = ((i / 8) & 1) ? 8000 : -8000;
	}
	LOG_INF("playing 1kHz / 0.5s boot beep");
	audio_play(beep_pcm, BEEP_SAMPLES);
	LOG_INF("beep done");
}

int main(void)
{
	const struct device *const camera_dev  = DEVICE_DT_GET(DT_CHOSEN(zephyr_camera));
	const struct device *const display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	const struct device *const gpio1       = DEVICE_DT_GET(DT_NODELABEL(gpio1));
	struct video_format fmt = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_RGB565,
		.width       = CAM_W,
		.height      = CAM_H,
		.pitch       = CAM_W * CAM_BPP,
	};
	int ret;

	printk("\n=== CHD-ESP32-S3-BOX Face Recognize firmware (Step 1a) ===\n");

	if (!device_is_ready(camera_dev) || !device_is_ready(display_dev)) {
		printk("device not ready\n");
		return 0;
	}
	if (device_is_ready(gpio1)) {
		gpio_pin_configure(gpio1, BL_PIN, GPIO_OUTPUT_ACTIVE);
		gpio_pin_set(gpio1, BL_PIN, 1);
	}
	display_blanking_off(display_dev);

	/* Step 1b-3a: ES8311 + I2S boot self-test */
	play_boot_beep();

	/* Pre-fill the solid-blue buffer (RGB565 BE: 0x001F LE -> 0x1F 00 BE) */
	for (int i = 0; i < CAM_BYTES; i += 2) {
		blue_screen[i]     = 0x00;
		blue_screen[i + 1] = 0x1F;
	}

#if JPEG_SELFTEST
	/* Give the host capture script time to (re)open USB-CDC after RST. */
	printk("\n[selftest will start in 5s — make sure capture script is running]\n");
	k_sleep(K_SECONDS(5));
	jpeg_selftest();
#endif

	if (video_set_format(camera_dev, &fmt) < 0 ||
	    video_get_format(camera_dev, &fmt) < 0) {
		printk("cam fmt err\n");
		return 0;
	}

	for (int i = 0; i < CAM_BUF_NUM; i++) {
		frame_buffers[i] = video_buffer_aligned_alloc(
			CAM_BYTES, CONFIG_VIDEO_BUFFER_POOL_ALIGN, K_FOREVER);
		if (!frame_buffers[i]) {
			printk("vbuf alloc %d failed\n", i);
			return 0;
		}
		frame_buffers[i]->type = VIDEO_BUF_TYPE_OUTPUT;
	}

	k_thread_create(&btn_thread, btn_stack, BTN_STACK,
			btn_thread_entry, NULL, NULL, NULL,
			K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
	k_thread_name_set(&btn_thread, "btn");

	if (cam_start(camera_dev) < 0) {
		printk("cam_start initial failed\n");
		return 0;
	}
	LOG_INF("Mode = LIVE");

	struct video_buffer *captured = NULL;
	bool have_snapshot = false;

	for (uint32_t frame = 0; ; frame++) {
		/* Handle button events first */
		if (g_btn_event) {
			g_btn_event = false;
			if (g_mode == MODE_LIVE) {
				/* Take snapshot from the most recently displayed buffer */
				if (captured) {
					memcpy(snapshot, captured->buffer, CAM_BYTES);
					have_snapshot = true;
					LOG_INF("Snapshot saved (%d B)", CAM_BYTES);
				}
				cam_stop(camera_dev);
				captured = NULL;

				LOG_INF("Mode LIVE → UPLOADING");
				g_mode = MODE_UPLOADING;
				fill_screen_blue(display_dev);

				/* Step 1b-1b: encode the snapshot RIGHT HERE (cam stopped,
				 * WiFi not up yet → safe LCD-CAM, deterministic timing). */
				int jpeg_len = 0;
				if (have_snapshot) {
					jpeg_len = dump_snapshot_as_jpeg(50);
				}

				ret = wifi_up();
				if (ret) {
					LOG_ERR("wifi_up failed: %d, skipping POST", ret);
				} else if (jpeg_len > 0) {
					/* Phase 1b-2: POST jpeg to qwen-vl-plus, dump response. */
					if (ai_client_init() == 0) {
						int pr = ai_client_post_jpeg(snap_jpeg, jpeg_len);
						if (pr < 0) {
							LOG_ERR("ai_client_post_jpeg failed: %d", pr);
						} else {
							/* Phase 1b-3b: speak the caption via CosyVoice TTS */
							static char caption[512];
							int n = ai_client_get_caption(caption, sizeof(caption));
							if (n > 0) {
								LOG_INF("caption (%d B): %s", n, caption);
								int tr = tts_client_speak(caption);
								if (tr < 0) LOG_ERR("tts_client_speak: %d", tr);
							} else {
								LOG_WRN("no caption found in response");
							}
						}
					}
				}

				LOG_INF("Mode UPLOADING → RESULT");
				g_mode = MODE_RESULT;
				if (have_snapshot) {
					blit_snapshot(display_dev);
				}
			} else if (g_mode == MODE_RESULT) {
				LOG_INF("Mode RESULT → LIVE");
				wifi_down();
				if (cam_start(camera_dev) < 0) {
					LOG_ERR("cam_start failed");
				}
				g_mode = MODE_LIVE;
			}
			continue;
		}

		if (g_mode != MODE_LIVE) {
			k_msleep(50);
			continue;
		}

		/* LIVE: dequeue + display + re-enqueue */
		ret = video_dequeue(camera_dev, &captured, K_MSEC(200));
		if (ret < 0) {
			continue;
		}
		struct display_buffer_descriptor d = {
			.buf_size = captured->bytesused,
			.width    = fmt.width,
			.pitch    = fmt.width,
			.height   = captured->bytesused / fmt.pitch,
		};
		display_write(display_dev, 0, captured->line_offset, &d, captured->buffer);

		if ((frame % 60U) == 0U) {
			printk("[Cam] frame %u\n", frame);
		}
		(void)video_enqueue(camera_dev, captured);
	}
	return 0;
}
