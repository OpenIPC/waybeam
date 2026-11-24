#ifndef CV610_ENCODER_CONFIG_H
#define CV610_ENCODER_CONFIG_H

#include "intra_refresh.h"
#include "venc_config.h"

#include <stdint.h>

/* The CV610 getter reports split_mode=1 for its H.265 row-split default.
 * OpenHisilicon does not publish a named enum for this field. Keep the raw
 * value isolated here and verify it by setter/readback plus slice-NAL census. */
#define CV610_SLICE_SPLIT_MODE_LCU_ROW 1u

typedef struct {
	IntraRefreshDerived derived;
	uint32_t enabled;
	/* Vendor sweep DIRECTION (ot_venc_intra_refresh_mode): 1 = COLUMN.
	 * Named refresh_dir, not mode, because `derived.mode` right above is an
	 * unrelated enum (IntraRefreshMode, where 1 == INTRA_MODE_FAST). Both read
	 * 1 in the racing preset, so two adjacent fields spelled `mode` would make
	 * a mix-up silent. */
	uint32_t refresh_dir;
	uint32_t refresh_num;
	uint32_t request_i_qp;
} Cv610IntraConfig;

typedef struct {
	uint32_t enabled;
	uint32_t base;
	uint32_t enhance;
	uint32_t pred;
} Cv610RefConfig;

typedef struct {
	uint32_t enabled;
	uint32_t requested_count;
	uint32_t total_lcu_rows;
	uint32_t split_mode;
	uint32_t split_size;
	uint32_t expected_count;
	/* video0.low_latency: slices leave the encoder as they finish
	 * (is_by_frame off, slice_output_en on), and VPSS releases rows to it
	 * after low_delay_lines -- one slice height, the value measured clean
	 * at 720p100 (128) and 1080p60 (192).  0 lines = low delay off. */
	uint32_t early_output;
	uint32_t low_delay_lines;
} Cv610SliceConfig;

typedef struct {
	Cv610IntraConfig intra;
	Cv610RefConfig ref;
	Cv610SliceConfig slice;
} Cv610EncoderConfig;

/* Pure mapping from the shared config to CV610 encoder concepts. Vendor
 * structs are intentionally filled in cv610_runtime.c so this logic remains
 * host-testable without the external OpenHisilicon headers. */
/* `width` feeds the intra-refresh sweep (CV610 refreshes by COLUMN, so the
 * refresh axis is horizontal); `height` feeds the slice split, which is always
 * a row split. Both are picture dimensions in pixels. */
int cv610_encoder_config_derive(const VencConfig *cfg, uint32_t width,
	uint32_t height, uint32_t fps, Cv610EncoderConfig *out);

#endif /* CV610_ENCODER_CONFIG_H */
