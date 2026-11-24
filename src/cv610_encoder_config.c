#include "cv610_encoder_config.h"

#include <string.h>

int cv610_encoder_config_derive(const VencConfig *cfg, uint32_t width,
	uint32_t height, uint32_t fps, Cv610EncoderConfig *out)
{
	IntraRefreshMode mode;
	uint32_t rows;
	uint32_t per;

	if (!cfg || !out || width == 0 || height == 0 || fps == 0 ||
	    cfg->video0.slice_count < 1 ||
	    cfg->video0.slice_count > VENC_SLICE_COUNT_MAX)
		return -1;

	memset(out, 0, sizeof(*out));
	mode = intra_refresh_parse_mode(cfg->video0.intra_refresh_mode);
	/* The sweep must cover the axis it travels along, and CV610 refreshes by
	 * COLUMN (cv610_runtime.c), so that axis is the picture WIDTH.
	 * intra_refresh_compute() is axis-agnostic — it divides whatever extent it
	 * is handed into 32-px units — so it gets the width, while the slice split
	 * below keeps the height because slices are always rows.
	 * Device-confirmed on .181: refresh_num 3 took 19 frames to clear 1920 px
	 * (60 units of 32 px), which pins both the unit and the axis. Feeding it
	 * the height instead made the sweep 1.7x slower than the preset's target.
	 * Consequence: on CV610 `total_rows` and `lines` in /api/v1/intra/status
	 * count 32-px COLUMNS, not rows. */
	intra_refresh_compute(mode, width, fps,
		cfg->video0.intra_refresh_lines,
		cfg->video0.intra_refresh_qp,
		cfg->video0.gop_size, &out->intra.derived);
	out->intra.enabled = mode != INTRA_MODE_OFF;
	out->intra.refresh_dir = 1; /* OT_VENC_INTRA_REFRESH_COLUMN */
	out->intra.refresh_num = out->intra.derived.lines;
	out->intra.request_i_qp = out->intra.derived.req_iqp;

	if (cfg->video0.ref_base > 0) {
		out->ref.enabled = 1;
		out->ref.base = cfg->video0.ref_base;
		out->ref.enhance = cfg->video0.ref_enhance
			? cfg->video0.ref_enhance : 1;
		out->ref.pred = cfg->video0.ref_pred ? 1u : 0u;
	}

	rows = (height + 31u) / 32u;
	out->slice.requested_count = cfg->video0.slice_count;
	out->slice.total_lcu_rows = rows;
	out->slice.split_mode = CV610_SLICE_SPLIT_MODE_LCU_ROW;
	out->slice.split_size = 1;
	out->slice.expected_count = 1;
	if (cfg->video0.slice_count > 1) {
		per = (rows + cfg->video0.slice_count - 1u) /
			cfg->video0.slice_count;
		if (per < 1)
			per = 1;
		out->slice.enabled = 1;
		out->slice.split_size = per;
		out->slice.expected_count = (rows + per - 1u) / per;
		if (cfg->video0.low_latency) {
			out->slice.early_output = 1;
			out->slice.low_delay_lines = per * 32u < height
				? per * 32u : height;
		}
	}

	return 0;
}
