# SSC338Q and CV610 encoder capability delta

This note distinguishes three different meanings of "supported":

1. **Declared** — present in the vendor header or exported runtime ABI.
2. **Accepted** — a getter/setter succeeds on the running device.
3. **Integrated** — Waybeam configures it, reports it honestly, transports the
   result correctly, and has device-level regression coverage.

Confusing these levels is particularly risky on CV610. Its capability record
advertises a broad encoder, but on the 2026-08-22 bench it reported a 30 fps
limit while the same channel was actively delivering 1080p100. Vendor capability
records are useful discovery hints, not substitutes for apply/readback and
bitstream tests.

## Executive summary

CV610 has the richer raw encoder API. SSC338Q currently has the richer and more
mature Waybeam product integration.

CV610 exposes native controls for nine RC families, six useful GOP structures,
hierarchical QP, reference prediction, SVC, frame-loss P-skip, super-frame
discard/re-encode, ROI/QP maps, VUI/SEI, analysis statistics, and early slice
delivery. SSC338Q's MI VENC surface is smaller, but Waybeam already integrates
and device-verifies resilience presets, reference-layer metadata, multi-slice
whole-access-unit output, ROI, recording, snapshots, and dynamic framing there.

## Evidence

The CV610 bench at `192.168.2.181` was inspected and tested on 2026-08-22:

- Waybeam `0.65.10`, H.265 1920x1080 at 100 fps;
- Linux 5.10.221 on armv7l;
- `/usr/lib/libss_mpi.so` SHA-256
  `9ec42150a2901376a91ccd1d3f051dc721b29d1895e7a3190539cb8df4bb4e1a`;
- getter calls succeeded for intra refresh, ref parameters, slice split,
  hierarchical QP, frame-loss strategy, super-frame strategy, H.265 deblock,
  and SAO.

The parity controls were then applied with strict readback. GDR and 12-slice
whole-access-unit output passed at 1080p30/60/100; requests for 1, 3, 4, 6, 9,
12 and 17 slices delivered those exact VCL NAL counts. The `rally`, `range`,
`fpv`, `ltr`, and `ltr:4` reference cadences matched vendor `ref_type` output,
and a 380-frame IDR-started 1080p60 capture decoded cleanly with FFmpeg
`-xerror`. These controls are therefore **Integrated**, not merely Declared.

The same shared slice contract is now also integrated on SSC378QE/Maruko. Its
SigmaStar ABI adds a VENC device argument but uses the same 8-byte enable/row
structure and the same pre-start lifecycle. Hardware readback exposed one
useful divergence: Maruko normalizes a requested one-32-pixel-row split to two
rows (one CTU-64). At 1280x720, requests 1/4/12 delivered 1/4/12 slices, while
17 and 32 saturated at 12. A `rally` run preserved whole-AU frame-SHM metadata
and exact VCL counts; its cold-start capture decoded cleanly.

## Local validation samples

The device never stores encoded captures. The frame-SHM consumer writes to a
FIFO and SSH carries the elementary stream directly into host FFmpeg. These
workspace-local `bench/` artifacts are intentionally gitignored rather than
release assets; the checksums make the evidence identifiable in this session.

| Backend | Host artifact | Evidence |
|---|---|---|
| CV610 | `bench/cv610-rally-slice12-1080p60.mp4` | 380 frames, 6.33 s, 1920x1080@60, SHA-256 `58eca88af6e753ef8a2ff90b2210a02acde2f955384d08eeb1941bc4e26a7759` |
| Maruko | `bench/maruko-rally-slice12-720p30.mp4` | 284 frames, 9.47 s, 1280x720@30, SHA-256 `355710cfc44f59d8944f131a517787d45ccf1aef8dd07cf7b1dac07583806cb8` |

Both samples start at an IDR and decode without FFmpeg warnings or errors.

The channel capability record returned:

| Record | Value | Interpretation |
|---|---:|---|
| Payload mask | `0x1f` | H.264, H.265, SVAC3, JPEG and MJPEG declared |
| RC mask | `0x1ff` | ABR, CBR, VBR, AVBR, QVBR, CVBR, FIXQP, QPMAP and RANGEQP declared |
| GOP mask | `0x1c7` | Normal-P, Dual-P, Smart-P, Smart-CRR, Single-SP and all-I declared |
| Maximum picture | 3840x2160 | Encoder record; the integrated IMX662 path is 1920x1080 |
| Feature enable | false | The returned structure behaves as a default/capability record |

## Detailed delta

| Area | SSC338Q / Star6E | CV610 | Current decision |
|---|---|---|---|
| Payloads | Vendor surface includes H.264/H.265/MJPEG; Waybeam uses H.265 | Header/capability record also includes SVAC3 and JPEG | Keep H.265 parity scope |
| Rate control | CBR, VBR, AVBR and Waybeam's QVBR policy integrated | Nine native RC families declared; only H.265 CBR integrated | RC expansion is separate work |
| GOP structures | Basic I/P GOP used | Normal/Dual/Smart P, CRR, Single-SP and all-I declared; B modes exist in headers but were absent from the device mask | Use Normal-P for parity |
| GDR | Integrated and device-confirmed | Row GDR integrated and confirmed at 1080p30/60/100 with strict readback | Keep column refresh outside the shared contract |
| Ref prediction | Base/enhance cadence, TRAIL_N propagation and ENHANCE metadata integrated | Equivalent ref cadence, TRAIL_N rewrite and metadata now integrated and device-confirmed; fuller SVC API also exported | Leave native SVC gated |
| Slices | Whole-AU slicing and delivered CTU geometry integrated | Whole-AU 32-pixel-row splitting integrated; requests through 17 confirmed; early-slice output also represented | Keep early output off |
| Loss handling | Output-ring chain breaks trigger paced recovery IDRs | Also exposes native P-skip and super-frame discard/re-encode | Preserve parity policy; experiment with native loss controls later |
| QP tools | I/P delta, bounds and frame-size caps integrated | Adds hierarchical QP, QP maps, foreground protection, skip bias and quality balance APIs | Do not bundle unverified tools |
| ROI | Up to four horizontal delta-QP bands integrated (`PIPELINE_ROI_MAX_STEPS`; `fpv.roiSteps` validates to [1,4], though the SDK exposes eight regions) | Eight ROI regions, extended per-frame ROI, background-rate and QP-map APIs | **Delivered in 0.76.0** — the same four bands, same shared geometry, via `ss_mpi_venc_set_roi_attr`. Per-frame ROI, background rate and QP maps remain unexplored |
| VUI/user data | No MI VUI/user-data wrapper loaded | H.265 VUI already programmed; user-data insertion exported | CV610 is already richer here |
| H.265 tools | Encoder defaults plus resilience/slices | Deblock, SAO, transform, entropy, CU prediction, search window, deblur and motion-stat controls exported | Documented future tuning surface |
| Low delay | SigmaStar stream modes were rejected in prior device tests; production is frame-based | Slice-output plus VI/VPSS low-delay APIs exist | Best candidate for a later sub-frame experiment, requiring transport redesign |
| Channels | Header declares 9; project uses main, optional record and snapshot channels | Header declares 16; project uses channel 0 | Theoretical CV610 headroom only |
| Sensor envelope here | IMX335 combinations reach 2560x1920 and up to 144 fps | IMX662 modes are 1080p30/60/90/95/100, with VPSS downscale | Star6E currently offers the broader integrated mode set |

## CV610 controls deliberately deferred

The following are real ABI opportunities, but none should be advertised merely
because a symbol exists:

- native QVBR/CVBR/RANGEQP and alternative GOP structures;
- P-skip frame-loss strategy and super-frame discard/re-encode;
- SVC v1/v2, hierarchical QP and per-frame SVC masks;
- ROI/QP-map, foreground protection, skip bias and quality balance;
- advanced deblur, motion analysis, CU/search-window tuning;
- early slice delivery with VI/VPSS low-delay mode.

Each requires setter/readback verification plus bitrate, decoder, reference-chain,
RF-loss and latency measurements. Early slice delivery additionally changes the
current one-`GetStream`-result-per-access-unit assumption in RTP and frame-SHM.

## Related implementation plan

See `documentation/CV610_RESILIENCE_SLICES_PLAN.md` for the phased parity port,
device matrix, rollback rules and completion gates.
