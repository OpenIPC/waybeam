# CV610 kernel modules

The kernel-side half of the CV610 sensor chain. `init.d/load-cv610-online`
insmods both from `/usr/lib/cv610` in place of the image's own; nothing else
in the MPP module set is replaced.

| Module | Source | Why |
|---|---|---|
| `open_sys_config_imx662.ko` | OpenHisilicon's `kernel/sys_config/hi3516cv6xx` + `sys_config-runtime-sensor-clock.patch` | Adds a writable `sns0_clk_hz` parameter, so the daemon feeds the IMX662 27 MHz for 1080p100 and 37.125 MHz for 30/60/90 within one boot. |
| `open_pm_stub.ko` | `pm_stub/open_pm_stub.c` | Registers MPP module id 58 (PM) without touching hardware: `open_base` will not initialise without that id, and the vendor `open_pm` hangs on the OpenIPC kernel. |

Neither is in OpenIPC. Its sys_config fixes the sensor clock at insmod from
the `sensors=` name, and its images carry the vendor `open_pm`, which
`load-cv610-online` refuses. OpenIPC changes conservatively, so these stay
here, next to the daemon that needs them: without the clock parameter
`src/cv610_pipeline.c` warns and runs every mode on the boot clock, so only
the IMX662 modes that match it run at their rate. The module keeps its
historical `_imx662` name because the loader and downstream packages install
it by that name; it serves any sensor.

Both came from the snokvist/firmware CV610 bring-up (the patch from its
`feature/cv610-runtime-sensor-clock` branch, the stub from its `waybeam`
package), where they were verified on a Hi3516CV610 demo board: 30/60/90/100
fps each delivered at its nominal rate with the boot profile left at sc4336p.

The patch is GPL-2.0, like the sys_config file it modifies; the stub is
`GPL-2.0 OR MIT` (its SPDX tag). Neither is linked into waybeam.

## Build

```sh
make drivers-cv610 \
  KSRC_CV610=/path/to/hisilicon-hi3516cv6xx/linux \
  CV610_SDK_INC=/path/to/openhisilicon
```

`KSRC_CV610` must be the OpenIPC `hisilicon-hi3516cv6xx` 5.10 kernel,
configured and `modules_prepare`d — the firmware build's `linux-custom` is
the right one, since module vermagic must match the image.
`CV610_SDK_INC` should be OpenHisilicon at the image's
`HISILICON_OPENSDK_VERSION`: the module replaces the image's own
sys_config, and older trees (such as the header pin CI lints against) do
not know `imx662`/`imx335` in `parse_sensor_clock()`. OpenHisilicon's
`kernel/` is copied to `build/`, patched and built with the options OpenIPC's
`hisilicon-opensdk` package uses for this SoC; the stub links against that
build's `Module.symvers`. Results land in `build/`, and `make stage
SOC_BUILD=cv610` copies them into `out/cv610/cv610/`.
