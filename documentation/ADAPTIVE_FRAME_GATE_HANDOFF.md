# Adaptive Frame Gate — Local Takeover Handoff

<!-- version: 19.0.0 -->

> **Superseded — kept as the PR #287 takeover record.** Frozen at the
> 2026-09-12 checkpoint. Since then: the gate shipped in 0.85.0 and was
> followed up through 0.85.6; the `video0.frameGate` switch this document
> describes was removed in 0.85.0 (see HISTORY), `FRAME_GATE_MIN_OPEN_US` was
> renamed `FRAME_GATE_ESCAPE_BACKSTOP_US` in 0.85.1, and the suite is now
> ~3177 assertions, not 3115. For the current state read `HISTORY.md` and
> `documentation/REVIEW_FIX_VERIFICATION.md`; do not treat the commands below
> as a live runbook.

Written for a local agent taking over PR #287. Updated at the 2026-09-12
checkpoint after the CV610 bench pass and the SigmaStar BUSY diagnosis. No
device was modified during this final checkpoint. See §8 and §9 for exact
state and next commands.

| | |
|---|---|
| Local branch/worktree | `verify/pr-287` in `.claude/worktrees/pr-287-verify` |
| Remote PR branch | `origin/claude/dazzling-goldberg-o49554` (not pushed) |
| PR | snokvist/waybeam_venc#287 |
| Design | `documentation/ADAPTIVE_FRAME_GATE_PLAN.md` — read §7 (risks) and §9 (Phase 2 findings) |

---

## 0. Start here

```sh
cd /home/snokvist/dev/waybeam-coordination/waybeam_venc/.claude/worktrees/pr-287-verify
git status --short --branch
make test-ci          # expect 3115 passed / 0 failed
make verify           # Star6E + Maruko; toolchains auto-download on first run
```

CV610 linking and its primary R1/no-IDR trial are complete. Continue with the
unchecked recording/config transitions and the secondary SigmaStar matrix.

### The four things only this machine can do

1. Link CV610 — needs `~/dev/hisilicon` and the firmware lib output (§5).
2. Reach the current benches — Star6E `root@192.168.2.232`, Maruko
   `root@192.168.2.233`, CV610 `root@192.168.2.181`.
3. Prove R1 (`StopRecvPic` under a full egress ring) — §3.
4. Measure the defaults, which were reasoned rather than benched — §6.

## 1. What you are inheriting

`video0.frameGate` (default `off`, restart-required) pauses the stream VENC
channel's frame intake while the frame-shm egress ring is not draining, and
resumes it when the consumer catches up.

It exists because a bitrate write — the only proportional rate actuator on
SigmaStar — implicitly emits an IDR (`MI_VENC_SetChnAttr`; measured on
SSC338Q, ten spaced writes → eleven IRAP access units,
`src/star6e_controls.c:252`). So the normal response to a congested link
sends the largest frame in the stream into the link that is already
overflowing. The gate changes **no encoder state at all**, so ROI keeps its
relative QP gradient and CBR keeps its contract.

| | |
|---|---|
| Policy | `src/frame_gate.c` — pure state machine, no SDK types, 68 host assertions |
| Star6E | `src/star6e_runtime.c` — `star6e_service_frame_gate()` |
| Maruko | `src/maruko_pipeline.c` — `maruko_service_frame_gate()` |
| CV610 | `src/cv610_runtime.c` — `cv610_service_frame_gate()` |

Actuator per backend: `MI_VENC_Stop/StartRecvPic` on SigmaStar,
`ss_mpi_venc_stop_chn` / `ss_mpi_venc_start_chn(recv_pic_num = -1)` on CV610.

Defaults: close at `>= 3` slots, reopen at `<= 1` after a 20 ms minimum closed
dwell, safety escape at 500 ms. Normal reopening is **not** tied to 500 ms:
Star6E reads occupancy every ~1 ms while closed; Maruko and CV610 every ~2 ms.
Expected drain-to-video latency is that poll plus SDK start latency and about
one frame interval. The 500 ms path only admits a safety-pulse frame while a
dead/stalled consumer leaves occupancy above the reopen threshold.

## 2. What the cloud session did and did not verify

Trust the first column; re-establish the second yourself.

| Verified | NOT verified |
|---|---|
| `make verify` — Star6E + Maruko build **and link** | Any runtime behaviour, on any device |
| CV610: all 90 objects compile against real OpenHisilicon headers | CV610 **link** — vendor `.so` set absent |
| Host suite 3087/3 (68 new frame-gate assertions) | That the gate ever actually closes or reopens on hardware |
| `make lint` clean, all three backends | That `StopRecvPic` does not D-state (R1) |
| Policy logic exhaustively unit-tested | That defaults are the right numbers |

The cloud session built CV610 against `OpenIPC/openhisilicon` @ `916e767`
(2026-09-06), the tree the Makefile's `CV610_SDK_INC ?= ../openhisilicon`
default points at. If `~/dev/hisilicon` is a different revision, re-run
`make lint SOC_BUILD=cv610` against it before assuming the compile still
holds.

## 3. R1 — blocking. Do this before anything else

**Claim to disprove:** `MI_VENC_StopRecvPic` can hang when the egress ring is
full. `src/star6e_pipeline.c:2501` warns that a gap in consumption causes VPE
backpressure → kernel D-state → `StopRecvPic` hangs, and the gate's trigger
condition is adjacent to that.

**Why it is believed safe:** `venc_frame_ring_begin_write()` increments
`full_drops` and returns `-1` on a full ring
(`include/venc_frame_ring.h:424`) — the producer **drops, it does not
block** — so the output thread keeps calling `GetStream`/`ReleaseStream`,
VENC output never backs up, and VPE never backpressures. That is static
reasoning only. Prove it on hardware.

```sh
scripts/star6e_direct_deploy.sh cycle                        # root@192.168.1.13
curl "http://192.168.1.13/api/v1/set?video0.frameGate=on"    # restart-required
```

Stall the frame-shm consumer (SIGSTOP waybeam-link, or attach and stop
reading) so the ring fills, then confirm:

- the daemon logs the gate closing and does **not** wedge;
- `/tmp/waybeam.log` shows no `MMU_Callback` storm;
- `SIGCONT` → the gate reopens and video resumes.

**If it hangs, stop.** The approach is dead; say so in the PR and close it
rather than reworking around a kernel D-state. The fallback worth proposing
is `min_qp` as a binary panic switch — a cliff (§2 of the plan), but usable
for a pure emergency.

**Never `killall -9`** either bench device — `feedback_no_sigkill_sigmastar.md`.
Use SIGTERM, or sysrq-b for D-state recovery (`sysrq_b_zombie_recovery.md`).
Maruko especially wants `ssh -o ConnectTimeout=10` —
`feedback_maruko_ssh_timeout.md`.

## 4. Bench matrix

| # | Test | Pass condition |
|---|---|---|
| 1 | **R1** (§3) | `StopRecvPic` returns, no D-state, reopens on SIGCONT |
| 2 | Gate cycles under real congestion | close/reopen logged, no oscillation, video recovers |
| 3 | **No IDR on a gate cycle** | `/api/v1/idr/stats` unchanged across many cycles. This is the entire justification for the feature — if a cycle keyframes, it has no reason to exist |
| 4 | GDR survives a cycle (`resilience=racing`) | stripe cadence intact after reopen; `star6e_pipeline.c:1447` silently no-ops hierarchical attrs set after `StartRecvPic` |
| 5 | SVC-T survives a cycle (`resilience=range`) | layer structure intact, not flattened to single-layer |
| 6 | ROI gradient unaffected | center-priority QP delta unchanged across a cycle (it should be — the gate never touches `SetRoiCfg`) |
| 7 | Safety escape | kill the consumer → reopens after ~500 ms, `full_drops` climbs, stream does not stop |
| 8 | Mirror-mode recording | record in mirror mode + congest → no gate closes, file is clean |
| 9 | Recording started while gated | gate reopens, resulting file has no holes |
| 10 | SIGHUP reinit while gated | clean fork+exec respawn, no D-state |
| 11 | Non-frame-shm transport | `frameGate=on` on `udp://` warns at bring-up and stays inert |
| 12 | Maruko parity | repeat 1–3 and 7 on `root@192.168.2.12` |
| 13 | Maruko idle-abort guard | gate closed > 20 s (`frameGateMaxClosedMs=30000`) does **not** abort the stream loop. New guard, never exercised |
| 14 | CV610 | link (§5), then repeat 1–3, 7, 8 |

## 5. CV610 — link it first

CV610 is wired but has never been linked. Every `.o` builds; the final link
stops at `ld: cannot find -lacs` because the vendor `.so` set (`libss_mpi`,
`libacs`, `libbnr`, `libldci`, `libsecurec`, `libot_osal`, …) is a firmware
build output, not part of the headers tree.

```sh
make build SOC_BUILD=cv610 \
  CV610_SDK_INC=$HOME/dev/hisilicon \
  CV610_SDK_LIB=/path/to/firmware/output/target/usr/lib
```

The Makefile defaults are `CV610_SDK_INC ?= ../openhisilicon` and
`CV610_SDK_LIB ?= ../firmware/output/target/usr/lib`, so if your checkouts
sit beside the repo in that shape, a bare `make build SOC_BUILD=cv610` works
and you can drop both flags.

Two things to expect if the link surfaces anything: `src/frame_gate.c` was
missing from `CV610_SRC` and had to be added (Makefile line 83) — a missing
symbol there means that edit was lost. Otherwise the only new code is
`cv610_service_frame_gate()` and `cv610_report_frame_gate_setup()`.

**CV610-specific behaviour.** It records in mirror mode only (the stream loop
refuses every other `record.mode`), so the recorder always shares the gated
channel and the close-suppression always applies — test 8 matters more here
than on SigmaStar. Its `select()` wait drops from 1 s to 2 ms while gated.

## 6. Tuning to bring back

`frameGateCloseSlots` default 3 and `frameGateMaxClosedMs` default 500 were
reasoned from the ring header's own notes, **not measured**. Sweep both at 60
and 120 fps and record what the craft actually wants. If the defaults are
wrong, change them in `include/frame_gate.h` and say so in `HISTORY.md`.

Also worth measuring: duty cycle under sustained congestion.
`g->close_events` and `g->closed_total_us` are tracked but not exposed over
HTTP — plan §8 open question 2 asks whether they should be. If the answer is
yes, that is a small `/api/v1/status` addition and a `contract_version` bump.

## 7. Known-weak spots — look here first

- **Reopen paths.** Every path reachable with the gate closed must evaluate
  it, or the stream hangs. There are six: Star6E `curPacks == 0` and
  `Query`-failure; Maruko `!POLLIN` and `Query`-failure; CV610 the `select()`
  return and its `cur_packs == 0` / query-failure `continue`. **If video stops
  and never returns, one of these is the reason.**
- **Wait-timeout shortening.** Maruko's fd poll and CV610's `select()` both
  drop from 1 s to 2 ms while gated, because that timeout would otherwise BE
  the reopen latency. If reopens feel sluggish on either, check that.
- **`star6e_record_wants_frame()` polarity.** Closes are suppressed while a
  mirror recording runs. If the gate never fires with recording *off*, the
  predicate is inverted.
- **Mirror is the default record mode.** Easy to forget when a gate that
  "should" close does not.

## 8. State of the tree and bench results

- `make verify` passes: Star6E and Maruko build and link.
- CV610 compiles and links against the firmware vendor libraries. The linked
  0.85.0 binary is confirmed on `192.168.2.181`.
- `make test-ci`: **3115 passed, 0 failed** at the current checkpoint.
- **CV610 primary gate path confirmed on device:** 1280x720 at 100 fps,
  `resilience=racing`, frame-shm ring with eight slots. A stopped consumer
  closed intake at three slots; the 500 ms safety escape admitted one frame per
  pulse; resuming drained the ring and restored ~100 fps without a D-state or
  kernel fault.
- **CV610 no-IDR and continuity confirmed:** a direct AU consumer observed 542
  frames across a three-second gate interval, including seven safety-pulse
  frames. All 542 were GDR, zero were IDR/IRAP, Annex-B framing was clean, and
  PTS stayed monotonic. On the x86 ground hub the received rate fell from
  ~100 fps to 1.6 fps, then returned to ~100 fps; `incomplete_frames` stayed
  zero, `shm_waiting_for_idr` stayed false, and neither recovery requests nor
  source switches increased.
- **CV610 restart while gated confirmed:** graceful stop completed in one
  second with the ring held at the close threshold; the replacement process
  started with the consumer still stopped, exercised the safety escape, and
  recovered normally when consumption resumed. No D-state or new kernel fault.
- **CV610 mirror-recording rows are blocked by bench storage, not code:** the
  device has no mounted recording medium and `/tmp` has only 28 MiB free, so
  `/api/v1/record/start` correctly refused with `stop_reason=disk_full` even
  after temporarily lowering `record.maxMB`. Rows 8 and 9 remain unverified.
- Hardware verification found and fixed CV610 gate setup running before
  `cv610_output_start()` created the ring. Setup now follows successful output
  creation, and startup reports `frame gate on ... ring=8 slots` rather than
  declaring the gate inert.
- **Star6E preliminary:** the consumer-stall test paused and recovered without
  D-state, but `MI_VENC_StopRecvPic` returned `0xA0022012`. This decodes as
  `MI_ERR_VENC_BUSY`: the normal-path call was made before `ReleaseStream`.
  The local checkpoint moves it after release, but that fix is not yet tested
  on device.
- **Maruko:** code-level only. The same pre-release ordering existed and is
  moved after release locally; `.233` is now available but untested.
- Rebased on upstream 0.84.0; version bumped to 0.85.0. The
  `contract_version` remains at upstream's 0.31.0 because the frame gate adds
  three restart-required fields without changing an endpoint or payload.
- The local verification branch contains the rebased PR plus the CV610
  initialization-order fix, bench results, post-release ordering, and reopen
  retry hardening. The remote PR branch has not been rewritten. A future push
  needs explicit authorization and `--force-with-lease` because of the rebase.

## 9. When it passes

### Exact next-agent sequence

1. The checkpoint binaries are current: `make verify`, the full CV610 vendor
   link, strict lint for all three backends, and the host suite all passed.
   Re-run them after the next edit, not before deploying this checkpoint.
2. Deploy only to `.232`, `.233`, and `.181`; stop the vehicle-local
   `S97waybeam-hub` consumer during the stall interval. The x86 receiver is
   deliberately paused and must not be part of the evidence.
3. On each vehicle, run `tools/frame_shm_consumer_test` locally, SIGSTOP it long
   enough to close the gate, then SIGCONT it. Confirm no `BUSY`, D-state, fault,
   unexpected IDR/IRAP, bad Annex-B framing, or PTS regression. Measure
   drain-to-first-new-frame latency; it should be milliseconds, not 500 ms.
4. Repeat with the consumer left stalled for >500 ms to distinguish the safety
   pulse from normal drain-triggered reopening. Restore the vehicle-local hub
   after every trial.
5. Only after all three pass, make frame gating automatic for frame-shm and
   remove the public `video0.frameGate` switch. Keep the two tuning fields
   (`frameGateCloseSlots`, `frameGateMaxClosedMs`) unless evidence says they
   should also become constants. Removing the switch touches all config layers:
   `include/venc_config.h`, defaults/parser/printer/cJSON in `src/venc_config.c`,
   field/alias/UI in `src/venc_api.c`, all three default JSON files, backend
   setup calls, tests, README/HISTORY, and the HTTP contract.
6. Re-run `make test-ci`, `make verify`, the CV610 link, a config-layout grep,
   and the three vehicle trials with configs that do not contain `frameGate`.
7. Tick the PR runtime checks and report each result as **confirmed on device**
   or **code-level only**.

### Local code changes at this checkpoint

- All normal-path gate evaluations now occur after the acquired stream is
  released. Idle-path calls remain in place; they are the fast reopen path.
- `frame_gate_restore_closed()` rolls policy back after a refused OPEN so the
  actuator retries after 20 ms. Previously all three backends could believe
  they were open while hardware stayed stopped. A host test covers the retry.
- No protocol bytes changed. Do not add a reverse futex in this PR: frame-shm
  v2 only wakes consumer-from-producer; a drain wake would require coordinated
  changes to every consumer for little gain over the current 1–2 ms polling.
- Coordination `protocols/frame-shm.md` is stale versus upstream ring v2
  (`low_water_slots`/`other_drops`). That pre-existing drift is out of this PR's
  implementation scope, but should be corrected separately.

If the post-release call still returns BUSY on either SigmaStar board, capture
the exact code and logs and stop default-on work. Do not treat BUSY as success.

If R1 fails, close the PR with the finding rather than reworking it — and
keep the plan and this document, because the measurement is the valuable part.

---

## 10. SigmaStar bench results — 2026-09-12, local takeover

Run on `.232` (Star6E SSC338Q, 1280x720 @ 100 fps) and `.233` (Maruko
ssc378qe, 1080p @ 30 fps), both `resilience=racing` (GDR), frame-shm ring
of 8 slots, vehicle-local `S97waybeam-hub` stopped so
`tools/frame_shm_consumer_test` was the only consumer. Stall = SIGSTOP the
consumer; resume = SIGCONT.

### R1 is answered: PASS

`MI_VENC_StopRecvPic` does **not** hang and no longer returns BUSY.

- `StopRecvPic` returned 0 on every close. The `0xA0022012`
  (`MI_ERR_VENC_BUSY`) seen at the previous checkpoint is **fixed** by the
  post-`ReleaseStream` ordering in `e483e37` — confirmed on device.
- The daemon stayed `S`/`R` throughout; never `D`. No `MMU_Callback` storm,
  no kernel fault, no wedge.
- Gate closed at exactly `usedSlots == 3` and reopened on drain. `fps/live`
  returned to 100, ring drained to 0, **all drop counters stayed 0**.

Drain-to-reopen was prompt — recovery appeared in the consumer's very next
1 s print, not after the 500 ms escape.

### Row 3 FAILS on both SigmaStar boards — the feature's premise does not hold

Row 3 is the one the plan calls decisive: *"if a cycle keyframes, it has no
reason to exist."* Six identical stall cycles (2 s stalled / 3 s running)
inside one 40 s consumer window, gate on versus gate off, nothing else
changed:

| Board | 6 cycles, `frameGate=on` | 6 cycles, `frameGate=off` (control) |
|---|---|---|
| `.232` Star6E | **7 IDR** / 39.5 s | **1 IDR** / 39.9 s |
| `.233` Maruko | **22 IDR** / 39.2 s | **0 IDR** / 39.8 s |

On Star6E every reopen produced exactly one IDR; on Maruko every reopen
produced three. The controls show the natural rate in racing/GDR is ~1 IDR
per 40 s or less, so the excess is entirely attributable to the gate.

The reopen path calls **only** `MI_VENC_StartRecvPic()`
(`src/star6e_runtime.c:1625`) — no IDR request, no attribute write. The
keyframe is emitted by the SigmaStar SDK actuator itself, so no amount of
policy tuning avoids it.

**Consequence.** The gate was built because a bitrate write implicitly emits
an IDR, so pausing intake was supposed to shed load without one. On
SigmaStar, pausing intake emits an IDR too — while simply letting the ring
drop on full (the `frameGate=off` behaviour that already ships) sheds the
same load for **zero** IDRs, with `bad_meta=0`, `bad_startcode=0` and
`pts_regress=0` across the whole control run. On these two backends the gate
is therefore strictly worse than the status quo it was meant to improve.

This does not touch the CV610 result in §8, which measured zero IDR across a
gate interval — `ss_mpi_venc_start_chn()` evidently does not keyframe on
resume. The feature is SDK-dependent, not universally dead.

### Secondary findings

- **The 500 ms safety escape is defeated on Star6E.** `frame_gate_observe()`
  debounces only closed->open (`min_closed_us`, 20 ms); the open->closed
  direction has no dwell at all (`src/frame_gate.c:92`). The escape opens the
  gate, the idle path re-polls 1 ms later with occupancy still at the close
  threshold, and re-closes before the encoder can deliver a frame (10 ms at
  100 fps). Measured: `framesSent` advanced by exactly 300 per 5 s cycle —
  3 s of frames — so **zero** frames were admitted across each 2 s closed
  interval, and no `full_drops` were recorded either. A dead consumer
  therefore stops the stream *silently*, which is the outcome the escape's
  own comment says it exists to prevent. Maruko does pulse (~2-3 frames/s
  leaking as `transportDrops`), so this is a Star6E-specific race.
- **Maruko does not hold the ring at the close threshold.** Occupancy reached
  7-8 slots during every stall rather than stopping at 3, so the close lands
  late enough that the ring overflows anyway.
- **`/api/v1/idr/stats` cannot answer row 3.** It reports `channels: []` on a
  live stream because it counts only API-*requested* IDRs, not SDK-emitted
  ones. The check as written in row 3 would have returned a false PASS. The
  bitstream is the only truth; `tools/frame_shm_consumer_test` reads it.
- **`tools/frame_shm_consumer_test` reports `VERDICT: FAIL` on any healthy
  GDR stream.** Its `vcl_ok` requires `min_vcl == max_vcl`, but racing/GDR
  varies slice count (6 on GDR frames, 12 on an IDR). Integrity was clean in
  every run. Read the counters, not the verdict.

### What was NOT done

- Rows 4-6, 8-11, 13 were not reached: row 3 is the gate on the whole
  feature, and it failed on both SigmaStar backends.
- `.181` CV610 was not re-touched this session; its §8 results stand as the
  previous session recorded them.
- Both benches were restored: `.233` to its original 0.81.0 binary and
  config, `.232` left on the 0.85.0 checkpoint build with `frameGate=off`
  (its default). Hubs restarted, rings draining, `usedSlots` 0 on both.

---

## 11. The way around it on SigmaStar: `MI_VENC_SetRcParam`

Row 3 kills the *actuator*, not the *idea*. The ring-occupancy signal the gate
computes is fine; what costs an IDR is `MI_VENC_StartRecvPic`. So the question
became: is there another way to shed rate on SigmaStar that does not route
through `MI_VENC_SetChnAttr` (implicit IDR, §1) or through intake stop/resume
(implicit IDR, §10)?

### What the SDK offers

From the OpenIPC HAL bindings in `sdk/ssc338q/hal/star/` — `i6_venc.h`
(Star6E) and `i6c_venc.h` (Maruko), both reverse-engineered against the real
`libmi_venc.so`:

- **`MI_VENC_StartRecvPicEx(chn, int *count)`** — present on both. Meters
  intake by frame count, the same semantic as CV610's
  `ss_mpi_venc_start_chn(recv_pic_num)`. Still a resume, so it was not
  pursued ahead of the option below.
- **`MI_VENC_SetInputSourceConfig(chn, src_conf*)`** — `NORMAL`, `RING_ONE`,
  `RING_HALF` (Maruko adds `HW_SYNC`, `RING_DMA`). This selects the input ring
  coupling mode for latency, not an intake throttle. Dead end.
- **`MI_VENC_SetRcParam(chn, MI_VENC_RcParam_t*)`** — the one that matters.

### `SetRcParam` does not keyframe — and this repo already knew

`src/star6e_controls.c:442` states it outright: *"Unlike apply_bitrate() this
is a real reduction — MI_VENC_SetRcParam does NOT implicitly keyframe."*
Measured 2026-08-23, ten spaced `video0.qpDelta` writes went from eleven IRAP
access units to one.

Re-confirmed on the 0.85.0 checkpoint build, `.232`, 2026-09-12: **eleven
distinct `SetRcParam` writes across five runs produced zero IDR frames**,
and `fps/live` never left 100-101 — the stream is never interrupted at all.

### It is a real lever, and it has a proportional band

`.232`, 720p100, H.265 CBR, racing/GDR, static bench scene, 8 s samples,
`video0.minQp` swept through `MI_VENC_SetRcParam`:

| `minQp` | Mbit/s | vs unclamped | IDR |
|---|---|---|---|
| 0 (driver default) | 5.74 | 1.00x | 0 |
| 20 | 2.67 | 0.47x | 0 |
| 23 | 0.84 | 0.15x | 0 |
| 26 | 0.35 | 0.06x | 0 |
| 29 | 0.17 | 0.03x | 0 |
| 30 | 0.17 | 0.03x | 0 |
| 34-45 | 0.12-0.13 | 0.02x | 0 |

Monotonic 16:1 through the 20-29 band, then saturated. The plan's §2 called
`min_qp` "a cliff, usable only as a binary panic switch" — that is half right:
it *is* steep (~6 dB per 3 QP steps, which is simply what QP does) and it
saturates above 30, but 20-29 is a usable continuous band, not a cliff.

### Why this beats the gate on SigmaStar

| | frame gate | `SetRcParam` clamp |
|---|---|---|
| IDR per actuation | 1 (Star6E) / 3 (Maruko) | **0** |
| fps while shedding | 0 (intake stopped) | **unchanged, ~100** |
| Control shape | binary open/closed | proportional |
| Failure modes | BUSY, D-state risk, six reopen paths, 500 ms escape | one call, no state |
| GDR refresh wave | broken by the resume keyframe | continuous |

Frames keep flowing, so the decoder never starves and the refresh wave is
never interrupted — the gate's whole hazard surface (§7) disappears.

**Caveat.** The absolute QP values are scene-dependent; this was a static
bench scene and a moving scene will move the band
(`feedback_moving_scene_required_for_venc_rate_tests`). That argues for a
closed loop driven by ring occupancy rather than a fixed QP table.

### Recommended shape

1. **Keep the gate for CV610.** `ss_mpi_venc_start_chn()` measured zero IDR on
   resume (§8), so the feature works as designed there. Scope `video0.frameGate`
   to the CV610 backend and report it inert elsewhere.
2. **On Star6E and Maruko, keep `frame_gate_observe()`'s occupancy signal but
   change the actuator** — drive an RC clamp through `MI_VENC_SetRcParam`
   instead of `Stop/StartRecvPic`. The policy, its thresholds and its 68 host
   assertions survive; only the two backend `service_frame_gate()` bodies
   change, and the six reopen paths and the safety escape all become
   unnecessary.
3. **Prefer a byte budget to a QP floor.** `MI_VENC_RcParam_t` carries
   `u32MaxPSize` / `u32MaxISize` on both SigmaStar backends
   (`include/star6e.h:395-440`), and `rc_commit_intent()` already does a
   read-modify-write so they survive every other RC write. A per-frame byte cap
   is scene-independent and is exactly the quantity a ring-occupancy controller
   wants, where a QP floor has to be re-found per scene. These were exposed as
   `video0.maxIBytes` / `video0.maxPBytes` in an earlier build and measured as
   a hard proportional lever (4096 B cap = 4.9 Mbps, uncapped = 13.0 Mbps,
   same scene, `.232` 2026-08-03); they are not in the 0.85.0 config surface.
   Re-exposing them is the natural next step and should be measured for IDR
   behaviour the same way.

---

## 12. The real vendor SDK — three purpose-built mechanisms

`/home/snokvist/dev/SDK_IPCAM` holds the actual SigmaStar releases, and they
answer this far better than the reverse-engineered HAL headers in §11:

- Star6E: `star6e_SDK/project-Pudding-ILC02V009/project/release/include/` —
  `mi_venc.h`, `mi_venc_datatype.h`, `mi_venc_user_rc.h`
- Maruko: `Maruko/SourceCode/project/project/release/include/` — same set
- Official docs: `SGS_IPU_SDK/.../API 说明文档/API/MI_VENC_API.pdf`
  (MI VENC API v2.12); extract with `pdftotext -layout`

The in-repo OpenIPC HAL binds ~20 VENC calls. **The real SDK exposes 80**, and
the ones that matter here were simply never bound. All layouts below are
**identical on Star6E and Maruko**, and all six symbols are **exported by the
shipped `/usr/lib/libmi_venc.so` on both `.232` and `.233`** (verified
2026-09-12), so none of this needs a firmware change.

### 12.1 `MI_VENC_SetFrameLostStrategy` — the purpose-built primitive

```c
typedef struct {
    MI_BOOL                 bFrmLostOpen;      /* switch */
    MI_U32                  u32FrmLostBpsThr;  /* threshold, bit/s */
    MI_VENC_FrameLostMode_e eFrmLostMode;      /* NORMAL | PSKIP */
    MI_U32                  u32EncFrmGaps;     /* spread evenly, not in bursts */
} MI_VENC_ParamFrameLost_t;
```

This *is* the frame gate, built into the SDK: "配置瞬时码率超出阈值时丢帧策略"
— shed load when the instantaneous bitrate exceeds a threshold. The doc says a
call during encoding "takes effect at the next frame", and `u32EncFrmGaps`
spreads the shedding evenly instead of dropping a burst.

`E_MI_VENC_FRMLOST_PSKIP` is the important mode: instead of dropping the frame,
the encoder emits a **P-skip frame** — a few bytes saying "same as reference".
So the bitstream has **no gap**, PTS stays continuous, the decoder never
starves, the GDR wave keeps advancing, and it is a P frame, so **no IDR**. That
is every property the gate wanted, natively, with a threshold in bit/s that a
ring-occupancy controller can drive directly.

### 12.2 `MI_VENC_EnableIdr` — suppress the resume keyframe outright

```c
MI_S32 MI_VENC_EnableIdr(MI_VENC_CHN VeChn, MI_BOOL bEnableIdr);
```

Doc, §1.3.19: *"若不使能 IDR 帧，则在下一帧之后都编不出 IDR 帧或 I 帧，直到再次使能为止"*
— with IDR disabled, no IDR or I frame is encoded from the next frame onward
until it is re-enabled. H.264/H.265 only.

That is a direct answer to §10: bracket the gate's reopen with
`EnableIdr(FALSE)` / `StartRecvPic` / `EnableIdr(TRUE)` and the resume keyframe
should never be emitted. It keeps the existing gate design intact. Two things
to prove on device: that it actually suppresses *this* IDR (the SDK may treat a
receive-restart keyframe as mandatory), and where to re-enable — too early and
the IDR returns, too late and a genuine recovery request is swallowed.

### 12.3 `MI_VENC_SetSuperFrameCfg` — RULED OUT (latency)

```c
typedef struct {
    MI_VENC_SuperFrmMode_e eSuperFrmMode;        /* NONE | DISCARD | REENCODE */
    MI_U32 u32SuperIFrmBitsThr;
    MI_U32 u32SuperPFrmBitsThr;
    MI_U32 u32SuperBFrmBitsThr;
} MI_VENC_SuperFrameCfg_t;
```

§1's complaint is that the response to congestion sends *the largest frame in
the stream* into an already-overflowing link. This caps that case at its
source: any frame over the per-type bit threshold is discarded or re-encoded.

### 12.4 Also unbound and worth knowing

`MI_VENC_SetRcPriority` (`BITRATE_FIRST` vs `FRAMEBITS_FIRST`),
`MI_VENC_SetRoiBgFrameRate` (background frame rate outside ROI),
`MI_VENC_AllocCustomMap`/`ApplyCustomMap` (per-CTU QP map),
`MI_VENC_SetAdvCustRcAttr` + `mi_venc_user_rc.h` (user-defined RC),
`MI_VENC_StartRecvPicEx(chn, {s32RecvPicNum})` (meter intake by frame count —
the direct analogue of CV610's `recv_pic_num`).

### 12.5 Suggested order of attack

1. **`FRMLOST_PSKIP`** — the native mechanism, no gap, no IDR, threshold in
   bit/s. Bind it, drive `u32FrmLostBpsThr` from ring occupancy, and the frame
   gate's whole state machine becomes unnecessary on SigmaStar.
2. **`EnableIdr(FALSE)` around the reopen** — smallest change if the gate as
   built is worth keeping; needs the two device proofs in §12.2.
3. **`SetRcParam`** (§11) — already measured IDR-free, already wired, good
   fallback and useful as the slow outer loop.
4. ~~`SuperFrameCfg`~~ — ruled out on latency grounds, see §12.3.

Measure each the same way §10 did: gate the bitstream through
`tools/frame_shm_consumer_test`, count IDRs against a no-change control, and
never trust `/api/v1/idr/stats` for this question.

---

## 13. All four SigmaStar mechanisms, measured — pros and cons

Everything in §12 was tried on hardware. Method throughout: gate the **wire**,
not the metadata — `tools/frame_shm_consumer_test` with a raw Annex-B dump, NAL
histogram via `scripts/hevc_analyze.py walk`, always against a no-change
control. `SuperFrameCfg` was dropped on the operator's call: `REENCODE` adds a
re-encode pass and `DISCARD` drops a whole frame, and neither belongs in a
latency-critical FPV path.

### Scoreboard

| Mechanism | Star6E `.232` | Maruko `.233` | Verdict |
|---|---|---|---|
| Frame gate (`Stop`/`StartRecvPic`) | works, **1 IDR per reopen** | works, **3 IDR per reopen** | rejected — §10 |
| `EnableIdr(FALSE)` | resume IDR **unchanged** | not retested | does not rescue the gate |
| `FrameLostStrategy` PSKIP | **`E_MI_ERR_NOT_SUPPORT`** | accepted, **no-op** | unusable |
| `FrameLostStrategy` NORMAL | accepted, **no-op** | accepted, **no-op** | unusable |
| `SetRcParam` (QP) | **0 IDR, fps unbroken, 16:1** | not retested | **the one that works** |

### 13.1 `MI_VENC_EnableIdr` — reduces parameter sets, does not stop keyframes

The headline question was whether it also suppresses the IDR a bitrate write
emits. Four spaced `video0.bitrate` writes per arm, 18 s captures, NAL
histogram:

| | `EnableIdr=TRUE` | `EnableIdr=FALSE` |
|---|---|---|
| IDR_W_RADL | 42 | 24 |
| VPS / SPS / PPS | 12 each | 4 each |

And on the path that actually matters, four gate cycles per arm:

| | `EnableIdr=TRUE` | `EnableIdr=FALSE` |
|---|---|---|
| IDR_W_RADL | 12 | **12** |
| VPS / SPS / PPS | 8 each | **2** each |

**Pros:** cuts VPS/SPS/PPS re-emission consistently (12→4, 8→2), which is real
if small bandwidth. Cheap, one call, no state.
**Cons:** does **not** suppress the `StartRecvPic` resume keyframe at all —
12 versus 12. On bitrate writes it roughly halves IRAPs but does not eliminate
them, consistent with a CBR reconfiguration forcing a mandatory resync point
the flag cannot override. And while disabled it would also swallow a genuine
recovery IDR.
**Verdict:** not the rescue. Worth keeping in mind only as a parameter-set
trimmer.

*Trap this exposed:* the consumer's `IDR frames` counter reads the producer's
`VENC_FRAME_FLAG_IDR` metadata bit, not the bitstream. In the disabled arm the
metadata still said "6 IDRs" while the parsed slice count fell from 12 to 6 —
two observation channels disagreeing. Only the NAL histogram settles it.

### 13.2 `MI_VENC_SetFrameLostStrategy` — exported, accepted, inert

The most promising candidate on paper, and the biggest disappointment.

- **Star6E:** `E_MI_VENC_FRMLOST_PSKIP` is refused outright with
  `0xA0022008` = `E_MI_ERR_NOT_SUPPORT`. `NORMAL` is accepted and does nothing.
- **Maruko:** both modes accepted, both do nothing.

Pushed hard on both boards — `u32FrmLostBpsThr = 1000` (1 kbit/s) against a
live 7 Mbit/s stream on Star6E and 19.6 Mbit/s on Maruko, with
`u32EncFrmGaps` 0, 1 and 3:

| Board | threshold | measured | fps |
|---|---|---|---|
| Star6E | 1 kbit/s | 7.03 Mbit/s | 101 |
| Maruko | 1 kbit/s | 19.58 Mbit/s | 31 |

Not a partial effect — no effect. The struct layout is right (the 16-byte
`_Static_assert` passes and the SDK validates the enum, since it rejects PSKIP
by *name* on Star6E). No demo in either vendor SDK calls this function; only
the RTOS linker maps mention it.

**Pros:** would have been ideal — threshold in bit/s, no gap in PSKIP mode, no
IDR, effective at the next frame.
**Cons:** it does not work on either board. Maruko is the worse failure of the
two: it returns **success** for both modes and silently does nothing, so a
return-code check alone would have shipped a dead actuator.
**Verdict:** unusable. Do not build on it without re-measuring on the exact
silicon, from the wire.

### 13.3 `MI_VENC_SetRcParam` — still the only thing that works

Unchanged from §11: eleven writes, zero IDRs, fps never left 100-101, and a
monotonic 16:1 range through `minQp` 20-29.

**Pros:** IDR-free; never interrupts the stream, so the decoder never starves
and the GDR wave keeps advancing; proportional; one call; already wired and
already unit-covered; no reopen paths, no escape, no BUSY, no D-state risk.
**Cons:** QP is scene-dependent, so the usable band moves with content — the
numbers above are from a static bench scene. It is steep (~6 dB per 3 QP) and
saturates above QP 30. It trades quality for rate, where the gate traded
frames for rate.
**Verdict:** the recommendation stands, and it is now the *only* surviving
candidate on SigmaStar.

### 13.4 Where this leaves the PR

- **CV610 keeps the frame gate.** `ss_mpi_venc_start_chn()` does not keyframe
  on resume (§8), so the feature works there as designed.
- **Star6E and Maruko get an RcParam clamp** driven by the same
  `frame_gate_observe()` occupancy signal. The policy and its 68 host
  assertions survive; the six reopen paths and the 500 ms escape do not.
- **Next measurement, not yet done:** re-expose `u32MaxPSize` /
  `u32MaxISize` (§11.3) and check from the wire whether a per-frame byte
  budget is also IDR-free. It is the scene-independent quantity a
  ring-occupancy controller actually wants, and unlike the two dead ends above
  it is known to bind hard.

### 13.5 Test hooks in this tree

`GET /api/v1/idr/enable?on=0|1` and
`GET /api/v1/frmlost?on=&bps=&mode=pskip|normal&gaps=` exist on both SigmaStar
backends, plus `star6e_controls_enable_idr()`,
`star6e_controls_frame_lost()` and their Maruko twins, and the
`MI_VENC_EnableIdr` / `MI_VENC_SetFrameLostStrategy` bindings.

**These are investigation scaffolding, not merge candidates.** They are kept
because they are what any re-measurement needs. Strip them, or gate them behind
a debug build, before this branch goes near main.

---

## 14. Drain-stall: the unix-socket hazard used deliberately

### The prior art

`outgoing.allowUnixEncoderStall` (`include/venc_config.h:256`, default false)
and the comment at `src/output_socket.c:49`: AF_UNIX SOCK_DGRAM has no
fire-and-forget — when the peer's receive queue fills, **the sender sleeps**.
Those sends are issued between `MI_VENC_GetStream` and `MI_VENC_ReleaseStream`,
so the sleep holds a VENC output slot and stalls capture. Measured at up to
**74 ms** against a wedged consumer. The repo treats this as a hazard and caps
it with a 2 ms `SO_SNDTIMEO`.

The idea worth testing: that stall pauses the encoder **without any SDK call**,
so there is no `StartRecvPic` and therefore no resume keyframe. Generalised
away from sockets, the actuator is simply *stop calling `MI_VENC_GetStream`
while the gate is closed* and let VENC's output FIFO fill.

Implemented as a second actuator behind `GET /api/v1/gatemode?mode=recvstop|drainstall`
(`src/star6e_runtime.c`), reusing `frame_gate_observe()` unchanged.

### It does reduce keyframes — about 4x

Five **proven** cycles per arm (2 s stalled / 3 s running), proof being
`usedSlots` at the close threshold and a `framesSent` delta of one cycle minus
two seconds. Both channels read from the same run; IRAP counted as access
units, not NALs:

| Arm | pictures | metadata IDR | **IRAP pictures** | per cycle |
|---|---|---|---|---|
| recv-stop (`Stop`/`StartRecvPic`) | 3103 | 5 | **5** | 1.0 |
| drain-stall (skip `GetStream`) | 5002 | 2 | **2** | 0.4 |

Natural rate is roughly 1 IRAP per 40 s (§10 control), so of drain-stall's two,
one or more is simply the baseline. Attributable reopen keyframes fall from
about one per cycle to roughly one per five to ten cycles.

Shedding is comparable: both suppress about 2 s of production per cycle.
Drain-stall's ring overshoots to 6 slots rather than holding at 3, because the
close is only observed on the following pass.

### And it can permanently wedge the encoder

Stall the drain **before flow is established** — gate on, no consumer ever
attached, so the ring reaches the close threshold within the first three
frames — and the channel never comes back:

```
framesSent: 3   usedSlots: 0   fps/live: 100
/tmp/waybeam.log: "waiting for encoder data..." (repeating)
```

The gate reopens correctly, the drain resumes, `MI_VENC_Query` returns
`curPacks == 0` forever, and the process stays `S` — no D-state, no crash, no
log of a fault. Still dead 25 s later. Only a daemon restart recovers it.

Mid-stream stalls recovered cleanly every time; it is the cold start that
kills it. That asymmetry matters because a craft boots with no ground
consumer attached, which is exactly the fatal case.

### Pros and cons

**Pros:** no SDK state change, so ~4x fewer reopen keyframes than recv-stop;
no `StopRecvPic`, so no BUSY and no D-state; reuses the existing policy and its
68 assertions; the actuator is a `return` rather than a call.

**Cons:** does not reach zero keyframes — it reduces, and the residue is not
yet explained. It can **permanently wedge the encoder** with no fault
signature, and the trigger (stall before flow is established) is the normal
boot condition for a vehicle. Recovery needs a process restart. It also
inherits the hazard `output_socket.c` was written to bound, which is a strong
prior that the SDK does not expect this.

**Verdict:** the mechanism is real and the reduction is large, but a silent
unrecoverable encoder wedge on cold boot is a worse failure than the IDR it
avoids. Not shippable as-is. It would need a watchdog — no frames for N ms
while the gate is open means kick the channel — and at that point the kick is
a `StartRecvPic`, which reintroduces the keyframe it was avoiding.

`SetRcParam` (§11) remains the recommendation: it sheds rate with **zero**
keyframes, never stops the stream, and has no wedge state at all.

### Method note

Three drain-stall runs were discarded before this one, each because the
stimulus never reached the device:

1. `ps w | grep frame_shm_consumer | head -1` matched the **shell running the
   loop**, so the script SIGSTOPped itself.
2. A leftover probe consumer from an earlier command was matched instead of
   the measurement consumer, which therefore ran unstalled at a clean 100 fps.
3. A dump was pulled while the consumer was still writing it, so the NAL
   histogram ran on a truncated file.

Each produced a plausible-looking result — "1 IRAP for 6 cycles" — that was an
artifact. `ps w | awk '$4=="/tmp/frame_shm_consumer"'` resolves the pid without
matching the shell, and **every** cycle must publish its own proof
(`usedSlots` at threshold, `framesSent` delta) or the run is void.
This is `feedback_drive_device_verification_manually` and
`feedback_assert_the_effect_not_the_stimulus` biting three times in one
session.

---

## 15. Correction: there is no cold-start wedge — drain-stall works from boot

### §14's blocker was my own test hook

The wedge in §14 was **not** a property of drain-stall. The sequence that
produced it was:

1. daemon restarts in `recvstop` (the default), the gate closes at three
   frames and **`MI_VENC_StopRecvPic` is issued** — hardware now stopped;
2. the actuator is switched to `drainstall` over HTTP;
3. a consumer attaches, drains the ring, the gate reopens — but the
   drain-stall guard returns *before* the actuator, so the matching
   `StartRecvPic` is never issued and the hardware stays stopped for good.

That is a hook artifact, and `star6e_runtime_set_gate_drain_stall()` now
re-arms the hardware and forces the policy open on every switch.

Two other hypotheses were tested and **falsified** on the way:

- *Stall duration.* A **12 s** mid-stream stall recovered cleanly — ring full
  at 8/8, then drained to 0 and production resumed. Duration is not the
  variable.
- *Absence of a consumer.* A warm channel with the consumer **killed outright**
  for 20 s, ring full, recovered completely on reattach (6104 -> 6906 frames).
  A missing consumer is not the variable either.

### Armed from boot, drain-stall is keyframe-free

The actuator is now selectable at boot (`WB_GATE_DRAIN_STALL=1`), because with
any small warmup the gate can arm inside 100 ms — far sooner than a request
could arrive. Cold boot, gate on, **no consumer at all** for 18 s, then a
consumer attaches and twelve stall cycles run:

| | recv-stop | drain-stall from boot |
|---|---|---|
| Cold start, no consumer | recovers | **recovers** |
| Proven cycles | 5 | **12** |
| Soak | — | **99.5 s** |
| Frames | 3103 | 7647 |
| **IRAP pictures** | **5** (1.0/cycle) | **0** |
| `bad_meta` / `bad_startcode` / `pts_regress` | 0 | **0** |
| Encoder state after | `S` | `S`, 100 fps, **zero errors logged** |

Every cycle published its own proof (`usedSlots` at 6, `framesSent` short by
two seconds). The raw dump is **0 bytes**, which is independent corroboration
rather than a failure: `tools/frame_shm_consumer_test` only begins dumping on
the first IDR (`tools/frame_shm_consumer_test.c:159`), so a zero-length file
means no IRAP ever arrived — a second code path agreeing with the counter.

### What this changes

Drain-stall is now the **best** actuator measured on SigmaStar: it sheds the
same load as recv-stop, costs **zero** keyframes rather than one per cycle,
makes no SDK call at all, and survives a cold boot with no consumer. It beats
the `SetRcParam` clamp (§11) too, because it drops frames rather than degrading
every frame's quality.

Remaining caveats, all still open:

- The warmup floor (`WB_GATE_WARMUP_FRAMES`) was built for a bisection that
  turned out to be unnecessary. It is inert at 0 and was **not** needed in any
  passing run; keep it only if a future board shows an early-stall sensitivity.
- The ring overshoots to 6 slots rather than holding at 3, because the close is
  only observed on the following pass. Harmless here, but it means the
  effective threshold is not the configured one.
- Only Star6E. Maruko has the same `frame_gate_observe()` policy but its drain
  loop is `maruko_pipeline_process_stream`, and the actuator swap is unported
  and unmeasured there.
- The §10 secondary finding still stands: with recv-stop, a dead consumer stops
  the stream silently. Drain-stall does not fix that — it changes which
  keyframes are emitted, not the escape's defeat.

---

## 16. Partial congestion, and the open points closed

### 16.1 It is a smooth partial stall, not 0/100 judder

The SIGSTOP consumer used throughout §10-§15 is a **binary** load — the link is
either at full capacity or dead. The FPV case is neither. `tools/frame_shm_rate_consumer.c`
drains at most N frames per second, emulating a link that has degraded to a
fraction of capacity and stayed there, and reports the per-second series rather
than a mean, because a producer alternating full-rate and zero gives the same
mean as one that settles.

Star6E `.232`, 100 fps production, drain-stall armed from boot:

| Link capacity | Delivered | Spread | Mean ring | IDR |
|---|---|---|---|---|
| 20 fps | 20.5 | 1 | 1.57 / 8 | 0 |
| 40 fps | 40.6 | 1 | 1.52 / 8 | 0 |
| 60 fps | 61.0 | **0** | 1.50 / 8 | 0 |
| 80 fps | 80.4 | 1 | 1.50 / 8 | 0 |

A representative 40 fps series: `41 40 41 41 40 41 40 41 40 40 41 41 40 41 41 41 41`.

**The producer settles at the link rate.** Spread is at most one frame per
second across every capacity tested, so there is no judder — and the ring sits
at ~1.5 of 8 slots throughout, so the shedding costs no queueing latency. That
is the behaviour an FPV link wants: degrade the frame rate smoothly and keep
the buffer shallow.

Maruko `.233`, 30 fps production, same actuator: 10 fps -> 10.7, 15 -> 15.8,
25 -> 25.5, spread 1 at every point, mean ring 1.50-1.74.

The mechanism is self-pacing and needs no controller: the gate closes the
instant occupancy reaches the threshold and reopens as soon as the consumer
takes a slot, so the duty cycle lands wherever the capacity ratio puts it.

### 16.2 Open point: the escape defeat — FIXED

§10 found the 500 ms safety escape defeated on Star6E, and §15 confirmed
drain-stall inherits it: with no consumer the stream stopped with **every drop
counter at zero**. Silent, which is precisely what the escape's own comment
says it exists to prevent.

Root cause: `frame_gate_observe()` debounced only closed->open. The escape
reopened while occupancy was still above `close_slots`, so the very next
observation closed again — 1 ms later on Star6E's idle path, against a 10 ms
frame period at 100 fps. The admitted frame never existed.

Fix: `FRAME_GATE_MIN_OPEN_US` (20 ms, two frame periods at 100 fps), applied
**only** to the pulse that follows an escape, so normal closes keep their
burst response. Nine new host assertions cover the dwell, the drain that
clears it, and the unchanged normal close.

Device proof with a control, no consumer attached:

| Escape setting | `transportDrops` | `framesSent` | ring |
|---|---|---|---|
| 60 s (effectively disabled) | **0** | 3, frozen | 3 |
| 500 ms + dwell | **climbing ~9/s** | 8 | 8 |

A dead consumer now overflows the ring and reports it, instead of stopping the
stream in silence. Re-running §16.1 with the dwell in place changed nothing
(40.6 mean, spread 1, ring 1.52): escapes never fire while a consumer drains.

One existing test, `escape_recloses`, asserted a re-close 1 ms after the
escape — it had encoded the defect as the contract. It is migrated, not
deleted, with the reason recorded in place.

### 16.3 Open point: Maruko — PORTED and measured

`maruko_pipeline_process_stream` now carries the same actuator, armed by the
same `WB_GATE_DRAIN_STALL` env. Five proven cycles on `.233`: **0 IDR frames**
in 1097 frames, `bad_meta`/`bad_startcode`/`pts_regress` all zero — against
**22 IDR for six cycles** (three per reopen) with recv-stop in §10.

### 16.4 Open point: ring overshoot — explained, not a defect

Under full stop the ring overshoots the close threshold (3) to 6-8 slots,
because the close is only observed on the following pass. Under **partial**
congestion — the regime that matters — mean occupancy is 1.50-1.57 slots and
never approaches the threshold, so the overshoot is an artifact of the binary
test load. With the escape dwell in place a full stop is now *supposed* to
reach 8: that is the overflow-and-report behaviour §16.2 restored.

### 16.5 Still open

- **The warmup floor (`WB_GATE_WARMUP_FRAMES`) is dead code.** It was built to
  bisect a boundary that turned out not to exist once the §15 artifact was
  removed. It is inert at 0 and every passing run had it at 0. Delete it.
- **The test hooks are not merge candidates** (§13.5): `/api/v1/idr/enable`,
  `/api/v1/frmlost`, `/api/v1/gatemode`, the env vars, and the
  `EnableIdr`/`FrameLostStrategy` bindings. The actuator choice should be a
  config field or a build-time decision, not a live HTTP toggle — a live
  switch has to re-arm hardware or it strands the channel, which is the trap
  §15 documents.
- **CV610 is untouched by any of this.** It keeps the recv-stop gate, which
  measured keyframe-free there (§8).
- **Not yet measured:** behaviour against the real waybeam-link consumer rather
  than an emulated rate limiter, and whether the ~1.5-slot steady-state ring
  occupancy holds when the consumer's drain is bursty rather than evenly paced.

---

## 17. CV610: one mechanism for all three backends

### §8's "CV610 is keyframe-free" was measured on one interval, not on cycles

§8 recorded 542 frames across a **single** three-second gate interval with zero
IDR, and that result stands. It does not generalise to repeated cycling, which
is what a congested link actually produces.

`.181`, 1280x720 @100 fps, `resilience=racing`, 8-slot ring, five **proven**
cycles per arm (2 s stalled / 3 s running, `usedSlots` at 8 each time), both
actuators on the same binary:

| Actuator | Frames | **IDR** | Integrity |
|---|---|---|---|
| recv-stop (`ss_mpi_venc_stop_chn`/`start_chn`) | 3443 | **3** | clean |
| drain-stall (skip `ss_mpi_venc_get_stream`) | 3768 | **0** | clean |

So CV610's shipped actuator is *better* than SigmaStar's (3 per five cycles
rather than one per cycle) but it is **not** keyframe-free, and drain-stall
removes the remainder. It also delivered ~9 % more frames over the same window.

Under partial congestion the two are indistinguishable, which is the expected
result — the escape never fires while a consumer drains, so the actuator is
never exercised:

| Link | recv-stop | drain-stall |
|---|---|---|
| 30 fps | 30.7, spread 1, ring 1.49 | 30.5, spread 1, ring 1.55 |
| 60 fps | 60.6, spread 1, ring 1.14 | 60.6, spread 1, ring 1.50 |

### Verdict: yes, share one mechanism

Drain-stall is now measured as the best actuator on **all three** backends:

| Backend | recv-stop IDR/cycle | drain-stall IDR |
|---|---|---|
| Star6E `.232` | 1.0 | 0 |
| Maruko `.233` | 3.0 | 0 |
| CV610 `.181` | 0.6 | 0 |

It also makes no SDK call, which is why it is portable: there is no
`Stop`/`StartRecvPic` versus `stop_chn`/`start_chn` divergence to maintain, no
BUSY path, and the per-backend reopen paths collapse into one early return.
`frame_gate_observe()` and its 77 host assertions are already shared; after the
swap the only per-backend code is *where* the drain loop returns early.

Build note: CV610 headers live at `$HOME/dev/hisilicon/oh` (not
`$HOME/dev/hisilicon`), and the vendor `.so` set is stashed at
`out/cv610/lib` — 53 libraries, enough to link. Both paths must be passed to
`make lint` as well as `make build`, or the SDK gate fails before the compiler
runs.

## 18. The RF question is blocked on the craft, not on the gate

The proposed end-to-end test — pin `min`/`max` MCS to one value, disable
adaptive bitrate, overdrive `video0.bitrate` so the radio genuinely cannot
carry it, and watch the gate against the real waybeam-link consumer — is the
right test, and it is **not runnable as the bench stands**:

- `/usr/bin/waybeam-link` **is not installed** on `.232`.
- There is no boot symlink (`S9xwaybeam-link`), and the daemon is not running.
- `/sys/class/net` has only `eth0` and `lo`: no wireless interface exists. The
  8812EU is present on USB (`0bda:a81a`) and the `8812eu` module is loaded
  against `cfg80211`, but nothing has put it into monitor mode.
- `/etc/waybeam-link/` still holds craft configs, so the craft *was* a link TX
  at some point — consistent with the integrated-hub migration.

What exists to make it runnable: a prebuilt vehicle binary at
`waybeam-link/build/ssc338q-au/waybeam-link`, and three radios on the dev host
(`0bda:c812` 8812AU, `0e8d:0616` MT7612U, `0bda:f72b` 8733BU) for the ground
end.

So the remaining work is a link bring-up — deploy the craft binary, start it in
monitor mode, stand up a ground node — and only then the gate measurement.
That is an RF arm on a shared bench and needs its own go-ahead; it is not a
continuation of the venc work.

**What the emulator already covers, and what it cannot.** §16.1's rate-limited
consumer reproduces the *shape* of a capacity-limited link exactly, and the
producer settled at the link rate at every capacity tested. What it cannot
reproduce is a consumer whose drain is **bursty** rather than evenly paced —
waybeam-link drains in FEC-block groups, not one frame per fixed interval — so
the ~1.5-slot steady-state occupancy is the number most likely to move under
the real consumer. That is the open risk the RF test would retire.

---

## 19. Correction to §18, and the RF result

### §18 was wrong: waybeam-link is already running on `.232`

`/usr/bin/waybeam-link` does not exist on the craft and no wireless interface
appears in `/sys/class/net`, which is what §18 concluded from. Both are true and
both are misleading: **`.232` runs the link as an in-process node inside
`waybeam_hub`** (`mod_wblink`), and the hub holds the 8812EU directly through
devourer — `/proc/<hub pid>/fd` points at `/dev/bus/usb/001/002` and
`/tmp/devourer-usb-1-1.lock`. That is why a second `waybeam-link` process is
refused with "USB adapter in use", and why there is no `wlanX`: devourer claims
the device from userspace, so the kernel never creates an interface.

The tell that would have short-circuited the whole detour: `:8091` answers
`/api/v1/stats` with `"tx":true` and a live `auto0-8812eu` adapter. **Check the
control plane before concluding a daemon is absent** — a missing binary on
`$PATH` says nothing when the feature has been folded into another process.
`waybeam_hub.conf` names it outright: `wblink.config_path` points at
`/etc/waybeam-link/craft.json`, the same file the craft is actually running.

### Setup

- `policy.select.min_profile = max_profile = 1` — MCS pinned, no adaptation.
- `venc.enabled = false` — waybeam-link's bitrate controller disabled, so
  nothing walks the rate back down.
- `video0.bitrate = 20000` — offered rate far above what MCS 1 can carry.
- Consumer is the **real** in-process wblink node reading `venc_frame`, not an
  emulator. Keyframes counted by the link's own `idr_frames`.

### Result: recv-stop is catastrophic here, drain-stall is clean

Eight 5-second samples per arm:

| | drain-stall | recv-stop |
|---|---|---|
| `idr_frames` across 40 s | **1 -> 1** (zero induced) | **61 -> 215** |
| Implied keyframe rate | **0 /s** | **~4.4 /s** |
| Delivered | ~47 fps | ~83 fps |
| Ring occupancy | 2-3 / 8 | 0-2 / 8 |
| `transportDrops` | frozen at 153 | 0 |
| Air throughput | ~9.2 Mbit/s | ~8.8 Mbit/s |

The SIGSTOP tests understated this badly. They cycled the gate once every five
seconds; a genuinely capacity-limited link cycles it **many times per second**,
and recv-stop pays one keyframe per cycle. At 4.4 IDR/s the craft is firing the
largest frame in the stream into an already-saturated link several times a
second — the exact pathology the feature was written to avoid, delivered by the
feature itself.

Drain-stall induced **zero** keyframes over the same window while holding the
ring at 2-3 slots and taking no ring overflow at all.

The fps difference is not a defect on either side: recv-stop delivers more
frames but spends the air budget on keyframes, and both arms moved almost
identical bytes. Which is preferable is a picture-quality question for the
operator, not a correctness one — but 4.4 IDR/s on a congested link is not a
defensible operating point.

### What this retires and what it does not

Retired: the open risk in §16 that a **bursty** consumer would not hold the
~1.5-slot steady state. The real node drains in FEC-block groups and the ring
still sat at 2-3 of 8.

Not retired: this was a **TX-side** measurement with no ground station
receiving, so nothing here exercises ARQ, retransmission, or a receiver's
recovery-IDR requests. A ground in the loop could ask for keyframes for its own
reasons, which is a separate source from the actuator and was not measured.

---

## 20. Reproduced with a ground receiving and claiming the craft

### Bring-up

The ground is `sudo systemctl start waybeam-hub` on the x86 dev host
(`/etc/waybeam_hub/waybeam_ground.conf`, three auto adapters, control on
`:8092`). Claiming a craft is not a dedicated endpoint — the hub WebUI's
"claim craft" button is literally:

```sh
c=$(curl -sS http://127.0.0.1:8092/api/v1/link/selection | sed -n 's/.*"channel":\([0-9]*\).*/\1/p')
curl -sS -X POST -d "{\"mhz\":$c}" http://127.0.0.1:8092/api/v1/csa
```

The ground CSAs *itself* onto the selected craft's channel, and the claim
falls out of the latch. Two gotchas cost time:

- `/api/v1/channel` answers "endpoint not available in this mode"; `/api/v1/csa`
  is the retune path, and it refuses with *"no craft selected (nothing latched,
  no claim)"* until something is latched. Chicken-and-egg: the way in is
  `POST /api/v1/scout/start`, then `/api/v1/scout/results` for candidates, then
  `POST /api/v1/scout/quickconnect {"originator":N}`.
- A ground restart reverts to its configured channel, so the CSA must be
  re-issued afterwards, and quickconnect refuses a candidate from before a
  craft reboot ("stale candidate — re-scout").

Scout also made the bench legible: originator 17 = `.232` on 5540,
18 = `.233` on 5825, 19 = `.181` on 5700. `.181` was claimed by this ground and
had **followed its CSA onto 5540**, putting two crafts co-channel; its hub was
stopped for the measurement and restarted afterwards.

### Result: the §19 numbers reproduce with a real receiver

Craft `.232`, MCS pinned (`min_profile = max_profile = 1`), waybeam-link's
bitrate controller off, `video0.bitrate` overdriven to 20000, ground latched
and **`claimed_by: 9`**, eight 5-second samples per arm:

| | drain-stall | recv-stop |
|---|---|---|
| Craft `idr_frames` | **2 -> 2** (zero induced) | **127 -> 289** |
| Implied keyframe rate | **0 /s** | **~4.6 /s** |
| Craft delivered | ~50 fps | ~87 fps |
| Craft ring | 1-3 / 8 | 0-3 / 8 |
| Ground `frame_bytes` | ~9.5 Mbit/s | ~9.0 Mbit/s |
| Ground `frames_unrecoverable` | **0** | 0 |
| Ground `idr_frames` | **1, flat** | 1 -> 2 |

§19 measured ~4.4 IDR/s TX-side with nothing receiving; with a ground latched,
claiming, and decoding it is ~4.6 IDR/s. The result is not an artifact of
transmitting into an empty channel.

Note the two `idr_frames` counters are not the same quantity. The craft's
counts keyframes it *emitted* and was validated against the bitstream in §10;
the ground's counts far fewer and did not track the craft's, so it is
evidently not a received-IDR tally. Do not use the ground counter to judge the
actuator — the craft-side one is the validated channel.

### What is still not covered

The ground-side half of the recv-stop arm is only four samples: the ground hub
was stopped mid-run by a **second Claude session using the same adapters**,
which is also the orderly "Deactivated successfully" in its journal. Nothing
here is a fault. ARQ and receiver-requested recovery IDRs remain unexercised —
`frames_with_arq` stayed 0 throughout, so the uplink path was never stressed.

### Bench returned

`.232` craft.json restored (`max_profile 4`, `venc.enabled true`), venc gate
off and bitrate 5754, supervised start, hub up. `.181` hub restarted. The
ground hub was left **stopped**, the state the other session had put it in.

---

## 21. Operator-confirmed run, and the latency it costs

Ground `waybeam-hub` on the x86 host, scouted onto craft 17, latched and
**`claimed_by: 9`**, MCS pinned at `min_profile = max_profile = 1` and
waybeam-link's bitrate controller disabled. Three phases on the shipping build
(gate always on, no switch, counters live):

| Phase | Offered | `gateClosed` | `closeEvents` | `escapeEvents` | ring | `transportDrops` |
|---|---|---|---|---|---|---|
| Healthy | 5.7 Mbps | false | 9089, **frozen** | 33, frozen | 0 | 145, frozen |
| **Stall** | 20 Mbps | **true** | 9089 -> 9633 (**~27/s**) | 33, **frozen** | 2-3 / 8 | 145, **frozen** |
| Recovery | 5.7 Mbps | false | 9981, **frozen** | 33, frozen | 0 | 145, frozen |

Ground-side across the whole run: `idr_frames` **0 before and 0 after**.

The three columns together are the diagnosis, and none of them existed before
this work:

- **closes climbing, escapes flat** = consumer slow, gate working. Escapes
  climbing *instead* is the dead-consumer signature (measured separately at
  ~2/s with `transportDrops` climbing alongside).
- **drops frozen at 145 through a 3.5x overload** — the gate throttled
  production to what the radio could carry rather than overflowing the ring.
- **ring held at 2-3 of 8** — it never fills, and it empties the moment the
  offered rate drops back.

### The cost: ~500-1000 ms of added latency while gated

Operator observation on the live image, 2026-09-12: the picture stays clean
and continuous under stall, at a visible **~500-1000 ms** delay versus the
ungated stream. Their verdict — *"much better than the alternative of a broken
stream"* — is the right trade for FPV, but the latency is a real cost and
should not be filed as free.

**Where it comes from, and why it is not the ring.** The ring holds 2-3 slots
(~50 ms at the throttled rate), so it cannot account for it. Drain-stall works
by letting frames accumulate **inside VENC's output FIFO**, which is exactly a
latency buffer: nothing is dropped, so everything is delayed. That is the
mechanism's defining trade — recv-stop and plain ring-overflow *drop* frames
and stay low-latency; drain-stall *keeps* them and pays in delay.

**It may not be inherent.** Two untested routes to bound it, in order of
promise:

1. `MI_VENC_SetMaxStreamCnt` (exported, unbound — §12.4) caps the output FIFO
   depth, which directly caps the accumulated delay. A shallow FIFO turns the
   tail of the backlog into ring drops rather than latency, making the trade
   tunable instead of fixed.
2. Discard stale frames on reopen rather than draining oldest-first, so a
   recovering link shows current video immediately and takes one gap instead
   of a second of lag.

Both are measurable with the same method used throughout: the craft's own
`idr_frames` against a rate-limited or RF consumer. Neither is in this PR.

---

## 22. Latency attributed — and §21's explanation was wrong twice

### Method

`tools/frame_shm_rate_consumer.c` now records **age at read**: wall clock minus
the frame's capture `pts` (`VencFrameMeta.pts`, µs). The pts epoch is the SDK's
packet clock and need not match `CLOCK_MONOTONIC`, so the absolute value is not
a latency — but the *difference* between a healthy run and a gated one is,
because the offset cancels. No radio is involved: the craft's own encoder and
ring are the whole measurement.

**The measurement is only valid as the sole ring consumer.** frame-shm is
SPSC. `waybeam_hub` on the craft is a live consumer *and is supervised*, so
`S97waybeam-hub stop` must be verified to hold — an earlier sweep was silently
invalidated by the hub sharing the ring, which reads as an impossibly empty
ring and a delivered rate below the requested cap. Assert `hub procs: 0` and
`Mean ring > 0` before believing any number here.

### Result: the latency is the buffer depth times the consumer period

| `maxStreamCnt` | Consumer cap | Delivered | Mean ring | **Age at read** |
|---|---|---|---|---|
| default (3) | 200 fps (healthy) | 100.1 | 0.00 | **4.1 ms** |
| default (3) | 30 fps | 30.9 | 1.50 | **170.7 ms** |
| default (3) | 10 fps | 10.4 | 1.50 | **538.0 ms** |
| **1** | 200 fps (healthy) | 100.1 | — | **4.1 ms** |
| **1** | 30 fps | 31.0 (spread **0**) | 1.50 | **104.5 ms** |
| **1** | 10 fps | 10.4 | 1.50 | **341.2 ms** |

Ungated latency is **4.1 ms** and the ring is empty — the encoder is not the
problem. Gated, the age is a near-constant **~5.2 consumer-frame-intervals**
(170.7/33.3 = 5.1; 538.0/100 = 5.4) while the ring holds only 1.50 of them. So
the delay is not the ring and not downstream: **~3.7 frames sit inside VENC**,
which is the default `maxStreamCnt` of 3 plus the frame in flight.

That is why it looks like "hundreds of milliseconds" at low link rates and
nothing at all when the link keeps up: the depth is fixed, the period is not.

### §21 was wrong, and so was the correction to it

§21 said the delay was frames accumulating in VENC's output FIFO. Then, reading
that `MI_VENC_SetMaxStreamCnt` defaults to 3, I concluded the FIFO was far too
shallow to hold ~500 ms and that the delay must be downstream of venc. **Both
are wrong.** The FIFO *is* the buffer, and 3 frames *is* ~500 ms — when the
consumer is draining at 10 fps. Depth in frames only becomes a latency once
multiplied by the drain period, and I compared it against the 100 fps
production period instead.

### `MI_VENC_SetMaxStreamCnt` works: −38 % gated latency, no other cost

Setting it to 1 at bring-up (accepted, returns 0):

- 30 fps cap: 170.7 -> **104.5 ms** (−39 %)
- 10 fps cap: 538.0 -> **341.2 ms** (−37 %)
- Delivered rate unchanged, and *smoother* at 30 fps (spread 0 vs 1)
- Healthy path untouched at 4.1 ms
- Ring unchanged at 1.50 slots

It is continuity-safe **by construction**: the SDK drops the pending image
*before encoding* when the buffer is full (v2.12 §1.3.16,
"直接丢掉不编码"), so it cannot break the reference chain the way discarding an
encoded frame would. The doc also says to set it after channel creation and
before encoding starts, not dynamically — which is exactly where the frame gate
is already configured.

**Not in this PR.** It is a one-call change with a measured win, but it is
unreviewed, its effect on GDR continuity is argued from the doc rather than
measured on the wire, and this PR is already large. It is the obvious immediate
follow-up.

### The remaining ~3.2 frames

At `maxStreamCnt=1` the residue is ring (1.50) plus ~1.7 elsewhere. The ring
half is already tunable by an operator today: `frameGateCloseSlots` defaults to
3, and closing at 2 would trade a little shedding headroom for a slot of
latency. That trade is untested.

### Struck: discard stale frames on reopen

Ruled out on the same evidence, not merely deprioritised. Discarding *encoded*
frames is a post-encode drop, which breaks the reference chain for every
following P-frame — the v0.9.2 rollback already recorded in
`include/venc_ring.h`. With GDR there is no periodic IRAP to recover at, so the
damage would persist until the refresh wave painted it out. It is directly
hostile to the continuity this feature exists to protect.

---

## 23. `maxStreamCnt = 2` shipped, and the mirror-recording risk cleared

### Why 2 and not 1

Latency while gated is the bitstream buffer depth times the **consumer's**
frame period — measured at a 10 fps drain, each slot costs ~97 ms:

| depth | gated @10 fps | gated @30 fps | ungated |
|---|---|---|---|
| 3 (SDK default) | 534 ms | 171 ms | 4.1 ms |
| **2 (shipped)** | **436 ms** | — | 4.1 ms |
| 1 | 341 ms | 105 ms | 4.1 ms |

Each slot also buys one **producer** frame period (10 ms at 100 fps) of
tolerance to a consumer that stalls. 2 keeps one slot of that headroom; 1
leaves none, on a channel that in mirror mode also feeds an SD recorder where
flash GC can stall a write — the same hazard that earned ch1 a 64-frame buffer
(`star6e_pipeline.c:2898`). The ~95 ms that 1 would buy is not worth spending
the last slot of margin.

Set once, after `MI_VENC_CreateChn` and before `MI_VENC_StartRecvPic`, which is
where the SDK says to set it; it advises against changing it live, so this is a
constant rather than a config field. Advisory: a refusal or an older
`libmi_venc.so` without the symbol leaves the SDK default of 3, which costs
latency while gated and nothing else.

### Mirror recording on SD — clean

`.232`, `record.mode=mirror`, `record.dir=/mnt/mmcblk0p1` (58 GB card), gate
armed, hub consuming:

| Run | Frames | Elapsed | fps | `droppedFrames` | `writerPeakDepth` |
|---|---|---|---|---|---|
| 30 s | 2999 | 30006 ms | 99.9 | **0** | 2 |
| 128 s | 12885 | 128878 ms | 100.0 | **0** | **1** |

The 30 s file was pulled and decoded end to end: HEVC, `avg_frame_rate 100/1`,
6885 frames read, **zero decoder errors**. (`ffmpeg -f null` also prints
"non monotonically increasing dts" lines — those are the *null muxer*
complaining about duplicate DTS at 100 fps, not decode failures; a decode-only
run is silent.)

`writerPeakDepth` of 1-2 says the SD writer never backed up, so the shallow
buffer was never asked to absorb a stall. That is the honest limit of this
test: it shows no regression at depth 2 under normal SD behaviour, not that
depth 2 survives a 500 ms flash-GC stall. A recording that *did* hit GC would
show it as a rising `writerPeakDepth` first.

### The close-suppression interaction, observed

While a mirror recording runs the gate suppresses closes, and the measurement
shows exactly what that costs: the recorder took **every** frame (100 fps,
0 dropped) while the *stream* ring sat full at 8/8 and shed ~51 fps to
`transportDrops`. That is the documented trade — congestion punches holes in
the radio stream rather than in the SD file — and it is the one case where the
gate deliberately stops protecting the link.

### 23.1 All three backends capped — and what is verified where

The cap was Star6E-only when first written. It is now set on all three, since
all three share the drain-stall actuator and therefore the same latency:

| Backend | Call | Bring-up |
|---|---|---|
| Star6E | `MI_VENC_SetMaxStreamCnt(chn, 2)` | accepted, no warning |
| Maruko | `MI_VENC_SetMaxStreamCnt(dev, chn, 2)` | accepted, no warning |
| CV610 | `ss_mpi_venc_set_chn_param()` read-modify-write of `max_stream_cnt` | applied, no warning |

CV610 has no `SetMaxStreamCnt` symbol; the same control lives in
`ot_venc_chn_param.max_stream_cnt`, which also carries crop, frame rate and
`in_depth` — hence the read-modify-write rather than a fresh struct.

**Verification is uneven, and the difference matters:**

- **Star6E — before/after measured.** 534 -> 436 ms at a 10 fps drain, plus
  the SD mirror-recording runs above.
- **CV610 — applied, gated delta measured, no control.** Healthy to gated at a
  20 fps drain is +343 ms with the ring at 1.50 slots. There is no
  before-measurement, so the cap's *contribution* on CV610 is unproven — only
  that it is accepted and nothing regressed. Note CV610's `pts` epoch is far
  enough from `CLOCK_MONOTONIC` that the 32-bit age subtraction wraps; only
  run-to-run deltas are meaningful there.
- **Maruko — applied, and largely beside the point.** At a 5 fps drain the age
  is 1701 ms with the ring holding **6.28 of 8 slots**, so ~1256 ms of it is
  ring residency. Maruko's gate closes late (§16.4's overshoot, far worse here
  than Star6E's 1.5 slots), so the bitstream buffer is not its dominant term
  and this cap barely moves it. The lever for Maruko is the close threshold,
  not the buffer depth — `frameGateCloseSlots` is already exposed for it, and
  that trade is untested.

---

## 24. Maruko's ring overshoot explained: the escape pulse is a burst

§16.4 wrote the overshoot off as an artifact of a binary test load. It is not.
Under sustained deep over-subscription Maruko settles with the ring near full,
and that — not the encoder buffer — is where its gated latency lives.

### The counters name the mechanism

`.233`, 30 fps production, consumer capped at 5 fps (6:1), gate armed, hub
stopped so the rate consumer owns the ring:

```
framesSent   167 -> 247   (5.0 /s — exactly the consumer's drain)
usedSlots    6            (of 8, steady)
gateClosed   true
closeEvents  181 -> 209   (1.75 /s)
escapeEvents 180 -> 208   (1.75 /s)
```

`closeEvents` and `escapeEvents` advance **in lockstep, one apart**. Every
close follows an escape, so the gate never reopens through the drain path at
all — occupancy never returns to `open_slots`. It is running permanently on the
500 ms safety pulse.

Each pulse admits ~2.9 frames, not one. `FRAME_GATE_MIN_OPEN_US` is a 20 ms
*time* dwell and Maruko's gated loop polls every 2 ms, so one dwell drains the
whole buffered backlog out of VENC into the ring as a burst. The consumer takes
5 frames/s, escapes return 5 frames/s, and the ring equilibrates wherever that
balance lands — 6 of 8 — rather than draining to 1. Ring residency is then
~1200 ms of the 1701 ms measured age.

### Why neither available knob reaches it

- **`FRAME_GATE_STREAM_BUF_FRAMES`** (this PR) caps the encoder buffer, the
  smaller term. It bounds the burst, but the equilibrium is set by the balance.
- **`frameGateCloseSlots`** moves the threshold linearly and no further: 3 -> 2
  took occupancy from 7.0 to ~6.3. The residual is the burst, not the trigger.

Star6E does not show this. Under RF congestion it closed ~27 times a second
with `escapeEvents` flat — reopening normally, ring at 1.5. The difference is
the ratio: 100 fps production against a 20-30 fps drain does fall back to the
reopen threshold between bursts; 30 fps against 5 fps never does.

### What would fix it, untested

Make the escape admit **one frame**, not "whatever fits in 20 ms" — a
frame-counted dwell rather than a time-based one. The gate already observes
every write, so it can close again on the first frame that lands instead of
waiting out a fixed window. That changes `frame_gate_observe()`'s escape branch
for all three backends, so it wants its own measurement and review rather than
being folded in here.

### Assessment for this PR

Not a regression and not a blocker. Maruko still gets the headline result —
**zero keyframes** — and the escape is doing what §16.2 restored it to do:
keeping a starved stream alive and diagnosable rather than silently stopped.
What it costs on that board, under sustained deep congestion, is latency.

## 25. The frame-counted escape, measured on all three boards

§24 predicted that ending the escape pulse on the first frame that lands —
rather than after a fixed 20 ms — would decouple `escapeEvents` from
`closeEvents` and drop Maruko's ring toward the close threshold. Measured
2026-09-13 on all three backends. It does, and it costs nothing on the two
boards that were already healthy.

### Method

A/B in one session per board: the pre-fix binary is `a8cc58d^`, the fixed one
`a8cc58d`, built from the same tree minutes apart and identified by md5 at the
device before each arm. Load is `tools/frame_shm_rate_consumer.c` as the SOLE
ring reader — the supervised hub is stopped and the stop VERIFIED to hold,
because frame-SHM is SPSC and a second reader silently invalidates the run.
Each arm runs a warm-up then a matched measurement from the same starting
state.

### Maruko `.233` — 30 fps production, 5 fps drain (6:1)

The board the fix targets.

| | before (20 ms time dwell) | after (frame-counted) |
|---|---|---|
| Mean ring | 6.29 slots | **1.64** |
| Frame age, min | 1460.1 ms | **725.9 ms** |
| Frame age, mean | 2488.4 ms | **1226.4 ms** |
| `closeEvents` | 1.75 /s | 2.5 /s |
| `escapeEvents` | 1.75 /s (lockstep) | **flat** |
| IDR frames | 0 | **0** |
| Delivered | 5.6 /s, spread 1 | 5.6 /s, spread 1 |

Counter samples 10 s apart make the decoupling unambiguous:

```
before:  close 80 -> 97 -> 115    escape 79 -> 96 -> 114   (one apart, always)
after:   close 44 -> 69 -> 94     escape 34 -> 34 -> 34    (frozen)
```

`transportDrops` also stops creeping (205->208->211 before, flat at 75 after):
the gate is no longer overrunning the ring on every pulse. Delivered rate and
judder are unchanged — the consumer still gets its full 5.6 /s at spread 1 —
so the ~734 ms comes off latency, not off throughput.

### Star6E `.232` — 100 fps production, 25 fps drain (4:1)

| | before | after |
|---|---|---|
| Mean ring | 1.52 slots | 1.52 |
| Frame age, min | 149.3 ms | 153.7 ms |
| `escapeEvents` | flat (64) | flat (25) |
| IDR frames | 0 | 0/1 |

A no-op, as predicted: this board reopens through the normal drain path, so the
escape branch is almost never taken. Lifetime counters show the ratio directly
— 2611 closes against 64 escapes.

### CV610 `.181` — 100 fps production, 15 fps drain (6.7:1)

| | before | after |
|---|---|---|
| Mean ring | 1.55 slots | **1.54** |
| `escapeEvents` | flat (49) | flat (34) |
| Delivered | 15.6 /s, spread 1 | 15.6 /s, spread 1 |

Also a no-op. Note CV610 reaches the reopen threshold even at 6.7:1, so deep
over-subscription alone is not what produced Maruko's equilibrium — Maruko's
combination of a low production rate and a 2 ms gated poll interval is.

`Age at read` is unusable on CV610: its pts epoch is far enough from
`CLOCK_MONOTONIC` that the 32-bit microsecond subtraction wraps (mean reads
~4.23e6 ms, just under 2^32 us). Ring occupancy is the sound metric there.

### What this changes about §24

§24's "what would fix it, untested" is now tested and is the shipped
behaviour. Its claim that `FRAME_GATE_STREAM_BUF_FRAMES` and
`frameGateCloseSlots` could not reach the residual was right, and the reason
is confirmed: the residual was the burst, and the burst is what the
frame-counted dwell removes.

### Bench note

`.232` is shared. During one Star6E run the supervised hub restarted
mid-measurement and the resulting numbers were discarded — the tell is the
documented one, an impossibly empty ring (0.00 slots) with delivered rate
BELOW the consumer's cap (23.3 against 25). Re-verify the stop before every
arm, not just once per session.
