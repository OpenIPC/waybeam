#include "cv610_runtime.h"

#include "cv610_audio.h"
#include "cv610_encoder_config.h"
#include "pipeline_common.h"
#include "cv610_iq.h"
#include "cv610_pq_bin.h"
#include "cv610_modes.h"
#include "cv610_pipeline.h"
#include "debug_osd.h"
#include "h26x_param_sets.h"
#include "h26x_util.h"
#include "hevc_rtp.h"
#include "idr_rate_limit.h"
#include "audio_ring.h"
#include "star6e_recorder.h"
#include "star6e_ts_recorder.h"
#include "venc_jpeg.h"
#include "venc_rec_writer.h"
#include "output_socket.h"
#include "rtp_session.h"
#include "rtp_sidecar.h"
#include "timing.h"
#include "frame_gate.h"
#include "venc_frame_ring.h"
#include "venc_ring.h"
#include "venc_api.h"
#include "venc_httpd.h"
#include "venc_respawn.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "ot_common.h"
#include "ot_common_sys.h"
#include "ot_common_venc.h"
#include "ss_mpi_sys.h"
#include "ss_mpi_sys_bind.h"
#include "ss_mpi_venc.h"

#define CV610_VPSS_GRP 0
#define CV610_VPSS_CHN 0
#define CV610_VENC_CHN 0
#define CV610_FRAME_RING_SLOTS 8
#define CV610_FRAME_RING_BYTES (512u * 1024u)

typedef struct {
	VencConfig config;
	Cv610PipelineConfig pipeline;
	VencOutputUri output_uri;
	int socket_handle;
	struct sockaddr_storage destination;
	socklen_t destination_len;
	VencOutputUriType transport;
	int connected_udp;
	/* Live retarget, seqlock-protected.  The four transport fields above are
	 * written by cv610_apply_server() on the httpd thread and read by the
	 * drain loop on the producer thread, so a naive read can tear a
	 * sockaddr mid-rewrite.  Odd generation = a write is in progress.
	 *
	 * Two things this seqlock does NOT cover, both of which were claimed and
	 * neither of which is true:
	 *
	 * 1. Under connectedUdp -- the shipped default in every config -- the
	 *    destination lives in the KERNEL socket, not in these fields, and
	 *    output_socket_configure() retargets it with connect() on the live
	 *    fd.  The snapshot cannot reach that, so a udp://a -> udp://b
	 *    retarget can still split one access unit across two receivers.  The
	 *    outcome is bounded: the bootstrap IDR that follows re-anchors the
	 *    stream.
	 * 2. "Only a udp <-> unix switch closes and reopens" is wrong.
	 *    output_socket_configure() also closes the fd when destination fill
	 *    FAILS with the type unchanged -- reachable by setting a hostname
	 *    URI, since nothing here resolves names -- and leaves socket_handle
	 *    -1 across the error return.  That window is wider than the type
	 *    switch it was compared to.
	 *
	 * Both are inherited: this pattern is copied from star6e_output.c and
	 * maruko_output.c, which have the same gaps.  Fixing it properly means
	 * validating the destination before the generation bump and routing a
	 * live transport-TYPE change to the restart class. */
	/* AE ceilings as the sensor plugin seeded them, latched on first apply so
	 * clearing one back to 0 is not a one-way door.  Lives here, in the
	 * context that already exists, rather than at file scope in cv610_iq.c. */
	Cv610IqAeDefaults ae_defaults;
	unsigned transport_gen;
	/* Producer-thread snapshot, refreshed once per frame.  Same shape as
	 * Maruko's output batch: take the seqlock once, then every datagram of
	 * that frame sends from a stable copy. */
	struct {
		int socket_handle;
		struct sockaddr_storage destination;
		socklen_t destination_len;
		int connected_udp;
		int output_enabled;
	} tx;
	/* outgoing.enabled as ACTUALLY applied.  Gates both transports: a
	 * disable has to stop the ring writes too, or the field would mean
	 * "stop sending" on a udp:// craft and nothing at all on a frame-shm://
	 * one. */
	int output_enabled;
	/* The destination this backend actually programmed.  Compared against
	 * in cv610_apply_server() INSTEAD of the config: venc_api commits the
	 * new value into g_cfg before dispatching the apply, so a vcfg
	 * comparison reads equal even for a real change and would skip the
	 * repoint -- device-verified on Star6E, where the socket stayed on the
	 * startup destination while /api/v1/config advertised the new one. */
	char applied_server[256];
	OutputSocketQueue send_queue;
	venc_frame_ring_t *frame_ring;
	/* frame-shm ring low-water measurement.  Every frame-shm producer must
	 * measure: venc_frame_ring_create() publishes the VHLT health marker
	 * unconditionally, so a ring nobody measures still advertises a live
	 * gauge sitting at its create-time 0 -- the healthiest value in the
	 * range.  "Not measured" would be indistinguishable from "the consumer
	 * is keeping up perfectly", to the rate controller that is now solely
	 * responsible for reacting. */
	VencRingLowWater low_water;
	int low_water_ready;
	/* Adaptive frame gate over the single VENC channel.  See
	 * include/frame_gate.h.  Armed automatically whenever the output is a
	 * frame ring; the drain loop below is the actuator. */
	FrameGate frame_gate;
	/* Per-frame metadata channel (protocols/rtp-sidecar.md).  Bound once at
	 * output start and closed unconditionally at output stop, like every
	 * other CV610 resource: pipeline state here survives a respawn, so a
	 * flag-guarded teardown leaves the port held by the dead process. */
	RtpSidecarSender sidecar;
	/* MPP PTS epoch -> CLOCK_MONOTONIC, sampled once (see
	 * cv610_capture_us_from_pts).  The sidecar contract says capture_us is
	 * CLOCK_MONOTONIC µs; the encoder's PTS is on the MPP timebase, which
	 * on this SoC sits seconds away from it. */
	int64_t pts_to_mono_us;
	int pts_epoch_valid;
	/* Sidecar ENC_INFO frames_since_idr; advanced every frame, not only
	 * while a probe is subscribed. */
	uint16_t frames_since_idr;
	/* Latched egress-pressure state, maintained by venc_observe_pressure()
	 * on the producer thread exactly as the SigmaStar backends do.  The
	 * wire field is defined as a 75/50 HYSTERESIS flag, not a bare
	 * threshold, and pressure_frames is "frames observed in pressure" --
	 * the `pressure_drops` wire name is retained for ABI stability across
	 * the v0.9.2 frame-skip rollback and was never a drop count. */
	int pressure_state;
	uint32_t pressure_frames;
	RtpPacketizerState rtp;
	H26xParamSets param_sets;
	DebugOsdState *debug_osd;
	Cv610AudioState *audio;
	HevcRtpStats rtp_stats;
	uint32_t frame_ticks;
	uint32_t live_bitrate;
	uint16_t max_payload_size;
	int verbose;
	uint64_t frames;
	uint64_t bytes;
	uint64_t output_drops;
	uint64_t packets_sent;
	uint8_t gdr_active;
	uint8_t gdr_cycle_len;
	uint8_t gdr_counter;
	uint8_t svct_active;
	uint8_t delivered_slice_count;
	uint8_t slice_census_done;
	uint32_t requested_slice_count;
	uint32_t intra_refresh_active;
	uint32_t applied_gop_frames;
	uint32_t ref_census_frames;
	uint32_t ref_type_counts[OT_VENC_P_SLICE_BUTT];
	uint32_t trail_n_patched;
	int venc_created;
	int venc_started;
	int venc_bound;
	/* Recording (record.mode=mirror only on this backend): the main
	 * channel's already-copied access unit is teed to file.  The shared
	 * recorder cores are SoC-independent — CV610 needs no SDK-typed
	 * adapter because cv610_copy_stream() has already produced one
	 * contiguous Annex-B buffer by the time either writer sees it. */
	Star6eRecorderState recorder;
	Star6eTsRecorderState ts_recorder;
	AudioRing audio_ring;
	/* Disk writes run here, not on the encode drain loop.  A blocking
	 * write(2) between the transport send and release_stream() stalls the
	 * LIVE stream, device-measured at 22% of normal throughput for a full
	 * second on a marginal card (see include/venc_rec_writer.h). */
	VencRecWriter *rec_writer;
	/* rec_writer is created and freed on the encode loop but READ by the
	 * httpd thread for /api/v1/record/status, so the pointer itself needs
	 * a lock: without one, a status poll racing a record/stop dereferences
	 * a freed writer, and the stop blocks in pthread_join for as long as
	 * the queue takes to flush — seconds on the stalled disk this exists
	 * to survive. */
	pthread_mutex_t rec_writer_lock;
	/* rec_writer_lock and both recorders are initialised partway through
	 * cv610_init(), which can fail before reaching them — and backend.c
	 * calls the teardown regardless.  Without this, that teardown locks a
	 * never-initialised mutex and calls star6e_ts_recorder_stop() on a
	 * calloc'd state whose fd is 0, which passes the `fd < 0` test and
	 * fdatasync/close()es STDIN.  star6e and maruko carry the same flag. */
	int rec_locks_ready;
	/* Kept in the context, not the writer: the counters have to outlive
	 * the writer or a recording that shed frames reports droppedFrames:0
	 * the moment it stops — exactly the silent-damage case the counter was
	 * added to prevent. */
	uint64_t rec_dropped_frames;
	uint32_t rec_writer_peak_depth;
	/* Non-zero while a detached reaper still owns the recorders: it has not
	 * yet joined a writer whose sink outlived the stop deadline, so their
	 * descriptors are still live and a new recording must not reuse them. */
	int rec_reap_pending;
	PipelineRateWatch rate_watch;
} Cv610RunnerContext;

/* Backend selection and the vendor MPP graph are process-singleton. The
 * shared VencApplyCallbacks ABI carries no opaque pointer, so—like the
 * Star6E and Maruko runtimes—the live-control wrappers refer to the one active
 * runner. It is published before HTTP starts and cleared after HTTP joins. */
static Cv610RunnerContext *g_cv610_runner;

static Cv610IntraRefreshStatus g_cv610_intra_status;
static Cv610RefPredStatus g_cv610_ref_status;
static pthread_mutex_t g_cv610_status_mutex = PTHREAD_MUTEX_INITIALIZER;

void cv610_runtime_intra_refresh_status(Cv610IntraRefreshStatus *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&g_cv610_status_mutex);
	*out = g_cv610_intra_status;
	pthread_mutex_unlock(&g_cv610_status_mutex);
}

void cv610_runtime_ref_pred_status(Cv610RefPredStatus *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&g_cv610_status_mutex);
	*out = g_cv610_ref_status;
	pthread_mutex_unlock(&g_cv610_status_mutex);
}

static void cv610_publish_encoder_status(
	const Cv610IntraRefreshStatus *intra,
	const Cv610RefPredStatus *ref)
{
	pthread_mutex_lock(&g_cv610_status_mutex);
	if (intra)
		g_cv610_intra_status = *intra;
	if (ref)
		g_cv610_ref_status = *ref;
	pthread_mutex_unlock(&g_cv610_status_mutex);
}

/* No qp_delta field: gop_attr.normal_p.ip_qp_delta is stored by the SDK and
 * ignored by the CBR rate controller, so writing it was a control that did
 * nothing.  CV610's I-frame lever is video0.intraRefreshQp. */
static int cv610_update_venc_attr(uint32_t bitrate, uint32_t gop,
	unsigned int fields)
{
	ot_venc_chn_attr attr;
	td_s32 ret;

	memset(&attr, 0, sizeof(attr));
	ret = ss_mpi_venc_get_chn_attr(CV610_VENC_CHN, &attr);
	if (ret != TD_SUCCESS || attr.rc_attr.rc_mode != OT_VENC_RC_MODE_H265_CBR)
		return -1;
	if (fields & 1u)
		attr.rc_attr.h265_cbr.bit_rate = bitrate;
	if (fields & 2u)
		attr.rc_attr.h265_cbr.gop = gop;
	ret = ss_mpi_venc_set_chn_attr(CV610_VENC_CHN, &attr);
	return ret == TD_SUCCESS ? 0 : -1;
}

static int cv610_apply_bitrate(uint32_t kbps)
{
	int ret = cv610_update_venc_attr(kbps, 0, 1u);

	if (ret == 0 && g_cv610_runner)
		__atomic_store_n(&g_cv610_runner->live_bitrate, kbps,
			__ATOMIC_RELEASE);
	return ret;
}

/* The CV610 encoder cannot take a GOP change while intra refresh is running.
 * Measured on .181 2026-08-23: writing attr.rc_attr.h265_cbr.gop resets the
 * channel's intra-refresh state, converting a GDR stream (recovery_point SEI,
 * no IRAP) into an IDR stream for the rest of this venc lifetime -- and writing
 * the old value back does not undo it, only a restart does.  Re-asserting intra
 * refresh right after the write does restore GDR, but the recovery period stays
 * where it was (identical at gop 0.5/1.0/2.0/4.0) while the CBR window still
 * moves, swinging the achieved rate 2.7..11.2 Mbps against a fixed 9.26 Mbps
 * target.  The write can therefore be neither honoured nor made harmless, so
 * refuse it and leave the encoder untouched.
 *
 * This is not a general venc rule: with resilience "off" intra refresh is
 * inactive, video0.gop_size is the operator's to set (venc_config.c: a named
 * preset owns gop_size, only "off" preserves the user's value) and the write
 * proceeds normally.  Star6E is unaffected and keeps live GOP control
 * (device-verified on .232: cadence moved 120 -> 60 frames, GDR intact). */
static int cv610_apply_gop(uint32_t frames)
{
	Cv610RunnerContext *ctx = g_cv610_runner;
	int ret;

	/* No runner means no channel to write to. Refuse rather than fall through
	 * to the write, matching cv610_apply_verbose()/cv610_apply_max_payload_size();
	 * falling through would perform exactly the write this function exists to
	 * prevent. */
	if (!ctx)
		return -1;

	if (ctx->intra_refresh_active) {
		/* Accept a write that asks for what is already in force.  When a
		 * later group in the same request fails, the API replays the
		 * previous value of every applied group as a rollback step;
		 * refusing that replay would report "rollback incomplete" against
		 * a channel nothing had touched, which reads as damage. */
		if (frames == ctx->applied_gop_frames)
			return 0;
		fprintf(stderr, "WARNING: CV610 refusing live gop=%u: the GOP is "
			"owned by intra refresh (resilience preset); applying it "
			"would drop the stream out of GDR\n", (unsigned)frames);
		return -1;
	}
	ret = cv610_update_venc_attr(0, frames, 2u);
	if (ret == 0)
		ctx->applied_gop_frames = frames;
	return ret;
}

/* Captured on the first write so that 0 restores the driver's own default
 * rather than pinning whatever happened to be in force at the time. */
static struct {
	td_u32 max_qp, min_qp, max_i_qp, min_i_qp;
	int captured;
} g_cv610_qp_defaults;

/* CBR can only hold its target by raising QP.  In a high-gain, noise-dominated
 * scene the QP the encoder needs can exceed the driver's default ceiling, and
 * once it saturates the bitrate overshoots with the rate controller powerless
 * — measured at 3-7x target on the .181 bench with the lights off.  This is
 * the knob that lets it squeeze; it does not make the picture better, it stops
 * the overshoot.  0 on either bound leaves/restores the driver default. */
static int cv610_apply_qp_bounds(uint32_t min_qp, uint32_t max_qp)
{
	ot_venc_chn_attr attr;
	ot_venc_rc_param param;
	td_s32 ret;

	if (min_qp == 0 && max_qp == 0 && !g_cv610_qp_defaults.captured)
		return 0;   /* never written — driver defaults already in force */

	memset(&attr, 0, sizeof(attr));
	if (ss_mpi_venc_get_chn_attr(CV610_VENC_CHN, &attr) != TD_SUCCESS)
		return -1;
	/* Only CBR carries these bounds; the backend configures nothing else,
	 * but a future mode change must not write through the wrong union arm. */
	if (attr.rc_attr.rc_mode != OT_VENC_RC_MODE_H265_CBR)
		return -1;

	memset(&param, 0, sizeof(param));
	if (ss_mpi_venc_get_rc_param(CV610_VENC_CHN, &param) != TD_SUCCESS)
		return -1;

	if (!g_cv610_qp_defaults.captured) {
		g_cv610_qp_defaults.max_qp   = param.h265_cbr_param.max_qp;
		g_cv610_qp_defaults.min_qp   = param.h265_cbr_param.min_qp;
		g_cv610_qp_defaults.max_i_qp = param.h265_cbr_param.max_i_qp;
		g_cv610_qp_defaults.min_i_qp = param.h265_cbr_param.min_i_qp;
		g_cv610_qp_defaults.captured = 1;
	}

	param.h265_cbr_param.max_qp = max_qp ? max_qp : g_cv610_qp_defaults.max_qp;
	param.h265_cbr_param.min_qp = min_qp ? min_qp : g_cv610_qp_defaults.min_qp;
	/* I-frames get the same CEILING -- raising only the P bound leaves the
	 * I-frame free to blow the budget on its own, which in a noisy scene is
	 * where the biggest frames come from.  Their FLOOR is deliberately left
	 * alone, so that an I-frame floor cannot silently cancel an I-frame bias.
	 * (video0.qp_delta is not offered on this backend and is never written to
	 * the encoder -- CV610's rate control stores every I-frame input and then
	 * ignores it -- so the bias in question is whatever the driver applies.)
	 * Star6E's apply_qp_bounds() touches only the P bounds for the same
	 * reason. */
	param.h265_cbr_param.max_i_qp = max_qp ? max_qp : g_cv610_qp_defaults.max_i_qp;

	/* The API validator only compares min against max when BOTH are non-zero,
	 * so a half-specified pair can still resolve to min > max against the
	 * driver default.  The SDK takes that without complaint and then behaves
	 * erratically, so reject it here rather than write it. */
	if (param.h265_cbr_param.min_qp > param.h265_cbr_param.max_qp ||
		param.h265_cbr_param.min_i_qp > param.h265_cbr_param.max_i_qp) {
		fprintf(stderr, "ERROR: qpBounds min>max after resolving defaults "
			"(p %u/%u, i %u/%u)\n",
			(unsigned)param.h265_cbr_param.min_qp,
			(unsigned)param.h265_cbr_param.max_qp,
			(unsigned)param.h265_cbr_param.min_i_qp,
			(unsigned)param.h265_cbr_param.max_i_qp);
		return -1;
	}

	ret = ss_mpi_venc_set_rc_param(CV610_VENC_CHN, &param);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "ERROR: ss_mpi_venc_set_rc_param=0x%x\n",
			(unsigned)ret);
		return -1;
	}
	printf("> qpBounds: min=%u max=%u (0 = driver default %u/%u)\n",
		(unsigned)param.h265_cbr_param.min_qp,
		(unsigned)param.h265_cbr_param.max_qp,
		(unsigned)g_cv610_qp_defaults.min_qp,
		(unsigned)g_cv610_qp_defaults.max_qp);
	return 0;
}

static int cv610_apply_verbose(bool on)
{
	if (!g_cv610_runner)
		return -1;
	__atomic_store_n(&g_cv610_runner->verbose, on ? 1 : 0,
		__ATOMIC_RELEASE);
	return 0;
}

static int cv610_apply_max_payload_size(uint16_t size)
{
	Cv610RunnerContext *ctx = g_cv610_runner;

	if (!ctx)
		return -1;
	__atomic_store_n(&ctx->max_payload_size, size, __ATOMIC_RELEASE);
	return 0;
}

static int cv610_request_idr(void)
{
	if (!idr_rate_limit_allow(CV610_VENC_CHN))
		return 0;
	return ss_mpi_venc_request_idr(CV610_VENC_CHN, TD_TRUE) == TD_SUCCESS
		? 0 : -1;
}

/* Recorder start is a BOOTSTRAP event, not a request for a fresher picture.
 * The file that just opened contains nothing, and the shipped CV610 config is
 * resilience=racing — a GDR craft emits no periodic IDR at all, so a request
 * that the rate limiter coalesces away leaves a recording with NO IRAP access
 * unit anywhere in it.  That file seeks to nothing and plays from nothing
 * while the caller was told the start succeeded.  Same reasoning and same
 * un-coalescible path as Star6E's runtime_request_idr_on(). */
static void cv610_record_force_idr(void)
{
	idr_rate_limit_force(CV610_VENC_CHN);
	if (ss_mpi_venc_request_idr(CV610_VENC_CHN, TD_TRUE) != TD_SUCCESS)
		fprintf(stderr, "WARN: record start could not force an IDR; "
			"the recording may not decode from its first frame\n");
}

/* Runs on the writer thread, one access unit at a time, in order.  Both
 * writers no-op while their recorder is closed, and the writer is always
 * stopped before either is, so this cannot race a close. */
static void cv610_record_sink(void *opaque, const uint8_t *au, size_t len,
	uint64_t pts_90khz, int is_idr)
{
	Cv610RunnerContext *ctx = opaque;

	if (star6e_ts_recorder_is_active(&ctx->ts_recorder))
		(void)star6e_ts_recorder_write_video(&ctx->ts_recorder, au, len,
			pts_90khz, is_idr);
	else
		(void)star6e_recorder_write_au(&ctx->recorder, au, len,
			is_idr);
}

/* Runs once the writer thread has been joined and freed, inline on the encode
 * loop when the sink finished inside the deadline and on the reaper thread
 * when it did not.  Closing the descriptors here rather than at the call site
 * is what makes the asynchronous stop safe: on both paths the sink is
 * guaranteed never to run again by the time this runs. */
/* Bounded wait for a detached reaper to release the recorders.  Teardown only:
 * a reaper that outlives the context would dereference it after backend.c has
 * freed it. */
#define REC_REAP_TEARDOWN_WAIT_MS 2000
static void cv610_wait_reap(Cv610RunnerContext *ctx, unsigned ms)
{
	unsigned waited = 0;

	while (__atomic_load_n(&ctx->rec_reap_pending, __ATOMIC_ACQUIRE) &&
	       waited < ms) {
		usleep(2000);
		waited += 2;
	}
}

static void cv610_record_reap(void *opaque)
{
	Cv610RunnerContext *ctx = opaque;

	star6e_ts_recorder_stop(&ctx->ts_recorder);
	star6e_recorder_stop(&ctx->recorder);
	__atomic_store_n(&ctx->rec_reap_pending, 0, __ATOMIC_RELEASE);
}

/* Close whichever recorder is open and drop the audio tee.  Idempotent.
 *
 * Order matters: the writer thread is the only thread that touches recorder
 * state while recording, so it has to be joined BEFORE either recorder is
 * closed -- which is why the closes live in cv610_record_reap() above, the
 * one place that runs after the join on both the inline and deferred paths.
 *
 * `async` picks whether this may join at all.
 *
 * Mid-run (the encode loop) it must be bounded: an unbounded drain would put
 * the disk stall straight back on the live video path at every stop, which is
 * the whole thing this writer removes.  At teardown the video path is going
 * away regardless, so blocking is free and the full flush is what gets the
 * recording's last seconds onto disk instead of discarding them. */
static void cv610_record_stop(Cv610RunnerContext *ctx, int async)
{
	VencRecWriter *w;

	/* An init that failed before the recorders and the mutex were set up
	 * leaves them zeroed; locking one of those is undefined, and a zeroed
	 * recorder fd of 0 reads as an open file. */
	if (!ctx->rec_locks_ready)
		return;

	/* A detached reaper still owns the recorders from an earlier stop: it
	 * has not joined the writer whose sink is still inside a write(), so
	 * both descriptors are live.  Closing again here would close them under
	 * that writer AND clear rec_reap_pending, letting the very next start
	 * reuse a descriptor the abandoned writer is about to append to.  The
	 * guard only means anything if nothing else clears it. */
	if (__atomic_load_n(&ctx->rec_reap_pending, __ATOMIC_ACQUIRE)) {
		if (async)
			return;
		/* Teardown has no "later", so wait it out rather than leave the
		 * reaper running against a context about to be freed. */
		cv610_wait_reap(ctx, REC_REAP_TEARDOWN_WAIT_MS);
		if (__atomic_load_n(&ctx->rec_reap_pending, __ATOMIC_ACQUIRE)) {
			fprintf(stderr, "WARN: recorder writer still holding the "
				"file at teardown; skipping the close\n");
			return;
		}
	}

	/* Detach under the lock so a status poll already inside
	 * venc_rec_writer_stats() finishes against a live writer, and any
	 * later one sees NULL.  Harvest the counters first — they must
	 * survive the writer they came from. */
	pthread_mutex_lock(&ctx->rec_writer_lock);
	w = ctx->rec_writer;
	if (w) {
		uint64_t dropped = 0;
		uint32_t peak = 0;

		venc_rec_writer_stats(w, NULL, &dropped, NULL, &peak);
		ctx->rec_dropped_frames = dropped;
		ctx->rec_writer_peak_depth = peak;
	}
	ctx->rec_writer = NULL;
	pthread_mutex_unlock(&ctx->rec_writer_lock);

	/* Bounded, and outside the lock.  An unbounded drain would put the
	 * disk stall back on the encode loop at every stop — the very thing
	 * the writer removes — so a stalled queue is abandoned instead: the
	 * recording loses its tail, the live link loses nothing. */
	/* Detach the audio tee now either way: it stops the capture thread
	 * feeding the ring, which is independent of the writer and must not
	 * wait on it. */
	cv610_audio_set_record_ring(ctx->audio, NULL);

	if (async) {
		uint64_t dropped = 0;

		/* Claim the recorders for the reap before the stop, because on
		 * the fast path the callback runs inside it and clears this
		 * again before the call returns. */
		__atomic_store_n(&ctx->rec_reap_pending, 1, __ATOMIC_RELEASE);
		venc_rec_writer_stop_bounded_async(w, 250, &dropped,
			cv610_record_reap, ctx);
		/* Only if there WAS a writer: the async stop leaves *dropped at
		 * 0 for a NULL one, which would clobber the harvest above.
		 * Stored under the lock because the httpd thread reads it and an
		 * unlocked 64-bit store tears on ARM32. */
		if (w) {
			pthread_mutex_lock(&ctx->rec_writer_lock);
			ctx->rec_dropped_frames = dropped;
			pthread_mutex_unlock(&ctx->rec_writer_lock);
		}
		return;
	}

	/* Teardown: there is no "later" for a reaper to run in, so join here.
	 * Still bounded -- the backlog is abandoned first. */
	{
		uint64_t dropped = 0;

		venc_rec_writer_stop_bounded(w, 250, &dropped);
		if (w) {
			pthread_mutex_lock(&ctx->rec_writer_lock);
			ctx->rec_dropped_frames = dropped;
			pthread_mutex_unlock(&ctx->rec_writer_lock);
		}
	}
	cv610_record_reap(ctx);
}

static void cv610_record_start(Cv610RunnerContext *ctx, const char *dir)
{
	int want_audio;
	int rc;

	if (!dir || !dir[0] || !ctx->rec_locks_ready)
		return;
	/* Stop first: a start over a live recording must not leak the open fd,
	 * and only one of the two recorders may ever be active. */
	cv610_record_stop(ctx, 1);

	/* On a healthy card the stop above completed inline and this is clear.
	 * If it did not, a detached reaper still owns both recorders'
	 * descriptors -- the previous recording's sink has not returned -- and
	 * starting over them would race the close.  Refuse rather than corrupt:
	 * the medium is not accepting writes anyway. */
	if (__atomic_load_n(&ctx->rec_reap_pending, __ATOMIC_ACQUIRE)) {
		fprintf(stderr, "WARN: record start refused; the previous "
			"recording's writer has not released the file yet "
			"(storage not completing writes)\n");
		return;
	}

	/* Before the open, not after it: these describe the recording being
	 * started, and a start that FAILS must not leave the previous
	 * recording's drop count standing as if it belonged to this one. */
	pthread_mutex_lock(&ctx->rec_writer_lock);
	ctx->rec_dropped_frames = 0;
	ctx->rec_writer_peak_depth = 0;
	pthread_mutex_unlock(&ctx->rec_writer_lock);

	if (strcmp(ctx->config.record.format, "hevc") == 0) {
		if (star6e_recorder_start(&ctx->recorder, dir) != 0)
			return;
	} else {
		want_audio = ctx->config.audio.enabled && ctx->audio;
		if (star6e_ts_recorder_start(&ctx->ts_recorder, dir,
				want_audio ? &ctx->audio_ring : NULL) != 0)
			return;
		if (want_audio)
			cv610_audio_set_record_ring(ctx->audio, &ctx->audio_ring);
	}

	/* After the file is open, so the sink never sees a closed recorder.
	 *
	 * A failed start CLOSES the recording rather than leaving it open: the
	 * drain loop only pushes when rec_writer is non-NULL, so a NULL writer
	 * with an open file reports active:true with framesWritten frozen at 0
	 * forever and a file holding nothing but PAT/PMT — a phantom recording,
	 * which is worse than an honest failure. */
	pthread_mutex_lock(&ctx->rec_writer_lock);
	rc = venc_rec_writer_start(&ctx->rec_writer, cv610_record_sink, ctx);
	pthread_mutex_unlock(&ctx->rec_writer_lock);
	if (rc != 0) {
		fprintf(stderr, "ERROR: recorder writer thread did not start "
			"(%d); recording not started\n", rc);
		cv610_record_stop(ctx, 1);
		return;
	}

	cv610_record_force_idr();
}

static void cv610_record_status_callback(VencRecordStatus *out)
{
	Cv610RunnerContext *ctx = g_cv610_runner;

	memset(out, 0, sizeof(*out));
	if (!ctx)
		return;

	{
		uint64_t dropped;
		uint32_t peak;

		/* Under the lock: cv610_record_stop() frees the writer from the
		 * encode loop, and this runs on the httpd thread.
		 *
		 * The stored fallbacks are read inside it too, not just the
		 * writer: a 64-bit load on ARM32 is two instructions and can
		 * straddle the encode loop's store as easily as the store can
		 * tear.  star6e and maruko already do it this way. */
		pthread_mutex_lock(&ctx->rec_writer_lock);
		dropped = ctx->rec_dropped_frames;
		peak = ctx->rec_writer_peak_depth;
		if (ctx->rec_writer)
			venc_rec_writer_stats(ctx->rec_writer, NULL, &dropped,
				NULL, &peak);
		pthread_mutex_unlock(&ctx->rec_writer_lock);
		out->dropped_frames = (uint32_t)dropped;
		out->writer_peak_depth = peak;
	}

	/* is_RECORDING, not is_active: rotation runs on the writer thread here
	 * too (see the recorder hand-off in the drain loop) and holds
	 * fd == -1 across it, so the descriptor is not the right question for a
	 * reader on the httpd thread. */
	{
		/* ONE coherent instant per recorder.  The fields below are
		 * mutated by the writer thread during writes and segment
		 * rotation: bytes_written is 64-bit on ARM32 and path is
		 * rewritten wholesale on a rotation, so reading them in place
		 * could tear outright, and reading active, counters and path at
		 * three different instants could disagree with each other. */
		Star6eRecorderSnapshot ts_snap, rec_snap;

		star6e_ts_recorder_snapshot(&ctx->ts_recorder, &ts_snap);
		star6e_recorder_snapshot(&ctx->recorder, &rec_snap);

		if (ts_snap.active) {
			out->active = 1;
			snprintf(out->format, sizeof(out->format), "ts");
			out->bytes_written = ts_snap.bytes_written;
			out->frames_written = ts_snap.frames_written;
			out->segments = ts_snap.segments;
			out->elapsed_ms = ts_snap.elapsed_ms;
			snprintf(out->path, sizeof(out->path), "%s",
				ts_snap.path);
			snprintf(out->stop_reason, sizeof(out->stop_reason),
				"none");
		} else if (rec_snap.active) {
			out->active = 1;
			snprintf(out->format, sizeof(out->format), "hevc");
			out->bytes_written = rec_snap.bytes_written;
			out->frames_written = rec_snap.frames_written;
			/* Meaningful since this recorder rotates: it read 0
			 * while 16 segments were on disk, device-measured. */
			out->segments = rec_snap.segments;
			out->elapsed_ms = rec_snap.elapsed_ms;
			snprintf(out->path, sizeof(out->path), "%s",
				rec_snap.path);
			snprintf(out->stop_reason, sizeof(out->stop_reason),
				"none");
		} else {
			/* Which recorder ran is decided by record.format, and
			 * that field is MUT_RESTART -- so within one process only
			 * ONE of these two can ever have recorded, and the other
			 * snapshot is zeroed.  Select on the format.
			 *
			 * The previous rule ("take the TS reason unless it is
			 * manual") guessed from a value that says nothing about
			 * which session ended last: a TS write_error retained
			 * from an earlier run would outrank a later, cleanly
			 * stopped hevc recording and now, with the counters
			 * following the reason, would name that older file's
			 * path and byte count too. */
			const char *reason = "manual";
			const Star6eRecorderSnapshot *last =
				(strcmp(ctx->config.record.format, "hevc") == 0) ? &rec_snap : &ts_snap;
			Star6eRecorderStopReason sr = last->last_stop_reason;

			if (sr == RECORDER_STOP_DISK_FULL)
				reason = "disk_full";
			else if (sr == RECORDER_STOP_WRITE_ERROR)
				reason = "write_error";
			else if (sr == RECORDER_STOP_SIZE_LIMIT)
				reason = "size_limit";
			/* Report what the finished recording produced, from the
			 * same snapshot the reason came from.  This branch used
			 * to leave them at zero, so a recorder that stopped on
			 * its own answered {path:"", frames:0, bytes:0} -- the
			 * operator lost both the file that was cut short and how
			 * far it got, which is the whole diagnosis for any stop
			 * that was not manual.  elapsed_ms stays out: the
			 * snapshot zeroes it when inactive by contract. */
			out->bytes_written = last->bytes_written;
			out->frames_written = last->frames_written;
			out->segments = last->segments;
			snprintf(out->path, sizeof(out->path), "%s", last->path);
			snprintf(out->stop_reason, sizeof(out->stop_reason),
				"%s", reason);
			snprintf(out->format, sizeof(out->format), "%s",
				ctx->config.record.format);
		}
	}
}

/* Convert an encoder PTS to the CLOCK_MONOTONIC µs the sidecar contract
 * requires for capture_us.  MPP keeps its own timebase: measured on .181 it
 * ran ~4.63 s ahead of CLOCK_MONOTONIC, so publishing the raw PTS put
 * capture_us *after* frame_ready_us and made the encode-duration derivation
 * come out negative — a field that looks populated and cannot be trended.
 *
 * The offset is sampled once, lazily, on the first frame that needs it: by
 * then MI_SYS is up, which it need not be when the output is started.  Both
 * clocks are read microseconds apart, which is noise against a seconds-scale
 * offset.  Returns 0 — the contract's "not available" — while the epoch is
 * unknown, rather than a number on the wrong base. */
static uint64_t cv610_capture_us_from_pts(Cv610RunnerContext *ctx, uint64_t pts)
{
	int64_t mono_us;

	if (pts == 0)
		return 0;
	if (!ctx->pts_epoch_valid) {
		td_u64 cur_pts = 0;
		uint64_t now_us = wb_monotonic_us();

		if (ss_mpi_sys_get_cur_pts(&cur_pts) != TD_SUCCESS || cur_pts == 0)
			return 0;
		ctx->pts_to_mono_us = (int64_t)now_us - (int64_t)cur_pts;
		ctx->pts_epoch_valid = 1;
	}
	mono_us = (int64_t)pts + ctx->pts_to_mono_us;
	return mono_us > 0 ? (uint64_t)mono_us : 0;
}

/* One observation of whatever transport is carrying video, in the shape both
 * consumers need: the /api/v1/transport/status route and the per-frame sidecar
 * TRANSPORT_INFO trailer.  They read the same collector so the number an
 * operator sees over HTTP and the number a probe sees on the wire cannot
 * disagree. */
typedef struct {
	int         active;
	int         is_ring;
	const char *name;            /* "frame-shm" | "unix" | "udp" | "none" */
	uint8_t     fill_pct;
	int         have_fill;        /* 0 = fill_pct is not a measurement      */
	int         in_pressure;      /* latched hysteresis flag, not fill>=HW */
	uint32_t    pressure_frames;  /* frames observed in pressure           */
	uint64_t    transport_drops; /* ring: full drops; socket: send failures */
	uint64_t    packets_sent;    /* ring: writes;     socket: datagrams    */
	/* Ring-only; zero on the socket transports. */
	uint64_t    oversize_drops;
	uint64_t    other_drops;
	uint32_t    slot_count;
	uint32_t    used_slots;
} Cv610TransportSample;

/* `fill` lets the drain loop hand over the ring reading it already took for
 * the low-water window, so a subscribed sidecar costs one ring load per frame
 * rather than two.  Pass NULL to read it here (the HTTP route, on the httpd
 * thread).  Returns -1 only when the ring read fails. */
static int cv610_collect_transport(Cv610RunnerContext *ctx,
	const venc_frame_ring_fill_t *fill, Cv610TransportSample *out)
{
	venc_frame_ring_fill_t local;

	if (!ctx || !out)
		return -1;
	memset(out, 0, sizeof(*out));
	out->name = "none";

	if (ctx->frame_ring) {
		if (!fill) {
			if (venc_frame_ring_get_fill(ctx->frame_ring, &local) != 0)
				return -1;
			fill = &local;
		}
		out->active = 1;
		out->is_ring = 1;
		out->name = "frame-shm";
		out->fill_pct = fill->fill_pct;
		out->transport_drops = fill->full_drops;
		out->packets_sent = fill->writes;
		out->oversize_drops = fill->oversize_drops;
		out->other_drops = fill->other_drops;
		out->slot_count = fill->slot_count;
		out->used_slots = fill->used_slots;
		out->have_fill = 1;
	} else if (ctx->socket_handle >= 0) {
		uint8_t fill_pct = 0;

		/* A failed SIOCOUTQ is "unknown", never "empty".  Publishing the
		 * substituted 0 as a measurement fed a real sample to the pressure
		 * observer and could report a full queue as fillPct 0 / inPressure
		 * false.  star6e_output.c keys on have_fill for the same reason and
		 * declines to observe; this now matches it. */
		out->have_fill = output_socket_get_fill_pct(ctx->socket_handle,
			&ctx->send_queue, &fill_pct) == 0;
		out->active = 1;
		out->name = ctx->transport == VENC_OUTPUT_URI_UNIX ? "unix" : "udp";
		out->fill_pct = out->have_fill ? fill_pct : 0;
		out->transport_drops = __atomic_load_n(&ctx->output_drops,
			__ATOMIC_RELAXED);
		out->packets_sent = __atomic_load_n(&ctx->packets_sent,
			__ATOMIC_RELAXED);
	}
	/* Sample-point divergence, deliberate and worth knowing: this backend
	 * reads the ring fill AFTER writing the current frame, while Star6E
	 * observes before its send.  On an 8-slot ring that is a systematic one
	 * slot (12.5 percentage points) of extra occupancy in fill_pct.  Moving
	 * it would also move the low-water window, which is pre-existing
	 * behaviour outside this change's scope; recorded here so a consumer
	 * comparing fill_pct across craft types is not surprised.
	 *
	 * Report the latched flag rather than recomputing fill >= high-water:
	 * the header defines this field as the hysteresis flag, and a bare
	 * threshold would make an operator's HTTP reading and a probe's wire
	 * reading disagree with the SigmaStar backends for identical ring
	 * behaviour.  The producer thread does the observing. */
	out->in_pressure = out->active &&
		__atomic_load_n(&ctx->pressure_state, __ATOMIC_RELAXED);
	out->pressure_frames = __atomic_load_n(&ctx->pressure_frames,
		__ATOMIC_RELAXED);
	return 0;
}

/* CV610 wording for frame_gate_setup()'s verdict; the checks are shared in
 * frame_gate.c so the three backends cannot drift. */
static void cv610_report_frame_gate_setup(FrameGateSetupStatus st,
	const FrameGate *g, uint32_t slot_count)
{
	switch (st) {
	case FRAME_GATE_SETUP_NO_RING:
		fprintf(stderr, "NOTE: [cv610] frame gate inert — this output "
			"is not frame-shm, so it has no occupancy signal\n");
		return;
	case FRAME_GATE_SETUP_CLOSE_TOO_HIGH:
		fprintf(stderr, "WARNING: frameGateCloseSlots %u exceeds the "
			"ring's %u slots; gate can never close\n",
			(unsigned)g->cfg.close_slots, (unsigned)slot_count);
		return;
	case FRAME_GATE_SETUP_READY:
		printf("> frame gate on: close>=%u open<=%u escape=%ums "
			"ring=%u slots\n",
			(unsigned)g->cfg.close_slots, (unsigned)g->cfg.open_slots,
			(unsigned)(g->cfg.max_closed_us / 1000u),
			(unsigned)slot_count);
		return;
	}
}

/* Adaptive frame gate — the CV610 twin of star6e_service_frame_gate().  Same
 * rationale (a bitrate write implicitly keyframes; stopping frame intake
 * changes no encoder state) and the same signal, the instantaneous egress
 * ring occupancy rather than the 200 ms low-water window.
 *
 * Called from the normal per-frame path AND from every branch of the stream
 * loop that can be reached while the gate is closed — no frames are produced
 * then, so the per-frame call never runs and a gate that cannot reopen is a
 * hung stream. */
static void cv610_service_frame_gate(Cv610RunnerContext *ctx)
{
	venc_frame_ring_fill_t fill;
	uint64_t now_us;

	if (!ctx || !frame_gate_enabled(&ctx->frame_gate))
		return;

	if (!ctx->frame_ring ||
	    venc_frame_ring_get_fill(ctx->frame_ring, &fill) != 0) {
		/* No SDK call: intake was never stopped, only the drain. */
		if (!frame_gate_is_open(&ctx->frame_gate))
			frame_gate_force_open(&ctx->frame_gate,
				wb_monotonic_us());
		return;
	}

	now_us = wb_monotonic_us();

	/* CV610 records in mirror mode only (the loop above refuses every
	 * other record.mode), so the recorder always shares this channel:
	 * suppress closes while a recording runs rather than punching holes
	 * in the file for a radio problem.  Reopens are never suppressed. */
	if (frame_gate_is_open(&ctx->frame_gate) &&
	    star6e_record_wants_frame(&ctx->ts_recorder, &ctx->recorder))
		return;

	/* The policy flip IS the actuator, so the returned action needs no
	 * handling: the loop below stops pulling while the gate reads closed
	 * and VENC's own output FIFO applies the backpressure.  Nothing is
	 * issued to the SDK deliberately — stop_chn/start_chn emit an IRAP on
	 * resume here too, measured at three per five cycles on this board. */
	(void)frame_gate_observe(&ctx->frame_gate, fill.used_slots, now_us);
}

/* Star6E/Maruko parity — see star6e_service_ring_low_water().  venc measures
 * egress pressure and publishes it; it never acts on it.
 *
 * `fill` is the reading the caller already took for this frame; NULL means
 * there is no ring, or the read failed. */
static void cv610_service_ring_low_water(Cv610RunnerContext *ctx,
	const venc_frame_ring_fill_t *fill)
{
	uint64_t now_us;

	if (!ctx)
		return;
	/* Clear the window when there is no ring, exactly as the SigmaStar
	 * backends do: a carried-over low_slots of 0 would swallow the next
	 * ring's first sample and publish "perfectly drained" over a ring
	 * that is full.  Unreachable today (the context is created once per
	 * process), kept symmetric so it stays that way. */
	if (!fill) {
		venc_ring_low_water_reset(&ctx->low_water, 0);
		ctx->low_water_ready = 0;
		return;
	}

	now_us = wb_monotonic_us();
	if (!ctx->low_water_ready) {
		venc_ring_low_water_reset(&ctx->low_water, now_us);
		ctx->low_water_ready = 1;
	}

	venc_ring_low_water_observe(&ctx->low_water, fill->used_slots,
		fill->slot_count);
	if (venc_ring_low_water_tick(&ctx->low_water, now_us))
		venc_frame_ring_set_low_water(ctx->frame_ring,
			venc_ring_low_water_slots(&ctx->low_water));
}

static uint32_t cv610_query_live_fps(void)
{
	return g_cv610_runner ? g_cv610_runner->pipeline.fps : 0;
}

static char *cv610_query_transport_status(void)
{
	Cv610RunnerContext *ctx = g_cv610_runner;
	char buf[640];
	int pos;
	Cv610TransportSample ts;

	if (!ctx)
		return NULL;
	if (cv610_collect_transport(ctx, NULL, &ts) != 0)
		return NULL;
	if (ts.is_ring) {
		char gate_json[FRAME_GATE_STATUS_JSON_CAP];

		frame_gate_status_json(&ctx->frame_gate, wb_monotonic_us(),
			gate_json, sizeof(gate_json));
		pos = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{"
			"\"active\":true,\"transport\":\"frame-shm\","
			"\"fillPct\":%u,\"inPressure\":%s,"
			"\"transportDrops\":%llu,\"pressureDrops\":%u,"
			"\"framesSent\":%llu,\"oversizeDrops\":%llu,"
			"\"slotCount\":%u,\"usedSlots\":%u,"
			"\"ringLowWaterSlots\":%u,\"otherDrops\":%llu%s}}",
			(unsigned)ts.fill_pct,
			ts.in_pressure ? "true" : "false",
			(unsigned long long)ts.transport_drops,
			(unsigned)ts.pressure_frames,
			(unsigned long long)ts.packets_sent,
			(unsigned long long)ts.oversize_drops,
			(unsigned)ts.slot_count, (unsigned)ts.used_slots,
			(unsigned)venc_ring_low_water_slots(&ctx->low_water),
			(unsigned long long)ts.other_drops, gate_json);
	} else if (ts.active) {
		pos = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{"
			"\"active\":true,\"transport\":\"%s\","
			"\"fillPct\":%u,\"inPressure\":%s,"
			"\"pressureDrops\":%u,\"transportDrops\":%llu,"
			"\"packetsSent\":%llu}}",
			ts.name, (unsigned)ts.fill_pct,
			ts.in_pressure ? "true" : "false",
			(unsigned)ts.pressure_frames,
			(unsigned long long)ts.transport_drops,
			(unsigned long long)ts.packets_sent);
	} else {
		pos = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{"
			"\"active\":false,\"transport\":\"none\"}}");
	}
	if (pos < 0 || pos >= (int)sizeof(buf))
		return NULL;
	return strdup(buf);
}

/* The CV610 audio path is fixed at 48 kHz mono Opus with a hardcoded mic
 * gain (src/cv610_audio.c), so this reports what the hardware is actually
 * doing rather than echoing config fields the encoder never reads.  The
 * counters come from the audio thread under its own stats lock. */
static char *cv610_query_audio_status(void)
{
	Cv610RunnerContext *ctx = g_cv610_runner;
	uint64_t frames = 0, bytes = 0, packets = 0, drops = 0;
	char buf[384];
	int pos;

	if (!ctx)
		return NULL;
	if (!ctx->audio) {
		pos = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{"
			"\"enabled\":false,\"backend\":\"cv610\"}}");
	} else {
		cv610_audio_get_stats(ctx->audio, &frames, &bytes, &packets,
			&drops);
		pos = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{"
			"\"enabled\":true,\"backend\":\"cv610\","
			"\"running\":%s,\"codec\":\"opus\","
			"\"sample_rate\":48000,\"channels\":1,"
			"\"muted\":%s,"
			"\"frames\":%llu,\"bytes\":%llu,"
			"\"packets\":%llu,\"drops\":%llu}}",
			cv610_audio_is_running(ctx->audio) ? "true" : "false",
			ctx->config.audio.mute ? "true" : "false",
			(unsigned long long)frames, (unsigned long long)bytes,
			(unsigned long long)packets, (unsigned long long)drops);
	}
	if (pos < 0 || pos >= (int)sizeof(buf))
		return NULL;
	return strdup(buf);
}

/* Horizontal centre-priority ROI bands -- the same geometry the two SigmaStar
 * backends draw (pipeline_common_roi_band); only the SDK struct differs.
 * ot_venc_roi_attr is field-for-field MI_VENC_RoiCfg_t, and this part accepts a
 * wider delta ([-51, 51] for H.265) than the +/-30 the shared API clamps to, so
 * nothing here needs a per-backend range.
 *
 * Geometry comes from the channel attr rather than a cached width: it is what
 * the encoder is actually running, and the get doubles as the "channel exists"
 * guard, so a live set arriving before cv610_venc_start() refuses instead of
 * programming bands against a zero-size frame.
 *
 * The readback is a WRITE check, not an effect check.  It catches an SDK that
 * clamped or silently dropped a rectangle; it cannot tell whether the rate
 * controller acts on the delta.  That distinction is not theoretical here --
 * issue #259 is a QP control on the sibling parts that returns success, logs as
 * applied, reads back clean, and never moves the bitstream, and this part's own
 * CBR does the same with ip_qp_delta (see the create-time comment in
 * cv610_venc_start).  The effect is a decoded-bitstream measurement, not an API
 * call: a horizontal detail profile has to step at the programmed rect edges. */
static int cv610_apply_roi_qp(int qp)
{
	ot_venc_chn_attr attr;
	ot_venc_roi_attr roi;
	const VencConfig *cfg = g_cv610_runner ? &g_cv610_runner->config : NULL;
	uint32_t width, height;
	uint16_t steps;
	float center_frac;
	int i;
	int ok = 1;
	int programmed = 0;

	if (!cfg)
		return -1;

	memset(&attr, 0, sizeof(attr));
	if (ss_mpi_venc_get_chn_attr(CV610_VENC_CHN, &attr) != TD_SUCCESS)
		return -1;
	width = attr.venc_attr.pic_width;
	height = attr.venc_attr.pic_height;
	if (width == 0 || height == 0)
		return -1;

	/* Clear first, unconditionally, so a shrinking steps count cannot leave
	 * a stale outer band enabled -- the same order both SigmaStar backends
	 * use.  Only the indices this backend ever writes are cleared; the part
	 * offers OT_VENC_MAX_ROI_NUM (8) and nothing here touches 4..7. */
	for (i = 0; i < PIPELINE_ROI_MAX_STEPS; i++) {
		td_s32 cret;

		memset(&roi, 0, sizeof(roi));
		roi.idx = (td_u32)i;
		roi.enable = TD_FALSE;
		cret = ss_mpi_venc_set_roi_attr(CV610_VENC_CHN, &roi);
		/* Checked, not discarded.  The rect is all-zero here, and the
		 * SDK header documents no rule on whether a disable still
		 * range-checks it.  If a clear is refused, the stale band stays
		 * enabled with its previous geometry -- so shrinking roiSteps
		 * from 4 to 2 would leave indices 2 and 3 holding the 4-step
		 * rects, overlapping the new pair with the wrong taper, while
		 * this function logged success. */
		if (cret != TD_SUCCESS) {
			fprintf(stderr, "ERROR: ROI[%d] clear=0x%x (stale band "
				"may still be enabled)\n", i, (unsigned)cret);
			ok = 0;
		}
	}

	if (!cfg->fpv.roi_enabled || qp == 0) {
		/* Name the cause -- see the matching comment in
		 * star6e_controls.c's apply_roi_qp().  A NULL cfg returned -1
		 * above, so only two states reach here. */
		printf("> ROI disabled (%s), %s\n",
			!cfg->fpv.roi_enabled ? "roiEnabled=false" :
				"roiQp=0, no delta to apply",
			ok ? "all regions cleared" :
				"CLEAR FAILED, see above");
		return ok ? 0 : -1;
	}

	if (qp < -20) qp = -20;
	if (qp > 20) qp = 20;

	steps = cfg->fpv.roi_steps;
	if (steps < 1) steps = 1;
	if (steps > PIPELINE_ROI_MAX_STEPS) steps = PIPELINE_ROI_MAX_STEPS;

	center_frac = (float)cfg->fpv.roi_center;
	if (center_frac < 0.1f) center_frac = 0.1f;
	if (center_frac > 0.9f) center_frac = 0.9f;

	for (i = 0; i < steps; i++) {
		PipelineRoiBand band;
		ot_venc_roi_attr back;
		td_s32 ret;

		if (pipeline_common_roi_band(width, height, center_frac, qp,
		    steps, i, &band) != 0)
			continue;

		memset(&roi, 0, sizeof(roi));
		roi.idx = (td_u32)i;
		roi.enable = TD_TRUE;
		roi.is_abs_qp = TD_FALSE;
		roi.qp = band.qp;
		roi.rect.x = (td_s32)band.x;
		roi.rect.y = (td_s32)band.y;
		roi.rect.width = band.width;
		roi.rect.height = band.height;

		ret = ss_mpi_venc_set_roi_attr(CV610_VENC_CHN, &roi);
		if (ret != TD_SUCCESS) {
			fprintf(stderr, "ERROR: ROI[%d] set=0x%x rect=(%u,%u %ux%u) "
				"qp=%+d\n", i, (unsigned)ret, band.x, band.y,
				band.width, band.height, band.qp);
			ok = 0;
			continue;
		}

		memset(&back, 0, sizeof(back));
		if (ss_mpi_venc_get_roi_attr(CV610_VENC_CHN, (td_u32)i,
			&back) != TD_SUCCESS) {
			fprintf(stderr, "ERROR: ROI[%d] readback failed\n", i);
			ok = 0;
		} else if (!back.enable || back.qp != band.qp ||
			back.rect.x != (td_s32)band.x ||
			back.rect.y != (td_s32)band.y ||
			back.rect.width != band.width ||
			back.rect.height != band.height) {
			/* The ORIGIN is compared too.  A driver that re-aligns
			 * or clamps x displaces the band horizontally, which is
			 * the one failure this check exists to catch and the
			 * least visible in the picture. */
			fprintf(stderr, "ERROR: ROI[%d] readback disagrees: "
				"enable=%d qp=%+d (%d,%d %ux%u), wrote "
				"enable=1 qp=%+d (%u,%u %ux%u)\n",
				i, (int)back.enable, (int)back.qp,
				(int)back.rect.x, (int)back.rect.y,
				(unsigned)back.rect.width,
				(unsigned)back.rect.height,
				band.qp, band.x, band.y,
				band.width, band.height);
			ok = 0;
		}
		programmed++;
	}

	if (programmed == 0) {
		/* Every band was skipped as degenerate.  Saying "ROI horizontal"
		 * here would assert an ROI that does not exist -- the exact
		 * class of lie this whole change set is about. */
		fprintf(stderr, "ERROR: ROI programmed no bands at %ux%u "
			"(steps=%u center=%.2f)\n", width, height, steps,
			(double)center_frac);
		return -1;
	}

	if (ok) {
		printf("> ROI horizontal: %ux%u, %u steps, center=%.0f%%, qp=%+d\n",
			width, height, steps, center_frac * 100.0f, qp);
	}

	return ok ? 0 : -1;
}

/* Live destination change.  Socket transports only, matching both SigmaStar
 * backends: the ring transports are created once at start and a live switch
 * across that boundary would have to destroy a ring while a consumer is
 * attached, so it is refused in both directions rather than half-supported.
 *
 * NOTE the consequence, which is inherited from Star6E rather than invented
 * here: outgoing.server advertises `live` on every backend, so on a craft
 * configured with frame-shm:// the field is live in the capability map and the
 * write fails with the message below.  That is the existing fleet-wide
 * behaviour; making CV610 differ would be the surprise. */
static int cv610_apply_server(const char *uri)
{
	VencOutputUri parsed;

	if (!g_cv610_runner || !uri)
		return -1;
	if (venc_config_parse_output_uri(uri, &parsed) != 0)
		return -1;

	/* Ring transports are RESTART-CLASS here, not refused.
	 *
	 * The ring is created once at start, so it genuinely cannot be switched
	 * in place -- but refusing outright (which is what both SigmaStar
	 * backends do) would REGRESS this backend.  Before outgoing.server
	 * became live on CV610 it was restart-required, so writing
	 * frame-shm://... persisted and took effect on the next boot; a hard
	 * refusal would make the fleet's normal production transport
	 * unreachable through the API, leaving hand-editing /etc/waybeam.json
	 * as the only way to provision a craft.
	 *
	 * So: commit the value, ask for the respawn, and report it.  Returning
	 * 0 is what keeps venc_api from rolling the config back;
	 * venc_api_request_reinit() is what makes the new URI actually take
	 * effect; and the live-set response carries reinit_pending so the
	 * caller is told this one is not live. */
	/* The no-op guard comes FIRST, ahead of the ring branch below.  Compare
	 * the APPLIED destination, not the config -- see the applied_server
	 * comment on the context.  An unchanged re-POST must cost nothing: it is
	 * not a bootstrap event, and firing the un-coalescible IDR on it would
	 * turn a no-op write into a keyframe at the caller's request rate.
	 *
	 * Ordering is load-bearing, not tidiness.  With the ring branch first, a
	 * craft already running frame-shm://venc_frame answered an identical
	 * re-POST by latching a reinit and respawning -- so every idempotent
	 * config re-apply from the ground cost a full pipeline restart and a
	 * multi-second video outage.  Star6E compares before it refuses for the
	 * same reason. */
	if (g_cv610_runner->applied_server[0] &&
	    strcmp(g_cv610_runner->applied_server, uri) == 0)
		return 0;

	/* Validate a socket destination BEFORE either branch acts, because both
	 * of them act destructively on a URI they never checked.
	 *
	 * Nothing here resolves names -- the fill is inet_pton only -- so
	 * `outgoing.server=udp://somehost:5600` used to reach:
	 *
	 *   - the ring branch, on a craft already running frame-shm://, which
	 *     COMMITS the value and respawns.  The re-exec then reloads the bad
	 *     URI, fails to bring the output up, and the daemon stays down --
	 *     measured on the bench: one HTTP set left the craft dead until the
	 *     config was repaired over ssh.  That is worse than the socket case
	 *     below, because it survives the restart and takes video with it.
	 *   - output_socket_configure(), which closes the fd when fill fails --
	 *     including on the fd-REUSE path where the type is unchanged and the
	 *     socket was working -- leaving socket_handle -1 across the error
	 *     return, the JSON build and the rollback re-entry.
	 *
	 * Filling a throwaway sockaddr with the same input the real call will use
	 * refuses both, with the live transport untouched, nothing committed and
	 * the generation counter never bumped.  One inet_pton on a stack buffer,
	 * on an operator-rate path.
	 *
	 * Gated on the type: the ring URIs carry no sockaddr, and their own
	 * validity is checked where they are created. */
	if ((parsed.type == VENC_OUTPUT_URI_UDP ||
	     parsed.type == VENC_OUTPUT_URI_UNIX) &&
	    !output_socket_destination_is_usable(&parsed)) {
		fprintf(stderr, "ERROR: outgoing.server %s has no usable "
			"destination; transport left unchanged\n", uri);
		return -1;
	}

	if (g_cv610_runner->frame_ring ||
	    parsed.type == VENC_OUTPUT_URI_SHM ||
	    parsed.type == VENC_OUTPUT_URI_FRAME_SHM) {
		printf("> Destination %s requires a restart (ring transports are "
			"created once); committed, respawning\n", uri);
		/* Record it as applied so the respawn window is idempotent too:
		 * a second identical write before the drain loop acts must not
		 * latch a second reinit. */
		snprintf(g_cv610_runner->applied_server,
			sizeof(g_cv610_runner->applied_server), "%s", uri);
		venc_api_request_reinit();
		return 0;
	}

	__atomic_fetch_add(&g_cv610_runner->transport_gen, 1, __ATOMIC_RELEASE);
	if (output_socket_configure(&g_cv610_runner->socket_handle,
		&g_cv610_runner->destination, &g_cv610_runner->destination_len,
		&g_cv610_runner->transport, &parsed,
		g_cv610_runner->config.outgoing.connected_udp,
		g_cv610_runner->config.outgoing.allow_unix_encoder_stall,
		&g_cv610_runner->connected_udp) != 0) {
		/* Kept as a backstop, not as the primary defence: the
		 * destination is now validated above, so the bad-URI route into
		 * this branch is unreachable and what remains is a genuine
		 * socket() or bind() failure.  output_socket_configure() still
		 * closes the socket when it fails partway, so the recovery below
		 * is still required.  Clear the applied record so the rollback
		 * that venc_api runs next actually re-configures instead of
		 * hitting the no-op guard above and returning 0 -- which left
		 * the craft with a rolled-back config, an HTTP 500, and no
		 * video at all until the process restarted. */
		g_cv610_runner->applied_server[0] = '\0';
		__atomic_fetch_add(&g_cv610_runner->transport_gen, 1,
			__ATOMIC_RELEASE);
		return -1;
	}
	(void)output_socket_capture_capacity(g_cv610_runner->socket_handle,
		&g_cv610_runner->send_queue);
	g_cv610_runner->output_uri = parsed;
	__atomic_fetch_add(&g_cv610_runner->transport_gen, 1, __ATOMIC_RELEASE);

	snprintf(g_cv610_runner->applied_server,
		sizeof(g_cv610_runner->applied_server), "%s", uri);

	/* Move the audio side channel with the video.  Its destination is derived
	 * from the video URI (peer host for udp://, loopback for the local
	 * transports), so leaving it behind pointed the Opus stream at the OLD
	 * receiver while video went to the new one -- silently, because nothing
	 * in the audio path knows the video URI changed.
	 *
	 * Not fatal: the video socket has already moved, and returning -1 here
	 * would send venc_api into a rollback that re-enters this function. A
	 * craft with video retargeted and audio stale is worse than one with
	 * video retargeted and a logged audio failure. */
	if (cv610_audio_apply_server(g_cv610_runner->audio,
		&g_cv610_runner->config, &parsed) != 0)
		fprintf(stderr, "WARN: video retargeted to %s but the audio side "
			"channel could not follow; audio is still going to the "
			"previous destination\n", uri);

	/* Deliberately NOT fatal past this point, and the reason matters: the
	 * socket has already moved.  Returning -1 sends venc_api into
	 * rollback_live_groups(), which re-enters this same call -- and if that
	 * also fails it commits the NEW outgoing.server while the socket sits
	 * on the OLD one, leaving /api/v1/config advertising a destination venc
	 * is not sending to.  Both SigmaStar backends made the same call. */
	if (cv610_request_idr() != 0)
		fprintf(stderr, "WARN: destination changed but the bootstrap IDR "
			"request failed; the new receiver has no start point "
			"until the next one\n");
	printf("> Destination changed to %s\n", uri);
	return 0;
}

/* Live output enable/disable.
 *
 * DIVERGENCE FROM STAR6E, deliberate: Star6E also drops the encoder to 5 fps
 * while the output is off, to stop burning bitrate on frames nobody receives.
 * CV610 cannot -- video0.fps is restart-only on this backend (it reprograms the
 * MIPI raw_bit and the RTP clock), so there is no live rate to idle to.  This
 * gates the send only; the encoder keeps running at its configured rate.
 *
 * Gates BOTH transports.  A disable that stopped udp:// but left the frame-shm
 * ring writing would make the field mean two different things depending on how
 * the craft happens to be configured. */
static int cv610_apply_output_enabled(bool on)
{
	if (!g_cv610_runner)
		return -1;

	if (g_cv610_runner->output_enabled == (on ? 1 : 0))
		return 0;

	if (!on) {
		__atomic_store_n(&g_cv610_runner->output_enabled, 0,
			__ATOMIC_RELEASE);
		printf("> Output disabled (encoder keeps running; video0.fps is "
			"restart-only on this backend, so there is no idle rate "
			"to drop to)\n");
		return 0;
	}

	/* Enabling needs a transport.  A craft that booted with
	 * outgoing.enabled=false never ran cv610_output_start()'s dispatch, so
	 * socket_handle is -1 and the RTP session was never seeded -- enabling
	 * without configuring would send from a closed fd with ssrc/seq 0. */
	if (g_cv610_runner->socket_handle < 0 && !g_cv610_runner->frame_ring) {
		if (!g_cv610_runner->config.outgoing.server[0]) {
			fprintf(stderr, "> Cannot enable output: no server "
				"configured\n");
			return -1;
		}
		if (cv610_apply_server(g_cv610_runner->config.outgoing.server) != 0)
			return -1;
	}

	__atomic_store_n(&g_cv610_runner->output_enabled, 1, __ATOMIC_RELEASE);
	if (cv610_request_idr() != 0)
		fprintf(stderr, "WARN: output enabled but the bootstrap IDR "
			"request failed; the receiver has no start point until "
			"the next one\n");
	printf("> Output enabled\n");
	return 0;
}

/* Thin adapters: the shared vtable takes bare values, and the AE-defaults
 * cache these three need lives in the runner context rather than at file scope
 * in cv610_iq.c.  g_cv610_runner is assigned before venc_api_register(), so it
 * is non-NULL for every call that can reach these. */
static int cv610_apply_gain_max(uint32_t gain)
{
	if (!g_cv610_runner)
		return -1;
	return cv610_iq_set_gain_max(&g_cv610_runner->ae_defaults, gain);
}

static int cv610_apply_shutter_max(uint32_t us)
{
	if (!g_cv610_runner)
		return -1;
	return cv610_iq_set_shutter_max_us(&g_cv610_runner->ae_defaults, us);
}

static int cv610_apply_isp_bin(const char *path)
{
	int rc;

	if (!g_cv610_runner)
		return -1;
	rc = cv610_pq_bin_import(path);
	/* A .bin carries an AE ext-register record, so a successful import makes
	 * the latched ceilings stale: without this, a later isp.gainMax=0 would
	 * write the PRE-import value back over the imported one. */
	if (rc == 0)
		cv610_iq_forget_ae_defaults(&g_cv610_runner->ae_defaults);
	return rc;
}

static const VencApplyCallbacks g_cv610_apply_callbacks = {
	.apply_bitrate = cv610_apply_bitrate,
	.apply_gop = cv610_apply_gop,
	.apply_verbose = cv610_apply_verbose,
	.request_idr = cv610_request_idr,
	.query_live_fps = cv610_query_live_fps,
	.apply_max_payload_size = cv610_apply_max_payload_size,
	.query_transport_status = cv610_query_transport_status,
	.query_audio_status = cv610_query_audio_status,
	.query_iq_info = cv610_iq_query,
	.query_awb_info = cv610_awb_query,
	.apply_iq_param = cv610_iq_set,
	.apply_qp_bounds = cv610_apply_qp_bounds,
	/* Unlike Star6E there is no /etc/sensors/<sensor>.bin convention to fall
	 * back to, so an empty isp.sensorBin is a no-op rather than a resolve. */
	.apply_isp_bin = cv610_apply_isp_bin,
	.export_isp_bin = cv610_pq_bin_export,
	.apply_gain_max = cv610_apply_gain_max,
	.apply_shutter_max = cv610_apply_shutter_max,
	.apply_roi_qp = cv610_apply_roi_qp,
	.apply_server = cv610_apply_server,
	.apply_output_enabled = cv610_apply_output_enabled,
	/* Same shared implementation Star6E and Maruko register; it dispatches
	 * to venc_jpeg_backend_set_quality() under the snapshot module lock, so
	 * a live q change cannot interleave with a capture in progress. Without
	 * it, /api/v1/capabilities advertises snapshot.quality supported while
	 * every write to it 501s. */
	.apply_snapshot_quality = venc_jpeg_set_quality,
};

static void cv610_signal_handler(int signo)
{
	(void)signo;
	cv610_pipeline_request_stop();
}

/* Seqlock read of the transport state, once per frame on the producer thread.
 * Retries while cv610_apply_server() holds an odd generation.  Mirrors the
 * reader in maruko_output_begin_frame(); yields rather than spinning, because
 * the writer is a short memcpy-class critical section on another thread and a
 * spin here burns the frame budget on a single-core SoC. */
static void cv610_transport_begin_frame(Cv610RunnerContext *ctx)
{
	unsigned gen_before, gen_after;

	for (;;) {
		gen_before = __atomic_load_n(&ctx->transport_gen,
			__ATOMIC_ACQUIRE);
		if (gen_before & 1u) {
			sched_yield();
			continue;
		}
		ctx->tx.socket_handle = ctx->socket_handle;
		ctx->tx.destination = ctx->destination;
		ctx->tx.destination_len = ctx->destination_len;
		ctx->tx.connected_udp = ctx->connected_udp;
		ctx->tx.output_enabled =
			__atomic_load_n(&ctx->output_enabled, __ATOMIC_ACQUIRE);
		gen_after = __atomic_load_n(&ctx->transport_gen,
			__ATOMIC_ACQUIRE);
		if (gen_before == gen_after)
			break;
	}
}

static int cv610_output_write(const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	Cv610RunnerContext *ctx = opaque;
	int ret;

	/* From the per-frame snapshot, never the live fields: every datagram of
	 * one frame must go to the same destination, or a retarget landing
	 * mid-frame splits an access unit across two receivers and neither can
	 * decode it.
	 *
	 * That holds only for the UNCONNECTED path.  With connectedUdp set --
	 * the shipped default -- msg_name is NULL and the destination is kernel
	 * state on the fd, so a concurrent connect() retargets mid-frame and the
	 * snapshot provides none of the protection this comment used to claim.
	 * See the seqlock note on transport_gen. */
	ret = output_socket_send_parts(ctx->tx.socket_handle,
		&ctx->tx.destination,
		ctx->tx.destination_len, ctx->tx.connected_udp, header, header_len,
		payload1, payload1_len, payload2, payload2_len);
	if (ret != 0) {
		__atomic_add_fetch(&ctx->output_drops, 1, __ATOMIC_RELAXED);
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)
			return 0;
	} else {
		__atomic_add_fetch(&ctx->packets_sent, 1, __ATOMIC_RELAXED);
	}
	return ret;
}

static int cv610_frame_is_idr(const uint8_t *frame, size_t len)
{
	size_t cursor = 0;
	const uint8_t *nal;
	size_t nal_len;

	while (h26x_util_annexb_next(frame, len, &cursor, &nal, &nal_len)) {
		uint8_t type = h26x_util_hevc_nalu_type(nal, nal_len);
		if (type == 19 || type == 20)
			return 1;
	}
	return 0;
}

static uint32_t cv610_frame_vcl_nal_count(const uint8_t *frame, size_t len)
{
	size_t cursor = 0;
	const uint8_t *nal;
	size_t nal_len;
	uint32_t count = 0;

	while (h26x_util_annexb_next(frame, len, &cursor, &nal, &nal_len)) {
		if (h26x_util_hevc_nalu_type(nal, nal_len) <= 31)
			count++;
	}
	return count;
}

static int cv610_send_rtp_frame(Cv610RunnerContext *ctx,
	const uint8_t *frame, size_t len)
{
	size_t cursor = 0;
	const uint8_t *nal;
	size_t nal_len;
	int nal_count = 0;
	int status = 0;
	uint16_t max_payload;

	max_payload = __atomic_load_n(&ctx->max_payload_size, __ATOMIC_ACQUIRE);

	while (h26x_util_annexb_next(frame, len, &cursor, &nal, &nal_len)) {
		uint8_t nal_type;
		int is_last;
		size_t sent;

		nal_type = h26x_util_hevc_nalu_type(nal, nal_len);
		nal_count++;
		h26x_param_sets_update(&ctx->param_sets, PT_H265, nal_type,
			nal, nal_len);
		is_last = cursor == len;
		if (nal_type == 19 || nal_type == 20)
			(void)hevc_rtp_prepend_param_sets(&ctx->param_sets, nal_type,
				&ctx->rtp, cv610_output_write, ctx,
				max_payload, &ctx->rtp_stats);
		sent = hevc_rtp_send_nal(nal, nal_len, &ctx->rtp,
			cv610_output_write, ctx, is_last,
			max_payload, &ctx->rtp_stats);
		if (sent != nal_len) {
			/* A fragmented NAL may fail after some FU packets were sent.
			 * Reserve the failed packet's sequence number so receivers see a
			 * loss gap, and never append later NALs to the incomplete AU. */
			ctx->rtp.seq++;
			status = -1;
			break;
		}
	}
	/* An output failure may drop this access unit, but time still advances. */
	ctx->rtp.timestamp += ctx->frame_ticks;
	return nal_count > 0 ? status : -1;
}

static int cv610_copy_stream(const ot_venc_stream *stream, uint8_t **out,
	size_t *out_len)
{
	size_t total = 0;
	size_t cursor = 0;
	uint8_t *frame;
	td_u32 i;

	for (i = 0; i < stream->pack_cnt; ++i) {
		if (stream->pack[i].len > stream->pack[i].offset)
			total += stream->pack[i].len - stream->pack[i].offset;
	}
	if (total == 0)
		return -1;
	frame = malloc(total);
	if (!frame)
		return -1;
	for (i = 0; i < stream->pack_cnt; ++i) {
		size_t chunk;

		if (stream->pack[i].len <= stream->pack[i].offset)
			continue;
		chunk = stream->pack[i].len - stream->pack[i].offset;
		memcpy(frame + cursor,
			stream->pack[i].addr + stream->pack[i].offset, chunk);
		cursor += chunk;
	}
	*out = frame;
	*out_len = total;
	return 0;
}

static int cv610_apply_encoder_config(Cv610RunnerContext *ctx,
	const Cv610EncoderConfig *enc)
{
	Cv610IntraRefreshStatus intra_status;
	Cv610RefPredStatus ref_status;
	ot_venc_intra_refresh intra;
	ot_venc_intra_refresh intra_rb;
	ot_venc_ref_param ref;
	ot_venc_ref_param ref_rb;
	ot_venc_slice_split split;
	ot_venc_slice_split split_rb;
	uint32_t delivered_slices = 1;
	uint32_t cycle_len = 0;
	td_s32 ret;

	if (!ctx || !enc)
		return -1;

	memset(&intra_status, 0, sizeof(intra_status));
	memset(&ref_status, 0, sizeof(ref_status));
	snprintf(intra_status.mode_name, sizeof(intra_status.mode_name), "%s",
		intra_refresh_mode_name(enc->intra.derived.mode));
	intra_status.mi_supported = 1;
	intra_status.target_ms = enc->intra.derived.target_ms;
	intra_status.total_rows = enc->intra.derived.total_rows;
	intra_status.requested_lines = ctx->config.video0.intra_refresh_lines;
	intra_status.effective_lines_per_p = enc->intra.refresh_num;
	intra_status.lines_clamped = enc->intra.derived.lines_clamped;
	intra_status.requested_qp = ctx->config.video0.intra_refresh_qp;
	intra_status.effective_qp = enc->intra.request_i_qp;
	intra_status.explicit_gop_sec = ctx->config.video0.gop_size;
	intra_status.effective_gop_sec = enc->intra.derived.gop_overridden
		? ctx->config.video0.gop_size : enc->intra.derived.gop_sec;
	intra_status.gop_auto = enc->intra.derived.gop_overridden
		? 0 : enc->intra.derived.gop_sec > 0.0;

	ref_status.mi_supported = 1;
	ref_status.base = enc->ref.base;
	ref_status.enhance = enc->ref.enhance;
	ref_status.pred = enc->ref.pred ? 1 : 0;

	ctx->gdr_active = 0;
	ctx->gdr_cycle_len = 0;
	ctx->gdr_counter = 0;
	ctx->svct_active = 0;
	ctx->delivered_slice_count = 1;
	ctx->slice_census_done = 0;
	ctx->requested_slice_count = enc->slice.requested_count;
	ctx->intra_refresh_active = enc->intra.enabled ? 1u : 0u;
	ctx->ref_census_frames = 0;
	memset(ctx->ref_type_counts, 0, sizeof(ctx->ref_type_counts));
	ctx->trail_n_patched = 0;

	/* Fresh channels default intra refresh off, but always write the enable
	 * bit so restart behavior does not depend on a future firmware default.
	 * Preserve the driver's inactive refresh/QP values when disabling because
	 * zero is outside some vendor revisions' accepted range. */
	memset(&intra, 0, sizeof(intra));
	ret = ss_mpi_venc_get_intra_refresh(CV610_VENC_CHN, &intra);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "ERROR: ss_mpi_venc_get_intra_refresh=0x%x\n",
			(unsigned)ret);
		goto fail;
	}
	intra.enable = enc->intra.enabled ? TD_TRUE : TD_FALSE;
	/* COLUMN, not ROW. Under ROW the intra band is a contiguous CTB range, and
	 * on the GDR recovery frame the encoder carves it off as its own I slice
	 * and then ignores the configured row split — measured on .181 as 2 slices
	 * (addresses [0,45]) instead of 17, on the largest frame in the stream and
	 * the one carrying VPS/SPS/PPS. A column band is not contiguous in raster
	 * order, so it cannot become a slice at all and the row split survives:
	 * 17/17 slices on every frame, refresh frame stays a P slice, and its size
	 * spike drops from 1.32x to 1.07x of the stream mean. waybeam-link's
	 * spatial repair drops any frame whose slice addresses are outside the
	 * learned geometry (core/src/spatial_repair.cpp:304), so under ROW the
	 * recovery frame was exactly the frame it could never salvage. */
	intra.mode = (ot_venc_intra_refresh_mode)enc->intra.refresh_dir;
	if (enc->intra.enabled) {
		intra.refresh_num = enc->intra.refresh_num;
		intra.request_i_qp = enc->intra.request_i_qp;
	}
	ret = ss_mpi_venc_set_intra_refresh(CV610_VENC_CHN, &intra);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "ERROR: ss_mpi_venc_set_intra_refresh=0x%x "
			"enable=%u rows=%u qp=%u\n", (unsigned)ret,
			(unsigned)intra.enable, (unsigned)intra.refresh_num,
			(unsigned)intra.request_i_qp);
		goto fail;
	}
	memset(&intra_rb, 0, sizeof(intra_rb));
	ret = ss_mpi_venc_get_intra_refresh(CV610_VENC_CHN, &intra_rb);
	if (ret != TD_SUCCESS || intra_rb.enable != intra.enable ||
	    (enc->intra.enabled &&
	     (intra_rb.mode != intra.mode ||
	      intra_rb.refresh_num != intra.refresh_num ||
	      intra_rb.request_i_qp != intra.request_i_qp))) {
		fprintf(stderr, "ERROR: CV610 intra-refresh readback mismatch "
			"ret=0x%x want=%u/%u/%u/%u got=%u/%u/%u/%u\n",
			(unsigned)ret, (unsigned)intra.enable, (unsigned)intra.mode,
			(unsigned)intra.refresh_num, (unsigned)intra.request_i_qp,
			(unsigned)intra_rb.enable, (unsigned)intra_rb.mode,
			(unsigned)intra_rb.refresh_num,
			(unsigned)intra_rb.request_i_qp);
		goto fail;
	}
	intra_status.apply_ok = 1;
	intra_status.active = enc->intra.enabled ? 1 : 0;
	intra_status.effective_lines_per_p = enc->intra.enabled
		? intra_rb.refresh_num : 0;
	intra_status.effective_qp = enc->intra.enabled
		? intra_rb.request_i_qp : 0;
	if (enc->intra.enabled && intra_rb.refresh_num > 0) {
		cycle_len = (enc->intra.derived.total_rows +
			intra_rb.refresh_num - 1u) / intra_rb.refresh_num;
		ctx->gdr_active = 1;
		ctx->gdr_cycle_len = cycle_len > 255u ? 255u : (uint8_t)cycle_len;
	}

	/* The disabled state is the fresh-channel default. Only program the
	 * reference cadence when a preset requested it, avoiding an invented
	 * "off" tuple for an API whose base field must be greater than zero. */
	memset(&ref_rb, 0, sizeof(ref_rb));
	ret = ss_mpi_venc_get_ref_param(CV610_VENC_CHN, &ref_rb);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "ERROR: ss_mpi_venc_get_ref_param=0x%x\n",
			(unsigned)ret);
		goto fail;
	}
	if (enc->ref.enabled) {
		memset(&ref, 0, sizeof(ref));
		ref.base = enc->ref.base;
		ref.enhance = enc->ref.enhance;
		ref.pred_en = enc->ref.pred ? TD_TRUE : TD_FALSE;
		ref.base_qp_delta_en = TD_FALSE;
		ref.base_qp_delta = 0;
		ret = ss_mpi_venc_set_ref_param(CV610_VENC_CHN, &ref);
		if (ret != TD_SUCCESS) {
			fprintf(stderr, "ERROR: ss_mpi_venc_set_ref_param=0x%x "
				"base=%u enhance=%u pred=%u\n", (unsigned)ret,
				(unsigned)ref.base, (unsigned)ref.enhance,
				(unsigned)ref.pred_en);
			goto fail;
		}
		memset(&ref_rb, 0, sizeof(ref_rb));
		ret = ss_mpi_venc_get_ref_param(CV610_VENC_CHN, &ref_rb);
		if (ret != TD_SUCCESS || ref_rb.base != ref.base ||
		    ref_rb.enhance != ref.enhance ||
		    ref_rb.pred_en != ref.pred_en ||
		    ref_rb.base_qp_delta_en != ref.base_qp_delta_en ||
		    ref_rb.base_qp_delta != ref.base_qp_delta) {
			fprintf(stderr, "ERROR: CV610 ref-param readback mismatch "
				"ret=0x%x want=%u/%u/%u/%u/%d "
				"got=%u/%u/%u/%u/%d\n",
				(unsigned)ret, (unsigned)ref.base,
				(unsigned)ref.enhance, (unsigned)ref.pred_en,
				(unsigned)ref.base_qp_delta_en,
				(int)ref.base_qp_delta,
				(unsigned)ref_rb.base, (unsigned)ref_rb.enhance,
				(unsigned)ref_rb.pred_en,
				(unsigned)ref_rb.base_qp_delta_en,
				(int)ref_rb.base_qp_delta);
			goto fail;
		}
		ref_status.active = 1;
		ctx->svct_active = 1;
	}
	ref_status.apply_ok = 1;

	/* split_mode=1 is the live firmware's row-split default. Early slice
	 * output remains off: this runtime publishes one whole access unit per
	 * GetStream result to both frame-SHM and RTP. */
	memset(&split, 0, sizeof(split));
	ret = ss_mpi_venc_get_slice_split(CV610_VENC_CHN, &split);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "ERROR: ss_mpi_venc_get_slice_split=0x%x\n",
			(unsigned)ret);
		goto fail;
	}
	split.enable = enc->slice.enabled ? TD_TRUE : TD_FALSE;
	split.slice_output_en = TD_FALSE;
	if (enc->slice.enabled) {
		split.split_mode = enc->slice.split_mode;
		split.split_size = enc->slice.split_size;
	}
	ret = ss_mpi_venc_set_slice_split(CV610_VENC_CHN, &split);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "ERROR: ss_mpi_venc_set_slice_split=0x%x "
			"enable=%u mode=%u size=%u\n", (unsigned)ret,
			(unsigned)split.enable, (unsigned)split.split_mode,
			(unsigned)split.split_size);
		goto fail;
	}
	memset(&split_rb, 0, sizeof(split_rb));
	ret = ss_mpi_venc_get_slice_split(CV610_VENC_CHN, &split_rb);
	if (ret != TD_SUCCESS || split_rb.enable != split.enable ||
	    split_rb.slice_output_en != TD_FALSE ||
	    (enc->slice.enabled &&
	     (split_rb.split_mode != split.split_mode ||
	      split_rb.split_size != split.split_size))) {
		fprintf(stderr, "ERROR: CV610 slice readback mismatch "
			"ret=0x%x want=%u/%u/%u/0 got=%u/%u/%u/%u\n",
			(unsigned)ret, (unsigned)split.enable,
			(unsigned)split.split_mode, (unsigned)split.split_size,
			(unsigned)split_rb.enable, (unsigned)split_rb.split_mode,
			(unsigned)split_rb.split_size,
			(unsigned)split_rb.slice_output_en);
		goto fail;
	}
	if (enc->slice.enabled && split_rb.split_size > 0)
		delivered_slices = (enc->slice.total_lcu_rows +
			split_rb.split_size - 1u) / split_rb.split_size;
	ctx->delivered_slice_count = delivered_slices > 255u
		? 255u : (uint8_t)delivered_slices;

	cv610_publish_encoder_status(&intra_status, &ref_status);
	fprintf(stderr, "[waybeam] CV610 resilience: intra=%s rows=%u qp=%u "
		"cycle=%u ref=%u/%u pred=%u\n", intra_status.mode_name,
		(unsigned)intra_status.effective_lines_per_p,
		(unsigned)intra_status.effective_qp, (unsigned)cycle_len,
		(unsigned)ref_status.base, (unsigned)ref_status.enhance,
		(unsigned)ref_status.pred);
	fprintf(stderr, "[waybeam] CV610 slices: requested=%u delivered=%u "
		"mode=%u lcu_rows_per_slice=%u early_output=0\n",
		(unsigned)enc->slice.requested_count, (unsigned)delivered_slices,
		(unsigned)split_rb.split_mode, (unsigned)split_rb.split_size);
	return 0;

fail:
	cv610_publish_encoder_status(&intra_status, &ref_status);
	return -1;
}

static int cv610_venc_start(Cv610RunnerContext *ctx)
{
	Cv610EncoderConfig enc;
	ot_venc_chn_attr attr;
	ot_venc_h265_vui vui;
	ot_venc_start_param start;
	ot_mpp_chn source;
	ot_mpp_chn destination;
	uint32_t gop;
	td_s32 ret;

	if (cv610_encoder_config_derive(&ctx->config, ctx->pipeline.out_width,
			ctx->pipeline.out_height,
		ctx->pipeline.fps, &enc) != 0) {
		fprintf(stderr, "ERROR: invalid CV610 encoder resilience/slice config\n");
		return -1;
	}
	gop = ctx->config.video0.gop_size <= 0.0 ? 1u :
		(uint32_t)(ctx->config.video0.gop_size * ctx->pipeline.fps + 0.5);
	if (enc.intra.enabled && !enc.intra.derived.gop_overridden &&
	    enc.intra.derived.gop_frames > 0)
		gop = enc.intra.derived.gop_frames;
	if (gop == 0)
		gop = 1;
	memset(&attr, 0, sizeof(attr));
	attr.venc_attr.type = OT_PT_H265;
	attr.venc_attr.max_pic_width = ctx->pipeline.out_width;
	attr.venc_attr.max_pic_height = ctx->pipeline.out_height;
	attr.venc_attr.buf_size =
		((ctx->pipeline.out_width * ctx->pipeline.out_height * 3 / 4) + 63) & ~63u;
	attr.venc_attr.profile = 0;
	attr.venc_attr.is_by_frame = TD_TRUE;
	attr.venc_attr.pic_width = ctx->pipeline.out_width;
	attr.venc_attr.pic_height = ctx->pipeline.out_height;
	attr.venc_attr.h265_attr.rcn_ref_share_buf_en = TD_TRUE;
	attr.venc_attr.h265_attr.frame_buf_ratio = 75;
	attr.rc_attr.rc_mode = OT_VENC_RC_MODE_H265_CBR;
	attr.rc_attr.h265_cbr.gop = gop;
	attr.rc_attr.h265_cbr.stats_time = 1;
	attr.rc_attr.h265_cbr.src_frame_rate = ctx->pipeline.fps;
	attr.rc_attr.h265_cbr.dst_frame_rate = ctx->pipeline.fps;
	attr.rc_attr.h265_cbr.bit_rate = ctx->config.video0.bitrate;
	attr.gop_attr.gop_mode = OT_VENC_GOP_MODE_NORMAL_P;
	/* normal_p.ip_qp_delta is deliberately left at 0.  The CBR rate
	 * controller stores it and ignores it -- measured across -12..+12, live
	 * and at create, with IDR size flat to <=1.2% -- so writing it bought
	 * nothing, while its narrower [-10, 30] range made a shared craft config
	 * carrying the portable qpDelta:-12 fail channel creation outright
	 * (ss_mpi_venc_create_chn=0xa0088007).  CV610's I-frame lever is
	 * intra refresh's request_i_qp, exposed as video0.intraRefreshQp. */

	ret = ss_mpi_venc_create_chn(CV610_VENC_CHN, &attr);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "ERROR: ss_mpi_venc_create_chn=0x%x\n", ret);
		return -1;
	}
	ctx->venc_created = 1;
	/* Seed from the API's own seconds->frames arithmetic — deliberately NOT the
	 * local `gop` above, and not a driver readback. This value exists only so
	 * cv610_apply_gop() can recognise the API replaying the value it believes is
	 * already in force (its rollback step) and accept it as a no-op, so it has
	 * to be computed the way the API computes it. `gop` above can diverge: it
	 * takes the intra-derived auto-GOP whenever a config enables intra refresh
	 * without pinning gop_size, and then the replay would be refused and report
	 * "rollback incomplete" against a channel nothing had touched. */
	ctx->applied_gop_frames = pipeline_common_gop_frames(
		ctx->config.video0.gop_size, ctx->pipeline.fps);
	if (cv610_apply_encoder_config(ctx, &enc) != 0)
		return -1;
	/* video0.minQp/maxQp are MUT_LIVE, but they also have to take effect on
	 * a cold boot -- the config is read before the channel exists, so the
	 * live path never runs for a value that was already in the file. */
	if (ctx->config.video0.min_qp || ctx->config.video0.max_qp) {
		/* Not (void): a rejected cold-boot apply would otherwise leave the
		 * operator booting with no QP bound and no indication. */
		if (cv610_apply_qp_bounds(ctx->config.video0.min_qp,
				ctx->config.video0.max_qp) != 0)
			fprintf(stderr, "WARN: qpBounds from config not applied "
				"(min=%u max=%u)\n",
				(unsigned)ctx->config.video0.min_qp,
				(unsigned)ctx->config.video0.max_qp);
	}
	memset(&vui, 0, sizeof(vui));
	ret = ss_mpi_venc_get_h265_vui(CV610_VENC_CHN, &vui);
	if (ret != TD_SUCCESS)
		return -1;
	vui.vui_time_info.timing_info_present_flag = 1;
	vui.vui_time_info.num_units_in_tick = 1000;
	vui.vui_time_info.time_scale = ctx->pipeline.fps * 1000;
	vui.vui_time_info.num_ticks_poc_diff_one_minus1 = 0;
	vui.vui_video_signal.video_signal_type_present_flag = 1;
	vui.vui_video_signal.video_format = 5;
	vui.vui_video_signal.video_full_range_flag = 1;
	vui.vui_video_signal.colour_description_present_flag = 1;
	vui.vui_video_signal.colour_primaries = 1;
	vui.vui_video_signal.transfer_characteristics = 1;
	vui.vui_video_signal.matrix_coefficients = 1;
	if (ss_mpi_venc_set_h265_vui(CV610_VENC_CHN, &vui) != TD_SUCCESS)
		return -1;
	/* Cap the bitstream buffer before encoding starts — see
	 * FRAME_GATE_STREAM_BUF_FRAMES.  Read-modify-write: chn_param also
	 * carries crop, frame rate and in_depth, none of which this owns.
	 * Advisory: a refusal leaves the SDK default depth, which costs
	 * latency while gated and nothing else. */
	{
		ot_venc_chn_param cp;

		memset(&cp, 0, sizeof(cp));
		if (ss_mpi_venc_get_chn_param(CV610_VENC_CHN, &cp)
		    == TD_SUCCESS) {
			cp.max_stream_cnt = FRAME_GATE_STREAM_BUF_FRAMES;
			if (ss_mpi_venc_set_chn_param(CV610_VENC_CHN, &cp)
			    != TD_SUCCESS)
				fprintf(stderr, "WARNING: [cv610] "
					"max_stream_cnt=%u refused; keeping "
					"the SDK default depth\n",
					FRAME_GATE_STREAM_BUF_FRAMES);
		} else {
			fprintf(stderr, "WARNING: [cv610] get_chn_param "
				"failed; keeping the SDK default depth\n");
		}
	}

	memset(&start, 0, sizeof(start));
	start.recv_pic_num = -1;
	if (ss_mpi_venc_start_chn(CV610_VENC_CHN, &start) != TD_SUCCESS)
		return -1;
	ctx->venc_started = 1;

	source.mod_id = OT_ID_VPSS;
	source.dev_id = CV610_VPSS_GRP;
	source.chn_id = CV610_VPSS_CHN;
	destination.mod_id = OT_ID_VENC;
	destination.dev_id = 0;
	destination.chn_id = CV610_VENC_CHN;
	if (ss_mpi_sys_bind(&source, &destination) != TD_SUCCESS)
		return -1;
	ctx->venc_bound = 1;

	/* JPEG snapshot: a second consumer on the source the main channel just
	 * bound, registered only now so it can never join a source that failed
	 * to come up.  Non-fatal — /api/v1/snapshot.jpg serves 503 if it does
	 * not initialise.  snapshot.width/height are not honoured here (the
	 * channel shares the main stream's VPSS output), which is why they are
	 * advertised unsupported for this backend. */
	{
		const VencConfigSnapshot *snap = &ctx->config.snapshot;
		VencJpegConfig jcfg = {
			.width   = ctx->pipeline.out_width,
			.height  = ctx->pipeline.out_height,
			.quality = snap->quality,
			.channel = snap->channel,
			.enabled = snap->enabled,
		};

		venc_jpeg_set_source(&source);
		(void)venc_jpeg_init(&jcfg);
	}

	printf("> CV610 H.265 %ux%u@%u CBR=%u kbit/s GOP=%.2fs/%uf\n",
		ctx->pipeline.out_width, ctx->pipeline.out_height, ctx->pipeline.fps,
		ctx->config.video0.bitrate, ctx->config.video0.gop_size, gop);
	return 0;
}

static void cv610_venc_stop(Cv610RunnerContext *ctx)
{
	ot_mpp_chn source = { OT_ID_VPSS, CV610_VPSS_GRP, CV610_VPSS_CHN };
	ot_mpp_chn destination = { OT_ID_VENC, 0, CV610_VENC_CHN };

	/* Before the source it is bound to goes away — the snapshot channel is
	 * the other consumer of `source`. */
	venc_jpeg_shutdown();

	if (ctx->venc_bound) {
		(void)ss_mpi_sys_unbind(&source, &destination);
		ctx->venc_bound = 0;
	}
	if (ctx->venc_started) {
		(void)ss_mpi_venc_stop_chn(CV610_VENC_CHN);
		ctx->venc_started = 0;
	}
	if (ctx->venc_created) {
		(void)ss_mpi_venc_destroy_chn(CV610_VENC_CHN);
		ctx->venc_created = 0;
	}
}

static int cv610_output_start(Cv610RunnerContext *ctx)
{
	RtpSessionState session;

	/* Before the outgoing.enabled bail and before the transport dispatch,
	 * on every path.  rtp_sidecar_sender_init() is what puts -1 in fd; the
	 * context arrives memset to zero, so an init that is skipped or that
	 * sits after an early return leaves fd == 0 — a perfectly valid
	 * descriptor — and the per-frame gate would poll() stdin. */
	if (rtp_sidecar_sender_init(&ctx->sidecar,
		ctx->config.outgoing.sidecar_port) != 0)
		fprintf(stderr, "WARNING: CV610 sidecar disabled (port %u)\n",
			(unsigned)ctx->config.outgoing.sidecar_port);

	ctx->output_enabled = ctx->config.outgoing.enabled ? 1 : 0;

	/* Seed the RTP session BEFORE the outgoing.enabled bail, not after.
	 * outgoing.enabled is live from 0.77.0, so a craft can boot disabled and
	 * be enabled over HTTP -- and the context is calloc'd, so bailing first
	 * left ssrc/seq/timestamp AND frame_ticks at 0.  Every packet then went
	 * out with ssrc 0 and a timestamp that never advanced (timestamp +=
	 * frame_ticks, with frame_ticks 0), which a depacketizer keying AU
	 * boundaries on the timestamp cannot follow.  Star6E seeds
	 * unconditionally at init and gates only the sends; this now matches. */
	if (!ctx->config.outgoing.enabled) {
		RtpSessionState idle;

		rtp_session_init(&idle, rtp_session_payload_type(PT_H265),
			ctx->pipeline.fps);
		ctx->rtp.seq = idle.seq;
		ctx->rtp.timestamp = idle.timestamp;
		ctx->rtp.ssrc = idle.ssrc;
		ctx->rtp.payload_type = idle.payload_type;
		ctx->frame_ticks = idle.frame_ticks;
		return 0;
	}

	/* Seed the RTP session for EVERY transport, not just the socket path.
	 * The SigmaStar backends gate this on the *stream mode* (rtp vs
	 * compact), not the transport — star6e_output_is_rtp() reads
	 * output->stream_mode, and the shipped default is "rtp" — so a Star6E
	 * craft on frame-shm:// still seeds ssrc/timestamp/seq and puts them on
	 * the sidecar wire.  Seeding only on the socket path made CV610 the one
	 * backend sending a zero ssrc there, which a consumer keying on
	 * "ssrc != 0 means session present" would read as a different craft
	 * state.  Nothing else consumes ctx->rtp or frame_ticks off the socket
	 * path, so this is inert for the ring transports beyond the sidecar. */
	rtp_session_init(&session, rtp_session_payload_type(PT_H265),
		ctx->pipeline.fps);
	ctx->rtp.seq = session.seq;
	ctx->rtp.timestamp = session.timestamp;
	ctx->rtp.ssrc = session.ssrc;
	ctx->rtp.payload_type = session.payload_type;
	ctx->frame_ticks = session.frame_ticks;

	/* Seed the applied-destination record for EVERY transport, before the
	 * dispatch below returns early on the ring paths.  Seeding it only on
	 * the socket path left a frame-shm:// craft with an empty record, so the
	 * no-op guard in cv610_apply_server() never matched and an identical
	 * re-POST of the URI already running fell through to the restart branch
	 * and respawned the craft -- measured on the bench.  No seqlock: the
	 * drain loop has not started yet. */
	snprintf(ctx->applied_server, sizeof(ctx->applied_server), "%s",
		ctx->config.outgoing.server);

	if (ctx->output_uri.type == VENC_OUTPUT_URI_FRAME_SHM) {
		ctx->frame_ring = venc_frame_ring_create(ctx->output_uri.endpoint,
			CV610_FRAME_RING_SLOTS, CV610_FRAME_RING_BYTES);
		return ctx->frame_ring ? 0 : -1;
	}
	if (ctx->output_uri.type == VENC_OUTPUT_URI_SHM) {
		fprintf(stderr, "ERROR: CV610 packet-shm output is not in the first bring-up slice; use frame-shm://\n");
		return -1;
	}
	/* Non-fatal at STARTUP, deliberately -- and the asymmetry with the live
	 * retarget path is the point.
	 *
	 * A destination that cannot be brought up used to abort the whole
	 * daemon: measured on a CV610 bench, a single bad `outgoing.server` in
	 * the config brought up sensor, ISP, VPSS, encoder, JPEG and sidecar and
	 * then tore all of it down, leaving no video AND no HTTP -- so the only
	 * way to correct a one-line config error was ssh, which a craft in the
	 * field does not have.  Coming up with the output inert leaves the API
	 * reachable, and `outgoing.server` is live-settable, so the operator can
	 * fix it remotely; output_socket_send_parts() already refuses a negative
	 * handle with EINVAL, so the frames are counted as drops rather than
	 * crashing anything.
	 *
	 * A live retarget still REFUSES a bad URI, because there the craft has a
	 * working output to lose.  Here it has none either way. */
	if (output_socket_configure(&ctx->socket_handle, &ctx->destination,
		&ctx->destination_len, &ctx->transport, &ctx->output_uri,
		ctx->config.outgoing.connected_udp,
		ctx->config.outgoing.allow_unix_encoder_stall,
		&ctx->connected_udp) != 0) {
		fprintf(stderr, "ERROR: outgoing.server %s could not be brought up; "
			"starting with NO video output so the craft stays reachable -- "
			"set outgoing.server over the API to recover\n",
			ctx->config.outgoing.server);
		/* The record was seeded above with a destination that never came up.
		 * Clearing it keeps the no-op guard in cv610_apply_server() from
		 * matching, so a re-POST of a corrected URI is not mistaken for a
		 * repeat of one already applied. */
		ctx->applied_server[0] = '\0';
		return 0;
	}
	(void)output_socket_capture_capacity(ctx->socket_handle, &ctx->send_queue);
	/* The RTP session was seeded above, for every transport; re-seeding here
	 * would re-randomise ssrc/seq/timestamp behind any reader added between
	 * the two points. */
	return 0;
}

static void cv610_output_stop(Cv610RunnerContext *ctx)
{
	/* Unconditional, like every other CV610 teardown: a restart-class set
	 * re-execs without reloading modules, so a port this process still
	 * holds is a port the successor cannot bind.  Safe when init never ran
	 * or was disabled — sender_close() tolerates fd == -1. */
	rtp_sidecar_sender_close(&ctx->sidecar);
	if (ctx->frame_ring) {
		venc_frame_ring_destroy(ctx->frame_ring);
		ctx->frame_ring = NULL;
	}
	if (ctx->socket_handle >= 0) {
		close(ctx->socket_handle);
		ctx->socket_handle = -1;
	}
}

static VencConfig *cv610_config(void *opaque)
{
	return &((Cv610RunnerContext *)opaque)->config;
}

static int cv610_prepare(void *opaque)
{
	Cv610RunnerContext *ctx = opaque;
	VencConfig *cfg = &ctx->config;
	const Cv610SensorMode *mode;
	int mode_index = -1;

	setvbuf(stdout, NULL, _IONBF, 0);
	/* Config loading and every staged HTTP mutation run the same lookup
	 * through cv610_validate_config(), so reaching this is a config file
	 * edited behind the daemon's back. */
	mode = cv610_mode_select(cfg->sensor.mode, cfg->video0.fps, &mode_index);
	if (mode == NULL || cv610_mode_check_output(mode, cfg->video0.width,
			cfg->video0.height) != NULL) {
		fprintf(stderr, "ERROR: CV610 cannot encode %ux%u @ %u fps\n",
			cfg->video0.width, cfg->video0.height, cfg->video0.fps);
		return 1;
	}
	/* The sensor runs at the MODE's rate.  Say so when that is not what was
	 * asked for — a forced sensor.mode and a substituted target both land
	 * here, and silence would leave every status endpoint reporting a rate
	 * the operator never chose. */
	if (mode->fps != cfg->video0.fps)
		printf("> Requested %u fps, using %u fps (sensor mode %d: %s)\n",
			cfg->video0.fps, mode->fps, mode_index, mode->desc);
	/* Publish what was actually selected, so /api/v1/modes reports the
	 * achieved mode rather than recomputing the configured one — the same
	 * contract star6e_pipeline.c fulfils after sensor_select(). */
	venc_api_set_sensor_info(0, mode_index, cfg->sensor.index);
	ctx->pipeline.width = mode->width;
	ctx->pipeline.height = mode->height;
	cv610_mode_resolve_output(mode, cfg->video0.width, cfg->video0.height,
		&ctx->pipeline.out_width, &ctx->pipeline.out_height);
	ctx->pipeline.keep_aspect = cfg->isp.keep_aspect ? 1 : 0;
	ctx->pipeline.fps = mode->fps;
	ctx->pipeline.lanes = 4;
	ctx->pipeline.data_rate_x2 = 0;
	ctx->pipeline.bayer = 0;
	ctx->pipeline.mirror = cfg->image.mirror ? 1 : 0;
	ctx->pipeline.flip = cfg->image.flip ? 1 : 0;
	ctx->pipeline.raw_bit = (int)mode->raw_bit;
	ctx->pipeline.sensor_clock_hz = mode->sensor_clock_hz;
	/* Match the standalone streamer's production graph. The CV610 module
	 * loader now provides the clean SYS/VB lifecycle required by online VI. */
	ctx->pipeline.vi_online = 1;
	ctx->pipeline.i2c_bus = 0;
	ctx->socket_handle = -1;
	ctx->live_bitrate = cfg->video0.bitrate;
	ctx->max_payload_size = cfg->outgoing.max_payload_size;
	ctx->verbose = cfg->system.verbose ? 1 : 0;
	if (cfg->outgoing.enabled &&
		venc_config_parse_output_uri(cfg->outgoing.server,
			&ctx->output_uri) != 0) {
		fprintf(stderr, "ERROR: invalid CV610 output URI: %s\n",
			cfg->outgoing.server);
		return 1;
	}
	printf("> CV610/IMX662 backend selected (initial streaming slice)\n");
	return 0;
}

static int cv610_init(void *opaque)
{
	Cv610RunnerContext *ctx = opaque;
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = cv610_signal_handler;
	sigemptyset(&action.sa_mask);
	(void)sigaction(SIGINT, &action, NULL);
	(void)sigaction(SIGTERM, &action, NULL);
	if (cv610_pipeline_start(&ctx->pipeline) != 0)
		return -1;
	if (cv610_venc_start(ctx) != 0)
		return -1;
	if (ctx->config.debug.show_osd) {
		ctx->debug_osd = debug_osd_create(ctx->pipeline.out_width,
			ctx->pipeline.out_height, NULL);
		if (!ctx->debug_osd)
			fprintf(stderr, "WARNING: CV610 debug OSD unavailable\n");
	}
	if (cv610_output_start(ctx) != 0)
		return -1;
	/* Adaptive frame gate.  Output setup must run first: it creates the
	 * frame-shm ring whose slot count enables the gate and validates the
	 * configured close threshold.  Initialising this in cv610_venc_start()
	 * made every CV610 frame-shm gate inert because ctx->frame_ring was
	 * necessarily still NULL there. */
	{
		venc_frame_ring_fill_t gfill = {0};

		if (ctx->frame_ring)
			(void)venc_frame_ring_get_fill(ctx->frame_ring, &gfill);
		cv610_report_frame_gate_setup(frame_gate_setup(&ctx->frame_gate,
			ctx->config.video0.frame_gate_close_slots,
			ctx->config.video0.frame_gate_max_closed_ms,
			gfill.slot_count), &ctx->frame_gate, gfill.slot_count);
	}
	if (ctx->config.audio.enabled) {
		/* Non-fatal, as on Star6E (star6e_pipeline.c discards the audio
		 * init result): audio needs kernel modules the loader only stages
		 * when CV610_AUDIO=1, and a daemon respawn cannot load them.  A
		 * config toggle must not be able to take video down with it.
		 * /api/v1/audio/status reports the running state, not the flag. */
		ctx->audio = cv610_audio_start(&ctx->config, &ctx->output_uri);
		/* No cause is guessed here. This line used to assert "needs
		 * CV610_AUDIO=1 at module load" for every failure, which sent the
		 * operator after the module set while the real cause was a
		 * predecessor's stale AI claim. Each failure point in
		 * cv610_audio_start() names itself, including the module-absent one. */
		if (!ctx->audio)
			fprintf(stderr,
				"WARNING: CV610 audio did not start; continuing without it "
				"(cause reported above)\n");
	}
	star6e_recorder_init(&ctx->recorder);
	pthread_mutex_init(&ctx->rec_writer_lock, NULL);
	audio_ring_init(&ctx->audio_ring);
	/* CV610 audio is fixed at 48 kHz mono Opus in cv610_audio.c; the rate
	 * and channel count come from the config only so a mismatch shows up
	 * as a bad TS rather than being silently papered over.  Zero rate ==
	 * "no audio in the mux". */
	star6e_ts_recorder_init(&ctx->ts_recorder,
		ctx->config.audio.enabled ? ctx->config.audio.sample_rate : 0,
		ctx->config.audio.enabled ? (uint8_t)ctx->config.audio.channels : 0,
		TS_AUDIO_CODEC_OPUS);
	/* AFTER all three — the mutex, star6e_recorder_init and
	 * ts_recorder_init.  A calloc'd ts_recorder has fd == 0, which
	 * cv610_record_stop() reads as an open file: setting the flag before
	 * the init leaves a window where a stop would fdatasync(0)/close(0),
	 * i.e. close STDIN.  No caller reaches it there today, but the flag
	 * means "the things I guard are initialised" and must not lie. */
	ctx->rec_locks_ready = 1;
	/* Both recorders: the rotation thresholds are the operator's,
	 * not the format's, and only one recorder is ever active. */
	if (ctx->config.record.max_seconds > 0) {
		ctx->ts_recorder.rot.max_seconds = ctx->config.record.max_seconds;
		ctx->recorder.rot.max_seconds = ctx->config.record.max_seconds;
	}
	if (ctx->config.record.max_mb > 0) {
		uint64_t max_bytes =
			(uint64_t)ctx->config.record.max_mb * 1024 * 1024;

		ctx->ts_recorder.rot.max_bytes = max_bytes;
		ctx->recorder.rot.max_bytes = max_bytes;
	}

	g_cv610_runner = ctx;
	/* A craft flashed without libbin.so cannot import or export a .bin, and
	 * /api/v1/capabilities is what a dashboard trusts instead of probing.
	 * routes.iq_export_bin tracks the callback pointer, so dropping the two
	 * entries is what makes the advertisement match reality -- the alternative
	 * is advertising a control surface whose every use returns an error.
	 * Static: venc_api_register keeps the pointer. */
	{
		static VencApplyCallbacks cv610_callbacks;

		cv610_callbacks = g_cv610_apply_callbacks;
		if (!cv610_pq_bin_available()) {
			cv610_callbacks.apply_isp_bin = NULL;
			cv610_callbacks.export_isp_bin = NULL;
		}
		if (venc_api_register(&ctx->config, "cv610",
			&cv610_callbacks, NULL) != 0)
			return -1;
	}
	venc_api_set_record_status_fn(cv610_record_status_callback);
	venc_api_set_record_http_control_supported(true);

	/* isp.sensorBin lands FIRST, and the order is load-bearing.  A .bin is a
	 * whole ISP image and carries an AE ext-register record of its own, so
	 * applying it after the two ceilings below would overwrite them on every
	 * boot while /api/v1/get kept reporting the config's values.  Broad image
	 * first, narrower per-knob intent on top.
	 *
	 * It needs a cold-boot apply for the reason the ceilings do: the ISP is
	 * seeded by the sensor plugin's compiled-in defaults, so a bin already
	 * named in the config would otherwise only take effect once someone
	 * re-wrote the field over HTTP.  Empty is the common case and costs
	 * nothing.  Non-fatal: a craft that boots on the plugin's tuning beats
	 * one that does not boot. */
	if (cv610_apply_isp_bin(ctx->config.isp.sensor_bin) != 0)
		fprintf(stderr, "WARN: isp.sensorBin from config not applied (%s)\n",
			ctx->config.isp.sensor_bin);

	/* isp.gain_max / isp.shutter_max_us are MUT_LIVE for the same reason
	 * fpv.roi* is, and need the same cold-boot apply: the ISP is seeded by
	 * the sensor plugin, which has never seen the config file.  A 0 here is
	 * "keep the plugin default", and cv610_iq_set_gain_max(0) captures that
	 * default and writes it back -- so this also fixes the snapshot before
	 * any live write can move the value it is supposed to restore. */
	if (cv610_apply_gain_max(ctx->config.isp.gain_max) != 0)
		fprintf(stderr, "WARN: isp.gainMax from config not applied (%u)\n",
			(unsigned)ctx->config.isp.gain_max);
	if (cv610_apply_shutter_max(ctx->config.isp.shutter_max_us) != 0)
		fprintf(stderr, "WARN: isp.shutterMaxUs from config not applied "
			"(%u)\n", (unsigned)ctx->config.isp.shutter_max_us);

	/* fpv.roi* are MUT_LIVE and must also take effect on a cold boot: the
	 * config is read before the channel exists, so the live path never runs
	 * for a value already in the file.  Placed here rather than beside the
	 * qpBounds cold-boot apply because cv610_apply_roi_qp() reads the config
	 * through g_cv610_runner, which is assigned just above -- calling it
	 * earlier would refuse with no channel and no config. */
	if (ctx->config.fpv.roi_enabled) {
		/* Not (void): a rejected apply would otherwise leave the operator
		 * booting with an ROI the dashboard reports as configured and the
		 * encoder never received. */
		if (cv610_apply_roi_qp(ctx->config.fpv.roi_qp) != 0)
			fprintf(stderr, "WARN: ROI from config not applied "
				"(qp=%+d steps=%u center=%.2f)\n",
				ctx->config.fpv.roi_qp,
				(unsigned)ctx->config.fpv.roi_steps,
				ctx->config.fpv.roi_center);
	}

	/* Auto-start.  Only "mirror" is implemented here: dual and dual-stream
	 * need a second VENC channel, deliberately deferred (docs/CV610_BACKEND.md).
	 * An unimplemented mode is refused loudly rather than silently recording
	 * the wrong channel. */
	if (ctx->config.record.enabled && ctx->config.record.dir[0]) {
		if (strcmp(ctx->config.record.mode, "mirror") == 0)
			cv610_record_start(ctx, ctx->config.record.dir);
		else if (strcmp(ctx->config.record.mode, "off") != 0)
			fprintf(stderr, "WARNING: record.mode=%s is not implemented "
				"on CV610; recording not started (mirror only)\n",
				ctx->config.record.mode);
	}
	if (venc_httpd_start(ctx->config.system.web_port) != 0)
		return -1;
	return 0;
}

static void cv610_report_frame_status(Cv610RunnerContext *ctx)
{
	uint64_t audio_frames = 0;
	uint64_t audio_bytes = 0;
	uint64_t audio_packets = 0;
	uint64_t audio_drops = 0;
	uint64_t frames;

	frames = __atomic_load_n(&ctx->frames, __ATOMIC_RELAXED);
	if (frames != 1 && frames % ctx->pipeline.fps != 0)
		return;
	cv610_audio_get_stats(ctx->audio, &audio_frames, &audio_bytes,
		&audio_packets, &audio_drops);
	if (ctx->debug_osd) {
		debug_osd_begin_frame(ctx->debug_osd);
		debug_osd_text(ctx->debug_osd, 0, "fps", "%u", ctx->pipeline.fps);
		debug_osd_text(ctx->debug_osd, 1, "cpu", "%d%%",
			debug_osd_get_cpu(ctx->debug_osd));
		debug_osd_text(ctx->debug_osd, 2, "enc", "%ux%u h265",
			ctx->pipeline.out_width, ctx->pipeline.out_height);
		debug_osd_text(ctx->debug_osd, 3, "br", "%uk",
			__atomic_load_n(&ctx->live_bitrate, __ATOMIC_ACQUIRE));
		debug_osd_text(ctx->debug_osd, 4, "drop", "%llu",
			(unsigned long long)__atomic_load_n(&ctx->output_drops,
				__ATOMIC_RELAXED));
		debug_osd_end_frame(ctx->debug_osd);
	}
	if (!__atomic_load_n(&ctx->verbose, __ATOMIC_ACQUIRE))
		return;
	printf("> CV610 frames=%llu bytes=%llu output_drops=%llu\n",
		(unsigned long long)frames,
		(unsigned long long)__atomic_load_n(&ctx->bytes, __ATOMIC_RELAXED),
		(unsigned long long)__atomic_load_n(&ctx->output_drops,
			__ATOMIC_RELAXED));
	if (ctx->audio)
		printf("> CV610 audio frames=%llu bytes=%llu packets=%llu drops=%llu\n",
			(unsigned long long)audio_frames,
			(unsigned long long)audio_bytes,
			(unsigned long long)audio_packets,
			(unsigned long long)audio_drops);
}

static int cv610_run(void *opaque)
{
	Cv610RunnerContext *ctx = opaque;
	int venc_fd = ss_mpi_venc_get_fd(CV610_VENC_CHN);

	if (venc_fd < 0)
		return -1;
	while (!cv610_pipeline_stop_requested()) {
		ot_venc_chn_status status;
		ot_venc_stream stream;
		fd_set readfds;
		/* 1 s normally; 2 ms while the gate is closed, where this
		 * timeout would otherwise BE the reopen latency. */
		struct timeval timeout =
			frame_gate_is_open(&ctx->frame_gate)
				? (struct timeval){ 1, 0 }
				: (struct timeval){ 0, 2000 };
		uint8_t *frame = NULL;
		size_t frame_len = 0;
		td_s32 ret;
		int ready;
	int select_errno;

		if (venc_api_get_reinit()) {
			venc_api_clear_reinit();
			venc_respawn_request();
			printf("> CV610 reinit requested: cold restart via fork+exec\n");
			break;
		}

		/* HTTP-driven record control.  Read both flags before acting so a
		 * start racing a stop cannot leave the request latched. */
		{
			char rec_dir[VENC_CONFIG_STRING_MAX];
			int start_pending = venc_api_get_record_start(rec_dir,
				sizeof(rec_dir));
			int stop_pending = venc_api_get_record_stop();

			if (stop_pending && !start_pending)
				cv610_record_stop(ctx, 1);
			if (start_pending) {
				if (!rec_dir[0])
					venc_api_get_record_dir(rec_dir,
						sizeof(rec_dir));
				if (strcmp(ctx->config.record.mode, "mirror") == 0 ||
				    ctx->config.record.mode[0] == '\0')
					cv610_record_start(ctx, rec_dir);
				else
					fprintf(stderr, "WARNING: record.mode=%s is not "
						"implemented on CV610; start ignored "
						"(mirror only)\n",
						ctx->config.record.mode);
			}
		}

		FD_ZERO(&readfds);
		FD_SET(venc_fd, &readfds);
		ready = select(venc_fd + 1, &readfds, NULL, NULL, &timeout);
		select_errno = errno;
		/* Service the sidecar here, ahead of every `continue` below:
		 * subscription and clock-sync traffic has to be answered while
		 * the encoder is producing nothing.  Polling only alongside a
		 * frame leaves a probe that attaches during a stall staring at
		 * a silent port — indistinguishable from a build with no
		 * sidecar at all, which is exactly the state an operator is
		 * trying to instrument.  Star6E has idle_wait() for this; this
		 * loop had no equivalent.  Note the saved errno: poll() runs
		 * recvfrom and would otherwise clobber select's EINTR. */
		if (ctx->sidecar.fd > 0)
			rtp_sidecar_poll(&ctx->sidecar);
		/* Reopen path.  While the gate is closed no frame ever
		 * arrives, so select() always times out and the per-frame
		 * call below never runs — this is the only thing that can
		 * let go again. */
		cv610_service_frame_gate(ctx);
		/* Gated: leave the encoded frame in VENC's output FIFO rather
		 * than draining it.  The gate is serviced above, off the ring's
		 * own occupancy, so the reopen path never depends on the
		 * drain. */
		/* A hard select() error is fatal whether or not the gate is
		 * closed, and it does NOT consume the timeout — taking the
		 * gated continue first would hot-spin until the escape. */
		if (ready < 0 && select_errno != EINTR)
			return -1;
		if (!frame_gate_is_open(&ctx->frame_gate)) {
			/* Gated: no frame is drained, but the ring's low-water
			 * gauge is published unconditionally, so it must keep
			 * being measured or it freezes for the whole closed
			 * interval — hiding exactly the congestion that closed
			 * the gate from waybeam-link's rate model.  Star6E and
			 * Maruko service their low-water gauge in the gated
			 * branch the same way.  Pass the reading through so the
			 * transport switch resets the window exactly as the
			 * per-frame path does when the ring read fails. */
			venc_frame_ring_fill_t gfill;
			const venc_frame_ring_fill_t *gfillp = NULL;

			if (ctx->frame_ring &&
			    venc_frame_ring_get_fill(ctx->frame_ring, &gfill) == 0)
				gfillp = &gfill;
			cv610_service_ring_low_water(ctx, gfillp);
			/* The pressure flag is deliberately NOT advanced here:
			 * venc_observe_pressure()'s counter counts frames in
			 * pressure, and both other backends leave it frozen while
			 * gated (their sampling is per-frame and, on Star6E,
			 * subscription-gated).  Advancing it on this ~500 Hz idle
			 * poll would inflate pressure_drops and diverge from the
			 * wire. */
			continue;
		}
		if (ready < 0)
			continue;
		if (ready == 0)
			continue;
		memset(&status, 0, sizeof(status));
		ret = ss_mpi_venc_query_status(CV610_VENC_CHN, &status);
		if (ret != TD_SUCCESS || status.cur_packs == 0)
			continue;
		memset(&stream, 0, sizeof(stream));
		stream.pack = calloc(status.cur_packs, sizeof(*stream.pack));
		if (!stream.pack)
			return -1;
		stream.pack_cnt = status.cur_packs;
		ret = ss_mpi_venc_get_stream(CV610_VENC_CHN, &stream, 1000);
		if (ret != TD_SUCCESS) {
			free(stream.pack);
			continue;
		}
		if (cv610_copy_stream(&stream, &frame, &frame_len) == 0) {
			int is_enhance = ctx->svct_active &&
				stream.h265_info.ref_type ==
					OT_VENC_ENHANCE_P_SLICE_NOT_FOR_REF;
			int is_idr;
			size_t patched = 0;
			/* Sidecar state for this frame; sc_fillp is the single
			 * ring reading shared by the low-water window and the
			 * TRANSPORT_INFO trailer. */
			venc_frame_ring_fill_t sc_fill;
			const venc_frame_ring_fill_t *sc_fillp = NULL;
			int sc_send;
			Cv610TransportSample sc_ts;
			int sc_have_ts = 0;
			uint32_t sc_rtp_ts = 0;
			uint16_t sc_seq_before = 0;
			uint64_t sc_capture_us = 0;
			uint64_t sc_ready_us = 0;

			if (is_enhance)
				patched = h26x_util_hevc_patch_trail_r_to_n(frame, frame_len);
			if (ctx->svct_active && ctx->ref_census_frames < 64u) {
				unsigned int ref_type = (unsigned int)stream.h265_info.ref_type;

				if (ref_type < OT_VENC_P_SLICE_BUTT)
					ctx->ref_type_counts[ref_type]++;
				ctx->trail_n_patched += (uint32_t)patched;
				ctx->ref_census_frames++;
				if (ctx->ref_census_frames == 64u) {
					fprintf(stderr, "[waybeam] CV610 ref census: frames=64 "
						"types=%u,%u,%u,%u,%u,%u trail_n_patched=%u\n",
						(unsigned)ctx->ref_type_counts[0],
						(unsigned)ctx->ref_type_counts[1],
						(unsigned)ctx->ref_type_counts[2],
						(unsigned)ctx->ref_type_counts[3],
						(unsigned)ctx->ref_type_counts[4],
						(unsigned)ctx->ref_type_counts[5],
						(unsigned)ctx->trail_n_patched);
				}
			}
			VencFrameMeta frame_meta;

			is_idr = cv610_frame_is_idr(frame, frame_len);
			/* Outside the sidecar gate on purpose: a counter that only
			 * advances while a probe is subscribed would report the
			 * frames since the subscription, not since the IDR. */
			if (is_idr)
				ctx->frames_since_idr = 0;
			else if (ctx->frames_since_idr < UINT16_MAX)
				ctx->frames_since_idr++;
			if (!ctx->slice_census_done &&
			    ctx->requested_slice_count > 1) {
				uint32_t vcl_nals = cv610_frame_vcl_nal_count(frame,
					frame_len);

				if (vcl_nals > 0) {
					fprintf(stderr, "[waybeam] CV610 slice census: "
						"requested=%u expected=%u actual_vcl_nals=%u\n",
						(unsigned)ctx->requested_slice_count,
						(unsigned)ctx->delivered_slice_count,
						(unsigned)vcl_nals);
					ctx->delivered_slice_count = vcl_nals > 255u
						? 255u : (uint8_t)vcl_nals;
					ctx->slice_census_done = 1;
				}
			}

			/* Sidecar (protocols/rtp-sidecar.md).  The socket itself
			 * is polled at the top of the loop so it stays responsive
			 * while no frames are produced; here we only decide
			 * whether to build a datagram.  fd > 0, not >= 0: the
			 * listener is always >= 3 and 0 means "never initialised",
			 * per rtp_sidecar_sender_close().  Gating on an actual
			 * subscriber spares an unsubscribed craft the clock read,
			 * the datagram assembly and up to four sendto calls; the
			 * ring fill is read every frame regardless, for the
			 * low-water window. */
			sc_send = 0;
			if (ctx->sidecar.fd > 0) {
				if (rtp_sidecar_is_subscribed(&ctx->sidecar)) {
					sc_send = 1;
					/* Snapshot before the write: the packetizer
					 * advances seq and timestamp as it sends. */
					sc_rtp_ts = ctx->rtp.timestamp;
					sc_seq_before = ctx->rtp.seq;
					sc_capture_us = stream.pack_cnt
						? cv610_capture_us_from_pts(ctx,
							(uint64_t)stream.pack[0].pts)
						: 0;
					sc_ready_us = wb_monotonic_us();
				}
			}

			/* One seqlock read per frame, before either transport is
			 * touched, so every datagram of this access unit uses
			 * the same destination and one enable state. */
			cv610_transport_begin_frame(ctx);

			/* GDR bookkeeping tracks the ENCODER's intra-refresh
			 * phase, so it advances every frame regardless of whether
			 * the frame is transmitted.  Skipping it while
			 * outgoing.enabled=false desynced meta.gdr_pos from the
			 * real wavefront for every frame after a re-enable, until
			 * the next IDR happened to resync it -- and the link
			 * consumer assigns slice importance from that position. */
			memset(&frame_meta, 0, sizeof(frame_meta));
			frame_meta.pts = stream.pack_cnt ?
				(uint32_t)stream.pack[0].pts : 0;
			frame_meta.codec = VENC_FRAME_CODEC_H265;
			frame_meta.flags = is_idr ? VENC_FRAME_FLAG_IDR : 0;
			if (is_enhance)
				frame_meta.flags |= VENC_FRAME_FLAG_ENHANCE;
			if (is_idr) {
				ctx->gdr_counter = 0;
			} else if (ctx->gdr_active && ctx->gdr_cycle_len > 0) {
				frame_meta.flags |= VENC_FRAME_FLAG_GDR;
				frame_meta.gdr_pos = ctx->gdr_counter;
				frame_meta.gdr_len = ctx->gdr_cycle_len;
				ctx->gdr_counter++;
				if (ctx->gdr_counter >= ctx->gdr_cycle_len)
					ctx->gdr_counter = 0;
			}

			if (ctx->frame_ring) {
				/* The ring's low-water gauge is published
				 * unconditionally, so it has to keep being
				 * measured even while nothing is written --
				 * otherwise it freezes at its last value and a
				 * consumer cannot tell a stale reading from a
				 * live one. */
				if (ctx->tx.output_enabled) {
					int write_ret = venc_frame_ring_write(
						ctx->frame_ring, &frame_meta,
						frame, (uint32_t)frame_len);
					if (write_ret != 0)
						__atomic_add_fetch(&ctx->output_drops,
							1, __ATOMIC_RELAXED);
				}
				if (venc_frame_ring_get_fill(ctx->frame_ring,
					&sc_fill) == 0)
					sc_fillp = &sc_fill;
				cv610_service_ring_low_water(ctx, sc_fillp);
			} else if (ctx->tx.output_enabled &&
				ctx->tx.socket_handle >= 0) {
				/* cv610_output_write owns per-datagram drop accounting. */
				(void)cv610_send_rtp_frame(ctx, frame, frame_len);
			}

			/* Maintain the latched pressure flag on the producer
			 * thread, every frame and whether or not anyone is
			 * subscribed — a counter that only advanced under a
			 * subscription would report frames since the subscription,
			 * and the flag's 75/50 hysteresis needs to see every
			 * sample to release correctly.  Same shared helper the
			 * SigmaStar backends use, so the wire means one thing. */
			/* have_fill, not just active: an unmeasured queue must not be
			 * observed as an empty one. */
			if (cv610_collect_transport(ctx, sc_fillp, &sc_ts) == 0 &&
				sc_ts.active && sc_ts.have_fill) {
				int p_state = __atomic_load_n(&ctx->pressure_state,
					__ATOMIC_RELAXED);
				uint32_t p_frames = __atomic_load_n(
					&ctx->pressure_frames, __ATOMIC_RELAXED);

				venc_observe_pressure(sc_ts.fill_pct, &p_state,
					&p_frames);
				/* Single writer (this thread); relaxed is enough, and
				 * it is what Star6E publishes this state with. */
				__atomic_store_n(&ctx->pressure_state, p_state,
					__ATOMIC_RELAXED);
				__atomic_store_n(&ctx->pressure_frames, p_frames,
					__ATOMIC_RELAXED);
				/* Fold this frame's observation back into the sample
				 * the trailer will use, so fill_pct, in_pressure and
				 * the counter all describe ONE observation.  Sampling
				 * twice let a trailer carry fill_pct=10 beside
				 * in_pressure=1 on the socket transports, where each
				 * call is its own SIOCOUTQ. */
				sc_ts.in_pressure = p_state;
				sc_ts.pressure_frames = p_frames;
				sc_have_ts = 1;
			}

			if (sc_send) {
				RtpSidecarEncInfo enc;
				RtpSidecarTransportInfo tinfo;
				const RtpSidecarTransportInfo *tinfo_ptr = NULL;

				/* Only what CV610 genuinely knows at this point.
				 * complexity and scene_change stay 0: both come from
				 * the shared scene detector, which this backend does
				 * not compile in.  gop_state stays 0 because
				 * nothing in this tree ever writes it — no backend
				 * produces it, so this is not a CV610 gap.  Note the
				 * spec reserves no "absent" sentinel for these, so a
				 * consumer cannot tell a real 0 from an unproduced
				 * one; see the follow-up on protocols/rtp-sidecar.md. */
				memset(&enc, 0, sizeof(enc));
				enc.frame_size_bytes = (uint32_t)frame_len;
				enc.frame_type = is_idr ? RTP_SIDECAR_FRAME_IDR
					: RTP_SIDECAR_FRAME_P;
				/* Left 0 deliberately, like complexity/scene_change/
				 * gop_state just below: the field means "the controller
				 * REQUESTED an IDR after this frame", and this backend has
				 * no such controller.  Setting it on every IDR made it a
				 * duplicate of frame_type and FLAG_KEYFRAME, and a ground
				 * consumer that sums it as "IDR insertions" would read a
				 * once-per-GOP static scene as one insertion per second
				 * while an identically configured SigmaStar craft reports
				 * zero. */
				enc.idr_inserted = 0;
				/* start_qp is the encoder's own per-frame value and is
				 * exactly what the contract asks for ("start QP /
				 * closest available per-frame QP").  The struct is
				 * already in hand -- the SVC-T check above reads
				 * h265_info.ref_type from it. */
				enc.qp = stream.h265_info.start_qp > 255u
					? 255u : (uint8_t)stream.h265_info.start_qp;
				enc.frames_since_idr = ctx->frames_since_idr;

				if (sc_have_ts) {
					const Cv610TransportSample *ts = &sc_ts;

					memset(&tinfo, 0, sizeof(tinfo));
					tinfo.fill_pct = ts->fill_pct;
					tinfo.in_pressure = ts->in_pressure ? 1 : 0;
					tinfo.transport_drops = (uint32_t)ts->transport_drops;
					/* Despite the name this is "frames observed in
					 * pressure" on every backend — see venc_ring.h;
					 * the wire name is kept for ABI stability across
					 * the v0.9.2 frame-skip rollback and no backend
					 * has ever dropped for pressure. */
					tinfo.pressure_drops = ts->pressure_frames;
					tinfo.packets_sent = (uint32_t)ts->packets_sent;
					tinfo_ptr = &tinfo;
				}

				/* Under frame-shm there is no RTP session, so ssrc,
				 * timestamp and both seq fields are 0 — the same
				 * thing Star6E puts on the wire for a non-RTP
				 * transport, where rtp_session_init() is skipped.
				 * The information lives in the trailer instead. */
				rtp_sidecar_send_frame_transport(&ctx->sidecar,
					ctx->rtp.ssrc, sc_rtp_ts, sc_seq_before,
					(uint16_t)(ctx->rtp.seq - sc_seq_before),
					sc_capture_us, sc_ready_us, &enc, tinfo_ptr);
			}
			__atomic_add_fetch(&ctx->frames, 1, __ATOMIC_RELAXED);
			__atomic_add_fetch(&ctx->bytes, frame_len, __ATOMIC_RELAXED);
			/* Beside the frame/byte counters, NOT inside the sidecar
			 * block above: that one is gated on a live subscriber, so
			 * putting the watch there made it observe only while a probe
			 * happened to be attached -- measured on .181, an overrun
			 * that delivered 12x its target logged nothing with no probe
			 * running. */
			pipeline_common_rate_watch(&ctx->rate_watch, &ctx->config,
				(uint32_t)frame_len, wb_monotonic_us());
			if (ctx->debug_osd)
				debug_osd_sample_cpu(ctx->debug_osd);
			cv610_report_frame_status(ctx);

			/* Mirror-mode recording: hand the SAME buffer the
			 * transport just used to the writer thread and let it
			 * block on the disk instead of this loop.  Ownership
			 * transfers, so there is no extra copy — cv610_copy_stream
			 * already produced the contiguous access unit, and push()
			 * frees it whether it is queued or dropped.
			 *
			 * This has to stay off the drain loop: a blocking write
			 * here delays ss_mpi_venc_release_stream() below, which
			 * backs up the encoder's output queue and stalls the LIVE
			 * transport, not just the recording. */
			/* A recorder that stopped ITSELF (disk full, write
			 * error) is torn down AFTER the release below, not
			 * here — see that site for why. */
			if (ctx->rec_writer) {
				struct timespec rec_now;

				clock_gettime(CLOCK_MONOTONIC, &rec_now);
				(void)venc_rec_writer_push(ctx->rec_writer,
					frame, frame_len,
					ts_mux_timespec_to_pts(
						(uint32_t)rec_now.tv_sec,
						(uint32_t)rec_now.tv_nsec),
					is_idr);
				frame = NULL;  /* the writer owns it now */
			}
			free(frame);
		}
		ret = ss_mpi_venc_release_stream(CV610_VENC_CHN, &stream);
		free(stream.pack);

		/* Keep the gate transition outside the acquired-stream lifetime on
		 * every backend.  CV610 tolerated stop_chn before release, but the
		 * shared ordering avoids depending on that vendor-specific accident. */
		cv610_service_frame_gate(ctx);

		/* A recorder that stopped ITSELF (disk full, write error) does
		 * so on the writer thread, so nothing but this loop is
		 * positioned to notice, and the gate above would go on
		 * queueing into a dead writer indefinitely.
		 *
		 * AFTER the release: the stop is bounded but not free, and
		 * before the release it would hold the encoder's output slot
		 * and stall the LIVE transport — the coupling this writer
		 * exists to remove.  Since 0.73.2 the stop no longer ends in an
		 * unbounded join either: past its deadline the writer goes to a
		 * detached reaper, so a medium that has stopped completing
		 * cannot park this loop. */
		if (ctx->rec_writer &&
		    !star6e_record_wants_frame(&ctx->ts_recorder,
				&ctx->recorder))
			cv610_record_stop(ctx, 1);

		if (ret != TD_SUCCESS)
			return -1;
	}
	return 0;
}

static void cv610_teardown(void *opaque)
{
	Cv610RunnerContext *ctx = opaque;

	venc_httpd_pause();
	venc_httpd_stop();
	g_cv610_runner = NULL;
	/* Before cv610_audio_stop(): the tee points into ctx->audio_ring, and
	 * the recorder is the only reader of it. */
	/* BOUNDED, even at teardown.  An unbounded stop drains the whole queue
	 * against the disk before the join, and CV610's medium can stall or
	 * vanish under load — a hang here never reaches the VENC/VPSS teardown
	 * below, which is exactly the leak the comment beneath describes, and
	 * this backend has no watchdog to cut it short.  Losing the tail of a
	 * recording on shutdown is the correct trade.  SYNCHRONOUS (async=0): the
	 * process is going away, so there is no later for a reaper to run in, and
	 * the VENC/VPSS teardown below must not race a writer still holding a
	 * descriptor. */
	cv610_record_stop(ctx, 0);
	cv610_audio_stop(ctx->audio);
	ctx->audio = NULL;
	/* AFTER cv610_audio_stop(), which is what joins the capture thread.
	 * Clearing the tee above is an atomic store and does not synchronise
	 * with a thread that already loaded a non-NULL ring, so destroying the
	 * mutex first can leave that thread locking freed primitives — and a
	 * hang there never reaches the VENC/VPSS teardown below, leaking
	 * kernel-state channels and binds that make the next start fail with
	 * EXIST.  star6e_runtime.c already orders it this way. */
	audio_ring_destroy(&ctx->audio_ring);
	if (ctx->rec_locks_ready) {
		pthread_mutex_destroy(&ctx->rec_writer_lock);
		ctx->rec_locks_ready = 0;
	}
	cv610_output_stop(ctx);
	debug_osd_destroy(ctx->debug_osd);
	ctx->debug_osd = NULL;
	cv610_venc_stop(ctx);
	cv610_pipeline_stop();
}

static int cv610_map_result(int result)
{
	return result == 0 ? 0 : 2;
}

static const BackendOps g_cv610_ops = {
	.name = "cv610",
	.context_size = sizeof(Cv610RunnerContext),
	.config = cv610_config,
	.prepare = cv610_prepare,
	.init = cv610_init,
	.run = cv610_run,
	.teardown = cv610_teardown,
	.map_pipeline_result = cv610_map_result,
};

const BackendOps *cv610_runtime_backend_ops(void)
{
	return &g_cv610_ops;
}
