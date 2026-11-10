#ifndef VENC_CONFIG_H
#define VENC_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Single fixed config path.  No CLI override, no legacy fallback —
 * deploy tooling is responsible for placing the file at
 * /etc/waybeam.json before launch. */
#define VENC_CONFIG_DEFAULT_PATH "/etc/waybeam.json"
#define VENC_CONFIG_STRING_MAX 256

/* Upper bound on outgoing.max_payload_size in bytes. Validation enforces
 * this in venc_api_validate_loaded_config() and the live `/api/v1/set`
 * path. SHM ring slots are sized at startup to fit
 * VENC_OUTPUT_PAYLOAD_CEILING_BYTES + 12 (RTP header) so any value in
 * [VENC_OUTPUT_PAYLOAD_MIN_BYTES, VENC_OUTPUT_PAYLOAD_CEILING_BYTES] can
 * be applied live regardless of the configured starting value, matching
 * UDP / unix:// behavior. Sized for jumbo-frame links such as the
 * Realtek 3993-byte MTU (4000 + 12 RTP + 8 UDP + 20 IP = 4040). */
#define VENC_OUTPUT_PAYLOAD_CEILING_BYTES 4000
#define VENC_OUTPUT_PAYLOAD_MIN_BYTES 576

/* Practical VENC bitrate bounds (kbps).  The min floor guards against
 * impractically low targets the encoder can't honor — below ~1 Mbit/s the
 * H.26x stream collapses (decoder drops SVC-T, RC oscillates).  Enforced as a
 * clamp (not a /set reject, so adaptive-link controllers that push low targets
 * keep working, just floored) on both the live apply_bitrate() paths and the
 * boot/reinit pipeline paths, so a persisted sub-floor bitrate can't birth the
 * encoder collapsed. */
#define VENC_BITRATE_MIN_KBPS 1000
#define VENC_BITRATE_MAX_KBPS 200000

/* Requested H.265 slices per picture.  A sanity bound, NOT the delivered
 * count: SigmaStar takes slice height in 32-px rows and quantizes it to
 * whole CTU-64 rows, while CV610 uses its own 32-pixel LCU-row geometry.
 * The delivered count therefore depends on backend and picture height.
 *
 * This was 8, on the stated grounds that the SDK's packetInfo table holds 8
 * NALs.  That table is per PACK and the output walker iterates packs
 * (star6e_output.c), so it never bounded slices per access unit: the GDR
 * refresh AU is a 17-slice picture and has always flowed through that walker
 * (measured 2026-08-20 on .232 — ~10,800 of them in 6 h, zero truncation
 * warnings).  The old cap made 9 and 17 unreachable and capped the fleet at
 * 6, since 7 and 8 both quantize down to 6. */
#define VENC_SLICE_COUNT_MAX 32

/* ── Sub-structs mirroring JSON sections ─────────────────────────────── */

typedef struct {
	uint16_t web_port;
	int overclock_level;   /* 0..2 */
	bool verbose;
} VencConfigSystem;

typedef struct {
	int index;             /* -1 = auto */
	int mode;              /* -1 = auto */
	bool unlock_enabled;
	uint32_t unlock_cmd;
	uint16_t unlock_reg;
	uint16_t unlock_value;
	int unlock_dir;        /* 0 = to_driver, 1 = to_user */
} VencConfigSensor;

typedef struct {
	char sensor_bin[VENC_CONFIG_STRING_MAX];
	/* AE engine selector.  "sdk" is the only value: the SDK firmware/bin AE
	 * drives convergence and a supervisory thread enforces the gain/shutter
	 * limits beside it (Star6E), or the paced-native 3A runs (Maruko).  The
	 * historical "custom" userspace governor was retired (Maruko 0.22.0,
	 * Star6E 0.47.0) and the value removed entirely — any non-"sdk" value now
	 * warns and falls back to "sdk". */
	char ae_engine[8];     /* "sdk" (only valid value) */
	uint32_t ae_fps;       /* custom AE rate in Hz (default 15) */
	uint32_t awb_fps;      /* userspace AWB loop rate in Hz (default 15, 0=off).
	                        * Decoupled from frame rate by design — this is
	                        * what keeps AWB affordable at 120fps. */
	uint32_t gain_max;     /* max sensor gain (0 = use ISP bin default) */
	char awb_mode[16];     /* "auto" or "ct_manual" */
	uint32_t awb_ct;       /* color temperature in Kelvin (for ct_manual) */
	bool keep_aspect;      /* true = center-crop sensor to encode aspect ratio
	                        * (preserves geometry); false = pass full sensor
	                        * downstream and stretch.  Star6E applies the
	                        * crop at VIF; Maruko applies it at the SCL
	                        * port.crop. */
	bool shutter_rule_180; /* 180° shutter rule: cap max shutter to
	                        * 1/(2*fps) instead of 1/fps, and disable
	                        * auto-exposure.  At 60 fps the cap becomes
	                        * 1/120 s (8333 µs).  false = default
	                        * frame-period cap (1/fps). */
	uint32_t shutter_max_us; /* live exposure cap in µs (0 = auto from
	                          * sensor_fps).  Setting this above the frame
	                          * period forces the sensor to skip frames,
	                          * reducing effective output FPS. */
	uint32_t gain_min;       /* min sensor gain floor (0 = use ISP bin
	                          * default).  Raises the darkest-scene gain the
	                          * supervisory AE may pick. */
	uint32_t shutter_min_us; /* min exposure floor in µs (0 = use ISP bin
	                          * default).  Overridden by shutter_rule_180,
	                          * which pins min==max. */
} VencConfigIsp;

typedef struct {
	bool mirror;
	bool flip;
	int rotate;            /* 0 or 180 */
} VencConfigImage;

typedef struct {
	/* Video codec is always H.265 — the encoder, RTP path, intra-refresh
	 * scheduler, and SVC-T refPred all assume HEVC.  H.264 was retired
	 * with the resilience-preset consolidation. */
	char rc_mode[16];      /* "cbr", "vbr", "avbr", "qvbr" */
	uint32_t fps;
	uint32_t width;
	uint32_t height;
	uint32_t bitrate;      /* kbps */
	double gop_size;       /* seconds between keyframes; 0 = all-intra */
	int qp_delta;              /* relative I/P QP delta, -12..12 */
	uint32_t min_qp;           /* RC QP floor; 0 = leave the driver default */
	uint32_t max_qp;           /* RC QP ceiling; 0 = leave the driver default */
	uint16_t scene_threshold;  /* frame size spike ratio x100 for scene IDR (0=off, 150=1.5x) */
	uint8_t scene_holdoff;     /* consecutive frames above threshold to trigger */
	uint32_t slice_count;      /* independent H.265 slices per picture,
	                            * 1..VENC_SLICE_COUNT_MAX; 1 = split off.
	                            * >1 enables spatial loss concealment on the
	                            * link RX (waybeam-link PROTOCOL.md §6.3b).
	                            * Star6E, Maruko and CV610. */
	/* Adaptive frame gate — stops draining the encoder's output FIFO while
	 * the frame-shm egress ring is not draining.  There is no enable
	 * switch: the gate arms itself on any frame-shm:// transport, and
	 * every other transport lacks the per-frame occupancy signal so it
	 * stays inert.  Where the recorder shares the encoder channel
	 * (record.mode "mirror") closes are suppressed while a recording runs,
	 * so congestion never punches holes in the file; reopens are never
	 * suppressed.  Both knobs take 0 for the frame_gate.h default. */
	uint32_t frame_gate_close_slots; /* 0 = FRAME_GATE_DEFAULT_CLOSE_SLOTS */
	uint32_t frame_gate_max_closed_ms; /* 0 = default; the safety escape */
	/* Derived from `resilience` preset only.  Not part of the JSON
	 * schema or HTTP API — written exclusively by
	 * apply_resilience_preset() at load time.  Do not parse from JSON,
	 * do not register in g_fields[].  Pipeline code reads these to
	 * drive MI_VENC_SetIntraRefresh / MI_VENC_SetRefParam. */
	char intra_refresh_mode[16]; /* "off" | "fast" | "balanced" | "robust" */
	uint16_t intra_refresh_lines; /* CTU rows refreshed per P-frame */
	uint8_t intra_refresh_qp;  /* I-CTU QP override for stripe */
	uint8_t ref_base;          /* SVC-T base-layer period; 0 = off */
	/* SVC-T reference PERIOD, not a count of non-reference frames:
	 * measured on Star6E (2026-08-06), exactly one frame in every
	 * (ref_enhance + 1) is emitted non-referenced.  So 1 is the most
	 * resilient value (50 % of frames droppable) and larger values are
	 * progressively less so — 4 gives 20 %, 299 gives 0.3 %. */
	uint8_t ref_enhance;
	bool ref_pred;             /* SVC-T enhance→base prediction */
	/* Resilience preset — sole user-facing knob for intra-refresh +
	 * SVC-T refPred + GOP.  Recognised values: "off", "quality",
	 * "racing", "range", "fpv", "ltr" / "ltr:<N>".  Every recognised
	 * value (including "off") overwrites the granular intra_refresh_*
	 * and ref_* fields with the preset's expansion; named presets
	 * additionally override gop_size, while "off" and the "ltr" family
	 * preserve the user's gopSize and (for "off") disable both
	 * intra-refresh and refPred.  Unknown values fall back to "off".
	 * See apply_resilience_preset() in src/venc_config.c for the
	 * canonical table. */
	char resilience[16];
	/* Framing mode — the single user-facing knob for what the crop path does
	 * (replaces the old standalone stab/zoom fields).  Recognised values:
	 *   "off"                              full image, no crop manipulation
	 *   "stab"                             digital image stabilization (HW crop
	 *                                      on both backends; Maruko via SCL crop,
	 *                                      Star6E via VPE crop)
	 *   "stab-fill"                        stabilization with a floating window
	 *                                      on a black border (Star6E; Maruko
	 *                                      planned)
	 *   "zoom-1.25x" | "zoom-1.50x" |      pure digital zoom (no stab) on
	 *   "zoom-1.75x" | "zoom-2x" | ...     both backends; zoom_x/y pan live
	 * The detector accuracy of the stab presets is a SEPARATE knob
	 * (stab_accuracy below), not a framing value.  Each preset expands at load
	 * time via apply_framing_preset() into the derived stab_crop_pct +
	 * stab_recenter_speed (stab presets) or zoom_pct (zoom presets); the two are
	 * mutually exclusive by construction.  Unknown values fall back to "off".
	 * Requires restart. */
	char framing[16];
	/* Derived from the `framing` preset only — NOT part of the JSON schema or
	 * HTTP API.  Written exclusively by apply_framing_preset() at load time.
	 * Do not parse from JSON, do not register in g_fields[].
	 *
	 * zoom_pct (Approach-C): shrinks BOTH the input crop and the encoded
	 * output dim — SCL 1:1, no upscale, no bandwidth pressure; the receiver
	 * sees the smaller resolution in SPS/PPS.  0 = off, 0.25..1.0 = crop
	 * fraction.  zoom_x/y pan is live (MUT_LIVE below). */
	double zoom_pct;
	double zoom_x;             /* crop centre x, 0..1 (live pan) */
	double zoom_y;             /* crop centre y, 0..1 (live pan) */
	/* Stabilization expansion (Star6E).  When a stab preset is active the
	 * source is clamped to <=1920x1080 (preserve aspect) to avoid the
	 * high-res fps regression, then the encoded resolution shrinks to
	 * (src_w * cropPct) x (src_h * cropPct).  Affects ch0 only; dual ch1,
	 * JPEG snapshot, and debug OSD see the unstabilized frame. */
	uint32_t stab_crop_pct;       /* 0 = off, 50..100 = crop % */
	uint32_t stab_recenter_speed; /* pauseStab glide-home rate (frames); 0 =
	                               * default ramp.  Inert during normal
	                               * stabilization (the Kalman recentres). */
	/* Kalman tuning knobs — the single control law for both "stab" and
	 * "stab-fill" (MUT_RESTART, scoped to the stab presets; inert under
	 * off/zoom).  Derive from the preset default unless explicitly overridden;
	 * see apply_framing_preset(). */
	double stab_kalman_q;         /* process noise / pan response. Supported
	                               * (API-validated) range 0.001..1.0; higher =
	                               * follows pans faster / weaker hold; preset
	                               * default 0.03.  (Loader clamp-fallback bound
	                               * is wider, 0.0001..5, for hand-edited files.) */
	double stab_kalman_r;         /* measurement noise / smoothness. Supported
	                               * (API-validated) range 0.1..50.0; higher =
	                               * smoother but laggier; preset default 2.0.
	                               * (Loader clamp-fallback bound is wider,
	                               * 0.01..200.) */
	/* Shift_Detector accuracy level (MUT_RESTART, shared by stab + stab-fill;
	 * inert under off/zoom).  "auto"|"high"|"medium"|"low" — the ONE detector
	 * geometry table lives in framing_stab_accuracy.h so Star6E and Maruko
	 * cannot drift.  "auto" (default) resolves per-backend: high on Star6E, low
	 * on single-core Maruko, so an unset field preserves each backend's
	 * historical behaviour.  Higher = smoother but costlier CPU. */
	char stab_accuracy[8];
	/* Runtime "stab-fill" bypass (MUT_LIVE).  Set true via
	 * /api/v1/set?pauseStab=true to glide the floating image back to centre
	 * (software ramp — no HW rebind); false resumes shake compensation.
	 * Not persisted: skipped by load/render/cJSON, so it always boots false
	 * and is never written to /etc/waybeam.json.  Inert unless
	 * framing=="stab-fill" (Star6E only). */
	bool pause_stab;
} VencConfigVideo;

typedef struct {
	bool enabled;
	uint32_t sample_rate;   /* Hz: 8000, 16000, 48000 */
	uint32_t channels;      /* 1 or 2 */
	char codec[16];         /* "pcm", "g711a", "g711u", "opus" */
	int volume;             /* 0..100 */
	bool mute;
} VencConfigAudio;

typedef struct {
	bool enabled;
	char server[VENC_CONFIG_STRING_MAX]; /* "udp://host:port", "unix://name",
	                                      * or "shm://name" */
	char stream_mode[16];               /* "rtp" or "compact" */
	uint16_t max_payload_size;
	bool connected_udp;             /* connect() socket (skip per-packet routing) */
	bool allow_unix_encoder_stall;   /* preserve blocking unix:// send semantics */
	int32_t audio_port;                 /* <0  = record-only: audio is captured and
	                                     *       recorded but NEVER streamed (no UDP send)
	                                     *  0   = same as video port — AVOID: mixing
	                                     *       audio and video RTP on one socket causes
	                                     *       video decoder instability at the receiver
	                                     * >0   = dedicated UDP audio port */
	uint16_t sidecar_port;              /* 0 = disabled */
} VencConfigOutgoing;

typedef struct {
	bool enabled;                              /* mDNS device beacon on/off */
	char service_type[VENC_CONFIG_STRING_MAX]; /* "_waybeam-venc._tcp"      */
	char name[VENC_CONFIG_STRING_MAX];         /* instance label; "" = host */
	bool bare_alias;                           /* also claim waybeam.local  */
} VencConfigDiscovery;

typedef struct {
	bool roi_enabled;
	int roi_qp;            /* signed ROI delta QP, -20..20 */
	uint16_t roi_steps;    /* 1..4 horizontal band regions */
	double roi_center;     /* center region fraction 0.1..0.9 */
	int noise_level;       /* 0..7 */
} VencConfigFpv;

typedef struct {
	bool enabled;
	char i2c_device[VENC_CONFIG_STRING_MAX];  /* "/dev/i2c-1" */
	uint8_t i2c_addr;                         /* 0x68 */
	int sample_rate_hz;                        /* 200 */
	int gyro_range_dps;                        /* 1000 */
	char cal_file[VENC_CONFIG_STRING_MAX];     /* "/etc/imu.cal" */
	int cal_samples;                           /* 400 */
} VencConfigImu;

typedef struct {
	bool enabled;
	char dir[VENC_CONFIG_STRING_MAX];
	char format[16];          /* "hevc" or "ts", default "ts" */
	char mode[16];            /* "off","mirror","dual","dual-stream" */
	uint32_t max_seconds;     /* rotation interval: 0=off, default 300 */
	uint32_t max_mb;          /* rotation size in MB: 0=off, default 500 */
	/* Dual/gemini channel settings (used when mode=dual or dual-stream) */
	uint32_t bitrate;         /* ch1 bitrate kbps, 0=match ch0 */
	uint32_t fps;             /* ch1 fps, 0=match sensor */
	double gop_size;          /* ch1 GOP in seconds, 0=match ch0 */
	char server[VENC_CONFIG_STRING_MAX]; /* dual-stream destination URI */
} VencConfigRecord;

typedef struct {
	bool show_osd;
} VencConfigDebug;

/* Attitude export over the rtp_sidecar ATTITUDE trailer
 * (protocols/rtp-sidecar.md). Requires imu.enabled; Star6E only for
 * now (Maruko's IMU push path is not wired). Lives in its own TRAILING
 * VencConfig member — the config ABI is append-only and VencConfigImu
 * is not the last member. */
typedef struct {
	bool enabled;        /* emit the sidecar attitude trailer          */
	int  mount_deg;      /* sensor yaw vs camera: 0/90/180/270         */
	bool invert_roll;    /* flip sign after mount rotation             */
	bool invert_pitch;
	char axis_fwd[4];    /* sensor axis out of the lens: "+x".."-z"    */
	char axis_down[4];   /* sensor axis pointing down when level       */
	float trim_roll_deg;  /* roll/pitch the estimator reads while the  */
	float trim_pitch_deg; /* camera is held level — undone input-side  */
} VencConfigAttitude;

typedef struct {
	bool enabled;       /* false → snapshot subsystem skipped entirely */
	uint32_t quality;   /* 1..100 MJPEG q-factor; clamped (0 → default 80) */
	int channel;        /* SDK VENC channel hint (Star6E ch7; ignored on Maruko which uses dedicated dev 8) */
	uint32_t width;     /* 0 → inherit from main stream */
	uint32_t height;    /* 0 → inherit from main stream */
} VencConfigSnapshot;

/* IPU NPU object detection (Star6E only). Feeds a VPE port1 NV12 tap to a
 * detector backend (see detect_plugin.h). Mutually exclusive with
 * video0.framing=stab (both claim the single VPE port1). Lives in its own
 * TRAILING VencConfig member — the config ABI is append-only. */
typedef struct {
	bool enabled;          /* run the IPU detector                        */
	char plugin[VENC_CONFIG_STRING_MAX];        /* detector plugin .so path  */
	char model_path[VENC_CONFIG_STRING_MAX];    /* offline .img; "" = default */
	char firmware_path[VENC_CONFIG_STRING_MAX]; /* IPU fw; "" = plugin default */
	int  infer_interval;   /* run 1 of every N captured frames (>=1)       */
	bool osd;              /* draw boxes on the debug OSD (needs
	                          debug.showOsd; native backends have no
	                          vehicle overlay of their own)              */
	/* Appended 2026-07-23 — new members go at the END; the config ABI is
	 * append-only (SigmaStar ISP bin loading breaks if VencConfig's
	 * layout shifts). */
	float    conf_thresh;  /* detector keep threshold; <=0 -> plugin
	                          default (0.40).  Worth lowering: INT8
	                          quantization costs ~30% of the FP32 score on
	                          small objects, and the ground tracker is
	                          built to confirm weak detections over time
	                          rather than trust any single frame.        */
	float    nms_iou;      /* NMS IoU; <=0 -> plugin default (0.45)      */
	uint32_t net_width;    /* Detector tap + model input width.          */
	uint32_t net_height;   /* MUST match the compiled .img —
	                          the backend rejects a mismatched frame.    */
	uint32_t model_id;     /* class-table selector put on the wire (see
	                          RTP_SIDECAR_DETECT_MODEL_* and the registry
	                          in protocols/rtp-sidecar.md).  Cannot be
	                          derived: the plugin loads whatever .img the
	                          config names, so only the operator knows
	                          which label table the boxes belong to.
	                          0 = VisDrone-10, 1 = SAR person.           */
} VencConfigDetect;

/* QR-oriented luma tap (Star6E only).  A read-only NV12 tap on VPE port1,
 * enabled once per pipeline run, whose luma plane is free of the MI_RGN
 * overlays that debug_osd and waybeam-hub's osd_render composite onto port0.
 * Mutually exclusive with video0.framing=stab and detect.enabled, which claim
 * the same single second-scaler output; this tap is the lowest-priority
 * claimant and is skipped when either holds port1.  Lives in its own TRAILING
 * VencConfig member — the config ABI is append-only. */
typedef struct {
	bool     tap_enabled;  /* program + enable the port1 luma tap        */
	uint32_t tap_width;    /* 0 → inherit the main stream                */
	uint32_t tap_height;   /* 0 → inherit the main stream.  Independent
	                          of port0: unlike the MJPEG snapshot
	                          channel, a VPE port really does scale.    */
	uint32_t window_ms;    /* scan-window budget; clamped 1000..60000.
	                          The port is held only this long, then the
	                          supervisor closes it and port1 goes back
	                          to stab/detect.                           */
} VencConfigQr;

/* ── Top-level config ────────────────────────────────────────────────── */

typedef struct {
	VencConfigSystem system;
	VencConfigSensor sensor;
	VencConfigIsp isp;
	VencConfigImage image;
	VencConfigVideo video0;
	VencConfigOutgoing outgoing;
	VencConfigDiscovery discovery;
	VencConfigFpv fpv;
	VencConfigAudio audio;
	VencConfigImu imu;
	VencConfigRecord record;
	VencConfigSnapshot snapshot;
	VencConfigDebug debug;
	VencConfigAttitude attitude;  /* trailing — ABI append-only */
	VencConfigDetect detect;      /* trailing — ABI append-only */
	VencConfigQr qr;              /* trailing — ABI append-only */
} VencConfig;

typedef enum {
	VENC_OUTPUT_URI_UDP = 0,
	VENC_OUTPUT_URI_UNIX = 1,
	VENC_OUTPUT_URI_SHM = 2,
	VENC_OUTPUT_URI_FRAME_SHM = 3,
} VencOutputUriType;

typedef struct {
	VencOutputUriType type;
	char host[128];
	uint16_t port;
	char endpoint[VENC_CONFIG_STRING_MAX];
} VencOutputUri;

/* Fill cfg with compiled defaults. */
void venc_config_defaults(VencConfig *cfg);

/* Load JSON config from path into cfg.  Missing keys keep their current
 * (default) values.  Returns 0 on success, -1 on parse error.
 * If the file does not exist, returns 0 (defaults are used). */
int venc_config_load(const char *path, VencConfig *cfg);

/* Parse the outgoing.server URI ("udp://host:port") into separate host and
 * port values.  Returns 0 on success, -1 on parse error. */
int venc_config_parse_server_uri(const char *uri, char *host, size_t host_len,
	uint16_t *port);

/* Parse outgoing.server into a transport-aware structure. */
int venc_config_parse_output_uri(const char *uri, VencOutputUri *out);

/* Serialize config to a newly allocated JSON string.  Caller must free(). */
char *venc_config_to_json_string(const VencConfig *cfg);

/* Save current config to a JSON file at path.
 * Returns 0 on success, -1 on write error. */
int venc_config_save(const char *path, const VencConfig *cfg);

/* Expand a resilience preset name into the derived intra_refresh_*, ref_*,
 * and (for named presets) gop_size fields of `v`.  Mirrors what the disk
 * loader does in load_video0() — exposed so the HTTP-API can evaluate the
 * delta a preset change *would* produce before deciding whether to take
 * the live-reinit path or the reboot-required path.
 *
 * Returns 0 on success ("off" or recognised preset), or -1 if `name` is
 * not recognised (caller should warn / fall back to "off"). */
int venc_config_apply_resilience_preset(const char *name, VencConfigVideo *v);

/* Expand a framing preset name ("off" | "low"|"medium"|"high" stab presets |
 * "zoom-1.25x"|"zoom-1.50x"|"zoom-1.75x"|"zoom-2x" zoom presets) into the
 * derived stab_crop_pct + stab_recenter_speed and/or zoom_pct fields of `v`
 * (the stab and zoom expansions are mutually exclusive).  Returns 0 on
 * success, or -1 if `name` is not recognised (caller should fall back to
 * "off").  See apply_framing_preset() in src/venc_config.c for the table. */
int venc_config_apply_framing_preset(const char *name, VencConfigVideo *v);

#ifdef __cplusplus
}
#endif

#endif /* VENC_CONFIG_H */
