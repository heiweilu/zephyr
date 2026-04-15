#ifndef JPEG_ENC_H
#define JPEG_ENC_H
#include <stdint.h>

/**
 * Encode RGB565 image to JPEG.
 * @param rgb565    Input pixel data (row-major, big-endian RGB565)
 * @param width     Image width (must be > 0)
 * @param height    Image height (must be > 0)
 * @param jpeg_out  Output buffer for JPEG data
 * @param max_out   Size of output buffer
 * @return          Number of bytes written to jpeg_out, or negative on error
 */
int jpeg_encode_rgb565(const uint16_t *rgb565, int width, int height,
                       uint8_t *jpeg_out, int max_out);

#endif
