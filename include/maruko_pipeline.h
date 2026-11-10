#ifndef MARUKO_PIPELINE_H
#define MARUKO_PIPELINE_H

#include "imu_bmi270.h"
#include "maruko_audio.h"
#include "maruko_bindings.h"
#include "maruko_config.h"
#include "maruko_output.h"
#include "pipeline_common.h"
#include "scene_detector.h"
#include "sensor_select.h"
#include "frame_gate.h"
#include "star6e_recorder.h"
#include "star6e_ts_recorder.h"
#include "venc_rec_writer.h"

#include <pthread.h>
#include <signal.h>

struct DebugOsdState; /* forward declaration — see debug_osd.h */
struct MarukoDualVenc; /* forward declaration — see maruko_pipeline.c */

typedef struct {
	int active;
	uint32_t level_x100;
	uint32_t output_w;
	uint32_t output_h;
	uint32_t crop_x;
	uint32_t crop_y;
	uint32_t crop_w;
	uint32_t crop_h;
} MarukoZoomStatus;

typedef struct {
  int system_initialized;
  int sensor_enabled;
  int vif_started;
  int vpe_started;
  int venc_dev_created;
  int venc_started;
  int bound_vif_vpe;
  int bound_isp_vpe;
  int bound_vpe_venc;
  int stream_started;
  MarukoOutput output;
  /* Adaptive frame gate over the LIVE channel (venc_channel / chn 0).
   * See include/frame_gate.h and the Star6E twin in star6e_pipeline.h. */
  FrameGate frame_gate;
  volatile sig_atomic_t output_enabled;
  volatile uint32_t stored_fps;
  MI_VENC_DEV venc_device;
  MI_VENC_CHN venc_channel;
  MI_SYS_ChnPort_t vif_port;
  MI_SYS_ChnPort_t isp_port;
  MI_SYS_ChnPort_t vpe_port;       /* SCL chn 0 port 0 — feeds main VENC */
  MI_SYS_ChnPort_t scl_port1;      /* SCL chn 0 port 1 — feeds MJPG snapshot VENC */
  MI_SYS_ChnPort_t venc_port;
  SensorSelectResult sensor;
  MarukoBackendConfig cfg;
  SceneDetector scene;
  PipelineRateWatch rate_watch;
  struct DebugOsdState *debug_osd;  /* NULL if debug OSD disabled */
  ImuState *imu;                    /* NULL if IMU disabled or init failed */
  /* Dual VENC (gemini-style) — heap-allocated, NULL when inactive. */
  struct MarukoDualVenc *dual;
  /* Maruko IPU detector host (SCL0/0/3), private to maruko_ipu_yolo.c. */
  void *detect;
  /* TS recorder state.  Active when ts_recorder.fd >= 0; that is the
   * single source of truth (see star6e_ts_recorder_is_active()).  In
   * mirror mode the chn 0 frame loop drives it; in dual mode the chn 1
   * drain thread does. */
  Star6eTsRecorderState ts_recorder;
  /* Raw HEVC recorder state — used when record.format == "hevc" (Star6E
   * parity).  Active when recorder.fd >= 0.  At most one of recorder /
   * ts_recorder is active at a time; format dispatch happens at start. */
  Star6eRecorderState recorder;
  /* Mirror-mode recording writes on this thread, not the encode loop: a
   * blocking write(2) before maruko_mi_venc_release_stream() backs up the
   * encoder output queue and stalls the LIVE stream.  Dual mode already
   * drains chn 1 on its own thread and is unaffected.
   *
   * PER-RECORDING: created after the recorder file is open and joined
   * before it closes.  That join is the barrier — nothing else has to
   * serialise the sink against a close, and the pointer being non-NULL IS
   * "a recording is running", so the producer gate needs no recorder-state
   * predicate.  NULL whenever nothing is recording. */
  VencRecWriter *rec_writer;
  pthread_mutex_t rec_writer_lock;   /* guards rec_writer + the counters */
  /* rec_writer_lock is initialised during graph configure and destroyed at
   * teardown, so a teardown after a failed bring-up needs to know whether
   * it ever ran. */
  int rec_locks_ready;
  uint64_t rec_dropped_frames;
  /* Access units the SDK-typed flatten refused.  Reported as part of
   * droppedFrames: to an operator both are the same fact — a frame that is
   * not in the file. */
  uint64_t rec_flatten_failures;
  uint32_t rec_writer_peak_depth;
  /* Non-zero while a detached reaper still owns the recorders: it has not
   * joined a writer whose sink outlived the stop deadline, so their
   * descriptors are still live and a new recording must not reuse them. */
  int rec_reap_pending;
  /* SCL crop base after optional binning and AR-matched precrop.  Stored
   * here so maruko_pipeline_apply_zoom can reposition the zoom rect on
   * live x/y pan without recomputing pipeline geometry. */
  uint32_t scl_crop_x;
  uint32_t scl_crop_y;
  uint32_t scl_crop_w;
  uint32_t scl_crop_h;
  /* Audio capture + RTP/UDP output (Phase 5).  Inactive when
   * audio.enabled=false or libmi_ai.so is missing. */
  MarukoAudioState audio;
  /* Bridge from audio encode thread to TS recorder.  Owned here, not
   * inside the audio state, so init/teardown can be sequenced
   * independently of audio init failures. */
  AudioRing audio_recorder_ring;
} MarukoBackendContext;

/** Initialize Maruko pipeline state and load SDK libraries. */
int maruko_pipeline_init(MarukoBackendContext *ctx);

/** Configure and bind the hardware graph (sensor/ISP/VPE/VENC). */
int maruko_pipeline_configure_graph(MarukoBackendContext *ctx);

/** Run the encoding loop (blocks until shutdown signal).
 *  Returns 0 for clean exit, 1 for reinit requested, -1 for error. */
int maruko_pipeline_run(MarukoBackendContext *ctx);

/** Tear down the pipeline graph only (keep httpd and MI_SYS alive). */
void maruko_pipeline_teardown_graph(MarukoBackendContext *ctx);

/** Full teardown: pipeline + httpd + MI_SYS_Exit. */
void maruko_pipeline_teardown(MarukoBackendContext *ctx);

/** Install SIGINT/SIGTERM/SIGHUP handlers for graceful shutdown/reinit. */
void maruko_pipeline_install_signal_handlers(void);

/** Create the secondary VENC channel (chn 1) and start a thread that
 *  drains its frames.  The drain destination depends on `mode`:
 *
 *    - "dual-stream": send each frame as RTP to `server`.
 *    - "dual"       : feed each frame to ctx->ts_recorder (TS file
 *                     opened by maruko_pipeline_start_recording()).
 *
 *  Must be called AFTER bind_maruko_pipeline() has finished setting up
 *  channel 0 (the SDK probe in Phase 7 confirmed CreateChn(dev,1,...)
 *  is rejected before chn 0 is fully bound).
 *
 *  Returns 0 on success (ctx->dual is allocated and active).  On
 *  failure ctx->dual is NULL and the caller continues with chn 0
 *  only — non-fatal degradation. */
int maruko_pipeline_start_dual(MarukoBackendContext *ctx,
  uint32_t bitrate, uint32_t fps, double gop_sec,
  const char *mode, const char *server);

/** Tear down the secondary VENC channel if active.  Safe to call when
 *  ctx->dual is NULL (no-op). */
void maruko_pipeline_stop_dual(MarukoBackendContext *ctx);

/** Apply SCL digital zoom pan on chn 0.  pct=0 disables (full frame).
 *  zoom_pct changes are restart-required; live calls only reposition the
 *  crop at the active output dim.  Affects ch1 mirror as well because SCL
 *  fan-out happens downstream of the shared crop.  Returns 0 on success,
 *  -1 if the graph is not started or the rect was rejected. */
int maruko_pipeline_apply_zoom(MarukoBackendContext *ctx,
  double pct, double x, double y);
void maruko_pipeline_zoom_status(MarukoZoomStatus *out);

/** Framing module (video0.framing, e.g. "stab") bring-up / teardown.
 *  framing_setup is called after configure_graph (scl_crop set, VENC bound);
 *  it selects the module for vcfg and starts it, returning 0 whether or not one
 *  engaged.  framing_stop joins the detector + disables its tap and MUST run
 *  before the SCL port0 is torn down (it is, from teardown_graph). */
int maruko_pipeline_framing_setup(MarukoBackendContext *ctx,
  const VencConfig *vcfg);
void maruko_pipeline_framing_stop(void);
/** Live pause for the active framing module (video0.pause_stab). Returns 0 on
 *  success, -1 if no framing module is active. */
int maruko_pipeline_set_pause_stab(bool paused);

/** Reload the ISP tuning bin against the running pipeline.
 *  configured_path is the new bin location (NULL/empty falls back to
 *  /etc/sensors/<sensor>.bin keyed off ctx->sensor.plane.sensName).
 *  No-op when the resolved path matches the bin already loaded.
 *  Returns 0 on success / no-op, -1 on resolve or SDK failure. */
int maruko_pipeline_load_isp_bin_live(MarukoBackendContext *ctx,
  const char *configured_path);

extern volatile sig_atomic_t g_maruko_running;

/** Snapshot of the IntraRefresh configuration applied to ch0 at the most
 *  recent pipeline start.  Mirrors Star6eIntraRefreshStatus exactly so the
 *  /api/v1/intra/status handler can serialize either backend uniformly. */
typedef struct {
	char mode_name[16];             /* "off" | "fast" | "balanced" | "robust" */
	int active;
	int mi_supported;
	int apply_ok;
	uint32_t target_ms;
	uint32_t total_rows;
	uint32_t requested_lines;
	uint32_t effective_lines_per_p;
	int      lines_clamped;
	uint32_t requested_qp;
	uint32_t effective_qp;
	double   explicit_gop_sec;
	double   effective_gop_sec;
	int      gop_auto;
} MarukoIntraRefreshStatus;

void maruko_pipeline_intra_refresh_status(MarukoIntraRefreshStatus *out);

/* Mirror of Star6eRefPredStatus — refPred state at most recent pipeline
 * apply.  Populated by maruko_apply_ref_pred(). */
typedef struct {
	int      active;
	int      mi_supported;
	int      apply_ok;
	uint32_t base;
	uint32_t enhance;
	int      pred;
} MarukoRefPredStatus;

void maruko_pipeline_ref_pred_status(MarukoRefPredStatus *out);

void mk_mirror_record_locks_init_public(MarukoBackendContext *ctx);

#endif /* MARUKO_PIPELINE_H */
