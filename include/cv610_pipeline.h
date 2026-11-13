#ifndef CV610_PIPELINE_H
#define CV610_PIPELINE_H

#include <stdint.h>

#include "cv610_modes.h"

typedef struct {
	/* Which plugin to dlopen and which sensor id the ISP binds. */
	const Cv610Sensor *sensor;
	/* Sensor capture geometry (what VI and the ISP see). */
	uint32_t width;
	uint32_t height;
	/* Encoded geometry (what VPSS scales to and VENC encodes).  Equal to
	 * the capture size unless video0.size asks for something smaller. */
	uint32_t out_width;
	uint32_t out_height;
	/* isp.keepAspect: centre-crop the capture to the encoded aspect ratio
	 * before scaling, so a 4:3 video0.size out of a 16:9 sensor is framed
	 * rather than squashed.  Same rule Star6E and Maruko apply. */
	int keep_aspect;
	uint32_t fps;
	int lanes;
	int data_rate_x2;
	int bayer;
	int raw_bit;
	/* image.mirror / image.flip.  Applied at the SENSOR (the plugin's
	 * pfn_mirror_flip), matching what both SigmaStar backends do, so
	 * `image.*` means the same thing on every board and the encoder is not
	 * asked to do geometry.
	 *
	 * The Bayer start phase does NOT move with the readout on IMX662 —
	 * measured, see isp_setup().  The textbook answer is one XOR per axis
	 * into pub.bayer_format and it is the WRONG one here; do not add it
	 * back without re-running that A/B. */
	int mirror;
	int flip;
	/* MCLK this mode's sensor line timing assumes.  Zero leaves whatever
	 * the loader set, which suits exactly one mode. */
	uint32_t sensor_clock_hz;
	int i2c_bus;
} Cv610PipelineConfig;

/* Start the MIPI -> VI -> ISP graph for config->sensor. */
int cv610_pipeline_start(const Cv610PipelineConfig *cfg);

/* Stop the ISP thread and release sensor, VI, SYS, and VB resources. */
void cv610_pipeline_stop(void);

/* Signal-safe stop flag used by the backend's SIGINT/SIGTERM handler. */
void cv610_pipeline_request_stop(void);
int cv610_pipeline_stop_requested(void);

/* Nonzero once ss_mpi_isp_run() is up, so callers outside this file know the
 * ISP will answer an ss_mpi_isp_* attribute call.  Used by cv610_iq.c to
 * refuse rather than fire MPI ioctls at an ISP that was never inited. */
int cv610_pipeline_isp_ready(void);

#endif /* CV610_PIPELINE_H */
