#ifndef RS_FEC_H
#define RS_FEC_H

#include <stddef.h>
#include <stdint.h>

/* Reed–Solomon parity encoder for protocols/venc-fec.md: the systematic
 * Cauchy code of waybeam-link PROTOCOL.md §14.1 over GF(256) (poly 0x11D,
 * g = 2), parity row x = k + idx, row[i] = 1 / (x ^ i).
 *
 * Writes parity symbols 0..r-1 of a k-source block, symbol idx at
 * out + idx * out_stride (`len` bytes each); source i is the `len` bytes at
 * src + i * stride.  Requires 1 <= k and k + r - 1 <= 255. */
void rs_fec_encode(unsigned k, unsigned r, const uint8_t *src,
	size_t stride, size_t len, uint8_t *out, size_t out_stride);

#endif /* RS_FEC_H */
