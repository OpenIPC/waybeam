#ifndef MARUKO_CONTROLS_H
#define MARUKO_CONTROLS_H

#include "maruko_bindings.h"
#include "maruko_output.h"
#include "maruko_pipeline.h"
#include "venc_api.h"

/** Bind pipeline context to runtime control state. */
void maruko_controls_bind(MarukoBackendContext *backend, VencConfig *vcfg);



/** Return Maruko backend's live control callback table. */
const VencApplyCallbacks *maruko_controls_callbacks(void);

/** Returns the VencConfig pointer registered via maruko_controls_bind, or
 * NULL if bind hasn't run yet.  Used by per-frame producer code (e.g. SHM
 * backpressure) that needs to read live config without threading a pointer
 * through every helper. */
const VencConfig *maruko_controls_vcfg(void);

/** Service a queued detector enable/disable/model reload on the pipeline
 * thread.  The HTTP callback only posts a coherent config snapshot. */
void maruko_controls_service_detect_reload(void);

/** Compact AE/AWB live status for the debug OSD.  Gains are in SDK x1024
 * units (1024 = 1.0x), color_temp in Kelvin.  Validity flags gate their
 * field groups: ae_valid covers shutter/gains (sensor-plane fallback);
 * ae_info_valid additionally covers luma/target/stable/boundary (only the
 * ISP AE query populates those); awb_valid covers the AWB group.  Limits
 * are 0 when the limit query fails. */
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
} MarukoAeOsdStatus;

/** Query live AE + AWB state (ISP SDK queries; ~1Hz callers only — each
 * call round-trips several MI_ISP getters). */
void maruko_controls_ae_osd_status(MarukoAeOsdStatus *out);

#endif /* MARUKO_CONTROLS_H */
