#ifndef STAR6E_MI_H
#define STAR6E_MI_H

/*
 * Star6E MI SDK dynamic loader
 *
 * Loads SigmaStar MI vendor libraries via dlopen at runtime, matching
 * the Maruko dlopen pattern (maruko_mi.h). Star6E functions have no
 * soc_id parameter, so the _impl structs differ from Maruko's.
 *
 * All config/attr params use void* — callers must pass the correct
 * MI SDK struct. Init/deinit must be called from a single thread.
 */

#include <stdint.h>

typedef struct {
	void *handle;
	int (*fnInit)(void);
	int (*fnExit)(void);
	int (*fnBindChnPort)(const void *src, const void *dst,
		uint32_t src_fps, uint32_t dst_fps);
	int (*fnUnBindChnPort)(const void *src, const void *dst);
	int (*fnBindChnPort2)(const void *src, const void *dst,
		uint32_t src_fps, uint32_t dst_fps,
		uint32_t link_type, uint32_t link_param);
	int (*fnSetChnOutputPortDepth)(const void *port,
		uint32_t user_depth, uint32_t buf_depth);
} star6e_sys_impl;

typedef struct {
	void *handle;
	int (*fnSetDevAttr)(int dev, void *attr);
	int (*fnEnableDev)(int dev);
	int (*fnDisableDev)(int dev);
	int (*fnSetChnPortAttr)(int chn, int port, void *attr);
	int (*fnEnableChnPort)(int chn, int port);
	int (*fnDisableChnPort)(int chn, int port);
} star6e_vif_impl;

typedef struct {
	void *handle;
	int (*fnCreateChannel)(int chn, void *attr);
	int (*fnGetChannelAttr)(int chn, void *attr);
	int (*fnSetChannelAttr)(int chn, void *attr);
	int (*fnDestroyChannel)(int chn);
	int (*fnStartChannel)(int chn);
	int (*fnStopChannel)(int chn);
	int (*fnSetChannelParam)(int chn, void *param);
	int (*fnSetPortMode)(int chn, int port, void *attr);
	int (*fnEnablePort)(int chn, int port);
	int (*fnDisablePort)(int chn, int port);
	int (*fnSetPortCrop)(int chn, int port, void *crop);
} star6e_vpe_impl;

typedef struct {
	void *handle;
	int (*fnInitDev)(void *init);
	int (*fnDeInitDev)(void);
	int (*fnSetPlaneMode)(int pad, int mode);
	int (*fnGetPlaneMode)(int pad, int *mode);
	int (*fnSetRes)(int pad, uint32_t idx);
	int (*fnGetCurRes)(int pad, uint8_t *idx, void *res);
	int (*fnSetFps)(int pad, uint32_t fps);
	int (*fnGetFps)(int pad, uint32_t *fps);
	int (*fnSetOrien)(int pad, uint8_t mirror, uint8_t flip);
	int (*fnCustFunction)(int pad, uint32_t cmd, uint32_t size, void *data,
		int dir);
	int (*fnQueryResCount)(int pad, uint32_t *count);
	int (*fnGetRes)(int pad, uint32_t idx, void *res);
	int (*fnGetPadInfo)(int pad, void *info);
	int (*fnGetPlaneInfo)(int pad, uint32_t plane, void *info);
	int (*fnEnable)(int pad);
	int (*fnDisable)(int pad);
} star6e_snr_impl;

typedef struct {
	void *handle;
	int (*fnCreateChn)(int chn, void *attr);
	int (*fnDestroyChn)(int chn);
	int (*fnStartRecvPic)(int chn);
	int (*fnStopRecvPic)(int chn);
	int (*fnGetStream)(int chn, void *stream, int timeout);
	int (*fnReleaseStream)(int chn, void *stream);
	int (*fnQuery)(int chn, void *stat);
	int (*fnGetFd)(int chn);
	int (*fnCloseFd)(int chn);
	int (*fnGetChnAttr)(int chn, void *attr);
	int (*fnSetChnAttr)(int chn, void *attr);
	int (*fnRequestIdr)(int chn, int instant);
	int (*fnSetMaxStreamCnt)(int chn, uint32_t cnt);
	int (*fnSetRoiCfg)(int chn, void *cfg);
	int (*fnGetRoiCfg)(int chn, uint32_t idx, void *cfg);
	int (*fnGetRcParam)(int chn, void *param);
	int (*fnSetRcParam)(int chn, void *param);
	int (*fnGetChnDevid)(int chn, uint32_t *dev_id);
	/* Optional — may be NULL on older libmi_venc.so builds. */
	int (*fnSetIntraRefresh)(int chn, void *cfg);
	int (*fnGetIntraRefresh)(int chn, void *cfg);
	int (*fnSetRefParam)(int chn, void *p);
	int (*fnGetRefParam)(int chn, void *p);
	int (*fnSetH265SliceSplit)(int chn, void *p);
	int (*fnGetH265SliceSplit)(int chn, void *p);
} star6e_venc_impl;

/* Global instances — defined in star6e_mi.c.
 * Same names as Maruko (g_mi_*) — only one backend is compiled. */
extern star6e_sys_impl  g_mi_sys;
extern star6e_vif_impl  g_mi_vif;
extern star6e_vpe_impl  g_mi_vpe;
extern star6e_snr_impl  g_mi_snr;
extern star6e_venc_impl g_mi_venc;

int star6e_mi_init(void);
void star6e_mi_deinit(void);

void *star6e_load_symbol(void *handle, const char *lib_name,
	const char *sym_name);

#endif /* STAR6E_MI_H */
