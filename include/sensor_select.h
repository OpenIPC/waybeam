#ifndef SENSOR_SELECT_H
#define SENSOR_SELECT_H

#include <stdbool.h>
#include <stdint.h>

#include "star6e.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Unlock configuration (IMX335/IMX415 cold-boot workaround) ─────── */

typedef struct {
	int enabled;
	MI_U32 cmd_id;
	MI_U16 reg;
	MI_U16 value;
	MI_SNR_CustDir_e dir;
} SensorUnlockConfig;

/* ── Strategy hooks for sensor-specific quirks ───────────────────────── */

/* Forward declaration for post_enable hook */
typedef struct SensorSelectResult SensorSelectResult;

typedef struct {
	const char *name;

	/* Called before MI_SNR_SetRes(). Return 0 to continue, -1 to abort. */
	int (*pre_set_mode)(MI_SNR_PAD_ID_e pad, int mode_index, void *ctx);

	/* Called during FPS retry recovery (before re-issuing SetRes). */
	int (*on_fps_retry)(MI_SNR_PAD_ID_e pad, int mode_index, void *ctx);

	/* Called after MI_SNR_Enable() succeeds. */
	int (*post_enable)(MI_SNR_PAD_ID_e pad,
		const SensorSelectResult *result, void *ctx);

	/* Opaque context passed to hooks. */
	void *ctx;
} SensorStrategy;

/* ── Input / Output types ────────────────────────────────────────────── */

typedef struct {
	int forced_pad;           /* -1 = auto-detect */
	int forced_mode;          /* -1 = auto-select */
	uint32_t target_width;
	uint32_t target_height;
	uint32_t target_fps;
	int image_mirror;         /* applied via MI_SNR_SetOrien before Enable */
	int image_flip;
} SensorSelectConfig;

struct SensorSelectResult {
	MI_SNR_PAD_ID_e pad_id;
	MI_SNR_PadInfo_t pad;
	MI_SNR_PlaneInfo_t plane;
	MI_SNR_Res_t mode;
	uint32_t fps;             /* actual fps set on the sensor */
	int fps_fallback;         /* the requested fps was refused; fps is a
	                           * fallback, the mode may run at another rate */
	int mode_index;
};

/* ── Public API ──────────────────────────────────────────────────────── */

/* Return a no-op strategy (standard API path, no quirks). */
SensorStrategy sensor_default_strategy(void);

/* Return a strategy that applies MI_SNR_CustFunction unlock at
 * pre_set_mode and on_fps_retry.  Caller must keep unlock_cfg alive
 * for the lifetime of the strategy. */
SensorStrategy sensor_unlock_strategy(SensorUnlockConfig *unlock_cfg);

/* Select and initialize the best sensor mode.
 * Returns 0 on success, populating *result.  Returns -1 on failure. */
int sensor_select(const SensorSelectConfig *cfg,
	const SensorStrategy *strategy, SensorSelectResult *result);

/* Print available sensor modes to stdout.
 * forced_pad: -1 to scan all pads, 0-3 to scan one.
 * selected_pad/selected_mode: highlight the selected mode (-1 to skip). */
void sensor_list_modes(int forced_pad, int selected_pad, int selected_mode);

/* Build a JSON string describing all available sensor modes.
 * Caller must free() the returned string.  Returns NULL on failure.
 * selected_pad/selected_mode: mark the currently active mode (-1 = none). */
char *sensor_modes_json(int forced_pad, int selected_pad, int selected_mode,
	uint32_t encode_width, uint32_t encode_height);

/* ── Scoring helpers (exposed for unit testing) ──────────────────────── */

/* Returns 1 if fps is within [mode.minFps, mode.maxFps]. */
int sensor_mode_fps_supported(const MI_SNR_Res_t *mode, uint32_t fps);

/* Clamp fps to [mode.minFps, mode.maxFps]. */
uint32_t sensor_mode_clamp_fps(const MI_SNR_Res_t *mode, uint32_t fps);

/* Score a mode: 3 = fps_ok+fit, 2 = fps_ok, 1 = fit, 0 = neither. */
int sensor_mode_score(const MI_SNR_Res_t *mode,
	uint32_t target_w, uint32_t target_h, uint32_t target_fps);

/* Combined cost: resolution distance² + FPS excess penalty.
 * Prefers modes whose maxFps is closest to target_fps. */
uint64_t sensor_mode_cost(const MI_SNR_Res_t *mode,
	uint32_t target_w, uint32_t target_h, uint32_t target_fps);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_SELECT_H */
