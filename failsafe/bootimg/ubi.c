// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Failsafe storage backend: UBI volumes.
 *
 * One of the three backends behind failsafe_storage_write() /
 * failsafe_storage_read() (see failsafe/bootimg/storage.c); the other two
 * are the raw MTD partitions (failsafe/bootimg/mtd.c) and the MMC
 * partitions (failsafe/bootimg/mmc.c).
 *
 * This is the backend the system image of the boards supported today uses:
 * the firmware ("fit") and the FIP ("fip", a static volume of
 * FAILSAFE_STORAGE_STATIC_SIZE) live in UBI volumes next to the OpenWrt
 * overlay ("rootfs_data").
 *
 * The file is only built when CONFIG_CMD_UBI is enabled (see the Makefile)
 * and its callers guard their use of it, so a board that does not store
 * anything in UBI volumes carries none of this code.
 */

#include <command.h>
#include <errno.h>
#include <linux/string.h>
#include <vsprintf.h>
#include <ubi_uboot.h>

#include <failsafe/storage.h>
#include <cprint.h>
#include <failsafe/error.h>

#if IS_ENABLED(CONFIG_CMD_UBI)

/*
 * Ensure UBI is attached to the UBI MTD partition (usually "ubi").
 * Safe to call even if already attached.
 */
int failsafe_ubi_attach(void)
{
	return run_command("ubi part ubi", 0);
}

/*
 * Remove the OpenWrt "rootfs_data" overlay volume, if present.
 *
 * "rootfs_data" is created as a dynamic volume spanning the maximum
 * available size ("ubi create rootfs_data - dynamic"), so it owns every
 * free PEB of the UBI device.  The "fit" volume is created from that very
 * same pool, so the overlay has to be dropped *before* the new "fit"
 * volume is created - otherwise "ubi create fit <size> dynamic" fails
 * with "not enough PEBs, only N available" as soon as the device already
 * carries an overlay, which is the normal case after the first boot.
 *
 * A missing volume is not an error: "ubi check" then returns non-zero and
 * there is nothing to free.
 */
static int failsafe_remove_rootfs_data(void)
{
	char cmd[256];
	int ret;

	snprintf(cmd, sizeof(cmd), "ubi check rootfs_data");
	if (run_command(cmd, 0))
		return 0;

	snprintf(cmd, sizeof(cmd), "ubi remove rootfs_data");
	ret = run_command(cmd, 0);
	if (ret) {
		return failsafe_error(-EIO, "remove 'rootfs_data' failed "
			"(ret=%d)", ret);
	}

	return 0;
}

/*
 * Rebuild the OpenWrt "rootfs_data" overlay volume after a FIT upgrade.
 *
 * A FIT upgrade deliberately starts from a fresh overlay: the volume is
 * recreated as a dynamic volume spanning all space left over by the new
 * "fit" volume ("-" = maximum available size).
 */
static int failsafe_recreate_rootfs_data(void)
{
	char cmd[256];
	int ret;

	snprintf(cmd, sizeof(cmd), "ubi create rootfs_data - dynamic");
	ret = run_command(cmd, 0);
	if (ret) {
		return failsafe_error(-EIO, "'ubi create rootfs_data' failed "
			"(ret=%d)", ret);
	}

	cprintln(SUCCESS, "Failsafe: 'rootfs_data' recreated");
	return 0;
}

int failsafe_ubi_capacity(const char *name, size_t size)
{
	if (!strcmp(name, FAILSAFE_STORAGE_STATIC_TARGET) &&
	    size > FAILSAFE_STORAGE_STATIC_SIZE) {
		/* The "fip" static volume is created at the configured
		 * FAILSAFE_STORAGE_STATIC_SIZE (see failsafe_ubi_write), so
		 * reject images that would not fit - otherwise "ubi write"
		 * fails after the volume has already been recreated empty.
		 */
		return failsafe_error(-EFBIG,
			"'%s' image too big (%zu > 0x%zx)", name, size,
			(size_t)FAILSAFE_STORAGE_STATIC_SIZE);
	}

	return 0;
}

int failsafe_ubi_write(const char *name, const void *data, size_t size)
{
	char cmd[256];
	int ret;

	ret = failsafe_ubi_attach();
	if (ret) {
		return failsafe_error(-EIO, "cannot attach UBI (ret=%d)", ret);
	}

	/*
	 * "fit" shares the UBI device with the OpenWrt overlay volume,
	 * which is created with the maximum available size: the overlay has
	 * to be dropped first, otherwise no PEBs are left for the new "fit"
	 * volume.  It is rebuilt once the FIT image has been written.
	 */
	if (!strcmp(name, FAILSAFE_STORAGE_FIT_TARGET)) {
		ret = failsafe_remove_rootfs_data();
		if (ret)
			return ret;
	}

	/* Remove old volume if it exists (best-effort). */
	snprintf(cmd, sizeof(cmd), "ubi check %s && ubi remove %s", name,
		 name);
	run_command(cmd, 0);

	/*
	 * Create the new volume.  "fip" is a static volume created at the
	 * configured FAILSAFE_STORAGE_STATIC_SIZE, exactly like the console
	 * "ubi_write_fip" environment command; "ubi write" later records the
	 * actual image size as used_bytes.  All other UBI targets are dynamic
	 * volumes sized to the uploaded image ("ubi create fit $filesize
	 * dynamic").
	 */
	if (!strcmp(name, FAILSAFE_STORAGE_STATIC_TARGET))
		snprintf(cmd, sizeof(cmd), "ubi create %s 0x%zx static", name,
			 (size_t)FAILSAFE_STORAGE_STATIC_SIZE);
	else
		snprintf(cmd, sizeof(cmd), "ubi create %s 0x%zx dynamic", name,
			 size);

	ret = run_command(cmd, 0);
	if (ret) {
		return failsafe_error(-EIO, "'ubi create %s' failed (ret=%d)",
			name, ret);
	}

	snprintf(cmd, sizeof(cmd), "ubi write 0x%lx %s 0x%lx",
		 (unsigned long)(uintptr_t)data, name, (unsigned long)size);
	ret = run_command(cmd, 0);
	if (ret) {
		return failsafe_error(-EIO, "'ubi write %s' failed (ret=%d)",
			name, ret);
	}

	/*
	 * FIT upgrade: rebuild the OpenWrt "rootfs_data" volume that was
	 * removed above, so the device boots with a fresh overlay spanning
	 * the space left by the new "fit" volume.
	 */
	if (!strcmp(name, FAILSAFE_STORAGE_FIT_TARGET)) {
		ret = failsafe_recreate_rootfs_data();
		if (ret)
			return ret;
	}

	return 0;
}

int failsafe_ubi_read(const char *name, u64 off, void *buf, size_t max_len,
		      size_t *read_len)
{
	struct ubi_volume *vol;
	size_t len;
	int ret;

	/*
	 * Attach only when UBI is not up yet: re-attaching would tear down
	 * and rebuild the volume structures, invalidating an in-flight
	 * session (e.g. a streamed volume backup).
	 */
	if (!ubi_devices[0]) {
		ret = failsafe_ubi_attach();
		if (ret)
			return -EIO;
	}

	vol = ubi_find_volume(name);
	if (!vol)
		return -ENODEV;

	if (off >= (u64)vol->used_bytes)
		return -EINVAL;

	len = (u64)vol->used_bytes - off;
	if (len > max_len)
		len = max_len;

	ret = ubi_volume_read(name, buf, off, len);
	if (ret)
		return -EIO;

	if (read_len)
		*read_len = len;

	return 0;
}

#endif /* CONFIG_CMD_UBI */
