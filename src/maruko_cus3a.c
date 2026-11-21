#include "maruko_cus3a.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── CUS3A / ISP ABI structures ──────────────────────────────────────── */

/* Maruko HW AE statistic cell (mi_isp_hw_dep_datatype.h: MI_ISP_AE_AVGS) —
 * 4 bytes per grid cell, 128×90 grid. */
typedef struct {
	unsigned char r;
	unsigned char g;
	unsigned char b;
	unsigned char y;
} MarukoAeSample;

#define MARUKO_AE_GRID_X   128
#define MARUKO_AE_GRID_Y   90
#define MARUKO_AE_GRID_SZ  (MARUKO_AE_GRID_X * MARUKO_AE_GRID_Y)

/* MI_ISP_AE_HW_STATISTICS_t — 8 bytes header + grid */
typedef struct {
	uint32_t       nBlkX;
	uint32_t       nBlkY;
	MarukoAeSample nAvg[MARUKO_AE_GRID_SZ];
} MarukoAeHwStats;

/* CusAEInfo_t — Maruko packed layout (mi_isp_hw_dep_datatype.h:144). */
typedef struct {
	uint32_t Size;
	uint32_t hist1;
	uint32_t hist2;
	uint32_t AvgBlkX;
	uint32_t AvgBlkY;
	uint32_t avgs;
	uint32_t Shutter;
	uint32_t SensorGain;
	uint32_t IspGain;
	uint32_t ShutterHDRShort;
	uint32_t SensorGainHDRShort;
	uint32_t IspGainHDRShort;
} __attribute__((packed)) MarukoAeInfo;

#define MARUKO_ISP_STATE_NORMAL 0

/* ── ISP exposure limit (matches MI_ISP_AE_ExpoLimitType_t) ──────────── */

typedef struct {
	uint32_t minShutterUs;
	uint32_t maxShutterUs;
	uint32_t minFNx10;
	uint32_t maxFNx10;
	uint32_t minSensorGain;
	uint32_t minIspGain;
	uint32_t maxSensorGain;
	uint32_t maxIspGain;
} MarukoIspExposureLimit;

/* ── Function pointer types (resolved via dlsym) ─────────────────────── */

typedef int (*fn_ae_get_hw_stats_t)(uint32_t dev, uint32_t chn,
	MarukoAeHwStats *stats);
typedef int (*fn_cus3a_get_ae_status_t)(uint32_t dev, uint32_t chn,
	MarukoAeInfo *info);
typedef int (*fn_ae_get_state_t)(uint32_t dev, uint32_t chn, int *state);

typedef int (*fn_ae_get_exposure_limit_t)(uint32_t dev, uint32_t chn,
	MarukoIspExposureLimit *limit);
typedef int (*fn_ae_set_exposure_limit_t)(uint32_t dev, uint32_t chn,
	MarukoIspExposureLimit *limit);

/* ── Module state ────────────────────────────────────────────────────── */

typedef struct {
	MarukoCus3aConfig cfg;

	/* runtime caps (writable from main thread) */
	volatile uint32_t shutter_max_us;
	int shutter_pin;                    /* pin minShutter==maxShutter */
	volatile uint32_t gain_max;
	volatile uint32_t shutter_min_us;   /* 0 = bin default; ignored if pinned */
	volatile uint32_t gain_min;         /* 0 = use ISP bin default */

	/* baseline limits read from ISP bin */
	uint32_t bin_max_shutter_us;
	uint32_t bin_max_sensor_gain;
	uint32_t bin_min_shutter_us;   /* bin's calibrated floors — restored */
	uint32_t bin_min_sensor_gain;  /* when the user sets the min back to 0 */

	pthread_t thread;
	volatile int running;

	/* refcounted handles — dlclose only on actual stop */
	void *h_isp;
	void *h_cus3a;

	fn_ae_get_hw_stats_t        fn_get_hw_stats;
	fn_cus3a_get_ae_status_t    fn_get_ae_status;
	fn_ae_get_state_t           fn_ae_get_state;
	fn_ae_get_exposure_limit_t  fn_get_exposure_limit;
	fn_ae_set_exposure_limit_t  fn_set_exposure_limit;
} Cus3aState;

static Cus3aState g_cus3a;

/* ── Helpers ─────────────────────────────────────────────────────────── */

void maruko_cus3a_config_defaults(MarukoCus3aConfig *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->sensor_fps = 120;
	cfg->ae_fps = 15;
}

static uint32_t compute_max_shutter(const Cus3aState *s)
{
	if (s->shutter_max_us > 0)
		return s->shutter_max_us;
	const uint32_t fps = __atomic_load_n(&s->cfg.sensor_fps,
		__ATOMIC_RELAXED);

	if (fps > 0)
		return 1000000 / fps;
	return 8333;  /* fallback ~120fps */
}

static int resolve_symbols(Cus3aState *s)
{
	s->h_isp = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!s->h_isp) {
		fprintf(stderr, "[maruko-cus3a] dlopen libmi_isp.so: %s\n",
			dlerror());
		return -1;
	}
	s->h_cus3a = dlopen("libcus3a.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!s->h_cus3a) {
		fprintf(stderr, "[maruko-cus3a] dlopen libcus3a.so: %s\n",
			dlerror());
		return -1;
	}

	s->fn_get_hw_stats = (fn_ae_get_hw_stats_t)dlsym(s->h_isp,
		"MI_ISP_AE_GetAeHwAvgStats");
	s->fn_get_ae_status = (fn_cus3a_get_ae_status_t)dlsym(s->h_isp,
		"MI_ISP_CUS3A_GetAeStatus");
	s->fn_ae_get_state = (fn_ae_get_state_t)dlsym(s->h_isp,
		"MI_ISP_AE_GetState");
	s->fn_get_exposure_limit = (fn_ae_get_exposure_limit_t)dlsym(s->h_isp,
		"MI_ISP_AE_GetExposureLimit");
	s->fn_set_exposure_limit = (fn_ae_set_exposure_limit_t)dlsym(s->h_isp,
		"MI_ISP_AE_SetExposureLimit");

	if (!s->fn_get_hw_stats || !s->fn_get_ae_status) {
		fprintf(stderr,
			"[maruko-cus3a] missing AE stats symbols\n");
		return -1;
	}
	if (!s->fn_get_exposure_limit || !s->fn_set_exposure_limit) {
		fprintf(stderr,
			"[maruko-cus3a] missing exposure limit symbols\n");
		return -1;
	}
	return 0;
}

static void release_symbols(Cus3aState *s)
{
	if (s->h_cus3a) dlclose(s->h_cus3a);
	if (s->h_isp)   dlclose(s->h_isp);
	s->h_isp = NULL;
	s->h_cus3a = NULL;
}

/* ── AE limits ───────────────────────────────────────────────────────── */

#define AE_GAIN_MIN         1024   /* 1.0x */
#define AE_GAIN_MAX_DEFAULT 32768  /* 32x sensor cap (IMX415) */

/* ── Supervisory AE thread (limits-only) ─────────────────────────────────
 *
 * The vendor AE/AWB runs under paced native 3A (maruko_ae_pacer_thread in
 * maruko_pipeline.c).  This thread does NOT drive exposure — it enforces the
 * user's gain/shutter caps via MI_ISP_AE_SetExposureLimit and reads AE stats
 * for the verbose log.  (The retired "throttle"/aeEngine=custom controller
 * that used to live here was removed in v0.24.1 — paced native superseded it
 * on every axis; HISTORY 0.22.0.) */

static void *cus3a_thread(void *arg)
{
	Cus3aState *s = arg;
	MarukoAeHwStats *ae_hw;
	MarukoAeInfo ae_info;
	MarukoIspExposureLimit cur_limit;
	unsigned int sleep_ms;
	unsigned long ticks = 0, limit_writes = 0;
	unsigned long last_log_ms = 0;
	uint32_t shutter_max_us;
	uint32_t gain_max_eff;

	ae_hw = malloc(sizeof(MarukoAeHwStats));
	if (!ae_hw) {
		fprintf(stderr, "[maruko-cus3a] AE stats alloc failed\n");
		return NULL;
	}

	if (s->fn_ae_get_state) {
		int state = -1;
		s->fn_ae_get_state(0, 0, &state);
		if (state != MARUKO_ISP_STATE_NORMAL)
			fprintf(stderr,
				"[maruko-cus3a] WARN: AE state=%d (not NORMAL); "
				"exposure may not converge\n", state);
		else if (s->cfg.verbose)
			printf("[maruko-cus3a] AE state: NORMAL\n");
	}

	memset(&cur_limit, 0, sizeof(cur_limit));
	s->fn_get_exposure_limit(0, 0, &cur_limit);

	/* Fold the user's shutter/gain caps into the live exposure limit and
	 * write it back — the SDK's native AE respects SetExposureLimit. */
	{
		uint32_t want_shutter = compute_max_shutter(s);
		uint32_t want_gain = s->gain_max;

		if (want_shutter > 0 &&
		    want_shutter <= cur_limit.maxShutterUs)
			cur_limit.maxShutterUs = want_shutter;
		if (s->shutter_pin && want_shutter > 0)
			cur_limit.minShutterUs = want_shutter;
		if (want_gain > 0 && want_gain <= cur_limit.maxSensorGain)
			cur_limit.maxSensorGain = want_gain;

		/* Manual floors (0 = leave the ISP-bin default).  The 180° pin
		 * already forces minShutter==maxShutter, so honour a manual
		 * shutter floor only when not pinned; never let a floor exceed
		 * the ceiling we just wrote. */
		if (!s->shutter_pin && s->shutter_min_us > 0)
			cur_limit.minShutterUs =
				s->shutter_min_us > cur_limit.maxShutterUs ?
				cur_limit.maxShutterUs : s->shutter_min_us;
		if (s->gain_min > 0)
			cur_limit.minSensorGain =
				s->gain_min > cur_limit.maxSensorGain ?
				cur_limit.maxSensorGain : s->gain_min;

		s->fn_set_exposure_limit(0, 0, &cur_limit);
		limit_writes++;
		if (s->cfg.verbose)
			printf("[maruko-cus3a] init limits: shutter=[%u,%u]us "
				"gain=[%u,%u]\n",
				cur_limit.minShutterUs,
				cur_limit.maxShutterUs,
				cur_limit.minSensorGain,
				cur_limit.maxSensorGain);
	}

	sleep_ms = s->cfg.ae_fps > 0 ? 1000 / s->cfg.ae_fps : 66;
	if (sleep_ms < 1) sleep_ms = 1;

	shutter_max_us = compute_max_shutter(s);
	/* Prefer the bin's calibrated ceiling when the user hasn't capped
	 * gain explicitly; AE_GAIN_MAX_DEFAULT (32x) is only a last-resort
	 * fallback when bin limits are unavailable.  Pushing past the bin
	 * ceiling produces visible color/brightness drift. */
	gain_max_eff = s->gain_max > 0
		? s->gain_max
		: (s->bin_max_sensor_gain > 0
			? s->bin_max_sensor_gain
			: AE_GAIN_MAX_DEFAULT);

	if (s->cfg.verbose)
		printf("[maruko-cus3a] thread started: %u Hz native, "
			"shutter max %uus, gain %u..%u\n",
			s->cfg.ae_fps, shutter_max_us,
			AE_GAIN_MIN, gain_max_eff);

	while (s->running) {
		unsigned int avg_y = 0;

		/* Supervisory read: AE luma average for the debug log.  The
		 * SDK's native AE owns the actual exposure loop. */
		if (s->fn_get_hw_stats(0, 0, ae_hw) == 0) {
			memset(&ae_info, 0, sizeof(ae_info));
			s->fn_get_ae_status(0, 0, &ae_info);

			unsigned int bx = ae_hw->nBlkX;
			unsigned int by = ae_hw->nBlkY;
			if (bx == 0 || by == 0 || bx > MARUKO_AE_GRID_X ||
			    by > MARUKO_AE_GRID_Y) {
				bx = MARUKO_AE_GRID_X;
				by = MARUKO_AE_GRID_Y;
			}
			unsigned int total = bx * by;
			if (total > 0) {
				unsigned long sumlin = 0;
				unsigned int x;
				/* Grid is stored packed at stride nBlkX. */
				for (x = 0; x < total; x++)
					sumlin += ae_hw->nAvg[x].y;
				avg_y = (unsigned int)(sumlin / total);
			}
		}

		/* ── Enforce static gain/shutter caps from config ──────── */
		{
			uint32_t want_shutter = compute_max_shutter(s);
			uint32_t want_gain = s->gain_max;
			uint32_t effective_gain = want_gain > 0 ? want_gain :
				s->bin_max_sensor_gain;
			int changed = 0;

			s->fn_get_exposure_limit(0, 0, &cur_limit);

			/* Compare against the FRESH read, not a local
			 * "applied" cache: the CUS3A AE init (first
			 * frame-syncs after start) resets limits to the bin
			 * values, silently undoing an early push — e.g.
			 * isp.gainMax above the bin ceiling never stuck
			 * (found with gainMax=32000 vs bin 8192, v0.22.0). */
			if (want_shutter > 0 &&
			    cur_limit.maxShutterUs != want_shutter) {
				cur_limit.maxShutterUs = want_shutter;
				shutter_max_us = want_shutter;
				changed = 1;
			}
			if (s->shutter_pin && want_shutter > 0 &&
			    cur_limit.minShutterUs != want_shutter) {
				cur_limit.minShutterUs = want_shutter;
				changed = 1;
			}
			if (effective_gain > 0 &&
			    cur_limit.maxSensorGain != effective_gain) {
				cur_limit.maxSensorGain = effective_gain;
				gain_max_eff = effective_gain;
				changed = 1;
			}
			/* Manual floors (compared against the fresh read, like
			 * the ceilings above).  Pin overrides the shutter floor;
			 * a 0 config falls back to the bin's calibrated floor so
			 * clearing the override restores it; clamp to the ceiling. */
			{
				uint32_t eff = s->shutter_min_us > 0 ?
					s->shutter_min_us : s->bin_min_shutter_us;
				uint32_t v = eff > cur_limit.maxShutterUs ?
					cur_limit.maxShutterUs : eff;
				if (!s->shutter_pin && eff > 0 &&
				    cur_limit.minShutterUs != v) {
					cur_limit.minShutterUs = v;
					changed = 1;
				}
			}
			{
				uint32_t eff = s->gain_min > 0 ?
					s->gain_min : s->bin_min_sensor_gain;
				uint32_t v = eff > cur_limit.maxSensorGain ?
					cur_limit.maxSensorGain : eff;
				if (eff > 0 && cur_limit.minSensorGain != v) {
					cur_limit.minSensorGain = v;
					changed = 1;
				}
			}

			if (changed) {
				int lret = s->fn_set_exposure_limit(
					0, 0, &cur_limit);
				limit_writes++;
				if (s->cfg.verbose)
					printf("[maruko-cus3a] limit "
						"push: shutter max "
						"%uus gain max %u "
						"ret=%d\n",
						cur_limit.maxShutterUs,
						cur_limit.maxSensorGain,
						lret);
			}
		}

		ticks++;

		{
			struct timespec ts;
			unsigned long now_ms;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			now_ms = ts.tv_sec * 1000UL + ts.tv_nsec / 1000000;
			if (s->cfg.verbose && now_ms - last_log_ms >= 5000) {
				printf("[maruko-cus3a] %lu ticks | "
					"%lu lim | avgY=%u | "
					"isp shutter=%uus sgain=%u "
					"igain=%u\n",
					ticks, limit_writes, avg_y,
					ae_info.Shutter,
					ae_info.SensorGain,
					ae_info.IspGain);
				last_log_ms = now_ms;
			}
		}

		{
			unsigned long ns = sleep_ms * 1000000UL;
			struct timespec req = {
				(time_t)(ns / 1000000000UL),
				(long)(ns % 1000000000UL)
			};
			nanosleep(&req, NULL);
		}
	}

	free(ae_hw);
	if (s->cfg.verbose)
		printf("[maruko-cus3a] thread stopped (%lu ticks, "
			"%lu limit writes)\n",
			ticks, limit_writes);
	return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int maruko_cus3a_start(const MarukoCus3aConfig *cfg)
{
	int ret;

	if (g_cus3a.running) {
		fprintf(stderr, "[maruko-cus3a] already running\n");
		return -1;
	}

	memset(&g_cus3a, 0, sizeof(g_cus3a));
	g_cus3a.cfg = *cfg;
	g_cus3a.shutter_max_us = cfg->shutter_max_us;
	g_cus3a.shutter_pin = cfg->shutter_pin;
	g_cus3a.gain_max = cfg->gain_max;
	g_cus3a.shutter_min_us = cfg->shutter_min_us;
	g_cus3a.gain_min = cfg->gain_min;

	if (resolve_symbols(&g_cus3a) != 0) {
		release_symbols(&g_cus3a);
		return -1;
	}

	{
		MarukoIspExposureLimit lim;
		memset(&lim, 0, sizeof(lim));
		if (g_cus3a.fn_get_exposure_limit(0, 0, &lim) == 0 &&
		    lim.maxSensorGain > 0) {
			g_cus3a.bin_max_shutter_us = lim.maxShutterUs;
			g_cus3a.bin_max_sensor_gain = lim.maxSensorGain;
			g_cus3a.bin_min_shutter_us = lim.minShutterUs;
			g_cus3a.bin_min_sensor_gain = lim.minSensorGain;
			if (cfg->verbose)
				printf("[maruko-cus3a] ISP bin limits: "
					"gain %u-%u, isp_gain max %u, "
					"shutter %u-%uus\n",
					lim.minSensorGain, lim.maxSensorGain,
					lim.maxIspGain, lim.minShutterUs,
					lim.maxShutterUs);
		} else if (cfg->verbose) {
			printf("[maruko-cus3a] ISP bin limits unavailable\n");
		}
	}

	g_cus3a.running = 1;
	ret = pthread_create(&g_cus3a.thread, NULL, cus3a_thread, &g_cus3a);
	if (ret != 0) {
		fprintf(stderr,
			"[maruko-cus3a] pthread_create failed: %d\n", ret);
		g_cus3a.running = 0;
		release_symbols(&g_cus3a);
		return -1;
	}

	return 0;
}

void maruko_cus3a_stop(void)
{
	maruko_cus3a_request_stop();
	maruko_cus3a_join();
}

void maruko_cus3a_request_stop(void)
{
	if (!g_cus3a.running)
		return;
	g_cus3a.running = 0;
}

void maruko_cus3a_join(void)
{
	if (g_cus3a.h_isp == NULL && g_cus3a.h_cus3a == NULL)
		return;
	pthread_join(g_cus3a.thread, NULL);
	release_symbols(&g_cus3a);
	printf("[maruko-cus3a] stopped\n");
}

int maruko_cus3a_running(void)
{
	return g_cus3a.running;
}

void maruko_cus3a_set_gain_max(uint32_t gain)
{
	g_cus3a.gain_max = gain;
}

void maruko_cus3a_set_shutter_max(uint32_t us)
{
	g_cus3a.shutter_max_us = us;
}

void maruko_cus3a_set_sensor_fps(uint32_t fps)
{
	__atomic_store_n(&g_cus3a.cfg.sensor_fps, fps, __ATOMIC_RELAXED);
}

void maruko_cus3a_set_gain_min(uint32_t gain)
{
	g_cus3a.gain_min = gain;
}

void maruko_cus3a_set_shutter_min(uint32_t us)
{
	g_cus3a.shutter_min_us = us;
}
