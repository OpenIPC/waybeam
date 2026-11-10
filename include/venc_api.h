#ifndef VENC_API_H
#define VENC_API_H


/* The HTTP API contract version this build serves.  Must equal the version in
 * documentation/HTTP_API_CONTRACT.md — tests/test_venc_api.c reads that file
 * and fails if they drift.  0.73.0 shipped with the document on 0.22.0 and the
 * code still answering 0.21.0, so every craft advertised a contract it was not
 * serving. */
#define VENC_CONTRACT_VERSION "0.32.0"
#include "venc_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Callback table for applying live settings changes.
 * Each function returns 0 on success, -1 on error.
 * NULL entries mean the operation is not supported. */
typedef struct {
	int (*apply_bitrate)(uint32_t kbps);
	int (*apply_fps)(uint32_t fps);
	int (*apply_gop)(uint32_t gop_size);
	int (*apply_qp_delta)(int delta);
	int (*apply_roi_qp)(int qp);
	int (*apply_gain_max)(uint32_t gain);
	int (*apply_shutter_max)(uint32_t us);
	int (*apply_gain_min)(uint32_t gain);
	int (*apply_shutter_min)(uint32_t us);
	int (*apply_verbose)(bool on);
	int (*apply_output_enabled)(bool on);
	int (*apply_server)(const char *uri);
	int (*apply_mute)(bool on);
	int (*request_idr)(void);
	/* Live output FPS query. Returns 0 if unavailable. */
	uint32_t (*query_live_fps)(void);
	/* AE query: returns malloc'd JSON string, caller frees. NULL if unsupported. */
	char *(*query_ae_info)(void);
	/* AWB query: returns malloc'd JSON string, caller frees. NULL if unsupported. */
	char *(*query_awb_info)(void);
	/* ISP metrics: returns malloc'd text string, caller frees. NULL if unsupported. */
	char *(*query_isp_metrics)(void);
	/* AWB mode: 0=auto, 1=ct_manual. Returns 0 on success. */
	int (*apply_awb_mode)(int mode, uint32_t ct);
	/* Userspace AWB loop rate in Hz; 0 stops the loop and hands AWB back
	 * to the ISP-internal algorithm.  NULL on backends that have no
	 * userspace AWB loop (Maruko drives AWB from the SDK's own 3A). */
	int (*apply_awb_rate)(uint32_t hz);
	/* IQ query: returns malloc'd JSON string, caller frees. NULL if unsupported. */
	char *(*query_iq_info)(void);
	/* IQ set: param name + value string. Returns 0 on success, -1 on error. */
	int (*apply_iq_param)(const char *param, const char *value);
	/* Live-update the RTP/compact max packet payload size in bytes.
	 * The validator caps requests at VENC_OUTPUT_PAYLOAD_CEILING_BYTES
	 * and SHM ring slots are sized to that ceiling at startup, so any
	 * value reaching the callback fits every transport — backends do
	 * not need to reject size-based requests. Returns 0 on success,
	 * -1 if the backend has no live state to mutate. */
	int (*apply_max_payload_size)(uint16_t size);
	/* Live-update the RC QP bounds (MinQp/MaxQp) in the RC param struct.
	 * 0 leaves that bound at whatever the driver reported, so a config
	 * that sets neither behaves exactly as before. These are knobs the SDK
	 * rate controller actually honours: the floor governs whether CBR can
	 * spend its budget on a simple
	 * scene, the ceiling governs how hard it may compress a scene change.
	 * NULL on backends that do not implement it. */
	int (*apply_qp_bounds)(uint32_t min_qp, uint32_t max_qp);
	/* Output transport observability snapshot.  Returns malloc'd JSON
	 * string (caller frees) describing output queue fill, lifetime
	 * delivery counters, and backpressure state.  The JSON includes a
	 * "transport" discriminator field ("shm" | "udp" | "unix") so
	 * consumers don't have to guess.  Returns NULL only if the
	 * backend has no observability hook at all. */
	char *(*query_transport_status)(void);
	/* Audio capture/encode observability snapshot.  Returns malloc'd
	 * JSON string (caller frees) describing whether the audio backend
	 * library loaded, whether capture is running, the configured codec,
	 * and Opus encoder availability.  Returns NULL when audio is
	 * compiled out or the backend has no observability hook. */
	char *(*query_audio_status)(void);
	/* Apply digital zoom (video0.zoom_pct/x/y) live.  pct=0 disables.
	 * Pan smoothing time-constant is hardcoded per backend
	 * (PAN_RAMP_DEFAULT_MS) — see star6e_pipeline.c / maruko_pipeline.c.
	 * Returns 0 on success, -1 on backend/SDK error.  NULL when the
	 * backend has no zoom path. */
	int (*apply_zoom)(double pct, double x, double y);
	/* Reload the ISP tuning bin (isp.sensor_bin) on the running pipeline.
	 * Empty path falls back to /etc/sensors/<sensor>.bin on the SigmaStar
	 * backends; CV610 has no such convention and treats empty as a no-op,
	 * and its file is a HiSilicon PQTools .bin, not a SigmaStar one.
	 * Returns 0 on
	 * success (or no-op when the resolved path already matches the
	 * last-loaded one), -1 on resolve/load error.  NULL when the backend
	 * cannot reload bins without a full restart. */
	int (*apply_isp_bin)(const char *path);
	/* Serialize the live ISP state into a PQTools `.bin` at `path`,
	 * overwriting it.  Returns the byte count written, -1 on failure.  NULL when
	 * the backend has no such on-disk format -- the SigmaStar backends
	 * round-trip IQ as JSON instead (see /api/v1/iq/import). */
	int (*export_isp_bin)(const char *path);
	/* Live-update the MJPEG snapshot channel q-factor (1..99).  Hits
	 * MI_VENC_SetChnAttr on the running snapshot channel — no pipeline
	 * reinit, no buffer reallocation.  Returns 0 on success, -1 on
	 * SDK error or if the snapshot subsystem is disabled.  Both Star6E
	 * and Maruko register venc_jpeg_set_quality() as the implementation,
	 * which dispatches to the per-backend venc_jpeg_backend_set_quality
	 * hook (Get→modify→Set on the existing MJPEG channel). */
	int (*apply_snapshot_quality)(uint32_t q);
	/* Toggle the stab-fill compose bypass (video0.pause_stab).  Software-only
	 * (D13): glides the floating image to centre via the recenter ramp, no
	 * HW rebind.  Returns 0 on success, -1 if framing!=stab-fill.  NULL when
	 * the backend has no stab path (e.g. Maruko). */
	int (*apply_pause_stab)(bool paused);
	/* Live attitude snapshot for GET /api/v1/attitude: malloc'd JSON
	 * (caller frees), {"valid":false} until the estimator runs. NULL
	 * when the backend has no attitude path (e.g. Maruko). */
	char *(*query_attitude)(void);
	/* Level-pose boresight calibration: blocks ≤3 s averaging accel,
	 * solves attitude.trimRollDeg/trimPitchDeg. 0 on success, -1 on
	 * timeout (attitude/IMU not running) or implausible gravity.
	 * NULL when unsupported. */
	int (*attitude_calibrate_level)(float *roll_deg, float *pitch_deg);
	/* Live-swap the offline NPU detector .img without respawning the
	 * pipeline.  Fired when detect.model_path/model_id/conf_thresh/nms_iou
	 * change with the net geometry unchanged; the backend re-creates only the
	 * detector plugin + VPE tap while the encoder keeps running, so the
	 * video0 RTP stream is uninterrupted.  Reads the freshly-committed
	 * detect.* fields from the config.  Returns 0 once applied (detection is
	 * best-effort, so a failed swap that leaves detection off is not surfaced
	 * as an error — the requested config is kept).  NULL when the backend has
	 * no live detector path (e.g. Maruko). */
	int (*apply_detect_reload)(void);
} VencApplyCallbacks;

/* Register all API routes with the httpd.
 * cfg points to the live config (read for GET, modified by SET).
 * backend_name is "star6e" or "maruko" (used in /api/v1/version).
 * cb may be NULL (set endpoints will return not_implemented).
 * backend_ctx is passed only to backend-specific route handlers; NULL is
 * valid for backends without such routes. */
int venc_api_register(VencConfig *cfg, const char *backend_name,
	const VencApplyCallbacks *cb, void *backend_ctx);

/* Register the config path so restart-required sets can persist to disk
 * before triggering reinit.  Pass NULL to disable save-on-restart. */
void venc_api_set_config_path(const char *path);

/* Return 1 if a config field is supported on the named backend.
 * field_key may be canonical (video0.scene_threshold) or an accepted
 * alias (video0.sceneThreshold). */
int venc_api_field_supported_for_backend(const char *backend_name,
	const char *field_key);

/* Run the same per-field validation that /api/v1/set applies, over an
 * already-parsed config (e.g. just loaded from disk).  Returns NULL when
 * every validated field is in range, or a static error message string
 * pointing at the first offender.  Used by venc_config_load() to refuse
 * boot on configs that would crash MI_VENC_CreateChn or similar. */
const char *venc_api_validate_loaded_config(const VencConfig *cfg);

/* Pipeline reinit request flag (shared between API and backend).
 * One path: full teardown + reload from disk + start.  SIGHUP,
 * /api/v1/restart, /api/v1/defaults, and MUT_RESTART /api/v1/set
 * all enqueue the same request. */
void venc_api_request_reinit(void);
/** Monotonic count of reinit requests.  Sample it around an apply to learn
 *  whether THAT apply asked for a respawn; the boolean latch cannot say, since
 *  it shows no transition when one is already pending. */
unsigned venc_api_reinit_seq(void);
bool venc_api_get_reinit(void);
void venc_api_clear_reinit(void);

/* Record control flags (set by HTTP thread, consumed by main loop). */
void venc_api_request_record_start(const char *dir);
void venc_api_request_record_stop(void);

/* Returns 1 if start requested, copies dir into buf. */
int  venc_api_get_record_start(char *buf, size_t buf_size);
/* Returns 1 if stop requested, clears the flag. */
int  venc_api_get_record_stop(void);

/* Copy the configured recording directory (record.dir) into buf, falling
 * back to RECORDER_DEFAULT_DIR when unset.  Buf is always NUL-terminated
 * (truncated if needed). */
void venc_api_get_record_dir(char *buf, size_t buf_size);

/* Dual VENC channel API -- live controls for ch1 (dual-stream mode).
 * Call venc_api_dual_register() after pipeline_start_dual() to enable
 * the dual endpoints.  Call venc_api_dual_unregister() on
 * pipeline stop to disable them. */
void venc_api_dual_register(int channel, uint32_t bitrate, uint32_t fps,
	uint32_t gop);
void venc_api_dual_unregister(void);

/* Sensor info — set by backend after sensor_select() to expose via /api/v1/modes. */
void venc_api_set_sensor_info(int pad, int mode_index, int forced_pad);

/* Active VIF precrop rectangle — set by backend whenever it programs the
 * VIF capture region (initial start and reinit).  Includes any sensor
 * overscan offsets so the rectangle reflects exactly what is in hardware.
 * Exposed in /api/v1/config (runtime block) and /api/v1/ae. */
void venc_api_set_active_precrop(uint16_t x, uint16_t y,
	uint16_t w, uint16_t h);
void venc_api_clear_active_precrop(void);
int  venc_api_get_active_precrop(uint16_t *x, uint16_t *y,
	uint16_t *w, uint16_t *h);

/* VPE scaler-output tap map for /api/v1/config runtime.vpe_taps (Star6E only).
 * The backend's port arbiter publishes a JSON object string
 * (e.g. {"port0":["main","jpeg"],"port1":"detect"}) on every change; pass NULL
 * to clear.  Stored verbatim and emitted inside the config runtime block, so
 * an operator/CI can see which feature owns the single second scaler (port1)
 * and who rides the shared main output (port0).  venc_api does not interpret
 * the string — it only relays it. */
void venc_api_set_vpe_taps(const char *json_obj);
/* Copy the stored tap-map object into buf (NUL-terminated).  Returns 1 when a
 * map is published (buf filled), 0 otherwise (buf untouched). */
int  venc_api_get_vpe_taps(char *buf, size_t buf_size);

/* Record status callback — set by backend to expose status to HTTP API. */
typedef struct {
	int active;
	uint64_t bytes_written;
	/* Wall time since the active recorder started, CLOCK_MONOTONIC. Only
	 * the recorder knows when it started, and a consumer polling status
	 * cannot reconstruct it without missing everything before its first
	 * poll (or losing the count across its own restart). 0 when idle. */
	uint64_t elapsed_ms;
	uint32_t frames_written;
	uint32_t segments;
	/* Access units the async writer refused because its queue was full —
	 * i.e. the disk could not keep up and the RECORDING was sacrificed to
	 * keep the live video path unstalled.  Reported because a silent drop
	 * makes a damaged recording look like a clean one.  0 on backends that
	 * still write synchronously. */
	uint32_t dropped_frames;
	uint32_t writer_peak_depth;
	char path[256];
	char stop_reason[32];
	char format[16];
} VencRecordStatus;

typedef void (*VencRecordStatusFn)(VencRecordStatus *out);
void venc_api_set_record_status_fn(VencRecordStatusFn fn);
/* Opt in to HTTP-driven record control (gates /api/v1/record/start|stop).
 * Set independently of the status callback so a backend can report state
 * without claiming it consumes the start/stop request flags. */
void venc_api_set_record_http_control_supported(bool supported);

/* Fill the supplied struct with live record status via the registered
 * callback.  When no callback is set, zeroes the struct. */
void venc_api_fill_record_status(VencRecordStatus *out);

#ifdef __cplusplus
}
#endif

#endif /* VENC_API_H */
