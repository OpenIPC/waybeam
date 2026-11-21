#include "venc_fec.h"

#include "rs_fec.h"

#include <stdlib.h>
#include <string.h>

#define RTP_HEADER_BYTES 12
#define PAR_ROW (VENC_FEC_HDR + VENC_FEC_SYM_MAX) /* one parity datagram */
#define ARENA_SRC_BYTES ((size_t)VENC_FEC_MAX_SRC * VENC_FEC_SYM_MAX)

static uint16_t read_be16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v & 0xff);
}

int venc_fec_set(VencFec *fec, int on)
{
	if (on && !fec->arena) {
		/* The frame's sources, then one block's parity rows (r <= k). */
		fec->arena = malloc(ARENA_SRC_BYTES +
			(size_t)VENC_FEC_BLOCK_MAX * PAR_ROW);
		if (!fec->arena)
			return -1;
		fec->rtp.payload_type = VENC_FEC_PT;
	}
	/* Release: the producer acquires this at begin_frame before it
	 * touches the arena set up above. */
	__atomic_store_n(&fec->on, on ? 1 : 0, __ATOMIC_RELEASE);
	return 0;
}

void venc_fec_begin_frame(VencFec *fec)
{
	fec->count = 0;
	/* Latched per frame so a live toggle never splits one. */
	fec->frame = __atomic_load_n(&fec->on, __ATOMIC_ACQUIRE);
}

/* Keep a packet's payload as a source symbol: 2 B length, then the payload
 * (emit pads it to the block's longest).  The first packet that cannot be
 * kept ends capture for the frame (frame = 2): later ones would break the
 * base + i pairing, and no block may then claim to end the frame. */
void venc_fec_capture(VencFec *fec, const uint8_t *header,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len)
{
	const size_t len = payload1_len + payload2_len;
	uint8_t *sym;

	if (fec->frame != 1 || fec->count >= VENC_FEC_MAX_SRC ||
	    len > VENC_FEC_MAX_PAYLOAD_BYTES) {
		fec->frame = 2;
		__atomic_fetch_add(&fec->unprotected, 1, __ATOMIC_RELAXED);
		return;
	}
	if (fec->count == 0) {
		fec->base_seq = read_be16(header + 2);
		fec->rtp.timestamp = read_be32(header + 4);
		fec->rtp.ssrc = read_be32(header + 8) ^ VENC_FEC_SSRC_XOR;
	}
	sym = fec->arena + (size_t)fec->count * VENC_FEC_SYM_MAX;
	write_be16(sym, (uint16_t)len);
	memcpy(sym + 2, payload1, payload1_len);
	if (payload2_len)
		memcpy(sym + 2 + payload1_len, payload2, payload2_len);
	fec->count++;
}

/* Blocks of at most VENC_FEC_BLOCK_MAX sources; r per block from a byte
 * credit worth 1/VENC_FEC_PARITY_DIV of the source datagrams, so a small
 * block may get none and small frames share protection instead of
 * overspending. */
void venc_fec_emit(VencFec *fec, RtpPacketizerWriteFn write, void *opaque)
{
	const unsigned total = fec->count;
	const unsigned nb = (total + VENC_FEC_BLOCK_MAX - 1) / VENC_FEC_BLOCK_MAX;
	uint8_t *const par = fec->arena + ARENA_SRC_BYTES;
	unsigned b, i, first = 0;

	for (b = 0; b < nb; b++) {
		const unsigned k = total / nb + (b < total % nb);
		const uint8_t *src = fec->arena +
			(size_t)first * VENC_FEC_SYM_MAX;
		size_t maxlen = 0, src_bytes = 0, plen;
		unsigned r;

		for (i = 0; i < k; i++) {
			const size_t len = read_be16(src +
				(size_t)i * VENC_FEC_SYM_MAX);

			if (len > maxlen)
				maxlen = len;
			src_bytes += RTP_HEADER_BYTES + len;
		}
		plen = RTP_HEADER_BYTES + VENC_FEC_HDR + 2 + maxlen;
		fec->credit += (int64_t)src_bytes;
		r = (unsigned)(fec->credit /
			(int64_t)(VENC_FEC_PARITY_DIV * plen));
		if (r > k) /* more is near-copies; also keeps k + idx <= 255 */
			r = k;
		fec->credit -= (int64_t)(r * VENC_FEC_PARITY_DIV * plen);
		if (!r)
			__atomic_fetch_add(&fec->unprotected, k,
				__ATOMIC_RELAXED);
		/* Zero-pad to this block's longest only, not the 3780 cap. */
		for (i = 0; r && i < k; i++) {
			uint8_t *s = fec->arena +
				(size_t)(first + i) * VENC_FEC_SYM_MAX;
			const size_t len = read_be16(s);

			memset(s + 2 + len, 0, maxlen - len);
		}

		rs_fec_encode(k, r, src, VENC_FEC_SYM_MAX, 2 + maxlen,
			par + VENC_FEC_HDR, PAR_ROW);
		for (i = 0; i < r; i++) {
			uint8_t *row = par + (size_t)i * PAR_ROW;

			row[0] = 0x7E; /* H.265 NAL type 63: decoders ignore it */
			row[1] = (uint8_t)((b == nb - 1 && fec->frame == 1 ?
				0x20 : 0) | 0x01);
			row[2] = (uint8_t)k;
			row[3] = (uint8_t)i;
			write_be16(row + 4, (uint16_t)(fec->base_seq + first));
			(void)rtp_packetizer_send_packet(&fec->rtp, write,
				opaque, row, VENC_FEC_HDR + 2 + maxlen,
				NULL, 0, 0);
		}
		first += k;
	}
}

void venc_fec_free(VencFec *fec)
{
	fec->on = 0;
	fec->frame = 0;
	free(fec->arena);
	fec->arena = NULL;
}
