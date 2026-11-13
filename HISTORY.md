# History

## [0.86.3] - 2026-11-13

CV610 stops allocating frames nothing reads: VI, ISP and VPSS now run online.

- **VB holds only VPSS's output.** VI already ran online into the ISP, so the
  RAW pool (4 capture-size frames at 2 bytes/pixel) was never touched
  (`min_free == blk_cnt` in `/proc/umap/vb`), and VPSS offline made the ISP
  write a full-size YUV frame for VPSS to read back. VPSS now runs online too,
  as in the vendor's own CV610 sample, and VB is one pool of 4 output-size
  frames. Measured on a Hi3516CV610 + IMX662, MMZ in use: 36.9 → 21.0 MiB at
  1080p30, 38.3 → 10.4 MiB at 720p100 and 44.1 → 16.3 MiB at 1440x1080@100,
  each at its nominal rate with 0 RTP loss (29.98, 100.02 and 99.99 fps;
  1080p100 100.01). On a Hi3516CV610 + IMX335 (DmtryJS, `hisi_mem` 96 MiB
  zone) at 1080p output: 89.6 → 22.2 MiB for 2592x1944 (modes 0 and 3),
  74.5 → 21.7 MiB for 2592x1520, 36.8 → 20.9 MiB for 1080p90, and the binned
  row at 720p 27.0 → 10.0 MiB, all five at their nominal rate. 5 MP output
  (`video0.size=2592x1944`), which used to get the daemon OOM-killed, now
  runs at 30 fps in 49.3 MiB.
- **The aspect crop moves to the VI pipe's post-ISP crop.** With VPSS online
  the group crop is refused outright (`0xa0078007`) and the channel crop is
  applied short (1440 wide at x=240 came out 1200 wide, so VENC dropped every
  frame); the VI channel crop is `NOT_SUPPORT` on this chip. The crop is an
  internal rectangle VPSS scales to `video0.size`, the ISP still meters the
  whole capture, and a 1440x1080 snapshot is centre-cropped, not squashed.
  The VI post-crop needs a 4-pixel grid (1280x728 asked for a 1898-wide crop
  and was refused, "crop width should be 4 align"), so the crop is rounded to
  it on CV610 only; 1280x728@100 then ran at 100.03 fps.
- The VI-offline graph is gone: it was unreachable (`vi_online` was always 1)
  and would have needed the VI channel resized for the new crop. The VPSS
  log line now reports VPSS's real input.

## [0.86.2] - 2026-11-12

The CV610 module loader takes its MMZ zone from the kernel that laid it out,
instead of naming one from the bootloader's side.

- **Fixed: `load-cv610-online` loaded no modules at all on an OpenIPC CV610
  image whose kernel has `hisi_mem`** (OpenIPC/firmware #2638, October 2026,
  and later). It passed `mmz_allocator=ot`, which is right only against a
  bootloader-driven layout: such a kernel measures DRAM and
  reserves the zone inside kernel memory through CMA, so `open_mmz` refused
  it (`Conflict MMZ: PHYS(0x42000000, 0x47FFFFFF)` / `MMZ conflict to kernel
  memory (0x40000000, 0x47FFFFFF)`), `load mmz failed!` ended the sequence,
  and the craft came up with no MPP modules -- a boot waybeam cannot start
  on. The loader now reads the allocator and the zone from
  `/sys/module/hisi_mem/parameters/{allocator,mmz}` when the kernel publishes
  them, the same source OpenIPC's own `load_hisilicon` uses, and keeps `ot`
  (or `cma`, for a `mmz=` carve-out) on a boot without `hisi_mem`.
  `CV610_MMZ_START`/`CV610_MMZ_SIZE` apply only in that second case, and
  only when the command line does not name the carve-out with `mmz=`; the
  loader says which of the two overrode them.
- **Measured, not estimated: the 2592x1944 IMX335 rows take 94 MiB of MMZ**
  at 1080p output, against 81 MiB for 2592x1520, 44 MiB for 1080p and 34 MiB
  for the binned row, with 0.86.1's VB pool sizing. On a `hisi_mem` kernel
  `mem=` and the u-boot env's `totalmem`/`osmem` are ignored, so the docs and
  the platform conf now point at OpenIPC's `set_allocator` plus a reboot to
  move the split; without `hisi_mem` the zone still grows only with a smaller
  `mem=` and `CV610_MMZ_START`/`CV610_MMZ_SIZE` moved with it.
- **Hardware:** the loader and `drivers/cv610` were run on a Hi3516CV610 +
  IMX335 board. All five modes come up, the daemon's `sns0_clk_hz` write takes
  (`37125000 -> CRG 0x8440 = 0x8010`), and every row delivers its nominal
  rate at 1080p output: 30.1, 40.2, 60.1, 30.1 and 90.2 fps.

## [0.86.1] - 2026-11-11

The kernel half of the CV610 sensor chain now lives here too, so 1080p100 is
buildable from this tree rather than only from the snokvist/firmware
package that carried it.

- **`drivers/cv610/`** holds the two modules `init.d/load-cv610-online`
  loads from `/usr/lib/cv610`: OpenHisilicon's hi3516cv6xx sys_config with
  `sys_config-runtime-sensor-clock.patch` (the writable `sns0_clk_hz` the
  daemon sets per mode, 27 MHz for 1080p100), installed as
  `open_sys_config_imx662.ko`, and `open_pm_stub.ko`, which registers MPP
  module 58 without touching hardware. Neither is in OpenIPC. Both come from
  the snokvist/firmware CV610 bring-up, where 30/60/90/100 fps were each
  measured at their nominal rate on one boot.
- **`make drivers-cv610 KSRC_CV610=... CV610_SDK_INC=...`** builds them
  against a configured hi3516cv6xx 5.10 kernel (never fetched, like
  `KSRC_MARUKO`); `make stage` stages them into `out/cv610/cv610/` when
  built, and says so when not.
- **CI builds them** (`kmod-cv610`) against the pinned OpenHisilicon and
  OpenIPC `hisilicon-hi3516cv6xx` kernel, so the patch cannot silently stop
  applying.

## [0.86.0] - 2026-11-11

**CV610 drives a Sony IMX335 as well as the IMX662**, chosen from the u-boot
`sensor` name. No config field, no API change: `contract_version` is
unchanged. Built from DmtryJS's OpenIPC/waybeam#126 and #127, cut down to what
this tree needs now that the IMX335 driver lives in OpenIPC/openhisilicon.

- **`$SENSOR` picks the plugin, the ISP sensor id and the mode table.** It is
  the u-boot name OpenIPC's rcS exports and its module loader brings the
  sensor up by; `S95waybeam` asks `fw_printenv -n sensor`, then
  `ipcinfo --short-sensor`, when started without it. A missing or unknown
  name comes up as IMX662 with a `WARNING`, and bring-up logs
  `> CV610 sensor: <name> (<plugin>)`. Switching parts is
  `fw_setenv sensor <name>`. Validation and `/api/v1/modes` read the same
  table, so `/api/v1/set` accepts exactly the modes of the part fitted.
- **IMX335 uses the image's plugin** (`/usr/lib/sensors/libsns_imx335.so`,
  OpenIPC/openhisilicon `sony_imx335`, on `ultimate` images) with five fixed
  modes: 2592x1944p30 RAW12, 1296x972p60 binned, 1080p90 crop, 2592x1944p40
  RAW10 and 2592x1520p30 crop, all on 37.125 MHz. Vertical flip is refused in
  the two crops, and their `/api/v1/modes` labels say so. `pub.sns_mode`
  stays 0: the plugin tells the rows apart by size and rate.
- **Mode selection matches the rate among modes that capture the encoded
  size**, which SigmaStar's `sensor_select()` also prefers (the remaining
  differences are listed at `cv610_mode_select()`). IMX662 is unaffected
  (every mode is 1080p); on IMX335, 1080p60 runs the 1080p90 crop instead of
  being rejected as an upscale of the binned mode. `video0.size=auto` encodes
  the capture rounded down to a multiple of 8 (1296x968 on the binned mode).
- **On waybeam's own `load-cv610-online`,** the IMX335 needs the
  `sns0_clk_hz` sys_config for its clock, and its 2592-wide rows may need
  more than the default 64 MiB MMZ (an unmeasured estimate of 60-80 MiB of
  VB), which on a 128 MiB board is already all the RAM above the kernel; if
  so, selecting one there fails bring-up. `CV610_MMZ_START` and
  `CV610_MMZ_SIZE` are now passed through from `/etc/waybeam-cv610.conf`, to
  move together with the kernel's `mem=` on a board laid out for it. 5 MP
  *output* (`video0.size=2592x1944`) runs a 128 MiB board out of memory.
- **Upgrade note: the IMX662 plugin moves to `/usr/lib/cv610/libsns_imx662.so`**
  and is staged at `out/cv610/cv610/`. OpenIPC images now install their own
  copy at `/usr/lib/sensors/`, which refuses 1080p100; keeping waybeam's out
  of that path means a sysupgrade cannot swap it in. A binary updated without
  its plugin fails with `dlopen /usr/lib/cv610/libsns_imx662.so`; a firmware
  package installing the old staged path now fails at build time instead.
- Hardware-verified by DmtryJS on a Hi3516CV610 + IMX335 with this code
  (`bc6af4a`): all five modes, mirror/flip, `/api/v1/modes` and the init
  script's sensor resolution. That run used a stock OpenIPC image's module
  loader and memory layout, not `load-cv610-online`.

## [0.85.8] - 2026-11-11

The CV610 IMX662 plugin takes the fixes its OpenHisilicon copy gained in
review (OpenIPC/openhisilicon#282 vendored it), and keeps what makes this the
whole-chain copy: **1080p100 stays**, on the 27 MHz clock the daemon selects.

- **The ISP black level is 800, not 200.** The ISP's black level is a fixed
  14-bit domain and the sensor's BLKLEVEL of 50 counts 10-bit LSBs: FRAMOS's
  calibration gives it 200/4095 at 12 bits, and IMX335, sharing the `0x032`
  default, pairs it with `0x320` on this ISP. 200 read it as a 12-bit
  pedestal and left ~600 codes unsubtracted, which the WB gains lift into
  blue. Code-level evidence only: **not yet run on an IMX662 board**. A
  capped-lens raw capture averaging ~200 at RAW12 confirms it, one line
  reverts it, and a visible shift is expected (deeper blacks, a different
  AWB and low-light AE settle). The `iq-profiles/cv610-bin/imx662.bin`
  restore point still carries 200.
- **A frame-rate change resets the running frame length** (`fl[0]`, AE's
  `full_lines`). The shutter is computed against `fl[0]`, so a stale one
  offset exposure once VMAX had moved. Bring-up publishes each mode's own
  rate, so this matters for a runtime ISP rate change.
- **A failed bring-up says which step failed.** `imx662_init()` checks the
  bus open and the register sequence, and a failed AE/AWB callback
  registration unregisters what did register (and the later unregister no
  longer overflows summing the errors).
- The plugin links `-lm` for its gain conversion (no change to the shipped
  binary on musl, where libm is part of libc).
- Not taken from upstream: its refusal of 100 fps, which assumes the stock
  37.125 MHz sys_config clock.
- The plugin README records that OpenHisilicon has since replaced its copy
  with the module vendor's driver (#296, 30/60/90 fps), which also uses a
  black level of 800.

## [0.85.7] - 2026-11-11

Fixes the findings of the upstream Qodo reviews of the 0.85.6 sync. No
contract change — `contract_version` stays **0.32.0**.

- **CV610 spun on the CPU while gated.** The closed gate leaves the encoded
  frame in VENC's output FIFO, so `venc_fd` stayed readable and `select()`
  returned at once instead of waiting its 2 ms — a busy loop for the whole
  closed interval, taking CPU from the very consumer whose slowness closed the
  gate. While gated the loop now waits on the timeout alone, matching Star6E
  and Maruko.
- **Gate status could report a bogus `gateClosedMs`.** The HTTP thread read
  the gate's state, timestamps and counters while the stream loop wrote them.
  Besides tearing 64-bit values on 32-bit ARM, a reopen zeroed
  `closed_since_us`, so a status read that overlapped it could report the whole
  clock epoch as closed time. The fields are now written and read with atomics,
  and the reopen leaves `closed_since_us` alone (it is only meaningful while
  closed).
- **`make test-werror` stopped running the CV610 validation suite** when 0.85.6
  gave it its own binary. It runs again.
- **Maruko logged a WARNING naming the removed `video0.frameGate` key** on every
  non-frame-shm start. It now prints the same NOTE as Star6E and CV610.
- **`frame_shm_rate_consumer` busy-spun on an empty ring**, distorting the very
  measurements it exists for on a single-core SoC. It backs off 1 ms instead.
- The config-defaults comment no longer says the gate is "off by default"; it
  has no switch and arms on every frame-shm output.
- **A negative frame-gate value in a config file was accepted as the
  default.** `load_video0()` clamped it to 0 ("use the frame_gate.h default")
  before validation could see it. It now reaches validation, and the load
  fails with the field's range error. Covered by `test_venc_config`.
- `HTTP_API_CONTRACT.md`: the UDP/Unix `transport/status` example no longer
  shows the four `gate*` fields (they are frame-shm only, as the field table
  says), and the `/api/v1/config` example shows the configured `0` defaults
  rather than the effective 3 / 500.
- Removed `documentation/ADAPTIVE_FRAME_GATE_HANDOFF.md`: a bench-session
  handoff, not maintained reference. The gate's behaviour is documented in
  `include/frame_gate.h`, `HTTP_API_CONTRACT.md` and the earlier 0.85.x entries.
- **Star6E dual ch1 no longer gets the 2-frame bitstream cap.** The gate only
  acts on ch0, so the cap bought ch1's recorder nothing and cost it one frame
  of storage-stall tolerance; ch1 is back on the SDK default depth.
- A concurrency test in `test_frame_gate` runs status reads against a writer
  thread. Mutation-checked: restoring the reopen's `closed_since_us = 0` fails
  its bound check and draws a ThreadSanitizer race report.
- Removed `frame_gate_restore_closed()`, which no backend called, and the
  redundant open-check before each `frame_gate_force_open()`. Comments that
  still described the retired `StopRecvPic` actuator now describe the drain
  stall. The README's stream-buffer note says the cap shipped (it did, in
  0.85.0); CV610's deferred-phases list drops the frame-SHM throttling item it
  already has; `REVIEW_FIX_VERIFICATION.md` uses placeholder SDK paths.

## [0.85.6] - 2026-09-14

Follow-ups to the two-week adversarial review: four behavioural fixes its
first pass missed, plus two build-gate traps. No contract change —
`contract_version` stays **0.32.0**.

- **CV610's gated loop froze the egress ring's low-water gauge.** While the
  frame gate was closed the drain loop `continue`d before
  `cv610_service_ring_low_water()`, so the 200 ms `low_water_slots` export
  waybeam-link's rate model reads held its last value for the whole closed
  interval (up to `frameGateMaxClosedMs`, 60 s) — hiding exactly the
  congestion that closed the gate. The gated branch now reads the ring fill
  and services the gauge, matching Star6E and Maruko. The latched pressure
  flag is deliberately left frozen: `venc_observe_pressure()` counts frames in
  pressure, and both other backends sample it per-frame (and, on Star6E, only
  under a sidecar subscription), so advancing it on the ~500 Hz idle poll
  would inflate `pressure_drops`.
- **The two frame-gate knobs were validated only on the live API path.**
  `frameGateCloseSlots` / `frameGateMaxClosedMs` carry 0-64 / 0-60000 rules in
  `validate_field_cfg()`, but the keys were missing from
  `venc_api_validate_loaded_config()`'s sweep, so a config FILE could carry any
  `uint32`. `frameGateMaxClosedMs = 4294968` then wrapped the `*1000` in
  `frame_gate_resolve()` to 704 µs, which the debounce floor raised to 20 ms,
  making the safety escape fire almost immediately. Both keys are now in the
  sweep and the multiply saturates in 64 bits. Covered by `test_frame_gate`
  and `test_venc_api`.
- **Maruko kept the SDK's 3-deep bitstream buffer under `framing=stab-fill`.**
  The `SetMaxStreamCnt(FRAME_GATE_STREAM_BUF_FRAMES)` call sat below the
  `g_stab_fill_graph` early return, so the stab-fill path never capped it —
  contradicting 0.85.0's "all three backends". The cap now runs before the
  early return, so both `StartRecvPic` paths get it.
- **A drained reopen at the backstop deadline was counted as a safety
  escape.** With `max_closed_ms` at or below the 20 ms debounce the two
  deadlines coincide, and the escape test ran first, so a normal drain reopen
  incremented `gateEscapeEvents` and armed a spurious pulse. The normal drain
  reopen is now evaluated before the escape.
- **`make test-werror` compiled nothing.** Its `-Werror` lived in a
  target-specific `HOST_CFLAGS` with no prerequisite that forced a rebuild, so
  once `tests/test_runner` existed the target only re-ran the non-werror
  binary — a false green of the same class 0.85.2/0.85.3 set out to close.
- **`test-asan`/`test-tsan` overwrote the normal test binary.** Compiling each
  variant to `tests/test_runner` left the last instrumented build behind, so a
  later `make test` ran it. Each now builds a distinct
  `tests/test_runner.{werror,asan,tsan}`.
- **Verified the CV610 `.bin` import bound, and documented the export
  slack.** Disassembling `libbin.so` confirmed `PQ_BIN_SetNRDataV2` rejects any
  NRX count outside [1,16] before its fixed 1298-byte copy, so the 1410-byte
  reader reach the 0.85.4 loader sizes for is a true ceiling, not an
  assumption. The export side's 64-byte slack already covers the vendor's
  60-byte overhang.

## [0.85.5] - 2026-09-13

Documentation-only follow-up to 0.85.4. No contract change —
`contract_version` stays **0.32.0**.

- **Corrected the hardware verification runbook** in
  `documentation/REVIEW_FIX_VERIFICATION.md` after the 0.85.4 fixes were
  exercised on the benches (CV610 `192.168.2.181`, Maruko `192.168.2.233`).
  All three findings passed; the run record is now in the doc and six
  procedure inaccuracies are fixed: the HTTP API takes one field per request
  (a combined query is 400), each restart-class set needs its own request and
  a settle, `record/start?dir=` requires the target directory to exist, the
  patched count=16 `.bin` is refused with HTTP 500 rather than imported, the
  gate reopens at `frameGateMaxClosedMs` and stays open with a dead consumer,
  and the observed sidecar RTT is ~250 µs rather than ~1 s.

## [0.85.4] - 2026-09-13

Fixes the first three findings of the two-week adversarial review. No contract
change — `contract_version` stays **0.32.0**.

- **CV610 `.bin` import could read past its heap buffer.** The 3DNR half is
  gated on the section length `OT_PQ_GetStructParamLen()` reports, but the
  vendor reader (`PQ_BIN_SetNRDataV2`, `memcpy_s` at `0x12fc`) copies a fixed
  1298 bytes from an offset that grows with a per-file count: the 36-byte
  `OT_PQ_BIN_NRX` sub-header plus `12 + 4*count`, so it reaches up to 1410
  bytes from the section start — 60 more than that gate's 1350. A section that
  passed the gate could therefore let the reader end up to 60 bytes past the
  allocation and feed those bytes to `ss_mpi_vi_set_pipe_3dnr_param`. The file
  loader now allocates `CV610_PQ_NRX_VENDOR_MAX` (the reader's 1410-byte reach)
  of zeroed slack past the file (`src/cv610_pq_bin_load.c`), so the bound holds
  whatever the gate reports; the gate itself is unchanged, so valid tunes are
  not rejected. New `test_cv610_pq_bin_load` performs the reader's worst-case
  copy out of the loaded buffer, which `make test-asan` / CI catch as a
  heap-buffer-overflow on an under-sized allocation.
- **Maruko starved record control and the sidecar while the frame gate was
  closed.** Both were serviced only inside
  `maruko_pipeline_process_stream()`, which the gated branch of
  `maruko_pipeline_await_frame()` returns before, so a `record/start` waited for
  an escape pulse (up to `frameGateMaxClosedMs`). The record-control block is
  now `maruko_service_record_control()`, called from both paths, and the gated
  branch wakes on the sidecar via `idle_wait()` instead of a bare `usleep`.
- **CV610 advertised the frame-gate tunables as unsupported.** `cv610_runtime.c`
  feeds both to `frame_gate_setup()` but neither was in the CV610 allowlist, so
  `/api/v1/set` returned 501 and the dashboard greyed controls the backend
  reads. `test_venc_api` now asserts them on CV610 (with star6e/maruko crosses)
  and `scripts/api_test_suite.sh` checks them on a live CV610.
- Host verification and the hardware procedures for all three are in
  `documentation/REVIEW_FIX_VERIFICATION.md`; the benches were offline when the
  fixes landed, so the hardware steps are unrun.

## [0.85.3] - 2026-09-13

Completes the 0.85.2 build fix, which closed only part of the hole it
described. No contract change — `contract_version` stays **0.32.0**.

- **The stale-test-binary hole was still open for five headers.** 0.85.2
  replaced a 20-header list with `$(wildcard include/*.h)`, but the tests also
  compile from `lib/`, `tests/` and `tools/qr/`. Appending `#error POISON` to
  `lib/cJSON.h` and re-running still printed "3147 passed, 0 failed" — the
  binary never recompiled. `TEST_HEADERS` now globs every tree the oracles
  reach, and each of the five is poison-verified to force a rebuild.
- **`$(QR_TEST_RUNNER)` had the same hand-picked list** and is in the `verify`
  path, so a stale QR oracle meant a green `make verify`. It and
  `$(QR_HOST_DECODE)` now share `TEST_HEADERS`.
- **The capacity test was blind to the case it advertised.** It looked for
  `gateClosedMs` by name to prove the tail was intact, which cannot see a field
  APPENDED after it — the old name is still present, just no longer last.
  Appending two fields without growing the cap truncated the output at 191 of a
  needed 213 bytes and the suite still passed. `frame_gate_status_json()` now
  returns snprintf's would-be length, and the test asserts that instead, which
  tracks the format string with no field names in it. `-Wformat-truncation`
  cannot cover this: `cap` is a runtime parameter, not a constant.
- **Dropped a tautological assertion.** `strlen(js) < sizeof(js)` cannot fail
  for any snprintf output, since it always terminates within `cap`.
- **Corrected the 0.85.2 arithmetic.** The fragment's worst case is 112 bytes
  including the NUL, not 115: `gateClosedMs` is `closed_us / 1000`, so 17
  digits rather than 20. The 128-byte buffers it replaced were therefore
  sufficient, and 0.85.2 was a locality fix rather than the near-miss its notes
  implied. The reason for the change is unaffected: one added field would have
  overrun all three.

## [0.85.2] - 2026-09-13

Two latent build and buffer faults found while evaluating a new gate counter.
No behaviour change on device, and no contract change — `contract_version`
stays **0.32.0**.

- **The gate status buffer was sized by three hand-copied constants.** All
  three backends declared `char gate_json[128]` for the fragment
  `frame_gate_status_json()` writes. The worst case is 112 bytes including the
  NUL, so 128 was sufficient and no truncation was ever reachable — what was
  wrong is that the number lived three files away from the format string that
  determines it, with 16 bytes of headroom, so the next field added would have
  overrun all three at once and done it silently.
  `FRAME_GATE_STATUS_JSON_CAP` now lives beside the format string.
- **The test runner depended on a hand-picked list of headers.** It named 20
  and was missing 34 of the headers its sources include, `include/frame_gate.h`
  among them. The tests build as one compiler invocation, so no `.d` files
  cover this the way `-MMD` does for the cross builds. The failure mode is a
  **false PASS**: change only a header and the suite runs against the previous
  code and reports green. Measured on the fix itself — an undersized-buffer
  mutation reported "3147 passed, 0 failed" before and "3146 passed, 1 failed"
  after, from the identical edit.
- **First test coverage of the status fragment**, which previously had none,
  including the leading comma the three callers splice on.
- **Documented how to read the gate on a live device.** A `gateEscapeBackstops`
  counter was considered and rejected as redundant: the ring counts a frame in
  `framesSent` only when the write lands, so `framesSent` flat while
  `gateEscapeEvents` rises already means "escape pulses firing, nothing getting
  through", and `transportDrops` then separates a full ring from a stopped
  encoder. Verified on `.232` by transitioning a live craft in both
  directions.

## [0.85.1] - 2026-09-13

A latency fix for the adaptive frame gate shipped in 0.85.0. No contract
change — `contract_version` stays **0.32.0**, and no endpoint, payload or
field changes.

**Device-verified A/B on all three backends**, pre-fix and fixed binaries
built from the same tree and identified by md5 at the device, with the
rate-limited frame-shm consumer as the sole ring reader.

- **The safety escape now admits one frame, not a burst.** When the gate has
  been closed for `max_closed_ms` it force-opens so a frame reaches the ring
  and `full_drops` reports a dead consumer instead of a silent stall. That
  pulse used to stay open for a fixed 20 ms, which on a slow producer let the
  drain loop flush its entire buffered backlog through as a burst. The pulse
  now ends on the first frame that actually lands.
- **Maruko, 30 fps against a 5 fps drain:** mean ring occupancy 6.29 -> 1.64
  slots and minimum frame age 1460 -> 726 ms. The gate had been running
  permanently on the safety pulse — close and escape counters advanced in
  lockstep, one apart — because each pulse admitted ~2.9 frames, exactly
  replacing what the consumer took, so occupancy never fell back to the
  reopen threshold. Escapes are now flat while closes continue normally.
- **Delivered rate and judder are unchanged** (5.6 /s at spread 1 in both
  arms), and both arms emit **zero keyframes**. The improvement is latency,
  not throughput.
- **Star6E and CV610 are unaffected**, as expected: both reach the reopen
  threshold through the normal drain path, so the escape branch is almost
  never taken. Ring occupancy 1.52 -> 1.52 at 100->25 fps and 1.55 -> 1.54 at
  100->15 fps.
- `FRAME_GATE_MIN_OPEN_US` is renamed `FRAME_GATE_ESCAPE_BACKSTOP_US`: it is
  now a maximum rather than a minimum, covering only the case where the
  admitted frame never arrives (stopped encoder, or a full ring that drops
  the write).

## [0.85.0] - 2026-09-12

An IDR-free way to shed load. `contract_version` **0.31.0 -> 0.32.0**: the
`video0.frameGate` switch is removed (so `/api/v1/set` now rejects it and
`/api/v1/config` no longer carries it), and `/api/v1/transport/status` gains
four `gate*` fields on frame-shm outputs. Two restart-required `video0` fields
remain.

**Device-verified on all three backends and over a real RF link.** Star6E
(SSC338Q, 720p100, `resilience=racing`), Maruko (ssc378qe, 1080p30) and CV610
(IMX662, 720p100), each with proven stall cycles; then `.232` transmitting to
an x86 ground that latched and claimed the craft, with the link's MCS pinned
and its bitrate controller disabled so the offered rate genuinely exceeded
what the radio could carry.

- **Adaptive frame gate, always on for frame-shm outputs.** While the egress
  ring is backed up the backend stops draining the encoder's output FIFO, and
  resumes when the consumer catches up. **No SDK call is made in either
  direction**, and that is the whole design.
- **Pausing intake instead emits a keyframe on every resume.** Measured, one
  IRAP picture per reopen on Star6E (`MI_VENC_StopRecvPic`/`StartRecvPic`),
  three on Maruko, and 0.6 on CV610 (`ss_mpi_venc_stop_chn`/`start_chn`) —
  against a natural background of about one per 40 s. On a real
  capacity-limited link it reached **~4.6 keyframes per second**, which is the
  craft firing its largest frame into a saturated radio several times a
  second: the exact pathology the feature exists to prevent. Letting the FIFO
  backpressure instead measured **zero** induced keyframes on every backend
  and on the live link.
- **The gate is observable.** `/api/v1/transport/status` reports `gateClosed`,
  `gateCloseEvents`, `gateEscapeEvents` and `gateClosedMs` on frame-shm
  outputs. Without them "cycling normally under load", "escape firing against
  a dead consumer" and "stream stopped" are indistinguishable from outside;
  `gateEscapeEvents` climbing on its own is the dead-consumer signature.
- **There is no enable switch.** `video0.frameGate` is gone. The gate arms
  itself whenever the output is a frame ring, because the ring is the
  occupancy signal it runs on and no other transport has one. Only
  `frameGateCloseSlots` and `frameGateMaxClosedMs` remain.
- **It trades latency for continuity, and that is visible.** Nothing is
  dropped while gated — frames accumulate in VENC's output FIFO — so the image
  stays clean and continuous but runs several hundred ms behind on a heavily
  oversubscribed link (operator-confirmed). Attributed: the delay is the
  encoder's bitstream buffer depth times the consumer's frame period — a
  near-constant ~5.2 consumer-frame-intervals, 4.1 ms ungated, 170 ms at a
  30 fps drain, 538 ms at 10 fps, of which the ring holds only 1.5 frames.
  The encoder's bitstream buffer is now capped at 2 frames
  (`FRAME_GATE_STREAM_BUF_FRAMES`, SDK default 3), which takes a 10 fps drain
  from 534 to 436 ms. It is continuity-safe because the SDK drops the pending
  image *before* encoding, and 2 rather than 1 keeps one slot of tolerance for
  a consumer that stalls — mirror-mode recordings share the channel. Verified
  on an SD card: 128 s at 100.0 fps, 0 dropped frames, writer never backed up,
  and the file decodes with zero errors.
- **Under partial congestion it settles at the link rate.** Against a
  rate-limited consumer the delivered rate tracked 20, 40, 60 and 80 fps with
  a spread of at most one frame per second, holding the ring at ~1.5 of 8
  slots — graceful degradation with no queueing latency, not 0/100 judder.
- **The safety escape needed a dwell to work at all.** It reopened while
  occupancy was still above the close threshold, so the next observation —
  1 ms later on Star6E, against a 10 ms frame period — closed again before the
  admitted frame could exist. A dead consumer therefore stopped the stream
  with *every drop counter at zero*. `FRAME_GATE_MIN_OPEN_US` holds the escape
  pulse open for 20 ms, and only the pulse, so burst response is unchanged:
  `transportDrops` now climbs instead of the stream going quiet.
- **A bitrate write is the only proportional rate actuator on SigmaStar and
  `MI_VENC_SetChnAttr` keyframes on its own** (measured on SSC338Q: ten spaced
  `video0.bitrate` writes, eleven IRAP access units), so the standard response
  to congestion injects the largest frame in the stream into the link that is
  already overflowing. The gate changes no encoder state at all, so ROI keeps
  its relative QP gradient and CBR keeps its contract.
- **No IDR-free proportional QP lever exists to substitute**, which is why
  this is a gate and not a rate write: `qpDelta` writes `s32IPQPDelta` and
  only redistributes bits between I and P, and `min_qp` abandons CBR at a
  cliff (19.58 Mbps -> 0.63 between 20 and 24 on the README bench) rather than
  scaling.
- **Signal is the instantaneous per-frame ring occupancy, not the published
  `low_water_slots`** — that is a 200 ms window, right for waybeam-link's rate
  model and far too slow to catch a burst. The existing per-frame ring read in
  the stream loop already owns the VENC channel, so the gate needs no poller
  and no extra thread. The 200 ms export is unchanged; rate policy still lives
  in waybeam-link and the gate only covers the transients between its
  decisions.
- **Hysteresis is closed-biased:** close at `>= 3` slots, reopen at `<= 1`
  (pinned — the ring's healthy idle occupancy is one frame, not zero) after a
  20 ms debounce, with a `frameGateMaxClosedMs` safety escape (default 500 ms)
  so a dead consumer cannot stop the stream for good. On escape the ring
  overflows and reports `full_drops`, which is diagnosable; a silently stopped
  stream is not.
- **Mirror-mode recordings are protected.** Where the recorder shares ch0,
  closes are suppressed while a recording is actually running, so congestion
  never punches holes in the SD file. Reopens are never suppressed.
  `dual`/`dual-stream` put the recorder on ch1 and are unaffected. (The plan
  called for refusing the gate outright in mirror mode; `mirror` turned out to
  be the *default* record mode, which would have made the feature unreachable.)
- **Maruko needed two guards Star6E did not.** Its idle path aborts the stream
  loop after 20 s without encoder data and warns at 1 s — a closed gate looks
  exactly like a stalled encoder — so both are suppressed while gated. And its
  fd wait polls at 1 s, which would have become the reopen latency; that drops
  to 2 ms while closed.
- **Two SDK routes were tried and measured dead**, and are not shipped:
  `MI_VENC_EnableIdr(false)` does not suppress the resume keyframe (12 IRAP
  NALs with it on, 12 with it off), and `MI_VENC_SetFrameLostStrategy` is
  inert on both SigmaStar boards — a 1 kbit/s threshold against a live
  7 Mbit/s stream changed nothing, and Maruko returns success while doing
  nothing.
- `src/frame_gate.c` is a pure state machine with no SDK types, unit-tested on
  the host like `src/intra_refresh.c`, and it now also owns the actuator so
  the three backends cannot drift.
- **All three backends.** CV610 uses `ss_mpi_venc_stop_chn` /
  `ss_mpi_venc_start_chn(recv_pic_num=-1)` — the same primitive the SigmaStar
  backends reach as `MI_VENC_Stop/StartRecvPic`, and exactly what the `ldy_sky`
  vendor streamer does on the same silicon. Its `select()` wait drops from 1 s
  to 2 ms while gated for the same reason Maruko's poll does. CV610 records in
  mirror mode only, so the recorder always shares the gated channel and the
  close-suppression always applies there.
- **CV610 is linked and confirmed on device.** Hardware verification found and
  fixed an initialization-order defect: gate setup ran before
  `cv610_output_start()` created the frame ring, so every frame-shm gate was
  incorrectly declared inert. Setup now runs immediately after output creation.

## [0.84.0] - 2026-09-06

`record.format="hevc"` now rotates, and **neither recorder forces a keyframe
to do it any more**. `contract_version` moves to **0.31.0**: `segments` is
meaningful on the raw path for the first time.

- **The raw recorder ignored `maxSeconds` and `maxMB` entirely.** It had no
  rotation code at all — no segment counter, no threshold check, no second
  `open()` — so a raw recording was one file that grew until the card filled,
  silently. It is also why the "lower `maxMB`" workaround for the 2 GB ceiling
  did not generalise: on that path there was no threshold to lower.
- **Rotation no longer requests an IDR, on either recorder.** Since 0.70.0 the
  TS recorder asked the encoder for a keyframe when a threshold was crossed
  and none was coming. That works, but an IDR is a large frame and one per
  segment raises the bitrate the link must carry — on an intra-refresh craft
  it undoes precisely what the mode exists for, and under
  `record.mode=mirror` the recorder taps the live channel, so the spike went
  out over the air for the benefit of a file. The whole ask — the pacing, the
  bound, the `take`/`requeue` hand-off at six sites across three backends — is
  deleted.
- **A segment now opens on a point the stream already produces:** an IRAP
  where one exists, or the VPS/SPS/PPS that head each refresh wave under
  intra-refresh. Both cases are measured on Star6E, not assumed. At
  `resilience=off`, 601 frames: 4 parameter-set groups, 4 IRAP access units,
  and **0/4** groups detached from an IDR — so normal GOP recording still cuts
  on its IRAP, which is where it has to cut. At `resilience=racing`, 800
  frames: ONE IRAP at startup and never again, with parameter sets every ~200
  frames decoupled from it — so there the wave head is the only point that
  ever arrives.
- **Segment granularity on an intra-refresh craft is one GOP.** The threshold
  decides whether to rotate; the wave head decides when. Verified on Star6E
  (`gopSize 2.0`, 100 fps, `maxMB=2`): 10 segments in 20 s, each opening
  `VPS, SPS, PPS`, each holding exactly 1206 slice NALs (201 frames x 6
  slices = one wave), and **zero IRAPs in any of them**.
- **The rotation policy is shared rather than copied.** `RecorderRotation` in
  `star6e_recorder.h` holds the decision once; each recorder supplies only its
  own open/close, which is the only place they differ.
- **`segments` is reported for `hevc`.** The three backends disagreed: two
  left it `0` while segments were on disk, and Maruko hardcoded `1`.
- **Maruko's own writer rotates too.** `maruko_recorder_write_frame()` — the
  dual and synchronous-fallback path on that backend — bypassed the rotation
  policy entirely, so `format="hevc"` there would still have grown one
  unbounded file. It now takes the same shared cut, and Maruko's TS adapter
  accepts parameter-set boundaries as well, without which rotation stayed
  inert on an intra-refresh craft there.
- **A rotation can no longer overwrite an existing recording.** The segment
  name carries only uptime seconds plus 16 bits from the nanosecond clock, and
  after a reboot the uptime restarts — so a name can repeat. Opening with
  `O_TRUNC` destroyed whatever was there; segments now open `O_EXCL` and retry
  with a fresh name, at start as well as on rotation. Rotation multiplied the
  exposure, since it is now one name per segment rather than one per
  recording.
- **A rotation that cannot reopen because the card filled now reports
  `disk_full`.** The space check only runs every 300 frames, so a card that
  fills in between surfaces as `ENOSPC` from the segment `open()`; it was
  reported as a write error, sending the operator after the media instead of
  the space.
- **Maruko's raw recorder clears its own producer gate when it stops itself.**
  A disk-full or write-error stop closed the file but left `recording` set, so
  the status reported that recording as active indefinitely, naming a file
  nothing was being written to.
- **A segment that fails to finalise is no longer reported as a clean
  rotation.** `fdatasync()` and `close()` results were discarded, so a delayed
  write surfacing at the end of a segment left a possibly short file and no
  indication of it. Both are checked; a failure stops the recorder the same
  way a failed reopen does.
- If a stream produces neither cut point, rotation waits rather than forcing
  anything, and says so once instead of growing the file in silence.

## [0.83.0] - 2026-09-06

Recording no longer stops at 2 GB. The ceiling was in the binary, not the
card: a 32-bit `off_t` build refuses any write past 2^31-1 with `EFBIG`
whatever the filesystem allows. `contract_version` moves to **0.30.0** — the
`stop_reason` enum gains `"size_limit"` and the inactive record status now
carries the counters of the recording that ended.

- **Built with `-D_FILE_OFFSET_BITS=64`.** Without it, glibc leaves `off_t`
  32-bit and `open()` does not set `O_LARGEFILE`, so the kernel caps every
  recording at 2147483647 bytes — reported on SSC338Q as a recording dying at
  exactly that count on exFAT, which has no such limit of its own. It also
  fixes `stat()`/`fstat()`, which returned `EOVERFLOW` for a file over 2 GB
  and would have hidden such a recording from `/api/v1/recordings` and its
  download even once the write succeeded. musl targets (Maruko, CV610) were
  never affected — they are 64-bit `off_t` and force `O_LARGEFILE` regardless
  — and are unchanged by the flag. **A CFLAGS change does not invalidate
  objects: this needs a clean build.**
- **Segment rotation is bounded by what `off_t` can reach.** A configured
  `record.maxMB` above the ceiling now produces more segments instead of a
  dead recorder. Inert on every shipped target.
- **A segment that cannot be cut stops cleanly.** Rotation can only happen on
  an IRAP and the IDR request is bounded, so a GDR stream can carry a segment
  past any threshold. Rather than walk into an `EFBIG` that truncates
  mid-access-unit, the recorder now stops on the frame boundary with the file
  intact and reports `stop_reason: "size_limit"`.
- **`EFBIG` is no longer reported as `"write_error"`.** All three recorders
  name it, and the raw (`format: "hevc"`) recorder says in the log that it
  does not rotate. Calling a size ceiling a write error sent the original
  report looking at the SD card.
- **The record status keeps its counters after a self-stop.** `GET
  /api/v1/record/status` answered `{"path":"","frames":0,"bytes":0}` for any
  stop the recorder made on its own — losing both the file that was cut short
  and how far it got, which is the whole diagnosis. It now reports the path,
  bytes, frames and segments of the recording that ended, taken from the
  recorder `record.format` selects. The previous rule picked between the two
  recorders by asking whether the TS one had stopped manually, which says
  nothing about which session ended last: a retained `write_error` from an
  earlier run would have outranked a later, cleanly stopped recording — and
  with the counters now following the reason, named that older file too.
- **A write that fails part-way through an access unit is rolled back.** The
  TS recorder had no equivalent of the raw recorder's truncate-to-frame-
  boundary, so a ceiling below the `off_t` one — FAT32 stops at 4 GB — left a
  half-written access unit at the end of the file. Measured on an unpatched
  build: the recorder's own counter stopped at 2147464968 while the file on
  disk was 2147483647, so 18679 bytes of partial AU stayed behind. The file
  now ends on a whole number of TS packets, which is what `size_limit`
  claiming an intact file requires.
- **Docs: `maxSeconds`/`maxMB` of `0` never meant "no limit".** The runtime
  overrides its compiled-in threshold only when the value is greater than
  zero, so `0` records 300-second / 500 MB segments. `documentation/
  SD_CARD_RECORDING.md` said the opposite. It now also states that rotation
  is `format: "ts"` only — `"hevc"` ignores both thresholds, which is tracked
  separately.

## [0.82.0] - 2026-09-06

A destination that cannot be brought up no longer destroys a working output,
and no longer brings the craft down at boot. `contract_version` stays
**0.29.0** — no field, endpoint or payload changed, only behaviour, which is
documented under "A destination that cannot be brought up".

- **`output_socket_configure()` no longer closes a working socket.** The
  destination fill ran after the open/reuse block and closed the fd when it
  failed — including on the reuse path, where the type was unchanged and the
  socket was working. Since names are not resolved, `outgoing.server=
  udp://somehost:5600` took a live output down on every backend. The fill now
  happens first and the result is committed only on success, so the call is
  all-or-nothing. Device-verified on Star6E and Maruko: the bad URI is refused,
  packets keep flowing, and the output is still retargetable afterwards.
- **A bad `outgoing.server` in the config no longer bricks the craft.**
  Measured on all three: the pipeline came up — sensor, ISP, VPSS, encoder,
  JPEG, sidecar, on Maruko even the HTTP server — and then tore all of it down
  and exited, leaving no video and no API. The output now comes up inert,
  `transport/status` honestly reports `active:false`, and setting a working
  destination recovers it live. Verified end to end on all three, no restart.
  A live retarget still refuses a bad URI: there the craft has a working output
  to lose.
- The startup path re-seeds the transport policy the teardown clears, so a
  craft that started inert does not silently recover onto unconnected UDP or
  without the configured Unix stall allowance.
- `output_socket_destination_is_usable()` names the "can this URI be used"
  question that the CV610 retarget guard previously open-coded.

## [0.81.0] - 2026-09-05

CV610 gains PQTools `.bin` import and export, so the whole ISP parameter image
can be moved at once instead of one `/api/v1/iq` knob at a time.
`contract_version` moves to **0.29.0** — one new endpoint, and `isp.sensorBin`
flips from unsupported to live on this backend.

- **`isp.sensorBin` is honoured on CV610**, applied live and at cold boot
  through the vendor `libbin.so`. The call contract was recovered from the
  stripped blob; the package we have ships the library but not `ot_pq_bin.h`,
  and the older `HI_PQ_BIN` API on Hi3516CV100 is a different shape whose error
  codes do not apply. A file is `[ISP image of OT_PQ_GetISPDataTotalLen()
  bytes][OTPQNRX 3DNR section]` — asking the library for that length is also
  the version check.
- **`GET /api/v1/iq/export_bin`** serializes the live ISP back out to
  `/tmp/isp_export.bin` and answers `{"path":…,"bytes":N}`. The destination is
  fixed because the endpoint is unauthenticated. 501 on Star6E/Maruko, which
  round-trip IQ as JSON instead; advertised as `routes.iq_export_bin`.
- **A `.bin` is locked to the chip register map and the SDK ISP version, not
  to the sensor.** Import validates an address/size walk against what the
  running chip's ISP reports and carries no sensor identity. Device-verified on
  a Hi3516CV610 + IMX662 bench: a tune built for an os02h10 imported cleanly,
  moved 39 of the 102 fields `/api/v1/iq` reads back across ten ISP groups, and
  turned the picture red — which is the expected outcome, since a foreign tune
  brings its own CCM, AWB, black level and noise calibration.
- **Round trip is exact.** Importing our own exported file restored an
  identical read-back. A live import and a cold-boot import of the same file
  produce read-backs that differ in **0 of 102 fields**, so the running ISP
  thread does not re-assert over an imported image.
- **The cold-boot order is load-bearing**: the image lands before
  `isp.gainMax` / `isp.shutterMaxUs`, because a `.bin` carries an AE
  ext-register record of its own and would otherwise overwrite the config's
  ceilings on every boot while `/api/v1/get` kept reporting them. A successful
  import also drops `cv610_iq.c`'s cached cold-boot AE ceilings, which would
  otherwise write pre-import values back on the next `isp.gainMax=0`.
- **A `.bin` is integrity-checked and cannot be edited or spliced in place** —
  one flipped byte returns `0xcb000005`. Both truncation classes are rejected:
  a file shorter than the ISP image by our own gate, naming both numbers, and a
  headed-but-truncated 3DNR section by a length check against
  `OT_PQ_GetStructParamLen` (the library does not bound that read itself, and
  would otherwise push uninitialised heap into the 3DNR registers).
- `iq-profiles/cv610-bin/imx662.bin` vendors an export of the reference bench's
  cold-boot state as a restore point — not a measured tune; nobody has run
  PQTools against this sensor yet. `make stage SOC_BUILD=cv610` ships it in
  `isp-bins/`.
- Requires `libbin.so` on the craft (`/usr/lib`); without it the import warns
  and no-ops, and the craft still boots.

## [0.80.0] - 2026-08-30

A sustained bitrate overrun is now reported instead of being silent, and the
`roiQp` bound gets the caveat 0.79.0 shipped without. `contract_version` stays
**0.28.0** — no field, endpoint or payload changes.

- **The ±20 bound in 0.79.0 is calibrated for the DEFAULT QP ceiling, and
  `video0.maxQp` moves that ceiling.** `roiQp` is a relative delta, so CBR
  raises the base QP ~1:1 to pay for it and pins once
  `base_qp + |roiQp|` passes the ceiling. Measured on a CV610 bench at a fixed
  `roiQp -20` — a value 0.79.0 calls safe — against a 9264 kbps target:

  | `video0.maxQp` | frame QP | delivered | vs target |
  |---|---|---|---|
  | default | 44.9 (44-47) | 9502 | 1.0x |
  | 45 | 45.0 (36-45) | 12528 | 1.4x |
  | 40 | **40.0 (40-40)** | 53493 | **5.8x** |
  | 35 | **35.0 (35-35)** | 111085 | **12.0x** |

  So an operator who lowers `maxQp` to control bitrate silently re-enters the
  failure 0.79.0 closed. Documented in the field table and the backend matrix.

- **Star6E's default ceiling is 48, not 51** — `MaxQp 48 MinQp 12` from
  `/proc/mi_modules/mi_venc/mi_venc0`. It therefore has three fewer QP of
  margin than CV610 at the same `roiQp`. `MinQp 12` may also be why Star6E
  never reproduced the collapse: if the post-ROI QP is clamped into
  `[MinQp, MaxQp]` the band cannot starve the controller. Not tested.

- **`pipeline_common_rate_watch()` reports a sustained overrun.** Two
  consecutive 2 s windows delivering >=150% of the configured target log once,
  naming `roiQp` and the ceiling when ROI is on. It clears below 120% and can
  arm again.

- **It watches the consequence, not the cause, and that is deliberate.** The
  frame QP that explains the failure is CV610-only — SigmaStar fills only
  `refType` in its stream info (0.79.0 HISTORY). Frame size is available on
  every backend, and the collapse is far easier to see in the delivered rate
  than in the QP anyway: 1.9x-39x, sustained, versus a controller reading a
  number near its ceiling.

- **Thresholds are compiled in, not configurable.** The measurement left no
  room to tune: normal operation ran 0.96-1.06x of target across every arm, the
  worst benign scene transient reached 1.43x for a single window, and a real
  collapse ran 1.9x-39x and did not decay. Two windows is what separates the
  transient from the failure.

## [0.79.0] - 2026-08-30

`fpv.roiQp` is bounded to **±20**, down from ±30. `contract_version` **0.27.0
-> 0.28.0** — this is a *narrowing*, not an addition: a client sending ±21..±30
now gets 400 where it previously got 200.

- **Past ±20 the delta stops being honoured, at both ends.** `roiQp` is a
  RELATIVE delta and H.265 caps QP at 51. Positive is the benign end and merely
  truncates: at `+30` the frame's base QP sat at 21.5, so the region wanted 51.5
  and got 51.

- **Negative is the expensive end, and it was a real defect.** CBR pays for the
  ROI discount by raising the frame's base QP roughly 1:1 with `|roiQp|`. Once
  `base_qp + |roiQp|` passes 51 the rate controller saturates — every frame at
  `qp 51/51/51`, zero variance, no authority left — while the ROI band still
  encodes at `51 - |roiQp|`, which is expensive. Measured on a CV610 bench,
  720p60, via the sidecar's per-frame `frame_size_bytes` and `qp`:

  | target kbps | roiQp | base QP | delivered | vs target |
  |---|---|---|---|---|
  | 2829 | 0 | 25.4 | 2849 | 1.01x |
  | 2829 | -20 | 45.1 | 2802 | 0.99x |
  | 2829 | -25 | 49.9 (max 51) | 2991 | 1.06x |
  | 2829 | **-30** | **51/51/51** | 16976 | **6.0x** |
  | 9000 | -20 | 42.5 | 8888 | 0.99x |
  | 9000 | **-30** | **51/51/51** | 17300 | **1.9x** |
  | 1200 | -15 | 42.4 (max 51) | 1186 | 0.99x |

  The rule `base_qp + |roiQp| > 51` predicts all three targets. **The QP column
  is the instrument** — delivered rate alone cannot see this, because below the
  cliff CBR absorbs the whole delta and the total does not move.

- **The band width does not move the cliff, only its severity.** At `roiQp -30`,
  `roiCenter` 0.6 / 0.4 / 0.2 all pinned at qp 51 and delivered 17661 / 12520 /
  6977 kbps. Base QP rises by the full delta however narrow the band is, so
  there is nothing to bound on that axis instead.

- **A config file is clamped, not rejected.** `load_fpv()` clamps at parse time,
  so a craft already carrying `-30` still boots and lands on `-20` — the fix
  applying itself. Only `/api/v1/set` rejects, so an operator typing `-30` is
  told rather than reading back a value they did not write.

- Shipped default `fpv.roiQp` **-25 -> -20**, so the default sits inside the
  range it documents (`venc_config.c`, `config/waybeam.default.json`,
  `config/waybeam.default.maruko.json`).

- **NOT reproduced on Star6E.** 1080p60 held its target at `roiQp` 0/-15/-25/-30,
  both at its configured 19092 kbps and forced down to 1500. Its sidecar reports
  `qp=0`, so its rate controller could not be watched directly — that is
  unreproduced, not immune. The bound applies on all three backends anyway,
  because the QP ceiling it derives from is an H.265 limit rather than a vendor
  one.

## [0.78.0] - 2026-08-30

The two portable AE ceilings reach the CV610 ISP. `contract_version` **0.26.0
-> 0.27.0**: `isp.gain_max` and `isp.shutter_max_us` flip from
`supported:false` to `supported:true` on that backend. Both were already
`MUT_LIVE` in the shared table, so nothing about mutability changes.

- **They map with no unit conversion, which is the only reason they are wired
  at all.** `isp.shutter_max_us` -> `exposure.auto.exp_time_max`, documented
  "sensor exposure time (unit: us)"; `isp.gain_max` ->
  `exposure.auto.a_gain_max`, documented "Format:22.10 ... unit: times, 10bit
  precision", i.e. 1024 == 1x, which is the same scale the SigmaStar
  supervisory AE uses (`maruko_cus3a.c`'s `AE_GAIN_MIN 1024`). The live board
  confirms the gain format independently of the header: `sys_gain_max` 1630616
  == `a_gain_max` 407654 x `ispd_gain_max` 4096 / 1024.

- **Analog gain, not system gain.** `sys_gain_range` caps the product of
  analog, sensor-digital and ISP-digital gain; the portable name means the
  SENSOR's ceiling on every other backend. Mapping it to the product would
  make the same number mean something different here.

- **Measured, not assumed.** On a CV610 bench (sc4336p, `ave_lum` 43), capping
  `shutterMaxUs` to 4000 pulled the applied `exp_time` from 16560 to exactly
  4000 us and the AE raised `a_gain` 1497 -> 6611 to hold the same luma.
  Capping `gainMax` to 2048 clamped the applied `a_gain` to 2043 while the ISP
  took up the slack in DIGITAL gain (`isp_d_gain` 1024 -> 1948) — which is also
  what shows the field is the analog ceiling rather than the system one.
  Readings come from `/api/v1/awb`, which reports the AE's own output, so this
  is the loop moving rather than a value reading back.

- **`0` restores the plugin default instead of sticking.** `0` means "use the
  sensor plugin's default" on every backend. The default is snapshotted before
  the first write of ours lands, so clearing a ceiling puts 873800 / 407654
  back. Without that snapshot, setting a ceiling and then clearing it would
  have left the ISP on the last non-zero value forever while the config read
  `0` — a one-way door, and the exact shape of bug this milestone keeps
  finding.

- **Applied at cold boot, not only on a live write.** The ISP is seeded by the
  sensor plugin, which has never seen the config file, so a value already in
  the file would otherwise never be programmed. Verified: a restart with
  6000/8192 in the file came up honouring both, `exp_time` 6000 and `a_gain`
  4220.

- **`isp.gain_min`, `isp.shutter_min_us`, `isp.awb_mode` and `isp.awb_ct` stay
  unsupported and still 501.** Neither pair was measured in this slice. For
  the AWB pair that is a two-call flow, not a missing capability: `ct_manual`
  means "pin white balance to a colour temperature", and where SigmaStar has
  one call (`MI_ISP_AWB_SetCTMwbAttr(ct)`) this SDK has
  `ss_mpi_isp_cal_gain_by_temp()` — declared in `ss_mpi_awb.h` and present in
  `libss_mpi_awb.so` — which converts Kelvin to r/gr/gb/b gains against the
  *current* `ot_isp_wb_attr`, after which those gains go back through
  `set_wb_attr` with `op_type` manual. The result depends on the sensor's AWB
  calibration, so it needs the same device measurement every other entry in
  the allowlist had to clear. Wiring `awbMode` while ignoring `awbCt` would
  meanwhile ship a name that reads back fine and means something different per
  backend. Manual white balance on CV610 is reachable today, under its own
  name, through `/api/v1/iq`'s `wb` group.

  An earlier draft of this entry claimed the ISP had "no Kelvin input" and
  that `ss_mpi_awb.h` exported "only register/unregister". Both were wrong —
  that header exports 14 functions, and a grep pattern anchored on
  `ss_mpi_awb` hid every `ss_mpi_isp_*` symbol in it.

- Docs: the backend support matrix said `/api/v1/awb` was **501** on CV610.
  It has not been since `cv610_awb_query()` landed, and it is the instrument
  this change was verified with, so the row is corrected here.

## [0.77.1] - 2026-08-30

Closes the audio gap 0.77.0 shipped with. `contract_version` stays **0.26.0** —
no field, endpoint or payload changes, only where the audio side channel points
after a live retarget.

- **A live `outgoing.server` change moved video and left audio behind.** The
  Opus destination is derived from the video URI — the peer host for `udp://`,
  loopback for the local transports — and that derivation ran once, at
  `cv610_audio_start()`. So a retarget sent video to the new receiver while
  audio kept going to the old one, silently, because nothing in the audio path
  knows the video URI changed. `cv610_apply_server()` now calls
  `cv610_audio_apply_server()`, which re-derives and repoints.

- **The derivation is now written once.** `cv610_audio_derive_output()` is
  shared by the start path and the retarget, so the mapping cannot drift
  between them — which is the actual failure mode for a rule that has to be
  applied in two places.

- **Same seqlock as the video path.** The audio transport fields are read by
  the capture thread in `cv610_audio_write()` and would be rewritten by the
  httpd thread, so they are seqlock-protected and the capture thread snapshots
  them once per encoded frame. Every packet of one Opus access unit therefore
  reaches the same destination.

- Not fatal on failure: the video socket has already moved by that point, and
  returning an error would send `venc_api` into a rollback that re-enters the
  function. A craft with video retargeted and a logged audio failure is better
  than one where the rollback leaves both half-applied.

**Star6E and Maruko do NOT have this gap** — corrected 2026-08-30 after
checking, having first claimed they did. Their audio output holds a *pointer*
to the live video output rather than a copy, and resolves the destination at
send time through a cache keyed on the video output's `transport_gen`
(`star6e_output.c` `resolve_cached_audio_target()`, `maruko_audio.c:61-85`). A
retarget bumps that generation, which invalidates the cache, so audio
re-resolves to the new destination on its own. Device-confirmed on a Star6E
craft running the *released* 0.76.0 binary with no fix applied: a live
`udp://:5700` -> `udp://:5800` retarget moved video (PT=97, ~1750/s) and audio
(PT=98, ~50/s) together in the same second, PID unchanged.

CV610 differed because `cv610_audio.c` COPIED the resolved destination into its
own state at start. That is the whole bug, and it is CV610-only.

## [0.77.0] - 2026-08-30

Live output retarget reaches CV610. `contract_version` 0.25.0 -> **0.26.0**
(additive; two per-backend mutability flags move restart_required -> live).

- **`outgoing.server` and `outgoing.enabled` were restart-only on CV610** —
  honestly advertised, because the URI was parsed once in `cv610_prepare()` and
  `cv610_output_start()` ran once from `cv610_init()`. Retargeting a craft meant
  a respawn. They are now backed by `cv610_apply_server()` and
  `cv610_apply_output_enabled()`.

  **The callbacks landed before the mutability widened**, which is an ordering
  requirement rather than a preference: `live_group_supported_for_cfg()` gates
  `LIVE_GROUP_OUTGOING` on those callbacks existing, so removing the entries
  from `cv610_field_is_restart_only()` first would have advertised `live` in
  `/api/v1/capabilities` while every write was refused.

- **The real work was thread safety, not the two callbacks.** `cv610_output_write()`
  read `socket_handle` / `destination` / `destination_len` / `connected_udp`
  straight off the context on the producer thread, while a live apply rewrites
  them from the httpd thread — a torn sockaddr, or a send on a closed fd. Both
  SigmaStar backends already guard this with an even/odd `transport_gen`
  seqlock; CV610 now does the same, snapshotting once per frame so every
  datagram of one access unit goes to the same destination. Splitting an access
  unit across two receivers leaves neither able to decode it.

  Worth knowing for anyone reasoning about the window: `output_socket_configure()`
  **reuses the existing socket** when the transport type is unchanged, so a
  `udp://a` -> `udp://b` retarget never churns the fd at all — it rewrites the
  destination. Only a `udp` <-> `unix` switch closes and reopens.

- **Socket transports switch live; ring transports are RESTART-CLASS, not
  refused.** The ring is created once at start, so `shm://` / `frame-shm://`
  genuinely cannot be switched in place. Both SigmaStar backends refuse such a
  write outright — CV610 deliberately does not, because refusing would have
  REGRESSED this backend: `outgoing.server` was restart-required here until
  now, so writing `frame-shm://...` persisted and took effect on the next boot.
  A hard refusal would have made the fleet's normal production transport
  unreachable through the API and left hand-editing `/etc/waybeam.json` as the
  only way to provision a craft. Instead the value is committed,
  `venc_api_request_reinit()` is called, and the response carries
  `reinit_pending` so the caller knows this one is not live.

- **A live apply that asks for a respawn now says so.** `/api/v1/set` on a
  live-class field answered a flat `ok` even when the backend's apply had
  requested a reinit. It now reports `reinit_pending`, scoped to a false->true
  transition across that single apply — `g_reinit` is a latch the runtime
  consumes on its own schedule, so reading it absolutely made an unrelated live
  set claim a respawn that belonged to an earlier request.

- **Review fixes folded in before merge**, each a real defect this change
  introduced: the applied-destination no-op guard now runs *before* the ring
  branch (an identical re-POST of the running `frame-shm://` URI was latching a
  reinit and respawning the craft, so every idempotent re-apply from the ground
  cost a video outage); a live apply that requests a respawn now forces the
  config to disk, because `/api/v1/live/set` passes `persist=0` and the respawn
  reloads from `/etc/waybeam.json`, silently discarding the write; the RTP
  session is seeded *before* the `outgoing.enabled` bail, or a craft booted
  disabled and enabled over HTTP sent every packet with `ssrc 0` and a timestamp
  frozen at 0 (`frame_ticks` was 0 too); the batch response reports
  `reinit_pending` like the single one; a failed `output_socket_configure()`
  clears `applied_server`, because it closes the socket even on the fd-reuse
  path and the stale record made the rollback a silent no-op that left the craft
  mute until restart; and the GDR counter plus the ring low-water gauge keep
  advancing while output is disabled, since both describe the encoder and the
  ring rather than the transmission.

- **Known gap, not fixed here:** the Opus side-channel destination is derived
  from the video URI once at start (`cv610_audio.c`), so a live retarget moves
  video and leaves audio pointed at the old host. Fixed in 0.77.1.
  (This bullet originally added "Star6E has the same gap" — **that was wrong**,
  see the 0.77.1 entry. Both SigmaStar backends resolve the audio destination
  at send time against the live video output, so they already follow.)

- **One deliberate divergence from Star6E.** Star6E also drops the encoder to
  5 fps while the output is disabled, so it is not burning bitrate on frames
  nobody receives. CV610 cannot: `video0.fps` is restart-only there (it
  reprograms the MIPI raw_bit and the RTP clock), so there is no live rate to
  idle to. A disable gates the send on both transports and the encoder keeps
  running at its configured rate.

## [0.76.0] - 2026-08-30

Centre-priority ROI reaches CV610, and stops being skipped at boot on Maruko.
`contract_version` 0.24.0 -> **0.25.0** (additive; four per-backend `supported`
flags change).

- **`fpv.roi*` had no CV610 implementation at all.** No `apply_roi_qp` in the
  backend callback table and no cold-boot apply, so every CV610 craft carried an
  `fpv` block in `/etc/waybeam.json` that nothing on the board read. The write
  path was honest about it (`/api/v1/capabilities` reported `supported:false`
  and `/api/v1/set` answered `501`), but `/api/v1/get` returned the stored value
  and the config file kept round-tripping it, so a craft could sit for months
  with `roiQp:-15` configured and no region ever programmed.

  `cv610_apply_roi_qp()` now programs the bands through
  `ss_mpi_venc_set_roi_attr()`. `ot_venc_roi_attr` is field-for-field
  `MI_VENC_RoiCfg_t`, and the part accepts a wider delta than the shared
  +/-30 clamp, so no per-backend range was needed. Geometry is read from the
  channel attr rather than a cached width: it is what the encoder is actually
  running, and the get doubles as the "channel exists" guard.

- **Maruko never applied ROI on a cold boot.** `fpv.roi*` are `MUT_LIVE`, and
  only the live path ever called `maruko_apply_roi_qp()`. Star6E has always
  applied it in `star6e_runtime_apply_startup_controls()`; Maruko applied
  `qp_delta` and `qp_bounds` there and not ROI. Witnessed on the bench: a craft
  booting with `roiEnabled:true, roiQp:-8` in its file logged the qpDelta apply
  and **no ROI line at all**, then logged the band immediately on one live set.
  Every craft that configured ROI in its file was flying without it.

- **The band geometry is now computed once.** `compute_horizontal_roi()` was
  byte-identical in `star6e_controls.c` and `maruko_controls.c` down to the
  32-px alignment; CV610 would have made a third copy. The rectangle and the
  QP taper move to `pipeline_common_roi_band()`, and each backend copies the
  result into its own SDK struct.

- **ROI is measured, not assumed.** Issue #259 is a QP control on these same
  SigmaStar parts that returns success, logs as applied, reads back clean and
  never moves the bitstream, so an SDK return code is not evidence. Maruko,
  1280x720@30 CBR 5754 kbps, `roiSteps:1 roiCenter:0.3` (a 384-px band at
  x=448), decoded and profiled as median Laplacian variance per 64-px column:

  | column | `roiQp:-30` | `roiQp:+30` |
  |---|---|---|
  | 384-448 (outside) | 54.5 | 640.1 |
  | 448-512 (inside) | 755.7 | 46.9 |
  | 768-832 (inside) | 854.0 | 6.3 |
  | 832-896 (outside) | 34.7 | 792.7 |

  The step lands exactly on the programmed rect edges: 13.9x across x=448 and
  24.6x across x=832 in the `-30` arm, reversing in the `+30` arm (13.6x down at
  the left edge, 126x up at the right). The repeatability figure is a different
  aggregation and is named here so it can be checked: median Laplacian variance
  over the **whole band** (x=448..832), which came out 1739 then 1854 across two
  runs of the `-30` arm -- 6.6% apart -- against 94.8 in the `+30` arm, an 18x
  swing. CV610 was measured the same way on its own silicon (the SDK call
  differs, so Maruko's result does not carry): whole-band 67.0 / 9.1 / 61.6 for
  `-30` / `+30` / repeat, a 7.4x swing against an 8.8% control. Whatever #259
  is, it does not generalise to ROI on either part.

- **The shipped ROI defaults now agree with themselves.** They were
  `roiEnabled:true, roiQp:0` — which reads as "ROI is on" while `apply_roi_qp()`
  clears every region, because a zero delta is not a region worth programming.
  Nothing was ignored, but the flag and the log disagreed, and that is the first
  thing an operator sees. They ship **`roiEnabled:false, roiQp:-25`**: off, but
  carrying a delta that bites the moment it is switched on. Existing crafts are
  unaffected — their own `/etc/waybeam.json` values win; this changes the
  compiled defaults, `config/waybeam.default*.json` and `/api/v1/defaults`.

- **The disabled log line names its cause.** `> ROI disabled (all regions
  cleared)` covered three different states. It now reads
  `> ROI disabled (roiEnabled=false), all regions cleared` or
  `> ROI disabled (roiQp=0, no delta to apply), all regions cleared`, on all
  three backends.

## [0.75.0] - 2026-08-30

Sensor orientation reaches CV610. `contract_version` 0.23.0 -> **0.24.0**
(additive; two per-backend `supported` flags change).

- **`image.mirror` / `image.flip` were hardcoded `TD_FALSE`** on the CV610 VI
  channel attribute (`src/cv610_pipeline.c`), and absent from the backend's
  advertised field list. An inverted lens mount had no software fix on that
  board: a config file could carry the values and the picture never moved,
  while `/api/v1/set` answered `501 not_implemented`.

  They are now applied at the **sensor**, through the plugin vtable's
  `pfn_mirror_flip`, which is where both SigmaStar backends already apply
  orientation (`MI_SNR_SetOrien`). Same place, same meaning, so a shared craft
  config reads the same on all three boards and the encoder is never asked to
  do geometry. The VI channel attribute stays `TD_FALSE` and now says why.

- **The Bayer start phase does NOT move on IMX662, and that is measured.** The
  sensor driver carries a "flipping changes the Bayer start phase — VERIFY on
  hardware" note, and the textbook answer is one XOR per axis into
  `ot_isp_bayer_format`. On this sensor it is wrong: the reverse shifts the
  readout window with the direction, so the phase the ISP sees is unchanged.
  Bench A/B with `mirror` on, same scene, same binary but for that one line —
  with the XOR, mean RGB went 106/98/141 -> **163/33/195**, green collapsing
  because the ISP was demosaicing green sites as red and blue; without it,
  105/96/145, which is the unmirrored frame's colour. Geometry was right in
  both arms, so the failure presents as a white-balance fault rather than an
  orientation one. All three reachable states were then checked for colour on
  the shipped path — mirror 106/97/144, flip 106/97/145, rotate-180
  106/97/144, against a 106/98/141 baseline — so the result is not generalised
  from the mirror arm alone. The driver's VERIFY note can now be closed.

- Orientation is programmed immediately after `isp_setup()`, and the gate is
  narrower than "once streaming": the plugin's i2c file descriptor is opened by
  `imx662_init()` as `pfn_cmos_sns_init` from inside `ss_mpi_isp_init()`,
  synchronously on the calling thread. Before that the driver's writes return
  `TD_SUCCESS` against a closed fd and do nothing. Deliberately **before** the
  ISP thread starts: applied after, the first frames and the first AE/AWB
  statistics are gathered in the old orientation and the readout then flips
  under a converging 3A loop.

- It is written **unconditionally, including the disable**, matching
  `MI_SNR_SetOrien` on both SigmaStar backends and the rule `vpss_setup()`
  already states about the crop — an external sensor chip holds whatever it was
  last given, more stubbornly than any MPP group. The SoC's MIPI reset happens
  to clear `0x3020`/`0x3021` on this board, but that is board wiring, not a
  property of the sensor.

- The log says **"requested"**, not "ok". `pfn_mirror_flip` returns `td_void`,
  the driver casts both register writes to `(td_void)`, and
  `imx662_write_register()` returns `TD_SUCCESS` even against a closed i2c fd —
  there is no success to report at any of the three layers. Claiming one would
  reproduce the exact failure this change exists to abolish: an orientation that
  silently did nothing, with a log line asserting otherwise.

- **`image.rotate` stays unsupported on CV610**, and the first draft of this
  change had it the other way round. The decomposition it relies on —
  `venc_config`'s `load_image()` turning `rotate: 180` into `mirror + flip` —
  runs on a **file parse and nowhere else**, so a value arriving through
  `/api/v1/set` is never decomposed and never read. Advertising it was measured
  on `.181` to turn a clean `501` into: accept `90`, persist `90` to the config
  file, raise `reinit_pending`, restart the encoder, then read back `0` once
  `load_image()` coerces it — leaving the file and the running config
  permanently disagreeing. A config file carrying `rotate: 180` still works;
  the field reports unsupported because nothing reads *rotate*.

## [0.74.0] - 2026-08-30

The RTP sidecar reaches CV610. `contract_version` 0.22.0 -> **0.23.0**
(additive; one per-backend `supported` flag changes).

- **`src/rtp_sidecar.c` was never compiled into the CV610 binary.** It sits in
  `HELPER_SRC` and in the host test suite, but not in `CV610_SRC`, so a CV610
  craft emitted no per-frame metadata at all while `outgoing.sidecarPort` was
  parsed, range-validated and persisted like any other field. Nothing errored;
  the port simply accepted a value and did nothing.

  The cost was downstream and silent: the ground station's attitude consumer
  takes the sidecar ATTITUDE trailer as its only input and, on a board with no
  flight controller, has no MSP fallback either — so it was compiled out of the
  CV610 build entirely, as was the link-log probe that binds through
  `MSG_SUBSCRIBE`. The ground pipeline-stats consumer had no clock-sync or
  end-to-end latency source for such a craft, and `tools/rtp_timing_probe` had
  nothing to read, which is why CV610's VPSS latency has stayed unquantified —
  the instrument was missing, not the bench time.

  CV610 now emits the same FRAME the SigmaStar backends do, so every existing
  consumer and probe works against it unchanged. No wire change.

- **What the CV610 trailers carry.** ENC_INFO reports `frame_size_bytes`,
  `frame_type`, `qp` and `frames_since_idr`. (`idr_inserted` was also set here
  originally; 0.81.0 leaves it 0, because the field means "the controller
  requested an IDR" and this backend has no such controller — setting it on
  every IDR duplicated `frame_type` and misled a consumer that sums it.) `qp` is the
  encoder's own `h265_info.start_qp`, exactly what the contract asks for
  ("start QP / closest available per-frame QP") and already in hand — the
  SVC-T enhance check reads `ref_type` from the same struct. `complexity` and
  `scene_change` stay 0 because both come from the shared scene detector, which
  this backend does not compile in; `gop_state` stays 0 because it is the
  shared GOP controller's enum and CV610 does not run that controller. A zero
  meaning "not produced" beats a fabricated value a consumer would trend.
  TRANSPORT_INFO carries ring or socket fill, pressure state, drops and
  delivery count. **ATTITUDE and DETECT are never appended** — this board has
  no IMU and no detector — so a consumer whose only input is the ATTITUDE
  trailer still has no source on CV610.

  Device-verified on `.181`: `qp` 31–38 (mean 32.6) across 783 frames, and
  `frames_since_idr` climbing under `resilience=racing` (no periodic IDR) then
  resetting **4922 → 0** exactly on a `/request/idr`, with that frame typed IDR
  at 22321 B against ~2000 B for its neighbours.

- **`capture_us` is converted from the MPP timebase, not published raw.** The
  sidecar contract says `capture_us` is CLOCK_MONOTONIC µs; the encoder's PTS
  is on MPP's own clock, measured on `.181` running **~4.63 s ahead** of it.
  Publishing the raw PTS put `capture_us` *after* `frame_ready_us` on every
  frame, so the probe's encode-duration derivation went negative and printed
  `-` — a field that looked populated and could not be trended.
  `cv610_capture_us_from_pts()` samples `ss_mpi_sys_get_cur_pts()` against
  `wb_monotonic_us()` once, lazily, on the first frame that needs it (MI_SYS
  need not be up when the output starts) and applies the offset. While the
  epoch is unknown it returns 0 — the contract's "not available" — rather than
  a number on the wrong base. Device-verified on `.181`: ordering correct on
  604/604 sampled frames, and capture-to-encode-complete resolves to a stable
  **~24.3 ms at 720p100**.

- **RTP identifiers under `frame-shm://` now match the SigmaStar backends.**
  The first cut seeded the RTP session only on the socket path, on the belief
  that Star6E skips it for non-RTP transports. That was wrong:
  `star6e_output_is_rtp()` tests the *stream mode* (`rtp` vs `compact`, and the
  shipped default is `rtp`), not the transport, so a Star6E craft on
  `frame-shm://` does seed and does put a non-zero `ssrc`, `rtp_timestamp` and
  `seq_first` on the sidecar wire. CV610 was the only backend sending a zero
  `ssrc` there, which a consumer keying on "ssrc != 0 means session present"
  would read as a different craft state. `cv610_output_start()` now seeds for
  every transport; nothing but the sidecar reads `ctx->rtp` off the socket
  path. `seq_count` remains 0 on the ring transports on all three backends —
  the packetizer never runs, so no sequence numbers are consumed.

- **`in_pressure` and `pressure_drops` now mean on CV610 what they mean
  everywhere else.** `in_pressure` is defined in `include/rtp_sidecar.h` as the
  75/50 **hysteresis** flag, and `pressure_drops` — despite the name, kept for
  ABI stability across the v0.9.2 frame-skip rollback — is "frames observed in
  pressure", not a drop count on any backend. The first cut published a bare
  `fill >= 75` threshold and a hardcoded 0. Both now come from the shared
  `venc_observe_pressure()` helper, maintained on the producer thread every
  frame so the hysteresis sees every sample, and `/api/v1/transport/status`
  reports the same latched values it puts on the wire.
  Device-verified on `.181` against a deliberately unconsumed ring: flag
  latched, counter advancing 824 over 825 frames, HTTP and wire agreeing.

- **The sidecar socket is serviced on the idle path, not only alongside a
  frame.** The poll sat inside the "a frame was produced" branch, so every
  `continue` in the drain loop skipped it. A probe attaching while the encoder
  is stalled would have seen a silent port — indistinguishable from a build
  with no sidecar compiled in, which is the exact condition this release exists
  to remove. It now runs right after `select()`, ahead of every `continue`,
  with `errno` saved across it so the `EINTR` test still sees `select`'s value.

- **The transport observation now has one collector.**
  `cv610_collect_transport()` backs both `/api/v1/transport/status` and the
  per-frame trailer, so the number an operator reads over HTTP and the number a
  probe reads on the wire cannot drift. The drain loop takes the ring reading
  once and hands it to both the low-water window and the trailer. **Corrected in
  0.81.0:** this entry originally also claimed the whole thing ran "only once a
  probe is actually subscribed", so an unsubscribed craft paid no clock read,
  ring load or `SIOCOUTQ` ioctl. The sidecar review work later in this same
  release moved the observation out of the subscription gate — it now runs
  every frame, which is what the neighbouring bullet in this section describes.
  The two statements contradicted each other; this is the one that was stale.

- **`rtp_sidecar_sender_init()` runs before the `outgoing.enabled` bail**, on
  every path, because it is what writes -1 into `fd`. The context arrives memset
  to zero and fd 0 is a valid descriptor, so an init placed after an early
  return would have left the per-frame gate polling stdin.

## [0.73.3] - 2026-08-29

Recorder status is now a coherent snapshot instead of an unsynchronised read.
`contract_version` stays **0.22.0** — the response shape is unchanged.

- **`/api/v1/record/status` raced the writer thread on all three backends.**
  The callbacks read `recording`, the counters, `segments`, `path`,
  `start_time` and `last_stop_reason` in place while the writer mutated them
  during writes and segment rotation. Star6E documented this as safe because
  the fields were "single words written by one thread" — which did not hold on
  two counts: `bytes_written` is **64-bit on ARM32**, so an unsynchronised load
  can tear outright, and `path` is not a word at all, being rewritten wholesale
  on every rotation. The separate reads could also disagree with each other:
  `active` from one instant, counters from another, `path` from a third.

  Both recorders gain a `status_lock` guarding exactly the status-visible
  fields, plus `star6e_ts_recorder_snapshot()` / `star6e_recorder_snapshot()`
  returning one coherent `Star6eRecorderSnapshot`. All three backends now build
  their response from that.

  The lock is held **only across field updates — never across `write()`,
  `open()`, `fdatasync()` or `close()`**. A status poll must not be able to
  block on the disk; that is the coupling 0.73.2 removed from the encode loop
  and it must not reappear on the httpd thread. In `open_new_segment()` the
  path is built under the lock and the `open()` deliberately follows outside
  it.

  A `status_lock_ready` flag covers the window where a backend publishes its
  status callback before the recorders are initialised: the snapshot reports
  inactive rather than locking an uninitialised mutex.

- **Two fatal defects in 0.73.2's own hand-off, found by the final review.**
  Both were invisible to CI because no test exercises the deferred-reap window.

  *The encode loop wrote into the recorders during the hand-off.* The producer
  gate is `rec_writer != NULL`, and the async close nulls that immediately —
  but the recorders stay OPEN until the reaper joins. Star6E's and Maruko's
  no-writer fallback therefore wrote synchronously into the same descriptor the
  abandoned writer was still inside: the stalled write straight back onto the
  encode loop, unbounded, plus interleaved TS packets and a race on the mux
  continuity counters. The fallback is now also gated on `rec_reap_pending`.
  CV610 was never affected — it has no synchronous fallback.

  *A second close during a hand-off reaped inline.* `stop_bounded_async(NULL,
  …)` runs `on_exit` immediately, which is right in isolation but wrong while a
  reaper is outstanding: it closed both descriptors under the live sink and
  cleared `rec_reap_pending`, so the start guard that exists to prevent exactly
  that then passed and the new recording reused a descriptor the abandoned
  writer was about to append to. Every close now returns early while a reap is
  pending; teardown instead waits it out (bounded), so a reaper cannot run
  against a context `backend.c` has already freed.

- **0.73.3's lock reached only two of the three writers.** `maruko_recorder.c`
  mutates the same `Star6eRecorderState` fields from the dual-mode chn 1
  thread, and could not take the guard because it was `static` in
  `star6e_recorder.c`. It is now `static inline` in the header and Maruko's
  five sites take it — a mutex only helps if every writer takes it.

- **Coherence tightened.** `recording` is published inside the same section as
  the counters, `write_video()` updates bytes and frames in one section rather
  than two, and `open_new_segment()` builds the path into a local and publishes
  path, segment count and byte total together — so a poll mid-rotation cannot
  report the new file with the old count. The unconditional frame count is
  preserved: making it conditional would have been a semantic change smuggled
  in by a locking refactor.

- **Stale comments** that this series' own earlier commits invalidated: three
  backends still described the mid-run stop as ending in a blocking join,
  `venc_rec_writer.h` still said "both stops" join unconditionally, and CV610
  documented a parameter under its old name. Maruko now zeroes the
  per-recording counters before the open attempt, as Star6E and CV610 already
  did. The `/api/v1/version` example in `HTTP_API_CONTRACT.md` had gone stale by
  three releases; the version pin now covers both docs, not just README.

- **Test:** a writer thread driving `write_video()` with rotation every 64 KB
  while the main thread polls the snapshot, asserting counters never go
  backwards and that `active` never coincides with an empty path. Verified to
  be a real test rather than a decorative one: removing the writer-side
  locking makes TSAN report the race at
  `star6e_ts_recorder_write_video` and `star6e_ts_recorder_snapshot`.

Completes the Qodo review of the upstream sync PR.

## [0.73.2] - 2026-08-29

A mid-run record/stop can no longer stall live video. `contract_version` stays
**0.22.0** — no endpoint, payload or status change.

- **`venc_rec_writer_stop_bounded()` was not actually time-bounded.** It
  abandons the queued backlog, which bounds the *queue*, and then joins
  unconditionally — so the access unit already inside the sink's `write()` held
  the caller for as long as the kernel did. On a medium that has stopped
  completing, that is unbounded.

  That mattered more than the teardown case 0.73.1 documented: every backend
  calls a mid-run stop **from its encode loop** — CV610 for the HTTP stop
  (`cv610_runtime.c`) and for the recorder's own disk-full/write-error stop,
  Star6E and Maruko likewise. So stopping a recording on a stalled card stalled
  the live stream, which is precisely the coupling the async writer exists to
  remove, reintroduced on the one path that only runs when storage is already
  misbehaving.

  New `venc_rec_writer_stop_bounded_async()` never joins past its deadline: one
  grace to let the queued tail reach the disk, one to let the in-flight write
  finish, and if the sink is still inside that write it hands the writer to a
  detached reaper and returns. The reaper joins, frees, and only then runs the
  caller's reap callback — which is where each backend now closes its
  recorders, so on **both** paths the close still happens strictly after the
  sink can never run again. `*dropped` is still filled synchronously, because
  the abandon happens on the caller's thread.

  Bounded by roughly 2 × grace (500 ms on CV610) instead of by the disk. The
  healthy path is unchanged: the sink finishes inside the grace, the join is
  immediate, and the reap runs inline before the call returns — byte-identical
  ordering to the synchronous stop.

- **Teardown deliberately still joins.** There is no later for a reaper to run
  in, and the VENC/VPSS teardown below must not race a writer that still holds
  a descriptor. It stays bounded (the backlog is abandoned first).

- **A recording cannot start over recorders a reaper still owns.** When the
  hand-off happens the previous recording's descriptors are still live, so
  `record/start` is refused with a warning until the reap completes, rather
  than racing the close. On a healthy card this is never observable — the stop
  reaps inline.

Found by the Qodo review on the upstream PR, which identified the encode-loop
exposure the 0.73.1 note had understated.

## [0.73.1] - 2026-08-29

Fixes found while reviewing the 0.68.0-0.73.0 range for upstream submission:
sixteen behaviour bugs, all regressions or asymmetries introduced in that
range, plus the documentation that contradicted the code.  (Three review passes
folded into this one unreleased version.) `contract_version` stays
**0.22.0** — no field, endpoint or response shape changes.

- **Restored recovery for a malformed access unit on every transport.** The
  ring-v2 work removed the recovery IDR as self-actuation on the egress, which
  is right for a ring-full drop — that is congestion, and the rate controller
  owns it. But the same helper was called from
  `star6e_output_reject_incomplete_access_unit()`, which runs *before* the
  transport branch in `star6e_output_send_frame()`. So `udp://` and `unix://`
  crafts, which have no ring and were never congested, lost recovery too: an
  invalid SDK packet table dropped the whole picture and left the decoder
  broken until the next GOP. With `resilience=off` that is seconds. A
  malformed AU is an encoder fault, not egress pressure — the ring header
  draws the same line, keeping `other_drops` apart from `full_drops` because
  it "is not congestion at all and slowing down fixes nothing". Recovery is
  back on both SigmaStar backends, paced by the shared 100 ms per-channel IDR
  limiter, and still skipped for the droppable SVC-T top layer. Ring-full
  recovery stays removed.

- **Maruko `record.mode=dual` no longer keyframes the live stream.**
  `mk_mirror_record_open()` was missing the `ctx->dual` bail-out its Star6E
  counterpart has. In dual mode it started a mirror writer that the producer
  gate (`!ctx->dual`) never feeds — an idle thread for the session — and
  called `maruko_recorder_start_idr()`, which names `ctx->venc_channel`, i.e.
  **chn 0, the live stream**, while the file is fed by chn 1. Every dual-mode
  bring-up injected a keyframe into the live path and re-armed the limiter so
  chn 1's own request was swallowed. The per-recording counters are now also
  zeroed before the bail-out, so a failed start no longer reports the previous
  recording's `droppedFrames`.

- **A `resilience` change no longer discards `video0.intraRefreshQp`.**
  `venc_config_apply_resilience_preset()` zeroes `intra_refresh_qp`, and the
  live-set path applied the preset to the staged config *after* the request's
  own fields had been staged — then persisted it. So
  `?video0.resilience=balanced&video0.intraRefreshQp=44` returned 200 and
  applied the preset default, and a craft already carrying an override lost it
  permanently on any resilience change. On CV610 this is the only working
  I-frame lever. An explicit non-zero override now survives the preset; `0`
  still means "use the preset's default".

- **Bounded the CV610 recorder flush at teardown.** `cv610_teardown()` used
  the unbounded writer stop, which drains the whole queue against the disk
  before joining. CV610's medium can stall or disappear under load, and this
  backend has no watchdog, so a stalled queue meant teardown never reached the
  VENC/VPSS stop below it — leaking the kernel-state channels and binds that
  make the next start fail, and wedging the SoC when the init script rmmods
  MPP underneath. Now uses the same 250 ms bounded stop as every other path.
  Note this narrows the window rather than closing it: `stop_bounded()` still
  ends in an unconditional join, so the access unit already in the sink's hands
  can still block on a medium that never completes. Bounding the whole backlog
  is the part that was in venc's gift.

- **Star6E rejects `min_qp > max_qp` instead of writing it.** The API
  validator only compares the two when *both* are non-zero, so a
  half-specified pair (`minQp=50`, `maxQp` unset) resolved against the
  captured driver default and reached `MI_VENC_SetRcParam` as min > max. The
  SDK accepts that and then behaves erratically. Maruko and CV610 already
  refused it; Star6E now does too.

- **Maruko `record.mode=dual` no longer smashes its thread stack.** The chn 1
  drain thread was created with a NULL attr, and its path holds
  `nal_buf[512 KB]` and then `ts_buf[551 KB]` — ~1.06 MB live. Maruko is a
  musl target (`arm-openipc-linux-musleabihf`, confirmed on the bench:
  `/lib/ld-musl-armhf.so.1`), and musl gives a new pthread 128 KB, so this
  overflowed on the first recorded frame. Both dual threads now set an
  explicit 2 MB stack — larger than `VENC_REC_WRITER_STACK_BYTES`, because the
  writer thread only ever reaches `write_video` while this path reaches both
  buffers. Star6E is glibc and its 8 MB default already covered it, which is
  why this went unnoticed; the constant is set there too so the requirement
  travels with the code path rather than with the libc. The comment in
  `venc_rec_writer.h` claiming musl for "every SigmaStar target" is corrected.

- **A coalesced rotation request is no longer lost.**
  `star6e_ts_recorder_take_idr_request()` clears the pending flag before the
  caller knows whether the shared 100 ms IDR limiter will honour it, and
  `check_rotation()` has already counted that ask against
  `TS_RECORDER_MAX_IDR_REQUESTS`. Eight limiter collisions therefore exhausted
  the budget and rotation degraded to waiting for a natural keyframe — which a
  GDR craft (`resilience=racing`, the shipped FPV config) never produces,
  silently disabling `max_seconds` and `max_mb` for the rest of the recording.
  All five call sites across the three backends now re-queue.
  `runtime_rotate_idr_on()` and the new `cv610_rotate_idr()` return 1/0/-1 so
  "coalesced" is distinguishable from "issued"; `cv610_request_idr()` keeps its
  API-callback contract unchanged.

- **`video0.qpDelta`'s default is no longer split between the code and the
  shipped JSON.** `venc_config_defaults()` still seeded `-4` while
  `waybeam.default.json` and `.maruko.json` shipped `-12` since 0.73.0. A
  config that omits `qpDelta` therefore got `-4`, and `GET /api/v1/defaults`
  actively reverted a craft to it — an IDR cost roughly 9x the shipped
  default. The seed is now `-12`. CV610's shipped config no longer carries the
  key at all, since that backend does not read it.

- **An oversize access unit is no longer a silent frame loss.** The ring
  counts `oversize_drops` (mirrored into the header's `other_drops`) but
  nothing was logged, while 0.73.0 removed the frame-size caps meant to
  prevent it and ring v2 removed the recovery IDR that healed the chain. Both
  SigmaStar backends now emit one WARN per pipeline start naming the slot
  size. Deliberately no IDR request: an AU that overflows the slot does so
  because the bitrate genuinely exceeds it, which repeats every GOP.

- **`video0.intraRefreshQp` is range-checked at the API.** `FT_UINT8` accepted
  0..255, persisted it and echoed it back, while the config loader clamped to
  51 on the next start — so the API reported a value the encoder would never
  use. Now rejected like `min_qp`/`max_qp`, with a regression test.

- **A refused cold-boot QP-bounds apply is visible on Maruko.**
  `apply_qp_bounds()` refuses on a VBR/AVBR `rcMode`, and the Maruko cold-boot
  call discarded the result, so the operator booted with no QP bound and no
  indication. Now warns, matching CV610.

- **CV610 sets `rec_locks_ready` after the recorders it guards.** A calloc'd
  `ts_recorder` has `fd == 0`, which `cv610_record_stop()` reads as an open
  file, so the flag being set one statement early left a window where a stop
  would `fdatasync(0)`/`close(0)` — closing STDIN. Unreachable today, but the
  flag means "the things I guard are initialised" and must not lie.

- **Star6E gained the two guards its siblings already had.** A refused
  cold-boot QP-bounds apply is now reported instead of discarded (it refuses on
  a VBR/AVBR `rcMode`, so the craft flew with no bound and nothing in the log),
  and `rec_locks_ready` is published after both recorders are initialised
  rather than ~45 lines earlier — `venc_api_register()` and the record HTTP
  control were being published inside that window, where a calloc'd recorder's
  `fd == 0` reads as an open file and `GET /api/v1/record/status` would report
  `active:1` on a recording that does not exist.

- **Maruko `record.mode=dual` starts its file on a keyframe.** The dual
  bail-out skipped the start IDR on the grounds that chn 1 asks for its own —
  true on Star6E, where the drain thread does it, but Maruko has no such path
  and its HTTP record controls are gated `!ctx->dual`, so nothing asked at all.
  On a GDR craft that meant a recording with no IRAP anywhere in it. The
  request is now issued on chn 1, the channel that actually feeds the file.

- **A re-queued rotation request cannot outlive what it was for.** Now that a
  coalesced ask is re-queued rather than dropped, one can still be in flight
  when the IRAP arrives or the recording stops, so both sites clear the pending
  flag — otherwise it would keyframe the live channel for a rotation that
  already happened, or fire into the next recording.

- **The dual-thread stack is raised, never lowered.** The explicit 2 MB is now
  applied only when the platform default is smaller, so musl's 128 KB comes up
  while Star6E keeps glibc's 8 MB — that thread also runs SDK entry points
  whose stack use is opaque, and there was nothing to gain by reducing a
  platform that was already safe.

- **Documentation corrected against the code.** The field table claimed
  `min_qp`/`max_qp` were "Star6E + CV610 only" nine lines above a support
  matrix that correctly said all three (Maruko gained them in 0.73.0). The
  `/api/v1/version` example still showed 0.72.0/0.21.0. The `qpDelta` sign in
  `HTTP_API_CONTRACT.md` was stated backwards — it said negative values lower
  the I-frame QP, when they raise it and shrink the I-frame, which is the
  whole basis for the `-12` default. The capabilities example advertised
  `qp_delta` and `intra_refresh_qp` as both supported, which no backend can
  report. A stale paragraph still described the CV610 `qpDelta` boot rejection
  that 0.73.0 removed, and the sample config still showed the old `-4`.

## [0.73.0] - 2026-08-29

`video0.qpDelta` now reaches the encoder on Star6E when QP bounds are also
configured, Maruko gains `minQp`/`maxQp`, and CV610 gains `intraRefreshQp`
while losing `qpDelta`. `contract_version` 0.21.0 -> **0.22.0** (additive
field; two per-backend `supported` flags change).

- **Fixed `video0.qpDelta` being silently reverted at startup (#255).**
  `MI_VENC_GetRcParam` does not reflect a just-written value until the driver
  commits the block — measured on SSC338Q as between t+0s and t+5s after
  `StartRecvPic` — so a second read-modify-write inside that window read a
  stale 0 and wrote it back. `apply_qp_bounds()` is exactly that shape and
  runs immediately after `apply_qp_delta()`, so **every Star6E craft with
  `minQp` or `maxQp` set flew with an effective IPQPDelta of 0**: IRAP
  66497-81400 B against 3159-3743 B once fixed, on one binary with only
  `minQp` differing. Both calls returned success and both logged it.

  Every RC write now stages the whole intent instead of patching a `Get`
  result, so ordering and timing cannot matter. The deleted
  `apply_max_frame_size()` had the same shape and is how the bug first
  surfaced — it bit through two different callers.

- **Added `video0.minQp` / `video0.maxQp` on Maruko.** Same MI VENC rate
  controller as Star6E, so the same `u32MinQp`/`u32MaxQp` write. Applied from
  config at startup as well as live. Device-measured on Infinity6C: `minQp 40`
  took the stream from 1.54 Mbps to 0.09, `maxQp 20` drove it to 45.31 Mbps,
  and both cleared cleanly.

  **Upgrade note**: these fields were always parsed, validated and persisted —
  only HTTP writes and the `supported` flag were gated. A Maruko craft already
  carrying `minQp` in a shared config has been booting with it inert and will
  now apply it. Check craft configs before rolling out.

- **Added `video0.intraRefreshQp`** (restart-only), the intra-refresh stripe
  QP. It was already a config member consumed by `intra_refresh_compute()` as
  `override_qp`, but was never parsed from JSON, so every craft ran
  `mode_default_qp()`. `0` keeps the resilience preset's default.

  Advertised on **CV610 only**, where it is the sole control that moves
  I-frame size: 0 (preset 36) -> 15442 B, 44 -> 7773 B, 51 -> 4716 B at CBR
  1500 kbps, rate flat. It reaches `MI_VENC_SetIntraRefresh` on the SigmaStar
  parts too and is logged as applied, but the encoder ignores it — a 38-QP
  sweep moved IRAP 0.7% and the delivered rate not at all. A consequence worth
  knowing: `mode_default_qp()`'s per-mode stripe QP (36/32/28) is therefore
  inert on Star6E and Maruko.

  On CV610 this one register sets both the forced-IDR size and the GDR
  recovery-anchor quality; they cannot be separated.

- **Removed `video0.qpDelta` from CV610.** The CBR rate controller stores
  `gop_attr.normal_p.ip_qp_delta` and ignores it — flat to <=1.2% across
  -12..+12, live and at channel create, saturated and unsaturated. It is no
  longer advertised, no longer range-checked (an inert field must not decide
  whether a craft boots), and no longer written at channel create, where its
  narrower [-10, 30] range made a shared config carrying the portable
  `qpDelta: -12` fail `ss_mpi_venc_create_chn` outright.

- **Default `qpDelta` is now -12 on Star6E and Maruko.** The shipped defaults
  are `gopSize 1.0` with `resilience: off`, i.e. a real IDR every second; at
  -4 that IDR is many times a P-frame. At -12 it is no larger than one
  (Star6E 29429 -> 3622 B, Maruko 16575 -> 7739 B) at unchanged delivered
  rate. CV610's default is left alone — `qpDelta` is inert there.


## [0.72.0] - 2026-08-29

The per-frame size caps are removed. `contract_version` 0.20.1 -> **0.21.0**.
**Breaking**: two config fields deleted, and a `GET /api/v1/set` or
`/api/v1/live/set` naming either now fails the whole request.

- **Removed `video0.maxIBytes` and `video0.maxPBytes`** from the config, the
  capabilities payload, and the Star6E and Maruko backends, along with
  `apply_max_frame_size()` and the now-unused `MI_VENC_SetRcPriority` binding
  on both. CV610 never implemented them.

  They never imposed the ceiling they named. Device-measured on a SSC338Q
  (IMX335 1280x720@60, H.265 CBR, GDR via `racing`), 2026-08-28, with the
  encoder's own bitstream as the tap and every step confirmed applied by
  `apply_max_frame_size()`'s own `priority=framebits` log line:

  | commanded `maxPBytes` | predicted | delivered |
  |---:|---:|---:|
  | 33144 B | 15909 kbps | 19333 kbps |
  | 25000 B | 12000 kbps | 19319 kbps |
  | 16000 B | 7680 kbps | 19285 kbps |
  | 10000 B | 4800 kbps | 19319 kbps |
  | 6000 B | 2880 kbps | 19327 kbps |

  Flat across a 5.5x range; at 6000 B every one of 863 access units exceeded
  the cap, mean 40247 bytes. `maxIBytes` stepped 91788 -> 26000 -> 8000 -> 2000
  left IDR size at 66-81 KB across **16 sampled IDRs** — a 46x cap range. That
  arm is the weaker of the two: the stream is GDR, so its IDRs are on-demand
  artefacts of the measurement tap rather than a natural population. Three
  controls make the result attributable: CBR tracked its 19092 kbps target to
  within 1.3% (delivered 19285-19333, slightly over — the rate controller was
  working), raising `video0.minQp` to 30 on the
  same scene gave ~490 bytes/frame (the encoder can produce frames that small),
  and every step was re-read from `/api/v1/get`. Startup application was not
  the gap either. The **I-cap arm** was corroborated on a second Star6E rig in
  OpenIPC/waybeam_venc#111 (2026-08-22, 10 Mbps: IDRs an identical 42-44 KB at
  caps 2000 / 26000 / 8) — a closed, unmerged PR that probed the I-cap only, so
  it does not corroborate the `maxPBytes` sweep above.

  **Scope.** One SSC338Q, one SDK build, 720p60 H.265 CBR, GDR. Maruko was not
  measured in this sweep; removal there is taken on parity with Star6E and is
  the operator's call, not a Maruko measurement.

  **The claim is bounded, and 0.45.0 is why.** That release recorded
  `maxPBytes=2000` moving a Star6E from 5619 to 1868 kbps — real influence, 3x
  below this sweep's floor of 6000 B, and this sweep did not re-test at 2000 B.
  It reconciles rather than contradicts: 1868 kbps at the bench's 60 fps
  (`.201`, IMX335 — the rate is recorded, the cadence is not, and the
  conclusion is cadence-sensitive: at 120 fps the same numbers would bind) is
  ~3892 B/frame against a 2000 B cap, ~1.95x over. That is a whole-stream mean
  against a P-only cap, so it mixes in uncapped I frames and the true P mean is
  lower — the error runs toward binding — and a mean cannot show that no frame
  was under the cap. The 2000 B point has never had a per-AU census on either
  backend.

  The one Maruko datum comes not from #111's body but from the comment in which
  its author **withdrew** that PR's "inert" wording: `maxIBytes=2000` moving a
  Maruko IDR *median* 12195 -> 5866 B, ~3x over. A median is the stronger form
  — at least half the IDRs exceeded the cap. The caps
  *influence* below ~6000 B and *bind* nowhere they have been measured. It is
  the not-binding that disqualifies them as a ceiling, not an absence of
  effect.

  **Upgrade note.** A config file still carrying the keys loads fine — unknown
  keys are ignored and they disappear on the next config write. A `GET
  /api/v1/set` **or** `GET /api/v1/live/set` **naming** them does not: the
  multi-field preflight rejects the whole request with `404 unknown config
  field` on the first unrecognised key, so a stored "apply my profile" batch
  that still carries them applies none of its other fields.
  `/api/v1/live/set` is the path a volatile-first controller hits first, and it
  fails the same way. Strip them from saved batches, and from any controller
  that pushes them, before upgrading.

  **Deployment order is craft-side, not ground-side.** The controller that
  pushed these caps (waybeam-link) runs on the *same box* as venc and talks to
  `127.0.0.1:80`; ground nodes never pushed them. So on each craft, update the
  controller BEFORE this venc. Get it backwards and the stale controller's cap
  transaction 404s at the head of its queue forever — write-on-change never
  advances past a request that never succeeds — which starves its bitrate, fps
  and IDR pushes too, not just the caps. That exact starvation was already
  observed on CV610, which answers 501 to these fields.

  **What to use instead — and what has no replacement.** For I-frame size,
  `video0.qpDelta` is the strongest lever left: a 68x range on the same rig
  (-12 -> ~3.8 KB IDRs, 0 -> ~67-82 KB, +12 -> ~261 KB), and it does not
  keyframe. (#111 read the opposite — "qpDelta barely moves IDR size, 43 KB at
  0 and at -6" — measured before #255 was understood, and consistent with a
  startup apply that never reached the encoder.) **It has an open defect of its
  own**: issue #255 — applied from venc's startup path it logs success without
  reaching the encoder, so a craft boots with IDRs up to ~20x larger than its
  config asks for (at `qpDelta=-12`; ~4x at -6) and only a live write corrects
  it. Fix #255 before depending on this to bound the IDR.

  For **P-frame** size there is no direct replacement. `video0.minQp` is the
  surviving rate lever — the control above measured it taking the same scene to
  ~490 bytes/frame — but it is Star6E-only (venc rejects it on Maruko), so this
  removal leaves Maruko with no per-frame size control at all.

## [0.71.0] - 2026-08-28

Star6E and Maruko mirror-mode recording moves off the encode loop, matching
what 0.70.0 did for CV610. `contract_version` 0.20.0 -> **0.20.1**:
`droppedFrames` and `writerPeakDepth` already existed, but this release widens
what `droppedFrames` counts and makes both per-recording rather than
per-process — see the contract change log.

Review found two defects in the first cut of this work, both of which shipped
past a green suite. Both were then fixed a second time, by deleting the
mechanism they lived in — that story is below, because the reason they got
through is more useful than the fixes are.

- **Same defect, same fix.** Both backends called the recorder from their
  encode loop between the transport send and ReleaseStream, with a blocking
  `write(2)`: `star6e_runtime.c` and `maruko_pipeline.c` both wrote, then
  released. When storage blocks, the release is late, the encoder's output
  queue backs up, and the LIVE stream stalls with it. Star6E's **dual** mode
  already drained ch1 on its own thread and was never affected; mirror mode
  was the gap.

  Stated plainly: the *symptom* was measured on CV610, not on these two.
  Star6E's bench SD is native MMC at 16 MB/s and absorbs everything short of
  abuse; Maruko has no storage that can block at all. The fix lands on the
  structural argument — identical call-site shape — plus CV610's measurement,
  with a device no-regression pass on both.

- **Moving the recorder to its own thread broke two things, and the second
  attempt at fixing them deleted the mechanism instead.**

  *The drain was not a barrier.* A `venc_rec_writer_drain()` existed so a
  caller could close the segment file the instant it returned. It waited for
  the queue to empty — but the writer pops under the lock and runs the sink
  outside it, so "queue empty" was true for the whole duration of a `write(2)`.
  The drain returned with the sink still writing and the caller closed the file
  under it: the tail of one recording landed in the next file, over its PAT/PMT
  and opening IRAP. The frequency was inverted against the design goal — rare
  on a healthy card, and on a stalling card, the only case this thread exists
  for, every time.

  *Rotation silently dropped frames.* Segment rotation moved onto the writer
  thread with it, and rotation holds `fd == -1` across `fdatasync`/`close`/
  `open`. The producer gated on `fd >= 0`, so every frame in that window fell
  to the synchronous fallback, hit its own `fd < 0` guard, and was discarded
  and counted nowhere — right after the new segment's opening IRAP.

  Both were first patched in place: an `in_flight` flag to make the drain a
  real barrier, a `rec_state_lock` for the case the bounded drain could not
  cover, a `venc_rec_writer_reset_counters()` so a new recording did not
  inherit the last one's drops, and an `is_recording()` predicate for the
  producer gate. Three adversarial reviews then returned about a dozen
  findings, and nearly every one had the same shape: a fix applied to one of
  the several places a mechanism reached. That is the argument against the
  mechanisms, not for more fixes.

  **So the writer's lifetime is now tied to the recording it serves** — one
  writer per recording on all three backends, as CV610 already did. It is
  created after the recorder file is open and stopped before it closes, and
  both stops end in an unconditional `pthread_join`. That join is the barrier;
  nothing else is needed. `venc_rec_writer_drain()`, `in_flight`,
  `venc_rec_writer_reset_counters()` and `rec_state_lock` are all deleted, the
  counters are per-recording because the writer is, and the producer gate
  becomes `rec_writer != NULL` — a handle that exists for exactly as long as
  the recording, so a rotation is invisible to it and cannot cost a frame.

  Two things the redesign surfaced that the patched version had wrong:

  - **`venc_rec_writer_stop_bounded()` was not actually bounded.** Setting
    `stopping` does not stop the writer thread short — it drains its queue to
    the end and only then honours the flag — so the join could block for a
    whole 4 MB backlog on the stalled card the writer exists for. It now
    abandons the queue before the join, which is what makes its documented
    bound (grace + one write) true. Maruko's teardown depends on that: its
    watchdog reboots the craft.
  - **A recorder that stops itself needs someone to notice.** Disk-full and
    write errors end a recording from the writer thread. The old gate handled
    that implicitly, because it read recorder state; a gate that reads only
    the writer handle does not, and would go on copying, allocating and
    queueing a frame per encode into a dead writer indefinitely. So all three
    backends now check `star6e_record_wants_frame()` once per frame and tear
    the writer down when it goes false. CV610 has had this leak since 0.70.0,
    where the gate was already the handle alone.

    What was **not** wrong, and was checked rather than assumed: reporting.
    Every backend's status callback already read `is_recording()`, so a
    self-stop was always reported honestly. Device control on `.2.232` — a
    56 MB tmpfs filled until the recorder stopped itself — has both the
    pre-redesign and the per-recording binary reporting `active: false`,
    `stopReason: disk_full` within one poll. The difference is the writer:
    it lingers on the old build and is joined and freed on the new one
    (thread count 9 -> 8).

  Rotation loss was never observed on the Star6E bench, and the reason is
  worth recording: the same `fdatasync`/`close`/`open` that opens the window
  also stalls the encoder, so no frames arrive to be dropped. Five runs there
  — fixed and unfixed — all show the recorder writing exactly what the encoder
  produced. On hardware where rotation is slow but does not stall the encoder,
  the window is live. The case for the fix is the source-level defect plus a
  mutation test, not a device measurement.

- **`star6e_output_stream_flatten()` and `maruko_video_stream_flatten()`.**
  The writer needs its own copy, because SDK stream memory dies at
  ReleaseStream. These also remove an existing cliff: both
  `*_ts_recorder_write_stream()` flattened into a **512 KB automatic and
  dropped the whole access unit** when it did not fit. At 19 Mbps an IRAP can
  exceed that, and a silently dropped keyframe is the worst frame to lose.
  Not an extra copy for the TS path, which was already flattening onto the
  stack.

- A writer whose thread fails to start falls back to the synchronous path
  rather than silently recording nothing. Star6E and Maruko keep the recording
  open in that case and write on the encode loop; CV610 has no such fallback
  and closes the recording instead of reporting a phantom one.

Device, after the per-recording rework (2026-08-28):

- **star6e `.2.232`**, IMX415 1280x720@60, `resilience=racing` (GDR),
  recording TS+Opus to the SD card. Manual record 69 s: 4149 frames /
  59.93 fps against a nominal 60, `droppedFrames` 0. Rotation at
  `max_seconds=20`: 4 segments in 66 s, 3971 frames / 59.95 fps,
  `droppedFrames` 0 across all three rotations, and every segment opens on
  an `I` frame — the forced IDR lands on a GDR craft at record start and at
  each rotation. Boot auto-start: same, 2 segments, both opening on an `I`.
  Full decode of a segment: 251 frames, no video errors.
- **The writer's lifetime is directly observable.** Thread count 8 -> 9 on
  record start and 9 -> 8 on stop, on every one of five recordings, and
  9 -> 8 on a self-stop as well.
- **`writerPeakDepth` measures the rotation window.** It reads 1 while
  writing steadily and jumps to 12 and then 20 at rotations — the queue
  absorbing the `fdatasync`/`close`/`open` stall that used to sit on the
  encode loop, with no frame lost to it.
- **maruko `.2.233`**, 30 fps, recording to tmpfs (this craft has no block
  device). 3 segments at `max_seconds=10`, 816 frames / 30.0 fps exactly,
  `droppedFrames` 0, every segment opening on an `I`, a full segment
  decoding to exactly 300 frames. Threads 6 -> 7 -> 6.
- **cv610 `.2.181`**, 100 fps, GDR. On tmpfs: 3 segments at `max_seconds=10`,
  3231 frames / 100.0 fps exactly, 0 dropped, every segment opening on an `I`,
  a full segment decoding to 1100 frames. On the **real FAT32 USB volume**:
  5 segments, 4518 frames / 100.03 fps, 0 dropped across 4 rotations, all five
  segments opening on an `I`, a segment decoding to 1101 frames — and
  `writerPeakDepth` 1 -> 5 -> 6 -> 7, the real rotation cost being absorbed by
  the queue exactly as on the star6e card. Threads 6 -> 7 -> 6 on both.
- **The CV610 self-stop leak is demonstrated, not asserted.** Same box, same
  test (a 52 MB tmpfs filled to the free-space floor), shipped 0.70.0 versus
  this build: both report `stopReason: disk_full`, but 0.70.0 leaves the
  writer thread alive — 6 threads before, **7** still 24 s after the recorder
  stopped itself — while this build tears it down inside one poll (7 -> 6).
  That thread goes on receiving a copied, allocated access unit per encoded
  frame, forever, because its gate is the handle alone.

Both benches were restored to their master binaries and original config
afterwards; no RF was raised for any of it (waybeam-link stayed down).

Three adversarial reviews then ran against the finished change — concurrency
and lifetime, behaviour preservation, and test coverage. They cleared the
design on the questions it rests on (no sink/close race on any path, no lock
inversion, no use-after-free, format dispatch and status fields unchanged,
rotation transparency holding) and found four things worth fixing:

- **The self-stop teardown was inside the GetStream/ReleaseStream window** on
  all three backends. It ends in a join over the access unit still in the
  sink's hands, which on the failing card that triggered the stop is a
  blocking write — so it held the encoder's output slot and stalled the live
  stream, the exact coupling this writer removes, reintroduced on the one path
  that only runs when storage is already misbehaving. Moved after the release,
  beside the rotation IDR request that is there for the same reason. Two of
  the three reviewers found this independently.
- **Boot auto-start forced an IDR on the wrong channel in `record.mode=dual`.**
  The shared open helper names chn 0, but in dual mode the file is fed by
  chn 1 — so it injected a keyframe into the live stream, re-armed the rate
  limiter so chn 1's own request was swallowed, and did nothing for the
  recording. That is verbatim the trap documented in
  `include/star6e_ts_recorder.h`. Dual mode now returns from the open helper
  before the writer start and the IDR, which also makes the invariant the
  producer gate rests on ("the handle is non-NULL exactly while a mirror
  recording runs") true rather than nearly true.
- **star6e's teardown flush was unbounded**, and the comment called it free.
  It is not: the teardown watchdog SIGKILLs after 3 s and then writes
  `sysrq-b`, so waiting out a stalled card there trades a lost recording tail
  for an emergency reboot. Bounded at 500 ms, the same reasoning maruko
  already used for its reboot watchdog.
- **CV610 kept two defects of its own**, both predating this change and both
  now fixed: its status callback read the 64-bit counters outside the lock
  that the store side takes, and it had no readiness flag, so an init failure
  before the recorders were set up reached `star6e_ts_recorder_stop()` on a
  zeroed state whose `fd` of 0 passes the `fd < 0` test — `fdatasync`ing and
  closing **stdin**.

The test review proved three coverage holes with surviving mutants rather than
asserting them, and all three are now closed: `stop_bounded(NULL, …)` had no
test despite running on every record start; nothing asserted the *positive*
half of the bounded contract, so the production 250 ms grace could have been
cut to an eighth with the suite green; and the unbounded stop's barrier was
caught only as a heap-corruption abort. It also showed the rotation sampler
failing on **correct** code in 6 of 8 runs at 4x CPU oversubscription — the
sampler thread was never scheduled before the writes finished — which is a
gate testing the machine rather than the code. It now waits for the sampler to
reach a CPU first, and is 6/6 green under the same load.

Re-verified on hardware AFTER those fixes, since three of the four touch the
SigmaStar backends:

- **star6e `.2.232`**, real SD card, 60 fps, GDR. Rotation at
  `max_seconds=10`: 5 segments, 2658 frames / 59.93 fps, `droppedFrames` 0
  across 4 rotations, every segment opening on an `I`, and `writerPeakDepth`
  1 -> 20 — the same rotation signature as before the fixes. Self-stop on a
  56 MB tmpfs, with the teardown now in its new position after the release:
  threads 8 -> 9 -> 8 and `stopReason: disk_full` within one poll. Boot
  auto-start: 3 segments, 0 dropped. Teardown **while recording**, three
  cycles: 1.4-1.9 s against a watchdog that SIGKILLs at 3 s, with no
  watchdog, sysrq or kill event in the log.
- **maruko `.2.233`**, 30 fps. Rotation: 4 segments, 965 frames / 29.98 fps,
  0 dropped, every segment opening on an `I`, a full segment decoding to
  exactly 300 frames. Boot auto-start: 2 segments, 0 dropped. Teardown while
  recording: 1756 ms twice, with uptime continuing to climb — the
  `reboot(RB_AUTOBOOT)` watchdog never fired.
- Threads 8 -> 9 -> 8 (star6e) and 6 -> 7 -> 6 (maruko) on every recording.

**Maruko's self-stop cannot be exercised on that hardware, and this is
permanent.** Its SD slot detects no card (the `ms_sdmmc` controller probes and
`mmc0` exists, but `/sys/bus/mmc/devices/` stays empty), and the kernel has no
SCSI bus and no `usb-storage`/`sd_mod`, so a USB drive cannot substitute
either. With no block device, `star6e_recorder_free_space()` returns 0 on its
ramfs and the disk-full guard is skipped entirely. What covers maruko instead:
the detection predicate and the recorder's disk-full stop are shared code
exercised by the host suite, and the three-line reaction is identical in shape
to star6e's and CV610's, both of which are device-verified — CV610 as a direct
A/B against the shipped build.

## [0.70.0] - 2026-08-27

Snapshot and recording reach the CV610 backend, and the config file is read
exactly once at startup. Contract `0.19.0` -> `0.20.0` — **additive**: no
field was removed, renamed, or given a new meaning, and a client written
against `0.19.0` sees strictly more capability and no changed response shape.

- **`GET /api/v1/snapshot.jpg` on CV610.** It was the one route a backend
  omitted entirely. The JPEG channel binds as a **second destination** on the
  same VPSS output the H.265 channel already consumes — the SDK allows four
  bind targets per source — so it needs no VPSS channel of its own: no extra
  scaling, no extra VB pool, and nothing running while nobody is asking. The
  bind is permanent and only the channel's receive state pulses
  (`start_chn(recv_pic_num=1)` -> `get_stream` -> `stop_chn`), which
  deliberately avoids reconfiguring the source per request.

  Because it shares the main stream's VPSS channel it inherits that geometry.
  `snapshot.width` / `snapshot.height` are advertised **unsupported** on CV610
  rather than accepted and ignored.

- **Recording on CV610 in `record.mode=mirror`**, `format=ts` (Opus muxed) and
  `format=hevc`, with rotation and `/api/v1/record/{start,stop,status}`.

  No SDK-typed adapter was needed. The CV610 drain loop already copies each
  access unit into one contiguous Annex-B buffer before any consumer sees it,
  and `star6e_ts_recorder_write_video()` has always taken a flat buffer. Only
  the `.hevc` path was missing a SoC-independent entry point, so
  `star6e_recorder_write_au()` was added beside the existing iovec writer,
  carrying the same side-effect order: disk-space check, frame boundary,
  write, truncate-back-to-boundary on failure, then counters and the
  `sync_file_range` cadence.

  `record.bitrate` / `fps` / `gop_size` / `server` stay **unsupported**: they
  describe the second VENC channel that `dual` and `dual-stream` need, which
  this backend does not create. Those modes are refused with a warning rather
  than silently recording ch0 at the wrong bitrate.

- **Record start forces an un-coalescible IDR on CV610.** The shipped
  configuration is `resilience=racing` — a GDR craft emits no periodic IDR at
  all — so a request the rate limiter coalesced away would leave a recording
  with no IRAP access unit anywhere in it: a file that seeks to nothing and
  plays from nothing, while the caller was told the start succeeded. Same
  reasoning and same path as Star6E.

- **The shared recorder is no longer gated behind a SigmaStar-only guard.**
  `star6e_recorder.c`, `star6e_ts_recorder.c` and `ts_mux.c` were always
  SoC-independent apart from their SDK-typed adapters; the guard around those
  adapters read `#ifndef PLATFORM_MARUKO`, which silently included them in any
  build that was not Maruko. It now names what it actually requires —
  `star6e_output.c` being linked — so CV610 can use the cores.

  Verified as a no-op for the existing backends: with the version strings held
  constant, `star6e_ts_recorder.o` and `ts_mux.o` are **byte-identical** on
  both star6e and maruko, and `star6e_recorder.o` differs only by the added
  `star6e_recorder_write_au` — no pre-existing symbol changed or disappeared.

- **`RECORDER_DEFAULT_DIR` had two definitions.** `venc_api.c` carried its own
  copy for CV610 because that backend did not link the recorder header. Now it
  includes the header like everything else.

- **Recording no longer stalls the live video path.** Every backend called the
  recorder from its encode drain loop, between the transport send and the
  SDK's ReleaseStream, with a blocking `write(2)`. When storage blocked,
  ReleaseStream was late, the encoder's output queue backed up, and the LIVE
  transport stalled with it.

  Device-measured on CV610 with the same card, same mount and the same
  stimulus (three concurrent direct-IO writers on a `-o sync` mount):

  | | stream min | stall seconds | recorder |
  |---|---|---|---|
  | synchronous | 608 pkt/s (42% of baseline) | **15 / 18** | 33-44 fps |
  | async writer | **1462 pkt/s (100%)** | **0 / 18** | 43-66 fps |

  The queue absorbed the stall (depth 1 -> 358, hitting its 4 MB cap), then
  shed recording frames — 526 of them, every one counted and reported through
  the new `droppedFrames`. A slow disk now costs recording frames and never
  the live link, which is the 0.69.0 principle applied one layer up.

  Star6E's dual mode already drained its second channel on its own thread and
  was never affected; this gives mirror mode the same property. **The star6e
  and maruko mirror paths still write synchronously** — the coupling is
  identical there by construction (`star6e_runtime.c`,
  `maruko_pipeline.c`) and wiring them needs their own device runs.

- **Rotation asks the right channel, from the right place, at the right rate.**
  An adversarial review of the first cut found the rotation hook was wrong
  three ways, all on the two backends that had not been on hardware: it fired
  *inside* the SDK's GetStream/ReleaseStream window, which no other IDR call
  site on any backend does; a single shared `int (*)(void)` could not name the
  channel, so under `record.mode=dual` it keyframed the LIVE stream while the
  ch1 recording rotated on nothing (a defect `star6e_runtime.c` already
  carried a fix for at record-start); and on Star6E it resolved to the
  *forced* path, which re-arms the rate limiter and would swallow a scene or
  operator keyframe arriving in the next 100 ms.

  The callback is gone. `check_rotation()` now raises a flag and the backend
  services it after its own ReleaseStream, coalesced, on the channel that
  actually feeds that recorder. Requests are bounded
  (`TS_RECORDER_MAX_IDR_REQUESTS`) so an unanswered ask degrades to the old
  wait-for-a-keyframe behaviour instead of becoming a permanent 1 Hz keyframe
  tax, paced by elapsed interval rather than "a different second", and reset
  per recording.

  They are also **grace-delayed by one second**, which is a measured
  correction rather than a precaution: on Maruko at `resilience=off` with a
  1 s GOP, `max_seconds=15` over 50 s took the honored-IDR count from **1 to
  4** — three needless keyframes for three rotations that happened naturally
  anyway. With the grace, Maruko is back to **4 segments, 1501 frames, delta
  1**, identical to 0.69.0.

- **`record.max_seconds` / `max_mb` were inert on a GDR craft.** Rotation can
  only cut on an IRAP or the new segment decodes from nothing, and the check
  simply returned early when the frame was not one — correct, but it assumed
  the stream produces IDRs by itself. Under `resilience=racing` it never does,
  so a configured cap did nothing and the file grew unbounded. The recorder
  now asks for an IDR when rotation is due, rate-limited to one request per
  second so a crossed threshold cannot become per-frame keyframing.

  Measured on CV610 under `racing`, `max_seconds=15` over 50 s: **1 segment
  before, 4 after**, with `/api/v1/idr/stats` showing exactly 4 honored
  requests and 0 dropped, and each rotated segment opening on `key_frame=1,
  pict_type=I`. `Star6eTsRecorderState.request_idr` is the callback the
  original author declared for exactly this and never wired; it was removed as
  dead code in 0.69.0 and is restored here with the wiring that makes it live.

- Note for operators: the shipped `record.dir` default is `/mnt/mmcblk0p1`,
  which does not exist on the CV610 reference board (the bench mounts USB
  storage at `/mnt/sda1`). It is left at the shared default rather than
  hardcoding one board's mount point; set it for the target before recording.

### The config file is read exactly once

venc parsed `/etc/waybeam.json` twice at startup — once in `main()` for the
mDNS beacon, once in `backend_execute()` for the backend. The two reads were
not atomic and not equally trusted: a config write landing between them handed
the beacon and the encoder different snapshots of `system.web_port`, and the
beacon's load discarded its return value, so a malformed config announced a
service on defaults and *then* exited.

The config file is a program-level input, not a module-level one, so it is now
parsed once at the program boundary and the value handed down; modules take a
`const VencConfig *`, never a path. Loading before the beacon also means a
malformed config exits without ever having announced.
`BackendOps.config_path` is gone, and `VENC_CONFIG_DEFAULT_PATH` went from
seven mentions across five files to one call site.

## [0.69.0] - 2026-08-26

venc stops actuating on its own egress. Contract `0.18.6` -> `0.19.0`
(the intermediate `0.18.7` never shipped — see the folded section below).
**Breaking**: one config key removed, two `transport/status` fields removed,
and the frame-SHM ring header goes to **version 2**. A v2 producer will not
serve a v1 consumer -- waybeam-link, waybeam-hub and radeon-vrx must be
rebuilt alongside.

The principle: **venc is a sensor and an actuator-on-request, never a
controller.** It measures egress pressure and publishes it; it changes
encoder behaviour only when something asks.

- **Removed the frame-shm ring-fill bitrate clamp** and its config key
  `outgoing.shmThrottle`. On a `frame-shm://` craft the rate controller
  (waybeam-link) runs on the same SoC, already reads the egress ring, and
  already owns `video0.bitrate` -- so the clamp was a second controller on
  the same input signal, and its only actuator keyframes
  (`MI_VENC_SetChnAttr` emits an IDR on every real rate change).

  Measured by @vertexodessa on a Star6E, 720p120, ~11 Mbps, 2026-08-24:

  | | clamp on | clamp off |
  |---|---|---|
  | IDR rate at receiver | 6.5 /sec | 0.2 /sec |
  | glass-to-glass | ~100-143 ms | **~15-37 ms** |
  | ring read latency | 40-120 ms | **1-9 ms** |

  A config *file* still carrying the key loads fine — unknown keys are
  ignored and the key disappears on the next config write. A `POST
  /api/v1/set` naming it does **not**: the multi-field preflight rejects the
  whole batch with `404 unknown config field` on the first unrecognised key,
  so a stored "apply my profile" batch that still carries `shmThrottle`
  applies none of its other fields. Strip it from saved batches before
  upgrading.

- **Removed the ring-full recovery IDR entirely**, for GOP as well as GDR.
  It fired precisely when the ring was full, so the largest frame in the
  stream could not be delivered anyway -- measured on a SSC338Q with the
  consumer stopped, 13 IDRs in 12 s, none of which reached anyone. Recovery
  is now the operator-selected GOP cadence, or an explicit request:
  `/request/idr`, or waybeam-link's §3.9 `RECOVERY_REQUEST`.

  Known gap, stated rather than hidden: waybeam-link's §3.9 currently arms
  only on stream latch, not on mid-stream damage, and `venc.recovery_enabled`
  defaults false. Until that is wired to the receiver's existing damage
  signal, a mid-flight ring-full drop on a **plain-GOP** stream has no
  automatic heal until the next GOP boundary. GDR (`resilience=racing`), the
  flight configuration, heals within one refresh cycle as before.

- **Ring header v2 -- offset 88 changed meaning.** `throttle_permille`
  (`1000` = unclamped) becomes `low_water_slots`: the lowest ring occupancy,
  **in slots**, reached in the producer's last 200 ms window. `sizeof` stays
  192 and nothing before offset 88 moves, but **the polarity inverts** (a
  LOW number is now the healthy end), so this is a deliberate hard version
  break: consumers validate `version` and refuse to attach rather than
  silently misread a healthy ring as a catastrophic clamp.

  Low-water rather than peak, and that is the measured choice: on a Star6E
  at 100 fps into an 8-slot ring with a healthy consumer, the ring routinely
  spikes 2-3 slots inside a window and drains again, so a peak reading calls
  that congestion 15-25% of the time with nothing wrong. Low-water asks
  whether the ring failed to drain at *any* point, which is what separates a
  transient burst from standing backlog. The consumer cannot derive it
  itself -- it can sample occupancy any time, but the low-water mark
  *between* its own reads is producer-side knowledge.

  **The healthy reading is 1, not 0, and it is reported in slots for exactly
  that reason.** The sample is taken immediately after the frame is written,
  so a consumer that is keeping up still leaves one frame queued -- which is
  why the clamp this replaced recovered at `<= 1` slot and engaged at `>= 2`.
  Whether a permille encoding round-trips that 1 depends on the geometry: at
  the 8 slots every venc backend currently creates it does (125 permille
  exactly), but at 16 it does not -- 62.5 truncates to 62, which converts back
  to 0, a healthy ring indistinguishable from a drained one. The header does
  not fix `slot_count`, so an encoding whose fidelity varies with it is the
  wrong choice regardless of what today's producers pick. Raw slots have no
  such dependence, and a consumer wanting a fraction already has `slot_count`
  at header offset 8.

- **Producer drops that are not congestion are now visible to the consumer.**
  Only `full_drops` ever reached the ring header. A frame the producer could
  not build at all -- an oversize access unit, or one whose SDK packet table
  was incomplete -- simply vanished: the malformed-AU path counted nothing
  anywhere (one stderr WARN, latched, then silence), and the producer-side
  oversize counters lived in process-local ring stats no consumer could
  reach. With venc no longer self-healing, nobody downstream could tell a
  frame had gone missing.

  Header offset 96 now carries `other_drops`, kept strictly apart from
  `full_drops`: congestion the consumer is causing calls for slowing down,
  a frame the producer could not build does not. Malformed access units also
  get a transport-independent per-output counter (they happen on RTP too),
  and both surface as `otherDrops`/`badAuDrops` on `transport/status`.

- **Bootstrap IDRs are counted but never coalesced.** The 100 ms spacing gate
  exists to absorb storms from producers asking for a *fresher* picture (the
  scene detector, `/api/v1/idr`), where losing one costs nothing because
  another is already in flight. A bootstrap IDR is a different event: output
  enable, a destination change, a live fps rebind and recorder start each hand
  the stream to a receiver — or a file — that has seen no parameter set, so a
  coalesced request leaves it with nothing to start from while the caller is
  told the apply succeeded. On a GDR craft, which emits no periodic IDRs at
  all, "nothing to start from" is permanent: a recording with no IRAP anywhere
  in it seeks to nothing and plays from nothing.

  `idr_rate_limit_force()` honors those unconditionally, still books them in
  `/api/v1/idr/stats`, and re-arms the spacing window so an ordinary request
  right behind a bootstrap still coalesces — a bootstrap must not open a hole
  for a storm. Applied at all four sites on both SigmaStar backends. Only a
  *real* fps rebind qualifies: `apply_fps()` returns 0 for its unchanged-fps
  early return too, and firing on that turned a ground re-posting an unchanged
  profile into ungated keyframes at the caller's write rate.

  A failed bootstrap IDR is **not** fatal to the apply. The work it accompanies
  has already succeeded — in `apply_server` the socket is repointed before the
  request — so failing the apply would send venc into a rollback that re-enters
  the same failing call and can commit the new `outgoing.server` while the
  socket sits on the old one. It is logged instead, and both backends now
  genuinely behave the same way.

- **Every frame-shm producer measures its own ring.** The low-water tracker
  moved from a per-backend file static onto the output, because
  `venc_frame_ring_create()` publishes the `VHLT` health marker unconditionally:
  a ring nobody measured still advertised a live gauge sitting at its
  create-time `0` — the *healthiest* value in the range — so "not measured" was
  indistinguishable from "the consumer is keeping up perfectly". That affected
  CV610 (which measured nothing at all) and Star6E dual-stream ch1, whose
  second ring a shared tracker could not represent. `badAuDrops` is likewise
  reported on every transport now, not only `frame-shm`, since a malformed
  packet table happens on RTP too and there is no ring header to see it through
  there.

- **`GET /api/v1/transport/status`**: `throttlePermille` and
  `effectiveBitrateKbps` removed, `ringLowWaterSlots` added on the
  `frame-shm` branch. Read `video0.bitrate` from `/api/v1/config`; nothing
  scales it any more.

- Sidecar `TRANSPORT_INFO`: `throttle_permille` returns to `_pad[2]`.
  Trailer stays 16 bytes; later trailers keep their offsets.

- Debug OSD: the `thr<n>%` bitrate annotation becomes `ring<n>`, showing
  ring low-water in slots when it stayed above the healthy 1-slot band, so a
  stuttering picture separates "the consumer is behind" from "the encoder is
  behind".

### Folded in: the work first staged as `0.68.1`

This began as a separate release, "venc stops injecting IDRs of its own
accord" (contract `0.18.6` -> `0.18.7`). It never shipped under that number —
`0.68.1` and `0.68.2` below were released while it was in review — so it is
recorded here as part of `0.69.0`, the release that actually carries it.

Several items below were superseded again by `0.69.0` above: the ring-full
recovery IDR they kept for plain GOP is now removed outright, and the
throttle-clamp deadband they tuned is gone with the clamp. Kept for the
reasoning and the device measurements, which still stand.

- **`video0.bitrate`, `video0.qpDelta` and `video0.maxIBytes`/`maxPBytes` no
  longer request an IDR after applying** (Star6E and Maruko). These are all
  decoder-neutral rate-control state, absorbed mid-GOP by the rate
  controller; the frame-shm throttle clamp relied on exactly that for as long
  as it existed (`0.69.0` above removed the clamp, so that corroboration is
  historical — the SDK behaviour it rested on is unchanged). A controller that wants a resync point calls `/request/idr`
  explicitly. CV610 never IDR'd on these paths.

  Device-measured on a SSC338Q (IMX335 1920x1080@60, H.265 CBR, GDR via the
  `racing` preset) using the venc recorder as a bitstream tap, ten live
  writes spaced 300 ms, counting IRAP access units:

  | field | before | after |
  |---|---:|---:|
  | `video0.qpDelta` | 11 | 1 |
  | `video0.maxIBytes` | 11 | 1 |
  | `video0.bitrate` | 11 | **11** |

  The residual 1 is the recorder's own start IDR — a do-nothing control arm
  on each binary recorded exactly that one and nothing else, and with the
  `racing` preset the stream is GDR, so there are no periodic IDRs to
  confound the count.

- **The bitrate row above is a no-op on the wire, deliberately kept.**
  `MI_VENC_SetChnAttr` emits an IDR by itself on Star6E, and
  `MI_VENC_RcParam_t` carries no bitrate field on either SigmaStar backend,
  so there is no rate-only actuator to switch to and the keyframe survives.
  Two real defects are still fixed by removing the request: it consumed a
  slot in the shared 100 ms IDR gate, so a genuine recovery request (a
  ring-full drop, or `/request/idr` from waybeam-link) landing within
  100 ms of a bitrate write was silently swallowed; and it inflated
  `/api/v1/idr/stats` with one honored IDR per bitrate write that the write
  did not cause, which is what made the counters read as though bitrate
  writes were the IDR source. Calling `SetChnAttr` less often is left as a
  follow-up.

- **Maruko's `/request/idr` now goes through the shared per-channel gate.**
  `maruko_request_idr()` called the vendor request directly, making it the
  only IDR source on any backend that was neither rate-limited nor counted:
  an HTTP caller could issue unbounded back-to-back IDRs and
  `/api/v1/idr/stats` reported none of them. Compile-tested only; Maruko
  hardware was unreachable.

- **Contract corrections, no code change.** The `/api/v1/dual/set` `bitrate`
  row claimed venc issues an IDR; `dual_apply_bitrate()` only updates the
  channel attribute and never has. The `/api/v1/idr/stats` example reported
  `min_spacing_us: 250000` against a compile-time
  `IDR_RATE_LIMIT_MIN_SPACING_US` of `100000`.

- **The frame-shm ring-full recovery IDR is now reserved for GOP streams.**
  A drop told venc its ring overflowed, not that any decoder lost sync. On a
  GDR stream that distinction is decisive: a rolling intra refresh repairs a
  chain break within one cycle for free, so the recovery IDR bought only the
  gap until the sweep reached the damage -- and paid for it with the largest
  frame the encoder makes, pushed into a ring that was by definition full.
  On a plain GOP stream there is no other heal and the IDR is genuinely a
  lifesaver, so that case keeps it, paced by the existing 1 s holdoff.

  (Superseded within this same release train: `0.69.0` below removed the
  ring-full recovery IDR outright, for plain GOP as well as GDR, so the helper
  named next never appeared in a shipped binary. The reasoning is kept because
  it is the argument `0.69.0` then extended.)

  `venc_frame_drop_needs_idr()` folds this with the existing SVC-T enhance
  exemption so all three backends share one definition. No configuration:
  venc already knows which kind of stream it is producing.

  Device-measured on a SSC338Q with the ring consumer stopped for 12 s (ring
  pinned at 100%, 8/8 slots, ~900 drops):

  | stream | IRAP access units |
  |---|---:|
  | GDR (`resilience=racing`), before | 13 |
  | GDR, after | **1** |
  | GOP (`resilience=off`), after | **kept** |

  The single remaining IRAP is the recorder's own start IDR; a do-nothing
  control arm recorded exactly that one.

- **The ring-fill clamp deadbands its own writes.** *(Superseded by `0.69.0`
  above, which removed the clamp entirely; the deadband no longer exists.)*
  It re-programmed the
  encoder on every permille change, as often as every 200 ms — and each
  programmed rate change costs an IDR, so a clamp chasing an oscillating
  fill signal keyframed the link it was trying to unclog. Movement below
  100 permille (10%) no longer re-programs. Reach is preserved in both
  directions: both rails are exempt, and the comparison is against the last
  *applied* value so suppressed movement accumulates. Device-confirmed —
  with the consumer stopped the clamp still walked to its floor (permille
  50, 1909 kbps).

- **The fps-rebind IDR has one owner instead of three.** Output enable
  emitted two back-to-back requests (its own plus `apply_fps`'s, coalesced
  only by luck of the 100 ms gate — and coalescing the *recovery* IDR
  behind an unrelated one is exactly what that gate should not do); output
  disable emitted one *after* the output was already off; the startup idle
  transition emitted one with no consumer at all. Only the live
  `video0.fps` write needs one, because the rebind re-creates the encoder
  channel, so that is where it now lives. Enable emits exactly one,
  disable none.

- **Every Maruko IDR source is now counted; the coalescible ones are paced.**
  `maruko_request_idr()` called the vendor request directly, and the
  output-enable and server-change paths bypassed the gate too, so Maruko's
  `/request/idr` was the only IDR source on any backend that was neither
  rate-limited nor visible in `/api/v1/idr/stats`.
## [0.68.2] - 2026-08-26

CV610 / IMX662 colour. Both changes device-verified on `.181`.

- **`/api/v1/awb` now works on the CV610 backend.** `cv610_awb_query()` in
  `src/cv610_iq.c` reads `ss_mpi_isp_query_wb_info()` and
  `ss_mpi_isp_query_exposure_info()` and reports the gains AWB actually
  converged on, the CCM saturation the AGC bucket selected, the detected
  colour temperature, the interpolated CCM, scene status, Bv, and the
  exposure that selected all of it. `query_awb_info` already existed for
  star6e and maruko; cv610 returned 501.

  The pre-existing `wb` group in `/api/v1/iq` could not answer this. It reads
  back `ot_isp_wb_attr`, whose `manual_attr` holds the last value *written* --
  on a backend that never leaves auto WB it reads 256/256/256/256 forever.
  Diagnosing a colour cast without this was guesswork.

- **The ISP black level is no longer rescaled by the sensor's bit depth.**
  `cmos_get_isp_black_level()` sent `IMX662_BLACK_LEVEL_12BIT >> 2` for the
  RAW10 modes. `ot_isp_cmos_black_level` is `Format:14.0` -- a fixed 14-bit ISP
  domain, not the sensor's output depth -- so the correct value is `50 << 2 =
  200` in *every* mode. Every vendor plugin on this silicon uses one constant
  across all of its modes (sc450ai/sc431hai/sc500ai/sc4336p `0x410`,
  os04d10/gc4023 `0x400`), sc450ai included, which runs both a 12-bit linear
  and a 10-bit WDR mode off `BLACK_LEVEL_DEFAULT`.

  The shift left ~150 codes of pedestal unsubtracted on the 90 and 100 fps
  modes -- which is what the craft flies. AWB then measures `(B+p)/(G+p)`,
  biased toward 1, so it reads blue as relatively stronger than it is and
  under-applies blue gain; the residue itself is multiplied by 2.9x on B
  against 1.1x on R, so it lifts blue hardest where signal is smallest.

  Verified at matched analog gain 12.02x, before/after: `b_gain` 732 -> 791
  (+8.1%), `b/r` 2.511 -> 2.810 (+11.9%), CCT 2976 -> 2898 K. The direction
  and asymmetry were written down before the deploy. Operator verdict on the
  image: "a bit more natural".

**Not included, deliberately:** the residual white point is still roughly 5%
too blue at ~2900 K indoor. A candidate correction to
`IMX662_AWB_STATIC_WB_B` recovers about half of it, but its magnitude could
not be resolved above the bench's own repeatability (a repeat arm on one
unchanged build spread 6.2% once the room light drifted), and everything was
measured at a single colour temperature. Tracked with the patch attached.

## [0.68.1] - 2026-08-23

Header only. Bit 3 of the frame-SHM metadata flags is reserved for the
receiver-set SALVAGED flag. No encoder path changes and nothing venc emits
is different.

- **`VENC_FRAME_FLAG_SALVAGED` (0x08) is reserved in
  `include/venc_frame_ring.h`.** waybeam-link sets the bit on a frame it
  rebuilt from an unrecoverable FEC block — synthesized skip slices over the
  lost region, or a whole-picture freeze (its `PROTOCOL.md` §6.3b) — and
  waybeam-hub reads it to keep a repaired picture out of the recording as a
  seek point and out of the parameter-set cache. venc never sets it, but this
  header is the canonical definition of the format
  (`protocols/frame-shm.md`), and a future encoder-side flag claiming 0x08
  would have been read downstream as damage on every frame carrying it. Both
  consumers already carry the bit (waybeam-link `cde72f7`, waybeam-hub
  `a214f7a`); this is the third and last definition of the flag set.
  radeon-vrx's `frame_shm_format.h` defines only `IDR` and tests that bit
  alone, so it needs nothing and is unaffected.
- **`venc_frame_drop_breaks_chain()` is deliberately unchanged.** A salvaged
  frame is still a reference frame, so the ring-full drop policy keeps
  keying off `ENHANCE` alone. `tests/test_venc_frame_ring.c` asserts that,
  and that the four defined flag bits stay disjoint — the guard that fails
  if a later flag claims bit 3.

## [0.68.0] - 2026-08-23

CV610 only. No wire format or config schema change, but see the frame-SHM
metadata note below: a field that was already on the wire now carries a
different (correct) value.

- **CV610 now refreshes by COLUMN, so the GDR recovery frame keeps its slices.**
  Under `OT_VENC_INTRA_REFRESH_ROW` the intra band is a contiguous CTB range, so
  on the recovery frame the encoder carved it off as its own I slice and then
  ignored the configured row split: measured on .181 at 1080p100 with
  `sliceCount: 17`, that frame shipped **2 slices at addresses `[0,45]`** — on
  the largest frame in the stream and the one carrying VPS/SPS/PPS and the
  `recovery_point` SEI. waybeam-link's spatial repair drops any frame whose
  slice addresses fall outside the geometry it has learned, so the recovery
  frame was precisely the frame it could never salvage. A column band is not
  contiguous in raster order, so it cannot become a slice and the row split
  survives.

  Measured ROW -> COLUMN: refresh-AU slices 2 -> 17; refresh-AU first slice
  I -> P; refresh-AU size 1.32x -> 1.07x of the stream mean; stream mean AU
  11235 -> 11416 B against a 9264 kbps target. GDR markers unchanged
  (`recovery_point` every 200 frames, no IRAP). Not configurable: there is no
  reason to select ROW.

- **The intra-refresh sweep now derives from the picture WIDTH on CV610.**
  `intra_refresh_compute()` divides whatever extent it is given into 32-px
  units; it was being handed the height, so a column sweep covered 34 units on a
  60-unit-wide picture and ran 1.7x slower than the mode's target
  (19 frames measured, against the 15 that "fast" asks for at 100 fps). With the
  width it derives `refresh_num` 4 and a 15-frame cycle, measured converging in
  14 frames. The slice split still uses the height, because slices are always
  rows. The shared `intra_refresh_compute()` parameter is renamed `height` ->
  `extent` and documented, since silently passing the wrong dimension is what
  caused this.

  Consequence: on CV610, `total_rows` and `lines` in `/api/v1/intra/status` now
  count 32-px **columns** (60 at 1080p, not 34). The JSON key is shared across
  backends and was left alone rather than renamed for one of them.

- **frame-SHM `gdr_len` was wrong on CV610 and is now correct.** Byte 7 of the
  frame metadata (`protocols/frame-shm.md`) advertises the GDR cycle length in
  frames. It is computed as `ceil(total_rows / refresh_num)`, so with the
  height-derived extent it published **12** while the encoder's true sweep was
  20; it now publishes 15, matching the device log. No consumer reads the field
  today (waybeam-link parses it into `FrameShmMeta::gdr_len` and never uses it),
  so nothing changes behaviourally — but a future consumer would have been
  reading a wrong number.

  Star6E and Maruko are unaffected: `MI_VENC_SetIntraRefresh` has no direction
  field, they still pass the height, and their derivation is unchanged. Measured
  on .232, Star6E sweeps top-to-bottom and converges in ~9 frames at 60 fps.

## [0.67.4] - 2026-08-23

CV610 only. No wire format, config schema, frame-SHM layout or HTTP contract
changes.

- **A live `video0.gop_size` change silently dropped the CV610 stream out of
  GDR.** Writing `attr.rc_attr.h265_cbr.gop` resets the channel's intra-refresh
  state, so one slider drag converted a GDR stream (`recovery_point` SEI, no
  IRAP) into an IDR stream for the rest of the venc lifetime — and writing the
  old value back did not undo it, only a restart did. `/api/v1/intra/status`
  kept reporting intra refresh active throughout, because it serves a snapshot
  published once at startup. Measured on .181 by RTP capture at each step.
  `cv610_apply_gop()` now refuses the write while intra refresh is active and
  leaves the encoder untouched.

  Re-asserting intra refresh after the write was tried first and rejected: it
  restores GDR, but the recovery period stays where it was (identical at gop
  0.5/1.0/2.0/4.0) while the CBR window still moves, swinging the achieved rate
  2.7–11.2 Mbps against a fixed 9.26 Mbps target.

  Scoped to intra-refresh-active: with `resilience: "off"` the write proceeds
  unchanged, and that is the only configuration where `video0.gop_size` is the
  operator's value at all — a named preset already owns it. A live **bitrate**
  write goes through the same `set_chn_attr` path with the gop member unchanged
  and is unaffected. Star6E and Maruko keep live GOP control.

  **Known limitation:** `/api/v1/capabilities` still reports
  `video0.gop_size` as `mutability: live, supported: true` on CV610. The
  backend gate is key-based and cannot express "live only while `resilience`
  is off". The write now fails loudly instead of silently destroying GDR, but
  the capability surface does not yet tell the operator why.

## [0.67.3] - 2026-08-23

Follow-up to 0.67.2 from the upstream review. No wire format, config schema,
frame-SHM layout or HTTP contract changes.

- **The debug-OSD CPU% denominator went stale after CPU hotplug.**
  `OsdCpuSampler` latches `ncores` on first use, but the schedstat path counts
  only *online* CPUs, so a core going offline or coming back changes the
  correct denominator. 0.67.2 dropped the sampling window when the *source*
  changed but not when the *core count* did, so `avail` kept using the old
  value and every subsequent reading was mis-scaled — persistently, long after
  the backwards-accumulator guard stopped firing. The window is now dropped on
  either change. Not reachable on CV610, whose second core is not hotpluggable,
  but the sampler should not depend on that.

## [0.67.2] - 2026-08-22

Debug-OSD `cpu NN%` reads honestly on CV610. No wire format, config schema,
frame-SHM layout or HTTP contract changes.

- **CV610 debug-OSD CPU% swung 1.9–95.3% for a box steady at ~33%.** The shared
  `OsdCpuSampler` in `src/debug_osd.c` derives busy from `/proc/stat` idle,
  which the CV610 5.10 kernel does not account honestly over short windows.
  Unlike Star6E — where the *total* is inconsistent and `idle` is the stable
  anchor — CV610's total is exactly right (`Δfields + Δidle == avail` in every
  window), so idle-derived and field-derived are algebraically one number and
  the v0.51.0 fix is a no-op there. It is the idle/busy *split* that
  oscillates: the aggregate line is purely tick-sampled while per-task times
  are rescaled to the ns-accurate `sum_exec_runtime`, and venc's 100 fps beats
  against HZ=100. Now prefers `/proc/schedstat` `rq_cpu_time` (field 8 —
  nanoseconds of real task execution off the scheduler clock): measured
  30.1–38.3%, stdev 2.02, against an independent all-task truth of 32.3.
- **Star6E and Maruko are unaffected — structurally.** The schedstat branch is
  compile-time `PLATFORM_CV610` only, not a runtime probe. Star6E's 4.9 kernel
  has no `CONFIG_SCHEDSTATS` and Maruko ships without it, so a probe would have
  fallen back anyway; the gate makes it independent of a kernel `.config`.
  That matters because schedstat would be *wrong* on Star6E: idle-derived reads
  60.5 there against an all-task sum of 52.7, a 7.7-point gap of real
  IRQ/softirq work charged to no task, which `rq_cpu_time` does not count.
  Verified a no-op on .232.
- **A backwards accumulator now means "no measurement".**
  `di = idle_all - cs->ring[oldest].idle` was an unguarded unsigned
  subtraction that wraps to a huge value on any non-monotonic counter. Simply
  clamping the delta to zero is not the fix — it fabricates an opposite
  extreme on each path: 0% on the schedstat path, and 100% on the `/proc/stat`
  path (`avail - 0`), which reads as an alarm on the overlay. The sampler now
  skips the update and keeps the last good value. Reachable via CPU hotplug,
  since schedstat rows cover only online CPUs.
- **The source is re-decided each sample rather than latched.** A latch taken
  on a transient `fopen` failure at the first encoded frame would have pinned
  the readout to the wrong source for the process lifetime; and a latched
  source that could not fall back retried per *frame* (100 Hz) instead of per
  500 ms window, because the cadence gate is only armed on a successful
  sample. If the source changes, the ring is dropped rather than differencing
  nanoseconds against jiffies.
- `documentation/STAR6E_CPU_PROFILE.md` gains a CV610 section and its stale
  render-site line references are corrected.

## [0.67.1] - 2026-08-22

Pre-upstream review pass over 0.65.3..0.67.0. Three defects introduced by that
range, one long-standing sensor-register error, and the documentation the
range changed but did not update. No wire format, config schema or frame-SHM
layout changes.

- **Star6E dual-VENC crashed at startup.** `star6e_pipeline_start_venc()`
  accepts `vcfg == NULL` — `star6e_pipeline_start_dual()` passes it for ch1 —
  but 0.66.0's slice block dereferenced it unguarded. Any craft with
  `record.mode=dual`/`dual-stream` segfaulted during startup controls,
  independent of `sliceCount`, because the fault is the *read* of
  `slice_count`. `record.mode` is gated on mode alone, not `record.enabled`.
  Host tests could not see it: `star6e_pipeline.c` is not in the test library.
- `MI_VENC_ParamH265SliceSplit_t` was passed to the vendor with its padding
  uninitialised; both sibling structs in the same function are memset.
- A `libmi_venc.so` without the optional `MI_VENC_SetH265SliceSplit` export
  failed startup with a message that read as a driver rejection. The failure
  stays fatal — delivering one slice while the API advertises N would lie to
  the receiver's spatial concealment — but now names the missing symbol.
- **The CV610 partial-load rollback could still wedge the SoC.** `stop_modules()`
  refuses to unload while a consumer holds an MPP device, and its comment
  claimed every unload path was covered; `cleanup_partial()`, armed as an EXIT
  trap across the whole load sequence, was not. A consumer can claim a device
  created earlier in that same sequence — `waybeam_hub` rebinds `/dev/rgn`
  within ~1 s of `open_rgn` appearing — so a later insmod failure unloaded it
  underneath a live holder. It now carries the same guard and leaves the
  modules loaded, which is recoverable; a wedged SoC is not.
- **The IMX662 analog gain register was never written.** Sony's register list
  puts `GAIN[10:0]` at `3070h`/`3071h` and jumps straight from `3069h` to
  `3070h` — `306Ch`/`306Dh` are not registers. Every AE gain update went to a
  dead address, so analog gain never actuated, all gain was ISP digital, and
  the HCG conversion-gain compensation never landed, making each HCG engage an
  uncompensated ~5.8x brightness step. The vendor's own reference driver
  carries the same pair. **This changes low-light behaviour materially and the
  gain/ISO tuning in 0.65.6 was measured against the dead register — the
  low-light path needs re-verification on hardware.**
- The HCG band's compile-time guard could not fire in the dangerous direction:
  all three operands are `u`-suffixed, so lowering `HCG_OFF_REG` below
  `HCG_GAIN_OFFSET` wrapped and passed clean. Now evaluated signed, with a
  second assert rejecting an inverted ON/OFF pair. Both mutation-tested.
- `make stage SOC_BUILD=cv610` returned 0 when the sensor plugin failed to
  build, because the recipe chains with `;` and the last `cp` succeeded. CI
  caught it only via its explicit `test -f` on the `.so`.
- `data.routes.iq` claimed `/api/v1/iq` *and* `/api/v1/iq/set` were serviced
  while testing only `query_iq_info`; `/iq/set` gates on `apply_iq_param`. Now
  requires both. Every shipped backend registers both, so no response changes.
  The test that asserted the old pairing gained the missing arm.
- Docs: `docs/CV610_BACKEND.md` said the init script unloads the MPP stack on
  stop and guards unloading itself — both untrue since 0.65.8/0.65.10 — and
  recorded none of the operator rules those releases established. Its audio
  timestamp note still claimed 10 ms against `CV610_AUDIO_POINT_NUM` 960.
  `/api/v1/intra/status` and `/api/v1/resilience/status` are documented for the
  first time. An orphaned code fence in the capabilities section rendered ~25
  lines of prose as a code block.

## [0.67.0] - 2026-08-22

Ports the shared resilience contract and whole-access-unit H.265 slicing to
CV610. All resilience presets now drive the HiSilicon row-intra-refresh and
base/enhance reference controls with strict setter/getter verification;
`video0.sliceCount` drives 32-pixel LCU-row splitting with early slice output
kept disabled. The existing config schema and frame-SHM v1 wire layout are
unchanged.

- CV610 consumes per-frame vendor `ref_type`, marks non-reference enhancement
  frames in frame-SHM, rewrites their copied TRAIL_R NAL headers to TRAIL_N,
  publishes GDR cycle position/length, and rate-limits recovery IDRs when a
  full output ring drops a reference-chain frame.
- `/api/v1/capabilities`, intra status, and resilience status now report the
  implemented CV610 controls. Startup fails on an encoder readback mismatch
  instead of silently claiming resilience or slice geometry.
- Confirmed on the IMX662 CV610 bench at 1080p30/60/100. Slice requests 1, 3,
  4, 6, 9, 12 and 17 delivered exact VCL-NAL counts; shared ref presets matched
  vendor cadence; frame-SHM metadata/integrity passed; and an IDR-started
  380-frame 1080p60 stream decoded cleanly under FFmpeg `-xerror`.
- Maruko now binds the device-aware H.265 slice Set/Get ABI and applies it in
  the required CreateChn-before-StartRecvPic window, including stab-fill.
  Explicit multi-slice requests fail startup if the ABI cannot be applied or
  read back; `sliceCount=1` remains compatible with older libraries.
- Confirmed on SSC378QE/Maruko at 1280x720@30 with `rally`: requests 1, 4 and
  12 delivered exactly, while 17 and 32 quantized to the picture maximum of
  12. All sampled access units retained their complete VCL census and a
  cold-start 284-frame, 12-slice capture decoded cleanly with FFmpeg.
- Fixed a pre-existing Maruko frame-SHM startup guard that rejected a valid
  frame ring because it checked only socket and packet-ring transports.
- The 0.66 eight-slice ceiling and CV610/Maruko deferrals are superseded:
  `packetInfo[8]` is per pack, while every output path walks all packs in the
  access unit. The shared request range is now 1..32 and device geometry sets
  the delivered ceiling.
- The frame-SHM consumer test can optionally dump an IDR-started Annex-B stream
  for offline decoder validation.

## [0.66.0] - 2026-08-19

Adds H.265 multi-slice output on Star6E: `video0.sliceCount` (1..8, default
1 = today's single-slice stream, restart-class) binds the vendor
`MI_VENC_SetH265SliceSplit` — exported by the shipped `libmi_venc.so` all
along, never wired — and asks for `ceil(rows/sliceCount)` CTU rows per slice
between CreateChn and StartRecvPic, the same SDK window as refPred.

The consumer is waybeam-link's new §6.3b spatial concealment: with several
independent slices per picture, RF loss past the FEC budget costs a frozen
region instead of the whole frame. The knob defaults off so nothing changes
until a craft opts in.

- Frame-SHM publication is untouched: still one access unit per slot, now
  with N slice NALs inside it. RTP/recorder walkers already iterate
  `packetInfo[]` and stay correct.

- The 8-entry `packetInfo[]` clamp is the known ceiling — the field is
  range-limited to 8, and the frame-ring walker now WARNs (once) if the SDK
  ever reports more NALs in a pack than the table holds, instead of silently
  shipping a truncated frame. Whether the SDK emits one pack per slice or
  packs slices into one `packetInfo[]` still needs the on-device readout;
  until then treat sliceCount 4 as the validated envelope (offline: 4-slice
  1080p streams repaired and decoded clean by ffmpeg/libde265/HM-18.0).

- CV610: not exposed (allowlist unchanged); the SDK headers are external and
  its slice-split contract is unverified. Maruko: `libmi_venc.so` exports the
  symbol, but only the Star6E pipeline applies it for now.

## [0.65.10] - 2026-08-17

Removes both CV610 paths that unloaded MPP modules on a running craft: the
`S95waybeam reload` action, and `start()`'s module rollback on a failed daemon
start. `reload` was dead code built on a premise the hardware disproved in the
release that added it; the rollback was reachable on the post-crash start this
release prescribes as the recovery.

- **Nothing in production called it** — the hub uses `restart`
  (`mod_venc.c:687`), venc never did, and the crash test does not. Only the
  suite's own T5 exercised it, to test the action itself.

- **Its premise was already falsified.** Its comment claimed VB is left held by a
  dead owner after a crash and reload is "the way out". 0.65.8's own notes say
  the opposite — *"ISP, not VB, is what a crash leaves behind… VB is released"* —
  and the suite prints `VB was released when the process died` on every run. What
  a crash actually leaves (ISP, and since 0.65.9 the AI device claim) venc
  pre-cleans itself, so a hard-killed venc recovers from a plain `start`, video
  and audio both.

- **Unloading is not free.** A hard kill poisons module reloading for the rest of
  the boot (`no sys ko!` → `load vpp.ko ...FAILURE!`), so a reload issued
  afterwards leaves the craft with no modules at all — and on a video-only craft
  it resets the SoC outright (measured). The underlying defect is in the vendor
  modules and is unchanged.

  `reload` was not the only unload on that path, which an adversarial review
  caught before this merged: `start()` rolled back with `"$LOADER" stop` on a
  failed daemon start, and since 0.65.8 made the loader's start idempotent
  ("MPP modules already loaded" → success) that rollback tore down a set the
  invocation had not created — on exactly the post-crash start this release
  prescribes as the recovery. That rollback is removed too. Leaving the modules
  up after a failed start costs nothing, because the next start re-inits against
  them.

- The loader's audio-mismatch hint pointed at `S95waybeam reload`; it now says
  reboot, which is what actually applies a `CV610_AUDIO` or sensor-profile
  change. `load-cv610-online restart` remains for a dirty or partial module set,
  but run **bare** it reloads with the loader's defaults (video-only, imx662
  clock) rather than the craft's profile — a caveat that used to live inside
  `reload()` and is now recorded where it survives it. `load-cv610-online stop`
  followed by `S95waybeam start` is the safer form; that passes the profile.

- The suite loses T5 (it tested `reload`) and, with it, the contaminated-boot
  precondition T5 needed. The suite no longer *performs* an unload: T1 still
  attempts one, but only to assert the loader refuses it while the hub holds
  `/dev/rgn`. It also no longer stops the hub anywhere, so T6 now always runs
  with a holder present.

## [0.65.9] - 2026-08-17

CV610 audio now survives an abnormal venc exit. Investigating the "aenc MMB
leak" reported in 0.65.8 showed it is **not a leak** — and that it was masking a
real regression.

- **The MMB LEAK lines are the kernel reclaiming, not losing.** Measured on
  `.181`: a hard kill prints `MMB LEAK(pid=N) 'aenc(0)_strm'`/`'aenc(0)_cir'`,
  and the two blocks are gone from `/proc/umap/media-mem` immediately after.
  Across **five** kill cycles the pool returned to its exact baseline —
  `34 blocks / 38332 KB` before and after. Nothing accumulates.

- **What actually broke: audio, silently.** MPP objects are kernel state, so a
  killed venc leaves the AI device claimed by the dead pid. Every AI call from
  the successor is then refused with `OT_ERR_AI_NOT_PERM` (`0xa015800d`) —
  including `ss_mpi_ai_disable()`, so the successor could not even clean up.
  `ss_mpi_ai_set_pub_attr()` failed, audio never started, and venc reported
  *"needs CV610_AUDIO=1 at module load"* — which is not what happened. Video
  streamed normally, so nothing looked wrong. Deterministic, 2 of 2.

  This was masked until 0.65.8: the module reload that release removed also
  wiped the AI claim. So the hard-kill path regressed there, and `reload` is not
  a way out — a hard kill also poisons module reloading for the rest of the boot.

- **Fix: cycle the audio module before bring-up.** `cv610_audio_start()` now
  calls `ss_mpi_audio_exit()` + `ss_mpi_audio_init()`, then `ss_mpi_ai_disable()`,
  before configuring anything. The module cycle is what clears the dead owner's
  claim — it is precisely why a *second* graceful restart used to recover audio
  where the first did not, and this folds that cycle into the first start. Same
  shape as `sys_setup()`'s `isp_exit`/`sys_exit`/`vb_exit` pre-clean.

  Releasing the objects one by one does **not** work and is not attempted: the
  SDK tracks `sys_unbind`, `aenc_destroy_chn`, `opus_deinit` and
  `ai_disable_chn` process-locally, so a successor cannot release a dead
  predecessor's copies. Only `ss_mpi_ai_disable()` reaches kernel state, and it
  is the only one whose return code ever differed between the broken and healthy
  cases. `opus_deinit` was additionally printing `illegal handle(-1)!` to stderr
  on every clean start.

  Verified on `.181`: kill a venc with healthy audio, start, audio back — **2 of
  2**, `aenc` blocks 0 → 2, `ai_disable` `0xa015800d` → `0x0`. It also recovered
  a craft already stuck in the broken state with no reboot, and a graceful
  restart is unchanged.

- **The audio warning no longer guesses a cause.** It asserted *"needs
  CV610_AUDIO=1 at module load"* for every failure — the misreport above. The
  module hint moved to `ss_mpi_audio_init()`, which is where an absent module set
  actually lands (it opens `/dev/ab`, created by `open_aio`, which the loader
  stages only under `CV610_AUDIO=1` — a switch independent of `audio.enabled`).
  The `/dev/acodec` open is *not* that path.

- The verify suite's T6 asserted *video* streaming, which is why this went
  unnoticed — it passed with audio dead. It now asserts the aenc block count and
  the absence of an audio-start failure, and skips explicitly (not silently) on
  a video-only craft. T5 refuses to run on a boot where a hard kill already
  happened, instead of failing for a reason that predates the run.

## [0.65.8] - 2026-08-17

CV610 venc restarts no longer reload the MPP kernel modules — the same model
star6e and maruko have always used, where the init script never touches
`insmod`/`rmmod` at all.

- **The reload was bring-up scaffolding.** Both cv610 init scripts have been
  unchanged since the initial backend commit. `mpp_cleanup()` is already a
  complete teardown and, critically, calls `ss_mpi_vb_exit()` *from the process
  that created VB* — the only process permitted to. So after a graceful stop
  there is nothing for a module reload to clean up, and the next start can
  re-init the graph against the modules already loaded.

- **What the reload was actually protecting.** A venc that dies without running
  `mpp_cleanup()` leaves VB held by a dead owner; `vb_set_cfg` then returns
  BUSY. That was tolerated blindly, which is only harmless while the modules
  are reloaded every time. Without the reload it would mean running on the
  previous mode's pool sizes.

  `sys_setup()` now reads the live config back with `ss_mpi_vb_get_cfg()` and
  compares the pools it was about to request. Identical layout is adopted and
  says so; a different layout is a hard failure naming the way out
  (`load-cv610-online restart`) instead of silently handing out wrong-sized
  blocks that surface later as corruption.

- `load-cv610-online start` is idempotent when the full set is up (`open_user`
  is the last module loaded, so its presence means a completed sequence); a
  *partial* set is still refused as dirty. `S95waybeam stop` leaves the modules
  loaded, and a new `S95waybeam reload` is the recovery path after a crash —
  it unloads, so the holder guard applies and the hub must be stopped first.

- **ISP, not VB, is what a crash leaves behind.** Measured on `.181`: after
  `kill -9`, VB *is* released (set_cfg succeeds) but ISP[0] stays inited and the
  next run dies at `ss_mpi_isp_mem_init` with `0xa01c800c` "already inited".
  `sys_setup()` now pre-cleans with `ss_mpi_isp_exit(VI_PIPE)` alongside the
  existing `sys_exit`/`vb_exit`, before anything of ours is registered, so a
  hard kill recovers without a module reload.

- `S95waybeam reload` passes `CV610_SENSOR_PROFILE`/`CV610_AUDIO` through to the
  loader. A bare `$LOADER restart` reloads with the loader's own defaults —
  video-only, imx662 clock — and `start()` then correctly refuses the audio
  mismatch, leaving the craft with no daemon at all.

- Device verification on `.181`, `scripts/cv610_reload_free_verify.sh`:
  **23 passed, 0 failed**. T3 restarts across a real mode change and asserts the
  *encoded geometry* moved (1920x1080 → 640x360) from venc's own VPSS line,
  because asserting the config key instead let an earlier revision report a mode
  change that never happened. T2 asserts the hub's OSD survived, from the
  compositor's VGS job counters in `/proc/umap/rgn` rather than the hub's own
  perf line — the hub counts the publish it made, which is the stimulus; those
  counters count the composite that consumed it. Verified to fail on a region
  that is not there. T3 asserts it too, across the mode change, because the hub
  rebinds at its old window size and only its own source poll re-anchors it: an
  oversized region sits on the smaller channel for a few seconds. Measured
  benign — 30 s at 640x360 with `job_fail` 0 and venc streaming.

Two things the hardware showed that are **not** fixed here:

- **A SIGKILL leaks two `aenc` MMB blocks** (`aenc(0)_strm`, `aenc(0)_cir`,
  16 KB each) and after that the next module unload/reload fails at sys.ko init
  — `no sys ko!` then `load vpp.ko ...FAILURE!` — for the rest of the boot, so
  even a fresh load from zero modules fails. Graceful stops leak nothing. This
  makes `reload` the wrong recovery after a hard kill; reboot is. It is also an
  independent argument for this change: the fewer module reload cycles, the
  better, and it plausibly underlies the old "reboot instead" folklore.

- **The hub's RGN OSD does not survive a venc restart.** With `open_rgn` no
  longer unloaded the hub keeps its `/dev/rgn` handle, but the region does not
  survive our teardown: after a restart `/proc/umap/rgn` lists **no regions at
  all** — `mpp_cleanup()`'s `ss_mpi_sys_exit()` takes the whole table with it,
  because MPP objects are kernel state shared across processes. The hub keeps
  painting into a handle that no longer exists: `osd_render` goes from
  `40 rgn … drops 0` to `0 rgn … drops 42`, silently. A hub restart restores it.
  Fixed on the hub side in waybeam-hub#213 (the region is recreated and rebound
  after five consecutive failed publishes), which is what T2 now asserts.

## [0.65.7] - 2026-08-17

CV610 SoC wedge on `stop`. On the `.181` bench, `S95waybeam stop` while
`waybeam_hub` was running killed the SoC outright — no network, ARP
incomplete, physical power cycle required. It had been misfiled as an
intermittent "reboot sometimes hangs" for weeks.

- **Root cause: the MPP modules were unloaded under a live consumer.**
  `stop()` waits only for `waybeam`/`waybeam-resp`/`waybeam-wd` and then calls
  `load-cv610-online stop`, which rmmods the whole stack. `waybeam_hub` holds
  `/dev/rgn` for the CV610 RGN OSD and `/dev/mmz_userdev`, and the MPP drivers
  do not set `.owner` on their file operations — `/proc/modules` reports
  `open_rgn used=0` while the hub has `/dev/rgn` open. `rmmod` therefore
  succeeds and frees the file operations out from under a live consumer.

  The script already carried the right invariant in a comment — *"Never unload
  the MPP stack while any of them still owns the graph"* — it just never
  counted the hub as an owner.

- **Measured, with controls.** 10/10 monitored `reboot` cycles clean (ping gap
  12.6–15.8 s, back 18–20.5 s). 20/20 venc stop/start cycles clean with the hub
  stopped, 2100 frames and 0 output drops afterwards. First stop/start with the
  hub running: dead. Then 5/5 clean again once the hub was released first.

  Reboot was never safe for the reason everyone assumed. `rcK` iterates
  `ls -r /etc/init.d/S??*`, so `S97waybeam-hub` stops *before* `S95waybeam`.
  That accident is the entire difference, and it retro-explains the standing
  "never stop/start venc on cv610, reboot instead" rule.

- **Fix.** The guard lives in `load-cv610-online stop_modules()`, next to the
  `rmmod` it protects, so it covers every path that unloads — `S95waybeam
  stop`, the start-failure rollback that also calls `$LOADER stop`, and a
  hand-run `load-cv610-online stop`. It scans `/proc/*/fd` and refuses, naming
  the holders.

  `S95waybeam stop` additionally asks up front, via `load-cv610-online holders`
  ignoring its own pids, so it can refuse *before* killing anything: refusing
  after the kill would leave venc dead with the modules still loaded, which
  `load-cv610-online start` then rejects as dirty state.

  `rcK` and `reboot` are unaffected because the hub is already gone by then; a
  manual `S95waybeam stop`/`restart` on a live craft now fails with a message
  instead of wedging the SoC. `/api/v1/restart` is untouched — it re-execs the
  daemon without unloading modules.

## [0.65.6] - 2026-08-16

Low-light bitrate. On the `.181` bench with the room lights off, a 9.26 Mbps
CBR target produced 29-65 Mbps. In good light the same build holds target
(measured 9.98 vs 9.26), so this was specific to high sensor gain.

- **The borrowed sharpen table was the cause — 88% of it.** Measured by
  A/B with interleaved controls: turning sharpen off dropped the stream
  65.4 -> 7.8 Mbps, *below* the target. Pinning sharpen strength to a constant
  gave 0 -> 11.8, 40 -> 36.5, 80 -> 62.7, 150 -> 91.5 Mbps. Nothing else came
  close: saturation -7.3%, CSC satu -4.5%, CA +1.7%, LDCI +2.2% (inside drift,
  no effect). Disabling bayer NR *raised* the bitrate 15.7%, so NR was already
  earning its keep and is untouched.

  Sharpening is a high-pass filter, so at high gain it amplifies precisely the
  noise that inter prediction cannot code. `g_cmos_yuv_sharpen` runs in AUTO,
  and its sc450ai-derived `[32 gain][16 ISO]` tables *rise* along the ISO axis
  — `texture_strength` to 310 where the low-ISO value is 216, `edge_strength`
  195 -> 500. They sharpen harder the darker it gets. Defensible for sc450ai's
  noise model and NR softening; backwards on IMX662.

  Both tables are now tapered along ISO only, to 0 by bucket 9, with the rule
  the original breaks: sharpening never increases with gain. Verified by four
  invariants — ISO buckets 0-3 byte-identical, ISO 9-15 zero, monotonic
  non-increasing from bucket 3, and no value anywhere raised.

- **QP bounds are wired on CV610** (`apply_qp_bounds`, `video0.minQp/maxQp`,
  live and at startup). The config fields, mutability and WebUI metadata
  already existed; only the backend callback was missing.

  This is **not** the low-light fix, and the measurement says so: the driver
  default is `min=10 max=51`, already the H.265 maximum. The rate controller
  always had full authority and still emitted 29-65 Mbps, so QP saturation was
  never the cause. At 100 fps even maximum-QP frames of a noise-dominated
  image exceed 10 Mbps. The knob is worth having to *cap* quality or raise the
  floor; it cannot rescue a scene the encoder is already coding as coarsely as
  the standard allows.

- **AE is exposed as an `exposure` IQ group** — the gain ceilings and exposure
  window that decide how much noise is created in the first place. Before this
  release the bench ran `a_gain_max` 407654 (398x, matching
  `IMX662_AGAIN_MAX`), `ispd_gain_max` 32768 (32x) and `sys_gain_max` 13044928
  — **12739x total, about 82 dB**, which is why a dark room ended up as
  amplified noise. Those ceilings are now readable and settable at runtime
  instead of being compile-time constants, and the digital one is lowered
  below.

- **Saturation tapers harder above daylight.** Chroma noise is the ugliest
  high-gain artifact and colour is least trustworthy exactly where gain is
  highest; dropping chroma measured -7.3%. The first four buckets are
  unchanged, the top drops from 90 to 32.

- **ISP digital gain is capped 32x -> 4x.** It is the worst gain in the chain:
  it amplifies noise and quantisation equally and buys no SNR, where analog
  gain and HCG act ahead of the ADC. The shipped total ceiling moves from
  12739x (~82 dB) to **398 x 4 = 1592x (64.0 dB)**, inside the range an
  operator preferred on the bench, and AE now exhausts analog before reaching
  for digital. Tunable live at
  `/api/v1/iq/set?exposure.auto.ispd_gain_max=<n>` (1024 = 1x), because
  darker-but-cleaner is an operator trade rather than a constant.
  `IMX662_AGAIN_MAX`'s comment claimed ~72 dB while the value gives 52; the
  comment is corrected and the value deliberately left alone.

- **IMX662 HCG conversion gain is selected at high analog gain.** `FDG_SEL0`
  (0x3030) was defined and never written, so the sensor sat at its 0h reset —
  low conversion gain — and every dark frame was amplified through the noisier
  path. It is now a ninth fast-update register, chosen per frame from the gain
  code with hysteresis.

  HCG is **not** brightness-neutral: the GAIN register is PGA gain applied
  after the conversion gain, so the two multiply. The datasheet gives the ratio
  (Rcg typ 5.8, corroborated by G sensitivity HCG 18383 vs LCG 3166
  Digit/lx/s), which is 15.3 dB — so the written code has the offset
  subtracted while HCG is on, and the thresholds sit where the compensated code
  still clears Sony's 34-code floor (on at 91, off below 85). Without that
  there is no fixed point between 12 dB and 25.5 dB: AE brightens 5.8x, slams
  the code down past the floor, HCG drops out, and it limit-cycles — a band
  that neither daylight nor a dark room enters, which is why an uncompensated
  first cut measured clean in both.

  Sony's 34-code floor is a saturation constraint, not the conversion gain:
  Vsat is 3895 digits in LCG against 1204 in HCG, a ratio of 3.235, and 34
  codes x 0.3 dB = 10.2 dB = 3.236x — the floor exists so HCG's clip level
  refills the ADC to LCG's.

- **`nr.md_en` and `nr.coring_ratio` are exposed.** The intended fix here was a
  re-fit of `g_cmos_noise_calibration`, which is sc450ai's photon-transfer
  curve — 16 doubles measured from different silicon. That is a lab
  calibration, not a code change, and inventing coefficients would be strictly
  worse than keeping a real fit while looking identical in review. The knobs
  that fit drives are exposed instead, so the borrowed model can be corrected
  by eye until someone measures the real one. `md_en` reads back 1 — CV610's
  motion-detect temporal NR was already on.

The IQ field table is now 64-bit internally: the AE gain and exposure fields
are `td_u32`, and a driver default of 0xFFFFFFFF would have read back as -1.

`/api/v1/capabilities` gains `data.routes` and `contract_version` moves to
`0.18.4`; the CV610 IQ response shape is documented in
`documentation/HTTP_API_CONTRACT.md`.

A pre-merge adversarial review pass also corrected this release's own record
and three defects in it: the WebUI IQ tab no longer starts hidden (that made
visibility depend on `init()` completing, so any failed init fetch latched the
tab off permanently on star6e and maruko); a chip click no longer prefills the
actuator (on star6e's legacy path even an identical value forces `enable=1`
and `op_type=manual`); the per-set full-ISP re-query is gone; the shared query
parser rejects an over-long value instead of truncating it mid-token; and the
QP-bounds path leaves the I-frame FLOOR alone so `video0.qp_delta` still
biases I-frames. The 0.65.5 entry above was revised where this stack
invalidated it.

Verified on `.181`: the sharpen taper was confirmed by the operator on the
live stream (dark room holds target) with daylight unchanged at 10.09 Mbps
against 9.26 and the encoder nominal at 100.20 fps. The gain-chain and HCG
changes landed after that arm and were re-verified separately — see the
per-commit evidence — and `make verify` is green with lint clean on all three
backends.

## [0.65.5] - 2026-08-16

0.65.4 seeded the CV610 ISP well but froze every value into the sensor plugin,
which the ISP reads once at pipe start — correcting one meant a cross-compile,
a plugin deploy and a reboot. This makes those blocks live knobs. No contract
change; `/api/v1/iq` and `/api/v1/iq/set` are existing routes that answered
`501` on this backend only because it supplied no callbacks.

- **`cv610_iq.c` exposes 11 ISP groups, 59 fields.** saturation, color_tone,
  csc, ccm, wb, sharpen, nr, drc, ldci, dehaze and ca, wired into
  `g_cv610_apply_callbacks`. Unlike `star6e_iq.c` there is no `dlopen` and no
  hand-computed struct offsets: the backend links `ss_mpi` directly, so every
  field is an `offsetof()` into the SDK's own type and every range is the
  header's documented one. Values are read back from the ISP rather than
  cached, and a value outside its declared range is REJECTED rather than
  clamped, so the value echoed by `/api/v1/iq/set` is the value applied.
- **Writing a field selects the half of the block it lives in.** A value under
  `manual_attr` is inert while the block runs its auto curve and vice versa,
  and an ignored write is indistinguishable from a broken setter — so the
  write selects that `op_type`. The schema reports each field's `domain`
  (`direct` | `manual` | `auto`) and the WebUI marks both kinds on the chip.
- **DRC and dehaze are registered but bypassed**, measured through the new
  surface: `drc.enable` and `dehaze.enable` both read 0, and
  `ss_mpi_isp_get_module_ctrl()` confirms the hardware bypasses both. Toggling
  `drc.enable` clears and restores that bypass bit, so the two agree. 0.65.4's
  note that the algorithm blocks were "enabled" over-stated it for these two;
  that entry is corrected. Their sc450ai-derived tables ship `enable = 0`, so
  DRC has never contributed to a CV610 image and remains unevaluated.
- **The WebUI IQ tab is capability-driven.** It was hidden by a hardcoded
  `backendName === 'cv610'` test; visibility now comes from
  `/api/v1/capabilities`' `data.routes.iq`, and the knob list is rendered from
  the `_schema` the backend describes itself with, so the field table stays in
  one place instead of being copied into the page. Backends with no schema keep
  the existing hardcoded list.

Verified on the `.181` bench, cold-booted onto a build of this change
(`/proc/PID/exe` confirmed not `(deleted)`; the binary was rebuilt again for
0.65.6, so the hash is not quoted here): all 11 groups
return `ret=0`; a mutation sweep wrote and read back one field per group with
11/11 agreeing and the device diffing clean against its boot state afterwards;
saturation 60 → 220 → 60 moved mean chroma 4.84 → 22.95 → 4.81, the repeat of
the first arm landing within 0.6% so the change is the knob and not the scene;
arrays of the wrong length in either direction and unknown names are
rejected; encoder rate stayed nominal at 100.10 fps with `pressureDrops`
frozen at 0.

`/api/v1/capabilities` advertises `routes.iq_import:false` because
`handle_iq_import()` is compiled only for Star6E and Maruko; the WebUI hides
the control rather than offer a button that 501s. (An earlier `_caps` block in
the IQ payload was replaced by this before release — one fact, one place.)

## [0.65.4] - 2026-08-16

The CV610 IMX662 plugin was a scaffold in two places its comments admitted to,
and both were costing image quality. It wrote 26 of the sensor's 131 power-on
registers, and it handed the ISP a zeroed `ot_isp_cmos_default`, which switches
every algorithm block off. No HTTP behaviour changes; contract stays `0.18.3`.

- **The power-on register sequence is written.** `imx662_sensor_ctl.c` carried a
  TODO naming the range it was missing, `0x301C..0x4549`. Sony's sequence for
  that range is 131 entries — the reserved analog/ADC trim, the MIPI TX timing,
  the readout window and the HDR context linear mode needs parked — and it now
  lives in `imx662_cfg.h`. The table runs first, in standby, so the per-mode
  writes that follow win where the two overlap; that ordering is load-bearing
  for `0x3A50/51/52`, which keys off ADBIT rather than output depth, so the
  table holds the 12-bit form and the RAW10 modes overwrite it. Measured on
  hardware: 94 entries differ from the module's power-on state, the
  readout-window group already matched bit for bit, and a RAW10-vs-RAW12 dump
  of the whole range differs in only 8 registers — none of them in the reserved
  blocks, which is what made writing the table wholesale safe.
- **The ISP algorithm blocks are enabled.** `cmos_get_isp_default()` memset the
  struct and returned, leaving demosaic, gamma, CLUT, LDCI, CAC,
  anti-false-colour, dehaze, CA, sharpen, DRC and bayer NR all off. That, not
  the sensor, was the flat and hazy image. Tables are ported from the in-tree
  `smart_sc450ai` driver, which targets this same ISP silicon. The blocks that
  describe ISP behaviour transfer on principle; the noise-fitted ones are
  borrowed from a 4 MP sensor and are kept on a hardware A/B rather than a
  calibration. Two of them are registered but not switched on: `g_cmos_drc`
  and `g_cmos_dehaze` carry `enable = 0`, inherited from sc450ai's linear-mode
  defaults, so the ISP bypasses both at runtime. See 0.65.5, which measured
  that on hardware.
- **Saturation is configured at all.** `cmos_get_awb_default()` left
  `agc_tbl.valid` at 0 with every `saturation[]` entry 0, so the ISP was never
  told what saturation to run — the washed-out colour. The shipped curve is
  scaled up at the low-ISO end from sc450ai's and keeps its taper at high gain,
  where chroma noise scales. Measured +41% mean chroma with sharpness flat.
- **`sns_mode` is populated.** `sns_id` was 0, disagreeing with
  `cv610_pipeline.c`'s `IMX662_SNS_ID` — which that file already documents as
  having to match, and which would misbind a future PQ `.bin`, since those are
  chip- and sensor-locked. `sns_mode` was never set.
- **The sensor plugin Makefile generates header dependencies.** A bare
  `%.o: %.c` rule meant a header-only edit relinked stale objects and shipped a
  binary without the change, while `make` exited 0. Both the register sequence
  and every tuning table live in headers. Found when two plugins built from
  different saturation tables came out byte-identical.
- **`tools/cv610_i2c_dump.c`** reads sensor registers over `/dev/i2c` from a
  second process while the pipeline runs. `imx662_read_register()` is a stub —
  the ISP only ever writes — so nothing could previously confirm an init landed.

Verified on the Hi3516CV610 bench at 192.168.2.181, cold boot per arm:
1080p30 30.03 fps, 1080p60 60.03, 1080p90 90.03, 1080p100 100.06 — all nominal
and matching the pre-change baseline — with a 1222-register readback at zero
I2C errors and every reference entry matching silicon.

## [0.65.3] - 2026-08-16

CV610 sensor-mode selection reaches parity with SigmaStar, so one client can
render one set of sensor controls against all three backends. Contract
`0.18.2` → `0.18.3`.

- **`sensor.index` and `sensor.mode` select on CV610 now.** Both parsed and
  both sat in the config file, but the backend read neither and the capability
  set advertised neither, so `/api/v1/set` answered `409` for a field the
  daemon carried. `video0.fps` was the selector instead, through an
  exact-match lookup — a request for 45 fps was a hard config error where
  SigmaStar substitutes a mode and says so. The two knobs now mean what
  `sensor_select()` makes them mean: a forced `sensor.mode` wins over the
  requested rate and must exist, otherwise the rate is a target.
- **One case diverges from SigmaStar deliberately.** Its `sensor_mode_cost()`
  tie-break scores fps-excess at zero for every mode *below* the target, so a
  target above them all falls back to table order — the slowest mode. That
  rarely fires there because its modes carry min–max fps ranges; on CV610's
  four point-rate modes it would fire constantly, so a target above every mode
  clamps to the fastest instead. Verified on the bench: `video0.fps=120`
  selects 1080p100, and `45` selects 1080p60.
- **`/api/v1/modes` reports what is running, not what is configured.**
  `selected_pad` / `selected_mode` were recomputed from `video0.fps` on every
  request, so they described the configured mode even when bring-up had chosen
  another or failed outright. They are now published by the backend once the
  pipeline resolves, through the same `venc_api_set_sensor_info()` Star6E uses.
- **`video0.gopSize` is checked against the selected mode's rate.** The
  encoder has always derived its GOP length from that rate rather than from
  `video0.fps`; the two were identical only while selection was exact-match.
  A forced mode or a substituted target separates them, and the old check
  would have passed a `gopSize` that then exceeded the encoder's 65536-frame
  limit.
- A substituted rate is announced (`Requested 45 fps, using 60 fps (sensor
  mode 1: 1080p60 RAW12)`) and shows up over HTTP as `/api/v1/fps/live`
  disagreeing with `/api/v1/fps/config`. Neither measures anything — the
  sensor clock follows the mode automatically, but a client that needs the
  delivered rate must still measure it downstream.
- **`isp.keepAspect` works on CV610 now; a 4:3 `video0.size` was silently
  squashed.** The field sat in the config defaulting `true`, but CV610
  advertised no `isp.*` field and read none — a knob that did nothing, which
  is worse than one that is absent. `1440x1080` validated, then VPSS stretched
  the whole 1920x1080 capture into it. The backend now takes a centred crop
  first, through the same `pipeline_common_compute_precrop()` Star6E and
  Maruko call, so the rule cannot drift between the three: `1440x1080` uses a
  1440x1080 window at x=240 and scales 1:1. `isp.keepAspect=false` restores
  stretch-to-fit. Verified both directions on the bench.
- **The CV610 default bitrate drops 8000 → 2600 kbps.** waybeam-link is the
  single rate controller and actuates within seconds, so the config value is
  a **boot seed**, not an operating point — and what a seed has to survive is
  the worst rung, not the expected one. Landing at the §9.8 MCS0 no-feedback
  floor while offering 8 Mbps floods the air, and on this fleet an
  over-offered craft has already been measured demoting a *second* craft's
  link on the same channel within a minute. Pinning the seed low costs
  nothing once the controller takes over.
- **CV610 was skipping the shared config validation entirely.**
  `venc_api_validate_loaded_config()` dispatched to the CV610 backend
  validator *instead of* the shared `validate_field_cfg()` sweep, so sixteen
  shared rules never ran there — and CV610 re-implemented two of them
  (`video0.size`'s >=128 and multiple-of-8 gates) inside
  `cv610_mode_check_output()`, which is the drift risk that made the gap
  visible. The `/api/v1/set` path always ran the shared rules, so the same
  value was accepted from the config file at boot and rejected with `409`
  over HTTP. The sweep now runs first and the backend rules after.
- The CV610 Opus banner said `10.0 ms frames` for a whole release after
  `CV610_AUDIO_POINT_NUM` went 480 → 960. Behaviour was right at 20 ms; only
  the one line a reader could check the packet rate against was wrong. It is
  now derived from the constants rather than spelled out.

## [0.65.2] - 2026-08-15

CV610 gets a scaler, one mode table instead of five copies of it, and a
default that is not a leftover. Contract `0.18.1` → `0.18.2`.

- **`video0.size` means something now.** CV610 bound VI straight to VENC, and
  VI has no scaler — its channel emits the captured geometry and nothing
  else, so `video0.size` had exactly one legal value while being advertised
  as supported. The chip does have the scaler: VPSS, already loaded
  (`open_vpss.ko`, `/dev/vpss`) and already implied by the pipeline's
  `OT_VI_ONLINE_VPSS_OFFLINE` declaration, which the graph then bound past.
  The group is now in the graph — VI chn → VPSS grp → VPSS chn → VENC — with
  the channel doing the scaling. `video0.fps` selects the sensor mode and
  `video0.size` is the encoded size, the same split Star6E has. The group is
  created even at 1:1 so the shipping path and a scaled one share one graph
  and one teardown order. Two attribute values were found by probing, both
  otherwise `0xa0078007` (`OT_ERR_ILLEGAL_PARAM`) and neither documented:
  `chn_mode` must be `USER` (`AUTO` makes the channel follow the group's
  input size and rejects an explicit geometry) and `frame_rate` must be
  `-1/-1` (`0/0` is rejected identically). Measured on the bring-up board by
  decoding the live stream rather than trusting the daemon's own report:
  1920x1080 → 1920x1080 @ 100.04, 1280x720 → 1280x720 @ 100.03, 640x480 →
  640x480 @ 100. MMZ cost of the third VB pool at 720p is 5.6 MB; at 1:1 no
  third pool is created. **Not measured: the latency VPSS adds as an extra
  buffer stage.**
- **One sensor mode table instead of five copies of it.** `video0.size`,
  `video0.fps`, the MIPI RAW bit depth and the sensor input clock were each
  written down separately in the fps allowlist in `cv610_prepare()`, the
  size/fps checks in `cv610_validate_config()`, the hand-written JSON in
  `handle_modes()`, `raw_bit = fps > 60 ? 10 : 12`, and
  `hz = fps > 90 ? 27M : 37.125M`. They describe one thing — a sensor mode —
  and a fifth mode meant finding all five. They now live in
  `src/cv610_modes.c`; `cv610_mode_for_fps()` plus `cv610_mode_check_output()`
  are the only rejection rules for `video0.fps` and `video0.size`, and
  `GET /api/v1/modes` is generated from the same table, so what it lists is
  exactly what `/api/v1/set` accepts. The clock column is the one that had
  already cost a day: a mismatch is silent and the delivered rate simply
  scales by the clock ratio while every status surface reports nominal.
  Delivered rate re-measured after the refactor by RTP marker bit — 30.00,
  60.00, 89.98, 100.04 — which a wrong clock would show as 43.6 for the
  60 fps mode.
- **A half-set geometry is a typo, not a request.** `1920x0` previously
  completed into the default mode; only a fully unset `video0.size` now means
  "the mode's own size".
- **VPSS teardown is unconditional, or a partial setup wedges every
  restart.** `vpss_setup()` does four things and bails on the first failure,
  but the success flag was only raised after the final `sys_bind` — so any
  earlier failure left `vpss_teardown()` returning early with the group
  already created. The group is kernel state, not process state, and a
  respawn reloads no modules (`venc_respawn` re-execs, which is the path
  every restart-class `/api/v1/set` takes), so the next start would hit
  `create_grp` `EXIST` and never come up again without a manual module
  reload. Not hypothetical: `set_chn_attr` rejecting an explicit geometry
  under `OT_VPSS_CHN_MODE_AUTO` is exactly the failure hit while bringing
  this up. Teardown now matches the VI and ISP blocks beside it, which
  already destroy unconditionally and ignore the result.
- **A sensor clock that cannot be set now fails bring-up.** `0.65.1` made the
  clock follow the selected mode, but treated *every* failure to apply it as a
  warning — including the case where the knob is right there and rejects the
  write. That left the one outcome the change exists to prevent still
  reachable: a stream at `fps * actual/expected` while `/api/v1/modes` and
  `/api/v1/fps/live` both report nominal. The two failures are now answered
  differently. A `sys_config` with no `sns0_clk_hz` parameter predates it, and
  refusing to run on that module would strand every craft still carrying one —
  that stays a warning, naming the clock the mode needs. A parameter that is
  present and will not take the value means the line timing is known to be
  running against the wrong MCLK, so `mipi_setup()` aborts instead. Raised by
  automated review on the upstream PR; no measured path changes, because a
  board whose write succeeds is untouched and a board whose write fails was
  already producing a rate nothing reported.
- **The shipped default is 1280x720@100 at 8000 kbps**, was 1920x1080@60 at
  8192. The old value predates VPSS scaling existing on this backend. 720p100
  halves the encoded pixel count while doubling the frame rate, and both cut
  latency on the measured model (`0.9 x frame_period + 11 + 5.9 x
  megapixels`): ~38 ms at 1080p60 down to ~24 ms at 720p100. The bitrate goes
  to 8000 so the number means 8 Mbps rather than 8.192. Verified by booting
  the device on this file as `/etc/waybeam.json`, which is what a fresh craft
  does. The existing `cv610_sample` assertion only proved the file validates —
  it would pass just as happily on 1280x728 — so the shipped values are now
  pinned and asserted to name a real sensor mode. **Noted, not fixed:** the
  wire rate measures ~14.2 Mbps against the 8 Mbps target while RC is
  programmed with 8000, the same shape as the known Star6E high-fps CBR
  overshoot.

## [0.65.1] - 2026-08-14

CV610 advertises the control surface it actually has, and `mutability`
becomes a per-backend answer.

- **The CV610 capability set was under-reporting the backend.** Eight fields
  the slice genuinely reads were advertised unsupported, so `/api/v1/set`
  returned `501` for values `cv610_prepare()` / `cv610_output_start()` /
  `cv610_audio_start()` would have honoured on the next start: `video0.fps`,
  `video0.size`, `outgoing.enabled`, `outgoing.server`,
  `outgoing.connected_udp`, `outgoing.allow_unix_encoder_stall`,
  `audio.enabled`, `audio.mute`. The practical consequence was that a CV610
  craft could not be retargeted at all — `outgoing.server` was rejected, and
  the only way to change the stream destination was to edit
  `/etc/waybeam.json` on the device by hand.
- **`mutability` is now per backend** (`field_mut_for_backend()`), and may
  only be *downgraded* from the shared table — a backend can never widen a
  restart-class field to live. CV610 reads `video0.fps`, `outgoing.enabled`,
  `outgoing.server` and `audio.mute` once at start, so it reports them
  `restart_required` where Star6E and Maruko report `live`. Without this the
  widening above would have handed the dashboard four live controls that
  accept a value and silently do nothing — the failure mode the Maruko
  capability gates already exist to prevent. `/api/v1/live/set` rejects them
  on CV610 with `400` and still applies them on Star6E; verified both ways.
- **What stays unsupported is now unsupported for a stated reason.**
  `audio.sample_rate` / `channels` / `codec` / `volume` are fixed at
  48000 / 1 / opus / gain 8 in `src/cv610_audio.c`, and `video0.rc_mode` is
  fixed at H.265 CBR in `cv610_venc_start()`. The shipped defaults happen to
  match those constants, which is exactly why advertising them would be a
  lie rather than a convenience.
- **`/api/v1/audio/status` implemented on CV610.** It answered `501` while
  the Opus encoder was in fact streaming. Now reports the fixed 48 kHz mono
  configuration, the mute state, and live `frames` / `bytes` / `packets` /
  `drops` from the audio thread's stats lock.
- **Dashboard stops offering dead controls on CV610**: the Image Quality tab
  is hidden (`/api/v1/iq`, `/ae`, `/awb` all answer `501` there) and the
  recording start/stop buttons are disabled — nothing services a record
  request on CV610, so those routes already answered `501` through the
  existing `venc_api_set_record_http_control_supported()` opt-in.
- **The sensor clock now follows the selected mode, instead of a boot-time
  module argument.** `CV610_SENSOR_PROFILE` never named a sensor — the part
  is IMX662 in every configuration, and the loader artifact is the
  IMX662-patched `open_sys_config`. The string is a key into
  `parse_sensor_clock()`, a three-entry table that writes one CRG register:
  the IMX662 needs 37.125 MHz for 30/60/90 fps and 27 MHz for 100 fps (27 MHz
  fed while the sensor selects its 24 MHz INCK profile, register
  `0x3014 = 0x04`). One clock per boot therefore made exactly one mode
  correct, and the failure was silent: the rate scaled by the clock ratio,
  measured **43.6 fps** for a 60 fps mode on the 27 MHz clock
  (60 x 27/37.125), with every status surface still reporting 60.
  `mipi_setup()` now sets the clock from `video0.fps` during bring-up — after
  `ENABLE_SENSOR_CLOCK`, which rewrites the same register, and before
  `UNRESET_SENSOR` so the sensor leaves reset on its final clock. It writes a
  **frequency**, not a register value, to
  `/sys/module/open_sys_config/parameters/sns0_clk_hz`, a new writable
  parameter on the patched sys_config module
  (`0002-hi3516cv6xx-runtime-sensor-clock.patch` in the firmware tree); the
  CRG encoding stays in the kernel next to `parse_sensor_clock()`.
  **Verified with the boot profile left at `sc4336p`, so the daemon overrides
  it every time: 30 / 60 / 90 / 100 fps all deliver their nominal rate**
  (ratio 1.000-1.002, 100 fps repeated at both ends of the sweep), where
  before only the mode matching the boot profile was right. Against an older
  sys_config without the parameter the daemon warns, naming the clock the
  mode needs, and continues on the boot-time clock.
- **Debug OSD verified on CV610 hardware** for the first time
  (Hi3516CV610 demo board, 1080p100): `debug.showOsd=true` composites the
  CLUT4 RGN overlay into the encoded stream, with correct live values
  (`fps: 100`, `br: 16000k`, `enc: 1920x1080 h265`). It was already wired in
  `debug_osd.c` and advertised supported; it had simply never been switched
  on with eyes on the output.

## [0.65.0] - 2026-08-13

Initial HiSilicon CV610 + Sony IMX662 backend slice (#220).

- Added `SOC_BUILD=cv610` and a third `BackendOps` implementation using the
  OpenIPC ARMv7 musl toolchain, public OpenHisilicon headers, and external
  CV610 runtime libraries. SigmaStar's forced compatibility header is now
  scoped to the Star6E and Maruko builds.
- Ported the hardware-verified IMX662 MIPI -> VI -> ISP -> H.265 VENC graph
  into the common daemon lifecycle using VI-online mode with explicit mode
  readback. The first slice accepts full-frame
  1080p30/60 RAW12 and 1080p90/100 RAW10, with bitrate, GOP, and I/P QP delta
  taken from the existing config ABI.
- Added shared HEVC RTP output over UDP/abstract UNIX sockets and VFRM v1
  whole-frame SHM output. Packet-SHM, live output switching, sidecar, and
  pressure throttling remain follow-up work.
- Enabled the shared HTTP/WebUI layer with a CV610 capability mask and native
  live bitrate, GOP, I/P QP-delta, RTP payload-size, and IDR controls.
- Added CV610 debug OSD through a thin CLUT4 region adapter over Waybeam's
  shared drawing, palette, font, stats-panel, and CPU-sampling helpers.
- Added the standalone-verified CV610 inner-ACODEC -> AI -> vendor AENC/Opus
  path at 48 kHz mono, 10 ms, and 32 kbit/s restricted low delay. Encoded Opus
  frames use the shared RTP packetizer and output-socket helpers; unsupported
  audio formats stay validation-gated. Audio configuration remains an explicit
  JSON plus platform-loader setting rather than an HTTP control because a
  daemon respawn cannot load the opt-in kernel modules safely. Local transport
  drops consume RTP sequence numbers, and UNIX/Frame-SHM video routes its
  separate audio stream to the established loopback Waybeam Link listener.
- Added the source-built IMX662 sensor plugin and CV610 staging/build docs.
  The staged bundle includes the device-verified CV610 init script and sensor
  clock profile. No proprietary SDK sources or runtime binaries are committed.
- Guarded the SigmaStar RIU die-ID reader from CV610 builds, added reusable
  Annex-B iteration with host tests, and added a CV610-specific config
  validation test.

The integrated binary is confirmed on CV610 hardware at 1080p100: Waybeam Hub
decoded the stream, an independent 15-second FFmpeg pass sustained about 99
fps, SIGTERM teardown completed in about 200 ms, and the installed init service
completed a cold module restart in eight seconds. Frame-SHM, repeated restart
cycles, and soak validation remain before the backend is production-ready.
The shared HTTP/WebUI now exposes only the controls backed by native CV610
callbacks; unsupported fields are capability-gated.
The audio encoder produced about 100 access units/s with zero send drops, and
a host check confirmed consecutive PT98 RTP packets with 480-tick (10 ms)
timestamp advances. Acoustic content is not verified because no microphone
was connected. A service-stop hang exposed stale CV610 pidfile handling after
an API respawn; the init script now tracks all respawn process names and
refuses module unload until the graph has no owner. Device retest of that fix
is pending a power cycle.

## [0.64.0] - 2026-08-06

`ltr` resilience preset — maximum non-reference density with a long GOP.

- **`video0.resilience = "ltr"` / `"ltr:<N>"`** selects the most resilient
  SVC-T reference structure this SoC can express: `InRnRnRn…`, half the
  frames non-referenced, so half of all frame losses cost exactly one frame
  instead of cascading to the next IDR. Unlike `rally` (same 1:1 ratio) it
  preserves the caller's `gopSize` and forces intra-refresh off, so it can be
  paired with a long GOP and asymmetric transport FEC — heavy protection on
  the IDR, light on the rest.
- **`MI_VENC_ParamRef_t.u32Enhance` semantics established by measurement**
  (Star6E, raw-ES NAL census; the SDK documents nothing). It is a *period*:
  exactly one frame in every `enhance + 1` is emitted non-referenced.
  Measured: `enhance=1` → 50.0 % droppable, `4` → 17.6 %, `299` → 0.3 %
  (the census dilutes the short group with IDRs and partial groups; the
  steady-state rule is exactly 1 in `enhance + 1`). **Smaller is more
  resilient**, so bare `ltr` uses 1 and `ltr:<N>` is strictly less resilient
  as N grows.
- Also established: P-frames always predict from the previous frame, never
  from the IDR (frame size is flat across the GOP under motion), and
  `bEnablePred` is a **no-op** for non-reference marking — `rally`
  (`pred=true`) and `ltr:1` (`pred=false`) yield identical patterns. The full
  SigmaStar SDK exposes no long-term-reference, SmartP, or GOP-mode API, so
  an IDR-anchored ("virtual IDR") structure is not achievable on Infinity6E.
- **`/api/v1/set?video0.resilience=<unknown>` now returns 409** instead of
  silently accepting. It previously returned 200, persisted the bad name to
  `/etc/waybeam.json`, and left the derived `intra_refresh_*` / `ref_*` fields
  holding the PREVIOUS preset's expansion — so `/api/v1/config` disagreed with
  the running encoder until the next start, where the disk loader falls back
  to `"off"` and the operator lands on a preset they never asked for.
  Pre-existing, but the parameterised `ltr:<N>` form makes the invalid space
  wide and plausible (`ltr:0`, `ltr:256`), so it is fixed here. Validation
  matches how `video0.framing` and `video0.stab_accuracy` already reject.
- Measured cost at pinned QP 30 on a moving scene: under 1 % bitrate delta vs
  `off` (5.62/5.64 vs 5.65/5.68 Mbps, alternating runs). `ltr` remains
  OSD-unsafe — it maximises the SVC-T chroma-smear condition. See README.
- Pre-upstream API contract cleanup: `qr.windowMs` now follows its declared
  live mutability (the next scan snapshots the new value), Maruko capabilities
  mark the Star6E-only QR and QP-bound fields unsupported, and the HTTP
  contract documents those backend gates plus recording `elapsed_ms`.

### Frame-SHM throttle floor

Dual floor for the frame-shm ring-fill bitrate clamp.

- The clamp floor was a pure percentage (250 permille), which tied how hard it
  may squeeze to the bitrate the OPERATOR configured -- a number unrelated to
  what the radio can carry. At the measured 21839 kbps it bottomed out at
  5459 kbps.
- That is the wrong shape for the case it exists to cover. If the link drops
  to MCS0 before waybeam-link's §9.6 bitrate demotion actuates (a 1.5-8 s
  outer loop), this clamp is the only thing defending the ring, and 5.5 Mbps
  is several times MCS0's deliverable capacity -- so the ring stays full and
  every referenced frame is a chain-breaking drop for the whole window.
- The floor is now `min(100 permille x configured, 2500 kbps)`: the percentage
  keeps it proportionate at ordinary bitrates, the absolute cap stops a high
  configured bitrate from stranding it above what the radio can carry. The two
  coincide at 25000 kbps, today's reference-craft ceiling; the absolute term
  only binds above that, and exists so the floor keeps its meaning when a
  higher-bitrate mode is configured. At the measured rate the floor is now
  2183 kbps instead of 5459.
- 2500 kbps derivation: MCS0/HT20 is 6.5 Mbps PHY, ~4-5 Mbps goodput after
  802.11 overhead, and 300/200 permille FEC leaves ~3.2-4 Mbps for video.
  2500 kbps of video is ~3.1 Mbps of airtime at that FEC, and sits clear of
  VENC_BITRATE_MIN_KBPS (1000), which remains the encoder collapse rail.
- The AIMD integrator gains a separate, lower control-range bound
  (VENC_SHM_THROTTLE_MIN_PERMILLE, 50) because the absolute cap can require an
  effective ratio below the percentage floor. Recovery to unclamped now takes
  19 AIMD windows (3.8 s) rather than 15 (3.0 s) from the deepest clamp.
  Cascade stability is unaffected -- this changes the clamp's range, not its
  200 ms period, so the inner/outer loop separation is preserved.

### Frame-SHM reference-chain recovery

Re-establish the reference chain when a frame-shm:// ring-full drop breaks it.

- A full ring discards a frame that is ALREADY ENCODED, so the decoder's
  reference chain breaks and it renders garbage until the next IDR — with a
  long GOP that is seconds. Nothing closed that loop: `waybeam-link` requests
  an IDR only on a `RecoveryRequest` arriving from the ground over RF, which
  is a full round trip and is off by default (`venc.recovery_enabled`), and
  the ring's `full_drops` counter is read by the link but wired to telemetry
  only. The producer is the one party that knows instantly, and now acts on
  it locally.
- Only when the drop actually broke the chain. An SVC-T enhance frame is
  non-referenced by construction, so discarding one costs exactly one frame;
  firing an IDR there would push the largest frame in the stream into a ring
  that is already full, to repair damage that never happened. The policy is
  one shared inline (`venc_frame_drop_breaks_chain`) rather than a copy per
  backend.
- Routed through the existing per-channel IDR rate limiter (100 ms), so a
  persistently full ring coalesces into at most ~10 IDRs/s instead of a
  keyframe storm. `video0.resilience=ltr` makes this materially cheaper still:
  half the frames are non-referenced and take the free path.

Deliberately NOT included: blocking the encoder when the ring fills. Bounded
blocking is a latency tradeoff rather than a free win, and unbounded blocking
cannot be made safe here — a dead SHM consumer gives no error signal (there is
no EPIPE equivalent), so it would wedge the encoder permanently with no
recovery path on SigmaStar.

### Unix queue accounting race fixes

Remove the data races on the `unix://` send-queue capacity accounting.

- `OutputSocketQueue` is touched by three threads, not one: the producer
  (encode) thread raises `unix_capacity` / sets `logged_capacity` from the
  send path, the HTTP/status thread reads both capacities to scale the
  reported transport fill percentage, and the control thread RESETS all three
  in `output_socket_capture_capacity()` on a live transport redirect. All
  three fields were plain `int`.
- The `transport_gen` seqlock does not serialise that third writer: the
  producer consults it only to snapshot the transport into its batch and then
  flushes outside it, so a redirect can land while a flush is still updating
  this struct. The reset was additionally a bulk `memset` racing those
  accesses.
- All three fields now use relaxed atomics, matching the `__atomic_*` idiom
  already used for `transport_gen`, and the reset writes them individually
  instead of `memset`. Relaxed is the correct strength: each is an
  independent scalar and nothing else is published through them.
- `output_socket_get_fill_pct()` now snapshots both capacities once instead of
  re-reading `unix_capacity` per use, so a calibration landing mid-call can no
  longer mix an old and a new value into a percentage that never existed.

This removes the undefined behaviour, not every interleaving: a redirect
racing an in-flight flush can still leave `unix_capacity` holding the previous
socket's learned value, because the compare-then-store in
`output_socket_note_saturation()` is not itself atomic. That is bounded and
self-healing — the next saturation on the new socket recalibrates it, and the
only consequence meanwhile is a skewed fill percentage. Serialising it would
need a lock on the encode thread's send path, which costs more than the
telemetry is worth. The threading contract is now documented on the struct.

No behavioural change. Flagged during review of #214, which introduced the
calibration write and deferred this to a maintainer call.

### Bounded Unix output and backpressure telemetry

`unix://` output no longer stalls the encoder, and its backpressure
telemetry now actually works.

For compatibility with transmission chains that intentionally used the old
blocking behavior as flow control, `outgoing.allowUnixEncoderStall` can now
restore it explicitly. It defaults to `false` and requires a restart. When
enabled, only `unix://` skips the 2 ms socket timeout and cumulative RTP frame
budget; UDP, SHM, and frame-shm behavior is unchanged.

Investigation started from streams misbehaving at and above 15 Mbps on
`unix://` with 1500-MTU RTP. AF_UNIX itself is not the limit — the same
send pattern measures 7.6 Gbps with a consumer that keeps up. Three
implementation issues converged at that bitrate instead.

- **`net.unix.max_dgram_qlen` is the real ceiling.** An AF_UNIX datagram
  sender blocks on the *receiver's* queue depth, which the kernel snapshots
  from this sysctl when the receiving socket is created. The default of 10
  datagrams is ~7 ms of buffer at 15 Mbps with 1400-byte payloads, while a
  single 60 fps frame is ~23 packets — so every frame overran the queue and
  the encode thread blocked mid-flush. Measured saturation: 11 in-flight
  datagrams. `init.d/S95waybeam` now raises it to 256 (~180 ms) at boot,
  before any consumer starts; raising it later cannot help a socket that
  already exists. venc warns on stderr when it finds a shallower value.

- **The blocking send sat inside the `GetStream`→`ReleaseStream` window**
  on the pinned encode thread, so a descheduled consumer held a VENC output
  slot and cascaded into dropped capture frames — measured at up to 74 ms
  for one `sendmmsg` burst, unbounded against a wedged consumer. Sends now
  carry `SO_SNDTIMEO` (2 ms), and the batch flush carries a 4 ms per-frame
  deadline. Both are needed: `sendmmsg()` applies `SO_SNDTIMEO` per message,
  so the timeout alone still let a 64-packet batch run past a frame period.

  Non-blocking sockets were evaluated and rejected: at the default qlen they
  dropped 54% of packets, far worse than the stall. Bounded blocking keeps
  zero drops whenever the queue is adequately sized.

- **The backpressure trigger was mathematically unable to fire on
  `unix://`.** The per-skb truesize estimate was 4096 bytes; a 1400-byte RTP
  datagram measures 2304, so a fully blocked socket read as 61% fill against
  a 75% high-water mark and `inPressure` never set. Worse, the denominator
  was derived from the sender's live view of the sysctl, which is not what
  the receiver captured — with the sysctl raised after the consumer started,
  a 100%-blocked socket reported **2%**. The denominator is now calibrated
  from the queue's actual saturation point, observed the first time a send
  blocks, which is exact and immune to the sysctl skew. The estimate is only
  a bootstrap.

- **Socket transports now report drops.** `transportDrops` / `packetsSent`
  were hardcoded to 0 for `udp://` and `unix://`, so a consumer that could
  not keep up produced no observable signal anywhere — no `EAGAIN` (blocking),
  no pressure flag, no counter. Congestion drops and successful sends are
  counted and surfaced through the sidecar trailer and
  `GET /api/v1/transport/status`. Congestion (`EAGAIN`) is counted separately
  from real errors.

- **Seqlock spin yields.** `begin_frame()` busy-spun while `apply_server()`
  held an odd generation across `socket()`/`setsockopt()`/`connect()`; the
  encode thread is pinned to CPU 0 and could starve the writer it waited on.

- **The 4 ms bound is truly per frame.** Large encoded frames can exceed the
  64-datagram `sendmmsg` batch. All internal flushes now share one cumulative
  budget; once it is spent, later packets from that already-broken frame are
  counted and discarded without starting another timeout window.

- **Target-local validation consumer.** `make unix-dgram-consumer` builds a
  small abstract-AF_UNIX RTP sink for either backend. It reports delivered
  bitrate, RTP gaps, marker cadence, receive spread, and per-payload-type
  packet counts, and can pause reads for deterministic backpressure/recovery
  and shared-socket audio tests. Direct-deploy helpers now start the standard
  binary through `S95waybeam`, so its queue-depth setup is exercised during a
  normal cycle.

Tests: the existing `unix bp` case pumped 1024-byte payloads and guarded its
high-water assertions behind `if (fill_pct >= 75)`, which is why the fill bug
shipped — the assertions simply never ran. It now uses MTU-sized payloads and
asserts unconditionally, plus a drain-side assertion that pressure clears.
A new `unix bound` case asserts a wedged consumer cannot hang the send path.
Both were confirmed to fail against the pre-fix code.

Handover and the on-device validation plan (V1-V10 with pass/fail criteria)
are in `documentation/UNIX_SOCKET_HANDOVER.md`. Star6E V1–V9 are confirmed on
device: 10/15/20/25 Mbps at 60 fps and 25 Mbps at 120 fps, deterministic
backpressure/recovery, UDP/SHM regression, live Unix↔UDP redirect, and
shared-socket Opus. The earlier device-unresponsive incident did not recur
after a human power cycle. Maruko V10 is also confirmed on SSC378QE / IMX335:
10/15/20/25 Mbps at 60 fps, 25 Mbps at 120 fps, and deterministic
backpressure/recovery all passed with zero steady-state gaps or drops.

Note: `SO_SNDBUF` is raised on both transports but is a no-op for `unix://`
(1 MiB effective is ~455 datagrams, far above any sane qlen); it remains the
binding limit for `udp://` only.

### Live rate-control QP bounds

RC QP bounds exposed as live controls (star6e).

- **`video0.minQp` / `video0.maxQp`** (MUT_LIVE, 0 = driver default) write
  `u32MinQp`/`u32MaxQp` in the CBR RcParam via a new `apply_qp_bounds()`
  callback. Unlike the frame-size caps (`u32MaxISize`/`u32MaxPSize` — verified
  no-op on star6e: 100/100 frames over a 4000 B cap at full target rate), the
  QP bounds are honoured instantly by the SDK rate controller: `minQp=35`
  collapses an on-target 10.3 Mbps stream to 1%.
- Driver-default bounds are captured on the first write so clearing a bound
  back to 0 restores the default live instead of latching the last value.
- Validation rejects QP values > 51 and `minQp > maxQp` (the SDK accepts the
  inversion silently and then misbehaves).
- Maruko returns 501 for the new group (callback not implemented).

Investigation context: the chronic CBR undershoot turned out to be
`fpv.noiseLevel` (VPE 3DNR) — level 1 yields 33-42% of target on a static
scene, level 0 yields 101-104%. No code change needed (shipped default is
already 0); these QP levers are the follow-on granular control.

### Recording elapsed-time reporting

Build fix: a version bump now actually reaches the binary.

- **`VERSION` is a prerequisite of every object** (`Makefile`) — `COMMON_CFLAGS`
  bakes the file's contents in as `-DVENC_VERSION`, but nothing depended on the
  file. `-MMD -MP` cannot cover it: `VERSION` is not a header, so the compiler
  never sees it and no `.d` file lists it. An incremental `make build` after a
  bump therefore recompiled nothing, and the binary went on reporting the
  previous release through `/api/v1/version` and the mDNS beacon — the two
  places a fleet is identified from. Only a `make clean` picked the new string
  up, which is precisely what release builds skip when they are in a hurry.

  Reproduced before the fix: bump `VERSION`, `make build`, and the old string is
  still the only one in the binary. After: the bump rebuilds and the new string
  is the only one present.

  The prerequisite goes on the pattern rule rather than on the two current
  consumers (`src/venc_api.c`, `src/mdns_beacon.c`) — a third consumer would
  otherwise go stale silently, and the cost is one full rebuild per release.

### Overlay-free QR scan windows

Inline QR scanning on Star6E: an overlay-free luma tap on VPE port1 and scan
windows decoded by the existing standalone `qr_decode` helper.

- **`qr.tapEnabled` / `qr.tapWidth` / `qr.tapHeight`** (`src/star6e_luma_tap.c`)
  — a read-only NV12 tap on VPE port1 with its own geometry, drained by a
  dedicated reader thread for the duration of a scan window. Off by default.
  `0` dimensions inherit the main stream. Unlike the MJPEG snapshot channel, a
  VPE port is a real scaler, so these genuinely change capture resolution.

  **Window-scoped, not pipeline-lifetime.** Bring-up only *arms* the tap;
  `/qr/tap/open` claims and enables port1, `/qr/tap/close` releases it back to
  stab/detect. Holding it for the whole run was measured too expensive: an
  always-on 1080p60 tap adds ~186 MB/s of SCL write traffic to shared DDR and
  cost 8-9 points of aggregate CPU while completely idle (45% → 54%). Device
  result for the cycling this introduces: **200/200 open/grab/close cycles with
  the encoder live, zero wedge signatures** — against snapshot.pgm's ~1 per 280.
  A live close stops and joins the reader, then **drains the port to quiescent**
  before resetting depth and disabling; disabling with buffers still queued is
  what races an in-flight mhal buffer.

  **The latch is the centre square** of the port output (1080×1080 from a
  1920×1080 stream), cropped during the copy. Done in software on purpose:
  `MI_VPE_SetPortCrop` is sticky on i6e and a leftover rect poisons a later
  detect run. The SCL scales but does not crop, so asking the port for a square
  directly would squash the aspect. Drops the outer frame where fisheye is
  worst, and cuts the latch from 2.07 MB to 1.17 MB.

  Why it exists: MI_RGN composites **per scaler output port**, and every
  overlay producer targets port0 — `debug_osd` here, `osd_render` in
  waybeam-hub. The MJPEG snapshot channel is a port0 1:N consumer, so every
  snapshot carries whatever HUD is running, over anything a QR marker might
  occupy. Measured on a bench craft: the hub HUD lands dead centre of frame
  with `debug.showOsd` false. port1 is a separate scaler output and is clean.

  The port is programmed and enabled **once per bounded scan window**, never
  once per capture. A minimum open lifetime, reopen cooldown, reader join and
  quiescent drain protect both lifecycle edges. This is the lesson of the
  retired `/api/v1/snapshot.pgm` (0.60.0), which cycled Enable/Disable per HTTP
  request — `DisablePort` raced an in-flight mhal buffer and jammed the VPE
  input FIFO, about twice per 560 stressed captures. Lowest-priority claimant:
  skipped with a log line when `video0.framing=stab` or `detect.enabled` holds
  port1.

- **Scan windows with self-closure.** `GET /api/v1/qr/scan[?ms=N]` opens a
  window (`409 port1_busy` when stab or detect holds port1 — reported
  immediately, never queued); a supervisor thread closes it when the deadline
  expires, so a client that dies mid-scan cannot strand the port. Re-scanning
  while a window is open only **extends the deadline** and never touches port
  state, which is what keeps repeated scans from re-introducing the
  Enable/Disable cycling that wedged snapshot.pgm. `GET /api/v1/qr/stop` ends a
  window early; `GET /api/v1/qr/status` polls one without disturbing it
  (`scanning`, `remaining_ms`, `frames`, `grabs`, `port1_owner`).
  `GET /api/v1/qr/tap.pgm` returns one frame as a P5 PGM (self-describing
  dimensions, stride removed; `503` with no window open, `504` on no frame).

  The supervisor is the **only** thread that opens or closes the port, so no SDK
  port call is ever concurrent. It waits in 200 ms slices against a
  `CLOCK_MONOTONIC` deadline rather than sleeping to an absolute wall-clock
  time, so a clock step cannot strand an open window.

  `qr.windowMs` (default 15000, clamped 1000–60000) is the default budget,
  `MUT_LIVE` since it is read when a window opens.

  Note `/api/v1/qr/stop`, not `/qr/scan/stop`: the HTTP router matches on prefix
  and accepts a `/` continuation, so a nested path is swallowed by the
  `/qr/scan` route.

- **The decode cascade remains isolated in `qr_decode`.** The daemon writes
  each frozen luma frame to a bounded temporary PGM and invokes the existing
  standalone helper, keeping vendored quirc/stb code out of the long-lived
  encoder process. If the helper is absent, scan windows and `/qr/tap.pgm`
  still work but no payload is decoded.

  **Peak heap 5.04x -> 3.30x W*H**, measured with a live-heap interposer on a
  1080x1080 exhaustive decode (5741 KB -> 3756 KB). The old peak held five live
  W*H buffers at the lens-blur stage. Now the 3x3 box blur runs **in place**
  behind a two-row ring of saved originals (bit-identical to the allocating
  form), one scratch arena is reused across blur/lens/lens-blur/inversion with
  the quarter-size half-scale slot behind it, and the blur is recomputed after
  the lens stage rather than held across it — one extra ~30 ms pass on the
  deep-failure path for a whole W*H.

  The cascade also takes an abort callback, checked at every region boundary.
  Without it the finest granularity a scan window has is one whole attempt.

- **Windows decode through an isolated helper** (`src/star6e_luma_tap.c`). The
  supervisor thread drives the helper as well as the port — one thread deliberately,
  because "only the supervisor ever opens or closes port1" is what removes the
  concurrency race class that retired snapshot.pgm. Each pass takes a fresh
  frame, writes the frozen luma latch to a temporary PGM, invokes
  `/usr/bin/qr_decode`, and publishes the result. **A decode ends the window
  early**: the point of a window is to find one code, and
  finishing hands port1 back to detect/stab seconds sooner.

  No second frame buffer. A `decoding` flag freezes the latch for the helper's
  duration: the reader keeps draining — that is never optional — but stops
  overwriting, and `/api/v1/qr/tap.pgm` answers `409` rather than blocking an
  httpd worker or handing back a frame being mutated.

  `/api/v1/qr/status` grows a `decode` block: `attempts`, `decoded`, `payload`,
  the helper `stage`, and both the winning and most recent invocation durations.
  It survives the window closing and is cleared only by the next `/qr/scan`, so
  a client polling at 1 Hz still sees the payload from a window that found its
  code and shut down between two polls.

  Decoder measurements before process isolation, on the Star6E bench (imx335,
  1080x1080 centre square): a marker in
  view decodes on the **first frame of the first attempt in 73-88 ms** (mean 81
  ms over 100 windows). Worst case — no code present, full cascade — is 431 ms
  at 1080x1080, 238 ms at 720x720, 117 ms at 540x540. The standalone helper
  builds the cascade and quirc at `-O3` (1.66x on Cortex-A7); the daemon does
  not link either. End-to-end helper overhead requires device revalidation.
  Star6E only; Maruko has no luma tap.

- **Scanning no longer starves the video pipeline.** A window that finds its
  marker decodes on the first frame in ~85 ms and closes, which is what every
  scan during development did. A window with *nothing to find* ran back-to-back
  431 ms cascades for its whole budget, and on this 2-core part that measured
  60 fps -> **23 fps** with the CPU pegged at 100%: only the encoder thread is
  `SCHED_FIFO`, so the ISP, AWB and frame-shm threads lose to a decoder that
  never yields. The scan thread now runs at `nice 10` and holds a duty cycle —
  after each attempt it idles for as long as that attempt took, keeping
  scanning under half a core at any geometry. The idle is an interruptible cond
  wait, so `/api/v1/qr/stop` does not wait it out. Measured after: 60 fps at 55%
  CPU through a 25 s no-decode window.

  Explicitly *not* fixed by holding port1 open continuously: that addresses none
  of the cascade CPU, and adds ~186 MB/s of SCL write traffic and 8-9 CPU points
  for the whole run while locking detect and stab out of port1 permanently.

- **Both edges of the port cycle are now rate-limited.** The port lifecycle
  itself is cheap — instrumented, `SetPortMode` 0 ms, `EnablePort` 0 ms,
  `SetChnOutputPortDepth` 0 ms either way, `DisablePort` 33-53 ms — but the
  *rate* is what matters. Two hard kernel panics on the bench (`panic=20`
  auto-reboot) came from `/api/v1/qr/scan` followed immediately by
  `/api/v1/qr/stop`, which disables a port that only just came up while the SCL
  still has buffers in flight: the #205 `snapshot.pgm` failure exactly.

  This corrected an over-claim. The 200/200 and 100/100 cycle soaks behind
  "window-scoped cycling is safe" had every window closing *naturally*, on
  decode or deadline, after reaching steady state. An operator-triggered close
  of a brand-new port was never exercised, and it is the dangerous case.

  So `LT_REOPEN_COOLDOWN_MS` (500 ms) floors the interval between opens, and
  `LT_MIN_WINDOW_MS` (750 ms) floors how long the port stays up once enabled.
  Every early exit — `/qr/stop`, the decode-triggered close, the out-of-memory
  exit — pulls the deadline in to that instant rather than to zero, so `/qr/stop`
  requests a close rather than forcing one while still blocking until port1 is
  actually free. Cost: a window that decodes in 85 ms holds port1 for 750 ms
  instead of ~200 ms.

  Device soak after the fix: **130 scan-then-immediate-stop cycles**, 30 natural
  closes, 60 extends and 25 concurrent stop/scan/status races — 245 port
  operations, zero wedge signatures, zero reboots, daemon pid unchanged
  throughout.

- **`tools/qr/test-images/phone.html` generates markers.** It was a viewer for
  one checked-in SVG; it now carries a Version-1/Q alphanumeric QR encoder and
  the 33x33 outer-frame wrapper inline, so a phone with no network can render
  any valid payload — typed or from a **Random** button — plus tiny and inverted
  presets alongside the existing sizes and 35-degree tilt.

  Inline because the page has to work off a memory card, and validated so that
  cannot rot: `make qr-test-phone` extracts the encoder from between sentinel
  comments and, for ten payloads spanning both legal prefixes and the whole
  alphanumeric set, asserts the wrapper is byte-identical to
  `tools/qr/generate_qr.py` and that every rendered marker decodes back through
  the same cascade the craft runs, at two scales. 51 assertions.

  The data mask is deliberately *not* required to match the Python generator:
  all eight masks are valid, and python-qrcode scores them slightly differently
  from ISO 18004, so the two disagree on about two thirds of payloads while both
  being correct. Where the masks do agree the matrices are identical to the
  module, and the test asserts exactly that.

- **A tap that cannot deliver is refused instead of squatting on port1.**
  `MI_VPE_SetPortMode` and `MI_VPE_EnablePort` both return success for
  geometries the SCL will not in fact drive — measured, a 160x90 port enables
  cleanly and then delivers zero frames forever, while holding port1 against
  detect and stab for the whole window. `lt_port_open()` now waits up to 1 s
  for a first frame and tears down if none arrives, so `/api/v1/qr/scan`
  answers an error immediately. Deliberately a frame probe rather than a
  geometry rule: the SDK's real constraint is undocumented and the obvious
  guess is wrong (the working 1920x1080 is not 16-aligned in height).

- **`snapshot.width` / `snapshot.height` tooltips corrected**
  (`src/venc_api.c`) — they described these as sizing the QR capture, which is
  false in both directions. They set the VENC channel's *maximum* encodable
  resolution and VENC has no scaler: measured, a value below the main stream
  makes every frame fail `_MI_VENC_ValidateResolution` and `/snapshot.jpg`
  answers `504` permanently, while a value above it changes nothing.

## [0.60.2] - 2026-07-30

Star6E IMX335 driver gets the same power-on fix as Maruko's in 0.60.1.

- **`pCus_poweron()` asserts before releasing**
  (`drivers/sensor_imx335_star6e.c`) — the first PWDN/RESET pair used the
  release polarity, so the sequence was release → CSI config → release
  again: no clean reset edge, the same flaw shape the 0.60.1 Maruko fix
  amended.  Now the canonical SDK pulse used by both IMX415 drivers and the
  0.60.1 Maruko IMX335 driver: assert PWDN+RESET through CSI config, release
  PWDN, 31 ms rail settle, release RESET, start MCLK.  The IMX415 Star6E
  driver already had the correct sequence — unchanged.
  `sensors/star6e/sensor_imx335_star6e.ko` rebuilt (vermagic unchanged).

## [0.60.1] - 2026-07-30

Two Maruko fixes contributed by @tipoman9 (upstream PRs #103/#104, verified on
SSC378QE + IMX335), inlined onto the fork with review amendments.

- **IMX335 driver: full power-up pulse in `pCus_poweron()`**
  (`drivers/sensor_imx335_maruko.c`) — the function only configured CSI and
  MCLK and never touched PWDN/RESET (the toggle lived in
  `pCus_HardwareReset()`, which nothing calls).  Boards that hold the sensor
  in reset until the driver releases it therefore never answered i2c: every
  mode's init table NAK'd and `MI_SNR_Enable` failed with `0xA01B201F`,
  regardless of mode.  Amended from the contribution to the canonical SDK
  sequence already used by the IMX415 Maruko driver: assert PWDN+RESET through
  CSI config, release PWDN, 31 ms rail settle, release RESET, start MCLK — a
  clean reset edge on every board, not just a pin release.

- **`isp.keepAspect=false` honours stretch-to-fill again**
  (`src/maruko_pipeline.c`) — v0.24.1 hardcoded the precrop to `keep_aspect=
  true`, silently centre-cropping every AR-mismatched Maruko pipeline and
  reducing `keepAspect=false` to a printed note, contradicting the documented
  contract.  The override was justified by a *single-axis* squeeze failure
  (1920x1080 -> 1440x1080, height unscaled — the I6C SCL genuinely refuses
  that), but a two-axis non-uniform scale is fine (contributor-verified:
  2496x1872 -> 1920x1080 at steady 50 fps, zero drops).  `keep_aspect` is
  passed through again; amended from the contribution to **keep the
  centre-crop override for the single-axis geometry** (device-verified fatal:
  0 frames, wedged the binned sensor) instead of warn-and-proceed.

- **`isp.keepAspect` un-gated on Maruko** (`src/venc_api.c`, contract
  `0.16.1`, non-breaking) — found during device verification of the above:
  the field was advertised `supported:false` on Maruko (added when the
  blanket override made it a no-op), so `/api/v1/set` rejected it with
  `not_implemented` and the WebUI greyed the control — the pipeline fix alone
  was unreachable through the API.  Capabilities now report it supported;
  dashboard tooltip rewritten for the single-axis-exception behaviour.

## [0.60.0] - 2026-07-30

QR scanning moves to `GET /api/v1/snapshot.jpg`; `GET /api/v1/snapshot.pgm`
(added 0.59.0) is **retired** and answers 404.  Contract 0.16.0.

- **Why: the PGM path could kill the SoC.**  It captured through a
  short-lived VPE port1 / SCL port3 tap programmed and torn down per request.
  Device stress-testing on the Star6E bench showed the teardown can race an
  in-flight MHAL buffer — `MI_VPE_IMPL_DisablePort ... mhal not return
  buffer` — after which the whole VPE input FIFO jams
  (`MI_SYS_IMPL_EnsureInputPortFifoEmpty ... no response in 1000ms!`
  repeating) and the box ends in a kernel panic-reboot or a hard hang
  needing a power cycle.  Two wedges in ~560 stressed captures; the race
  window scales with frame size.  The race is kernel-side — userspace can
  only stop rolling the dice.
- **The MJPEG channel has no such cycle.**  It is created once at pipeline
  start, stays bound, and pulse-encodes per capture (StartRecvPic →
  GetStream → StopRecvPic) — the same path `snapshot.jpg` has always used.
  Rapid back-to-back captures are safe, verified by re-running the same
  kill suite against it.
- **`qr_decode` now reads JPEG natively** (and still reads P5 PGM, sniffed
  from the first two bytes; both work from a file or stdin).  Decode is the
  vendored public-domain `stb_image` (`tools/qr/stb/`, JPEG-only build,
  luma extracted directly).  The loader slurps bounded input to memory
  first, so stdin JPEG works without seeking.
- **`qr_decode` is built -O3 instead of -Os** — 1.66x faster end-to-end on
  the Star6E bench (1265 ms vs 2206 ms on a 3.6 MB frame, best of 3) for
  +24 KB of binary.  Decode latency is what the tool is judged on.
- **VPE port1 / SCL port3 go back to stab framing and NPU detection
  exclusively.**  The snapshot arbiter claim, the sticky `SetPortCrop`
  restore discipline, the BSP-refusal fallback and the tap geometry helpers
  are all deleted (~600 lines across both backends).  The 409
  `snapshot_gray_busy` and 400 `bad_crop`/`bad_max_dim` error codes are gone.
- **QR capture resolution now follows `snapshot.width`/`snapshot.height`**
  (0 = inherit the main stream).  Size the MJPEG channel up when markers
  must decode from further away — QR range scales with pixels per module.
- **WebUI: new "Snapshot" card.**  `snapshot.enabled`, `quality`, `width`
  and `height` carry `FIELD_UI` metadata, so the group renders from
  `/api/v1/capabilities` with no dashboard edit; the section was API-only
  before.
- `qr_watch.sh` polls `snapshot.jpg`; `QR_TMP_PGM` env override renamed
  `QR_TMP_IMG`.

## [0.59.0] - 2026-07-26

New `GET /api/v1/snapshot.pgm` endpoint returns a grayscale frame as a binary
P5 PGM — the luma plane of an uncompressed NV12 frame, with no JPEG
encode/decode step. It rides the existing `snapshot.enabled` gate and the
snapshot subsystem mutex, so `.jpg` and `.pgm` share one switch and never run
concurrently. Implemented on both backends: Star6E taps VPE port1, Maruko taps
SCL port3.

On Star6E the grab programs a short-lived VPE **port1** tap, drains exactly one
frame (`MI_SYS_ChnOutputPortGetBuf`, mirroring the detector tap in
`star6e_ipu_yolo.c`), and tears the tap back down — resetting the user output
depth before `MI_VPE_DisablePort` on every exit path. It deliberately does not
touch port0: a user frame queue may only be registered on a port with no
downstream hardware bind, and port0 is bound to the H.265 encoder (1:N with the
MJPEG channel), where registering one makes the kernel SCL run user output
tasks alongside the bind and trips a hard `BUG` in
`_MI_SYS_IMPL_AllocBufDefaultPolicy` on the `vpe0_P0_MAIN` worker — stalling
the live encode path and, on some runs, resetting the board.

Because port1 is the single second scaler output, the capture takes it through
the `star6e_vpe_ports` arbiter: stab framing and NPU detection own that tap for
a whole run, so a capture attempted while either is active returns `409`
(`snapshot_gray_busy`) instead of reprogramming the tap underneath them. Tap
geometry starts from the configured snapshot size and caps the long side at
1280 px (aspect-preserved, width 16-aligned); PGM is self-describing, so
consumers read the real dimensions from the header. Capture failure is
non-fatal — the endpoint serves an error and the encode path is unaffected.

Maruko follows the same shape on i6c primitives. Every SCL output already has
an owner — port0 is the main H.265 output (RING, 1:1), port1 carries the bound
MJPEG channel, port2 is the stab tap — so the grayscale tap uses **port3**, the
NPU detector's, arbitrated by the new `maruko_scl_ports` module so a capture
loses to a running detector instead of reprogramming its tap mid-inference. The
i6c MI_SYS entry points take a leading `u16 soc_id` and the port is configured
through `SetPortConfig` with an explicit crop window, which the pipeline now
publishes via `venc_jpeg_set_gray_crop()`.

Ships a freestanding scanning backend under `tools/qr/`: `qr_decode.c`
(vendored quirc, ISC) decodes P5 PGM captures with mirror, overlapping-tile,
half-scale, light-denoise, and inverted passes. A required continuous black
outer frame now supplies direct four-corner projective geometry for a
Version-1 QR while keeping extraction in the original image; framed grids use
3×3 majority sampling. The default output is limited to the minimal
Version-1/Q alphanumeric envelope — exactly 16 characters, leading type `P` or
`C` — while `--raw` keeps arbitrary-symbol bench diagnostics available.

`qr_watch.sh` remains a standalone polling helper with no action dispatch. It
reports `409`/`503` distinctly and can stream decodes with `-c`. Pairing,
commands, boot scheduling, persistence, packaging, and service integration are
deferred to a separate consumer work package. A deterministic host corpus
compares stock and frame-assisted recognition across front, rotated, mirrored,
small, and projectively compressed renders. Build with `make qr-decode`; run
the core corpus with `make qr-test-host`. An extended 768-image Version-1/Q
series sweeps marker width, four perspective ratios down to 0.35, rotation,
perspective direction, and four defocus levels. The outer frame was identified
in 768/768 cases; exact payload decode reached 709/768 (92.3%) versus stock
quirc's 506/768 (65.9%). The SSC338Q Star6E bench reproduced those exact
counts natively. The production decoder now strictly requires the bounded
outer frame and never performs global finder discovery. It tries the frame's
direct transform first, then—only after ECC failure—runs finder refinement in
a tight ROI derived from the accepted frame. This preserves rejection of bare
QRs while absorbing fisheye curvature that a four-corner homography cannot
model. Opt-in `--stats` diagnostics report each applied pass, per-stage
monotonic timings, frame/refinement/finder candidate counts, QR and envelope
outcomes, and a final summary on stderr without contaminating payload-only
stdout. `qr_watch.sh -v` exposes the same trace during polling.
Continuous polling is completion-aware: the next snapshot starts immediately
after the previous capture/decode returns unless that would place capture
starts less than 0.5 seconds apart. This removes the former fixed post-decode
sleep for direct on-camera stress testing; `-i` may still request a slower
minimum start cadence.

Live phone-display validation on the SSC338Q Star6E confirmed the combined
path on real optical captures: the exact `P23456789ABCDEFG` envelope decoded
through both `sharp/tile/refine` and `sharp/half/refine`. The initial
five-frame run succeeded 2/5 times. Failed large presentations remain
dominated by the bench camera's strong fisheye curvature; offline radial
correction made those same frames decodable. The decoder now includes that
compensation as a late, single-coefficient (`k1=-0.30`) nearest-neighbour
fallback. It retries only corrected full-frame sharp and light-denoised images,
and both remain subject to the mandatory frame gate; no unbounded finder path
is introduced. A fresh live five-frame Star6E burst improved from 2/5 to 4/5
exact decodes: one normal bounded refinement and three lens-corrected bounded
refinements. The remaining miss found no acceptable frame even after
correction.

The bounded scan path retains its behavior-preserving reductions: symmetric
frame geometry is scored once per candidate, nine-tile scans are deferred,
blur-half no longer delays the fisheye fallback, radial correction precomputes
invariant column terms, repeated cell coordinates are cached, and quirc
retains high-water work buffers. The final standalone build prioritizes flash
size with `-Os`, per-function/data sections, linker garbage collection, the
existing Star6E NEON/VFPv4 flags, and quirc's single-precision perspective
math. The stripped decoder is 30,288 bytes on Star6E and 30,136 bytes on
Maruko, down from 63,052 bytes for the previous Star6E `-O3`/loop-unrolled
build. On the saved hard 1280x720 fisheye capture, the size build decoded the
same `lens-blur/full/refine` stage and exact payload in a 425 ms mean over 20
runs (404–472 ms), versus 257 ms for the prior speed build. Sampling,
thresholds, candidates, scan coverage, payloads, and the full 768-case counts
remain unchanged.

Merge-gate hardening caps PGM dimensions, detects decimal overflow, requires
the endpoint's 255 maxval, handles allocation failures as fatal input errors,
and accepts `--` before filenames beginning with a dash. New CLI regressions
cover valid fixture decode, option ordering, malformed headers, duplicate
inputs, and dimension limits. `qr_watch.sh` now uses a unique `mktemp` file
per process with signal/exit cleanup rather than a shared symlink-prone path,
and decoder execution failures no longer look like ordinary empty frames.
The included Python generator forces Version 1/Q/alphanumeric metadata,
validates the minimal envelope, and emits the exact bounded profile as SVG,
binary-clean PNG, or decoder-ready PGM. The checked-in phone and CLI fixtures
are generated by that tool.

## [0.58.0] - 2026-07-26

Maruko gains IPU object-detection parity with Star6E: a raw 800x448 NV12 tap
on SCL port 3 feeds the shared ABI-3 plugin, publishes the unchanged DETECT
sidecar trailer, draws optional debug-OSD boxes, and supports pipeline-thread
enable/disable and model reload without restarting video. Teardown follows the
i6c drain-while-disable rule so an IPU reader cannot pin ISP-to-SCL shutdown.

The I6C model is platform-specific; an I6E `.img` is rejected. On the
SSC378QE bench, `inferInterval=1` held the SCL buffer through every IPU invoke
and reduced video from 59.7 to 38.0 fps while producing only 6.9 inferences/s.
At the new Maruko default `inferInterval=2`, the alternate-frame drain breaks
that backpressure loop: video stays at 59.7 fps and detection reaches 8.5-10.1
Hz. The final 15-second sample measured 9.46 Hz with 51 ms median / 94 ms p95
snapshot age and no new VENC ring-drop lines. Star6E's existing 800x448 bench
is 9-10 Hz, 52/96 ms age, and 90 FRAME/s. Detector throughput and freshness
are therefore at parity; encode rate remains the configured sensor/backend
difference.

Small-flash Maruko deployments can keep the model as an xz archive under
`/root/models`; `S95waybeam` stages the exact configured `/tmp/*.img` before
startup. Contract 0.15.0 records Maruko support for the existing detect fields.

## [0.57.0] - 2026-07-26

`frame-shm://` egress no longer answers a full ring by throwing away an
encoded frame. When the ring backs up, the encoder now lowers its own
bitrate until the consumer keeps up.

**The bug this closes.** `venc_frame_ring_begin_write()` returns -1 on a full
ring and `star6e_output_send_frame_ring()` discards the whole frame. That drop
lands *after* encode, so the H.265 reference chain breaks and the receiver
decodes garbage until the next IDR or the end of the GDR cycle. Post-encode
frame skipping is the same mistake v0.9.2 shipped and reverted; dropping
frames was never backpressure, it was the damage.

Ring geometry is 8 slots x 384 KB — 80 ms of queue at 100 fps. By the time it
is *full* the latency budget is already gone, so the control variable is
occupancy, not the drop counter.

**What shipped** — `src/venc_shm_throttle.c`, a pure AIMD controller evaluated
every 200 ms on the window's **low-water** occupancy:

    low_water >= 2       permille = max(250, permille * 4/5)
    full_drops increased permille = max(250, permille * 3/5), once per window
    low_water <= 1       permille = min(1000, permille + 50)

Low-water, not high-water, and the bench is what settled it. On `.232` at
100 fps a *healthy* consumer still lets the ring spike to 2-3 slots inside a
200 ms window and drains it again — it reads one frame per event-loop
iteration, so short bursts are normal. High-water read those as congestion
and clamped 15-25 % at random, oscillating 740-1000 with nothing wrong.
Low-water asks whether the ring failed to drain *at any point* in the window;
if it touched bottom, the consumer is keeping up. After the change, 30 s of
healthy operation holds at exactly 1000 with zero drops.

The drop charge is capped at once per window for a related reason: a full
ring increments `full_drops` on *every* frame, so an uncapped per-frame x3/5
is `0.6^20` inside one window — an instant slam to the floor on the first
congested window, with no chance to settle at an intermediate rate. That is
what the bench showed before the cap: 781 -> 250 in well under a second.

Retreat to the floor takes ~1.4 s, recovery to unclamped ~3.0 s. The 250 floor
means a wedged consumer cannot drive the encoder to nothing; entering and
leaving the floor is logged exactly once each, because a clamp silently pinned
at its floor has spent all its authority and reads as "working" when it is not.

**It is a clamp, never a veto.** The factor multiplies the bitrate programmed
into the SDK and `video0.bitrate` is never written. That is what keeps an
external rate controller's write-on-change cache coherent — every write still
succeeds and lands in config — and keeps the WebUI slider, `mod_venc` and curl
truthful. Returning an error from the setter was considered and rejected: a
write-on-change controller treats non-2xx as a failure, never commits, and
re-pushes the identical value forever.

Steady-state rate stays owned by the external controller. The split is by
timescale: this loop is ~200 ms, the link's actuators are 1.5-8 s. Keep that
>=5x separation if you retune either side, or the two couple into oscillation.

**The IDR trap.** `apply_bitrate()` forces an IDR so the decoder resyncs
against the new RC state. The clamp re-programs as often as every 200 ms while
the ring is backed up, and an IDR is the largest frame the encoder emits —
IDR-ing through congestion would feed the queue being drained. The throttle
path goes through `apply_bitrate_ex(kbps, want_idr=0)`; small between-IDR rate
changes are absorbed by the rate controller, which the existing rate-limit
gate already relies on.

Pipeline-thread applies take `venc_api_cfg_trylock()` rather than blocking:
a restart-class HTTP set holds that mutex across a full pipeline reinit, and
stalling the encode loop behind it is worse than skipping one advisory update.
A missed window is retried 200 ms later.

**Config** — `outgoing.shmThrottle` (bool, live, **default on**). Everything
else is compile-time, following `include/idr_rate_limit.h`; exposing water
marks as config was tried in v0.9.2 and removed as more noise than useful.
Default-on because a clamp that only ever reduces below what was already
requested is hard to make worse than an uncorrected whole-frame drop, and the
wfb rigs, bench harnesses and `radeon-vrx` / hub consumers have no rate
controller at all.

**Observability** — `throttlePermille` and `effectiveBitrateKbps` in
`/api/v1/transport/status`; a `thrNN%` suffix on the debug-OSD `br` row when
engaged (absent when unclamped, so the common case stays uncluttered); and
`throttle_permille` in the sidecar TRANSPORT_INFO trailer, carved from the old
`_pad[2]` so the trailer stays 16 bytes and every later trailer keeps its
offset. A pre-0.57 producer sends 0 there, which consumers must read as "not
reported", not "clamped to nothing". `_Static_assert`s now pin all three
trailer sizes. frame-shm also emits a TRANSPORT_INFO trailer at all now — the
flag condition had only ever covered `shm://` and the socket transports.

Both backends, one control law — Star6E and Maruko are byte-symmetric here
(same 8 x 384 KB ring), and a per-backend divergence in shared behaviour is a
standing drift trap in this repo.

**Measured on Star6E `.232`**, IMX335 100 fps, `frame-shm://venc_frame`
consumed by `waybeam-link tx`, encoder asked for 60000 kbps on a link that
cannot carry it, 40 s each:

| | frames dropped | frames delivered | loss |
|---|---|---|---|
| 0.56.0 | 1949 | 2069 | **48.5 %** |
| 0.57.0 | 110 | 3892 | **2.7 %** |

Half the encoded stream was being discarded post-encode, every drop breaking
the reference chain. Also device-confirmed: `video0.bitrate` reads 25000 (then
60000) in `/api/v1/config` throughout, unchanged by the clamp; recovery from
the floor is 250 -> 500 -> 750 -> 1000 in exactly 3 s with no overshoot, then
pinned; and the floor warning fires exactly once on entry and once on exit.
**Producer health in the ring header.** `health_magic` (`"VHLT"`) at offset
76, `full_drops` u64 at 80, `throttle_permille` u16 at 88 — carved from the
producer-owned pad on cache line 1. `sizeof` stays 192, `version` stays 1,
nothing before them moves, and both external consumers
(`radeon-vrx`, `waybeam-link`) address this header by *byte offset*, so the
change is invisible to them. `_Static_assert`s now pin every header offset,
because a reorder would move these out from under both consumers with
nothing failing to compile.

`full_drops` closes a structural blind spot: the counter is otherwise
process-local to venc, so an ingress node cannot see the drops it is
causing. `health_magic` is what stops a new consumer reading an old
producer's zeroed pad as "no drops" — it is published last, after the
counters and before `init_complete`. Consumers must treat a mismatched
marker as "does not report health", and must never reject a ring because
these bytes are non-zero; that check, applied to the per-frame meta, is what
made `radeon-vrx` drop every delta frame against a #179 producer.
Canonical spec: `protocols/frame-shm.md`.

Not included: the bounded `outgoing.shm_block_us` net. It only engages after
this clamp has already failed, and it blocks the encode thread — the
SigmaStar D-state / MI_SYS wedge path. It waits until the clamp has device
numbers.

## [0.56.0] - 2026-07-25

Star6E AWB actually adapts now. `isp.awbMode=auto` was not adaptive: AWB latched
whatever it estimated in the first second and never moved again, leaving a
standing cast (a visible yellow on IMX335) that no scene change corrected.

**Cause.** Two things in `star6e_pipeline.c` left AWB with nothing driving it.
`MI_ISP_DisableUserspace3A` at ISP bin load kills the SDK 3A_Proc thread, and
`MI_ISP_CUS3A_Enable(0, {0,0,0})` ~1 s after start returns every module to the
ISP-internal algorithms. AE survives that (it is driven separately, which is
why only colour looked wrong), but the internal AWB does not converge here.

**Why not simply keep the SDK's 3A stack.** Because it runs per frame. Measured
on .232: correct colour and genuine convergence at 60 fps for +6pp venc CPU, but
at 120 fps it pins the CPU at 100% and the box is unusable. Maruko solves this
with a `CUS3A_RunOnce` pacer — not portable, i6e `libmi_isp.so` exports no
`RunOnce`/`RunOnceEn`/`SetRunMode` (i6c only).

**What shipped instead** — a userspace AWB loop (`src/star6e_awb.c`) that runs at
a rate we choose rather than per frame:

    MI_ISP_AWB_GetAwbHwAvgStats   128x90 block R/G/B averages, computed by
                                  hardware, free to read
    grey-world + block rejection  a few hundred microseconds
    MI_ISP_CUS3A_SetAwbParam      apply

AWB is handed to userspace (`Cus3AEnable_t.bAWB = 1`) so this loop owns it.
Because nothing is per frame, **cost is independent of frame rate** — that is the
entire point, and it is what the SDK stack could not offer. Rate is
`isp.awbFps` (default 15 Hz, 0 disables). Gains are damped (1/4 per tick) and
gated by a 3% deadband so a settled scene stops rewriting them; degenerate
scenes are handled by rejecting saturated/black blocks, requiring ≥5% of the
frame usable, and clamping the result.

`isp.awbMode=ct_manual` is unchanged for the operator and still hard-locks the
colour temperature: it hands AWB back to the ISP-internal algorithm, which is
what the existing `MI_ISP_AWB_SetCTMwbAttr` path applies through, and stands the
loop down. `awbFps=0`, a failed loop start, or manual mode all leave AWB exactly
where it is today.

`isp.awbFps` applies **live** — retuning the rate, stopping the loop and
restarting it all happen without a restart, because the rate is a plain store
the loop thread reads on its next wake and ownership of the AWB module can be
moved at any time. Crossing 0 in either direction moves that ownership: 0 hands
AWB back to the ISP before standing the thread down, and a non-zero rate starts
the loop and takes ownership only if the mode is `auto`. Range is clamped to
[0, 30] — the loop sleeps `1000/hz` integer milliseconds, so a large enough rate
would round to a zero sleep and spin a core. Backends without the loop (Maruko,
whose AWB is driven by the SDK's own 3A) answer `501` rather than accept the
write and silently do nothing with it.

Related fix in the same area: `isp.awbMode=auto` handed AWB to the userspace
loop unconditionally, including when `awbFps=0` meant no loop was running. That
left the module owned by an algorithm that does not exist — strictly worse than
the non-converging internal one it replaced. Ownership now follows the loop.

The debug OSD `awb` row now reads the applied gains from the loop rather than
`MI_ISP_AWB_QueryInfo` — that reports the ISP-internal algorithm's last state,
which is frozen precisely because it is no longer running. In userspace mode the
row reads `r<R> b<B> usr#<n>`, where `n` counts applied corrections and is the
liveness signal (moving = tracking, steady = converged).

**Device-verified on .232** (SSC338Q / IMX335): from the frozen 2111/2250 the
loop converges to ~1128/3496 — within a few percent of both what the SDK's own
AWB reached (1165/3254) and what grey-world predicts from the hardware stats
(1104/3498). At **100 fps with NPU detection also running: 51% venc CPU**,
0 CMDQ/ISP-WQ errors. A forced `ct_manual` excursion to 2800 K and back is
recovered smoothly.

**Do not "fix" the `int[]`-vs-struct call.** `Cus3AEnable_t` is
`{MI_BOOL bAE, bAWB, bAF}` with `MI_BOOL = unsigned char`, so
`int p110[13] = {1,1,0}` reads as `bAE=1, bAWB=0, bAF=0` — the AWB flag lands in
padding. That is true but the accidental behaviour was benign, because
`CUS3A_Enable` means "hand this module to *custom* code", not "run this algo".
The struct is now passed correctly and `bAWB` is set deliberately.

## [0.55.0] - 2026-07-25

Debug OSD: an actual-bitrate row on both backends, and the Maruko-only
diagnostic rows brought across to Star6E.

**New `br` row (both backends).** Shows the encoder's real output rate against
its configured RC target, e.g. `br: 8123/10000k`. The rate is summed from the
encoder's own per-frame sizes (`star6e_scene_frame_size` /
`maruko_scene_frame_size`) over the OSD's existing 1 Hz window, **not** from
bytes handed to the transport — so it stays truthful when `output_enabled` is 0
and excludes packetization overhead. The gap between actual and target is the RC
undershoot/overshoot, which is also the fastest way to tell an encoder problem
from a link problem: an encoder tracking target while the picture stutters points
at the radio. Previously this number was only reachable via `[verbose]` log
lines (gated on `system.verbose`) or a subscribed sidecar probe.

**Star6E gains four rows Maruko already had.** Both backends share the
`src/debug_osd.c` renderer, but row *content* is written at each backend's own
call site, and Star6E's had fallen behind:

- `exp` — shutter µs, sensor/ISP gain, gain ceiling.
- `ae` — scene luma, AE target, and stable/adjusting/at-limit state.
- `awb` — R/B gains, colour temperature, stable state.
- `stab` / `sfil` — Kalman correction (`a`) and raw detector measurement (`m`)
  in stab pixels, plus pause state. Hidden when no stab thread runs.

New `star6e_controls_ae_osd_status()` reuses the AE/AWB query machinery already
in `star6e_controls.c` (`ae_diag_snapshot_collect`, the `AeExpoInfo_t` /
`IspExposureLimit` / `AwbQueryInfo_t` typedefs, the `MI_ISP_AWB_QueryInfo`
dlopen pattern) — no new SDK bindings. Shutter/gain fall back to
`MI_SNR_GetPlaneInfo` when the ISP AE query does not answer, which is what keeps
the row populated under CUS3A/userspace-3A. `star6e_framing_stab_osd_status()`
mirrors its Maruko counterpart. All of it refreshes at 1 Hz on the existing OSD
tick, never per frame — each AE/AWB refresh dlopens libmi_isp and round-trips
several MI_ISP getters.

**Two rows deliberately not ported.** Maruko's AR centre-crop `crop` row reads
`ctx->scl_crop_*`, an i6c-only feature with no Star6E counterpart (Star6E's
`crop` row remains the zoom-derived one). Star6E's `det` detection-box overlay
stays Star6E-only — it is fed by the i6e IPU.

No renderer changes were needed: `debug_osd_text()` self-clips against the canvas
height and paints its own per-row background, so extra rows need no panel resize
or row-count constant. The added rows are conditional on their data being valid,
so an idle box does not pay for all of them.

## [0.54.0] - 2026-07-25

Startup NPU scrub: every Star6E start runs a bare `MI_IPU_CreateDevice` +
`MI_IPU_DestroyDevice` cycle before pipeline bring-up (`star6e_ipu_scrub`,
measured 0.1–2.2 s depending on how cold the IPU firmware is, no-op when
`/dev/mi_ipu` is absent). This closes the residual ISP-CMDQ
wedge left open by 0.53.0, and fresh bench work on a healthy box narrowed that
wedge considerably:

- **The actual wedge condition** (reproduced 2/2, all other paths clean): the
  detector was started **at pipeline bring-up** (from config at init), later
  stopped **live**, and then a fork+exec respawn fired — the successor, which
  never touches the IPU, wedges at ISP init. A detector started *live* on a
  running pipeline does not poison the same sequence (1/2 sessions' worth of
  contrary lore came from a degraded box).
- **Not actually broken on a healthy box**: external stop/start with *any* gap
  (the "wait ~20 s" rule measured earlier was an artifact of accumulated wedge
  damage — kernel-side MMA chunks and the per-device proc entries drain within
  ~1-2 s of teardown); and a `MUT_RESTART` respawn while detection is *running*
  (the successor re-creates the IPU device from the carried config and
  reconciles the state).
- **Maruko/i6c**: setting `detect.enabled` now returns HTTP 501 (the live-apply
  group requires the backend's `apply_detect_reload` callback, which only
  Star6E registers). Intentional — previously the set was accepted and burned a
  full respawn on a backend that has no detector at all; an explicit refusal is
  more honest than a pointless restart.
- **Why the scrub works**: whatever state a predecessor's IPU teardown leaves
  behind is reset by the next IPU device create — from any process. Running the
  cycle before VIF/VPE/ISP bring-up makes every successor a reconciler
  regardless of its detect config. Verified 2/2 on the previously-wedging
  sequence, plus clean cold starts and live toggles. A scrub *after* the ISP
  has wedged does not recover it, hence startup placement, and unconditional —
  the poison survives process exit, so no flag from the previous instance can
  know whether it is needed.

## [0.53.0] - 2026-07-25

`detect.enabled` is applied live instead of by a `MUT_RESTART` respawn. The
respawn was leaving the successor pipeline permanently frameless on Star6E, and
no respawn-side fix is possible — the state is pinned by an fd the child cannot
close.

- **The wedge** — after `/api/v1/set?detect.enabled=false`, the fresh process
  never emitted a frame: `fps/live` 0, `framesSent` stuck, `waiting for encoder
  data...`. Its ISP CMDQ sits mid-`WAIT` on the `ISP_TRIG` event with a corrupt
  ring read pointer (`r=0x03000026` where a healthy ring reads `0x30000260`), so
  `FrameStartProc` can never claim buffer space — `MDrvCmdqWriteCommandMask cmdq
  buffer isn't available(0)` and then an endless `ISP_IRQ_WQ_FRAME_START add WQ
  error!` storm. VPE's input FIFO never drains (`EnsureInputPortFifoEmpty ... no
  response in 1000ms`, `mod7 dev0 pass0 infer timeout`), and the storm floods the
  console hard enough to make the box unresponsive. `S95waybeam restart` often
  did not clear it. Reproduced 3/3 on .232 (SSC338Q / IMX335).
- **Root cause: the inherited `/dev/mi_sys` fd**, not time and not port state.
  Eliminated on device, in order: a stale-enabled port1 or stale user depth (the
  dying process's `SetChnOutputPortDepth(0,0)` and `DisablePort(0,1)` both
  returned 0, and the successor found port1 already disabled); a 3 s settle
  inside the parent's teardown; post-exit settles of 4.5 s and 15 s in the
  respawn child; `cold_vif` plus 4 s; closing every inherited `/dev/mi_*` except
  `mi_sys` plus 8 s. An external stop/start — every fd closed at exit — is clean
  with a 20 s gap and wedges with 1 s. Closing `mi_sys` in the child is this
  SoC's confirmed deadlock, so **no fork+exec respawn can recover from a
  detect-active parent**. A successor that re-creates an IPU device reconciles
  the state, which is why detect on->on restarts and live model swaps never
  showed it.
- **Fix: `detect.enabled` is `MUT_LIVE`** (`venc_api.c`), routed through the
  existing `LIVE_GROUP_DETECT` / `apply_detect_reload` service, which now
  dispatches start / stop / reload from the committed value. No respawn happens
  on the transition at all, so the wedge cannot be reached — and toggling the
  detector no longer costs a ~25 s stream outage.
- **Start/stop policy factored out** (`star6e_pipeline_detect_start/_stop`) so
  the port1 arbiter claim and the framing-conflict refusal cannot drift between
  the initial bring-up and the live toggle. Both run on the pipeline thread, the
  same thread that reads the detect snapshot.
- **`copy_live_group_fields()` now copies `detect.enabled`** — omitting it
  silently turned the toggle into a same-state reload (the value never reached
  the committed config).
- **Teardown hygiene** — `iy_unload_graph()` releases the port1 user output
  depth it registered before disabling the port, and the pre-init teardown
  disables VPE port1 as well as port0. Neither caused the wedge; both are state
  this code registers and should release.

Known limitation: any *other* `MUT_RESTART` change made while detection is
running (e.g. `video0.size`) still respawns and still hits the wedge above.
Disable detection first, or restart the process externally. A general fix needs
the respawn to exec from a process forked before any `/dev/mi_*` is opened.

## [0.52.0] - 2026-07-25

Hot-swap the offline NPU detector `.img` without respawning the pipeline, so
the main video0 RTP stream is uninterrupted (no gap, no keyframe reset, no
reconnect) when only the model changes. Detection runs off the VPE0 port1 tap,
independent of the video0 encode path, so a detector-only reload is clean.

- **Detector-only reload entrypoint** (`star6e_ipu_yolo.c`) — the init is
  factored into `iy_load_graph()` / `iy_unload_graph()` halves (plugin dlopen,
  VPE port1 tap create, backend init) plus an `iy_stop_reader()` reader-join
  helper. New `star6e_ipu_yolo_reload(state, vcfg)` tears down and re-creates
  only the detector plugin + tap channel while the encoder keeps running.
- **Geometry guard** — the live path is taken only when `net_width`/`net_height`
  are unchanged; a dims change needs the VPE port recreated, so it falls back to
  the full `MUT_RESTART` respawn. The multiple-of-32 / min-64 checks are shared
  by start and reload via `iy_resolve_net_dims()`.
- **Model geometry is now verified against the tap** — plugin ABI bumped to `2`,
  adding a required `model_dims()` that reports the loaded `.img`'s REAL input
  geometry, plus `net_width`/`net_height` in `DetectBackendConfig`. The host
  compares the two and refuses a mismatch (`iy_check_model_dims()`), because
  config is not evidence of what a model expects. Since `model_path` is live but
  the dims are restart-scope, pointing `model_path` at a different-geometry
  model was previously accepted silently and left an "active" detector that
  never detected — the backend rejects every frame and `process()` errors are
  not logged. A refusal names both geometries and the exact `netWidth`/
  `netHeight` to set, leaves detection off, releases the port1 claim, and does
  not disturb the stream. ABI is an exact match (no compatibility window during
  beta), so an ABI-1 plugin is rejected at load with a clear diagnostic.
- **Swap cost, measured** — a model change does not respawn the pipeline, drop
  frames at the transport, force a keyframe, or reconnect, but it is not free:
  the NPU graph load runs on the pipeline thread (which is what makes it atomic
  against the per-frame snapshot), so frame output stalls for its duration. On
  Star6E .232 at 100 fps the stall was **~100-450 ms** on some runs and
  **~2.2-2.5 s** on others, with no in-between. The fast runs correlate with a
  freshly booted or freshly restarted process and the slow ones with a pipeline
  respawn having happened, but that did not hold in every trial (one
  init-script restart stayed slow, another came back fast), so the trigger is
  **not isolated** — it is not memory pressure, not file I/O (a pre-warmed
  `.img` reads in 0.01 s with the stall unchanged), and not accumulation across
  reloads (flat within a process). Treat **~2.5 s** as the planning number.
  Only a `model_path` change pays this; see the in-place path below.
- **Only a model change reloads the graph** — `model_id` is a label stamped into
  the snapshot and `conf_thresh` / `nms_iou` are decode-time knobs, yet all
  three shared `LIVE_GROUP_DETECT` and so rebuilt the NPU graph, paying the full
  stall to change a number. `star6e_ipu_yolo_reload()` now compares the
  requested `model_path` against what is loaded and, when it is unchanged,
  applies the label and thresholds in place — the graph and the tap are never
  torn down. Thresholds go through the new optional ABI-3 `set_thresholds()`
  with the reader briefly parked (it parks between frames) so the decode is not
  reading them as they are written; a backend without it falls back to the full
  reload. Measured on .232 in the slow regime: threshold-only **50 ms** and
  `model_id`-only **0 ms**, against ~2300 ms before.
- **Mutability wiring** (`venc_api.c`) — `detect.model_path` is now `MUT_LIVE`,
  and `detect.model_id` / `conf_thresh` / `nms_iou` are newly settable
  (`MUT_LIVE`); a new `LIVE_GROUP_DETECT` applies them via `apply_detect_reload`
  instead of setting `g_reinit`. `detect.net_width` / `net_height` are settable
  as `MUT_RESTART`. Added range validators for the four new fields.
- **Atomicity + concurrency** — the swap runs on the pipeline (encode) thread
  via a request posted by the HTTP apply hook and serviced between frames
  (`star6e_controls_service_detect_reload`), so it is atomic w.r.t. the
  per-frame `DETECT` snapshot query and the failure path (which frees the
  detector context) can never race a `snapshot()` read. The request carries a
  full `VencConfig` snapshot taken under `g_cfg_mutex`, and the pipeline thread
  reloads from a private copy of it — so the (potentially slow) NPU graph load
  never reads the live config lock-free, even if the HTTP wait times out or a
  second `/set` arrives. A `paused` flag quiesces the consumer across the swap;
  the `DETECT` sidecar trailer is simply absent for the duration of the swap
  (consumers already tolerate "no DETECT"). The wire `model_id` is latched into
  the detector snapshot alongside the boxes, so it flips in lockstep with the
  first new-model `DETECT` (never tagging the last old-model boxes with the new
  id), and the `describe()` class-count cross-check re-runs after each reload.
- Both `.img` files can be pre-staged (e.g. on SD); switching needs no
  `/api/v1/restart`. Changing `netWidth`/`netHeight` still takes the full
  respawn path (unchanged). No change to the RTP-sidecar wire ABI.
- Contract `0.14.0`; `test_venc_api` gains four detect live/restart/validation
  cases (2089/0); both backends build clean.

### VPE port-ownership arbiter + `vpe_taps` observability

Cross-feature alignment for the VPE0 scaler outputs, so the detector and the
stab framing tap cannot both program the single second scaler, and an operator
can see the allocation.

- **Arbiter** (`star6e_vpe_ports.{c,h}`) — a single owner for VPE0 **port1**
  (the lone second scaler output): stab XOR detect. `star6e_vpe_port1_claim()`
  refuses a second claim while it is held; the pipeline claims it for the stab
  motion tap and for the detector, so their mutual exclusion is now enforced by
  the arbiter instead of an ad-hoc `if (g_framing)`. `FramingModule` gains a
  `uses_vpe_port1` flag (stab=true; stab-fill=false — it drains port0 and
  composes in SW, so it takes no port1 tap but stays detect-exclusive by an
  explicit resource policy).
- **Observability** — the arbiter publishes a `runtime.vpe_taps` block to
  `/api/v1/config` via `venc_api_set_vpe_taps()`: `port0` lists the 1:N
  consumers of the shared main output (`main` always; `jpeg`/`record` when
  those channels are up — the JPEG snapshot is a port0 consumer, not a second
  tap), and `port1` names its sole owner or `null`. Star6E only; absent on
  Maruko.
- `test_star6e_vpe_ports` covers claim/refuse/release/idempotence and the
  published JSON (2103/0).

## [0.51.1] - 2026-07-24

Detector hardening follow-ups from the v0.51.0 upstream review — four
defensive fixes, no behavioural change on the normal path.

- **Idempotent `libmi_sys` load** (`star6e_ipu_yolo.c`) — the detector opened
  `libmi_sys.so` on every start and never closed it, so each SIGHUP reinit with
  `detect.enabled` bumped the dlopen refcount unboundedly. The handle is now
  cached in a file-static and opened once for the run.
- **Float→pixel clamp before the cast** (`star6e_runtime.c`) — the debug-OSD box
  path clamped only the low side before the `float`→`uint32_t` conversion, so a
  huge/`inf`/`NaN` edge from an out-of-contract plugin hit implementation-defined
  UB before the high clamp ran. New `osd_box_px()` helper does the full range
  clamp (negatives, `inf`, `NaN` → in-range) in float, then casts.
- **NaN-safe wire quantization** (`detect_wire.c`) — `norm_coord()` / `score_u8()`
  used `<=0` / `>=1` guards that a `NaN` slips through (all comparisons false),
  reaching an implementation-defined cast. Reordered to `!(x > 0)` so `NaN` maps
  to 0. Adds four `test_detect_wire` cases (negative / huge / `inf` / `NaN`
  coords and scores) asserting the wire output stays in range.
- **ipu_probe overflow guard** (`tools/ipu_probe.c`, dev-only) — the tensor
  element count is now checked against `size_t` multiply overflow and the
  source `aligned_buf_size` before `malloc`/dequant, so a malformed `.img` fed
  to the bring-up tool can no longer short-allocate or over-read.
- `make test-werror` / `test-asan` **2078/0**; cross-build clean both backends;
  `ipu-probe` clean under `-Wall -Wextra`.

## [0.51.0] - 2026-07-24

Detector tuning surface: expose the confidence/NMS thresholds, the tap
geometry, and the wire `model_id` as config (#191). These rode into the fork
under the 0.50.0 tag without their own bump; versioned here so the config is
documented and the version uniquely identifies the build.

- **`detect.confThresh` / `detect.nmsIou`** now reach the plugin. The plugin
  ABI has carried these since the boundary landed, but the host passed zeros so
  every backend ran on its built-in `0.40` / `0.45`. `0.40` is too high for the
  small-object workload — INT8 quantization costs ~30% of the FP32 score on
  small objects — and the ground tracker confirms weak detections over time, so
  running the detector at ~`0.20` and letting the tracker's 2-hit confirmation
  reject noise is the intended architecture. `<=0` = plugin default.
- **`detect.netWidth` / `detect.netHeight`** replace the hardcoded `640x352`
  VPE port1 tap. That geometry was inherited from the collaborator's VisDrone
  model, not chosen for this sensor, and is the single largest constraint on
  small-object recall. Validated at start: both dims must be multiples of 32
  (the head strides 8/16/32) and must match the compiled `.img` (the backend
  already rejects a mismatched frame). `0` -> `640` / `352`.
- **`detect.modelId`** puts a configurable class-table selector on the DETECT
  sidecar trailer (see `documentation/RTP_SIDECAR_PROTOCOL.md`). It was
  hardcoded to VisDrone, so a one-class SAR-person model announced itself as
  VisDrone-10 and every box came out labelled "pedestrian". `model_id` cannot
  be derived — the plugin loads whatever `.img` `detect.modelPath` names — so it
  is operator config, registered alongside `netWidth`/`netHeight`. The host now
  calls the plugin's `describe()` hook at start and warns when the reported
  class count contradicts the configured id; unknown ids skip the check (a
  private `model_id` is legitimate).
- All new members appended at the END of `VencConfigDetect` — the config ABI is
  append-only (SigmaStar ISP bin loading breaks if `VencConfig`'s layout
  shifts). Host tests **2074/0**; cross-build clean both backends.
- **Docs**: adds `documentation/RTP_SIDECAR_PROTOCOL.md` — a self-contained
  in-tree spec for the RTP sidecar wire format (FRAME base, trailer ordering,
  ENC_INFO/TRANSPORT_INFO/ATTITUDE/DETECT, and the `model_id` registry),
  tracking the canonical `include/rtp_sidecar.h`.

## [0.50.0] - 2026-07-23

Volatile config writes: `GET /api/v1/live/set`.

- **New endpoint `/api/v1/live/set`** — `/set`'s field surface applied to the
  running config only, no write to `/etc/waybeam.json`.  Built for
  high-cadence automated writers (waybeam-link adaptive bitrate / frame-cap /
  fps actuation, waybeam-link spec Pass 73): persist-on-set at controller
  cadence wears flash and reboots into the last adaptive transient.
- Live (MUT_LIVE) fields only; restart-required fields answer `400`
  ("restart-class field requires persistence; use /api/v1/set") because a
  pipeline reinit reloads from disk and would silently discard the value.
  Response shapes are byte-identical to `/set`; single- and multi-set both
  supported.
- A later persisting `/set` / `/defaults` snapshots the whole running config,
  volatile changes included (one config struct, by design).
- Builds without the endpoint 404 — clients probe once and fall back to the
  persisting `/set` (waybeam-link does exactly this).
- Contract version 0.12.1 -> 0.13.0 (non-breaking endpoint addition).

## [0.49.0] - 2026-07-19

WebUI exposure for the per-frame size caps.

- **`video0.maxIBytes` / `video0.maxPBytes` in the dashboard** — the 0.45.0
  frame-size caps were API-only; they now carry data-driven UI metadata
  (`FIELD_UI`) and render as a "Frame size caps" group from
  `/api/v1/capabilities`. No dashboard.html change — the group comes entirely
  from the capabilities feed, tooltips included (cap semantics, the
  framebits-first/bitrate-first RC-priority flip, IDR-on-apply).

## [0.48.0] - 2026-07-19

Star6E NPU object detection (#183): pluggable object detection on the idle
IPU, results streamed as sidecar metadata and drawn on the debug OSD.

- **Detector plugin boundary** (`include/detect_plugin.h`): the host owns the
  VPE port1 tap (model-input-size NV12, drop-not-block), the reader thread,
  config, and the carriers; a dlopen'd plugin `.so` (`detect.plugin`) owns the
  IPU device and the decode and returns `DetectBox[]`.  New model = config
  change, no venc rebuild; all model-specific code (models, decode, class
  tables, usage docs) lives with the plugin, out of this repo.
- **RTP sidecar DETECT trailer** (flag 0x10, appended last): per-frame
  detection metadata — 16 B header (model_id, schema_ver, count, detect_seq,
  payload_len, age_ms) + TLV body with normalized-u16 BOX records.  Attached
  every frame while a subscriber is present, for RF-loss resilience.  The
  sidecar send path was rebuilt around a 512 B datagram buffer with
  flag-ordered variable trailers (spec: coordination `protocols/rtp-sidecar.md`).
- **Debug-OSD box rendering** (`detect.osd`, needs `debug.showOsd`): the
  encode-thread OSD pass scales the published snapshot onto the canvas and
  draws palette-colored rects per class plus a `det N` row; snapshots older
  than 700 ms are not drawn.
- Reader publishes a mutex double-buffered snapshot (<= 64 boxes) consumed by
  both the sidecar and the OSD; lock held only for the copy, never across an
  IPU invoke.  Detection is best-effort — any bring-up failure logs and the
  stream continues without it.  `detect` config block is trailing/append-only;
  detect and `framing=stab` are mutually exclusive (both claim VPE port1).
- WebUI: new **Detection** dashboard section exposing the `detect.*` config
  fields (enabled, plugin, modelPath, firmwarePath, inferInterval, osd).
- New host tests: `tests/test_detect_wire.c` (39 cases) for the trailer
  builder; sidecar send-path call sites updated.


## [0.47.0] - 2026-07-18

Retire the Star6E `aeEngine=custom` userspace AE and remove the `custom` value
entirely (mirrors Maruko's 0.22.0 move).

- **`aeEngine=custom` is removed.**  The `cus3a` thread never did AE convergence —
  the ISP firmware/bin AE always drives it, in both engine modes.  Since 0.46.0
  made the thread enforce gain/shutter limits under `sdk` too, the only thing
  `custom` still did was run the fps-derived shutter cap and the cold-boot fps
  kick from the thread instead of the pipeline.  Star6E now always runs the single
  limits-only enforcer beside the firmware AE.  `isp.aeEngine` accepts only `sdk`;
  any other value (including a stale `custom` in an old config) warns and falls
  back to `sdk`, so existing configs still load.
- Behaviour for the default (`sdk`) path is unchanged: convergence, cold-boot fps
  recovery (pipeline `cap_exposure_for_fps` + `MI_SNR_SetFps` kicks, now
  unconditional), and gain/shutter min/max enforcement all behave exactly as in
  0.46.0.  The retirement does **not** adopt custom's continuous fps-derived
  shutter cap — the pipeline already caps at init and on bin reload.
- Removed the now-dead custom-only internals from `star6e_cus3a` (the
  `limits_only`/`sensor_fps` config fields, the `MI_SNR_SetFps` symbol + the
  frame-15 cold-boot kick, and `compute_max_shutter`'s fps fallback) and made the
  pipeline's three cold-boot `SetFps` kicks unconditional.  `start_custom_ae` →
  `start_ae_enforcer`, `star6e_pipeline_legacy_fps_rekick` →
  `star6e_pipeline_cold_boot_fps_rekick`.
- Dropped the vestigial `VencConfigIsp::legacy_ae` / `ae_mode` derived fields, the
  `MarukoConfig::ae_mode` mirror, and both backends' retirement-NOTE branches —
  `custom` no longer exists to record.
- `make verify` clean both backends; `test-werror` + `test-asan` **1979/0**.
  Device-verified on `.201` (IMX335): `sdk` enforcer clamps the live firmware AE
  (gainMax 5000→gain 5000, revert→30000); cold-boot fps holds 60.  `.201`
  restored to its 0.45.0 baseline.

## [0.46.0] - 2026-07-18

Manual minimum exposure / gain floors for the supervisory AE.

- **New `isp.gainMin` / `isp.shutterMinUs` live controls** — set an explicit
  minimum sensor gain and minimum exposure (µs) floor for the 3A, completing
  the manual min/max envelope alongside the existing `isp.gainMax` /
  `isp.shutterMaxUs` ceilings.  Both default `0` = "use the ISP bin's
  calibrated floor" (no override).
- The supervisory cus3a thread writes the floors into `minSensorGain` /
  `minShutterUs` of the ISP exposure limit on startup and re-enforces them
  each tick.  Each floor is clamped so it can never exceed its ceiling, and
  `isp.shutterRule180` (which pins `min==max`) takes precedence over a manual
  `shutterMin`.  Setting a floor back to `0` restores the ISP bin's calibrated
  floor (the thread captures `minSensorGain`/`minShutterUs` from the bin at
  startup and falls back to it, mirroring the ceiling's `bin_max_*` fallback).
  `MUT_LIVE`, both Star6E and Maruko backends.  Device-verified on `.201`
  (IMX335) and `.233` (IMX415): floors apply, clamp to ceiling, pin overrides
  shutter floor, and `0` restores the bin default.
- **Star6E: the supervisory thread now runs as a limits-only enforcer under the
  SDK firmware AE too** (`aeEngine=sdk`), so `gainMin/Max` + `shutterMin/Max`
  work without switching to the custom AE (Maruko already enforced in both
  modes).  The thread starts whenever `aeFps>0`; in `sdk`/`legacy_ae` it sets
  `limits_only` — it enforces only explicit user gain/shutter min/max (with the
  bin baseline restored on `0`) and skips the fps-derived shutter cap and the
  cold-boot fps kick, which the pipeline already owns in legacy mode.
  Device-verified on `.201` in `sdk`: all four knobs apply and fully revert, and
  a binding `gainMax` genuinely clamps the live firmware AE (long gain 20129 →
  5000 → 3000, luma tracks; recovers on revert) — the AE stays converged.
- camelCase aliases `isp.gainMin` / `isp.shutterMinUs`; added to the config
  schema, pretty-printer, JSON export, default configs, and HTTP API contract.

## [0.45.0] - 2026-07-18

Live per-frame I/P frame-size caps (MaxISize/MaxPSize) with RC priority.

- **New `video0.maxIBytes` / `video0.maxPBytes` live controls** — cap the
  per-frame encoded I-frame and P-frame size via the MI_VENC RC params
  `u32MaxISize` / `u32MaxPSize`. `MUT_LIVE`, applied atomically as a group;
  0 = unlimited (default).
- **RC priority switch** — when either cap is > 0, RC priority is set to
  `FRAMEBITS_FIRST` so the encoder treats the size cap as a hard ceiling;
  when both return to 0, `BITRATE_FIRST` is restored. An IDR is requested
  after each apply.
- **Dual-backend parity** — Star6E (2-arg `MI_VENC_SetRcPriority`) and
  Maruko (3-arg with VeDev), both loaded as optional dlsym symbols
  (NULL-safe). Mode-aware `GetRcParam` → set the correct RC union member
  (H265/H264 × CBR/VBR/AVBR) → `SetRcParam` → `SetRcPriority`.
- **`SetRcPriority` takes a POINTER to the enum** — `MI_VENC_SetRcPriority`'s
  last arg is `MI_VENC_RcPriority_e *peRcPriority`, not the value (verified
  against the i6e/i6c SDK headers). The initial implementation passed the
  enum value, so the SDK dereferenced `0x2` as a pointer and faulted the
  encoder pipeline (SCL teardown + "Sensor abnormal"). Both backends now
  pass the address of a temporary. Device-verified: Star6E .201 (P-cap 5619→
  1868 kbps at maxPBytes=2000, no crash) and Maruko .233 (priority=framebits,
  no crash).
- Config JSON: `maxIBytes` / `maxPBytes` camelCase load/save/render.
- Adds `specs/2026-07-17-capped-vbr-rc-mode/` (requirements + plan) for a
  follow-on capped-VBR profile.

## [0.44.0] - 2026-07-18

Live FPS change improvements and exposure-based FPS override.

- **Reduced live FPS change stream stall** — `apply_fps()` (Star6E) and
  `maruko_apply_fps()` now skip the VPE→VENC unbind/rebind when the
  requested FPS matches the currently delivered FPS (no-op early-out),
  and request an IDR frame immediately after a successful rebind so the
  decoder recovers without waiting for the next GOP boundary.
- **New `isp.shutterMaxUs` live control** — exposes the CUS3A supervisory
  thread's `shutter_max_us` as a `MUT_LIVE` API field.  Setting this
  above the frame period (e.g. 33333 µs at 60 fps bound) forces the
  sensor to skip frames, reducing effective output FPS proportionally
  without the ~0.5 s bind-rebind stall.  Bitrate scales with effective
  FPS.  0 = automatic (1/sensor_fps).  Both Star6E and Maruko backends
  supported.  Dashboard ISP section updated with the new control.
- Config JSON: `isp.shutterMaxUs` parsed and serialized (persists across
  restarts).  Pipeline startup applies a non-zero persisted value to the
  CUS3A config (subordinate to `shutterRule180` when both are set).

## [0.43.0] - 2026-07-15

Frame-SHM tagging for GDR and SVC-T enhance-layer frames.

- **New `VencFrameMeta` flag bits** — `VENC_FRAME_FLAG_GDR` (0x02) and
  `VENC_FRAME_FLAG_ENHANCE` (0x04) in `include/venc_frame_ring.h`. No
  struct size change; uses bits 1–2 of the existing `flags` byte. Old
  consumers ignore unknown bits — no ring version bump needed.
- **GDR cycle position** — the former `reserved` field is replaced by
  `gdr_pos` (0-based position in the refresh cycle) and `gdr_len` (cycle
  length in frames). The transport layer can use these to apply stronger
  FEC or ARQ near the end of the cycle where the refresh completes.
  Cycle length is derived at pipeline init from `ceil(total_ctu_rows /
  lines_per_frame)`. Counter resets on each IDR.
- **GDR tagging** — when intra refresh is active (resilience preset is not
  "off"), every non-IDR frame is tagged with `VENC_FRAME_FLAG_GDR` to
  indicate a rolling intra stripe is present. Both Star6E and Maruko
  backends track `gdr_active` on the output struct.
- **SVC-T enhance tagging** — when temporal scalability is configured
  (`ref_base > 0`), frames with `refType == ENHANCE_P_NOTFORREF` (the
  droppable top enhance layer) are tagged with `VENC_FRAME_FLAG_ENHANCE`.
  Both backends track `svct_active` on the output struct.
- `frame_shm_consumer_test` reports GDR and ENHANCE frame counts plus
  cycle length in both per-second interval output and the final summary.

Device-verification fixes (Star6E IMX335 .201):
- **Fixed: Star6E GDR/SVC-T fields zeroed by output reset.** The pipeline
  set `gdr_active`/`svct_active`/`gdr_cycle_len` before
  `bind_and_finalize_pipeline()`, which calls `star6e_output_init()` →
  `star6e_output_reset()` (a full `memset`), wiping them — so Star6E emitted
  zero GDR/ENHANCE tags. The assignment now runs after finalize.
- **Fixed: wrong `ENHANCE_P_NOTFORREF` refType constant (both backends).**
  It was `4` (the HiSilicon enum value); the SigmaStar i6e/i6c enum inserts
  `BASE_P_REFTOIDR` at index 1, making `ENHANCE_P_NOTFORREF` = `5` (value 4
  is `ENHANCE_P_REFBYENHANCE`, a referenced, non-droppable frame).
  Consolidated into one shared define per backend
  (`STAR6E_REFTYPE_ENHANCE_P_NOTFORREF` in `star6e.h`,
  `MARUKO_REFTYPE_ENHANCE_P_NOTFORREF` in `maruko_video.h`), which also
  corrects the pre-existing TRAIL_R→TRAIL_N error-resilience rewrite in
  `star6e_runtime.c` / `maruko_pipeline.c` that shared the same wrong value.
  Verified on i6e: 1:1 SVC-T → 50% frames tagged ENHANCE and rewritten to
  TRAIL_N; reference frames stay TRAIL_R.

## [0.42.1] - 2026-07-11

- **Frame-SHM wait wakeup hardening.** `venc_frame_ring_read_wait()` now re-checks
  the ring after publishing `consumer_waiting` and before entering the futex wait,
  closing a lost-wake race where a consumer could sleep even though a frame had
  already been committed. The standalone frame-SHM consumer now sizes its read
  buffer from the attached ring header instead of assuming 512 KiB slots.
- **Frame-SHM ring geometry tuned for realtime.** The `frame-shm://` ring now
  allocates 8 slots × 384 KiB (~3 MiB) instead of 16 × 512 KiB (~8 MiB) on both
  Star6E and Maruko backends. Observed ≤1080p IDRs are ≤28 KiB, so 384 KiB
  retains large headroom while a drop-not-block realtime ring needs far fewer
  than 16 in-flight slots. Consumers read the geometry from the ring header, so
  no consumer change is required.
## [0.42.0] - 2026-07-11

Full-frame SHM emissions — a new `frame-shm://` URI scheme that transfers
whole encoded video frames over POSIX shared memory, bypassing RTP
packetization entirely.

- **`venc_frame_ring` — SPSC lock-free ring buffer for whole frames.**
  New `include/venc_frame_ring.h` + `src/venc_frame_ring.c`. Same POSIX SHM /
  futex architecture as `venc_ring` but tuned for video frames: `uint32_t`
  slot lengths (frames can exceed 64 KB), 8-byte `VencFrameMeta` header
  (pts, codec, flags), and a staged write API (`begin_write`/`append`/
  `commit_write`/`abort_write`) that gathers scattered NAL data from the
  encoder stream directly into the ring slot — no staging buffer needed.
  Magic `0x5646524D` ("VFRM"), version 1.
- **`frame-shm://` output URI** — new `VENC_OUTPUT_URI_FRAME_SHM` transport
  on both Star6E and Maruko backends. Creates a 16-slot × 512 KB frame ring
  (~8 MB SHM region). The frame sender iterates the encoder stream packs,
  detects IDR frames from `h265Nalu` types 19/20, populates `VencFrameMeta`,
  and writes raw Annex B data (start codes preserved) so the consumer
  (waybeam-link) can apply its own framing/FEC at frame boundaries.
- Frame-SHM bypasses RTP entirely — no `MarukoRtpState`, no
  `star6e_hevc_rtp`, no sendmmsg batching. The output pressure observer
  reports frame-ring fill alongside the existing RTP-packet ring.
- `apply_server` rejects frame-SHM outputs (no live retarget for SHM).
- Transport type string reports `"frame-shm"` in both backends' controls.
- Unit tests: `tests/test_venc_frame_ring.c` — lifecycle, staged write,
  abort, bulk write, fill/drain, wraparound, concurrent producer/consumer,
  stride alignment, corrupt header/slot, overflow, init_complete, stats,
  fill observation.

## [0.41.0] - 2026-07-11

- **180° shutter rule toggle (`isp.shutterRule180`).** New boolean config
  field (default `false`, restart-required) that pins AE exposure to
  exactly 1/(2×fps) — `minShutterUs == maxShutterUs` in the ISP exposure
  limit, so the AE shutter dimension is locked while gain still
  auto-adjusts for brightness.  At 60 fps the shutter is fixed at
  8 333 µs (1/120 s).  Both Star6E and Maruko backends; the supervisory
  cus3a threads continuously enforce the pin so ISP bin reloads or
  cold-boot AE init cannot override it.  Contract `0.12.1`.

## [0.40.1] - 2026-07-11

Pre-upstream hardening (adversarial review of the #167–175 workstreams).

- **Attitude estimator: reject corrupt IMU samples.** A single NaN/Inf
  accel/gyro value used to poison the complementary filter permanently
  (the accel gate stays false so it never recovers) while
  `attitude_frame_update` still stamped `ATTITUDE.status = valid` — the
  HUD AHI would show a stuck, "valid", perfectly-level horizon. `wrap_pi`
  also spun forever on an Inf argument, hanging the encode thread.
  `attitude_est_update` now drops non-finite samples, `wrap_pi` guards
  non-finite, and the sidecar trailer is gated on a new
  `attitude_est_healthy()` so a non-finite state emits no trailer.
- **Level calibration works at any IMU ODR.** `calibrate_level` required
  256 samples but waited only ≤3 s, so at ODR ≤ ~85 Hz it always timed
  out. It now completes early at the sample target and otherwise accepts
  the ≤2 s window's samples once ≥ 32 are in (and the shorter window
  bounds how long the calibration holds the HTTP dispatch lock).
- **Maruko plain-stab: skip un-mapped tap frames.** The detector ran
  `IveShift` even when the tap copy was skipped (unmapped/short frame),
  correlating a stale or all-black buffer into a bogus shift and a
  one-frame crop jump; it now retries without advancing (parity with the
  stab-fill thread).
- **Fix `make test-werror`/`test-asan`/`test-tsan`.** `test_attitude_est`
  called `fabsf()` on `int16_t` returns → `-Werror=absolute-value`; the
  strict test targets did not build. Added NaN/Inf-hardening regression
  coverage.
- **Remove committed host binary.** `rtp_timing_probe` (an x86-64 build
  artifact) was tracked at the repo root; removed and gitignored.

## [0.40.0] - 2026-07-10

One-click attitude calibration + live attitude API (contract 0.12.0).

- **`GET /api/v1/attitude`** — live fused roll/pitch/yaw snapshot
  (camera frame, post-remap/trims). The estimator now runs whenever
  `attitude.enabled` + `imu.enabled` — no sidecar subscriber needed —
  so the WebUI readout works standalone.
- **`GET /api/v1/attitude/calibrate_level`** — hold the camera level:
  averages ~1.3 s of accel in the frame loop, solves the boresight
  trims exactly (`attitude_axis_map_solve_trims`, unit-tested), and
  persists `attitude.trimRollDeg`/`trimPitchDeg` via the standard
  restart-set path. 409 on no-IMU/moving, 501 on Maruko.
- **WebUI Attitude section**: live roll/pitch/yaw readout (1 Hz poll)
  and a "Capture level trims" button that calibrates, updates the
  fields, and restarts the pipeline.

## [0.39.1] - 2026-07-10

- **Sensor→camera axis remap** (`attitude.axisFwd` / `attitude.axisDown`,
  MUT_RESTART, defaults `+x`/`+z` = identity). A signed permutation
  applied to gyro+accel BEFORE the estimator, correcting boards mounted
  in any of the 24 axis-aligned orientations (the output-side
  `mountDeg`/invert trims only rotate about the camera axis and cannot
  fix a vertical board). Set from a two-pose bench calibration: axisDown
  = the sensor axis reading "down" with the camera level, axisFwd = the
  one reading "down" with the camera nose pointed at the floor. Invalid
  or parallel axes log once and fall back to identity.

## [0.39.0] - 2026-07-10

RTP sidecar: multi-subscriber sender + ATTITUDE trailer (cross-repo spec
`protocols/rtp-sidecar.md`, multi-telemetry HUD group 6).

- **Multi-subscriber sidecar.** The sender now keeps 4 subscriber slots
  keyed by addr:port with per-slot 5 s TTLs; MSG_FRAME fans out to every
  live slot and SYNC_RESP goes only to the requester. Removes the
  single-slot hijack where any MSG_SUBSCRIBE stole the feed from the
  wfb link_controller or ground pipeline-stats consumer.
- **ATTITUDE trailer (flag 0x08, 12 B).** Optional roll/pitch/yaw
  (int16 0.1°) + status + imu_age_ms appended last in flag order.
  Fed by a new complementary-filter estimator (`src/attitude_est.c`,
  unit-tested) running on the BMI270 ring in the Star6E frame loop;
  computed only when `attitude.enabled` AND a subscriber is live.
  Maruko builds carry the config but never emit the trailer (its IMU
  push path is not wired yet).
- **New `attitude` config section** (trailing struct, ABI append-only):
  `enabled`, `mountDeg` (0/90/180/270), `invertRoll`, `invertPitch` —
  all MUT_RESTART, full 7-touch (defaults/parse/pretty/JSON/fields/
  aliases/docs).
- `tools/rtp_timing_probe` decodes and prints the ATTITUDE trailer.

## [0.38.0] - 2026-07-09

Star6E sensor-driver mode quality: **fixed-framerate exposure policy + exact
nominal fps landing** for the in-tree IMX335/IMX415 drivers. Device-verified
on SSC338Q at 192.168.2.201 (IMX335).

- **IMX335: exposure clamp instead of VMAX extension.** The stock
  `SetAEUSecs`/`SetFPS` paths stretched VMAX whenever AE requested more than
  the frame budget — with AE pinned at the 1/144s shutter ceiling (any
  indoor scene) mode 5 ran at VMAX 1887 instead of 1880, delivering a
  brightness-dependent 142–143.5fps instead of 144. Both sites now clamp
  exposure to `vts - 9` (SHR floor); VMAX is pinned by construction.
  Verified: encoder `Fps_1s` 144.00 sustained, VMAX register at seed.
- **IMX335: empirical vts trims** — the real pixel clock runs ~0.2–0.5%
  below the K constant; modes 2/3/4/5 seeds trimmed (3016→3001, 2707→2701,
  2256→2250, 1880→1875) so delivered fps lands at/just above the label.
- **IMX335: orientation (flip) rewrites fixed for the new lineup** — the
  inherited M0F1/M1F1 block used stock indices: our idx 3 got the stock
  1080p AREA3 value and idx 4/5 got no flip writes at all. Extended using
  the verified `AREA3_ST_flipped = 4288 − normal` relation (idx3 3392,
  idx4 3248, idx5 3068 + cropped-mode OB values). Not yet camera-verified
  (bench runs flip=off).
- **IMX335: mode 2 label corrected** to 2560×1440@90 (row said 2400×1350,
  hardware reads out 2560×1440, Y_OUT=1460).
- **IMX415: same fixed-framerate exposure clamp** in `SetAEUSecs`.
- **IMX415: idx 1 retimed 4K@40 → 4K@~33.3** (VMAX 2250→2700 at HMAX=825):
  the 40fps probe sat within ~5% of both the ISP throughput wall and the
  analog HMAX floor; 33.3 keeps margin and still beats stock 4K@30 by ~11%.
  Retime not yet device-verified (no IMX415 on the bench).
- **Exact-CBR compensation for >120fps modes** (closes the deferred
  follow-up in the 2026-07-06 spec): the RC budgets at the 120 fpsNum cap
  while the bind delivers the true rate, so the wire ran ~1.19× the set
  bitrate at 144. The encoder budget is now scaled ×rc_fps/delivered at
  the three sites (pipeline create `venc_max_rate`, live `apply_bitrate`,
  re-applied on `apply_fps` when the factor changes). Device-verified:
  19.6 Mbps wire against a 20 Mbps config at Fps_1s 143.99 (was 23.8).

## [0.37.0] - 2026-07-09

Stabilization: **Maruko `video0.framing = stab-fill`** — full-FOV
stabilization (the whole frame floats on a moving black border, no crop-in) on
the Infinity6C, completing stab parity with Star6E. Device-verified on
SSC378QE at IMX415 1080×720@50 (visual + 5-cycle teardown soak, 0 MMU resets).

- **The Phase 5a "i6c VENC cannot be manually pushed" device result is
  OVERTURNED** — it was an ABI artifact. The i6c `MI_SYS_BufConf_t` differs
  from Star6E's in three load-bearing ways: `E_MI_SYS_BUFDATA_FRAME` is **1**
  (0 is `RAW`!), the struct carries `bDirectBuf`+`bCrcCheck` before the config
  union (union at offset 24, not 16), and `BufFrameConfig` has no embedded
  extra-conf. With the corrected layout, `MI_SYS_ChnInputPortGetBuf/PutBuf`
  straight into a `NORMAL_FRMBASE` VENC input **encodes at the full sensor
  rate** — so Maruko stab-fill uses the same manual-feed shape as Star6E; no
  SCL bridge or FRAME_BASE bind needed.
- Graph rewire (only when `framing == "stab-fill"`): SCL port0 → RAW +
  unbound, drained by the fill thread; VENC created `NORMAL` (frame-base)
  with `StartRecvPic` deferred to post-graph; no VENC ring pool. `off`/`zoom`/
  `stab` keep the zero-copy RING leg untouched. Requires the unbound VENC
  input port to be given `MI_SYS_SetChnInputPortFrc(USERINJECT, fps/fps)`.
- Fill loop (in `maruko_framing_stab.c`, second registered module sharing the
  detector/Kalman/`stab_accuracy` with `stab`): drain port0 → detect on the
  centre patch → Kalman → compose (shift + Y=16/UV=128 borders) via
  `MI_SYS_BufBlitPa`/`BufFillPa` (present on i6c, leading-SocId signatures) →
  push. Measured: detect 5.2 ms + compose 3.0 ms ≈ **5.8 ms/frame thread CPU**
  (~29% of the single A7 at 50 fps) at `stab_accuracy=low`.
- `stab_crop_pct` is the float/border budget (max_off = enc·(100−pct)/200 per
  side); `pause_stab` glide-home works unchanged.
- WebUI: `stab-fill` un-gated on Maruko (reverses the #165 disable); tooltip
  updated. `record.mode=dual` is refused under stab-fill (chn 1 is RING-fed —
  can't mix with a frame-base chn 0 on the one H26x device).
- The 5a probe's BufConf ABI is corrected in-tree so the archived bench now
  reports the true answer.

Reinit hardening (found by adversarial resolution/sensor-mode switch testing
with the stab presets active — ~40 API-driven reinit switches on `.233`):

- **Two-phase stab teardown (BOTH presets).** Joining the consumer thread of a
  user-drained SCL port (fill: port0; stab: the port-2 tap) while the camera
  still produces pins SCL/ISP working tasks, and the ISP→SCL REALTIME unbind's
  UNBOUNDED kernel flush then wedges in uninterruptible D-state
  (`MI_SYS_IMPL_FlushRealTimeOutputBuf`) — ending as a zombie process or a
  hardware-watchdog reset.  Device-reproduced from stab-fill AND plain stab on
  size/preset switches; retroactively explains the 2026-07-03 teardown hangs.
  Now: `framing_stop` flips the thread to drain-only (no IVE/compose, pure
  GetBuf/PutBuf) so consumption continues while the ports are disabled;
  teardown joins + sweeps the residue afterwards (`finish_stop`).  Cut the
  wedge incidence from ~1-in-2 (fill) to <1-in-20 across the switch barrage.
- **Teardown watchdog (roadmap item, now shipped).** The residual SDK race is
  GENERIC and pre-existing — control test: `framing=off` wedged on its first
  size-change reinit, no stab code in the path (matches the 2026-07-03 hangs
  and the factory binary wedging identically).  It cannot be closed from
  userspace ordering, so a watchdog armed at teardown entry forces
  `reboot(RB_AUTOBOOT)` after 12 s if teardown has not completed — a bounded,
  logged, self-recovering ~45 s reboot instead of open-ended D-state limbo
  (no sysrq on I6C; SIGKILL leaves an MI zombie).  Benefits every mode, not
  just the stab presets.
- **Live fps change under stab-fill fixed.** `maruko_apply_fps` re-bound
  SCL→VENC RING as its fps divider — on the frame-base manual-fed VENC that
  stalls the encoder dead (instant "no encoder data" abort, device-reproduced).
  In fill mode the divider is now the VENC input port's USERINJECT FRC
  (src:dst), applied live with no graph change.

## [0.36.0] - 2026-07-09

Stabilization: **`video0.stab_accuracy` — a shared high/medium/low detector
level, replacing the silent per-backend divergence.** The Shift_Detector
geometry (crop/box/pyramid/search) was hardcoded differently in each backend
(Star6E 384/256/3, Maruko 256/128/2) — invisible to users and impossible to
tune per sensor mode.

- New enum field `video0.stab_accuracy = auto | high | medium | low`, shared by
  both backends and by both stab presets (`stab` + `stab-fill`). One geometry
  table in `include/framing_stab_accuracy.h` — the two backends now resolve
  through it, so they cannot drift again.
  - **high** 384/256/3/96 (smoothest) · **medium** 320/192/3/80 (new middle
    step) · **low** 256/128/2/64 (cheapest). All keep margin = (crop−box)/2 =
    64px. The detector is NEON software, so the level is the CPU/quality lever.
- **`auto` (default) resolves per-backend** — high on Star6E, low on
  single-core Maruko — so an unset field reproduces each backend's previous
  behaviour exactly (zero regression on upgrade).
- Data-driven WebUI: the field carries `FieldUi` metadata (`control:"select"`),
  so the dashboard renders the dropdown straight from `/api/v1/capabilities` —
  no `dashboard.html` edit or webui-blob rebuild. Supported on both backends
  (no Maruko gate); MUT_RESTART.
- Tests: `tests/test_framing_stab_accuracy.c` pins the level table, the 64px
  margin invariant, per-backend `auto` resolution and lenient unknown→fallback;
  plus config load + API set/validate guards. Suite 1744/0.

## [0.35.0] - 2026-07-09

Maruko (Infinity6C): **`video0.framing = "stab"` — IVE stabilization on i6c,
at parity with Star6E.** The motion detector (`MI_IVE_Shift_Detector`) was
previously dead on Maruko (`MI_IVE_Create` failed); the root cause was a
userspace blob vintage mismatch, not a kernel bug.

- **Blob unblock:** swap **only** `libmi_ive.so` for the BSP uClibc build
  (md5 `d608368e`, ive tag `c6a1e30`), which does its IC-version check via
  `/dev/mstar_ive0` ioctls instead of raw-mmapping `/dev/mem` (which EINVALs
  on this firmware). Do **not** also swap `libmi_sys`/`libmi_common` — the BSP
  variants segfault on the musl rootfs. Delivered as a builder osdrv override
  (builder#24).
- **Cost model, stated honestly:** the detector is SigmaStar's NEON software
  vision lib (`Simd::Neon`), **not** HW-accelerated — ~17 ms/call on one A7.
  i6c is single-core, so the config is cheapened to 256 tap / 128 box / 2-level
  pyramid (core 100%→55%, keeps 50 fps) at a documented noise cost.
- **Shared Kalman:** the control law is extracted to `framing_kalman.{c,h}`,
  linked by both backends; the Star6E refactor onto it is proven bit-identical
  by `tests/test_framing_kalman.c`. Maruko stabilizes in the SCL-input domain
  (crop + center-tap share one surface — simpler than Star6E's precrop path).
- **Teardown:** the detector thread is joined **before** the tap port is
  disabled (avoids an MI_SYS MMU-callback storm → watchdog); verified clean
  across repeated start/stop cycles, zero MMU resets.
- **Knobs un-gated on Maruko:** `stab_crop_pct`, `recenter_speed`,
  `stab_kalman_q`, `stab_kalman_r`, `pause_stab` now validate/apply and the
  WebUI no longer greys them out. `pause_stab` glides the frame home.
- **`framing = "stab-fill"` is planned, not yet shipped** on Maruko — it hinges
  on whether the i6c VENC accepts manually pushed input frames (RING_DMA-fed
  today); see `specs/2026-07-08-maruko-stab/plan.md` Phase 5.

## [0.34.1] - 2026-07-09

Star6E IMX335: **true 144fps encode — decouple VENC delivery from the RC
fpsNum parameter.** Supersedes the earlier cap-to-120 approach.

- The i6e VENC encodes 143fps fine; the 120 ceiling is only on the
  rate-control `fpsNum` parameter — `_MI_VENC_VerifyFps` rejects RC fps > 120
  and silently resets it to 30, wrecking CBR (3000 kbps → ~15 Mbps at 143fps).
- Fix: deliver the **true** sensor rate to VENC (encodes 143) but cap only the
  RC `fpsNum` to `STAR6E_VENC_INPUT_FPS_MAX` (120) so `VerifyFps` never resets.
  `rc_fps=120` vs `143` delivered = ~1.19× CBR overshoot (QP normal regime)
  instead of 4.7×.
- `star6e_pipeline.c`: create-path `venc_fps` (RC) capped to 120; both
  `bind_dst` deliver the true rate; GOP from the capped RC fps (240).
  `star6e_controls.c`: `apply_fps()` bind dst = true fps,
  `apply_encoder_fps(rc_fps)`.
- Device-verified (fps=144): RC SrcFrmRate 120/1, actual Fps_1s 143.0, no
  VerifyFps reset, wire ~4080–4188 kbps. Optional follow-up: bitrate
  compensation ×120/143 for exact CBR.

## [0.34.0] - 2026-07-06

Star6E IMX415: **hide four modes from the SDK/WebUI enumeration while keeping
them compiled in the driver.**

- New `imx415_linear_visible[] = {1, 2, 4, 6, 8}` maps table indices to the
  SDK-enumerated list. The capability loop enumerates only those (so `num_res`
  = 5) and `pCus_SetVideoRes()` maps the SDK index back to the table index,
  re-asserting `ulcur_res`. All 9 mode tables and dispatch cases stay compiled
  — re-enabling a hidden mode is a one-line edit (add its table index back).
- **Visible (5, fps-ordered):** 0:3840×2160@40, 1:2816×1584@60, 2:1920×1080@90,
  3:1728×972@100, 4:1728×816@120.
- **Hidden (4, kept in driver):** table idx 0 (4K@30), 3 (3840×1152@60),
  5 (2304×1296@100), 7 (1472×816@120).
- **Note:** the SDK-enumerated indices shifted (now 0–4). Configs referencing
  the old 0–8 indices must be remapped — e.g. the bench box `/etc/waybeam.json`
  moved `"mode": 6` → `"mode": 3` to keep 1728×972@100. Device-verified on .13:
  enumerates 5 modes, mode 3 runs clean at 100fps, 0 drops.

## [0.33.0] - 2026-07-05

Star6E IMX415: **removed the two image-corrupt full-FOV binned modes** and settled
on a clean 9-mode fps-ordered lineup.

- Removed **1920×1080@100** and **1920×1080@120** (full-FOV 2×2-binned). They
  reported correct fps but rendered black + colored horizontal lines on the I6E
  ISP — to reach 100/120fps their HMAX drops to 328/275, below the wide binned
  line's WINMODE=0x04 crop floor (~365). Init tables, thunks, enum entries, mode
  rows and dispatch cases all deleted (no dead code).
- **Kept 1920×1080@90** (full-FOV 2×2-binned, idx4): at 90fps HMAX stays above the
  crop floor, so it renders a clean image — device-confirmed.
- Final lineup (idx : res@fps): 0:4K@30, 1:4K@40, 2:2816×1584@60, 3:3840×1152@60,
  4:1920×1080@90, 5:2304×1296@100, 6:1728×972@100, 7:1472×816@120, 8:1728×816@120.
  Every mode renders a valid image; still strict fps order.
- Docs (`STAR6E_IMX415_MODES.md`, `HEADROOM.md` §5.6) updated to match.

## [0.32.0] - 2026-07-05

Star6E IMX415: **strict fps ordering** of the full 11-mode lineup. No behaviour
change per mode — only the resolution-table order and dispatch case indices were
permuted so `sensor.mode` steps monotonically in frame rate.

New index map (old → new): 0→0, 1→2, 2→4, 3→5, 4→6, 5→8, 6→9, 7→3, 8→1, 9→7,
10→10. Resulting order (idx : res@fps): 0:4K@30, 1:4K@40, 2:2816×1584@60,
3:3840×1152@60, 4:1920×1080@90, 5:2304×1296@100, 6:1920×1080@100, 7:1728×972@100,
8:1472×816@120, 9:1920×1080@120, 10:1728×816@120.

- The `imx415_mipi_linear[]` rows and the `pCus_SetVideoRes` cases were reordered
  together; each case keeps its exact init table / vts_30fps / fps / line_period /
  data_prec. Verified: `.ko` mode strings now enumerate in fps-ascending order.
- The full-FOV binned corruption warning (idx4/6/9 in the new numbering) is
  restated in the driver header and docs.
- Doc tables (`STAR6E_IMX415_MODES.md`) re-sorted to match; narrative
  sub-sections retain original-order idx labels (flagged inline) and identify
  modes by resolution.

## [0.31.0] - 2026-07-05

Star6E IMX415: two **binned wide-crop** modes (idx9/idx10) that give the widest
2×2-binned FOV which renders *clean* at 100/120fps, plus the diagnosis of why
full-FOV 1920×1080 binned is corrupt on the I6E ISP.

- **1728×972@100fps 2×2-binned wide crop** (`Sensor_bc1728_100fps_init_table_
  4lane_linear`, idx9) — widest binned FOV (~80% of full-4K linear) that renders
  clean at 100fps. HMAX=365, VMAX=2034, 891 link. Image-verified on the display.
- **1728×816@120fps 2×2-binned wide crop** (`Sensor_bc1728_120fps_init_table_
  4lane_linear`, idx10) — same 1728 width at 120fps. Height capped at 816 because
  120fps forces HMAX=365/VMAX=1700 and a taller frame drops HMAX below the crop
  floor. 17% wider than the idx5 1472×816 crop. Image-verified.
- **Root-caused the black+colored-lines corruption**: the full-FOV 1920×1080
  binned modes (idx2/4/6) report correct fps but render garbage on the I6E ISP —
  the `WINMODE=0x04` crop path needs `HMAX ≥ ~365` for a wide binned line. A
  1728-wide crop clears this; the 1920-wide full-FOV readout does not (its
  reduced-HMAX≈308/328 falls below the floor → malformed readout). The binned
  wide-crops keep HMAX=365 and stay clean. idx2/4/6 retained only for
  stock-index compatibility; use idx9/idx10 for high-FOV binned video.
- Both new modes clone the device-proven idx5 1472×816 crop (`WINMODE=0x04` +
  explicit centered window) and widen it, changing only the window + VMAX/HMAX.

## [0.30.0] - 2026-07-05

Star6E IMX415: a **native 3840×2160@40fps** full-4K mode (idx8), appended
alongside the lineup (indices 0–7 unchanged), plus the resolution of the
ISP/CSI-clock investigation.

- **3840×2160@40fps full 4K** (`Sensor_8m_40fps_init_table_4lane_linear`, idx8)
  — native 4K at 40fps, +33% over the stock idx0 4K@30. Non-binned on the 1485
  link, full-height window (VST=0/VWIDTH=4320), VMAX=2250, HMAX=825 (332 MPix/s).
  Device-verified on `.13`: sensor=enqueue=delivered=40.8fps, 0 drops, 0
  steady-state FIFO-FULL.
- This is the **clean full-4K ceiling**. Bench probes established the two walls
  just above it: 4K@42 (HMAX=779, 352 MPix/s) hits the ISP throughput wall
  (FIFO-FULL, ~7% loss); 4K@45 (HMAX=733) breaches the sensor's analog HMAX
  floor (733<floor≤779) with a clean silent halve. Neither is clock-fixable.
- **ISP/CSI-clock lever — resolved as unavailable on i6e.** The Maruko/i6c 288MHz
  CSI-MAC lever does not port: this SoC's vendor CSI driver rejects
  `CUS_CSI_CLK_288M` (dmesg `[Drv_CSISetClk] Not supported CSI CLK 288000` →
  sensor never powers on). 216M is the hard CSI ceiling; the driver pins it for
  all modes with an explanatory comment. See
  `documentation/STAR6E_IMX415_HEADROOM.md` §5.3-5.4.

## [0.29.0] - 2026-07-05

Star6E IMX415: an **ultrawide 3840×1152@60fps** mode (idx7) — full sensor
*width* at 60fps, appended alongside the lineup (indices 0–6 unchanged).

- **3840×1152@60fps ultrawide** (`Sensor_uw_3840x1152_60_init_table_4lane_linear`,
  idx7) — 100% horizontal FOV, letterboxed to 1152 lines (3.33:1), non-binned
  on the 1485 link. Built on idx1's 1485 base with the window widened to full
  3840 and height cropped to 1152; HMAX=1022 (idx0's proven full-width line, so
  no analog-floor risk), VMAX=1211 → 60fps, which clears the vertical wall
  (1211 ≥ 1152 physical + vblank) and the ISP wall (265 MPix/s). Device-verified
  59.98fps, 0 drops, 0 FIFO-FULL over a 15 s soak (`incrop 0,0,3840,1152`).
  Height is HMAX-bounded to ~1160; a taller 3840×1296 (2.96:1) would need
  HMAX≈917 (below idx0's line, ISP at ~299 MPix/s) — a riskier stretch, left
  out. See `STAR6E_IMX415_HEADROOM.md`.

## [0.28.0] - 2026-07-05

Star6E IMX415: a **full-FOV 2×2-binned 1920×1080@120fps** mode (idx6), added
alongside the existing lineup (indices 0–5 unchanged).

- **1920×1080@120fps full-FOV binned** (`Sensor_2m_120fps_init_table_4lane_linear`,
  appended at idx6) — the full sensor FOV at 120fps (soft), a full-FOV
  alternative to the stock 1472×816@120 crop (idx5, kept). Same reduced-HMAX
  approach as idx4, one notch faster: HMAX=275 (line 3712 ns) with VMAX=2250,
  which sits just above the full-width binned analog HMAX floor (~250–275;
  HMAX=229 halves at 144fps). This is the practical full-FOV binned ceiling —
  the ISP (249 MPix/s) would allow more, but the sensor's line-readout floor
  caps it near 120–130fps. Device-verified 120.15fps, 0 drops, 0 FIFO-FULL over
  a 20 s soak. See `STAR6E_IMX415_HEADROOM.md` for the full model.

## [0.27.0] - 2026-07-05

Star6E IMX415: a **full-FOV 2×2-binned 1920×1080@100fps** mode, and a **widened
2816×1584@60fps** replacing the old 2560×1440. Both device-verified on .13.

- **1920×1080@100fps full-FOV binned** (`drivers/sensor_imx415_star6e.c`,
  `Sensor_2m_100fps_init_table_4lane_linear`, inserted fps-ordered at idx4) —
  the *full sensor FOV* at 100fps (soft/binned), complementing the sharp
  non-binned 2304×1296@100 (idx3) at the same fps. Device-verified 100.00 fps,
  0 drops over a 30 s soak, warm-switch clean both directions.
  The obstacle was the **binned vertical-timing wall**, not bandwidth or the ISP
  MPix ceiling: a binned readout's VMAX must cover the *physical* lines read
  (2×output_h = 2160) plus vblank, so at the stock binned HMAX=365 a 100fps
  frame caps VMAX at ~2023 < 2160 and the VIF silently delivers *exactly half*
  (~50 fps, DropCnt=0). Fix = the same reduced-HMAX trick as the non-binned
  modes: HMAX 365→**328** lets VMAX be 2250 (=2160+90 vblank, matching the stock
  90fps mode) at 100fps. Bit depth (10 vs 12bpp) and link (891 vs 1485) were
  both ruled out as red herrings before the timing wall was identified.
- **2816×1584@60fps** (idx1, widened from 2560×1440) — the stock 60fps table
  already reads a 2952×1656 window but venc center-cropped it to 2560×1440,
  discarding FOV. Widening the output to 2816×1584 (mode-table only, no sensor
  register change) lifts FOV area from ~44% to ~58% of the sensor. Held at
  ~2816-wide (267 MPix/s) rather than the full 2952 (293 MPix/s, startup
  FIFO-FULL) to leave ISP headroom for the OSD overlay. Verified 59.52 fps,
  0 drops, 0 steady-state FIFO-FULL.

## [0.26.0] - 2026-07-05

Star6E IMX415: a new **non-binned 2304×1296@100fps** window-crop mode, plus
warm-switch register safety. Follows the 0.25.0 in-tree Star6E drivers.

- **2304×1296@100fps non-binned** (`drivers/sensor_imx415_star6e.c`, inserted
  fps-ordered at idx3) — the widest 16:9 the I6E ISP sustains non-binned at
  100fps: 2.99 MPix / ~299 MPix/s, device-verified 99.0 fps + 0 drops over a
  30 s soak. Challenges the "must bin for FOV at high fps" assumption: native
  sharp resolution (35% FOV area) where the stock 90/120 modes are 2×2 binned.
  Enabled by (1) the 1485 Mbps link (`SYS_MODE=0x08`, reused from idx1's base,
  no venc changes — Star6E's REALTIME bind carries it), (2) a **reduced HMAX**
  (548) to beat the vertical-timing wall a fixed HMAX=652 would cap at ~89fps,
  and (3) the I6E ISP sustaining ~300 MPix/s. The wall was device-mapped:
  2304×1296 clean, 2432×1368 (333 MPix/s) drops, 2560×1440 halves.
- **Warm-switch register safety** — the SDK keeps sensor registers across a
  mode switch, and the stock non-binned tables (idx0/idx1) never wrote the
  binning registers, so a warm switch binned→non-binned (e.g. 90→30) left 2×2
  binning latched and corrupted the readout. The non-binned idx0/idx1 tables
  and the new crop now write `0x3020/21/22=0x00` + all-pixel DIG_CLP
  (`0x30D9=0x06`/`0x30DA=0x02`) explicitly in standby. Verified both
  directions, 0 drops (120→30, 90→100, 120→60, 90→30).

## [0.25.0] - 2026-07-05

In-tree Star6E (Infinity6E) sensor drivers for IMX335 and IMX415 — the Star6E
counterpart to the Maruko custom drivers. Previously Star6E had only prebuilt
stock `.ko`; now the mode lineups are owned in-repo and buildable via
`make drivers-star6e KSRC_STAR6E=<i6e-4.9.84-kernel>`. Both seeded from the
OpenIPC infinity6e blueprints, HDR/DOL removed (the two HDR handles are `NULL`
and the SEF handle made `static`, so the compiler dead-code-eliminates the
whole HDR subtree). Device-verified on SSC338Q @192.168.1.13, 0 sustained
drops on every mode.

- **IMX335 — fps-ordered lineup with two new higher-FOV window-crop tiers**
  (`drivers/sensor_imx335_star6e.c`, `documentation/STAR6E_IMX335_MODES.md`):
  2560×1920@30 / @60, 2400×1350@90, **2176×1224@100** (new crop, VIF 99.8),
  1920×1080@120, **1600×900@144** (new crop, VIF 143.3). The two crops push to
  the highest FOV the I6E ISP sustains — measured ceiling ≈2.66 MPix@100 /
  ≈1.44 MPix@144; over budget the ISP silently halves (0 fifo/skip logged), so
  the VIF `/proc` FPS column is the truth signal. The Maruko I6C ceiling model
  does not apply to I6E. No 50fps mode. `imx335_init_window_crop()` reuses the
  proven 120fps analog/PLL base and overrides only the readout window + HMAX,
  latching the geometry in standby (PR#156 discipline).
- **IMX415 — stock fps-ordered lineup** (`drivers/sensor_imx415_star6e.c`,
  `documentation/STAR6E_IMX415_MODES.md`): 3840×2160@30, 2560×1440@60,
  1920×1080@90, 1472×816@120. Stock tables verbatim; no crop tiers.
- **Build wiring** (`drivers/Makefile`, `Makefile`): `SOC=star6e` obj-m builds
  both sensor objects; `make drivers-star6e` stages
  `sensors/star6e/sensor_imx*_star6e.ko`.

Note: venc persists `sensor.mode` in `/etc/waybeam.json`. These drivers expose
fewer modes than the stock 11-mode IMX415 driver, so a persisted mode index
beyond the new range makes venc fail mode-select and exit on boot — patch the
config to a valid index when deploying over a box that ran the stock driver.

## [0.24.1] - 2026-07-04

Pre-upstream-squash cleanup: adversarial review of the 0.22.0–0.24.0 range
(PRs #157–#160) surfaced a handful of straggling defects, all fixed here.
No functional feature changes.

- **GOP frame count now tracks the *actual* encoder fps, not the committed
  request** (`src/venc_api.c`) — a live `video0.fps` above the current sensor
  mode's max is clamped to `sensor_fps` for the bind, but the GOP was computed
  from the unclamped value, stretching the I-frame interval (e.g. GOP for 144
  while the encoder is pinned at 100 → 1.44 s instead of 1 s). Now uses
  `query_live_fps()`; the 120→144 ceiling raise had widened this drift.
- **Star6E debug-OSD "enc" row no longer shows `0x0`** (`src/star6e_runtime.c`)
  — it printed the raw `video0.width/height` config (default `0/0` = auto)
  instead of the resolved `image_width/height`; Maruko already did it right.
- **AE pacer teardown join is now bounded** (`src/maruko_pipeline.c`) — the
  pacer's steady-state loop calls `CUS3A_RunOnce`, which can stall inside the
  SDK during an ISP fault; the previous unbounded `pthread_join` would then
  wedge teardown/respawn. Now `pthread_timedjoin_np` (300 ms) + detach-on-
  timeout. Device-verified: 0 spurious timeouts across mode-cycle teardowns.
- **Star6E fps rebind restores on encoder-apply failure**
  (`src/star6e_controls.c`) — a post-bind encoder/scene fps write failure no
  longer leaves VPE→VENC bound at the new fps while the caller rolls back.
- **CPU% debug-OSD window corrected to the documented ~1 s**
  (`src/debug_osd.c`, `OSD_CPU_RING` 3→2 — was a 1.5 s span).
- **Removed the retired `throttle_mode` AE controller** (`src/maruko_cus3a.c`,
  `include/maruko_cus3a.h`) — the 0.9.12 "aeEngine=custom" log-domain IIR
  controller and its `MarukoAeResult`/`SetAeParam` plumbing were dead
  (`throttle_mode` hard-wired off since paced native 3A superseded it). The
  supervisory thread is now cleanly limits-only. Device-verified on I6C: native
  AE still enforces `isp.gainMax`, reads stats, adapts exposure, and tears down
  clean across mode cycles.
- **Upstream hygiene**: dropped `.serena/` tooling config from tracking (now
  gitignored) and four transient handoff/investigation docs.

## [0.24.0] - 2026-07-04

IMX335 gets a **144fps ultra-low-latency mode** + a `WAYBEAM_NO_3A` debug
toggle. Full mode detail: `documentation/MARUKO_IMX335_MODES.md`.

- **New mode 5 = 1536x864@144 (16:9)** — device-verified clean end-to-end on
  SSC378QE + IMX335: SCL 144.0 fps, VENC 143.4 fps, **0 DropCnt / 0 FIFO-FULL /
  0 Skip-IQ**. Reuses the proven 120fps analog window (HMAX=275, `imx335_geo_
  1536x864`), VMAX paced to 144 (`vts_30fps=1875`). The lowest-latency mode in
  the lineup (6.94 ms frame period).

- **Why 1536x864 and not larger** — 144fps is bound by TWO independent ISP
  walls, both device-proven: (a) **bandwidth** ~274 MPix/s — 1920x1080@144 =
  298 MPix/s overflows the ISP P0 FIFO and collapses to ~26 fps; (b) **per-frame
  time** T ≈ 1.7 ms fixed + 3.65 ns/px vs the 6.94 ms frame period — 1600x900
  (1.44 MP, ~6.96 ms) misses by ~1% and Skip-IQ stalls. Both limits are at the
  ISP *input*, so an output/SCL downscale rescues neither; the sensor readout
  window itself must shrink. 1536x864 (1.33 MP) sits under both.

- **`WAYBEAM_NO_3A=1` env var** (`src/maruko_pipeline.c`) — freezes 3A entirely
  (`CUS3A_SetRunMode(OFF)`, no pacer/supervisory) for bench diagnosis. Used to
  prove the fixed per-frame ISP cost is **not** reclaimable by disabling 3A
  (1600x900@144 still stalls with 3A frozen — the ~1.7 ms is vendor ISP pixel
  processing, not 3A). Exposure is static when set; bench use only.

- **Live `video0.fps` accepts up to 144** (`PIPELINE_LIVE_FPS_MAX`,
  `include/pipeline_common.h`; `maruko_apply_fps`, star6e `apply_fps`) — the
  live-apply path previously hard-rejected any `fps > 120`, making it
  impossible to *pre-stage* fps=144 while parked in a lower-fps mode (so the
  next respawn's auto sensor-select could pick the 144 mode). The request is no
  longer rejected: the requested value is committed to config, and each
  platform's existing clamp-to-`sensor_fps` caps the actual VPE→VENC rebind to
  the current mode's max — so a 100fps mode still binds at 100, but the config
  now carries 144 for `sensor.mode:-1` to resolve on the next mode switch
  (`sensor_mode_clamp_fps` re-clamps there too). 144 is the ceiling because it
  is the highest fps any mode offers; above it is still a client error.

## [0.23.0] - 2026-07-04

Maruko IMX335 best-per-fps mode lineup + debug-OSD readouts. Full mode
detail: `documentation/MARUKO_IMX335_MODES.md`.

- **IMX335 gets a 5-mode best-per-fps lineup**, porting the IMX415 method
  to the 4:3 5MP sensor. One mode per fps tier, native 4:3 aspect for
  0–3, 16:9 low-latency hero at 100 fps:

  | idx | resolution | fps | aspect | role |
  |---|---|---|---|---|
  | 0 | 2592x1944 | 30 | 4:3 | full-res all-pixel (best IQ / full FOV) |
  | 1 | 2496x1872 | 50 | 4:3 | center crop |
  | 2 | 2272x1704 | 60 | 4:3 | center crop |
  | 3 | 1792x1344 | 90 | 4:3 | center crop |
  | 4 | 1920x1080 | 100 | 16:9 | low-latency hero (REALTIME) |

  All device-verified on SSC378QE + IMX335. **Window mode works on I6C** —
  the old "windowed readout hangs the ISP" claim was a stale-register
  artifact. Each crop writes its full readout geometry
  (HTRIM/HNUM/Y_OUT/AREA3/HMAX) explicitly in standby
  (`imx335_init_window_crop`), reusing the proven 120fps analog config;
  geometry is derived exact from the 1920x1080 window mode. Crop sizes sit
  ~5–8% under the ISP throughput ceiling (~245–274 MPix/s); all bind
  REALTIME for minimum latency.
- **BREAKING `sensor.mode` remap** — dropped the two redundant 16:9
  1920x1080@60/@90 modes (60/90 are now the higher-res 4:3 crops) and
  renumbered. Pinned `sensor.mode` configs must be updated; `sensor.mode:
  -1` (auto) resolves by target dims and needs no change.
- **Cold-boot enum wedge documented**: repeated `reboot -f` after a hung
  teardown can leave IMX335 i2c enumeration wedged (`QueryResCount → 0`,
  every mode fails "not available on pad", mode-independent). Recovery is a
  power-cycle; graceful `reboot` avoids it. Not a geometry bug.
- **Debug OSD gains sensor + encode readouts** (both Maruko and Star6E):
  two fixed rows below `cpu` — `snr <WxH>@<fps> m<idx>` (sensor readout +
  mode index) and `enc <WxH> <codec>` — so a tester can read the live mode
  straight off the overlay. Inserted at rows 2–3; the AE/AWB/exp block
  shifts down, nothing clobbered.

## [0.22.0] - 2026-07-04

Maruko AE rework: **paced native 3A** replaces both historical AE engines.
Full investigation: `documentation/MARUKO_CUS3A_INJECT_HANDOFF.md`.

- **Maruko now runs ONE AE mode**: the vendor AE+AWB converge at full rate
  for ~3 s after pipeline start, then the CUS3A per-frame auto-run is
  paused (`CUS3A_SetRunMode(OFF)`) and a pacer thread re-runs the same
  converged algo via `CUS3A_RunOnce` at `sensor_fps/3` (floor 30 Hz).
  Applies keep flowing through the stock ISP-API agent path.
  Device-measured @1080p100: 70.0% of the core (old `sdk` full-rate) /
  52.8% (old `custom` throttle) → **39.4%**, at full vendor image quality
  (exposure + AWB convergence verified live, below majestic's 42.9%).
- **`isp.aeEngine` is retired on Maruko** (still honored on Star6E). The
  key still parses so existing configs load; `custom` logs a notice and
  behaves as paced. The 0.9.12 no-op AE adaptor + P1 throttle controller
  path is removed. `isp.aeFps` keeps governing only the supervisory
  limits thread (Star6E semantics unchanged).
- Pacer rate is auto-derived — no user knob: 100 fps → 33 Hz,
  90 fps → 30 Hz, ≤90 fps → 30 Hz floor (full-rate quality at low fps).
- **`isp.gainMax` fixes** (found live-testing gain headroom): (1) the
  supervisory limits thread now compares against a fresh
  `GetExposureLimit` read each tick instead of a local cache — the CUS3A
  AE init was silently resetting limits to bin values right after the
  startup push, so a config `gainMax` above the bin ceiling never stuck;
  (2) the live `isp.gainMax` API setter now routes through the
  supervisory target (`maruko_cus3a_set_gain_max`) instead of writing
  the raw limit — writing `0` ("bin default") used to push a literal
  0 limit and slam the image black.  Verified live on-device:
  32000 → sgain rises past the bin's 8192 (to the algo's internal
  ~11470 cap), 2048 → clamps down, 0 → returns to bin default 8192.
- Findings for posterity: CUS3A INJECT run-mode makes the agent mid-layer
  silently drop every exposure apply (do not revisit); `CUS3A_RunOnceEn`
  only arms algo selection while `CUS3A_RunOnce` executes synchronously in
  the caller; `MI_ISP_RegisterIspApiAgent` is pure userspace fp tables.
- **Debug OSD: smoothed CPU% + live AE/AWB readouts.** The CPU% readout
  is now a sliding ~1 s average (snapshot ring at 500 ms cadence) instead
  of a jumpy 500 ms delta; the duplicated per-platform `/proc/stat`
  sampler in `debug_osd.c` is folded into one shared implementation
  (both Star6E and Maruko). Maruko additionally gains three OSD rows,
  refreshed at 1 Hz, to watch the paced 3A adapt live:
  `exp` (shutter µs, sensor gain/limit, ISP gain), `ae` (measured luma
  vs scene target, stable/adj/bound state, pacer Hz), and `awb`
  (R/B gains, color temp, stable/adj) — backed by a new
  `maruko_controls_ae_osd_status()` reusing the `/api/v1/ae/info` +
  `/api/v1/awb/info` SDK queries.
- **Review-pass hardening** (adversarial multi-agent review of the above):
  (1) the `ae` OSD row is now gated on a distinct `ae_info_valid` flag so
  it no longer renders fabricated zeros when only the sensor-plane query
  answers (the `exp` row keeps its sensor-plane fallback); (2) the AE
  pacer now requires all three CUS3A symbols (`RunOnceEn`/`RunOnce`/
  `SetRunMode`) before engaging — a missing `RunOnceEn` falls back to
  vendor full-rate instead of pausing auto-run and ticking an unarmed
  `RunOnce` (frozen 3A); (3) the pacer's 3 s convergence wait is now an
  interruptible 50 ms poll of `g_inj_run`, so a teardown/respawn in the
  first 3 s joins in ≤50 ms instead of blocking the whole window — matters
  on this restart-latency-sensitive SoC. Device-verified: pacer
  re-derives 33 Hz on live switch to 1080p100, both OSD rows show valid
  data, ~48% busy with OSD active.

## [0.21.0] - 2026-07-03

Maruko IMX415 mode-lineup rework: one best mode per FPS tier, all non-binned
and ~16:9 (sensor native aspect). See `documentation/MARUKO_IMX415_1485_MODES.md`.

- **BREAKING: `sensor.mode` index remap** (Maruko IMX415). New lineup:
  0 = `3760x2116@30fps` (891, REALTIME), 1 = `2952x1656@50fps_1485`,
  2 = `2688x1512@60fps_1485` (NEW — exact 16:9, replaces 2952x1368 19.4:9),
  3 = `2112x1184@90fps_1485`, 4 = `1920x1080@100fps_1485` (NEW).
  Old→new: 0→0, 5→1, 7→3. Configs with a pinned mode index must be updated;
  `sensor.mode: -1` (auto) resolves correctly unchanged.
- **New 100 fps tier replaces the vendor 120 fps mode**: `1920x1080@100_1485`
  is non-binned, exact 16:9, and sized ~8% under the ISP m2m ceiling
  (capacity ~108 fps) so the FRAMEBASE queue stays empty — minimum latency
  at full nominal rate. The old binned 1472x816@120 never delivered 120
  (115–118 measured): its HMAX=365 table bursts 393 MPix/s, above the
  384 MPix/s ISP REALTIME drain.
- **Vendor superwide + binned modes unsurfaced** (superwide 3760x1024@59,
  binned 1080p60-ispsafe, binned 1080p90, 1472x816@120): tables parked
  under `#if 0` in the driver for posterity; all suffered either non-16:9
  geometry or the 393 MPix/s FIFO-pressure defect.
- CSI-MAC clock selection now keyed on a per-mode `link_mbps` field instead
  of a hardcoded index threshold (renumber-safe; the FRAMEBASE bind was
  already keyed on the `_1485` name suffix).
- **`video0.size` width alignment relaxed /16 → /8.** The former rule
  (#63/#55, derived from an 854×480 failure that is only ÷2) needlessly
  rejected native ÷8 widths like 2952. Mode 1's `2952x1656` may now be set
  explicitly instead of only via `auto`; forcing the nearest /16 (2944)
  anamorphically downscaled 2952→2944 and cost ~7 fps (43 vs 50, device-
  verified). Height /8 unchanged. HEVC's conformance window covers the
  sub-CTU remainder, so /8 is the correct minimum.

## [0.20.0] - 2026-07-03

Maruko (Infinity6C/SSC378QE) IMX415 1485 Mbps non-binned sensor modes, plus
the operational fixes discovered while bringing them up on hardware. Full
platform notes in `documentation/MARUKO_IMX415_1485_MODES.md`.

- **Three new sensor modes** (indexes 5–7): `2952x1656@50fps_1485` (1:1 5MP),
  `2952x1368@60fps_1485` (ISP-ceiling max @60), `2112x1184@90fps_1485`
  (ISP-ceiling max @90). Requires CSI-MAC clock 288 MHz (set in `poweron`
  for mode index ≥ 5) and a mode-conditional VIF→ISP bind: FRAMEBASE for
  `_1485` modes (REALTIME overflows the ISP input FIFO at 594 MPix/s line
  bursts), REALTIME retained for 891 modes (minimum latency).
- **Sensor register-state hardening**: 1485 and non-binned 891 tables now
  write the readout-mode registers explicitly (`0x3020/21/22`, `0x30D9/DA`)
  — the IMX415 latches binning across teardown and warm reboot, which
  previously wedged every warm binned→1485 switch until a power-cycle.
  Vendor 1485 table's `0x3032=0x00` dark-image bug fixed (`0x01`).
- **Mode 2 (1920x1080@60 binned) frame-rate fix**: the ISP-safe HMAX
  rework (HMAX=1100) could not deliver 60 fps at all — in 2x2 binning
  VMAX counts physical lines (~2250 minimum for 1080p output), which at
  a 14.8 µs line is 30 fps; the stored `vts_30fps=2250` shipped exactly
  that under a 60 fps label. Now HMAX=550 (7.407 µs line): VMAX=2250 →
  device-measured 60.0 fps, line burst 259 MPix/s (safely under the
  384 MPix/s ISP drain that FIFO-FULLed the vendor HMAX=365 table).
- **Teardown drain**: poll SCL/ISP output-port task counts (worst row
  across all ports) before unbind so the kernel's unbounded REALTIME/RING
  flush isn't entered with in-flight tasks that can no longer complete
  (D-state hang observed after long runs; drain is bounded + advisory).
- **Honest keep_aspect on Maruko**: true width crops are impossible on the
  I6C camera path (SCL output crop must match ring stride; RDMA input crop
  needs a FRAMEBASE producer bind that mi_sys refuses on ISP→SCL) — wide →
  narrow-AR now anamorphically squeezes full sensor width with a one-line
  startup notice (>2% width delta), height crops still honored, AE zoom
  crop normalized against full ISP dims, zoom windows confined to the
  keep_aspect framing surface. Star6E is unaffected (VIF capture-window
  crop remains a real crop there).

## [0.19.0] - 2026-07-02

Remove the SDK VENC frame-lost strategy entirely — config field, live plumbing,
and the `MI_VENC_SetFrameLostStrategy` dlopen bindings on both backends.

Device testing on Star6E (SSC338Q i6e, .13) showed the feature never did its
advertised job: as a bandwidth throttle it is inert — a `frameLostThreshold`
set to ⅛ of the CBR target dropped **zero** frames over 10 s of steady
streaming — and the `E_MI_VENC_FRMLOST_PSKIP` placeholder-frame variant is
rejected by the driver with `E_MI_ERR_NOT_SUPPORT` (declared in the SDK header
but unimplemented in `libmi_venc`).  In practice it only acted as an I-frame
overshoot guard, a role the CBR rate controller already fills.  The real
backpressure levers are `video0.bitrate` (smooth, quality) and `video0.fps`
(coarse, temporal — a genuine frame-skip via VPE→VENC bind decimation).

- Drop `video0.frameLost` (`frame_lost`) from `VencConfig`, the API field
  table, aliases, defaults, JSON load/render/serialize, and both default
  configs.
- Remove `star6e_controls_apply_frame_lost_threshold`,
  `pipeline_common_frame_lost_threshold`, the boot/dual frame-lost blocks in
  both pipelines, and the `frame_lost` params threaded through
  `*_pipeline_start_dual` / `venc_api_dual_register`.
- Remove the `MI_VENC_{Set,Get}FrameLostStrategy` bindings, macros, and
  `MI_VENC_ParamFrameLost_t`/`MI_VENC_FrameLostMode_e` decls from the star6e
  and maruko SDK shims.
- Contract `0.10.1` → `0.11.0` (breaking: field removed); in-binary
  `/api/v1/version` bumped to match.
- Supersedes the abandoned PR #153 (which had tried to expose the PSKIP mode
  and make the strategy live-tunable).

## [0.18.1] - 2026-06-24

Expose the mDNS `discovery` config section through the HTTP API and WebUI. The
fields shipped in 0.18.0 (`discovery.enabled`, `discovery.serviceType`,
`discovery.name`, `discovery.bareAlias`) were wired into the config
loader/serializer but never registered in the API field table, so they were
reachable only by hand-editing `/etc/waybeam.json` — invisible to
`/api/v1/set`, `/api/v1/capabilities`, and the dashboard.

- Register the four discovery fields in `g_fields[]` (all `restart_required`:
  the beacon reads config at boot / re-reads on SIGHUP-respawn, with no live
  re-announce path), plus camelCase aliases for `serviceType`/`bareAlias`.
- Add a **Discovery** section to the WebUI settings, with per-field tooltips
  (`web/dashboard.html`; `src/venc_webui.c` regenerated).

No protocol or beacon behavior change — purely makes the existing discovery
config settable at runtime and visible to operators.

## [0.18.0] - 2026-06-16

Add an mDNS device beacon (discovery migration, Phase 1). waybeam_venc now
announces itself as a `_waybeam-venc._tcp.local` service on the multicast
group 224.0.0.251:5353, so ground stations and Android clients can discover
the encoder directly — independent of the optional waybeam-hub, which is the
only thing that previously advertised the vehicle.

**Announce-only, self-contained, off the hot path.** The beacon runs on its
own thread (`src/mdns_beacon.c`), started from `main()` and torn down (with a
multicast goodbye) before any SIGHUP-respawn exec. It responds to PTR queries
for its own service type and re-announces periodically; it does **not**
discover peers or feed any trust layer. The RFC 6762/6763 wire codec lives in
`src/mdns_wire.c` (`MDNS_WIRE_VERSION 1`) — the source of truth that
waybeam-hub will vendor; keep both in sync. The wire codec and multicast
socket handling are derived from
[OpenIPC herald](https://github.com/OpenIPC/firmware/tree/master/general/package/herald)
(MIT), the compact mDNS/DNS-SD stack for the OpenIPC project.

TXT is kept **deliberately minimal**: the service type `_waybeam-venc._tcp`
is itself the recognition signal ("an OpenIPC camera running the waybeam
encoder"), so TXT carries only `proto` (wire-schema version) and `version`
(waybeam version). The hostname/IP/port travel in the standard SRV/A/instance
records; live state (bitrate/fps/mode) is never advertised. The sidecar port,
backend/SoC, codec, full serial, and hub presence are left for the consumer to
fetch with one `GET /api/v1/config` after discovery — the beacon announces an
always-true device fact, capabilities are queried on demand.

**Serial-suffix naming.** The instance/host name is `waybeam-<suffix>.local`,
where `<suffix>` is the tail of the SigmaStar SoC **die ID** read natively
from RIU registers via `/dev/mem` (`src/device_id.c`, method lifted from
OpenIPC `ipctool` and device-verified against `ipcinfo -i`). This gives every
Star6E a stable, collision-free name with no RFC 6762 rename churn. SoCs with
no die ID (ssc37x / Maruko, verified) fall back to `discovery.name` or bare
`waybeam`. The full 12-hex die ID is the fleet key, exposed read-only at
`GET /api/v1/config` → `data.device.serial`.

**Bare `waybeam.local` alias.** Because most setups run a single vehicle, the
beacon also claims the bare host name `waybeam.local` (config
`discovery.bareAlias`, default true) so it can be reached as
`http://waybeam.local` without the suffix. It publishes A records for both
names and answers `A` queries for either. If a second waybeam device contests
`waybeam.local`, the conflict is resolved on the existing RX socket by an
RFC 6762 §8.2 IP tiebreak (higher IP keeps it; the lower IP yields with a
goodbye and keeps only its unique suffixed name) — no flapping, every device
still reachable by `waybeam-<suffix>.local`.

New `discovery` config section (`enabled` default true, `serviceType`,
`name`, `bareAlias`). The beacon is inert when disabled, when no usable
interface exists, or if the socket can't bind — it never blocks the encode
path.

Design: `documentation/DISCOVERY_TRUST_MIGRATION_SPEC.md`.

## [0.17.1] - 2026-06-13

Fix dual-record mode never engaging under runtime control — the SD-card
recording bitrate is now independent of the live stream bitrate, as documented.

**Dual-VENC channel creation is gated on `record.mode`, not `record.enabled`.**
Previously the secondary (ch1) record channel was created at pipeline init only
when `record.enabled` was true. A runtime-control client (e.g. RubyFPV) that
sets `record.enabled=false` and starts recording later via
`/api/v1/record/start` never got the ch1 channel: the record path fell back to
mirror mode and captured ch0 (the `video0` stream bitrate, typically ~5–12 Mbps
adaptive) instead of ch1 (`record.bitrate`, e.g. 40 Mbps). Recordings silently
tracked the link bitrate and the documented "stream low / record high" dual mode
was unreachable outside the auto-start-at-boot path.

The channel topology now follows `record.mode` alone; `record.enabled` still
controls only whether recording auto-starts at boot. The ch1 TS writer already
no-ops while the recorder is closed, so an idle ch1 (mode=dual, not yet
recording) writes nothing until `/api/v1/record/start` opens the file. Star6E
only (Maruko has no TS recording).

## [0.17.0] - 2026-06-12

Dropped HEVC RTP Aggregation Packets (AP, NAL type 48) entirely — the wire
output is now single-NAL + FU-A only, on both backends.

**Why.** AP is valid RFC 7798, but it bought waybeam almost nothing in the
FPV profile and actively hurt resilience. In single-slice H.265 the picture
is one large VCL NAL that goes out as FU-A regardless; the only NALs small
enough to aggregate were the VPS/SPS/PPS triplet on each IDR. Bundling those
three into one packet means a single lost datagram takes out the entire
parameter set and renders the whole GOP undecodable — the worst thing to
aggregate on a lossy RF link. It also broke receivers that only implement
the universally-supported single-NAL + FU-A subset (e.g. majestic-tuned RTP
depacketizers), which forwarded the type-48 packets as corrupt video. The
benefit was ~2 fewer packets per keyframe; the cost was interop failures and
reduced loss tolerance. Net negative for this use case, so it's gone rather
than gated behind a config flag.

**What changed.**
- `hevc_rtp` no longer has an `HevcApBuilder`. `hevc_rtp_send_nal()` and
  `hevc_rtp_prepend_param_sets()` drop their aggregation-builder argument;
  each NAL is emitted immediately as a single-NAL packet or FU-A fragments.
  VPS/SPS/PPS are now prepended as separate packets on IDR (majestic-style).
- `HevcRtpStats` loses `ap_packets` / `ap_nals`; the `[pktzr]` verbose line
  is now `nals N | rtp N | fill N B | single N | fu N` on both backends.
- `h26x_util_hevc_get_layer_id()` / `h26x_util_hevc_get_tid_plus1()` removed —
  their only consumer was the deleted AP-header builder.
- `test_hevc_rtp` and `test_star6e_hevc_rtp` rewritten to assert that **no
  type-48 packet is ever emitted** (VPS+SPS+PPS+IDR now arrive as four
  separate datagrams), plus the existing single/FU-A/marker-bit coverage.

## [0.16.0] - 2026-06-06

Sensor-level image orientation — `image.flip` and `image.mirror` now work
correctly on both backends.

**`image.flip` / `image.mirror` are applied at the sensor, once at bring-up.**
Previously orientation rode the VPE digital flip (`MI_VPE_SetChannelParam`
`mirror`/`flip`), which is unreliable on several sensor combos — on IMX335
(Star6E) `flip` stalled the encoder outright. Orientation now programs the
sensor's own register via `MI_SNR_SetOrien`, applied **once** at bring-up
(`sensor_select` before `MI_SNR_Enable`, and `start_vpe`/`maruko_start_vpe`
before the VPE channel starts); the VPE digital flip stays disabled. The
sensor driver only rewrites orientation when we set it, so a single apply
holds for the life of the stream — device-verified on IMX335 (Star6E) and
IMX415 (Maruko), `flip` + `mirror` both correct and stable.

A one-shot re-apply follows the discrete `MI_SNR_SetFps` events that can
reprogram sensor timing (Star6E live ISP-bin reload; Maruko cold-boot fps
kick). There is **no** per-frame re-apply loop and **no** use of
`MI_SNR_GetOrien` (it reads stale values under AE I2C load on IMX335).

## [0.15.0] - 2026-06-06

Unified stabilization control law — `stab` and `stab-fill` now behave
identically, and the feel knobs were replaced with the Kalman Q/R.

**`stab` adopts the `stab-fill` Kalman trajectory smoother.**  Previously the two
presets ran different control laws — `stab` used an EMA + gated return-to-centre
(driven by `stabSmoothPct`/`StillFrames`/`EdgePct`/`MotionThresh`), `stab-fill`
used a Kalman trajectory filter — so identical settings produced different
return-to-centre behaviour.  The detector now runs ONE law (the Kalman) for both;
the only per-preset difference is the emit step (HW crop reprogram vs software
compose).  Same settings → same feel.

**Feel knobs replaced by the Kalman tuning.**  The four inert-after-unification
knobs (`stabSmoothPct`, `stabStillFrames`, `stabEdgePct`, `stabMotionThresh`)
were removed and replaced with the two knobs the Kalman law actually exposes,
shared by both presets:
- `stabKalmanQ` (FT_DOUBLE, default 0.03, range `0.001..1.0`) — pan response
  (process noise): higher = follows pans sooner / weaker hold.
- `stabKalmanR` (FT_DOUBLE, default 2.0, range `0.1..50.0`) — smoothness
  (measurement noise): higher = smoother but laggier.

`stabRecenterSpeed` is retained but now only sets the `pauseStab` glide-home
rate (the Kalman recentres during normal operation).  Schema migration is
graceful: old `stab*` keys in a saved config are ignored on load; absent
`stabKalman*` keys take the preset defaults.  Both new fields render in the
data-driven Stabilization WebUI section.

## [0.14.0] - 2026-06-06

Live stab pause for the HW-crop `stab` preset + a fully data-driven
**Stabilization** WebUI section.

**`pauseStab` now works on `framing=stab`, not just `stab-fill`.**  The D13
software-ramp pause was previously gated to the fill path (its `set_live` hook
was wired only on the `stab-fill` module; toggling it under `framing=stab`
returned `-1` → a WebUI error toast).  The pause branch is now mode-agnostic in
the detector: when paused it undoes the tick's measurement and decays the
trajectory toward centre, so HW-crop glides the crop window home (via the smooth
low-pass → `apply_port_crop`) and fill glides the floating image home — same
software ramp, no HW rebind on either.  `set_live` is wired on both stab
vtables; `star6e_stab_set_paused` now guards on `g_stab_running` (any stab
detector) instead of `g_stab_fill_mode`.

**Stabilization section is now data-driven.**  The six persisted stab knobs
(`stabCropPct`, `stabRecenterSpeed`, `stabSmoothPct`, `stabStillFrames`,
`stabEdgePct`, `stabMotionThresh`) carry `FIELD_UI` metadata and were removed
from the static Video section; together with the runtime-only `pauseStab` they
render as one collapsible **Stabilization** group from `/api/v1/capabilities`
— no per-field `dashboard.html` edit or webui-blob rebuild.  The four advanced
feel knobs (smooth/still/edge/motion), previously API-only, now have WebUI
controls.  Ranges mirror the API validators; `0 = preset default` where noted.

## [0.13.0] - 2026-06-06

Framing-module refactor + the `stab-fill` floating-image stabilization preset
(PR #136 reworked onto master), plus a data-driven field schema so module
fields reach the WebUI without a dashboard rebuild.

**Stabilization is now a registered `FramingModule` (Star6E).**  All `stab`
code moved out of `star6e_pipeline.c` into `src/star6e_framing_stab.c` behind a
`FramingModule` vtable, gated by a `STAB` build flag (default 1; `make build
STAB=0` excludes the module entirely — no stab code linked, `framing` falls to
`off`).  The legacy single-thread manual-drain fallback is gone; on port1-tap
failure the HW-crop path degrades to a bound static centre crop.  Behavior of
`framing="stab"` is unchanged (bench-verified on imx335).

**New preset `framing="stab-fill"` — floating image on a black border.**  A
second registered module sharing the IVE detector/geometry/Kalman with `stab`.
Instead of HW-cropping, it SCL-downscales the full sensor frame to the encode
size (no shrink) and the manual-drain compose shifts a window inside it,
black-filling the exposed edge (`BufFillPa` strips + `BufBlitPa` content) on a
threaded blit worker decoupled from IVE via a 2-slot ring.  `stabCropPct` sets
the max shift / black-border budget.  A Kalman trajectory smoother (folded
preset constants) replaces the `stab` EMA.  Device-verified 60 fps on imx335.

**Live `video0.pauseStab` (stab-fill only).**  Software ramp: glides the
applied offset back to centre via the recenter decay — no `MI_VENC_StopRecvPic`
/ `BindChnPort2` live rebind (the maneuver that wedged the SoC), no thread
teardown.  `MUT_LIVE`, not persisted; routed module-side through the
`FramingModule` set_live hook.

**`stabCropPct` hardened to [60,100]** for stab presets (API validator floor
50→60, plus load-time + module clamp).  Self-heals a stale `stabCropPct=0`
(saved while `framing=off`, then framing re-set via a string-only edit) that
previously overrode the preset default and silently disabled stabilization.

**Data-driven field schema.**  `FieldDesc` gains an optional `ui` descriptor
(group/label/control/range/options/tooltip); `/api/v1/capabilities` emits a
per-field `ui` block when present, and the dashboard renders those fields
generically — a module field is WebUI-visible with no `dashboard.html` edit or
webui-blob rebuild.  `pauseStab` ships via this path; `stab-fill` added to the
framing dropdown.  Core fields keep the static dashboard schema unchanged.

**OSD note (Star6E).**  The venc debug OSD composites on VPE port0 (pre-stab),
so under stab-fill it rides the content — a known cosmetic limit of this
vehicle-local dev overlay (off by default).  For a screen-fixed OSD on the
stabilized stream use waybeam_hub's `osd_render` (composites at the SCL stage,
post-shift).  The two share the single global `MI_RGN` device and are mutually
exclusive — run one or the other.

**Tooling.**  `tools/build_webui.py` now pins the gzip OS header byte so the
embedded blob is byte-identical regardless of the builder's Python version
(CPython 3.11+ changed it from 0x03 to 0xff), fixing `make webui-check` /
`make verify` on modern Python.

## [0.12.0] - 2026-05-20

Digital image stabilization (DIS) on the Star6E VPE pipeline, re-grafted
cleanly on top of the 0.11.0 zoom work (#120) rather than the original
stabilization branch (#118), which forked before the pan-ramp/AE-meter
changes landed and could no longer cherry-pick clean.

**Framing mode — one knob for stabilization and zoom.**  A single
`video0.framing` preset is the sole user-facing knob for what the VPE crop
does (resilience-style): `off`, the stabilization presets `low`|`medium`|
`high` (Star6E only), and the digital-zoom presets `zoom-1.25x`|`zoom-1.50x`|
`zoom-1.75x`|`zoom-2x` (both backends).  It expands into the derived
stab crop/recenter or zoom crop fraction — mutually exclusive — and replaces
the old standalone `stab` and continuous `zoomPct` fields, neither of which
is part of the JSON schema or HTTP API anymore.  Zoom presets shrink the
crop + encoded output together (1:1, no SCL upscale; e.g. `zoom-2x` of
1920×1080 → 960×528).  `zoomX`/`zoomY` remain live and pan the crop in every
mode.  `video0.framing` is `MUT_RESTART`.

**Stabilization data path (Star6E).**  Preferred path (HW-crop): VPE port0 hardware-crops the stab
window — `MI_VPE_SetPortCrop` per detection — straight into a VENC **bind**
(zero-copy, no per-frame blit), while a tiny 256×256 port1 tap feeds
`MI_IVE_Shift_Detector` for motion estimation.  Decoupling the detector
from the stream lets ch0 run at full sensor rate (90/120 fps at 1080p
confirmed).  The original single-port manual-drain + `MI_SYS_BufBlitPa`
path is retained as an **automatic fallback** if a BSP rejects the
simultaneous port1.  Binding port0→VENC also fixes the long-standing
teardown wedge (the legacy un-drained full-res port0 queue that wedged
`[vpe0_P0_MAIN]` into D-state on restart).  Return-to-center decays the
offset *vector* in a float accumulator → a straight diagonal back to
center with no per-axis rounding tail.  When enabled the source is clamped
to ≤1920×1080 (preserve aspect) to avoid the high-res fps regression; the
encoded resolution then shrinks to the crop fraction of the (clamped)
source, reported in SPS/PPS.  Only the live ch0 stream is stabilized; JPEG
snapshots and the debug OSD see the unstabilized frame.  Dual recording
(`record.mode` dual/dual-stream) is downgraded to single-channel while
stab is active (both would consume VPE port0) — it records the stabilized
ch0 at its bitrate, with a warning.  `video0.stab` is `MUT_RESTART`.

**Interplay with 0.11.0 zoom.**  Stabilization and zoom are now mutually
exclusive *by construction* — a `framing` preset is either one or the other,
so the old "zoomPct ignored while stab on" warning is gone.  `zoomX`/`zoomY`
are honoured in both modes: under a stab preset they pan the stabilized crop
via `star6e_stab_set_pan()`; under a zoom preset they drive the 0.11.0
pan-ramp path.  `apply_zoom` short-circuits to the stab pan when the stab
thread is running.

**Size-change reboot fixed (cold-init VIF/VPE on respawn).**  A
`video0.size` change crosses a sensor-mode boundary; the fork+exec respawn
keeps `/dev/mi_*` fds open (the PR#117/#120 deadlock fix), so the inherited
VIF/VPE fds pinned the *old* mode and the fresh process wedged
`vpe0_P0_MAIN` re-initing to the new mode.  The runtime now detects a
size change (the only field that changes the sensor mode) and the fd-scrub
additionally closes `/dev/mi_vif` + `/dev/mi_vpe` so they re-init cold —
gated so it never runs on same-mode respawns (resilience/framing), which
stay deadlock-safe.  Device-verified clean in both directions
(1920↔2560) on a non-degraded device; closing those two fds does not
deadlock.  (Heavy back-to-back respawn cycling can still hit the
pre-existing cumulative SoC-state degradation that needs a power cycle —
that is independent of this fix.)

**Cold-boot fps re-kick (legacy AE).**  On a cold boot the init-time
`MI_SNR_SetFps` can fire before the ISP bin's AE settles and leave the
sensor timing register below the configured fps.  The CUS3A path already
re-kicks at frame 15; legacy AE now gets an equivalent one-shot re-kick
~1.5s into the run loop.  The CUS3A frame-15 kick was also decoupled from
the shutter-above-cap gate so it fires reliably.

**AE meter follows the crop in both modes.**  The zoom-aware AE meter
(`MI_ISP_CUS3A_SetAECropSize`) now also tracks the stabilized crop window,
so exposure is metered on the framed view rather than the full sensor —
the only intended runtime difference between the two modes is the pan ramp
(smooth under zoom, direct under stab) plus the stab loop's small per-frame
correction.  The SDK emit/readiness/dedup/disable machinery is shared
(`star6e_emit_ae_crop`); the stab window is sized as the crop fraction of
the VPE output (`g_stab_enc / g_stab_src`), not `image_width / precrop` as
the zoom path uses, because stab crops the VPE output rather than precrop.

**Debug OSD under stabilization.**  The OSD attaches at the full source
dim (port0 is never cropped on the stab path) and its stats panel offset
tracks the live crop window per-frame via
`star6e_pipeline_stab_panel_anchor()`, so the panel stays put in the
encoded view as corrections shift the crop.

**IMU-gyro readiness.**  The motion estimate is purely optical today, but
the design leaves a clean seam for gyro fusion: the existing BMI270 driver
now routes its frame-synced samples into a shared `ImuRing` (replacing the
discard stub), and the per-frame estimate lives in
`star6e_stab_estimate_shift()` with `star6e_stab_gyro_window()` supplying
the frame-aligned angular rates for the interval.  Adding gyro-assisted
stabilization is then just the math: integrate yaw/pitch to pixels via the
lens focal length and fuse with the optical shift.  The gyro window is read
and surfaced in the periodic stab diagnostic so the plumbing and
frame-sync are live, not speculative.

## [0.11.0] - 2026-05-19

Star6E zoom improvements lifted from the DIVP/stabilization branch
work — no DIVP pipeline swap, no stabilization, just two additive
features that make the existing `zoom_pct` + live `zoomX`/`zoomY`
controls feel better — plus a respawn-handler bug fix that makes
the `zoom_pct` MUT_RESTART path safe to use at runtime.

**Pan ramping (hardcoded 150 ms).**  Live pan is now smoothed via
exponential decay: setting `zoomX`/`zoomY` records a *target*, and a
dedicated ramp thread tweens the current crop position toward it
(Star6E ~60 Hz on `MI_VPE_SetPortCrop`, Maruko ~30 Hz on
`MI_SCL_SetPortConfig`).  Time constant is hardcoded — `zoomRampMs`
was briefly a config field but the surface didn't earn its keep.

**Zoom-aware AE meter.**  Whenever zoom is active, the ISP's AE
statistics window (`MI_ISP_CUS3A_SetAECropSize`, 0..1023 normalized)
is constrained to the zoom rect.  Sky-into-the-frame and
bright-light-source-near-edge no longer crush the zoomed subject;
pan across light↔dark regions and exposure follows.  Silent no-op
if the SDK lacks the symbol; gated on CUS3A readiness so the
boot-time WARNING from calling before the ISP channel is up no
longer fires.

**Respawn fd-scrub fix (root cause for SoC hang on MUT_RESTART).**
The `venc_respawn` post-fork fd scrub loop unconditionally closed
every non-stdio file descriptor before `execv`.  On Star6E this
included `/dev/mi_sys` (and other `/dev/mi_*` SDK fds), whose
release handler in the SigmaStar driver hangs uninterruptibly
when called after `MI_SYS_Exit` returned cleanly — which is the
normal state after a zoom_pct (or any MUT_RESTART) reinit.
Userspace hang → watchdog 3s SIGKILL → sysrq-b emergency reboot.

The fix: detect /dev/mi_* targets via readlink and skip closing
them.  Reintroduces the slow 1-fd-per-respawn leak the scrub was
originally written to plug, but bounded by RLIMIT_NOFILE (~1024
respawns) — vastly preferable to wedging the device.  Bench-
validated: 4 back-to-back zoom_pct toggles complete cleanly with
no watchdog escalation.  This fixes a class of bug shared by
zoom_pct, resilience preset, sensor mode, and any other
MUT_RESTART field; resilience and sensor-mode just happened not to
trip it as reliably (their teardown timing left mi_sys in a
slightly different state).

Star6E only.  Maruko `apply_zoom` accepts the new `ramp_ms` argument
for API parity but ignores it; no Maruko pipeline changes.

## [0.10.16] - 2026-05-15

Two new OSD-safe resilience presets for ultra-low recovery latency:

- `rescue` — 0.25 s GOP, no intra-refresh, no SVC-T.  Pure IDR-spam
  fallback.  ~35–40 % of the bitstream is IDR data, but the recovery
  floor is the lowest of any preset (next IDR is never more than
  250 ms away).  Useful as a spec-compliant baseline when
  A/B-debugging whether an intra-refresh preset is misbehaving in
  the field.
- `sprint` — 0.5 s GOP + `fast` (150 ms) intra-refresh, no SVC-T.
  Combines the stripe-recovery of `racing` with a guaranteed IDR
  floor every 500 ms.  ~20–25 % IDR overhead.  Pick over `racing`
  when you have bitrate headroom and want belt-and-suspenders
  recovery on close-range / line-of-sight links.

Both join the OSD-safe column (no green smear).  No code-path
changes — just two new entries in the resilience preset table.

## [0.10.15] - 2026-05-15

Both backends: resilience SETs now persist their new value to
`/etc/waybeam.json` and return `{"reboot_required": true}` rather than
reinitialising the encoder in-place.

Empirically confirmed on Maruko (192.168.2.12) that in-process pipeline
reinit also crashes the SDK kernel module — not always, but reliably
within a small number of transitions.  A controlled 7-transition sweep
on the Feb 22 (pre-gate) binary worked for 6 transitions, then on the
7th (range→fpv) the daemon zombied with a kernel page fault inside
`MI_SYS_IMPL_FlushInputPortTasks` in the `mi` module:

    do_task_dead ← do_exit ← die ← __do_kernel_fault ← do_page_fault
        ← do_DataAbort ← __dabt_svc ← CamOsTimerModify
        ← MI_SYS_IMPL_FlushInputPortTasks [mi]

System stayed alive (ICMP OK) but waybeam process became State=Z and
did not respawn (no init supervisor).  SIGHUP did not recover; reboot
was required.

**Why.**  The SigmaStar VENC SDK does not cleanly release kernel
encoder driver state across live reinit cycles when intra-refresh or
refPred toggles, even with fork+exec respawn and a 500 ms post-exit
settle.  Bench testing (Star6E, 192.168.1.13) reproduced two failure
modes:

- **Cross-group transitions** (refPred on↔off, intra-refresh on↔off)
  crash on the first transition.  The original gate in 0.10.14b
  caught these.
- **In-group transitions** (e.g. `endurance` → `patrol`, both
  `ref_base = 0`, intra-refresh active) succeed once or twice then
  crash.  The second sweep wedged the device after racing → endurance
  (success) → patrol (SoC panic, ICMP dies, power cycle required).
  No combination of settle delay or partial state reset prevented the
  cumulative kernel state corruption.

Cold-boot into any preset is 100 % reliable.  Shipping the reboot
model is the conservative, no-surprises choice — users edit config or
issue a SET, the daemon writes the change, the user reboots, and the
new preset takes effect.

**Implementation.**

- `src/venc_api.c`: extended the `resilience_change` detector in
  `process_restart_set_query()` to fire on changes to any of
  `resilience`, `intra_refresh_mode/lines/qp`, or
  `ref_base/enhance/pred`.  When a change matches, the new config is
  persisted to disk, `venc_api_request_reinit()` is **not** called,
  and the response carries `reboot_required: true`.  `gop_size` is
  intentionally NOT gated — it has always been live-changeable as a
  plain MUT_RESTART field and preset switches are caught by the
  `resilience` name change already.
- README documents the reboot-required behaviour next to the field
  table.

## [0.10.14] - 2026-05-15

Three new resilience presets — `endurance`, `patrol`, `rally` — and a
revised classification along an OSD-safe / OSD-unsafe axis after
bench-isolating why `range`/`fpv` leave persistent green smear on the
OSD panel.

**Root cause (two layers):**

1. **SVC-T TRAIL_N effective wavefront math.**  The temporal-layering
   rewrite marks `ref_enhance` of every `(ref_enhance + 1)` frames as
   TRAIL_N (display-only, dropped from the decoder's DPB), so the
   effective wavefront in the DPB is
       effective_wavefront_ms = nominal_wavefront_ms × (ref_enhance + 1)
   `range` = 2500 ms and `fpv` = 5000 ms both exceed the 2.0 s GOP,
   so the picture never reaches stripe-only recovery — only an IDR
   completes it.

2. **OSD-specific chroma artefact.**  For static high-contrast overlay
   content (OSD text, near-neutral chroma everywhere), the R-D loop
   picks chroma skip-mode in every MB because chroma residual is
   essentially zero.  Once chroma drifts in a TRAIL_N frame, no
   amount of intra-refresh recovers it — the stripe MBs land in
   display-only frames and never reach the DPB.  Bench-confirmed via
   JPEG snapshot from the same VPE port: the encoder *input* is
   clean (sharp OSD, correct chroma), the H.265 bitstream is what
   produces the green smear.  ROI delta-QP doesn't help because
   skip-mode bypasses QP for zero-residual blocks, and the SigmaStar
   i6c VENC API exposes no force-intra-MB knob.

**Conclusion baked into the preset table:**

Any preset with `ref_enhance > 0` is OSD-unsafe.  The fix is to
classify presets along this axis and let users pick:

| preset      | intra-refresh   | ref_enhance | GOP   | OSD-safe?         | role                                       |
|-------------|-----------------|-------------|-------|-------------------|--------------------------------------------|
| `off`       | off             | 0           | user  | yes (no refresh)  | manual control                              |
| `quality`   | off             | 0           | 4.0 s | yes (IDR-based)   | best image, slow recovery                   |
| `racing`    | fast (150 ms)   | 0           | 2.0 s | yes               | close-range LOS, fast stripe recovery       |
| `endurance` | balanced (500ms)| **0** (was 2) | 2.0 s | yes               | less bitrate on stripes, slower wavefront   |
| `patrol`    | balanced (500ms)| **0** (was 1) | 4.0 s | yes               | long stable flight, 4 s GOP for bandwidth   |
| `rally`     | fast (150 ms)   | 1           | 2.0 s | no                | light refPred, motion-heavy scenes (no OSD) |
| `range`     | balanced (500ms)| 4           | 2.0 s | no                | long-range FPV, heavy refPred (no OSD)      |
| `fpv`       | robust (1000ms) | 4           | 2.0 s | no                | drone FPV, heaviest refPred (no OSD)        |

`endurance` and `patrol` lose their original 1:2 / 1:1 SVC-T pyramid —
they're now racing-class OSD-safe presets distinguished by slower
wavefront (less stripe bitrate) and longer GOP respectively.  `rally`
keeps its 1:1 SVC-T as the lightest refPred option for OSD-off
scenarios.  `range` and `fpv` remain the heavy-refPred presets,
unchanged.

Existing config files that set `resilience=racing`, `range`, or `fpv`
load with identical behaviour.  Users explicitly on `endurance` or
`patrol` from intermediate 0.10.14 dev binaries lose SVC-T but gain
OSD-safe recovery — a behavioural change documented in README.md.

WebUI dashboard enum, API test suite (1588 unit tests), and preset
expansion test cases all updated.  Bench-validated on Star6E
192.168.1.13: racing/endurance/patrol clean up the OSD area within
~10 wavefront cycles; rally/range/fpv leave persistent green smear
that only an IDR can clear.

## [0.10.13] - 2026-05-15

Config-surface simplification: drop dormant `sensor.unlock_*` fields
and merge per-backend AE selectors into one knob.

- **`isp.aeEngine` replaces `isp.legacyAe` (Star6E) + `isp.aeMode`
  (Maruko).**  Two values: `"sdk"` (default) — SDK firmware runs AE —
  and `"custom"` — userspace cus3a takes over.  Mapping on load:

  | `aeEngine` | Star6E | Maruko |
  |---|---|---|
  | `"sdk"` (default) | `legacy_ae=true`  | `ae_mode="native"`   |
  | `"custom"`        | `legacy_ae=false` | `ae_mode="throttle"` |

  Parser keeps the per-backend struct fields populated from the
  unified field, so existing call sites in `star6e_runtime.c`,
  `star6e_pipeline.c`, and `maruko_pipeline.c` need no change.
  Unknown values fall back to `"sdk"`.  Migration: existing
  `/etc/waybeam.json` files containing `legacyAe` and/or `aeMode`
  load cleanly — the parser silently drops both keys and the
  `aeEngine` default (`"sdk"`) drives behaviour, which is the same
  as the historical defaults (`legacyAe=true` + `aeMode="native"`).
  Bench-confirmed on 192.168.1.13: setting `legacyAe=false` cycled
  in custom-AE mode (`[cus3a]` supervisory thread + 15 Hz limits
  enforcement) before the unification commit.

- **H.265 dead-branch cleanup follow-up to 0.10.12.**
  `rtp_session_payload_type()` is now an unconditional `97`,
  `maruko_video.c` drops the defensive non-PT_H265 guard, and
  `star6e_scene_is_idr()` drops the codec parameter (always 1 since
  the H.264 retirement).  Three stale H.264/H.265 comments updated.
  Encoder rate-control union branches (`H264CBR`/`H264VBR`/`H264AVBR`
  in `*_controls.c`) remain in place as documented dead code — they
  follow the SDK enum and ripping them out is more churn than it's
  worth.



- **`sensor.unlockEnabled` / `unlockCmd` / `unlockReg` / `unlockValue` /
  `unlockDir` retired from the user surface; unlock now fires
  unconditionally on every cold boot.**  The
  `MI_SNR_CustFunction(pad, cmd=0x23, reg=0x300a, value=0x80, dir=0)`
  hook is required on IMX415 and IMX335 before `MI_SNR_SetRes`/
  `MI_SNR_SetFps(120)` will accept the high-FPS modes — without it
  the SDK returns -1608835041 and the sensor clamps to 30 fps.
  Initial hot-state bench testing on 192.168.1.13 suggested the hook
  was redundant, but a cold reboot proved that was kernel-driver
  sticky state from earlier unlocked frames; the hook is genuinely
  needed.

  Removing the user-facing knob (rather than reverting the
  simplification) keeps the rule out of the config surface where
  flipping it off accidentally would brick high-FPS sensors.

- **Migration:** existing `/etc/waybeam.json` files containing any of
  the five legacy `unlock*` keys load cleanly — the parser silently
  drops them.  Default JSON ships without the keys.  A user config
  with `"unlockEnabled": false` (which used to be valid) is silently
  ignored; the always-on default takes over.

- **Code path preserved.**  `VencConfigSensor::unlock_{enabled,cmd,
  reg,value,dir}` remain in the struct (defaults: enabled=true plus
  the IMX415/IMX335 register values), and `sensor_unlock_strategy()`
  + `MI_SNR_CustFunction` call sites in `star6e_pipeline.c` and
  `maruko_config.c` stay intact.



refPred (SVC-T temporal hierarchical reference) lands as a real feature
behind a single user-facing knob: `video0.resilience`.

- **`video0.resilience` is the sole knob for intra-refresh + SVC-T +
  GOP.**  Five values pick a 2x2 matrix of trade-offs:

  |                          | Low resilience (best image) | High resilience (more overhead) |
  |--------------------------|-----------------------------|---------------------------------|
  | **Fast recovery needed** | `racing` (intra=fast)       | `fpv` (intra=robust + refPred)  |
  | **Slow recovery OK**     | `quality` (no extras)       | `range` (intra=balanced + refPred) |

  `off` (default) disables both intra-refresh and refPred and honours
  the user's `gopSize`.  Named presets always set `gopSize` (4.0s for
  `quality`, 2.0s for the rest) — the previous "gop=0 means
  intra-refresh picks" auto-mode is gone.  Removed from the user
  surface: `intraRefreshMode`, `intraRefreshLines`, `intraRefreshQp`,
  `refBase`, `refEnhance`, `refPred` (granular fields are still
  populated internally by the preset).

- **refPred (SVC-T) TRAIL_N rewrite.**  The encoder produces a real
  base/enhance pyramid (`MI_VENC_SetRefParam(base=1, enhance=4)` for
  `range`/`fpv`) and the runtime patches NAL byte 0 from `TRAIL_R`
  (type 1) to `TRAIL_N` (type 0) for frames the SDK marked
  `ENHANCE_P_NOTFORREF`.  Without the rewrite the firmware emits every
  NAL as TRAIL_R regardless of its actual eRefType — generic HEVC
  decoders DPB-thrash and visibly warp.  H.265 only.  Mirrored on both
  Star6E (`src/star6e_runtime.c:79-150`) and Maruko
  (`src/maruko_pipeline.c:2904-2965`).

- **`GET /api/v1/resilience/status`** — combined view.  Returns
  `preset`, `intra.{mode,active,mi_supported,apply_ok,effective_lines,
  effective_qp}`, `refPred.{active,mi_supported,apply_ok,base,enhance,
  pred}`, `gop.{effective_sec,auto}`.  GOP value comes from
  `g_cfg->video0.gop_size` (post-preset expansion), so it stays
  accurate even when intra-refresh is off and the existing
  `/api/v1/intra/status` reports zero.

- **Debug OSD: resilience banner.**  When `resilience != "off"`, an
  extra row renders above the existing `intra`/`gop` lines —
  `res fpv rp=1/4` when refPred is active, `res quality` otherwise.

- **Migration:** existing `/etc/waybeam.json` files containing the old
  `intraRefreshMode` / `refBase` / `refEnhance` / `refPred` keys load
  cleanly — the parser ignores them and `resilience` (defaulting to
  `off`) drives behaviour.  Devices upgrading from 0.10.11 keep their
  `gopSize`, `outgoing.server`, and all operational state intact.

- **Bench validation:** Star6E 192.168.1.13 cycled through all five
  presets via REST + restart + log inspection.  Maruko 192.168.2.12
  surgically patched (old granular keys removed, `resilience: "off"`
  added, every other field preserved) and exercised through `quality`,
  `racing`, `range`, `fpv`.  Decoder picture clean at 192.168.2.20 with
  refPred on; OSD garbling on first apply resolves on next IDR (not a
  refPred corruption bug — documented in agent memory).

- **H.265 hardcoded — `video0.codec` retired.**  The video codec is now
  unconditionally HEVC across both backends; the user-facing field is
  gone from the schema, default JSON, and pretty-print/JSON-export.
  This removes the H.264 + refPred footgun (the SVC-T + TRAIL_N rewrite
  is HEVC-only) and collapses several rcMode / RTP / refType branches
  into a single H.265 path.  Migration: existing configs with
  `"codec": "h264"` load cleanly — the key is silently dropped and the
  daemon emits HEVC.  Legacy clients setting `video0.codec=h264` via
  `/api/v1/set` receive a 404 `unknown config field` rather than silent
  acceptance.  Resilience preset table is no longer codec-conditional.

## Investigation - 2026-05-14 — **SOLVED**: IMX415 driver regression is a single missing register write

**Root cause**: `drivers/sensor_imx415_maruko.c` does not write
**`0x3032 = 0x01`** in any of its four init tables.  The stock OpenIPC
`sensor_imx415_mipi.ko` does.  That is the entire dark-image regression.

**Proof**: After a full 0x3000–0x4FFF (8192 reg) i2ctransfer sweep in both
dark-venc and bright-venc states, exactly one register differs:

```
0x3032   dark=0x00   bright=0x01
```

**Smoking-gun experiment**: With venc running on the custom driver
producing a dark image, executed `i2ctransfer -y 1 w3@0x1a 0x30 0x32 0x01`
on the bench at 192.168.2.12.  User confirmed the image **immediately
became bright** on the ground-station receiver.

**Register identity**: `drivers/sensor_imx335_maruko.c:232` documents
`0x3032` as `VMAX` (vertical period — controls frame timing).  On IMX415
the same address appears to also be part of the VMAX/timing block.  The
IMX415 init tables in `drivers/sensor_imx415_maruko.c` set `0x3031` (ADBIT)
and `0x3033` (SYS_MODE) but leave `0x3032` unwritten, so it retains the
sensor's power-on default of `0x00` — which causes the frame-period /
exposure window to be wrong, producing the dark image.

**Workaround the user has confirmed working** (boot stock first, then
custom + venc) works because the stock driver writes `0x3032 = 0x01` and
that value survives the soft reboot.

**Fix applied** (one line per init table, four tables total):

```c
{ 0x3031, 0x00 }, // ADBIT (10bit)
{ 0x3032, 0x01 }, // VMAX MSB — must be 0x01, default 0x00 produces dark image
{ 0x3033, 0x05 }, // SYS_MODE (891Mbps)
```

Applied to all four `Sensor_*_init_table_*[]` arrays in
`drivers/sensor_imx415_maruko.c`.  Rebuilt via
`make drivers-maruko KSRC_MARUKO=<...>`:

- New `sensors/maruko/sensor_imx415_maruko.ko` — md5 `bd582d87...` (was `c236ac34...`)
- 4× `{ 0x3032, 0x01 }` patterns confirmed in the binary

**Followup (IMX335) — resolved 2026-05-14**: user swapped to an IMX335
sensor on the bench and confirmed bright image on firstboot with the
unpatched custom `sensor_imx335_maruko.ko`.  As static analysis
predicted, the IMX415 single-bit bug does not apply to IMX335 —
`0x3032` on IMX335 is genuinely the VMAX high nibble and our driver
already writes it correctly as `0x00` in all 5 init tables.  No action
required.

Captures + diffs: `bench_logs/manual_sensor_diff_20260514T093447Z/`

## Investigation - 2026-05-14 — Maruko firstboot dark image is a custom-driver regression

Manual session on bench 192.168.2.12 narrowed the long-standing "venc on
firstboot is dark, majestic-first warms it up" symptom to a regression in
our **custom-built `sensor_imx{335,415}_maruko.ko` kernel modules** (in
`sensors/maruko/`).

Findings, in order:

- Stock `/rom` driver (25K, md5 `a33cfa52...`) + majestic = bright.
- Custom overlay `_maruko.ko` (167K, md5 `c236ac34...`) + venc = dark.
- Custom overlay `_maruko.ko` + majestic = **also dark** — confirmed by
  the user.  This rules out the original "venc-vs-majestic init"
  hypothesis: the bad actor is the .ko, not the streamer.
- Restoring the stock driver via `rm /overlay/root/lib/modules/.../sensor_imx*_mipi.ko`
  and rebooting brings the image back to normal — overlayfs reveals the
  stock `/rom` copy.

Register deltas (regscan curated 254 entries, banks 0x30–0x40) between
dark-custom and bright-stock states: HMAX (`0x3024/0x3025`), BIN_MODE
(`0x3050/0x3051`), and `0x3090` (IMX415 analog).  The custom .ko leaves
all of these at firstboot defaults; the stock .ko writes them.

Workaround the user has confirmed working:

  stock .ko + majestic → soft reboot → custom .ko + venc → bright

That last step is the surprise: with custom .ko in place, `venc` on a
soft reboot still produces a bright image — yet only **one** scanned
register (`0x3032`: 0x00 → 0x01) survived the reboot from the stock
session.  All five HMAX/BIN_MODE/0x3090 registers reverted to dark-state
values.  So the state that actually distinguishes dark-vs-bright lives
**outside regscan's 254-entry range** — likely 0x4100+ (SHR), 0x5000+
(calibration), 0x6000+ (VOUT/MIPI), or ISP-side state.

See `documentation/MARUKO_FIRSTBOOT_DARK_IMAGE_TEST.md` (Findings
section) for the full table and Phase-2 brute-force-sweep plan.

Captures: `bench_logs/manual_sensor_diff_20260514T093447Z/`

## [0.10.11] - 2026-05-14

Maruko snapshot follow-up: SIGHUP reinit hardening, MJPG quality
actually applied, live-update `snapshot.quality`, and the
Maruko-specific default config finally reaches the release tarball.

- **`snapshot.quality` is MUT_LIVE.**  POST/GET `/api/v1/set?snapshot.quality=N`
  applies instantly with no pipeline reinit — Get→modify→Set on
  `MI_VENC_ChnAttr_t.rate.mjpgQp.quality` on the parked MJPG channel.
  Frontend (`src/venc_jpeg.c`) serializes the live-set call under the
  same module mutex as `venc_jpeg_capture`, so an in-flight snapshot
  request cannot race the SDK Get/Set sequence.  Backend hooks added:
  `venc_jpeg_backend_set_quality(uint32_t)` in both `star6e_jpeg.c`
  and `maruko_jpeg.c`; weak `-ENOSYS` fallback in the common layer
  keeps the host-test build link-clean.  Schema field flips from
  `MUT_RESTART` to `MUT_LIVE` in `g_fields[]`; full LIVE-group wiring
  through `venc_api.c` (key→group, name, supported, copy, apply) +
  `apply_snapshot_quality` callback on `VencApplyCallbacks`.
  Range validator: `[1, 99]` (SDK ceiling) at `validate_field_cfg`.
  Front-end `venc_jpeg_init` clamp aligned to ≤99 (was 100) for
  symmetric behaviour with live-set.
  - Bench (Maruko 192.168.2.12 firstboot): q=20→118 KB, q=50→257 KB,
    q=80→464 KB, q=99→2.03 MB across same pid, zero reinits.
  - Bench (Star6E 192.168.1.13): q=20→51 KB, q=50→78 KB, q=80→154 KB,
    q=99→261 KB across same pid, reinit count unchanged.

- **MJPG quality actually wires through on Maruko** (was silently
  ignored in 0.10.10).  Root cause: `attr.rate.mode` was set to
  `I6C_VENC_RATEMODE_MJPGQP` (= 8), which Maruko firmware interprets
  as `MJPEGVBR` in its UBR-shifted enum — the channel built fine but
  `attr.rate.mjpgQp.quality` was discarded since VBR mode has no
  quality field.  Fixed by adding `MARUKO_VENC_RC_MJPG_{CBR,VBR,FIXQP}`
  = {7,8,9} to `include/maruko_bindings.h` (the firmware-accepted enum
  values, distinct from the I6C SDK header's shifted layout) and
  pointing `maruko_jpeg.c` at `MARUKO_VENC_RC_MJPG_FIXQP` (= 9).
  DQT tables now scale correctly: q=99 → all-1's quantization,
  q=20 → coarse quantization, byte sizes track expected.

- **Maruko SIGHUP reinit no longer crashes on consecutive `kill -1`.**
  Root cause: `maruko_load_isp_bin` calls
  `MI_ISP_DisableUserspace3A` + post-load `CUS3A_Enable` hooks that
  are "once per process lifetime" — re-entering them on the second
  reinit segfaults inside the SDK with `Mutex is not initialized
  before lock` from `libcam_os_wrapper.so`.  Fixed by splitting the
  load path: cold boot keeps the full `maruko_load_isp_bin`; reinit
  uses a new minimal variant that skips both CUS3A hooks (sufficient
  because the kernel ISP module's state survives the in-process
  teardown).  10 consecutive `killall -1 venc` cycles verified clean
  on 192.168.2.12; pid 1894 stable, no SDK reset, no zombie state.

- **Maruko default config reaches the release tarball.**  Two-part
  fix.  First, `config/venc.default.maruko.json` gains the `snapshot`
  block (was missing entirely — Maruko users had snapshot disabled
  until they hand-edited the JSON, even though the schema and runtime
  defaults were already in place).  Second, `.github/workflows/release.yml`
  was copying `config/venc.default.json` for *both* backends when
  staging the release archives, so the Maruko tarball's bundled
  `venc.json` carried Star6E defaults (`sensor.unlockEnabled=true`,
  no `snapshot` block).  Now picks the Maruko-specific template when
  staging `venc-maruko.tar.gz`, falls back to the shared default for
  Star6E.  Firstboot Maruko devices installed from the release tarball
  now ship with `snapshot.enabled=true` and the right unlock policy.

- **Firstboot deployment verified end-to-end on Maruko.**  Wiped
  device (no `/usr/bin/venc`, no `/usr/lib/libmi_*.so`, no
  `/etc/venc.json`); pushed binary + 14 MI libs + 10 sensor `.ko` +
  3 ISP `.bin` + `json_cli` + the bundled `venc.json` in a single
  bulk-push via `scripts/maruko_direct_deploy.sh full
  --push-config config/venc.default.maruko.json`; rebooted; venc
  came up clean at pid 720, IMX335 sensor module loaded, pipeline
  configured at 1920×1080@60, `/api/v1/snapshot.jpg` worked first
  request without manual config edits.

## [0.10.10] - 2026-05-14

Maruko snapshot backend — closes the deferred follow-up from 0.10.9.
`GET /api/v1/snapshot.jpg` now serves a real JPEG on Maruko (was
`503 snapshot_disabled` in 0.10.9).  Star6E unchanged.

- **Architecture** — dedicated MJPG VENC device 8 (`I6C_VENC_DEV_MJPG_0`)
  channel 0, bound to a second SCL output port (SCL dev 0 chn 0 port 1)
  via `MI_SYS_BindChnPort2` in `I6_SYS_LINK_FRAMEBASE` mode at 5 fps
  destination rate.  Channel stays parked (`StopRecvPic`) between
  requests; capture flips `StartRecvPic` on, polls `Query` for ready
  packs, drains via `GetStream`, then parks again.  Same idle pattern
  as Star6E (`src/star6e_jpeg.c`) — no encoder CPU when no snapshot
  is in flight.
- **Why a second SCL port** — Maruko's SCL output port 0 is held by
  the main H.265 channel in `LINK_RING` mode (1:1, `0xA0092012` if
  re-bound).  Port 1 is a fresh tap from the same SCL channel, so
  no contention with the main stream.  Avoids the kthread-leak path
  the earlier HW_RING fan-out attempt hit — `dev 8` only sees SCL,
  never has any relationship to main `dev 0`, so failed-init teardown
  is clean (no `[venc8_P0_MAIN]` orphan).
- **Pipeline wiring** — `src/maruko_pipeline.c::configure_maruko_scl()`
  configures + enables SCL port 1 (YUV420SP, no IFC compress, same
  crop + output dims as port 0).  `bind_maruko_pipeline()` calls
  `venc_jpeg_set_source(&ctx->scl_port1)` before `venc_jpeg_init`.
  `maruko_pipeline_teardown_graph()` disables port 1 after
  `venc_jpeg_shutdown()`.  Port-1 setup failures are non-fatal:
  warning logged, snapshot returns `503` cleanly via the
  `g_have_scl_port=0` path in `venc_jpeg_backend_init`.
- **Bench verification (192.168.2.12, IMX415, 1920×1080@60)** — 10
  rapid snapshots in 679 ms (~14 req/s sustained, all `HTTP 200`);
  size 120–184 KB; mean Y ≈ 124 (bright, post-firstboot-fix); main
  RTP stream to 192.168.2.20 unaffected during snapshot bursts; no
  `[venc8_P0_MAIN]` kthread in `ps`.

Snapshot config schema fields are now part of the on-disk JSON,
fully wired through the standard 7-touch-point machinery:

- **`venc.json` → `snapshot.{enabled,quality,channel,width,height}`**
  with sensible defaults (`enabled=true`, `quality=80`, `channel=7`,
  `width=0`/`height=0` = inherit main stream).  Read on pipeline init
  via `MarukoBackendConfig::snapshot` (Maruko) and `VencConfig::snapshot`
  directly (Star6E).
- `/api/v1/get?snapshot.<field>` and `/api/v1/set?snapshot.<field>=<v>`
  resolve through `g_fields[]` (all MUT_RESTART since the SDK channel
  attrs are baked at `CreateChn` time).
- `config/venc.default.json`, `pretty_print`, `cJSON` serializer, and
  `MarukoBackendConfig` mirror all carry the new section — the
  `layout_size_equal` round-trip test in `tests/test_venc_config.c`
  protects against future drift.

Star6E hardware bench validation is still deferred (no Star6E bench
currently online).

Caveat (pre-existing, not introduced here): rapid back-to-back
`/api/v1/set?<MUT_RESTART_field>=...` requests within the reinit
window can crash venc — same SIGHUP reinit race already tracked in
`roadmaps/waybeam_venc.md` ("SIGHUP reinit stabilization — partial
teardown works, ISP race outstanding").  A single quality / dims
change settles cleanly; users should wait for `reinit_pending=true`
to resolve before issuing another restart-tier `set`.

## [0.10.9] - 2026-05-14

JPEG snapshot HTTP endpoint on both backends.

- **`GET /api/v1/snapshot.jpg`** — dedicated MJPEG VENC channel taps
  the same VPE/SCL output port the main H.264/H.265 stream consumes.
  Channel is created at pipeline-start, kept idle (StartRecvPic off)
  between requests; each request flips StartRecvPic on, polls Query
  for a ready pack, drains one JPEG frame via GetStream, then turns
  StartRecvPic back off.  Captures are serialized through a module
  mutex so concurrent HTTP clients queue rather than collide.
  Response is `Content-Type: image/jpeg`; failure modes are
  `503 snapshot_disabled` (subsystem not initialised),
  `504 snapshot_timeout` (no frame within 1.5 s), or
  `500 snapshot_failed` (SDK / alloc error).  Default quality 80.
- **Star6E backend** (`src/star6e_jpeg.c`) — `I6_VENC_CODEC_MJPG` on
  ch7, `I6_SYS_LINK_FRAMEBASE` bind to the pipeline's VPE port.
  Star6E supports 1:N from a VPE output port, so the snapshot channel
  binds alongside the main H.265 channel without contention.
- **Maruko backend** (`src/maruko_jpeg.c`) — **deferred**.  Bench
  investigation on 192.168.2.12 found two blockers:
  (a) the Maruko SCL output port is 1:1 — binding it to the MJPG
  channel after the main H.265 channel already holds it returns
  `0xA0092012` ("SYS busy", same code documented in `maruko_pipeline.c`
  line 2097 for the dual-stream path);
  (b) an attempted workaround using cross-device VENC HW_RING fan-out
  hit the SDK's teardown bug — failed init left an orphaned
  `[venc8_P0_MAIN]` kernel thread that blocked the next `MI_SYS_Init`
  indefinitely (HISTORY "venc_teardown_regression" pattern; recovered
  via sysrq-b).  Two viable paths forward (out of scope for this PR):
  configure a second SCL output port at pipeline init, or probe the
  cross-device VENC bind before `CreateChn` so failure paths don't
  leak kernel state.  Until then `src/maruko_jpeg.c` is a clean
  `-ENOSYS` stub so `/api/v1/snapshot.jpg` serves 503 cleanly without
  ever touching the SDK.
- **HTTP plumbing** — new `httpd_send_binary()` helper in
  `include/venc_httpd.h` for raw byte payloads with caller-supplied
  `Content-Type`.  Used by the snapshot handler; reusable for any
  future binary endpoint (PNG OSD overlay, IQ blob dumps, etc.).
- **Tests** — `tests/test_venc_jpeg.c` (13 assertions): pre-init
  capture refusal, NULL-arg rejection, `enabled=false` no-op init,
  failed-backend → clean -ENODEV degradation, idempotent shutdown,
  re-init after shutdown.  All run on the host test_runner because
  the common layer's weak-symbol backend stubs make the module
  exercisable without an SDK present.
- **Pipeline lifecycle** — pipeline init calls
  `venc_jpeg_set_source(vpe_port)` + `venc_jpeg_init(&cfg)` right
  after the main VPE/SCL→VENC bind; pipeline teardown calls
  `venc_jpeg_shutdown()` before the matching unbind (idempotent).
  Failure of the snapshot init is non-fatal — the main stream still
  comes up, the snapshot endpoint just serves 503.

Defaults are hardcoded for this release: `quality=80`, `channel=7`
(mapped to ch 0 on Maruko's MJPG_DEV), width/height inherited from
the main stream.  A future release will surface `snapshot.{enabled,
quality,width,height,channel}` in `venc.json`.

## [0.10.8] - 2026-05-14

Release tarball completeness — fixed three gaps that left a fresh
device install incomplete after extracting `venc-maruko.tar.gz`:

- **Sensor drivers shipped (Maruko).**  `sensors/maruko/sensor_imx{335,415}_maruko.ko`
  are now vendored in the repo (source-built via `make drivers-maruko`,
  ~320 KB total).  `make stage SOC_BUILD=maruko` renames them to
  `_mipi.ko` for drop-in compatibility with stock OpenIPC kernel
  module naming, and the release tarball ships them under
  `drivers/`.  Previously the release was sensor-driver-free, so a
  fresh-device install ended up running stock OpenIPC drivers
  regardless of the modifications in `drivers/sensor_*.c`.  See
  `sensors/maruko/README.md` for provenance.
- **ISP tuning blobs shipped (Maruko).**  `iq-profiles/maruko-bin/imx{335,415,415_fpv_api}.bin`
  are now vendored (~420 KB total), pulled from the verified bench
  at 192.168.2.12.  Release tarball ships them under `isp-bins/` so
  fresh devices have IQ tuning available without a manual
  `/etc/sensors/` pull.  See `iq-profiles/maruko-bin/README.md`.
- **regscan shipped (both backends).**  IMX335/IMX415 i2c register
  dumper (vendored from `tipoman9/star6c_sensor`) now builds in CI
  for both Star6E and Maruko and ships in both tarballs.  Read-only
  diagnostic; used by `scripts/maruko_sensor_init_diff.sh` for
  sensor-init investigations.

CI verification step now hard-checks the new artifacts exist before
upload, so a regression that drops them from the tarball will fail
the build instead of silently shipping an incomplete release.

Release-notes install snippet expanded to cover the new payloads
(`cp drivers/*.ko /lib/modules/.../sigmastar/`, `cp isp-bins/*.bin
/etc/sensors/`, `cp regscan /usr/bin/regscan`).

## [0.10.7] - 2026-05-11

Maruko deploy pipeline polish — fix four PR-review gaps and one
build-cycle ergonomics issue surfaced during bench testing.

- **Per-source object files + `-MMD -MP` dep tracking.**  The
  top-level Makefile used a single monolithic CC invocation that
  compiled and linked all 47 source files in one shot, so every
  iteration of `make build SOC_BUILD=maruko` rebuilt everything from
  scratch even for a one-line edit.  Split into `$(OBJ_DIR)/%.o`
  pattern rules with auto-generated `.d` dep files.  Cold build
  unchanged at ~3.3 s; touching one `.c` is now ~0.13 s (1 compile +
  relink); a header change only rebuilds dependent objects.
- **`push-drivers` no longer non-deterministic.**  When
  `sensors/maruko/` contains both a source-built `sensor_imxNNN_maruko.ko`
  (renamed to `_mipi.ko` on push) and a pulled-from-device blob
  `sensor_imxNNN_mipi.ko`, the alphabetical glob ordering let the
  pulled blob clobber the source-built rename.  `push_drivers` now
  skips the pulled `_mipi.ko` when a `_maruko.ko` sibling exists, and
  logs the skip count.
- **uClibc compat symlinks pushed automatically.**  Stock OpenIPC
  musl firmware ships `/lib/libc.so` only, but the vendor blob
  `libcam_os_wrapper.so` has hardcoded NEEDED tags `ld-uClibc.so.1`
  and `libc.so.0` in its `.dynamic` section (cannot be relinked — it
  is a binary drop).  `push-libs` (and `cycle`/`full` when libs are
  requested) now creates `ln -sf libc.so /lib/ld-uClibc.so.1` and
  `ln -sf libc.so /lib/libc.so.0` after the library push.  Idempotent;
  pristine firstboot devices no longer segfault on first `venc`
  start.  `vendor-libs/maruko/README.md` updated — the previous claim
  that `ld-uClibc.so.1` was "dead since v0.7.0" was wrong (only the
  shim binary is dead; the NEEDED tag inside the wrapper is not).
- **`json_cli` vendored from `waybeam-hub`.**  `scripts/maruko_direct_deploy.sh`'s
  `config-get` / `config-set` / `status` paths all require
  `/usr/bin/json_cli` on the target.  Previously the deploy script
  assumed someone else had pushed it, which broke on firstboot.  Now
  `tools/json_cli/{json_cli.c,jsmn.h}` ship in the repo (re-synced
  from `../waybeam-hub/tools/`); `make json_cli SOC_BUILD=maruko`
  builds `out/maruko/json_cli`; `scripts/maruko_direct_deploy.sh
  push-json-cli` (and `cycle --with-json-cli` / `full`) installs it
  to `/usr/bin/json_cli`.

## [0.10.6] - 2026-05-07

`isp.sensor_bin` is now a live mutable field on both backends.

Previously, swapping the ISP tuning bin via `/api/v1/set?isp.sensorBin=...`
fell into the `MUT_RESTART` path, which on Star6E means `g_running=0` →
clean teardown → fork+exec a successor venc → MI_SYS_Init → reselect sensor
→ rebuild VIF/VPE/VENC.  End-to-end downtime measured ~15 s on the
192.168.1.13 bench — far longer than the ~80 ms the actual
`MI_ISP_*CmdLoadBinFile` call needs.  The fork+exec path is the right call
for sensor mode / size / codec changes (in-process MI_SYS_Init is broken on
Star6E), but it is overkill for an ISP IQ refresh that does not touch the
graph at all.

- New `apply_isp_bin(const char *path)` callback on `VencApplyCallbacks`.
- `FIELD(isp, sensor_bin, ..., MUT_LIVE)` plus a `LIVE_GROUP_ISP_BIN`
  dispatch entry, so single-set and multi-set go through the same live
  apply path that already drives bitrate / fps / awb_mode.
- Star6E (`star6e_pipeline_load_isp_bin_live`):
  resolve via `pipeline_common_resolve_isp_bin` (configured path → sensor
  fallback → none), short-circuit when the resolved path matches the
  last-loaded one, otherwise call the existing `*_load_isp_bin` path.
  Reapplies `MI_ISP_AE_SetExposureLimit` (bin can reset AE limits) and,
  in `legacyAe` mode, kicks `MI_SNR_SetFps` so the sensor's physical
  shutter register isn't left at the bin's cold-boot value — without
  this, swapping to a darker bin locks the sensor at ~12 fps until reinit
  (cold_boot_fps_lock).
- Maruko (`maruko_pipeline_load_isp_bin_live`): same resolve+dedup
  shape, but uses a stripped-down `maruko_load_isp_bin_minimal` that
  intentionally **skips** the `MI_ISP_DisableUserspace3A` and post-load
  `MI_ISP_CUS3A_Enable` hooks the cold-boot path uses.  Re-entering
  CUS3A_Enable on the still-active channel trips the same kernel-mutex
  regression noted in `maruko_stop_vpe_channels` ("Skip DestroyChannel
  — kernel ISP retains CUS3A mutex state") and segfaults venc on the
  second swap.  3A_Proc_0 stays running across the load and picks up
  the new IQ tables on its next tick.  `g_last_isp_bin_path` is
  updated so the next reinit-time gate stays consistent, and cleared
  in `maruko_pipeline_teardown_graph` so the SIGHUP in-process reinit
  runs the cold-boot bin load unconditionally (Star6E gets that for
  free via fork+exec).
- New per-field validator: a non-empty `isp.sensor_bin` must point at a
  readable file or the set is rejected with `409 validation_failed`.
  Empty string still opts into the auto-detect fallback.

Verified on 192.168.1.13 (Star6E, imx415, legacyAe):
- 6 back-to-back swaps complete with end-to-end ~250 ms each (≈80 ms
  reload + ssh round-trip), down from ~15 s.
- Streaming holds at 59-60 fps through and after the swap sequence;
  earlier prototype without the SetFps kick locked at 12 fps.
- `/api/v1/set?isp.sensorBin=/no/such/bin` returns
  `{"code":"validation_failed","message":"isp.sensor_bin path is not
  readable"}` with HTTP 409 and leaves config unchanged.
- `/api/v1/set?isp.sensorBin=` (empty) accepted.
- Reloading the same bin twice short-circuits with
  `> ISP bin reload: <path> already loaded, skipping`.

Verified on 192.168.2.12 (Maruko, imx415):
- 5 back-to-back swaps complete cleanly; venc still up after, 3A_Proc_0
  ticks through the load.  End-to-end ~5.2 s/swap (the Maruko SDK's
  `MI_ISP_API_CmdLoadBinFile` itself takes ~5 s on this BSP — visible
  as a ~75 cus3a-tick gap in the log).  Same order of magnitude as the
  in-process reinit it replaces, but no pipeline graph teardown and
  no HTTP hang under load.
- First minimal-hooks prototype that mirrored the cold-boot
  disable/enable hooks crashed with "WARNING: Mutex is not initialized
  before lock" → segfault on the second swap; that prompted the
  cold-boot-vs-live hook split documented above.
- Validator + empty path + idempotent re-set behave identically to
  Star6E.

Two new unit tests cover the live dispatch, the validator, and the 501
fallback when a backend doesn't supply `apply_isp_bin`.

## [0.10.5] - 2026-05-05

Maruko-specific default config template (`config/venc.default.maruko.json`).

The shipped `config/venc.default.json` defaults `sensor.unlockEnabled` to
`true` because that flag is required on Star6E to unlock IMX415/IMX335
high-FPS modes from cold boot (see
`documentation/SENSOR_UNLOCK_IMX415_IMX335.md`).  The unlock command
(`MI_SNR_CustFunction` cmd `0x23`, reg `0x300a`, value `0x80`) targets a
driver internal latch present in the Star6E `sensor_imx*_mipi.ko`
modules.  On Maruko the sensor driver layout is different and that latch
does not exist — the call is at best a no-op + warning, at worst it
prints a confusing failure on every boot.

A new Maruko user starting from `venc.default.json` would inherit the
unlock-on flag and see those warnings even though their pipeline is
fine.  `config/venc.default.maruko.json` is identical to the Star6E
default except for `sensor.unlockEnabled: false`, so packagers and
first-time Maruko users have a clean starting point.  The runtime is
unchanged — `/etc/venc.json` is still the only path the binary reads.

## [0.10.4] - 2026-05-05

`/api/v1/dual/status` reachable on both backends.

Two related issues fixed:

- **Maruko never registered the dual VENC handle with the HTTP API.**
  `maruko_pipeline_start_dual` brought up chn 1, started the drain
  thread, and set `ctx->dual`, but it never called
  `venc_api_dual_register()`.  Result: `/api/v1/dual/status`,
  `/api/v1/dual/set`, and `/api/v1/dual/idr` all returned 404 even
  with `record.mode = "dual"` or `"dual-stream"` — a regression vs.
  Star6E parity claimed in HTTP_API_CONTRACT §"Dual VENC".
  `maruko_pipeline_stop_dual` now mirrors Star6E by calling
  `venc_api_dual_unregister()` before tearing down chn 1.
- **`/api/v1/dual/status` now always returns 200 with `active`.**
  Previously the handler returned 404 with `not_active` when dual
  was disabled, which made the endpoint indistinguishable from a
  routing miss.  Aligned with the `/api/v1/record/status` shape:
  `{"ok":true,"data":{"active":false}}` when off,
  `{"ok":true,"data":{"active":true,"channel":1,"bitrate":...,
  "fps":...,"gop":...}}` when on.  Write endpoints `/dual/set` and
  `/dual/idr` keep the 404+`not_active` semantics — those still need
  a live channel to operate on.
- **`/api/v1/dual/set` is Star6E-only** (returns 501 on Maruko).
  The previous handler dereferenced `MI_VENC_ChnAttr_t` (i.e.
  `i6_venc_chn`), but Maruko's venc library expects `i6c_venc_chn`
  with a different layout — calling it through the wrong typedef
  corrupted the attr struct.  The latent bug went undetected until
  this version because Maruko never registered the dual handle, so
  `/dual/set` always short-circuited to 404 on Maruko before the
  bad call.  `/dual/idr` is single-arg and works on both.

Verified on 192.168.1.13 (Star6E) with `record.mode` in `{off, dual}`
— `/dual/status` returns `active:false` and `active:true` respectively;
`/dual/set?bitrate=8000` and `/dual/idr` both return 200 in dual mode.
Verified on 192.168.2.12 (Maruko) with `record.mode` in `{off,
dual-stream}` — same status shape, `/dual/idr` returns 200, `/dual/set`
returns 501.  `dual` mode (TS file write) was not exercised on Maruko
because `record.dir=/tmp` is tmpfs there and fills RAM under load;
`dual-stream` exercises the same `venc_api_dual_register` path.

## [0.10.3] - 2026-05-05

HTTP dispatch pause/resume across pipeline reinit and teardown.

Closes a long-standing HTTP↔runner thread race: the httpd worker
dereferences SDK handles (VENC/ISP/SCL/VPE channels, audio capture,
output socket) that the runner thread destroys and recreates during
reinit or shutdown.  Symptoms ranged from `MI_*` errors against a
destroyed channel to outright segfaults under heavy WebUI traffic
during a SIGHUP reinit on Star6E, and visible HTTP hangs across the
in-process reinit window on Maruko.

Fix: a single chokepoint at the httpd worker's dispatch call.

- `venc_httpd_pause()` sets a flag and drains the in-flight handler
  (it takes the same mutex the worker holds across `dispatch()`).
  After pause returns, every new request is answered with 503
  immediately, so SDK state is safe to tear down.
- `venc_httpd_resume()` clears the flag.

Call sites:

- `maruko_runtime.c` brackets `teardown_graph` + `reinit_pipeline` with
  `pause` / `resume` (the in-process reinit window).
- `maruko_pipeline.c` pauses before final teardown (no resume — the
  process is exiting).
- `star6e_runtime.c` pauses across the SDK shutdown teardown until
  `venc_httpd_stop()` returns (no resume — fork+exec parent or normal
  exit).

Hardware verification on 192.168.1.13 (Star6E IMX335 @ 60 fps fork+exec
respawn) and 192.168.2.12 (Maruko in-process reinit): under sustained
mixed `apply_*` / `query_*` traffic, the pause window emits fast
sub-4 ms 503s for clients that hit it, no requests hang, no daemon
crashes, and the encoder keeps streaming throughout the Maruko reinit.

## [0.10.2] - 2026-05-05

Maruko: HTTP record control + raw HEVC recording (Star6E parity).

**HTTP record control** — `/api/v1/record/start` and `/stop` previously
returned 501 "HTTP record control not available on this backend" so
the WebUI dashboard record buttons were dead.  The HTTP request
flags are now drained in the chn 0 drain loop, gated by the same
`!ctx->dual` guard that protects the chn 0 write (the dual chn 1
drain thread owns the recorder when active).  Back-to-back `/start`
calls rotate the segment cleanly and request an IDR so the new
segment begins on a keyframe.

**Raw HEVC recording** — `record.format = "hevc"` is now accepted on
Maruko, matching Star6E.  The pipeline holds parallel `ts_recorder`
and `recorder` (Star6eRecorderState) state; format dispatch happens
at start and selects which one consumes the chn 0 / chn 1 stream.
A new `src/maruko_recorder.c` adapter walks `i6c_venc_strm` with the
same iovec-collected `writev` pattern as `star6e_recorder.c`,
reusing all the disk-space / sync_file_range plumbing.

Both modes verified on 192.168.2.12 via WebUI: 3760×2116 HEVC Main
in `.hevc`, HEVC + Opus in `.ts`.

## [0.10.1] - 2026-05-05

TS recorder: universally-decodable audio in `.ts` files.

The recorder previously muxed audio as private-data with an "LPCM"
registration descriptor that no standard player recognised — VLC,
ffmpeg, and mpv treated it as `bin_data` and either dropped it or
played white noise.  Recordings now carry audio in one of two forms,
selected by `audio.codec`:

- `audio.codec = "pcm"` → SMPTE 302M (BSSD).  Broadcast standard for
  16-bit PCM in MPEG-TS.  Mono inputs are upmixed to stereo per the
  302M requirement.  Universally decoded.
- `audio.codec = "opus"` → Opus-in-MPEG-TS provisional mapping.
  Re-uses the same Opus encoder feeding the RTP path, so no extra CPU
  cost.  Each PES carries one Opus access unit prefixed by the
  `0x7FE0` control header.  Recommended — about 30× smaller audio than
  PCM at the same intelligibility.
- `audio.codec = "g711a"` / `"g711u"` → audio is not muxed into the
  recording (no in-band TS framing that VLC/ffmpeg decode without
  hints).  Video-only file.

Filenames now carry a `_opus` or `_pcm` suffix so the codec is visible
without ffprobe (e.g. `rec_02h23m07s_c9e2_opus.ts`).

ts_mux additions:
- `ts_mux_init` gains an `audio_codec` argument
  (`TS_AUDIO_CODEC_PCM_S302M` / `TS_AUDIO_CODEC_OPUS`)
- PMT writer emits the matching registration descriptor (BSSD or Opus)
  plus the Opus extension descriptor with `channel_config_code`
- `ts_mux_write_audio` dispatches: SMPTE 302M packs raw s16le with
  bit-reversal per the AES3 16-bit layout; Opus path wraps each
  pre-encoded packet in the 11-bit prefix + au_size control header
- `star6e_ts_recorder_init` gains an `audio_codec` argument plumbed
  through from `audio.codec`

The Star6E and Maruko audio threads now route the encoded buffer
(rather than raw PCM) into the recording ring when codec is Opus.

Verification:
- Host: 1520 unit tests pass; offline sine encode round-trips
  bit-exact through ffmpeg's SMPTE 302M and Opus parsers.
- Hardware: Star6E bench (`192.168.1.13`, IMX335).  HEVC + audio
  recordings in both modes play directly in VLC, mpv, and ffmpeg.

## [0.10.0] - 2026-05-03

`video0` digital zoom (Approach C) — Star6E + Maruko parity.

Adds three new `video0` fields driving a 1:1 SCL crop (output dim = crop
dim, no upscale, no bandwidth pressure).  Receivers see the smaller dim
in SPS/PPS; receivers that pin to first SPS render deeper zoom invisibly,
which is why `zoom_pct` is clamped at a 0.25 floor in the parser.

Schema:
- `video0.zoomPct` — `0.0` = zoom OFF (full image); `0.25..1.0` = crop
  fraction (smaller = deeper zoom).  MUT_RESTART (encoder dim change).
- `video0.zoomX`, `video0.zoomY` — crop centre, `0..1` (0 = top/left,
  1 = bottom/right).  MUT_LIVE (no respawn — joystick / head-tracker
  friendly).

Implementation:
- **Star6E**: `MI_VPE_SetPortCrop(0, 0, ...)` on the existing VPE port —
  no new SCL channel, no extra mem.  SCL clock bumped 384 → 432 MHz to
  unblock crop+resize at full sensor input.  Debug OSD canvas stays 1:1
  with the encoded frame (RGN attaches at the post-SCL VPE port output,
  not at VPE input — no per-zoom offset needed).
- **Maruko**: `MI_SCL_SetPortConfig(0,0,0)` carrying both crop and
  output dim atomically.  Output dimensions stay 16-px aligned and crop
  offsets stay 2-px aligned; the smaller dimension drives the crop to
  keep the encoded AR matching the sensor.

Verification:
- Live sweep on both devices: pct ∈ {1.0, 0.7, 0.5, 0.3, 0.25} × pan ∈
  {(0.5,0.5), (0,0), (1,0), (0,1), (1,1), (0.5,0.5)}.  Star6E sustains
  60 fps, Maruko sustains 30 fps across all combinations.  Pan
  confirmed visually on the live RTP stream for both backends.

## [0.9.16] - 2026-05-03

IntraRefresh: single-knob `intraRefreshMode` enum.

Replaces the boolean `intraRefresh` + two zero-sentinel fields from PR #92
with one human-readable mode picker (`off` | `fast` | `balanced` | `robust`).
Each mode targets a self-heal window (150 ms / 500 ms / 1000 ms) and derives
lines, GOP, and QP from it; per-field overrides remain available.

**Breaking change** (no backward compat): `video0.intraRefresh` boolean is
removed. Configs from 0.9.15 and earlier carrying `intraRefresh: true` will
fall through to `intraRefreshMode: "off"` (default) — re-enable explicitly
via the mode field or `POST /api/v1/intra/mode?mode=balanced`.

Highlights:
- New `src/intra_refresh.{c,h}` shared helper (used by both backends)
  computes lines, auto-GOP (one IDR per full GDR pass), and codec-default
  QP (48 H.265 / 45 H.264 — `u32ReqIQp` is never passed as 0).
- New `POST /api/v1/intra/mode?mode=<name>` endpoint: sets mode, clears
  per-field overrides, persists, reinits. One call to switch presets.
- Extended `/api/v1/intra/status` response: returns mode, target_ms,
  total_rows, and per-field requested-vs-effective for lines/qp/gop.
- Both Star6E and Maruko backends share the helper — drift between
  parallel implementations is no longer possible.
- Schema field `video0.intra_refresh_mode` registered (FT_STRING,
  MUT_RESTART) with `intraRefreshMode` camelCase alias.
- Auto-GOP overrides `gopSize` only when user did not pin a value;
  explicit `gopSize > 0` is honored and logged at boot.
- Contract bump 0.8.4 → 0.9.0 (breaking config field rename).
- 44 new unit tests covering parse, compute, override, clamp, edge cases.

## [0.9.15] - 2026-05-02

Maruko parity Phase 5 — audio capture (Opus / G.711 / raw PCM).

Closes the last big standalone parity gap: Maruko now captures PCM via the
i6c MI_AI ABI, encodes via the shared codec helpers, and ships the result
as RTP (or compact UDP) on `outgoing.audioPort`.  The Phase 6 TS recorder
automatically picks up audio when capture is active — PMT advertises an
audio PID and PCM frames are interleaved by the existing `ts_mux_write_audio`
path that Star6E was already wired to.

What ships:
- `vendor-libs/maruko/libmi_ai.so` (110 KB) and `libmi_ao.so` (85 KB),
  pulled from the SDK uClibc bundle.  AO is reserved for a future Phase 5b
  playback path.  MD5SUMS + README updated.
- New `include/maruko_ai_types.h` carrying the small set of MI_AI types
  used at runtime (Attr / Data / Format / SoundMode / SampleRate / If),
  copied verbatim from the SDK headers so the build stays SDK-header-free.
- New `maruko_ai_impl` shim in `include/maruko_mi.h` + `src/maruko_mi.c`.
  Symbol set: `MI_AI_InitDev` / `DeInitDev` / `Open` / `Close` /
  `AttachIf` / `EnableChnGroup` / `DisableChnGroup` / `Read` /
  `ReleaseData` / `SetMute` / `SetGain` / `SetIfGain`.  Loaded with
  `RTLD_LAZY|RTLD_GLOBAL` and tolerates absence — no `libmi_ai.so` →
  audio disabled but the rest of the pipeline runs unchanged.
- New `src/maruko_audio.c` (~510 LOC) — full capture state machine:
  `Open(dev=0)` + `AttachIf(ADC_AB)` + `SetGain` + `SetMute` +
  `SetChnOutputPortDepth` + `EnableChnGroup`; capture thread on
  `SCHED_FIFO` doing `MI_AI_Read(0, 0, &mic, &echo, 50)` → push
  PCM into the shared `audio_ring`; encode thread does Opus / G.711 /
  L16 byte-swap and ships via RTP packetizer or compact UDP.
- New shared helper `src/audio_codec.{c,h}` — extracted Opus / G.711
  encoders + stdout filter (singleton, refcounted) from
  `src/star6e_audio.c`, ~250 LOC moved.  Star6E side switched to the
  shared helpers; `Star6eAudioState.opus_lib` / `opus_enc` replaced
  by the new `AudioCodecOpus opus`.  No behavioural change on Star6E.
- `MarukoBackendConfig` mirrors `vcfg->audio` + `vcfg->outgoing.audio_port`
  + `outgoing.max_payload_size` so the pipeline can call
  `maruko_audio_init` without taking a `VencConfig` dependency.
- `MarukoBackendContext` gains `audio` (state) + `audio_recorder_ring`
  (bridge from audio encode thread to TS recorder).  Init in
  `maruko_pipeline_configure_graph` after `bind_maruko_pipeline`,
  teardown in `maruko_pipeline_teardown_graph` after `stop_dual` and
  `ts_recorder_stop` so no consumer can race the ring teardown.
- `apply_mute = maruko_audio_apply_mute` in `maruko_controls.c`,
  closing the trivial-after-audio-lands gap from the parity matrix.
- Replaced the `maruko_runtime.c:58-60` "audio output is not supported"
  warning with the live init call.

Verify gate:
- `make verify` passes both backends.
- Bench (192.168.2.12, IMX415): `audio.enabled=true`,
  `audio.codec=opus|g711a|pcm` → `[audio] Initialized` + RTP arrives on
  the configured destination port.  Mute toggle via
  `/api/v1/set?audio.mute=true` cuts audio cleanly.

Caveats:
- The SSC378QE bench's analog mic wiring is unverified.  `MI_AI_Open`
  succeeds and `MI_AI_Read` returns frames, but the actual codec on this
  board may not be wired to a microphone — capture may yield silence.
  The init path still completes successfully so the userspace pipeline
  itself is exercised; an analog mic is a separate hardware fix-up.
- AO (playback) is intentionally not exposed yet — reserved for a
  future Phase 5b once a use case appears.

## [0.9.14] - 2026-05-02

Maruko parity Phase 6 — TS recording (`record.mode="mirror"` / `"dual"`).

Lights up on-device TS-mux recording on Maruko by reusing the Star6E
TS recorder state machine.  Two modes wired:

- **mirror**: chn 0 frames are written to the .ts file alongside the
  RTP stream.  Single encoder, simplest case.
- **dual**: chn 1 (created via Phase 7's `start_dual()`) drains into
  the .ts file while chn 0 keeps streaming RTP to the configured
  destination.  The chn 1 drain thread feeds the recorder; the chn 0
  loop is guarded so it never co-writes.

Audio is video-only for now (no audio backend on Maruko — Phase 5).
Raw `.hevc` format is rejected with a warning; only `format="ts"` is
implemented.

Implementation notes:
- Promoted `src/star6e_recorder.c`, `src/star6e_ts_recorder.c`,
  `src/ts_mux.c` from `STAR6E_ONLY_SRC` to a new `RECORDER_SRC` list
  built by both backends.  No `#ifdef PLATFORM_*` was needed — the
  files only depend on type names from `star6e.h`, which Maruko
  already pulls in for `MI_SYS_ChnPort_t`.
- Added a small adapter `src/maruko_ts_recorder.c` that pulls NAL
  units out of `i6c_venc_strm` (Maruko) and feeds the shared
  `star6e_ts_recorder_write_video()` primitive.  Mirrors
  `star6e_ts_recorder_write_stream()` 1:1.
- `MarukoBackendConfigRecord` extended with `dir`, `format`,
  `max_seconds`, `max_mb` (already present on the generic
  `VencConfigRecord`).
- New `Star6eTsRecorderState ts_recorder` field on
  `MarukoBackendContext`; opened in `configure_graph` after
  `start_dual` so the drain thread sees it ready.  Closed in
  `teardown_graph` after `stop_dual` (no race window).

Verified on 192.168.2.12 (OpenIPC SSC378QE / IMX415, no SD card —
written to `/tmp` tmpfs).  Live test pending in the next session.

Out of scope for this PR (deferred):
- Raw `.hevc` recorder on Maruko (the `_write_frame` adapter still
  takes Star6E's `MI_VENC_Stream_t`; will land alongside Phase 5
  audio if anyone wants it).
- HTTP `/api/v1/record/start|stop` for Maruko (daemon-config-driven
  only for now).
- Audio mux into the TS container (Phase 5).

## [0.9.13] - 2026-05-02

Maruko parity Phase 3 — BMI270 IMU port (opt-in via `imu.enabled`).

Wires the existing platform-agnostic `src/imu_bmi270.c` into the
Maruko pipeline so `imu.enabled=true` reads gyro + accel from a
BMI270 over I2C (frame-synced FIFO mode, 200 Hz default ODR).  The
push callback is currently a stub — samples are read and discarded —
so this lands the lifecycle but no consumer yet.  Future telemetry /
sidecar export plugs into the existing callback slot without touching
init/teardown.

Verified on 192.168.2.12 (OpenIPC SSC378QE / IMX415 1472x816@120,
H.265 25 Mbps RTP):
- BMI270 detected at `0x68` on `/dev/i2c-1` (chip_id=`0x24`).
- 400-sample auto-bias (~2 s) completes cleanly; gyro bias ≈
  (0.005, -0.006, -0.001) rad/s on bench.
- 200 Hz FIFO drain runs every video frame; 1963 samples / 9 s of
  streaming at 118 fps, 0 read errors.

- **Maruko-specific ordering constraint.**  IMU init must run BEFORE
  `MI_VENC_StartRecvPic` (i.e. before `bind_maruko_pipeline()`)
  because the auto-bias loop blocks the main thread for ~2 s
  (400 samples @ 200 Hz).  Empirically on Maruko, blocking the main
  thread for 2 s after StartRecvPic leaves the VENC fd in a state
  where `poll()` never returns POLLIN and the stream loop never
  progresses.  Star6E does not exhibit this — IMU init can stay
  post-VENC there.  The constraint is captured inline in
  `maruko_pipeline_configure_graph()` so future re-orders don't
  regress.
- `src/imu_bmi270.c` moved from `STAR6E_ONLY_SRC` to `HELPER_SRC`
  (already platform-agnostic; no `#ifdef PLATFORM_*` in the file).
- New `imu` field on `MarukoBackendContext` (`ImuState *`, NULL when
  disabled).  `MarukoBackendConfig` carries `VencConfigImu imu`
  embedded from `vcfg->imu` so the pipeline does not reach into
  `VencConfig` directly (consistent with the `show_osd` /
  `keep_aspect` bridge fields).
- Per-frame `imu_drain()` runs in `maruko_pipeline_process_stream()`
  before `MI_VENC_GetStream` (Star6E parity at
  `star6e_runtime.c:727`).  No-op when `ctx->imu == NULL`.
- Stop/destroy in `maruko_pipeline_teardown_graph()` ahead of any
  unbind/stop, mirroring Star6E.
- No config-schema change.  `imu.enabled` default remains `false`,
  so existing setups are untouched.

## [0.9.12] - 2026-05-02

Maruko parity Phase 9 — opt-in 3A CPU throttle (`isp.aeMode`).

The 1080p120 H.265 25 Mbps profile burned ~62% of a single Cortex-A7
core on Maruko (SSC378QE).  Per-thread sampling pinned ~17.5% to
libcus3a.so's `3A_Proc_0` worker (spawned by `MI_ISP_EnableUserspace3A`,
runs at sensor frame sync) plus matching kernel ISP/VENC kthread time.
The host pipeline was already lean (~3% main thread) so the ceiling
was the per-frame 3A loop — not anything we packetize or send.

The cut: keep `MI_ISP_EnableUserspace3A` so the IQ→HW pump (the same
`3A_Proc_0` thread) keeps writing saturation/sharpness/brightness to
silicon, but swap the SDK's NATIVE AE algorithm for a no-op stub via
`MI_ISP_CUS3A_RegInterfaceEX(ADAPTOR_1)`.  AWB stays NATIVE so white
balance still tracks the scene (the 4096/1024/1024 R/G/B gains in the
SDK demo turned out to be smoke-test values, not calibrated daylight
gains — letting the bin drive AWB gives a usable picture).  AE is
then driven by a 15 Hz supervisory thread (`src/maruko_cus3a.c`) that
reads the 128x90 luminance grid via `MI_ISP_AE_GetAeHwAvgStats` and
applies a three-stage cascade (shutter → sensor gain → ISP digital
gain) via `MI_ISP_CUS3A_SetAeParam`.

- New config field **`isp.aeMode`** (`"native"` default,
  `"throttle"` opt-in).  Default preserves existing behaviour and
  gives a safety hatch if a different sensor / firmware breaks the
  no-op adaptor.  Live mode change requires restart (the adaptor swap
  and `CUS3A_Enable` flags are init-only); SIGHUP at runtime is a
  documented limitation.
- New module **`src/maruko_cus3a.c` + `include/maruko_cus3a.h`**
  (≈700 lines).  `_install_noop_adaptor()` registers the AE stub;
  `_start()` launches the 15 Hz controller thread; `_stop()` joins
  on teardown.  `MarukoCus3aConfig.throttle_mode` gates the
  AE-control law — when 0 the thread still runs cap-enforcement +
  stats reads (Star6E-equivalent behaviour).
- **Pipeline integration** (`src/maruko_pipeline.c`): adaptor install
  is gated on `cfg.ae_mode == "throttle"` after `MI_SNR_SetFps`;
  thread starts when `ae_fps > 0` regardless of mode.  Default-on bin
  gain ceiling switched to `bin_max_sensor_gain` (8192 on IMX415)
  when user sets `gainMax=0`, fixing the previous over-bright bias
  (the old default capped at a 32× synthetic ceiling).
- **Config / fixture**: `config/venc.default.json` adds
  `"aeMode": "native"` so the round-trip layout test passes.

Verified on 192.168.2.12 (SSC378QE / IMX415 1472x816@120fps,
H.265 25 Mbps RTP):
- `aeMode=native` (default): unchanged behaviour, ~50% sys CPU.
- `aeMode=throttle`: ~36% sys CPU (≈24 percentage-point drop on a
  single-core SoC), `3A_Proc_0` ticks 89→36 per 3 s sample, AE
  responds visibly to scene changes at 15 Hz, IQ knobs
  (saturation / sharpness / brightness) still hot-apply.

## [0.9.11] - 2026-05-02

Maruko parity Phase 2b — debug OSD now functional (kernel-oops cured).

Verified on 192.168.2.12 (OpenIPC SSC378QE, kernel 5.10.61): with
`debug.showOsd=true`, RGN init/create/attach/getcanvas all succeed,
encode loop runs at ~117 fps, no kernel taint, OSD canvas mapped at
1472x816 stride 736.

- **Root cause was a build-time conditional bug, not a kernel/lib
  mismatch.**  The Maruko build defines BOTH `-DPLATFORM_STAR6E` and
  `-DPLATFORM_MARUKO` (the Star6E backend's MI shim headers are reused
  for type compatibility; see `Makefile:39`).  In Phase 2,
  `src/debug_osd.c` started with `#ifdef PLATFORM_STAR6E`, so the
  Star6E branch was compiled into the Maruko binary too — and the
  Star6E ABI (1-arg `MI_RGN_Init(palette*)`, mod_id 0 = VPE, 3-arg
  `AttachToChn`) ran against the Maruko kernel/lib pair.  That
  ABI mismatch produced the `MI_DEVICE_Ioctl → kfree → compound_head`
  oops with a userspace-shaped pointer (`r0=0x0f9c0900`) reaching
  kfree.  Fixed by changing the first conditional to
  `#if defined(PLATFORM_STAR6E) && !defined(PLATFORM_MARUKO)` so
  Maruko binaries enter the proper Maruko branch.
- **`debug_osd`: Maruko ABI branch (now active).**  Targets the
  OpenIPC libmi_rgn.so v3 API as documented in the SigmaStar
  Infinity6C BSP headers (`mi_rgn.h`) and used by the vendor's
  official IPC demo (`common/osd/osd.cpp`):
  `MI_RGN_Init(soc_id, palette*)` (palette as direct arg, not wrapped),
  3-arg `MI_RGN_Create(soc_id, handle, attr*)`, 4-arg
  `MI_RGN_AttachToChn(soc_id, handle, chnport*, param*)`, 64-bit
  `MI_PHY` / pointer-width `MI_VIRT` in `MI_RGN_CanvasInfo_t`,
  module ID 34 (`E_MI_MODULE_ID_SCL` — RGN is attached to SCL/0/0/0).
- **`maruko_mi`: pre-load `libmi_rgn.so`.**  Added to the existing
  RTLD_GLOBAL dep chain in `maruko_mi_init()` (alongside
  `libcam_os_wrapper`, `libmi_common`, `libispalgo`, `libcus3a`) so the
  later `dlopen` from `debug_osd.c` finds the dependency graph fully
  resolved.  (`src/maruko_mi.c`)
- **`maruko_pipeline`: init-before-kthread ordering.**  Moved
  `debug_osd_create()` ahead of `bind_maruko_pipeline()` so it runs
  after the SCL channel exists (`maruko_start_vpe`) but BEFORE
  `MI_VENC_StartRecvPic` spawns the encoder kthread.  The v5.10
  OpenIPC kernel mi_rgn driver requires the singlethread workqueue
  to be created from the main task.  Dropped the Phase 2 safety-gate
  WARN-and-skip — the runtime is now real.  (`src/maruko_pipeline.c`)

Recipe cross-referenced with `waybeam-hub/src/rgn_backend_maruko.c`,
which had already verified the dep preload + module-ID-34 pattern
against the same kernel/lib pair (different `MI_RGN_OsdChnPortParam_t`
trailing field — the current SigmaStar Infinity6C BSP `mi_rgn_datatype.h`
omits `stColorInvertAttr` that the hub's older vendored header
includes; both work because the kernel reads only the union prefix).

## [0.9.10] - 2026-05-02

Maruko parity Phase 2 — debug OSD overlay wired to both backends.

- **`maruko_pipeline`: debug OSD plumbed.**  Mirrors Star6E
  (`star6e_runtime.c:825-849`):
  `debug_osd_create()` runs at the end of
  `maruko_pipeline_configure_graph()` after VENC bind+start (gated on
  the new `cfg.show_osd`, sourced from `debug.showOsd`),
  `debug_osd_begin_frame / sample_cpu / text / end_frame` runs
  per frame inside `maruko_pipeline_process_stream()` showing fps + cpu,
  and `debug_osd_destroy()` runs at the top of
  `maruko_pipeline_teardown_graph()` before any unbind/stop.  Both
  backends now share `src/debug_osd.c` + `src/debug_osd_draw.c`
  (moved from `STAR6E_ONLY_SRC` to `HELPER_SRC`).  When
  `debug.showOsd=false` (default) no OSD code runs on either backend.
- **Maruko runtime path safety-gated.**  On the test target
  (192.168.2.12, OpenIPC SSC378QE), invoking `MI_RGN_Init` triggers a
  kernel Oops in `MI_DEVICE_Ioctl` (kfree path) and wedges the encode
  loop — the same lib/kernel SDK vintage mismatch documented in
  `memory/maruko_osd_render_bringup.md`.  Until Phase 2b ships the cure
  (RTLD_GLOBAL dep preload, `MI_MODULE_ID_SCL`=34, init-before-worker-
  thread; tracked in `documentation/MARUKO_PARITY_PLAN.md`),
  `debug.showOsd=true` on Maruko emits a one-time warning and skips the
  attach so a stale config never hangs venc.  Star6E behaviour is
  unchanged.  (`src/maruko_pipeline.c`, `src/maruko_config.c`,
  `include/maruko_config.h`, `include/maruko_pipeline.h`, `Makefile`)

## [0.9.9] - 2026-05-02

Maruko parity Phase 1 — aspect-ratio precrop on the SCL stage.

- **`maruko_pipeline`: SCL precrop wired up.**  `configure_maruko_scl()`
  now writes a centered crop rect into `scl_port.crop` instead of zero
  when `isp.keepAspect=true` and the encode aspect ratio differs from the
  sensor's effective output.  The rect is computed via the existing
  `pipeline_common_compute_precrop()` helper (Star6E parity) against the
  post-binning effective input (`sensor.plane.capt` clamped by
  `mode.output`), so it always matches the surface that actually feeds
  the SCL stage.  Falls back to zero crop (full source area, downstream
  stretch) when `isp.keepAspect=false`.  `venc_api_set_active_precrop()`
  is called on success for `/api/v1/config` visibility, and
  `venc_api_clear_active_precrop()` runs in
  `maruko_pipeline_teardown_graph()` for symmetry with Star6E.  Verified
  on bench (192.168.2.12, IMX415): 960x720 (4:3) on 1920x1080 sensor
  mode → `Precrop: 1920x1080 -> 1440x1080 (offset 240,0)`, encoding
  89 fps @ 25 Mbps without stretching; 1280x720 (16:9) and
  `keepAspect=false` paths both produce the legacy zero-crop output.
  (`src/maruko_pipeline.c`, `src/maruko_config.c`,
  `include/maruko_config.h`, `include/venc_config.h`,
  `documentation/PRECROP_ASPECT_RATIO.md`)

## [0.9.8] - 2026-05-02

Frame-drop fix: relax the SDK FrameLost rate-control threshold from 120% to
150% of target bitrate.  Affects both Star6E and Maruko (shared
`pipeline_common_frame_lost_threshold`).

On Maruko at 5 Mbps / 60 fps, hand-wave motion routinely caused the
encoder's measured output to spike to ~6.0–6.4 Mbps — past the old 120%
floor (6.144 Mbps).  `MI_VENC_SetFrameLostStrategy(NORMAL)` then dropped
whole frames as a safety net, costing 5–10 fps under motion even though
CBR rate control would have absorbed the overshoot in the next few frames
via QP feedback.

Confirmed via per-frame timing on device: user-space loop work stayed
≤230 µs avg / ≤365 µs max (well under the 16.67 ms 60-fps budget) in both
modes; the missing frames were skipped at the SDK encoder layer, not by
FIFO eviction.  Disabling `frameLost` entirely on test eliminated the
drop; raising the threshold to 150% restores the safety net for genuine
sustained network overload (>50% over target) while letting motion bursts
through.

- **`pipeline_common.c::pipeline_common_frame_lost_threshold`**: change
  margin from `bits / 5U` (20%) to `bits / 2U` (50%).  Floor of 524288
  bits (~512 kbps headroom) preserved for low-bitrate streams.
- No config-schema change.  `frameLost` default remains `true`.

Verified on 192.168.2.12 (SSC378QE / IMX415 @1920x1080 / 60 fps,
H.265 5 Mbps RTP): under continuous hand-wave motion, app-observed FPS
stays at 59 (vs 54–55 before); on-device VENC frame-arrival jitter goes
from 33.3 ms (one-frame skip) to a flat 16.7 ms.

## [0.9.7] - 2026-05-02

May 2026 code-review follow-up bundle (PRs P1+P2+P3+P4+P5 squashed).

- **P2 — `httpd`: Content-Length parser anchored.**  The HTTP request
  parser previously located `Content-Length:` via an unanchored
  case-insensitive substring search across the entire header block,
  which would latch onto the literal substring inside an arbitrary
  header value (e.g. `X-Forwarded: content-length:99`).  Fix walks
  the header block line by line and only matches at the start of a
  line.  Eliminates a request-smuggling vector against the live
  config / set endpoints; also drops the now-unused
  `httpd_strcasestr` helper.  (`src/venc_httpd.c`)

- **P4 — Move orphaned `snr_*` harnesses to `tools/`.**
  `snr_sequence_probe.c` and `snr_toggle_test.c` were never wired
  into the build (orphaned since 0.6.2).  Relocated under `tools/`
  to match the rest of the standalone diagnostic harnesses; updated
  the path reference in `documentation/REFACTORING_PLAN.md`.  No
  Makefile changes required.

- **P3 — `main`: pidfile + flock single-instance gate.**  The legacy
  guard scanned `/proc/*/comm` for a process named `venc`, which is
  inherently racy: two near-simultaneous launches each see no peer
  and both proceed to grab the SHM ring.  New `acquire_pidfile_lock()`
  takes an exclusive non-blocking `flock(LOCK_EX | LOCK_NB)` on
  `/var/run/venc.pid` (falling back to `/tmp/venc.pid`) before the
  legacy `/proc` scan; `EWOULDBLOCK` exits cleanly with rc=1.  The
  fd is held with `O_CLOEXEC` and intentionally leaked so the kernel
  releases the lock at process exit even on SIGKILL.  Old `/proc`
  fallback retained as defence-in-depth for the case where pidfile
  lock acquisition itself fails (e.g. read-only fs).  Updated comment
  in `src/venc_ring.c` to reference the new mechanism.
  (`src/main.c`, `src/venc_ring.c`)

- **P1 — `venc_api`: shrink `g_cfg_mutex` hold time.**  The live-set
  apply path held `g_cfg_mutex` across `stage_params_into_cfg` and
  `preflight_live_group_callbacks` even though both operate purely
  on stack-local config copies and do not read or mutate the shared
  `g_cfg` pointer.  Moved both calls outside the mutex region; the
  mutex is now only held during `apply_live_group_sequence_locked`,
  which is the irreducible window where backend `apply_*` callbacks
  read additional vcfg fields beyond their parameters via the
  registered `&ctx->vcfg`.  No behavior change; reduces serialization
  in the rapid-fire `/api/v1/set` path used by the link_controller.
  Documented the irreducible hold-time contract in the mutex
  declaration's doc comment.  (`src/venc_api.c`)

  Note on scope: the original CR claim of "torn-string reads at
  120 fps" did not match any reachable code path — backend reads
  of `vcfg->...` strings happen only at init/teardown or inside
  the `apply_*` callbacks (under the mutex).  The deeper deferred
  work-queue rework deferred until the apply-callback contract is
  changed.

- **P5 — `maruko_pipeline_run()` split.**  Reduced from 301 lines
  to ~50 by extracting `maruko_pipeline_init_streaming`,
  `maruko_pipeline_cleanup_streaming`,
  `maruko_pipeline_check_idle_abort`,
  `maruko_pipeline_await_frame`,
  `maruko_pipeline_process_stream`, and
  `maruko_pipeline_log_verbose_frame` into `MarukoStreamRuntime`
  helpers.  Outer loop now mirrors the Star6E shape
  (`star6e_runner_run` + `star6e_runtime_process_stream`).
  Behavior preserved: idle-abort timer (`MARUKO_IDLE_ABORT_US`),
  idle-warn timer (`MARUKO_IDLE_WARN_US`), FPS-kick, pressure-gating,
  verbose-cadence (`MARUKO_PKTZR_VERBOSE_ACTIVE`), cached pack
  reuse, POLLERR fallback.  (`src/maruko_pipeline.c`)

- **Tests.**  `tests/test_venc_httpd.c` gains coverage of the new
  Content-Length walker (anchored match, case-insensitive,
  multi-header, header-value-confused-with-body, missing/oversized,
  negative).  `scripts/test_pidfile_lock.sh` exercises the flock
  gate by launching two short-lived test processes and asserting
  the second exits with `EWOULDBLOCK`.  Existing `test_multi_set_*`
  cases continue to cover the now-mutex-free stage/preflight path
  in `apply_live_set_query`.

## [0.9.2] - 2026-04-28

Transport-pressure observability (the prior "Level 2 — local FPS skip"
plan was rolled back; see the post-encode-skip note at the end of this
section).

- **Universal fill source.** Producer-local backpressure now works
  uniformly for every output transport with a queue model:
    `shm://`   `(write_idx - read_idx) / slot_count`
    `unix://`  `SIOCOUTQ / SO_SNDBUF`
    `udp://`   `SIOCOUTQ / SO_SNDBUF`
  For UDP-over-WiFi the kernel send queue rarely fills (NIC drains
  fast) so the gate is a no-op in practice and the radio-link layer
  (`waybeam_wfb_ng/link_controller`) does the actual adaptive control.
  For local UDP and unix:// (e.g. gstreamer consumer) a slow consumer
  fills the kernel queue and the gate trips correctly — empirically
  validated on Linux 6.x by sending raw datagrams to a non-reading
  receiver and watching SIOCOUTQ rise to SO_SNDBUF cap.
- **Level 1 — observability.** New `GET /api/v1/transport/status`
  endpoint returns the active output transport (`"shm"` / `"udp"` /
  `"unix"` discriminator), queue fill, lifetime delivery counters
  (SHM only — `packetsSent`, `transportDrops`, `oversizeDrops`),
  live watermark config, current hysteresis state (`inPressure`),
  and the producer-local `pressureDrops` counter (all transports).
  Also new `query_transport_status` callback in `VencApplyCallbacks`
  (Star6E + Maruko both implement).  For unix:// / udp:// the
  socket-side lifetime counters are not yet tracked — `transportDrops`
  and `packetsSent` are absent from the JSON for those transports;
  the sidecar trailer carries 0s.  Future work: count
  sendmsg(EAGAIN/ENOBUFS) and successful sends in `output_socket_send_parts`.
- **Sidecar trailer.** `RTP_SIDECAR_FLAG_TRANSPORT_INFO` (0x04) +
  `RtpSidecarTransportInfoWire` (16 bytes).  Appended after the
  optional ENC_INFO trailer when any non-zero transport is active.
  Forward-compat: probes that don't recognise the flag read just the
  base frame (and ENC_INFO if present) and ignore the trailing bytes
  — no protocol version bump.
- **Hysteresis-driven pressure flag (telemetry only).**  Hardcoded
  watermarks `VENC_PRESSURE_HIGH_WATER_PCT=75` /
  `VENC_PRESSURE_LOW_WATER_PCT=50` in `venc_ring.h` — no live config
  surface.  Enter pressure when fill_pct ≥ HIGH, exit when fill_pct
  < LOW.  The flag flows out through the sidecar trailer
  (`in_pressure`) and through `/api/v1/transport/status`.  The
  `pressureDrops` counter increments while the flag is asserted and
  serves as a "frames-spent-in-pressure" metric for adaptive
  consumers.  **The producer never skips a frame on the basis of
  this flag** — see the rollback note below.
- **Auto-gated observation.** `*_observe_pressure` is only called
  per-frame when a sidecar probe is subscribed
  (`rtp_sidecar_is_subscribed`); when nobody is listening, the SIOCOUTQ
  ioctl / ring-fill load is skipped entirely.  Observation caches its
  fill_pct + lifetime stats into the output struct so the sidecar emit
  in the same frame reads the cache instead of re-querying — one
  query per frame on the producer hot path instead of two.  No live
  config surface for backpressure: prior `outgoing.backpressure /
  highWaterPct / lowWaterPct` knobs were dropped (the value never
  affected anything outside the trailer once frame-skip was rolled
  back, and exposing tuning knobs for a passive telemetry signal was
  noise).
- **Architecture note.** The hysteresis state machine sits on a
  pre-computed `fill_pct` (`venc_observe_pressure` in `venc_ring.h`)
  so each backend's `*_observe_pressure` is a transport-dispatch
  helper: pick the fill source, hand it to the shared state machine,
  cache the result.  Adding a new transport is one branch in
  `observe_pressure` + one branch in `query_transport_status`.
- **Internal/wire renames.**  Identifiers were scoped from "shm_*" to
  "transport_*" / "backpressure_*" before any consumer shipped, on
  the basis that the model is equally meaningful for any transport
  with a queue.  No deprecated aliases retained — PR isn't merged
  yet so churn cost is zero.
- **Post-encode frame-skip rolled back.** The original PR also shipped
  a producer-side skip path: when the hysteresis flag was asserted,
  `star6e_runtime` and `maruko_pipeline` would bypass
  `*_video_send_frame` entirely, advance the RTP timestamp, and emit
  a sidecar message with `seq_count=0`.  Hardware testing showed the
  approach was fundamentally broken for inter-frame-coded video:
  H.264 / H.265 P-frames reference the previous frame in the GOP, so
  dropping one P-frame leaves every following P-frame in that GOP
  undecodable at the receiver.  With `gopSize=5` at 120 fps, a
  pressure storm that "looked clean" on producer-side counters
  (`packetsSent` and `transport_drops` matched `bp=ON` baseline)
  produced unreconstructable garbage at the decoder.  The skip path
  has been removed.  Adaptation under link saturation belongs
  upstream of encode — either lowering `video0.bitrate` (which
  link_controller already does from radio stats and the trailer
  signal) or lowering encoder fps (sensor-side / `MI_SYS_BindChnPort2`
  divider).  The trailer / status endpoint / hysteresis state machine
  remain valuable as a fast pressure signal for adaptive consumers;
  they just no longer pretend to act on it locally.
- **Tests added / kept:**
  - Star6E hysteresis state machine — telemetry assertions only
    (`test_star6e_output_backpressure_hysteresis`)
  - UNIX datagram pressure observation with a non-reading receiver
    (`test_star6e_output_unix_backpressure`)
  - Always-send invariant under pressure
    (`test_star6e_output_always_sends_under_pressure`)
  - Wire-layout for {enc, transport} combinations
    (`test_star6e_video_sidecar_transport_layouts`)
- **Stale-ring hardening.** `venc_ring_create()` now `shm_unlink()`s
  the name before `O_EXCL`-creating a fresh inode, instead of
  `O_CREAT|O_TRUNC` reusing the existing one. After a SIGKILL'd venc
  is restarted while the old wfb_tx still has the ring mmapped, the
  old consumer keeps reading the orphaned inode (no SIGBUS, no race
  through magic=0/init_complete=0) until its watchdog detaches and
  re-attaches by name to the new inode. Pairs with the consumer-side
  per-iter epoch guard in `waybeam_wfb_ng/poc/shm-input.patch`. New
  regression test: `test_producer_restart_orphans_old_inode`.

## [0.9.1] - 2026-04-28

Live `outgoing.max_payload_size` (`/api/v1/set?outgoing.maxPayloadSize=...`):

- **Promoted from `MUT_RESTART` to `MUT_LIVE`.** The new size takes effect
  on the next encoded frame; the in-flight frame finishes packetizing at
  the old size, so a switch can never tear a single frame's FU/AP
  fragmentation. Composes with other live fields in a single multi-set
  request, e.g. `?video0.bitrate=8000&outgoing.maxPayloadSize=4000`.
- **Range validated to `[576, 4000]`** in `validate_field_cfg()`. Same
  validation now also gates boot via `venc_api_validate_loaded_config()`,
  so a bad on-disk config refuses to start instead of crashing later.
  4000 is sized for jumbo-frame links such as Realtek's 3993-byte MTU
  (4000 + 12 RTP + 8 UDP + 20 IP = 4040, fits comfortably).
- **Per-slot scratch bumped from 1616 → 4096 bytes** in both backends
  (`STAR6E_OUTPUT_BATCH_SLOT_SCRATCH`, `MARUKO_OUTPUT_BATCH_SLOT_SCRATCH`).
  Required so the sendmmsg() batch can hold an AP packet up to the new
  4000-byte limit. ~159 KiB extra per backend (64 slots × 2.4 KiB delta).
- **SHM parity with UDP/Unix.** SHM rings are sized at startup to fit
  the validated ceiling (`VENC_OUTPUT_PAYLOAD_CEILING_BYTES + 12` =
  4012 bytes per slot, 8-byte aligned), so `shm://` accepts the full
  live range without a restart-to-grow caveat. Costs ~1.3 MiB extra SHM
  per ring vs. the previous "size to configured starting value" scheme,
  but this is paid only when SHM output is actually configured. UDP and
  Unix datagram transports have no transport-level cap — only the
  validated range and scratch ceiling apply.
- **wfb_tx (or any SHM consumer) compatibility note.** The published
  `slot_data_size` in the ring header changes from
  `startup_max_payload + 12` (typically 1412) to a fixed 4012 after this
  release. Well-behaved consumers using `venc_ring_attach()` already
  read `slot_data_size` from the header and compute slot stride from
  it, so they handle the change automatically. A consumer that hard-
  codes a 1412-byte slot stride or uses a fixed-size read buffer below
  4012 will need to be updated.
- **Audio path tracks live updates too.** `Star6eAudioOutput.max_payload_size`
  is now updated in the live apply alongside the video state, so audio
  compact-mode chunking uses the new value on the next audio frame
  (RTP audio doesn't fragment, so the field is unused there but kept in
  sync for future-proofing).
- New optional callback `VencApplyCallbacks.apply_max_payload_size`,
  implemented in `star6e_controls.c` (covers dual-stream second channel)
  and `maruko_controls.c`.
- Cleanups while in the area:
  - `Star6eOutputSetup.max_frame_size` field and the
    `max_payload` parameter to `maruko_output_init_shm` were both
    rendered dead by sizing SHM rings to the ceiling; removed along
    with the now-redundant `*_output_max_payload_cap` helpers and
    SHM cap checks (validation is the single gate).
  - Removed a stale `outgoing.max_payload_size` paragraph in
    `HTTP_API_CONTRACT.md` that described an "adaptive algorithm" no
    longer in the codebase.

## [0.9.0] - 2026-04-26

Two themes shipped together:

### SIGHUP / `/api/v1/restart` cold restart via fork+exec (Star6E)

SIGHUP / `/api/v1/restart` rebuild the full pipeline including a
sensor-mode change, via process-level fork+exec respawn (Star6E).

- **Single reinit path.**  `SIGHUP`, `GET /api/v1/restart`,
  `GET /api/v1/defaults`, and `MUT_RESTART` `/api/v1/set` all enqueue
  the same request: clean teardown of the running venc, fork a child
  that execv's `/proc/self/exe`, parent exits.  The child inherits
  zero MI/ISP/sensor state from the kernel driver — it's a true cold
  boot at the SDK level, identical to a `killall venc; venc &` cycle.
- **`venc_api_request_reinit()` collapsed** from `int mode` (0/1/2
  priority queueing) to bool.  Every call site — SIGHUP handler,
  `/api/v1/restart`, `/api/v1/defaults`, `MUT_RESTART` set — enqueues
  the same request.
- **Removed:** `star6e_pipeline_reinit()` and
  `star6e_pipeline_stop_venc_level()` (partial-reinit codepath
  replaced by process-level respawn).  See git history for the
  abandoned escape hatch.
- **New helpers:**
  - `star6e_runtime_respawn_pending()` — `main()` checks this after
    backend teardown.
  - `star6e_runtime_respawn_after_exit()` — fork+execv successor.
  - `prctl(PR_SET_NAME, "venc-wd")` in the watchdog fork so the new
    venc's `is_another_venc_running()` skips it.
- **Audio `g_ai_persist` hack kept** — pipeline_stop's
  MI_AI_Disable cycle deadlocks `CamOsMutexLock` and would hang the
  parent's teardown past the watchdog window.  Kernel cleans up AI
  state on process exit anyway.
- **Watchdog timeouts** (process exit only): `alarm()` 5 → 2 s,
  watchdog poll 8 × 1 s → 6 × 500 ms, post-`SIGKILL` grace 3 → 1 s,
  VENC drain 500 → 150 ms.  ISP channel wait kept at 2000 ms (bench
  testing showed cuts cause "ISP channel readiness timeout" warnings).
- Maruko backend untouched (the rcvalue-vs-bool ripple from the
  reinit signature change is the only edit; the Maruko in-process
  reinit path stays).
- Docs: `documentation/SIGHUP_REINIT.md` rewritten with the
  fork+exec design and full bench evidence;
  `documentation/LIVE_FPS_CONTROL.md` "Mode Switching Limitation"
  section removed; `documentation/CRASH_LOG.md` added with the
  sysrq-b remote-recovery trick.

**Bench validation (2026-04-26, imx335 @ 192.168.1.13):** 24/24
consecutive cross-mode SIGHUPs (modes 0→1→2→3, 6 rounds) with no
degradation, no zombies, no dmesg faults.  Cycle time 393–795 ms to
respawn marker; ~13 s to new venc HTTP up (cold start dominates).
Phase 1 plan's in-process `MI_SYS_Exit` + `MI_SYS_Init` approach was
empirically disproven (PID-tied "already_inited" flags trip
`MI_DEVICE_Open` hangs); partial-reinit-without-MI_SYS-Exit survives
~4 cycles before VIF bindmode sync errors.  Process-level respawn is
the only path that scales.

### Hand-rolled config pretty printer (stable disk layout for `/etc/venc.json`)

- **Replace `cJSON_Print` in `venc_config_save`** with a hand-rolled
  emitter (`config_render_pretty` in `src/venc_config.c`). Every WebUI
  `/api/v1/set` save and every `record.path` save now produces the same
  canonical layout: 2-space indent, one key per line, `": "` separator,
  no blank lines between sections, single trailing newline. cJSON's
  tab-indented "shattered" pretty print is gone from the disk path.
  Parsing and HTTP API responses still use cJSON unchanged.
- **Unified layout for `config/venc.default.json`.** The hand-authored
  irregular layout (some sections one-line, others multi-line, mixed
  per-section indent rules) is replaced by the same canonical layout the
  printer emits, so the default file matches what venc actually writes
  on first save.
- **Self-policing round-trip test** (`test_save_layout_byte_equal` in
  `tests/test_venc_config.c`): loads `config/venc.default.json`, saves
  via `venc_config_save`, asserts the saved bytes are byte-equal to the
  original. Any future config field added to the struct/parser/serializer
  but missing from the printer (or the default file) trips the test.
- **`AGENTS.md` sync rules updated**: the per-section `render_*` helper
  in `src/venc_config.c` is now an explicit sync point alongside the
  struct, parser/serializer, API field+alias tables, WebUI `SECTIONS[]`,
  and `config/venc.default.json`.
## [0.8.1] - 2026-04-25

SD-card recording browser (dashboard tab + JSON API):

- **New `Recordings` tab on the dashboard** (`web/dashboard.html`).
  Fourth tab next to `Settings | API Reference | Image Quality`; the
  REC indicator in the top bar is now clickable and switches to it.
  Lists `.ts`/`.hevc` files in the configured `record.dir`, shows
  free/total bytes, current `record.mode`, live `recording:` state
  with frames + bytes + segment counter, start/stop buttons, per-file
  download + delete.  A 2 s poll of `/api/v1/recordings` +
  `/api/v1/record/status` runs while the tab is visible so the active
  recording's counters tick live; interval is cleared on tab switch.
- **Mode-aware start/stop gating.**  When `record.mode` is `dual` or
  `dual-stream` and `record.enabled` is true, the dedicated recording
  thread owns the recorder and the runtime silently skips the HTTP
  `/api/v1/record/start|stop` poll (`star6e_runtime.c`'s `if (!ps->dual)`
  guard).  The tab now reads `record.mode` from `/api/v1/config` and
  disables the Start/Stop buttons with a reason note in those cases,
  instead of letting clicks succeed but produce nothing.  Full truth
  table in `README.md`.
- **New JSON endpoints** (documented in `HTTP_API_CONTRACT.md`):
  - `GET /api/v1/recordings` — list files with `name`, `size`, `mtime`
    plus `free_bytes` / `total_bytes`.  Built in a growing heap buffer
    with proper JSON escaping (no silent truncation, no corruption of
    filenames containing `"` or `\`).  Capped at 512 entries; the
    response includes a `truncated` flag that the UI surfaces as a
    warning so the user doesn't assume old files vanished.
  - `GET /api/v1/recordings/download?file=<name>` — streams the file
    via the new shared `httpd_send_file()` helper (`send(MSG_NOSIGNAL)`
    so a mid-download client disconnect can't kill the server; RFC 5987
    `filename*=UTF-8''…` header).
  - `GET /api/v1/recordings/delete?file=<name>` — `unlink()`.  Refuses
    the currently-recording file by comparing inodes via `(dev, ino)`
    (a path-string compare would be defeated by trailing slashes or
    symlinks); returns 409 `record_active` in that case.
- **Shared httpd plumbing**, reusable by future endpoints:
  - `httpd_query_param()` — URL-decoding (percent + `+`) query parser
    in `venc_httpd.c`.
  - `httpd_send_file()` — streams a file with proper `Content-Length`
    and `Content-Disposition`; shares socket helpers already used by
    the rest of the server.
  - `venc_api_get_record_dir()` + `venc_api_fill_record_status()` —
    mutex-safe accessors exposing the active config directory + live
    recorder state (used to safely read config from the httpd thread).
- **Safety.**  Filenames validated (no path separators, no leading `.`,
  no control bytes); all JSON output properly escaped; no large stack
  frames on the httpd thread (entries buffer is `calloc`'d).
- **HTTP API contract** bumped `0.6.2` → `0.6.3` (non-breaking: three
  new endpoints, one new error code `record_active`).
- **Tests.**  `tests/test_venc_httpd.c` +11 cases for
  `httpd_query_param` covering percent-decoding, `+` → space, UTF-8,
  prefix collisions, empty values, trailing / invalid `%`, and buffer
  truncation safety.
- **Build.**  `tools/build_webui.py` and Makefile updated: dashboard
  blob re-regenerated from `web/dashboard.html`; new `src/venc_recordings.c`
  added to `CONFIG_SRC` and `TEST_LIB_SRCS`.

Implements the intent of PR #48 (PaddyP90) while fixing the safety
bugs flagged in that review: filename validation including `..`, JSON
escaping, heap buffer sizing, disconnect handling, and mutex-safe
config access.  Star6E only; Maruko lacks the `MI_VENC_TS_RECORDER`
plumbing so `/api/v1/record/status` reports `active: false`
there — but the list/download/delete endpoints still work for files
placed in `record.dir` by other means.

Originally drafted as a 0.7.12 fork-only release before the v0.8.0
upstream catch-up; rebased onto v0.8.0 and re-released as 0.8.1.

## [0.8.0] - 2026-04-25

Drop the EIS module and migrate the Star6E debug OSD from
`MI_RGN_PIXFMT_ARGB4444` (16 bpp) to `MI_RGN_PIXFMT_I4` (4 bpp,
two pixels per byte).  Major bump because `eis.*` config fields and
the EIS internal headers are removed; nothing else in the public HTTP
API or config schema breaks.

### EIS removal

An empirical sensor-mode sweep on Star6E IMX335 established that EIS
only worked in one validated config (sensor mode 3 native 1920x1080
+ real IMU + ≤90 fps); every other combination silently stalled the
encoder via `MI_VPE_SetPortCrop` interactions with VPE scaling,
VIF-side crop, or pixel-rate ceilings.  Increasingly elaborate guards
were added to refuse EIS in the broken cases, but the surface area
isn't worth maintaining for a single-config feature, and a future
LDC-warp rewrite (Phase C in `documentation/EIS_INTEGRATION_PLAN.md`)
would replace this code anyway.

- **Removed:** `src/eis.c`, `src/eis_gyroglide.c`, `include/eis.h`,
  `include/eis_gyroglide.h`, `include/eis_ring.h`,
  `tests/test_eis_gyroglide.c` (~1100 LoC).
- **Pipeline init:** EIS init/teardown blocks deleted from
  `bind_and_finalize_pipeline()` and `pipeline_stop()` /
  `pipeline_stop_venc_level()`.  All VPE-scaling / VIF-crop / testMode /
  pixel-rate refusal guards are gone with them — they only existed to
  make EIS misconfiguration loud.
- **Per-frame:** `eis_update()` removed from
  `star6e_runtime_process_stream()`.  `imu_drain()` still runs
  per-frame so a future telemetry consumer slots in cheaply.
- **OSD:** EIS visualization (1/3-scale crop miniature in
  bottom-right + crop/off/margin text rows) removed from
  `star6e_runtime.c`.  Debug OSD now shows only fps and CPU.
- **Config:** `eis.*` fields dropped from `VencConfig`,
  `venc.default.json`, the `/api/v1/set` snake-case alias map,
  and the WebUI dashboard (tab + tooltips + enum).
- **IMU module retained.**  `src/imu_bmi270.c` + `include/imu_bmi270.h`
  stay in the build; `imu.enabled` defaults to `false` so the BMI270
  is never opened unless explicitly enabled.  The push callback
  `star6e_pipeline_imu_push()` is now a stub — samples are discarded
  unless a future consumer (telemetry export, gcsv-style file logging
  for Gyroflow post-process) is wired in.

### Debug OSD: ARGB4444 → I4 format migration

Two-step migration delivered in one release.  First the rasterizer is
extracted into a pure host-testable module, then the MI_RGN backing
format is dropped from 16 bpp ARGB4444 → 8 bpp I8 → 4 bpp I4
palette-indexed.  Canvas footprint at 1920x1080 goes from 4.0 MB to
1.0 MB; OSD-on CPU drops by 74 % on Star6E IMX335.  Encoder hot path
is unchanged — the win is entirely in the OSD-on cost.

- **Pure rasterizer extracted.**  New `src/debug_osd_draw.{c,h}` holds
  the font, palette, dirty-rect logic, and drawing primitives.  The
  MI_RGN glue in `src/debug_osd.c` is now a thin wrapper.  The pure
  module compiles on the host and is exercised by
  `tests/test_debug_osd.c` (76 assertions covering every primitive,
  clipping, dirty-rect expansion, glyph rendering, and hashed
  composite-scene goldens).
- **OsdCanvas API.**  `stride_px` renamed to `stride_bytes` (now bytes
  per row regardless of pixel format).  `width` is still logical
  pixels.  `osd_fill_pixels(canvas, x, y, count, color)` handles I4
  nibble alignment internally:
    - Unaligned start (odd x): RMW the high nibble of byte (x/2).
    - Byte-aligned middle: `memset` the doubled-nibble byte
      `(color << 4) | color` over `(end - x) / 2` bytes.
    - Unaligned tail (end odd): RMW the low nibble of the last byte.
  Drawing primitives (`osd_draw_rect`, `osd_draw_char`) call this in
  place of their old byte-pointer inline math.  `osd_get_pixel` reads
  back the unpacked nibble through the same code path the rasterizer
  uses; production drawing never reads back.
- **Palette: 16 entries** (was 256 in I8, ARGB4444 in 16 bpp).
  Entries 1..8 map to `DEBUG_OSD_*` color constants; semi-transparent
  entries reuse the 4-bit ARGB4444 codes (0x4 → 68, 0xA → 170) so
  visual output is unchanged vs. the ARGB4444 implementation.  Entries
  9..15 are zeroed reserved.
- **MI_RGN region pixfmt:** `MI_RGN_PIXFMT_I4`.  Wire stride at
  1920x1080 is 960 bytes (was 1920 for I8; 3840 for ARGB4444).
- **Track-points use case** (filled small rects from upstream PR #23,
  motion-vector markers): supported unchanged via existing
  `debug_osd_rect` API; dirty-rect tracking gives sub-canvas clear,
  more efficient than the upstream's full-canvas `memset(0xFF)`.
- **Public API source-compatible.**  `debug_osd_rect/point/line/text`
  signatures unchanged; `DEBUG_OSD_*` constants are still palette
  indices (now 0..15 max).

Hardware comparison (Star6E IMX335 @ 90 fps, 30 s samples):

| Format          | Actual fps | CPU/core   | OSD-on cost vs baseline |
|-----------------|-----------:|-----------:|------------------------:|
| ARGB4444 (pre)  |      87.00 |    26.80 % |              +20.53 pp |
| I8 (intermediate) |    90.00 |    17.43 % |              +11.30 pp |
| **I4 (this)**   |  **90.00** | **11.70 %** |          **+5.43 pp** |

I4 cuts OSD CPU −74 % vs the original ARGB4444 path, and hits 2.36×
the fps-per-CPU% of ARGB4444 (7.69 vs 3.25).

### Bundles fork-only history

This release subsumes fork-only intermediate tags 0.7.12 (OSD I8
extraction), 0.7.13 (OSD I8 → I4), and 0.7.14 (EIS removal).  Bundled
because they share a hardware test footprint and were never released
upstream as separate tags.

## [0.7.11] - 2026-04-19

Pre-merge review fixes prior to upstream sync.  Two functional bugs
plus four polish items, all verified end-to-end on Star6E (IMX335 at
192.168.1.13) and Maruko (IMX415 at 192.168.2.12).

- **B1 — Maruko: split ISP-bin gate from CUS3A gate.**  v0.7.10's
  auto-detect fallback was silently a no-op on Maruko reinit because
  both `pipeline_common_resolve_isp_bin` and `maruko_load_isp_bin`
  were gated under `g_mi_isp_initialized`, which is set once and never
  cleared.  Resolve+load now runs every configure with a Star6E-style
  `g_last_isp_bin_path` cache; CUS3A enable + cold-boot exposure cap
  stay one-shot under the deadlock-protection gate.  Verified: 3
  successive `/api/v1/restart` cycles transitioned configured ->
  auto-detect fallback -> new configured -> restored.
- **B2 — IDR rate limiter: CAS loop for thread-safe spacing.**  The
  load-then-store pattern on `last_us` left a race where two
  concurrent producers could both pass the spacing check on the same
  `last` value and both honor an IDR inside the window — the exact
  storm-coalescing guarantee the gate was added for.  Replaced with
  `__atomic_compare_exchange_n` so exactly one caller wins each
  window.  ACQ_REL on the winning store synchronizes with the next
  caller's ACQUIRE load.  Verified: 100 concurrent `/request/idr` ->
  9 honored / 91 dropped (~10 honored/s, matches 100 ms spacing over
  the ~1 s curl burst).  All 18 idr_rate_limit unit tests still pass.
- **M1 — `venc_config_save`: preserve symlinks + mode bits.**  Resolve
  `path` via `readlink()` before writing the temp file so a symlinked
  `/etc/venc.json` is replaced in-place rather than being replaced by
  a regular file.  Preserve the existing target's mode bits via
  `stat()`+`fchmod()` so saves no longer silently widen 0600/0640 to
  0644.  Open the directory with `O_DIRECTORY`, propagate dir-fsync
  errors, retry trailing-newline write on EINTR.  Verified live:
  symlink intact, target mode 0640 preserved.
- **M2 — `/api/v1/defaults`: pick reinit mode based on save success.**
  The handler unconditionally requested reinit mode 1
  (reload-from-disk).  On disk-save failure (`EROFS` / `ENOSPC` /
  perm), the reload silently overlaid the stale on-disk config onto
  the in-memory defaults and reverted most of them.  Use mode 2
  (apply in-memory) when `save_rc != 0` so the operator at least gets
  the defaults they asked for at runtime.
- **M3 / L1 / L3 — Star6E hygiene.**
  - `prepare_pipeline_config`: stale comment about isp_bin_path
    resolution location refreshed (v0.7.10 moved it from
    `select_and_configure_sensor` to `bind_and_finalize_pipeline`).
  - `stop_venc_level`: stop and join `dual_rec_thread` BEFORE
    `star6e_output_teardown(&dual->output)`.  The thread calls
    `star6e_video_send_frame(&dual->output, ...)` inside its loop;
    tearing down output first left a use-after-close window.
  - `dual_rec_thread_fn`: always `usleep` after Query-empty (100 us on
    the fd path, 1 ms on the fallback) to prevent a runaway spin if
    the kernel ever signals POLLIN spuriously without a matching
    packet.

## [0.7.10] - 2026-04-19

Discoverable defaults + automatic ISP-bin selection:

- **`config/venc.default.json` lists `video0.size`.**  The field defaulted
  to `"auto"` in the parser but wasn't in the reference JSON, so users
  copying the file as a template never saw it.  Added with the same
  `"auto"` value.
- **Automatic ISP-bin fallback (both backends).**  New
  `pipeline_common_resolve_isp_bin()` runs after `sensor_select` and:
  1. Uses `isp.sensorBin` if non-empty and readable.
  2. Otherwise tries `/etc/sensors/<lowercase prefix>.bin` keyed off the
     live sensor name (`IMX335_MIPI` → `imx335`).
  3. Falls back to "no bin" (driver defaults).

  Stock devices that already ship `/etc/sensors/imx335.bin`,
  `/etc/sensors/imx415.bin`, etc. now run without per-host config.  A
  configured-but-missing path logs a warning and uses the fallback so
  typos and renamed bin files no longer cripple AE/AWB.  Logs the
  resolution decision once per pipeline start: `> ISP bin: %s
  (configured | auto-detected for sensor 'imx335') | none (no fallback…)`.
- **Star6E `Star6ePipelineConfig.isp_bin_path`** changed from
  `const char *` to `char[256]` to hold the resolved path.  Maruko
  `MarukoBackendConfig.isp_bin_path` got the same treatment.
- **Tests.**  10 new cases in `test_pipeline_common`: configured +
  readable, configured + missing, NULL/empty sensor name, no-alnum-prefix
  sensor name, NULL/zero output buffer (1190 tests, was 1180).

## [0.7.9] - 2026-04-19

Aspect-ratio crop is now opt-out (Star6E):

- **`isp.keepAspect` config (default `true`).** When `false`, VIF
  captures the full sensor area and VPE stretches it to the encode
  dimensions instead of center-cropping to preserve geometry. New
  parameter on `pipeline_common_compute_precrop()` keeps both call sites
  branch-free. Maruko parses but ignores the field until SCL crop port
  lands.
- **Reinit path now tracks the precrop currently programmed in VIF**
  (`state->active_precrop`) and compares against the freshly computed
  rect. A `keepAspect` toggle that doesn't change image dimensions now
  correctly triggers a VIF+VPE reconfigure. The previous code only
  re-armed precrop when `image_width`/`height` differed from the prior
  config.
- **Reinit branch reorganized.** `if (precrop_changed)` runs the full
  VIF+VPE rebuild; `else if (dims_changed)` falls through to the
  VPE-port-only resize. Equal-on-both-counts skips the block. The prior
  shape (outer `if (dims_changed)` then inner precrop check) couldn't
  express the keepAspect-toggle case, and the obvious patch-style fix
  would have re-fired the VPE-port resize on no-change reinits.
- **Reinit log shows precrop.** `> Reinit: VIF+VPE reconfigure %ux%u
  -> %ux%u (precrop %ux%u+%u+%u)` so an operator can tell whether the
  trigger was a resolution change, an AR change, or a keepAspect toggle.
- **HTTP API contract bumped to 0.6.2.** `isp.keepAspect` field added to
  `/api/v1/config`, `/api/v1/set` accepts both snake_case and the
  Majestic-style camelCase alias.
- **WebUI dashboard exposes the toggle** under the ISP section; embedded
  gzip regenerated via `make webui`.  Also drops the stale `isp.exposure`
  entry that lingered after the field was removed in 0.7.0.
- **Active precrop visible via API.** New
  `venc_api_set_active_precrop()` / `venc_api_get_active_precrop()` are
  called from the Star6E pipeline whenever VIF is (re)programmed.
  `/api/v1/config` gains a `runtime.active_precrop` block alongside the
  config; Star6E `/api/v1/ae` includes the same rect under
  `data.runtime.active_precrop`.  Maruko reports nothing until it gains
  precrop support.  Useful for confirming a `keepAspect` toggle landed
  without grepping the log.
- **Unit tests.** New `compute_precrop` cases cover both `keep_aspect`
  values plus the 2-pixel alignment guarantee, and a new
  `test_active_precrop_setter` exercises the venc_api setter/getter
  including the cleared-store, overwrite, and NULL-out-pointer paths
  (1180 tests, was 1139).

## [0.7.8] - 2026-04-18

Pre-merge review fixes folded in (see PR-47 review notes):

- **Atomic config write.** `venc_config_save()` now writes to `<path>.tmp`,
  fsyncs the file, renames over the target, and fsyncs the containing
  directory.  Power cut mid-write (a real failure mode on FPV hardware)
  no longer truncates `/etc/venc.json` — you always get either the old
  or the new copy, never a partial.
- **Flash-write guard.** `venc_api_save_config_to_disk()` caches the
  last successfully-saved VencConfig and skips the write when the
  candidate is byte-identical.  Hot loops (adaptive-link re-asserting
  the same kbps, WebUI sliders landing on their current value) no
  longer wear flash.
- **Save errors surface.** `venc_config_save()` return value is now
  honored.  `/api/v1/defaults` response gains `"saved":bool`.  LIVE /
  RESTART `/api/v1/set` paths log a `WARNING: config save to X failed
  — in-memory change committed but on-disk copy is stale` to stderr so
  operators catch disk-full / readonly-FS conditions from the venc log.
- **SDK call return values logged.** `MI_SNR_SetOrien`,
  `MI_VPE_SetChannelParam` (reinit), and `MI_SNR_SetFps` (reinit) now
  log non-zero returns so BSP regressions surface instead of silently
  leaving the image upside-down or the sensor stuck at the wrong FPS.
- **Dashboard source tracked.** HTML authored in `web/dashboard.html`;
  `tools/build_webui.py` regenerates the embedded gzip deterministically
  (mtime=0, compresslevel=9).  New `make webui` and `make webui-check`
  targets; `make verify` runs `webui-check` to catch drift.

- **WebUI reinit + IDR fixes.** Four related bugs in the reinit/save
  path and one missing IDR-on-bitrate behaviour.
- **Fix #1 — FPS kick on live reinit.** `star6e_pipeline_reinit`
  (`src/star6e_pipeline.c`) now re-kicks `MI_SNR_SetFps` at the end so
  a live FPS change actually reconfigures sensor timing.  Previously
  the kick only fired during the initial `star6e_pipeline_start_vpe`
  legacyAe branch and the once-per-process CUS3A `fps_kick_done`
  gate — neither re-armed on reinit, so the sensor stayed stuck at
  its cold-boot timing (e.g. 100 fps when 120 was requested).
- **Fix #2 — Save & Restart actually saves.** Added
  `venc_api_set_config_path()` (called by star6e_runtime and
  maruko_runtime with `VENC_CONFIG_DEFAULT_PATH`).  Both LIVE
  (`apply_live_set_query`) and RESTART (`process_restart_set_query`)
  set paths now call `venc_config_save()` before returning, so every
  `/api/v1/set` round-trip persists to `/etc/venc.json`.  `handle_restart`
  is intentionally left pure (reload-from-disk only, matching SIGHUP);
  the per-set save takes care of persistence so the WebUI "Save &
  Restart" flow ends with the on-disk copy already matching memory.
  Bonus: manual file swaps (e.g. scp of a config backup) followed by
  `/api/v1/restart` reload exactly what was written.
- **Fix #3 — Restore Defaults actually restores.** New
  `GET /api/v1/defaults` endpoint (`handle_defaults`) writes
  compiled-in defaults to disk and triggers reinit.  WebUI JS
  `restoreDefaults()` rewired from `/api/v1/restart` to the new
  endpoint (embedded gzip regenerated).  Previously the button just
  reloaded the on-disk config — misleading, and did nothing if the
  file already matched in-memory state.
- **Fix #5 — IDR on bitrate change.** `apply_bitrate` in both
  `src/star6e_controls.c` and `src/maruko_controls.c` now issues
  `MI_VENC_RequestIdr` after `MI_VENC_SetChnAttr`, gated through the
  existing `idr_rate_limit_allow` so storm callers stay coalesced.
  The decoder now gets a fresh keyframe to resync against the new
  rate-control state instead of drifting on stale P-frames.
- **Fix #4 — image.mirror / image.flip** now re-apply on reinit.
  `MI_VPE_SetChannelParam` is only invoked during VPE creation inside
  `star6e_pipeline_start_vpe`; the non-aspect-ratio reinit path skipped
  VPE rebuild, so a mirror/flip toggle would persist to disk and log
  "Pipeline reinit complete" but never actually change the output.
  Added an unconditional `MI_VPE_SetChannelParam(0, ...)` at the end
  of `star6e_pipeline_reinit` carrying the current mirror/flip/3DNR
  params.  Config round-trip verified on 192.168.1.13; visual flip
  verification requires a live decoder (operator check).

## [0.7.7] - 2026-04-18

- **Perf-series PR-C.1 — port MI_VENC_GetFd + poll() blocking wait to
  the Maruko main encoder loop.** Follow-up to PR-C (Star6E dual_rec
  only).  Maruko's main encoder loop in `maruko_pipeline_run`
  (`src/maruko_pipeline.c:1291`) was spinning on `maruko_mi_venc_query
  + usleep(500)` — ~2000 syscalls/s during idle gaps.  Replaced with
  `poll(MI_VENC_GetFd, 1000 ms)` and a wall-clock idle-abort timer
  (20 s of no frames → abort, preserved from the original
  `idle_counter * 500us` logic).
- **Fallback preserved:** if `MI_VENC_GetFd` returns < 0 on an unknown
  BSP variant, the loop falls back to the original Query+usleep(500)
  spin.  POLLERR/POLLHUP/POLLNVAL on the fd path drops into the
  fallback for the rest of the run.
- **Lifecycle:** `MI_VENC_CloseFd` called at cleanup when the fd was
  acquired.  The fd function pointers were already loaded by
  `maruko_mi.c` (dlsym'd but unused before this PR).
- **New bindings** (`include/maruko_bindings.h`): `maruko_mi_venc_get_fd`
  and `maruko_mi_venc_close_fd` macros alongside the existing
  MI_VENC_* wrappers.
- **Wall-clock idle timeout** consolidates the old dual idle paths
  (500us-keyed counter) into a single `wb_monotonic_us()`-based
  deadline that works identically on both the fd path (rare wakeups)
  and the fallback spin path (frequent wakeups).

## [0.7.6] - 2026-04-18

- **Perf-series PR-C — dual_rec_thread blocking wait via MI_VENC_GetFd.**
  Third of the 2026-04-18 perf series.  Replaces the 1-ms `usleep` spin
  in the dual-recorder thread with a `poll()` on the VENC channel's
  kernel fd (`MI_VENC_GetFd`).  The fd signals `POLLIN` when a frame is
  ready, so the thread wakes once per frame (~120/s at 120 fps) instead
  of ~1000/s from the old 1 ms spin — ~88 % fewer syscalls during
  recording.
- **Fallback preserved:** if `MI_VENC_GetFd` returns < 0 on an unknown
  BSP variant, the thread falls back to the original
  `MI_VENC_Query + usleep(1000)` loop — zero behaviour change on SDKs
  that don't expose the fd.
- **Lifecycle:** `MI_VENC_CloseFd` is called on thread exit when the fd
  was acquired.  The fd function pointers were already loaded by
  `star6e_mi.c` / `maruko_mi.c` (dlsym'd but unused before this PR).

## [0.7.5] - 2026-04-18

- **Perf-series PR-B — IDR request rate-limit gate.** Second of the
  2026-04-18 perf series.  Addresses the latent stability hazard where
  five independent IDR producers (scene detector, HTTP `/request/idr`
  and `/api/v1/dual/idr`, controls-apply, recorder-start) could storm
  `MI_VENC_RequestIdr` without coordination — a bug-driven burst
  (mis-tuned scene threshold during a camera pan) can crater per-frame
  bitrate by chaining forced keyframes.
- **New module (`include/idr_rate_limit.h`, `src/idr_rate_limit.c`):**
  per-channel (up to 8) last-honored timestamp + honored/dropped
  counters.  `idr_rate_limit_allow(chn)` enforces a compile-time
  `IDR_RATE_LIMIT_MIN_SPACING_US` of 100 ms — at 120 fps that is 12
  frames between honored forced IDRs, well below the GOP period
  (~83 ms at GOP=10, which auto-inserts an IDR without RequestIdr).
  State is lock-free (`__atomic_` load/store on `uint64_t`/`uint32_t`).
- **Wired through the 5 producer sites:**
  - `src/star6e_runtime.c` — `star6e_scene_request_idr`,
    `runtime_request_idr`
  - `src/star6e_controls.c` — `request_idr` (backend callback for HTTP
    `/request/idr`)
  - `src/venc_api.c` — `handle_dual_idr` (HTTP `/api/v1/dual/idr`);
    coalesced response returns `{"coalesced":true}`
  - `src/maruko_pipeline.c` — `maruko_scene_request_idr`
  - `src/maruko_controls.c` — `apply_qp_delta` IDR reissue
- **New endpoint `GET /api/v1/idr/stats`** returns per-channel honored
  and dropped counts plus the configured `min_spacing_us`.  Used by
  `tools/idr_storm.sh` to validate the gate.
- **Unit tests (`tests/test_idr_rate_limit.c`, 20 cases):** first-call
  honored, burst coalescing, per-channel independence, out-of-range
  bypass, post-spacing honored, reset semantics.  1139 tests pass
  (up from 1119).

## [0.7.4] - 2026-04-18

- **Perf-series PR-A — clock wrapper + dual_rec Query dedup + bench infra.**
  First of a five-PR series landing the post-review performance findings
  from 2026-04-18 (see `bench/perf-series/README.md`).
- **Clock reads via vDSO (`include/timing.h`, `src/timing.c`):** New
  `wb_monotonic_us()` helper using `CLOCK_MONOTONIC` (vDSO fast path on
  ARMv7, ~100 ns/call) instead of `CLOCK_MONOTONIC_RAW` (real syscall,
  ~1500 ns/call on A7).  Replaces three duplicated local wrappers —
  `monotonic_us` in `star6e_video.c` and `maruko_pipeline.c`, and
  `now_us` in `rtp_sidecar.c`.  NTP slew is <500 ppm → <4 us drift over
  a 60 s bench window, well inside frame-timing measurement error.
- **dual_rec backpressure signal (`src/star6e_runtime.c`):** Replaced the
  post-`MI_VENC_ReleaseStream` peek `MI_VENC_Query` with an inspection of
  the pre-`GetStream` `stat.curPacks >= 2` condition.  Equivalent
  semantics (queue had a backlog before we consumed) at one fewer syscall
  per recorded frame (~120/s at 120 fps).
- **Perf-series bench harness (`bench/perf-series/`):** New
  `run_bench.sh` drives the Tier A/B/C bench recipe end-to-end (deploy,
  probe, collect); `compare.py` emits a markdown Delta table between two
  labels with a 1.5×sigma regression flag.  Baseline tag
  `perf-series-baseline` pinned at master `40b8435`.
- **Host microbench (`tools/clock_bench.c`):** 1 M-iteration loop over
  `CLOCK_MONOTONIC_RAW`, `CLOCK_MONOTONIC`, `CLOCK_MONOTONIC_COARSE` to
  validate the vDSO assumption on A7 before deploying the PR.
- **IDR-storm stress (`tools/idr_storm.sh`):** Infrastructure for PR-B
  validation; fires N `POST /api/v1/dual/idr` back-to-back and reports
  the honored:fired ratio.

## [0.7.3] - 2026-04-14

- **Star6E sidecar gate (parity with Maruko PR #37):** Gated the per-frame
  `rtp_sidecar_poll` / `monotonic_us` / `rtp_sidecar_send_frame` work in
  `star6e_video_send_frame` on `state->sidecar.fd >= 0`.  When the
  sidecar feature is disabled (port 0), these calls are now skipped
  entirely rather than relying on each callee's early return.
- **SHM write: iovec-style 3-segment ring put (`venc_ring.h`, both backends):**
  Added `venc_ring_write3(hdr, p1, p2)` so the producer no longer has to
  pre-flatten `payload1 + payload2` into an 8 KB `flat[]` stack buffer
  before calling `venc_ring_write`.  Drops one memcpy per fragmented RTP
  packet (H.265 FU), removes the 8 KB stack allocation, and eliminates
  the `RTP_BUFFER_MAX` size clamp on the SHM write path.
  Applied to `src/star6e_output.c::star6e_output_send_rtp_parts` and
  `src/maruko_video.c::maruko_video_send_rtp_parts`.
  `venc_ring_write` is preserved as a thin wrapper for existing callers
  (C and C++, including the wfb_tx patched consumer).

## [0.7.1] - 2026-04-12

- **Phase 5 — Maruko HEVC RTP parity (PR #32):** Extracted the HEVC RTP
  output stage into a shared `hevc_rtp` module (`include/hevc_rtp.h` +
  `src/hevc_rtp.c`). Both Star6E and Maruko now go through the same
  Aggregation Packet (type 48) builder, FU-A fragmentation, VPS/SPS/PPS
  prepend-on-IDR, and per-frame `HevcRtpStats`. `star6e_hevc_rtp.c` is
  now a thin stream-iteration wrapper (227 lines → 111 lines);
  `Star6eHevcRtpStats` becomes a typedef alias of `HevcRtpStats` so
  existing call sites are unchanged. Maruko's RTP output gets standards-
  compliant AP aggregation for the first time: hardware-validated on
  SSC378QE at H.265 CBR 118 fps / 8 Mbps — IDR frames pack
  VPS+SPS+PPS+IDR as a single AP packet (`ap 1/6` in `[pktzr]` verbose
  line) instead of 4 separate RTP datagrams.
- **`[pktzr]` verbose line on Maruko:** Matches Star6E's exact format
  (`nals N | rtp N | fill N B | single N | ap N/N | fu N`) so log
  tooling works across both backends.
- **H.264 RTP output removed from Maruko:** Maruko ships H.265-only on
  the RTP wire path. The H.264 path was never hardware-verified and
  Maruko's FPV use case is H.265 exclusive. Channel creation still
  accepts `codec=h264` for forward compatibility, but the frame sender
  emits a warning and drops output. Net -~130 lines in `maruko_video.c`.
- **New `test_hevc_rtp` suite** (3 tests, 16 assertions): AP packing of
  small NALs, AP→FU-A fallback on oversized NALs, VPS/SPS/PPS prepend
  behavior — uses a capture-callback harness (no sockets) so tests run
  in <1 ms. Existing Star6E AP/FU-A test still passes unchanged as
  regression guard.

## [0.7.0] - 2026-04-11

- **dlopen migration (both backends):** Both Star6E and Maruko now load all
  MI vendor libraries (SYS, VIF, VPE, VENC, ISP, SCL, SNR) at runtime via
  dlopen/dlsym instead of direct linking. Function pointers are dispatched
  through `_impl` structs (`g_mi_sys`, `g_mi_vif`, etc.) with macro wrappers
  so call sites are unchanged. Three-way preprocessor guards
  (`PLATFORM_STAR6E` / `PLATFORM_MARUKO` / test stubs) keep all paths clean.
  - Star6E: dependency-ordered loading (cam_os_wrapper → SYS → ISP/CUS3A
    with RTLD_LAZY for circular deps → VIF/VPE/SNR/VENC with RTLD_NOW).
  - Maruko: eliminated uClibc shim and 3+ MB of redundant libs on device.
  - New files: `star6e_mi.h/.c`, `maruko_mi.h/.c`.
  - Removed: `-lmi_*` link flags, `MARUKO_UCLIBC_DIR`, shim build rules.
- **Maruko IQ parameter system:** Full 60-parameter ISP image quality API
  for Maruko (Phase 2), matching Star6E's existing IQ support. Includes
  multi-field struct params (colortrans, r2y, OBC, etc.), dot-notation
  set, and export/import. New files: `maruko_iq.h/.c`.
- **Maruko sensor mode diagnostics:** Auto-cap exposure to sensor FPS
  for reliable 120fps cold-boot. Fix SCL clock configuration. Gain
  control and exposure callback improvements.
- **Disable AF in CUS3A:** Fixed-focus cameras (IMX415) no longer trigger
  AF motor init errors. All CUS3A enable sequences changed from
  `{1,1,1}` (AE+AWB+AF) to `{1,1,0}` (AE+AWB only). Post-override
  after EnableUserspace3A which internally re-enables AF.
- **Star6E VPE exit(127) fix:** Under dlopen, vendor MI_VPE_DisablePort
  calls exit(127) on non-existent channel. Fixed by probing channel
  with MI_VPE_GetChannelAttr before VPE teardown.
- **Bool cast safety:** MI_SNR_GetPlaneMode vendor function writes 4 bytes
  through a `_Bool*` pointer. Fixed with temp-int wrappers on both backends.
- **Known issues documented:** Maruko encoder stall after output
  disable/re-enable (`documentation/KNOWN_ISSUES.md`).
- **Build cleanup:** Removed dead `snr_toggle_test` and `snr_sequence_probe`
  build recipes (unbuildable without direct MI linking). Removed stale
  uClibc references from deploy scripts and docs.
- Added `sensors-src` submodule pointing to OpenIPC/sensors for sensor
  driver source reference.
- Added IMX335 IQ profile (`iq-profiles/imx335_greg_fpvVII-gpt200.json`).

## [0.6.1] - 2026-04-03

- Fix cold-boot 54fps lock with legacyAe: call MI_SNR_SetFps during
  pipeline startup to force sensor timing compliance when CUS3A is not
  active.
- Fix sidecar telemetry NULL pointer: enriched encoder feedback was
  never sent (enc_ptr was NULL instead of &enc_info).
- Scene detector: saturate frame_count to prevent EMA warmup re-entry
  after ~13h; cache frame_size/type to avoid redundant packet walks;
  skip spike logic entirely when disabled (threshold=0).
- Change video0.scene_threshold and scene_holdoff to MUT_RESTART
  (no live-apply pathway exists).
- Remove all enc_ctrl/encCtrl references from code and documentation.

## [0.6.0] - 2026-04-02

- Add inline scene detector in star6e_runtime.c (~150 lines) behind
  `video0.scene_threshold` config field.
  - Tracks frame size EMA, computes complexity (0-255).
  - Detects spikes above configurable threshold for holdoff consecutive frames.
  - Waits for spike to subside before requesting IDR (when threshold>0).
  - Two config fields: `video0.scene_threshold` (uint16, 0=off, 150=1.5x EMA
    spike detection), `video0.scene_holdoff` (uint8, default 2).
  - Default off (`scene_threshold=0`): no IDR injection — zero-risk default.
- Enrich RTP timing sidecar with per-frame encoder telemetry:
  `frame_type`, `complexity`, `scene_change`, `idr_inserted`,
  `frames_since_idr`.
- Add multi-field set to HTTP API: `GET /api/v1/set?a=1&b=2` applies
  multiple live fields atomically in one request.
- Add field capabilities endpoint with backend-specific support filtering:
  `GET /api/v1/capabilities` reports mutability and per-backend support.
- API improvements: camelCase alias table for Majestic-compatible clients,
  duplicate-field rejection in multi-set, mixed live/restart rejection.

## [0.5.0] - 2026-04-01

- Add debug OSD overlay for encoder diagnostics and EIS crop visualization.
  Disabled by default (`debug.showOsd`), zero runtime cost when off.
  - MI_RGN canvas overlay via dlopen — ARGB4444 pixel format, full-frame canvas
    with dirty-rect tracking (only clears/draws changed areas per frame).
  - Stats panel (top-left): fps counter, CPU% from /proc/stat, 3x scaled 8x8
    bitmap font with semi-transparent background.
  - EIS crop visualization (bottom-right): 1/3 scale miniature showing sensor
    area (white), margin boundary (yellow), and moving crop window (green fill).
  - NEON-accelerated row fill (vst1q_u16, 8 pixels per store, 2.4x vs naive).
  - Mutually exclusive with waybeam-hub `mod_osd_render` — both use MI_RGN
    global state on VPE channel 0.
  - Config: `"debug": { "showOsd": true }`, API: `debug.show_osd` (MUT_RESTART).
  - New files: `include/debug_osd.h`, `src/debug_osd.c`.

## [0.4.1] - 2026-03-27

- Fix IMU webui fields invisible: rename config keys `sampleRate` →
  `sampleRateHz`, `gyroRange` → `gyroRangeDps` to match dashboard SECTIONS.
- Add 5 missing default config keys: `eis.mode`, `record.bitrate`,
  `record.fps`, `record.gopSize`, `record.server`.
- Fix camelCase capabilities lookup for `swapXY` and `maxMB` in webui.
- Remove legacy `sendFeedback` outgoing config alias.
- Document config/webui/API four-layer sync rules in AGENTS.md.
- Fix cold-boot sensor framerate lock: poll ISP exposure limits up to 500 ms
  instead of skipping the shutter cap when struct is all-zero. Apply synthetic
  gain defaults as fallback so AE cannot converge on exposure > frame period.
- Add IQ enable/disable toggle: virtual `.enabled` field for non-bool params
  (e.g. `colortrans.enabled=0`). Import respects `enabled` JSON field.
  Dashboard shows toggle switch in expanded form for applicable params.

## [0.4.0] - 2026-03-22

- Add built-in web dashboard at `/` with Settings, API Reference, and
  Image Quality tabs. Served as pre-compressed gzip (14KB on the wire).
- Add multi-field IQ parameter descriptors: colortrans (3 offsets + 3x3
  matrix), r2y, obc, demosaic, false_color, crosstalk, wdr_curve_adv now
  expose all sub-fields via dot-notation set API and `"fields"` JSON object.
- Add IQ export/import: `GET /api/v1/iq` exports all 62 ISP params as JSON,
  `POST /api/v1/iq/import` restores them. Partial imports supported.
- Add all missing config sections to the API: record (including dual channel
  bitrate/fps/gopSize/server), EIS (12 params), IMU (7 params), full audio
  (6 params), and ISP extras (legacyAe, aeFps). Total: 75 controllable fields.
- Add FT_FLOAT field type for EIS float params with `%.6g` precision to
  prevent artifacts like `0.001` displaying as `0.0010000000474974513`.
- Add FT_UINT8 field type for `imu.i2c_addr` — fixes memory corruption where
  `FT_UINT` wrote 4 bytes to a 1-byte field.
- Consolidate frame-loss threshold into shared function with minimum 512 kbit/s
  absolute margin for low-bitrate streams and 200 Mbps overflow clamp.
- Add `g_iq_mutex` for thread-safe IQ query/set operations.
- Add `g_dual_mutex` for thread-safe dual channel HTTP handlers.
- Fix `#ifdef` to `#if HAVE_BACKEND_STAR6E` in dual_apply_bitrate (Maruko
  link error from upstream PR #18).
- Fix stream_packs memory leak in SIGHUP reinit path.
- Fix diagnostic JSON trailing comma when dlsym lookups partially resolve.
- Add snprintf overflow protection (`JSON_CLAMP` macro) in IQ query output.
- Add EINTR handling in httpd read loops.
- Move dual channel settings from raw JSON file parsing to VencConfigRecord
  struct fields, simplifying star6e_runtime.c.
- Increase HTTPD_MAX_ROUTES to 64, HTTPD_MAX_BODY to 8192.

## [0.3.4] - 2026-03-22

- Refresh the Star6E frame-loss threshold on live bitrate changes so
  `/api/v1/set?video0.bitrate=...` keeps frame dropping aligned with the
  updated main-channel bitrate.
- Refresh the Star6E dual-channel frame-loss threshold on
  `/api/v1/dual/set?bitrate=...` so ch1 live bitrate changes keep the same
  overflow protection policy as channel creation.

## [0.3.3] - 2026-03-18

- Add Opus audio codec via `libopus.so` (loaded at runtime; graceful fallback
  to PCM if absent). RTP payload type PT=120, 48kHz nominal clock per RFC 7587.
- Fix 48kHz audio on SSC338Q — three root causes:
  - I2S clock misconfiguration: `i2s.clock` must be `0` (MCLK disabled; I2S
    master generates clock from internal PLL). Setting clock=1 caused hardware
    to deliver 16kHz data regardless of `rate` field. Source: SDK reference
    `audio_all_test_case.c` which uses `eMclk=0, bSyncClock=TRUE`.
  - Ring buffer too small: `AUDIO_RING_PCM_MAX` was 1280 (16kHz stereo
    headroom). 48kHz mono frames are 1920 bytes; silent truncation produced
    invalid Opus frame sizes → `OPUS_BAD_ARG`. Increased to 3840 (48kHz
    stereo 20ms = 960×2×2).
  - `bSyncClock` was 0; set to 1 per SDK reference.
- Fix stdout filter not active on SIGHUP reinit: `stdout_filter_start()` was
  inside `start_ai_capture()` which is skipped when AI device persists across
  reinit. Moved to `star6e_audio_init()` to run on every init cycle.
- Fix `stdout_filter_stop()` ordering: `close(pipe_read)` moved after
  `pthread_join` to avoid closing the fd while the filter thread may still
  be reading from it.
- Add `stdout_filter_stop()` to fail path and libmi_ai unavailable early-return
  to prevent filter leaks on audio init failure.
- Remove dead `star6e_audio_clock_for_rate()` function.
- Increase DMA ring: `frmNum` 8→20 (400ms), prevents data loss under ISP/AE
  preemption bursts.
- Reduce output port depth to `user=1, buf=2` (was 2,4), saving ~40ms latency.
- Audio init survives SIGHUP reinit: AI device/channel state is persisted in
  `g_ai_persist` across reinit cycles to avoid `CamOsMutexLock` deadlock after
  2+ VPE create/destroy cycles.

## [0.3.2] - 2026-03-17
- Fix SIGHUP reinit D-state: switch from full pipeline_stop/start to partial
  teardown that keeps sensor/VIF/VPE running. The SigmaStar MIPI PHY does not
  recover from MI_SNR_Disable/Enable cycles — partial teardown avoids touching
  it entirely. VENC, output, audio, IMU/EIS are torn down and rebuilt; the
  VIF→VPE REALTIME bind stays active across reinit.
- Live resolution switching: `video0.size` API change now reconfigures the
  pipeline in-process without a process restart.
  - Same-aspect-ratio changes (e.g. 1920x1080 → 1280x720): VPE output port
    resize only — VIF and VIF→VPE bind are untouched.
  - Aspect-ratio changes (e.g. 1920x1080 → 1920x1440): full VIF crop
    reconfiguration + VPE destroy/recreate. VIF device stays running;
    MIPI PHY is never touched.
  - Overscan correction applied during reinit precrop: uses `mode.output`
    (usable area) rather than `plane.capt` (raw MIPI frame) for sensors that
    report overscan in the MIPI frame dimensions.
- Guard VIF→VPE bind in `bind_and_finalize_pipeline` to prevent double-bind
  on reinit. Without the guard, re-binding an already-live VIF→VPE port
  caused continuous `IspApiGet channel not created` dmesg errors.
- ISP channel readiness poll (`star6e_pipeline_wait_isp_channel`) called
  immediately after every new VIF→VPE bind. The ISP channel initialises
  asynchronously after `MI_VPE_CreateChannel`; the poll (up to 2000 ms,
  1 ms intervals) ensures the ISP is ready before the bin load and exposure
  cap APIs probe it, eliminating `IspApiGet` dmesg errors on both cold boot
  and AR-change reinit.
- `__attribute__((flatten))` on `star6e_pipeline_reinit`: forces GCC -Os to
  inline all static callees, preserving the stack layout that the SigmaStar
  ISP driver requires for `MI_VPE_CreateChannel` to succeed.
- Error-path state consistency in VIF+VPE reconfiguration: on failure after
  VPE is destroyed, `MI_VIF_DisableDev` is called to leave the pipeline in a
  cleanly-stopped state rather than a partially-configured one.
- Details: `documentation/SIGHUP_REINIT.md`

## [0.3.1] - 2026-03-16
- Reduce G.711 audio latency: scale frame size to `sample_rate/50` (~20ms)
  instead of hardcoded 320. Reduce MI_AI ring (frmNum 16→8), output port
  depth (4,16)→(2,4), fnGetFrame timeout 100→50ms.
- Add dynamic RTP payload types: PT=112 (PCMU non-8kHz), PT=113 (PCMA
  non-8kHz). Standard PTs (0, 8, 11) still used when rate matches RFC 3551.
- Clamp audio sample_rate to 8000-48000 in config parser.
- Default audio codec changed from `pcm` to `g711a` in venc.default.json.
- Remove `slicesEnabled`/`sliceSize`/`lowDelay` config fields (no firmware support on I6E).
- Add `frameLost` config field for frame-lost strategy (default: true).
- Fix kbps verbose overflow on 32-bit ARM (displayed ~400 instead of ~13000 at high bitrates).

## [0.3.0] - 2026-03-15
- Custom 3A thread for Star6E — replaces ISP internal AE/AWB with a
  dedicated 15 Hz thread (default, no config change needed):
  - AE: proportional controller with shutter-first priority, configurable
    target luma (100-140), convergence rate (10%), and gain ceiling (20x).
  - AWB: grey-world algorithm with IIR smoothing (70/30) and 2% dead-band.
  - Pauses ISP AE via `MI_ISP_AE_SetState(PAUSE)`, disables CUS3A AWB
    callback via `MI_ISP_CUS3A_Enable(1,0,0)`.
  - Periodic ISP AE state verification with automatic re-pause.
  - Manual AWB (`ct_manual`) pauses custom AWB; `auto` resumes it.
  - `isp.exposure` API syncs max shutter to the custom AE thread.
  - Set `isp.legacyAe: true` to revert to old ISP AE + handoff behavior.
- New config fields: `aeFps`, `legacyAe` in the `isp` section.
  Gain/shutter limits now seeded from ISP bin (`MI_ISP_AE_GetExposureLimit`).
- HW verified: all 4 imx335 sensor modes (30/60/90/120fps), cold-boot,
  live FPS switching, gemini dual recording, manual AWB transitions.

## [0.2.3] - 2026-03-14
- Restored working Star6E AE across IMX335 modes `30`, `60`, `90`, and `120 fps`:
  - Startup now primes CUS3A with `100 -> 110 -> 111`.
  - Steady state no longer forces periodic `110` refreshes.
  - A delayed one-shot `000` handoff returns the pipeline to a live AE state
    while preserving the requested encoder rate.
- Added Star6E AE diagnostics for live verification:
  - `GET /api/v1/ae`
  - `GET /metrics/isp`
  - Existing `GET /api/v1/awb` remains available for AWB inspection.
- Documented the verified AE recovery and updated the HTTP API contract for
  the diagnostics endpoints.

## [0.2.2] - 2026-03-11
- Fixed GOP keyframe interval to be relative to FPS (seconds, not raw frames):
  - `gopSize` is now a float representing seconds between keyframes.
  - `1.0` = 1 keyframe/second (GOP = fps frames). `0.5` = every 0.5s. `0` = all-intra.
  - Example: `gopSize: 0.33` at 90fps = keyframe every ~30 frames.
  - Changing FPS now automatically recalculates GOP frame count.
  - Default changed from `3` (frames) to `1.0` (seconds).
- Fixed autoexposure not restoring via HTTP API:
  - Setting `isp.exposure=0` via API now correctly restores auto-exposure
    (caps max shutter to frame period). Previously it was a no-op due to
    both args being zero in `cap_exposure_for_fps(0, 0)`.
- Known issue: AWB (Auto White Balance) behavior unverified on device.
  - CUS3A enables AWB (`params[1]=1`) but actual color correction depends on
    ISP bin calibration data. Requires on-device testing. See
    `documentation/AWB_INVESTIGATION.md`.
- Known issue: ROI QP not yet wired to encoder backend.
  - Config plumbing and HTTP API exist but `apply_roi_qp` callback is NULL.
  - SDK supports overlapping ROI regions with delta QP via
    `MI_VENC_SetRoiCfg`. Implemented as horizontal bands with signed QP
    (1-4 steps). See `documentation/ROI_INVESTIGATION.md`.

## [0.2.1] - 2026-03-10
- Added audio output via UDP with configurable codec and port:
  - Supported codecs: raw PCM, G.711 A-law, G.711 μ-law (software encoding).
  - Audio captured via MI_AI SDK (dlopen at runtime, graceful degradation if unavailable).
  - New `audio` config section: `enabled`, `sampleRate`, `channels`, `codec`, `volume`, `mute`.
  - New `outgoing.audioPort` field: 0 = share video port, >0 = dedicated audio port (default 5601).
  - Audio runs in a separate thread from the video streaming loop.
  - Dual packetization: compact mode (0xAA magic header) and RTP mode (PT 110, distinct SSRC).
  - Live mute/unmute via HTTP API (`audio.mute`, MUT_LIVE).
  - Star6E backend: full implementation. Maruko backend: warning stub.
- RTP mode now reads `maxPayloadSize` from config (was hardcoded to 1200):
  - Both star6e and maruko backends respect `outgoing.maxPayloadSize` for
    RTP FU-A/FU fragmentation threshold. Default 1400.
  - Config values above 1400 are supported for jumbo-frame networks.
- Added adaptive RTP payload sizing to reduce CPU churn from packet overhead:
  - EWMA tracks average P-frame size; IDR-like spikes (>3x average) are
    excluded to prevent distortion.
  - Target payload = avg_frame * fps / targetPacketRate, aiming for ~850
    packets/sec by default across all bitrates (adaptive bitrate up to 50 Mbit).
  - `outgoing.targetPacketRate` config field (default 850, MUT_RESTART).
    Set to 0 to disable adaptive sizing and use fixed maxPayloadSize.
  - 15% hysteresis prevents oscillation on frame-to-frame jitter.
  - Payload clamped to [1000, maxPayloadSize]. The 1000-byte floor keeps
    packet rate under ~500 pkt/s on low-MCS WiFi links (MCS0 slot budget).

## [0.2.0] - 2026-03-10
- Added output enable/disable control (`outgoing.enabled`, MUT_LIVE):
  - When disabled: FPS reduces to 5fps idle rate, frames encoded and discarded.
  - When enabled: FPS restores to previous value, IDR keyframe issued.
  - Default: `false` (no more implicit localhost:5000 fallback).
- Added live destination redirect (`outgoing.server`, MUT_LIVE):
  - Change UDP destination without pipeline restart.
  - IDR keyframe issued on destination change for stream continuity.
  - Re-connects UDP socket when `connectedUdp` is enabled.
- Added stream mode config field (`outgoing.streamMode`, MUT_RESTART):
  - Values: `"rtp"` (default) or `"compact"`.
  - Replaces scheme-derived mode detection; URI scheme must be `udp://`.
- Added connected UDP (`outgoing.connectedUdp`, MUT_RESTART):
  - When true: `connect()` called on UDP socket, skips per-packet routing
    lookup and enables kernel ICMP error feedback.
- Added IDR request after live bitrate change for immediate quality update.
- Updated HTTP API contract to v0.2.0.

## [0.1.7] - 2026-02-26
- Fixed ISP FIFO stall on overscan sensor modes (imx335 mode 2 @ 90fps):
  - Added periodic CUS3A refresh (~15 Hz) in stream loop to keep ISP event
    loop alive; runs in both idle and active paths so a stalled pipeline
    can recover.
  - Fixed overscan detection: removed 10% threshold that skipped correction
    for single-axis overscan (imx335 mode 2: crop 2560x1440, output 2400x1350).
    Changed to per-axis independent clamping.
- Simplified ISP 3A management (Star6E + Maruko):
  - Replaced per-frame AE cadence toggling and ISP3AHandle/ISP3AState machinery
    with one-shot `enable_cus3a()` at pipeline init + periodic `cus3a_tick()`.
  - Removed CLI flags: `--ae-on/off`, `--awb-on/off`, `--af-on/off`, `--ae-cadence`.
- Added ISP/SCL clock boost (384 MHz) after pipeline setup.
- Added `--oc-level` for hardware overclocking:
  - Level 1: VENC clock boost to 480 MHz.
  - Level 2: Level 1 + CPU pinned to 1200 MHz with performance governor.

## [0.1.6] - 2026-02-25
- Added AE cadence control (`--ae-cadence N`) for high-FPS throughput recovery:
  - Toggles CUS3A processing on/off every N frames to reduce per-frame CPU overhead.
  - Auto mode: when FPS >60, cadence defaults to fps/15 (e.g. cadence=8 at 120fps).
  - Manual override via `--ae-cadence N` for fine-tuning.
- Moved ISP bin load earlier in pipeline setup (after start_vpe, before streaming)
  to ensure correct ae_init state before first frame.
- Added overscan crop detection for sensor modes where mode.output < mode.crop:
  - When overscan exceeds 10% on both axes, VIF center-crops to the usable output area.
  - Fixes imx415 mode 1 hang (crop=2952x1656, output=2560x1440).
  - Threshold prevents false positives from driver metadata quirks.
- Enhanced `--list-sensor-modes` to show crop/output details when they differ.
- Cleaned up pipeline summary prints: explicit MIPI frame vs cropped dimensions,
  precrop line only shown for actual aspect-ratio cropping.

## [0.1.5] - 2026-02-25
- Improved agentic coding workflow in AGENTS.md:
  - Added structured error recovery loop (observe → diagnose → repair → re-verify → document).
  - Added incremental verification guidance: run `make lint` after each logical change.
  - Added long-session guidance: progress checkpoints, decision stability, scope control.
  - Added error diagnosis reference table for compiler, linker, runtime, and timeout failures.
  - Added deployment test interpretation: exit codes, JSON summary, dmesg guidance, agent decision flow.
  - Added "Mistakes to Avoid" entries for stacking unverified changes and mid-task approach switching.
- Added `make lint` target: fast compile-only check with `-Wall -Wextra -Werror` for both backends.
- Added lint step to CI workflow (runs before build).
- Synced dual-agent infrastructure (Claude Code + OpenAI Codex):
  - Updated all `.agents/skills/` and `.claude/commands/` with decision documentation,
    error recovery loop, and incremental lint steps.
  - Added `Bash(make lint*)` to Claude permissions; switched PostToolUse hook
    from full build to fast lint for tighter feedback loop.
  - Enhanced `.codex/config.toml` with `sandbox_mode = "workspace-write"`.
- Improved `remote_test.sh`:
  - Added SSH ControlMaster multiplexing for persistent connections.
  - Removed runtime lib deployment (libs already in `/usr/lib` on target).
  - Added `--json-summary`, `--skip-build`, `--skip-deploy` flags.
  - Added strict exit codes (0=success, 1=failed, 2=unresponsive, 124=timeout).
- Added `documentation/TARGET_AGENT_ARCHITECTURE.md` design doc (deferred implementation).

## [0.1.4] - 2026-02-23
- Added automatic precrop for Star6E: when encode resolution has a different aspect ratio
  than the sensor mode, the VIF center-crops the sensor frame to match the target aspect
  ratio before the VPE scales, eliminating non-uniform scaling distortion.
- Precrop uses integer cross-multiplication (no floats) with 2-pixel alignment enforcement.
- Informational log line printed when precrop is active (e.g. `Precrop: 1920x1080 -> 1440x1080 (offset 240,0)`).
- Fixed high-FPS throttling when AE is disabled: caps exposure to frame period after ISP bin
  load, preventing default 10ms shutter from limiting 120fps mode to ~99fps.

## [0.1.3] - 2026-02-23
- Added duplicate-process guard: venc now detects and exits if another instance is already running.
- Added `--version` / `-v` flag to print version and backend name.
- Added `--verbose` flag to gate per-frame stats output (previously always printed).
- Removed obsolete HiSilicon/Goke `-v [Version]` hardware presets from Star6E backend and help text.
- Simplified sensor mode selection: prioritize FPS match over resolution fit in both backends.
- Fixed Star6E cleanup ordering: socket and ISP 3A handle now properly released on all exit paths.
- Added informational prints for FPS mismatch, resolution clamping, and VPE scaling.
- Embedded build-time version from VERSION file via Makefile (`VENC_VERSION`).
- Updated help text branding from "HiSilicon/Goke" to "SigmaStar".
- Added crash/hang tracking policy and initial crash log (`documentation/CRASH_LOG.md`).
- Added SigmaStar Pudding SDK API reference link to proc reference and documentation index.

## [0.1.2] - 2026-02-22
- Added low-risk ISP CPU-control knobs in both standalone backends:
  - `--ae-off/--ae-on`
  - `--awb-off/--awb-on`
  - `--af-off/--af-on` (default AF off)
  - `--vpe-3dnr 0..7`
- Updated ISP bin load/reapply behavior to honor requested AE/AWB/AF state.
- Added documentation for CPU/latency tuning profiles and usage:
  - `documentation/AE_AWB_CPU_TUNING.md`
- Updated status/index docs to reflect implemented 3A/3DNR tuning controls.

## [0.1.1] - 2026-02-22
- Added formal HTTP API contract source-of-truth document:
  - `documentation/HTTP_API_CONTRACT.md`
- Added repository PR checklist template with explicit contract/version/doc gates:
  - `.github/pull_request_template.md`
- Added default JSON config template and planning artifacts for config/API migration:
  - `config/venc.default.json`
  - `documentation/CONFIG_HTTP_API_ROADMAP.md`
- Updated documentation/plan/process files to enforce:
  - Star6E-first rollout for SigmaStar API-touching features,
  - contract sync on HTTP changes,
  - SemVer + changelog workflow.

## [0.1.0] - 2026-02-22
- Baseline established for standalone-only repository scope.
- Targeted dual-backend builds in place (`SOC_BUILD=star6e`, `SOC_BUILD=maruko`).
- Runtime SoC autodetect removed from `venc`; backend is selected at build time.
- Default stream behavior aligned to RTP + H.265 CBR.
- Planning updates introduced for JSON config migration and HTTP control API roadmap.
