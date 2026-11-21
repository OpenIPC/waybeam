# IMX335 two-lane: frame rate above 30 fps on Hi3516CV610

Test plan and log for raising the frame rate of the IMX335 two-lane mode
(plugin mode 6, `IMX335_5M_30FPS_10BIT_2LANE_MODE`, 2592x1944 RAW10) beyond
the vendor setting's 30 fps. Target: 45 fps.

- Board: Hi3516CV613 + IMX335, two CSI-2 lanes on MIPI-RX PHY lanes 0 and 2,
  `root@192.168.1.88`, stock OpenIPC modules, sensor clock 37.125 MHz.
- Software: the stages below ran on a pre-0.86 waybeam pipeline (VI online,
  VPSS offline); "Re-run on the 0.87.0 pipeline" repeats the limits on the
  current all-online one with `SENSOR=imx335_2l`. Plugin from openhisilicon
  branch `imx335-cv6xx-2lane`.
- Baseline (measured 2026-11-21): HMAX 0x258 (600), VMAX 0x101D (4125),
  1188 Mbps/lane, 33334 us between frames, no CRC errors.

## Method

The rate is raised the way `imx335_sensor/sony_imx335/imx335_sensor_ctl.c`
does it on the four-lane boards: VMAX stays, HMAX shrinks. HMAX counts the
sensor's 74.25 MHz internal clock, so

    fps = 74.25e6 / (HMAX * VMAX) = 18000 / HMAX      (VMAX = 4125)

| fps | 30 | 35 | 37.5 | 40 | 42 | 43 | 45 | 48 | 50 |
|---|---|---|---|---|---|---|---|---|---|
| HMAX | 600 | 514 | 480 | 450 | 428 | 418 | 400 | 375 | 360 |

## What should limit it

One output row takes two HMAX periods (VMAX 4125 covers about 2062 rows), so
the row period is `2 * HMAX / 74.25 MHz`. A RAW10 row is 2616 pixels, 26160
bits, and has to leave the sensor within that period, plus the per-row
LP/HS transitions and packet framing.

| Link rate (2 lanes) | Row data time | HMAX floor, no overhead | fps ceiling, no overhead |
|---|---|---|---|
| 1188 Mbps/lane | 11.01 us | 409 | 44.0 |
| 1336.5 Mbps/lane | 9.79 us | 364 | 49.5 |
| 1485 Mbps/lane | 8.81 us | 327 | 55.0 |

Prediction: at 1188 Mbps the link runs out somewhere around 40 fps once
overhead is counted, and 45 fps (HMAX 400, row period 10.77 us) cannot fit at
all. 45 fps needs a faster link. The sensor's own readout is not the limit
here: its vendor four-lane 10-bit all-pixel setting runs HMAX 300 (60 fps).

Other candidates for the first failure, in pipeline order: the MIPI receiver
(lane rate, FIFO), VI/ISP throughput (5 MP at 45 fps is 227 Mpixel/s; the
board's ISP is rated for 8 MP at 40 fps, 332 Mpixel/s), VPSS, the encoder.

## Stages

**A. HMAX sweep at 1188 Mbps, live.** With the pipeline streaming, write HMAX
(0x3034/0x3035) over I2C, starting at 514 (35 fps) and stepping down. Fast,
and volatile: restarting waybeam restores the table values. Known distortion
of a live poke: the plugin and the encoder still believe 30 fps, so exposure
time per line and the bitrate budget are off. That does not affect what this
stage measures (link and frame timing).

**B. Diagnose the limit.** At the first failing step, read which counter
moved first and follow the table below. Back off one step and hold it for a
soak to confirm the last good value.

**C. Faster link.** Raise the sensor's MIPI PLL: `INCKSEL1` (0x314C) times
the 37.125 MHz clock is four times the lane rate, so 0x80 is 1188, 0x90 is
1336.5, 0xA0 is 1485 Mbps. 1485 Mbps is outside every IMX335 vendor table
found locally, so each step is an experiment. Sequence per step: standby
(0x3000=1), write the PLL, release. If the receiver does not relock, restart
the pipeline with the value built into an experimental plugin, and if that
fails too, scale the CSI-2 global timing registers (0x3A18..0x3A28) by the
same ratio the 891 and 1188 Mbps sets differ by. Then repeat the HMAX sweep.

**D. Confirm from a clean start.** Build the best setting into an
experimental plugin table with the true `max_fps`, add the matching waybeam
mode, and bring the pipeline up normally, so exposure and the encoder know the
real rate. Soak, and check the picture. Only this stage's result is a
candidate for the driver.

## Measurements per step

Held for at least 30 s per step, 5 min for a soak:

- ISP interrupt gap (`/proc/umap/isp`): equals `1e6 / fps`, max close to it.
- MIPI-RX (`/proc/umap/mipi_rx`): measured size 2592x1944; CRC, ECC,
  frame-number, frame-mismatch and FIFO counters do not move; lane rate.
- VI (`/proc/umap/vi`): `lost_cnt` and `vb_fail_cnt` stay 0.
- Encoded rate at the host: RTP marker count per second.
- Snapshot: no torn, shifted or noisy rows.
- `dmesg`: nothing new.

A step passes only when all of them hold.

## Reading a failure

| First symptom | Likely cause | Next test |
|---|---|---|
| CRC/ECC errors, wrong measured width | Row does not fit the row period at this link rate | Stage C |
| Receiver FIFO overflow, link otherwise clean | Receiver cannot drain at this pixel rate | `data_rate` x2 on MIPI-RX and VI |
| No lock after a PLL change, lanes stuck in LP | Receiver needs a re-init, or D-PHY timing no longer valid | Clean restart, then scaled timing registers |
| Link clean, ISP gap irregular or VI lost frames | VI/ISP throughput | VI offline mode; compare against a cropped capture |
| VI clean, encoded rate below sensor rate | VPSS or encoder | Smaller `video0.size` to isolate |
| Everything counts clean, picture damaged | Sensor readout at this HMAX | Compare against the four-lane vendor floor (HMAX 300) |

## Safety

Every register write is volatile; a waybeam restart reprograms the sensor.
Reading registers over I2C while the ISP runs collides with its exposure
writes (`bsp-i2c ... wait idle abort` in dmesg), so writes are kept to the
few needed and reads to spot checks. Nothing here changes flash, modules or
the committed driver.

## Results (2026-11-21)

**45 fps is reached and holds from a clean start.** The ceiling is about
47.6 fps, set by the SoC's ISP clock, not by the sensor or the link.

| Limit | Value | Cause |
|---|---|---|
| HMAX floor, any link rate | 426 | A row must fit its row period; below this the picture breaks |
| 1188 Mbps (vendor PLL) | 42.25 fps | The HMAX floor |
| ISP, 5 MP | about 47.6 fps | ISP0 / VI-proc clock, 264 MHz |
| Native 2592x1944 encode | about 43 fps (38.6 on the pre-0.86 pipeline) | Encoder and VB; scaled output is not affected |

### Stage A/B: 1188 Mbps, fps = 18000 / HMAX

| HMAX | fps | Counters | Picture |
|---|---|---|---|
| 600 | 30.0 | clean | clean |
| 514 | 35.0 | clean | clean |
| 480 | 37.5 | clean | not sampled |
| 450 | 40.0 | clean | clean |
| 428 | 42.06 | clean | clean, 6 of 6 |
| 426 | 42.25 | clean | clean |
| 425 | 42.35 | clean | damaged |
| 420 | 42.86 | clean for a 3 min soak | damaged |
| 414 | 43.48 | clean | not sampled; inferred damaged from 425 and 420 |
| 412 | 43.69 | frame height 1906, encoder 0 fps | none |
| 410 / 404 / 400 | 43.9 / 44.6 / 45.0 | height 1894 / 1868 / 1850, encoder 0 fps | none |

The failure has two phases, and the first is invisible to every counter.
From HMAX 425 down to 414 the receiver still measures 2592x1944, no CRC, ECC
or FIFO counter moves, VI loses nothing and the encoder runs at the full rate
— while the picture is horizontal bands, shifted and demosaiced on the wrong
Bayer phase. Only from 412 do rows go missing and the frame come up short.
**A step that passes the counters has not passed; the snapshot check is the
test.** The 3 min soak at 420 was "clean" by every number.

The floor of 426 means a row needs 852 clocks where its data alone takes
818: 34 clocks, 0.46 us, of per-row overhead. The no-overhead prediction of
409 was 4% optimistic.

### Stage C: faster PLL

`INCKSEL1` does not only speed up the link. It scales the sensor's whole
clock tree, including the clock HMAX counts:

    fps = 18000 / HMAX * INCKSEL1 / 0x80

so at the same HMAX the frame rate rises with it, and the HMAX floor stays at
426 because row period and row data time shrink together. The receiver
relocked after a standby/release at every step, with no re-init and with the
CSI-2 timing registers left at their 1188 Mbps values. Its measured lane rate
followed: 1200, 1344, 1496 MHz.

| INCKSEL1 | Lane rate | HMAX | fps | Result |
|---|---|---|---|---|
| 0x90 | 1336.5 Mbps | 450 | 45.0 | clean |
| 0x90 | | 440 / 432 / 428 | 46.1 / 46.9 / 47.3 | clean |
| 0x90 | | 426 | 47.58 | clean |
| 0x90 | | 424 / 420 | 47.75 / 48.2 | picture damaged (HMAX floor) |
| 0xA0 | 1485 Mbps | 600 / 500 | 37.5 / 45.0 | clean |
| 0xA0 | | 480 / 476 / 473 | 46.9 / 47.3 / 47.52 | clean |
| 0xA0 | | 470 | 47.86 | no frames |
| 0xA0 | | 462 / 456 / 450 | 48.7 / 49.4 / 50.0 | no frames |

At 1485 Mbps the link is not what fails. At 47.86 fps and above the receiver
still measures full frames with no errors and the ISP still takes an
interrupt per frame, but the VI channel's `send_cnt` freezes and its
`lost_frame_cnt` climbs: every frame is dropped inside VI. The same link at
45 fps is clean, so it is the frame rate, not the lane rate.

The clock registers say why. ISP0 (CRG 0x9150 = 0x4010) and VI-proc (0x9740 =
0x14010) run at 264 MHz, the top tier `clk_cfg.c` has for this chip. 5.04 MP
at 47.6 fps is 240 Mpixel/s, 91% of that clock. By the same ratio an 8 MP
sensor would top out near 29 fps on this configuration, not 40; that is an
inference, not a measurement.

### Stage D: 45 fps from a clean start

Experimental plugin (mode 6 table with `INCKSEL1` 0x90, HMAX 0x1C2, `max_fps`
45) and a matching 45 fps waybeam mode, built in a scratch tree and not
committed. Normal bring-up, 1920x1440 output, held 5 minutes:

- ISP 45.00 fps, 22236 us between frames; encoder 45.00 fps.
- Receiver 2592x1944 at 1344 MHz; no MIPI counter moved; VI lost no frame.
- 14 of 14 snapshots clean.
- AE running against the true rate (analog gain 5.2x, max exposure VMAX - 8).

Native 2592x1944 output at 45 fps does not hold: the sensor stays at 44.99
fps but the encoder delivers 38.6, with `vb_fail_cnt` rising in VI.

1920x1080 output with `isp.keepAspect=false` — the whole 4:3 sensor area
scaled to 16:9, no crop — also holds: 45.01 fps at the ISP and the encoder
over 30 s, VI lost no frame, snapshot clean, and the same field of view as
the 1920x1440 frame. With `keepAspect` at its default (true) that size
centre-crops the sensor instead.

### Settings worth keeping

| Use | INCKSEL1 | HMAX | fps | HMAX margin | Notes |
|---|---|---|---|---|---|
| Vendor | 0x80 | 600 | 30 | 41% | What mode 6 ships today |
| Fastest on vendor clocks | 0x80 | 450 | 40 | 5.6% | Validated live only |
| 45 fps | 0x90 | 450 | 45 | 5.6% | Clean start, 5 min. 5.5% under the ISP ceiling |
| 45 fps, alternative | 0xA0 | 500 | 45 | 17% | Live only. 25% overclock, 1485 Mbps lanes |

The HMAX edge is sharp and repeatable (426 clean, 425 damaged, on two PLL
settings), and both sides of it derive from the same input clock, so the
margin does not drift with the oscillator.

### In the driver

The 45 fps setting is plugin mode 7, `IMX335_5M_45FPS_10BIT_2LANE_MODE`
(openhisilicon branch `imx335-cv6xx-2lane`): mode 6's table plus a
three-register delta (HMAX 0x01C2, `INCKSEL1` 0x90), hint-only
(`sns_mode` / `Isp_SnsMode` = 7), `max_fps` 45. Mode 6 stays the vendor 30 fps
setting. Run on the board from the committed code:

| Test | Result |
|---|---|
| Mode 7 at 45 fps, 3 min | 45.00 fps ISP and encoder, 1344 MHz, no MIPI errors, VI lost none, 8 of 8 snapshots clean |
| Mode 7 at 40 fps | VMAX 0x1220, 40.01 fps, clean |
| Mode 7 at 50 fps | refused: `imx335 2-lane: fps 50.000000 outside [0.020000, 45.000000]` |
| Mode 7, mirror + flip | 44.99 fps, clean |
| Mode 6 at 30 fps | HMAX 0x0258, `INCKSEL1` 0x80, 1200 MHz, 30.01 fps: unchanged |

Those runs used a scratch waybeam build. waybeam's `imx335_2l` sensor row
carries both entries (30 fps with `sns_mode` 6, 45 fps with `sns_mode` 7), so
`video0.fps` up to 30 keeps the vendor mode and a higher request selects the
45 fps one.

### Re-run on the 0.87.0 pipeline

0.86.3 put VI, ISP and VPSS all online and left VB with the encoded-size pool
only, so the limits were measured again on that graph, `SENSOR=imx335_2l`,
1920x1080 with `isp.keepAspect=false`:

| Test | Result |
|---|---|
| The stock `imx335` row (four lanes) on this board | 0 fps; receiver measures 0x0 |
| 45 fps, 3 min | 45.00 fps ISP and encoder, 1344 MHz, no MIPI errors, 8 of 8 snapshots clean |
| `video0.fps` 30 | mode 6, 29.97 fps, clean |
| `video0.fps` 40 | answered with the 45 fps mode, substitution logged |
| 45 fps, mirror + flip | 44.99 fps, clean |
| HMAX floor (`INCKSEL1` 0x90) | 426 clean at 47.58 fps, 424 damaged: unchanged |
| ISP ceiling (`INCKSEL1` 0xA0) | 47.58 fps clean, 47.86 fps no frames: unchanged |
| Native 2592x1944 at 45 fps | sensor 44.97 fps, encoder 43.03 |
| MMZ in use | 22.5 MiB at 1080p output, 49.6 MiB at native |

Above the ceiling the loss now shows in VPSS instead of VI: its output count
freezes and `frame_err` climbs, while the VI pipe still counts an interrupt
per frame.

### Not tested

- `INCKSEL1` above 0x80 is outside every vendor table found: it overclocks
  the sensor's pixel clock and ADC timing by 12.5% (0x90) or 25% (0xA0).
  Nothing here measured noise, fixed-pattern artefacts or black level against
  the vendor mode, and nothing ran longer than 5 minutes or away from room
  temperature.
- The CSI-2 timing registers were never rescaled for the faster lanes.
- One board, one cable. The 1485 Mbps lane rate in particular says nothing
  about another layout.
- 12-bit output.
