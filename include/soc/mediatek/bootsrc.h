/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * MediaTek boot source reporting.
 *
 * The BL2 of the MediaTek TF-A port publishes where the FIP it finally
 * booted came from, so that BL33 (U-Boot) can tell a normal flash boot from
 * a RAM recovery boot - the case in which the FIP was pushed into DRAM over
 * the console (bl2_boot_ram.c, or an XMODEM recovery that replaced the
 * stored FIP) and never reached the flash.
 *
 * That session is volatile: nothing uploaded through the network is stored
 * unless it is written explicitly, so forgetting to flash a bootloader - or
 * installing a firmware that does not match the bootloader already in the
 * flash - bricks the device on the next reset.  The Web failsafe warns
 * about it, see <boot_mode.h>.
 *
 * The value travels through the standard image description arguments:
 * BL2 puts it in BL33's entry point arguments, BL31 hands that description
 * over untouched and the AArch64 startup code stores the register in
 * save_boot_params() (see arch/arm/mach-mediatek/boot_params.S).  A boot
 * chain that does not publish anything (an older BL2) leaves it zero, which
 * is reported as "unknown" and keeps U-Boot quiet.
 */

#ifndef __MEDIATEK_BOOTSRC_H_
#define __MEDIATEK_BOOTSRC_H_

#include <linux/types.h>

/* Value layout: (magic << 16) | code */
#define MTK_BOOTSRC_MAGIC		0x424c		/* "BL" */
#define MTK_BOOTSRC_FLASH		0x0001
#define MTK_BOOTSRC_RAM			0x0002
#define MTK_BOOTSRC_VALUE(code)		((MTK_BOOTSRC_MAGIC << 16) | \
					 ((code) & 0xffff))

enum mtk_boot_source {
	/* The FIP was read from the configured boot device (flash). */
	MTK_BOOT_SOURCE_FLASH = 0,
	/* The FIP came from DRAM (RAM boot / XMODEM recovery). */
	MTK_BOOT_SOURCE_RAM,
};

#if defined(CONFIG_ARCH_MEDIATEK) && defined(CONFIG_ARM64)

/**
 * mtk_get_boot_source() - boot source reported by the previous stage
 * @src: receives the boot source (only valid on success)
 *
 * Reads the argument the firmware left in the register captured by
 * save_boot_params() and checks its magic.
 *
 * Returns 0 on success, -ENOENT when the argument carries no boot source
 * (boot chain without the handoff, e.g. an older BL2).
 */
int mtk_get_boot_source(enum mtk_boot_source *src);

#endif /* CONFIG_ARCH_MEDIATEK && CONFIG_ARM64 */

#endif /* __MEDIATEK_BOOTSRC_H_ */
