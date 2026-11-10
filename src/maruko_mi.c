/*
 * Maruko MI SDK dynamic loader
 *
 * Loads SigmaStar MI vendor libraries at runtime via dlopen/dlsym,
 * eliminating the need for direct linking and the uClibc compatibility
 * shim. Follows the same pattern used by majestic.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "maruko_mi.h"

/* Global instances */
maruko_sys_impl  g_mi_sys;
maruko_vif_impl  g_mi_vif;
maruko_snr_impl  g_mi_snr;
maruko_venc_impl g_mi_venc;
maruko_isp_impl  g_mi_isp;
maruko_scl_impl  g_mi_scl;
maruko_ai_impl   g_mi_ai;

/* Dependency handles — kept open for the process lifetime */
static void *h_cam_os;
static void *h_mi_common;
static void *h_ispalgo;
static void *h_cus3a;
static void *h_mi_rgn;

void *maruko_load_symbol(void *handle, const char *lib_name,
	const char *sym_name)
{
	void *sym = dlsym(handle, sym_name);
	if (!sym) {
		const char *err = dlerror();
		fprintf(stderr, "ERROR: [maruko] dlsym(%s:%s) failed: %s\n",
			lib_name, sym_name, err ? err : "unknown");
	}
	return sym;
}

#define LOAD_SYM(impl, lib_str, field, cast, sym_str) \
	(impl)->field = (cast)maruko_load_symbol((impl)->handle, lib_str, sym_str)

/* Optional symbol — quiet on miss (used for SDK features that may not
 * exist on older firmware).  The caller must NULL-check before invoking. */
#define LOAD_SYM_OPTIONAL(impl, lib_str, field, cast, sym_str) \
	do { (void)dlerror(); \
	     (impl)->field = (cast)dlsym((impl)->handle, sym_str); \
	     (void)dlerror(); } while (0)

/* --- SYS ---------------------------------------------------------------- */

static int i6c_sys_load(maruko_sys_impl *sys)
{
	memset(sys, 0, sizeof(*sys));
	sys->handle = dlopen("libmi_sys.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!sys->handle) {
		fprintf(stderr, "ERROR: [maruko] dlopen(libmi_sys.so): %s\n",
			dlerror());
		return -1;
	}

	LOAD_SYM(sys, "libmi_sys.so", fnInit,
		int (*)(uint16_t), "MI_SYS_Init");
	LOAD_SYM(sys, "libmi_sys.so", fnExit,
		int (*)(uint16_t), "MI_SYS_Exit");
	LOAD_SYM(sys, "libmi_sys.so", fnBindChnPort2,
		int (*)(uint16_t, void *, void *, uint32_t, uint32_t, uint32_t, uint32_t),
		"MI_SYS_BindChnPort2");
	LOAD_SYM(sys, "libmi_sys.so", fnUnBindChnPort,
		int (*)(uint16_t, void *, void *), "MI_SYS_UnBindChnPort");
	LOAD_SYM(sys, "libmi_sys.so", fnSetChnOutputPortDepth,
		int (*)(uint16_t, void *, uint32_t, uint32_t),
		"MI_SYS_SetChnOutputPortDepth");
	LOAD_SYM(sys, "libmi_sys.so", fnConfigPrivateMMAPool,
		int (*)(uint16_t, void *), "MI_SYS_ConfigPrivateMMAPool");

	if (!sys->fnInit || !sys->fnExit || !sys->fnBindChnPort2 ||
	    !sys->fnUnBindChnPort || !sys->fnSetChnOutputPortDepth ||
	    !sys->fnConfigPrivateMMAPool) {
		dlclose(sys->handle);
		memset(sys, 0, sizeof(*sys));
		return -1;
	}
	return 0;
}

static void i6c_sys_unload(maruko_sys_impl *sys)
{
	if (sys->handle)
		dlclose(sys->handle);
	memset(sys, 0, sizeof(*sys));
}

/* --- VIF ---------------------------------------------------------------- */

static int i6c_vif_load(maruko_vif_impl *vif)
{
	memset(vif, 0, sizeof(*vif));
	vif->handle = dlopen("libmi_vif.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!vif->handle) {
		fprintf(stderr, "ERROR: [maruko] dlopen(libmi_vif.so): %s\n",
			dlerror());
		return -1;
	}

	LOAD_SYM(vif, "libmi_vif.so", fnCreateDevGroup,
		int (*)(uint32_t, void *), "MI_VIF_CreateDevGroup");
	LOAD_SYM(vif, "libmi_vif.so", fnDestroyDevGroup,
		int (*)(uint32_t), "MI_VIF_DestroyDevGroup");
	LOAD_SYM(vif, "libmi_vif.so", fnSetDevAttr,
		int (*)(uint32_t, void *), "MI_VIF_SetDevAttr");
	LOAD_SYM(vif, "libmi_vif.so", fnEnableDev,
		int (*)(uint32_t), "MI_VIF_EnableDev");
	LOAD_SYM(vif, "libmi_vif.so", fnDisableDev,
		int (*)(uint32_t), "MI_VIF_DisableDev");
	LOAD_SYM(vif, "libmi_vif.so", fnSetOutputPortAttr,
		int (*)(uint32_t, uint32_t, void *), "MI_VIF_SetOutputPortAttr");
	LOAD_SYM(vif, "libmi_vif.so", fnEnableOutputPort,
		int (*)(uint32_t, uint32_t), "MI_VIF_EnableOutputPort");
	LOAD_SYM(vif, "libmi_vif.so", fnDisableOutputPort,
		int (*)(uint32_t, uint32_t), "MI_VIF_DisableOutputPort");

	if (!vif->fnCreateDevGroup || !vif->fnDestroyDevGroup ||
	    !vif->fnSetDevAttr || !vif->fnEnableDev || !vif->fnDisableDev ||
	    !vif->fnSetOutputPortAttr || !vif->fnEnableOutputPort ||
	    !vif->fnDisableOutputPort) {
		dlclose(vif->handle);
		memset(vif, 0, sizeof(*vif));
		return -1;
	}
	return 0;
}

static void i6c_vif_unload(maruko_vif_impl *vif)
{
	if (vif->handle)
		dlclose(vif->handle);
	memset(vif, 0, sizeof(*vif));
}

/* --- SNR (sensor) ------------------------------------------------------- */

static int i6c_snr_load(maruko_snr_impl *snr)
{
	memset(snr, 0, sizeof(*snr));
	snr->handle = dlopen("libmi_sensor.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!snr->handle) {
		fprintf(stderr, "ERROR: [maruko] dlopen(libmi_sensor.so): %s\n",
			dlerror());
		return -1;
	}

	LOAD_SYM(snr, "libmi_sensor.so", fnInitDev,
		int (*)(void *), "MI_SNR_InitDev");
	LOAD_SYM(snr, "libmi_sensor.so", fnDeInitDev,
		int (*)(void), "MI_SNR_DeInitDev");
	LOAD_SYM(snr, "libmi_sensor.so", fnSetPlaneMode,
		int (*)(int, int), "MI_SNR_SetPlaneMode");
	LOAD_SYM(snr, "libmi_sensor.so", fnGetPlaneMode,
		int (*)(int, int *), "MI_SNR_GetPlaneMode");
	LOAD_SYM(snr, "libmi_sensor.so", fnSetRes,
		int (*)(int, uint32_t), "MI_SNR_SetRes");
	LOAD_SYM(snr, "libmi_sensor.so", fnGetCurRes,
		int (*)(int, uint8_t *, void *), "MI_SNR_GetCurRes");
	LOAD_SYM(snr, "libmi_sensor.so", fnSetFps,
		int (*)(int, uint32_t), "MI_SNR_SetFps");
	LOAD_SYM(snr, "libmi_sensor.so", fnGetFps,
		int (*)(int, uint32_t *), "MI_SNR_GetFps");
	LOAD_SYM(snr, "libmi_sensor.so", fnSetOrien,
		int (*)(int, uint8_t, uint8_t), "MI_SNR_SetOrien");
	LOAD_SYM(snr, "libmi_sensor.so", fnCustFunction,
		int (*)(int, uint32_t, uint32_t, void *, int),
		"MI_SNR_CustFunction");
	LOAD_SYM(snr, "libmi_sensor.so", fnQueryResCount,
		int (*)(int, uint32_t *), "MI_SNR_QueryResCount");
	LOAD_SYM(snr, "libmi_sensor.so", fnGetRes,
		int (*)(int, uint32_t, void *), "MI_SNR_GetRes");
	LOAD_SYM(snr, "libmi_sensor.so", fnGetPadInfo,
		int (*)(int, void *), "MI_SNR_GetPadInfo");
	LOAD_SYM(snr, "libmi_sensor.so", fnGetPlaneInfo,
		int (*)(int, uint32_t, void *), "MI_SNR_GetPlaneInfo");
	LOAD_SYM(snr, "libmi_sensor.so", fnEnable,
		int (*)(int), "MI_SNR_Enable");
	LOAD_SYM(snr, "libmi_sensor.so", fnDisable,
		int (*)(int), "MI_SNR_Disable");

	if (!snr->fnInitDev || !snr->fnDeInitDev || !snr->fnSetPlaneMode ||
	    !snr->fnGetPlaneMode || !snr->fnSetRes || !snr->fnGetCurRes ||
	    !snr->fnSetFps || !snr->fnGetFps || !snr->fnSetOrien ||
	    !snr->fnCustFunction || !snr->fnQueryResCount ||
	    !snr->fnGetRes || !snr->fnGetPadInfo || !snr->fnGetPlaneInfo ||
	    !snr->fnEnable || !snr->fnDisable) {
		dlclose(snr->handle);
		memset(snr, 0, sizeof(*snr));
		return -1;
	}
	return 0;
}

static void i6c_snr_unload(maruko_snr_impl *snr)
{
	if (snr->handle)
		dlclose(snr->handle);
	memset(snr, 0, sizeof(*snr));
}

/* --- VENC --------------------------------------------------------------- */

static int i6c_venc_load(maruko_venc_impl *venc)
{
	memset(venc, 0, sizeof(*venc));
	venc->handle = dlopen("libmi_venc.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!venc->handle) {
		fprintf(stderr, "ERROR: [maruko] dlopen(libmi_venc.so): %s\n",
			dlerror());
		return -1;
	}

	LOAD_SYM(venc, "libmi_venc.so", fnCreateDev,
		int (*)(int, void *), "MI_VENC_CreateDev");
	LOAD_SYM(venc, "libmi_venc.so", fnDestroyDev,
		int (*)(int), "MI_VENC_DestroyDev");
	LOAD_SYM(venc, "libmi_venc.so", fnCreateChn,
		int (*)(int, int, void *), "MI_VENC_CreateChn");
	LOAD_SYM(venc, "libmi_venc.so", fnDestroyChn,
		int (*)(int, int), "MI_VENC_DestroyChn");
	LOAD_SYM(venc, "libmi_venc.so", fnStartRecvPic,
		int (*)(int, int), "MI_VENC_StartRecvPic");
	LOAD_SYM(venc, "libmi_venc.so", fnStopRecvPic,
		int (*)(int, int), "MI_VENC_StopRecvPic");
	LOAD_SYM(venc, "libmi_venc.so", fnGetStream,
		int (*)(int, int, void *, int), "MI_VENC_GetStream");
	LOAD_SYM(venc, "libmi_venc.so", fnReleaseStream,
		int (*)(int, int, void *), "MI_VENC_ReleaseStream");
	LOAD_SYM(venc, "libmi_venc.so", fnQuery,
		int (*)(int, int, void *), "MI_VENC_Query");
	LOAD_SYM(venc, "libmi_venc.so", fnGetFd,
		int (*)(int, int), "MI_VENC_GetFd");
	LOAD_SYM(venc, "libmi_venc.so", fnCloseFd,
		int (*)(int, int), "MI_VENC_CloseFd");
	LOAD_SYM(venc, "libmi_venc.so", fnGetChnAttr,
		int (*)(int, int, void *), "MI_VENC_GetChnAttr");
	LOAD_SYM(venc, "libmi_venc.so", fnSetChnAttr,
		int (*)(int, int, void *), "MI_VENC_SetChnAttr");
	LOAD_SYM(venc, "libmi_venc.so", fnRequestIdr,
		int (*)(int, int, int), "MI_VENC_RequestIdr");
	/* Optional: absent on older libmi_venc.so, so callers handle -1. */
	LOAD_SYM(venc, "libmi_venc.so", fnSetMaxStreamCnt,
		int (*)(int, int, uint32_t), "MI_VENC_SetMaxStreamCnt");
	LOAD_SYM(venc, "libmi_venc.so", fnSetRoiCfg,
		int (*)(int, int, void *), "MI_VENC_SetRoiCfg");
	LOAD_SYM(venc, "libmi_venc.so", fnGetRoiCfg,
		int (*)(int, int, uint32_t, void *), "MI_VENC_GetRoiCfg");
	LOAD_SYM(venc, "libmi_venc.so", fnGetRcParam,
		int (*)(int, int, void *), "MI_VENC_GetRcParam");
	LOAD_SYM(venc, "libmi_venc.so", fnSetRcParam,
		int (*)(int, int, void *), "MI_VENC_SetRcParam");
	LOAD_SYM(venc, "libmi_venc.so", fnSetInputSourceConfig,
		int (*)(int, int, void *), "MI_VENC_SetInputSourceConfig");
	/* Optional — older Maruko drops may lack these symbols.
	 * Loader does not fail if dlsym misses; callers must NULL-check. */
	LOAD_SYM(venc, "libmi_venc.so", fnSetIntraRefresh,
		int (*)(int, int, void *), "MI_VENC_SetIntraRefresh");
	LOAD_SYM(venc, "libmi_venc.so", fnGetIntraRefresh,
		int (*)(int, int, void *), "MI_VENC_GetIntraRefresh");
	LOAD_SYM(venc, "libmi_venc.so", fnSetRefParam,
		int (*)(int, int, void *), "MI_VENC_SetRefParam");
	LOAD_SYM(venc, "libmi_venc.so", fnGetRefParam,
		int (*)(int, int, void *), "MI_VENC_GetRefParam");
	LOAD_SYM_OPTIONAL(venc, "libmi_venc.so", fnSetH265SliceSplit,
		int (*)(int, int, void *), "MI_VENC_SetH265SliceSplit");
	LOAD_SYM_OPTIONAL(venc, "libmi_venc.so", fnGetH265SliceSplit,
		int (*)(int, int, void *), "MI_VENC_GetH265SliceSplit");

	if (!venc->fnCreateDev || !venc->fnDestroyDev ||
	    !venc->fnCreateChn || !venc->fnDestroyChn ||
	    !venc->fnStartRecvPic || !venc->fnStopRecvPic ||
	    !venc->fnGetStream || !venc->fnReleaseStream ||
	    !venc->fnQuery || !venc->fnGetFd || !venc->fnCloseFd ||
	    !venc->fnGetChnAttr || !venc->fnSetChnAttr ||
	    !venc->fnRequestIdr || !venc->fnSetRoiCfg ||
	    !venc->fnGetRoiCfg || !venc->fnGetRcParam ||
	    !venc->fnSetRcParam || !venc->fnSetInputSourceConfig) {
		dlclose(venc->handle);
		memset(venc, 0, sizeof(*venc));
		return -1;
	}
	return 0;
}

static void i6c_venc_unload(maruko_venc_impl *venc)
{
	if (venc->handle)
		dlclose(venc->handle);
	memset(venc, 0, sizeof(*venc));
}

/* --- ISP ---------------------------------------------------------------- */

static int i6c_isp_load(maruko_isp_impl *isp)
{
	memset(isp, 0, sizeof(*isp));
	isp->handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!isp->handle) {
		fprintf(stderr, "ERROR: [maruko] dlopen(libmi_isp.so): %s\n",
			dlerror());
		return -1;
	}

	LOAD_SYM(isp, "libmi_isp.so", fnCreateDevice,
		int (*)(int, unsigned int *), "MI_ISP_CreateDevice");
	LOAD_SYM(isp, "libmi_isp.so", fnDestroyDevice,
		int (*)(int), "MI_ISP_DestoryDevice"); /* vendor typo: "Destory" */
	LOAD_SYM(isp, "libmi_isp.so", fnCreateChannel,
		int (*)(int, int, void *), "MI_ISP_CreateChannel");
	LOAD_SYM(isp, "libmi_isp.so", fnDestroyChannel,
		int (*)(int, int), "MI_ISP_DestroyChannel");
	LOAD_SYM(isp, "libmi_isp.so", fnSetChannelParam,
		int (*)(int, int, void *), "MI_ISP_SetChnParam");
	LOAD_SYM(isp, "libmi_isp.so", fnStartChannel,
		int (*)(int, int), "MI_ISP_StartChannel");
	LOAD_SYM(isp, "libmi_isp.so", fnStopChannel,
		int (*)(int, int), "MI_ISP_StopChannel");
	LOAD_SYM(isp, "libmi_isp.so", fnDisablePort,
		int (*)(int, int, int), "MI_ISP_DisableOutputPort");
	LOAD_SYM(isp, "libmi_isp.so", fnEnablePort,
		int (*)(int, int, int), "MI_ISP_EnableOutputPort");
	LOAD_SYM(isp, "libmi_isp.so", fnSetPortConfig,
		int (*)(int, int, int, void *), "MI_ISP_SetOutputPortParam");

	/* Zoom APIs are optional — older SDK builds may not export them.
	 * Symbols are looked up but missing entries don't fail the load;
	 * apply_zoom checks for NULL at call time. */
	LOAD_SYM_OPTIONAL(isp, "libmi_isp.so", fnLoadPortZoomTable,
		int (*)(int, int, void *), "MI_ISP_LoadPortZoomTable");
	LOAD_SYM_OPTIONAL(isp, "libmi_isp.so", fnStartPortZoom,
		int (*)(int, int, void *), "MI_ISP_StartPortZoom");
	LOAD_SYM_OPTIONAL(isp, "libmi_isp.so", fnStopPortZoom,
		int (*)(int, int), "MI_ISP_StopPortZoom");
	LOAD_SYM_OPTIONAL(isp, "libmi_isp.so", fnGetPortCurZoomAttr,
		int (*)(int, int, void *), "MI_ISP_GetPortCurZoomAttr");

	if (!isp->fnCreateDevice || !isp->fnDestroyDevice ||
	    !isp->fnCreateChannel || !isp->fnDestroyChannel ||
	    !isp->fnSetChannelParam || !isp->fnStartChannel ||
	    !isp->fnStopChannel || !isp->fnDisablePort ||
	    !isp->fnEnablePort || !isp->fnSetPortConfig) {
		dlclose(isp->handle);
		memset(isp, 0, sizeof(*isp));
		return -1;
	}
	return 0;
}

static void i6c_isp_unload(maruko_isp_impl *isp)
{
	if (isp->handle)
		dlclose(isp->handle);
	memset(isp, 0, sizeof(*isp));
}

/* --- SCL ---------------------------------------------------------------- */

static int i6c_scl_load(maruko_scl_impl *scl)
{
	memset(scl, 0, sizeof(*scl));
	scl->handle = dlopen("libmi_scl.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!scl->handle) {
		fprintf(stderr, "ERROR: [maruko] dlopen(libmi_scl.so): %s\n",
			dlerror());
		return -1;
	}

	LOAD_SYM(scl, "libmi_scl.so", fnCreateDevice,
		int (*)(int, unsigned int *), "MI_SCL_CreateDevice");
	LOAD_SYM(scl, "libmi_scl.so", fnDestroyDevice,
		int (*)(int), "MI_SCL_DestroyDevice");
	LOAD_SYM(scl, "libmi_scl.so", fnAdjustChannelRotation,
		int (*)(int, int, int *), "MI_SCL_SetChnParam");
	LOAD_SYM(scl, "libmi_scl.so", fnCreateChannel,
		int (*)(int, int, unsigned int *), "MI_SCL_CreateChannel");
	LOAD_SYM(scl, "libmi_scl.so", fnDestroyChannel,
		int (*)(int, int), "MI_SCL_DestroyChannel");
	LOAD_SYM(scl, "libmi_scl.so", fnStartChannel,
		int (*)(int, int), "MI_SCL_StartChannel");
	LOAD_SYM(scl, "libmi_scl.so", fnStopChannel,
		int (*)(int, int), "MI_SCL_StopChannel");
	LOAD_SYM(scl, "libmi_scl.so", fnDisablePort,
		int (*)(int, int, int), "MI_SCL_DisableOutputPort");
	LOAD_SYM(scl, "libmi_scl.so", fnEnablePort,
		int (*)(int, int, int), "MI_SCL_EnableOutputPort");
	LOAD_SYM(scl, "libmi_scl.so", fnSetPortConfig,
		int (*)(int, int, int, void *), "MI_SCL_SetOutputPortParam");

	if (!scl->fnCreateDevice || !scl->fnDestroyDevice ||
	    !scl->fnAdjustChannelRotation || !scl->fnCreateChannel ||
	    !scl->fnDestroyChannel || !scl->fnStartChannel ||
	    !scl->fnStopChannel || !scl->fnDisablePort ||
	    !scl->fnEnablePort || !scl->fnSetPortConfig) {
		dlclose(scl->handle);
		memset(scl, 0, sizeof(*scl));
		return -1;
	}
	return 0;
}

static void i6c_scl_unload(maruko_scl_impl *scl)
{
	if (scl->handle)
		dlclose(scl->handle);
	memset(scl, 0, sizeof(*scl));
}

/* --- AI (audio input, optional) ----------------------------------------- */

/* AI loader is non-fatal: when libmi_ai.so is absent (stock OpenIPC firmware
 * without the audio bundle) the audio backend stays inert and the rest of
 * venc runs unchanged.  Audio code paths must check g_mi_ai.handle before
 * dispatching. */
static int i6c_ai_load(maruko_ai_impl *ai)
{
	memset(ai, 0, sizeof(*ai));
	/* RTLD_LAZY mirrors the pattern used for libmi_sys et al — libmi_ai.so
	 * has unresolved __assert / fopen / getenv references that only matter
	 * if the library actually fails internally. */
	ai->handle = dlopen("libmi_ai.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!ai->handle) {
		fprintf(stderr,
			"WARNING: [maruko] dlopen(libmi_ai.so): %s — audio capture disabled\n",
			dlerror());
		return 0;
	}

	LOAD_SYM(ai, "libmi_ai.so", fnInitDev,
		int (*)(void *), "MI_AI_InitDev");
	LOAD_SYM(ai, "libmi_ai.so", fnDeInitDev,
		int (*)(void), "MI_AI_DeInitDev");
	LOAD_SYM(ai, "libmi_ai.so", fnOpen,
		int (*)(int, const void *), "MI_AI_Open");
	LOAD_SYM(ai, "libmi_ai.so", fnClose,
		int (*)(int), "MI_AI_Close");
	LOAD_SYM(ai, "libmi_ai.so", fnAttachIf,
		int (*)(int, const int *, uint8_t), "MI_AI_AttachIf");
	LOAD_SYM(ai, "libmi_ai.so", fnEnableChnGroup,
		int (*)(int, uint8_t), "MI_AI_EnableChnGroup");
	LOAD_SYM(ai, "libmi_ai.so", fnDisableChnGroup,
		int (*)(int, uint8_t), "MI_AI_DisableChnGroup");
	LOAD_SYM(ai, "libmi_ai.so", fnRead,
		int (*)(int, uint8_t, void *, void *, int), "MI_AI_Read");
	LOAD_SYM(ai, "libmi_ai.so", fnReleaseData,
		int (*)(int, uint8_t, void *, void *), "MI_AI_ReleaseData");
	LOAD_SYM(ai, "libmi_ai.so", fnSetMute,
		int (*)(int, uint8_t, const int *, uint8_t), "MI_AI_SetMute");
	LOAD_SYM(ai, "libmi_ai.so", fnSetGain,
		int (*)(int, uint8_t, const int8_t *, uint8_t), "MI_AI_SetGain");
	LOAD_SYM(ai, "libmi_ai.so", fnSetIfGain,
		int (*)(int, int8_t, int8_t), "MI_AI_SetIfGain");

	if (!ai->fnOpen || !ai->fnClose || !ai->fnAttachIf ||
	    !ai->fnEnableChnGroup || !ai->fnDisableChnGroup ||
	    !ai->fnRead || !ai->fnReleaseData || !ai->fnSetMute) {
		fprintf(stderr,
			"WARNING: [maruko] libmi_ai.so missing required symbols — "
			"audio capture disabled\n");
		dlclose(ai->handle);
		memset(ai, 0, sizeof(*ai));
		return 0;
	}
	return 0;
}

static void i6c_ai_unload(maruko_ai_impl *ai)
{
	if (ai->handle)
		dlclose(ai->handle);
	memset(ai, 0, sizeof(*ai));
}

/* --- Init / Deinit ------------------------------------------------------ */

int maruko_mi_init(void)
{
	/* Pre-load transitive dependencies with RTLD_GLOBAL so MI libs
	 * can resolve their own dependencies at dlopen time. */
	h_cam_os = dlopen("libcam_os_wrapper.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h_cam_os)
		fprintf(stderr, "WARNING: [maruko] dlopen(libcam_os_wrapper.so): %s\n",
			dlerror());

	h_mi_common = dlopen("libmi_common.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h_mi_common)
		fprintf(stderr, "WARNING: [maruko] dlopen(libmi_common.so): %s\n",
			dlerror());

	h_ispalgo = dlopen("libispalgo.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h_ispalgo)
		fprintf(stderr, "WARNING: [maruko] dlopen(libispalgo.so): %s\n",
			dlerror());

	h_cus3a = dlopen("libcus3a.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h_cus3a)
		fprintf(stderr, "WARNING: [maruko] dlopen(libcus3a.so): %s\n",
			dlerror());

	/* libmi_rgn.so is opened here (RTLD_GLOBAL) so debug_osd.c's later
	 * dlopen sees the dependency chain already resolved.  Preload is
	 * non-fatal: if libmi_rgn is missing, debug_osd_create() will fail
	 * cleanly and the rest of the pipeline still runs. */
	h_mi_rgn = dlopen("libmi_rgn.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h_mi_rgn)
		fprintf(stderr, "WARNING: [maruko] dlopen(libmi_rgn.so): %s\n",
			dlerror());

	/* Load modules in dependency order. On failure, maruko_mi_deinit()
	 * safely unloads any already-loaded modules (handles NULL gracefully). */
	if (i6c_sys_load(&g_mi_sys) != 0) {
		fprintf(stderr, "ERROR: [maruko] failed to load MI SYS\n");
		goto fail;
	}
	if (i6c_vif_load(&g_mi_vif) != 0) {
		fprintf(stderr, "ERROR: [maruko] failed to load MI VIF\n");
		goto fail;
	}
	if (i6c_snr_load(&g_mi_snr) != 0) {
		fprintf(stderr, "ERROR: [maruko] failed to load MI SNR\n");
		goto fail;
	}
	if (i6c_venc_load(&g_mi_venc) != 0) {
		fprintf(stderr, "ERROR: [maruko] failed to load MI VENC\n");
		goto fail;
	}
	if (i6c_isp_load(&g_mi_isp) != 0) {
		fprintf(stderr, "ERROR: [maruko] failed to load MI ISP\n");
		goto fail;
	}
	if (i6c_scl_load(&g_mi_scl) != 0) {
		fprintf(stderr, "ERROR: [maruko] failed to load MI SCL\n");
		goto fail;
	}

	/* Optional: missing libmi_ai.so disables audio capture but keeps
	 * the rest of the pipeline alive. */
	(void)i6c_ai_load(&g_mi_ai);

	return 0;

fail:
	maruko_mi_deinit();
	return -1;
}

void maruko_mi_deinit(void)
{
	i6c_ai_unload(&g_mi_ai);
	i6c_scl_unload(&g_mi_scl);
	i6c_isp_unload(&g_mi_isp);
	i6c_venc_unload(&g_mi_venc);
	i6c_snr_unload(&g_mi_snr);
	i6c_vif_unload(&g_mi_vif);
	i6c_sys_unload(&g_mi_sys);

	if (h_mi_rgn)   { dlclose(h_mi_rgn);   h_mi_rgn = NULL; }
	if (h_cus3a)    { dlclose(h_cus3a);    h_cus3a = NULL; }
	if (h_ispalgo)  { dlclose(h_ispalgo);  h_ispalgo = NULL; }
	if (h_mi_common){ dlclose(h_mi_common);h_mi_common = NULL; }
	if (h_cam_os)   { dlclose(h_cam_os);   h_cam_os = NULL; }
}
