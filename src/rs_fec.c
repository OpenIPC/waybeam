/* Ported from waybeam-link core/src/rlc.cpp + gf256.cpp (2dd2912), relicensed
 * MIT by their author for waybeam.  See include/rs_fec.h. */
#include "rs_fec.h"

#include <pthread.h>
#include <string.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

static uint8_t g_log[256], g_exp[512];
static uint8_t g_mul_lo[256][16], g_mul_hi[256][16];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
	return (a && b) ? g_exp[g_log[a] + g_log[b]] : 0;
}

static void gf_init(void)
{
	unsigned x = 1;

	for (int i = 0; i < 255; i++) {
		g_exp[i] = (uint8_t)x;
		g_log[x] = (uint8_t)i;
		x <<= 1;
		if (x & 0x100)
			x ^= 0x11D;
	}
	/* Doubled so a log sum indexes without a modulo. */
	for (int i = 255; i < 512; i++)
		g_exp[i] = g_exp[i - 255];
	/* Nibble product tables for the NEON vtbl multiply. */
	for (int c = 0; c < 256; c++)
		for (int n = 0; n < 16; n++) {
			g_mul_lo[c][n] = gf_mul((uint8_t)c, (uint8_t)n);
			g_mul_hi[c][n] = gf_mul((uint8_t)c, (uint8_t)(n << 4));
		}
}

/* out ^= c * in, c != 0 */
static void gf_mul_xor(uint8_t c, const uint8_t *in, uint8_t *out, size_t len)
{
	size_t i = 0;

#if defined(__ARM_NEON)
	const uint8x8x2_t lo = { { vld1_u8(g_mul_lo[c]), vld1_u8(g_mul_lo[c] + 8) } };
	const uint8x8x2_t hi = { { vld1_u8(g_mul_hi[c]), vld1_u8(g_mul_hi[c] + 8) } };
	const uint8x8_t mask = vdup_n_u8(0x0f);

	for (; i + 16 <= len; i += 16) {
		const uint8x16_t v = vld1q_u8(in + i);
		const uint8x8_t a = vget_low_u8(v), b = vget_high_u8(v);
		const uint8x8_t pa = veor_u8(vtbl2_u8(lo, vand_u8(a, mask)),
			vtbl2_u8(hi, vshr_n_u8(a, 4)));
		const uint8x8_t pb = veor_u8(vtbl2_u8(lo, vand_u8(b, mask)),
			vtbl2_u8(hi, vshr_n_u8(b, 4)));
		vst1q_u8(out + i, veorq_u8(vld1q_u8(out + i), vcombine_u8(pa, pb)));
	}
#endif
	const int lc = g_log[c];
	for (; i < len; i++)
		if (in[i])
			out[i] ^= g_exp[lc + g_log[in[i]]];
}

/* All rows in one pass over the block, a 1 KB tile of every source and
 * every row at a time: a block larger than L1 is read from L2 once, not
 * once per row.  Byte-identical to one row at a time, and 9-19 % faster
 * on a Cortex-A7 (CV610, 2026-11-21: k 6-40, r 2-40, 1400-3780 B). */
#define RS_FEC_TILE 1024

void rs_fec_encode(unsigned k, unsigned r, const uint8_t *src,
	size_t stride, size_t len, uint8_t *out, size_t out_stride)
{
	pthread_once(&g_once, gf_init);
	for (unsigned j = 0; j < r; j++)
		memset(out + j * out_stride, 0, len);
	for (size_t t = 0; t < len; t += RS_FEC_TILE) {
		const size_t n = len - t < RS_FEC_TILE ? len - t : RS_FEC_TILE;

		/* Row x = k + j: x ^ i != 0 because k + j <= 255 keeps x out of
		 * 0..k-1. */
		for (unsigned i = 0; i < k; i++)
			for (unsigned j = 0; j < r; j++)
				gf_mul_xor(g_exp[255 - g_log[(uint8_t)((k + j) ^ i)]],
					src + i * stride + t,
					out + j * out_stride + t, n);
	}
}
