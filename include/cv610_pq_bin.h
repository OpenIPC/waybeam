#ifndef CV610_PQ_BIN_H
#define CV610_PQ_BIN_H

/* PQTools `.bin` import/export for the Hi3516CV610 ISP.
 *
 * cv610_iq.c exposes the ISP as individual live knobs.  This module moves the
 * whole parameter image at once, through the vendor `libbin.so` blob, which is
 * how PQTools tunes: a `.bin` is the serialized ISP + 3DNR state.
 *
 * The blob is dlopen'd rather than linked because it is not ours to ship in
 * the SDK library set, and because a craft without it must still boot -- every
 * failure here is a warning, never fatal, matching star6e's isp_runtime.c.
 *
 * The `.bin` is locked to the chip and the SDK ISP version, NOT to the sensor:
 * import validates a register address/size walk against what this chip's ISP
 * reports live, and carries no sensor identity.  A tune built for a different
 * sensor on the same SoC and SDK loads; it simply carries that sensor's
 * black level, CCM, AWB and noise calibration with it -- device-measured on
 * .181, where an os02h10 tune imported cleanly onto an IMX662 and turned the
 * picture red.
 *
 * The payload IS integrity-checked, so a `.bin` cannot be edited or spliced in
 * place: flipping one byte at ISP offset 20000 of an otherwise valid file made
 * OT_PQ_BIN_ImportBinData return 0xcb000005.  Produce files with PQTools, or
 * with cv610_pq_bin_export(), and treat them as opaque. */

/** Whether the vendor library is present and loadable on this craft.
 *  Probed on demand, never cached: both entry points below degrade to a
 *  warning without it, so this exists to keep /api/v1/capabilities honest
 *  rather than to gate the calls. */
int cv610_pq_bin_available(void);

/** Import a PQTools `.bin` into the running ISP.
 *  An empty or NULL path is a no-op and succeeds, so an unset config field
 *  costs nothing.  Returns 0 on success, -1 on any failure. */
int cv610_pq_bin_import(const char *path);

/** Export the live ISP state to a `.bin` at `path`, overwriting it.
 *  The buffer size is queried from the library, never assumed.
 *  Returns the number of bytes written on success, -1 on any failure. */
int cv610_pq_bin_export(const char *path);

#endif /* CV610_PQ_BIN_H */
