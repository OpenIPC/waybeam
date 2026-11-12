#include "cv610_modes.h"
#include "cv610_encoder_config.h"
#include "venc_api.h"
#include "venc_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect_valid(const char *name, VencConfig *cfg, int valid)
{
	const char *error = venc_api_validate_loaded_config(cfg);
	int ok = valid ? error == NULL : error != NULL;

	printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", name,
		error ? ": " : "", error ? error : "");
	return ok ? 0 : 1;
}

static int expect(const char *name, int ok)
{
	printf("  %s  %s\n", ok ? "PASS" : "FAIL", name);
	return ok ? 0 : 1;
}

/* The mode tests below are about the IMX662 table, which every assertion was
 * written against; main() also clears SENSOR so validation sees it too. */
static const Cv610Sensor *g_imx662;

/* The first row at exactly this rate, or NULL: what the table lists, as
 * opposed to what cv610_mode_select() would substitute. */
static const Cv610SensorMode *mode_at_fps(const Cv610Sensor *s, uint32_t fps)
{
	size_t i;

	for (i = 0; i < s->mode_count; i++)
		if (s->modes[i].fps == fps)
			return &s->modes[i];
	return NULL;
}

/* The mode table is the single source of truth for video0.size, video0.fps,
 * the MIPI bit depth, and the sensor MCLK.  The clock is the one that fails
 * silently on hardware (wrong clock => rate scales by the ratio, with every
 * status endpoint still reporting nominal), so assert it explicitly. */
static int test_mode_table(void)
{
	const Cv610SensorMode *modes;
	const Cv610SensorMode *m;
	size_t count = 0;
	size_t i;
	int failures = 0;

	modes = g_imx662->modes;
	count = g_imx662->mode_count;
	failures += expect("modes_table_non_empty", modes != NULL && count > 0);

	for (i = 0; i < count; i++) {
		m = mode_at_fps(g_imx662, modes[i].fps);
		failures += expect("modes_every_entry_resolves", m == &modes[i]);
		failures += expect("modes_clock_set", modes[i].sensor_clock_hz != 0);
		failures += expect("modes_raw_bit_sane",
			modes[i].raw_bit == 10 || modes[i].raw_bit == 12);
	}

	failures += expect("modes_reject_unknown_fps",
		mode_at_fps(g_imx662, 45) == NULL);

	/* Output geometry is checked against the mode, not matched to it: VPSS
	 * scales the capture down to whatever video0.size asks for. */
	m = &modes[0];
	failures += expect("out_auto_ok",
		cv610_mode_check_output(m, 0, 0) == NULL);
	failures += expect("out_native_ok",
		cv610_mode_check_output(m, m->width, m->height) == NULL);
	failures += expect("out_720p_ok",
		cv610_mode_check_output(m, 1280, 720) == NULL);
	failures += expect("out_reject_half_set",
		cv610_mode_check_output(m, m->width, 0) != NULL);
	failures += expect("out_reject_unaligned",
		cv610_mode_check_output(m, 1281, 720) != NULL);
	failures += expect("out_reject_tiny",
		cv610_mode_check_output(m, 64, 64) != NULL);
	/* VPSS will happily upscale; refuse it — it spends link bandwidth to
	 * carry no extra detail. */
	failures += expect("out_reject_upscale",
		cv610_mode_check_output(m, m->width + 8, m->height) != NULL);
	failures += expect("out_reject_null_mode",
		cv610_mode_check_output(NULL, 1280, 720) != NULL);

	{
		uint32_t w = 0, h = 0;

		cv610_mode_resolve_output(m, 0, 0, &w, &h);
		failures += expect("out_resolve_auto_is_capture",
			w == m->width && h == m->height);
		cv610_mode_resolve_output(m, 1280, 720, &w, &h);
		failures += expect("out_resolve_explicit", w == 1280 && h == 720);
	}

	/* Control: the 100 fps mode must not share the 30 fps mode's clock.
	 * If these ever converge the whole table is suspect. */
	{
		const Cv610SensorMode *slow = mode_at_fps(g_imx662, 30);
		const Cv610SensorMode *fast = mode_at_fps(g_imx662, 100);

		failures += expect("modes_1080p30_and_1080p100_present",
			slow != NULL && fast != NULL);
		if (slow && fast) {
			failures += expect("modes_100fps_uses_27mhz",
				fast->sensor_clock_hz == 27000000u);
			failures += expect("modes_30fps_uses_37125khz",
				slow->sensor_clock_hz == 37125000u);
		}
	}
	return failures;
}

/* Every row asserts the index contract too: out_index must address the very
 * entry that was returned, because /api/v1/modes publishes that index and the
 * ground's mode catalog addresses modes by it. */
static int select_ok(const char *name, int forced, uint32_t fps,
	uint32_t want_fps)
{
	const Cv610SensorMode *table = g_imx662->modes;
	int idx = 999;
	const Cv610SensorMode *m = cv610_mode_select(g_imx662, forced, fps,
		0, 0, &idx);

	return expect(name, m != NULL && m->fps == want_fps &&
		idx >= 0 && m == &table[idx]);
}

/* Derived rather than hardcoded so the tests below keep meaning what they say
 * if the table is ever reordered or extended. */
static int fastest_mode_index(void)
{
	const Cv610SensorMode *table = g_imx662->modes;
	size_t count = g_imx662->mode_count;
	size_t i;
	int best = 0;

	for (i = 1; i < count; i++)
		if (table[i].fps > table[best].fps)
			best = (int)i;
	return best;
}

static int select_rejects(const char *name, int forced, uint32_t fps)
{
	int idx = 999;
	const Cv610SensorMode *m = cv610_mode_select(g_imx662, forced, fps,
		0, 0, &idx);

	return expect(name, m == NULL && idx == -1);
}

/* sensor.mode and video0.fps resolve to one mode the way SigmaStar's
 * sensor_select() resolves them: a forced index wins and must exist, else the
 * rate is a target rather than a command.  One row diverges deliberately —
 * see cv610_mode_select(). */
static int test_mode_select(void)
{
	const Cv610SensorMode *table = g_imx662->modes;
	size_t count = g_imx662->mode_count;
	int failures = 0;

	/* A forced index beats the requested rate outright. */
	failures += select_ok("select_forced_beats_fps", 1, 100, table[1].fps);
	failures += select_ok("select_forced_last", (int)count - 1, 30,
		table[count - 1].fps);
	/* Past the table it is an error, not a fallback to something plausible. */
	failures += select_rejects("select_forced_out_of_range", (int)count, 100);

	/* Any negative index means auto, as forced_pad/forced_mode do on
	 * SigmaStar — only >= 0 is a command. */
	failures += select_ok("select_negative_is_auto", -5, 60, 60);

	failures += select_ok("select_auto_exact", -1, 90, 90);
	/* Between modes rounds UP: never deliver fewer frames than asked while a
	 * mode that can keep up exists. */
	failures += select_ok("select_auto_rounds_up", -1, 45, 60);
	failures += select_ok("select_auto_just_below_top", -1, 99, 100);
	failures += select_ok("select_auto_below_all", -1, 1, 30);
	/* Above every mode clamps to the FASTEST.  A literal port of SigmaStar's
	 * sensor_mode_cost tie-break would answer 30 here, which is the wart this
	 * table exists to pin down. */
	failures += select_ok("select_auto_clamps_to_fastest", -1, 120, 100);
	failures += select_ok("select_auto_clamps_far_above", -1, 100000, 100);
	/* No target at all: reject rather than invent a rate. */
	failures += select_rejects("select_auto_zero_fps", -1, 0);

	return failures;
}

/* The sensor table: what $SENSOR selects, and the IMX335 rows' contract with
 * the plugin, which picks its mode from the capture size and rate alone. */
static int test_sensor_table(void)
{
	const Cv610Sensor *imx335 = cv610_sensor_find("imx335");
	int idx = -1;
	size_t i, j;
	int failures = 0;

	failures += expect("sns_find_imx662", g_imx662 != NULL &&
		strcmp(g_imx662->name, "imx662") == 0 && g_imx662->sns_id == 662);
	failures += expect("sns_find_imx335", imx335 != NULL &&
		imx335->sns_id == 335 && imx335->mode_count == 5);
	failures += expect("sns_find_ignores_case",
		cv610_sensor_find("IMX335") == imx335);
	failures += expect("sns_find_unknown",
		cv610_sensor_find("imx999") == NULL &&
		cv610_sensor_find("") == NULL && cv610_sensor_find(NULL) == NULL);
	/* Waybeam's own IMX662 plugin lives outside /usr/lib/sensors, so the
	 * image's stock copy (which refuses 100 fps) cannot replace it. */
	failures += expect("sns_imx662_plugin_is_ours",
		strcmp(g_imx662->plugin_path,
			"/usr/lib/cv610/libsns_imx662.so") == 0);
	failures += expect("sns_imx335_plugin_is_the_images",
		strcmp(imx335->plugin_path,
			"/usr/lib/sensors/libsns_imx335.so") == 0 &&
		strcmp(imx335->obj_symbol, "g_sns_imx335_obj") == 0);

	/* Every IMX335 mode runs on the clock sys_config feeds an imx335. */
	for (j = 0; j < imx335->mode_count; j++)
		failures += expect("sns_imx335_mode_sane",
			imx335->modes[j].sensor_clock_hz == 37125000u &&
			(imx335->modes[j].raw_bit == 10 ||
			 imx335->modes[j].raw_bit == 12));

	/* The plugin tells rows apart by size and rate, so no two rows may share
	 * both. */
	for (i = 0; i < imx335->mode_count; i++) {
		for (j = i + 1; j < imx335->mode_count; j++) {
			const Cv610SensorMode *a = &imx335->modes[i];
			const Cv610SensorMode *b = &imx335->modes[j];

			failures += expect("sns_imx335_rows_distinct",
				a->width != b->width || a->height != b->height ||
				a->fps != b->fps);
		}
	}

	/* 30 fps has two rows; the full 5 MP RAW12 one is listed first. */
	failures += expect("sns_imx335_30_is_full_frame",
		cv610_mode_select(imx335, -1, 30, 0, 0, &idx) ==
			&imx335->modes[0] &&
		idx == 0 && imx335->modes[0].height == 1944 &&
		imx335->modes[0].raw_bit == 12);
	failures += expect("sns_imx335_40_full_raw10",
		cv610_mode_select(imx335, -1, 40, 0, 0, &idx) ==
			&imx335->modes[3]);
	failures += expect("sns_imx335_90_is_1080_crop",
		cv610_mode_select(imx335, -1, 90, 0, 0, &idx) ==
			&imx335->modes[2] &&
		imx335->modes[2].width == 1920 && imx335->modes[2].height == 1080);
	failures += expect("sns_imx335_forced_1520p",
		cv610_mode_select(imx335, 4, 30, 0, 0, &idx) ==
			&imx335->modes[4] &&
		idx == 4 && imx335->modes[4].height == 1520);

	/* The rate is matched among modes that capture the encode size, as on
	 * SigmaStar: 60 fps at 720p is the binned row, at 1080p the binned row
	 * would be an upscale, so the 90 fps crop -- the slowest that fits and
	 * keeps up -- runs instead. */
	failures += expect("sns_imx335_720p60_binned",
		cv610_mode_select(imx335, -1, 60, 1280, 720, &idx) ==
			&imx335->modes[1] && idx == 1);
	failures += expect("sns_imx335_1080p60_uses_crop90",
		cv610_mode_select(imx335, -1, 60, 1920, 1080, &idx) ==
			&imx335->modes[2] && idx == 2);
	failures += expect("sns_imx335_5mp60_clamps_to_40",
		cv610_mode_select(imx335, -1, 60, 2592, 1944, &idx) ==
			&imx335->modes[3] && idx == 3);
	/* Auto encodes the capture, held to the multiple of 8 an explicit size
	 * needs: only the binned row's 972 moves. */
	{
		uint32_t w = 0, h = 0;

		cv610_mode_resolve_output(&imx335->modes[1], 0, 0, &w, &h);
		failures += expect("sns_imx335_binned_auto_aligned",
			w == 1296 && h == 968);
		cv610_mode_resolve_output(&imx335->modes[0], 0, 0, &w, &h);
		failures += expect("sns_imx335_full_auto_unchanged",
			w == 2592 && h == 1944);
	}
	/* Nothing captures it: the rate alone decides, and the size check is
	 * left to reject it. */
	failures += expect("sns_imx335_no_fit_falls_back_to_rate",
		cv610_mode_select(imx335, -1, 30, 4000, 3000, &idx) ==
			&imx335->modes[0] && idx == 0);

	/* SENSOR picks the row; anything it cannot name is IMX662. */
	unsetenv("SENSOR");
	failures += expect("sns_current_unset_is_imx662",
		cv610_sensor_current() == g_imx662);
	setenv("SENSOR", "", 1);
	failures += expect("sns_current_empty_is_imx662",
		cv610_sensor_current() == g_imx662);
	setenv("SENSOR", "imx335", 1);
	failures += expect("sns_current_imx335",
		cv610_sensor_current() == imx335);
	setenv("SENSOR", "sc4336p", 1);
	failures += expect("sns_current_unsupported_is_imx662",
		cv610_sensor_current() == g_imx662);
	unsetenv("SENSOR");
	return failures;
}

/* Validation reads the table of the sensor SENSOR names, so an index that
 * exists only on IMX335 is valid there and an error on IMX662. */
static int test_validate_imx335(void)
{
	VencConfig cfg;
	int failures = 0;

	venc_config_defaults(&cfg);
	cfg.video0.fps = 30;
	cfg.video0.width = 1920;
	cfg.video0.height = 1080;
	cfg.sensor.mode = 4;
	failures += expect_valid("imx335_mode4_rejected_on_imx662", &cfg, 0);
	setenv("SENSOR", "imx335", 1);
	failures += expect_valid("imx335_mode4_accepted", &cfg, 1);
	cfg.sensor.mode = -1;
	cfg.video0.fps = 90;
	failures += expect_valid("imx335_1080p90_crop", &cfg, 1);
	/* 1080p60 resolves to the 1080p90 crop rather than to the binned mode,
	 * which would be an upscale; past every capture it is still an error. */
	cfg.video0.fps = 60;
	failures += expect_valid("imx335_1080p60_via_crop", &cfg, 1);
	cfg.video0.width = 1280;
	cfg.video0.height = 720;
	failures += expect_valid("imx335_720p60_binned", &cfg, 1);
	cfg.video0.width = 2600;
	cfg.video0.height = 1952;
	failures += expect_valid("imx335_reject_upscale", &cfg, 0);
	unsetenv("SENSOR");
	return failures;
}

static int test_encoder_config(void)
{
	VencConfig cfg;
	Cv610EncoderConfig enc;
	int failures = 0;

	venc_config_defaults(&cfg);
	cfg.video0.slice_count = 1;
	failures += expect("enc_off_derives",
		cv610_encoder_config_derive(&cfg, 1920, 1080, 100, &enc) == 0);
	failures += expect("enc_off_features_disabled",
		!enc.intra.enabled && !enc.ref.enabled && !enc.slice.enabled);
	failures += expect("enc_off_slice_defaults",
		enc.slice.total_lcu_rows == 34 && enc.slice.split_size == 1 &&
		enc.slice.expected_count == 1);

	(void)venc_config_apply_resilience_preset("racing", &cfg.video0);
	failures += expect("enc_racing_derives",
		cv610_encoder_config_derive(&cfg, 1920, 1080, 100, &enc) == 0);
	/* COLUMN refresh: the sweep covers the WIDTH, so at 1920 the extent is
	 * 60 units of 32 px, not the 34 that the height would give. "fast" allows
	 * 150 ms = 15 frames at 100 fps, so ceil(60/15) = 4 units per P frame. */
	failures += expect("enc_racing_gdr",
		enc.intra.enabled && enc.intra.refresh_dir == 1 &&
		enc.intra.refresh_num == 4 && enc.intra.request_i_qp == 36 &&
		enc.intra.derived.total_rows == 60);
	failures += expect("enc_racing_sweep_meets_target",
		enc.intra.refresh_num *
			((100u * enc.intra.derived.target_ms + 500u) / 1000u) >=
		enc.intra.derived.total_rows);
	failures += expect("enc_racing_no_refpred", !enc.ref.enabled);

	(void)venc_config_apply_resilience_preset("rally", &cfg.video0);
	failures += expect("enc_rally_derives",
		cv610_encoder_config_derive(&cfg, 1920, 1080, 60, &enc) == 0);
	failures += expect("enc_rally_refpred",
		enc.ref.enabled && enc.ref.base == 1 && enc.ref.enhance == 1 &&
		enc.ref.pred == 1);

	(void)venc_config_apply_resilience_preset("ltr:4", &cfg.video0);
	failures += expect("enc_ltr_derives",
		cv610_encoder_config_derive(&cfg, 1920, 1080, 60, &enc) == 0);
	failures += expect("enc_ltr_refpred_without_gdr",
		!enc.intra.enabled && enc.ref.enabled && enc.ref.base == 1 &&
		enc.ref.enhance == 4 && enc.ref.pred == 0);

	cfg.video0.slice_count = 17;
	failures += expect("enc_slice_17_derives",
		cv610_encoder_config_derive(&cfg, 1920, 1080, 100, &enc) == 0);
	failures += expect("enc_slice_17_geometry",
		enc.slice.enabled && enc.slice.split_mode == 1 &&
		enc.slice.split_size == 2 && enc.slice.expected_count == 17);
	cfg.video0.slice_count = 12;
	(void)cv610_encoder_config_derive(&cfg, 1920, 1080, 100, &enc);
	failures += expect("enc_slice_12_geometry",
		enc.slice.split_size == 3 && enc.slice.expected_count == 12);
	cfg.video0.slice_count = 32;
	(void)cv610_encoder_config_derive(&cfg, 1920, 1080, 100, &enc);
	failures += expect("enc_slice_saturates_at_rows",
		enc.slice.split_size == 2 && enc.slice.expected_count == 17);
	cfg.video0.slice_count = 9;
	(void)cv610_encoder_config_derive(&cfg, 1280, 720, 100, &enc);
	failures += expect("enc_slice_720_quantizes",
		enc.slice.total_lcu_rows == 23 && enc.slice.split_size == 3 &&
		enc.slice.expected_count == 8);

	/* The two axes must not be confused: in one derive call the intra sweep is
	 * width-derived and the slice split is height-derived. At 1280x720 they
	 * differ (40 vs 23 units), so a regression that fed height to the sweep --
	 * the bug this replaced -- fails here rather than silently slowing the
	 * refresh by the aspect ratio. */
	(void)venc_config_apply_resilience_preset("racing", &cfg.video0);
	cfg.video0.slice_count = 4;
	failures += expect("enc_720_axes_derive",
		cv610_encoder_config_derive(&cfg, 1280, 720, 100, &enc) == 0);
	failures += expect("enc_720_intra_uses_width",
		enc.intra.derived.total_rows == 40 && enc.intra.refresh_num == 3);
	failures += expect("enc_720_slices_use_height",
		enc.slice.total_lcu_rows == 23 && enc.slice.expected_count == 4);
	failures += expect("enc_720_sweep_meets_target",
		enc.intra.refresh_num * 15u >= enc.intra.derived.total_rows);

	/* A non-16:9 picture is the sharpest axis check: 1080x1920 portrait swaps
	 * which number is larger, so a height-fed sweep would read 60 here. */
	failures += expect("enc_portrait_derive",
		cv610_encoder_config_derive(&cfg, 1080, 1920, 100, &enc) == 0);
	failures += expect("enc_portrait_intra_uses_width",
		enc.intra.derived.total_rows == 34);
	failures += expect("enc_portrait_slices_use_height",
		enc.slice.total_lcu_rows == 60);

	failures += expect("enc_reject_zero_width",
		cv610_encoder_config_derive(&cfg, 0, 1080, 100, &enc) != 0);

	cfg.video0.slice_count = 0;
	failures += expect("enc_reject_slice_zero",
		cv610_encoder_config_derive(&cfg, 1920, 1080, 100, &enc) != 0);
	cfg.video0.slice_count = VENC_SLICE_COUNT_MAX + 1;
	failures += expect("enc_reject_slice_over_max",
		cv610_encoder_config_derive(&cfg, 1920, 1080, 100, &enc) != 0);

	return failures;
}

int main(void)
{
	VencConfig cfg;
	size_t mode_count;
	int failures = 0;

	/* Run against IMX662 whatever the build host exports. */
	unsetenv("SENSOR");
	g_imx662 = cv610_sensor_find("imx662");
	if (g_imx662 == NULL) {
		printf("  FAIL  sns_imx662_registered\n");
		return 1;
	}
	mode_count = g_imx662->mode_count;

	failures += test_mode_table();
	failures += test_mode_select();
	failures += test_sensor_table();
	failures += test_validate_imx335();
	failures += test_encoder_config();

	venc_config_defaults(&cfg);
	failures += expect_valid("cv610_defaults", &cfg, 1);
	if (venc_config_load("config/waybeam.default.cv610.json", &cfg) != 0) {
		printf("  FAIL  cv610_sample_load\n");
		return 1;
	}
	failures += expect_valid("cv610_sample", &cfg, 1);
	/* The shipped default is what a fresh craft boots with, so pin it.
	 * expect_valid alone cannot tell 1280x720 from any other geometry that
	 * happens to validate. */
	failures += expect("cv610_default_is_1280x720",
		cfg.video0.width == 1280 && cfg.video0.height == 720);
	failures += expect("cv610_default_is_100fps", cfg.video0.fps == 100);
	/* The shipped bitrate is a BOOT SEED, not an operating point: waybeam-link
	 * is the single rate controller and actuates within seconds of the link
	 * coming up.  What the seed has to survive is the worst rung — landing at
	 * the MCS0 no-feedback floor while offering 8 Mbps floods the air, and on
	 * this fleet an over-offered craft has already been measured demoting a
	 * SECOND craft's link on the same channel.  So this pins low on purpose;
	 * it asserted 8000 until that was understood. */
	failures += expect("cv610_default_bitrate_survives_mcs0",
		cfg.video0.bitrate == 2600);
	/* And that it names a real sensor mode rather than only passing the
	 * generic range checks. */
	failures += expect("cv610_default_names_a_mode",
		mode_at_fps(g_imx662, cfg.video0.fps) != NULL);
	/* A rate above every mode is no longer a config error — it selects the
	 * fastest mode.  (This asserted rejection until the selector landed; the
	 * substitution is what replaced it, not the removal of a check.) */
	cfg.video0.fps = 120;
	failures += expect_valid("cv610_accept_fps_above_table", &cfg, 1);
	/* But a config that names no rate at all still is. */
	cfg.video0.fps = 0;
	failures += expect_valid("cv610_reject_fps_zero", &cfg, 0);
	cfg.video0.fps = 60;
	cfg.video0.width = 1280;
	cfg.video0.height = 720;
	failures += expect_valid("cv610_accept_720p_scaled", &cfg, 1);
	cfg.video0.width = 1936;
	cfg.video0.height = 1080;
	failures += expect_valid("cv610_reject_upscale", &cfg, 0);
	/* A size the sensor can produce, at a rate between two modes: accepted
	 * now, and the 60 fps mode is what runs.  Geometry is then checked
	 * against THAT mode, not against the requested rate. */
	cfg.video0.width = 1920;
	cfg.video0.height = 1080;
	cfg.video0.fps = 45;
	failures += expect_valid("cv610_accept_size_ok_fps_between", &cfg, 1);
	/* sensor.index: one pad, so 0 and auto are the only answers. */
	cfg.video0.fps = 60;
	cfg.sensor.index = 0;
	failures += expect_valid("cv610_accept_sensor_pad_zero", &cfg, 1);
	cfg.sensor.index = 1;
	failures += expect_valid("cv610_reject_sensor_pad", &cfg, 0);
	cfg.sensor.index = -1;
	/* sensor.mode: an index into the advertised table, or auto. */
	cfg.sensor.mode = 0;
	failures += expect_valid("cv610_accept_sensor_mode", &cfg, 1);
	cfg.sensor.mode = (int)mode_count;
	failures += expect_valid("cv610_reject_sensor_mode_past_end", &cfg, 0);
	/* A forced mode drives the GOP limit, because it drives the encoder's
	 * frame rate.  gop_size 700 s passes at 60 fps (42000 frames) and fails
	 * at the forced 100 fps mode (70000) — video0.fps alone cannot tell
	 * these apart, which is the bug this check exists to catch. */
	cfg.sensor.mode = -1;
	cfg.video0.gop_size = 700.0;
	failures += expect_valid("cv610_gop_ok_at_requested_rate", &cfg, 1);
	cfg.sensor.mode = fastest_mode_index();
	failures += expect_valid("cv610_gop_uses_forced_mode_rate", &cfg, 0);
	cfg.sensor.mode = -1;
	cfg.video0.gop_size = 3.0;
	/* Half-set geometry must not widen into the default mode. */
	cfg.video0.height = 0;
	failures += expect_valid("cv610_reject_half_set_size", &cfg, 0);
	/* Control: video0.size=auto is still accepted. */
	cfg.video0.width = 0;
	failures += expect_valid("cv610_accept_auto_size", &cfg, 1);
	cfg.video0.width = 1920;
	cfg.video0.height = 1080;
	strcpy(cfg.outgoing.server, "shm://venc_wfb");
	failures += expect_valid("cv610_reject_packet_shm", &cfg, 0);
	strcpy(cfg.outgoing.server, "unix://venc_wfb");
	failures += expect_valid("cv610_accept_unix", &cfg, 1);
	strcpy(cfg.video0.rc_mode, "vbr");
	failures += expect_valid("cv610_reject_vbr", &cfg, 0);
	strcpy(cfg.video0.rc_mode, "cbr");
	strcpy(cfg.video0.resilience, "racing");
	(void)venc_config_apply_resilience_preset("racing", &cfg.video0);
	cfg.video0.slice_count = 17;
	failures += expect_valid("cv610_accept_resilience_and_slices", &cfg, 1);
	strcpy(cfg.video0.resilience, "off");
	(void)venc_config_apply_resilience_preset("off", &cfg.video0);
	cfg.video0.slice_count = 1;
	strcpy(cfg.video0.framing, "stab");
	failures += expect_valid("cv610_reject_framing", &cfg, 0);
	strcpy(cfg.video0.framing, "off");
	cfg.video0.gop_size = 2000.0;
	failures += expect_valid("cv610_reject_gop", &cfg, 0);
	cfg.video0.gop_size = 3.0;
	cfg.audio.enabled = true;
	strcpy(cfg.audio.codec, "opus");
	cfg.audio.sample_rate = 48000;
	cfg.audio.channels = 1;
	cfg.outgoing.audio_port = 5601;
	failures += expect_valid("cv610_accept_opus", &cfg, 1);
	cfg.audio.sample_rate = 16000;
	failures += expect_valid("cv610_reject_audio_rate", &cfg, 0);
	cfg.audio.sample_rate = 48000;
	cfg.audio.channels = 2;
	failures += expect_valid("cv610_reject_audio_channels", &cfg, 0);
	cfg.audio.channels = 1;
	strcpy(cfg.audio.codec, "pcm");
	failures += expect_valid("cv610_reject_audio_codec", &cfg, 0);
	strcpy(cfg.audio.codec, "opus");
	cfg.outgoing.audio_port = -1;
	failures += expect_valid("cv610_reject_invalid_audio_port", &cfg, 0);
	cfg.outgoing.audio_port = 5601;
	strcpy(cfg.outgoing.server, "frame-shm://venc_frame_out");
	failures += expect_valid("cv610_accept_frame_shm_audio_sidechannel",
		&cfg, 1);
	cfg.outgoing.audio_port = 0;
	failures += expect_valid("cv610_reject_frame_shm_audio_port_zero",
		&cfg, 0);

	return failures ? 1 : 0;
}
