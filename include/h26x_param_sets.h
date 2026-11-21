#ifndef H26X_PARAM_SETS_H
#define H26X_PARAM_SETS_H

#include "codec_types.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint8_t vps[256];
	size_t vps_len;
	uint8_t sps[512];
	size_t sps_len;
	uint8_t pps[512];
	size_t pps_len;
	uint8_t inband; /* hevc_rtp: VPS/SPS/PPS bits sent since the last slice */
} H26xParamSets;

typedef struct {
	const uint8_t *data;
	size_t len;
} H26xParamSetRef;

/** Store VPS/SPS/PPS from a NAL unit if it matches a parameter set type. */
void h26x_param_sets_update(H26xParamSets *sets, PAYLOAD_TYPE_E codec,
	uint8_t nal_type, const uint8_t *nal, size_t nal_len);

/** Return parameter sets that should precede the given NAL type. */
size_t h26x_param_sets_get_prepend(const H26xParamSets *sets,
	PAYLOAD_TYPE_E codec, uint8_t nal_type, H26xParamSetRef *refs,
	size_t max_refs);

#endif /* H26X_PARAM_SETS_H */
