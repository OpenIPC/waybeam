#ifndef MARUKO_CUS3A_H
#define MARUKO_CUS3A_H

#include <stdint.h>

/** Supervisory thread for Maruko AE (limits-only).
 *
 *  The vendor AE/AWB algorithms run under paced native 3A (see
 *  maruko_ae_pacer_thread in maruko_pipeline.c); this thread just enforces
 *  gain/shutter caps via MI_ISP_AE_SetExposureLimit and reads stats for
 *  verbose logging.  Equivalent to Star6E's pattern.  (The retired 0.9.12
 *  "aeEngine=custom" throttle controller was removed in v0.24.1 — paced
 *  native supersedes it on every axis; HISTORY 0.22.0.) */
typedef struct {
	uint32_t sensor_fps;       /* sensor output fps (for max shutter calc) */
	uint32_t ae_fps;           /* monitoring rate in Hz (default 15) */
	uint32_t shutter_max_us;   /* 0 = auto from sensor_fps */
	int      shutter_pin;      /* pin minShutter==maxShutter (180° rule) */
	uint32_t gain_max;         /* 0 = use ISP bin default */
	uint32_t shutter_min_us;   /* min exposure floor µs (0 = bin default;
	                            * ignored while shutter_pin is set) */
	uint32_t gain_min;         /* min sensor gain floor (0 = bin default) */
	int      verbose;          /* enable periodic status logging */
} MarukoCus3aConfig;

/** Fill config with sensible FPV defaults (sensor_fps=120, ae_fps=15). */
void maruko_cus3a_config_defaults(MarukoCus3aConfig *cfg);

/**
 * Start the pacing thread.
 *
 * Pre-conditions:
 *   - libmi_isp.so + libcus3a.so already opened by maruko_mi_init
 *   - MI_ISP_CUS3A_Enable(p100, p110) already called
 *   - MI_ISP_EnableUserspace3A called (so 3A_Proc_0 pumps IQ→HW)
 *   - CUS3A_SetRunMode(E_CUS3A_MODE_OFF) called (parks 3A_Proc_0)
 *   - ISP bin loaded
 *
 * Returns 0 on success, -1 on error.
 */
int maruko_cus3a_start(const MarukoCus3aConfig *cfg);

/** Stop and join the pacing thread. Safe if never started. */
void maruko_cus3a_stop(void);

/** Signal the thread to stop (non-blocking). Call join() later. */
void maruko_cus3a_request_stop(void);

/** Wait for the thread to exit after request_stop(). */
void maruko_cus3a_join(void);

/** Return 1 if the pacing thread is running. */
int maruko_cus3a_running(void);

/** Update the max sensor gain at runtime. */
void maruko_cus3a_set_gain_max(uint32_t gain);

/** Update the max shutter (exposure) at runtime. */
void maruko_cus3a_set_shutter_max(uint32_t us);
/** The sensor rate the auto shutter ceiling (shutter_max_us 0) follows;
 *  a live fps raise or rollback updates it. */
void maruko_cus3a_set_sensor_fps(uint32_t fps);

/** Update the min sensor gain floor at runtime. */
void maruko_cus3a_set_gain_min(uint32_t gain);

/** Update the min shutter (exposure) floor at runtime. */
void maruko_cus3a_set_shutter_min(uint32_t us);

#endif /* MARUKO_CUS3A_H */
