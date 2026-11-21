#include "rtp_packetizer.h"

#include <string.h>

#define RTP_HEADER_SIZE 12

static void rtp_packetizer_reset_result(RtpPacketizerResult *result)
{
	if (!result)
		return;
	memset(result, 0, sizeof(*result));
}

int rtp_packetizer_send_packet(RtpPacketizerState *state,
	RtpPacketizerWriteFn writer, void *opaque, const uint8_t *payload1,
	size_t payload1_len, const uint8_t *payload2, size_t payload2_len,
	int marker)
{
	uint8_t header[RTP_HEADER_SIZE];

	if (!state || !writer || !payload1 || payload1_len == 0)
		return -1;

	header[0] = 0x80;
	header[1] = (uint8_t)((marker ? 0x80 : 0x00) |
		(state->payload_type & 0x7F));
	header[2] = (uint8_t)((state->seq >> 8) & 0xFF);
	header[3] = (uint8_t)(state->seq & 0xFF);
	header[4] = (uint8_t)((state->timestamp >> 24) & 0xFF);
	header[5] = (uint8_t)((state->timestamp >> 16) & 0xFF);
	header[6] = (uint8_t)((state->timestamp >> 8) & 0xFF);
	header[7] = (uint8_t)(state->timestamp & 0xFF);
	header[8] = (uint8_t)((state->ssrc >> 24) & 0xFF);
	header[9] = (uint8_t)((state->ssrc >> 16) & 0xFF);
	header[10] = (uint8_t)((state->ssrc >> 8) & 0xFF);
	header[11] = (uint8_t)(state->ssrc & 0xFF);

	if (writer(header, sizeof(header), payload1, payload1_len,
		payload2, payload2_len, opaque) != 0)
		return -1;

	state->seq++;
	return 0;
}

size_t rtp_packetizer_send_hevc_fu(RtpPacketizerState *state,
	RtpPacketizerWriteFn writer, void *opaque, const uint8_t *data,
	size_t length, int is_last, size_t max_payload,
	RtpPacketizerResult *result)
{
	const uint8_t *payload;
	size_t remaining;
	size_t total_bytes = 0;
	size_t max_fragment;
	size_t pieces;
	int start = 1;
	uint8_t fu_hdr[3];
	uint8_t nal_header0;
	uint8_t nal_header1;
	uint8_t forbidden_zero;
	uint8_t nuh_layer_id;
	uint8_t temporal_id_plus1;
	uint8_t nal_type;
	uint8_t fu_indicator0;
	uint8_t fu_indicator1;

	rtp_packetizer_reset_result(result);

	if (!data || length <= 2 || !state || !writer || max_payload <= 3)
		return 0;

	if (max_payload > RTP_BUFFER_MAX)
		max_payload = RTP_BUFFER_MAX;
	max_fragment = max_payload - 3;

	nal_header0 = data[0];
	nal_header1 = data[1];
	forbidden_zero = (uint8_t)(nal_header0 & 0x80);
	nuh_layer_id = (uint8_t)(((nal_header0 & 0x01) << 5) |
		(nal_header1 >> 3));
	temporal_id_plus1 = (uint8_t)(nal_header1 & 0x07);
	nal_type = (uint8_t)((nal_header0 >> 1) & 0x3F);
	fu_indicator0 = (uint8_t)(forbidden_zero | (49 << 1) |
		((nuh_layer_id >> 5) & 0x01));
	fu_indicator1 = (uint8_t)(((nuh_layer_id & 0x1F) << 3) |
		temporal_id_plus1);

	payload = data + 2;
	remaining = length - 2;
	/* Equal fragments, not full ones and a small tail: the same packet
	 * count, but a shorter longest packet, and an RS parity symbol is as
	 * long as its block's longest packet (protocols/venc-fec.md). */
	pieces = (remaining + max_fragment - 1) / max_fragment;
	while (remaining > 0) {
		size_t chunk = (remaining + pieces - 1) / pieces;
		int end = (remaining == chunk);
		int marker = (end && is_last) ? 1 : 0;

		fu_hdr[0] = fu_indicator0;
		fu_hdr[1] = fu_indicator1;
		fu_hdr[2] = (uint8_t)((start ? 0x80 : 0x00) |
			(end ? 0x40 : 0x00) | (nal_type & 0x3F));

		if (rtp_packetizer_send_packet(state, writer, opaque,
			fu_hdr, 3, payload, chunk, marker) != 0)
			return total_bytes;

		if (result) {
			result->packet_count++;
			result->payload_bytes += (uint32_t)(chunk + 3);
			result->fragmented = 1;
		}

		total_bytes += chunk;
		payload += chunk;
		remaining -= chunk;
		pieces--;
		start = 0;
	}

	return total_bytes + 2;
}

size_t rtp_packetizer_send_hevc_nal(RtpPacketizerState *state,
	RtpPacketizerWriteFn writer, void *opaque, const uint8_t *data,
	size_t length, int is_last, size_t max_payload,
	RtpPacketizerResult *result)
{
	rtp_packetizer_reset_result(result);

	if (!data || length == 0 || !state || !writer)
		return 0;

	if (length <= max_payload) {
		if (rtp_packetizer_send_packet(state, writer, opaque, data,
			length, NULL, 0, is_last ? 1 : 0) == 0) {
			if (result) {
				result->packet_count = 1;
				result->payload_bytes = (uint32_t)length;
			}
			return length;
		}
		return 0;
	}

	return rtp_packetizer_send_hevc_fu(state, writer, opaque, data, length,
		is_last, max_payload, result);
}
