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

#define CAM_W        320
#define CAM_H        240
#define CAM_BPP      2
#define CAM_BYTES    (CAM_W * CAM_H * CAM_BPP)
#define CAM_BUF_NUM  2
#define BL_PIN       (47 - 32)            /* IO47 = gpio1.15 */
#define DISP_STACK   4096

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

	while (1) {
		ret = k_msgq_get(&disp_msgq, &buf, K_FOREVER);
		if (ret < 0) {
			printk("[Disp] msgq get err %d\n", ret);
			break;
		}
		ret = blit_frame(ctx->display_dev, buf, ctx->fmt);
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
