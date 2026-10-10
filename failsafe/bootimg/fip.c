// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Failsafe boot-image helper: generic FIP (Firmware Image Package)
 * parsing shared by the Airoha / MediaTek board code and by the
 * per-platform image validators.
 *
 * On-disk layout (see doc/board/airoha/boot-images.rst):
 *   header: magic u32 le (0xAA640001) + serial u32 le + flags u64 le
 *   ToC:    40-byte entries (uuid[16], offset u64 le, size u64 le, flags)
 *           terminated by an all-zero uuid entry.
 * Payload offsets are relative to the FIP start.
 */

#include <errno.h>
#include <stdio.h>
#include <linux/string.h>
#include <asm/unaligned.h>

#include <failsafe/fip.h>
#include <failsafe/image.h>
#include <cprint.h>
#include <failsafe/error.h>

const u8 failsafe_fip_uuid_tb_fw[16] = {
	0x5f, 0xf9, 0xec, 0x0b, 0x4d, 0x22, 0x3e, 0x4d,
	0xa5, 0x44, 0xc3, 0x9d, 0x81, 0xc7, 0x3f, 0x0a,
};

const u8 failsafe_fip_uuid_soc_fw[16] = {
	0x47, 0xd4, 0x08, 0x6d, 0x4c, 0xfe, 0x98, 0x46,
	0x9b, 0x95, 0x29, 0x50, 0xcb, 0xbd, 0x5a, 0x00,
};

const u8 failsafe_fip_uuid_nt_fw[16] = {
	0xd6, 0xd0, 0xee, 0xa7, 0xfc, 0xea, 0xd5, 0x4b,
	0x97, 0x82, 0x99, 0x34, 0xf2, 0x34, 0xb6, 0xe4,
};

bool failsafe_fip_check(const void *data, size_t size, size_t fip_off)
{
	if (fip_off > size ||
	    size - fip_off < FAILSAFE_FIP_HEADER_SIZE)
		return false;

	return get_unaligned_le32((const u8 *)data + fip_off) ==
	       FAILSAFE_FIP_MAGIC;
}

/*
 * Walk the ToC at data + fip_off looking for @want_uuid (NULL: the first
 * non-terminal entry).  With @first_only unset every matching entry is
 * checked and the first one is reported, which is what the "any entry is
 * fine" validator of the 'fip' volume needs.
 *
 * Returns 0 when an entry was found and its payload is contained in the
 * image, -ENOENT when no matching entry exists, -EINVAL when a matched
 * entry points outside the image (the declared offset/size are still
 * returned so the caller can print them).
 */
static int fip_find_entry(const void *data, size_t size, size_t fip_off,
			  const u8 *want_uuid, bool first_only,
			  const u8 **payload, size_t *payload_size,
			  u64 *payload_off)
{
	const u8 *base = (const u8 *)data + fip_off;
	size_t avail, pos;
	bool found = false;

	avail = size - fip_off - FAILSAFE_FIP_HEADER_SIZE;

	for (pos = 0; pos + FAILSAFE_FIP_TOC_ENTRY_SIZE <= avail;
	     pos += FAILSAFE_FIP_TOC_ENTRY_SIZE) {
		const u8 *e = base + FAILSAFE_FIP_HEADER_SIZE + pos;
		u64 off, len;
		int i, terminal = 1;

		/* A terminal entry has an all-zero UUID. */
		for (i = 0; i < 16; i++)
			if (e[i]) {
				terminal = 0;
				break;
			}
		if (terminal)
			break;

		if (want_uuid && memcmp(e, want_uuid, 16))
			continue;

		off = get_unaligned_le64(e + 16);
		len = get_unaligned_le64(e + 24);

		if (payload_off)
			*payload_off = off;
		if (payload_size)
			*payload_size = len;

		if (!len || off > size - fip_off ||
		    len > size - fip_off - off) {
			if (payload)
				*payload = NULL;
			return -EINVAL;
		}

		if (!found) {
			if (payload)
				*payload = base + off;
			found = true;
		}

		if (first_only)
			return 0;
	}

	return found ? 0 : -ENOENT;
}

int failsafe_fip_find(const void *data, size_t size, size_t fip_off,
		      const u8 *uuid, const u8 **payload,
		      size_t *payload_size, u64 *payload_off)
{
	if (!data || !failsafe_fip_check(data, size, fip_off))
		return -ENOENT;

	return fip_find_entry(data, size, fip_off, uuid, true, payload,
			      payload_size, payload_off);
}

int failsafe_fip_find_at(const void *data, size_t size,
			 const size_t *fip_offsets, size_t num_offsets,
			 const u8 *uuid, const u8 **payload,
			 size_t *payload_size)
{
	size_t i;

	if (!fip_offsets || !num_offsets)
		return -ENOENT;

	for (i = 0; i < num_offsets; i++) {
		if (failsafe_fip_check(data, size, fip_offsets[i]))
			return failsafe_fip_find(data, size, fip_offsets[i],
						 uuid, payload, payload_size,
						 NULL);
	}

	return -ENOENT;
}

int failsafe_fip_validate(const void *data, size_t size, size_t fip_off,
			  const u8 *want_uuid, const char *what)
{
	const u8 *payload;
	size_t payload_size;
	u64 off;
	int ret;

	if (fip_off > size ||
	    size - fip_off < FAILSAFE_FIP_HEADER_SIZE +
			     FAILSAFE_FIP_TOC_ENTRY_SIZE) {
		return failsafe_error(-EINVAL, "'%s' image too small for a FIP",
			what);
	}

	if (!failsafe_fip_check(data, size, fip_off)) {
		return failsafe_error(-EINVAL, "'%s' image has no FIP ToC "
			"(magic 0x%08x)", what, get_unaligned_le32((const u8 *)data + fip_off));
	}

	/*
	 * With @want_uuid the entry must be present and contained in the
	 * image.  Without it every non-terminal entry is checked and an
	 * empty ToC is accepted (mirrors the original validators).
	 */
	ret = fip_find_entry(data, size, fip_off, want_uuid,
			     want_uuid != NULL, &payload, &payload_size, &off);
	if (ret == -EINVAL) {
		return failsafe_error(-EINVAL, "'%s' payload (off 0x%llx, len "
			"0x%llx) exceeds image (%zu)", what, (unsigned long long)off,
			(unsigned long long)payload_size, size);
	}

	if (ret == -ENOENT && want_uuid) {
		return failsafe_error(-EINVAL, "'%s' FIP has no expected image "
			"entry", what);
	}

	return 0;
}

int failsafe_fip_check_model(const void *data, size_t size, size_t fip_off,
			     const char *what)
{
	const u8 *payload;
	size_t payload_size;
	int ret;

	ret = failsafe_fip_find(data, size, fip_off, failsafe_fip_uuid_nt_fw,
				&payload, &payload_size, NULL);

	/*
	 * -ENOENT covers both "no FIP here" and "a FIP without an nt-fw
	 * entry" (a BL31-only FIP is a valid upload): nothing to compare
	 * the board against, and the structural validator has already had
	 * its say about the container itself.
	 */
	if (ret == -ENOENT)
		return 0;

	/* A matched entry that points outside the image is a defect the
	 * structural validator reports as well, but it must not pass as a
	 * silently skipped board check. */
	if (ret)
		return failsafe_error(-EINVAL, "'%s' FIP U-Boot (nt-fw) entry "
			"is out of range", what);

	return failsafe_uboot_check_model(payload, payload_size, what);
}
