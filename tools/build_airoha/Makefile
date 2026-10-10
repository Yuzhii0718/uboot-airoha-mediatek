# SPDX-License-Identifier: GPL-2.0+
#
# Airoha FIP image builder
#
# This Makefile bundles the BL2/BL31 binaries (from per-SoC subdirectories of
# this directory: source/, opensource/ or legacy/) together with the just-built
# u-boot.bin into FIP (Firmware Image Package) images using fiptool.
#
# Kconfig-driven behaviour:
#   CONFIG_AIROHA_BUILD_MODERN    — split FIP build (AN7581/AN7583)
#   CONFIG_AIROHA_BUILD_LEGACY    — self-contained 512 KiB boot image (all
#                                   Airoha platforms; see airoha_pack_boot.sh)
#   CONFIG_AIROHA_LEGACY_BL1      — prepend BL1 (bl1-bl2-bl31-uboot.bin) or
#                                   not (bl2-bl31-uboot.bin); default y on
#                                   EN7523
#   CONFIG_AIROHA_BLOBS_SOURCE / _OPENSOURCE / _LEGACY — blob source directory
#                                   (source/, opensource/ or legacy/ under this
#                                   directory; defaults to source for the
#                                   modern build - airoha.sh stages the ATF
#                                   artifacts there - and to legacy for the
#                                   legacy one)
#   CONFIG_AIROHA_BUILD_CHAINLOADER     — chainloader build (AN7581)
#   CONFIG_AIROHA_CHAINLOADER_METHOD_*  — build method: OPENWRT (default,
#                                         mkits.sh style) or SHIM (legacy
#                                         airoha_pack_chainloader.sh)
#   CONFIG_AIROHA_CHAINLOADER_DIR       — chainloader board selection
#
# Every BL2/BL31 blob is structure-checked just before it is packed (see the
# _validate_bl2 / _validate_bl31 targets): a corrupted or truncated blob aborts
# the build instead of producing a broken FIP image.
#
# Output files (placed in U-Boot top-level directory), named to match the
# OpenWrt artifacts:
#   bl31-uboot.fip   — main FIP image (modern/split layout only: BL31+U-Boot)
#   preloader.bin    — BL2 FIP image (CONFIG_AIROHA_PRELOADER; built for both
#                      layouts)
#   bl1-preloader.bin — 2 KiB BL1 prefix (real BL1 or zeros) + BL2 FIP,
#                      for EN7523 (CONFIG_AIROHA_PRELOADER_BL1)
#   bl1-bl2-bl31-uboot.bin — 512 KiB boot image with BL1: BL1 + internal
#                      FIP @0x800 (LEGACY + AIROHA_LEGACY_BL1)
#   bl2-bl31-uboot.bin — the same 512 KiB boot image without BL1: 2 KiB zero
#                      prefix + internal FIP @0x800 (LEGACY without BL1)
# Both padded images report their real footprint (size + % of the 512 KiB
# container) via tools/airoha_pack_boot.sh.
#   <board>-chainload-uboot.itb — OpenWrt-style chainload FIT (OPENWRT method)
#   <board>-chainloader.itb      — chainloader FIT image (SHIM method)
#   <board>-chainloader-slot.bin — chainloader slot image (SHIM method)

-include ../../include/config/auto.conf
-include ../../include/autoconf.mk

HOSTCC      ?= cc
SOC          := $(subst ",,$(CONFIG_SYS_SOC))

# Prebuilt blobs live under tools/build_airoha/{legacy,opensource}/<variant>_default/.
# The variant defaults to the SoC (e.g. en7523 -> legacy/en7523_default) and can
# be overridden per board via CONFIG_AIROHA_FIP_BLOBS_DIR (e.g. set it to
# "en7562_fiberhome_sr1041f_default").  Strip quotes first so "" (empty string
# in Kconfig) is treated as unset.
_FIP_BLOBS_DIR_RAW := $(subst ",,$(CONFIG_AIROHA_FIP_BLOBS_DIR))
ifneq ($(_FIP_BLOBS_DIR_RAW),)
  SOC_VARIANT := $(_FIP_BLOBS_DIR_RAW)
else
  SOC_VARIANT := $(SOC)_default
endif

# Pick the blob directory:
#   source/<variant>     - built from the ATF source tree by airoha.sh
#                          (CONFIG_AIROHA_BLOBS_SOURCE, modern default),
#   opensource/<variant> - checked-in open-source blobs (deprecated),
#   legacy/<variant>     - vendor SDK blobs (legacy default).
ifeq ($(CONFIG_AIROHA_BLOBS_SOURCE),y)
  BLOBS_BASE := source
else ifeq ($(CONFIG_AIROHA_BLOBS_OPENSOURCE),y)
  BLOBS_BASE := opensource
else
  BLOBS_BASE := legacy
endif
BLOBS_DIR := $(BLOBS_BASE)/$(SOC_VARIANT)
FIPTOOL      := ../fiptool
UBOOT_BIN    := ../../u-boot.bin
OUT_DIR      := ../..
PACK_SH      := ../airoha_pack_boot.sh

BL1_BIN      := $(BLOBS_DIR)/bl1.bin
BL2_BIN      := $(BLOBS_DIR)/bl2.bin
BL31_LZMA    := $(BLOBS_DIR)/bl31.lzma
CERT_BIN     := $(BLOBS_DIR)/certificates.bin
KEY_AREA_BIN := $(BLOBS_DIR)/key_area.bin

# The 'source' layout is produced by airoha.sh from the ATF source tree and is
# deliberately not checked in.  Without these stubs make would abort with a bare
# "No rule to make target 'source/<variant>/bl2.bin'"; this turns that into the
# command that fixes it.  When the files do exist the stubs are inert (a target
# without prerequisites whose file exists is already up to date).
ifeq ($(BLOBS_BASE),source)
$(BL2_BIN) $(BL31_LZMA):
	@echo "ERROR: $(BLOBS_DIR) has no $@:"
	@echo "       the 'source' blob layout is built from the ATF source tree by airoha.sh."
	@echo "       Run:  SOC=<soc> BOARD=<board> ./airoha.sh            # STAGE=all"
	@echo "             SOC=<soc> BOARD=<board> STAGE=atf ./airoha.sh  # blobs only"
	@echo "       Or build against the checked-in blobs: re-run with BLOBS=opensource,"
	@echo "       or select CONFIG_AIROHA_BLOBS_OPENSOURCE / _LEGACY in the defconfig."
	@exit 1
endif

BL2_FIP      := $(OUT_DIR)/preloader.bin
BL1_PRELOADER := $(OUT_DIR)/bl1-preloader.bin
FIP_IMAGE    := $(OUT_DIR)/bl31-uboot.fip
LEGACY_IMAGE := $(OUT_DIR)/bl2-bl31-uboot.bin
_LEGACY_FIP  := $(OUT_DIR)/_legacy.fip

BOOTEXT_PY    := ../airoha_pack_bootext.py
BOOTEXT_IMAGE := $(OUT_DIR)/bootext.ram
CERT_EXTRACT_PY := ../airoha_extract_certificates.py
BOOTEXT_CERT_CNT := $(subst ",,$(CONFIG_AIROHA_BOOTEXT_CERT_COUNT))

BUILD_LEGACY := $(CONFIG_AIROHA_BUILD_LEGACY)
LEGACY_BL1   := $(CONFIG_AIROHA_LEGACY_BL1)

# ---- LZMA tool detection ------------------------------------------------
# One entry point for every LZMA encoder: tools/lzma_wrapper.sh uses the system
# LZMA SDK encoder when the host has one (that is what the released images were
# built with - Debian and Ubuntu ship it in the 'lzma' package as /usr/bin/lzmp,
# registered as the /usr/bin/lzma alternative with priority 30, ahead of xz's
# 20), then the in-tree SDK 4.32 encoder built from tools/lzma432, then xz.  It
# also leaves the real uncompressed size in the LZMA-Alone header, which is what
# BL2 requires ("Failed to decompress image (err=1)" otherwise).  The plain
# detection after it is the fallback for a tree without the wrapper.
LZMA_WRAPPER ?= ../lzma_wrapper.sh
LZMA_E := $(shell \
	if [ -x "$(LZMA_WRAPPER)" ]; then echo "$(LZMA_WRAPPER) -c"; \
	elif command -v lzma >/dev/null 2>&1; then echo "lzma -c"; \
	elif command -v xz >/dev/null 2>&1; then echo "xz --format=lzma --stdout"; fi \
)

# In-tree legacy LZMA SDK encoder (LZMA SDK 4.32, sources in tools/lzma432), the
# fallback for a host without an LZMA SDK of its own: building it here means no
# build machine needs the 'lzma' package, and it works on every architecture
# (the 4.32 encoder produces the same "size + no end marker" stream the vendor
# blobs use, which xz cannot).
LZMA_SDK_BIN := lzma-sdk
LZMA_SDK_DIR := ../lzma432/7zip/Compress/LZMA_Alone
LZMA_VERIFY ?= ../airoha_verify_lzma.py

# ---- BL31 / U-Boot path setup -------------------------------------------
# BL31 is always pre-compressed (bl31.lzma); u-boot.bin is always
# LZMA-compressed to u-boot.bin.lzma before packing.
UBOOT_OUT := $(UBOOT_BIN).lzma
UBOOT_TC_ARG := --uboot-lzma $(UBOOT_OUT)

# ---------------------------------------------------------------------------
# ATF metadata printout tools
# ---------------------------------------------------------------------------
_PRELOADER_INFO_PY := ../airoha_info_preloader.py
_BL31_INFO_PY      := ../airoha_info_bl31.py

# ---------------------------------------------------------------------------
# ATF blob validation (BL2 / BL31 build gate)
# ---------------------------------------------------------------------------
# BL2 and BL31 are prebuilt blobs: a truncated or corrupted image would
# otherwise silently end up inside the FIP.  Both targets run the standalone
# structure checker (--validate) of the matching info tool and hard-fail the
# build when a check does not pass.
#
# They are attached to every rule that consumes a blob as *order-only*
# prerequisites, so the check runs on each build but does not by itself force
# the artifact to be re-packed.
.PHONY: _validate_bl2 _validate_bl31

_validate_bl2: $(BL2_BIN)
	@echo "  [CHECK]  Validating BL2 image ($(BL2_BIN))..."
	@python3 "$(_PRELOADER_INFO_PY)" "$(BL2_BIN)" --validate || \
		{ echo "ERROR: BL2 image failed validation: $(BL2_BIN)"; \
		  echo "       the blob is corrupt/truncated - refusing to build"; \
		  exit 1; }

_validate_bl31: $(BL31_LZMA)
	@echo "  [CHECK]  Validating BL31 image ($(BL31_LZMA))..."
	@python3 "$(_BL31_INFO_PY)" "$(BL31_LZMA)" --validate || \
		{ echo "ERROR: BL31 image failed validation: $(BL31_LZMA)"; \
		  echo "       the blob is corrupt/truncated - refusing to build"; \
		  exit 1; }

.PHONY: _print_bl_info
_print_bl_info: $(BL2_BIN)
	@if [ -f "$(_PRELOADER_INFO_PY)" ] && [ -f "$(BL2_BIN)" ]; then \
		echo ""; \
		python3 "$(_PRELOADER_INFO_PY)" "$(BL2_BIN)" --summary 2>/dev/null; \
	fi
	@if [ -f "$(_BL31_INFO_PY)" ] && [ -f "$(BL31_LZMA)" ]; then \
		echo ""; \
		python3 "$(_BL31_INFO_PY)" "$(BL31_LZMA)" --summary 2>/dev/null; \
	fi
	@echo ""

# ---------------------------------------------------------------------------
# Build rules
# ---------------------------------------------------------------------------

.DEFAULT_GOAL := all

# Build fiptool from sources
$(FIPTOOL):
	@echo "  [FIP]    Building fiptool (host compiler)..."
	$(MAKE) -C ../fiptool_src CC=$(HOSTCC) ARCH_AIROHA=$(CONFIG_ARCH_AIROHA)

# ----- in-tree LZMA SDK encoder -------------------------------------------
# The LZMA SDK 4.32 sources are from 2005 and trip a pile of harmless warnings
# on a modern g++ (-Wmisleading-indentation, -Wformat-security, ...), hence -w.
# Building it is best effort on purpose: a host that already has the 'lzma'
# package never uses it, and a host without a C++ compiler must still build -
# the wrapper then falls back to the system encoder or xz.
# 'make lzma-sdk' builds it on demand; the SDK's own makefile is used in place
# and cleaned again afterwards, so only this single binary is left behind.
$(LZMA_SDK_BIN):
	@echo "  [LZMA]   Building the in-tree LZMA SDK encoder (SDK 4.32)..."
	@if $(MAKE) -C $(LZMA_SDK_DIR) -f makefile.gcc \
			CXX="g++ -O2 -w" CXX_C="gcc -O2 -w" >/dev/null 2>&1; then \
		cp -f $(LZMA_SDK_DIR)/lzma $@; \
		$(MAKE) -C $(LZMA_SDK_DIR) -f makefile.gcc clean >/dev/null 2>&1 || true; \
	else \
		echo "  [LZMA]   WARNING: cannot build $@ (no C++ compiler?) - the"; \
		echo "  [LZMA]            wrapper will use the system 'lzma' or xz"; \
	fi

# ----- LZMA compression targets -------------------------------------------
# u-boot.bin is built by the main U-Boot Makefile, not by us.
#
# Bytes 5..12 of the LZMA-Alone header declare the uncompressed size and the
# BL2 (preloader) decompressor requires the *real* value: xz-family encoders
# write -1 ("unknown size") there and BL2 then aborts the boot with
# "Failed to decompress image (err=1)" / "Stored BL31 + U-Boot FIP failed".
# tools/lzma_wrapper.sh already guarantees it for every encoder, and
# $(LZMA_VERIFY) is the independent last word: it repairs a header that still
# carries something else (bare-tool fallback, a foreign image, ...), then
# decodes the payload with BL2's semantics and compares it byte for byte with
# u-boot.bin - a stream that fails that check never reaches the FIP.
$(UBOOT_BIN).lzma: $(LZMA_SDK_BIN)
	@test -f $(UBOOT_BIN) || { echo "ERROR: $(UBOOT_BIN) not found (build u-boot.bin first)"; exit 1; }
	@if [ -x "$(LZMA_WRAPPER)" ]; then $(LZMA_WRAPPER) --version 2>&1 | sed 's/^/  [LZMA]   /'; fi
	@echo "  [LZMA]   Compressing u-boot.bin -> u-boot.bin.lzma..."
	$(LZMA_E) $(UBOOT_BIN) > $@
	@python3 $(LZMA_VERIFY) --repair --expect $(UBOOT_BIN) $@

# ----- preloader.bin (standalone BL2 FIP) ------------------------------------
# Built for both boot image layouts (modern/split and legacy/512 KiB) and
# controlled by the single CONFIG_AIROHA_PRELOADER switch:
#   - legacy (with or without BL1): --align 1024, so BL2 starts at 0xC00,
#     exactly where it sits inside the 512 KiB container (2 KiB prefix +
#     FIP @0x800),
#   - modern/split: plain container, BL2 lives in the 'bl2' MTD partition.
ifeq ($(CONFIG_AIROHA_PRELOADER),y)
all: $(FIPTOOL)
all: $(BL2_FIP)

# The 512 KiB legacy layout places the internal FIP at 0x800 and its first
# payload at 0xC00, so the standalone BL2 FIP uses the same 1024-byte
# alignment there (both with and without BL1).
ifeq ($(BUILD_LEGACY),y)
PRELOADER_ALIGN     := --align 1024
PRELOADER_ALIGN_MSG := , 1024-byte aligned
endif

$(BL2_FIP): $(BL2_BIN) $(FIPTOOL) | _validate_bl2
	@echo "  [FIP]    Creating preloader.bin (BL2-only$(PRELOADER_ALIGN_MSG))..."
	$(FIPTOOL) create $(strip $(PRELOADER_ALIGN)) --tb-fw $(BL2_BIN) $@

# ----- bl1-preloader.bin (2 KiB BL1 prefix + BL2 FIP) ------------------------
# EN7523 boots from a flat image: the 2 KiB BL1 region (padded/truncated to
# 0x800, exactly like tools/airoha_pack_boot.sh does for the 512 KiB image)
# followed by the BL2 FIP.
ifeq ($(CONFIG_AIROHA_BUILD_MODERN),y)
ifeq ($(CONFIG_AIROHA_PRELOADER_BL1),y)
all: $(BL1_PRELOADER)

$(BL1_PRELOADER): $(BL2_FIP)
	@echo "  [FIP]    Creating bl1-preloader.bin (2 KiB BL1 prefix + BL2 FIP)..."
	@if [ ! -f "$(BL1_BIN)" ]; then \
		echo "ERROR: $(BL1_BIN) not found, bl1-preloader build failed"; \
		exit 1; \
	fi
	cat "$(BL1_BIN)" > $@
	truncate -s 2048 $@
	cat $(BL2_FIP) >> $@
endif
endif
endif

ifeq ($(CONFIG_AIROHA_BUILD_MODERN),y)

# ----- bl31-uboot.fip (split layout: BL31 + U-Boot) ---------------------------
all: $(FIP_IMAGE)
$(FIP_IMAGE): $(BL31_LZMA) $(UBOOT_OUT) $(FIPTOOL) _print_bl_info | _validate_bl31
	@echo "  [FIP]    Creating bl31-uboot.fip (split layout: BL31+U-Boot)..."
	$(FIPTOOL) create --soc-fw $(BL31_LZMA) --nt-fw $(UBOOT_OUT) $@

endif  # CONFIG_AIROHA_BUILD_MODERN

# ----- shared internal FIP (BL2 + LZMA BL31 + LZMA U-Boot, 1024-aligned) -------
# The two 512 KiB flavours differ only in the 2 KiB prefix (real BL1 header vs
# zero prefix); the internal FIP is identical.  tools/airoha_pack_boot.sh adds
# the prefix and pads the image to 512 KiB.
$(_LEGACY_FIP): $(BL2_BIN) $(BL31_LZMA) $(UBOOT_OUT) $(FIPTOOL) _print_bl_info | _validate_bl2 _validate_bl31
	@echo "  [FIP]    Creating internal shared FIP (BL2+BL31+U-Boot, 1024-byte aligned)..."
	$(FIPTOOL) create --align 1024 \
		--tb-fw $(BL2_BIN) \
		--soc-fw $(BL31_LZMA) \
		--nt-fw $(UBOOT_OUT) $@

# ----- 512 KiB boot image (airoha_pack_boot.sh: prefix + shared FIP) -------
# Both flavours use the very same internal FIP; only the 2 KiB prefix differs:
#   AIROHA_LEGACY_BL1=y -> BL1 + FIP          -> bl1-bl2-bl31-uboot.bin
#   AIROHA_LEGACY_BL1=n -> 2 KiB zeros + FIP  -> bl2-bl31-uboot.bin
# (the latter is used by AN7563).
ifeq ($(BUILD_LEGACY),y)

ifeq ($(LEGACY_BL1),y)
LEGACY_IMAGE := $(OUT_DIR)/bl1-bl2-bl31-uboot.bin
all: $(LEGACY_IMAGE)
$(LEGACY_IMAGE): $(BL1_BIN) $(_LEGACY_FIP) $(PACK_SH)
	@echo "  [LEGACY] Packing bl1-bl2-bl31-uboot.bin (BL1 + FIP, 512 KiB)..."
	bash $(PACK_SH) --bl1 $(BL1_BIN) --fip $(_LEGACY_FIP) -o $@
else
LEGACY_IMAGE := $(OUT_DIR)/bl2-bl31-uboot.bin
all: $(LEGACY_IMAGE)
$(LEGACY_IMAGE): $(_LEGACY_FIP) $(PACK_SH)
	@echo "  [LEGACY] Packing bl2-bl31-uboot.bin (2 KiB zero prefix + FIP, 512 KiB)..."
	bash $(PACK_SH) --fip $(_LEGACY_FIP) -o $@
endif

endif  # BUILD_LEGACY

# ----- bootext.ram (airoha_pack_bootext.py) -------------------------------
ifeq ($(CONFIG_AIROHA_BOOTEXT_RAM),y)
all: $(BOOTEXT_IMAGE)

ifeq ($(CONFIG_AIROHA_BOOTEXT_EMBED_CERTS),y)
_BOOTEXT_CERT_TMP := $(BLOBS_DIR)/.bootext_cert_tmp

$(_BOOTEXT_CERT_TMP): $(CERT_BIN)
	@mkdir -p $@
	@echo "  [CERT]   Extracting $(BOOTEXT_CERT_CNT) TF-A certificate(s) from certificates.bin..."
	python3 $(CERT_EXTRACT_PY) $(CERT_BIN) -o $@ --expected $(BOOTEXT_CERT_CNT)

$(BOOTEXT_IMAGE): $(BL2_BIN) $(_BOOTEXT_CERT_TMP) | _validate_bl2
	@echo "  [BOOTEXT] Packing bootext.ram (BL2 + $(BOOTEXT_CERT_CNT) embedded TF-A cert(s), no CRC)..."
	@if [ "$(BOOTEXT_CERT_CNT)" = "1" ]; then \
		python3 $(BOOTEXT_PY) pack $(BL2_BIN) \
			--cert $(_BOOTEXT_CERT_TMP)/cert_01.der --no-crc -o $@; \
	else \
		CERTS=""; \
		for f in $$(ls $(_BOOTEXT_CERT_TMP)/*.der 2>/dev/null); do \
			CERTS="$$CERTS --cert $$f"; \
		done; \
		python3 $(BOOTEXT_PY) pack $(BL2_BIN) $$CERTS --no-crc -o $@; \
	fi
	@rm -rf $(_BOOTEXT_CERT_TMP)
else
$(BOOTEXT_IMAGE): $(BL2_BIN) | _validate_bl2
	@echo "  [BOOTEXT] Packing bootext.ram (bare BL2, no embedded certificates)..."
	python3 $(BOOTEXT_PY) pack $(BL2_BIN) -o $@
endif
endif

# ----- chainloader ----------------------------------------------------------
# Two build methods (selectable via Kconfig):
#   AIROHA_CHAINLOADER_METHOD_OPENWRT (default) — OpenWrt-style chainload FIT:
#     FIT with u-boot.bin.lzma (or u-boot.bin) + u-boot.dtb, generated via
#     tools/airoha_pack_chainloader_openwrt.sh (mkits.sh + mkimage).
#     Output: <board>-chainload-uboot.itb.
#   AIROHA_CHAINLOADER_METHOD_SHIM — legacy shim packer
#     (tools/airoha_pack_chainloader_shim.sh): prefix shim + control DTB + FIT.
#     Outputs: <board>-chainloader-slot.bin + <board>-chainloader.itb.

ifeq ($(CONFIG_AIROHA_BUILD_CHAINLOADER),y)
CHAINLOADER_SOC := $(SOC)

ifeq ($(CONFIG_AIROHA_CHAINLOADER_METHOD_SHIM),y)
# Shim method: requires the per-board sources dir, selected via
# CONFIG_AIROHA_CHAINLOADER_DIR (same pattern as CONFIG_AIROHA_FIP_BLOBS_DIR).
# Strip quotes first so "" (empty string in Kconfig) is treated as unset.
_CHAINLOADER_DIR_RAW := $(subst ",,$(CONFIG_AIROHA_CHAINLOADER_DIR))
ifneq ($(_CHAINLOADER_DIR_RAW),)
CHAINLOADER_SRC_DIR := chainloader/$(_CHAINLOADER_DIR_RAW)
CHAINLOADER_BOARD   := $(lastword $(subst _, ,$(_CHAINLOADER_DIR_RAW)))
else
$(error AIROHA_CHAINLOADER_METHOD_SHIM is enabled but CONFIG_AIROHA_CHAINLOADER_DIR \
	is not set; set it to the per-board chainloader directory, \
	e.g. CONFIG_AIROHA_CHAINLOADER_DIR="<soc>_<board>")
endif

CHAINLOADER_FIT  := $(OUT_DIR)/$(CHAINLOADER_BOARD)-chainloader.itb
CHAINLOADER_SLOT := $(OUT_DIR)/$(CHAINLOADER_BOARD)-chainloader-slot.bin

CHAINLOADER_SCRIPT := ../airoha_pack_chainloader_shim.sh
all: $(CHAINLOADER_SLOT)

$(CHAINLOADER_SLOT): $(UBOOT_BIN) $(CHAINLOADER_SCRIPT) \
		$(CHAINLOADER_SRC_DIR)/shim.bin \
		$(CHAINLOADER_SRC_DIR)/control.dts \
		$(CHAINLOADER_SRC_DIR)/chainloader.its
	@echo "  [CHAINLOADER] Packing $(CHAINLOADER_BOARD) chainloader (shim method: prefix shim + control DTB + FIT)..."
	bash $(CHAINLOADER_SCRIPT) \
		--soc $(CHAINLOADER_SOC) --board $(CHAINLOADER_BOARD) \
		--payload $(UBOOT_BIN) --output-dir $(OUT_DIR) \
		--mkimage ../../tools/mkimage --dtc ../../scripts/dtc/dtc

$(CHAINLOADER_FIT): $(CHAINLOADER_SLOT)

else
# OpenWrt-style chainload FIT (default method,
# tools/airoha_pack_chainloader_openwrt.sh):
#   kernel = u-boot.bin.lzma (COMP=lzma, fallback u-boot.bin COMP=none)
#   fdt    = u-boot.dtb
#   load/entry 0x80200000, fdt load 0x82000000  (mirrors OpenWrt
#   Build/an7581-chainloader: mkits.sh + mkimage, no shim prefix).
# No shim sources directory is needed: the board name comes from
# CONFIG_AIROHA_CHAINLOADER_BOARD.
_CHAINLOADER_BOARD_RAW := $(subst ",,$(CONFIG_AIROHA_CHAINLOADER_BOARD))
ifneq ($(_CHAINLOADER_BOARD_RAW),)
CHAINLOADER_BOARD := $(_CHAINLOADER_BOARD_RAW)
else
$(error AIROHA_CHAINLOADER_METHOD_OPENWRT is enabled but \
	CONFIG_AIROHA_CHAINLOADER_BOARD is not set; set it to the board name.")
endif

CHAINLOADER_ITB    := $(OUT_DIR)/$(CHAINLOADER_BOARD)-chainload-uboot.itb
CHAINLOADER_SCRIPT := ../airoha_pack_chainloader_openwrt.sh
MKITS_SH           := ../mkits.sh
MKIMAGE_BIN        := ../../tools/mkimage
UBOOT_DTB          := $(OUT_DIR)/u-boot.dtb

all: $(CHAINLOADER_ITB)

$(CHAINLOADER_ITB): $(UBOOT_BIN) $(UBOOT_OUT) $(UBOOT_DTB) $(CHAINLOADER_SCRIPT) $(MKITS_SH) $(MKIMAGE_BIN)
	@echo "  [CHAINLOADER] Packing $(CHAINLOADER_BOARD) chainloader (OpenWrt method: mkits.sh + mkimage)..."
	bash $(CHAINLOADER_SCRIPT) \
		--soc $(CHAINLOADER_SOC) --board $(CHAINLOADER_BOARD) \
		--payload $(UBOOT_BIN) --payload-lzma $(UBOOT_OUT) \
		--dtb $(UBOOT_DTB) --output-dir $(OUT_DIR) \
		--mkimage ../../tools/mkimage --mkits ../mkits.sh

endif  # OPENWRT vs SHIM method
endif  # CONFIG_AIROHA_BUILD_CHAINLOADER

# ---------------------------------------------------------------------------
clean:
	rm -f $(BL2_FIP) $(BL1_PRELOADER) $(FIP_IMAGE) $(LEGACY_IMAGE) $(_LEGACY_FIP) $(BOOTEXT_IMAGE) $(UBOOT_BIN).lzma \
		$(OUT_DIR)/bl1-bl2-bl31-uboot.bin \
		$(OUT_DIR)/bl2-bl31-uboot.bin \
		$(OUT_DIR)/*-chainload-uboot.itb \
		$(OUT_DIR)/*-chainloader.itb \
		$(OUT_DIR)/*-chainloader-slot.bin \
		$(OUT_DIR)/*-chainloader-prefix-shim.uImage \
		$(OUT_DIR)/*-chainloader-control.dtb
	rm -rf $(OUT_DIR)/.chainload-fit-*

.PHONY: all clean
