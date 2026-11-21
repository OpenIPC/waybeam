#ifndef HEVC_RTP_H
#define HEVC_RTP_H

#include "h26x_param_sets.h"
#include "rtp_packetizer.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t total_nals;
	uint32_t single_packets;
	uint32_t fu_packets;
	uint32_t rtp_packets;
	uint32_t rtp_payload_bytes;
} HevcRtpStats;

/** Send one HEVC NAL: single-NAL packet when it fits, else FU-A fragments.
 *  NALs are never aggregated (no RTP Aggregation Packets) so the wire stays
 *  compatible with receivers that only implement single-NAL + FU-A.
 *  If is_last is non-zero, the final packet carries the RTP marker bit. */
size_t hevc_rtp_send_nal(const uint8_t *data, size_t len,
	RtpPacketizerState *rtp, RtpPacketizerWriteFn write_cb, void *opaque,
	int is_last, size_t max_payload, HevcRtpStats *stats);

/** Send VPS/SPS/PPS prepend NALs for an IDR NAL type, each as its own packet.
 *  No-op for non-IDR types. */
size_t hevc_rtp_prepend_param_sets(const H26xParamSets *params, uint8_t nal_type,
	RtpPacketizerState *rtp, RtpPacketizerWriteFn write_cb, void *opaque,
	size_t max_payload, HevcRtpStats *stats);

/** Send one encoder pack region as one RTP NAL (single or FU) per Annex-B
 *  NAL: every slice segment travels on its own, so a lost packet costs one
 *  slice instead of the picture.  A region with no start code is one raw
 *  NAL.  With params, every NAL updates the parameter sets and VPS/SPS/PPS
 *  go out once, before an IRAP picture's first slice.  is_last puts the
 *  marker on the region's final packet only. */
size_t hevc_rtp_send_annexb(const uint8_t *data, size_t len,
	H26xParamSets *params, RtpPacketizerState *rtp,
	RtpPacketizerWriteFn write_cb, void *opaque, int is_last,
	size_t max_payload, HevcRtpStats *stats);

#endif /* HEVC_RTP_H */
