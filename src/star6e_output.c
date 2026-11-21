#include "star6e_output.h"

#include "venc_rec_writer.h"

#include "output_socket.h"
#include "rs_fec.h"
#include "timing.h"
#include "venc_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define STAR6E_RTP_HEADER_SIZE 12
/* STAR6E_REFTYPE_ENHANCE_P_NOTFORREF (=5) is defined in star6e.h. */

/* Walk the pack list once per pass: size, then copy.  Two passes rather than
 * a realloc loop because the sizes are already known and an IRAP can be
 * megabytes — a growing buffer would copy it repeatedly on the encode loop. */
static size_t stream_payload_bytes(const MI_VENC_Stream_t *stream,
	int *out_is_idr)
{
	size_t total = 0;
	unsigned int i;

	if (out_is_idr)
		*out_is_idr = 0;
	for (i = 0; i < stream->count; ++i) {
		const MI_VENC_Pack_t *pack = &stream->packet[i];
		unsigned int k;

		if (!pack->data)
			continue;
		if (pack->packNum == 0) {
			if (pack->length > pack->offset)
				total += pack->length - pack->offset;
			continue;
		}
		for (k = 0; k < (unsigned int)pack->packNum; ++k) {
			MI_U32 off = pack->packetInfo[k].offset;
			MI_U32 len = pack->packetInfo[k].length;
			unsigned int nalu;

			if (len == 0 || off >= pack->length ||
			    len > (pack->length - off))
				continue;
			/* IDR_W_RADL (19) and IDR_N_LP (20), matching the
			 * synchronous writer's test exactly. */
			nalu = (unsigned int)pack->packetInfo[k].packType.h265Nalu;
			if ((nalu == 19 || nalu == 20) && out_is_idr)
				*out_is_idr = 1;
			total += len;
		}
	}
	return total;
}

uint8_t *star6e_output_stream_flatten(const MI_VENC_Stream_t *stream,
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
	/* Same refusal as the synchronous writers: an incomplete packetInfo
	 * table means the pack boundaries are unknown, and half an access unit
	 * is worse than none. */
	if (!star6e_output_stream_packet_info_complete(stream))
		return NULL;

	total = stream_payload_bytes(stream, &is_idr);
	/* An access unit larger than the whole queue can never be recorded:
	 * push() would reject it at the cap and free it again.  Refusing here
	 * skips a multi-megabyte malloc+memcpy that exists only to be thrown
	 * away — and on a 64 MB SoC that allocation is not free. */
	if (total == 0 || total > VENC_REC_WRITER_MAX_BYTES)
		return NULL;
	buf = malloc(total);
	if (!buf)
		return NULL;

	for (i = 0; i < stream->count; ++i) {
		const MI_VENC_Pack_t *pack = &stream->packet[i];
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
			MI_U32 off = pack->packetInfo[k].offset;
			MI_U32 len = pack->packetInfo[k].length;

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

int star6e_output_stream_packet_info_complete(
	const MI_VENC_Stream_t *stream)
{
	unsigned int i;

	if (!stream || stream->count == 0 || !stream->packet)
		return 0;
	for (i = 0; i < stream->count; ++i) {
		const MI_VENC_Pack_t *pack = &stream->packet[i];
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

int star6e_output_reject_incomplete_access_unit(Star6eOutput *output,
	const MI_VENC_Stream_t *stream)
{
	if (star6e_output_stream_packet_info_complete(stream))
		return 0;
	if (output) {
		__atomic_add_fetch(&output->bad_au_drops, 1, __ATOMIC_RELAXED);
		/* No-op off frame-shm; the per-output counter above is the
		 * transport-independent record. */
		venc_frame_ring_note_other_drop(output->frame_ring);
		if (!output->trunc_warned) {
			output->trunc_warned = 1;
			fprintf(stderr,
				"WARN: Star6E packetInfo table is incomplete "
				"or invalid; dropping whole access unit\n");
		}
		/* Transport-independent: this runs before the frame_ring/RTP
		 * branch in star6e_output_send_frame(), because a malformed AU
		 * is an encoder fault, not ring pressure.  Skip only the
		 * droppable SVC-T top layer, which breaks no chain. */
		if (output->request_idr &&
		    !(output->svct_active &&
		      stream->h265Info.refType ==
			      STAR6E_REFTYPE_ENHANCE_P_NOTFORREF))
			output->request_idr(output->idr_ctx);
	}
	return 1;
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
static void star6e_output_abort_oversize(Star6eOutput *output)
{
	venc_frame_ring_abort_write(output->frame_ring);
	if (output->oversize_warned)
		return;
	output->oversize_warned = 1;
	fprintf(stderr,
		"WARN: Star6E access unit exceeds the frame-shm slot "
		"(%u bytes of payload); dropping the whole frame\n",
		(unsigned)(output->frame_ring->slot_data_size -
			VENC_FRAME_META_SIZE));
}

static uint16_t star6e_read_be16(const uint8_t *data)
{
	return (uint16_t)((uint16_t)data[0] << 8 | (uint16_t)data[1]);
}

static uint32_t star6e_read_be32(const uint8_t *data)
{
	return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
		((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static void star6e_write_be16(uint8_t *data, uint16_t value)
{
	data[0] = (uint8_t)(value >> 8);
	data[1] = (uint8_t)(value & 0xff);
}

static void star6e_write_be32(uint8_t *data, uint32_t value)
{
	data[0] = (uint8_t)(value >> 24);
	data[1] = (uint8_t)((value >> 16) & 0xff);
	data[2] = (uint8_t)((value >> 8) & 0xff);
	data[3] = (uint8_t)(value & 0xff);
}

/* Classify a failed send.  Congestion — the peer's datagram queue stayed
 * full for the whole SO_SNDTIMEO window, or the device queue is full — is a
 * transport drop, not a fault, and the EAGAIN case also calibrates the
 * unix:// fill denominator.  Everything else is a real error.  Reads the
 * errno set by the failed send, so call it immediately after. */
static void star6e_account_send_failure(Star6eOutput *output, int fd,
	uint32_t packets)
{
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
		output_socket_note_saturation(fd, &output->send_queue);
		__atomic_fetch_add(&output->socket_drops, packets,
			__ATOMIC_RELAXED);
	} else {
		output->send_errors += packets;
	}
}

static int resolve_shared_audio_target(const Star6eAudioOutput *audio_output,
	Star6eAudioSendTarget *target)
{
	if (!audio_output || !audio_output->video_output || !target)
		return -1;

	if (audio_output->video_output->socket_handle < 0 ||
	    audio_output->video_output->ring ||
	    audio_output->video_output->dst_len == 0) {
		return -1;
	}

	target->socket_handle = audio_output->video_output->socket_handle;
	memcpy(&target->dst, &audio_output->video_output->dst, sizeof(target->dst));
	target->dst_len = audio_output->video_output->dst_len;
	return 0;
}

static int resolve_dedicated_audio_target(const Star6eAudioOutput *audio_output,
	Star6eAudioSendTarget *target)
{
	struct sockaddr_in *udp_dst;

	if (!audio_output || !audio_output->video_output || !target ||
	    audio_output->socket_handle < 0) {
		return -1;
	}

	target->socket_handle = audio_output->socket_handle;
	if (audio_output->video_output->transport == VENC_OUTPUT_URI_UDP &&
	    audio_output->video_output->dst_len == sizeof(struct sockaddr_in)) {
		memcpy(&target->dst, &audio_output->video_output->dst,
			sizeof(target->dst));
		target->dst_len = audio_output->video_output->dst_len;
		udp_dst = (struct sockaddr_in *)&target->dst;
		udp_dst->sin_port = htons(audio_output->port_override);
		return 0;
	}

	memcpy(&target->dst, &audio_output->fallback_dst, sizeof(target->dst));
	target->dst_len = audio_output->fallback_dst_len;
	return 0;
}

static int resolve_audio_target(const Star6eAudioOutput *audio_output,
	Star6eAudioSendTarget *target)
{
	if (!audio_output || !target)
		return -1;
	if (audio_output->port_override == 0)
		return resolve_shared_audio_target(audio_output, target);
	return resolve_dedicated_audio_target(audio_output, target);
}

static int resolve_cached_audio_target(Star6eAudioOutput *ao,
	Star6eAudioSendTarget *target)
{
	uint32_t gen;

	if (!ao || !target || !ao->video_output)
		return -1;

	gen = __atomic_load_n(&ao->video_output->transport_gen, __ATOMIC_ACQUIRE);
	if (ao->cache_valid && ao->cached_gen == gen && !(gen & 1)) {
		*target = ao->cached_target;
		return 0;
	}

	/* Cache miss or writer in progress — resolve from live state */
	if (resolve_audio_target(ao, target) != 0)
		return -1;

	/* Only cache if the generation is stable (even = no write in progress)
	 * and unchanged since we started reading */
	gen = __atomic_load_n(&ao->video_output->transport_gen, __ATOMIC_ACQUIRE);
	if (!(gen & 1)) {
		ao->cached_target = *target;
		ao->cached_gen = gen;
		ao->cache_valid = 1;
	}
	return 0;
}

static int send_audio_rtp(const uint8_t *header,
	size_t header_len, const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	const Star6eAudioSendTarget *target = opaque;

	if (!target)
		return -1;

	return output_socket_send_parts(target->socket_handle, &target->dst,
		target->dst_len, 0 /* audio socket never connect()ed */,
		header, header_len, payload1, payload1_len, payload2, payload2_len);
}

static void star6e_output_setup_reset(Star6eOutputSetup *setup)
{
	if (!setup)
		return;

	memset(setup, 0, sizeof(*setup));
	setup->uri.type = VENC_OUTPUT_URI_UDP;
}

void star6e_output_reset(Star6eOutput *output)
{
	if (!output)
		return;

	memset(output, 0, sizeof(*output));
	output->socket_handle = -1;
	output->transport = VENC_OUTPUT_URI_UDP;
}

static Star6eStreamMode star6e_output_stream_mode_from_name(
	const char *stream_mode_name)
{
	if (stream_mode_name && strcmp(stream_mode_name, "compact") == 0)
		return STAR6E_STREAM_MODE_COMPACT;

	return STAR6E_STREAM_MODE_RTP;
}

int star6e_output_prepare(Star6eOutputSetup *setup, const char *server_uri,
	const char *stream_mode_name, int connected_udp)
{
	if (!setup)
		return -1;

	star6e_output_setup_reset(setup);
	setup->stream_mode = star6e_output_stream_mode_from_name(stream_mode_name);
	setup->requested_connected_udp = connected_udp ? 1 : 0;

	if (!server_uri || !server_uri[0])
		return 0;

	setup->has_server = 1;
	if (venc_config_parse_output_uri(server_uri, &setup->uri) != 0)
		return -1;
	if (setup->uri.type == VENC_OUTPUT_URI_SHM) {
		if (setup->stream_mode != STAR6E_STREAM_MODE_RTP) {
			fprintf(stderr, "ERROR: shm:// output requires RTP stream mode.\n");
			return -1;
		}
		return 0;
	}

	if (setup->uri.type == VENC_OUTPUT_URI_FRAME_SHM)
		return 0;

	return 0;
}

int star6e_output_setup_is_rtp(const Star6eOutputSetup *setup)
{
	return setup && setup->stream_mode == STAR6E_STREAM_MODE_RTP;
}

int star6e_output_init(Star6eOutput *output, const Star6eOutputSetup *setup)
{
	uint32_t slot_data;

	if (!output)
		return -1;

	star6e_output_reset(output);
	if (!setup)
		return -1;

	output->stream_mode = setup->stream_mode;
	output->requested_connected_udp = setup->requested_connected_udp;
	output->allow_unix_encoder_stall = setup->allow_unix_encoder_stall;
	/* RS parity rides the udp:// / unix:// RTP batch only; no server yet
	 * counts, since a live server change cannot leave those two. */
	if (setup->fec && (setup->uri.type == VENC_OUTPUT_URI_UDP ||
	    setup->uri.type == VENC_OUTPUT_URI_UNIX) &&
	    star6e_output_set_fec(output, 1) != 0)
		return -1;
	if (!setup->has_server)
		return 0;

	if (setup->uri.type == VENC_OUTPUT_URI_SHM) {
		/* Slot fits the validated payload ceiling so any value in
		 * [VENC_OUTPUT_PAYLOAD_MIN_BYTES,
		 * VENC_OUTPUT_PAYLOAD_CEILING_BYTES] applies live without
		 * restart, matching UDP/unix:// behavior. */
		slot_data = (uint32_t)VENC_OUTPUT_PAYLOAD_CEILING_BYTES + 12;
		output->ring = venc_ring_create(setup->uri.endpoint, 512, slot_data);
		if (!output->ring) {
			fprintf(stderr, "ERROR: venc_ring_create(%s) failed\n",
				setup->uri.endpoint);
			return -1;
		}

		output->transport = VENC_OUTPUT_URI_SHM;
		memset(&output->dst, 0, sizeof(output->dst));
		output->dst_len = 0;
		output->connected_udp = 0;
		__atomic_fetch_add(&output->transport_gen, 2, __ATOMIC_RELEASE);
		return 0;
	}

	if (setup->uri.type == VENC_OUTPUT_URI_FRAME_SHM) {
		output->frame_ring = venc_frame_ring_create(
			setup->uri.endpoint, 8, 384 * 1024);
		if (!output->frame_ring) {
			fprintf(stderr, "ERROR: venc_frame_ring_create(%s) failed\n",
				setup->uri.endpoint);
			return -1;
		}

		output->transport = VENC_OUTPUT_URI_FRAME_SHM;
		memset(&output->dst, 0, sizeof(output->dst));
		output->dst_len = 0;
		output->connected_udp = 0;
		__atomic_fetch_add(&output->transport_gen, 2, __ATOMIC_RELEASE);
		return 0;
	}

	if (output_socket_configure(&output->socket_handle, &output->dst,
	    &output->dst_len, &output->transport, &setup->uri,
	    output->requested_connected_udp, output->allow_unix_encoder_stall,
	    &output->connected_udp) != 0)
		return -1;
	(void)output_socket_capture_capacity(output->socket_handle,
		&output->send_queue);
	__atomic_fetch_add(&output->transport_gen, 2, __ATOMIC_RELEASE);
	return 0;
}

int star6e_output_set_fec(Star6eOutput *output, int on)
{
	if (!output)
		return -1;
	if (on && !output->fec_arena) {
		if (!star6e_output_is_rtp(output) ||
		    (output->transport != VENC_OUTPUT_URI_UDP &&
		     output->transport != VENC_OUTPUT_URI_UNIX))
			return -1;
		output->fec_arena = malloc((size_t)STAR6E_FEC_MAX_SRC *
			STAR6E_FEC_SYM_MAX);
		if (!output->fec_arena)
			return -1;
		output->fec_rtp.payload_type = STAR6E_FEC_PT;
	}
	/* Release: the encode thread acquires this at begin_frame before it
	 * touches the arena set up above. */
	__atomic_store_n(&output->fec_on, on ? 1 : 0, __ATOMIC_RELEASE);
	return 0;
}

int star6e_output_is_rtp(const Star6eOutput *output)
{
	return output && output->stream_mode == STAR6E_STREAM_MODE_RTP;
}

int star6e_output_is_shm(const Star6eOutput *output)
{
	return output && output->transport == VENC_OUTPUT_URI_SHM;
}

int star6e_output_is_frame_shm(const Star6eOutput *output)
{
	return output && output->transport == VENC_OUTPUT_URI_FRAME_SHM;
}

int star6e_output_frame_ring_fill(
	const Star6eOutput *output, venc_frame_ring_fill_t *out)
{
	if (!output || !output->frame_ring || !out)
		return -1;
	return venc_frame_ring_get_fill(output->frame_ring, out);
}

void star6e_output_observe_pressure(Star6eOutput *output)
{
	uint8_t fill_pct = 0;
	uint32_t full_drops = 0;
	uint32_t writes = 0;
	uint32_t oversize_drops = 0;
	int have_fill = 0;

	if (!output)
		return;

	/* Pick the fill source based on the active transport:
	 *   shm://   ring fill (write_idx - read_idx) / slot_count
	 *   unix://  SIOCOUTQ / SO_SNDBUF
	 *   udp://   same.  link_controller / external schedulers consume
	 *            the resulting in_pressure flag via the sidecar trailer
	 *            and adapt with bitrate / fps writes upstream of encode. */
	if (output->ring) {
		venc_ring_fill_t fill;
		if (venc_ring_get_fill(output->ring, &fill) == 0) {
			fill_pct = fill.fill_pct;
			full_drops = (uint32_t)fill.full_drops;
			writes = (uint32_t)fill.writes;
			oversize_drops = (uint32_t)fill.oversize_drops;
			have_fill = 1;
		}
	} else if (output->frame_ring) {
		venc_frame_ring_fill_t fill;
		if (venc_frame_ring_get_fill(output->frame_ring, &fill) == 0) {
			fill_pct = fill.fill_pct;
			full_drops = (uint32_t)fill.full_drops;
			writes = (uint32_t)fill.writes;
			oversize_drops = (uint32_t)fill.oversize_drops;
			have_fill = 1;
		}
	} else if ((output->transport == VENC_OUTPUT_URI_UNIX ||
	            output->transport == VENC_OUTPUT_URI_UDP) &&
	           output->socket_handle >= 0) {
		if (output_socket_get_fill_pct(output->socket_handle,
		    &output->send_queue, &fill_pct) == 0) {
			full_drops = __atomic_load_n(&output->socket_drops,
				__ATOMIC_RELAXED);
			writes = __atomic_load_n(&output->socket_writes,
				__ATOMIC_RELAXED);
			have_fill = 1;
		}
	}

	if (!have_fill) {
		/* No transport configured or query failed — clear flag so we
		 * don't get stuck reporting pressure across teardown / a
		 * transient ioctl error.  Cached fill_pct stays at its last
		 * value; trailer readers will see in_pressure=0 either way. */
		__atomic_store_n(&output->in_pressure, 0, __ATOMIC_RELAXED);
		return;
	}

	venc_observe_pressure(fill_pct,
		&output->in_pressure, &output->pressure_drops);

	__atomic_store_n(&output->last_fill_pct, fill_pct, __ATOMIC_RELAXED);
	__atomic_store_n(&output->last_full_drops, full_drops, __ATOMIC_RELAXED);
	__atomic_store_n(&output->last_writes, writes, __ATOMIC_RELAXED);
	__atomic_store_n(&output->last_oversize_drops, oversize_drops,
		__ATOMIC_RELAXED);
}

uint32_t star6e_output_drain_send_errors(Star6eOutput *output)
{
	uint32_t n;
	if (!output)
		return 0;
	n = output->send_errors;
	output->send_errors = 0;
	return n;
}

/* Flush the accumulated batch via sendmmsg().
 *
 * On partial success (sendmmsg returns 0 < n < count) or EINTR, retry
 * from the first unsent message.
 *
 * Two distinct failure modes are accounted separately:
 *
 *   EAGAIN / a short write  — the peer's queue is full.  On unix:// this
 *     is the SO_SNDTIMEO window expiring; it is congestion, not a fault,
 *     and it calibrates the fill_pct denominator (output_socket_note_
 *     saturation).  Counted into output->socket_drops, which reaches the
 *     sidecar trailer as transport_drops.
 *
 *   anything else — a real error (peer gone, bad address).  Counted into
 *     output->send_errors as before.
 *
 * Either way the remaining packets are abandoned rather than retried
 * indefinitely: an over-budget frame is already undecodable at the
 * receiver (the H.265 reference chain is broken by the first lost packet),
 * so continuing to block only propagates the stall into capture.
 *
 * Returns number of messages successfully sent. Always resets
 * batch->count to 0. */
static int star6e_batch_flush(Star6eOutput *output)
{
	Star6eOutputBatch *b = &output->batch;
	size_t sent_total = 0;
	uint64_t started;
	uint64_t deadline;
	uint64_t elapsed;
	int fd;

	if (b->count == 0)
		return 0;
	if (b->discard_remaining) {
		if (b->discard_as_error)
			output->send_errors += (uint32_t)b->count;
		else
			__atomic_fetch_add(&output->socket_drops,
				(uint32_t)b->count, __ATOMIC_RELAXED);
		b->count = 0;
		return 0;
	}

	/* Use the batch-snapshotted socket — output->socket_handle can be
	 * mutated by a concurrent apply_server() on the HTTP thread between
	 * begin_frame and here. */
	fd = b->socket_handle;
	if (fd < 0) {
		output->send_errors += (uint32_t)b->count;
		b->discard_remaining = 1;
		b->discard_as_error = 1;
		b->count = 0;
		return 0;
	}

	started = 0;
	deadline = 0;
	if (!b->allow_unix_encoder_stall) {
		started = wb_monotonic_us();
		deadline = started + b->flush_budget_us;
	}

	while (sent_total < b->count) {
		int n = sendmmsg(fd, b->msgs + sent_total,
			(unsigned int)(b->count - sent_total), 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				/* unix:// only — udp:// carries no send
				 * timeout and so never reports EAGAIN.  Each
				 * retry has already cost a SO_SNDTIMEO sleep
				 * in the kernel, so looping to the deadline
				 * costs ~2 iterations, not a busy-spin. */
				output_socket_note_saturation(fd,
					&output->send_queue);
				if (b->allow_unix_encoder_stall)
					continue;
				if (wb_monotonic_us() < deadline)
					continue;
				__atomic_fetch_add(&output->socket_drops,
					(uint32_t)(b->count - sent_total),
					__ATOMIC_RELAXED);
				b->discard_remaining = 1;
				break;
			}
			if (errno == ENOBUFS) {
				/* Device/qdisc queue full, typically udp:// on
				 * a congested link.  Returned immediately with
				 * no sleep, so retrying here would spin the
				 * encode thread for the whole budget.  Count
				 * as congestion and move on. */
				__atomic_fetch_add(&output->socket_drops,
					(uint32_t)(b->count - sent_total),
					__ATOMIC_RELAXED);
				b->discard_remaining = 1;
				break;
			}
			/* Permanent error on the next unsent message:
			 * account remaining as drops and bail. */
			output->send_errors +=
				(uint32_t)(b->count - sent_total);
			b->discard_remaining = 1;
			b->discard_as_error = 1;
			break;
		}
		if (n == 0) {
			/* Defensive: sendmmsg returning 0 should not happen,
			 * but treat as permanent to avoid a spin. */
			output->send_errors +=
				(uint32_t)(b->count - sent_total);
			b->discard_remaining = 1;
			b->discard_as_error = 1;
			break;
		}
		sent_total += (size_t)n;
		/* A short write means the queue filled mid-batch: an exact
		 * capacity reading for the unix:// fill denominator. */
		if (sent_total < b->count) {
			output_socket_note_saturation(fd, &output->send_queue);
			if (b->allow_unix_encoder_stall)
				continue;
			if (wb_monotonic_us() >= deadline) {
				__atomic_fetch_add(&output->socket_drops,
					(uint32_t)(b->count - sent_total),
					__ATOMIC_RELAXED);
				b->discard_remaining = 1;
				break;
			}
		}
	}

	__atomic_fetch_add(&output->socket_writes, (uint32_t)sent_total,
		__ATOMIC_RELAXED);
	if (!b->allow_unix_encoder_stall) {
		elapsed = wb_monotonic_us() - started;
		if (elapsed >= b->flush_budget_us) {
			b->flush_budget_us = 0;
			b->discard_remaining = 1;
		} else {
			b->flush_budget_us -= (uint32_t)elapsed;
		}
	}
	b->count = 0;
	return (int)sent_total;
}

void star6e_output_begin_frame(Star6eOutput *output)
{
	Star6eOutputBatch *b;
	uint32_t gen_before, gen_after;

	if (!output)
		return;
	b = &output->batch;
	b->count = 0;
	b->active = 0;
	output->fec_count = 0;
	/* Latched per frame so a live toggle never splits one. */
	output->fec_frame = __atomic_load_n(&output->fec_on, __ATOMIC_ACQUIRE);
	b->flush_budget_us = STAR6E_OUTPUT_FLUSH_BUDGET_US;
	b->discard_remaining = 0;
	b->discard_as_error = 0;

	/* SHM output is not batched — skip the snapshot entirely. */
	if (output->ring)
		return;

	/* Seqlock read of transport state: retry while apply_server() holds
	 * an odd generation. Matches the writer pattern in
	 * star6e_output_apply_server. */
	for (;;) {
		gen_before = __atomic_load_n(&output->transport_gen,
			__ATOMIC_ACQUIRE);
		if (gen_before & 1u) {
			/* Writer in progress.  apply_server() holds the odd
			 * generation across socket()/setsockopt()/connect(),
			 * so yield rather than burning the encode thread's
			 * slice against the HTTP thread — this thread is
			 * pinned to CPU 0 and can otherwise starve the writer
			 * it is waiting on. */
			sched_yield();
			continue;
		}
		b->socket_handle = output->socket_handle;
		b->dst = output->dst;
		b->dst_len = output->dst_len;
		b->connected_udp = output->connected_udp;
		b->allow_unix_encoder_stall =
			(output->transport == VENC_OUTPUT_URI_UNIX) &&
			output->allow_unix_encoder_stall;
		gen_after = __atomic_load_n(&output->transport_gen,
			__ATOMIC_ACQUIRE);
		if (gen_before == gen_after)
			break;
	}

	b->active = (b->socket_handle >= 0) ? 1 : 0;
}

static int star6e_output_send_parts(Star6eOutput *output,
	const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len);

/* Keep a sent video packet's payload as a source symbol: 2 B length, then
 * the payload zero-padded to the arena stride.  Called only after a
 * successful send, and the packetizer advances seq only then, so source i
 * is always seq fec_base_seq + i.  The first packet that cannot be kept
 * ends capture for the frame (fec_frame = 2): later ones would break the
 * base + i pairing, and no block may then claim to end the frame. */
static void star6e_fec_capture(Star6eOutput *output, const uint8_t *header,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len)
{
	const size_t len = payload1_len + payload2_len;
	uint8_t *sym;

	if (output->fec_frame != 1 || output->fec_count >= STAR6E_FEC_MAX_SRC ||
	    len > VENC_FEC_MAX_PAYLOAD_BYTES) {
		output->fec_frame = 2;
		__atomic_fetch_add(&output->fec_unprotected, 1, __ATOMIC_RELAXED);
		return;
	}
	if (output->fec_count == 0) {
		output->fec_base_seq = star6e_read_be16(header + 2);
		output->fec_rtp.timestamp = star6e_read_be32(header + 4);
		output->fec_rtp.ssrc = star6e_read_be32(header + 8) ^
			STAR6E_FEC_SSRC_XOR;
	}
	sym = output->fec_arena + (size_t)output->fec_count * STAR6E_FEC_SYM_MAX;
	star6e_write_be16(sym, (uint16_t)len);
	memcpy(sym + 2, payload1, payload1_len);
	if (payload2_len)
		memcpy(sym + 2 + payload1_len, payload2, payload2_len);
	memset(sym + 2 + len, 0, VENC_FEC_MAX_PAYLOAD_BYTES - len);
	output->fec_count++;
}

static int star6e_fec_write(const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	return star6e_output_send_parts(opaque, header, header_len,
		payload1, payload1_len, payload2, payload2_len);
}

/* Send this frame's parity (protocols/venc-fec.md): blocks of at most
 * STAR6E_FEC_BLOCK_MAX sources; r per block from a byte credit worth
 * 1/VENC_FEC_PARITY_DIV of the source datagrams, so a small block may get
 * none and small frames share protection instead of overspending. */
static void star6e_fec_emit(Star6eOutput *output)
{
	const unsigned total = output->fec_count;
	const unsigned nb = (total + STAR6E_FEC_BLOCK_MAX - 1) / STAR6E_FEC_BLOCK_MAX;
	uint8_t par[STAR6E_FEC_HDR + STAR6E_FEC_SYM_MAX];
	unsigned b, i, first = 0;

	for (b = 0; b < nb; b++) {
		const unsigned k = total / nb + (b < total % nb);
		const uint8_t *src = output->fec_arena +
			(size_t)first * STAR6E_FEC_SYM_MAX;
		size_t maxlen = 0, src_bytes = 0, plen;
		unsigned r;

		for (i = 0; i < k; i++) {
			const size_t len = star6e_read_be16(src +
				(size_t)i * STAR6E_FEC_SYM_MAX);

			if (len > maxlen)
				maxlen = len;
			src_bytes += STAR6E_RTP_HEADER_SIZE + len;
		}
		plen = STAR6E_RTP_HEADER_SIZE + STAR6E_FEC_HDR + 2 + maxlen;
		output->fec_credit += (int64_t)src_bytes;
		r = (unsigned)(output->fec_credit /
			(int64_t)(VENC_FEC_PARITY_DIV * plen));
		if (r > k) /* more is near-copies; also keeps k + idx <= 255 */
			r = k;
		output->fec_credit -= (int64_t)(r * VENC_FEC_PARITY_DIV * plen);
		if (!r)
			__atomic_fetch_add(&output->fec_unprotected, k,
				__ATOMIC_RELAXED);

		par[0] = 0x7E; /* H.265 NAL type 63: decoders ignore it */
		par[1] = (uint8_t)((b == nb - 1 && output->fec_frame == 1 ?
			0x20 : 0) | 0x01);
		par[2] = (uint8_t)k;
		star6e_write_be16(par + 4, (uint16_t)(output->fec_base_seq + first));
		for (i = 0; i < r; i++) {
			par[3] = (uint8_t)i;
			rs_fec_encode(k, i, src, STAR6E_FEC_SYM_MAX, 2 + maxlen,
				par + STAR6E_FEC_HDR);
			(void)rtp_packetizer_send_packet(&output->fec_rtp,
				star6e_fec_write, output, par,
				STAR6E_FEC_HDR + 2 + maxlen, NULL, 0, 0);
		}
		first += k;
	}
}

int star6e_output_end_frame(Star6eOutput *output)
{
	int sent;

	if (!output || !output->batch.active)
		return 0;
	/* Video first, so the frame never waits on its parity encode; none if
	 * that flush gave up (budget or socket full).  fec_count is 0 unless
	 * fec_frame latched on.  Parity is counted as it reaches the socket. */
	sent = star6e_batch_flush(output);
	if (output->fec_count && !output->batch.discard_remaining) {
		const uint32_t w0 = __atomic_load_n(&output->socket_writes,
			__ATOMIC_RELAXED);

		star6e_fec_emit(output);
		sent += star6e_batch_flush(output);
		__atomic_fetch_add(&output->fec_parity_pkts,
			__atomic_load_n(&output->socket_writes, __ATOMIC_RELAXED) - w0,
			__ATOMIC_RELAXED);
	}
	output->batch.active = 0;
	return sent;
}

/* Queue one RTP packet into the active batch. Returns 0 on success,
 * -1 if the packet cannot fit the scratch slot (caller falls back to
 * immediate send).
 *
 * Both the header and payload1 are copied into scratch because both live
 * on the caller's stack and are reused between packets (rtp header is
 * built in rtp_packetizer_send_packet; payload1 is either a 3-byte FU-A
 * header or a single NAL unit). payload2, when present, is a slice of the
 * VENC stream buffer
 * which remains valid until MI_VENC_ReleaseStream is called in the
 * pipeline after end_frame(), so we keep it as a zero-copy iovec. */
static int star6e_batch_enqueue(Star6eOutput *output,
	const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len)
{
	Star6eOutputBatch *b = &output->batch;
	size_t slot;
	size_t scratch_len = header_len + payload1_len;
	struct iovec *iov;
	struct msghdr *hdr;

	if (scratch_len > STAR6E_OUTPUT_BATCH_SLOT_SCRATCH)
		return -1;
	if (b->discard_remaining) {
		if (b->discard_as_error)
			output->send_errors++;
		else
			__atomic_fetch_add(&output->socket_drops, 1,
				__ATOMIC_RELAXED);
		return 0;
	}

	if (b->count >= STAR6E_OUTPUT_BATCH_MAX) {
		star6e_batch_flush(output);
		if (b->discard_remaining) {
			if (b->discard_as_error)
				output->send_errors++;
			else
				__atomic_fetch_add(&output->socket_drops, 1,
					__ATOMIC_RELAXED);
			return 0;
		}
	}

	slot = b->count;
	iov = &b->iov[slot * 2];
	hdr = &b->msgs[slot].msg_hdr;

	/* Copy header + payload1 into owned scratch so the caller can reuse
	 * both stack buffers for the next packet before we flush. */
	memcpy(b->scratch[slot], header, header_len);
	memcpy(b->scratch[slot] + header_len, payload1, payload1_len);
	iov[0].iov_base = b->scratch[slot];
	iov[0].iov_len = scratch_len;

	if (payload2 && payload2_len > 0) {
		iov[1].iov_base = (void *)payload2;
		iov[1].iov_len = payload2_len;
	}

	memset(hdr, 0, sizeof(*hdr));
	/* Use the transport snapshot taken at begin_frame(), not the live
	 * output fields — those may be mutated by apply_server() on the
	 * HTTP thread. */
	if (b->connected_udp) {
		hdr->msg_name = NULL;
		hdr->msg_namelen = 0;
	} else {
		hdr->msg_name = (void *)&b->dst;
		hdr->msg_namelen = b->dst_len;
	}
	hdr->msg_iov = iov;
	hdr->msg_iovlen = (payload2 && payload2_len > 0) ? 2 : 1;
	b->msgs[slot].msg_len = 0;

	b->count++;
	return 0;
}

int star6e_output_send_rtp_parts(Star6eOutput *output,
	const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len)
{
	if (star6e_output_send_parts(output, header, header_len,
	    payload1, payload1_len, payload2, payload2_len) != 0)
		return -1;
	if (output->fec_frame)
		star6e_fec_capture(output, header, payload1, payload1_len,
			payload2, payload2_len);
	return 0;
}

static int star6e_output_send_parts(Star6eOutput *output,
	const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len)
{
	if (!output || !header || !payload1 || header_len == 0 || payload1_len == 0)
		return -1;

	if (output->ring) {
		size_t total_payload = payload1_len + payload2_len;
		if (header_len > UINT16_MAX || total_payload > UINT16_MAX)
			return -1;
		return venc_ring_write3(output->ring,
			header, (uint16_t)header_len,
			payload1, (uint16_t)payload1_len,
			payload2, (uint16_t)payload2_len);
	}

	if (output->batch.active) {
		if (star6e_batch_enqueue(output, header, header_len,
		    payload1, payload1_len, payload2, payload2_len) == 0)
			return 0;
		/* Scratch slot too small for this packet — flush anything
		 * we already have (to preserve ordering), then fall through
		 * to immediate send for this one packet. */
		star6e_batch_flush(output);
	}

	/* Fallback: either batching inactive (probe/audio/compact paths) or
	 * a packet too big for scratch — send immediately. */
	if (output_socket_send_parts(output->socket_handle, &output->dst,
	    output->dst_len, output->connected_udp,
	    header, header_len, payload1, payload1_len,
	    payload2, payload2_len) != 0) {
		star6e_account_send_failure(output, output->socket_handle, 1);
		return -1;
	}
	__atomic_fetch_add(&output->socket_writes, 1, __ATOMIC_RELAXED);
	return 0;
}

int star6e_output_send_compact_packet(Star6eOutput *output,
	const uint8_t *packet, uint32_t packet_size, uint32_t max_size)
{
	uint32_t payload_offset = STAR6E_RTP_HEADER_SIZE;
	uint32_t payload_size;
	const uint8_t *payload;
	uint32_t offset = 0;
	uint8_t marker;
	uint16_t sequence;
	uint32_t timestamp;
	uint32_t ssrc_id;
	uint32_t max_fragment;

	if (!output || output->socket_handle < 0 ||
	    output->transport == VENC_OUTPUT_URI_SHM ||
	    !packet || packet_size == 0) {
		return -1;
	}

	if (packet_size <= max_size) {
		ssize_t sent = sendto(output->socket_handle, packet, packet_size, 0,
			(const struct sockaddr *)&output->dst, output->dst_len);

		if (sent < 0) {
			star6e_account_send_failure(output,
				output->socket_handle, 1);
			return -1;
		}
		__atomic_fetch_add(&output->socket_writes, 1,
			__ATOMIC_RELAXED);
		return 0;
	}

	if (max_size <= payload_offset || packet_size <= payload_offset)
		return 0;

	payload_size = packet_size - payload_offset;
	payload = packet + payload_offset;
	marker = packet[1] & 0x80;
	sequence = star6e_read_be16(packet + 2);
	timestamp = star6e_read_be32(packet + 4);
	ssrc_id = star6e_read_be32(packet + 8);
	max_fragment = max_size - payload_offset;

	while (offset < payload_size) {
		uint8_t fragment_header[STAR6E_RTP_HEADER_SIZE];
		struct iovec vec[2];
		struct msghdr msg;
		uint32_t fragment_size = payload_size - offset;

		if (fragment_size > max_fragment)
			fragment_size = max_fragment;

		memcpy(fragment_header, packet, STAR6E_RTP_HEADER_SIZE);
		fragment_header[1] = (uint8_t)((packet[1] & 0x7f) |
			((offset + fragment_size >= payload_size) ? marker : 0));
		star6e_write_be16(fragment_header + 2, sequence++);
		star6e_write_be32(fragment_header + 4, timestamp);
		star6e_write_be32(fragment_header + 8, ssrc_id);

		vec[0].iov_base = fragment_header;
		vec[0].iov_len = sizeof(fragment_header);
		vec[1].iov_base = (void *)(payload + offset);
		vec[1].iov_len = fragment_size;

		memset(&msg, 0, sizeof(msg));
		msg.msg_name = (void *)&output->dst;
		msg.msg_namelen = output->dst_len;
		msg.msg_iov = vec;
		msg.msg_iovlen = 2;
		if (sendmsg(output->socket_handle, &msg, 0) < 0) {
			star6e_account_send_failure(output,
				output->socket_handle, 1);
			return -1;
		}
		__atomic_fetch_add(&output->socket_writes, 1,
			__ATOMIC_RELAXED);
		offset += fragment_size;
	}

	return 0;
}

size_t star6e_output_send_compact_frame(Star6eOutput *output,
	const MI_VENC_Stream_t *stream, uint32_t max_size)
{
	size_t total_bytes = 0;
	unsigned int i;

	if (!output || !stream)
		return 0;

	for (i = 0; i < stream->count; ++i) {
		const MI_VENC_Pack_t *pack = &stream->packet[i];

		if (!pack->data)
			continue;

		if (pack->packNum > 0) {
			unsigned int nal_count = (unsigned int)pack->packNum;
			unsigned int k;

			for (k = 0; k < nal_count; ++k) {
				MI_U32 length = pack->packetInfo[k].length;
				MI_U32 offset = pack->packetInfo[k].offset;

				if (length == 0 || offset >= pack->length ||
				    length > (pack->length - offset)) {
					continue;
				}

				total_bytes += length;
				(void)star6e_output_send_compact_packet(output,
					pack->data + offset, length, max_size);
			}
			continue;
		}

		if (pack->length > pack->offset) {
			MI_U32 length = pack->length - pack->offset;

			total_bytes += length;
			(void)star6e_output_send_compact_packet(output,
				pack->data + pack->offset, length, max_size);
		}
	}

	return total_bytes;
}

static size_t star6e_output_send_frame_ring(Star6eOutput *output,
	const MI_VENC_Stream_t *stream)
{
	VencFrameMeta meta;
	size_t total_bytes = 0;
	unsigned int i;
	int is_idr = 0;

	if (!output || !output->frame_ring || !stream)
		return 0;

	/* IDR detection from packType.h265Nalu (types 19, 20) */
	for (i = 0; i < stream->count && !is_idr; ++i) {
		const MI_VENC_Pack_t *pack = &stream->packet[i];
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
	    stream->h265Info.refType == STAR6E_REFTYPE_ENHANCE_P_NOTFORREF)
		meta.flags |= VENC_FRAME_FLAG_ENHANCE;

	if (venc_frame_ring_begin_write(output->frame_ring, &meta) != 0)
		return 0;

	for (i = 0; i < stream->count; ++i) {
		const MI_VENC_Pack_t *pack = &stream->packet[i];

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

				if (venc_frame_ring_append(output->frame_ring,
				    pack->data + offset, length) != 0) {
					star6e_output_abort_oversize(output);
					return 0;
				}
				total_bytes += length;
			}
			continue;
		}

		if (pack->length > pack->offset) {
			MI_U32 length = pack->length - pack->offset;

			if (venc_frame_ring_append(output->frame_ring,
			    pack->data + pack->offset, length) != 0) {
				star6e_output_abort_oversize(output);
				return 0;
			}
			total_bytes += length;
		}
	}

	venc_frame_ring_commit_write(output->frame_ring);
	return total_bytes;
}

size_t star6e_output_send_frame(Star6eOutput *output,
	const MI_VENC_Stream_t *stream, uint32_t max_size,
	Star6eOutputRtpSendFn rtp_send, void *opaque)
{
	size_t total;

	if (!output || !stream)
		return 0;
	if (star6e_output_reject_incomplete_access_unit(output, stream))
		return 0;

	if (output->frame_ring)
		return star6e_output_send_frame_ring(output, stream);

	if (star6e_output_is_rtp(output)) {
		if (!rtp_send)
			return 0;
		star6e_output_begin_frame(output);
		total = rtp_send(output, stream, opaque);
		star6e_output_end_frame(output);
		return total;
	}

	return star6e_output_send_compact_frame(output, stream, max_size);
}

int star6e_output_apply_server(Star6eOutput *output, const char *uri)
{
	VencOutputUri parsed;

	if (!output || !uri)
		return -1;
	if (output->ring) {
		fprintf(stderr, "ERROR: live switch away from shm:// is not supported "
			"(requires restart)\n");
		return -1;
	}
	if (output->frame_ring) {
		fprintf(stderr, "ERROR: live switch away from frame-shm:// is not "
			"supported (requires restart)\n");
		return -1;
	}
	if (venc_config_parse_output_uri(uri, &parsed) != 0)
		return -1;
	if (parsed.type == VENC_OUTPUT_URI_SHM ||
	    parsed.type == VENC_OUTPUT_URI_FRAME_SHM) {
		fprintf(stderr, "ERROR: live switch to shm:// or frame-shm:// "
			"is not supported\n");
		return -1;
	}

	__atomic_fetch_add(&output->transport_gen, 1, __ATOMIC_RELEASE); /* odd = writing */
	if (output_socket_configure(&output->socket_handle, &output->dst,
	    &output->dst_len, &output->transport, &parsed,
	    output->requested_connected_udp, output->allow_unix_encoder_stall,
	    &output->connected_udp) != 0) {
		__atomic_fetch_add(&output->transport_gen, 1, __ATOMIC_RELEASE); /* restore even */
		return -1;
	}
	(void)output_socket_capture_capacity(output->socket_handle,
		&output->send_queue);
	__atomic_fetch_add(&output->transport_gen, 1, __ATOMIC_RELEASE); /* even = stable */
	return 0;
}

void star6e_output_teardown(Star6eOutput *output)
{
	if (!output)
		return;

	if (output->ring) {
		venc_ring_destroy(output->ring);
		output->ring = NULL;
	}
	if (output->frame_ring) {
		venc_frame_ring_destroy(output->frame_ring);
		output->frame_ring = NULL;
	}
	if (output->socket_handle >= 0) {
		close(output->socket_handle);
		output->socket_handle = -1;
	}
	output->fec_on = 0;
	output->fec_frame = 0;
	free(output->fec_arena);
	output->fec_arena = NULL;

	memset(&output->dst, 0, sizeof(output->dst));
	output->dst_len = 0;
	output->connected_udp = 0;
	output->requested_connected_udp = 0;
	output->allow_unix_encoder_stall = 0;
	output->transport = VENC_OUTPUT_URI_UDP;
}

void star6e_audio_output_reset(Star6eAudioOutput *audio_output)
{
	if (!audio_output)
		return;

	memset(audio_output, 0, sizeof(*audio_output));
	audio_output->socket_handle = -1;
}

int star6e_audio_output_init(Star6eAudioOutput *audio_output,
	const Star6eOutput *video_output, uint16_t port_override,
	uint16_t max_payload_size)
{
	if (!audio_output)
		return -1;

	star6e_audio_output_reset(audio_output);
	if (!video_output)
		return -1;

	audio_output->video_output = video_output;
	audio_output->port_override = port_override;
	audio_output->max_payload_size = max_payload_size;
	if (port_override == 0) {
		return 0;
	}

	audio_output->socket_handle = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (audio_output->socket_handle < 0) {
		fprintf(stderr, "[audio] ERROR: cannot create audio UDP socket\n");
		return -1;
	}

	if (output_socket_fill_udp_destination("127.0.0.1", port_override,
	    &audio_output->fallback_dst, &audio_output->fallback_dst_len) != 0) {
		close(audio_output->socket_handle);
		audio_output->socket_handle = -1;
		return -1;
	}

	return 0;
}

uint16_t star6e_audio_output_port(const Star6eAudioOutput *audio_output)
{
	const struct sockaddr_in *dst;

	if (!audio_output)
		return 0;
	if (audio_output->port_override != 0)
		return audio_output->port_override;
	if (!audio_output->video_output ||
	    audio_output->video_output->transport != VENC_OUTPUT_URI_UDP ||
	    audio_output->video_output->dst_len != sizeof(*dst)) {
		return 0;
	}

	dst = (const struct sockaddr_in *)&audio_output->video_output->dst;
	return ntohs(dst->sin_port);
}

int star6e_audio_output_send_rtp(Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len, RtpPacketizerState *rtp_state,
	uint32_t frame_ticks)
{
	Star6eAudioSendTarget target;

	if (!audio_output || !data || len == 0 || !rtp_state)
		return -1;
	if (resolve_cached_audio_target(audio_output, &target) != 0)
		return -1;

	if (rtp_packetizer_send_packet(rtp_state, send_audio_rtp, &target,
	    data, len, NULL, 0, 0) != 0) {
		return -1;
	}

	rtp_state->timestamp += frame_ticks;
	return 0;
}

int star6e_audio_output_send_compact(Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len)
{
	Star6eAudioSendTarget target;
	uint16_t max_data;
	size_t offset = 0;

	if (!audio_output || !data || len == 0 ||
	    resolve_cached_audio_target(audio_output, &target) != 0) {
		return -1;
	}

	max_data = audio_output->max_payload_size > 4 ?
		(uint16_t)(audio_output->max_payload_size - 4) : 0;
	if (max_data == 0)
		return 0;

	while (offset < len) {
		uint8_t hdr[4];
		struct iovec vec[2];
		struct msghdr msg;
		size_t chunk = len - offset;

		if (chunk > max_data)
			chunk = max_data;

		hdr[0] = 0xAA;
		hdr[1] = 0x01;
		hdr[2] = (uint8_t)((chunk >> 8) & 0xFF);
		hdr[3] = (uint8_t)(chunk & 0xFF);

		vec[0].iov_base = hdr;
		vec[0].iov_len = sizeof(hdr);
		vec[1].iov_base = (void *)(data + offset);
		vec[1].iov_len = chunk;

		memset(&msg, 0, sizeof(msg));
		msg.msg_name = (void *)&target.dst;
		msg.msg_namelen = target.dst_len;
		msg.msg_iov = vec;
		msg.msg_iovlen = 2;
		if (sendmsg(target.socket_handle, &msg, 0) < 0)
			return -1;
		offset += chunk;
	}

	return 0;
}

int star6e_audio_output_send(Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len, RtpPacketizerState *rtp_state,
	uint32_t frame_ticks)
{
	if (audio_output && audio_output->video_output &&
	    star6e_output_is_rtp(audio_output->video_output)) {
		return star6e_audio_output_send_rtp(audio_output, data, len,
			rtp_state, frame_ticks);
	}

	return star6e_audio_output_send_compact(audio_output, data, len);
}

void star6e_audio_output_teardown(Star6eAudioOutput *audio_output)
{
	if (!audio_output)
		return;

	if (audio_output->socket_handle >= 0 &&
	    audio_output->port_override != 0) {
		close(audio_output->socket_handle);
	}
	star6e_audio_output_reset(audio_output);
}
