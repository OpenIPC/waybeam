# `unix://` Transport Fix — Handover & On-Device Validation Plan

<!-- version: 1.3.0 -->

Branch: `claude/unix-socket-speed-limits-az62s4` · Target release: `0.63.0`

Status: **Star6E V1–V9 and Maruko V10–V11 confirmed on device.** The root-cause measurements
below marked *(measured)* came from a Linux 6.18 x86-64 container. The full
Star6E matrix completed on SSC338Q, including 25 Mbps at 60 and 120 fps,
backpressure recovery, UDP/SHM regression, live redirect, and shared-socket
audio. Maruko parity completed on SSC378QE at 10–25 Mbps and 60/120 fps,
including deterministic backpressure recovery.

---

## 1. What was wrong

Investigation started from `unix://` streams misbehaving at and above 15 Mbps
with 1500-MTU RTP. AF_UNIX itself is not the bottleneck — the same send
pattern measures **7.6 Gbps** *(measured)* when the consumer keeps up. Three
separate defects converged at that bitrate.

### 1.1 `net.unix.max_dgram_qlen` = 10 (root cause)

An AF_UNIX datagram sender blocks on the **receiver's** queue depth. The
kernel snapshots that depth from `net.unix.max_dgram_qlen` into
`sk_max_ack_backlog` when the *receiving* socket is created.

The default of 10 datagrams is ~7 ms of buffer at 15 Mbps with 1400-byte
payloads. Frame sizes against that queue:

| Bitrate @ 60 fps | Bytes/frame | RTP packets | vs. 10-deep queue |
|---|---|---|---|
| 5 Mbps | 10.4 KB | ~8 | fits |
| **15 Mbps** | **31 KB** | **~23** | **2.3× over** |
| 15 Mbps IDR (~4×) | 125 KB | ~90 | 9× over |

`STAR6E_OUTPUT_BATCH_MAX` is 64, so a frame is handed to one `sendmmsg`.
Below ~5 Mbps a frame fits the queue; at 15 Mbps every frame forces at least
two block/wake round-trips. That is the 15 Mbps threshold.

Measured saturation point: **11 in-flight datagrams** *(measured)*.

### 1.2 The blocking send sat inside the VENC critical section

`star6e_output_end_frame()` → `star6e_batch_flush()` runs between
`MI_VENC_GetStream` (`src/star6e_runtime.c:1175`) and `MI_VENC_ReleaseStream`
(`src/star6e_runtime.c:1293`), on the thread pinned to CPU 0. An unbounded
block therefore holds a VENC output slot and stalls capture rather than
dropping packets. Measured up to **74 ms** for a single `sendmmsg` burst
against a slowed consumer, and unbounded (**634 ms** observed) against a
wedged one *(measured)*.

### 1.3 Backpressure telemetry could not fire

`UNIX_DGRAM_AVG_SKB_TRUESIZE_BYTES` was 4096; a 1400-byte RTP datagram
measures **2304** *(measured)*. A fully blocked socket therefore read as 61%
fill against a 75% high-water mark — `inPressure` never set.

Worse, the denominator was derived from the *sender's* live view of the
sysctl, which is not what the receiver captured. With the sysctl raised after
the consumer started, a 100%-blocked socket reported **2% fill** *(measured)*.

### 1.4 No drop accounting

`transportDrops` / `packetsSent` were hardcoded to 0 for socket transports
(acknowledged as future work at `src/star6e_video.c:154`). Combined with the
above: a failing `unix://` consumer produced no EAGAIN (blocking), no pressure
flag, and no counter — only unexplained encoder timing jitter.

---

## 2. What changed

### 2.1 Queue depth — a **system** setting, not a venc setting

**This is deliberate and is the key deployment fact.**

`init.d/S95waybeam` and `init.d/S95waybeam.cv610` raise `net.unix.max_dgram_qlen`
to at least 256 in `start()`, before launching the daemon. It is a system-wide
sysctl, applied by the init script that ships in this repo and installs to
`/etc/init.d/S95waybeam` (see `scripts/star6e_direct_deploy.sh:11`; on CV610,
`Makefile:405`).

The `waybeam` binary **never writes the sysctl**. Two reasons:

1. Raising it from venc could not help anyway — the value only affects
   sockets created *after* the write, and the consumer's socket already
   exists by then.
2. Silently mutating a global kernel setting from a video encoder is a
   surprising side effect for anything else on the box.

venc only *warns*, in two places:

- **At socket open** (`output_socket_warn_dgram_qlen`) if the sysctl reads
  below 256.
- **At first saturation** (`output_socket_note_saturation`) if the peer's
  *actual* queue turns out shallow. This is the only way to detect a consumer
  that was started **before** the raise: the sysctl reads healthy, but that
  consumer's socket kept the shallow depth it was born with, so the first
  warning stays silent.

> ⚠️ **Ordering risk to validate on device.** `S95waybeam` runs at S95. If the
> `unix://` consumer is started by an init script with a **lower** number, it
> is created before the raise and keeps a 10-deep queue — and the startup
> warning will not fire. See check **V3**.
>
> If that is the case on real hardware, the raise must move earlier
> (`/etc/sysctl.conf`, or an `S0x`/`S1x` script). This repo does not ship the
> consumer's init script, so it could not be verified here.

### 2.2 Bounded sends

- `SO_SNDTIMEO` = 2 ms on `unix://` sockets (`src/output_socket.c`), bounding
  a single `sendmsg`.
- `STAR6E_OUTPUT_FLUSH_BUDGET_US` / `MARUKO_OUTPUT_FLUSH_BUDGET_US` = 4000,
  a cumulative per-frame send budget for batched RTP, shared by every internal
  64-packet `sendmmsg` flush. Once exhausted, the rest of that encoded frame is
  counted and discarded without opening a fresh deadline.

Both are required: `sendmmsg()` applies `SO_SNDTIMEO` **per message**, so the
timeout alone still let a 64-packet batch accumulate to 5.6 ms *(measured)*.
4 ms sits under a 120 fps frame period (8.3 ms). With an adequately sized
queue neither ever fires — a healthy 15 Mbps frame flushes in ~150 µs
*(measured)*.

**Non-blocking sockets were evaluated and rejected.** At the default qlen they
dropped **54%** of packets *(measured)* — far worse than the stall. Bounded
blocking keeps zero drops whenever the queue is adequate:

| max_dgram_qlen | blocking (before) | non-blocking + drop | **shipped: bounded blocking** |
|---|---|---|---|
| 10 | 5.5 ms stall/frame, 0 drops | 160 µs, **54% loss** | bounded 4 ms, drops counted |
| 256 | 169 µs, 0 drops | 160 µs, 0 drops | **169 µs, 0 drops** |

### 2.3 Self-calibrating fill denominator

Rather than a better truesize constant, `output_socket_note_saturation()`
records `SIOCOUTQ` at the moment a send blocks — by definition the exact full
queue. This is immune to the sysctl-skew problem in §1.3. The 2304 constant
remains only as a bootstrap before the first saturation event.

Verified on host: learned 25344 B / 11 datagrams at qlen=10, and
592128 B / 257 datagrams at qlen=256 *(measured)* — both exact.

### 2.4 Drop accounting

`socket_drops` / `socket_writes` on both output structs, surfaced as
`transportDrops` / `packetsSent` in the sidecar trailer and
`GET /api/v1/transport/status`. Congestion (`EAGAIN`/`ENOBUFS`) is counted
separately from hard errors via `star6e_account_send_failure()`, covering the
batch, RTP-fallback, and compact-mode paths.

### 2.5 Seqlock spin yields

`begin_frame()` busy-spun while `apply_server()` held an odd generation across
`socket()`/`setsockopt()`/`connect()`. The encode thread is pinned to CPU 0
and could starve the writer it was waiting on. Now `sched_yield()`.

---

## 3. Files changed

| File | Change |
|---|---|
| `include/output_socket.h` | `OutputSocketQueue`, qlen constant, new API |
| `src/output_socket.c` | `SO_SNDTIMEO`, truesize 4096→2304, calibration, warnings |
| `include/star6e_output.h` / `src/star6e_output.c` | flush deadline, drop accounting, failure classifier, yield |
| `include/maruko_output.h` / `src/maruko_output.c` | same, mirrored |
| `src/star6e_controls.c` / `src/maruko_controls.c` | new status fields |
| `src/star6e_video.c` | real values in sidecar trailer |
| `init.d/S95waybeam` | `raise_unix_dgram_qlen()` |
| `tests/test_star6e_output.c` | strengthened + new tests |
| `README.md`, `documentation/HTTP_API_CONTRACT.md`, `HISTORY.md`, `VERSION` | docs |

---

## 4. Host verification already done

- `make lint` — clean
- `make test` / `make test-werror` — 2314 pass, 0 fail
- `make test-asan`, `make test-tsan` — clean
- `make build SOC_BUILD=star6e`, `SOC_BUILD=maruko` — both clean

Both new tests were confirmed to **fail against the pre-fix code** rather than
passing vacuously:

- reverting the truesize constant + calibration → 4 failures
- removing `SO_SNDTIMEO` → `unix bound` hangs the runner outright

Note the old `unix bp` test is *why* §1.3 shipped: it pumped 1024-byte
payloads and wrapped its high-water assertions in `if (fill_pct >= 75)`, so
they never executed.

---

## 5. On-device validation plan

Hardware: Star6E first (per AGENTS.md backend policy), then Maruko.
Deploy: `scripts/star6e_direct_deploy.sh` / `scripts/maruko_direct_deploy.sh`.
Both helpers now start the standard `/usr/bin/waybeam` through
`/etc/init.d/S95waybeam`, so the queue-depth setup is part of a normal cycle.

Build the target-local receiver with `make unix-dgram-consumer
SOC_BUILD=<backend>` and deploy `out/<backend>/unix_dgram_consumer` to `/tmp`.
It binds a Linux abstract datagram name and reports delivered bitrate, RTP
sequence gaps, marker-frame cadence, maximum first-to-last receive spread, and
packet counts by RTP payload type (used to verify shared-socket audio).
The receive spread is a same-host proxy; the exact
`last_pkt_send_us - frame_ready_us` value still requires sidecar correlation.

### Results — 2026-08-01/02, SSC338Q / IMX335 (`192.168.2.232`)

V1 passed with `net.unix.max_dgram_qlen=1024` in the first run and 256 in the
completion run; `S95waybeam` starts before `S96waybeam-link`. At 1400-byte
payloads, the target-local consumer reported:

| Configured | Delivered | RTP gaps | Transport-drop delta | Max cadence interval | Max send/receive spread | Result |
|---:|---:|---:|---:|---:|---:|---|
| 10 Mbps | 10.054 Mbps | 0 | 0 | 17.454 ms | 0.359 ms | PASS |
| 15 Mbps | 15.176 Mbps | 0 | 0 | 17.309 ms | 0.546 ms | PASS |
| 20 Mbps | 20.250 Mbps | 0 | 0 | 17.477 ms | 0.687 ms | PASS |
| 25 Mbps / 60 fps | 25.063 Mbps | 0 | 0 | 18.005 ms | 1.035 ms receive | PASS |
| 25 Mbps / 120 fps | 25.340 Mbps | 0 | 0 | 10.817 ms sender | 0.888 ms sender | PASS |

The earlier 25 Mbps device-unresponsive incident did not recur after the human
power cycle; see `documentation/CRASH_LOG.md`. Authoritative sidecar sampling
at 120 fps covered 1,072 frames (P95/P99 send spread 227/520 µs). The
pre-existing `transportDrops=27` value did not change during that run.

V2 passed: with qlen deliberately set to 10 and init bypassed, the startup
warning appeared exactly once. V3 passed: the consumer starts after S95 and no
shallow-peer warning appeared under saturation.

V5/V6 passed with a five-second stall at 15 Mbps: fill reached 100%,
`inPressure` asserted, `transportDrops` increased by 2,705, then fill returned
to 0% and pressure cleared. Encoding and delivery recovered without a venc
restart, `MI_VENC_GetStream` errors, or watchdog activity.

V7 passed at 25 Mbps. UDP delivered 25.030 Mbps at 59.93 fps with zero RTP
gaps or new drops; sidecar maximum send spread was 1.313 ms. SHM sustained
24.6–25.5 Mbps in steady state with zero new ring drops and no encoder errors.
On this APFPV-mode craft S95 intentionally rewrites `shm://` to loopback UDP,
so the SHM-only run used the standard binary directly after explicitly
applying the qlen sysctl; no bootloader or init configuration was changed.

V8 passed using the implemented live endpoint (`GET /api/v1/live/set`): Unix
delivered before and after a Unix → UDP → Unix switch, the UDP phase delivered
15.240 Mbps with zero gaps, and all phases reported zero drops.

V9 passed with `audioPort=0`: across a five-second shared-socket stall the
consumer received 16,243 H.265 packets (PT 97) and 840 Opus packets (PT 98),
matching the expected Opus cadence over the 17 non-stalled seconds. Audio and
video both recovered. Audio EAGAIN remains intentionally absent from
`transportDrops`, as documented in §6.

After all Star6E tests, the byte-identical saved production config was restored,
`S95waybeam` and `S96waybeam-link` were started normally, and the frame-SHM
counter advanced from 2,275 to 2,576 with zero additional drops.

V10 passed on 2026-08-02 using SSC378QE / IMX335 (`192.168.2.233`). V1 raised
qlen from 10 to 256 before daemon start. At 60 fps the consumer delivered
10.327, 15.468, 20.455, and 25.765 Mbps with zero RTP gaps and zero transport
drops at every rate. At 25 Mbps / 120 fps it delivered 25.125 Mbps with zero
gaps/drops. Authoritative sidecar maximum Unix send spread was 2.155 ms at
60 fps and 3.567 ms at 120 fps, both below the 4 ms budget. A five-second
stall at 15 Mbps reached 100% fill, asserted pressure, counted 5,445 drops,
then returned to 0% fill and recovered without a stream or watchdog error.
The target's original 0.60.1 binary, init script, and UDP configuration were
restored byte-for-byte afterward and verified healthy.

V11 verified `outgoing.allowUnixEncoderStall` on the same Maruko target at
15 Mbps / 60 fps. With the default `false`, a four-second consumer pause
produced 4,254 transport drops and then recovered normally. With the option
enabled through `/api/v1/set`, a six-second pause held queue fill at 100%; two
status samples two seconds apart both showed `packetsSent=5784` and
`transportDrops=0`. After the consumer resumed, `packetsSent` advanced to
8,678. The consumer received 14,285 packets with zero RTP sequence gaps and a
6.016-second maximum marker gap, directly confirming stall-then-resume rather
than packet loss. Live FPS returned to 60, the process stayed healthy, and no
GetStream, watchdog, kernel fault, oops, or panic appeared. The original
0.60.1 binary, init script, config, and qlen were restored; all three file
hashes matched the pre-test backups byte-for-byte.

### V1 — Baseline: the sysctl is actually applied

```sh
/etc/init.d/S95waybeam restart
cat /proc/sys/net/unix/max_dgram_qlen
```

**Pass:** `256` or greater.
**Fail:** `10` → `raise_unix_dgram_qlen()` did not run or `/proc` is not
writable. Check the boot log for the `net.unix.max_dgram_qlen 10 -> 256` line.

### V2 — Startup warning fires when it should

Set the sysctl back to 10, restart venc with a `unix://` output, read
`/tmp/waybeam.log`.

```sh
echo 10 > /proc/sys/net/unix/max_dgram_qlen
# start venc WITHOUT the init script so the raise is skipped
```

**Pass:** the `WARNING: net.unix.max_dgram_qlen=10 is too shallow` block
appears exactly once.

### V3 — ⚠️ Consumer startup ordering (highest-value check)

Determine what actually consumes the `unix://` socket on the device and when
it starts relative to S95.

```sh
ls /etc/init.d/                       # what starts the consumer, at what S-number?
cat /proc/sys/net/unix/max_dgram_qlen # after full boot
```

Then confirm the consumer's *real* queue depth by watching for the
second-stage warning under load (see V5).

**Pass:** consumer starts after S95, or the sysctl is set earlier (e.g.
`/etc/sysctl.conf`), and no "peer queue holds only ~N datagrams" warning
appears.
**Fail:** that warning appears → move the raise earlier than S95 and re-test.
**This is the most likely thing to be wrong on real hardware.**

### V4 — Throughput and stability at and above 15 Mbps

Configure `outgoing.server = unix://<name>`, `maxPayloadSize = 1400`.
Sweep bitrate: 10 / 15 / 20 / 25 Mbps at 60 fps, then 25 Mbps at 120 fps.

Success criteria per step:

| Metric | Source | Target |
|---|---|---|
| Delivered bitrate | receiver-side | within 5% of configured |
| `transportDrops` | `/api/v1/transport/status` | **0** in steady state |
| `inPressure` | same | `false` in steady state |
| Encoder frame cadence | `--verbose` / sidecar `frame_ready_us` | no gaps > 1.5× frame period |
| Sidecar `last_pkt_send_us` spread | rtp_timing_probe | no per-frame spread > 4 ms |

**Primary goal: 15 Mbps and 25 Mbps run clean where they previously did not.**

### V5 — Backpressure now actually reports

Deliberately stall the consumer (`SIGSTOP` it, or a test consumer that sleeps).

**Pass, in order:**
1. `fillPct` climbs past **75** — this is the §1.3 fix; before, it capped at 61.
2. `inPressure` flips `true`.
3. `transportDrops` increments.
4. On `SIGCONT`: `fillPct` falls below 50, `inPressure` clears.

**Fail:** `fillPct` plateaus in the 50–70 band → the calibration did not run,
or the target kernel's truesize differs enough to matter. Capture the learned
value from the calibration log line.

### V6 — Encoder is not stalled by a wedged consumer

`SIGSTOP` the consumer for ~5 s while streaming at 15 Mbps.

**Pass:** venc keeps encoding; repeated `/api/v1/transport/status` snapshots
show `transportDrops` rising; no
`MI_VENC_GetStream` timeout errors; the stream recovers on `SIGCONT` without a
venc restart.
**Fail:** `GetStream` errors, watchdog restart, or capture-side frame loss →
the 4 ms budget is too generous for this platform; reduce
`STAR6E_OUTPUT_FLUSH_BUDGET_US`.

### V7 — No regression on `udp://` and `shm://`

Re-run V4 with `udp://` and `shm://`. Both paths were touched (shared
`output_socket.c`, shared status/trailer plumbing).

**Pass:** unchanged behaviour and bitrate; `udp://` shows `transportDrops = 0`
on a healthy link. Specifically watch CPU on the encode thread under a
congested link — the `ENOBUFS` busy-spin regression was caught and fixed in
review, and this is the check that would have caught it.

### V8 — Live redirect still works

`GET /api/v1/live/set?outgoing.server=...` switching `outgoing.server` between
`udp://` and `unix://` while streaming.

**Pass:** switch takes effect, no crash, no stall, counters reset sanely, the
calibration warning may re-fire once per new socket (expected).

### V9 — Audio path

With `audioPort = 0` (audio shares the video socket) on `unix://`.

**Pass:** audio continues under congestion. Known gap: audio-path EAGAIN drops
are **not** counted into `transportDrops` (see §6).

### V10 — Maruko parity

Repeat V1, V4, V5, V6 on Maruko.

---

## 6. Known gaps and accepted risks

1. **The 2304 truesize constant was measured on x86-64 / Linux 6.18**, not on
   the ARM target. It only matters before the first saturation event
   calibrates the real value. To pin it exactly on-device, the probe used
   during investigation can be re-run there. Low impact; worth confirming
   during V5.

2. **Audio-path congestion drops are not counted.** `send_audio_rtp()` goes
   through `output_socket_send_parts()` directly and its `EAGAIN` returns are
   not classified into `socket_drops`. Audio will now drop rather than block,
   which is the desired behaviour, but it is invisible in telemetry.

3. **Benign data race on `send_queue`.** The producer thread writes
   `unix_capacity` in `note_saturation()` while the HTTP thread reads it in
   `query_transport_status()`. Plain aligned `int` access on ARMv7 is
   single-instruction, and this is the same pattern as the pre-existing
   `send_buf_capacity` read. A torn read would corrupt one advisory telemetry
   sample. Not fixed to avoid widening scope; flagged for a maintainer call.

4. **`socket_writes` is `uint32_t`** and wraps in ~37 days at 1340 pps. This
   matches the existing SHM counters and the wire trailer's field width, so it
   is consistent rather than new.

5. **`connect()`ing the unix socket was evaluated and rejected** — only 4%
   cheaper per datagram *(measured)*, and it would regress transparent
   consumer restart (an unconnected `sendto` re-resolves the abstract name;
   a connected socket would need explicit `ECONNREFUSED` recovery).

6. **The timeout values remain compile-time, but blocking is selectable.**
   `outgoing.allowUnixEncoderStall=true` disables both the 2 ms Unix send
   timeout and cumulative RTP frame budget for `unix://`, restoring the legacy
   behavior for transmission chains that intentionally use encoder blocking
   as flow control. The default remains bounded (`false`).

7. **Compact stream mode has only the per-send timeout.** The cumulative 4 ms
   frame budget applies to the production batched RTP path. Compact mode is
   protected from an unbounded individual send by `SO_SNDTIMEO`, but a frame
   split into many compact datagrams can still spend multiple timeout windows.
   Add a compact-frame budget only if that legacy mode must be supported over
   `unix://`; it is not exercised by V4–V10.

---

## 7. Rollback

The PR is intentionally split into reviewable commits. The init.d change is
independently revertible and is the highest-value part — if the code changes
need backing out, **keep the `max_dgram_qlen` raise**, since it alone resolves
the dominant symptom.
