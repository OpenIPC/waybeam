#include "star6e_runtime.h"
#include "star6e_luma_tap.h"

#include "attitude_est.h"
#include "audio_codec.h"
#include "debug_osd.h"
#include "detect_wire.h"
#include "idr_rate_limit.h"
#include "imu_bmi270.h"
#include "imu_ring.h"
#include "star6e_framing_host.h"
#include "pipeline_common.h"
#include "scene_detector.h"
#include "sdk_quiet.h"
#include "star6e_controls.h"
#include "star6e_cus3a.h"
#if HAVE_FRAMING_STAB
#include "star6e_framing_stab.h"
#endif
#include "star6e_ipu.h"
#include "star6e_ipu_yolo.h"
#include "star6e_iq.h"
#include "star6e_pipeline.h"
#include "star6e.h"
#include "timing.h"
#include "venc_api.h"
#include "venc_config.h"
#include "venc_httpd.h"
#include "venc_respawn.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static SdkQuietState g_sdk_quiet = SDK_QUIET_STATE_INIT;

static MI_VENC_Pack_t *ensure_packs(MI_VENC_Pack_t **buf,
	uint32_t *cap, uint32_t need)
{
	if (need <= *cap)
		return *buf;
	free(*buf);
	*buf = malloc(need * sizeof(MI_VENC_Pack_t));
	*cap = *buf ? need : 0;
	return *buf;
}

/* Forward declaration — record status callback for HTTP API */
static void record_status_callback(VencRecordStatus *out);
/* Forward declaration — mirror_record_open() starts a recording on a
 * keyframe, and is defined above the IDR helpers it needs. */
static int runtime_request_idr(void);

/*
 * Sleep for up to timeout_ms, but wake early to service the sidecar fd
 * (sync responses need low latency).  Falls back to usleep when the
 * sidecar is disabled (fd < 0).
 */
static void idle_wait(RtpSidecarSender *sc, int timeout_ms)
{
	if (!sc || sc->fd < 0) {
		usleep((unsigned)(timeout_ms * 1000));
		return;
	}
	struct pollfd pfd = { .fd = sc->fd, .events = POLLIN };
	if (poll(&pfd, 1, timeout_ms) > 0)
		rtp_sidecar_poll(sc);
}

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_signal_count = 0;
static struct timespec g_imu_verbose_last = {0};

/* ── Attitude export (sidecar ATTITUDE trailer) ───────────────────────────
 *
 * The frame loop has just called imu_drain(), so the pipeline's IMU ring
 * holds everything up to "now". Feed the new samples into the
 * complementary filter and snapshot the attitude for this frame's sidecar
 * trailer. File-scope state is fine here: the Star6E respawn path is a
 * full re-exec (fresh process), and only the frame thread touches it. */
static AttitudeEst g_att_est;
static int g_att_inited;
static struct timespec g_att_cursor;   /* last consumed sample timestamp */
static AttitudeAxisMap g_att_map;      /* sensor→camera signed permutation */

/* Live snapshot for /api/v1/attitude (HTTP thread reads, frame thread
 * writes) and the level-trim calibration accumulator (HTTP thread arms,
 * frame thread fills, HTTP thread consumes). */
static pthread_mutex_t g_att_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	int valid, settled;
	float roll_deg, pitch_deg, yaw_deg;
} g_att_snap;
enum { ATT_CAL_IDLE = 0, ATT_CAL_PENDING, ATT_CAL_DONE };
#define ATT_CAL_SAMPLES     256u  /* early-complete target (~1.3 s @200 Hz) */
#define ATT_CAL_MIN_SAMPLES  32u  /* enough to average even at low IMU ODR */
static struct {
	int state;
	float sx, sy, sz;   /* raw sensor-frame accel sums */
	uint32_t n;
} g_att_cal;

static int16_t att_clamp16(int v)
{
	if (v > 32767) return 32767;
	if (v < -32768) return -32768;
	return (int16_t)v;
}

static const RtpSidecarAttitudeInfo *attitude_frame_update(
	const VencConfig *vcfg, RtpSidecarAttitudeInfo *out)
{
	ImuRing *ring = star6e_pipeline_imu_ring();
	struct timespec now;

	if (!ring)
		return NULL;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (!g_att_inited) {
		attitude_est_init(&g_att_est, 0.0f);
		if (attitude_axis_map_init(&g_att_map,
		                           vcfg->attitude.axis_fwd,
		                           vcfg->attitude.axis_down,
		                           vcfg->attitude.trim_roll_deg,
		                           vcfg->attitude.trim_pitch_deg) != 0)
			fprintf(stderr, "attitude: invalid axisFwd/axisDown "
			        "(\"%s\"/\"%s\") — using identity\n",
			        vcfg->attitude.axis_fwd,
			        vcfg->attitude.axis_down);
		g_att_cursor = now;
		g_att_inited = 1;
	}

	ImuRingSample s[256];
	uint32_t n = imu_ring_read_range(ring, g_att_cursor, now, s, 256);
	for (uint32_t i = 0; i < n; i++) {
		uint64_t ts_us = (uint64_t)s[i].ts.tv_sec * 1000000ULL +
		                 (uint64_t)s[i].ts.tv_nsec / 1000ULL;
		float g[3], a[3];
		attitude_axis_map_apply(&g_att_map,
			s[i].gyro_x, s[i].gyro_y, s[i].gyro_z, g);
		attitude_axis_map_apply(&g_att_map,
			s[i].accel_x, s[i].accel_y, s[i].accel_z, a);
		attitude_est_update(&g_att_est,
			g[0], g[1], g[2], a[0], a[1], a[2], ts_us);
	}
	if (n > 0) {
		pthread_mutex_lock(&g_att_lock);
		if (g_att_cal.state == ATT_CAL_PENDING) {
			for (uint32_t i = 0; i < n &&
			     g_att_cal.n < ATT_CAL_SAMPLES; i++) {
				g_att_cal.sx += s[i].accel_x;
				g_att_cal.sy += s[i].accel_y;
				g_att_cal.sz += s[i].accel_z;
				g_att_cal.n++;
			}
			if (g_att_cal.n >= ATT_CAL_SAMPLES)
				g_att_cal.state = ATT_CAL_DONE;
		}
		pthread_mutex_unlock(&g_att_lock);
	}
	if (n > 0) {
		/* Advance past the newest consumed sample; read_range is
		 * inclusive and attitude_est_update drops dt<=0 duplicates,
		 * so a 1 ns bump avoids re-feeding the boundary sample. */
		g_att_cursor = s[n - 1].ts;
		g_att_cursor.tv_nsec++;
		if (g_att_cursor.tv_nsec >= 1000000000L) {
			g_att_cursor.tv_nsec = 0;
			g_att_cursor.tv_sec++;
		}
	}

	if (!attitude_est_healthy(&g_att_est))
		return NULL;   /* no gravity reference yet, or state went
		                * non-finite — emit no trailer rather than a
		                * false-valid level attitude */

	int roll = attitude_est_roll_cdeg(&g_att_est);
	int pitch = attitude_est_pitch_cdeg(&g_att_est);
	int yaw = attitude_est_yaw_cdeg(&g_att_est);

	/* Mount rotation about the camera axis, then sign trims. */
	switch (vcfg->attitude.mount_deg) {
	case 90:  { int t = roll; roll = pitch; pitch = -t; } break;
	case 180: roll = -roll; pitch = -pitch; break;
	case 270: { int t = roll; roll = -pitch; pitch = t; } break;
	default: break;
	}
	if (vcfg->attitude.invert_roll)  roll = -roll;
	if (vcfg->attitude.invert_pitch) pitch = -pitch;

	long age_ms = (now.tv_sec - g_att_cursor.tv_sec) * 1000L +
	              (now.tv_nsec - g_att_cursor.tv_nsec) / 1000000L;
	if (age_ms < 0) age_ms = 0;
	if (age_ms > 65535) age_ms = 65535;

	out->roll_cdeg  = att_clamp16(roll);
	out->pitch_cdeg = att_clamp16(pitch);
	out->yaw_cdeg   = att_clamp16(yaw);
	out->status     = RTP_SIDECAR_ATT_VALID |
	                  (attitude_est_settled(&g_att_est)
	                   ? RTP_SIDECAR_ATT_SETTLED : 0);
	out->imu_age_ms = (uint16_t)age_ms;

	pthread_mutex_lock(&g_att_lock);
	g_att_snap.valid = 1;
	g_att_snap.settled = attitude_est_settled(&g_att_est);
	g_att_snap.roll_deg  = (float)roll  * 0.1f;
	g_att_snap.pitch_deg = (float)pitch * 0.1f;
	g_att_snap.yaw_deg   = (float)yaw   * 0.1f;
	pthread_mutex_unlock(&g_att_lock);
	return out;
}

/* ── Attitude HTTP hooks (called from the httpd thread) ─────────────────── */

/* Live attitude as malloc'd JSON for GET /api/v1/attitude. */
char *star6e_attitude_query(void)
{
	char *buf = malloc(192);
	if (!buf)
		return NULL;
	pthread_mutex_lock(&g_att_lock);
	if (!g_att_snap.valid) {
		snprintf(buf, 192, "{\"valid\":false}");
	} else {
		snprintf(buf, 192,
			"{\"valid\":true,\"settled\":%s,"
			"\"rollDeg\":%.1f,\"pitchDeg\":%.1f,\"yawDeg\":%.1f}",
			g_att_snap.settled ? "true" : "false",
			(double)g_att_snap.roll_deg,
			(double)g_att_snap.pitch_deg,
			(double)g_att_snap.yaw_deg);
	}
	pthread_mutex_unlock(&g_att_lock);
	return buf;
}

/* Level-trim calibration: arm the accumulator, wait for the frame loop
 * to fill it, solve the boresight trims. Completes early once
 * ATT_CAL_SAMPLES are in (~1.3 s @200 Hz); on the ≤2 s window it accepts
 * whatever arrived as long as ≥ ATT_CAL_MIN_SAMPLES — so calibration works
 * at any IMU ODR instead of always timing out below ~85 Hz. Blocking;
 * returns 0 on success, -1 on too few samples (attitude/IMU not running)
 * or an implausible gravity magnitude (device moving / free-fall). */
int star6e_attitude_calibrate_level(const VencConfig *vcfg,
	float *roll_deg, float *pitch_deg)
{
	float ax, ay, az;
	uint32_t cnt;
	int done = 0;

	pthread_mutex_lock(&g_att_lock);
	memset(&g_att_cal, 0, sizeof(g_att_cal));
	g_att_cal.state = ATT_CAL_PENDING;
	pthread_mutex_unlock(&g_att_lock);

	for (int i = 0; i < 40 && !done; i++) {   /* ≤2 s window */
		usleep(50 * 1000);
		pthread_mutex_lock(&g_att_lock);
		done = (g_att_cal.state == ATT_CAL_DONE);
		pthread_mutex_unlock(&g_att_lock);
	}

	pthread_mutex_lock(&g_att_lock);
	cnt = g_att_cal.n;
	ax = g_att_cal.sx; ay = g_att_cal.sy; az = g_att_cal.sz;
	g_att_cal.state = ATT_CAL_IDLE;
	pthread_mutex_unlock(&g_att_lock);

	if (cnt < ATT_CAL_MIN_SAMPLES)
		return -1;
	return attitude_axis_map_solve_trims(vcfg->attitude.axis_fwd,
		vcfg->attitude.axis_down, ax / (float)cnt, ay / (float)cnt,
		az / (float)cnt, roll_deg, pitch_deg);
}

/* ── Scene-detector stream decoders ───────────────────────────────────── */

static uint32_t star6e_scene_frame_size(const MI_VENC_Stream_t *s)
{
	uint32_t t = 0;
	unsigned int i;
	if (!s || !s->packet) return 0;
	for (i = 0; i < s->count; i++) t += s->packet[i].length;
	return t;
}

/* HEVC NAL types relevant for non-reference rewriting */
#define HEVC_NAL_TRAIL_N 0
#define HEVC_NAL_TRAIL_R 1
/* STAR6E_REFTYPE_ENHANCE_P_NOTFORREF (=5) is defined in star6e.h. Was locally
 * 4 (HiSilicon value) — wrong for the SigmaStar enum, so the TRAIL_N rewrite
 * marked referenced-enhance frames (or nothing under shallow SVC-T). */

/* Locate the NAL header byte 0 inside a payload buffer that may or may not
 * begin with a start-code prefix (00 00 01 / 00 00 00 01).  Returns the
 * index of NAL byte 0, or len on failure. */
static size_t star6e_nal_header_idx(const uint8_t *buf, size_t len)
{
	size_t i = 0;
	while (i < len && buf[i] == 0) i++;
	if (i < len && buf[i] == 0x01) i++;
	return i < len ? i : len;
}

/* If a NAL is TRAIL_R (type 1) and the SDK marked this frame as
 * ENHANCE_P_NOTFORREF, rewrite the NAL header to TRAIL_N (type 0).
 *
 * Byte 0 bit layout: forbidden_zero(1) | nal_unit_type(6) | layer_id_msb(1)
 *   TRAIL_R = 0x02   (type=1, layer_msb=0)
 *   TRAIL_N = 0x00   (type=0, layer_msb=0)
 *
 * No-op if NAL layer_id_msb != 0 (we only touch single-layer streams), if
 * the NAL is anything other than TRAIL_R, or if no slice NALs are present
 * in the pack (we never touch VPS/SPS/PPS — those are nal_type >= 32 and
 * fail the TRAIL_R check). */
static void star6e_patch_pack_to_trail_n(MI_VENC_Pack_t *pack)
{
	if (!pack || !pack->data || pack->length == 0)
		return;
	if (pack->packNum > 0) {
		const unsigned int info_cap = (unsigned int)(sizeof(pack->packetInfo) /
			sizeof(pack->packetInfo[0]));
		unsigned int n = pack->packNum > info_cap ? info_cap : pack->packNum;
		unsigned int k;
		for (k = 0; k < n; ++k) {
			MI_U32 off = pack->packetInfo[k].offset;
			MI_U32 nlen = pack->packetInfo[k].length;
			if (off >= pack->length || nlen == 0 ||
			    off + nlen > pack->length)
				continue;
			size_t hdr = star6e_nal_header_idx(pack->data + off, nlen);
			if (hdr >= nlen) continue;
			if (pack->data[off + hdr] == 0x02) {
				pack->data[off + hdr] = 0x00;
			}
		}
		return;
	}
	/* packNum == 0: single NAL */
	if (pack->offset >= pack->length)
		return;
	{
		MI_U32 off = pack->offset;
		MI_U32 nlen = pack->length - off;
		size_t hdr = star6e_nal_header_idx(pack->data + off, nlen);
		if (hdr >= nlen) return;
		if (pack->data[off + hdr] == 0x02) {
			pack->data[off + hdr] = 0x00;
		}
	}
}

static void star6e_patch_stream_to_trail_n(MI_VENC_Stream_t *s)
{
	unsigned int i;
	if (!s || !s->packet) return;
	for (i = 0; i < s->count; i++)
		star6e_patch_pack_to_trail_n(&s->packet[i]);
}

/* HEVC-only since 0.10.12: IDR_W_RADL = nal_type 19. */
static uint8_t star6e_scene_is_idr(const MI_VENC_Stream_t *s)
{
	unsigned int i;
	if (!s || !s->packet) return 0;
	for (i = 0; i < s->count; i++) {
		const MI_VENC_Pack_t *p = &s->packet[i];
		unsigned int k, n = p->packNum;
		if (n > 0) {
			for (k = 0; k < n; k++) {
				if (p->packetInfo[k].packType.h265Nalu == 19)
					return 1;
			}
		} else {
			if (p->naluType.h265Nalu == 19) return 1;
		}
	}
	return 0;
}

static void star6e_service_ring_low_water(Star6eOutput *output);
static void star6e_report_frame_gate_setup(FrameGateSetupStatus st,
	const FrameGate *g, uint32_t slot_count);

/* Scene-detector IDR: goes through the shared 100 ms limiter, and a
 * coalesced request is not an error — another is already in flight.
 * Mirrors maruko_scene_request_idr(). */
static void star6e_scene_request_idr(void *ctx)
{
	int venc_chn = *(const int *)ctx;

	if (idr_rate_limit_allow(venc_chn))
		MI_VENC_RequestIdr(venc_chn, 1);
}

/* ── Runner context ────────────────────────────────────────────────────── */

typedef struct {
	VencConfig vcfg;
	Star6ePipelineState ps;
	int system_initialized;
	int httpd_started;
	int pipeline_started;
	SceneDetector scene;
	PipelineRateWatch rate_watch;
	/* Configured base size this pipeline started with — used at reinit to
	 * detect a video0.size change (the only config field that crosses a
	 * sensor-mode boundary), so the respawn can cold-init VIF/VPE. */
	uint32_t started_base_w;
	uint32_t started_base_h;
} Star6eRunnerContext;

static void install_signal_handlers(void);

/* Write VPE SCL clock preset before forced exit.  Uses only
 * async-signal-safe syscalls (open/write/close). */
static void scl_preset_emergency(void)
{
	static const char path[] = "/sys/devices/virtual/mstar/mscl/clk";
	static const char val[] = "384000000\n";
	static const char msg[] = "[waybeam] Emergency SCL preset written\n";
	int fd = open(path, O_WRONLY);

	if (fd >= 0) {
		(void)write(fd, val, sizeof(val) - 1);
		close(fd);
		(void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
	}
}

static void handle_signal(int sig)
{
	if (sig == SIGALRM) {
		static const char msg[] =
			"\n> Shutdown timeout reached, force exiting.\n";

		(void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
		scl_preset_emergency();
		_exit(128 + SIGINT);
	}

	if (sig == SIGHUP) {
		static const char msg[] =
			"\n> SIGHUP received, reinit pending...\n";

		venc_api_request_reinit();
		(void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
		return;
	}

	g_running = 0;
	g_signal_count++;

	if (g_signal_count == 1) {
		static const char msg[] =
			"\n> Interrupt received, shutting down...\n";

		(void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
		alarm(2);
		return;
	}

	{
		static const char msg[] = "\n> Force exiting.\n";

		(void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
	}
	scl_preset_emergency();
	_exit(128 + sig);
}

static void install_signal_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGALRM, &sa, NULL);
}

static Star6eRunnerContext *g_runner_ctx;

/* Grace for a mid-run record/stop.  Waiting out a stalled disk on the encode
 * loop would put the stall straight back on the live video path, which is the
 * whole thing this writer removes; losing the tail of a recording is the
 * correct trade. */
#define MIRROR_REC_STOP_GRACE_MS 250

/* Teardown gets a longer grace but is still BOUNDED, and that is not
 * cosmetic: the teardown watchdog forked below SIGKILLs this process after
 * 3 s (6 x 500 ms) and then writes sysrq-b.  An unbounded flush of up to
 * VENC_REC_WRITER_MAX_BYTES on the stalled card this writer exists for could
 * therefore turn an orderly shutdown into an emergency reboot.  500 ms writes
 * a full queue several times over on a healthy card and leaves the SDK
 * teardown the bulk of the deadline. */
#define MIRROR_REC_TEARDOWN_GRACE_MS 500

/* Runs on the writer thread, one access unit at a time, in order.
 *
 * No lock: the writer is started only after a recorder is open and joined
 * before it closes (mirror_record_open / mirror_record_close), so the sink
 * cannot run concurrently with an open or a close.  That is the whole reason
 * the writer's lifetime is tied to the recording's. */
static void mirror_record_sink(void *opaque, const uint8_t *au, size_t len,
	uint64_t pts_90khz, int is_idr)
{
	Star6ePipelineState *ps = opaque;

	if (star6e_ts_recorder_is_active(&ps->ts_recorder))
		(void)star6e_ts_recorder_write_video(&ps->ts_recorder, au, len,
			pts_90khz, is_idr);
	else
		(void)star6e_recorder_write_au(&ps->recorder, au, len,
			is_idr);
}

/* Initialised before anything can reach it.  This runs ahead of
 * venc_api_set_record_status_fn(), because httpd is already accepting by
 * then: a /api/v1/record/status arriving in the gap would lock a mutex that
 * had never been initialised.
 *
 * rec_locks_ready survives because init and destroy live in different
 * runtime callbacks: teardown after a failed bring-up must not destroy a
 * mutex that was never initialised. */
/* Only the mutex.  rec_locks_ready is NOT set here: it means "the things I
 * guard are initialised", and on this backend the recorders are initialised
 * ~45 lines later while venc_api_register() and
 * venc_api_set_record_http_control_supported() publish the HTTP surface in
 * between.  A calloc'd recorder has fd == 0, which star6e_recorder_is_active()
 * reads as an open file, so a GET /api/v1/record/status landing in that window
 * would report active:1 on a recording that does not exist.  Set the flag
 * after the recorders instead -- see mirror_record_locks_ready(). */
static void mirror_record_locks_init(Star6ePipelineState *ps)
{
	if (ps->rec_locks_ready)
		return;
	pthread_mutex_init(&ps->rec_writer_lock, NULL);
}

/* Publish the guard once both recorders are initialised. */
static void mirror_record_locks_ready(Star6ePipelineState *ps)
{
	ps->rec_locks_ready = 1;
}

/* Runs once the writer has been joined and freed -- inline on the encode loop
 * when the sink finished inside the deadline, on the reaper thread when it did
 * not.  The closes live here so that on both paths they happen after the sink
 * is guaranteed never to run again. */
/* Bounded wait for a detached reaper to release the recorders.  Teardown only:
 * a reaper that outlives the context would dereference it after backend.c has
 * freed it. */
#define REC_REAP_TEARDOWN_WAIT_MS 2000
static void star6e_wait_reap(Star6ePipelineState *ps, unsigned ms)
{
	unsigned waited = 0;

	while (__atomic_load_n(&ps->rec_reap_pending, __ATOMIC_ACQUIRE) &&
	       waited < ms) {
		usleep(2000);
		waited += 2;
	}
}

static void mirror_record_reap(void *opaque)
{
	Star6ePipelineState *ps = opaque;

	star6e_ts_recorder_stop(&ps->ts_recorder);
	star6e_recorder_stop(&ps->recorder);
	__atomic_store_n(&ps->rec_reap_pending, 0, __ATOMIC_RELEASE);
}

/* `async` picks whether this may join.  Mid-run (the encode loop) it must not:
 * a medium that has stopped completing would park live video inside a
 * record/stop.  At teardown it must, because there is no later for a reaper to
 * run in. */
static void mirror_record_close_mode(Star6ePipelineState *ps, unsigned grace_ms,
	int async)
{
	VencRecWriter *w;

	if (!ps->rec_locks_ready)
		return;

	/* A detached reaper still owns the recorders from an earlier stop: it
	 * has not joined the writer whose sink is still inside a write(), so
	 * both descriptors are live.  Closing again here would close them under
	 * that writer AND clear rec_reap_pending, letting the very next start
	 * reuse a descriptor the abandoned writer is about to append to.  The
	 * guard only means anything if nothing else clears it. */
	if (__atomic_load_n(&ps->rec_reap_pending, __ATOMIC_ACQUIRE)) {
		if (async)
			return;
		/* Teardown has no "later", so wait it out rather than leave the
		 * reaper running against a context about to be freed. */
		star6e_wait_reap(ps, REC_REAP_TEARDOWN_WAIT_MS);
		if (__atomic_load_n(&ps->rec_reap_pending, __ATOMIC_ACQUIRE)) {
			fprintf(stderr, "WARN: recorder writer still holding the "
				"file at teardown; skipping the close\n");
			return;
		}
	}

	/* Detach under the lock so a status poll already inside
	 * venc_rec_writer_stats() finishes against a live writer and any later
	 * one sees NULL.  Harvest the counters first — they must outlive the
	 * writer they came from. */
	pthread_mutex_lock(&ps->rec_writer_lock);
	w = ps->rec_writer;
	if (w) {
		uint64_t dropped = 0;
		uint32_t peak = 0;

		venc_rec_writer_stats(w, NULL, &dropped, NULL, &peak);
		ps->rec_dropped_frames = dropped;
		ps->rec_writer_peak_depth = peak;
	}
	ps->rec_writer = NULL;
	pthread_mutex_unlock(&ps->rec_writer_lock);

	/* Independent of the writer, so it need not wait on one. */
	ps->audio.rec_ring = NULL;

	{
		uint64_t dropped = 0;

		if (async) {
			/* Claimed before the stop: on the fast path the callback
			 * runs inside it and clears this again before it
			 * returns. */
			__atomic_store_n(&ps->rec_reap_pending, 1,
				__ATOMIC_RELEASE);
			venc_rec_writer_stop_bounded_async(w, grace_ms,
				&dropped, mirror_record_reap, ps);
		} else {
			venc_rec_writer_stop_bounded(w, grace_ms, &dropped);
		}
		/* Only if there WAS a writer: the stop leaves *dropped at 0 for
		 * a NULL one, which would clobber the harvest above.  Stored
		 * under the lock because the httpd thread reads it and an
		 * unlocked 64-bit store tears on ARM32. */
		if (w) {
			pthread_mutex_lock(&ps->rec_writer_lock);
			ps->rec_dropped_frames = dropped;
			pthread_mutex_unlock(&ps->rec_writer_lock);
		}
	}
	if (!async)
		mirror_record_reap(ps);
}

/* Mid-run close: never joins past the deadline. */
static void mirror_record_close(Star6ePipelineState *ps, unsigned grace_ms)
{
	mirror_record_close_mode(ps, grace_ms, 1);
}

/* Begin a recording: open the recorder, then start the writer that feeds it.
 * The single entry point for every start — boot auto-start and HTTP alike. */
static void mirror_record_open(Star6ePipelineState *ps, const VencConfig *vcfg,
	const char *dir)
{
	/* Same guard as the close: rec_writer_lock is taken below, and locking
	 * a mutex that was never initialised is undefined. */
	if (!dir || !dir[0] || !ps->rec_locks_ready)
		return;
	/* Stop first: a start over a live recording must not leak the open fd,
	 * and only one of the two recorders may ever be active. */
	mirror_record_close(ps, MIRROR_REC_STOP_GRACE_MS);

	/* On a healthy card the stop above completed inline and this is clear.
	 * If it did not, a detached reaper still owns both recorders'
	 * descriptors -- the previous recording's sink has not returned -- and
	 * starting over them would race the close. */
	if (__atomic_load_n(&ps->rec_reap_pending, __ATOMIC_ACQUIRE)) {
		fprintf(stderr, "WARN: record start refused; the previous "
			"recording's writer has not released the file yet "
			"(storage not completing writes)\n");
		return;
	}

	/* Before the open, not after it: these describe the recording being
	 * started, and a start that FAILS must not leave the previous
	 * recording's drop count standing as if it belonged to this one. */
	pthread_mutex_lock(&ps->rec_writer_lock);
	ps->rec_dropped_frames = 0;
	ps->rec_flatten_failures = 0;
	ps->rec_writer_peak_depth = 0;
	pthread_mutex_unlock(&ps->rec_writer_lock);

	if (strcmp(vcfg->record.format, "hevc") == 0) {
		if (star6e_recorder_start(&ps->recorder, dir) != 0)
			return;
	} else {
		if (vcfg->audio.enabled)
			ps->audio.rec_ring = &ps->audio_ring;
		if (star6e_ts_recorder_start(&ps->ts_recorder, dir,
				ps->audio.rec_ring) != 0) {
			ps->audio.rec_ring = NULL;
			return;
		}
	}

	/* DUAL MODE STOPS HERE.  The file is fed by the ch1 recording thread,
	 * not by this loop, so a writer would idle unfed for the whole session
	 * — and, worse, would falsify the invariant the producer gate rests on
	 * ("the handle is non-NULL exactly while a mirror recording runs").  The
	 * IDR is skipped for the same reason: runtime_request_idr() names ch0,
	 * and firing it here would inject a keyframe into the LIVE stream while
	 * doing nothing for the recording (the rate limiter is per-channel, so
	 * it cannot swallow ch1's own request).  That is the trap named in
	 * include/star6e_ts_recorder.h — a shared hook cannot name the right
	 * channel.  ch1 requests its own on its start path. */
	if (ps->dual)
		return;

	/* After the file is open, so the sink never sees a closed recorder.
	 *
	 * A writer that fails to start leaves the recording OPEN, unlike
	 * CV610: the producer gate here falls back to writing synchronously
	 * when rec_writer is NULL, so the recording still lands — on the
	 * encode loop, which is what the warning is about. */
	pthread_mutex_lock(&ps->rec_writer_lock);
	if (venc_rec_writer_start(&ps->rec_writer, mirror_record_sink, ps) != 0)
		fprintf(stderr, "WARNING: recorder writer thread did not start; "
			"mirror recording will write on the encode loop and can "
			"stall the live stream on slow storage\n");
	pthread_mutex_unlock(&ps->rec_writer_lock);

	/* Start the file on a keyframe.  Forced, not rate-limited: a GDR craft
	 * (resilience=racing) emits no periodic IDR, so a request the limiter
	 * coalesces away yields a file with no IRAP anywhere in it. */
	(void)runtime_request_idr();
}

static void record_status_callback(VencRecordStatus *out)
{
	Star6ePipelineState *ps;

	memset(out, 0, sizeof(*out));
	if (!g_runner_ctx)
		return;
	ps = &g_runner_ctx->ps;

	{
		uint64_t dropped;
		uint32_t peak;

		/* Under the lock: the writer is freed from the encode loop at
		 * teardown and this runs on the httpd thread.  The stored
		 * values are the fallback so a finished recording still
		 * reports what it shed. */
		pthread_mutex_lock(&ps->rec_writer_lock);
		/* Inside the lock, not just the store: a 64-bit load on ARM32 is
		 * two instructions and can straddle the encode loop's two-store
		 * update just as easily. */
		dropped = ps->rec_dropped_frames;
		peak = ps->rec_writer_peak_depth;
		if (ps->rec_writer)
			venc_rec_writer_stats(ps->rec_writer, NULL, &dropped,
				NULL, &peak);
		dropped += ps->rec_flatten_failures;
		pthread_mutex_unlock(&ps->rec_writer_lock);
		out->dropped_frames = (uint32_t)dropped;
		out->writer_peak_depth = peak;
	}

	/* is_RECORDING, not is_active — same reason as the producer gate: a
	 * rotation holds fd == -1 on the writer thread, and reporting that as
	 * "not recording" makes a healthy recording blink off once per segment,
	 * with stop_reason "manual" because start() seeds it that way.
	 *
	 * The recorder state below IS synchronised against the writer thread --
	 * see Star6eRecorderState::status_lock.  The old claim that these were
	 * "single words written by one thread" did not hold: bytes_written is
	 * 64-bit on ARM32, and path is rewritten wholesale on a rotation. */
	{
		/* ONE coherent instant per recorder.  The fields below are
		 * mutated by the writer thread during writes and segment
		 * rotation: bytes_written is 64-bit on ARM32 and path is
		 * rewritten wholesale on a rotation, so reading them in place
		 * could tear outright, and reading active, counters and path at
		 * three different instants could disagree with each other. */
		Star6eRecorderSnapshot ts_snap, rec_snap;

		star6e_ts_recorder_snapshot(&ps->ts_recorder, &ts_snap);
		star6e_recorder_snapshot(&ps->recorder, &rec_snap);

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
				(strcmp(g_runner_ctx->vcfg.record.format, "hevc") == 0) ? &rec_snap : &ts_snap;
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
				g_runner_ctx->vcfg.record.format);
		}
	}
}

/* Recorder start is a BOOTSTRAP event, not a request for a fresher picture:
 * the file that just opened contains nothing, and on a GDR craft — the flight
 * configuration, which emits no periodic IDRs at all — a coalesced request
 * leaves a recording with no IRAP access unit anywhere in it.  That file seeks
 * to nothing and plays from nothing, while the caller was told the start
 * succeeded.  Same failure shape as a destination change, so it takes the same
 * un-coalescible path: counted in /api/v1/idr/stats, never swallowed. */
static int runtime_request_idr_on(int chn)
{
	idr_rate_limit_force(chn);
	return MI_VENC_RequestIdr(chn, 1) == 0 ? 0 : -1;
}

/* Mirror-mode recorder: the file is fed by the main channel. */
static int runtime_request_idr(void)
{
	if (!g_runner_ctx)
		return -1;
	return runtime_request_idr_on(g_runner_ctx->ps.venc_channel);
}

/* Start the supervisory AE limit enforcer.  This is the sole AE path on
 * Star6E: the ISP firmware/bin AE does convergence and this thread re-asserts
 * the user's gain/shutter min/max on the exposure limit each tick.  The
 * historical aeEngine=custom userspace governor was retired — both engine
 * values now run this same enforcer (see start_ae_enforcer's caller). */
static void start_ae_enforcer(const Star6ePipelineState *ps,
	const VencConfig *vcfg)
{
	Star6eCus3aConfig ae_cfg;

	star6e_cus3a_config_defaults(&ae_cfg);
	if (vcfg->isp.ae_fps > 0)
		ae_cfg.ae_fps = vcfg->isp.ae_fps;
	if (vcfg->isp.gain_max > 0)
		ae_cfg.gain_max = vcfg->isp.gain_max;
	if (vcfg->isp.shutter_rule_180 && ps->sensor.fps > 0) {
		ae_cfg.shutter_max_us = 1000000 / (ps->sensor.fps * 2);
		ae_cfg.shutter_pin = 1;
	} else if (vcfg->isp.shutter_max_us > 0) {
		ae_cfg.shutter_max_us = vcfg->isp.shutter_max_us;
	}
	if (vcfg->isp.gain_min > 0)
		ae_cfg.gain_min = vcfg->isp.gain_min;
	if (vcfg->isp.shutter_min_us > 0)
		ae_cfg.shutter_min_us = vcfg->isp.shutter_min_us;
	ae_cfg.verbose = vcfg->system.verbose;
	star6e_cus3a_start(&ae_cfg);
}
/* Reduce ch1 bitrate by 10%.  Mirrors apply_bitrate() from
 * star6e_controls.c but operates on the dual VENC channel. */
static int dual_rec_reduce_bitrate(MI_VENC_CHN chn, uint32_t *current_kbps,
	uint32_t min_kbps)
{
	MI_VENC_ChnAttr_t attr = {0};
	uint32_t new_kbps;
	MI_U32 bits;

	if (MI_VENC_GetChnAttr(chn, &attr) != 0)
		return -1;

	new_kbps = *current_kbps * 9 / 10;
	if (new_kbps < min_kbps)
		new_kbps = min_kbps;
	if (new_kbps == *current_kbps)
		return 0;  /* already at floor */

	bits = new_kbps * 1024;
	switch (attr.rate.mode) {
	case I6_VENC_RATEMODE_H265CBR:
		attr.rate.h265Cbr.bitrate = bits;
		break;
	case I6_VENC_RATEMODE_H264CBR:
		attr.rate.h264Cbr.bitrate = bits;
		break;
	case I6_VENC_RATEMODE_H265VBR:
		attr.rate.h265Vbr.maxBitrate = bits;
		break;
	case I6_VENC_RATEMODE_H264VBR:
		attr.rate.h264Vbr.maxBitrate = bits;
		break;
	case I6_VENC_RATEMODE_H265AVBR:
		attr.rate.h265Avbr.maxBitrate = bits;
		break;
	case I6_VENC_RATEMODE_H264AVBR:
		attr.rate.h264Avbr.maxBitrate = bits;
		break;
	default:
		return -1;
	}

	if (MI_VENC_SetChnAttr(chn, &attr) != 0)
		return -1;

	printf("[dual] SD backpressure: bitrate %u -> %u kbps\n",
		*current_kbps, new_kbps);
	*current_kbps = new_kbps;
	return 0;
}

/* Recording thread: drains ch1 frames at full speed so the main loop
 * is never blocked by TS mux + SD write.  Follows the audio thread
 * pattern (volatile running flag + pthread_join on stop).
 *
 * Adaptive bitrate: if the SD card can't keep up, the thread detects
 * backpressure (frames queuing faster than written) and reduces ch1
 * bitrate by 10% per second until stabilized.  Once reduced, the
 * bitrate stays at the lower level for the rest of the session. */
static void *dual_rec_thread_fn(void *arg)
{
	Star6eDualVenc *d = arg;
	uint32_t current_kbps = d->bitrate;
	uint32_t min_kbps = d->bitrate / 4;
	if (min_kbps < 1000) min_kbps = 1000;  /* floor at 25%, min 1 Mbps */
	struct timespec interval_start;
	unsigned int behind_count = 0;
	unsigned int total_count = 0;
	unsigned int pressure_seconds = 0;

	clock_gettime(CLOCK_MONOTONIC, &interval_start);

	/* Block on MI_VENC_GetFd(chn) via poll() instead of spinning on
	 * MI_VENC_Query + usleep(1000).  The fd signals POLLIN when a
	 * frame is ready, so we wake exactly once per frame (~120/s at
	 * 120 fps) instead of ~1000/s from the old 1 ms spin.  If the
	 * SDK returns fd < 0 (unknown BSP variant), fall back to the
	 * original polling loop. */
	int venc_fd = MI_VENC_GetFd(d->channel);

	while (d->rec_running) {
		MI_VENC_Stat_t stat = {0};
		MI_VENC_Stream_t stream = {0};
		int ret;

		/* Service HTTP record start/stop forwarded by main loop.
		 * Dual-stream skips this (ts_recorder is NULL); dual mode
		 * routes here so the ts_recorder is opened/closed by exactly
		 * one thread. */
		if (d->rec_req_stop && d->ts_recorder) {
			star6e_ts_recorder_stop(d->ts_recorder);
			d->rec_req_stop = 0;
		}
		if (d->rec_req_start && d->ts_recorder) {
			star6e_ts_recorder_stop(d->ts_recorder);
			star6e_ts_recorder_start(d->ts_recorder,
				d->rec_req_start_dir, d->audio_ring);
			/* Ask the channel that actually feeds this file.  The
			 * shared hook targets the main channel, so in dual mode
			 * it keyframed the LIVE stream while the ch1 recording
			 * still opened without an IRAP — the exact outcome the
			 * un-coalescible path exists to prevent, aimed one
			 * channel off. */
			(void)runtime_request_idr_on(d->channel);
			d->rec_req_start = 0;
		}

		if (venc_fd >= 0) {
			/* POLL_TIMEOUT_MS = 1000 is large on purpose — it
			 * caps the rec_running cancellation latency
			 * without wasting cycles on short periodic wakes.
			 * Encoder frames arrive every 8-9 ms at 120 fps,
			 * long before this timeout expires. */
			struct pollfd pfd = { .fd = venc_fd, .events = POLLIN };
			(void)poll(&pfd, 1, 1000);
			/* POLLERR/POLLHUP/POLLNVAL: the SDK closed the fd
			 * under us (BSP quirk, pipeline reinit, VPE unbind).
			 * Fall back to the Query+usleep path for the rest
			 * of the thread's lifetime — don't busy-loop on a
			 * dead fd. */
			if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
				MI_VENC_CloseFd(d->channel);
				venc_fd = -1;
				usleep(1000);
				continue;
			}
			if (!(pfd.revents & POLLIN))
				continue;  /* timeout / spurious wake */
		}

		ret = MI_VENC_Query(d->channel, &stat);
		if (ret != 0 || stat.curPacks == 0) {
			/* Always sleep before retry: even with venc_fd >= 0,
			 * a spurious POLLIN that's not matched by an actual
			 * Query packet (rare BSP edge case) would otherwise
			 * busy-loop.  100us keeps wakeup latency low while
			 * preventing a runaway spin. */
			usleep(venc_fd >= 0 ? 100 : 1000);
			continue;
		}

		stream.count = stat.curPacks;
		stream.packet = ensure_packs(&d->stream_packs,
			&d->stream_packs_cap, stat.curPacks);
		if (!stream.packet) {
			usleep(1000);
			continue;
		}

		ret = MI_VENC_GetStream(d->channel, &stream,
			g_running ? 40 : 0);
		if (ret != 0) {
			/* EAGAIN on either path: sleep briefly before
			 * retrying so we don't spin if Query said
			 * stat.curPacks>0 but GetStream keeps contending. */
			if (ret == -EAGAIN || ret == EAGAIN)
				usleep(1000);
			continue;
		}
		/* Through the shared helper rather than an inline check: ch1 can
		 * be its own frame-shm ring, and the open-coded version counted
		 * nothing, so a ch1 discard was invisible in both bad_au_drops
		 * and the ring header's other_drops while its one-shot warn had
		 * already latched. */
		if (star6e_output_reject_incomplete_access_unit(&d->output,
		    &stream)) {
			MI_VENC_ReleaseStream(d->channel, &stream);
			continue;
		}

			/* Skip slow SD writes during shutdown — keep draining
			 * to prevent VPE backpressure while pipeline tears down. */
			if (g_running) {
				if (d->is_dual_stream) {
					/* Dual-stream ch1 has no sidecar of its own;
					 * pressure observation is purely a sidecar-
					 * trailer signal so skip it here.  The send
					 * always runs — see HISTORY 0.9.2 for why
					 * post-encode skip is the wrong adaptation
					 * knob for inter-frame-coded video. */
					(void)star6e_video_send_frame(&d->video,
						&d->output, &stream, 1, 0, NULL,
						NULL, NULL, 0);
					/* ch1 can be its own frame-shm ring;
					 * measure it here or it publishes a
					 * health marker over a low-water
					 * nothing ever writes. */
					star6e_service_ring_low_water(
						&d->output);
				} else if (d->ts_recorder) {
					star6e_ts_recorder_write_stream(
						d->ts_recorder, &stream);
				}
			}

		MI_VENC_ReleaseStream(d->channel, &stream);

		total_count++;

		/* Backpressure signal: the pre-GetStream Query found >= 2
		 * packets queued, meaning the encoder produced another frame
		 * before we consumed the prior one.  Equivalent semantics to
		 * a post-ReleaseStream peek but avoids one MI_VENC_Query
		 * syscall per recorded frame (~120/s at 120 fps). */
		if (stat.curPacks >= 2)
			behind_count++;

		/* Every second: evaluate backpressure */
		{
			struct timespec now;
			long long elapsed_ms;

			clock_gettime(CLOCK_MONOTONIC, &now);
			elapsed_ms = (long long)(now.tv_sec - interval_start.tv_sec) * 1000LL +
				(long long)(now.tv_nsec - interval_start.tv_nsec) / 1000000LL;

			if (elapsed_ms >= 1000) {
				/* Sustained pressure: >80% of frames had
				 * another waiting behind them.  Transient
				 * peaks (single slow write) won't trigger
				 * because most frames will have empty queue. */
				if (total_count > 0 &&
				    behind_count > total_count * 4 / 5) {
					pressure_seconds++;
					if (pressure_seconds >= 3) {
						dual_rec_reduce_bitrate(
							d->channel,
							&current_kbps,
							min_kbps);
						pressure_seconds = 0;
					}
				} else {
					pressure_seconds = 0;
				}

				behind_count = 0;
				total_count = 0;
				interval_start = now;
			}
		}
	}

	if (venc_fd >= 0)
		MI_VENC_CloseFd(d->channel);

	return NULL;
}

static void dual_rec_thread_start(Star6eDualVenc *d)
{
	pthread_attr_t attr;
	int have_attr;

	d->rec_running = 1;
	/* dual_rec_thread_fn reaches star6e_ts_recorder_write_stream, whose
	 * ~1.06 MB of automatic buffers overflows a musl default stack.  Star6E
	 * is glibc so its 8 MB default already covers it; set it anyway so the
	 * requirement travels with the code path rather than with the libc. */
	have_attr = pthread_attr_init(&attr) == 0;
	if (have_attr) {
		size_t cur = 0;
		/* RAISE only.  musl's 128 KB default must come up; glibc's 8 MB
		 * must not come down -- this thread also runs SDK entry points
		 * whose stack use is opaque, and lowering a platform that was
		 * already safe buys nothing. */
		if (pthread_attr_getstacksize(&attr, &cur) == 0 &&
		    cur < STAR6E_TS_RECORDER_STREAM_STACK_BYTES)
			(void)pthread_attr_setstacksize(&attr,
				STAR6E_TS_RECORDER_STREAM_STACK_BYTES);
	}
	if (pthread_create(&d->rec_thread, have_attr ? &attr : NULL,
			dual_rec_thread_fn, d) != 0) {
		if (have_attr)
			pthread_attr_destroy(&attr);
		fprintf(stderr, "[dual] ERROR: pthread_create failed for recording thread\n");
		d->rec_running = 0;
		return;
	}
	if (have_attr)
		pthread_attr_destroy(&attr);
	d->rec_started = 1;
	printf("> Dual recording thread started (mode: %s)\n", d->mode);
}

static int star6e_runtime_apply_startup_controls(Star6eRunnerContext *ctx)
{
	Star6ePipelineState *ps = &ctx->ps;
	VencConfig *vcfg = &ctx->vcfg;

	g_runner_ctx = ctx;
	/* Before the status callback below: httpd is already accepting by the
	 * time this function runs, and record_status_callback() takes
	 * rec_writer_lock. */
	mirror_record_locks_init(ps);
	star6e_controls_bind(ps, vcfg);
	star6e_iq_init();
	venc_api_register(vcfg, "star6e", star6e_controls_callbacks(),
		ps->luma_tap);
	venc_api_set_record_status_fn(record_status_callback);
	venc_api_set_record_http_control_supported(true);

	scene_init(&ctx->scene, ctx->vcfg.video0.scene_threshold,
		ctx->vcfg.video0.scene_holdoff);

	/* AE runs in ONE mode on Star6E: the SDK firmware/bin AE converges and
	 * the supervisory thread enforces the gain/shutter limits beside it.  The
	 * thread needs aeFps>0 for a tick rate. */
	if (vcfg->isp.ae_fps > 0)
		start_ae_enforcer(ps, vcfg);

	if (vcfg->fpv.roi_enabled) {
		star6e_controls_apply_roi_qp(vcfg->fpv.roi_qp);
	}
	if (vcfg->video0.qp_delta != 0) {
		star6e_controls_apply_qp_delta(vcfg->video0.qp_delta);
	}
	if (vcfg->video0.min_qp > 0 || vcfg->video0.max_qp > 0) {
		const VencApplyCallbacks *cb = star6e_controls_callbacks();
		/* Not (void): apply_qp_bounds() refuses on a VBR/AVBR rcMode and
		 * on an inverted pair, and a rejected cold-boot apply would
		 * otherwise leave the operator booting with no QP bound and no
		 * indication.  Same reporting as Maruko and CV610. */
		if (cb->apply_qp_bounds &&
		    cb->apply_qp_bounds(vcfg->video0.min_qp,
			    vcfg->video0.max_qp) != 0)
			fprintf(stderr, "WARN: qpBounds from config not applied "
				"(min=%u max=%u)\n",
				(unsigned)vcfg->video0.min_qp,
				(unsigned)vcfg->video0.max_qp);
	}

	if (!ps->output_enabled) {
		ps->stored_fps = vcfg->video0.fps;
		star6e_controls_apply_fps(STAR6E_CONTROLS_IDLE_FPS);
		printf("> Output disabled at startup, idling at %u fps\n",
			STAR6E_CONTROLS_IDLE_FPS);
	}

	/* Let a malformed access unit re-establish the reference chain
	 * locally, on whatever transport is configured.  Same rate-limited
	 * primitive the scene detector uses, so both producers of forced IDRs
	 * coalesce through one 100 ms window.  Set here rather than in the
	 * pipeline because the callback is runtime-local, and after
	 * star6e_output_init(), whose reset would clear it. */
	ps->output.request_idr = star6e_scene_request_idr;
	ps->output.idr_ctx = &ps->venc_channel;

	/* Adaptive frame gate.  Set up here, after star6e_output_init(), so the
	 * ring exists and its slot_count can sanity-check the threshold. */
	{
		venc_frame_ring_fill_t gfill = {0};

		(void)star6e_output_frame_ring_fill(&ps->output, &gfill);
		star6e_report_frame_gate_setup(frame_gate_setup(&ps->frame_gate,
			vcfg->video0.frame_gate_close_slots,
			vcfg->video0.frame_gate_max_closed_ms, gfill.slot_count),
			&ps->frame_gate, gfill.slot_count);
	}

	star6e_recorder_init(&ps->recorder);
	audio_ring_init(&ps->audio_ring);
	{
		/* Match the TS-mux codec to audio.codec.  Opus and PCM are the
		 * only TS-recordable formats; G.711 is not recorded (rec_ring
		 * stays unfed in star6e_audio.c, audio_rate set to 0 below). */
		int parsed = audio_codec_parse_name(vcfg->audio.codec);
		uint8_t ts_codec = (parsed == AUDIO_CODEC_TYPE_OPUS)
			? TS_AUDIO_CODEC_OPUS : TS_AUDIO_CODEC_PCM_S302M;
		uint32_t rate = 0;
		uint8_t  ch   = 0;
		if (vcfg->audio.enabled &&
		    (parsed == AUDIO_CODEC_TYPE_RAW ||
		     parsed == AUDIO_CODEC_TYPE_OPUS)) {
			rate = vcfg->audio.sample_rate;
			ch   = (uint8_t)vcfg->audio.channels;
		}
		star6e_ts_recorder_init(&ps->ts_recorder, rate, ch, ts_codec);
	}
	/* Both recorders: the rotation thresholds are the operator's,
	 * not the format's, and only one recorder is ever active. */
	if (vcfg->record.max_seconds > 0) {
		ps->ts_recorder.rot.max_seconds = vcfg->record.max_seconds;
		ps->recorder.rot.max_seconds = vcfg->record.max_seconds;
	}
	if (vcfg->record.max_mb > 0) {
		uint64_t max_bytes =
			(uint64_t)vcfg->record.max_mb * 1024 * 1024;

		ps->ts_recorder.rot.max_bytes = max_bytes;
		ps->recorder.rot.max_bytes = max_bytes;
	}

	/* Both recorders now hold fd == -1, so record/status can no longer read
	 * a calloc'd fd == 0 as an open file, and a stop can no longer close
	 * STDIN.  Published here rather than beside the mutex init for that
	 * reason -- see mirror_record_locks_init(). */
	mirror_record_locks_ready(ps);

	/* Start dual VENC if mode is "dual" or "dual-stream".
	 *
	 * Gated on mode ALONE, not record.enabled: the VENC channel topology
	 * is independent of whether recording auto-starts at boot.  A
	 * runtime-control client (e.g. RubyFPV) sets record.enabled=false and
	 * starts recording later via /api/v1/record/start.  ps->dual must
	 * already exist by then, otherwise the record path falls back to
	 * mirror mode and captures ch0 (the video0 stream bitrate) instead of
	 * ch1 (record.bitrate) — the high-bitrate recording channel is silently
	 * never used.  The ch1 TS writer no-ops while the recorder is closed
	 * (see star6e_ts_recorder_write_stream), so an idle ch1 records nothing
	 * until a record/start opens the file. */
	if (strcmp(vcfg->record.mode, "dual") == 0 ||
	    strcmp(vcfg->record.mode, "dual-stream") == 0) {
		star6e_pipeline_start_dual(ps,
			vcfg->record.bitrate, vcfg->record.fps,
			vcfg->record.gop_size, vcfg->record.mode,
			vcfg->record.server[0] ? vcfg->record.server : "");

		/* For dual-stream: init second RTP output */
		if (ps->dual && strcmp(vcfg->record.mode, "dual-stream") == 0 &&
		    ps->dual->server[0]) {
			Star6eOutputSetup ds_setup;
			if (star6e_output_prepare(&ds_setup, ps->dual->server,
			    vcfg->outgoing.stream_mode,
			    vcfg->outgoing.connected_udp) == 0) {
				ds_setup.allow_unix_encoder_stall =
					vcfg->outgoing.allow_unix_encoder_stall ? 1 : 0;
				if (star6e_output_init(&ps->dual->output,
				    &ds_setup) == 0) {
					star6e_video_init(&ps->dual->video, vcfg,
						ps->sensor.mode.maxFps,
						&ps->dual->output);
					printf("> Dual-stream: ch1 → %s\n",
						ps->dual->server);
				}
			}
		}

		/* Launch recording thread for ch1 frame draining */
		if (ps->dual) {
			ps->dual->is_dual_stream =
				(strcmp(vcfg->record.mode, "dual-stream") == 0);
			if (!ps->dual->is_dual_stream) {
				ps->dual->ts_recorder = &ps->ts_recorder;
				ps->dual->audio_ring =
					vcfg->audio.enabled ? &ps->audio_ring : NULL;
			}
			dual_rec_thread_start(ps->dual);
			venc_api_dual_register(ps->dual->channel,
				ps->dual->bitrate, ps->dual->fps,
				ps->dual->gop);
		}
	}

	/* Start recording (mirror or dual mode, not dual-stream) */
	if (vcfg->record.enabled &&
	    strcmp(vcfg->record.mode, "dual-stream") != 0 &&
	    strcmp(vcfg->record.mode, "off") != 0 &&
	    vcfg->record.dir[0]) {
		mirror_record_open(ps, vcfg, vcfg->record.dir);
	}

	return 0;
}

/* Same-PID (in-process) pipeline reinit does NOT work on Star6E — the
 * reliable cold restart is a *new PID* via fork+exec.  This was retested
 * exhaustively on imx335 @ 192.168.1.13 (2026-06-07) and confirmed at the
 * SigmaStar I6E driver level — see
 * documentation/STAR6E_SINGLE_PID_REINIT_FINDINGS.md for the full evidence.
 *
 * Note the historical reason recorded here was partly OUTDATED: a second
 * MI_SYS_Init in the same PID no longer hangs MI_DEVICE_Open (that mode is
 * gone).  But resetting MI_SYS, the /dev/mi_vif+vpe fds, AND the MI vendor
 * lib globals (dlclose+dlopen) in one process is STILL insufficient — the
 * VIF/VPE/ISP channel state the rebuild needs is pinned to the task in the
 * kernel driver and only released by execv (fresh address space + fd
 * context, same PID slot).  Symptom of an in-process rebuild: ISP readiness
 * timeout + CmdLoadBinFile -1 + VIF "layout type 2 bindmode 4 not sync err"
 * → no frames.
 *
 * The fork+exec machinery lives in src/venc_respawn.c (shared with Maruko);
 * the Star6E runtime just calls venc_respawn_request() in its reinit
 * handler.  Bench-validated against 12 consecutive cross-mode sensor
 * SIGHUPs (rounds 0→1→2→3 ×3) with no degradation. */

static int star6e_runtime_handle_reinit(int *handled)
{
	*handled = 0;

	if (!venc_api_get_reinit())
		return 0;
	*handled = 1;
	venc_api_clear_reinit();

	printf("> Reinit requested: cold restart via fork+exec on shutdown\n");
	fflush(stdout);

	/* Detect a video0.size change.  size is the only config field that
	 * crosses a sensor-mode boundary; resilience/framing(stab|zoom) stay
	 * within one mode.  On a mode change the respawn must cold-init VIF/VPE
	 * (close their inherited /dev/mi_* fds in the fd-scrub) — otherwise the
	 * fresh process re-inits VIF to a different mode against the old mode's
	 * kernel state and wedges vpe0_P0_MAIN.  Same-size respawns leave the
	 * fds inherited (the deadlock-safe default).  See venc_respawn.c.
	 *
	 * NOTE: the same-mode MMU read-fault storm (MMU client 0x15, IsWrite=0)
	 * that used to wedge the ~2nd consecutive respawn is now fixed at its
	 * root — see debug_osd_destroy()'s detach→destroy settle.  Cold-vif here
	 * remains gated strictly to size changes (forcing it on every respawn
	 * storms immediately because the close lands mid-flight). */
	if (g_runner_ctx &&
	    (g_runner_ctx->vcfg.video0.width != g_runner_ctx->started_base_w ||
	     g_runner_ctx->vcfg.video0.height != g_runner_ctx->started_base_h)) {
		printf("> size change %ux%u -> %ux%u: cold-init VIF/VPE on respawn\n",
			g_runner_ctx->started_base_w, g_runner_ctx->started_base_h,
			g_runner_ctx->vcfg.video0.width,
			g_runner_ctx->vcfg.video0.height);
		venc_respawn_set_cold_vif(1);
	}

	/* Mark for respawn after teardown, then exit the run loop.
	 * main() will execute backend->teardown (clean MI_SYS_Exit) then
	 * fork+exec the successor process from a clean state. */
	venc_respawn_request();
	g_running = 0;
	return 0;
}

/* Map a plugin box edge (net-input coords) to a canvas pixel, doing the whole
 * range clamp in float BEFORE the cast so a negative/huge/inf/NaN edge from an
 * out-of-contract plugin cannot trigger float->unsigned UB.  `net` is non-zero
 * at the call sites; guarded anyway. */
static uint32_t osd_box_px(float v, uint32_t canvas, uint32_t net)
{
	float r;
	if (canvas == 0 || net == 0)
		return 0;
	r = v * (float)canvas / (float)net;
	if (!(r >= 0.0f))                 /* negative, or NaN (compares false) */
		r = 0.0f;
	if (r > (float)(canvas - 1))      /* also catches +inf */
		r = (float)(canvas - 1);
	return (uint32_t)r;
}

/* Encoded bytes accumulated since the debug OSD's last 1 Hz refresh.  Summed
 * from the encoder's own frame sizes rather than the transport's byte count so
 * the row stays truthful with output disabled and excludes packetization
 * overhead — it reads the encoder against its RC target, not the wire.  Single
 * writer (the pipeline thread, which is also the only reader). */
static uint64_t g_osd_enc_bytes;

/* frame-shm ring low-water measurement.
 *
 * venc measures and publishes; it does not act.  The rate controller is
 * co-located on this SoC, reads this ring, and owns every response to what the
 * measurement says.
 *
 * State lives on the OUTPUT, not in a file static: dual-stream ch1 can be a
 * second frame-shm ring with its own occupancy (star6e_output_init handles
 * frame-shm:// for it just as it does for ch0), and a shared tracker cannot
 * represent two rings.  A ring left unmeasured would still publish the VHLT
 * health marker with low_water_slots at its create-time 0 -- the healthiest
 * value in the range -- so "nobody measured this" would be indistinguishable
 * from "the consumer is keeping up perfectly". */
static void star6e_service_ring_low_water(Star6eOutput *output)
{
	venc_frame_ring_fill_t fill;
	uint64_t now_us;

	if (!output)
		return;
	if (star6e_output_frame_ring_fill(output, &fill) != 0) {
		/* Not a frame-shm transport (or the ring went away across a
		 * reinit).  Drop the state so a later frame-shm run starts
		 * from a fresh window. */
		output->low_water_ready = 0;
		venc_ring_low_water_reset(&output->low_water, 0);
		return;
	}

	now_us = wb_monotonic_us();
	if (!output->low_water_ready) {
		venc_ring_low_water_reset(&output->low_water, now_us);
		output->low_water_ready = 1;
	}

	venc_ring_low_water_observe(&output->low_water, fill.used_slots,
		fill.slot_count);
	if (venc_ring_low_water_tick(&output->low_water, now_us)) {
		uint16_t slots =
			venc_ring_low_water_slots(&output->low_water);

		/* Publish into the ring header: a window in which the ring
		 * never drained is direct evidence that the consumer's rate
		 * model is optimistic (protocols/frame-shm.md). */
		venc_frame_ring_set_low_water(output->frame_ring, slots);
	}
}

/* Report what frame_gate_setup() made of the config.  The checks live in
 * frame_gate.c so the two backends cannot drift; only the wording is local. */
static void star6e_report_frame_gate_setup(FrameGateSetupStatus st,
	const FrameGate *g, uint32_t slot_count)
{
	switch (st) {
	case FRAME_GATE_SETUP_NO_RING:
		/* Every other transport lacks a per-frame occupancy signal, so
		 * the gate would never fire.  Say so once instead of looking
		 * enabled and doing nothing. */
		fprintf(stderr, "NOTE: frame gate inert — this output is not "
			"frame-shm, so it has no occupancy signal\n");
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

/* ── Adaptive frame gate ─────────────────────────────────────────────────
 *
 * Pauses ch0's frame intake while the frame-shm egress ring is not draining,
 * and resumes it when the consumer catches up.  This is the IDR-free way to
 * shed load: MI_VENC_SetChnAttr (the only proportional rate actuator) emits a
 * keyframe of its own, so reacting to congestion with a bitrate write puts
 * the largest frame in the stream into an already overflowing link.
 * MI_VENC_StopRecvPic changes no encoder state at all — see
 * include/frame_gate.h and documentation/ADAPTIVE_FRAME_GATE_PLAN.md.
 *
 * Called from two places in the stream loop: after a frame is sent (the
 * normal path) and from the "no packets yet" idle branch (the reopen path).
 * The second matters — while the gate is closed no frames are produced, so
 * the post-send call never runs and the gate could never let go.  The idle
 * branch already polls at ~1 ms, which is finer than any frame period we
 * support, so no timer and no extra thread are needed. */
static void star6e_service_frame_gate(Star6ePipelineState *ps)
{
	venc_frame_ring_fill_t fill;
	uint64_t now_us;

	if (!ps || !frame_gate_enabled(&ps->frame_gate))
		return;

	if (star6e_output_frame_ring_fill(&ps->output, &fill) != 0) {
		/* The ring went away across a reinit.  Release the gate rather
		 * than leaving the drain stopped with no occupancy signal that
		 * could ever reopen it.  No SDK call: intake was never
		 * stopped, only the drain. */
		if (!frame_gate_is_open(&ps->frame_gate))
			frame_gate_force_open(&ps->frame_gate,
				wb_monotonic_us());
		return;
	}

	now_us = wb_monotonic_us();

	/* In mirror/off mode ch0 feeds the recorder as well as the stream
	 * (dual/dual-stream put the recorder on ch1), so closing the gate
	 * would punch holes in the SD file for what is a radio problem.
	 * Suppress closes — but keep evaluating, so a gate already closed
	 * when a recording starts still reopens normally. */
	if (!ps->dual && frame_gate_is_open(&ps->frame_gate) &&
	    star6e_record_wants_frame(&ps->ts_recorder, &ps->recorder))
		return;

	/* The policy flip IS the actuator, so the returned action needs no
	 * handling here: process_stream() stops draining while the gate reads
	 * closed and VENC's own output FIFO applies the backpressure.  Nothing
	 * is issued to the SDK deliberately — MI_VENC_StopRecvPic/StartRecvPic
	 * emit an IRAP on every resume, one per cycle on this board and ~4.6
	 * per second against a real capacity-limited link, which is precisely
	 * the keyframe this feature exists to avoid. */
	(void)frame_gate_observe(&ps->frame_gate, fill.used_slots, now_us);
}

/* HTTP record start/stop, lifted so the gated drain path can service it
 * too.  Maruko and CV610 already handle record flags at the top of their
 * loops; on Star6E this block sat below the drain, so while the gate was
 * closed a record command waited for the next escape pulse. */
static void star6e_service_record_control(Star6ePipelineState *ps,
	const VencConfig *vcfg)
{
	/* Check HTTP record control flags.
	 *
	 * Mirror mode: act on the ts_recorder / hevc recorder directly here.
	 *
	 * Dual mode (not dual-stream): forward the request to the dual
	 * recording thread, which owns the ts_recorder exclusively.  This
	 * keeps the recorder single-threaded; the dual thread acts on the
	 * request between frame writes.
	 *
	 * Dual-stream mode: ch1 is sent over RTP, no on-disk recorder —
	 * consume and ignore the flag.
	 */
	{
		char rec_dir[256];
		int start_pending = venc_api_get_record_start(rec_dir,
			sizeof(rec_dir));
		int stop_pending = venc_api_get_record_stop();

		if (start_pending) {
			if (ps->dual && !ps->dual->is_dual_stream) {
				if (vcfg->audio.enabled)
					ps->audio.rec_ring = &ps->audio_ring;
				snprintf(ps->dual->rec_req_start_dir,
					sizeof(ps->dual->rec_req_start_dir),
					"%s", rec_dir);
				/* Set start flag last so the dual thread sees the
				 * dir already populated when it consumes the flag. */
				ps->dual->rec_req_stop = 0;
				ps->dual->rec_req_start = 1;
			} else if (!ps->dual) {
				/* Mirror mode: act directly on the recorders.
				 * mirror_record_open() closes any live
				 * recording first, joining its writer, so the
				 * old file's queued tail cannot land in the new
				 * one. */
				mirror_record_open(ps, vcfg, rec_dir);
			}
			/* dual-stream: nothing to do */
		}
		if (stop_pending) {
			if (ps->dual && !ps->dual->is_dual_stream) {
				ps->audio.rec_ring = NULL;
				ps->dual->rec_req_start = 0;
				ps->dual->rec_req_stop = 1;
			} else if (!ps->dual) {
				mirror_record_close(ps, MIRROR_REC_STOP_GRACE_MS);
			}
			/* dual-stream: nothing to do */
		}
	}
}

static int star6e_runtime_process_stream(Star6eRunnerContext *ctx,
	struct timespec *cus3a_ts_last, unsigned int *idle_counter)
{
	Star6ePipelineState *ps = &ctx->ps;
	VencConfig *vcfg = &ctx->vcfg;
	MI_VENC_Stat_t stat = {0};
	MI_VENC_Stream_t stream = {0};
	int ret;

	/* Gated: leave the encoded frame in VENC's output FIFO rather than
	 * draining it.  The gate is still serviced every pass, off the egress
	 * ring's own occupancy, so the reopen path never depends on draining. */
	if (!frame_gate_is_open(&ps->frame_gate)) {
		star6e_service_frame_gate(ps);
		/* Keep publishing egress pressure while gated.  This is the
		 * 200 ms low_water_slots export waybeam-link's rate model reads;
		 * freezing it for the whole closed interval would hide exactly
		 * the congestion that closed the gate. */
		star6e_service_ring_low_water(&ps->output);
		/* HTTP record start/stop must not wait for the gate to reopen;
		 * with a dead consumer that is one escape pulse away.  Maruko
		 * and CV610 already service record flags at the top of their
		 * loops. */
		star6e_service_record_control(ps, vcfg);
		star6e_pipeline_cus3a_tick(&g_sdk_quiet, cus3a_ts_last);
		idle_wait(&ps->video.sidecar, 1);
		return 0;
	}

	ret = MI_VENC_Query(ps->venc_channel, &stat);
	if (ret != 0) {
		/* Also a reopen path: a Query that fails while the gate is
		 * closed would otherwise never reach the curPacks branch, and
		 * a gate that cannot reopen is a hung stream. */
		star6e_service_frame_gate(ps);
		if ((++(*idle_counter) % 60) == 0) {
			printf("MI_VENC_Query failed %d\n", ret);
			fflush(stdout);
		}
		star6e_pipeline_cus3a_tick(&g_sdk_quiet, cus3a_ts_last);
		idle_wait(&ps->video.sidecar, 5);
		return 0;
	}

	if (stat.curPacks == 0) {
		/* Reopen path.  No frames are produced while the gate is
		 * closed, so the post-send call below never runs and this is
		 * the only place that can let go again. */
		star6e_service_frame_gate(ps);
		/* A closed gate is why there are no packets — say so instead of
		 * reporting a stalled encoder, and do not count it as idle. */
		if (!frame_gate_is_open(&ps->frame_gate)) {
			star6e_pipeline_cus3a_tick(&g_sdk_quiet, cus3a_ts_last);
			idle_wait(&ps->video.sidecar, 1);
			return 0;
		}
		if ((++(*idle_counter) % 120) == 0) {
			printf("waiting for encoder data...\n");
			fflush(stdout);
		}
		star6e_pipeline_cus3a_tick(&g_sdk_quiet, cus3a_ts_last);
		idle_wait(&ps->video.sidecar, 1);
		return 0;
	}
	*idle_counter = 0;

	stream.count = stat.curPacks;
	stream.packet = ensure_packs(&ps->stream_packs,
		&ps->stream_packs_cap, stat.curPacks);
	if (!stream.packet) {
		fprintf(stderr, "ERROR: Unable to allocate stream packets\n");
		return -1;
	}

	/* Drain IMU FIFO BEFORE GetStream so any future telemetry/sidecar
	 * consumer sees fresh samples for the frame currently being
	 * captured.  Without EIS (removed in 0.8.0) the drained samples
	 * go to the stub push callback and are discarded — cheap when
	 * imu.enabled=false, as it is by default. */
	if (ps->imu)
		imu_drain(ps->imu);

	ret = MI_VENC_GetStream(ps->venc_channel, &stream, 40);
	if (ret != 0) {
		if (ret == -EAGAIN || ret == EAGAIN) {
			idle_wait(&ps->video.sidecar, 2);
			return 0;
		}
		fprintf(stderr, "ERROR: MI_VENC_GetStream failed %d\n", ret);
		return ret;
	}
	if (star6e_output_reject_incomplete_access_unit(&ps->output,
	    &stream)) {
		MI_VENC_ReleaseStream(ps->venc_channel, &stream);
		return 0;
	}

	/* refPred error-resilience marking — rewrite TRAIL_R → TRAIL_N for
	 * frames the SDK marked as ENHANCE_P_NOTFORREF.  The encoder's own
	 * SVC-T pyramid logic determines which frames are non-reference; we
	 * just propagate that designation into the bitstream so generic
	 * receivers can safely drop those NALs without cascade.
	 *
	 * Only active when refPred was successfully applied — otherwise the encoder
	 * produces a flat single-ref stream and every frame matters. */
	if (ps->output.svct_active &&
	    stream.h265Info.refType == STAR6E_REFTYPE_ENHANCE_P_NOTFORREF) {
		star6e_patch_stream_to_trail_n(&stream);
	}

	{
		RtpSidecarEncInfo enc_info;
		uint32_t frame_size = star6e_scene_frame_size(&stream);
		uint8_t is_idr = star6e_scene_is_idr(&stream);

		g_osd_enc_bytes += frame_size;

		scene_update(&ctx->scene, frame_size, is_idr,
			star6e_scene_request_idr, &ps->venc_channel);
		scene_fill_sidecar(&ctx->scene, &enc_info);
		pipeline_common_rate_watch(&ctx->rate_watch, vcfg,
			frame_size, wb_monotonic_us());

		/* Observe pressure only when a sidecar probe is subscribed
		 * — it is the only consumer of in_pressure / fill_pct / the
		 * pressure_drops counter on the producer hot path.  When no
		 * one is listening, skip the SIOCOUTQ ioctl / ring-fill load
		 * entirely.  Always sending — a producer-side skip would
		 * break the H.265 reference chain (see HISTORY 0.9.2). */
		if (rtp_sidecar_is_subscribed(&ps->video.sidecar))
			star6e_output_observe_pressure(&ps->output);

		/* Attitude: runs whenever configured (a few µs/frame + the
		 * IMU ring mutex) — no subscription gate, so the WebUI live
		 * readout and the level-trim calibration work standalone.
		 * The trailer itself still only reaches the wire when a
		 * sidecar subscriber is live (send fans out to live slots). */
		RtpSidecarAttitudeInfo att_info;
		const RtpSidecarAttitudeInfo *att_ptr = NULL;
		if (vcfg->attitude.enabled && ps->imu)
			att_ptr = attitude_frame_update(vcfg, &att_info);

		/* Detection: serialise the latest IPU snapshot into a DETECT
		 * trailer, but only when detection is active AND a sidecar
		 * subscriber is live — the blob is dead weight otherwise.  The
		 * object cap keeps the trailer well under the datagram buffer,
		 * so the full buffer is a safe budget. */
		uint8_t detect_buf[RTP_SIDECAR_DGRAM_MAX];
		const void *detect_ptr = NULL;
		uint16_t detect_len = 0;
		if (vcfg->detect.enabled &&
		    rtp_sidecar_is_subscribed(&ps->video.sidecar)) {
			Star6eDetectSnapshot snap;
			if (star6e_ipu_yolo_snapshot(ps, &snap)) {
				uint64_t now_us = wb_monotonic_us();
				uint64_t age = now_us > snap.produced_us
					? (now_us - snap.produced_us) / 1000 : 0;
				/* model_id comes from the snapshot (latched with the
				 * boxes), not vcfg — so a live model swap flips it in
				 * lockstep with the first new-model DETECT instead of
				 * tagging the last old-model boxes with the new id. */
				size_t len = detect_wire_build(detect_buf,
					sizeof(detect_buf), snap.boxes, snap.count,
					snap.model_id, snap.seq,
					age > 0xFFFF ? 0xFFFF : (uint16_t)age,
					snap.net_w, snap.net_h, sizeof(detect_buf));
				if (len > 0) {
					detect_ptr = detect_buf;
					detect_len = (uint16_t)len;
				}
			}
		}

		(void)star6e_video_send_frame(&ps->video, &ps->output, &stream,
			ps->output_enabled, vcfg->system.verbose, &enc_info,
			att_ptr, detect_ptr, detect_len);

		/* frame-shm ring low-water measurement.  Runs unconditionally
		 * (no subscription gate — the consumer reads the result from
		 * the ring header, so it must be published whether or not
		 * anyone is watching over HTTP) and costs two relaxed atomic
		 * loads plus a compare per frame off frame-shm. */
		star6e_service_ring_low_water(&ps->output);

	}

	/* Orientation (image.flip / image.mirror) is applied once at bring-up
	 * (sensor_select before MI_SNR_Enable + start_vpe before VPE start) and
	 * holds for the life of the stream — device-verified on IMX335 and
	 * IMX415.  The sensor driver only rewrites orientation when we set it
	 * (orien_dirty), so nothing clears it mid-stream; no per-frame re-apply
	 * is needed.  (MI_SNR_GetOrien proved unreliable under AE I2C load on
	 * IMX335 — it reads 0 while the image is plainly held — so we do not
	 * use it to second-guess the applied state.) */

	/* In dual/dual-stream mode, ch1 handles recording (see below).
	 * In mirror/off mode, ch0 feeds the recorder directly. */
	/* Mirror-mode recording.  The SDK's stream memory dies at
	 * ReleaseStream below, so the writer thread gets its own copy; the
	 * flatten is not an extra cost for the TS path, which was already
	 * flattening onto a 512 KB stack buffer.  Gated so an idle craft pays
	 * no malloc per frame.
	 *
	 * A recorder that stopped ITSELF is torn down AFTER the release below,
	 * not here — see that site for why. */

	/* The writer pointer IS the gate: it exists for exactly as long as the
	 * recording it feeds.  No recorder-state predicate, so a rotation —
	 * which holds fd == -1 on the writer thread across fdatasync/close/open,
	 * tens to hundreds of ms on an SD card — is invisible here and cannot
	 * cost a frame. */
	if (!ps->dual && ps->rec_writer) {
		size_t au_len = 0;
		int au_idr = 0;
		uint8_t *au = star6e_output_stream_flatten(&stream, &au_len,
			&au_idr);

		if (au) {
			struct timespec rec_now;

			clock_gettime(CLOCK_MONOTONIC, &rec_now);
			(void)venc_rec_writer_push(ps->rec_writer, au, au_len,
				ts_mux_timespec_to_pts(
					(uint32_t)rec_now.tv_sec,
					(uint32_t)rec_now.tv_nsec),
				au_idr);
		} else {
			/* A frame the flatten refused is a frame missing from
			 * the file; counting it keeps that visible instead of
			 * making a damaged recording look clean (S8). */
			pthread_mutex_lock(&ps->rec_writer_lock);
			ps->rec_flatten_failures++;
			pthread_mutex_unlock(&ps->rec_writer_lock);
		}
	} else if (!ps->dual &&
		   !__atomic_load_n(&ps->rec_reap_pending, __ATOMIC_ACQUIRE)) {
		/* No writer.  Either nothing is recording — both calls below
		 * no-op on a closed recorder — or the writer thread failed to
		 * start, in which case this writes synchronously rather than
		 * silently recording nothing.  With no writer thread, rotation
		 * runs here on the encode loop, so fd is never transiently -1
		 * from another thread and the descriptor IS the right gate. */
		/* NOT while a reap is pending: rec_writer is NULL from the
		 * moment the async close detaches it, but the recorders stay
		 * OPEN until the reaper joins the writer whose sink is still
		 * inside a write().  Writing here would put that stalled write
		 * straight onto the encode loop -- unbounded, and worse than
		 * the stall the async close exists to remove -- while racing
		 * the abandoned writer on the same fd and mux state. */
		star6e_recorder_write_frame(&ps->recorder, &stream);
		star6e_ts_recorder_write_stream(&ps->ts_recorder, &stream);
	}

	/* Release the encoder stream as soon as the last consumer of the
	 * stream payload (recorder writes above) is done. Everything
	 * below — HTTP record control, verbose IMU/EIS output, debug OSD —
	 * only reads independent state, so releasing here frees the VENC
	 * output slot for the next frame instead of waiting on blocking
	 * work (stdout printf, OSD draw) that can otherwise push send
	 * spread past a full frame period at 120 fps. */
	MI_VENC_ReleaseStream(ps->venc_channel, &stream);

	/* Evaluate the gate after ReleaseStream, so a close takes effect from
	 * the next drain rather than stranding an acquired stream.  The helper
	 * takes a fresh instantaneous ring reading, so this placement does not
	 * weaken the burst response. */
	star6e_service_frame_gate(ps);

	/* A recorder that stopped ITSELF (disk full, write error) does so on the
	 * writer thread, so nothing but this loop is positioned to notice, and
	 * the gate above would go on queueing into a dead writer indefinitely.
	 *
	 * AFTER the release, because inside the GetStream/ReleaseStream window
	 * this would hold the
	 * encoder's output slot and stall the LIVE stream — precisely the
	 * coupling the writer thread exists to remove.  Since 0.73.2 the stop
	 * itself no longer ends in an unbounded join: past its deadline the
	 * writer is handed to a detached reaper, so a card that has stopped
	 * completing cannot park this loop either.  Costs one frame of latency
	 * in noticing, which the closed recorder discards anyway. */
	if (!ps->dual && ps->rec_writer &&
	    !star6e_record_wants_frame(&ps->ts_recorder, &ps->recorder))
		mirror_record_close(ps, MIRROR_REC_STOP_GRACE_MS);

	star6e_service_record_control(ps, vcfg);

	if (vcfg->system.verbose && ps->imu) {
		struct timespec imu_now;
		clock_gettime(CLOCK_MONOTONIC, &imu_now);
		long long elapsed_ms =
			((long long)(imu_now.tv_sec - g_imu_verbose_last.tv_sec) * 1000LL) +
			((long long)(imu_now.tv_nsec - g_imu_verbose_last.tv_nsec) / 1000000LL);
		if (elapsed_ms >= 1000) {
			ImuStats ist;
			imu_get_stats(ps->imu, &ist);
			printf("[imu] samples=%lu gyro=(%.3f,%.3f,%.3f)\n",
				(unsigned long)ist.samples_read,
				ist.last_gyro_x, ist.last_gyro_y, ist.last_gyro_z);
			fflush(stdout);
			g_imu_verbose_last = imu_now;
		}
	}

	/* Debug OSD overlay */
	if (ps->debug_osd) {
		static unsigned int osd_prev_frame;
		static struct timespec osd_prev_ts;
		static unsigned int osd_fps;
		static unsigned int osd_kbps;
		static Star6eAeOsdStatus osd_ae;
		struct timespec osd_now;

			/* HW-crop stab outputs the cropped encoded dim on port0, so the
			 * OSD canvas is 1:1 with the encoded frame (static) — no
			 * per-frame panel-offset tracking needed. */
		debug_osd_begin_frame(ps->debug_osd);
		debug_osd_sample_cpu(ps->debug_osd);

		/* Compute fps from frame counter delta */
		clock_gettime(CLOCK_MONOTONIC, &osd_now);
		long osd_ms = (osd_now.tv_sec - osd_prev_ts.tv_sec) * 1000 +
			(osd_now.tv_nsec - osd_prev_ts.tv_nsec) / 1000000;
		if (osd_ms >= 1000) {
			unsigned int df = ps->video.frame_counter - osd_prev_frame;
			osd_fps = (unsigned int)(df * 1000 / (unsigned long)osd_ms);
			osd_prev_frame = ps->video.frame_counter;
			osd_prev_ts = osd_now;
			/* bytes*8/ms is bits/ms, i.e. kbps directly. */
			osd_kbps = (unsigned int)(g_osd_enc_bytes * 8 /
				(uint64_t)osd_ms);
			g_osd_enc_bytes = 0;
			/* AE/AWB readouts ride the same 1Hz window — each
			 * refresh dlopens libmi_isp and round-trips several
			 * MI_ISP getters. */
			star6e_controls_ae_osd_status(&osd_ae);
		}

		debug_osd_text(ps->debug_osd, 0, "fps", "%u", osd_fps);
		debug_osd_text(ps->debug_osd, 1, "cpu", "%d%%",
			debug_osd_get_cpu(ps->debug_osd));

		/* Sensor readout + encoded output — read back exactly which mode
		 * is live (WxH@fps + mode idx) and the encoded WxH straight off
		 * the overlay.  Star6E video codec is always H.265. */
		debug_osd_text(ps->debug_osd, 2, "snr", "%ux%u@%u m%d",
			(unsigned)ps->sensor.plane.capt.width,
			(unsigned)ps->sensor.plane.capt.height,
			ps->sensor.fps, ps->sensor.mode_index);
		debug_osd_text(ps->debug_osd, 3, "enc", "%ux%u h265",
			ps->image_width, ps->image_height);

		/* Actual encoded rate against the configured RC target — the
		 * gap between the two is the RC undershoot/overshoot, and it
		 * separates an encoder problem from a link problem at a
		 * glance (a healthy encoder tracking target while the picture
		 * stutters points at the radio, not here). */
		{
			/* Append the ring low-water when the egress ring did
			 * not drain, so a stuttering picture separates "the
			 * consumer is behind" from "the encoder is behind".
			 * Absent when the ring drained — the common case
			 * should stay uncluttered. */
			char osd_ring[16];
			unsigned int lw = venc_ring_low_water_slots(
				&ps->output.low_water);

			osd_ring[0] = '\0';
			if (lw > 1)
				snprintf(osd_ring, sizeof(osd_ring), " ring%u",
					lw);
			debug_osd_text(ps->debug_osd, 4, "br", "%u/%uk%s",
				osd_kbps, vcfg->video0.bitrate, osd_ring);
		}

		{
			int osd_row = 5;

#if HAVE_FRAMING_STAB
			/* Stabilization telemetry: Kalman correction (a) +
			 * raw detector measurement (m), in stab pixels.
			 * "sfil" = stab-fill (correction applied as the
			 * compose shift), "stab" = HW-crop.  Hidden when no
			 * stab thread runs. */
			{
				int sx, sy, mx, my, sp, sf;
				if (star6e_framing_stab_osd_status(&sx, &sy,
				    &mx, &my, &sp, &sf))
					debug_osd_text(ps->debug_osd, osd_row++,
						sf ? "sfil" : "stab",
						"a%+d%+d m%+d%+d%s",
						sx, sy, mx, my,
						sp ? " paused" : "");
			}
#endif

			if (osd_ae.ae_valid) {
				debug_osd_text(ps->debug_osd, osd_row++,
					"exp", "%uus sg%u/%u ig%u",
					osd_ae.shutter_us,
					osd_ae.sgain_x1024, osd_ae.max_sgain,
					osd_ae.igain_x1024);
			}
			if (osd_ae.ae_info_valid) {
				debug_osd_text(ps->debug_osd, osd_row++,
					"ae", "y%u t%u %s",
					osd_ae.luma_y, osd_ae.scene_target,
					osd_ae.boundary ? "bound" :
					osd_ae.stable ? "stable" : "adj");
			}
			if (osd_ae.awb_valid && osd_ae.awb_userspace) {
				/* Userspace loop drives AWB: show the applied
				 * gains and the running apply count (which is
				 * the liveness signal — colour temperature is
				 * not estimated in this mode). */
				debug_osd_text(ps->debug_osd, osd_row++,
					"awb", "r%u b%u usr#%u",
					osd_ae.rgain, osd_ae.bgain,
					osd_ae.awb_ticks);
			} else if (osd_ae.awb_valid) {
				debug_osd_text(ps->debug_osd, osd_row++,
					"awb", "r%u b%u %uk %s",
					osd_ae.rgain, osd_ae.bgain,
					osd_ae.color_temp,
					osd_ae.awb_stable ? "stable" : "adj");
			}

			Star6eIntraRefreshStatus ir;
			Star6eRefPredStatus      rp;
			star6e_pipeline_intra_refresh_status(&ir);
			star6e_pipeline_ref_pred_status(&rp);
			/* Resilience banner: only render when the preset is set
			 * to something other than "off" — keeps the OSD compact
			 * when no resilience features are active. */
			if (vcfg->video0.resilience[0] &&
			    strcmp(vcfg->video0.resilience, "off") != 0) {
				if (rp.active) {
					debug_osd_text(ps->debug_osd, osd_row++,
						"res", "%s rp=%u/%u",
						vcfg->video0.resilience,
						rp.base, rp.enhance);
				} else {
					debug_osd_text(ps->debug_osd, osd_row++,
						"res", "%s",
						vcfg->video0.resilience);
				}
			}
			if (ir.active) {
				debug_osd_text(ps->debug_osd, osd_row++, "intra",
					"%s L%u q%u",
					ir.mode_name, ir.effective_lines_per_p,
					ir.effective_qp);
				debug_osd_text(ps->debug_osd, osd_row++, "gop",
					"%.2fs %s",
					ir.effective_gop_sec,
					ir.gop_auto ? "auto" : "fixed");
			}

			Star6eZoomStatus zoom;
			star6e_pipeline_zoom_status(&zoom);
			if (zoom.active) {
				debug_osd_text(ps->debug_osd, osd_row++, "zoom",
					"%u.%02ux %ux%u",
					zoom.level_x100 / 100,
					zoom.level_x100 % 100,
					zoom.output_w, zoom.output_h);
				debug_osd_text(ps->debug_osd, osd_row++, "crop",
					"%ux%u+%u+%u",
					zoom.crop_w, zoom.crop_h,
					zoom.crop_x, zoom.crop_y);
			}

			/* Detection boxes (detect.osd): scale net-space boxes
			 * onto the canvas — the port1 tap squashes the full
			 * FOV linearly per axis, so net->canvas is a straight
			 * ratio (the same mapping the sidecar normalizes by).
			 * A stale snapshot (reader stalled) is not drawn. */
			if (vcfg->detect.enabled && vcfg->detect.osd) {
				Star6eDetectSnapshot snap;
				static const uint16_t det_col[] = {
					DEBUG_OSD_RED, DEBUG_OSD_GREEN,
					DEBUG_OSD_YELLOW, DEBUG_OSD_CYAN,
					DEBUG_OSD_BLUE, DEBUG_OSD_WHITE,
				};
				const unsigned ncol =
					sizeof(det_col) / sizeof(det_col[0]);

				if (star6e_ipu_yolo_snapshot(ps, &snap) &&
				    snap.count > 0 &&
				    snap.net_w && snap.net_h &&
				    wb_monotonic_us() - snap.produced_us <
					700000) {
					uint32_t cw = ps->image_width;
					uint32_t chh = ps->image_height;
					int di;

					for (di = 0; di < snap.count; di++) {
						const DetectBox *b =
							&snap.boxes[di];
						uint32_t x1 = osd_box_px(b->x1,
							cw, snap.net_w);
						uint32_t y1 = osd_box_px(b->y1,
							chh, snap.net_h);
						uint32_t x2 = osd_box_px(b->x2,
							cw, snap.net_w);
						uint32_t y2 = osd_box_px(b->y2,
							chh, snap.net_h);

						if (x1 >= x2 || y1 >= y2)
							continue;
						debug_osd_rect(ps->debug_osd,
							(uint16_t)x1,
							(uint16_t)y1,
							(uint16_t)(x2 - x1),
							(uint16_t)(y2 - y1),
							det_col[(unsigned)
								b->cls % ncol],
							0);
					}
					debug_osd_text(ps->debug_osd,
						osd_row++, "det", "%d",
						snap.count);
				}
			}
		}

		debug_osd_end_frame(ps->debug_osd);
	}

	/* ch1 frames are now drained by the dedicated recording thread
	 * (dual_rec_thread_fn) — no polling needed here. */

	star6e_pipeline_cus3a_tick(&g_sdk_quiet, cus3a_ts_last);
	return 0;
}

static int star6e_prepare(void *opaque)
{
	(void)opaque;
	g_running = 1;
	g_signal_count = 0;
	install_signal_handlers();

	sdk_quiet_state_init(&g_sdk_quiet);
	star6e_controls_reset();
	return 0;
}

static int star6e_runner_init(void *opaque)
{
	Star6eRunnerContext *ctx = opaque;
	int ret;

	if (star6e_mi_init() != 0) {
		fprintf(stderr, "ERROR: MI library load failed\n");
		return -1;
	}

	sdk_quiet_begin(&g_sdk_quiet);
	ret = MI_SYS_Init();
	sdk_quiet_end(&g_sdk_quiet);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_SYS_Init failed %d\n", ret);
		star6e_mi_deinit();
		return ret;
	}
	ctx->system_initialized = 1;

	/* Always, before any VIF/VPE/ISP bring-up: reconcile NPU driver
	 * state a predecessor may have poisoned (see star6e_ipu_scrub).
	 * Unconditional by design — the poison survives process exit and
	 * fd release, so no flag carried from the previous instance can
	 * be trusted to know whether it is needed. */
	(void)star6e_ipu_scrub();

	venc_httpd_start(ctx->vcfg.system.web_port);
	ctx->httpd_started = 1;

	ret = star6e_pipeline_start(&ctx->ps, &ctx->vcfg, &g_sdk_quiet);
	if (ret != 0) {
		return ret;
	}
	ctx->pipeline_started = 1;
	/* Snapshot the base size the pipeline started with (before any live
	 * SET mutates ctx->vcfg in place via the aliased g_cfg). */
	ctx->started_base_w = ctx->vcfg.video0.width;
	ctx->started_base_h = ctx->vcfg.video0.height;

	ret = star6e_runtime_apply_startup_controls(ctx);
	if (ret != 0)
		return ret;
	install_signal_handlers();
	return 0;
}

static int star6e_runner_run(void *opaque)
{
	Star6eRunnerContext *ctx = opaque;
	struct timespec cus3a_ts_last = {0};
	struct timespec run_start;
	int cold_boot_fps_kick_done = 0;
	unsigned int idle_counter = 0;
	int handled;
	int ret;

	clock_gettime(CLOCK_MONOTONIC, &cus3a_ts_last);
	clock_gettime(CLOCK_MONOTONIC, &run_start);

	/* Pin encoder to CPU 0 and run the GetStream->packetize->send path at
	 * an elevated SCHED_FIFO priority.  At the previous minimum priority (1)
	 * this thread was preempted mid-frame by other userspace RT threads
	 * (audio capture / IMU, also FIFO/1) and SCHED_OTHER work, surfacing as
	 * a periodic ~one-frame RTP delivery stall (a single idle gap on the
	 * wire, no catch-up burst).  The SDK pipeline kernel threads run at
	 * SCHED_RR/98 and MUST keep outranking us — we depend on them to produce
	 * frames — so the priority is clamped well below 98 (audio testing also
	 * found ~90 made timing worse, likely priority inversion).
	 *
	 * Tunable for on-device A/B without a rebuild via the VENC_RT_PRIO env
	 * var (clamped 1..80); VENC_RT_PRIO=1 reproduces the old behaviour.
	 * Silent fallback if unprivileged or single-core. */
	{
		unsigned long mask = 1UL;  /* CPU 0 */
		syscall(__NR_sched_setaffinity, 0, sizeof(mask), &mask);

		int rt_prio = 50;
		const char *env = getenv("VENC_RT_PRIO");
		if (env && *env) {
			int v = atoi(env);
			if (v < 1)
				v = 1;
			else if (v > 80)
				v = 80;
			rt_prio = v;
		}

		struct sched_param sp;
		sp.sched_priority = rt_prio;
		if (pthread_setschedparam(pthread_self(), SCHED_FIFO,
		    &sp) != 0)
			printf("> note: RT priority not available"
				" (run as root)\n");
		else
			printf("> encoder thread: SCHED_FIFO prio %d,"
				" pinned CPU0\n", rt_prio);
	}

	while (g_running) {
		ret = star6e_runtime_handle_reinit(&handled);
		if (ret != 0) {
			return ret;
		}
		if (handled) {
			continue;
		}

		/* Service any pending detector live model-swap on this (pipeline)
		 * thread, between frames, so the VPE port1 recreate is atomic w.r.t.
		 * the per-frame detect snapshot query below. */
		star6e_controls_service_detect_reload();

		ret = star6e_runtime_process_stream(ctx, &cus3a_ts_last,
			&idle_counter);
		if (ret != 0) {
			return ret;
		}

		/* One-shot cold-boot fps re-kick ~1.5s after start, once the ISP
		 * bin load + AE have settled (the init-time kick fires too early
		 * and doesn't stick on a cold boot). */
		if (!cold_boot_fps_kick_done) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			if ((now.tv_sec - run_start.tv_sec) +
			    (now.tv_nsec - run_start.tv_nsec) / 1e9 >= 1.5) {
				star6e_pipeline_cold_boot_fps_rekick(&ctx->ps,
					&ctx->vcfg);
				cold_boot_fps_kick_done = 1;
			}
		}
	}

	return 0;
}

static void star6e_runner_teardown(void *opaque)
{
	Star6eRunnerContext *ctx = opaque;

	/* Fork a watchdog child that will force-kill us if teardown hangs.
	 * Unlike SIGALRM + _exit(), a child's kill -9 works even if the
	 * parent is in D-state on a kernel-side VPE flush — the kernel
	 * delivers SIGKILL to the parent process, which tears down all
	 * threads and releases driver resources from a clean context.
	 *
	 * If even SIGKILL can't recover (driver holds an uninterruptible
	 * lock), the child triggers sysrq-b (emergency reboot) as a
	 * last resort to prevent an indefinitely hung system. */
	{
		pid_t watchdog = fork();
		if (watchdog == 0) {
			/* Rename so /proc/<pid>/comm reads "waybeam-wd"
			 * instead of "waybeam".  Required for SIGHUP-respawn
			 * flow: the new instance spawned by
			 * star6e_runtime_respawn_after_exit() runs the
			 * comm-based duplicate check at startup; without this
			 * rename the still-alive watchdog (kept around to
			 * SIGKILL/sysrq-b a hung parent) reads as "waybeam"
			 * and the respawn aborts with the "already running"
			 * banner. */
			(void)prctl(PR_SET_NAME, VENC_COMM_WATCHDOG, 0, 0, 0);
			/* Close inherited stdout — it may be a pipe from the
			 * audio stdout filter.  Keeping it open prevents the
			 * filter thread's read() from seeing EOF, which
			 * deadlocks pthread_join in audio teardown. */
			close(STDOUT_FILENO);
			/* Child: poll parent liveness, escalate if stuck.
			 * Check every second — exit early if parent dies
			 * normally so we don't linger as an orphan. */
			pid_t parent = getppid();
			int i;
			for (i = 0; i < 6; i++) {
				usleep(500 * 1000);
				if (kill(parent, 0) != 0) {
					_exit(0);  /* parent exited cleanly */
				}
			}
			if (kill(parent, 0) == 0) {
				static const char m1[] =
					"[watchdog] teardown hung, kill -9\n";
				(void)write(STDERR_FILENO, m1, sizeof(m1) - 1);
				kill(parent, SIGKILL);
				sleep(1);
				if (kill(parent, 0) == 0) {
					static const char m2[] =
						"[watchdog] D-state, sysrq reboot\n";
					(void)write(STDERR_FILENO, m2,
						sizeof(m2) - 1);
					int fd = open("/proc/sysrq-trigger",
						O_WRONLY);
					if (fd >= 0) {
						(void)write(fd, "b", 1);
						close(fd);
					}
				}
			}
			_exit(0);
		}
		/* Parent: ignore watchdog errors, continue teardown */
	}
	alarm(0);  /* cancel SIGALRM — watchdog replaces it */

	/* Pause HTTP dispatch across the SDK teardown window: the httpd
	 * worker is still alive (venc_httpd_stop runs later, and even then
	 * it only detaches), so any in-flight HTTP handler would dereference
	 * the static control context that star6e_controls_reset is about to
	 * zero, plus VENC channels that star6e_pipeline_stop destroys.
	 * pause() drains any in-flight handler before returning; new requests
	 * during the window receive 503.  No resume — the process is exiting
	 * (SIGHUP fork+exec parent, or normal shutdown). */
	venc_httpd_pause();
	star6e_cus3a_request_stop();

	/* Pipeline stop MUST happen before recorder stop.  The recording
	 * thread runs inside pipeline_stop() and needs the ts_recorder fd
	 * open until StopRecvPic completes.  The thread skips SD writes
	 * when g_running==0 (already set by the signal handler). */
	if (ctx->pipeline_started) {
		star6e_iq_cleanup();
		star6e_controls_reset();
		star6e_pipeline_stop(&ctx->ps);
		ctx->pipeline_started = 0;
	}

	/* Now safe to join the 3A thread — pipeline is stopped so ISP
	 * calls will return errors and the thread will exit. */
	star6e_cus3a_join();

	/* Safe to close files now — recording thread has been joined
	 * inside pipeline_stop(). */
	/* Bounded — see MIRROR_REC_TEARDOWN_GRACE_MS: this teardown is
	 * watchdogged, so waiting out a stalled card here trades a lost
	 * recording tail for an emergency reboot. */
	mirror_record_close_mode(&ctx->ps, MIRROR_REC_TEARDOWN_GRACE_MS, 0);
	audio_ring_destroy(&ctx->ps.audio_ring);
	if (ctx->ps.rec_locks_ready) {
		pthread_mutex_destroy(&ctx->ps.rec_writer_lock);
		ctx->ps.rec_locks_ready = 0;
	}
	if (ctx->httpd_started) {
		venc_httpd_stop();
		ctx->httpd_started = 0;
	}
	star6e_luma_tap_destroy(ctx->ps.luma_tap);
	ctx->ps.luma_tap = NULL;
	if (ctx->system_initialized) {
		MI_SYS_Exit();
		ctx->system_initialized = 0;
		star6e_pipeline_vpe_scl_preset_shutdown();
	}
	star6e_mi_deinit();
}

static VencConfig *star6e_config(void *opaque)
{
	Star6eRunnerContext *ctx = opaque;

	return &ctx->vcfg;
}

static int star6e_map_pipeline_result(int result)
{
	return result;
}

static const BackendOps g_backend_ops = {
	.name = "star6e",
	.context_size = sizeof(Star6eRunnerContext),
	.config = star6e_config,
	.prepare = star6e_prepare,
	.init = star6e_runner_init,
	.run = star6e_runner_run,
	.teardown = star6e_runner_teardown,
	.map_pipeline_result = star6e_map_pipeline_result,
};

const BackendOps *star6e_runtime_backend_ops(void)
{
	return &g_backend_ops;
}
