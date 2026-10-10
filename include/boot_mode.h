/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Boot mode of the running session.
 *
 * A bootloader can run either from the flash (its own image was stored there
 * by the boot chain) or from RAM, when the image was fetched over the console
 * by the previous boot stage (the TF-A BL2 XMODEM recovery on Airoha and
 * MediaTek, the BootROM XMODEM path on EcoNet).  The second case is a volatile
 * recovery session: nothing the user uploads reaches the flash until it is
 * written explicitly, so forgetting to write a bootloader - or installing a
 * firmware that does not match the bootloader already in the flash - bricks
 * the device on the next reset.
 *
 * The mode is reported by the boot chain where it can be reported at all (see
 * <soc/airoha/bootsrc.h> and <soc/mediatek/bootsrc.h>); everywhere else it
 * stays "unknown" instead of guessing.  U-Boot reports it on the console on
 * every boot, and the Web failsafe warns about a volatile session - see
 * common/boot_mode.c.
 */

#ifndef _BOOT_MODE_H_
#define _BOOT_MODE_H_

#ifdef __cplusplus
extern "C" {
#endif

enum boot_mode {
	/* No boot chain report available (unsupported platform, or a boot
	 * chain that does not implement the handoff yet). */
	BOOT_MODE_UNKNOWN = 0,
	/* The running image was read from the flash. */
	BOOT_MODE_FLASH,
	/* The running image came over the console and lives in DRAM only. */
	BOOT_MODE_RAM,
};

/**
 * boot_mode_get() - boot mode of the running session
 *
 * The environment variable 'boot_mode' (auto|ram|flash, default "auto")
 * overrides the report of the boot chain; it is meant for testing and for
 * boards whose boot chain cannot report the mode itself.
 *
 * Returns the mode, BOOT_MODE_UNKNOWN when it cannot be determined.
 */
enum boot_mode boot_mode_get(void);

/**
 * boot_mode_name() - stable name of a boot mode
 *
 * Returns "ram", "flash" or "unknown"; these are the names the Web failsafe
 * reports in the JSON answer of GET /sysinfo, so they are part of its
 * interface.
 */
const char *boot_mode_name(enum boot_mode mode);

/**
 * boot_mode_print() - report the boot mode on the console
 *
 * One line.  U-Boot prints it on every boot (see common/boot_mode.c), which
 * makes the report of the boot chain readable without opening the Web UI, and
 * lets a new platform's handoff be brought up before any Web interface exists.
 */
void boot_mode_print(void);

#ifdef __cplusplus
}
#endif

#endif /* _BOOT_MODE_H_ */
