#include <stdio.h>
#include "test_helpers.h"

/* Global test counters (defined here, declared extern in test_helpers.h) */
int g_test_pass_count;
int g_test_fail_count;

/* Test suite entry points */
extern int test_venc_config(void);
extern int test_venc_api(void);
extern int test_venc_httpd(void);
extern int test_sensor_select(void);
extern int test_venc_ring(void);
extern int test_file_util(void);
extern int test_h26x_util(void);
extern int test_h26x_param_sets(void);
extern int test_maruko_config(void);
extern int test_maruko_video(void);
extern int test_pipeline_common(void);
extern int test_codec_config(void);
extern int test_sdk_quiet(void);
extern int test_rtp_packetizer(void);
extern int test_hevc_rtp(void);
extern int test_isp_runtime(void);
extern int test_rtp_session(void);
extern int test_stream_metrics(void);
extern int test_star6e_hevc_rtp(void);
extern int test_star6e_output(void);
extern int test_star6e_audio(void);
extern int test_star6e_video(void);
extern int test_star6e_recorder(void);
extern int test_venc_rec_writer(void);
extern int test_ts_mux(void);
extern int test_audio_ring(void);
extern int test_star6e_ts_recorder(void);
extern int test_idr_rate_limit(void);
extern int test_backend(void);
extern int test_debug_osd(void);
extern int test_intra_refresh(void);
extern int test_frame_gate(void);
extern int test_venc_jpeg(void);
extern int test_mdns_beacon(void);
extern int test_framing_kalman(void);
extern int test_attitude_est(void);
extern int test_framing_stab_accuracy(void);
extern int test_venc_frame_ring(void);
extern int test_detect_dequant(void);
extern int test_detect_wire(void);
extern int test_star6e_vpe_ports(void);
extern int test_maruko_scl_ports(void);
extern int test_qr_scan(void);
extern int test_cv610_pq_bin_load(void);

int main(void)
{
	int failures = 0;

	printf("=== venc unit tests ===\n\n");

	printf("--- test_venc_config ---\n");
	failures += test_venc_config();

	printf("\n--- test_venc_api ---\n");
	failures += test_venc_api();

	printf("\n--- test_venc_httpd ---\n");
	failures += test_venc_httpd();

	printf("\n--- test_sensor_select ---\n");
	failures += test_sensor_select();

	printf("\n--- test_venc_ring ---\n");
	failures += test_venc_ring();

	printf("\n--- test_file_util ---\n");
	failures += test_file_util();

	printf("\n--- test_h26x_util ---\n");
	failures += test_h26x_util();

	printf("\n--- test_h26x_param_sets ---\n");
	failures += test_h26x_param_sets();

	printf("\n--- test_maruko_config ---\n");
	failures += test_maruko_config();

	printf("\n--- test_maruko_video ---\n");
	failures += test_maruko_video();

	printf("\n--- test_pipeline_common ---\n");
	failures += test_pipeline_common();

	printf("\n--- test_codec_config ---\n");
	failures += test_codec_config();

	printf("\n--- test_sdk_quiet ---\n");
	failures += test_sdk_quiet();

	printf("\n--- test_rtp_packetizer ---\n");
	failures += test_rtp_packetizer();

	printf("\n--- test_hevc_rtp ---\n");
	failures += test_hevc_rtp();

	printf("\n--- test_isp_runtime ---\n");
	failures += test_isp_runtime();

	printf("\n--- test_rtp_session ---\n");
	failures += test_rtp_session();

	printf("\n--- test_stream_metrics ---\n");
	failures += test_stream_metrics();

	printf("\n--- test_star6e_hevc_rtp ---\n");
	failures += test_star6e_hevc_rtp();

	printf("\n--- test_star6e_output ---\n");
	failures += test_star6e_output();

	printf("\n--- test_star6e_audio ---\n");
	failures += test_star6e_audio();

	printf("\n--- test_star6e_video ---\n");
	failures += test_star6e_video();

	printf("\n--- test_star6e_recorder ---\n");
	failures += test_star6e_recorder();
	printf("\n--- test_venc_rec_writer ---\n");
	failures += test_venc_rec_writer();

	printf("\n--- test_ts_mux ---\n");
	failures += test_ts_mux();

	printf("\n--- test_audio_ring ---\n");
	failures += test_audio_ring();

	printf("\n--- test_star6e_ts_recorder ---\n");
	failures += test_star6e_ts_recorder();

	printf("\n--- test_idr_rate_limit ---\n");
	failures += test_idr_rate_limit();

	printf("\n--- test_backend ---\n");
	failures += test_backend();

	printf("\n--- test_debug_osd ---\n");
	failures += test_debug_osd();

	printf("\n--- test_intra_refresh ---\n");
	failures += test_intra_refresh();

	printf("\n--- test_frame_gate ---\n");
	failures += test_frame_gate();

	printf("\n--- test_venc_jpeg ---\n");
	failures += test_venc_jpeg();

	printf("\n--- test_mdns_beacon ---\n");
	failures += test_mdns_beacon();

	printf("\n--- test_framing_kalman ---\n");
	failures += test_framing_kalman();

	printf("\n--- test_attitude_est ---\n");
	failures += test_attitude_est();

	printf("\n--- test_framing_stab_accuracy ---\n");
	failures += test_framing_stab_accuracy();

	printf("\n--- test_venc_frame_ring ---\n");
	failures += test_venc_frame_ring();

	printf("\n--- test_detect_dequant ---\n");
	failures += test_detect_dequant();

	printf("\n--- test_detect_wire ---\n");
	failures += test_detect_wire();

	printf("\n--- test_star6e_vpe_ports ---\n");
	failures += test_star6e_vpe_ports();

	printf("\n--- test_maruko_scl_ports ---\n");
	failures += test_maruko_scl_ports();

	printf("\n--- test_cv610_pq_bin_load ---\n");
	failures += test_cv610_pq_bin_load();

	failures += test_qr_scan();

	printf("\n=== Results: %d passed, %d failed ===\n",
		g_test_pass_count, g_test_fail_count);

	return failures > 0 ? 1 : 0;
}
