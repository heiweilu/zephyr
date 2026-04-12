#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
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
#define BLK_PIN (47 - 32)
#define SWAP_RGB565_FOR_DISPLAY 0

static struct video_buffer *frame_buffers[FRAME_BUFFER_COUNT];

static void swap_rgb565_bytes(struct video_buffer *vbuf)
{
	for (uint32_t index = 0; index + 1 < vbuf->bytesused; index += 2) {
		uint8_t tmp = vbuf->buffer[index];

		vbuf->buffer[index] = vbuf->buffer[index + 1];
		vbuf->buffer[index + 1] = tmp;
	}
}

static void log_frame_samples(const struct video_buffer *vbuf, const struct video_format *fmt,
			      uint32_t frame)
{
	static const uint16_t sample_x[] = {0, 80, 160, 240};
	uint32_t row_offset = (fmt->height / 2U) * fmt->pitch;

	printk("frame %u mid-row raw:", frame);
	for (size_t index = 0; index < ARRAY_SIZE(sample_x); index++) {
		uint32_t pixel_offset = row_offset + (sample_x[index] * 2U);
		uint32_t pair_offset = pixel_offset & ~0x3U;

		printk(" x%u=%02x %02x %02x %02x",
		       sample_x[index],
		       vbuf->buffer[pair_offset],
		       vbuf->buffer[pair_offset + 1U],
		       vbuf->buffer[pair_offset + 2U],
		       vbuf->buffer[pair_offset + 3U]);
	}
	printk("\n");
}

static int display_camera_frame(const struct device *display_dev,
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

int main(void)
{
	const struct device *const camera_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_camera));
	const struct device *const display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	const struct device *const gpio1_dev = DEVICE_DT_GET(DT_NODELABEL(gpio1));
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

	if (!device_is_ready(display_dev)) {
		printk("display device not ready\n");
		return 0;
	}

	if (device_is_ready(gpio1_dev)) {
		gpio_pin_configure(gpio1_dev, BLK_PIN, GPIO_OUTPUT_ACTIVE);
		gpio_pin_set(gpio1_dev, BLK_PIN, 1);
	}

	display_blanking_off(display_dev);

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

	for (uint32_t frame = 0; ; frame++) {
		ret = video_dequeue(camera_dev, &captured, K_FOREVER);
		if (ret < 0) {
			printk("frame dequeue failed: %d\n", ret);
			break;
		}

		if ((frame % 30U) == 0U) {
			printk("frame %u captured: %u bytes ts=%u first=%02x %02x %02x %02x\n",
				frame,
				captured->bytesused,
				captured->timestamp,
				captured->buffer[0],
				captured->buffer[1],
				captured->buffer[2],
				captured->buffer[3]);
			log_frame_samples(captured, &fmt, frame);
		}

		if (SWAP_RGB565_FOR_DISPLAY) {
			swap_rgb565_bytes(captured);
		}

		ret = display_camera_frame(display_dev, captured, &fmt);
		if (ret < 0) {
			printk("display_write failed: %d\n", ret);
			break;
		}

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