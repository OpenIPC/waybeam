# Review Fix Verification — findings 1–3

Verification for the fixes on `fix/adversarial-review-1-2-3` (base `origin/master`
`55d6844`), from the adversarial review of the last two weeks of work.

| # | Severity | Fix | Files |
|---|----------|-----|-------|
| 1 | CRITICAL | Size the `.bin` load buffer for the vendor reader's worst-case copy (heap overread) | `include/cv610_pq_bin_load.h`, `src/cv610_pq_bin_load.c`, `src/cv610_pq_bin.c`, `tests/test_cv610_pq_bin_load.c` |
| 2 | MAJOR | Maruko's gated drain path now services HTTP record control and the sidecar | `src/maruko_pipeline.c` |
| 3 | MAJOR | CV610 advertises the frame-gate tunables it reads | `src/venc_api.c`, `tests/test_venc_api.c` |

## 1. Host regression (automated, runs without hardware)

```
make test        # includes test_cv610_pq_bin_load + the CV610 gate-capability checks
make test-asan   # the guard that makes the pq-bin loader test bite (also in make test-ci)
make lint
make lint SOC_BUILD=maruko
make lint SOC_BUILD=cv610 CV610_SDK_INC=<sdk> CV610_SDK_LIB=<libs>
make verify      # both backends + webui-check
```

The guards that would have caught the original defects:

- `test_cv610_pq_bin_load`: loads a file, then performs the vendor reader's
  worst-case copy (`36 + 12 + 4*16` into the section for 1298 bytes, ending 60
  bytes past the file). Under ASan an allocation sized for the file alone is a
  heap-buffer-overflow — the original defect. It also checks the length cap,
  content round-trip, missing-file handling, and that the slack is zeroed.
- `test_venc_api`: `frame gate close slots/max closed supported cv610` (plus
  alias, star6e and maruko crosses). Before the fix these returned 0 for CV610.

CV610 lint needs the OpenHisilicon SDK headers and libs; on this machine:

```
make lint SOC_BUILD=cv610 \
  CV610_SDK_INC=/home/snokvist/dev/hisilicon/oh \
  CV610_SDK_LIB=/home/snokvist/dev/hisilicon/firmware/output-cv6xx/per-package/hisilicon-opensdk/target/usr/lib
```

## 2. Hardware verification

Run on 2026-09-13 on CV610 `192.168.2.181` (imx662) and Maruko `192.168.2.233`,
both updated from 0.85.3 to 0.85.4 for the run. All three findings passed; the
steps below double as a runbook, each naming its pass condition, with observed
results and the run's two limits (HW-1(d) cannot discriminate fixed from
unfixed on device, and the closed-gate window is bounded by
`frameGateMaxClosedMs`) noted per step. Star6E was not exercised — none of the
three fixes touch it.

### HW-1 — CV610 `.bin` import safety (finding 1)

Craft with `libbin.so` staged. `make stage SOC_BUILD=cv610` skips the vendor
lib unless `CV610_PQ_LIB` is overridden (its default `../hisilicon/...` does
not exist in this checkout); point it at the local vendor copy:

```sh
make stage SOC_BUILD=cv610 \
  CV610_PQ_LIB=/home/snokvist/dev/hisilicon/vendor/pq/libbin.so
```

```sh
HOST=<cv610-ip>
# (a) known-good import still works, and still applies the 3DNR half
scp iq-profiles/cv610-bin/imx662.bin root@$HOST:/tmp/tune.bin
wget -qO- "http://$HOST/api/v1/set?isp.sensorBin=/tmp/tune.bin"
ssh root@$HOST "grep -m1 'loading /tmp/tune.bin' /var/run/waybeam.log || grep -m1 'loading /tmp/tune.bin' /tmp/waybeam.log"
ssh root@$HOST "grep -m1 'ISP tuning applied' /var/run/waybeam.log || grep -m1 'ISP tuning applied' /tmp/waybeam.log"

# (b) truncated tail: the 3DNR half must be skipped, never overread
ssh root@$HOST "head -c 143900 /tmp/tune.bin > /tmp/tune-trunc.bin"
wget -qO- "http://$HOST/api/v1/set?isp.sensorBin=/tmp/tune-trunc.bin"
wget -qO- "http://$HOST/api/v1/version"          # still 200 -> daemon alive
ssh root@$HOST "dmesg | tail -5"                 # no segfault/oops

# (c) export/round-trip (path is fixed by the endpoint)
wget -qO- "http://$HOST/api/v1/iq/export_bin"    # {"path":"/tmp/isp_export.bin","bytes":N}
ssh root@$HOST "cp /tmp/isp_export.bin /tmp/roundtrip.bin"
wget -qO- "http://$HOST/api/v1/set?isp.sensorBin=/tmp/roundtrip.bin"

# (d) the exact shape the fix exists for: 16 NRX records in a 1350-byte
# section. imx662.bin carries count=1 at section+44, so (a)-(c) cannot overrun
# even on the unfixed build. Patch the count to 16 (section = file_len - 1350).
cp iq-profiles/cv610-bin/imx662.bin /tmp/tune-c16.bin
printf '\x10' | dd of=/tmp/tune-c16.bin bs=1 \
  seek=$(( $(stat -c %s /tmp/tune-c16.bin) - 1350 + 44 )) conv=notrunc
scp /tmp/tune-c16.bin root@$HOST:/tmp/
wget -qO- "http://$HOST/api/v1/set?isp.sensorBin=/tmp/tune-c16.bin"
wget -qO- "http://$HOST/api/v1/version"          # still 200 -> daemon alive
ssh root@$HOST "dmesg | tail -5"                 # no segfault/oops
```

Pass: (a) the load line reads `(143424 B ISP + 1350 B 3DNR)` (nonzero 3DNR — a
gate tightened on the reader's internal layout would print `0 B 3DNR` here and
silently drop the 3DNR half), 200, and the tuning line; (b) the log names the
3DNR section as too short ("importing the ISP half only") and reports
`0 B 3DNR`, the API stays up and dmesg is clean; (c) the round-trip import also
reports 1350 B 3DNR and applies; (d) the import is **refused** with HTTP 500 and
`ERROR: [pq] OT_PQ_BIN_ImportBinData failed 0xcb000102` in the log, with the
API alive and dmesg clean. Note (b) and (d) do not discriminate fixed from
unfixed by themselves: (b) leaves 476 B, below the 1350-byte gate, so both
builds skip 3DNR; (d) is the overreading shape, but the patched file fails the
vendor's own import validation before the copy matters, and on the unfixed
build a 60-byte read past a heap block is typically silent — the host
`make test-asan` run is the detector, (d) just exercises the shape end to end.
Fail: any crash, hang, or silent no-op that leaves the daemon unreachable.
Observed: (a) the repo `imx662.bin` (md5 `be8313cc…`) equals the bench's
`/etc/sensors/imx662.bin`; (b) 200, warning plus `0 B 3DNR`; (c) export returned
`{"path":"/tmp/isp_export.bin","bytes":144774}` and reimported as 1350 B 3DNR;
(d) 500 refusal, daemon alive, dmesg clean.

### HW-2 — Maruko gated record/sidecar servicing (finding 2)

Maruko bench. `frame-shm://` is CV610's default; Maruko's `outgoing.server`
defaults to `""`, and the gate is inert (with a warning) on any other
transport. Set it explicitly and restart first. `record.format` must be `ts`
or `hevc`, and `record.mode` mirror (the default) so `ctx->dual` is NULL. This
fix has no automated coverage, so this bench run is the guard. The point is to
act **while the gate is closed**, so make a pre-fix hang unambiguous by raising
the ceiling to 60 s. The API accepts one field per request — a combined
`?a=..&b=..` query is rejected with 400 — and every restart-class set answers
`reinit_pending:true`, so send each field on its own and let each restart
settle before the next request.

```sh
HOST=<maruko-ip>
# one field per request; <name> must match the frame-shm consumer (bench used venc_frame):
wget -qO- "http://$HOST/api/v1/set?outgoing.server=frame-shm://<name>"
wget -qO- "http://$HOST/api/v1/set?video0.frameGateCloseSlots=4"
# each set above reports reinit_pending:true; let the restart settle, then:
wget -qO- "http://$HOST/api/v1/set?video0.frameGateMaxClosedMs=60000"
wget -qO- "http://$HOST/api/v1/transport/status"   # confirm gateClosed:true, gateClosedMs climbing
```

With the gate confirmed closed (stop the frame-shm consumer so the ring fills,
if it is not already):

```sh
# the recorder does not create the target directory; make it first
ssh root@$HOST "mkdir -p /tmp/gate-rec"
wget -qO- "http://$HOST/api/v1/record/start?dir=/tmp/gate-rec"
sleep 2
wget -qO- "http://$HOST/api/v1/record/status"      # recording, not idle
ssh root@$HOST "ls -l /tmp/gate-rec"
```

Pass: the recording opens within ~1 s of the request, i.e. NOT after the 60 s
escape, with `gateClosed` still true (observed: 0.28 s — the 0.25 s poll
granularity — with `gateClosedMs` still climbing). Before the fix the start
flag was only drained inside `maruko_pipeline_process_stream()`, which the gated
branch returns before, so it waited for the escape pulse (observed pre-fix: not
active after 12.5 s). Sidecar: run the link's sidecar probe while gated and
confirm subscription/clock-sync replies with `gateClosed` still true throughout
(observed: 8 replies, RTT 247–251 µs). Note the closed window is bounded: the
gate is closed at most `frameGateMaxClosedMs`, after which the escape reopens
it; with a dead consumer it then **stays open** — the ring overflowing and
reporting `full_drops` is the designed outcome — so run the "act while closed"
checks inside that window. Restore: `record/stop`, delete `/tmp/gate-rec`, set
each gate knob back to 0 in its own request (a combined query is a 400), waiting
for each restart, and restart the consumer.

### HW-3 — CV610 frame-gate capabilities (finding 3)

```sh
scripts/api_test_suite.sh <cv610-ip> <port>
```

Pass: the new section prints
`PASS capabilities: video0.frame_gate_close_slots supported on cv610` and the
same for `video0.frame_gate_max_closed_ms`, with zero failures elsewhere.
Manual equivalent:

```sh
HOST=<cv610-ip>
wget -qO- "http://$HOST/api/v1/capabilities" | python3 -c \
  "import sys,json; f=json.load(sys.stdin)['data']['fields'];\
   print(f['video0.frame_gate_close_slots']['supported'], f['video0.frame_gate_max_closed_ms']['supported'])"
wget -qO- "http://$HOST/api/v1/set?video0.frameGateMaxClosedMs=1000"
```

Pass: `True True`, and the set returns 200 with `reinit_pending:true`. Before
the fix both were `False` and the set returned 501 (both observed on the bench).
Then confirm the gate really arms on CV610 by repeating the HW-2 consumer-stall
step and watching `gateCloseEvents` in `/api/v1/transport/status` increment.
