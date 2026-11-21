#include "maruko_video.h"

#include "venc_rec_writer.h"

#include "hevc_rtp.h"
#include "rtp_packetizer.h"
#include "rtp_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* MARUKO_REFTYPE_ENHANCE_P_NOTFORREF (=5) is defined in maruko_video.h. */

typedef struct {
	MarukoOutput *output;
	venc_ring_t *ring;
} MarukoRtpWriteContext;

/* Two passes — size, then copy — rather than a growing buffer: the sizes are
 * already known and an IRAP can be megabytes, so a realloc loop would copy it
 * repeatedly on the encode loop.  Mirrors star6e_output_stream_flatten(). */
static size_t maruko_stream_payload_bytes(const i6c_venc_strm *stream,
	int *out_is_idr)
{
	size_t total = 0;
	unsigned int i;

	if (out_is_idr)
		*out_is_idr = 0;
	for (i = 0; i < stream->count; ++i) {
		const i6c_venc_pack *pack = &stream->packet[i];
		unsigned int k;

		if (!pack->data)
			continue;
		if (pack->packNum == 0) {
			if (pack->length > pack->offset)
				total += pack->length - pack->offset;
			continue;
		}
		for (k = 0; k < (unsigned int)pack->packNum; ++k) {
			unsigned int off = pack->packetInfo[k].offset;
			unsigned int len = pack->packetInfo[k].length;
			unsigned int nalu;

			if (len == 0 || off >= pack->length ||
			    len > (pack->length - off))
				continue;
			nalu = (unsigned int)pack->packetInfo[k].packType.h265Nalu;
			if ((nalu == 19 || nalu == 20) && out_is_idr)
				*out_is_idr = 1;
			total += len;
		}
	}
	return total;
}

uint8_t *maruko_video_stream_flatten(const i6c_venc_strm *stream,
	size_t *out_len, int *out_is_idr)
{
	uint8_t *buf;
	size_t total;
	size_t off_out = 0;
	unsigned int i;
	int is_idr = 0;

	if (out_len)
		*out_len = 0;
	if (out_is_idr)
		*out_is_idr = 0;
	if (!maruko_video_stream_packet_info_complete(stream))
		return NULL;

	total = maruko_stream_payload_bytes(stream, &is_idr);
	/* An access unit larger than the whole queue can never be recorded:
	 * push() would reject it at the cap and free it again.  Refusing here
	 * skips a multi-megabyte malloc+memcpy that exists only to be thrown
	 * away — and on a 89 MB SoC that allocation is not free. */
	if (total == 0 || total > VENC_REC_WRITER_MAX_BYTES)
		return NULL;
	buf = malloc(total);
	if (!buf)
		return NULL;

	for (i = 0; i < stream->count; ++i) {
		const i6c_venc_pack *pack = &stream->packet[i];
		unsigned int k;

		if (!pack->data)
			continue;
		if (pack->packNum == 0) {
			if (pack->length > pack->offset) {
				size_t len = pack->length - pack->offset;

				memcpy(buf + off_out, pack->data + pack->offset,
					len);
				off_out += len;
			}
			continue;
		}
		for (k = 0; k < (unsigned int)pack->packNum; ++k) {
			unsigned int off = pack->packetInfo[k].offset;
			unsigned int len = pack->packetInfo[k].length;

			if (len == 0 || off >= pack->length ||
			    len > (pack->length - off))
				continue;
			memcpy(buf + off_out, pack->data + off, len);
			off_out += len;
		}
	}

	if (out_len)
		*out_len = off_out;
	if (out_is_idr)
		*out_is_idr = is_idr;
	return buf;
}

int maruko_video_stream_packet_info_complete(const i6c_venc_strm *stream)
{
	unsigned int i;

	if (!stream || stream->count == 0 || !stream->packet)
		return 0;

	for (i = 0; i < stream->count; ++i) {
		const i6c_venc_pack *pack = &stream->packet[i];
		const unsigned int info_cap = (unsigned int)(
			sizeof(pack->packetInfo) / sizeof(pack->packetInfo[0]));
		unsigned int k;

		if (!pack->data || pack->packNum > info_cap)
			return 0;
		if (pack->packNum == 0) {
			if (pack->length <= pack->offset)
				return 0;
			continue;
		}
		for (k = 0; k < (unsigned int)pack->packNum; ++k) {
			MI_U32 offset = pack->packetInfo[k].offset;
			MI_U32 length = pack->packetInfo[k].length;

			if (length == 0 || offset >= pack->length ||
			    length > pack->length - offset)
				return 0;
		}
	}

	return 1;
}

int maruko_video_reject_incomplete_access_unit(const i6c_venc_strm *stream,
	MarukoOutput *output)
{
	if (maruko_video_stream_packet_info_complete(stream))
		return 0;
	if (output) {
		__atomic_add_fetch(&output->bad_au_drops, 1, __ATOMIC_RELAXED);
		/* No-op off frame-shm — see the Star6E equivalent. */
		venc_frame_ring_note_other_drop(output->frame_ring);
		if (!output->trunc_warned) {
			output->trunc_warned = 1;
			fprintf(stderr,
				"WARN: Maruko packetInfo table is incomplete "
				"or invalid; dropping whole access unit\n");
		}
		/* Transport-independent, as on Star6E: this runs ahead of the
		 * frame_ring/RTP branch.  Skip only the droppable SVC-T top
		 * layer, which breaks no reference chain. */
		if (output->request_idr &&
		    !(output->svct_active &&
		      stream->h265Info.refType ==
			      MARUKO_REFTYPE_ENHANCE_P_NOTFORREF))
			output->request_idr(output->idr_ctx);
	}
	return 1;
}

static int maruko_rtp_write(const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	const MarukoRtpWriteContext *ctx = opaque;
	MarukoOutput *output;

	if (!ctx || !header || !payload1 || header_len == 0 || payload1_len == 0)
		return -1;

	/* SHM path: write RTP packet to ring buffer (zero pre-flatten copy). */
	if (ctx->ring) {
		size_t total_payload = payload1_len + payload2_len;
		if (header_len > UINT16_MAX || total_payload > UINT16_MAX)
			return -1;
		return venc_ring_write3(ctx->ring,
			header, (uint16_t)header_len,
			payload1, (uint16_t)payload1_len,
			payload2, (uint16_t)payload2_len);
	}

	output = ctx->output;
	if (!output)
		return -1;

	/* Batched path: enqueue into sendmmsg() batch when active. */
	if (output->batch.active) {
		if (maruko_output_batch_enqueue(output, header, header_len,
		    payload1, payload1_len, payload2, payload2_len) == 0)
			goto sent;
		/* Scratch slot too small for this packet — flush anything
		 * already queued (to preserve ordering), then fall through to
		 * immediate send.  The frame keeps its batch, so its parity
		 * still follows all of its video. */
		(void)maruko_output_flush(output);
	}

	/* Fallback: either batch inactive or packet too big for scratch —
	 * send immediately via sendmsg(). */
	if (output_socket_send_parts(output->socket_handle, &output->dst,
	    output->dst_len, output->connected_udp,
	    header, header_len, payload1, payload1_len,
	    payload2, payload2_len) != 0) {
		maruko_output_account_send_failure(output, output->socket_handle, 1);
		return -1;
	}
	__atomic_fetch_add(&output->socket_writes, 1, __ATOMIC_RELAXED);
sent:
	/* After a successful write only: the packetizer advances seq only
	 * then, so source i is always seq base_seq + i. */
	if (output->fec.frame)
		venc_fec_capture(&output->fec, header, payload1, payload1_len,
			payload2, payload2_len);
	return 0;
}

/* H.265-only frame sender: iterates the stream, delegates AP aggregation
 * and FU-A fragmentation to the shared hevc_rtp module. */
static size_t maruko_send_frame_hevc(const i6c_venc_strm *stream,
	MarukoRtpWriteContext *ctx, MarukoRtpState *rtp, H26xParamSets *params,
	size_t max_payload, HevcRtpStats *stats)
{
	RtpPacketizerState state;
	size_t total_bytes = 0;
	unsigned int i;

	state.seq = rtp->seq;
	state.timestamp = rtp->timestamp;
	state.ssrc = rtp->ssrc;
	state.payload_type = rtp->payload_type;

	if (max_payload > RTP_BUFFER_MAX)
		max_payload = RTP_BUFFER_MAX;

	for (i = 0; i < stream->count; ++i) {
		const i6c_venc_pack *pack = &stream->packet[i];
		unsigned int nal_count;
		unsigned int k;

		if (!pack->data)
			continue;

		nal_count = pack->packNum > 0 ? (unsigned int)pack->packNum : 1;

		for (k = 0; k < nal_count; ++k) {
			const uint8_t *data = NULL;
			size_t length = 0;
			int last_nal;

			if (pack->packNum > 0) {
				MI_U32 offset = pack->packetInfo[k].offset;
				MI_U32 len = pack->packetInfo[k].length;

				if (len == 0 || offset >= pack->length ||
				    len > (pack->length - offset))
					continue;
				data = pack->data + offset;
				length = len;
			} else {
				if (pack->length <= pack->offset)
					continue;
				data = pack->data + pack->offset;
				length = pack->length - pack->offset;
			}

			last_nal = (i == stream->count - 1) &&
				((pack->packNum > 0 && k == nal_count - 1) ||
				 (pack->packNum == 0));
			total_bytes += hevc_rtp_send_annexb(data, length, params,
				&state, maruko_rtp_write, ctx, last_nal, max_payload,
				stats);
		}
	}

	rtp->seq = state.seq;
	return total_bytes;
}

static size_t maruko_send_frame_rtp(const i6c_venc_strm *stream,
	MarukoOutput *output, MarukoRtpState *rtp, H26xParamSets *params,
	PAYLOAD_TYPE_E codec, size_t max_payload, HevcRtpStats *stats)
{
	MarukoRtpWriteContext ctx = {
		.output = output,
		.ring = output ? output->ring : NULL,
	};
	HevcRtpStats frame_stats = {0};
	size_t total_bytes;

	if (!stream || !output || !rtp)
		return 0;
	/* HEVC is the only supported codec since 0.10.12 — defensive
	 * non-PT_H265 branch removed. */
	(void)codec;

	/* Open sendmmsg batch for UDP; no-op for SHM. */
	maruko_output_begin_frame(output);
	if (output->fec.frame)
		max_payload = venc_fec_payload_size((uint16_t)max_payload,
			output->slice_avg);
	if (!stats)
		stats = &frame_stats; /* slice sizes, every frame */
	total_bytes = maruko_send_frame_hevc(stream, &ctx, rtp, params,
		max_payload, stats);
	(void)maruko_output_end_frame(output);
	__atomic_store_n(&output->payload_used, (uint16_t)max_payload,
		__ATOMIC_RELAXED);
	output->slice_avg = venc_fec_slice_avg(output->slice_avg,
		stats->slices, stats->slice_bytes);
	rtp->timestamp += rtp->frame_ticks;
	return total_bytes;
}

static size_t maruko_send_udp_chunks(const uint8_t *data, size_t length,
	int socket_handle, const struct sockaddr_storage *dst,
	socklen_t dst_len, int connected_udp, uint32_t max_size)
{
	size_t total_sent = 0;
	size_t chunk_cap;

	if (!data || length == 0 || socket_handle < 0)
		return 0;
	if (!connected_udp && (!dst || dst_len == 0))
		return 0;

	chunk_cap = max_size ? max_size : 1400;
	while (total_sent < length) {
		size_t remaining = length - total_sent;
		size_t chunk = remaining > chunk_cap ? chunk_cap : remaining;
		ssize_t rc;

		if (connected_udp) {
			/* Connected UDP socket: destination was set at
			 * connect() time. Passing a sockaddr here would
			 * return EISCONN on some kernels. */
			rc = send(socket_handle, data + total_sent, chunk, 0);
		} else {
			rc = sendto(socket_handle, data + total_sent, chunk, 0,
				(const struct sockaddr *)dst, dst_len);
		}

		if (rc < 0)
			break;
		total_sent += chunk;
	}
	return total_sent;
}

static size_t maruko_send_frame_compact(const i6c_venc_strm *stream,
	int socket_handle, const struct sockaddr_storage *dst,
	socklen_t dst_len, int connected_udp, uint32_t max_size)
{
	size_t total_bytes = 0;
	unsigned int i;

	if (!stream)
		return 0;
	if (!connected_udp && !dst)
		return 0;

	for (i = 0; i < stream->count; ++i) {
		const i6c_venc_pack *pack = &stream->packet[i];

		if (!pack->data)
			continue;

		if (pack->packNum > 0) {
			unsigned int nal_count = (unsigned int)pack->packNum;
			unsigned int k;

			for (k = 0; k < nal_count; ++k) {
				MI_U32 length = pack->packetInfo[k].length;
				MI_U32 offset = pack->packetInfo[k].offset;

				if (length == 0 || offset >= pack->length ||
				    length > (pack->length - offset))
					continue;
				total_bytes += maruko_send_udp_chunks(
					pack->data + offset, length,
					socket_handle, dst, dst_len,
					connected_udp, max_size);
			}
		} else if (pack->length > pack->offset) {
			MI_U32 length = pack->length - pack->offset;

			total_bytes += maruko_send_udp_chunks(pack->data +
				pack->offset, length, socket_handle, dst,
				dst_len, connected_udp, max_size);
		}
	}

	return total_bytes;
}

void maruko_video_init_rtp_state(MarukoRtpState *rtp,
	PAYLOAD_TYPE_E codec, uint32_t sensor_fps)
{
	if (!rtp)
		return;

	rtp_session_init(rtp, rtp_session_payload_type(codec), sensor_fps);
}

/* Abort a partially written ring slot because the access unit does not fit.
 *
 * The drop is counted -- stats.oversize_drops, and separately mirrored into
 * the ring header's other_drops by venc_frame_ring_note_other_drop() -- but
 * until now nothing was logged, so the frame
 * simply vanished: 0.73.0 removed the frame-size caps that were supposed to
 * prevent it, and ring v2 removed the recovery IDR that used to heal the
 * chain afterwards.
 *
 * Deliberately does NOT request an IDR.  An access unit that overflows the
 * slot does so because the craft's bitrate genuinely exceeds it, which
 * repeats every GOP -- asking each time would be exactly the automatic
 * keyframing ring v2 removed. */
static void maruko_video_abort_oversize(MarukoOutput *output)
{
	venc_frame_ring_abort_write(output->frame_ring);
	if (output->oversize_warned)
		return;
	output->oversize_warned = 1;
	fprintf(stderr,
		"WARN: Maruko access unit exceeds the frame-shm slot "
		"(%u bytes of payload); dropping the whole frame\n",
		(unsigned)(output->frame_ring->slot_data_size -
			VENC_FRAME_META_SIZE));
}

static size_t maruko_send_frame_ring(const i6c_venc_strm *stream,
	MarukoOutput *output)
{
	VencFrameMeta meta;
	venc_frame_ring_t *frame_ring;
	size_t total_bytes = 0;
	unsigned int i;
	int is_idr = 0;

	if (!stream || !output || !output->frame_ring)
		return 0;

	frame_ring = output->frame_ring;

	/* IDR detection from packType.h265Nalu (types 19, 20) */
	for (i = 0; i < stream->count && !is_idr; ++i) {
		const i6c_venc_pack *pack = &stream->packet[i];
		if (pack->packNum > 0) {
			unsigned int nal_count = (unsigned int)pack->packNum;
			unsigned int k;
			for (k = 0; k < nal_count; ++k) {
				uint8_t nt = (uint8_t)pack->packetInfo[k]
					.packType.h265Nalu;
				if (nt == 19 || nt == 20) {
					is_idr = 1;
					break;
				}
			}
		}
	}

	memset(&meta, 0, sizeof(meta));
	meta.pts = (stream->count > 0 && stream->packet)
		? (uint32_t)stream->packet[0].timestamp : 0;
	meta.codec = VENC_FRAME_CODEC_H265;
	meta.flags = is_idr ? VENC_FRAME_FLAG_IDR : 0;
	if (!is_idr && output->gdr_active) {
		meta.flags |= VENC_FRAME_FLAG_GDR;
		meta.gdr_pos = output->gdr_counter;
		meta.gdr_len = output->gdr_cycle_len;
		output->gdr_counter++;
		if (output->gdr_counter >= output->gdr_cycle_len)
			output->gdr_counter = 0;
	} else if (is_idr) {
		output->gdr_counter = 0;
	}
	if (output->svct_active &&
	    stream->h265Info.refType == MARUKO_REFTYPE_ENHANCE_P_NOTFORREF)
		meta.flags |= VENC_FRAME_FLAG_ENHANCE;

	if (venc_frame_ring_begin_write(frame_ring, &meta) != 0)
		return 0;

	for (i = 0; i < stream->count; ++i) {
		const i6c_venc_pack *pack = &stream->packet[i];

		if (!pack->data)
			continue;

		if (pack->packNum > 0) {
			unsigned int nal_count = (unsigned int)pack->packNum;
			unsigned int k;

			for (k = 0; k < nal_count; ++k) {
				unsigned int length = pack->packetInfo[k].length;
				unsigned int offset = pack->packetInfo[k].offset;

				if (length == 0 || offset >= pack->length ||
				    length > (pack->length - offset))
					continue;

				if (venc_frame_ring_append(frame_ring,
				    pack->data + offset, length) != 0) {
					maruko_video_abort_oversize(output);
					return 0;
				}
				total_bytes += length;
			}
			continue;
		}

		if (pack->length > pack->offset) {
			unsigned int length = pack->length - pack->offset;

			if (venc_frame_ring_append(frame_ring,
			    pack->data + pack->offset, length) != 0) {
				maruko_video_abort_oversize(output);
				return 0;
			}
			total_bytes += length;
		}
	}

	venc_frame_ring_commit_write(frame_ring);
	return total_bytes;
}

size_t maruko_video_send_frame(const i6c_venc_strm *stream,
	MarukoOutput *output, MarukoRtpState *rtp,
	H26xParamSets *params, MarukoBackendConfig *cfg, HevcRtpStats *stats)
{
	size_t total_bytes;

	if (!stream || !output || !cfg)
		return 0;
	if (maruko_video_reject_incomplete_access_unit(stream, output))
		return 0;

	if (output->frame_ring)
		return maruko_send_frame_ring(stream, output);

	if (output->socket_handle < 0 && !output->ring)
		return 0;

	if (cfg->stream_mode == MARUKO_STREAM_RTP) {
		total_bytes = maruko_send_frame_rtp(stream, output, rtp, params,
			cfg->rc_codec, cfg->rtp_payload_size, stats);
	} else if (!output->ring) {
		total_bytes = maruko_send_frame_compact(stream,
			output->socket_handle, &output->dst, output->dst_len,
			output->connected_udp, cfg->max_frame_size);
	} else {
		/* Compact mode not supported over SHM */
		return 0;
	}

	return total_bytes;
}
