#include "venc_config.h"
#include "venc_api.h"
#include "framing_stab_accuracy.h"
#include "pipeline_common.h"
#include "star6e_recorder.h"
#include "intra_refresh.h"
#include "frame_gate.h"
#include "../lib/cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void safe_strcpy(char *dst, size_t dst_size, const char *src)
{
	if (!src) {
		dst[0] = '\0';
		return;
	}
	size_t len = strlen(src);
	if (len >= dst_size)
		len = dst_size - 1;
	memcpy(dst, src, len);
	dst[len] = '\0';
}

static const char *json_get_string(const cJSON *obj, const char *key,
	const char *fallback)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (cJSON_IsString(item) && item->valuestring)
		return item->valuestring;
	return fallback;
}

static int json_get_int(const cJSON *obj, const char *key, int fallback)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (cJSON_IsNumber(item))
		return item->valueint;
	return fallback;
}

static bool json_get_bool(const cJSON *obj, const char *key, bool fallback)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (cJSON_IsBool(item))
		return cJSON_IsTrue(item) ? true : false;
	return fallback;
}

static double json_get_double(const cJSON *obj, const char *key, double fallback)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (cJSON_IsNumber(item))
		return item->valuedouble;
	return fallback;
}

/* ── Defaults ────────────────────────────────────────────────────────── */

void venc_config_defaults(VencConfig *cfg)
{
	memset(cfg, 0, sizeof(*cfg));

	/* system */
	cfg->system.web_port = 80;
	cfg->system.overclock_level = 1;
	cfg->system.verbose = false;

	/* sensor.  The unlock_* fields drive the IMX415/IMX335 high-FPS
	 * register hook (`MI_SNR_CustFunction(pad, cmd_id=0x23, reg=0x300a,
	 * value=0x80, dir=0)`) — required on cold boot for both sensors,
	 * otherwise `MI_SNR_SetFps(pad, 120)` returns -1608835041 and the
	 * sensor falls back to 30 fps.  Retired from the user-facing JSON
	 * schema in 0.10.13; the call now fires unconditionally at every
	 * pipeline start. */
	cfg->sensor.index = -1;
	cfg->sensor.mode = -1;
	cfg->sensor.unlock_enabled = true;
	cfg->sensor.unlock_cmd = 0x23;
	cfg->sensor.unlock_reg = 0x300a;
	cfg->sensor.unlock_value = 0x80;
	cfg->sensor.unlock_dir = 0;

	/* isp.  ae_engine is "sdk"-only (the SDK firmware/bin AE drives
	 * convergence; a supervisory thread enforces limits beside it). */
	cfg->isp.sensor_bin[0] = '\0';
	safe_strcpy(cfg->isp.ae_engine, sizeof(cfg->isp.ae_engine), "sdk");
	cfg->isp.ae_fps = 15;
	cfg->isp.awb_fps = 15;
	safe_strcpy(cfg->isp.awb_mode, sizeof(cfg->isp.awb_mode), "auto");
	cfg->isp.awb_ct = 5500;
	cfg->isp.keep_aspect = true;
	cfg->isp.shutter_rule_180 = false;

	/* image */
	cfg->image.mirror = false;
	cfg->image.flip = false;
	cfg->image.rotate = 0;

	/* video0 — codec is hardcoded H.265, see VencConfigVideo doc */
	safe_strcpy(cfg->video0.rc_mode, sizeof(cfg->video0.rc_mode), "cbr");
	cfg->video0.fps = 60;
	cfg->video0.width = 0;
	cfg->video0.height = 0;
	cfg->video0.bitrate = 8192;
	cfg->video0.gop_size = 1.0;
	/* -12, matching config/waybeam.default{,.maruko}.json since 0.73.0.
	 * Seeded here as well as shipped in the JSON because this is what a
	 * config that OMITS qpDelta gets, and what GET /api/v1/defaults hands
	 * back -- leaving it at -4 meant a craft could be silently reverted to
	 * an IDR cost 9x higher than the shipped default.  Inert on CV610,
	 * which does not read the field. */
	cfg->video0.qp_delta = -12;

	/* outgoing */
	cfg->outgoing.enabled = false;
	cfg->outgoing.server[0] = '\0';
	safe_strcpy(cfg->outgoing.stream_mode, sizeof(cfg->outgoing.stream_mode), "rtp");
	cfg->outgoing.max_payload_size = 1400;
	cfg->outgoing.connected_udp = true;
	cfg->outgoing.allow_unix_encoder_stall = false;

	/* fpv.
	 *
	 * The pair ships coherent: OFF, but carrying a delta that does something
	 * the moment it is switched on.  The old default was the other way round
	 * -- roiEnabled:true with roiQp:0 -- which reads as "ROI is on" while
	 * apply_roi_qp() clears every region, because a zero delta is not a
	 * region worth programming.  Nothing was ignored, but the flag and the
	 * log disagreed, and that is what an operator sees first. */
	cfg->fpv.roi_enabled = false;
	cfg->fpv.roi_qp = -20;
	cfg->fpv.roi_steps = 2;
	cfg->fpv.roi_center = 0.4;
	cfg->fpv.noise_level = 0;

	/* audio */
	cfg->audio.enabled = false;
	cfg->audio.sample_rate = 48000;
	cfg->audio.channels = 1;
	safe_strcpy(cfg->audio.codec, sizeof(cfg->audio.codec), "opus");
	cfg->audio.volume = 80;
	cfg->audio.mute = false;
	cfg->outgoing.audio_port = 5601;
	cfg->outgoing.sidecar_port = 5602;

	/* imu */
	cfg->imu.enabled = false;
	safe_strcpy(cfg->imu.i2c_device, sizeof(cfg->imu.i2c_device), "/dev/i2c-1");
	cfg->imu.i2c_addr = 0x68;
	cfg->imu.sample_rate_hz = 200;
	cfg->imu.gyro_range_dps = 1000;
	safe_strcpy(cfg->imu.cal_file, sizeof(cfg->imu.cal_file), "/etc/imu.cal");
	cfg->imu.cal_samples = 400;

	/* record */
	cfg->record.enabled = false;
	safe_strcpy(cfg->record.dir, sizeof(cfg->record.dir), RECORDER_DEFAULT_DIR);
	safe_strcpy(cfg->record.format, sizeof(cfg->record.format), "ts");
	safe_strcpy(cfg->record.mode, sizeof(cfg->record.mode), "mirror");
	cfg->record.max_seconds = 300;
	cfg->record.max_mb = 500;
	cfg->record.bitrate = 0;
	cfg->record.fps = 0;
	cfg->record.gop_size = 0;
	cfg->record.server[0] = '\0';

	/* scene detection (video0) */
	cfg->video0.scene_threshold = 0;   /* 0 = off */
	cfg->video0.scene_holdoff = 2;

	/* H.265 multi-slice split (video0, all encoder backends) — 1 = off */
	cfg->video0.slice_count = 1;

	/* Adaptive frame gate (video0) — off by default; 0 selects the
	 * frame_gate.h defaults for both knobs. */
	cfg->video0.frame_gate_close_slots = 0;
	cfg->video0.frame_gate_max_closed_ms = 0;

	/* intra refresh (video0) — disabled by default; mode-driven */
	safe_strcpy(cfg->video0.intra_refresh_mode,
		sizeof(cfg->video0.intra_refresh_mode), "off");
	cfg->video0.intra_refresh_lines = 0;
	cfg->video0.intra_refresh_qp = 0;

	/* SVC-T reference (video0) — disabled by default; opt-in via refBase>0 */
	cfg->video0.ref_base = 0;
	cfg->video0.ref_enhance = 0;
	cfg->video0.ref_pred = true;

	/* Resilience preset (video0) — off = granular fields drive */
	safe_strcpy(cfg->video0.resilience,
		sizeof(cfg->video0.resilience), "off");

	/* pan centre (video0) — live; zoom_pct is derived from the framing
	 * preset below */
	cfg->video0.zoom_x = 0.5;
	cfg->video0.zoom_y = 0.5;

	/* framing mode (video0) — off by default; expands zoom_pct + stab */
	safe_strcpy(cfg->video0.framing, sizeof(cfg->video0.framing), "off");
	(void)venc_config_apply_framing_preset("off", &cfg->video0);

	/* Shift_Detector accuracy — "auto" resolves per-backend (high on Star6E,
	 * low on Maruko); an unset field thus preserves each backend's default. */
	safe_strcpy(cfg->video0.stab_accuracy,
		sizeof(cfg->video0.stab_accuracy), "auto");

	/* Runtime-only "stab-fill" bypass — always boots false; not parsed or
	 * serialized (a fresh stab-fill run always comes up composing). */
	cfg->video0.pause_stab = false;

	/* discovery — mDNS device beacon (_waybeam-venc._tcp) */
	cfg->discovery.enabled = true;
	safe_strcpy(cfg->discovery.service_type,
		sizeof(cfg->discovery.service_type), "_waybeam-venc._tcp");
	cfg->discovery.name[0] = '\0';
	cfg->discovery.bare_alias = true;

	/* snapshot — MJPEG /api/v1/snapshot.jpg endpoint.  Defaults inherit
	 * main-stream dimensions (width=0/height=0) so a fresh config gets a
	 * snapshot at the same resolution as the live stream. */
	cfg->snapshot.enabled = true;
	cfg->snapshot.quality = 80;
	cfg->snapshot.channel = 7;
	cfg->snapshot.width   = 0;
	cfg->snapshot.height  = 0;

	/* debug */
	cfg->debug.show_osd = false;

	/* attitude (sidecar trailer export; requires imu.enabled) */
	cfg->attitude.enabled      = false;
	cfg->attitude.mount_deg    = 0;
	cfg->attitude.invert_roll  = false;
	cfg->attitude.invert_pitch = false;
	safe_strcpy(cfg->attitude.axis_fwd,
		sizeof(cfg->attitude.axis_fwd), "+x");
	safe_strcpy(cfg->attitude.axis_down,
		sizeof(cfg->attitude.axis_down), "+z");
	cfg->attitude.trim_roll_deg  = 0.0f;
	cfg->attitude.trim_pitch_deg = 0.0f;

	/* detect (IPU NPU object detection; Star6E + Maruko) */
	cfg->detect.enabled = false;
	safe_strcpy(cfg->detect.plugin, sizeof(cfg->detect.plugin),
		"/root/libwaybeam_detect.so");
	cfg->detect.model_path[0]    = '\0';
	cfg->detect.firmware_path[0] = '\0';
	cfg->detect.infer_interval   = 1;
	cfg->detect.osd              = true;
	cfg->detect.conf_thresh      = 0.0f;   /* 0 -> plugin default */
	cfg->detect.nms_iou          = 0.0f;
	cfg->detect.net_width        = 640;
	cfg->detect.net_height       = 352;
	cfg->detect.model_id         = 0;      /* VisDrone-10 */

	/* qr (overlay-free luma tap on VPE port1; Star6E) */
	cfg->qr.tap_enabled = false;
	cfg->qr.tap_width   = 0;   /* 0 -> inherit main stream */
	cfg->qr.tap_height  = 0;
	cfg->qr.window_ms   = 15000;
}

/* ── Load from JSON file ─────────────────────────────────────────────── */

static char *read_file(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;

	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	if (sz <= 0) {
		fclose(f);
		return NULL;
	}
	fseek(f, 0, SEEK_SET);

	char *buf = (char *)malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}

	size_t n = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[n] = '\0';
	if (out_len)
		*out_len = n;
	return buf;
}

static void load_system(const cJSON *root, VencConfigSystem *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "system");
	if (!obj) return;
	s->web_port = (uint16_t)json_get_int(obj, "webPort", s->web_port);
	s->overclock_level = json_get_int(obj, "overclockLevel", s->overclock_level);
	if (s->overclock_level < 0) s->overclock_level = 0;
	if (s->overclock_level > 2) s->overclock_level = 2;
	s->verbose = json_get_bool(obj, "verbose", s->verbose);
}

static void load_sensor(const cJSON *root, VencConfigSensor *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "sensor");
	if (!obj) return;
	s->index = json_get_int(obj, "index", s->index);
	s->mode = json_get_int(obj, "mode", s->mode);
	/* unlock* keys retired in 0.10.13 — legacy IMX415 high-FPS hook;
	 * silently ignored on read so existing configs migrate cleanly. */
}

/* ae_engine validation.  "sdk" is the only supported value: the SDK
 * firmware/bin AE drives convergence, with a supervisory limit enforcer beside
 * it (Star6E) or the paced-native 3A (Maruko).  The historical "custom"
 * userspace governor was retired (Maruko 0.22.0, Star6E 0.47.0) and the
 * "custom" value removed entirely.
 *
 * Returns 0 for "sdk" (or empty → "sdk"), -1 otherwise (caller warns and
 * falls back to "sdk", so old configs carrying "custom" still load). */
static int apply_ae_engine(const char *name, VencConfigIsp *s)
{
	const char *want = (!name || !*name) ? "sdk" : name;
	if (strcmp(want, "sdk") == 0) {
		safe_strcpy(s->ae_engine, sizeof(s->ae_engine), "sdk");
		return 0;
	}
	return -1;
}

static void load_isp(const cJSON *root, VencConfigIsp *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "isp");
	if (!obj) return;
	safe_strcpy(s->sensor_bin, sizeof(s->sensor_bin),
		json_get_string(obj, "sensorBin", s->sensor_bin));
	/* ae_engine is "sdk"-only.  Retired `legacyAe` / `aeMode` / the "custom"
	 * value are dropped on load — existing configs migrate to "sdk". */
	{
		const char *engine = json_get_string(obj, "aeEngine",
			s->ae_engine);
		if (apply_ae_engine(engine, s) != 0) {
			fprintf(stderr, "[config] WARNING: unsupported isp.aeEngine "
				"'%s' (only 'sdk' is supported) — using sdk\n",
				engine);
			(void)apply_ae_engine("sdk", s);
		}
	}
	s->ae_fps = (uint32_t)json_get_int(obj, "aeFps", (int)s->ae_fps);
	s->awb_fps = (uint32_t)json_get_int(obj, "awbFps", (int)s->awb_fps);
	s->gain_max = (uint32_t)json_get_int(obj, "gainMax", (int)s->gain_max);
	s->shutter_max_us = (uint32_t)json_get_int(obj, "shutterMaxUs",
		(int)s->shutter_max_us);
	s->gain_min = (uint32_t)json_get_int(obj, "gainMin", (int)s->gain_min);
	s->shutter_min_us = (uint32_t)json_get_int(obj, "shutterMinUs",
		(int)s->shutter_min_us);
	safe_strcpy(s->awb_mode, sizeof(s->awb_mode),
		json_get_string(obj, "awbMode", s->awb_mode));
	s->awb_ct = (uint32_t)json_get_int(obj, "awbCt", (int)s->awb_ct);
	s->keep_aspect = json_get_bool(obj, "keepAspect", s->keep_aspect);
	s->shutter_rule_180 = json_get_bool(obj, "shutterRule180",
		s->shutter_rule_180);
}

static void load_image(const cJSON *root, VencConfigImage *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "image");
	if (!obj) return;
	s->mirror = json_get_bool(obj, "mirror", s->mirror);
	s->flip = json_get_bool(obj, "flip", s->flip);
	s->rotate = json_get_int(obj, "rotate", s->rotate);
	/* rotate is a WRITE-ONLY convenience: it decomposes into mirror+flip and
	 * is then cleared, in both branches.  Leaving 180 in the struct made the
	 * decomposition re-run on every parse and win permanently -- and since
	 * venc_config_save() writes the field back, a craft that once had
	 * rotate:180 could never set image.mirror=false again.  The write was
	 * accepted, persisted, reported reinit_pending, read back false, and then
	 * silently reverted across the restart it had just asked for. */
	if (s->rotate == 180) {
		s->mirror = true;
		s->flip = true;
	}
	s->rotate = 0;
}

static int parse_resolution(const char *str, uint32_t *w, uint32_t *h)
{
	/* Accept "auto", "WxH", "720p", "1080p" */
	if (!strcmp(str, "auto")) {
		*w = 0; *h = 0;
		return 0;
	}
	if (!strcmp(str, "720p")) {
		*w = 1280; *h = 720;
		return 0;
	}
	if (!strcmp(str, "1080p")) {
		*w = 1920; *h = 1080;
		return 0;
	}
	if (sscanf(str, "%ux%u", w, h) == 2)
		return 0;
	return -1;
}

/* Resilience preset → field expansion.
 *
 * The 2x2 matrix is:                                              .
 *                  | Best efficiency      | Most resilient        .
 *   ---------------+----------------------+------------------     .
 *   Fast recovery  | racing               | fpv (drone FPV)       .
 *   Slow recovery  | quality              | range                 .
 *
 * Named presets set intra_refresh_*, ref_*, AND gop_size, so picking a
 * preset fully determines all recovery / GOP behaviour.  Only `off`
 * leaves gop_size untouched — that mode is the escape hatch where the
 * user's `gopSize` field drives.
 *
 * Returns 0 on success ("off" or recognised preset), or -1 if `name`
 * is unrecognised (caller should warn and fall back to "off"). */
int venc_config_apply_resilience_preset(const char *name, VencConfigVideo *v)
{
	struct preset {
		const char *name;
		const char *intra;
		uint8_t ref_base;
		uint8_t ref_enhance;
		double gop_sec;           /* 0 = preserve caller's gop_size */
	};
	/* Resilience preset table.
	 *
	 * Stripe-only recovery (no IDR needed) requires the effective
	 * wavefront to fit inside the GOP.  Effective wavefront is
	 *   nominal_wavefront × (ref_enhance + 1)
	 * because the TRAIL_N rewrite drops `ref_enhance` of every
	 * `(ref_enhance + 1)` frames from the decoder's reference list.
	 *
	 * OSD-safe column: presets with `ref_enhance > 0` (SVC-T) leave
	 * persistent chroma artefacts in static high-contrast overlays
	 * (OSD text).  Confirmed by bench testing: even `rally` (1:1
	 * SVC-T) shows green smear over the OSD area that won't clear
	 * via stripes — only via IDR.  Root cause: chroma stays in
	 * skip mode for static-content MBs, and intra-refresh stripes
	 * landing in TRAIL_N frames don't propagate the chroma fix
	 * into the DPB.  SDK exposes no force-intra-MB knob to fix it
	 * (ROI is delta-QP only, doesn't override skip-mode for
	 * zero-residual blocks).  See README.md for full discussion.
	 *
	 *   preset      nominal   enh  GOP    eff.wave  OSD-safe?
	 *   ─────────────────────────────────────────────────────
	 *   off         off       0    user   -         yes (no refresh)
	 *   rescue      off       0    0.25s  -         yes (IDR-spam, lowest latency)
	 *   quality     off       0    4.0s   -         yes (IDR-based)
	 *   sprint      150ms     0    0.5s   150ms     yes (intra+short GOP)
	 *   racing      150ms     0    2.0s   150ms     yes
	 *   endurance   500ms     0    2.0s   500ms     yes
	 *   patrol      500ms     0    4.0s   500ms     yes
	 *   rally       150ms     1    2.0s   300ms     no  (light refPred)
	 *   range       500ms     4    2.0s   2500ms    no  (heavy refPred)
	 *   fpv        1000ms     4    2.0s   5000ms    no  (heaviest refPred)
	 *
	 * The "ltr" family is handled below the table, not in it: its
	 * enhance ratio is derived from the GOP (which the user owns) and
	 * so cannot be a static column.  See the block above the loop.
	 */
	static const struct preset table[] = {
		{ "off",        "off",      0, 0, 0.0 },   /* gopSize honoured */
		{ "rescue",     "off",      0, 0, 0.25 },  /* IDR-spam, ~35% bitrate to IDRs */
		{ "quality",    "off",      0, 0, 4.0 },
		{ "sprint",     "fast",     0, 0, 0.5 },   /* intra-refresh + aggressive IDR */
		{ "racing",     "fast",     0, 0, 2.0 },
		{ "endurance",  "balanced", 0, 0, 2.0 },
		{ "patrol",     "balanced", 0, 0, 4.0 },
		{ "rally",      "fast",     1, 1, 2.0 },
		{ "range",      "balanced", 1, 4, 2.0 },
		{ "fpv",        "robust",   1, 4, 2.0 },
	};

	const char *want = (!name || !*name) ? "off" : name;

	/* "ltr" / "ltr:<N>" — maximum non-reference density, long GOP.
	 *
	 * Device-measured on Star6E 2026-08-06 (raw-ES NAL census, see
	 * HISTORY 0.63.0).  MI_VENC_ParamRef_t.u32Enhance is a PERIOD: the
	 * encoder emits exactly one non-referenced frame in every
	 * (u32Enhance + 1).  enhance=4 yields `IRRRnRRRRn…` (20 % droppable),
	 * enhance=299 yields 0.3 %.  So enhance=1 — `InRnRnRn…`, 50 %
	 * droppable — is the most resilient structure this SoC can express,
	 * and it is what bare "ltr" selects.  "ltr:<N>" pins the period for
	 * sweeps; larger N is strictly LESS resilient.
	 *
	 * A lost non-referenced frame costs exactly one frame.  The other
	 * half of the stream is an ordinary P-chain, so losing one of those
	 * still cascades to the next IDR — there is no long-term-reference
	 * API on Infinity6E to do better (the full SDK's MI_VENC_Set*
	 * surface has no LTR/SmartP/GOP-mode entry point).
	 *
	 * Unlike "rally" (the same 1:1 ratio) this preserves the caller's
	 * gop_size and forces intra-refresh off, so it can be paired with a
	 * long GOP and asymmetric transport FEC.  Stripes landing in
	 * non-referenced frames never enter the DPB, so with half the stream
	 * non-referenced they cost bitrate and repair nothing.
	 */
	if (strncmp(want, "ltr", 3) == 0 && (want[3] == '\0' || want[3] == ':')) {
		unsigned long enh = 1ul;

		if (want[3] == ':') {
			char *end = NULL;

			errno = 0;
			enh = strtoul(want + 4, &end, 10);
			if (errno != 0 || end == want + 4 || *end != '\0' ||
			    enh < 1ul || enh > 255ul)
				return -1;
		}
		safe_strcpy(v->intra_refresh_mode,
			sizeof(v->intra_refresh_mode), "off");
		v->intra_refresh_lines = 0;
		v->intra_refresh_qp = 0;
		v->ref_base = 1;
		v->ref_enhance = (uint8_t)enh;
		v->ref_pred = false;
		return 0;
	}

	for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); ++i) {
		if (strcmp(want, table[i].name) != 0)
			continue;
		safe_strcpy(v->intra_refresh_mode,
			sizeof(v->intra_refresh_mode), table[i].intra);
		v->intra_refresh_lines = 0;
		v->intra_refresh_qp = 0;
		v->ref_base = table[i].ref_base;
		v->ref_enhance = table[i].ref_enhance;
		v->ref_pred = true;
		if (table[i].gop_sec > 0.0)
			v->gop_size = table[i].gop_sec;
		return 0;
	}
	return -1;
}

int venc_config_apply_framing_preset(const char *name, VencConfigVideo *v)
{
	struct framing_preset {
		const char *name;
		uint32_t crop_pct;  /* stab kept fraction; 0 = no stab */
		uint32_t recenter;  /* pauseStab glide rate (frames); 0 = default */
		double zoom_pct;    /* Approach-C zoom crop fraction; 0 = no zoom */
		double kalman_q;    /* stab Kalman process noise / pan response */
		double kalman_r;    /* stab Kalman measurement noise / smoothness */
	};
	/* Framing preset table.  A preset is EITHER the stabilization preset
	 * ("stab": crop_pct/recenter set, zoom_pct 0) OR a zoom preset (zoom_pct
	 * set, stab 0) OR off (all 0) — never both.
	 *
	 * Stabilization is a SINGLE tuned mode ("stab"): 80% crop (±192/±108 dead
	 * border @1080p), recenter tau 180 frames, plus the EMA output low-pass in
	 * star6e_pipeline.c.  This is the on-device sweet spot — the earlier
	 * low/medium/high presets traded border vs magnification and only the
	 * middle one felt right, so they were collapsed into one.  Unknown values
	 * (incl. the never-shipped low/medium/high) fall back to "off" on load.
	 * Both backends (Star6E via VPE crop, Maruko via SCL crop —
	 * src/maruko_framing_stab.c).  Source auto-clamps to <=1920x1080.
	 *
	 * Zoom presets — zoom_pct = 1 / magnification (e.g. 2x -> 0.50).  Both
	 * backends; zoom_x/zoom_y pan live (stab is always centered).
	 *
	 *   preset       cropPct  tau   zoom_pct   effect
	 *   ──────────────────────────────────────────────────────────────
	 *   off          0        0     0.00       full image
	 *   stab         80       180   0.00       image stabilization (crop+shrink)
	 *   stab-fill    80       180   0.00       floating image + black border
	 *   zoom-1.25x   0        0     0.80       1.25x digital zoom
	 *   zoom-1.50x   0        0     0.6667     1.50x digital zoom
	 *   zoom-1.75x   0        0     0.5714     1.75x digital zoom
	 *   zoom-2x      0        0     0.50       2x digital zoom
	 *   zoom-3x      0        0     0.3333     3x digital zoom
	 *   zoom-4x      0        0     0.25       4x digital zoom
	 *
	 * Approach-C zoom never upscales (crop dim == SCL output dim), so the
	 * ch1 ~2x SCL upscale ceiling does NOT bound it; 3x/4x simply emit a
	 * smaller frame (1080p: 3x->640x352, 4x->480x256, both >=256 floor).
	 */
	static const struct framing_preset table[] = {
		{ "off",        0,  0,    0.0,    0.0,  0.0 },
		{ "stab",       80, 180,  0.0,    0.03, 2.0 },
		{ "stab-fill",  80, 180,  0.0,    0.03, 2.0 },
		{ "zoom-1.25x", 0,  0,    0.80,   0.0,  0.0 },
		{ "zoom-1.50x", 0,  0,    0.6667, 0.0,  0.0 },
		{ "zoom-1.75x", 0,  0,    0.5714, 0.0,  0.0 },
		{ "zoom-2x",    0,  0,    0.50,   0.0,  0.0 },
		{ "zoom-3x",    0,  0,    0.3333, 0.0,  0.0 },
		{ "zoom-4x",    0,  0,    0.25,   0.0,  0.0 },
	};

	const char *want = (!name || !*name) ? "off" : name;
	for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); ++i) {
		if (strcmp(want, table[i].name) != 0)
			continue;
		v->stab_crop_pct = table[i].crop_pct;
		v->stab_recenter_speed = table[i].recenter;
		v->zoom_pct = table[i].zoom_pct;
		v->stab_kalman_q = table[i].kalman_q;
		v->stab_kalman_r = table[i].kalman_r;
		return 0;
	}
	return -1;
}

static void load_video0(const cJSON *root, VencConfigVideo *v)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "video0");
	if (!obj) return;

	/* "codec" is silently ignored — H.265 is hardcoded.  Existing
	 * configs that set codec="h264" load cleanly; the value is
	 * dropped and HEVC is used. */
	safe_strcpy(v->rc_mode, sizeof(v->rc_mode),
		json_get_string(obj, "rcMode", v->rc_mode));
	v->fps = (uint32_t)json_get_int(obj, "fps", (int)v->fps);

	const char *size = json_get_string(obj, "size", NULL);
	if (size) {
		uint32_t w, h;
		if (parse_resolution(size, &w, &h) == 0) {
			v->width = w;
			v->height = h;
		} else {
			fprintf(stderr, "[venc_config] WARNING: invalid video0.size '%s', "
				"keeping %ux%u\n", size, v->width, v->height);
		}
	}
	/* Also accept separate width/height fields (override size if both present) */
	v->width = (uint32_t)json_get_int(obj, "width", (int)v->width);
	v->height = (uint32_t)json_get_int(obj, "height", (int)v->height);

	v->bitrate = (uint32_t)json_get_int(obj, "bitrate", (int)v->bitrate);
	v->gop_size = json_get_double(obj, "gopSize", v->gop_size);
	v->qp_delta = json_get_int(obj, "qpDelta", v->qp_delta);
	if (v->qp_delta < -12) v->qp_delta = -12;
	if (v->qp_delta > 12) v->qp_delta = 12;
	v->min_qp = (uint32_t)json_get_int(obj, "minQp", (int)v->min_qp);
	v->max_qp = (uint32_t)json_get_int(obj, "maxQp", (int)v->max_qp);
	v->scene_threshold = (uint16_t)json_get_int(obj, "sceneThreshold",
		(int)v->scene_threshold);
	v->scene_holdoff = (uint8_t)json_get_int(obj, "sceneHoldoff",
		(int)v->scene_holdoff);
	if (v->scene_holdoff < 1 && v->scene_threshold > 0) v->scene_holdoff = 1;
	{
		/* Clamp in signed space: a negative value must floor to 1
		 * (single slice), not wrap through uint32 to the maximum. */
		int sc = json_get_int(obj, "sliceCount", (int)v->slice_count);
		if (sc < 1) sc = 1;
		if (sc > VENC_SLICE_COUNT_MAX) sc = VENC_SLICE_COUNT_MAX;
		v->slice_count = (uint32_t)sc;
	}

	/* Adaptive frame gate thresholds.  The gate itself has no switch — it
	 * arms on any frame-shm transport; these only tune it. */
	{
		/* Clamp in signed space so a negative cannot wrap to a huge
		 * unsigned; 0 means "use the frame_gate.h default". */
		int cs = json_get_int(obj, "frameGateCloseSlots",
			(int)v->frame_gate_close_slots);
		if (cs < 0) cs = 0;
		v->frame_gate_close_slots = (uint32_t)cs;

		int mc = json_get_int(obj, "frameGateMaxClosedMs",
			(int)v->frame_gate_max_closed_ms);
		if (mc < 0) mc = 0;
		v->frame_gate_max_closed_ms = (uint32_t)mc;
	}

	/* Resilience preset is the sole driver of intra-refresh + SVC-T
	 * (refPred).  For named presets it also overrides gop_size; only
	 * "off" preserves the user's gopSize value above.  Unknown values
	 * fall back to "off". */
	{
		const char *rname = json_get_string(obj, "resilience",
			v->resilience);
		safe_strcpy(v->resilience, sizeof(v->resilience), rname);
		if (venc_config_apply_resilience_preset(v->resilience, v) != 0) {
			fprintf(stderr, "[config] WARNING: unknown video0.resilience "
				"'%s' (use off|quality|racing|range|fpv|ltr[:N]) — falling "
				"back to off\n", v->resilience);
			safe_strcpy(v->resilience, sizeof(v->resilience), "off");
			(void)venc_config_apply_resilience_preset("off", v);
		}
	}
	/* Per-field override on top of the preset, applied AFTER it because the
	 * preset zeroes this field; non-zero wins (see include/intra_refresh.h).
	 * This is the intra-refresh stripe QP — the quality of the GDR recovery
	 * anchor, and on CV610 the only control that moves I-frame size at all.
	 *
	 * Deliberately NOT paired with an intraRefreshLines override: `lines` is
	 * derived from the mode's target self-heal window (fast 150 ms /
	 * balanced 500 ms / robust 1000 ms) and also drives the auto-GOP, so a
	 * config could silently contradict the resilience name it declares.
	 * Timing belongs to the preset; anchor cost is the tunable axis. */
	{
		int irqp = json_get_int(obj, "intraRefreshQp",
			(int)v->intra_refresh_qp);
		if (irqp < 0)
			irqp = 0;
		if (irqp > 51)
			irqp = 51;
		v->intra_refresh_qp = (uint8_t)irqp;
	}

	/* zoom_pct is derived from the framing preset (below) — no longer
	 * parsed from JSON.  Only the live pan centre is read here. */
	v->zoom_x   = json_get_double(obj, "zoomX",   v->zoom_x);
	v->zoom_y   = json_get_double(obj, "zoomY",   v->zoom_y);
	if (v->zoom_x < 0.0)
		v->zoom_x = 0.0;
	if (v->zoom_x > 1.0)
		v->zoom_x = 1.0;
	if (v->zoom_y < 0.0)
		v->zoom_y = 0.0;
	if (v->zoom_y > 1.0)
		v->zoom_y = 1.0;

	/* Framing preset drives zoom_pct (zoom presets) and the BASE
	 * stab_crop_pct + stab_recenter_speed ("stab").  The two stab fields may
	 * then be overridden explicitly below for advanced tuning.  Unknown
	 * framing values fall back to "off". */
	{
		const char *fname = json_get_string(obj, "framing", v->framing);
		safe_strcpy(v->framing, sizeof(v->framing), fname);
		if (venc_config_apply_framing_preset(v->framing, v) != 0) {
			fprintf(stderr, "[config] WARNING: unknown video0.framing "
				"preset '%s', falling back to off\n", v->framing);
			safe_strcpy(v->framing, sizeof(v->framing), "off");
			(void)venc_config_apply_framing_preset("off", v);
		}
		/* Advanced overrides of the stab presets' derived feel knobs.  Read
		 * AFTER the preset expansion so an explicit value wins; absent keys
		 * keep the preset default (so a plain framing="stab" still gets
		 * 80/180).  Honored ONLY under the stab presets — under off/zoom the
		 * preset's cleared 0/0 must stand, else a stale stabCropPct left over
		 * from a prior stab session silently re-enables stabilization at
		 * framing=off (star6e_stab_*_enabled() key on stab_crop_pct).  Both
		 * "stab" and "stab-fill" share these: stabCropPct is the crop+shrink
		 * border for "stab" and the max-shift / black-border budget for
		 * "stab-fill". */
		if (strcmp(v->framing, "stab") == 0 ||
		    strcmp(v->framing, "stab-fill") == 0) {
			v->stab_crop_pct = (uint32_t)json_get_int(obj, "stabCropPct",
				(int)v->stab_crop_pct);
			v->stab_recenter_speed = (uint32_t)json_get_int(obj,
				"stabRecenterSpeed", (int)v->stab_recenter_speed);
			v->stab_kalman_q = json_get_double(obj,
				"stabKalmanQ", v->stab_kalman_q);
			v->stab_kalman_r = json_get_double(obj,
				"stabKalmanR", v->stab_kalman_r);
			/* Detector accuracy level (shared table in
			 * framing_stab_accuracy.h).  Lenient on load — an
			 * unrecognised hand-edited value falls back to "auto"
			 * (the resolver also treats unknown as auto). */
			{
				const char *acc = json_get_string(obj,
					"stabAccuracy", v->stab_accuracy);
				if (!framing_stab_accuracy_valid(acc))
					acc = "auto";
				safe_strcpy(v->stab_accuracy,
					sizeof(v->stab_accuracy), acc);
			}
			/* Harden stab_crop_pct to [60, 100] for the active stab preset.
			 * A stab preset must have a usable crop budget; clamping here
			 * also self-heals a stale stabCropPct (e.g. a 0 saved while
			 * framing=off, then framing re-set via json_cli string-only)
			 * that would otherwise override the preset's 80 down to 0 and
			 * silently disable stabilization (star6e_stab_*_enabled() key on
			 * stab_crop_pct >= 50 → fall through to the unstabilized bind). */
			if (v->stab_crop_pct < 60)
				v->stab_crop_pct = 60;
			if (v->stab_crop_pct > 100)
				v->stab_crop_pct = 100;
		}
	}
}

static void load_outgoing(const cJSON *root, VencConfigOutgoing *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "outgoing");
	if (!obj) return;
	s->enabled = json_get_bool(obj, "enabled", s->enabled);
	safe_strcpy(s->server, sizeof(s->server),
		json_get_string(obj, "server", s->server));
	safe_strcpy(s->stream_mode, sizeof(s->stream_mode),
		json_get_string(obj, "streamMode", s->stream_mode));
	s->max_payload_size = (uint16_t)json_get_int(obj, "maxPayloadSize",
		(int)s->max_payload_size);
	s->connected_udp = json_get_bool(obj, "connectedUdp", s->connected_udp);
	s->allow_unix_encoder_stall = json_get_bool(obj, "allowUnixEncoderStall",
		s->allow_unix_encoder_stall);
	s->audio_port = json_get_int(obj, "audioPort", s->audio_port);
	s->sidecar_port = (uint16_t)json_get_int(obj, "sidecarPort",
		(int)s->sidecar_port);
}

static void load_discovery(const cJSON *root, VencConfigDiscovery *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "discovery");
	if (!obj) return;
	s->enabled = json_get_bool(obj, "enabled", s->enabled);
	safe_strcpy(s->service_type, sizeof(s->service_type),
		json_get_string(obj, "serviceType", s->service_type));
	safe_strcpy(s->name, sizeof(s->name),
		json_get_string(obj, "name", s->name));
	s->bare_alias = json_get_bool(obj, "bareAlias", s->bare_alias);
}

static void load_audio(const cJSON *root, VencConfigAudio *a)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "audio");
	if (!obj) return;
	a->enabled = json_get_bool(obj, "enabled", a->enabled);
	a->sample_rate = (uint32_t)json_get_int(obj, "sampleRate",
		(int)a->sample_rate);
	if (a->sample_rate < 8000) a->sample_rate = 8000;
	if (a->sample_rate > 48000) a->sample_rate = 48000;
	a->channels = (uint32_t)json_get_int(obj, "channels",
		(int)a->channels);
	if (a->channels < 1) a->channels = 1;
	if (a->channels > 2) a->channels = 2;
	safe_strcpy(a->codec, sizeof(a->codec),
		json_get_string(obj, "codec", a->codec));
	a->volume = json_get_int(obj, "volume", a->volume);
	if (a->volume < 0) a->volume = 0;
	if (a->volume > 100) a->volume = 100;
	a->mute = json_get_bool(obj, "mute", a->mute);
}

static void load_imu(const cJSON *root, VencConfigImu *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "imu");
	if (!obj) return;
	s->enabled = json_get_bool(obj, "enabled", s->enabled);
	safe_strcpy(s->i2c_device, sizeof(s->i2c_device),
		json_get_string(obj, "i2cDevice", s->i2c_device));
	const char *addr_str = json_get_string(obj, "i2cAddr", NULL);
	if (addr_str)
		s->i2c_addr = (uint8_t)strtoul(addr_str, NULL, 0);
	s->sample_rate_hz = json_get_int(obj, "sampleRateHz", s->sample_rate_hz);
	if (s->sample_rate_hz < 25) s->sample_rate_hz = 25;
	if (s->sample_rate_hz > 1600) s->sample_rate_hz = 1600;
	s->gyro_range_dps = json_get_int(obj, "gyroRangeDps", s->gyro_range_dps);
	safe_strcpy(s->cal_file, sizeof(s->cal_file),
		json_get_string(obj, "calFile", s->cal_file));
	s->cal_samples = json_get_int(obj, "calSamples", s->cal_samples);
	if (s->cal_samples < 10) s->cal_samples = 10;
}

static void load_record(const cJSON *root, VencConfigRecord *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "record");
	if (!obj) return;
	s->enabled = json_get_bool(obj, "enabled", s->enabled);
	safe_strcpy(s->dir, sizeof(s->dir),
		json_get_string(obj, "dir", s->dir));
	safe_strcpy(s->format, sizeof(s->format),
		json_get_string(obj, "format", s->format));
	safe_strcpy(s->mode, sizeof(s->mode),
		json_get_string(obj, "mode", s->mode));
	s->max_seconds = (uint32_t)json_get_int(obj, "maxSeconds",
		(int)s->max_seconds);
	s->max_mb = (uint32_t)json_get_int(obj, "maxMB", (int)s->max_mb);
	s->bitrate = (uint32_t)json_get_int(obj, "bitrate", (int)s->bitrate);
	s->fps = (uint32_t)json_get_int(obj, "fps", (int)s->fps);
	s->gop_size = json_get_double(obj, "gopSize", s->gop_size);
	safe_strcpy(s->server, sizeof(s->server),
		json_get_string(obj, "server", s->server));
}

static void load_snapshot(const cJSON *root, VencConfigSnapshot *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "snapshot");
	if (!obj) return;
	s->enabled = json_get_bool(obj, "enabled", s->enabled);
	s->quality = (uint32_t)json_get_int(obj, "quality", (int)s->quality);
	if (s->quality < 1)   s->quality = 1;
	if (s->quality > 100) s->quality = 100;
	s->channel = json_get_int(obj, "channel", s->channel);
	s->width   = (uint32_t)json_get_int(obj, "width",  (int)s->width);
	s->height  = (uint32_t)json_get_int(obj, "height", (int)s->height);
}

static void load_fpv(const cJSON *root, VencConfigFpv *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "fpv");
	if (!obj) return;
	s->roi_enabled = json_get_bool(obj, "roiEnabled", s->roi_enabled);
	s->roi_qp = json_get_int(obj, "roiQp", s->roi_qp);
	/* Clamped here, not rejected: this runs on the FILE parse, and a craft
	 * already carrying the old -30 must still boot.  It lands on the new
	 * bound instead, which is the fix applying itself.  The API path
	 * rejects out-of-range instead (validate_field_cfg), so an operator
	 * typing -30 is told, rather than reading back a value they did not
	 * write. */
	if (s->roi_qp < -20) s->roi_qp = -20;
	if (s->roi_qp > 20) s->roi_qp = 20;
	s->roi_steps = (uint16_t)json_get_int(obj, "roiSteps", (int)s->roi_steps);
	if (s->roi_steps < 1) s->roi_steps = 1;
	if (s->roi_steps > PIPELINE_ROI_MAX_STEPS) s->roi_steps = PIPELINE_ROI_MAX_STEPS;
	s->roi_center = json_get_double(obj, "roiCenter", s->roi_center);
	if (s->roi_center < 0.1) s->roi_center = 0.1;
	if (s->roi_center > 0.9) s->roi_center = 0.9;
	s->noise_level = json_get_int(obj, "noiseLevel", s->noise_level);
	if (s->noise_level < 0) s->noise_level = 0;
	if (s->noise_level > 7) s->noise_level = 7;
}

int venc_config_load(const char *path, VencConfig *cfg)
{
	if (!cfg)
		return -1;

	size_t len = 0;
	char *data = read_file(path, &len);
	if (!data) {
		if (errno == ENOENT) {
			fprintf(stderr, "[config] %s not found, using defaults\n", path);
			return 0;
		}
		fprintf(stderr, "[config] ERROR: cannot read %s: %s\n",
			path, strerror(errno));
		return -1;
	}

	cJSON *root = cJSON_Parse(data);
	if (!root) {
		const char *err = cJSON_GetErrorPtr();
		fprintf(stderr, "[venc_config] ERROR: JSON parse error in %s near: %.30s\n",
			path, err ? err : "(unknown)");
		free(data);
		return -1;
	}
	free(data);

	load_system(root, &cfg->system);
	load_sensor(root, &cfg->sensor);
	load_isp(root, &cfg->isp);
	load_image(root, &cfg->image);
	load_video0(root, &cfg->video0);
	load_outgoing(root, &cfg->outgoing);
	load_discovery(root, &cfg->discovery);
	load_fpv(root, &cfg->fpv);
	load_audio(root, &cfg->audio);
	load_imu(root, &cfg->imu);
	load_record(root, &cfg->record);
	load_snapshot(root, &cfg->snapshot);
	{
		const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "debug");
		if (obj)
			cfg->debug.show_osd = json_get_bool(obj, "showOsd",
				cfg->debug.show_osd);
	}
	{
		const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root,
			"attitude");
		if (obj) {
			cfg->attitude.enabled = json_get_bool(obj, "enabled",
				cfg->attitude.enabled);
			cfg->attitude.mount_deg = json_get_int(obj, "mountDeg",
				cfg->attitude.mount_deg);
			cfg->attitude.invert_roll = json_get_bool(obj,
				"invertRoll", cfg->attitude.invert_roll);
			cfg->attitude.invert_pitch = json_get_bool(obj,
				"invertPitch", cfg->attitude.invert_pitch);
			safe_strcpy(cfg->attitude.axis_fwd,
				sizeof(cfg->attitude.axis_fwd),
				json_get_string(obj, "axisFwd",
					cfg->attitude.axis_fwd));
			safe_strcpy(cfg->attitude.axis_down,
				sizeof(cfg->attitude.axis_down),
				json_get_string(obj, "axisDown",
					cfg->attitude.axis_down));
			cfg->attitude.trim_roll_deg = (float)json_get_double(
				obj, "trimRollDeg",
				cfg->attitude.trim_roll_deg);
			cfg->attitude.trim_pitch_deg = (float)json_get_double(
				obj, "trimPitchDeg",
				cfg->attitude.trim_pitch_deg);
		}
	}
	{
		const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root,
			"detect");
		if (obj) {
			cfg->detect.enabled = json_get_bool(obj, "enabled",
				cfg->detect.enabled);
			safe_strcpy(cfg->detect.plugin,
				sizeof(cfg->detect.plugin),
				json_get_string(obj, "plugin",
					cfg->detect.plugin));
			safe_strcpy(cfg->detect.model_path,
				sizeof(cfg->detect.model_path),
				json_get_string(obj, "modelPath",
					cfg->detect.model_path));
			safe_strcpy(cfg->detect.firmware_path,
				sizeof(cfg->detect.firmware_path),
				json_get_string(obj, "firmwarePath",
					cfg->detect.firmware_path));
			cfg->detect.infer_interval = json_get_int(obj,
				"inferInterval", cfg->detect.infer_interval);
			cfg->detect.osd = json_get_bool(obj, "osd",
				cfg->detect.osd);
			cfg->detect.conf_thresh = (float)json_get_double(obj,
				"confThresh", cfg->detect.conf_thresh);
			cfg->detect.nms_iou = (float)json_get_double(obj,
				"nmsIou", cfg->detect.nms_iou);
			cfg->detect.net_width = (uint32_t)json_get_int(obj,
				"netWidth", (int)cfg->detect.net_width);
			cfg->detect.net_height = (uint32_t)json_get_int(obj,
				"netHeight", (int)cfg->detect.net_height);
			cfg->detect.model_id = (uint32_t)json_get_int(obj,
				"modelId", (int)cfg->detect.model_id);
		}
	}
	{
		const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "qr");
		if (obj) {
			cfg->qr.tap_enabled = json_get_bool(obj, "tapEnabled",
				cfg->qr.tap_enabled);
			cfg->qr.tap_width = (uint32_t)json_get_int(obj,
				"tapWidth", (int)cfg->qr.tap_width);
			cfg->qr.tap_height = (uint32_t)json_get_int(obj,
				"tapHeight", (int)cfg->qr.tap_height);
			cfg->qr.window_ms = (uint32_t)json_get_int(obj,
				"windowMs", (int)cfg->qr.window_ms);
		}
	}

	cJSON_Delete(root);

	{
		const char *err = venc_api_validate_loaded_config(cfg);
		if (err) {
			fprintf(stderr,
				"[venc_config] ERROR: invalid value in %s: %s\n",
				path, err);
			return -1;
		}
	}

	fprintf(stderr, "[venc_config] Loaded config from %s\n", path);
	return 0;
}

/* ── URI parser ──────────────────────────────────────────────────────── */

int venc_config_parse_server_uri(const char *uri, char *host, size_t host_len,
	uint16_t *port)
{
	VencOutputUri parsed;

	if (!uri || !host || !port)
		return -1;
	if (venc_config_parse_output_uri(uri, &parsed) != 0)
		return -1;
	if (parsed.type != VENC_OUTPUT_URI_UDP) {
		fprintf(stderr, "[venc_config] ERROR: URI '%s' is not udp://\n", uri);
		return -1;
	}

	safe_strcpy(host, host_len, parsed.host);
	*port = parsed.port;
	return 0;
}

int venc_config_parse_output_uri(const char *uri, VencOutputUri *out)
{
	const char *p;

	if (!uri || !out)
		return -1;

	memset(out, 0, sizeof(*out));
	p = uri;
	if (strncmp(p, "udp://", 6) == 0) {
		const char *colon;
		char *end = NULL;
		long pval;
		size_t hlen;

		out->type = VENC_OUTPUT_URI_UDP;
		p += 6;
		colon = strrchr(p, ':');
		if (!colon || colon == p) {
			fprintf(stderr, "[venc_config] ERROR: missing host:port in '%s'\n",
				uri);
			return -1;
		}

		hlen = (size_t)(colon - p);
		if (hlen >= sizeof(out->host))
			hlen = sizeof(out->host) - 1;
		memcpy(out->host, p, hlen);
		out->host[hlen] = '\0';

		pval = strtol(colon + 1, &end, 10);
		if (!end || *end != '\0' || pval <= 0 || pval > 65535) {
			fprintf(stderr, "[venc_config] ERROR: invalid port in '%s'\n", uri);
			return -1;
		}
		out->port = (uint16_t)pval;
		return 0;
	}

	if (strncmp(p, "unix://", 7) == 0) {
		out->type = VENC_OUTPUT_URI_UNIX;
		p += 7;
		if (!p[0]) {
			fprintf(stderr, "[venc_config] ERROR: unix:// URI missing socket name\n");
			return -1;
		}
		safe_strcpy(out->endpoint, sizeof(out->endpoint), p);
		return 0;
	}

	if (strncmp(p, "shm://", 6) == 0) {
		out->type = VENC_OUTPUT_URI_SHM;
		p += 6;
		if (!p[0]) {
			fprintf(stderr, "[venc_config] ERROR: shm:// URI missing name\n");
			return -1;
		}
		safe_strcpy(out->endpoint, sizeof(out->endpoint), p);
		return 0;
	}

	if (strncmp(p, "frame-shm://", 12) == 0) {
		out->type = VENC_OUTPUT_URI_FRAME_SHM;
		p += 12;
		if (!p[0]) {
			fprintf(stderr, "[venc_config] ERROR: frame-shm:// URI missing name\n");
			return -1;
		}
		safe_strcpy(out->endpoint, sizeof(out->endpoint), p);
		return 0;
	}

	fprintf(stderr, "[venc_config] ERROR: unsupported URI scheme in '%s' "
		"(expected udp://, unix://, shm://, or frame-shm://)\n", uri);
	return -1;
}

/* ── Hand-rolled pretty printer (used by venc_config_save) ──────────────
 *
 * Produces a deterministic, unified layout for /etc/venc.json so WebUI
 * saves diff cleanly against the canonical default and stay byte-stable
 * across writes.  cJSON_Print's tab-indented one-key-per-line output is
 * not used for disk writes; cJSON is still used for parsing and for
 * single-line HTTP response bodies elsewhere.
 *
 * Layout: 2-space indent, one key per line, ": " separator, no blank
 * lines between sections, single trailing newline at EOF.
 *
 * Adding a config field requires updating the matching render_<section>
 * helper here AND config/venc.default.json.  The byte-equal round-trip
 * test in tests/test_venc_config.c enforces this. */

typedef struct {
	char *buf;
	size_t len;
	size_t cap;
	int oom;
} PrettyBuf;

static void pp_init(PrettyBuf *p)
{
	p->buf = NULL;
	p->len = 0;
	p->cap = 0;
	p->oom = 0;
}

static int pp_reserve(PrettyBuf *p, size_t extra)
{
	if (p->oom)
		return -1;
	size_t need = p->len + extra + 1;
	if (need <= p->cap)
		return 0;
	size_t new_cap = p->cap ? p->cap : 1024;
	while (new_cap < need)
		new_cap *= 2;
	char *nb = realloc(p->buf, new_cap);
	if (!nb) {
		p->oom = 1;
		return -1;
	}
	p->buf = nb;
	p->cap = new_cap;
	return 0;
}

static void pp_raw(PrettyBuf *p, const char *s, size_t n)
{
	if (pp_reserve(p, n) != 0)
		return;
	memcpy(p->buf + p->len, s, n);
	p->len += n;
	p->buf[p->len] = '\0';
}

static void pp_str(PrettyBuf *p, const char *s)
{
	pp_raw(p, s, strlen(s));
}

static void pp_indent(PrettyBuf *p, int depth)
{
	for (int i = 0; i < depth; i++)
		pp_raw(p, "  ", 2);
}

/* JSON-escape a string into the buffer, including surrounding quotes. */
static void pp_string(PrettyBuf *p, const char *s)
{
	pp_raw(p, "\"", 1);
	if (!s) {
		pp_raw(p, "\"", 1);
		return;
	}
	for (const unsigned char *u = (const unsigned char *)s; *u; u++) {
		unsigned char c = *u;
		switch (c) {
		case '"':  pp_raw(p, "\\\"", 2); break;
		case '\\': pp_raw(p, "\\\\", 2); break;
		case '\b': pp_raw(p, "\\b",  2); break;
		case '\f': pp_raw(p, "\\f",  2); break;
		case '\n': pp_raw(p, "\\n",  2); break;
		case '\r': pp_raw(p, "\\r",  2); break;
		case '\t': pp_raw(p, "\\t",  2); break;
		default:
			if (c < 0x20) {
				char esc[8];
				snprintf(esc, sizeof(esc), "\\u%04x", c);
				pp_str(p, esc);
			} else {
				pp_raw(p, (const char *)&c, 1);
			}
			break;
		}
	}
	pp_raw(p, "\"", 1);
}

static void pp_bool(PrettyBuf *p, bool v)
{
	pp_str(p, v ? "true" : "false");
}

static void pp_int(PrettyBuf *p, long long v)
{
	char tmp[32];
	snprintf(tmp, sizeof(tmp), "%lld", v);
	pp_str(p, tmp);
}

static void pp_uint(PrettyBuf *p, unsigned long long v)
{
	char tmp[32];
	snprintf(tmp, sizeof(tmp), "%llu", v);
	pp_str(p, tmp);
}

/* Doubles: integral values print as "N.0" to keep the float hint in the
 * file (matches the hand-authored default's `gopSize: 1.0`).  Fractional
 * values try %1.15g first and fall back to %1.17g if the parse doesn't
 * round-trip — same policy cJSON uses, so existing files load identically.
 * Locale is pinned to C around snprintf so a non-C LC_NUMERIC can't
 * smuggle in a comma.  setlocale is process-global, so any concurrent
 * thread printing floats during venc_config_save sees C locale for the
 * window — harmless on this build (LC_NUMERIC is C by default and venc
 * never calls setlocale elsewhere), but switch to uselocale() if a
 * future thread needs a non-C numeric locale. */
static void pp_double(PrettyBuf *p, double v)
{
	char tmp[40];
	const char *saved_locale = setlocale(LC_NUMERIC, NULL);
	char saved[32];
	saved[0] = '\0';
	if (saved_locale)
		snprintf(saved, sizeof(saved), "%s", saved_locale);
	setlocale(LC_NUMERIC, "C");

	int is_finite = (v == v) && (v - v == 0.0);  /* NaN fails ==, inf fails - */
	int is_integral = is_finite && (v >= -9.2233720368547758e18) &&
		(v <= 9.2233720368547758e18) && ((double)(long long)v == v);

	if (!is_finite) {
		snprintf(tmp, sizeof(tmp), "0.0");
	} else if (is_integral) {
		snprintf(tmp, sizeof(tmp), "%lld.0", (long long)v);
	} else {
		snprintf(tmp, sizeof(tmp), "%1.15g", v);
		double parsed = 0.0;
		if (sscanf(tmp, "%lg", &parsed) != 1 || parsed != v)
			snprintf(tmp, sizeof(tmp), "%1.17g", v);
	}

	if (saved[0])
		setlocale(LC_NUMERIC, saved);
	pp_str(p, tmp);
}

/* Emit `<indent>"key": ` (no trailing newline). */
static void pp_key(PrettyBuf *p, int depth, const char *key)
{
	pp_indent(p, depth);
	pp_string(p, key);
	pp_raw(p, ": ", 2);
}

/* Field emitters: write `  "key": value,\n` (or no comma if is_last). */
static void pp_field_bool(PrettyBuf *p, int depth, const char *key, bool v,
	int is_last)
{
	pp_key(p, depth, key);
	pp_bool(p, v);
	pp_str(p, is_last ? "\n" : ",\n");
}

static void pp_field_int(PrettyBuf *p, int depth, const char *key,
	long long v, int is_last)
{
	pp_key(p, depth, key);
	pp_int(p, v);
	pp_str(p, is_last ? "\n" : ",\n");
}

static void pp_field_uint(PrettyBuf *p, int depth, const char *key,
	unsigned long long v, int is_last)
{
	pp_key(p, depth, key);
	pp_uint(p, v);
	pp_str(p, is_last ? "\n" : ",\n");
}

static void pp_field_double(PrettyBuf *p, int depth, const char *key,
	double v, int is_last)
{
	pp_key(p, depth, key);
	pp_double(p, v);
	pp_str(p, is_last ? "\n" : ",\n");
}

static void pp_field_string(PrettyBuf *p, int depth, const char *key,
	const char *v, int is_last)
{
	pp_key(p, depth, key);
	pp_string(p, v);
	pp_str(p, is_last ? "\n" : ",\n");
}

/* Section open/close: `<indent>"name": {\n` ... `<indent>}` (with comma
 * if not last). */
static void pp_section_open(PrettyBuf *p, int depth, const char *name)
{
	pp_indent(p, depth);
	pp_string(p, name);
	pp_str(p, ": {\n");
}

static void pp_section_close(PrettyBuf *p, int depth, int is_last)
{
	pp_indent(p, depth);
	pp_str(p, is_last ? "}\n" : "},\n");
}

/* ── Per-section renderers (mirror config_to_cjson key order) ──────── */

static void render_system(PrettyBuf *p, const VencConfig *cfg, int is_last)
{
	pp_section_open(p, 1, "system");
	pp_field_uint(p,   2, "webPort",         cfg->system.web_port,         0);
	pp_field_int(p,    2, "overclockLevel",  cfg->system.overclock_level,  0);
	pp_field_bool(p,   2, "verbose",         cfg->system.verbose,          1);
	pp_section_close(p, 1, is_last);
}

static void render_sensor(PrettyBuf *p, const VencConfig *cfg, int is_last)
{
	pp_section_open(p, 1, "sensor");
	pp_field_int(p,    2, "index",          cfg->sensor.index,          0);
	pp_field_int(p,    2, "mode",           cfg->sensor.mode,           1);
	pp_section_close(p, 1, is_last);
}

static void render_isp(PrettyBuf *p, const VencConfig *cfg, int is_last)
{
	pp_section_open(p, 1, "isp");
	pp_field_string(p, 2, "sensorBin",  cfg->isp.sensor_bin,  0);
	pp_field_string(p, 2, "aeEngine",   cfg->isp.ae_engine,   0);
	pp_field_uint(p,   2, "aeFps",      cfg->isp.ae_fps,      0);
	pp_field_uint(p,   2, "awbFps",     cfg->isp.awb_fps,     0);
	pp_field_uint(p,   2, "gainMax",    cfg->isp.gain_max,    0);
	pp_field_uint(p,   2, "shutterMaxUs", cfg->isp.shutter_max_us, 0);
	pp_field_uint(p,   2, "gainMin",    cfg->isp.gain_min,    0);
	pp_field_uint(p,   2, "shutterMinUs", cfg->isp.shutter_min_us, 0);
	pp_field_string(p, 2, "awbMode",    cfg->isp.awb_mode,    0);
	pp_field_uint(p,   2, "awbCt",      cfg->isp.awb_ct,      0);
	pp_field_bool(p,   2, "keepAspect",      cfg->isp.keep_aspect,      0);
	pp_field_bool(p,   2, "shutterRule180",  cfg->isp.shutter_rule_180, 1);
	pp_section_close(p, 1, is_last);
}

static void render_image(PrettyBuf *p, const VencConfig *cfg, int is_last)
{
	pp_section_open(p, 1, "image");
	pp_field_bool(p,   2, "mirror", cfg->image.mirror, 0);
	pp_field_bool(p,   2, "flip",   cfg->image.flip,   0);
	pp_field_int(p,    2, "rotate", cfg->image.rotate, 1);
	pp_section_close(p, 1, is_last);
}

static void render_video0(PrettyBuf *p, const VencConfig *cfg, int is_last)
{
	pp_section_open(p, 1, "video0");
	pp_field_string(p, 2, "rcMode", cfg->video0.rc_mode, 0);
	pp_field_uint(p,   2, "fps",    cfg->video0.fps,    0);
	if (cfg->video0.width > 0 && cfg->video0.height > 0) {
		char size_buf[32];
		snprintf(size_buf, sizeof(size_buf), "%ux%u",
			cfg->video0.width, cfg->video0.height);
		pp_field_string(p, 2, "size", size_buf, 0);
	} else {
		pp_field_string(p, 2, "size", "auto", 0);
	}
	pp_field_uint(p,   2, "bitrate",        cfg->video0.bitrate,         0);
	pp_field_double(p, 2, "gopSize",        cfg->video0.gop_size,        0);
	pp_field_int(p,    2, "qpDelta",        cfg->video0.qp_delta,        0);
	pp_field_uint(p,   2, "minQp",          cfg->video0.min_qp,          0);
	pp_field_uint(p,   2, "maxQp",          cfg->video0.max_qp,          0);
	pp_field_uint(p,   2, "sceneThreshold", cfg->video0.scene_threshold, 0);
	pp_field_uint(p,   2, "sceneHoldoff",   cfg->video0.scene_holdoff,   0);
	pp_field_uint(p,   2, "sliceCount",     cfg->video0.slice_count,     0);
	pp_field_uint(p,   2, "frameGateCloseSlots",
		cfg->video0.frame_gate_close_slots, 0);
	pp_field_uint(p,   2, "frameGateMaxClosedMs",
		cfg->video0.frame_gate_max_closed_ms, 0);
	pp_field_string(p, 2, "resilience",        cfg->video0.resilience,          0);
	pp_field_uint(p,   2, "intraRefreshQp", cfg->video0.intra_refresh_qp, 0);
	pp_field_double(p, 2, "zoomX",             cfg->video0.zoom_x,              0);
	pp_field_double(p, 2, "zoomY",             cfg->video0.zoom_y,              0);
	pp_field_string(p, 2, "framing",           cfg->video0.framing,             0);
	pp_field_uint(p,   2, "stabCropPct",       cfg->video0.stab_crop_pct,       0);
	pp_field_uint(p,   2, "stabRecenterSpeed", cfg->video0.stab_recenter_speed, 0);
	pp_field_double(p, 2, "stabKalmanQ",       cfg->video0.stab_kalman_q,       0);
	pp_field_double(p, 2, "stabKalmanR",       cfg->video0.stab_kalman_r,       1);
	pp_section_close(p, 1, is_last);
}

static void render_outgoing(PrettyBuf *p, const VencConfig *cfg, int is_last)
{
	pp_section_open(p, 1, "outgoing");
	pp_field_bool(p,   2, "enabled",         cfg->outgoing.enabled,           0);
	pp_field_string(p, 2, "server",          cfg->outgoing.server,            0);
	pp_field_string(p, 2, "streamMode",      cfg->outgoing.stream_mode,       0);
	pp_field_uint(p,   2, "maxPayloadSize",  cfg->outgoing.max_payload_size,  0);
	pp_field_bool(p,   2, "connectedUdp",    cfg->outgoing.connected_udp,     0);
	pp_field_bool(p,   2, "allowUnixEncoderStall",
		cfg->outgoing.allow_unix_encoder_stall, 0);
	pp_field_int(p,    2, "audioPort",       cfg->outgoing.audio_port,        0);
	pp_field_uint(p,   2, "sidecarPort",    cfg->outgoing.sidecar_port,    1);
	pp_section_close(p, 1, is_last);
}

static void render_discovery(PrettyBuf *p, const VencConfig *cfg, int is_last)
{
	pp_section_open(p, 1, "discovery");
	pp_field_bool(p,   2, "enabled",     cfg->discovery.enabled,      0);
	pp_field_string(p, 2, "serviceType", cfg->discovery.service_type, 0);
	pp_field_string(p, 2, "name",        cfg->discovery.name,         0);
	pp_field_bool(p,   2, "bareAlias",   cfg->discovery.bare_alias,   1);
	pp_section_close(p, 1, is_last);
}

static void render_fpv(PrettyBuf *p, const VencConfig *cfg, int is_last)
{
	pp_section_open(p, 1, "fpv");
	pp_field_bool(p,   2, "roiEnabled", cfg->fpv.roi_enabled, 0);
	pp_field_int(p,    2, "roiQp",      cfg->fpv.roi_qp,      0);
	pp_field_uint(p,   2, "roiSteps",   cfg->fpv.roi_steps,   0);
	pp_field_double(p, 2, "roiCenter",  cfg->fpv.roi_center,  0);
	pp_field_int(p,    2, "noiseLevel", cfg->fpv.noise_level, 1);
	pp_section_close(p, 1, is_last);
}

static void render_audio(PrettyBuf *p, const VencConfig *cfg, int is_last)
{
	pp_section_open(p, 1, "audio");
	pp_field_bool(p,   2, "enabled",    cfg->audio.enabled,     0);
	pp_field_uint(p,   2, "sampleRate", cfg->audio.sample_rate, 0);
	pp_field_uint(p,   2, "channels",   cfg->audio.channels,    0);
	pp_field_string(p, 2, "codec",      cfg->audio.codec,       0);
	pp_field_int(p,    2, "volume",     cfg->audio.volume,      0);
	pp_field_bool(p,   2, "mute",       cfg->audio.mute,        1);
	pp_section_close(p, 1, is_last);
}

static void render_imu(PrettyBuf *p, const VencConfig *cfg, int is_last)
{
	char addr_buf[8];
	snprintf(addr_buf, sizeof(addr_buf), "0x%02x", cfg->imu.i2c_addr);

	pp_section_open(p, 1, "imu");
	pp_field_bool(p,   2, "enabled",      cfg->imu.enabled,         0);
	pp_field_string(p, 2, "i2cDevice",    cfg->imu.i2c_device,      0);
	pp_field_string(p, 2, "i2cAddr",      addr_buf,                 0);
	pp_field_int(p,    2, "sampleRateHz", cfg->imu.sample_rate_hz,  0);
	pp_field_int(p,    2, "gyroRangeDps", cfg->imu.gyro_range_dps,  0);
	pp_field_string(p, 2, "calFile",      cfg->imu.cal_file,        0);
	pp_field_int(p,    2, "calSamples",   cfg->imu.cal_samples,     1);
	pp_section_close(p, 1, is_last);
}

static void render_record(PrettyBuf *p, const VencConfig *cfg, int is_last)
{
	pp_section_open(p, 1, "record");
	pp_field_bool(p,   2, "enabled",    cfg->record.enabled,     0);
	pp_field_string(p, 2, "dir",        cfg->record.dir,         0);
	pp_field_string(p, 2, "format",     cfg->record.format,      0);
	pp_field_string(p, 2, "mode",       cfg->record.mode,        0);
	pp_field_uint(p,   2, "maxSeconds", cfg->record.max_seconds, 0);
	pp_field_uint(p,   2, "maxMB",      cfg->record.max_mb,      0);
	pp_field_uint(p,   2, "bitrate",    cfg->record.bitrate,     0);
	pp_field_uint(p,   2, "fps",        cfg->record.fps,         0);
	pp_field_double(p, 2, "gopSize",    cfg->record.gop_size,    0);
	pp_field_string(p, 2, "server",     cfg->record.server,      1);
	pp_section_close(p, 1, is_last);
}

static void render_snapshot(PrettyBuf *p, const VencConfig *cfg, int is_last)
{
	pp_section_open(p, 1, "snapshot");
	pp_field_bool(p, 2, "enabled", cfg->snapshot.enabled,         0);
	pp_field_uint(p, 2, "quality", cfg->snapshot.quality,         0);
	pp_field_int(p,  2, "channel", cfg->snapshot.channel,         0);
	pp_field_uint(p, 2, "width",   cfg->snapshot.width,           0);
	pp_field_uint(p, 2, "height",  cfg->snapshot.height,          1);
	pp_section_close(p, 1, is_last);
}

static void render_debug(PrettyBuf *p, const VencConfig *cfg, int is_last)
{
	pp_section_open(p, 1, "debug");
	pp_field_bool(p,   2, "showOsd", cfg->debug.show_osd, 1);
	pp_section_close(p, 1, is_last);
}

static void render_attitude(PrettyBuf *p, const VencConfig *cfg, int is_last)
{
	pp_section_open(p, 1, "attitude");
	pp_field_bool(p, 2, "enabled",     cfg->attitude.enabled,      0);
	pp_field_int(p,  2, "mountDeg",    cfg->attitude.mount_deg,    0);
	pp_field_bool(p, 2, "invertRoll",  cfg->attitude.invert_roll,  0);
	pp_field_bool(p, 2, "invertPitch", cfg->attitude.invert_pitch, 0);
	pp_field_string(p, 2, "axisFwd",   cfg->attitude.axis_fwd,     0);
	pp_field_string(p, 2, "axisDown",  cfg->attitude.axis_down,    0);
	pp_field_double(p, 2, "trimRollDeg",
		cfg->attitude.trim_roll_deg,  0);
	pp_field_double(p, 2, "trimPitchDeg",
		cfg->attitude.trim_pitch_deg, 1);
	pp_section_close(p, 1, is_last);
}

static void render_detect(PrettyBuf *p, const VencConfig *cfg, int is_last)
{
	pp_section_open(p, 1, "detect");
	pp_field_bool(p,   2, "enabled",      cfg->detect.enabled,       0);
	pp_field_string(p, 2, "plugin",       cfg->detect.plugin,        0);
	pp_field_string(p, 2, "modelPath",    cfg->detect.model_path,    0);
	pp_field_string(p, 2, "firmwarePath", cfg->detect.firmware_path, 0);
	pp_field_int(p,    2, "inferInterval", cfg->detect.infer_interval, 0);
	pp_field_double(p, 2, "confThresh",   cfg->detect.conf_thresh,   0);
	pp_field_double(p, 2, "nmsIou",       cfg->detect.nms_iou,       0);
	pp_field_int(p,    2, "netWidth",     (int)cfg->detect.net_width,  0);
	pp_field_int(p,    2, "netHeight",    (int)cfg->detect.net_height, 0);
	pp_field_int(p,    2, "modelId",      (int)cfg->detect.model_id,   0);
	pp_field_bool(p,   2, "osd",          cfg->detect.osd,           1);
	pp_section_close(p, 1, is_last);
}

static void render_qr(PrettyBuf *p, const VencConfig *cfg, int is_last)
{
	pp_section_open(p, 1, "qr");
	pp_field_bool(p, 2, "tapEnabled", cfg->qr.tap_enabled,       0);
	pp_field_int(p,  2, "tapWidth",   (int)cfg->qr.tap_width,    0);
	pp_field_int(p,  2, "tapHeight",  (int)cfg->qr.tap_height,   0);
	pp_field_int(p,  2, "windowMs",   (int)cfg->qr.window_ms,    1);
	pp_section_close(p, 1, is_last);
}

/* Top-level: build the canonical pretty layout into a malloc'd string.
 * Caller must free.  Returns NULL on allocation failure. */
static char *config_render_pretty(const VencConfig *cfg)
{
	PrettyBuf p;
	pp_init(&p);

	pp_str(&p, "{\n");
	render_system(&p,   cfg, 0);
	render_sensor(&p,   cfg, 0);
	render_isp(&p,      cfg, 0);
	render_image(&p,    cfg, 0);
	render_video0(&p,   cfg, 0);
	render_outgoing(&p, cfg, 0);
	render_discovery(&p, cfg, 0);
	render_fpv(&p,      cfg, 0);
	render_audio(&p,    cfg, 0);
	render_imu(&p,      cfg, 0);
	render_record(&p,   cfg, 0);
	render_snapshot(&p, cfg, 0);
	render_debug(&p,    cfg, 0);
	render_attitude(&p, cfg, 0);
	render_detect(&p,   cfg, 0);
	render_qr(&p,       cfg, 1);
	pp_str(&p, "}");

	if (p.oom) {
		free(p.buf);
		return NULL;
	}
	return p.buf;
}

/* ── Serialize to JSON ────────────────────────────────────────────────── */

static cJSON *config_to_cjson(const VencConfig *cfg)
{
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;

	/* system */
	cJSON *sys = cJSON_AddObjectToObject(root, "system");
	if (sys) {
		cJSON_AddNumberToObject(sys, "webPort", cfg->system.web_port);
		cJSON_AddNumberToObject(sys, "overclockLevel", cfg->system.overclock_level);
		cJSON_AddBoolToObject(sys, "verbose", cfg->system.verbose);
	}

	/* sensor */
	cJSON *snr = cJSON_AddObjectToObject(root, "sensor");
	if (snr) {
		cJSON_AddNumberToObject(snr, "index", cfg->sensor.index);
		cJSON_AddNumberToObject(snr, "mode", cfg->sensor.mode);
	}

	/* isp */
	cJSON *isp = cJSON_AddObjectToObject(root, "isp");
	if (isp) {
		cJSON_AddStringToObject(isp, "sensorBin", cfg->isp.sensor_bin);
		cJSON_AddStringToObject(isp, "aeEngine", cfg->isp.ae_engine);
		cJSON_AddNumberToObject(isp, "aeFps", cfg->isp.ae_fps);
		cJSON_AddNumberToObject(isp, "awbFps", cfg->isp.awb_fps);
		cJSON_AddNumberToObject(isp, "gainMax", cfg->isp.gain_max);
		cJSON_AddNumberToObject(isp, "shutterMaxUs", cfg->isp.shutter_max_us);
		cJSON_AddNumberToObject(isp, "gainMin", cfg->isp.gain_min);
		cJSON_AddNumberToObject(isp, "shutterMinUs", cfg->isp.shutter_min_us);
		cJSON_AddStringToObject(isp, "awbMode", cfg->isp.awb_mode);
		cJSON_AddNumberToObject(isp, "awbCt", cfg->isp.awb_ct);
		cJSON_AddBoolToObject(isp, "keepAspect", cfg->isp.keep_aspect);
		cJSON_AddBoolToObject(isp, "shutterRule180",
			cfg->isp.shutter_rule_180);
	}

	/* image */
	cJSON *img = cJSON_AddObjectToObject(root, "image");
	if (img) {
		cJSON_AddBoolToObject(img, "mirror", cfg->image.mirror);
		cJSON_AddBoolToObject(img, "flip", cfg->image.flip);
		cJSON_AddNumberToObject(img, "rotate", cfg->image.rotate);
	}

	/* video0 */
	cJSON *vid = cJSON_AddObjectToObject(root, "video0");
	if (vid) {
		cJSON_AddStringToObject(vid, "rcMode", cfg->video0.rc_mode);
		cJSON_AddNumberToObject(vid, "fps", cfg->video0.fps);
		if (cfg->video0.width > 0 && cfg->video0.height > 0) {
			char size_buf[32];
			snprintf(size_buf, sizeof(size_buf), "%ux%u",
				cfg->video0.width, cfg->video0.height);
			cJSON_AddStringToObject(vid, "size", size_buf);
		} else {
			cJSON_AddStringToObject(vid, "size", "auto");
		}
		cJSON_AddNumberToObject(vid, "bitrate", cfg->video0.bitrate);
		cJSON_AddNumberToObject(vid, "gopSize", cfg->video0.gop_size);
		cJSON_AddNumberToObject(vid, "qpDelta", cfg->video0.qp_delta);
		cJSON_AddNumberToObject(vid, "minQp", cfg->video0.min_qp);
		cJSON_AddNumberToObject(vid, "maxQp", cfg->video0.max_qp);
		cJSON_AddNumberToObject(vid, "sceneThreshold", cfg->video0.scene_threshold);
		cJSON_AddNumberToObject(vid, "sceneHoldoff", cfg->video0.scene_holdoff);
		cJSON_AddNumberToObject(vid, "sliceCount", cfg->video0.slice_count);
		cJSON_AddNumberToObject(vid, "frameGateCloseSlots",
			cfg->video0.frame_gate_close_slots);
		cJSON_AddNumberToObject(vid, "frameGateMaxClosedMs",
			cfg->video0.frame_gate_max_closed_ms);
		cJSON_AddStringToObject(vid, "resilience", cfg->video0.resilience);
		cJSON_AddNumberToObject(vid, "intraRefreshQp",
			cfg->video0.intra_refresh_qp);
		cJSON_AddNumberToObject(vid, "zoomX",   cfg->video0.zoom_x);
		cJSON_AddNumberToObject(vid, "zoomY",   cfg->video0.zoom_y);
		cJSON_AddStringToObject(vid, "framing", cfg->video0.framing);
		cJSON_AddNumberToObject(vid, "stabCropPct", cfg->video0.stab_crop_pct);
		cJSON_AddNumberToObject(vid, "stabRecenterSpeed",
			cfg->video0.stab_recenter_speed);
		cJSON_AddNumberToObject(vid, "stabKalmanQ",
			cfg->video0.stab_kalman_q);
		cJSON_AddNumberToObject(vid, "stabKalmanR",
			cfg->video0.stab_kalman_r);
		cJSON_AddStringToObject(vid, "stabAccuracy",
			cfg->video0.stab_accuracy);
	}

	/* outgoing */
	cJSON *out = cJSON_AddObjectToObject(root, "outgoing");
	if (out) {
		cJSON_AddBoolToObject(out, "enabled", cfg->outgoing.enabled);
		cJSON_AddStringToObject(out, "server", cfg->outgoing.server);
		cJSON_AddStringToObject(out, "streamMode", cfg->outgoing.stream_mode);
		cJSON_AddNumberToObject(out, "maxPayloadSize", cfg->outgoing.max_payload_size);
		cJSON_AddBoolToObject(out, "connectedUdp", cfg->outgoing.connected_udp);
		cJSON_AddBoolToObject(out, "allowUnixEncoderStall",
			cfg->outgoing.allow_unix_encoder_stall);
		cJSON_AddNumberToObject(out, "audioPort", cfg->outgoing.audio_port);
		cJSON_AddNumberToObject(out, "sidecarPort", cfg->outgoing.sidecar_port);
	}

	/* discovery */
	cJSON *disc = cJSON_AddObjectToObject(root, "discovery");
	if (disc) {
		cJSON_AddBoolToObject(disc, "enabled", cfg->discovery.enabled);
		cJSON_AddStringToObject(disc, "serviceType", cfg->discovery.service_type);
		cJSON_AddStringToObject(disc, "name", cfg->discovery.name);
		cJSON_AddBoolToObject(disc, "bareAlias", cfg->discovery.bare_alias);
	}

	/* fpv */
	cJSON *fpv = cJSON_AddObjectToObject(root, "fpv");
	if (fpv) {
		cJSON_AddBoolToObject(fpv, "roiEnabled", cfg->fpv.roi_enabled);
		cJSON_AddNumberToObject(fpv, "roiQp", cfg->fpv.roi_qp);
		cJSON_AddNumberToObject(fpv, "roiSteps", cfg->fpv.roi_steps);
		cJSON_AddNumberToObject(fpv, "roiCenter", cfg->fpv.roi_center);
		cJSON_AddNumberToObject(fpv, "noiseLevel", cfg->fpv.noise_level);
	}

	/* audio */
	cJSON *aud = cJSON_AddObjectToObject(root, "audio");
	if (aud) {
		cJSON_AddBoolToObject(aud, "enabled", cfg->audio.enabled);
		cJSON_AddNumberToObject(aud, "sampleRate", cfg->audio.sample_rate);
		cJSON_AddNumberToObject(aud, "channels", cfg->audio.channels);
		cJSON_AddStringToObject(aud, "codec", cfg->audio.codec);
		cJSON_AddNumberToObject(aud, "volume", cfg->audio.volume);
		cJSON_AddBoolToObject(aud, "mute", cfg->audio.mute);
	}

	/* imu */
	cJSON *imu = cJSON_AddObjectToObject(root, "imu");
	if (imu) {
		cJSON_AddBoolToObject(imu, "enabled", cfg->imu.enabled);
		cJSON_AddStringToObject(imu, "i2cDevice", cfg->imu.i2c_device);
		char addr_buf[8];
		snprintf(addr_buf, sizeof(addr_buf), "0x%02x", cfg->imu.i2c_addr);
		cJSON_AddStringToObject(imu, "i2cAddr", addr_buf);
		cJSON_AddNumberToObject(imu, "sampleRateHz", cfg->imu.sample_rate_hz);
		cJSON_AddNumberToObject(imu, "gyroRangeDps", cfg->imu.gyro_range_dps);
		cJSON_AddStringToObject(imu, "calFile", cfg->imu.cal_file);
		cJSON_AddNumberToObject(imu, "calSamples", cfg->imu.cal_samples);
	}

	/* record */
	cJSON *rec = cJSON_AddObjectToObject(root, "record");
	if (rec) {
		cJSON_AddBoolToObject(rec, "enabled", cfg->record.enabled);
		cJSON_AddStringToObject(rec, "dir", cfg->record.dir);
		cJSON_AddStringToObject(rec, "format", cfg->record.format);
		cJSON_AddStringToObject(rec, "mode", cfg->record.mode);
		cJSON_AddNumberToObject(rec, "maxSeconds", cfg->record.max_seconds);
		cJSON_AddNumberToObject(rec, "maxMB", cfg->record.max_mb);
		cJSON_AddNumberToObject(rec, "bitrate", cfg->record.bitrate);
		cJSON_AddNumberToObject(rec, "fps", cfg->record.fps);
		cJSON_AddNumberToObject(rec, "gopSize", cfg->record.gop_size);
		cJSON_AddStringToObject(rec, "server", cfg->record.server);
	}

	/* snapshot */
	cJSON *snap = cJSON_AddObjectToObject(root, "snapshot");
	if (snap) {
		cJSON_AddBoolToObject(snap,   "enabled", cfg->snapshot.enabled);
		cJSON_AddNumberToObject(snap, "quality", cfg->snapshot.quality);
		cJSON_AddNumberToObject(snap, "channel", cfg->snapshot.channel);
		cJSON_AddNumberToObject(snap, "width",   cfg->snapshot.width);
		cJSON_AddNumberToObject(snap, "height",  cfg->snapshot.height);
	}

	/* debug */
	cJSON *dbg = cJSON_AddObjectToObject(root, "debug");
	if (dbg)
		cJSON_AddBoolToObject(dbg, "showOsd", cfg->debug.show_osd);

	/* attitude */
	cJSON *att = cJSON_AddObjectToObject(root, "attitude");
	if (att) {
		cJSON_AddBoolToObject(att, "enabled", cfg->attitude.enabled);
		cJSON_AddNumberToObject(att, "mountDeg",
			cfg->attitude.mount_deg);
		cJSON_AddBoolToObject(att, "invertRoll",
			cfg->attitude.invert_roll);
		cJSON_AddBoolToObject(att, "invertPitch",
			cfg->attitude.invert_pitch);
		cJSON_AddStringToObject(att, "axisFwd",
			cfg->attitude.axis_fwd);
		cJSON_AddStringToObject(att, "axisDown",
			cfg->attitude.axis_down);
		cJSON_AddNumberToObject(att, "trimRollDeg",
			cfg->attitude.trim_roll_deg);
		cJSON_AddNumberToObject(att, "trimPitchDeg",
			cfg->attitude.trim_pitch_deg);
	}

	cJSON *det = cJSON_AddObjectToObject(root, "detect");
	if (det) {
		cJSON_AddBoolToObject(det, "enabled", cfg->detect.enabled);
		cJSON_AddStringToObject(det, "plugin", cfg->detect.plugin);
		cJSON_AddStringToObject(det, "modelPath",
			cfg->detect.model_path);
		cJSON_AddStringToObject(det, "firmwarePath",
			cfg->detect.firmware_path);
		cJSON_AddNumberToObject(det, "inferInterval",
			cfg->detect.infer_interval);
		cJSON_AddBoolToObject(det, "osd", cfg->detect.osd);
		cJSON_AddNumberToObject(det, "confThresh",
			cfg->detect.conf_thresh);
		cJSON_AddNumberToObject(det, "nmsIou", cfg->detect.nms_iou);
		cJSON_AddNumberToObject(det, "netWidth", cfg->detect.net_width);
		cJSON_AddNumberToObject(det, "netHeight",
			cfg->detect.net_height);
		cJSON_AddNumberToObject(det, "modelId", cfg->detect.model_id);
	}

	cJSON *qr = cJSON_AddObjectToObject(root, "qr");
	if (qr) {
		cJSON_AddBoolToObject(qr, "tapEnabled", cfg->qr.tap_enabled);
		cJSON_AddNumberToObject(qr, "tapWidth", cfg->qr.tap_width);
		cJSON_AddNumberToObject(qr, "tapHeight", cfg->qr.tap_height);
		cJSON_AddNumberToObject(qr, "windowMs", cfg->qr.window_ms);
	}

	return root;
}

char *venc_config_to_json_string(const VencConfig *cfg)
{
	if (!cfg)
		return NULL;
	cJSON *root = config_to_cjson(cfg);
	if (!root)
		return NULL;
	char *str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return str;
}

int venc_config_save(const char *path, const VencConfig *cfg)
{
	if (!cfg) return -1;
	char *json = config_render_pretty(cfg);
	if (!json) return -1;

	/* Atomic write: write to temp file in the same directory as the *real*
	 * target, fsync, then rename over the target.  Survives power cut
	 * mid-write — we always end up with either the old or the new config
	 * on disk, never a truncated/empty file.  Symlink-aware: if `path` is
	 * a symlink we operate on the resolved target so we replace its
	 * contents (atomic in-place semantics) rather than replacing the
	 * symlink itself with a regular file.  Mode bits are preserved from
	 * the existing file when present (default 0644 on first save). */
	char target[512];
	mode_t target_mode = 0644;
	struct stat st;
	int have_existing = 0;
	{
		ssize_t n = readlink(path, target, sizeof(target) - 1);
		if (n > 0) {
			target[n] = '\0';
			/* Relative symlink — resolve against `path`'s dir.
			 * `joined` sized for worst case `dirname(path) + '/' +
			 * readlink_target + '\0'`; we then bail if the result
			 * doesn't fit `target`. */
			if (target[0] != '/') {
				char base[512];
				char *slash;
				int n_base = snprintf(base, sizeof(base),
					"%s", path);
				if (n_base < 0 ||
				    (size_t)n_base >= sizeof(base)) {
					fprintf(stderr,
						"[venc_config] ERROR: path "
						"too long: %s\n", path);
					free(json);
					return -1;
				}
				slash = strrchr(base, '/');
				if (slash) {
					char joined[1024];
					int n_join;
					*slash = '\0';
					n_join = snprintf(joined, sizeof(joined),
						"%s/%s", base, target);
					if (n_join < 0 ||
					    (size_t)n_join >= sizeof(joined) ||
					    (size_t)n_join >= sizeof(target)) {
						fprintf(stderr,
							"[venc_config] ERROR: "
							"resolved symlink "
							"path too long\n");
						free(json);
						return -1;
					}
					memcpy(target, joined,
						(size_t)n_join + 1);
				}
			}
		} else {
			int n_path = snprintf(target, sizeof(target),
				"%s", path);
			if (n_path < 0 ||
			    (size_t)n_path >= sizeof(target)) {
				fprintf(stderr,
					"[venc_config] ERROR: path too long: "
					"%s\n", path);
				free(json);
				return -1;
			}
		}
		if (stat(target, &st) == 0) {
			target_mode = st.st_mode & 07777;
			have_existing = 1;
		}
	}
	(void)have_existing;

	size_t target_len = strlen(target);
	/* tmp_path sized for `target` + `.tmp` + '\0'.  The runtime check
	 * below guarantees no truncation, but also size the buffer past
	 * `target` so -Wformat-truncation can't false-positive. */
	char tmp_path[sizeof(target) + 8];
	if (target_len + 5 > sizeof(tmp_path)) {
		fprintf(stderr, "[venc_config] ERROR: path too long: %s\n", target);
		free(json);
		return -1;
	}
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", target);

	int fd = open(tmp_path,
		O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, target_mode);
	if (fd < 0) {
		fprintf(stderr, "[venc_config] ERROR: cannot open %s: %s\n",
			tmp_path, strerror(errno));
		free(json);
		return -1;
	}
	/* O_CREAT mode is masked by umask; restore explicitly so the saved
	 * file matches the original mode bits even when umask is non-zero. */
	(void)fchmod(fd, target_mode);

	size_t json_len = strlen(json);
	ssize_t wrote = 0;
	while ((size_t)wrote < json_len) {
		ssize_t n = write(fd, json + wrote, json_len - (size_t)wrote);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "[venc_config] ERROR: write %s: %s\n",
				tmp_path, strerror(errno));
			close(fd);
			unlink(tmp_path);
			free(json);
			return -1;
		}
		wrote += n;
	}
	/* trailing newline — short-write retry for completeness */
	{
		const char nl = '\n';
		ssize_t n;
		do {
			n = write(fd, &nl, 1);
		} while (n < 0 && errno == EINTR);
		if (n != 1) {
			fprintf(stderr,
				"[venc_config] ERROR: trailing-nl write %s: %s\n",
				tmp_path, strerror(errno));
			close(fd);
			unlink(tmp_path);
			free(json);
			return -1;
		}
	}

	if (fsync(fd) != 0) {
		fprintf(stderr, "[venc_config] ERROR: fsync %s: %s\n",
			tmp_path, strerror(errno));
		close(fd);
		unlink(tmp_path);
		free(json);
		return -1;
	}
	close(fd);
	free(json);

	if (rename(tmp_path, target) != 0) {
		fprintf(stderr, "[venc_config] ERROR: rename %s -> %s: %s\n",
			tmp_path, target, strerror(errno));
		unlink(tmp_path);
		return -1;
	}

	/* fsync the containing directory so the rename itself is durable.
	 * O_DIRECTORY is informational on Linux but flags the intent and
	 * fails fast on platforms that enforce it. */
	{
		char dir_buf[512];
		snprintf(dir_buf, sizeof(dir_buf), "%s", target);
		char *slash = strrchr(dir_buf, '/');
		const char *dir_path = slash ?
			(*slash = '\0', dir_buf) : ".";
		int dfd = open(dir_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (dfd < 0) {
			fprintf(stderr,
				"[venc_config] ERROR: open dir %s: %s\n",
				dir_path, strerror(errno));
			return -1;
		}
		if (fsync(dfd) != 0) {
			fprintf(stderr,
				"[venc_config] ERROR: fsync dir %s: %s\n",
				dir_path, strerror(errno));
			close(dfd);
			return -1;
		}
		close(dfd);
	}

	fprintf(stderr, "[venc_config] Config saved to %s\n", target);
	return 0;
}
