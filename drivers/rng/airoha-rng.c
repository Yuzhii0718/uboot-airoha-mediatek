// SPDX-License-Identifier: GPL-2.0
/*
 * Airoha True Random Number Generator driver
 *
 * Ported from the Linux driver drivers/char/hw_random/airoha-trng.c
 *
 * Copyright (C) 2024 Christian Marangi <ansuelsmth@gmail.com>
 *
 * The Airoha/EcoNet uses two output path:
 *
 *   RAW entropy out(0x828)  : EN7523 / AN7563 / AN7581, 
 *   DRBG out(0x83c) : AN7583,  
 *
 * The data path is selected through the DT compatible.
 */

#include <dm.h>
#include <rng.h>
#include <dm/device_compat.h>
#include <asm/io.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/iopoll.h>
#include <linux/string.h>

#define TRNG_IP_RDY			0x800
#define   CNT_TRANS			GENMASK(15, 8)
#define   SAMPLE_RDY			BIT(0)
#define TRNG_NS_SEK_AND_DAT_EN		0x804
#define   RNG_EN			BIT(31)
#define   RAW_DATA_EN			BIT(16)
#define TRNG_HEALTH_TEST_SW_RST		0x808
#define   SW_RST			BIT(0) /* Active High */
#define TRNG_INTR_EN			0x818
#define   INTR_MASK			BIT(16)
#define   RST_STARTUP_INITR_EN		BIT(0)
#define TRNG_HEALTH_TEST_STATUS		0x824
#define   RST_STARTUP_TEST_DONE		BIT(18)
#define   RST_STARTUP_AP_TEST_FAIL	BIT(17)
#define   RST_STARTUP_RC_TEST_FAIL	BIT(16)
#define   RAW_DATA_VALID		BIT(7)
#define TRNG_RAW_DATA_OUT		0x828
#define TRNG_TEST_MODE_SHA_DONE		0x838
#define   DRBG_DATA_RDY			BIT(31)
#define TRNG_DRBG_DATA_OUT		0x83c

#define TRNG_CNT_TRANS_VALID		0x80
#define TRNG_TIMEOUT_US			100000

/*
 * TRNG output path, selected by the DT compatible:
 *
 * - RAW : enable the ring oscillator and take the raw entropy from
 *         TRNG_RAW_DATA_OUT, readiness is reported by RAW_DATA_VALID.
 *         Used by EN7523/AN7563/AN7581.
 * - DRBG: enable the ring oscillator only and take the SHA/DRBG output
 *         from TRNG_DRBG_DATA_OUT, readiness is reported by the DRBG
 *         valid bit in TEST_MODE_SHA_DONE.
 *         Used by AN7583.
 */
enum airoha_trng_path {
	TRNG_PATH_RAW = 0,
	TRNG_PATH_DRBG,
};

struct airoha_trng {
	void __iomem *base;
	enum airoha_trng_path path;
};

static int airoha_trng_read_raw(struct udevice *dev, void *data, size_t len)
{
	struct airoha_trng *trng = dev_get_priv(dev);
	u8 *buf = data;

	while (len) {
		size_t step = min(len, sizeof(u32));
		u32 status, val;
		int ret;

		ret = readl_poll_timeout(trng->base + TRNG_HEALTH_TEST_STATUS,
					 status, status & RAW_DATA_VALID,
					 TRNG_TIMEOUT_US);
		if (ret) {
			dev_err(dev, "Timeout waiting for TRNG RAW Data valid\n");
			return ret;
		}

		val = readl(trng->base + TRNG_RAW_DATA_OUT);
		memcpy(buf, &val, step);

		buf += step;
		len -= step;
	}

	return 0;
}

static int airoha_trng_read_drbg(struct udevice *dev, void *data, size_t len)
{
	struct airoha_trng *trng = dev_get_priv(dev);
	u8 *buf = data;

	while (len) {
		size_t step = min(len, sizeof(u32));
		u32 status, val;
		int ret;

		ret = readl_poll_timeout(trng->base + TRNG_TEST_MODE_SHA_DONE,
					 status, status & DRBG_DATA_RDY,
					 TRNG_TIMEOUT_US);
		if (ret) {
			dev_err(dev, "Timeout waiting for TRNG DRBG Data valid\n");
			return ret;
		}

		val = readl(trng->base + TRNG_DRBG_DATA_OUT);
		memcpy(buf, &val, step);

		buf += step;
		len -= step;
	}

	return 0;
}

static int airoha_trng_read(struct udevice *dev, void *data, size_t len)
{
	struct airoha_trng *trng = dev_get_priv(dev);

	if (trng->path == TRNG_PATH_DRBG)
		return airoha_trng_read_drbg(dev, data, len);

	return airoha_trng_read_raw(dev, data, len);
}

static int airoha_trng_probe_raw(struct udevice *dev)
{
	struct airoha_trng *trng = dev_get_priv(dev);
	u32 val;
	int ret;

	/* 0x80010002: select the noise source, enable raw data and oscillator */
	val = readl(trng->base + TRNG_NS_SEK_AND_DAT_EN);
	val |= RAW_DATA_EN | RNG_EN;
	writel(val, trng->base + TRNG_NS_SEK_AND_DAT_EN);

	/* Health Test runs only on the transition out of SW Reset */
	writel(SW_RST, trng->base + TRNG_HEALTH_TEST_SW_RST);
	writel(0, trng->base + TRNG_HEALTH_TEST_SW_RST);

	ret = readl_poll_timeout(trng->base + TRNG_HEALTH_TEST_STATUS, val,
				 val & RST_STARTUP_TEST_DONE, TRNG_TIMEOUT_US);
	if (ret) {
		dev_err(dev, "Timeout waiting for Health Check\n");
		return ret;
	}

	if (val & (RST_STARTUP_AP_TEST_FAIL | RST_STARTUP_RC_TEST_FAIL)) {
		dev_err(dev, "Health Check fail: %s test fail\n",
			val & RST_STARTUP_AP_TEST_FAIL ? "AP" : "RC");
		return -EIO;
	}

	ret = readl_poll_timeout(trng->base + TRNG_IP_RDY, val,
				 val & SAMPLE_RDY &&
				 FIELD_GET(CNT_TRANS, val) == TRNG_CNT_TRANS_VALID,
				 TRNG_TIMEOUT_US);
	if (ret) {
		dev_err(dev, "Timeout waiting for IP ready\n");
		return ret;
	}

	return 0;
}

static int airoha_trng_probe_drbg(struct udevice *dev)
{
	struct airoha_trng *trng = dev_get_priv(dev);
	u32 val;
	int ret;

	/*
	 * 0x80000002: select the noise source and enable the oscillator;
	 * the DRBG output is used instead of the raw one.
	 */
	val = readl(trng->base + TRNG_NS_SEK_AND_DAT_EN);
	val |= RNG_EN;
	writel(val, trng->base + TRNG_NS_SEK_AND_DAT_EN);

	ret = readl_poll_timeout(trng->base + TRNG_TEST_MODE_SHA_DONE, val,
				 val & DRBG_DATA_RDY, TRNG_TIMEOUT_US);
	if (ret) {
		dev_err(dev, "Timeout waiting for TRNG DRBG ready\n");
		return ret;
	}

	return 0;
}

static int airoha_trng_probe(struct udevice *dev)
{
	struct airoha_trng *trng = dev_get_priv(dev);
	u32 val;

	trng->base = dev_read_addr_ptr(dev);
	if (!trng->base)
		return -EINVAL;

	trng->path = dev_get_driver_data(dev);

	/* No interrupts in U-Boot: keep the TRNG one masked and poll instead */
	val = readl(trng->base + TRNG_INTR_EN);
	val |= INTR_MASK;
	if (trng->path == TRNG_PATH_RAW)
		val |= RST_STARTUP_INITR_EN;
	writel(val, trng->base + TRNG_INTR_EN);

	if (trng->path == TRNG_PATH_DRBG)
		return airoha_trng_probe_drbg(dev);

	return airoha_trng_probe_raw(dev);
}

static const struct dm_rng_ops airoha_trng_ops = {
	.read = airoha_trng_read,
};

static const struct udevice_id airoha_trng_match[] = {
	{ .compatible = "airoha,en7581-trng", .data = TRNG_PATH_RAW },
	{ .compatible = "airoha,an7583-trng", .data = TRNG_PATH_DRBG },
	{ }
};

U_BOOT_DRIVER(airoha_trng) = {
	.name = "airoha-trng",
	.id = UCLASS_RNG,
	.of_match = airoha_trng_match,
	.ops = &airoha_trng_ops,
	.probe = airoha_trng_probe,
	.priv_auto = sizeof(struct airoha_trng),
};
