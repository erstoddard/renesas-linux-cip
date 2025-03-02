/*
 * DMA support for Internal DMAC with SDHI SD/SDIO controller
 *
 * Copyright (C) 2016-17 Renesas Electronics Corporation
 * Copyright (C) 2016-17 Horms Solutions, Simon Horman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/io-64-nonatomic-hi-lo.h>
#include <linux/mfd/tmio.h>
#include <linux/mmc/host.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/scatterlist.h>
#include <linux/sys_soc.h>

#include "renesas_sdhi.h"
#include "tmio_mmc.h"

#define DM_CM_DTRAN_MODE	0x820
#define DM_CM_DTRAN_CTRL	0x828
#define DM_CM_RST		0x830
#define DM_CM_INFO1		0x840
#define DM_CM_INFO1_MASK	0x848
#define DM_CM_INFO2		0x850
#define DM_CM_INFO2_MASK	0x858
#define DM_DTRAN_ADDR		0x880

/* DM_CM_DTRAN_MODE */
#define DTRAN_MODE_CH_NUM_CH0	0	/* "downstream" = for write commands */
#define DTRAN_MODE_CH_NUM_CH1	BIT(16)	/* "uptream" = for read commands */
#define DTRAN_MODE_BUS_WID_TH	(BIT(5) | BIT(4))
#define DTRAN_MODE_ADDR_MODE	BIT(0)	/* 1 = Increment address */

/* DM_CM_DTRAN_CTRL */
#define DTRAN_CTRL_DM_START	BIT(0)

/* DM_CM_RST */
#define RST_DTRANRST1		BIT(9)
#define RST_DTRANRST0		BIT(8)
#define RST_RESERVED_BITS	GENMASK_ULL(31, 0)

/* DM_CM_INFO1 and DM_CM_INFO1_MASK */
#define INFO1_CLEAR		0
#define INFO1_MASK_CLEAR	GENMASK_ULL(31, 0)
#define INFO1_DTRANEND1		BIT(17)
#define INFO1_DTRANEND0		BIT(16)

/* DM_CM_INFO2 and DM_CM_INFO2_MASK */
#define INFO2_MASK_CLEAR	GENMASK_ULL(31, 0)
#define INFO2_DTRANERR1		BIT(17)
#define INFO2_DTRANERR0		BIT(16)

/*
 * Specification of this driver:
 * - host->chan_{rx,tx} will be used as a flag of enabling/disabling the dma
 * - Since this SDHI DMAC register set has 16 but 32-bit width, we
 *   need a custom accessor.
 */

static unsigned long global_flags;
/*
 * Workaround for avoiding to use RX DMAC by multiple channels.
 * On R-Car H3 ES1.* and M3-W ES1.0, when multiple SDHI channels use
 * RX DMAC simultaneously, sometimes hundreds of bytes data are not
 * stored into the system memory even if the DMAC interrupt happened.
 * So, this driver then uses one RX DMAC channel only.
 */
#define SDHI_INTERNAL_DMAC_ONE_RX_ONLY	0
#define SDHI_INTERNAL_DMAC_RX_IN_USE	1

/* Definitions for sampling clocks */
static struct renesas_sdhi_scc rcar_gen3_scc_taps[] = {
	{
		.clk_rate = 0,
		.tap = 0x00000300,
		.tap_hs400_4tap = 0x00000100,
	},
};

static const struct renesas_sdhi_of_data of_rcar_gen3_compatible = {
	.tmio_flags	= TMIO_MMC_HAS_IDLE_WAIT | TMIO_MMC_CLK_ACTUAL |
			  TMIO_MMC_HAVE_CBSY | TMIO_MMC_MIN_RCAR2,
	.capabilities	= MMC_CAP_SD_HIGHSPEED | MMC_CAP_SDIO_IRQ |
			  MMC_CAP_CMD23,
	.capabilities2	= MMC_CAP2_NO_WRITE_PROTECT,
	.bus_shift	= 2,
	.mmc0_addr	= 0xee160000,
	.scc_offset	= 0x1000,
	.taps		= rcar_gen3_scc_taps,
	.taps_num	= ARRAY_SIZE(rcar_gen3_scc_taps),
	/* DMAC can handle 32bit blk count but only 1 segment */
	.max_blk_count	= UINT_MAX / TMIO_MAX_BLK_SIZE,
	.max_segs	= 1,
};

static const struct of_device_id renesas_sdhi_internal_dmac_of_match[] = {
	{ .compatible = "renesas,rcar-gen3-sdhi", .data = &of_rcar_gen3_compatible, },
	{},
};
MODULE_DEVICE_TABLE(of, renesas_sdhi_internal_dmac_of_match);

static void
renesas_sdhi_internal_dmac_dm_write(struct tmio_mmc_host *host,
				    int addr, u64 val)
{
	writeq(val, host->ctl + addr);
}

static void
renesas_sdhi_internal_dmac_enable_dma(struct tmio_mmc_host *host, bool enable)
{
	struct renesas_sdhi *priv = host_to_priv(host);

	if (!host->chan_tx || !host->chan_rx)
		return;

	if (!enable)
		renesas_sdhi_internal_dmac_dm_write(host, DM_CM_INFO1,
						    INFO1_CLEAR);

	if (priv->dma_priv.enable)
		priv->dma_priv.enable(host, enable);
}

static void
renesas_sdhi_internal_dmac_abort_dma(struct tmio_mmc_host *host) {
	u64 val = RST_DTRANRST1 | RST_DTRANRST0;

	renesas_sdhi_internal_dmac_enable_dma(host, false);

	renesas_sdhi_internal_dmac_dm_write(host, DM_CM_RST,
					    RST_RESERVED_BITS & ~val);
	renesas_sdhi_internal_dmac_dm_write(host, DM_CM_RST,
					    RST_RESERVED_BITS | val);

	clear_bit(SDHI_INTERNAL_DMAC_RX_IN_USE, &global_flags);

	renesas_sdhi_internal_dmac_enable_dma(host, true);
}

static void
renesas_sdhi_internal_dmac_dataend_dma(struct tmio_mmc_host *host) {
	struct renesas_sdhi *priv = host_to_priv(host);

	tasklet_schedule(&priv->dma_priv.dma_complete);
}

static void
renesas_sdhi_internal_dmac_start_dma(struct tmio_mmc_host *host,
				     struct mmc_data *data)
{
	struct scatterlist *sg = host->sg_ptr;
	u32 dtran_mode = DTRAN_MODE_BUS_WID_TH | DTRAN_MODE_ADDR_MODE;

	if (!tmio_mmc_pre_dma_transfer(host, data, COOKIE_MAPPED))
		goto force_pio;

	if (data->flags & MMC_DATA_READ) {
		dtran_mode |= DTRAN_MODE_CH_NUM_CH1;
		if (test_bit(SDHI_INTERNAL_DMAC_ONE_RX_ONLY, &global_flags) &&
		    test_and_set_bit(SDHI_INTERNAL_DMAC_RX_IN_USE, &global_flags))
			goto force_pio_with_unmap;
	} else {
		dtran_mode |= DTRAN_MODE_CH_NUM_CH0;
	}

	renesas_sdhi_internal_dmac_enable_dma(host, true);

	/* set dma parameters */
	renesas_sdhi_internal_dmac_dm_write(host, DM_CM_DTRAN_MODE,
					    dtran_mode);
	renesas_sdhi_internal_dmac_dm_write(host, DM_DTRAN_ADDR,
					    sg_dma_address(sg));

	return;

force_pio_with_unmap:
	dma_unmap_sg(&host->pdev->dev, sg, host->sg_len, mmc_get_dma_dir(data));

force_pio:
	host->force_pio = true;
	renesas_sdhi_internal_dmac_enable_dma(host, false);
}

static void renesas_sdhi_internal_dmac_issue_tasklet_fn(unsigned long arg)
{
	struct tmio_mmc_host *host = (struct tmio_mmc_host *)arg;

	tmio_mmc_enable_mmc_irqs(host, TMIO_STAT_DATAEND);

	/* start the DMAC */
	renesas_sdhi_internal_dmac_dm_write(host, DM_CM_DTRAN_CTRL,
					    DTRAN_CTRL_DM_START);
}

static bool renesas_sdhi_internal_dmac_complete(struct tmio_mmc_host *host)
{
	if (!host->data)
		return false;

	renesas_sdhi_internal_dmac_enable_dma(host, false);
	if (host->data && host->data->host_cookie == COOKIE_MAPPED) {
		dma_unmap_sg(&host->pdev->dev, host->sg_ptr, host->sg_len,
				mmc_get_dma_dir(host->data));
		host->data->host_cookie = COOKIE_UNMAPPED;
	}

	if (host->data->flags & MMC_DATA_READ)
		clear_bit(SDHI_INTERNAL_DMAC_RX_IN_USE, &global_flags);

	return true;
}

static void renesas_sdhi_internal_dmac_complete_tasklet_fn(unsigned long arg)
{
	struct tmio_mmc_host *host = (struct tmio_mmc_host *)arg;

	spin_lock_irq(&host->lock);
	if (!renesas_sdhi_internal_dmac_complete(host))
		goto out;

	tmio_mmc_do_data_irq(host);
out:
	spin_unlock_irq(&host->lock);
}

static void
renesas_sdhi_internal_dmac_request_dma(struct tmio_mmc_host *host,
				       struct tmio_mmc_data *pdata)
{
	struct renesas_sdhi *priv = host_to_priv(host);

	/* Disable DMAC interrupts, we don't use them */
	renesas_sdhi_internal_dmac_dm_write(host, DM_CM_INFO1_MASK,
					    INFO1_MASK_CLEAR);
	renesas_sdhi_internal_dmac_dm_write(host, DM_CM_INFO2_MASK,
					    INFO2_MASK_CLEAR);

	/* Each value is set to non-zero to assume "enabling" each DMA */
	host->chan_rx = host->chan_tx = (void *)0xdeadbeaf;

	tasklet_init(&priv->dma_priv.dma_complete,
		     renesas_sdhi_internal_dmac_complete_tasklet_fn,
		     (unsigned long)host);
	tasklet_init(&host->dma_issue,
		     renesas_sdhi_internal_dmac_issue_tasklet_fn,
		     (unsigned long)host);
}

static void
renesas_sdhi_internal_dmac_release_dma(struct tmio_mmc_host *host)
{
	/* Each value is set to zero to assume "disabling" each DMA */
	host->chan_rx = host->chan_tx = NULL;
}

static const struct tmio_mmc_dma_ops renesas_sdhi_internal_dmac_dma_ops = {
	.start = renesas_sdhi_internal_dmac_start_dma,
	.enable = renesas_sdhi_internal_dmac_enable_dma,
	.request = renesas_sdhi_internal_dmac_request_dma,
	.release = renesas_sdhi_internal_dmac_release_dma,
	.abort = renesas_sdhi_internal_dmac_abort_dma,
	.dataend = renesas_sdhi_internal_dmac_dataend_dma,
};

/*
 * Whitelist of specific R-Car Gen3 SoC ES versions to use this DMAC
 * implementation as others may use a different implementation.
 */
static const struct soc_device_attribute gen3_soc_whitelist[] = {
	/* specific ones */
	{ .soc_id = "r8a7795", .revision = "ES1.*",
	  .data = (void *)BIT(SDHI_INTERNAL_DMAC_ONE_RX_ONLY) },
	{ .soc_id = "r8a7796", .revision = "ES1.0",
	  .data = (void *)BIT(SDHI_INTERNAL_DMAC_ONE_RX_ONLY) },
	/* generic ones */
	{ .soc_id = "r8a774a1" },
	{ .soc_id = "r8a774a3" },
	{ .soc_id = "r8a774b1" },
	{ .soc_id = "r8a774c0" },
	{ .soc_id = "r8a774e1" },
	{ .soc_id = "r8a7795" },
	{ .soc_id = "r8a7796" },
	{ .soc_id = "r8a77965" },
	{ .soc_id = "r8a77980" },
	{ .soc_id = "r8a77995" },
	{ .soc_id = "r9a07g044l2" },
	{ .soc_id = "r9a07g054l2" },
	{ /* sentinel */ }
};

static int renesas_sdhi_internal_dmac_probe(struct platform_device *pdev)
{
	const struct soc_device_attribute *soc = soc_device_match(gen3_soc_whitelist);
	struct device *dev = &pdev->dev;

	if (!soc)
		return -ENODEV;

	global_flags |= (unsigned long)soc->data;

	dev->dma_parms = devm_kzalloc(dev, sizeof(*dev->dma_parms), GFP_KERNEL);
	if (!dev->dma_parms)
		return -ENOMEM;

	/* value is max of SD_SECCNT. Confirmed by HW engineers */
	dma_set_max_seg_size(dev, 0xffffffff);

	#ifndef CONFIG_MMC_SDHI_PIO
		return renesas_sdhi_probe(pdev, &renesas_sdhi_internal_dmac_dma_ops);
	#else
		return renesas_sdhi_probe(pdev, NULL);
	#endif
}

static const struct dev_pm_ops renesas_sdhi_internal_dmac_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(tmio_mmc_host_runtime_suspend,
			   tmio_mmc_host_runtime_resume,
			   NULL)
};

static struct platform_driver renesas_internal_dmac_sdhi_driver = {
	.driver		= {
		.name	= "renesas_sdhi_internal_dmac",
		.pm	= &renesas_sdhi_internal_dmac_dev_pm_ops,
		.of_match_table = renesas_sdhi_internal_dmac_of_match,
	},
	.probe		= renesas_sdhi_internal_dmac_probe,
	.remove		= renesas_sdhi_remove,
};

module_platform_driver(renesas_internal_dmac_sdhi_driver);

MODULE_DESCRIPTION("Renesas SDHI driver for internal DMAC");
MODULE_AUTHOR("Yoshihiro Shimoda");
MODULE_LICENSE("GPL v2");
