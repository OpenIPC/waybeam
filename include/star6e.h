#ifndef STAR6E_PLATFORM_H
#define STAR6E_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SigmaStar MI API type definitions — self-contained, no HAL dependency. */
#include "sigmastar_types.h"

typedef int32_t MI_S32;
typedef uint32_t MI_U32;
typedef uint16_t MI_U16;
typedef uint8_t MI_U8;
typedef uint64_t MI_U64;
typedef bool MI_BOOL;

/* Common SigmaStar handles */
typedef i6_sys_bind MI_SYS_ChnPort_t;

enum {
  E_MI_MODULE_ID_VIF  = I6_SYS_MOD_VIF,
  E_MI_MODULE_ID_VPE  = I6_SYS_MOD_VPE,
  E_MI_MODULE_ID_VENC = I6_SYS_MOD_VENC,
};

typedef i6_snr_pad   MI_SNR_PadInfo_t;
typedef i6_snr_plane MI_SNR_PlaneInfo_t;
typedef i6_snr_res   MI_SNR_Res_t;

#if defined(PLATFORM_MARUKO)
typedef enum {
  E_MI_VIF_MODE_BT656 = I6_INTF_BT656,
  E_MI_VIF_MODE_DIGITAL_CAMERA = I6_INTF_DIGITAL_CAMERA,
  E_MI_VIF_MODE_BT1120_STANDARD = I6_INTF_BT1120_STANDARD,
  E_MI_VIF_MODE_BT1120_INTERLEAVED = I6_INTF_BT1120_INTERLEAVED,
  E_MI_VIF_MODE_MIPI = I6_INTF_MIPI,
  E_MI_VIF_MODE_LVDS = I6_INTF_END,
  E_MI_VIF_MODE_MAX,
} MI_VIF_IntfMode_e;

typedef enum {
  E_MI_VIF_WORK_MODE_1MULTIPLEX = I6_VIF_WORK_1MULTIPLEX,
  E_MI_VIF_WORK_MODE_2MULTIPLEX = I6_VIF_WORK_2MULTIPLEX,
  E_MI_VIF_WORK_MODE_4MULTIPLEX = I6_VIF_WORK_4MULTIPLEX,
  E_MI_VIF_WORK_MODE_MAX,
} MI_VIF_WorkMode_e;

typedef enum {
  E_MI_VIF_HDR_TYPE_OFF = I6_HDR_OFF,
  E_MI_VIF_HDR_TYPE_VC = I6_HDR_VC,
  E_MI_VIF_HDR_TYPE_DOL = I6_HDR_DOL,
  E_MI_VIF_HDR_TYPE_EMBEDDED = I6_HDR_EMBED,
  E_MI_VIF_HDR_TYPE_LI = I6_HDR_LI,
  E_MI_VIF_HDR_TYPE_MAX,
} MI_VIF_HDRType_e;

typedef enum {
  E_MI_VIF_CLK_EDGE_SINGLE_UP = I6_EDGE_SINGLE_UP,
  E_MI_VIF_CLK_EDGE_SINGLE_DOWN = I6_EDGE_SINGLE_DOWN,
  E_MI_VIF_CLK_EDGE_DOUBLE = I6_EDGE_DOUBLE,
  E_MI_VIF_CLK_EDGE_MAX,
} MI_VIF_ClkEdge_e;

typedef enum {
  E_MI_VIF_FRAMERATE_FULL = I6_VIF_FRATE_FULL,
  E_MI_VIF_FRAMERATE_HALF = I6_VIF_FRATE_HALF,
  E_MI_VIF_FRAMERATE_QUARTER = I6_VIF_FRATE_QUART,
  E_MI_VIF_FRAMERATE_OCTANT = I6_VIF_FRATE_OCTANT,
  E_MI_VIF_FRAMERATE_THREE_QUARTERS = I6_VIF_FRATE_3QUARTS,
  E_MI_VIF_FRAMERATE_MAX,
} MI_VIF_FrameRate_e;

enum {
  E_MI_VIF_GROUPMASK_ID0 = 0x0001,
};

enum {
  E_MI_SYS_COMPRESS_MODE_NONE = I6_COMPR_NONE,
};

typedef MI_U32 MI_VIF_GROUP;

typedef struct {
  MI_VIF_IntfMode_e eIntfMode;
  MI_VIF_WorkMode_e eWorkMode;
  MI_VIF_HDRType_e eHDRType;
  MI_VIF_ClkEdge_e eClkEdge;
  MI_U32 eMclk;
  MI_U32 eScanMode;
  MI_U32 u32GroupStitchMask;
} MI_VIF_GroupAttr_t;

typedef struct {
  i6_common_pixfmt eInputPixel;
  i6_common_rect stInputRect;
  MI_U32 eField;
  MI_BOOL bEnH2T1PMode;
} MI_VIF_DevAttr_t;

typedef struct {
  i6_common_rect stCapRect;
  i6_common_dim stDestSize;
  i6_common_pixfmt ePixFormat;
  MI_VIF_FrameRate_e eFrameRate;
  MI_U32 eCompressMode;
} MI_VIF_OutputPortAttr_t;

typedef MI_VIF_OutputPortAttr_t MI_VIF_PortAttr_t;
#else
typedef i6_vif_dev   MI_VIF_DevAttr_t;
typedef i6_vif_port  MI_VIF_PortAttr_t;
#endif

/* infinity6e (Star6E) driver expects the larger i6e_ struct variants which
   include additional lens-correction fields.  Using the smaller i6_ structs
   causes the driver to read past the struct boundary into stack garbage,
   corrupting VPE init and breaking the VIF→VPE REALTIME link. */
#if defined(PLATFORM_STAR6E)
typedef i6e_vpe_chn  MI_VPE_ChannelAttr_t;
typedef i6e_vpe_para MI_VPE_ChannelParam_t;
#else
typedef i6_vpe_chn   MI_VPE_ChannelAttr_t;
typedef i6_vpe_para  MI_VPE_ChannelParam_t;
#endif
typedef i6_vpe_port  MI_VPE_PortAttr_t;

typedef i6_venc_chn   MI_VENC_ChnAttr_t;
typedef i6_venc_pack  MI_VENC_Pack_t;
typedef i6_venc_stat  MI_VENC_Stat_t;
typedef i6_venc_strm  MI_VENC_Stream_t;

/* SVC-T per-frame reference type (MI_VENC_H264eRefType_e, H265 aliases it):
 * E_MI_VENC_ENHANCE_P_NOTFORREF = 5 — the droppable top enhance-layer frame.
 * NOTE: the SigmaStar enum inserts E_MI_VENC_BASE_P_REFTOIDR at index 1, so
 * this is 5, NOT 4 (the HiSilicon value); value 4 is ENHANCE_P_REFBYENHANCE,
 * a referenced enhance frame that must NOT be dropped. Device-verified on i6e
 * IMX335: 1:1 SVC-T droppable frames report refType=5. */
#define STAR6E_REFTYPE_ENHANCE_P_NOTFORREF 5

#if defined(PLATFORM_MARUKO)
typedef MI_U32 MI_VIF_DEV;
typedef MI_U32 MI_VIF_CHN;
typedef MI_U32 MI_VIF_PORT;
#else
typedef int MI_VIF_DEV;
typedef int MI_VIF_CHN;
typedef int MI_VIF_PORT;
#endif

typedef int MI_VPE_CHANNEL;
typedef int MI_VPE_PORT;

typedef int MI_VENC_CHN;
typedef int MI_VENC_DEV;

/* MI_SYS ------------------------------------------------------------------ */
#if defined(PLATFORM_MARUKO)
#include "maruko_mi.h"
#define MI_SYS_Init()  g_mi_sys.fnInit(0)
#define MI_SYS_Exit()  g_mi_sys.fnExit(0)
#define MI_SYS_BindChnPort(src, dst, src_fps, dst_fps) \
  g_mi_sys.fnBindChnPort2(0, (void *)(src), (void *)(dst), \
    (src_fps), (dst_fps), 0, 0)
#define MI_SYS_UnBindChnPort(src, dst) \
  g_mi_sys.fnUnBindChnPort(0, (void *)(src), (void *)(dst))
#define MI_SYS_BindChnPort2(src, dst, src_fps, dst_fps, link_type, link_param) \
  g_mi_sys.fnBindChnPort2(0, (void *)(src), (void *)(dst), \
    (src_fps), (dst_fps), (link_type), (link_param))
#define MI_SYS_SetChnOutputPortDepth(chn_port, user_depth, buf_depth) \
  g_mi_sys.fnSetChnOutputPortDepth(0, (void *)(chn_port), (user_depth), (buf_depth))
#elif defined(PLATFORM_STAR6E)
#include "star6e_mi.h"
#define MI_SYS_Init()  g_mi_sys.fnInit()
#define MI_SYS_Exit()  g_mi_sys.fnExit()
#define MI_SYS_BindChnPort(src, dst, sf, df) \
  g_mi_sys.fnBindChnPort((src), (dst), (sf), (df))
#define MI_SYS_UnBindChnPort(src, dst) \
  g_mi_sys.fnUnBindChnPort((src), (dst))
#define MI_SYS_BindChnPort2(src, dst, sf, df, lt, lp) \
  g_mi_sys.fnBindChnPort2((src), (dst), (sf), (df), (lt), (lp))
#define MI_SYS_SetChnOutputPortDepth(p, u, b) \
  g_mi_sys.fnSetChnOutputPortDepth((p), (u), (b))
#else
MI_S32 MI_SYS_Init(void);
MI_S32 MI_SYS_Exit(void);
MI_S32 MI_SYS_SetChnOutputPortDepth(const MI_SYS_ChnPort_t* chn_port,
  MI_U32 user_depth, MI_U32 buf_depth);
MI_S32 MI_SYS_BindChnPort(const MI_SYS_ChnPort_t* src, const MI_SYS_ChnPort_t* dst,
  MI_U32 src_fps, MI_U32 dst_fps);
MI_S32 MI_SYS_UnBindChnPort(const MI_SYS_ChnPort_t* src, const MI_SYS_ChnPort_t* dst);
MI_S32 MI_SYS_BindChnPort2(const MI_SYS_ChnPort_t* src, const MI_SYS_ChnPort_t* dst,
  MI_U32 src_fps, MI_U32 dst_fps, MI_U32 link_type, MI_U32 link_param);
#endif

/* MI_SNR ------------------------------------------------------------------ */
typedef enum {
  E_MI_SNR_PAD_ID_0 = 0,
  E_MI_SNR_PAD_ID_1 = 1,
  E_MI_SNR_PAD_ID_2 = 2,
  E_MI_SNR_PAD_ID_3 = 3,
} MI_SNR_PAD_ID_e;

typedef enum {
  E_MI_SNR_PLANE_MODE_LINEAR = 0,
} MI_SNR_PlaneMode_e;

typedef enum {
  E_MI_SNR_CUSTDATA_TO_DRIVER = 0,
  E_MI_SNR_CUSTDATA_TO_USER = 1,
} MI_SNR_CustDir_e;

typedef struct {
  MI_U32 u32DevId;
  MI_U8* u8Data;
} MI_SNR_InitParam_t;

#if defined(PLATFORM_MARUKO)
#define MI_SNR_InitDev(init)           g_mi_snr.fnInitDev(init)
#define MI_SNR_DeInitDev()             g_mi_snr.fnDeInitDev()
#define MI_SNR_SetPlaneMode(pad, mode) g_mi_snr.fnSetPlaneMode((pad), (mode))
/* Note: vendor MI_SNR_GetPlaneMode writes an int, but MI_BOOL is _Bool (1 byte).
 * Use a temp int to avoid writing past the bool boundary. */
static inline int _mi_snr_get_plane_mode(int pad, MI_BOOL *out) {
	int tmp = 0;
	int ret = g_mi_snr.fnGetPlaneMode(pad, &tmp);
	if (out) *out = (MI_BOOL)tmp;
	return ret;
}
#define MI_SNR_GetPlaneMode(pad, mode) _mi_snr_get_plane_mode((pad), (mode))
#define MI_SNR_SetRes(pad, idx)        g_mi_snr.fnSetRes((pad), (idx))
#define MI_SNR_GetCurRes(pad, idx, res) g_mi_snr.fnGetCurRes((pad), (idx), (res))
#define MI_SNR_SetFps(pad, fps)        g_mi_snr.fnSetFps((pad), (fps))
#define MI_SNR_GetFps(pad, fps)        g_mi_snr.fnGetFps((pad), (fps))
#define MI_SNR_SetOrien(pad, m, f)     g_mi_snr.fnSetOrien((pad), (m), (f))
#define MI_SNR_CustFunction(pad, cmd, sz, data, dir) \
  g_mi_snr.fnCustFunction((pad), (cmd), (sz), (data), (dir))
#define MI_SNR_QueryResCount(pad, cnt) g_mi_snr.fnQueryResCount((pad), (cnt))
#define MI_SNR_GetRes(pad, idx, res)   g_mi_snr.fnGetRes((pad), (idx), (res))
#define MI_SNR_GetPadInfo(pad, info)   g_mi_snr.fnGetPadInfo((pad), (info))
#define MI_SNR_GetPlaneInfo(pad, pl, info) g_mi_snr.fnGetPlaneInfo((pad), (pl), (info))
#define MI_SNR_Enable(pad)             g_mi_snr.fnEnable((pad))
#define MI_SNR_Disable(pad)            g_mi_snr.fnDisable((pad))
#elif defined(PLATFORM_STAR6E)
#define MI_SNR_InitDev(init)           g_mi_snr.fnInitDev(init)
#define MI_SNR_DeInitDev()             g_mi_snr.fnDeInitDev()
#define MI_SNR_SetPlaneMode(pad, mode) g_mi_snr.fnSetPlaneMode((pad), (mode))
/* Note: vendor MI_SNR_GetPlaneMode writes an int, but MI_BOOL is _Bool (1 byte).
 * Use a temp int to avoid writing past the bool boundary. */
static inline int _s6e_snr_get_plane_mode(int pad, MI_BOOL *out) {
	int tmp = 0;
	int ret = g_mi_snr.fnGetPlaneMode(pad, &tmp);
	if (out) *out = (MI_BOOL)tmp;
	return ret;
}
#define MI_SNR_GetPlaneMode(pad, mode) _s6e_snr_get_plane_mode((pad), (mode))
#define MI_SNR_SetRes(pad, idx)        g_mi_snr.fnSetRes((pad), (idx))
#define MI_SNR_GetCurRes(pad, idx, res) g_mi_snr.fnGetCurRes((pad), (idx), (res))
#define MI_SNR_SetFps(pad, fps)        g_mi_snr.fnSetFps((pad), (fps))
#define MI_SNR_GetFps(pad, fps)        g_mi_snr.fnGetFps((pad), (fps))
#define MI_SNR_SetOrien(pad, m, f)     g_mi_snr.fnSetOrien((pad), (m), (f))
#define MI_SNR_CustFunction(pad, cmd, sz, data, dir) \
  g_mi_snr.fnCustFunction((pad), (cmd), (sz), (data), (dir))
#define MI_SNR_QueryResCount(pad, cnt) g_mi_snr.fnQueryResCount((pad), (cnt))
#define MI_SNR_GetRes(pad, idx, res)   g_mi_snr.fnGetRes((pad), (idx), (res))
#define MI_SNR_GetPadInfo(pad, info)   g_mi_snr.fnGetPadInfo((pad), (info))
#define MI_SNR_GetPlaneInfo(pad, pl, info) g_mi_snr.fnGetPlaneInfo((pad), (pl), (info))
#define MI_SNR_Enable(pad)             g_mi_snr.fnEnable((pad))
#define MI_SNR_Disable(pad)            g_mi_snr.fnDisable((pad))
#else
MI_S32 MI_SNR_InitDev(MI_SNR_InitParam_t* init);
MI_S32 MI_SNR_DeInitDev(void);
MI_S32 MI_SNR_SetPlaneMode(MI_SNR_PAD_ID_e pad_id, MI_SNR_PlaneMode_e mode);
MI_S32 MI_SNR_GetPlaneMode(MI_SNR_PAD_ID_e pad_id, MI_BOOL* mode);
MI_S32 MI_SNR_SetRes(MI_SNR_PAD_ID_e pad_id, MI_U32 res_idx);
MI_S32 MI_SNR_GetCurRes(MI_SNR_PAD_ID_e pad_id, MI_U8* res_idx, MI_SNR_Res_t* res);
MI_S32 MI_SNR_SetFps(MI_SNR_PAD_ID_e pad_id, MI_U32 fps);
MI_S32 MI_SNR_GetFps(MI_SNR_PAD_ID_e pad_id, MI_U32* fps);
MI_S32 MI_SNR_SetOrien(MI_SNR_PAD_ID_e pad_id, MI_U8 mirror, MI_U8 flip);
MI_S32 MI_SNR_CustFunction(MI_SNR_PAD_ID_e pad_id, MI_U32 cmd_id,
  MI_U32 data_size, void* data, MI_SNR_CustDir_e dir);
MI_S32 MI_SNR_QueryResCount(MI_SNR_PAD_ID_e pad_id, MI_U32* count);
MI_S32 MI_SNR_GetRes(MI_SNR_PAD_ID_e pad_id, MI_U32 res_idx, MI_SNR_Res_t* res);
MI_S32 MI_SNR_GetPadInfo(MI_SNR_PAD_ID_e pad_id, MI_SNR_PadInfo_t* info);
MI_S32 MI_SNR_GetPlaneInfo(MI_SNR_PAD_ID_e pad_id, MI_U32 plane_idx, MI_SNR_PlaneInfo_t* info);
MI_S32 MI_SNR_Enable(MI_SNR_PAD_ID_e pad_id);
MI_S32 MI_SNR_Disable(MI_SNR_PAD_ID_e pad_id);
#endif

/* MI_VIF ------------------------------------------------------------------ */
#if defined(PLATFORM_MARUKO)
#define MI_VIF_SetDevAttr(dev, attr)    g_mi_vif.fnSetDevAttr((dev), (attr))
#define MI_VIF_EnableDev(dev)           g_mi_vif.fnEnableDev((dev))
#define MI_VIF_DisableDev(dev)          g_mi_vif.fnDisableDev((dev))
#define MI_VIF_CreateDevGroup(grp, attr) g_mi_vif.fnCreateDevGroup((grp), (attr))
#define MI_VIF_DestroyDevGroup(grp)     g_mi_vif.fnDestroyDevGroup((grp))
#define MI_VIF_SetOutputPortAttr(dev, port, attr) \
  g_mi_vif.fnSetOutputPortAttr((dev), (port), (attr))
#define MI_VIF_EnableOutputPort(dev, port)  g_mi_vif.fnEnableOutputPort((dev), (port))
#define MI_VIF_DisableOutputPort(dev, port) g_mi_vif.fnDisableOutputPort((dev), (port))
#define MI_VIF_SetChnPortAttr(chn, port, attr) \
  g_mi_vif.fnSetOutputPortAttr((MI_VIF_DEV)(chn), (MI_VIF_PORT)(port), (attr))
#define MI_VIF_EnableChnPort(chn, port) \
  g_mi_vif.fnEnableOutputPort((MI_VIF_DEV)(chn), (MI_VIF_PORT)(port))
#define MI_VIF_DisableChnPort(chn, port) \
  g_mi_vif.fnDisableOutputPort((MI_VIF_DEV)(chn), (MI_VIF_PORT)(port))
#elif defined(PLATFORM_STAR6E)
#define MI_VIF_SetDevAttr(dev, attr)    g_mi_vif.fnSetDevAttr((dev), (attr))
#define MI_VIF_EnableDev(dev)           g_mi_vif.fnEnableDev((dev))
#define MI_VIF_DisableDev(dev)          g_mi_vif.fnDisableDev((dev))
#define MI_VIF_SetChnPortAttr(chn, port, attr) \
  g_mi_vif.fnSetChnPortAttr((chn), (port), (attr))
#define MI_VIF_EnableChnPort(chn, port) g_mi_vif.fnEnableChnPort((chn), (port))
#define MI_VIF_DisableChnPort(chn, port) g_mi_vif.fnDisableChnPort((chn), (port))
#else
MI_S32 MI_VIF_SetDevAttr(MI_VIF_DEV dev, MI_VIF_DevAttr_t* attr);
MI_S32 MI_VIF_EnableDev(MI_VIF_DEV dev);
MI_S32 MI_VIF_DisableDev(MI_VIF_DEV dev);
MI_S32 MI_VIF_SetChnPortAttr(MI_VIF_CHN chn, MI_VIF_PORT port, MI_VIF_PortAttr_t* attr);
MI_S32 MI_VIF_EnableChnPort(MI_VIF_CHN chn, MI_VIF_PORT port);
MI_S32 MI_VIF_DisableChnPort(MI_VIF_CHN chn, MI_VIF_PORT port);
#endif

/* MI_VPE — Star6E only (Maruko uses ISP+SCL instead) */
#if defined(PLATFORM_STAR6E)
#define MI_VPE_CreateChannel(chn, attr) g_mi_vpe.fnCreateChannel((chn), (attr))
#define MI_VPE_GetChannelAttr(chn, attr) g_mi_vpe.fnGetChannelAttr((chn), (attr))
#define MI_VPE_SetChannelAttr(chn, attr) g_mi_vpe.fnSetChannelAttr((chn), (attr))
#define MI_VPE_DestroyChannel(chn)      g_mi_vpe.fnDestroyChannel((chn))
#define MI_VPE_StartChannel(chn)        g_mi_vpe.fnStartChannel((chn))
#define MI_VPE_StopChannel(chn)         g_mi_vpe.fnStopChannel((chn))
#define MI_VPE_SetChannelParam(chn, p)  g_mi_vpe.fnSetChannelParam((chn), (p))
#define MI_VPE_SetPortMode(chn, port, attr) g_mi_vpe.fnSetPortMode((chn), (port), (attr))
#define MI_VPE_EnablePort(chn, port)    g_mi_vpe.fnEnablePort((chn), (port))
#define MI_VPE_DisablePort(chn, port)   g_mi_vpe.fnDisablePort((chn), (port))
#define MI_VPE_SetPortCrop(chn, port, crop) g_mi_vpe.fnSetPortCrop((chn), (port), (crop))
#elif !defined(PLATFORM_MARUKO)
/* Test stubs */
MI_S32 MI_VPE_CreateChannel(MI_VPE_CHANNEL chn, MI_VPE_ChannelAttr_t* attr);
MI_S32 MI_VPE_GetChannelAttr(MI_VPE_CHANNEL chn, MI_VPE_ChannelAttr_t* attr);
MI_S32 MI_VPE_SetChannelAttr(MI_VPE_CHANNEL chn, MI_VPE_ChannelAttr_t* attr);
MI_S32 MI_VPE_DestroyChannel(MI_VPE_CHANNEL chn);
MI_S32 MI_VPE_StartChannel(MI_VPE_CHANNEL chn);
MI_S32 MI_VPE_StopChannel(MI_VPE_CHANNEL chn);
MI_S32 MI_VPE_SetChannelParam(MI_VPE_CHANNEL chn, MI_VPE_ChannelParam_t* param);
MI_S32 MI_VPE_SetPortMode(MI_VPE_CHANNEL chn, MI_VPE_PORT port, MI_VPE_PortAttr_t* attr);
MI_S32 MI_VPE_EnablePort(MI_VPE_CHANNEL chn, MI_VPE_PORT port);
MI_S32 MI_VPE_DisablePort(MI_VPE_CHANNEL chn, MI_VPE_PORT port);
MI_S32 MI_VPE_SetPortCrop(MI_VPE_CHANNEL chn, MI_VPE_PORT port,
	i6_common_rect *crop);
#endif

/* MI_VENC ROI -------------------------------------------------------------- */
typedef struct {
  MI_U32 u32Left;
  MI_U32 u32Top;
  MI_U32 u32Width;
  MI_U32 u32Height;
} MI_VENC_Rect_t;

typedef struct {
  MI_U32  u32Index;   /* ROI region index: 0-7 */
  MI_BOOL bEnable;
  MI_BOOL bAbsQp;     /* true=absolute QP, false=delta QP */
  MI_S32  s32Qp;      /* QP value (absolute) or QP offset (delta) */
  MI_VENC_Rect_t stRect;
} MI_VENC_RoiCfg_t;

#define RC_TEXTURE_THR_SIZE 1

typedef struct {
	MI_U32 u32MaxQp;
	MI_U32 u32MinQp;
	MI_S32 s32IPQPDelta;
	MI_U32 u32MaxIQp;
	MI_U32 u32MinIQp;
	MI_U32 u32MaxIPProp;
	MI_U32 u32MaxISize;
	MI_U32 u32MaxPSize;
} MI_VENC_ParamH264Cbr_t;

typedef struct {
	MI_S32 s32IPQPDelta;
	MI_S32 s32ChangePos;
	MI_U32 u32MaxIQp;
	MI_U32 u32MinIQP;
	MI_U32 u32MaxIPProp;
	MI_U32 u32MaxISize;
	MI_U32 u32MaxPSize;
} MI_VENC_ParamH264Vbr_t;

typedef struct {
	MI_S32 s32IPQPDelta;
	MI_S32 s32ChangePos;
	MI_U32 u32MinIQp;
	MI_U32 u32MaxIPProp;
	MI_U32 u32MaxIQp;
	MI_U32 u32MaxISize;
	MI_U32 u32MaxPSize;
	MI_U32 u32MinStillPercent;
	MI_U32 u32MaxStillQp;
} MI_VENC_ParamH264Avbr_t;

typedef struct {
	MI_U32 u32MaxQp;
	MI_U32 u32MinQp;
	MI_S32 s32IPQPDelta;
	MI_U32 u32MaxIQp;
	MI_U32 u32MinIQp;
	MI_U32 u32MaxIPProp;
	MI_U32 u32MaxISize;
	MI_U32 u32MaxPSize;
} MI_VENC_ParamH265Cbr_t;

typedef struct {
	MI_S32 s32IPQPDelta;
	MI_S32 s32ChangePos;
	MI_U32 u32MaxIQp;
	MI_U32 u32MinIQP;
	MI_U32 u32MaxIPProp;
	MI_U32 u32MaxISize;
	MI_U32 u32MaxPSize;
} MI_VENC_ParamH265Vbr_t;

typedef struct {
	MI_S32 s32IPQPDelta;
	MI_S32 s32ChangePos;
	MI_U32 u32MinIQp;
	MI_U32 u32MaxIPProp;
	MI_U32 u32MaxIQp;
	MI_U32 u32MaxISize;
	MI_U32 u32MaxPSize;
	MI_U32 u32MinStillPercent;
	MI_U32 u32MaxStillQp;
} MI_VENC_ParamH265Avbr_t;

typedef struct {
	MI_U32 u32MaxQfactor;
	MI_U32 u32MinQfactor;
} MI_VENC_ParamMjpegCbr_t;

typedef struct {
	MI_U32 u32ThrdI[RC_TEXTURE_THR_SIZE];
	MI_U32 u32ThrdP[RC_TEXTURE_THR_SIZE];
	MI_U32 u32RowQpDelta;
	union {
		MI_VENC_ParamH264Cbr_t stParamH264Cbr;
		MI_VENC_ParamH264Vbr_t stParamH264VBR;
		MI_VENC_ParamH264Avbr_t stParamH264Avbr;
		MI_VENC_ParamMjpegCbr_t stParamMjpegCbr;
		MI_VENC_ParamH265Cbr_t stParamH265Cbr;
		MI_VENC_ParamH265Vbr_t stParamH265Vbr;
		MI_VENC_ParamH265Avbr_t stParamH265Avbr;
	};
	void *pRcParam;
} MI_VENC_RcParam_t;

/* Intra refresh (GDR-style rolling stripe) — identical layout on star6e and
 * maruko (mi_venc_datatype.h:992 for both).  Only the function arity differs
 * (maruko adds VeDev), handled by the per-backend macros below. */
typedef struct {
	MI_BOOL bEnable;
	MI_U32  u32RefreshLineNum;
	MI_U32  u32ReqIQp;
} MI_VENC_IntraRefresh_t;
_Static_assert(sizeof(MI_VENC_IntraRefresh_t) == 12,
	"MI_VENC_IntraRefresh_t layout changed — verify SDK match");

/* Reference frame structure (SVC-T temporal hierarchical reference) —
 * identical layout on star6e and maruko (mi_venc_datatype.h:600).  Function
 * arity again differs (maruko adds VeDev). */
typedef struct {
	MI_U32  u32Base;
	MI_U32  u32Enhance;
	MI_BOOL bEnablePred;
} MI_VENC_ParamRef_t;
_Static_assert(sizeof(MI_VENC_ParamRef_t) == 12,
	"MI_VENC_ParamRef_t layout changed — verify SDK match");

/* H.265 multi-slice split (mi_venc_datatype.h:558) — u32SliceRowCount is
 * 32-px rows PER SLICE, not a slice count, and NOT CTU rows: the encoder's
 * CTU is 64 (measured 2026-08-20, SPS 30x17 CTBs at 1080p) and the SDK
 * rounds the request up to whole CTU-64 rows.  Symbol is exported by both
 * star6e and maruko libmi_venc.so; Maruko adds VeDev and therefore binds it
 * through its backend-specific loader. */
typedef struct {
	MI_BOOL bSplitEnable;
	MI_U32  u32SliceRowCount;
} MI_VENC_ParamH265SliceSplit_t;
_Static_assert(sizeof(MI_VENC_ParamH265SliceSplit_t) == 8,
	"MI_VENC_ParamH265SliceSplit_t layout changed — verify SDK match");

/* MI_VENC ----------------------------------------------------------------- */
#if defined(PLATFORM_MARUKO)
#define MI_VENC_CreateChn(chn, attr)  g_mi_venc.fnCreateChn(0, (chn), (attr))
#define MI_VENC_DestroyChn(chn)       g_mi_venc.fnDestroyChn(0, (chn))
#define MI_VENC_StartRecvPic(chn)     g_mi_venc.fnStartRecvPic(0, (chn))
#define MI_VENC_StopRecvPic(chn)      g_mi_venc.fnStopRecvPic(0, (chn))
#define MI_VENC_GetStream(chn, strm, ms) g_mi_venc.fnGetStream(0, (chn), (strm), (ms))
#define MI_VENC_ReleaseStream(chn, strm) g_mi_venc.fnReleaseStream(0, (chn), (strm))
#define MI_VENC_Query(chn, stat)      g_mi_venc.fnQuery(0, (chn), (stat))
#define MI_VENC_GetFd(chn)            g_mi_venc.fnGetFd(0, (chn))
#define MI_VENC_CloseFd(chn)          g_mi_venc.fnCloseFd(0, (chn))
#define MI_VENC_GetChnAttr(chn, attr) g_mi_venc.fnGetChnAttr(0, (chn), (attr))
#define MI_VENC_SetChnAttr(chn, attr) g_mi_venc.fnSetChnAttr(0, (chn), (attr))
#define MI_VENC_RequestIdr(chn, inst) g_mi_venc.fnRequestIdr(0, (chn), (inst))
#define MI_VENC_SetRoiCfg(chn, cfg)   g_mi_venc.fnSetRoiCfg(0, (chn), (cfg))
#define MI_VENC_GetRoiCfg(chn, idx, cfg) g_mi_venc.fnGetRoiCfg(0, (chn), (idx), (cfg))
#define MI_VENC_GetRcParam(chn, param) g_mi_venc.fnGetRcParam(0, (chn), (param))
#define MI_VENC_SetRcParam(chn, param) g_mi_venc.fnSetRcParam(0, (chn), (param))
#elif defined(PLATFORM_STAR6E)
#define MI_VENC_CreateChn(chn, attr)  g_mi_venc.fnCreateChn((chn), (attr))
#define MI_VENC_DestroyChn(chn)       g_mi_venc.fnDestroyChn((chn))
#define MI_VENC_StartRecvPic(chn)     g_mi_venc.fnStartRecvPic((chn))
#define MI_VENC_StopRecvPic(chn)      g_mi_venc.fnStopRecvPic((chn))
#define MI_VENC_GetStream(chn, strm, ms) g_mi_venc.fnGetStream((chn), (strm), (ms))
#define MI_VENC_ReleaseStream(chn, strm) g_mi_venc.fnReleaseStream((chn), (strm))
#define MI_VENC_Query(chn, stat)      g_mi_venc.fnQuery((chn), (stat))
#define MI_VENC_GetFd(chn)            g_mi_venc.fnGetFd((chn))
#define MI_VENC_CloseFd(chn)          g_mi_venc.fnCloseFd((chn))
#define MI_VENC_GetChnAttr(chn, attr) g_mi_venc.fnGetChnAttr((chn), (attr))
#define MI_VENC_SetChnAttr(chn, attr) g_mi_venc.fnSetChnAttr((chn), (attr))
#define MI_VENC_GetRcParam(chn, p)    g_mi_venc.fnGetRcParam((chn), (p))
#define MI_VENC_SetRcParam(chn, p)    g_mi_venc.fnSetRcParam((chn), (p))
#define MI_VENC_RequestIdr(chn, inst) g_mi_venc.fnRequestIdr((chn), (inst))
#define MI_VENC_SetMaxStreamCnt(chn, n) \
	(g_mi_venc.fnSetMaxStreamCnt ? g_mi_venc.fnSetMaxStreamCnt((chn), (n)) : -1)
#define MI_VENC_SetRoiCfg(chn, cfg)   g_mi_venc.fnSetRoiCfg((chn), (cfg))
#define MI_VENC_GetRoiCfg(chn, idx, cfg) g_mi_venc.fnGetRoiCfg((chn), (idx), (cfg))
#define MI_VENC_GetChnDevid(chn, dev) g_mi_venc.fnGetChnDevid((chn), (dev))
#define MI_VENC_SetIntraRefresh(chn, cfg) \
	(g_mi_venc.fnSetIntraRefresh ? \
		g_mi_venc.fnSetIntraRefresh((chn), (cfg)) : -1)
#define MI_VENC_GetIntraRefresh(chn, cfg) \
	(g_mi_venc.fnGetIntraRefresh ? \
		g_mi_venc.fnGetIntraRefresh((chn), (cfg)) : -1)
#define MI_VENC_SetRefParam(chn, p) \
	(g_mi_venc.fnSetRefParam ? \
		g_mi_venc.fnSetRefParam((chn), (p)) : -1)
#define MI_VENC_GetRefParam(chn, p) \
	(g_mi_venc.fnGetRefParam ? \
		g_mi_venc.fnGetRefParam((chn), (p)) : -1)
#define MI_VENC_SetH265SliceSplit(chn, p) \
	(g_mi_venc.fnSetH265SliceSplit ? \
		g_mi_venc.fnSetH265SliceSplit((chn), (p)) : -1)
#define MI_VENC_GetH265SliceSplit(chn, p) \
	(g_mi_venc.fnGetH265SliceSplit ? \
		g_mi_venc.fnGetH265SliceSplit((chn), (p)) : -1)
#else
MI_S32 MI_VENC_CreateChn(MI_VENC_CHN chn, MI_VENC_ChnAttr_t* attr);
MI_S32 MI_VENC_DestroyChn(MI_VENC_CHN chn);
MI_S32 MI_VENC_StartRecvPic(MI_VENC_CHN chn);
MI_S32 MI_VENC_StopRecvPic(MI_VENC_CHN chn);
MI_S32 MI_VENC_GetStream(MI_VENC_CHN chn, MI_VENC_Stream_t* stream, MI_S32 timeout_ms);
MI_S32 MI_VENC_ReleaseStream(MI_VENC_CHN chn, MI_VENC_Stream_t* stream);
MI_S32 MI_VENC_Query(MI_VENC_CHN chn, MI_VENC_Stat_t* stat);
int    MI_VENC_GetFd(MI_VENC_CHN chn);
MI_S32 MI_VENC_CloseFd(MI_VENC_CHN chn);
MI_S32 MI_VENC_GetChnAttr(MI_VENC_CHN chn, MI_VENC_ChnAttr_t* attr);
MI_S32 MI_VENC_SetChnAttr(MI_VENC_CHN chn, MI_VENC_ChnAttr_t* attr);
MI_S32 MI_VENC_GetRcParam(MI_VENC_CHN chn, MI_VENC_RcParam_t *param);
MI_S32 MI_VENC_SetRcParam(MI_VENC_CHN chn, MI_VENC_RcParam_t *param);
MI_S32 MI_VENC_RequestIdr(MI_VENC_CHN chn, MI_BOOL instant);
MI_S32 MI_VENC_SetRoiCfg(MI_VENC_CHN chn, MI_VENC_RoiCfg_t *cfg);
MI_S32 MI_VENC_GetRoiCfg(MI_VENC_CHN chn, MI_U32 idx, MI_VENC_RoiCfg_t *cfg);

MI_S32 MI_VENC_GetChnDevid(MI_VENC_CHN chn, MI_U32* device_id);
MI_S32 MI_VENC_SetRefParam(MI_VENC_CHN chn, MI_VENC_ParamRef_t *p);
MI_S32 MI_VENC_GetRefParam(MI_VENC_CHN chn, MI_VENC_ParamRef_t *p);
#endif

#ifdef __cplusplus
}
#endif

#endif
