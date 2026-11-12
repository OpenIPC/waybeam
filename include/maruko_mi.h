#ifndef MARUKO_MI_H
#define MARUKO_MI_H

/*
 * Maruko MI SDK dynamic loader
 *
 * Loads all SigmaStar MI vendor libraries via dlopen at runtime instead
 * of linking them at build time. This eliminates the uClibc/musl ABI
 * mismatch that previously required deploying libc.so.0, libgcc_s.so.1,
 * and a compatibility shim to the device.
 *
 * Pattern: each MI module has an _impl struct with function pointers
 * loaded via dlsym. Macros in star6e.h dispatch MI_* calls through
 * these structs when PLATFORM_MARUKO is defined.
 */

#include <stdint.h>

/*
 * All config/attr params use void* — callers must pass the correct MI SDK
 * struct. This is an inherent consequence of the dlopen/dlsym pattern.
 *
 * Init/deinit must be called from a single thread before/after any MI calls.
 * After init, function pointers are read-only and safe for concurrent access.
 */

typedef struct {
	void *handle;
	int (*fnCreateDevice)(int device, unsigned int *combo);
	int (*fnDestroyDevice)(int device);
	int (*fnCreateChannel)(int device, int channel, void *config);
	int (*fnDestroyChannel)(int device, int channel);
	int (*fnSetChannelParam)(int device, int channel, void *config);
	int (*fnStartChannel)(int device, int channel);
	int (*fnStopChannel)(int device, int channel);
	int (*fnDisablePort)(int device, int channel, int port);
	int (*fnEnablePort)(int device, int channel, int port);
	int (*fnSetPortConfig)(int device, int channel, int port, void *config);
	/* Optional: digital-zoom port APIs.  NULL on SDK builds without them.
	 * The "Port" in the name is misleading — these act on an ISP channel
	 * (Dev+Chn), with the cropped output flowing through SCL fan-out to
	 * every downstream VENC. */
	int (*fnLoadPortZoomTable)(int device, int channel, void *table);
	int (*fnStartPortZoom)(int device, int channel, void *attr);
	int (*fnStopPortZoom)(int device, int channel);
	int (*fnGetPortCurZoomAttr)(int device, int channel, void *attr);
} maruko_isp_impl;

typedef struct {
	void *handle;
	int (*fnCreateDevice)(int device, unsigned int *binds);
	int (*fnDestroyDevice)(int device);
	int (*fnAdjustChannelRotation)(int device, int channel, int *rotate);
	int (*fnCreateChannel)(int device, int channel, unsigned int *reserved);
	int (*fnDestroyChannel)(int device, int channel);
	int (*fnStartChannel)(int device, int channel);
	int (*fnStopChannel)(int device, int channel);
	int (*fnDisablePort)(int device, int channel, int port);
	int (*fnEnablePort)(int device, int channel, int port);
	int (*fnSetPortConfig)(int device, int channel, int port, void *config);
} maruko_scl_impl;

typedef struct {
	void *handle;
	int (*fnInit)(uint16_t soc_id);
	int (*fnExit)(uint16_t soc_id);
	int (*fnBindChnPort2)(uint16_t soc_id, void *src, void *dst,
		uint32_t src_fps, uint32_t dst_fps, uint32_t link_type,
		uint32_t link_param);
	int (*fnUnBindChnPort)(uint16_t soc_id, void *src, void *dst);
	int (*fnSetChnOutputPortDepth)(uint16_t soc_id, void *port,
		uint32_t user_depth, uint32_t buf_depth);
	int (*fnConfigPrivateMMAPool)(uint16_t soc_id, void *config);
} maruko_sys_impl;

typedef struct {
	void *handle;
	int (*fnCreateDevGroup)(uint32_t group, void *attr);
	int (*fnDestroyDevGroup)(uint32_t group);
	int (*fnSetDevAttr)(uint32_t dev, void *attr);
	int (*fnEnableDev)(uint32_t dev);
	int (*fnDisableDev)(uint32_t dev);
	int (*fnSetOutputPortAttr)(uint32_t dev, uint32_t port, void *attr);
	int (*fnEnableOutputPort)(uint32_t dev, uint32_t port);
	int (*fnDisableOutputPort)(uint32_t dev, uint32_t port);
} maruko_vif_impl;

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
} maruko_snr_impl;

typedef struct {
	void *handle;
	int (*fnCreateDev)(int dev, void *init);
	int (*fnDestroyDev)(int dev);
	int (*fnCreateChn)(int dev, int chn, void *attr);
	int (*fnDestroyChn)(int dev, int chn);
	int (*fnStartRecvPic)(int dev, int chn);
	int (*fnStopRecvPic)(int dev, int chn);
	int (*fnGetStream)(int dev, int chn, void *stream, int timeout);
	int (*fnReleaseStream)(int dev, int chn, void *stream);
	int (*fnQuery)(int dev, int chn, void *stat);
	int (*fnGetFd)(int dev, int chn);
	int (*fnCloseFd)(int dev, int chn);
	int (*fnGetChnAttr)(int dev, int chn, void *attr);
	int (*fnSetChnAttr)(int dev, int chn, void *attr);
	int (*fnRequestIdr)(int dev, int chn, int instant);
	int (*fnSetMaxStreamCnt)(int dev, int chn, uint32_t cnt);
	int (*fnSetRoiCfg)(int dev, int chn, void *cfg);
	int (*fnGetRoiCfg)(int dev, int chn, uint32_t idx, void *cfg);
	int (*fnGetRcParam)(int dev, int chn, void *param);
	int (*fnSetRcParam)(int dev, int chn, void *param);
	int (*fnSetInputSourceConfig)(int dev, int chn, void *cfg);
	/* Optional — Maruko SDK exports these but older drops may not.
	 * Code paths must NULL-check before invoking. */
	int (*fnSetIntraRefresh)(int dev, int chn, void *cfg);
	int (*fnGetIntraRefresh)(int dev, int chn, void *cfg);
	int (*fnSetRefParam)(int dev, int chn, void *p);
	int (*fnGetRefParam)(int dev, int chn, void *p);
	int (*fnSetH265SliceSplit)(int dev, int chn, void *p);
	int (*fnGetH265SliceSplit)(int dev, int chn, void *p);
} maruko_venc_impl;

/* MI_AI (audio input) — i6c surface differs from Star6E:
 * (devId, chnGrpIdx, channel-array) instead of (device, channel).  See
 * `include/maruko_ai_types.h` for the data structures.  The lib is loaded
 * lazily (RTLD_LAZY) — it pulls in __assert / fopen / getenv etc. that the
 * other vendor libs also reference unconditionally, and which only need to
 * resolve if the library actually invokes them. */
typedef struct {
	void *handle;
	int (*fnInitDev)(void *init_param);
	int (*fnDeInitDev)(void);
	int (*fnOpen)(int devId, const void *attr);
	int (*fnClose)(int devId);
	int (*fnAttachIf)(int devId, const int *ifaces, uint8_t count);
	int (*fnEnableChnGroup)(int devId, uint8_t grpIdx);
	int (*fnDisableChnGroup)(int devId, uint8_t grpIdx);
	int (*fnRead)(int devId, uint8_t grpIdx, void *mic_data,
		void *echo_data, int timeout_ms);
	int (*fnReleaseData)(int devId, uint8_t grpIdx, void *mic_data,
		void *echo_data);
	int (*fnSetMute)(int devId, uint8_t grpIdx, const int *mutes,
		uint8_t count);
	int (*fnSetGain)(int devId, uint8_t grpIdx, const int8_t *gains,
		uint8_t count);
	int (*fnSetIfGain)(int iface, int8_t leftDb, int8_t rightDb);
} maruko_ai_impl;

/* Global instances — defined in maruko_mi.c */
extern maruko_sys_impl  g_mi_sys;
extern maruko_vif_impl  g_mi_vif;
extern maruko_snr_impl  g_mi_snr;
extern maruko_venc_impl g_mi_venc;
extern maruko_isp_impl  g_mi_isp;
extern maruko_scl_impl  g_mi_scl;
extern maruko_ai_impl   g_mi_ai;

/* Load all MI libraries. Returns 0 on success, -1 on failure.
 * Must be called before any MI_* macro is used. */
int maruko_mi_init(void);

/* Unload all MI libraries. Call after MI_SYS_Exit(). */
void maruko_mi_deinit(void);

/* Symbol loader helper (also used by maruko_pipeline.c for ad-hoc dlsym) */
void *maruko_load_symbol(void *handle, const char *lib_name,
	const char *sym_name);

#endif /* MARUKO_MI_H */
