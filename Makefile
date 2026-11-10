SHELL := /bin/bash

SOC_BUILD ?= star6e

TOOLCHAIN_URL := https://github.com/openipc/firmware/releases/download/toolchain
TOOLCHAIN_TGZ := toolchain.sigmastar-infinity6e.tgz
TOOLCHAIN_DIR := toolchain/toolchain.sigmastar-infinity6e
CC_BIN := $(TOOLCHAIN_DIR)/bin/arm-openipc-linux-gnueabihf-gcc
TOOLCHAIN_MARUKO_TGZ := toolchain.sigmastar-infinity6c.tgz
TOOLCHAIN_MARUKO_DIR := toolchain/toolchain.sigmastar-infinity6c
CC_MARUKO_BIN := $(TOOLCHAIN_MARUKO_DIR)/bin/arm-openipc-linux-musleabihf-gcc
TOOLCHAIN_CV610_TGZ := toolchain.hisilicon-hi3516cv6xx.tgz
TOOLCHAIN_CV610_DIR := toolchain/toolchain.hisilicon-hi3516cv6xx
CC_CV610_BIN := $(TOOLCHAIN_CV610_DIR)/bin/arm-openipc-linux-musleabi-gcc

# Infinity6C kernel source for building sensor .ko via drivers/Makefile.
# KSRC_MARUKO must point at an existing Infinity6C 5.10.61 kernel source
# tree (arch/arm + top-level Makefile). The tree is not hosted in this
# repo and is not downloaded; obtain it from the appropriate SigmaStar
# SDK and pass it on the command line.
KSRC_MARUKO ?=

# Infinity6E kernel source for building the Star6E sensor .ko via
# drivers/Makefile. KSRC_STAR6E must point at an Infinity6E 4.9.84 kernel
# source tree (arch/arm + top-level Makefile), matching the target device's
# running kernel. Not hosted in this repo; pass it on the command line, e.g.
# the OpenIPC builder output at builder/openipc/output/build/linux-custom.
KSRC_STAR6E ?=

STAR6E_CC ?= $(TOOLCHAIN_DIR)/bin/arm-openipc-linux-gnueabihf-gcc
MARUKO_CC ?= $(TOOLCHAIN_MARUKO_DIR)/bin/arm-openipc-linux-musleabihf-gcc

# Hi3516CV610 uses the public OpenHisilicon V5 headers and OpenIPC's ARMv7
# musl sysroot.  These trees are intentionally external: the vendor SDK
# libraries are not redistributed by this repository.
CV610_CC ?= $(CC_CV610_BIN)
CV610_SDK_INC ?= ../openhisilicon
CV610_SDK_LIB ?= ../firmware/output/target/usr/lib
CV610_CC_SUBMAKE := $(if $(findstring /,$(CV610_CC)),$(abspath $(CV610_CC)),$(CV610_CC))

# Vendor PQ library, needed for isp.sensorBin and /api/v1/iq/export_bin on
# CV610.  It is NOT in this repo: it is a third-party blob whose licensing is
# the vendor's, so `make stage` copies it when the path resolves and says so
# when it does not.  A craft without it boots normally; the import warns and
# no-ops.  Override with CV610_PQ_LIB=/path/to/libbin.so.
CV610_PQ_LIB ?= ../hisilicon/vendor/pq/libbin.so

OUT_DIR := out/$(SOC_BUILD)
OBJ_DIR := $(OUT_DIR)/obj
TARGET := $(OUT_DIR)/waybeam
JSON_CLI_TARGET := $(OUT_DIR)/json_cli
REGSCAN_TARGET := $(OUT_DIR)/regscan
TIMING_PROBE_TARGET := rtp_timing_probe
TIMING_PROBE_SRC := tools/rtp_timing_probe.c
UNIX_CONSUMER_TARGET := $(OUT_DIR)/unix_dgram_consumer
UNIX_CONSUMER_SRC := tools/unix_dgram_consumer.c

VENC_VERSION := $(shell cat VERSION 2>/dev/null || echo unknown)
# -MMD -MP emits per-object .d files so a one-line change rebuilds just
# that object + relink, instead of every source under the sun.  -s is in
# LDFLAGS only (it's a link-time strip flag; not valid during -c).
#
# _FILE_OFFSET_BITS=64 is not optional on the 32-bit targets.  Without it
# glibc leaves off_t 32-bit and open() does not set O_LARGEFILE, so the
# kernel refuses any write past 2^31-1 with EFBIG whatever the filesystem
# allows -- reported on SSC338Q as a recording dying at exactly 2147483647
# bytes on exFAT, which has no such limit of its own, and reproduced here
# against the recorder core on a 58 GB FAT32 card.  It also fixes
# stat()/fstat(), which return
# EOVERFLOW for a >2 GB file and would otherwise hide such a recording from
# the listing and the download path.  musl targets (maruko, cv610) are
# already 64-bit off_t and ignore this; it is here for all of them because
# the flag must be identical across every translation unit -- a partial
# application silently mixes two struct stat/off_t layouts across a link.
COMMON_CFLAGS := -Os -Iinclude -Ilib -DVENC_VERSION=\"$(VENC_VERSION)\" -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -MMD -MP
CONFIG_SRC := src/venc_config.c src/venc_httpd.c src/venc_api.c src/venc_webui.c src/venc_recordings.c src/sensor_select.c src/venc_ring.c src/venc_frame_ring.c lib/cJSON.c
HELPER_SRC := src/backend.c src/file_util.c src/h26x_util.c src/h26x_param_sets.c src/codec_config.c src/pipeline_common.c src/scene_detector.c src/sdk_quiet.c src/rtp_packetizer.c src/hevc_rtp.c src/intra_refresh.c src/frame_gate.c src/isp_runtime.c src/rtp_session.c src/stream_metrics.c src/rtp_sidecar.c src/output_socket.c src/timing.c src/idr_rate_limit.c src/debug_osd.c src/debug_osd_draw.c src/imu_bmi270.c src/audio_codec.c src/venc_jpeg.c src/venc_rec_writer.c src/venc_respawn.c src/mdns_wire.c src/mdns_beacon.c src/device_id.c src/framing_kalman.c src/attitude_est.c src/detect_wire.c
MARUKO_ONLY_SRC := src/maruko_mi.c src/maruko_config.c src/maruko_video.c src/maruko_controls.c src/maruko_output.c src/maruko_pipeline.c src/maruko_runtime.c src/maruko_iq.c src/maruko_cus3a.c src/maruko_ts_recorder.c src/maruko_recorder.c src/maruko_audio.c src/maruko_jpeg.c src/maruko_stabfill_probe.c src/maruko_ipu_yolo.c src/maruko_scl_ports.c
STAR6E_ONLY_SRC := src/star6e_output.c src/star6e_audio.c src/star6e_hevc_rtp.c src/star6e_video.c src/star6e_pipeline.c src/star6e_controls.c src/star6e_runtime.c src/star6e_cus3a.c src/star6e_iq.c src/star6e_jpeg.c src/star6e_ipu.c src/star6e_ipu_yolo.c src/star6e_vpe_ports.c src/star6e_luma_tap.c src/star6e_awb.c
# Shared recorder core: the state machine, the TS mux and the contiguous-AU
# writer are SoC-independent; only the SDK-typed adapters inside
# star6e_recorder.c / star6e_ts_recorder.c are compiled out on other
# backends.  Defined before its first user because CV610_SRC uses := .
RECORDER_SRC := src/star6e_recorder.c src/star6e_ts_recorder.c src/ts_mux.c
CV610_SRC := src/main.c src/backend_cv610.c src/cv610_runtime.c \
	src/cv610_audio.c \
	src/cv610_encoder_config.c \
	src/cv610_iq.c \
	src/cv610_modes.c \
	src/cv610_pq_bin.c \
	src/cv610_pq_bin_load.c \
	src/cv610_jpeg.c \
	src/cv610_pipeline.c src/cv610_validation.c src/backend.c \
	src/venc_config.c src/venc_httpd.c src/venc_api.c src/venc_webui.c \
	src/venc_jpeg.c src/venc_rec_writer.c $(RECORDER_SRC) \
	src/venc_recordings.c src/codec_config.c src/pipeline_common.c \
	src/file_util.c src/idr_rate_limit.c src/timing.c src/intra_refresh.c src/frame_gate.c \
	src/framing_kalman.c src/attitude_est.c src/debug_osd.c \
	src/debug_osd_draw.c lib/cJSON.c \
	src/h26x_util.c src/h26x_param_sets.c src/rtp_packetizer.c \
	src/hevc_rtp.c src/rtp_session.c src/rtp_sidecar.c src/output_socket.c \
	src/venc_frame_ring.c src/mdns_wire.c src/mdns_beacon.c \
	src/device_id.c src/venc_respawn.c
# Image-stabilization framing module (Star6E).  STAB=1 (default) compiles it
# in; STAB=0 drops the source + the -DHAVE_FRAMING_STAB define, so the binary
# carries no stabilization code and framing="stab" validate-rejects to off.
STAB ?= 1
ifeq ($(STAB),1)
STAR6E_ONLY_SRC += src/star6e_framing_stab.c
MARUKO_ONLY_SRC += src/maruko_framing_stab.c
endif
# Phase-1 SCL bench for the Maruko stab port (DEVELOPMENT ONLY).  STAB_BENCH=1
# compiles src/maruko_stab_bench.c + defines HAVE_STAB_BENCH; the default 0
# keeps it out of production images entirely.  Even when compiled in, the bench
# is inert unless WAYBEAM_STAB_BENCH is set in the environment at launch.
STAB_BENCH ?= 0
ifeq ($(STAB_BENCH),1)
MARUKO_ONLY_SRC += src/maruko_stab_bench.c
endif
LIB_RUNPATH ?= /usr/lib
COMMON_LDFLAGS := -s -Wl,-rpath,$(LIB_RUNPATH) -Wl,--no-as-needed

# BASE_LIBS is set per-SOC below — all MI libs are loaded via dlopen at runtime.
ifeq ($(SOC_BUILD),maruko)
CC := $(MARUKO_CC)
SRC := src/main.c src/backend_maruko.c $(MARUKO_ONLY_SRC) $(RECORDER_SRC) $(HELPER_SRC) $(CONFIG_SRC)
# Stock OpenIPC Infinity6C firmware does NOT ship MI vendor libs; bundle them.
# Star6E doesn't need this — Infinity6E firmware has them in /rom/usr/lib/.
DRV := vendor-libs/maruko
DRV_EXTRA :=
SOC_CFLAGS :=
SOC_CFLAGS += -include include/ssc338q_compat.h
SOC_DEFS := -DPLATFORM_STAR6E -DPLATFORM_MARUKO -DHAVE_BACKEND_MARUKO=1
ifeq ($(STAB),1)
SOC_DEFS += -DHAVE_FRAMING_STAB=1
endif
ifeq ($(STAB_BENCH),1)
SOC_DEFS += -DHAVE_STAB_BENCH=1
endif
SOC_LDFLAGS :=
SOC_LIBS := -lm
BASE_LIBS := -Wl,--start-group -lpthread -ldl -lrt -Wl,--end-group
BUILD_TESTS := 0
TOOLCHAIN_TARGET := toolchain-maruko
else ifeq ($(SOC_BUILD),star6e)
CC := $(STAR6E_CC)
SRC := src/main.c src/backend_star6e.c src/star6e_mi.c $(STAR6E_ONLY_SRC) $(RECORDER_SRC) $(HELPER_SRC) $(CONFIG_SRC)
DRV :=
DRV_EXTRA :=
SOC_CFLAGS := -mfpu=neon-vfpv4 -mfloat-abi=hard -ftree-vectorize -include include/ssc338q_compat.h
SOC_DEFS := -DPLATFORM_STAR6E -DHAVE_BACKEND_STAR6E=1
ifeq ($(STAB),1)
SOC_DEFS += -DHAVE_FRAMING_STAB=1
endif
SOC_LDFLAGS :=
SOC_LIBS := -lm
BASE_LIBS := -Wl,--start-group -lpthread -ldl -lrt -Wl,--end-group
BUILD_TESTS := 1
TOOLCHAIN_TARGET := toolchain
else ifeq ($(SOC_BUILD),cv610)
CC := $(CV610_CC)
SRC := $(CV610_SRC)
DRV := $(CV610_SDK_LIB)
DRV_EXTRA :=
SOC_CFLAGS := -I$(CV610_SDK_INC)/kernel/include/hi3516cv6xx \
	-I$(CV610_SDK_INC)/kernel/include/hi3516cv6xx/exp_inc \
	-I$(CV610_SDK_INC)/libraries/isp/include/hi3516cv6xx \
	-I$(CV610_SDK_INC)/libraries/isp/include/hi3516cv6xx/3a \
	-I$(CV610_SDK_INC)/libraries/isp/include/hi3516cv6xx/ext_inc
SOC_DEFS := -DPLATFORM_CV610 -DHAVE_BACKEND_CV610=1 \
	-DHAVE_CV610_HTTP_API=1
SOC_LDFLAGS := -Wl,--allow-shlib-undefined
SOC_LIBS := -lss_mpi -lss_mpi_isp -lss_mpi_ae -lss_mpi_awb \
	-lot_mpi_isp -lss_mpi_sysmem -lss_mpi_sysbind \
	-lss_mpi_audio -lss_mpi_audio_adp \
	-lopus -laac_enc -laac_dec -laac_sbr_enc -laac_sbr_dec \
	-lmp3_enc -lmp3_dec -ldnvqe -lupvqe -lvoice_engine \
	-lacs -lbnr -lcalcflicker -ldehaze -ldrc -lextend_stats \
	-lir_auto -lldci -lsecurec -lot_osal -lm
BASE_LIBS := -Wl,--start-group -lpthread -ldl -lrt -Wl,--end-group
BUILD_TESTS := 0
TOOLCHAIN_TARGET := cv610-sdk
else
$(error Unsupported SOC_BUILD '$(SOC_BUILD)'; expected 'star6e', 'maruko', or 'cv610')
endif

CFLAGS += $(COMMON_CFLAGS) $(SOC_CFLAGS) $(SOC_DEFS)
LDFLAGS += $(COMMON_LDFLAGS) $(SOC_LDFLAGS)

.PHONY: help all build lint stage clean toolchain toolchain-maruko toolchain-cv610 cv610-sdk sensor-cv610 ksrc-maruko \
        drivers-maruko ksrc-star6e drivers-star6e maruko-pull maruko-deploy maruko-full json_cli regscan \
        remote-test verify pre-pr \
        check check-soc-stamp print-config test test-werror test-asan test-tsan test-ci \
        qr-decode unix-dgram-consumer qr-test-host qr-test-cli qr-test-extended qr-test-phone webui webui-check

help:
	@echo "Targets:"
	@echo "  make build       Build standalone binaries (default, SOC_BUILD=star6e)"
	@echo "  make build SOC_BUILD=maruko"
	@echo "  make build SOC_BUILD=cv610 CV610_CC=... CV610_SDK_INC=... CV610_SDK_LIB=..."
	@echo "  make lint        Fast warning check (-Wall -Werror, compile only)"
	@echo "  make qr-decode   Build the freestanding QR decoder for SOC_BUILD"
	@echo "  make unix-dgram-consumer Build the target-local unix:// RTP test consumer"
	@echo "  make qr-test-host Run the deterministic QR perspective corpus"
	@echo "  make qr-test-cli Run decoder CLI and image loader regressions"
	@echo "  make qr-test-extended Run the extended QR skew/defocus series"
	@echo "  make qr-test-phone Validate the phone.html marker generator"
	@echo "  make lint SOC_BUILD=maruko"
	@echo "  make stage       Build and stage runtime bundle in out/"
	@echo "  make test        Run host-native unit tests"
	@echo "  make test-ci     Run all test variants (test + asan + tsan)"
	@echo "  make clean       Clean build outputs"
	@echo "  make toolchain   Ensure Star6E cross-toolchain is present"
	@echo "  make toolchain-maruko Ensure Maruko cross-toolchain is present"
	@echo "  make toolchain-cv610 Ensure CV610 cross-toolchain is present"
	@echo "  make cv610-sdk Validate the external CV610 compiler, headers, and libraries"
	@echo "  make sensor-cv610 Build the CV610 IMX662 userspace sensor plugin"
	@echo "  make ksrc-maruko KSRC_MARUKO=/path/to/kernel  Validate Infinity6C kernel source tree"
	@echo "  make drivers-maruko KSRC_MARUKO=/path/to/kernel  Build sensors/maruko/sensor_imx*_maruko.ko"
	@echo "  make ksrc-star6e KSRC_STAR6E=/path/to/kernel  Validate Infinity6E 4.9.84 kernel source tree"
	@echo "  make drivers-star6e KSRC_STAR6E=/path/to/kernel  Build sensors/star6e/sensor_imx*_star6e.ko"
	@echo "  make json_cli SOC_BUILD=maruko  Build out/<soc>/json_cli (vendored from waybeam-hub)"
	@echo "  make regscan SOC_BUILD=maruko   Build out/<soc>/regscan (IMX335/IMX415 i2c register dumper)"
	@echo "  make maruko-pull HOST=root@<ip>  Pull libs/drivers/isp-bins from a device"
	@echo "  make maruko-deploy HOST=root@<ip>  Build + deploy venc (binary only) cycle"
	@echo "  make maruko-full   HOST=root@<ip>  Full bring-up: binary + libs + drivers + ISP bins"
	@echo "  make remote-test Run bounded remote CLI/test-binary workflow (pass ARGS='...')"
	@echo "  scripts/star6e_direct_deploy.sh cycle  Preferred Star6E venc deploy+HTTP smoke test"
	@echo "  scripts/maruko_direct_deploy.sh cycle  Preferred Maruko venc deploy+HTTP smoke test"
	@echo "  make verify      Build the production SigmaStar backends and run host checks"
	@echo "  make pre-pr      Full pre-PR checklist (version, changelog, build)"
	@echo "  make webui       Regenerate src/venc_webui.c from web/dashboard.html"
	@echo "  make webui-check Fail if src/venc_webui.c is stale vs HTML source"

all: build

build: $(TOOLCHAIN_TARGET) check check-soc-stamp | $(OUT_DIR)
build: $(TARGET)

# Per-source object list — one .o per .c, deps tracked via .d files.
# Maps both src/foo.c and lib/bar.c to $(OBJ_DIR)/src/foo.o, $(OBJ_DIR)/lib/bar.o.
OBJS := $(addprefix $(OBJ_DIR)/,$(SRC:.c=.o))
DEPS := $(OBJS:.o=.d)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

check-soc-stamp:
	@true

check:
	@test -x "$(CC)" || { echo "Compiler missing: $(CC)"; exit 1; }
	@if [ -n "$(DRV)" ]; then \
		test -d "$(DRV)" || { echo "Library dir missing: $(DRV)"; exit 1; }; \
	fi
	@if [ -n "$(DRV_EXTRA)" ]; then \
		test -d "$(DRV_EXTRA)" || { echo "Extra library dir missing: $(DRV_EXTRA)"; exit 1; }; \
	fi

# Syntax-check daemon sources only. The isolated QR helper has its own
# warning-clean compile in qr-decode/verify.
lint: $(TOOLCHAIN_TARGET) check
	$(CC) $(CFLAGS) -Wall -Wextra -Werror -Wno-unused-parameter -Wno-old-style-declaration -fsyntax-only $(SRC)

# Pattern rule — compile any source under src/, lib/, or vendor/ into
# the mirrored layout under $(OBJ_DIR)/.  Header dependencies are
# captured by -MMD -MP in COMMON_CFLAGS; the resulting .d files are
# -include'd below.
#
# VERSION is a prerequisite because COMMON_CFLAGS bakes its contents in as
# -DVENC_VERSION, and no .d file can track that — VERSION is not a header, so
# the compiler never sees it.  Without this a version bump rebuilds nothing and
# the binary keeps reporting the previous release over /api/v1/version and the
# mDNS beacon.  Listing it here rather than on just the two current consumers
# costs a full rebuild once per release and cannot go stale when a third one
# appears.
$(OBJ_DIR)/%.o: %.c VERSION
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# The QR cascade is the one place in the daemon where -Os is the wrong call.
# Decode latency decides whether a scan window lands a code inside the
# operator's patience; the identify/transform stages are float-heavy inner
# loops that -O3 unrolls and vectorizes well on Cortex-A7 + NEON.  Measured on
# the Star6E bench: -O3 is 1.66x faster than -Os (1265 ms vs 2206 ms on a
# 3.6 MB frame) for a couple of tens of KB.  Trailing -O wins over the -Os in
# COMMON_CFLAGS.
$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) $(if $(DRV),-L$(DRV),) $(if $(DRV_EXTRA),-L$(DRV_EXTRA),) $(if $(DRV),-Ltools,) $(BASE_LIBS) $(SOC_LIBS) -o $@

-include $(DEPS)

# Host-native timing probe (no cross-compiler or SDK libs needed)
$(TIMING_PROBE_TARGET): $(TIMING_PROBE_SRC) include/rtp_sidecar.h
	$(HOST_CC) -std=c99 -Wall -Wextra -O2 -D_GNU_SOURCE -Iinclude $(TIMING_PROBE_SRC) -lm -o $@

# Target-local abstract AF_UNIX datagram sink used for unix:// throughput and
# controlled-stall validation. It is a test tool, not part of the runtime image.
unix-dgram-consumer: $(TOOLCHAIN_TARGET) | $(OUT_DIR)
unix-dgram-consumer: $(UNIX_CONSUMER_TARGET)

$(UNIX_CONSUMER_TARGET): $(UNIX_CONSUMER_SRC)
	@mkdir -p $(@D)
	$(CC) -Os -s -Wall -Wextra -Werror -std=c99 -D_GNU_SOURCE -o $@ $<

# json_cli — vendored from waybeam-hub (tools/json_cli/{json_cli.c,jsmn.h}).
# Cross-compiled with the SOC's toolchain so the same binary that runs venc
# can also read/patch /etc/venc.json on the target.
json_cli: $(TOOLCHAIN_TARGET) | $(OUT_DIR)
json_cli: $(JSON_CLI_TARGET)

$(JSON_CLI_TARGET): tools/json_cli/json_cli.c tools/json_cli/jsmn.h
	@mkdir -p $(@D)
	$(CC) -Os -s -Wall -Wextra -std=c11 -D_GNU_SOURCE -Itools/json_cli -o $@ $< -lm

# regscan — Sony IMX335/IMX415 sensor register dumper.  Vendored from
# tipoman9/star6c_sensor; see tools/regscan/NOTICE.  Built with the SOC
# toolchain so it can read /dev/i2c-* on the target alongside venc.
# Used by scripts/maruko_sensor_init_diff.sh for firstboot/majestic/venc
# register-state diffs.
regscan: $(TOOLCHAIN_TARGET) | $(OUT_DIR)
regscan: $(REGSCAN_TARGET)

$(REGSCAN_TARGET): tools/regscan/regscan.c
	@mkdir -p $(@D)
	$(CC) -Os -s -Wall -Wextra -std=c11 -D_GNU_SOURCE -o $@ $<

# ipu_probe — Star6E NPU (IPU) bring-up tool.  DEV-ONLY, cross-compiled for
# the target; not part of the shipped waybeam binary.  See
# specs/2026-07-17-star6e-npu-detect/ (Phase 1).  Run on-device against
# libmi_ipu.so with a firmware + compiled-network blob.
IPU_PROBE_TARGET := $(OUT_DIR)/ipu_probe
IPU_PROBE_SRC    := tools/ipu_probe.c src/star6e_ipu.c src/detect_dequant.c
ipu-probe: $(TOOLCHAIN_TARGET) | $(OUT_DIR)
ipu-probe: $(IPU_PROBE_TARGET)

$(IPU_PROBE_TARGET): $(IPU_PROBE_SRC) include/star6e_ipu.h include/detect_dequant.h
	@mkdir -p $(@D)
	$(CC) -Os -s -Wall -Wextra -std=c99 -D_GNU_SOURCE -Iinclude $(IPU_PROBE_SRC) -ldl -o $@

# qr_decode — isolated QR decoder for the Star6E scan-window worker and for
# scripts which fetch GET /api/v1/snapshot.jpg. It accepts JPEG or P5 PGM;
# command/pairing semantics intentionally live outside this repository.
# quirc (ISC) is vendored under tools/qr/quirc/; stb_image (public domain,
# JPEG-only build) under tools/qr/stb/. Cross-compiled for the target.
QR_DECODE_TARGET := $(OUT_DIR)/qr_decode
QR_DECODE_SRC    := tools/qr/qr_decode.c src/qr_scan.c tools/qr/waybeam_qr_format.c \
                    tools/qr/quirc/quirc.c \
                    tools/qr/quirc/decode.c tools/qr/quirc/identify.c \
                    tools/qr/quirc/version_db.c
# Both supported targets are 32-bit ARM SoCs. quirc supports single-precision
# perspective math specifically for this class of CPU; on Cortex-A7 this also
# avoids the non-NEON double-precision path.
QR_MATH_CFLAGS   := -DQUIRC_FLOAT_TYPE=float -DQUIRC_USE_TGMATH
# Optimize for SPEED, not size. Decode latency is what this tool is judged on:
# a scan either lands inside the watcher's cadence or it does not, and the
# identify/transform stages are float-heavy inner loops that -O3 unrolls and
# vectorizes well on Cortex-A7 + NEON. Measured on the Star6E bench (imx335,
# fixed grayscale captures, best of 3): -O3 was 1.66x faster than -Os
# (1265 ms vs 2206 ms on a 3.6 MB frame) for +24 KB of binary — the wrong
# trade to make for size on a device with megabytes of free overlay. Section
# splitting plus --gc-sections stays: it drops what the bounded backend never
# calls, and costs no speed.
QR_OPT_CFLAGS    := -O3 -ffunction-sections -fdata-sections
QR_GC_LDFLAGS    := -Wl,--gc-sections
qr-decode: $(TOOLCHAIN_TARGET) | $(OUT_DIR)
qr-decode: $(QR_DECODE_TARGET)

$(QR_DECODE_TARGET): $(QR_DECODE_SRC) tools/qr/waybeam_qr_format.h tools/qr/quirc/quirc.h tools/qr/quirc/quirc_internal.h tools/qr/stb/stb_image.h include/qr_scan.h
	@mkdir -p $(@D)
	$(CC) $(QR_OPT_CFLAGS) $(SOC_CFLAGS) $(QR_MATH_CFLAGS) -s -Wall -Wextra -std=c99 -D_GNU_SOURCE -Iinclude -Itools/qr -Itools/qr/quirc -Itools/qr/stb $(QR_DECODE_SRC) $(QR_GC_LDFLAGS) -lm -lrt -o $@

stage: build qr-decode
	@if [ -n "$(DRV)" ] || [ -n "$(DRV_EXTRA)" ]; then mkdir -p $(OUT_DIR)/lib; fi
	@if [ -n "$(DRV)" ]; then cp -f $(DRV)/*.so $(OUT_DIR)/lib/; fi
	@if [ -n "$(DRV_EXTRA)" ]; then cp -f "$(DRV_EXTRA)"/*.so $(OUT_DIR)/lib/; fi
	@# Maruko-only: also stage sensor .ko (renamed _maruko.ko → _mipi.ko for
	@# drop-in install over stock OpenIPC names) and ISP .bin if cached locally.
	@if [ "$(SOC_BUILD)" = "maruko" ]; then \
		if ls sensors/maruko/sensor_*_maruko.ko >/dev/null 2>&1; then \
			mkdir -p $(OUT_DIR)/drivers; \
			for f in sensors/maruko/sensor_*_maruko.ko; do \
				base="$$(basename "$$f" _maruko.ko)"; \
				cp -f "$$f" "$(OUT_DIR)/drivers/$${base}_mipi.ko"; \
			done; \
		fi; \
		if ls iq-profiles/maruko-bin/*.bin >/dev/null 2>&1; then \
			mkdir -p $(OUT_DIR)/isp-bins; cp -f iq-profiles/maruko-bin/*.bin $(OUT_DIR)/isp-bins/; \
		fi; \
	fi
	@set -e; if [ "$(SOC_BUILD)" = "cv610" ]; then \
		$(MAKE) sensor-cv610 SOC_BUILD=cv610 \
			CV610_CC="$(CV610_CC)" CV610_SDK_INC="$(CV610_SDK_INC)"; \
		mkdir -p $(OUT_DIR)/sensors; \
		cp -f sensors/cv610/imx662/libsns_imx662.so $(OUT_DIR)/sensors/; \
		cp -f config/waybeam.default.cv610.json $(OUT_DIR)/waybeam.json; \
		cp -f init.d/S95waybeam.cv610 $(OUT_DIR)/S95waybeam; \
		chmod +x $(OUT_DIR)/S95waybeam; \
		cp -f init.d/load-cv610-online $(OUT_DIR)/load-cv610-online; \
		chmod +x $(OUT_DIR)/load-cv610-online; \
		cp -f config/waybeam-cv610-platform.conf \
			$(OUT_DIR)/waybeam-cv610.conf; \
		if ls iq-profiles/cv610-bin/*.bin >/dev/null 2>&1; then \
			mkdir -p $(OUT_DIR)/isp-bins; cp -f iq-profiles/cv610-bin/*.bin $(OUT_DIR)/isp-bins/; \
		fi; \
		if [ -f "$(CV610_PQ_LIB)" ]; then \
			mkdir -p $(OUT_DIR)/lib; cp -f "$(CV610_PQ_LIB)" $(OUT_DIR)/lib/libbin.so; \
			echo "  stage: libbin.so from $(CV610_PQ_LIB)"; \
		else \
			echo "  stage: NOTE no libbin.so at $(CV610_PQ_LIB) -- isp.sensorBin"; \
			echo "         and /api/v1/iq/export_bin will no-op on the craft."; \
			echo "         Set CV610_PQ_LIB to stage it."; \
		fi; \
	fi

print-config:
	@echo "SOC_BUILD=$(SOC_BUILD)"
	@echo "CC=$(CC)"
	@echo "DRV=$(DRV)"
	@echo "DRV_EXTRA=$(DRV_EXTRA)"
	@echo "BUILD_TESTS=$(BUILD_TESTS)"

# ── Host-native unit tests (x86_64, no cross-compiler needed) ────────

HOST_CC      := cc
# _FILE_OFFSET_BITS=64 mirrors COMMON_CFLAGS: a no-op on x86_64, where off_t
# is already 64-bit, but it keeps the tested ABI the same as the shipped one.
HOST_CFLAGS  := -std=c99 -Wall -Wextra -g -O0 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
                -Iinclude -Ilib -Itests -Itools/qr -Itools/qr/quirc \
                $(QR_MATH_CFLAGS)
TEST_RUNNER  := tests/test_runner
CV610_VALIDATION_TEST := tests/test_cv610_validation
TEST_SRCS    := tests/test_runner.c tests/test_venc_config.c \
                tests/test_venc_api.c tests/test_venc_httpd.c \
                tests/test_sensor_select.c tests/test_venc_ring.c \
                tests/test_file_util.c tests/test_h26x_util.c \
                tests/test_h26x_param_sets.c \
                tests/test_maruko_config.c tests/test_maruko_video.c \
                tests/test_pipeline_common.c \
                tests/test_codec_config.c tests/test_sdk_quiet.c \
                tests/test_rtp_packetizer.c \
                tests/test_hevc_rtp.c \
                tests/test_isp_runtime.c tests/test_rtp_session.c \
                tests/test_stream_metrics.c \
                tests/test_star6e_hevc_rtp.c tests/test_star6e_output.c \
                tests/test_star6e_audio.c tests/test_star6e_video.c \
                tests/test_star6e_recorder.c \
                tests/test_venc_rec_writer.c \
                tests/test_ts_mux.c tests/test_audio_ring.c \
                tests/test_star6e_ts_recorder.c \
                tests/test_idr_rate_limit.c \
                tests/test_backend.c \
                tests/test_debug_osd.c \
                tests/test_intra_refresh.c \
                tests/test_frame_gate.c \
                tests/test_venc_jpeg.c \
                tests/test_mdns_beacon.c \
                tests/test_framing_kalman.c \
                tests/test_attitude_est.c \
                tests/test_framing_stab_accuracy.c \
                tests/test_venc_frame_ring.c \
                tests/test_detect_dequant.c \
                tests/test_detect_wire.c \
                tests/test_star6e_vpe_ports.c \
                tests/test_maruko_scl_ports.c \
                tests/test_cv610_pq_bin_load.c \
                tests/test_qr_scan.c
# Production sources compiled into the test binary (pure-logic modules only).
# sensor_select.c is included here; its MI_SNR_* deps are stubbed in test_sensor_select.c.
TEST_LIB_SRCS := src/qr_scan.c tools/qr/waybeam_qr_format.c \
	tools/qr/quirc/quirc.c tools/qr/quirc/decode.c \
	tools/qr/quirc/identify.c tools/qr/quirc/version_db.c \
	src/backend.c src/venc_config.c src/venc_api.c src/venc_httpd.c src/venc_webui.c src/venc_recordings.c src/sensor_select.c src/venc_ring.c src/venc_frame_ring.c src/file_util.c src/h26x_util.c src/h26x_param_sets.c src/intra_refresh.c src/frame_gate.c src/isp_runtime.c src/maruko_config.c src/maruko_video.c src/maruko_output.c src/codec_config.c src/pipeline_common.c src/cv610_pq_bin_load.c src/rtp_session.c src/sdk_quiet.c src/rtp_packetizer.c src/hevc_rtp.c src/star6e_hevc_rtp.c src/star6e_output.c src/star6e_audio.c src/audio_codec.c src/star6e_video.c src/star6e_recorder.c src/star6e_ts_recorder.c src/ts_mux.c src/venc_rec_writer.c src/rtp_sidecar.c src/stream_metrics.c src/output_socket.c src/timing.c src/idr_rate_limit.c src/debug_osd_draw.c src/venc_jpeg.c src/mdns_wire.c src/mdns_beacon.c src/device_id.c src/framing_kalman.c src/attitude_est.c src/detect_dequant.c src/detect_wire.c src/star6e_vpe_ports.c src/maruko_scl_ports.c lib/cJSON.c

# Every header the test oracles can reach, not a hand-picked list.  They build
# as one compiler invocation each, so there are no .d files to drive this the
# way -MMD does for the cross builds -- and a hand-maintained list silently
# runs a STALE test binary when a header-only change is not on it.  That is a
# false PASS: the suite runs against the previous code and reports green,
# which is strictly worse than a build error because it is indistinguishable
# from success.
#
# Every tree the tests compile from must be here.  0.85.2 replaced a 20-header
# list with include/*.h alone and still left five reachable headers uncovered
# (lib/cJSON.h, tests/qr_marker_render.h and three under tools/qr), so the hole
# it set out to close was still open: appending `#error POISON` to lib/cJSON.h
# and re-running printed 3147 passed.  A full rebuild is ~9 s, far cheaper than
# one undetected stale run, so the globs are deliberately wide.
#
# QR_TEST_RUNNER and QR_HOST_DECODE share this: `verify` depends on
# qr-test-host, so a stale QR oracle is a green verify.
TEST_HEADERS := $(wildcard include/*.h) $(wildcard tests/*.h) \
	$(wildcard lib/*.h) $(wildcard tools/qr/*.h) \
	$(wildcard tools/qr/quirc/*.h) $(wildcard tools/qr/stb/*.h)

$(TEST_RUNNER): $(TEST_SRCS) $(TEST_LIB_SRCS) $(TEST_HEADERS)
	$(HOST_CC) $(HOST_CFLAGS) $(TEST_SRCS) $(TEST_LIB_SRCS) -lpthread -ldl -lm -o $@

$(CV610_VALIDATION_TEST): tests/test_cv610_validation.c src/cv610_validation.c \
		src/cv610_modes.c \
		src/cv610_encoder_config.c src/intra_refresh.c src/frame_gate.c \
		src/venc_config.c src/codec_config.c lib/cJSON.c $(TEST_HEADERS)
	$(HOST_CC) $(HOST_CFLAGS) -Werror $(filter %.c,$^) -lm -o $@

test: $(TEST_RUNNER) $(CV610_VALIDATION_TEST)
	./$(TEST_RUNNER)
	./$(CV610_VALIDATION_TEST)

# Distinct output binaries per variant.  Compiling each over $(TEST_RUNNER)
# left the instrumented binary behind, and a later `make test` saw it
# up-to-date and silently ran it.  `test-werror` was worse: a target-specific
# HOST_CFLAGS with nothing to force a rebuild meant the -Werror compile was
# skipped entirely once the runner existed, so it only ever re-ran the
# non-werror binary.  These always rebuild (no prerequisites).
TEST_RUNNER_WERROR := tests/test_runner.werror
TEST_RUNNER_ASAN   := tests/test_runner.asan
TEST_RUNNER_TSAN   := tests/test_runner.tsan

test-werror:
	$(HOST_CC) $(HOST_CFLAGS) -Werror $(TEST_SRCS) $(TEST_LIB_SRCS) -lpthread -ldl -lm -o $(TEST_RUNNER_WERROR)
	./$(TEST_RUNNER_WERROR)

test-asan:
	$(HOST_CC) $(HOST_CFLAGS) -Werror -fsanitize=address,undefined $(TEST_SRCS) $(TEST_LIB_SRCS) -lpthread -ldl -lm -o $(TEST_RUNNER_ASAN)
	./$(TEST_RUNNER_ASAN)

test-tsan:
	$(HOST_CC) $(HOST_CFLAGS) -Werror -fsanitize=thread $(TEST_SRCS) $(TEST_LIB_SRCS) -lpthread -ldl -lm -o $(TEST_RUNNER_TSAN)
	./$(TEST_RUNNER_TSAN)

test-ci: test test-asan test-tsan

QR_TEST_RUNNER := tests/test_qr_marker
QR_HOST_DECODE := tests/qr_decode_host
QR_TEST_SRCS   := tests/test_qr_marker.c tools/qr/waybeam_qr_format.c \
		  tools/qr/quirc/quirc.c tools/qr/quirc/decode.c \
		  tools/qr/quirc/identify.c tools/qr/quirc/version_db.c

$(QR_TEST_RUNNER): $(QR_TEST_SRCS) $(TEST_HEADERS)
	$(HOST_CC) -std=c99 -Wall -Wextra -g -O0 -D_GNU_SOURCE \
		$(QR_MATH_CFLAGS) -Itools/qr -Itools/qr/quirc \
		$(QR_TEST_SRCS) -lm -o $@

qr-test-host: $(QR_TEST_RUNNER)
	./$(QR_TEST_RUNNER)

$(QR_HOST_DECODE): $(QR_DECODE_SRC) $(TEST_HEADERS)
	$(HOST_CC) $(QR_OPT_CFLAGS) -Wall -Wextra -std=c99 -D_GNU_SOURCE \
		$(QR_MATH_CFLAGS) -Iinclude -Itools/qr -Itools/qr/quirc -Itools/qr/stb $(QR_DECODE_SRC) \
		$(QR_GC_LDFLAGS) -lm -lrt -o $@

qr-test-cli: $(QR_HOST_DECODE)
	sh tests/test_qr_cli.sh "$(CURDIR)/$(QR_HOST_DECODE)"

qr-test-extended: $(QR_TEST_RUNNER)
	./$(QR_TEST_RUNNER) --extended

# Validate the marker encoder embedded in tools/qr/test-images/phone.html.
# Not part of `verify`: it needs node plus the generator's Python deps, which
# a build host is not required to have.  It SKIPS loudly rather than passing
# quietly when they are missing.  PYTHON=/path/to/venv/bin/python if the
# distro python refuses to install qrcode (PEP 668).
PYTHON ?= python3
# One shell: each recipe line gets its own, so an early `exit 0` in a guard
# line would only end that line and make would run the test anyway.
qr-test-phone: $(QR_HOST_DECODE)
	@if ! command -v node >/dev/null 2>&1; then \
		echo "SKIP qr-test-phone: node not installed"; \
	elif ! $(PYTHON) -c "import qrcode" >/dev/null 2>&1; then \
		echo "SKIP qr-test-phone: $(PYTHON) lacks 'qrcode'"; \
		echo "  python3 -m venv .venv && .venv/bin/pip install -r tools/qr/requirements-generator.txt"; \
		echo "  make qr-test-phone PYTHON=.venv/bin/python"; \
	else \
		PYTHON=$(PYTHON) node tests/test_qr_phone.js; \
	fi

toolchain:
	@if [ ! -x "$(CC_BIN)" ]; then \
		echo "Fetching $(TOOLCHAIN_TGZ)..."; \
		wget -c -q --show-progress "$(TOOLCHAIN_URL)/$(TOOLCHAIN_TGZ)" -P "$$(pwd)"; \
		mkdir -p "$(TOOLCHAIN_DIR)"; \
		tar -xf "$(TOOLCHAIN_TGZ)" -C "$(TOOLCHAIN_DIR)" --strip-components=1; \
		rm -f "$(TOOLCHAIN_TGZ)"; \
	fi

toolchain-maruko:
	@if [ ! -x "$(CC_MARUKO_BIN)" ]; then \
		echo "Fetching $(TOOLCHAIN_MARUKO_TGZ)..."; \
		wget -c -q --show-progress "$(TOOLCHAIN_URL)/$(TOOLCHAIN_MARUKO_TGZ)" -P "$$(pwd)"; \
		mkdir -p "$(TOOLCHAIN_MARUKO_DIR)"; \
		tar -xf "$(TOOLCHAIN_MARUKO_TGZ)" -C "$(TOOLCHAIN_MARUKO_DIR)" --strip-components=1; \
		rm -f "$(TOOLCHAIN_MARUKO_TGZ)"; \
	fi

toolchain-cv610:
	@if ! command -v "$(CV610_CC)" >/dev/null 2>&1 && [ ! -x "$(CV610_CC)" ]; then \
		if [ "$(CV610_CC)" != "$(CC_CV610_BIN)" ]; then \
			echo "ERROR: CV610 compiler not found: $(CV610_CC)" >&2; exit 1; \
		fi; \
		echo "Fetching $(TOOLCHAIN_CV610_TGZ)..."; \
		wget -c -q --show-progress "$(TOOLCHAIN_URL)/$(TOOLCHAIN_CV610_TGZ)" -P "$$(pwd)"; \
		mkdir -p "$(TOOLCHAIN_CV610_DIR)"; \
		tar -xf "$(TOOLCHAIN_CV610_TGZ)" -C "$(TOOLCHAIN_CV610_DIR)" --strip-components=1; \
		rm -f "$(TOOLCHAIN_CV610_TGZ)"; \
	fi

cv610-sdk: toolchain-cv610
	@command -v "$(CV610_CC)" >/dev/null 2>&1 || { \
		echo "ERROR: CV610 compiler not found: $(CV610_CC)" >&2; \
		echo "       Pass CV610_CC=/path/to/arm-openipc-linux-musleabi-gcc" >&2; \
		exit 1; \
	}
	@test -f "$(CV610_SDK_INC)/kernel/include/hi3516cv6xx/ss_mpi_venc.h" || { \
		echo "ERROR: CV610 OpenHisilicon headers not found under $(CV610_SDK_INC)" >&2; \
		exit 1; \
	}
	@test -f "$(CV610_SDK_LIB)/libss_mpi.so" || { \
		echo "ERROR: CV610 runtime libraries not found under $(CV610_SDK_LIB)" >&2; \
		exit 1; \
	}

sensor-cv610: cv610-sdk
	$(MAKE) -C sensors/cv610/imx662 \
		CV610_CC="$(CV610_CC_SUBMAKE)" CV610_SDK_INC="$(abspath $(CV610_SDK_INC))"

# ── Maruko drivers ────────────────────────────────────────────────────
#
# Validate that KSRC_MARUKO points at a usable Infinity6C 5.10.61 kernel
# tree. The tree is not redistributable from this repo, so we never
# fetch — pass KSRC_MARUKO=/path/to/kernel on the command line:
#
#   make drivers-maruko KSRC_MARUKO=/path/to/infinity6c-kernel
#
# If you cannot get the kernel source, populate sensors/maruko/ via
# scripts/maruko_pull_artifacts.sh drivers from a known-good device
# instead.
ksrc-maruko:
	@if [ -z "$(KSRC_MARUKO)" ]; then \
		echo "ERROR: KSRC_MARUKO is not set."; \
		echo "       Pass KSRC_MARUKO=/path/to/infinity6c-kernel on the command line."; \
		echo "       The Infinity6C 5.10.61 kernel source is not hosted by this repo."; \
		echo "       Alternative: scripts/maruko_pull_artifacts.sh drivers to pull"; \
		echo "       prebuilt .ko from a working bench device into sensors/maruko/."; \
		exit 1; \
	fi
	@if [ ! -d "$(KSRC_MARUKO)" ] || [ ! -f "$(KSRC_MARUKO)/Makefile" ] || [ ! -d "$(KSRC_MARUKO)/arch/arm" ]; then \
		echo "ERROR: KSRC_MARUKO=$(KSRC_MARUKO) is not a valid kernel source tree"; \
		echo "       (need a directory containing Makefile and arch/arm/)."; \
		exit 1; \
	fi
	@echo "Using Maruko kernel source at $(KSRC_MARUKO)"

drivers-maruko: toolchain-maruko ksrc-maruko
	$(MAKE) -C drivers sensor KSRC="$(KSRC_MARUKO)" \
		CROSS="$(abspath $(TOOLCHAIN_MARUKO_DIR))/bin/arm-openipc-linux-musleabihf-"
	@mkdir -p sensors/maruko
	@cp -f drivers/sensor_imx*_maruko.ko sensors/maruko/ 2>/dev/null || true
	@echo ""
	@echo "Built modules in sensors/maruko/:"
	@ls -lh sensors/maruko/*.ko 2>/dev/null || echo "  (none — check drivers/ build output)"

# ── Star6E drivers ────────────────────────────────────────────────────
#
# Validate that KSRC_STAR6E points at a usable Infinity6E 4.9.84 kernel
# tree.  It must match the target device's running kernel build (vermagic),
# so prefer the exact OpenIPC builder output that produced the device
# firmware, e.g. builder/openipc/output/build/linux-custom:
#
#   make drivers-star6e KSRC_STAR6E=/path/to/infinity6e-kernel
#
ksrc-star6e:
	@if [ -z "$(KSRC_STAR6E)" ]; then \
		echo "ERROR: KSRC_STAR6E is not set."; \
		echo "       Pass KSRC_STAR6E=/path/to/infinity6e-kernel on the command line."; \
		echo "       The Infinity6E 4.9.84 kernel source is not hosted by this repo."; \
		echo "       Use the OpenIPC builder output (linux-custom) that built the"; \
		echo "       device firmware so module vermagic matches the target."; \
		exit 1; \
	fi
	@if [ ! -d "$(KSRC_STAR6E)" ] || [ ! -f "$(KSRC_STAR6E)/Makefile" ] || [ ! -d "$(KSRC_STAR6E)/arch/arm" ]; then \
		echo "ERROR: KSRC_STAR6E=$(KSRC_STAR6E) is not a valid kernel source tree"; \
		echo "       (need a directory containing Makefile and arch/arm/)."; \
		exit 1; \
	fi
	@echo "Using Star6E kernel source at $(KSRC_STAR6E)"

drivers-star6e: toolchain ksrc-star6e
	$(MAKE) -C drivers sensor SOC=star6e KSRC="$(KSRC_STAR6E)" \
		CROSS="$(abspath $(TOOLCHAIN_DIR))/bin/arm-openipc-linux-gnueabihf-"
	@mkdir -p sensors/star6e
	@cp -f drivers/sensor_imx*_star6e.ko sensors/star6e/ 2>/dev/null || true
	@echo ""
	@echo "Built modules in sensors/star6e/:"
	@ls -lh sensors/star6e/*_star6e.ko 2>/dev/null || echo "  (none — check drivers/ build output)"

# ── Maruko deploy convenience wrappers ────────────────────────────────

maruko-pull:
	@HOST="$${HOST:-root@192.168.2.12}"; \
	./scripts/maruko_pull_artifacts.sh --host "$${HOST}"

maruko-deploy:
	@HOST="$${HOST:-root@192.168.2.12}"; \
	./scripts/maruko_direct_deploy.sh --host "$${HOST}" cycle

maruko-full:
	@HOST="$${HOST:-root@192.168.2.12}"; \
	./scripts/maruko_direct_deploy.sh --host "$${HOST}" full

remote-test:
	SOC_BUILD=$(SOC_BUILD) ./scripts/remote_test.sh $(ARGS)

# ── Verification targets ──────────────────────────────────────────────

STAR6E_BINS := out/star6e/waybeam out/star6e/qr_decode out/star6e/unix_dgram_consumer
MARUKO_BINS := out/maruko/waybeam out/maruko/qr_decode out/maruko/unix_dgram_consumer

webui:
	python3 tools/build_webui.py

webui-check:
	python3 tools/build_webui.py --check

verify: webui-check qr-test-host qr-test-cli
	@echo "=== Building Maruko backend ==="
	$(MAKE) build SOC_BUILD=maruko
	$(MAKE) qr-decode SOC_BUILD=maruko
	$(MAKE) unix-dgram-consumer SOC_BUILD=maruko
	@echo ""
	@echo "=== Verifying Maruko binaries ==="
	@for f in $(MARUKO_BINS); do \
		if [ -x "$$f" ]; then echo "  OK  $$f"; \
		else echo "  FAIL  $$f not found or not executable"; exit 1; fi; \
	done
	@echo ""
	@echo "=== Building Star6E backend ==="
	$(MAKE) build SOC_BUILD=star6e
	$(MAKE) qr-decode SOC_BUILD=star6e
	$(MAKE) unix-dgram-consumer SOC_BUILD=star6e
	@echo ""
	@echo "=== Verifying Star6E binaries ==="
	@for f in $(STAR6E_BINS); do \
		if [ -x "$$f" ]; then echo "  OK  $$f"; \
		else echo "  FAIL  $$f not found or not executable"; exit 1; fi; \
	done
	@echo ""
	@echo "=== Verify passed ==="

pre-pr: verify
	@echo ""
	@echo "=== Pre-PR checks ==="
	@if [ ! -f VERSION ]; then echo "  FAIL  VERSION file missing"; exit 1; fi
	@echo "  VERSION: $$(cat VERSION)"
	@if ! grep -q "$$(cat VERSION)" HISTORY.md; then \
		echo "  WARN   VERSION $$(cat VERSION) not found in HISTORY.md"; \
		echo "         Add a changelog entry before opening a PR."; \
	else \
		echo "  OK  HISTORY.md has entry for $$(cat VERSION)"; \
	fi
	@echo ""
	@echo "=== Pre-PR complete ==="

clean:
	rm -rf out/star6e out/maruko out/cv610
	rm -f $(TIMING_PROBE_TARGET)
	rm -f $(TEST_RUNNER)
	rm -f $(TEST_RUNNER_WERROR) $(TEST_RUNNER_ASAN) $(TEST_RUNNER_TSAN)
	rm -f $(CV610_VALIDATION_TEST)
	rm -f $(QR_TEST_RUNNER)
	rm -f $(QR_HOST_DECODE)
	rm -f .build_soc
	$(MAKE) -C sensors/cv610/imx662 clean
