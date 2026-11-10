#ifndef STAR6E_PIPELINE_H
#define STAR6E_PIPELINE_H

#include "audio_ring.h"
#include "imu_bmi270.h"
#include "sdk_quiet.h"
#include "sensor_select.h"
#include "star6e_audio.h"
#include "star6e_output.h"
#include "star6e_recorder.h"
#include "star6e_ts_recorder.h"
#include "venc_rec_writer.h"
#include "star6e_video.h"
#include "frame_gate.h"
#include "venc_config.h"

#include <pthread.h>
#include <signal.h>
#include <time.h>

struct DebugOsdState; /* forward declaration — see debug_osd.h */
struct Star6eIpuDetect; /* forward declaration — see star6e_ipu_yolo.c */
struct Star6eLumaTap; /* forward declaration — see star6e_luma_tap.c */

/* Hard VENC encoder-input frame-rate ceiling on Infinity6E.  The SDK's
 * _MI_VENC_VerifyFps rejects any input FPS > 120 and silently resets it to
 * 30/1 ("Input invalid FPS:144/1 is over 120, set 30/1 by default"), which
 * makes CBR rate control budget for 30fps while ~143 frames/s actually arrive
 * -> ~4.7x bitrate overshoot.  Clamp the VPE->VENC bind dst AND the encoder
 * rate-control fps to this so a >120 sensor mode (e.g. 1600x900@144) encodes
 * cleanly at 120fps instead of overshooting.  Sensor/VIF may still run 143;
 * the framebase bind drops the surplus. */
#define STAR6E_VENC_INPUT_FPS_MAX 120u

typedef struct {
	uint16_t x;
	uint16_t y;
	uint16_t w;
	uint16_t h;
} Star6ePrecropRect;

typedef struct {
	int active;
	uint32_t level_x100;
	uint32_t output_w;
	uint32_t output_h;
	uint32_t crop_x;
	uint32_t crop_y;
	uint32_t crop_w;
	uint32_t crop_h;
} Star6eZoomStatus;

typedef struct {
	SensorSelectResult sensor;
	MI_VENC_CHN venc_channel;
	MI_SYS_ChnPort_t vif_port;
	MI_SYS_ChnPort_t vpe_port;
	MI_SYS_ChnPort_t venc_port;
	int bound_vif_vpe;
	int bound_vpe_venc;
	Star6eOutput output;
	/* Adaptive frame gate over the STREAM channel (venc_channel / ch0).
	 * Lives on the pipeline state, not the output, because the actuator
	 * is the VENC channel while the signal is the output's ring — and
	 * because a reinit must re-open the gate before the channel goes
	 * away. */
	FrameGate frame_gate;
	Star6eVideoState video;
	uint32_t image_width;
	uint32_t image_height;
	volatile sig_atomic_t output_enabled;
	volatile uint32_t stored_fps;
	Star6eAudioState audio;
	Star6eRecorderState recorder;
	Star6eTsRecorderState ts_recorder;
	AudioRing audio_ring;
	/* Mirror-mode recording runs its disk writes here, not on the encode
	 * loop.  A blocking write(2) between the transport send and
	 * MI_VENC_ReleaseStream() delays the release, backs up the encoder's
	 * output queue and stalls the LIVE stream — measured on CV610 at 22%
	 * of normal throughput (see include/venc_rec_writer.h).  Dual mode is
	 * unaffected: it already drains ch1 on its own thread.
	 *
	 * PER-RECORDING: created after the recorder file is open and joined
	 * before it closes.  That join is the barrier — nothing else has to
	 * serialise the sink against a close, and the pointer being non-NULL
	 * IS "a recording is running", so the producer gate needs no
	 * recorder-state predicate.  NULL whenever nothing is recording. */
	VencRecWriter *rec_writer;
	pthread_mutex_t rec_writer_lock;   /* guards rec_writer + the counters */
	/* rec_writer_lock is initialised in one runtime callback and destroyed
	 * in another, so teardown after a failed bring-up needs to know
	 * whether it ever ran. */
	int rec_locks_ready;
	uint64_t rec_dropped_frames;
	/* Access units the SDK-typed flatten refused (an incomplete packetInfo
	 * table, or one over the queue's byte cap).  Reported as part of
	 * droppedFrames rather than as a field of its own: to an operator both
	 * are the same fact — a frame that is not in the file — and a silent
	 * one would make a damaged recording look clean. */
	uint64_t rec_flatten_failures;
	uint32_t rec_writer_peak_depth;
	/* Non-zero while a detached reaper still owns the recorders: it has not
	 * joined a writer whose sink outlived the stop deadline, so their
	 * descriptors are still live and a new recording must not reuse them. */
	int rec_reap_pending;
	ImuState *imu;              /* NULL if IMU disabled */
	MI_VENC_Pack_t *stream_packs;     /* pre-allocated for main loop */
	uint32_t stream_packs_cap;
	/* Dual VENC (gemini mode) — heap-allocated, NULL when inactive */
	struct Star6eDualVenc *dual;
	struct DebugOsdState *debug_osd;  /* NULL if debug OSD disabled */
	struct Star6eIpuDetect *detect;   /* NULL when IPU detection inactive */
	struct Star6eLumaTap *luma_tap;  /* pipeline-owned QR tap context */
	Star6ePrecropRect active_precrop; /* precrop currently programmed into VIF
	                                   * (includes overscan offsets) */
} Star6ePipelineState;

/** Dual VENC channel state. Heap-allocated to avoid changing
 *  VencConfig struct size (which breaks SigmaStar ISP bin loading). */
typedef struct Star6eDualVenc {
	MI_VENC_CHN channel;
	MI_SYS_ChnPort_t port;
	int bound;
	Star6eOutput output;
	Star6eVideoState video;
	char mode[16];
	uint32_t bitrate;
	uint32_t fps;
	uint32_t gop;
	char server[64];
	/* Recording thread — drains ch1 frames independently of main loop */
	pthread_t rec_thread;
	volatile sig_atomic_t rec_running;
	volatile sig_atomic_t rec_started;
	Star6eTsRecorderState *ts_recorder;  /* borrowed, NULL in dual-stream */
	AudioRing *audio_ring;               /* borrowed, NULL when audio off */
	int is_dual_stream;
	MI_VENC_Pack_t *stream_packs;     /* pre-allocated for rec thread */
	uint32_t stream_packs_cap;
	/* HTTP /api/v1/record/start|stop forwarding (dual mode only —
	 * not dual-stream, which has no on-disk recorder).  The main loop
	 * consumes the venc_api pending flag and sets rec_req_start_dir +
	 * rec_req_start (or rec_req_stop) here; the recording thread acts
	 * between frame writes so the ts_recorder stays single-threaded. */
	volatile sig_atomic_t rec_req_start;
	volatile sig_atomic_t rec_req_stop;
	char rec_req_start_dir[256];
} Star6eDualVenc;

/** Create secondary VENC channel and bind to VPE output.
 *  Returns 0 on success (state->dual is allocated and active).
 *  On failure, state->dual is NULL and pipeline operates in mirror mode. */
int star6e_pipeline_start_dual(Star6ePipelineState *state,
	uint32_t bitrate, uint32_t fps, double gop_sec,
	const char *mode, const char *server);

/** Tear down secondary VENC channel if active. */
void star6e_pipeline_stop_dual(Star6ePipelineState *state);

/** Initialize and start the full encoder pipeline (sensor → VENC). */
int star6e_pipeline_start(Star6ePipelineState *state, const VencConfig *vcfg,
	SdkQuietState *sdk_quiet);

/** Stop streaming, unbind hardware, and release pipeline resources.
 *  Also clears pipeline-level persist state so the next start is cold. */
void star6e_pipeline_stop(Star6ePipelineState *state);

/** Start / stop the IPU detection tap on a running pipeline, under the VPE
 *  port1 arbiter (start applies the framing-conflict policy and returns 0 only
 *  if detection is up).  Called by the initial bring-up and by the live
 *  `detect.enabled` toggle — both on the pipeline thread. */
int star6e_pipeline_detect_start(Star6ePipelineState *state,
	const VencConfig *vcfg);
void star6e_pipeline_detect_stop(Star6ePipelineState *state);

/** Disable VPE prescaler (cleanup during shutdown). */
void star6e_pipeline_vpe_scl_preset_shutdown(void);

/** Apply digital zoom on VPE port 0.  pct=0 disables (full-frame).
 *  x/y set the *target* pan position; the live crop decays toward it
 *  with a backend-hardcoded time constant (PAN_RAMP_DEFAULT_MS).
 *  Returns 0 on success, -1 if VPE not yet started or SDK rejected the
 *  rect.  Safe to call before pipeline start (no-op returns -1). */
int star6e_pipeline_apply_zoom(Star6ePipelineState *state,
	double pct, double x, double y);

/** Toggle the stab-fill compose bypass (video0.pauseStab).  Software-only
 *  (D13): when paused the detector glides the applied offset back to centre
 *  via the recenter ramp and the compose feeds the unshifted full frame — no
 *  HW rebind, no thread teardown.  Routed to the active framing module's
 *  set_live hook.  Returns 0 on success, -1 when framing!=stab-fill (no-op). */
int star6e_pipeline_set_pause_stab(bool paused);

void star6e_pipeline_zoom_status(Star6eZoomStatus *out);

/** Service custom 3A (AWB/AE) at regular intervals. */
/** One-shot cold-boot fps re-kick.  Call once ~1.5s after pipeline start from
 *  the run loop; re-issues MI_SNR_SetFps to force the sensor timing register to
 *  the configured fps (the init-time kick fires before the ISP bin settles and
 *  can leave the sensor locked low on a cold boot).  No-op when fps is 0. */
void star6e_pipeline_cold_boot_fps_rekick(const Star6ePipelineState *state,
	const VencConfig *vcfg);

void star6e_pipeline_cus3a_tick(SdkQuietState *sdk_quiet,
	struct timespec *ts_last);

/** Choose which algorithm owns AWB: 1 = userspace (the star6e_awb loop),
 *  0 = ISP-internal (required by isp.awbMode=ct_manual, whose colour-
 *  temperature apply path targets the internal algorithm).  Applied live when
 *  the CUS3A handoff has already run. */
void star6e_pipeline_set_awb_userspace(int on);

/** Reset CUS3A handoff state (call on pipeline reinit). */
void star6e_pipeline_cus3a_reset(void);

/** Calculate max exposure time to avoid frame drops at target FPS.
 *  shutter_rule_180 = true halves the cap (180° shutter rule). */
int star6e_pipeline_cap_exposure_for_fps(uint32_t fps,
	bool shutter_rule_180);

/** Reload the ISP tuning bin against the running pipeline.
 *  configured_path is the new bin location (NULL/empty falls back to
 *  /etc/sensors/<sensor>.bin keyed off sensor_name).  vcfg supplies
 *  contextual state (video0.fps for the exposure cap) — its isp.sensor_bin
 *  is NOT consulted.  No-op when the resolved path matches the bin already
 *  loaded.  Reapplies the FPS-derived exposure cap on success since the
 *  bin's AE limits may override it, and kicks MI_SNR_SetFps so the sensor's
 *  physical shutter register isn't left at the bin's cold-boot value (which
 *  would lock the sensor below the configured fps — see star6e_pipeline.c
 *  bind_and_finalize_pipeline comment).  Returns 0 on success / no-op, -1 on
 *  resolve or SDK failure. */
int star6e_pipeline_load_isp_bin_live(const char *configured_path,
	const VencConfig *vcfg, const char *sensor_name,
	MI_SNR_PAD_ID_e pad_id, uint32_t sensor_framerate);

/** Snapshot of the IntraRefresh configuration applied to ch0 at the most
 *  recent pipeline start.  All zeros (mode_name="off") when feature is
 *  disabled.  Populated by mode-driven path in star6e_pipeline.c. */
typedef struct {
	char mode_name[16];             /* "off" | "fast" | "balanced" | "robust" */
	int active;                     /* mode != off and apply_ok */
	int mi_supported;               /* libmi_venc.so exports SetIntraRefresh */
	int apply_ok;                   /* SetIntraRefresh succeeded */
	uint32_t target_ms;             /* mode constant, 0 if off */
	uint32_t total_rows;            /* ceil(height / lcu_h) */
	uint32_t requested_lines;       /* config override value (0 = mode auto) */
	uint32_t effective_lines_per_p; /* what was actually programmed */
	int      lines_clamped;         /* override exceeded total_rows */
	uint32_t requested_qp;          /* config override value (0 = codec default) */
	uint32_t effective_qp;          /* what was actually programmed */
	double   explicit_gop_sec;      /* config gop_size (0.0 = mode auto) */
	double   effective_gop_sec;     /* what was actually programmed */
	int      gop_auto;              /* 1 if effective_gop_sec came from auto */
} Star6eIntraRefreshStatus;

void star6e_pipeline_intra_refresh_status(Star6eIntraRefreshStatus *out);

/* Snapshot of refPred (SVC-T) state at the most recent pipeline_start.
 * Populated by star6e_pipeline_pre_start_apply_ref_pred() — `active` is
 * true only when the resilience preset requested refBase>0 AND the
 * SDK SetRefParam call succeeded. */
typedef struct {
	int      active;
	int      mi_supported;
	int      apply_ok;
	uint32_t base;
	uint32_t enhance;
	int      pred;
} Star6eRefPredStatus;

void star6e_pipeline_ref_pred_status(Star6eRefPredStatus *out);

#endif /* STAR6E_PIPELINE_H */
