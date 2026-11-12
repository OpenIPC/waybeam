#ifndef MARUKO_CONFIG_H
#define MARUKO_CONFIG_H

#include "codec_types.h"
#include "sensor_select.h"
#include "star6e.h"
#include "venc_config.h"

#include <stdint.h>

typedef enum {
	MARUKO_STREAM_COMPACT = 0,
	MARUKO_STREAM_RTP = 1,
} MarukoStreamMode;

/* Subset of VencConfigRecord honoured by the Maruko backend.
 * Phase 6 lights up "mirror" (chn 0 → TS) and "dual" (chn 1 → TS) on top
 * of the existing "dual-stream" (chn 1 → UDP) and "off" modes.  Audio mux
 * and raw .hevc format are still Phase 5/6.5 territory — TS-only on
 * Maruko for now. */
typedef struct {
	int enabled;
	char mode[16];          /* "off", "mirror", "dual", "dual-stream" */
	char dir[256];          /* output directory for TS recordings */
	char format[16];        /* "ts" (only TS supported on Maruko today) */
	char server[256];       /* dual-stream destination URI */
	uint32_t bitrate;       /* chn 1 bitrate kbps, 0 = match chn 0 */
	uint32_t fps;           /* chn 1 fps, 0 = match sensor */
	double gop_size;        /* chn 1 GOP in seconds, 0 = match chn 0 */
	uint32_t max_seconds;   /* segment rotation, 0 = no time limit */
	uint32_t max_mb;        /* segment rotation, 0 = no size limit */
} MarukoBackendConfigRecord;

typedef struct {
	uint32_t sensor_width;
	uint32_t sensor_height;
	uint32_t image_width;
	uint32_t image_height;
	uint32_t sensor_fps;
	uint32_t venc_max_rate;
	uint32_t venc_gop_size;
	double venc_gop_seconds;
	uint16_t max_frame_size;
	uint16_t rtp_payload_size;
	VencOutputUri output_uri;
	uint16_t sidecar_port;
	PAYLOAD_TYPE_E rc_codec;
	int rc_mode;
	MarukoStreamMode stream_mode;
	MI_SNR_PAD_ID_e forced_sensor_pad;
	int forced_sensor_mode;
	/* Configured isp.sensorBin path; resolved (with sensor-name fallback)
	 * by the pipeline once sensor_select has run.  Empty string means
	 * "use default fallback". */
	char isp_bin_path[256];
	int vpe_level_3dnr;
	SensorUnlockConfig sensor_unlock;
	int oc_level;
	uint16_t scene_threshold;
	uint8_t scene_holdoff;
	/* IntraRefresh (GDR-style rolling stripe) — applied to ch0 after
	 * StartRecvPic.  Mirror of VencConfig::video0.intra_refresh_{mode,lines,qp}.
	 * Mode is parsed in pipeline; carry the raw string for traceability. */
	char intra_refresh_mode[16];
	uint16_t intra_refresh_lines;
	uint8_t intra_refresh_qp;
	/* SVC-T reference pyramid — mirror of video0.ref_{base,enhance,pred}. */
	uint8_t ref_base;
	uint8_t ref_enhance;
	uint8_t ref_pred;
	/* Requested whole-access-unit H.265 slice count. 1 disables splitting. */
	uint32_t slice_count;
	/* Adaptive frame gate — mirrors video0.frame_gate* so the Maruko
	 * pipeline, which sees MarukoBackendConfig rather than VencConfig,
	 * can resolve the gate at bring-up.  See include/frame_gate.h. */
	uint32_t frame_gate_close_slots;
	uint32_t frame_gate_max_closed_ms;
	/* Resilience preset name — mirror of video0.resilience.  Used by
	 * the debug OSD; pipeline behaviour is fully determined by the
	 * already-expanded intra_refresh_* / ref_* fields above. */
	char resilience[16];
	double gop_size_sec;            /* mirrors video0.gop_size; 0.0 = mode auto */
	int verbose;
	int connected_udp;
	int allow_unix_encoder_stall;
	int keep_aspect;
	int show_osd;
	uint32_t ae_fps;        /* supervisory 3A rate (Hz); 0 disables */
	uint32_t isp_gain_max;  /* sensor gain cap; 0 = use ISP bin default */
	uint32_t isp_shutter_max_us; /* exposure cap in µs; 0 = auto */
	uint32_t isp_gain_min;  /* sensor gain floor; 0 = use ISP bin default */
	uint32_t isp_shutter_min_us; /* exposure floor in µs; 0 = bin default */
	VencConfigImu imu;
	/* Audio capture mirror (Phase 5).  Init only runs when
	 * audio.enabled is set and libmi_ai.so is loaded. */
	VencConfigAudio audio;
	int32_t audio_port;             /* <0 → record-only (no stream); 0 → share
	                                 * video target; >0 → dedicated audio port */
	uint16_t max_payload_size;      /* mirrors outgoing.max_payload_size */
	/* Digital zoom (mirrors video0.zoom_pct/x/y).  Applied at pipeline
	 * start through the SCL crop/output config; live x/y pan reissues
	 * maruko_pipeline_apply_zoom at the same output dim. */
	double zoom_pct;
	double zoom_x;
	double zoom_y;
	int image_mirror;
	int image_flip;
	MarukoBackendConfigRecord record;
	/* MJPEG snapshot subsystem — mirror of VencConfigSnapshot.  Read by
	 * bind_maruko_pipeline() to fill VencJpegConfig; width=0/height=0
	 * inherits main stream dims. */
	VencConfigSnapshot snapshot;
	/* Mirror of video0.framing ("off"/"zoom"/"stab"/"stab-fill").  Read by
	 * the pipeline graph builder: "stab-fill" switches the SCL→VENC leg from
	 * the RING bind to the manual-compose bridge (SCL chn 1 → VENC
	 * FRAME_BASE) — see bind_maruko_pipeline().  The framing MODULE still
	 * selects off VencConfig; this mirror only steers graph topology. */
	char framing[16];
	int shutter_rule_180;
} MarukoBackendConfig;

/** Fill config with compiled-in defaults for Maruko backend. */
void maruko_config_defaults(MarukoBackendConfig *cfg);

/** Convert generic VencConfig into Maruko-specific backend config. */
int maruko_config_from_venc(const VencConfig *vcfg, MarukoBackendConfig *cfg);

#endif /* MARUKO_CONFIG_H */
