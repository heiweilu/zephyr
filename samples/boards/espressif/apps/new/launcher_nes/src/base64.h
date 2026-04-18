/*
 * base64.h — Base64 encode/decode for audio data
 */
#ifndef BASE64_H
#define BASE64_H

#include <stdint.h>
#include <stddef.h>

/* Encode binary data to base64 string.
 * out_buf must be at least ((in_len + 2) / 3) * 4 + 1 bytes.
 * Returns length of encoded string (not counting NUL). */
size_t base64_encode(const uint8_t *in, size_t in_len,
		     char *out, size_t out_max);

/* Decode base64 string to binary data.
 * out_buf must be at least (in_len / 4) * 3 bytes.
 * Returns number of decoded bytes, or negative on error. */
int base64_decode(const char *in, size_t in_len,
		  uint8_t *out, size_t out_max);

#endif /* BASE64_H */
