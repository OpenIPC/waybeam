/*
 * CV610 pipeline ported from the hardware-verified standalone bring-up in
 * snokvist/hisilicon.  Keep platform state confined to this translation unit;
 * the public lifecycle is the small cv610_pipeline_* interface below.
 */
#include "cv610_pipeline.h"

#include "pipeline_common.h"
/* Minimal Hi3516CV610 MIPI/VI/ISP graph for one sensor (cv610_modes.c).  The
 * target's MPP module loader and sensor-clock prerequisites are documented in
 * docs/CV610_BACKEND.md.  VENC and output ownership stay in cv610_runtime.c. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>

#include "ot_type.h"
#include "ot_common.h"
#include "ot_common_video.h"
#include "ot_common_vb.h"
#include "ot_common_sys.h"
#include "ot_common_vi.h"
#include "ot_common_vpss.h"
#include "ot_common_isp.h"
#include "ot_common_3a.h"
#include "ot_mipi_rx.h"
#include "ot_sns_ctrl.h"
#include "ss_mpi_sys.h"
#include "ss_mpi_sys_mem.h"
#include "ss_mpi_vb.h"
#include "ss_mpi_vi.h"
#include "ss_mpi_vpss.h"
#include "ss_mpi_sys_bind.h"
#include "ss_mpi_isp.h"
#include "ss_mpi_ae.h"
#include "ss_mpi_awb.h"

#define MIPI_DEV_NODE       "/dev/ot_mipi_rx"

#define VI_DEV              0
#define VI_PIPE             0
#define VI_CHN              0
#define VPSS_GRP            0
#define VPSS_CHN            0

#define CV610_CHECK(expr) do { \
		td_s32 check_ret = (expr); \
		if (check_ret != TD_SUCCESS) { \
			fprintf(stderr, "FAIL %s = 0x%x\n", #expr, check_ret); \
			return -1; \
		} \
		printf("  ok  %s\n", #expr); \
	} while (0)

/* Same, but tolerate "already initialized" (BUSY). */
static td_s32 cv610_error_id(td_s32 ret)
{
	return ret & 0x1fff;
}

#define CV610_CHECK_OR_BUSY(expr) do { \
		td_s32 check_ret = (expr); \
		if (check_ret != TD_SUCCESS && \
			cv610_error_id(check_ret) != OT_ERR_BUSY) { \
			fprintf(stderr, "FAIL %s = 0x%x\n", #expr, check_ret); \
			return -1; \
		} \
		printf("  ok  %s%s\n", #expr, \
			(check_ret == TD_SUCCESS) ? "" : "  (already up)"); \
	} while (0)

typedef struct {
	unsigned int width;
	unsigned int height;
	float        fps;
	int          lanes;        /* 1..4 */
	int          data_rate_x2; /* MIPI_DATA_RATE_X1 or X2 */
	int          bayer;        /* ot_isp_bayer_format 0..3 */
	int          raw_bit;      /* 10 or 12 */
	uint32_t     sensor_clock_hz; /* MCLK this mode's line timing assumes */
	unsigned int out_width;    /* VPSS output = encoded size */
	unsigned int out_height;
	int          keep_aspect;  /* isp.keepAspect: crop before scaling */
	int          mirror;       /* image.mirror: sensor H reverse */
	int          flip;         /* image.flip:   sensor V reverse */
	const Cv610Sensor *sensor; /* plugin to load, id the ISP binds */
} Cv610PipelineRuntimeConfig;

static volatile sig_atomic_t g_stop;
static pthread_t    g_isp_thread;
static int          g_isp_thread_ok;

static void *isp_thread_fn(void *arg)
{
	(void)arg;
	/* ss_mpi_isp_run() does not return until ss_mpi_isp_exit(). */
	td_s32 ret = ss_mpi_isp_run(VI_PIPE);
	printf("[isp] ss_mpi_isp_run returned 0x%x\n", ret);
	return NULL;
}

/* ------------------------------------------------------------------ MIPI -- */

/* Each sensor mode assumes a specific MCLK; the table is src/cv610_modes.c.
 * A mismatch is silent: the sensor keeps its programmed line timing against
 * the wrong input clock and the delivered rate scales by the ratio — measured
 * 43.6 fps for a 60 fps mode running on the 100 fps mode's 27 MHz clock.  The
 * clock is a CRG register only the kernel can write, and the MIPI ioctls gate
 * it without setting a rate, so sys_config exposes it as a frequency and
 * keeps the register encoding. */
#define CV610_SNS0_CLK_HZ_PATH "/sys/module/open_sys_config/parameters/sns0_clk_hz"

/* Absent knob and failed write are different answers.  A sys_config without
 * the parameter predates it, and refusing to run on that module would strand
 * every craft still carrying it — so that stays a warning, and the clock
 * remains whatever CV610_SENSOR_PROFILE loaded.  A knob that is present and
 * will not take the value is a different thing: the mode's line timing is
 * then known to be running against the wrong MCLK, and continuing would
 * stream at a rate nothing reports.  Fail the bring-up instead, loudly.
 *
 * Returns 0 to continue, -1 to abort. */
static int sensor_clock_select(uint32_t hz, uint32_t fps)
{
	char buf[16];
	int len, fd;

	/* No mode clock resolved: leave whatever the loader set. */
	if (hz == 0) {
		return 0;
	}
	fd = open(CV610_SNS0_CLK_HZ_PATH, O_WRONLY);
	if (fd < 0) {
		if (errno == ENOENT) {
			/* Older sys_config: the clock stays as CV610_SENSOR_PROFILE
			 * left it, which suits exactly one mode. */
			fprintf(stderr,
				"WARNING: %s absent — sensor clock stays as loaded; "
				"%u fps needs %u Hz and will otherwise run at the wrong rate\n",
				CV610_SNS0_CLK_HZ_PATH, fps, hz);
			return 0;
		}
		fprintf(stderr, "FAIL open %s: %s\n", CV610_SNS0_CLK_HZ_PATH,
			strerror(errno));
		return -1;
	}
	len = snprintf(buf, sizeof(buf), "%u", hz);
	if (len < 0 || (size_t)len >= sizeof(buf) ||
		write(fd, buf, (size_t)len) != len) {
		fprintf(stderr, "FAIL set sensor clock %u Hz for %u fps: %s\n", hz,
			fps, strerror(errno));
		close(fd);
		return -1;
	}
	printf("  ok  sensor clock %u Hz for %u fps\n", hz, fps);
	close(fd);
	return 0;
}

static int mipi_setup(const Cv610PipelineRuntimeConfig *c)
{
	combo_dev_attr_t attr;
	combo_dev_t      dev = 0;
	sns_clk_source_t clk = 0;
	sns_rst_source_t rst = 0;
	lane_divide_mode_t hs = LANE_DIVIDE_MODE_0;
	int fd, i;

	memset(&attr, 0, sizeof(attr));
	attr.devno          = dev;
	attr.input_mode     = INPUT_MODE_MIPI;
	attr.data_rate      = c->data_rate_x2 ? MIPI_DATA_RATE_X2 : MIPI_DATA_RATE_X1;
	attr.img_rect.x     = 0;
	attr.img_rect.y     = 0;
	attr.img_rect.width  = c->width;
	attr.img_rect.height = c->height;
	attr.mipi_attr.input_data_type =
		(c->raw_bit == 10) ? DATA_TYPE_RAW_10BIT : DATA_TYPE_RAW_12BIT;
	attr.mipi_attr.wdr_mode = OT_MIPI_WDR_MODE_NONE;
	for (i = 0; i < MIPI_LANE_NUM; i++) {
		attr.mipi_attr.lane_id[i] = (i < c->lanes) ? (short)i : (short)-1;
	}

	fd = open(MIPI_DEV_NODE, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", MIPI_DEV_NODE, strerror(errno));
		return -1;
	}

#define mipi_ioctl(req, arg) do { \
		if (ioctl(fd, (req), (arg)) < 0) { \
			fprintf(stderr, "FAIL mipi ioctl %s: %s\n", #req, strerror(errno)); \
			close(fd); \
			return -1; \
		} \
	} while (0)

	mipi_ioctl(OT_MIPI_SET_HS_MODE, &hs);
	mipi_ioctl(OT_MIPI_DISABLE_MIPI_CLOCK, &dev);
	mipi_ioctl(OT_MIPI_RESET_MIPI, &dev);
	mipi_ioctl(OT_MIPI_DISABLE_SENSOR_CLOCK, &clk);
	mipi_ioctl(OT_MIPI_RESET_SENSOR, &rst);

	mipi_ioctl(OT_MIPI_SET_DEV_ATTR, &attr);

	mipi_ioctl(OT_MIPI_ENABLE_MIPI_CLOCK, &dev);
	mipi_ioctl(OT_MIPI_UNRESET_MIPI, &dev);
	mipi_ioctl(OT_MIPI_ENABLE_SENSOR_CLOCK, &clk);
	/* After the enable, which rewrites the same CRG register, and before
	 * the sensor leaves reset so it comes up on its final clock. */
	if (sensor_clock_select(c->sensor_clock_hz, (uint32_t)c->fps) != 0) {
		close(fd);
		return -1;
	}
	mipi_ioctl(OT_MIPI_UNRESET_SENSOR, &rst);
#undef mipi_ioctl

	close(fd);
	printf("  ok  mipi: %ux%u raw%d, %d lane(s), data_rate x%d\n",
		   c->width, c->height, c->raw_bit, c->lanes, c->data_rate_x2 ? 2 : 1);
	return 0;
}

/* --------------------------------------------------------------- VB / SYS -- */

/* Two configs match when they would hand out the same buffers. Only the pools
 * actually in use are compared; the rest of the struct is zero-filled padding
 * and an mmz_name this backend never sets. */
static int vb_cfg_equal(const ot_vb_cfg *a, const ot_vb_cfg *b)
{
	unsigned int i;

	if (a->max_pool_cnt != b->max_pool_cnt) {
		return 0;
	}
	for (i = 0; i < a->max_pool_cnt && i < OT_VB_MAX_COMMON_POOLS; i++) {
		if (a->common_pool[i].blk_size != b->common_pool[i].blk_size ||
			a->common_pool[i].blk_cnt != b->common_pool[i].blk_cnt) {
			return 0;
		}
	}
	return 1;
}

static int sys_setup(const Cv610PipelineRuntimeConfig *c)
{
	ot_vb_cfg vb;
	ot_vi_vpss_mode vi_vpss_mode;
	td_s32 sys_ret, vb_ret, set_ret, isp_ret;
	unsigned int i;

	/* VI -> ISP -> VPSS all online: neither the RAW capture nor a full-size
	 * YUV frame lands in VB, only VPSS's output does (VENC and the snapshot
	 * channel read it) -- the vendor's own CV610 sample gives this mode no
	 * RAW and no YUV blocks. */
	memset(&vb, 0, sizeof(vb));
	vb.max_pool_cnt = 1;
	vb.common_pool[0].blk_size =
		(td_u64)c->out_width * c->out_height * 3 / 2 + 0x4000;
	vb.common_pool[0].blk_cnt  = 4;

	/* VB/SYS state lives in the kernel and survives the process that created
	 * it, and only the creating process may tear it down — so after a crashed
	 * run vb_exit returns NOT_PERM and the config below cannot be replaced.
	 * A clean exit runs mpp_cleanup()'s vb_exit, so this pre-clean normally
	 * finds nothing to do and the set_cfg below simply takes. For online mode
	 * BUSY is not acceptable: selecting VI-online must happen after a complete,
	 * successful SYS init and before any VI pipe exists. */
	/* C does not define function-argument evaluation order. Keep the vendor
	 * API's SYS-then-VB shutdown order explicit and make each result legible. */
	/* ISP mem-init is the one piece of graph state a hard kill leaves behind:
	 * measured on the .181 bench, SIGKILL releases VB (set_cfg then succeeds)
	 * but leaves ISP[0] inited, so the next run dies at ss_mpi_isp_mem_init
	 * with 0xa01c800c "already inited". Clear it here, with the other
	 * pre-cleans and before anything of ours is registered, so a crash does
	 * not need a module reload to recover. */
	isp_ret = ss_mpi_isp_exit(VI_PIPE);
	sys_ret = ss_mpi_sys_exit();
	vb_ret = ss_mpi_vb_exit();
	printf("  pre-clean: isp_exit=0x%x sys_exit=0x%x vb_exit=0x%x\n",
		   isp_ret, sys_ret, vb_ret);

	memset(&vi_vpss_mode, 0, sizeof(vi_vpss_mode));
	for (i = 0; i < OT_VI_MAX_PIPE_NUM; i++) {
		vi_vpss_mode.mode[i] = OT_VI_OFFLINE_VPSS_OFFLINE;
	}
	vi_vpss_mode.mode[VI_PIPE] = OT_VI_ONLINE_VPSS_ONLINE;

	set_ret = ss_mpi_vb_set_cfg(&vb);
	if (set_ret != TD_SUCCESS) {
		ot_vb_cfg live;

		if (cv610_error_id(set_ret) != OT_ERR_BUSY) {
			fprintf(stderr, "FAIL ss_mpi_vb_set_cfg = 0x%x\n", set_ret);
			return -1;
		}
		/* VB is still held by an owner that never tore it down — a crashed
		 * run, since a clean exit calls vb_exit from mpp_cleanup(). Adopting
		 * it is only safe when the live pool layout is the one this mode was
		 * about to ask for. Tolerating BUSY blindly would leave every block
		 * the wrong size, and that surfaces far downstream as corruption
		 * rather than as a failed init. */
		memset(&live, 0, sizeof(live));
		if (ss_mpi_vb_get_cfg(&live) != TD_SUCCESS ||
			!vb_cfg_equal(&live, &vb)) {
			/* Points at a reboot, not a module reload. This fires only after
			 * a predecessor died abnormally, which is exactly the boot state
			 * where an unload cannot be undone: the modules will not load
			 * back, and on a video-only craft the attempt resets the SoC. */
			fprintf(stderr, "FAIL VB is held by a dead owner and its layout "
					"differs from this mode; reboot to clear it\n");
			return -1;
		}
		printf("  ok  ss_mpi_vb_set_cfg  (adopted identical live config)\n");
	} else {
		printf("  ok  ss_mpi_vb_set_cfg\n");
	}
	CV610_CHECK_OR_BUSY(ss_mpi_vb_init());
	/* Online mode can only be selected after a complete SYS init. Do not
	 * hide a half-initialized module set behind the usual BUSY tolerance. */
	CV610_CHECK(ss_mpi_sys_init());
	CV610_CHECK(ss_mpi_sys_set_vi_vpss_mode(&vi_vpss_mode));
	CV610_CHECK(ss_mpi_sys_set_vi_aiisp_mode(VI_PIPE, OT_VI_AIISP_MODE_DEFAULT));
	memset(&vi_vpss_mode, 0, sizeof(vi_vpss_mode));
	CV610_CHECK(ss_mpi_sys_get_vi_vpss_mode(&vi_vpss_mode));
	if (vi_vpss_mode.mode[VI_PIPE] != OT_VI_ONLINE_VPSS_ONLINE) {
		fprintf(stderr, "FAIL VI pipe %d mode readback = %d\n", VI_PIPE,
				vi_vpss_mode.mode[VI_PIPE]);
		return -1;
	}
	printf("  ok  VI/ISP/VPSS mode: online\n");
	return 0;
}

/* --------------------------------------------------------------- VI setup -- */

static int vi_setup(const Cv610PipelineRuntimeConfig *c)
{
	ot_vi_dev_attr  dev_attr;
	ot_vi_pipe_attr pipe_attr;

	/* Same kernel-side-state problem as VB: a previous run that died leaves
	 * the VI device enabled, and set_dev_attr then fails NOT_DISABLE. */
	ss_mpi_vi_disable_chn(VI_PIPE, VI_CHN);
	ss_mpi_vi_stop_pipe(VI_PIPE);
	ss_mpi_vi_destroy_pipe(VI_PIPE);
	ss_mpi_vi_unbind(VI_DEV, VI_PIPE);
	ss_mpi_vi_disable_dev(VI_DEV);

	memset(&dev_attr, 0, sizeof(dev_attr));
	dev_attr.intf_mode          = OT_VI_INTF_MODE_MIPI;
	dev_attr.work_mode          = OT_VI_WORK_MODE_MULTIPLEX_1;
	dev_attr.component_mask[0]  = 0xFFC00000;
	dev_attr.component_mask[1]  = 0x0;
	dev_attr.scan_mode          = OT_VI_SCAN_PROGRESSIVE;
	dev_attr.ad_chn_id[0]       = -1;
	dev_attr.ad_chn_id[1]       = -1;
	dev_attr.ad_chn_id[2]       = -1;
	dev_attr.ad_chn_id[3]       = -1;
	dev_attr.data_seq           = OT_VI_DATA_SEQ_YUYV;
	dev_attr.data_type          = OT_VI_DATA_TYPE_RAW;
	dev_attr.data_reverse       = TD_FALSE;
	dev_attr.in_size.width      = c->width;
	dev_attr.in_size.height     = c->height;
	dev_attr.data_rate          = c->data_rate_x2 ? OT_DATA_RATE_X2 : OT_DATA_RATE_X1;

	CV610_CHECK(ss_mpi_vi_set_dev_attr(VI_DEV, &dev_attr));
	CV610_CHECK(ss_mpi_vi_enable_dev(VI_DEV));
	CV610_CHECK(ss_mpi_vi_bind(VI_DEV, VI_PIPE));

	memset(&pipe_attr, 0, sizeof(pipe_attr));
	pipe_attr.pipe_bypass_mode = OT_VI_PIPE_BYPASS_NONE;
	pipe_attr.isp_bypass       = TD_FALSE;
	pipe_attr.size.width       = c->width;
	pipe_attr.size.height      = c->height;
	pipe_attr.pixel_format     = (c->raw_bit == 10) ?
								 OT_PIXEL_FORMAT_RGB_BAYER_10BPP :
								 OT_PIXEL_FORMAT_RGB_BAYER_12BPP;
	pipe_attr.compress_mode    = OT_COMPRESS_MODE_NONE;
	pipe_attr.frame_rate_ctrl.src_frame_rate = OT_VI_INVALID_FRAME_RATE;
	pipe_attr.frame_rate_ctrl.dst_frame_rate = OT_VI_INVALID_FRAME_RATE;

	CV610_CHECK(ss_mpi_vi_create_pipe(VI_PIPE, &pipe_attr));
	/* The ISP's mem_init queries pipe size from VI, so the pipe must exist. */
	CV610_CHECK(ss_mpi_vi_start_pipe(VI_PIPE));
	return 0;
}

/* The aspect crop VI applies before VPSS: the shared centre-crop rule, held
 * to the 4-pixel grid the VI pipe post-crop demands -- the driver refuses
 * anything else ("crop width should be 4 align, which is 1898", 0xa0108007,
 * hardware-checked).  The shared helper rounds to 2 because Star6E and Maruko
 * need no more.  An internal rectangle VPSS scales to video0.size, so the
 * encoded size is unaffected: IMX335 2592x1944 -> 1080p crops 2592x1456
 * instead of 2592x1458. */
static PipelinePrecropRect cv610_aspect_crop(const Cv610PipelineRuntimeConfig *c)
{
	PipelinePrecropRect r = pipeline_common_compute_precrop(
		c->width, c->height, c->out_width, c->out_height,
		c->keep_aspect ? true : false);

	if (r.w != c->width || r.h != c->height) {
		r.w = (uint16_t)(r.w & ~3u);
		r.h = (uint16_t)(r.h & ~3u);
		r.x = (uint16_t)(((c->width - r.w) / 2) & ~3u);
		r.y = (uint16_t)(((c->height - r.h) / 2) & ~3u);
	}
	return r;
}

static int vi_start_chn(const Cv610PipelineRuntimeConfig *c)
{
	ot_vi_chn_attr chn_attr;

	memset(&chn_attr, 0, sizeof(chn_attr));
	chn_attr.size.width    = c->width;
	chn_attr.size.height   = c->height;
	/* CV610 VI accepts only YVU (NV21) semiplanar: set_chn_attr rejects
	 * YUV_SEMIPLANAR_420 with 0xa0108007, and open_vi.ko's
	 * vi_check_yuv_frame_pixel_format passes only formats 37/38/52
	 * (verified on hardware 2026-07-30). NV12 exists only via VPSS. */
	chn_attr.pixel_format  = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
	chn_attr.dynamic_range = OT_DYNAMIC_RANGE_SDR8;
	chn_attr.video_format  = OT_VIDEO_FORMAT_LINEAR;
	chn_attr.compress_mode = OT_COMPRESS_MODE_NONE;
	/* Orientation is applied at the sensor, not here — same call the
	 * SigmaStar backends make, and for the same reason: the VI/VPE digital
	 * path costs bandwidth and behaves inconsistently across sensor combos,
	 * while the sensor's own H/V reverse is free.  See
	 * apply_sensor_orientation(). */
	chn_attr.mirror_en     = TD_FALSE;
	chn_attr.flip_en       = TD_FALSE;
	chn_attr.depth         = 0;
	chn_attr.frame_rate_ctrl.src_frame_rate = OT_VI_INVALID_FRAME_RATE;
	chn_attr.frame_rate_ctrl.dst_frame_rate = OT_VI_INVALID_FRAME_RATE;

	CV610_CHECK(ss_mpi_vi_set_chn_attr(VI_PIPE, VI_CHN, &chn_attr));
	/* Aspect: VPSS scales whatever VI hands it, so without this a 4:3
	 * video0.size out of a 16:9 capture is squashed rather than framed.
	 * Crop the pipe's post-ISP output to the encoded aspect first (the ISP
	 * still meters the whole capture) -- the same centre-crop
	 * rule Star6E and Maruko apply through pipeline_common_compute_precrop(),
	 * from the same shared function so the three backends cannot drift.
	 *
	 * Written UNCONDITIONALLY, including the disable.  MPP objects are
	 * kernel state on this SoC and a pipe that outlives a teardown keeps
	 * whatever crop it was last given, so "skip the call when no crop is
	 * needed" would inherit a stale rectangle from the previous run's
	 * geometry.  vi_setup() destroys the pipe before creating it precisely
	 * so that cannot happen today -- this keeps it true without depending
	 * on it.
	 *
	 * On VI, not VPSS: with VPSS online behind VI, ss_mpi_vpss_set_grp_crop()
	 * refuses every call (0xa0078007, a disabled crop included), and the
	 * VPSS channel crop is accepted but applied short -- 1440 wide at x=240
	 * came out 1200 wide (trim_width in /proc/umap/vpss), so VENC dropped
	 * every frame.  The VI channel crop is NOT_SUPPORT (0xa010800c) on
	 * this chip.  All hardware-checked on the IMX662 at 1440x1080. */
	{
		PipelinePrecropRect precrop = cv610_aspect_crop(c);
		int cropping = (precrop.w != c->width || precrop.h != c->height);
		ot_crop_info crop;

		memset(&crop, 0, sizeof(crop));
		crop.enable = cropping ? TD_TRUE : TD_FALSE;
		crop.rect.x = precrop.x;
		crop.rect.y = precrop.y;
		crop.rect.width = precrop.w;
		crop.rect.height = precrop.h;
		CV610_CHECK(ss_mpi_vi_set_pipe_post_crop(VI_PIPE, &crop));
		if (cropping)
			printf("  ok  aspect crop %ux%u+%u+%u of %ux%u -> %ux%u\n",
				(unsigned)precrop.w, (unsigned)precrop.h,
				(unsigned)precrop.x, (unsigned)precrop.y,
				c->width, c->height, c->out_width, c->out_height);
	}

	CV610_CHECK(ss_mpi_vi_enable_chn(VI_PIPE, VI_CHN));

	/* isp_mem_init asks the VI kernel export vi_get_pipe_hdr_attr() for this
	 * pipe's dynamic range.  On CV610 that export reads physical channel 0's
	 * attributes, so the channel must be configured before isp_mem_init. */
	return 0;
}

/* ------------------------------------------------------------ sensor/ISP -- */

static void            *g_sns_handle;
static ot_isp_sns_obj  *g_sns_obj;

static ot_isp_3a_alg_lib g_ae_lib  = { .id = VI_PIPE, .lib_name = "ot_ae_lib"  };
static ot_isp_3a_alg_lib g_awb_lib = { .id = VI_PIPE, .lib_name = "ot_awb_lib" };

static int sensor_setup(const Cv610Sensor *sensor, int i2c_bus)
{
	ot_isp_sns_commbus bus;

	g_sns_handle = dlopen(sensor->plugin_path, RTLD_NOW | RTLD_GLOBAL);
	if (g_sns_handle == NULL) {
		fprintf(stderr, "dlopen %s: %s\n", sensor->plugin_path, dlerror());
		return -1;
	}
	g_sns_obj = (ot_isp_sns_obj *)dlsym(g_sns_handle, sensor->obj_symbol);
	if (g_sns_obj == NULL) {
		fprintf(stderr, "dlsym %s: %s\n", sensor->obj_symbol, dlerror());
		return -1;
	}
	printf("  ok  dlopen %s -> %s @ %p\n", sensor->plugin_path,
		sensor->obj_symbol, (void *)g_sns_obj);

	if (g_sns_obj->pfn_set_bus_info != NULL) {
		bus.i2c_dev = (td_s8)i2c_bus;
		CV610_CHECK(g_sns_obj->pfn_set_bus_info(VI_PIPE, bus));
	}

	CV610_CHECK(ss_mpi_ae_register(VI_PIPE, &g_ae_lib));
	CV610_CHECK(ss_mpi_awb_register(VI_PIPE, &g_awb_lib));
	CV610_CHECK(g_sns_obj->pfn_register_callback(VI_PIPE, &g_ae_lib, &g_awb_lib));
	return 0;
}

static int isp_setup(const Cv610PipelineRuntimeConfig *c)
{
	ot_isp_pub_attr pub;
	ot_isp_bind_attr bind;

	memset(&pub, 0, sizeof(pub));
	pub.wnd_rect.x      = 0;
	pub.wnd_rect.y      = 0;
	pub.wnd_rect.width  = c->width;
	pub.wnd_rect.height = c->height;
	pub.sns_size.width  = c->width;
	pub.sns_size.height = c->height;
	pub.frame_rate      = c->fps;
	/* Unchanged by mirror/flip, and that is a measured result rather than
	 * an assumption.  The sensor driver carries a "flipping changes the
	 * Bayer start phase -- VERIFY on hardware" note, and the textbook
	 * answer is one XOR per axis into this enum (it is ordered
	 * (row_phase << 1 | col_phase): RGGB 0, GRBG 1, GBRG 2, BGGR 3).
	 *
	 * On IMX662 that is wrong: the reverse shifts the readout window with
	 * the direction, so the phase the ISP sees does not move.  A/B on the
	 * bench with mirror on, same scene, same binary but for this line —
	 * with the XOR, mean RGB went 106/98/141 -> 163/33/195, green
	 * collapsing because the ISP was demosaicing green sites as red and
	 * blue; without it, 105/96/145, which is the unmirrored frame's colour.
	 * Geometry was correct in both, so the failure looks like a white
	 * balance fault rather than an orientation one -- exactly the trap the
	 * driver comment warns about. */
	pub.bayer_format    = (ot_isp_bayer_format)c->bayer;
	pub.wdr_mode        = OT_WDR_MODE_NONE;
	pub.sns_mode        = 0;

	/* Tell the ISP which registered 3A libs and which sensor this pipe uses;
	 * without it isp_mem_init cannot resolve the sensor and fails NOT_CFG. */
	memset(&bind, 0, sizeof(bind));
	bind.sns_id  = (ot_sensor_id)c->sensor->sns_id;
	bind.ae_lib  = g_ae_lib;
	bind.awb_lib = g_awb_lib;
	CV610_CHECK(ss_mpi_isp_set_bind_attr(VI_PIPE, &bind));

	CV610_CHECK(ss_mpi_isp_mem_init(VI_PIPE));
	CV610_CHECK(ss_mpi_isp_set_pub_attr(VI_PIPE, &pub));
	CV610_CHECK(ss_mpi_isp_init(VI_PIPE));
	return 0;
}

/* Program the sensor's H/V reverse through the plugin vtable.
 *
 * Call site: immediately after isp_setup().  The gate is the plugin's i2c
 * file descriptor, which imx662_init() opens as pfn_cmos_sns_init from
 * inside ss_mpi_isp_init() — synchronously, on this thread.  Before that
 * point the driver's register writes return TD_SUCCESS against a closed fd
 * and do nothing.  It is deliberately BEFORE the ISP thread starts: applied
 * after, the first frames and the first AE/AWB statistics are gathered in
 * the old orientation and the readout then flips under a converging 3A loop.
 *
 * Written UNCONDITIONALLY, including the disable, for the reason vpss_setup()
 * gives about the crop: an external sensor chip holds whatever it was last
 * given, and more stubbornly than any MPP group.  The SoC's MIPI reset
 * happens to clear 0x3020/0x3021 on this board, but that is board wiring,
 * not a property of the sensor — both SigmaStar backends call MI_SNR_SetOrien
 * unconditionally and this now matches them.
 *
 * Non-fatal on a missing vtable entry, matching star6e_pipeline.c and
 * maruko_pipeline.c: a craft that boots upside-down beats one that does not
 * boot.
 *
 * The log says "requested", not "ok", and that wording is load-bearing.
 * pfn_mirror_flip returns td_void, the driver casts both register writes to
 * (td_void), and imx662_write_register() returns TD_SUCCESS even when the
 * i2c fd is closed — so there is no success to report at any of the three
 * layers.  Claiming one would reproduce exactly the failure this function
 * exists to abolish: an orientation that silently did nothing, with a log
 * line asserting otherwise. */
static void apply_sensor_orientation(const Cv610PipelineRuntimeConfig *c)
{
	ot_isp_sns_mirrorflip_type type;

	if (g_sns_obj == NULL || g_sns_obj->pfn_mirror_flip == NULL) {
		if (c->mirror || c->flip)
			fprintf(stderr,
				"[pipeline] WARNING: image.mirror/flip requested "
				"but the sensor plugin exposes no "
				"pfn_mirror_flip — orientation NOT applied\n");
		return;
	}

	if (c->mirror && c->flip)
		type = ISP_SNS_MIRROR_FLIP;
	else if (c->mirror)
		type = ISP_SNS_MIRROR;
	else if (c->flip)
		type = ISP_SNS_FLIP;
	else
		type = ISP_SNS_NORMAL;

	g_sns_obj->pfn_mirror_flip(VI_PIPE, type);
	printf("  ok  sensor orientation requested: mirror=%d flip=%d\n",
		c->mirror ? 1 : 0, c->flip ? 1 : 0);
}

static int configure_output_color(void)
{
	ot_isp_csc_attr csc;
	td_s32 ret;

	memset(&csc, 0, sizeof(csc));
	ret = ss_mpi_isp_get_csc_attr(VI_PIPE, &csc);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "FAIL ss_mpi_isp_get_csc_attr = 0x%x\n", ret);
		return -1;
	}
	/* Preserve the standalone streamer's device-verified output.  The VENC
	 * VUI advertises full-range BT.709, so keep the ISP CSC in agreement. */
	csc.enable = TD_TRUE;
	csc.satu = 60;
	csc.contr = 53;
	ret = ss_mpi_isp_set_csc_attr(VI_PIPE, &csc);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "FAIL ss_mpi_isp_set_csc_attr = 0x%x\n", ret);
		return -1;
	}
	printf("  output CSC: gamut=%d saturation=%u contrast=%u full-range=%d\n",
		   csc.color_gamut, csc.satu, csc.contr, !csc.limited_range_en);
	return 0;
}

static int enable_sensor_ccm(void)
{
	ot_isp_color_matrix_attr attr;
	td_s32 ret;

	memset(&attr, 0, sizeof(attr));
	ret = ss_mpi_isp_get_ccm_attr(VI_PIPE, &attr);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "FAIL ss_mpi_isp_get_ccm_attr = 0x%x\n", ret);
		return -1;
	}
	if (attr.auto_attr.ccm_tab_num < 3) {
		fprintf(stderr, "FAIL sensor supplied only %u CCM anchors\n",
				attr.auto_attr.ccm_tab_num);
		return -1;
	}
	attr.op_type = OT_OP_MODE_AUTO;
	attr.auto_attr.iso_act_en = TD_FALSE;
	attr.auto_attr.temp_act_en = TD_FALSE;
	ret = ss_mpi_isp_set_ccm_attr(VI_PIPE, &attr);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "FAIL ss_mpi_isp_set_ccm_attr = 0x%x\n", ret);
		return -1;
	}
	printf("  sensor CCM enabled: %u anchors, ISO/temperature bypass off\n",
		   attr.auto_attr.ccm_tab_num);
	return 0;
}

/* ------------------------------------------------------------------ VPSS -- */

/* VI has no scaler: its channel emits the captured geometry and nothing else.
 * VPSS is the CV610's scaling stage (the counterpart of SigmaStar's VPE SCL
 * ports), so the encoded size lives here rather than on the VI channel.  The
 * group is created even when no scaling is asked for, so the shipping 1080p
 * path and a scaled one exercise the same graph and the same teardown order. */
static int vpss_setup(const Cv610PipelineRuntimeConfig *c)
{
	ot_vpss_grp_attr grp_attr;
	ot_vpss_chn_attr chn_attr;
	ot_mpp_chn src;
	ot_mpp_chn dst;

	memset(&grp_attr, 0, sizeof(grp_attr));
	grp_attr.max_width     = c->width;
	grp_attr.max_height    = c->height;
	grp_attr.pixel_format  = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
	grp_attr.dynamic_range = OT_DYNAMIC_RANGE_SDR8;
	grp_attr.dei_mode      = OT_VPSS_DEI_MODE_OFF;
	grp_attr.frame_rate.src_frame_rate = -1;
	grp_attr.frame_rate.dst_frame_rate = -1;
	CV610_CHECK(ss_mpi_vpss_create_grp(VPSS_GRP, &grp_attr));

	memset(&chn_attr, 0, sizeof(chn_attr));
	chn_attr.width         = c->out_width;
	chn_attr.height        = c->out_height;
	/* VI accepts only YVU (NV21) on this chip; keep the whole chain on it so
	 * VENC sees one pixel format regardless of whether VPSS scaled. */
	chn_attr.pixel_format  = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
	chn_attr.dynamic_range = OT_DYNAMIC_RANGE_SDR8;
	chn_attr.video_format  = OT_VIDEO_FORMAT_LINEAR;
	chn_attr.compress_mode = OT_COMPRESS_MODE_NONE;
	/* USER, not AUTO: AUTO makes the channel follow the group's input size
	 * and set_chn_attr rejects an explicit geometry with ILLEGAL_PARAM
	 * (0xa0078007, probed on hardware).  USER is what lets this channel be
	 * a scaler.  frame_rate must stay -1/-1 for the same reason — 0/0 is
	 * rejected with the same code. */
	chn_attr.chn_mode      = OT_VPSS_CHN_MODE_USER;
	chn_attr.depth         = 0;
	chn_attr.frame_rate.src_frame_rate = -1;
	chn_attr.frame_rate.dst_frame_rate = -1;
	CV610_CHECK(ss_mpi_vpss_set_chn_attr(VPSS_GRP, VPSS_CHN, &chn_attr));
	CV610_CHECK(ss_mpi_vpss_enable_chn(VPSS_GRP, VPSS_CHN));
	CV610_CHECK(ss_mpi_vpss_start_grp(VPSS_GRP));

	src.mod_id = OT_ID_VI;
	src.dev_id = VI_PIPE;
	src.chn_id = VI_CHN;
	dst.mod_id = OT_ID_VPSS;
	dst.dev_id = VPSS_GRP;
	dst.chn_id = 0;
	CV610_CHECK(ss_mpi_sys_bind(&src, &dst));
	/* Its input is VI's output, logged with the aspect crop in vi_start_chn(). */
	printf("  ok  VPSS -> %ux%u\n", c->out_width, c->out_height);
	return 0;
}

/* Unconditional, like the VI and ISP teardown below: vpss_setup() can fail
 * at any of its steps, and a group created by an earlier step is kernel
 * state that outlives this process.  Skipping the destroy would leave grp 0
 * behind, and the next start — a respawn reloads no modules — would fail
 * create_grp with EXIST and never recover.  Each call is a no-op returning an
 * error we ignore when the object was never created. */
static void vpss_teardown(void)
{
	ot_mpp_chn src = { OT_ID_VI, VI_PIPE, VI_CHN };
	ot_mpp_chn dst = { OT_ID_VPSS, VPSS_GRP, 0 };

	(void)ss_mpi_sys_unbind(&src, &dst);
	(void)ss_mpi_vpss_stop_grp(VPSS_GRP);
	(void)ss_mpi_vpss_disable_chn(VPSS_GRP, VPSS_CHN);
	(void)ss_mpi_vpss_destroy_grp(VPSS_GRP);
}

/* --------------------------------------------------------------- teardown -- */

/* Safe after partial initialization; BackendOps calls this on init failure. */
static void mpp_cleanup(void)
{
	td_s32 sys_ret, vb_ret;

	printf("== teardown ==\n");
	/* VPSS consumes VI, so unbind and stop it before the pipe it reads from
	 * — the same producer-last rule the ISP block below follows. */
	vpss_teardown();
	/* Match the vendor sample's shutdown dependency order: stop ISP and its
	 * 3A/sensor users before dismantling the VI pipe that feeds it. */
	ss_mpi_isp_exit(VI_PIPE);
	if (g_isp_thread_ok) {
		pthread_join(g_isp_thread, NULL);
		g_isp_thread_ok = 0;
	}
	if (g_sns_obj != NULL && g_sns_obj->pfn_un_register_callback != NULL) {
		g_sns_obj->pfn_un_register_callback(VI_PIPE, &g_ae_lib, &g_awb_lib);
	}
	ss_mpi_awb_unregister(VI_PIPE, &g_awb_lib);
	ss_mpi_ae_unregister(VI_PIPE, &g_ae_lib);
	ss_mpi_vi_disable_chn(VI_PIPE, VI_CHN);
	ss_mpi_vi_stop_pipe(VI_PIPE);
	ss_mpi_vi_destroy_pipe(VI_PIPE);
	ss_mpi_vi_unbind(VI_DEV, VI_PIPE);
	ss_mpi_vi_disable_dev(VI_DEV);
	if (g_sns_handle != NULL) {
		dlclose(g_sns_handle);
		g_sns_handle = NULL;
		g_sns_obj = NULL;
	}
	sys_ret = ss_mpi_sys_exit();
	vb_ret = ss_mpi_vb_exit();
	printf("  sys_exit=0x%x vb_exit=0x%x\n", sys_ret, vb_ret);
}

int cv610_pipeline_start(const Cv610PipelineConfig *config)
{
	Cv610PipelineRuntimeConfig c;

	if (config == NULL || config->sensor == NULL) {
		return -1;
	}
	memset(&c, 0, sizeof(c));
	c.width = config->width;
	c.height = config->height;
	c.fps = (float)config->fps;
	c.lanes = config->lanes;
	c.data_rate_x2 = config->data_rate_x2;
	c.bayer = config->bayer;
	c.raw_bit = config->raw_bit;
	c.sensor_clock_hz = config->sensor_clock_hz;
	c.out_width = config->out_width;
	c.out_height = config->out_height;
	c.keep_aspect = config->keep_aspect;
	c.mirror = config->mirror ? 1 : 0;
	c.flip = config->flip ? 1 : 0;
	c.sensor = config->sensor;
	g_stop = 0;

	printf("== cv610 sys/vb ==\n");
	if (sys_setup(&c) != 0) {
		return -1;
	}
	printf("== cv610 mipi ==\n");
	if (mipi_setup(&c) != 0) {
		return -1;
	}
	printf("== cv610 vi ==\n");
	if (vi_setup(&c) != 0 || vi_start_chn(&c) != 0) {
		return -1;
	}
	printf("== cv610 sensor/isp ==\n");
	if (sensor_setup(c.sensor, config->i2c_bus) != 0 ||
		isp_setup(&c) != 0 ||
		configure_output_color() != 0) {
		return -1;
	}
	apply_sensor_orientation(&c);
	printf("== cv610 vpss ==\n");
	if (vpss_setup(&c) != 0) {
		return -1;
	}
	if (pthread_create(&g_isp_thread, NULL, isp_thread_fn, NULL) != 0) {
		fprintf(stderr, "FAIL pthread_create ISP\n");
		return -1;
	}
	g_isp_thread_ok = 1;
	if (enable_sensor_ccm() != 0) {
		return -1;
	}
	return 0;
}

int cv610_pipeline_isp_ready(void)
{
	return g_isp_thread_ok;
}

void cv610_pipeline_stop(void)
{
	g_stop = 1;
	mpp_cleanup();
}

void cv610_pipeline_request_stop(void)
{
	g_stop = 1;
}

int cv610_pipeline_stop_requested(void)
{
	return g_stop != 0;
}
