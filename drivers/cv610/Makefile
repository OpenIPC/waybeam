# Kernel-side half of the CV610 sensor chain: the two modules
# init.d/load-cv610-online insmods from /usr/lib/cv610.  See README.md.
#
# Build via the top-level wrapper (it supplies KSRC, SDK and CROSS):
#   make drivers-cv610 KSRC_CV610=<kernel> CV610_SDK_INC=<openhisilicon>
#
# KSRC is the OpenIPC hisilicon-hi3516cv6xx 5.10 kernel, configured and
# modules_prepare'd -- the firmware build's linux-custom, so vermagic matches
# the target.  SDK is an OpenHisilicon checkout at the image's opensdk
# version; its kernel/ tree is copied to build/ and patched there, never in
# place, and the stub builds from a copy there too.

THIS_DIR   := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))
KSRC       ?=
SDK        ?=
CROSS      ?= $(abspath $(THIS_DIR)/../../toolchain/toolchain.hisilicon-hi3516cv6xx/bin/arm-openipc-linux-musleabi-)
BUILD      := $(THIS_DIR)/build
SDK_KERNEL := $(BUILD)/openhisilicon-kernel
KMAKE      := $(MAKE) ARCH=arm CROSS_COMPILE=$(CROSS) -C $(KSRC)
# What OpenIPC's hisilicon-opensdk package passes for this SoC (the DISABLE_*
# switches only act on other families' Kbuild).
SDK_OPTS   := CHIPARCH=hi3516cv6xx DISABLE_IST=1 DISABLE_PM=1 \
              DISABLE_VO=1 DISABLE_TDE=1

modules: check
	rm -rf $(BUILD)
	mkdir -p $(BUILD)
	cp -a $(SDK)/kernel $(SDK_KERNEL)
	patch -d $(SDK_KERNEL) -p2 --forward \
		< $(THIS_DIR)/sys_config-runtime-sensor-clock.patch
	cp -a $(THIS_DIR)/pm_stub $(BUILD)/pm_stub
	$(KMAKE) M=$(SDK_KERNEL) $(SDK_OPTS) modules
	$(KMAKE) M=$(BUILD)/pm_stub \
		KBUILD_EXTRA_SYMBOLS=$(SDK_KERNEL)/Module.symvers modules
	cp -f $(SDK_KERNEL)/open_sys_config.ko $(BUILD)/open_sys_config_imx662.ko
	cp -f $(BUILD)/pm_stub/open_pm_stub.ko $(BUILD)/open_pm_stub.ko
	@echo "Built: $(BUILD)/open_sys_config_imx662.ko $(BUILD)/open_pm_stub.ko"

clean:
	rm -rf $(BUILD)

check:
	@if [ -z "$(KSRC)" ] || [ ! -x "$(KSRC)/scripts/mod/modpost" ] || \
	    ! grep -q '^CONFIG_ARCH_HI3516CV610_FAMILY=y' "$(KSRC)/.config"; then \
		echo "ERROR: KSRC must be a configured, modules_prepare'd" >&2; \
		echo "       hi3516cv6xx kernel tree (KSRC='$(KSRC)')." >&2; \
		exit 1; \
	fi
	@if [ ! -f "$(SDK)/kernel/Kbuild" ] || \
	    [ ! -f "$(SDK)/kernel/hi3516cv6xx.kbuild" ] || \
	    [ ! -f "$(SDK)/kernel/sys_config/hi3516cv6xx/sys_cfg.c" ]; then \
		echo "ERROR: SDK must be a full OpenHisilicon checkout" >&2; \
		echo "       (SDK='$(SDK)')." >&2; \
		exit 1; \
	fi

.PHONY: modules clean check
