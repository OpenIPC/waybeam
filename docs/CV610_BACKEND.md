# CV610 backend (IMX662, IMX335)

Tracking: #220

The CV610 port follows the same staged model used for the Star6E-to-Maruko
bring-up. The first slice is deliberately narrow and device-oriented: preserve
the already verified HiSilicon graph, put it behind `BackendOps`, and connect
encoded H.265 frames to Waybeam's shared output contracts before porting the
large control surface.

## Current slice

- `SOC_BUILD=cv610` produces a 32-bit ARMv7 musl `waybeam` binary.
- The backend owns the proven IMX662 MIPI -> VI -> ISP -> VENC lifecycle, and
  drives an IMX335 through the same graph (see "Sensor selection").
- IMX662: linear 1920x1080 modes at 30/60 fps RAW12 and 90/100 fps RAW10.
  IMX335: five fixed modes (see "Sensor selection"). The tables in
  `src/cv610_modes.c` are the only place they are written down.
- H.265 CBR and GOP come from the existing `VencConfig` fields. `video0.qpDelta`
  is **not** among them: CV610's rate control stores every I-frame input and
  then ignores it, so the field is not advertised for this backend and
  `normal_p.ip_qp_delta` is left at 0. The I-frame lever here is
  `video0.intraRefreshQp`.
- Shared resilience presets drive row GDR and reference cadence with strict
  vendor readback; frame-SHM carries GDR/ENHANCE metadata and copied droppable
  NALs are marked TRAIL_N.
- `video0.sliceCount` enables whole-access-unit H.265 slicing. CV610 uses
  32-pixel LCU-row units and keeps early per-slice output disabled.
- UDP and abstract UNIX outputs use the shared HEVC RTP packetizer.
- `frame-shm://` publishes the VFRM v2 whole-frame contract (ring header
  version 2 since 0.69.0; consumers must be rebuilt to match).
- The shared HTTP/WebUI API advertises a CV610-specific capability mask and
  applies bitrate, GOP, RTP payload size, and IDR requests live.
- Debug OSD reuses the shared palette, font, primitive rasterizer, panel, and
  CPU sampler through a CV610 CLUT4 hardware-region adapter.
- Optional audio preserves the standalone streamer's verified inner-ACODEC ->
  AI -> vendor AENC/Opus graph (48 kHz mono, 10 ms, 32 kbit/s restricted low
  delay) and feeds its encoded access units into the shared RTP packetizer and
  output-socket helpers.
- The source-built `libsns_imx662.so` is included under
  `sensors/cv610/imx662/`, staged separately from the proprietary runtime
  libraries, and installed at `/usr/lib/cv610/` rather than
  `/usr/lib/sensors/`, where an OpenIPC image puts its own copy.
- `GET /api/v1/snapshot.jpg` serves a JPEG from a dedicated VENC channel bound
  as a **second destination** on the same VPSS output the H.265 channel
  consumes (`OT_MAX_BIND_DST_NUM` is 4). The channel is created once and left
  stopped; each capture pulses `start_chn(recv_pic_num=1)` -> `get_stream` ->
  `stop_chn`, so it costs nothing while idle and never reconfigures the source.
  Geometry is inherited from the main stream, so `snapshot.width`/`height` are
  advertised unsupported; `snapshot.quality` maps to
  `ss_mpi_venc_set_jpeg_param(qfactor)`.
- Recording in `record.mode=mirror`, both `format=ts` (with Opus audio muxed)
  and `format=hevc`, with rotation on `record.max_seconds`/`max_mb` and
  `/api/v1/record/{start,stop,status}`. No SDK-typed adapter was needed: the
  drain loop already copies each access unit into one contiguous Annex-B
  buffer, which is exactly what the shared recorder cores consume. Record
  start forces an un-coalescible IDR — the shipped config is
  `resilience=racing`, which emits no periodic IDR, so a coalesced request
  would leave a file with no IRAP access unit anywhere in it.
- SIGINT/SIGTERM unwind VENC, ISP, sensor callbacks, VI, SYS, and VB in order.

The shipped `record.dir` default is `/mnt/mmcblk0p1`, shared with the SigmaStar
backends. The CV610 reference board has no such device — the SIP-K662C6S bench
mounts USB storage at `/mnt/sda1` — so `record.dir` has to be set for the
target before recording will start. It is left at the shared default rather
than hardcoding one board's mount point into a generic config.

The graph runs VI, ISP and VPSS all online (`OT_VI_ONLINE_VPSS_ONLINE`), the
mode the vendor's own CV610 sample defaults to: neither the RAW capture nor a
full-size YUV frame is written to VB, only VPSS's output, which VENC and the
snapshot channel read. The module loader must provide a clean SYS/VB
lifecycle; the backend verifies the requested mode by reading it back before
creating the VI pipe.

## Encoder capability delta

CV610 has a substantially broader vendor encoder surface than the current
backend exposes: nine declared RC families, multiple P/CRR GOP structures,
intra refresh, reference prediction, SVC, hierarchical QP, native frame-loss
and super-frame policies, QP maps, VUI/user data, and slice/low-delay
controls. (Plain delta-QP **ROI is no longer in that list** -- it landed in
0.76.0 as `fpv.roi*`; the QP-map and per-frame-ROI halves of the vendor surface
are still unexposed.) SSC338Q currently has the broader device-proven Waybeam integration.

The evidence levels, live-device readback, exact comparison and deliberately
deferred controls are documented in
`documentation/SSC338Q_CV610_ENCODER_CAPABILITIES.md`. The resilience and
whole-access-unit slice port is tracked in
`documentation/CV610_RESILIENCE_SLICES_PLAN.md`.

The port is confirmed on device at 1080p30/60/100. Requests for 1, 3, 4, 6, 9,
12 and 17 slices delivered the requested VCL NAL census, reference cadences and
TRAIL_N counts matched, and an IDR-started 1080p60 capture decoded cleanly
under FFmpeg `-xerror`. Native SVC, extra RC/GOP modes and encoder-side loss
policies remain deliberately deferred.

## Build

The public OpenHisilicon headers and OpenIPC CV610 runtime library directory
must already exist locally. They are external inputs and are not copied into
this repository.

```sh
make build SOC_BUILD=cv610 \
  CV610_CC=/path/to/arm-openipc-linux-musleabi-gcc \
  CV610_SDK_INC=/path/to/openhisilicon \
  CV610_SDK_LIB=/path/to/cv610/rootfs/usr/lib

make sensor-cv610 SOC_BUILD=cv610 \
  CV610_CC=/path/to/arm-openipc-linux-musleabi-gcc \
  CV610_SDK_INC=/path/to/openhisilicon \
  CV610_SDK_LIB=/path/to/cv610/rootfs/usr/lib
```

Install `out/cv610/waybeam` as `/usr/bin/waybeam`, the plugin
`out/cv610/cv610/libsns_imx662.so` as `/usr/lib/cv610/libsns_imx662.so`, and
start with
`config/waybeam.default.cv610.json`. When `make drivers-cv610` has built
them, `make stage` also stages `out/cv610/cv610/open_sys_config_imx662.ko`
and `open_pm_stub.ko`, which go to `/usr/lib/cv610/`. It always emits the
tested CV610 init script and platform profile:

```sh
cp out/cv610/S95waybeam /etc/init.d/S95waybeam
cp out/cv610/load-cv610-online /usr/bin/load-cv610-online
cp out/cv610/waybeam-cv610.conf /etc/waybeam-cv610.conf
chmod +x /etc/init.d/S95waybeam /usr/bin/load-cv610-online
```

The init script invokes the staged `/usr/bin/load-cv610-online` module
loader before starting the daemon. It does **not** unload the MPP stack on
stop: the HiSilicon drivers take no module reference for an open fd, so an
`rmmod` under a live consumer (typically `waybeam_hub` holding `/dev/rgn`)
frees file operations still in use and wedges the SoC hard enough to need a
physical power cycle. Unloading is done only by an explicit
`load-cv610-online stop`, which refuses while any consumer holds an MPP
device. Only one service may own the graph; disable the standalone
`S95cv610-streamer` service when enabling `S95waybeam`.

Three operator rules follow from that:

- **Recovery after an abnormal exit is a plain `S95waybeam start`.** The
  modules are still loaded and the daemon re-initialises against them; there
  is no `reload` action and no module rollback on a failed start.
- **Changing `CV610_AUDIO`, `CV610_SENSOR_PROFILE` or the MMZ needs a
  reboot.** The
  loader refuses to re-load a differing module set over a live one.
- **A boot that has seen a hard kill cannot be unloaded cleanly.** Reboot
  rather than fighting it.

`CV610_SENSOR_PROFILE` picks the sensor clock the modules load with. It is
now only a fallback: the daemon sets the clock for the selected mode during
bring-up (see "Sensor clock" below), so the profile matters only against a
sys_config too old to expose `sns0_clk_hz`.

Audio is opt-in in both layers: set `CV610_AUDIO=1` in
`/etc/waybeam-cv610.conf` so the loader provides `open_aio`, `open_ai`,
`open_aenc`, and `open_acodec`, then set `audio.enabled=true`, 48000 Hz, mono,
Opus, and a non-negative `outgoing.audioPort` in `/etc/waybeam.json`. The
daemon refuses other CV610 audio formats. The init script tracks the daemon
across API-driven process respawns; module unloading is guarded separately in
`load-cv610-online`, which refuses while any process holds an MPP device. `audio.enabled` and `audio.mute` are
restart-required HTTP controls; the rest of the audio group is hardcoded and
advertised unsupported. Enabling audio from a video-only boot still needs
kernel modules that a daemon respawn cannot load, so the daemon warns and
runs without audio rather than failing to start — `/api/v1/audio/status`
reports the running state, not the config flag.
Audio remains a separate RTP/UDP side channel: UDP video uses the configured
remote host, while local `unix://` or `frame-shm://` video sends audio to the
co-located Waybeam Link listener on `127.0.0.1:<audioPort>`.

## Sensor selection

The sensor is a userspace plugin the daemon `dlopen`s, so which part a craft
runs is chosen at bring-up, from `$SENSOR`: the u-boot `sensor` name. OpenIPC's
rcS exports it and its module loader (`load_hisilicon`) brings the sensor up by
it; `S95waybeam` asks `fw_printenv -n sensor`, then `ipcinfo --short-sensor`,
when started without it. There is no config field — on OpenIPC's loader one
could only disagree with the modules the craft loaded — so switching parts is
`fw_setenv sensor <name>` and a reboot. Bring-up logs the choice:

```
> CV610 sensor: imx335 (/usr/lib/sensors/libsns_imx335.so)
```

A missing or unknown name comes up as IMX662, with a `WARNING` quoting what
`$SENSOR` held. `src/cv610_modes.c` holds one row per sensor:

| `$SENSOR` | plugin | from | modes |
|---|---|---|---|
| `imx662` | `/usr/lib/cv610/libsns_imx662.so` | this tree (`sensors/cv610/imx662`) | 1080p30/60 RAW12, 1080p90/100 RAW10 |
| `imx335` | `/usr/lib/sensors/libsns_imx335.so` | the image (OpenIPC/openhisilicon `sony_imx335`) | 2592x1944p30 RAW12, 1296x972p60 binned, 1080p90 crop, 2592x1944p40, 2592x1520p30 crop |

IMX662 stays waybeam's own because 1080p100 needs the daemon's clock switch
(see "Sensor clock") and the image's copy refuses that mode. `make stage`
puts it in `out/cv610/cv610/`; a firmware package must install it to
`/usr/lib/cv610/`, out of reach of the image's copy in `/usr/lib/sensors/`.

IMX335 needs no workaround in the plugin: every mode runs on 37.125 MHz and
`pub.sns_mode` stays 0 ("auto"), the plugin picking its mode from the exact
capture size and a rate inside the mode's range, which tells the five rows
apart. Its flexible window crop has no fixed size and is not listed. What it
needs from the craft:

- **An image that ships the plugin**: OpenIPC hi3516cv6xx `ultimate` (the
  `lite` image filters it out), with OpenIPC/openhisilicon#284/#288 for
  `image.mirror`/`image.flip`. Vertical flip is refused in the two crop
  windows (1080p90, 2592x1520p30) — the plugin logs it and the
  `/api/v1/modes` labels say so — so an inverted IMX335 craft wants a full-
  frame or binned mode.
- **The right clock on waybeam's own loader.** `load-cv610-online` loads the
  modules by `CV610_SENSOR_PROFILE`, a boot clock, not by `$SENSOR`; the
  daemon then sets each mode's clock through `sns0_clk_hz`. Without that
  parameter the IMX335 stays on the profile's clock (27 MHz for the default
  `sc4336p`) and runs slow. Keep the profile at a name the craft's sys_config
  knows — `sc4336p` always is.
- **MMZ for the 2592-wide rows.** Since 0.86.3 VB holds only VPSS's output,
  4 frames of the encoded size. Up to 0.86.2 it also held 4 RAW frames and 4
  full-size YUV frames of the capture, which on a 128 MiB Hi3516CV610 +
  IMX335 board at 1080p output came to more than the 64 MiB
  `load-cv610-online` defaults to on a boot without `hisi_mem`. MMZ in use
  (`/proc/umap/media-mem`), before → after: on the IMX335 (DmtryJS, `hisi_mem`
  96 MiB zone) 89.6 → 22.2 MiB for 2592x1944 (modes 0 and 3), 74.5 → 21.7 MiB
  for 2592x1520, 36.8 → 20.9 MiB for 1080p90 and 27.0 → 10.0 MiB for the
  binned row at 720p; on the IMX662 36.9 → 21.0 MiB at 1080p30 and 38.3 →
  10.4 MiB at 720p100. (0.86.2's "94 MiB" for 2592x1944 was CMA in use from
  `/proc/meminfo`, which reads higher than the zone's own count: 30.0 against
  22.2 MiB on the same row after this change.) The ISP and VI still keep private buffers that
  follow the capture. Should a zone still be too small
  on a boot without `hisi_mem`: on the reference 128 MiB board it
  (0x44000000 + 64 MiB) already reaches the top of RAM above the kernel's
  `mem=64M`, so it can only grow together with a smaller kernel `mem=` and
  `CV610_MMZ_START`/`CV610_MMZ_SIZE` moved with it, never SIZE alone: past
  the end of RAM it is not a recoverable error. On any boot, a mode whose VB
  does not fit the zone fails `vb` init, and a `/set` that selects it takes
  the daemon down at the restart it triggers.
- **Where the zone comes from.** A hi3516cv6xx kernel with `hisi_mem`
  (OpenIPC images from OpenIPC/firmware #2638, October 2026, and later)
  measures DRAM and lays memory out itself: it ignores `mem=` and the
  bootloader's `totalmem`/`osmem` variables ("64M of memory from the
  bootloader ignored, DRAM is 128M"). `CV610_MMZ_START`/`CV610_MMZ_SIZE`
  cannot move that split; OpenIPC's `set_allocator`, which writes
  `osmem=`/`mmz_allocator=`/`mmz=` into the bootargs, plus a reboot can.
  `load-cv610-online` therefore reads
  `/sys/module/hisi_mem/parameters/{allocator,mmz}` -- the same source
  OpenIPC's own `load_hisilicon` uses -- and
  `CV610_MMZ_START`/`CV610_MMZ_SIZE` apply only on a boot without `hisi_mem`,
  where the zone is a bootloader carve-out, and only when the command line
  does not name that carve-out itself with `mmz=`; either way the loader says
  which values it dropped. `mmz_allocator=ot` is what
  the bootloader layout needs; against `hisi_mem` it is fatal, because the
  zone then lies inside kernel memory and `open_mmz` refuses the zone
  ("MMZ conflict to kernel memory") and loads no MPP modules at all.
- **5 MP output fits a 128 MiB board since 0.86.3.** `video0.size=2592x1944`
  on the IMX335 measured 49.3 MiB of MMZ in use at 30.00 fps (`hisi_mem`
  96 MiB zone, 53.8 MiB `MemAvailable`). Up to 0.86.2 the capture-size RAW and
  YUV pools on top of it got the daemon killed by the OOM killer.

The IMX335 rows were verified on hardware by DmtryJS on a Hi3516CV610 +
IMX335 with this code (`bc6af4a`, #131): all five modes, mirror/flip, the
mode list and sensor resolution through the init script. That run used a
stock OpenIPC image's module loader and memory layout, with the daemon's
sensor clock unavailable.

`load-cv610-online` and `drivers/cv610` were run on that same board later:
the loader brings up its own module set (no vendor `open_pm`), the daemon's
`sns0_clk_hz` write is accepted (`37125000 -> CRG 0x8440 = 0x8010`), and each
row delivers its nominal rate at 1080p output -- 30.1, 40.2, 60.1, 30.1 and
90.2 fps for modes 0, 3, 1, 4 and 2. It needs the `hisi_mem`-derived allocator
above; with `mmz_allocator=ot` it loads nothing.

## Sensor modes

`video0.fps` selects a sensor mode; `video0.size` is the encoded size, which
VPSS (between VI and VENC) scales the capture down to — the same split
SigmaStar gets from its VPE SCL ports. The size only narrows which modes the
rate is matched among, those whose capture covers it: on IMX335, 720p60 runs the binned mode, 1080p60 the 1080p90 crop (the
binned capture is smaller than 1080p), and 30 fps the full 5 MP RAW12 capture,
the first row at that rate. `sensor.mode=4` picks the 2592x1520 crop instead.

VPSS is created even when no scaling is asked for, so the 1:1 path and a
scaled one exercise the same graph and the same teardown order.

The aspect crop (`isp.keepAspect`, for a `video0.size` of another aspect than
the capture) is the VI pipe's post-ISP crop: an internal rectangle VPSS then
scales to `video0.size`, so the encoded size stays the one asked for. It is
not a VPSS crop because, with VPSS online, `ss_mpi_vpss_set_grp_crop` refuses
every call (`0xa0078007`, a disabled crop included) and the VPSS channel crop
is accepted but applied short — 1440 wide at x=240 came out 1200 wide, so
VENC dropped every frame — and the VI channel crop is `OT_ERR_NOT_SUPPORT` on
this chip. The ISP still meters the whole capture. The post-crop must sit on
a 4-pixel grid (`vi_check_crop_align`: "crop width should be 4 align"), so
the centre crop is rounded to it — an IMX335 1080p crop is 2592x1456, not
2592x1458.

Two attribute values are not optional, both established by probing the
hardware (`ss_mpi_vpss_set_chn_attr` returns `0xa0078007`,
`OT_ERR_ILLEGAL_PARAM`, otherwise):

- `chn_mode` must be `OT_VPSS_CHN_MODE_USER`. `AUTO` makes the channel follow
  the group's input size and rejects an explicit geometry — `USER` is what
  makes the channel a scaler.
- `frame_rate` must stay `-1/-1`. `0/0` is rejected with the same code.

Pixel format is *not* one of them: VPSS accepts both NV21 and NV12 here, so
the chain stays on NV21 to match what VI is willing to emit.

Output geometry is validated against the selected mode: no upscaling past the
capture size, and both dimensions required. Note the shared validator
additionally requires a multiple of 8 in each dimension, so 960x540 is
rejected on the height.

`src/cv610_modes.c` holds the tables — capture geometry, frame rate,
MIPI RAW bit depth, and the input clock the mode's line timing assumes.
Everything derives from it: `cv610_validate_config()` rejects anything that
does not resolve, `cv610_prepare()` takes the bit depth and clock from the
resolved mode, and `GET /api/v1/modes` is generated from it, so what that
endpoint lists is exactly what `/api/v1/set` accepts — for the sensor
`$SENSOR` names. Each table mirrors its plugin's own (`g_imx662_mode_tbl` in
`sensors/cv610/imx662/imx662_cmos.c`, `mode_tbl[]` in openhisilicon's
`sony_imx335`); adding a mode means editing both. The geometry listed by
`/api/v1/modes` is what the sensor captures, not the encoded size.

A zero `video0.size` (`auto`) means the mode's own capture size, rounded
down to a multiple of 8 (IMX335's binned 1296x972 encodes 1296x968). A half-set
one (`1920x0`) is rejected rather than completed — it is a typo, not a
request.

## Sensor clock

The IMX662 runs 30/60/90 fps on a 37.125 MHz input clock and 100 fps on
27 MHz — the latter feeds 27 MHz while the sensor selects its 24 MHz INCK
profile (register `0x3014 = 0x04`). The two halves must agree: the sensor
keeps its programmed line timing regardless, so a mismatch is silent and
scales the rate by the clock ratio (a 60 fps mode on the 27 MHz clock
delivers 43.6 fps, measured, while every status surface still reports 60).

The clock is one CRG register that only the kernel can write, and the vendor
MIPI ioctls gate it without setting a rate. `mipi_setup()` therefore writes
the frequency carried by the resolved mode to
`/sys/module/open_sys_config/parameters/sns0_clk_hz` during bring-up — after
`ENABLE_SENSOR_CLOCK`, which rewrites the same register, and before
`UNRESET_SENSOR`. The parameter comes from
`drivers/cv610/sys_config-runtime-sensor-clock.patch`, built into
`open_sys_config_imx662.ko` with `make drivers-cv610` alongside the PM stub
the loader also needs (see `drivers/cv610/README.md`); OpenIPC's own
sys_config fixes the clock at insmod. Against a module without it the daemon
warns, names the clock the mode needs, and continues on the boot-time clock.

## Deferred phases

1. Run a longer-duration soak beyond the bounded device matrices already
   completed.
2. Add live output redirection and encoder output-FPS control.
3. Dual VENC (`record.mode=dual` / `dual-stream`) — a second encoder channel
   at its own bitrate/fps/GOP, needing a second VPSS channel, a drain thread
   and its own teardown ordering. Mirror-mode recording landed first, as this
   list prescribed; the remaining modes are refused with a warning rather than
   silently recording ch0. Stabilization likewise remains deferred. Add live
   audio gain/mute only after the analog control path can be operator-verified.
4. Independent snapshot geometry (`snapshot.width`/`height`), which needs a
   dedicated VPSS scaler channel running continuously.

Unsupported SigmaStar-only fields remain in the common configuration schema
but are marked unsupported in `/api/v1/capabilities` and rejected before
mutation. This keeps the shared dashboard honest while the CV610 control
surface grows feature by feature.

Pull-request verification cross-builds this backend. Release promotion remains
gated on the CV610 build plus the focused device matrix documented in
`documentation/CV610_RESILIENCE_SLICES_PLAN.md`.

## Hardware verification

The integrated daemon was deployed to the SIP-K662C6S CV610 target on
2026-08-13 and streamed 1920x1080 RAW10/H.265 at 100 fps to the x86 Waybeam
Hub decoder. An independent FFmpeg receive pass decoded 1,479 frames in 15
seconds (about 99 fps) at 1920x1080. Producer logs held 100 fps; the four UDP
drops observed during initial receiver synchronization did not increase over
the following 1,800 frames, and a subsequent init-service restart streamed
600 frames with zero drops. SIGTERM completed full ISP/VENC/SYS/VB teardown in
about 200 ms, and the installed service completed a cold module restart in
eight seconds.

The integrated audio path initialized on the same target, resolved the known
ACODEC power-up race after its expected 100 ms retry, and produced about 100
Opus access units/s with zero reported send drops. A host receiver checked 12
consecutive RTP v2/PT98 packets: sequence deltas were 1 and 48 kHz timestamp
deltas were 960, exactly 20 ms (`CV610_AUDIO_POINT_NUM` 960; the 480/10 ms
figure predates the Opus frame-size parity fix). No microphone was connected, so acoustic
content is explicitly not operator-verified.

On 2026-08-22 the resilience/slice branch was also confirmed through
frame-SHM at 1080p30/60/100. Slice requests 1, 3, 4, 6, 9, 12 and 17 produced
the requested VCL census; a 380-frame IDR-started 1080p60 capture decoded
cleanly with FFmpeg `-xerror`; and GDR/ENHANCE metadata, TRAIL_N rewriting and
one-second loss recovery passed. Only a longer-duration soak remains pending.
