/*
 * jpeg_enc.h — minimal in-memory JPEG encoder (ported from Jon Olick's
 * public-domain jo_jpeg.cpp v1.61).
 *
 * Encodes RGB888 (3 bytes/pixel, packed RGB order) into a baseline JPEG
 * written to a caller-supplied output buffer.
 *
 *   width, height : must both be multiples of 16 (we use chroma subsampling)
 *   quality       : 1..100 ; <=90 enables 4:2:0 subsampling for smaller files
 *
 * Returns total JPEG bytes written, or -1 if `out_cap` is too small.
 */
#ifndef JPEG_ENC_H
#define JPEG_ENC_H

#include <stdint.h>

int jpeg_encode_rgb888(uint8_t *out, int out_cap,
		       const uint8_t *rgb, int width, int height, int quality);

#endif /* JPEG_ENC_H */
