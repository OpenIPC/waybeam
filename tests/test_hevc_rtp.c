#include "hevc_rtp.h"
#include "h26x_util.h"

#include "h26x_param_sets.h"
#include "rtp_packetizer.h"
#include "test_helpers.h"

#include <stdint.h>
#include <string.h>

#define CAPTURE_MAX 32

typedef struct {
	size_t header_len;
	size_t payload_len;
	uint8_t payload[RTP_BUFFER_MAX];
	uint8_t header[16];
} CapturedPacket;

typedef struct {
	CapturedPacket packets[CAPTURE_MAX];
	size_t count;
	int fail_call; /* 1-based write to refuse, 0 = none */
	int calls;
} CaptureCtx;

static int capture_write(const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	CaptureCtx *ctx = opaque;
	CapturedPacket *pkt;
	size_t combined = payload1_len + payload2_len;

	if (!ctx || ctx->count >= CAPTURE_MAX || header_len > sizeof(pkt->header)
	    || combined > RTP_BUFFER_MAX)
		return -1;
	if (ctx->fail_call && ++ctx->calls == ctx->fail_call)
		return -1;

	pkt = &ctx->packets[ctx->count];
	memcpy(pkt->header, header, header_len);
	pkt->header_len = header_len;
	memcpy(pkt->payload, payload1, payload1_len);
	if (payload2 && payload2_len > 0)
		memcpy(pkt->payload + payload1_len, payload2, payload2_len);
	pkt->payload_len = combined;
	ctx->count++;
	return 0;
}

static int rtp_marker_set(const uint8_t *header)
{
	return (header[1] & 0x80) ? 1 : 0;
}

static uint8_t pkt_nal_type(const CapturedPacket *pkt)
{
	return (uint8_t)((pkt->payload[0] >> 1) & 0x3F);
}

/* The whole point of the hard drop: no captured packet may be an HEVC
 * Aggregation Packet (NAL type 48). */
static int no_aggregation_packets(const CaptureCtx *ctx)
{
	size_t i;

	for (i = 0; i < ctx->count; ++i) {
		if (pkt_nal_type(&ctx->packets[i]) == 48)
			return 0;
	}
	return 1;
}

/* Three tiny NALs must go out as three separate single-NAL packets, never
 * aggregated into one type-48 packet. */
static int test_hevc_rtp_no_aggregation_small_nals(void)
{
	static const uint8_t vps[] = { 0x40, 0x01, 0xAA };
	static const uint8_t sps[] = { 0x42, 0x01, 0xBB };
	static const uint8_t pps[] = { 0x44, 0x01, 0xCC };
	CaptureCtx capture = {0};
	HevcRtpStats stats = {0};
	RtpPacketizerState rtp = { .seq = 0x100, .timestamp = 0x1000,
		.ssrc = 0xDEAD, .payload_type = 97 };
	size_t total;
	int failures = 0;

	total = hevc_rtp_send_nal(vps, sizeof(vps), &rtp, capture_write,
		&capture, 0, 1400, &stats);
	total += hevc_rtp_send_nal(sps, sizeof(sps), &rtp, capture_write,
		&capture, 0, 1400, &stats);
	total += hevc_rtp_send_nal(pps, sizeof(pps), &rtp, capture_write,
		&capture, 1, 1400, &stats);

	CHECK("hevc_rtp no-agg three packets", capture.count == 3);
	CHECK("hevc_rtp no-agg no type-48", no_aggregation_packets(&capture));
	CHECK("hevc_rtp no-agg total bytes match",
		total == sizeof(vps) + sizeof(sps) + sizeof(pps));
	CHECK("hevc_rtp no-agg marker only on last",
		capture.count == 3 &&
		!rtp_marker_set(capture.packets[0].header) &&
		!rtp_marker_set(capture.packets[1].header) &&
		rtp_marker_set(capture.packets[2].header));
	CHECK("hevc_rtp no-agg stats", stats.total_nals == 3 &&
		stats.single_packets == 3 && stats.fu_packets == 0 &&
		stats.rtp_packets == 3);
	CHECK("hevc_rtp no-agg seq advanced by 3", rtp.seq == 0x103);
	return failures;
}

static int test_hevc_rtp_fallback_to_fu(void)
{
	/* Large NAL that exceeds max_payload — must use FU-A. */
	static uint8_t big[3000];
	CaptureCtx capture = {0};
	HevcRtpStats stats = {0};
	RtpPacketizerState rtp = { .seq = 0x200, .timestamp = 0x2000,
		.ssrc = 0xCAFE, .payload_type = 97 };
	size_t i;
	size_t total;
	int failures = 0;

	/* Build a plausible NAL header: type=1 (TRAIL_N), layer_id=0, tid=1 */
	big[0] = (uint8_t)(1 << 1);
	big[1] = 0x01;
	for (i = 2; i < sizeof(big); ++i)
		big[i] = (uint8_t)(i & 0xFF);

	total = hevc_rtp_send_nal(big, sizeof(big), &rtp, capture_write,
		&capture, 1, 1400, &stats);

	CHECK("hevc_rtp fu total bytes", total == sizeof(big));
	CHECK("hevc_rtp fu multi packet", capture.count >= 2);
	CHECK("hevc_rtp fu no type-48", no_aggregation_packets(&capture));
	CHECK("hevc_rtp fu stats", stats.fu_packets == capture.count
		&& stats.total_nals == 1 && stats.single_packets == 0);
	CHECK("hevc_rtp fu last marker",
		capture.count > 0 &&
		rtp_marker_set(capture.packets[capture.count - 1].header));
	CHECK("hevc_rtp fu indicator type 49",
		capture.count > 0 && pkt_nal_type(&capture.packets[0]) == 49);
	return failures;
}

/* On an IDR, VPS/SPS/PPS are prepended as separate single-NAL packets
 * (sent immediately), then the IDR itself — no aggregation anywhere. */
static int test_hevc_rtp_prepend_separate_param_sets_on_idr(void)
{
	static const uint8_t vps[] = { 0x40, 0x01, 0xAA };
	static const uint8_t sps[] = { 0x42, 0x01, 0xBB };
	static const uint8_t pps[] = { 0x44, 0x01, 0xCC };
	static const uint8_t idr[] = { 0x26, 0x01, 0xDD, 0xEE };
	H26xParamSets params = {0};
	CaptureCtx capture = {0};
	HevcRtpStats stats = {0};
	RtpPacketizerState rtp = { .seq = 0x300, .timestamp = 0x3000,
		.ssrc = 0xFEED, .payload_type = 97 };
	int failures = 0;
	size_t prepend_total;
	size_t idr_total;

	h26x_param_sets_update(&params, PT_H265, 32, vps, sizeof(vps));
	h26x_param_sets_update(&params, PT_H265, 33, sps, sizeof(sps));
	h26x_param_sets_update(&params, PT_H265, 34, pps, sizeof(pps));

	/* Prepend sends each param set as its own packet immediately. */
	prepend_total = hevc_rtp_prepend_param_sets(&params, 19, &rtp,
		capture_write, &capture, 1400, &stats);
	CHECK("hevc_rtp prepend three separate packets",
		capture.count == 3 && stats.total_nals == 3);
	CHECK("hevc_rtp prepend bytes",
		prepend_total == sizeof(vps) + sizeof(sps) + sizeof(pps));
	CHECK("hevc_rtp prepend no marker before idr",
		!rtp_marker_set(capture.packets[0].header) &&
		!rtp_marker_set(capture.packets[1].header) &&
		!rtp_marker_set(capture.packets[2].header));

	/* IDR is small enough for a single-NAL packet and carries the marker. */
	idr_total = hevc_rtp_send_nal(idr, sizeof(idr), &rtp, capture_write,
		&capture, 1, 1400, &stats);
	CHECK("hevc_rtp prepend+idr four packets total", capture.count == 4);
	CHECK("hevc_rtp prepend+idr no type-48", no_aggregation_packets(&capture));
	CHECK("hevc_rtp prepend+idr bytes", idr_total == sizeof(idr));
	CHECK("hevc_rtp prepend+idr marker on idr",
		rtp_marker_set(capture.packets[3].header));
	CHECK("hevc_rtp prepend+idr stats",
		stats.total_nals == 4 && stats.single_packets == 4 &&
		stats.fu_packets == 0 && stats.rtp_packets == 4);

	/* Non-IDR nal type should not prepend anything. */
	memset(&stats, 0, sizeof(stats));
	memset(&capture, 0, sizeof(capture));
	prepend_total = hevc_rtp_prepend_param_sets(&params, 1, &rtp,
		capture_write, &capture, 1400, &stats);
	CHECK("hevc_rtp prepend skipped for non-idr",
		prepend_total == 0 && stats.total_nals == 0 && capture.count == 0);
	return failures;
}

/* A small NAL followed by a large NAL in the same frame: the small NAL is a
 * single-RTP packet, the large NAL goes out as FU-A.  No aggregation; the
 * FU-A tail carries the marker bit. */
static int test_hevc_rtp_mixed_small_then_large(void)
{
	static const uint8_t small[] = { 0x02, 0x01, 0x33, 0x44 };
	static uint8_t big[3000];
	CaptureCtx capture = {0};
	HevcRtpStats stats = {0};
	RtpPacketizerState rtp = { .seq = 0x400, .timestamp = 0x4000,
		.ssrc = 0xBEEF, .payload_type = 97 };
	size_t i;
	size_t total;
	int failures = 0;

	big[0] = (uint8_t)(1 << 1);
	big[1] = 0x01;
	for (i = 2; i < sizeof(big); ++i)
		big[i] = (uint8_t)(i & 0xFF);

	total = hevc_rtp_send_nal(small, sizeof(small), &rtp, capture_write,
		&capture, 0, 1400, &stats);
	total += hevc_rtp_send_nal(big, sizeof(big), &rtp, capture_write,
		&capture, 1, 1400, &stats);

	CHECK("hevc_rtp mixed total bytes", total == sizeof(small) + sizeof(big));
	CHECK("hevc_rtp mixed multiple packets", capture.count >= 3);
	CHECK("hevc_rtp mixed no type-48", no_aggregation_packets(&capture));
	CHECK("hevc_rtp mixed first is single",
		capture.count > 0 && pkt_nal_type(&capture.packets[0]) != 49);
	CHECK("hevc_rtp mixed last marker",
		capture.count > 0 &&
		rtp_marker_set(capture.packets[capture.count - 1].header));
	CHECK("hevc_rtp mixed stats split",
		stats.total_nals == 2 && stats.single_packets == 1 &&
		stats.fu_packets >= 2);
	return failures;
}

/* TRAIL_N (type 0) NALs are the refPred droppability marker — the rewritten
 * header byte is 0x00, so verify the leading-zero NAL survives both wire
 * shapes: verbatim in a single-NAL packet, and as type 0 in the FU header. */
static int test_hevc_rtp_trail_n_type_preserved(void)
{
	static const uint8_t small_trail_n[] = { 0x00, 0x01, 0x55, 0x66 };
	static uint8_t big_trail_n[3000];
	CaptureCtx capture = {0};
	HevcRtpStats stats = {0};
	RtpPacketizerState rtp = { .seq = 0x700, .timestamp = 0x7000,
		.ssrc = 0x4321, .payload_type = 97 };
	size_t i;
	size_t total;
	int failures = 0;

	big_trail_n[0] = 0x00;	/* TRAIL_N: type=0, layer_msb=0 */
	big_trail_n[1] = 0x01;	/* layer_lsb=0, tid_plus1=1 */
	for (i = 2; i < sizeof(big_trail_n); ++i)
		big_trail_n[i] = (uint8_t)(i & 0xFF);

	total = hevc_rtp_send_nal(small_trail_n, sizeof(small_trail_n), &rtp,
		capture_write, &capture, 0, 1400, &stats);
	CHECK("hevc_rtp trail_n single verbatim",
		capture.count == 1 && total == sizeof(small_trail_n) &&
		capture.packets[0].payload[0] == 0x00 &&
		capture.packets[0].payload[1] == 0x01);

	total = hevc_rtp_send_nal(big_trail_n, sizeof(big_trail_n), &rtp,
		capture_write, &capture, 1, 1400, &stats);
	CHECK("hevc_rtp trail_n fu total", total == sizeof(big_trail_n));
	CHECK("hevc_rtp trail_n fu indicator type 49",
		capture.count >= 3 && pkt_nal_type(&capture.packets[1]) == 49);
	CHECK("hevc_rtp trail_n fu carries type 0",
		capture.count >= 3 &&
		(capture.packets[1].payload[2] & 0x3F) == 0 &&
		(capture.packets[capture.count - 1].payload[2] & 0x3F) == 0);
	CHECK("hevc_rtp trail_n no type-48", no_aggregation_packets(&capture));
	return failures;
}

/* Passing stats=NULL must be safe at every entry point. */
static int test_hevc_rtp_null_stats(void)
{
	static const uint8_t vps[] = { 0x40, 0x01, 0xAA };
	static const uint8_t sps[] = { 0x42, 0x01, 0xBB };
	CaptureCtx capture = {0};
	RtpPacketizerState rtp = { .seq = 0x500, .timestamp = 0x5000,
		.ssrc = 0xABCD, .payload_type = 97 };
	H26xParamSets params = {0};
	size_t total;
	int failures = 0;

	h26x_param_sets_update(&params, PT_H265, 32, vps, sizeof(vps));
	h26x_param_sets_update(&params, PT_H265, 33, sps, sizeof(sps));

	total = hevc_rtp_send_nal(vps, sizeof(vps), &rtp, capture_write,
		&capture, 0, 1400, NULL);
	total += hevc_rtp_send_nal(sps, sizeof(sps), &rtp, capture_write,
		&capture, 1, 1400, NULL);
	CHECK("hevc_rtp null-stats send ok", total == sizeof(vps) + sizeof(sps));

	memset(&capture, 0, sizeof(capture));
	total = hevc_rtp_prepend_param_sets(&params, 19, &rtp,
		capture_write, &capture, 1400, NULL);
	CHECK("hevc_rtp null-stats prepend ok",
		total == sizeof(vps) + sizeof(sps) && capture.count == 2);
	return failures;
}

/* hevc_rtp_send_nal with is_last=1 on a lone small NAL must emit a single
 * RTP packet with the marker bit set. */
static int test_hevc_rtp_single_marker_on_last(void)
{
	static const uint8_t nal[] = { 0x02, 0x01, 0x77, 0x88, 0x99 };
	CaptureCtx capture = {0};
	HevcRtpStats stats = {0};
	RtpPacketizerState rtp = { .seq = 0x600, .timestamp = 0x6000,
		.ssrc = 0x1234, .payload_type = 97 };
	size_t total;
	int failures = 0;

	total = hevc_rtp_send_nal(nal, sizeof(nal), &rtp, capture_write,
		&capture, 1, 1400, &stats);
	CHECK("hevc_rtp single total", total == sizeof(nal));
	CHECK("hevc_rtp single one packet", capture.count == 1);
	CHECK("hevc_rtp single marker set",
		capture.count > 0 && rtp_marker_set(capture.packets[0].header));
	CHECK("hevc_rtp single stats",
		stats.total_nals == 1 && stats.single_packets == 1 &&
		stats.fu_packets == 0);
	return failures;
}

/* Append start code + a NAL of `len` bytes (type, first_slice flag) to buf. */
static size_t annexb_put(uint8_t *buf, size_t at, int long_code, uint8_t type,
	int first_slice, size_t len)
{
	static const uint8_t code[] = { 0, 0, 0, 1 };

	memcpy(buf + at, long_code ? code : code + 1, long_code ? 4 : 3);
	at += long_code ? 4 : 3;
	buf[at] = (uint8_t)(type << 1);
	buf[at + 1] = 0x01;
	memset(buf + at + 2, 0x5A, len - 2);
	if (type < 32)
		buf[at + 2] = first_slice ? 0x80 : 0x00;
	return at + len;
}

/* hevc_rtp_send_annexb: one NAL per slice; VPS/SPS/PPS once per IRAP
 * picture; marker on the end. */
static int test_hevc_rtp_send_annexb(void)
{
	static uint8_t buf[4096];
	static const uint8_t vps[] = { 0x40, 0x01, 0xAA };
	static const uint8_t sps[] = { 0x42, 0x01, 0xBB };
	static const uint8_t pps[] = { 0x44, 0x01, 0xCC };
	static const uint8_t raw[] = { 0x02, 0x01, 0x80, 0x11, 0x22 };
	H26xParamSets params = {0};
	static CaptureCtx cap;
	RtpPacketizerState rtp = { .seq = 0x500, .timestamp = 0x5000,
		.ssrc = 0xFEED, .payload_type = 97 };
	int failures = 0;
	size_t n;

	/* Two 1000 B slices, two NALs; the 4-byte start code's leading zero
	 * does not stick to the first slice. */
	n = annexb_put(buf, 0, 1, 1, 1, 1000);
	n = annexb_put(buf, n, 1, 1, 0, 1000);
	memset(&cap, 0, sizeof(cap));
	hevc_rtp_send_annexb(buf, n, NULL, &rtp, capture_write, &cap, 1, 1400,
		NULL);
	CHECK("annexb: two slices, two NALs",
		cap.count == 2 && cap.packets[0].payload_len == 1000 &&
		cap.packets[1].payload_len == 1000 &&
		memcmp(cap.packets[1].payload, buf + 1008, 1000) == 0);
	CHECK("annexb: marker on the last packet only",
		!rtp_marker_set(cap.packets[0].header) &&
		rtp_marker_set(cap.packets[1].header));

	/* 1500 + 100: never joined -- an FU pair, then the small slice alone. */
	n = annexb_put(buf, 0, 0, 1, 1, 1500);
	n = annexb_put(buf, n, 0, 1, 0, 100);
	memset(&cap, 0, sizeof(cap));
	hevc_rtp_send_annexb(buf, n, NULL, &rtp, capture_write, &cap, 1, 1400,
		NULL);
	CHECK("annexb: every slice its own NAL, even when joining saves a packet",
		cap.count == 3 && pkt_nal_type(&cap.packets[0]) == 49 &&
		pkt_nal_type(&cap.packets[1]) == 49 &&
		cap.packets[2].payload_len == 100 &&
		rtp_marker_set(cap.packets[2].header));

	/* Parameter sets each go out as their own NAL. */
	n = annexb_put(buf, 0, 1, 32, 0, 24);
	n = annexb_put(buf, n, 1, 33, 0, 40);
	n = annexb_put(buf, n, 1, 34, 0, 8);
	n = annexb_put(buf, n, 1, 19, 1, 60);
	memset(&cap, 0, sizeof(cap));
	hevc_rtp_send_annexb(buf, n, &params, &rtp, capture_write, &cap, 1,
		1400, NULL);
	CHECK("annexb: VPS SPS PPS learned, kept separate, not sent twice",
		params.vps_len == 24 && params.pps_len == 8 && cap.count == 4 &&
		pkt_nal_type(&cap.packets[0]) == 32 &&
		pkt_nal_type(&cap.packets[1]) == 33 &&
		pkt_nal_type(&cap.packets[2]) == 34);

	/* An IRAP picture in two slices: the prepend goes before the first
	 * slice only, not once per slice. */
	h26x_param_sets_update(&params, PT_H265, 32, vps, sizeof(vps));
	h26x_param_sets_update(&params, PT_H265, 33, sps, sizeof(sps));
	h26x_param_sets_update(&params, PT_H265, 34, pps, sizeof(pps));
	n = annexb_put(buf, 0, 1, 19, 1, 1000);
	n = annexb_put(buf, n, 1, 19, 0, 1000);
	memset(&cap, 0, sizeof(cap));
	hevc_rtp_send_annexb(buf, n, &params, &rtp, capture_write, &cap, 1,
		1400, NULL);
	CHECK("annexb: one VPS/SPS/PPS prepend per IRAP picture",
		cap.count == 5 && pkt_nal_type(&cap.packets[0]) == 32 &&
		pkt_nal_type(&cap.packets[3]) == 19 &&
		pkt_nal_type(&cap.packets[4]) == 19);

	/* The encoder's own sets in a pack of their own, then the IDR: the
	 * in-band copy still counts, so nothing is prepended. */
	n = annexb_put(buf, 0, 1, 32, 0, 24);
	n = annexb_put(buf, n, 1, 33, 0, 40);
	n = annexb_put(buf, n, 1, 34, 0, 8);
	memset(&cap, 0, sizeof(cap));
	hevc_rtp_send_annexb(buf, n, &params, &rtp, capture_write, &cap, 0,
		1400, NULL);
	n = annexb_put(buf, 0, 1, 19, 1, 60);
	hevc_rtp_send_annexb(buf, n, &params, &rtp, capture_write, &cap, 1,
		1400, NULL);
	CHECK("annexb: in-band sets in an earlier pack suppress the prepend",
		cap.count == 4 && pkt_nal_type(&cap.packets[3]) == 19);

	/* A refused in-band PPS does not count: the IDR still gets ours. */
	n = annexb_put(buf, 0, 1, 32, 0, 24);
	n = annexb_put(buf, n, 1, 33, 0, 40);
	n = annexb_put(buf, n, 1, 34, 0, 8);
	memset(&cap, 0, sizeof(cap));
	cap.fail_call = 3; /* the PPS */
	hevc_rtp_send_annexb(buf, n, &params, &rtp, capture_write, &cap, 0,
		1400, NULL);
	n = annexb_put(buf, 0, 1, 19, 1, 60);
	memset(&cap, 0, sizeof(cap));
	hevc_rtp_send_annexb(buf, n, &params, &rtp, capture_write, &cap, 1,
		1400, NULL);
	CHECK("annexb: a refused in-band set still gets the prepend",
		cap.count == 4 && pkt_nal_type(&cap.packets[0]) == 32);

	/* FEC payload sizing counts non-IRAP slices only: an IDR's large
	 * slices must not inflate the average. */
	{
		HevcRtpStats st = {0};

		n = annexb_put(buf, 0, 1, 19, 1, 900);
		n = annexb_put(buf, n, 1, 1, 0, 300);
		memset(&cap, 0, sizeof(cap));
		hevc_rtp_send_annexb(buf, n, NULL, &rtp, capture_write, &cap, 1,
			1400, &st);
		CHECK("annexb: slice stats skip IRAP",
			st.slices == 1 && st.slice_bytes == 300);
	}

	/* No start code: the region is one raw NAL. */
	memset(&cap, 0, sizeof(cap));
	hevc_rtp_send_annexb(raw, sizeof(raw), NULL, &rtp, capture_write, &cap,
		1, 1400, NULL);
	CHECK("annexb: raw NAL sent whole",
		cap.count == 1 && cap.packets[0].payload_len == sizeof(raw) &&
		memcmp(cap.packets[0].payload, raw, sizeof(raw)) == 0);
	return failures;
}

/* A sender that walks NALs itself (CV610): before/sent around each NAL. */
static size_t walk_with_param_helpers(const uint8_t *buf, size_t len,
	H26xParamSets *params, RtpPacketizerState *rtp, CaptureCtx *cap)
{
	const uint8_t *nal;
	size_t nal_len, cursor = 0, total = 0;

	while (h26x_util_annexb_next(buf, len, &cursor, &nal, &nal_len)) {
		const uint8_t type = h26x_util_hevc_nalu_type(nal, nal_len);

		total += hevc_rtp_param_sets_before(params, type, nal, nal_len,
			rtp, capture_write, cap, 1400, NULL);
		if (hevc_rtp_send_nal(nal, nal_len, rtp, capture_write, cap,
		    cursor == len, 1400, NULL) == nal_len)
			hevc_rtp_param_sets_sent(params, type);
	}
	return total;
}

static int test_hevc_rtp_param_set_helpers(void)
{
	static uint8_t buf[4096];
	static CaptureCtx cap;
	H26xParamSets params = {0};
	RtpPacketizerState rtp = { .seq = 0x600, .timestamp = 0x6000,
		.ssrc = 0xBEEF, .payload_type = 97 };
	int failures = 0;
	size_t n, i;

	/* An IDR picture of 3 slices after the encoder's own VPS/SPS/PPS: the
	 * old CV610 loop added 3 sets before EVERY slice (9 extra). */
	n = annexb_put(buf, 0, 1, 32, 0, 24);
	n = annexb_put(buf, n, 1, 33, 0, 40);
	n = annexb_put(buf, n, 1, 34, 0, 8);
	for (i = 0; i < 3; i++)
		n = annexb_put(buf, n, 1, 19, i == 0, 200);
	memset(&cap, 0, sizeof(cap));
	CHECK("param helpers: in-band sets, nothing prepended",
		walk_with_param_helpers(buf, n, &params, &rtp, &cap) == 0 &&
		cap.count == 6 && pkt_nal_type(&cap.packets[0]) == 32 &&
		pkt_nal_type(&cap.packets[3]) == 19);

	/* The next IDR without in-band sets: exactly one prepend, before
	 * slice 0 only. */
	n = 0;
	for (i = 0; i < 3; i++)
		n = annexb_put(buf, n, 1, 19, i == 0, 200);
	memset(&cap, 0, sizeof(cap));
	(void)walk_with_param_helpers(buf, n, &params, &rtp, &cap);
	CHECK("param helpers: one prepend, before the first slice only",
		cap.count == 6 && pkt_nal_type(&cap.packets[0]) == 32 &&
		pkt_nal_type(&cap.packets[2]) == 34 &&
		pkt_nal_type(&cap.packets[3]) == 19 &&
		pkt_nal_type(&cap.packets[4]) == 19 &&
		pkt_nal_type(&cap.packets[5]) == 19);

	/* All three in-band sets, the SPS refused: the set is incomplete, so
	 * the next IDR without in-band sets still gets ours. */
	n = annexb_put(buf, 0, 1, 32, 0, 24);
	n = annexb_put(buf, n, 1, 33, 0, 40);
	n = annexb_put(buf, n, 1, 34, 0, 8);
	memset(&cap, 0, sizeof(cap));
	cap.fail_call = 2;
	(void)walk_with_param_helpers(buf, n, &params, &rtp, &cap);
	n = annexb_put(buf, 0, 1, 19, 1, 200);
	memset(&cap, 0, sizeof(cap));
	(void)walk_with_param_helpers(buf, n, &params, &rtp, &cap);
	CHECK("param helpers: a failed in-band SPS still gets the prepend",
		cap.count == 4 && pkt_nal_type(&cap.packets[0]) == 32);
	return failures;
}

int test_hevc_rtp(void)
{
	int failures = 0;

	failures += test_hevc_rtp_no_aggregation_small_nals();
	failures += test_hevc_rtp_fallback_to_fu();
	failures += test_hevc_rtp_prepend_separate_param_sets_on_idr();
	failures += test_hevc_rtp_mixed_small_then_large();
	failures += test_hevc_rtp_trail_n_type_preserved();
	failures += test_hevc_rtp_null_stats();
	failures += test_hevc_rtp_single_marker_on_last();
	failures += test_hevc_rtp_send_annexb();
	failures += test_hevc_rtp_param_set_helpers();
	return failures;
}
