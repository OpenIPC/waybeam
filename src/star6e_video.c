#include "star6e_video.h"
#include "star6e_audio.h"

#include "output_socket.h"
#include "rtp_session.h"
#include "timing.h"

#include <stdio.h>
#include <string.h>

typedef struct {
	RtpPacketizerState *rtp;
	uint32_t frame_ticks;
	H26xParamSets *params;
	size_t max_payload;
	Star6eHevcRtpStats *stats;
} Star6eRtpFrameContext;

static size_t send_frame_output_rtp(Star6eOutput *output,
	const MI_VENC_Stream_t *stream, void *opaque)
{
	Star6eRtpFrameContext *ctx = opaque;

	if (!ctx)
		return 0;

	return star6e_hevc_rtp_send_frame(stream, output, ctx->rtp,
		ctx->frame_ticks, ctx->params, ctx->max_payload, ctx->stats);
}

void star6e_video_reset(Star6eVideoState *state)
{
	if (!state)
		return;

	rtp_sidecar_sender_close(&state->sidecar);
	memset(state, 0, sizeof(*state));
}

void star6e_video_init(Star6eVideoState *state, const VencConfig *vcfg,
	uint32_t sensor_framerate, const Star6eOutput *output)
{
	if (!state || !vcfg)
		return;

	/* Defensive: callers normally go through star6e_video_reset first,
	 * but the dual-stream init path at star6e_runtime.c calls us on a
	 * struct that may already hold an open sidecar fd from a previous
	 * lifecycle.  Closing here is a no-op on a freshly-zeroed struct
	 * (sender_close skips fd <= 0) and avoids leaking the previous fd
	 * across the upcoming memset. */
	rtp_sidecar_sender_close(&state->sidecar);
	memset(state, 0, sizeof(*state));
	state->sensor_framerate = sensor_framerate;
	state->max_frame_size = vcfg->outgoing.max_payload_size;
	state->rtp_payload_size = vcfg->outgoing.max_payload_size;

	if (output && star6e_output_is_rtp(output)) {
		RtpSessionState session;

		rtp_session_init(&session, rtp_session_payload_type(PT_H265),
			sensor_framerate);
		state->rtp_state.seq = session.seq;
		state->rtp_state.timestamp = session.timestamp;
		state->rtp_state.ssrc = session.ssrc;
		state->rtp_state.payload_type = session.payload_type;
		state->rtp_frame_ticks = session.frame_ticks;
	}

	if (vcfg->system.verbose) {
		struct timespec now;

		clock_gettime(CLOCK_MONOTONIC, &now);
		stream_metrics_start(&state->verbose_metrics, &now);
	}

	rtp_sidecar_sender_init(&state->sidecar, vcfg->outgoing.sidecar_port);
}

size_t star6e_video_send_frame(Star6eVideoState *state,
	Star6eOutput *output, const MI_VENC_Stream_t *stream,
	int output_enabled, int verbose_enabled,
	const RtpSidecarEncInfo *enc_info,
	const RtpSidecarAttitudeInfo *att_info,
	const void *detect_blob, uint16_t detect_len)
{
	size_t total_bytes = 0;
	Star6eHevcRtpStats frame_packetizer = {0};

	if (!state || !output || !stream)
		return 0;

	state->frame_counter++;

	if (output_enabled) {
		const uint16_t payload =
			__atomic_load_n(&output->fec_on, __ATOMIC_RELAXED) ?
			venc_fec_payload_size(state->rtp_payload_size,
				state->slice_avg) : state->rtp_payload_size;
		Star6eRtpFrameContext rtp_frame = {
			.rtp = &state->rtp_state,
			.frame_ticks = state->rtp_frame_ticks,
			.params = &state->param_sets,
			.max_payload = payload,
			.stats = &frame_packetizer, /* slice sizes, every frame */
		};

		/* Sidecar work — poll, timestamp snapshot, send — is only
		 * meaningful when the sidecar socket is configured.  Gating
		 * on sidecar.fd >= 0 avoids one recvfrom syscall, one
		 * wb_monotonic_us() read, and the sidecar send argument setup
		 * per frame when the sidecar feature is disabled.  Mirrors
		 * the Maruko gate from PR #37. */
		int sidecar_active = (state->sidecar.fd >= 0);

		uint32_t frame_rtp_ts = 0;
		uint16_t seq_before = 0;
		uint64_t ready_us = 0;
		uint64_t capture_us = 0;

		if (sidecar_active) {
			rtp_sidecar_poll(&state->sidecar);
			frame_rtp_ts = state->rtp_state.timestamp;
			seq_before = state->rtp_state.seq;
			ready_us = wb_monotonic_us();
			capture_us = (stream->count > 0 && stream->packet)
				? stream->packet[0].timestamp : 0;
		}

		total_bytes = star6e_output_send_frame(output, stream,
			state->max_frame_size, send_frame_output_rtp, &rtp_frame);
		if (frame_packetizer.rtp_packets) /* not frame-shm, compact, dropped AU */
			__atomic_store_n(&state->payload_used, payload,
				__ATOMIC_RELAXED);
		if (frame_packetizer.slices) {
			const int32_t mean = (int32_t)(frame_packetizer.slice_bytes /
				frame_packetizer.slices);

			/* EWMA 1/8: follows the scene, not one frame. */
			state->slice_avg = state->slice_avg ?
				(uint32_t)((int32_t)state->slice_avg +
				(mean - (int32_t)state->slice_avg) / 8) :
				(uint32_t)mean;
		}

		if (sidecar_active) {
			RtpSidecarTransportInfo tinfo;
			const RtpSidecarTransportInfo *tinfo_ptr = NULL;

			/* Producer thread already populated output->last_*
			 * fields and the in_pressure / pressure_drops state
			 * inside star6e_output_observe_pressure() at the top
			 * of the frame.  Read those instead of re-querying —
			 * one SIOCOUTQ ioctl / ring-fill load per frame
			 * instead of two.  Trailer is only emitted when the
			 * runtime decided to observe (i.e. a probe is
			 * subscribed); same thread, no atomic load needed. */
			if (output->ring || output->frame_ring ||
			    ((output->transport == VENC_OUTPUT_URI_UNIX ||
			      output->transport == VENC_OUTPUT_URI_UDP) &&
			     output->socket_handle >= 0)) {
				memset(&tinfo, 0, sizeof(tinfo));
				tinfo.fill_pct = output->last_fill_pct;
				tinfo.in_pressure = output->in_pressure ? 1 : 0;
				tinfo.pressure_drops = output->pressure_drops;
				/* For socket transports these carry the
				 * congestion drops / successful sends counted
				 * in the batch flush; for the SHM rings they
				 * carry the ring's lifetime counters. */
				tinfo.transport_drops = output->last_full_drops;
				tinfo.packets_sent = output->last_writes;
				tinfo_ptr = &tinfo;
			}

			rtp_sidecar_send_frame_detect(&state->sidecar,
				state->rtp_state.ssrc, frame_rtp_ts,
				seq_before,
				(uint16_t)(state->rtp_state.seq - seq_before),
				capture_us, ready_us, enc_info, tinfo_ptr,
				att_info, detect_blob, detect_len);
		}
	}

	if (!verbose_enabled)
		return total_bytes;

	stream_metrics_record_frame(&state->verbose_metrics, total_bytes);
	if (star6e_output_is_rtp(output)) {
		state->verbose_packetizer_interval.total_nals += frame_packetizer.total_nals;
		state->verbose_packetizer_interval.single_packets += frame_packetizer.single_packets;
		state->verbose_packetizer_interval.fu_packets += frame_packetizer.fu_packets;
		state->verbose_packetizer_interval.rtp_packets += frame_packetizer.rtp_packets;
		state->verbose_packetizer_interval.rtp_payload_bytes += frame_packetizer.rtp_payload_bytes;
	}

	{
		StreamMetricsSample sample;
		struct timespec verbose_ts_now;

		clock_gettime(CLOCK_MONOTONIC, &verbose_ts_now);
		if (stream_metrics_sample(&state->verbose_metrics, &verbose_ts_now,
		    &sample)) {
			int ofd = stdout_filter_real_fd();
			dprintf(ofd, "[verbose] %lds | %u fps | %u kbps | frame %u | avg %u B/frame | %u packs\n",
				sample.uptime_s, sample.fps, sample.kbps,
				state->frame_counter, sample.avg_bytes, stream->count);
			if (star6e_output_is_rtp(output)) {
				unsigned int avg_rtp_payload =
					state->verbose_packetizer_interval.rtp_packets > 0
					? (unsigned int)(state->verbose_packetizer_interval.rtp_payload_bytes /
						state->verbose_packetizer_interval.rtp_packets) : 0;

				dprintf(ofd, "[pktzr] nals %u | rtp %u | fill %u B | single %u | fu %u\n",
					state->verbose_packetizer_interval.total_nals,
					state->verbose_packetizer_interval.rtp_packets,
					avg_rtp_payload,
					state->verbose_packetizer_interval.single_packets,
					state->verbose_packetizer_interval.fu_packets);
			}
			{
				uint32_t errs = star6e_output_drain_send_errors(
					(Star6eOutput *)output);
				if (errs > 0)
					dprintf(ofd, "[net] %u send errors\n",
						errs);
			}
			memset(&state->verbose_packetizer_interval, 0,
				sizeof(state->verbose_packetizer_interval));
		}
	}

	return total_bytes;
}
