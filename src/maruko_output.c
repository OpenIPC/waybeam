#include "maruko_output.h"

#include "output_socket.h"
#include "timing.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void maruko_output_account_send_failure(MarukoOutput *output, int socket_handle,
	uint32_t packets)
{
	if (!output)
		return;
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
		output_socket_note_saturation(socket_handle, &output->send_queue);
		__atomic_fetch_add(&output->socket_drops, packets,
			__ATOMIC_RELAXED);
	} else {
		output->send_errors += packets;
	}
}

int maruko_output_init(MarukoOutput *output, const VencOutputUri *uri,
	int requested_connected_udp, int allow_unix_encoder_stall)
{
	if (!output)
		return -1;
	if (!uri || uri->type == VENC_OUTPUT_URI_SHM)
		return -1;

	output->socket_handle = -1;
	output->ring = NULL;
	output->dst_len = 0;
	output->transport = VENC_OUTPUT_URI_UDP;
	memset(&output->dst, 0, sizeof(output->dst));
	output->requested_connected_udp = requested_connected_udp ? 1 : 0;
	output->connected_udp = 0;
	output->allow_unix_encoder_stall = allow_unix_encoder_stall ? 1 : 0;
	output->send_errors = 0;
	output->trunc_warned = 0;
	output->oversize_warned = 0;
	output->bad_au_drops = 0;
	/* The window state too, not just the published scalar: a stale
	 * low_water_ready would skip the reset on the next ring and let a
	 * carried-over low_slots of 0 swallow the new ring's first sample,
	 * publishing "perfectly drained" over a ring that is full. */
	output->low_water_ready = 0;
	memset(&output->low_water, 0, sizeof(output->low_water));
	memset(&output->send_queue, 0, sizeof(output->send_queue));
	__atomic_store_n(&output->socket_drops, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&output->socket_writes, 0, __ATOMIC_RELAXED);
	memset(&output->batch, 0, sizeof(output->batch));
	output->batch.socket_handle = -1;

	if (output_socket_configure(&output->socket_handle, &output->dst,
	    &output->dst_len, &output->transport, uri,
	    output->requested_connected_udp, output->allow_unix_encoder_stall,
	    &output->connected_udp) != 0)
		return -1;
	(void)output_socket_capture_capacity(output->socket_handle,
		&output->send_queue);
	__atomic_fetch_add(&output->transport_gen, 2, __ATOMIC_RELEASE);
	return 0;
}

int maruko_output_init_shm(MarukoOutput *output, const char *shm_name)
{
	uint32_t slot_data;

	if (!output || !shm_name || !shm_name[0])
		return -1;

	output->socket_handle = -1;
	output->ring = NULL;
	output->dst_len = 0;
	output->transport = VENC_OUTPUT_URI_UDP;
	memset(&output->dst, 0, sizeof(output->dst));
	output->requested_connected_udp = 0;
	output->connected_udp = 0;
	output->allow_unix_encoder_stall = 0;
	output->send_errors = 0;
	output->trunc_warned = 0;
	output->oversize_warned = 0;
	output->bad_au_drops = 0;
	/* The window state too, not just the published scalar: a stale
	 * low_water_ready would skip the reset on the next ring and let a
	 * carried-over low_slots of 0 swallow the new ring's first sample,
	 * publishing "perfectly drained" over a ring that is full. */
	output->low_water_ready = 0;
	memset(&output->low_water, 0, sizeof(output->low_water));
	memset(&output->send_queue, 0, sizeof(output->send_queue));
	__atomic_store_n(&output->socket_drops, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&output->socket_writes, 0, __ATOMIC_RELAXED);
	memset(&output->batch, 0, sizeof(output->batch));
	output->batch.socket_handle = -1;

	/* Slot fits the validated payload ceiling so any value in
	 * [VENC_OUTPUT_PAYLOAD_MIN_BYTES, VENC_OUTPUT_PAYLOAD_CEILING_BYTES]
	 * applies live without restart, matching UDP/unix:// behavior. */
	slot_data = (uint32_t)VENC_OUTPUT_PAYLOAD_CEILING_BYTES + 12;
	output->ring = venc_ring_create(shm_name, 512, slot_data);
	if (!output->ring) {
		fprintf(stderr, "ERROR: [maruko] venc_ring_create(%s) failed\n",
			shm_name);
		return -1;
	}

	printf("> [maruko] SHM output: %s (slot_data=%u)\n", shm_name,
		slot_data);
	__atomic_fetch_add(&output->transport_gen, 2, __ATOMIC_RELEASE);
	return 0;
}

int maruko_output_init_frame_shm(MarukoOutput *output, const char *shm_name)
{
	if (!output || !shm_name || !shm_name[0])
		return -1;

	output->socket_handle = -1;
	output->ring = NULL;
	output->frame_ring = NULL;
	output->dst_len = 0;
	output->transport = VENC_OUTPUT_URI_FRAME_SHM;
	memset(&output->dst, 0, sizeof(output->dst));
	output->requested_connected_udp = 0;
	output->connected_udp = 0;
	output->allow_unix_encoder_stall = 0;
	output->send_errors = 0;
	output->trunc_warned = 0;
	output->oversize_warned = 0;
	output->bad_au_drops = 0;
	/* The window state too, not just the published scalar: a stale
	 * low_water_ready would skip the reset on the next ring and let a
	 * carried-over low_slots of 0 swallow the new ring's first sample,
	 * publishing "perfectly drained" over a ring that is full. */
	output->low_water_ready = 0;
	memset(&output->low_water, 0, sizeof(output->low_water));
	memset(&output->send_queue, 0, sizeof(output->send_queue));
	__atomic_store_n(&output->socket_drops, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&output->socket_writes, 0, __ATOMIC_RELAXED);
	memset(&output->batch, 0, sizeof(output->batch));
	output->batch.socket_handle = -1;

	output->frame_ring = venc_frame_ring_create(shm_name, 8, 384 * 1024);
	if (!output->frame_ring) {
		fprintf(stderr, "ERROR: [maruko] venc_frame_ring_create(%s) failed\n",
			shm_name);
		return -1;
	}

	printf("> [maruko] Frame-SHM output: %s (384 KB slots)\n", shm_name);
	__atomic_fetch_add(&output->transport_gen, 2, __ATOMIC_RELEASE);
	return 0;
}

int maruko_output_frame_ring_fill(
	const MarukoOutput *output, venc_frame_ring_fill_t *out)
{
	if (!output || !output->frame_ring || !out)
		return -1;
	return venc_frame_ring_get_fill(output->frame_ring, out);
}

void maruko_output_observe_pressure(MarukoOutput *output)
{
	uint8_t fill_pct = 0;
	uint32_t full_drops = 0;
	uint32_t writes = 0;
	uint32_t oversize_drops = 0;
	int have_fill = 0;

	if (!output)
		return;

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

int maruko_output_apply_server(MarukoOutput *output, const char *uri)
{
	VencOutputUri parsed;

	if (!output || !uri)
		return -1;

	/* SHM output doesn't support live server change */
	if (output->ring) {
		fprintf(stderr, "ERROR: [maruko] cannot change server in SHM mode\n");
		return -1;
	}
	if (output->frame_ring) {
		fprintf(stderr, "ERROR: [maruko] cannot change server in "
			"frame-SHM mode\n");
		return -1;
	}

	if (venc_config_parse_output_uri(uri, &parsed) != 0)
		return -1;
	if (parsed.type == VENC_OUTPUT_URI_SHM ||
	    parsed.type == VENC_OUTPUT_URI_FRAME_SHM) {
		fprintf(stderr, "ERROR: [maruko] cannot change server to "
			"shm:// or frame-shm:// live\n");
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

uint32_t maruko_output_drain_send_errors(MarukoOutput *output)
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
 * from the first unsent message.  Congestion (EAGAIN / short write) is
 * accounted into output->socket_drops and bounded by a per-frame deadline;
 * real errors go to output->send_errors.  Mirror of star6e_batch_flush —
 * see the rationale there.
 *
 * Returns number of messages successfully sent. Always resets
 * batch->count to 0. */
static int maruko_batch_flush(MarukoOutput *output)
{
	MarukoOutputBatch *b = &output->batch;
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
			output->send_errors +=
				(uint32_t)(b->count - sent_total);
			b->discard_remaining = 1;
			b->discard_as_error = 1;
			break;
		}
		if (n == 0) {
			output->send_errors +=
				(uint32_t)(b->count - sent_total);
			b->discard_remaining = 1;
			b->discard_as_error = 1;
			break;
		}
		sent_total += (size_t)n;
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

int maruko_output_set_fec(MarukoOutput *output, int on)
{
	if (!output)
		return -1;
	if (on && !output->fec.arena && (output->ring || output->frame_ring ||
	    (output->transport != VENC_OUTPUT_URI_UDP &&
	     output->transport != VENC_OUTPUT_URI_UNIX)))
		return -1;
	return venc_fec_set(&output->fec, on);
}

/* Video datagrams this output has failed to send, by any cause. */
static uint32_t maruko_output_lost(const MarukoOutput *output)
{
	return __atomic_load_n(&output->socket_drops, __ATOMIC_RELAXED) +
		output->send_errors;
}

void maruko_output_begin_frame(MarukoOutput *output)
{
	MarukoOutputBatch *b;
	uint32_t gen_before, gen_after;

	if (!output)
		return;
	b = &output->batch;
	b->count = 0;
	b->active = 0;
	b->flush_budget_us = MARUKO_OUTPUT_FLUSH_BUDGET_US;
	b->discard_remaining = 0;
	b->discard_as_error = 0;
	venc_fec_begin_frame(&output->fec);
	output->fec_lost0 = maruko_output_lost(output);

	/* SHM output is not batched — skip the snapshot entirely. */
	if (output->ring)
		return;

	/* Seqlock read of transport state: retry while apply_server() holds
	 * an odd generation. Matches the writer pattern in
	 * maruko_output_apply_server. */
	for (;;) {
		gen_before = __atomic_load_n(&output->transport_gen,
			__ATOMIC_ACQUIRE);
		if (gen_before & 1u) {
			/* Writer in progress — yield rather than spin; see
			 * star6e_output_begin_frame for the rationale. */
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

int maruko_output_flush(MarukoOutput *output)
{
	return output && output->batch.active ? maruko_batch_flush(output) : 0;
}

static int maruko_fec_write(const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	return maruko_output_batch_enqueue(opaque, header, header_len,
		payload1, payload1_len, payload2, payload2_len);
}

/* Video first, so the frame never waits on its parity encode (Star6E's
 * star6e_output_end_frame).  No parity once the frame's budget is spent or
 * the socket gave up; the frame counts as unprotected only if its video all
 * went out, since a refused packet is already a transport drop.  fec.count
 * is 0 unless fec.frame latched on.  Parity is counted as it reaches the
 * socket. */
int maruko_output_end_frame(MarukoOutput *output)
{
	int sent;

	if (!output || !output->batch.active)
		return 0;
	sent = maruko_batch_flush(output);
	if (output->fec.count && output->batch.discard_remaining) {
		if (maruko_output_lost(output) == output->fec_lost0)
			__atomic_fetch_add(&output->fec.unprotected,
				output->fec.count, __ATOMIC_RELAXED);
	} else if (output->fec.count) {
		const uint32_t w0 = __atomic_load_n(&output->socket_writes,
			__ATOMIC_RELAXED);

		venc_fec_emit(&output->fec, maruko_fec_write, output);
		sent += maruko_batch_flush(output);
		__atomic_fetch_add(&output->fec.parity_pkts,
			__atomic_load_n(&output->socket_writes, __ATOMIC_RELAXED) - w0,
			__ATOMIC_RELAXED);
	}
	output->batch.active = 0;
	return sent;
}

int maruko_output_batch_enqueue(MarukoOutput *output,
	const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len)
{
	MarukoOutputBatch *b;
	size_t slot;
	size_t scratch_len;
	struct iovec *iov;
	struct msghdr *hdr;

	if (!output || !output->batch.active)
		return -1;
	if (!header || !payload1 || header_len == 0 || payload1_len == 0)
		return -1;

	b = &output->batch;
	scratch_len = header_len + payload1_len;
	if (scratch_len > MARUKO_OUTPUT_BATCH_SLOT_SCRATCH)
		return -1;
	if (b->discard_remaining) {
		if (b->discard_as_error)
			output->send_errors++;
		else
			__atomic_fetch_add(&output->socket_drops, 1,
				__ATOMIC_RELAXED);
		return 0;
	}

	if (b->count >= MARUKO_OUTPUT_BATCH_MAX) {
		maruko_batch_flush(output);
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

void maruko_output_teardown(MarukoOutput *output)
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
	memset(&output->dst, 0, sizeof(output->dst));
	output->dst_len = 0;
	output->transport = VENC_OUTPUT_URI_UDP;
	output->allow_unix_encoder_stall = 0;
	output->batch.active = 0;
	output->batch.count = 0;
	output->batch.socket_handle = -1;
	venc_fec_free(&output->fec);
}
