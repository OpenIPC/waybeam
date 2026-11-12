#ifndef CV610_MODES_H
#define CV610_MODES_H

#include <stddef.h>
#include <stdint.h>

/* One sensor mode of the CV610 graph.
 *
 * Mirrors the mode table each sensor plugin keeps to itself — nothing in the
 * MPP API reports a mode's bit depth or its expected input clock, and both are
 * needed before the sensor is even probed.  Config validation, pipeline setup,
 * and /api/v1/modes all read these tables so the facts cannot drift apart.
 *
 * width/height here are what the SENSOR captures.  The encoded size is
 * video0.size and is reached by scaling in VPSS, so the two are independent
 * — see cv610_mode_check_output(). */
typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t fps;
	uint32_t raw_bit;          /* MIPI RAW bit depth carried by this mode */
	uint32_t sensor_clock_hz;  /* MCLK the mode's line timing assumes */
	const char *desc;          /* /api/v1/modes label */
} Cv610SensorMode;

/* One sensor the CV610 backend drives: the userspace plugin it dlopens, what
 * the ISP binds it as, and its modes.  Adding a sensor is one row in
 * cv610_modes.c plus its mode table. */
typedef struct {
	const char *name;          /* the system's name for the part ($SENSOR) */
	const char *plugin_path;   /* dlopen'd userspace sensor driver */
	const char *obj_symbol;    /* its exported ot_isp_sns_obj */
	int sns_id;                /* ot_sensor_id the ISP binds; must match
	                            * the plugin's own *_ID */
	const Cv610SensorMode *modes;
	size_t mode_count;
} Cv610Sensor;

/* The sensor with this name, ignoring case, or NULL. */
const Cv610Sensor *cv610_sensor_find(const char *name);

/* The sensor this process drives: the one $SENSOR names, else IMX662, the
 * part this backend served before it had a table.  Never NULL.
 *
 * $SENSOR is the u-boot `sensor` name: OpenIPC's rcS exports it, its module
 * loader brings the sensor up by it, and S95waybeam asks fw_printenv/ipcinfo
 * when started without it.  It picks the plugin and the mode table; waybeam's
 * own load-cv610-online loads the modules by CV610_SENSOR_PROFILE instead, a
 * boot clock the daemon then overrides per mode.  The daemon never changes its
 * environment, so every call answers the same. */
const Cv610Sensor *cv610_sensor_current(void);

/* Resolve sensor.mode + video0.fps to one mode, the way SigmaStar's
 * sensor_select() resolves sensor.mode + video0.fps against MI_SNR_GetRes():
 * an explicit index wins and must exist, otherwise the frame rate is a TARGET
 * rather than a command and the closest usable mode is chosen.  The auto
 * choice first keeps the modes whose capture is at least the requested encode
 * size (width x height; 0x0 fits every mode), and falls back to all modes only
 * when none is, so cv610_mode_check_output() can name the size problem against
 * the mode the rate picked.
 *
 *   forced_mode >= 0   that index, or NULL when the table has no such index
 *   forced_mode <  0   exact rate if a mode has it (the first row wins); else
 *                      the slowest mode still faster than the target; else
 *                      the fastest mode
 *   target_fps == 0    NULL — nothing to select against
 *
 * Three deliberate divergences from SigmaStar's scoring (sensor_select.c):
 *  - a target above every mode clamps to the fastest.  SigmaStar scores
 *    fps_excess at 0 for every mode BELOW the target, so it falls back to
 *    table order -- the slowest mode.  That rarely fires there because its
 *    modes carry min-max fps ranges; on this point-fps table it would fire
 *    constantly, and answering "100" to a request for "120" is what the
 *    operator meant.
 *  - a mode that fits but is slower beats one that keeps up but is too small:
 *    SigmaStar ranks those the other way round, then fails the size check.
 *    So 2592x1944@60 on IMX335 runs at 40 fps instead of being refused.
 *  - modes at the same rate are told apart by table order, not resolution
 *    distance: tables list the preferred mode first (IMX335 lists the full
 *    5 MP RAW12 capture, which keeps vertical flip, ahead of its crop).
 *
 * *out_index (optional) receives the table index, or -1 when NULL is returned.
 * A caller that acts on the result must report a substituted rate: the sensor
 * runs at the MODE's rate, not the requested one. */
const Cv610SensorMode *cv610_mode_select(const Cv610Sensor *sensor,
	int forced_mode, uint32_t target_fps, uint32_t width, uint32_t height,
	int *out_index);

/* Whether an encoded geometry can be produced from this mode.  Returns NULL
 * when it can, or a static reason string.  A zero width AND height mean
 * "the mode's own size" and always pass. */
const char *cv610_mode_check_output(const Cv610SensorMode *mode,
	uint32_t width, uint32_t height);

/* The encoded geometry to use, resolving 0x0 to the mode's own size rounded
 * down to the multiple of 8 an explicit size must be. */
void cv610_mode_resolve_output(const Cv610SensorMode *mode,
	uint32_t req_width, uint32_t req_height,
	uint32_t *out_width, uint32_t *out_height);

#endif /* CV610_MODES_H */
