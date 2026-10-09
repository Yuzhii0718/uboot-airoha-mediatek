// SPDX-License-Identifier: GPL-2.0+

#include <init.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <mach/en751221.h>
#include <soc/airoha/bootsrc.h>
#include <soc/airoha/pkgids-econet.h>

DECLARE_GLOBAL_DATA_PTR;

#define NP_SCU_BASE		((void __iomem *)CKSEG1ADDR(0x1fb00000))

#define EN7512_BOOTROM_RECOVERY_LATCH	BIT(0)

/* Sampled from the latch before it is cleared, see
 * en751221_clear_bootrom_recovery_latch(). */
static bool bootrom_recovery;

/*
 * econet_bootrom_recovery() - did the BootROM enter its XMODEM recovery path?
 *
 * The BootROM sets CHIP_SCU[0] when it enters the XMODEM recovery path, which
 * is how the chainloader gets us here, i.e. the image running now was
 * fetched over the console and never came from the flash.  The latch is
 * cleared very early (see mach_cpu_init()), because leaving it set sends the
 * SoC straight back into recovery on the next warm reset, so the value is
 * sampled first and kept for the rest of the session.  The Web failsafe uses
 * it to warn the user that the device booted from RAM.
 */
bool econet_bootrom_recovery(void)
{
	return bootrom_recovery;
}

/*
 * The BootROM sets CHIP_SCU[0] when it enters the XMODEM recovery path. Clear
 * it so a warm reset does not go straight back into recovery, remembering
 * whether it was set.
 */
static void en751221_clear_bootrom_recovery_latch(void)
{
	void __iomem *reg = (void __iomem *)EN7512_CHIP_SCU_BASE;
	u32 val;

	val = __raw_readl(reg);

	bootrom_recovery = !!(val & EN7512_BOOTROM_RECOVERY_LATCH);
	if (!bootrom_recovery)
		return;

	__raw_writel(val & ~EN7512_BOOTROM_RECOVERY_LATCH, reg);

	/* Flush the MMIO write before a following reset. */
	(void)__raw_readl(reg);
}

#define EN751221_INTC_BASE	((void __iomem *)CKSEG1ADDR(0x1fb40000))
#define EN751221_INTC_IPSR(n)	(0x10 + 4 * (n))

/*
 * Interrupt priority table of the interrupt controller, as the BootROM leaves
 * it on the XMODEM recovery path (the OEM bootloader programmed the same
 * thing on the flash path). One byte per priority slot, each naming an
 * interrupt source. Linux's en751221 intc driver never programs these
 * registers; with the table left at zero no source reaches the CPU and the
 * kernel stops in calibrate_delay(), waiting for its first timer tick.
 */
static const u32 en751221_intc_ipsr[] = {
	0x1f1e1d13, 0x16150111, 0x0008090a, 0x0b0c0d0f,
	0x10060e07, 0x12030217, 0x18191a1b, 0x1c050414,
};

static void en751221_intc_init_priorities(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(en751221_intc_ipsr); i++)
		if (__raw_readl(EN751221_INTC_BASE + EN751221_INTC_IPSR(i)))
			return;

	for (i = 0; i < ARRAY_SIZE(en751221_intc_ipsr); i++)
		__raw_writel(en751221_intc_ipsr[i],
			     EN751221_INTC_BASE + EN751221_INTC_IPSR(i));
}

int mach_cpu_init(void)
{
	en751221_clear_bootrom_recovery_latch();
	en751221_intc_init_priorities();

	return 0;
}

int dram_init(void)
{
	u32 val = __raw_readl((void __iomem *)EN7512_REG_SAVE_INFO);
	u32 size_mb = val & EN7512_SAVE_DRAM_MASK;

	if (!size_mb)
		size_mb = 128;

	gd->ram_size = (phys_size_t)size_mb << 20;
	return 0;
}

ulong notrace get_tbclk(void)
{
	u32 val = __raw_readl((void __iomem *)EN7512_REG_SAVE_INFO);
	u32 clk = (val & EN7512_SAVE_CLK_MASK) >> EN7512_SAVE_CLK_SHIFT;

	/*
	 * TCBoot stores the CPU clock in units of 4 MHz (0xe1 = 225 for the
	 * 900 MHz EN7526G) and CP0 Count advances at half the CPU clock on the
	 * MIPS 34K, so the timebase is clk * 2 MHz.  With the old * 500000 a
	 * "sleep 5" lasted 1.4 s and every udelay/mdelay was four times too
	 * short.  The BootROM leaves the field at zero on the XMODEM recovery
	 * path, hence the fallback.
	 */
	if (clk)
		return (ulong)clk * 2000000;

	return CONFIG_SYS_MIPS_TIMER_FREQ;
}

void _machine_restart(void)
{
	en751221_clear_bootrom_recovery_latch();

	__raw_writel(0x80000000, (void __iomem *)EN7512_RESET_CONTROL);

	for (;;)
		;
}
