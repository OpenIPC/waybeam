/*
 * Unit tests for venc_config.c
 *
 * Tests: defaults, JSON loading, URI parsing, round-trip serialization,
 * missing file fallback, bad JSON handling, resolution parsing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "venc_config.h"
#include "test_helpers.h"
#include "../lib/cJSON.h"

/* ── Helper: write a temp JSON file ──────────────────────────────────── */

static char *write_temp_json(const char *json)
{
	char *path = strdup("/tmp/venc_test_XXXXXX");
	int fd = mkstemp(path);
	if (fd < 0) { free(path); return NULL; }
	write(fd, json, strlen(json));
	close(fd);
	return path;
}

/* ── Tests ───────────────────────────────────────────────────────────── */

static int test_defaults(void)
{
	int failures = 0;
	VencConfig cfg;
	venc_config_defaults(&cfg);

	CHECK("defaults_web_port", cfg.system.web_port == 80);
	CHECK("defaults_overclock", cfg.system.overclock_level == 1);
	CHECK("defaults_verbose", cfg.system.verbose == false);

	CHECK("defaults_sensor_index", cfg.sensor.index == -1);
	CHECK("defaults_sensor_mode", cfg.sensor.mode == -1);
	/* unlock_* retired from user surface but still drives the
	 * IMX415/IMX335 cold-boot register hook unconditionally. */
	CHECK("defaults_unlock_enabled_on", cfg.sensor.unlock_enabled == true);
	CHECK("defaults_unlock_cmd", cfg.sensor.unlock_cmd == 0x23);
	CHECK("defaults_unlock_reg", cfg.sensor.unlock_reg == 0x300a);
	CHECK("defaults_unlock_value", cfg.sensor.unlock_value == 0x80);

	CHECK("defaults_mirror", cfg.image.mirror == false);
	CHECK("defaults_flip", cfg.image.flip == false);

	CHECK("defaults_rc_mode", strcmp(cfg.video0.rc_mode, "cbr") == 0);
	CHECK("defaults_fps", cfg.video0.fps == 60);
	CHECK("defaults_width_auto", cfg.video0.width == 0);
	CHECK("defaults_height_auto", cfg.video0.height == 0);
	CHECK("defaults_bitrate", cfg.video0.bitrate == 8192);
	CHECK("defaults_gop_size", cfg.video0.gop_size == 1.0);
	CHECK("defaults_qp_delta", cfg.video0.qp_delta == -12);
	CHECK("defaults_zoom_off", cfg.video0.zoom_pct == 0.0);
	CHECK("defaults_zoom_x", cfg.video0.zoom_x == 0.5);
	CHECK("defaults_zoom_y", cfg.video0.zoom_y == 0.5);
	CHECK("defaults_enabled", cfg.outgoing.enabled == false);
	CHECK("defaults_server", cfg.outgoing.server[0] == '\0');
	CHECK("defaults_stream_mode", strcmp(cfg.outgoing.stream_mode, "rtp") == 0);
	CHECK("defaults_payload", cfg.outgoing.max_payload_size == 1400);
	CHECK("defaults_connected_udp", cfg.outgoing.connected_udp == true);
	CHECK("defaults_allow_unix_encoder_stall",
		cfg.outgoing.allow_unix_encoder_stall == false);

	/* Off, but pre-loaded with a delta that bites when switched on -- the
	 * two halves of the shipped pair are asserted together on purpose. */
	CHECK("defaults_roi_off", cfg.fpv.roi_enabled == false);
	CHECK("defaults_roi_qp", cfg.fpv.roi_qp == -20);
	CHECK("defaults_roi_steps", cfg.fpv.roi_steps == 2);
	CHECK("defaults_noise", cfg.fpv.noise_level == 0);

	CHECK("defaults_audio_off", cfg.audio.enabled == false);
	CHECK("defaults_audio_rate", cfg.audio.sample_rate == 48000);
	CHECK("defaults_audio_ch", cfg.audio.channels == 1);
	CHECK("defaults_audio_codec", strcmp(cfg.audio.codec, "opus") == 0);
	CHECK("defaults_audio_vol", cfg.audio.volume == 80);
	CHECK("defaults_audio_port", cfg.outgoing.audio_port == 5601);
	CHECK("defaults_scene_threshold_off", cfg.video0.scene_threshold == 0);
	CHECK("defaults_scene_holdoff", cfg.video0.scene_holdoff == 2);
	CHECK("defaults_slice_count_off", cfg.video0.slice_count == 1);

	CHECK("defaults_ref_base_off", cfg.video0.ref_base == 0);
	CHECK("defaults_ref_enhance", cfg.video0.ref_enhance == 0);
	CHECK("defaults_ref_pred_on", cfg.video0.ref_pred == true);
	CHECK("defaults_resilience_off", strcmp(cfg.video0.resilience, "off") == 0);

	return failures;
}

/* Resilience preset is the sole driver of intra-refresh, SVC-T, and
 * (for named presets) gop_size.  "off" preserves the user's gopSize. */
/* A negative frame-gate value in a config FILE must be rejected with a range
 * error, not clamped to 0 (= "use the default"), or an operator typo is
 * accepted silently. */
static int test_frame_gate_negative_not_clamped(void)
{
	int failures = 0;
	VencConfig cfg;
	char path[] = "/tmp/venc_frame_gate_test_XXXXXX";
	int fd = mkstemp(path);
	FILE *f;

	CHECK("frame_gate_mkstemp_ok", fd >= 0);
	if (fd < 0)
		return failures;
	f = fdopen(fd, "w");
	if (!f) { close(fd); unlink(path); return failures + 1; }
	fputs("{\"video0\":{\"frameGateCloseSlots\":-1,"
		"\"frameGateMaxClosedMs\":-1}}", f);
	fclose(f);

	venc_config_defaults(&cfg);
	CHECK("frame_gate_negative_rejected", venc_config_load(path, &cfg) != 0);
	unlink(path);
	return failures;
}

static int test_resilience_preset_expansion(void)
{
	int failures = 0;
	VencConfig cfg;
	char path[] = "/tmp/venc_resilience_test_XXXXXX";
	int fd = mkstemp(path);
	CHECK("resilience_mkstemp_ok", fd >= 0);
	if (fd < 0) return failures;

	struct {
		const char *name;
		const char *ir;
		uint8_t    b;
		uint8_t    e;
		double     gop;
	} cases[] = {
		{ "rescue",    "off",      0, 0, 0.25 }, /* IDR-spam, lowest recovery latency */
		{ "quality",   "off",      0, 0, 4.0 },
		{ "sprint",    "fast",     0, 0, 0.5 },  /* intra + aggressive IDR */
		{ "racing",    "fast",     0, 0, 2.0 },
		{ "endurance", "balanced", 0, 0, 2.0 },  /* OSD-safe */
		{ "patrol",    "balanced", 0, 0, 4.0 },  /* OSD-safe + long GOP */
		{ "rally",     "fast",     1, 1, 2.0 },  /* light refPred (OSD-unsafe) */
		{ "range",     "balanced", 1, 4, 2.0 },  /* heavy refPred (OSD-unsafe) */
		{ "fpv",       "robust",   1, 4, 2.0 },  /* heaviest refPred (OSD-unsafe) */
	};
	for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
		/* User's gopSize=7.5 must be discarded by any named preset. */
		char json[160];
		snprintf(json, sizeof(json),
			"{\"video0\":{\"resilience\":\"%s\",\"gopSize\":7.5}}",
			cases[i].name);
		FILE *f = fopen(path, "w");
		if (!f) { close(fd); unlink(path); return failures; }
		fputs(json, f);
		fclose(f);

		venc_config_defaults(&cfg);
		int rc = venc_config_load(path, &cfg);
		CHECK("resilience_load_ok", rc == 0);
		CHECK("resilience_preset_stored",
			strcmp(cfg.video0.resilience, cases[i].name) == 0);
		CHECK("resilience_preset_intra_refresh",
			strcmp(cfg.video0.intra_refresh_mode, cases[i].ir) == 0);
		CHECK("resilience_preset_ref_base",
			cfg.video0.ref_base == cases[i].b);
		CHECK("resilience_preset_ref_enhance",
			cfg.video0.ref_enhance == cases[i].e);
		CHECK("resilience_preset_overrides_gop",
			cfg.video0.gop_size == cases[i].gop);
	}

	/* off mode preserves the user's gopSize. */
	{
		const char *json =
			"{\"video0\":{\"resilience\":\"off\",\"gopSize\":3.25}}";
		FILE *f = fopen(path, "w");
		if (!f) { close(fd); unlink(path); return failures; }
		fputs(json, f);
		fclose(f);

		venc_config_defaults(&cfg);
		int rc = venc_config_load(path, &cfg);
		CHECK("resilience_off_load_ok", rc == 0);
		CHECK("resilience_off_stored",
			strcmp(cfg.video0.resilience, "off") == 0);
		CHECK("resilience_off_keeps_user_gop",
			cfg.video0.gop_size == 3.25);
		CHECK("resilience_off_intra_off",
			strcmp(cfg.video0.intra_refresh_mode, "off") == 0);
		CHECK("resilience_off_ref_base_zero", cfg.video0.ref_base == 0);
	}

	/* Unknown preset falls back to off with the user's gopSize honoured. */
	{
		const char *json =
			"{\"video0\":{\"resilience\":\"bogus\",\"gopSize\":2.0}}";
		FILE *f = fopen(path, "w");
		if (!f) { close(fd); unlink(path); return failures; }
		fputs(json, f);
		fclose(f);

		venc_config_defaults(&cfg);
		int rc = venc_config_load(path, &cfg);
		CHECK("resilience_unknown_load_ok", rc == 0);
		CHECK("resilience_unknown_falls_back_to_off",
			strcmp(cfg.video0.resilience, "off") == 0);
		CHECK("resilience_unknown_intra_off",
			strcmp(cfg.video0.intra_refresh_mode, "off") == 0);
		CHECK("resilience_unknown_keeps_user_gop",
			cfg.video0.gop_size == 2.0);
	}

	/* "ltr" family: base=1, pred off, intra-refresh off, user's gopSize
	 * preserved (a long GOP is the point of the scheme).  Bare "ltr" must
	 * be enhance=1 — device-measured as the MAXIMUM non-reference density
	 * (one droppable frame in every enhance+1), so any larger default
	 * would silently be less resilient. */
	{
		struct {
			const char *preset;
			uint8_t     enhance;
		} ltr_cases[] = {
			{ "ltr",     1 },
			{ "ltr:1",   1 },
			{ "ltr:4",   4 },
			{ "ltr:255", 255 },
		};
		for (size_t i = 0; i < sizeof(ltr_cases)/sizeof(ltr_cases[0]); ++i) {
			char json[160];
			snprintf(json, sizeof(json),
				"{\"video0\":{\"resilience\":\"%s\",\"gopSize\":5.0}}",
				ltr_cases[i].preset);
			FILE *f = fopen(path, "w");
			if (!f) { close(fd); unlink(path); return failures; }
			fputs(json, f);
			fclose(f);

			venc_config_defaults(&cfg);
			int rc = venc_config_load(path, &cfg);
			CHECK("ltr_load_ok", rc == 0);
			CHECK("ltr_preset_stored",
				strcmp(cfg.video0.resilience, ltr_cases[i].preset) == 0);
			CHECK("ltr_ref_base_one", cfg.video0.ref_base == 1);
			CHECK("ltr_ref_pred_off", cfg.video0.ref_pred == false);
			CHECK("ltr_intra_off",
				strcmp(cfg.video0.intra_refresh_mode, "off") == 0);
			CHECK("ltr_ref_enhance",
				cfg.video0.ref_enhance == ltr_cases[i].enhance);
			CHECK("ltr_keeps_user_gop", cfg.video0.gop_size == 5.0);
		}
	}

	/* Malformed "ltr:" forms are rejected outright (fall back to off) —
	 * a silently-clamped ratio would make a sweep measure the wrong thing. */
	{
		const char *bad[] = { "ltr:", "ltr:0", "ltr:abc", "ltr:12x",
				      "ltr:256", "ltr:99999", "ltrx" };
		for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
			VencConfigVideo v;
			venc_config_defaults(&cfg);
			v = cfg.video0;
			CHECK("ltr_bad_rejected",
				venc_config_apply_resilience_preset(bad[i], &v) != 0);
		}
	}

	/* "ltr" differs from "rally" (same 1:1 ratio) exactly in that it keeps
	 * the caller's GOP and forces intra-refresh off.  Guard both, since
	 * that difference is the whole reason the preset exists. */
	{
		VencConfigVideo v;
		venc_config_defaults(&cfg);
		v = cfg.video0;
		v.gop_size = 5.0;
		CHECK("ltr_vs_rally_apply_ok",
			venc_config_apply_resilience_preset("ltr", &v) == 0);
		CHECK("ltr_vs_rally_keeps_gop", v.gop_size == 5.0);
		CHECK("ltr_vs_rally_intra_off",
			strcmp(v.intra_refresh_mode, "off") == 0);

		venc_config_defaults(&cfg);
		v = cfg.video0;
		v.gop_size = 5.0;
		CHECK("rally_apply_ok",
			venc_config_apply_resilience_preset("rally", &v) == 0);
		CHECK("rally_same_ratio", v.ref_enhance == 1 && v.ref_base == 1);
		CHECK("rally_overrides_gop", v.gop_size == 2.0);
		CHECK("rally_enables_intra",
			strcmp(v.intra_refresh_mode, "off") != 0);
	}

	close(fd);
	unlink(path);
	return failures;
}

static int test_load_full_json(void)
{
	int failures = 0;
	const char *json =
		"{"
		"  \"system\": { \"webPort\": 8080, \"overclockLevel\": 1, \"verbose\": true },"
		/* unlockEnabled/Cmd/Reg/Value/Dir are retired in 0.10.13 —
		 * parser must silently drop them.  Setting unlockEnabled=false
		 * here proves migration: the JSON value is ignored and the
		 * always-on default (true) drives the cold-boot register hook. */
		"  \"sensor\": { \"index\": 2, \"mode\": 3, \"unlockEnabled\": false },"
		/* aeEngine "custom" is retired/removed — parser must migrate a
		 * stale value to "sdk" (with a warning) rather than reject it. */
		"  \"isp\": { \"sensorBin\": \"/etc/sensors/imx415.bin\","
		"    \"aeEngine\": \"custom\","
		"    \"gainMax\": 8192, \"shutterMaxUs\": 8000,"
		"    \"gainMin\": 1500, \"shutterMinUs\": 200 },"
		"  \"image\": { \"mirror\": true, \"flip\": true },"
		"  \"video0\": { \"codec\": \"h264\", \"rcMode\": \"vbr\", \"fps\": 90,"
		/* "codec" above is intentionally legacy — parser must silently drop it. */
		"    \"size\": \"1280x720\", \"bitrate\": 4096, \"gopSize\": 1, \"qpDelta\": -7,"
		"    \"framing\": \"zoom-2x\", \"zoomX\": 0.25, \"zoomY\": 0.75 },"
		"  \"outgoing\": { \"enabled\": true, \"server\": \"udp://10.0.0.1:6000\", \"streamMode\": \"compact\", \"maxPayloadSize\": 1200, \"connectedUdp\": false, \"allowUnixEncoderStall\": true },"
		"  \"fpv\": { \"roiEnabled\": true, \"roiQp\": -18, \"roiSteps\": 2, \"noiseLevel\": 5 }"
		"}";

	char *path = write_temp_json(json);
	CHECK("tmpfile_created", path != NULL);
	if (!path) return failures;

	VencConfig cfg;
	venc_config_defaults(&cfg);
	int ret = venc_config_load(path, &cfg);
	unlink(path);
	free(path);

	CHECK("load_ok", ret == 0);
	CHECK("load_web_port", cfg.system.web_port == 8080);
	CHECK("load_overclock", cfg.system.overclock_level == 1);
	CHECK("load_verbose", cfg.system.verbose == true);
	CHECK("load_sensor_index", cfg.sensor.index == 2);
	CHECK("load_sensor_mode", cfg.sensor.mode == 3);
	CHECK("load_unlock_legacy_ignored",
		cfg.sensor.unlock_enabled == true);   /* JSON had false; default wins */
	CHECK("load_isp_bin", strcmp(cfg.isp.sensor_bin, "/etc/sensors/imx415.bin") == 0);
	CHECK("load_ae_engine_custom_migrates_sdk",
		strcmp(cfg.isp.ae_engine, "sdk") == 0);   /* JSON had "custom"; falls back */
	CHECK("load_isp_gain_max", cfg.isp.gain_max == 8192);
	CHECK("load_isp_shutter_max", cfg.isp.shutter_max_us == 8000);
	CHECK("load_isp_gain_min", cfg.isp.gain_min == 1500);
	CHECK("load_isp_shutter_min", cfg.isp.shutter_min_us == 200);
	CHECK("load_mirror", cfg.image.mirror == true);
	CHECK("load_flip", cfg.image.flip == true);
	CHECK("load_rc", strcmp(cfg.video0.rc_mode, "vbr") == 0);
	CHECK("load_fps", cfg.video0.fps == 90);
	CHECK("load_width", cfg.video0.width == 1280);
	CHECK("load_height", cfg.video0.height == 720);
	CHECK("load_bitrate", cfg.video0.bitrate == 4096);
	CHECK("load_gop", cfg.video0.gop_size == 1);
	CHECK("load_qp_delta", cfg.video0.qp_delta == -7);
	CHECK("load_framing_zoom2x", strcmp(cfg.video0.framing, "zoom-2x") == 0);
	CHECK("load_framing_zoom_pct", cfg.video0.zoom_pct == 0.5);
	CHECK("load_zoom_x", cfg.video0.zoom_x == 0.25);
	CHECK("load_zoom_y", cfg.video0.zoom_y == 0.75);
	CHECK("load_enabled", cfg.outgoing.enabled == true);
	CHECK("load_server", strcmp(cfg.outgoing.server, "udp://10.0.0.1:6000") == 0);
	CHECK("load_stream_mode", strcmp(cfg.outgoing.stream_mode, "compact") == 0);
	CHECK("load_payload", cfg.outgoing.max_payload_size == 1200);
	CHECK("load_connected_udp", cfg.outgoing.connected_udp == false);
	CHECK("load_allow_unix_encoder_stall",
		cfg.outgoing.allow_unix_encoder_stall == true);
	CHECK("load_roi_on", cfg.fpv.roi_enabled == true);
	CHECK("load_roi_qp", cfg.fpv.roi_qp == -18);
	CHECK("load_roi_steps", cfg.fpv.roi_steps == 2);
	CHECK("load_noise", cfg.fpv.noise_level == 5);

	/* A craft already carrying the old -30 must still BOOT, landing on the
	 * new bound rather than failing to load: beyond +-20 the delta exceeds
	 * what the encoder's QP range can honour, and on the negative side it
	 * saturates rate control and overruns the bitrate target.  Clamped on
	 * the file path, rejected on the API path -- an operator typing -30
	 * gets told, a config file that predates the bound gets migrated. */
	{
		VencConfig old;
		char *p2 = write_temp_json("{ \"fpv\": { \"roiQp\": -30 } }");

		CHECK("roi_qp_tmpfile", p2 != NULL);
		if (p2) {
			venc_config_defaults(&old);
			CHECK("roi_qp_old_file_loads",
				venc_config_load(p2, &old) == 0);
			CHECK("roi_qp_old_file_clamps", old.fpv.roi_qp == -20);
			unlink(p2); free(p2);
		}
		p2 = write_temp_json("{ \"fpv\": { \"roiQp\": 30 } }");
		CHECK("roi_qp_tmpfile_pos", p2 != NULL);
		if (p2) {
			venc_config_defaults(&old);
			CHECK("roi_qp_pos_file_loads",
				venc_config_load(p2, &old) == 0);
			CHECK("roi_qp_pos_file_clamps", old.fpv.roi_qp == 20);
			unlink(p2); free(p2);
		}
		/* In-range values must survive untouched, or the clamp above
		 * would be indistinguishable from one that pins everything. */
		p2 = write_temp_json("{ \"fpv\": { \"roiQp\": -17 } }");
		CHECK("roi_qp_tmpfile_mid", p2 != NULL);
		if (p2) {
			venc_config_defaults(&old);
			CHECK("roi_qp_mid_file_loads",
				venc_config_load(p2, &old) == 0);
			CHECK("roi_qp_mid_survives", old.fpv.roi_qp == -17);
			unlink(p2); free(p2);
		}
	}
	/* scene_threshold/scene_holdoff live in video0 section */

	return failures;
}

static int test_slice_count(void)
{
	int failures = 0;
	const char *json =
		"{ \"video0\": { \"sliceCount\": 4 } }";
	char *path = write_temp_json(json);
	VencConfig cfg;

	CHECK("slice_tmpfile", path != NULL);
	if (!path) return failures;
	venc_config_defaults(&cfg);
	CHECK("slice_load", venc_config_load(path, &cfg) == 0);
	CHECK("slice_count_4", cfg.video0.slice_count == 4);
	unlink(path); free(path);

	/* out-of-range values clamp to VENC_SLICE_COUNT_MAX */
	path = write_temp_json("{ \"video0\": { \"sliceCount\": 99 } }");
	CHECK("slice_tmpfile2", path != NULL);
	if (!path) return failures;
	CHECK("slice_load2", venc_config_load(path, &cfg) == 0);
	CHECK("slice_count_clamped",
	      cfg.video0.slice_count == VENC_SLICE_COUNT_MAX);
	unlink(path); free(path);

	/* 17 must survive the clamp: it is one slice per CTU-64 row at 1080p,
	 * the finest the encoder can deliver, and the old ceiling of 8 made it
	 * — and 9 — unreachable, capping the fleet at 6 delivered slices. */
	path = write_temp_json("{ \"video0\": { \"sliceCount\": 17 } }");
	CHECK("slice_tmpfile5", path != NULL);
	if (!path) return failures;
	CHECK("slice_load5", venc_config_load(path, &cfg) == 0);
	CHECK("slice_count_17_survives", cfg.video0.slice_count == 17);
	unlink(path); free(path);

	path = write_temp_json("{ \"video0\": { \"sliceCount\": 0 } }");
	CHECK("slice_tmpfile3", path != NULL);
	if (!path) return failures;
	CHECK("slice_load3", venc_config_load(path, &cfg) == 0);
	CHECK("slice_count_floor", cfg.video0.slice_count == 1);
	unlink(path); free(path);

	/* A negative value must floor to 1, not wrap through the unsigned
	 * store to the 8-slice maximum. */
	path = write_temp_json("{ \"video0\": { \"sliceCount\": -1 } }");
	CHECK("slice_tmpfile4", path != NULL);
	if (!path) return failures;
	CHECK("slice_load4", venc_config_load(path, &cfg) == 0);
	CHECK("slice_count_negative_floors", cfg.video0.slice_count == 1);
	unlink(path); free(path);

	return failures;
}

static int test_load_partial_json(void)
{
	int failures = 0;
	const char *json = "{ \"video0\": { \"fps\": 120 } }";

	char *path = write_temp_json(json);
	CHECK("partial_tmpfile", path != NULL);
	if (!path) return failures;

	VencConfig cfg;
	venc_config_defaults(&cfg);
	int ret = venc_config_load(path, &cfg);
	unlink(path);
	free(path);

	CHECK("partial_ok", ret == 0);
	CHECK("partial_fps_set", cfg.video0.fps == 120);
	/* All other fields retain defaults */
	CHECK("partial_bitrate_default", cfg.video0.bitrate == 8192);
	CHECK("partial_web_port_default", cfg.system.web_port == 80);

	return failures;
}

static int test_load_missing_file(void)
{
	int failures = 0;
	VencConfig cfg;
	venc_config_defaults(&cfg);
	int ret = venc_config_load("/tmp/nonexistent_venc_test_file.json", &cfg);
	CHECK("missing_file_ok", ret == 0);
	/* Defaults preserved */
	CHECK("missing_defaults_fps", cfg.video0.fps == 60);
	return failures;
}

static int test_load_bad_json(void)
{
	int failures = 0;
	const char *bad = "{ this is not json }";
	char *path = write_temp_json(bad);
	CHECK("bad_tmpfile", path != NULL);
	if (!path) return failures;

	VencConfig cfg;
	venc_config_defaults(&cfg);
	int ret = venc_config_load(path, &cfg);
	unlink(path);
	free(path);

	CHECK("bad_json_fails", ret == -1);
	return failures;
}

static int test_load_qp_bounds_validation(void)
{
	static const struct {
		const char *json;
		int expected;
	} cases[] = {
		{ "{\"video0\":{\"minQp\":10,\"maxQp\":40}}", 0 },
		{ "{\"video0\":{\"minQp\":52}}", -1 },
		{ "{\"video0\":{\"maxQp\":52}}", -1 },
		{ "{\"video0\":{\"minQp\":40,\"maxQp\":10}}", -1 },
		/* The loader reads JSON integers before assigning the unsigned
		 * config fields, so a negative value wraps large and must still be
		 * rejected by the common startup validator. */
		{ "{\"video0\":{\"minQp\":-1}}", -1 },
	};
	int failures = 0;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		char *path = write_temp_json(cases[i].json);
		VencConfig cfg;
		int ret;

		CHECK("qp bounds temp file", path != NULL);
		if (!path)
			continue;
		venc_config_defaults(&cfg);
		ret = venc_config_load(path, &cfg);
		unlink(path);
		free(path);
		CHECK("qp bounds startup validation", ret == cases[i].expected);
	}

	return failures;
}

static int test_uri_parsing(void)
{
	int failures = 0;
	char host[64];
	uint16_t port;
	VencOutputUri parsed;

	/* UDP URI */
	int ret = venc_config_parse_server_uri("udp://10.0.0.1:6000",
		host, sizeof(host), &port);
	CHECK("udp_ok", ret == 0);
	CHECK("udp_host", strcmp(host, "10.0.0.1") == 0);
	CHECK("udp_port", port == 6000);

	ret = venc_config_parse_output_uri("udp://10.0.0.1:6000", &parsed);
	CHECK("udp_output_uri_ok", ret == 0);
	CHECK("udp_output_uri_type", parsed.type == VENC_OUTPUT_URI_UDP);
	CHECK("udp_output_uri_host", strcmp(parsed.host, "10.0.0.1") == 0);
	CHECK("udp_output_uri_port", parsed.port == 6000);

	ret = venc_config_parse_output_uri("unix://waybeam_venc", &parsed);
	CHECK("unix_output_uri_ok", ret == 0);
	CHECK("unix_output_uri_type", parsed.type == VENC_OUTPUT_URI_UNIX);
	CHECK("unix_output_uri_name",
		strcmp(parsed.endpoint, "waybeam_venc") == 0);

	ret = venc_config_parse_output_uri("shm://venc_ring", &parsed);
	CHECK("shm_output_uri_ok", ret == 0);
	CHECK("shm_output_uri_type", parsed.type == VENC_OUTPUT_URI_SHM);
	CHECK("shm_output_uri_name", strcmp(parsed.endpoint, "venc_ring") == 0);

	/* Bad scheme */
	ret = venc_config_parse_server_uri("http://bad:80",
		host, sizeof(host), &port);
	CHECK("bad_scheme_fails", ret == -1);

	/* rtp:// scheme rejected */
	ret = venc_config_parse_server_uri("rtp://192.168.1.2:5600",
		host, sizeof(host), &port);
	CHECK("rtp_scheme_fails", ret == -1);

	/* Missing port */
	ret = venc_config_parse_server_uri("udp://host",
		host, sizeof(host), &port);
	CHECK("missing_port_fails", ret == -1);

	ret = venc_config_parse_server_uri("unix://waybeam_venc",
		host, sizeof(host), &port);
	CHECK("unix_server_uri_rejected", ret == -1);

	/* NULL args */
	ret = venc_config_parse_server_uri(NULL, host, sizeof(host), &port);
	CHECK("null_uri_fails", ret == -1);

	return failures;
}

static int test_roundtrip(void)
{
	int failures = 0;
	VencConfig cfg;
	venc_config_defaults(&cfg);
	cfg.video0.fps = 90;
	cfg.video0.bitrate = 12000;
	cfg.video0.qp_delta = 6;
	cfg.system.verbose = true;
	cfg.video0.scene_threshold = 150;
	strcpy(cfg.video0.framing, "zoom-2x");  /* zoom_pct derived on reload */
	cfg.video0.zoom_x = 0.25;
	cfg.video0.zoom_y = 0.75;

	char *json = venc_config_to_json_string(&cfg);
	CHECK("serialize_ok", json != NULL);
	if (!json) return failures;

	/* Write serialized JSON to file and reload */
	char *path = write_temp_json(json);
	free(json);
	CHECK("roundtrip_tmpfile", path != NULL);
	if (!path) return failures;

	VencConfig cfg2;
	venc_config_defaults(&cfg2);
	int ret = venc_config_load(path, &cfg2);
	unlink(path);
	free(path);

	CHECK("roundtrip_load_ok", ret == 0);
	CHECK("roundtrip_fps", cfg2.video0.fps == 90);
	CHECK("roundtrip_bitrate", cfg2.video0.bitrate == 12000);
	CHECK("roundtrip_qp_delta", cfg2.video0.qp_delta == 6);
	CHECK("roundtrip_verbose", cfg2.system.verbose == true);
	CHECK("roundtrip_scene_threshold", cfg2.video0.scene_threshold == 150);
	CHECK("roundtrip_framing", strcmp(cfg2.video0.framing, "zoom-2x") == 0);
	CHECK("roundtrip_zoom_pct", cfg2.video0.zoom_pct == 0.5);
	CHECK("roundtrip_zoom_x", cfg2.video0.zoom_x == 0.25);
	CHECK("roundtrip_zoom_y", cfg2.video0.zoom_y == 0.75);
	/* Unchanged fields preserved */
	CHECK("roundtrip_gop", cfg2.video0.gop_size == 1.0);

	return failures;
}

static int test_overclock_clamping(void)
{
	int failures = 0;
	const char *json = "{ \"system\": { \"overclockLevel\": 5 } }";
	char *path = write_temp_json(json);
	CHECK("clamp_tmpfile", path != NULL);
	if (!path) return failures;

	VencConfig cfg;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);

	CHECK("overclock_clamped_to_2", cfg.system.overclock_level == 2);
	return failures;
}

static int test_noise_level_clamping(void)
{
	int failures = 0;
	const char *json = "{ \"fpv\": { \"noiseLevel\": 10 } }";
	char *path = write_temp_json(json);
	CHECK("noise_tmpfile", path != NULL);
	if (!path) return failures;

	VencConfig cfg;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);

	CHECK("noise_clamped_to_7", cfg.fpv.noise_level == 7);
	return failures;
}

static int test_framing_presets(void)
{
	int failures = 0;
	VencConfig cfg;
	char *path;

	/* Zoom presets expand into zoom_pct; pan still clamps. */
	path = write_temp_json("{ \"video0\": { \"framing\": \"zoom-1.25x\", "
		"\"zoomX\": -1, \"zoomY\": 2 } }");
	CHECK("framing zoom tmpfile", path != NULL);
	if (!path) return failures;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("framing zoom1.25 pct", cfg.video0.zoom_pct == 0.80);
	CHECK("framing zoom no stab", cfg.video0.stab_crop_pct == 0);
	CHECK("framing zoom x clamped low", cfg.video0.zoom_x == 0.0);
	CHECK("framing zoom y clamped high", cfg.video0.zoom_y == 1.0);

	/* 2x maps to 0.50. */
	path = write_temp_json("{ \"video0\": { \"framing\": \"zoom-2x\" } }");
	if (!path) return failures;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("framing zoom2x pct", cfg.video0.zoom_pct == 0.50);

	/* 3x maps to 0.3333 (no upscale — Approach-C emits a smaller frame). */
	path = write_temp_json("{ \"video0\": { \"framing\": \"zoom-3x\" } }");
	if (!path) return failures;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("framing zoom3x pct", cfg.video0.zoom_pct == 0.3333);
	CHECK("framing zoom3x no stab", cfg.video0.stab_crop_pct == 0);

	/* 4x maps to 0.25 (tightest zoom; 1080p -> 480x256, above 256 floor). */
	path = write_temp_json("{ \"video0\": { \"framing\": \"zoom-4x\" } }");
	if (!path) return failures;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("framing zoom4x pct", cfg.video0.zoom_pct == 0.25);
	CHECK("framing zoom4x no stab", cfg.video0.stab_crop_pct == 0);

	/* The single "stab" preset expands into stab_crop_pct/recenter, zoom 0. */
	path = write_temp_json("{ \"video0\": { \"framing\": \"stab\" } }");
	if (!path) return failures;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("framing stab name", strcmp(cfg.video0.framing, "stab") == 0);
	CHECK("framing stab crop", cfg.video0.stab_crop_pct == 80);
	CHECK("framing stab recenter", cfg.video0.stab_recenter_speed == 180);
	CHECK("framing stab no zoom", cfg.video0.zoom_pct == 0.0);
	/* The "stab" preset also seeds the shared Kalman knobs (q=0.03, r=2.0). */
	CHECK("framing stab kalman q", cfg.video0.stab_kalman_q == 0.03);
	CHECK("framing stab kalman r", cfg.video0.stab_kalman_r == 2.0);

	/* Advanced overrides win over the preset's derived 80/180, and are read
	 * after preset expansion (stick mode = recenter 0, tighter 60% crop). */
	path = write_temp_json("{ \"video0\": { \"framing\": \"stab\", "
		"\"stabCropPct\": 60, \"stabRecenterSpeed\": 0 } }");
	if (!path) return failures;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("stab override crop", cfg.video0.stab_crop_pct == 60);
	CHECK("stab override recenter stick", cfg.video0.stab_recenter_speed == 0);

	/* Kalman q/r override the preset defaults the same way. */
	path = write_temp_json("{ \"video0\": { \"framing\": \"stab\", "
		"\"stabKalmanQ\": 0.08, \"stabKalmanR\": 5.0 } }");
	if (!path) return failures;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("stab override kalman q", cfg.video0.stab_kalman_q == 0.08);
	CHECK("stab override kalman r", cfg.video0.stab_kalman_r == 5.0);

	/* Absent Kalman keys keep the preset defaults (0.03/2.0). */
	path = write_temp_json("{ \"video0\": { \"framing\": \"stab\", "
		"\"stabCropPct\": 60 } }");
	if (!path) return failures;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("stab kalman absent keeps q", cfg.video0.stab_kalman_q == 0.03);
	CHECK("stab kalman absent keeps r", cfg.video0.stab_kalman_r == 2.0);

	/* Absent override keys keep the preset default (plain stab = 80/180). */
	path = write_temp_json("{ \"video0\": { \"framing\": \"stab\" } }");
	if (!path) return failures;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("stab override absent keeps crop", cfg.video0.stab_crop_pct == 80);
	CHECK("stab override absent keeps recenter",
		cfg.video0.stab_recenter_speed == 180);

	/* A stale stabCropPct/stabRecenterSpeed left over from a prior stab
	 * session must NOT re-enable stabilization at framing="off" — the
	 * overrides are scoped to framing="stab" (regression: stab ran with
	 * framing=off because star6e_stab_enabled() keys on stab_crop_pct). */
	path = write_temp_json("{ \"video0\": { \"framing\": \"off\", "
		"\"stabCropPct\": 60, \"stabRecenterSpeed\": 0 } }");
	if (!path) return failures;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("framing off ignores stale crop override", cfg.video0.stab_crop_pct == 0);
	CHECK("framing off ignores stale recenter override",
		cfg.video0.stab_recenter_speed == 0);

	/* Same for zoom presets: a leftover stabCropPct stays inert. */
	path = write_temp_json("{ \"video0\": { \"framing\": \"zoom-2x\", "
		"\"stabCropPct\": 80 } }");
	if (!path) return failures;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("framing zoom ignores stale crop override", cfg.video0.stab_crop_pct == 0);

	/* The never-shipped low/medium/high presets are no longer special-cased;
	 * they are unknown values and fall back to "off" like any other. */
	path = write_temp_json("{ \"video0\": { \"framing\": \"medium\" } }");
	if (!path) return failures;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("framing retired low/med/high off", strcmp(cfg.video0.framing, "off") == 0);
	CHECK("framing retired no stab", cfg.video0.stab_crop_pct == 0);

	/* Unknown preset falls back to off (all derived fields cleared). */
	path = write_temp_json("{ \"video0\": { \"framing\": \"bogus\" } }");
	if (!path) return failures;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("framing bogus off", strcmp(cfg.video0.framing, "off") == 0);
	CHECK("framing bogus no zoom", cfg.video0.zoom_pct == 0.0);
	CHECK("framing bogus no stab", cfg.video0.stab_crop_pct == 0);

	/* stab_accuracy: defaults to "auto"; parses the camelCase key under a stab
	 * preset; an unrecognised value falls back to "auto" (lenient load). */
	venc_config_defaults(&cfg);
	CHECK("stab_accuracy default auto",
		strcmp(cfg.video0.stab_accuracy, "auto") == 0);

	path = write_temp_json("{ \"video0\": { \"framing\": \"stab\", "
		"\"stabAccuracy\": \"medium\" } }");
	if (!path) return failures;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("stab_accuracy load medium",
		strcmp(cfg.video0.stab_accuracy, "medium") == 0);

	path = write_temp_json("{ \"video0\": { \"framing\": \"stab\", "
		"\"stabAccuracy\": \"ludicrous\" } }");
	if (!path) return failures;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("stab_accuracy bad value -> auto",
		strcmp(cfg.video0.stab_accuracy, "auto") == 0);

	return failures;
}

static int test_resolution_aliases(void)
{
	int failures = 0;

	/* 720p alias */
	const char *json_720 = "{ \"video0\": { \"size\": \"720p\" } }";
	char *path = write_temp_json(json_720);
	VencConfig cfg;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("720p_width", cfg.video0.width == 1280);
	CHECK("720p_height", cfg.video0.height == 720);

	/* 1080p alias */
	const char *json_1080 = "{ \"video0\": { \"size\": \"1080p\" } }";
	path = write_temp_json(json_1080);
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("1080p_width", cfg.video0.width == 1920);
	CHECK("1080p_height", cfg.video0.height == 1080);

	/* auto alias */
	const char *json_auto = "{ \"video0\": { \"size\": \"auto\" } }";
	path = write_temp_json(json_auto);
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("auto_width", cfg.video0.width == 0);
	CHECK("auto_height", cfg.video0.height == 0);

	/* WxH format */
	const char *json_wxh = "{ \"video0\": { \"size\": \"640x480\" } }";
	path = write_temp_json(json_wxh);
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("wxh_width", cfg.video0.width == 640);
	CHECK("wxh_height", cfg.video0.height == 480);

	return failures;
}

static int test_rotate_180(void)
{
	int failures = 0;
	const char *json = "{ \"image\": { \"rotate\": 180 } }";
	char *path = write_temp_json(json);
	VencConfig cfg;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);

	CHECK("rotate180_mirror", cfg.image.mirror == true);
	CHECK("rotate180_flip", cfg.image.flip == true);
	/* rotate is write-only: it decomposes and is then cleared.  Left at 180 it
	 * re-decomposed on every parse and won permanently -- and because save()
	 * writes the field back, a craft that once carried rotate:180 could never
	 * turn image.mirror off again.  Checking the decomposition alone cannot
	 * see that, which is why this assertion is here. */
	CHECK("rotate180_cleared_after_decompose", cfg.image.rotate == 0);
	return failures;
}

/* The failure the cleared rotate prevents, end to end: turn mirror off on a
 * config that arrived with rotate:180, save, reload.  Before the fix the value
 * came back true, silently, across the restart the MUT_RESTART write had just
 * asked for. */
static int test_rotate_180_does_not_resurrect(void)
{
	int failures = 0;
	const char *json = "{ \"image\": { \"rotate\": 180 } }";
	char *path = write_temp_json(json);
	VencConfig cfg;

	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	CHECK("rotate_resurrect_loaded_mirror", cfg.image.mirror == true);

	cfg.image.mirror = false;
	CHECK("rotate_resurrect_saved", venc_config_save(path, &cfg) == 0);

	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	CHECK("rotate_resurrect_mirror_stays_off", cfg.image.mirror == false);
	CHECK("rotate_resurrect_flip_untouched", cfg.image.flip == true);

	unlink(path);
	free(path);
	return failures;
}

static int test_sample_config_file(void)
{
	int failures = 0;
	VencConfig cfg;
	venc_config_defaults(&cfg);

	/* Load the shipped sample config relative to the test binary's
	 * expected working directory (repo root) */
	int ret = venc_config_load("config/waybeam.default.json", &cfg);
	CHECK("sample_load_ok", ret == 0);
	if (ret != 0) return failures;

	CHECK("sample_fps_30", cfg.video0.fps == 60);
	{
		/* The shipped JSON and the compiled seed must agree on every
		 * field the JSON states.  They silently diverged on qpDelta
		 * (seed -4, JSON -12), so a config that OMITTED the key got a
		 * different value from one that shipped it, and
		 * GET /api/v1/defaults reverted crafts to the seed.  Comparing
		 * against a fresh defaults struct catches the next such split
		 * instead of restating a constant. */
		VencConfig seeded;
		venc_config_defaults(&seeded);
		CHECK("sample_qp_delta_matches_seed",
			cfg.video0.qp_delta == seeded.video0.qp_delta);
		CHECK("sample_bitrate_matches_seed",
			cfg.video0.bitrate == seeded.video0.bitrate);
		CHECK("sample_web_port_matches_seed",
			cfg.system.web_port == seeded.system.web_port);
		/* roiQp is stated by BOTH shipped JSONs, so a bound change that
		 * moves the compiled seed and forgets them ships a default
		 * outside the range it documents.  Read the RAW json, not the
		 * loaded struct: load_fpv() clamps into range, so a stale -25
		 * loads as -20 and a comparison against the seed passes while
		 * the shipped file is still wrong.  Mutation-checked -- against
		 * the loaded value, seeding -25 into either file produced no
		 * failure here at all. */
		static const char *const shipped[] = {
			"config/waybeam.default.json",
			"config/waybeam.default.maruko.json",
		};
		size_t si;

		for (si = 0; si < sizeof(shipped) / sizeof(shipped[0]); si++) {
			char name[96];
			char *text = NULL;
			cJSON *root, *fpv, *qp;
			long len;
			FILE *jf = fopen(shipped[si], "rb");

			snprintf(name, sizeof(name), "shipped_open_%zu", si);
			CHECK(name, jf != NULL);
			if (!jf)
				continue;
			fseek(jf, 0, SEEK_END);
			len = ftell(jf);
			fseek(jf, 0, SEEK_SET);
			if (len > 0 && (text = malloc((size_t)len + 1)) != NULL) {
				size_t got = fread(text, 1, (size_t)len, jf);
				text[got] = '\0';
			}
			fclose(jf);
			snprintf(name, sizeof(name), "shipped_read_%zu", si);
			CHECK(name, text != NULL);
			if (!text)
				continue;
			root = cJSON_Parse(text);
			free(text);
			snprintf(name, sizeof(name), "shipped_parse_%zu", si);
			CHECK(name, root != NULL);
			if (!root)
				continue;
			fpv = cJSON_GetObjectItem(root, "fpv");
			qp = fpv ? cJSON_GetObjectItem(fpv, "roiQp") : NULL;
			snprintf(name, sizeof(name), "shipped_roi_qp_present_%zu",
				si);
			CHECK(name, qp != NULL && cJSON_IsNumber(qp));
			snprintf(name, sizeof(name),
				"shipped_roi_qp_matches_seed_%zu", si);
			CHECK(name, qp != NULL && cJSON_IsNumber(qp) &&
				(int)cJSON_GetNumberValue(qp) ==
					seeded.fpv.roi_qp);
			cJSON_Delete(root);
		}
	}
	CHECK("sample_enabled", cfg.outgoing.enabled == false);
	CHECK("sample_server", strcmp(cfg.outgoing.server, "") == 0);
	CHECK("sample_stream_mode", strcmp(cfg.outgoing.stream_mode, "rtp") == 0);
	CHECK("sample_bitrate", cfg.video0.bitrate == 8192);
	CHECK("sample_web_port", cfg.system.web_port == 80);
	CHECK("sample_audio_off", cfg.audio.enabled == false);
	CHECK("sample_audio_port", cfg.outgoing.audio_port == 5601);

	return failures;
}

static int test_audio_config(void)
{
	int failures = 0;
	const char *json =
		"{"
		"  \"audio\": { \"enabled\": true, \"sampleRate\": 48000,"
		"    \"channels\": 2, \"codec\": \"g711a\", \"volume\": 50 },"
		"  \"outgoing\": { \"audioPort\": 5700 }"
		"}";

	char *path = write_temp_json(json);
	CHECK("audio_tmpfile", path != NULL);
	if (!path) return failures;

	VencConfig cfg;
	venc_config_defaults(&cfg);
	int ret = venc_config_load(path, &cfg);
	unlink(path);
	free(path);

	CHECK("audio_load_ok", ret == 0);
	CHECK("audio_enabled", cfg.audio.enabled == true);
	CHECK("audio_rate", cfg.audio.sample_rate == 48000);
	CHECK("audio_channels", cfg.audio.channels == 2);
	CHECK("audio_codec", strcmp(cfg.audio.codec, "g711a") == 0);
	CHECK("audio_volume", cfg.audio.volume == 50);
	CHECK("audio_port", cfg.outgoing.audio_port == 5700);

	return failures;
}

static int test_audio_port_record_only(void)
{
	int failures = 0;
	const char *json =
		"{ \"outgoing\": { \"audioPort\": -1 } }";

	char *path = write_temp_json(json);
	CHECK("audio_ro_tmpfile", path != NULL);
	if (!path) return failures;

	VencConfig cfg;
	venc_config_defaults(&cfg);
	int ret = venc_config_load(path, &cfg);
	unlink(path);
	free(path);

	CHECK("audio_ro_load_ok", ret == 0);
	/* -1 must survive (not wrap to 65535) so the runtime can route it to
	 * the record-only path. */
	CHECK("audio_ro_neg", cfg.outgoing.audio_port == -1);

	/* Round-trip: render to JSON and reload, value preserved (not wrapped). */
	char *rendered = venc_config_to_json_string(&cfg);
	CHECK("audio_ro_render_ok", rendered != NULL);
	if (rendered) {
		char *path2 = write_temp_json(rendered);
		free(rendered);
		if (path2) {
			VencConfig cfg2;
			venc_config_defaults(&cfg2);
			ret = venc_config_load(path2, &cfg2);
			unlink(path2);
			free(path2);
			CHECK("audio_ro_reload_ok", ret == 0);
			CHECK("audio_ro_roundtrip", cfg2.outgoing.audio_port == -1);
		}
	}

	return failures;
}

static int test_audio_volume_clamping(void)
{
	int failures = 0;

	/* Volume > 100 should clamp to 100 */
	const char *json_high = "{ \"audio\": { \"volume\": 150 } }";
	char *path = write_temp_json(json_high);
	VencConfig cfg;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("vol_clamped_100", cfg.audio.volume == 100);

	/* Volume < 0 should clamp to 0 */
	const char *json_low = "{ \"audio\": { \"volume\": -10 } }";
	path = write_temp_json(json_low);
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("vol_clamped_0", cfg.audio.volume == 0);

	return failures;
}

static int test_audio_channel_clamping(void)
{
	int failures = 0;

	const char *json = "{ \"audio\": { \"channels\": 5 } }";
	char *path = write_temp_json(json);
	VencConfig cfg;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("ch_clamped_2", cfg.audio.channels == 2);

	return failures;
}

static int test_audio_roundtrip(void)
{
	int failures = 0;
	VencConfig cfg;
	venc_config_defaults(&cfg);
	cfg.audio.enabled = true;
	cfg.audio.sample_rate = 48000;
	cfg.audio.channels = 2;
	snprintf(cfg.audio.codec, sizeof(cfg.audio.codec), "g711u");
	cfg.audio.volume = 60;
	cfg.outgoing.audio_port = 5700;

	char *json = venc_config_to_json_string(&cfg);
	CHECK("audio_serialize_ok", json != NULL);
	if (!json) return failures;

	char *path = write_temp_json(json);
	free(json);
	CHECK("audio_rt_tmpfile", path != NULL);
	if (!path) return failures;

	VencConfig cfg2;
	venc_config_defaults(&cfg2);
	int ret = venc_config_load(path, &cfg2);
	unlink(path);
	free(path);

	CHECK("audio_rt_load_ok", ret == 0);
	CHECK("audio_rt_enabled", cfg2.audio.enabled == true);
	CHECK("audio_rt_rate", cfg2.audio.sample_rate == 48000);
	CHECK("audio_rt_ch", cfg2.audio.channels == 2);
	CHECK("audio_rt_codec", strcmp(cfg2.audio.codec, "g711u") == 0);
	CHECK("audio_rt_vol", cfg2.audio.volume == 60);
	CHECK("audio_rt_port", cfg2.outgoing.audio_port == 5700);

	return failures;
}

/* Self-policing round-trip: load config/waybeam.default.json, save it via
 * venc_config_save (which uses the hand-rolled pretty printer), and assert
 * the saved bytes are byte-equal to the original.  Any future config field
 * added to the struct/parser/serializer but missing from the printer (or
 * the default file) will trip this test. */
static int test_save_layout_byte_equal(void)
{
	int failures = 0;
	VencConfig cfg;
	venc_config_defaults(&cfg);

	const char *src_path = "config/waybeam.default.json";
	int ret = venc_config_load(src_path, &cfg);
	CHECK("layout_load_default_ok", ret == 0);
	if (ret != 0) return failures;

	char tmp_path[] = "/tmp/venc_layout_test_XXXXXX";
	int fd = mkstemp(tmp_path);
	CHECK("layout_mkstemp_ok", fd >= 0);
	if (fd < 0) return failures;
	close(fd);

	int save_rc = venc_config_save(tmp_path, &cfg);
	CHECK("layout_save_ok", save_rc == 0);
	if (save_rc != 0) { unlink(tmp_path); return failures; }

	FILE *fa = fopen(src_path, "rb");
	FILE *fb = fopen(tmp_path, "rb");
	CHECK("layout_open_files", fa && fb);
	if (!fa || !fb) {
		if (fa) fclose(fa);
		if (fb) fclose(fb);
		unlink(tmp_path);
		return failures;
	}

	fseek(fa, 0, SEEK_END); long sa = ftell(fa); fseek(fa, 0, SEEK_SET);
	fseek(fb, 0, SEEK_END); long sb = ftell(fb); fseek(fb, 0, SEEK_SET);
	CHECK("layout_size_equal", sa == sb);

	if (sa == sb && sa > 0) {
		char *ba = malloc((size_t)sa);
		char *bb = malloc((size_t)sb);
		if (ba && bb) {
			fread(ba, 1, (size_t)sa, fa);
			fread(bb, 1, (size_t)sb, fb);
			int eq = memcmp(ba, bb, (size_t)sa) == 0;
			CHECK("layout_byte_equal", eq);
			if (!eq) {
				/* Print the first divergence to ease debugging */
				for (long i = 0; i < sa; i++) {
					if (ba[i] != bb[i]) {
						fprintf(stderr,
						    "  layout diff at byte %ld: "
						    "expected 0x%02x got 0x%02x\n",
						    i, (unsigned char)ba[i],
						    (unsigned char)bb[i]);
						break;
					}
				}
			}
		}
		free(ba); free(bb);
	}

	fclose(fa); fclose(fb);
	unlink(tmp_path);
	return failures;
}

/* Populated-config round trip: load defaults, mutate string and numeric
 * fields across every section (escapes, fractional doubles, large ints),
 * save via the pretty printer, parse the result with cJSON.  This catches
 * is_last comma bugs in render_<section> helpers — when a developer adds a
 * field at the end of a section and forgets to flip the previous field's
 * is_last marker, the produced file gets a trailing comma and cJSON_Parse
 * fails.  Also exercises the JSON-escape and float-format paths that
 * test_save_layout_byte_equal does not. */
static int test_save_layout_populated_round_trip(void)
{
	int failures = 0;
	VencConfig cfg;
	venc_config_defaults(&cfg);

	int ret = venc_config_load("config/waybeam.default.json", &cfg);
	CHECK("layout_populated_load_default_ok", ret == 0);
	if (ret != 0) return failures;

	/* Hammer string fields with characters that exercise every escape
	 * branch in pp_string (quote, backslash, control chars) and a UTF-8
	 * byte sequence to confirm raw passthrough above 0x7F. */
	const char *poison = "x\"\\\b\f\n\r\t\x01""y\xe2\x9c\x93z";
	snprintf(cfg.isp.sensor_bin, sizeof(cfg.isp.sensor_bin), "%s", poison);
	snprintf(cfg.outgoing.server, sizeof(cfg.outgoing.server), "%s", poison);
	snprintf(cfg.record.dir, sizeof(cfg.record.dir), "%s", poison);
	snprintf(cfg.record.server, sizeof(cfg.record.server), "%s", poison);

	/* Force pp_double to take the fractional %1.15g / %1.17g paths. */
	cfg.video0.gop_size = 0.123456789012345;
	cfg.record.gop_size = 2.5;

	char tmp_path[] = "/tmp/venc_layout_pop_XXXXXX";
	int fd = mkstemp(tmp_path);
	CHECK("layout_populated_mkstemp_ok", fd >= 0);
	if (fd < 0) return failures;
	close(fd);

	int save_rc = venc_config_save(tmp_path, &cfg);
	CHECK("layout_populated_save_ok", save_rc == 0);
	if (save_rc != 0) { unlink(tmp_path); return failures; }

	FILE *f = fopen(tmp_path, "rb");
	CHECK("layout_populated_open_saved", f != NULL);
	if (!f) { unlink(tmp_path); return failures; }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)sz + 1);
	if (buf) {
		fread(buf, 1, (size_t)sz, f);
		buf[sz] = '\0';
	}
	fclose(f);

	if (buf) {
		cJSON *root = cJSON_Parse(buf);
		CHECK("layout_populated_parses_as_json", root != NULL);
		if (!root) {
			const char *err = cJSON_GetErrorPtr();
			fprintf(stderr,
				"  cJSON_Parse failed near: %.40s\n",
				err ? err : "(null)");
		}
		if (root) cJSON_Delete(root);
		free(buf);
	}

	/* Round-trip back into VencConfig and re-save; second save must be
	 * byte-equal to the first (printer is deterministic over identical
	 * input). */
	VencConfig cfg2;
	venc_config_defaults(&cfg2);
	int load_rc = venc_config_load(tmp_path, &cfg2);
	CHECK("layout_populated_reload_ok", load_rc == 0);

	char tmp2[] = "/tmp/venc_layout_pop2_XXXXXX";
	int fd2 = mkstemp(tmp2);
	if (fd2 >= 0) {
		close(fd2);
		int rc2 = venc_config_save(tmp2, &cfg2);
		CHECK("layout_populated_resave_ok", rc2 == 0);
		if (rc2 == 0) {
			FILE *fa = fopen(tmp_path, "rb");
			FILE *fb = fopen(tmp2, "rb");
			if (fa && fb) {
				fseek(fa, 0, SEEK_END); long sa = ftell(fa);
				fseek(fb, 0, SEEK_END); long sb = ftell(fb);
				CHECK("layout_populated_resave_size_eq", sa == sb);
				if (sa == sb && sa > 0) {
					fseek(fa, 0, SEEK_SET);
					fseek(fb, 0, SEEK_SET);
					char *ba = malloc((size_t)sa);
					char *bb = malloc((size_t)sb);
					if (ba && bb) {
						fread(ba, 1, (size_t)sa, fa);
						fread(bb, 1, (size_t)sb, fb);
						CHECK("layout_populated_resave_byte_eq",
						    memcmp(ba, bb, (size_t)sa) == 0);
					}
					free(ba); free(bb);
				}
			}
			if (fa) fclose(fa);
			if (fb) fclose(fb);
		}
		unlink(tmp2);
	}

	unlink(tmp_path);
	return failures;
}

/* Guards the step-5 config export: a field settable via /api/v1/set but
 * missing from venc_config_to_json_string() is invisible in /api/v1/config
 * and the WebUI.  Load detect.* from JSON, render via the API serializer, and
 * confirm the camelCase keys survive a render->reload round-trip. */
static int test_detect_export_roundtrip(void)
{
	int failures = 0;
	const char *json =
		"{ \"detect\": { \"enabled\": true, "
		"\"plugin\": \"/root/libwaybeam_detect.so\", "
		"\"modelPath\": \"/root/models/m.img\", "
		"\"inferInterval\": 3, \"osd\": false, "
		"\"confThresh\": 0.2, \"nmsIou\": 0.5, "
		"\"netWidth\": 800, \"netHeight\": 448, "
		"\"modelId\": 1 } }";

	char *path = write_temp_json(json);
	CHECK("detect_ro_tmpfile", path != NULL);
	if (!path) return failures;

	VencConfig cfg;
	venc_config_defaults(&cfg);
	int ret = venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("detect_load_ok", ret == 0);
	CHECK("detect_enabled", cfg.detect.enabled == true);
	CHECK("detect_interval", cfg.detect.infer_interval == 3);
	/* A typo'd key here would silently leave the plugin on its 0.40
	 * default, which is precisely the threshold INT8 quantization makes
	 * too high -- so assert the parse, not just the round-trip. */
	CHECK("detect_conf_thresh", cfg.detect.conf_thresh > 0.199f &&
		cfg.detect.conf_thresh < 0.201f);
	CHECK("detect_nms_iou", cfg.detect.nms_iou > 0.499f &&
		cfg.detect.nms_iou < 0.501f);
	CHECK("detect_net_width", cfg.detect.net_width == 800);
	CHECK("detect_net_height", cfg.detect.net_height == 448);
	/* Defaulting this to 0 relabels every box as VisDrone "pedestrian",
	 * so the parse has to actually take effect, not just round-trip. */
	CHECK("detect_model_id", cfg.detect.model_id == 1);

	char *rendered = venc_config_to_json_string(&cfg);
	CHECK("detect_render_ok", rendered != NULL);
	if (rendered) {
		/* step-5 export must include the camelCase keys */
		CHECK("detect_export_has_plugin",
			strstr(rendered, "\"plugin\"") != NULL);
		CHECK("detect_export_has_modelPath",
			strstr(rendered, "\"modelPath\"") != NULL);
		CHECK("detect_export_has_inferInterval",
			strstr(rendered, "\"inferInterval\"") != NULL);
		CHECK("detect_export_has_confThresh",
			strstr(rendered, "\"confThresh\"") != NULL);
		CHECK("detect_export_has_netWidth",
			strstr(rendered, "\"netWidth\"") != NULL);
		CHECK("detect_export_has_modelId",
			strstr(rendered, "\"modelId\"") != NULL);

		char *path2 = write_temp_json(rendered);
		free(rendered);
		if (path2) {
			VencConfig cfg2;
			venc_config_defaults(&cfg2);
			ret = venc_config_load(path2, &cfg2);
			unlink(path2);
			free(path2);
			CHECK("detect_reload_ok", ret == 0);
			CHECK("detect_roundtrip_enabled",
				cfg2.detect.enabled == true);
			CHECK("detect_roundtrip_interval",
				cfg2.detect.infer_interval == 3);
			CHECK("detect_roundtrip_model",
				strcmp(cfg2.detect.model_path,
					"/root/models/m.img") == 0);
		}
	}

	return failures;
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int test_venc_config(void)
{
	int failures = 0;
	failures += test_defaults();
	failures += test_load_full_json();
	failures += test_load_partial_json();
	failures += test_slice_count();
	failures += test_load_missing_file();
	failures += test_load_bad_json();
	failures += test_load_qp_bounds_validation();
	failures += test_uri_parsing();
	failures += test_roundtrip();
	failures += test_overclock_clamping();
	failures += test_noise_level_clamping();
	failures += test_framing_presets();
	failures += test_resolution_aliases();
	failures += test_rotate_180();
	failures += test_rotate_180_does_not_resurrect();
	failures += test_sample_config_file();
	failures += test_audio_config();
	failures += test_audio_volume_clamping();
	failures += test_audio_channel_clamping();
	failures += test_audio_roundtrip();
	failures += test_audio_port_record_only();
	failures += test_save_layout_byte_equal();
	failures += test_save_layout_populated_round_trip();
	failures += test_resilience_preset_expansion();
	failures += test_frame_gate_negative_not_clamped();
	failures += test_detect_export_roundtrip();
	return failures;
}
