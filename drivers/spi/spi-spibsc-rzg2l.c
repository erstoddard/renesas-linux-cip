// SPDX-License-Identifier: GPL-2.0-only
//
// SPI Bus Space Controller (SPIBSC) bus driver
// Otherwise known as a SPI Multi I/O Bus Controller
//
// Copyright (C) 2019 Renesas Electronics
// Copyright (C) 2019 Chris Brandt
//
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/spi/spi.h>
#include <linux/reset.h>

/* SPIBSC registers */
#define	CMNCR	0x00
#define	SSLDR	0x04
#define SPBCR	0x08
#define DRCR	0x0c
#define DRCMR	0x10
#define DREAR	0x14
#define DROPR	0x18
#define DRENR	0x1c
#define	SMCR	0x20
#define	SMCMR	0x24
#define	SMADR	0x28
#define	SMOPR	0x2c
#define	SMENR	0x30
#define SMRDR0	0x38
#define SMRDR1	0x3c
#define	SMWDR0	0x40
#define SMWDR1	0x44
#define	CMNSR	0x48
#define DRDMCR	0x58
#define SMDMCR	0x60
#define SMDRENR	0x64

/* CMNCR */
#define	CMNCR_MD	BIT(31)
#define	CMNCR_SFDE	BIT(24)
#define	CMNCR_MOIIO3(x)	(((u32)(x) & 0x3) << 22)
#define	CMNCR_MOIIO2(x)	(((u32)(x) & 0x3) << 20)
#define	CMNCR_MOIIO1(x)	(((u32)(x) & 0x3) << 18)
#define	CMNCR_MOIIO0(x)	(((u32)(x) & 0x3) << 16)
#define	CMNCR_IO3FV(x)	(((u32)(x) & 0x3) << 14)
#define	CMNCR_IO2FV(x)	(((u32)(x) & 0x3) << 12)
#define	CMNCR_IO0FV(x)	(((u32)(x) & 0x3) << 8)
#define	CMNCR_CPHAT	BIT(6)
#define	CMNCR_CPHAR	BIT(5)
#define	CMNCR_SSLP	BIT(4)
#define	CMNCR_CPOL	BIT(3)
#define	CMNCR_BSZ(n)	(((u32)(n) & 0x3) << 0)
#define	OUT_0		(0u)
#define	OUT_1		(1u)
#define	OUT_REV		(2u)
#define	OUT_HIZ		(3u)
#define	BSZ_SINGLE	(0)
#define	BSZ_DUAL	(1)
#define CMNCR_INIT	(CMNCR_MD | \
			CMNCR_MOIIO3(OUT_HIZ) | \
			CMNCR_MOIIO2(OUT_HIZ) | \
			CMNCR_MOIIO1(OUT_HIZ) | \
			CMNCR_MOIIO0(OUT_HIZ) | \
			CMNCR_IO3FV(OUT_HIZ) | \
			CMNCR_IO2FV(OUT_HIZ) | \
			CMNCR_IO0FV(OUT_HIZ) | \
			CMNCR_BSZ(BSZ_SINGLE))

/* SSLDR */
#define	SSLDR_SPNDL(x)	(((u32)(x) & 0x7) << 16)
#define	SSLDR_SLNDL(x)	(((u32)(x) & 0x7) << 8)
#define	SSLDR_SCKDL(x)	(((u32)(x) & 0x7) << 0)
#define	SSLDR_INIT	(SSLDR_SPNDL(3) | SSLDR_SLNDL(3) | SSLDR_SCKDL(3))

/* SPBCR */
#define	SPBCR_SPBR(x)	(((u32)(x) & 0xff) << 8)
#define	SPBCR_BRDV(x)	(((u32)(x) & 0x3) << 0)

/* DRCR (read mode) */
#define	DRCR_SSLN	BIT(24)
#define	DRCR_RBURST(x)	(((u32)(x) & 0xf) << 16)
#define	DRCR_RCF	BIT(9)
#define	DRCR_RBE	BIT(8)
#define	DRCR_SSLE	BIT(0)

/* DROPR (read mode) */
#define	DROPR_OPD3(o)	(((u32)(o) & 0xff) << 24)
#define	DROPR_OPD2(o)	(((u32)(o) & 0xff) << 16)
#define	DROPR_OPD1(o)	(((u32)(o) & 0xff) << 8)
#define	DROPR_OPD0(o)	(((u32)(o) & 0xff) << 0)

/* DRENR (read mode) */
#define DRENR_OCDB_4BIT	BIT(29)
#define DRENR_ADB_4BIT	BIT(25)
#define DRENR_OPDB_4BIT	BIT(21)
#define DRENR_DRDB_4BIT	BIT(17)
#define DRENR_DME	BIT(15)
#define	DRENR_CDE	BIT(14)
#define	DRENR_OCDE	BIT(12)
#define	DRENR_OPDE(o)	(((u32)(o) & 0xf) << 4)

/* SMCR (spi mode) */
#define	SMCR_SSLKP	BIT(8)
#define	SMCR_SPIRE	BIT(2)
#define	SMCR_SPIWE	BIT(1)
#define	SMCR_SPIE	BIT(0)

/* SMCMR (spi mode) */
#define	SMCMR_CMD(c)	(((u32)(c) & 0xff) << 16)
#define	SMCMR_OCMD(o)	(((u32)(o) & 0xff) << 0)

/* SMENR (spi mode) */
#define SMENR_SPIDB_4BIT	BIT(17)
#define	SMENR_SPIDE(b)	(((u32)(b) & 0xf) << 0)
#define	SPIDE_8BITS	(0x8)
#define	SPIDE_16BITS	(0xc)
#define	SPIDE_32BITS	(0xf)

/* CMNSR (spi mode) */
#define	CMNSR_SSLF	BIT(1)
#define	CMNSR_TEND	BIT(0)

#define MAX_CMD_LEN 10
#define MAX_TX_DATA_LEN 3
#define TX_CMD 0
#define TX_ADDR 1
#define TX_DUMMY 2

/* HW Option Flags */
#define HAS_SPBCR BIT(0)

struct spibsc_priv {
	void __iomem *base;	/* register base */
	void __iomem *mmio;	/* memory mapped io space of SPI Flash */
	int mmio_size;
	struct spi_controller *master;
	struct device *dev;
	struct clk *clk;
	int last_xfer;
	int tx_only;
	u64 flags;
	struct reset_control *rstc;
	bool low_data_order;	/* bytes transfer from lowest to */
					/*highest address order*/
};

static void spibsc_write(struct spibsc_priv *sbsc, int reg, u32 val)
{
	iowrite32(val, sbsc->base + reg);
}

static void spibsc_write8(struct spibsc_priv *sbsc, int reg, u8 val)
{
	iowrite8(val, sbsc->base + reg);
}

static void spibsc_write16(struct spibsc_priv *sbsc, int reg, u16 val)
{
	iowrite16(val, sbsc->base + reg);
}

static u32 spibsc_read(struct spibsc_priv *sbsc, int reg)
{
	return ioread32(sbsc->base + reg);
}

static int spibsc_wait_trans_completion(struct spibsc_priv *sbsc)
{
	int t = 320; /* 32us = 4 bytes at 1MHz */

	while (t--) {
		if (spibsc_read(sbsc, CMNSR) & CMNSR_TEND)
			return 0;
		ndelay(100);
	}

	dev_err(sbsc->dev, "Timeout waiting for TEND\n");
	return -ETIMEDOUT;
}

/*
 * This function sends data using 'SPI operation mode'. It is intended to be
 * used only with SPI Flash commands that do not require any reading back from
 * the SPI flash.
 */
static int spibsc_send_data(struct spibsc_priv *sbsc, const u8 *data,
				int len, int tx_nbits, bool low_data_order)
{
	u32 smcr, smenr, smwdr0;
	int ret, unit, sslkp;

	/* print data (for debugging) */
#if defined(DEBUG_PRINT_DATA)
	print_hex_dump_debug("TX data: ", DUMP_PREFIX_NONE, 16, 1, data, len,
			     false);
#endif

	while (len > 0) {
		if (len >= 4) {
			unit = 4;
			smenr = SMENR_SPIDE(SPIDE_32BITS);
		} else {
			unit = len;
			if (unit == 3)
				unit = 2;

			if (unit >= 2)
				smenr = SMENR_SPIDE(SPIDE_16BITS);
			else
				smenr = SMENR_SPIDE(SPIDE_8BITS);
		}

		/* set 4bytes data, bit stream */
		smwdr0 = *data++;
		if (low_data_order) {
			if (unit >= 2)
				smwdr0 |= (u32)(*data++ << 8);
			if (unit >= 3)
				smwdr0 |= (u32)(*data++ << 16);
			if (unit >= 4)
				smwdr0 |= (u32)(*data++ << 24);
		} else {
			if (unit >= 2)
				smwdr0 = (u32)(*data++) | (smwdr0 << 8);
			if (unit >= 3)
				smwdr0 = (u32)(*data++) | (smwdr0 << 8);
			if (unit >= 4)
				smwdr0 = (u32)(*data++) | (smwdr0 << 8);
		}

		/* mask unwrite area */
		if (unit == 3)
			smwdr0 &= ~0xFF000000;
		else if (unit == 2)
			smwdr0 &= ~0xFFFF0000;
		else if (unit == 1)
			smwdr0 &= ~0xFFFFFF00;

		/* write send data. */
		if (unit == 2)
			spibsc_write16(sbsc, SMWDR0, (u16)smwdr0);
		else if (unit == 1)
			spibsc_write8(sbsc, SMWDR0, (u8)smwdr0);
		else
			spibsc_write(sbsc, SMWDR0, smwdr0);

		len -= unit;
		if ((len <= 0) && sbsc->last_xfer)
			sslkp = 0;
		else
			sslkp = 1;

		if (tx_nbits == 4)
			smenr |= SMENR_SPIDB_4BIT;

		/* set params */
		spibsc_write(sbsc, SMCMR, 0);
		spibsc_write(sbsc, SMADR, 0);
		spibsc_write(sbsc, SMOPR, 0);
		spibsc_write(sbsc, SMENR, smenr);

		/* start spi transfer */
		smcr = SMCR_SPIE|SMCR_SPIWE;
		if (sslkp)
			smcr |= SMCR_SSLKP;
		spibsc_write(sbsc, SMCR, smcr);

		ret = spibsc_wait_trans_completion(sbsc);
		if (ret)
			return  ret;
	}
	return 0;
}

/*
 * This function uses 'Data Read' mode to send the command (and address) then
 * read data back out.
 * The HW was designed such that you program the registers with the command,
 * base address, additional command data, etc... But, that makes things too
 * difficult because it means this driver has to pick out those parameters from
 * the data stream that was passed.
 * Instead, we will ignore how the HW was 'supposed' to be used and just
 * blindly put the Tx data (commands) to send in the registers in the order
 * in which we know they will be transmitted:
 *
 * [DRCMR.CMD]+[DRCMR.OCMD]+[DROPR.OPD3]+[DROPR.OPD2]+[DROPR.OPD1]+[DROPR.OPD0]
 *
 * We can send up to 6 bytes this way.
 * We will tell the HW to skip sending the 'address' because we are secretly
 * including it in the "command" and that way we can leave the address registers
 * blank.
 *
 * Note that when reading data, the HW will read in 8-byte units which usually
 * is not an issue for SPI Flash devices.
 */
static int spibsc_send_recv_data(struct spibsc_priv *sbsc, u8 *tx_data,
			    u8 *tx_data_len, u8 *tx_nbits, u8 *rx_data,
						int rx_len, int rx_nbits)
{
	u32 drcmr, drenr, dropr;
	u8 opde;
	int tx_len;

	tx_len = tx_data_len[TX_CMD] + tx_data_len[TX_ADDR] +
					tx_data_len[TX_DUMMY];

	dev_dbg(sbsc->dev, "%s (tx=%d, rx=%d)\n", __func__, tx_len, rx_len);

	if (tx_len > MAX_CMD_LEN) {
		dev_err(sbsc->dev, "Command length too long (%d)", tx_len);
		return -EIO;
	}

	if (rx_len > sbsc->mmio_size) {
		dev_err(sbsc->dev, "Receive length too long (%d)", rx_len);
		return -EIO;
	}

	/* Setup Data Read mode
	 * Flush read cache and enable burst mode. Burst mode is required
	 * in order to keep chip select low between read transfers, but it
	 * also means data is read in 8-byte intervals.
	 */
	spibsc_write(sbsc, DRCR, DRCR_SSLN | DRCR_RCF | DRCR_RBE | DRCR_SSLE);
	spibsc_read(sbsc, DRCR);

	/* Enter Data Read mode */
	spibsc_write(sbsc, CMNCR, 0x01FFF300);
	drcmr = 0;
	drenr = 0;
	dropr = 0;
	opde = 0;

	if (tx_len >= 1) {
		/* Use 'Command' register for the 1st byte */
		drcmr |= SMCMR_CMD(tx_data[0]);
		drenr |= DRENR_CDE;
	}

	/* Use 'Option Data' for 2rd-5th bytes */
	switch (tx_len - tx_data_len[TX_DUMMY]) {
	case 5:
		dropr |= DROPR_OPD0(tx_data[4]);
		opde |= (1 << 0);
		/* fall through */
	case 4:
		dropr |= DROPR_OPD1(tx_data[3]);
		opde |= (1 << 1);
		/* fall through */
	case 3:
		dropr |= DROPR_OPD2(tx_data[2]);
		opde |= (1 << 2);
		/* fall through */
	case 2:
		dropr |= DROPR_OPD3(tx_data[1]);
		opde |= (1 << 3);
		drenr |= DRENR_OPDE(opde);
	}

	if (tx_data_len[2] > 0) {
		drenr |= DRENR_DME;

		/* Calculate Dummy Cycles */
		spibsc_write(sbsc, DRDMCR,
			tx_data_len[TX_DUMMY]*8/tx_nbits[TX_DUMMY]-1);
	}

	if (rx_nbits == 4)
		drenr |= DRENR_DRDB_4BIT;

	if (tx_nbits[TX_ADDR] == 4)
		drenr |= DRENR_OPDB_4BIT;

	spibsc_write(sbsc, DRCMR, drcmr);
	spibsc_write(sbsc, DROPR, dropr);
	spibsc_write(sbsc, DRENR, drenr);

	/* This internal bus access is what will trigger the actual Tx/Rx
	 * operation. Remember, we don't care about the address.
	 */
	memcpy(rx_data, sbsc->mmio, rx_len);

	/* Deactivate chip select */
	spibsc_write(sbsc, DRCR, DRCR_SSLN);

	/* Delay for the next transfer. This is the workaround to fix mount
	 * issue in 4bit data transfer. */
	if (sbsc->master->mode_bits & SPI_RX_QUAD)
		udelay(500);

	/* Print data (for debugging) */
#if defined(DEBUG_PRINT_DATA)
	print_hex_dump_debug("TX data: ", DUMP_PREFIX_NONE, 16, 1, tx_data,
			     tx_len, false);
	print_hex_dump_debug("RX data: ", DUMP_PREFIX_NONE, 16, 1, rx_data,
			     rx_len, false);
#endif
	return 0;
}

static int spibsc_prepare_message(struct spi_controller *ctlr,
				  struct spi_message *msg)
{
	struct spibsc_priv *sbsc = spi_controller_get_devdata(ctlr);
	struct spi_transfer *t, *t_last;

	sbsc->last_xfer = 0;
	sbsc->tx_only = 1;

	t_last = list_last_entry(&msg->transfers, struct spi_transfer,
				 transfer_list);

	t = list_first_entry(&msg->transfers, struct spi_transfer,
			     transfer_list);

	if (t->rx_buf) {
		dev_err(sbsc->dev, "Cannot Rx without Tx first!\n");
		return -EINVAL;
	}

	list_for_each_entry(t, &msg->transfers, transfer_list) {

		if (t->rx_buf) {
			sbsc->tx_only = 0;
			if (t != t_last) {
				dev_err(sbsc->dev, "RX transaction is not the last transaction!\n");
				return -EINVAL;
			}
		}
		if (t->cs_change) {
			dev_err(sbsc->dev, "cs_change not supported");
			return -EINVAL;
		}
	}

	return 0;
}

static int spibsc_transfer_one_message(struct spi_controller *master,
				       struct spi_message *msg)
{
	struct spibsc_priv *sbsc = spi_controller_get_devdata(master);
	struct spi_transfer *t, *t_last;
	u8 tx_data[MAX_CMD_LEN];
	u8 tx_len;
	u8 tx_data_len[MAX_TX_DATA_LEN] = {0, 0, 0};	/* CMD, ADDR, DUMMY */
	u8 tx_nbits[MAX_TX_DATA_LEN] = {0, 0, 0};	/* CMD, ADDR, DUUMY */
	int ret, index;

	t_last = list_last_entry(&msg->transfers, struct spi_transfer,
				 transfer_list);

	/* Tx Only (SPI Mode is used) */
	if (sbsc->tx_only) {

		dev_dbg(sbsc->dev, "%s: TX only\n", __func__);

		/* Initialize for SPI Mode */
		spibsc_write(sbsc, CMNCR, CMNCR_INIT);

		/* Send messages */
		list_for_each_entry(t, &msg->transfers, transfer_list) {

			if (t == t_last)
				sbsc->last_xfer = 1;

			ret = spibsc_send_data(sbsc, t->tx_buf, t->len,
					t->tx_nbits, sbsc->low_data_order);
			if (ret)
				break;

			msg->actual_length += t->len;
		}

		goto done;
	}

	/* Tx, then RX (Data Read Mode is used) */
	dev_dbg(sbsc->dev, "%s: Tx then Rx\n", __func__);

	/* Buffer up the transmit portion (cmd + addr) so we can send it all at
	 * once
	 */
	tx_len = 0;
	index  = 0;
	list_for_each_entry(t, &msg->transfers, transfer_list) {
		if (t->tx_buf) {
			if ((tx_len + t->len) > sizeof(tx_data)) {
				dev_err(sbsc->dev, "Command too big (%d)\n",
					tx_len + t->len);
				return -EINVAL;
			}
			memcpy(tx_data + tx_len, t->tx_buf, t->len);
			tx_len += t->len;
			tx_data_len[index] = t->len;
			tx_nbits[index++] = t->tx_nbits;
		}

		if (t->rx_buf)
			ret = spibsc_send_recv_data(sbsc, tx_data, tx_data_len,
							   tx_nbits, t->rx_buf,
							  t->len, t->rx_nbits);

		msg->actual_length += t->len;
	}

done:
	msg->status = ret;
	spi_finalize_current_message(master);

	return ret;
}

static int spibsc_setup(struct spi_device *spi)
{
	struct spibsc_priv *sbsc = spi_controller_get_devdata(spi->master);
	u32 max_speed_hz;
	unsigned long rate;
	u8 spbr;
	u8 div_ratio;

	if (sbsc->flags & HAS_SPBCR) {
		max_speed_hz = spi->max_speed_hz;
		rate = clk_get_rate(sbsc->clk);

		div_ratio = 2;
		spbr = 1;
		while ((rate / div_ratio) > max_speed_hz) {
			spbr++;
			div_ratio += 2;
			if (spbr == 255)
				break;
		}
		spibsc_write(sbsc, SPBCR, SPBCR_SPBR(spbr) | SPBCR_BRDV(0));
		dev_dbg(sbsc->dev, "Clock set to %ld Hz\n", rate/div_ratio);
	}

	return 0;
}

static int spibsc_probe(struct platform_device *pdev)
{
	struct spi_controller *master;
	struct spibsc_priv *sbsc;
	struct resource *res;
	int ret;

	master = spi_alloc_master(&pdev->dev, sizeof(*sbsc));
	if (!master)
		return -ENOMEM;

	sbsc = spi_controller_get_devdata(master);
	dev_set_drvdata(&pdev->dev, sbsc);
	sbsc->master	= master;
	sbsc->dev	= &pdev->dev;

	sbsc->rstc = devm_reset_control_get(&pdev->dev, NULL);

	if (of_property_read_bool(pdev->dev.of_node, "spibsc,low_data_order"))
		sbsc->low_data_order = true;

	if (IS_ERR(sbsc->rstc))
		dev_warn(&pdev->dev, "failed to get cpg reset\n");
	else
		reset_control_deassert(sbsc->rstc);

	sbsc->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(sbsc->clk)) {
		dev_err(&pdev->dev, "cannot get clock\n");
		ret = PTR_ERR(sbsc->clk);
		goto error;
	}
	clk_prepare_enable(sbsc->clk);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	sbsc->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(sbsc->base)) {
		ret = PTR_ERR(sbsc->base);
		goto error;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	sbsc->mmio_size = resource_size(res);
	sbsc->mmio = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(sbsc->mmio)) {
		ret = PTR_ERR(sbsc->mmio);
		goto error;
	}

	sbsc->flags = (u64) of_device_get_match_data(&pdev->dev);

	master->bus_num = pdev->id;
	master->num_chipselect	= 1;
	master->mode_bits		= SPI_CPOL | SPI_CPHA |
					  SPI_RX_QUAD | SPI_TX_QUAD;
	master->bits_per_word_mask	= SPI_BPW_MASK(8);
	master->setup			= spibsc_setup;
	master->prepare_message		= spibsc_prepare_message;
	master->transfer_one_message	= spibsc_transfer_one_message;
	master->dev.of_node		= pdev->dev.of_node;

	/* Initialize HW */
	spibsc_write(sbsc, CMNCR, CMNCR_INIT);
	spibsc_write(sbsc, DRCR, DRCR_RCF);
	spibsc_write(sbsc, SSLDR, SSLDR_INIT);

	ret = devm_spi_register_controller(&pdev->dev, master);
	if (ret < 0) {
		dev_err(&pdev->dev, "spi_register_controller error.\n");
		goto error;
	}

	return 0;

error:
	if (sbsc->clk && !IS_ERR(sbsc->clk))
		clk_disable_unprepare(sbsc->clk);

	spi_controller_put(master);

	return ret;
}

static int spibsc_remove(struct platform_device *pdev)
{
	struct spibsc_priv *sbsc = dev_get_drvdata(&pdev->dev);

	clk_disable_unprepare(sbsc->clk);

	return 0;
}

static const struct of_device_id of_spibsc_match[] = {
	{ .compatible = "renesas,r7s72100-spibsc", .data = (void *)HAS_SPBCR},
	{ .compatible = "renesas,r9a07g044l-spibsc"},
	{ .compatible = "renesas,r9a07g054l-spibsc"},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_spibsc_match);

static struct platform_driver spibsc_driver = {
	.probe = spibsc_probe,
	.remove = spibsc_remove,
	.driver = {
		.name = "spibsc",
		.owner = THIS_MODULE,
		.of_match_table = of_spibsc_match,
	},
};
module_platform_driver(spibsc_driver);

MODULE_DESCRIPTION("Renesas SPIBSC SPI Flash driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Chris Brandt");
