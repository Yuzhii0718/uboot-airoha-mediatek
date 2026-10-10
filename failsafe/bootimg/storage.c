// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Failsafe storage helper: target -> backend resolution.
 *
 * The storage work itself lives in one file per storage type, the same
 * split the MediaTek tree uses between mmc_helper.c and mtd_helper.c:
 *
 *   failsafe/bootimg/mtd.c   raw MTD partitions
 *   failsafe/bootimg/mmc.c   MMC (GPT / MBR) partitions, the partition
 *			      table itself, raw regions of a hardware
 *			      partition and the split system image
 *   failsafe/bootimg/ubi.c   UBI volumes
 *
 * This file only decides which backend handles a target, with two rules:
 *
 *   - the first matching backend wins, in this order:
 *     MTD partition -> MMC -> UBI volume;
 *   - a target that only exists on one kind of storage (the bootloader
 *     stages, the partition table) is never rerouted to another backend:
 *     a missing one is an error, not a UBI volume to create.
 *
 * A backend is only built when its storage type is enabled (see the
 * Makefile), so every call into one is guarded with the same option here:
 * an eMMC-only board links neither the MTD nor the UBI backend.
 *
 * The bootloader stages are the platform's own business: where a preloader
 * or a FIP lives (an MTD partition offset, the boot0 hardware partition of
 * an eMMC, a fixed offset in the user area, a GPT partition) differs
 * between the SoCs, so the board's failsafe code picks the location and
 * calls the backend primitives directly.  The generic path below only
 * knows the targets that mean the same thing everywhere.
 */

#include <errno.h>
#include <linux/string.h>

#include <failsafe/storage.h>
#include <cprint.h>
#include <failsafe/error.h>

#if IS_ENABLED(CONFIG_MMC)
#include <failsafe/mmc.h>
#endif

/*
 * Whether 'name' is a bootloader target that only ever exists as a raw
 * partition and can never be a UBI volume: the second stage U-Boot, the
 * legacy 512 KiB boot image and the preloader.
 *
 * Without this check a missing partition - a typo, or a board built with
 * the wrong device tree - made failsafe_storage_write() silently take the
 * UBI branch and run "ubi create chainloader", which fails with a
 * misleading "not enough PEBs, only 0 available" (or, worse, succeeds and
 * creates a bogus volume next to the real one).
 *
 * The platform code handles its own preloader write before calling the
 * generic path, so reaching "bl2" here means no backend has it.
 */
static bool is_mtd_only_target(const char *name)
{
	return !strcmp(name, FAILSAFE_STORAGE_BL2_TARGET) ||
	       !strcmp(name, FAILSAFE_STORAGE_CHAINLOADER_TARGET) ||
	       !strcmp(name, FAILSAFE_STORAGE_UBOOT_TARGET);
}

#if IS_ENABLED(CONFIG_MMC)
/* Whether 'name' is the MMC partition table (GPT) target.  It only exists
 * on MMC devices, so - like the bootloader targets - it must never be
 * rerouted to another backend.
 */
static bool is_mmc_only_target(const char *name)
{
	return !strcmp(name, FAILSAFE_STORAGE_GPT_TARGET);
}

/* Whether the target is a partition of the MMC device (GPT or MBR
 * label).  This is what makes the generic targets ("fit", "fip", ...)
 * work on eMMC / SD devices without any board level mapping.
 */
static bool is_mmc_partition(const char *name)
{
	if (!name || !name[0])
		return false;

	return failsafe_mmc_part_size(name, NULL) == 0;
}
#endif /* CONFIG_MMC */

int failsafe_check_capacity(const char *target, u64 mtd_off, size_t size)
{
#if IS_ENABLED(CONFIG_MTD)
	if (failsafe_mtd_exists(target))
		return failsafe_mtd_capacity(target, mtd_off, size);
#endif

#if IS_ENABLED(CONFIG_MMC)
	/* MMC only targets: the partition table image ("gpt"). */
	if (is_mmc_only_target(target)) {
		if (size > FAILSAFE_STORAGE_GPT_MAX_SIZE) {
			return failsafe_error(-EFBIG, "GPT image too big "
				"(%zu > %u)", size, (unsigned int)FAILSAFE_STORAGE_GPT_MAX_SIZE);
		}

		return 0;
	}

	if (is_mmc_partition(target)) {
		u64 partition_size;

		if (failsafe_mmc_part_size(target, &partition_size))
			return -ENODEV;

		if (mtd_off + (u64)size > partition_size) {
			return failsafe_error(-EFBIG, "image (%zu) exceeds MMC "
				"partition '%s' (%llu), write offset 0x%llx",
				size, target, (unsigned long long)partition_size,
				(unsigned long long)mtd_off);
		}

		return 0;
	}
#endif /* CONFIG_MMC */

	/* A bootloader target without its MTD partition is a hard error:
	 * never report "fits" for something that cannot be written.
	 */
	if (is_mtd_only_target(target)) {
		return failsafe_error(-ENODEV, "MTD partition '%s' not found",
			target);
	}

#if IS_ENABLED(CONFIG_CMD_UBI)
	return failsafe_ubi_capacity(target, size);
#else
	/* Neither an MTD partition, nor an MMC one, nor a UBI volume:
	 * nothing in this build can hold the target.
	 */
	return failsafe_error(-ENODEV, "storage target '%s' not found", target);
#endif
}

int failsafe_storage_write(const char *target, u64 mtd_off,
			   const void *data, size_t size)
{
	int ret;

	cprintln(NORMAL, "\n*** Failsafe upgrade: %zu (0x%zx) bytes -> "
		 "'%s' ***\n", size, size, target);

	/* Without MTD support a target can only be an MMC partition or a
	 * UBI volume; the check is compiled out with the backend itself.
	 */
#if IS_ENABLED(CONFIG_MTD)
	if (failsafe_mtd_exists(target)) {
		/* ----- MTD partition ----- */
		ret = failsafe_mtd_write(target, mtd_off, data, size);
	} else
#endif /* CONFIG_MTD */
	if (is_mtd_only_target(target)) {
		/* A bootloader stage is always an MTD partition; falling
		 * back to UBI here would create a bogus volume and hide the
		 * real problem (missing partition / wrong device tree).
		 */
		ret = failsafe_error(-ENODEV, "MTD partition '%s' not found, "
			"refusing to fall back to a UBI volume", target);
	} else {
#if IS_ENABLED(CONFIG_MMC)
		if (is_mmc_only_target(target)) {
			/* ----- MMC partition table (GPT) ----- */
			ret = failsafe_mmc_write_gpt(data, size);
		} else if (is_mmc_partition(target)) {
			/* ----- MMC partition ----- */
			ret = failsafe_mmc_write_part(target, data, size);
		} else
#endif /* CONFIG_MMC */
		{
#if IS_ENABLED(CONFIG_CMD_UBI)
			/* ----- UBI volume ----- */
			ret = failsafe_ubi_write(target, data, size);
#else
			ret = failsafe_error(-ENODEV,
				"storage target '%s' not found", target);
#endif /* CONFIG_CMD_UBI */
		}
	}

	if (ret)
		return ret;

	cprintln(SUCCESS, "\n*** Failsafe upgrade completed ('%s') ***\n",
		 target);
	return 0;
}

#if IS_ENABLED(CONFIG_MMC)
/*
 * The partition names of the standard single-image (ITB / FIT) layout of an
 * MMC system image: the board's own target ("fit") comes first, then the
 * two well-known ones.  Only an existing partition is used, so a board
 * with one of the other layouts (or with a UBI volume instead) is not
 * affected.
 *
 * The resolver is public (see include/failsafe/storage.h) because the
 * board level capacity check has to name the same partition the write path
 * picks: the board target need not exist at all ("fit" is a UBI volume on
 * NAND / NOR, while the OpenWrt eMMC layout carries the single FIT in
 * "production"), and validating the raw target instead of the resolved one
 * rejected the upload before the fallback could run.
 */
static const char *const mmc_fw_part_names[] = {
	FAILSAFE_STORAGE_FIT_TARGET,
	FAILSAFE_STORAGE_FIRMWARE_TARGET,
	FAILSAFE_STORAGE_PRODUCTION_TARGET,
};
#endif /* CONFIG_MMC */

const char *failsafe_storage_firmware_part(const char *target)
{
#if IS_ENABLED(CONFIG_MMC)
	int i;

	/* The target the board asked for wins when it is a partition. */
	if (target && target[0] && !failsafe_mmc_part_size(target, NULL))
		return target;

	for (i = 0; i < ARRAY_SIZE(mmc_fw_part_names); i++)
		if (!failsafe_mmc_part_size(mmc_fw_part_names[i], NULL))
			return mmc_fw_part_names[i];
#else
	(void)target;
#endif /* CONFIG_MMC */

	return NULL;
}

int failsafe_storage_write_firmware(const char *target, const void *data,
				    size_t size)
{
	const char *part;

	if (!data || !size)
		return -EINVAL;

	/*
	 * An eMMC / SD system image is one ITB / FIT in a single partition of
	 * the standard layout, not a UBI volume.
	 */
	part = failsafe_storage_firmware_part(target);
	if (part)
		return failsafe_storage_write(part, 0, data, size);

	/* NAND / NOR devices: the "fit" volume, as before. */
	return failsafe_storage_write(target, 0, data, size);
}

int failsafe_storage_read(const char *target, u64 offset, void *buf,
			  size_t max_len, size_t *read_len)
{
	if (!target || !buf || !max_len)
		return -EINVAL;

#if IS_ENABLED(CONFIG_MTD)
	if (failsafe_mtd_exists(target))
		return failsafe_mtd_read(target, offset, buf, max_len,
					 read_len);
#endif

#if IS_ENABLED(CONFIG_MMC)
	/* "fip" is the only UBI volume that also exists as an MMC partition.
	 * Reading it by name has to find the same copy the write path uses
	 * (the platform decides where the FIP lives). */
	if (is_mmc_partition(target))
		return failsafe_mmc_read_part(target, offset, buf, max_len,
					      read_len);
#endif /* CONFIG_MMC */

#if IS_ENABLED(CONFIG_CMD_UBI)
	return failsafe_ubi_read(target, offset, buf, max_len, read_len);
#else
	return -ENODEV;
#endif
}
