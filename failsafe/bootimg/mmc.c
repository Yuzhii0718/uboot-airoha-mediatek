// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Failsafe storage backend: MMC devices (eMMC / SD).
 *
 * This is the "mechanism" half of the MMC support; the other half, which
 * image goes to which region or partition, is platform policy and lives in
 * the board's own failsafe code (board/{airoha,mediatek}/common/failsafe.c)
 * - that is where the layout differences between the SoCs are expressed.
 *
 * Ported from the MediaTek board helper (board/mediatek/common/mmc_helper.c)
 * and reduced to what a generic U-Boot tree needs: apart from the standard
 * U-Boot MMC / block / partition API nothing board specific is used, so the
 * backend works on any eMMC or SD device:
 *
 *   - write / read / erase an MMC partition by its GPT or MBR name;
 *   - read / write / check a raw region of a hardware partition
 *     (FAILSAFE_MMC_HWPART_*: the user area, boot0, boot1);
 *   - install the partition table (GPT) itself: the uploaded primary
 *     table is adjusted to the real device size, the secondary table is
 *     generated at the end of the device, and both are read back.
 */

#include <errno.h>
#include <malloc.h>
#include <blk.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/byteorder/little_endian.h>
#include <u-boot/crc.h>
#include <mmc.h>
#include <part.h>
#include <part_efi.h>

#include <failsafe/mmc.h>
#include <failsafe/storage.h>
#include <cprint.h>
#include <failsafe/error.h>

#if IS_ENABLED(CONFIG_MMC)

/*
 * part_efi.h only defines the LBA of the GPT header itself; the partition
 * entry array always starts on the next LBA.
 */
#define GPT_HEADER_LBA		GPT_PRIMARY_PARTITION_TABLE_LBA
#define GPT_ENTRIES_LBA		2ULL

/* ------------------------------------------------------------------ */
/*  Partitions by name                                                 */
/* ------------------------------------------------------------------ */

int failsafe_mmc_part_size(const char *name, u64 *size)
{
	struct mmc *mmc = failsafe_mmc_get_dev();
	struct disk_partition dpart;
	int ret;

	if (!mmc)
		return -ENODEV;

	ret = failsafe_mmc_find_part(mmc, name, &dpart);
	if (ret)
		return ret;

	if (size)
		*size = (u64)dpart.size * dpart.blksz;

	return 0;
}

int failsafe_mmc_write_part(const char *name, const void *data, size_t size)
{
	struct mmc *mmc = failsafe_mmc_get_dev();
	struct disk_partition dpart;
	u64 partition_size, offset;
	int ret;

	if (!mmc || !data || !size)
		return -EINVAL;

	ret = failsafe_mmc_find_part(mmc, name, &dpart);
	if (ret)
		return ret;

	partition_size = (u64)dpart.size * dpart.blksz;
	if ((u64)size > partition_size) {
		return failsafe_error(-EFBIG, "image (%zu) exceeds MMC "
			"partition '%s' (%llu)", size, name, (unsigned long long)partition_size);
	}

	offset = (u64)dpart.start * dpart.blksz;

	cprintln(NORMAL, "Failsafe: writing %zu (0x%zx) bytes to MMC "
		 "partition '%s' @ 0x%llx", size, size, name,
		 (unsigned long long)offset);

	return failsafe_mmc_write(mmc, offset, data, size);
}

int failsafe_mmc_read_part(const char *name, u64 offset, void *buf,
			   size_t max_len, size_t *read_len)
{
	struct mmc *mmc = failsafe_mmc_get_dev();
	struct disk_partition dpart;
	u64 partition_size, partition_offset;
	size_t len;
	int ret;

	if (read_len)
		*read_len = 0;

	if (!mmc || !buf || !max_len)
		return -EINVAL;

	ret = failsafe_mmc_find_part(mmc, name, &dpart);
	if (ret)
		return ret;

	partition_size = (u64)dpart.size * dpart.blksz;
	partition_offset = (u64)dpart.start * dpart.blksz;

	if (offset >= partition_size)
		return -EINVAL;

	len = partition_size - offset;
	if (len > max_len)
		len = max_len;

	ret = failsafe_mmc_read(mmc, partition_offset + offset, buf, len);
	if (ret)
		return ret;

	if (read_len)
		*read_len = len;

	return 0;
}

int failsafe_mmc_erase_part(const char *name, u64 offset, u64 size)
{
	struct mmc *mmc = failsafe_mmc_get_dev();
	struct disk_partition dpart;
	u64 partition_size, partition_offset;
	int ret;

	if (!mmc)
		return -ENODEV;

	ret = failsafe_mmc_find_part(mmc, name, &dpart);
	if (ret)
		return ret;

	partition_size = (u64)dpart.size * dpart.blksz;
	partition_offset = (u64)dpart.start * dpart.blksz;

	if (offset >= partition_size)
		return -EINVAL;

	if (!size || offset + size > partition_size)
		size = partition_size - offset;

	return failsafe_mmc_erase(mmc, partition_offset + offset, (size_t)size);
}

/* ------------------------------------------------------------------ */
/*  Partition table (GPT)                                              */
/* ------------------------------------------------------------------ */

/*
 * Installing a partition table is a generic operation, not a platform
 * one: the result is the standard EFI layout both boot chains parse, so
 * that the running system can look its partitions up by name afterwards.
 *
 * The layout written here (and the one the uploads carry) is what the
 * loaders expect:
 *
 *   - LBA 0: protective MBR, partition 1 marked EFI_PMBR_OSTYPE_EFI_GPT
 *            (0xEE);
 *   - LBA 1: primary GPT header ("EFI PART" + header CRC32), followed by
 *            the partition entry array;
 *   - the last 33 blocks: the backup table (entry array + header with
 *            my_lba = alternate_lba), i.e. what
 *            drivers/partition/partition.c of the Airoha TF-A reads when
 *            the primary header is damaged (load_backup_gpt()), and what
 *            the U-Boot side reads through the standard partition code.
 *
 * The uploaded image only carries the primary table area (MBR + header +
 * entries, 34 blocks): the header is adjusted to the real device and the
 * backup table is derived from it.
 */

/*
 * Adjust a GPT header to the real device: alternate_lba has to point at
 * the last LBA, last_usable_lba just in front of the secondary table (so
 * the two never overlap), and the header CRC32 has to be recomputed after
 * both changes.  The image is modified in place.
 */
static int mmc_gpt_adjust(struct mmc *mmc, void *data, size_t size)
{
	struct blk_desc *bd = failsafe_mmc_blk_desc(mmc);
	const legacy_mbr *mbr = data;
	u64 blksz, lastlba, secondary_lbas, last_usable;
	u32 crc_saved, crc_calc;
	gpt_header gpt;

	if (!bd || !bd->blksz)
		return -ENODEV;

	blksz = bd->blksz;

	/* The protective MBR and the header must be part of the image. */
	if (size < (GPT_HEADER_LBA + 1) * blksz)
		return -EINVAL;

	/* A GPT disk announces itself in its protective MBR; a loader that
	 * follows the flow above bails out right there, so a table without
	 * it must not be installed. */
	if (le16_to_cpu(mbr->signature) != MSDOS_MBR_SIGNATURE ||
	    mbr->partition_record[0].sys_ind != EFI_PMBR_OSTYPE_EFI_GPT) {
		return failsafe_error(-EINVAL,
			"image has no protective GPT MBR");
	}

	memcpy(&gpt, (u8 *)data + GPT_HEADER_LBA * blksz, sizeof(gpt));

	if (le64_to_cpu(gpt.signature) != GPT_HEADER_SIGNATURE_UBOOT)
		return -EINVAL;

	/* Validate the header CRC of the uploaded table first. */
	memcpy(&crc_saved, &gpt.header_crc32, sizeof(crc_saved));
	memset(&gpt.header_crc32, 0, sizeof(gpt.header_crc32));
	crc_calc = crc32(0, (const unsigned char *)&gpt,
			 le32_to_cpu(gpt.header_size));
	memcpy(&gpt.header_crc32, &crc_saved, sizeof(crc_saved));

	if (crc_calc != le32_to_cpu(crc_saved))
		return -EINVAL;

	if (le64_to_cpu(gpt.my_lba) != GPT_HEADER_LBA)
		return -EINVAL;

	lastlba = (u64)bd->lba;
	if (le64_to_cpu(gpt.first_usable_lba) >= lastlba)
		return -EINVAL;

	gpt.alternate_lba = cpu_to_le64(lastlba - 1);

	/* The secondary table has no protective MBR, so it is one LBA
	 * shorter than the primary one. */
	secondary_lbas = DIV_ROUND_UP(size, blksz) - 1;
	last_usable = lastlba - secondary_lbas - 1;
	gpt.last_usable_lba = cpu_to_le64(last_usable);

	memset(&gpt.header_crc32, 0, sizeof(gpt.header_crc32));
	crc_calc = crc32(0, (const unsigned char *)&gpt,
			 le32_to_cpu(gpt.header_size));
	memcpy(&gpt.header_crc32, &crc_calc, sizeof(crc_calc));

	memcpy((u8 *)data + GPT_HEADER_LBA * blksz, &gpt, sizeof(gpt));

	cprintln(NORMAL, "Failsafe: GPT adjusted to the device: last usable "
		 "LBA 0x%llx", (unsigned long long)last_usable);

	return 0;
}

/*
 * Derive the secondary (backup) table from the primary image and write it
 * to the end of the device: the partition entries move to the front of
 * the buffer, my_lba / partition_entry_lba are rewritten, the CRC is
 * recomputed and the buffer is written one LBA before the last one.
 */
static int mmc_gpt_write_secondary(struct mmc *mmc, void *data, size_t size)
{
	struct blk_desc *bd = failsafe_mmc_blk_desc(mmc);
	u64 blksz, lastlba, secondary_lbas, backup_offset;
	u32 crc_calc;
	size_t secondary_size, entry_size;
	gpt_header gpt;

	if (!bd || !bd->blksz)
		return -ENODEV;

	blksz = bd->blksz;

	secondary_size = size - GPT_HEADER_LBA * blksz;
	secondary_lbas = DIV_ROUND_UP(secondary_size, blksz);
	entry_size = secondary_size - GPT_HEADER_LBA * blksz;

	memcpy(&gpt, (u8 *)data + GPT_HEADER_LBA * blksz, sizeof(gpt));

	/* The entry array is the first block of the secondary image. */
	memmove(data, (u8 *)data + GPT_ENTRIES_LBA * blksz, entry_size);

	gpt.my_lba = cpu_to_le64(gpt.alternate_lba);

	lastlba = (u64)bd->lba;
	gpt.partition_entry_lba = cpu_to_le64(lastlba - secondary_lbas);

	memset(&gpt.header_crc32, 0, sizeof(gpt.header_crc32));
	crc_calc = crc32(0, (const unsigned char *)&gpt,
			 le32_to_cpu(gpt.header_size));
	memcpy(&gpt.header_crc32, &crc_calc, sizeof(crc_calc));

	memcpy((u8 *)data + entry_size, &gpt, sizeof(gpt));
	memset((u8 *)data + entry_size + sizeof(gpt), 0,
	       blksz - sizeof(gpt));

	backup_offset = (lastlba - secondary_lbas) * blksz;

	cprintln(NORMAL, "Failsafe: writing secondary GPT @ 0x%llx",
		 (unsigned long long)backup_offset);

	return failsafe_mmc_write(mmc, backup_offset, data, secondary_size);
}

/*
 * Read one GPT header back from the device and verify it the way a loader
 * does: signature, header CRC32 (computed with the CRC field zeroed) and
 * my_lba.  Used for both copies, so a table the loader would reject is
 * never reported as installed.
 */
static int mmc_gpt_verify_header(struct mmc *mmc, u64 lba)
{
	struct blk_desc *bd = failsafe_mmc_blk_desc(mmc);
	u32 crc_saved, crc_calc;
	gpt_header gpt;
	int ret;

	ret = failsafe_mmc_read(mmc, lba * bd->blksz, &gpt, sizeof(gpt));
	if (ret)
		return ret;

	if (le64_to_cpu(gpt.signature) != GPT_HEADER_SIGNATURE_UBOOT)
		return -EINVAL;

	if (le64_to_cpu(gpt.my_lba) != lba)
		return -EINVAL;

	memcpy(&crc_saved, &gpt.header_crc32, sizeof(crc_saved));
	memset(&gpt.header_crc32, 0, sizeof(gpt.header_crc32));
	crc_calc = crc32(0, (const unsigned char *)&gpt,
			 le32_to_cpu(gpt.header_size));

	if (crc_calc != le32_to_cpu(crc_saved))
		return -EINVAL;

	return 0;
}

int failsafe_mmc_write_gpt(const void *data, size_t size)
{
	struct mmc *mmc = failsafe_mmc_get_dev();
	u8 *buf;
	int ret;

	if (!mmc)
		return -ENODEV;

	if (!data || !size || size > FAILSAFE_STORAGE_GPT_MAX_SIZE) {
		return failsafe_error(-EINVAL,
			"GPT image size %zu is out of range "
			"(1 - %u bytes)",
			size, (unsigned int)FAILSAFE_STORAGE_GPT_MAX_SIZE);
	}

	/* Work on a private copy: the staging buffer stays untouched. */
	buf = malloc(size);
	if (!buf)
		return -ENOMEM;

	memcpy(buf, data, size);

	ret = mmc_gpt_adjust(mmc, buf, size);
	if (ret) {
		failsafe_error(ret, "invalid GPT image");
		goto out;
	}

	cprintln(NORMAL, "Failsafe: writing GPT (%zu bytes) to MMC @ 0x0",
		 size);

	ret = failsafe_mmc_write(mmc, 0, buf, size);
	if (ret) {
		failsafe_error(ret, "GPT write failed");
		goto out;
	}

	ret = mmc_gpt_write_secondary(mmc, buf, size);
	if (ret) {
		failsafe_error(ret, "secondary GPT write failed");
		goto out;
	}

	/*
	 * Read both headers back from the device and check signature, CRC
	 * and my_lba again: the table is only useful when the loaders can
	 * parse it, and the backup one is an independent copy the primary
	 * falls back to when it is damaged.
	 */
	ret = mmc_gpt_verify_header(mmc, GPT_HEADER_LBA);
	if (ret) {
		failsafe_error(ret, "primary GPT is not readable back");
		goto out;
	}

	ret = mmc_gpt_verify_header(mmc, (u64)failsafe_mmc_blk_desc(mmc)->lba - 1);
	if (ret) {
		failsafe_error(ret, "backup GPT is not readable back");
		goto out;
	}

	cprintln(SUCCESS, "Failsafe: GPT written and verified (primary + "
		 "backup)");

#if IS_ENABLED(CONFIG_PARTITIONS)
	/* Re-read the table so the partitions are usable right away. */
	part_init(failsafe_mmc_blk_desc(mmc));
#endif

out:
	free(buf);
	return ret;
}

/* ------------------------------------------------------------------ */
/*  Raw regions inside a hardware partition                            */
/* ------------------------------------------------------------------ */

/*
 * Which hardware partition holds what - the preloader, a FIP, ... - is the
 * platform's policy and is implemented in the board's own failsafe code.
 * What this backend provides is the mechanism: select the hardware
 * partition, then read / write a byte range of it.  An SD card has no
 * hardware partitions at all (mmc->part_config == MMCPART_NOAVAILABLE),
 * so anything but the user area is refused there.
 */
static int mmc_select_hwpart(struct mmc *mmc, int hwpart)
{
	if (mmc->part_config == MMCPART_NOAVAILABLE)
		return -ENOTSUPP;

	if (blk_select_hwpart_devnum(UCLASS_MMC, FAILSAFE_MMC_DEV_NUM, hwpart))
		return -EIO;

	if (mmc_get_blk_desc(mmc)->hwpart != hwpart)
		return -EIO;

	return 0;
}

int failsafe_mmc_is_sd(void)
{
	struct mmc *mmc = failsafe_mmc_get_dev();

	if (!mmc)
		return -ENODEV;

	return IS_SD(mmc) ? 1 : 0;
}

/* Start a raw region access: the device has to exist and the hardware
 * partition has to be selectable (see mmc_select_hwpart()).
 */
static int mmc_region_begin(int hwpart, struct mmc **retmmc)
{
	struct mmc *mmc = failsafe_mmc_get_dev();
	int ret;

	*retmmc = NULL;

	if (!mmc)
		return -ENODEV;

	if (hwpart != FAILSAFE_MMC_HWPART_USER) {
		ret = mmc_select_hwpart(mmc, hwpart);
		if (ret)
			return failsafe_error(ret, "cannot access MMC hardware "
				"partition %d", hwpart);
	}

	*retmmc = mmc;
	return 0;
}

/* End a raw region access: back to the user area, so whatever runs next
 * (partition table, firmware image, ...) sees the normal device again.
 */
static void mmc_region_end(struct mmc *mmc, int hwpart)
{
	if (mmc && hwpart != FAILSAFE_MMC_HWPART_USER)
		mmc_select_hwpart(mmc, FAILSAFE_MMC_HWPART_USER);
}

int failsafe_mmc_region_capacity(int hwpart, u64 off, size_t size)
{
	struct mmc *mmc;
	u64 capacity;
	int ret;

	ret = mmc_region_begin(hwpart, &mmc);
	if (ret)
		return ret;

	capacity = (u64)failsafe_mmc_blk_desc(mmc)->lba *
		   failsafe_mmc_blk_desc(mmc)->blksz;

	if (off + (u64)size > capacity) {
		ret = failsafe_error(-EFBIG, "region (%zu @ 0x%llx) is outside "
			"the MMC device (%llu bytes)", size, (unsigned long long)off,
			(unsigned long long)capacity);
	}

	mmc_region_end(mmc, hwpart);
	return ret;
}

int failsafe_mmc_write_region(int hwpart, u64 off, const void *data,
			      size_t size)
{
	struct mmc *mmc;
	int ret;

	if (!data || !size)
		return -EINVAL;

	ret = mmc_region_begin(hwpart, &mmc);
	if (ret)
		return ret;

	cprintln(NORMAL, "Failsafe: writing %zu (0x%zx) bytes to MMC "
		 "hardware partition %d @ 0x%llx", size, size, hwpart,
		 (unsigned long long)off);

	ret = failsafe_mmc_write(mmc, off, data, size);

	mmc_region_end(mmc, hwpart);
	return ret;
}

int failsafe_mmc_read_region(int hwpart, u64 off, void *buf, size_t max_len,
			     size_t *read_len)
{
	struct mmc *mmc;
	int ret;

	if (read_len)
		*read_len = 0;

	if (!buf || !max_len)
		return -EINVAL;

	ret = mmc_region_begin(hwpart, &mmc);
	if (ret)
		return ret;

	ret = failsafe_mmc_read(mmc, off, buf, max_len);

	mmc_region_end(mmc, hwpart);

	if (!ret && read_len)
		*read_len = max_len;

	return ret;
}

u64 failsafe_mmc_region_size(int hwpart)
{
	struct mmc *mmc = failsafe_mmc_get_dev();

	if (!mmc)
		return 0;

	switch (hwpart) {
	case FAILSAFE_MMC_HWPART_BOOT0:
	case FAILSAFE_MMC_HWPART_BOOT1:
		/* Both boot partitions have the same size; an SD card has
		 * none (and reports MMCPART_NOAVAILABLE). */
		if (mmc->part_config == MMCPART_NOAVAILABLE)
			return 0;

		return mmc->capacity_boot;
	case FAILSAFE_MMC_HWPART_USER:
		return mmc->capacity_user;
	default:
		return 0;
	}
}

int failsafe_mmc_erase_region(int hwpart, u64 off, u64 size)
{
	struct mmc *mmc;
	u64 limit;
	int ret;

	ret = mmc_region_begin(hwpart, &mmc);
	if (ret)
		return ret;

	limit = failsafe_mmc_region_size(hwpart);
	if (!limit || off >= limit) {
		mmc_region_end(mmc, hwpart);
		return -EINVAL;
	}

	if (!size || off + size > limit)
		size = limit - off;

	cprintln(NORMAL, "Failsafe: erasing MMC hardware partition %d @ "
		 "0x%llx (%llu bytes)", hwpart, (unsigned long long)off,
		 (unsigned long long)size);

	ret = failsafe_mmc_erase(mmc, off, (size_t)size);

	mmc_region_end(mmc, hwpart);
	return ret;
}

#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_RPMB)

u64 failsafe_mmc_rpmb_size(void)
{
	struct mmc *mmc = failsafe_mmc_get_dev();

	if (!mmc)
		return 0;

	return mmc->capacity_rpmb;
}

int failsafe_mmc_rpmb_read(u64 off, void *buf, size_t max_len,
			   size_t *read_len)
{
	struct mmc *mmc = failsafe_mmc_get_dev();
	u64 size;
	size_t done = 0;
	int ret;

	if (read_len)
		*read_len = 0;

	if (!mmc || !buf || !max_len)
		return -EINVAL;

	size = mmc->capacity_rpmb;
	if (!size)
		return -ENOTSUPP;

	if (off >= size)
		return -EINVAL;

	if (off + max_len > size)
		max_len = (size_t)(size - off);

	/* The device is addressed in whole 256 byte blocks, so a request
	 * that starts inside a block is served through a scratch block; the
	 * surplus bytes of a trailing partial block are dropped, exactly as
	 * for a normal MMC read.
	 */
	if (off % FAILSAFE_MMC_RPMB_BLOCK) {
		size_t skip = (size_t)(off % FAILSAFE_MMC_RPMB_BLOCK);
		size_t head = min_t(size_t, FAILSAFE_MMC_RPMB_BLOCK - skip,
				    max_len);
		unsigned short blk = (unsigned short)
				     (off / FAILSAFE_MMC_RPMB_BLOCK);
		u8 *block = malloc(FAILSAFE_MMC_RPMB_BLOCK);

		if (!block)
			return -ENOMEM;

		ret = mmc_rpmb_read(mmc, block, blk, 1, NULL);
		if (ret != 1) {
			free(block);
			return -EIO;
		}

		memcpy(buf, block + skip, head);
		free(block);
		done = head;
	}

	/* The whole blocks go out in one call: the RPMB layer loops over
	 * them internally, one exchange per block.
	 */
	if (max_len > done) {
		unsigned short blk = (unsigned short)
				     ((off + done) / FAILSAFE_MMC_RPMB_BLOCK);
		unsigned short blocks = (unsigned short)
					((max_len - done) /
					 FAILSAFE_MMC_RPMB_BLOCK);

		if (blocks) {
			ret = mmc_rpmb_read(mmc, (u8 *)buf + done, blk, blocks,
					    NULL);
			if (ret != blocks)
				return -EIO;

			done += (size_t)blocks * FAILSAFE_MMC_RPMB_BLOCK;
		}
	}

	if (read_len)
		*read_len = done;

	return 0;
}

#endif /* CONFIG_WEBUI_FAILSAFE_RPMB */

#endif /* IS_ENABLED(CONFIG_MMC) */
