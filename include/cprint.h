/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 MediaTek Inc. All Rights Reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 *
 * Helper to print colored texts.
 *
 * This started out as a failsafe-only helper (include/failsafe/cprint.h); it
 * is an ordinary U-Boot facility now, so messages outside the Web failsafe -
 * the boot mode report of common/boot_mode.c - are printed in the same way.
 */

#ifndef _COLORED_PRINT_H_
#define _COLORED_PRINT_H_

#include <ansi.h>
#include <stdio.h>

#define COLOR_PROMPT	ANSI_COLOR_YELLOW
#define COLOR_INPUT	"\x1b[4;36m"
#define COLOR_ERROR	"\x1b[93;41m"
#define COLOR_CAUTION	ANSI_COLOR_RED
#define COLOR_SUCCESS	ANSI_COLOR_GREEN
#define COLOR_NORMAL	ANSI_COLOR_RESET

/*
 * Colored printing for U-Boot messages.
 *
 *   cprintln(color, fmt, ...)  - one line, reset to normal at the end
 *   cprint(color, fmt, ...)    - same, but no trailing newline
 *   cprint_cont(color, fmt, ...) - continue a line in @color (no reset)
 *
 * The color argument is one of PROMPT / INPUT / ERROR / CAUTION / SUCCESS /
 * NORMAL and is pasted onto the COLOR_ prefix.  When CONFIG_CONSOLE_COLOR is
 * disabled the escape codes are dropped and the macros behave like plain
 * printf(), so callers never need a #ifdef of their own - that option is where
 * a console which is not a VT100 terminal is accounted for, see
 * common/Kconfig.
 */
#ifdef CONFIG_CONSOLE_COLOR
#define cprintln(color, fmt, ...) \
	printf(COLOR_##color fmt COLOR_NORMAL "\n", ##__VA_ARGS__)

#define cprint(color, fmt, ...) \
	printf(COLOR_##color fmt COLOR_NORMAL, ##__VA_ARGS__)

#define cprint_cont(color, fmt, ...) \
	printf(COLOR_##color fmt, ##__VA_ARGS__)
#else
#define cprintln(color, fmt, ...) \
	printf(fmt "\n", ##__VA_ARGS__)

#define cprint(color, fmt, ...) \
	printf(fmt, ##__VA_ARGS__)

#define cprint_cont(color, fmt, ...) \
	printf(fmt, ##__VA_ARGS__)
#endif

#endif /* _COLORED_PRINT_H_ */
