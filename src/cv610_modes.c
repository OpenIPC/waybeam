#include "cv610_modes.h"

#include <stdlib.h>
#include <strings.h>

/* ---- IMX662: 1920x1080, four rates ---------------------------------------
 *
 * All four entries are hardware-verified on the IMX662 bring-up board.
 *
 * The clock column is the one that bites.  The sensor keeps whatever line
 * timing it was programmed with regardless of the MCLK it is actually fed,
 * so a mismatch is silent: frames simply arrive at fps * actual / expected
 * while /fps/live and /api/v1/modes both still report the nominal rate.
 * Measured 43.6 fps for the 60 fps mode when it ran on the 100 fps mode's
 * 27 MHz clock.  Only 100 fps takes the sensor's 24 MHz INCK profile
 * overclocked to 27 MHz (INCK_SEL 0x04); the rest run 37.125 MHz. */
static const Cv610SensorMode g_imx662_modes[] = {
	{ 1920, 1080,  30, 12, 37125000u, "1080p30 RAW12" },
	{ 1920, 1080,  60, 12, 37125000u, "1080p60 RAW12" },
	{ 1920, 1080,  90, 10, 37125000u, "1080p90 RAW10" },
	{ 1920, 1080, 100, 10, 27000000u, "1080p100 RAW10" },
};

/* ---- IMX335: 2592x1944 and crops, five fixed modes -----------------------
 *
 * The plugin is OpenIPC/openhisilicon's sony_imx335 as the firmware image
 * installs it; nothing about it needs a workaround here.  These rows are the
 * fixed modes of its mode_tbl[], in the same order, at the rates #127 brought
 * them up at (binning can reach 76), and every one runs on 37.125 MHz.
 * pub.sns_mode stays 0 ("auto"): the plugin then picks its mode from the exact
 * capture size and a rate inside that mode's range, which keeps rows 0 and 3
 * (same size, 30 vs 40) and 0 and 4 (same rate, different size) apart.  Its
 * flexible window crop (mode 5) has no fixed size and is not listed.  The
 * plugin refuses vertical flip in the two crop windows and logs why; their
 * labels say so, since /api/v1/modes is where an operator reads them. */
static const Cv610SensorMode g_imx335_modes[] = {
	{ 2592, 1944, 30, 12, 37125000u, "2592x1944p30 RAW12" },
	{ 1296,  972, 60, 10, 37125000u, "1296x972p60 RAW10 binned" },
	{ 1920, 1080, 90, 10, 37125000u, "1080p90 RAW10 crop, no V-flip" },
	{ 2592, 1944, 40, 10, 37125000u, "2592x1944p40 RAW10" },
	{ 2592, 1520, 30, 10, 37125000u, "2592x1520p30 RAW10 crop, no V-flip" },
};

#define TABLE(t) (t), (sizeof(t) / sizeof((t)[0]))

/* IMX662 is waybeam's own plugin, built from sensors/cv610/imx662 and kept
 * out of /usr/lib/sensors so an image's stock copy (which refuses 100 fps)
 * can never stand in for it.  IMX335 is the image's. */
static const Cv610Sensor g_sensors[] = {
	{ "imx662", "/usr/lib/cv610/libsns_imx662.so", "g_sns_imx662_obj", 662,
	  TABLE(g_imx662_modes) },
	{ "imx335", "/usr/lib/sensors/libsns_imx335.so", "g_sns_imx335_obj", 335,
	  TABLE(g_imx335_modes) },
};

const Cv610Sensor *cv610_sensor_find(const char *name)
{
	size_t i;

	if (name == NULL)
		return NULL;
	for (i = 0; i < sizeof(g_sensors) / sizeof(g_sensors[0]); ++i) {
		if (strcasecmp(name, g_sensors[i].name) == 0)
			return &g_sensors[i];
	}
	return NULL;
}

const Cv610Sensor *cv610_sensor_current(void)
{
	const Cv610Sensor *s = cv610_sensor_find(getenv("SENSOR"));

	return s ? s : &g_sensors[0];
}

/* VPSS scales, it does not invent detail: upscaling past the captured size
 * costs encoder bandwidth for no information.  Refuse it rather than let a
 * config quietly waste the link. */
#define CV610_OUT_ALIGN 8u
#define CV610_OUT_MIN   128u

/* Whether a mode captures at least the requested encode size, so VPSS can
 * produce it without upscaling.  An unset size (0x0) fits every mode. */
static int mode_fits(const Cv610SensorMode *m, uint32_t width,
	uint32_t height)
{
	return (width == 0 && height == 0) ||
		(m->width >= width && m->height >= height);
}

/* The rate rule over the modes that fit: exact rate (first row wins), else the
 * slowest mode still faster than the target -- never deliver fewer frames than
 * asked for while a mode can keep up -- else the fastest.  NULL when no mode
 * fits.  *pick_i receives the row. */
static const Cv610SensorMode *select_by_rate(const Cv610Sensor *sensor,
	uint32_t target_fps, uint32_t width, uint32_t height, size_t *pick_i)
{
	const Cv610SensorMode *pick = NULL;
	size_t i;

	for (i = 0; i < sensor->mode_count; ++i) {
		const Cv610SensorMode *m = &sensor->modes[i];

		if (mode_fits(m, width, height) && m->fps == target_fps) {
			*pick_i = i;
			return m;
		}
	}
	for (i = 0; i < sensor->mode_count; ++i) {
		const Cv610SensorMode *m = &sensor->modes[i];

		if (mode_fits(m, width, height) && m->fps > target_fps &&
			(pick == NULL || m->fps < pick->fps)) {
			pick = m;
			*pick_i = i;
		}
	}
	if (pick != NULL)
		return pick;
	/* Target above every mode that fits: clamp to the fastest. */
	for (i = 0; i < sensor->mode_count; ++i) {
		const Cv610SensorMode *m = &sensor->modes[i];

		if (mode_fits(m, width, height) &&
			(pick == NULL || m->fps > pick->fps)) {
			pick = m;
			*pick_i = i;
		}
	}
	return pick;
}

const Cv610SensorMode *cv610_mode_select(const Cv610Sensor *sensor,
	int forced_mode, uint32_t target_fps, uint32_t width, uint32_t height,
	int *out_index)
{
	const Cv610SensorMode *pick;
	size_t pick_i = 0;

	if (out_index)
		*out_index = -1;

	/* A forced index is the whole answer: it exists or the config is wrong.
	 * Parity with find_best_mode(), which fails bring-up rather than
	 * quietly running a mode the operator did not ask for. */
	if (forced_mode >= 0) {
		if ((size_t)forced_mode >= sensor->mode_count)
			return NULL;
		if (out_index)
			*out_index = forced_mode;
		return &sensor->modes[forced_mode];
	}
	/* Nothing to select against.  Rejecting rather than inventing a rate for
	 * a config that names none. */
	if (target_fps == 0)
		return NULL;

	pick = select_by_rate(sensor, target_fps, width, height, &pick_i);
	/* No mode captures the requested size: choose on rate alone and let
	 * cv610_mode_check_output() reject the size against that mode. */
	if (pick == NULL)
		pick = select_by_rate(sensor, target_fps, 0, 0, &pick_i);
	if (out_index)
		*out_index = (int)pick_i;
	return pick;
}

const char *cv610_mode_check_output(const Cv610SensorMode *mode,
	uint32_t width, uint32_t height)
{
	if (mode == NULL)
		return "no sensor mode selected";
	/* Only a fully unset geometry means "the mode's own size".  A half-set
	 * one (1920x0) is a typo, not a request, and must not be completed into
	 * something the caller did not ask for. */
	if (width == 0 && height == 0)
		return NULL;
	if (width == 0 || height == 0)
		return "CV610 video0.size needs both a width and a height";
	if (width % CV610_OUT_ALIGN != 0 || height % CV610_OUT_ALIGN != 0)
		return "CV610 video0.size must be a multiple of 8";
	if (width < CV610_OUT_MIN || height < CV610_OUT_MIN)
		return "CV610 video0.size must be at least 128x128";
	if (width > mode->width || height > mode->height)
		return "CV610 video0.size exceeds the sensor mode's capture size; "
			"lower video0.size, or select a larger mode (sensor.mode, "
			"or video0.fps when the mode is automatic)";
	return NULL;
}

void cv610_mode_resolve_output(const Cv610SensorMode *mode,
	uint32_t req_width, uint32_t req_height,
	uint32_t *out_width, uint32_t *out_height)
{
	uint32_t w = 0;
	uint32_t h = 0;

	if (mode != NULL) {
		int given = (req_width != 0 && req_height != 0);

		/* Auto is the capture, held to the alignment an explicit size
		 * must meet: IMX335's binned 1296x972 encodes as 1296x968. */
		w = given ? req_width : mode->width / CV610_OUT_ALIGN * CV610_OUT_ALIGN;
		h = given ? req_height : mode->height / CV610_OUT_ALIGN * CV610_OUT_ALIGN;
	}
	if (out_width)
		*out_width = w;
	if (out_height)
		*out_height = h;
}
