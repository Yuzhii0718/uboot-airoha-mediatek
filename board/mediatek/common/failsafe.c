// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Mediatek failsafe board hooks (Web recovery / httpd upload).
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
 * Only the Mediatek specific policy lives here: the uploaded DRAM
 * staging buffer / RAM-boot fallback address and the firmware-type →
 * storage-target mapping.  The generic FIP / BL2 / image / storage
 * helpers live in failsafe/bootimg/ and are shared with the Airoha
 * board code.
 *
 * Target selection is driven by the failsafe_fw_t firmware type that the
 * Web UI derives from the uploaded form field:
 *   firmware    → the system image (one single FIT), written through the
 *                 shared failsafe_storage_write_firmware() to the "fit" /
 *                 "firmware" / "production" partition of an MMC device, or
 *                 to the "fit" UBI volume
 *   bl2         → the preloader; the "bl2" MTD partition on NAND / NOR
 *                 devices, on MMC the eMMC boot0 hardware partition or the
 *                 "bl2" partition of an SD card - see mtk_write_bl2()
 *   fip         → "fip"   (UBI static volume on NAND; the GPT partition
 *                          "fip" on eMMC, which is where the MediaTek ATF
 *                          looks for it - generic_mmc_write_fip_uda())
 *   initramfs   → no flash target, RAM boot via boot_from_mem()
 */

#include <command.h>
#include <env.h>
#include <errno.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
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

/*
 * Staging buffer for received uploads.
 *
 * ram_base + 64 MiB keeps the buffer clear of the ARM Trusted Firmware
 * (BL31 / secmon) no-map reserved area that starts at ram_base + 48 MiB
 * (0x43000000 on 0x40000000-based SoCs).  A buffer at ram_base + 32 MiB
 * used to overlap that area once an upload grew past 16 MiB, and writes
 * into the BL31 region were clobbered (observed as a zeroed hole in the
 * received image).  The original MediaTek httpd moves the buffer to
 * ram_base + 96 MiB for the same reason; 64 MiB is enough here while
 * still leaving room below the FAILSAFE_INITRAMFS_LOAD_FALLBACK address.
 */
#define FAILSAFE_UPLOAD_OFFSET	0x04000000

/* Fallback RAM address used to stage an uploaded RAM-boot image (a FIT
 * initramfs booted via bootm, or a raw image started with the "go"
 * command) when the "loadaddr" environment variable is not set.  It is
 * safely above the failsafe upload staging buffer (ram_base + 64 MiB) and
 * the kernel / dtb / initramfs load addresses on all current Mediatek SoCs.
 */
#define FAILSAFE_INITRAMFS_LOAD_FALLBACK	0x46000000

/*
 * Map a failsafe firmware type to the physical storage target name.
 *
 * The bootloader types are selected by the build mode (see
 * failsafe/Kconfig):
 *   - modern FIP devices expose "bl2" and "fip" → "bl2" MTD partition
 *     and "fip" UBI static volume
 *
 * Returns NULL for the RAM-boot case (initramfs FIT or raw "go" image;
 * no flash target).
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
/*  MediaTek specific storage policy                                  */
/* ------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_MMC)
/*
 * The preloader area inside the eMMC boot0 partition.  boot0 is 4 MiB on
 * the parts supported today and the preloader owns its first 1 MiB, the
 * same layout the MediaTek tree uses (generic_emmc_write_bl2()).
 */
#define MTK_EMMC_BL2_AREA_SIZE		SZ_1M

/*
 * Point the bootrom at the boot0 partition it has just been given a
 * preloader in: BOOT_BUS_WIDTH / PART_CONF / RST_N_FUNCTION, the three
 * EXT_CSD fields mmc_setup_boot_options() of the MediaTek tree
 * (board/mediatek/common/mmc_helper.c) writes.
 *
 * A failure is reported but not fatal: the image is already in flash.
 */
static void mtk_setup_boot_options(struct mmc *mmc)
{
#if IS_ENABLED(CONFIG_SUPPORT_EMMC_BOOT)
	int ret;

	ret = mmc_set_boot_bus_width(mmc, EXT_CSD_BUS_WIDTH_8, 0, 0);
	if (ret)
		cprintln(CAUTION, "Failsafe: set BOOT_BUS_WIDTH failed (%d)",
			 ret);

	ret = mmc_set_part_conf(mmc, 1, 1, 0);
	if (ret)
		cprintln(CAUTION, "Failsafe: set PART_CONF failed (%d)", ret);

	ret = mmc_set_rst_n_function(mmc, 1);
	if (ret)
		cprintln(CAUTION, "Failsafe: set RST_N_FUNCTION failed (%d)",
			 ret);
#else
	(void)mmc;
#endif
}
#endif /* CONFIG_MMC */

/*
 * Capacity check of the preloader location: the "bl2" MTD partition when
 * the device has one, on MMC the "bl2" partition of an SD card or the
 * boot0 hardware partition of an eMMC.
 */
static int mtk_bl2_capacity(size_t size)
{
#if IS_ENABLED(CONFIG_MTD)
	if (failsafe_mtd_exists(FAILSAFE_STORAGE_BL2_TARGET))
		return failsafe_mtd_capacity(FAILSAFE_STORAGE_BL2_TARGET, 0,
					     size);
#endif

#if IS_ENABLED(CONFIG_MMC)
	switch (failsafe_mmc_is_sd()) {
	case 1:
		/* SD card: the preloader has its own partition. */
		return failsafe_mmc_part_size(FAILSAFE_STORAGE_BL2_TARGET,
					      NULL) ? -ENODEV : 0;
	case 0:
		/* eMMC: the first MTK_EMMC_BL2_AREA_SIZE bytes of boot0. */
		if (size > MTK_EMMC_BL2_AREA_SIZE) {
			return failsafe_error(-EFBIG,
				"preloader (%zu) exceeds the "
				"0x%zx byte boot area of the eMMC boot0 "
				"partition", size,
				(size_t)MTK_EMMC_BL2_AREA_SIZE);
		}

		return 0;
	default:
		break;
	}
#endif

	return failsafe_error(-ENODEV,
		"no MTD partition '%s' and no MMC device "
		"to hold the preloader", FAILSAFE_STORAGE_BL2_TARGET);
}

/*
 * Write the preloader where the MediaTek bootrom looks for it: the boot0
 * hardware partition of an eMMC (it is addressable without any partition
 * table, so this also works on a device that has never seen a GPT) or the
 * "bl2" partition of an SD card (which has no hardware partitions).
 */
static int mtk_write_bl2(const void *data, size_t size)
{
#if IS_ENABLED(CONFIG_MMC)
	struct mmc *mmc = failsafe_mmc_get_dev();
	int ret;
#endif

#if IS_ENABLED(CONFIG_MTD)
	if (failsafe_mtd_exists(FAILSAFE_STORAGE_BL2_TARGET))
		return failsafe_mtd_write(FAILSAFE_STORAGE_BL2_TARGET, 0, data,
					  size);
#endif

#if IS_ENABLED(CONFIG_MMC)
	if (!mmc) {
		return failsafe_error(-ENODEV,
			"no MTD partition '%s' and no MMC "
			"device to hold the preloader",
			FAILSAFE_STORAGE_BL2_TARGET);
	}

	if (failsafe_mmc_is_sd() == 1)
		return failsafe_mmc_write_part(FAILSAFE_STORAGE_BL2_TARGET,
					       data, size);

	cprintln(NORMAL, "Failsafe: preloader -> MMC boot0 @ 0x0");

	ret = failsafe_mmc_write_region(FAILSAFE_MMC_HWPART_BOOT0, 0, data,
					size);
	if (ret)
		return ret;

	mtk_setup_boot_options(mmc);
	return 0;
#else
	return failsafe_error(-ENODEV, "no MTD partition '%s' to hold the "
		"preloader", FAILSAFE_STORAGE_BL2_TARGET);
#endif /* CONFIG_MMC */
}

/* Read the flashed preloader back, from the same locations.  Used by the
 * ARM only /atfversion hook, hence __maybe_unused.
 */
static int __maybe_unused mtk_read_bl2(void *buf, size_t max_len,
				       size_t *read_len)
{
#if IS_ENABLED(CONFIG_MMC)
	int is_sd;
#endif

#if IS_ENABLED(CONFIG_MTD)
	if (failsafe_mtd_exists(FAILSAFE_STORAGE_BL2_TARGET))
		return failsafe_mtd_read(FAILSAFE_STORAGE_BL2_TARGET, 0, buf,
					 max_len, read_len);
#endif

#if IS_ENABLED(CONFIG_MMC)
	is_sd = failsafe_mmc_is_sd();

	if (is_sd == 1)
		return failsafe_mmc_read_part(FAILSAFE_STORAGE_BL2_TARGET, 0,
					      buf, max_len, read_len);

	if (is_sd == 0)
		return failsafe_mmc_read_region(FAILSAFE_MMC_HWPART_BOOT0, 0,
						buf, max_len, read_len);
#endif

	return -ENODEV;
}

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
	 * storage safety), regardless of CONFIG_MTK_FAILSAFE_VALIDATE.
	 * For MTD partitions the image must fit inside the partition;
	 * this is the single size gate for fip.  The preloader has its own
	 * three locations (MTD partition, eMMC boot0, SD partition), so it
	 * is checked by the MediaTek specific helper.
	 *
	 * The system image is checked against the partition the write path
	 * will use, not against the board target: on an MMC device the
	 * target ("fit") need not be a partition at all - the OpenWrt eMMC
	 * layout keeps the single FIT in "production" - so the shared
	 * resolver is applied here as well.  Without it an upload to such a
	 * device was rejected with -ENODEV before the write path's fallback
	 * could pick "production".
	 */
	if (fw == FW_TYPE_BL2) {
		ret = mtk_bl2_capacity(size);
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
#if IS_ENABLED(CONFIG_MTK_FAILSAFE_VALIDATE)
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
	 * The preloader is the one type whose location is MediaTek specific
	 * (see mtk_write_bl2()); the system image is one FIT written
	 * through the shared helper, and everything else is a generic
	 * target that means the same thing on every SoC.
	 */
	if (fw == FW_TYPE_FW)
		return failsafe_storage_write_firmware(target, data, size);

	if (fw == FW_TYPE_BL2)
		return mtk_write_bl2(data, size);

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

int failsafe_bl2_version_info(const void *data, size_t size,
			      failsafe_fw_t fw,
			      struct failsafe_version_info *info)
{
	if (!data || !size || !info)
		return -EINVAL;

	memset(info, 0, sizeof(*info));

	/*
	 * Only a direct BL2 upload carries a preloader banner.  The
	 * MediaTek U-Boot image is not a BL2 container, so nothing is
	 * reported for it.
	 */
	if (fw != FW_TYPE_BL2)
		return -ENOENT;

	if (!failsafe_bl2_parse_banner(data, size, info))
		return -ENOENT;

	return 0;
}

/* ------------------------------------------------------------------ */
/*  Flashed BL2 / BL31 version banners (GET /atfversion)               */
/* ------------------------------------------------------------------ */

/*
 * Scratch area used to read the flashed boot chain: one
 * FAILSAFE_STORAGE_STATIC_SIZE (the configured FIP size, 2 MiB on
 * MediaTek - see CONFIG_WEBUI_FAILSAFE_FIP_SIZE) fits the whole "fip" UBI
 * volume and, clamped to its real size by the storage helper, the "bl2"
 * MTD partition.  With CONFIG_WEBUI_FAILSAFE_BL31 the BL31 decompression
 * scratch follows it.
 */
#define FAILSAFE_ATF_READ_MAX	FAILSAFE_STORAGE_STATIC_SIZE

#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_BL31)
#define FAILSAFE_ATF_SCRATCH_SIZE	(FAILSAFE_ATF_READ_MAX + \
					 FAILSAFE_BL31_SCRATCH_SIZE)
#else
#define FAILSAFE_ATF_SCRATCH_SIZE	FAILSAFE_ATF_READ_MAX
#endif /* CONFIG_WEBUI_FAILSAFE_BL31 */

int failsafe_atf_version_info(struct failsafe_version_info *bl2,
			      struct failsafe_version_info *bl31)
{
	/* The preloader may keep its FIP at 0 or behind a BL1 prefix. */
	static const size_t fip_offsets[] = { 0, 0x800 };
	u8 *read_buf;
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_BL31)
	u8 *bl31_buf;
	const u8 *fip_payload;
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

	/*
	 * BL2: the preloader stored in the "bl2" MTD partition, or on MMC
	 * in the boot0 hardware partition / the "bl2" partition of an SD
	 * card.  It is a bare image on MediaTek, but the FIP probe keeps
	 * boards that wrap it working.
	 */
	ret = mtk_read_bl2(read_buf, FAILSAFE_ATF_READ_MAX, &read_len);
	if (!ret) {
		failsafe_bl2_locate(read_buf, read_len, fip_offsets,
				    ARRAY_SIZE(fip_offsets), &payload,
				    &payload_size);
		failsafe_bl2_parse_banner(payload, payload_size, bl2);
	}

#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_BL31)
	/* BL31: the "soc-fw" entry of the FIP stored in the "fip" UBI
	 * volume (raw or XZ-compressed on MediaTek). */
	ret = failsafe_storage_read(FAILSAFE_STORAGE_STATIC_TARGET, 0,
				    read_buf, FAILSAFE_ATF_READ_MAX,
				    &read_len);
	if (!ret &&
	    !failsafe_fip_find_at(read_buf, read_len, fip_offsets,
				  ARRAY_SIZE(fip_offsets),
				  failsafe_fip_uuid_soc_fw, &fip_payload,
				  &payload_size))
		failsafe_bl31_parse_banner(fip_payload, payload_size, bl31_buf,
					   FAILSAFE_BL31_SCRATCH_SIZE, bl31);
#endif

	return (bl2->found || bl31->found) ? 0 : -ENOENT;
}

#endif /* CONFIG_ARM */
