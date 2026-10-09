/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Airoha / EcoNet boot source reporting.
 *
 * On the ARM Airoha platforms (EN7523 / AN7581 / AN7583 / AN7552) the BL2 of
 * the TF-A port (../atf-airoha, plat/ecnt/en7523) publishes which storage the
 * FIP it finally booted came from in a NP-SCU scratch register, right before
 * it hands over to BL31 / BL33.
 *
 * This lets U-Boot tell a normal flash boot from a RAM recovery boot, i.e.
 * the case in which the FIP was received over the XMODEM console and never
 * reached the flash.  That session is volatile: whatever the user uploads
 * through the network stays in DRAM, so forgetting to flash a bootloader -
 * or flashing a firmware that does not match the bootloader already in the
 * flash - bricks the device on the next reset.  The Web failsafe uses this
 * to warn the user (see <boot_mode.h>).
 *
 * The register is written on every BL2 boot, including the flash path,
 * because the NP-SCU is in the always-on domain and survives a warm reset;
 * a value left behind by a RAM session would otherwise still be reported
 * after a later flash boot.  A register that does not carry the magic is
 * reported as "unknown", which covers a boot chain that does not implement
 * the handoff at all (an older BL2) and keeps U-Boot quiet in that case.
 */

#ifndef __AIROHA_BOOTSRC_H_
#define __AIROHA_BOOTSRC_H_

#include <linux/types.h>

/*
 * NP-SCU scratch register written by BL2, offset inside the NP-SCU block
 * (absolute address 0x1fb00244 on every Airoha platform).
 *
 * The scratch area of the NP-SCU is two banks of two registers; the offsets
 * between and after them are not implemented and read back as 0xdeadbeef,
 * the value this bus returns for an address it cannot access:
 *
 *	WF0 (0x240)	BL1 debug / boot mode + BL2 error code
 *	WF1 (0x244)	BL1 debug magic + BL1 error code
 *	WR0 (0x280)	L2C SRAM size, written by BL31 for the kernel
 *	WR1 (0x284)	SYS_GLOBAL_PARM (DRAM size, package id, ...)
 *
 * The handoff uses WF1: it is the only one of the four that nothing after
 * BL2 consumes (BL1 reads it before BL2 runs, and BL2 leaves it alone when
 * it holds BL1's debug magic).  WR0/WR1 are read by U-Boot and by the
 * kernel and must keep their content, WF0 is written by BL2's own
 * plat_error_handler().  EN7523_SCREG_BOOTSRC in the TF-A port is the same
 * register.
 */
#define AIROHA_NP_SCU_SCREG_BOOTSRC	0x244

/* Value layout: (magic << 16) | code */
#define AIROHA_BOOTSRC_MAGIC		0x424c		/* "BL" */
#define AIROHA_BOOTSRC_FLASH		0x0001
#define AIROHA_BOOTSRC_XMODEM		0x0002
#define AIROHA_BOOTSRC_VALUE(code)	((AIROHA_BOOTSRC_MAGIC << 16) | \
					 ((code) & 0xffff))

enum airoha_boot_source {
	/* The FIP was read from the configured boot device (flash). */
	AIROHA_BOOT_SOURCE_FLASH = 0,
	/* The FIP was received over the console (XMODEM recovery). */
	AIROHA_BOOT_SOURCE_XMODEM,
};

#if defined(CONFIG_ARCH_AIROHA) && \
	(defined(CONFIG_ARM) || defined(CONFIG_ARM64))

/**
 * airoha_get_boot_source() - boot source reported by the BL2 handoff
 * @src: receives the boot source (only valid on success)
 *
 * Reads the NP-SCU boot source register through the NP-SCU syscon and
 * checks its magic.  Only callable after the driver model is up (the syscon
 * lookup goes through the device tree).
 *
 * Returns 0 on success, -ENODEV when the NP-SCU syscon is not available,
 * -ENOENT when the register does not carry the BL2 magic (boot chain without
 * the handoff, e.g. an older BL2).
 */
int airoha_get_boot_source(enum airoha_boot_source *src);

#endif /* CONFIG_ARCH_AIROHA && (CONFIG_ARM || CONFIG_ARM64) */

#ifdef CONFIG_ARCH_ECONET

/**
 * econet_bootrom_recovery() - did the BootROM enter its XMODEM recovery path?
 *
 * The MIPS EcoNet platforms have no TF-A: their BootROM latches the XMODEM
 * recovery state itself in the CHIP-SCU.  Sampled and cleared by
 * arch/mips/mach-econet/en751221/init.c.
 *
 * Implemented (strong) only for the SoCs whose BootROM is known to have the
 * latch; everywhere else a weak default reports false, which makes the boot
 * source "unknown" instead of guessing.
 *
 * Returns true when the running image came over the console, i.e. the device
 * booted from RAM.
 */
bool econet_bootrom_recovery(void);

#endif /* CONFIG_ARCH_ECONET */

#endif /* __AIROHA_BOOTSRC_H_ */
