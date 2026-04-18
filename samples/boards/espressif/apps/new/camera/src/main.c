/*
 * Camera firmware — Phase 1: live preview only.
 *
 * Direct display_write path (mirrors camera_test sample). LVGL is NOT used
 * for the live preview because LVGL's image rendering produced tearing and
 * byte-order issues on this board. Phase 2 will add a Gallery mode that
 * stops the camera stream and switches to LVGL for browsing photos.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/video.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#define CAM_W        320
#define CAM_H        240
#define CAM_BPP      2
#define CAM_BYTES    (CAM_W * CAM_H * CAM_BPP)
#define CAM_BUF_NUM  2
#define BL_PIN       (47 - 32)            /* IO47 = gpio1.15 */
#define DISP_STACK   4096
#define BTN_STACK    1024

#define MAX_PHOTOS         10
#define LONG_PRESS_MS      1500
#define EXTRA_LONG_PRESS_MS 3000
#define DEBOUNCE_MS        30
#define BANNER_H           8
#define BANNER_BYTES       (CAM_W * BANNER_H * CAM_BPP)

/* PSRAM photo store. Lost on reboot. */
static uint8_t photos[MAX_PHOTOS][CAM_BYTES] __attribute__((section(".ext_ram.bss"), aligned(4)));
static volatile int photo_count;
static volatile bool capture_request;

/* App mode + gallery state. */
enum app_mode { MODE_LIVE, MODE_GALLERY };
static volatile enum app_mode app_mode = MODE_LIVE;
static volatile int gallery_idx;
static volatile bool gallery_delete_request;

/* Pre-filled banner drawn at top of LCD in GALLERY mode. Acts as a
 * progress indicator: divided into photo_count segments, current photo's
 * segment is white, the rest are red. RGB565 BE.
 */
static uint8_t gallery_banner[BANNER_BYTES] __attribute__((aligned(4)));
static void update_banner(int idx, int count)
{
	if (count <= 0) {
		return;
	}
	int seg_w = CAM_W / count;
	if (seg_w == 0) {
		seg_w = 1;
	}
	for (int x = 0; x < CAM_W; x++) {
		int seg = x / seg_w;
		if (seg >= count) {
			seg = count - 1;
		}
		bool active = (seg == idx);
		uint8_t hi = active ? 0xFF : 0xF8;
		uint8_t lo = active ? 0xFF : 0x00;
		for (int y = 0; y < BANNER_H; y++) {
			int p = (y * CAM_W + x) * 2;
			gallery_banner[p]     = hi;
			gallery_banner[p + 1] = lo;
		}
	}
}

static struct video_buffer *frame_buffers[CAM_BUF_NUM];

K_MSGQ_DEFINE(disp_msgq, sizeof(struct video_buffer *), 1, sizeof(void *));
static K_THREAD_STACK_DEFINE(disp_stack, DISP_STACK);
static struct k_thread disp_thread;

struct disp_ctx {
	const struct device *display_dev;
	const struct device *camera_dev;
	struct video_format *fmt;
};

static int blit_frame(const struct device *display_dev,
		      const struct video_buffer *vbuf,
		      const struct video_format *fmt)
{
	struct display_buffer_descriptor desc = {
		.buf_size = vbuf->bytesused,
		.width = fmt->width,
		.pitch = fmt->width,
		.height = vbuf->bytesused / fmt->pitch,
	};
	return display_write(display_dev, 0, vbuf->line_offset, &desc, vbuf->buffer);
}

static void disp_thread_entry(void *p1, void *p2, void *p3)
{
	struct disp_ctx *ctx = p1;
	struct video_buffer *buf;
	int ret;
	enum app_mode last_mode = MODE_LIVE;
	int last_gallery_idx = -1;
	int last_photo_count = -1;

	while (1) {
		ret = k_msgq_get(&disp_msgq, &buf, K_FOREVER);
		if (ret < 0) {
			printk("[Disp] msgq get err %d\n", ret);
			break;
		}

		/* Handle queued delete (long press in gallery). */
		if (gallery_delete_request) {
			gallery_delete_request = false;
			if (photo_count > 0 && gallery_idx >= 0 &&
			    gallery_idx < photo_count) {
				int del = gallery_idx;
				for (int i = del; i < photo_count - 1; i++) {
					memcpy(photos[i], photos[i + 1], CAM_BYTES);
				}
				photo_count--;
				printk("[Photo] deleted #%d (now %d)\n", del, photo_count);
				if (photo_count == 0) {
					app_mode = MODE_LIVE;
					printk("[Mode] gallery empty → LIVE\n");
				} else if (gallery_idx >= photo_count) {
					gallery_idx = photo_count - 1;
				}
			}
		}

		/* Capture (only meaningful in LIVE mode). */
		if (capture_request) {
			capture_request = false;
			if (app_mode == MODE_LIVE && photo_count < MAX_PHOTOS) {
				memcpy(photos[photo_count], buf->buffer, CAM_BYTES);
				photo_count++;
				printk("[Photo] saved #%d (%d/%d)\n",
				       photo_count - 1, photo_count, MAX_PHOTOS);
			} else if (app_mode == MODE_LIVE) {
				printk("[Photo] FULL (%d/%d) — drop\n",
				       photo_count, MAX_PHOTOS);
			}
		}

		if (app_mode == MODE_LIVE) {
			ret = blit_frame(ctx->display_dev, buf, ctx->fmt);
			last_mode = MODE_LIVE;
		} else {
			/* GALLERY: blit current photo. Only re-write the LCD
			 * when the displayed image actually changed (entering
			 * gallery, deleting, switching photo) — saves SPI BW.
			 */
			if (last_mode != MODE_GALLERY ||
			    last_gallery_idx != gallery_idx ||
			    last_photo_count != photo_count) {
				if (photo_count > 0 && gallery_idx >= 0 &&
				    gallery_idx < photo_count) {
					struct display_buffer_descriptor desc = {
						.buf_size = CAM_BYTES,
						.width = ctx->fmt->width,
						.pitch = ctx->fmt->width,
						.height = ctx->fmt->height,
					};
					display_write(ctx->display_dev, 0, 0,
						      &desc, photos[gallery_idx]);
					/* Overlay banner with current segment highlighted. */
					update_banner(gallery_idx, photo_count);
					struct display_buffer_descriptor bdesc = {
						.buf_size = BANNER_BYTES,
						.width = CAM_W,
						.pitch = CAM_W,
						.height = BANNER_H,
					};
					display_write(ctx->display_dev, 0, 0,
						      &bdesc, gallery_banner);
				}
				last_mode = MODE_GALLERY;
				last_gallery_idx = gallery_idx;
				last_photo_count = photo_count;
			}
			ret = 0;
		}
		if (ret < 0) {
			printk("[Disp] write err %d\n", ret);
		}

		ret = video_enqueue(ctx->camera_dev, buf);
		if (ret < 0) {
			printk("[Disp] re-enqueue err %d\n", ret);
			break;
		}
	}
}

/* BOOT button (sw0 = gpio0.0, active low, pull-up). 100 Hz polling.
 * LIVE mode:
 *   - short press     : capture photo to PSRAM
 *   - long press      : enter GALLERY (shows latest photo)
 * GALLERY mode:
 *   - short press     : next photo (wrap around)
 *   - long press      : delete current photo (auto-exit if empty)
 *   - extra-long >3s  : exit GALLERY back to LIVE
 */
static const struct gpio_dt_spec boot_btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static K_THREAD_STACK_DEFINE(btn_stack, BTN_STACK);
static struct k_thread btn_thread;

static void btn_thread_entry(void *p1, void *p2, void *p3)
{
	bool was_pressed = false;
	int64_t press_start = 0;

	if (!gpio_is_ready_dt(&boot_btn)) {
		printk("[Btn] not ready\n");
		return;
	}
	gpio_pin_configure_dt(&boot_btn, GPIO_INPUT);
	printk("[Btn] BOOT monitor started\n");

	while (1) {
		bool pressed = gpio_pin_get_dt(&boot_btn) == 1;
		int64_t now = k_uptime_get();

		if (pressed && !was_pressed) {
			press_start = now;
			was_pressed = true;
		} else if (!pressed && was_pressed) {
			int64_t held = now - press_start;
			was_pressed = false;
			if (held < DEBOUNCE_MS) {
				/* glitch */
			} else if (held < LONG_PRESS_MS) {
				/* SHORT */
				if (app_mode == MODE_LIVE) {
					printk("[Btn] short (%lldms) LIVE → capture\n", held);
					capture_request = true;
				} else {
					if (photo_count > 0) {
						gallery_idx = (gallery_idx + 1) % photo_count;
						printk("[Btn] short (%lldms) GAL → next [%d/%d]\n",
						       held, gallery_idx + 1, photo_count);
					}
				}
			} else {
				/* LONG (>=1.5s). LIVE: any long-press → enter gallery.
				 * GALLERY: 1.5–3s = delete, >3s = exit to LIVE.
				 */
				if (app_mode == MODE_LIVE) {
					if (photo_count > 0) {
						gallery_idx = photo_count - 1;
						app_mode = MODE_GALLERY;
						printk("[Btn] long (%lldms) LIVE → GALLERY[%d]\n",
						       held, gallery_idx);
					} else {
						printk("[Btn] long (%lldms) LIVE — no photos\n", held);
					}
				} else if (held < EXTRA_LONG_PRESS_MS) {
					printk("[Btn] long (%lldms) GAL → delete[%d]\n",
					       held, gallery_idx);
					gallery_delete_request = true;
				} else {
					printk("[Btn] xlong (%lldms) GAL → LIVE\n", held);
					app_mode = MODE_LIVE;
				}
			}
		}
		k_msleep(10);
	}
}

int main(void)
{
	const struct device *const camera_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_camera));
	const struct device *const display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	const struct device *const gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
	struct video_format fmt = {
		.type = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_RGB565,
		.width = CAM_W,
		.height = CAM_H,
		.pitch = CAM_W * CAM_BPP,
	};
	static struct disp_ctx ctx;
	struct video_buffer *captured = NULL;
	int ret;

	printk("\n=== CHD-ESP32-S3-BOX Camera firmware ===\n");

	printk("[Boot] no BT, no WiFi — Video only\n");

	if (!device_is_ready(camera_dev) || !device_is_ready(display_dev)) {
		printk("device not ready\n");
		return 0;
	}

	if (device_is_ready(gpio1)) {
		gpio_pin_configure(gpio1, BL_PIN, GPIO_OUTPUT_ACTIVE);
		gpio_pin_set(gpio1, BL_PIN, 1);
	}
	display_blanking_off(display_dev);

	ret = video_set_format(camera_dev, &fmt);
	if (ret < 0) {
		printk("set_format err %d\n", ret);
		return 0;
	}
	ret = video_get_format(camera_dev, &fmt);
	if (ret < 0) {
		printk("get_format err %d\n", ret);
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
		ret = video_enqueue(camera_dev, frame_buffers[i]);
		if (ret < 0) {
			printk("enqueue %d err %d\n", i, ret);
			return 0;
		}
	}

	ctx.display_dev = display_dev;
	ctx.camera_dev = camera_dev;
	ctx.fmt = &fmt;
	k_thread_create(&disp_thread, disp_stack, DISP_STACK,
			disp_thread_entry, &ctx, NULL, NULL,
			K_PRIO_COOP(7), 0, K_NO_WAIT);
	k_thread_name_set(&disp_thread, "disp");

	k_thread_create(&btn_thread, btn_stack, BTN_STACK,
			btn_thread_entry, NULL, NULL, NULL,
			K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
	k_thread_name_set(&btn_thread, "btn");

	ret = video_stream_start(camera_dev, VIDEO_BUF_TYPE_OUTPUT);
	if (ret < 0) {
		printk("stream_start err %d\n", ret);
		return 0;
	}
	printk("[Cam] stream started\n");

	for (uint32_t frame = 0; ; frame++) {
		ret = video_dequeue(camera_dev, &captured, K_FOREVER);
		if (ret < 0) {
			printk("dequeue err %d\n", ret);
			break;
		}
		if ((frame % 60U) == 0U) {
			printk("[Cam] frame %u\n", frame);
		}
		ret = k_msgq_put(&disp_msgq, &captured, K_FOREVER);
		if (ret < 0) {
			printk("msgq put err %d\n", ret);
			video_enqueue(camera_dev, captured);
		}
	}

	return 0;
}
