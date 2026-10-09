/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * save_boot_params() runs as the very first thing of the AArch64 startup
 * code, with the registers the previous boot stage left behind still intact
 * (x0 - x3).  The MediaTek firmware hands the image description arguments of
 * BL33 over in x2 - see mtk_boot_source.h in the TF-A port - which is the
 * only place they can still be read, so they are stored here for the rest of
 * the boot (the U-Boot failsafe reads them through mtk_get_boot_source()).
 *
 * The storage lives in .data on purpose: it must keep its content across the
 * relocation (a BSS variable would be cleared), and everything before that
 * point is why nothing else may touch it.
 */

#include <linux/linkage.h>

ENTRY(save_boot_params)
	/* PC-relative, so a position independent build works as well. */
	adrp	x9, mtk_bl33_arg2
	add	x9, x9, :lo12:mtk_bl33_arg2
	str	x2, [x9]
	b	save_boot_params_ret
ENDPROC(save_boot_params)

	.section .data
	.align	3
	.globl	mtk_bl33_arg2
	/* Hidden: addressed directly (PC-relative) from here as well as from
	 * cpu.c, so it must not be treated as preemptible. */
	.hidden	mtk_bl33_arg2
mtk_bl33_arg2:
	.quad	0
