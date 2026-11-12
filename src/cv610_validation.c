#include "venc_api.h"
#include "cv610_modes.h"

#include <math.h>
#include <string.h>

/* Validate the deliberately narrow CV610 feature set for both startup config
 * loading and staged HTTP mutations. */
const char *cv610_validate_config(const VencConfig *cfg)
{
	const Cv610Sensor *sensor = cv610_sensor_current();
	const Cv610SensorMode *mode;
	VencOutputUri uri;
	const char *err;

	if (!cfg)
		return "missing config";
	/* sensor.mode and video0.fps select the sensor mode together, the way
	 * they do on SigmaStar: an explicit sensor.mode wins, otherwise the rate
	 * is a target.  video0.size is the encoded size VPSS scales that capture
	 * down to.  /api/v1/modes lists the modes of the sensor fitted. */
	if (cfg->sensor.index > 0)
		return "CV610 has a single sensor pad: sensor.index must be -1 or 0";
	if (cfg->sensor.mode >= 0 &&
		(size_t)cfg->sensor.mode >= sensor->mode_count)
		return "CV610 sensor.mode is not an index listed by /api/v1/modes "
			"for the sensor $SENSOR names";
	mode = cv610_mode_select(sensor, cfg->sensor.mode, cfg->video0.fps,
		cfg->video0.width, cfg->video0.height, NULL);
	if (mode == NULL)
		return "CV610 video0.fps must name a mode listed by /api/v1/modes";
	err = cv610_mode_check_output(mode, cfg->video0.width,
		cfg->video0.height);
	if (err)
		return err;
	if (cfg->video0.bitrate < VENC_BITRATE_MIN_KBPS ||
		cfg->video0.bitrate > VENC_BITRATE_MAX_KBPS)
		return "video0.bitrate is outside the supported range";
	/* video0.qp_delta is NOT range-checked here, and is absent from CV610's
	 * supported-field list.  The backend cannot honour it -- the CBR rate
	 * controller stores gop_attr.normal_p.ip_qp_delta and ignores it -- so an
	 * inert field must not decide whether this craft boots.  Rejecting it
	 * meant a shared craft config carrying the portable qpDelta:-12, legal on
	 * Star6E and Maruko, stopped venc dead on CV610:
	 *
	 *   [venc_config] ERROR: invalid value in /etc/waybeam.json:
	 *   CV610 video0.qp_delta must be between -10 and 30
	 *
	 * The I-frame lever CV610 does have is video0.intra_refresh_qp. */
	/* Against the SELECTED mode's rate, which is what cv610_runtime.c turns
	 * into a GOP length.  Once sensor.mode can force a mode, or a target
	 * rate can be substituted, that is no longer video0.fps. */
	if (!isfinite(cfg->video0.gop_size) || cfg->video0.gop_size < 0.0 ||
		cfg->video0.gop_size * mode->fps > 65536.0)
		return "CV610 video0.gop_size exceeds the encoder's 65536-frame limit";
	if (strcmp(cfg->video0.rc_mode, "cbr") != 0)
		return "CV610 phase one supports video0.rc_mode=cbr only";
	if (strcmp(cfg->video0.framing, "off") != 0)
		return "CV610 phase one supports video0.framing=off only";
	if (cfg->audio.enabled &&
		(cfg->audio.sample_rate != 48000 || cfg->audio.channels != 1 ||
		 strcmp(cfg->audio.codec, "opus") != 0))
		return "CV610 audio requires 48000 Hz, mono, Opus";
	if (cfg->audio.enabled && !cfg->outgoing.enabled)
		return "CV610 audio requires outgoing output to be enabled";
	if (cfg->audio.enabled &&
		(cfg->outgoing.audio_port < 0 || cfg->outgoing.audio_port > 65535))
		return "CV610 audio requires outgoing.audio_port in range 0..65535";
	if (cfg->outgoing.max_payload_size < VENC_OUTPUT_PAYLOAD_MIN_BYTES ||
		cfg->outgoing.max_payload_size > VENC_OUTPUT_PAYLOAD_CEILING_BYTES)
		return "outgoing.max_payload_size is outside the supported range";
	if (!cfg->outgoing.enabled)
		return NULL;
	if (strcmp(cfg->outgoing.stream_mode, "rtp") != 0)
		return "CV610 phase one supports outgoing.stream_mode=rtp only";
	if (venc_config_parse_output_uri(cfg->outgoing.server, &uri) != 0)
		return "invalid outgoing.server URI";
	if (uri.type == VENC_OUTPUT_URI_SHM)
		return "CV610 phase one supports udp://, unix://, or frame-shm:// output";
	/* unix:// and frame-shm:// are local video transports. Their positive
	 * audio_port intentionally selects the Waybeam Link loopback UDP side
	 * channel; port 0 can inherit a destination only from UDP video. */
	if (cfg->audio.enabled && cfg->outgoing.audio_port == 0 &&
		uri.type != VENC_OUTPUT_URI_UDP)
		return "CV610 audio port 0 requires a UDP video destination";
	return NULL;
}

#ifndef HAVE_CV610_HTTP_API
const char *venc_api_validate_loaded_config(const VencConfig *cfg)
{
	return cv610_validate_config(cfg);
}
#endif
