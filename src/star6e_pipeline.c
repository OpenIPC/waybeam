#include "star6e_pipeline.h"
#include "star6e_framing.h"
#include "star6e_ipu_yolo.h"
#include "star6e_vpe_ports.h"
#include "star6e_luma_tap.h"
#include "star6e_awb.h"
#include "star6e_framing_host.h"
#if HAVE_FRAMING_STAB
#include "star6e_framing_stab.h"
#endif

#include "codec_config.h"
#include "codec_types.h"
#include "debug_osd.h"
#include "imu_ring.h"
#include "star6e_controls.h"
#include "star6e_cus3a.h"
#include "file_util.h"
#include "intra_refresh.h"
#include "isp_runtime.h"
#include "pipeline_common.h"
#include "venc_api.h"
#include "venc_jpeg.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct {
	unsigned int minShutterUs;
	unsigned int maxShutterUs;
	unsigned int minApertX10;
	unsigned int maxApertX10;
	unsigned int minSensorGain;
	unsigned int minIspGain;
	unsigned int maxSensorGain;
	unsigned int maxIspGain;
} IspExposureLimit;

typedef int (*isp_get_exposure_limit_fn_t)(int channel,
	IspExposureLimit *config);
typedef int (*isp_set_exposure_limit_fn_t)(int channel,
	IspExposureLimit *config);

typedef int (*isp_load_bin_fn_t)(int channel, char *path, unsigned int key);
typedef int (*isp_disable_userspace3a_fn_t)(int channel);
typedef int (*cus3a_fn_t)(int channel, void *params);

/* ── Framing-module registry ──────────────────────────────────────────────
 * Compiled-in framing modules (e.g. the "stab" preset) self-register here via
 * star6e_framing_register_builtins().  g_framing is the module selected for
 * the current run (NULL when framing is off / a plain zoom or full-frame
 * bind), set in star6e_pipeline_start and used through bind/stop/zoom. */
#define FRAMING_MAX 4
static const FramingModule *g_framing_registry[FRAMING_MAX];
static int g_framing_count;
static const FramingModule *g_framing;

void star6e_framing_register(const FramingModule *m)
{
	if (m && g_framing_count < FRAMING_MAX)
		g_framing_registry[g_framing_count++] = m;
}

const FramingModule *star6e_framing_select(const VencConfig *vcfg)
{
	int i;
	for (i = 0; i < g_framing_count; i++)
		if (g_framing_registry[i]->enabled(vcfg))
			return g_framing_registry[i];
	return NULL;
}

void star6e_framing_register_builtins(void)
{
	if (g_framing_count > 0)
		return;   /* idempotent */
#if HAVE_FRAMING_STAB
	star6e_framing_register(&star6e_framing_stab);
	star6e_framing_register(&star6e_framing_stab_fill);
#endif
}

/* Bring the IPU object-detection tap (VPE port1) up under the arbiter.  Two
 * ways it can be refused:
 *   - stab holds port1 (the arbiter claim fails), or
 *   - stab-fill is active: it drains port0 (no port1 conflict), but its SW
 *     compose + blit plus the IPU detector together exceed the validated
 *     Star6E budget, so they stay mutually exclusive by policy.
 * Best-effort (never fatal to the stream); on failure the port1 claim is
 * released so runtime.vpe_taps reads free.  Returns 0 iff detection is
 * running on return.
 *
 * Shared by the initial bring-up and the live `detect.enabled` toggle, so the
 * refusal policy cannot drift between them.  Must be called on the pipeline
 * thread — it starts the detector whose snapshot that thread reads.
 */
int star6e_pipeline_detect_start(Star6ePipelineState *state,
	const VencConfig *vcfg)
{
	const char *blocker = NULL;

	if (!state || !vcfg)
		return -1;
	if (state->detect)
		return 0;   /* already running */

	if (g_framing && !g_framing->uses_vpe_port1)
		blocker = g_framing->preset_name;   /* stab-fill: resource policy */
	else if (star6e_vpe_port1_claim("detect") != 0)
		blocker = star6e_vpe_port1_owner();  /* stab owns the tap */

	if (blocker) {
		fprintf(stderr, "[waybeam] detect: skipped — framing '%s' active "
			"(VPE port1 owner / resource policy)\n", blocker);
		return -1;
	}

	star6e_ipu_yolo_start(state, vcfg);
	if (!state->detect) {
		star6e_vpe_port1_release("detect");
		return -1;
	}
	return 0;
}

/* Tear the detector down and hand port1 back.  Idempotent.  Same threading
 * rule as _detect_start. */
void star6e_pipeline_detect_stop(Star6ePipelineState *state)
{
	if (!state || !state->detect)
		return;
	star6e_ipu_yolo_stop(state);
	star6e_vpe_port1_release("detect");
}

static void star6e_pipeline_reset(Star6ePipelineState *state)
{
	struct Star6eLumaTap *luma_tap;

	if (!state)
		return;

	luma_tap = state->luma_tap;
	star6e_video_reset(&state->video);
	memset(state, 0, sizeof(*state));
	state->luma_tap = luma_tap;
	star6e_output_reset(&state->output);
}

/* VPE SCL clock workaround — written at shutdown, effective on next start.
 *
 * Root cause: at process exit, mi_vpe_process_exit → MI_VPE_IMPL_DeInit →
 * DrvSclModuleClkDeInit disables the VPE SCL clock (fclk1) via
 * clk_disable_unprepare. On the next run, MI_VPE_IMPL_Init has a persistent
 * kernel-side "already_inited" flag that causes it to skip
 * DrvSclModuleClkInit, so fclk1 is never re-enabled.
 *
 * Writing after MI_SYS_Exit() triggers the preset path while the VPE fd is
 * closed. The preset persists in kernel memory until the next init. */
void star6e_pipeline_vpe_scl_preset_shutdown(void)
{
	int fd = open("/sys/devices/virtual/mstar/mscl/clk", O_WRONLY);

	if (fd < 0)
		return;

	(void)write(fd, "384000000\n", 10);
	close(fd);
	(void)write(STDERR_FILENO, "[waybeam] VPE SCL preset stored for next run\n",
		42);
}

/* Called after MI_SYS_Init() to silently tear down any pipeline state left
 * by an unclean previous exit. All errors are ignored.
 *
 * When MI libs are loaded via dlopen (no direct linking), the vendor VPE
 * module calls exit(127) on API calls before a channel is created. Guard
 * VPE teardown with a channel existence check to avoid this. */
static void star6e_pipeline_pre_init_teardown(void)
{
	MI_SYS_ChnPort_t vif_port = {
		.module = I6_SYS_MOD_VIF, .device = 0, .channel = 0, .port = 0 };
	MI_SYS_ChnPort_t vpe_port = {
		.module = I6_SYS_MOD_VPE, .device = 0, .channel = 0, .port = 0 };
	MI_SYS_ChnPort_t venc_port = {
		.module = I6_SYS_MOD_VENC, .device = 0, .channel = 0, .port = 0 };

	(void)MI_SYS_UnBindChnPort(&vpe_port, &venc_port);
	(void)MI_SYS_UnBindChnPort(&vif_port, &vpe_port);
	(void)MI_VENC_StopRecvPic(0);
	(void)MI_VENC_DestroyChn(0);

	/* VPE: probe channel existence before teardown — MI_VPE_DisablePort
	 * calls exit(127) when called on a non-existent channel under dlopen. */
	MI_VPE_ChannelAttr_t probe_attr;
	if (MI_VPE_GetChannelAttr(0, &probe_attr) == 0) {
		/* port1 too, not just port0.  port1 is the optional second-scaler
		 * tap (detect / stab), so an unclean exit can leave it enabled and
		 * the teardown below would then run with a live port still
		 * registered.  Hygiene, not the fix for the detect-off respawn
		 * wedge — on that path port1 was verified already disabled.
		 * Reached only when the probe says the channel exists, so this
		 * cannot trip the dlopen exit(127) path. */
		MI_S32 p1 = MI_VPE_DisablePort(0, 1);
		if (p1 == 0)
			fprintf(stderr, "[waybeam] pre-init: stale VPE port1 was "
				"enabled — disabled\n");
		(void)MI_VPE_DisablePort(0, 0);
		(void)MI_VPE_StopChannel(0);
		(void)MI_VPE_DestroyChannel(0);
	}

	(void)MI_VIF_DisableChnPort(0, 0);
	(void)MI_VIF_DisableDev(0);
}

static int star6e_pipeline_disable_userspace3a(const IspRuntimeLib *lib,
	void *ctx)
{
	isp_disable_userspace3a_fn_t fn;

	(void)ctx;
	fn = (isp_disable_userspace3a_fn_t)lib->disable_userspace3a;
	return fn ? fn(0) : 0;
}

/* Forward decl — definition lives alongside the zoom/pan block but is
 * referenced from the ISP-ready waiters above it. */
static void star6e_ae_crop_mark_ready(void);

/* Poll MI_ISP_IQ_GetParaInitStatus until bFlag==1 or timeout (2000 ms).
 * Called standalone after VIF→VPE bind when a new VPE channel was just
 * created (first start or AR-change reinit): the ISP channel initialises
 * asynchronously after MI_VPE_CreateChannel returns, so anything that
 * touches the ISP (bin load, exposure cap) must wait here first.
 * Without this poll the ISP would emit "IspApiGet channel not created"
 * kernel errors on the first probe attempt. */
static void star6e_pipeline_wait_isp_channel(void)
{
	typedef struct { int bFlag; } IspParaInitInfoParam;
	typedef struct { IspParaInitInfoParam stParaAPI; } IspParaInitInfoType;
	typedef int (*fn_get_para_init_t)(int, IspParaInitInfoType *);
	fn_get_para_init_t fn;
	void *handle;
	int elapsed_ms = 0;

	handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!handle) {
		usleep(100 * 1000);
		return;
	}

	fn = (fn_get_para_init_t)dlsym(handle, "MI_ISP_IQ_GetParaInitStatus");
	if (!fn) {
		usleep(100 * 1000);
		dlclose(handle);
		return;
	}

	while (elapsed_ms < 2000) {
		IspParaInitInfoType info;

		memset(&info, 0, sizeof(info));
		if (fn(0, &info) == 0 && info.stParaAPI.bFlag == 1) {
			printf("> ISP channel ready after %d ms\n", elapsed_ms);
			dlclose(handle);
			star6e_ae_crop_mark_ready();
			return;
		}
		usleep(1000);
		elapsed_ms++;
	}

	fprintf(stderr, "WARNING: ISP channel readiness timeout after 2000 ms\n");
	dlclose(handle);
}

static int star6e_pipeline_wait_isp_ready(const IspRuntimeLib *lib, void *ctx)
{
	typedef struct { int bFlag; } IspParaInitInfoParam;
	typedef struct { IspParaInitInfoParam stParaAPI; } IspParaInitInfoType;
	typedef int (*fn_get_para_init_t)(int, IspParaInitInfoType *);
	fn_get_para_init_t fn;
	int elapsed_ms = 0;

	(void)ctx;
	fn = (fn_get_para_init_t)dlsym(lib->handle,
		"MI_ISP_IQ_GetParaInitStatus");
	if (!fn) {
		/* Symbol not available — fall back to fixed delay */
		usleep(100 * 1000);
		return 0;
	}

	while (elapsed_ms < 2000) {
		IspParaInitInfoType info;

		memset(&info, 0, sizeof(info));
		if (fn(0, &info) == 0 && info.stParaAPI.bFlag == 1) {
			printf("> ISP ready after %d ms\n", elapsed_ms);
			star6e_ae_crop_mark_ready();
			return 0;
		}
		usleep(1000);
		elapsed_ms++;
	}

	fprintf(stderr, "WARNING: ISP readiness timeout after 2000 ms\n");
	return -1;
}

static int star6e_pipeline_call_load_bin(const IspRuntimeLib *lib,
	const char *path, unsigned int load_key, void *ctx)
{
	isp_load_bin_fn_t fn_api;
	isp_load_bin_fn_t fn_api_alt;
	int ret;

	(void)ctx;
	fn_api = (isp_load_bin_fn_t)lib->load_bin_api;
	fn_api_alt = (isp_load_bin_fn_t)lib->load_bin_api_alt;
	ret = -1;
	if (fn_api)
		ret = fn_api(0, (char *)path, load_key);
	if (ret != 0 && fn_api_alt && fn_api_alt != fn_api)
		ret = fn_api_alt(0, (char *)path, load_key);
	return ret;
}

/* MI_ISP_CUS3A_Enable(MI_U32 Channel, Cus3AEnable_t *data).  Each flag selects
 * who OWNS that module: 1 = custom/userspace code, 0 = the ISP-internal
 * algorithm.  It is not an "enable the algorithm" switch — setting bAWB=1
 * without supplying an AWB implementation leaves AWB driven by nothing (and
 * makes ct_manual inert), which is exactly why this is paired with the
 * star6e_awb loop.
 *
 * MI_BOOL is `unsigned char` on i6e, so the struct is THREE BYTES.  The call
 * previously passed `int p110[13] = {1,1,0}`, whose leading bytes
 * 01 00 00 00 01 00 00 00 read as bAE=1, bAWB=0, bAF=0 — the AWB flag landed
 * in padding.  That was harmless while nothing wanted userspace AWB, but the
 * flag has to mean what it says now.  Maruko declares MI_BOOL[3] and was
 * always correct.  Layout from the i6e SDK header (mi_isp_datatype.h). */
typedef struct {
	unsigned char bAE;
	unsigned char bAWB;
	unsigned char bAF;
} Star6eCus3AEnable;

static void star6e_pipeline_post_load_cus3a(const IspRuntimeLib *lib,
	void *ctx)
{
	cus3a_fn_t fn_cus3a;
	Star6eCus3AEnable ae_only  = {1, 0, 0};
	Star6eCus3AEnable ae_awb   = {1, 1, 0};

	(void)ctx;
	fn_cus3a = (cus3a_fn_t)lib->cus3a_enable;
	if (!fn_cus3a)
		return;

	fn_cus3a(0, &ae_only);
	fn_cus3a(0, &ae_awb);
}

static int star6e_pipeline_load_isp_bin(const char *isp_bin_path,
	SdkQuietState *sdk_quiet)
{
	IspRuntimeLoadHooks hooks;

	if (!isp_bin_path || !*isp_bin_path)
		return 0;

	memset(&hooks, 0, sizeof(hooks));
	hooks.load_key = 1234;
	hooks.ctx = sdk_quiet;
	hooks.quiet_begin = (void (*)(void *))sdk_quiet_begin;
	hooks.quiet_end = (void (*)(void *))sdk_quiet_end;
	hooks.disable_userspace3a = star6e_pipeline_disable_userspace3a;
	hooks.wait_ready = star6e_pipeline_wait_isp_ready;
	hooks.load_bin = star6e_pipeline_call_load_bin;
	hooks.post_load = star6e_pipeline_post_load_cus3a;

	return isp_runtime_load_bin_file(isp_bin_path, &hooks);
}


static void star6e_pipeline_enable_cus3a(SdkQuietState *sdk_quiet)
{
	typedef int (*cus3a_fn_t)(int channel, void *params);
	void *handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	cus3a_fn_t fn;

	if (!handle)
		return;

	fn = (cus3a_fn_t)dlsym(handle, "MI_ISP_CUS3A_Enable");
	if (fn) {
		Star6eCus3AEnable ae_only = {1, 0, 0};
		Star6eCus3AEnable ae_awb  = {1, 1, 0};
		MI_S32 ret;

		sdk_quiet_begin(sdk_quiet);
		fn(0, &ae_only);
		ret = fn(0, &ae_awb);
		sdk_quiet_end(sdk_quiet);
		if (ret != 0)
			fprintf(stderr, "WARNING: MI_ISP_CUS3A_Enable(AE+AWB) failed %d\n", ret);
	}

	dlclose(handle);
}

/* Reachable from two threads — the pipeline thread's handoff tick and the HTTP
 * thread's isp.awbMode change — so the symbol lookup is done under pthread_once
 * rather than the usual lazy-static idiom, which would race on first use. */
static void *g_cus3a_lib;
static int (*g_cus3a_fn)(int channel, void *params);
static pthread_once_t g_cus3a_once = PTHREAD_ONCE_INIT;

static void star6e_cus3a_bind_once(void)
{
	g_cus3a_lib = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (g_cus3a_lib)
		g_cus3a_fn = (int (*)(int, void *))dlsym(g_cus3a_lib,
			"MI_ISP_CUS3A_Enable");
}

static void star6e_pipeline_cus3a_apply(SdkQuietState *sdk_quiet,
	Star6eCus3AEnable *params)
{
	int (*fn)(int, void *);

	pthread_once(&g_cus3a_once, star6e_cus3a_bind_once);
	fn = g_cus3a_fn;
	if (!fn)
		return;

	sdk_quiet_begin(sdk_quiet);
	fn(0, params);
	sdk_quiet_end(sdk_quiet);
}

static int g_cus3a_handoff_done = 0;

void star6e_pipeline_cus3a_reset(void)
{
	g_cus3a_handoff_done = 0;
}

/* Delayed cold-boot fps re-kick.  The pipeline-init MI_SNR_SetFps
 * (bind_and_finalize_pipeline) fires before the ISP bin's AE has settled, so on
 * a cold boot the sensor's physical timing register can be left below the
 * configured fps (observed ~70fps @ target 90; a warm restart keeps the kernel
 * sensor state so it shows ~90).  The supervisory AE enforcer only touches the
 * ISP exposure limit, not the sensor timing register, so re-issue SetFps once
 * from the run loop ~1.5s after start, after the bin load + AE converge, to
 * force the sensor register to the target.  No-op when fps is 0. */
void star6e_pipeline_cold_boot_fps_rekick(const Star6ePipelineState *state,
	const VencConfig *vcfg)
{
	if (!state || !vcfg)
		return;
	if (state->sensor.fps == 0)
		return;
	printf("[waybeam] cold-boot fps re-kick: SetFps(%u)\n",
		state->sensor.fps);
	fflush(stdout);
	MI_SNR_SetFps(state->sensor.pad_id, state->sensor.fps);
}

/* Which algorithm owns AWB.  1 = handed to userspace (our star6e_awb loop
 * drives it); 0 = ISP-internal, which is what isp.awbMode=ct_manual needs so
 * the existing MI_ISP_AWB_SetCTMwbAttr apply path still reaches the hardware.
 * Read by the handoff below and re-applied live on a mode change. */
static volatile int g_awb_userspace = 0;

void star6e_pipeline_set_awb_userspace(int on)
{
	Star6eCus3AEnable en = {0, 0, 0};

	g_awb_userspace = on ? 1 : 0;
	en.bAWB = (unsigned char)g_awb_userspace;
	/* NULL sdk_quiet: this runs off the HTTP thread, where suppressing
	 * stdio would race the pipeline thread's own quiet windows. */
	if (g_cus3a_handoff_done)
		star6e_pipeline_cus3a_apply(NULL, &en);
}

void star6e_pipeline_cus3a_tick(SdkQuietState *sdk_quiet,
	struct timespec *ts_last)
{
	struct timespec now;
	long long elapsed_ms;
	/* Hand AE back to the ISP-internal algorithm (as before — that is what
	 * keeps AE cheap at high frame rates) but KEEP AWB in the CUS3A engine,
	 * otherwise it stops converging the moment this fires and the boot-time
	 * cast is frozen in for the life of the process. */
	Star6eCus3AEnable handoff = {0, 0, 0};

	handoff.bAWB = (unsigned char)g_awb_userspace;

	if (g_cus3a_handoff_done || !ts_last)
		return;

	clock_gettime(CLOCK_MONOTONIC, &now);
	elapsed_ms =
		((long long)(now.tv_sec - ts_last->tv_sec) * 1000LL) +
		((long long)(now.tv_nsec - ts_last->tv_nsec) / 1000000LL);
	if (elapsed_ms < 1000)
		return;

	star6e_pipeline_cus3a_apply(sdk_quiet, &handoff);
	g_cus3a_handoff_done = 1;
	/* Orientation (image.flip/mirror) is applied once at bring-up and holds
	 * across this CUS3A handoff — device-verified on IMX335, so no re-apply
	 * is needed here. */
}

int star6e_pipeline_cap_exposure_for_fps(uint32_t fps,
	bool shutter_rule_180)
{
	return pipeline_common_cap_exposure_for_fps(fps, shutter_rule_180);
}

static void star6e_pipeline_stop_sensor(MI_SNR_PAD_ID_e pad_id)
{
	MI_SNR_Disable(pad_id);
}

static Star6ePrecropRect star6e_pipeline_compute_precrop(uint32_t sensor_w,
	uint32_t sensor_h, uint32_t image_w, uint32_t image_h,
	bool keep_aspect)
{
	PipelinePrecropRect common = pipeline_common_compute_precrop(
		sensor_w, sensor_h, image_w, image_h, keep_aspect);
	Star6ePrecropRect rect = {common.x, common.y, common.w, common.h};
	return rect;
}

static int star6e_pipeline_start_vif(const SensorSelectResult *sensor,
	const Star6ePrecropRect *precrop)
{
	MI_VIF_DevAttr_t dev = {0};
	MI_VIF_PortAttr_t port = {0};
	MI_S32 ret;

	dev.intf = sensor->pad.intf;
	dev.work = (sensor->pad.intf == I6_INTF_BT656) ? I6_VIF_WORK_1MULTIPLEX :
		I6_VIF_WORK_RGB_REALTIME;
	dev.hdr = I6_HDR_OFF;

	if (sensor->pad.intf == I6_INTF_MIPI) {
		dev.edge = I6_EDGE_DOUBLE;
		dev.input = sensor->pad.intfAttr.mipi.input;
	} else if (sensor->pad.intf == I6_INTF_BT656) {
		dev.edge = sensor->pad.intfAttr.bt656.edge;
		dev.sync = sensor->pad.intfAttr.bt656.sync;
		dev.bitswap = sensor->pad.intfAttr.bt656.bitswap;
	}

	ret = MI_VIF_SetDevAttr(0, &dev);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VIF_SetDevAttr failed %d\n", ret);
		return ret;
	}

	ret = MI_VIF_EnableDev(0);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VIF_EnableDev failed %d\n", ret);
		return ret;
	}

	port.capt.x = sensor->plane.capt.x + precrop->x;
	port.capt.y = sensor->plane.capt.y + precrop->y;
	port.capt.width = precrop->w;
	port.capt.height = precrop->h;
	port.dest.width = precrop->w;
	port.dest.height = precrop->h;
	port.field = 0;
	port.interlaceOn = 0;
	if (sensor->plane.bayer > I6_BAYER_END) {
		port.pixFmt = sensor->plane.pixFmt;
	} else {
		port.pixFmt = (i6_common_pixfmt)(I6_PIXFMT_RGB_BAYER +
			sensor->plane.precision * I6_BAYER_END + sensor->plane.bayer);
	}
	port.frate = I6_VIF_FRATE_FULL;
	port.frameLineCnt = 0;

	ret = MI_VIF_SetChnPortAttr(0, 0, &port);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VIF_SetChnPortAttr failed %d\n", ret);
		MI_VIF_DisableDev(0);
		return ret;
	}

	ret = MI_VIF_EnableChnPort(0, 0);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VIF_EnableChnPort failed %d\n", ret);
		MI_VIF_DisableChnPort(0, 0);
		MI_VIF_DisableDev(0);
		return ret;
	}

	return 0;
}

static void star6e_pipeline_stop_vif(void)
{
	MI_VIF_DisableChnPort(0, 0);
	MI_VIF_DisableDev(0);
}

static int star6e_pipeline_start_vpe(const SensorSelectResult *sensor,
	const Star6ePrecropRect *precrop, uint32_t out_width,
	uint32_t out_height, int mirror, int flip, int level_3dnr,
	SdkQuietState *sdk_quiet)
{
	MI_VPE_ChannelAttr_t channel_attr = {0};
	MI_VPE_ChannelParam_t param = {0};
	MI_VPE_PortAttr_t port = {0};
	MI_S32 ret;

	channel_attr.capt.width = precrop->w;
	channel_attr.capt.height = precrop->h;
	channel_attr.hdr = I6_HDR_OFF;
	channel_attr.sensor = (i6_vpe_sens)((int)sensor->pad_id + 1);
	channel_attr.mode = I6_VPE_MODE_REALTIME;
	if (sensor->plane.bayer > I6_BAYER_END) {
		channel_attr.pixFmt = sensor->plane.pixFmt;
	} else {
		channel_attr.pixFmt = (i6_common_pixfmt)(I6_PIXFMT_RGB_BAYER +
			sensor->plane.precision * I6_BAYER_END + sensor->plane.bayer);
	}

	sdk_quiet_begin(sdk_quiet);
	ret = MI_VPE_CreateChannel(0, &channel_attr);
	sdk_quiet_end(sdk_quiet);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VPE_CreateChannel failed %d\n", ret);
		return ret;
	}

	param.hdr = I6_HDR_OFF;
	param.level3DNR = level_3dnr;
	param.mirror = 0;
	param.flip = 0;
	param.lensAdjOn = 0;
	ret = MI_VPE_SetChannelParam(0, &param);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VPE_SetChannelParam failed %d\n", ret);
		MI_VPE_DestroyChannel(0);
		return ret;
	}

	/* Sensor-level orientation.  VPE digital mirror/flip is not used
	 * (unreliable on several sensor combos; param above keeps them at 0).
	 * MI_SNR_SetOrien programs the sensor's own register directly.
	 * Non-fatal: log if it fails so BSP regressions surface. */
	{
		MI_S32 orien_ret = MI_SNR_SetOrien(sensor->pad_id,
			mirror ? 1 : 0, flip ? 1 : 0);
		if (orien_ret != 0)
			fprintf(stderr, "[pipeline] WARNING: "
				"MI_SNR_SetOrien(pad=%d mirror=%d flip=%d) "
				"returned %d\n",
				(int)sensor->pad_id, mirror ? 1 : 0,
				flip ? 1 : 0, (int)orien_ret);
	}

	ret = MI_VPE_StartChannel(0);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VPE_StartChannel failed %d\n", ret);
		MI_VPE_DestroyChannel(0);
		return ret;
	}

	port.output.width = out_width;
	port.output.height = out_height;
	port.pixFmt = I6_PIXFMT_YUV420SP;
	port.compress = I6_COMPR_NONE;

	ret = MI_VPE_SetPortMode(0, 0, &port);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VPE_SetPortMode failed %d\n", ret);
		MI_VPE_StopChannel(0);
		MI_VPE_DestroyChannel(0);
		return ret;
	}

	ret = MI_VPE_EnablePort(0, 0);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VPE_EnablePort failed %d\n", ret);
		MI_VPE_DisablePort(0, 0);
		MI_VPE_StopChannel(0);
		MI_VPE_DestroyChannel(0);
		return ret;
	}

	return 0;
}

static void star6e_pipeline_stop_vpe(void)
{
	MI_VPE_DisablePort(0, 0);
	MI_VPE_StopChannel(0);
	MI_VPE_DestroyChannel(0);
}


/* ── Gyro motion source (IMU-assisted stabilization seam) ────────────────
 * The BMI270 driver (imu_bmi270.c) already runs frame-synced; when
 * imu.enabled it pushes timestamped 6-axis samples here via
 * star6e_pipeline_imu_push().  Today motion is estimated optically with
 * MI_IVE_Shift_Detector; this ring makes CLOCK_MONOTONIC-stamped gyro data
 * available frame-aligned so a gyro/optical fusion can drop into
 * star6e_stab_estimate_shift() without new plumbing.  Populated whenever the
 * IMU is enabled (also useful for telemetry/sidecar logging), independent of
 * whether stabilization is active. */
static ImuRing g_imu_ring;
static volatile int g_imu_ring_ready;


/* Compute the effective output dim for digital zoom.
 *
 * Approach C: zoom_pct shrinks BOTH crop and encoded output.  SCL output
 * dim == crop dim → 1:1 read/write, no upscale, no SCL bandwidth pressure.
 * The receiver sees the smaller frame in SPS/PPS and renders accordingly.
 *
 * Alignment 16 matches VENC and SCL.  Floor at 256 keeps the smallest
 * crop sane.  pct outside (0, 1) returns image dim unchanged. */
static void star6e_compute_zoom_dim(uint32_t image_w, uint32_t image_h,
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

/* Build a VPE port-crop rect that places a (rect_w × rect_h) 1:1 window
 * inside (vpe_w × vpe_h) input, centred at fractional (x, y).  No scaling:
 * the VPE port output is set to the same (rect_w × rect_h) by SetPortMode,
 * so SCL reads the rect verbatim and emits it unchanged.  X/Y are 8-px
 * aligned (VPE requirement); rect_w/h are 16-px-aligned by the caller via
 * compute_zoom_dim. */
static i6_common_rect star6e_compute_zoom_rect(uint32_t vpe_w, uint32_t vpe_h,
	uint32_t rect_w, uint32_t rect_h, double x, double y)
{
	const uint32_t XY_ALIGN = 8;
	i6_common_rect r;
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
	if (rect_w > vpe_w) rect_w = vpe_w & ~(XY_ALIGN - 1);
	if (rect_h > vpe_h) rect_h = vpe_h & ~(XY_ALIGN - 1);

	cx = (double)vpe_w * x - (double)rect_w * 0.5;
	cy = (double)vpe_h * y - (double)rect_h * 0.5;
	if (cx < 0.0) cx = 0.0;
	if (cy < 0.0) cy = 0.0;
	if (cx + (double)rect_w > (double)vpe_w) cx = (double)(vpe_w - rect_w);
	if (cy + (double)rect_h > (double)vpe_h) cy = (double)(vpe_h - rect_h);

	rx = (uint32_t)(cx + 0.5) & ~(XY_ALIGN - 1);
	ry = (uint32_t)(cy + 0.5) & ~(XY_ALIGN - 1);
	if (rx + rect_w > vpe_w) rx = (vpe_w - rect_w) & ~(XY_ALIGN - 1);
	if (ry + rect_h > vpe_h) ry = (vpe_h - rect_h) & ~(XY_ALIGN - 1);

	r.x = (unsigned short)rx;
	r.y = (unsigned short)ry;
	r.width = (unsigned short)rect_w;
	r.height = (unsigned short)rect_h;
	return r;
}

static Star6eZoomStatus g_zoom_status;
static pthread_mutex_t g_zoom_status_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t star6e_zoom_level_x100(double pct)
{
	if (!isfinite(pct) || pct <= 0.0)
		return 0;
	return (uint32_t)(100.0 / pct + 0.5);
}

static void star6e_pipeline_set_zoom_status(double pct,
	uint32_t output_w, uint32_t output_h, const Star6ePrecropRect *base,
	const i6_common_rect *rect)
{
	Star6eZoomStatus snap;

	memset(&snap, 0, sizeof(snap));
	if (base && rect) {
		snap.active = 1;
		snap.level_x100 = star6e_zoom_level_x100(pct);
		snap.output_w = output_w;
		snap.output_h = output_h;
		snap.crop_x = (uint32_t)base->x + rect->x;
		snap.crop_y = (uint32_t)base->y + rect->y;
		snap.crop_w = rect->width;
		snap.crop_h = rect->height;
	}

	pthread_mutex_lock(&g_zoom_status_mutex);
	g_zoom_status = snap;
	pthread_mutex_unlock(&g_zoom_status_mutex);
}

static void star6e_pipeline_clear_zoom_status(void)
{
	pthread_mutex_lock(&g_zoom_status_mutex);
	memset(&g_zoom_status, 0, sizeof(g_zoom_status));
	pthread_mutex_unlock(&g_zoom_status_mutex);
}

void star6e_pipeline_zoom_status(Star6eZoomStatus *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&g_zoom_status_mutex);
	*out = g_zoom_status;
	pthread_mutex_unlock(&g_zoom_status_mutex);
}

/* ----------------------------------------------------------------------- */
/*  Pan ramp: smooth live x/y panning via exponential decay.               */
/*                                                                          */
/*  apply_zoom records the target x/y; a dedicated tick thread runs at      */
/*  ~60Hz and steps `current` toward `target` each tick.  The crop rect     */
/*  the SDK sees is computed from `current`, not from the user input.      */
/*                                                                          */
/*  Per-step: current += (target - current) * alpha, where                 */
/*  alpha = 1 - exp(-tick_ms / PAN_RAMP_DEFAULT_MS).  Hardcoded — see       */
/*  PAN_RAMP_DEFAULT_MS rationale below.                                   */
/* ----------------------------------------------------------------------- */

#define PAN_RAMP_TICK_MS         16   /* ~60 Hz */
#define PAN_RAMP_SNAP_EPSILON    0.0005  /* fraction of frame */
/* Pan smoothing time constant.  150 ms ≈ 95 % settle in ~450 ms — fast
 * enough to feel responsive on a joystick, slow enough to mask the
 * step-quantization of an HTTP-driven SET stream.  Was a live config
 * field briefly; reverted to a hardcoded constant because the
 * additional knob added schema/UI/API surface without a use case
 * stronger than "user might want a different feel". */
#define PAN_RAMP_DEFAULT_MS      150

typedef struct {
	pthread_t        thread;
	pthread_mutex_t  lock;
	pthread_cond_t   cv;

	int       running;        /* 1 while pipeline is live */
	int       has_state;      /* 1 once pipeline_start populated `state` */

	double    target_pct;
	double    target_x, target_y;
	double    current_x, current_y;
	uint32_t  ramp_ms;        /* 0 = snap, else exponential decay τ */

	/* Borrowed; lifetime owned by pipeline_start/stop.  Always NULL when
	 * running==0. */
	Star6ePipelineState *state;
} Star6ePanRampState;

static Star6ePanRampState g_pan_ramp = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.cv   = PTHREAD_COND_INITIALIZER,
	.ramp_ms = PAN_RAMP_DEFAULT_MS,
};

/* MI_ISP_CUS3A_SetAECropSize confines the AE meter to a sub-rect of the
 * ISP frame in 0..1023 normalized coords.  Resolved lazily; if absent or
 * libmi_isp didn't dlopen, every call is a silent no-op so the pipeline
 * still runs (master behaviour before AE-aware zoom was added).
 * Star6eAeCropRect lives in star6e_framing_host.h (shared with framing
 * modules). */
typedef int (*star6e_set_ae_crop_fn_t)(uint32_t channel,
	Star6eAeCropRect *data);

static star6e_set_ae_crop_fn_t g_star6e_set_ae_crop;
static int g_star6e_ae_crop_resolved;
static int g_star6e_ae_crop_ready;     /* 1 once CUS3A/ISP can accept calls */
static int g_star6e_ae_crop_disabled;  /* 1 if the SDK rejected a call — give up */
static Star6eAeCropRect g_star6e_ae_crop_last = { 0, 0, 1023, 1023 };

static void star6e_apply_ae_crop(double pct, double x, double y);

/* Compute and program the VPE port crop for (pct, x, y).  Caller must
 * already hold a valid pipeline state and image_width/height.  Side-
 * effect: updates the zoom_status snapshot. */
static int star6e_pan_apply_locked(Star6ePipelineState *state,
	double pct, double x, double y)
{
	i6_common_rect rect;
	MI_S32 ret;
	uint32_t in_w = state->active_precrop.w;
	uint32_t in_h = state->active_precrop.h;

	if (in_w == 0 || in_h == 0)
		return -1;

	rect = star6e_compute_zoom_rect(in_w, in_h,
		state->image_width, state->image_height, x, y);
	ret = MI_VPE_SetPortCrop(0, 0, &rect);
	if (ret != 0) {
		fprintf(stderr,
			"WARNING: MI_VPE_SetPortCrop(0,0) pan x=%.3f y=%.3f failed %d\n",
			x, y, (int)ret);
		return -1;
	}
	star6e_pipeline_set_zoom_status(pct, state->image_width,
		state->image_height, &state->active_precrop, &rect);
	return 0;
}

static void *star6e_pan_ramp_thread(void *arg)
{
	(void)arg;
	for (;;) {
		struct timespec ts;
		Star6ePipelineState *st;
		double tx, ty, pct, cx, cy, alpha;
		uint32_t ramp_ms;
		int has_state;

		pthread_mutex_lock(&g_pan_ramp.lock);
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += (long)PAN_RAMP_TICK_MS * 1000000L;
		while (ts.tv_nsec >= 1000000000L) {
			ts.tv_nsec -= 1000000000L;
			ts.tv_sec  += 1;
		}
		/* Idle when current already at target — wake on signal only. */
		while (g_pan_ramp.running &&
		    fabs(g_pan_ramp.target_x - g_pan_ramp.current_x) < PAN_RAMP_SNAP_EPSILON &&
		    fabs(g_pan_ramp.target_y - g_pan_ramp.current_y) < PAN_RAMP_SNAP_EPSILON) {
			int rc = pthread_cond_wait(&g_pan_ramp.cv, &g_pan_ramp.lock);
			if (rc != 0)
				break;
		}
		if (!g_pan_ramp.running) {
			pthread_mutex_unlock(&g_pan_ramp.lock);
			break;
		}
		/* Now we have a delta to step; pace the next iteration. */
		(void)pthread_cond_timedwait(&g_pan_ramp.cv, &g_pan_ramp.lock, &ts);
		if (!g_pan_ramp.running) {
			pthread_mutex_unlock(&g_pan_ramp.lock);
			break;
		}

		st        = g_pan_ramp.state;
		has_state = g_pan_ramp.has_state;
		ramp_ms   = g_pan_ramp.ramp_ms;
		tx        = g_pan_ramp.target_x;
		ty        = g_pan_ramp.target_y;
		pct       = g_pan_ramp.target_pct;

		if (!has_state || !st || pct <= 0.0 || pct >= 1.0) {
			g_pan_ramp.current_x = tx;
			g_pan_ramp.current_y = ty;
			pthread_mutex_unlock(&g_pan_ramp.lock);
			continue;
		}

		if (ramp_ms == 0) {
			alpha = 1.0;
		} else {
			alpha = 1.0 - exp(-(double)PAN_RAMP_TICK_MS / (double)ramp_ms);
			if (alpha > 1.0) alpha = 1.0;
			if (alpha < 0.0) alpha = 0.0;
		}

		g_pan_ramp.current_x += (tx - g_pan_ramp.current_x) * alpha;
		g_pan_ramp.current_y += (ty - g_pan_ramp.current_y) * alpha;
		if (fabs(tx - g_pan_ramp.current_x) < PAN_RAMP_SNAP_EPSILON)
			g_pan_ramp.current_x = tx;
		if (fabs(ty - g_pan_ramp.current_y) < PAN_RAMP_SNAP_EPSILON)
			g_pan_ramp.current_y = ty;

		cx = g_pan_ramp.current_x;
		cy = g_pan_ramp.current_y;
		pthread_mutex_unlock(&g_pan_ramp.lock);

		(void)star6e_pan_apply_locked(st, pct, cx, cy);
	}
	return NULL;
}

static int star6e_pan_ramp_start(Star6ePipelineState *state,
	double pct, double x, double y)
{
	int rc;

	pthread_mutex_lock(&g_pan_ramp.lock);
	if (g_pan_ramp.running) {
		/* Re-arm with the new state without recreating the thread. */
		g_pan_ramp.state        = state;
		g_pan_ramp.has_state    = 1;
		g_pan_ramp.target_pct   = pct;
		g_pan_ramp.target_x     = x;
		g_pan_ramp.target_y     = y;
		g_pan_ramp.current_x    = x;
		g_pan_ramp.current_y    = y;
		g_pan_ramp.ramp_ms      = PAN_RAMP_DEFAULT_MS;
		pthread_cond_signal(&g_pan_ramp.cv);
		pthread_mutex_unlock(&g_pan_ramp.lock);
		return 0;
	}
	g_pan_ramp.state        = state;
	g_pan_ramp.has_state    = 1;
	g_pan_ramp.target_pct   = pct;
	g_pan_ramp.target_x     = x;
	g_pan_ramp.target_y     = y;
	g_pan_ramp.current_x    = x;
	g_pan_ramp.current_y    = y;
	g_pan_ramp.ramp_ms      = PAN_RAMP_DEFAULT_MS;
	g_pan_ramp.running      = 1;
	pthread_mutex_unlock(&g_pan_ramp.lock);

	rc = pthread_create(&g_pan_ramp.thread, NULL,
		star6e_pan_ramp_thread, NULL);
	if (rc != 0) {
		fprintf(stderr,
			"WARNING: pthread_create(pan_ramp) failed %d (%s)\n",
			rc, strerror(rc));
		pthread_mutex_lock(&g_pan_ramp.lock);
		g_pan_ramp.running   = 0;
		g_pan_ramp.has_state = 0;
		g_pan_ramp.state     = NULL;
		pthread_mutex_unlock(&g_pan_ramp.lock);
		return -1;
	}

	/* Program the initial crop rect synchronously.  The ramp thread would
	 * otherwise idle because current==target on first start, leaving the
	 * VPE port at full-frame.  AE crop fires from the ISP-ready hook
	 * (star6e_ae_crop_mark_ready) — too early to call here. */
	if (pct > 0.0 && pct < 1.0)
		(void)star6e_pan_apply_locked(state, pct, x, y);
	return 0;
}

static void star6e_pan_ramp_stop(void)
{
	pthread_t th;
	int was_running;

	pthread_mutex_lock(&g_pan_ramp.lock);
	was_running          = g_pan_ramp.running;
	g_pan_ramp.running   = 0;
	g_pan_ramp.has_state = 0;
	g_pan_ramp.state     = NULL;
	th                   = g_pan_ramp.thread;
	pthread_cond_broadcast(&g_pan_ramp.cv);
	pthread_mutex_unlock(&g_pan_ramp.lock);

	if (was_running) {
		pthread_join(th, NULL);
	} else {
	}
}

/* Live pan: zoom_pct is MUT_RESTART (changing crop dim resizes VPE port and
 * VENC, which needs reinit), so the live path only handles x/y.  pct is
 * accepted just to short-circuit when zoom is off (rect dim == image dim).
 * Updates the *target*; the ramp thread tweens `current` toward it. */
/* Live stab pause toggle (framing=stab and framing=stab-fill).  Routed to the
 * active framing module's set_live hook so the pipeline stays agnostic of the
 * preset's semantics.  No-op (returns -1) when no stab module is selected
 * (framing off / zoom / STAB=0). */
int star6e_pipeline_set_pause_stab(bool paused)
{
	if (g_framing && g_framing->set_live)
		return g_framing->set_live("pause", paused ? "1" : "0");
	return -1;
}

int star6e_pipeline_apply_zoom(Star6ePipelineState *state,
	double pct, double x, double y)
{
	if (!state) return -1;
	if (!isfinite(pct) || !isfinite(x) || !isfinite(y))
		return -1;
	/* When stabilization is active the crop window is enforced CENTERED
	 * (0.5/0.5) so the accumulator has symmetric headroom — zoomX/zoomY are
	 * the zoom-mode pan and must not steer the stab window off-center (that
	 * would pin it against the frame edge and kill stabilization).  Ignore
	 * live x/y here; keep the window centered. */
	if (g_framing && g_framing->active && g_framing->active()) {
		if (g_framing->set_pan)
			g_framing->set_pan(0.5, 0.5);
		return 0;
	}
	if (pct <= 0.0 || pct >= 1.0) {
		star6e_pipeline_clear_zoom_status();
		pthread_mutex_lock(&g_pan_ramp.lock);
		g_pan_ramp.target_pct = 0.0;
		g_pan_ramp.target_x   = x;
		g_pan_ramp.target_y   = y;
		g_pan_ramp.current_x  = x;
		g_pan_ramp.current_y  = y;
		pthread_cond_signal(&g_pan_ramp.cv);
		pthread_mutex_unlock(&g_pan_ramp.lock);
		star6e_apply_ae_crop(pct, x, y);
		return 0;  /* zoom off — nothing to pan */
	}

	if (state->active_precrop.w == 0 || state->active_precrop.h == 0)
		return -1;  /* VPE not started yet */

	pthread_mutex_lock(&g_pan_ramp.lock);
	g_pan_ramp.state      = state;
	g_pan_ramp.has_state  = 1;
	g_pan_ramp.target_pct = pct;
	g_pan_ramp.target_x   = x;
	g_pan_ramp.target_y   = y;
	if (g_pan_ramp.ramp_ms == 0) {
		g_pan_ramp.current_x = x;
		g_pan_ramp.current_y = y;
	}
	pthread_cond_signal(&g_pan_ramp.cv);
	pthread_mutex_unlock(&g_pan_ramp.lock);

	star6e_apply_ae_crop(pct, x, y);
	return 0;
}

/* Emit a normalized (0..1023) AE-meter crop rect to the ISP.  Shared by the
 * zoom and stabilization tracking paths.  Coords are normalized against the
 * ISP frame (the post-VIF input — same domain as `active_precrop`).
 *
 * Failure modes the bench taught us:
 *   - The SDK refuses calls before CUS3A handoff; we gate on
 *     g_star6e_ae_crop_ready and just queue the change in the cache.
 *   - Calling with full-frame (0,0,1023,1023) on this BSP returns -1
 *     during init.  Whether it works at all is uncertain, so callers never
 *     build a full-frame rect — the cache is pre-seeded full-frame and only
 *     sub-rects ever get pushed.  When the crop is removed live, the meter
 *     stays on the last rect; the ISP's own filtering re-equilibrates
 *     exposure over the next few seconds.
 *   - On any non-zero return from the SDK, we set
 *     g_star6e_ae_crop_disabled and stop calling for the rest of the
 *     process.  Next start cold-resets via pipeline_stop. */
void star6e_emit_ae_crop(const Star6eAeCropRect *r)
{
	if (g_star6e_ae_crop_disabled)
		return;
	if (!g_star6e_ae_crop_resolved) {
		g_star6e_set_ae_crop = (star6e_set_ae_crop_fn_t)
			dlsym(RTLD_DEFAULT, "MI_ISP_CUS3A_SetAECropSize");
		g_star6e_ae_crop_resolved = 1;
		if (!g_star6e_set_ae_crop)
			fprintf(stderr,
				"WARNING: MI_ISP_CUS3A_SetAECropSize not present — "
				"AE will not track zoom/stab\n");
	}
	if (!g_star6e_set_ae_crop)
		return;

	if (r->crop_x == g_star6e_ae_crop_last.crop_x &&
	    r->crop_y == g_star6e_ae_crop_last.crop_y &&
	    r->crop_w == g_star6e_ae_crop_last.crop_w &&
	    r->crop_h == g_star6e_ae_crop_last.crop_h)
		return;

	g_star6e_ae_crop_last = *r;

	if (!g_star6e_ae_crop_ready)
		return;  /* queued — ISP not yet ready */

	if (g_star6e_set_ae_crop(0, (Star6eAeCropRect *)r) != 0) {
		fprintf(stderr,
			"WARNING: MI_ISP_CUS3A_SetAECropSize(%u,%u,%u,%u) failed — "
			"disabling AE crop-tracking for this run\n",
			r->crop_x, r->crop_y, r->crop_w, r->crop_h);
		g_star6e_ae_crop_disabled = 1;
	}
}

/* Zoom path: constrain the AE meter to the zoom rect.  The encoded dim
 * (state->image_width) is the crop size in precrop coords (Approach C SCL is
 * 1:1), so the meter fraction is image_width / active_precrop.w. */
static void star6e_apply_ae_crop(double pct, double x, double y)
{
	Star6eAeCropRect r;
	uint32_t in_w, in_h;
	uint32_t rect_w, rect_h;
	double cx, cy, rx, ry;
	Star6ePipelineState *state;

	/* Skip the "full frame" path entirely.  See star6e_emit_ae_crop. */
	if (!isfinite(pct) || pct <= 0.0 || pct >= 1.0)
		return;

	pthread_mutex_lock(&g_pan_ramp.lock);
	state = g_pan_ramp.has_state ? g_pan_ramp.state : NULL;
	pthread_mutex_unlock(&g_pan_ramp.lock);
	if (!state || state->active_precrop.w == 0 ||
	    state->active_precrop.h == 0)
		return;

	in_w   = state->active_precrop.w;
	in_h   = state->active_precrop.h;
	rect_w = state->image_width;
	rect_h = state->image_height;
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
	rx = cx * 1023.0 / (double)in_w;
	ry = cy * 1023.0 / (double)in_h;
	memset(&r, 0, sizeof(r));
	r.crop_x = (uint16_t)(rx + 0.5);
	r.crop_y = (uint16_t)(ry + 0.5);
	r.crop_w = (uint16_t)((double)rect_w * 1023.0 / (double)in_w + 0.5);
	r.crop_h = (uint16_t)((double)rect_h * 1023.0 / (double)in_h + 0.5);
	if (r.crop_w == 0) r.crop_w = 1;
	if (r.crop_h == 0) r.crop_h = 1;
	if (r.crop_x + r.crop_w > 1023)
		r.crop_x = (uint16_t)(1023 - r.crop_w);
	if (r.crop_y + r.crop_h > 1023)
		r.crop_y = (uint16_t)(1023 - r.crop_h);

	star6e_emit_ae_crop(&r);
}


/* Called from the ISP-ready hook in pipeline_start.  If the user booted
 * with zoom active, this is the first chance the AE crop can actually
 * reach the SDK; flush the cached rect now. */
static void star6e_ae_crop_mark_ready(void)
{
	double pct, x, y;

	g_star6e_ae_crop_ready = 1;

	pthread_mutex_lock(&g_pan_ramp.lock);
	pct = g_pan_ramp.target_pct;
	x   = g_pan_ramp.target_x;
	y   = g_pan_ramp.target_y;
	pthread_mutex_unlock(&g_pan_ramp.lock);

	if (isfinite(pct) && pct > 0.0 && pct < 1.0) {
		/* Force re-emit: invalidate cache, then call. */
		g_star6e_ae_crop_last.crop_w = 0;
		star6e_apply_ae_crop(pct, x, y);
	}
}

static void star6e_pipeline_fill_h26x_attr(i6_venc_attr_h26x *attr,
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

static int star6e_pipeline_pre_start_apply_ref_pred(MI_VENC_CHN chn,
	const VencConfig *vcfg);

static int star6e_pipeline_start_venc(uint32_t width, uint32_t height,
	uint32_t bitrate, uint32_t framerate, uint32_t gop, PAYLOAD_TYPE_E codec,
	int rc_mode, const VencConfig *vcfg,
	MI_VENC_CHN *chn)
{
	MI_VENC_ChnAttr_t attr = {0};
	MI_U32 bit_rate_bits;
	MI_S32 ret;

	/* Same practical bitrate bounds as the live apply_bitrate() path, so a
	 * persisted sub-floor video0.bitrate can't birth the encoder collapsed
	 * at boot / SIGHUP re-exec. */
	if (bitrate > VENC_BITRATE_MAX_KBPS)
		bitrate = VENC_BITRATE_MAX_KBPS;
	if (bitrate < VENC_BITRATE_MIN_KBPS)
		bitrate = VENC_BITRATE_MIN_KBPS;
	bit_rate_bits = bitrate * 1024;

	if (codec == PT_H265) {
		attr.attrib.codec = I6_VENC_CODEC_H265;
		star6e_pipeline_fill_h26x_attr(&attr.attrib.h265, width, height);
	} else {
		attr.attrib.codec = I6_VENC_CODEC_H264;
		star6e_pipeline_fill_h26x_attr(&attr.attrib.h264, width, height);
	}

	switch (codec) {
	case PT_H265:
		switch (rc_mode) {
		case 4:
			attr.rate.mode = I6_VENC_RATEMODE_H265VBR;
			attr.rate.h265Vbr = (i6_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 45, .minQual = 20,
			};
			break;
		case 5:
			attr.rate.mode = I6_VENC_RATEMODE_H265AVBR;
			attr.rate.h265Avbr = (i6_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 45, .minQual = 20,
			};
			break;
		case 6:
			attr.rate.mode = I6_VENC_RATEMODE_H265VBR;
			attr.rate.h265Vbr = (i6_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 40, .minQual = 28,
			};
			break;
		case 3:
		default:
			attr.rate.mode = I6_VENC_RATEMODE_H265CBR;
			attr.rate.h265Cbr = (i6_venc_rate_h26xcbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.bitrate = bit_rate_bits, .avgLvl = 1,
			};
			break;
		}
		break;

	case PT_H264:
	default:
		switch (rc_mode) {
		case 2:
			attr.rate.mode = I6_VENC_RATEMODE_H264VBR;
			attr.rate.h264Vbr = (i6_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 45, .minQual = 20,
			};
			break;
		case 0:
			attr.rate.mode = I6_VENC_RATEMODE_H264AVBR;
			attr.rate.h264Avbr = (i6_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 45, .minQual = 20,
			};
			break;
		case 1:
			attr.rate.mode = I6_VENC_RATEMODE_H264VBR;
			attr.rate.h264Vbr = (i6_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 40, .minQual = 28,
			};
			break;
		case 3:
		default:
			attr.rate.mode = I6_VENC_RATEMODE_H264CBR;
			attr.rate.h264Cbr = (i6_venc_rate_h26xcbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.bitrate = bit_rate_bits, .avgLvl = 1,
			};
			break;
		}
		break;
	}

	ret = MI_VENC_CreateChn(*chn, &attr);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VENC_CreateChn(%d) failed %d\n",
			*chn, ret);
		return ret;
	}

	/* SDK convention: SetRefParam must be called between CreateChn and
	 * StartRecvPic.  Star6E silently no-ops the call if invoked after
	 * StartRecvPic, producing a flat single-layer stream. */
	(void)star6e_pipeline_pre_start_apply_ref_pred(*chn, vcfg);

	/* H.265 multi-slice split (same pre-Start window as SetRefParam).
	 * sliceCount is what the operator asks for; the SDK field is 32-px
	 * rows per slice, but the encoder's CTU is 64 (measured 2026-08-20:
	 * SPS says 30x17 CTBs at 1080p), so the SDK rounds the request up to
	 * whole CTU-64 rows.  Log the DELIVERED geometry, not the request —
	 * at 1080p sliceCount 8 quantizes to 6 slices of 3 CTU rows.
	 * vcfg is optional on this path: star6e_pipeline_start_dual()
	 * passes NULL for ch1, so guard before reading slice_count. */
	if (vcfg && vcfg->video0.slice_count > 1) {
		uint32_t rows = (height + 31) / 32;
		uint32_t per = (rows + vcfg->video0.slice_count - 1) / vcfg->video0.slice_count;
		uint32_t ctu_rows = (height + 63) / 64;
		uint32_t ctu_per;
		MI_VENC_ParamH265SliceSplit_t split;
		MI_VENC_ParamH265SliceSplit_t rb;

		if (codec != PT_H265) {
			fprintf(stderr, "ERROR: sliceCount=%u requires H.265\n",
				vcfg->video0.slice_count);
			MI_VENC_DestroyChn(*chn);
			return -1;
		}

		/* Optional export: some libmi_venc.so revisions omit it and the
		 * macro then returns -1, which reads as a driver rejection and
		 * sends the operator hunting geometry.  Stay fatal -- delivering
		 * one slice while the API advertises N would lie to the
		 * receiver's spatial concealment -- but say why. */
		if (!g_mi_venc.fnSetH265SliceSplit ||
		    !g_mi_venc.fnGetH265SliceSplit) {
			fprintf(stderr, "ERROR: sliceCount=%u requested but "
				"libmi_venc.so does not export "
				"MI_VENC_SetH265SliceSplit/GetH265SliceSplit\n",
				vcfg->video0.slice_count);
			MI_VENC_DestroyChn(*chn);
			return -1;
		}

		/* B3: the vendor reads all 8 bytes; leaving the pad
		 * uninitialised puts stack garbage across the ABI. */
		memset(&split, 0, sizeof(split));
		split.bSplitEnable = 1;
		split.u32SliceRowCount = per;
		ret = MI_VENC_SetH265SliceSplit(*chn, &split);
		if (ret != 0) {
			fprintf(stderr,
				"ERROR: MI_VENC_SetH265SliceSplit(%u rows/slice) "
				"failed %d\n",
				split.u32SliceRowCount, ret);
			MI_VENC_DestroyChn(*chn);
			return ret;
		}
		memset(&rb, 0, sizeof(rb));
		ret = MI_VENC_GetH265SliceSplit(*chn, &rb);
		if (ret != 0 || !rb.bSplitEnable ||
		    rb.u32SliceRowCount == 0 || rb.u32SliceRowCount > rows) {
			fprintf(stderr, "ERROR: H.265 slice split readback invalid: "
				"ret=%d enable=%u rows=%u requested_rows=%u\n",
				ret, (unsigned)rb.bSplitEnable,
				(unsigned)rb.u32SliceRowCount,
				(unsigned)split.u32SliceRowCount);
			MI_VENC_DestroyChn(*chn);
			return ret != 0 ? ret : -1;
		}
		ctu_per = (rb.u32SliceRowCount + 1) / 2;
		printf("VENC: H.265 slice split ON: requested=%u rows32=%u "
		       "applied_rows32=%u predicted=%u (readback verified)\n",
		       vcfg->video0.slice_count, split.u32SliceRowCount,
		       rb.u32SliceRowCount,
		       (ctu_rows + ctu_per - 1) / ctu_per);
	}

	/* Cap the bitstream buffer before encoding starts — see
	 * FRAME_GATE_STREAM_BUF_FRAMES.  Advisory: an older libmi_venc.so
	 * without the symbol, or a refusal, just leaves the SDK default of 3,
	 * which costs latency while gated but nothing else. */
	if (MI_VENC_SetMaxStreamCnt(*chn, FRAME_GATE_STREAM_BUF_FRAMES) != 0)
		fprintf(stderr, "WARNING: SetMaxStreamCnt(%u) refused; "
			"keeping the SDK default depth\n",
			FRAME_GATE_STREAM_BUF_FRAMES);

	ret = MI_VENC_StartRecvPic(*chn);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VENC_StartRecvPic failed %d\n", ret);
		MI_VENC_DestroyChn(*chn);
		return ret;
	}

	return 0;
}

static void star6e_pipeline_stop_venc(MI_VENC_CHN chn)
{
	MI_VENC_StopRecvPic(chn);
	MI_VENC_DestroyChn(chn);
}

static Star6eIntraRefreshStatus g_intra_status;
static pthread_mutex_t g_intra_status_mutex = PTHREAD_MUTEX_INITIALIZER;

void star6e_pipeline_intra_refresh_status(Star6eIntraRefreshStatus *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&g_intra_status_mutex);
	*out = g_intra_status;
	pthread_mutex_unlock(&g_intra_status_mutex);
}

/* Compute IntraRefresh derived params from the current config.  Out param
 * is always populated; mode is also returned for caller convenience. */
static IntraRefreshMode star6e_pipeline_intra_refresh_derive(
	const VencConfig *vcfg, uint32_t height, uint32_t fps,
	PAYLOAD_TYPE_E codec, IntraRefreshDerived *out_ir)
{
	IntraRefreshMode mode = INTRA_MODE_OFF;

	memset(out_ir, 0, sizeof(*out_ir));
	if (vcfg) {
		mode = intra_refresh_parse_mode(vcfg->video0.intra_refresh_mode);
		(void)codec; /* H.265 only */
		intra_refresh_compute(mode, height, fps,
			vcfg->video0.intra_refresh_lines,
			vcfg->video0.intra_refresh_qp,
			vcfg->video0.gop_size, out_ir);
	}
	return mode;
}

static int star6e_pipeline_apply_intra_refresh(MI_VENC_CHN chn,
	const VencConfig *vcfg, uint32_t height, uint32_t fps,
	PAYLOAD_TYPE_E codec)
{
	MI_VENC_IntraRefresh_t cfg;
	Star6eIntraRefreshStatus snap;
	IntraRefreshDerived ir;
	IntraRefreshMode mode;
	const char *name;

	memset(&snap, 0, sizeof(snap));
	mode = star6e_pipeline_intra_refresh_derive(vcfg, height, fps, codec, &ir);
	name = intra_refresh_mode_name(mode);

	snprintf(snap.mode_name, sizeof(snap.mode_name), "%s", name);
	snap.mi_supported = g_mi_venc.fnSetIntraRefresh ? 1 : 0;
	if (vcfg) {
		snap.requested_lines  = vcfg->video0.intra_refresh_lines;
		snap.requested_qp     = vcfg->video0.intra_refresh_qp;
		snap.explicit_gop_sec = vcfg->video0.gop_size;
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
			"but libmi_venc.so does not export MI_VENC_SetIntraRefresh\n",
			name);
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

	memset(&cfg, 0, sizeof(cfg));
	cfg.bEnable = 1;
	cfg.u32RefreshLineNum = ir.lines;
	cfg.u32ReqIQp = ir.req_iqp;

	if (MI_VENC_SetIntraRefresh(chn, &cfg) != 0) {
		fprintf(stderr, "[waybeam] ERROR: MI_VENC_SetIntraRefresh(chn=%d, "
			"lines=%u, qp=%u) failed\n", chn,
			cfg.u32RefreshLineNum, cfg.u32ReqIQp);
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
	fprintf(stderr, "[waybeam] intraRefresh: mode=%s lines/P=%u qp=%u "
		"gop=%.2fs (%s)\n", name, cfg.u32RefreshLineNum, cfg.u32ReqIQp,
		snap.effective_gop_sec, snap.gop_auto ? "auto" : "explicit");
	return 0;
}

static Star6eRefPredStatus g_ref_pred_status;
static pthread_mutex_t g_ref_pred_status_mutex = PTHREAD_MUTEX_INITIALIZER;

void star6e_pipeline_ref_pred_status(Star6eRefPredStatus *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&g_ref_pred_status_mutex);
	*out = g_ref_pred_status;
	pthread_mutex_unlock(&g_ref_pred_status_mutex);
}

/* SVC-T reference structure — opt-in via video0.refBase.  Disabled means
 * SDK default single-layer reference (one P references previous P).
 *
 * Star6E SDK requires SetRefParam to land between CreateChn and
 * StartRecvPic — calling it later silently no-ops (verified with the
 * test_ref_pred harness: bitstream identical at any post-Start value).
 * Therefore this helper is invoked from star6e_pipeline_start_venc()
 * immediately after CreateChn. */
static int star6e_pipeline_pre_start_apply_ref_pred(MI_VENC_CHN chn,
	const VencConfig *vcfg)
{
	MI_VENC_ParamRef_t ref;
	Star6eRefPredStatus snap;

	memset(&snap, 0, sizeof(snap));
	snap.mi_supported = g_mi_venc.fnSetRefParam ? 1 : 0;
	if (vcfg) {
		snap.base    = vcfg->video0.ref_base;
		snap.enhance = vcfg->video0.ref_enhance;
		snap.pred    = vcfg->video0.ref_pred ? 1 : 0;
	}

	if (!vcfg || vcfg->video0.ref_base == 0)
		goto publish;
	if (!g_mi_venc.fnSetRefParam) {
		fprintf(stderr, "[waybeam] WARNING: refBase=%u requested but "
			"libmi_venc.so does not export MI_VENC_SetRefParam\n",
			vcfg->video0.ref_base);
		goto publish;
	}

	memset(&ref, 0, sizeof(ref));
	ref.u32Base     = vcfg->video0.ref_base;
	ref.u32Enhance  = vcfg->video0.ref_enhance ? vcfg->video0.ref_enhance : 1;
	ref.bEnablePred = vcfg->video0.ref_pred ? 1 : 0;

	if (MI_VENC_SetRefParam(chn, &ref) != 0) {
		fprintf(stderr, "[waybeam] ERROR: MI_VENC_SetRefParam(chn=%d, "
			"base=%u, enhance=%u, pred=%u) failed\n", chn,
			ref.u32Base, ref.u32Enhance, ref.bEnablePred);
		goto publish;
	}
	snap.apply_ok = 1;
	snap.active   = 1;
	fprintf(stderr, "[waybeam] refPred: chn=%d base=%u enhance=%u pred=%u "
		"(applied pre-Start)\n", chn, ref.u32Base, ref.u32Enhance,
		ref.bEnablePred);
publish:
	pthread_mutex_lock(&g_ref_pred_status_mutex);
	g_ref_pred_status = snap;
	pthread_mutex_unlock(&g_ref_pred_status_mutex);
	return snap.active ? 0 : (vcfg && vcfg->video0.ref_base > 0 ? -1 : 0);
}

static void star6e_pipeline_sysfs_write(const char *path, const char *value)
{
	FILE *handle = fopen(path, "w");

	if (!handle)
		return;

	fprintf(handle, "%s\n", value);
	fclose(handle);
}

static void star6e_pipeline_set_hw_clocks(int oc_level, int verbose)
{
	static const struct {
		const char *path;
		const char *label;
	} clocks[] = {
		{ "/sys/devices/virtual/mstar/isp0/isp_clk", "ISP" },
		{ "/sys/devices/virtual/mstar/mscl/clk", "SCL" },
	};
	unsigned int i;

	for (i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		FILE *handle = fopen(clocks[i].path, "w");

		if (!handle)
			continue;

		fprintf(handle, "384000000\n");
		fclose(handle);
		if (verbose)
			printf("> Set %s clock to 384 MHz\n", clocks[i].label);
	}

	if (oc_level >= 1) {
		star6e_pipeline_sysfs_write(
			"/sys/devices/virtual/mstar/venc0/clk", "480000000");
		if (verbose)
			printf("> Set VENC clock to 480 MHz (oc-level %d)\n",
				oc_level);
	}

	if (oc_level >= 2) {
		star6e_pipeline_sysfs_write(
			"/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
			"performance");
		star6e_pipeline_sysfs_write(
			"/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq",
			"1200000");
		star6e_pipeline_sysfs_write(
			"/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq",
			"1200000");
		if (verbose)
			printf("> Set CPU to 1200 MHz performance governor (oc-level %d)\n",
				oc_level);
	}
}

/* Aggregates all parameters derived from VencConfig before pipeline hardware
 * is touched.  Populated by prepare_pipeline_config(), consumed by the
 * remaining helpers and the main orchestrator. */
typedef struct {
	Star6eOutputSetup  output_setup;
	SensorSelectConfig sensor_cfg;
	SensorUnlockConfig sensor_unlock;
	SensorStrategy     sensor_strategy;
	char               isp_bin_path[256];   /* "" if no bin should be loaded;
	                                         * resolved by select_and_configure_sensor()
	                                         * after we know the live sensor name */
	uint32_t           sensor_width;
	uint32_t           sensor_height;
	uint32_t           image_width;
	uint32_t           image_height;
	uint32_t           sensor_framerate;
	uint32_t           venc_max_rate;
	uint32_t           venc_gop_size;
	uint32_t           exposure_cap_us;
	Star6ePrecropRect  precrop;
	PAYLOAD_TYPE_E     rc_codec;
	int                rc_mode;
	int                image_mirror;
	int                image_flip;
	int                vpe_level_3dnr;
	int                oc_level;
} Star6ePipelineConfig;

/* Phase 1: validate vcfg, derive all scalar parameters, build output_setup and
 * sensor strategy.  No hardware is touched here. */
static int prepare_pipeline_config(Star6ePipelineState *state,
	const VencConfig *vcfg, Star6ePipelineConfig *pconf)
{
	memset(pconf, 0, sizeof(*pconf));

	pconf->sensor_width    = vcfg->video0.width;
	pconf->sensor_height   = vcfg->video0.height;
	pconf->image_width     = pconf->sensor_width;
	pconf->image_height    = pconf->sensor_height;
	pconf->sensor_framerate = vcfg->video0.fps;
	pconf->venc_max_rate   = vcfg->video0.bitrate;

	if (codec_config_resolve_codec_rc(vcfg->video0.rc_mode,
	    &pconf->rc_codec, &pconf->rc_mode) != 0)
		return -1;

	state->output_enabled = vcfg->outgoing.enabled ? 1 : 0;
	if (!vcfg->outgoing.server[0] && vcfg->outgoing.enabled) {
		fprintf(stderr,
			"ERROR: outgoing.enabled=true but outgoing.server is empty\n");
		return -1;
	}
	if (star6e_output_prepare(&pconf->output_setup, vcfg->outgoing.server,
	    vcfg->outgoing.stream_mode,
	    vcfg->outgoing.connected_udp) != 0)
		return -1;
	pconf->output_setup.allow_unix_encoder_stall =
		vcfg->outgoing.allow_unix_encoder_stall ? 1 : 0;

	/* Auto-cap exposure to frame period so the AE shutter never exceeds
	 * the frame period.  Without this, the AE converges on a long
	 * exposure that locks fps below the target. */
	pconf->exposure_cap_us = (pconf->sensor_framerate > 0) ?
		1000000 / pconf->sensor_framerate : 0;
	pconf->image_mirror    = vcfg->image.mirror ? 1 : 0;
	pconf->image_flip      = vcfg->image.flip   ? 1 : 0;
	pconf->vpe_level_3dnr  = vcfg->fpv.noise_level;
	pconf->oc_level        = vcfg->system.overclock_level;
	/* isp_bin_path is resolved later in bind_and_finalize_pipeline() once
	 * the live sensor name is known.  Resolved there (rather than in
	 * select_and_configure_sensor) so that reinit — which reuses the
	 * existing sensor and skips select_and_configure_sensor — also picks
	 * up SIGHUP-driven `isp.sensorBin` changes.  Leave empty here. */
	pconf->isp_bin_path[0] = '\0';

	pconf->sensor_cfg = pipeline_common_build_sensor_select_config(
		vcfg->sensor.index, vcfg->sensor.mode,
		pconf->sensor_width, pconf->sensor_height, pconf->sensor_framerate);
	pconf->sensor_cfg.image_mirror = pconf->image_mirror;
	pconf->sensor_cfg.image_flip   = pconf->image_flip;
	pconf->sensor_unlock = (SensorUnlockConfig){
		.enabled = vcfg->sensor.unlock_enabled ? 1 : 0,
		.cmd_id  = vcfg->sensor.unlock_cmd,
		.reg     = vcfg->sensor.unlock_reg,
		.value   = vcfg->sensor.unlock_value,
		.dir     = (MI_SNR_CustDir_e)vcfg->sensor.unlock_dir,
	};
	pconf->sensor_strategy = pconf->sensor_unlock.enabled ?
		sensor_unlock_strategy(&pconf->sensor_unlock) :
		sensor_default_strategy();

	return 0;
}

/* Phase 2: run sensor_select(), resolve actual dimensions, compute precrop and
 * populate the relevant pconf fields.  Logs the pipeline geometry summary. */
static int select_and_configure_sensor(Star6ePipelineState *state,
	Star6ePipelineConfig *pconf, const VencConfig *vcfg,
	SdkQuietState *sdk_quiet)
{
	uint32_t sensor_width;
	uint32_t sensor_height;
	uint16_t overscan_x;
	uint16_t overscan_y;
	int ret;

	sdk_quiet_begin(sdk_quiet);
	star6e_pipeline_pre_init_teardown();
	sdk_quiet_end(sdk_quiet);

	ret = sensor_select(&pconf->sensor_cfg, &pconf->sensor_strategy,
		&state->sensor);
	if (ret != 0)
		return ret;

	/* Expose sensor info to HTTP API for /api/v1/modes */
	venc_api_set_sensor_info((int)state->sensor.pad_id,
		state->sensor.mode_index, pconf->sensor_cfg.forced_pad);

	sensor_width  = state->sensor.plane.capt.width;
	sensor_height = state->sensor.plane.capt.height;
	if (state->sensor.mode.output.width > 0 &&
	    state->sensor.mode.output.height > 0 &&
	    (state->sensor.mode.output.width  < sensor_width ||
	     state->sensor.mode.output.height < sensor_height)) {
		if (vcfg->system.verbose)
			printf("> Note: MIPI frame %ux%u, usable output %ux%u (cropping overscan)\n",
				sensor_width, sensor_height,
				state->sensor.mode.output.width,
				state->sensor.mode.output.height);
		if (state->sensor.mode.output.width  < sensor_width)
			sensor_width  = state->sensor.mode.output.width;
		if (state->sensor.mode.output.height < sensor_height)
			sensor_height = state->sensor.mode.output.height;
	}

	pipeline_common_report_selected_fps("", pconf->sensor_framerate,
		&state->sensor);
	pconf->sensor_framerate = state->sensor.fps;
	pconf->venc_gop_size = pipeline_common_gop_frames(vcfg->video0.gop_size,
		pconf->sensor_framerate);
	/* Auto resolution: 0x0 means use sensor native dimensions */
	if (pconf->image_width == 0 || pconf->image_height == 0) {
		pconf->image_width = sensor_width;
		pconf->image_height = sensor_height;
	}
	pipeline_common_clamp_image_size("", sensor_width, sensor_height,
		&pconf->image_width, &pconf->image_height);

	/* Precrop is computed BEFORE the zoom override so the VIF→VPE window
	 * keeps the user-configured aspect ratio against the full sensor.
	 * Zoom is applied as a 1:1 sub-rect of the VPE output (not by shrinking
	 * the sensor read area). */
	pconf->precrop = star6e_pipeline_compute_precrop(sensor_width,
		sensor_height, pconf->image_width, pconf->image_height,
		vcfg->isp.keep_aspect);

	/* Approach C zoom: when zoom_pct ∈ (0, 1), shrink image_width/height
	 * to the crop dim.  VPE port output, SetPortCrop rect, and VENC channel
	 * all run at this smaller dim — no SCL upscale, no bandwidth pressure.
	 * The receiver sees the smaller resolution in SPS/PPS.  zoom_pct is
	 * MUT_RESTART so this only runs at start; live x/y pan stays in
	 * apply_zoom which only moves the rect inside VPE input. */
	if (vcfg->video0.zoom_pct > 0.0 && vcfg->video0.zoom_pct < 1.0) {
		uint32_t zw = pconf->image_width;
		uint32_t zh = pconf->image_height;
		star6e_compute_zoom_dim(pconf->image_width, pconf->image_height,
			vcfg->video0.zoom_pct, &zw, &zh);
		if (vcfg->system.verbose)
			printf("> Zoom: pct=%.3f → %ux%u (from image %ux%u)\n",
				vcfg->video0.zoom_pct, zw, zh,
				pconf->image_width, pconf->image_height);
		pconf->image_width  = zw;
		pconf->image_height = zh;
	}

	state->image_width  = pconf->image_width;
	state->image_height = pconf->image_height;
	overscan_x = (uint16_t)(((state->sensor.plane.capt.width  - sensor_width)
		/ 2) & ~1u);
	overscan_y = (uint16_t)(((state->sensor.plane.capt.height - sensor_height)
		/ 2) & ~1u);
	pconf->precrop.x += overscan_x;
	pconf->precrop.y += overscan_y;

	if (vcfg->system.verbose) {
		printf("> Starting star6e pipeline\n");
		printf("  - Sensor: %ux%u @ %u\n", sensor_width, sensor_height,
			pconf->sensor_framerate);
		if (overscan_x || overscan_y) {
			printf("  - MIPI  : %ux%u, cropped to %ux%u (offset %u,%u)\n",
				state->sensor.plane.capt.width,
				state->sensor.plane.capt.height,
				sensor_width, sensor_height, overscan_x, overscan_y);
		}
		printf("  - Image : %ux%u\n", pconf->image_width,
			pconf->image_height);
		if (pconf->precrop.w != sensor_width ||
		    pconf->precrop.h != sensor_height) {
			printf("  - Precrop: %ux%u -> %ux%u (VIF offset %u,%u)\n",
				sensor_width, sensor_height,
				pconf->precrop.w, pconf->precrop.h,
				pconf->precrop.x, pconf->precrop.y);
		}
		if (pconf->image_width  != pconf->precrop.w ||
		    pconf->image_height != pconf->precrop.h) {
			printf("  - VPE scaling: %ux%u -> %ux%u\n",
				pconf->precrop.w, pconf->precrop.h,
				pconf->image_width, pconf->image_height);
		}
		printf("  - 3DNR  : level %d\n", pconf->vpe_level_3dnr);
	}

	return 0;
}

/* IMU push callback: route frame-synced 6-axis samples into the shared
 * gyro ring (g_imu_ring).  The ring is the single home for IMU data —
 * the stabilization motion estimator reads it (see
 * star6e_stab_estimate_shift), and telemetry/sidecar consumers can read the
 * same ring.  Cheap (mutex + copy); a no-op until the ring is initialized in
 * the IMU bring-up block. */
static void star6e_pipeline_imu_push(void *ctx, const ImuSample *sample)
{
	(void)ctx;
	if (!g_imu_ring_ready || !sample)
		return;
	ImuRingSample rs = {
		.ts      = sample->ts,
		.gyro_x  = sample->gyro_x,
		.gyro_y  = sample->gyro_y,
		.gyro_z  = sample->gyro_z,
		.accel_x = sample->accel_x,
		.accel_y = sample->accel_y,
		.accel_z = sample->accel_z,
	};
	imu_ring_push(&g_imu_ring, &rs);
}

/* Framing-module host service: the pipeline-owned IMU ring, or NULL until
 * imu.enabled init has run.  Read-only consumers (a framing module's motion
 * estimator, telemetry) use this. */
ImuRing *star6e_pipeline_imu_ring(void)
{
	return g_imu_ring_ready ? &g_imu_ring : NULL;
}

/* Tracks whether CUS3A has been enabled in this MI_SYS lifetime.  Cleared
 * by star6e_pipeline_stop(), which is always followed by MI_SYS_Exit in
 * runner_teardown — so the next process start runs a true cold sequence
 * including CUS3A enable. */
static int g_isp_initialized = 0;

/* Tracks the last-loaded ISP bin path within this MI_SYS lifetime so we
 * skip redundant reloads.  Cleared by star6e_pipeline_stop() since the
 * following MI_SYS_Exit releases ISP driver state and the next start
 * needs to reload the bin against the fresh kernel. */
static char g_last_isp_bin_path[256] = {0};

int star6e_pipeline_load_isp_bin_live(const char *configured_path,
	const VencConfig *vcfg, const char *sensor_name,
	MI_SNR_PAD_ID_e pad_id, uint32_t sensor_framerate)
{
	char resolved[256];
	const char *configured;
	int ret;

	if (!vcfg)
		return -1;

	configured = (configured_path && *configured_path) ? configured_path : NULL;
	if (!pipeline_common_resolve_isp_bin(configured, sensor_name,
	    resolved, sizeof(resolved))) {
		fprintf(stderr,
			"ERROR: ISP bin reload — no readable bin for '%s' "
			"(sensor '%s')\n",
			configured ? configured : "(unset)",
			sensor_name ? sensor_name : "(unknown)");
		return -1;
	}

	if (strcmp(resolved, g_last_isp_bin_path) == 0) {
		printf("> ISP bin reload: %s already loaded, skipping\n",
			resolved);
		return 0;
	}

	/* sdk_quiet is only used to silence noisy SDK chatter during boot.
	 * On a live reload we accept the extra logging — passing NULL keeps
	 * the wrapper free of cross-module quiet-state plumbing. */
	ret = star6e_pipeline_load_isp_bin(resolved, NULL);
	if (ret != 0)
		return ret;

	snprintf(g_last_isp_bin_path, sizeof(g_last_isp_bin_path), "%s",
		resolved);

	/* Bin can reset MI_ISP_AE_SetExposureLimit to its own defaults; reapply
	 * the FPS cap so AE doesn't relock to a shutter longer than the frame
	 * period.  Same logic as bind_and_finalize_pipeline. */
	if (vcfg->video0.fps > 0)
		star6e_pipeline_cap_exposure_for_fps(vcfg->video0.fps,
			vcfg->isp.shutter_rule_180);

	/* The ISP bin's AE writes the bin's cold-boot shutter (often ~100 ms)
	 * directly to the sensor register, which the SetExposureLimit cap above
	 * doesn't touch.  Kick MI_SNR_SetFps to force the sensor driver to
	 * reconfigure timing — without this, swapping to a darker bin can lock
	 * the sensor at ~12 fps until reinit (cold_boot_fps_lock memory).  The
	 * supervisory AE enforcer only touches the exposure limit, so the
	 * pipeline owns this kick. */
	if (sensor_framerate > 0) {
		MI_SNR_SetFps(pad_id, sensor_framerate);
		/* SetFps can reprogram sensor timing, which on some sensors also
		 * resets the orientation registers.  This is a discrete runtime
		 * event (live ISP-bin reload), so just re-apply orientation once
		 * — cheap and safe (a single SetOrien is fine on all sensors). */
		if (vcfg->image.mirror || vcfg->image.flip)
			(void)MI_SNR_SetOrien(pad_id,
				(uint8_t)(vcfg->image.mirror ? 1 : 0),
				(uint8_t)(vcfg->image.flip   ? 1 : 0));
	}

	return 0;
}

/* Phase 3: assign port structs, issue all MI_SYS bind calls, init output,
 * video, ISP bin, exposure cap, cus3a, clocks, and audio.
 * pconf is non-const because we resolve isp_bin_path in here (we need
 * the live sensor name from state->sensor, which is only populated
 * after Phase 2). */
static int bind_and_finalize_pipeline(Star6ePipelineState *state,
	const VencConfig *vcfg, Star6ePipelineConfig *pconf,
	SdkQuietState *sdk_quiet)
{
	MI_U32 venc_device = 0;
	uint32_t bind_src_fps;
	uint32_t bind_dst_fps;
	int ret;

	state->vif_port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_VIF, .device = 0, .channel = 0, .port = 0 };
	state->vpe_port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_VPE, .device = 0, .channel = 0, .port = 0 };

	if (MI_VENC_GetChnDevid(state->venc_channel, &venc_device) != 0) {
		fprintf(stderr, "ERROR: MI_VENC_GetChnDevid failed\n");
		return -1;
	}
	state->venc_port = (MI_SYS_ChnPort_t){
		.module  = I6_SYS_MOD_VENC, .device  = venc_device,
		.channel = state->venc_channel, .port = 0 };

	if (!state->bound_vif_vpe) {
		ret = MI_SYS_BindChnPort2(&state->vif_port, &state->vpe_port,
			pconf->sensor_framerate, pconf->sensor_framerate,
			I6_SYS_LINK_REALTIME, 0);
		if (ret != 0) {
			fprintf(stderr, "ERROR: MI_SYS_Bind VIF->VPE failed %d\n", ret);
			return ret;
		}
		state->bound_vif_vpe = 1;

		/* A new VPE channel was just created (first start or AR-change
		 * reinit). The ISP channel initialises asynchronously after
		 * MI_VPE_CreateChannel.  Poll here before the bin load and
		 * cap_exposure_for_fps touch the ISP, so the kernel ISP driver
		 * does not emit "IspApiGet channel not created" errors. */
		star6e_pipeline_wait_isp_channel();
	}

	/* Cap exposure BEFORE binding VPE→VENC.  The AE starts running as
	 * soon as VIF→VPE is bound (above).  Without an early cap the AE
	 * can converge on a shutter time longer than the frame period during
	 * the ISP bin load + CUS3A init window, locking the pipeline at a
	 * lower framerate until reinit. */
	star6e_pipeline_cap_exposure_for_fps(pconf->sensor_framerate,
		vcfg->isp.shutter_rule_180);

	/* VPE port arbiter: port0 now carries "main"; port1 starts free.  Framing
	 * and detect claim port1 below (mutually exclusive on the single tap). */
	star6e_vpe_ports_begin();

	g_framing = star6e_framing_select(vcfg);
	if (g_framing) {
		/* Framing module owns the VPE port0 → VENC path.  The "stab" preset
		 * hardware-crops the stab window straight to a VENC bind (zero-copy)
		 * while a tiny port1 tap feeds the detector, setting
		 * state->bound_vpe_venc=1 for the standard teardown. */
		bind_src_fps = state->sensor.mode.maxFps ?
			state->sensor.mode.maxFps : pconf->sensor_framerate;
		bind_dst_fps = vcfg->video0.fps;
		if (bind_dst_fps == 0 || bind_dst_fps > bind_src_fps)
			bind_dst_fps = bind_src_fps;
		/* Deliver the TRUE sensor rate to VENC (e.g. ~143 for the 144 mode) so
		 * the encoder actually outputs it.  The RC fpsNum is separately capped
		 * to STAR6E_VENC_INPUT_FPS_MAX below — see venc_fps. */

		ret = g_framing->setup_ports(state, bind_src_fps, bind_dst_fps);
		if (ret != 0) {
			MI_SYS_UnBindChnPort(&state->vif_port, &state->vpe_port);
			state->bound_vif_vpe = 0;
			return ret;
		}
		ret = g_framing->start();
		if (ret != 0) {
			g_framing->stop();   /* tears down the tap / partial state */
			if (state->bound_vpe_venc) {
				MI_SYS_UnBindChnPort(&state->vpe_port,
					&state->venc_port);
				state->bound_vpe_venc = 0;
			}
			MI_SYS_UnBindChnPort(&state->vif_port, &state->vpe_port);
			state->bound_vif_vpe = 0;
			return ret;
		}
		/* Seed the AE meter to the stabilized crop window.  The ISP-ready
		 * wait above has already fired star6e_ae_crop_mark_ready(), so
		 * this emits immediately rather than queueing. */
		g_framing->apply_ae_crop();
		/* Reserve port1 for the stab motion tap so the detector below is
		 * refused it (stab-fill rides port0 and claims nothing). */
		if (g_framing->uses_vpe_port1)
			(void)star6e_vpe_port1_claim(g_framing->preset_name);
	} else {
		bind_src_fps = state->sensor.mode.maxFps ?
			state->sensor.mode.maxFps : pconf->sensor_framerate;
		bind_dst_fps = vcfg->video0.fps;
		if (bind_dst_fps == 0 || bind_dst_fps > bind_src_fps)
			bind_dst_fps = bind_src_fps;
		/* Deliver the TRUE sensor rate to VENC (e.g. ~143 for the 144 mode) so
		 * the encoder actually outputs it.  The RC fpsNum is separately capped
		 * to STAR6E_VENC_INPUT_FPS_MAX below — see venc_fps. */

		ret = MI_SYS_BindChnPort2(&state->vpe_port, &state->venc_port,
			bind_src_fps, bind_dst_fps, I6_SYS_LINK_FRAMEBASE, 0);
		if (ret != 0) {
			fprintf(stderr, "ERROR: MI_SYS_Bind VPE->VENC failed %d\n", ret);
			MI_SYS_UnBindChnPort(&state->vif_port, &state->vpe_port);
			state->bound_vif_vpe = 0;
			return ret;
		}
		state->bound_vpe_venc = 1;
		MI_SYS_SetChnOutputPortDepth(&state->venc_port, 1, 3);
	}

	if (vcfg->detect.enabled)
		(void)star6e_pipeline_detect_start(state, vcfg);

	/* Arm the overlay-free luma tap on VPE port1.  This does NOT open the
	 * port: an always-on tap costs ~186 MB/s of SCL write bandwidth at
	 * 1080p60 and holds port1 against stab/detect for the whole run, all for
	 * nothing when no scan is in progress.  The port is claimed and enabled
	 * only for the duration of a scan window, and never per request. */
	if (!state->luma_tap)
		state->luma_tap = star6e_luma_tap_create();
	if (!state->luma_tap) {
		fprintf(stderr, "[luma-tap] context allocation failed\n");
	} else {
		star6e_luma_tap_configure(state->luma_tap, vcfg,
			state->image_width, state->image_height);
	}

	/* Bring up the JPEG snapshot subsystem on the same VPE source port the
	 * main channel just bound to.  Failure is non-fatal — /api/v1/snapshot.jpg
	 * just serves 503 if init fails.  Config from venc.json snapshot.*
	 * section; width=0/height=0 inherits main stream dimensions. */
	{
		venc_jpeg_set_source(&state->vpe_port);
		const VencConfigSnapshot *snap = &vcfg->snapshot;
		VencJpegConfig jcfg = {
			.width   = snap->width  ? snap->width  : state->image_width,
			.height  = snap->height ? snap->height : state->image_height,
			.quality = snap->quality,
			.channel = snap->channel,
			.enabled = snap->enabled,
		};
		/* The JPEG channel binds 1:N alongside the encoder on port0 (idle
		 * until a snapshot is pulled), so it is a port0 consumer, not a
		 * second tap.  init() is always called (it sets internal state even
		 * when disabled); report the tap only when it actually came up. */
		int jpeg_rc = venc_jpeg_init(&jcfg);
		if (jcfg.enabled && jpeg_rc == 0)
			star6e_vpe_port0_set("jpeg", true);
	}

	if (star6e_output_init(&state->output, &pconf->output_setup) != 0) {
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
		star6e_output_teardown(&state->output);
		/* The teardown resets transport policy along with transport state,
		 * and a later live recovery reads those fields back out of the
		 * output -- so leaving them cleared would silently apply a DIFFERENT
		 * policy than the config asks for once the operator sets a working
		 * destination.  Star6E loses connected-UDP as well as the Unix
		 * stall option; Maruko loses the stall option.  Re-seed them from
		 * the same source init would have. */
		state->output.requested_connected_udp =
			pconf->output_setup.requested_connected_udp;
		state->output.allow_unix_encoder_stall =
			pconf->output_setup.allow_unix_encoder_stall;
		fprintf(stderr, "ERROR: outgoing.server could not be brought up; "
			"starting with NO video output so the craft stays reachable -- "
			"set outgoing.server over the API to recover\n");
	}

	star6e_video_init(&state->video, vcfg, pconf->sensor_framerate,
		&state->output);

	/* Resolve isp.sensorBin: configured path takes precedence; if empty
	 * or unreadable, fall back to /etc/sensors/<sensor>.bin keyed off
	 * the live sensor name.  Resolved here (rather than in
	 * select_and_configure_sensor) so reinit also picks up SIGHUP-driven
	 * isp.sensorBin changes — reinit reuses the existing sensor and
	 * skips select_and_configure_sensor. */
	pipeline_common_resolve_isp_bin(
		vcfg->isp.sensor_bin[0] ? vcfg->isp.sensor_bin : NULL,
		state->sensor.plane.sensName,
		pconf->isp_bin_path, sizeof(pconf->isp_bin_path));

	/* Load ISP bin on first start, or when the bin path changes.  Skipping
	 * redundant reloads avoids the vendor AE resetting the sensor shutter
	 * register back to its default (~10000us on IMX335) on every SIGHUP /
	 * Save&Restart, which would otherwise lock the sensor VTS at ~100 fps.
	 * The kernel ISP driver accepts repeated loads but each one disturbs
	 * the running sensor timing. */
	if (pconf->isp_bin_path[0] &&
	    strcmp(pconf->isp_bin_path, g_last_isp_bin_path) != 0) {
		ret = star6e_pipeline_load_isp_bin(pconf->isp_bin_path, sdk_quiet);
		if (ret != 0) {
			fprintf(stderr, "WARNING: ISP bin load failed; continuing with default ISP settings\n");
		} else {
			snprintf(g_last_isp_bin_path, sizeof(g_last_isp_bin_path),
				"%s", pconf->isp_bin_path);
		}
	}
	if (!g_isp_initialized) {
		star6e_pipeline_enable_cus3a(sdk_quiet);
		g_isp_initialized = 1;
	}
	/* Userspace AWB: the ISP-internal algorithm does not converge here
	 * (see star6e_awb.h).  Paused up front when the operator asked for a
	 * fixed colour temperature — ct_manual keeps AWB on the ISP-internal
	 * path so the existing MI_ISP_AWB_SetCTMwbAttr apply still works. */
	if (vcfg->isp.awb_fps > 0 &&
	    star6e_awb_start(vcfg->isp.awb_fps) == 0) {
		int manual = strcmp(vcfg->isp.awb_mode, "auto") != 0;

		star6e_awb_set_paused(manual);
		/* Hand AWB over only when this loop will actually drive it.
		 * awbFps=0, a failed start, or ct_manual all leave AWB with the
		 * ISP-internal algorithm, which is also what the colour-
		 * temperature apply path needs. */
		star6e_pipeline_set_awb_userspace(!manual);
	}
	/* Reapply exposure cap after ISP bin load — the bin may reset AE
	 * limits to its own defaults which could exceed the frame period. */
	star6e_pipeline_cap_exposure_for_fps(pconf->sensor_framerate,
		vcfg->isp.shutter_rule_180);

	/* Cold-boot fix: the ISP bin's AE may initialize the sensor at a shutter
	 * exceeding the frame period.  SetExposureLimit only constrains the AE
	 * algorithm, not the physical sensor register.  MI_SNR_SetFps forces the
	 * sensor driver to reconfigure timing, resetting the shutter to fit the
	 * frame period.  The supervisory AE enforcer only touches the exposure
	 * limit, so the pipeline owns this kick. */
	if (pconf->exposure_cap_us > 0 &&
	    pconf->sensor_framerate > 0) {
		MI_SNR_SetFps(state->sensor.pad_id, pconf->sensor_framerate);
	}

	star6e_pipeline_set_hw_clocks(pconf->oc_level, vcfg->system.verbose);

	if (star6e_output_is_shm(&state->output) &&
	    vcfg->outgoing.audio_port == 0) {
		printf("[audio] Disabled in SHM mode (audioPort=0 has no socket to share)\n");
	} else {
		star6e_audio_init(&state->audio, vcfg, &state->output);
	}

	/* IMU */
	if (vcfg->imu.enabled) {
		if (!g_imu_ring_ready) {
			imu_ring_init(&g_imu_ring);
			g_imu_ring_ready = 1;
		}
		ImuConfig imu_cfg = {
			.i2c_device = vcfg->imu.i2c_device,
			.i2c_addr = vcfg->imu.i2c_addr,
			.sample_rate_hz = vcfg->imu.sample_rate_hz,
			.gyro_range_dps = vcfg->imu.gyro_range_dps,
			.cal_file = vcfg->imu.cal_file,
			.cal_samples = vcfg->imu.cal_samples,
			.push_fn = star6e_pipeline_imu_push,
			.push_ctx = state,
			.use_thread = 0,
		};
		state->imu = imu_init(&imu_cfg);
		if (state->imu) {
			imu_start(state->imu);
		} else {
			fprintf(stderr, "WARNING: IMU init failed, continuing without IMU\n");
		}
	}

	/* Debug OSD.  Canvas dim is the encoded frame dim; on Star6E RGN attaches
	 * at the VPE port output (post-SCL crop), so the canvas is 1:1 with the
	 * encoded frame.  HW-crop stab is no different (port0 outputs the cropped
	 * encoded dim).  Vehicle-local diagnostic only.
	 *
	 * D16 note: under framing=stab-fill the OSD rides the stabilized content
	 * (it composites on VPE port0, before the manual-drain compose shifts the
	 * frame) — a known cosmetic limitation of this dev overlay.  For a
	 * screen-fixed OSD on the stabilized stream use waybeam_hub's osd_render,
	 * which composites at the SCL stage (post-shift).  The two share MI_RGN
	 * (a single global RGN device), so they are mutually exclusive — run one
	 * or the other, not both. */
	if (vcfg->debug.show_osd) {
		state->debug_osd = debug_osd_create(state->image_width,
			state->image_height, &state->vpe_port);
		if (!state->debug_osd) {
			fprintf(stderr, "WARNING: debug OSD requested but MI_RGN unavailable\n");
		}
	}

	return 0;
}

/* Drain buffered frames from a VENC channel (non-blocking).
 * Relieves VPE backpressure before StopRecvPic to prevent D-state hangs.
 * Drains until no frames remain or max_ms elapsed — time-bounded to
 * handle continuous 120fps production during teardown. */
static void drain_venc_channel(MI_VENC_CHN chn, int max_ms,
	const char *label)
{
	struct timespec start;
	int drained = 0;

	clock_gettime(CLOCK_MONOTONIC, &start);

	for (;;) {
		MI_VENC_Stat_t stat = {0};
		MI_VENC_Stream_t stream = {0};
		struct timespec now;
		long long elapsed_ms;

		if (MI_VENC_Query(chn, &stat) != 0 || stat.curPacks == 0)
			break;

		stream.count = stat.curPacks;
		stream.packet = calloc(stat.curPacks, sizeof(MI_VENC_Pack_t));
		if (!stream.packet)
			break;

		if (MI_VENC_GetStream(chn, &stream, 0) != 0) {
			free(stream.packet);
			break;
		}

		MI_VENC_ReleaseStream(chn, &stream);
		free(stream.packet);
		drained++;

		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed_ms = (long long)(now.tv_sec - start.tv_sec) * 1000LL +
			(long long)(now.tv_nsec - start.tv_nsec) / 1000000LL;
		if (elapsed_ms >= max_ms)
			break;
	}

	if (drained > 0)
		printf("> Drained %d frames from VENC %s before stop\n",
			drained, label);
}

void star6e_pipeline_stop(Star6ePipelineState *state)
{
	if (!state)
		return;

	star6e_awb_stop();

	/* Clear userspace persist flags.  runner_teardown follows with
	 * MI_SYS_Exit, and the next pipeline_start always runs in a fresh
	 * process (SIGHUP-respawn forks a successor), so the kernel
	 * ISP/CUS3A state is genuinely cold on the next start.  Skipping
	 * these clears would leave stale "already initialised" flags that
	 * bypass the very work the fresh kernel state expects us to redo. */
	g_isp_initialized = 0;
	g_last_isp_bin_path[0] = '\0';
	g_cus3a_handoff_done = 0;
	venc_api_clear_active_precrop();
	star6e_pipeline_clear_zoom_status();
	star6e_pan_ramp_stop();
	/* AE crop: the SDK rejected our "restore full-frame" call on this BSP,
	 * so we never emit it.  Cycle the ready flag so the next start
	 * re-arms via the ISP-ready hook, and re-seed the cache so the next
	 * sub-rect (if any) is unconditionally re-emitted. */
	g_star6e_ae_crop_ready = 0;
	g_star6e_ae_crop_last.crop_x = 0;
	g_star6e_ae_crop_last.crop_y = 0;
	g_star6e_ae_crop_last.crop_w = 1023;
	g_star6e_ae_crop_last.crop_h = 1023;

	/* Clear IntraRefresh status snapshot — the channel it described is
	 * about to be destroyed, so /api/v1/intra/status should not keep
	 * reporting enabled=true until the next pipeline_start runs. */
	pthread_mutex_lock(&g_intra_status_mutex);
	memset(&g_intra_status, 0, sizeof(g_intra_status));
	pthread_mutex_unlock(&g_intra_status_mutex);

	/* The recording thread must keep consuming ch1 frames through
	 * the ENTIRE teardown sequence.  At 120fps, the 12-frame ch1
	 * buffer fills in ~100ms — any gap in consumption causes VPE
	 * backpressure → kernel D-state → StopRecvPic hangs.
	 *
	 * Sequence: teardown peripherals → drain both channels →
	 * StopRecvPic (thread still draining ch1) → signal thread
	 * to stop → join thread. */

	/* Stop + destroy IMU.  The push callback is now a stub (EIS was
	 * removed; see HISTORY 0.8.0) so order vs other teardown steps
	 * doesn't matter, but keeping the stop-then-destroy split lets a
	 * future telemetry consumer slot in without rework. */
	if (state->imu) {
		imu_stop(state->imu);
		imu_destroy(state->imu);
		state->imu = NULL;
	}
	if (state->debug_osd) {
		debug_osd_destroy(state->debug_osd);
		state->debug_osd = NULL;
	}

	star6e_audio_teardown(&state->audio);
	star6e_output_teardown(&state->output);
	if (state->dual)
		star6e_output_teardown(&state->dual->output);

	/* Stop the recording thread FIRST.  The thread's GetStream and
	 * the main thread's UnBindChnPort contend for the same kernel
	 * VPE lock — running them concurrently causes intermittent
	 * deadlock (D-state).  By joining the thread first, we ensure
	 * no concurrent VENC consumers exist during unbind/stop.
	 *
	 * The thread checks rec_running at the top of each loop and
	 * uses non-blocking GetStream (timeout=0) when g_running==0,
	 * so it exits within one iteration (~1ms). */
	if (state->dual && state->dual->rec_started) {
		state->dual->rec_running = 0;
		pthread_join(state->dual->rec_thread, NULL);
		state->dual->rec_started = 0;
		printf("> Dual recording thread joined\n");
	}

	/* Tear down JPEG snapshot channel first — it's bound to the same
	 * VPE port we're about to unbind, and its UnBindChnPort/DestroyChn
	 * must run while the SDK still holds a consistent view of the VPE
	 * source.  Idempotent; safe even if init was skipped or failed. */
	venc_jpeg_shutdown();

	/* Stop the IPU detection reader (joins before DisablePort(0,1), MMU-safe)
	 * and tear down its backend.  Idempotent; no-op when detection inactive.
	 * Detect and framing are mutually exclusive, so at most one of these
	 * two blocks does work. */
	star6e_ipu_yolo_stop(state);

	/* Stop the port1 luma tap: parks + joins its reader before DisablePort,
	 * same MMU-safety rule as the detect tap.  Idempotent. */
	star6e_luma_tap_stop(state->luma_tap);

	/* Stop the stabilization detector thread: park the detector, disable the
	 * tiny port1 tap, and join the thread; port0 stays a normal VPE→VENC bind
	 * torn down by the standard unbind path below (state->bound_vpe_venc set).
	 * Idempotent: no-op when stab was not started (incl. the static-crop
	 * degrade with no detector). */
	if (g_framing) {
		g_framing->stop();
		g_framing = NULL;
	}

	/* Release the VPE port arbiter: port1 owner (stab/detect) is freed and the
	 * runtime.vpe_taps observability block is cleared for the stopped pipeline. */
	star6e_vpe_ports_end();

	/* MI teardown order: StopRecvPic each VENC consumer BEFORE unbinding
	 * its input port.  The previous Star6E order unbound VPE→VENC first and
	 * only then stopped VENC, leaving the kernel SDK still encoding/flushing
	 * a buffered frame out of a port userspace had just ripped out — VENC
	 * (MMU client 0x15) then reads a freed VPE buffer:
	 * `_MI_SYS_MMU_Callback Status=0x2 IsWrite=0` storms into a hardware
	 * watchdog reset on the ~2nd rapid respawn (reproduced on master from a
	 * cold boot, so it predates the stab/framing work).  Maruko hit the same
	 * root cause as a page fault in MI_SYS_IMPL_FlushInputPortTasks and was
	 * fixed to stop-first — see maruko_pipeline_teardown_graph(); Star6E was
	 * never given that fix until now.  StopRecvPic is a soft pause and does
	 * not deadlock while still bound.  Sequence: StopRecvPic → drain output →
	 * unbind VPE→VENC → unbind VIF→VPE → destroy VENC → stop VPE/VIF/sensor. */
	if (state->dual)
		MI_VENC_StopRecvPic(state->dual->channel);
	MI_VENC_StopRecvPic(state->venc_channel);

	/* Drain the last buffered frames that StopRecvPic let flow out. */
	drain_venc_channel(state->venc_channel, 150, "ch0");
	if (state->dual)
		drain_venc_channel(state->dual->channel, 150, "ch1-post");

	/* Unbind VPE→VENC now that the consumer is stopped. */
	if (state->dual && state->dual->bound) {
		MI_SYS_UnBindChnPort(&state->vpe_port, &state->dual->port);
		state->dual->bound = 0;
	}
	if (state->bound_vpe_venc) {
		MI_SYS_UnBindChnPort(&state->vpe_port, &state->venc_port);
		state->bound_vpe_venc = 0;
	}

	if (state->bound_vif_vpe) {
		MI_SYS_UnBindChnPort(&state->vif_port, &state->vpe_port);
		state->bound_vif_vpe = 0;
	}

	/* Destroy channels */
	if (state->dual) {
		venc_api_dual_unregister();
		MI_VENC_DestroyChn(state->dual->channel);
		/* Release the dual sidecar fd before freeing the struct.  Without
		 * this, every dual-stream stop leaks the ch1 listener; usually
		 * masked by fork+exec respawn but a clean process-exit path or
		 * future in-process reinit would accumulate it. */
		star6e_video_reset(&state->dual->video);
		free(state->dual->stream_packs);
		free(state->dual);
		state->dual = NULL;
	}
	MI_VENC_DestroyChn(state->venc_channel);
	free(state->stream_packs);
	state->stream_packs = NULL;
	state->stream_packs_cap = 0;
	star6e_pipeline_stop_vpe();
	star6e_pipeline_stop_vif();
	star6e_pipeline_stop_sensor(state->sensor.pad_id);
}


/* flatten: force GCC to inline all static callees into this function.
 * The SigmaStar I6E ISP driver depends on the monolithic stack layout
 * that results from inlining bind_and_finalize_pipeline() and
 * prepare_pipeline_config().  When these are emitted as separate functions
 * (as happens with -Os when they have multiple call-sites), the VPE→ISP
 * channel init fails (MI_ISP_IQ_GetParaInitStatus returns error 6). */
__attribute__((flatten))
int star6e_pipeline_start(Star6ePipelineState *state, const VencConfig *vcfg,
	SdkQuietState *sdk_quiet)
{
	Star6ePipelineConfig pconf;
	uint32_t venc_fps;
	int ret;

	if (!state || !vcfg)
		return -1;

	star6e_pipeline_reset(state);
	star6e_pipeline_clear_zoom_status();
	star6e_framing_register_builtins();

	if (prepare_pipeline_config(state, vcfg, &pconf) != 0)
		return -1;

	ret = select_and_configure_sensor(state, &pconf, vcfg, sdk_quiet);
	if (ret != 0)
		return ret;

	state->active_precrop = pconf.precrop;
	venc_api_set_active_precrop(pconf.precrop.x, pconf.precrop.y,
		pconf.precrop.w, pconf.precrop.h);

	ret = star6e_pipeline_start_vif(&state->sensor, &pconf.precrop);
	if (ret != 0)
		goto fail_sensor;

	ret = star6e_pipeline_start_vpe(&state->sensor, &pconf.precrop,
		pconf.image_width, pconf.image_height,
		pconf.image_mirror, pconf.image_flip,
		pconf.vpe_level_3dnr, sdk_quiet);
	if (ret != 0)
		goto fail_vif;

	/* image_width/height aren't stored on state until bind; pre-populate so
	 * the initial zoom apply (and subsequent live updates before first
	 * frame) sees correct port-output dims. */
	state->image_width = pconf.image_width;
	state->image_height = pconf.image_height;

	/* Framing mode: the video0.framing preset expands into either stab
	 * (stab_crop_pct/recenter) or zoom (zoom_pct).  They are mutually
	 * exclusive, so exactly one branch below runs.
	 *
	 * Stab path (HW-crop): VPE port0 hardware-crops the stab window into a
	 * VENC bind; a tiny port1 tap feeds IVE shift detection.  If the BSP
	 * rejects the tap, port0 stays bound at a static centre crop (no
	 * detector).  VENC ch0 is created at the crop dim.  zoomX/zoomY pick the
	 * crop center so the stabilized stream pans live.  Zoom path: the pan-ramp
	 * thread owns
	 * x/y at the configured zoom_pct.  When framing is off, zoom_pct is 0 and
	 * the pan-ramp runs at full image. */
	g_framing = star6e_framing_select(vcfg);
	if (g_framing) {
		/* The framing module computes its geometry (clamping the source as
		 * needed) and returns the encode dims the pipeline sizes VENC to. */
		uint32_t enc_w = pconf.image_width, enc_h = pconf.image_height;
		if (g_framing->prepare(vcfg, pconf.image_width, pconf.image_height,
		    &enc_w, &enc_h) == 0) {
			pconf.image_width = enc_w;
			pconf.image_height = enc_h;
			state->image_width = enc_w;
			state->image_height = enc_h;
		}
	} else {
		(void)star6e_pan_ramp_start(state, vcfg->video0.zoom_pct,
			vcfg->video0.zoom_x, vcfg->video0.zoom_y);
	}

	state->venc_channel = 0;
	/* Encoder rate control must budget for the frame rate that actually
	 * reaches VENC — the VPE->VENC framebase bind's delivered rate =
	 * min(mode.maxFps, video0.fps), then capped to the VENC input ceiling.
	 * Do NOT clamp to pconf.sensor_framerate: a SetFps fallback can record
	 * state->sensor.fps well below the mode's real (fixed-timing) rate.  And
	 * do NOT let it exceed STAR6E_VENC_INPUT_FPS_MAX: the VENC rejects >120
	 * and resets to 30, so the 1600x900@144 mode must encode at 120 (the bind
	 * below drops the sensor's ~143 down to match).  Both mismatches otherwise
	 * make CBR overshoot the configured bitrate several-fold. */
	{
		uint32_t delivered = state->sensor.mode.maxFps ?
			state->sensor.mode.maxFps : pconf.sensor_framerate;
		uint32_t delivered_true = delivered;
		if (delivered > STAR6E_VENC_INPUT_FPS_MAX)
			delivered = STAR6E_VENC_INPUT_FPS_MAX;
		venc_fps = vcfg->video0.fps;
		if (venc_fps == 0 || venc_fps > delivered)
			venc_fps = delivered;
		/* Exact-CBR: the bind delivers delivered_true frames while the
		 * RC budgets at venc_fps (capped 120) — scale the encoder budget
		 * by venc_fps/delivered_true so the wire rate lands on the
		 * configured bitrate (x120/144 for the 144 mode).  Mirrors
		 * rc_compensate_kbps() in star6e_controls.c for the live paths. */
		if (vcfg->video0.fps && vcfg->video0.fps < delivered_true)
			delivered_true = vcfg->video0.fps;
		if (delivered_true > venc_fps && venc_fps > 0)
			pconf.venc_max_rate = (uint32_t)((uint64_t)
				pconf.venc_max_rate * venc_fps / delivered_true);
	}

	/* GOP frame count must track the encoder's real fps (venc_fps), not
	 * pconf.sensor_framerate which may hold the stale SetFps-fallback value;
	 * otherwise the I-frame interval desyncs on modes whose delivered rate
	 * exceeds the recorded sensor.fps (e.g. GOP=60 instead of 288 @144). */
	pconf.venc_gop_size = pipeline_common_gop_frames(vcfg->video0.gop_size,
		venc_fps);

	/* IntraRefresh auto-GOP: when intraRefreshMode != off and the user did
	 * not pin gopSize, override the GOP frame count so each IDR aligns with
	 * one full GDR pass. */
	{
		IntraRefreshDerived ir;
		IntraRefreshMode mode = star6e_pipeline_intra_refresh_derive(
			vcfg, pconf.image_height, venc_fps, pconf.rc_codec, &ir);
		if (mode != INTRA_MODE_OFF && !ir.gop_overridden && ir.gop_frames > 0)
			pconf.venc_gop_size = ir.gop_frames;
	}

	ret = star6e_pipeline_start_venc(pconf.image_width, pconf.image_height,
		pconf.venc_max_rate, venc_fps, pconf.venc_gop_size,
		pconf.rc_codec, pconf.rc_mode,
		vcfg, &state->venc_channel);
	if (ret != 0)
		goto fail_vpe;

	/* IntraRefresh — opt-in via video0.intra_refresh.  Failure is logged
	 * but not fatal: stream still works without rolling refresh. */
	(void)star6e_pipeline_apply_intra_refresh(state->venc_channel, vcfg,
		pconf.image_height, venc_fps, pconf.rc_codec);

	/* SVC-T reference pyramid (refPred) is applied inside
	 * star6e_pipeline_start_venc() before StartRecvPic — the SDK
	 * requires that ordering or the call silently no-ops. */

	ret = bind_and_finalize_pipeline(state, vcfg, &pconf, sdk_quiet);
	if (ret != 0)
		goto fail_venc;

	/* Must run AFTER bind_and_finalize_pipeline(): it calls
	 * star6e_output_init() -> star6e_output_reset(), which memsets the
	 * whole output struct and would otherwise zero these GDR/SVC-T
	 * tagging fields. */
	{
		Star6eIntraRefreshStatus ir_status;
		Star6eRefPredStatus ref_status;
		uint32_t clen;

		star6e_pipeline_intra_refresh_status(&ir_status);
		star6e_pipeline_ref_pred_status(&ref_status);
		state->output.gdr_active = ir_status.active && ir_status.apply_ok;
		state->output.svct_active = ref_status.active && ref_status.apply_ok;
		clen = (state->output.gdr_active && ir_status.total_rows &&
			ir_status.effective_lines_per_p)
			? (ir_status.total_rows + ir_status.effective_lines_per_p - 1) /
				ir_status.effective_lines_per_p
			: 0;
		state->output.gdr_cycle_len = clen > 255 ? 255 : (uint8_t)clen;
		state->output.gdr_counter = 0;
	}

	return 0;

fail_venc:
	/* bind_and_finalize_pipeline() called star6e_vpe_ports_begin(); release it
	 * so a failed bring-up does not leave a stale runtime.vpe_taps / port1
	 * owner behind (no-op when begin() had not run). */
	star6e_vpe_ports_end();
	star6e_pipeline_stop_venc(state->venc_channel);
fail_vpe:
	star6e_pipeline_stop_vpe();
fail_vif:
	star6e_pipeline_stop_vif();
fail_sensor:
	star6e_pipeline_stop_sensor(state->sensor.pad_id);
	return ret ? ret : -1;
}

int star6e_pipeline_start_dual(Star6ePipelineState *state,
	uint32_t bitrate, uint32_t fps, double gop_sec,
	const char *mode, const char *server)
{
	Star6eDualVenc *d;
	MI_U32 dev = 0;
	MI_VENC_CHN ch1 = 1;
	uint32_t sensor_fps;
	uint32_t gop;
	int ret;

	if (!state || !mode)
		return -1;

	/* Stabilization owns VPE port0 via a manual GetBuf drain; a dual VENC
	 * bind on the same source port would contend with that drain (and dual
	 * would inherit the crop-overridden image dims).  The two are mutually
	 * exclusive for the *second* channel — so we skip the dual ch1 here but
	 * the caller still starts the ts_recorder on the stabilized main ch0
	 * (runtime's generic record block).  Net effect: recording is NOT
	 * disabled, it is downgraded to single-channel — one .ts of the
	 * stabilized main stream, at the main-stream bitrate rather than
	 * record.bitrate. */
	if (g_framing && g_framing->active && g_framing->active()) {
		fprintf(stderr, "[waybeam] WARNING: dual recording downgraded to "
			"single-channel while image stabilization is active "
			"(video0.framing): no separate ch1 — recording the "
			"stabilized main stream (ch0) at its bitrate, not "
			"record.bitrate\n");
		return 0;
	}

	sensor_fps = state->sensor.mode.maxFps;
	if (sensor_fps == 0) sensor_fps = 30;
	if (fps == 0) fps = sensor_fps;
	if (fps > sensor_fps) fps = sensor_fps;
	if (bitrate == 0) bitrate = 8000;
	gop = (uint32_t)(gop_sec * fps + 0.5);
	if (gop < 1) gop = fps;  /* default 1-second GOP */

	d = calloc(1, sizeof(*d));
	if (!d)
		return -1;

	d->channel = ch1;
	d->bitrate = bitrate;
	d->fps = fps;
	d->gop = gop;
	snprintf(d->mode, sizeof(d->mode), "%s", mode);
	if (server)
		snprintf(d->server, sizeof(d->server), "%s", server);

	/* Dual ch1 does not see vcfg here — refPred not applied to ch1.  The
	 * secondary stream is typically a low-bandwidth recorder feed where
	 * SVC-T is less impactful; revisit if needed. */
	ret = star6e_pipeline_start_venc(state->image_width,
		state->image_height, bitrate, fps, gop,
		PT_H265, 3 /* CBR */, NULL, &d->channel);
	if (ret != 0) {
		fprintf(stderr, "WARNING: dual VENC ch1 create failed (%d), "
			"falling back to mirror mode\n", ret);
		free(d);
		return -1;
	}

	MI_VENC_GetChnDevid(d->channel, &dev);
	d->port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_VENC, .device = dev,
		.channel = d->channel, .port = 0 };

	ret = MI_SYS_BindChnPort2(&state->vpe_port, &d->port,
		sensor_fps, fps, I6_SYS_LINK_FRAMEBASE, 0);
	if (ret != 0) {
		fprintf(stderr, "WARNING: dual VENC bind failed (%d), "
			"falling back to mirror mode\n", ret);
		star6e_pipeline_stop_venc(d->channel);
		free(d);
		return -1;
	}
	d->bound = 1;
	/* Deep buffer for ch1: the recording thread can stall on SD card
	 * writes (flash GC takes up to 500ms).  At 120fps, a 64-frame
	 * buffer gives ~533ms of headroom before VPE backpressure. */
	MI_SYS_SetChnOutputPortDepth(&d->port, 8, 56);

	state->dual = d;
	/* ch1 binds 1:N alongside the encoder on port0 — another port0 consumer. */
	star6e_vpe_port0_set("record", true);
	printf("> Dual VENC: ch1 = %u kbps %u fps (mode: %s)\n",
		bitrate, fps, mode);
	return 0;
}

void star6e_pipeline_stop_dual(Star6ePipelineState *state)
{
	Star6eDualVenc *d;

	if (!state || !state->dual)
		return;

	d = state->dual;
	star6e_output_teardown(&d->output);
	/* Stop receiving first, then unbind. Reverse order deadlocks
	 * because UnBind waits for the pipeline to drain while VPE
	 * is still feeding frames to VENC. */
	MI_VENC_StopRecvPic(d->channel);
	if (d->bound) {
		MI_SYS_UnBindChnPort(&state->vpe_port, &d->port);
		d->bound = 0;
	}
	MI_VENC_DestroyChn(d->channel);
	/* Mirrors the close-before-free in star6e_pipeline_stop above:
	 * free(d) without releasing d->video.sidecar.fd would leak the ch1
	 * sidecar listener.  star6e_pipeline_stop_dual is the live toggle-off
	 * path (no respawn), so the SOCK_CLOEXEC safety net does not apply
	 * here. */
	star6e_video_reset(&d->video);
	free(d->stream_packs);
	free(d);
	state->dual = NULL;
	star6e_vpe_port0_set("record", false);
}
