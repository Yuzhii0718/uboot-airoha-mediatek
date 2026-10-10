/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project bl-mt798x-dhcpd
 * You may not use, copy, modify or distribute this file except in compliance with the license agreement.
 *
 * Failsafe flash management
 *
 * Single module merging the former "backup download" and "flash editor"
 * modules: both operated on the same raw storage targets and shared the
 * very same target resolution code, so they are presented here as one
 * page (flash.html):
 *
 *   - chip / partition identification   GET  /flash/info
 *   - backup download (streamed)        POST /flash/backup
 *   - hex read (chunked)                POST /flash/read
 *   - hex write                         POST /flash/write
 *   - restore a backup file             POST /flash/restore
 *   - erase a partition / range         POST /flash/erase
 *
 * The page picks a target itself (an MTD partition, an MMC partition or a
 * whole MMC device) and then reads, erases and programs byte ranges of it:
 * the MTD side of that is the shared range API in <failsafe/mtd.h>, the
 * MMC side the helpers in <failsafe/mmc.h>.  The board-level storage
 * targets (the ones the firmware upgrade writes to) go through
 * failsafe_storage_*() instead.
 */

#include <errno.h>
#include <malloc.h>
#include <limits.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/err.h>
#include <vsprintf.h>
#include <net/mtk_httpd.h>

#ifdef CONFIG_MTD
#include <mtd.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/spi-nor.h>
#include <linux/mtd/spinand.h>
#include <failsafe/mtd.h>
#endif

#include <failsafe/mmc.h>
#include <cprint.h>
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_FLASH_LAYOUT)
#include <failsafe/layout.h>
#endif

#ifdef CONFIG_PARTITIONS
#include <part.h>
#endif

#include <failsafe/internal.h>

#ifdef CONFIG_WEBUI_FAILSAFE_NAND_RAW
#include <failsafe/nand_raw.h>
DECLARE_GLOBAL_DATA_PTR;
#endif

/* Max bytes to read per /flash/read request in chunked mode.
 * Each chunk is hex-encoded (3x expansion) + JSON overhead,
 * so 256 KiB -> ~770 KiB JSON, safe for U-Boot's heap.
 */
#define FLASH_READ_CHUNK	(256 * 1024)

/* Maximum number of MTD devices probed while looking for the master. */
#define FLASH_MTD_MAX_DEVICES	64

/* JSON buffer for the device description. */
#define FLASH_INFO_BUF_SZ	16384

/* JSON buffer of GET /flash/layouts: the layouts of the board device tree,
 * bounded by FAILSAFE_LAYOUT_MAX * FAILSAFE_LAYOUT_MAX_PARTS entries. */
#define FLASH_LAYOUTS_BUF_SZ	(16 * 1024)

/* ------------------------------------------------------------------ */
/*  Storage target abstraction (MTD / MMC)                             */
/* ------------------------------------------------------------------ */

/*
 * A storage target is one of:
 *
 *   FAILSAFE_SRC_MTD         a raw MTD partition;
 *   FAILSAFE_SRC_MMC         an MMC partition (GPT / MBR label), or the
 *                            whole user area when the target is "raw";
 *   FAILSAFE_SRC_MMC_HWPART  a hardware partition of an eMMC - boot0 or
 *                            boot1 - addressed without any partition
 *                            table, the same regions the board code uses
 *                            for a preloader that lives there;
 *   FAILSAFE_SRC_RPMB        the eMMC replay protected memory block, read
 *                            only (CONFIG_WEBUI_FAILSAFE_RPMB).
 *
 * Where the first byte of an MMC partition is depends on the partition
 * table, so @base carries it and the operations add it to their offset;
 * the hardware partitions and the RPMB are addressed from 0.
 *
 * A partition of a device-tree layout (see <failsafe/layout.h>) is the same
 * idea applied to the raw device: @base is the offset the layout gives it,
 * @size its length, and @layout the layout it belongs to, so a backup can
 * say where it came from.  Layout targets live on the master MTD chip (that
 * is where a NAND / NOR layout is) or on the MMC user area.
 */
enum failsafe_storage_src {
	FAILSAFE_SRC_MTD = 0,
	FAILSAFE_SRC_MMC = 1,
	FAILSAFE_SRC_MMC_HWPART = 2,
	FAILSAFE_SRC_RPMB = 3,
};

struct flash_target {
	enum failsafe_storage_src src;
	const char *layout;	/* layout it was opened in, NULL otherwise */
	u64 base;
	u64 size;
#ifdef CONFIG_MTD
	struct mtd_info *mtd;
#endif
#if IS_ENABLED(CONFIG_MMC)
	int hwpart;		/* FAILSAFE_MMC_HWPART_* */
	struct mmc *mmc;
	struct disk_partition dpart;
#endif
};

/* ------------------------------------------------------------------ */
/*  MTD target helpers                                                 */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_MTD
/*
 * First master MTD device, i.e. the raw chip (partitions have ->parent
 * set).  Returns a referenced device the caller must put_mtd_device().
 */
static struct mtd_info *flash_mtd_master(void)
{
	struct mtd_info *mtd;
	u32 i;

	mtd_probe_devices();

	for (i = 0; i < FLASH_MTD_MAX_DEVICES; i++) {
		mtd = get_mtd_device(NULL, i);
		if (IS_ERR(mtd))
			continue;

		if (!mtd->parent)
			return mtd;

		put_mtd_device(mtd);
	}

	return NULL;
}

static bool flash_mtd_part_exists(const char *name)
{
	struct mtd_info *mtd;

	if (!name || !*name)
		return false;

	mtd_probe_devices();
	mtd = get_mtd_device_nm(name);
	if (IS_ERR_OR_NULL(mtd))
		return false;

	put_mtd_device(mtd);
	return true;
}

#if IS_ENABLED(CONFIG_MTD_SPI_NAND)
static bool flash_mtd_is_spinand(struct mtd_info *mtd)
{
	struct udevice *dev;

	if (!mtd)
		return false;

	dev = mtd->dev;

	return dev && dev->driver && dev->driver->name &&
	       !strcmp(dev->driver->name, "spi_nand");
}

static const struct spinand_info *
flash_spinand_match_info(struct spinand_device *spinand)
{
	size_t i;
	const struct spinand_manufacturer *manufacturer;
	const u8 *id;

	if (!spinand)
		return NULL;

	manufacturer = spinand->manufacturer;
	if (!manufacturer || !manufacturer->chips || !manufacturer->nchips)
		return NULL;

	id = spinand->id.data;

	for (i = 0; i < manufacturer->nchips; i++) {
		const struct spinand_info *info = &manufacturer->chips[i];

		if (!info->devid.id || !info->devid.len)
			continue;

		/* spinand->id.data[0] is the manufacturer ID, the device ID
		 * starts at [1]. */
		if (spinand->id.len < (int)(1 + info->devid.len))
			continue;

		if (!memcmp(id + 1, info->devid.id, info->devid.len))
			return info;
	}

	return NULL;
}
#endif /* CONFIG_MTD_SPI_NAND */

/*
 * Human readable chip model: the SPI NOR / SPI NAND driver name, with the
 * MTD device name as a last resort.
 */
static const char *flash_mtd_chip_model(struct mtd_info *mtd, char *out,
					size_t out_sz)
{
	if (!out || !out_sz)
		return "";

	out[0] = '\0';

	if (!mtd)
		return "";

	/* SPI NOR: mtd->priv points to struct spi_nor (see spi-nor-core.c). */
	if (mtd->type == MTD_NORFLASH) {
		struct spi_nor *nor = mtd->priv;

		if (nor && nor->name && nor->name[0]) {
			snprintf(out, out_sz, "%s", nor->name);
			return out;
		}
	}

#if IS_ENABLED(CONFIG_MTD_SPI_NAND)
	/* SPI NAND: mtd->priv points to the struct nand_device embedded in
	 * struct spinand_device - but only for the generic SPI NAND driver,
	 * see flash_mtd_is_spinand(). */
	if ((mtd->type == MTD_NANDFLASH || mtd->type == MTD_MLCNANDFLASH) &&
	    flash_mtd_is_spinand(mtd)) {
		struct spinand_device *spinand = mtd_to_spinand(mtd);
		const struct spinand_manufacturer *manufacturer;
		const struct spinand_info *info;
		const char *mname = NULL;
		const char *model = NULL;

		if (spinand) {
			manufacturer = spinand->manufacturer;
			info = flash_spinand_match_info(spinand);

			if (manufacturer && manufacturer->name &&
			    manufacturer->name[0])
				mname = manufacturer->name;
			if (info && info->model && info->model[0])
				model = info->model;

			if (mname && model) {
				snprintf(out, out_sz, "%s %s", mname, model);
				return out;
			}
			if (model) {
				snprintf(out, out_sz, "%s", model);
				return out;
			}
			if (mname) {
				snprintf(out, out_sz, "%s", mname);
				return out;
			}
		}
	}
#endif

	if (mtd->name && mtd->name[0]) {
		snprintf(out, out_sz, "%s", mtd->name);
		return out;
	}

	return "";
}
#else /* !CONFIG_MTD */
/*
 * Without MTD support there is no MTD partition to find, so the "auto"
 * storage selection only has the MMC branches left to try.  The stub is
 * needed because flash_open_target() asks this before it knows which kind
 * of storage the target is.
 */
static bool flash_mtd_part_exists(const char *name)
{
	(void)name;
	return false;
}
#endif /* CONFIG_MTD */

/* ------------------------------------------------------------------ */
/*  Storage target helpers                                             */
/* ------------------------------------------------------------------ */

static int parse_u64_len(const char *s, u64 *out)
{
	char *end;
	unsigned long long v;

	if (!s || !*s || !out)
		return -EINVAL;

	v = simple_strtoull(s, &end, 0);
	if (end == s)
		return -EINVAL;

	while (*end == ' ' || *end == '\t')
		end++;

	if (!*end) {
		*out = (u64)v;
		return 0;
	}

	if (!strcasecmp(end, "k") || !strcasecmp(end, "kb") ||
	    !strcasecmp(end, "kib")) {
		*out = (u64)v * 1024ULL;
		return 0;
	}

	return -EINVAL;
}

#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_FLASH_LAYOUT)
/* Defined after flash_close_target(): opens a partition of a device-tree
 * layout, which is a range of the raw device (see <failsafe/layout.h>). */
static int flash_open_layout_target(const char *layout_name,
				    const char *part_name,
				    struct flash_target *t);
#endif

static int flash_open_target(const char *storage_sel, const char *target_name,
			     const char *layout_name, struct flash_target *t)
{
	if (!storage_sel || !target_name || !t)
		return -EINVAL;

	memset(t, 0, sizeof(*t));

#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_FLASH_LAYOUT)
	/* A layout target is named by the layout and by the partition label
	 * inside it, not by a partition table entry. */
	if (layout_name && *layout_name)
		return flash_open_layout_target(layout_name, target_name, t);
#else
	(void)layout_name;
#endif

	if (!strcasecmp(storage_sel, "mtd") ||
	    (!strcasecmp(storage_sel, "auto") &&
	     flash_mtd_part_exists(target_name))) {
#ifdef CONFIG_MTD
		t->mtd = get_mtd_device_nm(target_name);
		if (IS_ERR_OR_NULL(t->mtd)) {
			t->mtd = NULL;
			return -ENODEV;
		}

		t->src = FAILSAFE_SRC_MTD;
		t->base = 0;
		t->size = t->mtd->size;
		return 0;
#else
		return -ENODEV;
#endif
	}

#if IS_ENABLED(CONFIG_MMC)
	t->mmc = failsafe_mmc_get_dev();
	if (!t->mmc)
		return -ENODEV;

	t->src = FAILSAFE_SRC_MMC;
	t->hwpart = FAILSAFE_MMC_HWPART_USER;
	if (!strcmp(target_name, "raw")) {
		t->base = 0;
		t->size = t->mmc->capacity_user;
		return 0;
	}

	/* eMMC hardware partitions: no partition table, no name lookup -
	 * the boot ROM of a board that boots from there reads them the
	 * same way.  An SD card has none, which the size check catches.
	 */
	if (!strcmp(target_name, "boot0") || !strcmp(target_name, "boot1")) {
		t->src = FAILSAFE_SRC_MMC_HWPART;
		t->hwpart = !strcmp(target_name, "boot0") ?
			    FAILSAFE_MMC_HWPART_BOOT0 :
			    FAILSAFE_MMC_HWPART_BOOT1;
		t->base = 0;
		t->size = failsafe_mmc_region_size(t->hwpart);
		if (!t->size)
			return -ENOTSUPP;

		return 0;
	}

#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_RPMB)
	if (!strcmp(target_name, "rpmb")) {
		t->src = FAILSAFE_SRC_RPMB;
		t->base = 0;
		t->size = failsafe_mmc_rpmb_size();
		if (!t->size)
			return -ENOTSUPP;

		return 0;
	}
#endif /* CONFIG_WEBUI_FAILSAFE_RPMB */

	if (failsafe_mmc_find_part(t->mmc, target_name, &t->dpart))
		return -ENODEV;

	t->base = (u64)t->dpart.start * t->dpart.blksz;
	t->size = (u64)t->dpart.size * t->dpart.blksz;
	return 0;
#else
	return -ENODEV;
#endif
}

static void flash_close_target(struct flash_target *t)
{
	if (!t)
		return;
#ifdef CONFIG_MTD
	if (t->mtd)
		put_mtd_device(t->mtd);
	t->mtd = NULL;
#endif
}

#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_FLASH_LAYOUT)
/*
 * Open a partition of a device-tree layout (see <failsafe/layout.h>).
 *
 * A layout describes raw device offsets, so such a target is a range of the
 * raw device rather than a partition of a partition table: the master MTD
 * chip when the board has one - that is where a NAND / NOR layout lives -
 * and the user area of an MMC device otherwise.  The offset of the partition
 * goes into @t->base and its length into @t->size, which is what bounds every
 * operation on it: editing a layout partition can never reach into its
 * neighbours, and a partition whose layout says "size 0" ends where the
 * device ends.
 */
static int flash_open_layout_target(const char *layout_name,
				    const char *part_name,
				    struct flash_target *t)
{
	const struct failsafe_layout_part *part = NULL;
	const struct failsafe_layout *layout = NULL;
	struct failsafe_layout *layouts;
	bool have_mtd = false;
	u64 capacity = 0;
	int count, i, ret = -ENODEV;

	layouts = malloc(FAILSAFE_LAYOUT_MAX * sizeof(*layouts));
	if (!layouts)
		return -ENOMEM;

	count = failsafe_layout_parse(layouts, FAILSAFE_LAYOUT_MAX);
	for (i = 0; i < count; i++) {
		if (!strcmp(layouts[i].label, layout_name)) {
			layout = &layouts[i];
			break;
		}
	}

	if (layout)
		part = failsafe_layout_find_part(layout, part_name);

	if (!part)
		goto out;

#if IS_ENABLED(CONFIG_MTD)
	t->mtd = flash_mtd_master();
	if (t->mtd) {
		t->src = FAILSAFE_SRC_MTD;
		capacity = t->mtd->size;
		have_mtd = true;
	}
#endif

	if (!have_mtd) {
#if IS_ENABLED(CONFIG_MMC)
		t->mmc = failsafe_mmc_get_dev();
		if (!t->mmc)
			goto out;

		t->src = FAILSAFE_SRC_MMC;
		t->hwpart = FAILSAFE_MMC_HWPART_USER;
		capacity = t->mmc->capacity_user;
#else
		goto out;
#endif
	}

	if (part->offset >= capacity)
		goto out;

	t->base = part->offset;
	t->size = part->size ? part->size : capacity - part->offset;
	if (t->size > capacity - t->base)
		t->size = capacity - t->base;

	/* The label points into the control device tree, which stays mapped,
	 * so it outlives the array freed below - that is what lets a backup
	 * file name say which layout it was taken from. */
	t->layout = layout->label;

	ret = 0;
out:
	free(layouts);

	if (ret)
		flash_close_target(t);

	return ret;
}
#endif /* CONFIG_WEBUI_FAILSAFE_FLASH_LAYOUT */


/* ------------------------------------------------------------------ */
/*  Target operations                                                  */
/* ------------------------------------------------------------------ */

/*
 * One place per operation, so that every handler supports every target:
 * the backends differ only in the call they make, and keeping that in one
 * function is what keeps the hex editor, the streamed backup, the restore
 * path and the erase path in sync - an MMC hardware partition, for
 * instance, is read and written through the region helpers, not through a
 * partition lookup.
 */

/* Read @len bytes of @t at @off.  A short read is reported through
 * @read_len and treated as an error by the callers.
 */
static int flash_target_read(const struct flash_target *t, u64 off, void *buf,
			     size_t len, size_t *read_len)
{
	if (read_len)
		*read_len = 0;

	switch (t->src) {
	case FAILSAFE_SRC_MTD:
#ifdef CONFIG_MTD
		return failsafe_mtd_read_range(t->mtd, t->base + off, len,
					      buf, read_len);
#else
		return -ENODEV;
#endif
	case FAILSAFE_SRC_MMC:
#if IS_ENABLED(CONFIG_MMC)
		if (failsafe_mmc_read(t->mmc, t->base + off, buf, len))
			return -EIO;

		if (read_len)
			*read_len = len;

		return 0;
#else
		return -ENODEV;
#endif
	case FAILSAFE_SRC_MMC_HWPART:
#if IS_ENABLED(CONFIG_MMC)
		return failsafe_mmc_read_region(t->hwpart, off, buf, len,
						read_len);
#else
		return -ENODEV;
#endif
	case FAILSAFE_SRC_RPMB:
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_RPMB)
		return failsafe_mmc_rpmb_read(off, buf, len, read_len);
#else
		return -ENOTSUPP;
#endif
	}

	return -EINVAL;
}

/*
 * Write @len bytes to @t at @off.  @erase_first selects how an MTD target
 * is programmed: a whole backup covering the range erases it first, while
 * the hex editor patches it block by block and leaves the rest alone.  An
 * MMC device overwrites in place, so both are the same write there.
 *
 * @skipped (may be NULL) receives the number of bytes that were not written
 * because they fall into a bad block; a device that cannot have any (an MMC
 * partition) always reports 0.
 */
static int flash_target_write(const struct flash_target *t, u64 off,
			      const void *buf, size_t len, bool erase_first,
			      size_t *skipped)
{
	if (skipped)
		*skipped = 0;

	switch (t->src) {
	case FAILSAFE_SRC_MTD:
#ifdef CONFIG_MTD
		if (erase_first)
			return failsafe_mtd_restore_range(t->mtd,
							  t->base + off,
							  buf, len, skipped);

		return failsafe_mtd_update_range(t->mtd, t->base + off, buf,
						 len, skipped);
#else
		return -ENODEV;
#endif
	case FAILSAFE_SRC_MMC:
#if IS_ENABLED(CONFIG_MMC)
		return failsafe_mmc_write(t->mmc, t->base + off, buf, len);
#else
		return -ENODEV;
#endif
	case FAILSAFE_SRC_MMC_HWPART:
#if IS_ENABLED(CONFIG_MMC)
		return failsafe_mmc_write_region(t->hwpart, off, buf, len);
#else
		return -ENODEV;
#endif
	case FAILSAFE_SRC_RPMB:
		/* Writing the RPMB has to be authenticated with the key the
		 * device was programmed with, which a bootloader does not
		 * have - see the Kconfig help of CONFIG_WEBUI_FAILSAFE_RPMB.
		 */
		cprintln(ERROR, "Failsafe: the RPMB partition is read only "
			 "(writing it needs its authentication key)");
		return -ENOTSUPP;
	}

	return -EINVAL;
}

/*
 * Erase [off, off + len) of @t.  @skipped_blocks (may be NULL) receives the
 * number of bad blocks that were not erased.
 */
static int flash_target_erase(const struct flash_target *t, u64 off, u64 len,
			      u32 *skipped_blocks)
{
	if (skipped_blocks)
		*skipped_blocks = 0;

	switch (t->src) {
	case FAILSAFE_SRC_MTD:
#ifdef CONFIG_MTD
		return failsafe_mtd_erase_range(t->mtd, t->base + off, len,
						skipped_blocks);
#else
		return -ENODEV;
#endif
	case FAILSAFE_SRC_MMC:
#if IS_ENABLED(CONFIG_MMC)
		return failsafe_mmc_erase(t->mmc, t->base + off, (size_t)len);
#else
		return -ENODEV;
#endif
	case FAILSAFE_SRC_MMC_HWPART:
#if IS_ENABLED(CONFIG_MMC)
		return failsafe_mmc_erase_region(t->hwpart, off, len);
#else
		return -ENODEV;
#endif
	case FAILSAFE_SRC_RPMB:
		cprintln(ERROR, "Failsafe: the RPMB partition is read only");
		return -ENOTSUPP;
	}

	return -EINVAL;
}

/*
 * Extract the storage type and target name from a request.  A
 * "mtd:" / "mmc:" prefix on the target overrides the "storage" value, so
 * the front end can always send the fully qualified target string.
 */
static int flash_parse_storage_target(struct httpd_request *request,
				      char *storage_sel, size_t storage_sz,
				      char *target_name, size_t target_sz,
				      char *layout_name, size_t layout_sz)
{
	struct httpd_form_value *storage, *target;
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_FLASH_LAYOUT)
	struct httpd_form_value *layout;
#endif

	if (!request || !storage_sel || !target_name)
		return -EINVAL;

	if (layout_name && layout_sz)
		layout_name[0] = '\0';

	storage = httpd_request_find_value(request, "storage");
	target = httpd_request_find_value(request, "target");
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_FLASH_LAYOUT)
	layout = httpd_request_find_value(request, "layout");

	/* The layout a target belongs to is independent of the storage
	 * selection: it names a partition scheme of the raw device instead of
	 * a partition table entry (see <failsafe/layout.h>). */
	if (layout && layout->data && layout_name)
		strlcpy(layout_name, layout->data, layout_sz);
#endif

	if (storage && storage->data)
		strlcpy(storage_sel, storage->data, storage_sz);

	if (target && target->data)
		strlcpy(target_name, target->data, target_sz);

	if (!strncmp(target_name, "mtd:", 4)) {
		memmove(target_name, target_name + 4,
			strlen(target_name + 4) + 1);
		strlcpy(storage_sel, "mtd", storage_sz);
	} else if (!strncmp(target_name, "mmc:", 4)) {
		memmove(target_name, target_name + 4,
			strlen(target_name + 4) + 1);
		strlcpy(storage_sel, "mmc", storage_sz);
	}

	return target_name[0] ? 0 : -EINVAL;
}

/* ------------------------------------------------------------------ */
/*  Generic range / hex helpers                                        */
/* ------------------------------------------------------------------ */

static int flash_parse_start_end(const char *start_s, const char *end_s,
				 u64 *start, u64 *end)
{
	if (!start_s || !end_s || !start || !end)
		return -EINVAL;

	if (parse_u64_len(start_s, start))
		return -EINVAL;
	if (parse_u64_len(end_s, end))
		return -EINVAL;
	if (*end <= *start)
		return -ERANGE;

	return 0;
}

static int flash_parse_hex(const char *in, u8 **out, size_t *out_len)
{
	size_t digits = 0, i = 0, o = 0, bytes;
	u8 *buf;
	int high = -1;

	if (!in || !out || !out_len)
		return -EINVAL;

	*out = NULL;
	*out_len = 0;

	while (in[i]) {
		if (in[i] == '0' && (in[i + 1] == 'x' || in[i + 1] == 'X')) {
			i += 2;
			continue;
		}
		if (isxdigit((unsigned char)in[i]))
			digits++;
		i++;
	}

	if (!digits || (digits & 1))
		return -EINVAL;

	bytes = digits / 2;

	buf = malloc(bytes);
	if (!buf)
		return -ENOMEM;

	for (i = 0; in[i]; i++) {
		int v;

		if (in[i] == '0' && (in[i + 1] == 'x' || in[i + 1] == 'X')) {
			i++;
			high = -1;
			continue;
		}

		if (!isxdigit((unsigned char)in[i]))
			continue;

		if (in[i] >= '0' && in[i] <= '9')
			v = in[i] - '0';
		else if (in[i] >= 'a' && in[i] <= 'f')
			v = in[i] - 'a' + 10;
		else
			v = in[i] - 'A' + 10;

		if (high < 0) {
			high = v;
		} else {
			buf[o++] = (u8)((high << 4) | v);
			high = -1;
		}
	}

	if (o != bytes) {
		free(buf);
		return -EINVAL;
	}

	*out = buf;
	*out_len = bytes;
	return 0;
}

static char *flash_hex_dump(const u8 *data, size_t len, size_t *out_len)
{
	static const char hex[] = "0123456789abcdef";
	size_t i, cap;
	char *out;

	if (!data || !out_len)
		return NULL;

	cap = len * 3 + 8;
	out = malloc(cap);
	if (!out)
		return NULL;

	for (i = 0; i < len; i++) {
		out[i * 3] = hex[(data[i] >> 4) & 0xf];
		out[i * 3 + 1] = hex[data[i] & 0xf];
		out[i * 3 + 2] = (i + 1 == len) ? '\0' : ' ';
	}
	out[len * 3] = '\0';

	*out_len = strlen(out);
	return out;
}

/* ------------------------------------------------------------------ */
/*  Backup filename parsing (used to auto-detect the restore target)   */
/* ------------------------------------------------------------------ */

static const char *flash_find_last_before(const char *s, const char *needle,
					  const char *limit)
{
	const char *p = s;
	const char *last = NULL;

	if (!s || !needle || !limit || limit <= s)
		return NULL;

	while ((p = strstr(p, needle)) != NULL) {
		if (p >= limit)
			break;
		last = p;
		p++;
	}

	return last;
}

/*
 * Decode "<storage>_<model>_<target>_0x<start>-0x<end>.bin" back into its
 * storage type, target name and byte range.
 */
static int flash_parse_backup_filename(const char *filename,
				       char *storage, size_t storage_sz,
				       char *target, size_t target_sz,
				       char *layout, size_t layout_sz,
				       u64 *start, u64 *end)
{
	const char *range, *dash, *stype_mtd, *stype_mmc, *stype;
	char *range_end = NULL;
	char tmp[128];
	size_t seg_len;

	if (!filename || !storage || !target || !start || !end)
		return -EINVAL;

	if (layout && layout_sz)
		layout[0] = '\0';

	range = strstr(filename, "_0x");
	if (!range)
		return -EINVAL;

	dash = strstr(range, "-0x");
	if (!dash)
		return -EINVAL;

	*start = simple_strtoull(range + 1, &range_end, 0);
	if (!range_end || range_end <= range + 1)
		return -EINVAL;

	*end = simple_strtoull(dash + 1, &range_end, 0);
	if (!range_end || range_end <= dash + 1)
		return -EINVAL;

	if (*end <= *start)
		return -ERANGE;

	stype_mtd = flash_find_last_before(filename, "_mtd_", range);
	stype_mmc = flash_find_last_before(filename, "_mmc_", range);
	if (stype_mtd && stype_mmc)
		stype = (stype_mtd > stype_mmc) ? stype_mtd : stype_mmc;
	else
		stype = stype_mtd ? stype_mtd : stype_mmc;

	if (!stype)
		return -EINVAL;

	if (stype == stype_mtd)
		strlcpy(storage, "mtd", storage_sz);
	else
		strlcpy(storage, "mmc", storage_sz);

	stype += 5;
	seg_len = (size_t)(range - stype);
	if (!seg_len || seg_len >= sizeof(tmp))
		return -EINVAL;

	/* A raw NAND dump carries an extra "_oob" marker before the range;
	 * it must not be mistaken for the target name. */
	if (seg_len > 4 && !memcmp(range - 4, "_oob", 4))
		seg_len -= 4;

	if (!seg_len)
		return -EINVAL;

	memcpy(tmp, stype, seg_len);
	tmp[seg_len] = '\0';

	{
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_FLASH_LAYOUT)
		char *seat = strstr(tmp, "layout-");

		/* A backup taken in a layout says which one, so a restore
		 * puts it back into the same layout: its partitions are raw
		 * device ranges, not partition table entries (see
		 * flash_layouts_handler()). */
		if (seat && (seat == tmp || seat[-1] == '_')) {
			char *stop = strchr(seat + 7, '_');

			if (stop && stop > seat + 7) {
				*stop = '\0';
				strlcpy(layout, seat + 7, layout_sz);
			}
		}
#endif /* CONFIG_WEBUI_FAILSAFE_FLASH_LAYOUT */

		char *last = strrchr(tmp, '_');
		const char *name = last ? last + 1 : tmp;

		if (!name || !name[0])
			return -EINVAL;
		if (strlen(name) >= target_sz)
			return -E2BIG;
		strlcpy(target, name, target_sz);
	}

	return 0;
}

/* "layout-<name>_" for the backup file name, or "" without a layout. */
static const char *flash_layout_segment(const char *layout)
{
	static char segment[80];

	segment[0] = '\0';
	if (layout) {
		snprintf(segment, sizeof(segment), "layout-%s_", layout);
		failsafe_str_sanitize(segment);
	}

	return segment;
}

/* ------------------------------------------------------------------ */
/*  GET /flash/info - chip identification + partitions                 */
/* ------------------------------------------------------------------ */

/**
 * flash_info_handler - GET /flash/info
 *
 * Reports the storage devices the flash page can work on:
 * {"mmc":{"present":bool,"vendor":...,"product":...,"parts":[...]},
 *  "mtd":{"present":bool,"model":"...","type":N,"parts":[...]}}
 */
void flash_info_handler(enum httpd_uri_handler_status status,
			struct httpd_request *request,
			struct httpd_response *response)
{
	char *buf;
	int len = 0;
	int left = FLASH_INFO_BUF_SZ;

	(void)request;

	if (status == HTTP_CB_CLOSED) {
		free(response->session_data);
		response->session_data = NULL;
		return;
	}

	if (status != HTTP_CB_NEW)
		return;

	buf = malloc(left);
	if (!buf) {
		failsafe_http_reply_json(response, 500, "{}");
		return;
	}

	len = buf_appendf(buf, left, len, "{");

	/* Whether this build can edit flash in a device-tree layout (see
	 * <failsafe/layout.h>): the page hides its layout picker when it
	 * cannot, the way it does for the NAND raw fields below. */
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_FLASH_LAYOUT)
	len = buf_appendf(buf, left, len, "\"layout\":true,");
#else
	len = buf_appendf(buf, left, len, "\"layout\":false,");
#endif

	/* MMC info + partitions */
	len = buf_appendf(buf, left, len, "\"mmc\":{");
#if IS_ENABLED(CONFIG_MMC)
	{
		struct mmc *mmc;
		struct blk_desc *bd;
		bool present;

		mmc = failsafe_mmc_get_dev();
		bd = failsafe_mmc_blk_desc(mmc);
		present = bd && bd->type != DEV_TYPE_UNKNOWN;

		if (present) {
			char pretty_vendor[256];
			char esc_vendor[256], esc_product[128];

			failsafe_mmc_vendor_pretty(bd->vendor, pretty_vendor,
						   sizeof(pretty_vendor));
			json_escape(esc_vendor, sizeof(esc_vendor),
				    pretty_vendor);
			json_escape(esc_product, sizeof(esc_product),
				    bd->product);
			len = buf_appendf(buf, left, len,
				"\"present\":true,\"vendor\":\"%s\","
				"\"product\":\"%s\",\"blksz\":%lu,"
				"\"size\":%llu,",
				esc_vendor, esc_product,
				(unsigned long)bd->blksz,
				(unsigned long long)mmc->capacity_user);
		} else {
			len = buf_appendf(buf, left, len, "\"present\":false,");
		}

		len = buf_appendf(buf, left, len, "\"parts\":[");
#ifdef CONFIG_PARTITIONS
		if (present) {
			struct disk_partition dpart;
			char esc_name[128];
			u32 i = 1;
			bool first = true;

			part_init(bd);
			while (len < left - 128) {
				if (part_get_info(bd, i, &dpart))
					break;

				if (!dpart.name[0]) {
					i++;
					continue;
				}

				json_escape(esc_name, sizeof(esc_name),
					    dpart.name);
				len = buf_appendf(buf, left, len,
					"%s{\"name\":\"%s\",\"size\":%llu}",
					first ? "" : ",",
					esc_name,
					(unsigned long long)dpart.size *
					dpart.blksz);

				first = false;
				i++;
			}
		}
#endif
		len = buf_appendf(buf, left, len, "]");

		/* The raw regions of the device that are not partitions: the
		 * two boot partitions of an eMMC and its RPMB.  The front end
		 * offers them as additional targets; "ro" marks the ones that
		 * can only be read (see the RPMB note in <failsafe/mmc.h>).
		 */
		len = buf_appendf(buf, left, len, ",\"regions\":[");
		if (present) {
			static const char *const boot_names[] = {
				"boot0", "boot1",
			};
			bool first = true;
			int i;

			for (i = 0; i < ARRAY_SIZE(boot_names); i++) {
				int hwpart = i ? FAILSAFE_MMC_HWPART_BOOT1 :
						 FAILSAFE_MMC_HWPART_BOOT0;
				u64 rsize = failsafe_mmc_region_size(hwpart);

				if (!rsize)
					continue;

				len = buf_appendf(buf, left, len,
						 "%s{\"name\":\"%s\","
						 "\"size\":%llu,\"ro\":false}",
						 first ? "" : ",",
						 boot_names[i],
						 (unsigned long long)rsize);
				first = false;
			}

#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_RPMB)
			{
				u64 rsize = failsafe_mmc_rpmb_size();

				if (rsize) {
					len = buf_appendf(buf, left, len,
							 "%s{\"name\":\"rpmb\","
							 "\"size\":%llu,\"ro\":true}",
							 first ? "" : ",",
							 (unsigned long long)rsize);
					first = false;
				}
			}
#endif /* CONFIG_WEBUI_FAILSAFE_RPMB */
		}
		len = buf_appendf(buf, left, len, "]");
	}
#else
	len = buf_appendf(buf, left, len,
			  "\"present\":false,\"parts\":[],\"regions\":[]");
#endif
	len = buf_appendf(buf, left, len, "},");

	/* MTD info + partitions */
	len = buf_appendf(buf, left, len, "\"mtd\":{");
#ifdef CONFIG_MTD
	{
		struct mtd_info *mtd, *sel;
		u32 i;
		bool first = true;
		const char *model = NULL;
		char model_buf[128];
		char esc_model[128];
		int type = -1;
		bool present = false;

		/* Prefer a master MTD device (mtd->parent == NULL) for the
		 * chip model, it describes the raw chip rather than a
		 * partition. */
		sel = flash_mtd_master();
		if (sel) {
			present = true;
			type = sel->type;
			model = flash_mtd_chip_model(sel, model_buf,
						     sizeof(model_buf));
			put_mtd_device(sel);
		}

		json_escape(esc_model, sizeof(esc_model), model ? model : "");
		len = buf_appendf(buf, left, len,
			"\"present\":%s,\"model\":\"%s\",\"type\":%d,",
			present ? "true" : "false",
			esc_model, type);

#ifdef CONFIG_WEBUI_FAILSAFE_NAND_RAW
		/* NAND raw metadata - re-open the master to read the info */
		{
			u64 raw_sz = 0;
			u32 oob_sz = 0, page_sz = 0;
			const char *ntype = "none";
			u64 ram_avail = gd ? gd->ram_size : 0;

			sel = flash_mtd_master();
			if (sel) {
				raw_sz = nand_raw_total_size(sel);
				if (nand_raw_is_nand(sel)) {
					oob_sz = sel->oobsize;
					page_sz = sel->writesize;
					ntype = "spi";
				}
				put_mtd_device(sel);
			}

			len = buf_appendf(buf, left, len,
				"\"nand_raw_size\":%llu,\"nand_oob_size\":%u,"
				"\"nand_page_size\":%u,\"nand_type\":\"%s\","
				"\"ram_available\":%llu,",
				(unsigned long long)raw_sz, oob_sz, page_sz,
				ntype, (unsigned long long)ram_avail);
		}
#else
		len = buf_appendf(buf, left, len,
			"\"nand_raw_size\":0,\"nand_oob_size\":0,"
			"\"nand_page_size\":0,\"nand_type\":\"none\","
			"\"ram_available\":0,");
#endif

		len = buf_appendf(buf, left, len, "\"parts\":[");
		for (i = 0; i < FLASH_MTD_MAX_DEVICES && len < left - 128; i++) {
			char esc_name[128];

			mtd = get_mtd_device(NULL, i);
			if (IS_ERR(mtd))
				continue;

			if (!mtd->name || !mtd->name[0]) {
				put_mtd_device(mtd);
				continue;
			}

			json_escape(esc_name, sizeof(esc_name), mtd->name);
			len = buf_appendf(buf, left, len,
				"%s{\"name\":\"%s\",\"size\":%llu,"
				"\"master\":%s}",
				first ? "" : ",",
				esc_name,
				(unsigned long long)mtd->size,
				mtd->parent ? "false" : "true");

			first = false;
			put_mtd_device(mtd);
		}
		len = buf_appendf(buf, left, len, "]");
	}
#else
	len = buf_appendf(buf, left, len, "\"present\":false,\"parts\":[]");
#endif
	len = buf_appendf(buf, left, len, "}");
	len = buf_appendf(buf, left, len, "}");

	failsafe_http_reply_json_alloc(response, 200, buf, buf);
}

/* ------------------------------------------------------------------ */
/*  POST /flash/backup - streamed download                             */
/* ------------------------------------------------------------------ */

enum flash_backup_phase {
	FLASH_BACKUP_PHASE_HDR = 0,
	FLASH_BACKUP_PHASE_DATA = 1,
};

struct flash_backup_session {
	enum failsafe_storage_src src;
	enum flash_backup_phase phase;

	u64 start;
	u64 end;
	u64 total;
	u64 cur;
	u64 target_size;

	bool raw;
	size_t raw_page_sz;

	char filename[128];
	char hdr[512];
	int hdr_len;

	void *buf;
	size_t buf_size;

	/* The target stays open for the whole transfer, so the stream loop
	 * only has to read from it (it owns the MTD reference, released by
	 * flash_close_target() below).
	 */
	struct flash_target tgt;
};

/**
 * flash_backup_handler - POST /flash/backup
 *
 * Form parameters:
 *   storage - "auto" (default), "mtd" or "mmc"
 *   target  - partition / device name ("mtd:xxx", "mmc:xxx" or plain)
 *   mode    - "part" (whole target) or "range" (start/end)
 *   start   - range start (mode=range)
 *   end     - range end, exclusive (mode=range)
 *   raw     - "1" to dump NAND pages including their OOB area
 *
 * Streams the requested bytes back as an octet-stream attachment.
 */
void flash_backup_handler(enum httpd_uri_handler_status status,
			  struct httpd_request *request,
			  struct httpd_response *response)
{
	struct flash_backup_session *st;
	char target_name[64] = "";
	char storage_sel[16] = "auto";
	char layout_name[64] = "";
	u64 off_start = 0, off_end = 0;
	struct flash_target tgt;
	int ret;

	if (status == HTTP_CB_CLOSED) {
		st = response->session_data;
		if (st) {
			flash_close_target(&st->tgt);
			free(st->buf);
			free(st);
		}
		response->session_data = NULL;
		return;
	}

	if (status == HTTP_CB_NEW) {
		struct httpd_form_value *mode, *start, *end, *rawv;
		bool raw_mode = false;

		mode = httpd_request_find_value(request, "mode");
		start = httpd_request_find_value(request, "start");
		end = httpd_request_find_value(request, "end");
		rawv = httpd_request_find_value(request, "raw");

		if (rawv && rawv->data && !strcmp(rawv->data, "1"))
			raw_mode = true;

		ret = flash_parse_storage_target(request, storage_sel,
						 sizeof(storage_sel),
						 target_name,
						 sizeof(target_name),
						 layout_name,
						 sizeof(layout_name));
		if (ret || !mode || !mode->data)
			goto bad;

		if (!strcmp(mode->data, "part")) {
			off_start = 0;
			off_end = ULLONG_MAX;
		} else if (!strcmp(mode->data, "range")) {
			if (!start || !end || !start->data || !end->data)
				goto bad;

			if (parse_u64_len(start->data, &off_start))
				goto bad;
			if (parse_u64_len(end->data, &off_end))
				goto bad;
		} else {
			goto bad;
		}

		st = calloc(1, sizeof(*st));
		if (!st)
			goto oom;

		st->buf_size = 64 * 1024;
		st->buf = malloc(st->buf_size);
		if (!st->buf) {
			free(st);
			goto oom;
		}

		ret = flash_open_target(storage_sel, target_name, layout_name,
					     &tgt);
		if (ret)
			goto bad_target;

		st->src = tgt.src;
		st->target_size = tgt.size;

		/* The session takes the open target over, including the MTD
		 * reference: flash_close_target() on the local copy must not
		 * release it.
		 */
		st->tgt = tgt;
#ifdef CONFIG_MTD
		tgt.mtd = NULL;
#endif
		flash_close_target(&tgt);

		/* --- Raw NAND mode initialization --- */
#ifdef CONFIG_WEBUI_FAILSAFE_NAND_RAW
		if (raw_mode && st->src == FAILSAFE_SRC_MTD) {
			if (!nand_raw_is_nand(st->tgt.mtd))
				goto bad_raw;

			st->raw = true;
			st->raw_page_sz = nand_raw_page_size(st->tgt.mtd);
			if (!st->raw_page_sz)
				goto bad_range;

			/* Align buf_size to raw_page_sz multiples */
			st->buf_size = (st->buf_size / st->raw_page_sz) *
				       st->raw_page_sz;
			if (st->buf_size < st->raw_page_sz) {
				st->buf_size = st->raw_page_sz;
				free(st->buf);
				st->buf = malloc(st->buf_size);
				if (!st->buf)
					goto bad_range;
			}

			/* Raw dumps cover the whole chip, OOB included. */
			st->target_size = nand_raw_total_size(st->tgt.mtd);
		}
#endif

		/* range normalization */
		if (off_end == ULLONG_MAX)
			off_end = st->target_size;

		if (off_start >= off_end)
			goto bad_range;
		if (off_end > st->target_size)
			goto bad_range;

		st->start = off_start;
		st->end = off_end;
		st->total = st->end - st->start;
		st->cur = 0;
		st->phase = FLASH_BACKUP_PHASE_HDR;

		/* filename */
		{
			char model[64] = "";
			const char *stype = st->src == FAILSAFE_SRC_MTD ?
					    "mtd" : "mmc";

			if (st->src == FAILSAFE_SRC_MMC) {
#if IS_ENABLED(CONFIG_MMC)
				struct blk_desc *bd =
					failsafe_mmc_blk_desc(st->tgt.mmc);

				if (bd)
					strlcpy(model, bd->product,
						sizeof(model));
#endif
			} else {
#ifdef CONFIG_MTD
				if (st->tgt.mtd && st->tgt.mtd->name)
					strlcpy(model, st->tgt.mtd->name,
						sizeof(model));
#endif
			}

			failsafe_str_sanitize(model);
			failsafe_str_sanitize(target_name);

			snprintf(st->filename, sizeof(st->filename),
				 "backup_%s_%s_%s%s%s_0x%llx-0x%llx.bin",
				 stype,
				 model[0] ? model : "device",
				 flash_layout_segment(st->tgt.layout),
				 target_name,
				 st->raw ? "_oob" : "",
				 (unsigned long long)st->start,
				 (unsigned long long)st->end);
		}

		/* The CUSTOM response must carry the HTTP header itself. */
		st->hdr_len = snprintf(st->hdr, sizeof(st->hdr),
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: application/octet-stream\r\n"
			"Content-Length: %llu\r\n"
			"Content-Disposition: attachment; filename=\"%s\"\r\n"
			"Cache-Control: no-store\r\n"
			"Connection: close\r\n"
			"\r\n",
			(unsigned long long)st->total,
			st->filename);

		response->session_data = st;
		response->status = HTTP_RESP_CUSTOM;
		response->data = st->hdr;
		response->size = st->hdr_len;
		return;
	}

	if (status == HTTP_CB_RESPONDING) {
		u64 remain;
		size_t to_read, got = 0;

		st = response->session_data;
		if (!st) {
			response->status = HTTP_RESP_NONE;
			return;
		}

		if (st->phase == FLASH_BACKUP_PHASE_HDR)
			st->phase = FLASH_BACKUP_PHASE_DATA;

		remain = st->total - st->cur;
		if (!remain) {
			response->status = HTTP_RESP_NONE;
			return;
		}

		to_read = (size_t)min_t(u64, remain, st->buf_size);

#ifdef CONFIG_WEBUI_FAILSAFE_NAND_RAW
		if (st->raw) {
			u64 first_page, pages_to_read;
			size_t actual;

			/* Align to raw page boundary */
			first_page = (st->start + st->cur) / st->raw_page_sz;
			pages_to_read = to_read / st->raw_page_sz;
			if (!pages_to_read)
				pages_to_read = 1;

			ret = nand_raw_read_pages(st->tgt.mtd, first_page,
						  pages_to_read, st->buf,
						  st->buf_size, &actual);
			if (ret)
				goto io_err;

			got = actual;
		} else
#endif
		{
			ret = flash_target_read(&st->tgt, st->start + st->cur,
						st->buf, to_read, &got);
			if (ret)
				goto io_err;
		}

		if (!got)
			goto io_err;

		st->cur += got;

		response->status = HTTP_RESP_CUSTOM;
		response->data = (const char *)st->buf;
		response->size = got;
		return;
	}

	return;

bad:
	failsafe_http_reply_text(response, 400, "bad request");
	return;

bad_target:
	free(st->buf);
	free(st);
	failsafe_http_reply_text(response, 404, "target not found");
	return;

bad_range:
	flash_close_target(&st->tgt);
	free(st->buf);
	free(st);
	failsafe_http_reply_text(response, 400, "invalid range");
	return;

#ifdef CONFIG_WEBUI_FAILSAFE_NAND_RAW
bad_raw:
	flash_close_target(&st->tgt);
	free(st->buf);
	free(st);
	failsafe_http_reply_text(response, 400,
				 "raw mode requires NAND device");
	return;
#endif

oom:
	failsafe_http_reply_text(response, 500, "no mem");
	return;

io_err:
	response->status = HTTP_RESP_NONE;
	return;
}

/* ------------------------------------------------------------------ */
/*  POST /flash/{read,write,restore,erase}                             */
/* ------------------------------------------------------------------ */

/**
 * flash_handler - read / write / restore / erase on a storage target
 *
 * The operation is taken from the "op" form value, or inferred from the
 * requested URI when it is omitted:
 *   /flash/read, /flash/write, /flash/restore, /flash/erase
 *
 * Returns JSON in every case.
 */
void flash_handler(enum httpd_uri_handler_status status,
		   struct httpd_request *request,
		   struct httpd_response *response)
{
	const char *op = NULL;
	char *json = NULL;
	char storage_sel[16] = "auto";
	char target_name[64] = "";
	char layout_name[64] = "";
	u64 start = 0, end = 0;
	int ret;

	if (status == HTTP_CB_CLOSED) {
		free(response->session_data);
		response->session_data = NULL;
		return;
	}

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_json(response, 405,
			"{\"ok\":false,\"error\":\"method\"}\n");
		return;
	}

	{
		struct httpd_form_value *opv =
			httpd_request_find_value(request, "op");

		if (opv && opv->data)
			op = opv->data;
	}

	if (!op) {
		if (request->urih && request->urih->uri) {
			const char *uri = request->urih->uri;

			if (!strcmp(uri, "/flash/read"))
				op = "read";
			else if (!strcmp(uri, "/flash/write"))
				op = "write";
			else if (!strcmp(uri, "/flash/restore"))
				op = "restore";
			else if (!strcmp(uri, "/flash/erase"))
				op = "erase";
		}
	}

	if (!op) {
		failsafe_http_reply_json(response, 400,
			"{\"ok\":false,\"error\":\"no_op\"}\n");
		return;
	}

	if (!strcmp(op, "read")) {
		struct httpd_form_value *startv, *endv, *chunkv;
#ifdef CONFIG_WEBUI_FAILSAFE_NAND_RAW
		struct httpd_form_value *rawv;
#endif
		struct flash_target tgt;
		u8 *buf = NULL;
		char *hex = NULL;
		size_t len, read_len, hex_len = 0;
		u64 read_start;
		int chunk = -1;
		bool has_chunk;
#ifdef CONFIG_WEBUI_FAILSAFE_NAND_RAW
		bool raw_mode = false;
#endif

		ret = flash_parse_storage_target(request, storage_sel,
						 sizeof(storage_sel),
						 target_name,
						 sizeof(target_name),
						 layout_name,
						 sizeof(layout_name));
		if (ret)
			goto bad_req;

		startv = httpd_request_find_value(request, "start");
		endv = httpd_request_find_value(request, "end");

		if (!startv || !endv || !startv->data || !endv->data)
			goto bad_req;

		ret = flash_parse_start_end(startv->data, endv->data,
					    &start, &end);
		if (ret)
			goto bad_range;

		len = (size_t)(end - start);
		if (!len)
			goto bad_req;

		chunkv = httpd_request_find_value(request, "chunk");
		has_chunk = chunkv && chunkv->data && chunkv->data[0];

		if (has_chunk)
			chunk = simple_strtoul(chunkv->data, NULL, 0);

		if (has_chunk) {
			read_start = start + (u64)chunk * FLASH_READ_CHUNK;
			if (chunk < 0 || read_start >= end)
				goto bad_req;
			read_len = (size_t)min((u64)FLASH_READ_CHUNK,
					       end - read_start);
		} else {
			read_start = start;
			read_len = len;
		}

		if (!read_len)
			goto bad_req;

#ifdef CONFIG_WEBUI_FAILSAFE_NAND_RAW
		rawv = httpd_request_find_value(request, "raw");
		if (rawv && rawv->data && !strcmp(rawv->data, "1"))
			raw_mode = true;
#endif

		ret = flash_open_target(storage_sel, target_name, layout_name,
					     &tgt);
		if (ret)
			goto bad_target;

		if (read_start + read_len > tgt.size) {
			flash_close_target(&tgt);
			goto bad_range;
		}

#ifdef CONFIG_WEBUI_FAILSAFE_NAND_RAW
		if (raw_mode && tgt.src == FAILSAFE_SRC_MTD) {
			size_t rps = nand_raw_page_size(tgt.mtd);
			u64 first_page, pages_to_read;
			size_t actual;

			if (!rps || !nand_raw_is_nand(tgt.mtd)) {
				flash_close_target(&tgt);
				goto bad_req;
			}

			first_page = read_start / rps;
			pages_to_read = (read_len + rps - 1) / rps;
			read_len = (size_t)(pages_to_read * rps);

			buf = malloc(read_len);
			if (!buf) {
				flash_close_target(&tgt);
				goto oom;
			}

			ret = nand_raw_read_pages(tgt.mtd, first_page,
						  pages_to_read, buf, read_len,
						  &actual);
			if (ret) {
				free(buf);
				flash_close_target(&tgt);
				goto io_err;
			}
			read_len = actual;
		} else
#endif
		{
			buf = malloc(read_len);
			if (!buf) {
				flash_close_target(&tgt);
				goto oom;
			}

			size_t readsz = 0;

			ret = flash_target_read(&tgt, read_start, buf,
						read_len, &readsz);
			if (ret || readsz != read_len) {
				free(buf);
				flash_close_target(&tgt);
				goto io_err;
			}
		}

		hex = flash_hex_dump(buf, read_len, &hex_len);
		free(buf);
		flash_close_target(&tgt);
		if (!hex)
			goto oom;

		json = malloc(hex_len + 320);
		if (!json) {
			free(hex);
			goto oom;
		}

		if (has_chunk) {
			size_t total_chunks = (len + FLASH_READ_CHUNK - 1) /
					      FLASH_READ_CHUNK;

			snprintf(json, hex_len + 320,
				"{\"ok\":true,\"start\":\"0x%llx\","
				"\"end\":\"0x%llx\",\"size\":%zu,\"chunk\":%d,"
				"\"chunk_total\":%zu,\"chunk_offset\":\"0x%llx\","
				"\"chunk_size\":%zu,\"data\":\"%s\"}\n",
				(unsigned long long)start,
				(unsigned long long)end,
				len, chunk, total_chunks,
				(unsigned long long)read_start, read_len,
				hex);
		} else {
			snprintf(json, hex_len + 320,
				"{\"ok\":true,\"start\":\"0x%llx\","
				"\"end\":\"0x%llx\",\"size\":%zu,"
				"\"data\":\"%s\"}\n",
				(unsigned long long)start,
				(unsigned long long)end,
				len, hex);
		}
		free(hex);

		failsafe_http_reply_json_alloc(response, 200, json, json);
		return;
	}

	if (!strcmp(op, "write")) {
		struct httpd_form_value *startv, *datav;
#ifdef CONFIG_WEBUI_FAILSAFE_NAND_RAW
		struct httpd_form_value *rawv;
#endif
		struct flash_target tgt;
		u8 *buf = NULL;
		size_t len = 0;
		size_t skipped = 0;
#ifdef CONFIG_WEBUI_FAILSAFE_NAND_RAW
		bool raw_mode = false;
#endif

		ret = flash_parse_storage_target(request, storage_sel,
						 sizeof(storage_sel),
						 target_name,
						 sizeof(target_name),
						 layout_name,
						 sizeof(layout_name));
		if (ret)
			goto bad_req;

		startv = httpd_request_find_value(request, "start");
		datav = httpd_request_find_value(request, "data");

		if (!startv || !startv->data || !datav || !datav->data)
			goto bad_req;

#ifdef CONFIG_WEBUI_FAILSAFE_NAND_RAW
		rawv = httpd_request_find_value(request, "raw");
		if (rawv && rawv->data && !strcmp(rawv->data, "1"))
			raw_mode = true;
#endif

		if (parse_u64_len(startv->data, &start))
			goto bad_range;

		ret = flash_parse_hex(datav->data, &buf, &len);
		if (ret)
			goto bad_req;

		ret = flash_open_target(storage_sel, target_name, layout_name,
					     &tgt);
		if (ret) {
			free(buf);
			goto bad_target;
		}

		if (start + len > tgt.size) {
			flash_close_target(&tgt);
			free(buf);
			goto bad_range;
		}

		if (tgt.src == FAILSAFE_SRC_MTD) {
#ifdef CONFIG_MTD
#ifdef CONFIG_WEBUI_FAILSAFE_NAND_RAW
			if (raw_mode && nand_raw_is_nand(tgt.mtd)) {
				size_t rps = nand_raw_page_size(tgt.mtd);
				size_t req_len = len;
				u64 first_page, pages;

				if (!rps) {
					ret = -EINVAL;
				} else {
					first_page = start / rps;
					pages = (len + rps - 1) / rps;

					ret = nand_raw_erase_blocks(tgt.mtd,
								    first_page,
								    pages);
					if (!ret) {
						size_t wr = 0;

						ret = nand_raw_write_pages(
							tgt.mtd, first_page,
							pages, buf, len, &wr);
						len = wr;

						/* The raw writer skips bad
						 * blocks as well. */
						skipped = req_len - wr;
					}
				}
			} else
#endif
			{
				ret = flash_target_write(&tgt, start, buf, len,
							 false, &skipped);
			}
#else
			ret = -ENODEV;
#endif
		} else {
			ret = flash_target_write(&tgt, start, buf, len, false,
						 &skipped);
		}

		flash_close_target(&tgt);
		free(buf);

		if (ret)
			goto io_err;

		json = malloc(128);
		if (!json)
			goto oom;
		snprintf(json, 128,
			 "{\"ok\":true,\"written\":%zu,\"skipped\":%zu}\n",
			 len, skipped);
		failsafe_http_reply_json_alloc(response, 200, json, json);
		return;
	}

	if (!strcmp(op, "restore")) {
		struct httpd_form_value *fw, *startv, *endv;
		struct flash_target tgt;
		char storage_from_name[16] = "";
		char target_from_name[64] = "";
		char layout_name[64] = "";
		u64 name_start = 0, name_end = 0;
		size_t len = 0;
		size_t skipped = 0;
#ifdef CONFIG_WEBUI_FAILSAFE_NAND_RAW
		bool raw_restore = false;
#endif

		fw = httpd_request_find_value(request, "backup");
		if (!fw)
			fw = httpd_request_find_value(request, "file");

		if (!fw || !fw->data || !fw->size)
			goto bad_req;

		ret = fw->filename ?
			flash_parse_backup_filename(fw->filename,
				storage_from_name, sizeof(storage_from_name),
				target_from_name, sizeof(target_from_name),
				layout_name, sizeof(layout_name),
				&name_start, &name_end) : -EINVAL;

		if (!ret) {
			strlcpy(storage_sel, storage_from_name,
				sizeof(storage_sel));
			strlcpy(target_name, target_from_name,
				sizeof(target_name));
			start = name_start;
			end = name_end;

#ifdef CONFIG_WEBUI_FAILSAFE_NAND_RAW
			/* Auto-detect an OOB backup from the filename suffix. */
			if (fw->filename && strstr(fw->filename, "_oob"))
				raw_restore = true;
#endif
		} else {
			startv = httpd_request_find_value(request, "start");
			endv = httpd_request_find_value(request, "end");

			ret = flash_parse_storage_target(request, storage_sel,
							 sizeof(storage_sel),
							 target_name,
							 sizeof(target_name),
							 layout_name,
							 sizeof(layout_name));
			if (ret)
				goto bad_req;

			if (!startv || !endv || !startv->data || !endv->data)
				goto bad_req;

			if (flash_parse_start_end(startv->data, endv->data,
						  &start, &end))
				goto bad_range;
		}

		len = fw->size;
		if (end <= start || (u64)len != (end - start))
			goto bad_range;

		ret = flash_open_target(storage_sel, target_name, layout_name,
					     &tgt);
		if (ret)
			goto bad_target;

		if (end > tgt.size) {
			flash_close_target(&tgt);
			goto bad_range;
		}

		if (tgt.src == FAILSAFE_SRC_MTD) {
#ifdef CONFIG_MTD
#ifdef CONFIG_WEBUI_FAILSAFE_NAND_RAW
			if (raw_restore && nand_raw_is_nand(tgt.mtd)) {
				size_t rps = nand_raw_page_size(tgt.mtd);
				size_t req_len = len;
				u64 first_page, pages;

				if (!rps) {
					ret = -EINVAL;
				} else {
					first_page = start / rps;
					pages = (len + rps - 1) / rps;

					ret = nand_raw_erase_blocks(tgt.mtd,
								    first_page,
								    pages);
					if (!ret) {
						size_t wr = 0;

						ret = nand_raw_write_pages(
							tgt.mtd, first_page,
							pages, fw->data, len,
							&wr);
						len = wr;

						/* The raw writer skips bad
						 * blocks as well. */
						skipped = req_len - wr;
					}
				}
			} else
#endif
			{
				ret = flash_target_write(&tgt, start, fw->data,
							 len, true, &skipped);
			}
#else
			ret = -ENODEV;
#endif
		} else {
			ret = flash_target_write(&tgt, start, fw->data, len,
						 true, &skipped);
		}

		flash_close_target(&tgt);

		if (ret)
			goto io_err;

		json = malloc(192);
		if (!json)
			goto oom;
		snprintf(json, 192,
			 "{\"ok\":true,\"restored\":%zu,\"skipped\":%zu,"
			 "\"alert\":\"Backup restore completed.\"}\n",
			 len, skipped);
		failsafe_http_reply_json_alloc(response, 200, json, json);
		return;
	}

	if (!strcmp(op, "erase")) {
		struct httpd_form_value *startv, *endv;
		struct flash_target tgt;
		u32 skipped_blocks = 0;
		u64 len;

		ret = flash_parse_storage_target(request, storage_sel,
						 sizeof(storage_sel),
						 target_name,
						 sizeof(target_name),
						 layout_name,
						 sizeof(layout_name));
		if (ret)
			goto bad_req;

		ret = flash_open_target(storage_sel, target_name, layout_name,
					     &tgt);
		if (ret)
			goto bad_target;

		startv = httpd_request_find_value(request, "start");
		endv = httpd_request_find_value(request, "end");

		if (startv && endv && startv->data && startv->data[0] &&
		    endv->data && endv->data[0]) {
			if (flash_parse_start_end(startv->data, endv->data,
						  &start, &end)) {
				flash_close_target(&tgt);
				goto bad_range;
			}
		} else if ((!startv || !startv->data || !startv->data[0]) &&
			   (!endv || !endv->data || !endv->data[0])) {
			/* No range given: erase the whole target. */
			start = 0;
			end = tgt.size;
		} else {
			flash_close_target(&tgt);
			goto bad_range;
		}

		if (start >= end || end > tgt.size) {
			flash_close_target(&tgt);
			goto bad_range;
		}

		len = end - start;

		ret = flash_target_erase(&tgt, start, len, &skipped_blocks);

		flash_close_target(&tgt);

		if (ret)
			goto io_err;

		json = malloc(192);
		if (!json)
			goto oom;
		snprintf(json, 192,
			 "{\"ok\":true,\"erased\":%llu,\"start\":\"0x%llx\","
			 "\"end\":\"0x%llx\",\"skipped_blocks\":%u}\n",
			 (unsigned long long)len,
			 (unsigned long long)start,
			 (unsigned long long)end,
			 skipped_blocks);
		failsafe_http_reply_json_alloc(response, 200, json, json);
		return;
	}

bad_req:
	failsafe_http_reply_json(response, 400,
		"{\"ok\":false,\"error\":\"bad_request\"}\n");
	return;
bad_target:
	failsafe_http_reply_json(response, 404,
		"{\"ok\":false,\"error\":\"target_not_found\"}\n");
	return;
bad_range:
	failsafe_http_reply_json(response, 400,
		"{\"ok\":false,\"error\":\"bad_range\"}\n");
	return;
oom:
	failsafe_http_reply_json(response, 500,
		"{\"ok\":false,\"error\":\"oom\"}\n");
	return;
io_err:
	failsafe_http_reply_json(response, 500,
		"{\"ok\":false,\"error\":\"io\"}\n");
	return;
}

/* ------------------------------------------------------------------ */
/*  Registration                                                       */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_WEBUI_FAILSAFE_FLASH

#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_FLASH_LAYOUT)
/* ------------------------------------------------------------------ */
/*  GET /flash/layouts - flash layouts of the board device tree        */
/* ------------------------------------------------------------------ */

/**
 * flash_layouts_handler - GET /flash/layouts
 *
 * Lists the flash layouts the board describes in its device tree (see
 * <failsafe/layout.h>), so that the editor can work in a layout the device
 * does not currently run - which is how a device is moved from one layout to
 * another:
 *
 *	{"ok":true,"layouts":[{"name":"stock","parts":[
 *		{"name":"bootloader","offset":"0x0","size":"0x80000"}]}]}
 *
 * The offsets are raw device offsets; a size of 0 means "to the end of the
 * device".  A target of such a layout is selected by sending the layout name
 * in the "layout" field together with the partition label in "target" (see
 * flash_parse_storage_target()).
 */
void flash_layouts_handler(enum httpd_uri_handler_status status,
			   struct httpd_request *request,
			   struct httpd_response *response)
{
	struct failsafe_layout *layouts;
	char esc[80];
	char *buf;
	size_t size;
	int count, i, j, len = 0;

	if (status != HTTP_CB_NEW)
		return;

	layouts = malloc(FAILSAFE_LAYOUT_MAX * sizeof(*layouts));
	buf = malloc(FLASH_LAYOUTS_BUF_SZ);
	if (!layouts || !buf) {
		free(layouts);
		free(buf);
		failsafe_http_reply_json(response, 500,
					 "{\"ok\":false,\"error\":\"oom\"}\n");
		return;
	}

	count = failsafe_layout_parse(layouts, FAILSAFE_LAYOUT_MAX);

	size = FLASH_LAYOUTS_BUF_SZ;
	len = buf_appendf(buf, size, len, "{\"ok\":true,\"layouts\":[");
	for (i = 0; i < count; i++) {
		json_escape(esc, sizeof(esc), layouts[i].label);
		len = buf_appendf(buf, size, len,
				  "%s{\"name\":\"%s\",\"parts\":[",
				  i ? "," : "", esc);

		for (j = 0; j < layouts[i].num_parts; j++) {
			u64 off = layouts[i].parts[j].offset;
			u64 psize = layouts[i].parts[j].size;

			json_escape(esc, sizeof(esc),
				    layouts[i].parts[j].label);
			len = buf_appendf(buf, size, len,
					  "%s{\"name\":\"%s\","
					  "\"offset\":\"0x%llx\","
					  "\"size\":\"0x%llx\"}",
					  j ? "," : "", esc,
					  (unsigned long long)off,
					  (unsigned long long)psize);
		}

		len = buf_appendf(buf, size, len, "]}");
	}
	len = buf_appendf(buf, size, len, "]}\n");

	free(layouts);
	failsafe_http_reply_json_alloc(response, 200, buf, buf);
}
#endif /* CONFIG_WEBUI_FAILSAFE_FLASH_LAYOUT */

void flash_register_handlers(struct httpd_instance *inst)
{
	/* The page and its script are registered by the page inventory
	 * (failsafe/pages.c); this module only owns the endpoints. */
	httpd_register_uri_handler(inst, "/flash/info", &flash_info_handler,
				   NULL);
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_FLASH_LAYOUT)
	httpd_register_uri_handler(inst, "/flash/layouts",
				   &flash_layouts_handler, NULL);
#endif
	httpd_register_uri_handler(inst, "/flash/backup", &flash_backup_handler,
				   NULL);
	httpd_register_uri_handler(inst, "/flash/read", &flash_handler, NULL);
	httpd_register_uri_handler(inst, "/flash/write", &flash_handler, NULL);
	httpd_register_uri_handler(inst, "/flash/erase", &flash_handler, NULL);
	httpd_register_uri_handler(inst, "/flash/restore", &flash_handler, NULL);
}
#endif
