# IMX335 on two CSI-2 lanes on Hi3516CV610: the modes and what limits them

How fast an IMX335 can run on a board that routes only two of its four data
lanes, what stops it going faster, and the four modes that came out of it.
Measured on 2026-11-21.

- Board: Hi3516CV613 + IMX335, two CSI-2 lanes on MIPI-RX PHY lanes 0 and 2,
  stock OpenIPC modules, sensor clock 37.125 MHz.
- Plugin: openhisilicon's `sony_imx335` with its two-lane modes 6 to 9.
- Software: waybeam (VI, ISP and VPSS all online) and majestic, both at
  1920x1080 H.265 unless noted. Majestic's noise-reduced snapshots are what
  the picture measurements were made on.

## The modes

| Plugin mode | Picture | Rate | Sensor clocks | HMAX | VMAX | Margins: PLL / HMAX / VMAX |
|---|---|---|---|---|---|---|
| 6 | 2592x1944 | 30 fps | Sony's two-lane setting | 600 | 4125 | vendor |
| 7 | 2592x1944 | 40 fps | Sony's, shorter line | 450 | 4125 | none needed / 5.6% / 3.5% |
| 8 | 2592x1944 | 45 fps | PLL 6.25% fast | 434 | 4040 | 2.9% / 1.9% / 1.4% |
| 9 | 2560x1440 window | 60 fps | PLL 6.25% fast | 430 | 3058 | 2.9% / 2.1% / 2.6% |

- waybeam's `imx335_2l` row lists them in this order; `video0.fps` picks the
  first row at or above the request, among those that fill `video0.size`.
- The 60 fps mode is a sensor window: full width less 32 columns, 74% of the
  height. Asking for more than 45 fps therefore narrows the vertical field of
  view. Vertical flip is not implemented in the window; mirror is.
- Only mode 6 is a vendor setting. Margins are against the limits below.

## What limits the rate

    fps = 74.25 MHz * INCKSEL1 / 0x80 / (HMAX * VMAX)

`INCKSEL1` (0x314C) scales the sensor's whole clock tree: pixel clock, ADC
and link together. HMAX is the row time in pixel-clock counts, VMAX the
frame length in half rows. Each of the three has a limit.

| Limit | Value | What sets it | How it fails |
|---|---|---|---|
| HMAX floor | 426 (421 in the 2560-wide window) | A row must leave the sensor over two lanes within its row period | Picture breaks; every counter stays clean |
| VMAX floor | rows plus about 50 blank ones: 3984 full, 2980 at 1440 rows | The SoC's VI/VPSS chain | No frames; VPSS counts a `frame_err` each |
| `INCKSEL1`, bright scene | 0x89 clean, 0x8C broken | Sensor ADC at 1x analog gain | Pattern noise in dark areas, blacks lifted |
| `INCKSEL1`, dark scene | 0x98 clean, 0x9A broken | Sensor ADC at full gain | Blue cast, column stripes |
| Row time at the ISP | 10.17 us waybeam, 10.37 us majestic | ISP clock, 264 MHz | No frames; VPSS counts a `frame_err` each |
| Encoder, native-size output | about 217 Mpixel/s waybeam, 170 majestic | VENC | Encoded rate falls below the sensor's |

With the PLL held to what the picture tolerates, the two-lane link is what
binds: 43.7 fps on Sony's clocks and 46.5 fps at 0x88 for the full sensor,
62.9 fps for the 1440-row window. The ISP limit (48 to 49 fps at 5 MP) is
only reached on faster clocks than the picture allows.

### The HMAX floor is invisible to every counter

| HMAX (full sensor, Sony's clocks) | fps | Counters | Picture |
|---|---|---|---|
| 600 | 30.0 | clean | clean |
| 450 | 40.0 | clean | clean |
| 426 | 42.25 | clean | clean |
| 425 | 42.35 | clean | damaged |
| 420 | 42.86 | clean for a 3 min soak | damaged |
| 412 | 43.69 | frame height 1906, encoder 0 fps | none |

From 425 down to 414 the receiver still measures 2592x1944, no CRC, ECC or
FIFO counter moves, VI loses nothing and the encoder runs at the full rate,
while the picture is horizontal bands, shifted and demosaiced on the wrong
Bayer phase. Only from 412 do rows go missing. **A step that passes the
counters has not passed; a snapshot is the test.**

The floor is the same count at every `INCKSEL1` (row period and row data time
shrink together), and 421 in the window, whose row is 2584 pixels instead of
2616. A row needs 852 clocks where its data alone takes 818: 34 clocks of
per-row overhead.

### The sensor PLL: two limits, and daylight is the strict one

At 1x analog gain, in a lit room with a bright window, at 45 fps timings.
Noise is the high-frequency residue in flat areas; 0x80 is the reference.

| `INCKSEL1` | Overclock | Noise, dark areas | Noise, mid areas | Picture |
|---|---|---|---|---|
| 0x80 (vendor) | – | 1.5 | 0.70 | reference |
| 0x86 | 4.7% | 1.6 | 0.70 | same |
| **0x88** | **6.3%** | **1.6** | **0.69** | **same** |
| 0x89 | 7.0% | 1.7 | 0.67 | same |
| 0x8A | 7.8% | 2.3 | 0.70 | dark areas slightly noisier |
| 0x8B | 8.6% | 1.5 | 0.66 | same |
| 0x8C | 9.4% | 16 | 1.3 | pattern noise in dark areas |
| 0x90 | 12.5% | no dark areas left | 5.6 | pattern noise, blacks lifted |

- The break is one step wide. It is the gain, not the exposure or the rate:
  at a fixed 30 fps, 0x90 against 0x80 reads 24 against 5.2 at gain 1.0x,
  6.3 against 6.5 at 1.46x, 7.0 against 6.9 at 3.2x (waybeam, no noise
  reduction).
- 0x8A read slightly high in four snapshots out of four and 0x8B did not.
  Unexplained; 0x89 is the last setting with no change at all.
- In a dark room at full gain (ISO about 250 000) the picture is unchanged
  up to 0x98 (18.8%), turns blue from 0x9A and is a blue striped field at
  0xA0.
- So a setting has to be checked at both ends of the gain range. A 45 fps
  mode on 0x90 passed every test in room light and at night, and failed in
  daylight; that is why mode 8 gets its last few percent from HMAX and VMAX
  instead.
- The receiver locked at every step, up to 1485 Mbps per lane, with the
  CSI-2 timing registers left at their reset values.

### The pipeline: row time at the ISP

Above the picture's PLL limit the pipeline has its own. It is the row time,
not the frame rate (majestic, `INCKSEL1` 0x90):

| HMAX | VMAX | Row time | Frame rate | Result |
|---|---|---|---|---|
| 450 | 4125 | 10.77 us | 45.0 | frames |
| 426 | 4400 | 10.20 us | 44.6 | no frames |
| 450 | 4000 | 10.77 us | 46.4 | frames |
| 433 | 4125 | 10.37 us | 46.8 | frames |
| 432 | 4400 | 10.34 us | 43.9 | no frames |

When it fails the sensor, the receiver and VI are clean and the ISP still
takes an interrupt per frame; VPSS reports a `frame_err` for every frame and
emits nothing. It recovers by itself when the row time is restored.

Slowing the module clocks one at a time shows which stage it is:

| Clock changed | Smallest working HMAX | Row time | In that clock's cycles |
|---|---|---|---|
| none: ISP 264 MHz, VICAP 475 MHz | 433 | 10.37 us | 2737 ISP |
| ISP0 (CRG 0x9150) to 198 MHz | 577 | 13.82 us | 2735 ISP |
| VI-proc (0x9740) to 198 MHz | 433 | unchanged | – |
| VPSS (0x9b40) to 198 MHz | 433 | unchanged | – |
| VICAP (0x9140, 0x9164) to 198 MHz | 561 | 13.43 us | 2659 VICAP |
| none, 1485 Mbps lanes | 481 | 10.36 us | 2737 ISP |

- The ISP clock moved the threshold by exactly 264/198. A faster link did
  not move it at all.
- The capture stage has its own limit and signature (MIPI write-FIFO errors,
  garbage measured width); at its real 475 MHz it is 5.6 us per row, half of
  what these modes use.
- waybeam needs 2685 ISP cycles per row and majestic 2737. Majestic runs 3DNR
  and line compression in the VI pipe; waybeam runs neither.
- ISP clock selects above 264 MHz stop the pipeline. Two pixels per clock is
  refused by MIPI-RX and by VI. VI offline, VPSS online is refused; both
  offline starts, takes 20.4 ms per 5 MP frame (the same ceiling) and needs
  about 50 MB more buffers.
- By the same arithmetic an 8 MP sensor tops out near 30 fps on this ISP.
  That is a calculation, not a measurement.

### Blank rows

The 30 fps table carries 119 blank rows per frame. The pipeline keeps
delivering down to about 48 (VMAX 3984 for 1944 rows, 2980 for 1440) and
drops every frame below. The sensor itself only starts losing rows lower
still. This is where modes 8 and 9 take the margin the PLL cannot give.

## The window

Registers by the datasheet rules, which are also what the SigmaStar
infinity6e driver's four-lane 2560x1440 table uses, except that it starts at
column 48:

| Register | Value |
|---|---|
| WINMODE 0x3018 | 0x04 |
| HTRIMMING_START 0x302C | 60 (the allowed step nearest the centre) |
| HNUM 0x302E | 2584 = W + 24 |
| Y_OUT_SIZE 0x3056 | 1460 = H + 20 |
| AREA3_ST_ADR_1 0x3074 | 680 = 176 + (1944 - H) |
| AREA3_WIDTH_1 0x3076 | 2920 = 2 * Y_OUT_SIZE |
| UNREAD_ED_ADR 0x30D8 | 3808 = start + width + 208 |
| 0x30C6 / 0x30CE | 0x12 / 0x64 |

The rate follows the rows, not the pixel count. The row time barely moves
(32 columns fewer); the frame is 1440 rows plus 50 blank against 1944 plus
48, 1.34 times fewer, and 45 fps times 1.34 is 60. A 2560x1920 window was
measured too: 46 fps, one more than the full sensor, and not kept.

## Tests

From the plugin source, clean starts, daylight at 1x analog gain,
1920x1080 H.265:

| Test | Result |
|---|---|
| Mode 6, 30 fps | 30.0 fps; HMAX 600, VMAX 4125, `INCKSEL1` 0x80, Sony's values |
| Mode 7, 40 fps, majestic, 3 min | 40.0 fps throughout, no lost frame; 40.00 fps at the RTSP receiver |
| Mode 8, 45 fps, majestic, 3 min | 45.0 fps throughout, no lost frame; 44.99 fps at the RTSP receiver |
| Mode 9, 60 fps, majestic, 5 min | 60.00 fps throughout, no lost frame; 1199 frames in 20 s at the RTSP receiver, evenly 16.7 ms apart |
| Mode 9 at 50 and 30 fps | VMAX 3669 and 6116, 49.95 and 30.0 fps |
| Mode 9, mirror / flip | mirrored / stays upright, 60 fps both |
| Picture noise, dark areas, modes 6 / 7 / 8 | 1.2 / 1.3 / 1.5: no difference beyond the scene's |
| waybeam, `imx335_2l`, 30 / 40 / 45 / 60 fps | 30.01 / 40.06 / 45.04 / 60.01 fps at the ISP and the encoder, no MIPI error, snapshots clean; 60 fps held 3 min; 21.9 MiB of MMZ |
| waybeam, mirror + flip at 45 fps | 44.99 fps, clean |
| waybeam, flip asked for at 60 fps | upright, the plugin logs why, 60 fps |
| waybeam, 50 fps asked for | answered with the 60 fps window, substitution logged |
| waybeam, 60 fps at 2592x1944 output | stays on mode 8: sensor 45 fps, encoder 43.3 |
| The four-lane `imx335` row on this board | 0 fps; receiver measures 0x0 |

Native-size output is encoder-bound: 2592x1944 at 45 fps delivers about 43
fps under waybeam and 34 under majestic; 2560x1440 at 60 fps delivers 58.6
and 45.

## Not measured

- Temperature, and any board but this one. The PLL limit is analog and will
  move with heat; the HMAX and VMAX floors are digital, repeatable to one
  count, and derive from the same crystal as the row they bound.
- Anything longer than 5 minutes.
- Modes 8 and 9 at night. Every `INCKSEL1` up to 0x98 was clean at full
  gain, at 30 fps timing; the 45 and 60 fps timings were not run in the dark.
- Vertical flip in the window, 12-bit output, HDR.
