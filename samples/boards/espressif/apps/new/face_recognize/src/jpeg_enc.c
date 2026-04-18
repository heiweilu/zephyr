/*
 * jpeg_enc.c — in-memory port of Jon Olick's public-domain jo_jpeg.cpp v1.61.
 * Original: http://jonolick.com  (public domain)
 *
 * Differences vs upstream:
 *   - FILE* output replaced with a (uint8_t *out, int cap, int pos) buffer.
 *   - C++ kept-style const arrays converted to plain C.
 *   - Returns number of bytes written, or -1 on out-of-buffer.
 *   - Only RGB888 input supported (comp = 3).
 *   - Width/height must be multiples of 16 (we always use chroma subsampling).
 */

#include "jpeg_enc.h"
#include <math.h>
#include <string.h>

struct out_buf {
	uint8_t *buf;
	int      cap;
	int      pos;
	int      overflow;
};

static void put_u8(struct out_buf *o, uint8_t b)
{
	if (o->pos < o->cap) {
		o->buf[o->pos] = b;
	} else {
		o->overflow = 1;
	}
	o->pos++;
}

static void put_bytes(struct out_buf *o, const void *data, int len)
{
	const uint8_t *p = data;
	for (int i = 0; i < len; i++) {
		put_u8(o, p[i]);
	}
}

static const uint8_t s_jo_ZigZag[] = {
	 0, 1, 5, 6,14,15,27,28,
	 2, 4, 7,13,16,26,29,42,
	 3, 8,12,17,25,30,41,43,
	 9,11,18,24,31,40,44,53,
	10,19,23,32,39,45,52,54,
	20,22,33,38,46,51,55,60,
	21,34,37,47,50,56,59,61,
	35,36,48,49,57,58,62,63
};

static void jo_writeBits(struct out_buf *o, int *bitBuf, int *bitCnt,
			 const unsigned short *bs)
{
	*bitCnt += bs[1];
	*bitBuf |= bs[0] << (24 - *bitCnt);
	while (*bitCnt >= 8) {
		uint8_t c = (*bitBuf >> 16) & 255;
		put_u8(o, c);
		if (c == 255) {
			put_u8(o, 0);
		}
		*bitBuf <<= 8;
		*bitCnt -= 8;
	}
}

static void jo_DCT(float *d0, float *d1, float *d2, float *d3,
		   float *d4, float *d5, float *d6, float *d7)
{
	float tmp0 = *d0 + *d7;
	float tmp7 = *d0 - *d7;
	float tmp1 = *d1 + *d6;
	float tmp6 = *d1 - *d6;
	float tmp2 = *d2 + *d5;
	float tmp5 = *d2 - *d5;
	float tmp3 = *d3 + *d4;
	float tmp4 = *d3 - *d4;

	float tmp10 = tmp0 + tmp3;
	float tmp13 = tmp0 - tmp3;
	float tmp11 = tmp1 + tmp2;
	float tmp12 = tmp1 - tmp2;

	*d0 = tmp10 + tmp11;
	*d4 = tmp10 - tmp11;

	float z1 = (tmp12 + tmp13) * 0.707106781f;
	*d2 = tmp13 + z1;
	*d6 = tmp13 - z1;

	tmp10 = tmp4 + tmp5;
	tmp11 = tmp5 + tmp6;
	tmp12 = tmp6 + tmp7;

	float z5 = (tmp10 - tmp12) * 0.382683433f;
	float z2 = tmp10 * 0.541196100f + z5;
	float z4 = tmp12 * 1.306562965f + z5;
	float z3 = tmp11 * 0.707106781f;

	float z11 = tmp7 + z3;
	float z13 = tmp7 - z3;

	*d5 = z13 + z2;
	*d3 = z13 - z2;
	*d1 = z11 + z4;
	*d7 = z11 - z4;
}

static void jo_calcBits(int val, unsigned short bits[2])
{
	int tmp1 = val < 0 ? -val : val;
	val = val < 0 ? val - 1 : val;
	bits[1] = 1;
	while (tmp1 >>= 1) {
		++bits[1];
	}
	bits[0] = val & ((1 << bits[1]) - 1);
}

static int jo_processDU(struct out_buf *o, int *bitBuf, int *bitCnt,
			float *CDU, int du_stride, float *fdtbl, int DC,
			const unsigned short HTDC[256][2],
			const unsigned short HTAC[256][2])
{
	const unsigned short EOB[2]       = { HTAC[0x00][0], HTAC[0x00][1] };
	const unsigned short M16zeroes[2] = { HTAC[0xF0][0], HTAC[0xF0][1] };

	for (int i = 0; i < du_stride * 8; i += du_stride) {
		jo_DCT(&CDU[i], &CDU[i + 1], &CDU[i + 2], &CDU[i + 3],
		       &CDU[i + 4], &CDU[i + 5], &CDU[i + 6], &CDU[i + 7]);
	}
	for (int i = 0; i < 8; ++i) {
		jo_DCT(&CDU[i],                    &CDU[i + du_stride],
		       &CDU[i + du_stride * 2],    &CDU[i + du_stride * 3],
		       &CDU[i + du_stride * 4],    &CDU[i + du_stride * 5],
		       &CDU[i + du_stride * 6],    &CDU[i + du_stride * 7]);
	}

	int DU[64];
	for (int y = 0, j = 0; y < 8; ++y) {
		for (int x = 0; x < 8; ++x, ++j) {
			int i = y * du_stride + x;
			float v = CDU[i] * fdtbl[j];
			DU[s_jo_ZigZag[j]] = (int)(v < 0 ? ceilf(v - 0.5f)
						       : floorf(v + 0.5f));
		}
	}

	int diff = DU[0] - DC;
	if (diff == 0) {
		jo_writeBits(o, bitBuf, bitCnt, HTDC[0]);
	} else {
		unsigned short bits[2];
		jo_calcBits(diff, bits);
		jo_writeBits(o, bitBuf, bitCnt, HTDC[bits[1]]);
		jo_writeBits(o, bitBuf, bitCnt, bits);
	}

	int end0pos = 63;
	for (; end0pos > 0 && DU[end0pos] == 0; --end0pos) {
	}
	if (end0pos == 0) {
		jo_writeBits(o, bitBuf, bitCnt, EOB);
		return DU[0];
	}
	for (int i = 1; i <= end0pos; ++i) {
		int startpos = i;
		for (; DU[i] == 0 && i <= end0pos; ++i) {
		}
		int nrzeroes = i - startpos;
		if (nrzeroes >= 16) {
			int lng = nrzeroes >> 4;
			for (int nrmarker = 1; nrmarker <= lng; ++nrmarker) {
				jo_writeBits(o, bitBuf, bitCnt, M16zeroes);
			}
			nrzeroes &= 15;
		}
		unsigned short bits[2];
		jo_calcBits(DU[i], bits);
		jo_writeBits(o, bitBuf, bitCnt,
			     HTAC[(nrzeroes << 4) + bits[1]]);
		jo_writeBits(o, bitBuf, bitCnt, bits);
	}
	if (end0pos != 63) {
		jo_writeBits(o, bitBuf, bitCnt, EOB);
	}
	return DU[0];
}

static inline void jo_calcYUV(float *y, float *u, float *v,
			      uint8_t r, uint8_t g, uint8_t b)
{
	*y = +0.29900f * r + 0.58700f * g + 0.11400f * b - 128;
	*u = -0.16874f * r - 0.33126f * g + 0.50000f * b;
	*v = +0.50000f * r - 0.41869f * g - 0.08131f * b;
}

int jpeg_encode_rgb888(uint8_t *out, int out_cap,
		       const uint8_t *rgb, int width, int height, int quality)
{
	static const uint8_t std_dc_luminance_nrcodes[]   = {0,0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
	static const uint8_t std_dc_luminance_values[]    = {0,1,2,3,4,5,6,7,8,9,10,11};
	static const uint8_t std_ac_luminance_nrcodes[]   = {0,0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
	static const uint8_t std_ac_luminance_values[] = {
		0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
		0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
		0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
		0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
		0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
		0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
		0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
		0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
		0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
		0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
		0xf9,0xfa
	};
	static const uint8_t std_dc_chrominance_nrcodes[] = {0,0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
	static const uint8_t std_dc_chrominance_values[]  = {0,1,2,3,4,5,6,7,8,9,10,11};
	static const uint8_t std_ac_chrominance_nrcodes[] = {0,0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
	static const uint8_t std_ac_chrominance_values[] = {
		0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
		0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
		0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
		0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
		0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
		0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
		0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
		0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
		0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
		0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
		0xf9,0xfa
	};
	static const unsigned short YDC_HT[256][2] = {
		{0,2},{2,3},{3,3},{4,3},{5,3},{6,3},{14,4},{30,5},{62,6},{126,7},{254,8},{510,9}
	};
	static const unsigned short UVDC_HT[256][2] = {
		{0,2},{1,2},{2,2},{6,3},{14,4},{30,5},{62,6},{126,7},{254,8},{510,9},{1022,10},{2046,11}
	};
	static const unsigned short YAC_HT[256][2] = {
		{10,4},{0,2},{1,2},{4,3},{11,4},{26,5},{120,7},{248,8},{1014,10},{65410,16},{65411,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{12,4},{27,5},{121,7},{502,9},{2038,11},{65412,16},{65413,16},{65414,16},{65415,16},{65416,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{28,5},{249,8},{1015,10},{4084,12},{65417,16},{65418,16},{65419,16},{65420,16},{65421,16},{65422,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{58,6},{503,9},{4085,12},{65423,16},{65424,16},{65425,16},{65426,16},{65427,16},{65428,16},{65429,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{59,6},{1016,10},{65430,16},{65431,16},{65432,16},{65433,16},{65434,16},{65435,16},{65436,16},{65437,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{122,7},{2039,11},{65438,16},{65439,16},{65440,16},{65441,16},{65442,16},{65443,16},{65444,16},{65445,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{123,7},{4086,12},{65446,16},{65447,16},{65448,16},{65449,16},{65450,16},{65451,16},{65452,16},{65453,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{250,8},{4087,12},{65454,16},{65455,16},{65456,16},{65457,16},{65458,16},{65459,16},{65460,16},{65461,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{504,9},{32704,15},{65462,16},{65463,16},{65464,16},{65465,16},{65466,16},{65467,16},{65468,16},{65469,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{505,9},{65470,16},{65471,16},{65472,16},{65473,16},{65474,16},{65475,16},{65476,16},{65477,16},{65478,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{506,9},{65479,16},{65480,16},{65481,16},{65482,16},{65483,16},{65484,16},{65485,16},{65486,16},{65487,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{1017,10},{65488,16},{65489,16},{65490,16},{65491,16},{65492,16},{65493,16},{65494,16},{65495,16},{65496,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{1018,10},{65497,16},{65498,16},{65499,16},{65500,16},{65501,16},{65502,16},{65503,16},{65504,16},{65505,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{2040,11},{65506,16},{65507,16},{65508,16},{65509,16},{65510,16},{65511,16},{65512,16},{65513,16},{65514,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{65515,16},{65516,16},{65517,16},{65518,16},{65519,16},{65520,16},{65521,16},{65522,16},{65523,16},{65524,16},{0,0},{0,0},{0,0},{0,0},{0,0},
		{2041,11},{65525,16},{65526,16},{65527,16},{65528,16},{65529,16},{65530,16},{65531,16},{65532,16},{65533,16},{65534,16},{0,0},{0,0},{0,0},{0,0},{0,0}
	};
	static const unsigned short UVAC_HT[256][2] = {
		{0,2},{1,2},{4,3},{10,4},{24,5},{25,5},{56,6},{120,7},{500,9},{1014,10},{4084,12},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{11,4},{57,6},{246,8},{501,9},{2038,11},{4085,12},{65416,16},{65417,16},{65418,16},{65419,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{26,5},{247,8},{1015,10},{4086,12},{32706,15},{65420,16},{65421,16},{65422,16},{65423,16},{65424,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{27,5},{248,8},{1016,10},{4087,12},{65425,16},{65426,16},{65427,16},{65428,16},{65429,16},{65430,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{58,6},{502,9},{65431,16},{65432,16},{65433,16},{65434,16},{65435,16},{65436,16},{65437,16},{65438,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{59,6},{1017,10},{65439,16},{65440,16},{65441,16},{65442,16},{65443,16},{65444,16},{65445,16},{65446,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{121,7},{2039,11},{65447,16},{65448,16},{65449,16},{65450,16},{65451,16},{65452,16},{65453,16},{65454,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{122,7},{2040,11},{65455,16},{65456,16},{65457,16},{65458,16},{65459,16},{65460,16},{65461,16},{65462,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{249,8},{65463,16},{65464,16},{65465,16},{65466,16},{65467,16},{65468,16},{65469,16},{65470,16},{65471,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{503,9},{65472,16},{65473,16},{65474,16},{65475,16},{65476,16},{65477,16},{65478,16},{65479,16},{65480,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{504,9},{65481,16},{65482,16},{65483,16},{65484,16},{65485,16},{65486,16},{65487,16},{65488,16},{65489,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{505,9},{65490,16},{65491,16},{65492,16},{65493,16},{65494,16},{65495,16},{65496,16},{65497,16},{65498,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{506,9},{65499,16},{65500,16},{65501,16},{65502,16},{65503,16},{65504,16},{65505,16},{65506,16},{65507,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{2041,11},{65508,16},{65509,16},{65510,16},{65511,16},{65512,16},{65513,16},{65514,16},{65515,16},{65516,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
		{16352,14},{65517,16},{65518,16},{65519,16},{65520,16},{65521,16},{65522,16},{65523,16},{65524,16},{65525,16},{0,0},{0,0},{0,0},{0,0},{0,0},
		{1018,10},{32707,15},{65526,16},{65527,16},{65528,16},{65529,16},{65530,16},{65531,16},{65532,16},{65533,16},{65534,16},{0,0},{0,0},{0,0},{0,0},{0,0}
	};
	static const int YQT[] = {
		16, 11, 10, 16, 24, 40, 51, 61,
		12, 12, 14, 19, 26, 58, 60, 55,
		14, 13, 16, 24, 40, 57, 69, 56,
		14, 17, 22, 29, 51, 87, 80, 62,
		18, 22, 37, 56, 68,109,103, 77,
		24, 35, 55, 64, 81,104,113, 92,
		49, 64, 78, 87,103,121,120,101,
		72, 92, 95, 98,112,100,103, 99
	};
	static const int UVQT[] = {
		17, 18, 24, 47, 99, 99, 99, 99,
		18, 21, 26, 66, 99, 99, 99, 99,
		24, 26, 56, 99, 99, 99, 99, 99,
		47, 66, 99, 99, 99, 99, 99, 99,
		99, 99, 99, 99, 99, 99, 99, 99,
		99, 99, 99, 99, 99, 99, 99, 99,
		99, 99, 99, 99, 99, 99, 99, 99,
		99, 99, 99, 99, 99, 99, 99, 99
	};
	static const float aasf[] = {
		1.0f          * 2.828427125f,
		1.387039845f  * 2.828427125f,
		1.306562965f  * 2.828427125f,
		1.175875602f  * 2.828427125f,
		1.0f          * 2.828427125f,
		0.785694958f  * 2.828427125f,
		0.541196100f  * 2.828427125f,
		0.275899379f  * 2.828427125f
	};

	if (!out || !rgb || width <= 0 || height <= 0 || (width & 15) || (height & 15)) {
		return -1;
	}

	struct out_buf o = { .buf = out, .cap = out_cap, .pos = 0, .overflow = 0 };

	int qf = quality ? quality : 90;
	int subsample = qf <= 90 ? 1 : 0;
	qf = qf < 1 ? 1 : qf > 100 ? 100 : qf;
	qf = qf < 50 ? 5000 / qf : 200 - qf * 2;

	uint8_t YTable[64], UVTable[64];
	for (int i = 0; i < 64; ++i) {
		int yti  = (YQT[i]  * qf + 50) / 100;
		YTable [s_jo_ZigZag[i]] = yti  < 1 ? 1 : yti  > 255 ? 255 : yti;
		int uvti = (UVQT[i] * qf + 50) / 100;
		UVTable[s_jo_ZigZag[i]] = uvti < 1 ? 1 : uvti > 255 ? 255 : uvti;
	}

	float fdtbl_Y[64], fdtbl_UV[64];
	for (int row = 0, k = 0; row < 8; ++row) {
		for (int col = 0; col < 8; ++col, ++k) {
			fdtbl_Y [k] = 1.0f / (YTable [s_jo_ZigZag[k]] * aasf[row] * aasf[col]);
			fdtbl_UV[k] = 1.0f / (UVTable[s_jo_ZigZag[k]] * aasf[row] * aasf[col]);
		}
	}

	/* Headers */
	static const uint8_t head0[] = {
		0xFF, 0xD8,
		0xFF, 0xE0, 0, 0x10, 'J','F','I','F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0,
		0xFF, 0xDB, 0, 0x84, 0
	};
	put_bytes(&o, head0, sizeof(head0));
	put_bytes(&o, YTable, sizeof(YTable));
	put_u8(&o, 1);
	put_bytes(&o, UVTable, sizeof(UVTable));

	const uint8_t head1[] = {
		0xFF, 0xC0, 0, 0x11, 8,
		(uint8_t)(height >> 8), (uint8_t)(height & 0xFF),
		(uint8_t)(width  >> 8), (uint8_t)(width  & 0xFF),
		3,
		1, (uint8_t)(subsample ? 0x22 : 0x11), 0,
		2, 0x11, 1,
		3, 0x11, 1,
		0xFF, 0xC4, 0x01, 0xA2, 0
	};
	put_bytes(&o, head1, sizeof(head1));
	put_bytes(&o, std_dc_luminance_nrcodes + 1, sizeof(std_dc_luminance_nrcodes) - 1);
	put_bytes(&o, std_dc_luminance_values,      sizeof(std_dc_luminance_values));
	put_u8(&o, 0x10);
	put_bytes(&o, std_ac_luminance_nrcodes + 1, sizeof(std_ac_luminance_nrcodes) - 1);
	put_bytes(&o, std_ac_luminance_values,      sizeof(std_ac_luminance_values));
	put_u8(&o, 1);
	put_bytes(&o, std_dc_chrominance_nrcodes + 1, sizeof(std_dc_chrominance_nrcodes) - 1);
	put_bytes(&o, std_dc_chrominance_values,      sizeof(std_dc_chrominance_values));
	put_u8(&o, 0x11);
	put_bytes(&o, std_ac_chrominance_nrcodes + 1, sizeof(std_ac_chrominance_nrcodes) - 1);
	put_bytes(&o, std_ac_chrominance_values,      sizeof(std_ac_chrominance_values));
	static const uint8_t head2[] = { 0xFF, 0xDA, 0, 0xC, 3, 1, 0, 2, 0x11, 3, 0x11, 0, 0x3F, 0 };
	put_bytes(&o, head2, sizeof(head2));

	const int comp = 3;
	int bitBuf = 0, bitCnt = 0;
	int DCY = 0, DCU = 0, DCV = 0;

	if (subsample) {
		float Y[256], U[256], V[256];
		for (int y = 0; y < height; y += 16) {
			for (int x = 0; x < width; x += 16) {
				int pos;
				float subU[64], subV[64];
				for (int row = y; row < y + 16; ++row) {
					for (int col = x; col < x + 16; ++col) {
						int prow = row >= height ? height - 1 : row;
						int pcol = col >= width  ? width  - 1 : col;
						int p = prow * width * comp + pcol * comp;
						pos = (row - y) * 16 + (col - x);
						jo_calcYUV(&Y[pos], &U[pos], &V[pos],
							   rgb[p], rgb[p + 1], rgb[p + 2]);
					}
				}
				DCY = jo_processDU(&o, &bitBuf, &bitCnt, Y +   0, 16, fdtbl_Y, DCY, YDC_HT, YAC_HT);
				DCY = jo_processDU(&o, &bitBuf, &bitCnt, Y +   8, 16, fdtbl_Y, DCY, YDC_HT, YAC_HT);
				DCY = jo_processDU(&o, &bitBuf, &bitCnt, Y + 128, 16, fdtbl_Y, DCY, YDC_HT, YAC_HT);
				DCY = jo_processDU(&o, &bitBuf, &bitCnt, Y + 136, 16, fdtbl_Y, DCY, YDC_HT, YAC_HT);
				for (int yy = 0, p = 0; yy < 8; ++yy) {
					for (int xx = 0; xx < 8; ++xx, ++p) {
						int j = yy * 32 + xx * 2;
						subU[p] = (U[j + 0] + U[j + 1] + U[j + 16] + U[j + 17]) * 0.25f;
						subV[p] = (V[j + 0] + V[j + 1] + V[j + 16] + V[j + 17]) * 0.25f;
					}
				}
				DCU = jo_processDU(&o, &bitBuf, &bitCnt, subU, 8, fdtbl_UV, DCU, UVDC_HT, UVAC_HT);
				DCV = jo_processDU(&o, &bitBuf, &bitCnt, subV, 8, fdtbl_UV, DCV, UVDC_HT, UVAC_HT);
			}
		}
	} else {
		float Y[64], U[64], V[64];
		for (int y = 0; y < height; y += 8) {
			for (int x = 0; x < width; x += 8) {
				int pos;
				for (int row = y; row < y + 8; ++row) {
					for (int col = x; col < x + 8; ++col) {
						int prow = row >= height ? height - 1 : row;
						int pcol = col >= width  ? width  - 1 : col;
						int p = prow * width * comp + pcol * comp;
						pos = (row - y) * 8 + (col - x);
						jo_calcYUV(&Y[pos], &U[pos], &V[pos],
							   rgb[p], rgb[p + 1], rgb[p + 2]);
					}
				}
				DCY = jo_processDU(&o, &bitBuf, &bitCnt, Y, 8, fdtbl_Y,  DCY, YDC_HT, YAC_HT);
				DCU = jo_processDU(&o, &bitBuf, &bitCnt, U, 8, fdtbl_UV, DCU, UVDC_HT, UVAC_HT);
				DCV = jo_processDU(&o, &bitBuf, &bitCnt, V, 8, fdtbl_UV, DCV, UVDC_HT, UVAC_HT);
			}
		}
	}

	static const unsigned short fillBits[] = { 0x7F, 7 };
	jo_writeBits(&o, &bitBuf, &bitCnt, fillBits);
	put_u8(&o, 0xFF);
	put_u8(&o, 0xD9);

	return o.overflow ? -1 : o.pos;
}
