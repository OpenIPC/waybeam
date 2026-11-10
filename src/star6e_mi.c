/*
 * Star6E MI SDK dynamic loader
 *
 * Same dlopen/dlsym pattern as maruko_mi.c but with Star6E-specific
 * function signatures (no soc_id parameter, VPE instead of ISP+SCL).
 */

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "star6e_mi.h"

/* Global instances */
star6e_sys_impl  g_mi_sys;
star6e_vif_impl  g_mi_vif;
star6e_vpe_impl  g_mi_vpe;
star6e_snr_impl  g_mi_snr;
star6e_venc_impl g_mi_venc;

/* Dependency handles */
static void *h_cam_os;
static void *h_ispalgo;
static void *h_cus3a;
static void *h_isp;

void *star6e_load_symbol(void *handle, const char *lib_name,
	const char *sym_name)
{
	void *sym = dlsym(handle, sym_name);
	if (!sym) {
		const char *err = dlerror();
		fprintf(stderr, "ERROR: [star6e] dlsym(%s:%s) failed: %s\n",
			lib_name, sym_name, err ? err : "unknown");
	}
	return sym;
}

#define LOAD_SYM(impl, lib_str, field, cast, sym_str) \
	(impl)->field = (cast)star6e_load_symbol((impl)->handle, lib_str, sym_str)

/* --- SYS ---------------------------------------------------------------- */

static int i6e_sys_load(star6e_sys_impl *sys)
{
	memset(sys, 0, sizeof(*sys));
	sys->handle = dlopen("libmi_sys.so", RTLD_NOW | RTLD_GLOBAL);
	if (!sys->handle) {
		fprintf(stderr, "ERROR: [star6e] dlopen(libmi_sys.so): %s\n",
			dlerror());
		return -1;
	}

	LOAD_SYM(sys, "libmi_sys.so", fnInit,
		int (*)(void), "MI_SYS_Init");
	LOAD_SYM(sys, "libmi_sys.so", fnExit,
		int (*)(void), "MI_SYS_Exit");
	LOAD_SYM(sys, "libmi_sys.so", fnBindChnPort,
		int (*)(const void *, const void *, uint32_t, uint32_t),
		"MI_SYS_BindChnPort");
	LOAD_SYM(sys, "libmi_sys.so", fnUnBindChnPort,
		int (*)(const void *, const void *), "MI_SYS_UnBindChnPort");
	LOAD_SYM(sys, "libmi_sys.so", fnBindChnPort2,
		int (*)(const void *, const void *, uint32_t, uint32_t, uint32_t, uint32_t),
		"MI_SYS_BindChnPort2");
	LOAD_SYM(sys, "libmi_sys.so", fnSetChnOutputPortDepth,
		int (*)(const void *, uint32_t, uint32_t),
		"MI_SYS_SetChnOutputPortDepth");

	if (!sys->fnInit || !sys->fnExit || !sys->fnBindChnPort ||
	    !sys->fnUnBindChnPort || !sys->fnBindChnPort2 ||
	    !sys->fnSetChnOutputPortDepth) {
		dlclose(sys->handle);
		memset(sys, 0, sizeof(*sys));
		return -1;
	}
	return 0;
}

static void i6e_sys_unload(star6e_sys_impl *sys)
{
	if (sys->handle) dlclose(sys->handle);
	memset(sys, 0, sizeof(*sys));
}

/* --- VIF ---------------------------------------------------------------- */

static int i6e_vif_load(star6e_vif_impl *vif)
{
	memset(vif, 0, sizeof(*vif));
	vif->handle = dlopen("libmi_vif.so", RTLD_NOW | RTLD_GLOBAL);
	if (!vif->handle) {
		fprintf(stderr, "ERROR: [star6e] dlopen(libmi_vif.so): %s\n",
			dlerror());
		return -1;
	}

	LOAD_SYM(vif, "libmi_vif.so", fnSetDevAttr,
		int (*)(int, void *), "MI_VIF_SetDevAttr");
	LOAD_SYM(vif, "libmi_vif.so", fnEnableDev,
		int (*)(int), "MI_VIF_EnableDev");
	LOAD_SYM(vif, "libmi_vif.so", fnDisableDev,
		int (*)(int), "MI_VIF_DisableDev");
	LOAD_SYM(vif, "libmi_vif.so", fnSetChnPortAttr,
		int (*)(int, int, void *), "MI_VIF_SetChnPortAttr");
	LOAD_SYM(vif, "libmi_vif.so", fnEnableChnPort,
		int (*)(int, int), "MI_VIF_EnableChnPort");
	LOAD_SYM(vif, "libmi_vif.so", fnDisableChnPort,
		int (*)(int, int), "MI_VIF_DisableChnPort");

	if (!vif->fnSetDevAttr || !vif->fnEnableDev || !vif->fnDisableDev ||
	    !vif->fnSetChnPortAttr || !vif->fnEnableChnPort ||
	    !vif->fnDisableChnPort) {
		dlclose(vif->handle);
		memset(vif, 0, sizeof(*vif));
		return -1;
	}
	return 0;
}

static void i6e_vif_unload(star6e_vif_impl *vif)
{
	if (vif->handle) dlclose(vif->handle);
	memset(vif, 0, sizeof(*vif));
}

/* --- VPE ---------------------------------------------------------------- */

static int i6e_vpe_load(star6e_vpe_impl *vpe)
{
	memset(vpe, 0, sizeof(*vpe));
	vpe->handle = dlopen("libmi_vpe.so", RTLD_NOW | RTLD_GLOBAL);
	if (!vpe->handle) {
		fprintf(stderr, "ERROR: [star6e] dlopen(libmi_vpe.so): %s\n",
			dlerror());
		return -1;
	}

	LOAD_SYM(vpe, "libmi_vpe.so", fnCreateChannel,
		int (*)(int, void *), "MI_VPE_CreateChannel");
	LOAD_SYM(vpe, "libmi_vpe.so", fnGetChannelAttr,
		int (*)(int, void *), "MI_VPE_GetChannelAttr");
	LOAD_SYM(vpe, "libmi_vpe.so", fnSetChannelAttr,
		int (*)(int, void *), "MI_VPE_SetChannelAttr");
	LOAD_SYM(vpe, "libmi_vpe.so", fnDestroyChannel,
		int (*)(int), "MI_VPE_DestroyChannel");
	LOAD_SYM(vpe, "libmi_vpe.so", fnStartChannel,
		int (*)(int), "MI_VPE_StartChannel");
	LOAD_SYM(vpe, "libmi_vpe.so", fnStopChannel,
		int (*)(int), "MI_VPE_StopChannel");
	LOAD_SYM(vpe, "libmi_vpe.so", fnSetChannelParam,
		int (*)(int, void *), "MI_VPE_SetChannelParam");
	LOAD_SYM(vpe, "libmi_vpe.so", fnSetPortMode,
		int (*)(int, int, void *), "MI_VPE_SetPortMode");
	LOAD_SYM(vpe, "libmi_vpe.so", fnEnablePort,
		int (*)(int, int), "MI_VPE_EnablePort");
	LOAD_SYM(vpe, "libmi_vpe.so", fnDisablePort,
		int (*)(int, int), "MI_VPE_DisablePort");
	LOAD_SYM(vpe, "libmi_vpe.so", fnSetPortCrop,
		int (*)(int, int, void *), "MI_VPE_SetPortCrop");

	if (!vpe->fnCreateChannel || !vpe->fnGetChannelAttr ||
	    !vpe->fnSetChannelAttr ||
	    !vpe->fnDestroyChannel || !vpe->fnStartChannel ||
	    !vpe->fnStopChannel || !vpe->fnSetChannelParam ||
	    !vpe->fnSetPortMode || !vpe->fnEnablePort ||
	    !vpe->fnDisablePort || !vpe->fnSetPortCrop) {
		dlclose(vpe->handle);
		memset(vpe, 0, sizeof(*vpe));
		return -1;
	}
	return 0;
}

static void i6e_vpe_unload(star6e_vpe_impl *vpe)
{
	if (vpe->handle) dlclose(vpe->handle);
	memset(vpe, 0, sizeof(*vpe));
}

/* --- SNR (sensor) ------------------------------------------------------- */

static int i6e_snr_load(star6e_snr_impl *snr)
{
	memset(snr, 0, sizeof(*snr));
	snr->handle = dlopen("libmi_sensor.so", RTLD_NOW | RTLD_GLOBAL);
	if (!snr->handle) {
		fprintf(stderr, "ERROR: [star6e] dlopen(libmi_sensor.so): %s\n",
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

static void i6e_snr_unload(star6e_snr_impl *snr)
{
	if (snr->handle) dlclose(snr->handle);
	memset(snr, 0, sizeof(*snr));
}

/* --- VENC --------------------------------------------------------------- */

static int i6e_venc_load(star6e_venc_impl *venc)
{
	memset(venc, 0, sizeof(*venc));
	venc->handle = dlopen("libmi_venc.so", RTLD_NOW | RTLD_GLOBAL);
	if (!venc->handle) {
		fprintf(stderr, "ERROR: [star6e] dlopen(libmi_venc.so): %s\n",
			dlerror());
		return -1;
	}

	LOAD_SYM(venc, "libmi_venc.so", fnCreateChn,
		int (*)(int, void *), "MI_VENC_CreateChn");
	LOAD_SYM(venc, "libmi_venc.so", fnDestroyChn,
		int (*)(int), "MI_VENC_DestroyChn");
	LOAD_SYM(venc, "libmi_venc.so", fnStartRecvPic,
		int (*)(int), "MI_VENC_StartRecvPic");
	LOAD_SYM(venc, "libmi_venc.so", fnStopRecvPic,
		int (*)(int), "MI_VENC_StopRecvPic");
	LOAD_SYM(venc, "libmi_venc.so", fnGetStream,
		int (*)(int, void *, int), "MI_VENC_GetStream");
	LOAD_SYM(venc, "libmi_venc.so", fnReleaseStream,
		int (*)(int, void *), "MI_VENC_ReleaseStream");
	LOAD_SYM(venc, "libmi_venc.so", fnQuery,
		int (*)(int, void *), "MI_VENC_Query");
	LOAD_SYM(venc, "libmi_venc.so", fnGetFd,
		int (*)(int), "MI_VENC_GetFd");
	LOAD_SYM(venc, "libmi_venc.so", fnCloseFd,
		int (*)(int), "MI_VENC_CloseFd");
	LOAD_SYM(venc, "libmi_venc.so", fnGetChnAttr,
		int (*)(int, void *), "MI_VENC_GetChnAttr");
	LOAD_SYM(venc, "libmi_venc.so", fnSetChnAttr,
		int (*)(int, void *), "MI_VENC_SetChnAttr");
	LOAD_SYM(venc, "libmi_venc.so", fnRequestIdr,
		int (*)(int, int), "MI_VENC_RequestIdr");
	/* Optional: absent on older libmi_venc.so, so callers handle -1. */
	LOAD_SYM(venc, "libmi_venc.so", fnSetMaxStreamCnt,
		int (*)(int, uint32_t), "MI_VENC_SetMaxStreamCnt");
	LOAD_SYM(venc, "libmi_venc.so", fnSetRoiCfg,
		int (*)(int, void *), "MI_VENC_SetRoiCfg");
	LOAD_SYM(venc, "libmi_venc.so", fnGetRoiCfg,
		int (*)(int, uint32_t, void *), "MI_VENC_GetRoiCfg");
	LOAD_SYM(venc, "libmi_venc.so", fnGetRcParam,
		int (*)(int, void *), "MI_VENC_GetRcParam");
	LOAD_SYM(venc, "libmi_venc.so", fnSetRcParam,
		int (*)(int, void *), "MI_VENC_SetRcParam");
	LOAD_SYM(venc, "libmi_venc.so", fnGetChnDevid,
		int (*)(int, uint32_t *), "MI_VENC_GetChnDevid");
	/* Optional — older libmi_venc.so may not export these. */
	LOAD_SYM(venc, "libmi_venc.so", fnSetIntraRefresh,
		int (*)(int, void *), "MI_VENC_SetIntraRefresh");
	LOAD_SYM(venc, "libmi_venc.so", fnGetIntraRefresh,
		int (*)(int, void *), "MI_VENC_GetIntraRefresh");
	LOAD_SYM(venc, "libmi_venc.so", fnSetRefParam,
		int (*)(int, void *), "MI_VENC_SetRefParam");
	LOAD_SYM(venc, "libmi_venc.so", fnGetRefParam,
		int (*)(int, void *), "MI_VENC_GetRefParam");
	LOAD_SYM(venc, "libmi_venc.so", fnSetH265SliceSplit,
		int (*)(int, void *), "MI_VENC_SetH265SliceSplit");
	LOAD_SYM(venc, "libmi_venc.so", fnGetH265SliceSplit,
		int (*)(int, void *), "MI_VENC_GetH265SliceSplit");

	if (!venc->fnCreateChn || !venc->fnDestroyChn ||
	    !venc->fnStartRecvPic || !venc->fnStopRecvPic ||
	    !venc->fnGetStream || !venc->fnReleaseStream ||
	    !venc->fnQuery || !venc->fnGetFd || !venc->fnCloseFd ||
	    !venc->fnGetChnAttr || !venc->fnSetChnAttr ||
	    !venc->fnRequestIdr || !venc->fnSetRoiCfg ||
	    !venc->fnGetRoiCfg || !venc->fnGetRcParam ||
	    !venc->fnSetRcParam || !venc->fnGetChnDevid) {
		dlclose(venc->handle);
		memset(venc, 0, sizeof(*venc));
		return -1;
	}
	return 0;
}

static void i6e_venc_unload(star6e_venc_impl *venc)
{
	if (venc->handle) dlclose(venc->handle);
	memset(venc, 0, sizeof(*venc));
}

/* --- Init / Deinit ------------------------------------------------------ */

int star6e_mi_init(void)
{
	/* Load order matters: vendor libs have cross-library symbol deps.
	 * With direct linking the dynamic linker resolved these at startup.
	 * With dlopen we must load in dependency order:
	 *   cam_os_wrapper → sys → ispalgo → cus3a → isp → vif/vpe/snr/venc
	 *
	 * Using RTLD_NOW ensures constructors run with all symbols resolved.
	 * Using RTLD_GLOBAL makes symbols available to subsequently loaded libs. */

	h_cam_os = dlopen("libcam_os_wrapper.so", RTLD_NOW | RTLD_GLOBAL);
	if (!h_cam_os)
		fprintf(stderr, "WARNING: [star6e] dlopen(libcam_os_wrapper.so): %s\n",
			dlerror());

	/* SYS must load before cus3a (needs MI_SYS_Mmap) */
	if (i6e_sys_load(&g_mi_sys) != 0) {
		fprintf(stderr, "ERROR: [star6e] failed to load MI SYS\n");
		goto fail;
	}

	/* ISP and CUS3A have circular symbol dependencies — load both with
	 * RTLD_LAZY first, then their cross-references resolve on first use.
	 * ispalgo must load before both. */
	h_ispalgo = dlopen("libispalgo.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h_ispalgo)
		fprintf(stderr, "WARNING: [star6e] dlopen(libispalgo.so): %s\n",
			dlerror());

	h_cus3a = dlopen("libcus3a.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h_cus3a)
		fprintf(stderr, "WARNING: [star6e] dlopen(libcus3a.so): %s\n",
			dlerror());

	/* ISP must load before VPE (VPE references MI_ISP_DisableUserspace3A) */
	h_isp = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h_isp)
		fprintf(stderr, "WARNING: [star6e] dlopen(libmi_isp.so): %s\n",
			dlerror());

	if (i6e_vif_load(&g_mi_vif) != 0) {
		fprintf(stderr, "ERROR: [star6e] failed to load MI VIF\n");
		goto fail;
	}
	if (i6e_vpe_load(&g_mi_vpe) != 0) {
		fprintf(stderr, "ERROR: [star6e] failed to load MI VPE\n");
		goto fail;
	}
	if (i6e_snr_load(&g_mi_snr) != 0) {
		fprintf(stderr, "ERROR: [star6e] failed to load MI SNR\n");
		goto fail;
	}
	if (i6e_venc_load(&g_mi_venc) != 0) {
		fprintf(stderr, "ERROR: [star6e] failed to load MI VENC\n");
		goto fail;
	}

	return 0;

fail:
	star6e_mi_deinit();
	return -1;
}

void star6e_mi_deinit(void)
{
	i6e_venc_unload(&g_mi_venc);
	i6e_snr_unload(&g_mi_snr);
	i6e_vpe_unload(&g_mi_vpe);
	i6e_vif_unload(&g_mi_vif);
	i6e_sys_unload(&g_mi_sys);

	if (h_isp)     { dlclose(h_isp);     h_isp = NULL; }
	if (h_cus3a)   { dlclose(h_cus3a);   h_cus3a = NULL; }
	if (h_ispalgo) { dlclose(h_ispalgo); h_ispalgo = NULL; }
	if (h_cam_os)  { dlclose(h_cam_os);  h_cam_os = NULL; }
}
