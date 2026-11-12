#include "maruko_config.h"

#include "codec_config.h"
#include "pipeline_common.h"
#include "rtp_packetizer.h"

#include <stdio.h>
#include <string.h>

void maruko_config_defaults(MarukoBackendConfig *cfg)
{
	if (!cfg) {
		return;
	}

	memset(cfg, 0, sizeof(*cfg));
	cfg->sensor_width = 1920;
	cfg->sensor_height = 1080;
	cfg->image_width = 1920;
	cfg->image_height = 1080;
	cfg->sensor_fps = 30;
	cfg->venc_max_rate = 8192;
	cfg->venc_gop_size = 30;
	cfg->venc_gop_seconds = 1.0;
	cfg->max_frame_size = RTP_DEFAULT_PAYLOAD;
	cfg->output_uri.type = VENC_OUTPUT_URI_UDP;
	snprintf(cfg->output_uri.host, sizeof(cfg->output_uri.host), "%s",
		"127.0.0.1");
	cfg->output_uri.port = 5000;
	cfg->rc_codec = PT_H265;
	cfg->rc_mode = 3;
	cfg->stream_mode = MARUKO_STREAM_RTP;
	cfg->forced_sensor_pad = (MI_SNR_PAD_ID_e)-1;
	cfg->forced_sensor_mode = -1;
	cfg->isp_bin_path[0] = '\0';
	cfg->vpe_level_3dnr = 1;
	cfg->verbose = 0;
	cfg->connected_udp = 1;  /* match VencConfig default */
	cfg->allow_unix_encoder_stall = 0;
	cfg->keep_aspect = 1;    /* match VencConfig default (true) */
	cfg->show_osd = 0;
	cfg->ae_fps = 15;
	cfg->isp_gain_max = 0;
	cfg->isp_gain_min = 0;
	snprintf(cfg->intra_refresh_mode, sizeof(cfg->intra_refresh_mode),
		"%s", "off");
	cfg->ref_base = 0;
	cfg->ref_enhance = 0;
	cfg->ref_pred = 1;
	cfg->slice_count = 1;
	cfg->frame_gate_close_slots = 0;
	cfg->frame_gate_max_closed_ms = 0;
	snprintf(cfg->resilience, sizeof(cfg->resilience), "%s", "off");
	memset(&cfg->imu, 0, sizeof(cfg->imu));
	memset(&cfg->audio, 0, sizeof(cfg->audio));
	cfg->audio_port = 5601;
	cfg->max_payload_size = RTP_DEFAULT_PAYLOAD;
	memset(&cfg->record, 0, sizeof(cfg->record));
	snprintf(cfg->record.mode, sizeof(cfg->record.mode), "%s", "off");
	snprintf(cfg->record.format, sizeof(cfg->record.format), "%s", "ts");
	cfg->record.max_seconds = 300;
	cfg->record.max_mb = 500;

	cfg->snapshot.enabled = true;
	cfg->snapshot.quality = 80;
	cfg->snapshot.channel = 7;
	cfg->snapshot.width   = 0;
	cfg->snapshot.height  = 0;
}

int maruko_config_from_venc(const VencConfig *vcfg, MarukoBackendConfig *cfg)
{
	if (!vcfg || !cfg) {
		return -1;
	}

	maruko_config_defaults(cfg);

	cfg->sensor_width = vcfg->video0.width;
	cfg->sensor_height = vcfg->video0.height;
	cfg->image_width = vcfg->video0.width;
	cfg->image_height = vcfg->video0.height;
	cfg->sensor_fps = vcfg->video0.fps;
	cfg->venc_max_rate = vcfg->video0.bitrate;
	cfg->venc_gop_seconds = vcfg->video0.gop_size;
	cfg->venc_gop_size = pipeline_common_gop_frames(cfg->venc_gop_seconds,
		cfg->sensor_fps);

	cfg->max_frame_size = vcfg->outgoing.max_payload_size;
	cfg->rtp_payload_size = vcfg->outgoing.max_payload_size;
	cfg->sidecar_port = vcfg->outgoing.sidecar_port;

	if (vcfg->outgoing.server[0] &&
	    venc_config_parse_output_uri(vcfg->outgoing.server,
	    &cfg->output_uri) != 0)
		return -1;

	cfg->stream_mode =
		(strcmp(vcfg->outgoing.stream_mode, "compact") == 0) ?
		MARUKO_STREAM_COMPACT : MARUKO_STREAM_RTP;

	if (codec_config_resolve_codec_rc(vcfg->video0.rc_mode,
	    &cfg->rc_codec, &cfg->rc_mode) != 0) {
		return -1;
	}

	cfg->forced_sensor_pad = (vcfg->sensor.index >= 0) ?
		(MI_SNR_PAD_ID_e)vcfg->sensor.index : (MI_SNR_PAD_ID_e)-1;
	cfg->forced_sensor_mode = vcfg->sensor.mode;
	/* Configured path is copied verbatim; pipeline resolves it after
	 * sensor_select runs (may pick the per-sensor fallback). */
	snprintf(cfg->isp_bin_path, sizeof(cfg->isp_bin_path), "%s",
		vcfg->isp.sensor_bin);
	cfg->vpe_level_3dnr = vcfg->fpv.noise_level;
	cfg->sensor_unlock = (SensorUnlockConfig){
		.enabled = vcfg->sensor.unlock_enabled ? 1 : 0,
		.cmd_id = vcfg->sensor.unlock_cmd,
		.reg = vcfg->sensor.unlock_reg,
		.value = vcfg->sensor.unlock_value,
		.dir = (MI_SNR_CustDir_e)vcfg->sensor.unlock_dir,
	};
	cfg->oc_level = vcfg->system.overclock_level;
	cfg->scene_threshold = vcfg->video0.scene_threshold;
	cfg->scene_holdoff = vcfg->video0.scene_holdoff;
	snprintf(cfg->intra_refresh_mode, sizeof(cfg->intra_refresh_mode), "%s",
		vcfg->video0.intra_refresh_mode);
	cfg->intra_refresh_lines = vcfg->video0.intra_refresh_lines;
	cfg->intra_refresh_qp = vcfg->video0.intra_refresh_qp;
	cfg->ref_base = vcfg->video0.ref_base;
	cfg->ref_enhance = vcfg->video0.ref_enhance;
	cfg->ref_pred = vcfg->video0.ref_pred ? 1 : 0;
	cfg->slice_count = vcfg->video0.slice_count;
	cfg->frame_gate_close_slots = vcfg->video0.frame_gate_close_slots;
	cfg->frame_gate_max_closed_ms =
		vcfg->video0.frame_gate_max_closed_ms;
	snprintf(cfg->resilience, sizeof(cfg->resilience), "%s",
		vcfg->video0.resilience);
	cfg->gop_size_sec = vcfg->video0.gop_size;
	cfg->zoom_pct = vcfg->video0.zoom_pct;
	cfg->zoom_x   = vcfg->video0.zoom_x;
	cfg->zoom_y   = vcfg->video0.zoom_y;
	cfg->image_mirror = vcfg->image.mirror ? 1 : 0;
	cfg->image_flip   = vcfg->image.flip   ? 1 : 0;
	cfg->verbose = vcfg->system.verbose ? 1 : 0;
	cfg->connected_udp = vcfg->outgoing.connected_udp ? 1 : 0;
	cfg->allow_unix_encoder_stall =
		vcfg->outgoing.allow_unix_encoder_stall ? 1 : 0;
	cfg->keep_aspect = vcfg->isp.keep_aspect ? 1 : 0;
	cfg->show_osd = vcfg->debug.show_osd ? 1 : 0;
	cfg->ae_fps = vcfg->isp.ae_fps;
	cfg->isp_gain_max = vcfg->isp.gain_max;
	cfg->isp_shutter_max_us = vcfg->isp.shutter_max_us;
	cfg->isp_gain_min = vcfg->isp.gain_min;
	cfg->isp_shutter_min_us = vcfg->isp.shutter_min_us;
	cfg->imu = vcfg->imu;
	cfg->audio = vcfg->audio;
	cfg->audio_port = vcfg->outgoing.audio_port;
	cfg->max_payload_size = vcfg->outgoing.max_payload_size;

	cfg->record.enabled = vcfg->record.enabled ? 1 : 0;
	snprintf(cfg->record.mode, sizeof(cfg->record.mode), "%s",
		vcfg->record.mode);
	snprintf(cfg->record.dir, sizeof(cfg->record.dir), "%s",
		vcfg->record.dir);
	snprintf(cfg->record.format, sizeof(cfg->record.format), "%s",
		vcfg->record.format[0] ? vcfg->record.format : "ts");
	snprintf(cfg->record.server, sizeof(cfg->record.server), "%s",
		vcfg->record.server);
	cfg->record.bitrate = vcfg->record.bitrate;
	cfg->record.fps = vcfg->record.fps;
	cfg->record.gop_size = vcfg->record.gop_size;
	cfg->record.max_seconds = vcfg->record.max_seconds;
	cfg->record.max_mb = vcfg->record.max_mb;

	cfg->snapshot = vcfg->snapshot;

	snprintf(cfg->framing, sizeof(cfg->framing), "%s",
		vcfg->video0.framing);

	cfg->shutter_rule_180 = vcfg->isp.shutter_rule_180 ? 1 : 0;

	return 0;
}
