// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Airoha failsafe image validation (Web recovery / httpd upload).
 *
 * Standalone validation module used by board/airoha/common/failsafe.c.
 * Every uploaded firmware image is structurally checked before it is
 * flashed.  The generic storage capacity checks (MTD partition size /
 * 'fip' static volume size) are done by the board write path and are
 * always performed, independently of this module's master switch.
 *
 * The whole module is controlled by the master switch
 * CONFIG_AIROHA_FAILSAFE_VALIDATE ("Failsafe image validation" menu in
 * board/airoha/Kconfig).  Per-image-type checks can be enabled or
 * disabled individually with:
 *   u-boot            CONFIG_AIROHA_FAILSAFE_VALIDATE_UBOOT
 *   bl2               CONFIG_AIROHA_FAILSAFE_VALIDATE_BL2
 *   fip               CONFIG_AIROHA_FAILSAFE_VALIDATE_FIP
 *   chainloader       CONFIG_AIROHA_FAILSAFE_VALIDATE_CHAINLOADER
 *   firmware          CONFIG_AIROHA_FAILSAFE_VALIDATE_FIRMWARE
 *
 * The layout decides which checks apply:
 *   - legacy layout: 'u-boot' carries the whole boot chain (BL1 + BL2 +
 *     BL31 + U-Boot) inside one image, so its validator (FIP ToC @0x800)
 *     inherently covers BL2 too; there is no standalone 'bl2' check.
 *     Without BL1 (CONFIG_AIROHA_LEGACY_BL1 disabled, e.g. AN7563) the
 *     same self-contained FIP sits behind a 2 KiB zero prefix - the legacy
 *     image without BL1.  The prefix is only there to keep the FIP at the
 *     same offset as in the image with BL1, so both flavours are validated
 *     as a FIP ToC @0x800.
 *   - FIP mode: BL2 lives in its own partition / image (preloader.bin),
 *     so CONFIG_AIROHA_FAILSAFE_VALIDATE_BL2 always includes the deep
 *     LZMA-layout + trailing CRC32 check for bare preloaders.
 *   - MIPS (EcoNet): no FIP is involved.  The 'u-boot' storage holds the
 *     whole bootloader image ([TCBoot loader][ECNT descriptor][u-boot.bin
 *     payload]), from which the shared helper extracts the U-Boot, checks
 *     that it is really there and compares its device tree with this
 *     board.
 *
 * Both the legacy 'u-boot' image and the 'fip' volume ship a U-Boot, so
 * each of them is also checked against this board: the 'compatible' of
 * the device tree that U-Boot carries must match (env
 * 'failsafe_strict_model', always on unless set to "0").
 *
 * The board independent FIP ToC walk, FIT and legacy uImage checks live
 * in failsafe/bootimg/ and are shared with the MediaTek board code; this
 * file keeps only the Airoha specific policy.
 */

#include <errno.h>
#include <stdio.h>
#include <linux/kconfig.h>
#include <linux/string.h>
#include <asm/unaligned.h>
#include <u-boot/crc.h>
#include <failsafe/fw_type.h>
#include <failsafe/fip.h>
#include <failsafe/image.h>
#include <cprint.h>
#include <failsafe/error.h>

#include "failsafe_validate.h"

/*
 * This object is only built with the master switch on (see
 * board/airoha/common/Makefile).  Guard the contents so a direct object
 * build with the switch off stays consistent with the header stub.
 */
#if IS_ENABLED(CONFIG_AIROHA_FAILSAFE_VALIDATE)

/* 'u-boot' layout: the internal FIP ToC is at 0x800 in both variants -
 * with BL1 (BL1 @0x0, FIP @0x800, env @0x7c000) and without BL1 (2 KiB
 * zero prefix, FIP @0x800).  See tools/airoha_pack_boot.sh. */
#define AIROHA_FAILSAFE_LEGACY_FIP_OFF		0x800

/* 'bl2' (preloader) LZMA layout (AN7563/AN7581/AN7583):
 *
 *   [stage-1 BL21 @0x0] [opt header @0x3800] [BL22 LZMA @0x3820]
 *   [BL23 LZMA] [flash-table LZMA] [CRC32 @ size-4]
 *
 * The optimization header (little-endian) holds the BL22/BL23/flash-table
 * compressed sizes in fields 0-2; all segments are back-to-back.  The
 * trailing CRC32 is the raw accumulator without the final one's
 * complement ("no final XOR" format).  Struct-layout boards (EN7523
 * family) embed the flash table at build time and have no opt header or
 * trailing CRC, so they are skipped automatically.
 */
#define AIROHA_FAILSAFE_BL2_MIN_SIZE		0x4000
#define AIROHA_FAILSAFE_BL2_OPT_HDR_OFF		0x3800
#define AIROHA_FAILSAFE_BL2_LZMA_DEF_OFF	0x3820

/* ------------------------------------------------------------------ */
/*  Per-type image validators                                          */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_BL2)
/*
 * Detect the AN7xxx LZMA BL2 layout from the optimization header: the
 * three compressed sizes (BL22, BL23, flash table) must be back-to-back
 * and end exactly at size - 4 (the trailing CRC32 slot).  On success the
 * stored CRC is returned in *crc.  Returns false for struct-layout BL2
 * images (EN7523 family), which have no opt header / trailing CRC.
 */
static bool bl2_lzma_layout_ok(const u8 *data, size_t size, u32 *crc)
{
	const u8 *h = data + AIROHA_FAILSAFE_BL2_OPT_HDR_OFF;
	u32 bl22, bl23, ft;
	u64 end;

	if (size < AIROHA_FAILSAFE_BL2_OPT_HDR_OFF + 0x24 + 4)
		return false;

	bl22 = get_unaligned_le32(h + 0);
	bl23 = get_unaligned_le32(h + 4);
	ft   = get_unaligned_le32(h + 8);

	/* Plausible size ranges (mirrors tools/airoha_info_preloader.py) */
	if (bl22 <= 0x1000 || bl22 >= 0x200000 ||
	    bl23 <= 0x1000 || bl23 >= 0x200000 ||
	    ft <= 0x100 || ft >= 0x20000)
		return false;

	end = (u64)AIROHA_FAILSAFE_BL2_LZMA_DEF_OFF + bl22 + bl23 + ft;
	if (end != size - 4)
		return false;

	*crc = get_unaligned_le32(data + size - 4);
	return true;
}
#endif /* CONFIG_AIROHA_FAILSAFE_VALIDATE_BL2 */

#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_UBOOT)
static int failsafe_validate_uboot(const void *data, size_t size)
{
#if defined(CONFIG_AIROHA_BUILD_LEGACY)
	int ret;

	/* ARM: the 'u-boot' MTD partition holds either the legacy image
	 * with BL1 (BL1 @0x0 + internal FIP @0x800) or, when BL1 is left
	 * out (CONFIG_AIROHA_LEGACY_BL1 disabled, e.g. AN7563), the same
	 * internal FIP (BL2 + BL31 + U-Boot) behind a 2 KiB zero prefix
	 * instead of BL1.  tools/airoha_pack_boot.sh writes the FIP at
	 * 0x800 in both cases, so both are validated as a FIP ToC @0x800.
	 * Both carry the whole boot chain in one image, so this check
	 * inherently covers BL2 too.  Size is bounded by the generic
	 * MTD partition capacity check in failsafe_validate_image().
	 */
	ret = failsafe_fip_validate(data, size,
				    AIROHA_FAILSAFE_LEGACY_FIP_OFF,
				    NULL, "u-boot");
	if (ret)
		return ret;

	/* The image ships the whole boot chain, so the U-Boot inside it
	 * must be built for this board (env 'failsafe_strict_model'). */
	return failsafe_fip_check_model(data, size,
					AIROHA_FAILSAFE_LEGACY_FIP_OFF,
					"u-boot");
#else
	/* MIPS (EcoNet): no FIP.  The 'u-boot' storage holds the whole
	 * bootloader image - [TCBoot loader][ECNT descriptor][u-boot.bin
	 * payload] - so the shared helper looks for the U-Boot inside it,
	 * requires it to be there, and compares its device tree with this
	 * board (env 'failsafe_strict_model').  Size is bounded by the
	 * generic MTD partition capacity check in failsafe_validate_image().
	 */
	return failsafe_image_validate_uboot(data, size, "u-boot");
#endif
}
#endif /* CONFIG_AIROHA_FAILSAFE_VALIDATE_UBOOT */

#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_BL2)
static int failsafe_validate_bl2(const void *data, size_t size)
{
	u32 magic;

	if (size < AIROHA_FAILSAFE_BL2_MIN_SIZE) {
		return failsafe_error(-EINVAL, "'bl2' image too small "
			"(%zu < 0x%zx)", size,
			(size_t)AIROHA_FAILSAFE_BL2_MIN_SIZE);
	}

	/* preloader.bin: FIP container wrapping the raw preloader
	 * ($(FIPTOOL) create --tb-fw bl2.bin preloader.bin).  Validate the
	 * ToC and that the BL2 payload lies inside the image.
	 */
	if (failsafe_fip_check(data, size, 0))
		return failsafe_fip_validate(data, size, 0,
					     failsafe_fip_uuid_tb_fw, "bl2");

	magic = get_unaligned_le32(data);

	/* A corrupted preloader.bin with a bad header magic still carries
	 * the BL2 ToC entry at offset 16; a genuine bare preloader would
	 * never match the 128-bit UUID there by chance. */
	if (size >= FAILSAFE_FIP_HEADER_SIZE +
		   FAILSAFE_FIP_TOC_ENTRY_SIZE &&
	    !memcmp((const u8 *)data + FAILSAFE_FIP_HEADER_SIZE,
		    failsafe_fip_uuid_tb_fw, 16)) {
		return failsafe_error(-EINVAL,
			"'bl2' image looks like a FIP with "
			"a bad header (BL2 ToC entry present, magic 0x%08x)",
			magic);
	}

	{
		/* Bare preloader (legacy bootext images): LZMA layout
		 * deep check (AN7563/AN7581/AN7583).  Always performed
		 * when BL2 validation is enabled (FIP mode). */
		const u8 *s1 = data;
		u32 stored, calc;
		size_t i, nz = 0;

		if (bl2_lzma_layout_ok(data, size, &stored)) {
			/* stage-1 (BL21) region must not be blank. */
			for (i = 0; i < AIROHA_FAILSAFE_BL2_OPT_HDR_OFF; i++)
				if (s1[i] != 0x00 && s1[i] != 0xFF)
					nz++;
			if (nz < AIROHA_FAILSAFE_BL2_OPT_HDR_OFF / 8) {
				return failsafe_error(-EINVAL, "'bl2' stage-1 "
					"(BL21) region blank");
			}

			/* Airoha stores the raw CRC accumulator without the
			 * final one's complement that crc32() adds, so XOR
			 * it back out for comparison. */
			calc = crc32(0, data, size - 4) ^ 0xFFFFFFFF;
			if (calc != stored) {
				return failsafe_error(-EINVAL, "'bl2' CRC32 "
					"mismatch (stored 0x%08x, calc "
					"0x%08x)", stored, calc);
			}
		}
		/* Struct layout (EN7523 family): no opt header / trailing
		 * CRC; the size checks above are the only gate. */
	}

	return 0;
}
#endif /* CONFIG_AIROHA_FAILSAFE_VALIDATE_BL2 */

#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_FIP)
static int failsafe_validate_fip(const void *data, size_t size)
{
	int ret;

	/* 'fip' = UBI volume with a FIP (BL31+U-Boot, or the legacy
	 * BL2+BL31+U-Boot single-FIP).  Any non-empty ToC is accepted.
	 */
	ret = failsafe_fip_validate(data, size, 0, NULL, "fip");
	if (ret)
		return ret;

	/* The U-Boot the FIP ships must be built for this board (env
	 * 'failsafe_strict_model'). */
	return failsafe_fip_check_model(data, size, 0, "fip");
}
#endif /* CONFIG_AIROHA_FAILSAFE_VALIDATE_FIP */

#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_CHAINLOADER)
static int failsafe_validate_chainloader(const void *data, size_t size)
{
	/* The chainloader partition may hold either a legacy uImage
	 * (shim-based packing) or an OpenWrt-style FIT;
	 * dispatch on the header magic. */
	if (failsafe_image_is_legacy(data, size))
		return failsafe_image_validate_legacy(data, size, "chainloader");

	return failsafe_image_validate_fit(data, size, "chainloader");
}
#endif /* CONFIG_AIROHA_FAILSAFE_VALIDATE_CHAINLOADER */

#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_FIRMWARE)
static int failsafe_validate_firmware(const void *data, size_t size)
{
	/*
	 * A single boot image (FIT) is checked structurally plus by the
	 * opt-in strict board-model gate (env 'failsafe_strict_model').
	 * On boards with an MMC system image (CONFIG_MMC) an OpenWrt
	 * sysupgrade TAR - the split kernel + rootfs image - is accepted as
	 * well; the shared helper picks the right check (see
	 * failsafe_image_validate_firmware()).
	 */
	return failsafe_image_validate_firmware(data, size, "firmware");
}
#endif /* CONFIG_AIROHA_FAILSAFE_VALIDATE_FIRMWARE */

/* ------------------------------------------------------------------ */
/*  Public entry point                                                */
/* ------------------------------------------------------------------ */

int failsafe_validate_image_content(const void *data, size_t size,
				    failsafe_fw_t fw)
{
	const char *name = failsafe_fw_type_name(fw);
	bool checked = false;
	int ret = 0;

	cprintln(NORMAL, "Failsafe: validating '%s' image (%zu bytes)",
		 name, size);

	/* Per-type structural validation.  Each validator is gated by its
	 * own Kconfig toggle (board/airoha/Kconfig, "Failsafe image
	 * validation").  The generic storage capacity checks are done by
	 * failsafe_validate_image() in core.c, always enabled.
	 */
	switch (fw) {
	case FW_TYPE_UBOOT:
#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_UBOOT)
		checked = true;
		ret = failsafe_validate_uboot(data, size);
#endif
		break;
	case FW_TYPE_BL2:
#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_BL2)
		checked = true;
		ret = failsafe_validate_bl2(data, size);
#endif
		break;
	case FW_TYPE_FIP:
#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_FIP)
		checked = true;
		ret = failsafe_validate_fip(data, size);
#endif
		break;
	case FW_TYPE_CHAINLOADER:
#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_CHAINLOADER)
		checked = true;
		ret = failsafe_validate_chainloader(data, size);
#endif
		break;
	case FW_TYPE_FW:
#if defined(CONFIG_AIROHA_FAILSAFE_VALIDATE_FIRMWARE)
		checked = true;
		ret = failsafe_validate_firmware(data, size);
#endif
		break;
	default:
		break;
	}

	if (ret)
		return failsafe_error(ret, "'%s' image validation FAILED",
			name);

	if (!checked)
		cprintln(CAUTION, "Failsafe: '%s' image validation skipped "
			 "(no check enabled)", name);
	else
		cprintln(SUCCESS, "Failsafe: '%s' image validation OK", name);

	return 0;
}

#endif /* IS_ENABLED(CONFIG_AIROHA_FAILSAFE_VALIDATE) */
