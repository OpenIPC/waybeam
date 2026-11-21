#include "maruko_video.h"
#include "rs_fec.h"
#include "test_helpers.h"
#include "venc_rec_writer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ── maruko_video_stream_flatten: the async recorder's copy ───────────── */

static int test_maruko_flatten_matches_the_star6e_contract(void)
{
	i6c_venc_pack packs[2];
	i6c_venc_strm stream;
	uint8_t d0[16], d1[8];
	uint8_t *out;
	size_t len = 0;
	int is_idr = -1;
	int failures = 0;
	int i;

	for (i = 0; i < 16; i++) d0[i] = (uint8_t)(0x10 + i);
	for (i = 0; i < 8; i++)  d1[i] = (uint8_t)(0xA0 + i);

	memset(packs, 0, sizeof(packs));
	memset(&stream, 0, sizeof(stream));
	packs[0].data = d0; packs[0].length = 16; packs[0].packNum = 2;
	packs[0].packetInfo[0].offset = 0; packs[0].packetInfo[0].length = 4;
	packs[0].packetInfo[1].offset = 8; packs[0].packetInfo[1].length = 4;
	packs[1].data = d1; packs[1].length = 8; packs[1].offset = 2;
	packs[1].packNum = 0;
	stream.packet = packs; stream.count = 2;

	out = maruko_video_stream_flatten(&stream, &len, &is_idr);
	CHECK("mk flatten returned a buffer", out != NULL);
	CHECK("mk flatten length", len == 4 + 4 + 6);
	if (out && len == 14) {
		CHECK("mk flatten span 1", memcmp(out, d0, 4) == 0);
		CHECK("mk flatten span 2", memcmp(out + 4, d0 + 8, 4) == 0);
		CHECK("mk flatten fallback offset",
			memcmp(out + 8, d1 + 2, 6) == 0);
	} else {
		CHECK("mk flatten payload reachable", 0);
	}
	CHECK("mk flatten no IRAP", is_idr == 0);
	free(out);

	packs[0].packetInfo[0].packType.h265Nalu = 20;
	out = maruko_video_stream_flatten(&stream, &len, &is_idr);
	CHECK("mk flatten idr_n_lp detected", out && is_idr == 1);
	free(out);

	/* A partly-valid stream must be refused whole, not truncated. */
	packs[1].packNum = (unsigned int)(sizeof(packs[1].packetInfo) /
		sizeof(packs[1].packetInfo[0])) + 1;
	CHECK("mk flatten refuses a partly-valid stream",
		maruko_video_stream_flatten(&stream, &len, &is_idr) == NULL);

	CHECK("mk flatten NULL stream",
		maruko_video_stream_flatten(NULL, &len, &is_idr) == NULL);
	return failures;
}

/* Same byte cap as the star6e helper, and it has to be tested separately —
 * the two are independent implementations over different SDK types, and a
 * guard present in one says nothing about the other. */
static int test_maruko_flatten_refuses_an_access_unit_over_the_queue_cap(void)
{
	i6c_venc_pack pack;
	i6c_venc_strm stream;
	size_t oversized = VENC_REC_WRITER_MAX_BYTES + 1;
	uint8_t *big = malloc(oversized);
	size_t len = 0;
	int is_idr = -1;
	int failures = 0;

	CHECK("mk cap test allocated its stimulus", big != NULL);
	if (!big)
		return failures;
	memset(big, 0x5A, oversized);

	memset(&pack, 0, sizeof(pack));
	memset(&stream, 0, sizeof(stream));
	pack.data = big;
	pack.length = (unsigned int)oversized;
	pack.packNum = 0;
	stream.packet = &pack;
	stream.count = 1;

	CHECK("mk flatten refuses an AU over the queue cap",
		maruko_video_stream_flatten(&stream, &len, &is_idr) == NULL);
	CHECK("mk refused AU reports no length", len == 0);

	pack.length = (unsigned int)(VENC_REC_WRITER_MAX_BYTES - 1);
	{
		uint8_t *out = maruko_video_stream_flatten(&stream, &len,
			&is_idr);

		CHECK("mk flatten accepts an AU just under the cap",
			out != NULL);
		CHECK("mk accepted AU reports its length",
			len == VENC_REC_WRITER_MAX_BYTES - 1);
		free(out);
	}
	free(big);
	return failures;
}


/* ── RS parity on the Maruko RTP path (protocols/venc-fec.md) ─────────── */

#define MK_FEC_SLICES 5
#define MK_FEC_MAX_PKTS 64

typedef struct {
	uint8_t d[MK_FEC_MAX_PKTS][4096];
	ssize_t len[MK_FEC_MAX_PKTS];
	int n;
} MkFecRx;

/* One non-IRAP access unit: MK_FEC_SLICES TRAIL_R slices of 800..1200 B,
 * one pack, one packetInfo per slice.  Payload bytes are never 0, so no
 * start code is emulated. */
static void mk_fec_frame(i6c_venc_pack *pack, i6c_venc_strm *stream,
	uint8_t *buf, unsigned frame)
{
	size_t off = 0;

	memset(pack, 0, sizeof(*pack));
	memset(stream, 0, sizeof(*stream));
	for (unsigned s = 0; s < MK_FEC_SLICES; s++) {
		const size_t len = 800 + s * 100;

		pack->packetInfo[s].offset = (MI_U32)off;
		pack->packetInfo[s].length = (MI_U32)(3 + 2 + len);
		buf[off++] = 0; buf[off++] = 0; buf[off++] = 1;
		buf[off++] = 0x02; buf[off++] = 0x01; /* TRAIL_R, tid 1 */
		for (size_t j = 0; j < len; j++)
			buf[off++] = (uint8_t)(((frame + s) * 31u + j * 7u) | 1u);
	}
	pack->data = buf;
	pack->length = (MI_U32)off;
	pack->packNum = MK_FEC_SLICES;
	stream->packet = pack;
	stream->count = 1;
}

static void mk_fec_recv(int rx, MkFecRx *r)
{
	r->n = 0;
	while (r->n < MK_FEC_MAX_PKTS) {
		r->len[r->n] = recv(rx, r->d[r->n], sizeof(r->d[0]), MSG_DONTWAIT);
		if (r->len[r->n] <= 0)
			break;
		r->n++;
	}
}

/* Re-encode every parity datagram from the source datagrams received
 * before it (symbol = 2-byte length + RTP payload, zero-padded to the
 * block's longest); return the parity count, -1 on any mismatch. */
static int mk_fec_check(const MkFecRx *r, int *sources)
{
	static uint8_t sym[VENC_FEC_BLOCK_MAX][VENC_FEC_SYM_MAX];
	static uint8_t want[VENC_FEC_BLOCK_MAX][VENC_FEC_SYM_MAX];
	int nsrc = 0, parity = 0;

	while (nsrc < r->n && (r->d[nsrc][1] & 0x7F) != VENC_FEC_PT)
		nsrc++;
	*sources = nsrc;
	for (int p = nsrc; p < r->n; p++) {
		const uint8_t *d = r->d[p];
		const unsigned k = d[14], idx = d[15];
		const uint16_t base = (uint16_t)(d[16] << 8 | d[17]);
		const uint16_t seq0 = (uint16_t)(r->d[0][2] << 8 | r->d[0][3]);
		const unsigned first = (uint16_t)(base - seq0);
		size_t maxlen = 0;

		if ((d[1] & 0x7F) != VENC_FEC_PT || d[12] != 0x7E || k == 0 ||
		    idx >= k || first + k > (unsigned)nsrc)
			return -1;
		memset(sym, 0, sizeof(sym));
		for (unsigned i = 0; i < k; i++) {
			const size_t len = (size_t)r->len[first + i] - 12;

			sym[i][0] = (uint8_t)(len >> 8);
			sym[i][1] = (uint8_t)len;
			memcpy(sym[i] + 2, r->d[first + i] + 12, len);
			if (len > maxlen)
				maxlen = len;
		}
		rs_fec_encode(k, idx + 1, sym[0], sizeof(sym[0]), 2 + maxlen,
			want[0], sizeof(want[0]));
		if (r->len[p] != (ssize_t)(12 + VENC_FEC_HDR + 2 + maxlen) ||
		    memcmp(d + 12 + VENC_FEC_HDR, want[idx], 2 + maxlen) != 0)
			return -1;
		parity++;
	}
	return parity;
}

/* The whole Maruko send path with outgoing.fec: off sends video only at
 * the configured payload; on sends parity after the frame's video, sized by
 * the slice average; a live toggle takes effect at the next frame. */
static int test_maruko_video_fec(void)
{
	static uint8_t buf[MK_FEC_SLICES * 1300];
	static MkFecRx r;
	MarukoOutput output;
	MarukoBackendConfig cfg;
	MarukoRtpState rtp;
	H26xParamSets params;
	VencOutputUri uri;
	i6c_venc_pack pack;
	i6c_venc_strm stream;
	struct sockaddr_in a = { .sin_family = AF_INET };
	socklen_t alen = sizeof(a);
	char server[64];
	int failures = 0, rx, sources = 0, parity;
	unsigned f = 0;
	uint32_t par0;

	rx = socket(AF_INET, SOCK_DGRAM, 0);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK("mk fec: receiver", rx >= 0 &&
		bind(rx, (struct sockaddr *)&a, sizeof(a)) == 0 &&
		getsockname(rx, (struct sockaddr *)&a, &alen) == 0);
	snprintf(server, sizeof(server), "udp://127.0.0.1:%u",
		(unsigned)ntohs(a.sin_port));
	memset(&output, 0, sizeof(output));
	memset(&cfg, 0, sizeof(cfg));
	memset(&params, 0, sizeof(params));
	cfg.stream_mode = MARUKO_STREAM_RTP;
	cfg.rc_codec = PT_H265;
	cfg.rtp_payload_size = 3780;
	maruko_video_init_rtp_state(&rtp, PT_H265, 60);
	CHECK("mk fec: output", venc_config_parse_output_uri(server, &uri) == 0 &&
		maruko_output_init(&output, &uri, 0, 0) == 0);

	mk_fec_frame(&pack, &stream, buf, f++);
	(void)maruko_video_send_frame(&stream, &output, &rtp, &params, &cfg, NULL);
	mk_fec_recv(rx, &r);
	CHECK("mk fec off: one datagram per slice, no parity, full payload",
		r.n == MK_FEC_SLICES && mk_fec_check(&r, &sources) == 0 &&
		output.payload_used == 3780 && !output.fec.arena);

	CHECK("mk fec: enable", maruko_output_set_fec(&output, 1) == 0);
	for (int i = 0; i < 3; i++) {
		mk_fec_frame(&pack, &stream, buf, f++);
		(void)maruko_video_send_frame(&stream, &output, &rtp, &params,
			&cfg, NULL);
		mk_fec_recv(rx, &r);
		parity = mk_fec_check(&r, &sources);
		CHECK("mk fec on: video first, then parity that re-encodes",
			sources == MK_FEC_SLICES && parity >= 1);
	}
	/* Mean slice 1000 B (+ 2 B NAL header): 3/2 of it, above the floor. */
	CHECK("mk fec on: payload follows the slice average",
		output.payload_used == 1503);
	CHECK("mk fec on: parity counted",
		output.fec.parity_pkts >= 3 && output.fec.unprotected == 0);

	par0 = output.fec.parity_pkts;
	CHECK("mk fec: disable", maruko_output_set_fec(&output, 0) == 0);
	mk_fec_frame(&pack, &stream, buf, f++);
	(void)maruko_video_send_frame(&stream, &output, &rtp, &params, &cfg, NULL);
	mk_fec_recv(rx, &r);
	CHECK("mk fec off again: video only",
		r.n == MK_FEC_SLICES && mk_fec_check(&r, &sources) == 0 &&
		output.fec.parity_pkts == par0 && output.payload_used == 3780);

	maruko_output_teardown(&output);
	CHECK("mk fec: teardown frees the arena", !output.fec.arena);
	close(rx);
	return failures;
}

int test_maruko_video(void)
{
	MarukoOutput output;
	i6c_venc_pack pack;
	i6c_venc_strm stream;
	uint8_t data[16] = { 0 };
	int failures = 0;

	failures += test_maruko_flatten_matches_the_star6e_contract();
	failures += test_maruko_flatten_refuses_an_access_unit_over_the_queue_cap();
	failures += test_maruko_video_fec();
	unsigned int info_cap;

	memset(&output, 0, sizeof(output));
	memset(&pack, 0, sizeof(pack));
	memset(&stream, 0, sizeof(stream));
	info_cap = (unsigned int)(sizeof(pack.packetInfo) /
		sizeof(pack.packetInfo[0]));

	CHECK("maruko packetInfo null stream rejected",
		!maruko_video_stream_packet_info_complete(NULL));
	CHECK("maruko packetInfo empty stream rejected",
		!maruko_video_stream_packet_info_complete(&stream));

	stream.count = 1;
	stream.packet = &pack;
	CHECK("maruko packetInfo null data rejected",
		!maruko_video_stream_packet_info_complete(&stream));

	pack.data = data;
	pack.length = sizeof(data);
	pack.offset = 0;
	CHECK("maruko packetInfo fallback accepted",
		maruko_video_stream_packet_info_complete(&stream));
	pack.offset = pack.length;
	CHECK("maruko packetInfo empty fallback rejected",
		!maruko_video_stream_packet_info_complete(&stream));

	pack.offset = 0;
	pack.packNum = 1;
	pack.packetInfo[0].offset = 0;
	pack.packetInfo[0].length = 0;
	CHECK("maruko packetInfo zero descriptor rejected",
		!maruko_video_stream_packet_info_complete(&stream));
	pack.packetInfo[0].offset = pack.length;
	pack.packetInfo[0].length = 1;
	CHECK("maruko packetInfo offset rejected",
		!maruko_video_stream_packet_info_complete(&stream));
	pack.packetInfo[0].offset = 8;
	pack.packetInfo[0].length = 9;
	CHECK("maruko packetInfo overrun rejected",
		!maruko_video_stream_packet_info_complete(&stream));
	pack.packetInfo[0].offset = 4;
	pack.packetInfo[0].length = 8;
	CHECK("maruko packetInfo descriptor accepted",
		maruko_video_stream_packet_info_complete(&stream));
	pack.packNum = info_cap + 1;
	CHECK("maruko packetInfo table overflow rejected",
		!maruko_video_stream_packet_info_complete(&stream));

	/* An invalid AU is rejected every time and actuates nothing -- the
	 * decision to ask for a keyframe belongs to the consumer, which is
	 * the only party that can see whether the decoder is broken. */
	output.bad_au_drops = 0;
	CHECK("maruko invalid AU rejected",
		maruko_video_reject_incomplete_access_unit(&stream, &output) == 1);
	CHECK("maruko invalid AU counted", output.bad_au_drops == 1);
	CHECK("maruko invalid AU rejected again",
		maruko_video_reject_incomplete_access_unit(&stream, &output) == 1);
	/* The one-shot WARN must not become a one-shot COUNTER -- the whole
	 * point is that a repeating fault stays visible after the first line. */
	CHECK("maruko invalid AU counts every time", output.bad_au_drops == 2);
	output.svct_active = 1;
	stream.h265Info.refType = MARUKO_REFTYPE_ENHANCE_P_NOTFORREF;
	CHECK("maruko droppable invalid AU still rejected",
		maruko_video_reject_incomplete_access_unit(&stream, &output) == 1);

	return failures;
}
