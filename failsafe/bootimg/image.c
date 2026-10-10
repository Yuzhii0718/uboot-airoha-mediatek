// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Failsafe boot-image helper: FIT / legacy uImage validation and the
 * RAM-boot path, shared by the Airoha / MediaTek board code and by the
 * per-platform image validators.
 */

#include <asm/global_data.h>
#include <asm/unaligned.h>
#include <command.h>
#include <cpu_func.h>
#include <env.h>
#include <errno.h>
#include <image.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/libfdt.h>
#include <linux/string.h>
#include <malloc.h>
#include <vsprintf.h>

#include <failsafe/image.h>
#include <failsafe/compress.h>
#include <cprint.h>
#include <failsafe/error.h>

#if CONFIG_IS_ENABLED(FIT)
int failsafe_image_validate_fit(const void *data, size_t size,
				const char *what)
{
	if (size < 4) {
		return failsafe_error(-EINVAL, "'%s' image too small (%zu)",
			what, size);
	}

	if (fit_check_format(data, size)) {
		return failsafe_error(-EINVAL, "'%s' image is not a valid FIT",
			what);
	}

	return 0;
}
#else
int failsafe_image_validate_fit(const void *data, size_t size,
				const char *what)
{
	(void)data;
	(void)size;
	return failsafe_error(-EINVAL, "'%s' image rejected (no FIT support "
		"built in)", what);
}
#endif

bool failsafe_image_is_legacy(const void *data, size_t size)
{
	return size >= image_get_header_size() &&
	       image_check_magic((const struct legacy_img_hdr *)data);
}

int failsafe_image_validate_legacy(const void *data, size_t size,
				   const char *what)
{
	const struct legacy_img_hdr *hdr = data;
	size_t dsize;

	if (size < image_get_header_size()) {
		return failsafe_error(-EINVAL,
			"'%s' legacy image too small (%zu)", what, size);
	}
	if (!image_check_magic(hdr)) {
		return failsafe_error(-EINVAL,
			"'%s' has no legacy uImage magic", what);
	}
	if (!image_check_hcrc(hdr)) {
		return failsafe_error(-EINVAL,
			"'%s' legacy header CRC mismatch", what);
	}

	dsize = image_get_data_size(hdr);
	if (dsize > size - image_get_header_size()) {
		return failsafe_error(-EINVAL, "'%s' legacy payload (0x%zx) "
			"exceeds image (%zu)", what, dsize, size);
	}
	if (!image_check_dcrc(hdr)) {
		return failsafe_error(-EINVAL,
			"'%s' legacy payload CRC mismatch", what);
	}

	return 0;
}

int failsafe_image_validate_firmware(const void *data, size_t size,
				     const char *what)
{
	if (failsafe_image_validate_fit(data, size, what))
		return -EINVAL;

	/* Strict board-model gate, on unless disabled by the environment
	 * variable 'failsafe_strict_model'. */
	return failsafe_firmware_check_model(data, size, what);
}

/* ------------------------------------------------------------------ */
/*  Strict board-model validation (env 'failsafe_strict_model')        */
/* ------------------------------------------------------------------ */

/* Number of root 'compatible' entries compared; mirrors the reference
 * implementation (struct compat_list in the MediaTek tree). */
#define FAILSAFE_MODEL_COMPAT_MAX	10

/*
 * Strict board-model validation is on by default: only an explicit "0"
 * in the environment disables it.  It rejects a boot image that was
 * built for a different board than the one this recovery runs on.
 */
#if CONFIG_IS_ENABLED(FIT) || CONFIG_IS_ENABLED(OF_LIBFDT)
static bool failsafe_strict_model_enabled(void)
{
	const char *strict = env_get("failsafe_strict_model");

	return !(strict && !strcmp(strict, "0"));
}
#endif

#if CONFIG_IS_ENABLED(FIT)
/*
 * Strict model validation of a firmware (FIT) image.
 *
 * The recovery runs on one specific board, identified by the 'compatible'
 * string in this U-Boot's control device tree (e.g. "nokia,xg-040g-md").
 * In strict mode a FIT firmware is only accepted when it declares a
 * 'compatible' that matches the running board - using the same matching
 * logic U-Boot itself uses to pick a FIT configuration
 * (fit_conf_find_compat()).  This blocks, for example, an image built
 * for a different board from being flashed on this one.
 *
 * The check is a no-op (returns 0) when:
 *   - the switch is off,
 *   - the running board DT has no 'compatible' (cannot decide),
 *   - the image is not a FIT (nothing to match against; the structural
 *     validator handles non-FIT firmware).
 */
int failsafe_firmware_check_model(const void *data, size_t size,
				  const char *what)
{
	DECLARE_GLOBAL_DATA_PTR;
	const void *fdt = gd_fdt_blob();
	const char *board_compat;
	int off;

	if (!failsafe_strict_model_enabled())
		return 0;

	if (!fdt || fdt_check_header(fdt)) {
		cprintln(CAUTION, "Failsafe: %s strict-model check skipped "
			 "(board DT unavailable)", what);
		return 0;
	}

	board_compat = fdt_getprop(fdt, 0, "compatible", NULL);
	if (!board_compat) {
		cprintln(CAUTION, "Failsafe: %s strict-model check skipped "
			 "(board DT has no compatible)", what);
		return 0;
	}

	if (fit_check_format(data, size)) {
		cprintln(CAUTION, "Failsafe: %s strict-model check skipped "
			 "(not a FIT image)", what);
		return 0;
	}

	off = fit_conf_find_compat(data, fdt);
	if (off < 0) {
		return failsafe_error(-EINVAL, "%s rejected by strict-model "
			"check (firmware does not match board '%s')", what,
			board_compat);
	}

	cprintln(SUCCESS, "Failsafe: %s strict-model check OK (matches "
		 "board '%s')", what, board_compat);
	return 0;
}
#else
int failsafe_firmware_check_model(const void *data, size_t size,
				  const char *what)
{
	(void)data;
	(void)size;
	(void)what;
	return 0;
}
#endif

#if CONFIG_IS_ENABLED(OF_LIBFDT)
/*
 * U-Boot (BL33) validation and strict board-model check.
 *
 * A bootloader image ships its own U-Boot, and that U-Boot carries the
 * control device tree the board boots with.  In strict mode a boot chain
 * is only accepted when the U-Boot inside it declares a 'compatible' that
 * matches the running board - the rule the reference MediaTek tree
 * applies to the "u-boot" entry of a FIP (fip_check_uboot_data()).  This
 * blocks, for example, the boot chain of a different board model from
 * being flashed on this one.
 *
 * The packagings below are all handled here, so the board code only has
 * to know how its U-Boot is stored:
 *   - ARM (Airoha / MediaTek Filogic): the U-Boot is an "nt-fw" entry of
 *     a FIP, extracted by failsafe_fip_check_model(); a bare u-boot.bin
 *     works as well.
 *   - MIPS (MediaTek mtmips / EcoNet): no FIP.  The "u-boot" storage
 *     holds the whole bootloader image, with U-Boot inside it:
 *       mtmips: [TPL][SPL][u-boot-lzma.img], or that image alone -
 *               u-boot-lzma.img being a legacy uImage whose payload is
 *               the LZMA-compressed u-boot.bin;
 *       econet: [TCBoot loader][ECNT descriptor][u-boot.bin payload],
 *               optionally padded behind it.
 *     The device tree is appended to U-Boot, so it is searched for from
 *     the end of the image (or of the uImage payload).
 *
 * The board-model check is a no-op (returns 0) when the switch is off,
 * when the running board DT has no 'compatible' (cannot decide), or when
 * the U-Boot carries no device tree this build can read (compressed and
 * no decoder, out of memory, ...).  failsafe_image_validate_uboot() turns
 * the last case into a diagnostic as well, but accepts the image - an
 * intact uImage is proof enough of a genuine U-Boot.
 */

/* Does a plausible FDT header start at @p, @avail bytes being available? */
static bool failsafe_uboot_fdt_header_ok(const u8 *p, size_t avail)
{
	u32 totalsize;

	if (avail < 8)
		return false;

	if (get_unaligned_be32(p) != FDT_MAGIC)
		return false;

	totalsize = get_unaligned_be32(p + 4);

	return totalsize >= 16 && totalsize <= avail;
}

/*
 * Locate the control device tree inside a U-Boot image.  The device tree
 * is either linked into the binary (OF_EMBED) or appended to it
 * (OF_SEPARATE, the default), so the search walks back from the end and
 * takes the first plausible FDT header that really carries a
 * 'compatible' - mirrors locate_fdt_in_uboot() of the reference tree.
 *
 * This is a heuristic, not an integrity check: an FDT header with a sane
 * total size and a root 'compatible' is taken as the board description.
 * libfdt (with OF_LIBFDT_ASSUME_MASK defaulting to 0) bounds-checks every
 * access, so a false candidate costs nothing but the scan.  The header
 * check runs on raw bytes, so libfdt itself is only entered for the rare
 * offset that looks like an FDT.
 */
static const void *failsafe_uboot_find_fdt(const void *data, size_t size)
{
	const u8 *buf = data;
	size_t off;

	if (size < 8)
		return NULL;

	/* Walk down to offset 0 (a bare device tree image). */
	for (off = size - 8; ; off--) {
		const void *fdt = buf + off;
		int len;

		if (failsafe_uboot_fdt_header_ok(fdt, size - off) &&
		    fdt_getprop(fdt, 0, "compatible", &len))
			return fdt;

		if (!off)
			return NULL;
	}
}

/*
 * Locate a legacy uImage inside a bootloader image and return its
 * payload.  A match requires the magic plus both the header and the data
 * CRC to check out, so it is a real image and not a coincidence in
 * unrelated data; the payload is U-Boot itself (IH_COMP_NONE) or its
 * compressed form.
 */
static const void *failsafe_uboot_find_uimage(const void *data, size_t size,
					      size_t *payload_size)
{
	const u8 *buf = data;
	size_t hdr_size = image_get_header_size();
	const struct legacy_img_hdr *hdr;
	size_t off, dsize;

	if (size < hdr_size)
		return NULL;

	for (off = 0; off + hdr_size <= size; off++) {
		/* Read the magic off the raw bytes: the image may sit at
		 * any offset. */
		if (get_unaligned_be32(buf + off) != IH_MAGIC)
			continue;

		hdr = (const struct legacy_img_hdr *)(buf + off);

		if (!image_check_hcrc(hdr))
			continue;

		dsize = image_get_data_size(hdr);
		if (dsize > size - off - hdr_size)
			continue;

		if (!image_check_dcrc(hdr))
			continue;

		*payload_size = dsize;
		return buf + off + hdr_size;
	}

	return NULL;
}

/*
 * Expand the compressed container @data into a scratch buffer and look
 * for a device tree in the result.  Returns NULL when nothing was found;
 * @why then explains why, so the console can tell "could not expand it"
 * from "expanded it, but it carries no device tree".
 */
static const void *failsafe_uboot_expand_fdt(const void *data, size_t size,
					     void **scratch, const char **why)
{
	const void *fdt;
	size_t scratch_size, out_len = 0;
	void *buf;

	scratch_size = failsafe_payload_expand_size(data, size);

	buf = malloc(scratch_size);
	if (!buf) {
		*why = "out of memory";
		return NULL;
	}

	if (failsafe_payload_decompress(data, size, buf, scratch_size,
					&out_len)) {
		free(buf);
		*why = "could not be expanded";
		return NULL;
	}

	fdt = failsafe_uboot_find_fdt(buf, out_len);
	if (!fdt) {
		free(buf);
		*why = "its payload carries no device tree";
		return NULL;
	}

	*scratch = buf;
	return fdt;
}

/* Console note for a U-Boot whose board model cannot be checked. */
static void failsafe_uboot_report_skip(const char *what, const char *container,
				       const char *why)
{
	if (container && why)
		cprintln(CAUTION, "Failsafe: %s strict-model check skipped "
			 "(compressed U-Boot (%s): %s)", what, container, why);
	else if (container)
		cprintln(CAUTION, "Failsafe: %s strict-model check skipped "
			 "(compressed U-Boot (%s) could not be expanded)",
			 what, container);
	else if (why)
		cprintln(CAUTION, "Failsafe: %s strict-model check skipped "
			 "(%s)", what, why);
	else
		cprintln(CAUTION, "Failsafe: %s strict-model check skipped "
			 "(no control device tree in the U-Boot)", what);
}

/* How a U-Boot was recognised in the bootloader image. */
enum failsafe_uboot_kind {
	/* No U-Boot: neither a legacy uImage nor a device tree. */
	FAILSAFE_UBOOT_NONE,
	/* A U-Boot image was recognised, but its device tree could not be
	 * read (compressed and not expandable here, out of memory). */
	FAILSAFE_UBOOT_IMAGE,
	/* The control device tree was found and is returned in @fdt. */
	FAILSAFE_UBOOT_FDT,
};

/*
 * Look for the U-Boot carried by a bootloader image.  @fdt receives its
 * device tree when one was found, @scratch the buffer the caller has to
 * free in that case; any skip is reported on the console, as both callers
 * want the note.
 */
static enum failsafe_uboot_kind
failsafe_uboot_locate(const void *data, size_t size, const void **fdt,
		      void **scratch, const char *what)
{
	const void *payload, *found;
	size_t payload_size = 0;
	const char *container;
	const char *why = NULL;

	*scratch = NULL;

	/* The image as it is: a bare u-boot.bin, or a composite whose
	 * U-Boot - and with it the appended device tree - sits at the end
	 * (SPL or TCBoot loader in front of it, padding behind it). */
	found = failsafe_uboot_find_fdt(data, size);
	if (found) {
		*fdt = found;
		return FAILSAFE_UBOOT_FDT;
	}

	/* A legacy uImage inside it: how MediaTek MIPS packs the U-Boot
	 * (u-boot-lzma.img, itself inside u-boot-with-spl.bin or
	 * u-boot-mt7621.bin). */
	payload = failsafe_uboot_find_uimage(data, size, &payload_size);
	if (payload) {
		container = failsafe_payload_container(payload, payload_size);

		found = failsafe_uboot_find_fdt(payload, payload_size);
		if (!found && container)
			found = failsafe_uboot_expand_fdt(payload, payload_size,
							  scratch, &why);
		if (found) {
			*fdt = found;
			return FAILSAFE_UBOOT_FDT;
		}

		/* The uImage header and CRCs were verified, so this really
		 * is a U-Boot image; only its device tree stayed out of
		 * reach, which leaves nothing to compare. */
		failsafe_uboot_report_skip(what, container, why);
		return FAILSAFE_UBOOT_IMAGE;
	}

	/* No uImage: the image itself may be a compressed bare U-Boot
	 * (u-boot.bin.lzma, or an .xz one). */
	container = failsafe_payload_container(data, size);
	if (container) {
		found = failsafe_uboot_expand_fdt(data, size, scratch, &why);
		if (found) {
			*fdt = found;
			return FAILSAFE_UBOOT_FDT;
		}

		failsafe_uboot_report_skip(what, container, why);
		return FAILSAFE_UBOOT_IMAGE;
	}

	return FAILSAFE_UBOOT_NONE;
}

/* Read the root 'compatible' list of @fdt into @compats. */
static int failsafe_model_read_compat(const void *fdt, const char *compats[],
				      int max)
{
	const char *prop, *p, *end;
	int len, count = 0;

	if (!fdt || fdt_check_header(fdt))
		return -EINVAL;

	prop = fdt_getprop(fdt, 0, "compatible", &len);
	if (!prop || len < 2)
		return -ENOENT;

	end = prop + len;
	for (p = prop; p < end && *p && count < max; p += strlen(p) + 1)
		compats[count++] = p;

	return count ? count : -ENOENT;
}

/*
 * Are two root 'compatible' lists compatible?
 *
 * The rule is the one the reference tree applies (check_uboot_data() in
 * board/mediatek/common/fip_helper.c of the uboot-mtk-20250711 tree,
 * with struct compat_list in board_info.[ch]): the lists are compared
 * entry by entry and the shorter one must be a prefix of the longer one,
 * i.e. the two device trees describe the same board - "<board>,<soc>"
 * matches "<soc>", while two different boards sharing a SoC do not match.
 *
 * A looser "any common entry" rule would make the gate nearly pointless:
 * every board of one SoC carries the same "<vendor>,<soc>" string, so an
 * image built for a different board of that family would be accepted.
 * Board-specific strings are exactly what this check is for; the
 * environment variable 'failsafe_strict_model=0' is the escape hatch for
 * the cases where it is too strict (e.g. moving a board from a vendor
 * device tree to its own), and 'failsafe_compatible_num' below relaxes
 * it in a controlled way for bootloaders shared between board variants.
 */
static bool failsafe_model_compat_match(const char *a[], int a_count,
					const char *b[], int b_count)
{
	int i, count = a_count < b_count ? a_count : b_count;

	for (i = 0; i < count; i++)
		if (strcmp(a[i], b[i]))
			return false;

	return true;
}

/*
 * Relaxed comparison, enabled by the environment variable
 * 'failsafe_compatible_num' = N (> 0).
 *
 * The running board is the authority here: the first N 'compatible'
 * entries of its own device tree are the board names this device
 * accepts.  A bootloader shared by several board variants then passes as
 * soon as its 'compatible' list declares one of them, wherever that
 * entry sits in its list - the image lists every variant it serves, and
 * the device only has to find itself among them.  The entries beyond the
 * N-th one on the board side (typically the SoC / reference names) are
 * not accepted as a board identity.
 *
 * N = 0 (the default) leaves the strict comparison above in charge; the
 * two are OR-ed, so the setting can only widen what is accepted, never
 * narrow it.
 */
static int failsafe_model_compat_num(void)
{
	const char *val = env_get("failsafe_compatible_num");
	unsigned long num;

	if (!val || !val[0])
		return 0;

	num = simple_strtoul(val, NULL, 10);
	if (num > FAILSAFE_MODEL_COMPAT_MAX)
		num = FAILSAFE_MODEL_COMPAT_MAX;

	return (int)num;
}

static bool failsafe_model_compat_match_any(const char *board[],
					    int board_count,
					    const char *target[],
					    int target_count, int num)
{
	int i, j;

	if (num > board_count)
		num = board_count;

	for (i = 0; i < num; i++)
		for (j = 0; j < target_count; j++)
			if (!strcmp(board[i], target[j]))
				return true;

	return false;
}

int failsafe_board_names(const char *names[], int max)
{
	DECLARE_GLOBAL_DATA_PTR;
	const char *all[FAILSAFE_MODEL_COMPAT_MAX];
	int count, num, i;

	count = failsafe_model_read_compat(gd_fdt_blob(), all,
					   (int)ARRAY_SIZE(all));
	if (count < 0)
		return 0;

	/* The names the model check accepts: the window this device
	 * declared through 'failsafe_compatible_num', or the primary
	 * (board) name alone when no window is configured. */
	num = failsafe_model_compat_num();
	if (num <= 0)
		num = 1;

	if (num > count)
		num = count;
	if (num > max)
		num = max;

	for (i = 0; i < num; i++)
		names[i] = all[i];

	return num;
}

/*
 * Compare the 'compatible' of the U-Boot device tree @fdt with the board
 * this recovery runs on.  Returns 0 when they match (or when the board DT
 * cannot decide), -EINVAL when the U-Boot is built for another board.
 */
static int failsafe_uboot_match_board(const void *fdt, const char *what)
{
	DECLARE_GLOBAL_DATA_PTR;
	const char *board[FAILSAFE_MODEL_COMPAT_MAX];
	const char *target[FAILSAFE_MODEL_COMPAT_MAX];
	int board_count, target_count, num;

	board_count = failsafe_model_read_compat(gd_fdt_blob(), board,
						 (int)ARRAY_SIZE(board));
	if (board_count < 0) {
		cprintln(CAUTION, "Failsafe: %s strict-model check skipped "
			 "(board DT has no compatible)", what);
		return 0;
	}

	target_count = failsafe_model_read_compat(fdt, target,
						  (int)ARRAY_SIZE(target));
	if (target_count < 0) {
		cprintln(CAUTION, "Failsafe: %s strict-model check skipped "
			 "(U-Boot DT has no compatible)", what);
		return 0;
	}

	if (failsafe_model_compat_match(board, board_count, target,
					target_count)) {
		cprintln(SUCCESS, "Failsafe: %s strict-model check OK "
			 "(matches board '%s')", what, board[0]);
		return 0;
	}

	/* Shared bootloader: the leading entries of this board's own list
	 * are the names it accepts (env 'failsafe_compatible_num'). */
	num = failsafe_model_compat_num();
	if (num && failsafe_model_compat_match_any(board, board_count, target,
						   target_count, num)) {
		cprintln(SUCCESS, "Failsafe: %s strict-model check OK "
			 "(matches one of this board's first %d compatible "
			 "entries)", what, num);
		return 0;
	}

	return failsafe_error(-EINVAL, "%s rejected by strict-model "
		"check (built for '%s', board is '%s')", what,
		target[0], board[0]);
}

int failsafe_uboot_check_model(const void *data, size_t size, const char *what)
{
	const void *fdt;
	void *scratch;
	int ret;

	if (!failsafe_strict_model_enabled())
		return 0;

	/* Any skip is reported by the locator itself. */
	switch (failsafe_uboot_locate(data, size, &fdt, &scratch, what)) {
	case FAILSAFE_UBOOT_FDT:
		ret = failsafe_uboot_match_board(fdt, what);
		free(scratch);
		return ret;
	case FAILSAFE_UBOOT_IMAGE:
		return 0;
	default:
		cprintln(CAUTION, "Failsafe: %s strict-model check skipped "
			 "(no control device tree in the U-Boot)", what);
		return 0;
	}
}

int failsafe_image_validate_uboot(const void *data, size_t size,
				  const char *what)
{
	const void *fdt;
	void *scratch;
	int ret;

	switch (failsafe_uboot_locate(data, size, &fdt, &scratch, what)) {
	case FAILSAFE_UBOOT_FDT:
		break;
	case FAILSAFE_UBOOT_IMAGE:
		/* A genuine U-Boot image (its header and CRCs are intact):
		 * only its device tree was out of reach, which the locator
		 * reported - there is no board to compare against. */
		return 0;
	default:
		return failsafe_error(-EINVAL, "'%s' image carries no U-Boot "
			"(no legacy uImage and no control device tree)", what);
	}

	if (!failsafe_strict_model_enabled()) {
		free(scratch);
		return 0;
	}

	ret = failsafe_uboot_match_board(fdt, what);
	free(scratch);

	return ret;
}
#else
int failsafe_uboot_check_model(const void *data, size_t size,
			       const char *what)
{
	(void)data;
	(void)size;
	(void)what;
	return 0;
}

int failsafe_image_validate_uboot(const void *data, size_t size,
				  const char *what)
{
	(void)data;
	(void)size;
	(void)what;
	return 0;
}

int failsafe_board_names(const char *names[], int max)
{
	(void)names;
	(void)max;
	return 0;
}
#endif /* CONFIG_IS_ENABLED(OF_LIBFDT) */

int failsafe_boot_image_from_mem(ulong data_load_addr, size_t image_size,
				 ulong load_fallback)
{
	const char *loadaddr = env_get("loadaddr");
	const char *bootconf = env_get("bootconf");
	ulong load_addr;
	char cmd[96];
	int ret;

	if (loadaddr && loadaddr[0])
		load_addr = simple_strtoul(loadaddr, NULL, 0);
	else
		load_addr = load_fallback;

	if (load_addr != data_load_addr)
		memcpy((void *)load_addr, (const void *)data_load_addr,
		       image_size);

	switch (genimg_get_format((const void *)load_addr)) {
	case IMAGE_FORMAT_FIT:
	case IMAGE_FORMAT_LEGACY:
		/* Parseable boot image: use bootm (the initramfs path). */
		if (bootconf && bootconf[0])
			snprintf(cmd, sizeof(cmd), "bootm 0x%lx#%s",
				 load_addr, bootconf);
		else
			snprintf(cmd, sizeof(cmd), "bootm 0x%lx", load_addr);

		return run_command(cmd, 0);

	default:
		/* Raw binary: jump straight to it with "go".  The image was
		 * copied with memcpy() while caches were active, so make sure
		 * it actually reached RAM and is not served from a stale
		 * i-cache line before executing.
		 */
		flush_cache(load_addr, image_size);
		invalidate_icache_all();
		cprintln(NORMAL, "\n*** Failsafe: raw image - 'go 0x%lx' ***\n",
			 load_addr);
		snprintf(cmd, sizeof(cmd), "go 0x%lx", load_addr);
		ret = run_command(cmd, 0);
		if (ret)
			return failsafe_error(ret, "'go 0x%lx' failed",
				load_addr);
		return 0;
	}
}
