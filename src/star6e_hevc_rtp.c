#include "star6e_hevc_rtp.h"

#include "h26x_util.h"
#include "hevc_rtp.h"

#include <string.h>

static int star6e_hevc_rtp_write(const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	return star6e_output_send_rtp_parts(opaque, header, header_len,
		payload1, payload1_len, payload2, payload2_len);
}

size_t star6e_hevc_rtp_send_frame(const MI_VENC_Stream_t *stream,
	Star6eOutput *output, RtpPacketizerState *rtp,
	uint32_t frame_ticks, H26xParamSets *params, size_t max_payload,
	Star6eHevcRtpStats *stats)
{
	size_t total_bytes = 0;

	if (!stream || !output || !rtp)
		return 0;

	if (max_payload > RTP_BUFFER_MAX)
		max_payload = RTP_BUFFER_MAX;

	for (unsigned int i = 0; i < stream->count; ++i) {
		const MI_VENC_Pack_t *pack = &stream->packet[i];
		unsigned int nal_count = (pack->packNum > 0) ?
			(unsigned int)pack->packNum : 1;

		if (!pack->data)
			continue;

		for (unsigned int k = 0; k < nal_count; ++k) {
			const uint8_t *data = NULL;
			size_t length = 0;
			int last_nal;

			if (pack->packNum > 0) {
				MI_U32 offset = pack->packetInfo[k].offset;
				MI_U32 len = pack->packetInfo[k].length;

				if (len == 0 || offset >= pack->length ||
				    len > (pack->length - offset)) {
					continue;
				}
				data = pack->data + offset;
				length = len;
			} else {
				if (pack->length <= pack->offset)
					continue;
				data = pack->data + pack->offset;
				length = pack->length - pack->offset;
			}

			if (!data || length == 0)
				continue;

			last_nal = (i == stream->count - 1) &&
				((pack->packNum > 0 && k == nal_count - 1) ||
				 (pack->packNum == 0));
			total_bytes += hevc_rtp_send_annexb(data, length, params,
				rtp, star6e_hevc_rtp_write, output, last_nal, max_payload,
				stats);
		}
	}

	rtp->timestamp += frame_ticks;
	return total_bytes;
}
