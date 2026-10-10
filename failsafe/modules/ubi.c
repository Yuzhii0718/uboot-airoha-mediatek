/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project bl-mt798x-dhcpd
 * You may not use, copy, modify or distribute this file except in compliance with the license agreement.
 *
 * Failsafe UBI volume management
 */

#include <errno.h>
#include <malloc.h>
#include <memalign.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <net/mtk_httpd.h>
#include <mtd.h>
#include <nand.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/err.h>
#include <ubi_uboot.h>
#include <linux/errno.h>
#include <vsprintf.h>
#include <command.h>

#ifdef CONFIG_CMD_UBIFS
#include <ubifs_uboot.h>
#endif

#include <failsafe/internal.h>
#include <failsafe/storage.h>
#include <cprint.h>

#include <env.h>
#if IS_ENABLED(CONFIG_ENV_IS_IN_UBI)
#include <net.h>
#endif

/* Max buffer size for JSON response */
#define UBI_JSON_BUF_SZ		16384

/* Max volume name length */
#define UBI_VOL_NAME_MAX_LEN	128

/* Max MTD partition name length */
#define UBI_MTD_NAME_MAX_LEN	64

/**
 * ubi_info_handler - GET /ubi/info
 *
 * Returns JSON with UBI device information:
 * {"mtd_name":"...","flash_size":0,"peb_size":0,"leb_size":0,...}
 */
void ubi_info_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *buf;
	int len = 0;
	int left = UBI_JSON_BUF_SZ;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_GET) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	buf = malloc(left);
	if (!buf) {
		failsafe_http_reply_json(response, 500, "{\"error\":\"oom\"}");
		return;
	}

	/* Check if UBI is attached */
	struct ubi_device *ubi = ubi_devices[0];

	if (!ubi) {
		len = buf_appendf(buf, left, len,
			"{\"error\":\"no ubi device\",\"attached\":false}");
		goto done;
	}

	len = buf_appendf(buf, left, len,
		"{\"attached\":true,"
		"\"mtd_name\":\"%s\","
		"\"ubi_num\":%d,"
		"\"flash_size\":%llu,"
		"\"peb_size\":%d,"
		"\"leb_size\":%d,"
		"\"good_peb_count\":%d,"
		"\"bad_peb_count\":%d,"
		"\"min_io_size\":%d,"
		"\"max_vol_count\":%d,"
		"\"vol_count\":%d,"
		"\"avail_pebs\":%d,"
		"\"rsvd_pebs\":%d,"
		"\"beb_rsvd_pebs\":%d,"
		"\"max_ec\":%d,"
		"\"mean_ec\":%d}",
		ubi->mtd ? ubi->mtd->name : "unknown",
		ubi->ubi_num,
		(unsigned long long)ubi->flash_size,
		ubi->peb_size,
		ubi->leb_size,
		ubi->good_peb_count,
		ubi->bad_peb_count,
		ubi->min_io_size,
		ubi->vtbl_slots,
		ubi->vol_count - UBI_INT_VOL_COUNT,
		ubi->avail_pebs,
		ubi->rsvd_pebs,
		ubi->beb_rsvd_pebs,
		ubi->max_ec,
		ubi->mean_ec);

done:
	failsafe_http_reply_json_alloc(response, 200, buf, buf);
}

/**
 * ubi_volumes_handler - GET /ubi/volumes
 *
 * Returns JSON array of UBI volumes:
 * {"volumes":[{"id":0,"name":"...","size":0,"type":"dynamic",...},...]}
 */
void ubi_volumes_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *buf;
	int len = 0;
	int left = UBI_JSON_BUF_SZ;
	bool first = true;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_GET) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	buf = malloc(left);
	if (!buf) {
		failsafe_http_reply_json(response, 500, "{\"error\":\"oom\"}");
		return;
	}

	struct ubi_device *ubi = ubi_devices[0];

	if (!ubi) {
		len = buf_appendf(buf, left, len,
			"{\"error\":\"no ubi device\",\"volumes\":[]}");
		goto done;
	}

	len = buf_appendf(buf, left, len, "{\"volumes\":[");

	for (int i = 0; i < ubi->vtbl_slots && len < left - 256; i++) {
		struct ubi_volume *vol = ubi->volumes[i];

		if (!vol)
			continue;
		if (vol->vol_id >= UBI_INTERNAL_VOL_START)
			continue;

		len = buf_appendf(buf, left, len,
			"%s{\"id\":%d,\"name\":\"%s\","
			"\"size\":%llu,\"used_bytes\":%llu,"
			"\"type\":\"%s\","
			"\"corrupted\":%d,\"upd_marker\":%d,"
			"\"skip_check\":%d,"
			"\"reserved_peb\":%d,\"alignment\":%d,"
			"\"data_pad\":%d,\"usable_leb_size\":%d}",
			first ? "" : ",",
			vol->vol_id,
			vol->name,
			(unsigned long long)vol->reserved_pebs * ubi->leb_size,
			(unsigned long long)vol->used_bytes,
			vol->vol_type == UBI_DYNAMIC_VOLUME ? "dynamic" : "static",
			vol->corrupted,
			vol->upd_marker,
			vol->skip_check,
			vol->reserved_pebs,
			vol->alignment,
			vol->data_pad,
			vol->usable_leb_size);

		first = false;
	}

	len = buf_appendf(buf, left, len, "]}");

done:
	failsafe_http_reply_json_alloc(response, 200, buf, buf);
}

/**
 * ubi_attach_handler - POST /ubi/attach
 *
 * Form parameters:
 *   mtd_name - MTD partition name to attach
 *
 * Returns JSON: {"ok":true} or {"error":"..."}
 */
void ubi_attach_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *mtd_name = NULL;
	char *json_out;
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	/* Get MTD name */
	ret = failsafe_get_form_value(request, "mtd_name", &mtd_name,
		UBI_MTD_NAME_MAX_LEN, false, false);
	if (ret || !mtd_name || !mtd_name[0]) {
		json_out = strdup("{\"error\":\"missing mtd_name\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing mtd_name\"}",
			json_out);
		return;
	}

	/* Detach existing UBI first */
	ubi_detach();

	/* Attach to new partition */
	ret = ubi_part(mtd_name, NULL);
	free(mtd_name);

	if (ret) {
		json_out = malloc(128);
		if (json_out)
			snprintf(json_out, 128,
				"{\"error\":\"attach failed: %d\"}", ret);
		failsafe_http_reply_json_alloc(response, 500,
			json_out ? json_out : "{\"error\":\"attach failed\"}",
			json_out);
		return;
	}

	json_out = strdup("{\"ok\":true}");
	failsafe_http_reply_json_alloc(response, 200,
		json_out ? json_out : "{\"ok\":true}", json_out);
}

/**
 * ubi_detach_handler - POST /ubi/detach
 *
 * Returns JSON: {"ok":true} or {"error":"..."}
 */
void ubi_detach_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *json_out;
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	ret = ubi_detach();

	if (ret) {
		json_out = malloc(128);
		if (json_out)
			snprintf(json_out, 128,
				"{\"error\":\"detach failed: %d\"}", ret);
		failsafe_http_reply_json_alloc(response, 500,
			json_out ? json_out : "{\"error\":\"detach failed\"}",
			json_out);
		return;
	}

	json_out = strdup("{\"ok\":true}");
	failsafe_http_reply_json_alloc(response, 200,
		json_out ? json_out : "{\"ok\":true}", json_out);
}

/* ------------------------------------------------------------------ */
/*  Rebuild: the volumes that have to cross the wipe                   */
/* ------------------------------------------------------------------ */

/* Same fallback as the environment driver (see env/ubi.c) */
#ifndef CONFIG_ENV_UBI_EXTRA_VOLUMES
#define CONFIG_ENV_UBI_EXTRA_VOLUMES ""
#endif

/* Most volumes a rebuild can carry across; a longer list is refused before
 * anything has been erased rather than kept only in part.
 */
#define UBI_KEEP_MAX_VOLUMES	8

/* Scratch buffer the search for the used part of a volume works in, and the
 * distance under which two pieces of data still count as the same run.
 */
#define UBI_KEEP_WINDOW		(64 * 1024)
#define UBI_KEEP_GAP		UBI_KEEP_WINDOW

/* How much of the partition one "erase" request wipes: small enough for the
 * page to show progress while the wipe runs, large enough for the request
 * overhead not to matter.
 */
#define UBI_REBUILD_ERASE_CHUNK	(2 * 1024 * 1024)

/*
 * The volumes CONFIG_ENV_UBI_EXTRA_VOLUMES lists hold board data in the KiB
 * range - they are raw partition dumps written into a UBI volume, so their
 * size is the size of the partition they were dumped from, and everything
 * past the data reads as erased bytes.  What is worth keeping are therefore
 * the pieces that are not erased, wherever they sit: at the start of the
 * volume on most boards, at the end of it or spread over it (a piece at the
 * start, one in the middle, one at the end) on others.
 *
 * A volume that turns out to hold more pieces than this has its two closest
 * ones merged until it holds this many, so the copy grows by the smallest
 * gaps there are and nothing is ever dropped.
 */
#define UBI_KEEP_MAX_RUNS	8

/* A piece of a volume that is not erased. */
struct ubi_keep_run {
	u64 off;
	void *buf;
	size_t size;
};

/*
 * One volume whose content is held in the heap until the fresh device has
 * the volume again.  @name points into the caller's name buffer, @kept is
 * the total number of bytes the runs hold, see ubi_keep_read().
 */
struct ubi_keep_entry {
	const char *name;
	struct ubi_keep_run runs[UBI_KEEP_MAX_RUNS];
	int nr_runs;
	size_t kept;
};

#if IS_ENABLED(CONFIG_ENV_IS_IN_UBI)
static void ubi_keep_free(struct ubi_keep_entry *keep, int kept)
{
	int i, j;

	for (i = 0; i < kept; i++) {
		for (j = 0; j < keep[i].nr_runs; j++) {
			free(keep[i].runs[j].buf);
			keep[i].runs[j].buf = NULL;
			keep[i].runs[j].size = 0;
		}

		keep[i].nr_runs = 0;
		keep[i].kept = 0;
	}
}

/*
 * Merge the two pieces of @entry that are closest to each other.
 *
 * Called when a volume turns out to hold more pieces of data than
 * UBI_KEEP_MAX_RUNS.  The copy then grows by the smallest gap there is
 * instead of by all of them, and nothing is dropped: the merged piece covers
 * the erased bytes between the two, which are 0xff in the volume and read as
 * 0xff in the copy, so they are written back exactly as they were.
 */
static void ubi_keep_merge_closest(struct ubi_keep_entry *entry)
{
	u64 best = 0, gap;
	int at = 0, i;

	if (entry->nr_runs < 2)
		return;

	for (i = 0; i + 1 < entry->nr_runs; i++) {
		gap = entry->runs[i + 1].off -
			(entry->runs[i].off + entry->runs[i].size);

		if (!i || gap < best) {
			best = gap;
			at = i;
		}
	}

	entry->runs[at].size = entry->runs[at + 1].off +
		entry->runs[at + 1].size - entry->runs[at].off;

	memmove(&entry->runs[at + 1], &entry->runs[at + 2],
		(entry->nr_runs - at - 2) * sizeof(entry->runs[0]));

	entry->nr_runs--;
}

/*
 * Note the data of one window of a volume: @len bytes at @off, in @buf.
 *
 * A byte of 0xff is an erased byte, so a window without any is skipped
 * whole.  Data extends the piece in front of it when it is closer than
 * UBI_KEEP_GAP and otherwise becomes a piece of its own - merging the two
 * closest pieces first when the volume already holds UBI_KEEP_MAX_RUNS of
 * them.  Data that is scattered over the volume (a piece at the start, one
 * in the middle, one at the end) is therefore kept whole, and with the least
 * memory the limit allows.
 */
static void ubi_keep_scan_window(struct ubi_keep_entry *entry, u64 off,
				 const u8 *buf, size_t len)
{
	struct ubi_keep_run *run = NULL;
	size_t first = len, last = 0, i;
	bool has_data = false, closer;

	for (i = 0; i < len; i++) {
		if (buf[i] == 0xff)
			continue;

		if (first == len)
			first = i;

		last = i;
		has_data = true;
	}

	if (!has_data)
		return;

	/* The pieces are kept in ascending order, so the piece this data
	 * could belong to is the last one. */
	closer = false;
	if (entry->nr_runs) {
		run = &entry->runs[entry->nr_runs - 1];
		closer = off + first - (run->off + run->size) <= UBI_KEEP_GAP;
	}

	if (!closer && entry->nr_runs == UBI_KEEP_MAX_RUNS) {
		ubi_keep_merge_closest(entry);

		/* Merging reaches into a gap, which can put this data back
		 * within range of the piece before it. */
		run = &entry->runs[entry->nr_runs - 1];
		closer = off + first - (run->off + run->size) <= UBI_KEEP_GAP;
	}

	if (!closer) {
		run = &entry->runs[entry->nr_runs];
		run->off = off + first;
		entry->nr_runs++;
	}

	run->size = off + last + 1 - run->off;
}

/*
 * Read out the pieces of one volume that are not erased.
 *
 * UBI reads the logical eraseblocks of a volume that were never written as
 * 0xFF (ubi_eba_read_leb()), and the volume the rebuild creates again reads
 * that way as well - so a byte of 0xff carries nothing and never has to be
 * copied, wherever it sits in the volume.  What is left of these volumes is
 * a few KiB in one place or two (they are raw partition dumps written into a
 * volume, so their size is the size of the partition, not of the data),
 * which is what makes the copies fit in the U-Boot heap: copying a whole
 * "art" volume would mean 2.3 MiB of erased bytes.
 *
 * The volume is walked window by window (@scratch, @scratch_sz bytes) to
 * find the pieces - see ubi_keep_scan_window() - and each piece is then read
 * into a buffer of its own.
 *
 * @entry comes back with no runs and nothing kept when the volume holds
 * only erased bytes: an empty "ri" or "bosa" costs nothing at all.
 *
 * Returns 0, or a negative errno.
 */
static int ubi_keep_read_volume(const char *name, void *scratch,
				size_t scratch_sz, struct ubi_keep_entry *entry)
{
	struct ubi_volume *vol = ubi_find_volume(name);
	u64 cap, off;
	int ret, i;

	entry->nr_runs = 0;
	entry->kept = 0;

	/* The scan below only records where the pieces are: the buffers come
	 * with the read of each piece, and the failure path frees whatever it
	 * finds - so they all start out empty. */
	for (i = 0; i < UBI_KEEP_MAX_RUNS; i++)
		entry->runs[i].buf = NULL;

	if (!vol || !vol->used_bytes)
		return 0;

	/*
	 * The pieces go back into the volume one update at a time (see
	 * ubi_keep_restore()), and updating a part of a volume reads the
	 * eraseblocks it does not hold yet - which a static volume trips an
	 * assert on, and in U-Boot that assert hangs the device.  Refusing
	 * it here happens before anything is erased: such a volume can be
	 * left out of CONFIG_ENV_UBI_EXTRA_VOLUMES, or the board can create
	 * its volumes dynamically, which is the default.
	 */
	if (vol->vol_type != UBI_DYNAMIC_VOLUME) {
		cprintln(ERROR, "ubi: '%s' is a static volume, it cannot be "
			 "kept across a rebuild", name);
		return -EPERM;
	}

	cap = (u64)vol->used_bytes;

	for (off = 0; off < cap; off += scratch_sz) {
		size_t len = min_t(u64, cap - off, (u64)scratch_sz);

		ret = ubi_volume_read(name, scratch, off, len);
		if (ret)
			goto fail;

		ubi_keep_scan_window(entry, off, scratch, len);
	}

	for (i = 0; i < entry->nr_runs; i++) {
		struct ubi_keep_run *run = &entry->runs[i];

		if (!run->size)
			continue;

		run->buf = malloc(run->size);
		if (!run->buf) {
			ret = -ENOMEM;
			goto fail;
		}

		ret = ubi_volume_read(name, run->buf, run->off, run->size);
		if (ret)
			goto fail;

		entry->kept += run->size;
	}

	return 0;

fail:
	for (i = 0; i < entry->nr_runs; i++) {
		free(entry->runs[i].buf);
		entry->runs[i].buf = NULL;
	}

	entry->nr_runs = 0;
	entry->kept = 0;

	return ret;
}

/*
 * Read the volumes CONFIG_ENV_UBI_EXTRA_VOLUMES lists into @keep.
 *
 * Those are the volumes the environment driver creates again on a fresh
 * device - "bosa", "ri", "art" on the boards that use them - and they hold
 * board data that exists nowhere else: the factory MAC, the radio
 * calibration.  @names is the caller's buffer (@names_sz bytes) for the
 * names; the entries point into it, so it has to outlive them.
 *
 * A volume that does not exist, or that holds nothing but erased bytes, is
 * skipped: there is nothing to carry across for it.  Everything else
 * failing fails the whole rebuild, and it does so before the erase - erasing
 * data that cannot be given back is worse than not rebuilding at all.
 *
 * Returns the number of volumes kept (>= 0), or a negative errno.
 */
static int ubi_keep_read(struct ubi_keep_entry *keep, int max, char *names,
			 size_t names_sz)
{
	char *p, *name;
	void *scratch;
	int kept = 0;
	int ret;

	ret = env_ubi_extra_volume_names(names, names_sz);
	if (ret) {
		cprintln(ERROR, "ubi: cannot read the extra volume list (%d)",
			 ret);
		return ret;
	}

	scratch = malloc(UBI_KEEP_WINDOW);
	if (!scratch)
		return -ENOMEM;

	p = names;
	while ((name = strsep(&p, ",")) != NULL) {
		if (!name[0])
			continue;

		if (kept >= max) {
			cprintln(ERROR, "ubi: more than %d volumes to keep",
				 max);
			ret = -ENOSPC;
			goto out;
		}

		ret = ubi_keep_read_volume(name, scratch, UBI_KEEP_WINDOW,
					   &keep[kept]);
		if (ret)
			goto out;

		if (!keep[kept].kept) {
			cprintln(NORMAL, "ubi: '%s' is empty, nothing to keep",
				 name);
			continue;
		}

		cprintln(NORMAL, "ubi: '%s' kept (%zu bytes in %d piece(s))",
			 name, keep[kept].kept, keep[kept].nr_runs);

		keep[kept].name = name;
		kept++;
	}

	free(scratch);
	return kept;

out:
	ubi_keep_free(keep, kept);
	free(scratch);

	return ret;
}

/*
 * Write the kept volumes back into the volumes of the same name, which the
 * environment driver has just created again (env_ubi_volumes_create()).
 *
 * @restored receives the number of bytes written back (may be NULL) and
 * @failed the name of the volume that could not be written (may be NULL).
 *
 * Returns 0, or the negative errno of the volume that failed.
 */
static int ubi_keep_restore(struct ubi_keep_entry *keep, int kept,
			    size_t *restored, const char **failed)
{
	int i, j;
	int ret;

	if (restored)
		*restored = 0;
	if (failed)
		*failed = NULL;

	/*
	 * The volumes are created again with the size this board configures
	 * (CONFIG_ENV_UBI_EXTRA_VOLUMES), so a device that was set up with
	 * other sizes has a volume that is smaller now - and a piece running
	 * past the end of it would end up written off the volume, because
	 * ubi_volume_offset_write() only checks the size it is given and not
	 * where that size starts.  Every piece is checked before anything is
	 * written back at all.
	 */
	for (i = 0; i < kept; i++) {
		struct ubi_volume *vol = ubi_find_volume(keep[i].name);

		if (!vol)
			continue;

		for (j = 0; j < keep[i].nr_runs; j++) {
			struct ubi_keep_run *run = &keep[i].runs[j];

			if (run->off + run->size > (u64)vol->used_bytes) {
				cprintln(ERROR,
					 "ubi: '%s' is smaller than it was, "
					 "0x%llx does not fit",
					 keep[i].name,
					 (unsigned long long)(run->off +
							      run->size));
				if (failed)
					*failed = keep[i].name;
				return -EINVAL;
			}
		}
	}

	for (i = 0; i < kept; i++) {
		for (j = 0; j < keep[i].nr_runs; j++) {
			struct ubi_keep_run *run = &keep[i].runs[j];

			/* The piece goes back where it came from: at 0 that
			 * is a plain volume update, further in it is an
			 * update of the eraseblocks it covers. */
			ret = ubi_volume_write(keep[i].name, run->buf,
					       run->off, run->size);
			if (ret) {
				cprintln(ERROR,
					 "ubi: cannot write '%s' back at 0x%llx (%d)",
					 keep[i].name,
					 (unsigned long long)run->off, ret);
				if (failed)
					*failed = keep[i].name;
				return ret;
			}

			if (restored)
				*restored += run->size;
		}

		cprintln(NORMAL, "ubi: '%s' restored (%zu bytes in %d piece(s))",
			 keep[i].name, keep[i].kept, keep[i].nr_runs);
	}

	return 0;
}
#else /* ! CONFIG_ENV_IS_IN_UBI */
/*
 * Without a UBI environment there is no CONFIG_ENV_UBI_EXTRA_VOLUMES list
 * and nothing that creates those volumes again, so there is nothing to
 * carry across: the rebuild works, it just keeps no extra volume.
 */
static void ubi_keep_free(struct ubi_keep_entry *keep, int kept)
{
}

static int ubi_keep_read(struct ubi_keep_entry *keep, int max, char *names,
			 size_t names_sz)
{
	return 0;
}

static int ubi_keep_restore(struct ubi_keep_entry *keep, int kept,
			    size_t *restored, const char **failed)
{
	if (restored)
		*restored = 0;
	if (failed)
		*failed = NULL;

	return 0;
}
#endif /* CONFIG_ENV_IS_IN_UBI */

/*
 * The steps a rebuild is made of, in the order the page runs them.  Each one
 * is a request of its own, so the page can say which one is running, and each
 * one leaves the progress bar at the percentage that is listed below - the
 * wipe and the format are whole-partition operations and own most of it,
 * while writing the FIP and the board data back is a few MiB at most.
 */
enum rebuild_step {
	REBUILD_STEP_ERASE,	/* detach the old device, wipe the partition */
	REBUILD_STEP_FORMAT,	/* format and attach a fresh, empty device */
	REBUILD_STEP_FIP,	/* create the "fip" volume, write it back */
	REBUILD_STEP_ENV,	/* environment volume, save the environment */
	REBUILD_STEP_RESTORE,	/* write the board data volumes back */
	REBUILD_STEP_DONE,
};

/* Where the bar stands once a step has been reached, in percent.  The wipe
 * fills the gap up to REBUILD_STEP_FORMAT as it goes on, see
 * rebuild_progress(). */
static const u8 rebuild_step_percent[] = {
	[REBUILD_STEP_ERASE]	= 5,
	[REBUILD_STEP_FORMAT]	= 55,
	[REBUILD_STEP_FIP]	= 78,
	[REBUILD_STEP_ENV]	= 88,
	[REBUILD_STEP_RESTORE]	= 94,
	[REBUILD_STEP_DONE]	= 100,
};

/*
 * The rebuild the page drives, one step per request (see
 * ubi_rebuild_handler()).  The copies taken before the erase have to live in
 * the heap across those requests, and @step is what lets the page
 * show where the rebuild is - and pick an unfinished one up again in the
 * middle of the steps, instead of starting over on a device that is already
 * half erased.
 */
static struct {
	bool active;
	u8 step;
	char mtd_name[UBI_MTD_NAME_MAX_LEN];
	u64 erase_total;
	u64 erase_done;
	u32 erasesize;
	u32 bad_blocks;
	void *fip;
	size_t fip_size;
	struct ubi_keep_entry keep[UBI_KEEP_MAX_VOLUMES];
	char keep_names[sizeof(CONFIG_ENV_UBI_EXTRA_VOLUMES)];
	int kept;
	size_t extra_bytes;
#if IS_ENABLED(CONFIG_ENV_IS_IN_UBI)
	char ethaddr[18];
	bool ethaddr_present;
	bool ethaddr_generated;
#endif
} rebuild;

/* Bytes of board data a rebuild holds at the moment (for the page). */
static size_t rebuild_kept_bytes(void)
{
	size_t bytes = 0;
	int i;

	for (i = 0; i < rebuild.kept; i++)
		bytes += rebuild.keep[i].kept;

	return bytes;
}

/*
 * How far a rebuild has got, in percent, for the page's progress bar: the
 * percentage of the step it has reached, and for the wipe the share of the
 * partition that is already gone.  A rebuild that has not started reports
 * where the first step begins.
 */
static u8 rebuild_progress(void)
{
	u8 start = rebuild_step_percent[rebuild.step];

	if (rebuild.step != REBUILD_STEP_ERASE || !rebuild.erase_total)
		return start;

	return start + (u8)((u64)(rebuild_step_percent[REBUILD_STEP_FORMAT] -
				  start) * rebuild.erase_done /
			   rebuild.erase_total);
}

/* Give up on a rebuild and release everything it holds. */
static void rebuild_release(void)
{
	ubi_keep_free(rebuild.keep, rebuild.kept);
	free(rebuild.fip);

	rebuild.active = false;
	rebuild.step = REBUILD_STEP_ERASE;
	rebuild.mtd_name[0] = '\0';
	rebuild.erase_total = 0;
	rebuild.erase_done = 0;
	rebuild.erasesize = 0;
	rebuild.bad_blocks = 0;
	rebuild.fip = NULL;
	rebuild.fip_size = 0;
	rebuild.kept = 0;
	rebuild.extra_bytes = 0;
#if IS_ENABLED(CONFIG_ENV_IS_IN_UBI)
	rebuild.ethaddr_present = false;
	rebuild.ethaddr_generated = false;
#endif
}

/*
 * Steps 0 and 0b: take out of the device everything a fresh one cannot
 * provide again - the FIP, without which the board does not boot, and the
 * board data volumes the environment driver owns (the factory MAC of "ri",
 * the radio calibration of "art") - and note the partition to erase.
 *
 * A rebuild that is already under way is left alone: the page asking for
 * "begin" again means it picked the unfinished one up, and reading a device
 * that has been half erased already would only lose the copies.
 */
static int rebuild_read(const char *mtd_name)
{
	struct ubi_volume *vol;
	struct mtd_info *mtd;
	int ret;

	if (rebuild.active)
		return 0;

	strlcpy(rebuild.mtd_name, mtd_name, sizeof(rebuild.mtd_name));

	mtd_probe_devices();
	mtd = get_mtd_device_nm(rebuild.mtd_name);
	if (IS_ERR_OR_NULL(mtd))
		return -ENODEV;

	if (!mtd->erasesize) {
		put_mtd_device(mtd);
		return -EINVAL;
	}

	rebuild.erase_total = mtd->size;
	rebuild.erasesize = mtd->erasesize;
	put_mtd_device(mtd);

	/*
	 * Nothing can be read out while UBI is not attached, so a device
	 * whose volume table is broken enough to keep it from attaching
	 * gives its volumes up on the way (the FIP can be put back with the
	 * flashing page afterwards).  This is the case the rebuild exists
	 * for, so it is not an error.
	 */
	if (!ubi_devices[0]) {
		rebuild.active = true;
		return 0;
	}

	vol = ubi_find_volume(FAILSAFE_STORAGE_STATIC_TARGET);
	if (vol && vol->used_bytes) {
		rebuild.fip_size = (size_t)vol->used_bytes;
		rebuild.fip = malloc(rebuild.fip_size);
		if (!rebuild.fip)
			return -ENOMEM;

		ret = ubi_volume_read(FAILSAFE_STORAGE_STATIC_TARGET,
				      rebuild.fip, 0, rebuild.fip_size);
		if (ret) {
			cprintln(ERROR, "ubi: cannot read the '%s' volume (%d)",
				 FAILSAFE_STORAGE_STATIC_TARGET, ret);
			return ret;
		}

		cprintln(NORMAL, "ubi: '%s' kept (%zu bytes)",
			 FAILSAFE_STORAGE_STATIC_TARGET, rebuild.fip_size);
	}

	rebuild.kept = ubi_keep_read(rebuild.keep, ARRAY_SIZE(rebuild.keep),
				     rebuild.keep_names,
				     sizeof(rebuild.keep_names));
	if (rebuild.kept < 0)
		return rebuild.kept;

#if IS_ENABLED(CONFIG_ENV_IS_IN_UBI)
	/*
	 * The environment lives in this UBI device as well, so the wipe
	 * takes it - and with it the MAC address the network stack needs.
	 * Keep it, but only when it is a usable address: a device can carry
	 * a placeholder such as ff:ff:ff:ff:ff:ff (the factory address of
	 * this family sits in "ri", which the rebuild brings back empty, so
	 * a unit that lost it once has the placeholder from then on), and
	 * writing that back only makes the Ethernet driver reject it.
	 * Anything else is replaced by a generated address at the end.
	 */
	{
		const char *val = env_get("ethaddr");

		if (val) {
			uchar mac[ARP_HLEN];

			string_to_enetaddr(val, mac);
			if (is_valid_ethaddr(mac)) {
				snprintf(rebuild.ethaddr,
					 sizeof(rebuild.ethaddr), "%pM", mac);
				rebuild.ethaddr_present = true;
			}
		}
	}
#endif

	rebuild.active = true;

	return 0;
}

/*
 * Step 1 and one chunk of step 2: wipe the next slice of the partition,
 * block by block with the bad ones skipped - exactly what
 * "mtd erase <partition>" does.  One mtd_erase() over the whole partition is
 * not usable here: the NAND drivers abort such a range erase with -EIO on
 * the first bad block they meet, and they do it silently (the only message
 * is a debug one), so it would fail on every device that has a single
 * factory bad block - which every NAND has a chance of.
 */
static int rebuild_erase(void)
{
	struct mtd_info *mtd;
	loff_t off, end;
	int ret = 0;

	if (!rebuild.active)
		return -EINVAL;

	if (!rebuild.erase_done) {
		/* Step 1: the old device goes away before its partition is
		 * erased.  Everything worth keeping is in the heap by now. */
		ubi_detach();
	}

	mtd = get_mtd_device_nm(rebuild.mtd_name);
	if (IS_ERR_OR_NULL(mtd))
		return -ENODEV;

	end = min_t(u64, rebuild.erase_done + UBI_REBUILD_ERASE_CHUNK,
		    rebuild.erase_total);

	for (off = rebuild.erase_done; off < end; off += rebuild.erasesize) {
		struct erase_info ei;
		int bad = mtd_block_isbad(mtd, off);

		if (bad < 0) {
			ret = bad;
			break;
		}

		/* Nothing to erase, and erasing it is not allowed: the UBI
		 * layer finds it again when it attaches. */
		if (bad) {
			rebuild.bad_blocks++;
			continue;
		}

		memset(&ei, 0, sizeof(ei));
		ei.mtd = mtd;
		ei.addr = off;
		ei.len = rebuild.erasesize;

		ret = mtd_erase(mtd, &ei);
		if (ret == -EIO) {
			/* Like "mtd erase": a block the driver refuses to
			 * erase does not stop the wipe of the others. */
			cprintln(CAUTION,
				 "ubi: erase of 0x%llx failed, continuing",
				 (unsigned long long)off);
			ret = 0;
			continue;
		}

		if (ret)
			break;
	}

	put_mtd_device(mtd);

	if (ret)
		return ret;

	rebuild.erase_done = end;

	return 0;
}

/*
 * Step 3: format the partition and attach a fresh, empty device to it.  A
 * format erases every eraseblock again and writes the UBI headers, so this is
 * a whole-partition operation - the page gets its own step for it rather than
 * having it hidden behind the end of the wipe.
 */
static int rebuild_format(void)
{
	if (!rebuild.active || rebuild.erase_done < rebuild.erase_total)
		return -EINVAL;

	return ubi_part(rebuild.mtd_name, NULL);
}

/*
 * Step 4: create the "fip" volume again - as the static volume of
 * FAILSAFE_STORAGE_STATIC_SIZE the firmware upgrade creates (see
 * <failsafe/storage.h>), so the rebuilt device is laid out exactly like a
 * freshly upgraded one - and write the saved image back into it.
 *
 * The image stays in the heap until it is written, so the step can simply be
 * asked for again after a failure; the volume the attempt before left behind
 * is dropped first.
 */
static int rebuild_fip(void)
{
	int ret;

	if (!rebuild.active)
		return -EINVAL;

	if (!rebuild.fip)
		return 0;

	if (ubi_find_volume(FAILSAFE_STORAGE_STATIC_TARGET))
		ubi_remove_vol(FAILSAFE_STORAGE_STATIC_TARGET);

	ret = ubi_create_vol(FAILSAFE_STORAGE_STATIC_TARGET,
			     FAILSAFE_STORAGE_STATIC_SIZE, false,
			     UBI_VOL_NUM_AUTO, false);
	if (ret)
		return ret;

	ret = ubi_volume_write(FAILSAFE_STORAGE_STATIC_TARGET, rebuild.fip, 0,
			       rebuild.fip_size);
	if (ret)
		return ret;

	/* The image is in the volume now: it does not have to take heap space
	 * while the environment is rebuilt. */
	free(rebuild.fip);
	rebuild.fip = NULL;

	return 0;
}

/*
 * Step 5: the environment volume is gone with the old volume table, so put
 * one back and save the environment this session is running with into it -
 * everything else would be lost on the next reset, starting with the MAC
 * address of the network interfaces.  This is also what creates the board
 * data volumes step 0b read out, which the step after this one writes back.
 */
static int rebuild_env(void)
{
#if IS_ENABLED(CONFIG_ENV_IS_IN_UBI)

	if (!rebuild.active)
		return -EINVAL;

#if IS_ENABLED(CONFIG_ENV_UBI_VOLUME_CREATE)
	int ret;
	ret = env_ubi_volumes_create();
	if (ret)
		return ret;
#endif /* ENV_UBI_VOLUME_CREATE */

	if (rebuild.ethaddr_present) {
		env_set("ethaddr", rebuild.ethaddr);
	} else {
		/* Same address the network stack would fall back to, only
		 * this one is written back and stays. */
		uchar mac[ARP_HLEN];
		char buf[18];

		net_random_ethaddr(mac);
		snprintf(buf, sizeof(buf), "%pM", mac);
		env_set("ethaddr", buf);
		rebuild.ethaddr_generated = true;
	}

	return env_save();
#else
	/* Without a UBI environment there is no environment volume and no
	 * board data volume list: there is nothing to put back. */
	return rebuild.active ? 0 : -EINVAL;
#endif
}

/*
 * Step 6: write the board data volumes back, now that the environment driver
 * has created them again.
 */
static int rebuild_restore(void)
{
	const char *failed = NULL;

	if (!rebuild.active)
		return -EINVAL;

	return ubi_keep_restore(rebuild.keep, rebuild.kept,
				&rebuild.extra_bytes, &failed);
}

/* Send a JSON reply built here; it is freed once the session is over. */
static void rebuild_reply(struct httpd_response *response, int code,
			  const char *fallback, const char *fmt, ...)
{
	char *json = malloc(256);
	va_list ap;

	if (json) {
		va_start(ap, fmt);
		vscnprintf(json, 256, fmt, ap);
		va_end(ap);
	}

	failsafe_http_reply_json_alloc(response, code,
				       json ? json : fallback, json);
}

/*
 * Report the step that failed, by name.  The rebuild itself is left as it
 * is: the copies are still in the heap, so the page can ask for the step
 * again (see rebuild_finish()).
 */
static void rebuild_reply_fail(struct httpd_response *response,
			       const char *what, int err)
{
	rebuild_reply(response, 500, "{\"error\":\"rebuild failed\"}",
		      "{\"error\":\"%s failed (%d)\"}", what, err);
}

/**
 * ubi_rebuild_handler - POST /ubi/rebuild, one step per request
 *
 * Rebuild the UBI device from nothing.  The page drives the steps, so it
 * can say which one is running and how far the wipe got - and pick an
 * unfinished rebuild up again after a reload, instead of starting over on a
 * device that is already half erased:
 *
 *   op=status (default) - report whether a rebuild is under way, which step
 *                         it is in and how far it got.  Changes nothing, so
 *                         a request without "op" is safe;
 *   op=begin            - steps 0 and 0b: read the FIP and the board data
 *                         volumes into the heap and note the partition to
 *                         erase.  An unfinished rebuild is picked up
 *                         instead of read again - which would lose the
 *                         copies of a half erased device;
 *   op=erase            - step 1 ("ubi detach", on the first call) and one
 *                         chunk of step 2: wipe the next slice of the
 *                         partition, block by block, bad blocks skipped.
 *                         Asked for again until "done" has reached "total";
 *   op=format           - step 3: "ubi part <partition>", which formats the
 *                         partition and attaches a fresh, empty device;
 *   op=fip              - step 4: create the "fip" volume again and write
 *                         the saved image back into it;
 *   op=env              - step 5: give the environment a home again and save
 *                         it, with its ethaddr (a random one is generated
 *                         when the device has no usable address);
 *   op=restore          - step 6: write the board data volumes back, into
 *                         the volumes the step before it created;
 *   op=abort            - drop the copies and forget the rebuild.
 *
 * Everything after the wipe is a step of its own as well, because the page
 * has to be able to say what is running: a format is another whole-partition
 * operation, and the wipe of a partition is long enough on its own.
 *
 * Every volume the device does not need to boot is gone afterwards: that is
 * the point of the operation, and the page warns about it before asking.
 *
 * Form parameters:
 *   op       - the step, see above (optional, "status" by default)
 *   mtd_name - MTD partition holding UBI, for op=begin (optional: the
 *              partition the device is attached to right now, or "ubi")
 *
 * Returns JSON, one object per step.  All of them carry "step" (the step to
 * ask for next, see enum rebuild_step) and "progress" (where the progress
 * bar belongs after this reply, in percent); on top of that the step reports
 * what it did:
 *   {"ok":true,"active":false}                        (status, no rebuild)
 *   {"ok":true,"active":true,"mtd":"...","done":N,"total":N,
 *    "bad_blocks":N,"fip_bytes":N,"kept_bytes":N}     (status)
 *   {"ok":true,"mtd":"...","total":N,"erasesize":N,"chunk":N,"fip_bytes":N,
 *    "kept_bytes":N,"done":N,"resumed":false}         (begin)
 *   {"ok":true,"done":N,"total":N,"bad_blocks":N}     (erase)
 *   {"ok":true}                                       (format, fip, env)
 *   {"ok":true,"mtd":"...","fip_bytes":N,"extra_bytes":N,"env_restored":true,
 *    "ethaddr_generated":false}                       (restore)
 * or {"error":"<step> failed (<errno>)"}.
 */
void ubi_rebuild_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char mtd_name[UBI_MTD_NAME_MAX_LEN];
	char *op = NULL, *form_name = NULL;
	bool resumed;
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	(void)failsafe_get_form_value(request, "op", &op, 16, true, true);

	if (!op || !op[0] || !strcmp(op, "status")) {
		if (!rebuild.active)
			rebuild_reply(response, 200, "{\"ok\":true}",
				      "{\"ok\":true,\"active\":false}");
		else
			rebuild_reply(response, 200, "{\"ok\":true}",
				      "{\"ok\":true,\"active\":true,"
				      "\"mtd\":\"%s\",\"done\":%llu,"
				      "\"total\":%llu,\"bad_blocks\":%u,"
				      "\"fip_bytes\":%zu,\"kept_bytes\":%zu,"
				      "\"step\":%d,\"progress\":%u}",
				      rebuild.mtd_name,
				      (unsigned long long)rebuild.erase_done,
				      (unsigned long long)rebuild.erase_total,
				      rebuild.bad_blocks, rebuild.fip_size,
				      rebuild_kept_bytes(), rebuild.step,
				      rebuild_progress());

		free(op);
		return;
	}

	if (!strcmp(op, "abort")) {
		rebuild_release();
		rebuild_reply(response, 200, "{\"ok\":true}",
			      "{\"ok\":true,\"aborted\":true}");
		free(op);
		return;
	}

	if (!strcmp(op, "begin")) {
		/* Which partition to erase: the form value when given,
		 * otherwise the one the device is attached to, otherwise the
		 * usual name. */
		ret = failsafe_get_form_value(request, "mtd_name", &form_name,
					      UBI_MTD_NAME_MAX_LEN, true, true);
		if (ret == 0 && form_name && form_name[0])
			strlcpy(mtd_name, form_name, sizeof(mtd_name));
		else if (ubi_devices[0] && ubi_devices[0]->mtd &&
			 ubi_devices[0]->mtd->name)
			strlcpy(mtd_name, ubi_devices[0]->mtd->name,
				sizeof(mtd_name));
		else
			strlcpy(mtd_name, "ubi", sizeof(mtd_name));
		free(form_name);
		free(op);

		resumed = rebuild.active;

		ret = rebuild_read(mtd_name);
		if (ret) {
			rebuild_release();
			rebuild_reply_fail(response, "reading the volumes",
					   ret);
			return;
		}

		rebuild_reply(response, 200, "{\"ok\":true}",
			      "{\"ok\":true,\"mtd\":\"%s\",\"total\":%llu,"
			      "\"erasesize\":%u,\"chunk\":%u,\"fip_bytes\":%zu,"
			      "\"kept_bytes\":%zu,\"done\":%llu,"
			      "\"step\":%d,\"progress\":%u,"
			      "\"resumed\":%s}",
			      rebuild.mtd_name,
			      (unsigned long long)rebuild.erase_total,
			      rebuild.erasesize, UBI_REBUILD_ERASE_CHUNK,
			      rebuild.fip_size, rebuild_kept_bytes(),
			      (unsigned long long)rebuild.erase_done,
			      rebuild.step, rebuild_progress(),
			      resumed ? "true" : "false");
		return;
	}

	if (!strcmp(op, "erase")) {
		free(op);

		ret = rebuild_erase();
		if (ret) {
			rebuild_reply_fail(response, "the erase", ret);
			return;
		}

		/* The partition is gone: the format is next. */
		if (rebuild.erase_done >= rebuild.erase_total)
			rebuild.step = REBUILD_STEP_FORMAT;

		rebuild_reply(response, 200, "{\"ok\":true}",
			      "{\"ok\":true,\"done\":%llu,\"total\":%llu,"
			      "\"bad_blocks\":%u,\"step\":%d,\"progress\":%u}",
			      (unsigned long long)rebuild.erase_done,
			      (unsigned long long)rebuild.erase_total,
			      rebuild.bad_blocks, rebuild.step,
			      rebuild_progress());
		return;
	}

	/*
	 * Steps 3 to 6, one request each: the page walks them in order, and one
	 * that stops in the middle of them is picked up there again (see the
	 * "step" of op=begin).  Every step ends where the next one starts on
	 * the progress bar.
	 */
	{
		static const struct {
			const char *op;
			const char *what;
			int (*run)(void);
		} steps[] = {
			{ "format", "the format", rebuild_format },
			{ "fip", "writing the FIP back", rebuild_fip },
			{ "env", "the environment", rebuild_env },
			{ "restore", "writing the board data back",
			  rebuild_restore },
		};
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(steps); i++) {
			if (strcmp(op, steps[i].op))
				continue;

			free(op);

			ret = steps[i].run();
			if (ret) {
				rebuild_reply_fail(response, steps[i].what, ret);
				return;
			}

			if (i + 1 == ARRAY_SIZE(steps)) {
				u8 full = rebuild_step_percent[REBUILD_STEP_DONE];

				/* Everything is back: report what the rebuild
				 * ended up with and let it go. */
#if IS_ENABLED(CONFIG_ENV_IS_IN_UBI)
				rebuild_reply(response, 200, "{\"ok\":true}",
					      "{\"ok\":true,\"mtd\":\"%s\","
					      "\"fip_bytes\":%zu,"
					      "\"extra_bytes\":%zu,"
					      "\"env_restored\":true,"
					      "\"ethaddr_generated\":%s,"
					      "\"step\":%d,\"progress\":%u}",
					      rebuild.mtd_name, rebuild.fip_size,
					      rebuild.extra_bytes,
					      rebuild.ethaddr_generated ?
					      "true" : "false",
					      REBUILD_STEP_DONE, full);
#else
				rebuild_reply(response, 200, "{\"ok\":true}",
					      "{\"ok\":true,\"mtd\":\"%s\","
					      "\"fip_bytes\":%zu,"
					      "\"extra_bytes\":%zu,"
					      "\"env_restored\":false,"
					      "\"step\":%d,\"progress\":%u}",
					      rebuild.mtd_name, rebuild.fip_size,
					      rebuild.extra_bytes,
					      REBUILD_STEP_DONE, full);
#endif
				rebuild_release();
				return;
			}

			rebuild.step = REBUILD_STEP_FORMAT + i + 1;

			rebuild_reply(response, 200, "{\"ok\":true}",
				      "{\"ok\":true,\"step\":%d,"
				      "\"progress\":%u}",
				      rebuild.step, rebuild_progress());
			return;
		}
	}

	free(op);

	rebuild_reply(response, 400, "{\"error\":\"unknown_op\"}",
		      "{\"error\":\"unknown op\"}");
}


/**
 * ubi_create_vol_handler - POST /ubi/create
 *
 * Form parameters:
 *   name - Volume name
 *   size - Volume size in bytes (0 or empty for maximum)
 *   type - Volume type: "dynamic" or "static"
 *   skipcheck - Skip CRC check: "1" or "0"
 *
 * Returns JSON: {"ok":true} or {"error":"..."}
 */
void ubi_create_vol_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *name = NULL;
	char *size_str = NULL;
	char *type_str = NULL;
	char *skipcheck_str = NULL;
	char *json_out;
	int64_t size = 0;
	int dynamic = 1;
	bool skipcheck = false;
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	struct ubi_device *ubi = ubi_devices[0];

	if (!ubi) {
		json_out = strdup("{\"error\":\"no ubi device attached\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"no ubi device\"}",
			json_out);
		return;
	}

	/* Get volume name */
	ret = failsafe_get_form_value(request, "name", &name,
		UBI_VOL_NAME_MAX_LEN, false, false);
	if (ret || !name || !name[0]) {
		json_out = strdup("{\"error\":\"missing volume name\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing name\"}",
			json_out);
		return;
	}

	/* Get size (optional) */
	ret = failsafe_get_form_value(request, "size", &size_str, 32, false, true);
	if (ret == 0 && size_str && size_str[0]) {
		size = simple_strtoull(size_str, NULL, 0);
	}
	free(size_str);

	/* Get type (optional, default dynamic) */
	ret = failsafe_get_form_value(request, "type", &type_str, 16, false, true);
	if (ret == 0 && type_str) {
		if (strncmp(type_str, "s", 1) == 0)
			dynamic = 0;
	}
	free(type_str);

	/* Get skipcheck (optional) */
	ret = failsafe_get_form_value(request, "skipcheck", &skipcheck_str, 4, false, true);
	if (ret == 0 && skipcheck_str) {
		skipcheck = (skipcheck_str[0] == '1');
	}
	free(skipcheck_str);

	/* Use maximum available size if not specified */
	if (size <= 0) {
		size = (int64_t)ubi->avail_pebs * ubi->leb_size;
	}

	/* Create volume */
	ret = ubi_create_vol(name, size, dynamic, UBI_VOL_NUM_AUTO, skipcheck);
	free(name);

	if (ret) {
		json_out = malloc(128);
		if (json_out)
			snprintf(json_out, 128,
				"{\"error\":\"create failed: %d\"}", ret);
		failsafe_http_reply_json_alloc(response, 500,
			json_out ? json_out : "{\"error\":\"create failed\"}",
			json_out);
		return;
	}

	json_out = strdup("{\"ok\":true}");
	failsafe_http_reply_json_alloc(response, 200,
		json_out ? json_out : "{\"ok\":true}", json_out);
}

/**
 * ubi_remove_vol_handler - POST /ubi/remove
 *
 * Form parameters:
 *   name - Volume name to remove
 *
 * Returns JSON: {"ok":true} or {"error":"..."}
 */
void ubi_remove_vol_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *name = NULL;
	char *json_out;
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	struct ubi_device *ubi = ubi_devices[0];

	if (!ubi) {
		json_out = strdup("{\"error\":\"no ubi device attached\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"no ubi device\"}",
			json_out);
		return;
	}

	/* Get volume name */
	ret = failsafe_get_form_value(request, "name", &name,
		UBI_VOL_NAME_MAX_LEN, false, false);
	if (ret || !name || !name[0]) {
		json_out = strdup("{\"error\":\"missing volume name\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing name\"}",
			json_out);
		return;
	}

	/* Remove volume */
	ret = ubi_remove_vol(name);
	free(name);

	if (ret) {
		json_out = malloc(128);
		if (json_out)
			snprintf(json_out, 128,
				"{\"error\":\"remove failed: %d\"}", ret);
		failsafe_http_reply_json_alloc(response, 500,
			json_out ? json_out : "{\"error\":\"remove failed\"}",
			json_out);
		return;
	}

	json_out = strdup("{\"ok\":true}");
	failsafe_http_reply_json_alloc(response, 200,
		json_out ? json_out : "{\"ok\":true}", json_out);
}

/**
 * ubi_rename_vol_handler - POST /ubi/rename
 *
 * Form parameters:
 *   old_name - Current volume name
 *   new_name - New volume name
 *
 * Returns JSON: {"ok":true} or {"error":"..."}
 */
void ubi_rename_vol_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *old_name = NULL;
	char *new_name = NULL;
	char *json_out;
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	struct ubi_device *ubi = ubi_devices[0];

	if (!ubi) {
		json_out = strdup("{\"error\":\"no ubi device attached\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"no ubi device\"}",
			json_out);
		return;
	}

	/* Get old name */
	ret = failsafe_get_form_value(request, "old_name", &old_name,
		UBI_VOL_NAME_MAX_LEN, false, false);
	if (ret || !old_name || !old_name[0]) {
		json_out = strdup("{\"error\":\"missing old_name\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing old_name\"}",
			json_out);
		return;
	}

	/* Get new name */
	ret = failsafe_get_form_value(request, "new_name", &new_name,
		UBI_VOL_NAME_MAX_LEN, false, false);
	if (ret || !new_name || !new_name[0]) {
		free(old_name);
		json_out = strdup("{\"error\":\"missing new_name\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing new_name\"}",
			json_out);
		return;
	}

	/* Find volume */
	struct ubi_volume *vol;
	vol = ubi_find_volume(old_name);
	if (!vol) {
		free(old_name);
		free(new_name);
		json_out = strdup("{\"error\":\"volume not found\"}");
		failsafe_http_reply_json_alloc(response, 404,
			json_out ? json_out : "{\"error\":\"not found\"}",
			json_out);
		return;
	}

	/* Rename volume */
	struct ubi_rename_entry rename;
	struct ubi_volume_desc desc;
	struct list_head list;

	rename.new_name_len = strlen(new_name);
	strcpy(rename.new_name, new_name);
	rename.remove = 0;
	desc.vol = vol;
	desc.mode = 0;
	rename.desc = &desc;
	INIT_LIST_HEAD(&rename.list);
	INIT_LIST_HEAD(&list);
	list_add(&rename.list, &list);

	ret = ubi_rename_volumes(ubi, &list);
	free(old_name);
	free(new_name);

	if (ret) {
		json_out = malloc(128);
		if (json_out)
			snprintf(json_out, 128,
				"{\"error\":\"rename failed: %d\"}", ret);
		failsafe_http_reply_json_alloc(response, 500,
			json_out ? json_out : "{\"error\":\"rename failed\"}",
			json_out);
		return;
	}

	json_out = strdup("{\"ok\":true}");
	failsafe_http_reply_json_alloc(response, 200,
		json_out ? json_out : "{\"ok\":true}", json_out);
}

/**
 * ubi_mtd_list_handler - GET /ubi/mtd_list
 *
 * Returns JSON array of available MTD partitions:
 * {"partitions":[{"name":"...","size":0,"type":"..."},...]}
 */
void ubi_mtd_list_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *buf;
	int len = 0;
	int left = UBI_JSON_BUF_SZ;
	bool first = true;
	struct mtd_info *mtd;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_GET) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	buf = malloc(left);
	if (!buf) {
		failsafe_http_reply_json(response, 500, "{\"error\":\"oom\"}");
		return;
	}

	len = buf_appendf(buf, left, len, "{\"partitions\":[");

	/* Probe all MTD devices */
	mtd_probe_devices();

	mtd_for_each_device(mtd) {
		if (len >= left - 256)
			break;

		len = buf_appendf(buf, left, len,
			"%s{\"name\":\"%s\",\"size\":%llu,\"erasesize\":%lu}",
			first ? "" : ",",
			mtd->name,
			(unsigned long long)mtd->size,
			(unsigned long)mtd->erasesize);

		first = false;
	}

	len = buf_appendf(buf, left, len, "]}");

	failsafe_http_reply_json_alloc(response, 200, buf, buf);
}

/**
 * ubi_backup_handler - POST /ubi/backup
 *
 * Form parameters:
 *   name - Volume name to backup/download
 *
 * Returns the volume content as a binary download.
 */
void ubi_backup_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	struct ubi_backup_session {
		struct ubi_volume *vol;
		struct ubi_device *ubi;
		u64 total;
		u64 cur;
		void *buf;
		size_t buf_size;
		char hdr[512];
		int hdr_len;
	} *st;
	struct httpd_form_value *name_val;
	char *vol_name = NULL;
	char filename[128];
	int ret;

	failsafe_free_session(status, response);

	if (status == HTTP_CB_RESPONDING) {
		u64 remain;
		size_t to_read;

		st = response->session_data;
		if (!st) {
			response->status = HTTP_RESP_NONE;
			return;
		}

		remain = st->total - st->cur;
		if (!remain) {
			response->status = HTTP_RESP_NONE;
			return;
		}

		to_read = (size_t)min_t(u64, remain, st->buf_size);

		ret = ubi_volume_read(st->vol->name, st->buf, st->cur, to_read);
		if (ret) {
			response->status = HTTP_RESP_NONE;
			return;
		}

		st->cur += to_read;

		response->status = HTTP_RESP_CUSTOM;
		response->data = (const char *)st->buf;
		response->size = to_read;
		return;
	}

	if (status == HTTP_CB_CLOSED) {
		st = response->session_data;
		if (st) {
			free(st->buf);
			free(st);
		}
		return;
	}

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	/* Get volume name */
	name_val = httpd_request_find_value(request, "name");
	if (!name_val || !name_val->data || !name_val->size) {
		failsafe_http_reply_text(response, 400, "missing name");
		return;
	}

	if (name_val->size > UBI_VOL_NAME_MAX_LEN) {
		failsafe_http_reply_text(response, 400, "name too long");
		return;
	}

	vol_name = malloc(name_val->size + 1);
	if (!vol_name) {
		failsafe_http_reply_text(response, 500, "oom");
		return;
	}

	memcpy(vol_name, name_val->data, name_val->size);
	vol_name[name_val->size] = '\0';

	/* Check if UBI is attached */
	struct ubi_device *ubi = ubi_devices[0];

	if (!ubi) {
		free(vol_name);
		failsafe_http_reply_text(response, 400, "no ubi device");
		return;
	}

	/* Find volume */
	struct ubi_volume *vol = ubi_find_volume(vol_name);
	if (!vol) {
		free(vol_name);
		failsafe_http_reply_text(response, 404, "volume not found");
		return;
	}

	/* Allocate session */
	st = calloc(1, sizeof(*st));
	if (!st) {
		free(vol_name);
		failsafe_http_reply_text(response, 500, "oom");
		return;
	}

	st->buf_size = 64 * 1024;
	st->buf = malloc(st->buf_size);
	if (!st->buf) {
		free(st);
		free(vol_name);
		failsafe_http_reply_text(response, 500, "oom");
		return;
	}

	st->vol = vol;
	st->ubi = ubi;
	st->total = (u64)vol->used_bytes;
	st->cur = 0;

	/* Generate filename */
	{
		char safe_name[64];
		const char *p;
		size_t i;

		/* Sanitize volume name for filename */
		p = vol_name;
		for (i = 0; i < sizeof(safe_name) - 1 && *p; i++, p++) {
			unsigned char c = *p;
			if (isalnum(c) || c == '-' || c == '_')
				safe_name[i] = c;
			else
				safe_name[i] = '_';
		}
		safe_name[i] = '\0';

		snprintf(filename, sizeof(filename), "ubi_%s.bin", safe_name);
	}

	free(vol_name);

	/* Build HTTP header */
	st->hdr_len = snprintf(st->hdr, sizeof(st->hdr),
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: application/octet-stream\r\n"
		"Content-Length: %llu\r\n"
		"Content-Disposition: attachment; filename=\"%s\"\r\n"
		"Cache-Control: no-store\r\n"
		"Connection: close\r\n"
		"\r\n",
		(unsigned long long)st->total,
		filename);

	response->session_data = st;
	response->status = HTTP_RESP_CUSTOM;
	response->data = st->hdr;
	response->size = st->hdr_len;
}

/**
 * ubi_check_vol_handler - POST /ubi/check
 *
 * Form parameters:
 *   name - Volume name to check
 *
 * Returns JSON: {"exists":true} or {"exists":false}
 */
void ubi_check_vol_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *name = NULL;
	char *json_out;
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	struct ubi_device *ubi = ubi_devices[0];

	if (!ubi) {
		json_out = strdup("{\"error\":\"no ubi device attached\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"no ubi device\"}",
			json_out);
		return;
	}

	/* Get volume name */
	ret = failsafe_get_form_value(request, "name", &name,
		UBI_VOL_NAME_MAX_LEN, false, false);
	if (ret || !name || !name[0]) {
		json_out = strdup("{\"error\":\"missing volume name\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing name\"}",
			json_out);
		return;
	}

	/* Check volume existence */
	struct ubi_volume *vol = ubi_find_volume(name);
	free(name);

	json_out = malloc(64);
	if (json_out)
		snprintf(json_out, 64, "{\"exists\":%s}",
			vol ? "true" : "false");
	failsafe_http_reply_json_alloc(response, 200,
		json_out ? json_out : "{\"exists\":false}", json_out);
}

/**
 * ubi_write_vol_handler - POST /ubi/write
 *
 * Form parameters:
 *   name      - Volume name to write
 *   data      - File content to write (binary safe)
 *   offset    - Write offset in bytes (optional, default 0)
 *   full_size - Total size of the update (optional, for partial updates)
 *
 * Behavior:
 *   - offset > 0:   offset-based write at the given byte offset
 *   - offset == 0 && full_size > 0 && full_size != size:
 *                   begin a partial update declaring full_size as total
 *   - otherwise:    full volume update with size == full_size
 *
 * Returns JSON: {"ok":true} or {"error":"..."}
 */
void ubi_write_vol_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	struct httpd_form_value *name_val;
	struct httpd_form_value *data_val;
	char *vol_name = NULL;
	char *offset_str = NULL;
	char *full_str = NULL;
	char *json_out;
	loff_t offset = 0;
	size_t full_size = 0;
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	struct ubi_device *ubi = ubi_devices[0];

	if (!ubi) {
		json_out = strdup("{\"error\":\"no ubi device attached\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"no ubi device\"}",
			json_out);
		return;
	}

	/* Get volume name (binary-safe form value) */
	name_val = httpd_request_find_value(request, "name");
	if (!name_val || !name_val->data || !name_val->size) {
		json_out = strdup("{\"error\":\"missing volume name\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing name\"}",
			json_out);
		return;
	}

	if (name_val->size > UBI_VOL_NAME_MAX_LEN) {
		json_out = strdup("{\"error\":\"volume name too long\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"name too long\"}",
			json_out);
		return;
	}

	vol_name = malloc(name_val->size + 1);
	if (!vol_name) {
		failsafe_http_reply_json(response, 500, "{\"error\":\"oom\"}");
		return;
	}

	memcpy(vol_name, name_val->data, name_val->size);
	vol_name[name_val->size] = '\0';

	/* Get file data (binary safe) */
	data_val = httpd_request_find_value(request, "data");
	if (!data_val || !data_val->data || !data_val->size) {
		free(vol_name);
		json_out = strdup("{\"error\":\"missing data\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing data\"}",
			json_out);
		return;
	}

	/* Get optional offset */
	ret = failsafe_get_form_value(request, "offset", &offset_str, 32,
		true, true);
	if (ret == 0 && offset_str && offset_str[0]) {
		offset = (loff_t)simple_strtoull(offset_str, NULL, 0);
		if (offset < 0)
			offset = 0;
	}
	free(offset_str);

	/* Get optional full_size */
	ret = failsafe_get_form_value(request, "full_size", &full_str, 32,
		true, true);
	if (ret == 0 && full_str && full_str[0])
		full_size = (size_t)simple_strtoull(full_str, NULL, 0);
	free(full_str);

	/* Write data */
	if (offset > 0) {
		ret = ubi_volume_write(vol_name, data_val->data,
			offset, data_val->size);
	} else if (full_size > 0 && full_size != data_val->size) {
		ret = ubi_volume_begin_write(vol_name, data_val->data,
			data_val->size, full_size);
	} else {
		ret = ubi_volume_write(vol_name, data_val->data,
			0, data_val->size);
	}
	free(vol_name);

	if (ret) {
		json_out = malloc(128);
		if (json_out)
			snprintf(json_out, 128,
				"{\"error\":\"write failed: %d\"}", ret);
		failsafe_http_reply_json_alloc(response, 500,
			json_out ? json_out : "{\"error\":\"write failed\"}",
			json_out);
		return;
	}

	json_out = strdup("{\"ok\":true}");
	failsafe_http_reply_json_alloc(response, 200,
		json_out ? json_out : "{\"ok\":true}", json_out);
}

/**
 * ubi_skipcheck_handler - POST /ubi/skipcheck
 *
 * Form parameters:
 *   name - Volume name
 *   mode - "on" or "1" to enable skip check, "off" or "0" to disable
 *
 * Returns JSON: {"ok":true} or {"error":"..."}
 */
void ubi_skipcheck_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *name = NULL;
	char *mode = NULL;
	char *json_out;
	bool skip_check;
	int ret;

	failsafe_free_session(status, response);

	if (status != HTTP_CB_NEW)
		return;

	if (!request || request->method != HTTP_POST) {
		failsafe_http_reply_text(response, 405, "method");
		return;
	}

	struct ubi_device *ubi = ubi_devices[0];

	if (!ubi) {
		json_out = strdup("{\"error\":\"no ubi device attached\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"no ubi device\"}",
			json_out);
		return;
	}

	/* Get volume name */
	ret = failsafe_get_form_value(request, "name", &name,
		UBI_VOL_NAME_MAX_LEN, false, false);
	if (ret || !name || !name[0]) {
		json_out = strdup("{\"error\":\"missing volume name\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing name\"}",
			json_out);
		return;
	}

	/* Get mode */
	ret = failsafe_get_form_value(request, "mode", &mode, 8, false, false);
	if (ret || !mode || !mode[0]) {
		free(name);
		json_out = strdup("{\"error\":\"missing mode\"}");
		failsafe_http_reply_json_alloc(response, 400,
			json_out ? json_out : "{\"error\":\"missing mode\"}",
			json_out);
		return;
	}

	skip_check = (mode[0] == 'o' && mode[1] == 'n') ||
		     (mode[0] == '1');
	free(mode);

	/* Find volume */
	struct ubi_volume *vol = ubi_find_volume(name);
	if (!vol) {
		free(name);
		json_out = strdup("{\"error\":\"volume not found\"}");
		failsafe_http_reply_json_alloc(response, 404,
			json_out ? json_out : "{\"error\":\"not found\"}",
			json_out);
		return;
	}

	/* Set/clear skip check flag */
	ret = ubi_set_skip_check(name, skip_check);
	free(name);

	if (ret) {
		json_out = malloc(128);
		if (json_out)
			snprintf(json_out, 128,
				"{\"error\":\"skipcheck failed: %d\"}", ret);
		failsafe_http_reply_json_alloc(response, 500,
			json_out ? json_out : "{\"error\":\"skipcheck failed\"}",
			json_out);
		return;
	}

	json_out = malloc(96);
	if (json_out)
		snprintf(json_out, 96, "{\"ok\":true,\"skip_check\":%s}",
			skip_check ? "true" : "false");
	failsafe_http_reply_json_alloc(response, 200,
		json_out ? json_out : "{\"ok\":true}", json_out);
}

#ifdef CONFIG_WEBUI_FAILSAFE_UBI
void ubi_register_handlers(struct httpd_instance *inst)
{
	/* The page and its script are registered by the page inventory
	 * (failsafe/pages.c); this module only owns the endpoints. */
	httpd_register_uri_handler(inst, "/ubi/info", &ubi_info_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/volumes", &ubi_volumes_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/attach", &ubi_attach_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/detach", &ubi_detach_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/rebuild", &ubi_rebuild_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/create", &ubi_create_vol_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/remove", &ubi_remove_vol_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/rename", &ubi_rename_vol_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/check", &ubi_check_vol_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/write", &ubi_write_vol_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/skipcheck", &ubi_skipcheck_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/mtd_list", &ubi_mtd_list_handler, NULL);
	httpd_register_uri_handler(inst, "/ubi/backup", &ubi_backup_handler, NULL);
}
#endif
