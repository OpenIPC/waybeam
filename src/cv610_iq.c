/*
 * cv610_iq.c — runtime IQ control for the Hi3516CV610 ISP.
 *
 * The IMX662 sensor plugin seeds this ISP once, at pipe start, through
 * cmos_get_isp_default() (PR #229).  This module exposes the same blocks as
 * live knobs so a value can be corrected without a cross-compile, a plugin
 * deploy and a reboot.
 *
 * Two things differ from star6e_iq.c and make this the simpler backend:
 *
 *   - No dlopen.  The CV610 backend links ss_mpi directly, so the attribute
 *     structs are the SDK's own types.  Every field offset comes from
 *     offsetof(), not from a hand-computed constant that can drift when the
 *     SDK header changes.
 *   - The table is self-describing.  query() emits a "_schema" block so the
 *     WebUI renders the knob list this file declares, rather than carrying a
 *     second copy of it.
 *
 * Ranges in the field table are the "Range:" comments from the SDK's
 * ot_common_isp.h.  A value outside its range is REJECTED, not clamped: the
 * HTTP layer echoes the requested value back, so silently substituting a
 * different one would make both the response and the log a record of
 * something that never happened.
 */

#include "cv610_iq.h"
#include "cv610_pipeline.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ot_type.h"
#include "ot_common.h"
#include "ot_common_video.h"
#include "ot_common_isp.h"
#include "ss_mpi_isp.h"
#include "ss_mpi_awb.h"
#include "ss_mpi_ae.h"

/* The backend runs a single VI pipe; cv610_pipeline.c calls it VI_PIPE 0. */
#define CV610_IQ_PIPE 0

/* td_bool and ot_op_mode are enums, so both are int-sized.  The field table
 * encodes them as FT_S32; assert that rather than trusting it. */
typedef char cv610_iq_bool_is_int[(sizeof(td_bool) == 4) ? 1 : -1];
typedef char cv610_iq_mode_is_int[(sizeof(ot_op_mode) == 4) ? 1 : -1];

typedef enum {
	FT_U8,
	FT_U16,
	FT_S32,   /* also carries td_bool and ot_op_mode */
	FT_U32,   /* AE gain/exposure ranges; needs 64-bit to stay exact */
} Cv610IqType;

typedef struct {
	const char *name;
	Cv610IqType type;
	uint16_t    offset;   /* offsetof() into the group's attr struct */
	uint16_t    count;    /* 1 = scalar, N = array */
	/* 64-bit because td_u32 fields (AE gain and exposure ranges) do not fit
	 * an int32_t -- a driver default of 0xFFFFFFFF would read back as -1. */
	int64_t     min;
	int64_t     max;
	/* Which half of the block this value lives in.  A manual_attr value is
	 * ignored while the block runs its auto curve and vice versa, and an
	 * ignored write reads exactly like a broken setter — so a write to a
	 * domain also selects it.  F_DIRECT fields sit outside both halves. */
	uint8_t     domain;
} Cv610IqField;

typedef struct {
	const char *name;
	int       (*get)(void *attr);
	int       (*set)(const void *attr);
	int32_t     op_type_offset;  /* -1 when the group has no op_type */
	const Cv610IqField *fields;
	uint16_t    field_count;
} Cv610IqGroup;

/* domain values.  Every field here is writable; these say only which op_type
 * the value is read under, and therefore which one a write must select. */
#define F_DIRECT 0   /* outside manual_attr/auto_attr; op_type irrelevant */
#define F_MANUAL 1   /* under manual_attr; a write selects op_type = manual */
#define F_AUTO   2   /* under auto_attr;   a write selects op_type = auto   */

/* ── MPI wrappers ───────────────────────────────────────────────────────
 * Typed one-liners rather than casting function pointers: converting a
 * function pointer to a different signature and calling through it is
 * undefined behaviour, and void * -> struct * is a plain implicit
 * conversion. */

#define IQ_ACCESSORS(group, getter, setter)                                \
	static int iq_get_##group(void *a) { return getter(CV610_IQ_PIPE, a); } \
	static int iq_set_##group(const void *a) { return setter(CV610_IQ_PIPE, a); }

IQ_ACCESSORS(saturation,
	ss_mpi_isp_get_saturation_attr, ss_mpi_isp_set_saturation_attr)
IQ_ACCESSORS(color_tone,
	ss_mpi_isp_get_color_tone_attr, ss_mpi_isp_set_color_tone_attr)
IQ_ACCESSORS(csc,
	ss_mpi_isp_get_csc_attr, ss_mpi_isp_set_csc_attr)
IQ_ACCESSORS(ccm,
	ss_mpi_isp_get_ccm_attr, ss_mpi_isp_set_ccm_attr)
IQ_ACCESSORS(wb,
	ss_mpi_isp_get_wb_attr, ss_mpi_isp_set_wb_attr)
IQ_ACCESSORS(sharpen,
	ss_mpi_isp_get_sharpen_attr, ss_mpi_isp_set_sharpen_attr)
IQ_ACCESSORS(nr,
	ss_mpi_isp_get_nr_attr, ss_mpi_isp_set_nr_attr)
IQ_ACCESSORS(drc,
	ss_mpi_isp_get_drc_attr, ss_mpi_isp_set_drc_attr)
IQ_ACCESSORS(ldci,
	ss_mpi_isp_get_ldci_attr, ss_mpi_isp_set_ldci_attr)
IQ_ACCESSORS(dehaze,
	ss_mpi_isp_get_dehaze_attr, ss_mpi_isp_set_dehaze_attr)
IQ_ACCESSORS(ca,
	ss_mpi_isp_get_ca_attr, ss_mpi_isp_set_ca_attr)
IQ_ACCESSORS(exposure,
	ss_mpi_isp_get_exposure_attr, ss_mpi_isp_set_exposure_attr)

#undef IQ_ACCESSORS

/* ── Field tables ───────────────────────────────────────────────────────
 * Ranges are the SDK header's documented limits.  Fields the CV610 does not
 * support (motion sharpen, tnr, dering, cp LUT, radial crop) are omitted
 * rather than exposed as knobs that do nothing. */

#define OFS(t, m) ((uint16_t)offsetof(t, m))

static const Cv610IqField f_saturation[] = {
	{ "op_type",           FT_S32, OFS(ot_isp_saturation_attr, op_type),
		1, 0, 1, F_DIRECT },
	{ "manual.saturation", FT_U8,  OFS(ot_isp_saturation_attr, manual_attr.saturation),
		1, 0, 255, F_MANUAL },
	/* Indexed by AGC bucket; 128 == nominal.  This is the curve the sensor
	 * plugin's g_imx662_awb_agc_table seeds. */
	{ "auto.sat",          FT_U8,  OFS(ot_isp_saturation_attr, auto_attr.sat),
		OT_ISP_AUTO_ISO_NUM, 0, 255, F_AUTO },
};

static const Cv610IqField f_color_tone[] = {
	{ "red_cast_gain",   FT_U16, OFS(ot_isp_color_tone_attr, red_cast_gain),
		1, 256, 384, F_DIRECT },
	{ "green_cast_gain", FT_U16, OFS(ot_isp_color_tone_attr, green_cast_gain),
		1, 256, 384, F_DIRECT },
	{ "blue_cast_gain",  FT_U16, OFS(ot_isp_color_tone_attr, blue_cast_gain),
		1, 256, 384, F_DIRECT },
};

static const Cv610IqField f_csc[] = {
	{ "enable",           FT_S32, OFS(ot_isp_csc_attr, enable), 1, 0, 1, F_DIRECT },
	{ "hue",              FT_U8,  OFS(ot_isp_csc_attr, hue),    1, 0, 100, F_DIRECT },
	{ "luma",             FT_U8,  OFS(ot_isp_csc_attr, luma),   1, 0, 100, F_DIRECT },
	{ "contr",            FT_U8,  OFS(ot_isp_csc_attr, contr),  1, 0, 100, F_DIRECT },
	{ "satu",             FT_U8,  OFS(ot_isp_csc_attr, satu),   1, 0, 100, F_DIRECT },
	{ "limited_range_en", FT_S32, OFS(ot_isp_csc_attr, limited_range_en),
		1, 0, 1, F_DIRECT },
};

static const Cv610IqField f_ccm[] = {
	{ "op_type",          FT_S32, OFS(ot_isp_color_matrix_attr, op_type),
		1, 0, 1, F_DIRECT },
	{ "manual.sat_en",    FT_S32, OFS(ot_isp_color_matrix_attr, manual_attr.sat_en),
		1, 0, 1, F_MANUAL },
	{ "manual.ccm",       FT_U16, OFS(ot_isp_color_matrix_attr, manual_attr.ccm),
		OT_ISP_CCM_MATRIX_SIZE, 0, 65535, F_MANUAL },
	/* cv610_pipeline.c's enable_sensor_ccm() clears both of these so the
	 * auto CCM never bypasses; they are surfaced to make that visible. */
	{ "auto.iso_act_en",  FT_S32, OFS(ot_isp_color_matrix_attr, auto_attr.iso_act_en),
		1, 0, 1, F_AUTO },
	{ "auto.temp_act_en", FT_S32, OFS(ot_isp_color_matrix_attr, auto_attr.temp_act_en),
		1, 0, 1, F_AUTO },
};

static const Cv610IqField f_wb[] = {
	{ "bypass",         FT_S32, OFS(ot_isp_wb_attr, bypass),  1, 0, 1, F_DIRECT },
	{ "op_type",        FT_S32, OFS(ot_isp_wb_attr, op_type), 1, 0, 1, F_DIRECT },
	{ "manual.r_gain",  FT_U16, OFS(ot_isp_wb_attr, manual_attr.r_gain),
		1, 0, 4095, F_MANUAL },
	{ "manual.gr_gain", FT_U16, OFS(ot_isp_wb_attr, manual_attr.gr_gain),
		1, 0, 4095, F_MANUAL },
	{ "manual.gb_gain", FT_U16, OFS(ot_isp_wb_attr, manual_attr.gb_gain),
		1, 0, 4095, F_MANUAL },
	{ "manual.b_gain",  FT_U16, OFS(ot_isp_wb_attr, manual_attr.b_gain),
		1, 0, 4095, F_MANUAL },
};

static const Cv610IqField f_sharpen[] = {
	{ "enable",                  FT_S32, OFS(ot_isp_sharpen_attr, enable),
		1, 0, 1, F_DIRECT },
	{ "op_type",                 FT_S32, OFS(ot_isp_sharpen_attr, op_type),
		1, 0, 1, F_DIRECT },
	{ "manual.texture_strength", FT_U16, OFS(ot_isp_sharpen_attr, manual_attr.texture_strength),
		OT_ISP_SHARPEN_GAIN_NUM, 0, 4095, F_MANUAL },
	{ "manual.edge_strength",    FT_U16, OFS(ot_isp_sharpen_attr, manual_attr.edge_strength),
		OT_ISP_SHARPEN_GAIN_NUM, 0, 4095, F_MANUAL },
	{ "manual.texture_freq",     FT_U16, OFS(ot_isp_sharpen_attr, manual_attr.texture_freq),
		1, 0, 4095, F_MANUAL },
	{ "manual.edge_freq",        FT_U16, OFS(ot_isp_sharpen_attr, manual_attr.edge_freq),
		1, 0, 4095, F_MANUAL },
	{ "manual.over_shoot",       FT_U8,  OFS(ot_isp_sharpen_attr, manual_attr.over_shoot),
		1, 0, 127, F_MANUAL },
	{ "manual.under_shoot",      FT_U8,  OFS(ot_isp_sharpen_attr, manual_attr.under_shoot),
		1, 0, 127, F_MANUAL },
	{ "manual.shoot_sup_strength", FT_U8, OFS(ot_isp_sharpen_attr, manual_attr.shoot_sup_strength),
		1, 0, 255, F_MANUAL },
	{ "manual.detail_ctrl",      FT_U8,  OFS(ot_isp_sharpen_attr, manual_attr.detail_ctrl),
		1, 0, 255, F_MANUAL },
	{ "manual.edge_filt_strength", FT_U8, OFS(ot_isp_sharpen_attr, manual_attr.edge_filt_strength),
		1, 0, 63, F_MANUAL },
	{ "manual.max_sharp_gain",   FT_U16, OFS(ot_isp_sharpen_attr, manual_attr.max_sharp_gain),
		1, 0, 2047, F_MANUAL },
	{ "manual.luma_wgt",         FT_U8,  OFS(ot_isp_sharpen_attr, manual_attr.luma_wgt),
		OT_ISP_SHARPEN_LUMA_NUM, 0, 31, F_MANUAL },
};

/* Bayer NR.  Only the switches are exposed: the strength LUTs are the
 * sc450ai-derived tables PR #229 borrowed, and re-tuning those is a
 * calibration exercise, not a slider. */
static const Cv610IqField f_nr[] = {
	{ "enable",  FT_S32, OFS(ot_isp_nr_attr, enable),  1, 0, 1, F_DIRECT },
	{ "op_type", FT_S32, OFS(ot_isp_nr_attr, op_type), 1, 0, 1, F_DIRECT },
	/* Motion-detect temporal NR -- CV610's variant of the tnr_en slot, which
	 * this chip does not have.  Temporal filtering is the strongest tool
	 * against high-gain grain because the noise is uncorrelated frame to
	 * frame while the scene is not; its cost is smearing on motion. */
	{ "md_en",   FT_S32, OFS(ot_isp_nr_attr, md_en),   1, 0, 1, F_DIRECT },
	/* "Strength of reserving the random noise according to luma" -- so LOWER
	 * values filter harder.  This is what g_cmos_noise_calibration's fit
	 * ultimately drives, and that fit is sc450ai's, not IMX662's.  Until a
	 * real photon-transfer calibration exists for this sensor, this is the
	 * honest way to correct it: by eye, against the live image. */
	{ "coring_ratio", FT_U16, OFS(ot_isp_nr_attr, coring_ratio),
		OT_ISP_BAYERNR_LUT_LENGTH, 0, 1023, F_DIRECT },
};

static const Cv610IqField f_drc[] = {
	{ "enable",             FT_S32, OFS(ot_isp_drc_attr, enable),  1, 0, 1, F_DIRECT },
	{ "op_type",            FT_S32, OFS(ot_isp_drc_attr, op_type), 1, 0, 1, F_DIRECT },
	{ "manual.strength",    FT_U16, OFS(ot_isp_drc_attr, manual_attr.strength),
		1, 0, 1023, F_MANUAL },
	{ "auto.strength",      FT_U16, OFS(ot_isp_drc_attr, auto_attr.strength),
		1, 0, 1023, F_AUTO },
	{ "auto.strength_max",  FT_U16, OFS(ot_isp_drc_attr, auto_attr.strength_max),
		1, 0, 1023, F_AUTO },
	{ "auto.strength_min",  FT_U16, OFS(ot_isp_drc_attr, auto_attr.strength_min),
		1, 0, 1023, F_AUTO },
	{ "contrast_ctrl",      FT_U8,  OFS(ot_isp_drc_attr, contrast_ctrl),
		1, 0, 15, F_DIRECT },
	{ "detail_adjust_coef", FT_U8,  OFS(ot_isp_drc_attr, detail_adjust_coef),
		1, 0, 15, F_DIRECT },
	{ "bright_gain_limit",  FT_U8,  OFS(ot_isp_drc_attr, bright_gain_limit),
		1, 0, 15, F_DIRECT },
	{ "dark_gain_limit_luma", FT_U8, OFS(ot_isp_drc_attr, dark_gain_limit_luma),
		1, 0, 133, F_DIRECT },
};

static const Cv610IqField f_ldci[] = {
	{ "enable",           FT_S32, OFS(ot_isp_ldci_attr, enable),  1, 0, 1, F_DIRECT },
	{ "op_type",          FT_S32, OFS(ot_isp_ldci_attr, op_type), 1, 0, 1, F_DIRECT },
	{ "gauss_lpf_sigma",  FT_U8,  OFS(ot_isp_ldci_attr, gauss_lpf_sigma),
		1, 1, 255, F_DIRECT },
	{ "manual.blc_ctrl",  FT_U16, OFS(ot_isp_ldci_attr, manual_attr.blc_ctrl),
		1, 0, 511, F_MANUAL },
	{ "tpr_incr_coef",    FT_U16, OFS(ot_isp_ldci_attr, tpr_incr_coef),
		1, 0, 256, F_DIRECT },
	{ "tpr_decr_coef",    FT_U16, OFS(ot_isp_ldci_attr, tpr_decr_coef),
		1, 0, 256, F_DIRECT },
};

static const Cv610IqField f_dehaze[] = {
	{ "enable",          FT_S32, OFS(ot_isp_dehaze_attr, enable),  1, 0, 1, F_DIRECT },
	{ "op_type",         FT_S32, OFS(ot_isp_dehaze_attr, op_type), 1, 0, 1, F_DIRECT },
	{ "manual.strength", FT_U8,  OFS(ot_isp_dehaze_attr, manual_attr.strength),
		1, 0, 255, F_MANUAL },
	{ "auto.strength",   FT_U8,  OFS(ot_isp_dehaze_attr, auto_attr.strength),
		1, 0, 255, F_AUTO },
};

static const Cv610IqField f_ca[] = {
	{ "enable", FT_S32, OFS(ot_isp_ca_attr, enable), 1, 0, 1, F_DIRECT },
};

/* AE.  Gains are 22.10 fixed point, so 1024 == 1x.  The ceilings are what
 * stop the sensor climbing into the regime where the frame is mostly
 * amplified noise -- which is where sharpening then multiplies the bitrate.
 * Exposure time is in microseconds and is the better lever of the two: it
 * collects photons rather than amplifying them, at the cost of frame rate.
 *
 * These sit under auto_attr, so writing one selects AE auto mode -- which is
 * the mode a ceiling is meant to apply in. */
static const Cv610IqField f_exposure[] = {
	{ "bypass",  FT_S32, OFS(ot_isp_exposure_attr, bypass),  1, 0, 1, F_DIRECT },
	{ "op_type", FT_S32, OFS(ot_isp_exposure_attr, op_type), 1, 0, 1, F_DIRECT },
	{ "ae_run_interval", FT_U8, OFS(ot_isp_exposure_attr, ae_run_interval),
		1, 1, 255, F_DIRECT },
	{ "auto.exp_time_max",  FT_U32,
		OFS(ot_isp_exposure_attr, auto_attr.exp_time_range.max),
		1, 0, 4294967295LL, F_AUTO },
	{ "auto.exp_time_min",  FT_U32,
		OFS(ot_isp_exposure_attr, auto_attr.exp_time_range.min),
		1, 0, 4294967295LL, F_AUTO },
	{ "auto.a_gain_max",    FT_U32,
		OFS(ot_isp_exposure_attr, auto_attr.a_gain_range.max),
		1, 1024, 4294967295LL, F_AUTO },
	{ "auto.d_gain_max",    FT_U32,
		OFS(ot_isp_exposure_attr, auto_attr.d_gain_range.max),
		1, 1024, 4294967295LL, F_AUTO },
	{ "auto.ispd_gain_max", FT_U32,
		OFS(ot_isp_exposure_attr, auto_attr.ispd_gain_range.max),
		1, 1024, 262144, F_AUTO },
	/* The single cap that bounds the product of all three. */
	{ "auto.sys_gain_max",  FT_U32,
		OFS(ot_isp_exposure_attr, auto_attr.sys_gain_range.max),
		1, 1024, 4294967295LL, F_AUTO },
	{ "auto.compensation",  FT_U8,
		OFS(ot_isp_exposure_attr, auto_attr.compensation), 1, 0, 255, F_AUTO },
	{ "auto.speed",         FT_U8,
		OFS(ot_isp_exposure_attr, auto_attr.speed),        1, 0, 255, F_AUTO },
	{ "auto.tolerance",     FT_U8,
		OFS(ot_isp_exposure_attr, auto_attr.tolerance),    1, 0, 255, F_AUTO },
};

#undef OFS

#define GROUP(n, ops) \
	{ #n, iq_get_##n, iq_set_##n, ops, f_##n, \
	  (uint16_t)(sizeof(f_##n) / sizeof(f_##n[0])) }

static const Cv610IqGroup g_groups[] = {
	GROUP(saturation,
		(int32_t)offsetof(ot_isp_saturation_attr, op_type)),
	GROUP(color_tone, -1),
	GROUP(csc, -1),
	GROUP(ccm,
		(int32_t)offsetof(ot_isp_color_matrix_attr, op_type)),
	GROUP(wb,
		(int32_t)offsetof(ot_isp_wb_attr, op_type)),
	GROUP(sharpen,
		(int32_t)offsetof(ot_isp_sharpen_attr, op_type)),
	GROUP(nr,
		(int32_t)offsetof(ot_isp_nr_attr, op_type)),
	GROUP(drc,
		(int32_t)offsetof(ot_isp_drc_attr, op_type)),
	GROUP(ldci,
		(int32_t)offsetof(ot_isp_ldci_attr, op_type)),
	GROUP(dehaze,
		(int32_t)offsetof(ot_isp_dehaze_attr, op_type)),
	GROUP(ca, -1),
	GROUP(exposure,
		(int32_t)offsetof(ot_isp_exposure_attr, op_type)),
};

#undef GROUP

#define NUM_GROUPS (sizeof(g_groups) / sizeof(g_groups[0]))

/* Largest attribute struct, with the right alignment for every member. */
typedef union {
	ot_isp_saturation_attr   saturation;
	ot_isp_color_tone_attr   color_tone;
	ot_isp_csc_attr          csc;
	ot_isp_color_matrix_attr ccm;
	ot_isp_wb_attr           wb;
	ot_isp_sharpen_attr      sharpen;
	ot_isp_nr_attr           nr;
	ot_isp_drc_attr          drc;
	ot_isp_ldci_attr         ldci;
	ot_isp_dehaze_attr       dehaze;
	ot_isp_ca_attr           ca;
	ot_isp_exposure_attr     exposure;
} Cv610IqAttr;

/* Guards the shared scratch attribute and the static emit buffer below —
 * nothing more.  It does NOT serialize the ss_mpi_isp_* calls themselves:
 * cv610_pipeline.c's ISP thread never takes this lock, and MPI-level
 * serialization is the SDK's own responsibility. */
static pthread_mutex_t g_iq_mutex = PTHREAD_MUTEX_INITIALIZER;
static Cv610IqAttr     g_attr;

/* ── Field access ───────────────────────────────────────────────────────── */

static size_t field_elem_size(Cv610IqType t)
{
	switch (t) {
	case FT_U8:  return 1;
	case FT_U16: return 2;
	case FT_S32:
	case FT_U32: return 4;
	}
	return 4;
}

static int64_t field_read(const void *attr, const Cv610IqField *f, uint16_t idx)
{
	const uint8_t *p = (const uint8_t *)attr + f->offset +
		idx * field_elem_size(f->type);

	switch (f->type) {
	case FT_U8:  return (int64_t)*p;
	case FT_U16: { uint16_t v; memcpy(&v, p, sizeof(v)); return (int64_t)v; }
	case FT_S32: { int32_t  v; memcpy(&v, p, sizeof(v)); return (int64_t)v; }
	case FT_U32: { uint32_t v; memcpy(&v, p, sizeof(v)); return (int64_t)v; }
	}
	return 0;
}

/* Callers must range-check first; cv610_iq_set() is the only caller and
 * rejects out-of-range values rather than silently substituting one. */
static void field_write(void *attr, const Cv610IqField *f, uint16_t idx, int64_t val)
{
	uint8_t *p = (uint8_t *)attr + f->offset + idx * field_elem_size(f->type);

	switch (f->type) {
	case FT_U8:  *p = (uint8_t)val; return;
	case FT_U16: { uint16_t v = (uint16_t)val; memcpy(p, &v, sizeof(v)); return; }
	case FT_S32: { int32_t  v = (int32_t)val;  memcpy(p, &v, sizeof(v)); return; }
	case FT_U32: { uint32_t v = (uint32_t)val; memcpy(p, &v, sizeof(v)); return; }
	}
}

/* ── JSON emission ──────────────────────────────────────────────────────── */

#define JSON_CLAMP(p, sz) do { \
	if ((p) >= (int)(sz)) (p) = (int)(sz) - 1; } while (0)

#define JSON_PUT(buf, pos, sz, ...) do { \
	(pos) += snprintf((buf) + (pos), (sz) - (size_t)(pos), __VA_ARGS__); \
	JSON_CLAMP(pos, sz); } while (0)

static int emit_group_values(char *buf, size_t sz, int pos,
	const Cv610IqGroup *g, const void *attr)
{
	for (uint16_t i = 0; i < g->field_count; i++) {
		const Cv610IqField *f = &g->fields[i];

		JSON_PUT(buf, pos, sz, "%s\"%s\":", i ? "," : "", f->name);
		if (f->count == 1) {
			JSON_PUT(buf, pos, sz, "%lld",
				(long long)field_read(attr, f, 0));
			continue;
		}
		JSON_PUT(buf, pos, sz, "[");
		for (uint16_t e = 0; e < f->count; e++)
			JSON_PUT(buf, pos, sz, "%s%lld",
				e ? "," : "", (long long)field_read(attr, f, e));
		JSON_PUT(buf, pos, sz, "]");
	}
	return pos;
}

/* The WebUI builds its knob list from this, so the table here stays the only
 * copy of the field set. */
static int emit_schema(char *buf, size_t sz, int pos)
{
	JSON_PUT(buf, pos, sz, "\"_schema\":[");
	for (size_t i = 0; i < NUM_GROUPS; i++) {
		const Cv610IqGroup *g = &g_groups[i];

		JSON_PUT(buf, pos, sz, "%s{\"name\":\"%s\",\"fields\":[",
			i ? "," : "", g->name);
		for (uint16_t f = 0; f < g->field_count; f++) {
			const Cv610IqField *fd = &g->fields[f];
			JSON_PUT(buf, pos, sz,
				"%s{\"name\":\"%s\",\"count\":%u,"
				"\"min\":%lld,\"max\":%lld,\"domain\":\"%s\"}",
				f ? "," : "", fd->name,
				(unsigned)fd->count, (long long)fd->min, (long long)fd->max,
				fd->domain == F_MANUAL ? "manual" :
				fd->domain == F_AUTO   ? "auto" : "direct");
		}
		JSON_PUT(buf, pos, sz, "]}");
	}
	JSON_PUT(buf, pos, sz, "]");
	return pos;
}

/* Read-only: which ISP blocks the hardware is bypassing right now.  This is
 * the control for "did enabling that block actually reach silicon" — a
 * seeded-but-bypassed block looks identical to one that was never seeded. */
static int emit_module_ctrl(char *buf, size_t sz, int pos)
{
	ot_isp_module_ctrl mod;
	int ret;

	memset(&mod, 0, sizeof(mod));
	ret = ss_mpi_isp_get_module_ctrl(CV610_IQ_PIPE, &mod);
	JSON_PUT(buf, pos, sz, "\"module_ctrl\":{\"ret\":%d", ret);
	if (ret == 0) {
		JSON_PUT(buf, pos, sz,
			",\"bypass\":{\"anti_false_color\":%u,\"crosstalk\":%u,"
			"\"dpc\":%u,\"nr\":%u,\"dehaze\":%u,\"wb_gain\":%u,"
			"\"mesh_shading\":%u,\"drc\":%u,\"demosaic\":%u,"
			"\"color_matrix\":%u,\"gamma\":%u,\"ca\":%u,\"csc\":%u,"
			"\"sharpen\":%u,\"cac\":%u,\"ldci\":%u}",
			(unsigned)mod.bit_bypass_anti_false_color,
			(unsigned)mod.bit_bypass_crosstalk_removal,
			(unsigned)mod.bit_bypass_dpc,
			(unsigned)mod.bit_bypass_nr,
			(unsigned)mod.bit_bypass_dehaze,
			(unsigned)mod.bit_bypass_wb_gain,
			(unsigned)mod.bit_bypass_mesh_shading,
			(unsigned)mod.bit_bypass_drc,
			(unsigned)mod.bit_bypass_demosaic,
			(unsigned)mod.bit_bypass_color_matrix,
			(unsigned)mod.bit_bypass_gamma,
			(unsigned)mod.bit_bypass_ca,
			(unsigned)mod.bit_bypass_csc,
			(unsigned)mod.bit_bypass_sharpen,
			(unsigned)mod.bit_bypass_cac,
			(unsigned)mod.bit_bypass_ldci);
	}
	JSON_PUT(buf, pos, sz, "}");
	return pos;
}

char *cv610_iq_query(void)
{
	static char buf[24576];
	int pos = 0;
	char *result;

	if (!cv610_pipeline_isp_ready())
		return NULL;

	pthread_mutex_lock(&g_iq_mutex);

	JSON_PUT(buf, pos, sizeof(buf), "{\"ok\":true,\"data\":{");
	pos = emit_schema(buf, sizeof(buf), pos);

	for (size_t i = 0; i < NUM_GROUPS; i++) {
		const Cv610IqGroup *g = &g_groups[i];
		int ret;

		memset(&g_attr, 0, sizeof(g_attr));
		ret = g->get(&g_attr);

		JSON_PUT(buf, pos, sizeof(buf), ",\"%s\":{\"ret\":%d",
			g->name, ret);
		if (ret == 0) {
			JSON_PUT(buf, pos, sizeof(buf), ",\"fields\":{");
			pos = emit_group_values(buf, sizeof(buf), pos, g, &g_attr);
			JSON_PUT(buf, pos, sizeof(buf), "}");
		}
		JSON_PUT(buf, pos, sizeof(buf), "}");
	}

	JSON_PUT(buf, pos, sizeof(buf), ",");
	pos = emit_module_ctrl(buf, sizeof(buf), pos);
	JSON_PUT(buf, pos, sizeof(buf), "}}");

	/* JSON_PUT clamps at the buffer end, so an overflow would serve
	 * syntactically broken JSON and the page would fail to parse it with no
	 * clue why.  Fail the request instead; the caller answers 500. */
	if (pos >= (int)sizeof(buf) - 1) {
		fprintf(stderr, "[cv610-iq] query truncated at %d bytes; "
			"grow buf[] past %zu\n", pos, sizeof(buf));
		pthread_mutex_unlock(&g_iq_mutex);
		return NULL;
	}

	result = strdup(buf);
	pthread_mutex_unlock(&g_iq_mutex);
	return result;
}

/* ── Set ────────────────────────────────────────────────────────────────── */

/* strtoll with the whole-token check the caller needs: "12abc" and "" are
 * rejected rather than silently read as 12 and 0.  64-bit because the AE
 * gain/exposure fields are td_u32 and a legitimate value can exceed INT32_MAX.
 *
 * 64-bit because the AE gain and exposure fields are td_u32 and a legitimate
 * value can exceed INT32_MAX; overflow is caught by errno == ERANGE from
 * strtoll, which is the only portable check once the value type is as wide as
 * the return type. */
static int parse_i64(const char *s, const char **end, int64_t *out)
{
	char *stop = NULL;
	long long v;

	while (*s == ' ')
		s++;
	if (*s == '\0')
		return -1;
	errno = 0;
	v = strtoll(s, &stop, 10);
	if (stop == s)
		return -1;
	if (errno == ERANGE)
		return -1;
	while (*stop == ' ')
		stop++;
	if (*stop != '\0' && *stop != ',')
		return -1;
	*out = (int64_t)v;
	*end = stop;
	return 0;
}

int cv610_iq_set(const char *param, const char *value)
{
	const Cv610IqGroup *group = NULL;
	const Cv610IqField *field = NULL;
	char group_name[32];
	const char *field_name;
	const char *dot;
	size_t glen;
	int ret;
	int rc = -1;

	if (!param || !value)
		return -1;
	if (!cv610_pipeline_isp_ready()) {
		fprintf(stderr, "[cv610-iq] ISP is not running\n");
		return -1;
	}

	/* Group and field split on the FIRST dot: field names carry their own
	 * dot ("manual.saturation"), which is what makes the auto/manual half
	 * of the struct visible in the name. */
	dot = strchr(param, '.');
	if (!dot) {
		fprintf(stderr, "[cv610-iq] %s: expected <group>.<field>\n", param);
		return -1;
	}
	glen = (size_t)(dot - param);
	if (glen == 0 || glen >= sizeof(group_name)) {
		fprintf(stderr, "[cv610-iq] %s: bad group name\n", param);
		return -1;
	}
	memcpy(group_name, param, glen);
	group_name[glen] = '\0';
	field_name = dot + 1;

	for (size_t i = 0; i < NUM_GROUPS; i++) {
		if (strcmp(g_groups[i].name, group_name) == 0) {
			group = &g_groups[i];
			break;
		}
	}
	if (!group) {
		fprintf(stderr, "[cv610-iq] unknown group: %s\n", group_name);
		return -1;
	}
	for (uint16_t i = 0; i < group->field_count; i++) {
		if (strcmp(group->fields[i].name, field_name) == 0) {
			field = &group->fields[i];
			break;
		}
	}
	if (!field) {
		fprintf(stderr, "[cv610-iq] %s: unknown field: %s\n",
			group_name, field_name);
		return -1;
	}

	pthread_mutex_lock(&g_iq_mutex);

	/* Read-modify-write: the attribute structs carry far more state than
	 * this table describes, and a zeroed struct would wipe the sensor
	 * plugin's seeds. */
	memset(&g_attr, 0, sizeof(g_attr));
	ret = group->get(&g_attr);
	if (ret != 0) {
		fprintf(stderr, "[cv610-iq] %s: get failed: 0x%x\n",
			group_name, (unsigned)ret);
		goto out;
	}

	if (field->count == 1) {
		int64_t v;
		const char *end;
		if (parse_i64(value, &end, &v) != 0 || *end == ',') {
			fprintf(stderr, "[cv610-iq] %s: expected one integer\n",
				param);
			goto out;
		}
		if (v < field->min || v > field->max) {
			fprintf(stderr, "[cv610-iq] %s: %lld out of range [%lld, %lld]\n",
				param, (long long)v, (long long)field->min,
				(long long)field->max);
			goto out;
		}
		field_write(&g_attr, field, 0, v);
	} else {
		const char *p = value;
		const char *end = value;
		uint16_t n = 0;
		while (n < field->count) {
			int64_t v;
			if (parse_i64(p, &end, &v) != 0)
				break;
			if (v < field->min || v > field->max) {
				fprintf(stderr,
					"[cv610-iq] %s: element %u = %lld out of range "
					"[%lld, %lld]\n",
					param, (unsigned)n, (long long)v,
					(long long)field->min, (long long)field->max);
				goto out;
			}
			field_write(&g_attr, field, n, v);
			n++;
			if (*end != ',')
				break;
			p = end + 1;
		}
		/* Require exactly the declared count, in both directions.  A short
		 * list leaves the tail at whatever the ISP held; a long one drops
		 * its excess.  Either way the curve applied is not the curve asked
		 * for, and a partly-applied curve reads as a successful set. */
		if (n != field->count || *end != '\0') {
			fprintf(stderr, "[cv610-iq] %s: expected exactly %u values\n",
				param, (unsigned)field->count);
			goto out;
		}
	}

	/* A value is inert unless the block is running the domain it lives in,
	 * so writing one selects that domain.  One rule, both directions:
	 * without the auto half, restoring an ISO curve after any manual write
	 * stores the numbers, reads them back, and changes nothing. */
	if (field->domain != F_DIRECT && group->op_type_offset >= 0) {
		int32_t mode = (field->domain == F_MANUAL) ?
			OT_OP_MODE_MANUAL : OT_OP_MODE_AUTO;
		int32_t was = 0;

		memcpy(&was, (uint8_t *)&g_attr + group->op_type_offset,
			sizeof(was));
		memcpy((uint8_t *)&g_attr + group->op_type_offset,
			&mode, sizeof(mode));
		/* Say so when it actually moves.  Selecting a domain is a side effect
		 * of the write, and an operator who pinned op_type by hand deserves a
		 * trace of it being taken back. */
		if (was != mode)
			printf("[cv610-iq] %s: op_type %s -> %s\n", group->name,
				was == OT_OP_MODE_MANUAL ? "manual" : "auto",
				mode == OT_OP_MODE_MANUAL ? "manual" : "auto");
	}

	ret = group->set(&g_attr);
	if (ret != 0) {
		fprintf(stderr, "[cv610-iq] %s: set failed: 0x%x\n",
			param, (unsigned)ret);
		goto out;
	}

	printf("[cv610-iq] %s = %s\n", param, value);
	rc = 0;

out:
	pthread_mutex_unlock(&g_iq_mutex);
	return rc;
}

/* ==========================================================================
 *  Portable AE ceilings
 *
 *  isp.gain_max and isp.shutter_max_us are the fleet-wide names for the two
 *  AE limits every backend has.  They map onto this ISP's exposure group
 *  with NO unit conversion, which is the only reason they are wired at all:
 *
 *    isp.shutter_max_us -> exposure.auto.exp_time_max
 *         ot_isp_ae_range exp_time_range, "sensor exposure time (unit: us)"
 *    isp.gain_max       -> exposure.auto.a_gain_max
 *         ot_isp_ae_range a_gain_range, "Format:22.10 ... sensor analog gain
 *         (unit: times, 10bit precision)" -- 1024 == 1x, the same scale the
 *         SigmaStar supervisory AE uses (maruko_cus3a.c AE_GAIN_MIN).
 *
 *  Analog gain, not sys_gain: the portable name means the SENSOR's gain
 *  ceiling on every other backend, and sys_gain_range caps the product of
 *  analog, sensor-digital and ISP-digital gain.  Capping the product would
 *  make the same number mean something different here.
 *
 *  Both live under auto_attr, so cv610_iq_set() selects AE auto mode for
 *  them -- which is the mode a ceiling is meant to apply in.
 *
 *  0 means "use the sensor plugin's default" on every backend, so the
 *  default has to survive a non-zero write.  Without the snapshot below,
 *  setting a ceiling and then clearing it would leave the ISP on the last
 *  non-zero value forever while the config read 0: a one-way door, and
 *  exactly the shape of bug this milestone keeps finding.  The snapshot is
 *  taken on the first apply, before any write of ours has landed.
 * ========================================================================== */


/* Caller must NOT hold g_iq_mutex: this takes it, and cv610_iq_set() takes
 * it again on the way out.
 *
 * The readiness check is the same one cv610_iq_set() and both query paths
 * make, and it is what lets the snapshot below trust the value it latches:
 * the plugin seeds the exposure attr inside ss_mpi_isp_init(), which runs
 * before cv610_pipeline_isp_ready() turns true, so there is no window where
 * this reads an unseeded struct and caches it as "the default". */
static int ae_defaults(Cv610IqAeDefaults *d, uint32_t *gain_max,
	uint32_t *exp_time_max)
{
	int rc = -1;

	if (!cv610_pipeline_isp_ready()) {
		fprintf(stderr, "[cv610-iq] AE defaults: ISP is not running\n");
		return -1;
	}

	pthread_mutex_lock(&g_iq_mutex);
	if (!d->valid) {
		int ret;

		/* Reuses the shared scratch the mutex exists to guard, rather
		 * than putting an ot_isp_exposure_attr on the caller's stack. */
		memset(&g_attr, 0, sizeof(g_attr));
		ret = iq_get_exposure(&g_attr);
		if (ret != 0) {
			fprintf(stderr, "[cv610-iq] AE defaults: get failed: 0x%x\n",
				(unsigned)ret);
			goto out;
		}
		d->gain_max =
			(uint32_t)g_attr.exposure.auto_attr.a_gain_range.max;
		d->exp_time_max =
			(uint32_t)g_attr.exposure.auto_attr.exp_time_range.max;
		d->valid = 1;
		printf("[cv610-iq] AE defaults from the sensor plugin: "
			"a_gain_max=%u (%.2fx) exp_time_max=%u us\n",
			d->gain_max,
			(double)d->gain_max / 1024.0,
			d->exp_time_max);
	}
	*gain_max = d->gain_max;
	*exp_time_max = d->exp_time_max;
	rc = 0;
out:
	pthread_mutex_unlock(&g_iq_mutex);
	return rc;
}

/* A whole-image .bin import rewrites the AE ext-register record, so the
 * ceilings latched at cold boot are no longer "the plugin's defaults".  Left
 * cached, a later isp.gainMax=0 ("keep the default") would write the
 * PRE-import value back over the imported one.  The isp.sensorBin apply calls
 * this whenever the import reached the vendor call; the next ae_defaults()
 * re-latches from the live ISP. */
void cv610_iq_forget_ae_defaults(Cv610IqAeDefaults *d)
{
	if (!d)
		return;
	pthread_mutex_lock(&g_iq_mutex);
	d->valid = 0;
	pthread_mutex_unlock(&g_iq_mutex);
}

static int ae_set_limit(const char *param, uint32_t value, uint32_t dflt)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "%u", value ? value : dflt);
	return cv610_iq_set(param, buf);
}

int cv610_iq_set_gain_max(Cv610IqAeDefaults *d, uint32_t gain)
{
	uint32_t dflt_gain, dflt_exp;

	if (!d || ae_defaults(d, &dflt_gain, &dflt_exp) != 0)
		return -1;
	return ae_set_limit("exposure.auto.a_gain_max", gain, dflt_gain);
}

int cv610_iq_set_shutter_max_us(Cv610IqAeDefaults *d, uint32_t us)
{
	uint32_t dflt_gain, dflt_exp;

	if (!d || ae_defaults(d, &dflt_gain, &dflt_exp) != 0)
		return -1;
	return ae_set_limit("exposure.auto.exp_time_max", us, dflt_exp);
}

/* ==========================================================================
 *  Applied AWB result
 *
 *  None of the groups above can show this.  f_wb reads back ot_isp_wb_attr,
 *  whose manual_attr holds whatever was last written -- not what the auto
 *  loop converged on -- so on a backend that never leaves auto WB it reads
 *  as a constant.  ot_isp_wb_info is the auto loop's own output.
 *
 *  It carries the two numbers a colour cast has to be attributed between:
 *  the gains actually in force (the white point) and the CCM saturation the
 *  ISO bucket selected (how hard an error in that white point is rendered).
 *  Exposure comes along because saturation is indexed by AGC bucket, so the
 *  gain is what picks it -- without the ISO the saturation reading cannot be
 *  tied back to g_imx662_awb_agc_table.
 * ========================================================================== */
char *cv610_awb_query(void)
{
	/* Local, not static like cv610_iq_query()'s: that one is static because
	 * 24 KB does not belong on the httpd thread's stack, and it holds
	 * g_iq_mutex across the fill.  This one is 2 KB and touches no shared
	 * state, so a local keeps it correct without depending on the server
	 * staying single-threaded. */
	char buf[2048];
	ot_isp_wb_info wb;
	ot_isp_exp_info exp;
	td_s32 wb_ret, exp_ret;
	int pos = 0;

	if (!cv610_pipeline_isp_ready())
		return NULL;

	(void)memset(&wb, 0, sizeof(wb));
	(void)memset(&exp, 0, sizeof(exp));
	wb_ret = ss_mpi_isp_query_wb_info(CV610_IQ_PIPE, &wb);
	exp_ret = ss_mpi_isp_query_exposure_info(CV610_IQ_PIPE, &exp);

	JSON_PUT(buf, pos, sizeof(buf), "{\"ok\":true,\"data\":{\"wb\":{\"ret\":%d",
		(int)wb_ret);
	if (wb_ret == TD_SUCCESS) {
		/* Gains are Format:8.8, so 256 == 1.0x; emitted raw to stay exact. */
		JSON_PUT(buf, pos, sizeof(buf),
			",\"r_gain\":%u,\"gr_gain\":%u,\"gb_gain\":%u,\"b_gain\":%u"
			",\"saturation\":%u,\"color_temp\":%u"
			",\"ls0_ct\":%u,\"ls1_ct\":%u,\"ls0_area\":%u,\"ls1_area\":%u"
			",\"multi_degree\":%u,\"scene\":\"%s\",\"bv\":%d"
			",\"first_stable_time\":%u,\"ccm\":[",
			(unsigned)wb.r_gain, (unsigned)wb.gr_gain,
			(unsigned)wb.gb_gain, (unsigned)wb.b_gain,
			(unsigned)wb.saturation, (unsigned)wb.color_temp,
			(unsigned)wb.ls0_ct, (unsigned)wb.ls1_ct,
			(unsigned)wb.ls0_area, (unsigned)wb.ls1_area,
			(unsigned)wb.multi_degree,
			(wb.scene_status == OT_ISP_AWB_SCENE_MODE_OUTDOOR) ?
				"outdoor" : "indoor",
			(int)wb.bv, (unsigned)wb.first_stable_time);
		for (int i = 0; i < OT_ISP_CCM_MATRIX_SIZE; i++)
			JSON_PUT(buf, pos, sizeof(buf), "%s%u",
				i ? "," : "", (unsigned)wb.ccm[i]);
		JSON_PUT(buf, pos, sizeof(buf), "]");
	}
	JSON_PUT(buf, pos, sizeof(buf), "},\"exposure\":{\"ret\":%d", (int)exp_ret);
	if (exp_ret == TD_SUCCESS) {
		/* Gains are Format:22.10, so 1024 == 1.0x. */
		JSON_PUT(buf, pos, sizeof(buf),
			",\"iso\":%u,\"a_gain\":%u,\"d_gain\":%u,\"isp_d_gain\":%u"
			",\"exp_time\":%u,\"ave_lum\":%u,\"fps\":%u",
			(unsigned)exp.iso, (unsigned)exp.a_gain, (unsigned)exp.d_gain,
			(unsigned)exp.isp_d_gain, (unsigned)exp.exp_time,
			(unsigned)exp.ave_lum, (unsigned)exp.fps);
	}
	JSON_PUT(buf, pos, sizeof(buf), "}}}");

	if (pos >= (int)sizeof(buf) - 1) {
		fprintf(stderr, "[cv610-iq] awb query truncated at %d bytes; "
			"grow buf[] past %zu\n", pos, sizeof(buf));
		return NULL;
	}
	return strdup(buf);
}
