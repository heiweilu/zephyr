/*
 * base64.c — Base64 encode/decode
 */

#include "base64.h"

static const char b64_enc_table[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const uint8_t b64_dec_table[256] = {
	['A'] = 0,  ['B'] = 1,  ['C'] = 2,  ['D'] = 3,  ['E'] = 4,
	['F'] = 5,  ['G'] = 6,  ['H'] = 7,  ['I'] = 8,  ['J'] = 9,
	['K'] = 10, ['L'] = 11, ['M'] = 12, ['N'] = 13, ['O'] = 14,
	['P'] = 15, ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19,
	['U'] = 20, ['V'] = 21, ['W'] = 22, ['X'] = 23, ['Y'] = 24,
	['Z'] = 25, ['a'] = 26, ['b'] = 27, ['c'] = 28, ['d'] = 29,
	['e'] = 30, ['f'] = 31, ['g'] = 32, ['h'] = 33, ['i'] = 34,
	['j'] = 35, ['k'] = 36, ['l'] = 37, ['m'] = 38, ['n'] = 39,
	['o'] = 40, ['p'] = 41, ['q'] = 42, ['r'] = 43, ['s'] = 44,
	['t'] = 45, ['u'] = 46, ['v'] = 47, ['w'] = 48, ['x'] = 49,
	['y'] = 50, ['z'] = 51, ['0'] = 52, ['1'] = 53, ['2'] = 54,
	['3'] = 55, ['4'] = 56, ['5'] = 57, ['6'] = 58, ['7'] = 59,
	['8'] = 60, ['9'] = 61, ['+'] = 62, ['/'] = 63,
};

size_t base64_encode(const uint8_t *in, size_t in_len,
		     char *out, size_t out_max)
{
	size_t out_len = ((in_len + 2) / 3) * 4;
	if (out_len >= out_max) {
		return 0;
	}

	size_t i, j;
	for (i = 0, j = 0; i + 2 < in_len; i += 3, j += 4) {
		uint32_t v = ((uint32_t)in[i] << 16) |
			     ((uint32_t)in[i + 1] << 8) |
			     (uint32_t)in[i + 2];
		out[j]     = b64_enc_table[(v >> 18) & 0x3F];
		out[j + 1] = b64_enc_table[(v >> 12) & 0x3F];
		out[j + 2] = b64_enc_table[(v >> 6) & 0x3F];
		out[j + 3] = b64_enc_table[v & 0x3F];
	}

	if (i < in_len) {
		uint32_t v = (uint32_t)in[i] << 16;
		if (i + 1 < in_len) {
			v |= (uint32_t)in[i + 1] << 8;
		}
		out[j]     = b64_enc_table[(v >> 18) & 0x3F];
		out[j + 1] = b64_enc_table[(v >> 12) & 0x3F];
		out[j + 2] = (i + 1 < in_len) ? b64_enc_table[(v >> 6) & 0x3F] : '=';
		out[j + 3] = '=';
		j += 4;
	}

	out[j] = '\0';
	return j;
}

int base64_decode(const char *in, size_t in_len,
		  uint8_t *out, size_t out_max)
{
	/* Skip trailing whitespace/newlines */
	while (in_len > 0 && (in[in_len - 1] == '\n' || in[in_len - 1] == '\r' ||
			       in[in_len - 1] == ' ')) {
		in_len--;
	}

	if (in_len == 0 || (in_len % 4) != 0) {
		return -1;
	}

	size_t out_len = (in_len / 4) * 3;
	if (in[in_len - 1] == '=') out_len--;
	if (in[in_len - 2] == '=') out_len--;

	if (out_len > out_max) {
		return -2;
	}

	size_t i, j;
	for (i = 0, j = 0; i < in_len; i += 4) {
		uint32_t a = b64_dec_table[(unsigned char)in[i]];
		uint32_t b = b64_dec_table[(unsigned char)in[i + 1]];
		uint32_t c = b64_dec_table[(unsigned char)in[i + 2]];
		uint32_t d = b64_dec_table[(unsigned char)in[i + 3]];

		uint32_t v = (a << 18) | (b << 12) | (c << 6) | d;

		if (j < out_len) out[j++] = (v >> 16) & 0xFF;
		if (j < out_len) out[j++] = (v >> 8) & 0xFF;
		if (j < out_len) out[j++] = v & 0xFF;
	}

	return (int)out_len;
}
