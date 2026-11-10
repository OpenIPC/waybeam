/*
 * Unit tests for venc_api.c
 *
 * Tests: field descriptor lookup, registration, mutability,
 * field serialization/deserialization.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "venc_config.h"
#include "venc_api.h"
#include "venc_httpd.h"
#include "star6e.h"
#include "test_helpers.h"

/* Stubs for MI_VENC functions used by the dual VENC API in venc_api.c.
 * The test binary doesn't link the SDK, so these satisfy the linker. */
MI_S32 MI_VENC_GetChnAttr(MI_VENC_CHN chn, MI_VENC_ChnAttr_t *attr)
{
	(void)chn; (void)attr; return -1;
}
MI_S32 MI_VENC_SetChnAttr(MI_VENC_CHN chn, MI_VENC_ChnAttr_t *attr)
{
	(void)chn; (void)attr; return -1;
}
MI_S32 MI_VENC_RequestIdr(MI_VENC_CHN chn, MI_BOOL instant)
{
	(void)chn; (void)instant; return -1;
}

typedef struct {
	int apply_bitrate_calls;
	int apply_fps_calls;
	int apply_gop_calls;
	int apply_qp_delta_calls;
	int apply_verbose_calls;
	int apply_awb_mode_calls;
	int apply_awb_rate_calls;
	uint32_t last_awb_rate;
	int apply_server_calls;
	int apply_output_enabled_calls;
	bool last_output_enabled;
	int apply_max_payload_calls;
	int apply_zoom_calls;
	int apply_isp_bin_calls;
	int apply_detect_reload_calls;

	uint32_t last_bitrate;
	uint32_t last_fps;
	uint32_t last_gop;
	int last_qp_delta;
	bool last_verbose;
	int last_awb_mode;
	uint32_t last_awb_ct;
	char last_server[128];
	/* Set by a test that needs to inspect the committed config from
	 * inside an apply callback (see the commit-ordering test). */
	const VencConfig *live_cfg;
	int server_committed_at_callback;
	uint16_t last_max_payload;
	double last_zoom_pct;
	double last_zoom_x;
	double last_zoom_y;
	char last_isp_bin[256];

	int fail_bitrate;
	int fail_verbose;
	int fail_fps;
	int fail_gop;
	int fail_server;
	int fail_max_payload;
	int fail_zoom;
	int fail_isp_bin;
} ApiCallbackState;

static ApiCallbackState g_api_cb_state;
static uint16_t g_api_test_port;
static int g_api_test_server_started;

static void reset_api_cb_state(void)
{
	memset(&g_api_cb_state, 0, sizeof(g_api_cb_state));
}

static uint16_t reserve_test_port(void)
{
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);
	uint16_t port = 0;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return 0;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(0);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
	    getsockname(fd, (struct sockaddr *)&addr, &addr_len) == 0) {
		port = ntohs(addr.sin_port);
	}

	close(fd);
	return port;
}

static int ensure_api_test_server(void)
{
	if (g_api_test_server_started)
		return 0;

	g_api_test_port = reserve_test_port();
	if (g_api_test_port == 0)
		return -1;
	if (venc_httpd_start(g_api_test_port) != 0) {
		g_api_test_port = 0;
		return -1;
	}

	g_api_test_server_started = 1;
	return 0;
}

static void stop_api_test_server(void)
{
	if (!g_api_test_server_started)
		return;

	venc_httpd_stop();
	g_api_test_server_started = 0;
	g_api_test_port = 0;
}

static int connect_api_test_socket(void)
{
	struct sockaddr_in addr;
	int fd;
	int attempt;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(g_api_test_port);

	for (attempt = 0; attempt < 50; attempt++) {
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			return -1;

		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
			return fd;

		close(fd);
		if (errno != ECONNREFUSED && errno != ENOENT)
			break;
		usleep(10000);
	}

	return -1;
}

static int read_http_response(int fd, int *http_status, char *response_buf,
	size_t response_buf_size)
{
	/* Must exceed the largest response under test — /api/v1/capabilities
	 * grows with every config field and silently truncated here before. */
	char raw[65536];
	char *body;
	size_t used = 0;
	ssize_t n;

	while (used < sizeof(raw) - 1) {
		n = read(fd, raw + used, sizeof(raw) - 1 - used);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		used += (size_t)n;
	}
	raw[used] = '\0';

	if (http_status) {
		int status = 0;
		if (sscanf(raw, "HTTP/%*u.%*u %d", &status) != 1)
			return -1;
		*http_status = status;
	}

	if (response_buf && response_buf_size > 0) {
		body = strstr(raw, "\r\n\r\n");
		if (!body)
			return -1;
		snprintf(response_buf, response_buf_size, "%s", body + 4);
	}

	return 0;
}

static int apply_query_http_path(VencConfig *cfg, const char *backend_name,
	const VencApplyCallbacks *cb, const char *path, const char *query,
	int *http_status, char *response_buf, size_t response_buf_size)
{
	char request[1024];
	int fd;
	size_t sent = 0;
	size_t req_len;

	if (venc_api_register(cfg, backend_name, cb, NULL) != 0)
		return -1;
	if (ensure_api_test_server() != 0)
		return -1;

	fd = connect_api_test_socket();
	if (fd < 0)
		return -1;

	req_len = (size_t)snprintf(request, sizeof(request),
		"GET %s?%s HTTP/1.0\r\n"
		"Host: 127.0.0.1\r\n"
		"\r\n",
		path, query ? query : "");
	if (req_len >= sizeof(request)) {
		close(fd);
		return -1;
	}

	while (sent < req_len) {
		ssize_t nwrite = write(fd, request + sent, req_len - sent);
		if (nwrite < 0 && errno == EINTR)
			continue;
		if (nwrite <= 0) {
			close(fd);
			return -1;
		}
		sent += (size_t)nwrite;
	}

	shutdown(fd, SHUT_WR);
	if (read_http_response(fd, http_status, response_buf,
	    response_buf_size) != 0) {
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

static int apply_set_query_http(VencConfig *cfg, const char *backend_name,
	const VencApplyCallbacks *cb, const char *query, int *http_status,
	char *response_buf, size_t response_buf_size)
{
	return apply_query_http_path(cfg, backend_name, cb, "/api/v1/set",
		query, http_status, response_buf, response_buf_size);
}

static int test_apply_bitrate(uint32_t kbps)
{
	g_api_cb_state.apply_bitrate_calls++;
	g_api_cb_state.last_bitrate = kbps;
	return g_api_cb_state.fail_bitrate ? -1 : 0;
}

static int test_apply_fps(uint32_t fps)
{
	g_api_cb_state.apply_fps_calls++;
	g_api_cb_state.last_fps = fps;
	return g_api_cb_state.fail_fps ? -1 : 0;
}

static int test_apply_gop(uint32_t gop_size)
{
	g_api_cb_state.apply_gop_calls++;
	g_api_cb_state.last_gop = gop_size;
	return g_api_cb_state.fail_gop ? -1 : 0;
}

static int test_apply_qp_delta(int delta)
{
	g_api_cb_state.apply_qp_delta_calls++;
	g_api_cb_state.last_qp_delta = delta;
	return 0;
}

static int test_apply_verbose(bool on)
{
	g_api_cb_state.apply_verbose_calls++;
	g_api_cb_state.last_verbose = on;
	return g_api_cb_state.fail_verbose ? -1 : 0;
}

static int test_apply_awb_mode(int mode, uint32_t ct)
{
	g_api_cb_state.apply_awb_mode_calls++;
	g_api_cb_state.last_awb_mode = mode;
	g_api_cb_state.last_awb_ct = ct;
	return 0;
}

static int test_apply_awb_rate(uint32_t hz)
{
	g_api_cb_state.apply_awb_rate_calls++;
	g_api_cb_state.last_awb_rate = hz;
	return 0;
}

static int test_apply_output_enabled(bool on)
{
	g_api_cb_state.apply_output_enabled_calls++;
	g_api_cb_state.last_output_enabled = on;
	return 0;
}

/* Accepts the value but asks for a respawn -- the shape CV610's apply_server()
 * uses for the ring transports, which cannot be switched in place but must
 * still be settable through the API. */
static int test_apply_server_needs_restart(const char *uri)
{
	(void)uri;
	g_api_cb_state.apply_server_calls++;
	venc_api_request_reinit();
	return 0;
}

static int test_apply_server(const char *uri)
{
	g_api_cb_state.apply_server_calls++;
	snprintf(g_api_cb_state.last_server, sizeof(g_api_cb_state.last_server),
		"%s", uri ? uri : "");
	if (g_api_cb_state.live_cfg && uri &&
	    strcmp(g_api_cb_state.live_cfg->outgoing.server, uri) == 0)
		g_api_cb_state.server_committed_at_callback = 1;
	return g_api_cb_state.fail_server ? -1 : 0;
}

static int test_apply_max_payload(uint16_t size)
{
	g_api_cb_state.apply_max_payload_calls++;
	g_api_cb_state.last_max_payload = size;
	return g_api_cb_state.fail_max_payload ? -1 : 0;
}

static int test_apply_zoom(double pct, double x, double y)
{
	g_api_cb_state.apply_zoom_calls++;
	g_api_cb_state.last_zoom_pct = pct;
	g_api_cb_state.last_zoom_x = x;
	g_api_cb_state.last_zoom_y = y;
	return g_api_cb_state.fail_zoom ? -1 : 0;
}

static int test_apply_isp_bin(const char *path)
{
	g_api_cb_state.apply_isp_bin_calls++;
	snprintf(g_api_cb_state.last_isp_bin,
		sizeof(g_api_cb_state.last_isp_bin), "%s", path ? path : "");
	return g_api_cb_state.fail_isp_bin ? -1 : 0;
}

static int test_apply_detect_reload(void)
{
	g_api_cb_state.apply_detect_reload_calls++;
	return 0;
}

/* Whitebox access to internal functions via extern declarations.
 * These are static in venc_api.c — we re-declare them here for testing.
 * This pattern matches the waybeam-hub test approach. */

/* We can't directly access statics, so we test through the public
 * venc_api_register() interface and verify side effects. */

/* ── Stub handler to capture responses ───────────────────────────────── */

/* For httpd route tests, we just verify registration succeeds */

/* ── Tests ───────────────────────────────────────────────────────────── */

static int test_register(void)
{
	int failures = 0;
	VencConfig cfg;
	venc_config_defaults(&cfg);

	/* Registration with NULL callbacks should succeed */
	int ret = venc_api_register(&cfg, "test", NULL, NULL);
	CHECK("register_ok", ret == 0);

	return failures;
}

static int test_active_precrop_setter(void)
{
	int failures = 0;
	uint16_t x = 0xAA, y = 0xBB, w = 0xCC, h = 0xDD;

	/* Cleared store: getter returns 0 (invalid), out args untouched. */
	venc_api_clear_active_precrop();
	CHECK("active_precrop initial invalid",
		venc_api_get_active_precrop(&x, &y, &w, &h) == 0);
	CHECK("active_precrop unread x", x == 0xAA);
	CHECK("active_precrop unread y", y == 0xBB);
	CHECK("active_precrop unread w", w == 0xCC);
	CHECK("active_precrop unread h", h == 0xDD);

	venc_api_set_active_precrop(240, 0, 1440, 1080);
	CHECK("active_precrop set valid",
		venc_api_get_active_precrop(&x, &y, &w, &h) == 1);
	CHECK("active_precrop set x", x == 240);
	CHECK("active_precrop set y", y == 0);
	CHECK("active_precrop set w", w == 1440);
	CHECK("active_precrop set h", h == 1080);

	/* Overwrite from a subsequent reinit. */
	venc_api_set_active_precrop(0, 240, 2560, 1440);
	CHECK("active_precrop overwrite",
		venc_api_get_active_precrop(&x, &y, &w, &h) == 1);
	CHECK("active_precrop overwrite x", x == 0);
	CHECK("active_precrop overwrite y", y == 240);
	CHECK("active_precrop overwrite w", w == 2560);
	CHECK("active_precrop overwrite h", h == 1440);

	/* Pipeline stop clears the store. */
	venc_api_clear_active_precrop();
	CHECK("active_precrop cleared",
		venc_api_get_active_precrop(&x, &y, &w, &h) == 0);

	/* NULL out-pointers must not crash even when valid. */
	venc_api_set_active_precrop(1, 2, 3, 4);
	CHECK("active_precrop null safe",
		venc_api_get_active_precrop(NULL, NULL, NULL, NULL) == 1);
	venc_api_clear_active_precrop();

	return failures;
}

static int test_register_with_callbacks(void)
{
	int failures = 0;
	VencConfig cfg;
	venc_config_defaults(&cfg);

	VencApplyCallbacks cb;
	memset(&cb, 0, sizeof(cb));

	int ret = venc_api_register(&cfg, "star6e", &cb, NULL);
	CHECK("register_cb_ok", ret == 0);

	return failures;
}

static int test_field_support_by_backend(void)
{
	int failures = 0;

	CHECK("scene_threshold supported star6e",
		venc_api_field_supported_for_backend("star6e",
			"video0.scene_threshold") == 1);
	CHECK("scene_threshold alias supported star6e",
		venc_api_field_supported_for_backend("star6e",
			"video0.sceneThreshold") == 1);
	CHECK("scene_threshold supported maruko",
		venc_api_field_supported_for_backend("maruko",
			"video0.scene_threshold") == 1);
	CHECK("scene_holdoff supported maruko",
		venc_api_field_supported_for_backend("maruko",
			"video0.sceneHoldoff") == 1);
	CHECK("regular field supported maruko",
		venc_api_field_supported_for_backend("maruko",
			"video0.bitrate") == 1);
	/* The userspace AWB loop is Star6E-only; Maruko greys the control. */
	CHECK("awb_fps supported star6e",
		venc_api_field_supported_for_backend("star6e",
			"isp.awb_fps") == 1);
	CHECK("awb_fps unsupported maruko",
		venc_api_field_supported_for_backend("maruko",
			"isp.awb_fps") == 0);
	CHECK("awb_fps alias unsupported maruko",
		venc_api_field_supported_for_backend("maruko",
			"isp.awbFps") == 0);
	CHECK("qr unsupported maruko",
		venc_api_field_supported_for_backend("maruko",
			"qr.tap_enabled") == 0);
	CHECK("qr alias unsupported maruko",
		venc_api_field_supported_for_backend("maruko",
			"qr.windowMs") == 0);
	CHECK("min qp supported star6e",
		venc_api_field_supported_for_backend("star6e",
			"video0.minQp") == 1);
	/* Maruko gained QP bounds with maruko_apply_qp_bounds(): same MI VENC
	 * RC as Star6E, so the same u32MinQp/u32MaxQp write.  Device-verified
	 * on .233 -- minQp 40 took the stream from 1.54 Mbps to 0.09. */
	CHECK("min qp supported maruko",
		venc_api_field_supported_for_backend("maruko",
			"video0.minQp") == 1);
	CHECK("max qp supported maruko",
		venc_api_field_supported_for_backend("maruko",
			"video0.max_qp") == 1);
	CHECK("slice count supported maruko",
		venc_api_field_supported_for_backend("maruko",
			"video0.sliceCount") == 1);
	CHECK("bitrate supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"video0.bitrate") == 1);
	CHECK("gop alias supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"video0.gopSize") == 1);
	CHECK("resilience supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"video0.resilience") == 1);
	CHECK("slice count supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"video0.sliceCount") == 1);
	/* The adaptive frame gate is wired on CV610: cv610_runtime.c passes
	 * both tunables to frame_gate_setup().  Greyed controls here while a
	 * hand-edited config was honoured is the parity gap this guards. */
	CHECK("frame gate close slots supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"video0.frame_gate_close_slots") == 1);
	CHECK("frame gate max closed supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"video0.frame_gate_max_closed_ms") == 1);
	CHECK("frame gate close slots alias supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"video0.frameGateCloseSlots") == 1);
	CHECK("frame gate max closed alias supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"video0.frameGateMaxClosedMs") == 1);
	CHECK("frame gate close slots supported star6e",
		venc_api_field_supported_for_backend("star6e",
			"video0.frame_gate_close_slots") == 1);
	CHECK("frame gate max closed supported maruko",
		venc_api_field_supported_for_backend("maruko",
			"video0.frame_gate_max_closed_ms") == 1);
	CHECK("opus config API unsupported cv610",
		venc_api_field_supported_for_backend("cv610",
			"audio.codec") == 0);
	/* audio.enabled and audio.mute ARE read by cv610_audio_start(); the
	 * rest of the audio group is hardcoded there, so it stays unsupported
	 * even though the shipped defaults happen to match the constants. */
	CHECK("audio enable supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"audio.enabled") == 1);
	CHECK("audio mute supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"audio.mute") == 1);
	CHECK("audio volume unsupported cv610",
		venc_api_field_supported_for_backend("cv610",
			"audio.volume") == 0);
	CHECK("audio sample rate unsupported cv610",
		venc_api_field_supported_for_backend("cv610",
			"audio.sample_rate") == 0);
	CHECK("output fps supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"video0.fps") == 1);
	CHECK("output server supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"outgoing.server") == 1);
	CHECK("rc mode unsupported cv610",
		venc_api_field_supported_for_backend("cv610",
			"video0.rc_mode") == 0);
	/* The sidecar producer is compiled into the CV610 binary, so the port
	 * is a field the backend genuinely reads rather than one that accepts
	 * a value and does nothing.  Checked on all three backends: the entry
	 * was added to CV610's allowlist, not to the shared gate. */
	CHECK("sidecar port supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"outgoing.sidecar_port") == 1);
	CHECK("sidecar port alias supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"outgoing.sidecarPort") == 1);
	CHECK("sidecar port supported star6e",
		venc_api_field_supported_for_backend("star6e",
			"outgoing.sidecar_port") == 1);
	CHECK("sidecar port supported maruko",
		venc_api_field_supported_for_backend("maruko",
			"outgoing.sidecar_port") == 1);
	/* ROI reaches CV610 through ss_mpi_venc_set_roi_attr().  fpv.noise_level
	 * is the control here and is deliberately NOT in the list: it proves the
	 * allowlist matches whole field names, so a prefix match on "fpv." would
	 * fail this block rather than quietly advertising a field the backend
	 * never reads. */
	CHECK("roi enabled supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"fpv.roi_enabled") == 1);
	CHECK("roi qp supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"fpv.roi_qp") == 1);
	CHECK("roi steps supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"fpv.roi_steps") == 1);
	CHECK("roi center supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"fpv.roi_center") == 1);
	CHECK("roi qp alias supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"fpv.roiQp") == 1);
	CHECK("roi center alias supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"fpv.roiCenter") == 1);
	CHECK("fpv noise level unsupported cv610",
		venc_api_field_supported_for_backend("cv610",
			"fpv.noise_level") == 0);
	CHECK("roi qp supported star6e",
		venc_api_field_supported_for_backend("star6e",
			"fpv.roi_qp") == 1);
	CHECK("roi qp supported maruko",
		venc_api_field_supported_for_backend("maruko",
			"fpv.roi_qp") == 1);
	/* The two portable AE ceilings map onto CV610's exposure group with no
	 * unit conversion (us and 22.10 gain, both documented in the SDK header
	 * and confirmed on the live board).  The floors and the AWB pair are the
	 * controls here: gain_min/shutter_min_us were not measured in this
	 * slice, and awb_mode/awb_ct cannot be honoured at all because the ISP
	 * has no Kelvin input.  All four must stay unsupported on CV610 while
	 * remaining supported on the SigmaStar backends -- which is what
	 * distinguishes a per-field allowlist entry from an "isp." prefix. */
	CHECK("gain max supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"isp.gain_max") == 1);
	CHECK("shutter max supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"isp.shutter_max_us") == 1);
	CHECK("gain max alias supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"isp.gainMax") == 1);
	CHECK("shutter max alias supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"isp.shutterMaxUs") == 1);
	CHECK("gain min unsupported cv610",
		venc_api_field_supported_for_backend("cv610",
			"isp.gain_min") == 0);
	CHECK("shutter min unsupported cv610",
		venc_api_field_supported_for_backend("cv610",
			"isp.shutter_min_us") == 0);
	CHECK("awb mode unsupported cv610",
		venc_api_field_supported_for_backend("cv610",
			"isp.awb_mode") == 0);
	CHECK("awb ct unsupported cv610",
		venc_api_field_supported_for_backend("cv610",
			"isp.awb_ct") == 0);
	CHECK("gain max supported star6e",
		venc_api_field_supported_for_backend("star6e",
			"isp.gain_max") == 1);
	CHECK("shutter max supported maruko",
		venc_api_field_supported_for_backend("maruko",
			"isp.shutter_max_us") == 1);
	CHECK("awb mode supported star6e",
		venc_api_field_supported_for_backend("star6e",
			"isp.awb_mode") == 1);
	CHECK("gain min supported maruko",
		venc_api_field_supported_for_backend("maruko",
			"isp.gain_min") == 1);
	/* Sensor orientation reaches CV610 through the plugin's
	 * pfn_mirror_flip, the same place the SigmaStar backends apply it.
	 * Checked on all three so the entry lands in CV610's allowlist rather
	 * than the shared gate. */
	CHECK("image mirror supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"image.mirror") == 1);
	CHECK("image flip supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"image.flip") == 1);
	CHECK("image mirror supported star6e",
		venc_api_field_supported_for_backend("star6e",
			"image.mirror") == 1);
	CHECK("image flip supported maruko",
		venc_api_field_supported_for_backend("maruko",
			"image.flip") == 1);
	/* image.rotate stays UNSUPPORTED on cv610 even though the pair it
	 * decomposes into is now supported.  The decomposition runs in
	 * venc_config's load_image(), on a file parse only, so a value that
	 * arrives through /api/v1/set is never decomposed and never read by
	 * any backend — advertising it would accept 90, persist it, raise
	 * reinit_pending and read back 0.  A config file with rotate:180
	 * still works; the field is unsupported because nothing reads ROTATE.
	 * Device-measured on .181 before this was reverted. */
	CHECK("image rotate unsupported cv610",
		venc_api_field_supported_for_backend("cv610",
			"image.rotate") == 0);
	/* Recording and snapshot arrived on CV610 in mirror mode: the main
	 * channel's access unit is teed to file, and the JPEG channel is a
	 * second bind target on the main stream's VPSS output. */
	CHECK("recording supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"record.enabled") == 1);
	CHECK("record dir supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"record.dir") == 1);
	CHECK("record format supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"record.format") == 1);
	/* dual / dual-stream need a second VENC channel, which this backend
	 * does not create — so the fields that only describe that channel stay
	 * unsupported rather than being accepted and ignored. */
	CHECK("record bitrate unsupported cv610",
		venc_api_field_supported_for_backend("cv610",
			"record.bitrate") == 0);
	CHECK("record server unsupported cv610",
		venc_api_field_supported_for_backend("cv610",
			"record.server") == 0);
	CHECK("snapshot enabled supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"snapshot.enabled") == 1);
	CHECK("snapshot quality supported cv610",
		venc_api_field_supported_for_backend("cv610",
			"snapshot.quality") == 1);
	/* The JPEG channel shares the main stream's VPSS output, so it cannot
	 * honour an independent snapshot geometry. */
	CHECK("snapshot width unsupported cv610",
		venc_api_field_supported_for_backend("cv610",
			"snapshot.width") == 0);
	CHECK("snapshot height unsupported cv610",
		venc_api_field_supported_for_backend("cv610",
			"snapshot.height") == 0);

	return failures;
}

/* CV610 reads video0.fps, outgoing.server/enabled and audio.mute only at
 * start, so the shared table's MUT_LIVE must be downgraded per backend.  The
 * observable contract: /api/v1/live/set rejects them on cv610 and keeps the
 * config untouched, while the same field stays live on star6e and a field
 * cv610 really does apply live still goes through. */
static int test_cv610_restart_only_mutability(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_fps = test_apply_fps;
	/* An fps live-set also re-derives the GOP in frames, so the timing
	 * group needs apply_gop as well (live_group_supported_for_cfg). */
	cb.apply_gop = test_apply_gop;
	cb.apply_bitrate = test_apply_bitrate;

	cfg.video0.fps = 100;
	CHECK("cv610 live fps handled",
		apply_query_http_path(&cfg, "cv610", &cb, "/api/v1/live/set",
			"video0.fps=60", &status, response,
			sizeof(response)) == 0);
	CHECK("cv610 live fps rejected", status == 400);
	CHECK("cv610 live fps cfg untouched", cfg.video0.fps == 100);
	CHECK("cv610 live fps not applied",
		g_api_cb_state.apply_fps_calls == 0);

	/* image.mirror is newly SUPPORTED on cv610 but still restart-required:
	 * the sensor is programmed once at bring-up, so there is no live apply
	 * callback behind it.  Advertising `supported` without this check is
	 * exactly how a dashboard ends up offering a control that 501s. */
	venc_config_defaults(&cfg);
	reset_api_cb_state();
	cfg.image.mirror = false;
	CHECK("cv610 live mirror handled",
		apply_query_http_path(&cfg, "cv610", &cb, "/api/v1/live/set",
			"image.mirror=true", &status, response,
			sizeof(response)) == 0);
	CHECK("cv610 live mirror rejected", status == 400);
	CHECK("cv610 live mirror cfg untouched", cfg.image.mirror == false);
	/* Same on star6e — this one is not a backend gate but the shared
	 * table's mutability, so both must reject. */
	venc_config_defaults(&cfg);
	CHECK("star6e live mirror handled",
		apply_query_http_path(&cfg, "star6e", &cb, "/api/v1/live/set",
			"image.mirror=true", &status, response,
			sizeof(response)) == 0);
	CHECK("star6e live mirror rejected", status == 400);

	/* outgoing.server / outgoing.enabled LEFT the restart-only list in
	 * 0.77.0.  Asserted as an ACCEPTED live set that reaches the callback,
	 * not merely as "not rejected": the failure this guards against is the
	 * ordering one, where the field is widened to `live` before a backend
	 * callback exists to honour it, and the write is then refused by
	 * live_group_supported_for_cfg() instead of the mutability gate.  Both
	 * paths would show a 4xx, so only the callback count separates them. */
	venc_config_defaults(&cfg);
	reset_api_cb_state();
	cb.apply_server = test_apply_server;
	cb.apply_output_enabled = test_apply_output_enabled;
	snprintf(cfg.outgoing.server, sizeof(cfg.outgoing.server),
		"udp://10.0.0.1:5600");
	CHECK("cv610 live server handled",
		apply_query_http_path(&cfg, "cv610", &cb, "/api/v1/live/set",
			"outgoing.server=udp://10.0.0.2:5600", &status, response,
			sizeof(response)) == 0);
	CHECK("cv610 live server accepted", status == 200);
	CHECK("cv610 live server reached the backend",
		g_api_cb_state.apply_server_calls == 1);
	CHECK("cv610 live server committed",
		strcmp(cfg.outgoing.server, "udp://10.0.0.2:5600") == 0);

	/* Control: strip the callback and the SAME request must fail, which is
	 * what proves the accept above came from the widening rather than from
	 * the field having been live all along. */
	venc_config_defaults(&cfg);
	reset_api_cb_state();
	cb.apply_server = NULL;
	snprintf(cfg.outgoing.server, sizeof(cfg.outgoing.server),
		"udp://10.0.0.1:5600");
	CHECK("cv610 live server w/o callback handled",
		apply_query_http_path(&cfg, "cv610", &cb, "/api/v1/live/set",
			"outgoing.server=udp://10.0.0.2:5600", &status, response,
			sizeof(response)) == 0);
	CHECK("cv610 live server w/o callback rejected", status != 200);
	cb.apply_server = test_apply_server;

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	cfg.outgoing.enabled = true;
	CHECK("cv610 live enabled handled",
		apply_query_http_path(&cfg, "cv610", &cb, "/api/v1/live/set",
			"outgoing.enabled=false", &status, response,
			sizeof(response)) == 0);
	CHECK("cv610 live enabled accepted", status == 200);
	CHECK("cv610 live enabled reached the backend",
		g_api_cb_state.apply_output_enabled_calls == 1);
	CHECK("cv610 live enabled committed", cfg.outgoing.enabled == false);

	/* A live apply that requests a respawn must SAY so.  Without this the
	 * caller is told the change took effect live while the craft is about
	 * to restart under it. */
	venc_config_defaults(&cfg);
	reset_api_cb_state();
	venc_api_clear_reinit();
	cb.apply_server = test_apply_server_needs_restart;
	snprintf(cfg.outgoing.server, sizeof(cfg.outgoing.server),
		"udp://10.0.0.1:5600");
	CHECK("cv610 restart-class server handled",
		apply_query_http_path(&cfg, "cv610", &cb, "/api/v1/live/set",
			"outgoing.server=frame-shm://venc_frame", &status,
			response, sizeof(response)) == 0);
	CHECK("cv610 restart-class server accepted", status == 200);
	CHECK("cv610 restart-class server committed",
		strcmp(cfg.outgoing.server, "frame-shm://venc_frame") == 0);
	CHECK("cv610 restart-class server reports reinit",
		strstr(response, "\"reinit_pending\":true") != NULL);
	/* Assert the response SHAPE too, not just the flag.  A single-field set
	 * must answer the single form ("field": ...), never the batch form
	 * ("applied": [...]).  Without this the suite is blind to the
	 * single_response / reinit_requested arguments being swapped at the
	 * call site -- which happened, and which both flag-only checks passed
	 * straight through because the two values correlate in these cases. */
	CHECK("cv610 restart-class server single-form response",
		strstr(response, "\"field\":") != NULL &&
		strstr(response, "\"applied\":") == NULL);

	/* Control: the same field with a plain live apply must NOT claim a
	 * respawn, or the flag would be noise on every write. */
	venc_config_defaults(&cfg);
	reset_api_cb_state();
	venc_api_clear_reinit();
	cb.apply_server = test_apply_server;
	snprintf(cfg.outgoing.server, sizeof(cfg.outgoing.server),
		"udp://10.0.0.1:5600");
	CHECK("cv610 live server handled 2",
		apply_query_http_path(&cfg, "cv610", &cb, "/api/v1/live/set",
			"outgoing.server=udp://10.0.0.3:5600", &status,
			response, sizeof(response)) == 0);
	CHECK("cv610 live server accepted 2", status == 200);
	CHECK("cv610 live server reports no reinit",
		strstr(response, "reinit_pending") == NULL);
	CHECK("cv610 live server single-form response",
		strstr(response, "\"field\":") != NULL &&
		strstr(response, "\"applied\":") == NULL);

	/* The BATCH path must report a requested respawn too.  This had no test,
	 * which is exactly how it shipped reporting nothing while the single
	 * path had just been fixed to report it. */
	venc_config_defaults(&cfg);
	reset_api_cb_state();
	venc_api_clear_reinit();
	cb.apply_server = test_apply_server_needs_restart;
	cb.apply_bitrate = test_apply_bitrate;
	snprintf(cfg.outgoing.server, sizeof(cfg.outgoing.server),
		"udp://10.0.0.1:5600");
	CHECK("cv610 batch restart-class handled",
		apply_query_http_path(&cfg, "cv610", &cb, "/api/v1/live/set",
			"outgoing.server=frame-shm://venc_frame&video0.bitrate=9000",
			&status, response, sizeof(response)) == 0);
	CHECK("cv610 batch restart-class accepted", status == 200);
	CHECK("cv610 batch is the batch form",
		strstr(response, "\"applied\":") != NULL);
	CHECK("cv610 batch reports reinit",
		strstr(response, "\"reinit_pending\":true") != NULL);

	/* Control: a batch with no reinit-requesting apply must stay silent, or
	 * the flag is noise on every multi-field write. */
	venc_config_defaults(&cfg);
	reset_api_cb_state();
	venc_api_clear_reinit();
	cb.apply_server = test_apply_server;
	snprintf(cfg.outgoing.server, sizeof(cfg.outgoing.server),
		"udp://10.0.0.1:5600");
	CHECK("cv610 batch live handled",
		apply_query_http_path(&cfg, "cv610", &cb, "/api/v1/live/set",
			"outgoing.server=udp://10.0.0.4:5600&video0.bitrate=9000",
			&status, response, sizeof(response)) == 0);
	CHECK("cv610 batch live accepted", status == 200);
	CHECK("cv610 batch live reports no reinit",
		strstr(response, "reinit_pending") == NULL);

	/* Control 1: the same field on star6e is still a live apply, so the
	 * rejection above is the backend gate and not a blanket fps block. */
	venc_config_defaults(&cfg);
	reset_api_cb_state();
	cfg.video0.fps = 100;
	CHECK("star6e live fps ok",
		apply_query_http_path(&cfg, "star6e", &cb, "/api/v1/live/set",
			"video0.fps=60", &status, response,
			sizeof(response)) == 0);
	CHECK("star6e live fps status 200", status == 200);
	CHECK("star6e live fps cfg", cfg.video0.fps == 60);

	/* Control 2: a field cv610 does apply live is unaffected — the
	 * downgrade is per field, not "cv610 refuses live sets". */
	venc_config_defaults(&cfg);
	reset_api_cb_state();
	CHECK("cv610 live bitrate ok",
		apply_query_http_path(&cfg, "cv610", &cb, "/api/v1/live/set",
			"video0.bitrate=4096", &status, response,
			sizeof(response)) == 0);
	CHECK("cv610 live bitrate status 200", status == 200);
	CHECK("cv610 live bitrate cfg", cfg.video0.bitrate == 4096);
	CHECK("cv610 live bitrate applied",
		g_api_cb_state.apply_bitrate_calls == 1);

	return failures;
}

static int test_qr_window_live_config_only(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	memset(&cb, 0, sizeof(cb));

	CHECK("qr window live apply ok",
		apply_query_http_path(&cfg, "star6e", &cb, "/api/v1/live/set",
			"qr.windowMs=12500", &status, response,
			sizeof(response)) == 0);
	CHECK("qr window live status 200", status == 200);
	CHECK("qr window live cfg", cfg.qr.window_ms == 12500);
	CHECK("qr window live no reinit",
		strstr(response, "\"reinit_pending\":true") == NULL);

	return failures;
}

static int test_multi_set_live_success(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_bitrate = test_apply_bitrate;
	cb.apply_verbose = test_apply_verbose;

	CHECK("multi set apply ok",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.bitrate=4096&system.verbose=true",
			&status, response, sizeof(response)) == 0);
	CHECK("multi set status 200", status == 200);
	CHECK("multi set bitrate cfg", cfg.video0.bitrate == 4096);
	CHECK("multi set verbose cfg", cfg.system.verbose == true);
	CHECK("multi set bitrate applied once", g_api_cb_state.apply_bitrate_calls == 1);
	CHECK("multi set verbose applied once", g_api_cb_state.apply_verbose_calls == 1);
	CHECK("multi set bitrate value", g_api_cb_state.last_bitrate == 4096);
	CHECK("multi set verbose value", g_api_cb_state.last_verbose == true);
	CHECK("multi set response array", strstr(response, "\"applied\"") != NULL);

	return failures;
}

/* detect.modelPath is now a live field: setting it fires apply_detect_reload
 * (the detector swaps its .img without a pipeline respawn) instead of the
 * MUT_RESTART reinit, so the video0 RTP stream is uninterrupted. */
static int test_detect_model_path_live_reload(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_detect_reload = test_apply_detect_reload;

	CHECK("detect modelPath+modelId live ok",
		apply_set_query_http(&cfg, "star6e", &cb,
			"detect.modelPath=/sd/person.img&detect.modelId=1",
			&status, response, sizeof(response)) == 0);
	CHECK("detect live status 200", status == 200);
	CHECK("detect live model_path cfg",
		strcmp(cfg.detect.model_path, "/sd/person.img") == 0);
	CHECK("detect live model_id cfg", cfg.detect.model_id == 1);
	/* One grouped reload for the whole detect batch, not one per field. */
	CHECK("detect live reload once",
		g_api_cb_state.apply_detect_reload_calls == 1);
	/* No reinit: the stream keeps running through a live swap. */
	CHECK("detect live no reinit", strstr(response,
		"\"reinit_pending\":true") == NULL);

	reset_api_cb_state();
	CHECK("maruko detect modelPath live ok",
		apply_set_query_http(&cfg, "maruko", &cb,
			"detect.modelPath=/sd/person-i6c.img",
			&status, response, sizeof(response)) == 0);
	CHECK("maruko detect live status 200", status == 200);
	CHECK("maruko detect live reload once",
		g_api_cb_state.apply_detect_reload_calls == 1);

	return failures;
}

/* Missing apply_detect_reload on a minimal backend → 501, the standard
 * "not supported" preflight for a live field with no callback. */
static int test_detect_model_path_no_callback(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));   /* apply_detect_reload = NULL */

	(void)apply_set_query_http(&cfg, "star6e", &cb,
		"detect.modelPath=/sd/x.img", &status, response, sizeof(response));
	CHECK("detect no-callback status 501", status == 501);

	return failures;
}

/* detect.netWidth stays MUT_RESTART (tap geometry needs the VPE port
 * recreated): a single set persists + requests reinit, never a live reload. */
static int test_detect_net_width_restart(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_detect_reload = test_apply_detect_reload;

	CHECK("detect netWidth set ok",
		apply_set_query_http(&cfg, "star6e", &cb, "detect.netWidth=416",
			&status, response, sizeof(response)) == 0);
	CHECK("detect netWidth status 200", status == 200);
	CHECK("detect netWidth cfg", cfg.detect.net_width == 416);
	CHECK("detect netWidth reinit pending",
		strstr(response, "\"reinit_pending\":true") != NULL);
	CHECK("detect netWidth no live reload",
		g_api_cb_state.apply_detect_reload_calls == 0);

	return failures;
}

/* Geometry / threshold validators reject out-of-range detect values. */
static int test_detect_field_validation(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_detect_reload = test_apply_detect_reload;

	/* netWidth not a multiple of 32 → 409 validation error. */
	(void)apply_set_query_http(&cfg, "star6e", &cb, "detect.netWidth=100",
		&status, response, sizeof(response));
	CHECK("detect netWidth ragged rejected", status == 409);

	/* confThresh out of [0,1) → 409. */
	(void)apply_set_query_http(&cfg, "star6e", &cb, "detect.confThresh=1.5",
		&status, response, sizeof(response));
	CHECK("detect confThresh out of range rejected", status == 409);

	return failures;
}

static int test_multi_set_awb_grouped_apply(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_awb_mode = test_apply_awb_mode;

	CHECK("multi awb apply ok",
		apply_set_query_http(&cfg, "star6e", &cb,
			"isp.awbMode=ct_manual&isp.awbCt=6000",
			&status, response, sizeof(response)) == 0);
	CHECK("multi awb status 200", status == 200);
	CHECK("multi awb mode cfg", strcmp(cfg.isp.awb_mode, "ct_manual") == 0);
	CHECK("multi awb ct cfg", cfg.isp.awb_ct == 6000);
	CHECK("multi awb grouped once", g_api_cb_state.apply_awb_mode_calls == 1);
	CHECK("multi awb mode value", g_api_cb_state.last_awb_mode == 1);
	CHECK("multi awb ct value", g_api_cb_state.last_awb_ct == 6000);
	CHECK("multi awb response alias", strstr(response, "isp.awbMode") != NULL);

	return failures;
}

static int test_awb_rate_live_apply(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_awb_mode = test_apply_awb_mode;
	cb.apply_awb_rate = test_apply_awb_rate;

	/* Rate alone applies live and must NOT re-drive the AWB mode. */
	CHECK("awbFps live ok",
		apply_set_query_http(&cfg, "star6e", &cb, "isp.awbFps=10",
			&status, response, sizeof(response)) == 0);
	CHECK("awbFps live status 200", status == 200);
	CHECK("awbFps live cfg", cfg.isp.awb_fps == 10);
	CHECK("awbFps live rate call", g_api_cb_state.apply_awb_rate_calls == 1);
	CHECK("awbFps live rate value", g_api_cb_state.last_awb_rate == 10);
	CHECK("awbFps live no mode call",
		g_api_cb_state.apply_awb_mode_calls == 0);
	CHECK("awbFps live not restart",
		strstr(response, "\"reinit_pending\":true") == NULL);

	/* Batched with the mode: one call each, rate before mode so the mode
	 * apply sees the committed rate when it decides AWB ownership. */
	reset_api_cb_state();
	CHECK("awbFps batch ok",
		apply_set_query_http(&cfg, "star6e", &cb,
			"isp.awbFps=0&isp.awbMode=auto",
			&status, response, sizeof(response)) == 0);
	CHECK("awbFps batch status 200", status == 200);
	CHECK("awbFps batch cfg", cfg.isp.awb_fps == 0);
	CHECK("awbFps batch rate call", g_api_cb_state.apply_awb_rate_calls == 1);
	CHECK("awbFps batch rate value", g_api_cb_state.last_awb_rate == 0);
	CHECK("awbFps batch mode call", g_api_cb_state.apply_awb_mode_calls == 1);

	/* Out of range is rejected: 1000/hz is integer ms, so a large rate
	 * would round the loop's sleep to zero and spin a core. */
	reset_api_cb_state();
	CHECK("awbFps range rejected",
		apply_set_query_http(&cfg, "star6e", &cb, "isp.awbFps=5000",
			&status, response, sizeof(response)) == 0);
	CHECK("awbFps range status 409", status == 409);
	CHECK("awbFps range no apply",
		g_api_cb_state.apply_awb_rate_calls == 0);

	/* A backend without the loop advertises the field unsupported, so the
	 * write is rejected up front rather than accepted and silently
	 * dropped.  The WebUI greys the control off the same signal. */
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_awb_mode = test_apply_awb_mode;
	CHECK("awbFps unsupported ok",
		apply_set_query_http(&cfg, "maruko", &cb, "isp.awbFps=10",
			&status, response, sizeof(response)) == 0);
	CHECK("awbFps unsupported status 501", status == 501);
	CHECK("awbFps unsupported message",
		strstr(response, "not supported on this backend") != NULL);

	return failures;
}

static int test_multi_set_video_timing_grouped_apply(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_fps = test_apply_fps;
	cb.apply_gop = test_apply_gop;

	CHECK("multi timing apply ok",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.fps=30&video0.gopSize=1.0",
			&status, response, sizeof(response)) == 0);
	CHECK("multi timing status 200", status == 200);
	CHECK("multi timing fps cfg", cfg.video0.fps == 30);
	CHECK("multi timing gop cfg", cfg.video0.gop_size == 1.0);
	CHECK("multi timing fps once", g_api_cb_state.apply_fps_calls == 1);
	CHECK("multi timing gop once", g_api_cb_state.apply_gop_calls == 1);
	CHECK("multi timing fps value", g_api_cb_state.last_fps == 30);
	CHECK("multi timing gop value", g_api_cb_state.last_gop == 30);
	CHECK("multi timing response alias", strstr(response, "video0.gopSize") != NULL);

	return failures;
}

static int test_multi_set_rejects_restart_fields(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];
	uint32_t old_bitrate;
	uint32_t old_width;
	uint32_t old_height;

	venc_config_defaults(&cfg);
	old_bitrate = cfg.video0.bitrate;
	old_width = cfg.video0.width;
	old_height = cfg.video0.height;
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_bitrate = test_apply_bitrate;

	CHECK("multi reject restart rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.bitrate=4096&video0.size=1280x720",
			&status, response, sizeof(response)) == 0);
	CHECK("multi reject restart status", status == 400);
	CHECK("multi reject restart error",
		strstr(response, "multi-set only supports live fields") != NULL);
	CHECK("multi reject restart bitrate unchanged", cfg.video0.bitrate == old_bitrate);
	CHECK("multi reject restart width unchanged", cfg.video0.width == old_width);
	CHECK("multi reject restart height unchanged", cfg.video0.height == old_height);
	CHECK("multi reject restart no callbacks", g_api_cb_state.apply_bitrate_calls == 0);

	return failures;
}

static int test_multi_set_rejects_duplicate_fields(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_qp_delta = test_apply_qp_delta;

	CHECK("multi dup rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.qp_delta=1&video0.qpDelta=2",
			&status, response, sizeof(response)) == 0);
	CHECK("multi dup status", status == 400);
	CHECK("multi dup error", strstr(response, "duplicate field") != NULL);
	CHECK("multi dup qp unchanged", cfg.video0.qp_delta == -12);
	CHECK("multi dup no apply", g_api_cb_state.apply_qp_delta_calls == 0);

	return failures;
}

static int test_multi_set_preflights_missing_callback(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_bitrate = test_apply_bitrate;

	CHECK("multi preflight rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.bitrate=4096&system.verbose=true",
			&status, response, sizeof(response)) == 0);
	CHECK("multi preflight status", status == 501);
	CHECK("multi preflight error",
		strstr(response, "apply callback not available") != NULL);
	CHECK("multi preflight bitrate unchanged", cfg.video0.bitrate == 8192);
	CHECK("multi preflight verbose unchanged", cfg.system.verbose == false);
	CHECK("multi preflight no side effects", g_api_cb_state.apply_bitrate_calls == 0);

	return failures;
}

static int test_multi_set_rolls_back_on_apply_failure(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_bitrate = test_apply_bitrate;
	cb.apply_verbose = test_apply_verbose;
	g_api_cb_state.fail_verbose = 1;

	CHECK("multi rollback rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.bitrate=4096&system.verbose=true",
			&status, response, sizeof(response)) == 0);
	CHECK("multi rollback status", status == 500);
	CHECK("multi rollback error",
		strstr(response, "failed to apply live field group") != NULL);
	CHECK("multi rollback bitrate restored", cfg.video0.bitrate == 8192);
	CHECK("multi rollback verbose restored", cfg.system.verbose == false);
	CHECK("multi rollback bitrate forward+rollback",
		g_api_cb_state.apply_bitrate_calls == 2);
	CHECK("multi rollback verbose attempted+rollback",
		g_api_cb_state.apply_verbose_calls == 2);
	CHECK("multi rollback bitrate restored value",
		g_api_cb_state.last_bitrate == 8192);
	CHECK("multi rollback verbose restored value",
		g_api_cb_state.last_verbose == false);

	return failures;
}

static int test_single_set_runtime_apply_failure(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];
	uint32_t old_fps;
	double old_gop_size;

	venc_config_defaults(&cfg);
	old_fps = cfg.video0.fps;
	old_gop_size = cfg.video0.gop_size;
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_fps = test_apply_fps;
	cb.apply_gop = test_apply_gop;
	g_api_cb_state.fail_gop = 1;

	CHECK("single failure rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.fps=30", &status, response,
			sizeof(response)) == 0);
	CHECK("single failure status", status == 500);
	CHECK("single failure error",
		strstr(response, "failed to apply live field group") != NULL);
	CHECK("single failure fps restored", cfg.video0.fps == old_fps);
	CHECK("single failure gop restored", cfg.video0.gop_size == old_gop_size);
	CHECK("single failure fps forward+rollback",
		g_api_cb_state.apply_fps_calls == 2);
	CHECK("single failure gop attempted+rollback",
		g_api_cb_state.apply_gop_calls == 2);
	CHECK("single failure fps restored value",
		g_api_cb_state.last_fps == old_fps);

	return failures;
}

static int test_live_set_rejects_out_of_range_roi_values(void)
{
	int failures = 0;
	VencConfig cfg;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);

	CHECK("roi center reject rc",
		apply_set_query_http(&cfg, "star6e", NULL,
			"fpv.roi_center=42", &status, response,
			sizeof(response)) == 0);
	CHECK("roi center reject status", status == 409);
	CHECK("roi center reject error",
		strstr(response, "roi_center must be in range [0.1, 0.9]") != NULL);
	CHECK("roi center unchanged", cfg.fpv.roi_center == 0.4);

	CHECK("roi steps reject rc",
		apply_set_query_http(&cfg, "star6e", NULL,
			"fpv.roi_steps=999", &status, response,
			sizeof(response)) == 0);
	CHECK("roi steps reject status", status == 409);
	CHECK("roi steps reject error",
		strstr(response, "roi_steps must be in range [1, 4]") != NULL);
	CHECK("roi steps unchanged", cfg.fpv.roi_steps == 2);

	return failures;
}

/* H.265-only: video0.codec was retired with the resilience-preset
 * consolidation.  Legacy clients setting `video0.codec=h264` must now
 * receive a clean 404 rather than silent acceptance. */
/* /api/v1/live/set: applies to the running config, never writes disk;
 * restart-class fields are rejected (a respawn reloads from disk). */
static int test_live_set_endpoint_volatile_no_disk_write(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];
	const char *path = "/tmp/test_venc_api_live_set.json";

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_bitrate = test_apply_bitrate;
	cb.apply_verbose = test_apply_verbose;

	unlink(path);
	venc_api_set_config_path(path);

	/* Single live field: applied, callback fired, nothing on disk. */
	CHECK("live/set apply rc",
		apply_query_http_path(&cfg, "star6e", &cb, "/api/v1/live/set",
			"video0.bitrate=9000", &status, response,
			sizeof(response)) == 0);
	CHECK("live/set status 200", status == 200);
	CHECK("live/set cfg updated", cfg.video0.bitrate == 9000);
	CHECK("live/set callback fired",
		g_api_cb_state.apply_bitrate_calls == 1);
	CHECK("live/set no disk write", access(path, F_OK) != 0);

	/* Multi-param stays volatile too. */
	CHECK("live/set multi rc",
		apply_query_http_path(&cfg, "star6e", &cb, "/api/v1/live/set",
			"video0.bitrate=9500&system.verbose=true", &status,
			response, sizeof(response)) == 0);
	CHECK("live/set multi status 200", status == 200);
	CHECK("live/set multi cfg updated", cfg.video0.bitrate == 9500);
	CHECK("live/set multi no disk write", access(path, F_OK) != 0);

	/* Restart-class field: rejected, not silently discarded. */
	CHECK("live/set restart-class rc",
		apply_query_http_path(&cfg, "star6e", &cb, "/api/v1/live/set",
			"image.mirror=true", &status, response,
			sizeof(response)) == 0);
	CHECK("live/set restart-class 400", status == 400);
	CHECK("live/set restart-class message",
		strstr(response, "requires persistence") != NULL);
	CHECK("live/set restart-class no disk write",
		access(path, F_OK) != 0);

	/* The persisting /set still writes — the whole running config,
	 * earlier volatile changes included. */
	CHECK("set apply rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.bitrate=9200", &status, response,
			sizeof(response)) == 0);
	CHECK("set status 200", status == 200);
	CHECK("set disk write", access(path, F_OK) == 0);

	unlink(path);
	venc_api_set_config_path(NULL);
	return failures;
}

static int test_restart_set_rejects_legacy_codec_field(void)
{
	int failures = 0;
	VencConfig cfg;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);

	CHECK("legacy codec rc",
		apply_set_query_http(&cfg, "star6e", NULL,
			"video0.codec=h264", &status, response,
			sizeof(response)) == 0);
	CHECK("legacy codec status", status == 404);
	CHECK("legacy codec error",
		strstr(response, "unknown config field") != NULL);

	/* The contract version the code serves must equal the one the contract
	 * document declares.  Reads the file rather than a second literal, so a
	 * doc-only bump (which is what shipped in 0.73.0) fails here. */
	{
		FILE *cf = fopen("documentation/HTTP_API_CONTRACT.md", "r");
		char line[256];
		int found = 0;

		CHECK("contract_doc_readable", cf != NULL);
		while (cf && fgets(line, sizeof(line), cf)) {
			if (strncmp(line, "- `contract_version`: `", 23) != 0)
				continue;
			found = 1;
			CHECK("contract_version_code_matches_doc",
				strncmp(line + 23, VENC_CONTRACT_VERSION,
					strlen(VENC_CONTRACT_VERSION)) == 0 &&
				line[23 + strlen(VENC_CONTRACT_VERSION)] == '`');
			break;
		}
		if (cf)
			fclose(cf);
		CHECK("contract_version_line_present", found);
	}
	{
		/* README carries a worked /api/v1/version example, and it went
		 * stale twice in one release cycle -- the pin above covers only
		 * HTTP_API_CONTRACT.md, which is exactly why nobody noticed.
		 * Pin both strings in it against the compiled values. */
		/* Both docs carry a worked /api/v1/version example.  The pin
		 * originally covered README alone, and the contract doc's copy
		 * promptly went stale by three releases -- which is the whole
		 * argument for pinning rather than eye-checking. */
		static const char *const version_docs[] = {
			"README.md",
			"documentation/HTTP_API_CONTRACT.md",
		};
		size_t di;
		FILE *rf;
		char line[512];
		int found = 0;
		char want_app[96], want_contract[96];
		char ver[64] = {0};
		FILE *vf = fopen("VERSION", "r");

		/* Read VERSION rather than VENC_VERSION: the host test build
		 * does not define it, and the file is the source of truth the
		 * README example is supposed to track anyway. */
		CHECK("version_file_readable", vf != NULL);
		if (vf) {
			if (fgets(ver, sizeof(ver), vf))
				ver[strcspn(ver, "\r\n")] = '\0';
			fclose(vf);
		}
		snprintf(want_app, sizeof(want_app), "\"app_version\":\"%s\"",
			ver);
		snprintf(want_contract, sizeof(want_contract),
			"\"contract_version\":\"%s\"", VENC_CONTRACT_VERSION);

		/* Quoted values only, so the match survives either formatting:
		 * README's compact JSON and the contract doc's pretty-printed
		 * copy put different whitespace after the colon. */
		snprintf(want_app, sizeof(want_app), "\"%s\"", ver);
		snprintf(want_contract, sizeof(want_contract), "\"%s\"",
			VENC_CONTRACT_VERSION);

		for (di = 0; di < sizeof(version_docs) / sizeof(version_docs[0]);
		     di++) {
			int seen_app = 0, seen_contract = 0;

			rf = fopen(version_docs[di], "r");
			CHECK("version_doc_readable", rf != NULL);
			while (rf && fgets(line, sizeof(line), rf)) {
				if (!seen_app && strstr(line, "\"app_version\"")) {
					seen_app = 1;
					CHECK("doc_app_version_matches_VERSION",
						strstr(line, want_app) != NULL);
				}
				if (!seen_contract &&
				    strstr(line, "\"contract_version\"")) {
					seen_contract = 1;
					CHECK("doc_contract_version_matches_code",
						strstr(line, want_contract) != NULL);
				}
				if (seen_app && seen_contract)
					break;
			}
			if (rf)
				fclose(rf);
			found = seen_app && seen_contract;
			CHECK("version_example_present_in_doc", found);
		}
	}

	/* 0.21.0 removed video0.maxIBytes/maxPBytes.  The contract promises a
	 * controller still pushing them gets 404 "unknown config field" — on
	 * BOTH set paths, and that a multi-field request naming one is rejected
	 * WHOLE rather than partially applied.  That promise is what forces the
	 * craft-side deploy order (controller before venc), so it is worth a
	 * test rather than an assertion in prose. */
	CHECK("removed cap rc",
		apply_set_query_http(&cfg, "star6e", NULL,
			"video0.maxIBytes=60000", &status, response,
			sizeof(response)) == 0);
	CHECK("removed cap status", status == 404);
	CHECK("removed cap error",
		strstr(response, "unknown config field") != NULL);

	CHECK("removed cap alias rc",
		apply_set_query_http(&cfg, "star6e", NULL,
			"video0.maxPBytes=12000", &status, response,
			sizeof(response)) == 0);
	CHECK("removed cap alias status", status == 404);

	/* Whole-request rejection: the surviving field must NOT be applied. */
	cfg.video0.bitrate = 8192;
	CHECK("removed cap batch rc",
		apply_set_query_http(&cfg, "star6e", NULL,
			"video0.bitrate=4096&video0.maxPBytes=12000",
			&status, response, sizeof(response)) == 0);
	CHECK("removed cap batch status", status == 404);
	CHECK("removed cap batch applied nothing", cfg.video0.bitrate == 8192);

	return failures;
}

static int test_single_set_url_decodes_outgoing_server(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_server = test_apply_server;

	/* encodeURIComponent("udp://192.168.1.5:5601") */
	CHECK("url-decode single rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"outgoing.server=udp%3A%2F%2F192.168.1.5%3A5601",
			&status, response, sizeof(response)) == 0);
	CHECK("url-decode single status", status == 200);
	CHECK("url-decode single cfg",
		strcmp(cfg.outgoing.server, "udp://192.168.1.5:5601") == 0);
	CHECK("url-decode single callback invoked",
		g_api_cb_state.apply_server_calls == 1);
	CHECK("url-decode single callback value",
		strcmp(g_api_cb_state.last_server,
			"udp://192.168.1.5:5601") == 0);

	return failures;
}

static int test_live_apply_sees_already_committed_config(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	/* apply_live_group_for_cfg() calls commit_config_locked() BEFORE it
	 * dispatches to the apply callbacks.  A backend guard that asks "did
	 * this value change?" by comparing the incoming argument against the
	 * committed config therefore compares the new value against itself and
	 * matches on EVERY call -- including a real change.
	 *
	 * That is not hypothetical: it shipped, and on Star6E it made
	 * outgoing.server live changes a silent no-op -- the socket stayed on
	 * the startup destination while /api/v1/config reported the new one.
	 * A backend guard must compare against its own applied runtime state.
	 *
	 * Pin the ordering so the trap is visible to the next person who
	 * writes such a guard. */
	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_server = test_apply_server;
	snprintf(cfg.outgoing.server, sizeof(cfg.outgoing.server), "%s",
		"udp://127.0.0.1:5600");
	g_api_cb_state.live_cfg = &cfg;

	CHECK("commit-order rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"outgoing.server=udp%3A%2F%2F127.0.0.1%3A5610",
			&status, response, sizeof(response)) == 0);
	CHECK("commit-order status", status == 200);
	CHECK("commit-order callback invoked",
		g_api_cb_state.apply_server_calls == 1);
	CHECK("commit-order callback saw the NEW value already committed",
		g_api_cb_state.server_committed_at_callback == 1);

	g_api_cb_state.live_cfg = NULL;
	return failures;
}

static int test_multi_set_url_decodes_values(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_server = test_apply_server;
	cb.apply_verbose = test_apply_verbose;

	CHECK("url-decode multi rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"outgoing.server=udp%3A%2F%2F10.0.0.1%3A5600"
			"&system.verbose=true",
			&status, response, sizeof(response)) == 0);
	CHECK("url-decode multi status", status == 200);
	CHECK("url-decode multi cfg",
		strcmp(cfg.outgoing.server, "udp://10.0.0.1:5600") == 0);
	CHECK("url-decode multi callback value",
		strcmp(g_api_cb_state.last_server,
			"udp://10.0.0.1:5600") == 0);

	return failures;
}

static int test_set_rejects_malformed_percent_escape(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_server = test_apply_server;

	/* "%ZZ" is not a valid percent-escape */
	CHECK("malformed %% multi rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"outgoing.server=udp%ZZ://1.2.3.4:5600"
			"&system.verbose=true",
			&status, response, sizeof(response)) == 0);
	CHECK("malformed %% multi status", status == 400);
	CHECK("malformed %% multi error",
		strstr(response, "malformed percent-escape") != NULL);
	CHECK("malformed %% no callback",
		g_api_cb_state.apply_server_calls == 0);

	return failures;
}

static int test_live_set_max_payload_size_bounds(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_max_payload_size = test_apply_max_payload;

	/* Below min: 575 rejects, [576, 4000] message. */
	CHECK("max_payload 575 reject rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"outgoing.maxPayloadSize=575", &status, response,
			sizeof(response)) == 0);
	CHECK("max_payload 575 reject status", status == 409);
	CHECK("max_payload 575 reject error",
		strstr(response, "max_payload_size must be in range [576, 4000]") != NULL);
	CHECK("max_payload 575 callback skipped",
		g_api_cb_state.apply_max_payload_calls == 0);

	/* Above max: 4001 rejects. */
	CHECK("max_payload 4001 reject rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"outgoing.maxPayloadSize=4001", &status, response,
			sizeof(response)) == 0);
	CHECK("max_payload 4001 reject status", status == 409);
	CHECK("max_payload 4001 reject error",
		strstr(response, "max_payload_size must be in range [576, 4000]") != NULL);
	CHECK("max_payload 4001 callback skipped",
		g_api_cb_state.apply_max_payload_calls == 0);

	/* Lower bound 576 accepts; callback fires; cfg updated. */
	CHECK("max_payload 576 accept rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"outgoing.maxPayloadSize=576", &status, response,
			sizeof(response)) == 0);
	CHECK("max_payload 576 accept status", status == 200);
	CHECK("max_payload 576 callback fired",
		g_api_cb_state.apply_max_payload_calls == 1);
	CHECK("max_payload 576 callback value",
		g_api_cb_state.last_max_payload == 576);
	CHECK("max_payload 576 cfg updated",
		cfg.outgoing.max_payload_size == 576);

	/* Upper bound 4000 accepts. */
	CHECK("max_payload 4000 accept rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"outgoing.maxPayloadSize=4000", &status, response,
			sizeof(response)) == 0);
	CHECK("max_payload 4000 accept status", status == 200);
	CHECK("max_payload 4000 callback fired",
		g_api_cb_state.apply_max_payload_calls == 2);
	CHECK("max_payload 4000 callback value",
		g_api_cb_state.last_max_payload == 4000);
	CHECK("max_payload 4000 cfg updated",
		cfg.outgoing.max_payload_size == 4000);

	return failures;
}

static int test_live_set_max_payload_size_no_callback(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	/* Backend without apply_max_payload_size callback rejects the live
	 * set during preflight rather than silently dropping the change. */
	venc_config_defaults(&cfg);
	memset(&cb, 0, sizeof(cb));

	CHECK("max_payload no-cb rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"outgoing.maxPayloadSize=2000", &status, response,
			sizeof(response)) == 0);
	CHECK("max_payload no-cb status", status == 501);
	CHECK("max_payload no-cb cfg unchanged",
		cfg.outgoing.max_payload_size == 1400);

	return failures;
}

static int test_live_zoom_pan_applies(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	cfg.video0.zoom_pct = 0.5;
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_zoom = test_apply_zoom;

	CHECK("zoom pan rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.zoomX=0.25&video0.zoomY=0.75",
			&status, response, sizeof(response)) == 0);
	CHECK("zoom pan status", status == 200);
	CHECK("zoom pan x cfg", cfg.video0.zoom_x == 0.25);
	CHECK("zoom pan y cfg", cfg.video0.zoom_y == 0.75);
	CHECK("zoom pan callback once", g_api_cb_state.apply_zoom_calls == 1);
	CHECK("zoom pan callback pct", g_api_cb_state.last_zoom_pct == 0.5);
	CHECK("zoom pan callback x", g_api_cb_state.last_zoom_x == 0.25);
	CHECK("zoom pan callback y", g_api_cb_state.last_zoom_y == 0.75);
	CHECK("zoom pan response alias", strstr(response, "video0.zoomX") != NULL);

	return failures;
}

static int test_zoom_validation_rejects_invalid(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	cfg.video0.zoom_pct = 0.5;
	cfg.video0.zoom_x = 0.5;
	cfg.video0.zoom_y = 0.5;
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_zoom = test_apply_zoom;

	CHECK("zoom x reject rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.zoomX=1.1", &status, response,
			sizeof(response)) == 0);
	CHECK("zoom x reject status", status == 409);
	CHECK("zoom x reject error",
		strstr(response, "zoom_x must be in range [0.0, 1.0]") != NULL);
	CHECK("zoom x unchanged", cfg.video0.zoom_x == 0.5);
	CHECK("zoom x no callback", g_api_cb_state.apply_zoom_calls == 0);

	CHECK("zoom y nan reject rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.zoomY=nan", &status, response,
			sizeof(response)) == 0);
	CHECK("zoom y nan reject status", status == 409);
	CHECK("zoom y nan reject error",
		strstr(response, "zoom_y must be in range [0.0, 1.0]") != NULL);
	CHECK("zoom y unchanged", cfg.video0.zoom_y == 0.5);
	CHECK("zoom y no callback", g_api_cb_state.apply_zoom_calls == 0);

	return failures;
}

static int test_framing_preset_restart(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_zoom = test_apply_zoom;

	/* A zoom framing preset is MUT_RESTART: derives zoom_pct, no live cb. */
	CHECK("framing restart rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.framing=zoom-2x", &status, response,
			sizeof(response)) == 0);
	CHECK("framing restart status", status == 200);
	CHECK("framing restart cfg", strcmp(cfg.video0.framing, "zoom-2x") == 0);
	CHECK("framing restart derives pct", cfg.video0.zoom_pct == 0.5);
	CHECK("framing restart response",
		strstr(response, "\"reinit_pending\":true") != NULL);
	CHECK("framing restart no live callback",
		g_api_cb_state.apply_zoom_calls == 0);
	venc_api_clear_reinit();

	/* stab-fill is a valid stab preset (MUT_RESTART); the preset derives a
	 * crop budget clamped to [60,100]. */
	CHECK("framing stab-fill rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.framing=stab-fill", &status, response,
			sizeof(response)) == 0);
	CHECK("framing stab-fill status", status == 200);
	CHECK("framing stab-fill cfg",
		strcmp(cfg.video0.framing, "stab-fill") == 0);
	CHECK("framing stab-fill crop budget",
		cfg.video0.stab_crop_pct >= 60 && cfg.video0.stab_crop_pct <= 100);
	venc_api_clear_reinit();

	/* Invalid framing preset is rejected. */
	CHECK("framing reject rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.framing=bogus", &status, response,
			sizeof(response)) == 0);
	CHECK("framing reject status", status == 409);
	CHECK("framing reject error",
		strstr(response, "framing must be one of") != NULL);
	CHECK("framing reject unchanged",
		strcmp(cfg.video0.framing, "stab-fill") == 0);

	/* stab_accuracy: a valid level applies via the camelCase alias on BOTH
	 * backends (no maruko gate); garbage is rejected 409 and leaves it unchanged. */
	CHECK("stab_accuracy medium rc (maruko)",
		apply_set_query_http(&cfg, "maruko", &cb,
			"video0.stabAccuracy=medium", &status, response,
			sizeof(response)) == 0);
	CHECK("stab_accuracy medium status", status == 200);
	CHECK("stab_accuracy medium cfg",
		strcmp(cfg.video0.stab_accuracy, "medium") == 0);
	venc_api_clear_reinit();

	CHECK("stab_accuracy reject rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.stab_accuracy=ludicrous", &status, response,
			sizeof(response)) == 0);
	CHECK("stab_accuracy reject status", status == 409);
	CHECK("stab_accuracy reject error",
		strstr(response, "stab_accuracy must be one of") != NULL);
	CHECK("stab_accuracy reject unchanged",
		strcmp(cfg.video0.stab_accuracy, "medium") == 0);

	/* A valid resilience preset applies; an unknown one must be REJECTED,
	 * not committed.  Committing it would persist the bad name to disk with
	 * the previous preset's derived ref_ and intra_ fields still in place,
	 * and the next start would silently fall back to "off". */
	CHECK("resilience ltr rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.resilience=ltr", &status, response,
			sizeof(response)) == 0);
	CHECK("resilience ltr status", status == 200);
	CHECK("resilience ltr expands",
		cfg.video0.ref_base == 1 && cfg.video0.ref_enhance == 1 &&
		cfg.video0.ref_pred == false);
	venc_api_clear_reinit();

	{
		/* Every malformed parameterised form the "ltr:<N>" syntax
		 * newly makes reachable. */
		const char *bad[] = { "ltr:0", "ltr:256", "ltr:abc", "ltr:",
				      "bogus" };
		size_t i;
		for (i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
			char q[64];
			snprintf(q, sizeof(q), "video0.resilience=%s", bad[i]);
			CHECK("resilience reject rc",
				apply_set_query_http(&cfg, "star6e", &cb, q,
					&status, response,
					sizeof(response)) == 0);
			CHECK("resilience reject status", status == 409);
			CHECK("resilience reject error",
				strstr(response, "resilience must be one of")
					!= NULL);
			CHECK("resilience reject unchanged",
				strcmp(cfg.video0.resilience, "ltr") == 0);
			CHECK("resilience reject derived intact",
				cfg.video0.ref_enhance == 1);
		}
	}

	return failures;
}

static int test_allow_unix_encoder_stall_restart(void)
{
	int failures = 0;
	VencConfig cfg;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	CHECK("unix stall default false",
		cfg.outgoing.allow_unix_encoder_stall == false);
	CHECK("unix stall restart rc",
		apply_set_query_http(&cfg, "maruko", NULL,
			"outgoing.allowUnixEncoderStall=true", &status, response,
			sizeof(response)) == 0);
	CHECK("unix stall restart status", status == 200);
	CHECK("unix stall restart cfg",
		cfg.outgoing.allow_unix_encoder_stall == true);
	CHECK("unix stall restart response",
		strstr(response, "\"reinit_pending\":true") != NULL);
	venc_api_clear_reinit();

	return failures;
}

static int test_live_set_isp_bin_dispatches_callback(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];
	/* /dev/null always exists and is readable — satisfies the path
	 * validator without bringing in mkstemp / cleanup. The mocked
	 * apply_isp_bin doesn't actually open the file. */
	const char *bin_path = "/dev/null";
	char query[160];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_isp_bin = test_apply_isp_bin;

	snprintf(query, sizeof(query), "isp.sensorBin=%s", bin_path);
	CHECK("isp_bin live rc",
		apply_set_query_http(&cfg, "star6e", &cb, query,
			&status, response, sizeof(response)) == 0);
	CHECK("isp_bin live status", status == 200);
	CHECK("isp_bin live cfg",
		strcmp(cfg.isp.sensor_bin, bin_path) == 0);
	CHECK("isp_bin live callback once",
		g_api_cb_state.apply_isp_bin_calls == 1);
	CHECK("isp_bin live callback path",
		strcmp(g_api_cb_state.last_isp_bin, bin_path) == 0);
	CHECK("isp_bin live no reinit pending",
		strstr(response, "\"reinit_pending\":true") == NULL);
	CHECK("isp_bin live did not request reinit",
		venc_api_get_reinit() == false);

	return failures;
}

static int test_live_set_isp_bin_rejects_unreadable_path(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_isp_bin = test_apply_isp_bin;

	CHECK("isp_bin bad path rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"isp.sensorBin=/no/such/bin",
			&status, response, sizeof(response)) == 0);
	CHECK("isp_bin bad path status 409", status == 409);
	CHECK("isp_bin bad path cfg unchanged", cfg.isp.sensor_bin[0] == '\0');
	CHECK("isp_bin bad path callback skipped",
		g_api_cb_state.apply_isp_bin_calls == 0);
	CHECK("isp_bin bad path error message",
		strstr(response, "not readable") != NULL);

	return failures;
}

static int test_live_set_isp_bin_no_callback_returns_501(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));  /* apply_isp_bin == NULL */

	CHECK("isp_bin no-cb rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"isp.sensorBin=/dev/null",
			&status, response, sizeof(response)) == 0);
	CHECK("isp_bin no-cb status 501", status == 501);
	CHECK("isp_bin no-cb cfg unchanged",
		cfg.isp.sensor_bin[0] == '\0');
	CHECK("isp_bin no-cb error code",
		strstr(response, "\"not_implemented\"") != NULL);

	return failures;
}

/* /api/v1/capabilities emits the data-driven `ui` block for fields that carry
 * UI metadata (video0.pause_stab), so the dashboard can render a control with
 * no static SECTIONS entry.  Core fields stay ui-less. */
/* A resilience preset OWNS intra_refresh_mode/lines, but must NOT own
 * intra_refresh_qp: that one is an explicit operator override where 0 already
 * means "use the preset's default".
 *
 * venc_config_apply_resilience_preset() zeroes the field, and the restart-set
 * path applies the preset to the staged config AFTER the request's own fields
 * are staged -- then persists it.  So a resilience change silently discarded
 * the override, returning 200 with no log.  On CV610 that field is the only
 * working I-frame lever, which is why it is tested on that backend (it is
 * also the only one that advertises it).
 *
 * Asserts the surviving VALUE, not that the code took some path, so it fails
 * if the preserve is removed.  The third case is the do-nothing control: the
 * fix must not invent a value where the operator set none. */
static int test_roi_qp_range_is_pm20(void)
{
	int failures = 0;
	VencConfig cfg;
	static const int accept[] = { 0, -20, 20, -19, 19 };
	static const int reject[] = { -21, 21, -30, 30, -51, 51 };
	size_t i;

	/* +-20, not +-30.  roiQp is a RELATIVE delta and H.265 caps QP at 51,
	 * so past 20 it stops being honoured at both ends -- and the negative
	 * end is expensive: CBR pays for the discount by raising base QP about
	 * 1:1, so once base + |roiQp| passes 51 the rate controller saturates.
	 * Measured on a CV610 bench: -30 pinned every frame at qp 51 and
	 * delivered 6x its bitrate target, -20 held it.
	 *
	 * Driven through venc_api_validate_loaded_config(), which is the gate
	 * that actually runs -- validate_field_cfg() is static, and asserting
	 * on a value assigned straight into the struct would test nothing. */
	for (i = 0; i < sizeof(accept) / sizeof(accept[0]); i++) {
		char name[64];

		venc_config_defaults(&cfg);
		cfg.fpv.roi_qp = accept[i];
		snprintf(name, sizeof(name), "roi_qp %+d accepted", accept[i]);
		CHECK(name, venc_api_validate_loaded_config(&cfg) == NULL);
	}
	for (i = 0; i < sizeof(reject) / sizeof(reject[0]); i++) {
		char name[64];
		const char *err;

		venc_config_defaults(&cfg);
		cfg.fpv.roi_qp = reject[i];
		err = venc_api_validate_loaded_config(&cfg);
		snprintf(name, sizeof(name), "roi_qp %+d rejected", reject[i]);
		CHECK(name, err != NULL);
		/* The message must name the field, not just fail: this validator
		 * sweeps many keys and a bare non-NULL would also pass if some
		 * unrelated default started failing. */
		snprintf(name, sizeof(name), "roi_qp %+d names the field",
			reject[i]);
		CHECK(name, err != NULL && strstr(err, "roi_qp") != NULL);
	}
	return failures;
}

static int test_frame_gate_loaded_config_bounds(void)
{
	int failures = 0;
	VencConfig cfg;

	/* The two frame-gate tunables were validated only on the live API
	 * path: validate_field_cfg() carries the 0-64 / 0-60000 rules, but
	 * they were missing from keys[] here, so a config FILE carrying
	 * frameGateMaxClosedMs skipped them and the uint32 *1000 in
	 * frame_gate_resolve() could wrap to an escape shorter than the
	 * debounce.  Drive the same gate the loader runs. */
	static const uint32_t close_ok[] = { 0, 1, 64 };
	static const uint32_t close_bad[] = { 65, 1000 };
	static const uint32_t max_ok[] = { 0, 1, 60000 };
	static const uint32_t max_bad[] = { 60001, 4294968u };
	size_t i;

	for (i = 0; i < sizeof(close_ok) / sizeof(close_ok[0]); i++) {
		venc_config_defaults(&cfg);
		cfg.video0.frame_gate_close_slots = close_ok[i];
		CHECK("frameGateCloseSlots loaded config accepted",
			venc_api_validate_loaded_config(&cfg) == NULL);
	}
	for (i = 0; i < sizeof(close_bad) / sizeof(close_bad[0]); i++) {
		venc_config_defaults(&cfg);
		cfg.video0.frame_gate_close_slots = close_bad[i];
		CHECK("frameGateCloseSlots loaded config rejected",
			venc_api_validate_loaded_config(&cfg) != NULL);
	}
	for (i = 0; i < sizeof(max_ok) / sizeof(max_ok[0]); i++) {
		venc_config_defaults(&cfg);
		cfg.video0.frame_gate_max_closed_ms = max_ok[i];
		CHECK("frameGateMaxClosedMs loaded config accepted",
			venc_api_validate_loaded_config(&cfg) == NULL);
	}
	for (i = 0; i < sizeof(max_bad) / sizeof(max_bad[0]); i++) {
		venc_config_defaults(&cfg);
		cfg.video0.frame_gate_max_closed_ms = max_bad[i];
		CHECK("frameGateMaxClosedMs loaded config rejected",
			venc_api_validate_loaded_config(&cfg) != NULL);
	}
	return failures;
}

static int test_resilience_preset_preserves_intra_refresh_qp(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	/* The real-world sequence: both fields are MUT_RESTART, and /api/v1/set
	 * refuses to carry two of those in one request, so an operator sets the
	 * override and then changes the preset.  Driven through the API on both
	 * steps so the whole staging path is exercised. */
	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));

	CHECK("ir_qp set rc",
		apply_set_query_http(&cfg, "cv610", &cb,
			"video0.intraRefreshQp=44", &status, response,
			sizeof(response)) == 0);
	CHECK("ir_qp set status", status == 200);
	CHECK("ir_qp set committed", cfg.video0.intra_refresh_qp == 44);
	venc_api_clear_reinit();

	CHECK("ir_qp preset change rc",
		apply_set_query_http(&cfg, "cv610", &cb,
			"video0.resilience=racing", &status, response,
			sizeof(response)) == 0);
	CHECK("ir_qp preset change status", status == 200);
	CHECK("ir_qp preset change took",
		strcmp(cfg.video0.resilience, "racing") == 0);
	CHECK("ir_qp survives preset change",
		cfg.video0.intra_refresh_qp == 44);
	venc_api_clear_reinit();

	/* The preserve must not make the field STICKY: an explicit 0 still
	 * means "use the preset's default", so an operator can undo their own
	 * override.  (A bare "no override set" case would be a tautology --
	 * defaults already leave it 0 and both the fixed and unfixed code
	 * leave it 0 through a preset change.) */
	CHECK("ir_qp explicit reset rc",
		apply_set_query_http(&cfg, "cv610", &cb,
			"video0.intraRefreshQp=0", &status, response,
			sizeof(response)) == 0);
	CHECK("ir_qp explicit reset status", status == 200);
	CHECK("ir_qp explicit reset clears the override",
		cfg.video0.intra_refresh_qp == 0);
	venc_api_clear_reinit();

	/* The range check added alongside: FT_UINT8 would otherwise take
	 * 0..255, persist it, and echo back a value the encoder never uses. */
	venc_config_defaults(&cfg);
	reset_api_cb_state();

	(void)apply_set_query_http(&cfg, "cv610", &cb,
		"video0.intraRefreshQp=200", &status, response,
		sizeof(response));
	CHECK("ir_qp out of range rejected", status == 409);
	CHECK("ir_qp out of range not committed",
		cfg.video0.intra_refresh_qp == 0);
	/* Correct today only because request_reinit() runs after validation, so
	 * a 409 stages nothing.  Clear it anyway: if the range check ever
	 * regresses, a leaked g_reinit turns one failure into a cascade through
	 * every test that follows. */
	venc_api_clear_reinit();

	return failures;
}

static int test_capabilities_emits_ui(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0, fd;
	char response[65536];
	char request[256];
	size_t sent = 0, req_len;

	venc_config_defaults(&cfg);
	memset(&cb, 0, sizeof(cb));

	if (venc_api_register(&cfg, "star6e", &cb, NULL) != 0) {
		CHECK("cap register", 0);
		return failures;
	}
	if (ensure_api_test_server() != 0) {
		CHECK("cap server", 0);
		return failures;
	}
	fd = connect_api_test_socket();
	CHECK("cap connect", fd >= 0);
	if (fd < 0)
		return failures;

	req_len = (size_t)snprintf(request, sizeof(request),
		"GET /api/v1/capabilities HTTP/1.0\r\n"
		"Host: 127.0.0.1\r\n"
		"\r\n");
	while (sent < req_len) {
		ssize_t n = write(fd, request + sent, req_len - sent);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			close(fd);
			CHECK("cap write", 0);
			return failures;
		}
		sent += (size_t)n;
	}
	shutdown(fd, SHUT_WR);
	CHECK("cap read",
		read_http_response(fd, &status, response, sizeof(response)) == 0);
	close(fd);

	CHECK("cap status", status == 200);
	CHECK("cap has pause_stab",
		strstr(response, "\"video0.pause_stab\"") != NULL);
	CHECK("cap pause_stab ui block", strstr(response, "\"ui\"") != NULL);
	CHECK("cap pause_stab group",
		strstr(response, "\"group\":\"Stabilization\"") != NULL);
	CHECK("cap pause_stab control",
		strstr(response, "\"control\":\"toggle\"") != NULL);
	/* A core field is still present (with mutability) and carries no ui. */
	CHECK("cap has core field",
		strstr(response, "\"video0.bitrate\"") != NULL);
	/* Persisted stab knobs now carry data-driven ui (Stabilization section)
	 * with a number control + range, so the whole group renders without a
	 * static SECTIONS row.  Verify against the stab_crop_pct entry. */
	{
		const char *p = strstr(response, "\"video0.stab_crop_pct\"");
		CHECK("cap has stab_crop_pct", p != NULL);
		if (p) {
			/* Confine the checks to this field's object (its ui block ends
			 * at the first '}' after p) so a match can't leak in from a
			 * neighbouring entry. */
			const char *end = strchr(p, '}');
			const char *g = strstr(p, "\"group\":\"Stabilization\"");
			const char *c = strstr(p, "\"control\":\"number\"");
			const char *m = strstr(p, "\"max\":100");
			CHECK("cap stab_crop_pct ui group",
				end && g && g < end);
			CHECK("cap stab_crop_pct number control",
				end && c && c < end);
			CHECK("cap stab_crop_pct range",
				end && m && m < end);
		}
	}
	/* The snapshot section is data-driven too — it has no static SECTIONS row,
	 * so without this ui block neither snapshot endpoint is reachable from the
	 * dashboard at all.  Confine the checks to the field's own object. */
	{
		const char *p = strstr(response, "\"snapshot.enabled\"");
		CHECK("cap has snapshot.enabled", p != NULL);
		if (p) {
			const char *end = strchr(p, '}');
			const char *g = strstr(p, "\"group\":\"Snapshot\"");
			CHECK("cap snapshot.enabled ui group", end && g && g < end);
		}
		p = strstr(response, "\"snapshot.quality\"");
		CHECK("cap has snapshot.quality", p != NULL);
		if (p) {
			const char *end = strchr(p, '}');
			const char *c = strstr(p, "\"control\":\"number\"");
			CHECK("cap snapshot.quality number control",
				end && c && c < end);
		}
	}
	return failures;
}

/* The dashboard greys a control off `supported:false` in /api/v1/capabilities,
 * so assert the JSON the browser actually consumes — not just the predicate
 * behind it.  isp.awb_fps paces a loop only Star6E has; on Maruko the entry
 * must still be present (schema is shared) but marked unsupported, with the
 * tooltip explaining why. */
/* Minimal stand-in: the routes test only cares that the pointer is non-NULL,
 * not what it returns. */
static char *api_test_stub_query_iq(void)
{
	return strdup("{\"ok\":true,\"data\":{}}");
}

static int api_test_stub_apply_iq(const char *param, const char *value)
{
	(void)param; (void)value;
	return 0;
}

/* /api/v1/capabilities advertises which optional routes the running backend
 * actually services, so the dashboard can decide whether to draw the Image
 * Quality tab without paying for a whole ISP sweep on /api/v1/iq.  The value
 * has to track the callback pointer, not the backend name -- the two existing
 * capabilities tests both zero the callbacks struct, so neither could ever
 * observe routes.iq true. */
static int test_capabilities_routes_track_callbacks(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0, fd;
	char response[65536];
	const char *request =
		"GET /api/v1/capabilities HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
	size_t sent, req_len = strlen(request);
	int pass;

	/* Three arms: neither callback, query_iq_info alone, then both.  Only
	 * the pointers change, so a difference in the response can only come
	 * from them.  The middle arm is the one that matters: routes.iq claims
	 * BOTH /api/v1/iq and /api/v1/iq/set are serviced, and with only
	 * query_iq_info registered the advertised /iq/set answers 501. */
	for (pass = 0; pass < 3; pass++) {
		venc_config_defaults(&cfg);
		memset(&cb, 0, sizeof(cb));
		if (pass >= 1)
			cb.query_iq_info = api_test_stub_query_iq;
		if (pass == 2)
			cb.apply_iq_param = api_test_stub_apply_iq;

		if (venc_api_register(&cfg, "star6e", &cb, NULL) != 0) {
			CHECK("routes register", 0);
			return failures;
		}
		if (ensure_api_test_server() != 0) {
			CHECK("routes server", 0);
			return failures;
		}
		fd = connect_api_test_socket();
		CHECK("routes connect", fd >= 0);
		if (fd < 0)
			return failures;
		for (sent = 0; sent < req_len; ) {
			ssize_t n = write(fd, request + sent, req_len - sent);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				close(fd);
				CHECK("routes write", 0);
				return failures;
			}
			sent += (size_t)n;
		}
		shutdown(fd, SHUT_WR);
		CHECK("routes read",
			read_http_response(fd, &status, response, sizeof(response)) == 0);
		close(fd);
		CHECK("routes status", status == 200);
		CHECK("routes block present", strstr(response, "\"routes\"") != NULL);
		if (pass == 0) {
			CHECK("routes.iq false without either callback",
				strstr(response, "\"iq\":false") != NULL);
		} else if (pass == 1) {
			CHECK("routes.iq false with query_iq_info alone",
				strstr(response, "\"iq\":false") != NULL);
		} else {
			CHECK("routes.iq true with both callbacks",
				strstr(response, "\"iq\":true") != NULL);
		}
	}
	return failures;
}

/* GET /api/v1/iq/export_bin serializes the live ISP to a PQTools .bin.
 *
 * The middle arm is the one that earns its keep.  venc_httpd routes by
 * first-match prefix and accepts '/' as a boundary, so "/api/v1/iq" would
 * swallow "/api/v1/iq/export_bin" if it were ever registered first -- and the
 * request would then answer 200 with the full ISP sweep from handle_iq
 * instead of the path.  That is a green-looking wrong answer no other test in
 * this file would notice, so assert on the BODY, not just the status. */
static int api_test_stub_export_bin_ok(const char *path)
{
	return path && *path ? 144774 : -1;
}

static int api_test_stub_export_bin_fail(const char *path)
{
	(void)path;
	return -1;
}

static int test_iq_export_bin_route(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0, fd;
	char response[65536];
	static const char *const req =
		"GET /api/v1/iq/export_bin HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
	size_t req_len = strlen(req);
	int pass;

	for (pass = 0; pass < 3; pass++) {
		size_t sent;

		venc_config_defaults(&cfg);
		memset(&cb, 0, sizeof(cb));
		if (pass == 1)
			cb.export_isp_bin = api_test_stub_export_bin_ok;
		else if (pass == 2)
			cb.export_isp_bin = api_test_stub_export_bin_fail;

		if (venc_api_register(&cfg, "cv610", &cb, NULL) != 0) {
			CHECK("export_bin register", 0);
			return failures;
		}
		if (ensure_api_test_server() != 0) {
			CHECK("export_bin server", 0);
			return failures;
		}
		fd = connect_api_test_socket();
		CHECK("export_bin connect", fd >= 0);
		if (fd < 0)
			return failures;
		for (sent = 0; sent < req_len; ) {
			ssize_t n = write(fd, req + sent, req_len - sent);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				close(fd);
				CHECK("export_bin write", 0);
				return failures;
			}
			sent += (size_t)n;
		}
		shutdown(fd, SHUT_WR);
		CHECK("export_bin read",
			read_http_response(fd, &status, response, sizeof(response)) == 0);
		close(fd);

		if (pass == 0) {
			CHECK("export_bin 501 without the callback", status == 501);
			CHECK("export_bin 501 names not_implemented",
				strstr(response, "not_implemented") != NULL);
		} else if (pass == 1) {
			CHECK("export_bin 200 with the callback", status == 200);
			/* The route-shadowing detector: handle_iq would answer 200 too. */
			CHECK("export_bin body carries the path",
				strstr(response, "\"path\":\"/tmp/isp_export.bin\"") != NULL);
			CHECK("export_bin body carries the byte count",
				strstr(response, "\"bytes\":144774") != NULL);
			CHECK("export_bin body is not the IQ sweep",
				strstr(response, "_schema") == NULL);
		} else {
			CHECK("export_bin 500 when the backend fails", status == 500);
			CHECK("export_bin 500 points at the log",
				strstr(response, "venc log") != NULL);
		}
	}
	return failures;
}

static int test_capabilities_awb_fps_backend_gate(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0, fd;
	char response[65536];
	static const char *const req =
		"GET /api/v1/capabilities HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
	size_t sent = 0, req_len = strlen(req);
	const char *p, *end;

	venc_config_defaults(&cfg);
	memset(&cb, 0, sizeof(cb));

	if (venc_api_register(&cfg, "maruko", &cb, NULL) != 0) {
		CHECK("cap maruko register", 0);
		return failures;
	}
	if (ensure_api_test_server() != 0) {
		CHECK("cap maruko server", 0);
		return failures;
	}
	fd = connect_api_test_socket();
	CHECK("cap maruko connect", fd >= 0);
	if (fd < 0)
		return failures;
	while (sent < req_len) {
		ssize_t n = write(fd, req + sent, req_len - sent);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			close(fd);
			CHECK("cap maruko write", 0);
			return failures;
		}
		sent += (size_t)n;
	}
	shutdown(fd, SHUT_WR);
	CHECK("cap maruko read",
		read_http_response(fd, &status, response, sizeof(response)) == 0);
	close(fd);
	CHECK("cap maruko status", status == 200);

	p = strstr(response, "\"isp.awb_fps\"");
	CHECK("cap maruko has awb_fps", p != NULL);
	if (p) {
		/* Confine to this field's entry so a neighbour can't satisfy it. */
		end = strstr(p, "\"isp.keep_aspect\"");
		CHECK("cap maruko awb_fps unsupported",
			strstr(p, "\"supported\":false") != NULL &&
			(!end || strstr(p, "\"supported\":false") < end));
		CHECK("cap maruko awb_fps still live",
			strstr(p, "\"mutability\":\"live\"") != NULL &&
			(!end || strstr(p, "\"mutability\":\"live\"") < end));
		CHECK("cap maruko awb_fps tooltip explains",
			strstr(p, "no rate to set") != NULL &&
			(!end || strstr(p, "no rate to set") < end));
	}

	/* Same build, Star6E backend: the control is live and adjustable. */
	if (venc_api_register(&cfg, "star6e", &cb, NULL) != 0) {
		CHECK("cap star6e re-register", 0);
		return failures;
	}
	fd = connect_api_test_socket();
	CHECK("cap star6e connect", fd >= 0);
	if (fd < 0)
		return failures;
	sent = 0;
	while (sent < req_len) {
		ssize_t n = write(fd, req + sent, req_len - sent);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			close(fd);
			CHECK("cap star6e write", 0);
			return failures;
		}
		sent += (size_t)n;
	}
	shutdown(fd, SHUT_WR);
	CHECK("cap star6e read",
		read_http_response(fd, &status, response, sizeof(response)) == 0);
	close(fd);
	p = strstr(response, "\"isp.awb_fps\"");
	CHECK("cap star6e has awb_fps", p != NULL);
	if (p) {
		end = strstr(p, "\"isp.keep_aspect\"");
		CHECK("cap star6e awb_fps supported",
			strstr(p, "\"supported\":true") != NULL &&
			(!end || strstr(p, "\"supported\":true") < end));
	}

	return failures;
}

/* The snapshot surface is the single MJPEG route.  The PGM endpoints were
 * retired in 0.60.0 (the per-request VPE tap could wedge the SoC — see
 * HISTORY.md), so they must 404 rather than linger half-alive.  No backend is
 * linked in the host runner, so the live route gets as far as the
 * disabled-subsystem 503. */
static int test_snapshot_routes(void)
{
	static const struct { const char *path; int want; } cases[] = {
		{ "snapshot.jpg",        503 },
		{ "snapshot.pgm",        404 },  /* retired 0.60.0 */
		{ "snapshot-center.pgm", 404 },  /* never shipped */
	};
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	size_t i;

	venc_config_defaults(&cfg);
	memset(&cb, 0, sizeof(cb));
	if (venc_api_register(&cfg, "star6e", &cb, NULL) != 0) {
		CHECK("snap-route register", 0);
		return failures;
	}
	if (ensure_api_test_server() != 0) {
		CHECK("snap-route server", 0);
		return failures;
	}

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		char request[256], response[65536];
		int status = 0, fd;
		size_t sent = 0, req_len;

		fd = connect_api_test_socket();
		CHECK("snap-route connect", fd >= 0);
		if (fd < 0)
			continue;
		req_len = (size_t)snprintf(request, sizeof(request),
			"GET /api/v1/%s HTTP/1.0\r\n"
			"Host: 127.0.0.1\r\n"
			"\r\n", cases[i].path);
		while (sent < req_len) {
			ssize_t n = write(fd, request + sent, req_len - sent);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			sent += (size_t)n;
		}
		shutdown(fd, SHUT_WR);
		CHECK("snap-route read",
			read_http_response(fd, &status, response, sizeof(response)) == 0);
		close(fd);
		if (status != cases[i].want)
			fprintf(stderr, "  snap-route '%s': got %d want %d\n",
				cases[i].path, status, cases[i].want);
		CHECK("snap-route status", status == cases[i].want);
	}
	return failures;
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int test_venc_api(void)
{
	int failures = 0;
	failures += test_register();
	failures += test_active_precrop_setter();
	failures += test_register_with_callbacks();
	failures += test_field_support_by_backend();
	failures += test_cv610_restart_only_mutability();
	failures += test_qr_window_live_config_only();
	failures += test_multi_set_live_success();
	failures += test_detect_model_path_live_reload();
	failures += test_detect_model_path_no_callback();
	failures += test_detect_net_width_restart();
	failures += test_detect_field_validation();
	failures += test_multi_set_awb_grouped_apply();
	failures += test_awb_rate_live_apply();
	failures += test_multi_set_video_timing_grouped_apply();
	failures += test_multi_set_rejects_restart_fields();
	failures += test_multi_set_rejects_duplicate_fields();
	failures += test_multi_set_preflights_missing_callback();
	failures += test_multi_set_rolls_back_on_apply_failure();
	failures += test_single_set_runtime_apply_failure();
	failures += test_live_set_rejects_out_of_range_roi_values();
	failures += test_live_set_max_payload_size_bounds();
	failures += test_live_set_max_payload_size_no_callback();
	failures += test_live_zoom_pan_applies();
	failures += test_zoom_validation_rejects_invalid();
	failures += test_framing_preset_restart();
	failures += test_allow_unix_encoder_stall_restart();
	failures += test_live_set_isp_bin_dispatches_callback();
	failures += test_live_set_isp_bin_rejects_unreadable_path();
	failures += test_live_set_isp_bin_no_callback_returns_501();
	failures += test_live_set_endpoint_volatile_no_disk_write();
	failures += test_restart_set_rejects_legacy_codec_field();
	failures += test_single_set_url_decodes_outgoing_server();
	failures += test_live_apply_sees_already_committed_config();
	failures += test_multi_set_url_decodes_values();
	failures += test_set_rejects_malformed_percent_escape();
	failures += test_roi_qp_range_is_pm20();
	failures += test_frame_gate_loaded_config_bounds();
	failures += test_resilience_preset_preserves_intra_refresh_qp();
	failures += test_capabilities_emits_ui();
	failures += test_capabilities_awb_fps_backend_gate();
	failures += test_capabilities_routes_track_callbacks();
	failures += test_iq_export_bin_route();
	failures += test_snapshot_routes();
	stop_api_test_server();
	return failures;
}
