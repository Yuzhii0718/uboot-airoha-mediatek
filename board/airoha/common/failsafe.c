// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Airoha failsafe board hooks (Web recovery / httpd upload).
 *
 * Covers the __weak hooks required by failsafe/core.c and used
 * by failsafe/modules/upgrade.c:
 *   - httpd_get_upload_buffer_ptr()  — return a safe DRAM staging buffer
 *   - failsafe_validate_image()      — basic size check plus the generic
 *                                      storage capacity checks, then the
 *                                      structural image validation
 *   - failsafe_write_image()         — flash the staged image to the
 *                                      target partition (MTD) or UBI volume
 *   - boot_from_mem()                — boot an uploaded image from DRAM
 *   - failsafe_bl2_version_info()    — extract the preloader banner
 *   - failsafe_atf_version_info()    — read the flashed BL2 / BL31 banners
 *
 * Only the Airoha specific policy lives here:
 *   - the uploaded DRAM staging buffer and the RAM-boot fallback address,
 *   - the firmware-type → storage-target mapping and the BL2 write
 *     offset (0x800, or 0 when the image already carries the BL1 head),
 *   - the BL2 banner extraction, which on Airoha has to locate the
 *     "tb-fw" payload inside the boot image FIP first.
 *
 * The generic FIP / BL2 / image / storage helpers live in
 * failsafe/bootimg/ and are shared with the MediaTek board code.
 *
 * Target selection is driven by the failsafe_fw_t firmware type that the
 * Web UI derives from the uploaded form field:
 *   firmware    → the system image (one single FIT), written through the
 *                 shared failsafe_storage_write_firmware() to the "fit" /
 *                 "firmware" / "production" partition of an MMC device, or
 *                 to the "fit" UBI volume
 *   bl2         → the preloader; the "bl2" MTD partition on NAND / NOR
 *                 devices, the fixed offset 0x800 of the MMC user area on
 *                 eMMC / SD - see airoha_write_bl2()
 *   chainloader → "chainloader" (MTD partition, either an OpenWrt-style
 *                          second-stage U-Boot FIT or a shim-based
 *                          legacy uImage, modern FIP devices only)
 *   uboot       → "u-boot" (MTD partition, legacy-image devices; the
 *                          512 KiB boot image, with or without BL1)
 *   fip         → "fip"   (UBI static volume on NAND; a GPT partition of
 *                          that name on eMMC, which is where the Airoha
 *                          BL2 looks for it - fill_io_block_spec_gpt())
 *   initramfs   → no flash target, RAM boot via boot_from_mem()
 */

#include <command.h>
#include <env.h>
#include <errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <vsprintf.h>
#include <asm/global_data.h>
#include <failsafe/fw_type.h>
#include <failsafe/internal.h>
#include <failsafe/bl2.h>
#include <failsafe/bl31.h>
#include <failsafe/fip.h>
#include <failsafe/image.h>
#include <failsafe/storage.h>
#include <cprint.h>
#include <failsafe/error.h>

#if IS_ENABLED(CONFIG_MMC)
#include <failsafe/mmc.h>
#endif

#include "failsafe_validate.h"

DECLARE_GLOBAL_DATA_PTR;

/* Staging buffer: 32 MiB past ram_base, safely above U-Boot's working
 * set on all current Airoha SoCs (AN7563, AN7581, EN7523 family, etc.).
 */
#define FAILSAFE_UPLOAD_OFFSET	0x02000000

/* Fallback RAM address used to stage an uploaded RAM-boot image (a FIT
 * initramfs booted via bootm, or a raw image started with the "go"
 * command) when the "loadaddr" environment variable is not set.  It is
 * safely above the failsafe upload staging buffer (ram_base + 32 MiB) and
 * the kernel / dtb / initramfs load addresses on all current Airoha SoCs.
 */
#define FAILSAFE_INITRAMFS_LOAD_FALLBACK	0x89400000

/* Airoha BL2 (preloader) write offset.
 *
 * By default the Airoha bootrom expects the BL2 image 0x800 (2 KiB) into
 * the "bl2" MTD partition, with the leading bytes left at 0xFF.  The
 * default environment writes it exactly that way:
 *
 *     mw.b $loadaddr 0xff 0x800
 *     setexpr loadaddr_bl2 $loadaddr + 0x800
 *     tftpboot $loadaddr_bl2 $bootfile_bl2 && mtd write bl2 $loadaddr
 *
 * "mtd erase bl2" already leaves the partition at 0xFF, so the Web
 * failsafe only needs to start writing at this offset.
 *
 * With CONFIG_AIROHA_PRELOADER_BL1 the flashed artifact is
 * bl1-preloader.bin, which already carries the 2 KiB BL1 region in front
 * of the BL2 FIP.  Writing that image at 0x800 would push the FIP to
 * 0x1000, so it has to be written from offset 0 instead.
 */
#ifdef CONFIG_AIROHA_PRELOADER_BL1
#define FAILSAFE_BL2_WRITE_OFFSET	0
#else
#define FAILSAFE_BL2_WRITE_OFFSET	0x800
#endif

/* The preloader offset on an MMC device.
 *
 * eMMC / SD cards have no "bl2" partition and the Airoha bootrom does not
 * use the boot0 hardware partition either (that layout belongs to the
 * MediaTek SoCs): it reads the preloader from the fixed address 0x800 of
 * the user area, the same offset as in the MTD partition.  The image
 * written with CONFIG_AIROHA_PRELOADER_BL1 carries its 2 KiB BL1 head, so
 * it goes to offset 0 there too.
 */
#ifdef CONFIG_AIROHA_PRELOADER_BL1
#define FAILSAFE_MMC_BL2_OFFSET		0
#else
#define FAILSAFE_MMC_BL2_OFFSET		0x800
#endif

/* Offset of the internal FIP in the legacy 512 KiB boot image: the 2 KiB
 * BL1 (or zero) prefix always precedes it (see tools/airoha_pack_boot.sh).
 */
#define FAILSAFE_LEGACY_FIP_OFFSET	0x800

/* FIP offsets probed for the BL2 "tb-fw" payload, in order: the split
 * preloader.bin keeps the FIP at 0, the legacy image at 0x800.  Only the
 * ARM banner reporting below uses them (see the CONFIG_ARM block near the
 * end of this file).
 */
#if IS_ENABLED(CONFIG_ARM)
static const size_t airoha_bl2_fip_offsets[] = {
	0,
	FAILSAFE_LEGACY_FIP_OFFSET,
};
#endif

/*
 * Map a failsafe firmware type to the physical storage target name.
 *
 * The two bootloader types are mutually exclusive and are selected by the
 * build mode (see failsafe/Kconfig):
 *   - devices built with the legacy 512 KiB image expose "uboot" →
 *     "u-boot" MTD partition (with BL1: BL1 + internal FIP @0x800;
 *     without it - AN7563 - the same self-contained FIP behind a 2 KiB
 *     zero prefix)
 *   - modern FIP devices expose "bl2", "chainloader" and "fip" → "bl2"
 *     MTD partition, "chainloader" MTD partition (OpenWrt-style second
 *     stage U-Boot FIT) and "fip" UBI static volume
 *
 * Returns NULL for the RAM-boot initramfs case (no flash target).
 */
static const char *fw_to_target(failsafe_fw_t fw)
{
	switch (fw) {
	case FW_TYPE_FW:
		return FAILSAFE_STORAGE_FIT_TARGET;
	case FW_TYPE_BL2:
		return "bl2";
	case FW_TYPE_CHAINLOADER:
		return "chainloader";
	case FW_TYPE_UBOOT:
		return "u-boot";
	case FW_TYPE_FIP:
		return FAILSAFE_STORAGE_STATIC_TARGET;
	case FW_TYPE_GPT:
		return FAILSAFE_STORAGE_GPT_TARGET;
	case FW_TYPE_INITRD:
		return NULL;	/* RAM boot, no flash target */
	default:
		return NULL;
	}
}

/* ------------------------------------------------------------------ */
/*  Airoha specific storage policy                                    */
/* ------------------------------------------------------------------ */

/*
 * Capacity check of the preloader location: the "bl2" MTD partition when
 * the device has one (the image goes in at FAILSAFE_BL2_WRITE_OFFSET), the
 * fixed offset of the MMC user area otherwise.
 */
static int airoha_bl2_capacity(size_t size)
{
#if IS_ENABLED(CONFIG_MTD)
	if (failsafe_mtd_exists(FAILSAFE_STORAGE_BL2_TARGET))
		return failsafe_mtd_capacity(FAILSAFE_STORAGE_BL2_TARGET,
					     FAILSAFE_BL2_WRITE_OFFSET, size);
#endif

#if IS_ENABLED(CONFIG_MMC)
	if (failsafe_mmc_present())
		return failsafe_mmc_region_capacity(FAILSAFE_MMC_HWPART_USER,
						    FAILSAFE_MMC_BL2_OFFSET,
						    size);
#endif

	return failsafe_error(-ENODEV,
		"no MTD partition '%s' and no MMC device "
		"to hold the preloader", FAILSAFE_STORAGE_BL2_TARGET);
}

/*
 * Write the preloader where the Airoha bootrom looks for it.  This is the
 * board's business because the location is SoC specific: the MediaTek
 * boards write it to the boot0 hardware partition of an eMMC, Airoha keeps
 * it in the user area at a fixed offset (the bootrom does not read a
 * partition table).
 */
static int airoha_write_bl2(const void *data, size_t size)
{
#if IS_ENABLED(CONFIG_MTD)
	int ret;

	if (failsafe_mtd_exists(FAILSAFE_STORAGE_BL2_TARGET)) {
		ret = failsafe_mtd_capacity(FAILSAFE_STORAGE_BL2_TARGET,
					    FAILSAFE_BL2_WRITE_OFFSET, size);
		if (ret)
			return ret;

		return failsafe_mtd_write(FAILSAFE_STORAGE_BL2_TARGET,
					  FAILSAFE_BL2_WRITE_OFFSET, data,
					  size);
	}
#endif

#if IS_ENABLED(CONFIG_MMC)
	if (failsafe_mmc_present())
		return failsafe_mmc_write_region(FAILSAFE_MMC_HWPART_USER,
						 FAILSAFE_MMC_BL2_OFFSET,
						 data, size);
#endif

	return failsafe_error(-ENODEV,
		"no MTD partition '%s' and no MMC device "
		"to hold the preloader", FAILSAFE_STORAGE_BL2_TARGET);
}

/* Only the ARM banner reporting below reads the preloader back (see the
 * CONFIG_ARM block near the end of this file); the MIPS SoCs (EcoNet)
 * have no FIP to take a banner from.
 */
#if IS_ENABLED(CONFIG_ARM) && !defined(CONFIG_AIROHA_BUILD_LEGACY)
/* Read the flashed preloader back, from the same two locations. */
static int airoha_read_bl2(void *buf, size_t max_len, size_t *read_len)
{
#if IS_ENABLED(CONFIG_MTD)
	if (failsafe_mtd_exists(FAILSAFE_STORAGE_BL2_TARGET))
		return failsafe_mtd_read(FAILSAFE_STORAGE_BL2_TARGET,
					 FAILSAFE_BL2_WRITE_OFFSET, buf,
					 max_len, read_len);
#endif

#if IS_ENABLED(CONFIG_MMC)
	if (failsafe_mmc_present())
		return failsafe_mmc_read_region(FAILSAFE_MMC_HWPART_USER,
						FAILSAFE_MMC_BL2_OFFSET, buf,
						max_len, read_len);
#endif

	return -ENODEV;
}
#endif /* CONFIG_ARM && !CONFIG_AIROHA_BUILD_LEGACY */


/* ------------------------------------------------------------------ */
/*  Public hooks – called by failsafe/core.c                 */
/* ------------------------------------------------------------------ */

void *httpd_get_upload_buffer_ptr(size_t size)
{
	return (void *)(uintptr_t)(gd->ram_base + FAILSAFE_UPLOAD_OFFSET);
}

int failsafe_validate_image(const void *data, size_t size, failsafe_fw_t fw)
{
	const char *target = fw_to_target(fw);
	int ret;

	if (!size) {
		return failsafe_error(-EINVAL, "empty image");
	}

	/* RAM boot (initramfs FIT or raw "go" image): no flash partition,
	 * so only the empty-image check above applies.
	 */
	if (!target)
		return 0;

	/* Generic storage capacity checks - always performed (basic
	 * storage safety), regardless of CONFIG_AIROHA_FAILSAFE_VALIDATE.
	 * For MTD partitions the image, including the write offset, must
	 * fit inside the partition; this is the single size gate for
	 * u-boot / chainloader.  The preloader has its own two locations
	 * (MTD partition with the bootrom offset, MMC user area), so it is
	 * checked by the Airoha specific helper.
	 *
	 * The system image is checked against the partition the write path
	 * will use, not against the board target: on an MMC device the
	 * target ("fit") need not be a partition at all (the OpenWrt eMMC
	 * layout keeps the single FIT in "production"), so the shared
	 * resolver is applied here as well - the same reason as on
	 * MediaTek.
	 */
	if (fw == FW_TYPE_BL2) {
		ret = airoha_bl2_capacity(size);
	} else if (fw == FW_TYPE_FW) {
		const char *part = failsafe_storage_firmware_part(target);

		ret = failsafe_check_capacity(part ? part : target, 0, size);
	} else {
		ret = failsafe_check_capacity(target, 0, size);
	}

	if (ret)
		return ret;

	/* Structural validation - the standalone validation module
	 * (failsafe_validate.c, no-op stub when the master switch is off)
	 * can additionally be turned off at runtime by setting the
	 * "failsafe_validate" environment variable to 0/no; the generic
	 * storage capacity checks above are always performed regardless of
	 * this switch.  When the variable is unset, validation stays
	 * enabled (default).
	 */
#if IS_ENABLED(CONFIG_AIROHA_FAILSAFE_VALIDATE)
	if (env_get_yesno("failsafe_validate") == 0) {
		cprintln(CAUTION, "Failsafe: structural image validation "
			 "disabled by the 'failsafe_validate' environment "
			 "variable");
		return 0;
	}
#endif

	return failsafe_validate_image_content(data, size, fw);
}

int failsafe_write_image(const void *data, size_t size, failsafe_fw_t fw)
{
	const char *target = fw_to_target(fw);

	/* RAM boot (initramfs FIT or raw "go" image): nothing to flash */
	if (!target) {
		return failsafe_error(-EINVAL,
			"no flash target for firmware type %d", fw);
	}

	/*
	 * The preloader is the one type whose location is Airoha specific
	 * (see airoha_write_bl2()); the system image is one FIT written
	 * through the shared helper, and everything else is a generic
	 * target that means the same thing on every SoC.
	 */
	if (fw == FW_TYPE_FW)
		return failsafe_storage_write_firmware(target, data, size);

	if (fw == FW_TYPE_BL2)
		return airoha_write_bl2(data, size);

	return failsafe_storage_write(target, 0, data, size);
}

/*
 * Boot an image previously uploaded by the failsafe Web UI directly from
 * DRAM (RAM boot).  The uploaded image is classified by its header: FIT /
 * legacy uImage images (an initramfs) are booted with bootm, any other
 * image is treated as a raw binary and executed with the "go" command at
 * the same address.
 */
int boot_from_mem(ulong data_load_addr)
{
	return failsafe_boot_image_from_mem(data_load_addr, upload_size,
					    FAILSAFE_INITRAMFS_LOAD_FALLBACK);
}

#if IS_ENABLED(CONFIG_ARM)

/*
 * The banner of an uploaded preloader and the flashed BL2 / BL31 banners
 * (GET /atfversion) are read out of the Airoha FIP - the "tb-fw" and
 * "soc-fw" entries - which only the ARM SoCs use; the helpers live in
 * failsafe/bootimg/bl2.c and failsafe/bootimg/fip.c, built for CONFIG_ARM
 * alone.  The MIPS SoCs (EcoNet) have no FIP at all, their boot chain is
 * a TCBoot loader with the U-Boot payload, so they keep the weak stubs of
 * failsafe/core.c, which report "no information".
 */
int failsafe_bl2_version_info(const void *data, size_t size,
			      failsafe_fw_t fw,
			      struct failsafe_version_info *info)
{
	const void *bl2;
	size_t bl2_size;

	if (!data || !size || !info)
		return -EINVAL;

	memset(info, 0, sizeof(*info));

	/*
	 * Only images that really carry a preloader: a direct BL2 upload
	 * and the legacy 512 KiB U-Boot image (BL2 + BL31 + U-Boot packed
	 * into one internal FIP).  The modern split FIP
	 * (bl31-uboot.fip) holds no BL2 at all.
	 */
	if (fw != FW_TYPE_BL2 && fw != FW_TYPE_UBOOT)
		return -ENOENT;

	/* Restrict the banner scan to the FIP "tb-fw" entry so the BL31 /
	 * U-Boot strings do not leak into the result.
	 */
	failsafe_bl2_locate(data, size, airoha_bl2_fip_offsets,
			    ARRAY_SIZE(airoha_bl2_fip_offsets), &bl2,
			    &bl2_size);
	if (!failsafe_bl2_parse_banner(bl2, bl2_size, info))
		return -ENOENT;

	return 0;
}

/* ------------------------------------------------------------------ */
/*  Flashed BL2 / BL31 version banners (GET /atfversion)               */
/* ------------------------------------------------------------------ */

/*
 * Scratch area used to read the flashed boot chain: one
 * FAILSAFE_STORAGE_STATIC_SIZE (the configured FIP size, see
 * CONFIG_WEBUI_FAILSAFE_FIP_SIZE) fits the whole "fip" UBI volume
 * and, clamped to its real size by the storage helper, the legacy
 * 512 KiB "u-boot" partition.  With CONFIG_WEBUI_FAILSAFE_BL31 the BL31
 * decompression scratch follows it.
 */
#define FAILSAFE_ATF_READ_MAX	FAILSAFE_STORAGE_STATIC_SIZE

#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_BL31)
#define FAILSAFE_ATF_SCRATCH_SIZE	(FAILSAFE_ATF_READ_MAX + \
					 FAILSAFE_BL31_SCRATCH_SIZE)

/* Extract the BL31 banner from the "soc-fw" entry of the boot image FIP.
 * BL31 is LZMA-compressed in the Airoha FIP, so it is expanded into the
 * second half of the scratch area first.
 */
static void airoha_atf_parse_bl31(const void *data, size_t size,
				  u8 *scratch, size_t scratch_size,
				  struct failsafe_version_info *info)
{
	const u8 *payload;
	size_t payload_size;

	if (failsafe_fip_find_at(data, size, airoha_bl2_fip_offsets,
				 ARRAY_SIZE(airoha_bl2_fip_offsets),
				 failsafe_fip_uuid_soc_fw, &payload,
				 &payload_size))
		return;

	failsafe_bl31_parse_banner(payload, payload_size, scratch,
				   scratch_size, info);
}
#else
#define FAILSAFE_ATF_SCRATCH_SIZE	FAILSAFE_ATF_READ_MAX
#endif /* CONFIG_WEBUI_FAILSAFE_BL31 */

int failsafe_atf_version_info(struct failsafe_version_info *bl2,
			      struct failsafe_version_info *bl31)
{
	u8 *read_buf;
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_BL31)
	u8 *bl31_buf;
#endif
	size_t read_len;
	const void *payload;
	size_t payload_size;
	int ret;

	if (!bl2 || !bl31)
		return -EINVAL;

	memset(bl2, 0, sizeof(*bl2));
	memset(bl31, 0, sizeof(*bl31));

	/*
	 * The upload staging buffer doubles as the read scratch: it is the
	 * designated safe DRAM area, and the Web UI hides the fetch button
	 * while an upload is running, so the two never overlap for the
	 * normal single-client case.  (A second client uploading at the
	 * same time would be clobbered - /atfversion is a manual debugging
	 * endpoint, never polled in the background.)
	 *
	 * Split the area into the storage read buffer and the BL31
	 * decompression scratch.
	 */
	read_buf = httpd_get_upload_buffer_ptr(FAILSAFE_ATF_SCRATCH_SIZE);
	if (!read_buf)
		return -ENOMEM;
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_BL31)
	bl31_buf = read_buf + FAILSAFE_ATF_READ_MAX;
#endif

#ifdef CONFIG_AIROHA_BUILD_LEGACY
	/*
	 * Legacy 512 KiB image: BL2 and BL31 share the same internal FIP
	 * (at 0x800, behind the BL1 / zero prefix) stored in the "u-boot"
	 * MTD partition.
	 */
	ret = failsafe_storage_read("u-boot", 0, read_buf,
				    FAILSAFE_ATF_READ_MAX, &read_len);
	if (ret)
		return ret;

	failsafe_bl2_locate(read_buf, read_len, airoha_bl2_fip_offsets,
			    ARRAY_SIZE(airoha_bl2_fip_offsets), &payload,
			    &payload_size);
	failsafe_bl2_parse_banner(payload, payload_size, bl2);

#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_BL31)
	airoha_atf_parse_bl31(read_buf, read_len, bl31_buf,
			      FAILSAFE_BL31_SCRATCH_SIZE, bl31);
#endif
#else
	/*
	 * Modern split FIP: BL2 is the "tb-fw" entry of the preloader - the
	 * one in the "bl2" MTD partition, or the one at the fixed offset of
	 * the MMC user area - and BL31 the "soc-fw" entry of the FIP in the
	 * "fip" volume / partition.
	 */
	ret = airoha_read_bl2(read_buf, FAILSAFE_ATF_READ_MAX, &read_len);
	if (!ret) {
		failsafe_bl2_locate(read_buf, read_len, airoha_bl2_fip_offsets,
				    ARRAY_SIZE(airoha_bl2_fip_offsets),
				    &payload, &payload_size);
		failsafe_bl2_parse_banner(payload, payload_size, bl2);
	}

#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_BL31)
	ret = failsafe_storage_read(FAILSAFE_STORAGE_STATIC_TARGET, 0,
				    read_buf, FAILSAFE_ATF_READ_MAX,
				    &read_len);
	if (!ret)
		airoha_atf_parse_bl31(read_buf, read_len, bl31_buf,
				      FAILSAFE_BL31_SCRATCH_SIZE, bl31);
#endif
#endif

	return (bl2->found || bl31->found) ? 0 : -ENOENT;
}

#endif /* CONFIG_ARM */
