// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Mediatek failsafe image validation (Web recovery / httpd upload).
 *
 * The whole module is controlled by the master switch
 * CONFIG_MTK_FAILSAFE_VALIDATE ("Failsafe image validation" menu in
 * board/mediatek/Kconfig).  Per-image-type checks can be enabled or
 * disabled individually with:
 *   bl2               CONFIG_MTK_FAILSAFE_VALIDATE_BL2
 *   fip               CONFIG_MTK_FAILSAFE_VALIDATE_FIP
 *   u-boot (MIPS)     CONFIG_MTK_FAILSAFE_VALIDATE_UBOOT
 *   firmware          CONFIG_MTK_FAILSAFE_VALIDATE_FIRMWARE
 *
 * The layout decides which checks apply:
 *   - FIP mode: BL2 lives in its own partition / image (preloader.bin),
 *     so CONFIG_MTK_FAILSAFE_VALIDATE_BL2 always includes the raw
 *     preloader header check (SF_BOOT / SPINAND! / NANDCFG! /
 *     EMMC_BOOT / SDMMC_BOOT) and, when the preloader is delivered as a
 *     FIP container ($(FIPTOOL) create --tb-fw bl2.bin preloader.bin),
 *     a FIP ToC check for the BL2 payload.
 *   - U-Boot layout on the MIPS SoCs (ARCH_MTMIPS): no FIP is involved,
 *     the 'u-boot' partition holds the whole bootloader image (SPL +
 *     U-Boot as a legacy uImage, or a TCBoot loader with the U-Boot
 *     payload behind it), which is validated by the shared
 *     failsafe_image_validate_uboot().
 *   - 'fip' is a UBI static volume holding a FIP (BL31 + U-Boot, or the
 *     legacy BL2+BL31+U-Boot single-FIP), validated as a FIP ToC.  The
 *     U-Boot it ships is additionally checked against this board - the
 *     'compatible' of the device tree U-Boot carries must match (env
 *     'failsafe_strict_model', always on unless set to "0").
 *
 * The board independent FIP ToC walk and FIT check live in
 * failsafe/bootimg/ and are shared with the Airoha board code; this file
 * keeps only the MediaTek specific preloader detection.
 */

#include <errno.h>
#include <stdio.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <asm/unaligned.h>
#include <failsafe/fw_type.h>
#include <failsafe/fip.h>
#include <failsafe/image.h>
#include <cprint.h>
#include <failsafe/error.h>

#include "failsafe_validate.h"

/*
 * This object is only built with the master switch on (see
 * board/mediatek/common/Makefile).  Guard the contents so a direct object
 * build with the switch off stays consistent with the header stub.
 */
#if IS_ENABLED(CONFIG_MTK_FAILSAFE_VALIDATE)

/* ------------------------------------------------------------------ */
/*  BL2 (preloader) header detection                                   */
/* ------------------------------------------------------------------ */

/* The raw Mediatek preloader (boot ROM image) starts with one of these
 * magic headers at offset 0 (mirrors the reference bl2_helper.h).  Only
 * the first 8 bytes are compared, as the tail varies across preloader
 * versions. */
#if defined(CONFIG_MTK_FAILSAFE_VALIDATE_BL2)
#define MTK_BL2_HDR_SIZE		8

static const u8 mtk_bl2_hdr_sf_nor[8] = {
	0x53, 0x46, 0x5f, 0x42, 0x4f, 0x4f, 0x54, 0x00,	/* "SF_BOOT\0" */
};
static const u8 mtk_bl2_hdr_spi_nand[8] = {
	0x53, 0x50, 0x49, 0x4e, 0x41, 0x4e, 0x44, 0x21,	/* "SPINAND!" */
};
static const u8 mtk_bl2_hdr_snfi_nand[8] = {
	0x4e, 0x41, 0x4e, 0x44, 0x43, 0x46, 0x47, 0x21,	/* "NANDCFG!" */
};
static const u8 mtk_bl2_hdr_emmc[8] = {
	0x45, 0x4d, 0x4d, 0x43, 0x5f, 0x42, 0x4f, 0x4f,	/* "EMMC_BOOT" */
};
static const u8 mtk_bl2_hdr_sd[8] = {
	0x53, 0x44, 0x4d, 0x4d, 0x43, 0x5f, 0x42, 0x4f,	/* "SDMMC_BOOT" */
};

/*
 * Return the name of the storage type a raw preloader is built for, or
 * NULL if 'data' does not carry any known Mediatek preloader magic.
 */
static const char *mtk_bl2_storage_name(const void *data, size_t size)
{
	static const u8 *const magics[] = {
		mtk_bl2_hdr_sf_nor, mtk_bl2_hdr_spi_nand,
		mtk_bl2_hdr_snfi_nand, mtk_bl2_hdr_emmc, mtk_bl2_hdr_sd,
	};
	static const char *const names[] = {
		"spim-nor", "spim-nand", "snfi-nand", "emmc", "sd",
	};
	int i;

	if (size < MTK_BL2_HDR_SIZE)
		return NULL;

	for (i = 0; i < ARRAY_SIZE(magics); i++)
		if (!memcmp(data, magics[i], MTK_BL2_HDR_SIZE))
			return names[i];

	return NULL;
}
#endif /* CONFIG_MTK_FAILSAFE_VALIDATE_BL2 */

/* ------------------------------------------------------------------ */
/*  Per-type image validators                                          */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_MTK_FAILSAFE_VALIDATE_BL2)
static int failsafe_validate_bl2(const void *data, size_t size)
{
	u32 magic;

	if (size < FAILSAFE_FIP_HEADER_SIZE) {
		return failsafe_error(-EINVAL, "'bl2' image too small (%zu)",
			size);
	}

	/* preloader.bin delivered as a FIP container wrapping the raw
	 * preloader ($(FIPTOOL) create --tb-fw bl2.bin preloader.bin).
	 * Validate the ToC and that the BL2 payload lies inside the image. */
	if (failsafe_fip_check(data, size, 0))
		return failsafe_fip_validate(data, size, 0,
					     failsafe_fip_uuid_tb_fw, "bl2");

	magic = get_unaligned_le32(data);

	/* A corrupted preloader.bin with a bad header magic still carries
	 * the BL2 ToC entry at offset 16; a genuine raw preloader would
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

	/* Bare preloader: must carry one of the known Mediatek storage
	 * magic headers. */
	if (!mtk_bl2_storage_name(data, size)) {
		return failsafe_error(-EINVAL,
			"'bl2' image has no known preloader "
			"header (magic 0x%08x)", magic);
	}

	return 0;
}
#endif /* CONFIG_MTK_FAILSAFE_VALIDATE_BL2 */

#if defined(CONFIG_MTK_FAILSAFE_VALIDATE_UBOOT)
static int failsafe_validate_uboot(const void *data, size_t size)
{
	/* The MIPS 'u-boot' storage holds the whole bootloader image: SPL
	 * + U-Boot packed as a legacy uImage (u-boot-with-spl.bin,
	 * u-boot-lzma.img, u-boot-mt7621.bin), or a TCBoot loader with
	 * the U-Boot payload behind it.  The shared helper checks that it
	 * really carries a U-Boot and matches the board the model of
	 * (env 'failsafe_strict_model'); the image size is bounded by the
	 * generic partition capacity check in failsafe_validate_image().
	 */
	return failsafe_image_validate_uboot(data, size, "u-boot");
}
#endif /* CONFIG_MTK_FAILSAFE_VALIDATE_UBOOT */

#if defined(CONFIG_MTK_FAILSAFE_VALIDATE_FIP)
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
#endif /* CONFIG_MTK_FAILSAFE_VALIDATE_FIP */

#if defined(CONFIG_MTK_FAILSAFE_VALIDATE_FIRMWARE)
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
#endif /* CONFIG_MTK_FAILSAFE_VALIDATE_FIRMWARE */

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
	 * own Kconfig toggle (board/mediatek/Kconfig, "Failsafe image
	 * validation").  The generic storage capacity checks are done by
	 * failsafe_validate_image() in core.c, always enabled.
	 */
	switch (fw) {
	case FW_TYPE_BL2:
#if defined(CONFIG_MTK_FAILSAFE_VALIDATE_BL2)
		checked = true;
		ret = failsafe_validate_bl2(data, size);
#endif
		break;
	case FW_TYPE_UBOOT:
#if defined(CONFIG_MTK_FAILSAFE_VALIDATE_UBOOT)
		checked = true;
		ret = failsafe_validate_uboot(data, size);
#endif
		break;
	case FW_TYPE_FIP:
#if defined(CONFIG_MTK_FAILSAFE_VALIDATE_FIP)
		checked = true;
		ret = failsafe_validate_fip(data, size);
#endif
		break;
	case FW_TYPE_FW:
#if defined(CONFIG_MTK_FAILSAFE_VALIDATE_FIRMWARE)
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

#endif /* IS_ENABLED(CONFIG_MTK_FAILSAFE_VALIDATE) */
