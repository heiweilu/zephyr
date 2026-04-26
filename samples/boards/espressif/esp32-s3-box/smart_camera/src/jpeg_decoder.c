/*
 * smart_camera JPEG decoder wrapper.
 *
 * Backed by LVGL's bundled TJpgDec (C ChaN, public-domain). The CMakeLists
 * adds the lvgl tjpgd dir to the include path so we can call jd_prepare /
 * jd_decomp directly. We enable CONFIG_LV_USE_TJPGD=y in prj.conf so the
 * tjpgd.c symbols are compiled into the image.
 *
 * tjpgd outputs RGB888 (24-bit). We convert to RGB565 byte-swapped on the fly
 * inside the output callback to match the LCD pipeline (LV_COLOR_16_SWAP=y).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

#include "tjpgd.h"
#include "jpeg_decoder.h"

LOG_MODULE_REGISTER(jpeg_dec, LOG_LEVEL_INF);

#define OUT_W 320
#define OUT_H 240

/* tjpgd work pool. ~3.5 KB is plenty for 320x240 baseline JPEGs. */
#define WORKPOOL_SIZE 4096

struct in_ctx {
	const uint8_t *buf;
	size_t len;
	size_t pos;
};

struct out_ctx {
	uint8_t *out;          /* 320*240*2 RGB565 LE-swapped */
	uint16_t out_w;
	uint16_t out_h;
};

struct dec_ctx {
	struct in_ctx in;
	struct out_ctx out;
};

static size_t in_func(JDEC *jd, uint8_t *dst, size_t len)
{
	struct dec_ctx *ctx = jd->device;
	size_t remain = ctx->in.len - ctx->in.pos;
	size_t n = MIN(len, remain);

	if (dst) {
		memcpy(dst, ctx->in.buf + ctx->in.pos, n);
	}
	ctx->in.pos += n;
	return n;
}

static int out_func(JDEC *jd, void *bitmap, JRECT *rect)
{
	struct dec_ctx *ctx = jd->device;
	const uint8_t *src = bitmap;            /* RGB888 row-major */
	uint16_t bw = rect->right - rect->left + 1;
	uint16_t bh = rect->bottom - rect->top + 1;

	/* Skip MCUs that are entirely outside the LCD area. */
	if (rect->left >= ctx->out.out_w || rect->top >= ctx->out.out_h) {
		return 1;
	}

	uint16_t copy_w = MIN(bw, ctx->out.out_w - rect->left);
	uint16_t copy_h = MIN(bh, ctx->out.out_h - rect->top);

	for (uint16_t y = 0; y < copy_h; y++) {
		const uint8_t *sp = src + (uint32_t)y * bw * 3;
		uint8_t *dp = ctx->out.out +
			      ((uint32_t)(rect->top + y) * ctx->out.out_w +
			       rect->left) * 2;
		for (uint16_t x = 0; x < copy_w; x++) {
			uint8_t r = sp[0];
			uint8_t g = sp[1];
			uint8_t b = sp[2];
			/* LCD controller expects BGR565 (R/B swapped vs standard
			 * RGB565). Pack as BGR then write LE bytes.
			 */
			uint16_t px = ((b & 0xF8) << 8) |
				      ((g & 0xFC) << 3) |
				      (r >> 3);
			dp[0] = (uint8_t)(px & 0xFF);
			dp[1] = (uint8_t)(px >> 8);
			sp += 3;
			dp += 2;
		}
	}
	return 1;
}

int jpeg_decode_to_rgb565(const uint8_t *in, size_t in_len, uint8_t *out_565)
{
	if (!in || !out_565 || in_len < 2) {
		return -EINVAL;
	}

	static uint8_t workpool[WORKPOOL_SIZE];
	JDEC jd = {0};
	struct dec_ctx ctx = {
		.in = { .buf = in, .len = in_len, .pos = 0 },
		.out = { .out = out_565, .out_w = OUT_W, .out_h = OUT_H },
	};

	JRESULT r = jd_prepare(&jd, in_func, workpool, sizeof(workpool), &ctx);
	if (r != JDR_OK) {
		LOG_DBG("jd_prepare: %d", r);
		return -EIO;
	}

	r = jd_decomp(&jd, out_func, 0);
	if (r != JDR_OK) {
		LOG_DBG("jd_decomp: %d", r);
		return -EIO;
	}

	return 0;
}

size_t jpeg_decoder_workpool_bytes(void)
{
	return WORKPOOL_SIZE;
}
