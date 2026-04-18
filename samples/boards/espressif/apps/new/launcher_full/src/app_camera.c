/*
 * app_camera.c — Camera live preview (one-way; never returns).
 *
 * Phase 2d: borrowed from samples/.../new/camera (LIVE-only path, no
 * gallery, no capture, no BOOT handling). Once the user activates the
 * Camera tile from the HOME screen, the preview owns the display
 * forever. This matches the "进入相机就一直在相机模式" requirement.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/video.h>
#include <zephyr/logging/log.h>

#include "app_camera.h"

LOG_MODULE_REGISTER(app_camera, LOG_LEVEL_INF);

#define CAM_W       320
#define CAM_H       240
#define CAM_BPP     2
#define CAM_BYTES   (CAM_W * CAM_H * CAM_BPP)
#define CAM_BUF_NUM 2

static struct video_buffer *vbufs[CAM_BUF_NUM];

static int blit_frame(const struct device *display_dev,
		      const struct video_buffer *vbuf,
		      const struct video_format *fmt)
{
	struct display_buffer_descriptor desc = {
		.buf_size = vbuf->bytesused,
		.width    = fmt->width,
		.pitch    = fmt->width,
		.height   = vbuf->bytesused / fmt->pitch,
	};
	return display_write(display_dev, 0, vbuf->line_offset, &desc, vbuf->buffer);
}

void app_camera_run(void)
{
	const struct device *camera_dev  = DEVICE_DT_GET(DT_CHOSEN(zephyr_camera));
	const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	struct video_format fmt = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_RGB565,
		.width       = CAM_W,
		.height      = CAM_H,
		.pitch       = CAM_W * CAM_BPP,
	};
	struct video_buffer *captured = NULL;
	int ret;

	LOG_INF("entering Camera (one-way preview)");

	if (!device_is_ready(camera_dev) || !device_is_ready(display_dev)) {
		LOG_ERR("camera or display not ready");
		return;
	}

	ret = video_set_format(camera_dev, &fmt);
	if (ret < 0) {
		LOG_ERR("set_format err %d", ret);
		return;
	}
	video_get_format(camera_dev, &fmt);

	for (int i = 0; i < CAM_BUF_NUM; i++) {
		vbufs[i] = video_buffer_aligned_alloc(
			CAM_BYTES, CONFIG_VIDEO_BUFFER_POOL_ALIGN, K_FOREVER);
		if (!vbufs[i]) {
			LOG_ERR("vbuf alloc %d failed", i);
			return;
		}
		vbufs[i]->type = VIDEO_BUF_TYPE_OUTPUT;
		ret = video_enqueue(camera_dev, vbufs[i]);
		if (ret < 0) {
			LOG_ERR("enqueue %d err %d", i, ret);
			return;
		}
	}

	ret = video_stream_start(camera_dev, VIDEO_BUF_TYPE_OUTPUT);
	if (ret < 0) {
		LOG_ERR("stream_start err %d", ret);
		return;
	}
	LOG_INF("camera stream started");

	for (uint32_t frame = 0; ; frame++) {
		ret = video_dequeue(camera_dev, &captured, K_FOREVER);
		if (ret < 0) {
			LOG_ERR("dequeue err %d", ret);
			break;
		}
		if ((frame % 120U) == 0U) {
			LOG_INF("frame %u", frame);
		}
		(void)blit_frame(display_dev, captured, &fmt);
		(void)video_enqueue(camera_dev, captured);
	}
}
