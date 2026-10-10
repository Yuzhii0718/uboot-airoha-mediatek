// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Failsafe storage backend: raw MTD partitions.
 *
 * One of the three backends behind failsafe_storage_write() /
 * failsafe_storage_read() (see failsafe/bootimg/storage.c); the other two
 * are the MMC partitions (failsafe/bootimg/mmc.c) and the UBI volumes
 * (failsafe/bootimg/ubi.c).
 *
 * This is the backend the boards supported today use for the bootloader
 * stages ("bl2", "chainloader", "u-boot"): those live in a raw MTD
 * partition and nowhere else.
 *
 * The file has two layers:
 *
 *   1. the backend itself, working by partition name and writing a whole
 *      image at a time (failsafe_mtd_exists() / _capacity() / _write() /
 *      _read()), used by the storage dispatcher;
 *   2. the fine-grained range access (failsafe_mtd_read_range() and
 *      friends, see <failsafe/mtd.h>), used by the pages that manage a
 *      flash chip directly: the flash editor (modules/flash.c) and the
 *      whole-chip restore (modules/simg.c).
 *
 * The file is only built when CONFIG_MTD is enabled (see the Makefile) and
 * its callers guard their use of it, so an eMMC-only board carries no MTD
 * code at all.
 */

#include <command.h>
#include <errno.h>
#include <malloc.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <vsprintf.h>
#include <mtd.h>
#include <linux/mtd/mtd.h>

#include <failsafe/storage.h>
#include <failsafe/mtd.h>
#include <cprint.h>
#include <failsafe/error.h>

#if IS_ENABLED(CONFIG_MTD)

/*
 * Probe whether 'name' refers to a known MTD partition.
 *
 * This is also what decides between the MTD and the other write paths: if
 * an MTD partition with that name exists it is erased and written through
 * the MTD API, otherwise the target is treated as an MMC partition or a
 * UBI volume.
 */
bool failsafe_mtd_exists(const char *name)
{
	struct mtd_info *mtd;

	if (!name || !name[0])
		return false;

	mtd_probe_devices();

	mtd = get_mtd_device_nm(name);
	if (IS_ERR_OR_NULL(mtd))
		return false;

	put_mtd_device(mtd);
	return true;
}

/*
 * Capacity check: the image, including the write offset (the Airoha BL2 is
 * written 0x800 bytes into the "bl2" partition), must fit entirely inside
 * the target MTD partition.
 *
 * This is the single size gate for the MTD-backed bootloader targets
 * (bl2 / u-boot / chainloader): the image size must not exceed the MTD
 * partition size.  There are no fixed image size limits.
 */
int failsafe_mtd_capacity(const char *name, u64 off, size_t size)
{
	struct mtd_info *mtd;

	mtd_probe_devices();
	mtd = get_mtd_device_nm(name);
	if (IS_ERR_OR_NULL(mtd)) {
		return failsafe_error(-ENODEV, "MTD partition '%s' not found",
			name);
	}

	if (off + (u64)size > mtd->size) {
		put_mtd_device(mtd);
		return failsafe_error(-EFBIG, "image (%zu) exceeds partition "
			"'%s' (%llu), write offset 0x%llx", size, name,
			(unsigned long long)mtd->size, (unsigned long long)off);
	}

	put_mtd_device(mtd);
	return 0;
}

int failsafe_mtd_write(const char *name, u64 off, const void *data,
		       size_t size)
{
	char cmd[256];
	int ret;

	snprintf(cmd, sizeof(cmd), "mtd erase %s", name);
	ret = run_command(cmd, 0);
	if (ret) {
		return failsafe_error(-EIO, "erase '%s' failed (ret=%d)", name,
			ret);
	}

	snprintf(cmd, sizeof(cmd), "mtd write %s 0x%lx 0x%llx 0x%lx",
		 name, (unsigned long)(uintptr_t)data,
		 (unsigned long long)off, (unsigned long)size);
	ret = run_command(cmd, 0);
	if (ret) {
		return failsafe_error(-EIO, "write '%s' failed (ret=%d)", name,
			ret);
	}

	return 0;
}

int failsafe_mtd_read(const char *name, u64 off, void *buf, size_t max_len,
		      size_t *read_len)
{
	struct mtd_info *mtd;
	size_t len, retlen = 0;
	int ret;

	mtd_probe_devices();
	mtd = get_mtd_device_nm(name);
	if (IS_ERR_OR_NULL(mtd))
		return -ENODEV;

	if (off >= mtd->size) {
		put_mtd_device(mtd);
		return -EINVAL;
	}

	len = mtd->size - off;
	if (len > max_len)
		len = max_len;

	ret = mtd_read(mtd, off, len, &retlen, buf);
	put_mtd_device(mtd);

	/* -EUCLEAN only reports corrected bit flips: the data is good. */
	if (ret && ret != -EUCLEAN)
		return -EIO;

	if (read_len)
		*read_len = retlen;

	return 0;
}

/* ------------------------------------------------------------------ */
/*  Fine-grained range access (see <failsafe/mtd.h>)                   */
/* ------------------------------------------------------------------ */

int failsafe_mtd_read_range(struct mtd_info *mtd, u64 off, size_t len,
			    u8 *buf, size_t *out_len)
{
	size_t retlen = 0;
	int ret;

	if (!mtd || !buf || !len)
		return -EINVAL;

	ret = mtd_read(mtd, off, len, &retlen, buf);

	/* -EUCLEAN only reports corrected bit flips: the data is good. */
	if (ret && ret != -EUCLEAN)
		return -EIO;

	if (out_len)
		*out_len = retlen;

	return 0;
}

int failsafe_mtd_program_range(struct mtd_info *mtd, u64 off,
			       const u8 *data, size_t len)
{
	size_t write_sz, done = 0;

	if (!mtd || !data || !len)
		return -EINVAL;

	/* One write unit (page) per call; a device that reports none is
	 * programmed in a single call.
	 */
	write_sz = mtd->writesize ? mtd->writesize : len;

	while (done < len) {
		size_t step = min_t(size_t, write_sz, len - done);
		size_t retlen = 0;
		int ret;

		ret = mtd_write(mtd, off + done, step, &retlen, data + done);
		if (ret)
			return ret;
		if (retlen != step)
			return -EIO;

		done += retlen;
	}

	return 0;
}

int failsafe_mtd_erase_blocks(struct mtd_info *mtd, u64 start, u64 len)
{
	u64 erase_sz, block_start, block_end, blk;

	if (!mtd || !len)
		return -EINVAL;

	erase_sz = mtd->erasesize;
	if (!erase_sz)
		return -EINVAL;

	/* The MTD erase API works on whole blocks, so the range is rounded
	 * outwards; blocks outside it are not touched.
	 */
	block_start = start & ~(erase_sz - 1);
	block_end = (start + len + erase_sz - 1) & ~(erase_sz - 1);

	/* One block per call, the bad ones skipped, which is what
	 * "mtd erase" does and the only thing that can work: a NAND driver
	 * walks a range erase internally and stops at the first bad block
	 * with -EIO - and it does so silently, its only message being a
	 * debug one - so asking for a whole partition in one call fails on
	 * every device that has a single bad block.
	 */
	for (blk = block_start; blk < block_end; blk += erase_sz) {
		struct erase_info ei;
		int bad, ret;

		bad = mtd_block_isbad(mtd, blk);
		if (bad < 0)
			return bad;

		/* Nothing to erase in a block the driver refuses to erase:
		 * its content is already unreadable. */
		if (bad)
			continue;

		memset(&ei, 0, sizeof(ei));
		ei.mtd = mtd;
		ei.addr = blk;
		ei.len = erase_sz;

		ret = mtd_erase(mtd, &ei);
		if (ret)
			return ret;
	}

	return 0;
}

int failsafe_mtd_update_range(struct mtd_info *mtd, u64 start,
			      const u8 *data, size_t len, size_t *skipped)
{
	u64 block_start, block_end, blk;
	size_t erase_sz, lost = 0;
	u8 *blkbuf;
	int ret = 0;

	if (!mtd || !data || !len)
		return -EINVAL;

	erase_sz = mtd->erasesize;
	if (!erase_sz)
		return -EINVAL;

	block_start = start & ~((u64)erase_sz - 1);
	block_end = (start + len + erase_sz - 1) & ~((u64)erase_sz - 1);

	blkbuf = malloc(erase_sz);
	if (!blkbuf)
		return -ENOMEM;

	for (blk = block_start; blk < block_end; blk += erase_sz) {
		size_t readsz = 0;
		u64 data_start = max(start, blk);
		u64 data_end = min(start + (u64)len, blk + (u64)erase_sz);
		size_t copy_len = (size_t)(data_end - data_start);
		int bad;

		/* A bad block can neither be read back nor written, so the
		 * bytes of @data that fall into it are lost - the rest of the
		 * update still happens instead of failing the whole write.
		 */
		bad = mtd_block_isbad(mtd, blk);
		if (bad < 0) {
			ret = bad;
			goto out;
		}

		if (bad) {
			lost += copy_len;
			continue;
		}

		ret = failsafe_mtd_read_range(mtd, blk, erase_sz, blkbuf,
					      &readsz);
		if (ret || readsz != erase_sz) {
			ret = ret ? ret : -EIO;
			goto out;
		}

		if (copy_len)
			memcpy(blkbuf + (data_start - blk),
			       data + (size_t)(data_start - start), copy_len);

		ret = failsafe_mtd_erase_blocks(mtd, blk, erase_sz);
		if (ret)
			goto out;

		ret = failsafe_mtd_program_range(mtd, blk, blkbuf, erase_sz);
		if (ret)
			goto out;
	}

out:
	free(blkbuf);

	if (skipped)
		*skipped = lost;

	return ret;
}

int failsafe_mtd_restore_range(struct mtd_info *mtd, u64 start,
			       const u8 *data, size_t len, size_t *skipped)
{
	u64 block_start, block_end, blk;
	size_t erase_sz, lost = 0;
	int ret = 0;

	if (!mtd || !data || !len)
		return -EINVAL;

	erase_sz = mtd->erasesize;
	if (!erase_sz)
		return -EINVAL;

	block_start = start & ~((u64)erase_sz - 1);
	block_end = (start + len + erase_sz - 1) & ~((u64)erase_sz - 1);

	/* Block by block, so that a bad block only costs its own share of
	 * the payload: the bytes around it are still written.
	 */
	for (blk = block_start; blk < block_end; blk += erase_sz) {
		u64 data_start = max(start, blk);
		u64 data_end = min(start + (u64)len, blk + (u64)erase_sz);
		size_t copy_len = (size_t)(data_end - data_start);
		int bad;

		bad = mtd_block_isbad(mtd, blk);
		if (bad < 0) {
			ret = bad;
			goto out;
		}

		if (bad) {
			lost += copy_len;
			continue;
		}

		ret = failsafe_mtd_erase_blocks(mtd, blk, erase_sz);
		if (ret)
			goto out;

		if (!copy_len)
			continue;

		ret = failsafe_mtd_program_range(mtd, data_start,
						 data + (size_t)(data_start -
								 start),
						 copy_len);
		if (ret)
			goto out;
	}

out:
	if (skipped)
		*skipped = lost;

	return ret;
}

int failsafe_mtd_erase_range(struct mtd_info *mtd, u64 start, u64 len,
			     u32 *skipped_blocks)
{
	u64 block_start, block_end, blk;
	size_t erase_sz;
	u32 skipped = 0;
	u8 *blkbuf = NULL;
	int ret = 0;

	if (!mtd || !len)
		return -EINVAL;

	erase_sz = mtd->erasesize;
	if (!erase_sz)
		return -EINVAL;

	block_start = start & ~((u64)erase_sz - 1);
	block_end = (start + len + erase_sz - 1) & ~((u64)erase_sz - 1);

	for (blk = block_start; blk < block_end; blk += erase_sz) {
		u64 data_start = max(start, blk);
		u64 data_end = min(start + len, blk + (u64)erase_sz);
		bool full_block = (data_start == blk) &&
				  (data_end == blk + (u64)erase_sz);
		size_t readsz = 0;
		int bad;

		/* A bad block is not erased and is counted: the check has to
		 * come before the read of a partially covered block below,
		 * which is what would fail on one. */
		bad = mtd_block_isbad(mtd, blk);
		if (bad < 0) {
			ret = bad;
			goto out;
		}

		if (bad) {
			skipped++;
			continue;
		}

		if (full_block) {
			ret = failsafe_mtd_erase_blocks(mtd, blk, erase_sz);
			if (ret)
				goto out;
			continue;
		}

		/* Partially covered block: read it back, blank the
		 * requested bytes and write the block out again.
		 */
		if (!blkbuf) {
			blkbuf = malloc(erase_sz);
			if (!blkbuf) {
				ret = -ENOMEM;
				goto out;
			}
		}

		ret = failsafe_mtd_read_range(mtd, blk, erase_sz, blkbuf,
					      &readsz);
		if (ret || readsz != erase_sz) {
			ret = ret ? ret : -EIO;
			goto out;
		}

		memset(blkbuf + (size_t)(data_start - blk), 0xff,
		       (size_t)(data_end - data_start));

		ret = failsafe_mtd_erase_blocks(mtd, blk, erase_sz);
		if (ret)
			goto out;

		ret = failsafe_mtd_program_range(mtd, blk, blkbuf, erase_sz);
		if (ret)
			goto out;
	}

out:
	free(blkbuf);

	if (skipped_blocks)
		*skipped_blocks = skipped;

	return ret;
}

#endif /* CONFIG_MTD */
