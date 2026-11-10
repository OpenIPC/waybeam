#include "star6e_controls.h"

#include "idr_rate_limit.h"
#include "output_socket.h"
#include "pipeline_common.h"
#include "star6e_audio.h"
#include "star6e_awb.h"
#include "star6e_cus3a.h"
#include "star6e_ipu_yolo.h"
#include "star6e_iq.h"
#include "star6e_output.h"
#include "star6e_runtime.h"
#include "star6e_vpe_ports.h"
#include "timing.h"
#include "venc_api.h"
#include "venc_jpeg.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
	MI_VENC_CHN venc_chn;
	MI_SYS_ChnPort_t vpe_port;
	MI_SYS_ChnPort_t venc_port;
	volatile uint32_t sensor_fps;
	/* Current VPE->VENC bind delivery rate (true frames reaching the
	 * encoder) — above STAR6E_VENC_INPUT_FPS_MAX this exceeds the RC
	 * fpsNum and the bitrate budget must be compensated (see
	 * rc_compensate_kbps). */
	volatile uint32_t delivered_fps;
	uint32_t frame_width;
	uint32_t frame_height;
	Star6ePipelineState *pipeline;
	VencConfig *vcfg;
	/* Destination the output socket is ACTUALLY pointed at.  Must not be
	 * read from vcfg: venc_api's apply_live_group_for_cfg() commits the
	 * new config into g_cfg *before* dispatching to these callbacks, so
	 * vcfg->outgoing.server already equals the incoming uri -- comparing
	 * against it matches even on a real change. */
	char applied_server[VENC_CONFIG_STRING_MAX];
} Star6eControlContext;

typedef struct {
	int /*MI_ISP_BOOL_e*/ bIsStable;
	uint16_t u16Rgain;
	uint16_t u16Grgain;
	uint16_t u16Gbgain;
	uint16_t u16Bgain;
	uint16_t u16ColorTemp;
	uint8_t u8WPInd;
	int bMultiLSDetected;
	uint8_t u8FirstLSInd;
	uint8_t u8SecondLSInd;
} AwbQueryInfo_t;

typedef struct {
	uint32_t Size;
	uint32_t AvgBlkX;
	uint32_t AvgBlkY;
	uint32_t CurRGain;
	uint32_t CurGGain;
	uint32_t CurBGain;
	void *avgs;
	uint8_t HDRMode;
	void **pAwbStatisShort;
	uint32_t u4BVx16384;
	int32_t WeightY;
} __attribute__((packed, aligned(1))) AwbCus3aInfo_t;

typedef struct {
	int bEnable;
	uint16_t u16GlbGainThd;
	uint16_t u16CountThd;
	uint16_t u16ForceTriGainThd;
} AwbStabilizer_t;

typedef struct {
	uint32_t u32CT;
} AwbCtMwb_t;

typedef struct {
	int eState;
	int eOpType;
	uint16_t mwb_rgain;
	uint16_t mwb_grgain;
	uint16_t mwb_gbgain;
	uint16_t mwb_bgain;
	uint8_t _pad[8192];
} AwbAttr_t;

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

typedef struct {
	uint32_t u32FNx10;
	uint32_t u32SensorGain;
	uint32_t u32ISPGain;
	uint32_t u32US;
} AeExpoValue_t;

typedef struct {
	uint32_t u32LumY;
	uint32_t u32AvgY;
	uint32_t u32Hits[128];
} AeHistWeightY_t;

typedef struct {
	int bIsStable;
	int bIsReachBoundary;
	AeExpoValue_t stExpoValueLong;
	AeExpoValue_t stExpoValueShort;
	AeHistWeightY_t stHistWeightY;
	uint32_t u32LVx10;
	int32_t s32BV;
	uint32_t u32SceneTarget;
} AeExpoInfo_t;

typedef struct {
	int plane_ret;
	int limit_ret;
	int info_ret;
	int state_ret;
	int mode_ret;
	MI_SNR_PAD_ID_e pad_id;
	MI_SNR_PlaneInfo_t plane;
	IspExposureLimit limit;
	AeExpoInfo_t info;
	int ae_state;
	int ae_mode_raw;
} AeDiagSnapshot;

enum {
	STAR6E_AWB_MODE_AUTO = 0,
	STAR6E_AWB_MODE_MANUAL = 1,
	STAR6E_AWB_MODE_CT_MANUAL = 2,
};

enum {
	STAR6E_ISP_STATE_NORMAL = 0,
	STAR6E_ISP_STATE_PAUSE = 1,
	STAR6E_AE_EXPO_MODE_AUTO = 0,
};

static Star6eControlContext g_star6e_control_ctx;

static int apply_encoder_gop(uint32_t gop_size);
static int request_idr(void);
static int request_idr_bootstrap(void);

static int apply_rc_qp_delta(const MI_VENC_ChnAttr_t *attr, MI_VENC_RcParam_t *param,
	int delta)
{
	if (!attr || !param || delta < -12 || delta > 12)
		return -1;

	switch (attr->rate.mode) {
	case I6_VENC_RATEMODE_H265CBR:
		param->stParamH265Cbr.s32IPQPDelta = delta;
		return 0;
	case I6_VENC_RATEMODE_H264CBR:
		param->stParamH264Cbr.s32IPQPDelta = delta;
		return 0;
	case I6_VENC_RATEMODE_H265VBR:
		param->stParamH265Vbr.s32IPQPDelta = delta;
		return 0;
	case I6_VENC_RATEMODE_H264VBR:
		param->stParamH264VBR.s32IPQPDelta = delta;
		return 0;
	case I6_VENC_RATEMODE_H265AVBR:
		param->stParamH265Avbr.s32IPQPDelta = delta;
		return 0;
	case I6_VENC_RATEMODE_H264AVBR:
		param->stParamH264Avbr.s32IPQPDelta = delta;
		return 0;
	default:
		return -1;
	}
}

/* Only CBR carries QP bounds we can steer; other modes leave them alone. */
static int rc_qp_bound_ptrs(const MI_VENC_ChnAttr_t *attr,
	MI_VENC_RcParam_t *param, uint32_t **pmin, uint32_t **pmax)
{
	if (!attr || !param)
		return -1;
	switch (attr->rate.mode) {
	case I6_VENC_RATEMODE_H265CBR:
		*pmin = &param->stParamH265Cbr.u32MinQp;
		*pmax = &param->stParamH265Cbr.u32MaxQp;
		return 0;
	case I6_VENC_RATEMODE_H264CBR:
		*pmin = &param->stParamH264Cbr.u32MinQp;
		*pmax = &param->stParamH264Cbr.u32MaxQp;
		return 0;
	default:
		return -1;
	}
}

/* Everything we have asked the rate controller for, kept because
 * MI_VENC_GetRcParam cannot be trusted to give it back.
 *
 * The SDK does not reflect a just-written s32IPQPDelta (or QP bound) in the
 * next Get: it keeps returning driver defaults until the driver commits the
 * pending block, measured on SSC338Q 2026-08-29 as somewhere between t+0s
 * and t+5s after StartRecvPic.  A second read-modify-write inside that
 * window therefore reads a stale 0 and writes it straight back, silently
 * reverting the earlier apply -- both calls return success and both log it.
 *
 * That is issue #255.  Startup applies qpDelta and then apply_qp_bounds()
 * reverts it, so every craft with minQp/maxQp configured flew with an
 * effective IPQPDelta of 0 and ~20x larger IDRs (78-81 KB against 3.1-3.8 KB
 * measured at 19 Mbps 720p60).  The deleted apply_max_frame_size() had the
 * same shape and was how the bug first surfaced.
 *
 * So every RC write re-stages the whole intent rather than trusting the Get.
 * Ordering and timing then cannot matter, which is the point: this hazard has
 * already bitten twice through two different callers. */
static struct {
	int      qp_delta;
	uint32_t min_qp;   /* 0 = driver default */
	uint32_t max_qp;   /* 0 = driver default */
} g_rc_intent;

/* Exact-CBR compensation for >STAR6E_VENC_INPUT_FPS_MAX modes: the RC
 * budgets kbps at rc_fps (capped 120) while the bind delivers more frames,
 * so the wire rate is kbps * delivered/rc (~1.19x at 144).  Scale the
 * encoder budget down so the wire lands on the configured bitrate.  Safe
 * with rc_fps=120: the per-frame budget stays large enough that QP does
 * not saturate (the naive version was only rejected when RC wrongly
 * budgeted 30fps — see specs/2026-07-06-star6e-144fps-bitrate-overshoot). */
static uint32_t rc_compensate_kbps(uint32_t kbps, uint32_t delivered_fps)
{
	if (delivered_fps > STAR6E_VENC_INPUT_FPS_MAX)
		kbps = (uint32_t)((uint64_t)kbps * STAR6E_VENC_INPUT_FPS_MAX /
			delivered_fps);
	return kbps;
}

/* No IDR request on bitrate writes.
 *
 * This removal is a no-op on the wire, and that is worth stating plainly:
 * MI_VENC_SetChnAttr() below emits an IDR on its own.  Measured on a
 * SSC338Q (IMX335 1920x1080@60, H.265 CBR, GDR via the racing preset,
 * 2026-08-23), ten spaced video0.bitrate writes produced eleven IRAP
 * access units both with and without the request — it only ever
 * duplicated a keyframe the SDK had already inserted.
 *
 * It is still worth removing.  The request consumed a slot in the shared
 * 100 ms gate (idr_rate_limit_allow), so a genuine request — /request/idr,
 * or the receiver's own recovery ask — landing within 100 ms of a bitrate
 * write was silently swallowed.  And every write inflated
 * /api/v1/idr/stats with an IDR it did not cause, which is what made the
 * counters read as if bitrate writes were the IDR source.
 *
 * The surviving implicit IDR is a SetChnAttr property that this layer
 * cannot gate: MI_VENC_RcParam_t carries no bitrate field on either
 * SigmaStar backend (include/star6e.h), so there is no rate-only actuator
 * to switch to.  Calling SetChnAttr less often is the open follow-up. */
static int apply_bitrate(uint32_t kbps)
{
	MI_VENC_ChnAttr_t attr = {0};
	MI_U32 bits;

	/* Order is pinned: the >120 fps exact-CBR compensation first, then the
	 * absolute MIN/MAX rails, so the correction cannot push the programmed
	 * value outside what the encoder accepts. */
	kbps = rc_compensate_kbps(kbps, g_star6e_control_ctx.delivered_fps);
	if (kbps > VENC_BITRATE_MAX_KBPS)
		kbps = VENC_BITRATE_MAX_KBPS;
	if (kbps < VENC_BITRATE_MIN_KBPS)
		kbps = VENC_BITRATE_MIN_KBPS;
	bits = kbps * 1024;

	if (MI_VENC_GetChnAttr(g_star6e_control_ctx.venc_chn, &attr) != 0)
		return -1;

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

	if (MI_VENC_SetChnAttr(g_star6e_control_ctx.venc_chn, &attr) != 0)
		return -1;
	return 0;
}

static int apply_encoder_gop(uint32_t gop_size)
{
	MI_VENC_ChnAttr_t attr = {0};

	if (MI_VENC_GetChnAttr(g_star6e_control_ctx.venc_chn, &attr) != 0)
		return -1;

	switch (attr.rate.mode) {
	case I6_VENC_RATEMODE_H265CBR:
		attr.rate.h265Cbr.gop = gop_size;
		break;
	case I6_VENC_RATEMODE_H264CBR:
		attr.rate.h264Cbr.gop = gop_size;
		break;
	case I6_VENC_RATEMODE_H265VBR:
		attr.rate.h265Vbr.gop = gop_size;
		break;
	case I6_VENC_RATEMODE_H264VBR:
		attr.rate.h264Vbr.gop = gop_size;
		break;
	case I6_VENC_RATEMODE_H265AVBR:
		attr.rate.h265Avbr.gop = gop_size;
		break;
	case I6_VENC_RATEMODE_H264AVBR:
		attr.rate.h264Avbr.gop = gop_size;
		break;
	default:
		return -1;
	}

	return MI_VENC_SetChnAttr(g_star6e_control_ctx.venc_chn, &attr) == 0 ?
		0 : -1;
}

static int apply_gop(uint32_t gop_size)
{
	return apply_encoder_gop(gop_size);
}

static int apply_zoom(double pct, double x, double y)
{
	if (!g_star6e_control_ctx.pipeline)
		return -1;
	return star6e_pipeline_apply_zoom(g_star6e_control_ctx.pipeline,
		pct, x, y);
}

static int apply_pause_stab(bool paused)
{
	return star6e_pipeline_set_pause_stab(paused);
}

/* Driver-default QP bounds, captured on the first Get before any write so
 * a later 0 can restore them. The channel's rate mode is fixed for the
 * process lifetime (reinit is fork+exec), so one capture is enough. */
static struct {
	int      captured;
	uint32_t min_qp;
	uint32_t max_qp;
} g_qp_defaults;

/* Write the whole of g_rc_intent, never a patched Get result — see the note
 * on g_rc_intent for why the Get cannot be trusted.  require_bounds makes
 * the call fail without writing when this rate mode has no QP bounds. */
static int rc_commit_intent(int require_bounds)
{
	MI_VENC_ChnAttr_t attr = {0};
	MI_VENC_RcParam_t param = {0};
	uint32_t *pmin, *pmax;

	if (MI_VENC_GetChnAttr(g_star6e_control_ctx.venc_chn, &attr) != 0)
		return -1;
	if (MI_VENC_GetRcParam(g_star6e_control_ctx.venc_chn, &param) != 0)
		return -1;
	if (apply_rc_qp_delta(&attr, &param, g_rc_intent.qp_delta) != 0)
		return -1;
	if (rc_qp_bound_ptrs(&attr, &param, &pmin, &pmax) == 0) {
		/* Captured from the first Get, which still shows driver state:
		 * nothing has been written to this channel yet. */
		if (!g_qp_defaults.captured) {
			g_qp_defaults.min_qp = *pmin;
			g_qp_defaults.max_qp = *pmax;
			g_qp_defaults.captured = 1;
		}
		*pmin = g_rc_intent.min_qp ? g_rc_intent.min_qp
					   : g_qp_defaults.min_qp;
		*pmax = g_rc_intent.max_qp ? g_rc_intent.max_qp
					   : g_qp_defaults.max_qp;
		/* The API validator only compares min against max when BOTH are
		 * non-zero, so a half-specified pair can still resolve to
		 * min > max against the driver default.  The SDK takes that
		 * without complaint and then behaves erratically, so reject it
		 * here rather than write it.  Same guard as
		 * maruko_apply_qp_bounds() and cv610_apply_qp_bounds(). */
		if (*pmin > *pmax) {
			fprintf(stderr, "ERROR: qpBounds min>max after resolving "
				"defaults (%u/%u)\n", (unsigned)*pmin,
				(unsigned)*pmax);
			return -1;
		}
	} else if (require_bounds) {
		/* This rate mode carries no QP bounds.  Return BEFORE the write:
		 * master's apply_qp_bounds() bailed out of its switch without ever
		 * calling SetRcParam, and a rejected request must not touch the
		 * encoder.  Reachable — video0.rcMode selects VBR/AVBR, and the
		 * API's rollback re-invokes the failing group, so writing here
		 * would poke the hardware twice per refusal. */
		return -1;
	}
	return MI_VENC_SetRcParam(g_star6e_control_ctx.venc_chn, &param) == 0
		? 0 : -1;
}

static int apply_qp_delta(int delta)
{
	int prev = g_rc_intent.qp_delta;

	g_rc_intent.qp_delta = delta;
	if (rc_commit_intent(0) != 0) {
		g_rc_intent.qp_delta = prev;
		return -1;
	}
	/* No IDR: a QP-delta change is rate-control state, absorbed mid-GOP.
	 * Unlike apply_bitrate() this is a real reduction — MI_VENC_SetRcParam
	 * does NOT implicitly keyframe, so the request removed here was the
	 * only IDR source on this path.  Measured 2026-08-23: ten spaced
	 * video0.qpDelta writes went from eleven IRAP access units to one. */
	printf("> qpDelta changed to %d\n", delta);
	return 0;
}

static int apply_qp_bounds(uint32_t min_qp, uint32_t max_qp)
{
	uint32_t prev_min = g_rc_intent.min_qp;
	uint32_t prev_max = g_rc_intent.max_qp;
	if (min_qp == 0 && max_qp == 0 && !g_qp_defaults.captured)
		return 0;   /* never written — driver defaults already in force */

	/* 0 restores the captured driver default so bounds can be cleared
	 * live, not just overridden. */
	g_rc_intent.min_qp = min_qp;
	g_rc_intent.max_qp = max_qp;
	if (rc_commit_intent(1) != 0) {
		g_rc_intent.min_qp = prev_min;
		g_rc_intent.max_qp = prev_max;
		return -1;
	}
	printf("> qpBounds changed: min=%u max=%u (0 = driver default %u/%u)\n",
		min_qp, max_qp, g_qp_defaults.min_qp, g_qp_defaults.max_qp);
	return 0;
}

static int apply_encoder_fps(uint32_t fps)
{
	MI_VENC_ChnAttr_t attr = {0};
	if (MI_VENC_GetChnAttr(g_star6e_control_ctx.venc_chn, &attr) != 0)
		return -1;

	switch (attr.rate.mode) {
	case I6_VENC_RATEMODE_H265CBR:
		attr.rate.h265Cbr.fpsNum = fps;
		break;
	case I6_VENC_RATEMODE_H264CBR:
		attr.rate.h264Cbr.fpsNum = fps;
		break;
	case I6_VENC_RATEMODE_H265VBR:
		attr.rate.h265Vbr.fpsNum = fps;
		break;
	case I6_VENC_RATEMODE_H264VBR:
		attr.rate.h264Vbr.fpsNum = fps;
		break;
	case I6_VENC_RATEMODE_H265AVBR:
		attr.rate.h265Avbr.fpsNum = fps;
		break;
	case I6_VENC_RATEMODE_H264AVBR:
		attr.rate.h264Avbr.fpsNum = fps;
		break;
	default:
		break;
	}

	return MI_VENC_SetChnAttr(g_star6e_control_ctx.venc_chn, &attr) == 0 ?
		0 : -1;
}

static int apply_scene_fps(uint32_t fps)
{
	(void)fps;
	return 0;
}

static int apply_fps(uint32_t fps)
{
	MI_S32 bind_ret;
	uint32_t sensor_fps;
	uint32_t rc_fps;

	if (fps == 0 || fps > PIPELINE_LIVE_FPS_MAX)
		return -1;

	/* A request above the current mode's max is clamped for the rebind (see
	 * PIPELINE_LIVE_FPS_MAX) — the caller's config still holds the requested
	 * value so a later respawn can select a higher-fps sensor mode for it. */
	sensor_fps = g_star6e_control_ctx.sensor_fps;
	if (fps > sensor_fps) {
		printf("> FPS %u exceeds sensor mode max %u, clamping\n", fps,
			sensor_fps);
		fps = sensor_fps;
	}
	if (fps == g_star6e_control_ctx.delivered_fps)
		return 0;
	/* Decouple delivery from rate control: the VPE->VENC bind DELIVERS the
	 * true fps (VENC encodes it fine — the block does ~143fps), but the RC
	 * fpsNum is capped to STAR6E_VENC_INPUT_FPS_MAX because _MI_VENC_VerifyFps
	 * resets any RC fps > 120 to 30/1 (which wrecks CBR).  rc_fps=120 vs a 143
	 * delivered rate leaves only ~19% CBR overshoot instead of ~4.7x. */
	rc_fps = fps > STAR6E_VENC_INPUT_FPS_MAX ? STAR6E_VENC_INPUT_FPS_MAX : fps;

	MI_SYS_UnBindChnPort(&g_star6e_control_ctx.vpe_port,
		&g_star6e_control_ctx.venc_port);
	bind_ret = MI_SYS_BindChnPort2(&g_star6e_control_ctx.vpe_port,
		&g_star6e_control_ctx.venc_port, sensor_fps, fps,
		I6_SYS_LINK_FRAMEBASE, 0);
	if (bind_ret != 0) {
		printf("> Rebind VPE->VENC at %u:%u fps failed %d, restoring\n",
			sensor_fps, fps, bind_ret);
		MI_SYS_BindChnPort2(&g_star6e_control_ctx.vpe_port,
			&g_star6e_control_ctx.venc_port, sensor_fps, sensor_fps,
			I6_SYS_LINK_FRAMEBASE, 0);
		g_star6e_control_ctx.delivered_fps = sensor_fps;
		return -1;
	}

	if (apply_encoder_fps(rc_fps) != 0 || apply_scene_fps(rc_fps) != 0) {
		/* Bind succeeded but the encoder/scene fps write failed —
		 * restore the bind to sensor_fps:sensor_fps so we don't leave
		 * VPE->VENC bound at the new fps while the caller treats the
		 * group as failed and rolls back the committed config. */
		printf("> Encoder fps apply failed, restoring bind %u:%u\n",
			sensor_fps, sensor_fps);
		MI_SYS_UnBindChnPort(&g_star6e_control_ctx.vpe_port,
			&g_star6e_control_ctx.venc_port);
		MI_SYS_BindChnPort2(&g_star6e_control_ctx.vpe_port,
			&g_star6e_control_ctx.venc_port, sensor_fps, sensor_fps,
			I6_SYS_LINK_FRAMEBASE, 0);
		g_star6e_control_ctx.delivered_fps = sensor_fps;
		return -1;
	}

	/* Delivered rate changed — if the exact-CBR compensation factor
	 * changed with it (either side above the RC cap), re-program the
	 * encoder budget from the committed config so the wire rate stays
	 * on video0.bitrate. */
	{
		uint32_t old_delivered = g_star6e_control_ctx.delivered_fps;
		uint32_t cfg_kbps = g_star6e_control_ctx.vcfg ?
			g_star6e_control_ctx.vcfg->video0.bitrate : 0;

		g_star6e_control_ctx.delivered_fps = fps;
		if (cfg_kbps > 0 &&
		    rc_compensate_kbps(cfg_kbps, old_delivered) !=
		    rc_compensate_kbps(cfg_kbps, fps))
			(void)apply_bitrate(cfg_kbps);
	}

	/* No IDR here.  A rebind does need one, but three of this function's
	 * four callers do not: output-enable issues its own immediately after
	 * (two back-to-back requests, coalesced only by luck of the 100 ms
	 * gate), output-disable would emit one *after* the output is already
	 * off, and the startup idle transition has no consumer at all.  Only
	 * the live video0.fps write needs it, so it is issued there — see
	 * apply_fps_live(). */
	printf("> FPS delivered %u, RC fpsNum %u (bind %u:%u)\n", fps, rc_fps,
		sensor_fps, fps);
	return 0;
}

/* Live video0.fps write: the rebind re-creates the encoder channel, so the
 * stream is genuinely discontinuous and a receiver needs a random-access
 * point.  This is the one fps path that owns an IDR. */
static int apply_fps_live(uint32_t fps)
{
	/* Only a real rebind is a bootstrap event.  apply_fps() returns 0 both
	 * for a rebind and for its unchanged-fps early return, and the caller
	 * cannot tell them apart -- venc_api dispatches on key name and never
	 * compares old against new, so any POST naming video0.fps lands here.
	 * Firing the deliberately un-coalescible bootstrap IDR on the no-op
	 * turned a ground re-posting its profile into an ungated keyframe at
	 * the caller's write rate: exactly the storm the spacing gate exists
	 * to absorb.  Compare the delivered fps instead of trusting ret. */
	uint32_t before = g_star6e_control_ctx.delivered_fps;
	int ret = apply_fps(fps);

	if (ret == 0 && g_star6e_control_ctx.delivered_fps != before)
		(void)request_idr_bootstrap();
	return ret;
}

static int apply_gain_max(uint32_t gain)
{
	if (star6e_cus3a_running())
		star6e_cus3a_set_gain_max(gain);
	return 0;
}

static int apply_shutter_max(uint32_t us)
{
	if (star6e_cus3a_running())
		star6e_cus3a_set_shutter_max(us);
	return 0;
}

static int apply_gain_min(uint32_t gain)
{
	if (star6e_cus3a_running())
		star6e_cus3a_set_gain_min(gain);
	return 0;
}

static int apply_shutter_min(uint32_t us)
{
	if (star6e_cus3a_running())
		star6e_cus3a_set_shutter_min(us);
	return 0;
}

static int apply_verbose(bool on)
{
	printf("> Verbose %s via API\n", on ? "enabled" : "disabled");
	return 0;
}

static int request_idr(void)
{
	int chn = g_star6e_control_ctx.venc_chn;
	if (!idr_rate_limit_allow(chn))
		return 0;  /* coalesced — not an error */
	return MI_VENC_RequestIdr(chn, 1) == 0 ? 0 : -1;
}

/* Bootstrap form: output enable, destination change, live fps rebind.  Each
 * hands the stream to a receiver that has seen no parameter set, so the
 * spacing gate must not swallow it — see idr_rate_limit_force().  Counted,
 * never coalesced, so a -1 here means a real SDK failure and the callers'
 * error handling finally means what it says. */
static int request_idr_bootstrap(void)
{
	int chn = g_star6e_control_ctx.venc_chn;
	idr_rate_limit_force(chn);
	return MI_VENC_RequestIdr(chn, 1) == 0 ? 0 : -1;
}

/* Compute one horizontal ROI band for step index of steps.
 * Full-height bands centered horizontally, tapered QP toward edges.
 * Returns 0 if valid, -1 if region should be skipped. */
static int compute_horizontal_roi(uint32_t width, uint32_t height,
	float center_frac, int qp, int steps, int index,
	MI_VENC_RoiCfg_t *roi)
{
	PipelineRoiBand band;

	if (!roi || pipeline_common_roi_band(width, height, center_frac, qp,
	    steps, index, &band) != 0)
		return -1;

	roi->u32Index = (uint32_t)index;
	roi->bEnable = 1;
	roi->bAbsQp = 0;
	roi->s32Qp = band.qp;
	roi->stRect.u32Left = band.x;
	roi->stRect.u32Top = band.y;
	roi->stRect.u32Width = band.width;
	roi->stRect.u32Height = band.height;
	return 0;
}

static int apply_roi_qp(int qp)
{
	uint32_t width = g_star6e_control_ctx.frame_width;
	uint32_t height = g_star6e_control_ctx.frame_height;
	int ok = 1;
	int programmed = 0;
	uint16_t steps;
	float center_frac;

	if (width == 0 || height == 0)
		return -1;

	for (int i = 0; i < PIPELINE_ROI_MAX_STEPS; i++) {
		MI_VENC_RoiCfg_t roi = {0};
		MI_S32 cret;

		roi.u32Index = i;
		roi.bEnable = 0;
		cret = MI_VENC_SetRoiCfg(g_star6e_control_ctx.venc_chn, &roi);
		/* Checked, not discarded: a refused clear leaves the stale band
		 * enabled with its previous geometry, so shrinking roiSteps
		 * would overlap the new bands with the old ones while this
		 * function logged success. */
		if (cret != 0) {
			printf("> ROI[%d] clear failed (ret=0x%08x), stale "
				"band may still be enabled\n", i,
				(unsigned)cret);
			ok = 0;
		}
	}

	if (!g_star6e_control_ctx.vcfg ||
	    !g_star6e_control_ctx.vcfg->fpv.roi_enabled || qp == 0) {
		/* Name the cause.  Three different states land here and the old
		 * single line could not tell them apart, so an operator who set
		 * roiEnabled:true and left roiQp at 0 read "disabled" as "my
		 * flag was ignored". */
		printf("> ROI disabled (%s), %s\n",
			!g_star6e_control_ctx.vcfg ? "no config bound" :
			!g_star6e_control_ctx.vcfg->fpv.roi_enabled ?
				"roiEnabled=false" :
				"roiQp=0, no delta to apply",
			ok ? "all regions cleared" :
				"CLEAR FAILED, see above");
		return ok ? 0 : -1;
	}

	if (qp < -20) qp = -20;
	if (qp > 20) qp = 20;

	steps = g_star6e_control_ctx.vcfg->fpv.roi_steps;
	if (steps < 1) steps = 1;
	if (steps > PIPELINE_ROI_MAX_STEPS) steps = PIPELINE_ROI_MAX_STEPS;

	center_frac = (float)g_star6e_control_ctx.vcfg->fpv.roi_center;
	if (center_frac < 0.1f) center_frac = 0.1f;
	if (center_frac > 0.9f) center_frac = 0.9f;

	for (int i = 0; i < steps; i++) {
		MI_VENC_RoiCfg_t roi = {0};
		MI_S32 ret;

		if (compute_horizontal_roi(width, height, center_frac, qp,
		    steps, i, &roi) != 0)
			continue;

		ret = MI_VENC_SetRoiCfg(g_star6e_control_ctx.venc_chn, &roi);
		if (ret != 0) {
			printf("> ROI[%d] set failed (ret=0x%08x) rect=(%u,%u %ux%u) qp=%+d\n",
				i, (unsigned)ret,
				roi.stRect.u32Left, roi.stRect.u32Top,
				roi.stRect.u32Width, roi.stRect.u32Height,
				roi.s32Qp);
			ok = 0;
		}
		programmed++;
	}

	if (programmed == 0) {
		/* Every band was skipped as degenerate.  Logging "ROI
		 * horizontal" here would assert an ROI that does not exist. */
		printf("> ROI programmed no bands at %ux%u (steps=%u "
			"center=%.2f)\n", width, height, steps,
			(double)center_frac);
		return -1;
	}

	if (ok) {
		printf("> ROI horizontal: %ux%u, %u steps, center=%.0f%%, qp=%+d\n",
			width, height, steps, center_frac * 100.0f, qp);
	}

	return ok ? 0 : -1;
}

static const char *ae_state_name(int state)
{
	return state == STAR6E_ISP_STATE_PAUSE ? "pause" : "normal";
}

static const char *ae_expo_mode_name(int mode)
{
	return mode == STAR6E_AE_EXPO_MODE_AUTO ? "auto" : "unknown";
}

static void ae_diag_snapshot_defaults(AeDiagSnapshot *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->pad_id = 0;
	snapshot->limit_ret = -1;
	snapshot->info_ret = -1;
	snapshot->state_ret = -1;
	snapshot->mode_ret = -1;
	snapshot->ae_state = STAR6E_ISP_STATE_NORMAL;
	snapshot->ae_mode_raw = STAR6E_AE_EXPO_MODE_AUTO;
}

static void ae_diag_snapshot_collect(AeDiagSnapshot *snapshot)
{
	typedef MI_S32 (*fn_get_limit_t)(uint32_t, IspExposureLimit *);
	typedef MI_S32 (*fn_query_info_t)(uint32_t, AeExpoInfo_t *);
	typedef MI_S32 (*fn_get_state_t)(uint32_t, int *);
	typedef MI_S32 (*fn_get_mode_t)(uint32_t, int *);
	void *handle;

	ae_diag_snapshot_defaults(snapshot);
	if (g_star6e_control_ctx.pipeline)
		snapshot->pad_id = g_star6e_control_ctx.pipeline->sensor.pad_id;
	snapshot->plane_ret = MI_SNR_GetPlaneInfo(snapshot->pad_id, 0,
		&snapshot->plane);

	handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!handle)
		return;

	{
		fn_get_limit_t fn_get_limit = (fn_get_limit_t)dlsym(handle,
			"MI_ISP_AE_GetExposureLimit");
		fn_query_info_t fn_query_info = (fn_query_info_t)dlsym(handle,
			"MI_ISP_AE_QueryExposureInfo");
		fn_get_state_t fn_get_state = (fn_get_state_t)dlsym(handle,
			"MI_ISP_AE_GetState");
		fn_get_mode_t fn_get_mode = (fn_get_mode_t)dlsym(handle,
			"MI_ISP_AE_GetExpoMode");

		if (fn_get_limit)
			snapshot->limit_ret = fn_get_limit(0, &snapshot->limit);
		if (fn_query_info)
			snapshot->info_ret = fn_query_info(0, &snapshot->info);
		if (fn_get_state)
			snapshot->state_ret = fn_get_state(0, &snapshot->ae_state);
		if (fn_get_mode)
			snapshot->mode_ret = fn_get_mode(0, &snapshot->ae_mode_raw);
	}

	dlclose(handle);
}

static uint32_t ae_diag_exposure_us(const AeDiagSnapshot *snapshot)
{
	if (snapshot->info_ret == 0)
		return snapshot->info.stExpoValueLong.u32US;
	return snapshot->plane.shutter;
}

static uint32_t ae_diag_sensor_gain(const AeDiagSnapshot *snapshot)
{
	if (snapshot->info_ret == 0)
		return snapshot->info.stExpoValueLong.u32SensorGain;
	return snapshot->plane.sensGain;
}

static uint32_t ae_diag_isp_gain(const AeDiagSnapshot *snapshot)
{
	if (snapshot->info_ret == 0)
		return snapshot->info.stExpoValueLong.u32ISPGain;
	return snapshot->plane.compGain;
}

typedef struct { uint32_t r, g, b; } AwbCus3aStatus;

/* Applied AWB gains as the ISP reports them (MI_ISP_CUS3A_GetAwbStatus).  This
 * is the truth under userspace AWB — MI_ISP_AWB_QueryInfo tracks the internal
 * algorithm, which is not the one driving.  Returns 0 on success. */
static int awb_cus3a_applied_gains(AwbCus3aStatus *out)
{
	typedef MI_S32 (*fn_t)(uint32_t, AwbCus3aInfo_t *);
	void *handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	int rc = -1;

	if (!handle)
		return -1;
	{
		fn_t fn = (fn_t)dlsym(handle, "MI_ISP_CUS3A_GetAwbStatus");
		AwbCus3aInfo_t info;

		if (fn) {
			memset(&info, 0, sizeof(info));
			info.Size = sizeof(info);
			if (fn(0, &info) == 0) {
				out->r = info.CurRGain;
				out->g = info.CurGGain;
				out->b = info.CurBGain;
				rc = 0;
			}
		}
	}
	dlclose(handle);
	return rc;
}

void star6e_controls_ae_osd_status(Star6eAeOsdStatus *out)
{
	AeDiagSnapshot s;

	memset(out, 0, sizeof(*out));
	ae_diag_snapshot_collect(&s);

	/* Sensor plane shutter/gain remain valid even if the AE query fails,
	 * so surface those as long as either source answered. */
	out->ae_valid = (s.info_ret == 0 || s.plane_ret == 0);
	out->shutter_us = ae_diag_exposure_us(&s);
	out->sgain_x1024 = ae_diag_sensor_gain(&s);
	out->igain_x1024 = ae_diag_isp_gain(&s);
	if (s.limit_ret == 0) {
		out->max_shutter_us = s.limit.maxShutterUs;
		out->max_sgain = s.limit.maxSensorGain;
	}
	if (s.info_ret == 0) {
		out->ae_info_valid = 1;
		out->luma_y = s.info.stHistWeightY.u32LumY;
		out->scene_target = s.info.u32SceneTarget;
		out->stable = s.info.bIsStable ? 1 : 0;
		out->boundary = s.info.bIsReachBoundary ? 1 : 0;
	}

	{
		typedef MI_S32 (*fn_awb_query_t)(uint32_t, AwbQueryInfo_t *);
		void *handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);

		if (handle) {
			fn_awb_query_t fn = (fn_awb_query_t)dlsym(handle,
				"MI_ISP_AWB_QueryInfo");
			if (fn) {
				AwbQueryInfo_t qi;

				memset(&qi, 0, sizeof(qi));
				if (fn(0, &qi) == 0) {
					out->awb_valid = 1;
					out->rgain = qi.u16Rgain;
					out->bgain = qi.u16Bgain;
					out->color_temp = qi.u16ColorTemp;
					out->awb_stable = qi.bIsStable ? 1 : 0;
				}
			}
			dlclose(handle);
		}
	}

	/* When the userspace loop owns AWB, MI_ISP_AWB_QueryInfo reports the
	 * ISP-internal algorithm's last state — frozen precisely because that
	 * algorithm is no longer running.  Show the applied gains instead.
	 *
	 * Ownership is decided from config, NOT from whether the loop thread
	 * happens to be up yet: keying off liveness made the row render the
	 * stale internal view (with its misleading "adj") for the first refresh
	 * after a start or reinit, then flip format once the thread ticked. */
	{
		const VencConfig *vc = g_star6e_control_ctx.vcfg;
		int owns = vc && vc->isp.awb_fps > 0 &&
			strcmp(vc->isp.awb_mode, "auto") == 0;

		if (owns) {
			uint32_t lr = 0, lg = 0, lb = 0, lticks = 0;
			int lpaused = 0;
			AwbCus3aStatus st;

			out->awb_valid = 1;
			out->awb_userspace = 1;
			out->color_temp = 0;   /* not estimated by the loop */
			out->awb_stable = 0;
			if (star6e_awb_status(&lr, &lg, &lb, &lticks, &lpaused))
				out->awb_ticks = lticks;
			/* Applied gains straight from the ISP — correct even
			 * before the loop's first tick has populated its own
			 * cache. */
			if (awb_cus3a_applied_gains(&st) == 0) {
				out->rgain = st.r;
				out->bgain = st.b;
			} else if (lr || lb) {
				out->rgain = lr;
				out->bgain = lb;
			}
		}
	}
}

static uint32_t ae_diag_sensor_fps(void)
{
	if (g_star6e_control_ctx.sensor_fps != 0)
		return g_star6e_control_ctx.sensor_fps;
	if (g_star6e_control_ctx.vcfg)
		return g_star6e_control_ctx.vcfg->video0.fps;
	return 0;
}

static uint32_t query_live_fps(void)
{
	MI_VENC_ChnAttr_t attr = {0};

	if (MI_VENC_GetChnAttr(g_star6e_control_ctx.venc_chn, &attr) != 0)
		return 0;

	switch (attr.rate.mode) {
	case I6_VENC_RATEMODE_H265CBR:
		return attr.rate.h265Cbr.fpsNum;
	case I6_VENC_RATEMODE_H264CBR:
		return attr.rate.h264Cbr.fpsNum;
	case I6_VENC_RATEMODE_H265VBR:
		return attr.rate.h265Vbr.fpsNum;
	case I6_VENC_RATEMODE_H264VBR:
		return attr.rate.h264Vbr.fpsNum;
	case I6_VENC_RATEMODE_H265AVBR:
		return attr.rate.h265Avbr.fpsNum;
	case I6_VENC_RATEMODE_H264AVBR:
		return attr.rate.h264Avbr.fpsNum;
	default:
		return 0;
	}
}

static char *query_ae_info(void)
{
	AeDiagSnapshot snapshot;
	char buf[2048];
	char precrop_field[96];
	uint32_t exposure_us;
	uint32_t sensor_gain;
	uint32_t isp_gain;
	uint16_t px = 0, py = 0, pw = 0, ph = 0;

	ae_diag_snapshot_collect(&snapshot);
	exposure_us = ae_diag_exposure_us(&snapshot);
	sensor_gain = ae_diag_sensor_gain(&snapshot);
	isp_gain = ae_diag_isp_gain(&snapshot);

	if (venc_api_get_active_precrop(&px, &py, &pw, &ph)) {
		snprintf(precrop_field, sizeof(precrop_field),
			",\"active_precrop\":{\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u}",
			px, py, pw, ph);
	} else {
		precrop_field[0] = '\0';
	}

	snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{"
		"\"sensor_plane\":{\"ret\":%d,\"pad\":%d,\"shutter_us\":%u,"
		"\"sensor_gain_x1024\":%u,\"comp_gain_x1024\":%u},"
		"\"exposure_limit\":{\"ret\":%d,\"min_shutter_us\":%u,"
		"\"max_shutter_us\":%u,\"min_sensor_gain\":%u,\"max_sensor_gain\":%u,"
		"\"min_isp_gain\":%u,\"max_isp_gain\":%u},"
		"\"exposure_info\":{\"ret\":%d,\"stable\":%s,\"reach_boundary\":%s,"
		"\"long_us\":%u,\"long_sensor_gain_x1024\":%u,"
		"\"long_isp_gain_x1024\":%u,\"luma_y\":%u,\"avg_y\":%u,"
		"\"lv_x10\":%u,\"bv\":%d,\"scene_target\":%u},"
		"\"state\":{\"ret\":%d,\"raw\":%d,\"name\":\"%s\"},"
		"\"expo_mode\":{\"ret\":%d,\"raw\":%d,\"name\":\"%s\"},"
		"\"metrics\":{\"exposure_us\":%u,\"sensor_gain_x1024\":%u,"
		"\"isp_gain_x1024\":%u,\"fps\":%u},"
		"\"runtime\":{\"sensor_fps\":%u%s}}}",
		snapshot.plane_ret, snapshot.pad_id, snapshot.plane.shutter,
		snapshot.plane.sensGain, snapshot.plane.compGain,
		snapshot.limit_ret, snapshot.limit.minShutterUs,
		snapshot.limit.maxShutterUs, snapshot.limit.minSensorGain,
		snapshot.limit.maxSensorGain, snapshot.limit.minIspGain,
		snapshot.limit.maxIspGain,
		snapshot.info_ret,
		snapshot.info_ret == 0 && snapshot.info.bIsStable ? "true" : "false",
		snapshot.info_ret == 0 && snapshot.info.bIsReachBoundary ? "true" : "false",
		snapshot.info.stExpoValueLong.u32US,
		snapshot.info.stExpoValueLong.u32SensorGain,
		snapshot.info.stExpoValueLong.u32ISPGain,
		snapshot.info.stHistWeightY.u32LumY, snapshot.info.stHistWeightY.u32AvgY,
		snapshot.info.u32LVx10, snapshot.info.s32BV,
		snapshot.info.u32SceneTarget,
		snapshot.state_ret, snapshot.ae_state, ae_state_name(snapshot.ae_state),
		snapshot.mode_ret, snapshot.ae_mode_raw,
		ae_expo_mode_name(snapshot.ae_mode_raw),
		exposure_us, sensor_gain, isp_gain, ae_diag_sensor_fps(),
		ae_diag_sensor_fps(), precrop_field);
	return strdup(buf);
}

static char *query_isp_metrics(void)
{
	AeDiagSnapshot snapshot;
	char buf[512];

	ae_diag_snapshot_collect(&snapshot);
	snprintf(buf, sizeof(buf),
		"# HELP isp_again Analog Gain\n"
		"# TYPE isp_again gauge\n"
		"isp_again %u\n"
		"# HELP isp_dgain Digital Gain\n"
		"# TYPE isp_dgain gauge\n"
		"isp_dgain %u\n"
		"# HELP isp_exposure Exposure\n"
		"# TYPE isp_exposure gauge\n"
		"isp_exposure %u\n"
		"# HELP isp_fps Sensor fps\n"
		"# TYPE isp_fps gauge\n"
		"isp_fps %u\n"
		"# HELP isp_luma_y Scene luma Y\n"
		"# TYPE isp_luma_y gauge\n"
		"isp_luma_y %u\n"
		"# HELP isp_avg_y Scene average Y\n"
		"# TYPE isp_avg_y gauge\n"
		"isp_avg_y %u\n"
		"# HELP isp_ae_stable AE stability flag\n"
		"# TYPE isp_ae_stable gauge\n"
		"isp_ae_stable %u\n",
		ae_diag_sensor_gain(&snapshot),
		ae_diag_isp_gain(&snapshot),
		ae_diag_exposure_us(&snapshot) / 1000,
		ae_diag_sensor_fps(),
		snapshot.info.stHistWeightY.u32LumY,
		snapshot.info.stHistWeightY.u32AvgY,
		snapshot.info_ret == 0 && snapshot.info.bIsStable ? 1u : 0u);
	return strdup(buf);
}

static char *query_awb_info(void)
{
	typedef MI_S32 (*fn_query_t)(uint32_t, AwbQueryInfo_t *);
	typedef MI_S32 (*fn_cus3a_status_t)(uint32_t, AwbCus3aInfo_t *);
	typedef MI_S32 (*fn_stab_t)(uint32_t, AwbStabilizer_t *);
	typedef MI_S32 (*fn_attr_t)(uint32_t, AwbAttr_t *);
	typedef MI_S32 (*fn_ctmwb_get_t)(uint32_t, AwbCtMwb_t *);
	void *handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	char buf[2048];
	int pos = 0;

	if (!handle)
		return NULL;

	pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"ok\":true,\"data\":{");

	{
		fn_query_t fn_query = (fn_query_t)dlsym(handle, "MI_ISP_AWB_QueryInfo");

		if (fn_query) {
			AwbQueryInfo_t qi;
			MI_S32 ret;

			memset(&qi, 0, sizeof(qi));
			ret = fn_query(0, &qi);
			pos += snprintf(buf + pos, sizeof(buf) - pos,
				"\"query_info\":{\"ret\":%d,\"stable\":%s,"
				"\"r_gain\":%u,\"gr_gain\":%u,\"gb_gain\":%u,\"b_gain\":%u,"
				"\"color_temp\":%u,\"wp_index\":%u,"
				"\"multi_ls\":%s,\"ls1_idx\":%u,\"ls2_idx\":%u},",
				ret, qi.bIsStable ? "true" : "false", qi.u16Rgain,
				qi.u16Grgain, qi.u16Gbgain, qi.u16Bgain,
				qi.u16ColorTemp, qi.u8WPInd,
				qi.bMultiLSDetected ? "true" : "false",
				qi.u8FirstLSInd, qi.u8SecondLSInd);
		}
	}

	{
		fn_cus3a_status_t fn_cus3a = (fn_cus3a_status_t)dlsym(handle,
			"MI_ISP_CUS3A_GetAwbStatus");

		if (fn_cus3a) {
			AwbCus3aInfo_t ci;
			MI_S32 ret;

			memset(&ci, 0, sizeof(ci));
			ci.Size = sizeof(ci);
			ret = fn_cus3a(0, &ci);
			pos += snprintf(buf + pos, sizeof(buf) - pos,
				"\"cus3a_status\":{\"ret\":%d,"
				"\"avg_blk_x\":%u,\"avg_blk_y\":%u,"
				"\"cur_r_gain\":%u,\"cur_g_gain\":%u,\"cur_b_gain\":%u,"
				"\"hdr_mode\":%u,\"bv_x16384\":%u,\"weight_y\":%d},",
				ret, ci.AvgBlkX, ci.AvgBlkY, ci.CurRGain, ci.CurGGain,
				ci.CurBGain, ci.HDRMode, ci.u4BVx16384, ci.WeightY);
		}
	}

	{
		fn_stab_t fn_stab = (fn_stab_t)dlsym(handle,
			"MI_ISP_AWB_GetStabilizer");

		if (fn_stab) {
			AwbStabilizer_t st;
			MI_S32 ret;

			memset(&st, 0, sizeof(st));
			ret = fn_stab(0, &st);
			pos += snprintf(buf + pos, sizeof(buf) - pos,
				"\"stabilizer\":{\"ret\":%d,\"enabled\":%s,"
				"\"glb_gain_thd\":%u,\"count_thd\":%u,\"force_tri_thd\":%u},",
				ret, st.bEnable ? "true" : "false", st.u16GlbGainThd,
				st.u16CountThd, st.u16ForceTriGainThd);
		}
	}

	{
		fn_attr_t fn_attr = (fn_attr_t)dlsym(handle, "MI_ISP_AWB_GetAttr");

		if (fn_attr) {
			AwbAttr_t attr;
			MI_S32 ret;
			const char *mode_str = "unknown";

			memset(&attr, 0, sizeof(attr));
			ret = fn_attr(0, &attr);
			if (attr.eOpType == STAR6E_AWB_MODE_AUTO)
				mode_str = "auto";
			else if (attr.eOpType == STAR6E_AWB_MODE_MANUAL)
				mode_str = "manual";
			else if (attr.eOpType == STAR6E_AWB_MODE_CT_MANUAL)
				mode_str = "ct_manual";

			pos += snprintf(buf + pos, sizeof(buf) - pos,
				"\"attr\":{\"ret\":%d,\"state\":%d,\"mode\":\"%s\",\"mode_raw\":%d,"
				"\"mwb_r\":%u,\"mwb_gr\":%u,\"mwb_gb\":%u,\"mwb_b\":%u},",
				ret, attr.eState, mode_str, attr.eOpType, attr.mwb_rgain,
				attr.mwb_grgain, attr.mwb_gbgain, attr.mwb_bgain);
		}
	}

	{
		fn_ctmwb_get_t fn_ctmwb_get = (fn_ctmwb_get_t)dlsym(handle,
			"MI_ISP_AWB_GetCTMwbAttr");

		if (fn_ctmwb_get) {
			AwbCtMwb_t mwb;
			MI_S32 ret;

			memset(&mwb, 0, sizeof(mwb));
			ret = fn_ctmwb_get(0, &mwb);
			pos += snprintf(buf + pos, sizeof(buf) - pos,
				"\"ct_mwb\":{\"ret\":%d,\"ct\":%u}", ret, mwb.u32CT);
		}
	}

	pos += snprintf(buf + pos, sizeof(buf) - pos, "}}");
	dlclose(handle);
	return strdup(buf);
}

static int apply_awb_mode(int mode, uint32_t ct)
{
	typedef MI_S32 (*fn_get_t)(uint32_t, AwbAttr_t *);
	typedef MI_S32 (*fn_set_t)(uint32_t, AwbAttr_t *);
	typedef MI_S32 (*fn_ctmwb_t)(uint32_t, AwbCtMwb_t *);
	void *handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	int ret = 0;

	if (!handle)
		return -1;

	/* Push the colour temperature on EVERY apply, not just under ct_manual.
	 * Both isp.awbCt and isp.awbMode route here, so writing it only in the
	 * manual branch meant setting awbCt while in auto was recorded in config
	 * but never reached the hardware register: /api/v1/awb reported one CT
	 * while config claimed another, silently, until the next restart.  The
	 * write does not itself change who drives AWB — the mode attr set below
	 * does that — so priming it here simply keeps config and ISP in step. */
	if (ct > 0) {
		fn_ctmwb_t fn_ctmwb = (fn_ctmwb_t)dlsym(handle,
			"MI_ISP_AWB_SetCTMwbAttr");

		if (fn_ctmwb) {
			AwbCtMwb_t mwb = { .u32CT = ct };
			MI_S32 ct_ret = fn_ctmwb(0, &mwb);

			if (ct_ret != 0) {
				fprintf(stderr, "WARNING: MI_ISP_AWB_SetCTMwbAttr(%u) "
					"failed: 0x%08x\n", ct, (unsigned)ct_ret);
				ret = -1;
			}
		}
	}

	if (mode == 0) {
		fn_get_t fn_get = (fn_get_t)dlsym(handle,
			"MI_ISP_AWB_GetAttr");
		fn_set_t fn_set = (fn_set_t)dlsym(handle,
			"MI_ISP_AWB_SetAttr");

		if (fn_get && fn_set) {
			AwbAttr_t attr;

			memset(&attr, 0, sizeof(attr));
			if (fn_get(0, &attr) == 0) {
				MI_S32 awb_ret;

				attr.eState = 0;
				attr.eOpType = STAR6E_AWB_MODE_AUTO;
				awb_ret = fn_set(0, &attr);
				if (awb_ret != 0) {
					fprintf(stderr, "WARNING: MI_ISP_AWB_SetAttr(auto) failed: 0x%08x\n",
						(unsigned)awb_ret);
					ret = -1;
				} else {
					const VencConfig *vc =
						g_star6e_control_ctx.vcfg;

					printf("> AWB mode: auto\n");
					/* Userspace loop owns AWB in auto —
					 * the ISP-internal algorithm does not
					 * converge on this platform.  Only hand
					 * it over when that loop is actually
					 * running: with awbFps=0 nothing would
					 * drive AWB at all, which is worse than
					 * the non-converging internal one. */
					if (vc && vc->isp.awb_fps > 0) {
						star6e_pipeline_set_awb_userspace(1);
						star6e_awb_set_paused(0);
					} else {
						star6e_pipeline_set_awb_userspace(0);
					}
				}
			} else {
				ret = -1;
			}
		} else {
			ret = -1;
		}
	} else {
		fn_get_t fn_get = (fn_get_t)dlsym(handle, "MI_ISP_AWB_GetAttr");
		fn_set_t fn_set = (fn_set_t)dlsym(handle, "MI_ISP_AWB_SetAttr");

		if (fn_get && fn_set) {
			/* CT was written above — for both modes. */
			{
				MI_S32 awb_ret;
				AwbAttr_t attr;

				memset(&attr, 0, sizeof(attr));
				if (fn_get(0, &attr) == 0) {
					attr.eState = 0;
					attr.eOpType = STAR6E_AWB_MODE_CT_MANUAL;
					awb_ret = fn_set(0, &attr);
					if (awb_ret != 0) {
						fprintf(stderr, "WARNING: MI_ISP_AWB_SetAttr(ct_manual) failed: 0x%08x\n",
							(unsigned)awb_ret);
						ret = -1;
					} else {
						printf("> AWB mode: ct_manual (%uK)\n", ct);
						/* ISP-internal owns AWB again so
						 * the CT apply above reaches the
						 * hardware; our loop stands down. */
						star6e_awb_set_paused(1);
						star6e_pipeline_set_awb_userspace(0);
					}
				} else {
					ret = -1;
				}
			}
		} else {
			ret = -1;
		}
	}

	dlclose(handle);
	return ret;
}

/* Live rate change for the userspace AWB loop.  Retuning the rate is just a
 * volatile store the loop thread picks up on its next wake, but crossing 0 in
 * either direction also moves ownership of the AWB module, so it has to
 * follow the same rule the pipeline uses at startup: hand AWB to userspace
 * only while this loop is running AND the operator asked for auto. */
static int apply_awb_rate(uint32_t hz)
{
	const VencConfig *vc = g_star6e_control_ctx.vcfg;
	int manual = vc && strcmp(vc->isp.awb_mode, "auto") != 0;

	if (hz == 0) {
		star6e_pipeline_set_awb_userspace(0);
		star6e_awb_stop();
		printf("> AWB: userspace loop stopped (awbFps=0)\n");
		return 0;
	}

	/* Already running: this is a no-op start that just retunes the rate. */
	if (star6e_awb_start(hz) != 0)
		return -1;
	star6e_awb_set_paused(manual);
	star6e_pipeline_set_awb_userspace(!manual);
	return 0;
}

static int apply_output_enabled(bool on)
{
	uint32_t restored_fps;

	if (!g_star6e_control_ctx.pipeline)
		return -1;

	if (on) {
		/* Enabling an already-enabled output is not a bootstrap event:
		 * nothing was re-created and no consumer lost its parameter
		 * sets.  Firing the un-coalescible IDR on it turned an
		 * unchanged re-POST into an ungated keyframe at the caller's
		 * request rate, and a forced IDR re-arms the spacing anchor,
		 * starving the scene detector for the duration.
		 *
		 * The guard belongs HERE, not above the branch: the disable
		 * arm has side effects beyond the flag — it captures
		 * stored_fps and idles the encoder — and apply_fps() does not
		 * consult output_enabled.  Guarding both arms let a re-POST
		 * carrying video0.fps AND enabled=false rebind to full rate
		 * and then skip the re-idle, leaving the encoder running flat
		 * out with the output off.  Re-running the ENABLE arm is not
		 * safe the same way: it would restore a stale stored_fps over
		 * an fps the caller set in the same batch. */
		if (g_star6e_control_ctx.pipeline->output_enabled)
			return 0;
		if (!g_star6e_control_ctx.vcfg ||
		    !g_star6e_control_ctx.vcfg->outgoing.server[0]) {
			fprintf(stderr, "> Cannot enable output: no server configured\n");
			return -1;
		}
		g_star6e_control_ctx.pipeline->output_enabled = 1;
		restored_fps = g_star6e_control_ctx.pipeline->stored_fps ?
			g_star6e_control_ctx.pipeline->stored_fps :
			g_star6e_control_ctx.vcfg->video0.fps;
		if (apply_fps(restored_fps) != 0) {
			g_star6e_control_ctx.pipeline->output_enabled = 0;
			return -1;
		}
		/* Not fatal.  The output IS enabled and the fps restored; a
		 * failed keyframe request is a slower first picture, not a
		 * failed apply, and unwinding output_enabled here would leave
		 * the restored fps behind on a disabled output.  Recovery has
		 * other routes (the explicit endpoint, and the receiver's own
		 * request), so log it and keep the work. */
		if (request_idr_bootstrap() != 0)
			fprintf(stderr, "WARN: output enabled but the bootstrap "
				"IDR request failed; the receiver has no start "
				"point until the next one\n");
		printf("> Output enabled, FPS restored to %u\n", restored_fps);
	} else {
		g_star6e_control_ctx.pipeline->output_enabled = 0;
		g_star6e_control_ctx.pipeline->stored_fps = g_star6e_control_ctx.vcfg ?
			g_star6e_control_ctx.vcfg->video0.fps : 30;
		if (apply_fps(STAR6E_CONTROLS_IDLE_FPS) != 0) {
			g_star6e_control_ctx.pipeline->output_enabled = 1;
			return -1;
		}
		printf("> Output disabled, FPS reduced to %u (idle)\n",
			STAR6E_CONTROLS_IDLE_FPS);
	}

	return 0;
}

static int apply_server(const char *uri)
{
	if (!g_star6e_control_ctx.pipeline)
		return -1;
	/* Re-pointing at the destination already in use is not a bootstrap
	 * event — see apply_output_enabled() above.  Compare before touching
	 * the socket so an unchanged re-POST costs nothing at all.
	 *
	 * Compare the destination we actually APPLIED, never vcfg: venc_api's
	 * apply_live_group_for_cfg() commits the new config into g_cfg before
	 * dispatching here, so a vcfg comparison is equal even for a real
	 * change — which skipped the repoint entirely and left
	 * /api/v1/config advertising a destination venc was not sending to.
	 * Device-verified on Star6E: the socket stayed on the startup
	 * destination while the API reported the new one. */
	if (uri && g_star6e_control_ctx.applied_server[0] &&
	    strcmp(g_star6e_control_ctx.applied_server, uri) == 0)
		return 0;
	if (star6e_output_apply_server(&g_star6e_control_ctx.pipeline->output,
	    uri) != 0) {
		return -1;
	}
	/* Not fatal, and here it is actively important: the socket has ALREADY
	 * been repointed above, so returning -1 sends venc_api into
	 * rollback_live_groups(), which re-enters this same failing call --
	 * and if that fails too it commits the NEW outgoing.server while the
	 * socket sits on the OLD one, leaving /api/v1/config advertising a
	 * destination venc is not sending to.  Maruko already treated this as
	 * non-fatal; the backends now genuinely agree. */
	snprintf(g_star6e_control_ctx.applied_server,
		sizeof(g_star6e_control_ctx.applied_server), "%s", uri);
	if (request_idr_bootstrap() != 0)
		fprintf(stderr, "WARN: destination changed but the bootstrap "
			"IDR request failed; the new receiver has no start "
			"point until the next one\n");
	printf("> Destination changed to %s\n", uri);
	return 0;
}

static int apply_max_payload_size(uint16_t size)
{
	Star6ePipelineState *ps = g_star6e_control_ctx.pipeline;

	if (!ps)
		return -1;

	/* Validation enforces [VENC_OUTPUT_PAYLOAD_MIN_BYTES,
	 * VENC_OUTPUT_PAYLOAD_CEILING_BYTES] and the SHM ring is sized to
	 * the ceiling at startup, so any value reaching here fits every
	 * transport. Plain uint16_t stores are atomic on ARM; readers
	 * (encoder thread, audio compact thread) re-read once per frame.
	 * The next frame uses the new value; in-flight fragmentation for
	 * the current frame keeps the old. */
	ps->video.rtp_payload_size = size;
	ps->video.max_frame_size = size;
	if (ps->dual) {
		ps->dual->video.rtp_payload_size = size;
		ps->dual->video.max_frame_size = size;
	}
	/* Audio compact-mode chunking reads this on every audio frame
	 * (star6e_audio_output_send_compact). RTP audio doesn't fragment so
	 * the field is unused there, but we keep both modes in sync. */
	ps->audio.output.max_payload_size = size;

	/* Use stderr (unbuffered, bypasses the audio stdout filter pipe in
	 * star6e_audio.c) so the live-apply trace lands in the log even
	 * when stdout is captured by the filter. */
	fprintf(stderr, "> max_payload_size set to %u (live)\n", (unsigned)size);
	return 0;
}

static int apply_mute(bool on)
{
	if (!g_star6e_control_ctx.pipeline)
		return -1;
	if (!g_star6e_control_ctx.pipeline->audio.channel_enabled) {
		printf("[audio] %s (no-op, audio not active)\n",
			on ? "Muted" : "Unmuted");
		return 0;
	}
	if (star6e_audio_apply_mute(&g_star6e_control_ctx.pipeline->audio,
	    on ? 1 : 0) != 0) {
		return -1;
	}

	printf("[audio] %s\n", on ? "Muted" : "Unmuted");
	return 0;
}

static const char *output_transport_name(const Star6eOutput *o)
{
	if (!o)
		return "none";
	if (o->ring)
		return "shm";
	if (o->frame_ring)
		return "frame-shm";
	switch (o->transport) {
	case VENC_OUTPUT_URI_UDP:       return "udp";
	case VENC_OUTPUT_URI_UNIX:      return "unix";
	case VENC_OUTPUT_URI_SHM:       return "shm";
	case VENC_OUTPUT_URI_FRAME_SHM: return "frame-shm";
	default:                        return "none";
	}
}

static char *query_transport_status(void)
{
	Star6ePipelineState *ps = g_star6e_control_ctx.pipeline;
	char buf[640];
	const char *transport;
	int pos;
	uint32_t pressure_drops;

	if (!ps)
		return NULL;
	transport = output_transport_name(&ps->output);

	/* `pressure_drops` accumulates while a sidecar probe is subscribed
	 * (the only time observation runs).  Off-thread RELAXED load —
	 * naturally aligned, no ordering needed for telemetry. */
	pressure_drops = __atomic_load_n(&ps->output.pressure_drops,
		__ATOMIC_RELAXED);

	if (ps->output.ring) {
		venc_ring_fill_t fill;
		int in_pressure;
		if (venc_ring_get_fill(ps->output.ring, &fill) != 0)
			return NULL;
		/* HTTP `inPressure` is a point-in-time snapshot derived
		 * from the live fill_pct queried for this response — NOT
		 * the cached hysteresis flag.  The cached flag only updates
		 * while a sidecar probe is subscribed and would go stale
		 * once the probe disconnected, leaving HTTP readers seeing
		 * "true" against an empty ring.  The trailer keeps the
		 * hysteresis flag for adaptive consumers; HTTP wants
		 * "is the ring full right now?". */
		in_pressure = fill.fill_pct >= VENC_PRESSURE_HIGH_WATER_PCT;
		pos = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{"
			"\"active\":true,"
			"\"transport\":\"%s\","
			"\"fillPct\":%u,"
			"\"inPressure\":%s,"
			"\"transportDrops\":%u,"
			"\"pressureDrops\":%u,"
			"\"packetsSent\":%llu,"
			"\"oversizeDrops\":%llu,"
			"\"slotCount\":%u,"
			"\"usedSlots\":%u,"
			"\"badAuDrops\":%llu}}",
			transport,
			(unsigned)fill.fill_pct,
			in_pressure ? "true" : "false",
			(unsigned)fill.full_drops,
			(unsigned)pressure_drops,
			(unsigned long long)fill.writes,
			(unsigned long long)fill.oversize_drops,
			(unsigned)fill.slot_count,
			(unsigned)fill.used_slots,
			(unsigned long long)__atomic_load_n(
				&ps->output.bad_au_drops, __ATOMIC_RELAXED));
	} else if (ps->output.frame_ring) {
		venc_frame_ring_fill_t fill;
		char gate_json[FRAME_GATE_STATUS_JSON_CAP];
		int in_pressure;
		if (venc_frame_ring_get_fill(ps->output.frame_ring, &fill) != 0)
			return NULL;
		in_pressure = fill.fill_pct >= VENC_PRESSURE_HIGH_WATER_PCT;
		frame_gate_status_json(&ps->frame_gate, wb_monotonic_us(),
			gate_json, sizeof(gate_json));
		pos = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{"
			"\"active\":true,"
			"\"transport\":\"%s\","
			"\"fillPct\":%u,"
			"\"inPressure\":%s,"
			"\"transportDrops\":%u,"
			"\"pressureDrops\":%u,"
			"\"framesSent\":%llu,"
			"\"oversizeDrops\":%llu,"
			"\"slotCount\":%u,"
			"\"usedSlots\":%u,"
			"\"ringLowWaterSlots\":%u,"
			"\"otherDrops\":%llu,"
			"\"badAuDrops\":%llu%s}}",
			transport,
			(unsigned)fill.fill_pct,
			in_pressure ? "true" : "false",
			(unsigned)fill.full_drops,
			(unsigned)pressure_drops,
			(unsigned long long)fill.writes,
			(unsigned long long)fill.oversize_drops,
			(unsigned)fill.slot_count,
			(unsigned)fill.used_slots,
			(unsigned)venc_ring_low_water_slots(&ps->output.low_water),
			(unsigned long long)fill.other_drops,
			(unsigned long long)__atomic_load_n(
				&ps->output.bad_au_drops, __ATOMIC_RELAXED),
			gate_json);
	} else if ((ps->output.transport == VENC_OUTPUT_URI_UNIX ||
	            ps->output.transport == VENC_OUTPUT_URI_UDP) &&
	           ps->output.socket_handle >= 0) {
		uint8_t fill_pct = 0;
		int in_pressure;
		if (output_socket_get_fill_pct(ps->output.socket_handle,
		    &ps->output.send_queue, &fill_pct) != 0)
			fill_pct = 0;
		in_pressure = fill_pct >= VENC_PRESSURE_HIGH_WATER_PCT;
		pos = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{"
			"\"active\":true,"
			"\"transport\":\"%s\","
			"\"fillPct\":%u,"
			"\"inPressure\":%s,"
			"\"pressureDrops\":%u,"
			"\"transportDrops\":%u,"
			"\"packetsSent\":%u,"
			"\"badAuDrops\":%llu}}",
			transport,
			(unsigned)fill_pct,
			in_pressure ? "true" : "false",
			(unsigned)pressure_drops,
			(unsigned)__atomic_load_n(&ps->output.socket_drops,
				__ATOMIC_RELAXED),
			(unsigned)__atomic_load_n(&ps->output.socket_writes,
				__ATOMIC_RELAXED),
			(unsigned long long)__atomic_load_n(
				&ps->output.bad_au_drops, __ATOMIC_RELAXED));
	} else {
		pos = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{"
			"\"active\":false,"
			"\"transport\":\"%s\"}}",
			transport);
	}
	if (pos < 0 || pos >= (int)sizeof(buf))
		return NULL;
	return strdup(buf);
}

static char *query_audio_status(void)
{
	Star6ePipelineState *ps = g_star6e_control_ctx.pipeline;
	if (!ps)
		return NULL;
	return star6e_audio_query_status(&ps->audio);
}

static int apply_isp_bin(const char *path)
{
	const char *sensor_name = NULL;
	MI_SNR_PAD_ID_e pad_id = 0;

	if (!g_star6e_control_ctx.vcfg)
		return -1;

	if (g_star6e_control_ctx.pipeline) {
		sensor_name = g_star6e_control_ctx.pipeline->sensor.plane.sensName;
		pad_id = g_star6e_control_ctx.pipeline->sensor.pad_id;
	}

	return star6e_pipeline_load_isp_bin_live(path, g_star6e_control_ctx.vcfg,
		sensor_name, pad_id, g_star6e_control_ctx.sensor_fps);
}

static char *query_attitude(void)
{
	return star6e_attitude_query();
}

/* Detector live model swap.  The swap itself (VPE port1 recreate + plugin
 * dlopen/dlclose) MUST run on the pipeline (encode) thread — the same thread
 * that queries the detect snapshot every frame — so it is atomic w.r.t. that
 * consumer and the failure path (which frees the detector context) can never
 * race a snapshot() read.  The HTTP apply callback therefore only posts the
 * request here and waits (bounded) for the pipeline thread to service it via
 * star6e_controls_service_detect_reload(); a single slot coalesces bursts.
 *
 * The request carries a full VencConfig SNAPSHOT taken while the caller holds
 * g_cfg_mutex, and the pipeline thread reloads from a private copy of it — it
 * never reads the live g_cfg during the (potentially >3s) NPU graph load.  So
 * the bounded wait is purely a courtesy timeout: even if it expires and the
 * HTTP path re-commits g_cfg (or a second /set arrives), the in-flight reload
 * keeps using its stable snapshot — no torn model_path read. */
static struct {
	pthread_mutex_t lock;
	pthread_cond_t  cond;
	int             pending;   /* request posted, not yet serviced */
	int             done;      /* pipeline thread finished the swap  */
	VencConfig      cfg;       /* config snapshot for the pending swap */
} g_detect_reload = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.cond = PTHREAD_COND_INITIALIZER,
};

static int apply_detect_reload(void)
{
	struct timespec deadline;

	if (!g_star6e_control_ctx.pipeline || !g_star6e_control_ctx.vcfg)
		return 0;   /* no pipeline bound — best-effort no-op */

	pthread_mutex_lock(&g_detect_reload.lock);
	/* Caller holds g_cfg_mutex, so *vcfg (== g_cfg) is a coherent read. */
	g_detect_reload.cfg = *g_star6e_control_ctx.vcfg;
	g_detect_reload.pending = 1;
	g_detect_reload.done = 0;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 3;   /* the pipeline thread services within a frame */
	while (!g_detect_reload.done) {
		if (pthread_cond_timedwait(&g_detect_reload.cond,
		    &g_detect_reload.lock, &deadline) == ETIMEDOUT)
			break;
	}
	pthread_mutex_unlock(&g_detect_reload.lock);
	/* Always report success: the request is committed and the swap is
	 * best-effort (a failed load leaves detection off but keeps the
	 * requested config).  Returning an error would trigger a config
	 * rollback that cannot restore the already-torn-down old graph. */
	return 0;
}

void star6e_controls_service_detect_reload(void)
{
	VencConfig cfg;
	int do_it;

	pthread_mutex_lock(&g_detect_reload.lock);
	do_it = g_detect_reload.pending;
	g_detect_reload.pending = 0;
	if (do_it)
		cfg = g_detect_reload.cfg;   /* private copy, stable across the reload */
	pthread_mutex_unlock(&g_detect_reload.lock);
	if (!do_it)
		return;

	if (g_star6e_control_ctx.pipeline) {
		Star6ePipelineState *ps = g_star6e_control_ctx.pipeline;

		/* detect.enabled is applied HERE rather than by a MUT_RESTART
		 * respawn.  A respawn from a detect-active instance leaves the
		 * successor permanently frameless: the ISP CMDQ stays stuck
		 * mid-WAIT on ISP_TRIG and storms `ISP_IRQ_WQ_FRAME_START add WQ
		 * error!` forever.  Device-localized on .232 to the inherited
		 * /dev/mi_sys fd — the respawn child holds a copy, so that open
		 * file's driver release never runs.  Closing it in the child is
		 * this SoC's confirmed deadlock (see venc_respawn.c), and closing
		 * every OTHER /dev/mi_* plus settling 8 s did not help, so no
		 * fork+exec respawn can recover.  Toggling the tap in place skips
		 * the respawn entirely — and costs no stream outage.
		 *
		 * Same thread, same atomicity as the model swap below, so the
		 * per-frame snapshot() consumer never sees a half-built detector. */
		if (!cfg.detect.enabled) {
			if (ps->detect) {
				fprintf(stderr, "[ipu-yolo] detect.enabled=false: "
					"stopping detector\n");
				star6e_pipeline_detect_stop(ps);
			}
		} else if (!ps->detect) {
			fprintf(stderr, "[ipu-yolo] detect.enabled=true: "
				"starting detector\n");
			(void)star6e_pipeline_detect_start(ps, &cfg);
		} else {
			(void)star6e_ipu_yolo_reload(ps, &cfg);
			/* If the swap left the detector down (bad new model, thread
			 * create failure), free the port1 claim so runtime.vpe_taps
			 * reads truthfully.  A no-op when "detect" does not own
			 * port1. */
			if (!ps->detect)
				star6e_vpe_port1_release("detect");
		}
	}

	pthread_mutex_lock(&g_detect_reload.lock);
	g_detect_reload.done = 1;
	pthread_cond_broadcast(&g_detect_reload.cond);
	pthread_mutex_unlock(&g_detect_reload.lock);
}

static int attitude_calibrate_level(float *roll_deg, float *pitch_deg)
{
	if (!g_star6e_control_ctx.vcfg)
		return -1;
	return star6e_attitude_calibrate_level(g_star6e_control_ctx.vcfg,
		roll_deg, pitch_deg);
}

static const VencApplyCallbacks g_star6e_apply_callbacks = {
	.apply_bitrate = apply_bitrate,
	.apply_qp_bounds = apply_qp_bounds,
	.apply_fps = apply_fps_live,
	.apply_gop = apply_gop,
	.apply_qp_delta = apply_qp_delta,
	.apply_roi_qp = apply_roi_qp,
	.apply_gain_max = apply_gain_max,
	.apply_shutter_max = apply_shutter_max,
	.apply_gain_min = apply_gain_min,
	.apply_shutter_min = apply_shutter_min,
	.apply_verbose = apply_verbose,
	.apply_output_enabled = apply_output_enabled,
	.apply_server = apply_server,
	.apply_mute = apply_mute,
	.request_idr = request_idr,
	.query_live_fps = query_live_fps,
	.query_ae_info = query_ae_info,
	.query_awb_info = query_awb_info,
	.query_isp_metrics = query_isp_metrics,
	.apply_awb_mode = apply_awb_mode,
	.apply_awb_rate = apply_awb_rate,
	.query_iq_info = star6e_iq_query,
	.apply_iq_param = star6e_iq_set,
	.apply_max_payload_size = apply_max_payload_size,
	.query_transport_status = query_transport_status,
	.query_audio_status = query_audio_status,
	.apply_zoom = apply_zoom,
	.apply_isp_bin = apply_isp_bin,
	.apply_snapshot_quality = venc_jpeg_set_quality,
	.apply_pause_stab = apply_pause_stab,
	.query_attitude = query_attitude,
	.attitude_calibrate_level = attitude_calibrate_level,
	.apply_detect_reload = apply_detect_reload,
};

void star6e_controls_bind(Star6ePipelineState *pipeline, VencConfig *vcfg)
{
	memset(&g_star6e_control_ctx, 0, sizeof(g_star6e_control_ctx));
	if (!pipeline || !vcfg)
		return;

	g_star6e_control_ctx.venc_chn = pipeline->venc_channel;
	g_star6e_control_ctx.vpe_port = pipeline->vpe_port;
	g_star6e_control_ctx.venc_port = pipeline->venc_port;
	g_star6e_control_ctx.sensor_fps = pipeline->sensor.mode.maxFps ?
		pipeline->sensor.mode.maxFps : pipeline->video.sensor_framerate;
	/* Mirror the create-path VPE->VENC bind: delivery = video0.fps
	 * clamped to the mode's true rate (0 = full rate). */
	g_star6e_control_ctx.delivered_fps = g_star6e_control_ctx.sensor_fps;
	if (vcfg->video0.fps &&
	    vcfg->video0.fps < g_star6e_control_ctx.delivered_fps)
		g_star6e_control_ctx.delivered_fps = vcfg->video0.fps;
	g_star6e_control_ctx.frame_width = pipeline->image_width;
	g_star6e_control_ctx.frame_height = pipeline->image_height;
	g_star6e_control_ctx.pipeline = pipeline;
	g_star6e_control_ctx.vcfg = vcfg;
	/* Seed from the create path so the first live set naming the
	 * already-active destination is correctly seen as unchanged -- but
	 * ONLY if the output actually came up.  Seeding from config over a
	 * failed bring-up would make a re-POST of the same URI a no-op and
	 * remove the one route back: re-POSTing the destination to retry
	 * output_socket_configure().  Left empty, the guard falls through and
	 * the retry happens. */
	if (pipeline->output.ring || pipeline->output.frame_ring ||
	    pipeline->output.socket_handle >= 0)
		snprintf(g_star6e_control_ctx.applied_server,
			sizeof(g_star6e_control_ctx.applied_server), "%s",
			vcfg->outgoing.server);
}

void star6e_controls_reset(void)
{
	memset(&g_star6e_control_ctx, 0, sizeof(g_star6e_control_ctx));
}

const VencApplyCallbacks *star6e_controls_callbacks(void)
{
	return &g_star6e_apply_callbacks;
}

int star6e_controls_apply_fps(uint32_t fps)
{
	return apply_fps(fps);
}

int star6e_controls_apply_roi_qp(int qp)
{
	return apply_roi_qp(qp);
}

int star6e_controls_apply_qp_delta(int delta)
{
	return apply_qp_delta(delta);
}
