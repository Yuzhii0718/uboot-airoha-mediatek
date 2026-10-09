// SPDX-License-Identifier: GPL-2.0

#include <cpu_func.h>
#include <dm.h>
#include <errno.h>
#include <init.h>
#include <linux/err.h>
#include <linux/regmap.h>
#include <soc/airoha/bootsrc.h>
#include <soc/airoha/scu-regmap.h>
#include <wdt.h>
#include <dm/uclass-internal.h>

int arch_cpu_init(void)
{
	icache_enable();

	return 0;
}

void enable_caches(void)
{
	/* Enable D-cache. I-cache is already enabled in start.S */
	dcache_enable();
}

int airoha_get_boot_source(enum airoha_boot_source *src)
{
	struct regmap *np_scu;
	u32 value;

	np_scu = airoha_get_scu_regmap();
	if (IS_ERR_OR_NULL(np_scu))
		return -ENODEV;

	if (regmap_read(np_scu, AIROHA_NP_SCU_SCREG_BOOTSRC, &value))
		return -EIO;

	/*
	 * The magic is what tells a value published by BL2 from whatever a
	 * register without the handoff (or a reset value) happens to hold.
	 */
	if ((value >> 16) != AIROHA_BOOTSRC_MAGIC)
		return -ENOENT;

	switch (value & 0xffff) {
	case AIROHA_BOOTSRC_FLASH:
		*src = AIROHA_BOOT_SOURCE_FLASH;
		return 0;
	case AIROHA_BOOTSRC_XMODEM:
		*src = AIROHA_BOOT_SOURCE_XMODEM;
		return 0;
	default:
		return -ENOENT;
	}
}
