# CV610 resilience and H.265 multi-slice plan

> **Superseded in part by 0.69.0.** venc no longer requests an IDR in response
> to its own egress — the ring-full recovery IDR described below was removed
> for every backend and every stream type, because the receiver is the only
> party that can see whether a decoder is actually broken. Recovery now comes
> from the operator-selected GOP cadence, an explicit `/request/idr`, or the
> receiver's own recovery request. Everything else here — the GDR metadata, the
> slice layout, the bench measurements — still stands; read the recovery-IDR
> passages as a record of what 0.6x did, not of current behaviour.

Status: implemented and confirmed on CV610 and Maruko devices on 2026-08-22.

Branch: `feature/encoder-resilience-slices`, based directly on
`origin/master` (`921b131`) so the already-squashed Star6E slice work is not
reintroduced as duplicate ancestry.

## Objective

Bring the existing resilience contract and whole-access-unit H.265 multi-slice
support to the CV610 backend without changing the config schema or frame-SHM
wire format. Preserve the current Star6E behavior and only expose CV610 modes
whose vendor behavior is confirmed on the device.

The first release keeps `slice_output_en` disabled. Early per-slice delivery is
a different transport contract: the current CV610 output loop treats every
`GetStream` result as a complete access unit and advances frame-SHM/RTP state
once per result.

## Evidence gathered

The online CV610 bench at `192.168.2.181` was inspected and tested on 2026-08-22.
It was running Waybeam `0.65.10`, H.265 1920x1080 at 100 fps, on Linux 5.10.221.
The device's `/usr/lib/libss_mpi.so` SHA-256 was
`9ec42150a2901376a91ccd1d3f051dc721b29d1895e7a3190539cb8df4bb4e1a`.

The shipped library exports and the running channel accepts the getter side of:

- intra refresh;
- reference parameters;
- slice splitting;
- hierarchical QP;
- frame-loss strategy;
- super-frame discard/re-encode strategy;
- H.265 deblock and SAO controls.

Current channel readback was:

| Control | Current value |
|---|---|
| Intra refresh | disabled; row mode; 8 units; request-I QP 51 |
| Reference parameters | base 1; enhance 0; prediction enabled |
| Slice splitting | disabled; mode 1; size 1; early output disabled |
| Frame-loss strategy | disabled; normal mode |
| Super-frame strategy | disabled; 500,000-bit I/P/B thresholds |
| Hierarchical QP | disabled; default deltas -2/-4 |
| H.265 filtering | deblock enabled across slices; luma/chroma SAO enabled |

`ss_mpi_venc_get_chn_capability()` reported payload mask `0x1f`, RC mask
`0x1ff`, GOP mask `0x1c7`, and a 3840x2160 maximum. This means the record lists
H.264, H.265, SVAC3, JPEG and MJPEG; all nine declared RC families; and Normal-P,
Dual-P, Smart-P, Smart-CRR, Single-SP and all-I GOPs. However, the same record
reported only 30 fps while the live channel was delivering 100 fps and its
`enable` field was false. Treat this record as a vendor capability/default
record, not as proof of the effective channel limit. Every newly used setter
still requires device validation and readback.

## SSC338Q and CV610 divergence

| Area | SSC338Q / Star6E | CV610 | Consequence |
|---|---|---|---|
| Waybeam maturity | Resilience, ref cadence, slice geometry, ENHANCE/GDR metadata, ROI, framing, recording and snapshot paths are implemented | Backend deliberately exposes a narrower subset | Port in small independently verifiable stages |
| Encoder API breadth | Simpler MI VENC surface; CBR/VBR/ABR/QP/AVBR families and basic I/P GOP | Nine RC families, multiple P/B/CRR GOP structures, hierarchical QP, native frame-loss and super-frame policies, SVC, QP-map, VUI/SEI and advanced analysis/tuning APIs | CV610 has more future encoder headroom |
| Current RC integration | CBR, VBR, AVBR and a QVBR policy are exposed | H.265 CBR only | Do not mix RC expansion into the resilience port |
| GDR | Implemented and device-confirmed | Row-mode setter/readback and bitstream behavior device-confirmed | Integrated with strict readback |
| Ref prediction | Implemented through base/enhance cadence and TRAIL_N rewriting | Compatible ref-param API and stream `ref_type`, now integrated; dedicated SVC APIs also exist | Keep full native SVC separate |
| Slices | Whole-AU multi-slice implemented; delivered geometry is logged | Whole-AU row splitting confirmed; one unit is a 32-pixel LCU row | Integrated with early output disabled |
| Native loss handling | Waybeam heals reference-chain-breaking output drops with paced IDRs | Additionally offers encoder-side P-skip and super-frame discard/re-encode | Experiment separately after parity |
| VUI/user data | No VUI or SEI control is currently loaded | H.265 VUI is already programmed; user-data insertion is available | CV610 is ahead here |
| Sensor envelope in this project | IMX335 modes reach 2560x1920 and up to 144 fps depending on geometry | IMX662 modes are 1080p30/60/90/95/100; VPSS can scale down | Star6E currently offers more spatial/high-FPS mode variety; CV610's encoder record is not the sensor limit |
| Channels | Vendor header declares 9 VENC channels; project uses main, optional record and snapshot channels | Vendor header declares 16 channels; project uses only channel 0 | CV610 has theoretical multi-channel headroom, not current feature parity |
| Early slice/low delay | SigmaStar streaming modes were rejected in prior device work; production remains frame-based | VENC slice output plus VI/VPSS low-delay APIs exist | CV610 is the better candidate for a future sub-frame-latency experiment |

In short: CV610 has the richer vendor encoder, but SSC338Q has the richer,
device-proven Waybeam product surface today.

## Implementation and hardware results

The implementation derives CV610 intra-refresh, ref cadence and slice controls
in a pure mapping module, applies them between channel creation and start, and
requires exact getter readback. Runtime status exposes requested/effective GDR
settings, while startup and first-frame census logs report slice geometry. The
stream path consumes vendor `ref_type`, marks
droppable enhancement frames, rewrites copied TRAIL_R NALs to TRAIL_N,
publishes GDR cycle metadata, and requests a paced recovery IDR only when a
dropped frame breaks the reference chain.

Confirmed on the IMX662 CV610 bench:

| Test | Result |
|---|---|
| Resilience presets | `off`, `sprint`, `racing`, `endurance`, `patrol`, `quality`, `rescue`, `fpv`, `rally`, `range`, `ltr`, and `ltr:4` applied and read back |
| Sensor rates | GDR + 12 slices passed at 1080p30, 1080p60 and 1080p100 |
| Slice requests | 1, 3, 4, 6, 9, 12 and 17 delivered exactly the requested VCL NAL count |
| Slice geometry | 1080p uses 34 32-pixel LCU rows; request 12 -> split size 3 and 12 slices; request 17 -> split size 2 and 17 slices |
| Ref cadence | `rally` 1/1, `range`/`fpv` 1/4, `ltr` 1/1 and `ltr:4` 1/4 matched requested cadence and prediction mode |
| TRAIL_N rewrite | With 12 slices, 31 non-reference frames produced 372 rewritten NALs for `rally`; the 1/4 presets produced 144 for 12 non-reference frames |
| Frame-SHM metadata | 60 fps run: 454 frames, 450 GDR, 227 ENHANCE, zero metadata/start-code/PTS errors |
| Decoder | IDR-started 1080p60 capture decoded 380/380 frames with FFmpeg `-xerror`, no warnings or errors |
| Ring-pressure recovery | With no consumer, full-ring reference losses produced roughly one recovery IDR per second; droppable enhancement losses did not request recovery |

All slice runs retained one whole access unit per frame-ring slot. CV610's
32-pixel row unit differs from the 64-pixel CTU geometry observed on Star6E;
the shared config contract is the requested count, while each backend reports
its delivered geometry.

## Scope and phases

### Phase 1: pure mappings and configuration surface

1. Add testable CV610 mapping helpers for:
   - resilience preset to `ot_venc_intra_refresh`;
   - preset reference cadence to `ot_venc_ref_param`;
   - requested slice count to `ot_venc_slice_split`.
2. Preserve the shared preset expansion and `video0.sliceCount` validation.
3. Add `resilience: off` and `sliceCount: 1` to the CV610 default config.
4. Remove the blanket CV610 resilience rejection. Continue rejecting a mode
   individually if the device gate below fails.
5. Advertise the two fields in `/api/v1/capabilities` only when their backend
   setup and status callbacks are present.

Verification: focused host tests for all presets, disabled values, boundary
slice counts, signed/clamped inputs and 1080p row geometry; existing config/API
tests must remain green.

### Phase 2: CV610 GDR and whole-AU slices

1. Apply intra-refresh and slice-split after channel creation and before start.
2. Use row refresh for H.265 and read back every applied structure. A mismatch
   fails channel startup rather than claiming resilience that is not active.
3. Keep `slice_output_en = false` and concatenate all packs into one access unit,
   as today.
4. Store effective refresh/slice geometry in the CV610 runtime for status and
   frame metadata.
5. Add CV610 intra/resilience status output.

Device gate on `192.168.2.181`:

- `off`, `sprint`, `racing`, `endurance` and `patrol` at 1080p30/60/100;
- slice requests 1, 3, 4, 6, 9, 12 and 17;
- setter return, getter equality, raw H.265 slice-NAL census, decoded FPS,
  bitrate stability, truncation logs and IDR cadence;
- restoration of the original `/etc/waybeam.json` after the matrix.

The delivered slice count may differ from the request because a row split must
land on the encoder's effective CTU/LCU geometry. Log both requested and
delivered geometry, as the Star6E backend does.

### Phase 3: ref prediction and loss metadata

1. Apply `refBase`, `refEnhance` and `refPred` through
   `ss_mpi_venc_set_ref_param()`.
2. Read `stream.h265_info.ref_type` before releasing each stream.
3. Mark non-reference enhancement frames with the existing frame-SHM ENHANCE
   flag and rewrite copied H.265 TRAIL_R NAL headers to TRAIL_N.
4. Publish existing GDR cycle position/length metadata.
5. Port the one-per-second recovery IDR when an output-ring loss breaks the
   reference chain; do not request recovery for a confirmed droppable ENHANCE
   frame.

Device gate:

- `rally`, `range`, `fpv`, and `ltr:N` preset/readback matrix;
- raw-ES reference-type and TRAIL_N census;
- loss injection showing that ENHANCE loss remains decodable while reference
  loss triggers one paced recovery IDR;
- frame-SHM consumer confirmation of ENHANCE and GDR metadata.

If CV610's base/enhance cadence differs materially from SigmaStar, keep the
affected presets unsupported rather than silently changing their shared names.

### Phase 4: Maruko slice integration

Completed on the SSC378QE/Maruko bench at `192.168.2.233`:

1. Bound the device-aware `MI_VENC_SetH265SliceSplit` and Get ABI through the
   optional Maruko loader.
2. Applied it immediately after `CreateChn`, before input-source setup and the
   stab-fill early return, then verified enabled/nonzero/in-picture readback.
3. Kept `sliceCount=1` compatible with older libraries; explicit multi-slice
   requests fail startup when Set/Get is unavailable or invalid.
4. Confirmed whole-AU delivery through frame-SHM with a per-frame VCL census:
   1 -> 1, 4 -> 4, 12 -> 12, 17 -> 12, and 32 -> 12 at 1280x720. Maruko
   normalizes a requested one-row split to two 32-pixel rows, exposing the
   CTU-64 ceiling directly in getter readback.
5. Confirmed the combination with `rally` GDR/ref-pred metadata and decoded a
   cold-start 284-frame, 12-slice capture cleanly with FFmpeg.

The test also fixed a pre-existing frame-SHM run-loop guard that failed to
recognize `frame_ring` as a valid output transport.

### Phase 5: advanced CV610 experiments

Keep these outside the parity implementation and evaluate separately:

- native frame-loss P-skip strategy;
- super-frame discard/re-encode versus Waybeam's max-I/P-byte contract;
- native QVBR/CVBR/RANGEQP and non-Normal-P GOP modes;
- dedicated SVC v1/v2 and hierarchical QP;
- ROI/QP-map support;
- `slice_output_en=true` plus VI/VPSS low-delay operation.

Each changes rate control, reference structure or transport timing enough to
need its own RF-loss, decoder and latency experiment.

## Files expected to change

- `config/waybeam.default.cv610.json`
- `config/waybeam.default.maruko.json`
- `include/cv610_runtime.h` or a focused new CV610 encoder-control header
- Maruko loader/config/pipeline bindings
- `src/cv610_runtime.c`
- `src/cv610_validation.c`
- `src/venc_api.c`
- focused config/mapping/API tests
- `HISTORY.md`, `VERSION`, and relevant backend/API documentation at release

No `VencConfig` field addition and no frame-SHM layout change are required.
The coordination `protocols/frame-shm.md` producer list should include CV610;
the wire layout itself is unchanged because CV610 emits the already-defined
GDR/ENHANCE fields.

## Completion gates

1. `make lint SOC_BUILD=cv610`
2. CV610 cross-build with the OpenHisilicon headers and shipped firmware libs
3. `make verify` for Star6E/Maruko regressions
4. Focused CV610 device matrix above, marked **confirmed on device**
5. Maruko Set/Get, delivered-VCL and decoder matrix above
6. Coordination `/audit-protocols`, `/check-ports`, `/sync-check`
7. Protocol documentation matches the implemented producer behavior
