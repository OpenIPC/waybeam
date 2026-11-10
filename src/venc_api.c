#include "venc_api.h"
#include "attitude_est.h"
#include "device_id.h"
#include "framing_stab_accuracy.h"
#include "idr_rate_limit.h"
#include "intra_refresh.h"
#include "pipeline_common.h"
#if HAVE_BACKEND_CV610
#include "cv610_runtime.h"
#include "cv610_modes.h"
#include "cv610_validation.h"
#endif
#if HAVE_BACKEND_STAR6E
#include "star6e_pipeline.h"
#include "star6e_luma_tap.h"
#endif
#if HAVE_BACKEND_MARUKO
#include "maruko_pipeline.h"
#endif
#include "rtp_packetizer.h"
#if !HAVE_BACKEND_CV610
#include "sensor_select.h"
#endif
/* The recorder state machine, its RECORDER_DEFAULT_DIR and the TS mux are
 * SoC-independent; only the SDK-typed adapters inside are compiled out per
 * backend.  CV610 used to carry its own copy of the default-dir constant
 * because it did not link the recorder — two definitions that had to agree
 * by hand. */
#include "star6e_recorder.h"
#include "venc_httpd.h"
#include "venc_jpeg.h"
#include "venc_webui.h"
#include "cJSON.h"

#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

/* Guard the live max_payload_size ceiling against future tightenings:
 * - RTP_BUFFER_MAX is the hard cap inside the packetizer (silently
 *   truncates above it).
 * - SHM ring slot_data_size is uint32 in the header but published as
 *   uint16-fitting (slot_data + 12 must fit into the 65535 cap that
 *   venc_ring_create rejects). */
_Static_assert(VENC_OUTPUT_PAYLOAD_CEILING_BYTES + 12 <= RTP_BUFFER_MAX,
	"VENC_OUTPUT_PAYLOAD_CEILING_BYTES exceeds RTP_BUFFER_MAX cap");
_Static_assert(VENC_OUTPUT_PAYLOAD_CEILING_BYTES + 12 <= 65535,
	"VENC_OUTPUT_PAYLOAD_CEILING_BYTES would overflow SHM slot_data_size");

/* ── Shared state (set by venc_api_register) ─────────────────────────── */

static VencConfig *g_cfg;
static const VencApplyCallbacks *g_cb;
static char g_backend[32];
static char g_config_path[256];
static int g_api_routes_registered = 0;

/* Mutex protecting g_cfg field access from the httpd thread.
 * All handle_set/handle_get calls run on the httpd pthread; the main
 * streaming thread reads config fields concurrently.  This mutex
 * serializes field reads/writes to prevent torn values on ARM.  It also
 * guards g_config_path and the g_last_saved cache below (the httpd is
 * single-threaded so no handler-vs-handler race, but the main thread
 * calls venc_api_set_config_path at startup).
 *
 * Hold-time policy: keep this mutex hot — backends register their own
 * VencConfig pointer as g_cfg (e.g. &ctx->vcfg), and apply_*
 * callbacks may read additional vcfg fields beyond the value they were
 * passed.  So
 * apply_live_group_for_cfg() commits the staged value to g_cfg before
 * each callback, and the mutex must remain held across the whole
 * apply sequence to keep that commit + read pair coherent.  The
 * pre-apply validation/preflight phase is run outside the mutex
 * because it only reads write-once globals and a local cfg copy. */
static pthread_mutex_t g_cfg_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Last config snapshot that was successfully persisted, used to skip
 * redundant flash writes when /api/v1/set changes nothing (e.g. a
 * slider that lands back on its current value, or an adaptive-bitrate
 * loop re-asserting the same kbps).  memcmp is safe because both the
 * saved copy and the candidate are produced by byte-wise struct copies
 * (`*g_cfg = new_cfg` and `actual_cfg = *g_cfg`), so any padding bytes
 * are bit-identical. */
static VencConfig g_last_saved;
static int g_last_saved_valid = 0;

/* Pipeline runtime state exposed via /api/v1/config and /api/v1/ae.
 * Backends call venc_api_set_active_precrop() after programming VIF; the
 * store stays "valid=0" until the first successful pipeline start.  Reads
 * are atomic under g_precrop_mutex (HTTP thread vs pipeline thread). */
static pthread_mutex_t g_precrop_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct {
	uint16_t x, y, w, h;
	int valid;
} g_precrop = {0, 0, 0, 0, 0};

void venc_api_set_active_precrop(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	pthread_mutex_lock(&g_precrop_mutex);
	g_precrop.x = x;
	g_precrop.y = y;
	g_precrop.w = w;
	g_precrop.h = h;
	g_precrop.valid = 1;
	pthread_mutex_unlock(&g_precrop_mutex);
}

void venc_api_clear_active_precrop(void)
{
	pthread_mutex_lock(&g_precrop_mutex);
	g_precrop.valid = 0;
	pthread_mutex_unlock(&g_precrop_mutex);
}

int venc_api_get_active_precrop(uint16_t *x, uint16_t *y,
	uint16_t *w, uint16_t *h)
{
	int valid;
	pthread_mutex_lock(&g_precrop_mutex);
	valid = g_precrop.valid;
	if (valid && x && y && w && h) {
		*x = g_precrop.x;
		*y = g_precrop.y;
		*w = g_precrop.w;
		*h = g_precrop.h;
	}
	pthread_mutex_unlock(&g_precrop_mutex);
	return valid;
}

/* VPE tap map published by the Star6E port arbiter (star6e_vpe_ports.c) and
 * emitted in /api/v1/config runtime.vpe_taps.  Guarded by its own mutex: the
 * pipeline thread writes it, the httpd thread reads it in handle_config. */
static pthread_mutex_t g_vpe_taps_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_vpe_taps[192];
static int g_vpe_taps_valid;

void venc_api_set_vpe_taps(const char *json_obj)
{
	pthread_mutex_lock(&g_vpe_taps_mutex);
	if (json_obj && json_obj[0]) {
		snprintf(g_vpe_taps, sizeof(g_vpe_taps), "%s", json_obj);
		g_vpe_taps_valid = 1;
	} else {
		g_vpe_taps_valid = 0;
	}
	pthread_mutex_unlock(&g_vpe_taps_mutex);
}

int venc_api_get_vpe_taps(char *buf, size_t buf_size)
{
	int valid;

	pthread_mutex_lock(&g_vpe_taps_mutex);
	valid = g_vpe_taps_valid;
	if (valid && buf && buf_size > 0)
		snprintf(buf, buf_size, "%s", g_vpe_taps);
	pthread_mutex_unlock(&g_vpe_taps_mutex);
	return valid;
}

void venc_api_set_config_path(const char *path)
{
	pthread_mutex_lock(&g_cfg_mutex);
	if (path)
		snprintf(g_config_path, sizeof(g_config_path), "%s", path);
	else
		g_config_path[0] = '\0';
	/* Path change invalidates the last-saved cache so the first save to
	 * the new path is unconditional. */
	g_last_saved_valid = 0;
	pthread_mutex_unlock(&g_cfg_mutex);
}

/* Persist current config to disk if a config path was registered and the
 * snapshot differs from the last-saved copy.  Caller must NOT hold
 * g_cfg_mutex (this function takes it).
 * Returns:
 *   0  — saved successfully, or skipped because content is unchanged
 *  -1  — save failed (disk full, readonly FS, permission, fsync error).
 *        In-memory state was already committed before this call; callers
 *        should surface the failure so operators know the runtime and
 *        on-disk config have diverged. */
static int venc_api_save_config_to_disk(const VencConfig *cfg_snapshot)
{
	char path[sizeof(g_config_path)];
	int is_same = 0;
	int rc;

	pthread_mutex_lock(&g_cfg_mutex);
	snprintf(path, sizeof(path), "%s", g_config_path);
	if (g_last_saved_valid &&
	    memcmp(&g_last_saved, cfg_snapshot, sizeof(*cfg_snapshot)) == 0)
		is_same = 1;
	pthread_mutex_unlock(&g_cfg_mutex);
	if (!path[0])
		return 0;  /* no path registered — silently no-op */
	if (is_same)
		return 0;  /* identical to last save — skip flash write */

	rc = venc_config_save(path, cfg_snapshot);
	if (rc == 0) {
		pthread_mutex_lock(&g_cfg_mutex);
		g_last_saved = *cfg_snapshot;
		g_last_saved_valid = 1;
		pthread_mutex_unlock(&g_cfg_mutex);
	} else {
		fprintf(stderr, "[venc_api] WARNING: config save to %s failed — "
			"in-memory change committed but on-disk copy is stale\n",
			path);
	}
	return rc;
}

/* ── Sensor info (set by backend after sensor_select) ─────────────────── */

static int g_sensor_pad = -1;
static int g_sensor_mode = -1;
static int g_sensor_forced_pad = -1;

void venc_api_set_sensor_info(int pad, int mode_index, int forced_pad)
{
	pthread_mutex_lock(&g_cfg_mutex);
	g_sensor_pad = pad;
	g_sensor_mode = mode_index;
	g_sensor_forced_pad = forced_pad;
	pthread_mutex_unlock(&g_cfg_mutex);
}

/* ── Reinit flag (shared with backend via accessors) ─────────────────── */

static volatile sig_atomic_t g_reinit = 0;
/* Monotonic count of reinit requests.  Only the single httpd dispatch thread
 * writes it, and only via venc_api_request_reinit(). */
static volatile sig_atomic_t g_reinit_seq = 0;

/* ── Record control flags ────────────────────────────────────────────── */

static volatile sig_atomic_t g_record_start_pending = 0;
static volatile sig_atomic_t g_record_stop_pending = 0;
static char g_record_start_dir[256];
static pthread_mutex_t g_record_mutex = PTHREAD_MUTEX_INITIALIZER;
static VencRecordStatusFn g_record_status_fn;
/* Separate from g_record_status_fn: a backend may expose live status
 * (so /api/v1/record/status reflects daemon-config-driven recording)
 * without consuming the HTTP-driven start/stop request flags.  Backends
 * that *do* consume those flags (currently Star6E only) call
 * venc_api_set_record_http_control_supported(1) so /api/v1/record/start
 * and /stop stop returning 501. */
static bool g_record_http_control_supported;

void venc_api_request_reinit(void)
{
	g_reinit = 1;
	/* Bumped alongside the latch because the latch alone cannot answer "did
	 * THIS apply ask for a respawn".  A caller that samples the boolean sees
	 * no transition when a reinit is already pending from an earlier request,
	 * which is exactly the case the persistence guard below exists for. */
	g_reinit_seq++;
}

unsigned venc_api_reinit_seq(void)
{
	return (unsigned)g_reinit_seq;
}

bool venc_api_get_reinit(void)
{
	return g_reinit != 0;
}

void venc_api_clear_reinit(void)
{
	g_reinit = 0;
}

void venc_api_request_record_start(const char *dir)
{
	pthread_mutex_lock(&g_record_mutex);
	snprintf(g_record_start_dir, sizeof(g_record_start_dir), "%s",
		dir ? dir : RECORDER_DEFAULT_DIR);
	g_record_start_pending = 1;
	g_record_stop_pending = 0;
	pthread_mutex_unlock(&g_record_mutex);
}

void venc_api_request_record_stop(void)
{
	pthread_mutex_lock(&g_record_mutex);
	g_record_stop_pending = 1;
	g_record_start_pending = 0;
	pthread_mutex_unlock(&g_record_mutex);
}

int venc_api_get_record_start(char *buf, size_t buf_size)
{
	int pending;

	pthread_mutex_lock(&g_record_mutex);
	pending = g_record_start_pending;
	if (pending && buf && buf_size > 0)
		snprintf(buf, buf_size, "%s", g_record_start_dir);
	g_record_start_pending = 0;
	pthread_mutex_unlock(&g_record_mutex);
	return pending;
}

int venc_api_get_record_stop(void)
{
	int pending;

	pthread_mutex_lock(&g_record_mutex);
	pending = g_record_stop_pending;
	g_record_stop_pending = 0;
	pthread_mutex_unlock(&g_record_mutex);
	return pending;
}

void venc_api_set_record_status_fn(VencRecordStatusFn fn)
{
	g_record_status_fn = fn;
}

void venc_api_set_record_http_control_supported(bool supported)
{
	g_record_http_control_supported = supported;
}

void venc_api_get_record_dir(char *buf, size_t buf_size)
{
	if (!buf || buf_size == 0)
		return;
	pthread_mutex_lock(&g_cfg_mutex);
	const char *src = (g_cfg && g_cfg->record.dir[0]) ?
		g_cfg->record.dir : RECORDER_DEFAULT_DIR;
	snprintf(buf, buf_size, "%s", src);
	pthread_mutex_unlock(&g_cfg_mutex);
}

void venc_api_fill_record_status(VencRecordStatus *out)
{
	if (!out) return;
	memset(out, 0, sizeof(*out));
	if (g_record_status_fn)
		g_record_status_fn(out);
}

/* ── Field descriptor table ──────────────────────────────────────────── */

typedef enum { MUT_LIVE, MUT_RESTART } Mutability;
typedef enum { FT_BOOL, FT_INT, FT_UINT, FT_UINT8, FT_UINT16, FT_DOUBLE, FT_FLOAT, FT_STRING, FT_SIZE } FieldType;

/* Optional UI metadata for a field.  When present (FieldDesc.ui != NULL) it is
 * emitted in /api/v1/capabilities so the dashboard can render a control for the
 * field WITHOUT a hardcoded SECTIONS entry — i.e. a module field becomes
 * WebUI-visible with no dashboard.html edit / webui-blob rebuild.  Core fields
 * keep ui = NULL and use the dashboard's static SECTIONS path. */
typedef struct {
	const char *group;          /* collapsible section title */
	const char *label;          /* human label; NULL = derive from key tail */
	const char *control;        /* "toggle" | "number" | "select" | "text" */
	double min, max, step;      /* "number" range (0/0/0 = unset) */
	const char *const *options; /* NULL-terminated list for "select", else NULL */
	const char *tooltip;
} FieldUi;

typedef struct {
	const char *key;          /* dot-separated JSON path, e.g. "video0.bitrate" */
	FieldType type;
	Mutability mut;
	size_t offset;            /* offsetof into VencConfig */
	size_t size;              /* sizeof the field (for strings) */
	const FieldUi *ui;        /* optional data-driven UI metadata (NULL = core) */
} FieldDesc;

#define FIELD(section, member, ft, m) \
	{ #section "." #member, ft, m, \
	  offsetof(VencConfig, section.member), \
	  sizeof(((VencConfig*)0)->section.member), NULL }

/* Like FIELD but carries data-driven UI metadata (see FieldUi). */
#define FIELD_UI(section, member, ft, m, uiptr) \
	{ #section "." #member, ft, m, \
	  offsetof(VencConfig, section.member), \
	  sizeof(((VencConfig*)0)->section.member), (uiptr) }

/* UI descriptors for the Stabilization section.  These carry data-driven UI
 * metadata so the dashboard renders the whole group from /api/v1/capabilities
 * with no static SECTIONS rows — adding/retuning a stab knob needs no
 * dashboard.html edit / webui-blob rebuild.  Ranges mirror the validators in
 * validate_field(); 0 = "use preset default" where noted.  group = the
 * collapsible section title the renderer buckets them under. */
static const FieldUi ui_awb_fps = {
	"ISP", "AWB rate (Hz)", "number", 0, 30, 1, NULL,
	/* Reads on BOTH backends — this text is served to Maruko too, where the
	 * control is greyed out, so it must not assert the i6e-only behaviour as
	 * if it were universal. */
	"Rate of the Star6E userspace auto-white-balance loop. The i6e "
	"ISP-internal AWB does not converge, so on that SoC AWB is driven from "
	"userspace instead. "
	"Deliberately decoupled from frame rate — the same cost at 120fps as at "
	"60fps. Default 15. 0 stops the loop and hands AWB back to the ISP, "
	"which leaves it wherever it last was. "
	"Ignored while awbMode=ct_manual. Applied live. "
	"Not on Maruko/I6C, whose AWB is driven by the SDK's own 3A through the "
	"CUS3A RunOnce pacer (which i6e does not export) — there is no rate to "
	"set, so the control is disabled."
};
static const FieldUi ui_stab_crop_pct = {
	"Stabilization", "Stab crop %", "number", 60, 100, 1, NULL,
	"Kept-frame percentage for framing=stab / stab-fill. 0 = preset default "
	"(80, API only); 60..100 = explicit crop %. Smaller = bigger dead border "
	"= more room to absorb motion but more zoomed-in. Requires restart."
};
static const FieldUi ui_stab_recenter_speed = {
	"Stabilization", "Recenter speed", "number", 0, 3600, 5, NULL,
	"How fast the stabilized window glides back to centre after motion "
	"(decay time-constant in frames). 0 = stick (never recenters); higher = "
	"slower, gentler return. Production default 180 (~3s @60fps). Requires restart."
};
static const FieldUi ui_stab_kalman_q = {
	"Stabilization", "Pan response (Q)", "number", 0.001, 1.0, 0.005, NULL,
	"Kalman process noise — how fast the view follows slow pans. Higher = "
	"tracks pans sooner / weaker hold; lower = holds tighter, more locked. "
	"Shared by stab + stab-fill. Default 0.03. Requires restart."
};
static const FieldUi ui_stab_kalman_r = {
	"Stabilization", "Smoothness (R)", "number", 0.1, 50.0, 0.1, NULL,
	"Kalman measurement noise — output smoothness. Higher = smoother but "
	"laggier; lower = snappier, more jitter passes through. Shared by stab + "
	"stab-fill. Default 2.0. Requires restart."
};
static const char *const stab_accuracy_opts[] = {
	"auto", "high", "medium", "low", NULL
};
static const FieldUi ui_stab_accuracy = {
	"Stabilization", "Detector accuracy", "select", 0, 0, 0, stab_accuracy_opts,
	"Motion-detector geometry / CPU cost (the NEON detector is the dominant "
	"per-frame stab cost). high=384/256/3 smoothest; medium=320/192/3; "
	"low=256/128/2 cheapest. auto = per-backend default (high on Star6E, low on "
	"single-core Maruko). Higher costs more CPU — pick to fit your resolution / "
	"fps. Shared by stab + stab-fill. Requires restart."
};

/* UI descriptor for video0.pause_stab — the live stab pause.  Rendered as a
 * toggle in the "Stabilization" group purely from capabilities (the field is
 * runtime-only / not in /api/v1/config, so it has no static SECTIONS row). */
static const FieldUi ui_pause_stab = {
	"Stabilization", "Pause stab", "toggle", 0, 0, 0, NULL,
	"Live pause for framing=stab and stab-fill: glide the stabilized window / "
	"floating image back to centre (software ramp, no rebind). No effect under "
	"framing=off or zoom."
};

/* UI descriptors for the RC QP bounds.  Rendered purely from capabilities —
 * these were API-only (no static SECTIONS rows). */
static const FieldUi ui_frame_gate_close_slots = {
	"Video", "Frame gate close slots", "number", 0, 64, 1, NULL,
	"Egress ring occupancy, in slots, at which the gate closes. 0 = "
	"default 3. The ring's healthy idle occupancy is one frame, so 2 is "
	"standing backlog and 3 leaves one slot of burst tolerance. Reopen is "
	"pinned at <= 1. Requires restart."
};
static const FieldUi ui_frame_gate_max_closed_ms = {
	"Video", "Frame gate max closed ms", "number", 0, 60000, 10, NULL,
	"Safety escape: reopen unconditionally after this long closed, so a "
	"dead consumer cannot stop the stream for good — the ring then "
	"overflows and reports full_drops, which is diagnosable. 0 = default "
	"500. Requires restart."
};
static const FieldUi ui_min_qp = {
	"Video", "Min QP", "number", 0, 51, 1, NULL,
	"RC QP floor. 0 = leave the SDK default. Raising the floor caps quality and saves bitrate; LOWERING it lets CBR actually spend its budget on a simple scene instead of undershooting the target. Applied live."
};
static const FieldUi ui_max_qp = {
	"Video", "Max QP", "number", 0, 51, 1, NULL,
	"RC QP ceiling. 0 = leave the SDK default. Raising the ceiling lets the encoder compress a scene change hard enough to stay inside the frame budget instead of emitting a burst frame. Applied live."
};
static const FieldUi ui_intra_refresh_qp = {
	"Video", "Intra-refresh QP", "number", 0, 51, 1, NULL,
	"QP of the GDR intra-refresh stripe — the quality of the recovery "
	"anchor, and the bitrate it costs. 0 = the resilience preset's default "
	"(fast 36 / balanced 32 / robust 28; robust runs lowest because lossy "
	"links want the cleanest anchor). Lower = cleaner anchor, more bits. "
	"On CV610 this is also the ONLY control that moves I-frame size, so it "
	"trades forced-IDR size against anchor quality — they are one register. "
	"Setting it below the scene's natural P QP collapses the I-frame instead "
	"of growing it. Restart-only; needs resilience != off."
};

static const FieldUi ui_slice_count = {
	"Video", "Slices per frame", "number", 1, VENC_SLICE_COUNT_MAX, 1, NULL,
	"Independent H.265 slices per picture. 1 = off. Multi-slice "
	"output lets the waybeam-link receiver conceal RF loss spatially "
	"instead of dropping the whole frame, and the concealed area shrinks "
	"as 1/N. The request is quantized to encoder row geometry; startup logs "
	"the requested/applied mapping and validation tools report the VCL census. On Star6E, "
	"1080p delivers only 1,2,3,4,5,6,9,17 and saturates at 17. Restart-only."
};

/* UI descriptors for the snapshot subsystem.  The whole section was API-only
 * (no static SECTIONS rows), so /snapshot.jpg could not be enabled or tuned
 * from the dashboard at all — these give it a "Snapshot" group rendered
 * purely from capabilities. */
static const FieldUi ui_snapshot_enabled = {
	"Snapshot", "Enabled", "toggle", 0, 0, 0, NULL,
	"Gate for /api/v1/snapshot.jpg (MJPEG pulse-encode; also the QR-scan "
	"source — qr_decode reads JPEG). Off means no MJPEG channel is "
	"allocated and the endpoint answers 503."
};
static const FieldUi ui_snapshot_quality = {
	"Snapshot", "JPEG quality", "number", 1, 99, 1, NULL,
	"MJPEG q-factor for /api/v1/snapshot.jpg. Applied live on the running "
	"channel."
};
static const FieldUi ui_snapshot_width = {
	"Snapshot", "JPEG width", "number", 0, 8192, 16, NULL,
	"Max encodable width of the MJPEG snapshot channel — NOT a scaler. "
	"0 = inherit the main stream, which is the only safe value: VENC has no "
	"scaler, so a value below the main stream makes every frame fail "
	"validation and the endpoint answers 504 forever."
};
static const FieldUi ui_snapshot_height = {
	"Snapshot", "JPEG height", "number", 0, 8192, 2, NULL,
	"Max encodable height of the MJPEG snapshot channel. 0 = inherit the "
	"main stream; see the width field."
};
static const FieldUi ui_qr_tap_enabled = {
	"QR", "Luma tap", "toggle", 0, 0, 0, NULL,
	"Enable the overlay-free NV12 luma tap on VPE port1 (Star6E). Unlike the "
	"MJPEG snapshot, this carries no OSD pixels. Requires port1 to be free: "
	"turn off video0.framing=stab and detect.enabled first."
};
static const FieldUi ui_qr_tap_width = {
	"QR", "Tap width", "number", 0, 4096, 16, NULL,
	"Width of the port1 luma tap. 0 = inherit the main stream. A VPE port is "
	"a real scaler, so this genuinely changes capture resolution."
};
static const FieldUi ui_qr_window_ms = {
	"QR", "Scan window (ms)", "number", 1000, 60000, 500, NULL,
	"How long a /api/v1/qr/scan holds VPE port1 before the supervisor closes "
	"it and hands the port back to stab/detect. Re-scanning while a window is "
	"open only extends the deadline; it never re-opens the port."
};
static const FieldUi ui_qr_tap_height = {
	"QR", "Tap height", "number", 0, 4096, 2, NULL,
	"Height of the port1 luma tap. 0 = inherit the main stream."
};

static const FieldDesc g_fields[] = {
	FIELD(system, web_port,        FT_UINT16, MUT_RESTART),
	FIELD(system, overclock_level, FT_INT,    MUT_RESTART),
	FIELD(system, verbose,         FT_BOOL,   MUT_LIVE),

	FIELD(sensor, index,           FT_INT,    MUT_RESTART),
	FIELD(sensor, mode,            FT_INT,    MUT_RESTART),

	FIELD(isp, sensor_bin,         FT_STRING, MUT_LIVE),
	FIELD(isp, gain_max,           FT_UINT,   MUT_LIVE),
	FIELD(isp, shutter_max_us,     FT_UINT,   MUT_LIVE),
	FIELD(isp, gain_min,           FT_UINT,   MUT_LIVE),
	FIELD(isp, shutter_min_us,     FT_UINT,   MUT_LIVE),
	FIELD(isp, awb_mode,           FT_STRING, MUT_LIVE),
	FIELD(isp, awb_ct,             FT_UINT,   MUT_LIVE),

	FIELD(image, mirror,           FT_BOOL,   MUT_RESTART),
	FIELD(image, flip,             FT_BOOL,   MUT_RESTART),
	FIELD(image, rotate,           FT_INT,    MUT_RESTART),

	FIELD(video0, rc_mode,         FT_STRING, MUT_RESTART),
	FIELD(video0, fps,             FT_UINT,   MUT_LIVE),
	{ "video0.size", FT_SIZE, MUT_RESTART,
	  offsetof(VencConfig, video0.width),
	  sizeof(uint32_t) * 2, NULL },  /* covers width + height */
	FIELD(video0, bitrate,         FT_UINT,   MUT_LIVE),
	FIELD(video0, gop_size,        FT_DOUBLE, MUT_LIVE),
	FIELD(video0, qp_delta,        FT_INT,    MUT_LIVE),
	FIELD_UI(video0, min_qp,       FT_UINT,   MUT_LIVE, &ui_min_qp),
	FIELD_UI(video0, max_qp,       FT_UINT,   MUT_LIVE, &ui_max_qp),
	FIELD(outgoing, enabled,           FT_BOOL,   MUT_LIVE),
	FIELD(outgoing, server,            FT_STRING, MUT_LIVE),
	FIELD(outgoing, stream_mode,       FT_STRING, MUT_RESTART),
	FIELD(outgoing, max_payload_size,  FT_UINT16, MUT_LIVE),
	FIELD(outgoing, connected_udp,     FT_BOOL,   MUT_RESTART),
	FIELD(outgoing, allow_unix_encoder_stall, FT_BOOL, MUT_RESTART),
	FIELD(outgoing, audio_port,        FT_INT,    MUT_RESTART),
	FIELD(outgoing, sidecar_port,      FT_UINT16, MUT_RESTART),

	/* mDNS device beacon — read at boot / re-read on SIGHUP-respawn, so
	 * all restart-required (no live re-announce path). */
	FIELD(discovery, enabled,      FT_BOOL,   MUT_RESTART),
	FIELD(discovery, service_type, FT_STRING, MUT_RESTART),
	FIELD(discovery, name,         FT_STRING, MUT_RESTART),
	FIELD(discovery, bare_alias,   FT_BOOL,   MUT_RESTART),

	FIELD(isp, ae_engine,         FT_STRING, MUT_RESTART),
	FIELD(isp, ae_fps,            FT_UINT,   MUT_RESTART),
	FIELD_UI(isp, awb_fps,        FT_UINT,   MUT_LIVE,    &ui_awb_fps),
	FIELD(isp, keep_aspect,       FT_BOOL,   MUT_RESTART),
	FIELD(isp, shutter_rule_180,  FT_BOOL,   MUT_RESTART),

	FIELD(audio, enabled,      FT_BOOL,   MUT_RESTART),
	FIELD(audio, sample_rate,  FT_UINT,   MUT_RESTART),
	FIELD(audio, channels,     FT_UINT,   MUT_RESTART),
	FIELD(audio, codec,        FT_STRING, MUT_RESTART),
	FIELD(audio, volume,       FT_INT,    MUT_RESTART),
	FIELD(audio, mute,         FT_BOOL,   MUT_LIVE),

	FIELD(fpv, roi_enabled,  FT_BOOL,   MUT_LIVE),
	FIELD(fpv, roi_qp,       FT_INT,    MUT_LIVE),
	FIELD(fpv, roi_steps,    FT_UINT16, MUT_LIVE),
	FIELD(fpv, roi_center,   FT_DOUBLE, MUT_LIVE),
	FIELD(fpv, noise_level,  FT_INT,    MUT_RESTART),

	FIELD(imu, enabled,        FT_BOOL,   MUT_RESTART),
	FIELD(imu, i2c_device,     FT_STRING, MUT_RESTART),
	FIELD(imu, i2c_addr,       FT_UINT8,  MUT_RESTART),
	FIELD(imu, sample_rate_hz, FT_INT,    MUT_RESTART),
	FIELD(imu, gyro_range_dps, FT_INT,    MUT_RESTART),
	FIELD(imu, cal_file,       FT_STRING, MUT_RESTART),
	FIELD(imu, cal_samples,    FT_INT,    MUT_RESTART),

	FIELD(record, enabled,     FT_BOOL,   MUT_RESTART),
	FIELD(record, dir,         FT_STRING, MUT_RESTART),
	FIELD(record, format,      FT_STRING, MUT_RESTART),
	FIELD(record, mode,        FT_STRING, MUT_RESTART),
	FIELD(record, max_seconds, FT_UINT,   MUT_RESTART),
	FIELD(record, max_mb,      FT_UINT,   MUT_RESTART),
	FIELD(record, bitrate,     FT_UINT,   MUT_RESTART),
	FIELD(record, fps,         FT_UINT,   MUT_RESTART),
	FIELD(record, gop_size,    FT_DOUBLE, MUT_RESTART),
	FIELD(record, server,      FT_STRING, MUT_RESTART),

	FIELD_UI(snapshot, enabled, FT_BOOL, MUT_RESTART, &ui_snapshot_enabled),
	FIELD_UI(snapshot, quality, FT_UINT, MUT_LIVE,    &ui_snapshot_quality),
	FIELD(snapshot, channel,   FT_INT,    MUT_RESTART),
	FIELD_UI(snapshot, width,  FT_UINT, MUT_RESTART, &ui_snapshot_width),
	FIELD_UI(snapshot, height, FT_UINT, MUT_RESTART, &ui_snapshot_height),
	FIELD(video0, scene_threshold,  FT_UINT16, MUT_RESTART),
	FIELD(video0, scene_holdoff,   FT_UINT8,  MUT_RESTART),
	FIELD_UI(video0, slice_count,  FT_UINT,   MUT_RESTART, &ui_slice_count),
	/* Frame gate: restart-required like resilience/sliceCount.  It is a
	 * set-once tuning knob, not something swept in flight, so it does not
	 * earn a live-apply group. */
	FIELD_UI(video0, frame_gate_close_slots, FT_UINT, MUT_RESTART,
		&ui_frame_gate_close_slots),
	FIELD_UI(video0, frame_gate_max_closed_ms, FT_UINT, MUT_RESTART,
		&ui_frame_gate_max_closed_ms),
	FIELD(video0, resilience,           FT_STRING, MUT_RESTART),
	FIELD_UI(video0, intra_refresh_qp, FT_UINT8, MUT_RESTART, &ui_intra_refresh_qp),
	/* zoom_x/y stay live for smooth panning via MI_VPE_SetPortCrop; the zoom
	 * magnitude is part of the framing preset (derived zoom_pct), not a
	 * settable field. */
	FIELD(video0, zoom_x,      FT_DOUBLE, MUT_LIVE),
	FIELD(video0, zoom_y,      FT_DOUBLE, MUT_LIVE),
	/* Framing preset — sole user-facing knob for the VPE crop (stabilization
	 * preset "stab" or zoom presets zoom-1.25x..zoom-4x); expands into derived
	 * stab_crop_pct/recenter or zoom_pct.  Encoded resolution changes when
	 * toggled, so the whole pipeline must restart. */
	FIELD(video0, framing,             FT_STRING, MUT_RESTART),
	/* Stabilization knobs — shared by stab + stab-fill (one Kalman control law).
	 * crop_pct = kept-frame/border budget (0 or 60..100); recenter_speed = the
	 * pauseStab glide-home rate; kalman_q/r = the pan-response / smoothness of
	 * the trajectory filter.  Set framing=stab|stab-fill first; inert under
	 * off/zoom.  Restart-required.  All carry FIELD_UI metadata so the dashboard
	 * renders the whole "Stabilization" group data-driven. */
	FIELD_UI(video0, stab_crop_pct,       FT_UINT,   MUT_RESTART, &ui_stab_crop_pct),
	FIELD_UI(video0, stab_kalman_q,       FT_DOUBLE, MUT_RESTART, &ui_stab_kalman_q),
	FIELD_UI(video0, stab_kalman_r,       FT_DOUBLE, MUT_RESTART, &ui_stab_kalman_r),
	FIELD_UI(video0, stab_recenter_speed, FT_UINT,   MUT_RESTART, &ui_stab_recenter_speed),
	FIELD_UI(video0, stab_accuracy,       FT_STRING, MUT_RESTART, &ui_stab_accuracy),
	/* Runtime stab pause (D13 software ramp) — MUT_LIVE, not persisted.  Carries
	 * UI metadata so the dashboard renders it data-driven (no static SECTIONS
	 * row; it isn't in /api/v1/config). */
	FIELD_UI(video0, pause_stab,       FT_BOOL,   MUT_LIVE, &ui_pause_stab),
	FIELD(debug,  show_osd,    FT_BOOL,   MUT_RESTART),
	/* Attitude export over the rtp_sidecar trailer (requires imu.enabled;
	 * Star6E only). MUT_RESTART: the estimator + trailer wiring is set up
	 * at pipeline init. */
	FIELD(attitude, enabled,      FT_BOOL, MUT_RESTART),
	FIELD(attitude, mount_deg,    FT_INT,  MUT_RESTART),
	FIELD(attitude, invert_roll,  FT_BOOL, MUT_RESTART),
	FIELD(attitude, invert_pitch, FT_BOOL, MUT_RESTART),
	FIELD(attitude, axis_fwd,     FT_STRING, MUT_RESTART),
	FIELD(attitude, axis_down,    FT_STRING, MUT_RESTART),
	FIELD(attitude, trim_roll_deg,  FT_FLOAT, MUT_RESTART),
	FIELD(attitude, trim_pitch_deg, FT_FLOAT, MUT_RESTART),

	/* Live, NOT MUT_RESTART: a respawn triggered while detection is running
	 * leaves the successor's pipeline permanently frameless (stuck ISP CMDQ —
	 * see star6e_controls_service_detect_reload), and toggling the detector
	 * in place is both safe and what the operator wants anyway (no stream
	 * outage).  The port1 tap is created/destroyed live by the same code the
	 * model swap already uses. */
	FIELD(detect, enabled,        FT_BOOL,   MUT_LIVE),
	FIELD(detect, plugin,         FT_STRING, MUT_RESTART),
	/* model_path/model_id/conf_thresh/nms_iou apply live: the detector plugin
	 * + VPE tap are re-created without respawning the pipeline, so the video0
	 * RTP stream is uninterrupted (see LIVE_GROUP_DETECT / apply_detect_reload).
	 * net_width/net_height stay MUT_RESTART — a tap-geometry change needs the
	 * VPE port recreated, which only the full respawn path does. */
	FIELD(detect, model_path,     FT_STRING, MUT_LIVE),
	FIELD(detect, model_id,       FT_UINT,   MUT_LIVE),
	FIELD(detect, conf_thresh,    FT_FLOAT,  MUT_LIVE),
	FIELD(detect, nms_iou,        FT_FLOAT,  MUT_LIVE),
	FIELD(detect, net_width,      FT_UINT,   MUT_RESTART),
	FIELD(detect, net_height,     FT_UINT,   MUT_RESTART),
	FIELD(detect, firmware_path,  FT_STRING, MUT_RESTART),
	FIELD(detect, infer_interval, FT_INT,    MUT_RESTART),
	FIELD(detect, osd,            FT_BOOL,   MUT_RESTART),
	/* MUT_RESTART throughout: the tap port is programmed at graph configure
	 * time, and reprogramming a live port is exactly what this design exists
	 * to avoid (see documentation/QR_LUMA_TAP_PLAN.md). */
	FIELD_UI(qr, tap_enabled, FT_BOOL, MUT_RESTART, &ui_qr_tap_enabled),
	FIELD_UI(qr, tap_width,   FT_UINT, MUT_RESTART, &ui_qr_tap_width),
	FIELD_UI(qr, tap_height,  FT_UINT, MUT_RESTART, &ui_qr_tap_height),
	/* Live: read when a window opens, so a change takes effect next scan. */
	FIELD_UI(qr, window_ms,   FT_UINT, MUT_LIVE,    &ui_qr_window_ms),
};

#define FIELD_COUNT (sizeof(g_fields) / sizeof(g_fields[0]))

static const FieldDesc *find_field(const char *key)
{
	for (size_t i = 0; i < FIELD_COUNT; i++) {
		if (strcmp(g_fields[i].key, key) == 0)
			return &g_fields[i];
	}
	return NULL;
}

typedef struct {
	const char *alias;
	const char *canonical;
} FieldAlias;

static const FieldAlias g_field_aliases[] = {
	{ "system.webPort", "system.web_port" },
	{ "system.overclockLevel", "system.overclock_level" },
	{ "isp.sensorBin", "isp.sensor_bin" },
	{ "isp.gainMax", "isp.gain_max" },
	{ "isp.shutterMaxUs", "isp.shutter_max_us" },
	{ "isp.gainMin", "isp.gain_min" },
	{ "isp.shutterMinUs", "isp.shutter_min_us" },
	{ "isp.awbMode", "isp.awb_mode" },
	{ "isp.awbCt", "isp.awb_ct" },
	{ "video0.rcMode", "video0.rc_mode" },
	{ "video0.gopSize", "video0.gop_size" },
	{ "video0.qpDelta", "video0.qp_delta" },
	{ "video0.minQp", "video0.min_qp" },
	{ "video0.maxQp", "video0.max_qp" },
	{ "outgoing.maxPayloadSize", "outgoing.max_payload_size" },
	{ "outgoing.audioPort", "outgoing.audio_port" },
	{ "fpv.roiEnabled", "fpv.roi_enabled" },
	{ "fpv.roiQp", "fpv.roi_qp" },
	{ "fpv.roiSteps", "fpv.roi_steps" },
	{ "fpv.roiCenter", "fpv.roi_center" },
	{ "fpv.noiseLevel", "fpv.noise_level" },
	{ "isp.aeEngine", "isp.ae_engine" },
	{ "isp.aeFps", "isp.ae_fps" },
	{ "isp.awbFps", "isp.awb_fps" },
	{ "isp.keepAspect", "isp.keep_aspect" },
	{ "isp.shutterRule180", "isp.shutter_rule_180" },
	{ "audio.sampleRate", "audio.sample_rate" },
	{ "imu.i2cDevice", "imu.i2c_device" },
	{ "imu.i2cAddr", "imu.i2c_addr" },
	{ "imu.sampleRateHz", "imu.sample_rate_hz" },
	{ "imu.gyroRangeDps", "imu.gyro_range_dps" },
	{ "imu.calFile", "imu.cal_file" },
	{ "imu.calSamples", "imu.cal_samples" },
	{ "record.maxSeconds", "record.max_seconds" },
	{ "record.maxMB", "record.max_mb" },
	{ "record.gopSize", "record.gop_size" },
	{ "video0.sceneThreshold", "video0.scene_threshold" },
	{ "video0.sceneHoldoff", "video0.scene_holdoff" },
	{ "video0.sliceCount", "video0.slice_count" },
	{ "video0.frameGateCloseSlots", "video0.frame_gate_close_slots" },
	{ "video0.frameGateMaxClosedMs", "video0.frame_gate_max_closed_ms" },
	{ "video0.intraRefreshQp", "video0.intra_refresh_qp" },
	{ "video0.zoomX", "video0.zoom_x" },
	{ "video0.zoomY", "video0.zoom_y" },
	{ "video0.stabCropPct", "video0.stab_crop_pct" },
	{ "video0.stabRecenterSpeed", "video0.stab_recenter_speed" },
	{ "video0.stabKalmanQ", "video0.stab_kalman_q" },
	{ "video0.stabKalmanR", "video0.stab_kalman_r" },
	{ "video0.stabAccuracy", "video0.stab_accuracy" },
	{ "video0.pauseStab", "video0.pause_stab" },
	{ "outgoing.sidecarPort", "outgoing.sidecar_port" },
	{ "outgoing.connectedUdp", "outgoing.connected_udp" },
	{ "outgoing.allowUnixEncoderStall", "outgoing.allow_unix_encoder_stall" },
	{ "outgoing.streamMode", "outgoing.stream_mode" },
	{ "discovery.serviceType", "discovery.service_type" },
	{ "discovery.bareAlias", "discovery.bare_alias" },
	{ "debug.showOsd", "debug.show_osd" },
	{ "attitude.mountDeg", "attitude.mount_deg" },
	{ "attitude.invertRoll", "attitude.invert_roll" },
	{ "attitude.invertPitch", "attitude.invert_pitch" },
	{ "attitude.axisFwd", "attitude.axis_fwd" },
	{ "attitude.axisDown", "attitude.axis_down" },
	{ "attitude.trimRollDeg", "attitude.trim_roll_deg" },
	{ "attitude.trimPitchDeg", "attitude.trim_pitch_deg" },
	{ "detect.modelPath", "detect.model_path" },
	{ "detect.firmwarePath", "detect.firmware_path" },
	{ "detect.inferInterval", "detect.infer_interval" },
	{ "detect.confThresh", "detect.conf_thresh" },
	{ "detect.nmsIou", "detect.nms_iou" },
	{ "detect.netWidth", "detect.net_width" },
	{ "detect.netHeight", "detect.net_height" },
	{ "detect.modelId", "detect.model_id" },
	{ "qr.tapEnabled", "qr.tap_enabled" },
	{ "qr.tapWidth", "qr.tap_width" },
	{ "qr.tapHeight", "qr.tap_height" },
	{ "qr.windowMs", "qr.window_ms" },
};

static const char *canonicalize_field_key(const char *key)
{
	if (!key)
		return NULL;

	for (size_t i = 0; i < sizeof(g_field_aliases) / sizeof(g_field_aliases[0]); i++) {
		if (strcmp(g_field_aliases[i].alias, key) == 0)
			return g_field_aliases[i].canonical;
	}

	return key;
}

/* Fields the shared table marks MUT_LIVE that CV610 reads only at start.
 * Star6E and Maruko can retarget the transport, re-rate the encoder and
 * change mic gain without a respawn; the CV610 slice reads these once in
 * cv610_prepare() / cv610_output_start() / cv610_audio_start() and never
 * again.  Reporting them live would give the dashboard a control that
 * accepts a value and silently does nothing — the same lie the Maruko
 * capability gates exist to prevent. */
static int cv610_field_is_restart_only(const char *canonical_key)
{
	static const char *const restart_only[] = {
		"video0.fps",        /* pipeline fps, MIPI raw_bit, RTP clock */
		/* outgoing.enabled and outgoing.server LEFT this list in 0.77.0,
		 * once cv610_apply_output_enabled() and cv610_apply_server()
		 * existed to honour them.  Order matters and is not cosmetic:
		 * live_group_supported_for_cfg() gates LIVE_GROUP_OUTGOING on
		 * those callbacks being present, so widening first would have
		 * advertised `live` in /api/v1/capabilities while every write
		 * was rejected -- the exact lie this list exists to prevent. */
		"audio.mute",        /* acodec gain set once, at audio start */
	};
	size_t i;

	for (i = 0; i < sizeof(restart_only) / sizeof(restart_only[0]); ++i) {
		if (strcmp(canonical_key, restart_only[i]) == 0)
			return 1;
	}
	return 0;
}

/* Mutability as this backend can actually honour it.  Never more permissive
 * than the shared table — a backend may only downgrade live to restart. */
static Mutability field_mut_for_backend(const FieldDesc *f)
{
	if (!f)
		return MUT_RESTART;
	if (f->mut == MUT_LIVE && strcmp(g_backend, "cv610") == 0 &&
	    cv610_field_is_restart_only(f->key))
		return MUT_RESTART;
	return f->mut;
}

int venc_api_field_supported_for_backend(const char *backend_name,
	const char *field_key)
{
	const char *canonical_key;

	canonical_key = canonicalize_field_key(field_key);
	if (!canonical_key)
		return 0;

	/* CV610 starts with an intentionally small, truthful control surface.
	 * All other shared-schema fields remain visible in config JSON but are
	 * advertised unsupported and rejected before mutation.
	 *
	 * Every entry below is a field the CV610 backend genuinely reads —
	 * either through g_cv610_apply_callbacks (live) or once in
	 * cv610_prepare()/cv610_init()/cv610_audio_start() (restart-class, see
	 * cv610_field_is_restart_only()).  Fields the slice hardcodes are
	 * deliberately absent even when the shipped default happens to match
	 * the hardcoded value: audio.sample_rate/channels/codec/volume are
	 * fixed at 48000/1/opus/gain 8 in src/cv610_audio.c, and video0.rc_mode
	 * is fixed at H.265 CBR in cv610_venc_start().  Advertising those would
	 * accept a value the encoder never reads. */
	if (backend_name && strcmp(backend_name, "cv610") == 0) {
		static const char *const supported[] = {
			"system.web_port", "system.verbose",
			"sensor.index", "sensor.mode",
			"isp.keep_aspect",
			/* The two portable AE ceilings, mapped onto the exposure
			 * group cv610_iq.c already owns.  Wired because the units
			 * match exactly and need no conversion: exp_time_range is
			 * documented "unit: us" and a_gain_range "Format:22.10 ...
			 * unit: times, 10bit precision", i.e. 1024 == 1x, which is
			 * the scale the SigmaStar supervisory AE uses.  The live
			 * board confirms the format independently -- sys_gain_max
			 * 1630616 == a_gain_max 407654 x ispd_gain_max 4096 / 1024.
			 * Listed because the ceilings MOVE THE AE LOOP on this
			 * part, not merely because the write returns success.
			 * Measured on a CV610 bench, sc4336p, ave_lum 43: capping
			 * shutterMaxUs to 4000 pulled the applied exp_time from
			 * 16560 to exactly 4000 us and the AE raised a_gain 1497
			 * -> 6611 to hold the same luma, and capping gainMax to
			 * 2048 clamped the applied a_gain to 2043 while the ISP
			 * took up the slack in DIGITAL gain (isp_d_gain 1024 ->
			 * 1948) -- which is also what shows the field is the
			 * ANALOG ceiling rather than the system one.  Clearing
			 * each back to 0 restored 873800 / 407654 and the
			 * original applied values, and a restart with 6000/8192
			 * in the file came up honouring both.  All readings from
			 * /api/v1/awb, which reports the AE's own output.
			 *
			 * isp.gain_min / isp.shutter_min_us are deliberately ABSENT:
			 * the floors are a separate pair and this slice did not
			 * measure them.  isp.awb_mode / isp.awb_ct are absent for a
			 * different reason.  ct_manual means "pin white balance to a
			 * colour temperature", and unlike SigmaStar -- one call,
			 * MI_ISP_AWB_SetCTMwbAttr(ct) -- this SDK has no single CT
			 * setter.  It has ss_mpi_isp_cal_gain_by_temp() (ss_mpi_awb.h,
			 * present in libss_mpi_awb.so), which converts Kelvin to
			 * r/gr/gb/b gains against the CURRENT ot_isp_wb_attr; those
			 * gains then go back through set_wb_attr with op_type manual.
			 * A two-call flow whose result depends on the sensor's AWB
			 * calibration, and this slice neither implemented nor measured
			 * it -- so the pair stays unsupported pending measurement, the
			 * same bar every other entry in this list had to clear.
			 * Manual white balance is reachable today, under its own name,
			 * through /api/v1/iq's "wb" group. */
			"isp.gain_max", "isp.shutter_max_us",
			/* The PQTools `.bin` path, applied through libbin.so by
			 * src/cv610_pq_bin.c -- live via apply_isp_bin and once at
			 * cold boot in cv610_init().  Listed under the same bar as
			 * the AE ceilings above: it MOVES THE ISP on this part, not
			 * merely returns success.  Measured on .181 (IMX662) with a
			 * vendor tune built for a DIFFERENT sensor: the import moved
			 * 39 of the 102 fields /api/v1/iq reads back, across ten
			 * groups, and the operator saw the picture go red -- which is
			 * the point, since a foreign tune brings its own CCM and AWB.
			 * Export round-trips: importing our own exported file
			 * restored a read-back identical to the pre-import one.
			 *
			 * Unlike the SigmaStar backends there is no
			 * /etc/sensors/<sensor>.bin fallback, so an empty value is a
			 * no-op rather than a resolve -- which is also what makes the
			 * cold-boot apply free on a craft that names no bin. */
			"isp.sensor_bin",
			/* Applied at the SENSOR by apply_sensor_orientation(),
			 * through the plugin's pfn_mirror_flip — the same place
			 * both SigmaStar backends apply orientation, so image.*
			 * means one thing across the fleet.  Both are already
			 * MUT_RESTART in the shared table, so no
			 * cv610_field_is_restart_only() entry and no mutability
			 * widening.
			 *
			 * image.rotate is deliberately ABSENT, though the pair it
			 * decomposes into is here.  The decomposition lives in
			 * venc_config's load_image(), which runs on a FILE parse
			 * and nowhere else, so it never sees a value that arrives
			 * through /api/v1/set.  Listing it would turn today's
			 * clean 501 into: accept 90, persist 90, raise
			 * reinit_pending, restart the encoder, and read back 0
			 * once load_image() coerces it — measured on .181.  A
			 * config file carrying rotate:180 still works, because
			 * load_image() decomposes it before the backend reads
			 * mirror/flip; the field reads unsupported because no
			 * backend reads ROTATE, which is what this list means. */
			"image.mirror", "image.flip",
			"video0.fps", "video0.size",
			"video0.bitrate", "video0.gop_size",
			"video0.slice_count", "video0.resilience",
			/* Read by cv610_runtime.c's frame_gate_setup() -- the
			 * gate is armed whenever the output is a frame ring
			 * (frame-shm://, the CV610 default), so both knobs are
			 * fields this backend genuinely reads.  The shared table
			 * already marks them MUT_RESTART, which is what the
			 * backend wants too: the gate is built once at pipeline
			 * start and re-read across a respawn. */
			"video0.frame_gate_close_slots",
			"video0.frame_gate_max_closed_ms",
			/* video0.qp_delta is deliberately ABSENT.  CV610's CBR rate
			 * controller stores gop_attr.normal_p.ip_qp_delta and ignores
			 * it (measured; README), and the register that does move
			 * I-frame size is intra refresh's request_i_qp, which is the
			 * GDR recovery anchor owned by the resilience preset.  Driving
			 * qpDelta into it would silently retune the anchor, so the
			 * anchor is exposed under its own name instead. */
			"video0.intra_refresh_qp",
			/* Read by cv610_apply_qp_bounds(), live and at startup.
			 * CBR cannot hold its target in a noise-dominated scene
			 * without room to raise QP. */
			"video0.min_qp", "video0.max_qp",
			/* Read by cv610_apply_roi_qp(), live and at cold boot.
			 * Listed only because the delta reaches the encoder and
			 * moves the picture ON THIS PART -- a measurement on a
			 * sibling SoC would not qualify it, because the SDK call
			 * differs (ss_mpi_venc_set_roi_attr here,
			 * MI_VENC_SetRoiCfg there).  Measured on a CV610 bench,
			 * 720p100 CBR: with a 384-px band at x=448, decoded
			 * detail inside the band drops 7.4x between roiQp -30 and
			 * +30 while the columns just outside it move the opposite
			 * way, and a repeat of the -30 arm lands within 8.8%.
			 * That measurement is the bar for this list -- issue #259
			 * is the counter-example, where an SDK call returned
			 * success, logged as applied and read back clean while
			 * the bitstream never moved. */
			"fpv.roi_enabled", "fpv.roi_qp",
			"fpv.roi_steps", "fpv.roi_center",
			"outgoing.enabled", "outgoing.server",
			"outgoing.max_payload_size", "outgoing.audio_port",
			"outgoing.connected_udp",
			"outgoing.allow_unix_encoder_stall",
			/* Read once by cv610_output_start(), which binds the
			 * sidecar listener before the transport dispatch.  The
			 * shared table already marks it MUT_RESTART, so there is
			 * no cv610_field_is_restart_only() entry to add and no
			 * mutability widening here. */
			"outgoing.sidecar_port",
			"audio.enabled", "audio.mute",
			/* Snapshot: the JPEG channel is a second bind target on
			 * the main stream's VPSS output, so it inherits that
			 * geometry.  snapshot.width/height are deliberately NOT
			 * listed — nothing reads them here, and advertising a
			 * field the encoder ignores is what this list exists to
			 * prevent. */
			"snapshot.enabled", "snapshot.quality",
			"snapshot.channel",
			/* Recording, mirror mode only.  record.bitrate/fps/
			 * gop_size/server describe a second VENC channel that
			 * dual and dual-stream would need; those modes are not
			 * implemented on this backend. */
			"record.enabled", "record.dir", "record.format",
			"record.mode", "record.max_seconds", "record.max_mb",
			"debug.show_osd",
			"discovery.enabled", "discovery.service_type",
			"discovery.name", "discovery.bare_alias",
		};
		size_t i;

		for (i = 0; i < sizeof(supported) / sizeof(supported[0]); ++i) {
			if (strcmp(canonical_key, supported[i]) == 0)
				break;
		}
		if (i == sizeof(supported) / sizeof(supported[0]))
			return 0;

		/* One entry is conditional on more than the backend name.
		 * isp.sensor_bin needs the vendor PQ blob, which is not shipped in
		 * the source tree, so a craft flashed without it must not advertise
		 * the field as supported and then 501 every write.  cv610_init drops
		 * apply_isp_bin when the probe fails, so the callback is the truth --
		 * the same thing routes.iq_export_bin already tracks.  Before
		 * registration g_cb is NULL; answer from the list then, since no
		 * request can be in flight yet. */
		if (g_cb && strcmp(canonical_key, "isp.sensor_bin") == 0)
			return g_cb->apply_isp_bin != NULL;
		return 1;
	}

	/* video0.intra_refresh_qp reaches MI_VENC_SetIntraRefresh on Star6E and
	 * Maruko and is logged as applied ("intraRefresh: ... qp=10"), but the
	 * SigmaStar encoder ignores it: sweeping it 10 / 36 / 48 on .232 moved
	 * IRAP 80099 / 79791 / 79566 and the delivered rate not at all, and
	 * 0 vs 10 on .233 moved IRAP 16485 / 16466.  A 38-QP span with no effect
	 * is not a control, so only CV610 -- where it IS the I-frame lever --
	 * advertises it.  (Which also means mode_default_qp()'s per-mode stripe
	 * QP is inert on the SigmaStar parts.) */
	if (backend_name && strcmp(backend_name, "cv610") != 0 &&
	    strcmp(canonical_key, "video0.intra_refresh_qp") == 0)
		return 0;

	/* These controls have no Maruko implementation.  Keep them in the shared
	 * schema so clients can render one dashboard, but advertise them honestly
	 * and reject writes before they reach a missing callback or route.
	 *
	 * video0.min_qp/max_qp left this list when maruko_apply_qp_bounds()
	 * landed; Maruko is the same MI VENC RC as Star6E, so the bounds are
	 * the same u32MinQp/u32MaxQp write. */
	if (backend_name && strcmp(backend_name, "maruko") == 0 &&
	    strncmp(canonical_key, "qr.", 3) == 0)
		return 0;

	/* isp.awb_fps paces the Star6E userspace AWB loop (src/star6e_awb.c),
	 * which exists because the i6e ISP-internal AWB does not converge.
	 * Maruko has no such loop — its AWB is driven by the SDK's own 3A via
	 * the CUS3A RunOnce pacer, which i6e does not export — so there is no
	 * rate to set.  Advertise it unsupported: the WebUI greys the control
	 * and the set-path rejects writes, rather than accepting a value that
	 * would silently do nothing. */
	if (backend_name && strcmp(backend_name, "maruko") == 0 &&
	    strcmp(canonical_key, "isp.awb_fps") == 0)
		return 0;

	/* Image stabilization (video0.framing=stab + the stab_* / pause_stab tuning
	 * group) is now supported on Maruko/I6C: the IVE motion detector works once
	 * the BSP-matched libmi_ive.so is installed (see the builder osdrv override),
	 * driven by src/maruko_framing_stab.c.  No backend gate — the knobs are live
	 * on both SoCs.  (stab-fill remains Star6E-only, gated in the dashboard.) */

	return 1;
}

/* ── Field value helpers ─────────────────────────────────────────────── */

/* Format a field value as a JSON fragment string (caller must free).
 * Uses cJSON for strings to ensure proper escaping of special chars. */
static char *field_to_json_value_from_cfg(const VencConfig *cfg,
	const FieldDesc *f)
{
	const void *ptr;
	char buf[320];

	if (!cfg || !f)
		return strdup("null");

	ptr = (const char *)cfg + f->offset;
	switch (f->type) {
	case FT_BOOL:
		snprintf(buf, sizeof(buf), "%s", *(const bool *)ptr ? "true" : "false");
		return strdup(buf);
	case FT_INT:
		snprintf(buf, sizeof(buf), "%d", *(const int *)ptr);
		return strdup(buf);
	case FT_UINT:
		snprintf(buf, sizeof(buf), "%u", *(const uint32_t *)ptr);
		return strdup(buf);
	case FT_UINT8:
		snprintf(buf, sizeof(buf), "%u", (unsigned)*(const uint8_t *)ptr);
		return strdup(buf);
	case FT_UINT16:
		snprintf(buf, sizeof(buf), "%u", (unsigned)*(const uint16_t *)ptr);
		return strdup(buf);
	case FT_DOUBLE:
		snprintf(buf, sizeof(buf), "%g", *(const double *)ptr);
		return strdup(buf);
	case FT_FLOAT:
		snprintf(buf, sizeof(buf), "%.6g", (double)*(const float *)ptr);
		return strdup(buf);
	case FT_STRING: {
		cJSON *s = cJSON_CreateString((const char *)ptr);
		if (!s) return strdup("\"\"");
		char *json = cJSON_PrintUnformatted(s);
		cJSON_Delete(s);
		return json;
	}
	case FT_SIZE: {
		const uint32_t *wh = (const uint32_t *)ptr;
		if (wh[0] == 0 && wh[1] == 0)
			return strdup("\"auto\"");
		snprintf(buf, sizeof(buf), "\"%ux%u\"", wh[0], wh[1]);
		return strdup(buf);
	}
	}
	return strdup("null");
}

static char *field_to_json_value(const FieldDesc *f)
{
	return field_to_json_value_from_cfg(g_cfg, f);
}

/* Parse a string value and write it into the config field.
 * Returns 0 on success, -1 on parse error. */
static int field_from_string_cfg(VencConfig *cfg, const FieldDesc *f,
	const char *val)
{
	void *ptr;

	if (!cfg || !f || !val)
		return -1;

	ptr = (char *)cfg + f->offset;
	switch (f->type) {
	case FT_BOOL:
		if (strcmp(val, "true") == 0 || strcmp(val, "1") == 0)
			*(bool *)ptr = true;
		else if (strcmp(val, "false") == 0 || strcmp(val, "0") == 0)
			*(bool *)ptr = false;
		else
			return -1;
		break;
	case FT_INT: {
		char *end;
		long v = strtol(val, &end, 10);
		if (end == val || *end != '\0') return -1;
		*(int *)ptr = (int)v;
		break;
	}
	case FT_UINT: {
		char *end;
		unsigned long v = strtoul(val, &end, 10);
		if (end == val || *end != '\0') return -1;
		*(uint32_t *)ptr = (uint32_t)v;
		break;
	}
	case FT_UINT8: {
		char *end;
		unsigned long v = strtoul(val, &end, 0);  /* base 0: accepts 0x hex */
		if (end == val || *end != '\0' || v > 255) return -1;
		*(uint8_t *)ptr = (uint8_t)v;
		break;
	}
	case FT_UINT16: {
		char *end;
		unsigned long v = strtoul(val, &end, 10);
		if (end == val || *end != '\0' || v > 65535) return -1;
		*(uint16_t *)ptr = (uint16_t)v;
		break;
	}
	case FT_DOUBLE: {
		char *end;
		double v = strtod(val, &end);
		if (end == val || *end != '\0') return -1;
		*(double *)ptr = v;
		break;
	}
	case FT_FLOAT: {
		char *end;
		float v = (float)strtod(val, &end);
		if (end == val || *end != '\0') return -1;
		*(float *)ptr = v;
		break;
	}
	case FT_STRING:
		snprintf((char *)ptr, f->size, "%s", val);
		break;
	case FT_SIZE: {
		uint32_t w, h;
		if (!strcmp(val, "auto")) { w = 0; h = 0; }
		else if (!strcmp(val, "720p")) { w = 1280; h = 720; }
		else if (!strcmp(val, "1080p")) { w = 1920; h = 1080; }
		else if (sscanf(val, "%ux%u", &w, &h) != 2) return -1;
		uint32_t *wh = (uint32_t *)ptr;
		wh[0] = w;
		wh[1] = h;
		break;
	}
	}
	return 0;
}

/* ── Field-level validation ──────────────────────────────────────────── */

/* Check a single field value after parsing.  Returns NULL if valid,
 * or a static error message string if invalid. */
static const char *validate_field_cfg(const VencConfig *cfg, const char *key)
{
	if (!cfg || !key)
		return "invalid config state";

	if (strcmp(key, "isp.awb_mode") == 0) {
		if (strcmp(cfg->isp.awb_mode, "auto") != 0 &&
		    strcmp(cfg->isp.awb_mode, "ct_manual") != 0)
			return "awb_mode must be 'auto' or 'ct_manual'";
	}
	if (strcmp(key, "isp.ae_engine") == 0) {
		if (strcmp(cfg->isp.ae_engine, "sdk") != 0)
			return "ae_engine must be 'sdk'";
	}
	if (strcmp(key, "isp.sensor_bin") == 0) {
		/* Empty string opts into the /etc/sensors/<sensor>.bin fallback on
		 * the SigmaStar backends -- on CV610 it is a no-op, and there is no
		 * live way to un-apply an imported image short of importing another;
		 * a non-empty path must point at a readable file or the live
		 * apply callback would silently fall back to auto-detect (or to
		 * the previously-loaded bin via dedup) while the persisted
		 * config still names a bogus path.  Note the check cannot help a
		 * path that was readable at set time and is gone by the next boot,
		 * which is what a bin left in tmpfs does. */
		if (cfg->isp.sensor_bin[0] &&
		    access(cfg->isp.sensor_bin, R_OK) != 0)
			return "isp.sensor_bin path is not readable";
	}
	if (strcmp(key, "video0.qp_delta") == 0) {
		if (strcmp(g_backend, "cv610") == 0) {
			if (cfg->video0.qp_delta < -10 || cfg->video0.qp_delta > 30)
				return "CV610 qp_delta must be in range [-10, 30]";
			return NULL;
		}
		if (cfg->video0.qp_delta < -12 || cfg->video0.qp_delta > 12)
			return "qp_delta must be in range [-12, 12]";
	}
	if (strcmp(key, "video0.frame_gate_close_slots") == 0) {
		/* 0 = use the default.  The upper bound is the largest ring we
		 * would ever build; a value above the live slot_count simply
		 * never fires, and the runtime warns about that at bring-up. */
		if (cfg->video0.frame_gate_close_slots > 64)
			return "video0.frame_gate_close_slots must be 0-64";
		return NULL;
	}
	if (strcmp(key, "video0.frame_gate_max_closed_ms") == 0) {
		if (cfg->video0.frame_gate_max_closed_ms > 60000)
			return "video0.frame_gate_max_closed_ms must be 0-60000";
		return NULL;
	}
	if (strcmp(key, "video0.zoom_x") == 0) {
		double v = cfg->video0.zoom_x;
		if (!isfinite(v) || v < 0.0 || v > 1.0)
			return "zoom_x must be in range [0.0, 1.0]";
	}
	if (strcmp(key, "video0.zoom_y") == 0) {
		double v = cfg->video0.zoom_y;
		if (!isfinite(v) || v < 0.0 || v > 1.0)
			return "zoom_y must be in range [0.0, 1.0]";
	}
	if (strcmp(key, "video0.framing") == 0) {
		VencConfigVideo probe;
		if (venc_config_apply_framing_preset(cfg->video0.framing, &probe) != 0)
			return "framing must be one of: off, stab, stab-fill, "
				"zoom-1.25x, zoom-1.50x, zoom-1.75x, zoom-2x, "
				"zoom-3x, zoom-4x";
	}
	/* Without this an unknown preset is committed: the SET returns 200,
	 * persists the bad name to /etc/waybeam.json, and leaves the derived
	 * intra_refresh_* / ref_* fields holding the PREVIOUS preset's
	 * expansion — so /api/v1/config disagrees with the encoder until the
	 * next start, where the disk loader silently falls back to "off" and
	 * the operator lands on a preset they never asked for.  The
	 * parameterised "ltr:<N>" form makes the invalid space wide and
	 * plausible ("ltr:0", "ltr:256"), so this must reject, not clamp. */
	if (strcmp(key, "video0.resilience") == 0) {
		VencConfigVideo probe = cfg->video0;
		if (venc_config_apply_resilience_preset(cfg->video0.resilience,
				&probe) != 0)
			return "resilience must be one of: off, rescue, quality, "
				"sprint, racing, endurance, patrol, rally, range, "
				"fpv, ltr, ltr:<1-255>";
	}
	if (strcmp(key, "attitude.mount_deg") == 0) {
		int v = cfg->attitude.mount_deg;
		if (v != 0 && v != 90 && v != 180 && v != 270)
			return "mount_deg must be 0, 90, 180, or 270";
	}
	if (strcmp(key, "attitude.trim_roll_deg") == 0) {
		float v = cfg->attitude.trim_roll_deg;
		if (!isfinite(v) || v < -180.0f || v > 180.0f)
			return "trim_roll_deg must be finite, in range [-180, 180]";
	}
	if (strcmp(key, "attitude.trim_pitch_deg") == 0) {
		float v = cfg->attitude.trim_pitch_deg;
		if (!isfinite(v) || v < -180.0f || v > 180.0f)
			return "trim_pitch_deg must be finite, in range [-180, 180]";
	}
	if (strcmp(key, "attitude.axis_fwd") == 0 ||
	    strcmp(key, "attitude.axis_down") == 0) {
		/* Reject unparseable or non-orthogonal fwd/down at set time —
		 * otherwise the estimator silently falls back to identity at
		 * init and reports a wrong-but-"valid" attitude. */
		AttitudeAxisMap probe;
		if (attitude_axis_map_init(&probe, cfg->attitude.axis_fwd,
		        cfg->attitude.axis_down, 0.0f, 0.0f) != 0)
			return "axis_fwd/axis_down must each be one of "
				"+x,-x,+y,-y,+z,-z and be non-parallel";
	}
	if (strcmp(key, "video0.stab_crop_pct") == 0) {
		uint32_t v = cfg->video0.stab_crop_pct;
		/* Floor 60: below it the kept window is too small (huge border /
		 * upscale) for usable stabilization on either preset. */
		if (v != 0 && (v < 60 || v > 100))
			return "stab_crop_pct must be 0 (off) or in range [60, 100]";
	}
	if (strcmp(key, "video0.stab_recenter_speed") == 0) {
		if (cfg->video0.stab_recenter_speed > 3600)
			return "stab_recenter_speed must be in range [0, 3600] "
				"(pauseStab glide rate; 0 = default ramp)";
	}
	if (strcmp(key, "video0.stab_kalman_q") == 0) {
		double v = cfg->video0.stab_kalman_q;
		if (v < 0.001 || v > 1.0)
			return "stab_kalman_q must be in range [0.001, 1.0] "
				"(pan response; higher = follows pans faster)";
	}
	if (strcmp(key, "video0.stab_kalman_r") == 0) {
		double v = cfg->video0.stab_kalman_r;
		if (v < 0.1 || v > 50.0)
			return "stab_kalman_r must be in range [0.1, 50.0] "
				"(smoothness; higher = smoother but laggier)";
	}
	if (strcmp(key, "video0.stab_accuracy") == 0) {
		if (!framing_stab_accuracy_valid(cfg->video0.stab_accuracy))
			return "stab_accuracy must be one of: auto, high, medium, low";
	}
	if (strcmp(key, "isp.awb_fps") == 0) {
		/* Ceiling, not a tuning limit: the loop derives its sleep as
		 * 1000/hz in integer ms, so anything above 1000 Hz would round
		 * to a zero sleep and spin a core. 30 is already far past the
		 * point where AWB tracking improves. */
		if (cfg->isp.awb_fps > 30)
			return "awb_fps must be in range [0, 30] "
				"(0 = stop the userspace AWB loop)";
	}
	if (strcmp(key, "fpv.roi_qp") == 0) {
		/* +-20, not +-30.  Beyond 20 the delta stops being honoured at
		 * BOTH ends, because it is a RELATIVE delta and H.265 caps QP at
		 * 51.  Negative is the expensive end: CBR pays for the ROI
		 * discount by raising the frame's base QP roughly 1:1 with
		 * |roiQp|, so once base + |roiQp| passes 51 the rate controller
		 * saturates -- measured on a CV610 bench at 720p60, roiQp -30
		 * pinned every frame at qp 51/51/51 and delivered 16976 kbps
		 * against a 2829 kbps target, a 6x overrun, while -20 landed at
		 * qp 45 and held the target at 2802.  Positive is the benign end
		 * but truncates just as quietly: at +30 the base sat at 21.5, so
		 * the region wanted 51.5 and got 51.  The band width does NOT
		 * move the cliff, only its severity -- at -30, roiCenter 0.6 /
		 * 0.4 / 0.2 all pinned at 51 and delivered 17661 / 12520 / 6977
		 * kbps -- so there is nothing to clamp on that axis instead. */
		if (cfg->fpv.roi_qp < -20 || cfg->fpv.roi_qp > 20)
			return "roi_qp must be in range [-20, 20] (beyond that the "
				"delta exceeds the encoder's QP range: a large "
				"negative value saturates rate control and overruns "
				"the bitrate target)";
	}
	if (strcmp(key, "fpv.roi_steps") == 0) {
		if (cfg->fpv.roi_steps < 1 ||
		    cfg->fpv.roi_steps > PIPELINE_ROI_MAX_STEPS)
			return "roi_steps must be in range [1, 4]";
	}
	if (strcmp(key, "fpv.roi_center") == 0) {
		if (cfg->fpv.roi_center < 0.1 || cfg->fpv.roi_center > 0.9)
			return "roi_center must be in range [0.1, 0.9]";
	}
	if (strcmp(key, "video0.bitrate") == 0) {
		if (cfg->video0.bitrate == 0 || cfg->video0.bitrate > 200000)
			return "bitrate must be 1-200000 kbps";
	}
	if (strcmp(key, "video0.size") == 0) {
		uint32_t w = cfg->video0.width;
		uint32_t h = cfg->video0.height;
		/* w==0 && h==0 means "auto" (use sensor native) — allowed. */
		if (w != 0 || h != 0) {
			if (w < 128 || h < 128 || w > 4096 || h > 2176)
				return "video0.size must be 128-4096 wide and 128-2176 tall (SigmaStar VENC device limit 4096x2176)";
			/* HEVC min-CU alignment: both width and height must be a
			 * multiple of 8.  The encoder's conformance window handles
			 * the remainder up to the CTU, so /8 (not /16) is the real
			 * constraint — native sensor widths like 2952 (÷8, not ÷16)
			 * create the VENC channel and stream fine (the auto path
			 * uses exactly these).  A width below /8 (e.g. 854×480, which
			 * is only ÷2) makes MI_VENC_CreateChn fail with
			 * MI_ERR_VENC_ILLEGAL_PARAM (-1610473469) and the daemon
			 * cannot recover — that is what this gate blocks.  The former
			 * /16 rule needlessly forced `video0.size auto` for 2952-wide
			 * modes; setting the nearest /16 (2944) instead made the SCL
			 * anamorphically downscale 2952→2944 and cost ~7 fps. */
			if (w % 8 != 0)
				return "video0.size width must be a multiple of 8";
			if (h % 8 != 0)
				return "video0.size height must be a multiple of 8";
		}
	}
	if (strcmp(key, "video0.scene_holdoff") == 0 &&
	    cfg->video0.scene_holdoff == 0 &&
	    cfg->video0.scene_threshold > 0)
		return "video0.scene_holdoff must be >= 1 when scene_threshold > 0";
	if (strcmp(key, "video0.slice_count") == 0) {
		/* 1 = split off.  The ceiling is a cross-backend sanity bound,
		 * not necessarily the delivered count: row-based vendor APIs
		 * quantize or saturate requests to the picture geometry. */
		if (cfg->video0.slice_count < 1 ||
		    cfg->video0.slice_count > VENC_SLICE_COUNT_MAX)
			return "video0.slice_count must be 1..32";
	}
	if (strcmp(key, "video0.min_qp") == 0 || strcmp(key, "video0.max_qp") == 0) {
		/* H.264/H.265 QP range; the SDK accepts min > max without
		 * complaint and then behaves erratically (device-observed),
		 * so reject the combination here. 0 = driver default. */
		if (cfg->video0.min_qp > 51)
			return "video0.min_qp must be 0..51";
		if (cfg->video0.max_qp > 51)
			return "video0.max_qp must be 0..51";
		if (cfg->video0.min_qp > 0 && cfg->video0.max_qp > 0 &&
		    cfg->video0.min_qp > cfg->video0.max_qp)
			return "video0.min_qp must not exceed max_qp";
	}
	/* Same range as min_qp/max_qp beside it.  Without this the FT_UINT8
	 * parser accepts 0..255, persists it, and echoes it back from GET,
	 * while the config loader clamps to 51 on the next start -- so the API
	 * reports a value the encoder will never use. */
	if (strcmp(key, "video0.intra_refresh_qp") == 0) {
		if (cfg->video0.intra_refresh_qp > 51)
			return "video0.intra_refresh_qp must be 0..51";
	}
	if (strcmp(key, "snapshot.quality") == 0) {
		/* JPEG q-factor range.  Backend clamps internally too, but
		 * the validator gives a clean error response instead of a
		 * silent clamp. */
		if (cfg->snapshot.quality < 1 || cfg->snapshot.quality > 99)
			return "snapshot.quality must be in range [1, 99]";
	}
	if (strcmp(key, "outgoing.max_payload_size") == 0) {
		uint16_t v = cfg->outgoing.max_payload_size;
		/* Lower bound keeps RTP/FU header overhead a small fraction of
		 * payload; upper bound fits inside the per-slot scratch
		 * (STAR6E_OUTPUT_BATCH_SLOT_SCRATCH/MARUKO_OUTPUT_BATCH_SLOT_SCRATCH
		 * = 4096 minus 12-byte RTP header) and inside the SHM ring slot
		 * sized at startup. Above that range UDP datagrams exceed any
		 * realistic single-hop MTU and IP fragmentation defeats the
		 * point. */
		if (v < VENC_OUTPUT_PAYLOAD_MIN_BYTES ||
		    v > VENC_OUTPUT_PAYLOAD_CEILING_BYTES)
			return "outgoing.max_payload_size must be in range [576, 4000]";
	}
	if (strcmp(key, "detect.net_width") == 0 ||
	    strcmp(key, "detect.net_height") == 0) {
		/* Tap/model dims must be a multiple of 32 (>=64) — the YOLO head
		 * strides 8/16/32.  0 opts into the default (640x352).  Mirrors the
		 * check in star6e_ipu_yolo.c so a bad value is rejected before the
		 * respawn instead of just disabling detection at bring-up. */
		uint32_t v = strcmp(key, "detect.net_width") == 0 ?
			cfg->detect.net_width : cfg->detect.net_height;
		if (v != 0 && (v % 32 != 0 || v < 64))
			return "detect.netWidth/netHeight must be 0 (default) or a "
				"multiple of 32 (>=64)";
	}
	if (strcmp(key, "detect.conf_thresh") == 0) {
		float v = cfg->detect.conf_thresh;
		/* <=0 means "plugin default"; a positive value is a probability. */
		if (!isfinite(v) || v < 0.0f || v >= 1.0f)
			return "detect.confThresh must be in range [0, 1) "
				"(0 = plugin default)";
	}
	if (strcmp(key, "detect.nms_iou") == 0) {
		float v = cfg->detect.nms_iou;
		if (!isfinite(v) || v < 0.0f || v >= 1.0f)
			return "detect.nmsIou must be in range [0, 1) "
				"(0 = plugin default)";
	}
	return NULL;
}

const char *venc_api_validate_loaded_config(const VencConfig *cfg)
{
	/* Keys with rules in validate_field_cfg().  Backend-coupled checks
	 * (validate_backend_config) intentionally excluded — g_backend is
	 * registered after config load, so they cannot run here. */
	static const char *const keys[] = {
		"isp.awb_mode",
		"video0.bitrate",
		"video0.qp_delta",
		"video0.min_qp",
		"video0.max_qp",
		"video0.intra_refresh_qp",
		"video0.size",
		"video0.scene_holdoff",
		"video0.slice_count",
		"video0.frame_gate_close_slots",
		"video0.frame_gate_max_closed_ms",
		"video0.zoom_x",
		"video0.zoom_y",
		"video0.framing",
		"video0.resilience",
		"fpv.roi_qp",
		"fpv.roi_steps",
		"fpv.roi_center",
		"outgoing.max_payload_size",
		"snapshot.quality",
	};
	size_t i;

	if (!cfg)
		return "invalid config state";

	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
		const char *err = validate_field_cfg(cfg, keys[i]);
		if (err)
			return err;
	}
#if HAVE_BACKEND_CV610
	/* CV610's rules run AFTER the shared sweep, not instead of it.  This
	 * branch used to return cv610_validate_config() directly, so CV610 was
	 * the one backend that never ran validate_field_cfg() at all — it
	 * re-implemented video0.size's >=128 and multiple-of-8 gates inside
	 * cv610_mode_check_output() and silently skipped every other shared
	 * rule.  Two copies of one HEVC constraint is a drift risk; missing the
	 * rest is a parity hole, and it also meant the same bad value produced
	 * different error text on CV610 than on Star6E.  CV610 keeps its own
	 * copy of the size gates as a pipeline-boundary guard, because
	 * cv610_prepare() calls cv610_mode_check_output() directly against a
	 * config file edited behind the daemon's back. */
	return cv610_validate_config(cfg);
#else
	return NULL;
#endif
}

/* ── Config validation ───────────────────────────────────────────────── */

/* Check config consistency after a field change.  Returns NULL if valid,
 * or a static error message string if invalid.  Video codec is hardcoded
 * H.265, so the historical star6e/H.264 RTP gate is no longer required. */
static const char *validate_backend_config(const char *backend_name,
	const VencConfig *cfg)
{
	if (!cfg)
		return "invalid config state";
#if HAVE_BACKEND_CV610
	if (backend_name && strcmp(backend_name, "cv610") == 0)
		return cv610_validate_config(cfg);
#else
	(void)backend_name;
#endif
	return NULL;
}

/* ── Query string helpers ────────────────────────────────────────────── */

static int hex_nibble(unsigned char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* RFC 3986 percent-decode in place. '+' is preserved as-is (matches
 * JavaScript encodeURIComponent, which never emits '+' for spaces).
 * Returns 0 on success, -1 on malformed escape (truncated or non-hex). */
static int url_decode_inplace(char *s)
{
	char *src;
	char *dst;

	if (!s)
		return -1;

	src = s;
	dst = s;
	while (*src) {
		if (*src == '%') {
			int hi = hex_nibble((unsigned char)src[1]);
			int lo = (hi >= 0) ? hex_nibble((unsigned char)src[2]) : -1;
			if (hi < 0 || lo < 0)
				return -1;
			*dst++ = (char)((hi << 4) | lo);
			src += 3;
		} else {
			*dst++ = *src++;
		}
	}
	*dst = '\0';
	return 0;
}

/* Find the first key=value in a query string.  Writes key and value into
 * provided buffers.  Both are percent-decoded in place.  Returns 0 on
 * success, -1 if no key found or decode fails.  On percent-decode
 * failure, *error_message (if provided) is set to a static string;
 * otherwise it is left untouched. */
static int parse_first_query_param(const char *query, char *key, size_t key_sz,
	char *val, size_t val_sz, const char **error_message)
{
	if (!query || !*query) return -1;
	const char *eq = strchr(query, '=');
	const char *amp = strchr(query, '&');
	if (eq) {
		size_t klen = (size_t)(eq - query);
		if (klen >= key_sz) {
			if (error_message) *error_message = "parameter name too long";
			return -1;
		}
		memcpy(key, query, klen);
		key[klen] = '\0';
		const char *vstart = eq + 1;
		size_t vlen = amp ? (size_t)(amp - vstart) : strlen(vstart);
		/* Reject rather than truncate.  A silently shortened value can still
		 * satisfy a caller's own validation -- an array setter counting
		 * comma-separated elements sees the right count with its last element
		 * cut mid-token -- and then writes something the operator never asked
		 * for.  No legitimate request approaches this length. */
		if (vlen >= val_sz) {
			if (error_message) *error_message = "parameter value too long";
			return -1;
		}
		memcpy(val, vstart, vlen);
		val[vlen] = '\0';
	} else {
		/* key only, no value (used by GET) */
		size_t klen = amp ? (size_t)(amp - query) : strlen(query);
		if (klen >= key_sz) {
			if (error_message) *error_message = "parameter name too long";
			return -1;
		}
		memcpy(key, query, klen);
		key[klen] = '\0';
		val[0] = '\0';
	}
	if (url_decode_inplace(key) != 0 || url_decode_inplace(val) != 0) {
		if (error_message)
			*error_message = "malformed percent-escape in query";
		return -1;
	}
	return 0;
}

#define SET_QUERY_MAX_PARAMS 16

typedef struct {
	char key[128];
	char canonical_key[128];
	char value[256];
	const FieldDesc *field;
} SetQueryParam;

typedef enum {
	LIVE_GROUP_INVALID = -1,
	LIVE_GROUP_BITRATE = 0,
	LIVE_GROUP_VIDEO_TIMING,
	LIVE_GROUP_QP_DELTA,
	LIVE_GROUP_ROI,
	LIVE_GROUP_GAIN_MAX,
	LIVE_GROUP_SHUTTER_MAX,
	LIVE_GROUP_GAIN_MIN,
	LIVE_GROUP_SHUTTER_MIN,
	LIVE_GROUP_AWB,
	LIVE_GROUP_VERBOSE,
	LIVE_GROUP_OUTGOING,
	LIVE_GROUP_MAX_PAYLOAD,
	LIVE_GROUP_MUTE,
	LIVE_GROUP_ZOOM,
	LIVE_GROUP_ISP_BIN,
	LIVE_GROUP_SNAPSHOT_QUALITY,
	LIVE_GROUP_PAUSE_STAB,
	LIVE_GROUP_QP_BOUNDS,
	LIVE_GROUP_DETECT,
	LIVE_GROUP_QR_WINDOW,
	LIVE_GROUP_COUNT
} LiveApplyGroup;

typedef struct {
	int video_fps;
	int video_gop;
	int awb_mode;
	int awb_ct;
	int awb_fps;
	int outgoing_enabled;
	int outgoing_server;
	int isp_sensor_bin;
} LiveBatchTouched;

static int make_error_json(const char *code, const char *message, char **out_json)
{
	char buf[1024];
	int len;

	if (!out_json)
		return -1;

	len = snprintf(buf, sizeof(buf),
		"{\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
		code ? code : "internal_error",
		message ? message : "unknown error");
	if (len >= (int)sizeof(buf))
		len = (int)sizeof(buf) - 1;

	*out_json = strdup(buf);
	return *out_json ? 0 : -1;
}

static int make_handled_error_json(int status, const char *code,
	const char *message, int *status_code, char **response_json)
{
	if (status_code)
		*status_code = status;
	if (make_error_json(code, message, response_json) != 0)
		return -1;
	return 1;
}

static int make_single_set_success_json(const char *field_key,
	const char *json_value, int reinit_pending, char **out_json)
{
	char buf[512];
	int len;

	if (!field_key || !json_value || !out_json)
		return -1;

	if (reinit_pending) {
		len = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{\"field\":\"%s\",\"value\":%s,"
			"\"reinit_pending\":true}}",
			field_key, json_value);
	} else {
		len = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{\"field\":\"%s\",\"value\":%s}}",
			field_key, json_value);
	}
	if (len >= (int)sizeof(buf))
		len = (int)sizeof(buf) - 1;

	*out_json = strdup(buf);
	return *out_json ? 0 : -1;
}

static int make_multi_live_set_success_json(const SetQueryParam *params,
	size_t count, int reinit_requested, char **out_json)
{
	cJSON *root;
	cJSON *data;
	cJSON *applied;
	size_t i;
	char *str;

	if (!params || count == 0 || !out_json)
		return -1;

	root = cJSON_CreateObject();
	if (!root)
		return -1;

	cJSON_AddBoolToObject(root, "ok", 1);
	data = cJSON_AddObjectToObject(root, "data");
	/* Same contract as the single-field response: a batch whose apply asked
	 * for a respawn has to say so, or a caller batching outgoing.server with
	 * anything else is told the whole set went live while the craft is about
	 * to restart under it. */
	if (reinit_requested)
		cJSON_AddBoolToObject(data, "reinit_pending", 1);
	applied = cJSON_AddArrayToObject(data, "applied");

	for (i = 0; i < count; i++) {
		cJSON *entry;
		cJSON *value_item;
		char *json_value;

		entry = cJSON_CreateObject();
		if (!entry) {
			cJSON_Delete(root);
			return -1;
		}
		cJSON_AddStringToObject(entry, "field", params[i].key);

		json_value = field_to_json_value(params[i].field);
		if (!json_value) {
			cJSON_Delete(entry);
			cJSON_Delete(root);
			return -1;
		}

		value_item = cJSON_Parse(json_value);
		if (!value_item) {
			free(json_value);
			cJSON_Delete(entry);
			cJSON_Delete(root);
			return -1;
		}
		free(json_value);

		cJSON_AddItemToObject(entry, "value", value_item);
		cJSON_AddItemToArray(applied, entry);
	}

	str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!str)
		return -1;

	*out_json = str;
	return 0;
}

static LiveApplyGroup live_group_for_key(const char *canonical_key)
{
	if (!canonical_key)
		return LIVE_GROUP_INVALID;

	if (strcmp(canonical_key, "video0.bitrate") == 0)
		return LIVE_GROUP_BITRATE;
	if (strcmp(canonical_key, "video0.fps") == 0 ||
	    strcmp(canonical_key, "video0.gop_size") == 0)
		return LIVE_GROUP_VIDEO_TIMING;
	if (strcmp(canonical_key, "video0.qp_delta") == 0)
		return LIVE_GROUP_QP_DELTA;
	if (strcmp(canonical_key, "fpv.roi_enabled") == 0 ||
	    strcmp(canonical_key, "fpv.roi_qp") == 0 ||
	    strcmp(canonical_key, "fpv.roi_steps") == 0 ||
	    strcmp(canonical_key, "fpv.roi_center") == 0)
		return LIVE_GROUP_ROI;
	if (strcmp(canonical_key, "isp.gain_max") == 0)
		return LIVE_GROUP_GAIN_MAX;
	if (strcmp(canonical_key, "isp.shutter_max_us") == 0)
		return LIVE_GROUP_SHUTTER_MAX;
	if (strcmp(canonical_key, "isp.gain_min") == 0)
		return LIVE_GROUP_GAIN_MIN;
	if (strcmp(canonical_key, "isp.shutter_min_us") == 0)
		return LIVE_GROUP_SHUTTER_MIN;
	if (strcmp(canonical_key, "isp.awb_mode") == 0 ||
	    strcmp(canonical_key, "isp.awb_ct") == 0 ||
	    strcmp(canonical_key, "isp.awb_fps") == 0)
		return LIVE_GROUP_AWB;
	if (strcmp(canonical_key, "system.verbose") == 0)
		return LIVE_GROUP_VERBOSE;
	if (strcmp(canonical_key, "outgoing.enabled") == 0 ||
	    strcmp(canonical_key, "outgoing.server") == 0)
		return LIVE_GROUP_OUTGOING;
	if (strcmp(canonical_key, "outgoing.max_payload_size") == 0)
		return LIVE_GROUP_MAX_PAYLOAD;
	if (strcmp(canonical_key, "audio.mute") == 0)
		return LIVE_GROUP_MUTE;
	if (strcmp(canonical_key, "video0.zoom_x") == 0 ||
	    strcmp(canonical_key, "video0.zoom_y") == 0)
		return LIVE_GROUP_ZOOM;
	if (strcmp(canonical_key, "isp.sensor_bin") == 0)
		return LIVE_GROUP_ISP_BIN;
	if (strcmp(canonical_key, "snapshot.quality") == 0)
		return LIVE_GROUP_SNAPSHOT_QUALITY;
	if (strcmp(canonical_key, "video0.pause_stab") == 0)
		return LIVE_GROUP_PAUSE_STAB;
	if (strcmp(canonical_key, "video0.min_qp") == 0 ||
	    strcmp(canonical_key, "video0.max_qp") == 0)
		return LIVE_GROUP_QP_BOUNDS;
	if (strcmp(canonical_key, "detect.enabled") == 0 ||
	    strcmp(canonical_key, "detect.model_path") == 0 ||
	    strcmp(canonical_key, "detect.model_id") == 0 ||
	    strcmp(canonical_key, "detect.conf_thresh") == 0 ||
	    strcmp(canonical_key, "detect.nms_iou") == 0)
		return LIVE_GROUP_DETECT;
	if (strcmp(canonical_key, "qr.window_ms") == 0)
		return LIVE_GROUP_QR_WINDOW;

	return LIVE_GROUP_INVALID;
}

static const char *live_group_name(LiveApplyGroup group)
{
	switch (group) {
	case LIVE_GROUP_BITRATE:
		return "video0.bitrate";
	case LIVE_GROUP_VIDEO_TIMING:
		return "video0.fps/video0.gop_size";
	case LIVE_GROUP_QP_DELTA:
		return "video0.qp_delta";
	case LIVE_GROUP_ROI:
		return "fpv.roi_*";
	case LIVE_GROUP_GAIN_MAX:
		return "isp.gain_max";
	case LIVE_GROUP_SHUTTER_MAX:
		return "isp.shutter_max_us";
	case LIVE_GROUP_GAIN_MIN:
		return "isp.gain_min";
	case LIVE_GROUP_SHUTTER_MIN:
		return "isp.shutter_min_us";
	case LIVE_GROUP_AWB:
		return "isp.awb_*";
	case LIVE_GROUP_VERBOSE:
		return "system.verbose";
	case LIVE_GROUP_OUTGOING:
		return "outgoing.*";
	case LIVE_GROUP_MAX_PAYLOAD:
		return "outgoing.max_payload_size";
	case LIVE_GROUP_MUTE:
		return "audio.mute";
	case LIVE_GROUP_ZOOM:
		return "video0.zoom_*";
	case LIVE_GROUP_ISP_BIN:
		return "isp.sensor_bin";
	case LIVE_GROUP_SNAPSHOT_QUALITY:
		return "snapshot.quality";
	case LIVE_GROUP_PAUSE_STAB:
		return "video0.pauseStab";
	case LIVE_GROUP_QP_BOUNDS:
		return "video0.minQp/maxQp";
	case LIVE_GROUP_DETECT:
		return "detect.enabled/model_path/model_id/conf_thresh/nms_iou";
	case LIVE_GROUP_QR_WINDOW:
		return "qr.windowMs";
	default:
		return "unknown";
	}
}

static void note_live_group_touch(LiveBatchTouched *touched,
	const char *canonical_key)
{
	if (!touched || !canonical_key)
		return;

	if (strcmp(canonical_key, "video0.fps") == 0)
		touched->video_fps = 1;
	else if (strcmp(canonical_key, "video0.gop_size") == 0)
		touched->video_gop = 1;
	else if (strcmp(canonical_key, "isp.awb_mode") == 0)
		touched->awb_mode = 1;
	else if (strcmp(canonical_key, "isp.awb_ct") == 0)
		touched->awb_ct = 1;
	else if (strcmp(canonical_key, "isp.awb_fps") == 0)
		touched->awb_fps = 1;
	else if (strcmp(canonical_key, "outgoing.enabled") == 0)
		touched->outgoing_enabled = 1;
	else if (strcmp(canonical_key, "outgoing.server") == 0)
		touched->outgoing_server = 1;
	else if (strcmp(canonical_key, "isp.sensor_bin") == 0)
		touched->isp_sensor_bin = 1;
}

static int parse_query_params(const char *query, SetQueryParam *params,
	size_t max_params, size_t *out_count, const char **error_message)
{
	const char *cursor;
	size_t count = 0;

	if (out_count)
		*out_count = 0;
	if (error_message)
		*error_message = NULL;

	if (!query || !*query) {
		if (error_message)
			*error_message = "missing query parameter key=value";
		return -1;
	}

	cursor = query;
	while (*cursor) {
		const char *segment_end = strchr(cursor, '&');
		const char *eq;
		size_t key_len;
		size_t value_len;
		const char *canonical_key;

		if (!segment_end)
			segment_end = cursor + strlen(cursor);
		if (segment_end == cursor) {
			if (error_message)
				*error_message = "empty query parameter";
			return -1;
		}
		if (count >= max_params) {
			if (error_message)
				*error_message = "too many query parameters";
			return -1;
		}

		eq = memchr(cursor, '=', (size_t)(segment_end - cursor));
		if (!eq || eq == cursor) {
			if (error_message)
				*error_message = "missing query parameter key=value";
			return -1;
		}

		key_len = (size_t)(eq - cursor);
		if (key_len >= sizeof(params[count].key))
			key_len = sizeof(params[count].key) - 1;
		memcpy(params[count].key, cursor, key_len);
		params[count].key[key_len] = '\0';

		value_len = (size_t)(segment_end - (eq + 1));
		if (value_len >= sizeof(params[count].value))
			value_len = sizeof(params[count].value) - 1;
		memcpy(params[count].value, eq + 1, value_len);
		params[count].value[value_len] = '\0';

		if (url_decode_inplace(params[count].key) != 0 ||
		    url_decode_inplace(params[count].value) != 0) {
			if (error_message)
				*error_message = "malformed percent-escape in query";
			return -1;
		}

		canonical_key = canonicalize_field_key(params[count].key);
		if (!canonical_key)
			canonical_key = params[count].key;
		snprintf(params[count].canonical_key,
			sizeof(params[count].canonical_key), "%s", canonical_key);
		params[count].field = NULL;
		count++;

		cursor = *segment_end ? segment_end + 1 : segment_end;
	}

	if (out_count)
		*out_count = count;
	return 0;
}

static int live_group_supported_for_cfg(const VencConfig *cfg,
	LiveApplyGroup group, const LiveBatchTouched *touched)
{
	if (!g_cb)
		return 0;

	switch (group) {
	case LIVE_GROUP_BITRATE:
		return g_cb->apply_bitrate != NULL;
	case LIVE_GROUP_VIDEO_TIMING:
		if (touched && touched->video_fps && !g_cb->apply_fps)
			return 0;
		if (cfg && !(cfg->video0.scene_threshold > 0) &&
		    touched && (touched->video_fps || touched->video_gop) &&
		    !g_cb->apply_gop)
			return 0;
		if (cfg && cfg->video0.scene_threshold > 0 &&
		    touched && touched->video_gop)
			return 0;
		return 1;
	case LIVE_GROUP_QP_DELTA:
		return g_cb->apply_qp_delta != NULL;
	case LIVE_GROUP_ROI:
		return g_cb->apply_roi_qp != NULL;
	case LIVE_GROUP_GAIN_MAX:
		return g_cb->apply_gain_max != NULL;
	case LIVE_GROUP_SHUTTER_MAX:
		return g_cb->apply_shutter_max != NULL;
	case LIVE_GROUP_GAIN_MIN:
		return g_cb->apply_gain_min != NULL;
	case LIVE_GROUP_SHUTTER_MIN:
		return g_cb->apply_shutter_min != NULL;
	case LIVE_GROUP_AWB:
		/* Backstop, not the primary gate: awbFps is advertised
		 * unsupported on backends without the loop (see
		 * venc_api_field_supported_for_backend), so a write is normally
		 * rejected before it reaches here.  This keeps the apply below
		 * from calling through a NULL pointer if a future backend is
		 * added without its gate entry. */
		if (touched && touched->awb_fps && !g_cb->apply_awb_rate)
			return 0;
		if (touched && !touched->awb_mode && !touched->awb_ct)
			return 1;
		return g_cb->apply_awb_mode != NULL;
	case LIVE_GROUP_VERBOSE:
		return g_cb->apply_verbose != NULL;
	case LIVE_GROUP_OUTGOING:
		if (touched && touched->outgoing_server && !g_cb->apply_server)
			return 0;
		if (touched && touched->outgoing_enabled &&
		    !g_cb->apply_output_enabled)
			return 0;
		return 1;
	case LIVE_GROUP_MAX_PAYLOAD:
		return g_cb->apply_max_payload_size != NULL;
	case LIVE_GROUP_MUTE:
		return g_cb->apply_mute != NULL;
	case LIVE_GROUP_ZOOM:
		return g_cb->apply_zoom != NULL;
	case LIVE_GROUP_ISP_BIN:
		return g_cb->apply_isp_bin != NULL;
	case LIVE_GROUP_SNAPSHOT_QUALITY:
		return g_cb->apply_snapshot_quality != NULL;
	case LIVE_GROUP_PAUSE_STAB:
		return g_cb->apply_pause_stab != NULL;
	case LIVE_GROUP_QP_BOUNDS:
		return g_cb->apply_qp_bounds != NULL;
	case LIVE_GROUP_DETECT:
		return g_cb->apply_detect_reload != NULL;
	case LIVE_GROUP_QR_WINDOW:
		/* No callback: a new scan snapshots qr.window_ms when it opens. */
		return 1;
	default:
		return 0;
	}
}

static void copy_live_group_fields(VencConfig *dst, const VencConfig *src,
	LiveApplyGroup group, const LiveBatchTouched *touched)
{
	if (!dst || !src)
		return;

	switch (group) {
	case LIVE_GROUP_BITRATE:
		dst->video0.bitrate = src->video0.bitrate;
		break;
	case LIVE_GROUP_VIDEO_TIMING:
		if (touched && touched->video_fps)
			dst->video0.fps = src->video0.fps;
		if (touched && touched->video_gop)
			dst->video0.gop_size = src->video0.gop_size;
		break;
	case LIVE_GROUP_QP_DELTA:
		dst->video0.qp_delta = src->video0.qp_delta;
		break;
	case LIVE_GROUP_ROI:
		dst->fpv.roi_enabled = src->fpv.roi_enabled;
		dst->fpv.roi_qp = src->fpv.roi_qp;
		dst->fpv.roi_steps = src->fpv.roi_steps;
		dst->fpv.roi_center = src->fpv.roi_center;
		break;
	case LIVE_GROUP_GAIN_MAX:
		dst->isp.gain_max = src->isp.gain_max;
		break;
	case LIVE_GROUP_SHUTTER_MAX:
		dst->isp.shutter_max_us = src->isp.shutter_max_us;
		break;
	case LIVE_GROUP_GAIN_MIN:
		dst->isp.gain_min = src->isp.gain_min;
		break;
	case LIVE_GROUP_SHUTTER_MIN:
		dst->isp.shutter_min_us = src->isp.shutter_min_us;
		break;
	case LIVE_GROUP_AWB:
		if (touched && touched->awb_mode) {
			snprintf(dst->isp.awb_mode, sizeof(dst->isp.awb_mode), "%s",
				src->isp.awb_mode);
		}
		if (touched && touched->awb_ct)
			dst->isp.awb_ct = src->isp.awb_ct;
		if (touched && touched->awb_fps)
			dst->isp.awb_fps = src->isp.awb_fps;
		break;
	case LIVE_GROUP_VERBOSE:
		dst->system.verbose = src->system.verbose;
		break;
	case LIVE_GROUP_OUTGOING:
		if (touched && touched->outgoing_enabled)
			dst->outgoing.enabled = src->outgoing.enabled;
		if (touched && touched->outgoing_server) {
			snprintf(dst->outgoing.server, sizeof(dst->outgoing.server), "%s",
				src->outgoing.server);
		}
		break;
	case LIVE_GROUP_MAX_PAYLOAD:
		dst->outgoing.max_payload_size = src->outgoing.max_payload_size;
		break;
	case LIVE_GROUP_MUTE:
		dst->audio.mute = src->audio.mute;
		break;
	case LIVE_GROUP_ZOOM:
		dst->video0.zoom_pct = src->video0.zoom_pct;
		dst->video0.zoom_x   = src->video0.zoom_x;
		dst->video0.zoom_y   = src->video0.zoom_y;
		break;
	case LIVE_GROUP_ISP_BIN:
		if (touched && touched->isp_sensor_bin) {
			snprintf(dst->isp.sensor_bin, sizeof(dst->isp.sensor_bin),
				"%s", src->isp.sensor_bin);
		}
		break;
	case LIVE_GROUP_SNAPSHOT_QUALITY:
		dst->snapshot.quality = src->snapshot.quality;
		break;
	case LIVE_GROUP_PAUSE_STAB:
		dst->video0.pause_stab = src->video0.pause_stab;
		break;
	case LIVE_GROUP_QP_BOUNDS:
		dst->video0.min_qp = src->video0.min_qp;
		dst->video0.max_qp = src->video0.max_qp;
		break;
	case LIVE_GROUP_DETECT:
		/* Copy the whole live detector group; the backend re-reads all of
		 * them from the committed config on reload, and unchanged members
		 * equal the base anyway (new_cfg starts as a copy of old_cfg).
		 * `enabled` MUST be here — the apply callback decides start vs stop
		 * vs reload from the committed value, so omitting it silently turns
		 * a toggle into a same-state reload. */
		dst->detect.enabled     = src->detect.enabled;
		snprintf(dst->detect.model_path, sizeof(dst->detect.model_path),
			"%s", src->detect.model_path);
		dst->detect.model_id    = src->detect.model_id;
		dst->detect.conf_thresh = src->detect.conf_thresh;
		dst->detect.nms_iou     = src->detect.nms_iou;
		break;
	case LIVE_GROUP_QR_WINDOW:
		dst->qr.window_ms = src->qr.window_ms;
		break;
	default:
		break;
	}
}

static void build_live_group_config(VencConfig *out, const VencConfig *base,
	const VencConfig *updates, LiveApplyGroup group,
	const LiveBatchTouched *touched)
{
	if (!out || !base || !updates)
		return;

	*out = *base;
	copy_live_group_fields(out, updates, group, touched);
}

static int commit_config_locked(const VencConfig *cfg)
{
	if (!g_cfg || !cfg)
		return -1;

	*g_cfg = *cfg;
	return 0;
}

static int apply_live_group_for_cfg(const VencConfig *cfg,
	LiveApplyGroup group, const LiveBatchTouched *touched)
{
	int rc;
	int mode;
	uint32_t gop_frames;

	if (!cfg || commit_config_locked(cfg) != 0)
		return -1;
	if (!live_group_supported_for_cfg(cfg, group, touched))
		return -2;

	switch (group) {
	case LIVE_GROUP_BITRATE:
		return g_cb->apply_bitrate(cfg->video0.bitrate);
	case LIVE_GROUP_VIDEO_TIMING:
		if (touched && touched->video_fps) {
			rc = g_cb->apply_fps(cfg->video0.fps);
			if (rc != 0)
				return -1;
		}
		if (!(cfg->video0.scene_threshold > 0) &&
		    touched && (touched->video_fps || touched->video_gop)) {
			/* Base the GOP frame count on the fps the encoder is
			 * actually running at, not the committed request: a live
			 * fps above the current sensor mode's max is clamped to
			 * sensor_fps for the bind (see PIPELINE_LIVE_FPS_MAX), so
			 * using the unclamped request here would stretch the
			 * I-frame interval (GOP frames for 144 while the encoder
			 * is pinned at 100 -> 1.44s instead of the intended 1s). */
			uint32_t gop_fps = cfg->video0.fps;
			if (g_cb->query_live_fps) {
				uint32_t live_fps = g_cb->query_live_fps();
				if (live_fps > 0)
					gop_fps = live_fps;
			}
			gop_frames = pipeline_common_gop_frames(
				cfg->video0.gop_size, gop_fps);
			rc = g_cb->apply_gop(gop_frames);
			if (rc != 0)
				return -1;
		}
		return 0;
	case LIVE_GROUP_QP_DELTA:
		return g_cb->apply_qp_delta(cfg->video0.qp_delta);
	case LIVE_GROUP_ROI:
		return g_cb->apply_roi_qp(cfg->fpv.roi_qp);
	case LIVE_GROUP_GAIN_MAX:
		return g_cb->apply_gain_max(cfg->isp.gain_max);
	case LIVE_GROUP_SHUTTER_MAX:
		return g_cb->apply_shutter_max(cfg->isp.shutter_max_us);
	case LIVE_GROUP_GAIN_MIN:
		return g_cb->apply_gain_min(cfg->isp.gain_min);
	case LIVE_GROUP_SHUTTER_MIN:
		return g_cb->apply_shutter_min(cfg->isp.shutter_min_us);
	case LIVE_GROUP_AWB:
		/* Rate first: it starts/stops the userspace loop, and the mode
		 * apply below decides ownership from the committed awbFps. */
		if (touched && touched->awb_fps) {
			rc = g_cb->apply_awb_rate(cfg->isp.awb_fps);
			if (rc != 0)
				return -1;
			if (!touched->awb_mode && !touched->awb_ct)
				return 0;
		}
		mode = strcmp(cfg->isp.awb_mode, "ct_manual") == 0 ? 1 : 0;
		return g_cb->apply_awb_mode(mode, cfg->isp.awb_ct);
	case LIVE_GROUP_VERBOSE:
		return g_cb->apply_verbose(cfg->system.verbose);
	case LIVE_GROUP_OUTGOING:
		if (touched && touched->outgoing_enabled && touched->outgoing_server) {
			if (cfg->outgoing.enabled) {
				rc = g_cb->apply_server(cfg->outgoing.server);
				if (rc != 0)
					return -1;
				rc = g_cb->apply_output_enabled(cfg->outgoing.enabled);
				if (rc != 0)
					return -1;
				return 0;
			}

			rc = g_cb->apply_output_enabled(cfg->outgoing.enabled);
			if (rc != 0)
				return -1;
			rc = g_cb->apply_server(cfg->outgoing.server);
			if (rc != 0)
				return -1;
			return 0;
		}
		if (touched && touched->outgoing_server)
			return g_cb->apply_server(cfg->outgoing.server);
		if (touched && touched->outgoing_enabled)
			return g_cb->apply_output_enabled(cfg->outgoing.enabled);
		return 0;
	case LIVE_GROUP_MAX_PAYLOAD:
		return g_cb->apply_max_payload_size(cfg->outgoing.max_payload_size);
	case LIVE_GROUP_MUTE:
		return g_cb->apply_mute(cfg->audio.mute);
	case LIVE_GROUP_ZOOM:
		return g_cb->apply_zoom(cfg->video0.zoom_pct,
			cfg->video0.zoom_x, cfg->video0.zoom_y);
	case LIVE_GROUP_ISP_BIN:
		return g_cb->apply_isp_bin(cfg->isp.sensor_bin);
	case LIVE_GROUP_SNAPSHOT_QUALITY:
		return g_cb->apply_snapshot_quality(cfg->snapshot.quality);
	case LIVE_GROUP_PAUSE_STAB:
		return g_cb->apply_pause_stab(cfg->video0.pause_stab);
	case LIVE_GROUP_QP_BOUNDS:
		/* Backends without RC QP bounds leave the hook NULL. */
		if (!g_cb->apply_qp_bounds)
			return -2;
		return g_cb->apply_qp_bounds(cfg->video0.min_qp,
			cfg->video0.max_qp);
	case LIVE_GROUP_DETECT:
		/* cfg is already committed to g_cfg (commit_config_locked above),
		 * so the backend reads the new model_path/model_id/conf/iou from the
		 * live config when it performs the swap on the pipeline thread. */
		return g_cb->apply_detect_reload();
	case LIVE_GROUP_QR_WINDOW:
		/* Committed above; the next scan snapshots the new duration. */
		return 0;
	default:
		return -2;
	}
}

static int rollback_live_groups(const LiveApplyGroup *groups,
	size_t applied_count, LiveApplyGroup current_group,
	const LiveBatchTouched *touched, const VencConfig *old_cfg,
	VencConfig *actual_cfg)
{
	VencConfig rollback_cfg;
	int rollback_incomplete = 0;
	size_t i;

	if (!old_cfg || !actual_cfg)
		return 1;

	if (current_group != LIVE_GROUP_INVALID) {
		build_live_group_config(&rollback_cfg, actual_cfg, old_cfg,
			current_group, touched);
		if (apply_live_group_for_cfg(&rollback_cfg, current_group,
		    touched) == 0) {
			*actual_cfg = rollback_cfg;
		} else {
			fprintf(stderr,
				"[venc_api] live batch rollback failed for %s\n",
				live_group_name(current_group));
			rollback_incomplete = 1;
			commit_config_locked(actual_cfg);
		}
	}

	for (i = applied_count; i > 0; i--) {
		build_live_group_config(&rollback_cfg, actual_cfg, old_cfg,
			groups[i - 1], touched);
		if (apply_live_group_for_cfg(&rollback_cfg, groups[i - 1],
		    touched) == 0) {
			*actual_cfg = rollback_cfg;
		} else {
			fprintf(stderr,
				"[venc_api] live batch rollback failed for %s\n",
				live_group_name(groups[i - 1]));
			rollback_incomplete = 1;
			commit_config_locked(actual_cfg);
		}
	}

	return rollback_incomplete;
}

static int collect_live_groups(SetQueryParam *params, size_t param_count,
	LiveApplyGroup *group_order, size_t *group_count,
	LiveBatchTouched *touched, int *status_code, char **response_json)
{
	int group_seen[LIVE_GROUP_COUNT] = {0};
	size_t i;

	if (!params || !group_order || !group_count || !touched ||
	    !status_code || !response_json)
		return -1;

	memset(touched, 0, sizeof(*touched));
	*group_count = 0;

	for (i = 0; i < param_count; i++) {
		size_t j;
		LiveApplyGroup group;

		if (!params[i].field)
			params[i].field = find_field(params[i].canonical_key);
		if (!params[i].field) {
			return make_handled_error_json(404, "not_found",
				"unknown config field", status_code,
				response_json);
		}
		if (!venc_api_field_supported_for_backend(g_backend,
		    params[i].canonical_key)) {
			return make_handled_error_json(501, "not_implemented",
				"field not supported on this backend",
				status_code, response_json);
		}
		if (field_mut_for_backend(params[i].field) != MUT_LIVE) {
			return make_handled_error_json(400, "invalid_request",
				"multi-set only supports live fields; restart-required fields must be set one at a time",
				status_code, response_json);
		}

		for (j = 0; j < i; j++) {
			if (strcmp(params[i].canonical_key,
			    params[j].canonical_key) == 0) {
				return make_handled_error_json(400,
					"invalid_request",
					"duplicate field in multi-set request",
					status_code, response_json);
			}
		}

		group = live_group_for_key(params[i].canonical_key);
		if (group == LIVE_GROUP_INVALID) {
			return make_handled_error_json(400, "invalid_request",
				"field does not support multi-set batching",
				status_code, response_json);
		}
		note_live_group_touch(touched, params[i].canonical_key);
		if (!group_seen[group]) {
			group_seen[group] = 1;
			group_order[*group_count] = group;
			(*group_count)++;
		}
	}

	return 0;
}

static int stage_params_into_cfg(VencConfig *cfg, const SetQueryParam *params,
	size_t param_count, int *status_code, char **response_json)
{
	size_t i;

	if (!cfg || !params || !status_code || !response_json)
		return -1;

	for (i = 0; i < param_count; i++) {
		const char *field_err;

		if (field_from_string_cfg(cfg, params[i].field, params[i].value) != 0) {
			*status_code = 400;
			return make_error_json("validation_failed",
				"invalid value for field", response_json) == 0 ? 1 : -1;
		}

		field_err = validate_field_cfg(cfg, params[i].canonical_key);
		if (field_err) {
			*status_code = 409;
			return make_error_json("validation_failed", field_err,
				response_json) == 0 ? 1 : -1;
		}
	}

	{
		const char *err = validate_backend_config(g_backend, cfg);
		if (err) {
			*status_code = 409;
			return make_error_json("validation_failed", err,
				response_json) == 0 ? 1 : -1;
		}
	}

	return 0;
}

static int preflight_live_group_callbacks(const VencConfig *cfg,
	const LiveApplyGroup *group_order, size_t group_count,
	const LiveBatchTouched *touched, size_t param_count,
	int *status_code, char **response_json)
{
	size_t i;

	if (!cfg || !group_order || !status_code || !response_json)
		return -1;

	for (i = 0; i < group_count; i++) {
		if (!live_group_supported_for_cfg(cfg, group_order[i], touched)) {
			*status_code = 501;
			return make_error_json("not_implemented",
				param_count == 1 ? "apply callback not available" :
				"apply callback not available for one or more live fields",
				response_json) == 0 ? 1 : -1;
		}
	}

	return 0;
}

static int apply_live_group_sequence_locked(const LiveApplyGroup *group_order,
	size_t group_count, const LiveBatchTouched *touched,
	const VencConfig *old_cfg, const VencConfig *new_cfg,
	VencConfig *actual_cfg, int *status_code, char **response_json)
{
	size_t i;

	if (!group_order || !old_cfg || !new_cfg || !actual_cfg ||
	    !status_code || !response_json)
		return -1;

	for (i = 0; i < group_count; i++) {
		VencConfig group_cfg;
		int rollback_incomplete;
		char message[192];

		build_live_group_config(&group_cfg, actual_cfg, new_cfg,
			group_order[i], touched);
		if (apply_live_group_for_cfg(&group_cfg, group_order[i],
		    touched) == 0) {
			*actual_cfg = group_cfg;
			continue;
		}

		commit_config_locked(actual_cfg);
		rollback_incomplete = rollback_live_groups(group_order, i,
			group_order[i], touched, old_cfg, actual_cfg);
		commit_config_locked(actual_cfg);

		snprintf(message, sizeof(message),
			rollback_incomplete ?
			"failed to apply live field group %s; rollback incomplete" :
			"failed to apply live field group %s",
			live_group_name(group_order[i]));
		*status_code = 500;
		return make_error_json("internal_error", message,
			response_json) == 0 ? 1 : -1;
	}

	return 0;
}

static int make_live_set_response_locked(const VencConfig *cfg,
	const SetQueryParam *params, size_t param_count, int single_response,
	int reinit_requested, int *status_code, char **response_json)
{
	if (!cfg || !params || param_count == 0 || !status_code || !response_json)
		return -1;

	if (single_response) {
		char *jval;
		int rc;

		jval = field_to_json_value_from_cfg(cfg, params[0].field);
		if (!jval) {
			*status_code = 500;
			return make_error_json("internal_error", "out of memory",
				response_json);
		}

		/* Report a respawn the apply itself asked for.  A backend can
		 * accept a live-class field and still need a restart to honour
		 * it -- CV610's apply_server() does exactly that for the ring
		 * transports, committing the value and calling
		 * venc_api_request_reinit() rather than refusing.  Answering a
		 * flat "ok" there would tell the operator the change was live
		 * when the craft is about to respawn under them. */
		rc = make_single_set_success_json(params[0].key, jval,
			reinit_requested, response_json);
		free(jval);
		if (rc != 0)
			return -1;
	} else {
		if (make_multi_live_set_success_json(params, param_count,
		    reinit_requested, response_json) != 0) {
			return -1;
		}
	}

	*status_code = 200;
	return 0;
}

static int apply_live_set_query(SetQueryParam *params, size_t param_count,
	int single_response, int persist, int *status_code,
	char **response_json)
{
	LiveApplyGroup group_order[LIVE_GROUP_COUNT];
	LiveBatchTouched touched;
	size_t group_count = 0;
	VencConfig old_cfg;
	VencConfig new_cfg;
	VencConfig actual_cfg;
	int rc;
	/* g_reinit is a LATCH the runtime consumes on its own schedule, so it
	 * can already be set from an earlier request when this one arrives.
	 * Only a false->true transition across THIS apply means this write is
	 * the one that needs a respawn -- reading the flag absolutely made an
	 * unrelated live set report reinit_pending. */
	unsigned reinit_seq_before = venc_api_reinit_seq();

	rc = collect_live_groups(params, param_count, group_order, &group_count,
		&touched, status_code, response_json);
	if (rc != 0)
		return rc > 0 ? 0 : rc;

	/* Snapshot g_cfg under the mutex, then drop it for the
	 * validation/preflight pass.  field_from_string_cfg(),
	 * validate_field_cfg(), validate_backend_config(), and
	 * live_group_supported_for_cfg() all operate on the local copy and
	 * only read write-once globals (g_backend, g_cb function table) —
	 * no shared mutable state, so they're safe outside the mutex.
	 *
	 * Safety of the unlock/relock split: the httpd is single-threaded
	 * (one accept loop, one in-flight handler), so no other writer can
	 * mutate g_cfg in this window.  Any future move to a multi-threaded
	 * httpd would need to revisit this. */
	pthread_mutex_lock(&g_cfg_mutex);
	old_cfg = *g_cfg;
	pthread_mutex_unlock(&g_cfg_mutex);

	new_cfg = old_cfg;
	actual_cfg = old_cfg;

	rc = stage_params_into_cfg(&new_cfg, params, param_count, status_code,
		response_json);
	if (rc != 0)
		return rc > 0 ? 0 : rc;

	rc = preflight_live_group_callbacks(&new_cfg, group_order, group_count,
		&touched, param_count, status_code, response_json);
	if (rc != 0)
		return rc > 0 ? 0 : rc;

	pthread_mutex_lock(&g_cfg_mutex);

	rc = apply_live_group_sequence_locked(group_order, group_count, &touched,
		&old_cfg, &new_cfg, &actual_cfg, status_code, response_json);
	if (rc != 0) {
		pthread_mutex_unlock(&g_cfg_mutex);
		return rc > 0 ? 0 : rc;
	}

	if (commit_config_locked(&actual_cfg) != 0) {
		pthread_mutex_unlock(&g_cfg_mutex);
		return -1;
	}

	rc = make_live_set_response_locked(&actual_cfg, params, param_count,
		single_response,
		(venc_api_reinit_seq() != reinit_seq_before) ? 1 : 0,
		status_code, response_json);
	if (rc != 0) {
		pthread_mutex_unlock(&g_cfg_mutex);
		return rc;
	}

	pthread_mutex_unlock(&g_cfg_mutex);
	/* Persist LIVE changes too.  Matches user expectation that a
	 * /api/v1/set round-trip (WebUI slider, curl, etc.) survives restart.
	 * Done after the mutex is released to avoid holding it across fsync.
	 * The helper already logs failures to stderr and caches the
	 * last-saved snapshot, so repeated identical sets skip the flash
	 * write entirely.
	 *
	 * /api/v1/live/set passes persist=0: the change applies to the
	 * running config only — no flash write.  Built for high-cadence
	 * automated writers (waybeam-link adaptive actuation); a later
	 * persisting /set snapshots the whole running config, volatile
	 * changes included (one config struct, by design). */
	/* `persist || reinit` is the condition, not `persist`.
	 *
	 * /api/v1/live/set passes persist=0 by design, and that is safe for a
	 * change that took effect in the running process.  It is NOT safe once a
	 * backend's apply has asked for a respawn: the respawn re-execs and
	 * reloads /etc/waybeam.json, so an unpersisted value is silently
	 * discarded by a restart the caller did not ask for.  The restart-class
	 * path already refuses this shape outright ("restart-class field
	 * requires persistence; use /api/v1/set"); a backend-level restart class
	 * reaches the same hazard through the live path, so the value is written
	 * out instead of lost.
	 *
	 * The test is a sequence number, not a false->true transition on the
	 * latch.  The latch is consumed by the main loop, which can be a whole
	 * second away (its select() carries a 1 s timeout), so a second live/set
	 * of a restart-class field while the first respawn is still pending saw
	 * no transition, skipped this write, and was then discarded by the
	 * re-exec that reloaded the FIRST value -- silently, in exactly the
	 * situation this guard was written to cover. */
	if (persist || venc_api_reinit_seq() != reinit_seq_before)
		(void)venc_api_save_config_to_disk(&actual_cfg);
	return 0;
}

static int resolve_set_query_field(const char *key, const char **canonical_key,
	const FieldDesc **field, int *status_code, char **response_json)
{
	if (!key || !*key || !canonical_key || !field || !status_code ||
	    !response_json)
		return -1;

	*canonical_key = canonicalize_field_key(key);
	*field = find_field(*canonical_key);
	if (!*field) {
		*status_code = 404;
		return make_error_json("not_found", "unknown config field",
			response_json) == 0 ? 1 : -1;
	}
	if (!venc_api_field_supported_for_backend(g_backend, *canonical_key)) {
		*status_code = 501;
		return make_error_json("not_implemented",
			"field not supported on this backend",
			response_json) == 0 ? 1 : -1;
	}

	return 0;
}

static void init_single_set_param(SetQueryParam *param, const char *key,
	const char *canonical_key, const char *value, const FieldDesc *field)
{
	if (!param || !key || !canonical_key || !value || !field)
		return;

	memset(param, 0, sizeof(*param));
	snprintf(param->key, sizeof(param->key), "%s", key);
	snprintf(param->canonical_key, sizeof(param->canonical_key), "%s",
		canonical_key);
	snprintf(param->value, sizeof(param->value), "%s", value);
	param->field = field;
}

/* Rapid-SET protection: after a successful resilience SET, the
 * pipeline runner pauses HTTP and tears down for respawn.  Any
 * subsequent SET arriving before HTTP resumes gets HTTP 503
 * `paused` from venc_httpd, which is the de-facto rate limit on
 * both backends.  Per-process static-timer rate limit (5 s window
 * once attempted) was removed during S7 bench validation because:
 *
 *   (a) The static resets on every respawn — so even if it fired,
 *       the next SET in a new process would not see the prior
 *       timestamp.
 *   (b) The window where the static could be consulted is the few
 *       milliseconds between SET completion and HTTP pause —
 *       narrower than network/SSH latency in practice.
 *
 * If a stricter rate limit is required, persist the timestamp to
 * /tmp/waybeam_resilience.ts and check on entry. */

static int process_restart_set_query(const SetQueryParam *param,
	int *status_code, char **response_json)
{
	VencConfig new_cfg;
	char *jval;
	int rc;
	int needs_respawn = 0;

	if (!param || !status_code || !response_json)
		return -1;

	pthread_mutex_lock(&g_cfg_mutex);
	new_cfg = *g_cfg;
	rc = stage_params_into_cfg(&new_cfg, param, 1, status_code,
		response_json);
	if (rc != 0) {
		pthread_mutex_unlock(&g_cfg_mutex);
		return rc > 0 ? 0 : rc;
	}

	/* Detect a resilience preset change.  stage_params_into_cfg() only
	 * copied the new preset *name* into new_cfg; the derived
	 * intra_refresh_* / ref_* / gop_size fields still hold the *old*
	 * preset's expansion.  Expand the new preset into new_cfg now so
	 * downstream code (and the diff log below) sees both old and new
	 * derived state side-by-side.
	 *
	 * Phase 0 instrumentation: log the field-level delta produced by
	 * the preset change.  This is the data we need to decide whether a
	 * given resilience SET needs the full reinit / reboot path or can
	 * be applied via lighter machinery (Phase 1+). */
	if (strcmp(g_cfg->video0.resilience, new_cfg.video0.resilience) != 0) {
		const VencConfigVideo old_v = g_cfg->video0;
		/* The preset owns intra_refresh_mode/lines, but NOT
		 * intra_refresh_qp: that one is an explicit operator override
		 * where 0 already means "use the preset's default", and on
		 * CV610 it is the only working I-frame lever.  Letting the
		 * preset zero it here would discard a value staged in THIS
		 * request, or one the craft already carried, with a 200 and no
		 * log — and new_cfg is what gets persisted. */
		const uint8_t staged_ir_qp = new_cfg.video0.intra_refresh_qp;

		(void)venc_config_apply_resilience_preset(
			new_cfg.video0.resilience, &new_cfg.video0);
		if (staged_ir_qp)
			new_cfg.video0.intra_refresh_qp = staged_ir_qp;

		/* Classify the delta:
		 *
		 *   ref_* changed     The SVC-T reference pyramid is bound
		 *                     to the VENC channel at creation and
		 *                     cannot be reconfigured by any
		 *                     documented MI SDK call.  Route via
		 *                     process-level respawn (fork+exec a
		 *                     fresh waybeam after clean teardown):
		 *                     a new VENC channel binds the new
		 *                     pyramid at creation.
		 *
		 *   intra/gop only    Honoured by the in-process reinit
		 *                     path (reload from disk, re-expand
		 *                     preset, teardown+reconfigure
		 *                     pipeline).  Same path every other
		 *                     MUT_RESTART field uses. */
		if (old_v.ref_base != new_cfg.video0.ref_base ||
		    old_v.ref_enhance != new_cfg.video0.ref_enhance ||
		    old_v.ref_pred != new_cfg.video0.ref_pred)
			needs_respawn = 1;

		fprintf(stderr,
			"[waybeam] resilience-diff: '%s' -> '%s'  "
			"intra=%s->%s ref_base=%u->%u ref_enhance=%u->%u "
			"ref_pred=%d->%d gop=%.3fs->%.3fs  path=%s\n",
			old_v.resilience, new_cfg.video0.resilience,
			old_v.intra_refresh_mode,
			new_cfg.video0.intra_refresh_mode,
			(unsigned)old_v.ref_base,
			(unsigned)new_cfg.video0.ref_base,
			(unsigned)old_v.ref_enhance,
			(unsigned)new_cfg.video0.ref_enhance,
			(int)old_v.ref_pred,
			(int)new_cfg.video0.ref_pred,
			old_v.gop_size, new_cfg.video0.gop_size,
			needs_respawn ? "respawn" : "live-reinit");
	}

	/* Detect a framing preset change.  Like resilience, the staged new_cfg
	 * holds the new preset *name* but the derived stab_crop_pct /
	 * stab_recenter_speed / zoom_pct still hold the old expansion.  Re-expand
	 * now so in-memory /status + GET stay coherent until the respawn reloads
	 * from disk.  Always a plain restart — no respawn classification needed. */
	if (strcmp(g_cfg->video0.framing, new_cfg.video0.framing) != 0)
		(void)venc_config_apply_framing_preset(new_cfg.video0.framing,
			&new_cfg.video0);

	/* Commit g_cfg in memory and persist to disk for both paths.
	 * For respawn, the fresh process will reload from disk anyway,
	 * but committing in-memory keeps /status / GET responses
	 * coherent with the SET that just succeeded. */
	*g_cfg = new_cfg;
	jval = field_to_json_value_from_cfg(&new_cfg, param->field);
	pthread_mutex_unlock(&g_cfg_mutex);
	(void)venc_api_save_config_to_disk(&new_cfg);

	/* Both classifications enqueue the same in-process signal: drop
	 * out of the stream loop.  Each backend's runner then decides
	 * what to do (Star6E and Maruko both currently always respawn on
	 * reinit; the `path=` label above is forward-looking — it tells
	 * an operator *why* this transition needs the slower path).  Do
	 * not branch routing on needs_respawn yet — if a future change
	 * re-enables in-process reconfigure for intra-only deltas, that
	 * lives in the runner, not the HTTP path. */
	(void)needs_respawn;
	venc_api_request_reinit();

	if (!jval) {
		*status_code = 500;
		return make_error_json("internal_error", "out of memory",
			response_json);
	}

	*status_code = 200;
	rc = make_single_set_success_json(param->key, jval,
		1 /* reinit_pending */, response_json);
	free(jval);
	return rc;
}

static int process_single_set_query(const char *query, int persist,
	int *status_code, char **response_json)
{
	char key[128], val[256];
	const char *canonical_key;
	const FieldDesc *f;
	SetQueryParam param;
	const char *parse_error = NULL;
	int rc;

	if (parse_first_query_param(query, key, sizeof(key), val, sizeof(val),
	    &parse_error) != 0 || !*key) {
		*status_code = 400;
		return make_error_json("invalid_request",
			parse_error ? parse_error :
			"missing query parameter key=value", response_json);
	}

	rc = resolve_set_query_field(key, &canonical_key, &f, status_code,
		response_json);
	if (rc != 0)
		return rc > 0 ? 0 : rc;

	init_single_set_param(&param, key, canonical_key, val, f);
	if (field_mut_for_backend(f) == MUT_LIVE) {
		return apply_live_set_query(&param, 1, 1, persist,
			status_code, response_json);
	}

	/* Restart-class fields respawn the pipeline, which reloads from
	 * disk — a volatile value would be silently discarded.  Reject on
	 * the live endpoint rather than pretend. */
	if (!persist) {
		*status_code = 400;
		return make_error_json("invalid_request",
			"restart-class field requires persistence; use /api/v1/set",
			response_json);
	}

	return process_restart_set_query(&param, status_code, response_json);
}

static int process_multi_live_set_query(const char *query, int persist,
	int *status_code, char **response_json)
{
	SetQueryParam params[SET_QUERY_MAX_PARAMS];
	const char *parse_error = NULL;
	size_t param_count = 0;

	if (parse_query_params(query, params, SET_QUERY_MAX_PARAMS, &param_count,
	    &parse_error) != 0) {
		*status_code = 400;
		return make_error_json("invalid_request",
			parse_error ? parse_error : "invalid query parameters",
			response_json);
	}
	if (param_count < 2)
		return process_single_set_query(query, persist, status_code,
			response_json);

	return apply_live_set_query(params, param_count, 0, persist,
		status_code, response_json);
}

static int process_set_query(const char *query, int persist, int *status_code,
	char **response_json)
{
	if (!status_code || !response_json)
		return -1;

	*status_code = 500;
	*response_json = NULL;

	if (query && strchr(query, '&'))
		return process_multi_live_set_query(query, persist,
			status_code, response_json);

	return process_single_set_query(query, persist, status_code,
		response_json);
}

/* ── Route handlers ──────────────────────────────────────────────────── */

#if HAVE_BACKEND_STAR6E
/* GET /api/v1/qr/tap.pgm — one frame of the overlay-free VPE port1 luma tap as
 * a P5 PGM.  Debug-grade instrumentation for validating the tap (OSD-freedom,
 * geometry, stability); the port itself is enabled at pipeline bring-up and is
 * NOT touched here — this only asks the reader thread to copy a frame out. */
static int handle_qr_tap_pgm(int fd, const HttpRequest *req, void *ctx)
{
	uint8_t *buf = NULL;
	size_t   len = 0;
	int rc;

	(void)req; (void)ctx;

	rc = star6e_luma_tap_grab_pgm(ctx, &buf, &len, 1000);
	if (rc == -ENODEV)
		return httpd_send_error(fd, 503, "tap_disabled",
			"luma tap not running (qr.tap_enabled off, VPE port1 "
			"held by stab/detect, or pipeline not running)");
	if (rc == -EBUSY)
		return httpd_send_error(fd, 409, "scan_decoding",
			"a QR cascade is reading the latch; retry shortly");
	if (rc == -ETIMEDOUT)
		return httpd_send_error(fd, 504, "tap_timeout",
			"timed out waiting for a frame from the VPE port1 tap");
	if (rc != 0 || !buf || len == 0) {
		star6e_luma_tap_free(buf);
		return httpd_send_error(fd, 500, "tap_failed",
			"luma tap capture failed");
	}

	rc = httpd_send_binary(fd, 200, "image/x-portable-graymap", buf,
		(int)len);
	star6e_luma_tap_free(buf);
	return rc;
}
#endif

#if HAVE_BACKEND_STAR6E
/* GET /api/v1/qr/scan[?ms=N] — open a scan window on VPE port1, or extend the
 * one already running.  The port is closed automatically by the supervisor when
 * the window expires, so a client that dies mid-scan cannot strand it. */
static int handle_qr_scan(int fd, const HttpRequest *req, void *ctx)
{
	char buf[192];
	Star6eLumaTapStatus st;
	char msbuf[16];
	uint32_t ms = 0;
	int rc;

	(void)ctx;
	if (req && httpd_query_param(req, "ms", msbuf, sizeof(msbuf)) == 0)
		ms = (uint32_t)strtoul(msbuf, NULL, 10);

	rc = star6e_luma_tap_scan(ctx, ms);
	if (rc == -ENODEV)
		return httpd_send_error(fd, 503, "tap_disabled",
			"luma tap not armed (qr.tap_enabled off or pipeline "
			"not running)");
	if (rc == -EBUSY)
		return httpd_send_error(fd, 409, "port1_busy",
			"VPE port1 is held by stab or detect");
	if (rc != 0)
		return httpd_send_error(fd, 500, "scan_failed",
			"could not program the VPE port1 tap");

	star6e_luma_tap_status(ctx, &st);
	snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{\"scanning\":true,\"window_ms\":%u,"
		"\"remaining_ms\":%lld,\"capture\":\"%ux%u\"}}",
		st.window_ms, (long long)st.remaining_ms, st.width, st.height);
	return httpd_send_json(fd, 200, buf);
}

/* GET /api/v1/qr/stop — end the window now and hand port1 back.  Deliberately
 * NOT /qr/scan/stop: the router matches on prefix and accepts a '/'
 * continuation (venc_httpd.c), so a nested path would be swallowed by the
 * /qr/scan route unless registration order happened to save it. */
static int handle_qr_scan_stop(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	star6e_luma_tap_scan_stop(ctx);
	return httpd_send_json(fd, 200,
		"{\"ok\":true,\"data\":{\"scanning\":false}}");
}

/* GET /api/v1/qr/status — poll a running scan without disturbing it.
 *
 * The decode block survives the window closing and is cleared only by the next
 * /qr/scan, so a client polling at 1 Hz still sees the payload from a window
 * that both found its code and shut itself down between two polls. */
static int handle_qr_status(int fd, const HttpRequest *req, void *ctx)
{
	char buf[512];
	Star6eLumaTapStatus st;

	(void)req; (void)ctx;
	star6e_luma_tap_status(ctx, &st);
	snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{\"armed\":%s,\"scanning\":%s,"
		"\"window_ms\":%u,\"remaining_ms\":%lld,\"capture\":\"%ux%u\","
		"\"frames\":%llu,\"grabs\":%llu,\"port1_owner\":\"%s\","
		"\"decode\":{\"attempts\":%u,\"decoded\":%s,\"payload\":\"%s\","
		"\"stage\":\"%s\",\"decode_ms\":%llu,\"last_ms\":%llu}}}",
		st.armed ? "true" : "false", st.scanning ? "true" : "false",
		st.window_ms, (long long)st.remaining_ms, st.width, st.height,
		(unsigned long long)st.frames, (unsigned long long)st.grabs,
		st.port1_owner,
		st.attempts, st.decoded ? "true" : "false", st.payload,
		st.stage,
		(unsigned long long)(st.decode_us / 1000),
		(unsigned long long)(st.last_us / 1000));
	return httpd_send_json(fd, 200, buf);
}
#endif

static int handle_version(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	char buf[512];
#ifndef VENC_VERSION
#define VENC_VERSION "unknown"
#endif
	snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{"
		"\"app_version\":\"%s\","
		"\"contract_version\":\"%s\","
		"\"config_schema_version\":\"1.0.0\","
		"\"backend\":\"%s\""
		"}}", VENC_VERSION, VENC_CONTRACT_VERSION, g_backend);
	return httpd_send_json(fd, 200, buf);
}

static int handle_config(int fd, const HttpRequest *req, void *ctx)
{
	uint16_t px = 0, py = 0, pw = 0, ph = 0;
	int precrop_valid;
	char taps[192];
	int taps_valid;
	char runtime[384];

	(void)req; (void)ctx;
	pthread_mutex_lock(&g_cfg_mutex);
	char *cfg_json = venc_config_to_json_string(g_cfg);
	pthread_mutex_unlock(&g_cfg_mutex);
	if (!cfg_json)
		return httpd_send_error(fd, 500, "internal_error",
			"failed to serialize config");

	/* runtime block: active_precrop (VIF capture rect) and vpe_taps (VPE
	 * scaler-output ownership).  Either may be absent; emit the object only
	 * when at least one is present, comma-joining what is. */
	precrop_valid = venc_api_get_active_precrop(&px, &py, &pw, &ph);
	taps_valid = venc_api_get_vpe_taps(taps, sizeof(taps));
	if (precrop_valid || taps_valid) {
		int n = snprintf(runtime, sizeof(runtime), ",\"runtime\":{");
		if (precrop_valid)
			n += snprintf(runtime + n, sizeof(runtime) - n,
				"\"active_precrop\":{\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u}",
				px, py, pw, ph);
		if (taps_valid)
			n += snprintf(runtime + n, sizeof(runtime) - n,
				"%s\"vpe_taps\":%s",
				precrop_valid ? "," : "", taps);
		snprintf(runtime + n, sizeof(runtime) - n, "}");
	} else {
		runtime[0] = '\0';
	}

	/* Stable device identity (SoC die ID) — the fleet key consumers read
	 * after discovering the mDNS beacon.  Empty string when the SoC exposes
	 * no die ID (e.g. ssc37x / Maruko). */
	char device[64];
	snprintf(device, sizeof(device), ",\"device\":{\"serial\":\"%s\"}",
		device_id_serial_cached());

	/* Wrap in envelope */
	size_t len = strlen(cfg_json) + strlen(runtime) + strlen(device) + 64;
	char *buf = malloc(len);
	if (!buf) {
		free(cfg_json);
		return httpd_send_error(fd, 500, "internal_error", "out of memory");
	}
	snprintf(buf, len, "{\"ok\":true,\"data\":{\"config\":%s%s%s}}",
		cfg_json, runtime, device);
	int ret = httpd_send_json(fd, 200, buf);
	free(buf);
	free(cfg_json);
	return ret;
}

static int handle_capabilities(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	/* Build a JSON object with field mutability */
	cJSON *root = cJSON_CreateObject();
	cJSON_AddBoolToObject(root, "ok", 1);
	cJSON *data = cJSON_AddObjectToObject(root, "data");

	/* Which optional routes this backend actually services.  The dashboard
	 * already fetches this at init, so it can decide whether to show the IQ
	 * tab from a pointer test here instead of firing a full ISP sweep at
	 * /api/v1/iq on every page load just to find out. */
	cJSON *routes = cJSON_AddObjectToObject(data, "routes");
	cJSON_AddBoolToObject(routes, "iq",
		(g_cb && g_cb->query_iq_info && g_cb->apply_iq_param) ? 1 : 0);
#if HAVE_BACKEND_STAR6E || HAVE_BACKEND_MARUKO
	cJSON_AddBoolToObject(routes, "iq_import", 1);
#else
	cJSON_AddBoolToObject(routes, "iq_import", 0);
#endif
	/* Tracks the callback rather than a compile-time backend test, which is
	 * what tests/test_venc_api.c asserts the value must do. */
	cJSON_AddBoolToObject(routes, "iq_export_bin",
		(g_cb && g_cb->export_isp_bin) ? 1 : 0);

	cJSON *fields = cJSON_AddObjectToObject(data, "fields");
	for (size_t i = 0; i < FIELD_COUNT; i++) {
		cJSON *entry = cJSON_AddObjectToObject(fields, g_fields[i].key);
		cJSON_AddStringToObject(entry, "mutability",
			field_mut_for_backend(&g_fields[i]) == MUT_LIVE ?
				"live" : "restart_required");
		cJSON_AddBoolToObject(entry, "supported",
			venc_api_field_supported_for_backend(g_backend,
				g_fields[i].key));
		/* Data-driven UI metadata (opt-in per field).  The dashboard
		 * renders a control from this when present, so a module field is
		 * WebUI-visible with no dashboard.html edit / blob rebuild. */
		if (g_fields[i].ui) {
			const FieldUi *u = g_fields[i].ui;
			cJSON *ui = cJSON_AddObjectToObject(entry, "ui");
			if (u->group)   cJSON_AddStringToObject(ui, "group", u->group);
			if (u->label)   cJSON_AddStringToObject(ui, "label", u->label);
			if (u->control) cJSON_AddStringToObject(ui, "control", u->control);
			if (u->tooltip) cJSON_AddStringToObject(ui, "tooltip", u->tooltip);
			if (u->control && strcmp(u->control, "number") == 0) {
				cJSON_AddNumberToObject(ui, "min", u->min);
				cJSON_AddNumberToObject(ui, "max", u->max);
				cJSON_AddNumberToObject(ui, "step", u->step);
			}
			if (u->options) {
				cJSON *arr = cJSON_AddArrayToObject(ui, "options");
				for (const char *const *o = u->options; *o; o++)
					cJSON_AddItemToArray(arr, cJSON_CreateString(*o));
			}
		}
	}
	char *str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!str)
		return httpd_send_error(fd, 500, "internal_error", "out of memory");
	int ret = httpd_send_json(fd, 200, str);
	free(str);
	return ret;
}

static int handle_fps_config(int fd, const HttpRequest *req, void *ctx)
{
	char buf[128];

	(void)req;
	(void)ctx;
	pthread_mutex_lock(&g_cfg_mutex);
	snprintf(buf, sizeof(buf), "{\"ok\":true,\"data\":{\"fps\":%u}}",
		g_cfg ? g_cfg->video0.fps : 0);
	pthread_mutex_unlock(&g_cfg_mutex);
	return httpd_send_json(fd, 200, buf);
}

static int handle_fps_live(int fd, const HttpRequest *req, void *ctx)
{
	uint32_t fps = 0;
	char buf[128];

	(void)req;
	(void)ctx;
	if (g_cb && g_cb->query_live_fps)
		fps = g_cb->query_live_fps();
	if (fps == 0) {
		pthread_mutex_lock(&g_cfg_mutex);
		fps = g_cfg ? g_cfg->video0.fps : 0;
		pthread_mutex_unlock(&g_cfg_mutex);
	}

	snprintf(buf, sizeof(buf), "{\"ok\":true,\"data\":{\"fps\":%u}}", fps);
	return httpd_send_json(fd, 200, buf);
}

static int handle_set(int fd, const HttpRequest *req, void *ctx)
{
	char *json = NULL;
	int status = 500;
	int rc;

	(void)ctx;

	rc = process_set_query(req->query, 1, &status, &json);
	if (rc != 0 || !json) {
		free(json);
		return httpd_send_error(fd, 500, "internal_error", "out of memory");
	}

	rc = httpd_send_json(fd, status, json);
	free(json);
	return rc;
}

/* /api/v1/live/set — /set's field surface, applied to the running config
 * only (no flash write).  Serves MUT_LIVE fields; restart-class fields are
 * rejected (a respawn reloads from disk and would discard the value). */
static int handle_live_set(int fd, const HttpRequest *req, void *ctx)
{
	char *json = NULL;
	int status = 500;
	int rc;

	(void)ctx;

	rc = process_set_query(req->query, 0, &status, &json);
	if (rc != 0 || !json) {
		free(json);
		return httpd_send_error(fd, 500, "internal_error", "out of memory");
	}

	rc = httpd_send_json(fd, status, json);
	free(json);
	return rc;
}

static int handle_get(int fd, const HttpRequest *req, void *ctx)
{
	(void)ctx;
	char key[128], dummy[4];
	const char *canonical_key;
	const char *parse_error = NULL;
	if (parse_first_query_param(req->query, key, sizeof(key),
			dummy, sizeof(dummy), &parse_error) != 0 || !*key) {
		return httpd_send_error(fd, 400, "invalid_request",
			parse_error ? parse_error :
			"missing query parameter (field name)");
	}

	canonical_key = canonicalize_field_key(key);
	const FieldDesc *f = find_field(canonical_key);
	if (!f) {
		return httpd_send_error(fd, 404, "not_found",
			"unknown config field");
	}

	pthread_mutex_lock(&g_cfg_mutex);
	char *jval = field_to_json_value(f);
	pthread_mutex_unlock(&g_cfg_mutex);

	char buf[512];
	snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{\"field\":\"%s\",\"value\":%s}}",
		key, jval);
	free(jval);
	return httpd_send_json(fd, 200, buf);
}

static int handle_awb(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	if (!g_cb || !g_cb->query_awb_info) {
		return httpd_send_error(fd, 501, "not_implemented",
			"AWB query not available");
	}
	char *json = g_cb->query_awb_info();
	if (!json) {
		return httpd_send_error(fd, 500, "internal_error",
			"AWB query failed");
	}
	int ret = httpd_send_json(fd, 200, json);
	free(json);
	return ret;
}

static int handle_iq(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	if (!g_cb || !g_cb->query_iq_info) {
		return httpd_send_error(fd, 501, "not_implemented",
			"IQ query not available");
	}
	char *json = g_cb->query_iq_info();
	if (!json) {
		return httpd_send_error(fd, 500, "internal_error",
			"IQ query failed");
	}
	int ret = httpd_send_json(fd, 200, json);
	free(json);
	return ret;
}

static int handle_iq_set(int fd, const HttpRequest *req, void *ctx)
{
	(void)ctx;
	if (!g_cb || !g_cb->apply_iq_param) {
		return httpd_send_error(fd, 501, "not_implemented",
			"IQ set not available");
	}
	char key[64], val[256];
	const char *parse_error = NULL;
	if (parse_first_query_param(req->query, key, sizeof(key),
			val, sizeof(val), &parse_error) != 0 || !*key || !*val) {
		return httpd_send_error(fd, 400, "invalid_request",
			parse_error ? parse_error :
			"usage: /api/v1/iq/set?param=value");
	}
	/* Validate value is numeric (with commas for arrays) */
	{
		const char *p = val;
		while (*p == '-' || *p == ',' || (*p >= '0' && *p <= '9')) p++;
		if (*p != '\0') {
			return httpd_send_error(fd, 400, "invalid_request",
				"value must be numeric (comma-separated for arrays)");
		}
	}
	if (g_cb->apply_iq_param(key, val) != 0) {
		return httpd_send_error(fd, 400, "apply_failed",
			"IQ parameter set failed");
	}
	char buf[512];
	if (strchr(val, ','))
		snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{\"param\":\"%s\",\"value\":[%s]}}",
			key, val);
	else
		snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{\"param\":\"%s\",\"value\":%s}}",
			key, val);
	return httpd_send_json(fd, 200, buf);
}

#if HAVE_BACKEND_STAR6E
extern int star6e_iq_import(const char *json_str);
#endif
#if HAVE_BACKEND_MARUKO
extern int maruko_iq_import(const char *json_str);
#endif

static int handle_iq_import(int fd, const HttpRequest *req, void *ctx)
{
	(void)ctx;
#if HAVE_BACKEND_STAR6E || HAVE_BACKEND_MARUKO
	if (req->body_len <= 0 || !req->body[0]) {
		return httpd_send_error(fd, 400, "invalid_request",
			"POST JSON body required (output of /api/v1/iq)");
	}
#if HAVE_BACKEND_STAR6E
	int ret = star6e_iq_import(req->body);
#else
	int ret = maruko_iq_import(req->body);
#endif
	if (ret != 0)
		return httpd_send_error(fd, 500, "import_partial",
			"some parameters failed to apply");
	return httpd_send_ok(fd, "{\"imported\":true}");
#else
	(void)req;
	return httpd_send_error(fd, 501, "not_implemented",
		"IQ import not available on this backend");
#endif
}

/* Fixed export destination.  /tmp is tmpfs on these boards, which is the
 * right lifetime for a file the operator is about to copy off. */
#define VENC_ISP_BIN_EXPORT_PATH "/tmp/isp_export.bin"

static int handle_iq_export_bin(int fd, const HttpRequest *req, void *ctx)
{
	char body[128];
	int written;

	(void)req; (void)ctx;
	if (!g_cb || !g_cb->export_isp_bin) {
		return httpd_send_error(fd, 501, "not_implemented",
			"ISP bin export not available on this backend");
	}
	/* The destination is fixed rather than taken from the query string: this
	 * endpoint is unauthenticated, and a caller-supplied path would make it a
	 * write-anywhere primitive.  Copy the file off afterwards. */
	written = g_cb->export_isp_bin(VENC_ISP_BIN_EXPORT_PATH);
	if (written <= 0) {
		/* Every distinct cause -- no libbin.so, a refused tuning connect, a
		 * short write on a full /tmp -- is named in the venc log; the HTTP
		 * layer has no channel for it. */
		return httpd_send_error(fd, 500, "internal_error",
			"ISP bin export failed; see the venc log for the reason");
	}
	/* Echo the byte count so a caller can confirm the write it is about to
	 * scp off, rather than trusting a constant path string. */
	snprintf(body, sizeof(body),
		"{\"path\":\"%s\",\"bytes\":%d}", VENC_ISP_BIN_EXPORT_PATH, written);
	return httpd_send_ok(fd, body);
}

static int handle_ae(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	if (!g_cb || !g_cb->query_ae_info) {
		return httpd_send_error(fd, 501, "not_implemented",
			"AE query not available");
	}
	char *json = g_cb->query_ae_info();
	if (!json) {
		return httpd_send_error(fd, 500, "internal_error",
			"AE query failed");
	}
	int ret = httpd_send_json(fd, 200, json);
	free(json);
	return ret;
}

static int handle_isp_metrics(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	if (!g_cb || !g_cb->query_isp_metrics) {
		return httpd_send_error(fd, 501, "not_implemented",
			"ISP metrics not available");
	}
	char *text = g_cb->query_isp_metrics();
	if (!text) {
		return httpd_send_error(fd, 500, "internal_error",
			"ISP metrics query failed");
	}
	int ret = httpd_send_text(fd, 200, text);
	free(text);
	return ret;
}

static int handle_transport_status(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	if (!g_cb || !g_cb->query_transport_status) {
		return httpd_send_error(fd, 501, "not_implemented",
			"transport status not available on this backend");
	}
	char *text = g_cb->query_transport_status();
	if (!text) {
		return httpd_send_error(fd, 500, "internal_error",
			"transport status query failed");
	}
	int ret = httpd_send_json(fd, 200, text);
	free(text);
	return ret;
}

static int handle_audio_status(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	if (!g_cb || !g_cb->query_audio_status) {
		return httpd_send_error(fd, 501, "not_implemented",
			"audio status not available on this backend");
	}
	char *text = g_cb->query_audio_status();
	if (!text) {
		return httpd_send_error(fd, 500, "internal_error",
			"audio status query failed");
	}
	int ret = httpd_send_json(fd, 200, text);
	free(text);
	return ret;
}

static int handle_defaults(int fd, const HttpRequest *req, void *ctx)
{
	VencConfig snapshot;
	VencConfig fresh;
	int save_rc;
	char resp[80];

	(void)req; (void)ctx;
	/* Build defaults in a local first, then swap under the lock.  Keeps
	 * the critical section short so live readers of g_cfg fields see a
	 * consistent commit point rather than a half-mutated mid-memset. */
	venc_config_defaults(&fresh);
	pthread_mutex_lock(&g_cfg_mutex);
	if (!g_cfg) {
		pthread_mutex_unlock(&g_cfg_mutex);
		return httpd_send_error(fd, 500, "internal_error",
			"config not registered");
	}
	*g_cfg = fresh;
	snapshot = fresh;
	pthread_mutex_unlock(&g_cfg_mutex);
	save_rc = venc_api_save_config_to_disk(&snapshot);
	/* Reinit always reloads the on-disk config.  On save failure the
	 * caller sees saved:false in the response and can decide whether to
	 * retry; reapplying the in-memory defaults silently would diverge
	 * from disk and confuse the next reload. */
	venc_api_request_reinit();
	snprintf(resp, sizeof(resp),
		"{\"defaults\":true,\"reinit\":true,\"saved\":%s}",
		save_rc == 0 ? "true" : "false");
	return httpd_send_ok(fd, resp);
}

static int handle_restart(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	/* /api/v1/restart is pure "reload from disk + reinit" (like SIGHUP).
	 * We do NOT write the in-memory config back to disk here, so that a
	 * manual file swap (scp, editor) followed by /api/v1/restart reloads
	 * exactly what the operator put on disk.  Persistence happens at the
	 * /api/v1/set level (LIVE and RESTART both now save per set). */
	venc_api_request_reinit();
	return httpd_send_ok(fd, "{\"reinit\":true}");
}

/* ── Attitude endpoints ──────────────────────────────────────────────── */

static int handle_attitude(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	if (!g_cb || !g_cb->query_attitude)
		return httpd_send_error(fd, 501, "not_implemented",
			"attitude not supported on this backend");
	char *json = g_cb->query_attitude();
	if (!json)
		return httpd_send_error(fd, 500, "internal_error",
			"out of memory");
	size_t len = strlen(json) + 32;
	char *buf = malloc(len);
	if (!buf) {
		free(json);
		return httpd_send_error(fd, 500, "internal_error",
			"out of memory");
	}
	snprintf(buf, len, "{\"ok\":true,\"data\":%s}", json);
	int rc = httpd_send_json(fd, 200, buf);
	free(buf);
	free(json);
	return rc;
}

/* GET /api/v1/attitude/calibrate_level — the "it's laying flat now"
 * calibration: average the level-pose accel (ODR-independent — completes
 * early at the sample target, else takes the <=2 s window's samples once
 * >=32 are in), solve the boresight trims, persist them through the
 * standard restart-set path (config file write; the running estimator
 * picks them up on the next restart). */
static int handle_attitude_calibrate(int fd, const HttpRequest *req,
	void *ctx)
{
	float roll = 0.0f, pitch = 0.0f;
	char q[64], buf[192];
	int status = 0;
	char *resp = NULL;

	(void)req; (void)ctx;
	if (!g_cb || !g_cb->attitude_calibrate_level)
		return httpd_send_error(fd, 501, "not_implemented",
			"attitude calibration not supported on this backend");
	if (g_cb->attitude_calibrate_level(&roll, &pitch) != 0)
		return httpd_send_error(fd, 409, "calibration_failed",
			"no IMU samples (attitude.enabled + imu.enabled?) "
			"or implausible gravity — hold the camera still");

	snprintf(q, sizeof(q), "attitude.trim_roll_deg=%.2f",
		(double)roll);
	if (process_set_query(q, 1, &status, &resp) != 0 || status != 200) {
		free(resp);
		return httpd_send_error(fd, 500, "internal_error",
			"failed to persist trim_roll_deg");
	}
	free(resp);
	resp = NULL;
	snprintf(q, sizeof(q), "attitude.trim_pitch_deg=%.2f",
		(double)pitch);
	if (process_set_query(q, 1, &status, &resp) != 0 || status != 200) {
		free(resp);
		return httpd_send_error(fd, 500, "internal_error",
			"failed to persist trim_pitch_deg");
	}
	free(resp);

	snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{\"trimRollDeg\":%.2f,"
		"\"trimPitchDeg\":%.2f,\"restartRequired\":true}}",
		(double)roll, (double)pitch);
	return httpd_send_json(fd, 200, buf);
}

static int handle_idr(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	if (!g_cb || !g_cb->request_idr) {
		return httpd_send_error(fd, 501, "not_implemented",
			"IDR request not available");
	}
	if (g_cb->request_idr() != 0) {
		return httpd_send_error(fd, 500, "internal_error",
			"IDR request failed");
	}
	return httpd_send_ok(fd, "{\"idr\":true}");
}

/* ── Record control endpoints ────────────────────────────────────────── */

/* `g_record_status_fn` only signals that the backend can report live
 * recorder state (used by `/api/v1/record/status`).  HTTP-driven
 * start/stop requires a backend that actually consumes the request
 * flags from its main loop; Star6E does, Maruko does not yet (Phase 6.5
 * backlog).  Backends opt in via
 * `venc_api_set_record_http_control_supported(1)` so /record/start|stop
 * don't lie with `{"ok":true}` on backends that silently drop the
 * request. */
static int record_http_supported(void)
{
	return g_record_http_control_supported ? 1 : 0;
}

static int handle_record_start(int fd, const HttpRequest *req, void *ctx)
{
	(void)ctx;
	char dir[256] = {0};
	char dummy[4];

	if (!record_http_supported())
		return httpd_send_error(fd, 501, "not_implemented",
			"HTTP record control not available on this backend");

	/* Optional ?dir=/path query parameter */
	if (req->query[0]) {
		char key[64];
		if (parse_first_query_param(req->query, key, sizeof(key),
				dir, sizeof(dir), NULL) == 0 &&
		    strcmp(key, "dir") == 0 && dir[0]) {
			/* Use provided dir */
		} else {
			/* No dir= param, check if config has one */
			pthread_mutex_lock(&g_cfg_mutex);
			snprintf(dir, sizeof(dir), "%s",
				g_cfg->record.dir[0] ? g_cfg->record.dir :
				RECORDER_DEFAULT_DIR);
			pthread_mutex_unlock(&g_cfg_mutex);
		}
	} else {
		pthread_mutex_lock(&g_cfg_mutex);
		snprintf(dir, sizeof(dir), "%s",
			g_cfg->record.dir[0] ? g_cfg->record.dir :
			RECORDER_DEFAULT_DIR);
		pthread_mutex_unlock(&g_cfg_mutex);
	}

	(void)dummy;
	venc_api_request_record_start(dir);

	char buf[512];
	snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{\"action\":\"start\",\"dir\":\"%s\"}}",
		dir);
	return httpd_send_json(fd, 200, buf);
}

static int handle_record_stop(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	if (!record_http_supported())
		return httpd_send_error(fd, 501, "not_implemented",
			"HTTP record control not available on this backend");
	venc_api_request_record_stop();
	return httpd_send_ok(fd, "{\"action\":\"stop\"}");
}

static int handle_record_status(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	VencRecordStatus st;
	char buf[1024];

	memset(&st, 0, sizeof(st));
	if (g_record_status_fn)
		g_record_status_fn(&st);

	snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{"
		"\"active\":%s,"
		"\"format\":\"%s\","
		"\"path\":\"%s\","
		"\"frames\":%u,"
		"\"bytes\":%llu,"
		"\"elapsed_ms\":%llu,"
		"\"segments\":%u,"
		"\"droppedFrames\":%u,"
		"\"writerPeakDepth\":%u,"
		"\"stop_reason\":\"%s\""
		"}}",
		st.active ? "true" : "false",
		st.format,
		st.path,
		st.frames_written,
		(unsigned long long)st.bytes_written,
		(unsigned long long)st.elapsed_ms,
		st.segments,
		st.dropped_frames,
		st.writer_peak_depth,
		st.stop_reason);
	return httpd_send_json(fd, 200, buf);
}

/* ── Dual VENC channel API ───────────────────────────────────────────── */

#if !HAVE_BACKEND_CV610
#include "star6e.h"  /* MI_VENC_* */
#if HAVE_BACKEND_MARUKO
#include "maruko_controls.h"
#else
#include "star6e_controls.h"
#endif
#endif

static struct {
	int active;
	int channel;
	uint32_t bitrate;   /* current kbps (may differ from config after adaptive) */
	uint32_t fps;
	uint32_t gop;
} g_dual;

/* Mutex protecting g_dual field access from the httpd thread.
 * Handlers run on the httpd pthread; register/unregister run on the
 * main thread.  This mutex prevents torn reads during registration
 * and ensures handlers don't start operations on a channel being
 * torn down. */
static pthread_mutex_t g_dual_mutex = PTHREAD_MUTEX_INITIALIZER;

void venc_api_dual_register(int channel, uint32_t bitrate, uint32_t fps,
	uint32_t gop)
{
	pthread_mutex_lock(&g_dual_mutex);
	g_dual.channel = channel;
	g_dual.bitrate = bitrate;
	g_dual.fps = fps;
	g_dual.gop = gop;
	g_dual.active = 1;
	pthread_mutex_unlock(&g_dual_mutex);
}

void venc_api_dual_unregister(void)
{
	pthread_mutex_lock(&g_dual_mutex);
	g_dual.active = 0;
	pthread_mutex_unlock(&g_dual_mutex);
}

static int handle_dual_status(int fd, const HttpRequest *req, void *ctx)
{
	char buf[512];
	int active, ch;
	uint32_t br, fps, gop;

	(void)req; (void)ctx;

	pthread_mutex_lock(&g_dual_mutex);
	active = g_dual.active;
	ch = (int)g_dual.channel;
	br = g_dual.bitrate;
	fps = g_dual.fps;
	gop = g_dual.gop;
	pthread_mutex_unlock(&g_dual_mutex);

	if (!active)
		return httpd_send_json(fd, 200,
			"{\"ok\":true,\"data\":{\"active\":false}}");

	snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{"
		"\"active\":true,"
		"\"channel\":%d,"
		"\"bitrate\":%u,"
		"\"fps\":%u,"
		"\"gop\":%u"
		"}}",
		ch, br, fps, gop);
	return httpd_send_json(fd, 200, buf);
}

#if HAVE_BACKEND_STAR6E
/* Apply bitrate/gop to ch1 via MI_VENC — same kernel ioctl pattern as ch0.
 * Star6E-only: the MI_VENC_*ChnAttr macros bind to i6_venc_chn here, but
 * Maruko's venc library expects i6c_venc_chn (different layout).  Until
 * a Maruko binding is wired up, /api/v1/dual/set returns 501 there. */
static int dual_apply_bitrate(uint32_t kbps)
{
	MI_VENC_ChnAttr_t attr = {0};
	MI_U32 bits;

	if (kbps > VENC_BITRATE_MAX_KBPS)
		kbps = VENC_BITRATE_MAX_KBPS;
	if (kbps < VENC_BITRATE_MIN_KBPS)
		kbps = VENC_BITRATE_MIN_KBPS;
	bits = kbps * 1024;

	if (MI_VENC_GetChnAttr(g_dual.channel, &attr) != 0)
		return -1;

	switch (attr.rate.mode) {
	case I6_VENC_RATEMODE_H265CBR:
		attr.rate.h265Cbr.bitrate = bits;  break;
	case I6_VENC_RATEMODE_H264CBR:
		attr.rate.h264Cbr.bitrate = bits;  break;
	case I6_VENC_RATEMODE_H265VBR:
		attr.rate.h265Vbr.maxBitrate = bits;  break;
	case I6_VENC_RATEMODE_H264VBR:
		attr.rate.h264Vbr.maxBitrate = bits;  break;
	case I6_VENC_RATEMODE_H265AVBR:
		attr.rate.h265Avbr.maxBitrate = bits;  break;
	case I6_VENC_RATEMODE_H264AVBR:
		attr.rate.h264Avbr.maxBitrate = bits;  break;
	default:
		return -1;
	}

	if (MI_VENC_SetChnAttr(g_dual.channel, &attr) != 0)
		return -1;
	g_dual.bitrate = kbps;
	return 0;
}

static int dual_apply_gop(uint32_t gop_frames)
{
	MI_VENC_ChnAttr_t attr = {0};

	if (MI_VENC_GetChnAttr(g_dual.channel, &attr) != 0)
		return -1;

	switch (attr.rate.mode) {
	case I6_VENC_RATEMODE_H265CBR:
		attr.rate.h265Cbr.gop = gop_frames;  break;
	case I6_VENC_RATEMODE_H264CBR:
		attr.rate.h264Cbr.gop = gop_frames;  break;
	case I6_VENC_RATEMODE_H265VBR:
		attr.rate.h265Vbr.gop = gop_frames;  break;
	case I6_VENC_RATEMODE_H264VBR:
		attr.rate.h264Vbr.gop = gop_frames;  break;
	case I6_VENC_RATEMODE_H265AVBR:
		attr.rate.h265Avbr.gop = gop_frames;  break;
	case I6_VENC_RATEMODE_H264AVBR:
		attr.rate.h264Avbr.gop = gop_frames;  break;
	default:
		return -1;
	}

	if (MI_VENC_SetChnAttr(g_dual.channel, &attr) != 0)
		return -1;
	g_dual.gop = gop_frames;
	return 0;
}
#endif /* HAVE_BACKEND_STAR6E */

static int handle_dual_set(int fd, const HttpRequest *req, void *ctx)
{
	char buf[256];
	const char *q;
	int ret;

	(void)ctx;

	/* dual_apply_{bitrate,gop} below operate through MI_VENC_*ChnAttr
	 * macros that bind to the Star6E i6_venc_chn struct layout.  On
	 * Maruko the venc library expects i6c_venc_chn (different layout)
	 * and the call path is wrong.  Until dual_apply_* is ported to the
	 * Maruko binding, refuse the write rather than corrupt the channel
	 * attr struct. */
#if !HAVE_BACKEND_STAR6E
	(void)req; (void)buf; (void)q; (void)ret;
	return httpd_send_error(fd, 501, "not_implemented",
		"dual/set not implemented on this backend");
#else
	pthread_mutex_lock(&g_dual_mutex);
	if (!g_dual.active) {
		pthread_mutex_unlock(&g_dual_mutex);
		return httpd_send_error(fd, 404, "not_active",
			"Dual VENC channel is not active");
	}
	if (!*req->query) {
		pthread_mutex_unlock(&g_dual_mutex);
		return httpd_send_error(fd, 400, "missing_param",
			"Usage: /api/v1/dual/set?bitrate=N or ?gop=N");
	}

	q = req->query;

	if (strncmp(q, "bitrate=", 8) == 0) {
		char *end;
		unsigned long val = strtoul(q + 8, &end, 10);
		uint32_t kbps;
		if (end == q + 8 || (*end != '\0' && *end != '&') ||
		    val == 0 || val > 200000) {
			pthread_mutex_unlock(&g_dual_mutex);
			return httpd_send_error(fd, 400, "invalid_value",
				"bitrate must be 1-200000 kbps");
		}
		kbps = (uint32_t)val;
		ret = dual_apply_bitrate(kbps);
		pthread_mutex_unlock(&g_dual_mutex);
		if (ret != 0)
			return httpd_send_error(fd, 500, "apply_failed",
				"MI_VENC_SetChnAttr failed");
		snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{\"field\":\"bitrate\",\"value\":%u}}",
			kbps);
		return httpd_send_json(fd, 200, buf);
	}

	if (strncmp(q, "gop=", 4) == 0) {
		double gop_sec = atof(q + 4);
		uint32_t frames;
		if (gop_sec <= 0) {
			pthread_mutex_unlock(&g_dual_mutex);
			return httpd_send_error(fd, 400, "invalid_value",
				"gop must be > 0 (seconds)");
		}
		frames = (uint32_t)(gop_sec * g_dual.fps + 0.5);
		if (frames < 1) frames = 1;
		ret = dual_apply_gop(frames);
		pthread_mutex_unlock(&g_dual_mutex);
		if (ret != 0)
			return httpd_send_error(fd, 500, "apply_failed",
				"MI_VENC_SetChnAttr failed");
		snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{\"field\":\"gop\",\"value\":%.2f,\"frames\":%u}}",
			gop_sec, frames);
		return httpd_send_json(fd, 200, buf);
	}

	pthread_mutex_unlock(&g_dual_mutex);
	return httpd_send_error(fd, 400, "unknown_param",
		"Supported: bitrate, gop");
#endif /* HAVE_BACKEND_STAR6E */
}

static int handle_idr_stats(int fd, const HttpRequest *req, void *ctx)
{
	/* 8 channels × ~40 B/entry + envelope ≈ 360 B worst case; 768 B
	 * leaves comfortable headroom and avoids the tight `sizeof(buf)-16`
	 * break margin needing to match the 3-byte `"]}}"` tail exactly. */
	char buf[768];
	size_t n = 0;

	(void)req; (void)ctx;

	n += (size_t)snprintf(buf + n, sizeof(buf) - n,
		"{\"ok\":true,\"data\":{\"min_spacing_us\":%u,"
		"\"channels\":[",
		IDR_RATE_LIMIT_MIN_SPACING_US);
	for (int chn = 0; chn < IDR_RATE_LIMIT_MAX_CHANNELS; chn++) {
		uint32_t h = idr_rate_limit_honored(chn);
		uint32_t d = idr_rate_limit_dropped(chn);
		if (h == 0 && d == 0)
			continue;  /* skip inactive channels */
		if (n > 0 && buf[n - 1] != '[')
			n += (size_t)snprintf(buf + n, sizeof(buf) - n, ",");
		n += (size_t)snprintf(buf + n, sizeof(buf) - n,
			"{\"idx\":%d,\"honored\":%u,\"dropped\":%u}",
			chn, h, d);
		if (n >= sizeof(buf) - 16)
			break;
	}
	(void)snprintf(buf + n, sizeof(buf) - n, "]}}");
	return httpd_send_json(fd, 200, buf);
}

#if HAVE_BACKEND_STAR6E || HAVE_BACKEND_MARUKO || HAVE_BACKEND_CV610
static int handle_intra_status(int fd, const HttpRequest *req, void *ctx)
{
	struct {
		char mode_name[16];
		int active, mi_supported, apply_ok;
		uint32_t target_ms, total_rows;
		uint32_t requested_lines, effective_lines_per_p;
		int      lines_clamped;
		uint32_t requested_qp, effective_qp;
		double   explicit_gop_sec, effective_gop_sec;
		int      gop_auto;
	} s;
	char buf[512];

	(void)req; (void)ctx;
	memset(&s, 0, sizeof(s));
#if HAVE_BACKEND_STAR6E
	{
		Star6eIntraRefreshStatus st;
		star6e_pipeline_intra_refresh_status(&st);
		snprintf(s.mode_name, sizeof(s.mode_name), "%s", st.mode_name);
		s.active                = st.active;
		s.mi_supported          = st.mi_supported;
		s.apply_ok              = st.apply_ok;
		s.target_ms             = st.target_ms;
		s.total_rows            = st.total_rows;
		s.requested_lines       = st.requested_lines;
		s.effective_lines_per_p = st.effective_lines_per_p;
		s.lines_clamped         = st.lines_clamped;
		s.requested_qp          = st.requested_qp;
		s.effective_qp          = st.effective_qp;
		s.explicit_gop_sec      = st.explicit_gop_sec;
		s.effective_gop_sec     = st.effective_gop_sec;
		s.gop_auto              = st.gop_auto;
	}
#elif HAVE_BACKEND_MARUKO
	{
		MarukoIntraRefreshStatus st;
		maruko_pipeline_intra_refresh_status(&st);
		snprintf(s.mode_name, sizeof(s.mode_name), "%s", st.mode_name);
		s.active                = st.active;
		s.mi_supported          = st.mi_supported;
		s.apply_ok              = st.apply_ok;
		s.target_ms             = st.target_ms;
		s.total_rows            = st.total_rows;
		s.requested_lines       = st.requested_lines;
		s.effective_lines_per_p = st.effective_lines_per_p;
		s.lines_clamped         = st.lines_clamped;
		s.requested_qp          = st.requested_qp;
		s.effective_qp          = st.effective_qp;
		s.explicit_gop_sec      = st.explicit_gop_sec;
		s.effective_gop_sec     = st.effective_gop_sec;
		s.gop_auto              = st.gop_auto;
	}
#elif HAVE_BACKEND_CV610
	{
		Cv610IntraRefreshStatus st;
		cv610_runtime_intra_refresh_status(&st);
		snprintf(s.mode_name, sizeof(s.mode_name), "%s", st.mode_name);
		s.active                = st.active;
		s.mi_supported          = st.mi_supported;
		s.apply_ok              = st.apply_ok;
		s.target_ms             = st.target_ms;
		s.total_rows            = st.total_rows;
		s.requested_lines       = st.requested_lines;
		s.effective_lines_per_p = st.effective_lines_per_p;
		s.lines_clamped         = st.lines_clamped;
		s.requested_qp          = st.requested_qp;
		s.effective_qp          = st.effective_qp;
		s.explicit_gop_sec      = st.explicit_gop_sec;
		s.effective_gop_sec     = st.effective_gop_sec;
		s.gop_auto              = st.gop_auto;
	}
#endif
	if (s.mode_name[0] == '\0')
		snprintf(s.mode_name, sizeof(s.mode_name), "off");

	snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{"
		"\"mode\":\"%s\","
		"\"active\":%s,"
		"\"mi_supported\":%s,"
		"\"apply_ok\":%s,"
		"\"target_ms\":%u,"
		"\"total_rows\":%u,"
		"\"lines\":{\"requested\":%u,\"effective\":%u,\"clamped\":%s},"
		"\"qp\":{\"requested\":%u,\"effective\":%u},"
		"\"gop\":{\"explicit_sec\":%.3f,\"effective_sec\":%.3f,\"auto\":%s}"
		"}}",
		s.mode_name,
		s.active ? "true" : "false",
		s.mi_supported ? "true" : "false",
		s.apply_ok ? "true" : "false",
		s.target_ms,
		s.total_rows,
		s.requested_lines, s.effective_lines_per_p,
		s.lines_clamped ? "true" : "false",
		s.requested_qp, s.effective_qp,
		s.explicit_gop_sec, s.effective_gop_sec,
		s.gop_auto ? "true" : "false");
	return httpd_send_json(fd, 200, buf);
}

/* Combined view of the resilience preset and its applied feature state.
 * Returns: preset name, intra-refresh runtime state, refPred runtime
 * state, effective GOP — everything needed to verify a preset took. */
static int handle_resilience_status(int fd, const HttpRequest *req, void *ctx)
{
	char preset[16] = "off";
	char intra_mode[16] = "off";
	int  intra_active = 0, intra_supported = 0, intra_apply_ok = 0;
	uint32_t intra_lines = 0, intra_qp = 0;
	int  rp_active = 0, rp_supported = 0, rp_apply_ok = 0;
	uint32_t rp_base = 0, rp_enhance = 0;
	int  rp_pred = 0;
	double gop_sec = 0.0;
	int    gop_auto = 0;
	char buf[640];

	(void)req; (void)ctx;

	pthread_mutex_lock(&g_cfg_mutex);
	if (g_cfg) {
		snprintf(preset, sizeof(preset), "%s",
			g_cfg->video0.resilience);
		gop_sec = g_cfg->video0.gop_size;
	}
	pthread_mutex_unlock(&g_cfg_mutex);

#if HAVE_BACKEND_STAR6E
	{
		Star6eIntraRefreshStatus st;
		Star6eRefPredStatus      rp;
		star6e_pipeline_intra_refresh_status(&st);
		star6e_pipeline_ref_pred_status(&rp);
		snprintf(intra_mode, sizeof(intra_mode), "%s",
			st.mode_name[0] ? st.mode_name : "off");
		intra_active    = st.active;
		intra_supported = st.mi_supported;
		intra_apply_ok  = st.apply_ok;
		intra_lines     = st.effective_lines_per_p;
		intra_qp        = st.effective_qp;
		gop_auto        = st.gop_auto;
		rp_active       = rp.active;
		rp_supported    = rp.mi_supported;
		rp_apply_ok     = rp.apply_ok;
		rp_base         = rp.base;
		rp_enhance      = rp.enhance;
		rp_pred         = rp.pred;
	}
#elif HAVE_BACKEND_MARUKO
	{
		MarukoIntraRefreshStatus st;
		MarukoRefPredStatus      rp;
		maruko_pipeline_intra_refresh_status(&st);
		maruko_pipeline_ref_pred_status(&rp);
		snprintf(intra_mode, sizeof(intra_mode), "%s",
			st.mode_name[0] ? st.mode_name : "off");
		intra_active    = st.active;
		intra_supported = st.mi_supported;
		intra_apply_ok  = st.apply_ok;
		intra_lines     = st.effective_lines_per_p;
		intra_qp        = st.effective_qp;
		gop_auto        = st.gop_auto;
		rp_active       = rp.active;
		rp_supported    = rp.mi_supported;
		rp_apply_ok     = rp.apply_ok;
		rp_base         = rp.base;
		rp_enhance      = rp.enhance;
		rp_pred         = rp.pred;
	}
#elif HAVE_BACKEND_CV610
	{
		Cv610IntraRefreshStatus st;
		Cv610RefPredStatus rp;
		cv610_runtime_intra_refresh_status(&st);
		cv610_runtime_ref_pred_status(&rp);
		snprintf(intra_mode, sizeof(intra_mode), "%s",
			st.mode_name[0] ? st.mode_name : "off");
		intra_active    = st.active;
		intra_supported = st.mi_supported;
		intra_apply_ok  = st.apply_ok;
		intra_lines     = st.effective_lines_per_p;
		intra_qp        = st.effective_qp;
		gop_auto        = st.gop_auto;
		rp_active       = rp.active;
		rp_supported    = rp.mi_supported;
		rp_apply_ok     = rp.apply_ok;
		rp_base         = rp.base;
		rp_enhance      = rp.enhance;
		rp_pred         = rp.pred;
	}
#endif

	snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{"
		"\"preset\":\"%s\","
		"\"intra\":{\"mode\":\"%s\",\"active\":%s,"
			"\"mi_supported\":%s,\"apply_ok\":%s,"
			"\"effective_lines\":%u,\"effective_qp\":%u},"
		"\"refPred\":{\"active\":%s,\"mi_supported\":%s,"
			"\"apply_ok\":%s,\"base\":%u,\"enhance\":%u,\"pred\":%s},"
		"\"gop\":{\"effective_sec\":%.3f,\"auto\":%s}"
		"}}",
		preset, intra_mode,
		intra_active ? "true" : "false",
		intra_supported ? "true" : "false",
		intra_apply_ok ? "true" : "false",
		intra_lines, intra_qp,
		rp_active ? "true" : "false",
		rp_supported ? "true" : "false",
		rp_apply_ok ? "true" : "false",
		rp_base, rp_enhance,
		rp_pred ? "true" : "false",
		gop_sec,
		gop_auto ? "true" : "false");
	return httpd_send_json(fd, 200, buf);
}

#endif

static int handle_dual_idr(int fd, const HttpRequest *req, void *ctx)
{
#if HAVE_BACKEND_CV610
	(void)req;
	(void)ctx;
	return httpd_send_error(fd, 501, "not_implemented",
		"dual VENC is not available on CV610");
#else
	MI_VENC_CHN ch;
	int ret;

	(void)req; (void)ctx;

	pthread_mutex_lock(&g_dual_mutex);
	if (!g_dual.active) {
		pthread_mutex_unlock(&g_dual_mutex);
		return httpd_send_error(fd, 404, "not_active",
			"Dual VENC channel is not active");
	}
	ch = g_dual.channel;
	pthread_mutex_unlock(&g_dual_mutex);

	if (!idr_rate_limit_allow((int)ch))
		return httpd_send_json(fd, 200,
			"{\"ok\":true,\"data\":{\"idr\":true,\"coalesced\":true}}");

	ret = MI_VENC_RequestIdr(ch, 1);
	if (ret != 0)
		return httpd_send_error(fd, 500, "idr_failed",
			"MI_VENC_RequestIdr failed");

	return httpd_send_json(fd, 200, "{\"ok\":true,\"data\":{\"idr\":true}}");
#endif
}

/* ── Sensor modes ────────────────────────────────────────────────────── */

static int handle_modes(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
#if HAVE_BACKEND_CV610
	/* Generated from the same table cv610_validate_config() rejects
	 * against, so what this lists is exactly what /set accepts. */
	size_t count = 0;
	size_t i;
	const Cv610SensorMode *modes = cv610_mode_table(&count);
	int selected, selected_pad;
	cJSON *root, *data, *pads, *pad, *arr;
	char *str;
	int rc;

	/* What the backend actually brought up, published by cv610_prepare()
	 * through the same venc_api_set_sensor_info() star6e_pipeline.c uses.
	 * Recomputing it from video0.fps here would report the CONFIGURED mode
	 * even when a forced sensor.mode, a substituted rate, or a failed
	 * bring-up means something else is running.  -1 until bring-up runs. */
	pthread_mutex_lock(&g_cfg_mutex);
	selected = g_sensor_mode;
	selected_pad = g_sensor_pad;
	pthread_mutex_unlock(&g_cfg_mutex);

	root = cJSON_CreateObject();
	if (!root)
		return httpd_send_error(fd, 500, "internal_error", "out of memory");
	cJSON_AddBoolToObject(root, "ok", 1);
	data = cJSON_AddObjectToObject(root, "data");
	cJSON_AddNumberToObject(data, "selected_pad", selected_pad);
	cJSON_AddNumberToObject(data, "selected_mode", selected);
	pads = cJSON_AddArrayToObject(data, "pads");
	pad = cJSON_CreateObject();
	cJSON_AddItemToArray(pads, pad);
	cJSON_AddNumberToObject(pad, "pad", 0);
	arr = cJSON_AddArrayToObject(pad, "modes");
	for (i = 0; i < count; i++) {
		cJSON *m = cJSON_CreateObject();

		cJSON_AddItemToArray(arr, m);
		cJSON_AddNumberToObject(m, "index", (double)i);
		cJSON_AddNumberToObject(m, "width", modes[i].width);
		cJSON_AddNumberToObject(m, "height", modes[i].height);
		cJSON_AddNumberToObject(m, "min_fps", modes[i].fps);
		cJSON_AddNumberToObject(m, "max_fps", modes[i].fps);
		cJSON_AddStringToObject(m, "desc", modes[i].desc);
		cJSON_AddBoolToObject(m, "selected", (int)i == selected);
	}
	str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!str)
		return httpd_send_error(fd, 500, "internal_error", "out of memory");
	rc = httpd_send_json(fd, 200, str);
	free(str);
	return rc;
#else
	pthread_mutex_lock(&g_cfg_mutex);
	int pad = g_sensor_pad, mode = g_sensor_mode, forced = g_sensor_forced_pad;
	pthread_mutex_unlock(&g_cfg_mutex);
	char *json = sensor_modes_json(forced, pad, mode);
	if (!json)
		return httpd_send_error(fd, 500, "modes_failed", "Failed to query sensor modes");
	int rc = httpd_send_json(fd, 200, json);
	free(json);
	return rc;
#endif
}

/* ── Registration ────────────────────────────────────────────────────── */

int venc_api_register(VencConfig *cfg, const char *backend_name,
	const VencApplyCallbacks *cb, void *backend_ctx)
{
	int r = 0;
	(void)backend_ctx; /* QR routes consume it in Star6E builds. */

	pthread_mutex_lock(&g_cfg_mutex);
	g_cfg = cfg;
	g_cb = cb;
	snprintf(g_backend, sizeof(g_backend), "%s",
		backend_name ? backend_name : "unknown");
	if (g_api_routes_registered) {
		pthread_mutex_unlock(&g_cfg_mutex);
		return 0;
	}
	g_api_routes_registered = 1;
	pthread_mutex_unlock(&g_cfg_mutex);

	r |= venc_httpd_route("GET", "/api/v1/snapshot.jpg", handle_snapshot_jpeg, NULL);
#if HAVE_BACKEND_STAR6E
	r |= venc_httpd_route("GET", "/api/v1/qr/tap.pgm", handle_qr_tap_pgm,
		backend_ctx);
	r |= venc_httpd_route("GET", "/api/v1/qr/scan", handle_qr_scan,
		backend_ctx);
	r |= venc_httpd_route("GET", "/api/v1/qr/stop", handle_qr_scan_stop,
		backend_ctx);
	r |= venc_httpd_route("GET", "/api/v1/qr/status", handle_qr_status,
		backend_ctx);
#endif
	r |= venc_httpd_route("GET", "/api/v1/version",      handle_version, NULL);
	r |= venc_httpd_route("GET", "/api/v1/config",       handle_config, NULL);
	r |= venc_httpd_route("GET", "/api/v1/config.json",  handle_config, NULL);
	r |= venc_httpd_route("GET", "/api/v1/capabilities", handle_capabilities, NULL);
	r |= venc_httpd_route("GET", "/api/v1/set",          handle_set, NULL);
	r |= venc_httpd_route("GET", "/api/v1/live/set",     handle_live_set, NULL);
	r |= venc_httpd_route("GET", "/api/v1/get",          handle_get, NULL);
	r |= venc_httpd_route("GET", "/api/v1/fps/config",   handle_fps_config, NULL);
	r |= venc_httpd_route("GET", "/api/v1/fps/live",     handle_fps_live, NULL);
	r |= venc_httpd_route("GET", "/api/v1/restart",      handle_restart, NULL);
	/* longer prefix first — routing is first-match prefix */
	r |= venc_httpd_route("GET", "/api/v1/attitude/calibrate_level",
		handle_attitude_calibrate, NULL);
	r |= venc_httpd_route("GET", "/api/v1/attitude",
		handle_attitude, NULL);
	r |= venc_httpd_route("GET", "/api/v1/defaults",     handle_defaults, NULL);
	r |= venc_httpd_route("GET", "/api/v1/ae",           handle_ae, NULL);
	r |= venc_httpd_route("GET", "/api/v1/awb",          handle_awb, NULL);
	/* Longer prefix first — routing is first-match prefix and accepts '/' as
	 * a boundary, so "/api/v1/iq" would otherwise swallow any longer
	 * "/api/v1/iq/..." route registered after it and answer that request with
	 * the full ISP sweep instead of 404. */
	r |= venc_httpd_route("GET", "/api/v1/iq/set",       handle_iq_set, NULL);
	r |= venc_httpd_route("POST", "/api/v1/iq/import",  handle_iq_import, NULL);
	r |= venc_httpd_route("GET", "/api/v1/iq/export_bin", handle_iq_export_bin, NULL);
	r |= venc_httpd_route("GET", "/api/v1/iq",           handle_iq, NULL);
	r |= venc_httpd_route("GET", "/api/v1/modes",        handle_modes, NULL);
	r |= venc_httpd_route("GET", "/metrics/isp",         handle_isp_metrics, NULL);
	r |= venc_httpd_route("GET", "/api/v1/transport/status", handle_transport_status, NULL);
	r |= venc_httpd_route("GET", "/api/v1/audio/status", handle_audio_status, NULL);
	r |= venc_httpd_route("GET", "/request/idr",         handle_idr, NULL);
	r |= venc_httpd_route("GET", "/api/v1/record/start",  handle_record_start, NULL);
	r |= venc_httpd_route("GET", "/api/v1/record/stop",   handle_record_stop, NULL);
	r |= venc_httpd_route("GET", "/api/v1/record/status", handle_record_status, NULL);
	r |= venc_httpd_route("GET", "/api/v1/dual/status", handle_dual_status, NULL);
	r |= venc_httpd_route("GET", "/api/v1/dual/set",    handle_dual_set, NULL);
	r |= venc_httpd_route("GET", "/api/v1/dual/idr",    handle_dual_idr, NULL);
	r |= venc_httpd_route("GET", "/api/v1/idr/stats",   handle_idr_stats, NULL);
#if HAVE_BACKEND_STAR6E || HAVE_BACKEND_MARUKO || HAVE_BACKEND_CV610
	r |= venc_httpd_route("GET", "/api/v1/intra/status", handle_intra_status, NULL);
	r |= venc_httpd_route("GET", "/api/v1/resilience/status",
		handle_resilience_status, NULL);
#endif
	r |= venc_webui_register();
	if (r != 0) {
		pthread_mutex_lock(&g_cfg_mutex);
		g_api_routes_registered = 0;
		pthread_mutex_unlock(&g_cfg_mutex);
		fprintf(stderr, "[api] ERROR: failed to register one or more routes\n");
		return -1;
	}
	return 0;
}
