#include "timing.h"
#include "maruko_controls.h"

#include "idr_rate_limit.h"
#include "maruko_audio.h"
#include "maruko_bindings.h"
#include "maruko_cus3a.h"
#include "maruko_framing.h"
#include "maruko_framing_host.h"
#include "maruko_iq.h"
#include "maruko_ipu_yolo.h"
#include "maruko_output.h"
#include "maruko_pipeline.h"
#include "output_socket.h"
#include "pipeline_common.h"
#include "venc_config.h"
#include "venc_api.h"
#include "venc_jpeg.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── ISP type definitions (match SigmaStar SDK ABI) ──────────────────── */

typedef struct {
	unsigned int minShutterUs;
	unsigned int maxShutterUs;
	unsigned int minApertX10;
	unsigned int maxApertX10;
	unsigned int minSensorGain;
	unsigned int minIspGain;
	unsigned int maxSensorGain;
	unsigned int maxIspGain;
} MarukoIspExposureLimit;

typedef struct {
	uint32_t u32FNx10;
	uint32_t u32SensorGain;
	uint32_t u32ISPGain;
	uint32_t u32US;
} MarukoAeExpoValue;

typedef struct {
	uint32_t u32LumY;
	uint32_t u32AvgY;
	uint32_t u32Hits[128];
} MarukoAeHistWeightY;

typedef struct {
	int bIsStable;
	int bIsReachBoundary;
	MarukoAeExpoValue stExpoValueLong;
	MarukoAeExpoValue stExpoValueShort;
	MarukoAeHistWeightY stHistWeightY;
	uint32_t u32LVx10;
	int32_t s32BV;
	uint32_t u32SceneTarget;
} MarukoAeExpoInfo;

typedef struct {
	int plane_ret;
	int limit_ret;
	int info_ret;
	int state_ret;
	int mode_ret;
	MI_SNR_PAD_ID_e pad_id;
	MI_SNR_PlaneInfo_t plane;
	MarukoIspExposureLimit limit;
	MarukoAeExpoInfo info;
	int ae_state;
	int ae_mode_raw;
} MarukoAeDiagSnapshot;

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
} MarukoAwbQueryInfo;

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
	/* i6c adds sync 3A fields here — pad to avoid buffer overrun */
	uint8_t _sync3a_pad[32];
} __attribute__((packed, aligned(1))) MarukoAwbCus3aInfo;

typedef struct {
	int bEnable;
	uint16_t u16GlbGainThd;
	uint16_t u16CountThd;
	uint16_t u16ForceTriGainThd;
} MarukoAwbStabilizer;

typedef struct {
	uint32_t u32CT;
} MarukoAwbCtMwb;

typedef struct {
	int eState;
	int eOpType;
	uint16_t mwb_rgain;
	uint16_t mwb_grgain;
	uint16_t mwb_gbgain;
	uint16_t mwb_bgain;
	uint8_t _pad[8192];
} MarukoAwbAttr;

enum {
	MARUKO_AWB_MODE_AUTO = 0,
	MARUKO_AWB_MODE_MANUAL = 1,
	MARUKO_AWB_MODE_CT_MANUAL = 2,
};

enum {
	MARUKO_ISP_STATE_NORMAL = 0,
	MARUKO_ISP_STATE_PAUSE = 1,
	MARUKO_AE_EXPO_MODE_AUTO = 0,
};

/* ── Control context ─────────────────────────────────────────────────── */

typedef struct {
	MI_VENC_DEV venc_dev;
	MI_VENC_CHN venc_chn;
	int *verbose_ptr;
	MI_SNR_PAD_ID_e pad_id;
	uint32_t sensor_fps;
	uint32_t frame_width;
	uint32_t frame_height;
	VencConfig *vcfg;
	MarukoBackendConfig *backend_cfg;
	MarukoBackendContext *backend;   /* full backend handle for output/ring access */
	MI_SYS_ChnPort_t vpe_port;
	MI_SYS_ChnPort_t venc_port;
	volatile sig_atomic_t *output_enabled_ptr;
	volatile uint32_t *stored_fps_ptr;
	/* Destination the output socket is ACTUALLY pointed at -- see the note
	 * in maruko_apply_server().  Never derive this from vcfg.  Held here
	 * rather than as a file-scope global, matching
	 * Star6eControlContext::applied_server. */
	char applied_server[VENC_CONFIG_STRING_MAX];
} MarukoControlContext;

enum {
	MARUKO_CONTROLS_IDLE_FPS = 5,
};

static MarukoControlContext g_ctx;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int maruko_apply_rc_qp_delta(const i6c_venc_chn *attr, MI_VENC_RcParam_t *param,
	int delta)
{
	if (!attr || !param || delta < -12 || delta > 12)
		return -1;

	switch (attr->rate.mode) {
	case MARUKO_VENC_RC_H265_CBR:
		param->stParamH265Cbr.s32IPQPDelta = delta;
		return 0;
	case MARUKO_VENC_RC_H264_CBR:
		param->stParamH264Cbr.s32IPQPDelta = delta;
		return 0;
	case MARUKO_VENC_RC_H265_VBR:
		param->stParamH265Vbr.s32IPQPDelta = delta;
		return 0;
	case MARUKO_VENC_RC_H264_VBR:
		param->stParamH264VBR.s32IPQPDelta = delta;
		return 0;
	case MARUKO_VENC_RC_H265_AVBR:
		param->stParamH265Avbr.s32IPQPDelta = delta;
		return 0;
	case MARUKO_VENC_RC_H264_AVBR:
		param->stParamH264Avbr.s32IPQPDelta = delta;
		return 0;
	default:
		return -1;
	}
}

/* Only CBR carries QP bounds we can steer; other modes leave them alone. */
static int maruko_rc_qp_bound_ptrs(const i6c_venc_chn *attr,
	MI_VENC_RcParam_t *param, uint32_t **pmin, uint32_t **pmax)
{
	if (!attr || !param)
		return -1;
	switch (attr->rate.mode) {
	case MARUKO_VENC_RC_H265_CBR:
		*pmin = &param->stParamH265Cbr.u32MinQp;
		*pmax = &param->stParamH265Cbr.u32MaxQp;
		return 0;
	case MARUKO_VENC_RC_H264_CBR:
		*pmin = &param->stParamH264Cbr.u32MinQp;
		*pmax = &param->stParamH264Cbr.u32MaxQp;
		return 0;
	default:
		return -1;
	}
}

/* Everything we have asked the rate controller for.  Maruko is the same MI
 * VENC RC as Star6E and therefore has the same hazard: MI_VENC_GetRcParam
 * does not reflect a just-written value until the driver commits the block
 * (~5 s after StartRecvPic on SSC338Q), so a second read-modify-write inside
 * that window reads a stale 0 and writes it back.  Adding qp bounds gives
 * Maruko a second RC writer for the first time, so it gets Star6E's fix at
 * the same moment rather than inheriting the bug -- see g_rc_intent in
 * src/star6e_controls.c and issue #255. */
static struct {
	int      qp_delta;
	uint32_t min_qp;   /* 0 = driver default */
	uint32_t max_qp;   /* 0 = driver default */
} g_maruko_rc_intent;

/* Driver-default QP bounds, captured on the first Get before any write so a
 * later 0 can restore them.  The rate mode is fixed for the process lifetime
 * (Maruko always respawns on reinit), so one capture is enough. */
static struct {
	int      captured;
	uint32_t min_qp;
	uint32_t max_qp;
} g_maruko_qp_defaults;

/* Write the whole intent, never a patched Get result. */
static int maruko_rc_commit_intent(int require_bounds)
{
	i6c_venc_chn attr = {0};
	MI_VENC_RcParam_t param = {0};
	uint32_t *pmin, *pmax;

	if (maruko_mi_venc_get_chn_attr(g_ctx.venc_dev, g_ctx.venc_chn, &attr) != 0)
		return -1;
	if (maruko_mi_venc_get_rc_param(g_ctx.venc_dev, g_ctx.venc_chn, &param) != 0)
		return -1;
	if (maruko_apply_rc_qp_delta(&attr, &param, g_maruko_rc_intent.qp_delta) != 0)
		return -1;
	if (maruko_rc_qp_bound_ptrs(&attr, &param, &pmin, &pmax) == 0) {
		if (!g_maruko_qp_defaults.captured) {
			g_maruko_qp_defaults.min_qp = *pmin;
			g_maruko_qp_defaults.max_qp = *pmax;
			g_maruko_qp_defaults.captured = 1;
		}
		*pmin = g_maruko_rc_intent.min_qp ? g_maruko_rc_intent.min_qp
						  : g_maruko_qp_defaults.min_qp;
		*pmax = g_maruko_rc_intent.max_qp ? g_maruko_rc_intent.max_qp
						  : g_maruko_qp_defaults.max_qp;
		/* The API validator only compares min against max when BOTH are
		 * non-zero, so a half-specified pair can still resolve to min > max
		 * against the driver default.  CV610's sibling refuses this and says
		 * why: "the SDK takes that without complaint and then behaves
		 * erratically".  Maruko could not reach it before qp bounds were
		 * wired up here, so guard it at the same time. */
		if (*pmin > *pmax) {
			fprintf(stderr, "ERROR: qpBounds min>max after resolving "
				"defaults (%u/%u)\n", (unsigned)*pmin, (unsigned)*pmax);
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
	return maruko_mi_venc_set_rc_param(g_ctx.venc_dev, g_ctx.venc_chn,
		&param) == 0 ? 0 : -1;
}

/* ── Basic controls (existing) ───────────────────────────────────────── */

/* No IDR request on bitrate writes — see the matching note in
 * src/star6e_controls.c apply_bitrate() for the measurement.
 *
 * Compile-tested only here.  Operator ruling 2026-08-26: assume i6c
 * set_chn_attr keyframes implicitly the way Star6E's MI_VENC_SetChnAttr was
 * measured to, and keep the two backends symmetric rather than diverging on
 * an untested difference.  Stated as an assumption because it is one — if a
 * Maruko ever reaches a bench, the check is ten spaced video0.bitrate writes
 * against a count of IRAP access units (not /api/v1/idr/stats, which only
 * sees requests and is blind to an SDK-implicit keyframe). */
static int maruko_apply_bitrate(uint32_t kbps)
{
	i6c_venc_chn attr = {0};
	if (maruko_mi_venc_get_chn_attr(g_ctx.venc_dev,
	    g_ctx.venc_chn, &attr) != 0)
		return -1;
	if (kbps > VENC_BITRATE_MAX_KBPS)
		kbps = VENC_BITRATE_MAX_KBPS;
	if (kbps < VENC_BITRATE_MIN_KBPS)
		kbps = VENC_BITRATE_MIN_KBPS;
	unsigned int bits = kbps * 1024;
	switch (attr.rate.mode) {
	case MARUKO_VENC_RC_H265_CBR:
		attr.rate.h265Cbr.bitrate = bits; break;
	case MARUKO_VENC_RC_H264_CBR:
		attr.rate.h264Cbr.bitrate = bits; break;
	case MARUKO_VENC_RC_H265_VBR:
		attr.rate.h265Vbr.maxBitrate = bits; break;
	case MARUKO_VENC_RC_H264_VBR:
		attr.rate.h264Vbr.maxBitrate = bits; break;
	case MARUKO_VENC_RC_H265_AVBR:
		attr.rate.h265Avbr.maxBitrate = bits; break;
	case MARUKO_VENC_RC_H264_AVBR:
		attr.rate.h264Avbr.maxBitrate = bits; break;
	default:
		return -1;
	}
	if (maruko_mi_venc_set_chn_attr(g_ctx.venc_dev,
	    g_ctx.venc_chn, &attr) != 0)
		return -1;
	return 0;
}

static int maruko_apply_gop(uint32_t gop_size)
{
	i6c_venc_chn attr = {0};
	if (maruko_mi_venc_get_chn_attr(g_ctx.venc_dev,
	    g_ctx.venc_chn, &attr) != 0)
		return -1;
	switch (attr.rate.mode) {
	case MARUKO_VENC_RC_H265_CBR: attr.rate.h265Cbr.gop = gop_size; break;
	case MARUKO_VENC_RC_H264_CBR: attr.rate.h264Cbr.gop = gop_size; break;
	case MARUKO_VENC_RC_H265_VBR: attr.rate.h265Vbr.gop = gop_size; break;
	case MARUKO_VENC_RC_H264_VBR: attr.rate.h264Vbr.gop = gop_size; break;
	case MARUKO_VENC_RC_H265_AVBR: attr.rate.h265Avbr.gop = gop_size; break;
	case MARUKO_VENC_RC_H264_AVBR: attr.rate.h264Avbr.gop = gop_size; break;
	default: return -1;
	}
	return maruko_mi_venc_set_chn_attr(g_ctx.venc_dev,
		g_ctx.venc_chn, &attr) == 0 ? 0 : -1;
}

static int maruko_apply_qp_delta(int delta)
{
	int prev = g_maruko_rc_intent.qp_delta;

	g_maruko_rc_intent.qp_delta = delta;
	if (maruko_rc_commit_intent(0) != 0) {
		g_maruko_rc_intent.qp_delta = prev;
		return -1;
	}

	/* No IDR: a QP-delta change is rate-control state, absorbed mid-GOP.
	 * Star6E parity — see apply_qp_delta() in src/star6e_controls.c. */
	printf("> qpDelta changed to %d\n", delta);
	return 0;
}

/* CBR holds its target by raising QP; these bound how far it may go in each
 * direction.  Both break the rate contract once they bind -- a floor above
 * the scene's natural QP collapses the rate, a ceiling below it overshoots --
 * so they default to 0 (driver default) and the README records the measured
 * behaviour.  Star6E parity: apply_qp_bounds() in src/star6e_controls.c. */
static int maruko_apply_qp_bounds(uint32_t min_qp, uint32_t max_qp)
{
	uint32_t prev_min = g_maruko_rc_intent.min_qp;
	uint32_t prev_max = g_maruko_rc_intent.max_qp;
	if (min_qp == 0 && max_qp == 0 && !g_maruko_qp_defaults.captured)
		return 0;   /* never written — driver defaults already in force */

	/* 0 restores the captured driver default so bounds can be cleared
	 * live, not just overridden. */
	g_maruko_rc_intent.min_qp = min_qp;
	g_maruko_rc_intent.max_qp = max_qp;
	if (maruko_rc_commit_intent(1) != 0) {
		g_maruko_rc_intent.min_qp = prev_min;
		g_maruko_rc_intent.max_qp = prev_max;
		return -1;
	}
	printf("> qpBounds changed: min=%u max=%u (0 = driver default %u/%u)\n",
		min_qp, max_qp, g_maruko_qp_defaults.min_qp,
		g_maruko_qp_defaults.max_qp);
	return 0;
}

static int maruko_request_idr(void);
static int maruko_request_idr_bootstrap(void);
static uint32_t maruko_query_live_fps(void);

static int maruko_apply_fps(uint32_t fps)
{
	i6c_venc_chn attr = {0};
	MI_S32 bind_ret;
	uint32_t sensor_fps;
	int rebound = 0;

	if (fps == 0 || fps > PIPELINE_LIVE_FPS_MAX)
		return -1;

	/* A request above the current mode's max is clamped for the rebind (see
	 * PIPELINE_LIVE_FPS_MAX) — the caller's config still holds the requested
	 * value so a later respawn can select a higher-fps sensor mode for it. */
	sensor_fps = g_ctx.sensor_fps;
	if (fps > sensor_fps) {
		printf("> FPS %u exceeds sensor mode max %u, clamping\n", fps,
			sensor_fps);
		fps = sensor_fps;
	}
	if (fps == maruko_query_live_fps())
		return 0;

	/* stab-fill: there is NO SCL→VENC bind — the VENC is NORMAL_FRMBASE and
	 * manually fed.  The unbind/RING-rebind fps-divider below would force a
	 * RING bind onto the frame-base input and stall the encoder dead
	 * (device-reproduced: live fps set → instant "no encoder data" abort).
	 * The fill-mode divider is the VENC input port's USERINJECT FRC. */
#if HAVE_FRAMING_STAB
	if (maruko_framing_fill_graph_ready()) {
		if (maruko_framing_stab_fill_set_fps(sensor_fps, fps) != 0) {
			printf("> FPS %u: fill FRC update failed\n", fps);
			return -1;
		}
	} else
#endif
	{
		MI_SYS_UnBindChnPort(&g_ctx.vpe_port, &g_ctx.venc_port);
		bind_ret = MI_SYS_BindChnPort2(&g_ctx.vpe_port,
			&g_ctx.venc_port, sensor_fps, fps,
			I6_SYS_LINK_RING, 0);
		if (bind_ret != 0) {
			printf("> Rebind SCL->VENC at %u:%u fps failed %d, restoring\n",
				sensor_fps, fps, bind_ret);
			MI_SYS_BindChnPort2(&g_ctx.vpe_port,
				&g_ctx.venc_port, sensor_fps, sensor_fps,
				I6_SYS_LINK_RING, 0);
			return -1;
		}
	}

	/* The encoder channel HAS been rebound at this point.  Everything
	 * below is best-effort bookkeeping of the RC fpsNum, and it is
	 * skipped outright when the attr read fails or the rate mode is not
	 * one we enumerate -- so a caller must NOT infer "did it change?"
	 * by diffing fpsNum.  Report it in the return value instead. */
	rebound = 1;
	if (maruko_mi_venc_get_chn_attr(g_ctx.venc_dev,
	    g_ctx.venc_chn, &attr) == 0) {
		switch (attr.rate.mode) {
		case MARUKO_VENC_RC_H265_CBR:
			attr.rate.h265Cbr.fpsNum = fps; break;
		case MARUKO_VENC_RC_H264_CBR:
			attr.rate.h264Cbr.fpsNum = fps; break;
		case MARUKO_VENC_RC_H265_VBR:
			attr.rate.h265Vbr.fpsNum = fps; break;
		case MARUKO_VENC_RC_H264_VBR:
			attr.rate.h264Vbr.fpsNum = fps; break;
		case MARUKO_VENC_RC_H265_AVBR:
			attr.rate.h265Avbr.fpsNum = fps; break;
		case MARUKO_VENC_RC_H264_AVBR:
			attr.rate.h264Avbr.fpsNum = fps; break;
		default:
			break;
		}
		maruko_mi_venc_set_chn_attr(g_ctx.venc_dev,
			g_ctx.venc_chn, &attr);
	}

	/* No IDR here — see apply_fps() in src/star6e_controls.c.  Only the
	 * live video0.fps write needs one; it is issued in
	 * maruko_apply_fps_live(). */
	printf("> FPS changed to %u (bind %u:%u)\n", fps, sensor_fps, fps);
	/* 1 = the encoder was rebound, 0 = nothing to do, -1 = failed.
	 * Callers that only care about failure keep testing < 0. */
	return rebound;
}

static int maruko_apply_verbose(bool on)
{
	if (g_ctx.verbose_ptr)
		*g_ctx.verbose_ptr = on ? 1 : 0;
	printf("> Verbose %s via API\n", on ? "enabled" : "disabled");
	return 0;
}

/* Live video0.fps write: the rebind re-creates the encoder channel, so a
 * receiver needs a random-access point.  Star6E parity — see
 * apply_fps_live() in src/star6e_controls.c. */
static int maruko_apply_fps_live(uint32_t fps)
{
	/* Only a real rebind is a bootstrap event, and maruko_apply_fps()
	 * reports that directly rather than leaving us to diff the RC fpsNum:
	 * it rebinds BEFORE writing fpsNum, and skips that write when the attr
	 * read fails or the rate mode is not one we enumerate -- so a diff
	 * reads "unchanged" over a channel that WAS rebound, leaving the
	 * receiver with no random-access point.  Star6E compares delivered_fps,
	 * which its apply_fps() sets on exactly that path. */
	int ret = maruko_apply_fps(fps);

	if (ret < 0)
		return -1;
	if (ret == 1)
		(void)maruko_request_idr_bootstrap();
	return 0;
}

static int maruko_request_idr(void)
{
	/* Through the shared per-channel gate, as Star6E's request_idr() and
	 * CV610's cv610_request_idr() already are.  Without it this endpoint
	 * was neither coalesced against an IDR storm nor visible in
	 * /api/v1/idr/stats, so the contract's claim that IDR sources are
	 * paced and counted equivalently on both SigmaStar backends was not
	 * true for Maruko.  A coalesced request is not an error: the caller
	 * asked for a resync point and one is already in flight. */
	if (!idr_rate_limit_allow(g_ctx.venc_chn))
		return 0;
	return maruko_mi_venc_request_idr(g_ctx.venc_dev,
		g_ctx.venc_chn, 1) == 0 ? 0 : -1;
}

/* Bootstrap form — Star6E parity, see request_idr_bootstrap() in
 * src/star6e_controls.c for why these three sites must not be coalesced. */
static int maruko_request_idr_bootstrap(void)
{
	idr_rate_limit_force(g_ctx.venc_chn);
	return maruko_mi_venc_request_idr(g_ctx.venc_dev,
		g_ctx.venc_chn, 1) == 0 ? 0 : -1;
}

static uint32_t maruko_query_live_fps(void)
{
	i6c_venc_chn attr = {0};

	if (maruko_mi_venc_get_chn_attr(g_ctx.venc_dev,
	    g_ctx.venc_chn, &attr) != 0)
		return 0;

	switch (attr.rate.mode) {
	case MARUKO_VENC_RC_H265_CBR: return attr.rate.h265Cbr.fpsNum;
	case MARUKO_VENC_RC_H264_CBR: return attr.rate.h264Cbr.fpsNum;
	case MARUKO_VENC_RC_H265_VBR: return attr.rate.h265Vbr.fpsNum;
	case MARUKO_VENC_RC_H264_VBR: return attr.rate.h264Vbr.fpsNum;
	case MARUKO_VENC_RC_H265_AVBR: return attr.rate.h265Avbr.fpsNum;
	case MARUKO_VENC_RC_H264_AVBR: return attr.rate.h264Avbr.fpsNum;
	default: return 0;
	}
}

/* ── AE diagnostics (Step 2A) ────────────────────────────────────────── */

static const char *ae_state_name(int state)
{
	return state == MARUKO_ISP_STATE_PAUSE ? "pause" : "normal";
}

static const char *ae_expo_mode_name(int mode)
{
	return mode == MARUKO_AE_EXPO_MODE_AUTO ? "auto" : "unknown";
}

static void ae_diag_defaults(MarukoAeDiagSnapshot *s)
{
	memset(s, 0, sizeof(*s));
	s->pad_id = g_ctx.pad_id;
	s->limit_ret = -1;
	s->info_ret = -1;
	s->state_ret = -1;
	s->mode_ret = -1;
	s->ae_state = MARUKO_ISP_STATE_NORMAL;
	s->ae_mode_raw = MARUKO_AE_EXPO_MODE_AUTO;
}

static void ae_diag_collect(MarukoAeDiagSnapshot *s)
{
	/* i6c ISP functions take (dev, channel, data*) */
	typedef MI_S32 (*fn_get_limit_t)(uint32_t, uint32_t, MarukoIspExposureLimit *);
	typedef MI_S32 (*fn_query_info_t)(uint32_t, uint32_t, MarukoAeExpoInfo *);
	typedef MI_S32 (*fn_get_state_t)(uint32_t, uint32_t, int *);
	typedef MI_S32 (*fn_get_mode_t)(uint32_t, uint32_t, int *);
	void *handle;

	ae_diag_defaults(s);
	s->plane_ret = MI_SNR_GetPlaneInfo(s->pad_id, 0, &s->plane);

	handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!handle)
		return;

	{
		fn_get_limit_t fn = (fn_get_limit_t)dlsym(handle,
			"MI_ISP_AE_GetExposureLimit");
		if (fn)
			s->limit_ret = fn(0, 0, &s->limit);
	}
	{
		fn_query_info_t fn = (fn_query_info_t)dlsym(handle,
			"MI_ISP_AE_QueryExposureInfo");
		if (fn)
			s->info_ret = fn(0, 0, &s->info);
	}
	{
		fn_get_state_t fn = (fn_get_state_t)dlsym(handle,
			"MI_ISP_AE_GetState");
		if (fn)
			s->state_ret = fn(0, 0, &s->ae_state);
	}
	{
		fn_get_mode_t fn = (fn_get_mode_t)dlsym(handle,
			"MI_ISP_AE_GetExpoMode");
		if (fn)
			s->mode_ret = fn(0, 0, &s->ae_mode_raw);
	}

	dlclose(handle);
}

static uint32_t ae_diag_exposure_us(const MarukoAeDiagSnapshot *s)
{
	if (s->info_ret == 0)
		return s->info.stExpoValueLong.u32US;
	return s->plane.shutter;
}

static uint32_t ae_diag_sensor_gain(const MarukoAeDiagSnapshot *s)
{
	if (s->info_ret == 0)
		return s->info.stExpoValueLong.u32SensorGain;
	return s->plane.sensGain;
}

static uint32_t ae_diag_isp_gain(const MarukoAeDiagSnapshot *s)
{
	if (s->info_ret == 0)
		return s->info.stExpoValueLong.u32ISPGain;
	return s->plane.compGain;
}

static uint32_t ae_diag_sensor_fps(void)
{
	if (g_ctx.sensor_fps != 0)
		return g_ctx.sensor_fps;
	if (g_ctx.vcfg)
		return g_ctx.vcfg->video0.fps;
	return 0;
}

static char *maruko_query_ae_info(void)
{
	MarukoAeDiagSnapshot s;
	char buf[2048];
	char precrop_field[96];
	uint32_t exposure_us, sensor_gain, isp_gain;
	uint16_t px = 0, py = 0, pw = 0, ph = 0;

	ae_diag_collect(&s);
	exposure_us = ae_diag_exposure_us(&s);
	sensor_gain = ae_diag_sensor_gain(&s);
	isp_gain = ae_diag_isp_gain(&s);

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
		s.plane_ret, s.pad_id, s.plane.shutter,
		s.plane.sensGain, s.plane.compGain,
		s.limit_ret, s.limit.minShutterUs, s.limit.maxShutterUs,
		s.limit.minSensorGain, s.limit.maxSensorGain,
		s.limit.minIspGain, s.limit.maxIspGain,
		s.info_ret,
		s.info_ret == 0 && s.info.bIsStable ? "true" : "false",
		s.info_ret == 0 && s.info.bIsReachBoundary ? "true" : "false",
		s.info.stExpoValueLong.u32US,
		s.info.stExpoValueLong.u32SensorGain,
		s.info.stExpoValueLong.u32ISPGain,
		s.info.stHistWeightY.u32LumY, s.info.stHistWeightY.u32AvgY,
		s.info.u32LVx10, s.info.s32BV, s.info.u32SceneTarget,
		s.state_ret, s.ae_state, ae_state_name(s.ae_state),
		s.mode_ret, s.ae_mode_raw, ae_expo_mode_name(s.ae_mode_raw),
		exposure_us, sensor_gain, isp_gain, ae_diag_sensor_fps(),
		ae_diag_sensor_fps(), precrop_field);
	return strdup(buf);
}

static char *maruko_query_isp_metrics(void)
{
	MarukoAeDiagSnapshot s;
	char buf[512];

	ae_diag_collect(&s);
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
		ae_diag_sensor_gain(&s),
		ae_diag_isp_gain(&s),
		ae_diag_exposure_us(&s) / 1000,
		ae_diag_sensor_fps(),
		s.info.stHistWeightY.u32LumY,
		s.info.stHistWeightY.u32AvgY,
		s.info_ret == 0 && s.info.bIsStable ? 1u : 0u);
	return strdup(buf);
}

/* ── AWB diagnostics and control (Steps 2A, 2C) ─────────────────────── */

static char *maruko_query_awb_info(void)
{
	/* i6c ISP functions take (dev, channel, data*) */
	typedef MI_S32 (*fn_query_t)(uint32_t, uint32_t, MarukoAwbQueryInfo *);
	typedef MI_S32 (*fn_cus3a_status_t)(uint32_t, uint32_t, MarukoAwbCus3aInfo *);
	typedef MI_S32 (*fn_stab_t)(uint32_t, uint32_t, MarukoAwbStabilizer *);
	typedef MI_S32 (*fn_attr_t)(uint32_t, uint32_t, MarukoAwbAttr *);
	typedef MI_S32 (*fn_ctmwb_get_t)(uint32_t, uint32_t, MarukoAwbCtMwb *);
	void *handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	char buf[2048];
	int pos = 0;

	if (!handle)
		return NULL;

	pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"ok\":true,\"data\":{");

	{
		fn_query_t fn = (fn_query_t)dlsym(handle, "MI_ISP_AWB_QueryInfo");
		if (fn) {
			MarukoAwbQueryInfo qi;
			MI_S32 ret;
			memset(&qi, 0, sizeof(qi));
			ret = fn(0, 0, &qi);
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
		fn_cus3a_status_t fn = (fn_cus3a_status_t)dlsym(handle,
			"MI_ISP_CUS3A_GetAwbStatus");
		if (fn) {
			MarukoAwbCus3aInfo ci;
			MI_S32 ret;
			memset(&ci, 0, sizeof(ci));
			ci.Size = sizeof(ci);
			ret = fn(0, 0, &ci);
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
		fn_stab_t fn = (fn_stab_t)dlsym(handle, "MI_ISP_AWB_GetStabilizer");
		if (fn) {
			MarukoAwbStabilizer st;
			MI_S32 ret;
			memset(&st, 0, sizeof(st));
			ret = fn(0, 0, &st);
			pos += snprintf(buf + pos, sizeof(buf) - pos,
				"\"stabilizer\":{\"ret\":%d,\"enabled\":%s,"
				"\"glb_gain_thd\":%u,\"count_thd\":%u,\"force_tri_thd\":%u},",
				ret, st.bEnable ? "true" : "false", st.u16GlbGainThd,
				st.u16CountThd, st.u16ForceTriGainThd);
		}
	}

	{
		fn_attr_t fn = (fn_attr_t)dlsym(handle, "MI_ISP_AWB_GetAttr");
		if (fn) {
			MarukoAwbAttr attr;
			MI_S32 ret;
			const char *mode_str = "unknown";
			memset(&attr, 0, sizeof(attr));
			ret = fn(0, 0, &attr);
			if (attr.eOpType == MARUKO_AWB_MODE_AUTO)
				mode_str = "auto";
			else if (attr.eOpType == MARUKO_AWB_MODE_MANUAL)
				mode_str = "manual";
			else if (attr.eOpType == MARUKO_AWB_MODE_CT_MANUAL)
				mode_str = "ct_manual";
			pos += snprintf(buf + pos, sizeof(buf) - pos,
				"\"attr\":{\"ret\":%d,\"state\":%d,\"mode\":\"%s\",\"mode_raw\":%d,"
				"\"mwb_r\":%u,\"mwb_gr\":%u,\"mwb_gb\":%u,\"mwb_b\":%u},",
				ret, attr.eState, mode_str, attr.eOpType, attr.mwb_rgain,
				attr.mwb_grgain, attr.mwb_gbgain, attr.mwb_bgain);
		}
	}

	{
		fn_ctmwb_get_t fn = (fn_ctmwb_get_t)dlsym(handle,
			"MI_ISP_AWB_GetCTMwbAttr");
		if (!fn)
			fn = (fn_ctmwb_get_t)dlsym(handle,
				"MI_ISP_AWB_GetCtMwbAttr");
		if (fn) {
			MarukoAwbCtMwb mwb;
			MI_S32 ret;
			memset(&mwb, 0, sizeof(mwb));
			ret = fn(0, 0, &mwb);
			pos += snprintf(buf + pos, sizeof(buf) - pos,
				"\"ct_mwb\":{\"ret\":%d,\"ct\":%u}", ret, mwb.u32CT);
		}
	}

	pos += snprintf(buf + pos, sizeof(buf) - pos, "}}");
	dlclose(handle);
	return strdup(buf);
}

/* Temporarily disable CUS3A AWB to allow MI_ISP_AWB_SetAttr on i6c.
 * The ISP rejects attr changes while CUS3A AWB is active. */
static void cus3a_awb_set(void *handle, int enable)
{
	typedef int (*fn_t)(MI_U32, MI_U32, void *);
	fn_t fn = (fn_t)dlsym(handle, "MI_ISP_CUS3A_Enable");
	if (fn) {
		MI_BOOL p[3] = {1, enable ? 1 : 0, 1};
		fn(0, 0, p);
	}
}

static int maruko_apply_awb_mode(int mode, uint32_t ct)
{
	/* i6c ISP functions take (dev, channel, data*) */
	typedef MI_S32 (*fn_get_t)(uint32_t, uint32_t, MarukoAwbAttr *);
	typedef MI_S32 (*fn_set_t)(uint32_t, uint32_t, MarukoAwbAttr *);
	typedef MI_S32 (*fn_ctmwb_t)(uint32_t, uint32_t, MarukoAwbCtMwb *);
	void *handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	int ret = 0;

	if (!handle)
		return -1;

	/* Disable CUS3A AWB before changing attributes */
	cus3a_awb_set(handle, 0);

	if (mode == 0) {
		fn_get_t fn_get = (fn_get_t)dlsym(handle, "MI_ISP_AWB_GetAttr");
		fn_set_t fn_set = (fn_set_t)dlsym(handle, "MI_ISP_AWB_SetAttr");

		if (fn_get && fn_set) {
			MarukoAwbAttr attr;
			memset(&attr, 0, sizeof(attr));
			if (fn_get(0, 0, &attr) == 0) {
				MI_S32 awb_ret;
				attr.eState = 0;
				attr.eOpType = MARUKO_AWB_MODE_AUTO;
				awb_ret = fn_set(0, 0, &attr);
				if (awb_ret != 0) {
					fprintf(stderr, "WARNING: MI_ISP_AWB_SetAttr(auto) failed: 0x%08x\n",
						(unsigned)awb_ret);
					ret = -1;
				} else {
					printf("> AWB mode: auto\n");
				}
			} else {
				ret = -1;
			}
		} else {
			ret = -1;
		}
	} else {
		fn_ctmwb_t fn_ctmwb = (fn_ctmwb_t)dlsym(handle,
			"MI_ISP_AWB_SetCTMwbAttr");
		if (!fn_ctmwb)
			fn_ctmwb = (fn_ctmwb_t)dlsym(handle,
				"MI_ISP_AWB_SetCtMwbAttr");
		fn_get_t fn_get = (fn_get_t)dlsym(handle, "MI_ISP_AWB_GetAttr");
		fn_set_t fn_set = (fn_set_t)dlsym(handle, "MI_ISP_AWB_SetAttr");

		if (fn_ctmwb && fn_get && fn_set) {
			MarukoAwbCtMwb mwb = { .u32CT = ct };
			MI_S32 awb_ret = fn_ctmwb(0, 0, &mwb);

			if (awb_ret != 0) {
				fprintf(stderr, "WARNING: MI_ISP_AWB_SetCTMwbAttr(%u) failed: 0x%08x\n",
					ct, (unsigned)awb_ret);
				ret = -1;
			} else {
				MarukoAwbAttr attr;
				memset(&attr, 0, sizeof(attr));
				if (fn_get(0, 0, &attr) == 0) {
					attr.eState = 0;
					attr.eOpType = MARUKO_AWB_MODE_CT_MANUAL;
					awb_ret = fn_set(0, 0, &attr);
					if (awb_ret != 0) {
						fprintf(stderr, "WARNING: MI_ISP_AWB_SetAttr(ct_manual) failed: 0x%08x\n",
							(unsigned)awb_ret);
						ret = -1;
					} else {
						printf("> AWB mode: ct_manual (%uK)\n", ct);
					}
				} else {
					ret = -1;
				}
			}
		} else {
			ret = -1;
		}
	}

	/* Re-enable CUS3A AWB */
	cus3a_awb_set(handle, 1);

	dlclose(handle);
	return ret;
}

static int maruko_apply_gain_max(uint32_t gain)
{
	/* Route through the supervisory thread when it runs: it re-reads
	 * the limit every tick and pushes the effective ceiling (config
	 * value, or the ISP bin default when gain==0).  Writing the raw
	 * value here instead is wrong twice over: gain==0 becomes a
	 * literal 0 limit (image slams to 1x gain = black), and the
	 * supervisory would fight any direct write with its own target. */
	maruko_cus3a_set_gain_max(gain);
	if (maruko_cus3a_running()) {
		printf("> [maruko] Gain max -> %u (0 = bin default; applied "
			"by supervisory within one tick)\n", gain);
		return 0;
	}

	/* Fallback (supervisory disabled, isp.aeFps=0): direct write.
	 * gain==0 means "bin default", which we cannot know here — leave
	 * the current limit untouched. */
	if (gain == 0) {
		printf("> [maruko] Gain max -> 0 ignored (supervisory off; "
			"keeping current limit)\n");
		return 0;
	}
	typedef int (*ae_get_fn)(uint32_t, uint32_t, MarukoIspExposureLimit *);
	typedef int (*ae_set_fn)(uint32_t, uint32_t, MarukoIspExposureLimit *);
	void *h = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h) return -1;

	ae_get_fn fn_get = (ae_get_fn)dlsym(h, "MI_ISP_AE_GetExposureLimit");
	ae_set_fn fn_set = (ae_set_fn)dlsym(h, "MI_ISP_AE_SetExposureLimit");
	if (!fn_get || !fn_set) { dlclose(h); return -1; }

	MarukoIspExposureLimit limit = {0};
	int ret = fn_get(0, 0, &limit);
	if (ret != 0) { dlclose(h); return ret; }

	printf("> [maruko] Gain max: %u -> %u\n",
		limit.maxSensorGain, gain);
	limit.maxSensorGain = gain;
	ret = fn_set(0, 0, &limit);
	dlclose(h);
	return ret;
}

static int maruko_apply_shutter_max(uint32_t us)
{
	maruko_cus3a_set_shutter_max(us);
	if (maruko_cus3a_running()) {
		printf("> [maruko] Shutter max -> %u us (0 = auto; applied "
			"by supervisory within one tick)\n", us);
		return 0;
	}

	if (us == 0) {
		printf("> [maruko] Shutter max -> 0 ignored (supervisory off; "
			"keeping current limit)\n");
		return 0;
	}
	typedef int (*ae_get_fn)(uint32_t, uint32_t, MarukoIspExposureLimit *);
	typedef int (*ae_set_fn)(uint32_t, uint32_t, MarukoIspExposureLimit *);
	void *h = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h) return -1;

	ae_get_fn fn_get = (ae_get_fn)dlsym(h, "MI_ISP_AE_GetExposureLimit");
	ae_set_fn fn_set = (ae_set_fn)dlsym(h, "MI_ISP_AE_SetExposureLimit");
	if (!fn_get || !fn_set) { dlclose(h); return -1; }

	MarukoIspExposureLimit limit = {0};
	int ret = fn_get(0, 0, &limit);
	if (ret != 0) { dlclose(h); return ret; }

	printf("> [maruko] Shutter max: %u -> %u us\n",
		limit.maxShutterUs, us);
	limit.maxShutterUs = us;
	ret = fn_set(0, 0, &limit);
	dlclose(h);
	return ret;
}

static int maruko_apply_gain_min(uint32_t gain)
{
	/* Route through the supervisory thread when it runs (same rationale
	 * as maruko_apply_gain_max): 0 = keep the ISP-bin default floor. */
	maruko_cus3a_set_gain_min(gain);
	if (maruko_cus3a_running()) {
		printf("> [maruko] Gain min -> %u (0 = bin default; applied "
			"by supervisory within one tick)\n", gain);
		return 0;
	}

	if (gain == 0) {
		printf("> [maruko] Gain min -> 0 ignored (supervisory off; "
			"keeping current limit)\n");
		return 0;
	}
	typedef int (*ae_get_fn)(uint32_t, uint32_t, MarukoIspExposureLimit *);
	typedef int (*ae_set_fn)(uint32_t, uint32_t, MarukoIspExposureLimit *);
	void *h = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h) return -1;

	ae_get_fn fn_get = (ae_get_fn)dlsym(h, "MI_ISP_AE_GetExposureLimit");
	ae_set_fn fn_set = (ae_set_fn)dlsym(h, "MI_ISP_AE_SetExposureLimit");
	if (!fn_get || !fn_set) { dlclose(h); return -1; }

	MarukoIspExposureLimit limit = {0};
	int ret = fn_get(0, 0, &limit);
	if (ret != 0) { dlclose(h); return ret; }

	printf("> [maruko] Gain min: %u -> %u\n",
		limit.minSensorGain, gain);
	limit.minSensorGain = gain;
	ret = fn_set(0, 0, &limit);
	dlclose(h);
	return ret;
}

static int maruko_apply_shutter_min(uint32_t us)
{
	maruko_cus3a_set_shutter_min(us);
	if (maruko_cus3a_running()) {
		printf("> [maruko] Shutter min -> %u us (0 = bin default; applied "
			"by supervisory within one tick)\n", us);
		return 0;
	}

	if (us == 0) {
		printf("> [maruko] Shutter min -> 0 ignored (supervisory off; "
			"keeping current limit)\n");
		return 0;
	}
	typedef int (*ae_get_fn)(uint32_t, uint32_t, MarukoIspExposureLimit *);
	typedef int (*ae_set_fn)(uint32_t, uint32_t, MarukoIspExposureLimit *);
	void *h = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h) return -1;

	ae_get_fn fn_get = (ae_get_fn)dlsym(h, "MI_ISP_AE_GetExposureLimit");
	ae_set_fn fn_set = (ae_set_fn)dlsym(h, "MI_ISP_AE_SetExposureLimit");
	if (!fn_get || !fn_set) { dlclose(h); return -1; }

	MarukoIspExposureLimit limit = {0};
	int ret = fn_get(0, 0, &limit);
	if (ret != 0) { dlclose(h); return ret; }

	printf("> [maruko] Shutter min: %u -> %u us\n",
		limit.minShutterUs, us);
	limit.minShutterUs = us;
	ret = fn_set(0, 0, &limit);
	dlclose(h);
	return ret;
}

/* ── Audio mute callback (Phase 5) ───────────────────────────────────── */

static int maruko_apply_mute(bool muted)
{
	if (!g_ctx.backend)
		return -1;
	return maruko_audio_apply_mute(&g_ctx.backend->audio, muted ? 1 : 0);
}

/* ── ROI horizontal bands ────────────────────────────────────────────── */

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

static int maruko_apply_roi_qp(int qp)
{
	uint32_t width = g_ctx.frame_width;
	uint32_t height = g_ctx.frame_height;
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
		cret = MI_VENC_SetRoiCfg(g_ctx.venc_chn, &roi);
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

	if (!g_ctx.vcfg || !g_ctx.vcfg->fpv.roi_enabled || qp == 0) {
		/* Name the cause -- see the matching comment in
		 * star6e_controls.c's apply_roi_qp(). */
		printf("> ROI disabled (%s), %s\n",
			!g_ctx.vcfg ? "no config bound" :
			!g_ctx.vcfg->fpv.roi_enabled ? "roiEnabled=false" :
				"roiQp=0, no delta to apply",
			ok ? "all regions cleared" :
				"CLEAR FAILED, see above");
		return ok ? 0 : -1;
	}

	if (qp < -20) qp = -20;
	if (qp > 20) qp = 20;

	steps = g_ctx.vcfg->fpv.roi_steps;
	if (steps < 1) steps = 1;
	if (steps > PIPELINE_ROI_MAX_STEPS) steps = PIPELINE_ROI_MAX_STEPS;

	center_frac = (float)g_ctx.vcfg->fpv.roi_center;
	if (center_frac < 0.1f) center_frac = 0.1f;
	if (center_frac > 0.9f) center_frac = 0.9f;

	for (int i = 0; i < steps; i++) {
		MI_VENC_RoiCfg_t roi = {0};
		MI_S32 ret;

		if (compute_horizontal_roi(width, height, center_frac, qp,
		    steps, i, &roi) != 0)
			continue;

		ret = MI_VENC_SetRoiCfg(g_ctx.venc_chn, &roi);
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

/* ── Output enable/disable (Step 3B) ─────────────────────────────────── */

static int maruko_apply_output_enabled(bool on)
{
	uint32_t restored_fps;

	if (!g_ctx.output_enabled_ptr)
		return -1;

	if (on) {
		/* Star6E parity — see apply_output_enabled() there for why an
		 * unchanged re-POST must not reach the un-coalescible IDR, and
		 * why this guard covers only the enable arm: the disable arm
		 * also idles the encoder, and skipping that leaves it at full
		 * rate with the output off. */
		if (*g_ctx.output_enabled_ptr)
			return 0;
		if (!g_ctx.vcfg || !g_ctx.vcfg->outgoing.server[0]) {
			fprintf(stderr, "> Cannot enable output: no server configured\n");
			return -1;
		}
		*g_ctx.output_enabled_ptr = 1;
		restored_fps = (g_ctx.stored_fps_ptr && *g_ctx.stored_fps_ptr) ?
			*g_ctx.stored_fps_ptr :
			(g_ctx.vcfg ? g_ctx.vcfg->video0.fps : 30);
		maruko_apply_fps(restored_fps);
		/* Exactly one IDR for the enable: counted, but NOT coalesced —
		 * the consumer has seen no parameter set.  apply_fps() no
		 * longer issues its own, so this is not a back-to-back pair. */
		(void)maruko_request_idr_bootstrap();
		printf("> Output enabled, FPS restored to %u\n", restored_fps);
	} else {
		*g_ctx.output_enabled_ptr = 0;
		if (g_ctx.stored_fps_ptr)
			*g_ctx.stored_fps_ptr = g_ctx.vcfg ?
				g_ctx.vcfg->video0.fps : 30;
		maruko_apply_fps(MARUKO_CONTROLS_IDLE_FPS);
		printf("> Output disabled, FPS reduced to %u (idle)\n",
			MARUKO_CONTROLS_IDLE_FPS);
	}

	return 0;
}

/* ── Live server change (Step 3C) ────────────────────────────────────── */

static MarukoOutput *g_maruko_output_ptr;
static int maruko_apply_server(const char *uri)
{
	if (!g_maruko_output_ptr)
		return -1;
	/* Star6E parity — an unchanged destination is not a bootstrap event.
	 * Compare what we actually applied, never vcfg: the config is
	 * committed before this callback runs, so a vcfg comparison matches
	 * even on a real change and would skip the repoint. */
	if (uri && g_ctx.applied_server[0] &&
	    strcmp(g_ctx.applied_server, uri) == 0)
		return 0;
	if (maruko_output_apply_server(g_maruko_output_ptr, uri) != 0)
		return -1;
	snprintf(g_ctx.applied_server,
		sizeof(g_ctx.applied_server), "%s", uri);

	/* A new destination is a receiver that has never seen a parameter set,
	 * so this is counted but never coalesced. */
	(void)maruko_request_idr_bootstrap();
	printf("> Destination changed to %s\n", uri);
	return 0;
}

static int maruko_apply_max_payload_size(uint16_t size)
{
	if (!g_ctx.backend_cfg)
		return -1;

	/* Validation enforces [VENC_OUTPUT_PAYLOAD_MIN_BYTES,
	 * VENC_OUTPUT_PAYLOAD_CEILING_BYTES] and the SHM ring is sized to
	 * the ceiling at startup, so any value reaching here fits every
	 * transport. Plain uint16_t stores are atomic on ARM; the encoder
	 * thread re-reads cfg->rtp_payload_size / cfg->max_frame_size once
	 * per frame. */
	g_ctx.backend_cfg->rtp_payload_size = size;
	g_ctx.backend_cfg->max_frame_size = size;

	/* stderr (unbuffered) so the live trace lands in the log even when
	 * stdout is buffered or captured by the audio filter. */
	fprintf(stderr, "> max_payload_size set to %u (live)\n", (unsigned)size);
	return 0;
}

static const char *maruko_output_transport_name(const MarukoOutput *o)
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

static char *maruko_query_transport_status(void)
{
	MarukoBackendContext *backend = g_ctx.backend;
	char buf[640];
	const char *transport;
	int pos;
	uint32_t pressure_drops;

	if (!backend)
		return NULL;
	transport = maruko_output_transport_name(&backend->output);

	pressure_drops = __atomic_load_n(&backend->output.pressure_drops,
		__ATOMIC_RELAXED);

	if (backend->output.ring) {
		venc_ring_fill_t fill;
		int in_pressure;
		if (venc_ring_get_fill(backend->output.ring, &fill) != 0)
			return NULL;
		/* See star6e_controls equivalent: HTTP `inPressure` is a
		 * point-in-time snapshot derived from the freshly-queried
		 * fill_pct; the cached hysteresis flag would go stale after
		 * a probe disconnects. */
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
				&backend->output.bad_au_drops, __ATOMIC_RELAXED));
	} else if (backend->output.frame_ring) {
		venc_frame_ring_fill_t fill;
		char gate_json[FRAME_GATE_STATUS_JSON_CAP];
		int in_pressure;
		if (venc_frame_ring_get_fill(backend->output.frame_ring,
		    &fill) != 0)
			return NULL;
		in_pressure = fill.fill_pct >= VENC_PRESSURE_HIGH_WATER_PCT;
		frame_gate_status_json(&backend->frame_gate, wb_monotonic_us(),
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
			(unsigned)venc_ring_low_water_slots(&backend->output.low_water),
			(unsigned long long)fill.other_drops,
			(unsigned long long)__atomic_load_n(
				&backend->output.bad_au_drops, __ATOMIC_RELAXED),
			gate_json);
	} else if ((backend->output.transport == VENC_OUTPUT_URI_UNIX ||
	            backend->output.transport == VENC_OUTPUT_URI_UDP) &&
	           backend->output.socket_handle >= 0) {
		uint8_t fill_pct = 0;
		int in_pressure;
		if (output_socket_get_fill_pct(backend->output.socket_handle,
		    &backend->output.send_queue, &fill_pct) != 0)
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
			(unsigned)__atomic_load_n(&backend->output.socket_drops,
				__ATOMIC_RELAXED),
			(unsigned)__atomic_load_n(&backend->output.socket_writes,
				__ATOMIC_RELAXED),
			(unsigned long long)__atomic_load_n(
				&backend->output.bad_au_drops, __ATOMIC_RELAXED));
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

/* ── Callback table ──────────────────────────────────────────────────── */

static char *maruko_query_audio_status(void)
{
	MarukoBackendContext *backend = g_ctx.backend;
	if (!backend)
		return NULL;
	return maruko_audio_query_status(&backend->audio);
}

static int maruko_apply_zoom(double pct, double x, double y)
{
	MarukoBackendContext *backend = g_ctx.backend;
	if (!backend)
		return -1;
	/* Don't mirror into ctx->cfg — venc_api owns the canonical config and
	 * the next maruko_config_from_venc() (SIGHUP / reinit) reads from
	 * VencConfig directly.  Avoiding the cfg write also avoids a torn
	 * 8-byte double race with the runner thread on 32-bit ARM. */
	return maruko_pipeline_apply_zoom(backend, pct, x, y);
}

static int maruko_apply_pause_stab(bool paused)
{
	return maruko_pipeline_set_pause_stab(paused);
}

static int maruko_apply_isp_bin(const char *path)
{
	if (!g_ctx.backend)
		return -1;

	return maruko_pipeline_load_isp_bin_live(g_ctx.backend, path);
}

/*
 * Detector graph changes are serialized on the pipeline thread, which is the
 * only snapshot consumer.  The API thread posts a full config copy and waits
 * boundedly; model loading remains best-effort and never rolls video config
 * back after the old graph has been released.
 */
static struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int pending;
	int done;
	VencConfig cfg;
} g_detect_reload = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.cond = PTHREAD_COND_INITIALIZER,
};

static int maruko_apply_detect_reload(void)
{
	struct timespec deadline;

	if (!g_ctx.backend || !g_ctx.vcfg)
		return 0;
	pthread_mutex_lock(&g_detect_reload.lock);
	g_detect_reload.cfg = *g_ctx.vcfg;
	g_detect_reload.pending = 1;
	g_detect_reload.done = 0;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 5;
	while (!g_detect_reload.done) {
		if (pthread_cond_timedwait(&g_detect_reload.cond,
		    &g_detect_reload.lock, &deadline) == ETIMEDOUT)
			break;
	}
	pthread_mutex_unlock(&g_detect_reload.lock);
	return 0;
}

void maruko_controls_service_detect_reload(void)
{
	VencConfig cfg;
	int pending;

	pthread_mutex_lock(&g_detect_reload.lock);
	pending = g_detect_reload.pending;
	g_detect_reload.pending = 0;
	if (pending)
		cfg = g_detect_reload.cfg;
	pthread_mutex_unlock(&g_detect_reload.lock);
	if (!pending)
		return;

	if (g_ctx.backend) {
		if (!cfg.detect.enabled) {
			maruko_ipu_yolo_stop(g_ctx.backend);
		} else if (strcmp(cfg.video0.framing, "off") != 0 ||
		    cfg.video0.zoom_pct > 0.0) {
			fprintf(stderr, "[maruko-ipu] live reload refused while "
				"framing=%s zoom=%.2f\n", cfg.video0.framing,
				cfg.video0.zoom_pct);
		} else if (!g_ctx.backend->detect) {
			(void)maruko_ipu_yolo_start(g_ctx.backend, &cfg);
		} else {
			(void)maruko_ipu_yolo_reload(g_ctx.backend, &cfg);
		}
	}

	pthread_mutex_lock(&g_detect_reload.lock);
	g_detect_reload.done = 1;
	pthread_cond_broadcast(&g_detect_reload.cond);
	pthread_mutex_unlock(&g_detect_reload.lock);
}

static const VencApplyCallbacks g_maruko_apply_cb = {
	.apply_bitrate = maruko_apply_bitrate,
	.apply_fps = maruko_apply_fps_live,
	.apply_gop = maruko_apply_gop,
	.apply_qp_delta = maruko_apply_qp_delta,
	.apply_qp_bounds = maruko_apply_qp_bounds,
	.apply_roi_qp = maruko_apply_roi_qp,
	.apply_verbose = maruko_apply_verbose,
	.apply_output_enabled = maruko_apply_output_enabled,
	.apply_server = maruko_apply_server,
	.apply_gain_max = maruko_apply_gain_max,
	.apply_shutter_max = maruko_apply_shutter_max,
	.apply_gain_min = maruko_apply_gain_min,
	.apply_shutter_min = maruko_apply_shutter_min,
	.apply_mute = maruko_apply_mute,
	.request_idr = maruko_request_idr,
	.query_live_fps = maruko_query_live_fps,
	.query_ae_info = maruko_query_ae_info,
	.query_awb_info = maruko_query_awb_info,
	.query_isp_metrics = maruko_query_isp_metrics,
	.apply_awb_mode = maruko_apply_awb_mode,
	.query_iq_info = maruko_iq_query,
	.apply_iq_param = maruko_iq_set,
	.apply_max_payload_size = maruko_apply_max_payload_size,
	.query_transport_status = maruko_query_transport_status,
	.query_audio_status = maruko_query_audio_status,
	.apply_zoom = maruko_apply_zoom,
	.apply_pause_stab = maruko_apply_pause_stab,
	.apply_isp_bin = maruko_apply_isp_bin,
	.apply_snapshot_quality = venc_jpeg_set_quality,
	.apply_detect_reload = maruko_apply_detect_reload,
};

void maruko_controls_bind(MarukoBackendContext *backend, VencConfig *vcfg)
{
	memset(&g_ctx, 0, sizeof(g_ctx));
	if (!backend)
		return;

	g_ctx.venc_dev = backend->venc_device;
	g_ctx.venc_chn = backend->venc_channel;
	g_ctx.verbose_ptr = &backend->cfg.verbose;
	g_ctx.pad_id = backend->sensor.pad_id;
	g_ctx.sensor_fps = backend->sensor.fps;
	g_ctx.frame_width = backend->cfg.image_width;
	g_ctx.frame_height = backend->cfg.image_height;
	g_ctx.vcfg = vcfg;
	/* backend_cfg points into MarukoBackendContext, which lives in the
	 * runner context for the entire process lifetime. Reinit re-runs
	 * maruko_config_from_venc and rebinds, so the pointer remains
	 * valid and the snapshot stays in sync with vcfg. */
	g_ctx.backend_cfg = &backend->cfg;
	g_ctx.backend = backend;
	g_ctx.vpe_port = backend->vpe_port;
	g_ctx.venc_port = backend->venc_port;
	g_ctx.output_enabled_ptr = &backend->output_enabled;
	g_ctx.stored_fps_ptr = &backend->stored_fps;
	g_maruko_output_ptr = &backend->output;
	/* Seed from the create path so the first live set naming the
	 * already-active destination is correctly seen as unchanged -- but
	 * ONLY if the output actually came up; see the Star6E twin for why
	 * seeding over a failed bring-up would remove the retry route. */
	if (g_ctx.vcfg &&
	    (backend->output.ring || backend->output.frame_ring ||
	     backend->output.socket_handle >= 0))
		snprintf(g_ctx.applied_server,
			sizeof(g_ctx.applied_server), "%s",
			g_ctx.vcfg->outgoing.server);
}

const VencApplyCallbacks *maruko_controls_callbacks(void)
{
	return &g_maruko_apply_cb;
}

const VencConfig *maruko_controls_vcfg(void)
{
	return g_ctx.vcfg;
}

void maruko_controls_ae_osd_status(MarukoAeOsdStatus *out)
{
	MarukoAeDiagSnapshot s;

	memset(out, 0, sizeof(*out));
	ae_diag_collect(&s);

	/* Sensor plane shutter/gain remain valid even if the AE query
	 * fails, so surface those as long as either source answered. */
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
		typedef MI_S32 (*fn_awb_query_t)(uint32_t, uint32_t,
			MarukoAwbQueryInfo *);
		void *handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
		if (handle) {
			fn_awb_query_t fn = (fn_awb_query_t)dlsym(handle,
				"MI_ISP_AWB_QueryInfo");
			if (fn) {
				MarukoAwbQueryInfo qi;
				memset(&qi, 0, sizeof(qi));
				if (fn(0, 0, &qi) == 0) {
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
}
