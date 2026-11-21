# Documentation Index

This folder is the canonical place for waybeam_venc project documentation.

## Core Docs
- `documentation/SSC338Q_CV610_ENCODER_CAPABILITIES.md`
  - Evidence-tiered comparison of the two encoder APIs and their current
    Waybeam integration, including the richer CV610-only future surface.
- `documentation/CV610_IMX335_2LANE_FPS_TESTS.md`
  - Test plan and results for running the IMX335 two-lane mode above 30 fps on
    CV610: the HMAX floor, what the sensor PLL really scales, the ISP-clock
    ceiling near 47.6 fps, and the 45 fps setting that held from a clean start.
- `documentation/CV610_RESILIENCE_SLICES_PLAN.md`
  - Approved phased plan and device gates for CV610 resilience, ref prediction,
    frame metadata and whole-access-unit H.265 slicing.
- `documentation/DUAL_BACKEND_SPLIT_PLAN.md`
  - Current architecture/implementation plan for Star6E + Maruko targeted builds.
  - Tracks only remaining implementation steps from the current stable baseline.
- `documentation/CONFIG_HTTP_API_ROADMAP.md`
  - JSON config/runtime hardening roadmap (`/etc/venc.json`) and HTTP runtime-control API scope.
  - Contains full settings inventory and mutability classes.
- `documentation/JSON_CONFIG_ROLLBACK_NOTES.md`
  - Historical postmortem notes from the earlier rolled-back JSON runtime attempt.
  - Captures regression signatures and guardrails that still matter for current hardening work.
- `documentation/HTTP_API_CONTRACT.md`
  - Canonical HTTP API contract (endpoints, payloads, status codes, mutability semantics).
  - Must be updated in the same PR as any HTTP API behavior change.
- `documentation/SENSOR_UNLOCK_IMX415_IMX335.md`
  - Reproducible cold-boot unlock sequence for IMX415/IMX335 high-FPS modes.
- `documentation/AE_AWB_CPU_TUNING.md`
  - Implemented AE/AWB/AF and 3DNR runtime knobs for CPU/latency tuning.
  - Includes current defaults and practical usage presets.
- `documentation/LIVE_FPS_CONTROL.md`
  - Live FPS control via hardware bind decimation (MI_SYS_BindChnPort2).
  - Covers clamping behavior, mode selection, tested configurations, and mode switching limitation.
- `documentation/DEBUG_OSD_PLAN.md`
  - Debug OSD overlay API reference. Covers enabling, drawing primitives (text,
    rect, point, line), color constants, and how to add debug output from new modules.
- `documentation/OPTFLOW_FUSION_ROADMAP.md`
  - Optical flow + IMU fusion implementation roadmap.
  - Covers derotation, flight controller integration (MAVLink/MSP), degraded modes,
    performance budget, and phased implementation plan.
- `documentation/REMOTE_TEST_WORKFLOW.md`
  - Bounded remote CLI/test-binary workflow, cold-state rules, and hang handling.
- `documentation/CRASH_LOG.md`
  - Incident log for SoC crashes/hangs. Three-strike rule triggers deeper investigation.
- `documentation/IMPLEMENTATION_PHASES.md`
  - Detailed implementation-phase timeline and checkpoints from the debugging/refactor sessions.
  - Contains historical command references from earlier phases; use current build commands from this index/README.
- `documentation/CURRENT_STATUS_AND_NEXT_STEPS.md`
  - Short operational status and prioritized next actions.
  - Includes latest Maruko hardware smoke-test outcomes.
- `HISTORY.md`
  - Canonical changelog from the current baseline onward.
- `VERSION`
  - Current repository/application version (SemVer).

## SigmaStar SDK API Reference

- [Pudding SDK Documentation](https://wx.comake.online/doc/doc/SigmaStarDocs-Pudding-0120/) — covers SENSOR, VIF, VPE, VENC, SYS, and ISP modules used by this project.

## Scope
- Active implementation: `src/`
- All new implementation notes should be added under `documentation/`.
- Default JSON config template is maintained in:
  - `config/venc.default.json`
- `AGENTS.md` is intentionally minimal and points here for canonical project docs.
