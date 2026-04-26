#ifndef SMART_CAMERA_JPEG_DECODER_H_
#define SMART_CAMERA_JPEG_DECODER_H_

#include <stdint.h>
#include <stddef.h>

/* Decode a complete JPEG (baseline, RGB888 or YCbCr) into a 320x240 RGB565
 * buffer.
 *
 *   in        Pointer to JPEG bytes.
 *   in_len    Length of JPEG bytes.
 *   out_565   Pointer to caller-provided 320*240*2 = 153600 byte buffer.
 *             Bytes are written big-endian (matches LV_COLOR_16_SWAP=y).
 *
 * Returns 0 on success, negative errno on failure.
 *
 * Pixels outside the source image (smaller than 320x240) and pixels of
 * sources larger than 320x240 are clipped/ignored without scaling.
 */
int jpeg_decode_to_rgb565(const uint8_t *in, size_t in_len, uint8_t *out_565);

/* Returns the (small) work-pool size used internally; for diagnostic logging. */
size_t jpeg_decoder_workpool_bytes(void);

#endif /* SMART_CAMERA_JPEG_DECODER_H_ */
