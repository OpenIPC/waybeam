#include "maruko_pipeline.h"
#include "maruko_ipu_yolo.h"
#include "maruko_scl_ports.h"

#include "audio_codec.h"
#include "debug_osd.h"
#include "detect_wire.h"
#include "hevc_rtp.h"
#include "h26x_util.h"
#include "idr_rate_limit.h"
#include "intra_refresh.h"
#include "isp_runtime.h"
#include "maruko_bindings.h"
#include "maruko_config.h"
#include "maruko_controls.h"
#include "maruko_cus3a.h"
#include "maruko_output.h"
#include "maruko_framing.h"
#include "maruko_framing_host.h"
#include "maruko_recorder.h"
#include "maruko_stab_bench.h"
#include "maruko_stabfill_probe.h"
#include "maruko_ts_recorder.h"
#include "maruko_video.h"
#include "output_socket.h"
#include "pipeline_common.h"
#include "rtp_sidecar.h"
#include "sensor_select.h"
#include "stream_metrics.h"
#include "venc_api.h"
#include "venc_httpd.h"
#include "venc_jpeg.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "h26x_param_sets.h"
#include "maruko_video.h"
#include "rtp_session.h"
#include "timing.h"

/* Mirror-mode async recorder helpers — defined lower down, next to the
 * frame loop they serve, but referenced from pipeline start/stop above it. */
static int  mk_mirror_record_open(MarukoBackendContext *ctx, const char *dir);
static void mk_mirror_record_close(MarukoBackendContext *ctx, unsigned grace_ms);
static void mk_mirror_record_locks_init(MarukoBackendContext *ctx);
static void maruko_recorder_start_idr(MarukoBackendContext *ctx);
static void maruko_service_record_control(MarukoBackendContext *ctx);

/* Rate-limited forced IDR — defined next to the scene detector that was its
 * first caller, and also wired as MarukoOutput::request_idr in
 * bind_maruko_pipeline(). */
static void maruko_report_frame_gate_setup(FrameGateSetupStatus st,
	const FrameGate *g, uint32_t slot_count);

static void maruko_scene_request_idr(void *ctx_ptr);

/* Grace for a mid-run record/stop.  Waiting out a stalled disk on the encode
 * loop would put the stall straight back on the live video path, which is the
 * whole thing this writer removes; losing the tail of a recording is the
 * correct trade. */
#define MARUKO_REC_STOP_GRACE_MS 250

/* Recorder flush budget inside maruko_pipeline_teardown_graph()'s watchdog.  2 s covers a full 4 MB queue on
 * a healthy card several times over (~1.7 s of video at 19 Mbps, and it writes
 * far faster than realtime), while leaving the MI teardown below the bulk of
 * the deadline.  BOUNDED at teardown because maruko_pipeline_teardown_graph()
 * calls reboot(RB_AUTOBOOT) when its watchdog expires; star6e bounds its own
 * teardown for the same reason (a SIGKILL then sysrq-b at 3 s), just with a
 * smaller budget. */
#define MARUKO_REC_TEARDOWN_GRACE_MS 2000

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

volatile sig_atomic_t g_maruko_running = 1;
static volatile sig_atomic_t g_maruko_reinit = 0;
static int g_mi_isp_initialized = 0;
/* Last ISP bin path successfully loaded (Star6E parity).  Lets reinit
 * pick up SIGHUP-driven `isp.sensorBin` changes without re-running the
 * one-shot CUS3A enable below (which deadlocks the vendor mutex on a
 * second invocation).  Empty until the first successful load. */
static char g_last_isp_bin_path[256] = {0};
static int g_mi_isp_dev_created = 0;
static int g_mi_scl_dev_created = 0;
static int g_mi_isp_chn_created = 0;
static int g_mi_scl_chn_created = 0;
/* SCL channel 0 port 1 — second tap from the SCL channel for the MJPEG
 * snapshot backend.  Configured + enabled in configure_maruko_scl, bound
 * to MJPG VENC dev 8 by venc_jpeg_backend_init (src/maruko_jpeg.c). */
static int g_mi_scl_port1_enabled = 0;
/* stab-fill graph mode (cfg.framing == "stab-fill", latched per bring-up in
 * maruko_pipeline_configure_graph).  stab-fill swaps the SCL→VENC leg: SCL(0,0)
 * port0 becomes RAW + UNBOUND (drained by the fill module), and the VENC is
 * created in NORMAL_FRMBASE with NO bind — the fill module manually pushes the
 * composed frames into its input port (MI_SYS_ChnInputPortGetBuf/PutBuf),
 * device-proven at 50 fps.  off/zoom/stab keep the zero-copy RING leg
 * untouched.  (Phase 5a's "i6c VENC cannot be pushed" was an ABI artifact —
 * see maruko_setup_stabfill_venc.) */
static int g_stab_fill_graph = 0;
/* Mirrors MI_SYS_WindowRect_t.  Defined up here because both the SCL
 * configure path and the zoom helpers below use it. */
typedef struct {
	uint16_t u16X, u16Y, u16Width, u16Height;
} MaruWindowRect_t;

/* MI_ISP_CUS3A_SetAECropSize confines AE statistics to a sub-rect of the
 * ISP frame in 0..1023 normalized coords.  Resolved lazily via dlsym; if
 * absent every call becomes a silent no-op so the pipeline still runs.
 * Same ABI as Star6E — see src/star6e_pipeline.c:712-1086 for the parent
 * implementation (this is its Maruko twin). */
typedef struct {
	uint16_t crop_x;
	uint16_t crop_y;
	uint16_t crop_w;
	uint16_t crop_h;
} MarukoAeCropRect;

/* Note: Maruko (infinity6c) ABI differs from Star6E (infinity6e).
 *   Star6E:  MI_ISP_CUS3A_SetAECropSize(Channel, rect *)
 *   Maruko:  MI_ISP_CUS3A_SetAECropSize(DevId, Channel, rect *)
 * Calling with the Star6E 2-arg signature on Maruko corrupts the stack
 * and crashes the process before the SDK function even returns. */
typedef int (*maruko_set_ae_crop_fn_t)(uint32_t dev_id, uint32_t channel,
	MarukoAeCropRect *data);

static maruko_set_ae_crop_fn_t g_maruko_set_ae_crop;
static int g_maruko_ae_crop_resolved;
static int g_maruko_ae_crop_ready;     /* 1 once CUS3A handoff complete */
static int g_maruko_ae_crop_disabled;  /* 1 if the SDK rejected a call */
static MarukoAeCropRect g_maruko_ae_crop_last = { 0, 0, 1023, 1023 };

static void maruko_apply_ae_crop(MarukoBackendContext *ctx,
	double pct, double x, double y);
static void maruko_ae_crop_mark_ready(MarukoBackendContext *ctx);

/* Pan ramping — Star6E parity at lower tick rate.
 *
 * MI_SCL_SetPortConfig is heavier than Star6E's MI_VPE_SetPortCrop
 * (writes a full port struct, not just a rect), so we drive the ramp
 * at 30 Hz instead of 60 Hz to keep per-tick cost bounded.  Empirically
 * the bench X/Y walk (9 SETs back-to-back via HTTP at ~5 Hz) ran clean
 * without frame drops, so 30 Hz with the smoother in between should
 * stay well under the budget.  Halve PAN_RAMP_TICK_MS to revisit later
 * if smoothness suffers. */
#define MARUKO_PAN_RAMP_TICK_MS       33   /* ~30 Hz */
#define MARUKO_PAN_RAMP_SNAP_EPSILON  0.0005
/* Pan smoothing time constant — see star6e_pipeline.c
 * PAN_RAMP_DEFAULT_MS for the rationale.  Same value on both backends
 * so live pan feels identical regardless of platform. */
#define MARUKO_PAN_RAMP_DEFAULT_MS    150

typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t  cv;
	pthread_t       thread;
	int             running;
	int             has_state;
	double          target_pct;
	double          target_x;
	double          target_y;
	double          current_x;
	double          current_y;
	uint32_t        ramp_ms;
	MarukoBackendContext *ctx;  /* borrowed; cleared on ramp_stop */
} MarukoPanRampState;

static MarukoPanRampState g_maruko_pan_ramp = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.cv   = PTHREAD_COND_INITIALIZER,
	.ramp_ms = MARUKO_PAN_RAMP_DEFAULT_MS,
};

static int  maruko_pan_apply_locked(MarukoBackendContext *ctx,
	double pct, double x, double y);
static int  maruko_pan_ramp_start(MarukoBackendContext *ctx,
	double pct, double x, double y);
static void maruko_pan_ramp_stop(void);

static int maruko_config_dev_ring_pool(i6c_sys_mod module, MI_U32 device,
	MI_U16 max_width, MI_U16 max_height, MI_U16 ring_line)
{
	if (max_width == 0 || max_height == 0 || ring_line == 0)
		return 0;

	i6c_sys_pool pool;
	memset(&pool, 0, sizeof(pool));
	pool.type = I6C_SYS_POOL_DEVICE_RING;
	pool.create = 1;
	pool.config.ring.module = module;
	pool.config.ring.device = device;
	pool.config.ring.maxWidth = max_width;
	pool.config.ring.maxHeight = max_height;
	pool.config.ring.ringLine = ring_line;

	MI_S32 ret = maruko_mi_sys_config_private_pool(0, &pool);
	if (ret != 0) {
		fprintf(stderr,
			"WARNING: [maruko] MI_SYS_ConfigPrivateMMAPool failed %d"
			" (module=%d dev=%u size=%ux%u ring=%u)\n",
			ret, module, device, max_width, max_height, ring_line);
	} else {
		printf("> [maruko] private ring pool configured"
			" (module=%d dev=%u size=%ux%u ring=%u)\n",
			module, device, max_width, max_height, ring_line);
	}
	return ret;
}

/* Paced native 3A (the only AE mode on Maruko, HISTORY 0.22.0).
 *
 * Mechanism — every step device-verified on I6C (see
 * documentation/MARUKO_CUS3A_INJECT_HANDOFF.md for the full investigation):
 *   1. Pipeline init runs the stock userspace-3A stack
 *      (MI_ISP_EnableUserspace3A = CUS3A framework + native vendor algos +
 *      the ISP-API agent).  The agent is the algo's CONFIG FEED — IQ bin
 *      calibration reaches the algo init through it; without it the vendor
 *      AE never leaves state=-1.
 *   2. AE+AWB converge at full rate for AE_PACER_CONVERGE_S seconds
 *      (framework thread auto-runs per frame-sync in NORMAL run-mode).
 *   3. CUS3A_SetRunMode(OFF) pauses the per-frame auto-run — the source of
 *      the ~35pt 3A CPU cost at 100 fps — while leaving the apply path
 *      untouched.  (Run-mode INJECT is a DEAD END: InjectModeEnable(1)
 *      makes the agent mid-layer silently DROP every exposure apply.)
 *   4. This pacer thread then ticks the SAME converged vendor algo at
 *      sensor_fps/3 (floor 30 Hz): CUS3A_RunOnceEn once to arm AE+AWB,
 *      CUS3A_RunOnce per tick (runs Cus3A_ProcAE/AWB synchronously in this
 *      thread; applies land through the agent at ~0.25pt/Hz).
 * Result @1080p100: 70.0% (full-rate native) → 39.4% busy, vendor image. */
#define AE_PACER_CONVERGE_S 3

static int (*g_inj_runonce)(int, int, unsigned char, unsigned char,
	unsigned char);
static int (*g_inj_setrunmode)(int, int, int);
static int (*g_inj_runonce_now)(int, int);
static volatile int g_inj_run;
static unsigned g_inj_fps = 30;
static pthread_t g_inj_flip_thread;

static void *maruko_ae_pacer_thread(void *arg)
{
	unsigned us = 1000000u / (g_inj_fps ? g_inj_fps : 30);
	int i;

	(void)arg;
	/* Interruptible convergence wait: poll g_inj_run at 50 ms so a
	 * teardown/respawn issued during the first AE_PACER_CONVERGE_S
	 * seconds joins within ~50 ms instead of blocking the whole window
	 * inside pthread_join — respawn latency is fragile on this SoC. */
	for (i = 0; i < AE_PACER_CONVERGE_S * 20 && g_inj_run; i++)
		usleep(50000);
	if (!g_inj_run)
		return NULL;
	int rm_ret = g_inj_setrunmode(0, 0, 1); /* E_CUS3A_MODE_OFF */
	printf("> [maruko] CUS3A_SetRunMode(OFF) ret=%d — paced native 3A "
		"@%u Hz\n", rm_ret, g_inj_fps);
	if (rm_ret != 0)
		fprintf(stderr, "WARNING: [maruko] CUS3A_SetRunMode(OFF) failed "
			"(ret=%d) — vendor per-frame auto-run may still be active; "
			"AE could be double-driven (paced + auto)\n", rm_ret);
	g_inj_runonce(0, 0, 1, 1, 0); /* arm AE+AWB, no AF */
	while (g_inj_run) {
		g_inj_runonce_now(0, 0);
		usleep(us);
	}
	return NULL;
}

static void maruko_ae_pacer_stop(void)
{
	struct timespec deadline;
	int jr;

	if (!g_inj_run)
		return;
	g_inj_run = 0;
	/* Bounded join: the pacer's steady-state loop calls CUS3A_RunOnce,
	 * which on this SoC can stall inside the SDK during an ISP FIFO/MMU
	 * fault (the documented teardown D-state class).  An unbounded
	 * pthread_join would then wedge teardown/respawn forever.  Give the
	 * loop a short deadline to observe g_inj_run==0 and exit (normal case:
	 * one usleep, <=~33 ms); if it is stuck, detach and let teardown
	 * proceed rather than hanging the whole process. */
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_nsec += 300L * 1000L * 1000L; /* 300 ms */
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_nsec -= 1000000000L;
		deadline.tv_sec += 1;
	}
	jr = pthread_timedjoin_np(g_inj_flip_thread, NULL, &deadline);
	if (jr != 0) {
		fprintf(stderr, "WARNING: [maruko] AE pacer join timed out "
			"(%d) — detaching; thread may be stuck in CUS3A_RunOnce\n",
			jr);
		pthread_detach(g_inj_flip_thread);
	}
}

/* Enable CUS3A framework — required for ISP frame processing (without it
 * the ISP FIFO stalls at >=60fps).
 *
 * Always calls:
 *   - MI_ISP_CUS3A_Enable({1,1,0})  — bring up AE+AWB algos in the engine.
 *   - MI_ISP_EnableUserspace3A      — spawns the SDK's 3A_Proc_0 thread,
 *                                     which is what pumps IQ buffer writes
 *                                     (saturation/sharpness/brightness) into
 *                                     ISP HW.  Without this the IQ knobs
 *                                     accept writes but never reach the
 *                                     pipeline.
 *
 * The per-frame CPU cost of this stack is later cut by the RunOnce pacer
 * (paced native 3A) — see maruko_ae_pacer_thread and HISTORY 0.22.0. */
static void maruko_enable_cus3a(void)
{
	void *h = g_mi_isp.handle;
	if (!h)
		return;

	/* Step 1: enable AE+AWB algos (AF disabled for fixed-focus IMX415).
	 * Engine must be ENABLED so MI_ISP_CUS3A_SetAeParam is honored. */
	typedef int (*cus3a_enable_fn)(MI_U32 dev, MI_U32 chn, void *params);
	cus3a_enable_fn fn_enable = (cus3a_enable_fn)dlsym(h,
		"MI_ISP_CUS3A_Enable");
	if (!fn_enable)
		return;
	MI_BOOL p100[3] = {1, 0, 0};
	MI_BOOL p110[3] = {1, 1, 0};
	fn_enable(0, 0, p100);
	MI_S32 ret = fn_enable(0, 0, p110);
	printf("> [maruko] CUS3A_Enable(1,1,0) ret=%d\n", ret);

	/* Step 2: the stock userspace-3A stack.  MI_ISP_EnableUserspace3A =
	 * CUS3A_Init (spawns the 3A_Proc_0 framework thread) + native algo
	 * registration (CUS3A_EnableUserspaceAE/AWB/AF) + the ISP-API agent
	 * (the algo's calibration feed AND the working apply path).  The
	 * per-frame CPU cost of this stack is later cut by the pacer — see
	 * maruko_ae_pacer_thread. */
	typedef int (*enable_us3a_fn)(MI_U32 dev, MI_U32 chn);
	enable_us3a_fn fn_enable_us3a = (enable_us3a_fn)dlsym(h,
		"MI_ISP_EnableUserspace3A");
	if (fn_enable_us3a)
		printf("> [maruko] EnableUserspace3A ret=%d\n",
			fn_enable_us3a(0, 0));

	/* Pacer entry points (libcus3a; also linked by libmi_isp, so the
	 * dlopen resolves to the already-loaded copy). */
	void *cus = dlopen("libcus3a.so", RTLD_NOW | RTLD_GLOBAL);
	void *cs = cus ? cus : h;
	g_inj_setrunmode = (int (*)(int, int, int))dlsym(cs,
		"CUS3A_SetRunMode");
	g_inj_runonce = (int (*)(int, int, unsigned char, unsigned char,
		unsigned char))dlsym(cs, "CUS3A_RunOnceEn");
	g_inj_runonce_now = (int (*)(int, int))dlsym(cs, "CUS3A_RunOnce");
}

static int maruko_disable_userspace3a(const IspRuntimeLib *lib, void *ctx)
{
	maruko_isp_disable_userspace3a_fn_t fn;

	(void)ctx;
	fn = (maruko_isp_disable_userspace3a_fn_t)lib->disable_userspace3a;
	return fn ? fn(0, 0) : 0;
}

static int maruko_call_load_bin(const IspRuntimeLib *lib,
	const char *path, unsigned int load_key, void *ctx)
{
	maruko_isp_load_bin_fn_t fn_api;
	maruko_isp_load_bin_fn_t fn_api_alt;
	int ret;

	(void)ctx;
	fn_api = (maruko_isp_load_bin_fn_t)lib->load_bin_api;
	fn_api_alt = (maruko_isp_load_bin_fn_t)lib->load_bin_api_alt;
	ret = -1;
	if (fn_api)
		ret = fn_api(0, 0, (char *)path, load_key);
	if (ret != 0 && fn_api_alt && fn_api_alt != fn_api)
		ret = fn_api_alt(0, 0, (char *)path, load_key);
	return ret;
}

static void maruko_post_load_cus3a(const IspRuntimeLib *lib, void *ctx)
{
	typedef int (*cus3a_fn_t)(MI_U32 dev_id, MI_U32 channel, void *params);
	cus3a_fn_t fn_cus3a;
	MI_BOOL p100[3] = {1, 0, 0};
	MI_BOOL p110[3] = {1, 1, 0};

	(void)ctx;
	fn_cus3a = (cus3a_fn_t)lib->cus3a_enable;
	if (!fn_cus3a)
		return;

	fn_cus3a(0, 0, p100);
	fn_cus3a(0, 0, p110);
}

static int maruko_load_isp_bin(const char *isp_bin_path)
{
	IspRuntimeLoadHooks hooks;

	memset(&hooks, 0, sizeof(hooks));
	hooks.log_prefix = "[maruko] ";
	hooks.load_key = 1234;
	hooks.disable_userspace3a = maruko_disable_userspace3a;
	hooks.load_bin = maruko_call_load_bin;
	hooks.post_load = maruko_post_load_cus3a;
	int ret = isp_runtime_load_bin_file(isp_bin_path, &hooks);

	/* Also load via MI_ISP_IQ_ApiCmdLoadBinFile to initialize the
	 * IQ parameter subsystem. Without this, MI_ISP_IQ_Set* calls
	 * are accepted but have no effect on the image. The IQ variant
	 * takes raw bin data (not a file path).
	 * NOTE: this second load may reset AE parameters from the API
	 * bin — testing if skipping it fixes dark image issue. */
	if (ret == 0) {
		FILE *f = fopen(isp_bin_path, "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			long sz = ftell(f);
			fseek(f, 0, SEEK_SET);
			uint8_t *buf = malloc((size_t)sz);
			if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
				typedef int (*iq_load_fn_t)(uint32_t, uint32_t,
					uint8_t *, uint32_t);
				iq_load_fn_t fn = (iq_load_fn_t)dlsym(
					RTLD_DEFAULT,
					"MI_ISP_IQ_ApiCmdLoadBinFile");
				if (fn) {
					/* Load the IQ "api bin" — this is where the
					 * NR/sharpness/CCM tuning lives.  majestic does
					 * exactly this ("Load api bin Success"); skipping
					 * it (the old "AE fix") is what left NR at 0 =
					 * grainy image.  The earlier "dark image" it was
					 * meant to fix was the AE metering bug, not this
					 * load. */
					/* 4th arg is user_key (NOT the buffer size);
					 * use the same 1234 the base-bin load uses. */
					int r = fn(0, 0, buf, 1234);
					printf("> [maruko] IQ api bin load: %s (%s)\n",
						r == 0 ? "OK" : "FAILED", isp_bin_path);
				} else {
					printf("> [maruko] IQ bin load: "
						"symbol not found (skipped)\n");
				}
			}
			free(buf);
			fclose(f);
		}
	}

	return ret;
}

/* Live-reload variant of maruko_load_isp_bin: only calls the bin loader
 * symbol, intentionally skipping disable_userspace3a and post_load
 * CUS3A_Enable.  Both are required at cold boot but on a running pipeline
 * they trip the same kernel mutex regression that maruko_stop_vpe_channels
 * works around — re-entering CUS3A_Enable on the still-active channel
 * triggers "WARNING: Mutex is not initialized before lock" and segfaults
 * within the next IQ access.  3A_Proc_0 keeps running across the load and
 * picks up the new IQ tables on its next tick. */
static int maruko_load_isp_bin_minimal(const char *isp_bin_path)
{
	IspRuntimeLoadHooks hooks;

	memset(&hooks, 0, sizeof(hooks));
	hooks.log_prefix = "[maruko] ";
	hooks.load_key = 1234;
	/* disable_userspace3a, wait_ready, post_load left NULL */
	hooks.load_bin = maruko_call_load_bin;
	return isp_runtime_load_bin_file(isp_bin_path, &hooks);
}

int maruko_pipeline_load_isp_bin_live(MarukoBackendContext *ctx,
	const char *configured_path)
{
	char resolved[256];
	const char *configured;
	const char *sensor_name;

	if (!ctx)
		return -1;

	configured = (configured_path && *configured_path) ? configured_path : NULL;
	sensor_name = ctx->sensor.plane.sensName;

	if (!pipeline_common_resolve_isp_bin(configured, sensor_name,
	    resolved, sizeof(resolved))) {
		fprintf(stderr,
			"ERROR: [maruko] ISP bin reload — no readable bin for "
			"'%s' (sensor '%s')\n",
			configured ? configured : "(unset)",
			sensor_name ? sensor_name : "(unknown)");
		return -1;
	}

	if (strcmp(resolved, g_last_isp_bin_path) == 0) {
		printf("> [maruko] ISP bin reload: %s already loaded, skipping\n",
			resolved);
		return 0;
	}

	if (maruko_load_isp_bin_minimal(resolved) != 0)
		return -1;

	snprintf(g_last_isp_bin_path, sizeof(g_last_isp_bin_path), "%s",
		resolved);
	return 0;
}

/* maruko_load_symbol, i6c_isp_load/unload, i6c_scl_load/unload moved to
 * maruko_mi.c — ISP/SCL are loaded centrally via maruko_mi_init(). */

static void maruko_handle_signal(int sig)
{
	(void)sig;
	g_maruko_running = 0;
}

static void maruko_handle_sighup(int sig)
{
	(void)sig;
	g_maruko_reinit = 1;
}

void maruko_pipeline_install_signal_handlers(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = maruko_handle_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = maruko_handle_sighup;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGHUP, &sa, NULL);
}

static int maruko_start_vif(const SensorSelectResult *sensor)
{
	MI_S32 ret = 0;
	int group_created = 0;
	int dev_enabled = 0;
	int port_enabled = 0;

	MI_VIF_GroupAttr_t group = {0};
	group.eIntfMode = (MI_VIF_IntfMode_e)sensor->pad.intf;
	group.eWorkMode = E_MI_VIF_WORK_MODE_1MULTIPLEX;
	group.eHDRType = E_MI_VIF_HDR_TYPE_OFF;
	group.u32GroupStitchMask = E_MI_VIF_GROUPMASK_ID0;
	if (sensor->pad.intf == I6_INTF_BT656) {
		group.eClkEdge =
			(MI_VIF_ClkEdge_e)sensor->pad.intfAttr.bt656.edge;
	} else {
		group.eClkEdge = E_MI_VIF_CLK_EDGE_DOUBLE;
	}

	ret = MI_VIF_CreateDevGroup(0, &group);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_VIF_CreateDevGroup failed %d\n",
			ret);
		return ret;
	}
	group_created = 1;

	MI_VIF_DevAttr_t dev = {0};
	dev.stInputRect = sensor->plane.capt;
	dev.eField = 0;
	dev.bEnH2T1PMode = 0;
	if (sensor->plane.bayer > I6_BAYER_END) {
		dev.eInputPixel = sensor->plane.pixFmt;
	} else {
		dev.eInputPixel = (i6_common_pixfmt)
			(I6_PIXFMT_RGB_BAYER +
			 sensor->plane.precision * I6_BAYER_END +
			 sensor->plane.bayer);
	}

	printf("> [maruko] VIF dev: inputRect(%u,%u %ux%u) pixel=%d\n",
		dev.stInputRect.x, dev.stInputRect.y,
		dev.stInputRect.width, dev.stInputRect.height,
		dev.eInputPixel);
	ret = MI_VIF_SetDevAttr(0, &dev);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_VIF_SetDevAttr failed %d\n", ret);
		goto fail;
	}

	ret = MI_VIF_EnableDev(0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_VIF_EnableDev failed %d\n", ret);
		goto fail;
	}
	dev_enabled = 1;

	MI_VIF_OutputPortAttr_t port = {0};
	port.stCapRect = dev.stInputRect;
	port.stDestSize.width = dev.stInputRect.width;
	port.stDestSize.height = dev.stInputRect.height;
	port.ePixFormat = dev.eInputPixel;
	port.eFrameRate = E_MI_VIF_FRAMERATE_FULL;
	port.eCompressMode = E_MI_SYS_COMPRESS_MODE_NONE;

	printf("> [maruko] VIF port: capRect(%u,%u %ux%u) dest(%ux%u) "
		"pixel=%d compress=%d\n",
		port.stCapRect.x, port.stCapRect.y,
		port.stCapRect.width, port.stCapRect.height,
		port.stDestSize.width, port.stDestSize.height,
		port.ePixFormat, port.eCompressMode);
	ret = MI_VIF_SetOutputPortAttr(0, 0, &port);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_VIF_SetOutputPortAttr failed %d\n",
			ret);
		goto fail;
	}

	ret = MI_VIF_EnableOutputPort(0, 0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_VIF_EnableOutputPort failed %d\n",
			ret);
		goto fail;
	}
	port_enabled = 1;

	return 0;

fail:
	if (port_enabled)
		(void)MI_VIF_DisableOutputPort(0, 0);
	if (dev_enabled)
		(void)MI_VIF_DisableDev(0);
	if (group_created)
		(void)MI_VIF_DestroyDevGroup(0);
	return ret;
}

static void maruko_stop_vif(void)
{
	(void)MI_VIF_DisableOutputPort(0, 0);
	(void)MI_VIF_DisableDev(0);
	(void)MI_VIF_DestroyDevGroup(0);
}

static int configure_maruko_isp(const SensorSelectResult *sensor,
	int vpe_level_3dnr)
{
	MI_S32 ret = 0;
	int dev = 0, chn = 0, started = 0, port = 0;


	if (!g_mi_isp_dev_created) {
		unsigned int sensor_mask = (1u << (unsigned int)sensor->pad_id);
		ret = g_mi_isp.fnCreateDevice(0, &sensor_mask);
		if (ret != 0) {
			fprintf(stderr,
				"ERROR: [maruko] MI_ISP_CreateDevice failed %d\n", ret);
			goto fail;
		}
		g_mi_isp_dev_created = 1;
	}
	dev = 1;

	if (!g_mi_isp_chn_created) {
		i6c_isp_chn isp_chn = {0};
		/* SigmaStar ISP sensorId is 1-based (pad 0 -> sensorId 1),
		 * matching majestic behavior. pad_id=0 with sensorId=0
		 * causes ISP frame processing to stall at larger resolutions. */
		isp_chn.sensorId = (unsigned int)(sensor->pad_id + 1);
		ret = g_mi_isp.fnCreateChannel(0, 0, &isp_chn);
		if (ret != 0) {
			fprintf(stderr,
				"ERROR: [maruko] MI_ISP_CreateChannel failed %d\n",
				ret);
			goto fail;
		}
		g_mi_isp_chn_created = 1;
	}
	chn = 1;

	{
		i6c_isp_para isp_para;
		memset(&isp_para, 0, sizeof(isp_para));
		isp_para.hdr = I6_HDR_OFF;
		isp_para.level3DNR = vpe_level_3dnr;
		/* Match majestic: set yuv2BayerOn based on sensor bayer type.
		 * Bayer sensors (bayer <= I6_BAYER_END) feed raw Bayer data;
		 * YUV sensors have bayer > I6_BAYER_END. */
		isp_para.yuv2BayerOn =
			(sensor->plane.bayer > I6_BAYER_END) ? 1 : 0;
		printf("> [maruko] ISP params: 3DNR=%d hdr=%d yuv2Bayer=%d\n",
			isp_para.level3DNR, isp_para.hdr,
			isp_para.yuv2BayerOn);
		ret = g_mi_isp.fnSetChannelParam(0, 0, &isp_para);
		if (ret != 0) {
			fprintf(stderr,
				"ERROR: [maruko] MI_ISP_SetChnParam failed %d\n",
				ret);
			goto fail;
		}
	}

	ret = g_mi_isp.fnStartChannel(0, 0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_ISP_StartChannel failed %d\n",
			ret);
		goto fail;
	}
	started = 1;

	i6c_isp_port isp_port;
	memset(&isp_port, 0, sizeof(isp_port));
	/* Match majestic: ISP output port uses YUV422_YUYV with zero crop
	 * (let SCL handle crop/scale). Setting crop to sensor dimensions
	 * caused ISP frame processing to stall at higher resolutions. */
	isp_port.pixFmt = I6_PIXFMT_YUV422_YUYV;
	isp_port.compress = I6_COMPR_NONE;
	printf("> [maruko] ISP port: crop(%u,%u %ux%u) fmt=%d compress=%d\n",
		isp_port.crop.x, isp_port.crop.y,
		isp_port.crop.width, isp_port.crop.height,
		isp_port.pixFmt, isp_port.compress);
	ret = g_mi_isp.fnSetPortConfig(0, 0, 0, &isp_port);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_ISP_SetOutputPortParam failed %d\n",
			ret);
		goto fail;
	}

	ret = g_mi_isp.fnEnablePort(0, 0, 0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_ISP_EnableOutputPort failed %d\n",
			ret);
		goto fail;
	}
	port = 1;

	return 0;

fail:
	if (port)
		(void)g_mi_isp.fnDisablePort(0, 0, 0);
	if (started)
		(void)g_mi_isp.fnStopChannel(0, 0);
	if (chn)
		(void)g_mi_isp.fnDestroyChannel(0, 0);
	if (dev)
		(void)g_mi_isp.fnDestroyDevice(0);
	return ret ? ret : -1;
}

/* ar_crop: keep_aspect AR rect in full-input coords (NULL = no crop).
 * zoom_rect: Approach-C zoom window relative to the AR surface (NULL =
 * no zoom).  See the keep_aspect block below for why the WIDTH
 * component of the AR rect degrades to an anamorphic squeeze on this
 * platform. */
static int configure_maruko_scl(const SensorSelectResult *sensor,
	uint32_t out_width, uint32_t out_height,
	const PipelinePrecropRect *ar_crop,
	const PipelinePrecropRect *zoom_rect)
{
	MI_S32 ret = 0;
	int dev = 0, chn = 0, started = 0, port = 0;

	if (!g_mi_scl_dev_created) {
		/* Match majestic: enable all 4 HW scaler ports (bits 0-3). */
		unsigned int scl_bind = 0xF;
		ret = g_mi_scl.fnCreateDevice(0, &scl_bind);
		if (ret != 0) {
			fprintf(stderr,
				"ERROR: [maruko] MI_SCL_CreateDevice failed %d\n",
				ret);
			goto fail;
		}
		g_mi_scl_dev_created = 1;
	}
	dev = 1;

	if (!g_mi_scl_chn_created) {
		unsigned int scl_reserved = 0;
		ret = g_mi_scl.fnCreateChannel(0, 0, &scl_reserved);
		if (ret != 0) {
			fprintf(stderr,
				"ERROR: [maruko] MI_SCL_CreateChannel failed %d\n",
				ret);
			goto fail;
		}
		g_mi_scl_chn_created = 1;
	}
	chn = 1;

	int rotation = 0;
	ret = g_mi_scl.fnAdjustChannelRotation(0, 0, &rotation);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_SCL_SetChnParam failed %d\n", ret);
		goto fail;
	}

	/* keep_aspect AR crop is applied here as the SCL OUTPUT-port *source*
	 * crop — the sub-window the scaler reads before scaling to the encode
	 * size (honouring the FULL ar rect, x + width, not just its height).
	 * This is the only crop stage that works on the I6C camera pipeline;
	 * the other avenues are hardware-blocked (verified on device):
	 *   - VIF sub-window crop is unsupported on I6C (Star6E crops here);
	 *   - a non-zero ISP output-port crop stalls ISP processing at high res;
	 *   - MI_SCL_SetInputPortCrop (the vendor mixer path) needs a FRAMEBASE
	 *     SCL input, which mi_sys refuses on the REALTIME ISP->SCL edge
	 *     (-1610014712).
	 * A SCL source-crop whose width != ring stride IS fine at a fresh
	 * bringup (an earlier "stalls the scaler / live-poke panics" reading was
	 * an artifact of mutating a running pipeline — see the note below). */
	uint32_t in_w = sensor->plane.capt.width;
	uint32_t in_h = sensor->plane.capt.height;
	PipelinePrecropRect ar = {0, 0, (uint16_t)in_w, (uint16_t)in_h};
	if (ar_crop)
		ar = *ar_crop;
	/* Real AR centre-crop via the SCL *source* crop rect — honour the
	 * FULL ar rect (x + width), not just its height.  The ar rect always
	 * carries the OUTPUT aspect ratio (computed by
	 * pipeline_common_compute_precrop), so the SCL then scales it to the
	 * encode size by a SINGLE near-uniform factor on both axes (exact up to
	 * the crop rect's 2-px even-rounding) — never the single-axis
	 * (anamorphic) scale that stalls.
	 *
	 * This is what makes it stall-free.  Device-verified 2026-07-04 on
	 * ssc378qe/I6C across 28 configs (all 5 modes x 16:9 + 4:3, down- AND
	 * up-scale): the ONLY shape the I6C SCL cannot do is the old
	 * full-width squeeze — read full 1920 and downscale ONE axis while the
	 * other passes 1:1 (crop full 1920 -> output 1440x1080).  That starved
	 * the encoder (0 frames -> 20s abort) and wedged the binned sensor.
	 * A crop-then-uniform-scale (up or down) is always clean, because
	 * cropping to the output AR first guarantees both axes move together.
	 * (An earlier "upscale spams ISP P0 FIFO FULL" reading was a
	 * live-poke artifact of mutating a running pipeline whose ring pools
	 * were still sized for the native mode; at a fresh bringup, upscale is
	 * fine — hence no crop/upscale guard here.) */
	PipelinePrecropRect pcrop;
	pcrop.x = ar.x;
	pcrop.y = ar.y;
	pcrop.w = ar.w;
	pcrop.h = ar.h;
	if (zoom_rect) {
		/* 1:1 zoom window (crop == output dims, no scaling) — safe
		 * at any width.  Positioned relative to the AR surface,
		 * which sits at ar.x/ar.y inside the full input. */
		pcrop.x = (uint16_t)(ar.x + zoom_rect->x);
		pcrop.y = (uint16_t)(ar.y + zoom_rect->y);
		pcrop.w = zoom_rect->w;
		pcrop.h = zoom_rect->h;
	}

	ret = g_mi_scl.fnStartChannel(0, 0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_SCL_StartChannel failed %d\n",
			ret);
		goto fail;
	}
	started = 1;

	/* Output = target dimensions; IFC compress required for HW_RING
	 * binding to VENC. */
	i6c_scl_port scl_port;
	memset(&scl_port, 0, sizeof(scl_port));
	scl_port.crop.x = pcrop.x;
	scl_port.crop.y = pcrop.y;
	scl_port.crop.width = pcrop.w;
	scl_port.crop.height = pcrop.h;
	scl_port.output.width = (unsigned short)out_width;
	scl_port.output.height = (unsigned short)out_height;
	scl_port.pixFmt = I6_PIXFMT_YUV420SP;
	/* IFC for the zero-copy VENC RING leg; RAW in stab-fill, where port0 is
	 * unbound and the fill module CPU-reads + PA-blits the frames. */
	scl_port.compress = (i6_common_compr)(g_stab_fill_graph ? 0 : 6);
	printf("> [maruko] SCL port: crop(%u,%u %ux%u) out(%ux%u) "
		"fmt=%d compress=%d\n",
		scl_port.crop.x, scl_port.crop.y,
		scl_port.crop.width, scl_port.crop.height,
		scl_port.output.width, scl_port.output.height,
		scl_port.pixFmt, scl_port.compress);
	ret = g_mi_scl.fnSetPortConfig(0, 0, 0, &scl_port);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_SCL_SetOutputPortParam failed %d\n",
			ret);
		goto fail;
	}

	ret = g_mi_scl.fnEnablePort(0, 0, 0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_SCL_EnableOutputPort failed %d\n",
			ret);
		goto fail;
	}
	port = 1;

	/* SCL port 1 — second tap from the same SCL channel, dedicated to
	 * the MJPEG snapshot backend (src/maruko_jpeg.c).  Same crop and
	 * output dims as port 0 so snapshots match the live stream framing;
	 * pixFmt YUV420SP with no IFC compress because the MJPG VENC
	 * channel reads raw YUV, not IFC-compressed tiles.  Bind happens
	 * later in venc_jpeg_backend_init via BindChnPort2 FRAMEBASE @
	 * 5 fps so this port only sees a trickle of frames between
	 * snapshot requests.
	 *
	 * Port-1 failures are non-fatal: we log a warning, leave
	 * g_mi_scl_port1_enabled=0, and the snapshot endpoint will return
	 * 503 (venc_jpeg_backend_init() detects no source registered). */
	if (!g_mi_scl_port1_enabled) {
		i6c_scl_port scl_port1;
		memset(&scl_port1, 0, sizeof(scl_port1));
		scl_port1.crop.x = pcrop.x;
		scl_port1.crop.y = pcrop.y;
		scl_port1.crop.width = pcrop.w;
		scl_port1.crop.height = pcrop.h;
		scl_port1.output.width = (unsigned short)out_width;
		scl_port1.output.height = (unsigned short)out_height;
		scl_port1.pixFmt = I6_PIXFMT_YUV420SP;
		scl_port1.compress = 0; /* raw — MJPG VENC won't take IFC */
		MI_S32 p1_ret = g_mi_scl.fnSetPortConfig(0, 0, 1, &scl_port1);
		if (p1_ret != 0) {
			fprintf(stderr,
				"WARNING: [maruko] SCL port-1 SetPortConfig failed %d "
				"(snapshot disabled)\n", p1_ret);
		} else {
			p1_ret = g_mi_scl.fnEnablePort(0, 0, 1);
			if (p1_ret != 0) {
				fprintf(stderr,
					"WARNING: [maruko] SCL port-1 EnablePort failed %d "
					"(snapshot disabled)\n", p1_ret);
			} else {
				g_mi_scl_port1_enabled = 1;
				printf("> [maruko] SCL port 1 (snapshot): out(%ux%u) "
					"fmt=%d compress=%d\n",
					out_width, out_height,
					scl_port1.pixFmt, scl_port1.compress);
			}
		}
	}

	/* Report the sensor-absolute AR rect (pre-zoom) — same semantic as
	 * Star6E's venc_api_set_active_precrop call site. */
	venc_api_set_active_precrop(ar.x, ar.y, ar.w, ar.h);
	return 0;

fail:
	if (port)
		(void)g_mi_scl.fnDisablePort(0, 0, 0);
	if (started)
		(void)g_mi_scl.fnStopChannel(0, 0);
	if (chn)
		(void)g_mi_scl.fnDestroyChannel(0, 0);
	if (dev)
		(void)g_mi_scl.fnDestroyDevice(0);
	return ret ? ret : -1;
}

static int maruko_start_vpe(const SensorSelectResult *sensor,
	uint32_t out_width, uint32_t out_height, int vpe_level_3dnr,
	const PipelinePrecropRect *ar_crop,
	const PipelinePrecropRect *zoom_rect, int mirror, int flip)
{
	int isp_started = 0;

	if (configure_maruko_isp(sensor, vpe_level_3dnr) != 0)
		return -1;
	isp_started = 1;

	/* Sensor-level orientation — mirrors star6e MI_SNR_SetOrien usage.
	 * Non-fatal: some sensor drivers reject the call; log so regressions
	 * surface. */
	{
		MI_S32 orien_ret = MI_SNR_SetOrien(sensor->pad_id,
			(uint8_t)mirror, (uint8_t)flip);
		if (orien_ret != 0)
			fprintf(stderr, "[maruko] WARNING: MI_SNR_SetOrien"
				"(pad=%d mirror=%d flip=%d) returned %d\n",
				(int)sensor->pad_id, mirror, flip, (int)orien_ret);
	}

	if (configure_maruko_scl(sensor, out_width, out_height, ar_crop,
	    zoom_rect) != 0)
		goto fail_scl;

	return 0;

fail_scl:
	if (isp_started) {
		(void)g_mi_isp.fnDisablePort(0, 0, 0);
		(void)g_mi_isp.fnStopChannel(0, 0);
		(void)g_mi_isp.fnDestroyChannel(0, 0);
		(void)g_mi_isp.fnDestroyDevice(0);
	}
	return -1;
}

/* Poll an MI module's debug proc node until every output-port row
 * reports at most max_inflight tasks (workingTask_cnt / PipeBuf_cnt in
 * the "Output port common info" section; the worst row across all
 * ports is used, so multi-port configs drain too).
 * MI_SYS_UnBindChnPort runs an
 * UNBOUNDED uninterruptible kernel poll (MI_SYS_IMPL_FlushRealTimeOutputBuf)
 * for REALTIME/RING binds — if an in-flight buffer's consumer is already
 * gone, the process hangs in D-state forever.  max_inflight is 0 for a
 * stopped producer (truly drains) and 1 for the SCL->VENC RING leg,
 * whose ring keeps one resident slot busy the whole time it is bound.
 * This wait is bounded and advisory: on timeout or unreadable proc node
 * the caller proceeds anyway. */
static int maruko_wait_output_idle(const char *proc_path, int timeout_ms,
	int max_inflight)
{
	int waited = 0;

	while (1) {
		FILE *f = fopen(proc_path, "r");
		char line[256];
		int header_seen = 0, working = -1, pipebuf = -1;

		if (!f)
			return 0; /* no proc node — nothing to wait on */
		while (fgets(line, sizeof(line), f)) {
			if (strstr(line, "workingTask_cnt") &&
			    strstr(line, "finishedTask_cnt")) {
				header_seen = 1;
				continue;
			}
			if (header_seen) {
				int chn, pass, port, w, finished, pb, maxenq;
				if (sscanf(line, "%d %d %d %d %d %d %d",
				    &chn, &pass, &port, &w, &finished,
				    &pb, &maxenq) == 7) {
					if (w > working)
						working = w;
					if (pb > pipebuf)
						pipebuf = pb;
				} else if (working >= 0) {
					break; /* end of the port-row block */
				} else {
					header_seen = 0;
				}
			}
		}
		fclose(f);
		if (working <= max_inflight && pipebuf <= max_inflight)
			return 0;
		if (waited >= timeout_ms) {
			printf("> [maruko] output not idle after %dms "
				"(%s: working=%d pipebuf=%d) — proceeding\n",
				waited, proc_path, working, pipebuf);
			return -1;
		}
		usleep(10 * 1000);
		waited += 10;
	}
}

#define MARUKO_PROC_ISP "/proc/mi_modules/mi_isp/mi_isp0"
#define MARUKO_PROC_SCL "/proc/mi_modules/mi_scl/mi_scl0"

/* Stop the VPE channels (ISP quiesce + SCL destroy).  Reached only via
 * teardown_graph (full shutdown) — Maruko fork+exec respawns on every
 * reinit, so there is no in-process channel-recreate to guard.  The ISP
 * DestroyChannel is skipped deliberately: the kernel ISP retains CUS3A
 * mutex state that crashes a destroy+recreate cycle, and MI_SYS_Exit
 * reclaims the channel on process exit. */
static void maruko_stop_vpe_channels(void)
{
	/* Stop the producer (ISP) before destroying the consumer (SCL).
	 * An ISP->SCL REALTIME buffer still in flight when the SCL channel
	 * dies is never completed, and the later ISP->SCL unbind then hangs
	 * the process in D-state inside MI_SYS_IMPL_FlushRealTimeOutputBuf
	 * (observed once on a 1485/90fps SIGTERM teardown; no sysrq on the
	 * I6C kernel, only reboot -f recovers).  Quiesce ISP, give SCL a
	 * bounded window to drain the last frame, then destroy SCL. */
	if (g_mi_isp_chn_created) {
		(void)g_mi_isp.fnDisablePort(0, 0, 0);
		(void)g_mi_isp.fnStopChannel(0, 0);
		/* Skip DestroyChannel — kernel ISP retains CUS3A mutex
		 * state that crashes on destroy+recreate cycle. */
		(void)maruko_wait_output_idle(MARUKO_PROC_ISP, 200, 0);
	}
	if (g_mi_scl_chn_created) {
		(void)g_mi_scl.fnDisablePort(0, 0, 0);
		(void)g_mi_scl.fnStopChannel(0, 0);
		(void)g_mi_scl.fnDestroyChannel(0, 0);
		g_mi_scl_chn_created = 0;
	}
}

static void maruko_fill_h26x_attr(i6c_venc_attr_h26x *attr,
	uint32_t width, uint32_t height)
{
	attr->maxWidth = width;
	attr->maxHeight = height;
	attr->bufSize = width * height * 3 / 2;
	attr->profile = 0;
	attr->byFrame = 1;
	attr->width = width;
	attr->height = height;
	attr->bFrameNum = 0;
	attr->refNum = 1;
}

static void fill_maruko_rc_attr(i6c_venc_chn *attr,
	const MarukoBackendConfig *cfg, uint32_t gop, MI_U32 bit_rate_bits,
	uint32_t framerate)
{
	/* rc_mode values come from codec_config_resolve_codec_rc():
	 *   cbr=3, h265 vbr=4 avbr=5 qvbr=6, h264 vbr=2 avbr=0 qvbr=1. */
	if (cfg->rc_codec == PT_H265) {
		switch (cfg->rc_mode) {
		case 4: /* VBR */
			attr->rate.mode = MARUKO_VENC_RC_H265_VBR;
			attr->rate.h265Vbr = (i6c_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 45, .minQual = 20,
			};
			break;
		case 5: /* AVBR */
			attr->rate.mode = MARUKO_VENC_RC_H265_AVBR;
			attr->rate.h265Avbr = (i6c_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 45, .minQual = 20,
			};
			break;
		case 6: /* QVBR: VBR with tighter QP range */
			attr->rate.mode = MARUKO_VENC_RC_H265_VBR;
			attr->rate.h265Vbr = (i6c_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 40, .minQual = 28,
			};
			break;
		case 3: /* CBR */
		default:
			attr->rate.mode = MARUKO_VENC_RC_H265_CBR;
			attr->rate.h265Cbr = (i6c_venc_rate_h26xcbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.bitrate = bit_rate_bits, .avgLvl = 1,
			};
			break;
		}
	} else {
		switch (cfg->rc_mode) {
		case 2: /* VBR */
			attr->rate.mode = MARUKO_VENC_RC_H264_VBR;
			attr->rate.h264Vbr = (i6c_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 45, .minQual = 20,
			};
			break;
		case 0: /* AVBR */
			attr->rate.mode = MARUKO_VENC_RC_H264_AVBR;
			attr->rate.h264Avbr = (i6c_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 45, .minQual = 20,
			};
			break;
		case 1: /* QVBR */
			attr->rate.mode = MARUKO_VENC_RC_H264_VBR;
			attr->rate.h264Vbr = (i6c_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 40, .minQual = 28,
			};
			break;
		case 3: /* CBR */
		default:
			attr->rate.mode = MARUKO_VENC_RC_H264_CBR;
			attr->rate.h264Cbr = (i6c_venc_rate_h26xcbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.bitrate = bit_rate_bits, .avgLvl = 1,
			};
			break;
		}
	}
}

/* ── Digital zoom (Approach C: SCL crop, no upscale) ──────────────── */
/*
 * Same model as Star6E: zoom_pct shrinks BOTH the SCL output dim AND the
 * crop window — SCL reads the rect at 1:1 and emits it verbatim, no
 * upscale, no SCL bandwidth pressure.  The encoded resolution drops with
 * zoom (receiver sees the smaller dim in SPS/PPS).  Maruko's SCL has a
 * single SetPortConfig API that programs crop + output dim atomically;
 * live pan re-issues SetPortConfig with new crop offsets while keeping
 * the same output dim (ISP / VENC channels never resize).
 *
 * MaruWindowRect_t (defined near the top of this file) mirrors
 * MI_SYS_WindowRect_t.  Compute helpers live here so they're reusable
 * by both initial setup and live pan. */

/* Effective output dim for a given zoom_pct.  16-px alignment matches
 * SCL output and VENC create requirements; floor at 256 keeps the
 * encoded dim above VENC_CreateChn's minimum. */
static void maruko_compute_zoom_dim(uint32_t image_w, uint32_t image_h,
	double pct, uint32_t *out_w, uint32_t *out_h)
{
	const uint32_t ALIGN = 16;
	const uint32_t MIN_DIM = 256;
	double dw, dh;
	uint32_t w, h;

	if (!isfinite(pct) || pct <= 0.0 || pct >= 1.0) {
		if (out_w) *out_w = image_w;
		if (out_h) *out_h = image_h;
		return;
	}

	/* Promote pct so neither dim drops under MIN_DIM — keeps the zoom
	 * AR-preserving instead of squishing the shorter axis when the
	 * floor kicks in. */
	if ((double)image_w * pct < (double)MIN_DIM)
		pct = (double)MIN_DIM / (double)image_w;
	if ((double)image_h * pct < (double)MIN_DIM)
		pct = (double)MIN_DIM / (double)image_h;
	if (pct > 1.0) pct = 1.0;

	dw = (double)image_w * pct;
	dh = (double)image_h * pct;
	w = (uint32_t)(dw + 0.5);
	h = (uint32_t)(dh + 0.5);
	w &= ~(ALIGN - 1);
	h &= ~(ALIGN - 1);
	if (w < MIN_DIM) w = MIN_DIM;
	if (h < MIN_DIM) h = MIN_DIM;
	if (w > image_w) w = image_w & ~(ALIGN - 1);
	if (h > image_h) h = image_h & ~(ALIGN - 1);
	if (out_w) *out_w = w;
	if (out_h) *out_h = h;
}

/* Place a (rect_w × rect_h) 1:1 window inside (in_w × in_h) at fractional
 * (x, y) — typically the SCL input dim (post-precrop).  2-px aligned per
 * Maruko ISP/SCL spec.  Caller guarantees rect_w/h are 16-px-aligned via
 * compute_zoom_dim. */
static MaruWindowRect_t maruko_compute_zoom_rect(
	uint32_t in_w, uint32_t in_h, uint32_t rect_w, uint32_t rect_h,
	double x, double y)
{
	const uint32_t XY_ALIGN = 2;
	MaruWindowRect_t r;
	double cx, cy;
	uint32_t rx, ry;

	if (!isfinite(x)) x = 0.5;
	if (!isfinite(y)) y = 0.5;
	if (x < 0.0)
		x = 0.0;
	if (x > 1.0)
		x = 1.0;
	if (y < 0.0)
		y = 0.0;
	if (y > 1.0)
		y = 1.0;
	if (rect_w > in_w) rect_w = in_w & ~(XY_ALIGN - 1);
	if (rect_h > in_h) rect_h = in_h & ~(XY_ALIGN - 1);

	cx = (double)in_w * x - (double)rect_w * 0.5;
	cy = (double)in_h * y - (double)rect_h * 0.5;
	if (cx < 0.0) cx = 0.0;
	if (cy < 0.0) cy = 0.0;
	if (cx + (double)rect_w > (double)in_w) cx = (double)(in_w - rect_w);
	if (cy + (double)rect_h > (double)in_h) cy = (double)(in_h - rect_h);

	rx = (uint32_t)(cx + 0.5) & ~(XY_ALIGN - 1);
	ry = (uint32_t)(cy + 0.5) & ~(XY_ALIGN - 1);
	if (rx + rect_w > in_w) rx = (in_w - rect_w) & ~(XY_ALIGN - 1);
	if (ry + rect_h > in_h) ry = (in_h - rect_h) & ~(XY_ALIGN - 1);

	r.u16X = (uint16_t)rx;
	r.u16Y = (uint16_t)ry;
	r.u16Width  = (uint16_t)rect_w;
	r.u16Height = (uint16_t)rect_h;
	return r;
}

static MarukoZoomStatus g_zoom_status;
static pthread_mutex_t g_zoom_status_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t maruko_zoom_level_x100(double pct)
{
	if (!isfinite(pct) || pct <= 0.0)
		return 0;
	return (uint32_t)(100.0 / pct + 0.5);
}

static void maruko_pipeline_set_zoom_status(double pct,
	uint32_t output_w, uint32_t output_h,
	uint32_t crop_x, uint32_t crop_y, uint32_t crop_w, uint32_t crop_h)
{
	MarukoZoomStatus snap;

	memset(&snap, 0, sizeof(snap));
	snap.active = 1;
	snap.level_x100 = maruko_zoom_level_x100(pct);
	snap.output_w = output_w;
	snap.output_h = output_h;
	snap.crop_x = crop_x;
	snap.crop_y = crop_y;
	snap.crop_w = crop_w;
	snap.crop_h = crop_h;

	pthread_mutex_lock(&g_zoom_status_mutex);
	g_zoom_status = snap;
	pthread_mutex_unlock(&g_zoom_status_mutex);
}

static void maruko_pipeline_clear_zoom_status(void)
{
	pthread_mutex_lock(&g_zoom_status_mutex);
	memset(&g_zoom_status, 0, sizeof(g_zoom_status));
	pthread_mutex_unlock(&g_zoom_status_mutex);
}

void maruko_pipeline_zoom_status(MarukoZoomStatus *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&g_zoom_status_mutex);
	*out = g_zoom_status;
	pthread_mutex_unlock(&g_zoom_status_mutex);
}

/* Constrain the ISP's AE statistics window to the zoom rect.  Coords are
 * 0..1023 normalized against the SCL input surface (`scl_crop_*`).
 *
 * Failure modes inherited from the Star6E twin:
 *   - SDK refuses calls before CUS3A handoff is complete; gate on
 *     g_maruko_ae_crop_ready and stash the change in the cache.
 *   - Calling with a "full frame" 0..1023 rect returns -1 on some
 *     vintages — skip that path entirely and let AE re-equilibrate
 *     naturally when zoom is disabled.
 *   - On any non-zero return latch g_maruko_ae_crop_disabled and stop
 *     calling for the rest of the process; next start cold-resets. */
static void maruko_apply_ae_crop(MarukoBackendContext *ctx,
	double pct, double x, double y)
{
	MarukoAeCropRect r;
	uint32_t in_w, in_h;
	uint32_t rect_w, rect_h;
	double cx, cy, rx, ry;

	if (!ctx)
		return;
	if (g_maruko_ae_crop_disabled)
		return;
	if (!g_maruko_ae_crop_resolved) {
		void *h = g_mi_isp.handle;
		if (h)
			g_maruko_set_ae_crop = (maruko_set_ae_crop_fn_t)
				dlsym(h, "MI_ISP_CUS3A_SetAECropSize");
		if (!g_maruko_set_ae_crop)
			g_maruko_set_ae_crop = (maruko_set_ae_crop_fn_t)
				dlsym(RTLD_DEFAULT,
					"MI_ISP_CUS3A_SetAECropSize");
		g_maruko_ae_crop_resolved = 1;
		if (!g_maruko_set_ae_crop)
			fprintf(stderr,
				"WARNING: MI_ISP_CUS3A_SetAECropSize not present — "
				"AE will not track zoom\n");
	}
	if (!g_maruko_set_ae_crop)
		return;

	/* Skip the "full frame" path entirely (see header comment). */
	if (!isfinite(pct) || pct <= 0.0 || pct >= 1.0)
		return;

	in_w   = ctx->scl_crop_w;
	in_h   = ctx->scl_crop_h;
	rect_w = ctx->cfg.image_width;
	rect_h = ctx->cfg.image_height;
	if (in_w == 0 || in_h == 0 || rect_w == 0 || rect_h == 0)
		return;

	if (!isfinite(x)) x = 0.5;
	if (!isfinite(y)) y = 0.5;
	if (x < 0.0) x = 0.0;
	if (x > 1.0) x = 1.0;
	if (y < 0.0) y = 0.0;
	if (y > 1.0) y = 1.0;
	cx = (double)in_w * x - (double)rect_w * 0.5;
	cy = (double)in_h * y - (double)rect_h * 0.5;
	if (cx < 0.0) cx = 0.0;
	if (cy < 0.0) cy = 0.0;
	if (cx + (double)rect_w > (double)in_w)
		cx = (double)(in_w - rect_w);
	if (cy + (double)rect_h > (double)in_h)
		cy = (double)(in_h - rect_h);
	/* The AE grid spans the FULL ISP surface; the pan window lives on
	 * the (possibly input-cropped) SCL surface.  Translate to absolute
	 * ISP coords before normalizing to 0..1023. */
	{
		uint32_t full_w = ctx->sensor.plane.capt.width;
		uint32_t full_h = ctx->sensor.plane.capt.height;
		if (full_w == 0 || full_h == 0) {
			full_w = in_w;
			full_h = in_h;
		}
		rx = ((double)ctx->scl_crop_x + cx) * 1023.0 / (double)full_w;
		ry = ((double)ctx->scl_crop_y + cy) * 1023.0 / (double)full_h;
		memset(&r, 0, sizeof(r));
		r.crop_x = (uint16_t)(rx + 0.5);
		r.crop_y = (uint16_t)(ry + 0.5);
		r.crop_w = (uint16_t)((double)rect_w * 1023.0 /
			(double)full_w + 0.5);
		r.crop_h = (uint16_t)((double)rect_h * 1023.0 /
			(double)full_h + 0.5);
	}
	if (r.crop_w == 0) r.crop_w = 1;
	if (r.crop_h == 0) r.crop_h = 1;
	if (r.crop_x + r.crop_w > 1023)
		r.crop_x = (uint16_t)(1023 - r.crop_w);
	if (r.crop_y + r.crop_h > 1023)
		r.crop_y = (uint16_t)(1023 - r.crop_h);

	if (r.crop_x == g_maruko_ae_crop_last.crop_x &&
	    r.crop_y == g_maruko_ae_crop_last.crop_y &&
	    r.crop_w == g_maruko_ae_crop_last.crop_w &&
	    r.crop_h == g_maruko_ae_crop_last.crop_h)
		return;

	g_maruko_ae_crop_last = r;

	if (!g_maruko_ae_crop_ready)
		return;  /* queued — CUS3A not yet ready */

	if (g_maruko_set_ae_crop(0, 0, &r) != 0) {
		fprintf(stderr,
			"WARNING: MI_ISP_CUS3A_SetAECropSize(%u,%u,%u,%u) failed — "
			"disabling AE zoom-tracking for this run\n",
			r.crop_x, r.crop_y, r.crop_w, r.crop_h);
		g_maruko_ae_crop_disabled = 1;
	}
}

/* Called from bind_maruko_pipeline after the CUS3A handoff block runs
 * (cold boot) or is known good (reinit, framework persists across the
 * in-process pipeline reset).  Force a re-push by zeroing the cached w
 * so apply_ae_crop's dedup short-circuit can't swallow the call. */
static void maruko_ae_crop_mark_ready(MarukoBackendContext *ctx)
{
	g_maruko_ae_crop_ready = 1;
	g_maruko_ae_crop_last.crop_w = 0;
	if (ctx)
		maruko_apply_ae_crop(ctx, ctx->cfg.zoom_pct,
			ctx->cfg.zoom_x, ctx->cfg.zoom_y);
}

/* Program the SCL port crop for (pct, x, y).  Caller must already hold a
 * valid ctx and a created SCL channel; the AR-precrop rect must be stashed
 * in ctx->scl_crop_*.  Side effects: updates the public zoom_status
 * snapshot and pushes the matching AE crop window. */
static int maruko_pan_apply_locked(MarukoBackendContext *ctx,
	double pct, double x, double y)
{
	MaruWindowRect_t rect;
	i6c_scl_port scl_port;
	uint32_t scl_crop_x, scl_crop_y, scl_crop_w, scl_crop_h;
	uint32_t crop_x, crop_y;
	MI_S32 ret;

	if (!ctx) return -1;
	if (!g_mi_scl_chn_created)
		return -1;

	scl_crop_x = ctx->scl_crop_x;
	scl_crop_y = ctx->scl_crop_y;
	scl_crop_w = ctx->scl_crop_w;
	scl_crop_h = ctx->scl_crop_h;
	if (scl_crop_w == 0 || scl_crop_h == 0)
		return -1;

	rect = maruko_compute_zoom_rect(scl_crop_w, scl_crop_h,
		ctx->cfg.image_width, ctx->cfg.image_height, x, y);
	crop_x = scl_crop_x + rect.u16X;
	crop_y = scl_crop_y + rect.u16Y;

	memset(&scl_port, 0, sizeof(scl_port));
	scl_port.crop.x = (unsigned short)crop_x;
	scl_port.crop.y = (unsigned short)crop_y;
	scl_port.crop.width = rect.u16Width;
	scl_port.crop.height = rect.u16Height;
	scl_port.output.width = (unsigned short)ctx->cfg.image_width;
	scl_port.output.height = (unsigned short)ctx->cfg.image_height;
	scl_port.pixFmt = I6_PIXFMT_YUV420SP;
	scl_port.compress = (i6_common_compr)6;  /* IFC */

	ret = g_mi_scl.fnSetPortConfig(0, 0, 0, &scl_port);
	if (ret != 0) {
		fprintf(stderr,
			"[maruko] WARNING: SCL pan SetPortConfig failed %d "
			"(rect %ux%u+%u+%u out %ux%u)\n",
			(int)ret, rect.u16Width, rect.u16Height,
			crop_x, crop_y,
			ctx->cfg.image_width, ctx->cfg.image_height);
		return -1;
	}
	maruko_pipeline_set_zoom_status(pct, ctx->cfg.image_width,
		ctx->cfg.image_height, crop_x, crop_y,
		rect.u16Width, rect.u16Height);
	maruko_apply_ae_crop(ctx, pct, x, y);
	return 0;
}

static void *maruko_pan_ramp_thread(void *arg)
{
	(void)arg;
	for (;;) {
		struct timespec ts;
		MarukoBackendContext *ctx;
		double tx, ty, pct, cx, cy, alpha;
		uint32_t ramp_ms;
		int has_state;

		pthread_mutex_lock(&g_maruko_pan_ramp.lock);
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += (long)MARUKO_PAN_RAMP_TICK_MS * 1000000L;
		while (ts.tv_nsec >= 1000000000L) {
			ts.tv_nsec -= 1000000000L;
			ts.tv_sec  += 1;
		}
		while (g_maruko_pan_ramp.running &&
		    fabs(g_maruko_pan_ramp.target_x - g_maruko_pan_ramp.current_x) < MARUKO_PAN_RAMP_SNAP_EPSILON &&
		    fabs(g_maruko_pan_ramp.target_y - g_maruko_pan_ramp.current_y) < MARUKO_PAN_RAMP_SNAP_EPSILON) {
			int rc = pthread_cond_wait(&g_maruko_pan_ramp.cv,
				&g_maruko_pan_ramp.lock);
			if (rc != 0)
				break;
		}
		if (!g_maruko_pan_ramp.running) {
			pthread_mutex_unlock(&g_maruko_pan_ramp.lock);
			break;
		}
		(void)pthread_cond_timedwait(&g_maruko_pan_ramp.cv,
			&g_maruko_pan_ramp.lock, &ts);
		if (!g_maruko_pan_ramp.running) {
			pthread_mutex_unlock(&g_maruko_pan_ramp.lock);
			break;
		}

		ctx       = g_maruko_pan_ramp.ctx;
		has_state = g_maruko_pan_ramp.has_state;
		ramp_ms   = g_maruko_pan_ramp.ramp_ms;
		tx        = g_maruko_pan_ramp.target_x;
		ty        = g_maruko_pan_ramp.target_y;
		pct       = g_maruko_pan_ramp.target_pct;

		if (!has_state || !ctx || pct <= 0.0 || pct >= 1.0) {
			g_maruko_pan_ramp.current_x = tx;
			g_maruko_pan_ramp.current_y = ty;
			pthread_mutex_unlock(&g_maruko_pan_ramp.lock);
			continue;
		}

		if (ramp_ms == 0) {
			alpha = 1.0;
		} else {
			alpha = 1.0 - exp(-(double)MARUKO_PAN_RAMP_TICK_MS /
				(double)ramp_ms);
			if (alpha > 1.0) alpha = 1.0;
			if (alpha < 0.0) alpha = 0.0;
		}

		g_maruko_pan_ramp.current_x += (tx - g_maruko_pan_ramp.current_x) * alpha;
		g_maruko_pan_ramp.current_y += (ty - g_maruko_pan_ramp.current_y) * alpha;
		if (fabs(tx - g_maruko_pan_ramp.current_x) < MARUKO_PAN_RAMP_SNAP_EPSILON)
			g_maruko_pan_ramp.current_x = tx;
		if (fabs(ty - g_maruko_pan_ramp.current_y) < MARUKO_PAN_RAMP_SNAP_EPSILON)
			g_maruko_pan_ramp.current_y = ty;

		cx = g_maruko_pan_ramp.current_x;
		cy = g_maruko_pan_ramp.current_y;
		pthread_mutex_unlock(&g_maruko_pan_ramp.lock);

		(void)maruko_pan_apply_locked(ctx, pct, cx, cy);
	}
	return NULL;
}

static int maruko_pan_ramp_start(MarukoBackendContext *ctx,
	double pct, double x, double y)
{
	int rc;

	pthread_mutex_lock(&g_maruko_pan_ramp.lock);
	if (g_maruko_pan_ramp.running) {
		/* Re-arm without recreating the thread. */
		g_maruko_pan_ramp.ctx         = ctx;
		g_maruko_pan_ramp.has_state   = 1;
		g_maruko_pan_ramp.target_pct  = pct;
		g_maruko_pan_ramp.target_x    = x;
		g_maruko_pan_ramp.target_y    = y;
		g_maruko_pan_ramp.current_x   = x;
		g_maruko_pan_ramp.current_y   = y;
		g_maruko_pan_ramp.ramp_ms     = MARUKO_PAN_RAMP_DEFAULT_MS;
		pthread_cond_signal(&g_maruko_pan_ramp.cv);
		pthread_mutex_unlock(&g_maruko_pan_ramp.lock);
		return 0;
	}
	g_maruko_pan_ramp.ctx         = ctx;
	g_maruko_pan_ramp.has_state   = 1;
	g_maruko_pan_ramp.target_pct  = pct;
	g_maruko_pan_ramp.target_x    = x;
	g_maruko_pan_ramp.target_y    = y;
	g_maruko_pan_ramp.current_x   = x;
	g_maruko_pan_ramp.current_y   = y;
	g_maruko_pan_ramp.ramp_ms     = MARUKO_PAN_RAMP_DEFAULT_MS;
	g_maruko_pan_ramp.running     = 1;
	pthread_mutex_unlock(&g_maruko_pan_ramp.lock);

	rc = pthread_create(&g_maruko_pan_ramp.thread, NULL,
		maruko_pan_ramp_thread, NULL);
	if (rc != 0) {
		fprintf(stderr,
			"WARNING: pthread_create(maruko_pan_ramp) failed %d (%s)\n",
			rc, strerror(rc));
		pthread_mutex_lock(&g_maruko_pan_ramp.lock);
		g_maruko_pan_ramp.running   = 0;
		g_maruko_pan_ramp.has_state = 0;
		g_maruko_pan_ramp.ctx       = NULL;
		pthread_mutex_unlock(&g_maruko_pan_ramp.lock);
		return -1;
	}
	return 0;
}

static void maruko_pan_ramp_stop(void)
{
	pthread_t th;
	int was_running;

	pthread_mutex_lock(&g_maruko_pan_ramp.lock);
	was_running               = g_maruko_pan_ramp.running;
	g_maruko_pan_ramp.running = 0;
	g_maruko_pan_ramp.has_state = 0;
	g_maruko_pan_ramp.ctx     = NULL;
	th                        = g_maruko_pan_ramp.thread;
	pthread_cond_broadcast(&g_maruko_pan_ramp.cv);
	pthread_mutex_unlock(&g_maruko_pan_ramp.lock);

	if (was_running)
		pthread_join(th, NULL);
}

/* ── Framing-module registry (parallel of star6e_pipeline.c:60-88) ─────────── */
#define MARUKO_FRAMING_MAX 4
static const MarukoFramingModule *g_maruko_framing_registry[MARUKO_FRAMING_MAX];
static int g_maruko_framing_count;
static const MarukoFramingModule *g_maruko_framing;

void maruko_framing_register(const MarukoFramingModule *m)
{
	if (m && g_maruko_framing_count < MARUKO_FRAMING_MAX)
		g_maruko_framing_registry[g_maruko_framing_count++] = m;
}

const MarukoFramingModule *maruko_framing_select(const VencConfig *vcfg)
{
	int i;
	for (i = 0; i < g_maruko_framing_count; i++)
		if (g_maruko_framing_registry[i]->enabled(vcfg))
			return g_maruko_framing_registry[i];
	return NULL;
}

void maruko_framing_register_builtins(void)
{
	if (g_maruko_framing_count > 0)
		return;   /* idempotent */
#if HAVE_FRAMING_STAB
	{
		extern const MarukoFramingModule maruko_framing_stab;
		extern const MarukoFramingModule maruko_framing_stab_fill;
		maruko_framing_register(&maruko_framing_stab);
		maruko_framing_register(&maruko_framing_stab_fill);
	}
#endif
}

/* Host callbacks for framing modules — thin wrappers over the file-static
 * helpers above (see include/maruko_framing_host.h). */
void maruko_framing_apply_ae_crop(MarukoBackendContext *ctx,
	double pct, double x, double y)
{
	maruko_apply_ae_crop(ctx, pct, x, y);
}
int maruko_framing_fill_graph_ready(void)
{
	return g_stab_fill_graph;
}

void maruko_framing_pan_ramp_stop(void)
{
	maruko_pan_ramp_stop();
}

/* Select + bring up the framing module for this run.  Called after
 * configure_graph — scl_crop is set, VENC is bound, the SCL ports are up.
 * Returns 0 whether or not a module engaged (g_maruko_framing == NULL if not). */
int maruko_pipeline_framing_setup(MarukoBackendContext *ctx,
	const VencConfig *vcfg)
{
	const MarukoFramingModule *m;
	uint32_t ew = 0, eh = 0;

	g_maruko_framing = NULL;
	if (!ctx || !vcfg)
		return 0;
	m = maruko_framing_select(vcfg);
	/* In stab-fill the framing module is the frame-base VENC's ONLY producer
	 * — the graph was built with port0 unbound and no RING leg, so degrading
	 * to "off" here would ship a healthy-looking daemon that streams nothing,
	 * forever.  Any fill bring-up failure is therefore FATAL (the daemon
	 * exits visibly; a config fix or respawn retries).  Other presets keep
	 * the graceful degrade: the RING leg is intact without them. */
	if (!m)
		return g_stab_fill_graph ? -1 : 0;
	if (m->prepare)
		(void)m->prepare(vcfg, ctx->scl_crop_w, ctx->scl_crop_h, &ew, &eh);
	if (m->setup_ports &&
	    m->setup_ports(ctx, ctx->sensor.fps, ctx->sensor.fps) != 0) {
		fprintf(stderr,
			"> [maruko] framing '%s' setup_ports failed — %s\n",
			m->preset_name, g_stab_fill_graph ? "FATAL" : "off");
		return g_stab_fill_graph ? -1 : 0;
	}
	if (m->start && m->start() != 0) {
		fprintf(stderr, "> [maruko] framing '%s' start failed — %s\n",
			m->preset_name, g_stab_fill_graph ? "FATAL" : "off");
		if (m->stop)
			m->stop();
		return g_stab_fill_graph ? -1 : 0;
	}
	g_maruko_framing = m;
	printf("> [maruko] framing '%s' active\n", m->preset_name);
	return 0;
}

/* Stop the active framing module (join detector, disable tap) — MUST run before
 * teardown_graph disables the SCL port0 the module drives. */
void maruko_pipeline_framing_stop(void)
{
	if (g_maruko_framing) {
		if (g_maruko_framing->stop)
			g_maruko_framing->stop();
		g_maruko_framing = NULL;
	}
}

/* Live pause for the active framing module (video0.pause_stab).  Mirrors
 * star6e_pipeline_set_pause_stab. */
int maruko_pipeline_set_pause_stab(bool paused)
{
	if (g_maruko_framing && g_maruko_framing->set_live)
		return g_maruko_framing->set_live("pause", paused ? "1" : "0");
	return -1;
}

/* Live pan: zoom_pct is MUT_RESTART (encoder dim change), so the live path
 * only updates x/y.  Updates the *target*; the ramp thread tweens
 * `current` toward it via maruko_pan_apply_locked.  pct is accepted to
 * short-circuit when zoom is off (no rect to pan). */
int maruko_pipeline_apply_zoom(MarukoBackendContext *ctx,
	double pct, double x, double y)
{
	if (!ctx) return -1;
	if (!isfinite(pct) || !isfinite(x) || !isfinite(y))
		return -1;

	/* A framing module (stab) owns the SCL port-0 crop and runs its own pan
	 * ramp-free.  Route x/y pan to it and swallow the zoom/ramp path so the
	 * two never fight over port 0 (R2).  Mirrors star6e_pipeline.c's
	 * active()->set_pan override. */
	if (g_maruko_framing && g_maruko_framing->active &&
	    g_maruko_framing->active()) {
		if (g_maruko_framing->set_pan)
			g_maruko_framing->set_pan(x, y);
		return 0;
	}
	if (pct <= 0.0 || pct >= 1.0) {
		maruko_pipeline_clear_zoom_status();
		pthread_mutex_lock(&g_maruko_pan_ramp.lock);
		g_maruko_pan_ramp.target_pct = 0.0;
		g_maruko_pan_ramp.target_x   = x;
		g_maruko_pan_ramp.target_y   = y;
		g_maruko_pan_ramp.current_x  = x;
		g_maruko_pan_ramp.current_y  = y;
		pthread_cond_signal(&g_maruko_pan_ramp.cv);
		pthread_mutex_unlock(&g_maruko_pan_ramp.lock);
		maruko_apply_ae_crop(ctx, pct, x, y);
		return 0;  /* zoom off — nothing to pan */
	}

	/* SCL channel must be created; otherwise SetPortConfig has nothing
	 * to update. */
	if (!g_mi_scl_chn_created)
		return -1;
	if (ctx->scl_crop_w == 0 || ctx->scl_crop_h == 0)
		return -1;

	/* If the ramp thread isn't up yet, spawn it on first live pan with
	 * current==target so the smoothing engages from the next SET. */
	pthread_mutex_lock(&g_maruko_pan_ramp.lock);
	int already_running = g_maruko_pan_ramp.running;
	pthread_mutex_unlock(&g_maruko_pan_ramp.lock);
	if (!already_running) {
		(void)maruko_pan_ramp_start(ctx, pct, x, y);
		return 0;
	}

	pthread_mutex_lock(&g_maruko_pan_ramp.lock);
	g_maruko_pan_ramp.ctx        = ctx;
	g_maruko_pan_ramp.has_state  = 1;
	g_maruko_pan_ramp.target_pct = pct;
	g_maruko_pan_ramp.target_x   = x;
	g_maruko_pan_ramp.target_y   = y;
	pthread_cond_signal(&g_maruko_pan_ramp.cv);
	pthread_mutex_unlock(&g_maruko_pan_ramp.lock);

	/* AE crop still fires on the user-requested target (not the smoothed
	 * intermediate value): the meter window stays committed to where the
	 * pan WILL be, and the rect is identical from frame 1 of the SET so
	 * deduplication is robust.  Same approach as Star6E. */
	maruko_apply_ae_crop(ctx, pct, x, y);
	return 0;
}

/* IntraRefresh status snapshot — populated by
 * maruko_pipeline_apply_intra_refresh() at every pipeline_start, cleared
 * by the VENC destroy step of teardown_graph.  Read by venc_api's
 * /api/v1/intra/status handler. */
static MarukoIntraRefreshStatus g_intra_status;
static pthread_mutex_t g_intra_status_mutex = PTHREAD_MUTEX_INITIALIZER;

void maruko_pipeline_intra_refresh_status(MarukoIntraRefreshStatus *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&g_intra_status_mutex);
	*out = g_intra_status;
	pthread_mutex_unlock(&g_intra_status_mutex);
}

/* Compute IntraRefresh derived params from the maruko config snapshot. */
static IntraRefreshMode maruko_intra_refresh_derive(
	const MarukoBackendConfig *cfg, uint32_t height, uint32_t fps,
	PAYLOAD_TYPE_E codec, IntraRefreshDerived *out_ir)
{
	IntraRefreshMode mode = INTRA_MODE_OFF;

	memset(out_ir, 0, sizeof(*out_ir));
	if (cfg) {
		mode = intra_refresh_parse_mode(cfg->intra_refresh_mode);
		(void)codec; /* H.265 only */
		intra_refresh_compute(mode, height, fps,
			cfg->intra_refresh_lines, cfg->intra_refresh_qp,
			cfg->gop_size_sec, out_ir);
	}
	return mode;
}

static int maruko_apply_intra_refresh(MI_VENC_DEV dev, MI_VENC_CHN chn,
	const MarukoBackendConfig *cfg, uint32_t height, uint32_t fps,
	PAYLOAD_TYPE_E codec)
{
	MI_VENC_IntraRefresh_t ir_sdk;
	MarukoIntraRefreshStatus snap;
	IntraRefreshDerived ir;
	IntraRefreshMode mode;
	const char *name;

	memset(&snap, 0, sizeof(snap));
	mode = maruko_intra_refresh_derive(cfg, height, fps, codec, &ir);
	name = intra_refresh_mode_name(mode);

	snprintf(snap.mode_name, sizeof(snap.mode_name), "%s", name);
	snap.mi_supported = g_mi_venc.fnSetIntraRefresh ? 1 : 0;
	if (cfg) {
		snap.requested_lines  = cfg->intra_refresh_lines;
		snap.requested_qp     = cfg->intra_refresh_qp;
		snap.explicit_gop_sec = cfg->gop_size_sec;
	}
	snap.target_ms             = ir.target_ms;
	snap.total_rows            = ir.total_rows;
	snap.effective_lines_per_p = ir.lines;
	snap.lines_clamped         = ir.lines_clamped;
	snap.effective_qp          = ir.req_iqp;
	snap.effective_gop_sec     = ir.gop_overridden ? snap.explicit_gop_sec : ir.gop_sec;
	snap.gop_auto              = ir.gop_overridden ? 0 : (ir.gop_sec > 0.0);

	if (mode == INTRA_MODE_OFF) {
		pthread_mutex_lock(&g_intra_status_mutex);
		g_intra_status = snap;
		pthread_mutex_unlock(&g_intra_status_mutex);
		return 0;
	}
	if (!g_mi_venc.fnSetIntraRefresh) {
		fprintf(stderr, "[waybeam] WARNING: intraRefreshMode=%s requested "
			"but libmi_venc.so does not export "
			"MI_VENC_SetIntraRefresh\n", name);
		pthread_mutex_lock(&g_intra_status_mutex);
		g_intra_status = snap;
		pthread_mutex_unlock(&g_intra_status_mutex);
		return -1;
	}
	if (ir.lines_clamped) {
		fprintf(stderr, "[waybeam] WARNING: intraRefreshLines exceeds picture "
			"LCU rows=%u, clamped\n", ir.total_rows);
	}
	if (ir.gop_overridden) {
		fprintf(stderr, "[waybeam] intra auto-GOP suppressed: explicit "
			"gopSize=%.2fs\n", snap.explicit_gop_sec);
	}

	memset(&ir_sdk, 0, sizeof(ir_sdk));
	ir_sdk.bEnable = 1;
	ir_sdk.u32RefreshLineNum = ir.lines;
	ir_sdk.u32ReqIQp = ir.req_iqp;

	if (maruko_mi_venc_set_intra_refresh(dev, chn, &ir_sdk) != 0) {
		fprintf(stderr, "[waybeam] ERROR: MI_VENC_SetIntraRefresh(dev=%d, "
			"chn=%d, lines=%u, qp=%u) failed\n", dev, chn,
			ir_sdk.u32RefreshLineNum, ir_sdk.u32ReqIQp);
		pthread_mutex_lock(&g_intra_status_mutex);
		g_intra_status = snap;
		pthread_mutex_unlock(&g_intra_status_mutex);
		return -1;
	}
	snap.apply_ok = 1;
	snap.active   = 1;
	pthread_mutex_lock(&g_intra_status_mutex);
	g_intra_status = snap;
	pthread_mutex_unlock(&g_intra_status_mutex);
	fprintf(stderr, "[waybeam] intraRefresh: mode=%s dev=%d chn=%d lines/P=%u "
		"qp=%u gop=%.2fs (%s)\n", name, dev, chn, ir_sdk.u32RefreshLineNum,
		ir_sdk.u32ReqIQp, snap.effective_gop_sec,
		snap.gop_auto ? "auto" : "explicit");
	return 0;
}

static MarukoRefPredStatus g_ref_pred_status;
static pthread_mutex_t g_ref_pred_status_mutex = PTHREAD_MUTEX_INITIALIZER;

void maruko_pipeline_ref_pred_status(MarukoRefPredStatus *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&g_ref_pred_status_mutex);
	*out = g_ref_pred_status;
	pthread_mutex_unlock(&g_ref_pred_status_mutex);
}

static int maruko_apply_ref_pred(MI_VENC_DEV dev, MI_VENC_CHN chn,
	const MarukoBackendConfig *cfg)
{
	MI_VENC_ParamRef_t ref;
	MarukoRefPredStatus snap;

	memset(&snap, 0, sizeof(snap));
	snap.mi_supported = g_mi_venc.fnSetRefParam ? 1 : 0;
	if (cfg) {
		snap.base    = cfg->ref_base;
		snap.enhance = cfg->ref_enhance;
		snap.pred    = cfg->ref_pred ? 1 : 0;
	}

	if (!cfg || cfg->ref_base == 0)
		goto publish;
	if (!g_mi_venc.fnSetRefParam) {
		fprintf(stderr, "[waybeam] WARNING: refBase=%u requested but "
			"libmi_venc.so does not export MI_VENC_SetRefParam\n",
			cfg->ref_base);
		goto publish;
	}

	memset(&ref, 0, sizeof(ref));
	ref.u32Base     = cfg->ref_base;
	ref.u32Enhance  = cfg->ref_enhance ? cfg->ref_enhance : 1;
	ref.bEnablePred = cfg->ref_pred ? 1 : 0;

	if (maruko_mi_venc_set_ref_param(dev, chn, &ref) != 0) {
		fprintf(stderr, "[waybeam] ERROR: MI_VENC_SetRefParam(dev=%d, "
			"chn=%d, base=%u, enhance=%u, pred=%u) failed\n", dev, chn,
			ref.u32Base, ref.u32Enhance, ref.bEnablePred);
		goto publish;
	}
	snap.apply_ok = 1;
	snap.active   = 1;
	fprintf(stderr, "[waybeam] refPred: dev=%d chn=%d base=%u enhance=%u "
		"pred=%u (applied)\n", dev, chn, ref.u32Base, ref.u32Enhance,
		ref.bEnablePred);
publish:
	pthread_mutex_lock(&g_ref_pred_status_mutex);
	g_ref_pred_status = snap;
	pthread_mutex_unlock(&g_ref_pred_status_mutex);
	return snap.active ? 0 : (cfg && cfg->ref_base > 0 ? -1 : 0);
}

static int maruko_apply_slice_split(MI_VENC_DEV dev, MI_VENC_CHN chn,
	const MarukoBackendConfig *cfg, uint32_t height)
{
	MI_VENC_ParamH265SliceSplit_t requested;
	MI_VENC_ParamH265SliceSplit_t actual;
	uint32_t rows32;
	uint32_t rows_per_slice;
	uint32_t ctu_rows;
	uint32_t ctu_per_slice;
	uint32_t predicted;

	if (!cfg || cfg->slice_count <= 1)
		return 0;
	if (cfg->rc_codec != PT_H265) {
		fprintf(stderr, "ERROR: [maruko] sliceCount=%u requires H.265\n",
			cfg->slice_count);
		return -1;
	}
	if (!g_mi_venc.fnSetH265SliceSplit ||
	    !g_mi_venc.fnGetH265SliceSplit) {
		fprintf(stderr, "ERROR: [maruko] sliceCount=%u requested but "
			"libmi_venc.so lacks H.265 slice split Set/Get\n",
			cfg->slice_count);
		return -1;
	}

	rows32 = (height + 31U) / 32U;
	rows_per_slice = (rows32 + cfg->slice_count - 1U) /
		cfg->slice_count;
	memset(&requested, 0, sizeof(requested));
	requested.bSplitEnable = 1;
	requested.u32SliceRowCount = rows_per_slice;
	if (maruko_mi_venc_set_h265_slice_split(dev, chn, &requested) != 0) {
		fprintf(stderr, "ERROR: [maruko] MI_VENC_SetH265SliceSplit"
			"(dev=%d, chn=%d, rows=%u) failed\n",
			dev, chn, rows_per_slice);
		return -1;
	}

	memset(&actual, 0, sizeof(actual));
	if (maruko_mi_venc_get_h265_slice_split(dev, chn, &actual) != 0 ||
	    !actual.bSplitEnable ||
	    actual.u32SliceRowCount == 0 ||
	    actual.u32SliceRowCount > rows32) {
		fprintf(stderr, "ERROR: [maruko] H.265 slice split readback "
			"invalid: enable=%u rows=%u requested_rows=%u\n",
			(unsigned int)actual.bSplitEnable,
			actual.u32SliceRowCount, rows_per_slice);
		return -1;
	}

	ctu_rows = (height + 63U) / 64U;
	ctu_per_slice = (actual.u32SliceRowCount + 1U) / 2U;
	predicted = (ctu_rows + ctu_per_slice - 1U) / ctu_per_slice;
	fprintf(stderr, "[waybeam] sliceSplit: dev=%d chn=%d requested=%u "
		"requested_rows32=%u applied_rows32=%u predicted=%u "
		"(readback verified)\n", dev, chn, cfg->slice_count,
		rows_per_slice, actual.u32SliceRowCount, predicted);
	return 0;
}

static int maruko_start_venc(const MarukoBackendConfig *cfg,
	uint32_t width, uint32_t height, uint32_t framerate,
	MI_VENC_DEV venc_dev, MI_VENC_CHN *chn, int *dev_created)
{
	if (dev_created)
		*dev_created = 0;

	i6c_venc_init init = {
		.maxWidth = 4096,
		.maxHeight = 2176,
	};
	MI_S32 ret = maruko_mi_venc_create_dev(venc_dev, &init);
	if (ret == 0) {
		if (dev_created)
			*dev_created = 1;
	} else {
		fprintf(stderr,
			"WARNING: [maruko] MI_VENC_CreateDev failed %d"
			" (continuing)\n", ret);
	}

	/* Ring pool MUST be configured before CreateChannel (matching
	 * majestic i6c_hal.c order: pool → CreateChn → SetSource → Start).
	 * stab-fill skips it: the ring is the RING-DMA input backing store; the
	 * NORMAL_FRMBASE input allocates its own ~3-buffer handshake pool. */
	if (!g_stab_fill_graph) {
		MI_U16 venc_ring = (MI_U16)height;
		if (venc_ring == 0)
			venc_ring = 1;
		MI_S32 pool_ret = maruko_config_dev_ring_pool(I6C_SYS_MOD_VENC,
			(MI_U32)venc_dev, (MI_U16)width, (MI_U16)height, venc_ring);
		printf("> [maruko] VENC ring pool: %ux%u ring=%u ret=%d\n",
			width, height, venc_ring, pool_ret);
	}

	i6c_venc_chn attr = {0};
	if (cfg->rc_codec == PT_H265) {
		attr.attrib.codec = I6C_VENC_CODEC_H265;
		maruko_fill_h26x_attr(&attr.attrib.h265, width, height);
	} else {
		attr.attrib.codec = I6C_VENC_CODEC_H264;
		maruko_fill_h26x_attr(&attr.attrib.h264, width, height);
	}

	uint32_t gop = cfg->venc_gop_size;
	if (gop == 0)
		gop = 1;
	/* Same practical bitrate bounds as apply_bitrate() so a persisted
	 * sub-floor bitrate can't birth the encoder collapsed at boot. */
	uint32_t rate_kbps = cfg->venc_max_rate;
	if (rate_kbps > VENC_BITRATE_MAX_KBPS)
		rate_kbps = VENC_BITRATE_MAX_KBPS;
	if (rate_kbps < VENC_BITRATE_MIN_KBPS)
		rate_kbps = VENC_BITRATE_MIN_KBPS;
	MI_U32 bit_rate_bits = rate_kbps * 1024;

	fill_maruko_rc_attr(&attr, cfg, gop, bit_rate_bits, framerate);

	*chn = 0;
	ret = maruko_mi_venc_create_chn(venc_dev, *chn, &attr);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_VENC_CreateChn failed %d\n", ret);
		if (dev_created && *dev_created) {
			(void)maruko_mi_venc_destroy_dev(venc_dev);
			*dev_created = 0;
		}
		return ret;
	}
	ret = maruko_apply_slice_split(venc_dev, *chn, cfg, height);
	if (ret != 0) {
		(void)maruko_mi_venc_destroy_chn(venc_dev, *chn);
		if (dev_created && *dev_created) {
			(void)maruko_mi_venc_destroy_dev(venc_dev);
			*dev_created = 0;
		}
		return ret;
	}

	/* stab-fill: NORMAL (frame-base) input — required for the manual
	 * ChnInputPortGetBuf/PutBuf feed (NORMAL == E_MI_VENC_INPUT_MODE_
	 * NORMAL_FRMBASE = 0).  Everything else: RING_DMA fed by the zero-copy
	 * SCL ring. */
	i6c_venc_src_conf input_mode = g_stab_fill_graph ?
		I6C_VENC_SRC_CONF_NORMAL : I6C_VENC_SRC_CONF_RING_DMA;
	ret = maruko_mi_venc_set_input_source(venc_dev, *chn, &input_mode);
	if (ret != 0) {
		fprintf(stderr,
			"WARNING: [maruko] MI_VENC_SetInputSourceConfig"
			" failed %d\n", ret);
	}

	/* SVC-T reference pyramid (refPred) — Star6E SDK testing showed the
	 * call silently no-ops if invoked after StartRecvPic.  Apply here
	 * between CreateChn and StartRecvPic for both backends. */
	(void)maruko_apply_ref_pred(venc_dev, *chn, cfg);

	/* Cap the bitstream buffer before encoding starts — see
	 * FRAME_GATE_STREAM_BUF_FRAMES.  Advisory: a refusal leaves the SDK
	 * default depth, which costs latency while gated and nothing else.
	 * Must sit ABOVE the stab-fill early return so both StartRecvPic
	 * paths (here and maruko_setup_stabfill_venc) get it; the SDK wants
	 * it after channel creation and before encoding starts either way. */
	if (!g_mi_venc.fnSetMaxStreamCnt ||
	    g_mi_venc.fnSetMaxStreamCnt(venc_dev, *chn,
		    FRAME_GATE_STREAM_BUF_FRAMES) != 0)
		fprintf(stderr, "WARNING: [maruko] SetMaxStreamCnt(%u) "
			"refused; keeping the SDK default depth\n",
			FRAME_GATE_STREAM_BUF_FRAMES);

	/* stab-fill: StartRecvPic (+ IntraRefresh, which must follow it) is
	 * deferred to maruko_setup_stabfill_venc — after the rest of the graph
	 * is assembled, before the fill module's first push. */
	if (g_stab_fill_graph)
		return 0;

	ret = maruko_mi_venc_start_recv(venc_dev, *chn);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_VENC_StartRecvPic failed %d\n",
			ret);
		(void)maruko_mi_venc_destroy_chn(venc_dev, *chn);
		if (dev_created && *dev_created) {
			(void)maruko_mi_venc_destroy_dev(venc_dev);
			*dev_created = 0;
		}
		return ret;
	}

	/* IntraRefresh — opt-in via video0.intra_refresh.  Ch0 only; the
	 * dual ch1 path is intentionally skipped since TS containers need
	 * IDRs.  Failure is logged and snapshotted but never aborts the
	 * pipeline (matches Star6E behavior). */
	(void)maruko_apply_intra_refresh(venc_dev, *chn, cfg, height,
		framerate, cfg->rc_codec);
	/* refPred is applied earlier (pre-StartRecvPic); see comment above. */

	/* Phase 7 dual-VENC SDK probe (debug-only, env-gated).
	 *
	 * Set MARUKO_DUAL_VENC_PROBE=1 to attempt CreateChn(dev, 1, ...)
	 * after channel 0 is fully started.  Reports a 3-stage signal:
	 *   (a) CreateChn ret  — does SDK accept a 2nd channel on dev 0?
	 *   (b) SetInputSourceConfig ret — RING_DMA path accepts ch1?
	 *   (c) StartRecvPic ret — channel actually starts?
	 *
	 * Channel 1 is torn down immediately; this does NOT bind VPE -> ch1.
	 * Use the result to decide whether to commit to a full Phase 7 port
	 * (Gemini-style mirror or per-channel resolution) on Maruko.
	 */
	if (getenv("MARUKO_DUAL_VENC_PROBE")) {
		MI_VENC_CHN probe_ch = 1;
		i6c_venc_chn probe_attr = attr;  /* mirror chn 0 attrs */
		MI_S32 probe_ret;

		printf("> [maruko][probe] dual-VENC SDK probe BEGIN "
			"(dev=%d, probe_chn=%d)\n",
			(int)venc_dev, (int)probe_ch);

		probe_ret = maruko_mi_venc_create_chn(venc_dev, probe_ch,
			&probe_attr);
		printf("> [maruko][probe] CreateChn(dev=%d, chn=%d) ret=%d\n",
			(int)venc_dev, (int)probe_ch, (int)probe_ret);

		if (probe_ret == 0) {
			i6c_venc_src_conf src = I6C_VENC_SRC_CONF_RING_DMA;
			MI_S32 src_ret = maruko_mi_venc_set_input_source(
				venc_dev, probe_ch, &src);
			printf("> [maruko][probe] SetInputSourceConfig(chn=%d,"
				" RING_DMA) ret=%d\n",
				(int)probe_ch, (int)src_ret);

			MI_S32 start_ret = maruko_mi_venc_start_recv(venc_dev,
				probe_ch);
			printf("> [maruko][probe] StartRecvPic(chn=%d) ret=%d\n",
				(int)probe_ch, (int)start_ret);
			if (start_ret == 0)
				(void)maruko_mi_venc_stop_recv(venc_dev, probe_ch);

			(void)maruko_mi_venc_destroy_chn(venc_dev, probe_ch);
			printf("> [maruko][probe] DestroyChn(chn=%d) — torn down\n",
				(int)probe_ch);
		}

		printf("> [maruko][probe] dual-VENC SDK probe END\n");
	}

	/* Phase 5a stab-fill go/no-go: does the i6c VENC accept manual
	 * ChnInputPort pushes? (env-gated; see maruko_stabfill_probe.h). */
	if (getenv("MARUKO_STABFILL_PROBE"))
		maruko_stabfill_probe_run((int)venc_dev, &attr, width, height);

	return 0;
}

static void maruko_sysfs_write(const char *path, const char *value)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "%s\n", value);
	fclose(f);
}

static void maruko_set_hw_clocks(int oc_level, int verbose)
{
	/* ISP clock: 384 MHz */
	maruko_sysfs_write("/sys/devices/virtual/mstar/isp0/isp_clk",
		"384000000");

	/* SCL clock: index 1 = 533 MHz (max available).
	 * Must be set before SCL device creation (locks clock). */
	maruko_sysfs_write("/sys/devices/virtual/mstar/mscl/clk", "1");

	printf("> [maruko] ISP clock -> 384 MHz, SCL clock -> 533 MHz\n");

	if (oc_level >= 1) {
		/* VENC secondary + AXI clocks: try to boost from 320→288
		 * (writes may be ignored while streaming). */
		maruko_sysfs_write(
			"/sys/devices/virtual/mstar/venc/ven_clock_2nd",
			"288000000");
		maruko_sysfs_write(
			"/sys/devices/virtual/mstar/venc/ven_clock_axi",
			"288000000");
		printf("> [maruko] VENC 2nd/AXI -> 288 MHz (oc-level %d)\n",
			oc_level);
	}

	if (oc_level >= 2) {
		maruko_sysfs_write(
			"/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
			"performance");
		maruko_sysfs_write(
			"/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq",
			"1200000");
		if (verbose)
			printf("> [maruko] CPU -> performance @ 1200 MHz "
				"(oc-level %d)\n", oc_level);
	}
}

int maruko_pipeline_init(MarukoBackendContext *ctx)
{
	if (maruko_mi_init() != 0) {
		fprintf(stderr, "ERROR: [maruko] MI library load failed\n");
		return -1;
	}

	MI_S32 ret = MI_SYS_Init();
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_SYS_Init failed %d\n", ret);
		maruko_mi_deinit();
		return ret;
	}
	ctx->system_initialized = 1;

	/* Register compiled-in framing modules (idempotent). */
	maruko_framing_register_builtins();

	/* Set HW clocks AFTER MI_SYS_Init (kernel modules now loaded)
	 * but BEFORE ISP/SCL device creation (which locks clocks). */
	maruko_set_hw_clocks(ctx->cfg.oc_level, 1);

	printf("> [maruko] stage init: MI_SYS_Init ok\n");
	return 0;
}

static int setup_maruko_graph_dimensions(MarukoBackendContext *ctx)
{
	SensorSelectConfig sel_cfg = pipeline_common_build_sensor_select_config(
		(int)ctx->cfg.forced_sensor_pad, ctx->cfg.forced_sensor_mode,
		ctx->cfg.sensor_width, ctx->cfg.sensor_height,
		ctx->cfg.sensor_fps);
	sel_cfg.image_mirror = ctx->cfg.image_mirror;
	sel_cfg.image_flip   = ctx->cfg.image_flip;
	SensorStrategy strategy;
	if (ctx->cfg.sensor_unlock.enabled) {
		strategy = sensor_unlock_strategy(&ctx->cfg.sensor_unlock);
		printf("> [maruko] sensor unlock enabled (reg=0x%04x val=0x%04x)\n",
			ctx->cfg.sensor_unlock.reg, ctx->cfg.sensor_unlock.value);
	} else {
		strategy = sensor_default_strategy();
	}
	if (sensor_select(&sel_cfg, &strategy, &ctx->sensor) != 0)
		return -1;
	ctx->sensor_enabled = 1;

	pipeline_common_report_selected_fps("[maruko] ", ctx->cfg.sensor_fps,
		&ctx->sensor);

	/* Overscan detection: capture rect > crop rect can cause pipeline
	 * hangs (e.g. binning modes report pre-binning capture).
	 * Clamp to crop and zero offsets — the VIF inputRect must
	 * match the actual MIPI output, not the pre-binning window. */
	if (ctx->sensor.mode.crop.width > 0 &&
	    ctx->sensor.plane.capt.width > ctx->sensor.mode.crop.width) {
		fprintf(stderr, "WARNING: [maruko] sensor overscan detected: "
			"capture (%u,%u %ux%u) > crop %ux%u — clamping\n",
			ctx->sensor.plane.capt.x, ctx->sensor.plane.capt.y,
			ctx->sensor.plane.capt.width, ctx->sensor.plane.capt.height,
			ctx->sensor.mode.crop.width, ctx->sensor.mode.crop.height);
		ctx->sensor.plane.capt.x = 0;
		ctx->sensor.plane.capt.y = 0;
		ctx->sensor.plane.capt.width = ctx->sensor.mode.crop.width;
		ctx->sensor.plane.capt.height = ctx->sensor.mode.crop.height;
	}

	/* Effective output dimensions: when mode.output < crop, the sensor
	 * does internal binning and the actual frame is smaller than crop.
	 * Use output dimensions for image clamping and ring pool sizing. */
	uint32_t eff_w = ctx->sensor.plane.capt.width;
	uint32_t eff_h = ctx->sensor.plane.capt.height;
	if (ctx->sensor.mode.output.width > 0 &&
	    ctx->sensor.mode.output.width < eff_w) {
		printf("> [maruko] sensor output binning: %ux%u -> %ux%u\n",
			eff_w, eff_h,
			ctx->sensor.mode.output.width,
			ctx->sensor.mode.output.height);
		eff_w = ctx->sensor.mode.output.width;
		eff_h = ctx->sensor.mode.output.height;
	}
	uint32_t out_w = ctx->cfg.image_width;
	uint32_t out_h = ctx->cfg.image_height;
	if (out_w == 0 || out_h == 0) {
		out_w = eff_w;
		out_h = eff_h;
	}
	pipeline_common_clamp_image_size("[maruko] ", eff_w, eff_h,
		&out_w, &out_h);
	ctx->cfg.image_width = out_w;
	ctx->cfg.image_height = out_h;
	ctx->cfg.venc_gop_size = pipeline_common_gop_frames(
		ctx->cfg.venc_gop_seconds, ctx->sensor.fps);

	/* IntraRefresh auto-GOP override: when intraRefreshMode != off and the
	 * user did not pin gopSize, align IDR period with one full GDR pass. */
	{
		IntraRefreshDerived ir;
		IntraRefreshMode mode = maruko_intra_refresh_derive(
			&ctx->cfg, ctx->cfg.image_height, ctx->sensor.fps,
			ctx->cfg.rc_codec, &ir);
		if (mode != INTRA_MODE_OFF && !ir.gop_overridden && ir.gop_frames > 0)
			ctx->cfg.venc_gop_size = ir.gop_frames;
	}

	/* ISP throughput note: the Maruko ISP stalls when the sensor
	 * pixel throughput exceeds ~144M pix/s.  Mode 3 (1472x816@120fps
	 * = 144M) works; modes 0-2 (>=1920x1080) stall regardless of
	 * FPS because the MIPI data rate is set by the sensor mode, not
	 * the target FPS.  VIF sub-window crop is NOT supported on I6C.
	 * To enable lower-FPS modes at 1080p, the sensor driver needs
	 * custom mode entries with appropriate binning/MIPI settings. */

	/* Configure SCL ring pool using sensor capture dimensions.
	 * Note: majestic skips this, but the SDK sample_venc.c uses it.
	 * Use capture (ISP output) size, not effective/binned size. */
	uint32_t capt_w = ctx->sensor.plane.capt.width;
	uint32_t capt_h = ctx->sensor.plane.capt.height;
	MI_U16 scl_ring = (MI_U16)(capt_h / 4);
	if (scl_ring == 0)
		scl_ring = 1;
	MI_S32 pool_ret = maruko_config_dev_ring_pool(I6C_SYS_MOD_SCL, 0,
		(MI_U16)capt_w, (MI_U16)capt_h, scl_ring);
	printf("> [maruko] SCL ring pool: %ux%u ring=%u ret=%d\n",
		capt_w, capt_h, scl_ring, pool_ret);
	printf("> [maruko] sensor capt: %ux%u  eff: %ux%u  out: %ux%u\n",
		ctx->sensor.plane.capt.width, ctx->sensor.plane.capt.height,
		eff_w, eff_h, out_w, out_h);

	return 0;
}

static void assign_maruko_ports(MarukoBackendContext *ctx,
	MI_U32 venc_device)
{
	ctx->vif_port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_VIF, .device = 0,
		.channel = 0, .port = 0,
	};
	ctx->isp_port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_ISP, .device = 0,
		.channel = 0, .port = 0,
	};
	ctx->vpe_port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_SCL, .device = 0,
		.channel = 0, .port = 0,
	};
	ctx->scl_port1 = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_SCL, .device = 0,
		.channel = 0, .port = 1,
	};
	ctx->venc_port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_VENC, .device = venc_device,
		.channel = ctx->venc_channel, .port = 0,
	};
}

static void maruko_update_output_resilience_flags(MarukoBackendContext *ctx)
{
	MarukoIntraRefreshStatus ir_status;
	MarukoRefPredStatus ref_status;
	uint32_t clen;

	if (!ctx)
		return;
	maruko_pipeline_intra_refresh_status(&ir_status);
	maruko_pipeline_ref_pred_status(&ref_status);
	ctx->output.gdr_active = ir_status.active && ir_status.apply_ok;
	ctx->output.svct_active = ref_status.active && ref_status.apply_ok;
	clen = (ctx->output.gdr_active && ir_status.total_rows &&
		ir_status.effective_lines_per_p)
		? (ir_status.total_rows + ir_status.effective_lines_per_p - 1) /
			ir_status.effective_lines_per_p
		: 0;
	ctx->output.gdr_cycle_len = clen > 255 ? 255 : (uint8_t)clen;
	ctx->output.gdr_counter = 0;
}

/* stab-fill: finish the manual-feed VENC bring-up.  The fill module pushes the
 * composed frames straight into the (NORMAL_FRMBASE, unbound) VENC input port
 * — device-proven at 50 fps.  Phase 5a's "the i6c VENC cannot be manually
 * pushed" device result was an ABI artifact (its BufConf used eBufType=0,
 * which is BUFDATA_RAW on i6c, plus a Star6E-shaped union offset); with the
 * corrected MI_SYS_BufConf_t the direct push encodes fine, no SCL bridge or
 * FRAME_BASE bind needed.  StartRecvPic is deferred here from
 * maruko_start_venc (after the graph is assembled, before the first push). */
static int maruko_setup_stabfill_venc(MarukoBackendContext *ctx)
{
	MI_S32 ret;

	ret = maruko_mi_venc_start_recv(ctx->venc_device, ctx->venc_channel);
	if (ret != 0) {
		fprintf(stderr, "ERROR: [maruko] stab-fill StartRecvPic "
			"failed %d\n", (int)ret);
		return -1;
	}
	(void)maruko_apply_intra_refresh(ctx->venc_device, ctx->venc_channel,
		&ctx->cfg, ctx->cfg.image_height, ctx->sensor.fps,
		ctx->cfg.rc_codec);
	maruko_update_output_resilience_flags(ctx);

	printf("> [maruko] stab-fill: VENC(%d,%d) NORMAL_FRMBASE manual-feed "
		"@ %ux%u (port0 RAW unbound -> compose -> push)\n",
		(int)ctx->venc_device, (int)ctx->venc_channel,
		ctx->cfg.image_width, ctx->cfg.image_height);
	return 0;
}

static int bind_maruko_pipeline(MarukoBackendContext *ctx)
{
	uint32_t out_w = ctx->cfg.image_width;
	uint32_t out_h = ctx->cfg.image_height;

	ctx->venc_device = 0;
	if (maruko_start_venc(&ctx->cfg, out_w, out_h, ctx->sensor.fps,
	    ctx->venc_device, &ctx->venc_channel,
	    &ctx->venc_dev_created) != 0)
		return -1;
	ctx->venc_started = 1;

	maruko_update_output_resilience_flags(ctx);

	MI_U32 venc_device = (MI_U32)ctx->venc_device;
	assign_maruko_ports(ctx, venc_device);

	/* VIF->ISP bind is mode-conditional.  1485Mbps/lane x4 line bursts
	 * (~594MPix/s) exceed the ISP 384MHz drain and overflow the P0 input
	 * FIFO in REALTIME, so _1485 sensor modes need FRAME_BASE: VIF output
	 * is buffered in DRAM and the ISP drains at its own rate (m2m; costs
	 * up to one frame of latency and caps throughput at ~1.7ms+3.65ns/px
	 * per frame).  891Mbps modes keep REALTIME for minimum glass-to-glass
	 * latency — their burst rate (~356MPix/s) matches the ISP drain.
	 * VIF->ISP on I6C accepts only these two link types. */
	int vif_isp_link = strstr(ctx->sensor.mode.desc, "_1485") ?
		I6_SYS_LINK_FRAMEBASE : I6_SYS_LINK_REALTIME;
	printf("> [maruko] VIF->ISP bind: %s (mode %s)\n",
		vif_isp_link == I6_SYS_LINK_FRAMEBASE ? "FRAMEBASE" : "REALTIME",
		ctx->sensor.mode.desc);
	MI_S32 ret = MI_SYS_BindChnPort2(&ctx->vif_port, &ctx->isp_port,
		ctx->sensor.fps, ctx->sensor.fps, vif_isp_link, 0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] bind VIF->ISP failed %d\n", ret);
		return -1;
	}
	ctx->bound_vif_vpe = 1;

	ret = MI_SYS_BindChnPort2(&ctx->isp_port, &ctx->vpe_port,
		ctx->sensor.fps, ctx->sensor.fps, I6_SYS_LINK_REALTIME, 0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] bind ISP->SCL failed %d\n", ret);
		return -1;
	}
	ctx->bound_isp_vpe = 1;

	if (!g_stab_fill_graph) {
		ret = MI_SYS_BindChnPort2(&ctx->vpe_port, &ctx->venc_port,
			ctx->sensor.fps, ctx->sensor.fps, I6_SYS_LINK_RING, 0);
		if (ret != 0) {
			fprintf(stderr,
				"ERROR: [maruko] bind SCL->VENC failed %d\n", ret);
			return -1;
		}
		ctx->bound_vpe_venc = 1;
	} else {
		/* stab-fill: port0 stays UNBOUND (the fill module drains it) and the
		 * module manually feeds the frame-base VENC input per frame. */
		if (maruko_setup_stabfill_venc(ctx) != 0)
			return -1;
	}

	/* Set output port buffer depths to allow pipelining between
	 * stages.  Without this, the pipeline has zero frame buffering
	 * and any processing jitter causes frame drops, capping FPS
	 * well below the sensor's output rate.
	 * Star6E uses (1, 3) on the VENC port; SDK samples use (2, 4). */
	/* VIF output depth 4 (= SDK default, set explicitly): depth 8 was
	 * tested on the FRAMEBASE 1485 modes and changed nothing — the fps
	 * ceiling is the ISP m2m dispatch cost, not pool exhaustion.  RAW12
	 * buffers are ~10MB each at 4K width, so don't raise this casually
	 * (depth 8 = ~83MB MMA). */
	(void)MI_SYS_SetChnOutputPortDepth(&ctx->vif_port, 0, 4);
	(void)MI_SYS_SetChnOutputPortDepth(&ctx->isp_port, 1, 3);
	/* stab-fill: port0 is a userspace-drained port; the fill module sets its
	 * own (4,8) user-frame queue in setup_ports — don't shrink it here. */
	if (!g_stab_fill_graph)
		(void)MI_SYS_SetChnOutputPortDepth(&ctx->vpe_port, 1, 3);
	(void)MI_SYS_SetChnOutputPortDepth(&ctx->venc_port, 1, 3);

	/* JPEG snapshot backend on Maruko: dedicated MJPG VENC dev 8 chn 0
	 * bound to SCL chn 0 port 1 in FRAMEBASE mode (see src/maruko_jpeg.c).
	 * If SCL port-1 setup failed earlier (warning logged), backend_init
	 * detects no source registered and returns -ENODEV → snapshot endpoint
	 * cleanly serves 503.  Config from venc.json snapshot.* section;
	 * width=0/height=0 inherits main stream dims; quality/channel/enabled
	 * round-trip through /api/v1/get|set. */
	if (g_mi_scl_port1_enabled)
		venc_jpeg_set_source(&ctx->scl_port1);
	{
		const VencConfigSnapshot *snap = &ctx->cfg.snapshot;
		VencJpegConfig jcfg = {
			.width   = snap->width  ? snap->width  : out_w,
			.height  = snap->height ? snap->height : out_h,
			.quality = snap->quality,
			.channel = snap->channel,
			.enabled = snap->enabled,
		};
		(void)venc_jpeg_init(&jcfg);
	}

	/* ISP bin: resolve every configure (so SIGHUP / `/api/v1/restart`
	 * changes to `isp.sensorBin` and the auto-detect fallback are picked
	 * up on reinit), but skip the actual reload when the resolved path
	 * matches the last-loaded path.  Avoids redundant vendor reloads
	 * which can disturb running AE.  Star6E parity. */
	{
		char isp_bin_resolved[256];
		const char *configured = ctx->cfg.isp_bin_path[0] ?
			ctx->cfg.isp_bin_path : NULL;

		if (pipeline_common_resolve_isp_bin(configured,
			ctx->sensor.plane.sensName,
			isp_bin_resolved, sizeof(isp_bin_resolved)) &&
		    strcmp(isp_bin_resolved, g_last_isp_bin_path) != 0) {
			/* Cold boot vs. SIGHUP reinit dispatch.  The full
			 * loader runs three vendor hooks: disable_userspace3a
			 * → load_bin → post_load CUS3A_Enable(0,0,{1,0,0})
			 * + CUS3A_Enable(0,0,{1,1,0}).  CUS3A_Enable can
			 * only run once per process lifetime — a second call
			 * trips "WARNING: Mutex is not initialized before
			 * lock" inside libmi_isp and segfaults during the
			 * next IQ access.  The `if (!g_mi_isp_initialized)`
			 * block below protects the bare maruko_enable_cus3a()
			 * call, but does NOT cover the post_load hook reached
			 * via maruko_load_isp_bin.  On reinit the IQ/CUS3A
			 * framework from the previous lifecycle is still
			 * resident — we only need to re-load the bin bytes
			 * (same path the live `isp.sensorBin` reload takes
			 * via maruko_pipeline_load_isp_bin_live). */
			if (g_mi_isp_initialized)
				ret = maruko_load_isp_bin_minimal(isp_bin_resolved);
			else
				ret = maruko_load_isp_bin(isp_bin_resolved);
			if (ret != 0)
				return -1;
			snprintf(g_last_isp_bin_path,
				sizeof(g_last_isp_bin_path), "%s",
				isp_bin_resolved);
		}
	}

	/* CUS3A enable + cold-boot exposure cap: must only run once per
	 * process lifetime.  Re-running CUS3A enable causes a vendor mutex
	 * deadlock; the exposure cap and `MI_SNR_SetFps` kick are documented
	 * cold-boot fixups, not reinit operations. */
	if (!g_mi_isp_initialized) {
		/* Initialize CUS3A framework + enable userspace 3A (keeps
		 * IQ→HW pump alive).  The optional no-op AE adaptor for
		 * throttle mode is installed below, after the cold-boot
		 * exposure cap and SetFps kick. */
		maruko_enable_cus3a();

		typedef struct {
			unsigned int minShutterUs, maxShutterUs;
			unsigned int minApertX10, maxApertX10;
			unsigned int minSensorGain, minIspGain;
			unsigned int maxSensorGain, maxIspGain;
		} MarukoIspExposureLimit;
		/* Auto-cap exposure to frame period for max FPS.
		 * 120fps sensor → 8333us cap → 118fps. */
		if (ctx->sensor.fps > 0) {
			uint32_t divisor = ctx->cfg.shutter_rule_180
				? ctx->sensor.fps * 2 : ctx->sensor.fps;
			uint32_t frame_period_us = 1000000 / divisor;
			typedef int (*ae_get_fn)(uint32_t, uint32_t,
				MarukoIspExposureLimit *);
			typedef int (*ae_set_fn)(uint32_t, uint32_t,
				MarukoIspExposureLimit *);
			ae_get_fn fn_get = (ae_get_fn)dlsym(RTLD_DEFAULT,
				"MI_ISP_AE_GetExposureLimit");
			ae_set_fn fn_set = (ae_set_fn)dlsym(RTLD_DEFAULT,
				"MI_ISP_AE_SetExposureLimit");
			if (fn_get && fn_set) {
				MarukoIspExposureLimit lim = {0};
				if (fn_get(0, 0, &lim) == 0) {
					printf("> [maruko] Exposure %s: "
						"%uus -> %uus (for %u fps%s)\n",
						ctx->cfg.shutter_rule_180
							? "pin" : "cap",
						lim.maxShutterUs,
						frame_period_us,
						ctx->sensor.fps,
						ctx->cfg.shutter_rule_180
							? ", 180\xc2\xb0 rule"
							: "");
					lim.maxShutterUs = frame_period_us;
					if (ctx->cfg.shutter_rule_180)
						lim.minShutterUs = frame_period_us;
					fn_set(0, 0, &lim);
				}
			}

			/* Force sensor timing reconfiguration after AE
			 * init.  The vendor AE may have extended VTS
			 * based on ISP bin defaults; MI_SNR_SetFps
			 * forces the sensor driver to reset VTS to the
			 * mode's native value. */
			MI_SNR_SetFps(ctx->sensor.pad_id, ctx->sensor.fps);
			printf("> [maruko] MI_SNR_SetFps kick: "
				"pad %d fps %u\n",
				(int)ctx->sensor.pad_id,
				ctx->sensor.fps);
			/* SetFps can reset sensor timing registers and clobber
			 * orientation; re-apply after each kick. */
			if (ctx->cfg.image_mirror || ctx->cfg.image_flip)
				(void)MI_SNR_SetOrien(ctx->sensor.pad_id,
					(uint8_t)ctx->cfg.image_mirror,
					(uint8_t)ctx->cfg.image_flip);
		}

		/* AE runs in ONE mode on Maruko: PACED NATIVE 3A.  The vendor
		 * AE+AWB converge at full rate for the first ~3 s, then the
		 * framework auto-run is paused (run-mode OFF) and a pacer
		 * thread ticks the same algo at sensor_fps/3 (floor 30 Hz).
		 * Device-measured @1080p100: 70.0% (old native) / 52.8% (old
		 * throttle) → 39.4%, with full vendor image quality.  The
		 * historical isp.aeEngine=custom mode was retired in 0.22.0 and
		 * the value removed entirely in 0.47.0 — paced native is the only
		 * AE (HISTORY 0.22.0). */

		/* Experimental bench toggle: WAYBEAM_NO_3A=1 freezes 3A entirely —
		 * pause the vendor auto-run (SetRunMode OFF) and start NO pacer /
		 * supervisory thread, so AE/AWB hold at the ISP-bin defaults (static
		 * exposure). Sheds all per-frame 3A/apply cost, to test whether 3A —
		 * vs the raw ISP per-frame time budget — is what caps high fps.
		 * Bench use only (exposure no longer adapts). */
		const char *no_3a_env = getenv("WAYBEAM_NO_3A");
		int no_3a = (no_3a_env && no_3a_env[0] && no_3a_env[0] != '0');
		if (no_3a) {
			if (g_inj_setrunmode) {
				int rm = g_inj_setrunmode(0, 0, 1); /* E_CUS3A_MODE_OFF */
				printf("> [maruko] WAYBEAM_NO_3A: CUS3A_SetRunMode(OFF) "
					"ret=%d — 3A FROZEN (static IQ, no pacer/"
					"supervisory)\n", rm);
			} else {
				printf("> [maruko] WAYBEAM_NO_3A set but CUS3A_SetRunMode "
					"symbol missing — vendor 3A stays active\n");
			}
		}

		/* Supervisory thread: limits-only (enforces isp.gainMax and
		 * the fps-derived shutter ceiling on the vendor algo). */
		if (!no_3a && ctx->cfg.ae_fps > 0) {
			MarukoCus3aConfig ae_cfg;
			maruko_cus3a_config_defaults(&ae_cfg);
			ae_cfg.sensor_fps    = ctx->sensor.fps;
			ae_cfg.ae_fps        = ctx->cfg.ae_fps;
			ae_cfg.gain_max      = ctx->cfg.isp_gain_max;
			if (ctx->cfg.shutter_rule_180 &&
			    ctx->sensor.fps > 0) {
				ae_cfg.shutter_max_us =
					1000000 / (ctx->sensor.fps * 2);
				ae_cfg.shutter_pin = 1;
			} else if (ctx->cfg.isp_shutter_max_us > 0) {
				ae_cfg.shutter_max_us =
					ctx->cfg.isp_shutter_max_us;
			}
			ae_cfg.gain_min      = ctx->cfg.isp_gain_min;
			ae_cfg.shutter_min_us = ctx->cfg.isp_shutter_min_us;
			ae_cfg.verbose       = ctx->cfg.verbose;
			(void)maruko_cus3a_start(&ae_cfg);
		} else if (ctx->cfg.verbose) {
			printf("> [maruko] supervisory 3A disabled "
				"(isp.aeFps=0)\n");
		}

		/* Pacer rate auto-derives from the sensor rate: fps/3 keeps
		 * AE latency imperceptible at high fps while shedding 2/3 of
		 * the algo+apply cost; the 30 Hz floor keeps low-fps modes at
		 * effectively full-rate quality.  No user knob. */
		{
			unsigned sfps = ctx->sensor.fps ? ctx->sensor.fps : 100;
			g_inj_fps = sfps / 3;
			if (g_inj_fps < 30)
				g_inj_fps = 30;
		}
		/* All three symbols are required: RunOnceEn arms AE+AWB,
		 * RunOnce executes them per tick, SetRunMode pauses the vendor
		 * auto-run.  If any is missing, leave the vendor auto-run
		 * active (full-rate, correct image, higher CPU) rather than
		 * pausing it and ticking an unarmed RunOnce (frozen 3A). */
		if (!no_3a && g_inj_setrunmode && g_inj_runonce && g_inj_runonce_now) {
			g_inj_run = 1;
			if (pthread_create(&g_inj_flip_thread, NULL,
					maruko_ae_pacer_thread, NULL) != 0)
				g_inj_run = 0;
		}
		if (g_inj_run)
			printf("> [maruko] AE mode: paced native 3A "
				"(vendor AE+AWB, pacer @%u Hz after "
				"convergence)\n", g_inj_fps);
		else if (!no_3a)
			printf("> [maruko] AE mode: native full-rate "
				"(pacer unavailable — CUS3A symbols "
				"missing)\n");

		g_mi_isp_initialized = 1;
	}

	/* Flush any pending AE crop now that CUS3A is up (cold boot) or the
	 * framework is known-good from a prior lifecycle (reinit).  Star6E
	 * parity — see star6e_ae_crop_mark_ready. */
	maruko_ae_crop_mark_ready(ctx);

	if (ctx->cfg.output_uri.type == VENC_OUTPUT_URI_SHM) {
		if (ctx->cfg.stream_mode != MARUKO_STREAM_RTP) {
			fprintf(stderr, "ERROR: [maruko] shm:// requires RTP mode\n");
			return -1;
		}
		if (maruko_output_init_shm(&ctx->output,
		    ctx->cfg.output_uri.endpoint) != 0)
			return -1;
	} else if (ctx->cfg.output_uri.type == VENC_OUTPUT_URI_FRAME_SHM) {
		if (maruko_output_init_frame_shm(&ctx->output,
		    ctx->cfg.output_uri.endpoint) != 0)
			return -1;
	} else {
		if (maruko_output_init(&ctx->output, &ctx->cfg.output_uri,
		    ctx->cfg.connected_udp,
		    ctx->cfg.allow_unix_encoder_stall) != 0) {
		/* Non-fatal, deliberately, and only HERE -- a live retarget still
		 * refuses a bad URI, because there the craft has a working output to
		 * lose.  At boot it has none either way.
		 *
		 * Aborting used to take the whole daemon down: measured on all three
		 * backends, one bad `outgoing.server` in the config brought the
		 * pipeline up and then tore everything down, leaving no video AND no
		 * HTTP -- so correcting a one-line config error needed ssh, which a
		 * craft in the field does not have.  Coming up with the output inert
		 * keeps the API reachable, and `outgoing.server` is live-settable, so
		 * the operator can fix it remotely.
		 *
		 * The teardown is what makes that safe: it leaves socket_handle at
		 * -1 and the destination zeroed, and every send path already refuses
		 * a negative handle, so frames are counted as drops rather than
		 * touching an unopened socket. */
			maruko_output_teardown(&ctx->output);
		/* The teardown resets transport policy along with transport state,
		 * and a later live recovery reads those fields back out of the
		 * output -- so leaving them cleared would silently apply a DIFFERENT
		 * policy than the config asks for once the operator sets a working
		 * destination.  Star6E loses connected-UDP as well as the Unix
		 * stall option; Maruko loses the stall option.  Re-seed them from
		 * the same source init would have. */
			ctx->output.requested_connected_udp =
				ctx->cfg.connected_udp ? 1 : 0;
			ctx->output.allow_unix_encoder_stall =
				ctx->cfg.allow_unix_encoder_stall ? 1 : 0;
			fprintf(stderr, "ERROR: outgoing.server could not be brought up; "
				"starting with NO video output so the craft stays reachable "
				"-- set outgoing.server over the API to recover\n");
		}
	}

	/* After the init above, whose reset would clear it.  Same
	 * rate-limited primitive the scene detector uses, so both producers
	 * of forced IDRs coalesce through one 100 ms window. */
	ctx->output.request_idr = maruko_scene_request_idr;
	ctx->output.idr_ctx = ctx;

	/* Adaptive frame gate — set up after the output exists so the ring's
	 * slot_count can sanity-check the close threshold. */
	{
		venc_frame_ring_fill_t gfill = {0};

		(void)maruko_output_frame_ring_fill(&ctx->output, &gfill);
		maruko_report_frame_gate_setup(frame_gate_setup(&ctx->frame_gate,
			ctx->cfg.frame_gate_close_slots,
			ctx->cfg.frame_gate_max_closed_ms, gfill.slot_count),
			&ctx->frame_gate, gfill.slot_count);
	}

	return 0;
}

/* IMU push callback: stub.  Drained samples are discarded for now —
 * the callback exists so a future telemetry / sidecar consumer can
 * slot in without rewiring imu_init.  Mirrors star6e_pipeline.c's
 * star6e_pipeline_imu_push. */
static void maruko_pipeline_imu_push(void *ctx, const ImuSample *sample)
{
	(void)ctx;
	(void)sample;
}

/* ── Dual VENC (Phase 7) ─────────────────────────────────────────────────
 *
 * Mirrors src/star6e_pipeline.c's Star6eDualVenc lifecycle and
 * src/star6e_runtime.c's dual_rec_thread_fn frame-drain.  Currently
 * only the "dual-stream" mode is wired (chn 1 → UDP); the "dual" /
 * TS-record variant lives behind Phase 6 (no MI_AI / TS mux on
 * Maruko yet).
 *
 * The chn 1 stream is sourced from the same VPE port as chn 0 — both
 * channels see identical pre-encoder frames.  Bitrate / FPS / GOP can
 * differ per channel via the cfg.record.* knobs.
 */

struct MarukoDualVenc {
	MarukoBackendContext *ctx;          /* back-pointer for cfg lookup in thread */
	MI_VENC_CHN channel;
	MI_SYS_ChnPort_t port;              /* dst port for chn 1 (VENC/dev/1/0) */
	MI_SYS_ChnPort_t bind_src;          /* src port used at bind time
	                                     * (VENC chn 0 output) — captured so
	                                     * UnBind can pair with the same args
	                                     * even if the source port shape ever
	                                     * changes upstream. */
	int bound;
	int started;                        /* StartRecvPic succeeded */
	MarukoOutput output;
	MarukoRtpState rtp_state;
	H26xParamSets params;
	char mode[16];
	uint32_t bitrate;
	uint32_t fps;
	uint32_t gop;
	char server[128];
	int is_dual_stream;
	/* Non-NULL when mode == "dual": chn 1 frames are written here.
	 * Exactly one of ts_recorder / recorder is non-NULL based on
	 * record.format ("ts" or "hevc"), set when the dual struct is
	 * built.  Both owned by MarukoBackendContext, weak ptrs here. */
	Star6eTsRecorderState *ts_recorder;
	Star6eRecorderState *recorder;
	pthread_t thread;
	volatile sig_atomic_t running;
	int started_thread;
	/* Pre-allocated stream packs to avoid alloc-per-frame in the drain
	 * loop.  Grown on demand by dual_ensure_packs() if a frame exceeds
	 * the cached cap. */
	i6c_venc_pack *stream_packs;
	uint32_t stream_packs_cap;
};

static i6c_venc_pack *dual_ensure_packs(i6c_venc_pack **packs,
	uint32_t *cap, uint32_t need)
{
	if (need > *cap) {
		i6c_venc_pack *p = realloc(*packs, need * sizeof(*p));
		if (!p)
			return NULL;
		*packs = p;
		*cap = need;
	}
	return *packs;
}

/* Reduce chn 1 bitrate by 10% on sustained pressure.  Mirrors
 * star6e_runtime.c:dual_rec_reduce_bitrate(). */
static int dual_reduce_bitrate(MI_VENC_DEV venc_dev, MI_VENC_CHN chn,
	uint32_t *current_kbps, uint32_t min_kbps)
{
	i6c_venc_chn attr = {0};
	uint32_t new_kbps;
	MI_U32 bits;

	if (maruko_mi_venc_get_chn_attr(venc_dev, chn, &attr) != 0)
		return -1;

	new_kbps = *current_kbps * 9 / 10;
	if (new_kbps < min_kbps)
		new_kbps = min_kbps;
	if (new_kbps == *current_kbps)
		return 0;

	bits = new_kbps * 1024;
	switch (attr.rate.mode) {
	case MARUKO_VENC_RC_H265_CBR:
		attr.rate.h265Cbr.bitrate = bits;
		break;
	case MARUKO_VENC_RC_H264_CBR:
		attr.rate.h264Cbr.bitrate = bits;
		break;
	case MARUKO_VENC_RC_H265_VBR:
		attr.rate.h265Vbr.maxBitrate = bits;
		break;
	case MARUKO_VENC_RC_H264_VBR:
		attr.rate.h264Vbr.maxBitrate = bits;
		break;
	case MARUKO_VENC_RC_H265_AVBR:
		attr.rate.h265Avbr.maxBitrate = bits;
		break;
	case MARUKO_VENC_RC_H264_AVBR:
		attr.rate.h264Avbr.maxBitrate = bits;
		break;
	default:
		return -1;
	}

	if (maruko_mi_venc_set_chn_attr(venc_dev, chn, &attr) != 0)
		return -1;

	printf("[maruko][dual] backpressure: bitrate %u -> %u kbps\n",
		*current_kbps, new_kbps);
	*current_kbps = new_kbps;
	return 0;
}

/* Drain frames from chn 1 and forward them via UDP.  Mirrors the
 * Star6E dual_rec_thread_fn but with the SD-card writer replaced by
 * maruko_video_send_frame (UDP only — dual-stream variant). */
static void *maruko_dual_stream_thread(void *arg)
{
	struct MarukoDualVenc *d = arg;
	MarukoBackendContext *ctx = d->ctx;
	uint32_t current_kbps = d->bitrate;
	uint32_t min_kbps = d->bitrate / 4;
	struct timespec interval_start;
	unsigned int behind_count = 0;
	unsigned int total_count = 0;
	unsigned int pressure_seconds = 0;

	if (min_kbps < 1000)
		min_kbps = 1000;

	clock_gettime(CLOCK_MONOTONIC, &interval_start);

	int venc_fd = maruko_mi_venc_get_fd(ctx->venc_device, d->channel);
	if (ctx->cfg.verbose)
		printf("> [maruko][dual] drain thread up (fd=%d)\n", venc_fd);

	while (d->running) {
		i6c_venc_stat stat = {0};
		i6c_venc_strm stream = {0};
		int ret;

		if (venc_fd >= 0) {
			struct pollfd pfd = { .fd = venc_fd, .events = POLLIN };
			(void)poll(&pfd, 1, 1000);
			if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
				maruko_mi_venc_close_fd(ctx->venc_device,
					d->channel);
				venc_fd = -1;
				usleep(1000);
				continue;
			}
			if (!(pfd.revents & POLLIN))
				continue;
		}

		ret = maruko_mi_venc_query(ctx->venc_device, d->channel, &stat);
		if (ret != 0 || stat.curPacks == 0) {
			usleep(venc_fd >= 0 ? 100 : 1000);
			continue;
		}

		stream.count = stat.curPacks;
		stream.packet = dual_ensure_packs(&d->stream_packs,
			&d->stream_packs_cap, stat.curPacks);
		if (!stream.packet) {
			usleep(1000);
			continue;
		}

		ret = maruko_mi_venc_get_stream(ctx->venc_device, d->channel,
			&stream, g_maruko_running ? 40 : 0);
		if (ret != 0) {
			if (ret == -EAGAIN || ret == EAGAIN)
				usleep(1000);
			continue;
		}
		/* Through the shared helper, as Star6E's ch1 is: the open-coded
		 * version counted nothing, so a ch1 discard never reached
		 * bad_au_drops and its one-shot warn had already latched.  (No
		 * ring is involved here — maruko_output_init() rejects
		 * frame-shm:// for the dual output — so this is a lost counter,
		 * not a false-healthy gauge.) */
		if (maruko_video_reject_incomplete_access_unit(&stream,
		    &d->output)) {
			(void)maruko_mi_venc_release_stream(ctx->venc_device,
				d->channel, &stream);
			continue;
		}

		if (g_maruko_running) {
			if (d->is_dual_stream) {
				(void)maruko_video_send_frame(&stream,
					&d->output, &d->rtp_state, &d->params,
					&ctx->cfg, NULL);
			} else if (d->ts_recorder) {
				/* Skip slow SD writes during shutdown — keep
				 * draining to prevent VPE backpressure while
				 * pipeline tears down. */
				(void)maruko_ts_recorder_write_stream(
					d->ts_recorder, &stream);
			} else if (d->recorder) {
				(void)maruko_recorder_write_frame(
					d->recorder, &stream);
			}
		}

		(void)maruko_mi_venc_release_stream(ctx->venc_device,
			d->channel, &stream);

		total_count++;
		if (stat.curPacks >= 2)
			behind_count++;

		{
			struct timespec now;
			long long elapsed_ms;

			clock_gettime(CLOCK_MONOTONIC, &now);
			elapsed_ms = (long long)(now.tv_sec -
				interval_start.tv_sec) * 1000LL +
				(long long)(now.tv_nsec -
				interval_start.tv_nsec) / 1000000LL;

			if (elapsed_ms >= 1000) {
				if (total_count > 0 &&
				    behind_count > total_count * 4 / 5) {
					pressure_seconds++;
					if (pressure_seconds >= 3) {
						dual_reduce_bitrate(
							ctx->venc_device,
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
		maruko_mi_venc_close_fd(ctx->venc_device, d->channel);

	return NULL;
}

/* Compose the chn 1 attribute block.  Mirrors the chn 0 path in
 * maruko_start_venc (codec branch + fill helper) but lets the caller
 * override bitrate / fps / gop independently. */
static void dual_fill_attr(i6c_venc_chn *attr,
	const MarukoBackendConfig *base_cfg,
	uint32_t width, uint32_t height,
	uint32_t bitrate_kbps, uint32_t framerate, uint32_t gop)
{
	MarukoBackendConfig dual_cfg = *base_cfg;
	dual_cfg.venc_max_rate = bitrate_kbps;
	dual_cfg.venc_gop_size = gop;

	if (base_cfg->rc_codec == PT_H265) {
		attr->attrib.codec = I6C_VENC_CODEC_H265;
		maruko_fill_h26x_attr(&attr->attrib.h265, width, height);
	} else {
		attr->attrib.codec = I6C_VENC_CODEC_H264;
		maruko_fill_h26x_attr(&attr->attrib.h264, width, height);
	}

	uint32_t safe_gop = gop ? gop : 1;
	/* Same practical bitrate bounds as apply_bitrate() (ch1/dual boot). */
	uint32_t rate_kbps = bitrate_kbps;
	if (rate_kbps > VENC_BITRATE_MAX_KBPS)
		rate_kbps = VENC_BITRATE_MAX_KBPS;
	if (rate_kbps < VENC_BITRATE_MIN_KBPS)
		rate_kbps = VENC_BITRATE_MIN_KBPS;
	MI_U32 bit_rate_bits = rate_kbps * 1024;
	fill_maruko_rc_attr(attr, &dual_cfg, safe_gop, bit_rate_bits, framerate);
}

int maruko_pipeline_start_dual(MarukoBackendContext *ctx,
	uint32_t bitrate, uint32_t fps, double gop_sec,
	const char *mode, const char *server)
{
	struct MarukoDualVenc *d;
	MI_VENC_DEV dev = ctx->venc_device;
	MI_VENC_CHN chn = 1;
	uint32_t sensor_fps = ctx->sensor.fps;
	uint32_t gop_frames;
	pthread_attr_t thr_attr;
	int have_attr;
	int ret;

	if (!mode)
		return -1;

	if (sensor_fps == 0)
		sensor_fps = 30;
	if (fps == 0)
		fps = sensor_fps;
	if (fps > sensor_fps)
		fps = sensor_fps;
	if (bitrate == 0)
		bitrate = ctx->cfg.venc_max_rate;
	if (bitrate == 0)
		bitrate = 8000;
	gop_frames = (uint32_t)(gop_sec * fps + 0.5);
	if (gop_frames < 1)
		gop_frames = fps;

	d = calloc(1, sizeof(*d));
	if (!d)
		return -1;

	d->ctx = ctx;
	d->channel = chn;
	d->bitrate = bitrate;
	d->fps = fps;
	d->gop = gop_frames;
	d->is_dual_stream = (strcmp(mode, "dual-stream") == 0);
	if (d->is_dual_stream) {
		d->ts_recorder = NULL;
		d->recorder = NULL;
	} else if (strcmp(ctx->cfg.record.format, "hevc") == 0) {
		d->ts_recorder = NULL;
		d->recorder = &ctx->recorder;
	} else {
		d->ts_recorder = &ctx->ts_recorder;
		d->recorder = NULL;
	}
	snprintf(d->mode, sizeof(d->mode), "%s", mode);
	if (server)
		snprintf(d->server, sizeof(d->server), "%s", server);
	d->output.socket_handle = -1;

	/* SDK pattern: both VENC channels must exist before SCL -> chn 0
	 * fan-out is established.  bind_maruko_pipeline already bound
	 * SCL -> chn 0 (RING) — temporarily unbind so chn 1 can be
	 * created, then re-bind.  Skipping this leaves chn 0's encoder
	 * in a degraded ~5 Mbps mode (verified empirically: 25 Mbps ->
	 * 5 Mbps when chn 1 is added without rebind).  Safe here because
	 * configure_graph runs before the encoder loop, so no in-flight
	 * frames are lost during the unbind window. */
	int rebind_main = 0;
	if (ctx->bound_vpe_venc) {
		(void)MI_SYS_UnBindChnPort(&ctx->vpe_port, &ctx->venc_port);
		ctx->bound_vpe_venc = 0;
		rebind_main = 1;
	}

	/* CreateChn(dev, 1, &attr) — Phase 7 probe confirmed this works
	 * after chn 0 is fully started.  Ring pool is per-dev and was
	 * already provisioned for chn 0 in maruko_start_venc; no
	 * second pool reservation is required for chn 1. */
	i6c_venc_chn attr = {0};
	dual_fill_attr(&attr, &ctx->cfg, ctx->cfg.image_width,
		ctx->cfg.image_height, bitrate, fps, gop_frames);
	ret = maruko_mi_venc_create_chn(dev, chn, &attr);
	if (ret != 0) {
		fprintf(stderr,
			"WARNING: [maruko][dual] CreateChn(dev=%d, chn=%d)"
			" failed %d\n", (int)dev, (int)chn, ret);
		if (rebind_main) {
			(void)MI_SYS_BindChnPort2(&ctx->vpe_port, &ctx->venc_port,
				ctx->sensor.fps, ctx->sensor.fps,
				I6_SYS_LINK_RING, 0);
			ctx->bound_vpe_venc = 1;
		}
		free(d);
		return -1;
	}

	/* Maruko dual-VENC topology (per Maruko SDK sample_venc.c):
	 * chn 1 is sourced from chn 0's VENC output port (NOT from SCL).
	 * The VENC hardware exposes a chn 0 -> chn 1 HW_RING fan-out so
	 * the second encoder sees the same input frames.  Confirmed
	 * empirically: binding SCL/0/0/0 -> VENC/0/1/0 directly returns
	 * 0xA0092012 (SYS busy) because chn 0 already holds the SCL
	 * output port in RING mode, and the SCL port cannot multi-consume.
	 *
	 * The SDK sample does NOT call SetInputSourceConfig on chn 1 —
	 * sub-channels default to NORMAL_FRMBASE (handshake by ~3-buffer
	 * frame mode) which is what chn 0 -> chn 1 HW_RING expects on
	 * the destination side.  Setting RING_UNIFIED_DMA on chn 1
	 * starves chn 1 (the encoder waits for ring DMA frames that
	 * never arrive). */

	ret = maruko_mi_venc_start_recv(dev, chn);
	if (ret != 0) {
		fprintf(stderr,
			"WARNING: [maruko][dual] StartRecvPic(chn=%d)"
			" failed %d\n", (int)chn, ret);
		(void)maruko_mi_venc_destroy_chn(dev, chn);
		if (rebind_main) {
			(void)MI_SYS_BindChnPort2(&ctx->vpe_port, &ctx->venc_port,
				ctx->sensor.fps, ctx->sensor.fps,
				I6_SYS_LINK_RING, 0);
			ctx->bound_vpe_venc = 1;
		}
		free(d);
		return -1;
	}
	d->started = 1;

	/* Re-establish SCL -> VENC chn 0.  Both channels now exist, so
	 * the encoder enters dual-channel mode correctly. */
	if (rebind_main) {
		ret = MI_SYS_BindChnPort2(&ctx->vpe_port, &ctx->venc_port,
			ctx->sensor.fps, ctx->sensor.fps,
			I6_SYS_LINK_RING, 0);
		if (ret != 0) {
			fprintf(stderr,
				"WARNING: [maruko][dual] re-bind SCL->chn0"
				" failed %d\n", ret);
			(void)maruko_mi_venc_stop_recv(dev, chn);
			(void)maruko_mi_venc_destroy_chn(dev, chn);
			free(d);
			return -1;
		}
		ctx->bound_vpe_venc = 1;
	}

	/* Source: chn 0's VENC output port.  Dst: chn 1 input.
	 * Maruko SDK sample_venc.c:
	 *   src = VENC/dev/MainChn/0  (chn 0 output)
	 *   dst = VENC/dev/SubChn/0   (chn 1)
	 *   bind type = HW_RING
	 */
	MI_SYS_ChnPort_t src_port = {
		.module = I6_SYS_MOD_VENC, .device = (MI_U32)dev,
		.channel = ctx->venc_channel, .port = 0,
	};
	d->port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_VENC, .device = (MI_U32)dev,
		.channel = chn, .port = 0,
	};

	ret = MI_SYS_BindChnPort2(&src_port, &d->port,
		sensor_fps, fps, I6_SYS_LINK_RING, 0);
	if (ret != 0) {
		fprintf(stderr,
			"WARNING: [maruko][dual] bind VENC chn0->chn%d"
			" failed %d\n", (int)chn, ret);
		(void)maruko_mi_venc_stop_recv(dev, chn);
		(void)maruko_mi_venc_destroy_chn(dev, chn);
		free(d);
		return -1;
	}
	d->bound = 1;
	d->bind_src = src_port;  /* remember for unbind */

	(void)MI_SYS_SetChnOutputPortDepth(&d->port, 1, 3);

	if (d->is_dual_stream && d->server[0]) {
		VencOutputUri uri;
		if (venc_config_parse_output_uri(d->server, &uri) == 0) {
			if (maruko_output_init(&d->output, &uri,
				ctx->cfg.connected_udp,
				ctx->cfg.allow_unix_encoder_stall) == 0) {
				maruko_video_init_rtp_state(&d->rtp_state,
					ctx->cfg.rc_codec, sensor_fps);
				printf("> [maruko][dual] dual-stream chn=%d ->"
					" %s (%u kbps, %u fps, gop=%u)\n",
					(int)chn, d->server, bitrate, fps,
					gop_frames);
			} else {
				fprintf(stderr,
					"WARNING: [maruko][dual] output_init"
					" failed for %s\n", d->server);
			}
		} else {
			fprintf(stderr,
				"WARNING: [maruko][dual] cannot parse"
				" record.server '%s'\n", d->server);
		}
	}

	d->running = 1;
	/* maruko_dual_stream_thread reaches star6e_ts_recorder_write_stream
	 * under record.mode=dual, whose ~1.06 MB of automatic buffers overflows
	 * musl's 128 KB pthread default — and Maruko IS a musl target. */
	have_attr = pthread_attr_init(&thr_attr) == 0;
	if (have_attr) {
		size_t cur = 0;
		/* RAISE only.  musl's 128 KB default must come up; glibc's 8 MB
		 * must not come down -- this thread also runs SDK entry points
		 * whose stack use is opaque, and lowering a platform that was
		 * already safe buys nothing. */
		if (pthread_attr_getstacksize(&thr_attr, &cur) == 0 &&
		    cur < STAR6E_TS_RECORDER_STREAM_STACK_BYTES)
			(void)pthread_attr_setstacksize(&thr_attr,
				STAR6E_TS_RECORDER_STREAM_STACK_BYTES);
	}
	if (pthread_create(&d->thread, have_attr ? &thr_attr : NULL,
			maruko_dual_stream_thread, d) != 0) {
		if (have_attr)
			pthread_attr_destroy(&thr_attr);
		/* Spawn failure: tear down everything we just allocated,
		 * leaving ctx->dual NULL so the caller continues with
		 * chn 0 only. */
		fprintf(stderr,
			"ERROR: [maruko][dual] pthread_create failed\n");
		d->running = 0;
		if (d->bound) {
			(void)MI_SYS_UnBindChnPort(&d->bind_src, &d->port);
			d->bound = 0;
		}
		if (d->started)
			(void)maruko_mi_venc_stop_recv(dev, chn);
		(void)maruko_mi_venc_destroy_chn(dev, chn);
		maruko_output_teardown(&d->output);
		free(d->stream_packs);
		free(d);
		return -1;
	}
	d->started_thread = 1;

	if (have_attr)
		pthread_attr_destroy(&thr_attr);
	ctx->dual = d;
	venc_api_dual_register(d->channel, d->bitrate, d->fps, d->gop);
	return 0;
}

void maruko_pipeline_stop_dual(MarukoBackendContext *ctx)
{
	if (!ctx || !ctx->dual)
		return;

	venc_api_dual_unregister();

	struct MarukoDualVenc *d = ctx->dual;
	MI_VENC_DEV dev = ctx->venc_device;

	if (d->started_thread) {
		d->running = 0;
		pthread_join(d->thread, NULL);
		d->started_thread = 0;
	}

	if (d->started) {
		(void)maruko_mi_venc_stop_recv(dev, d->channel);
		d->started = 0;
	}
	if (d->bound) {
		(void)MI_SYS_UnBindChnPort(&d->bind_src, &d->port);
		d->bound = 0;
	}
	(void)maruko_mi_venc_destroy_chn(dev, d->channel);

	maruko_output_teardown(&d->output);
	free(d->stream_packs);
	free(d);
	ctx->dual = NULL;
}

int maruko_pipeline_configure_graph(MarukoBackendContext *ctx)
{
	/* Latch the stab-fill graph mode for this bring-up (also re-evaluated on
	 * every MUT_RESTART reinit, so leaving stab-fill restores the RING leg).
	 * Only meaningful when the fill module is compiled in — without it the
	 * frame-base VENC would have no producer and the stream would starve. */
#if HAVE_FRAMING_STAB
	g_stab_fill_graph = strcmp(ctx->cfg.framing, "stab-fill") == 0;
#else
	g_stab_fill_graph = 0;
#endif
	if (g_stab_fill_graph)
		printf("> [maruko] framing=stab-fill: manual-feed graph "
			"(port0 RAW unbound -> compose -> VENC NORMAL_FRMBASE push)\n");

	if (setup_maruko_graph_dimensions(ctx) != 0)
		return -1;

	uint32_t out_w = ctx->cfg.image_width;
	uint32_t out_h = ctx->cfg.image_height;

	/* SCL input dims = full sensor capture.  The ISP output port is a
	 * zero-crop passthrough (see configure_maruko_isp), so the surface that
	 * actually feeds the SCL is always the full capt — which is also the SCL
	 * ring-pool stride.  Do NOT shrink this to mode.output: mode.output is the
	 * *encode* target (applied as the SCL output size below, not its input).
	 * The old "sensor binning" override set scl_in = mode.output; for genuine
	 * downscale modes (capt 2952x1656 -> encode 2560x1440, senif != senout) it
	 * made the SCL crop width != ring-pool stride and stalled the scaler
	 * (0 frames out, ISP P0 FIFO FULL, encoder starved).  For binned modes the
	 * sensor already emits capt == output, so this is a no-op for modes 0-4. */
	uint32_t scl_in_w = ctx->sensor.plane.capt.width;
	uint32_t scl_in_h = ctx->sensor.plane.capt.height;
	/* Honour keep_aspect: true centre-crops the source to the encode AR,
	 * false passes the full sensor frame and lets the SCL scale both axes
	 * non-uniformly (stretch-to-fill).
	 *
	 * A blanket override forcing the crop was added 2026-07-04 on the
	 * strength of one device failure: mode 4 1920x1080@100 -> 1440x1080
	 * hung with 0 frames.  That case is a *single-axis* squeeze — width
	 * 1920->1440 with height unchanged at 1080 — which the SCL does refuse.
	 * It does not generalise to a genuine two-axis non-uniform scale such
	 * as 2496x1872 -> 1920x1080 (/1.30 wide, /1.73 tall), which the SCL
	 * handles.  Overriding every AR mismatch broke stretch-to-fill for all
	 * 4:3-sensor-to-16:9-output setups, so keep_aspect is passed through
	 * again.
	 *
	 * Still true: a request that leaves one axis unscaled while the other
	 * shrinks stalls the scaler — that one geometry keeps the centre-crop
	 * override (device-verified fatal: 0 frames, wedged the binned
	 * sensor), everything else passes through. */
	bool single_axis_squeeze = !ctx->cfg.keep_aspect &&
		(scl_in_w == out_w) != (scl_in_h == out_h);
	PipelinePrecropRect precrop = pipeline_common_compute_precrop(
		scl_in_w, scl_in_h, out_w, out_h,
		ctx->cfg.keep_aspect || single_axis_squeeze);
	if (single_axis_squeeze)
		printf("> [maruko] note: keepAspect=false requests a "
			"single-axis squeeze (%ux%u -> %ux%u) the I6C SCL "
			"stalls on — using a centre-crop %ux%u@%u,%u instead\n",
			scl_in_w, scl_in_h, out_w, out_h,
			precrop.w, precrop.h, precrop.x, precrop.y);
	/* configure_maruko_scl applies this AR rect as the SCL source crop
	 * (real centre-crop), then scales it 1:1/uniformly to the encode
	 * size — no anamorphic squeeze. */
	if (ctx->cfg.verbose) {
		if (precrop.x || precrop.y ||
		    precrop.w != scl_in_w || precrop.h != scl_in_h) {
			printf("  - Precrop: %ux%u -> %ux%u (offset %u,%u)\n",
				scl_in_w, scl_in_h, precrop.w, precrop.h,
				precrop.x, precrop.y);
		}
	}

	/* Approach C zoom: when zoom_pct ∈ (0, 1), shrink BOTH the SCL
	 * output dim AND the SCL crop window — SCL reads exactly the rect
	 * and emits it 1:1, no upscale, no bandwidth pressure.  The encoded
	 * resolution drops to the crop dim; SPS/PPS carries it to the
	 * receiver.  zoom_pct is MUT_RESTART so this only runs at start;
	 * live x/y pan is handled by maruko_pipeline_apply_zoom which
	 * re-issues SetPortConfig with new offsets at the same dim. */
	PipelinePrecropRect zoom_rect = {0, 0, 0, 0};
	int have_zoom = 0;
	if (ctx->cfg.zoom_pct > 0.0 && ctx->cfg.zoom_pct < 1.0) {
		uint32_t zw = out_w;
		uint32_t zh = out_h;
		MaruWindowRect_t zrect;
		maruko_compute_zoom_dim(out_w, out_h, ctx->cfg.zoom_pct, &zw, &zh);
		zrect = maruko_compute_zoom_rect(precrop.w, precrop.h, zw, zh,
			ctx->cfg.zoom_x, ctx->cfg.zoom_y);
		if (ctx->cfg.verbose)
			printf("> [maruko] Zoom: pct=%.3f → %ux%u "
				"(rect %ux%u@%u,%u within precrop %ux%u)\n",
				ctx->cfg.zoom_pct, zw, zh,
				zrect.u16Width, zrect.u16Height,
				zrect.u16X, zrect.u16Y, precrop.w, precrop.h);
		ctx->cfg.image_width = zw;
		ctx->cfg.image_height = zh;
		out_w = zw;
		out_h = zh;
		/* The zoom window stays relative to the AR surface here;
		 * configure_maruko_scl translates it into channel-surface
		 * coords once it knows whether the AR rect became an input
		 * crop. */
		zoom_rect.x = zrect.u16X;
		zoom_rect.y = zrect.u16Y;
		zoom_rect.w = zrect.u16Width;
		zoom_rect.h = zrect.u16Height;
		have_zoom = 1;
	}

	if (maruko_start_vif(&ctx->sensor) != 0)
		return -1;
	ctx->vif_started = 1;

	if (maruko_start_vpe(&ctx->sensor, out_w, out_h,
	    ctx->cfg.vpe_level_3dnr, &precrop,
	    have_zoom ? &zoom_rect : NULL,
	    ctx->cfg.image_mirror, ctx->cfg.image_flip) != 0)
		return -1;
	ctx->vpe_started = 1;
	/* Stash the AR base surface for live pan.  The zoom rect is always
	 * positioned inside this surface, not the full SCL input, or
	 * panning can leak outside keep-aspect framing. */
	ctx->scl_crop_x = precrop.x;
	ctx->scl_crop_y = precrop.y;
	ctx->scl_crop_w = precrop.w;
	ctx->scl_crop_h = precrop.h;
	if (have_zoom) {
		maruko_pipeline_set_zoom_status(ctx->cfg.zoom_pct,
			out_w, out_h,
			ctx->scl_crop_x + zoom_rect.x,
			ctx->scl_crop_y + zoom_rect.y,
			zoom_rect.w, zoom_rect.h);
	} else {
		maruko_pipeline_clear_zoom_status();
	}

	/* Debug OSD must initialise on the main thread BEFORE the VENC
	 * kthread spawns: the kernel mi_rgn driver creates a singlethread
	 * workqueue inside MI_RGN_InitDev, and the v5.10 OpenIPC kernel
	 * rejects that allocation when invoked from a non-main thread.
	 * SCL device/channel exist by this point (maruko_start_vpe), so
	 * AttachToChn can resolve to SCL/0/0/0 immediately. */
	if (ctx->cfg.show_osd && !ctx->debug_osd) {
		ctx->debug_osd = debug_osd_create(out_w, out_h, NULL);
		if (!ctx->debug_osd)
			fprintf(stderr,
				"WARNING: [maruko] debug OSD init failed — "
				"continuing without overlay\n");
	}

	/* IMU: opt-in BMI270 reader, FIFO mode (frame-synced via imu_drain
	 * in the per-frame loop).  The push callback is a stub right now —
	 * samples are read and discarded; future telemetry/sidecar export
	 * can replace the stub without touching the lifecycle.
	 *
	 * Init MUST run BEFORE bind_maruko_pipeline() because that path
	 * calls MI_VENC_StartRecvPic, and the IMU's auto-bias calibration
	 * is a blocking ~2 s loop (400 samples @ 200 Hz default).  On
	 * Maruko, blocking the main thread for 2 s after StartRecvPic
	 * leaves the VENC fd in a state where poll() never returns
	 * POLLIN and the stream loop never progresses (verified empirically
	 * on 192.168.2.12: drops to 0 fps with 2 s cal, recovers cleanly
	 * with 250 ms cal — the issue is the blocking duration vs the
	 * VENC's internal queue, not the IMU code itself).  Star6E does
	 * not exhibit this; pre-init only on Maruko. */
	if (ctx->cfg.imu.enabled && !ctx->imu) {
		ImuConfig imu_cfg = {
			.i2c_device = ctx->cfg.imu.i2c_device,
			.i2c_addr = ctx->cfg.imu.i2c_addr,
			.sample_rate_hz = ctx->cfg.imu.sample_rate_hz,
			.gyro_range_dps = ctx->cfg.imu.gyro_range_dps,
			.cal_file = ctx->cfg.imu.cal_file,
			.cal_samples = ctx->cfg.imu.cal_samples,
			.push_fn = maruko_pipeline_imu_push,
			.push_ctx = ctx,
			.use_thread = 0,
		};
		ctx->imu = imu_init(&imu_cfg);
		if (ctx->imu) {
			imu_start(ctx->imu);
		} else {
			fprintf(stderr,
				"WARNING: [maruko] IMU init failed, "
				"continuing without IMU\n");
		}
	}

	if (bind_maruko_pipeline(ctx) != 0)
		return -1;
	/* Initial zoom is built into SCL setup above (zoom rect = precrop,
	 * encoded dim = crop dim).  Live x/y pan goes through
	 * maruko_pipeline_apply_zoom; pct change triggers MUT_RESTART. */

	/* Phase 5: audio capture + RTP/UDP output.  Init AFTER bind_maruko
	 * so ctx->output socket is up; init returns 0 even on failure (warns
	 * and leaves ctx->audio.started=0). */
	(void)maruko_audio_init(&ctx->audio, &ctx->cfg, &ctx->output);
	if (ctx->audio.started) {
		audio_ring_init(&ctx->audio_recorder_ring);
		/* Aligned pointer write — encode thread picks this up on its
		 * next loop iteration. */
		ctx->audio.rec_ring = &ctx->audio_recorder_ring;
	}

	/* TS recorder state — initialised regardless of mode so the chn 1
	 * drain thread can call maruko_ts_recorder_write_stream() safely
	 * (no-op while fd<0).  Audio rate/channels reflect whether
	 * maruko_audio_init succeeded — when active, the PMT advertises
	 * audio and the recorder pops PCM frames off audio_recorder_ring. */
	{
		/* Mirror star6e_runtime: route Opus capture into Opus-in-TS PMT,
		 * PCM into SMPTE 302M.  G.711 is not muxed (rate stays 0). */
		int parsed = audio_codec_parse_name(ctx->cfg.audio.codec);
		uint8_t ts_codec = (parsed == AUDIO_CODEC_TYPE_OPUS)
			? TS_AUDIO_CODEC_OPUS : TS_AUDIO_CODEC_PCM_S302M;
		uint32_t rate = 0, ch = 0;
		if (ctx->audio.started &&
		    (parsed == AUDIO_CODEC_TYPE_RAW ||
		     parsed == AUDIO_CODEC_TYPE_OPUS)) {
			rate = ctx->audio.sample_rate;
			ch   = ctx->audio.channels;
		}
		star6e_ts_recorder_init(&ctx->ts_recorder, rate, (uint8_t)ch,
			ts_codec);
	}
	star6e_recorder_init(&ctx->recorder);
	mk_mirror_record_locks_init(ctx);
	/* Both recorders: the rotation thresholds are the operator's,
	 * not the format's, and only one recorder is ever active. */
	if (ctx->cfg.record.max_seconds > 0) {
		ctx->ts_recorder.rot.max_seconds = ctx->cfg.record.max_seconds;
		ctx->recorder.rot.max_seconds = ctx->cfg.record.max_seconds;
	}
	if (ctx->cfg.record.max_mb > 0) {
		uint64_t max_bytes =
			(uint64_t)ctx->cfg.record.max_mb * 1024 * 1024;

		ctx->ts_recorder.rot.max_bytes = max_bytes;
		ctx->recorder.rot.max_bytes = max_bytes;
	}

	/* Phase 7: dual VENC chn 1.  Started AFTER bind_maruko_pipeline
	 * succeeds — the Phase 7 SDK probe confirmed CreateChn(dev,1,...)
	 * needs chn 0 fully bound first.  Failure is non-fatal: ctx->dual
	 * stays NULL and the daemon continues with chn 0 only.
	 *
	 * Phase 6: also recognises "dual" (chn 1 → TS file) in addition to
	 * the existing "dual-stream" (chn 1 → UDP). */
	if (g_stab_fill_graph && ctx->cfg.record.enabled &&
	    (strcmp(ctx->cfg.record.mode, "dual-stream") == 0 ||
	     strcmp(ctx->cfg.record.mode, "dual") == 0)) {
		/* Dual chn 1 is RING-fed from port0 — port0 is unbound in stab-fill
		 * and RING+FRAME_BASE cannot mix on the one H26x device (F0a). */
		fprintf(stderr, "WARNING: [maruko] record.mode=%s is not available "
			"with framing=stab-fill — recording disabled this run\n",
			ctx->cfg.record.mode);
	} else if (ctx->cfg.record.enabled &&
	    (strcmp(ctx->cfg.record.mode, "dual-stream") == 0 ||
	     strcmp(ctx->cfg.record.mode, "dual") == 0) &&
	    !ctx->dual) {
		(void)maruko_pipeline_start_dual(ctx,
			ctx->cfg.record.bitrate,
			ctx->cfg.record.fps,
			ctx->cfg.record.gop_size,
			ctx->cfg.record.mode,
			ctx->cfg.record.server);
	}

	/* Phase 6: open recorder for "mirror" (chn 0 → file) and "dual"
	 * (chn 1 → file) modes.  Must run AFTER start_dual so that, in
	 * dual mode, the drain thread sees the recorder fd >= 0 by the
	 * time it pulls the first chn 1 frame.  Started here so that a
	 * follow-up SIGHUP reload that flips mode also re-opens the file.
	 *
	 * Format dispatch: "ts" → TS muxer + audio interleaving;
	 * "hevc" → raw HEVC stream (no audio).  Both backends share the
	 * same disk-space / rotation / sync_file_range plumbing. */
	if (ctx->cfg.record.enabled && ctx->cfg.record.dir[0] &&
	    (strcmp(ctx->cfg.record.mode, "mirror") == 0 ||
	     /* dual is refused under stab-fill (see the start_dual guard) —
	      * don't open a file its absent chn-1 drain thread would never
	      * feed.  mirror (chn 0 → file) works fine under stab-fill. */
	     (strcmp(ctx->cfg.record.mode, "dual") == 0 &&
	      !g_stab_fill_graph))) {
		(void)mk_mirror_record_open(ctx, ctx->cfg.record.dir);
	}

	ctx->output_enabled = 1;
	printf("> [maruko] pipeline configured\n");
	printf("  - Sensor: %ux%u @ %u\n",
		ctx->sensor.mode.crop.width, ctx->sensor.mode.crop.height,
		ctx->sensor.fps);
	printf("  - Image : %ux%u\n",
		ctx->cfg.image_width, ctx->cfg.image_height);
	if (ctx->cfg.image_width != ctx->sensor.mode.crop.width ||
	    ctx->cfg.image_height != ctx->sensor.mode.crop.height) {
		printf("  - VPE scaling: sensor %ux%u -> encode %ux%u\n",
			ctx->sensor.mode.crop.width,
			ctx->sensor.mode.crop.height,
			ctx->cfg.image_width, ctx->cfg.image_height);
	}
	printf("  - 3DNR  : level %d\n", ctx->cfg.vpe_level_3dnr);

	return 0;
}

/* ── Scene-detector stream decoders (i6c_venc_strm) ──────────────────── */

static uint32_t maruko_scene_frame_size(const i6c_venc_strm *s)
{
	uint32_t t = 0;
	unsigned int i;
	if (!s || !s->packet) return 0;
	for (i = 0; i < s->count; i++) t += s->packet[i].length;
	return t;
}

static uint8_t maruko_scene_is_idr(const i6c_venc_strm *s, int codec)
{
	unsigned int i;
	if (!s || !s->packet) return 0;
	for (i = 0; i < s->count; i++) {
		const i6c_venc_pack *p = &s->packet[i];
		const unsigned int info_cap = (unsigned int)(
			sizeof(p->packetInfo) / sizeof(p->packetInfo[0]));
		unsigned int k, n = p->packNum;
		if (n > info_cap) {
			if (p->data && p->length > p->offset) {
				size_t cursor = 0;
				const uint8_t *nal;
				size_t nal_len;

				while (h26x_util_annexb_next(p->data + p->offset,
				    p->length - p->offset, &cursor, &nal,
				    &nal_len)) {
					uint8_t type = codec == 0
						? h26x_util_h264_nalu_type(nal,
							nal_len)
						: h26x_util_hevc_nalu_type(nal,
							nal_len);
					if ((codec == 0 && type == 5) ||
					    (codec != 0 &&
					     (type == 19 || type == 20)))
						return 1;
				}
			}
			continue;
		}
		if (n > 0) {
			for (k = 0; k < n; k++) {
				if (codec == 0 && p->packetInfo[k].packType.h264Nalu == 5)
					return 1;
				if (codec != 0 &&
				    (p->packetInfo[k].packType.h265Nalu == 19 ||
				     p->packetInfo[k].packType.h265Nalu == 20))
					return 1;
			}
		} else {
			if (codec == 0 && p->naluType.h264Nalu == 5) return 1;
			if (codec != 0 && p->naluType.h265Nalu == 19) return 1;
		}
	}
	return 0;
}

static void maruko_scene_request_idr(void *ctx_ptr)
{
	MarukoBackendContext *ctx = ctx_ptr;
	if (idr_rate_limit_allow(ctx->venc_channel))
		maruko_mi_venc_request_idr(ctx->venc_device, ctx->venc_channel, 1);
}

/* Per-iteration state for maruko_pipeline_run / maruko_pipeline_process_stream.
 * Keeps the per-frame work in its own function (mirroring
 * star6e_runtime_process_stream) without ballooning its parameter list. */
typedef struct {
	MarukoRtpState rtp_state;
	H26xParamSets param_sets;
	RtpSidecarSender sidecar;
	StreamMetricsState metrics;
	HevcRtpStats pktzr_interval;
	i6c_venc_pack *cached_packs;
	uint32_t cached_packs_cap;
	unsigned int frame_counter;
	int venc_fd;
	uint64_t last_activity_us;
	uint64_t last_warn_us;
} MarukoStreamRuntime;

#define MARUKO_IDLE_ABORT_US ((uint64_t)20 * 1000000ULL)
#define MARUKO_IDLE_WARN_US  ((uint64_t)1 * 1000000ULL)
#define MARUKO_PKTZR_VERBOSE_ACTIVE(ctx) ((ctx)->cfg.verbose && \
	(ctx)->cfg.rc_codec == PT_H265 && \
	(ctx)->cfg.stream_mode == MARUKO_STREAM_RTP)

static void maruko_pipeline_init_streaming(MarukoBackendContext *ctx,
	MarukoStreamRuntime *rt)
{
	memset(rt, 0, sizeof(*rt));
	if (ctx->cfg.stream_mode == MARUKO_STREAM_RTP) {
		maruko_video_init_rtp_state(&rt->rtp_state, ctx->cfg.rc_codec,
			ctx->sensor.fps);
		rtp_sidecar_sender_init(&rt->sidecar, ctx->cfg.sidecar_port);
	}
	if (ctx->cfg.verbose) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		stream_metrics_start(&rt->metrics, &now);
	}
	/* Block on MI_VENC_GetFd(chn) via poll() instead of spinning on
	 * Query + usleep(500).  The fd signals POLLIN when a frame is
	 * ready, so the thread wakes ~fps-times/s instead of ~2000/s.
	 * If the SDK returns fd < 0 on an unknown BSP variant, fall back
	 * to the original spin loop. */
	rt->venc_fd = maruko_mi_venc_get_fd(ctx->venc_device, ctx->venc_channel);
	rt->last_activity_us = wb_monotonic_us();
	rt->last_warn_us = rt->last_activity_us;

	/* Phase-1 stab SCL bench (development builds only; no-op stub in
	 * production).  Runs once, inert unless WAYBEAM_STAB_BENCH is set. */
	maruko_stab_bench_maybe_start(ctx);
}

static void maruko_pipeline_cleanup_streaming(MarukoBackendContext *ctx,
	MarukoStreamRuntime *rt)
{
	/* Join the bench thread before tearing down ports (mirrors the stab
	 * teardown ordering rule: never disable a tapped port with the reader
	 * still live). */
	maruko_stab_bench_stop();
	if (rt->venc_fd >= 0) {
		maruko_mi_venc_close_fd(ctx->venc_device, ctx->venc_channel);
		rt->venc_fd = -1;
	}
	free(rt->cached_packs);
	rt->cached_packs = NULL;
	rt->cached_packs_cap = 0;
	rtp_sidecar_sender_close(&rt->sidecar);
}

static void maruko_service_frame_gate(MarukoBackendContext *ctx);
static void maruko_service_ring_low_water(MarukoOutput *output);

/* Maruko wording for frame_gate_setup()'s verdict; the checks themselves are
 * shared in frame_gate.c. */
static void maruko_report_frame_gate_setup(FrameGateSetupStatus st,
	const FrameGate *g, uint32_t slot_count)
{
	switch (st) {
	case FRAME_GATE_SETUP_NO_RING:
		fprintf(stderr, "WARNING: [maruko] video0.frameGate=on needs a "
			"frame-shm:// transport; gate inert\n");
		return;
	case FRAME_GATE_SETUP_CLOSE_TOO_HIGH:
		fprintf(stderr, "WARNING: [maruko] frameGateCloseSlots %u exceeds "
			"the ring's %u slots; gate can never close\n",
			(unsigned)g->cfg.close_slots, (unsigned)slot_count);
		return;
	case FRAME_GATE_SETUP_READY:
		printf("> [maruko] frame gate on: close>=%u open<=%u escape=%ums "
			"ring=%u slots\n",
			(unsigned)g->cfg.close_slots, (unsigned)g->cfg.open_slots,
			(unsigned)(g->cfg.max_closed_us / 1000u),
			(unsigned)slot_count);
		return;
	}
}

/* `gated` suppresses both the warning and the abort: a closed frame gate is
 * the reason there is no encoder data, and a 20 s gate (reachable via
 * frameGateMaxClosedMs) would otherwise tear the stream loop down for doing
 * exactly what it was told to do. */
static int maruko_pipeline_check_idle_abort(MarukoStreamRuntime *rt, int gated)
{
	uint64_t now_us = wb_monotonic_us();

	if (gated) {
		/* Keep the clocks moving so neither timer fires the moment
		 * the gate reopens. */
		rt->last_activity_us = now_us;
		rt->last_warn_us = now_us;
		return 0;
	}
	if (now_us - rt->last_warn_us >= MARUKO_IDLE_WARN_US) {
		printf("> [maruko] waiting for encoder data...\n");
		rt->last_warn_us = now_us;
	}
	if (now_us - rt->last_activity_us >= MARUKO_IDLE_ABORT_US) {
		fprintf(stderr,
			"ERROR: [maruko] no encoder data"
			" received; aborting stream loop\n");
		return -1;
	}
	return 0;
}

/* Returns: -1 fatal, 0 retry the outer loop, 1 frame ready (use *stat). */
static int maruko_pipeline_await_frame(MarukoBackendContext *ctx,
	MarukoStreamRuntime *rt, i6c_venc_stat *stat)
{
	/* Gated: leave the encoded frame in VENC's output FIFO rather than
	 * draining it.  The gate is still serviced every pass, off the egress
	 * ring's own occupancy, so the reopen path never depends on the
	 * drain. */
	if (!frame_gate_is_open(&ctx->frame_gate)) {
		maruko_service_frame_gate(ctx);
		/* Keep publishing egress pressure while gated — see the Star6E
		 * twin; freezing it hides the congestion that closed the gate. */
		maruko_service_ring_low_water(&ctx->output);
		/* The two things that otherwise run only inside
		 * maruko_pipeline_process_stream(), which this branch returns
		 * before: an HTTP record command, and the sidecar's
		 * subscription/clock-sync traffic.  Neither may wait for the
		 * next escape pulse -- with frameGateMaxClosedMs allowed to
		 * 60 s that is a minute of an unresponsive API.  Star6E lifted
		 * the same pair; CV610 services record control at the top of
		 * its loop.  idle_wait() polls the sidecar and sleeps only the
		 * remainder, so the reopen latency stays 2 ms. */
		maruko_service_record_control(ctx);
		idle_wait(&rt->sidecar, 2);
		return maruko_pipeline_check_idle_abort(rt,
			!frame_gate_is_open(&ctx->frame_gate));
	}

	if (rt->venc_fd >= 0) {
		/* 1 s timeout caps the g_maruko_running cancellation latency
		 * without wasting cycles on short periodic wakes.  Frames at
		 * ≥60 fps arrive well within this. */
		struct pollfd pfd = { .fd = rt->venc_fd, .events = POLLIN };

		/* The gate-closed case is handled above this poll, so the 1 s
		 * cancellation latency applies only to the frame wait. */
		(void)poll(&pfd, 1, 1000);
		/* POLLERR/POLLHUP/POLLNVAL: the SDK closed the fd under us
		 * (BSP quirk, pipeline reinit, VPE unbind).  Fall back to the
		 * Query+usleep path for the rest of the loop's lifetime. */
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			maruko_mi_venc_close_fd(ctx->venc_device,
				ctx->venc_channel);
			rt->venc_fd = -1;
			usleep(1000);
			return 0;
		}
		if (!(pfd.revents & POLLIN)) {
			/* Reopen path: nothing else runs while gated. */
			maruko_service_frame_gate(ctx);
			return maruko_pipeline_check_idle_abort(rt,
				!frame_gate_is_open(&ctx->frame_gate));
		}
	}

	memset(stat, 0, sizeof(*stat));
	MI_S32 ret = maruko_mi_venc_query(ctx->venc_device,
		ctx->venc_channel, stat);
	if (ret != 0) {
		if (ret == -EAGAIN || ret == EAGAIN) {
			maruko_service_frame_gate(ctx);
			usleep(100);
			return 0;
		}
		fprintf(stderr,
			"ERROR: [maruko] MI_VENC_Query failed %d\n", ret);
		return -1;
	}

	if (stat->curPacks == 0) {
		int rc;

		maruko_service_frame_gate(ctx);
		rc = maruko_pipeline_check_idle_abort(rt,
			!frame_gate_is_open(&ctx->frame_gate));
		if (rc != 0)
			return rc;
		/* On the fd path this means spurious POLLIN (rare).  On the
		 * fallback spin path, sleep 500us: <5 % of the 11 ms frame
		 * period at 90 fps, keeps frame latency low while reducing
		 * idle syscalls. */
		if (rt->venc_fd < 0)
			usleep(500);
		return 0;
	}

	rt->last_activity_us = wb_monotonic_us();
	rt->last_warn_us = rt->last_activity_us;
	return 1;
}

static void maruko_pipeline_log_verbose_frame(MarukoBackendContext *ctx,
	MarukoStreamRuntime *rt, size_t total_bytes,
	const HevcRtpStats *frame_pktzr, unsigned int pack_count)
{
	StreamMetricsSample sample;
	struct timespec verbose_ts_now;

	stream_metrics_record_frame(&rt->metrics, total_bytes);
	if (MARUKO_PKTZR_VERBOSE_ACTIVE(ctx)) {
		rt->pktzr_interval.total_nals += frame_pktzr->total_nals;
		rt->pktzr_interval.single_packets += frame_pktzr->single_packets;
		rt->pktzr_interval.fu_packets += frame_pktzr->fu_packets;
		rt->pktzr_interval.rtp_packets += frame_pktzr->rtp_packets;
		rt->pktzr_interval.rtp_payload_bytes += frame_pktzr->rtp_payload_bytes;
	}
	clock_gettime(CLOCK_MONOTONIC, &verbose_ts_now);
	if (!stream_metrics_sample(&rt->metrics, &verbose_ts_now, &sample))
		return;

	printf("[verbose] %lds | %u fps | %u kbps"
		" | frame %u | avg %u B/frame | %u packs\n",
		sample.uptime_s, sample.fps, sample.kbps,
		rt->frame_counter, sample.avg_bytes, pack_count);
	if (MARUKO_PKTZR_VERBOSE_ACTIVE(ctx)) {
		unsigned int avg_rtp_payload =
			rt->pktzr_interval.rtp_packets > 0
			? (unsigned int)(rt->pktzr_interval.rtp_payload_bytes /
				rt->pktzr_interval.rtp_packets) : 0;
		printf("[pktzr] nals %u | rtp %u | fill %u B"
			" | single %u | fu %u\n",
			rt->pktzr_interval.total_nals,
			rt->pktzr_interval.rtp_packets,
			avg_rtp_payload,
			rt->pktzr_interval.single_packets,
			rt->pktzr_interval.fu_packets);
		memset(&rt->pktzr_interval, 0, sizeof(rt->pktzr_interval));
	}
	fflush(stdout);
}

/* Runs on the writer thread, one access unit at a time, in order.
 *
 * No lock: the writer is started only after a recorder is open and joined
 * before it closes (mk_mirror_record_open / mk_mirror_record_close), so the
 * sink cannot run concurrently with an open or a close.  That is the whole
 * reason the writer's lifetime is tied to the recording's. */
static void mk_mirror_record_sink(void *opaque, const uint8_t *au, size_t len,
	uint64_t pts_90khz, int is_idr)
{
	MarukoBackendContext *ctx = opaque;

	if (star6e_ts_recorder_is_active(&ctx->ts_recorder))
		(void)star6e_ts_recorder_write_video(&ctx->ts_recorder, au, len,
			pts_90khz, is_idr);
	else
		(void)star6e_recorder_write_au(&ctx->recorder, au, len,
			is_idr);
}

/* Initialised before anything can reach it — the status callback runs on the
 * httpd thread and takes rec_writer_lock.  rec_locks_ready survives because
 * init and destroy sit in different phases of the graph lifecycle: a teardown
 * after a failed bring-up must not destroy a mutex that was never
 * initialised. */
static void mk_mirror_record_locks_init(MarukoBackendContext *ctx)
{
	if (ctx->rec_locks_ready)
		return;
	pthread_mutex_init(&ctx->rec_writer_lock, NULL);
	ctx->rec_locks_ready = 1;
}

/* Idempotent, so the runtime can insist on the ordering without caring whether
 * configure_graph() already did it. */
void mk_mirror_record_locks_init_public(MarukoBackendContext *ctx)
{
	mk_mirror_record_locks_init(ctx);
}

/* Runs once the writer has been joined and freed -- inline on the encode loop
 * when the sink finished inside the deadline, on the reaper thread when it did
 * not.  The closes live here so that on both paths they happen only after the
 * sink is guaranteed never to run again. */
/* Bounded wait for a detached reaper to release the recorders.  Teardown only:
 * a reaper that outlives the context would dereference it after backend.c has
 * freed it. */
#define REC_REAP_TEARDOWN_WAIT_MS 2000
static void mk_wait_reap(MarukoBackendContext *ctx, unsigned ms)
{
	unsigned waited = 0;

	while (__atomic_load_n(&ctx->rec_reap_pending, __ATOMIC_ACQUIRE) &&
	       waited < ms) {
		usleep(2000);
		waited += 2;
	}
}

static void mk_mirror_record_reap(void *opaque)
{
	MarukoBackendContext *ctx = opaque;

	star6e_ts_recorder_stop(&ctx->ts_recorder);
	star6e_recorder_stop(&ctx->recorder);
	__atomic_store_n(&ctx->rec_reap_pending, 0, __ATOMIC_RELEASE);
}

/* `async` picks whether this may join.  Mid-run (the encode loop) it must not:
 * a medium that has stopped completing would park live video inside a
 * record/stop.  At teardown it must, because there is no later. */
static void mk_mirror_record_close_mode(MarukoBackendContext *ctx,
	unsigned grace_ms, int async)
{
	VencRecWriter *w;

	/* An init that failed before mk_mirror_record_locks_init() leaves the
	 * mutex zeroed; locking or destroying one of those is undefined. */
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
		mk_wait_reap(ctx, REC_REAP_TEARDOWN_WAIT_MS);
		if (__atomic_load_n(&ctx->rec_reap_pending, __ATOMIC_ACQUIRE)) {
			fprintf(stderr, "WARN: recorder writer still holding the "
				"file at teardown; skipping the close\n");
			return;
		}
	}

	/* Detach under the lock so a status poll already inside
	 * venc_rec_writer_stats() finishes against a live writer and any later
	 * one sees NULL.  Harvest the counters first — they must outlive the
	 * writer they came from. */
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

	/* Independent of the writer, so it need not wait on one. */
	ctx->audio.rec_ring = NULL;

	{
		uint64_t dropped = 0;

		if (async) {
			__atomic_store_n(&ctx->rec_reap_pending, 1,
				__ATOMIC_RELEASE);
			venc_rec_writer_stop_bounded_async(w, grace_ms,
				&dropped, mk_mirror_record_reap, ctx);
		} else {
			venc_rec_writer_stop_bounded(w, grace_ms, &dropped);
		}
		/* Only if there WAS a writer: the stop leaves *dropped at 0 for
		 * a NULL one, which would clobber the harvest above.  Stored
		 * under the lock because the httpd thread reads it and an
		 * unlocked 64-bit store tears on ARM32. */
		if (w) {
			pthread_mutex_lock(&ctx->rec_writer_lock);
			ctx->rec_dropped_frames = dropped;
			pthread_mutex_unlock(&ctx->rec_writer_lock);
		}
	}
	if (!async)
		mk_mirror_record_reap(ctx);
}
/* Mid-run close: never joins past the deadline. */
static void mk_mirror_record_close(MarukoBackendContext *ctx, unsigned grace_ms)
{
	mk_mirror_record_close_mode(ctx, grace_ms, 1);
}


/* Begin a recording: open the recorder, then start the writer that feeds it.
 * The single entry point for every start — boot auto-start and HTTP alike.
 * Returns 1 when a file is open. */
static int mk_mirror_record_open(MarukoBackendContext *ctx, const char *dir)
{
	int started;

	/* Same guard as the close: rec_writer_lock is taken below, and locking
	 * a mutex that was never initialised is undefined. */
	if (!dir || !dir[0] || !ctx->rec_locks_ready)
		return 0;
	/* Stop first: a start over a live recording must not leak the open fd,
	 * and only one of the two recorders may ever be active. */
	mk_mirror_record_close(ctx, MARUKO_REC_STOP_GRACE_MS);

	/* On a healthy card the stop above completed inline and this is clear.
	 * If it did not, a detached reaper still owns both recorders'
	 * descriptors -- the previous recording's sink has not returned -- and
	 * starting over them would race the close. */
	if (__atomic_load_n(&ctx->rec_reap_pending, __ATOMIC_ACQUIRE)) {
		fprintf(stderr, "WARN: [maruko] record start refused; the "
			"previous recording's writer has not released the file "
			"yet (storage not completing writes)\n");
		return 0;
	}

	/* Before the open, not after it: these describe the recording being
	 * started, and a start that FAILS must not leave the previous
	 * recording's drop count standing as if it belonged to this one.
	 * Star6E and CV610 both order it this way. */
	pthread_mutex_lock(&ctx->rec_writer_lock);
	ctx->rec_dropped_frames = 0;
	ctx->rec_flatten_failures = 0;
	ctx->rec_writer_peak_depth = 0;
	pthread_mutex_unlock(&ctx->rec_writer_lock);

	if (strcmp(ctx->cfg.record.format, "hevc") == 0) {
		started = star6e_recorder_start(&ctx->recorder, dir) == 0;
	} else if (strcmp(ctx->cfg.record.format, "ts") == 0) {
		ctx->audio.rec_ring = ctx->audio.started
			? &ctx->audio_recorder_ring : NULL;
		started = star6e_ts_recorder_start(&ctx->ts_recorder, dir,
			ctx->audio.rec_ring) == 0;
		if (!started)
			ctx->audio.rec_ring = NULL;
	} else {
		fprintf(stderr,
			"WARNING: [maruko] record.format '%s' not "
			"supported (ts|hevc) — recording disabled\n",
			ctx->cfg.record.format);
		return 0;
	}
	if (!started)
		return 0;

	printf("> [maruko] recording %s to %s (%s%s)\n",
		ctx->cfg.record.format, dir, ctx->cfg.record.mode,
		ctx->audio.rec_ring ? " + audio" : "");

	/* Dual: chn 1 feeds the file through its own drain thread, so the
	 * mirror producer gate (!ctx->dual, see maruko_pipeline_process_stream)
	 * never feeds this writer — starting one leaves an idle thread for the
	 * whole session.  The start IDR is skipped for the same reason:
	 * maruko_recorder_start_idr() names ctx->venc_channel, i.e. chn 0, the
	 * LIVE stream, so firing it here would inject a keyframe into the live
	 * path while doing nothing for the recording.  That is the trap named in
	 * include/star6e_ts_recorder.h — a shared hook cannot name the right
	 * channel.  Same guard as star6e_runtime.c's mirror_record_open().
	 *
	 * Unlike Star6E, Maruko has no separate chn-1 start path to issue the
	 * keyframe (star6e_runtime.c's dual_rec_thread_fn does it there), and
	 * the HTTP record controls are gated !ctx->dual — so this is the only
	 * place that can ask.  Forced, not rate-limited: a GDR craft emits no
	 * periodic IDR, so a request the limiter coalesces away yields a file
	 * with no IRAP anywhere in it. */
	if (ctx->dual) {
		idr_rate_limit_force(ctx->dual->channel);
		(void)maruko_mi_venc_request_idr(ctx->venc_device,
			ctx->dual->channel, 1);
		return 1;
	}

	/* After the file is open, so the sink never sees a closed recorder.
	 *
	 * A writer that fails to start leaves the recording OPEN: the producer
	 * gate falls back to writing synchronously when rec_writer is NULL, so
	 * the recording still lands — on the encode loop, which is what the
	 * warning is about. */
	pthread_mutex_lock(&ctx->rec_writer_lock);
	if (venc_rec_writer_start(&ctx->rec_writer, mk_mirror_record_sink,
			ctx) != 0)
		fprintf(stderr, "WARNING: [maruko] recorder writer thread did "
			"not start; mirror recording will write on the encode "
			"loop and can stall the live stream on slow storage\n");
	pthread_mutex_unlock(&ctx->rec_writer_lock);

	/* Start the file on a keyframe.  Forced, not rate-limited: a GDR craft
	 * (resilience=racing) emits no periodic IDR, so a request the limiter
	 * coalesces away yields a file with no IRAP anywhere in it. */
	maruko_recorder_start_idr(ctx);
	return 1;
}

/* HTTP record control: drain the start/stop request flags so they never
 * accumulate across reinit, then act only when this runtime actually owns the
 * recorder.  Maruko's chn 1 drain thread (dual mode) writes the file directly
 * -- same race rationale as the `if (!ctx->dual)` guard on the chn 0 write.
 * Format dispatch mirrors the config-driven start path.
 *
 * Called from maruko_pipeline_process_stream() AND from the gated branch of
 * maruko_pipeline_await_frame(): while the gate is closed the stream path
 * never runs, so servicing it only there made a record command wait for an
 * escape pulse (up to frameGateMaxClosedMs) instead of being acted on. */
static void maruko_service_record_control(MarukoBackendContext *ctx)
{
	char rec_dir[256];
	int start_pending;
	int stop_pending;
	int is_hevc;
	int is_ts;
	int started = 0;

	if (ctx->dual)
		return;
	start_pending = venc_api_get_record_start(rec_dir, sizeof(rec_dir));
	stop_pending = venc_api_get_record_stop();
	is_hevc = (strcmp(ctx->cfg.record.format, "hevc") == 0);
	is_ts = (strcmp(ctx->cfg.record.format, "ts") == 0);

	if ((start_pending || stop_pending) && !is_ts && !is_hevc) {
		fprintf(stderr,
			"WARNING: [maruko] HTTP record control "
			"ignored: format='%s' not supported "
			"(ts|hevc)\n", ctx->cfg.record.format);
		return;
	}
	if (start_pending) {
		/* mk_mirror_record_open() closes any live recording first,
		 * joining its writer, so the old file's queued tail cannot land
		 * in the new one. */
		started = mk_mirror_record_open(ctx, rec_dir);
		(void)started;
	}
	if (stop_pending)
		mk_mirror_record_close(ctx, MARUKO_REC_STOP_GRACE_MS);
}

/* Process one ready frame: GetStream → per-frame work → ReleaseStream
 * → optional sidecar trailer + verbose log.  Mirrors
 * star6e_runtime_process_stream().  Returns: -1 fatal, 0 ok, 1 retry
 * outer loop. */
/* HEVC NAL type / SDK eRefType constants — mirror star6e_runtime.c.
 * SigmaStar i6c VENC firmware has the same gap as i6e: it computes the
 * SVC-T pyramid and labels each frame's eRefType correctly, but writes
 * every NAL as TRAIL_R (type 1).  Generic HEVC decoders need TRAIL_N
 * (type 0) to know which frames are non-reference. */
#define HEVC_NAL_TRAIL_N 0
#define HEVC_NAL_TRAIL_R 1
/* MARUKO_REFTYPE_ENHANCE_P_NOTFORREF (=5) is defined in maruko_video.h. Was
 * locally 4 (HiSilicon value) — wrong for the SigmaStar enum. */

static size_t maruko_nal_header_idx(const uint8_t *buf, size_t len)
{
	size_t i = 0;
	while (i < len && buf[i] == 0) i++;
	if (i < len && buf[i] == 0x01) i++;
	return i < len ? i : len;
}

static void maruko_patch_pack_to_trail_n(i6c_venc_pack *pack)
{
	if (!pack || !pack->data || pack->length == 0)
		return;
	if (pack->packNum > 0) {
		const unsigned int info_cap = (unsigned int)(sizeof(pack->packetInfo) /
			sizeof(pack->packetInfo[0]));
		unsigned int n = pack->packNum;
		unsigned int k;
		if (n > info_cap) {
			unsigned int off = pack->offset;
			if (off < pack->length)
				(void)h26x_util_hevc_patch_trail_r_to_n(
					pack->data + off, pack->length - off);
			return;
		}
		for (k = 0; k < n; ++k) {
			unsigned int off = pack->packetInfo[k].offset;
			unsigned int nlen = pack->packetInfo[k].length;
			if (off >= pack->length || nlen == 0 ||
			    off + nlen > pack->length)
				continue;
			size_t hdr = maruko_nal_header_idx(pack->data + off, nlen);
			if (hdr >= nlen) continue;
			if (pack->data[off + hdr] == 0x02) {
				pack->data[off + hdr] = 0x00;
			}
		}
		return;
	}
	if (pack->offset >= pack->length)
		return;
	{
		unsigned int off = pack->offset;
		unsigned int nlen = pack->length - off;
		size_t hdr = maruko_nal_header_idx(pack->data + off, nlen);
		if (hdr >= nlen) return;
		if (pack->data[off + hdr] == 0x02) {
			pack->data[off + hdr] = 0x00;
		}
	}
}

static void maruko_patch_stream_to_trail_n(i6c_venc_strm *s)
{
	unsigned int i;
	if (!s || !s->packet) return;
	for (i = 0; i < s->count; i++)
		maruko_patch_pack_to_trail_n(&s->packet[i]);
}

/* Encoded bytes accumulated since the debug OSD's last 1 Hz refresh.  Summed
 * from the encoder's own frame sizes rather than the transport's byte count so
 * the row stays truthful with output disabled and excludes packetization
 * overhead — it reads the encoder against its RC target, not the wire.  Single
 * writer (the pipeline thread, which is also the only reader).  Mirrors
 * g_osd_enc_bytes in star6e_runtime.c. */
static uint64_t g_osd_enc_bytes;

/* Recorder start — Star6E parity, see runtime_request_idr() in
 * src/star6e_runtime.c.  These two sites previously called the SDK directly,
 * so they were neither paced nor counted: the only IDR sources on any backend
 * outside /api/v1/idr/stats, which is precisely what made the contract's
 * "every source is paced and counted" claim false for Maruko. */
static void maruko_recorder_start_idr(MarukoBackendContext *ctx)
{
	idr_rate_limit_force(ctx->venc_channel);
	(void)maruko_mi_venc_request_idr(ctx->venc_device,
		ctx->venc_channel, 1);
}

/* frame-shm ring low-water measurement — byte-symmetric with the Star6E side
 * (src/star6e_runtime.c star6e_service_ring_low_water), state on the output
 * for the same reason.  Both backends share one definition on purpose: a
 * per-backend divergence in shared behaviour is a standing drift trap here. */
/* Adaptive frame gate over chn 0 — the Maruko twin of
 * star6e_service_frame_gate().  Same rationale (a bitrate write implicitly
 * keyframes, StopRecvPic changes no encoder state) and the same signal, the
 * instantaneous egress-ring occupancy rather than the 200 ms low-water
 * window.  See include/frame_gate.h. */
static void maruko_service_frame_gate(MarukoBackendContext *ctx)
{
	venc_frame_ring_fill_t fill;
	uint64_t now_us;

	if (!ctx || !frame_gate_enabled(&ctx->frame_gate))
		return;

	if (maruko_output_frame_ring_fill(&ctx->output, &fill) != 0) {
		/* The ring went away across a reinit.  Release the gate rather
		 * than leaving the drain stopped with no occupancy signal that
		 * could ever reopen it.  No SDK call: intake was never
		 * stopped, only the drain. */
		if (!frame_gate_is_open(&ctx->frame_gate))
			frame_gate_force_open(&ctx->frame_gate,
				wb_monotonic_us());
		return;
	}

	now_us = wb_monotonic_us();

	/* Mirror mode (!ctx->dual) puts the recorder on chn 0 as well, so a
	 * close would punch holes in the file for a radio problem.  Suppress
	 * closes only — an already-closed gate must still be able to reopen. */
	if (!ctx->dual && frame_gate_is_open(&ctx->frame_gate) &&
	    star6e_record_wants_frame(&ctx->ts_recorder, &ctx->recorder))
		return;

	/* The policy flip IS the actuator, so the returned action needs no
	 * handling: the drain loop stops pulling while the gate reads closed
	 * and VENC's own output FIFO applies the backpressure.  Nothing is
	 * issued to the SDK deliberately — stop_recv/start_recv emit an IRAP on
	 * every resume, measured at three per reopen on this board. */
	(void)frame_gate_observe(&ctx->frame_gate, fill.used_slots, now_us);
}

static void maruko_service_ring_low_water(MarukoOutput *output)
{
	venc_frame_ring_fill_t fill;
	uint64_t now_us;

	if (!output)
		return;
	if (maruko_output_frame_ring_fill(output, &fill) != 0) {
		/* Transport switch away from frame-shm — see the
		 * star6e_runtime equivalent. */
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

		venc_frame_ring_set_low_water(output->frame_ring, slots);
	}
}

static uint32_t maruko_osd_box_px(float v, uint32_t canvas, uint32_t net)
{
	float r;

	if (canvas == 0 || net == 0)
		return 0;
	r = v * (float)canvas / (float)net;
	if (!(r >= 0.0f))
		r = 0.0f;
	if (r > (float)(canvas - 1))
		r = (float)(canvas - 1);
	return (uint32_t)r;
}

static int maruko_pipeline_process_stream(MarukoBackendContext *ctx,
	MarukoStreamRuntime *rt, const i6c_venc_stat *stat)
{
	i6c_venc_strm stream = {0};

	stream.count = stat->curPacks;
	if (stat->curPacks > rt->cached_packs_cap) {
		free(rt->cached_packs);
		rt->cached_packs = malloc(stat->curPacks * sizeof(i6c_venc_pack));
		if (!rt->cached_packs) {
			rt->cached_packs_cap = 0;
			fprintf(stderr,
				"ERROR: [maruko] packet alloc failed\n");
			return -1;
		}
		rt->cached_packs_cap = stat->curPacks;
	}
	stream.packet = rt->cached_packs;

	/* Drain IMU FIFO BEFORE GetStream so any future telemetry/sidecar
	 * consumer sees fresh samples for the frame currently being
	 * captured.  Cheap when imu.enabled=false (state pointer is NULL).
	 * Mirrors star6e_runtime.c:727-728. */
	if (ctx->imu)
		imu_drain(ctx->imu);

	MI_S32 ret = maruko_mi_venc_get_stream(ctx->venc_device,
		ctx->venc_channel, &stream, 10);
	if (ret != 0) {
		if (ret == -EAGAIN || ret == EAGAIN) {
			usleep(100);
			return 1;
		}
		fprintf(stderr,
			"ERROR: [maruko] MI_VENC_GetStream failed %d\n", ret);
		return -1;
	}
	if (maruko_video_reject_incomplete_access_unit(&stream,
	    &ctx->output)) {
		(void)maruko_mi_venc_release_stream(ctx->venc_device,
			ctx->venc_channel, &stream);
		return 0;
	}

	/* refPred error-resilience marking — rewrite TRAIL_R → TRAIL_N for
	 * frames the SDK marked as ENHANCE_P_NOTFORREF.  i6c shares this
	 * encoder-firmware gap with i6e (Star6E): the pyramid logic is
	 * correct internally but every NAL is emitted as TRAIL_R, leaving
	 * generic decoders unable to identify non-reference frames.  See
	 * star6e_runtime.c for the matching helper. */
	if (ctx->output.svct_active &&
	    stream.h265Info.refType == MARUKO_REFTYPE_ENHANCE_P_NOTFORREF) {
		maruko_patch_stream_to_trail_n(&stream);
	}

	++rt->frame_counter;

	/* Cold-boot FPS kick: the ISP bin's AE overrides sensor timing on
	 * first init, locking FPS below target (e.g. 74fps instead of
	 * 89fps).  Re-kick MI_SNR_SetFps after ~1 second of frames to
	 * force correct sensor timing.  Same fix as Star6E CUS3A
	 * cold-boot FPS lock. */
	if (rt->frame_counter == (unsigned int)ctx->sensor.fps) {
		MI_SNR_SetFps(ctx->sensor.pad_id, ctx->sensor.fps);
		printf("> [maruko] delayed FPS kick: pad %d fps %u "
			"(cold-boot fix at frame %u)\n",
			ctx->sensor.pad_id, ctx->sensor.fps, rt->frame_counter);
	}

	/* Orientation (image.flip / image.mirror) is applied once at bring-up
	 * (sensor_select + maruko_start_vpe) and holds — no per-frame re-apply
	 * needed (IMX415 device-verified zero drift).  See star6e_runtime.c. */

	/* Debug OSD overlay — Star6E parity (star6e_runtime.c:825-849). */
	if (ctx->debug_osd) {
		static unsigned int osd_prev_frame;
		static struct timespec osd_prev_ts;
		static unsigned int osd_fps;
		static unsigned int osd_kbps;
		static MarukoAeOsdStatus osd_ae;
		struct timespec osd_now;

		debug_osd_begin_frame(ctx->debug_osd);
		debug_osd_sample_cpu(ctx->debug_osd);

		clock_gettime(CLOCK_MONOTONIC, &osd_now);
		long osd_ms = (osd_now.tv_sec - osd_prev_ts.tv_sec) * 1000 +
			(osd_now.tv_nsec - osd_prev_ts.tv_nsec) / 1000000;
		if (osd_ms >= 1000) {
			unsigned int df = rt->frame_counter - osd_prev_frame;
			osd_fps = (unsigned int)(df * 1000 / (unsigned long)osd_ms);
			osd_prev_frame = rt->frame_counter;
			osd_prev_ts = osd_now;
			/* bytes*8/ms is bits/ms, i.e. kbps directly. */
			osd_kbps = (unsigned int)(g_osd_enc_bytes * 8 /
				(uint64_t)osd_ms);
			g_osd_enc_bytes = 0;
			/* AE/AWB readouts ride the same 1Hz window — each
			 * refresh round-trips several MI_ISP getters. */
			maruko_controls_ae_osd_status(&osd_ae);
		}

		debug_osd_text(ctx->debug_osd, 0, "fps", "%u", osd_fps);
		debug_osd_text(ctx->debug_osd, 1, "cpu", "%d%%",
			debug_osd_get_cpu(ctx->debug_osd));

		/* Sensor readout + encoded output — lets a tester read back
		 * exactly which mode is live (WxH@fps + mode idx, encoded WxH
		 * + codec) straight off the overlay. */
		debug_osd_text(ctx->debug_osd, 2, "snr", "%ux%u@%u m%d",
			(unsigned)ctx->sensor.plane.capt.width,
			(unsigned)ctx->sensor.plane.capt.height,
			ctx->sensor.fps, ctx->sensor.mode_index);
		debug_osd_text(ctx->debug_osd, 3, "enc", "%ux%u %s",
			ctx->cfg.image_width, ctx->cfg.image_height,
			ctx->cfg.rc_codec == PT_H265 ? "h265" : "h264");

		/* Actual encoded rate against the configured RC target — the
		 * gap between the two is the RC undershoot/overshoot, and it
		 * separates an encoder problem from a link problem at a
		 * glance (a healthy encoder tracking target while the picture
		 * stutters points at the radio, not here). */
		{
			/* See the star6e_runtime equivalent. */
			char osd_ring[16];
			unsigned int lw = venc_ring_low_water_slots(
				&ctx->output.low_water);

			osd_ring[0] = '\0';
			if (lw > 1)
				snprintf(osd_ring, sizeof(osd_ring), " ring%u",
					lw);
			debug_osd_text(ctx->debug_osd, 4, "br", "%u/%uk%s",
				osd_kbps, ctx->cfg.venc_max_rate, osd_ring);
		}

		{
			int osd_row = 5;

			/* AR centre-crop readout: the AR base crop rect (WxH+X+Y),
			 * i.e. the sub-window of the sensor selected to match the
			 * output AR (excludes any active zoom window, which narrows
			 * the real read further).  Shown only when a crop is active
			 * (output AR != sensor AR); confirms the keep_aspect crop. */
			if (ctx->scl_crop_w &&
			    (ctx->scl_crop_w != ctx->sensor.plane.capt.width ||
			     ctx->scl_crop_h != ctx->sensor.plane.capt.height))
				debug_osd_text(ctx->debug_osd, osd_row++, "crop",
					"%ux%u+%u+%u",
					ctx->scl_crop_w, ctx->scl_crop_h,
					ctx->scl_crop_x, ctx->scl_crop_y);

#if HAVE_FRAMING_STAB
			/* Stabilization telemetry: Kalman correction (a) + raw
			 * detector measurement (m), in stab pixels.  "sfil" =
			 * stab-fill (correction applied as the compose shift),
			 * "stab" = HW-crop.  Hidden when no stab thread runs. */
			{
				int sx, sy, mx, my, sp, sf;
				if (maruko_framing_stab_osd_status(&sx, &sy,
				    &mx, &my, &sp, &sf))
					debug_osd_text(ctx->debug_osd, osd_row++,
						sf ? "sfil" : "stab",
						"a%+d%+d m%+d%+d%s",
						sx, sy, mx, my,
						sp ? " paused" : "");
			}
#endif

			if (osd_ae.ae_valid) {
				debug_osd_text(ctx->debug_osd, osd_row++,
					"exp", "%uus sg%u/%u ig%u",
					osd_ae.shutter_us,
					osd_ae.sgain_x1024, osd_ae.max_sgain,
					osd_ae.igain_x1024);
			}
			if (osd_ae.ae_info_valid) {
				debug_osd_text(ctx->debug_osd, osd_row++,
					"ae", "y%u t%u %s %uhz",
					osd_ae.luma_y, osd_ae.scene_target,
					osd_ae.boundary ? "bound" :
					osd_ae.stable ? "stable" : "adj",
					g_inj_run ? g_inj_fps : 0);
			}
			if (osd_ae.awb_valid) {
				debug_osd_text(ctx->debug_osd, osd_row++,
					"awb", "r%u b%u %uk %s",
					osd_ae.rgain, osd_ae.bgain,
					osd_ae.color_temp,
					osd_ae.awb_stable ? "stable" : "adj");
			}
			MarukoIntraRefreshStatus ir;
			MarukoRefPredStatus      rp;
			maruko_pipeline_intra_refresh_status(&ir);
			maruko_pipeline_ref_pred_status(&rp);
			if (ctx->cfg.resilience[0] &&
			    strcmp(ctx->cfg.resilience, "off") != 0) {
				if (rp.active) {
					debug_osd_text(ctx->debug_osd, osd_row++,
						"res", "%s rp=%u/%u",
						ctx->cfg.resilience,
						rp.base, rp.enhance);
				} else {
					debug_osd_text(ctx->debug_osd, osd_row++,
						"res", "%s",
						ctx->cfg.resilience);
				}
			}
			if (ir.active) {
				debug_osd_text(ctx->debug_osd, osd_row++, "intra",
					"%s L%u q%u",
					ir.mode_name, ir.effective_lines_per_p,
					ir.effective_qp);
				debug_osd_text(ctx->debug_osd, osd_row++, "gop",
					"%.2fs %s",
					ir.effective_gop_sec,
					ir.gop_auto ? "auto" : "fixed");
			}

			MarukoZoomStatus zoom;
			maruko_pipeline_zoom_status(&zoom);
			if (zoom.active) {
				debug_osd_text(ctx->debug_osd, osd_row++, "zoom",
					"%u.%02ux %ux%u",
					zoom.level_x100 / 100,
					zoom.level_x100 % 100,
					zoom.output_w, zoom.output_h);
				debug_osd_text(ctx->debug_osd, osd_row++, "crop",
					"%ux%u+%u+%u",
					zoom.crop_w, zoom.crop_h,
					zoom.crop_x, zoom.crop_y);
			}

			{
				const VencConfig *vcfg = maruko_controls_vcfg();
				MarukoDetectSnapshot snap;
				static const uint16_t colors[] = {
					DEBUG_OSD_RED, DEBUG_OSD_GREEN,
					DEBUG_OSD_YELLOW, DEBUG_OSD_CYAN,
					DEBUG_OSD_BLUE, DEBUG_OSD_WHITE,
				};
				int di;

				if (vcfg && vcfg->detect.enabled &&
				    vcfg->detect.osd &&
				    maruko_ipu_yolo_snapshot(ctx, &snap) &&
				    snap.count > 0 && snap.net_w && snap.net_h &&
				    wb_monotonic_us() - snap.produced_us < 700000) {
					for (di = 0; di < snap.count; di++) {
						const DetectBox *b = &snap.boxes[di];
						uint32_t x1 = maruko_osd_box_px(b->x1,
							ctx->cfg.image_width,
							snap.net_w);
						uint32_t y1 = maruko_osd_box_px(b->y1,
							ctx->cfg.image_height,
							snap.net_h);
						uint32_t x2 = maruko_osd_box_px(b->x2,
							ctx->cfg.image_width,
							snap.net_w);
						uint32_t y2 = maruko_osd_box_px(b->y2,
							ctx->cfg.image_height,
							snap.net_h);
						if (x1 >= x2 || y1 >= y2)
							continue;
						debug_osd_rect(ctx->debug_osd,
							(uint16_t)x1, (uint16_t)y1,
							(uint16_t)(x2 - x1),
							(uint16_t)(y2 - y1),
							colors[(unsigned)b->cls %
								(sizeof(colors) /
								sizeof(colors[0]))],
							0);
					}
					debug_osd_text(ctx->debug_osd, osd_row++,
						"det", "%d", snap.count);
				}
			}
		}

		debug_osd_end_frame(ctx->debug_osd);
	}

	rtp_sidecar_poll(&rt->sidecar);

	uint32_t frame_rtp_ts = rt->rtp_state.timestamp;
	uint16_t seq_before = rt->rtp_state.seq;
	uint64_t ready_us = wb_monotonic_us();
	uint64_t capture_us = (stream.count > 0 && stream.packet)
		? stream.packet[0].timestamp : 0;

	int codec = (ctx->cfg.rc_codec == PT_H265) ? 1 : 0;
	uint32_t frame_size = maruko_scene_frame_size(&stream);
	uint8_t is_idr = maruko_scene_is_idr(&stream, codec);
	g_osd_enc_bytes += frame_size;
	scene_update(&ctx->scene, frame_size, is_idr,
		maruko_scene_request_idr, ctx);
	RtpSidecarEncInfo enc_info = {0};
	scene_fill_sidecar(&ctx->scene, &enc_info);
	pipeline_common_rate_watch(&ctx->rate_watch, maruko_controls_vcfg(),
		frame_size, ready_us);

	size_t total_bytes = 0;
	HevcRtpStats frame_pktzr = {0};
	int sidecar_subscribed = rtp_sidecar_is_subscribed(&rt->sidecar);
	if (ctx->output_enabled) {
		/* Observe pressure only when a sidecar probe is subscribed —
		 * see star6e_runtime equivalent.  Always sending: post-encode
		 * skip breaks the H.265 reference chain (HISTORY 0.9.2). */
		if (sidecar_subscribed)
			maruko_output_observe_pressure(&ctx->output);
		total_bytes = maruko_video_send_frame(&stream, &ctx->output,
			&rt->rtp_state, &rt->param_sets, &ctx->cfg,
			MARUKO_PKTZR_VERBOSE_ACTIVE(ctx) ? &frame_pktzr : NULL);
	}

	/* frame-shm ring low-water measurement — see the matching call in
	 * star6e_runtime.c.  Deliberately OUTSIDE the output_enabled block:
	 * gating it there froze the tracker mid-window on disable, so the
	 * header kept publishing the last verdict (a full ring reads as
	 * permanent standing backlog on a ring nobody is writing) and the
	 * first frame after re-enable closed a "200 ms window" built from one
	 * sample.  Star6E reaches its reset branch every frame regardless. */
	maruko_service_ring_low_water(&ctx->output);

	/* Mirror mode: write chn 0 frames to whichever recorder is active
	 * before the stream is released.  In dual mode the chn 1 drain
	 * thread feeds the recorder, so chn 0 must NOT also write —
	 * concurrent writes would interleave NAL units from two encoders.
	 * At most one of recorder / ts_recorder is active (format dispatch
	 * happens at start), so the inactive call is an fd<0 no-op. */
	/* Mirror-mode recording.  The SDK's stream memory dies at the release
	 * below, so the writer thread gets its own copy; the flatten is not an
	 * extra cost for the TS path, which was already flattening onto a
	 * 512 KB stack buffer.  Gated so an idle craft pays no malloc per
	 * frame.
	 *
	 * A recorder that stopped ITSELF is torn down AFTER the release below,
	 * not here — see that site for why. */

	/* The writer pointer IS the gate: it exists for exactly as long as the
	 * recording it feeds.  No recorder-state predicate, so a rotation —
	 * which holds fd == -1 on the writer thread across fdatasync/close/open
	 * — is invisible here and cannot cost a frame. */
	if (!ctx->dual && ctx->rec_writer) {
		size_t au_len = 0;
		int au_idr = 0;
		uint8_t *au = maruko_video_stream_flatten(&stream, &au_len,
			&au_idr);

		if (au) {
			struct timespec rec_now;

			clock_gettime(CLOCK_MONOTONIC, &rec_now);
			(void)venc_rec_writer_push(ctx->rec_writer, au, au_len,
				ts_mux_timespec_to_pts(
					(uint32_t)rec_now.tv_sec,
					(uint32_t)rec_now.tv_nsec),
				au_idr);
		} else {
			/* A frame the flatten refused is a frame missing from
			 * the file; counting it keeps that visible. */
			pthread_mutex_lock(&ctx->rec_writer_lock);
			ctx->rec_flatten_failures++;
			pthread_mutex_unlock(&ctx->rec_writer_lock);
		}
	} else if (!ctx->dual &&
		   !__atomic_load_n(&ctx->rec_reap_pending, __ATOMIC_ACQUIRE)) {
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
		(void)maruko_ts_recorder_write_stream(&ctx->ts_recorder,
			&stream);
		(void)maruko_recorder_write_frame(&ctx->recorder, &stream);
	}

	/* Release the encoder stream immediately after the last consumer
	 * of stream payload (maruko_video_send_frame) so the sidecar emit
	 * and verbose printf below don't hold the VENC output slot.
	 * stream.count is captured locally for the verbose line; sidecar
	 * uses rtp_state only. */
	unsigned int pack_count = stream.count;
	(void)maruko_mi_venc_release_stream(ctx->venc_device,
		ctx->venc_channel, &stream);

	/* SigmaStar refuses StopRecvPic with MI_ERR_VENC_BUSY while an acquired
	 * stream is outstanding.  The helper reads current ring occupancy, so
	 * evaluating immediately after ReleaseStream preserves the same signal
	 * while respecting the SDK lifetime contract. */
	maruko_service_frame_gate(ctx);

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
	 * completing cannot park this loop either. */
	if (!ctx->dual && ctx->rec_writer &&
	    !star6e_record_wants_frame(&ctx->ts_recorder, &ctx->recorder))
		mk_mirror_record_close(ctx, MARUKO_REC_STOP_GRACE_MS);

	/* HTTP record control.  Also called from the gated path in
	 * maruko_pipeline_await_frame(); the flags are drained there so a
	 * command does not wait for a frame while the gate is closed. */
	maruko_service_record_control(ctx);

	if (sidecar_subscribed) {
		RtpSidecarTransportInfo tinfo;
		const RtpSidecarTransportInfo *tinfo_ptr = NULL;
		const VencConfig *vcfg = maruko_controls_vcfg();
		uint8_t detect_buf[RTP_SIDECAR_DGRAM_MAX];
		const void *detect_ptr = NULL;
		uint16_t detect_len = 0;

		/* Producer already cached fill_pct + lifetime stats inside
		 * maruko_output_observe_pressure() above — read the cache
		 * instead of re-querying.  One SIOCOUTQ ioctl / ring-fill
		 * load per frame, not two. */
		if (ctx->output.ring || ctx->output.frame_ring ||
		    ((ctx->output.transport == VENC_OUTPUT_URI_UNIX ||
		      ctx->output.transport == VENC_OUTPUT_URI_UDP) &&
		     ctx->output.socket_handle >= 0)) {
			memset(&tinfo, 0, sizeof(tinfo));
			tinfo.fill_pct = ctx->output.last_fill_pct;
			tinfo.in_pressure = ctx->output.in_pressure ? 1 : 0;
			tinfo.pressure_drops = ctx->output.pressure_drops;
			tinfo.transport_drops = ctx->output.last_full_drops;
			tinfo.packets_sent = ctx->output.last_writes;
			tinfo_ptr = &tinfo;
		}
		if (vcfg && vcfg->detect.enabled) {
			MarukoDetectSnapshot snap;
			if (maruko_ipu_yolo_snapshot(ctx, &snap)) {
				uint64_t now_us = wb_monotonic_us();
				uint64_t age_ms = now_us > snap.produced_us ?
					(now_us - snap.produced_us) / 1000 : 0;
				size_t len = detect_wire_build(detect_buf,
					sizeof(detect_buf), snap.boxes, snap.count,
					snap.model_id, snap.seq,
					age_ms > UINT16_MAX ? UINT16_MAX :
						(uint16_t)age_ms,
					snap.net_w, snap.net_h,
					sizeof(detect_buf));
				if (len > 0) {
					detect_ptr = detect_buf;
					detect_len = (uint16_t)len;
				}
			}
		}
		rtp_sidecar_send_frame_detect(&rt->sidecar,
			rt->rtp_state.ssrc, frame_rtp_ts, seq_before,
			(uint16_t)(rt->rtp_state.seq - seq_before),
			capture_us, ready_us, &enc_info, tinfo_ptr, NULL,
			detect_ptr, detect_len);
	}

	if (ctx->cfg.verbose)
		maruko_pipeline_log_verbose_frame(ctx, rt, total_bytes,
			&frame_pktzr, pack_count);

	return 0;
}

int maruko_pipeline_run(MarukoBackendContext *ctx)
{
	MarukoStreamRuntime rt;
	int result = -1;

	if (!ctx)
		return -1;

	/* Running with NO output is a supported state, deliberately.
	 *
	 * This guard used to refuse it, which defeated the whole point of coming
	 * up after a destination that could not be brought up: the pipeline
	 * started, the API came up, and then this returned -1 and the daemon
	 * exited anyway -- measured on the bench, which is the only reason this
	 * was found.  Star6E and CV610 have no equivalent precondition.
	 *
	 * Safe because every path that touches the socket already guards a
	 * negative handle: maruko_output_begin_frame() marks the batch inactive,
	 * and output_socket_send_parts() refuses with EINVAL, so frames are
	 * counted as drops.  The operator gets an HTTP-reachable craft and can
	 * fix outgoing.server live. */
	if (ctx->output.socket_handle < 0 && !ctx->output.ring &&
	    !ctx->output.frame_ring)
		fprintf(stderr, "WARNING: [maruko] running with no video output; "
			"set outgoing.server over the API to bring one up\n");

	if (ctx->cfg.stream_mode == MARUKO_STREAM_RTP &&
	    ctx->cfg.rc_codec != PT_H265) {
		fprintf(stderr,
			"ERROR: [maruko] RTP output mode requires codec=h265;"
			" rc_codec=%d not supported — aborting pipeline\n",
			(int)ctx->cfg.rc_codec);
		return -1;
	}

	if (ctx->cfg.stream_mode == MARUKO_STREAM_RTP)
		printf("> [maruko] RTP packetizer enabled\n");
	printf("> [maruko] entering stream loop\n");

	maruko_pipeline_init_streaming(ctx, &rt);

	while (g_maruko_running) {
		i6c_venc_stat stat;
		int rc;

		if (g_maruko_reinit || venc_api_get_reinit()) {
			g_maruko_reinit = 0;
			printf("> [maruko] reinit requested, breaking stream loop\n");
			result = 1;
			goto cleanup;
		}
		maruko_controls_service_detect_reload();

		rc = maruko_pipeline_await_frame(ctx, &rt, &stat);
		if (rc < 0)
			goto cleanup;
		if (rc == 0)
			continue;

		rc = maruko_pipeline_process_stream(ctx, &rt, &stat);
		if (rc < 0)
			goto cleanup;
	}
	result = 0;

cleanup:
	maruko_pipeline_cleanup_streaming(ctx, &rt);
	return result;
}

/* Teardown watchdog.  The i6c MI teardown can wedge in an UNBOUNDED kernel
 * flush (MI_SYS_IMPL_FlushRealTimeOutputBuf, uninterruptible D-state) when an
 * SCL/ISP working task is pinned at unbind time — a long-standing SDK race
 * (first logged 2026-07-03; reproduced from stab, stab-fill AND the factory
 * binary on 2026-07-09).  The stab two-phase drain removes the dominant
 * trigger, but the residual race cannot be closed from userspace ordering
 * alone.  A wedged teardown is unrecoverable (no sysrq on I6C; SIGKILL leaves
 * an MI zombie): the only outcome is a silent dead encoder until the HARDWARE
 * watchdog eventually resets.  So: arm a deadline at teardown entry — if
 * teardown hasn't completed in time, log and reboot(RB_AUTOBOOT) for a
 * bounded, deterministic recovery instead of open-ended limbo. */
#define MARUKO_TEARDOWN_WATCHDOG_S 12
static volatile int g_teardown_done;

static void *maruko_teardown_watchdog_main(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < MARUKO_TEARDOWN_WATCHDOG_S * 10; i++) {
		if (g_teardown_done)
			return NULL;
		usleep(100000);
	}
	fprintf(stderr, "ERROR: [maruko] teardown wedged for %ds (MI flush "
		"D-state) — forcing reboot for deterministic recovery\n",
		MARUKO_TEARDOWN_WATCHDOG_S);
	sync();
	(void)reboot(RB_AUTOBOOT);
	return NULL;
}

void maruko_pipeline_teardown_graph(MarukoBackendContext *ctx)
{
	pthread_t wdt;
	int wdt_armed;

	if (!ctx)
		return;

	g_teardown_done = 0;
	wdt_armed = pthread_create(&wdt, NULL,
		maruko_teardown_watchdog_main, NULL) == 0;

	/* Stop the framing module FIRST: it joins its detector thread and
	 * disables its SCL tap port before we tear down port0 / the SCL channel
	 * below (R6 — never disable the tapped port with the reader still live). */
	maruko_pipeline_framing_stop();
	/* Port 3 follows Maruko's drain-while-disable rule and must be gone
	 * before SCL channel teardown. */
	maruko_ipu_yolo_stop(ctx);

	venc_api_clear_active_precrop();
	maruko_pipeline_clear_zoom_status();
	maruko_pan_ramp_stop();
	/* Cycle AE-crop state so the next pipeline_start re-arms via the
	 * ISP-ready hook and re-emits any cached sub-rect.  Star6E parity. */
	g_maruko_ae_crop_ready = 0;
	g_maruko_ae_crop_disabled = 0;
	g_maruko_ae_crop_last.crop_x = 0;
	g_maruko_ae_crop_last.crop_y = 0;
	g_maruko_ae_crop_last.crop_w = 1023;
	g_maruko_ae_crop_last.crop_h = 1023;
	/* Drop the cached ISP bin path: the next configure_graph cold-boot
	 * load must run unconditionally (kernel ISP state was destroyed by
	 * the teardown sequence below).  Star6E gets this for free via the
	 * fork+exec process boundary; Maruko reinit is in-process so we
	 * have to do it explicitly. */
	g_last_isp_bin_path[0] = '\0';
	/* Stop dual VENC FIRST — its thread reads from chn 1's fd and
	 * the binding shares the VPE port with chn 0.  Tearing down chn 0
	 * before unbinding chn 1 wedges the SDK on the next reinit. */
	maruko_pipeline_stop_dual(ctx);
	/* Recorders run after stop_dual: in dual mode the drain thread
	 * has already exited and is no longer issuing writes, so close()
	 * cannot race with a write().  In mirror mode the recorder writer
	 * thread issues them, which is what the stop below joins.
	 * Stop both — at most one is open, the inactive call is a no-op. */
	/* The join inside this is what makes the close safe; bounded because
	 * this teardown is watchdogged. */
	mk_mirror_record_close_mode(ctx, MARUKO_REC_TEARDOWN_GRACE_MS, 0);
	if (ctx->rec_locks_ready) {
		pthread_mutex_destroy(&ctx->rec_writer_lock);
		ctx->rec_locks_ready = 0;
	}
	/* Audio teardown after recorder stop: the recorder pops from
	 * audio_recorder_ring, so the recorder must be quiet before we
	 * can destroy the ring or join the encode thread that pushes
	 * into it.  Audio still pushes into MarukoOutput, so this must
	 * also precede maruko_output_teardown. */
	{
		int audio_was_started = ctx->audio.started;
		ctx->audio.rec_ring = NULL;
		maruko_audio_teardown(&ctx->audio);
		if (audio_was_started) {
			audio_ring_destroy(&ctx->audio_recorder_ring);
			memset(&ctx->audio_recorder_ring, 0,
				sizeof(ctx->audio_recorder_ring));
		}
	}
	/* IMU stop+destroy next — push callback is a stub so order vs
	 * other teardown steps doesn't matter, but keeping the
	 * stop-then-destroy split lets a future telemetry consumer slot
	 * in without rework (Star6E parity). */
	if (ctx->imu) {
		imu_stop(ctx->imu);
		imu_destroy(ctx->imu);
		ctx->imu = NULL;
	}
	if (ctx->debug_osd) {
		debug_osd_destroy(ctx->debug_osd);
		ctx->debug_osd = NULL;
	}
	maruko_output_teardown(&ctx->output);
	/* Tear down JPEG snapshot subsystem first — its MJPG VENC channel
	 * is bound to the SCL port we're about to disable.  Idempotent. */
	venc_jpeg_shutdown();
	/* Both tap claimants are down by now (detector stopped above, snapshot
	 * subsystem just shut down), so drop any lingering claim — a reinit must
	 * not inherit a tap owner from the previous graph. */
	maruko_scl_tap_reset();
	if (g_mi_scl_port1_enabled) {
		(void)g_mi_scl.fnDisablePort(0, 0, 1);
		g_mi_scl_port1_enabled = 0;
	}

	/* MI teardown order mirrors Star6E (proven over many reinit
	 * cycles).  The critical invariant: each consumer must be
	 * stopped — or at least StopRecvPic'd — BEFORE its input port
	 * is unbound.  The previous order tore down every UnBind first
	 * and only then stopped VENC/VPE/VIF; this triggered a page
	 * fault inside MI_SYS_IMPL_FlushInputPortTasks because the
	 * kernel SDK was still flushing buffered frames into a port the
	 * userspace side had just ripped out.  Empirically zombie'd the
	 * process on ~14% of resilience reinits (S1 bench 2026-05-15).
	 *
	 * Sequence:
	 *   0. Drain the SCL→VENC RING leg while the encoder still
	 *      consumes, then stop SCL output production.
	 *   1. VENC StopRecvPic (soft pause, lets buffered frames flow
	 *      out one last time).
	 *   2. Unbind VPE→VENC.
	 *   3. Destroy VENC channel.
	 *   4. Stop VPE channels (drains VPE's own buffer queue).
	 *   5. Unbind ISP→VPE.
	 *   6. Stop VIF.
	 *   7. Unbind VIF→ISP.
	 *   8. Sensor disable. */

	/* Step 0: drain BEFORE StopRecvPic.  Once VENC stops receiving,
	 * in-flight SCL→VENC output tasks can never complete, and the
	 * VPE→VENC unbind's kernel flush then spins forever in
	 * uninterruptible D-state (MI_SYS_IMPL_FlushRealTimeOutputBuf —
	 * observed twice, both after long streams where the queue runs
	 * deeper; SCL proc showed workingTask_cnt=4 stuck).  So drain
	 * while the encoder is still consuming, then disable the SCL
	 * output port so nothing new enters the leg before the
	 * pause+unbind. */
	if (g_mi_scl_chn_created) {
		(void)maruko_wait_output_idle(MARUKO_PROC_SCL, 200, 1);
		(void)g_mi_scl.fnDisablePort(0, 0, 0);
		(void)maruko_wait_output_idle(MARUKO_PROC_SCL, 100, 1);
	}
#if HAVE_FRAMING_STAB
	/* stab-fill teardown phase 2: the drain-only fill thread (armed by
	 * maruko_pipeline_framing_stop above) kept consuming port0 WHILE the
	 * port was disabled, so the SCL/ISP working queues could quiesce — an
	 * unconsumed port0 pins them and wedges the ISP→SCL REALTIME unbind
	 * flush in unbounded D-state (MI_SYS_IMPL_FlushRealTimeOutputBuf →
	 * zombie or watchdog reset; both device-reproduced on reinit switches,
	 * 2026-07-09).  Join it and sweep the residue now.  No-op unless the
	 * fill module armed phase 1. */
	maruko_framing_stab_finish_stop();
#endif

	/* stab-fill: the VENC's only producer (the fill compose thread) was
	 * joined by maruko_pipeline_framing_stop above, so the manual-feed leg is
	 * already quiescent; there is no SCL→VENC bind to unwind. */

	if (ctx->venc_started)
		(void)maruko_mi_venc_stop_recv(ctx->venc_device,
			ctx->venc_channel);

	if (ctx->bound_vpe_venc) {
		(void)MI_SYS_UnBindChnPort(&ctx->vpe_port, &ctx->venc_port);
		ctx->bound_vpe_venc = 0;
	}

	if (ctx->venc_started) {
		(void)maruko_mi_venc_destroy_chn(ctx->venc_device,
			ctx->venc_channel);
		if (ctx->venc_dev_created)
			(void)maruko_mi_venc_destroy_dev(ctx->venc_device);
		ctx->venc_started = 0;
		ctx->venc_dev_created = 0;

		/* Clear IntraRefresh status — the channel is gone. */
		pthread_mutex_lock(&g_intra_status_mutex);
		memset(&g_intra_status, 0, sizeof(g_intra_status));
		pthread_mutex_unlock(&g_intra_status_mutex);
		maruko_pipeline_clear_zoom_status();
	}


	if (ctx->vpe_started) {
		maruko_stop_vpe_channels();
		ctx->vpe_started = 0;
	}

	if (ctx->bound_isp_vpe) {
		/* Belt to the producer-first stop order in
		 * maruko_stop_vpe_channels(): only enter the kernel's
		 * unbounded REALTIME flush once the ISP output port reports
		 * no in-flight tasks.  Bounded — proceeds on timeout. */
		(void)maruko_wait_output_idle(MARUKO_PROC_ISP, 200, 0);
		(void)MI_SYS_UnBindChnPort(&ctx->isp_port, &ctx->vpe_port);
		ctx->bound_isp_vpe = 0;
	}

	if (ctx->vif_started) {
		maruko_stop_vif();
		ctx->vif_started = 0;
	}

	if (ctx->bound_vif_vpe) {
		(void)MI_SYS_UnBindChnPort(&ctx->vif_port, &ctx->isp_port);
		ctx->bound_vif_vpe = 0;
	}

	if (ctx->sensor_enabled) {
		(void)MI_SNR_Disable(ctx->sensor.pad_id);
		ctx->sensor_enabled = 0;
	}

	/* Teardown completed within the deadline — disarm the watchdog. */
	g_teardown_done = 1;
	if (wdt_armed)
		(void)pthread_join(wdt, NULL);
}

void maruko_pipeline_teardown(MarukoBackendContext *ctx)
{
	/* Pause first: venc_httpd_stop() only detaches the worker thread, so
	 * a request accepted before the listen socket closed may still be in
	 * dispatch.  pause() drains it and gates any further dispatch with
	 * 503 — by the time it returns, no handler is touching SDK state. */
	venc_httpd_pause();
	venc_httpd_stop();
	maruko_ae_pacer_stop();
	maruko_cus3a_stop();
	maruko_pipeline_teardown_graph(ctx);
	if (ctx && ctx->system_initialized) {
		(void)MI_SYS_Exit();
		ctx->system_initialized = 0;
		printf("> [maruko] stage teardown: MI_SYS_Exit\n");
	}
	maruko_mi_deinit();
	/* No resume: the process is exiting (or about to). */
}
