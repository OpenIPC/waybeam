#ifndef STAR6E_CONTROLS_H
#define STAR6E_CONTROLS_H

#include "star6e_pipeline.h"
#include "venc_api.h"

enum {
	STAR6E_CONTROLS_IDLE_FPS = 5,
};

/** Bind pipeline and config to runtime control state. */
void star6e_controls_bind(Star6ePipelineState *pipeline, VencConfig *vcfg);

/** Reset control state to defaults (called between pipeline restarts). */
void star6e_controls_reset(void);

/** Return Star6E backend's live control callback table. */
const VencApplyCallbacks *star6e_controls_callbacks(void);

/** Apply frame rate change to running encoder pipeline. */
int star6e_controls_apply_fps(uint32_t fps);

/** Apply ROI-based QP adjustment for FPV center emphasis. */
int star6e_controls_apply_roi_qp(int qp);

/** Apply relative I/P QP delta to the running encoder. */
int star6e_controls_apply_qp_delta(int delta);




/** Service a pending detector live model-swap request on the pipeline thread.
 *  Called once per encode-loop iteration; a no-op when no swap is pending. */
void star6e_controls_service_detect_reload(void);

/** Compact AE/AWB live status for the debug OSD.  Mirrors MarukoAeOsdStatus
 *  (include/maruko_controls.h) so both backends render identical rows.  Gains
 *  are in SDK x1024 units (1024 = 1.0x), color_temp in Kelvin.  Validity flags
 *  gate their field groups: ae_valid covers shutter/gains (sensor-plane
 *  fallback — this is what keeps the row useful under CUS3A, where the ISP AE
 *  query may not populate); ae_info_valid additionally covers
 *  luma/target/stable/boundary (only the ISP AE query fills those); awb_valid
 *  covers the AWB group.  Limits are 0 when the limit query fails. */
typedef struct {
	int ae_valid;           /* shutter_us + gains populated */
	uint32_t shutter_us;
	uint32_t sgain_x1024;
	uint32_t igain_x1024;
	uint32_t max_shutter_us;
	uint32_t max_sgain;
	int ae_info_valid;      /* luma/target/stable/boundary populated */
	uint32_t luma_y;        /* AE-measured scene luma */
	uint32_t scene_target;  /* AE's current luma target */
	int stable;
	int boundary;           /* AE pinned at an exposure limit */
	int awb_valid;
	unsigned rgain, bgain;  /* AWB channel gains, x1024 */
	unsigned color_temp;    /* Kelvin */
	int awb_stable;
	int awb_userspace;      /* gains come from the star6e_awb loop, not the
	                         * ISP-internal algorithm — colour temperature is
	                         * not estimated in that mode */
	uint32_t awb_ticks;     /* loop applies since start (userspace mode) */
} Star6eAeOsdStatus;

/** Query live AE + AWB state (ISP SDK queries; ~1Hz callers only — each call
 *  dlopens libmi_isp and round-trips several MI_ISP getters). */
void star6e_controls_ae_osd_status(Star6eAeOsdStatus *out);

#endif /* STAR6E_CONTROLS_H */
