#ifndef VENC_FEC_H
#define VENC_FEC_H

#include "rtp_packetizer.h"
#include "venc_config.h"

#include <stddef.h>
#include <stdint.h>

/* RS parity after each frame, protocols/venc-fec.md.  Shared by every
 * backend: each one captures the video packets it sends and emits the
 * parity through its own socket path. */
#define VENC_FEC_PT 100
#define VENC_FEC_SSRC_XOR 0x52534643u
#define VENC_FEC_MAX_SRC 256    /* source packets protected per frame */
#define VENC_FEC_BLOCK_MAX 40   /* sources per block (encode-time cap) */
#define VENC_FEC_SYM_MAX (2 + VENC_FEC_MAX_PAYLOAD_BYTES) /* length + payload */
#define VENC_FEC_HDR 6

/* Producer thread only, except `on` (venc_fec_set, latched into `frame` at
 * begin_frame) and the two counters (atomic relaxed, read by the API). */
typedef struct {
	int on;
	int frame;      /* this frame: 0 off, 1 capturing, 2 capture ended */
	uint8_t *arena; /* allocated on first enable, kept until venc_fec_free */
	uint16_t count;
	uint16_t base_seq;
	RtpPacketizerState rtp;
	int64_t credit; /* parity bytes owed, x VENC_FEC_PARITY_DIV */
	uint32_t parity_pkts; /* counted by the backend as parity reaches the socket */
	uint32_t unprotected;
} VencFec;

/* Live toggle.  The arena is allocated on the first enable; the producer
 * picks the flag up at its next begin_frame.  -1 if the allocation fails. */
int venc_fec_set(VencFec *fec, int on);
void venc_fec_begin_frame(VencFec *fec);
/* Keep a video packet that went out (or was counted as dropped) with its
 * sequence number used: source i must be seq base_seq + i.  Call only while
 * fec->frame is set. */
void venc_fec_capture(VencFec *fec, const uint8_t *header,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len);
/* Send this frame's parity through `write`. */
void venc_fec_emit(VencFec *fec, RtpPacketizerWriteFn write, void *opaque);
void venc_fec_free(VencFec *fec);

#endif /* VENC_FEC_H */
