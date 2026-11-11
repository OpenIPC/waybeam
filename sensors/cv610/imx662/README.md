# Sony IMX662 plugin for Hi3516CV610

This is the source-built HiSilicon V5 sensor plugin used by the CV610
backend. It was developed and verified on the SIP-K662C6S board with four
MIPI lanes.

Verified full-frame modes are 1920x1080 at 30 and 60 fps in RAW12, and 90
and 100 fps in RAW10. The 100 fps mode requires the board loader's 27 MHz
sensor-clock profile; the other modes use the IMX662 37.125 MHz profile.

Build it with the same public OpenHisilicon headers and OpenIPC ARMv7 musl
toolchain as the daemon:

```sh
make sensor-cv610 SOC_BUILD=cv610 \
  CV610_CC=/path/to/arm-openipc-linux-musleabi-gcc \
  CV610_SDK_INC=/path/to/openhisilicon
```

Install the result as `/usr/lib/sensors/libsns_imx662.so`. No proprietary
SDK source or binary is stored here; the build consumes the public sensor
interface and `sensor_common.c` from the external OpenHisilicon checkout.

Linear mode only — `cmos_set_wdr_mode()` rejects every other WDR mode, and HDR
stays deferred by issue #220.

OpenIPC/openhisilicon carries a copy as `sony_imx662` (#282), which is what
stock OpenIPC images install at `/usr/lib/sensors/libsns_imx662.so`. This one
stays the source of truth for waybeam: that copy refuses 100 fps because stock
sys_config feeds the IMX662 37.125 MHz, while here the daemon selects the
27 MHz clock for that mode. Fixes found on either side are ported across; the
0.85.8 entry in `HISTORY.md` lists the last round.

## What the plugin programs

`imx662_cfg.h` holds Sony's power-on sequence — the reserved analog/ADC trim,
the MIPI TX timing, the readout window and the HDR context that linear mode
needs parked. It runs once, in standby, before the per-mode writes, and the
per-mode writes win where the two overlap. That ordering matters for
`0x3A50/51/52`, which keys off ADBIT rather than output depth: the table holds
the 12-bit form and the RAW10 modes overwrite it.

`cmos_get_isp_default()` and `cmos_get_awb_default()` seed the ISP once at init.
The algorithm tables in `imx662_cmos_param.h` are ported from the in-tree
`smart_sc450ai` driver, which targets this same ISP silicon. The blocks that
describe ISP behaviour (gamma, CLUT, demosaic, CAC, anti-false-colour, LDCI,
dehaze, CA) transfer on principle; the noise-fitted ones (bayer_nr, sharpen,
noise_calibration, DRC) are borrowed from a 4 MP sensor and are kept on a
hardware A/B rather than a calibration — a measured IMX662 fit should replace
them. The AWB saturation curve is scaled up from sc450ai's at the low-ISO end.

These are compile-time seeds, read once at pipe start. `src/cv610_iq.c` exposes
the same blocks as live knobs over `/api/v1/iq` and `/api/v1/iq/set`, so a value
here is a starting point that can be corrected without a rebuild — but only the
seed survives a reboot, so a value worth keeping belongs back in this file.

Two blocks are seeded but not switched on: `g_cmos_drc` and `g_cmos_dehaze`
carry `enable = 0`, inherited from sc450ai's linear-mode defaults, and the ISP
bypasses both at runtime (confirmed by `ss_mpi_isp_get_module_ctrl()`). Setting
`drc.enable=1` over the IQ API is the cheap way to evaluate DRC on this sensor
before deciding whether the seed should change.

## Low-light behaviour

The sharpen auto tables are tapered along their ISO axis to zero by bucket 9,
with the daylight buckets (0-3) left byte-identical. Sharpening is a high-pass
filter, so at high gain the sc450ai tables — which *rise* along ISO, edge
strength 195 to 500 — were amplifying noise into the encoder: measured 65.4 to
7.8 Mbps on the bench with sharpen off. The AWB saturation curve tapers harder
above daylight for the same reason, chroma noise being the ugliest high-gain
artifact.

Conversion gain follows the analog gain: `FDG_SEL0` selects HCG above a
threshold and LCG below it, with the written gain code compensated by 15.3 dB
(Rcg typ 5.8) so the switch is continuous. `max_isp_dgain_target` is 4x, which
puts the total ceiling at 398 x 4 = 1592x (~64 dB) and makes AE exhaust analog
before it reaches for digital.

`g_cmos_noise_calibration` is still sc450ai's photon-transfer fit and is the
main thing left wrong for this sensor; `nr.coring_ratio` and `nr.md_en` are
exposed over `/api/v1/iq` so it can be corrected by eye until a real IMX662
calibration exists.

Reference material for further tuning lives outside this repo, in
`hisilicon/vendor/imx662-docs/`.
