#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#if !DT_HAS_CHOSEN(zephyr_camera)
#error Missing zephyr,camera chosen node
#endif

#define FRAME_WIDTH 320
#define FRAME_HEIGHT 240
#define FRAME_BYTES (FRAME_WIDTH * FRAME_HEIGHT * 2)
#define FRAME_BUFFER_COUNT 2

static struct video_buffer *frame_buffers[FRAME_BUFFER_COUNT];

int main(void)
{
	const struct device *const camera_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_camera));
	struct video_format fmt = {
		.type = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_RGB565,
		.width = FRAME_WIDTH,
		.height = FRAME_HEIGHT,
		.pitch = FRAME_WIDTH * 2,
	};
	struct video_caps caps = {0};
	struct video_buffer *captured = NULL;
	int ret;

	printk("\n*** CHD-ESP32-S3-BOX OV3660 Capture Test ***\n");

	if (!device_is_ready(camera_dev)) {
		printk("camera device not ready\n");
		return 0;
	}

	ret = video_get_caps(camera_dev, &caps);
	if (ret < 0) {
		printk("video_get_caps failed: %d\n", ret);
		return 0;
	}

	if (caps.min_vbuf_count > FRAME_BUFFER_COUNT) {
		printk("camera requires %u buffers, app only provides %u\n",
			caps.min_vbuf_count, FRAME_BUFFER_COUNT);
		return 0;
	}

	ret = video_set_format(camera_dev, &fmt);
	if (ret < 0) {
		printk("video_set_format failed: %d\n", ret);
		return 0;
	}

	ret = video_get_format(camera_dev, &fmt);
	if (ret < 0) {
		printk("video_get_format failed: %d\n", ret);
		return 0;
	}

	printk("capture format: %c%c%c%c %ux%u pitch=%u\n",
		(char)(fmt.pixelformat & 0xff),
		(char)((fmt.pixelformat >> 8) & 0xff),
		(char)((fmt.pixelformat >> 16) & 0xff),
		(char)((fmt.pixelformat >> 24) & 0xff),
		fmt.width, fmt.height, fmt.pitch);

	for (int index = 0; index < FRAME_BUFFER_COUNT; index++) {
		frame_buffers[index] = video_buffer_aligned_alloc(
			FRAME_BYTES, CONFIG_VIDEO_BUFFER_POOL_ALIGN, K_FOREVER);
		if (frame_buffers[index] == NULL) {
			printk("video buffer alloc failed at %d\n", index);
			return 0;
		}

		frame_buffers[index]->type = VIDEO_BUF_TYPE_OUTPUT;

		ret = video_enqueue(camera_dev, frame_buffers[index]);
		if (ret < 0) {
			printk("video_enqueue failed at %d: %d\n", index, ret);
			return 0;
		}
	}

	ret = video_stream_start(camera_dev, VIDEO_BUF_TYPE_OUTPUT);
	if (ret < 0) {
		printk("video_stream_start failed: %d\n", ret);
		return 0;
	}

	printk("stream started\n");

	for (int frame = 0; frame < 5; frame++) {
		ret = video_dequeue(camera_dev, &captured, K_FOREVER);
		if (ret < 0) {
			printk("frame dequeue failed: %d\n", ret);
			break;
		}

		printk("frame %d captured: %u bytes ts=%u first=%02x %02x %02x %02x\n",
			frame,
			captured->bytesused,
			captured->timestamp,
			captured->buffer[0],
			captured->buffer[1],
			captured->buffer[2],
			captured->buffer[3]);

		ret = video_enqueue(camera_dev, captured);
		if (ret < 0) {
			printk("frame requeue failed: %d\n", ret);
			break;
		}
	}

	video_stream_stop(camera_dev, VIDEO_BUF_TYPE_OUTPUT);
	printk("stream stopped\n");

	for (int index = 0; index < FRAME_BUFFER_COUNT; index++) {
		if (frame_buffers[index] != NULL) {
			video_buffer_release(frame_buffers[index]);
			frame_buffers[index] = NULL;
		}
	}

	return 0;
}