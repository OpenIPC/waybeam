#include "hevc_rtp.h"

#include "h26x_util.h"

static void apply_stats(HevcRtpStats *stats, const RtpPacketizerResult *result)
{
	if (!stats || !result || result->packet_count == 0)
		return;

	if (result->fragmented)
		stats->fu_packets += result->packet_count;
	else
		stats->single_packets += result->packet_count;
	stats->rtp_packets += result->packet_count;
	stats->rtp_payload_bytes += result->payload_bytes;
}

size_t hevc_rtp_send_nal(const uint8_t *data, size_t len,
	RtpPacketizerState *rtp, RtpPacketizerWriteFn write_cb, void *opaque,
	int is_last, size_t max_payload, HevcRtpStats *stats)
{
	RtpPacketizerResult result;
	size_t total_bytes;

	if (!data || len == 0 || !rtp || !write_cb)
		return 0;

	if (stats)
		stats->total_nals++;

	total_bytes = rtp_packetizer_send_hevc_nal(rtp, write_cb, opaque, data,
		len, is_last, max_payload, stats ? &result : NULL);
	if (stats)
		apply_stats(stats, &result);
	return total_bytes;
}

size_t hevc_rtp_prepend_param_sets(const H26xParamSets *params, uint8_t nal_type,
	RtpPacketizerState *rtp, RtpPacketizerWriteFn write_cb, void *opaque,
	size_t max_payload, HevcRtpStats *stats)
{
	H26xParamSetRef refs[3];
	size_t count;
	size_t total_bytes = 0;
	size_t i;

	if (!params || !rtp)
		return 0;

	count = h26x_param_sets_get_prepend(params, PT_H265, nal_type, refs,
		sizeof(refs) / sizeof(refs[0]));
	for (i = 0; i < count; ++i) {
		total_bytes += hevc_rtp_send_nal(refs[i].data, refs[i].len,
			rtp, write_cb, opaque, 0, max_payload, stats);
	}

	return total_bytes;
}

/* Packets rtp_packetizer_send_hevc_nal() spends on len bytes: one single-NAL
 * packet, or FU fragments carrying max_payload - 3 bytes of the body each. */
static size_t hevc_rtp_packets(size_t len, size_t max_payload)
{
	return len <= max_payload ? 1 :
		(len - 2 + max_payload - 4) / (max_payload - 3);
}

static size_t hevc_rtp_send_group(const uint8_t *nal, size_t len,
	const H26xParamSets *params, RtpPacketizerState *rtp,
	RtpPacketizerWriteFn write_cb, void *opaque, int is_last,
	size_t max_payload, HevcRtpStats *stats)
{
	size_t total_bytes = 0;

	/* VPS/SPS/PPS once per IRAP picture: before its first slice only. */
	if (params && len > 2 && (nal[2] & 0x80))
		total_bytes += hevc_rtp_prepend_param_sets(params,
			h26x_util_hevc_nalu_type(nal, len), rtp, write_cb, opaque,
			max_payload, stats);
	return total_bytes + hevc_rtp_send_nal(nal, len, rtp, write_cb, opaque,
		is_last, max_payload, stats);
}

size_t hevc_rtp_send_annexb(const uint8_t *data, size_t len,
	H26xParamSets *params, RtpPacketizerState *rtp,
	RtpPacketizerWriteFn write_cb, void *opaque, int is_last,
	size_t max_payload, HevcRtpStats *stats)
{
	const uint8_t *nal, *group = NULL;
	size_t nal_len, group_len = 0, cursor = 0, total_bytes = 0;
	int group_vcl = 0;
	const int annexb = h26x_util_annexb_next(data, len, &cursor, &nal,
		&nal_len);

	if (max_payload <= 3) /* the packetizer refuses it too */
		return 0;
	if (!annexb) {
		/* No start code: one raw NAL (a raw NAL cannot contain one). */
		nal = data;
		nal_len = len;
		h26x_util_strip_start_code(&nal, &nal_len);
		if (!nal || nal_len == 0)
			return 0;
	}
	do {
		const uint8_t type = h26x_util_hevc_nalu_type(nal, nal_len);

		if (params)
			h26x_param_sets_update(params, PT_H265, type, nal, nal_len);
		/* Every datagram costs a fixed slice of airtime, so a slice joins
		 * the previous one -- one RTP NAL with the start code inside, as a
		 * whole pack went out before -- only when that saves a packet.
		 * Otherwise each slice is its own NAL and a lost packet costs one
		 * slice, not the picture. */
		if (group && group_vcl && type < 32) {
			const size_t joined = (size_t)(nal + nal_len - group);

			if (hevc_rtp_packets(joined, max_payload) <
			    hevc_rtp_packets(group_len, max_payload) +
			    hevc_rtp_packets(nal_len, max_payload)) {
				group_len = joined;
				continue;
			}
		}
		if (group)
			total_bytes += hevc_rtp_send_group(group, group_len, params,
				rtp, write_cb, opaque, 0, max_payload, stats);
		group = nal;
		group_len = nal_len;
		group_vcl = type < 32;
	} while (annexb &&
		 h26x_util_annexb_next(data, len, &cursor, &nal, &nal_len));
	return total_bytes + hevc_rtp_send_group(group, group_len, params, rtp,
		write_cb, opaque, is_last, max_payload, stats);
}
